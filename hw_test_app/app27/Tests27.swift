import HWTestKit

/// What only iOS 27 has: QuartzCore 27's glass states on the raw path, Metal 4, Liquid Glass through UIKit and the
/// render server, and VideoToolbox's frame processors. The frame processors run last: they may reach hardware J181
/// has and the Switch lacks.
enum Tests27 {
    static let all: [any HWTest] = [
        GPUSceneTest(scene: CAGlassScene()), Metal4Test(), UIGlassTest(), PocketStripTest(), GlassPacing.over,
        GlassPacing.under, FrameProcessorTest(),
    ]
}
