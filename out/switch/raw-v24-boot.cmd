# IOSwitch native XNU experiment v17 (persistent Data; cacheable copy). Generated constants pin both inputs.
# This script reads bounded payload files and mutates their RAM staging areas.
# All script refusals hold. bootm go is a nonreturning kernel handoff: returning
# from the native entry would trigger U-Boot's own reset path before this script.
# The screen stays dark until the loader draws: U-Boot's output (and echoe's stderr) goes to the serial console only.
# The video console must still exist (the refusal below writes to it); a refusal switches back to it.
# Boot timing (native/loader/stamps.h): setexpr.l samples the microsecond timer (TIMERUS_CNTR_1US, 0x60005010) after
# each step; just before bootm go the samples go to the loader's slot at 0xA001EF00. Diagnostic only: nothing here
# depends on them. ios_uboot comes from the hekate entry (its scan_dev_for_extlinux).
setexpr.l ios_script *0x60005010
if setenv stdout vidconsole; then
    setenv stdout serial
    setenv stderr serial
else
    echoe "CONSOLE FAILED"
    while true; do
        sleep 1
    done
fi
base 0
setenv handoff_status READY
if test "${t210b01}" != "1" -o "${sku}" != "3"; then
    setenv handoff_status WRONG_BOARD
fi

# These original BL33 instruction words are an additional runtime fingerprint.
# File-test revisions use these leafs to publish the exporter. Final bootm
# performs the normal cache/MMU cleanup before the native kernel handoff.
if test "${handoff_status}" = "READY"; then
    if itest.l *0xAA001920 == 0xd508711f && itest.l *0xAA001924 == 0xd5033fdf && itest.l *0xAA001928 == 0xd65f03c0 && itest.l *0xAA001A64 == 0xd5033f9f && itest.l *0xAA001A68 == 0xd5033fdf && itest.l *0xAA001A6C == 0xd65f03c0; then
        true
    else
        setenv handoff_status FIRMWARE_MISMATCH
    fi
fi

# Inspect only the existing control FDT. Never dereference a framebuffer address
# obtained from it: root's native code uses the separately verified fixed range.
if test "${handoff_status}" = "READY"; then
    setenv handoff_fb_addr
    setenv handoff_fb_size_bytes
    setenv handoff_fb_width
    setenv handoff_fb_height
    setenv handoff_fb_format
    setenv handoff_fb_rot
    if test -n "${fdtcontroladdr}" && fdt addr ${fdtcontroladdr}; then
        # get-value wrongly treats 00384000 as a printable string and returns
        # empty. get-addr has an undersized pointer-format buffer in this fork.
        # Verify the size property's four-byte width and print its value only;
        # numeric extent is pinned by original BL33 and the hardware photos.
        if fdt get value handoff_fb_addr /framebuffer address && fdt get size handoff_fb_size_bytes /framebuffer size && fdt get value handoff_fb_width /framebuffer width && fdt get value handoff_fb_height /framebuffer height && fdt get value handoff_fb_format /framebuffer format && fdt get value handoff_fb_rot /framebuffer rot; then
            if itest.l ${handoff_fb_addr} == 0xF5A00000 && itest.l ${handoff_fb_size_bytes} == 4 && itest.l ${handoff_fb_width} == 0x2D0 && itest.l ${handoff_fb_height} == 0x500 && test "${handoff_fb_format}" = "a8b8g8r8" && itest.l ${handoff_fb_rot} == 3; then
                true
                fdt print /framebuffer size
            else
                setenv handoff_status FRAMEBUFFER_MISMATCH
            fi
        else
            setenv handoff_status FRAMEBUFFER_PROPERTY_FAILED
        fi
    else
        setenv handoff_status CONTROL_FDT_FAILED
    fi
fi

if test "${handoff_status}" = "READY"; then
    if fatsize mmc ${devnum}:${distro_bootpart} /switchroot/ioswitch-native-xnu-v23/loader.uboot.bin; then
        if itest.l ${filesize} == 0x1c040; then
            true
        else
            setenv handoff_status IMAGE_SIZE_FAILED
        fi
    else
        setenv handoff_status IMAGE_STAT_FAILED
    fi
fi
if test "${handoff_status}" = "READY"; then
    if load mmc ${devnum}:${distro_bootpart} 0xA0100000 /switchroot/ioswitch-native-xnu-v23/loader.uboot.bin 0x1c040; then
        if itest.l ${filesize} == 0x1c040; then
            echoe "LOADER READY"
            setexpr.l ios_loader *0x60005010
        else
            setenv handoff_status IMAGE_READ_CHECK_FAILED
        fi
    else
        setenv handoff_status IMAGE_READ_FAILED
    fi
fi

if test "${handoff_status}" = "READY"; then
    if fatsize mmc ${devnum}:${distro_bootpart} /switchroot/ioswitch-native-xnu-v10/oled-sku3.dtb; then
        if itest.l ${filesize} == 0x28c52; then
            true
        else
            setenv handoff_status DTB_SIZE_FAILED
        fi
    else
        setenv handoff_status DTB_STAT_FAILED
    fi
fi
if test "${handoff_status}" = "READY"; then
    if load mmc ${devnum}:${distro_bootpart} 0x8D000000 /switchroot/ioswitch-native-xnu-v10/oled-sku3.dtb 0x28c52; then
        if itest.l ${filesize} == 0x28c52; then
            echoe "DTB READY"
            setexpr.l ios_dtb *0x60005010
        else
            setenv handoff_status DTB_READ_CHECK_FAILED
        fi
    else
        setenv handoff_status DTB_READ_FAILED
    fi
fi

# Stage the kernel window (packed: its nonzero extents, LZ4-compressed, back to
# back; the loader decodes it into XNU's window) after the size check. Files are
# checked by size and header only (no CRC passes): FAT reads have their own card
# CRC. fatsize is the size check: this U-Boot's FatFS glue sets filesize to the
# length asked of load, not the file's, so the itest after each load always holds.
if test "${handoff_status}" = "READY"; then
    if fatsize mmc ${devnum}:${distro_bootpart} /switchroot/ioswitch-native-xnu-v23/kernel-window.bin; then
        if itest.l ${filesize} == 0x4324000; then
            true
        else
            setenv handoff_status KERNEL_SIZE_FAILED
        fi
    else
        setenv handoff_status KERNEL_STAT_FAILED
    fi
fi
if test "${handoff_status}" = "READY"; then
    if load mmc ${devnum}:${distro_bootpart} 0xA2000000 /switchroot/ioswitch-native-xnu-v23/kernel-window.bin 0x4324000; then
        if itest.l ${filesize} == 0x4324000; then
            echoe "KERNEL LOADED"
            setexpr.l ios_window *0x60005010
        else
            setenv handoff_status KERNEL_READ_CHECK_FAILED
        fi
    else
        setenv handoff_status KERNEL_READ_FAILED
    fi
fi

# Select raw only in the staged XNU command; the inherited payload stays intact.
if test "${handoff_status}" = "READY"; then
    if fatsize mmc ${devnum}:${distro_bootpart} /switchroot/ioswitch-native-raw-v24/raw-args.bin; then
        if itest.l ${filesize} == 0x400; then
            if load mmc ${devnum}:${distro_bootpart} 0xa617c06c /switchroot/ioswitch-native-raw-v24/raw-args.bin 0x400; then
                if itest.l ${filesize} == 0x400; then
                    echoe "APFS PROFILE READY"
                else
                    setenv handoff_status RAW_ARGS_READ_SIZE_FAILED
                fi
            else
                setenv handoff_status RAW_ARGS_READ_FAILED
            fi
        else
            setenv handoff_status RAW_ARGS_SIZE_FAILED
        fi
    else
        setenv handoff_status RAW_ARGS_STAT_FAILED
    fi
fi

# Only the copied working tree is changed. fdt addr's length is hexadecimal.
# This fork may return success even if fdt_open_into reports an error, so check
# the big-endian totalsize through an explicit little-endian word comparison.
# Reserve256KiB for FDT; open to192KiB; bootm prep adds12KiB => at most204KiB.
if test "${handoff_status}" = "READY"; then
    if fdt addr 0x8D000000 0x30000; then
        if itest.l *0x8D000000 == 0xedfe0dd0 && itest.l *0x8D000004 == 0x00000300; then
            if fdt rsvmem add 0xF5A00000 0x400000; then
                echoe "FDT READY"
            else
                setenv handoff_status FRAMEBUFFER_RESERVE_FAILED
            fi
        else
            setenv handoff_status DTB_PADDING_FAILED
        fi
    else
        setenv handoff_status DTB_HEADER_FAILED
    fi
fi

# The exporter lies inside the existing excluded kernel-staging allocation.
# File-test builds cap the kernel below A7000000; this wrapper is at A7100000.
# Publish the exact executable using the same load-only/cache sequence already
# exercised by the native CPU probe. The following main bootm start resets
# U-Boot's temporary image state before preparing the actual kernel handoff.
if test "${handoff_status}" = "READY"; then
    if fatsize mmc ${devnum}:${distro_bootpart} /switchroot/ioswitch-native-xnu-v10/exporter.uimg; then
        if itest.l ${filesize} == 0x5e90; then
            if load mmc ${devnum}:${distro_bootpart} 0xA7100000 /switchroot/ioswitch-native-xnu-v10/exporter.uimg 0x5e90; then
                if itest.l ${filesize} == 0x5e90; then
                    if setenv verify no && setenv autostart no && bootm start 0xA7100000; then
                        if bootm loados; then
                            echo "CACHE HELPERS: RC=1 EXPECTED"
                            go 0xAA001920
                            go 0xAA001A64
                            echoe "SD EXPORTER READY"
                            setexpr.l ios_exporter *0x60005010
                        else
                            setenv handoff_status EXPORTER_LOAD_FAILED
                        fi
                    else
                        setenv handoff_status EXPORTER_HEADER_FAILED
                    fi
                else
                    setenv handoff_status EXPORTER_READ_CHECK_FAILED
                fi
            else
                setenv handoff_status EXPORTER_READ_FAILED
            fi
        else
            setenv handoff_status EXPORTER_SIZE_FAILED
        fi
    else
        setenv handoff_status EXPORTER_STAT_FAILED
    fi
fi

if test "${handoff_status}" = "READY"; then
    # These environment changes exist only for this boot. No saveenv is used.
    # All-ones fdt_high is the exact ARM64 U-Boot in-place FDT contract.
    if setenv fdt_high ffffffffffffffff && setenv bootm_low 0x80000000 && setenv bootm_size 0x7FD00000 && setenv bootm_mapsize 0x7FD00000 && setenv bootargs ioswitch_native_xnu=1 && setenv verify no; then
        if bootm start 0xA0100000 - 0x8D000000; then
            if bootm loados; then
                echoe "LOADER LOADED"
            else
                setenv handoff_status IMAGE_LOAD_FAILED
            fi
        else
            setenv handoff_status BOOT_HEADER_FAILED
        fi
    else
        setenv handoff_status BOOT_ENVIRONMENT_FAILED
    fi
fi
if test "${handoff_status}" = "READY"; then
    echoe "PREPARE FDT"
    # prep performs in-place FDT padding, real-bank/PSCI/GPU-node fixups, then
    # shrinks/reserves the FDT. It can visibly hang on firmware preparation error.
    if bootm prep; then
        setexpr.l ios_prep *0x60005010
        # Capture the reference and active initialized card state after final
        # bootm prep. Exporter reads cached firmware RAM; it performs no card
        # commands, DMA, initialization or filesystem/device writes.
        if test "${devnum}" = "1"; then
            if mw.b 0xA001F000 0 0x1000; then
                if read mmc 1 0xA001F200 0 1; then
                    echoe "READ SD METADATA"
                    if go 0xA7000000; then
                        if itest.l *0xA001F400 == 0x4d435349 && itest.l *0xA001F404 == 1 && itest.l *0xA001F408 == 0x100; then
                            if crc32 0xA001F200 0x200 0xA001F028 && mw.l 0xA001F004 2 1 && mw.l 0xA001F008 0x500 1 && mw.l 0xA001F00C 0x200 1 && mw.l 0xA001F010 0x200 1 && mw.l 0xA001F014 0x700B0000 1 && mw.l 0xA001F018 0 1 && mw.l 0xA001F01C 1 1 && mw.l 0xA001F020 1 1 && mw.l 0xA001F024 0 1 && mw.l 0xA001F02C 0 1 && mw.l 0xA001F000 0x52445349 1; then
                                echoe "SD METADATA READY"
                            else
                                setenv handoff_status SD_REFERENCE_METADATA_FAILED
                            fi
                        else
                            setenv handoff_status SD_CARD_RECORD_FAILED
                        fi
                    else
                        setenv handoff_status SD_CARD_EXPORT_FAILED
                    fi
                else
                    setenv handoff_status SD_REFERENCE_READ_FAILED
                fi
            else
                setenv handoff_status SD_REFERENCE_CLEAR_FAILED
            fi
        else
            setenv handoff_status SD_REFERENCE_WRONG_DEVICE
        fi
        if test "${handoff_status}" = "READY"; then
            setexpr.l ios_go *0x60005010
            # The loader's timing slot (native/loader/stamps.h): samples, then the magic. A missing one stays 0.
            mw.l 0xA001EF00 0 0x40
            if env exists ios_uboot; then mw.l 0xA001EF04 ${ios_uboot} 1; fi
            if env exists ios_script; then mw.l 0xA001EF08 ${ios_script} 1; fi
            if env exists ios_loader; then mw.l 0xA001EF0C ${ios_loader} 1; fi
            if env exists ios_dtb; then mw.l 0xA001EF10 ${ios_dtb} 1; fi
            if env exists ios_window; then mw.l 0xA001EF14 ${ios_window} 1; fi
            if env exists ios_exporter; then mw.l 0xA001EF18 ${ios_exporter} 1; fi
            if env exists ios_prep; then mw.l 0xA001EF1C ${ios_prep} 1; fi
            mw.l 0xA001EF20 ${ios_go} 1
            mw.l 0xA001EF00 0x544f4255 1
            echoe "HANDOFF"
            bootm go
            setenv handoff_status UNEXPECTED_BOOTM_RETURN
        fi
    else
        setenv handoff_status BOOT_PREP_FAILED
    fi
fi
setenv stdout vidconsole
setenv stderr vidconsole
echoe "REFUSED: ${handoff_status}"
while true; do
    sleep 1
done
