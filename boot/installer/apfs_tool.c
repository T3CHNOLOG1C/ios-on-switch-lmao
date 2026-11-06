// SPDX-License-Identifier: GPL-2.0-only
// The installer's container fit on the Mac, for //:image and the tests:
//   apfs_tool IMAGE OFFSET             prints the container's block count and the smallest it fits to
//   apfs_tool IMAGE OFFSET BLOCKS      fits it to BLOCKS
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "apfs.h"

struct file {
    int fd;
    off_t offset;
};

static int read_block(void *context, uint64_t block, uint8_t *buffer)
{
    const struct file *f = context;
    off_t at = f->offset + (off_t)(block * SWITCH_APFS_BLOCK);
    return pread(f->fd, buffer, SWITCH_APFS_BLOCK, at) != SWITCH_APFS_BLOCK;
}

static int write_block(void *context, uint64_t block, const uint8_t *buffer)
{
    const struct file *f = context;
    off_t at = f->offset + (off_t)(block * SWITCH_APFS_BLOCK);
    return pwrite(f->fd, buffer, SWITCH_APFS_BLOCK, at) != SWITCH_APFS_BLOCK;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s IMAGE OFFSET [BLOCKS]\n", argv[0]);
        return 2;
    }
    struct file f = {open(argv[1], argc == 4 ? O_RDWR : O_RDONLY), (off_t)strtoull(argv[2], NULL, 0)};
    void *work = malloc(SWITCH_APFS_WORK_BYTES);
    if (f.fd < 0 || !work) {
        perror(argv[1]);
        return 1;
    }
    struct switch_apfs_io io = {&f, read_block, write_block};
    uint64_t blocks = 0;
    uint64_t minimum = 0;
    if (argc == 3) {
        if (switch_apfs_measure(&io, work, &blocks, &minimum)) {
            fprintf(stderr, "%s: not a container the installer can fit\n", argv[1]);
            return 1;
        }
        printf("%" PRIu64 " %" PRIu64 "\n", blocks, minimum);
        return 0;
    }
    if (switch_apfs_fit(&io, work, strtoull(argv[3], NULL, 0)) || fsync(f.fd)) {
        fprintf(stderr, "%s: the container was not fitted to %s blocks (nothing was written)\n", argv[1], argv[3]);
        return 1;
    }
    return 0;
}
