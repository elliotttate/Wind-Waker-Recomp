/* Two collision checks (GZLE01 cBgS_Chk::ChkSameActorPid and
 * dBgW::ChkGrpThrough), native.
 *
 * Both are small integer functions the collision code calls per polygon
 * group and per check: ChkSameActorPid (four blocks) 0.24 percent of the game
 * thread on Outset, ChkGrpThrough (a virtual call, 0.64 percent) more, and
 * the translation of ChkGrpThrough is split between two chunks: its blocks
 * from 0x800A96E0 on (the water, lava, poison and light group tests) are in
 * the next chunk, so every call that reaches them leaves its chunk at the
 * boundary, goes back through the chassis loop to run the rest, and returns
 * through the loop again.
 *
 * Each runs its blocks in order on the same values - the loads, the compares
 * and record forms into CR0 (with XER's SO), the cycles each block charges -
 * and leaves every register as the translation does: r0, r3, r4 and r5 as
 * the last instruction to write them left them, CR, the cycles, pc at the
 * return address. The prepaid copies' RAM loads store no cycle suffix; the
 * one block ChkGrpThrough has no prepaid copy of (0x800A96DC, the last in its
 * chunk) stores zero, and so does the native on that path.
 *
 * It declines, changing nothing, unless that is certain: every load plain
 * RAM, no exception pending, no write journal, the turn's budget not spent at
 * any block's entry, and the next deadline beyond each block (so the
 * translation prepays every block and never leaves its copy). A path across
 * ChkGrpThrough's chunk boundary also needs the boundary to pass silently,
 * as it does in play: the module's edge filter on (direct_calls.h: the host
 * handed over its flags and the builder's watch list), the host's edge
 * service quiet, and the boundary address (0x800A96E0 or 0x800A96F0) not one
 * the host watches - then neither the direct call that entered the function
 * nor the chassis loop between the chunks asks the host anything, and the
 * function's result is the same however its blocks are reached.
 *
 * tests/native_bg_test.c compares both with the translation, every register
 * and byte. No identifier here may be `ctx`. */
#include "native_bg.h"
#include "direct_calls.h"

#include <stdio.h>

int bluewake_native_bg_enabled;

enum { BG_SAME_ACTOR, BG_GRP_THROUGH, BG_COUNT };
static unsigned long long s_bg_runs[BG_COUNT], s_bg_declined[BG_COUNT], s_bg_crossed;

void bluewake_native_bg_report(void) {
    fprintf(stderr,
            "[native-bg] same-actor-pid=%llu/%llu grp-through=%llu/%llu (native/declined; %llu across the chunk "
            "boundary)\n",
            s_bg_runs[0], s_bg_declined[0], s_bg_runs[1], s_bg_declined[1], s_bg_crossed);
}

/* One block's entry as the translation makes it, on a running downcount:
 * false where it would stop for the turn's budget, or not prepay the block,
 * or leave its prepaid copy at a deadline (an access's suffix beyond it). */
static inline bool bg_block(const CPUState* cpu, s64* downcount, s64 cycles) {
    if (*downcount <= -cpu->cycle_budget)
        return false;
    if (cpu->cycle_deadline_budget > 0 &&
        (cpu->cycle_deadline_budget < cycles || cpu->cycle_deadline_budget + *downcount < cycles))
        return false;
    *downcount -= cycles;
    return true;
}

static inline bool bg_ram(const CPUState* cpu, u32 address, u32 size) {
    return ppc_dispatch_poll_read_stable((CPUState*)cpu, address, size);
}

static inline u32 bg_word(const CPUState* cpu, u32 address) {
    return read_be32(cpu->ram + (address - GC_RAM_BASE));
}

/* CR0 from a compare (or a record form's result against zero), with SO. */
static inline u32 bg_cr0(u32 cr, u32 xer, bool less, bool greater) {
    const u32 bits = (less ? 0x8u : greater ? 0x4u : 0x2u) | ((xer >> 31) & 1u);
    return (cr & 0x0FFFFFFFu) | (bits << 28);
}

static inline u32 bg_cr0_signed(u32 cr, u32 xer, s32 a, s32 b) { return bg_cr0(cr, xer, a < b, a > b); }
static inline u32 bg_cr0_unsigned(u32 cr, u32 xer, u32 a, u32 b) { return bg_cr0(cr, xer, a < b, a > b); }

static inline bool bg_ready(const CPUState* cpu) {
    return cpu->exception == 0u && g_mem_write_journal == NULL && cpu->cycle_budget > 0;
}

/* cBgS_Chk::ChkSameActorPid(r3 this, r4 pid): this->mActorPid (8) and
 * this->mSameActorChk (12, a byte). */
static int bg_same_actor(CPUState* cpu) {
    if (!bg_ready(cpu))
        return 0;
    const u32 xer = cpu->xer;
    s64 downcount = cpu->downcount;
    const u32 self = cpu->gpr[3], pid = cpu->gpr[4];
    u32 r0, r3, r5, cr;
    /* 8024734C (4): lwz r5,8(r3); addis r0,r5,1; cmplwi r0,0xFFFF; beq 80247374 */
    if (!bg_block(cpu, &downcount, 4) || !bg_ram(cpu, self + 8u, 4))
        return 0;
    r5 = bg_word(cpu, self + 8u);
    r0 = r5 + 0x10000u;
    cr = bg_cr0_unsigned(cpu->cr, xer, r0, 0xFFFFu);
    if (r0 != 0xFFFFu) {
        /* 8024735C (3): addis r0,r4,1; cmplwi r0,0xFFFF; beq 80247374 */
        if (!bg_block(cpu, &downcount, 3))
            return 0;
        r0 = pid + 0x10000u;
        cr = bg_cr0_unsigned(cr, xer, r0, 0xFFFFu);
        if (r0 != 0xFFFFu) {
            /* 80247368 (3): lbz r0,12(r3); cmplwi r0,0; bne 8024737C */
            if (!bg_block(cpu, &downcount, 3) || !bg_ram(cpu, self + 12u, 1))
                return 0;
            r0 = cpu->ram[self + 12u - GC_RAM_BASE];
            cr = bg_cr0_unsigned(cr, xer, r0, 0u);
            if (r0 != 0u) {
                /* 8024737C (4): subf r0,r5,r4; cntlzw r0,r0; rlwinm r3,r0,27,24,31; blr */
                if (!bg_block(cpu, &downcount, 4))
                    return 0;
                const u32 difference = pid - r5;
                r0 = difference == 0u ? 32u : (u32)__builtin_clz(difference);
                r3 = ((r0 << 27) | (r0 >> 5)) & 0xFFu;
                goto done;
            }
        }
    }
    /* 80247374 (2): li r3,0; blr */
    if (!bg_block(cpu, &downcount, 2))
        return 0;
    r3 = 0u;
done:
    cpu->gpr[0] = r0;
    cpu->gpr[3] = r3;
    cpu->gpr[5] = r5;
    cpu->cr = cr;
    cpu->downcount = downcount;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

/* The chunk boundary inside ChkGrpThrough passes without the host: the edge
 * filter's own test (dispatch_loop.h), which a direct call's return path
 * (direct_calls.h) only narrows. */
static inline bool bg_boundary_silent(const CPUState* cpu, u32 address) {
    return bw_edge_filter_enabled && bw_edge_watch_ready && bw_host_quiet(cpu) && bw_edge_unwatched(address);
}

/* dBgW::ChkGrpThrough(r3 this, r4 group, r5 pass check, r6 depth):
 * this->pm_bgd (148) -> m_g_tbl (36) -> [group].m_info (52-byte entries, at
 * 48), against the pass check's mask flags (4). */
static int bg_grp_through(CPUState* cpu) {
    if (!bg_ready(cpu))
        return 0;
    const u32 xer = cpu->xer;
    s64 downcount = cpu->downcount;
    const u32 check = cpu->gpr[5], depth = cpu->gpr[6];
    u32 r0 = cpu->gpr[0], r3 = cpu->gpr[3], r4 = cpu->gpr[4], cr;
    u32 boundary = 0u;
    bool suffix_zero = false;
    u32 flags = 0u;
    /* 800A9684 (2): cmpwi r6,2; bne 800A9694 */
    if (!bg_block(cpu, &downcount, 2))
        return 0;
    cr = bg_cr0_signed(cpu->cr, xer, (s32)depth, 2);
    if (depth != 2u)
        goto false_9694;
    /* 800A968C (2): cmplwi r5,0; bne 800A969C */
    if (!bg_block(cpu, &downcount, 2))
        return 0;
    cr = bg_cr0_unsigned(cr, xer, check, 0u);
    if (check == 0u)
        goto false_9694;
    /* 800A969C (11): lwz r3,148(r3); lwz r3,36(r3); mulli r0,r4,52; add r3,r3,r0;
     * lwz r4,48(r3); lis r3,8; addi r0,r3,1792; and. r0,r4,r0; bne 800A96D4 */
    if (!bg_block(cpu, &downcount, 11) || !bg_ram(cpu, r3 + 148u, 4))
        return 0;
    r3 = bg_word(cpu, r3 + 148u);
    if (!bg_ram(cpu, r3 + 36u, 4))
        return 0;
    r3 = bg_word(cpu, r3 + 36u);
    r0 = r4 * 52u;
    r3 += r0;
    if (!bg_ram(cpu, r3 + 48u, 4) || !bg_ram(cpu, check + 4u, 4))
        return 0;
    r4 = bg_word(cpu, r3 + 48u);
    flags = bg_word(cpu, check + 4u); /* every later load of the pass check's flags reads this word */
    r3 = 0x00080000u;
    r0 = r4 & 0x00080700u;
    cr = bg_cr0_signed(cr, xer, (s32)r0, 0);
    if (r0 == 0u) {
        /* 800A96C0 (3): lwz r0,4(r5); rlwinm. r0,r0,0,31,31; beq 800A96D4 */
        if (!bg_block(cpu, &downcount, 3))
            return 0;
        r0 = flags & 1u;
        cr = bg_cr0_signed(cr, xer, (s32)r0, 0);
        if (r0 != 0u)
            goto false_96CC;
    }
    /* 800A96D4 (2): rlwinm. r0,r4,0,23,23; beq 800A96F0 (the next chunk) */
    if (!bg_block(cpu, &downcount, 2))
        return 0;
    r0 = r4 & 0x100u;
    cr = bg_cr0_signed(cr, xer, (s32)r0, 0);
    if (r0 != 0u) {
        /* 800A96DC (1, no prepaid copy; suffix 0): lwz r0,4(r5); the chunk ends */
        if (!bg_block(cpu, &downcount, 1))
            return 0;
        r0 = flags;
        suffix_zero = true;
        boundary = 0x800A96E0u;
        /* 800A96E0 (2): rlwinm. r0,r0,0,30,30; beq 800A96F0 */
        if (!bg_block(cpu, &downcount, 2))
            return 0;
        r0 &= 2u;
        cr = bg_cr0_signed(cr, xer, (s32)r0, 0);
        if (r0 != 0u) {
            /* 800A96E8 (2): li r3,0; blr */
            if (!bg_block(cpu, &downcount, 2))
                return 0;
            r3 = 0u;
            goto done;
        }
    } else {
        boundary = 0x800A96F0u;
    }
    /* 800A96F0, 800A970C, 800A9728 (2 each): rlwinm. r0,r4 with the group's
     * bit; beq to the next test. 800A96F8, 800A9714, 800A9730 (3 each):
     * lwz r0,4(r5); rlwinm. r0,r0 with the check's bit; beq to the next test.
     * 800A9704, 800A9720, 800A973C (2 each): li r3,0; blr. */
    {
        static const u32 group_bits[3] = {0x200u, 0x400u, 0x80000u};
        static const u32 check_bits[3] = {0x4u, 0x8u, 0x10u};
        for (unsigned test = 0; test < 3; ++test) {
            if (!bg_block(cpu, &downcount, 2))
                return 0;
            r0 = r4 & group_bits[test];
            cr = bg_cr0_signed(cr, xer, (s32)r0, 0);
            if (r0 == 0u)
                continue;
            if (!bg_block(cpu, &downcount, 3))
                return 0;
            r0 = flags & check_bits[test];
            cr = bg_cr0_signed(cr, xer, (s32)r0, 0);
            if (r0 == 0u)
                continue;
            if (!bg_block(cpu, &downcount, 2))
                return 0;
            r3 = 0u;
            goto done;
        }
    }
    /* 800A9744 (2): li r3,1; blr */
    if (!bg_block(cpu, &downcount, 2))
        return 0;
    r3 = 1u;
    goto done;
false_96CC:
    /* 800A96CC (2): li r3,0; blr */
    if (!bg_block(cpu, &downcount, 2))
        return 0;
    r3 = 0u;
    goto done;
false_9694:
    /* 800A9694 (2): li r3,0; blr */
    if (!bg_block(cpu, &downcount, 2))
        return 0;
    r3 = 0u;
done:
    if (boundary != 0u) {
        if (!bg_boundary_silent(cpu, boundary))
            return 0;
        s_bg_crossed++;
    }
    cpu->gpr[0] = r0;
    cpu->gpr[3] = r3;
    cpu->gpr[4] = r4;
    cpu->cr = cr;
    cpu->downcount = downcount;
    if (suffix_zero)
        cpu->cycle_observation_suffix = 0u;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

int bluewake_native_bg(CPUState* cpu, u32 address) {
    unsigned which;
    int done;
    switch (address) {
    case BLUEWAKE_BG_CHK_SAME_ACTOR_PID: which = BG_SAME_ACTOR; done = bg_same_actor(cpu); break;
    case BLUEWAKE_BG_CHK_GRP_THROUGH: which = BG_GRP_THROUGH; done = bg_grp_through(cpu); break;
    default: return 0;
    }
    if (done)
        s_bg_runs[which]++;
    else
        s_bg_declined[which]++;
    return done;
}
