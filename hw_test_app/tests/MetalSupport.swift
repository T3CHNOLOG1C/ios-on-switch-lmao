import Foundation
import HWTestCore
import Metal

/// What every Metal test needs: the device, the app's precompiled library, pipelines, command buffers awaited with
/// their GPU timestamps, and readback of private (tiled) textures through the blit engine, the way apps do it.
final class GPU: @unchecked Sendable {
    let device: MTLDevice
    let queue: MTLCommandQueue
    let library: MTLLibrary

    static func make() throws -> GPU {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw TestFailure("MTLCreateSystemDefaultDevice returned nil")
        }
        guard let queue = device.makeCommandQueue() else { throw TestFailure("makeCommandQueue returned nil") }
        let library = try device.makeDefaultLibrary(bundle: .main)
        return GPU(device: device, queue: queue, library: library)
    }

    init(device: MTLDevice, queue: MTLCommandQueue, library: MTLLibrary) {
        self.device = device
        self.queue = queue
        self.library = library
    }

    struct Timing {
        let gpuMilliseconds: Double
        let wallMilliseconds: Double

        /// GPU timestamps that advance prove the work ran on the GPU's clock, not a CPU fallback.
        var gpuTimed: Bool { gpuMilliseconds > 0 }
    }

    func compute(_ name: String) throws -> MTLComputePipelineState {
        guard let function = library.makeFunction(name: name) else { throw TestFailure("no function \(name)") }
        return try device.makeComputePipelineState(function: function)
    }

    func render(
        vertex: String = "quad_vertex", fragment: String, format: MTLPixelFormat, samples: Int = 1,
        depth: MTLPixelFormat = .invalid, stencil: MTLPixelFormat = .invalid
    ) throws -> MTLRenderPipelineState {
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: vertex)
        descriptor.fragmentFunction = library.makeFunction(name: fragment)
        descriptor.colorAttachments[0].pixelFormat = format
        descriptor.rasterSampleCount = samples
        descriptor.depthAttachmentPixelFormat = depth
        descriptor.stencilAttachmentPixelFormat = stencil
        return try device.makeRenderPipelineState(descriptor: descriptor)
    }

    /// Encodes, commits and waits for one command buffer; throws its error if the GPU reports one.
    @discardableResult
    func run(_ encode: (MTLCommandBuffer) throws -> Void) async throws -> Timing {
        guard let buffer = queue.makeCommandBuffer() else { throw TestFailure("makeCommandBuffer returned nil") }
        try encode(buffer)
        let start = CFAbsoluteTimeGetCurrent()
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
            buffer.addCompletedHandler { _ in done.resume() }
            buffer.commit()
        }
        let wall = (CFAbsoluteTimeGetCurrent() - start) * 1000
        if buffer.status == .error {
            throw TestFailure("command buffer failed: \(buffer.error.map { String(describing: $0) } ?? "no error")")
        }
        return Timing(gpuMilliseconds: max(0, buffer.gpuEndTime - buffer.gpuStartTime) * 1000, wallMilliseconds: wall)
    }

    func buffer(_ length: Int) throws -> MTLBuffer {
        guard let buffer = device.makeBuffer(length: max(16, length), options: .storageModeShared) else {
            throw TestFailure("makeBuffer(\(length)) returned nil")
        }
        return buffer
    }

    func buffer<T>(_ values: [T]) throws -> MTLBuffer {
        let buffer = try buffer(values.count * MemoryLayout<T>.stride)
        values.withUnsafeBytes { buffer.contents().copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
        return buffer
    }

    func texture(
        _ format: MTLPixelFormat, _ width: Int, _ height: Int, usage: MTLTextureUsage = [.renderTarget, .shaderRead],
        samples: Int = 1, storage: MTLStorageMode = .private, mipmaps: Int = 1
    ) throws -> MTLTexture {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: width, height: height, mipmapped: false)
        descriptor.usage = usage
        descriptor.storageMode = storage
        descriptor.sampleCount = samples
        descriptor.mipmapLevelCount = mipmaps
        if samples > 1 { descriptor.textureType = .type2DMultisample }
        guard let texture = device.makeTexture(descriptor: descriptor) else {
            throw TestFailure("makeTexture(\(format.rawValue) \(width)x\(height)) returned nil")
        }
        return texture
    }

    /// Copies a texture (any storage) into a shared buffer with the blit engine and returns its bytes, rows packed.
    func read(
        _ texture: MTLTexture, bytesPerPixel: Int, level: Int = 0, slice: Int = 0,
        options: MTLBlitOption = []
    ) async throws -> [UInt8] {
        let width = max(1, texture.width >> level)
        let height = max(1, texture.height >> level)
        let rowBytes = width * bytesPerPixel
        let out = try buffer(rowBytes * height)
        try await run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: texture, sourceSlice: slice, sourceLevel: level, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: width, height: height, depth: 1), to: out, destinationOffset: 0,
                destinationBytesPerRow: rowBytes, destinationBytesPerImage: 0, options: options)
            blit.endEncoding()
        }
        return [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: rowBytes * height))
    }

    func upload(_ bytes: [UInt8], to texture: MTLTexture, bytesPerRow: Int, level: Int = 0, slice: Int = 0) async throws
    {
        let staging = try buffer(bytes)
        let width = max(1, texture.width >> level)
        let height = max(1, texture.height >> level)
        try await run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: staging, sourceOffset: 0, sourceBytesPerRow: bytesPerRow, sourceBytesPerImage: 0,
                sourceSize: MTLSize(width: width, height: height, depth: 1), to: texture, destinationSlice: slice,
                destinationLevel: level, destinationOrigin: MTLOrigin())
            blit.endEncoding()
        }
    }

    /// Corners of an axis-aligned quad in clip space for quad_vertex's triangle strip.
    static func quad(x0: Float, y0: Float, x1: Float, y1: Float, z: Float) -> [SIMD4<Float>] {
        [SIMD4(x0, y0, z, 1), SIMD4(x1, y0, z, 1), SIMD4(x0, y1, z, 1), SIMD4(x1, y1, z, 1)]
    }

    static let fullScreen = quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.5)

    static func draw(_ encoder: MTLRenderCommandEncoder, corners: [SIMD4<Float>], color: SIMD4<Float>) {
        var corners = corners
        var color = color
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
        encoder.setFragmentBytes(&color, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    var proof: String { "\(device.name) registry=\(String(device.registryID, radix: 16))" }
}

extension Array where Element == UInt8 {
    func pixel(_ x: Int, _ y: Int, width: Int, bytesPerPixel: Int = 4) -> ArraySlice<UInt8> {
        let at = (y * width + x) * bytesPerPixel
        return self[at..<(at + bytesPerPixel)]
    }

    func loaded<T>(as type: T.Type, at index: Int) -> T {
        withUnsafeBytes { $0.loadUnaligned(fromByteOffset: index * MemoryLayout<T>.stride, as: T.self) }
    }
}
