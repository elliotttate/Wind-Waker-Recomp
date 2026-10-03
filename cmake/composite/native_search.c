/* The actor search by name (GZLE01 strcmp and dStage_searchName), native.
 *
 * fopAcM_searchFromName walks every actor, and for each one its judge,
 * fopAcM_findObjectCB, calls dStage_searchName(name) - the same name every
 * time - which strcmps it against each entry of the stage's object-name table
 * (l_objectName, 825 entries of 12 bytes at 0x80372818) until one matches. At
 * Dragon Roost Island daTag_Island_c looks up "ikada_h" (entry 383) every
 * frame: 7.4 percent of the game thread, strcmp 6.4 percent of it by its own
 * time and dStage_searchName 2.5 (2026-10-03). In the translation every one
 * of those strcmps is a call into another chunk (strcmp's, 0x8032D6E0) and a
 * return out of it.
 *
 * strcmp (the MSL's: a byte compare, then byte by byte to a word boundary
 * when both strings share their alignment, then word by word while the
 * 0xFEFEFEFF/0x80808080 test sees no zero byte in the first string, then byte
 * by byte again) runs its blocks in order on the same values: the loads, the
 * differences and record forms into CR0 (with XER's SO), the counter and
 * XER's CA where it aligns, and the cycles each block charges. It returns
 * what the translation returns - the byte difference, 0, or -1/+1 from the
 * word compare - and leaves r0, r3 to r8, CTR, CR0 and CA as the last
 * instruction to write each left it, pc at the return address. The one
 * suffix store on its prepaid paths (the aligning block's mtctr, 2) is made
 * where that block runs; its loads, lean in the prepaid copies
 * (scripts/windows/lean_memory.py), store none.
 *
 * dStage_searchName runs its frame (the back chain, the saved LR, r29 to r31
 * through the inline register save and restore), its loop and every strcmp
 * it calls, and returns the matching entry or NULL with the registers its
 * last strcmp and its epilogue leave: r0 and LR the saved return address, r4
 * to r8, CTR and CA from the strcmps, r11 the frame's top, CR0 equal (the
 * last compare is cmpwi r3,0 on a match, cmplwi r30,825 at the end), the
 * suffix 2 (its mtlr). An entry whose first byte differs from the name's -
 * almost all of them - costs the translation 15 cycles (the call block,
 * strcmp's first two blocks, the result test and the loop step); the native
 * charges the same without the call.
 *
 * Each declines, changing nothing, unless that is certain: no exception
 * pending, no aliases over MEM1, every load plain RAM, the turn's budget not
 * spent anywhere in the work, and no deadline inside it (the next deadline
 * at least 8 cycles out, beyond every block's suffix, and beyond the last
 * cycle, so the translation prepays every block and never leaves its copy).
 * dStage_searchName also needs its frame's five words in plain RAM, no write
 * journal, nothing it reads (the name, the table) under its own frame, and
 * its calls into strcmp's chunk and back to pass silently, as they do in
 * play: the module's edge filter on (direct_calls.h: the host handed over its
 * flags and the builder's watch list), the host's edge service quiet, and
 * neither strcmp's entry nor the return address (0x80041578) one the host
 * watches - then neither the direct call nor the chassis loop it falls back
 * to asks the host anything, and the result is the same however each call
 * is made.
 *
 * tests/native_search_test.c compares both with the translation, every
 * register and byte. No identifier here may be `ctx`. */
#include "native_search.h"
#include "direct_calls.h"

#include <stdio.h>

#define SEARCH_INLINE static inline __attribute__((always_inline))

int bluewake_native_search_enabled;

enum { SEARCH_STRCMP, SEARCH_STAGE_NAME, SEARCH_COUNT };
static unsigned long long s_search_runs[SEARCH_COUNT], s_search_declined[SEARCH_COUNT], s_search_entries;

void bluewake_native_search_report(void) {
    fprintf(stderr,
            "[native-search] strcmp=%llu/%llu stage-name=%llu/%llu (native/declined; %llu table entries "
            "compared natively)\n",
            s_search_runs[0], s_search_declined[0], s_search_runs[1], s_search_declined[1], s_search_entries);
}

#define SEARCH_TABLE 0x80372818u   /* l_objectName */
#define SEARCH_ENTRIES 0x339u      /* 825 */
#define SEARCH_ENTRY_BYTES 12u
#define SEARCH_RETURN 0x80041578u  /* dStage_searchName's return from strcmp */
/* Every block's suffix stays below this, and so does every block's length. */
#define SEARCH_DEADLINE_MIN 8

/* What one strcmp leaves: the registers it writes (r6 from its second block
 * on, r7 and r8 from its word loop on, CTR and CA where it aligns), its CR0,
 * and the cycles charged so far. */
typedef struct StrOut {
    u32 r0, r3, r4, r5, r6, r7, r8, ctr, ca;
    u32 cr0;
    u32 cycles;
    bool wrote6, wrote78, aligned;
    u32 end3, end4; /* one past the last byte read through each pointer */
} StrOut;

SEARCH_INLINE u32 str_cr0(bool less, bool greater, u32 so) {
    return (less ? 0x8u : greater ? 0x4u : 0x2u) | so;
}

SEARCH_INLINE bool str_byte(const u8* ram, u32 size, u32 address, u32* value) {
    const u32 offset = address - GC_RAM_BASE;
    if (offset >= size)
        return false;
    *value = ram[offset];
    return true;
}

SEARCH_INLINE bool str_word(const u8* ram, u32 size, u32 address, u32* value) {
    const u32 offset = address - GC_RAM_BASE;
    if (size < 4u || offset > size - 4u)
        return false;
    *value = read_be32(ram + offset);
    return true;
}

SEARCH_INLINE void str_reach(u32* end, u32 address, u32 bytes) {
    if (address + bytes > *end)
        *end = address + bytes;
}

/* strcmp(r3, r4) from its entry to its blr, charging from `cycles`: true
 * with o filled in; false where a load is not plain RAM or the cycles would
 * pass `limit`. o's end3 and end4 are only raised. */
SEARCH_INLINE bool str_run(const u8* ram, u32 size, u32 so, u32 limit, u32 r3, u32 r4, u32 cycles, StrOut* o) {
    u32 r0, r5, r6 = 0u, r7 = 0u, r8 = 0u, cr0;
    /* 8032DB44 (4): lbz r5,0(r3); lbz r0,0(r4); subf. r0,r0,r5; beq 8032DB5C */
    cycles += 4u;
    if (!str_byte(ram, size, r3, &r5) || !str_byte(ram, size, r4, &r0))
        return false;
    str_reach(&o->end3, r3, 1u);
    str_reach(&o->end4, r4, 1u);
    o->wrote6 = o->wrote78 = o->aligned = false;
    r0 = r5 - r0;
    cr0 = str_cr0((s32)r0 < 0, (s32)r0 > 0, so);
    if (r0 != 0u) {
        /* 8032DB54 (2): or r3,r0,r0; blr */
        cycles += 2u;
        r3 = r0;
        goto done;
    }
    /* 8032DB5C (4): rlwinm r0,r4,0,30,31; rlwinm r6,r3,0,30,31; cmplw r0,r6; bne 8032DC34 */
    cycles += 4u;
    r0 = r4 & 3u;
    r6 = r3 & 3u;
    o->wrote6 = true;
    cr0 = str_cr0(r0 < r6, r0 > r6, so);
    if (r0 != r6)
        goto byte_tail_test;
    /* 8032DB6C (2): cmplwi r6,0; beq 8032DBC8 */
    cycles += 2u;
    cr0 = str_cr0(false, r6 != 0u, so);
    if (r6 != 0u) {
        /* 8032DB74 (2): cmplwi r5,0; bne 8032DB84 */
        cycles += 2u;
        cr0 = str_cr0(false, r5 != 0u, so);
        if (r5 == 0u) {
            /* 8032DB7C (2): li r3,0; blr */
            cycles += 2u;
            r3 = 0u;
            goto done;
        }
        /* 8032DB84 (5): subfic r0,r6,3; mtctr r0; cmplwi r0,0; beq 8032DBC0 */
        cycles += 5u;
        {
            const u64 sum = 3ull + (u64)(u32)~r6 + 1ull;
            r0 = (u32)sum;
            o->ca = (u32)(sum >> 32) & 1u;
        }
        u32 ctr = r0;
        o->aligned = true;
        cr0 = str_cr0(false, r0 != 0u, so);
        if (r0 != 0u) {
            for (;;) {
                /* 8032DB94 (4): lbzu r5,1(r3); lbzu r0,1(r4); subf. r0,r0,r5; beq 8032DBAC */
                cycles += 4u;
                if (cycles > limit)
                    return false;
                r3 += 1u;
                r4 += 1u;
                if (!str_byte(ram, size, r3, &r5) || !str_byte(ram, size, r4, &r0))
                    return false;
                str_reach(&o->end3, r3, 1u);
                str_reach(&o->end4, r4, 1u);
                r0 = r5 - r0;
                cr0 = str_cr0((s32)r0 < 0, (s32)r0 > 0, so);
                if (r0 != 0u) {
                    /* 8032DBA4 (2): or r3,r0,r0; blr */
                    cycles += 2u;
                    r3 = r0;
                    o->ctr = ctr;
                    goto done;
                }
                /* 8032DBAC (2): cmplwi r5,0; bne 8032DBBC */
                cycles += 2u;
                cr0 = str_cr0(false, r5 != 0u, so);
                if (r5 == 0u) {
                    /* 8032DBB4 (2): li r3,0; blr */
                    cycles += 2u;
                    r3 = 0u;
                    o->ctr = ctr;
                    goto done;
                }
                /* 8032DBBC (1): bdnz 8032DB94 */
                cycles += 1u;
                if (--ctr == 0u)
                    break;
            }
        }
        o->ctr = ctr;
        /* 8032DBC0 (2): addi r3,r3,1; addi r4,r4,1 */
        cycles += 2u;
        r3 += 1u;
        r4 += 1u;
    }
    /* 8032DBC8 (8): lwz r7,0(r3); lis r5,0x8081; addi r6,r5,0x8080 (0x80808080); lwz r8,0(r4);
     * addis r5,r7,-257; addi r0,r5,-257; and. r0,r0,r6; bne 8032DC1C */
    cycles += 8u;
    if (!str_word(ram, size, r3, &r7) || !str_word(ram, size, r4, &r8))
        return false;
    str_reach(&o->end3, r3, 4u);
    str_reach(&o->end4, r4, 4u);
    o->wrote78 = true;
    r6 = 0x80808080u;
    r5 = r7 + 0xFEFF0000u;
    r0 = (r5 + 0xFFFFFEFFu) & r6;
    cr0 = str_cr0((s32)r0 < 0, (s32)r0 > 0, so);
    if (r0 == 0u) {
        /* 8032DBE8 (1): b 8032DC04 */
        cycles += 1u;
        for (;;) {
            /* 8032DC04 (2): cmplw r7,r8; beq 8032DBEC */
            cycles += 2u;
            cr0 = str_cr0(r7 < r8, r7 > r8, so);
            if (r7 != r8) {
                /* 8032DC0C (2): li r3,-1; bnelr- (returns unless greater) */
                cycles += 2u;
                r3 = 0xFFFFFFFFu;
                if (r7 > r8) {
                    /* 8032DC14 (2): li r3,1; blr */
                    cycles += 2u;
                    r3 = 1u;
                }
                goto done;
            }
            /* 8032DBEC (6): lwzu r7,4(r3); lwzu r8,4(r4); addis r5,r7,-257; addi r0,r5,-257;
             * and. r0,r0,r6; bne 8032DC1C */
            cycles += 6u;
            if (cycles > limit)
                return false;
            r3 += 4u;
            r4 += 4u;
            if (!str_word(ram, size, r3, &r7) || !str_word(ram, size, r4, &r8))
                return false;
            str_reach(&o->end3, r3, 4u);
            str_reach(&o->end4, r4, 4u);
            r5 = r7 + 0xFEFF0000u;
            r0 = (r5 + 0xFFFFFEFFu) & r6;
            cr0 = str_cr0((s32)r0 < 0, (s32)r0 > 0, so);
            if (r0 != 0u)
                break;
        }
    }
    /* 8032DC1C (4): lbz r5,0(r3); lbz r0,0(r4); subf. r0,r0,r5; beq 8032DC34 */
    cycles += 4u;
    if (!str_byte(ram, size, r3, &r5) || !str_byte(ram, size, r4, &r0))
        return false;
    r0 = r5 - r0;
    cr0 = str_cr0((s32)r0 < 0, (s32)r0 > 0, so);
    if (r0 != 0u) {
        /* 8032DC2C (2): or r3,r0,r0; blr */
        cycles += 2u;
        r3 = r0;
        goto done;
    }
byte_tail_test:
    /* 8032DC34 (2): cmplwi r5,0; bne 8032DC44 */
    cycles += 2u;
    cr0 = str_cr0(false, r5 != 0u, so);
    if (r5 == 0u) {
        /* 8032DC3C (2): li r3,0; blr */
        cycles += 2u;
        r3 = 0u;
        goto done;
    }
    for (;;) {
        /* 8032DC44 (4): lbzu r5,1(r3); lbzu r0,1(r4); subf. r0,r0,r5; beq 8032DC5C */
        cycles += 4u;
        if (cycles > limit)
            return false;
        r3 += 1u;
        r4 += 1u;
        if (!str_byte(ram, size, r3, &r5) || !str_byte(ram, size, r4, &r0))
            return false;
        str_reach(&o->end3, r3, 1u);
        str_reach(&o->end4, r4, 1u);
        r0 = r5 - r0;
        cr0 = str_cr0((s32)r0 < 0, (s32)r0 > 0, so);
        if (r0 != 0u) {
            /* 8032DC54 (2): or r3,r0,r0; blr */
            cycles += 2u;
            r3 = r0;
            goto done;
        }
        /* 8032DC5C (2): cmplwi r5,0; bne 8032DC44 */
        cycles += 2u;
        cr0 = str_cr0(false, r5 != 0u, so);
        if (r5 == 0u)
            break;
    }
    /* 8032DC64 (2): li r3,0; blr */
    cycles += 2u;
    r3 = 0u;
done:
    o->r0 = r0;
    o->r3 = r3;
    o->r4 = r4;
    o->r5 = r5;
    o->r6 = r6;
    o->r7 = r7;
    o->r8 = r8;
    o->cr0 = cr0;
    o->cycles = cycles;
    return cycles <= limit;
}

/* Ready for any of these: nothing pending, no aliases, a running budget, and
 * no deadline nearer than every block's suffix and length. */
SEARCH_INLINE bool search_ready(const CPUState* cpu) {
    return cpu->exception == 0u && !g_ppc_guest_aliases_overlap_mem1 && cpu->cycle_budget > 0 &&
           (cpu->cycle_deadline_budget <= 0 || cpu->cycle_deadline_budget >= SEARCH_DEADLINE_MIN);
}

/* The most cycles the work may charge: every block entry (and every budget
 * check between) still above the budget, and the last cycle no later than
 * the deadline; 0 where nothing fits, at most 2^31 - 1. */
SEARCH_INLINE u32 search_limit(const CPUState* cpu) {
    s64 limit = cpu->downcount + cpu->cycle_budget - 1;
    if (cpu->cycle_deadline_budget > 0 && cpu->downcount + cpu->cycle_deadline_budget < limit)
        limit = cpu->downcount + cpu->cycle_deadline_budget;
    return limit <= 0 ? 0u : limit > 0x7FFFFFFF ? 0x7FFFFFFFu : (u32)limit;
}

/* strcmp(r3, r4), entered with the return address in LR. */
static int search_strcmp(CPUState* cpu) {
    if (!search_ready(cpu))
        return 0;
    StrOut o;
    o.end3 = o.end4 = 0u;
    if (!str_run(cpu->ram, cpu->ram_size, cpu->xer >> 31, search_limit(cpu), cpu->gpr[3], cpu->gpr[4], 0u, &o))
        return 0;
    cpu->gpr[0] = o.r0;
    cpu->gpr[3] = o.r3;
    cpu->gpr[4] = o.r4;
    cpu->gpr[5] = o.r5;
    if (o.wrote6)
        cpu->gpr[6] = o.r6;
    if (o.wrote78) {
        cpu->gpr[7] = o.r7;
        cpu->gpr[8] = o.r8;
    }
    if (o.aligned) {
        cpu->ctr = o.ctr;
        cpu->xer = (cpu->xer & ~0x20000000u) | o.ca << 29;
        cpu->cycle_observation_suffix = 2u;
    }
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (o.cr0 << 28);
    cpu->downcount -= (s64)o.cycles;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

/* A boundary inside the work passes without the host: the edge filter's own
 * test (dispatch_loop.h), which a direct call's ready test only narrows. */
SEARCH_INLINE bool search_boundary_silent(const CPUState* cpu, u32 address) {
    return bw_edge_filter_enabled && bw_edge_watch_ready && bw_host_quiet(cpu) && bw_edge_unwatched(address);
}

SEARCH_INLINE bool search_apart(u32 a, u32 a_end, u32 b, u32 b_end) {
    return a_end <= b || b_end <= a;
}

/* dStage_searchName(r3 name), entered with the return address in LR. */
static int search_stage_name(CPUState* cpu) {
    if (!search_ready(cpu) || g_mem_write_journal != NULL)
        return 0;
    const u32 limit = search_limit(cpu);
    if (limit < 40u)
        return 0;
    if (!search_boundary_silent(cpu, BLUEWAKE_SEARCH_STRCMP) || !search_boundary_silent(cpu, SEARCH_RETURN))
        return 0;
    const u8* ram = cpu->ram;
    const u32 size = cpu->ram_size;
    const u32 sp = cpu->gpr[1];
    /* The frame's words: the back chain (sp-32), the saved LR (sp+4) and
     * r29 to r31 (sp-12 to sp-4), stored and read back as plain RAM. */
    const u32 frame = sp - 32u;
    if (size < 8u || sp - GC_RAM_BASE > size - 8u || frame - GC_RAM_BASE > size - 4u || frame > sp)
        return 0;
    if (SEARCH_TABLE - GC_RAM_BASE > size - SEARCH_ENTRIES * SEARCH_ENTRY_BYTES)
        return 0;
    const u32 so = cpu->xer >> 31;
    const u32 name = cpu->gpr[3];
    u32 first;
    if (!str_byte(ram, size, name, &first))
        return 0;

    /* What the last strcmp left, and the sticky registers from the last one
     * to write them. */
    StrOut o;
    o.end3 = SEARCH_TABLE + 1u;
    o.end4 = name + 1u;
    u32 r4 = name, r5 = 0u;
    u32 r6 = 0u, r7 = 0u, r8 = 0u, ctr = 0u, ca = 0u;
    bool wrote6 = false, wrote78 = false, aligned = false;
    /* 80041544 (5) and the inline _savegpr_29 (4); 80041558 (5) */
    u32 cycles = 14u, result = 0u, index = 0u;
    const u8* entry_bytes = ram + (SEARCH_TABLE - GC_RAM_BASE);
    for (;; ++index) {
        /* The entries whose first byte differs from the name's: for each,
         * 8004156C (3), strcmp's 8032DB44 (4) and 8032DB54 (2), 80041578 (2)
         * and 80041588 (4); strcmp leaves r5 the entry's byte, r4 the name. */
        const u32 start = index;
        while (index < SEARCH_ENTRIES && entry_bytes[index * SEARCH_ENTRY_BYTES] != first)
            index++;
        if (index != start) {
            cycles += 15u * (index - start);
            r4 = name;
            r5 = entry_bytes[(index - 1u) * SEARCH_ENTRY_BYTES];
        }
        if (index == SEARCH_ENTRIES)
            break;
        const u32 entry = SEARCH_TABLE + index * SEARCH_ENTRY_BYTES;
        if (!str_run(ram, size, so, limit, entry, name, cycles + 3u, &o))
            return 0;
        r4 = o.r4;
        r5 = o.r5;
        if (o.wrote6) {
            r6 = o.r6;
            wrote6 = true;
        }
        if (o.wrote78) {
            r7 = o.r7;
            r8 = o.r8;
            wrote78 = true;
        }
        if (o.aligned) {
            ctr = o.ctr;
            ca = o.ca;
            aligned = true;
        }
        cycles = o.cycles + 2u;
        if (o.r3 == 0u) {
            /* 80041580 (2): or r3,r31,r31; b 8004159C */
            cycles += 2u;
            result = entry;
            break;
        }
        cycles += 4u;
    }
    if (index == SEARCH_ENTRIES) {
        /* 80041598 (1): li r3,0 */
        cycles += 1u;
        str_reach(&o.end3, SEARCH_TABLE + (SEARCH_ENTRIES - 1u) * SEARCH_ENTRY_BYTES, 1u);
    } else {
        str_reach(&o.end3, SEARCH_TABLE + index * SEARCH_ENTRY_BYTES, 1u);
    }
    /* 8004159C (2), the inline _restgpr_29 (4), 800415A4 (5) */
    cycles += 11u;
    if (cycles > limit)
        return 0;
    /* Nothing read lies under the frame's stores, which come first. */
    if (!search_apart(SEARCH_TABLE, o.end3, frame, sp + 8u) || !search_apart(name, o.end4, frame, sp + 8u))
        return 0;

    /* stwu r1,-32(r1); stw r0,36(r1) (the LR); the inline save of r29 to r31. */
    u8* bytes = cpu->ram;
    const u32 lr = cpu->lr;
    clear_matching_reservation(cpu, frame);
    write_be32(bytes + (frame - GC_RAM_BASE), sp);
    clear_matching_reservation(cpu, sp + 4u);
    write_be32(bytes + (sp + 4u - GC_RAM_BASE), lr);
    clear_matching_reservation(cpu, sp - 12u);
    write_be32(bytes + (sp - 12u - GC_RAM_BASE), cpu->gpr[29]);
    clear_matching_reservation(cpu, sp - 8u);
    write_be32(bytes + (sp - 8u - GC_RAM_BASE), cpu->gpr[30]);
    clear_matching_reservation(cpu, sp - 4u);
    write_be32(bytes + (sp - 4u - GC_RAM_BASE), cpu->gpr[31]);
    cpu->gpr[0] = lr;
    cpu->gpr[3] = result;
    cpu->gpr[4] = r4;
    cpu->gpr[5] = r5;
    if (wrote6)
        cpu->gpr[6] = r6;
    if (wrote78) {
        cpu->gpr[7] = r7;
        cpu->gpr[8] = r8;
    }
    if (aligned) {
        cpu->ctr = ctr;
        cpu->xer = (cpu->xer & ~0x20000000u) | ca << 29;
    }
    cpu->gpr[11] = sp;
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | ((0x2u | so) << 28);
    cpu->downcount -= (s64)cycles;
    cpu->cycle_observation_suffix = 2u;
    cpu->pc = lr & ~3u;
    s_search_entries += index + (index < SEARCH_ENTRIES);
    return 1;
}

int bluewake_native_search(CPUState* cpu, u32 address) {
    unsigned which;
    int done;
    switch (address) {
    case BLUEWAKE_SEARCH_STRCMP: which = SEARCH_STRCMP; done = search_strcmp(cpu); break;
    case BLUEWAKE_SEARCH_STAGE_NAME: which = SEARCH_STAGE_NAME; done = search_stage_name(cpu); break;
    default: return 0;
    }
    if (done)
        s_search_runs[which]++;
    else
        s_search_declined[which]++;
    return done;
}
