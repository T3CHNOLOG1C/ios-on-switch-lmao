# Run the guarded iPadOS experiment on the Switch

**Superseded deployment approach:** the owner clarified that iPadOS must execute natively, not inside Linux/QEMU. The transfer was stopped before the full console guest started. The instructions below describe the earlier experimental wrapper only; they are not the current native installation path. No native bootable release is available yet.

The owner's Switch OLED already boots Switchroot Ubuntu Noble through Hekate. The current experiment runs inside that Linux installation. It does not require another Hekate entry, a new partition or an Android change.

The small XNU userspace test has passed on this console. The full iPadOS bundle has reached Home on the Mac; its first full console trial is pending. Apps still have rendering problems, and guest Data resets at each launch.

## Files and desktop

The working directory on the Switch is `/home/brunostjohn/ioswitch`. Keep Linux open and the charger connected. The transfer must finish before running the guest; the launcher verifies every bundled file before QEMU starts.

The dedicated folder needs `ioswitch.py`, `scripts/` and `bundles/ipados-17.7.11-tcg/`. Its disks belong on the Linux filesystem: the OS file exceeds FAT32's file-size limit. Ubuntu's QEMU 8.2.2 and AArch64 firmware packages are installed on this console.

Open a terminal in the Linux desktop:

```sh
cd /home/brunostjohn/ioswitch
python3 scripts/switch_trial.py \
  --bundle bundles/ipados-17.7.11-tcg \
  --display gtk --fullscreen \
  --tcg-cache-mib 256 --seconds 1800
```

Run the command as the normal desktop user. The wrapper uses `sudo` to create a temporary service but runs the guest as that user. It obtains the local desktop display credentials; SSH display forwarding is unnecessary.

The command prints the trial directory and an exact `sudo systemctl stop ...` command. Save that stop command. Starting a service is not evidence that iPadOS booted.

## What the wrapper enforces

The trial has a 3 GiB resident-memory cgroup cap, a finite runtime, and a watchdog. The watchdog refuses an unverified memory cap and stops the guest if Linux has less than 384 MiB available memory, any observed thermal zone reaches 80°C, or the battery is at most 5% while discharging or its status is unknown. The existing zram swap remains in use; the RAM cap is not a combined RAM-plus-swap cap.

The first live watchdog acceptance test verified the cgroup-v1 cap and sensor reads with a harmless three-second command. It did not deliberately overheat the device, exhaust memory or establish full-guest memory fit. The [receipt](../out/switch/guard-acceptance-1/summary.json) records that distinction.

The trial directory contains `launch.json`, `guard/summary.json`, `guard/guard.jsonl`, `guard/child.log`, and the guest's `runs/` directory. The guest records serial output, sampled QEMU memory and periodic screenshots. An interrupted or failed trial should be checked through these files before retrying.

## Input and completion

Home or F1 sends the iPad Home action. A left click/drag is a single finger. Physical touchscreen forwarding still needs to be checked on this console; a successful remote QEMU click does not prove it. Ordinary guest keyboard typing, networking, audio and persistent Data are not implemented in this runner.

Use the printed stop command to end the trial early. A separate temporary service, `ioswitch-awake.service`, currently inhibits automatic sleep for our Linux work. When that work is finished, release it with:

```sh
sudo systemctl stop ioswitch-awake.service
```

This inhibitor changes no persistent sleep settings. Its current service also has an eight-hour expiry as a backstop. Its active state can be checked with `systemctl is-active ioswitch-awake.service` and `systemd-inhibit --list`.

References: [systemd inhibitor behavior](https://www.freedesktop.org/software/systemd/man/latest/systemd-inhibit.html), [resource control](https://www.freedesktop.org/software/systemd/man/latest/systemd.resource-control.html), and the [console evidence](verification.md).
