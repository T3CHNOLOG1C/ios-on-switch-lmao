//! m2v-corpus: every function of one metallib through the metal2vulkan runtime (the bridge's device options) on the host NVK over
//! the mock GM20B user client, so that NAK compiles each pipeline for SM53.
//!
//! usage: m2v-corpus <libvulkan_nouveau.dylib> <cache dir> <metallib> [--from N] [--only a,b] [--tag T]
//!
//! - kernel: a compute state, then one dispatch with every argument the kernel declares bound to a stand-in (the runtime makes a
//!   compute VkPipeline at the dispatch, not at the creation);
//! - fragment: a render state with a vertex function of the same library whose outputs cover its inputs (same name with
//!   vert/frag swapped first), color attachments from its outputs' types, a vertex descriptor from the vertex function's inputs;
//!   the creation makes the predicted VkPipeline;
//! - vertex function not used by any fragment pairing: a render state with rasterization disabled;
//! - each function with constants twice: all zero/false ("z") and all one/true ("1").
//!
//! One TSV row per (function, variant) on stdout: tag, function, stage, variant, result, phase, detail, ms.
//! stderr gets "BEGIN <index> <name>" before each function (the driver resumes after a crash).
use metal2vulkan::bind::SamplerDescriptor;
use metal2vulkan::library::{FunctionConstantValue, FunctionConstantValues};
use metal2vulkan::metal::{DataType, PixelFormat, PrimitiveTopologyClass, TextureType, VertexFormat};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, RenderPipelineDescriptor, StageSource, VertexAttributeDescriptor, VertexBufferLayoutDescriptor,
    VertexDescriptor,
};
use metal2vulkan_air::abi::{AbiInt, EntryPoint, OutputRole, ParamRole};
use metal2vulkan_air::budget::{Budget, Meter};
use metal2vulkan_air::container::{self, FunctionStage};
use metal2vulkan_runtime::{
    Buffer, CommandQueue, Device, DeviceOptions, Function, Library, MessageSink, MetalLevel, PipelineOptions, ResourceOptions,
    SamplerState, Size, Texture, TextureDescriptor,
};
use std::collections::{BTreeMap, HashSet};
use std::io::Write;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::Instant;

static MESSAGES: Mutex<Vec<String>> = Mutex::new(Vec::new());

fn take_messages() -> Vec<String> {
    std::mem::take(&mut *MESSAGES.lock().unwrap())
}

fn one_line(s: &str, max: usize) -> String {
    let s: String = s.chars().map(|c| if c == '\t' || c == '\n' || c == '\r' { ' ' } else { c }).collect();
    s.chars().take(max).collect()
}

fn n(i: &AbiInt) -> u32 {
    match i {
        AbiInt::Const(v) => *v,
        _ => 0,
    }
}

/// `MTLDataType` byte sizes of the scalar and vector types function constants take.
fn constant_bytes(ty: DataType) -> usize {
    match ty.as_raw() {
        53 => 1, 54 => 2, 55 => 3, 56 => 4,
        45 | 49 => 1, 46 | 50 => 2, 47 | 51 => 3, 48 | 52 => 4,
        37 | 41 => 2, 38 | 42 => 4, 39 | 43 => 6, 40 | 44 => 8,
        29 | 33 => 4, 30 | 34 => 8, 31 | 35 => 12, 32 | 36 => 16,
        81 | 85 => 8, 82 | 86 => 16, 83 | 87 => 24, 84 | 88 => 32,
        16 => 2, 17 => 4, 18 => 6, 19 => 8,
        3 => 4, 4 => 8, 5 => 12, 6 => 16,
        121 => 2, 122 => 4, 123 => 6, 124 => 8,
        _ => 0,
    }
}

/// The value of every element of a constant of type `ty` in variant `v` ("z" zero, "1" one).
fn constant_value(ty: DataType, v: &str) -> Option<Vec<u8>> {
    let n = constant_bytes(ty);
    if n == 0 {
        return None;
    }
    let mut bytes = vec![0u8; n];
    if v == "1" {
        let raw = ty.as_raw();
        let (elem, one): (usize, &[u8]) = match raw {
            53..=56 | 45..=52 => (1, &[1]),
            37..=44 => (2, &[1, 0]),
            29..=36 => (4, &[1, 0, 0, 0]),
            81..=88 => (8, &[1, 0, 0, 0, 0, 0, 0, 0]),
            16..=19 => (2, &[0x00, 0x3c]),
            3..=6 => (4, &[0, 0, 0x80, 0x3f]),
            121..=124 => (2, &[0x80, 0x3f]),
            _ => (n, &[]),
        };
        for chunk in bytes.chunks_mut(elem) {
            chunk.copy_from_slice(&one[..elem]);
        }
    }
    Some(bytes)
}

/// A type name's element and count: "float4" -> ("float", 4), "half" -> ("half", 1).
fn element(name: &str) -> (String, u32) {
    let name = name.trim();
    let digits = name.len() - name.trim_end_matches(|c: char| c.is_ascii_digit()).len();
    let (base, count) = name.split_at(name.len() - digits);
    (base.to_string(), count.parse().unwrap_or(1))
}

fn vertex_format(name: &str) -> Option<(u32, u32)> {
    // (MTLVertexFormat, bytes)
    let (base, c) = element(name);
    Some(match (base.as_str(), c) {
        ("float", 1) => (28, 4), ("float", 2) => (29, 8), ("float", 3) => (30, 12), ("float", 4) => (31, 16),
        ("half", 1) => (53, 2), ("half", 2) => (25, 4), ("half", 3) => (26, 6), ("half", 4) => (27, 8),
        ("int", 1) => (32, 4), ("int", 2) => (33, 8), ("int", 3) => (34, 12), ("int", 4) => (35, 16),
        ("uint", 1) => (36, 4), ("uint", 2) => (37, 8), ("uint", 3) => (38, 12), ("uint", 4) => (39, 16),
        ("short", 1) => (50, 2), ("short", 2) => (16, 4), ("short", 3) => (17, 6), ("short", 4) => (18, 8),
        ("ushort", 1) => (49, 2), ("ushort", 2) => (13, 4), ("ushort", 3) => (14, 6), ("ushort", 4) => (15, 8),
        ("char", 1) => (46, 1), ("char", 2) => (4, 2), ("char", 3) => (5, 3), ("char", 4) => (6, 4),
        ("uchar", 1) => (45, 1), ("uchar", 2) => (1, 2), ("uchar", 3) => (2, 3), ("uchar", 4) => (3, 4),
        _ => return None,
    })
}

/// A color attachment format for a fragment output of type `name`.
fn color_format(name: &str) -> u32 {
    let (base, c) = element(name);
    match (base.as_str(), c) {
        ("float" | "half", 1) => 25,                  // R16Float
        ("float" | "half", 2) => 65,                  // RG16Float
        ("float" | "half", _) => 80,                  // BGRA8Unorm
        ("int", 1) => 54, ("int", 2) => 104, ("int", _) => 124,
        ("uint", 1) => 53, ("uint", 2) => 103, ("uint", _) => 123,
        ("short", 1) => 24, ("short", 2) => 64, ("short", _) => 114,
        ("ushort", 1) => 23, ("ushort", 2) => 63, ("ushort", _) => 113,
        ("char", 1) => 14, ("char", 2) => 34, ("char", _) => 74,
        ("uchar", 1) => 13, ("uchar", 2) => 33, ("uchar", _) => 73,
        _ => 80,
    }
}

struct Info {
    name: String,
    stage: FunctionStage,
    entry: Option<EntryPoint>,
    decode_error: Option<String>,
}

fn entries_of(lib: &container::Library<'_>) -> Vec<Info> {
    let budget = Budget::with_deadline(None);
    match lib {
        container::Library::Metallib(m) => m
            .functions
            .iter()
            .map(|f| {
                let mut meter = Meter::new(&budget);
                let (entry, decode_error) = match metal2vulkan_air::decode(f.bitcode, &mut meter) {
                    Ok(d) => (d.abi.entries.into_iter().find(|e| e.name.to_string() == f.name), None),
                    Err(e) => (None, Some(format!("{e:?}"))),
                };
                Info { name: f.name.to_string(), stage: f.stage, entry, decode_error }
            })
            .collect(),
        container::Library::Module(_) => Vec::new(),
    }
}

fn user_inputs(e: &EntryPoint) -> Vec<(Vec<u8>, String)> {
    e.params
        .iter()
        .filter_map(|p| match &p.role {
            ParamRole::FragmentInput(v) => Some((v.key.0.to_vec(), p.ty.name.as_ref().map(|n| n.to_string()).unwrap_or_default())),
            _ => None,
        })
        .collect()
}

fn user_outputs(e: &EntryPoint) -> Vec<(Vec<u8>, String)> {
    e.outputs
        .iter()
        .filter_map(|o| match &o.role {
            OutputRole::VertexOutput(k) => Some((k.0.to_vec(), o.ty.name.as_ref().map(|n| n.to_string()).unwrap_or_default())),
            _ => None,
        })
        .collect()
}

/// How well vertex function `v` feeds fragment `f`: None when an input is missing; else the extra outputs (fewer is better).
fn cover(v: &EntryPoint, f: &EntryPoint) -> Option<usize> {
    let outs = user_outputs(v);
    for (key, ty) in user_inputs(f) {
        let hit = outs.iter().find(|(k, _)| *k == key)?;
        if !ty.is_empty() && !hit.1.is_empty() && element(&ty).1 != element(&hit.1).1 {
            return None;
        }
    }
    Some(outs.len() - user_inputs(f).len().min(outs.len()))
}

fn swapped(name: &str) -> Vec<String> {
    let pairs = [("_frag_", "_vert_"), ("frag", "vert"), ("Frag", "Vert"), ("FRAG", "VERT"), ("fragment", "vertex"),
                 ("Fragment", "Vertex"), ("_fs", "_vs"), ("FS", "VS"), ("_ps", "_vs"), ("PS", "VS"), ("_f_", "_v_"), ("_f", "_v")];
    pairs.iter().filter(|(a, _)| name.contains(a)).map(|(a, b)| name.replacen(a, b, 1)).collect()
}

fn common_prefix(a: &str, b: &str) -> usize {
    a.bytes().zip(b.bytes()).take_while(|(x, y)| x == y).count()
}

struct Harness {
    device: Device,
    queue: CommandQueue,
    buffer: Buffer,
    sampler: SamplerState,
    textures: Mutex<BTreeMap<(u32, u32, bool), Option<Texture>>>,
    draw: bool,
}

impl Harness {
    fn texture(&self, ty: u32, format: u32, ms: bool) -> Option<Texture> {
        let mut cache = self.textures.lock().unwrap();
        cache
            .entry((ty, format, ms))
            .or_insert_with(|| {
                let mut d = TextureDescriptor::texture_2d(PixelFormat::try_from(format).ok()?, 8, 8, false);
                d.texture_type = TextureType::try_from(ty).ok()?;
                match ty {
                    0 | 1 | 9 => d.height = 1,
                    7 => d.depth = 4,
                    _ => {}
                }
                if matches!(ty, 1 | 3 | 6 | 8) {
                    d.array_length = 2;
                }
                if ms {
                    d.sample_count = 4;
                }
                if ty == 9 {
                    d.resource_options = ResourceOptions::default();
                }
                self.device.new_texture(&d).ok()
            })
            .clone()
    }

    /// The texture type and format a texture argument of type name `name` takes.
    fn texture_for(&self, name: &str) -> Option<Texture> {
        let (kind, rest) = name.split_once('<').unwrap_or((name, "float>"));
        let elem = rest.split([',', '>']).next().unwrap_or("float").trim();
        let ty = match kind.trim() {
            "texture1d" => 0, "texture1d_array" => 1, "texture2d" | "depth2d" => 2, "texture2d_array" | "depth2d_array" => 3,
            "texture2d_ms" | "depth2d_ms" => 4, "texturecube" | "depthcube" => 5, "texturecube_array" | "depthcube_array" => 6,
            "texture3d" => 7, "texture2d_ms_array" | "depth2d_ms_array" => 8, "texture_buffer" => 9, _ => 2,
        };
        let format = if kind.starts_with("depth") {
            252
        } else {
            match elem {
                "int" => 54, "uint" => 53, "short" => 114, "ushort" => 113, _ => 115,
            }
        };
        self.texture(ty, format, matches!(ty, 4 | 8))
    }
}

/// Bind a stand-in for every argument of kernel `e` and dispatch it once: the runtime makes the region's VkPipeline (NAK).
fn dispatch(h: &Harness, state: &metal2vulkan_runtime::ComputePipelineState, e: Option<&EntryPoint>) -> Result<(), String> {
    let cb = h.queue.command_buffer().map_err(|e| format!("command buffer: {e:?}"))?;
    let mut enc = cb.compute_command_encoder().map_err(|e| format!("encoder: {e:?}"))?;
    enc.set_compute_pipeline_state(state).map_err(|e| format!("set state: {e:?}"))?;
    if let Some(e) = e {
        for p in &e.params {
            match &p.role {
                ParamRole::Buffer(b) | ParamRole::ArgumentBuffer(b) => {
                    for i in 0..n(&b.location.count).max(1) {
                        let _ = enc.set_buffer(Some(&h.buffer), 0, n(&b.location.index) + i);
                    }
                }
                ParamRole::Threadgroup(b) => {
                    let len = b.buffer_size.unwrap_or(256).clamp(16, 16384).next_multiple_of(16);
                    let _ = enc.set_threadgroup_memory_length(len, n(&b.location.index));
                }
                ParamRole::Texture(t) => {
                    let name = p.ty.name.as_ref().map(|n| n.to_string()).unwrap_or_default();
                    if let Some(tex) = h.texture_for(&name) {
                        for i in 0..n(&t.location.count).max(1) {
                            let _ = enc.set_texture(Some(&tex), n(&t.location.index) + i);
                        }
                    }
                }
                ParamRole::Sampler { location } => {
                    for i in 0..n(&location.count).max(1) {
                        let _ = enc.set_sampler_state(Some(&h.sampler), n(&location.index) + i);
                    }
                }
                _ => {}
            }
        }
    }
    let width = u64::from(state.thread_execution_width().max(1)).min(u64::from(state.max_total_threads_per_threadgroup().max(1)));
    enc.dispatch_threadgroups(Size { width: 1, height: 1, depth: 1 }, Size { width, height: 1, depth: 1 })
        .map_err(|e| format!("dispatch: {e:?}"))?;
    enc.end_encoding().map_err(|e| format!("end: {e:?}"))?;
    cb.commit().map_err(|e| format!("commit: {e:?}"))?;
    cb.wait_until_completed().map_err(|e| format!("wait: {e:?}"))?;
    if let Some(err) = cb.error() {
        return Err(format!("command buffer error: {err:?}"));
    }
    Ok(())
}

fn specialize(lib: &Library, name: &str, variant: &str) -> Result<(Function, usize), String> {
    let f = lib.new_function_with_name(name).map_err(|e| format!("function: {e:?}"))?;
    let consts = f.function_constants_dictionary().map_err(|e| format!("constants: {e:?}"))?;
    if consts.is_empty() {
        return Ok((f, 0));
    }
    let mut values = FunctionConstantValues::defaults();
    for (cname, c) in &consts {
        if let Some(bytes) = constant_value(c.data_type(), variant) {
            values.by_name.insert(cname.clone(), FunctionConstantValue::new(c.data_type(), bytes));
        }
    }
    lib.new_function_with_name_constant_values(name, &values).map(|f| (f, consts.len())).map_err(|e| format!("specialize: {e:?}"))
}

/// The symbol a compile asks to be linked (`NeedsInput(LinkedFunction { symbol })`), if that is the refusal.
fn wanted_link(error: &str) -> Option<String> {
    let at = error.find("LinkedFunction { symbol: \"")? + "LinkedFunction { symbol: \"".len();
    let rest = &error[at..];
    Some(rest[..rest.find('"')?].to_string())
}

/// Add the library's function `symbol` to `linked` (MPS and RealityKit link their own visible functions, which Metal
/// receives through `MTLLinkedFunctions`).
fn link(lib: &Library, linked: &mut metal2vulkan::pipeline::LinkedFunctions, symbol: &str) -> bool {
    match lib.new_function_with_name(symbol) {
        Ok(f) if !linked.functions.iter().any(|l| l.function == f.linked_function().function) => {
            linked.functions.push(f.linked_function());
            true
        }
        _ => false,
    }
}

fn vertex_descriptor(v: &EntryPoint) -> Option<VertexDescriptor> {
    let mut d = VertexDescriptor::new();
    let mut offset = 0u32;
    for p in &v.params {
        if let ParamRole::VertexInput { location } = &p.role {
            let name = p.ty.name.as_ref().map(|n| n.to_string()).unwrap_or_else(|| "float4".into());
            let (format, bytes) = vertex_format(&name).unwrap_or((31, 16));
            let format = VertexFormat::try_from(format).ok()?;
            d.attributes.insert(n(&location.index), VertexAttributeDescriptor::new(format, offset, 30));
            offset += bytes.next_multiple_of(4);
        }
    }
    if d.attributes.is_empty() {
        return None;
    }
    d.layouts.insert(30, VertexBufferLayoutDescriptor::new(Some(offset.max(4))));
    Some(d)
}

fn render(h: &Harness, lib: &Library, v: &Function, ve: Option<&EntryPoint>, f: Option<(&Function, Option<&EntryPoint>)>) -> Result<String, String> {
    let mut vlinked = metal2vulkan::pipeline::LinkedFunctions::default();
    let mut flinked = metal2vulkan::pipeline::LinkedFunctions::default();
    loop {
        match render_once(h, v, ve, f, &vlinked, &flinked) {
            Err(e) => match wanted_link(&e) {
                Some(sym) if link(lib, &mut vlinked, &sym) | link(lib, &mut flinked, &sym) => continue,
                _ => return Err(e),
            },
            Ok((state, d, note)) => {
                if h.draw {
                    draw(h, &state, &d, ve, f.and_then(|(_, fe)| fe)).map_err(|e| format!("draw: {e}"))?;
                    return Ok(format!("{note} drawn"));
                }
                return Ok(note);
            }
        }
    }
}

/// Bind stand-ins for the arguments of `e` on one stage of `enc`.
fn bind_stage(h: &Harness, enc: &mut metal2vulkan_runtime::RenderCommandEncoder, e: Option<&EntryPoint>, fragment: bool) {
    let Some(e) = e else { return };
    for p in &e.params {
        match &p.role {
            ParamRole::Buffer(b) | ParamRole::ArgumentBuffer(b) => {
                for i in 0..n(&b.location.count).max(1) {
                    let index = n(&b.location.index) + i;
                    let _ = if fragment { enc.set_fragment_buffer(Some(&h.buffer), 0, index) } else { enc.set_vertex_buffer(Some(&h.buffer), 0, index) };
                }
            }
            ParamRole::Texture(t) => {
                let name = p.ty.name.as_ref().map(|n| n.to_string()).unwrap_or_default();
                if let Some(tex) = h.texture_for(&name) {
                    for i in 0..n(&t.location.count).max(1) {
                        let index = n(&t.location.index) + i;
                        let _ = if fragment { enc.set_fragment_texture(Some(&tex), index) } else { enc.set_vertex_texture(Some(&tex), index) };
                    }
                }
            }
            ParamRole::Sampler { location } => {
                for i in 0..n(&location.count).max(1) {
                    let index = n(&location.index) + i;
                    let _ = if fragment { enc.set_fragment_sampler_state(Some(&h.sampler), index) } else { enc.set_vertex_sampler_state(Some(&h.sampler), index) };
                }
            }
            _ => {}
        }
    }
}

/// One pass over stand-in attachments of the pipeline's formats: three draws of the state, so a pipeline that fetches its
/// color draws after earlier draws of its pass (the unordered fetch's barrier).
fn draw(h: &Harness, state: &metal2vulkan_runtime::RenderPipelineState, d: &RenderPipelineDescriptor, ve: Option<&EntryPoint>,
        fe: Option<&EntryPoint>) -> Result<(), String> {
    let mut pass = metal2vulkan_runtime::RenderPassDescriptor::new();
    let target = |format: PixelFormat| -> Result<Texture, String> {
        let td = TextureDescriptor::texture_2d(format, 64, 64, false);
        h.device.new_texture(&td).map_err(|e| format!("target {format:?}: {e:?}"))
    };
    for (i, c) in d.color_attachments.iter().enumerate() {
        if let Some(c) = c {
            pass.color_attachments[i].texture = Some(target(c.pixel_format)?);
            pass.color_attachments[i].load_action = metal2vulkan_runtime::LoadAction::Clear;
        }
    }
    if let Some(depth) = d.depth_format {
        let t = target(depth)?;
        pass.depth_attachment.texture = Some(t.clone());
        if d.stencil_format.is_some() {
            pass.stencil_attachment.texture = Some(t);
        }
    }
    if d.color_attachments.iter().all(Option::is_none) && d.depth_format.is_none() {
        pass.render_target_width = 64;
        pass.render_target_height = 64;
        pass.default_raster_sample_count = 1;
    }
    let cb = h.queue.command_buffer().map_err(|e| format!("command buffer: {e:?}"))?;
    let mut enc = cb.render_command_encoder(&pass).map_err(|e| format!("encoder: {e:?}"))?;
    enc.set_render_pipeline_state(state).map_err(|e| format!("set state: {e:?}"))?;
    let _ = enc.set_vertex_buffer(Some(&h.buffer), 0, 30);
    bind_stage(h, &mut enc, ve, false);
    bind_stage(h, &mut enc, fe, true);
    let primitive = match d.input_primitive_topology {
        PrimitiveTopologyClass::Point => metal2vulkan::metal::PrimitiveType::Point,
        _ => metal2vulkan::metal::PrimitiveType::Triangle,
    };
    for _ in 0..3 {
        enc.draw_primitives(primitive, 0, 3, 1, 0).map_err(|e| format!("{e:?}"))?;
    }
    enc.end_encoding().map_err(|e| format!("end: {e:?}"))?;
    cb.commit().map_err(|e| format!("commit: {e:?}"))?;
    cb.wait_until_completed().map_err(|e| format!("wait: {e:?}"))?;
    if let Some(err) = cb.error() {
        return Err(format!("command buffer error: {err:?}"));
    }
    Ok(())
}

fn render_once(h: &Harness, v: &Function, ve: Option<&EntryPoint>, f: Option<(&Function, Option<&EntryPoint>)>,
               vlinked: &metal2vulkan::pipeline::LinkedFunctions, flinked: &metal2vulkan::pipeline::LinkedFunctions)
               -> Result<(metal2vulkan_runtime::RenderPipelineState, RenderPipelineDescriptor, String), String> {
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(v.function_descriptor().with_linked(vlinked.clone())));
    if let Some(ve) = ve {
        d.vertex_descriptor = vertex_descriptor(ve);
    }
    match f {
        Some((f, fe)) => {
            d.fragment = Some(StageSource::Function(f.function_descriptor().with_linked(flinked.clone())));
            if let Some(fe) = fe {
                let mut colors: BTreeMap<u32, String> = BTreeMap::new();
                for o in &fe.outputs {
                    match &o.role {
                        OutputRole::RenderTarget { attachment, index: 0, .. } => {
                            colors.insert(n(attachment), o.ty.name.as_ref().map(|n| n.to_string()).unwrap_or_else(|| "float4".into()));
                        }
                        OutputRole::Depth(_) => d.depth_format = PixelFormat::try_from(260).ok(),
                        OutputRole::Stencil => {
                            d.depth_format = PixelFormat::try_from(260).ok();
                            d.stencil_format = PixelFormat::try_from(260).ok();
                        }
                        _ => {}
                    }
                }
                for p in &fe.params {
                    if let ParamRole::ColorInput { index, .. } = &p.role {
                        colors.entry(n(index)).or_insert_with(|| p.ty.name.as_ref().map(|n| n.to_string()).unwrap_or_else(|| "float4".into()));
                    }
                }
                for (i, ty) in colors {
                    if let (Some(slot), Ok(format)) = (d.color_attachments.get_mut(i as usize), PixelFormat::try_from(color_format(&ty))) {
                        *slot = Some(ColorAttachmentDescriptor::new(format));
                    }
                }
            }
        }
        None => {}
    }
    d.input_primitive_topology = PrimitiveTopologyClass::Triangle;
    match h.device.new_render_pipeline_state(&d, &PipelineOptions::default()) {
        Ok(state) => Ok((state, d, String::new())),
        // A vertex function writing [[point_size]] draws points: Metal refuses it under the triangle class.
        Err(e) if format!("{e:?}").contains("PointSizeTopology") => {
            d.input_primitive_topology = PrimitiveTopologyClass::Point;
            let state = h.device.new_render_pipeline_state(&d, &PipelineOptions::default()).map_err(|e| format!("{e:?}"))?;
            Ok((state, d, "points".to_string()))
        }
        Err(e) => Err(format!("{e:?}")),
    }
}

/// The AIR64 slice of an iOS universal metallib (0xcafebabe), as the bridge's `air_slice` takes it; other input as it is.
fn air_slice(bytes: &[u8]) -> Option<&[u8]> {
    let word = |at: usize| bytes.get(at..at + 4).map(|w| u32::from_be_bytes([w[0], w[1], w[2], w[3]]));
    if word(0)? != 0xcafe_babe {
        return Some(bytes);
    }
    for i in 0..(word(4)? as usize).min(16) {
        let at = 8 + 20 * i;
        if word(at)? == 0x0100_0017 {
            let (offset, size) = (word(at + 8)? as usize, word(at + 12)? as usize);
            return bytes.get(offset..offset.checked_add(size)?);
        }
    }
    None
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 4 {
        eprintln!("usage: m2v-corpus <loader> <cache> <metallib> [--from N] [--only a,b] [--tag T]");
        std::process::exit(2);
    }
    let opt = |k: &str| a.iter().position(|x| x == k).and_then(|i| a.get(i + 1)).cloned();
    let from: usize = opt("--from").and_then(|v| v.parse().ok()).unwrap_or(0);
    let only: Option<HashSet<String>> = opt("--only").map(|v| v.split(',').map(str::to_string).collect());
    let tag = opt("--tag").unwrap_or_else(|| a[3].clone());
    let mut options = DeviceOptions::default();
    options.loader = Some(PathBuf::from(&a[1]));
    options.metal_level = Some(MetalLevel::NEWEST);
    options.shader_cache = metal2vulkan_runtime::ShaderCacheOptions::in_directory(a[2].clone());
    options.policy.framebuffer_fetch = metal2vulkan::device::FramebufferFetchPolicy::AcceptUnordered;
    options.messages = Some(MessageSink::new(|m| {
        if std::env::var_os("M2V_PRINT_MESSAGES").is_some() {
            eprintln!("MSG {}: {}", m.id_name, m.text);
        }
        if m.id_name == "metal2vulkan-pipeline-creation" || m.severity >= metal2vulkan_runtime::MessageSeverity::Warning {
            MESSAGES.lock().unwrap().push(format!("{}: {}", m.id_name, m.text));
        }
    }));
    let device = Device::system_default_with(&options).expect("device");
    let h = Harness {
        queue: device.new_command_queue().expect("queue"),
        buffer: device.new_buffer(1 << 16, ResourceOptions::default()).expect("buffer"),
        sampler: device.new_sampler_state(&SamplerDescriptor::default()).expect("sampler"),
        textures: Mutex::new(BTreeMap::new()),
        draw: a.iter().any(|x| x == "--draw"),
        device,
    };
    let _ = take_messages();
    let file = std::fs::read(&a[3]).expect("read");
    let Some(data) = air_slice(&file) else {
        println!("{tag}\t-\t-\t-\tERR\tcontainer\tno AIR64 slice\t0");
        eprintln!("END");
        return;
    };
    let out = std::io::stdout();
    let row = |name: &str, stage: &str, variant: &str, result: &str, phase: &str, detail: &str, ms: u128| {
        let mut o = out.lock();
        let _ = writeln!(o, "{tag}\t{name}\t{stage}\t{variant}\t{result}\t{phase}\t{}\t{ms}", one_line(detail, 1200));
        let _ = o.flush();
    };
    let parsed = match container::parse(data) {
        Ok(l) => l,
        Err(e) => {
            row("-", "-", "-", "ERR", "container", &format!("{e:?}"), 0);
            eprintln!("END");
            return;
        }
    };
    let infos = entries_of(&parsed);
    let lib = match h.device.new_library_with_data(data) {
        Ok(l) => l,
        Err(e) => {
            row("-", "-", "-", "ERR", "library", &format!("{e:?}"), 0);
            eprintln!("END");
            return;
        }
    };
    let vertices: Vec<usize> = infos.iter().enumerate().filter(|(_, i)| i.stage == FunctionStage::Vertex).map(|(k, _)| k).collect();
    let mut paired: HashSet<usize> = HashSet::new();
    // Fragments first (they pair vertex functions), then kernels, then the vertex functions no fragment took, then the rest.
    let order: Vec<usize> = {
        let rank = |s: FunctionStage| match s {
            FunctionStage::Fragment => 0,
            FunctionStage::Kernel => 1,
            FunctionStage::Vertex => 2,
            _ => 3,
        };
        let mut o: Vec<usize> = (0..infos.len()).collect();
        o.sort_by_key(|&k| (rank(infos[k].stage), k));
        o
    };
    for (pos, &k) in order.iter().enumerate() {
        let info = &infos[k];
        if pos < from || only.as_ref().is_some_and(|o| !o.contains(&info.name)) {
            if info.stage == FunctionStage::Fragment && pos < from {
                // keep the pairing record of fragments done before a resume: re-pair cheaply below is not needed
            }
            continue;
        }
        eprintln!("BEGIN {pos} {}", info.name);
        let stage = format!("{:?}", info.stage);
        let entry_point = matches!(info.stage, FunctionStage::Kernel | FunctionStage::Vertex | FunctionStage::Fragment);
        if let Some(err) = &info.decode_error {
            row(&info.name, &stage, "-", if entry_point { "ERR" } else { "SKIP" }, "decode", err, 0);
            continue;
        }
        let has_constants = lib.new_function_with_name(&info.name).ok().and_then(|f| f.function_constants_dictionary().ok()).is_some_and(|c| !c.is_empty());
        let variants: &[&str] = if has_constants { &["z", "1"] } else { &["z"] };
        for &variant in variants {
            let began = Instant::now();
            let _ = take_messages();
            let result: Result<String, (String, String)> = match info.stage {
                FunctionStage::Kernel => specialize(&lib, &info.name, variant).map_err(|e| ("function".into(), e)).and_then(|(f, _)| {
                    let mut linked = metal2vulkan::pipeline::LinkedFunctions::default();
                    let made = loop {
                        let d = metal2vulkan::pipeline::ComputePipelineDescriptor::new(StageSource::Function(f.function_descriptor().with_linked(linked.clone())));
                        match h.device.new_compute_pipeline_state(&d, &PipelineOptions::default()) {
                            Err(e) if wanted_link(&format!("{e:?}")).is_some_and(|sym| link(&lib, &mut linked, &sym)) => continue,
                            other => break other,
                        }
                    };
                    let state = match made {
                        Ok(s) => s,
                        Err(e) if format!("{e:?}").contains("TileAttachments") => {
                            let mut d = metal2vulkan::pipeline::TileRenderPipelineDescriptor::new(StageSource::Function(f.function_descriptor()));
                            d.color_attachment_formats[0] = PixelFormat::try_from(115).ok();
                            d.threadgroup_size_matches_tile_size = true;
                            h.device.new_tile_render_pipeline_state(&d, &PipelineOptions::default())
                                .map_err(|e| ("create".to_string(), format!("tile: {e:?}")))?;
                            return Ok("tile pipeline (not dispatched)".to_string());
                        }
                        Err(e) => return Err(("create".to_string(), format!("{e:?}"))),
                    };
                    dispatch(&h, &state, info.entry.as_ref()).map_err(|e| ("dispatch".to_string(), e))?;
                    Ok(String::new())
                }),
                FunctionStage::Fragment => (|| {
                    let (f, _) = specialize(&lib, &info.name, variant).map_err(|e| ("function".to_string(), e))?;
                    let fe = info.entry.as_ref();
                    // Candidates: swapped names, then covering vertex functions by fewest extra outputs and longest shared prefix.
                    let mut cands: Vec<usize> = Vec::new();
                    for s in swapped(&info.name) {
                        if let Some(&v) = vertices.iter().find(|&&v| infos[v].name == s) {
                            cands.push(v);
                        }
                    }
                    if let Some(fe) = fe {
                        let mut covering: Vec<(usize, usize, usize)> = vertices
                            .iter()
                            .filter_map(|&v| infos[v].entry.as_ref().and_then(|ve| cover(ve, fe)).map(|extra| (extra, usize::MAX - common_prefix(&infos[v].name, &info.name), v)))
                            .collect();
                        covering.sort();
                        let extra: Vec<usize> = covering.into_iter().map(|(_, _, v)| v).filter(|v| !cands.contains(v)).take(3).collect();
                        cands.extend(extra);
                    }
                    if cands.is_empty() {
                        return Err(("pair".to_string(), format!("no vertex function of the library covers its {} inputs", fe.map_or(0, |e| user_inputs(e).len()))));
                    }
                    let mut last = (String::new(), String::new());
                    for v in cands.into_iter().take(4) {
                        let (vf, _) = match specialize(&lib, &infos[v].name, variant) {
                            Ok(x) => x,
                            Err(e) => {
                                last = ("vertex".to_string(), format!("{}: {e}", infos[v].name));
                                continue;
                            }
                        };
                        match render(&h, &lib, &vf, infos[v].entry.as_ref(), Some((&f, fe))) {
                            Ok(_) => {
                                paired.insert(v);
                                return Ok(format!("with {}", infos[v].name));
                            }
                            Err(e) => last = ("create".to_string(), format!("with {}: {e}", infos[v].name)),
                        }
                    }
                    Err(last)
                })(),
                FunctionStage::Vertex => {
                    if paired.contains(&k) && variant == "z" && !has_constants {
                        Ok("paired".to_string())
                    } else {
                        specialize(&lib, &info.name, variant).map_err(|e| ("function".into(), e)).and_then(|(vf, _)| {
                            render(&h, &lib, &vf, info.entry.as_ref(), None).map(|_| "no fragment".to_string()).map_err(|e| ("create".to_string(), e))
                        })
                    }
                }
                other => Err(("skip".to_string(), format!("not an entry point ({other:?})"))),
            };
            let ms = began.elapsed().as_millis();
            let msgs = take_messages();
            let failed: Vec<&String> = msgs.iter().filter(|m| m.contains(" failed in ") || m.contains("VK_ERROR")).collect();
            match result {
                Ok(note) if failed.is_empty() => {
                    let made = msgs.iter().filter(|m| m.contains(" made in ")).count();
                    row(&info.name, &stage, variant, "OK", "", &format!("{note} vkpipelines={made}"), ms)
                }
                Ok(_) => row(&info.name, &stage, variant, "ERR", "vkpipeline", failed[0], ms),
                Err((phase, why)) if phase == "skip" => row(&info.name, &stage, variant, "SKIP", &phase, &why, ms),
                Err((phase, why)) => row(&info.name, &stage, variant, "ERR", &phase, &why, ms),
            }
        }
    }
    eprintln!("END");
}
