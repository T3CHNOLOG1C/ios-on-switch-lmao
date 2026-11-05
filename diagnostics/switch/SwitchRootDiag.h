/* Root-boot observations only. Stage numbers never authorize storage or exec. */
volatile uint64_t switch_root_record[24];
static unsigned root_boot_enabled;
static uint64_t root_last_draw;
static void root_screen(void)
{
    if (!root_boot_enabled || !switch_diag_record.ready || switch_diag_record.panicked) return;
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    if (switch_diag_console_active()) {
        char line[105];
        for (unsigned y = 120; y < 180; y++)
            for (unsigned x = 16; x < 1264; x++) switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
        (void)snprintf(line, sizeof(line), "ROOT %llu ERROR %llx AUTH %llu OWNER %llu CACHE %llx DATA %llx",
                       switch_root_record[2], switch_root_record[3], switch_root_record[8],
                       switch_root_record[9], switch_root_record[11], switch_root_record[12]);
        text(120, line, sizeof(line), switch_root_record[3] ? SWITCH_RED : SWITCH_GREEN);
        (void)snprintf(line, sizeof(line), "POLICY %llu/%llu VERIFIED SD %llu", switch_root_record[5],
                       switch_root_record[6], switch_root_record[7]);
        text(140, line, sizeof(line), SWITCH_WHITE);
        (void)snprintf(line, sizeof(line), "PID1 CALLS %llu EXEC %llx ROOT OWNER %llx MOBILE %llx",
                       switch_root_record[15], switch_root_record[13], switch_root_record[19],
                       switch_root_record[20]);
        text(160, line, sizeof(line), SWITCH_WHITE);
        console_flush();
        sync_pixels();
        (void)ml_set_interrupts_enabled(previous);
        return;
    }
    for (unsigned y = 340; y < 660; y++)
        for (unsigned x = 16; x < 1264; x++) switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    const uint64_t stage = switch_root_record[2];
    const char *message = switch_root_record[3] ? "NATIVE ROOT BOOT REFUSED OR FAILED"
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
    text(360, message, 104, switch_root_record[3] ? SWITCH_RED : SWITCH_GREEN);
    hex_line(400, "ROOT STAGE / ERROR=", (stage << 32) | switch_root_record[3]);
    hex_line(420, "POLICY DISK=", switch_root_record[4]);
    hex_line(440, "POLICY LOADED BYTES=", switch_root_record[5]);
    hex_line(460, "POLICY TOTAL BYTES=", switch_root_record[6]);
    hex_line(480, "AUTHENTICATED SD BYTES=", switch_root_record[7]);
    hex_line(500, "APPLE ROOT AUTH / OWNER=", (switch_root_record[8] << 32) | switch_root_record[9]);
    hex_line(520, "MOUNTED ROOT DEVICE=", switch_root_record[10]);
    hex_line(540, "CACHE / DATA RESULT=", (switch_root_record[11] << 32) | switch_root_record[12]);
    if (stage == 12) {
        hex_line(560, "DATA ROOT UID / GID=", switch_root_record[19]);
        hex_line(580, "DATA MOBILE UID / GID=", switch_root_record[20]);
        hex_line(600, "DATA MOUNT FLAGS=", switch_root_record[21]);
    } else {
        hex_line(560, "ORIGINAL LAUNCHD EXEC RESULT=", switch_root_record[13]);
        hex_line(580, "FIRST PID1 SYSCALL PC=", switch_root_record[14]);
        hex_line(600, "PID1 SYSCALL COUNT=", switch_root_record[15]);
    }
    hex_line(620, "LAST SYSCALL / ERROR=", (switch_root_record[16] << 32) | switch_root_record[17]);
    text(640, "OS READ ONLY - DATA WRITES STAY IN RAM", 104, SWITCH_WHITE);
    sync_pixels();
    (void)ml_set_interrupts_enabled(previous);
}
void switch_root_progress(unsigned stage, unsigned disk, uint64_t done, uint64_t total)
{
    if (stage & 0x80000000u)
        switch_root_record[3] = stage & 0x7fffffffu;
    else
        switch_root_record[2] = stage;
    switch_root_record[4] = disk;
    switch_root_record[5] = done;
    switch_root_record[6] = total;
    root_screen();
}
void switch_root_io_progress(uint64_t bytes, unsigned error)
{
    switch_root_record[7] = bytes;
    if (error && !switch_root_record[3]) switch_root_record[3] = 500 + error;
    uint64_t now, freq;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(now));
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(freq));
    if (error || (freq && now - root_last_draw >= freq)) {
        root_last_draw = now;
        root_screen();
    }
}
void switch_root_data_ownership(uint64_t flags, uint64_t root, uint64_t mobile)
{
    switch_root_record[19] = root;
    switch_root_record[20] = mobile;
    switch_root_record[21] = flags;
}
void switch_root_event(unsigned stage, int error, uint64_t detail)
{
    switch_root_record[2] = stage;
    if (stage == 8) {
        switch_root_record[8] = (unsigned)error;
        switch_root_record[9] = detail & 1;
        switch_root_record[10] = detail >> 32;
    } else {
        if (error && !switch_root_record[3]) switch_root_record[3] = (stage << 16) | (unsigned)error;
        if (stage == 10) switch_root_record[11] = (unsigned)error;
        if (stage == 12) switch_root_record[12] = (unsigned)error;
        if (stage == 13) {
            switch_root_record[13] = (unsigned)error;
            switch_root_record[18] = error == 0;
        }
    }
    root_screen();
}
static void root_syscall(unsigned code, int error, int pid, uint64_t pc, uint64_t cpsr)
{
    if (!root_boot_enabled || !switch_root_record[18] || pid != 1 || (cpsr & 31)) return;
    if (!switch_root_record[15]) switch_root_record[14] = pc;
    switch_root_record[15]++;
    switch_root_record[16] = code;
    switch_root_record[17] = (unsigned)error;
    switch_root_record[2] = 14;
    if (switch_root_record[15] == 1) switch_diag_stage("original PID1 EL0 syscall observed");
    if (switch_root_record[15] == 1 || error) root_screen();
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
extern int switch_motion_start(void);
extern int switch_sensors_start(void); /* thermalmonitord's HID temperature sensors, after SwitchThermal/SwitchBattery */
extern int switch_als_start(void);       /* on unless switch-als=0; after SwitchTouch, which powers the sensor (LDO6) */
extern int switch_bluetooth_start(void); /* no-op unless switch-bt=1; runs its bring-up on its own thread */
extern int switch_joycon_start(void);    /* no-op unless switch-joycon-rail=1 */
extern int switch_wlan_publish(void);    /* no-op unless switch-wlan=1; runs its bring-up on its own thread */
extern int tegra_gm20b_probe(void);      /* no-op unless tegra-gpu=<stage>; runs its bring-up on its own thread */
extern int tegra_host1x_probe(void);     /* no-op unless switch-host1x=1; own thread */
extern int tegra_nvdec_probe(void);      /* no-op unless switch-nvdec=1; own thread, starts host1x itself */
extern int tegra_nvenc_probe(void);      /* on with NVDEC (switch-nvdec=0 turns both off); own thread */
extern int tegra_vic_probe(void);        /* on by default; own thread, waits for NVDEC's bring-up to end */
extern int tegra_nvjpg_probe(void);      /* on by default; own thread, waits for NVDEC's bring-up to end */
extern int
switch_audio_publish(void); /* null sink unless switch-audio=1|3; the Tegra backend sleeps on I2C1, PZ4 and the GIC */
extern int switch_suspend_start(void); /* after every board driver it suspends */
extern int switch_usb_start(void); /* no-op without the ADT's /arm-io/usb-device; runs its bring-up on its own thread */
extern int switch_watchdog_publish(void); /* the IOWatchdog launchd starts watchdogd for (NativeSwitchWatchdog) */
extern int tegra_se_start(void);            /* SE2: known answers, kernel PRNG reseed, a timing run later; own thread */
static int switch_board_enabled(const char *name)
{
    unsigned enabled = 1;
    PE_parse_boot_argn(name, &enabled, sizeof(enabled));
    return enabled != 0;
}
static void switch_board_thread(void *parameter __unused, wait_result_t result __unused)
{
    /* First: returns at once and publishes the per-unit BD address before anything here can sleep. */
    if (switch_bluetooth_start()) printf("Switch: SwitchBluetooth did not start\n");
    /* Before anything that can sleep long: launchd must find the IOWatchdog when it reads its LaunchEvents. */
    if (switch_watchdog_publish()) printf("Switch: NativeSwitchWatchdog did not start\n");
    /* Early: returns at once; its thread reseeds the kernel PRNG before most of userspace has asked it for anything. */
    if (tegra_se_start()) printf("Switch: TegraSE did not start\n");
    /* After Bluetooth: rail pairing hands Joy-Cons the console's (fuse-derived) BD address. */
    if (switch_joycon_start()) printf("Switch: SwitchJoyCon did not start\n");
    if (switch_usb_start()) printf("Switch: SwitchUSB did not start\n");
    if (switch_board_enabled("switch-gpio") && tegra_gpio_start()) printf("Switch: TegraGPIO did not start\n");
    if (switch_board_enabled("switch-thermal") && switch_thermal_start())
        printf("Switch: SwitchThermal did not start\n");
    if (switch_board_enabled("switch-battery") && switch_battery_start())
        printf("Switch: SwitchBattery did not start\n");
    /* Right after its sources: the 38 services should exist before thermalmonitord first looks (it rebuilds a partial
     * list only every 20 s) and before watchdogd's open asks for them (it waits for them, at most 20 s). */
    if (switch_sensors_start()) printf("Switch: SwitchSensors did not start\n");
    if (switch_board_enabled("switch-touch") && switch_touch_start()) printf("Switch: SwitchTouch did not start\n");
    if (switch_board_enabled("switch-buttons") && switch_buttons_start())
        printf("Switch: SwitchButtons did not start\n");
    if (switch_als_start()) printf("Switch: SwitchALS did not start\n");
    if (switch_board_enabled("switch-motion") && switch_motion_start())
        printf("Switch: SwitchMotion did not start\n");
    if (switch_wlan_publish()) printf("Switch: Wi-Fi driver not started\n");
    if (tegra_gm20b_probe()) printf("Switch: TegraGM20B did not start\n");
    if (tegra_host1x_probe()) printf("Switch: TegraHost1x did not start\n");
    if (tegra_nvdec_probe()) printf("Switch: TegraNVDEC did not start\n");
    if (tegra_nvenc_probe()) printf("Switch: TegraNVENC did not start\n");
    if (tegra_vic_probe()) printf("Switch: TegraVIC did not start\n");
    if (tegra_nvjpg_probe()) printf("Switch: TegraNVJPG did not start\n");
    if (switch_audio_publish()) printf("Switch: SwitchAudio did not start\n");
    (void)switch_suspend_start();
    thread_terminate_self();
}
static void switch_board_start(void)
{
    thread_t thread;
    if (kernel_thread_start(switch_board_thread, NULL, &thread) != KERN_SUCCESS)
        panic("Switch: cannot start the board driver thread");
    thread_deallocate(thread);
}
void switch_root_boot_prepare(void)
{
    int enabled = 0, files = 0, legacy = 0;
    if (!PE_parse_boot_argn("switch-root-boot", &enabled, sizeof(enabled)) || enabled != 1)
        panic("native root boot requires explicit switch-root-boot=1");
    root_boot_enabled = 1;
    switch_root_record[0] = UINT64_C(0x53574348524f4f54);
    switch_root_record[1] = 1;
    switch_root_record[8] = switch_root_record[11] = switch_root_record[12] = switch_root_record[13] =
        UINT32_MAX;
    switch_root_record[19] = switch_root_record[20] = UINT64_MAX;
    (void)files;
    (void)legacy;
    if (switch_files_storage_profile() != SWITCH_STORAGE_RAW) panic("Switch: only switch-storage=raw is supported");
    /* The SD block driver owns the controller and finds the APFS partition;
     * IOKit and APFS publish the container and volumes from there. */
    int error = switch_block_publish();
    if (error) panic("Switch: SD block device publish failed (%d); see SwitchSD lines above", error);
    /* NVRAM controller: IODTNVRAM gains a backend and the "IONVRAM" resource is
     * published (AppleImage4's NVRAM writes wait on it forever). It keeps the
     * NVRAM in RAM until Data is mounted, then in /private/var/Switch/nvram.bin. */
    extern int switch_nvram_publish(void);
    if (switch_nvram_publish()) printf("Switch: NVRAM controller not registered\n");
    switch_board_start();
    switch_root_record[2] = 5;
}
