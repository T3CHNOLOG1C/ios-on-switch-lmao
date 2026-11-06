"""The three pretranslated roles the iOS 27 image composer consumes (//install reads PretranslatedInfo)."""

PretranslatedInfo = provider(
    doc = "How to produce one role's translated tree at compose time (the composer has the volume mounted).",
    fields = {
        "role": "os, system_cryptex or app_cryptex",
        "driver": "the translate_tree executable (File)",
        "tool": "the arm64e CLI (File)",
        "apple_trustcaches": "Apple's trust cache IM4Ps, for each translated binary's launch-constraint category",
        "runfiles": "runfiles the driver and tool need",
    },
)

def _pretranslate_role_impl(ctx):
    runfiles = ctx.runfiles(files = ctx.files.apple_trustcaches)
    runfiles = runfiles.merge(ctx.attr.driver[DefaultInfo].default_runfiles)
    runfiles = runfiles.merge(ctx.attr.tool[DefaultInfo].default_runfiles)
    return [
        DefaultInfo(files = depset([ctx.executable.driver, ctx.executable.tool]), runfiles = runfiles),
        PretranslatedInfo(
            role = ctx.attr.role,
            driver = ctx.executable.driver,
            tool = ctx.executable.tool,
            apple_trustcaches = ctx.files.apple_trustcaches,
            runfiles = runfiles,
        ),
    ]

pretranslate_role = rule(
    implementation = _pretranslate_role_impl,
    doc = "The driver, the translator and Apple's trust caches for one role. The composer mounts the decrypted " +
          "image and runs the driver (mounting and the system-volume reseal need root, which no Bazel action has).",
    attrs = {
        "role": attr.string(mandatory = True, values = ["os", "system_cryptex", "app_cryptex"]),
        "driver": attr.label(mandatory = True, executable = True, cfg = "exec"),
        "tool": attr.label(mandatory = True, executable = True, cfg = "exec"),
        "apple_trustcaches": attr.label_list(allow_files = True),
    },
)
