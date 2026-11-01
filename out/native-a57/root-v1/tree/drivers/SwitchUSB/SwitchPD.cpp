// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD: the BM92T36 USB-PD controller (I2C1 0x18) deciding the USB-C port's roles and contract.
//
// A port of CTCaer's L4T driver drivers/misc/bm92txx.c (bm92t_event_handler state machine, PDO choice,
// RDO, DR swap, VDM discovery, Nintendo dock alt mode, DisplayPort alt mode with pin C/D selection),
// with Linux's extcon/regulator plumbing replaced by what this port has:
//   EXTCON_USB_HOST -> switchxhci_set_role(true)    (XUSB host owns USB2 pad 0, SwitchXHCI.cpp)
//   EXTCON_USB      -> switchxhci_set_role(false)   (pad back to the XUDC)
//   vbus regulator  -> BQ24193 REG01 CHG_CONFIG = OTG, BOOST_LIM 1.3 A (bq2419x_vbus_enable)
//   pd_bat_chg      -> BQ24193 REG00 IINLIM        (bq2419x_reg_set_input_charging_current)
//   EXTCON_DISP_DP  -> switch_dp_alt_mode()        (SwitchDPOut.cpp)
//   EXTCON_USB_PD   -> the SwitchBattery listener (it re-reads and publishes the adapter details)
// Only the BQ24193 bits named above are read-modify-written; charge current, voltage and its watchdog
// stay SwitchBattery's/firmware's. Power role and data role come out of the chip's own state (STATUS1
// SRC_MODE, DR field) and the commands the state machine sends (DR_SWAP to become host of a dock/hub).
//
// Board (OLED DTB bm92t@18, bq2419x@6b): ALERT PK4, level low (bm92t_probe requests IRQF_TRIGGER_LOW;
// the DT's edge flag is not what the driver uses), reading ALERT_STATUS releases it; VCONN enable PK5
// (rohm,vconn-en-gpio, active high); 5 V source rail v_vdd5v3 enable PX3 (vbus-source-supply, active
// high); rohm,dp-lanes 4; PD current limits 5 V/9 V 2000 mA, 12 V 1500 mA, 15 V 1200 mA.
//
// Interrupt-driven: every step waits for the chip's ALERT (CMD_DONE, VDM_RECEIVED, CONTRACT, PLUGPULL).
// A reply that never comes is checked against the pin (tegra_gpio_check_delivery panics when the
// interrupt is pending but lost); otherwise it is the far end's silence, logged once, state reset.
#include "SwitchUSBC.h"
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"

namespace {
constexpr unsigned kBus = 1;
constexpr uint8_t kPD = 0x18, kCharger = 0x6b;
constexpr unsigned kVconnGpio = TEGRA_GPIO(K, 5), kSourceRailGpio = TEGRA_GPIO(X, 3);

enum : uint8_t {  // BM92T36 registers (bm92txx.c)
    ALERT_STATUS = 0x02,
    STATUS1 = 0x03,
    STATUS2 = 0x04,
    COMMAND = 0x05,
    CONFIG1 = 0x06,
    READ_PDOS_SRC = 0x08,
    DP_STATUS = 0x18,
    DP_ALERT_EN = 0x19,
    VENDOR_CONFIG = 0x1a,
    CURRENT_PDO = 0x28,
    CURRENT_RDO = 0x2b,
    SET_RDO = 0x30,
    FW_TYPE = 0x4b,
    FW_REVISION = 0x4c,
    INCOMING_VDM = 0x50,
    OUTGOING_VDM = 0x60,
};
enum : uint16_t {
    ALERT_SNK_FAULT = 1u << 0,
    ALERT_SRC_FAULT = 1u << 1,
    ALERT_CMD_DONE = 1u << 2,
    ALERT_PLUGPULL = 1u << 3,
    ALERT_DP_EVENT = 1u << 6,
    ALERT_VDM_RECEIVED = 1u << 11,
    ALERT_CONTRACT = 1u << 12,
    ALERT_SRC_PLUGIN = 1u << 13,
    ALERT_PDO = 1u << 14,
    STATUS1_FAULT_MASK = 3u,
    STATUS1_LASTCMD_SHIFT = 4,
    STATUS1_INSERT = 1u << 7,
    STATUS1_DR_SHIFT = 8,
    STATUS1_SRC_MODE = 1u << 12,
    STATUS1_SPDSNK = 1u << 14,
    STATUS2_OTG_INSERT = 1u << 13,
    CONFIG1_SPDSRC_SHIFT = 14,
    VENDOR_CONFIG_OCP_DISABLE = 1u << 2,
    DATA_ROLE_UFP = 1,
    DATA_ROLE_DFP = 2,
    SPDSRC12_ON = 0,
    SPDSRC12_OFF = 3,
};
enum : uint16_t {  // COMMAND_REG
    PS_RDY_CMD = 0x0505,
    SEND_RDO_CMD = 0x0707,
    PD_HARD_RST_CMD = 0x0808,
    SYS_RESET_CMD = 0x0d0d,
    SEND_VDM_CMD = 0x1111,
    ACCEPT_VDM_CMD = 0x1616,
    DR_SWAP_CMD = 0x1818,
    DP_ENTER_MODE_CMD = 0x3131,
    DP_CFG_AND_START_HPD_CMD = 0x3636,
};
enum : uint8_t {
    VDM_CMD_DISC_ID = 0x01,
    VDM_CMD_DISC_SVID = 0x02,
    VDM_CMD_DISC_MODE = 0x03,
    VDM_CMD_ENTER_MODE = 0x04,
    VDM_ACK = 0x40,
    VDM_STRUCTURED = 0x80,
    VDM_UNSTRUCTURED = 0x00,
    VDO_DP_UFP_D = 1u << 0,
    VDO_DP_SUPPORT = 1u << 2,
    VDO_DP_RECEPTACLE = 1u << 6,
    VDO_DP_U_UFP_D = 1u << 1,
    VDO_DP_PIN_C = 1u << 2,
    VDO_DP_PIN_D = 1u << 3,
    VDO_ID_TYPE_ALTERNATE = 5,
    VDM_ND_READ = 0,
    VDM_ND_WRITE = 1,
    VDM_ND_HOST = 1,
    VDM_ND_DOCK = 2,
    VDM_ND_REQST = 0x00,
    VDM_ND_BUSY = 1,
    VDM_NCMD_LED_CONTROL = 0x01,
    VDM_NCMD_DEVICE_STATE = 0x16,
    VDM_NCMD_HUB_CONTROL = 0x20,
};
constexpr uint16_t VID_NINTENDO = 0x057e, PID_NIN_DOCK = 0x2003;

// VDM messages: register, length, payload (bm92txx.c).
const uint8_t kDiscoverId[] = {OUTGOING_VDM, 4, VDM_CMD_DISC_ID, VDM_STRUCTURED, 0x00, 0xff};
const uint8_t kDiscoverSvid[] = {OUTGOING_VDM, 4, VDM_CMD_DISC_SVID, VDM_STRUCTURED, 0x00, 0xff};
const uint8_t kDiscoverDpMode[] = {OUTGOING_VDM, 4, VDM_CMD_DISC_MODE, VDM_STRUCTURED, 0x01, 0xff};
const uint8_t kEnterNintendoMode[] = {OUTGOING_VDM, 4, VDM_CMD_ENTER_MODE, VDM_STRUCTURED | 1, 0x7e, 0x05};
const uint8_t kQueryDock[] = {OUTGOING_VDM, 8,           VDM_ND_REQST, VDM_UNSTRUCTURED,      0x7e,
                              0x05,         VDM_ND_READ, VDM_ND_HOST,  VDM_NCMD_DEVICE_STATE, 0x00};
const uint8_t kDockHubOn[] = {
    OUTGOING_VDM, 8, VDM_ND_REQST, VDM_UNSTRUCTURED, 0x7e, 0x05, VDM_ND_WRITE, VDM_ND_HOST, VDM_NCMD_HUB_CONTROL, 0x00};
const uint8_t kDockLed[] = {OUTGOING_VDM,
                            12,
                            VDM_ND_REQST,
                            VDM_UNSTRUCTURED,
                            0x7e,
                            0x05,
                            VDM_ND_WRITE,
                            VDM_ND_HOST,
                            VDM_NCMD_LED_CONTROL,
                            0x00,
                            64,
                            0,
                            0,
                            128};  // fade, off, on, duty (bm92t: 128,0,0,64)

// BQ24193 (bq2419x-charger.h): REG00 IINLIM 2:0, REG01 CHG_CONFIG 5:4 (01 charge, 10 OTG), BOOST_LIM 0.
enum : uint8_t { BQ_INPUT_SRC = 0x00, BQ_PWR_ON = 0x01 };
constexpr uint8_t kIinlimMask = 0x07, kChgConfigMask = 0x30, kChgConfigCharge = 0x10, kChgConfigOtg = 0x20,
                  kBoostLim = 0x01;
const unsigned kInputLimits[8] = {100, 150, 500, 900, 1200, 1500, 2000, 3000};

// PDO/RDO fields (struct pd_object / rd_object, little-endian bitfields).
inline unsigned pdoAmp(uint32_t p)
{
    return (p & 0x3ff) * 10;
}
inline unsigned pdoVolt(uint32_t p)
{
    return ((p >> 10) & 0x3ff) * 50;
}
inline unsigned pdoInfo(uint32_t p)
{
    return (p >> 20) & 0x3ff;
}
inline unsigned pdoType(uint32_t p)
{
    return p >> 30;
}
constexpr unsigned PDO_INFO_DR_DATA = 1u << 5, PDO_INFO_USB_COMM = 1u << 6;
inline uint32_t le32(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

enum class State : uint8_t {
    Init,
    NewPdo,
    PsRdySent,
    DrSwapSent,
    DiscIdSent,
    AcceptDiscId,
    DiscSvidSent,
    AcceptDiscSvid,
    DiscModeSent,
    AcceptDiscMode,
    EnterNintendoSent,
    AcceptEnterNintendo,
    DpDiscover,
    DpConfigSent,
    DockQuerySent,
    AcceptDockQuery,
    DockHubSent,
    AcceptDockHub,
    DockLedSent,
    AcceptDockLed,
    NintendoHandled,
    NormalHandled,
};
const char *const kStateNames[] = {
    "INIT",
    "NEW_PDO",
    "PS_RDY_SENT",
    "DR_SWAP_SENT",
    "VDM_DISC_ID_SENT",
    "VDM_ACCEPT_DISC_ID_REPLY",
    "VDM_DISC_SVID_SENT",
    "VDM_ACCEPT_DISC_SVID_REPLY",
    "VDM_DISC_MODE_SENT",
    "VDM_ACCEPT_DISC_MODE_REPLY",
    "VDM_ENTER_ND_ALT_MODE_SENT",
    "VDM_ACCEPT_ENTER_NIN_ALT_MODE_REPLY",
    "DP_DISCOVER_MODE",
    "DP_CFG_START_HPD_SENT",
    "VDM_ND_QUERY_DEVICE_SENT",
    "VDM_ACCEPT_ND_QUERY_DEVICE_REPLY",
    "VDM_ND_ENABLE_USBHUB_SENT",
    "VDM_ACCEPT_ND_ENABLE_USBHUB_REPLY",
    "VDM_ND_LED_ON_SENT",
    "VDM_ACCEPT_ND_LED_ON_REPLY",
    "NINTENDO_CONFIG_HANDLED",
    "NORMAL_CONFIG_HANDLED",
};

constexpr uint32_t kReplyMS = 1500;       // a command or VDM answer (the chip answers in tens of ms)
constexpr uint32_t kPowerDelayMS = 2000;  // bm92t power_work: charging limit 2 s after PS_RDY
constexpr uint32_t kDeliveryMS = 30000;   // lost-ALERT check
constexpr unsigned kAlertBurst = 50;      // ALERTs per second that mean a stuck line

bool gHostWanted, gDpWanted, gSuperSpeedWanted;
void (*gListener)(void *);
void *gListenerContext;
}

class SwitchPD : public IOService {
    OSDeclareDefaultStructors(SwitchPD) IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *alert = nullptr;
    IOTimerEventSource *replyTimer = nullptr, *powerTimer = nullptr, *deliveryTimer = nullptr;
    State state = State::Init;
    bool firstInit = true, sysReset = false, dpEntered = false, vbusOn = false, hostRole = false, charging = false;
    // Cable (struct bm92t_device).
    unsigned pdoNumber = 0, chargingLimit = 0, hubRetries = 10, dpLanes = 0;
    uint32_t pdo = 0;
    bool drdSupport = false, usbHostSource = false, dockSignature = false, nintendoDock = false;
    uint8_t savedIinlim = 0xff;
    unsigned alertCount = 0;
    uint64_t alertWindow = 0;

    int read(uint8_t reg, void *buffer, size_t length) { return tegra_i2c_read(kBus, kPD, reg, buffer, length); }
    int read16(uint8_t reg, uint16_t &value);
    int write16(uint8_t reg, uint16_t value);
    int command(uint16_t code);
    int sendVdm(const uint8_t *message, size_t length);
    int chargerUpdate(uint8_t reg, uint8_t mask, uint8_t value);
    void setVbus(bool on);
    void setRole(bool host);
    void setSourceMode(unsigned mode);
    void notify();
    void enterState(State next, bool expectReply);
    bool checkPdo();
    void sendRdo();
    bool dpConfigure();
    void resetCable();
    void handle();
    bool lastCommandOk(const char *what, uint16_t status1);

    static void alertFired(OSObject *owner, IOInterruptEventSource *, int);
    static void replyTimeout(OSObject *owner, IOTimerEventSource *);
    static void powerReady(OSObject *owner, IOTimerEventSource *);
    static void deliveryCheck(OSObject *owner, IOTimerEventSource *);

public:
    bool begin();
};
OSDefineMetaClassAndStructors(SwitchPD, IOService)

    int SwitchPD::read16(uint8_t reg, uint16_t &value)
{
    uint8_t b[2] = {};
    int err = read(reg, b, 2);
    value = (uint16_t)(b[0] | b[1] << 8);
    return err;
}

int SwitchPD::write16(uint8_t reg, uint16_t value)
{
    const uint8_t b[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return tegra_i2c_write(kBus, kPD, reg, b, 2);
}

int SwitchPD::command(uint16_t code)
{
    return write16(COMMAND, code);
}

int SwitchPD::sendVdm(const uint8_t *message, size_t length)
{
    int err = tegra_i2c_write(kBus, kPD, message[0], message + 1, length - 1);
    if (err) {
        IOLog("SwitchPD: VDM write failed (%d)\n", err);
        return err;
    }
    return command(SEND_VDM_CMD);
}

// Read-modify-write of only `mask` in one BQ24193 register.
int SwitchPD::chargerUpdate(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    int err = tegra_i2c_read(kBus, kCharger, reg, &current, 1);
    if (err) return err;
    const uint8_t next = (uint8_t)((current & ~mask) | (value & mask));
    return next == current ? 0 : tegra_i2c_write(kBus, kCharger, reg, &next, 1);
}

// bq2419x_vbus_enable/disable: we source VBUS from the BQ24193 boost (1.3 A) or go back to charging.
void SwitchPD::setVbus(bool on)
{
    if (on == vbusOn) return;
    int err = on ? chargerUpdate(BQ_PWR_ON, kBoostLim, kBoostLim) : chargerUpdate(BQ_PWR_ON, kBoostLim, 0);
    if (!err) err = chargerUpdate(BQ_PWR_ON, kChgConfigMask, on ? kChgConfigOtg : kChgConfigCharge);
    if (err)
        IOLog("SwitchPD: BQ24193 VBUS %s failed (%d)\n", on ? "on" : "off", err);
    else
        vbusOn = on;
    notify();
}

// extcon USB_HOST / USB. Host role only with switch-usb-host=1; otherwise the port stays a device.
void SwitchPD::setRole(bool host)
{
    host = host && gHostWanted;
    if (host == hostRole) return;
    hostRole = host;
    switchxhci_set_role(host);
}

// bm92t_set_source_mode: CONFIG1 SPDSRC 15:14.
void SwitchPD::setSourceMode(unsigned mode)
{
    uint16_t value = 0;
    if (read16(CONFIG1, value)) return;
    if (((value >> CONFIG1_SPDSRC_SHIFT) & 3) != mode)
        write16(CONFIG1, (uint16_t)((value & ~(3u << CONFIG1_SPDSRC_SHIFT)) | mode << CONFIG1_SPDSRC_SHIFT));
}

void SwitchPD::notify()
{
    if (gListener) gListener(gListenerContext);
}

void SwitchPD::enterState(State next, bool expectReply)
{
    state = next;
    if (expectReply)
        replyTimer->setTimeoutMS(kReplyMS);
    else
        replyTimer->cancelTimeout();
}

bool SwitchPD::lastCommandOk(const char *what, uint16_t status1)
{
    unsigned last = (status1 >> STATUS1_LASTCMD_SHIFT) & 7;
    if (last) IOLog("SwitchPD: %s ended with %u (2 aborted, 4 invalid, 6 rejected, 7 terminated)\n", what, last);
    return last == 0;
}

void SwitchPD::resetCable()
{
    pdoNumber = 0;
    chargingLimit = 0;
    pdo = 0;
    drdSupport = false;
    usbHostSource = false;
    dockSignature = false;
    nintendoDock = false;
}

// bm92t_check_pdo: the dock's two-PDO signature (5 V 500 mA + 15 V 2.6-3 A) takes PDO 2; otherwise the
// fixed PDO of highest wattage at 5/9/12/15 V and <= 3 A, the higher voltage on a tie.
bool SwitchPD::checkPdo()
{
    uint8_t raw[29] = {};
    resetCable();
    if (read(READ_PDOS_SRC, raw, sizeof(raw))) return false;
    unsigned count = raw[0] / 4;
    if (!count) return false;
    if (count > 7) count = 7;
    uint32_t pdos[7];
    for (unsigned i = 0; i < count; ++i) pdos[i] = le32(raw + 1 + 4 * i);
    drdSupport = pdoInfo(pdos[0]) & PDO_INFO_DR_DATA;
    usbHostSource = pdoInfo(pdos[0]) & PDO_INFO_USB_COMM;
    if (count == 2 && pdoVolt(pdos[0]) == 5000 && pdoAmp(pdos[0]) == 500) {
        if (pdoType(pdos[1]) == 0 && pdoVolt(pdos[1]) == 15000 && pdoAmp(pdos[1]) >= 2600 && pdoAmp(pdos[1]) <= 3000) {
            pdoNumber = 2;
            pdo = pdos[1];
            dockSignature = true;
            return true;
        }
        IOLog("SwitchPD: adapter in dock mode with improper current\n");
        return false;
    }
    unsigned best = 0;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned volt = pdoVolt(pdos[i]), amp = pdoAmp(pdos[i]), watt = volt * amp;
        if (volt != 5000 && volt != 9000 && volt != 12000 && volt != 15000) continue;
        if (pdoType(pdos[i]) != 0 || amp > 3000) continue;
        if (watt > best || (watt && watt == best && volt > pdoVolt(pdo)) || (!pdoNumber && !amp && volt == 5000)) {
            best = watt;
            pdoNumber = i + 1;
            pdo = pdos[i];
        }
    }
    return pdoNumber != 0;
}

// bm92t_send_rdo + bm92t_calculate_current_limit: request the PDO with the operating current the
// charger will be allowed (a USB2/USB3 port's power reserved, the DT's per-voltage limits, then the
// BQ24193 input-limit step at or below it).
void SwitchPD::sendRdo()
{
    const unsigned volt = pdoVolt(pdo), amp = pdoAmp(pdo);
    unsigned limit = amp - (volt > 5000 ? 4500000u / volt : 2500000u / volt);
    const unsigned cap = volt == 5000 ? 2000 : volt == 9000 ? 2000 : volt == 12000 ? 1500 : 1200;
    if (limit > cap) limit = cap;
    unsigned step = 0;
    for (int i = 7; i >= 0; --i)
        if (limit >= kInputLimits[i]) {
            step = kInputLimits[i];
            break;
        }
    chargingLimit = step;
    const uint32_t rdo = (pdo & 0x3ff) | (uint32_t)(chargingLimit / 10) << 10 | 1u << 26 | (uint32_t)pdoNumber << 28;
    const uint8_t message[5] = {4, (uint8_t)rdo, (uint8_t)(rdo >> 8), (uint8_t)(rdo >> 16), (uint8_t)(rdo >> 24)};
    IOLog("SwitchPD: requesting PDO %u: %u mV, %u mA (charging %u mA)\n", pdoNumber, volt, amp, chargingLimit);
    if (tegra_i2c_write(kBus, kPD, SET_RDO, message, sizeof(message))) {
        IOLog("SwitchPD: RDO write failed\n");
        return;
    }
    command(SEND_RDO_CMD);
}

// bm92t_handle_dp_config_and_hpd: pin C (4 lanes) first with rohm,dp-lanes 4, then pin D (2 lanes);
// the dock reports Plug but uses Receptacle assignments.
bool SwitchPD::dpConfigure()
{
    uint8_t caps[5] = {};
    if (read(INCOMING_VDM, caps, sizeof(caps))) return false;
    uint8_t config[6] = {OUTGOING_VDM, 4, VDO_DP_SUPPORT | VDO_DP_U_UFP_D, 0, 0, 0};
    unsigned lanes = 0;
    uint8_t pin = VDO_DP_PIN_C;
    for (int attempt = 0; attempt < 2 && !lanes; ++attempt, pin = VDO_DP_PIN_D) {
        if (nintendoDock) {
            if (caps[3] & pin) {
                config[4] = pin;
                lanes = pin == VDO_DP_PIN_C ? 4 : 2;
            }
        } else if (!(caps[1] & VDO_DP_RECEPTACLE)) {
            if (caps[2] & pin) {
                config[3] = pin;
                lanes = pin == VDO_DP_PIN_C ? 4 : 2;
            }
        } else if (caps[3] & pin) {
            config[3] = pin;
            lanes = pin == VDO_DP_PIN_C ? 4 : 2;
        }
    }
    if (caps[0] != 4 || !lanes) {
        IOLog("SwitchPD: no usable DP pin assignment (%02x %02x %02x %02x)\n", caps[0], caps[1], caps[2], caps[3]);
        return false;
    }
    if (tegra_i2c_write(kBus, kPD, config[0], config + 1, sizeof(config) - 1)) return false;
    command(DP_CFG_AND_START_HPD_CMD);
    dpLanes = lanes;
    return true;
}

// bm92t_event_handler. Runs on the work loop with PK4 masked; reading ALERT_STATUS releases the line.
void SwitchPD::handle()
{
    uint16_t alertData = 0, status1 = 0, status2 = 0, dp = 0;
    if (read16(ALERT_STATUS, alertData) || read16(STATUS1, status1) || read16(STATUS2, status2) ||
        read16(DP_STATUS, dp)) {
        IOLog("SwitchPD: status read failed\n");
        return;
    }
    const bool plugged = status1 & STATUS1_INSERT;
    const unsigned dataRole = (status1 >> STATUS1_DR_SHIFT) & 3;
    if (alertData & (ALERT_SNK_FAULT | ALERT_SRC_FAULT))
        IOLog("SwitchPD: %s fault\n", alertData & ALERT_SNK_FAULT ? "sink" : "source");
    if (alertData == ALERT_DP_EVENT) return;  // HPD and friends are DPAUX1's (SwitchDPOut.cpp)

    if (status1 & STATUS1_FAULT_MASK) {
        IOLog("SwitchPD: internal error %u, resetting\n", status1 & STATUS1_FAULT_MASK);
        enterState(State::Init, false);
        if (dpEntered) {
            dpEntered = false;
            switch_dp_alt_mode(false, 0);
        }
        setRole(false);
        setVbus(false);
        if (plugged || (alertData & ALERT_SNK_FAULT) || !alertData) {
            command(SYS_RESET_CMD);
            IOSleep(33);
            uint16_t ignored;
            read16(ALERT_STATUS, ignored);
            sysReset = true;
        }
        return;
    }
    if (sysReset) {
        sysReset = false;
        IOSleep(100);
        uint16_t vendor = 0;
        if (!read16(VENDOR_CONFIG, vendor) && (vendor & VENDOR_CONFIG_OCP_DISABLE))
            write16(VENDOR_CONFIG, vendor & ~VENDOR_CONFIG_OCP_DISABLE);
        setSourceMode(SPDSRC12_ON);
    }

    bool pulled = false;
    if (alertData & ALERT_SRC_FAULT) {
        command(PD_HARD_RST_CMD);
        pulled = true;
    } else if (alertData & ALERT_PLUGPULL) {
        if (!plugged) {
            pulled = true;
        } else if ((status1 & STATUS1_SRC_MODE) && (status2 & STATUS2_OTG_INSERT)) {
            setVbus(true);  // an OTG device: we source VBUS and host it
            setRole(true);
            return;
        } else if ((alertData & ALERT_CONTRACT) && !firstInit) {
            if (!(alertData & ALERT_PDO)) IOSleep(500);
        } else {
            return;  // simple plug event: wait for the contract
        }
    }
    if (pulled) {
        powerTimer->cancelTimeout();
        setVbus(false);
        if (charging) {
            charging = false;
            if (savedIinlim != 0xff) chargerUpdate(BQ_INPUT_SRC, kIinlimMask, savedIinlim);
        }
        hubRetries = 10;
        if (dpEntered) {
            dpEntered = false;
            switch_dp_alt_mode(false, 0);
        }
        setRole(false);
        enterState(State::Init, false);
        notify();
        return;
    }

    switch (state) {
    case State::Init:
        if (alertData & ALERT_SRC_PLUGIN) {  // we are the source (OTG): data role decides
            firstInit = false;
            if (dataRole == DATA_ROLE_DFP) {
                resetCable();
                setRole(true);
                sendVdm(kDiscoverId, sizeof(kDiscoverId));
                enterState(State::DiscIdSent, true);
            }
            break;
        }
        if ((status1 & STATUS1_SRC_MODE) && (status2 & STATUS2_OTG_INSERT)) {
            firstInit = false;
            break;
        }
        if ((alertData & ALERT_CONTRACT) || firstInit) {
            const bool first = firstInit;
            firstInit = false;
            if (!plugged) break;
            if (first && !(alertData & ALERT_CONTRACT) && !(status1 & STATUS1_SPDSNK)) break;
            if (!checkPdo()) {  // a non-PD charger or host: we are a device
                IOLog("SwitchPD: no PD contract, device role\n");
                IOSleep(550);  // bm92t WAR: charger good-power test
                setRole(false);
                notify();
                break;
            }
            sendRdo();
            enterState(State::NewPdo, true);
            IOSleep(20);
            break;
        }
        if (!alertData && !plugged) setRole(false);
        break;

    case State::NewPdo:
        if (alertData & ALERT_CONTRACT) {
            uint8_t curPdo[5] = {}, curRdo[5] = {};
            read(CURRENT_PDO, curPdo, sizeof(curPdo));
            read(CURRENT_RDO, curRdo, sizeof(curRdo));
            const uint32_t p = le32(curPdo + 1), r = le32(curRdo + 1);
            IOLog("SwitchPD: contract %u mV %u mA, RDO op %u mA max %u mA%s\n", pdoVolt(p), pdoAmp(p),
                  ((r >> 10) & 0x3ff) * 10, (r & 0x3ff) * 10, (r >> 27) & 1 ? " (mismatch)" : "");
            command(PS_RDY_CMD);
            enterState(State::PsRdySent, true);
        }
        break;

    case State::PsRdySent:
        if (alertData & ALERT_CMD_DONE) {
            powerTimer->setTimeoutMS(kPowerDelayMS);
            if (dataRole == DATA_ROLE_UFP) {
                // Linux swaps to host whenever the source is dual-role. A source that says it talks USB (a
                // computer) keeps us its device here, so usbmux (switchusb=3) keeps working on a Mac; the
                // dock's PDO signature (5 V 0.5 A + 15 V 2.6-3 A) and plain dual-role hubs still get the swap.
                if (!drdSupport || !gHostWanted || (usbHostSource && !dockSignature)) {
                    setRole(false);
                    enterState(State::NormalHandled, false);
                    break;
                }
                command(DR_SWAP_CMD);
                enterState(State::DrSwapSent, true);
            } else if (dataRole == DATA_ROLE_DFP) {
                setRole(true);
                sendVdm(kDiscoverId, sizeof(kDiscoverId));
                enterState(State::DiscIdSent, true);
            }
        }
        break;

    case State::DrSwapSent:
        if ((alertData & ALERT_CMD_DONE) && plugged && lastCommandOk("DR_SWAP", status1) && dataRole == DATA_ROLE_DFP) {
            setRole(true);
            sendVdm(kDiscoverId, sizeof(kDiscoverId));
            enterState(State::DiscIdSent, true);
        } else if (alertData & ALERT_CMD_DONE) {
            setRole(false);  // swap refused: we remain the device
            enterState(State::NormalHandled, false);
        }
        break;

    case State::DiscIdSent:
    case State::DiscSvidSent:
    case State::DiscModeSent:
    case State::EnterNintendoSent:
    case State::DockQuerySent:
    case State::DockHubSent:
    case State::DockLedSent:
        if (alertData & ALERT_VDM_RECEIVED) {
            command(ACCEPT_VDM_CMD);
            enterState(State((uint8_t)state + 1), true);
        }
        break;

    case State::AcceptDiscId:
        if (alertData & ALERT_CMD_DONE) {
            uint8_t vdm[29] = {};
            read(INCOMING_VDM, vdm, sizeof(vdm));
            const uint32_t idHeader = le32(vdm + 5), product = le32(vdm + 13);
            const uint16_t vid = idHeader & 0xffff, pid = product >> 16;
            const unsigned type = (idHeader >> 27) & 7;
            IOLog("SwitchPD: partner %04x:%04x type %u\n", vid, pid, type);
            if (type == VDO_ID_TYPE_ALTERNATE && vid == VID_NINTENDO && pid == PID_NIN_DOCK) {
                nintendoDock = true;
                sendVdm(kEnterNintendoMode, sizeof(kEnterNintendoMode));
                enterState(State::EnterNintendoSent, true);
            } else if (!gDpWanted) {
                enterState(State::NormalHandled, false);
            } else {
                sendVdm(kDiscoverSvid, sizeof(kDiscoverSvid));
                enterState(State::DiscSvidSent, true);
            }
        }
        break;

    case State::AcceptDiscSvid:
        if (alertData & ALERT_CMD_DONE) {
            uint8_t vdm[29] = {};
            read(INCOMING_VDM, vdm, sizeof(vdm));
            bool dp = false;
            if (vdm[1] == (VDM_ACK | VDM_CMD_DISC_SVID))
                for (unsigned i = 0; i + 1 < (unsigned)(vdm[0] - 4) && 6 + i < sizeof(vdm); i += 2)
                    dp |= (vdm[5 + i] | vdm[6 + i] << 8) == 0xff01;
            if (dp) {
                sendVdm(kDiscoverDpMode, sizeof(kDiscoverDpMode));
                enterState(State::DiscModeSent, true);
            } else
                enterState(State::NormalHandled, false);
        }
        break;

    case State::AcceptDiscMode:
        if (alertData & ALERT_CMD_DONE) {
            uint8_t vdm[29] = {};
            read(INCOMING_VDM, vdm, sizeof(vdm));
            if (vdm[0] > 4 && vdm[1] == (VDM_ACK | VDM_CMD_DISC_MODE) && vdm[2] == VDM_STRUCTURED && vdm[3] == 0x01 &&
                vdm[4] == 0xff && (vdm[5] & VDO_DP_UFP_D) && (vdm[5] & VDO_DP_SUPPORT)) {
                command(DP_ENTER_MODE_CMD);
                IOSleep(100);
                enterState(State::DpDiscover, true);
            } else {
                enterState(State::NormalHandled, false);
            }
        }
        break;

    case State::AcceptEnterNintendo:
        if (alertData & ALERT_CMD_DONE) {
            uint8_t vdm[29] = {};
            read(INCOMING_VDM, vdm, sizeof(vdm));
            if (!(vdm[1] == (VDM_ACK | VDM_CMD_ENTER_MODE) && vdm[2] == (VDM_STRUCTURED | 1) && vdm[3] == 0x7e &&
                  vdm[4] == 0x05)) {
                IOLog("SwitchPD: dock refused the Nintendo alt mode\n");
                enterState(State::NormalHandled, false);
                break;
            }
            if (!gDpWanted) {
                sendVdm(kQueryDock, sizeof(kQueryDock));
                enterState(State::DockQuerySent, true);
                break;
            }
            command(DP_ENTER_MODE_CMD);
            IOSleep(100);
            enterState(State::DpDiscover, true);
        }
        break;

    case State::DpDiscover:
        if (alertData & ALERT_CMD_DONE) {
            if (dpConfigure())
                enterState(State::DpConfigSent, true);
            else
                enterState(State::Init, false);
        }
        break;

    case State::DpConfigSent:
        if ((alertData & ALERT_CMD_DONE) && plugged && lastCommandOk("DP_CFG_AND_START_HPD", status1)) {
            dpEntered = true;
            switch_dp_alt_mode(true, dpLanes);
            if (nintendoDock) {
                sendVdm(kQueryDock, sizeof(kQueryDock));
                enterState(State::DockQuerySent, true);
            } else
                enterState(State::NormalHandled, false);
        }
        break;

    case State::AcceptDockQuery:
        if (alertData & ALERT_CMD_DONE) {
            uint8_t vdm[29] = {};
            if (!read(INCOMING_VDM, vdm, sizeof(vdm)) && vdm[6] == VDM_ND_DOCK && vdm[7] == VDM_NCMD_DEVICE_STATE + 1) {
                if (vdm[11] & 0x02) {  // old dock firmware: no hub control, USB device only
                    setRole(false);
                    IOLog("SwitchPD: dock has old firmware\n");
                }
                IOLog("SwitchPD: dock state %02x %02x %02x %02x\n", vdm[9], vdm[10], vdm[11], vdm[12]);
            }
            sendVdm(kDockLed, sizeof(kDockLed));
            enterState(State::DockLedSent, true);
        }
        break;

    case State::AcceptDockLed:
        if (alertData & ALERT_CMD_DONE) {
            IOSleep(500);  // the hub powers up
            sendVdm(kDockHubOn, sizeof(kDockHubOn));
            enterState(State::DockHubSent, true);
        }
        break;

    case State::AcceptDockHub:
        if (alertData & ALERT_CMD_DONE) {
            uint8_t vdm[29] = {};
            read(INCOMING_VDM, vdm, sizeof(vdm));
            if (vdm[6] == VDM_ND_DOCK && vdm[7] == VDM_NCMD_HUB_CONTROL + 1 && (vdm[5] & VDM_ND_BUSY) && hubRetries) {
                --hubRetries;
                IOSleep(250);
                sendVdm(kDockHubOn, sizeof(kDockHubOn));
                enterState(State::DockHubSent, true);
                break;
            }
            enterState(State::NintendoHandled, false);
        }
        break;

    case State::NintendoHandled:
    case State::NormalHandled: break;
    }
}

void SwitchPD::alertFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchPD *>(owner);
    const uint64_t now = mach_absolute_time();
    uint64_t elapsed = 0;
    absolutetime_to_nanoseconds(now - self->alertWindow, &elapsed);
    if (elapsed > 1000000000ull) {
        self->alertWindow = now;
        self->alertCount = 0;
    }
    if (++self->alertCount > kAlertBurst) {
        char pin[256];
        tegra_gpio_describe(TEGRA_GPIO_PD_ALERT, pin, sizeof(pin));
        panic("SwitchPD: BM92T36 ALERT (PK4) fired %u times in 1 s (stuck low?): %s", self->alertCount, pin);
    }
    self->handle();
}

// Silence after a command: a lost interrupt panics inside check_delivery; otherwise the partner did not
// answer (state machine back to INIT, as an unplug would leave it).
void SwitchPD::replyTimeout(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchPD *>(owner);
    tegra_gpio_check_delivery(TEGRA_GPIO_PD_ALERT, "SwitchPD");
    IOLog("SwitchPD: no answer in %s\n", kStateNames[(unsigned)self->state]);
    self->state = State::Init;
}

// bm92t_power_work: the charger may draw the contract's current now.
void SwitchPD::powerReady(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchPD *>(owner);
    uint8_t step = 0;
    while (step < 7 && kInputLimits[step + 1] <= self->chargingLimit) ++step;
    if (self->savedIinlim == 0xff) {
        uint8_t current = 0;
        if (!tegra_i2c_read(kBus, kCharger, BQ_INPUT_SRC, &current, 1)) self->savedIinlim = current & kIinlimMask;
    }
    if (!self->chargerUpdate(BQ_INPUT_SRC, kIinlimMask, step)) self->charging = true;
    IOLog("SwitchPD: charging input limit %u mA\n", kInputLimits[step]);
    self->notify();
}

void SwitchPD::deliveryCheck(OSObject *owner, IOTimerEventSource *sender)
{
    (void)owner;
    tegra_gpio_check_delivery(TEGRA_GPIO_PD_ALERT, "SwitchPD");
    sender->setTimeoutMS(kDeliveryMS);
}

// bm92t_probe + bm92t_extcon_cable_set_init_state.
bool SwitchPD::begin()
{
    uint16_t type = 0, revision = 0;
    int err = tegra_i2c_init(kBus);
    if (!err) err = read16(FW_TYPE, type);
    if (!err) err = read16(FW_REVISION, revision);
    if (err || revision <= 0x644) {
        IOLog("SwitchPD: BM92T36 not usable (err %d, fw type %#x revision %#x)\n", err, type, revision);
        return false;
    }
    loop = IOWorkLoop::workLoop();
    replyTimer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchPD::replyTimeout) : nullptr;
    powerTimer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchPD::powerReady) : nullptr;
    deliveryTimer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchPD::deliveryCheck) : nullptr;
    if (!replyTimer || !powerTimer || !deliveryTimer || loop->addEventSource(replyTimer) ||
        loop->addEventSource(powerTimer) || loop->addEventSource(deliveryTimer)) {
        IOLog("SwitchPD: work loop setup failed\n");
        return false;
    }
    setSourceMode(SPDSRC12_OFF);
    tegra_gpio_output(kVconnGpio, 0);
    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_PD_ALERT, TEGRA_GPIO_LEVEL_LOW);
    alert = nub ? IOInterruptEventSource::interruptEventSource(this, &SwitchPD::alertFired, nub, 0) : nullptr;
    if (!alert || loop->addEventSource(alert) != kIOReturnSuccess) {
        panic("SwitchPD: cannot attach the BM92T36 ALERT (PK4, GPIO %u, level low): is SwitchBattery still holding it?",
              TEGRA_GPIO_PD_ALERT);
    }
    tegra_gpio_output(kSourceRailGpio, 1);  // v_vdd5v3, 10 ms ramp
    tegra_gpio_output(kVconnGpio, 1);
    IOSleep(100);

    // Init state, as the oneshot work: VBUS off, a stuck UFP reset, OCP on, both source switches
    // powered (OTG and charger), DP alerts as asked, then one pass of the handler (first init).
    setVbus(false);
    uint16_t status1 = 0, dp = 0;
    if (!read16(STATUS1, status1) && !read16(DP_STATUS, dp) && (status1 & STATUS1_INSERT) &&
        (((status1 >> STATUS1_DR_SHIFT) & 3) == DATA_ROLE_DFP || (dp & (1u << 15)) ||
         ((status1 >> STATUS1_LASTCMD_SHIFT) & 7))) {
        IOLog("SwitchPD: invalid state at start, system reset\n");
        command(SYS_RESET_CMD);
        IOSleep(100);
    }
    uint16_t vendor = 0;
    if (!read16(VENDOR_CONFIG, vendor) && (vendor & VENDOR_CONFIG_OCP_DISABLE))
        write16(VENDOR_CONFIG, vendor & ~VENDOR_CONFIG_OCP_DISABLE);
    setSourceMode(SPDSRC12_ON);
    write16(DP_ALERT_EN, gDpWanted ? 0xffff : 0x0000);
    IOSleep(100);
    alert->enable();
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<SwitchPD *>(owner)->handle();
            return kIOReturnSuccess;
        },
        this);
    deliveryTimer->setTimeoutMS(kDeliveryMS);
    IOLog("SwitchPD: started (fw type %#x revision %#x)\n", type, revision);
    return true;
}

// ---------------------------------------------------------------------------
namespace {
bool readFlag(const char *name)
{
    uint32_t value = 0;
    return PE_parse_boot_argn(name, &value, sizeof(value)) && value;
}

void usbcThread(void *, wait_result_t)
{
    // The host first: its bring-up shows its own failures, and a port already in host role (an OTG
    // device left plugged at boot) needs it running before SwitchPD hands it the pad.
    if (gHostWanted) switchxhci_start(gSuperSpeedWanted);
    if (gDpWanted) switch_dp_start();
    IOService *resources = IOService::getResourceService();
    SwitchPD *pd = OSTypeAlloc(SwitchPD);
    if (!resources || !pd || !pd->init() || !pd->attach(resources) || !pd->begin()) {
        IOLog("SwitchPD: setup failed; the port stays a device\n");
        return;  // stays attached when begin() failed late: the timers and pin are released with it never
    }
    pd->setName("SwitchPD");
    pd->registerService();
}
}

extern "C" bool switch_pd_owns_alert(void)
{
    return readFlag("switch-usb-host") || readFlag("switch-dp");
}

extern "C" void switch_pd_set_power_listener(void (*listener)(void *), void *context)
{
    gListenerContext = context;
    __atomic_store_n(&gListener, listener, __ATOMIC_RELEASE);
}

extern "C" int switch_usbc_start(void)
{
    static uint32_t started;
    gHostWanted = readFlag("switch-usb-host");
    gDpWanted = readFlag("switch-dp");
    gSuperSpeedWanted = gHostWanted && readFlag("switch-usb3");
    if (!gHostWanted && !gDpWanted) return 0;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return 0;
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(&usbcThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("SwitchUSBC: thread start failed\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
