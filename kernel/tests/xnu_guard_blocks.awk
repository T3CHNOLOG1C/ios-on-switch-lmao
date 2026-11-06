# The native-4K guard's blocks of one XNU source, each from the guard line through its #endif, as the body of
# `static void <name>_<n>(void)`. At most `max` blocks (all when it is unset); fails when there are none.
/^#if !defined\(APPLE_ARM64_ARCH_FAMILY\) && !__ARM_16K_PG__ && !SWITCH_VM_16K_PAGES$/ && !on && (max == "" || n < max) {
    on = 1
    depth = 0
    printf "static void %s_%d(void)\n{\n", name, n++
}
on {
    print
    if ($0 ~ /^#[ \t]*if/)
        depth++
    else if ($0 ~ /^#[ \t]*endif/ && --depth == 0) {
        on = 0
        print "}"
    }
}
END {
    if (!n || on)
        exit 1
}
