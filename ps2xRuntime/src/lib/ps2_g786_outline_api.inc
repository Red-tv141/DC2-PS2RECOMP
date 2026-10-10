// ==============================================================================================
// G786 — VERSIONED OUTLINE MERGE: the cross-TU surface between the rasterizer (front end, which
// plans the merge and translates it to vertex indices) and the GL backend (which takes the
// snapshots and swaps the composite texture). plans/phase-G786-ledger.md §4.
//
// A run of character-outline pairs [S1 C1 S2 C2 … Sn Cn] on one work RTT W is executed as ONE
// silhouette batch S* = S1..Sn and ONE composite batch C* = C1..Cn. Composite k must see W as it
// was after silhouette k, so wherever a later silhouette writes over composite k's read rect, the
// backend copies that rect of W into snapshot slot k right after silhouette k's last draw, and
// composite k's draws sample the snapshot instead of the live surface.
//
// Exec-thread only; both sides run inside one synchronous flush. Declarations only (Rule 12b): no
// std:: type, fixed capacity.
// ==============================================================================================
#ifndef DC2_G786_OUTLINE_API_INC
#define DC2_G786_OUTLINE_API_INC 1

#include <cstdint>

constexpr int kG786MaxSlots = 24;

struct G786SnapDir
{
    int vtx;            // take the snapshot before the first draw whose firstVtx >= vtx
    int slot;
    int x0, y0, x1, y1; // W-space texel rect, inclusive, top-origin
    uint32_t wfbp;      // the work RTT (atlas page offset = wfbp - 0x139)
};
struct G786OvrDir
{
    int vtxBegin, vtxEnd; // composite draws with firstVtx in [vtxBegin, vtxEnd) sample `slot`
    int slot;
};

// Written by the rasterizer immediately before a synchronous submit, cleared right after it.
extern G786SnapDir g_g786Snap[kG786MaxSlots];
extern int g_g786SnapN;
extern G786OvrDir g_g786Ovr[kG786MaxSlots];
extern int g_g786OvrN;
// Backend bookkeeping (monotonic): directives honoured, snapshot copies, overridden draws, and
// overrides that could NOT be honoured (unexpected texture source) — the last must stay 0.
extern uint64_t g_g786Consumed, g_g786Copies, g_g786OvrDraws, g_g786OvrMiss;
// Atlas-layout replication: set (exec thread) around the composite flush's G310 atlas refresh so the
// backend's compositor pass also builds each pending slot's atlas image; replicated / refused slots.
extern uint32_t g_g786ReplReq;
extern uint64_t g_g786Repl, g_g786ReplMiss;
// Overrides refused because the slot's snapshot belongs to another run (must stay 0).
extern uint64_t g_g786OvrStale;

#endif // DC2_G786_OUTLINE_API_INC
