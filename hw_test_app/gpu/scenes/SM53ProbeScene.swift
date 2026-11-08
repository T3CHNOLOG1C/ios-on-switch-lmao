import Foundation
import HWTestCore
import Metal

/// The device's answers NAK needs before it emits SM53's half ALU with an f32 result, or leaves metal2vulkan's unorm8
/// rounding to the store. The app runs with NAK_DEBUG=sm53_probe (debug builds; kit/sm53_probe_env.c), under which NAK
/// lowers each sm53_probe form to HADD2/HFMA2 .F32 (nak/opt_sm53_half_probe.rs). <form>=pass|fail and <form>_first=
/// (its first wrong input) say which reading holds: half_* if the ops round to half before widening, float_* if they
/// round the exact result once to f32. unorm8= compares the device's float-to-unorm8 store of every half with Metal's
/// rule (NaN 0, clamped to [0, 1], the exact product with 255 rounded to nearest).
struct SM53ProbeScene: GPUScene {
    let id = "gpu.sm53_probe"
    let name = "SM53 half ALU f32 results and the unorm8 store, against the CPU"

    static let forms = ["half_sum", "half_product", "float_sum", "float_product", "float_fma"]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        result.metrics.set(
            "probe_on", ProcessInfo.processInfo.environment["NAK_DEBUG"]?.contains("sm53_probe") ?? false)
        try await alu(gpu, &result)
        try await unorm8(gpu, &result)
        return result
    }

    private func alu(_ gpu: GPU, _ result: inout SceneResult) async throws {
        let (a, b) = HalfArithmeticScene.pairs()
        let c = a.indices.map { a[($0 &* 7919) % a.count] }
        let out = try gpu.buffer(a.count * 4 * Self.forms.count)
        let pipeline = try gpu.compute("sm53_probe")
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setBuffer(try gpu.buffer(a), offset: 0, index: 0)
            encoder.setBuffer(try gpu.buffer(b), offset: 0, index: 1)
            encoder.setBuffer(try gpu.buffer(c), offset: 0, index: 2)
            encoder.setBuffer(out, offset: 0, index: 3)
            encoder.dispatchThreads(
                MTLSize(width: a.count, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(
                    width: min(256, pipeline.maxTotalThreadsPerThreadgroup), height: 1, depth: 1))
            encoder.endEncoding()
        }
        let got = UnsafeBufferPointer(
            start: out.contents().bindMemory(to: UInt32.self, capacity: a.count * Self.forms.count),
            count: a.count * Self.forms.count)
        var first = [String?](repeating: nil, count: Self.forms.count)
        var wrong = [Int](repeating: 0, count: Self.forms.count)
        for i in a.indices {
            let x = Float16(bitPattern: a[i]), y = Float16(bitPattern: b[i]), z = Float16(bitPattern: c[i])
            let want: [Float] = [
                Float(x + y), Float(x * y), Float(x) + Float(y), Float(x) * Float(y),
                Float(z).addingProduct(Float(x), Float(y)),
            ]
            for form in Self.forms.indices {
                let have = got[Self.forms.count * i + form], expected = want[form].bitPattern
                if have == expected || (Float(bitPattern: have).isNaN && want[form].isNaN) { continue }
                wrong[form] += 1
                if first[form] == nil {
                    first[form] = "\(hex(a[i])) \(hex(b[i])) \(hex(c[i])): \(hex(have)), want \(hex(expected))"
                }
            }
        }
        result.metrics.set("inputs", a.count)
        for (form, name) in Self.forms.enumerated() {
            result.metrics.set(name, wrong[form] == 0 ? "pass" : "fail")
            result.metrics.set(name + "_wrong", wrong[form])
            if let input = first[form] { result.metrics.set(name + "_first", input) }
        }
    }

    private func unorm8(_ gpu: GPU, _ result: inout SceneResult) async throws {
        let (width, height) = (256, 128)
        let target = try gpu.texture(.bgra8Unorm, width, height, usage: [.renderTarget, .shaderRead])
        let pipeline = try gpu.render(fragment: "sm53_probe_unorm", format: .bgra8Unorm) { descriptor in
            let attachment = descriptor.colorAttachments[0]!
            attachment.isBlendingEnabled = true
            attachment.rgbBlendOperation = .max
            attachment.alphaBlendOperation = .max
        }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        var wrong = 0
        var first: String?
        for i in 0..<(width * height) {
            let bits: UInt16 = i == 32767 ? 0x7E00 : i < 16384 ? UInt16(i) : 0x8000 | UInt16(i - 16384)
            let want = Self.metalUnorm8(Float16(bitPattern: bits))
            let have = bytes[4 * i]
            guard have != want else { continue }
            wrong += 1
            if first == nil { first = "\(hex(bits)): \(have), want \(want)" }
        }
        result.metrics.set("unorm8", wrong == 0 ? "pass" : "fail")
        result.metrics.set("unorm8_wrong", wrong)
        if let first { result.metrics.set("unorm8_first", first) }
    }

    /// Metal's store of a float into an 8-bit unorm channel (metal2vulkan's ChannelWrite::Unorm, probed on Apple's
    /// GPUs): NaN is 0, the value clamped to [0, 1], the exact product with 255 rounded to nearest, its one tie (0.5)
    /// to 128. A half's product with 255 is exact in Double.
    static func metalUnorm8(_ h: Float16) -> UInt8 {
        if h.isNaN { return 0 }
        let v = min(max(Double(h), 0), 1) * 255
        return UInt8(v.rounded(.toNearestOrAwayFromZero))
    }

    private func hex<T: BinaryInteger>(_ bits: T) -> String { String(bits, radix: 16) }
}
