// SPDX-License-Identifier: BSD-2-Clause-Patent
// The boundary between the Switch's Wi-Fi radio (SwitchWLAN.cpp: Tegra PCIe, BCM4356A3 FullMAC firmware over
// msgbuf rings, the WPA2 supplicant) and the network side (SwitchWLANNet.cpp: en0, the IO80211 registry entries
// and the Apple80211 requests wifid sends). Its shape is UniXNU-sheng's nw_abi.h station half (the WifiNet.cpp
// this network side is ported from calls exactly these), with the key passed as what wifid hands over.
// Plain C types only.
#ifndef SWITCH_WLAN_STATION_H
#define SWITCH_WLAN_STATION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SWLAN_STA_OFF,
    SWLAN_STA_IDLE,
    SWLAN_STA_SCANNING,
    SWLAN_STA_AUTH,
    SWLAN_STA_ASSOC,
    SWLAN_STA_KEYS,
    SWLAN_STA_UP
};
enum {
    SWLAN_RES_OK,
    SWLAN_RES_NOT_FOUND,
    SWLAN_RES_TIMEOUT,
    SWLAN_RES_REJECTED,
    SWLAN_RES_BAD_KEY,
    SWLAN_RES_UNSUPPORTED,
    SWLAN_RES_FAILED,
};

struct swlan_bss {
    uint8_t bssid[6];
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint32_t freq_mhz;
    int8_t signal_dbm;
    uint16_t caps, beacon_int;
    uint32_t age_ms;
    uint16_t ie_len;
    uint8_t ie[384];  // information elements as the access point sent them
};

struct swlan_status {
    uint32_t state;  // SWLAN_STA_*
    int32_t result;  // of the last join, SWLAN_RES_*
    uint8_t bssid[6];
    uint8_t ssid_len;
    uint8_t ssid[32];
    uint32_t freq_mhz;
    int8_t signal_dbm;
    uint8_t mac[6];
    uint32_t power;  // radio on
};

// Called from the radio's threads: scan_done, associated, connect_done and link_lost from its worker thread, rx from
// its work loop. Handlers must not block. A join that gets as far as the access point calls associated (the 802.11
// link is up, the key handshake of a secured network still to come) and then connect_done with how it ended.
struct swlan_events {
    void (*scan_done)(void);
    void (*associated)(void);
    void (*connect_done)(int result);  // SWLAN_RES_*
    void (*link_lost)(int reason);
    void (*rx)(const uint8_t *eth, size_t length);  // Ethernet header + payload
};

// Radio side. Every call only queues work (or copies a snapshot) and returns; any thread context.
void switch_wlan_set_events(const struct swlan_events *events);
int switch_wlan_scan(const uint8_t *ssid, size_t ssid_len);  // ssid NULL: every network
int switch_wlan_bss_count(void);
int switch_wlan_bss_get(int index, struct swlan_bss *out);  // 0 on success
// The access point the radio is associated with, as the firmware reports it (it picks the access point itself, so
// it need not be in the scan table). 0 while associated.
int switch_wlan_current_bss(struct swlan_bss *out);
// key NULL: open network. key_is_pmk: the 32-byte PMK wifid derived (Apple80211 cipher 6); otherwise a passphrase
// of 8..63 characters or the PMK as 64 hex digits. bssid NULL: any access point of that name.
int switch_wlan_connect(const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid, const uint8_t *key, size_t key_len,
                        int key_is_pmk);
void switch_wlan_disconnect(void);
int switch_wlan_power(int on);
int switch_wlan_tx(const uint8_t *eth, size_t length);  // 0 queued
void switch_wlan_status_get(struct swlan_status *out);
void switch_wlan_counters(char *text, size_t size);  // frames and losses, one line

// SwitchWLANNet.cpp: what en0 did with the frames, for the radio's stats line.
struct swlan_net_counters {
    uint32_t rx_input, rx_no_mbuf, rx_input_errors;  // handed to the stack, dropped for want of an mbuf, refused by it
    uint32_t tx_oversize, tx_refused;                // dropped by if_output: larger than a frame, or not queued
};
void switch_wlan_net_counters(struct swlan_net_counters *out);

// SwitchWLAN.cpp: the radio driver behind boot-arg switch-wlan=1 (prints the probe line, publishes the fuse MAC in
// the device tree, then brings the chip up on its own thread). Returns 0 when the boot-arg is off or the driver
// started. Thread context, IOKit up, before userland.
int switch_wlan_publish(void);

// SwitchWLANNet.cpp: en0 and the IO80211 registry entries, once the radio is up with its address.
void switch_wlan_net_start(const uint8_t mac[6]);

// SwitchWLANProbe.cpp
void switch_wlan_probe(void);                         // read-only state report, boot-arg switch-wlan=1
int switch_wlan_mac(uint8_t wifi[6], uint8_t bt[6]);  // 0: fuse-derived addresses as switchroot U-Boot makes them

#ifdef __cplusplus
}
#endif

#endif
