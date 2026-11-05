OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS {
    . = 0xa7000000;
    __image_start = .;
    .text : { KEEP(*(.text.entry)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .private ALIGN(16) : { *(.private) }
    .bss : { *(.bss*) *(COMMON) }
    __image_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
    ASSERT(__image_end - __image_start <= 0x10000, "exporter exceeds 64KiB")
    ASSERT(SIZEOF(.bss) == 0, "exporter must have no uninitialized allocation")
}
