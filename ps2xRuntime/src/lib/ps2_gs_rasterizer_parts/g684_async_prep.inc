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
