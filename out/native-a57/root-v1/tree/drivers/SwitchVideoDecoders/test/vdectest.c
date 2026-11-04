// SPDX-License-Identifier: BSD-2-Clause-Patent
// vdectest: the clip check (AVD/ClipCheck.c) as a command: test/VDECClip.h's H.264 clip decoded through VideoToolbox
// twice, once requiring a hardware decoder and once with hardware disabled, every output frame's luma CRC compared
// with x264's reconstruction in display order. On a Mac it checks the clip and the check itself against Apple's
// decoders (tests/test_switch_video.py). On the Switch the same check runs inside AVD.videodecoder once per boot
// (launchd does not start jobs of ours: 21H461's launchd reads only its signed cache), logged under com.ioswitch.avd.
#include <os/log.h>
#include <stdio.h>
#include "../AVD/ClipCheck.h"

int main(void)
{
    os_log_t log = os_log_create("com.ioswitch.avd", "vdectest");
    char line[512];
    int failed = 0;
    for (int mode = ClipCheckHardware; mode <= ClipCheckSoftware; mode++) {
        failed += !clip_check((ClipCheckMode)mode, NULL, line, sizeof(line));
        os_log(log, "vdectest: %{public}s", line);
        printf("vdectest: %s\n", line);
    }
    return failed != 0;
}
