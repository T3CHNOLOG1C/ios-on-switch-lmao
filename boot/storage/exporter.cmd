# The exporter lies inside the existing excluded kernel-staging allocation.
# File-test builds cap the kernel below A7000000; this wrapper is at A7100000.
# Publish the exact executable using the same load-only/cache sequence already
# exercised by the native CPU probe. The following main bootm start resets
# U-Boot's temporary image state before preparing the actual kernel handoff.
if test "${handoff_status}" = "READY"; then
    if fatsize mmc ${devnum}:${distro_bootpart} /switchroot/ioswitch-native-xnu/exporter.uimg; then
        if itest.l ${filesize} == @EXPORTER_BYTES_HEX@; then
            if load mmc ${devnum}:${distro_bootpart} 0xA7100000 /switchroot/ioswitch-native-xnu/exporter.uimg @EXPORTER_BYTES_HEX@; then
                if itest.l ${filesize} == @EXPORTER_BYTES_HEX@ && crc32 -v 0xA7100000 @EXPORTER_BYTES_HEX@ @EXPORTER_CRC32@; then
                    if setenv verify yes && setenv autostart no && bootm start 0xA7100000; then
                        if bootm loados; then
                            if crc32 -v 0xA7000000 @EXPORTER_RAW_BYTES_HEX@ @EXPORTER_RAW_CRC32@; then
                                echo "CACHE HELPERS: RC=1 EXPECTED"
                                go 0xAA001920
                                go 0xAA001A64
                                echoe "SD EXPORTER READY"
                            else
                                setenv handoff_status EXPORTER_DESTINATION_CHECK_FAILED
                            fi
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
