ENTRY(_start)
SECTIONS {
    . = 0xA0000000;
    __image_start = .;
    .text : { KEEP(*(.text.entry)) *(.text*) }
    .vectors ALIGN(2048) : { KEEP(*(.vectors)) }
    . = ALIGN(4096);
    __loader_rx_end = .;
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    __loader_ro_end = .;
    .data : { *(.data*) }
    .bss ALIGN(16) : { *(.bss*) *(COMMON) }
    .stacks ALIGN(16) : { KEEP(*(.stacks)) }
    .copy_tables ALIGN(4096) : { KEEP(*(.copy_tables)) }
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(_start == 0xA0000000, "entry must match the reviewed physical address")
ASSERT((probe_vectors & 2047) == 0, "vectors must be 2 KiB aligned")
ASSERT(SIZEOF(.bss) == 0, "all writable bytes must be present in the raw image")
ASSERT(SIZEOF(.copy_tables) == 7 * 4096, "temporary copy requires exactly seven table pages")
ASSERT(__image_end <= 0xA001F000, "loader and tables overlap the fixed SD reference page")
ASSERT(__image_end <= 0xA001EF00, "loader and tables overlap U-Boot's boot timing slot (stamps.h)")
