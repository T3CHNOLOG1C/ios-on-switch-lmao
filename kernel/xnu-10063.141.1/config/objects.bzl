"""The kernel's own objects (makedefs/MakeInc.kernel: lastkerneldataconst.o lastkernelconstructor.o nonlto.o version.o, in the link's order) for each XNU config (DEVELOPMENT, RELEASE), in link order (make's link.filelist), and what
each one adds to its component's recipe: copts (conf/Makefile.*'s <object>_CFLAGS_ADD and _CWARNFLAGS_ADD),
includes (<object>_INCFLAGS, after the recipe's) and segments (the setsegname passes make runs on the object).
"gen:" sources are generated (MIG, iig, makesyscalls, newvers) in the component's object directory."""

DEVELOPMENT = struct(
    objects = [
        "libsa/lastkerneldataconst.c",
        "libsa/lastkernelconstructor.c",
        "libsa/nonlto.c",
        "gen:version.c",
    ],
    copts = {},
    includes = {},
    segments = {
        "libsa/lastkernelconstructor.c": ["-s __DATA -n __LASTDATA_CONST"],
    },
)

RELEASE = struct(
    objects = [
        "libsa/lastkerneldataconst.c",
        "libsa/lastkernelconstructor.c",
        "libsa/nonlto.c",
        "gen:version.c",
    ],
    copts = {},
    includes = {
        "libsa/lastkernelconstructor.c": ["-fno-lto"],
        "libsa/nonlto.c": ["-fno-lto"],
    },
    segments = {
        "libsa/lastkernelconstructor.c": ["-s __DATA -n __LASTDATA_CONST"],
    },
)
