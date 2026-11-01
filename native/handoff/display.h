#ifndef IOSWITCH_HANDOFF_DISPLAY_H
#define IOSWITCH_HANDOFF_DISPLAY_H
#include <stdint.h>

// Live OLED firmware description, photographed in IMG_5509:
// 720 x 1280, a8b8g8r8, rot=3, pitch=2880, span=0x384000.
// No controller/MMIO access: only aligned stores inside the pixel buffer.
#define DISPLAY_BASE UINT64_C(0xf5a00000)
#define DISPLAY_BYTES UINT64_C(0x384000)
#define DISPLAY_WHITE UINT32_C(0xffffffff)
#define DISPLAY_GREEN UINT32_C(0xff70ff70)
#define DISPLAY_RED UINT32_C(0xff7070ff)
#define DISPLAY_BACKGROUND UINT32_C(0xff000000)

static inline void display_pixel(unsigned x, unsigned y, uint32_t color) {
    if (x >= 1280 || y >= 720) return;
    uint64_t offset = (uint64_t)(1279 - x) * 2880 + (uint64_t)y * 4;
    *(volatile uint32_t *)(uintptr_t)(DISPLAY_BASE + offset) = color;
}

static void display_clear(void) {
    volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)DISPLAY_BASE;
    for (uint32_t i = 0; i < DISPLAY_BYTES / 4; ++i) pixels[i] = DISPLAY_BACKGROUND;
    __asm__ volatile("dsb sy" ::: "memory");
}

// Five-column, seven-row glyphs. Bit 4 is the leftmost pixel of each row.
static const struct { char ch; uint8_t row[7]; } display_font[] = {
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{14,4,4,4,4,4,14}}, {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
    {':',{0,4,4,0,4,4,0}}, {'-',{0,0,0,31,0,0,0}},
    {'.',{0,0,0,0,0,6,6}}, {'=',{0,0,31,0,31,0,0}},
    {'/',{1,2,2,4,8,8,16}}, {'(',{2,4,8,8,8,4,2}},
    {')',{8,4,2,2,2,4,8}}, {'_',{0,0,0,0,0,0,31}},
    {'?',{14,17,1,2,4,0,4}}, {' ',{0,0,0,0,0,0,0}},
};

static void display_text(unsigned x, unsigned y, const char *text, uint32_t color) {
    for (unsigned n = 0; text[n] && n < 104; ++n) {
        char ch = text[n];
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        const uint8_t *glyph = display_font[sizeof(display_font) / sizeof(display_font[0]) - 2].row;
        for (unsigned i = 0; i < sizeof(display_font) / sizeof(display_font[0]); ++i)
            if (display_font[i].ch == ch) { glyph = display_font[i].row; break; }
        for (unsigned row = 0; row < 7; ++row)
            for (unsigned col = 0; col < 5; ++col)
                for (unsigned dy = 0; dy < 2; ++dy)
                    for (unsigned dx = 0; dx < 2; ++dx)
                        display_pixel(x + n * 12 + col * 2 + dx, y + row * 2 + dy,
                                      glyph[row] & (1u << (4 - col)) ? color : DISPLAY_BACKGROUND);
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif
