/*
 * gen_toptable: for each top-16-bit prefix, can any word with that prefix be a non-OK a64x class? Brute force over
 * all 2^32 words (16 threads); prints the bitmap a64x_classify uses as its fast path.
 * Build: clang -O2 tools/gen_toptable.c a64x/a64x_classify.c (with a64x_classify defined as the slow one)
 */
#include "../a64x/a64x.h"
#include <pthread.h>
#include <stdio.h>

static int maybe[65536];

static void *worker(void *arg)
{
    int t = (int)(long)arg;
    for (int b = t; b < 65536; b += 16) {
        for (uint32_t low = 0; low < (1u << 16); low++) {
            uint32_t w = ((uint32_t)b << 16) | low;
            if (a64x_classify_slow(w) != A64X_OK) {
                maybe[b] = 1;
                break;
            }
        }
    }
    return NULL;
}

int a64x_cmd_gen_toptable(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    pthread_t th[16];
    for (long i = 0; i < 16; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < 16; i++)
        pthread_join(th[i], NULL);
    int n = 0;
    printf("static const uint32_t a64x_top16_maybe[2048] = {");
    for (int b = 0; b < 65536; b += 32) {
        uint32_t v = 0;
        for (int i = 0; i < 32; i++) {
            v |= (uint32_t)maybe[b + i] << i;
            n += maybe[b + i];
        }
        printf("%s0x%08x,", (b / 32) % 8 ? " " : "\n\t", v);
    }
    printf("\n}; /* %d of 65536 top-16-bit prefixes can hold a class */\n", n);
    return 0;
}
