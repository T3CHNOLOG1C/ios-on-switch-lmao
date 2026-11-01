// SPDX-License-Identifier: MIT
// GM20B falcon firmware embedded in the kernel image (files under firmware/, see firmware/SOURCE).
// Assemble with -I <this directory>/firmware so .incbin finds them. The table maps the names nvkm
// passes to request_firmware() to the blobs (shim/xnu_glue.cpp nvxnu_firmware_find).
    .section __DATA_CONST,__const
    .p2align 3
Lfw0_name: .asciz "nvidia/gm20b/acr/bl.bin"
Lfw1_name: .asciz "nvidia/gm20b/acr/ucode_load.bin"
Lfw2_name: .asciz "nvidia/gm20b/gr/fecs_bl.bin"
Lfw3_name: .asciz "nvidia/gm20b/gr/fecs_data.bin"
Lfw4_name: .asciz "nvidia/gm20b/gr/fecs_inst.bin"
Lfw5_name: .asciz "nvidia/gm20b/gr/fecs_sig.bin"
Lfw6_name: .asciz "nvidia/gm20b/gr/gpccs_data.bin"
Lfw7_name: .asciz "nvidia/gm20b/gr/gpccs_inst.bin"
Lfw8_name: .asciz "nvidia/gm20b/gr/sw_bundle_init.bin"
Lfw9_name: .asciz "nvidia/gm20b/gr/sw_ctx.bin"
Lfw10_name: .asciz "nvidia/gm20b/gr/sw_method_init.bin"
Lfw11_name: .asciz "nvidia/gm20b/gr/sw_nonctx.bin"
Lfw12_name: .asciz "nvidia/gm20b/pmu/desc.bin"
Lfw13_name: .asciz "nvidia/gm20b/pmu/image.bin"
Lfw14_name: .asciz "nvidia/gm20b/pmu/sig.bin"
    .p2align 8
Lfw0_start: .incbin "nvidia/gm20b/acr/bl.bin"
Lfw0_end:
    .p2align 8
Lfw1_start: .incbin "nvidia/gm20b/acr/ucode_load.bin"
Lfw1_end:
    .p2align 8
Lfw2_start: .incbin "nvidia/gm20b/gr/fecs_bl.bin"
Lfw2_end:
    .p2align 8
Lfw3_start: .incbin "nvidia/gm20b/gr/fecs_data.bin"
Lfw3_end:
    .p2align 8
Lfw4_start: .incbin "nvidia/gm20b/gr/fecs_inst.bin"
Lfw4_end:
    .p2align 8
Lfw5_start: .incbin "nvidia/gm20b/gr/fecs_sig.bin"
Lfw5_end:
    .p2align 8
Lfw6_start: .incbin "nvidia/gm20b/gr/gpccs_data.bin"
Lfw6_end:
    .p2align 8
Lfw7_start: .incbin "nvidia/gm20b/gr/gpccs_inst.bin"
Lfw7_end:
    .p2align 8
Lfw8_start: .incbin "nvidia/gm20b/gr/sw_bundle_init.bin"
Lfw8_end:
    .p2align 8
Lfw9_start: .incbin "nvidia/gm20b/gr/sw_ctx.bin"
Lfw9_end:
    .p2align 8
Lfw10_start: .incbin "nvidia/gm20b/gr/sw_method_init.bin"
Lfw10_end:
    .p2align 8
Lfw11_start: .incbin "nvidia/gm20b/gr/sw_nonctx.bin"
Lfw11_end:
    .p2align 8
Lfw12_start: .incbin "nvidia/gm20b/pmu/desc.bin"
Lfw12_end:
    .p2align 8
Lfw13_start: .incbin "nvidia/gm20b/pmu/image.bin"
Lfw13_end:
    .p2align 8
Lfw14_start: .incbin "nvidia/gm20b/pmu/sig.bin"
Lfw14_end:
    .p2align 3
    .globl _tegra_gm20b_firmware
_tegra_gm20b_firmware:
    .quad Lfw0_name, Lfw0_start, Lfw0_end
    .quad Lfw1_name, Lfw1_start, Lfw1_end
    .quad Lfw2_name, Lfw2_start, Lfw2_end
    .quad Lfw3_name, Lfw3_start, Lfw3_end
    .quad Lfw4_name, Lfw4_start, Lfw4_end
    .quad Lfw5_name, Lfw5_start, Lfw5_end
    .quad Lfw6_name, Lfw6_start, Lfw6_end
    .quad Lfw7_name, Lfw7_start, Lfw7_end
    .quad Lfw8_name, Lfw8_start, Lfw8_end
    .quad Lfw9_name, Lfw9_start, Lfw9_end
    .quad Lfw10_name, Lfw10_start, Lfw10_end
    .quad Lfw11_name, Lfw11_start, Lfw11_end
    .quad Lfw12_name, Lfw12_start, Lfw12_end
    .quad Lfw13_name, Lfw13_start, Lfw13_end
    .quad Lfw14_name, Lfw14_start, Lfw14_end
    .quad 0, 0, 0
