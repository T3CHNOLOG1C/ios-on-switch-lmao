"""ios_arm64e: an ios_application built for arm64e alone, whatever --ios_multi_cpus says (.bazelrc: arm64).

rules_apple takes an app's architectures from --ios_multi_cpus only, so a per-app arm64e build needs this outgoing
transition. Its files are the app's .ipa.
"""

def _arm64e_transition_impl(_settings, _attr):
    return {"//command_line_option:ios_multi_cpus": ["arm64e"]}

_arm64e_transition = transition(
    implementation = _arm64e_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:ios_multi_cpus"],
)

def _ios_arm64e_impl(ctx):
    return [DefaultInfo(files = ctx.attr.app[0][DefaultInfo].files)]

ios_arm64e = rule(
    implementation = _ios_arm64e_impl,
    attrs = {
        "app": attr.label(
            cfg = _arm64e_transition,
            mandatory = True,
            doc = "the ios_application to build for arm64e",
        ),
    },
    doc = "An ios_application's .ipa, built for arm64e alone.",
)
