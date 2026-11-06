"""The original classes drivers subclass or call, per IPSW kind: generated headers and bindings (kits/SwitchABI)."""

load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")
load("//bazel/platforms:transitions.bzl", "kernel_transition")

ABI_PACKAGE = "//kits/SwitchABI"

# What a driver passes to its compiles to find Original<Class>.h.
ABI_INCLUDE = "$(BINDIR)/kits/SwitchABI/include"

def _header_impl(ctx):
    info = SwitchAbiInfo(interface = ctx.file.interface, table = ctx.file.table)
    if not ctx.attr.header:
        return [DefaultInfo(), CcInfo(), info]
    out = ctx.actions.declare_file("include/" + (ctx.attr.header_name or "Original{}.h".format(ctx.attr.cls)))
    ctx.actions.run(
        executable = ctx.executable._gen,
        arguments = ["header", "--interface", ctx.file.interface.path, "--table", ctx.file.table.path, "--out", out.path],
        inputs = [ctx.file.interface, ctx.file.table],
        outputs = [out],
        mnemonic = "SwitchAbiHeader",
        progress_message = "Generating Original{}.h for the selected IPSW kind".format(ctx.attr.cls),
    )
    return [
        DefaultInfo(files = depset([out])),
        CcInfo(compilation_context = cc_common.create_compilation_context(headers = depset([out]))),
        info,
    ]

SwitchAbiInfo = provider(doc = "An original class's ABI tables for the selected kind.", fields = ["interface", "table"])

switch_abi_header = rule(
    implementation = _header_impl,
    doc = "Original<cls>.h for the selected IPSW kind, as a header-only CcInfo (drivers add ABI_INCLUDE to their -I).",
    attrs = {
        "cls": attr.string(mandatory = True),
        "header": attr.bool(default = True, doc = "False: drivers declare the class from another header (its bindings only)"),
        "header_name": attr.string(doc = "the header's file name, if not Original<cls>.h"),
        "interface": attr.label(mandatory = True, allow_single_file = [".json"]),
        "table": attr.label(mandatory = True, allow_single_file = [".json"]),
        "_gen": attr.label(default = ABI_PACKAGE + ":gen", executable = True, cfg = "exec"),
    },
)

def switch_abi_class(cls, header = True, header_name = None):
    """:<cls>, the class's header for the selected kind; a kind without kinds/<kind>/<cls>.json fails at analysis."""
    tables = native.glob(["kinds/*/{}.json".format(cls)], allow_empty = True)
    switch_abi_header(
        name = cls,
        cls = cls,
        header = header,
        header_name = header_name,
        interface = "interface/{}.json".format(cls),
        table = select(
            {"//ipsw:" + t.split("/")[1]: t for t in tables},
            no_match_error = "kits/SwitchABI has no {} table for this IPSW kind (kinds/<kind>/{}.json)".format(cls, cls),
        ),
        visibility = ["//visibility:public"],
    )

def _bindings_impl(ctx):
    objects = []
    for dep in ctx.attr.objects:
        for linker_input in dep[CcInfo].linking_context.linker_inputs.to_list():
            if linker_input.owner == dep.label:
                for library in linker_input.libraries:
                    objects += library.objects
    args = ["bindings", "--out", ctx.outputs.out.path]
    inputs = list(objects)
    for cls in ctx.attr.classes:
        info = cls[SwitchAbiInfo]
        args += ["--pair", info.interface.path, info.table.path]
        inputs += [info.interface, info.table]
    for o in objects:
        args += ["--object", o.path]
    ctx.actions.run(
        executable = ctx.executable._gen,
        arguments = args,
        inputs = inputs,
        outputs = [ctx.outputs.out],
        mnemonic = "SwitchAbiBindings",
        progress_message = "Binding %{label}'s original imports for the selected IPSW kind",
    )

switch_abi_bindings = rule(
    implementation = _bindings_impl,
    doc = "out (.S): every original symbol the objects import from classes, at the selected kind's addresses.",
    attrs = {
        "out": attr.output(mandatory = True),
        "classes": attr.label_list(mandatory = True, providers = [SwitchAbiInfo]),
        "objects": attr.label_list(mandatory = True, providers = [CcInfo], cfg = kernel_transition),
        "_gen": attr.label(default = ABI_PACKAGE + ":gen", executable = True, cfg = "exec"),
        "_allowlist_function_transition": attr.label(default = "@bazel_tools//tools/allowlists/function_transition_allowlist"),
    },
)
