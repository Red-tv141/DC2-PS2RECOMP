// ==============================================================================================
// G791 — NATIVE ShadowMDT: the cross-TU surface between the EE seam
// (dc2_game_override_parts/g791_shadow_native.inc) and the GS front door (ps2_g791_shadow_gs.inc,
// called from gpu_gif_and_registers.inc's tag loop). plans/phase-G791-ledger.md.
//
// The EE generates, per admitted shadow face, the exact GIF bytes the mg shadow microprogram would have
// XGKICK'd, publishes them in a slot of g_g791Ring, and replaces the face's REF with a REF to a 2-qword marker
// ([VIF NOP NOP NOP DIRECT 1] + a NLOOP=0 GIF tag whose REGS carry 0xC791 | kind | seq). The parse thread
// meets the marker at the face's exact stream position and parses the slot's bytes right there.
// ==============================================================================================
#ifndef DC2_G791_SHADOW_API_INC
#define DC2_G791_SHADOW_API_INC 1

#include <atomic>
#include <cstdint>
#include <vector>

constexpr uint64_t kG791MarkerMagic = 0xC791ull;
constexpr uint32_t kG791KindBegin = 1u, kG791KindEnd = 2u, kG791KindNative = 3u; // REGS bits 40..47
// Power of two. In flight = faces the EE has published and the parse thread has not reached: about one to two
// frames (Dungeon1 ~40-50 faces/frame, every other route fewer). A slot still in flight is never reused: that face
// stays legacy (refusal "ring"). Memory = kG791Ring x the largest face a slot has held (~150 KB worst).
constexpr uint32_t kG791Ring = 512u;

// One face's bytes in flight. EE: state 0 -> write bytes -> seq -> state 1 (release). Parse thread: state 1 and
// seq match -> parse -> state 0. The EE never overwrites a slot still in state 1 (the face then stays legacy).
struct G791Slot
{
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> state{0};
    std::vector<uint8_t> bytes;
};
extern G791Slot g_g791Ring[kG791Ring];
struct G791Counters
{
    std::atomic<uint64_t> markers{0}, parsed{0}, orphans{0};
};
extern G791Counters g_g791C;

#endif
