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

enum { SEARCH_STRCMP, SEARCH_STAGE_NAME, SEARCH_JUDGE, SEARCH_COUNT };
static unsigned long long s_search_runs[SEARCH_COUNT], s_search_declined[SEARCH_COUNT], s_search_entries;
static unsigned long long s_judge_other; /* JudgeFilter calls with another judge */

void bluewake_native_search_report(void) {
    fprintf(stderr,
            "[native-search] strcmp=%llu/%llu stage-name=%llu/%llu judge-filter=%llu/%llu (native/declined; "
            "%llu JudgeFilter calls with another judge among the declined; %llu table entries compared "
            "natively)\n",
            s_search_runs[0], s_search_declined[0], s_search_runs[1], s_search_declined[1], s_search_runs[2],
            s_search_declined[2], s_judge_other, s_search_entries);
    bluewake_native_search_judge_report();
}

#define SEARCH_TABLE 0x80372818u   /* l_objectName */
#define SEARCH_ENTRIES 0x339u      /* 825 */
#define SEARCH_ENTRY_BYTES 12u
#define SEARCH_RETURN 0x80041578u  /* dStage_searchName's return from strcmp */
/* Every block's suffix stays below this, and so does every block's length. */
#define SEARCH_DEADLINE_MIN 8

static int search_judge_filter(CPUState* cpu); /* the judge's section, below */

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

/* What dStage_searchName(name) leaves, run from its entry to its blr with
 * its frame at sp - 32: the result, the registers its last strcmp (or the
 * last to write each) leaves, the cycles, and how far it read the table and
 * the name. */
typedef struct SearchName {
    u32 result, r4, r5, r6, r7, r8, ctr, ca;
    bool wrote6, wrote78, aligned;
    u32 cycles;
    u32 end3, end4; /* one past the last byte read of the table and of the name */
    u32 entries;    /* the table entries it compared */
} SearchName;

/* dStage_searchName(name) computed: false where a load is not plain RAM or
 * the cycles would pass `limit`. Nothing is written. */
static bool search_name_run(const u8* ram, u32 size, u32 so, u32 limit, u32 name, SearchName* n) {
    if (SEARCH_TABLE - GC_RAM_BASE > size - SEARCH_ENTRIES * SEARCH_ENTRY_BYTES)
        return false;
    u32 first;
    if (!str_byte(ram, size, name, &first))
        return false;
    StrOut o;
    o.end3 = SEARCH_TABLE + 1u;
    o.end4 = name + 1u;
    n->r4 = name;
    n->r5 = 0u;
    n->wrote6 = n->wrote78 = n->aligned = false;
    n->r6 = n->r7 = n->r8 = n->ctr = n->ca = 0u;
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
            n->r4 = name;
            n->r5 = entry_bytes[(index - 1u) * SEARCH_ENTRY_BYTES];
        }
        if (index == SEARCH_ENTRIES)
            break;
        const u32 entry = SEARCH_TABLE + index * SEARCH_ENTRY_BYTES;
        if (!str_run(ram, size, so, limit, entry, name, cycles + 3u, &o))
            return false;
        n->r4 = o.r4;
        n->r5 = o.r5;
        if (o.wrote6) {
            n->r6 = o.r6;
            n->wrote6 = true;
        }
        if (o.wrote78) {
            n->r7 = o.r7;
            n->r8 = o.r8;
            n->wrote78 = true;
        }
        if (o.aligned) {
            n->ctr = o.ctr;
            n->ca = o.ca;
            n->aligned = true;
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
    n->result = result;
    n->cycles = cycles;
    n->end3 = o.end3;
    n->end4 = o.end4;
    n->entries = index + (index < SEARCH_ENTRIES);
    return cycles <= limit;
}

/* The registers dStage_searchName leaves (all but r0, r1, r3, r11, r29 to
 * r31, LR, CR0 and the suffix, which its caller sets as it leaves them). */
SEARCH_INLINE void search_name_registers(CPUState* cpu, const SearchName* n) {
    cpu->gpr[4] = n->r4;
    cpu->gpr[5] = n->r5;
    if (n->wrote6)
        cpu->gpr[6] = n->r6;
    if (n->wrote78) {
        cpu->gpr[7] = n->r7;
        cpu->gpr[8] = n->r8;
    }
    if (n->aligned) {
        cpu->ctr = n->ctr;
        cpu->xer = (cpu->xer & ~0x20000000u) | n->ca << 29;
    }
}

/* A store of the work's: the reservation cleared as the translation's store
 * clears it. The address is plain RAM. */
SEARCH_INLINE void search_store(CPUState* cpu, u32 address, u32 value) {
    clear_matching_reservation(cpu, address);
    write_be32(cpu->ram + (address - GC_RAM_BASE), value);
}

/* dStage_searchName's frame, stored as its prologue stores it: the back
 * chain, the saved LR, r29 to r31 (sp its caller's r1). */
SEARCH_INLINE void search_name_frame(CPUState* cpu, u32 sp, u32 lr, u32 r29, u32 r30, u32 r31) {
    search_store(cpu, sp - 32u, sp);
    search_store(cpu, sp + 4u, lr);
    search_store(cpu, sp - 12u, r29);
    search_store(cpu, sp - 8u, r30);
    search_store(cpu, sp - 4u, r31);
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
    const u32 size = cpu->ram_size;
    const u32 sp = cpu->gpr[1];
    /* The frame's words: the back chain (sp-32), the saved LR (sp+4) and
     * r29 to r31 (sp-12 to sp-4), stored and read back as plain RAM. */
    const u32 frame = sp - 32u;
    if (size < 8u || sp - GC_RAM_BASE > size - 8u || frame - GC_RAM_BASE > size - 4u || frame > sp)
        return 0;
    const u32 so = cpu->xer >> 31;
    const u32 name = cpu->gpr[3];
    SearchName n;
    if (!search_name_run(cpu->ram, size, so, limit, name, &n))
        return 0;
    /* Nothing read lies under the frame's stores, which come first. */
    if (!search_apart(SEARCH_TABLE, n.end3, frame, sp + 8u) || !search_apart(name, n.end4, frame, sp + 8u))
        return 0;

    const u32 lr = cpu->lr;
    search_name_frame(cpu, sp, lr, cpu->gpr[29], cpu->gpr[30], cpu->gpr[31]);
    search_name_registers(cpu, &n);
    cpu->gpr[0] = lr;
    cpu->gpr[3] = n.result;
    cpu->gpr[11] = sp;
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | ((0x2u | so) << 28);
    cpu->downcount -= (s64)n.cycles;
    cpu->cycle_observation_suffix = 2u;
    cpu->pc = lr & ~3u;
    s_search_entries += n.entries;
    return 1;
}

int bluewake_native_search(CPUState* cpu, u32 address) {
    unsigned which;
    int done;
    switch (address) {
    case BLUEWAKE_SEARCH_STRCMP: which = SEARCH_STRCMP; done = search_strcmp(cpu); break;
    case BLUEWAKE_SEARCH_STAGE_NAME: which = SEARCH_STAGE_NAME; done = search_stage_name(cpu); break;
    case BLUEWAKE_SEARCH_JUDGE_FILTER: which = SEARCH_JUDGE; done = search_judge_filter(cpu); break;
    default: return 0;
    }
    if (done)
        s_search_runs[which]++;
    else
        s_search_declined[which]++;
    return done;
}

/* ---- The judge: cTgIt_JudgeFilter with fopAcM_findObjectCB, and the walk batched ----
 *
 * fopAcM_searchFromName's walk (fopAcIt_Judge -> cLsIt_Judge -> cNdIt_Judge)
 * calls cTgIt_JudgeFilter for every actor through a bctrl whose target and
 * return the host names (0x80245640, 0x80244F88): each node goes back to the
 * chassis loop and the host's edge service once, then into JudgeFilter's
 * chunk, then through an indirect call into fopAcM_findObjectCB's chunk, a
 * direct call into dStage_searchName's, and its strcmps. Two natives here:
 *
 * cTgIt_JudgeFilter (0x80245640, hooked at its entry like the others) when
 * the filter's judge is fopAcM_findObjectCB: one node, from JudgeFilter's
 * entry to its blr - JudgeFilter's frame and loads, findObjectCB's frame (r31
 * and r30 saved, the LR), dStage_searchName (the native above) with its frame
 * (r30 the actor), findObjectCB's tests (the entry's procname and argument
 * against the actor's, the parameter under the mask) and both epilogues. It
 * returns the actor or NULL, with r0 and LR the return address, r4 what the
 * last test loaded (dStage_searchName's r4 when the name is not in the table,
 * the entry's procname, or the mask), r5 to r8, CA and the counter from the
 * strcmps (else CTR the judge), r11 dStage_searchName's frame top, r12 the
 * judge, CR0 from the last test, the suffix 2 (JudgeFilter's mtlr). Its
 * return to cNdIt_Judge (0x80244F88) is in the same chunk, so the walk goes
 * on in the translation as before.
 *
 * The walk batched (bluewake_native_search_judge, for the host's edge
 * service at the boundary into JudgeFilter, where it finds nothing to do -
 * as host_actor_search_native in runtime/host/src/main.c batches the walk
 * whose judge is fpcSch_JudgeByID): entered at that boundary (pc 0x80245640,
 * LR 0x80244F88, r29 and CTR the filter function, r30 = r4 the judge_filter,
 * r3 the node, r31 the next node), it runs whole iterations whose judge
 * answers NULL - the call above, then cNdIt_Judge's NULL test, its step to
 * the next node and its call block - and hands back the same boundary one or
 * more nodes later, with the state the translated blocks leave there: r0 and
 * LR 0x80244F88, r3 the node, r4 the filter, r5 to r8 and CA from the
 * strcmps, CTR and r12 the filter function, r11 dStage_searchName's frame
 * top, r31 the node after, CR0 greater (the loop test on a node), the suffix
 * 1 (the call block's mtctr), the cycles of every block, and the eleven stack
 * words the last iteration stored. The name and the table are the same at
 * every node, and nothing the walk stores lies under them, so
 * dStage_searchName runs once for the whole batch. The node the judge
 * matches, the list's last node, and a node any of whose loads is not plain
 * RAM or lies under the stores are left to the translated code, which the
 * host then runs from this boundary. The boundaries it skips are that same
 * one at later nodes (where the host's service would again have nothing to
 * do: it is the host's own call, as for host_actor_search_native), the
 * in-chunk return to 0x80244F88, and the silent calls below. It runs only in
 * a module whose JudgeFilter hook is certified and has run
 * (bluewake_native_search_judge_ready), so the translation it stands in for
 * is the one tests/native_search_judge_test.c compared it with.
 *
 * Both run a node only when every block would take its prepaid path (no
 * deadline before the work's last cycle or nearer than 10, beyond every
 * suffix) and no budget check would stop the guest - each block's leader,
 * the direct calls' ready tests, the return dispatches, the loop's back edge
 * and, for the batch, the chassis check at the next boundary - and every call
 * inside passes silently (findObjectCB's entry and return, the call into
 * dStage_searchName and its return, strcmp's entry and return). They decline
 * (nothing changed) on anything pending, a write journal, aliases over MEM1,
 * a judge other than fopAcM_findObjectCB, a NULL search parameter, or a load
 * under the frames' stores. */
#define SEARCH_JUDGE_FILTER BLUEWAKE_SEARCH_JUDGE_FILTER /* cTgIt_JudgeFilter */
#define SEARCH_FIND_OBJECT 0x8002833Cu                     /* fopAcM_findObjectCB */
#define SEARCH_NDIT_RETURN 0x80244F88u                     /* cNdIt_Judge, after its bctrl */
#define SEARCH_FILTER_RETURN 0x80245664u                   /* cTgIt_JudgeFilter, after its bctrl */
#define SEARCH_FIND_RETURN 0x80028394u                     /* fopAcM_findObjectCB, after its bl */
/* JudgeFilter's first store has the suffix 9; every block is shorter than 11. */
#define SEARCH_JUDGE_DEADLINE_MIN 10
/* One call: JudgeFilter's entry (10), findObjectCB's (8), its call block (2),
 * the result test (2), its epilogue (7) and JudgeFilter's (5); the tests and
 * dStage_searchName come on top. */
#define SEARCH_JUDGE_CALL 34u
/* cNdIt_Judge's NULL test (2), node step (3), next load (2), loop test (2)
 * and call block (5). */
#define SEARCH_JUDGE_STEP 14u

int bluewake_native_search_judge_ready;
static unsigned long long s_judge_runs, s_judge_nodes, s_judge_declined;

/* The walk's counts, where the host has called it (the JudgeFilter entry's
 * are in bluewake_native_search_report's line). */
void bluewake_native_search_judge_report(void) {
    if (s_judge_runs + s_judge_declined != 0u)
        fprintf(stderr, "[native-search] judge walk=%llu/%llu (runs/declined; %llu nodes)\n", s_judge_runs,
                s_judge_declined, s_judge_nodes);
}

/* Not under the stores [lo, hi). */
SEARCH_INLINE bool judge_clear(u32 address, u32 bytes, u32 lo, u32 hi) {
    return (u64)address + bytes <= lo || hi <= address;
}

SEARCH_INLINE bool judge_word(const u8* ram, u32 size, u32 address, u32 lo, u32 hi, u32* value) {
    return judge_clear(address, 4u, lo, hi) && str_word(ram, size, address, value);
}

SEARCH_INLINE bool judge_half(const u8* ram, u32 size, u32 address, u32 lo, u32 hi, u32* value) {
    const u32 offset = address - GC_RAM_BASE;
    if (!judge_clear(address, 2u, lo, hi) || size < 2u || offset > size - 2u)
        return false;
    *value = read_be16(ram + offset);
    return true;
}

SEARCH_INLINE bool judge_byte(const u8* ram, u32 size, u32 address, u32 lo, u32 hi, u32* value) {
    return judge_clear(address, 1u, lo, hi) && str_byte(ram, size, address, value);
}

/* What one call needs that is the same at every node: the search parameter,
 * dStage_searchName's run and its entry's procname and argument. */
typedef struct JudgeSearch {
    u32 filter, judge, prm, name, mask, param;
    u32 entry_procname, entry_argument;
    u32 lo, hi; /* the stores: sp-64 to sp+8 */
    SearchName n;
} JudgeSearch;

/* What the call leaves that depends on the node. */
typedef struct JudgeCall {
    u32 actor, result, r4, cr0, cycles;
} JudgeCall;

SEARCH_INLINE bool judge_ready(const CPUState* cpu) {
    return search_ready(cpu) && g_mem_write_journal == NULL &&
           (cpu->cycle_deadline_budget <= 0 || cpu->cycle_deadline_budget >= SEARCH_JUDGE_DEADLINE_MIN);
}

/* The search's constants, for JudgeFilter entered with r4 the filter and
 * sp its r1: false where the call must not run natively. */
static bool judge_search(const CPUState* cpu, u32 limit, JudgeSearch* s) {
    const u8* ram = cpu->ram;
    const u32 size = cpu->ram_size;
    const u32 sp = cpu->gpr[1];
    /* The judge first: JudgeFilter runs for every kind of search. */
    s->filter = cpu->gpr[4];
    if (!str_word(ram, size, s->filter, &s->judge) || s->judge != SEARCH_FIND_OBJECT)
        return false;
    static const u32 calls[] = {SEARCH_FIND_OBJECT, SEARCH_FILTER_RETURN, BLUEWAKE_SEARCH_STAGE_NAME,
                                SEARCH_FIND_RETURN, BLUEWAKE_SEARCH_STRCMP, SEARCH_RETURN};
    for (unsigned i = 0; i < sizeof calls / sizeof calls[0]; ++i)
        if (!search_boundary_silent(cpu, calls[i]))
            return false;
    /* The words a call stores: JudgeFilter's frame (sp-16, sp+4),
     * findObjectCB's (sp-32, sp-24 to sp-12) and dStage_searchName's (sp-64,
     * sp-44 to sp-28), all in plain RAM. */
    if (size < 72u || sp - GC_RAM_BASE < 64u || sp - GC_RAM_BASE > size - 8u)
        return false;
    s->lo = sp - 64u;
    s->hi = sp + 8u;
    if (!judge_clear(s->filter, 4u, s->lo, s->hi) ||
        !judge_word(ram, size, s->filter + 4u, s->lo, s->hi, &s->prm) || s->prm == 0u)
        return false;
    if (!judge_word(ram, size, s->prm, s->lo, s->hi, &s->name) ||
        !judge_word(ram, size, s->prm + 4u, s->lo, s->hi, &s->mask) ||
        !judge_word(ram, size, s->prm + 8u, s->lo, s->hi, &s->param))
        return false;
    if (!search_name_run(ram, size, cpu->xer >> 31, limit, s->name, &s->n) ||
        !judge_clear(SEARCH_TABLE, s->n.end3 - SEARCH_TABLE, s->lo, s->hi) ||
        !judge_clear(s->name, s->n.end4 - s->name, s->lo, s->hi))
        return false;
    s->entry_procname = s->entry_argument = 0u;
    if (s->n.result != 0u && (!judge_half(ram, size, s->n.result + 8u, s->lo, s->hi, &s->entry_procname) ||
                              !judge_byte(ram, size, s->n.result + 10u, s->lo, s->hi, &s->entry_argument)))
        return false;
    return true;
}

SEARCH_INLINE u32 judge_signed_cr0(s32 a, s32 b, u32 so) { return str_cr0(a < b, a > b, so); }

/* JudgeFilter on `node`: the actor, findObjectCB's tests on it and the
 * call's cycles; false where a load is not plain RAM or under the stores. */
SEARCH_INLINE bool judge_call(const CPUState* cpu, const JudgeSearch* s, u32 node, JudgeCall* c) {
    const u8* ram = cpu->ram;
    const u32 size = cpu->ram_size, so = cpu->xer >> 31;
    if (!judge_word(ram, size, node + 12u, s->lo, s->hi, &c->actor))
        return false;
    const u32 actor = c->actor;
    c->result = 0u;
    if (s->n.result == 0u) {
        /* 80028394 (2): cmplwi r3,0 (equal); 8002839C (2): li r3,0; b 800283F8 */
        c->r4 = s->n.r4;
        c->cr0 = 0x2u | so;
        c->cycles = SEARCH_JUDGE_CALL + s->n.cycles + 2u;
        return true;
    }
    /* 800283A4 (4): lha r4,8(r3); lha r0,14(r30); cmpw r4,r0; bne 800283F4 */
    u32 procname;
    if (!judge_half(ram, size, actor + 14u, s->lo, s->hi, &procname))
        return false;
    c->r4 = (u32)(s32)(s16)s->entry_procname;
    c->cr0 = judge_signed_cr0((s16)s->entry_procname, (s16)procname, so);
    if (procname != s->entry_procname) {
        /* 800283F4 (1): li r3,0 */
        c->cycles = SEARCH_JUDGE_CALL + s->n.cycles + 5u;
        return true;
    }
    /* 800283B4 (6): lbz r0,10(r3); extsb r3,r0; lbz r0,449(r30); extsb r0,r0; cmpw r3,r0; bne 800283F4 */
    u32 argument;
    if (!judge_byte(ram, size, actor + 449u, s->lo, s->hi, &argument))
        return false;
    c->cr0 = judge_signed_cr0((s8)s->entry_argument, (s8)argument, so);
    if (argument != s->entry_argument) {
        c->cycles = SEARCH_JUDGE_CALL + s->n.cycles + 11u;
        return true;
    }
    /* 800283CC (3): lwz r4,4(r31); cmplwi r4,0; beq 800283EC */
    c->r4 = s->mask;
    c->cr0 = str_cr0(false, s->mask != 0u, so);
    if (s->mask == 0u) {
        /* 800283EC (2): or r3,r30,r30; b 800283F8 */
        c->result = actor;
        c->cycles = SEARCH_JUDGE_CALL + s->n.cycles + 15u;
        return true;
    }
    /* 800283D8 (5): lwz r3,8(r31); lwz r0,176(r30); and r0,r4,r0; cmplw r3,r0; bne 800283F4 */
    u32 actor_param;
    if (!judge_word(ram, size, actor + 176u, s->lo, s->hi, &actor_param))
        return false;
    const u32 masked = s->mask & actor_param;
    c->cr0 = str_cr0(s->param < masked, s->param > masked, so);
    if (s->param == masked) {
        c->result = actor;
        c->cycles = SEARCH_JUDGE_CALL + s->n.cycles + 20u;
    } else {
        c->cycles = SEARCH_JUDGE_CALL + s->n.cycles + 19u;
    }
    return true;
}

/* One call's stores, in its order: JudgeFilter's stwu and saved LR,
 * findObjectCB's stwu, saved LR, r31 and r30, dStage_searchName's frame (r29,
 * r30 the actor, r31 the search parameter). */
static void judge_stores(CPUState* cpu, const JudgeSearch* s, u32 lr, u32 r29, u32 r30, u32 r31, u32 actor) {
    const u32 sp = cpu->gpr[1];
    search_store(cpu, sp - 16u, sp);
    search_store(cpu, sp + 4u, lr);
    search_store(cpu, sp - 32u, sp - 16u);
    search_store(cpu, sp - 12u, SEARCH_FILTER_RETURN);
    search_store(cpu, sp - 20u, r31);
    search_store(cpu, sp - 24u, r30);
    search_name_frame(cpu, sp - 32u, SEARCH_FIND_RETURN, r29, actor, s->prm);
}

/* cTgIt_JudgeFilter(r3 node, r4 filter), entered with the return address in
 * LR, when the filter's judge is fopAcM_findObjectCB. */
static int search_judge_filter(CPUState* cpu) {
    /* The hook is in the chunk only where the translation is the certified
     * one: the walk may be batched from now on. */
    bluewake_native_search_judge_ready = 1;
    if (!judge_ready(cpu))
        return 0;
    const u32 limit = search_limit(cpu);
    JudgeSearch s;
    JudgeCall c;
    s.judge = 0u;
    if (limit < SEARCH_JUDGE_CALL || !judge_search(cpu, limit, &s) || !judge_call(cpu, &s, cpu->gpr[3], &c) ||
        c.cycles > limit) {
        s_judge_other += s.judge != SEARCH_FIND_OBJECT; /* another kind of search */
        return 0;
    }
    const u32 lr = cpu->lr;
    judge_stores(cpu, &s, lr, cpu->gpr[29], cpu->gpr[30], cpu->gpr[31], c.actor);
    cpu->ctr = s.judge;
    search_name_registers(cpu, &s.n);
    cpu->gpr[0] = lr;
    cpu->gpr[3] = c.result;
    cpu->gpr[4] = c.r4;
    cpu->gpr[11] = cpu->gpr[1] - 32u;
    cpu->gpr[12] = s.judge;
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (c.cr0 << 28);
    cpu->downcount -= (s64)c.cycles;
    cpu->cycle_observation_suffix = 2u;
    cpu->pc = lr & ~3u;
    s_search_entries += s.n.entries;
    return 1;
}

/* The walk batched: the nodes run (0, nothing changed). */
static unsigned search_judge_walk(CPUState* cpu) {
    if (!bluewake_native_search_enabled || !bluewake_native_search_judge_ready)
        return 0;
    if (cpu->pc != SEARCH_JUDGE_FILTER || cpu->lr != SEARCH_NDIT_RETURN || cpu->gpr[29] != SEARCH_JUDGE_FILTER ||
        (cpu->ctr & ~3u) != SEARCH_JUDGE_FILTER || cpu->gpr[30] != cpu->gpr[4] || cpu->host_call != NULL)
        return 0;
    if (!judge_ready(cpu))
        return 0;
    const u32 limit = search_limit(cpu);
    JudgeSearch s;
    if (limit < SEARCH_JUDGE_CALL + SEARCH_JUDGE_STEP || !judge_search(cpu, limit, &s))
        return 0;
    const u8* ram = cpu->ram;
    const u32 size = cpu->ram_size;
    u32 node = cpu->gpr[3], next = cpu->gpr[31], cycles = 0u, actor = 0u, judged_next = 0u;
    unsigned nodes = 0;
    for (;;) {
        if (next == 0u)
            break; /* the walk ends after this node */
        JudgeCall c;
        u32 after;
        /* cNdIt_Judge's 80244FA0 (2): lwz r31,8(r31) */
        if (!judge_call(cpu, &s, node, &c) || c.result != 0u ||
            !judge_word(ram, size, next + 8u, s.lo, s.hi, &after))
            break;
        const u32 iteration = c.cycles + SEARCH_JUDGE_STEP;
        if (cycles + iteration > limit)
            break;
        cycles += iteration;
        nodes++;
        actor = c.actor;
        judged_next = next;
        node = next;
        next = after;
    }
    if (nodes == 0u)
        return 0;
    /* The last iteration's stores: findObjectCB saved cNdIt_Judge's r31 (the
     * node after the one judged) and r30 (the filter). */
    judge_stores(cpu, &s, SEARCH_NDIT_RETURN, cpu->gpr[29], cpu->gpr[30], judged_next, actor);
    search_name_registers(cpu, &s.n);
    cpu->gpr[0] = SEARCH_NDIT_RETURN;
    cpu->gpr[3] = node;
    cpu->gpr[4] = s.filter;
    cpu->gpr[11] = cpu->gpr[1] - 32u;
    cpu->gpr[12] = cpu->gpr[29];
    cpu->gpr[31] = next;
    cpu->ctr = cpu->gpr[29];
    cpu->lr = SEARCH_NDIT_RETURN;
    cpu->cr = (cpu->cr & 0x0FFFFFFFu) | ((0x4u | (cpu->xer >> 31)) << 28);
    cpu->cycle_observation_suffix = 1u;
    cpu->downcount -= (s64)cycles;
    s_search_entries += (unsigned long long)s.n.entries * nodes;
    return nodes;
}

BLUEWAKE_SEARCH_EXPORT unsigned bluewake_native_search_judge(CPUState* cpu) {
    const unsigned nodes = search_judge_walk(cpu);
    if (nodes != 0u) {
        s_judge_runs++;
        s_judge_nodes += nodes;
    } else {
        s_judge_declined++;
    }
    return nodes;
}
/* ---- end of the judge ---- */
