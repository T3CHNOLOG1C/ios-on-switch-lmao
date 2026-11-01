/* Native data-mount proof; only the exact signed PID1 fixture can reach these
 * PC checkpoints. Registry discovery never substitutes a guessed disk name. */
volatile uint64_t ioswitch_block_record[24];
uint64_t ioswitch_block_user_results[40];
static struct {
    uint64_t status;
    char path[2][64];
} block_query;
static uint8_t block_hash_buffer[4096];
static uint64_t block_started, block_frequency;
static unsigned block_enabled, block_finished;
static uint64_t block_ticks(void)
{
    uint64_t t;
    __asm__ volatile("isb\n\tmrs %0,cntpct_el0" : "=r"(t)::"memory");
    return t;
}
static void block_screen(void)
{
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    for (unsigned y = 340; y < 660; y++)
        for (unsigned x = 16; x < 1264; x++) ioswitch_diag_pixel(framebuffer, x, y, IOSWITCH_BACKGROUND);
    const char *message = ioswitch_block_record[3]        ? "NATIVE APFS TEST REFUSED OR FAILED"
                          : ioswitch_block_record[2] == 5 ? "APFS FILES VERIFIED - READ ONLY"
                          : ioswitch_block_record[2] >= 3 ? "APFS VOLUMES FOUND - READING FILES"
                          : ioswitch_block_record[2] == 2 ? "READ ONLY BLOCK DEVICES PUBLISHED"
                                                          : "BUILDING READ ONLY DISK INDEXES";
    text(360, message, 104, ioswitch_block_record[2] == 5 ? IOSWITCH_GREEN : IOSWITCH_WHITE);
    hex_line(400, "BLOCK TEST STAGE=", ioswitch_block_record[2]);
    hex_line(420, "BLOCK TEST ERROR=", ioswitch_block_record[3]);
    hex_line(440, "PUBLISH STATUS=", ioswitch_block_record[4]);
    hex_line(460, "VOLUME DISCOVERY STATUS=", ioswitch_block_record[6] | (ioswitch_block_record[7] << 32));
    hex_line(480, "USER STAGE/ERROR=", ioswitch_block_record[8] | (ioswitch_block_record[9] << 32));
    hex_line(500, "OS FILE BYTES=", ioswitch_block_record[10]);
    hex_line(520, "SYSTEMOS FILE BYTES=", ioswitch_block_record[11]);
    hex_line(540, "MATCHING FULL FILE HASHES=", ioswitch_block_record[12] + ioswitch_block_record[13]);
    hex_line(560, "DISCOVERY ATTEMPTS=", ioswitch_block_record[5]);
    hex_line(580, "ELAPSED COUNTER TICKS=", ioswitch_block_record[14]);
    text(620, "OWN SIGNED PID1 - DATA MOUNTS ONLY", 104, IOSWITCH_WHITE);
    sync_pixels();
    (void)ml_set_interrupts_enabled(previous);
}
static void block_start(void)
{
    int enabled = 0;
    if (!PE_parse_boot_argn("ioswitch-block-test", &enabled, sizeof(enabled)) || enabled != 1) return;
    block_enabled = 1;
    block_query.status = IOSW_BLOCK_NOT_READY;
    ioswitch_block_record[0] = UINT64_C(0x494f5357424c4f43);
    ioswitch_block_record[1] = 1;
    ioswitch_block_record[2] = 1;
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(block_frequency));
    block_started = block_ticks();
    ioswitch_block_record[15] = block_frequency;
    block_screen();
    if (ioswitch_files_record[2] != 9 || ioswitch_files_record[3] || block_frequency < 1000 ||
        block_frequency > 1000000000) {
        ioswitch_block_record[3] = 300;
        block_query.status = IOSW_BLOCK_NOT_READY;
    } else {
        int status = ioswitch_block_publish();
        ioswitch_block_record[4] = (unsigned)status;
        if (status) {
            ioswitch_block_record[3] = 301;
            block_query.status = (unsigned)status;
        } else
            ioswitch_block_record[2] = 2;
    }
    ioswitch_block_record[14] = block_ticks() - block_started;
    block_screen();
}
static int block_user_hash(uint64_t address, unsigned size, const uint8_t expected[32])
{
    SHA256_CTX hash;
    SHA256_Init(&hash);
    for (unsigned offset = 0; offset < size;) {
        unsigned n = size - offset;
        if (n > sizeof(block_hash_buffer)) n = sizeof(block_hash_buffer);
        if (copyin(address + offset, block_hash_buffer, n)) return 0;
        SHA256_Update(&hash, block_hash_buffer, n);
        offset += n;
    }
    uint8_t got[32];
    SHA256_Final(got, &hash);
    for (unsigned i = 0; i < 32; i++)
        if (got[i] != expected[i]) return 0;
    return 1;
}
static int block_volume_result_valid(const uint64_t *r, unsigned size)
{
    static const unsigned flags[] = {1, 3, 5, 8, 10, 12};
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
        if (r[flags[i]] & (1u << 29)) return 0;
    return !r[0] && !r[2] && r[4] <= 0x7fffffff && r[6] == size && r[7] && r[7] <= size && !r[9] && !r[11] && !r[13] &&
           !r[14] && !r[15];
}
static void block_checkpoint(uint64_t pc, uint64_t cpsr, int pid, int code, int error, int64_t result)
{
    if (!block_enabled || block_finished || (cpsr & 0x1f) || pid != 1 || code != 20 || error || result != 1 ||
        (pc != IOSW_USER_BLOCK_QUERY_ACK && pc != IOSW_USER_BLOCK_FINAL_ACK))
        return;
    ioswitch_block_record[16] = pc;
    ioswitch_block_record[14] = block_ticks() - block_started;
    if (pc == IOSW_USER_BLOCK_QUERY_ACK) {
        ioswitch_block_record[5]++;
        block_query.status = IOSW_BLOCK_NOT_READY;
        if (ioswitch_block_record[3])
            block_query.status = IOSW_BLOCK_CORRUPT;
        else if (ioswitch_block_record[5] > 200 || ioswitch_block_record[14] > block_frequency * 120) {
            ioswitch_block_record[3] = 302;
            block_query.status = IOSW_BLOCK_TIMEOUT;
        } else {
            int a = ioswitch_block_volume_path(0, block_query.path[0], 64);
            int b = ioswitch_block_volume_path(1, block_query.path[1], 64);
            ioswitch_block_record[6] = (unsigned)a;
            ioswitch_block_record[7] = (unsigned)b;
            if (a > IOSW_BLOCK_NOT_READY || b > IOSW_BLOCK_NOT_READY) {
                ioswitch_block_record[3] = 303;
                block_query.status = IOSW_BLOCK_CORRUPT;
            } else if (!a && !b) {
                block_query.status = IOSW_BLOCK_OK;
                ioswitch_block_record[2] = 3;
            }
        }
        if (copyout(&block_query, IOSW_USER_BLOCK_QUERY, sizeof(block_query))) ioswitch_block_record[3] = 304;
        if (ioswitch_block_record[5] == 1 || ioswitch_block_record[2] == 3 || ioswitch_block_record[3]) block_screen();
        return;
    }
    block_finished = 1;
    if (copyin(IOSW_USER_BLOCK_RESULTS, ioswitch_block_user_results, sizeof(ioswitch_block_user_results))) {
        ioswitch_block_record[3] = 305;
        block_screen();
        return;
    }
    const uint64_t *r = ioswitch_block_user_results;
    ioswitch_block_record[8] = r[0];
    ioswitch_block_record[9] = r[1];
    ioswitch_block_record[10] = r[14];
    ioswitch_block_record[11] = r[30];
    if (ioswitch_block_record[3] || ioswitch_block_record[2] != 3 || r[0] != 4 || r[1] ||
        r[4] != ioswitch_block_record[5] || r[4] < 1 || r[4] > 200 || r[5] + 1 != r[4] || r[6] || r[7] ||
        !block_volume_result_valid(r + 8, ioswitch_block_file_size_0) ||
        !block_volume_result_valid(r + 24, ioswitch_block_file_size_1)) {
        if (!ioswitch_block_record[3]) ioswitch_block_record[3] = 306;
        block_screen();
        return;
    }
    ioswitch_block_record[2] = 4;
    ioswitch_block_record[12] =
        block_user_hash(IOSW_USER_BLOCK_OS_BYTES, ioswitch_block_file_size_0, ioswitch_block_file_digest_0);
    ioswitch_block_record[13] =
        block_user_hash(IOSW_USER_BLOCK_SYSTEM_BYTES, ioswitch_block_file_size_1, ioswitch_block_file_digest_1);
    if (ioswitch_block_record[12] && ioswitch_block_record[13])
        ioswitch_block_record[2] = 5;
    else
        ioswitch_block_record[3] = 307;
    block_screen();
}
