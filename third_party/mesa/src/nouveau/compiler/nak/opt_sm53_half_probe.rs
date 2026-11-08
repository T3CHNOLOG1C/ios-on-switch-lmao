// Copyright © 2026 Bruno St John
// SPDX-License-Identifier: MIT

//! ios-on-switch-lmao: SM53's half ALU with an f32 result (OFMT F32), in the
//! two readings the device has to tell apart, for SwitchHWTest's
//! gpu.compute.sm53_probe (NAK_DEBUG=sm53_probe, never in release builds):
//!
//! - the f32 widening of a half sum, product or fma is the op with an f32
//!   result, if the op rounds to half before it widens;
//! - an f32 sum, product or fma of widened halves is the half op with an f32
//!   result, if the op rounds the exact result once to f32.
//!
//! At most one of them holds. Its kernels match the CPU and the other's do
//! not; the one that holds becomes a rule of its own.

use crate::ir::*;

use rustc_hash::FxHashMap;

#[derive(Clone, Copy, PartialEq)]
enum Half {
    Add,
    Mul,
    Fma,
}

enum Def {
    /// F2F.F32.F16 of a half, unmodified: the half (its lane in the swizzle)
    Widened(Src),
    /// A half op with a half result, no flushes
    Half {
        kind: Half,
        srcs: Vec<Src>,
        saturate: bool,
    },
}

fn ssa(src: &Src) -> Option<SSAValue> {
    let ssa = src.src_ref.as_ssa()?;
    if ssa.comps() == 1 { Some(ssa[0]) } else { None }
}

fn widening(op: &OpF2F) -> bool {
    op.src_type == FloatType::F16
        && op.dst_type == FloatType::F32
        && !op.integer_rnd
        && op.src.src_mod.is_none()
}

/// The half a widened source of an f32 op reads, with the op's modifiers
fn half_of(defs: &FxHashMap<SSAValue, Def>, src: &Src) -> Option<Src> {
    let Some(Def::Widened(h)) = defs.get(&ssa(src)?) else {
        return None;
    };
    let swizzle = if matches!(h.src_swizzle, SrcSwizzle::Yy) {
        SrcSwizzle::Yy
    } else {
        SrcSwizzle::Xx
    };
    Some(Src {
        src_ref: h.src_ref.clone(),
        src_mod: src.src_mod,
        src_swizzle: swizzle,
    })
}

/// The half op `kind` of `srcs` with an f32 result: a product as an fma
/// adding -0, which leaves every product as it is
fn with_f32_result(dst: Dst, kind: Half, srcs: &[Src], saturate: bool) -> Option<Op> {
    if kind != Half::Add && srcs.iter().any(|s| s.src_mod.has_fabs()) {
        return None;
    }
    Some(match (kind, srcs) {
        (Half::Add, [a, b]) => OpHAdd2 {
            dst,
            srcs: [a.clone(), b.clone()],
            saturate,
            ftz: false,
            f32: true,
        }
        .into(),
        (Half::Mul, [a, b]) => OpHFma2 {
            dst,
            srcs: [a.clone(), b.clone(), Src::ZERO.fneg()],
            saturate,
            ftz: false,
            dnz: false,
            f32: true,
        }
        .into(),
        (Half::Fma, [a, b, c]) => OpHFma2 {
            dst,
            srcs: [a.clone(), b.clone(), c.clone()],
            saturate,
            ftz: false,
            dnz: false,
            f32: true,
        }
        .into(),
        _ => return None,
    })
}

fn halves(defs: &FxHashMap<SSAValue, Def>, srcs: &[Src]) -> Option<Vec<Src>> {
    srcs.iter().map(|s| half_of(defs, s)).collect()
}

fn rewrite(defs: &FxHashMap<SSAValue, Def>, op: &Op) -> Option<Op> {
    match op {
        Op::F2F(f)
            if widening(f)
                && matches!(f.src.src_swizzle, SrcSwizzle::None | SrcSwizzle::Xx) =>
        {
            let Def::Half { kind, srcs, saturate } = defs.get(&ssa(&f.src)?)? else {
                return None;
            };
            with_f32_result(f.dst.clone(), *kind, srcs, *saturate)
        }
        Op::FAdd(o) if o.rnd_mode == FRndMode::NearestEven => {
            with_f32_result(o.dst.clone(), Half::Add, &halves(defs, &o.srcs)?, o.saturate)
        }
        Op::FMul(o) if o.rnd_mode == FRndMode::NearestEven && !o.dnz => {
            with_f32_result(o.dst.clone(), Half::Mul, &halves(defs, &o.srcs)?, o.saturate)
        }
        Op::FFma(o) if o.rnd_mode == FRndMode::NearestEven && !o.dnz => {
            with_f32_result(o.dst.clone(), Half::Fma, &halves(defs, &o.srcs)?, o.saturate)
        }
        _ => None,
    }
}

fn record(defs: &mut FxHashMap<SSAValue, Def>, op: &Op) {
    let (dst, def) = match op {
        Op::F2F(f) if widening(f) && f.src.src_ref.as_ssa().is_some() => {
            (&f.dst, Def::Widened(f.src.clone()))
        }
        Op::HAdd2(h) if !h.f32 && !h.ftz => (
            &h.dst,
            Def::Half { kind: Half::Add, srcs: h.srcs.to_vec(), saturate: h.saturate },
        ),
        Op::HMul2(h) if !h.ftz && !h.dnz => (
            &h.dst,
            Def::Half { kind: Half::Mul, srcs: h.srcs.to_vec(), saturate: h.saturate },
        ),
        Op::HFma2(h) if !h.f32 && !h.ftz && !h.dnz => (
            &h.dst,
            Def::Half { kind: Half::Fma, srcs: h.srcs.to_vec(), saturate: h.saturate },
        ),
        _ => return,
    };
    if let Some(ssa) = dst.as_ssa() {
        if ssa.comps() == 1 {
            defs.insert(ssa[0], def);
        }
    }
}

impl Shader<'_> {
    pub fn opt_sm53_half_probe(&mut self) {
        for f in &mut self.functions {
            let mut defs = FxHashMap::default();
            for b in &mut f.blocks {
                for instr in &mut b.instrs {
                    if let Some(op) = rewrite(&defs, &instr.op) {
                        instr.op = op;
                    }
                    record(&mut defs, &instr.op);
                }
            }
        }
    }
}
