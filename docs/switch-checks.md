# Check your Switch OLED for IOSwitch

Your Switch runs **Switchroot Ubuntu Noble (24.04)**. This first pass records its exact kernel, available RAM, installed QEMU, and GPU capabilities. You can do it whenever you have the console handy. It does not boot iPadOS, install packages, change Hekate, or write to eMMC.

You need your Switch's existing **Linux desktop**, a way to copy a small ZIP to it, and a terminal. Android is not the environment for these commands. No SSH setup is needed.

## 1. Copy the small checks package

On the Mac, use `out/ioswitch-checks.zip` in this IOSwitch project. The archive contains only this guide and `collect_switch.py`; you do not need the large `vendor`, firmware or build folders.

Copy the ZIP to the Switch using a USB drive or your usual file-transfer method. Boot your existing Switchroot Linux entry through Hekate, open its file manager, and extract the archive into **Downloads**. The result should be:

```text
Downloads/
  ioswitch-checks/
    README.md
    collect_switch.py
```

If you prefer to copy two files directly, put this guide and `scripts/collect_switch.py` into a folder named `ioswitch-checks` in the Switch's Downloads folder. Keep the script named `collect_switch.py`.

## 2. Open a terminal on the Switch

Use the Linux desktop's Terminal app. Enter:

```sh
cd ~/Downloads/ioswitch-checks
python3 --version
```

The collector supports **Python 3.8 or newer**. If `python3` is missing or older, send me that output and the Linux release shown in the desktop's system information. You do not need to install anything for now.

If `cd` says the folder does not exist, find the extracted `ioswitch-checks` folder in the file manager and use **Open in Terminal**. That avoids differences in the Downloads folder's name or location.

## 3. Collect the report

Keep the Linux desktop running and use your normal user account. Run:

```sh
python3 collect_switch.py --graphics --storage-path "$HOME" --output switch-report.json
```

The command is quiet on success. It creates `switch-report.json` in the same folder. Individual external checks have a 15-second timeout; allow a couple of minutes if several tools are slow. It only uses tools already installed on the Switch.

The report covers:

- Linux distribution, kernel, CPU features and device-tree identity.
- Total/available RAM, existing swap and zram statistics.
- Free space on the filesystem containing your Linux home folder.
- QEMU version, listed CPU models, accelerators and display backends.
- Whether `/dev/kvm` is accessible and answers its API-version query.
- Vulkan/OpenGL device and feature information, if the diagnostic tools are installed.

The KVM query creates no virtual machine. This script does not upload anything. It filters hostnames, network addresses, serial numbers, UUIDs and local paths from its report; it is still sensible to read the JSON before sharing it.

## 4. Send the report back

Open `switch-report.json` in the file manager and send it in this chat, or copy its text into the chat. You can print it in the terminal with:

```sh
cat switch-report.json
```

That is all I need for this pass. The report will let me choose compatible dependencies and judge whether the first bounded iPadOS experiment has enough memory and storage.

## If something looks wrong

| What you see | What to do |
| --- | --- |
| `output already exists` | Preserve the first report. Rerun with `--output switch-report-2.json`. |
| `unavailable`, `null` or `not_requested` fields | Send the report as it is. These usually mean a tool or observation was absent, not that the hardware cannot do it. |
| KVM `EACCES` | Send it as it is; it records your normal user's permissions. No need to rerun the whole script with `sudo`. |
| Missing `vulkaninfo`, `glxinfo` or QEMU | Send the report. We can select Noble packages after seeing what is already installed. |
| A graphics query times out | Try `python3 collect_switch.py --storage-path "$HOME" --output switch-report-basic.json` and send that report. |
| Python prints a traceback | Copy the traceback into this chat. |
| Report says `Darwin` or describes the Mac | The script ran on the Mac. Run it from the Switch's Linux terminal instead. |

## What happens after this

I will compare your report with the locally tested build, then provide the matching transfer and launch instructions. The portable image needs a Linux filesystem capable of files larger than 4 GiB; the Hekate FAT32 boot partition is unsuitable for the OS disk.

The intended path is **Hekate → your existing Switchroot Linux → QEMU → iPadOS**. A successful inventory is not a Switch boot test. Memory fit, boot time, touch response and app rendering will be measured on the console after the Mac build passes its checks.
