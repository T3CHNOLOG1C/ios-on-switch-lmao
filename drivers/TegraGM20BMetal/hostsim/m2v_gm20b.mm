// Host run of the stage-4 stack: the Metal frontend (NativeBridgeMetalFrontend.mm) over the metal2vulkan bridge over NVK
// (host build, nvkmd_darwin) over mock_iokit.c's GM20B. Opens the device, then the frontend's own benchmark: the first
// vertex/fragment pairs of a QuartzCore metallib made into BGRA8 render states and drawn once each.
// usage: m2v_gm20b <libvulkan_nouveau.dylib> <cache dir> [<default.metallib> <pairs>]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
extern "C" void mock_iokit_report(void);
static void missing(const char *cls, const char *sel) { fprintf(stderr, "MISSING -[%s %s]\n", cls, sel); }
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 3) return 2;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id device = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!device) { fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error()); mock_iokit_report(); return 1; }
        printf("device: %s\n", [[(id<MTLDevice>)device name] UTF8String]);
        char status[512] = {};
        native_bridge_metal_status(status, sizeof status);
        printf("status: %s\n", status);
        if (argc >= 5) {
            static char out[8192];
            native_bridge_metal_benchmark(device, argv[3], (unsigned)atoi(argv[4]), out, sizeof out);
            printf("benchmark:\n%s\n", out);
            native_bridge_metal_status(status, sizeof status);
            printf("status: %s\n", status);
            char formats[256] = {};
            native_bridge_metal_formats(formats, sizeof formats);
            printf("formats: %s\n", formats);
        }
        mock_iokit_report();
    }
    return 0;
}
