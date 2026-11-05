"""XNU's pexpert objects for each XNU config (DEVELOPMENT, RELEASE), in link order (make's pexpert.filelist), and what
each one adds to its component's recipe: copts (conf/Makefile.*'s <object>_CFLAGS_ADD and _CWARNFLAGS_ADD),
includes (<object>_INCFLAGS, after the recipe's) and segments (the setsegname passes make runs on the object).
"gen:" sources are generated (MIG, iig, makesyscalls, newvers) in the component's object directory."""

DEVELOPMENT = struct(
    objects = [
        "pexpert/gen/device_tree.c",
        "pexpert/gen/bootargs.c",
        "pexpert/gen/pe_gen.c",
        "pexpert/gen/kcformat.c",
        "pexpert/arm/pe_bootargs.c",
        "pexpert/arm/pe_consistent_debug.c",
        "pexpert/arm/pe_fiq.c",
        "pexpert/arm/pe_identify_machine.c",
        "pexpert/arm/pe_init.c",
        "pexpert/arm/pe_kprintf.c",
        "pexpert/arm/pe_serial.c",
        "pexpert/arm/hwtrace/hwtrace.c",
        "pexpert/arm/hwtrace/hwtrace_multbuf.c",
        "pexpert/arm/hwtrace/panic_trace.c",
    ],
    copts = {
        "pexpert/arm/pe_identify_machine.c": ["-Wno-cast-align", "-Wno-shorten-64-to-32", "-Wno-sign-conversion"],
        "pexpert/arm/pe_init.c": ["-D", "XNU_DT_PANIC_REGION=1", "-Wno-shorten-64-to-32", "-Wno-sign-conversion"],
        "pexpert/arm/pe_serial.c": ["-Wno-implicit-int-conversion", "-Wno-sign-conversion"],
        "pexpert/gen/bootargs.c": ["-Wno-sign-conversion"],
        "pexpert/gen/device_tree.c": ["-Wno-sign-conversion"],
    },
    includes = {},
    segments = {},
)

RELEASE = struct(
    objects = [
        "pexpert/gen/device_tree.c",
        "pexpert/gen/bootargs.c",
        "pexpert/gen/pe_gen.c",
        "pexpert/gen/kcformat.c",
        "pexpert/arm/pe_bootargs.c",
        "pexpert/arm/pe_consistent_debug.c",
        "pexpert/arm/pe_fiq.c",
        "pexpert/arm/pe_identify_machine.c",
        "pexpert/arm/pe_init.c",
        "pexpert/arm/pe_kprintf.c",
        "pexpert/arm/pe_serial.c",
        "pexpert/arm/hwtrace/hwtrace.c",
        "pexpert/arm/hwtrace/hwtrace_multbuf.c",
        "pexpert/arm/hwtrace/panic_trace.c",
    ],
    copts = {
        "pexpert/arm/pe_identify_machine.c": ["-Wno-cast-align", "-Wno-shorten-64-to-32", "-Wno-sign-conversion"],
        "pexpert/arm/pe_init.c": ["-D", "XNU_DT_PANIC_REGION=1", "-Wno-shorten-64-to-32", "-Wno-sign-conversion"],
        "pexpert/arm/pe_serial.c": ["-Wno-implicit-int-conversion", "-Wno-sign-conversion"],
        "pexpert/gen/bootargs.c": ["-Wno-sign-conversion"],
        "pexpert/gen/device_tree.c": ["-Wno-sign-conversion"],
    },
    includes = {},
    segments = {},
)
