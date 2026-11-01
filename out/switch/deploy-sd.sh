#!/bin/zsh
# Deploy: save the last boot's Data logs, copy the boot package to the SD, unmount the iPad APFS container, eject.
# Touches only switchroot/ioswitch/* and bootloader/ini/ioswitch.ini on the FAT volume; on the APFS Data volume it reads
# logs and replaces only ioswitch/overlay (the userland overlay), plus the Metal bundle with STAGE_GPU=1.
# Usage: PKG=out/boot/lkg/<snapshot> [STAGE_GPU=1] zsh out/switch/deploy-sd.sh
set -eu
ROOT=/Users/brunostjohn/Development/Projects/ioswitch
SD="/Volumes/SWITCH SD"
PKG=${PKG:-$ROOT/out/boot/ioswitch}
LOGS=$ROOT/out/switch/device-logs/boot-$(date +%m%d-%H%M)
[ -d "$SD" ] || { echo "SD not mounted"; exit 1; }
disk=$(diskutil info "$SD" | awk '/Part of Whole/ {print $4}')
container=$(diskutil apfs list | awk -v store="${disk}s9" '/APFS Container Reference/ {ref=$NF} $0 ~ "Physical Store Disk:.*" store {print ref; exit}')
echo "SD whole disk: $disk, APFS container: ${container:-none}"

# 1. Logs from the Data volume (read-only copy).
data=$(mount | awk '/IOSwitchDataTrial/ {print $3}')
if [ -n "$data" ]; then
  mkdir -p "$LOGS"
  cp -RX "$data/db/diagnostics" "$LOGS/diagnostics"
  cp -RX "$data/db/uuidtext" "$LOGS/uuidtext"
  cp -X "$data/log/com.apple.xpc.launchd/launchd.log" "$LOGS/" 2>/dev/null || true
  [ -d "$data/mobile/Library/Logs/CrashReporter" ] && cp -RX "$data/mobile/Library/Logs/CrashReporter" "$LOGS/mobile-CrashReporter"
  # Every process's m2v shader cache to $LOGS/m2v (backboardd's also as $LOGS/m2v-cache), and the render pipeline states and
  # specializations their journals name merged into the precompile corpus the next Metal bundle build seeds from. Read-only but
  # for the mode-000 Library and Library/Caches of the system containers it reads, opened and closed again (harvest.py).
  harvest=$ROOT/out/native-a57/root-v1/tree/drivers/TegraGM20BMetal/precompile/harvest.py
  if [ -f "$harvest" ]; then
    python3 "$harvest" "${harvest:h}/corpus.log" --volume "$data" --save "$LOGS/m2v" || echo "m2v journals NOT harvested"
    bbd=m2v/System_6C7A65AF-CB18-421C-A07A-D724B05A7AB6_Library_Caches_m2v
    [ -d "$LOGS/$bbd" ] && ln -sfn "$bbd" "$LOGS/m2v-cache"
  fi
  # LaunchServices database (read-only copy). Safari's web-process launch fails at its LaunchServices lookup of the WebKit
  # extensions by bundle ID; lsdb-webkit.txt counts each ID's occurrences, with a sibling from the same directory last.
  # 0 for the WebKit IDs and >0 for com.apple.fskit.exfat means LaunchServices never registered the WebKit extensions.
  lsdb=( "$data"/mobile/Containers/Data/InternalDaemon/*/Library/Caches/com.apple.LaunchServices-*.csstore(N) )
  if (( ${#lsdb} )) && cp -X "${lsdb[@]}" "$LOGS/"; then
    for id in com.apple.WebKit.WebContent com.apple.WebKit.Networking com.apple.WebKit.GPU \
        com.apple.WebKit.WebContent.CaptivePortal WebContentExtension.appex com.apple.fskit.exfat; do
      echo "$id $(LC_ALL=C grep -a -c -F "$id" "${lsdb[@]}")"
    done > "$LOGS/lsdb-webkit.txt"
  else
    echo "LaunchServices database NOT copied (not found or not readable)"
  fi
  echo "logs -> $LOGS"
fi

# 2. The package, file by file, then our AppleDouble files by name.
cp -X "$PKG/bootloader/ini/ioswitch.ini" "$SD/bootloader/ini/ioswitch.ini"
rm -f "$SD/bootloader/ini/._ioswitch.ini"
for f in "$PKG"/switchroot/ioswitch/*; do
  name=${f:t}
  cp -X "$f" "$SD/switchroot/ioswitch/$name"
  rm -f "$SD/switchroot/ioswitch/._$name"
done
ls -la "$SD/switchroot/ioswitch"
sync

# 2b. The userland overlay, as a whole, into <Data>/ioswitch/overlay.
if [ -n "$data" ] && [ -d "$PKG/overlay" ]; then
  if rm -rf "$data/ioswitch/overlay" && mkdir -p "$data/ioswitch" && cp -RX "$PKG/overlay" "$data/ioswitch/overlay"; then
    find "$data/ioswitch/overlay" -name '._*' -delete
    echo "overlay -> $data/ioswitch/overlay"
  else
    echo "overlay NOT copied (is the Data volume mounted read-write?)"
  fi
fi

# 2c. GPU stage 4 only: the Metal plugin bundle from out/gpu into <Data>/ngpu/addons.
if [ "${STAGE_GPU:-0}" = 1 ] && [ -n "$data" ]; then
  gpu=$ROOT/out/gpu; [ -d "$PKG/gpu/ngpu" ] && gpu=$PKG/gpu   # the snapshot's own bundle (matches its trust cache)
  python3 $ROOT/out/native-a57/root-v1/tree/drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py "$data" --build "$gpu" \
    || echo "GPU bundle NOT staged"
fi

# 3. Unmount the iPad volumes, then eject the card.
[ -n "${container:-}" ] && diskutil unmountDisk "$container"
diskutil eject "$disk"
