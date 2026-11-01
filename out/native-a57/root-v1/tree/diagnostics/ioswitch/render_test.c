/* Host-only bounds/geometry test; never touches the actual framebuffer. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "IOSwitchDiagDisplay.h"

int main(void)
{
    const unsigned count = IOSWITCH_FB_BYTES / 4;
    uint32_t *storage = malloc((count + 2) * sizeof(*storage));
    assert(storage);
    storage[0] = 0x12345678; storage[count + 1] = 0x9abcdef0;
    uint32_t *pixels = storage + 1;
    assert(ioswitch_diag_video_matches(0xf5a00000, 1, 2880, 720, 1280, 32));
    assert(!ioswitch_diag_video_matches(0xf5a00004, 1, 2880, 720, 1280, 32));
    assert(!ioswitch_diag_video_matches(0xf5a00000, 0, 2880, 720, 1280, 32));
    assert(!ioswitch_diag_video_matches(0xf5a00000, 1, 2884, 720, 1280, 32));
    assert(!ioswitch_diag_video_matches(0xf5a00000, 1, 2880, 721, 1280, 32));
    assert(!ioswitch_diag_video_matches(0xf5a00000, 1, 2880, 720, 1279, 32));
    assert(!ioswitch_diag_video_matches(0xf5a00000, 1, 2880, 720, 1280, 32 | 0x300));
    ioswitch_diag_clear(pixels);
    for (unsigned i = 0; i < count; ++i) assert(pixels[i] == IOSWITCH_BACKGROUND);
    ioswitch_diag_pixel(pixels, 0, 0, 1); assert(pixels[1279 * 720] == 1);
    ioswitch_diag_pixel(pixels, 1279, 0, 2); assert(pixels[0] == 2);
    ioswitch_diag_pixel(pixels, 0, 719, 3); assert(pixels[count - 1] == 3);
    ioswitch_diag_pixel(pixels, 1279, 719, 4); assert(pixels[719] == 4);
    ioswitch_diag_pixel(pixels, 1280, 0, 5); ioswitch_diag_pixel(pixels, 0, 720, 6);
    assert(pixels[0] == 2 && pixels[count - 1] == 3);
    ioswitch_diag_clear(pixels);
    assert(!ioswitch_diag_text(pixels, 16, 24, 704, "XNU VM INITIALIZED", 104, IOSWITCH_GREEN));
    unsigned changed = 0;
    for (unsigned i = 0; i < count; ++i) if (pixels[i] != IOSWITCH_BACKGROUND) ++changed;
    assert(changed > 100 && changed < 3000);
    char long_text[2048]; memset(long_text, 'W', sizeof(long_text));
    assert(ioswitch_diag_text(pixels, 16, 324, 628, long_text, sizeof(long_text), IOSWITCH_WHITE));
    for (unsigned y = 628; y < 720; ++y)
        for (unsigned x = 0; x < 1280; ++x)
            assert(pixels[(1279 - x) * 720 + y] == IOSWITCH_BACKGROUND);
    assert(storage[0] == 0x12345678 && storage[count + 1] == 0x9abcdef0);
    free(storage);
    puts("IOSwitch renderer whitelist, rotation, bounds, wrapping and canaries passed");
    return 0;
}
