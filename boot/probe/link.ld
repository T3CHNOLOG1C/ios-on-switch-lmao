ENTRY(_start)
SECTIONS {
    . = 0;
    .probe : { KEEP(*(.probe)) }
    /DISCARD/ : { *(.comment) *(.note*) }
}
ASSERT(probe_result == 0x200, "result offset must be 0x200")
ASSERT(SIZEOF(.probe) == 0x300, "probe must be exactly 768 bytes")
