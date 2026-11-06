// SPDX-License-Identifier: MIT
// Debug builds: once per process, the frame rate UIKit and Core Animation believe the display runs at, and the first display link
// intervals a UIKit process gets. UIKit asked for a 48-120 Hz range on this 60 Hz panel (the iPad7,1 identity is a ProMotion
// iPad); these lines tell whether CA paces for 120 Hz.
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <os/log.h>
#include <atomic>
#include "DisplayRateProbe.h"

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG

static os_log_t probeLog()
{
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "display-rate");
    return log;
}

static double valueOf(id object, NSString *key)
{
    if (!object)
        return -1;
    @try {
        id value = [object valueForKey:key];
        return [value respondsToSelector:@selector(doubleValue)] ? [value doubleValue] : -1;
    } @catch (NSException *) {
        return -1;
    }
}

static NSString *textOf(id object, NSString *key)
{
    if (!object)
        return @"-1";
    @try {
        return [[object valueForKey:key] description] ?: @"-1";
    } @catch (NSException *) {
        return @"-1";
    }
}

@interface SwitchDisplayRateProbe : NSObject
@end

@implementation SwitchDisplayRateProbe {
    unsigned _ticks;
}

- (void)tick:(id)link
{
    os_log(probeLog(), "SwitchDisplayRate: %{public}s display link %u: duration %.6f target-timestamp %.6f",
           getprogname(), _ticks, valueOf(link, @"duration"),
           valueOf(link, @"targetTimestamp") - valueOf(link, @"timestamp"));
    if (++_ticks == 6)
        ((void (*)(id, SEL))objc_msgSend)(link, @selector(invalidate));
}

@end

static void logRates()
{
    Class screenClass = NSClassFromString(@"UIScreen");
    id screen = [screenClass respondsToSelector:@selector(mainScreen)] ? [screenClass valueForKey:@"mainScreen"] : nil;
    Class displayClass = NSClassFromString(@"CADisplay");
    id display = [displayClass respondsToSelector:NSSelectorFromString(@"mainDisplay")]
                     ? [displayClass valueForKey:@"mainDisplay"]
                     : nil;
    os_log(probeLog(),
           "SwitchDisplayRate: %{public}s UIScreen maximumFramesPerSecond %.0f; CADisplay refreshRate %.3f "
           "minimumFrameDuration %.6f maximumFramesPerSecond %.0f preferredFrameRate %.3f supportsExtendedColors %.0f "
           "colorGamut %{public}@ (-1: none)",
           getprogname(), valueOf(screen, @"maximumFramesPerSecond"), valueOf(display, @"refreshRate"),
           valueOf(display, @"minimumFrameDuration"), valueOf(display, @"maximumFramesPerSecond"),
           valueOf(display, @"preferredFrameRate"), valueOf(display, @"supportsExtendedColors"),
           textOf(display, @"colorGamut"));
    Class application = NSClassFromString(@"UIApplication"), linkClass = NSClassFromString(@"CADisplayLink");
    if (screen && linkClass && [application respondsToSelector:@selector(sharedApplication)] &&
        [application valueForKey:@"sharedApplication"]) {
        id link = ((id (*)(id, SEL, id, SEL))objc_msgSend)(linkClass, @selector(displayLinkWithTarget:selector:),
                                                           [SwitchDisplayRateProbe new], @selector(tick:));
        ((void (*)(id, SEL, NSRunLoop *, NSRunLoopMode))objc_msgSend)(link, @selector(addToRunLoop:forMode:),
                                                                      [NSRunLoop mainRunLoop], NSRunLoopCommonModes);
    }
}

void switch_display_rate_probe(void)
{
    static std::atomic<bool> done{false};
    if (done.exchange(true))
        return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        logRates();
    });
}

#else

void switch_display_rate_probe(void)
{}

#endif
