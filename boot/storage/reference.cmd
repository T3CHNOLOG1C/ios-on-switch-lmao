        # Capture only after bootm has loaded the complete adapter and prepared
        # its FDT. The final4KiB of the already excluded128KiB adapter allocation
        # is outside every ELF segment and both stacks (checked by the builder).
        # Explicit raw device1 avoids cmd/mmc's unrelated current-device state.
        # A successful firmware block read may use its same-boot block cache.
        if test "${devnum}" = "1"; then
            if mw.b 0xA001F000 0 0x1000; then
                if read mmc 1 0xA001F200 0 1; then
                    # U-Boot CRC32_VERIFY stores its four-byte digest in BE.
                    # All metadata is LE32; commit remains0 for loader validation.
                    if crc32 0xA001F200 0x200 0xA001F028 && mw.l 0xA001F004 1 1 && mw.l 0xA001F008 0x400 1 && mw.l 0xA001F00C 0x200 1 && mw.l 0xA001F010 0x200 1 && mw.l 0xA001F014 0x700B0000 1 && mw.l 0xA001F018 0 1 && mw.l 0xA001F01C 1 1 && mw.l 0xA001F020 1 1 && mw.l 0xA001F024 0 1 && mw.l 0xA001F02C 0 1 && mw.l 0xA001F000 0x52445349 1; then
                        echoe "SD REFERENCE READY"
                    else
                        setenv handoff_status SD_REFERENCE_METADATA_FAILED
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
