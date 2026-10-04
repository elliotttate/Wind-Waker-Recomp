/* cmake/composite/native_anim.c against the translations it stands in for.
 *
 * Build and run from the worktree root (x64, in the Visual Studio environment):
 *
 *   clang -O2 -march=x86-64-v3 -ffp-contract=off -Icmake/composite
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\GXRuntime\include
 *     -IE:\Github\Wind-Waker-Recomp\ref\recompcore\Source\Core\Core\PowerPC\StaticRecomp
 *     tests/native_anim_test.c cmake/composite/native_anim.c cmake/composite/direct_calls.c
 *     E:\Github\Wind-Waker-Recomp\build\windows\app\gxruntime_build\gxruntime.lib -o native_anim_test.exe
 *   native_anim_test MODULE.dll [CASES_PER_FUNCTION=60000] [BENCH_CALLS=200000] [--module-natives]
 *
 * MODULE.dll is a Windows game module (gGZLE01_recomp.dll) without these
 * natives (E:\Github\Wind-Waker-Recomp\build\windows\W-final\gGZLE01_recomp.dll),
 * read, never written. Its translation runs as in play: direct calls and its
 * edge filter on, the host's flags quiet, an edge service that ends the run at
 * the return address and fails the case if asked anything anywhere else; its
 * own natives off (the second set's partial calcTransform and
 * J3DGetKeyFrameInterpolationS among them), or with --module-natives on.
 * (A joint none of whose channels searches its keys this native leaves to
 * the second set's: such cases decline here.)
 *
 * For each function: random key tables (1 to 300 keys, three or four values
 * a key, times strictly increasing, or with equal or decreasing times now
 * and then), frames before the first key, after the last, on a key, between
 * keys, and NaN; values, tangents and registers (both halves) mostly
 * ordinary, sometimes zeros, denormals, huge values, infinities and NaNs;
 * GQR5's scale; FPSCR (enables, NI with or without the host's flush to zero);
 * reservations on the frames; and for calcTransform random joints of nine
 * channels, each with zero, one or many keys, the decimal shift, and the
 * transform anywhere near. The declining cases: tables, keys, the object or
 * the transform out of RAM or over a frame or under the transform, the magic
 * double not the usual one, GQR5 not loading shorts, paired singles off or
 * quantised, FP off, rounding not to nearest, an exception pending, a write
 * journal, aliases over MEM1, the budget or the deadline inside the function
 * (and cases right at the edge, which must run), and the host not quiet, the
 * edge filter off or a boundary the path crosses watched. Every byte of the
 * CPU state (the cycle suffix included) and of the writable pages must match
 * - or, where the native declines, nothing may have changed. RAM outside the
 * test's pages is read-only in both images.
 *
 * Then a microbenchmark: the translation through the module's dispatcher
 * against the native, ns per call. */
#include "native_anim.h"
#include "native4_harness.h"

#define AREA 0x80100000u
#define AREA_BYTES 0x20000u
#define SDA2 (AREA + 0x100u)        /* the constants r2 reaches, at r2 - 13176 .. r2 - 12988 */
#define R2 (SDA2 + 13176u)
#define OBJECT (AREA + 0x400u)      /* J3DAnmTransformKey */
#define TRANSFORM (AREA + 0x500u)   /* J3DTransformInfo */
#define TABLES (AREA + 0x1000u)     /* key tables (6 bytes each) */
#define KEYS (AREA + 0x4000u)       /* key data, 0x8000 bytes */
#define KEYS_BYTES 0x8000u
#define STACK (AREA + 0x1E000u)
#define MAGIC 0x4330000080000000ull

static const Region TEST_REGIONS[] = {{AREA, AREA_BYTES}};

static const u32 FUNCTIONS[] = {BLUEWAKE_ANIM_HERMITE, BLUEWAKE_ANIM_KEY_F, BLUEWAKE_ANIM_KEY_S,
                                BLUEWAKE_ANIM_TRANSFORM, BLUEWAKE_ANIM_INVERSE_TRANSPOSE};
static const char* const NAMES[] = {"JMAHermiteInterpolation", "J3DGetKeyFrameInterpolation<f32>",
                                    "J3DGetKeyFrameInterpolationS", "J3DAnmTransformKey::calcTransform",
                                    "J3DPSCalcInverseTranspose"};
#define FUNCTION_COUNT 5u
#define MATRIX (AREA + 0x600u)

typedef struct Case {
    CPUState cpu;
    bool must_decline, journal, aliases, flush, trouble;
    unsigned trouble_kind;
} Case;

static u32 bits_of(f32 v) {
    u32 b;
    memcpy(&b, &v, 4);
    return b;
}

static unsigned s_specials; /* specials in 64 for this case's values */

static u32 value_bits(void) {
    return below(4u) == 0u ? any_single(s_specials) : bits_of((f32)((unit() - 0.5) * 200.0));
}

/* A key table's data at `data`: `count` keys of `stride` values (floats or
 * shorts), and the time the frame should fall at. Returns the frame. */
static f64 make_keys(u8* ram, u32 data, unsigned count, unsigned stride, bool shorts, unsigned scenario) {
    const bool ordered = scenario % 13u != 5u;
    f32 t = shorts ? (f32)(s32)(below(200u)) - 100.0f : (f32)((unit() - 0.3) * 50.0);
    f32 first = t, last = t, pick = t;
    const unsigned at = below(count);
    for (unsigned i = 0; i < count; ++i) {
        if (i == at)
            pick = t;
        last = t;
        if (shorts) {
            put16(ram, data + 2u * stride * i, (u16)(s16)t);
            for (unsigned j = 1; j < stride; ++j)
                put16(ram, data + 2u * (stride * i + j), (u16)(next() % 4u == 0u ? next() : below(2000u) - 1000u));
        } else {
            put32(ram, data + 4u * stride * i, bits_of(t));
            for (unsigned j = 1; j < stride; ++j)
                put32(ram, data + 4u * (stride * i + j), value_bits());
        }
        f32 step = shorts ? (f32)(1u + below(20u)) : (f32)(0.05 + unit() * 5.0);
        if (!ordered && below(4u) == 0u)
            step = below(2u) ? 0.0f : -step;
        t += step;
        if (shorts && t > 32000.0f)
            t = 32000.0f;
    }
    if (!shorts && s_specials != 0u && below(8u) == 0u)
        put32(ram, data + 4u * stride * below(count), any_single(64u)); /* a special time */
    if (below(5u) == 0u) /* a frame that is a double, not a single: the general replay's */
        return (f64)(first + unit() * (last - first)) * (1.0 + 0x1p-40);
    switch (below(10u)) {
    case 0: return first - 1.0 - unit() * 10.0;
    case 1: return last + unit() * 10.0;
    case 2: return pick;
    case 3: return below(2u) ? first : last;
    case 4: return s_specials != 0u ? any_register(64u) : (f64)first;
    default: return (f64)(f32)(first + unit() * (last - first));
    }
}

static void table(u8* ram, u32 at, u16 count, u16 offset, u16 type) {
    put16(ram, at, count);
    put16(ram, at + 2u, offset);
    put16(ram, at + 4u, type);
}

static unsigned key_count(void) {
    switch (below(8u)) {
    case 0: return 2u;
    case 1: return 1u + below(300u);
    case 2: return 3u;
    default: return 2u + below(40u);
    }
}

/* One channel of a transform: zero, one or many keys at a fresh place. */
static u32 s_next_key;
static void make_channel(u8* ram, u32 entry, bool shorts, u32 base, unsigned scenario) {
    const unsigned kind = below(10u);
    const unsigned count = kind < 2u ? 0u : kind < 4u ? 1u : key_count();
    const unsigned stride = below(2u) ? 3u : 4u;
    const u32 size = shorts ? 2u : 4u;
    if (count == 0u) {
        table(ram, entry, 0, (u16)next(), (u16)next());
        return;
    }
    const u32 need = size * stride * (count + 1u);
    if (s_next_key + need > KEYS + KEYS_BYTES)
        s_next_key = KEYS + 4u * below(16u);
    const u32 data = s_next_key;
    s_next_key += need + 4u * below(4u);
    (void)make_keys(ram, data, count, stride, shorts, scenario);
    if (count == 1u) {
        /* The one key's value. */
        if (shorts)
            put16(ram, data, (u16)next());
        else
            put32(ram, data, value_bits());
    }
    table(ram, entry, (u16)count, (u16)((data - base) / size), (u16)(stride == 4u));
}

static Case build(u8* ram, unsigned which, unsigned scenario) {
    Case k;
    memset(&k, 0, sizeof k);
    s_specials = scenario % 9u == 4u ? 24u : scenario % 9u == 7u ? 6u : 0u;
    CPUState* c = &k.cpu;
    random_cpu(c, ram);
    for (u32 i = 0; i < AREA_BYTES; i += 4u)
        put32(ram, AREA + i, next());
    /* The constants. */
    c->gpr[2] = R2;
    put32(ram, R2 - 13000u, 0x3F800000u); /* 1.0 */
    put32(ram, R2 - 12996u, 0x40000000u); /* 2.0 */
    put32(ram, R2 - 12992u, 0x40400000u); /* 3.0 */
    put32(ram, R2 - 12988u, 0xC0000000u); /* -2.0 */
    put32(ram, R2 - 13176u, 0x3F800000u); /* 1.0 */
    put32(ram, R2 - 13172u, 0u);          /* 0.0 */
    put64(ram, R2 - 13160u, MAGIC);
    if (scenario % 31u == 9u)
        put32(ram, R2 - 13000u + 4u * below(4u), any_single(32u));
    if (scenario % 37u == 11u) {
        /* Not the magic double: J3DGetKeyFrameInterpolationS always converts
         * a short with it (calcTransform only on a rotation's search). */
        put64(ram, R2 - 13160u, ((u64)next() << 32) | next());
        k.must_decline = which == 2u;
    }
    /* GQR5: loading signed shorts, the scale mostly zero; its other fields random. */
    c->gqr[5] = (next() & ~0x3F070000u) | (7u << 16) | (below(8u) == 0u ? (below(64u) << 24) : 0u);
    u32 sp = STACK - 8u * below(64u);
    c->gpr[1] = sp;
    c->reserve_addr = below(2u) ? sp - 8u * below(10u) : TRANSFORM + 4u * below(8u);
    k.flush = (c->fpscr & 0x4u) != 0u && below(2u) != 0u;
    s_next_key = KEYS + 4u * below(16u);
    switch (which) {
    case 0: /* JMAHermiteInterpolation */
        for (unsigned r = 1; r <= 7u; ++r)
            c->fpr[r] = below(3u) == 0u ? any_register(s_specials ? s_specials : 4u)
                                         : (f64)(f32)((unit() - 0.5) * 100.0);
        if (below(3u) != 0u) { /* time1 after time0, the frame between */
            c->fpr[5] = (f64)(f32)(c->fpr[2] + 0.5 + unit() * 10.0);
            c->fpr[1] = (f64)(f32)(c->fpr[2] + unit() * (c->fpr[5] - c->fpr[2]));
        }
        break;
    case 1:   /* J3DGetKeyFrameInterpolation<f32> */
    case 2: { /* J3DGetKeyFrameInterpolationS */
        const bool shorts = which == 2u;
        const unsigned count = key_count(), stride = below(2u) ? 3u : 4u;
        const u32 data = KEYS + (shorts ? 2u : 4u) * below(64u);
        c->fpr[1] = make_keys(ram, data, count, stride, shorts, scenario);
        const u32 at = TABLES + 2u * below(64u);
        table(ram, at, (u16)count, (u16)next(), (u16)(stride == 4u ? 1u + below(3u) : 0u));
        c->gpr[3] = at;
        c->gpr[4] = data;
        break;
    }
    case 4: { /* J3DPSCalcInverseTranspose */
        const u32 src = MATRIX + 4u * below(8u);
        for (unsigned i = 0; i < 12u; ++i)
            put32(ram, src + 4u * i, below(6u) == 0u ? any_single(s_specials ? s_specials : 2u)
                                                    : bits_of((f32)((unit() - 0.5) * 8.0)));
        switch (below(8u)) {
        case 0: /* a zero determinant: two rows the same */
            for (unsigned i = 0; i < 3u; ++i)
                put32(ram, src + 16u + 4u * i, get32(ram, src + 4u * i));
            break;
        case 2: /* tiny or huge: products leave the normal range (the general replay's) */
        case 3: {
            const f32 scale = below(2u) ? 1e-20f : 3e18f;
            for (unsigned i = 0; i < 12u; ++i)
                put32(ram, src + 4u * i, bits_of((f32)((unit() - 0.5) * 8.0) * scale));
            break;
        }
        case 1: /* a rotation and a scale */
            put32(ram, src, bits_of(0.6f)), put32(ram, src + 4u, bits_of(-0.8f)), put32(ram, src + 8u, 0u);
            put32(ram, src + 16u, bits_of(0.8f)), put32(ram, src + 20u, bits_of(0.6f)), put32(ram, src + 24u, 0u);
            put32(ram, src + 32u, 0u), put32(ram, src + 36u, 0u), put32(ram, src + 40u, bits_of(2.0f));
            break;
        default: break;
        }
        c->gpr[3] = src;
        c->gpr[4] = below(4u) == 0u ? src + 4u * below(8u) : MATRIX + 0x80u + 4u * below(8u); /* sometimes on the source */
        if (below(32u) == 0u)
            c->gpr[4] = 0xCC000000u; /* not RAM: only a zero determinant (no stores) may run */
        c->gqr[0] = 0u;
        break;
    }
    default: { /* calcTransform */
        const u32 joint = below(16u);
        const u32 anm_table = TABLES + 2u * below(32u);
        put32(ram, OBJECT + 40u, anm_table);
        put32(ram, OBJECT + 16u, KEYS);
        put32(ram, OBJECT + 20u, KEYS);
        put32(ram, OBJECT + 24u, KEYS);
        put32(ram, OBJECT + 36u, below(4u) == 0u ? next() : below(8u) == 0u ? 32u + below(32u) : below(4u));
        for (unsigned ch = 0; ch < 9u; ++ch) {
            static const u32 offsets[9] = {0u, 18u, 36u, 6u, 24u, 42u, 12u, 30u, 48u};
            make_channel(ram, anm_table + joint * 54u + offsets[ch], ch >= 3u && ch < 6u, KEYS, scenario);
        }
        c->fpr[1] = s_specials != 0u && below(4u) == 0u ? any_register(64u) : (f64)(f32)(unit() * 300.0 - 20.0);
        if (below(5u) == 0u)
            c->fpr[1] *= 1.0 + 0x1p-40; /* a double, not a single: the general replay's */
        c->gpr[3] = OBJECT;
        c->gpr[4] = (next() & 0xFFFF0000u) | joint;
        c->gpr[5] = TRANSFORM + 2u * below(16u);
        c->gqr[0] = 0u;
        break;
    }
    }
    c->pc = FUNCTIONS[which];
    /* The declining cases, and the edges. */
    switch (scenario % 67u) {
    case 1: c->msr &= ~PPC_MSR_FP; k.must_decline = true; break;
    case 2: c->exception = 1u; k.must_decline = true; break;
    case 3: k.journal = k.must_decline = which != 0u; break;
    case 4: k.aliases = k.must_decline = true; break;
    case 5: c->fpscr = (c->fpscr & ~3u) | (1u + below(3u)); k.must_decline = true; break;
    case 6:
        if (which == 2u || which == 3u) {
            c->gqr[5] = (c->gqr[5] & ~0x00070000u) | (below(7u) << 16);
            k.must_decline = true;
        }
        break;
    case 7:
        if (which >= 2u) {
            c->hid2 &= ~PPC_HID2_LSQE;
            k.must_decline = true;
        }
        break;
    case 8:
        if (which >= 3u) {
            c->gqr[0] = below(2u) ? 0x00040000u : 0x00000006u;
            k.must_decline = true;
        }
        break;
    case 9:
        if (which != 0u && which != 4u) {
            c->gpr[1] = 0x80000000u + 4u * below(3u); /* the frame below RAM */
            k.must_decline = true;
        }
        break;
    case 10:
        if (which == 1u || which == 2u) {
            c->gpr[3] = 0xCC000000u;
            k.must_decline = true;
        }
        if (which == 3u) {
            c->gpr[3] = OBJECT | 0x40000000u; /* the object through the uncached mirror */
            k.must_decline = true;
        }
        if (which == 4u) {
            c->gpr[3] = below(2u) ? 0xCC000000u : MATRIX | 0x40000000u;
            k.must_decline = true;
        }
        break;
    case 11:
        if (which == 3u) {
            c->gpr[5] = sp - 24u; /* the transform on the frame */
            k.must_decline = true;
        }
        if (which == 1u || which == 2u) {
            c->gpr[4] = sp - 8u; /* the keys under the frame */
            k.must_decline = true;
        }
        break;
    case 12:
        if (which == 3u) {
            put32(ram, OBJECT + 40u, TRANSFORM - 54u * (c->gpr[4] & 0xFFFFu)); /* the table under the transform */
            c->gpr[5] = TRANSFORM;
            k.must_decline = true;
        }
        break;
    case 13: c->downcount = -c->cycle_budget + 1 + (s64)below(700u); break; /* the budget near: either */
    case 14: c->cycle_deadline_budget = 1 + (s64)below(800u); break;         /* the deadline near: either */
    case 15: c->cycle_budget = 0; k.must_decline = true; break;
    case 16: c->downcount = 1 + (s64)below(8u); k.must_decline = true; break;
    case 17:
    case 18:
        k.trouble = true;
        k.trouble_kind = below(4u);
        break;
    default: break;
    }
    return k;
}

/* The host busy (for the module's translation too: it reads the same
 * flags), its edge filter off (both), or a boundary watched (the native's
 * list only: the module's is its build's). */
static void make_trouble(Harness* h, unsigned which, unsigned kind, CPUState* c) {
    switch (kind) {
    case 0: s_sources_dirty = true; break;
    case 1: s_decrementer_pending = true; c->msr |= PPC_MSR_EE; break;
    case 2: bw_edge_filter_enabled = false; h->filter(false); break;
    default: watch(which == 3u ? 0x802F2DACu : 0x803012D8u); break;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: native_anim_test MODULE.dll [CASES_PER_FUNCTION] [BENCH_CALLS] [--module-natives]\n");
        return 2;
    }
    const unsigned cases = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 60000u;
    const unsigned bench_calls = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 200000u;
    const bool module_natives = argc > 4 && strcmp(argv[4], "--module-natives") == 0;
    Harness h;
    if (!harness_load(&h, argv[1], module_natives, TEST_REGIONS, sizeof TEST_REGIONS / sizeof TEST_REGIONS[0]))
        return 1;
    if (module_natives)
        printf("the module's own natives on\n");
    bluewake_native_anim_enabled = 1;
    int status = 0;
    for (unsigned which = 0; which < FUNCTION_COUNT; ++which) {
        const u32 entry = FUNCTIONS[which];
        unsigned ran = 0, declined = 0, edges = 0, troubled = 0;
        s64 most = 0;
        for (unsigned i = 0; i < cases; ++i) {
            Case k = build(h.native_ram, which, i);
            reset_host();
            if (k.trouble)
                make_trouble(&h, which, k.trouble_kind, &k.cpu);
            const int same = harness_case(&h, bluewake_native_anim, entry, NAMES[which], i, &k.cpu, k.must_decline,
                                          k.journal, k.aliases, k.flush);
            if (k.trouble && k.trouble_kind == 2u)
                h.filter(true);
            if (!same) {
                declined++;
                continue;
            }
            ran++;
            const u32 scenario = i % 67u;
            edges += scenario == 13u || scenario == 14u;
            troubled += k.trouble;
            const s64 spent = k.cpu.downcount - h.guest_cpu()->downcount;
            if (spent > most)
                most = spent;
        }
        printf("%08X %s: %u cases, %u identical (%llu by the general replay, %u at the budget or deadline edge, %u "
               "with the host not quiet or a boundary watched, at most %lld cycles), %u declined unchanged, 0 "
               "mismatches\n",
               entry, NAMES[which], cases, ran, bluewake_native_anim_general(which), edges, troubled, (long long)most,
               declined);
        fflush(stdout);
        if (ran < 30000u && cases >= 60000u)
            status = 1; /* at least 30,000 compared cases per function */
    }

    if (bench_calls != 0u) {
        CPUState* g = h.guest_cpu();
        reset_host();
        /* Representative cases: a spline between two keys; a search over 20
         * keys (three floats a key, or three shorts); a joint whose scale and
         * translation are one key each and whose rotation searches 20 keys a
         * channel (the common shape of a skeletal animation's joint); and one
         * whose nine channels all search. */
        static const char* const SHAPES[] = {"spline", "20 keys", "20 keys", "rotation keyed", "all keyed",
                                             "a rotation and a scale"};
        for (unsigned variant = 0; variant < 6u; ++variant) {
            const unsigned which = variant < 4u ? variant : variant == 4u ? 3u : 4u;
            const u32 entry = FUNCTIONS[which];
            seed = 0x2545F491u + variant;
            Case k = build(h.native_ram, which, 1u);
            CPUState* c = &k.cpu;
            c->exception = 0;
            c->fpscr = 0;
            c->msr = PPC_MSR_FP;
            c->hid2 = PPC_HID2_LSQE;
            c->gqr[0] = 0;
            c->gqr[5] = 7u << 16;
            c->reserve_valid = false;
            c->downcount = 0;
            c->cycle_deadline_budget = 0;
            c->cycle_budget = (s64)bench_calls * 4000 + 100000;
            u8* ram = h.native_ram;
            put64(ram, R2 - 13160u, MAGIC);
            switch (which) {
            case 0:
                c->fpr[1] = 2.5; c->fpr[2] = 2.0; c->fpr[3] = 10.0; c->fpr[4] = 0.5;
                c->fpr[5] = 4.0; c->fpr[6] = 12.0; c->fpr[7] = -0.25;
                break;
            case 1:
            case 2: {
                const bool shorts = which == 2u;
                for (unsigned i = 0; i < 20u; ++i)
                    for (unsigned j = 0; j < 3u; ++j) {
                        const s32 v = j == 0u ? (s32)(3u * i) : (s32)(i * 7u % 11u) - 5;
                        if (shorts)
                            put16(ram, KEYS + 2u * (3u * i + j), (u16)(s16)v);
                        else
                            put32(ram, KEYS + 4u * (3u * i + j), bits_of((f32)v * 0.5f));
                    }
                table(ram, TABLES, 20u, 0u, 0u);
                c->gpr[3] = TABLES;
                c->gpr[4] = KEYS;
                c->fpr[1] = shorts ? 31.5 : 15.25;
                break;
            }
            case 4: {
                static const f32 m[12] = {0.6f, -0.8f, 0.0f, 5.0f, 0.8f, 0.6f, 0.0f, -2.0f, 0.0f, 0.0f, 2.0f, 1.0f};
                for (unsigned i = 0; i < 12u; ++i)
                    put32(ram, MATRIX + 4u * i, bits_of(m[i]));
                c->gpr[3] = MATRIX;
                c->gpr[4] = MATRIX + 0x80u;
                break;
            }
            default: {
                const u32 joint = 3u, anm = TABLES;
                put32(ram, OBJECT + 40u, anm);
                put32(ram, OBJECT + 16u, KEYS);
                put32(ram, OBJECT + 20u, KEYS);
                put32(ram, OBJECT + 24u, KEYS);
                put32(ram, OBJECT + 36u, 0u);
                for (unsigned i = 0; i < 20u; ++i)
                    for (unsigned j = 0; j < 3u; ++j) {
                        const s32 v = j == 0u ? (s32)(3u * i) : (s32)(i * 7u % 11u) - 5;
                        put32(ram, KEYS + 4u * (3u * i + j), bits_of(j == 0u ? (f32)v : (f32)v * 0.5f));
                        put16(ram, KEYS + 0x1000u + 2u * (3u * i + j), (u16)(s16)(v * 100));
                    }
                static const u32 offsets[9] = {0u, 18u, 36u, 6u, 24u, 42u, 12u, 30u, 48u};
                for (unsigned ch = 0; ch < 9u; ++ch) {
                    const bool rotation = ch >= 3u && ch < 6u, searched = variant == 4u || (variant == 3u && rotation);
                    table(ram, anm + joint * 54u + offsets[ch], searched ? 20u : 1u,
                          (u16)(rotation ? 0x800u : 0u), 0u);
                }
                c->gpr[3] = OBJECT;
                c->gpr[4] = joint;
                c->gpr[5] = TRANSFORM;
                c->fpr[1] = 31.5;
                break;
            }
            }
            {
                CPUState probe = k.cpu;
                u8 saved[AREA_BYTES];
                memcpy(saved, ram + (AREA - GC_RAM_BASE), AREA_BYTES);
                const int ok = bluewake_native_anim(&probe, entry);
                memcpy(ram + (AREA - GC_RAM_BASE), saved, AREA_BYTES);
                if (!ok) {
                    fprintf(stderr, "the benchmark's case declines (%s)\n", SHAPES[variant]);
                    return 1;
                }
            }
            copy_regions(h.reference_ram, ram);
            double best_t = 1e30, best_n = 1e30;
            const CPUState base = k.cpu;
            s64 cycles = 0;
            for (unsigned round = 0; round < 5; ++round) {
                *g = base;
                g->ram = h.reference_ram;
                h.mod->on_state_loaded(g);
                double t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    g->gpr[1] = base.gpr[1];
                    g->gpr[3] = base.gpr[3];
                    g->gpr[4] = base.gpr[4];
                    g->gpr[5] = base.gpr[5];
                    g->fpr[1] = base.fpr[1];
                    g->lr = RETURN_ADDRESS;
                    g->pc = entry;
                    h.mod->dispatch(g, entry);
                }
                const double t = (now_ns() - t0) / bench_calls;
                cycles = (base.downcount - g->downcount) / (s64)bench_calls;
                CPUState native = base;
                t0 = now_ns();
                for (unsigned i = 0; i < bench_calls; ++i) {
                    native.gpr[1] = base.gpr[1];
                    native.gpr[3] = base.gpr[3];
                    native.gpr[4] = base.gpr[4];
                    native.gpr[5] = base.gpr[5];
                    native.fpr[1] = base.fpr[1];
                    native.lr = RETURN_ADDRESS;
                    if (!bluewake_native_anim(&native, entry)) {
                        fprintf(stderr, "the benchmark's native declined\n");
                        return 1;
                    }
                }
                const double n = (now_ns() - t0) / bench_calls;
                if (t < best_t) best_t = t;
                if (n < best_n) best_n = n;
            }
            printf("%08X %s, %s: translation %.1f ns/call through the dispatcher, native %.1f ns/call (%lld guest "
                   "cycles a call)\n",
                   entry, NAMES[which], SHAPES[variant], best_t, best_n, (long long)cycles);
        }
    }
    return status;
}
