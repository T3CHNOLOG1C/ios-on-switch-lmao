"""Our code in XNU: which kernel/switch sources each XNU component compiles, and where each one links.

Both kernels compile every entry, with the recipe of the component it is listed under (that component's XNU flags,
includes and, in release, LTO) plus its own copts, and link it right after the Apple object it names. Every XNU object
also gets -I kernel/switch/include (the hook headers) and the selected XNU's version macro (XNU_10063_141_1,
XNU_12377_121_6), so code that differs between versions is `#if XNU_<version>` here, never a second copy.
"""

SWITCH_ROOT = "kernel/switch"

# component: [(the Apple object it links after, the source below kernel/switch, its own copts)]
SWITCH_OBJECTS = {
    "iokit": [
        ("iokit/Kernel/IOUserClient.cpp", "iokit/iosurface.cpp", []),
    ],
    "osfmk": [
        ("osfmk/arm64/arm_vm_init.c", "ram/ram.c", []),
    ],
}

def switch_objects(component, objects):
    """A component's objects (its table, in link order) with its kernel/switch sources as "switch:" entries.

    Args:
        component: osfmk, bsd, ...
        objects: the component's objects, in link order.

    Returns:
        The objects, each kernel/switch source right after the Apple object its entry names.
    """
    out = list(objects)
    for after, src, _ in SWITCH_OBJECTS.get(component, []):
        if after not in out:
            fail("kernel/switch/objects.bzl: {} links after {}, which {} does not build".format(src, after, component))
        at = out.index(after) + 1
        for _ in range(len(out)):
            if at < len(out) and out[at].startswith("switch:"):
                at += 1
        out.insert(at, "switch:" + src)
    return out

def switch_copts(component, copts):
    """A component's per-object copts with its kernel/switch sources' own."""
    out = dict(copts)
    for _, src, flags in SWITCH_OBJECTS.get(component, []):
        out["switch:" + src] = flags
    return out
