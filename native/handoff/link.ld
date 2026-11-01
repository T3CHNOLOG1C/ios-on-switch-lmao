ENTRY(_start)
SECTIONS {
    . = 0xA0000000;
    __image_start = .;
    .text : { KEEP(*(.text.entry)) *(.text*) }
    .vectors ALIGN(2048) : { KEEP(*(.vectors)) }
    .rodata ALIGN(16) : { *(.rodata*) }
    .data ALIGN(16) : { *(.data*) }
    .bss ALIGN(16) : { *(.bss*) *(COMMON) }
    .stacks ALIGN(16) : { KEEP(*(.stacks)) }
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(_start == 0xA0000000, "entry must match the reviewed physical address")
ASSERT((probe_vectors & 2047) == 0, "vectors must be 2 KiB aligned")
ASSERT(SIZEOF(.bss) == 0, "all writable bytes must be present in the raw image")
ASSERT(__image_end - __image_start <= 0x20000, "probe exceeds 128 KiB window")
