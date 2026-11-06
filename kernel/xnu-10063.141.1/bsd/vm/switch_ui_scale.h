/*
 * SwitchUIScale (vm_unix.c): the scales boot-arg switch-ui-scale names, and the cache text each one's pager sites
 * write. Free of kernel types so kernel/tests can drive it.
 */
#ifndef _VM_SWITCH_UI_SCALE_H_
#define _VM_SWITCH_UI_SCALE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Pixels per point on the 720x1280 panel. Only scales whose screen is whole points both ways: UIKit and every
 * framework below it assume a point grid that lands on pixels, and 720 x 1280 shares the factor 80 with nothing
 * finer. num and den stay at most 31, the integers fmov encodes.
 */
struct switch_ui_scale {
	const char *name;    /* the boot-arg's value */
	uint8_t     num, den;
	uint32_t    float_bits; /* num / den as MobileGestalt's float */
	/* ProgressUI's 239 x 4 point bar, as two floats whose single-precision products with float_bits are whole */
	uint32_t    progress_bar_bits[2];
};

static const struct switch_ui_scale switch_ui_scales[] = {
	/* 720x1280 points: the stock screen, every UI scale site left stock */
	{"1", 1, 1, 0x3f800000, {0x436f0000, 0x40800000}},
	/* 576x1024; 239.2 x 4: a 299 x 5 pixel bar */
	{"1.25", 5, 4, 0x3fa00000, {0x436f3333, 0x40800000}},
	/* 450x800: a 9.7- to 12.9-inch iPad's point at 209.8 ppi; 238.75 x 3.75: a 382 x 6 pixel bar */
	{"1.6", 8, 5, 0x3fcccccd, {0x436ec000, 0x40700000}},
};
#define SWITCH_UI_SCALES        (sizeof(switch_ui_scales) / sizeof(switch_ui_scales[0]))
#define SWITCH_UI_SCALE_UNIT    0u
#define SWITCH_UI_SCALE_DEFAULT 1u /* 1.25 */

static inline bool
switch_ui_scale_is_unit(const struct switch_ui_scale *scale)
{
	return scale->num == scale->den;
}

/* The scale for the boot-arg's value (NULL: no switch-ui-scale), or NULL for a value that names none. */
static inline const struct switch_ui_scale *
switch_ui_scale_named(const char *value)
{
	if (value == NULL) {
		return &switch_ui_scales[SWITCH_UI_SCALE_DEFAULT];
	}
	for (size_t i = 0; i < SWITCH_UI_SCALES; i++) {
		const char *name = switch_ui_scales[i].name;
		size_t n = 0;
		while (name[n] != '\0' && name[n] == value[n]) {
			n++;
		}
		if (name[n] == '\0' && value[n] == '\0') {
			return &switch_ui_scales[i];
		}
	}
	return NULL;
}

/* The scale a boot uses: the named one, and the stock screen for a value that names none. */
static inline const struct switch_ui_scale *
switch_ui_scale_for_boot_arg(const char *value)
{
	const struct switch_ui_scale *scale = switch_ui_scale_named(value);
	return scale != NULL ? scale : &switch_ui_scales[SWITCH_UI_SCALE_UNIT];
}

/*
 * fmov d<rd>, #value for an integer value 1..31. VFPExpandImm: value = (16 + efgh) / 16 * 2^n with imm8 = 0:b:cd:efgh,
 * n = cd + 1 when b is 0 (1..4) and n = cd - 3 when b is 1 (0 here: b:cd = 0b111).
 */
static inline uint32_t
switch_a64_fmov_d_int(unsigned rd, unsigned value)
{
	unsigned n = 0;
	while ((value >> (n + 1)) != 0) {
		n++;
	}
	const unsigned efgh = ((value << 4) >> n) - 16;
	const unsigned b_cd = n == 0 ? 0x7u : n - 1;
	return 0x1e601000u | (((b_cd << 4) | efgh) << 13) | rd;
}

static inline uint32_t
switch_a64_movz_w(unsigned rd, uint16_t imm16)
{
	return 0x52800000u | ((uint32_t)imm16 << 5) | rd;
}

static inline uint32_t
switch_a64_movk_w_lsl16(unsigned rd, uint16_t imm16)
{
	return 0x72a00000u | ((uint32_t)imm16 << 5) | rd;
}

static inline void
switch_a64_put(uint8_t *out, uint32_t insn)
{
	out[0] = (uint8_t)insn;
	out[1] = (uint8_t)(insn >> 8);
	out[2] = (uint8_t)(insn >> 16);
	out[3] = (uint8_t)(insn >> 24);
}

/* How a site's edit gets its bytes: fixed in the site, or made for the boot's scale. */
enum switch_ui_scale_edit_kind {
	SWITCH_US_FIXED = 0,
	SWITCH_US_FBS_OVERRIDE, /* 20 bytes: fmov d8, #num; fmov d1, #den; fdiv d8, d8, d1; tbnz w0, #0, +8; fmov d8, xzr */
	SWITCH_US_MG_SCALE,     /* 8 bytes: movz w19, #float_bits[15:0]; movk w19, #float_bits[31:16], lsl #16 */
	SWITCH_US_PROGRESS_BAR, /* 8 bytes: progress_bar_bits, little-endian */
};

/* Writes the edit's bytes for the scale; false when the kind's length is not size. */
static inline bool
switch_ui_scale_edit_bytes(enum switch_ui_scale_edit_kind kind, const struct switch_ui_scale *scale, uint8_t *out,
    size_t size)
{
	switch (kind) {
	case SWITCH_US_FBS_OVERRIDE:
		if (size != 20) {
			return false;
		}
		switch_a64_put(out, switch_a64_fmov_d_int(8, scale->num));
		switch_a64_put(out + 4, switch_a64_fmov_d_int(1, scale->den));
		switch_a64_put(out + 8, 0x1e611908u);  /* fdiv d8, d8, d1 */
		switch_a64_put(out + 12, 0x37000040u); /* tbnz w0, #0, +0x8 */
		switch_a64_put(out + 16, 0x9e6703e8u); /* fmov d8, xzr */
		return true;
	case SWITCH_US_MG_SCALE:
		if (size != 8) {
			return false;
		}
		switch_a64_put(out, switch_a64_movz_w(19, (uint16_t)scale->float_bits));
		switch_a64_put(out + 4, switch_a64_movk_w_lsl16(19, (uint16_t)(scale->float_bits >> 16)));
		return true;
	case SWITCH_US_PROGRESS_BAR:
		if (size != 8) {
			return false;
		}
		switch_a64_put(out, scale->progress_bar_bits[0]);
		switch_a64_put(out + 4, scale->progress_bar_bits[1]);
		return true;
	case SWITCH_US_FIXED:
		break;
	}
	return false;
}

#endif /* _VM_SWITCH_UI_SCALE_H_ */
