#include "draw_tags.h"

#include "gxruntime/platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    kParticleAge = 0x78u, // JPABaseParticle::mCurFrame (f32), one more each game frame
    // BP registers the retail GX never writes (Dolphin's BPMemory leaves
    // 0x6A-0x7F unassigned); gxcore takes them with the next draw.
    kTagRegister = 0x7Eu, // the particle: (address - 0x80000000) / 4
    kAgeRegister = 0x7Du, // its age in game frames
    kScopeRegister = 0x7Cu, // an emitter whose callback draws next: (address - 0x80000000) / 4
    kScopeCountRegister = 0x7Bu, // how many draws it makes
    // In the count: the scope's draws index their positions (cloth), where a
    // wake's send them.
    kScopeIndexed = 0x800000u,
    kEmitterParticles = 0x184u, // JPABaseEmitter: its particle list's count (mNumLinks)
    kClothFlyGrid = 0x10u,      // dCloth_packet_c::mFlyGridSize (columns of vertices)
    // The sail's strips: one display list (d_a_grid's l_DL, 0x220 bytes),
    // called for its front and its back.
    kSailList = 0x8038B880u,
    kSailListSize = 0x220u,
    kSailVertexSize = 3u, // position, normal and texture coordinate, an 8-bit index each
    kLoadBp = 0x61u,
};

bool bluewake_draw_tags_enabled = true;

void bluewake_draw_tags_attach(CPUState* cpu) {
    (void)cpu;
    const char* on = getenv("BLUEWAKE_DRAW_TAGS");
    bluewake_draw_tags_enabled = on == NULL || on[0] != '0';
}

static void write_bp(u32 reg, u32 value) {
    dol_platform_gx_write(kLoadBp, 1u);
    dol_platform_gx_write((reg << 24) | (value & 0x00FFFFFFu), 4u);
}

static bool guest_object(u32 address) {
    return address >= 0x80000000u && address < 0x81800000u && (address & 3u) == 0u;
}

// The draws in the sail's display list: its primitives, read once.
static u32 sail_list_draws(CPUState* cpu) {
    static u32 draws = UINT32_MAX;
    if (draws != UINT32_MAX)
        return draws;
    u32 count = 0;
    u32 at = 0;
    while (at < kSailListSize) {
        const u32 op = mem_read8(cpu, kSailList + at);
        if (op == 0u) { // GX_NOP
            at += 1u;
            continue;
        }
        if ((op & 0x80u) == 0u || at + 3u > kSailListSize) {
            count = 0; // not the list this was written for
            break;
        }
        const u32 vertices = ((u32)mem_read8(cpu, kSailList + at + 1u) << 8) | mem_read8(cpu, kSailList + at + 2u);
        at += 3u + vertices * kSailVertexSize;
        count++;
    }
    draws = count;
    return draws;
}

void bluewake_draw_tags_enter(CPUState* cpu, u32 address) {
    if (cpu == NULL)
        return;
    if (address == BLUEWAKE_CLOTH_DRAW || address == BLUEWAKE_SAIL_DRAW) {
        const u32 packet = cpu->gpr[3];
        if (!guest_object(packet))
            return;
        u32 draws = 0;
        if (address == BLUEWAKE_CLOTH_DRAW) {
            // A strip between each column of vertices and the next (plot), for
            // the front and the back.
            const u32 columns = mem_read32(cpu, packet + kClothFlyGrid);
            draws = columns >= 2u && columns <= 64u ? 2u * (columns - 1u) : 0u;
        } else {
            draws = 2u * sail_list_draws(cpu);
        }
        if (draws == 0u)
            return;
        static unsigned reported;
        if (reported < 4u) {
            reported++;
            fprintf(stderr, "[draw-tags] %s %08X: %u strips\n",
                    address == BLUEWAKE_CLOTH_DRAW ? "cloth" : "sail", packet, draws);
        }
        write_bp(kScopeRegister, (packet - 0x80000000u) >> 2);
        write_bp(kScopeCountRegister, draws | kScopeIndexed);
        return;
    }
    if (address == BLUEWAKE_WAKE_DRAW_FIRST || address == BLUEWAKE_WAKE_DRAW_LAST) {
        const u32 emitter = cpu->gpr[4];
        if (!guest_object(emitter))
            return;
        const u32 particles = mem_read32(cpu, emitter + kEmitterParticles);
        u32 draws = 0;
        if (address == BLUEWAKE_WAKE_DRAW_FIRST) {
            // Two fans (the wave's two sides), from its anchor through each particle.
            draws = particles >= 2u ? 2u : 0u;
        } else if (particles >= 6u) {
            // A strip between each row of three particles and the row before.
            draws = (u32)((float)particles * (1.0f / 3.0f)) - 1u;
        }
        if (draws == 0u)
            return;
        write_bp(kScopeRegister, (emitter - 0x80000000u) >> 2);
        write_bp(kScopeCountRegister, draws);
        return;
    }
    // The functions that draw one particle (JPADrawVisitor.cpp); the others
    // in the range set colours and texture matrices, or draw a whole emitter.
    switch (address) {
    case 0x80260BACu: // JPADrawExecBillBoard
    case 0x80260D24u: // JPADrawExecRotBillBoard
    case 0x80260F2Cu: // JPADrawExecYBillBoard
    case 0x8026110Cu: // JPADrawExecRotYBillBoard
    case 0x8026168Cu: // JPADrawExecDirectional
    case 0x80261AD0u: // JPADrawExecRotDirectional
    case 0x80261F60u: // JPADrawExecDirectionalCross
    case 0x802624D4u: // JPADrawExecRotDirectionalCross
    case 0x80262A98u: // JPADrawExecDirBillBoard
    case 0x80262DC0u: // JPADrawExecRotation
    case 0x80262FBCu: // JPADrawExecRotationCross
    case 0x802632ECu: // JPADrawExecPoint
    case 0x80263380u: // JPADrawExecLine
    case 0x80264C4Cu: // JPADrawExecCallBack (the particle's callback draws it)
        break;
    default:
        return;
    }
    const u32 particle = cpu->gpr[5];
    if (!guest_object(particle))
        return;
    const u32 bits = mem_read32(cpu, particle + kParticleAge);
    float age;
    memcpy(&age, &bits, sizeof age);
    const u32 frames = age >= 0.f && age < 16777215.f ? (u32)age : 0u;
    // A tag goes with the next draw only. An invisible particle draws
    // nothing, so its tag goes with whatever is drawn next: the next
    // particle, whose own tag replaces it, or a draw the renderer does not
    // take as this particle's (another shape or vertex count).
    write_bp(kTagRegister, (particle - 0x80000000u) >> 2);
    write_bp(kAgeRegister, frames);
}
