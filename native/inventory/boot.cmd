# IOSwitch read-only firmware inventory. No new CPU payload or OS handoff.
# Only existing firmware display, environment and inspection commands execute.
# Changing stdout and the working-FDT pointer affects this boot session only.
echoe "IOSWITCH MEMORY CHECK: SCRIPT ENTERED"
if setenv stdout vidconsole; then
    echoe "IOSWITCH MEMORY CHECK: SCREEN OUTPUT SELECTED"
else
    echoe "CANNOT SELECT SCREEN OUTPUT - HOLDING"
    while true; do
        sleep 1
    done
fi
if test "${t210b01}" != "1" -o "${sku}" != "3"; then
    echoe "WRONG BOARD - OLED ONLY - HOLDING"
    while true; do
        sleep 1
    done
fi
setenv inventory_fdt UNAVAILABLE
if test -n "${fdtcontroladdr}"; then
    # No length argument: this sets a pointer and validates the existing header.
    # It does not resize, copy or modify the control tree.
    if fdt addr ${fdtcontroladdr}; then
        setenv inventory_fdt READY
    fi
fi

# Fifty empty lines scroll away the previous page without relying on ANSI/cls.
# Reset the pixel cursor only after clearing. Each page stays for 20 seconds.
while true; do
    for inventory_row in 1 2 3 4 5 6 7 8 9 10; do
        echo
        echo
        echo
        echo
        echo
    done
    setcurs 0 0
    echo "IOSWITCH MEMORY CHECK - PAGE 1 OF 3 - U-BOOT RAM"
    echo "Photograph all three pages; changes every 20 seconds and repeats."
    echo "Exit: hold POWER at least 20 seconds; then start Hekate normally."
    echo
    if bdinfo; then
        echo "BOARD INFO READ COMPLETE"
    else
        echo "BOARD INFO COMMAND FAILED"
    fi
    echo
    echo "Board: t210b01=${t210b01} sku=${sku}"
    echo "These are firmware banks, not a proved XNU allocation window."
    sleep 20

    for inventory_row in 1 2 3 4 5 6 7 8 9 10; do
        echo
        echo
        echo
        echo
        echo
    done
    setcurs 0 0
    echo "IOSWITCH MEMORY CHECK - PAGE 2 OF 3 - DISPLAY / CONTROL TREE"
    echo "Photograph this page. Next page in 20 seconds; all pages repeat."
    echo "Exit: hold POWER at least 20 seconds; then start Hekate normally."
    echo
    echo "Control FDT: ${fdtcontroladdr}; read status: ${inventory_fdt}"
    if test "${inventory_fdt}" = "READY"; then
        echo "--- FIRMWARE FRAMEBUFFER ---"
        fdt print /framebuffer
        echo "--- CONTROL-TREE MEMORY (MAY BE A TEMPLATE) ---"
        fdt print /memory
        echo "--- CONTROL-TREE RESERVE MAP ---"
        fdt rsvmem print
    else
        echo "CONTROL FDT UNAVAILABLE - NO MEMORY READ ATTEMPTED"
    fi
    echo
    echo "This is the firmware control tree, not the selected kernel tree."
    sleep 20

    for inventory_row in 1 2 3 4 5 6 7 8 9 10; do
        echo
        echo
        echo
        echo
        echo
    done
    setcurs 0 0
    echo "IOSWITCH MEMORY CHECK - PAGE 3 OF 3 - CARVEOUT REGISTERS"
    echo "Photograph this page. Page 1 returns in 20 seconds."
    echo "Exit: hold POWER at least 20 seconds; then start Hekate normally."
    echo
    # Exact read-only fields used by this firmware's DRAM-bank calculation.
    # SEC/VPR use MiB sizes, MTS low/size/high, GSC low/high/128-KiB count.
    # No control, status-clear or write registers are included.
    base 0
    echo "MC_EMEM_CFG: raw DRAM configuration"
    md.l 0x70019050 1
    echo "SEC low,size_MiB / high"
    md.l 0x70019670 2
    md.l 0x700199D4 1
    echo "MTS low,size_MiB,high"
    md.l 0x700199A0 3
    echo "VPR low,size_MiB / high"
    md.l 0x70019648 2
    md.l 0x70019978 1
    echo "GSC1..5 each: low,high,size_128KiB"
    md.l 0x70019C0C 3
    md.l 0x70019C5C 3
    md.l 0x70019CAC 3
    md.l 0x70019CFC 3
    md.l 0x70019D4C 3
    echo
    echo "Firmware inventory only. No kernel has been started."
    sleep 20
done
