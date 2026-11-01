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
