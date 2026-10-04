/* inline_fp.h's dolrecomp_f32_from_bits against the generated header's, over
 * every 32-bit pattern: the same double, bit for bit.
 *
 * Built with the chunks' headers, in the chunks' order (gather_pipe.h, the
 * generated header, inline_fp.h), from a Visual Studio x64 prompt:
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -DBW_GUEST_MEM1=bw_guest_mem1
 *     -DBW_GUEST_MEM1_SIZE=0x02000000u -DDOLRECOMP_CPU_HEADER=\"core/cpu.h\"
 *     -Icmake/composite -Ibuild/windows/composite-src -Iref/recompcore/GXRuntime/include
 *     -Iref/recompcore/Source/Core/Core/PowerPC/StaticRecomp
 *     tests/f32_from_bits_test.c -o build/windows/f32_from_bits_test.exe
 */
#include "gather_pipe.h"
#include "generated.h"
#include "inline_fp.h"

#include <stdio.h>

#ifndef dolrecomp_f32_from_bits
int main(void) {
    u64 mismatches = 0, fast = 0;
    u32 bits = 0;
    do {
        const f64 a = dolrecomp_f32_from_bits(bits);
        const f64 b = bw_generated_f32_from_bits(bits);
        u64 x, y;
        memcpy(&x, &a, sizeof x);
        memcpy(&y, &b, sizeof y);
        if (x != y) {
            if (mismatches < 8)
                printf("mismatch %08X: %016llX vs %016llX\n", bits, (unsigned long long)x, (unsigned long long)y);
            mismatches++;
        }
        fast += ((bits >> 23) & 0xFFu) - 1u < 254u;
    } while (++bits != 0u);
    printf("4294967296 patterns, %llu on the fast path, %llu mismatches\n", (unsigned long long)fast,
           (unsigned long long)mismatches);
    return mismatches != 0;
}
#else
#error "inline_fp.h did not replace dolrecomp_f32_from_bits"
#endif
