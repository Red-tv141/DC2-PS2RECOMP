// G684: one cooperative preparation dispatch for the accepted graph's front end.
//
// Two independent transforms sit between dependency binding and the synchronous backend submit:
// the texture decode (a read-only transform from the frozen guest VRAM image into a private RGBA
// vector) and the translation of the ordered entry stream into vertices and state-batched draw
// runs. The accepted graph used to run both on the GS owner, in series, while the machine's other
// cores were idle (G671: 45.7 CPU-ms/f over a 25.5 ms frame = 1.79 of 6 physical cores).
//
// ⭐ P4/P5 run BOTH through ONE `GSRowPool::runCoop` dispatch. The pool is the one G637 already
// built for CPU band replay — eleven affinitised lanes that G671 measured at ~1% occupancy — so
// there are no new threads, no second affinity policy, and the join waits for the worst CHUNK
// rather than the worst half. The chunk vector is heterogeneous: the low indices are decode jobs
// and the high indices are ordered entry ranges. They are mutually independent, so one fence
// covers both and a lane that finishes a short decode immediately steals vertex work.
//
// ⛔⛔ A CHUNK BODY MUST NEVER RE-ENTER THE POOL. `g178DecodeWhole` carries its own promoted
// 8-lane row split (G298), and `GSRowPool` has ONE generation and ONE cursor: a second dispatch
// raised while the first is live overwrites `m_coopBounds`/`m_coopJob` and corrupts both. The
// decode therefore runs single-lane INSIDE a chunk, which loses nothing — the outer dispatch is
// already supplying the parallelism, over a strictly larger pool of independent work.
#include <cassert>

template <typename T, typename U>
static inline void g684DiagAdd(std::atomic<T> &counter, U value)
{
#if defined(PS2X_G684_HOT_DIAG)
    counter.fetch_add(static_cast<T>(value), std::memory_order_relaxed);
#else
    (void)counter;
    (void)value;
#endif
}

// ⭐⭐⭐ G741: THE SHIP-READABLE SIBLING OF `g684DiagAdd`, FOR PER-FLUSH EVENTS ONLY.
//
// WHY IT EXISTS. `g684DiagAdd` is `(void)counter` unless `PS2X_G684_HOT_DIAG`, and every release
// build sets that OFF (`PS2Recomp/ps2xRuntime/CMakeLists.txt:134`). G736 §2 found one counter
// stranded behind it (`g_g261FboRejects`) and made that single site unconditional; G737 §1 turned
// the finding into a law — ⭐ **Rule G737-1: a refusal census must not be compile-gated**, because
// a `constexpr false` answers "why did this population take the slow path" with a silence that is
// indistinguishable from "nothing was refused", and G735 §6.2d built a whole conclusion on exactly
// that silence.
//
// G741 measured how much of the law was actually applied: **45 call sites were still dead**, and
// they carry the single highest-value routing decision in the renderer — GPU raster vs. the scalar
// CPU band replay (`g_g178Fallbacks`, 26 sites), its reason buckets, the per-flush VRAM->FBO
// upload/skip split, and the texture-cache hit rate.
//
// ⛔ AND THE STATED REASON FOR GATING THEM WAS FALSE. `g419_ab_instrument.inc` justified it with
// "making 26 PER-ENTRY increments unconditional would tax the shipped hot path". They are not
// per-entry: every one of the 26 sets `g_g266FlushRej`, calls `g581InvalidateCpuFallbackSnapshots()`
// and `return false` OUT of `g178TryFlushGpu` — so at most ONE of them can fire per flush, and the
// measured flush rate is **~44/frame** (G735 §1, `dungeon1`, 5161 frames). One relaxed atomic add at
// 44-88 events/frame is ~1 us/frame, four orders of magnitude under this host's 0.4 ms/f
// arm-independent spread.
//
// ⚠️ RULE 63 IS SATISFIED BY THE SAME NUMBER, not waived. Rule 63 forbids arming an instrument
// whose cost scales with the count a lever changes. A fallback-reducing lever does change this
// count — by at most the 44/frame that bounds it, i.e. by ~1 us/frame of instrument. This is the
// identical argument G736 §2 recorded when it made `fboRej` unconditional ("~1.3 calls/frame, so
// Rule 63 does not apply"), with the rate measured rather than assumed.
//
// ⛔ DO NOT USE THIS FOR A PER-ENTRY COUNTER. The bind/texture loop runs ~350 entries x ~47
// flushes = ~16,000 times per frame, and that population IS what a cache lever moves. Those
// counters stay on `g684DiagAdd` and are additionally reachable under `PS2X_G741_CENSUS` (default
// OFF) so a census binary can read them without any shipping arm paying for them — G710's law:
// keep diagnostics out of a hot TU by COMPILE-TIME exclusion, never by a runtime flag.
//
// Every reporter that mixes the two tiers must publish WHICH columns are structurally zero
// (`avail=`), or a reader cannot tell a real zero from a folded one (Rule G736-1).
template <typename T, typename U>
static inline void g741ShipAdd(std::atomic<T> &counter, U value)
{
    counter.fetch_add(static_cast<T>(value), std::memory_order_relaxed);
}

// G741: the per-ENTRY tier. Unconditional would violate Rule 63 (see above); `PS2X_G741_CENSUS`
// makes it readable in a purpose-built census binary, which is never a timing arm.
template <typename T, typename U>
static inline void g741CensusAdd(std::atomic<T> &counter, U value)
{
#if defined(PS2X_G684_HOT_DIAG) || defined(PS2X_G741_CENSUS)
    counter.fetch_add(static_cast<T>(value), std::memory_order_relaxed);
#else
    (void)counter;
    (void)value;
#endif
}

// True when the per-entry tier above is compiled in. Printed by every reporter that reads it, so a
// structural zero can never be quoted as a measurement.
static constexpr bool g741CensusCompiled()
{
#if defined(PS2X_G684_HOT_DIAG) || defined(PS2X_G741_CENSUS)
    return true;
#else
    return false;
#endif
}

// Set for the duration of a chunk body. Read by g178DecodeWhole to stand its own row split down.
static thread_local bool t_g684InPoolChunk = false;

struct G684PoolChunkScope
{
    bool prev;
    G684PoolChunkScope() : prev(t_g684InPoolChunk) { t_g684InPoolChunk = true; }
    ~G684PoolChunkScope() { t_g684InPoolChunk = prev; }
    G684PoolChunkScope(const G684PoolChunkScope &) = delete;
    G684PoolChunkScope &operator=(const G684PoolChunkScope &) = delete;
};

// One ordered slice of the translated vertex stream.  The vectors retain capacity between flushes;
// the GS owner only appends the completed slices, in index order, after the dispatch has joined.
struct G684VertexChunk
{
    std::vector<G178Vtx> verts;
    std::vector<G178Draw> draws;
    size_t firstEntry = static_cast<size_t>(-1);

    void reset()
    {
        verts.clear();
        draws.clear();
        firstEntry = static_cast<size_t>(-1);
    }
};

// Draw ranges differ by construction.  Everything else is shader/backend state and must match
// before the single run crossing the two independently-built chunks can be fused again.
static inline bool g684DrawStateEqual(const G178Draw &a, const G178Draw &b)
{
    return a.texKey == b.texKey && a.srcFbp == b.srcFbp &&
           a.blend == b.blend && a.tfx == b.tfx && a.tcc == b.tcc &&
           a.fge == b.fge && a.fogCol == b.fogCol &&
           a.depthFunc == b.depthFunc && a.depthWrite == b.depthWrite &&
           a.bilinear == b.bilinear && a.wrapU == b.wrapU && a.wrapV == b.wrapV &&
           a.uvOriginX == b.uvOriginX && a.uvOriginY == b.uvOriginY &&
           a.uvRoundU == b.uvRoundU && a.uvRoundV == b.uvRoundV &&
           a.uvPeriodU == b.uvPeriodU && a.uvPeriodV == b.uvPeriodV &&
           a.screenSprite == b.screenSprite &&
           a.scX0 == b.scX0 && a.scY0 == b.scY0 &&
           a.scX1 == b.scX1 && a.scY1 == b.scY1;
}
