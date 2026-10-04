/* J3D's immediate matrix loads to the GX FIFO (GZLE01 J3DFifoLoadPosMtxImm,
 * J3DFifoLoadNrmMtxImm, J3DFifoLoadNrmMtxImm3x3), native.
 *
 * Each is one block: a command byte, a length and an XF address (the matrix
 * index scaled) as halfwords, then the matrix's words copied one at a time
 * with lwz/stw from the matrix to the gather pipe at 0xCC008000 (r5 holds
 * 0xCC010000, the store offset -32768). The J3D draw code calls them for
 * every shape it draws: 0.28 percent of the game thread on Outset together,
 * most of it the per-store path of the translation (each pipe store leaves
 * the RAM fast path for gather_pipe.h's out-of-line store) and the block
 * machinery around it.
 *
 * A native call leaves what the translation leaves: r0 the last word copied,
 * r5 0xCC010000, the scaled index in r4 (the normal matrix forms' mulli), the
 * block's cycles, the last pipe store's pc and cycle suffix (each pipe store
 * leaves the RAM fast path, which stores them), pc at the return address -
 * and the gather pipe sees the same writes in the same order: through
 * bw_gather_pipe_put, store by store, when the host takes words one at a
 * time or when the batch would reach its flush point inside the function (so
 * every host call, and every read of the matrix after one, happens where the
 * translation makes it); otherwise appended to the batch at once, which is
 * the same bytes with no host call in between. Loads of the matrix are plain
 * RAM reads; stores to the pipe touch no reservation and no write journal.
 *
 * It declines, changing nothing, unless that is certain: the matrix entirely
 * in plain RAM (no mirror, no alias over MEM1), the host's pipe writer set
 * (without it a pipe store goes to the host's MMIO handler), no exception
 * pending, no write journal, the turn's budget not spent, and the next
 * deadline beyond the block (so the translation prepays it and never leaves
 * its copy for the per-instruction path). The host's GX writers change no
 * cycle state, deadline or interrupt source (runtime/host/src/main.c,
 * host_mmio_write).
 *
 * tests/native_fifo_test.c compares each against the translation, every
 * register, RAM byte and pipe write. No identifier here may be `ctx`. */
#include "native_fifo.h"
#include "gather_pipe.h"

#include <stdio.h>

int bluewake_native_fifo_enabled;

enum { FIFO_POS, FIFO_NRM, FIFO_NRM33, FIFO_COUNT };
static unsigned long long s_fifo_runs[FIFO_COUNT], s_fifo_declined[FIFO_COUNT];

void bluewake_native_fifo_report(void) {
    fprintf(stderr, "[native-fifo] pos=%llu/%llu nrm=%llu/%llu nrm3x3=%llu/%llu (native/declined)\n", s_fifo_runs[0],
            s_fifo_declined[0], s_fifo_runs[1], s_fifo_declined[1], s_fifo_runs[2], s_fifo_declined[2]);
}

/* The three functions' shapes: the block's cycles, the length halfword, the
 * pc and cycle suffix of each header store and of the first word's stw (the
 * next ones follow 8 bytes and 2 cycles apart), the matrix words' offsets and
 * the bytes they span. */
typedef struct FifoShape {
    u32 entry, cycles, length;
    u32 suffix_command, suffix_length, suffix_address, suffix_first_store;
    u32 pc_command, pc_length, pc_address, pc_first_store;
    u32 words, span;
    u8 offsets[12];
} FifoShape;

static const FifoShape k_fifo_shapes[FIFO_COUNT] = {
    /* J3DFifoLoadPosMtxImm: li r0,16; lis r5; stb; li r0,11; sth; rlwinm r0,r4,2,16,29; sth; 12 x (lwz, stw). */
    {BLUEWAKE_J3D_FIFO_POS_MTX, 32u, 11u, 29u, 27u, 25u, 23u, 0x802D8BE0u, 0x802D8BE8u, 0x802D8BF0u, 0x802D8BF8u,
     12u, 48u, {0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44}},
    /* J3DFifoLoadNrmMtxImm: li r0,16; lis r5; stb; li r0,8; sth; mulli r4,r4,3; addi r0,r4,1024; sth; 9 x. */
    {BLUEWAKE_J3D_FIFO_NRM_MTX, 29u, 8u, 26u, 24u, 19u, 17u, 0x802D8C60u, 0x802D8C68u, 0x802D8C74u, 0x802D8C7Cu,
     9u, 44u, {0, 4, 8, 16, 20, 24, 32, 36, 40}},
    /* J3DFifoLoadNrmMtxImm3x3: the same, with the nine words in a row. */
    {BLUEWAKE_J3D_FIFO_NRM_MTX33, 29u, 8u, 26u, 24u, 19u, 17u, 0x802D8CCCu, 0x802D8CD4u, 0x802D8CE0u, 0x802D8CE8u,
     9u, 36u, {0, 4, 8, 12, 16, 20, 24, 28, 32}},
};

/* The block's entry (the translation prepays it and refunds nothing): no
 * exception, the turn's budget not spent, and either no deadline or one at
 * least the whole block away, so no in-block deadline test (an access whose
 * suffix exceeds the deadline) leaves the prepaid copy. */
static inline bool fifo_ready(const CPUState* cpu, s64 cycles) {
    return cpu->exception == 0u && g_mem_write_journal == NULL && bw_gather_pipe_write != NULL &&
           cpu->cycle_budget > 0 && cpu->downcount > -cpu->cycle_budget &&
           (cpu->cycle_deadline_budget <= 0 ||
            (cpu->cycle_deadline_budget >= cycles && cpu->cycle_deadline_budget + cpu->downcount >= cycles));
}

/* A pipe store of the prepaid copy (bw_write*_at off the RAM fast path):
 * the instruction's pc and suffix, then the pipe. */
static inline void fifo_put(CPUState* cpu, u32 pc, u32 suffix, u64 value, u8 size) {
    cpu->pc = pc;
    cpu->cycle_observation_suffix = suffix;
    bw_gather_pipe_put(value, size);
}

static int fifo_load(CPUState* cpu, const FifoShape* shape) {
    const u32 matrix = cpu->gpr[3];
    if (!fifo_ready(cpu, shape->cycles) || !ppc_dispatch_poll_read_stable(cpu, matrix, shape->span))
        return 0;
    const u8* m = cpu->ram + (matrix - GC_RAM_BASE);
    const bool position = shape->entry == BLUEWAKE_J3D_FIFO_POS_MTX;
    /* rlwinm r0,r4,2,16,29, or mulli r4,r4,3 and addi r0,r4,1024. */
    const u32 scaled = position ? cpu->gpr[4] : cpu->gpr[4] * 3u;
    const u32 address = position ? (cpu->gpr[4] << 2) & 0x0000FFFCu : scaled + 1024u;
    cpu->downcount -= shape->cycles;
    const u32 bytes = 5u + 4u * shape->words;
    if (bw_gather_pipe_bytes != NULL && bw_gather_pipe_length + bytes < BW_GATHER_PIPE_BATCH) {
        /* No flush inside: the batch gets the same bytes the stores would put. */
        u8* out = bw_gather_pipe_buffer + bw_gather_pipe_length;
        out[0] = 16u;
        out[1] = 0u;
        out[2] = (u8)shape->length;
        out[3] = (u8)(address >> 8);
        out[4] = (u8)address;
        u32 word = 0;
        for (u32 i = 0; i < shape->words; ++i) {
            word = read_be32(m + shape->offsets[i]);
            write_be32(out + 5u + 4u * i, word);
        }
        bw_gather_pipe_length += bytes;
        cpu->gpr[0] = word;
    } else {
        /* Store by store, as the copy makes them: a host call (a word to the
         * writer, or a full batch) may come between any two. */
        cpu->gpr[0] = 16u;
        cpu->gpr[5] = 0xCC010000u;
        fifo_put(cpu, shape->pc_command, shape->suffix_command, 16u, 1);
        cpu->gpr[0] = shape->length;
        fifo_put(cpu, shape->pc_length, shape->suffix_length, (u16)shape->length, 2);
        cpu->gpr[4] = scaled;
        cpu->gpr[0] = address;
        fifo_put(cpu, shape->pc_address, shape->suffix_address, (u16)address, 2);
        for (u32 i = 0; i < shape->words; ++i) {
            cpu->gpr[0] = read_be32(m + shape->offsets[i]);
            fifo_put(cpu, shape->pc_first_store + 8u * i, shape->suffix_first_store - 2u * i, cpu->gpr[0], 4);
        }
    }
    cpu->gpr[4] = scaled;
    cpu->gpr[5] = 0xCC010000u;
    cpu->cycle_observation_suffix = 1u;
    cpu->pc = cpu->lr & ~3u;
    return 1;
}

int bluewake_native_fifo(CPUState* cpu, u32 address) {
    unsigned which;
    switch (address) {
    case BLUEWAKE_J3D_FIFO_POS_MTX: which = FIFO_POS; break;
    case BLUEWAKE_J3D_FIFO_NRM_MTX: which = FIFO_NRM; break;
    case BLUEWAKE_J3D_FIFO_NRM_MTX33: which = FIFO_NRM33; break;
    default: return 0;
    }
    const int done = fifo_load(cpu, &k_fifo_shapes[which]);
    if (done)
        s_fifo_runs[which]++;
    else
        s_fifo_declined[which]++;
    return done;
}
