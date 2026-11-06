// SPDX-License-Identifier: GPL-2.0-only
#include "stream.h"

struct source {
    const struct switch_image *image;
    const struct switch_install_io *io;
    uint8_t *buffer;
    uint32_t chunk;
    uint32_t filled;
    uint32_t used;
};

static int refill(struct source *s)
{
    uint64_t at = (uint64_t)s->chunk * SWITCH_IMAGE_HASH_CHUNK;
    if (s->chunk >= s->image->hash_count || at >= s->image->source_bytes) {
        return 1;
    }
    uint64_t left = s->image->source_bytes - at;
    uint32_t n = left < SWITCH_IMAGE_HASH_CHUNK ? (uint32_t)left : SWITCH_IMAGE_HASH_CHUNK;
    if (s->io->read(s->io->context, s->buffer, n) || s->io->verify(s->io->context, s->chunk, s->buffer, n) ||
        (!s->chunk && switch_image_check_superblock(s->image, s->buffer, n))) {
        return 1;
    }
    s->chunk++;
    s->filled = n;
    s->used = 0;
    return 0;
}

int switch_install_stream(const struct switch_image *image, const struct switch_install_io *io, uint64_t first_sector,
                          uint64_t partition_sectors, uint8_t *buffer, const uint8_t *zeros, uint32_t zero_bytes)
{
    if (!image || !io || !io->read || !io->verify || !io->write || !buffer || !zeros || !zero_bytes ||
        zero_bytes % image->block_size || partition_sectors * 512 < image->minimum_bytes) {
        return 1;
    }
    struct source s = {image, io, buffer, 0, 0, 0};
    uint64_t done = 0;
    if (io->progress) {
        io->progress(io->context, 0, image->write_bytes);
    }
    for (uint32_t i = 0; i < image->extent_count; i++) {
        const struct switch_image_extent *e = &image->extents[i];
        for (uint64_t at = 0; at < e->bytes;) {
            const uint8_t *data = zeros;
            uint64_t left = e->bytes - at;
            uint32_t n = left < zero_bytes ? (uint32_t)left : zero_bytes;
            if (e->source != SWITCH_IMAGE_ZERO) {
                if (s.used == s.filled && refill(&s)) {
                    return 1;
                }
                data = buffer + s.used;
                n = left < s.filled - s.used ? (uint32_t)left : s.filled - s.used;
                s.used += n;
            }
            if (io->write(io->context, first_sector + (e->offset + at) / 512, n / 512, data)) {
                return 1;
            }
            at += n;
            done += n;
            if (io->progress) {
                io->progress(io->context, done, image->write_bytes);
            }
        }
    }
    return s.used != s.filled || s.chunk != image->hash_count;
}
