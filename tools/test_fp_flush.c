// Synthetic regression for FPCR.FZ initialization and scalar FMINNM operand flushing; no game data needed.
// clang -O1 -march=x86-64-v3 -fno-strict-aliasing -Iruntime tools/test_fp_flush.c -o build/test_fp_flush.exe
#include <inttypes.h>
#include <stdio.h>

#include "cpu.h"
#include "simd.h"

// A zero/subnormal comparison must return the correct operand bits even when host DAZ makes them compare equal.
// Keep the operation out of the caller so the compiler cannot constant-fold these fixed regression operands.
static __attribute__((noinline)) int check_s(uint64_t fpcr, uint32_t a, uint32_t b, uint32_t want) {
    Cpu c = {0};
    c.v[0] = ~(u128)0;
    c.v[1] = a;
    c.v[2] = b;
    rt_fpcr_write(&c, fpcr);
    fp_op_s(&c, FOP_MINNM, 0, 1, 2);
    if (c.v[0] == (u128)want) return 0;
    fprintf(stderr, "fminnm.s fpcr=%08" PRIx64 " a=%08" PRIx32 " b=%08" PRIx32
                    " got=%016" PRIx64 ":%016" PRIx64 " want=%08" PRIx32 "\n",
            fpcr, a, b, (uint64_t)(c.v[0] >> 64), (uint64_t)c.v[0], want);
    return 1;
}

static __attribute__((noinline)) int check_d(uint64_t fpcr, uint64_t a, uint64_t b, uint64_t want) {
    Cpu c = {0};
    c.v[0] = ~(u128)0;
    c.v[1] = a;
    c.v[2] = b;
    rt_fpcr_write(&c, fpcr);
    fp_op_d(&c, FOP_MINNM, 0, 1, 2);
    if (c.v[0] == (u128)want) return 0;
    fprintf(stderr, "fminnm.d fpcr=%08" PRIx64 " a=%016" PRIx64 " b=%016" PRIx64
                    " got=%016" PRIx64 ":%016" PRIx64 " want=%016" PRIx64 "\n",
            fpcr, a, b, (uint64_t)(c.v[0] >> 64), (uint64_t)c.v[0], want);
    return 1;
}

// Inputs and expected FMINNM results, first without FZ and then with FZ. Include both operand orders, signed
// zero, tiny and largest subnormals, smallest normals, and ordinary normals. DN should not change these numbers.
static const struct { uint32_t a, b, gradual, flush; } cases_s[] = {
    {0x00000021, 0x00000000, 0x00000000, 0x00000000},
    {0x00000000, 0x00000021, 0x00000000, 0x00000000},
    {0x80000021, 0x00000000, 0x80000021, 0x80000000},
    {0x00000000, 0x80000021, 0x80000021, 0x80000000},
    {0x00000001, 0x3f800000, 0x00000001, 0x00000000},
    {0x3f800000, 0x00000001, 0x00000001, 0x00000000},
    {0x80000001, 0x3f800000, 0x80000001, 0x80000000},
    {0x3f800000, 0x80000001, 0x80000001, 0x80000000},
    {0x007fffff, 0x00800000, 0x007fffff, 0x00000000},
    {0x00800000, 0x007fffff, 0x007fffff, 0x00000000},
    {0x807fffff, 0x80800000, 0x80800000, 0x80800000},
    {0x80800000, 0x807fffff, 0x80800000, 0x80800000},
    {0x00000000, 0x80000000, 0x80000000, 0x80000000},
    {0x80000000, 0x00000000, 0x80000000, 0x80000000},
    {0x00800000, 0x3f800000, 0x00800000, 0x00800000},
    {0x80800000, 0x3f800000, 0x80800000, 0x80800000},
    {0xbf800000, 0x3f800000, 0xbf800000, 0xbf800000},
    {0x3f800000, 0xbf800000, 0xbf800000, 0xbf800000},
};
static const struct { uint64_t a, b, gradual, flush; } cases_d[] = {
    {0x21, 0, 0, 0},
    {0, 0x21, 0, 0},
    {0x8000000000000021ull, 0, 0x8000000000000021ull, 0x8000000000000000ull},
    {0, 0x8000000000000021ull, 0x8000000000000021ull, 0x8000000000000000ull},
    {1, 0x3ff0000000000000ull, 1, 0},
    {0x3ff0000000000000ull, 1, 1, 0},
    {0x8000000000000001ull, 0x3ff0000000000000ull, 0x8000000000000001ull, 0x8000000000000000ull},
    {0x3ff0000000000000ull, 0x8000000000000001ull, 0x8000000000000001ull, 0x8000000000000000ull},
    {0x000fffffffffffffull, 0x0010000000000000ull, 0x000fffffffffffffull, 0},
    {0x0010000000000000ull, 0x000fffffffffffffull, 0x000fffffffffffffull, 0},
    {0x800fffffffffffffull, 0x8010000000000000ull, 0x8010000000000000ull, 0x8010000000000000ull},
    {0x8010000000000000ull, 0x800fffffffffffffull, 0x8010000000000000ull, 0x8010000000000000ull},
    {0, 0x8000000000000000ull, 0x8000000000000000ull, 0x8000000000000000ull},
    {0x8000000000000000ull, 0, 0x8000000000000000ull, 0x8000000000000000ull},
    {0x0010000000000000ull, 0x3ff0000000000000ull, 0x0010000000000000ull, 0x0010000000000000ull},
    {0x8010000000000000ull, 0x3ff0000000000000ull, 0x8010000000000000ull, 0x8010000000000000ull},
    {0xbff0000000000000ull, 0x3ff0000000000000ull, 0xbff0000000000000ull, 0xbff0000000000000ull},
    {0x3ff0000000000000ull, 0xbff0000000000000ull, 0xbff0000000000000ull, 0xbff0000000000000ull},
};

int main(void) {
    // Alternate FZ on/off so one case's MXCSR state cannot silently determine the next case's behavior.
    static const uint64_t modes[] = {FPCR_FZ, 0, FPCR_FZ | FPCR_DN, FPCR_DN};
    int failed = 0, tested = 0;
    for (size_t i = 0; i < sizeof cases_s / sizeof *cases_s; i++)
        for (size_t j = 0; j < sizeof modes / sizeof *modes; j++) {
            uint64_t fpcr = modes[j];
            failed += check_s(fpcr, cases_s[i].a, cases_s[i].b,
                              fpcr & FPCR_FZ ? cases_s[i].flush : cases_s[i].gradual);
            tested++;
        }
    for (size_t i = 0; i < sizeof cases_d / sizeof *cases_d; i++)
        for (size_t j = 0; j < sizeof modes / sizeof *modes; j++) {
            uint64_t fpcr = modes[j];
            failed += check_d(fpcr, cases_d[i].a, cases_d[i].b,
                              fpcr & FPCR_FZ ? cases_d[i].flush : cases_d[i].gradual);
            tested++;
        }
    printf("FP flush regression: %d cases, %d failures\n", tested, failed);
    return failed != 0;
}
