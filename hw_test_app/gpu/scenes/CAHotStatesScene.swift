import Foundation
import HWTestCore
import Metal

/// The GPU time of Core Animation's costliest states (CAHotCorpus) at the console's resolution: each state draws
/// `draws` full-screen rectangles into a 1280x720 target of its first colour's format, its other colours memoryless,
/// sampling a BGRA8 picture with zero uniforms (as the raw pack's probe binds them). us_<label>= is the median of
/// `runs` command buffers' GPU time per draw in microseconds: the before and after of a compiler change, on the
/// device (the label lowercased). The states this QuartzCore lacks (the other iOS version's) are left out.
struct CAHotStatesScene: GPUScene {
    let id = "gpu.ca.hot_states"
    let name = "Core Animation's costliest states: GPU time per full-screen draw"

    static let width = 1280
    static let height = 720
    static let draws = 8
    static let runs = 5

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            result.notes.append("no QuartzCore library here (\(GPU.describe(error)))")
            return result
        }
        let source = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.shaderRead])
        try await gpu.blitUpload(
            [UploadRect(x: 0, y: 0, width: Self.width, height: Self.height)],
            [Patterns.bytes(Patterns.busy(width: Self.width, height: Self.height, channels: 4), .bgra8Unorm)],
            into: source, texel: 4)
        var timed = 0
        for line in CAHotCorpus.states {
            let pipeline: MTLRenderPipelineState
            do {
                pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
            } catch {
                continue
            }
            let attachments = try line.colors.map { color -> MTLTexture in
                let format = MTLPixelFormat(rawValue: color.format) ?? .bgra8Unorm
                return try color.index == 0
                    ? gpu.texture(format, Self.width, Self.height)
                    : gpu.texture(format, Self.width, Self.height, usage: [.renderTarget], storage: .memoryless)
            }
            var times: [Double] = []
            var paths: Set<String> = []
            for _ in 0..<Self.runs {
                let timing = try await gpu.run(tolerateError: true) { commands in
                    let pass = MTLRenderPassDescriptor()
                    for (color, texture) in zip(line.colors, attachments) {
                        pass.colorAttachments[color.index].texture = texture
                        pass.colorAttachments[color.index].loadAction = .clear
                        pass.colorAttachments[color.index].storeAction = color.index == 0 ? .store : .dontCare
                    }
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(pipeline)
                    CAState.bind(
                        encoder, vertices: CAState.rectangle(x0: -1, y0: -1, x1: 1, y1: 1, color: SIMD4(1, 1, 1, 1)),
                        source: source)
                    for _ in 0..<Self.draws {
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    }
                    encoder.endEncoding()
                }
                times.append(timing.gpuMilliseconds * 1000 / Double(Self.draws))
                paths.insert(timing.path)
            }
            timed += 1
            result.expect(
                !paths.contains { $0.hasPrefix("error:") },
                "\(line.shortLabel): \(paths.sorted().joined(separator: ","))")
            result.metrics.set("us_" + line.shortLabel.lowercased(), Stats.percentile(times, 0.5), digits: 1)
        }
        if timed == 0 { result.notes.append("no hot state made from this QuartzCore") }
        return result
    }
}
