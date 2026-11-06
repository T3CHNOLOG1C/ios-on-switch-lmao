/*
 * Built for an A12 (-mcpu=apple-a12): its atomics are LSE, its acquire loads LDAPR, its half-precision arithmetic
 * FP16. run_translated_test translates it offline and runs both; they must print the same.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

static _Atomic uint64_t counter;
static _Atomic uint32_t flags;
static _Atomic uint64_t slot;

static void *work(void *arg)
{
    uint64_t n = (uint64_t)(uintptr_t)arg;
    for (uint64_t i = 0; i < 200000; i++) {
        atomic_fetch_add_explicit(&counter, n, memory_order_acq_rel);
        atomic_fetch_or_explicit(&flags, 1u << (i & 31), memory_order_relaxed);
        uint64_t seen = atomic_load_explicit(&slot, memory_order_acquire);
        while (!atomic_compare_exchange_weak_explicit(&slot, &seen, seen + 1, memory_order_acq_rel,
                                                      memory_order_acquire)) {
        }
    }
    return NULL;
}

static __attribute__((noinline)) _Float16 half_poly(_Float16 x)
{
    return x * x * (_Float16)0.5f + x - (_Float16)1.25f;
}

int main(void)
{
    pthread_t t[4];
    for (uintptr_t i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, work, (void *)(i + 1));
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    float sum = 0;
    for (int i = -8; i <= 8; i++)
        sum += (float)half_poly((_Float16)(i * 0.375f));
    uint64_t old = atomic_exchange_explicit(&slot, 7, memory_order_seq_cst);
    printf("counter %llu flags %08x slot %llu then %llu half %.6f\n", (unsigned long long)atomic_load(&counter),
           atomic_load(&flags), (unsigned long long)old, (unsigned long long)atomic_load(&slot), (double)sum);
    return 0;
}
