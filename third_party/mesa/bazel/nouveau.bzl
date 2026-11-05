"""src/nouveau/headers/meson.build's per-class generators, as it loops over nv_classes: one genrule per output.

  nv_push_<cl>.h/.c              class_parser.py (the C push-method helpers), --prev-in-h the latest earlier class
                                 of the same kind (the last two hex digits)
  nv_push_class_dump.h           nv_push_class_dump_h.py over every class
  nvh_classes_<cl>.rs, _mthd.rs  class_parser.py for NVK's Rust (nvidia_headers)
  nvh_classes_<cl>_qmd.rs        struct_parser.py, for a compute class (…c0) with a QMD header
  nvh_classes_<cl>_tex.rs        struct_parser.py, for a 3D class (…97)
  nvh_classes_<cl>_sph.rs        struct_parser.py, for a class with a shader program header
  lib.rs                         lib_rs_gen.py over all of the Rust modules, in meson's order, then `extra_rs`

The Rust ones run rustfmt 1.96.1 (util.write_template_rs; without it they are left unformatted, and the line numbers
reach the binary through panic locations).
"""

load(":defs.bzl", "mesa_genrule")

_RUSTFMT = "@nvk_rust//:bin/rustfmt"
_RUSTFMT_FILES = "//third_party/mesa/bazel:rustfmt"
_RUSTFMT_PATH = 'PATH="$$PWD/$$(dirname $(execpath {})):$$PATH" '.format(_RUSTFMT)

def _prev(classes, cl):
    prev = None
    for p in classes:
        if p == cl:
            break
        if p[-2:] == cl[-2:]:
            prev = p
    return prev

def _gen(name, srcs, outs, tool, args, rust = False):
    mesa_genrule(
        name = name,
        srcs = srcs + ["util.py"],
        outs = outs,
        cmd = (_RUSTFMT_PATH if rust else "") + "$(execpath :{}) {}".format(tool, args),
        tools = [":" + tool] + ([_RUSTFMT_FILES, _RUSTFMT] if rust else []),
    )

def _header(cl):
    return "nvidia/classes/{}.h".format(cl)

def nvidia_class_generators(name, classes, qmd_headers, sph_headers, extra_rs):
    """Every per-class generator of meson.build, plus the filegroups the rest of the package takes.

    Args:
        name: the filegroup of every generated C header (nv_class_headers).
        classes: meson.build's nv_classes, in its order.
        qmd_headers: the nvidia/classes/<cl>qmd.h files there are (meson asks fs.is_file).
        sph_headers: the nvidia/classes/<cl>sph.h files there are.
        extra_rs: the other generated modules of nvidia_headers (the hwref bindgen outputs), after the classes'.

    Defines:
        `name` (filegroup): the generated C headers; nvidia_headers_rs_srcs (filegroup): every file of the
        nvidia_headers crate.
    """
    headers = []
    for cl in classes:
        prev = _prev(classes, cl)
        prev_srcs = [_header(prev)] if prev else []
        prev_args = "--prev-in-h $(execpath {}) ".format(_header(prev)) if prev else ""
        _gen(
            name = "nv_push_{}_h".format(cl),
            srcs = [_header(cl)] + prev_srcs,
            outs = ["nv_push_{}.h".format(cl), "nv_push_{}.c".format(cl)],
            tool = "class_parser",
            args = "--in-h $(execpath {}) {}--out-h $(execpath nv_push_{}.h) --out-c $(execpath nv_push_{}.c)".format(
                _header(cl),
                prev_args,
                cl,
                cl,
            ),
        )
        headers.append("nv_push_{}.h".format(cl))

    # meson passes the loop's last class header as this one's input (its loop variable outlives the loop).
    _gen(
        name = "nv_push_class_dump_h_gen",
        srcs = [_header(classes[-1])],
        outs = ["nv_push_class_dump.h"],
        tool = "nv_push_class_dump_h",
        args = "--classes '{} ' --out-h $(execpath nv_push_class_dump.h)".format(" ".join(classes)),
    )
    headers.append("nv_push_class_dump.h")
    native.filegroup(name = name, srcs = headers)

    rs = []
    for cl in classes:
        out = "nvh_classes_{}.rs".format(cl)
        _gen(
            name = "nvh_classes_{}_rs".format(cl),
            srcs = [_header(cl)],
            outs = [out],
            tool = "class_parser",
            args = "--in-h $(execpath {}) --out-rs $(execpath {})".format(_header(cl), out),
            rust = True,
        )
        rs.append(out)
        prev = _prev(classes, cl)
        mthd = "nvh_classes_{}_mthd.rs".format(cl)
        _gen(
            name = "nvh_classes_{}_mthd_rs".format(cl),
            srcs = [_header(cl)] + ([_header(prev)] if prev else []),
            outs = [mthd],
            tool = "class_parser",
            args = "--in-h $(execpath {}) {}--out-rs-mthd $(execpath {})".format(
                _header(cl),
                "--prev-in-h $(execpath {}) ".format(_header(prev)) if prev else "",
                mthd,
            ),
            rust = True,
        )
        rs.append(mthd)
        for kind, present in (
            ("qmd", cl.endswith("c0") and "nvidia/classes/{}qmd.h".format(cl) in qmd_headers),
            ("tex", cl.endswith("97")),
            ("sph", "nvidia/classes/{}sph.h".format(cl) in sph_headers),
        ):
            if not present:
                continue
            src = "nvidia/classes/{}{}.h".format(cl, kind)
            out = "nvh_classes_{}_{}.rs".format(cl, kind)
            _gen(
                name = "nvh_classes_{}_{}_rs".format(cl, kind),
                srcs = [src],
                outs = [out],
                tool = "struct_parser",
                args = "--in-h $(execpath {}) --out-rs $(execpath {})".format(src, out),
                rust = True,
            )
            rs.append(out)
    rs = rs + extra_rs
    _gen(
        name = "lib_rs",
        srcs = rs,
        outs = ["lib.rs"],
        tool = "lib_rs_gen",
        args = "--out-rs $(execpath lib.rs) " + " ".join(["$(execpath {})".format(f) for f in rs]),
        rust = True,
    )
    native.filegroup(name = "nvidia_headers_rs_srcs", srcs = ["lib.rs"] + rs)
