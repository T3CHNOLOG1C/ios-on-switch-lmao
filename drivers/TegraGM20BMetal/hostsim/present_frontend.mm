// Host check of the frontend's presentation contract (NativeBridgeMetalFrontend.mm's NBCommandBuffer) over the bridge over NVK
// over the mock GM20B, against Metal's own -[_MTLCommandBuffer presentDrawable:] (21H461): the drawable is presented from a
// scheduled handler, once, after the command buffer is scheduled, and not at all when the command buffer failed; the atTime: and
// afterMinimumDuration: forms forward their argument; a handler added after its point has passed still runs. A recording
// MTLDrawable stands for CAMetalDrawable (ClockKitUI's CLKUIMetalQuadView ends each frame with presentDrawable: + commit).
// usage: present_frontend <libvulkan_nouveau.dylib> <cache dir>; prints one ok/FAIL line per check, exits non-zero on a FAIL.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <atomic>
#include <memory>
#include <objc/message.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

@interface RecordingDrawable : NSObject <MTLDrawable>
@property (atomic) int presents;
@property (atomic) double lastTime;  // presentAtTime: / presentAfterMinimumDuration: argument, -1 for present
@property (atomic) MTLCommandBufferStatus statusAtPresent;
@property (atomic, weak) id<MTLCommandBuffer> watched;
@end
@implementation RecordingDrawable
- (void)record:(double)t
{
    self.statusAtPresent = self.watched.status;
    self.lastTime = t;
    self.presents = self.presents + 1;
}
- (void)present
{
    [self record:-1];
}
- (void)presentAtTime:(CFTimeInterval)t
{
    [self record:t];
}
- (void)presentAfterMinimumDuration:(CFTimeInterval)d
{
    [self record:d];
}
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)block
{
    (void)block;
}
- (CFTimeInterval)presentedTime
{
    return 0;
}
- (NSUInteger)drawableID
{
    return 1;
}
@end

static int failures;
static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}
static bool within(double seconds, bool (^done)(void))
{
    for (int i = 0; i < (int)(seconds * 1000) && !done(); ++i) usleep(1000);
    return done();
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3) return 2;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:64
                                                                                     height:64
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> target = [dev newTextureWithDescriptor:td];
        // One frame as CLKUIMetalQuadView encodes it: a pass into the drawable's texture, presentDrawable:, commit.
        id<MTLCommandBuffer> (^frame)(void) = ^{
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
            [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
            return cb;
        };

        check([[queue commandBuffer] respondsToSelector:@selector(presentDrawable:)],
              "command buffer answers presentDrawable:");

        {
            RecordingDrawable *d = [RecordingDrawable new];
            id<MTLCommandBuffer> cb = frame();
            d.watched = cb;
            [cb presentDrawable:d];
            check(d.presents == 0, "presentDrawable: does not present before commit");
            [cb commit];
            [cb waitUntilCompleted];
            check(within(2, ^{ return d.presents == 1; }), "presentDrawable: presents once");
            check(d.lastTime == -1, "presentDrawable: calls -present");
            check(d.statusAtPresent >= MTLCommandBufferStatusScheduled &&
                      d.statusAtPresent != MTLCommandBufferStatusError,
                  "presented once scheduled");
        }
        {
            RecordingDrawable *d = [RecordingDrawable new];
            id<MTLCommandBuffer> cb = frame();
            [cb presentDrawable:d atTime:12.5];
            [cb commit];
            [cb waitUntilCompleted];
            check(within(2, ^{ return d.presents == 1; }) && d.lastTime == 12.5,
                  "presentDrawable:atTime: forwards the time");
        }
        {
            RecordingDrawable *d = [RecordingDrawable new];
            id<MTLCommandBuffer> cb = frame();
            [cb presentDrawable:d afterMinimumDuration:0.25];
            [cb commit];
            [cb waitUntilCompleted];
            check(within(2, ^{ return d.presents == 1; }) && d.lastTime == 0.25,
                  "presentDrawable:afterMinimumDuration: forwards the duration");
        }
        {
            // A command buffer the frontend refuses (protection options) completes with an error and presents nothing,
            // as Metal's scheduled handler skips -present when the command buffer has an error.
            RecordingDrawable *d = [RecordingDrawable new];
            id<MTLCommandBuffer> cb = frame();
            ((void (*)(id, SEL, uint64_t))objc_msgSend)(cb, sel_registerName("setProtectionOptions:"), 1);
            [cb presentDrawable:d];
            auto completed = std::make_shared<std::atomic<bool>>(false);
            [cb addCompletedHandler:^(id<MTLCommandBuffer>) { *completed = true; }];
            [cb commit];
            [cb waitUntilCompleted];
            check(within(2, ^{ return completed->load(); }) && cb.status == MTLCommandBufferStatusError,
                  "a refused command buffer completes with an error");
            usleep(100000);
            check(d.presents == 0, "a refused command buffer presents nothing");
        }
        {
            id<MTLCommandBuffer> cb = frame();
            [cb commit];
            [cb waitUntilCompleted];
            auto scheduledRan = std::make_shared<std::atomic<bool>>(false),
                 completedRan = std::make_shared<std::atomic<bool>>(false);
            [cb addScheduledHandler:^(id<MTLCommandBuffer>) { *scheduledRan = true; }];
            [cb addCompletedHandler:^(id<MTLCommandBuffer>) { *completedRan = true; }];
            check(within(2, ^{ return scheduledRan->load(); }), "a scheduled handler added after scheduling runs");
            check(within(2, ^{ return completedRan->load(); }), "a completed handler added after completion runs");
        }
        {
            // Frames in a row, each presenting its own drawable: every drawable presented exactly once.
            NSMutableArray<RecordingDrawable *> *drawables = [NSMutableArray array];
            id<MTLCommandBuffer> last = nil;
            for (int i = 0; i < 32; ++i) {
                RecordingDrawable *d = [RecordingDrawable new];
                [drawables addObject:d];
                last = frame();
                [last presentDrawable:d];
                [last commit];
            }
            [last waitUntilCompleted];
            bool once = within(2, ^{
                for (RecordingDrawable *d in drawables)
                    if (d.presents != 1) return false;
                return true;
            });
            check(once, "32 frames: each drawable presented once");
        }
        printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    }
    return failures ? 1 : 0;
}
