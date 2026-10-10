// G791 — native ShadowMDT, GS front-door side (plans/phase-G791-ledger.md; API ps2_g791_shadow_api.inc).
// Included by ps2_gs_gpu.cpp at global scope, after the API, before gpu_gif_and_registers.inc.
//
// A native marker (kind 3) arrives inside processGIFPacket's tag loop at the face's exact stream position:
// every packet before it (renderInfo / GS env DIRECTs, earlier faces, earlier VU kicks sealed into earlier
// windows) has been parsed. The slot's bytes are the face's whole PATH1 output — K44 (ALPHA ADD), the ADD
// TRIFANs, K44, K46 (ALPHA SUB), the SUB TRIFANs, K46 per prism — and are parsed through the same front door,
// nested, before the outer tag loop continues. Nesting is safe at a tag boundary: no IMAGE transfer can be
// pending (the marker tag was reached), every PACKED tag resets its own register cursor and Q, and the G508
// holder is depth-counted. A marker whose slot does not hold its seq is an ORPHAN (counted, nothing drawn).
G791Slot g_g791Ring[kG791Ring];
G791Counters g_g791C;

#if defined(PS2X_G775_DIAG)
#include "ps2_g791_shadow_oracle.inc" // verify mode (kinds 1/2): the geometry oracle
#endif

static void g791OnTagMarker(GS *gs, uint64_t tagHi)
{
    const uint32_t kind = uint32_t((tagHi >> 40) & 0xffu), seq = uint32_t(tagHi);
    if (kind == kG791KindNative)
    {
        g_g791C.markers.fetch_add(1u, std::memory_order_relaxed);
        G791Slot &s = g_g791Ring[seq & (kG791Ring - 1u)];
        if (s.state.load(std::memory_order_acquire) != 1u || s.seq.load(std::memory_order_relaxed) != seq)
        {
            g_g791C.orphans.fetch_add(1u, std::memory_order_relaxed);
            return;
        }
        if (!s.bytes.empty())
            gs->processGIFPacket(s.bytes.data(), uint32_t(s.bytes.size()));
        s.state.store(0u, std::memory_order_release);
        g_g791C.parsed.fetch_add(1u, std::memory_order_relaxed);
        return;
    }
#if defined(PS2X_G775_DIAG)
    g791OracleMarker(tagHi);
#endif
}
