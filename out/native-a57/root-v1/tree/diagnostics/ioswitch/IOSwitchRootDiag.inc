/* Root-boot observations only. Stage numbers never authorize storage or exec. */
volatile uint64_t ioswitch_root_record[24];
static unsigned root_boot_enabled;
static uint64_t root_last_draw;
static void root_screen(void)
{
    if (!root_boot_enabled || !ioswitch_diag_record.ready || ioswitch_diag_record.panicked) return;
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    if (ioswitch_diag_console_active()) {
        char line[105];
        for (unsigned y = 120; y < 180; y++)
            for (unsigned x = 16; x < 1264; x++) ioswitch_diag_pixel(framebuffer, x, y, IOSWITCH_BACKGROUND);
        (void)snprintf(line, sizeof(line), "ROOT %llu ERROR %llx AUTH %llu OWNER %llu CACHE %llx DATA %llx",
                       ioswitch_root_record[2], ioswitch_root_record[3], ioswitch_root_record[8],
                       ioswitch_root_record[9], ioswitch_root_record[11], ioswitch_root_record[12]);
        text(120, line, sizeof(line), ioswitch_root_record[3] ? IOSWITCH_RED : IOSWITCH_GREEN);
        (void)snprintf(line, sizeof(line), "POLICY %llu/%llu VERIFIED SD %llu", ioswitch_root_record[5],
                       ioswitch_root_record[6], ioswitch_root_record[7]);
        text(140, line, sizeof(line), IOSWITCH_WHITE);
        (void)snprintf(line, sizeof(line), "PID1 CALLS %llu EXEC %llx ROOT OWNER %llx MOBILE %llx",
                       ioswitch_root_record[15], ioswitch_root_record[13], ioswitch_root_record[19],
                       ioswitch_root_record[20]);
        text(160, line, sizeof(line), IOSWITCH_WHITE);
        console_flush();
        sync_pixels();
        (void)ml_set_interrupts_enabled(previous);
        return;
    }
    for (unsigned y = 340; y < 660; y++)
        for (unsigned x = 16; x < 1264; x++) ioswitch_diag_pixel(framebuffer, x, y, IOSWITCH_BACKGROUND);
    const uint64_t stage = ioswitch_root_record[2];
    const char *message = ioswitch_root_record[3] ? "NATIVE ROOT BOOT REFUSED OR FAILED"
                          : stage == 1            ? "QUALIFYING NATIVE READ ONLY STORAGE"
                          : stage == 2            ? "LOADING SIGNED ROOT POLICY"
                          : stage == 3            ? "AUTHENTICATING ROOT POLICY"
                          : stage == 4            ? "ROOT POLICIES AUTHENTICATED - PUBLISHING"
                          : stage == 5            ? "VERIFIED STORAGE READY - FINDING ROOT"
                          : stage == 6            ? "MOUNTING ORIGINAL APFS ROOT"
                          : stage == 7            ? "ORIGINAL APFS ROOT MOUNTED"
                          : stage == 8            ? "OWNER ROOT POLICY DECISION RECORDED"
                          : stage == 9            ? "PREPARING ORIGINAL SYSTEMOS CACHE"
                          : stage == 10           ? "SYSTEMOS CACHE PREPARATION RETURNED"
                          : stage == 11           ? "MOUNTING DISPOSABLE RAM DATA"
                          : stage == 12           ? "RAM DATA PREPARATION RETURNED"
                          : stage == 13           ? "ORIGINAL LAUNCHD EXEC RETURNED"
                          : stage == 14           ? "ORIGINAL PID1 SYSCALL OBSERVED"
                                                  : "NATIVE ROOT BOOT IN PROGRESS";
    text(360, message, 104, ioswitch_root_record[3] ? IOSWITCH_RED : IOSWITCH_GREEN);
    hex_line(400, "ROOT STAGE / ERROR=", (stage << 32) | ioswitch_root_record[3]);
    hex_line(420, "POLICY DISK=", ioswitch_root_record[4]);
    hex_line(440, "POLICY LOADED BYTES=", ioswitch_root_record[5]);
    hex_line(460, "POLICY TOTAL BYTES=", ioswitch_root_record[6]);
    hex_line(480, "AUTHENTICATED SD BYTES=", ioswitch_root_record[7]);
    hex_line(500, "APPLE ROOT AUTH / OWNER=", (ioswitch_root_record[8] << 32) | ioswitch_root_record[9]);
    hex_line(520, "MOUNTED ROOT DEVICE=", ioswitch_root_record[10]);
    hex_line(540, "CACHE / DATA RESULT=", (ioswitch_root_record[11] << 32) | ioswitch_root_record[12]);
    if (stage == 12) {
        hex_line(560, "DATA ROOT UID / GID=", ioswitch_root_record[19]);
        hex_line(580, "DATA MOBILE UID / GID=", ioswitch_root_record[20]);
        hex_line(600, "DATA MOUNT FLAGS=", ioswitch_root_record[21]);
    } else {
        hex_line(560, "ORIGINAL LAUNCHD EXEC RESULT=", ioswitch_root_record[13]);
        hex_line(580, "FIRST PID1 SYSCALL PC=", ioswitch_root_record[14]);
        hex_line(600, "PID1 SYSCALL COUNT=", ioswitch_root_record[15]);
    }
    hex_line(620, "LAST SYSCALL / ERROR=", (ioswitch_root_record[16] << 32) | ioswitch_root_record[17]);
    text(640, "OS READ ONLY - DATA WRITES STAY IN RAM", 104, IOSWITCH_WHITE);
    sync_pixels();
    (void)ml_set_interrupts_enabled(previous);
}
void ioswitch_root_progress(unsigned stage, unsigned disk, uint64_t done, uint64_t total)
{
    if (stage & 0x80000000u)
        ioswitch_root_record[3] = stage & 0x7fffffffu;
    else
        ioswitch_root_record[2] = stage;
    ioswitch_root_record[4] = disk;
    ioswitch_root_record[5] = done;
    ioswitch_root_record[6] = total;
    root_screen();
}
void ioswitch_root_io_progress(uint64_t bytes, unsigned error)
{
    ioswitch_root_record[7] = bytes;
    if (error && !ioswitch_root_record[3]) ioswitch_root_record[3] = 500 + error;
    uint64_t now, freq;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(now));
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(freq));
    if (error || (freq && now - root_last_draw >= freq)) {
        root_last_draw = now;
        root_screen();
    }
}
void ioswitch_root_data_ownership(uint64_t flags, uint64_t root, uint64_t mobile)
{
    ioswitch_root_record[19] = root;
    ioswitch_root_record[20] = mobile;
    ioswitch_root_record[21] = flags;
}
void ioswitch_root_event(unsigned stage, int error, uint64_t detail)
{
    ioswitch_root_record[2] = stage;
    if (stage == 8) {
        ioswitch_root_record[8] = (unsigned)error;
        ioswitch_root_record[9] = detail & 1;
        ioswitch_root_record[10] = detail >> 32;
    } else {
        if (error && !ioswitch_root_record[3]) ioswitch_root_record[3] = (stage << 16) | (unsigned)error;
        if (stage == 10) ioswitch_root_record[11] = (unsigned)error;
        if (stage == 12) ioswitch_root_record[12] = (unsigned)error;
        if (stage == 13) {
            ioswitch_root_record[13] = (unsigned)error;
            ioswitch_root_record[18] = error == 0;
        }
    }
    root_screen();
}
static void root_syscall(unsigned code, int error, int pid, uint64_t pc, uint64_t cpsr)
{
    if (!root_boot_enabled || !ioswitch_root_record[18] || pid != 1 || (cpsr & 31)) return;
    if (!ioswitch_root_record[15]) ioswitch_root_record[14] = pc;
    ioswitch_root_record[15]++;
    ioswitch_root_record[16] = code;
    ioswitch_root_record[17] = (unsigned)error;
    ioswitch_root_record[2] = 14;
    if (ioswitch_root_record[15] == 1) ioswitch_diag_stage("original PID1 EL0 syscall observed");
    if (ioswitch_root_record[15] == 1 || error) root_screen();
}
/* Board drivers (GPIO, fan/thermal, battery) sleep on I2C transfers and wait for the GIC, so they
 * start on their own thread: the root mount never waits behind them. Each is on by default and can be
 * switched off with switch-<name>=0; a driver whose interrupt is missing panics with its own reason. */
#include <kern/kern_types.h>
extern kern_return_t kernel_thread_start(thread_continue_t continuation, void *parameter, thread_t *new_thread);
extern void thread_deallocate(thread_t thread);
extern void thread_terminate_self(void);
extern int tegra_gpio_start(void);
extern int switch_thermal_start(void);
extern int switch_battery_start(void);
extern int switch_touch_start(void);
extern int switch_buttons_start(void);
extern int switch_als_start(void);       /* on unless switch-als=0; after SwitchTouch, which powers the sensor (LDO6) */
extern int switch_bluetooth_start(void); /* no-op unless switch-bt=1; runs its bring-up on its own thread */
extern int switch_joycon_start(void);    /* no-op unless switch-joycon-rail=1 */
extern int switch_wlan_publish(void);    /* no-op unless switch-wlan=1; runs its bring-up on its own thread */
extern int tegra_gm20b_probe(void);      /* no-op unless tegra-gpu=<stage>; runs its bring-up on its own thread */
extern int tegra_host1x_probe(void);     /* no-op unless switch-host1x=1; own thread */
extern int tegra_nvdec_probe(void);      /* no-op unless switch-nvdec=1; own thread, starts host1x itself */
extern int
ioswitch_audio_publish(void); /* null sink unless switch-audio=1|3; the Tegra backend sleeps on I2C1, PZ4 and the GIC */
extern int switch_suspend_start(void); /* no-op unless switch-sleep=2; after every board driver it suspends */
extern int switch_usb_start(void);     /* no-op without the ADT's /arm-io/usb-device; runs its bring-up on its own thread */
extern int ioswitch_watchdog_publish(void); /* the IOWatchdog launchd starts watchdogd for (NativeIOSwitchWatchdog) */
static int ioswitch_board_enabled(const char *name)
{
    unsigned enabled = 1;
    PE_parse_boot_argn(name, &enabled, sizeof(enabled));
    return enabled != 0;
}
static void ioswitch_board_thread(void *parameter __unused, wait_result_t result __unused)
{
    /* First: returns at once and publishes the per-unit BD address before anything here can sleep. */
    if (switch_bluetooth_start()) printf("IOSwitch: SwitchBluetooth did not start\n");
    /* Before anything that can sleep long: launchd must find the IOWatchdog when it reads its LaunchEvents. */
    if (ioswitch_watchdog_publish()) printf("IOSwitch: NativeIOSwitchWatchdog did not start\n");
    /* After Bluetooth: rail pairing hands Joy-Cons the console's (fuse-derived) BD address. */
    if (switch_joycon_start()) printf("IOSwitch: SwitchJoyCon did not start\n");
    if (switch_usb_start()) printf("IOSwitch: SwitchUSB did not start\n");
    if (ioswitch_board_enabled("switch-gpio") && tegra_gpio_start()) printf("IOSwitch: TegraGPIO did not start\n");
    if (ioswitch_board_enabled("switch-thermal") && switch_thermal_start())
        printf("IOSwitch: SwitchThermal did not start\n");
    if (ioswitch_board_enabled("switch-battery") && switch_battery_start())
        printf("IOSwitch: SwitchBattery did not start\n");
    if (ioswitch_board_enabled("switch-touch") && switch_touch_start()) printf("IOSwitch: SwitchTouch did not start\n");
    if (ioswitch_board_enabled("switch-buttons") && switch_buttons_start())
        printf("IOSwitch: SwitchButtons did not start\n");
    if (switch_als_start()) printf("IOSwitch: SwitchALS did not start\n");
    if (switch_wlan_publish()) printf("IOSwitch: Wi-Fi driver not started\n");
    if (tegra_gm20b_probe()) printf("IOSwitch: TegraGM20B did not start\n");
    if (tegra_host1x_probe()) printf("IOSwitch: TegraHost1x did not start\n");
    if (tegra_nvdec_probe()) printf("IOSwitch: TegraNVDEC did not start\n");
    if (ioswitch_audio_publish()) printf("IOSwitch: SwitchAudio did not start\n");
    (void)switch_suspend_start();
    thread_terminate_self();
}
static void ioswitch_board_start(void)
{
    thread_t thread;
    if (kernel_thread_start(ioswitch_board_thread, NULL, &thread) != KERN_SUCCESS)
        panic("IOSwitch: cannot start the board driver thread");
    thread_deallocate(thread);
}
void ioswitch_root_boot_prepare(void)
{
    int enabled = 0, files = 0, legacy = 0;
    if (!PE_parse_boot_argn("ioswitch-root-boot", &enabled, sizeof(enabled)) || enabled != 1)
        panic("native root boot requires explicit ioswitch-root-boot=1");
    root_boot_enabled = 1;
    ioswitch_root_record[0] = UINT64_C(0x494f5357524f4f54);
    ioswitch_root_record[1] = 1;
    ioswitch_root_record[8] = ioswitch_root_record[11] = ioswitch_root_record[12] = ioswitch_root_record[13] =
        UINT32_MAX;
    ioswitch_root_record[19] = ioswitch_root_record[20] = UINT64_MAX;
    (void)files;
    (void)legacy;
    if (ioswitch_files_storage_profile() != IOSW_STORAGE_RAW) panic("IOSwitch: only ioswitch-storage=raw is supported");
    /* The SD block driver owns the controller and finds the APFS partition;
     * IOKit and APFS publish the container and volumes from there. */
    int error = ioswitch_block_publish();
    if (error) panic("IOSwitch: SD block device publish failed (%d); see IOSwitchSD lines above", error);
    /* RAM-backed NVRAM controller: IODTNVRAM gains a backend and the "IONVRAM"
     * resource is published (AppleImage4's NVRAM writes wait on it forever). */
    extern int ioswitch_nvram_publish(void);
    if (ioswitch_nvram_publish()) printf("IOSwitch: NVRAM controller not registered\n");
    ioswitch_board_start();
    ioswitch_root_record[2] = 5;
}
