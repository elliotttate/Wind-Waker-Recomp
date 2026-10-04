#ifndef BLUEWAKE_DRAW_TAGS_H
#define BLUEWAKE_DRAW_TAGS_H

#include "core/cpu.h"

#include <stdbool.h>

// Draw tags, for Smooth Motion: what the game is drawing, where GX alone
// cannot tell. Just before such a draw, the host writes the game object's
// address (and a particle's age) to two BP registers the retail GX never
// uses; the renderer takes them with the next draw (DrawPlan::draw_tag) and
// frame_interp.cpp pairs the draw with the same object's in the frame before.
// Only a function the game calls through a pointer (a virtual call) starts a
// dispatch boundary the host sees; one it calls directly does not.
//
// Particles. The game computes a particle's corners on the CPU each game
// frame and sends them to GX as positions, so the in-between frame, which
// blends matrices, drew every particle (pot dust, spray, smoke, sparkles)
// where the next frame has it: they moved at 30 FPS. Copies of one particle
// shape cannot be told apart by what GX receives. Each JPA particle draw (a
// JPADrawExec*::exec, the particle in r5) is tagged with the particle and its
// age, and drawn halfway along its own path. So is a particle an emitter's
// callback draws itself (JPADrawExecCallBack: the ripples on the water).
//
// Wakes. The boat's bow waves and the trail behind it are drawn by their
// emitters' callbacks (dPa_waveEcallBack, dPa_trackEcallBack) as fans and
// strips through their particles, where the boat is now: half a frame ahead
// of the boat in the in-between frame at full sail, a second wake at its bow.
// Before the callback draws, the host tags the next draws with the emitter
// and how many there will be; each is paired with the draw at its place in
// the frame before, and vertex by vertex (the particles of a steady wake shift
// one place a frame, so that is the wake half a frame later).
//
// Cloth. Flags (dCloth_packet_c, which the game's flag actors create) and the
// boat's sail (daHo_packet_c) move their vertices on the CPU each game frame
// and draw them as strips through position arrays. Their matrices barely
// move, so the in-between frame drew the cloth where the next frame has it:
// it flapped at 30 FPS. Before such a packet draws, the host tags its strips
// the same way as a wake's (a scope over a count of draws), with the flag
// that they index their positions; each strip is blended vertex by vertex.
//
//   BLUEWAKE_DRAW_TAGS=0   off
#define BLUEWAKE_PARTICLE_DRAW_FIRST 0x80260BACu // JPADrawExecBillBoard::exec
#define BLUEWAKE_PARTICLE_DRAW_LAST 0x80264C4Cu  // JPADrawExecCallBack::exec
#define BLUEWAKE_WAKE_DRAW_FIRST 0x8007E484u     // dPa_waveEcallBack::draw
#define BLUEWAKE_WAKE_DRAW_LAST 0x8007F3BCu      // dPa_trackEcallBack::draw
#define BLUEWAKE_CLOTH_DRAW 0x80063728u          // dCloth_packet_c::draw
#define BLUEWAKE_SAIL_DRAW 0x800E93B8u           // daHo_packet_c::draw (the boat's sail)

// Once the guest is running.
void bluewake_draw_tags_attach(CPUState* cpu);

// At every dispatch boundary (the chassis edge service): two compares while
// the address is outside the tagged draw functions. Writes to the GX stream only;
// the guest's state is not changed.
extern bool bluewake_draw_tags_enabled;
void bluewake_draw_tags_enter(CPUState* cpu, u32 address);
static inline void bluewake_draw_tags_dispatch(CPUState* cpu, u32 address) {
    if (__builtin_expect(address - BLUEWAKE_PARTICLE_DRAW_FIRST <=
                                 BLUEWAKE_PARTICLE_DRAW_LAST - BLUEWAKE_PARTICLE_DRAW_FIRST ||
                             address - BLUEWAKE_WAKE_DRAW_FIRST <= BLUEWAKE_WAKE_DRAW_LAST - BLUEWAKE_WAKE_DRAW_FIRST ||
                             address == BLUEWAKE_CLOTH_DRAW || address == BLUEWAKE_SAIL_DRAW,
                         0) &&
        bluewake_draw_tags_enabled)
        bluewake_draw_tags_enter(cpu, address);
}

#endif
