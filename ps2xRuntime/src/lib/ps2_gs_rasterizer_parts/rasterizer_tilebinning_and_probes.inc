// ===== G144 FRAME-LEVEL TILE-BINNING (G168 DEFAULT 8 LANES, DC2_G144_NO_TILEBIN disables) =====
// Defer deferrable triangles (fast-path textured tris to a display framebuffer, non-RTT) into a
// display list instead of rasterizing each immediately; at a flush (a non-deferrable primitive, an
// fbp change, or frame end) rasterize the WHOLE list with T threads, each owning a disjoint
// horizontal BAND of scanlines and replaying every overlapping entry IN SUBMISSION ORDER clipped to
// its band. Bit-exact by the SAME argument as G143 (disjoint rows ⇒ disjoint FB/Z writes; in-order
// per band ⇒ Z/blend order preserved), but the parallelism now spans the WHOLE FRAME's triangle
// stream (one dispatch, thousands of tris/thread) instead of one triangle — so it is not limited by
// per-triangle size the way G143's intra-triangle row split is. A full POD snapshot of the exact GS
// draw state drawTriangle reads (ctx/prim/texa/texclut/pabe/3 verts) makes each entry self-contained.
// G521 ENTRY-SIZE INFLATION PROBE (compile-time, default 0 = ABSENT — the shipped struct is
// byte-identical to G520's). Appends dead payload so the capture's push and all five flush
// pre-passes stride over a LARGER entry while every behaviour, every field offset before the pad,
// and every consumer stays identical. Reading [G298:profile]'s five per-pass clocks on a padded
// build against an unpadded one gives the pre-passes' MARGINAL COST PER BYTE OF ENTRY SIZE — the
// only honest ceiling for G510 §6's state-table refactor, which is otherwise a ~580-site edit
// priced off a bucket total (G508's count x cost law, G520 §7a's fold law).
// The pad is non-elidable (G496): std::vector really allocates it and the implicit move/copy
// constructor really copies the array, so both the write side and the read stride move.
#ifndef DC2_G521_PADBYTES
#define DC2_G521_PADBYTES 0
#endif
// G521: tag type for constructing a G144Entry IN PLACE in g_g144List with DEFAULT-initialization.
// `emplace_back()` with no arguments VALUE-initializes, and because G144Entry's default ctor is
// implicit (non-trivial only via the shared_ptr member) that zero-fills all 256 bytes — a memset
// the pre-G521 path never paid. This tag gives a user-provided ctor that default-initializes
// instead, so the in-place arm writes each field exactly once and nothing more.
// `G144Entry() = default;` is defaulted ON FIRST DECLARATION, so it is NOT user-provided and the
// one `G144Entry e{}` site (rasterizer_g401_composite_probe.inc) keeps its zero-initialization.
// G521 `capinplace` VERDICT: REFUTED (mean raw −0.065 ms/f / −0.30%, 3/4 signs; blocked −0.011,
// **2/4** — sign-inconsistent on the blocked estimator, against an arc bar of 4/4 on both).
// 0 = compiled out, and the shipped capture is then byte-identical to pre-G521: a plain aggregate
// `G144Entry e;` stack local written at known rsp offsets, then G510's move-push. Set to 1 only to
// re-gate. Full detail: plans/phase-G521-fix-log.md §6.
#ifndef DC2_G521_INPLACEARM
#define DC2_G521_INPLACEARM 0
#endif
struct G144NoInit {};
struct G144Entry
{
#if DC2_G521_INPLACEARM
    // These two make G144Entry a NON-AGGREGATE, so they are compiled out with the lever: the
    // shipped struct must stay the exact aggregate every other phase measured.
    G144Entry() = default;
    explicit G144Entry(G144NoInit) {}
#endif
    GSContext ctx;
    GSPrimReg prim;
    GSTexaReg texa;
    GSTexClutReg texclut;
    bool pabe;
    uint32_t fogcol; // G364: FOGCOL is global (not per-context) GS state — snapshot it per entry
    bool titleRockScope;
    bool townDepthScope;
    GSVertex v[3];
    int minY, maxY; // screen-space bbox rows (post-scissor) for band-overlap prefilter
    G149Buf decoded; // G149: pre-decoded texture buffer (null unless texcache eligible); kept alive here
#if DC2_G521_PADBYTES > 0
    uint8_t g521Pad[DC2_G521_PADBYTES];
#endif
};
// G521: report the measured layout once, so the probe's slope is divided by a number that was
// READ from the binary rather than hand-computed. Inert unless DC2_G521_SIZE=1.
static void g521ReportEntrySize()
{
    static const bool s_on = (std::getenv("DC2_G521_SIZE") != nullptr);
    if (!s_on)
        return;
    static std::atomic<bool> s_done{false};
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        return;
    std::fprintf(stderr,
                 "[G521:size] G144Entry=%zu pad=%d ctx=%zu prim=%zu texa=%zu texclut=%zu "
                 "vertex=%zu v3=%zu buf=%zu\n",
                 sizeof(G144Entry), (int)DC2_G521_PADBYTES, sizeof(GSContext), sizeof(GSPrimReg),
                 sizeof(GSTexaReg), sizeof(GSTexClutReg), sizeof(GSVertex), sizeof(GSVertex) * 3u,
                 sizeof(G149Buf));
    std::fflush(stderr);
}
static std::vector<G144Entry> g_g144List;       // reused across frames (clear keeps capacity)
// G219 diagnostic snapshot buffer (see the [G219:cap]/[G219:dif] probes; inert unless
// DC2_G219_SKYPX=1).
uint8_t g_g219Snap[0x8000];
// G219 transition sentinel: called at raster/transfer choke points with a location tag; logs
// whenever the watched work-page qword (0x278420, blue after the zoom screen-copy) changes value,
// bracketing WHICH processing step the change happened inside. Inert unless DC2_G219_SKYPX=1.
static std::atomic<uint64_t> g_g219SentinelVal{0xDEADBEEFDEADBEEFull};

#if defined(_WIN32)
// G219 write-watchpoint (default-off, DC2_G219_GUARD=1 in addition to DC2_G219_SKYPX=1): make the
// host page holding VRAM offset 0x278400 read-only once the sentinel sees the post-copy content;
// the first write then faults, the vectored handler logs the writer's instruction address + thread
// and restores write access. Identifies the zeroing writer definitively (every prior software
// choke-point instrumentation missed it).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
namespace
{
    uint8_t *g_g219GuardBase = nullptr;   // host address of the guarded page
    std::atomic<bool> g_g219GuardArmed{false};

    LONG CALLBACK g219VectoredHandler(PEXCEPTION_POINTERS info)
    {
        if (!info || !info->ExceptionRecord ||
            info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
            return EXCEPTION_CONTINUE_SEARCH;
        const ULONG_PTR isWrite = info->ExceptionRecord->ExceptionInformation[0];
        const ULONG_PTR addr = info->ExceptionRecord->ExceptionInformation[1];
        uint8_t *base = g_g219GuardBase;
        if (!base || addr < reinterpret_cast<ULONG_PTR>(base) ||
            addr >= reinterpret_cast<ULONG_PTR>(base) + 0x1000u)
            return EXCEPTION_CONTINUE_SEARCH;
        DWORD oldProt = 0;
        VirtualProtect(base, 0x1000u, PAGE_READWRITE, &oldProt);
        g_g219GuardArmed.store(false, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_hits{0};
        const uint32_t n = s_hits.fetch_add(1u, std::memory_order_relaxed);
        if (n < 64u)
        {
            HMODULE mod = nullptr;
            const void *rip = info->ExceptionRecord->ExceptionAddress;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &mod);
            char name[260] = {0};
            if (mod)
                GetModuleFileNameA(mod, name, sizeof(name) - 1);
            std::fprintf(stderr,
                "[G219:guard] n=%u tid=%lu write=%llu vramOff=0x%llx rip=%p module=%s base=%p delta=0x%llx\n",
                n, (unsigned long)GetCurrentThreadId(),
                (unsigned long long)isWrite,
                (unsigned long long)(addr - reinterpret_cast<ULONG_PTR>(base) + 0x278000u),
                rip, name[0] ? name : "?", (void *)mod,
                mod ? (unsigned long long)(reinterpret_cast<ULONG_PTR>(rip) -
                                           reinterpret_cast<ULONG_PTR>(mod))
                    : 0ull);
            std::fflush(stderr);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void g219GuardArm(uint8_t *vramBase)
    {
        static const bool s_on = (std::getenv("DC2_G219_GUARD") != nullptr);
        if (!s_on || !vramBase)
            return;
        static bool s_handler = []() {
            AddVectoredExceptionHandler(1, g219VectoredHandler);
            return true;
        }();
        (void)s_handler;
        bool expected = false;
        if (!g_g219GuardArmed.compare_exchange_strong(expected, true, std::memory_order_relaxed))
            return;
        g_g219GuardBase = vramBase + 0x278000u;
        DWORD oldProt = 0;
        VirtualProtect(g_g219GuardBase, 0x1000u, PAGE_READONLY, &oldProt);
    }
}
#else
namespace
{
    void g219GuardArm(uint8_t *) {}
}
#endif
void g219Sentinel(const uint8_t *vram, uint32_t vramSize, const char *where)
{
    static const bool s_on = (std::getenv("DC2_G219_SKYPX") != nullptr);
    if (!s_on || !vram || 0x278420u + 8u > vramSize)
        return;
    // Ring of the last 8 sentinel visits (single GS worker thread proven by the tid trace), so a
    // detected transition can name its exact predecessor event, not just the detector.
    static char s_ring[8][96];
    static uint32_t s_ringPos = 0;
    uint64_t v = 0;
    std::memcpy(&v, vram + 0x278420u, 8);
    const uint64_t prev = g_g219SentinelVal.exchange(v, std::memory_order_relaxed);
    if (prev != v && prev != 0xDEADBEEFDEADBEEFull)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 600u)
        {
            const std::size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
            std::fprintf(stderr, "[G219:trans] n=%u tid=%04x at=%s old=%016llx new=%016llx\n",
                         n, (unsigned)(tid & 0xFFFFu), where,
                         (unsigned long long)prev, (unsigned long long)v);
            if (n < 40u)
                for (uint32_t k = 0; k < 8u; ++k)
                    std::fprintf(stderr, "[G219:ring] -%u %s\n", 8u - k,
                                 s_ring[(s_ringPos + k) & 7u]);
        }
    }
    std::snprintf(s_ring[s_ringPos & 7u], sizeof(s_ring[0]), "%s v=%016llx",
                  where, (unsigned long long)v);
    s_ringPos = (s_ringPos + 1u) & 7u;
    g219GuardArm(const_cast<uint8_t *>(vram));
}
static int g_g144Threads = -1;                  // -1 = env not read yet; 0/1 = off; >1 = lanes
static uint32_t g_g144BlockFbp = 0xFFFFFFFFu;   // fbp of the current deferred block (flush on change)
static GS *g_g144LastGs = nullptr;              // live GS for VRAM at flush (set each capture)
static uint8_t *g_g144LastVram = nullptr;       // member-safe frame-boundary alias, set beside LastGs
// Flush closures, assigned ONCE inside GSRasterizer::drawPrimitive so they keep that member's friend
// access to GS/private drawTriangle even when invoked later from g144FlushPending() (frame end).
static std::function<void()> g_g144FlushFn;     // PARALLEL band replay (mid-frame, on guest thread)
static std::function<void()> g_g144FlushSeqFn;  // SEQUENTIAL drain (frame end; no pool, full range)
// Replay control (thread_local: each band lane its own; set only during a flush).
static thread_local bool t_g144InReplay = false;
static thread_local bool t_g144Banded = false;
static thread_local int t_g144BandY0 = 0;
static thread_local int t_g144BandY1 = 0;
static thread_local size_t t_g258EntryIndex = static_cast<size_t>(-1);

// ===== G364: GS per-pixel FOG =====
// PRIM.FGE=1 blends the post-texture fragment colour towards the global FOGCOL by the
// interpolated per-vertex F byte (F=0xFF -> unfogged, F=0 -> pure fog colour; alpha untouched,
// applied BEFORE alpha blending). The GS state was parsed but never applied, so DC2's dungeon
// distance fade (72% of the scene's vertices carry FGE=1, FOGCOL=0x3c2828) was missing entirely.
// Rollback: DC2_G364_NO_FOG=1.
static bool g364FogEnabled()
{
    static const bool disabled = envFlagEnabled("DC2_G364_NO_FOG");
    return !disabled;
}
// During a deferred tile-bin band replay the live register may already have moved on, so read the
// value snapshotted into the G144 entry instead.
static inline uint32_t g364FogColNow()
{
    return t_g144InReplay ? t_g364FogCol : g_g364FogCol;
}
static inline uint8_t g364FogLerp(uint8_t c, uint32_t f, uint32_t fc)
{
    // Exact at both endpoints: f=255 -> c, f=0 -> fc.
    return static_cast<uint8_t>((static_cast<uint32_t>(c) * f + fc * (255u - f) + 127u) / 255u);
}
static inline void g364ApplyFog(uint32_t f, uint8_t &r, uint8_t &g, uint8_t &b)
{
    const uint32_t fc = g364FogColNow();
    r = g364FogLerp(r, f, fc & 0xFFu);
    g = g364FogLerp(g, f, (fc >> 8) & 0xFFu);
    b = g364FogLerp(b, f, (fc >> 16) & 0xFFu);
}

// ===== G248 MEASURE ONLY (DEFAULT OFF, DC2_G248_STAT=1) =====
// G247 found that A+D XYZ2 kicks to the character-RTT work targets dominate map_0's
// packed-register time, but that outer timing includes any pending display-list drain and
// GPU-depth synchronization before drawSprite() starts.  Time the sprite body itself here and
// aggregate by target / texture / blend state.  Pixel coverage is computed from the already-
// clipped loop rectangle; no per-pixel instrumentation is added.  This keeps the discriminator
// quiet and cheap enough to answer whether G248 should target fill work or flush fragmentation.
static const bool g_g248Stat = (std::getenv("DC2_G248_STAT") != nullptr);
// G553/G556/G557 reserve diagnostic residency slots for the dominant discovered targets on s05
// (0x159/0x13a) and Georama (0x181). Each slot is inert unless its phase lever is enabled; appending
// preserves every legacy index and lets the fixed-size residency machinery remain compile-time.
//
// G569 appends SIX MORE for route3's discovered targets (0x13d/0x157/0x14b/0x151/0x13f/0x141).
// ⭐ APPEND-ONLY IS THE WHOLE SAFETY ARGUMENT: every array in the residency machinery is declared
// `[kG248TargetCount]` (g_g261Res, g_g280ContentGen/g_g280Ovl, g_g310TargetPageGen,
// g_g310ProducerN/ProducerPages/SourcePages, g_g261MatByTarget, g_g248Sprite*, the T8 atlas and
// resolver), so raising the constant resizes all of them with no per-array edit, and indices 0..7
// keep their exact meaning — including G557's compositor slot 6. The ONE hand-written table that
// must grow in the same edit is kG261Fbp (rasterizer_rtt_census_and_waves.inc).
static constexpr uint32_t kG248TargetCount = 14u;
static constexpr uint32_t kG248StateCount = 4u; // bit0=tme, bit1=abe
static constexpr uint32_t kG248BucketCount = kG248TargetCount * kG248StateCount;
static std::atomic<uint64_t> g_g248SpriteCalls[kG248BucketCount]{};
static std::atomic<uint64_t> g_g248SpritePixels[kG248BucketCount]{};
static std::atomic<uint64_t> g_g248SpriteNs[kG248BucketCount]{};

// G400 (DEFAULT OFF, diagnostic): drop fbp=0x139 out of the resident-target set, so the whole
// Dungeon 6 alias family (0x139 + the post-process targets 0x13b/0x13c/0x14b that render inside
// its VRAM footprint) shares ONE owner — the CPU/VRAM path. If the free-roam darkening is caused
// by split ownership across that physical alias, this arm renders correctly (and slowly).
// DC2_G400_NO_RESIDENT_139=1 is the single-target shorthand; DC2_G400_NO_RESIDENT="139,13c"
// (hex, comma separated) drops an arbitrary subset, so the whole alias family can share one owner.
static bool g400NoResident(uint32_t fbp)
{
    static const uint64_t s_mask = []() -> uint64_t {
        uint64_t m = 0ull;
        if (std::getenv("DC2_G400_NO_RESIDENT_139") != nullptr)
            m |= 1ull << 0;
        const char *e = std::getenv("DC2_G400_NO_RESIDENT");
        if (e && *e)
        {
            const char *p = e;
            while (*p)
            {
                char *end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 16);
                if (end == p)
                    break;
                switch (v)
                {
                case 0x139u: m |= 1ull << 0; break;
                case 0x13cu: m |= 1ull << 1; break;
                case 0x143u: m |= 1ull << 2; break;
                case 0x146u: m |= 1ull << 3; break;
                case 0x155u: m |= 1ull << 4; break;
                default: break;
                }
                p = end;
                while (*p == ',' || *p == ' ')
                    ++p;
            }
        }
        return m;
    }();
    if (s_mask == 0ull)
        return false;
    switch (fbp)
    {
    case 0x139u: return (s_mask & (1ull << 0)) != 0ull;
    case 0x13cu: return (s_mask & (1ull << 1)) != 0ull;
    case 0x143u: return (s_mask & (1ull << 2)) != 0ull;
    case 0x146u: return (s_mask & (1ull << 3)) != 0ull;
    case 0x155u: return (s_mask & (1ull << 4)) != 0ull;
    default: return false;
    }
}

// ===== G569: GPU residency for route3's discovered render targets =====
//
// After G568 promoted discovered-target TRIANGLE deferral, route3's triangles are correctly batched
// and then the WHOLE batch is rejected `cpuonlyrtt` and CPU-replayed by construction, because
// `g255Rtt = g255GpuRttOn() && g248TargetIndex(fbp) >= 0` and none of route3's targets is named
// here. `[G290:gpufail]` on the G568 binary: `fbp=13d ent=1015`, `157 ent=327`, `14b ent=241/233/169`.
// All six are fbw=8, CT32, and share zbp=0x0d0.
//
// ⚠️ THE ADMISSION HAS MORE THAN ONE QUESTION (g534_admission_proof_true_but_insufficient). What was
// checked before writing this:
//   * guest DEPTH — `if (g255Rtt && guestDepth && !g262WideOn())` returns kG266FrRttDepth. These
//     batches all carry guest Z, so admission would merely trade `cpuonlyrtt` for `rttdepth` if the
//     G262 per-wave Z round trip were off. `g262WideOn()` is DEFAULT-ON whenever waves are on, so
//     the depth authority is the non-persistent per-wave one, not a new persistent claim.
//   * PAGE OVERLAP — with fbw=8 one page-row is 8 pages, so these targets (0x139=313 … 0x157=343,
//     spaced 2..6 pages apart) alias each other's VRAM. That is NOT new: the legacy five already
//     contain 0x139 and 0x13c three pages apart, and the alias/page-authority machinery
//     (rasterizer_alias_page_state / _page_authority / _target_page_authority) exists to arbitrate
//     exactly this. It is, however, the reason this lever ships DEFAULT-OFF with a bisect mask.
//   * ⚠️ 0x14b IS IN THE OPEN DUNGEON-6 ALIAS FAMILY. g400NoResident's own comment names
//     "0x139 + the post-process targets 0x13b/0x13c/0x14b that render inside its VRAM footprint"
//     as the split-ownership suspect for the STILL-OPEN Dungeon 6 darkening (G397–G400). It is slot
//     10 = mask bit 2 so it can be dropped alone; the Dungeon 6 route is a required gate.
//
// ⭐ PROMOTED DEFAULT-ON (G569). Route-ALIGNED gate on route3 (buckets of 200 scriptFrames, 3 whole
// runs per arm, only buckets where both arms have matched sample counts — 17,358 frames each):
// OFF 33.00 -> ON 29.74 ms/f, **-3.26 ms/f = -9.9%**, over-budget frames 46.9% -> 30.9%,
// 52/64 buckets favour ON, and the per-run means do not overlap (OFF 33.64/32.79/32.56 vs
// ON 30.35/29.70/29.17). `[G290:probe]`: failed flushes fell to 1,267 of 184,840 (0.7%) and the
// publication bucket from 8,015 to 676 ms. Four deterministic pixel gates clean — see the fix log.
//
// Levers: DC2_G569_NO_RTTWIDE=1 is the rollback · DC2_G569_RTTWIDE_MASK=<hex> arms a subset
// (bit i = slot 8+i, e.g. 0x3B drops 0x14b) · DC2_G569_RTTWIDE=1 is now redundant.
//
// ⛔ NOT gated by a DC2_G419_AB arm, deliberately. Residency is not a per-frame speed knob: a target
// admitted while arm=1 leaves GPU-owned rows in g_g261Res that an arm=0 window would never
// materialize, i.e. the two arms would render from different authorities and the "control" frames
// would show stale pixels. This lever must be measured with alternating WHOLE RUNS.
static uint32_t g569RttWideMask()
{
    static const uint32_t mask = []() -> uint32_t {
        if (envFlagEnabled("DC2_G569_NO_RTTWIDE"))
            return 0u;
        const char *m = std::getenv("DC2_G569_RTTWIDE_MASK");
        if (m && *m)
            return static_cast<uint32_t>(std::strtoul(m, nullptr, 16)) & 0x3Fu;
        // ⛔ G569 DEMOTED TO DEFAULT-OFF (user-verified visual defect, 2026-08-10). Admitting these
        // targets produces a GRAPHICAL BUG on every player jump/dash; DC2_G569_NO_RTTWIDE=1 fixes
        // it. The mechanism is the `[G261:invariant] escaped writer` storm this file's header
        // already described as a PERF cost (2,505/run on route2, 0 with the rollback) — an escaped
        // guest write to a page the GPU owns is a CORRECTNESS event first and a materialize cost
        // second, and the escape rate spikes exactly on the animation changes a jump/dash causes.
        // Re-arming requires fixing the escape, not re-running the gate.
        return envFlagEnabled("DC2_G569_RTTWIDE") ? 0x3Fu : 0u;
    }();
    return mask;
}
// slot 8..13 -> bit 0..5. Immutable for the process lifetime (see the ⛔ note above).
static inline int g569WideSlot(uint32_t bit, int slot)
{
    return (g569RttWideMask() & (1u << bit)) != 0u ? slot : -1;
}

static int g248TargetIndex(uint32_t fbp)
{
    if (g400NoResident(fbp))
        return -1;
    switch (fbp)
    {
    case 0x139u: return 0;
    case 0x13cu: return 1;
    case 0x143u: return 2;
    case 0x146u: return 3;
    case 0x155u: return 4;
    // G569 promoted slots 5/6/7 to DEFAULT-ON. They shipped as `envFlagEnabled` opt-ins under
    // G553/G556/G557 and nothing in the tree ever set them, so the `s05` 95.8 -> 30.4 ms/f headline
    // in the docs had been describing a flags-ON run for six phases (G568 caught the discrepancy).
    // Measured properly on `s05`, route-ALIGNED (bucketed by scriptFrame, matched sample counts,
    // 3 whole runs per arm, 14,106 frames each): 39.74 -> 31.01 ms/f, **-8.73 ms/f = -22.0%**, with
    // non-overlapping per-run means (39.77/39.68/39.77 vs 30.86/31.51/30.66).
    case 0x159u:
    {
        static const bool g553On = !envFlagEnabled("DC2_G553_NO_RTT159") &&
                                   !envFlagDisabled("DC2_G553_RTT159");
        return g553On ? 5 : -1;
    }
    case 0x181u:
    {
        static const bool g556On = !envFlagEnabled("DC2_G556_NO_RTT181") &&
                                   !envFlagDisabled("DC2_G556_RTT181");
        return g556On ? 6 : -1;
    }
    case 0x13au:
    {
        static const bool g557On = !envFlagEnabled("DC2_G557_NO_RTT13A") &&
                                   !envFlagDisabled("DC2_G557_RTT13A");
        return g557On ? 7 : -1;
    }
    // G569 route3 targets, ordered by CPU-replayed ENTRY weight. Mask bit = slot - 8.
    case 0x13du: return g569WideSlot(0u,  8);
    case 0x157u: return g569WideSlot(1u,  9);
    case 0x14bu: return g569WideSlot(2u, 10); // ⚠️ open Dungeon-6 alias family — bisect me first
    case 0x151u: return g569WideSlot(3u, 11);
    case 0x13fu: return g569WideSlot(4u, 12);
    case 0x141u: return g569WideSlot(5u, 13);
    default:     return -1;
    }
}

// ===== G526: FAST-PATH ADMISSION BY PROPERTY, NOT BY ADDRESS (PROMOTED DEFAULT-ON) =====
// Rollback: DC2_G526_NO_ANYTGT=1 (or DC2_G526_ANYTGT=0). Oracle: DC2_G248_VERIFY=1 -> [G248:vfy].
// Gate: Georama 46.26 -> 37.72 ms/f, mean -8.543 ms/f = -18.47%, 4/4 paired runs negative;
// per-pixel oracle 263,192,576 texels compared on the newly admitted targets, bad=0.
//
// G526 attributed Georama's 31.2 ms/f "per-draw residual" — the quantity G525 ranked eleven scenes
// by and could not explain with any workload term — to ~20 primitives per FRAME at ~1.4 ms each,
// 100% CPU-occupied (cyc/ns 3.18 against a 3.15 GHz core, so G524's blocked-span discount does not
// apply). Every one of them is a sprite to 0x181/0x18d/0x19f/0x1b7/0x1d7: five off-screen render
// targets this table does not name. The "0.24 -> 8.90 us per draw" spread was an average over a
// bimodal population — 99.7% of draws cost 0.06–0.36 us and a handful cost 300–1,450 us.
//
// The port gates THREE independent fast paths on this one five-address list, discovered on a
// single route: deferral admission (g252RttTarget), the G260 command graph (g260RttTri), and the
// G248 fast sprite sampler. A scene whose maps render to different addresses falls onto the
// slowest path in all three at once, and nothing in the census makes that visible.
//
// This predicate widens the narrowest and safest of the three: the SAMPLER. g248FastSample reads
// tex.* and guest VRAM and never reads frame.fbp — a destination address cannot change which texel
// a sampler returns — so the target test there was G248's measurement SCOPE, not a safety
// condition. The genuine safety conjuncts at that call site (fst, CT32/CT24/T8 psm, tbw != 0, no
// diagnostic trace, no special filter) are untouched, and per-pixel exactness against the very
// body it replaces is provable in-process with DC2_G248_VERIFY=1 ([G248:vfy] bad must stay 0).
//
// ⛔ Deferral admission and the G260 graph are deliberately NOT widened here. Both would need a
// residency and write-generation-publication story for a target with no slot in the fixed 5-entry
// G261 table (g_g261Res / g_g280ContentGen / g_g310TargetPageGen are all sized kG248TargetCount),
// and the three deferred-publication sites all filter on this same index — so admitting a target
// to deferral without giving it publication leaves a GPU-cached texture decoded from its pages
// stale. That is a separate phase with its own parity gate.
static bool g526AnyTargetSamplerOn()
{
    static const bool on = (std::getenv("DC2_G526_NO_ANYTGT") == nullptr) &&
                           !envFlagDisabled("DC2_G526_ANYTGT");
    return on;
}

static bool g526FastSpriteTarget(uint32_t fbp)
{
    return g248TargetIndex(fbp) >= 0 || g526AnyTargetSamplerOn();
}

// G255 (DEFAULT OFF): promote the already-isolated G252 RTT batches through the existing
// all-or-nothing G178 GPU backend without crossing any upload, local-copy, or FBP boundary.
// The important RTT ownership rule is that a GPU color write must advance the same page
// generations used by the texture cache; otherwise a later display composite can legally reuse
// the pre-RTT texture.  DC2_G255_VERIFY adds an exact same-run oracle: save the pre-GPU target,
// capture the GPU result, restore the target, run the proven CPU replay, then compare every pixel.
// G261 (DEFAULT ON since G266; stack kill DC2_G26X_NO_NATIVE=1): GPU-resident RTT waves on the G260 command-graph
// substrate. RTT batches render through the G255 backend but SKIP the per-flush
// readback+swizzle-writeback; the display composite samples the producer's FBO texture directly;
// guest VRAM materializes only at a real CPU-consumer edge (CPU band replay overlap, inline
// primitive overlap, transfer overlap, local→host read). Ownership contract: every CPU write to
// a resident target's pages MUST pass a g261 materialize hook first; an escaped writer trips the
// gen invariant and residency is dropped loudly (VRAM stays authoritative), never read back over
// newer guest bytes. Implies DC2_G260_NR and the G255 RTT routing; declines under the exact
// oracles and the separate G242/G256 ownership arms (their contracts assume per-flush readback).
static bool g261WaveOn()
{
    static const bool on = []() {
        // G266 PROMOTION (2026-07-16): the native-renderer wave stack (G260 graph + G261 waves +
        // G262 widening + G263 bilinear bind + G264 upload mirror + G265 display-Z + G266 fbw
        // split) is DEFAULT-ON after the full G253 promotion matrix passed on the final binary.
        // Rollback: DC2_G26X_NO_NATIVE=1 kills the whole stack; DC2_G261_NO_WAVE=1 kills waves
        // only (NR then follows its own DC2_G260_NR flag). DC2_G261_WAVE=1 stays accepted (it is
        // now redundant); DC2_G261_WAVE=0 also disables.
        if (envFlagEnabled("DC2_G26X_NO_NATIVE") || envFlagEnabled("DC2_G261_NO_WAVE") ||
            envFlagDisabled("DC2_G261_WAVE"))
            return false;
        // Exact same-run oracles compare VRAM after every flush — residency would break their
        // contract, so they force the readback path (G255 semantics) instead of engaging waves.
        if ((envFlagEnabled("DC2_G255_VERIFY") &&
             !envFlagEnabled("DC2_G553_VERIFY_HEAVY") &&
             !envFlagEnabled("DC2_G557_VERIFY_HEAVY")) ||
            envFlagEnabled("DC2_G248_VERIFY"))
            return false;
        // Separate architecture arms with their own ownership models.
        if (envFlagEnabled("DC2_G242_GPU_DEPTH") || envFlagEnabled("DC2_G256_EXACT_RTT"))
            return false;
        // Waves ride the G144 capture machinery and the G178 backend.
        if (envFlagEnabled("DC2_G144_NO_TILEBIN") || envFlagDisabled("DC2_G144_TILEBIN"))
            return false;
        if (envFlagEnabled("DC2_G178_NO_GPU") || envFlagDisabled("DC2_G178_GPU"))
            return false;
        // Diagnostics that read guest VRAM outside the materialize hooks (capture-time texture
        // decode, raw-VRAM dumps) keep the readback-per-flush contract instead.
        if (std::getenv("DC2_G149_TEXCACHE") != nullptr ||
            std::getenv("DC2_G38_VRAMDUMP") != nullptr)
            return false;
        return true;
    }();
    return on;
}

static bool g255GpuRttOn()
{
    // G256 is the exact-arithmetic successor arm and implicitly selects the same proven G255
    // dependency/eligibility path.  Requiring two environment switches made it too easy to run
    // the exact shader without ever exercising it.
    // G261 waves route the same five targets through the same backend path (with readback
    // deferred to real consumer edges), so the wave lever implies this gate too.
    static const bool on = envFlagEnabled("DC2_G255_GPU_RTT") ||
                           envFlagEnabled("DC2_G256_EXACT_RTT") ||
                           g261WaveOn();
    return on;
}

static bool g256ExactRttOn()
{
    static const bool on = envFlagEnabled("DC2_G256_EXACT_RTT");
    return on;
}

// ===== G262 RTT-family classifier widening (default-on since G266; requires waves) =====
// The G262 census split G261's rttRej(classify=1836): ~89% is the universal-Z depth reject
// INSIDE g178ClassifyEntry (character-body triangles: zte=1 ztst=GEQUAL zmsk=0 zpsm=Z24 —
// G261's wave-level "depth=0" counter was unreachable because it required DC2_G242_GPU_DEPTH),
// ~11% an additive (Cs-0)*(FIX=128)>>7+Cd blend whose entries also Z-test. Widening therefore
// means: per-wave NON-PERSISTENT guest depth (fresh Z-window upload each wave + immediate
// readback after execution — no cross-flush GPU Z ownership, so the G242/G249 dirty-generation
// trap cannot occur by construction) plus the additive blend mode, both scoped to the five RTT
// targets only. Riding the wave lever inherits every verify/exact/G242/dump decline.
static bool g262WideOn()
{
    // G266 promotion: default-on when waves are on. Kill: DC2_G262_NO_WIDE=1 (or the wave/stack
    // kills upstream). DC2_G262_WIDE=1 stays accepted (redundant); =0 disables.
    static const bool on = !envFlagEnabled("DC2_G262_NO_WIDE") &&
                           !envFlagDisabled("DC2_G262_WIDE") && g261WaveOn();
    return on;
}

// ===== G265 display-batch non-persistent guest depth (opt-in) =====
// Reuse G262's wave-granular Z upload/readback for display targets while keeping color on the
// normal G178 readback path. VRAM remains authoritative for both color and Z after every batch;
// unlike G242, this creates no persistent display-depth ownership to escape across CPU writers.
static bool g265DisplayZWaveOn()
{
    // G266 promotion: default-on when waves+widening are on. Kill: DC2_G265_NO_DISPLAY_ZWAVE=1
    // (or the wave/stack kills upstream). DC2_G265_DISPLAY_ZWAVE=1 stays accepted; =0 disables.
    static const bool on = !envFlagEnabled("DC2_G265_NO_DISPLAY_ZWAVE") &&
                           !envFlagDisabled("DC2_G265_DISPLAY_ZWAVE") && g262WideOn();
    return on;
}

// G411: let the native wave backend own DC2's private shared ZBP=0xd0 surface.
// G405 moved this depth surface out of aliased GS VRAM so display clears and RTT draws share one
// owner without corrupting loading textures. The default bridge maps that private mirror to the
// backend's existing ZBP-keyed depth texture; the pre-G411 CPU-only path remains the rollback.
static bool g411SharedGpuDepthOn()
{
    static const bool on =
        !envFlagEnabled("DC2_G411_NO_GPU_SHARED_Z") &&
        !envFlagDisabled("DC2_G411_GPU_SHARED_Z") &&
        !envFlagEnabled("DC2_G404_LEGACY_DISPLAY_Z_SCOPE") &&
        !(envFlagEnabled("DC2_G242_GPU_DEPTH") &&
          !envFlagEnabled("DC2_G242_NO_GPU_DEPTH"));
    // G242 has a different cross-frame ownership contract. G411 deliberately rides the proven
    // G262/G278 within-drain path until that persistent mode gains an explicit private-mirror tag.
    return on && g261WaveOn();
}

static bool g411SharedGpuDepthStatOn()
{
    static const bool on = envFlagEnabled("DC2_G411_STAT");
    return on;
}

static bool g265CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G265_CENSUS");
    return on;
}

static bool g258TraceOn()
{
    static const bool on = envFlagEnabled("DC2_G258_TRACE");
    return on;
}

static bool g255VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G255_VERIFY");
    return on;
}

static bool g286VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G286_VERIFY");
    return on;
}

static bool g570Verify13dOn()
{
    static const bool on = envFlagEnabled("DC2_G570_VERIFY_13D");
    return on;
}

static bool g570Verify139On()
{
    static const bool on = envFlagEnabled("DC2_G570_VERIFY_139");
    return on;
}

// G583: same-run exact oracle for the 0x13b transient target WITH guest depth.
//
// G286's own 0x13b oracle is gated `!guestDepth` and therefore never fires on this population —
// G573 recorded exactly that (zero checks over 2.2 M admitted primitives). G570 already built the
// depth-carrying shape of this oracle for 0x13d; this reuses it verbatim, so the comparison arm
// is the SAME banded CPU replay that is authoritative today, for both colour and the G403/G411
// private Z mirror.
//
// Like G570's, the verifier IMPLIES its path (see g583Trn13bWaveOn) so it can never end up
// comparing the CPU replay against itself and reporting a vacuous bad=0.
static bool g583Verify13bOn()
{
    static const bool on = envFlagEnabled("DC2_G583_VERIFY_TRN13B");
    return on;
}

// G586: exact same-run oracle for the non-persistent 0x141 contract.  Like G583's verifier it
// snapshots colour and the G403/G411 private Z mirror, stages the GPU result, restores both
// authorities, then lets the existing CPU band replay remain authoritative for comparison.
// g586Transient141On() treats this flag as an implication, preventing a vacuous CPU-vs-CPU pass.
static bool g586Verify141On()
{
    static const bool on = envFlagEnabled("DC2_G586_VERIFY_TRN141");
    return on;
}
// G587: same oracle contract, isolated to the residual untextured FBW=8 target at 0x15d.
static bool g587Verify15dOn()
{
    static const bool on = envFlagEnabled("DC2_G587_VERIFY_TRN15D");
    return on;
}

// G604: same-run exact oracle for the DISPLAY batches (fbp 0x0 / 0x68).
//
// G603 measured -8.27% end-to-end on route `s03` by admitting the one missing GS blend combo
// (`Cd - Cs*As/128`, mode 6) on the display targets, but could not CERTIFY exactness: every
// existing oracle's arming site is scoped to an RTT/transient target set the display buffers are
// in NONE of, and the generic G255 arm additionally excludes `guestDepth` outright. A verified
// run of `DC2_G255_VERIFY=1` on that route emitted ZERO `[G255:*]` lines — `bad=0` with
// `checks=0` is a failed run, not a pass (g589_oracle_with_zero_checks).
//
// The contract here is G586/G587's verbatim: a display batch's depth authority is the SAME
// G403/G411 private ZBP=0xd0/Z24 mirror those two snapshot and restore (g404SharedZScope admits
// exactly fbw=8/ZBP 0xd0/Z24 and the display FBPs), so g570StageDepthVerify is valid unchanged.
//
// ⚠️ READ `bigRgb`, NOT `big`. The display path stores GL-convention alpha (0..255) while the CPU
// band replay writes PS2's raw 0..128 framebuffer alpha: `G178Batch::rttRawAlpha` is asserted
// only for the tagged RTT/transient targets and never for a display batch. That divergence is a
// PRE-EXISTING property of every GPU-admitted display batch in the shipped default, not something
// any one admission introduces, so this oracle reports an RGB-restricted big/maxd beside the
// all-channel pair. The CONTROL is this same oracle with the candidate arm OFF, which then covers
// exactly the display batches the shipped binary already admits — a same-population, same-binary
// control (g515_parity_gate_needs_a_same_binary_control), strictly better than reading it against
// `[G570:verify139]`'s different target.
static bool g604VerifyDispOn()
{
    static const bool on = envFlagEnabled("DC2_G604_VERIFY_DISP");
    return on;
}

// G604: the same same-run oracle contract for the G527-DISCOVERED transient targets (ROADMAP
// Target 1, 47.8% of s03's replay wall). Like G570/G583/G586/G587's verifiers this flag IMPLIES
// its own path (g604DiscovTransientOn reads it), so the oracle can never end up comparing the CPU
// replay against itself and reporting a vacuous bad=0.
static bool g604VerifyDiscovOn()
{
    static const bool on = envFlagEnabled("DC2_G604_VERIFY_DISCOV");
    return on;
}
// ⛔ NOT a forward declaration of g604DiscovTransientTarget. ps2_gs_rasterizer.cpp nests TWO
// anonymous namespaces (rasterizer_headers_and_diagnostics.inc opens one, rasterizer_draw_prim_
// probe.inc opens a second that rasterizer_rtt_census_and_waves.inc closes), and this file sits in
// the OUTER one while rasterizer_command_graph.inc sits in the INNER. A declaration here would
// therefore name a DIFFERENT function from the definition there, and every call site that can see
// both is an ambiguous overload (g568_anon_namespace_include_window, one level deeper).
// The arming site publishes the answer instead, exactly like g_g604PendSubDraws below.
static bool g_g604PendIsDiscov = false;
// G604: the FBO-upload decision for this batch. A pixel the batch never draws must read
// back equal to the pre-batch destination; the discov oracle shows it reading back BLACK,
// which can only mean the FBO was not seeded from guest VRAM over that pixel.
static uint32_t g_g604PendUpload = 0u;
static std::atomic<uint64_t> g_g604DscChecks{0}, g_g604DscBad{0}, g_g604DscZBad{0},
    g_g604DscPixels{0}, g_g604DscBadBig{0}, g_g604DscMaxDelta{0}, g_g604DscBigRgb{0},
    g_g604DscMaxDeltaRgb{0};

// g265DisplayTarget lives in rasterizer_rtt_census_and_waves.inc, which this file PRECEDES in
// ps2_gs_rasterizer.cpp's include order. Same predicate; the arming site uses g265DisplayTarget.
static bool g604DisplayFbp(uint32_t fbp)
{
    return fbp == 0u || fbp == 0x68u;
}

static std::atomic<uint64_t> g_g604VerifyChecks{0}, g_g604VerifyBad{0},
    g_g604VerifyZBad{0}, g_g604VerifyPixels{0}, g_g604VerifyBadBig{0},
    g_g604VerifyMaxDelta{0}, g_g604VerifyBigRgb{0}, g_g604VerifyMaxDeltaRgb{0},
    g_g604VerifySubBatches{0}, g_g604VerifySubDraws{0};
// ⭐ THE GATE. The whole-population rate is NOT the number to read: this oracle's own noise floor
// on the SHIPPED display path is large (the CPU arm samples textures out of guest VRAM while the
// GPU arm samples the resident FBO family directly through `srcFbp`, so any residency skew shows
// up as a pixel difference that has nothing to do with the admission under test). Split the
// SAME RUN into the batches that carry a G603 blend-mode-6 draw and the batches that do not: both
// halves see identical residency, identical texture state and identical geometry, so the
// difference between their rates is the lever's own contribution and nothing else.
// (g515_parity_gate_needs_a_same_binary_control, taken one step further to a same-RUN control.)
static std::atomic<uint64_t> g_g604SubPixels{0}, g_g604SubBad{0}, g_g604SubBigRgb{0},
    g_g604SubZBad{0}, g_g604SubMaxDeltaRgb{0};
static std::atomic<uint64_t> g_g604NoSubPixels{0}, g_g604NoSubBad{0}, g_g604NoSubBigRgb{0},
    g_g604NoSubZBad{0}, g_g604NoSubMaxDeltaRgb{0};
// The residency-skew hypothesis needs its own counter, not an argument: a draw with srcFbp != 0
// samples another target's FBO on the GPU arm and that target's guest-VRAM bytes on the CPU arm.
// Splitting the no-sub population again by this tag says whether the floor is that, or the port's
// ordinary sampler/blend rounding.
static std::atomic<uint64_t> g_g604FboSrcPixels{0}, g_g604FboSrcBigRgb{0},
    g_g604PlainPixels{0}, g_g604PlainBigRgb{0};
// g538: a null oracle result is worthless without a counter proving the two arms took different
// code paths. The arming site publishes this batch's blend-mode-6 draw count here just before
// g255BeginVerify; the compare arm folds it into the per-check line and the cumulative totals.
static uint32_t g_g604PendSubDraws = 0u;
static uint32_t g_g604PendFboSrcDraws = 0u;
// The mode-6 draw's own GS state, recovered at the arming site. A blend-exactness argument has to
// be made against ALPHA/TEX0/TFX/TCC/filter, not against the classified summary.
static uint32_t g_g604PendSubVerts = 0u, g_g604PendSubTfx = 0u, g_g604PendSubTcc = 0u,
    g_g604PendSubBilinear = 0u, g_g604PendSubTexKey = 0u, g_g604PendSubSrcFbp = 0u,
    g_g604PendSubTpsm = 0u, g_g604PendSubTme = 0u;
static uint64_t g_g604PendSubAlpha = 0u;
static int g_g604PendSubX0 = 0, g_g604PendSubX1 = -1, g_g604PendSubY0 = 0, g_g604PendSubY1 = -1;
// Pixels inside vs outside the mode-6 draws' own footprint, over SUB batches only. If the
// admission is what diverges, these two rates separate; if the batch is merely a hard one, they
// do not. (The oracle's own version of g601_finder_is_not_a_gate: scope the finder.)
static std::atomic<uint64_t> g_g604InBoxPixels{0}, g_g604InBoxBigRgb{0},
    g_g604OutBoxPixels{0}, g_g604OutBoxBigRgb{0}, g_g604InBoxMaxDRgb{0};
// ⛔ THE BOX IS NOT A CLEAN ATTRIBUTION ON ITS OWN. The mode-6 draw's footprint is the burning
// wreck, and the SAME region carries the effect's ADDITIVE (mode 1/2) draws — the worst-pixel
// dump shows both arms ADDING 100-240 there, one saturating at 255 and the other not. So "inside
// the box diverges, outside does not" is equally consistent with "the fire is hard" as with
// "mode 6 is wrong". The control is the SAME REGION in the batches that carry no mode-6 draw:
// carry the last observed box forward and score NOSUB batches against it too.
static int g_g604LastBoxX0 = 0, g_g604LastBoxX1 = -1, g_g604LastBoxY0 = 0, g_g604LastBoxY1 = -1;
static std::atomic<uint64_t> g_g604NoSubInBoxPixels{0}, g_g604NoSubInBoxBigRgb{0},
    g_g604NoSubInBoxMaxDRgb{0};
static std::atomic<uint64_t> g_g583VerifyChecks{0}, g_g583VerifyBad{0},
    g_g583VerifyZBad{0}, g_g583VerifyPixels{0};
static std::atomic<uint64_t> g_g586VerifyChecks{0}, g_g586VerifyBad{0},
    g_g586VerifyZBad{0}, g_g586VerifyPixels{0}, g_g586VerifyBadBig{0},
    g_g586VerifyMaxDelta{0};
static std::atomic<uint64_t> g_g587VerifyChecks{0}, g_g587VerifyBad{0},
    g_g587VerifyZBad{0}, g_g587VerifyPixels{0}, g_g587VerifyBadBig{0},
    g_g587VerifyMaxDelta{0};
// G584: `bad` is a bit-equality count, so it cannot tell a WRONG pixel from a pixel that rounded
// the other way. The fixed-function GL blend evaluates ((A-B)*C)>>7 + D in float and the CPU
// replay in integers, so a +-1 LSB disagreement is the port's standing approximation on EVERY
// GPU-admitted blended draw, not a property of any one admission. Split it: `big` counts pixels
// whose worst channel differs by MORE than 1, which is the number a promotion gate should read.
static std::atomic<uint64_t> g_g584VerifyBadBig{0}, g_g584VerifyMaxDelta{0};
static std::atomic<uint64_t> g_g584Verify139BadBig{0}, g_g584Verify139Pixels{0};

struct G255VerifyState
{
    bool pending = false;
    uint32_t fbp = 0;
    uint32_t fbw = 0;
    uint32_t fbBlock = 0;
    int fbW = 0;
    int rowLo = 0;
    int rowHi = -1;
    size_t entries = 0;
    uint32_t prim = 0;
    uint32_t tme = 0;
    uint32_t abe = 0;
    uint64_t alpha = 0;
    uint32_t tbp = 0;
    uint32_t tpsm = 0;
    uint32_t tfx = 0;
    uint64_t tex1 = 0;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    uint16_t u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    uint64_t traceBatch = 0;
    std::vector<uint32_t> before;
    std::vector<uint32_t> gpu;
    bool g570Depth = false;
    int depthRowLo = 0;
    int depthRowHi = -1;
    std::vector<uint32_t> depthBefore;
    std::vector<uint32_t> depthGpu;
    uint32_t g604SubDraws = 0; // G604: blend-mode-6 draws in this batch (arm-divergence counter)
    uint32_t g604FboSrcDraws = 0; // G604: draws sampling another target's FBO (residency skew)
    bool g604IsDiscov = false;    // G604: this batch is a G527-discovered transient target
    uint32_t g604Upload = 0u;     // G604: bit0 uploadFb, bit1 forceNoUpload, bit2 snapValid
    uint32_t g604SubVerts = 0, g604SubTfx = 0, g604SubTcc = 0, g604SubBilinear = 0;
    uint32_t g604SubTexKey = 0, g604SubSrcFbp = 0, g604SubTpsm = 0, g604SubTme = 0;
    uint64_t g604SubAlpha = 0;
    int g604SubX0 = 0, g604SubX1 = -1, g604SubY0 = 0, g604SubY1 = -1;
};

static G255VerifyState g_g255Verify;
static std::atomic<uint64_t> g_g255Attempts[kG248TargetCount]{};
static std::atomic<uint64_t> g_g255Gpu[kG248TargetCount]{};
static std::atomic<uint64_t> g_g255Checks[kG248TargetCount]{};
static std::atomic<uint64_t> g_g255Bad[kG248TargetCount]{};
static std::atomic<uint64_t> g_g286VerifyChecks{0}, g_g286VerifyBad{0};
static std::atomic<uint64_t> g_g570VerifyChecks{0}, g_g570VerifyBad{0};
static std::atomic<uint64_t> g_g570Verify139Checks{0}, g_g570Verify139Bad{0};

static bool g553HeavyVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G553_VERIFY_HEAVY");
    return on;
}

static bool g557HeavyVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G557_VERIFY_HEAVY");
    return on;
}

static bool g553VerifySelected(uint32_t fbp)
{
    // s05's early 0x159 batches are also one-entry batches. Activate by observed route progress,
    // not shape: the measured dense cutscene is well established after attempt 10,000.
    return g553HeavyVerifyOn() && fbp == 0x159u &&
           g_g255Attempts[5].load(std::memory_order_relaxed) >= 10000u;
}

static bool g557VerifySelected(uint32_t fbp)
{
    // G557's 0x13a population is already the compact dense batch the lever targets. Narrowing
    // from its first occurrence avoids the unrelated legacy 0x139 sampler-rounding differences
    // while checking every GPU batch admitted by the new slot.
    return g557HeavyVerifyOn() && fbp == 0x13au;
}

static void g255Report()
{
    static const bool s_stat = envFlagEnabled("DC2_G255_STAT") || g255VerifyOn();
    if (!s_stat)
        return;
    std::fprintf(stderr, "[G255:stat]");
    static constexpr uint32_t kFbp[kG248TargetCount] = {
        0x139u, 0x13cu, 0x143u, 0x146u, 0x155u, 0x159u, 0x181u, 0x13au
    };
    for (uint32_t i = 0; i < kG248TargetCount; ++i)
        std::fprintf(stderr, " %03x(a=%llu gpu=%llu chk=%llu bad=%llu)", kFbp[i],
                     static_cast<unsigned long long>(g_g255Attempts[i].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g255Gpu[i].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g255Checks[i].load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g255Bad[i].load(std::memory_order_relaxed)));
    std::fprintf(stderr, "\n");
}

static void g255NoteAttempt(uint32_t fbp)
{
    const int i = g248TargetIndex(fbp);
    if (i < 0)
        return;
    const uint64_t n = g_g255Attempts[i].fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % 256u) == 0u)
        g255Report();
}

static void g255BeginVerify(uint8_t *vram, uint32_t vramSize, uint32_t fbp, uint32_t fbw,
                            int fbW, int rowLo, int rowHi,
                            bool g570Depth = false, int depthRowLo = 0, int depthRowHi = -1)
{
    g_g255Verify.pending = false;
    g_g255Verify.g570Depth = false;
    g_g255Verify.depthBefore.clear();
    g_g255Verify.depthGpu.clear();
    // G553 diagnostic: reach s05's heavy triangle population without comparing a full 512x416
    // target after every one-entry sprite batch. Existing G255 verification is unchanged unless
    // explicitly narrowed; clearing both buffers makes the later staging hook fail closed.
    if ((g553HeavyVerifyOn() || g557HeavyVerifyOn()) &&
        !g553VerifySelected(fbp) && !g557VerifySelected(fbp))
    {
        g_g255Verify.before.clear();
        g_g255Verify.gpu.clear();
        return;
    }
    if (!(g255VerifyOn() || (g286VerifyOn() && fbp == 0x13bu) ||
          (g583Verify13bOn() && fbp == 0x13bu) ||
          (g586Verify141On() && fbp == 0x141u) ||
          (g587Verify15dOn() && fbp == 0x15du) ||
          (g570Verify13dOn() && fbp == 0x13du) ||
          (g570Verify139On() && fbp == 0x139u) ||
          (g604VerifyDispOn() && g604DisplayFbp(fbp)) ||
          (g604VerifyDiscovOn() && g_g604PendIsDiscov)) ||
        !vram || rowHi < rowLo)
        return;
    G255VerifyState &s = g_g255Verify;
    s.g604SubDraws = g_g604PendSubDraws; // published by the arming site, just above the call
    s.g604FboSrcDraws = g_g604PendFboSrcDraws;
    s.g604IsDiscov = g_g604PendIsDiscov;
    s.g604Upload = g_g604PendUpload;
    s.g604SubVerts = g_g604PendSubVerts;
    s.g604SubTfx = g_g604PendSubTfx;
    s.g604SubTcc = g_g604PendSubTcc;
    s.g604SubBilinear = g_g604PendSubBilinear;
    s.g604SubTexKey = g_g604PendSubTexKey;
    s.g604SubSrcFbp = g_g604PendSubSrcFbp;
    s.g604SubTpsm = g_g604PendSubTpsm;
    s.g604SubTme = g_g604PendSubTme;
    s.g604SubAlpha = g_g604PendSubAlpha;
    if (s.g604SubDraws != 0u && g_g604PendSubX1 >= g_g604PendSubX0)
    {
        g_g604LastBoxX0 = g_g604PendSubX0; g_g604LastBoxX1 = g_g604PendSubX1;
        g_g604LastBoxY0 = g_g604PendSubY0; g_g604LastBoxY1 = g_g604PendSubY1;
    }
    // Every display batch is scored against the last observed mode-6 footprint, so the SUB and
    // NOSUB in-region rates are directly comparable.
    s.g604SubX0 = g_g604LastBoxX0; s.g604SubX1 = g_g604LastBoxX1;
    s.g604SubY0 = g_g604LastBoxY0; s.g604SubY1 = g_g604LastBoxY1;
    s.fbp = fbp;
    s.fbw = fbw;
    s.fbBlock = GSInternal::framePageBaseToBlock(fbp);
    s.fbW = fbW;
    s.rowLo = rowLo;
    s.rowHi = rowHi;
    s.entries = g_g144List.size();
    if (g258TraceOn() && fbp == 0x146u)
    {
        static uint64_t s_g258Batch = 0;
        s.traceBatch = ++s_g258Batch;
    }
    else
    {
        s.traceBatch = 0;
    }
    if (!g_g144List.empty())
    {
        const G144Entry &e = g_g144List.front();
        s.prim = static_cast<uint32_t>(e.prim.type);
        s.tme = e.prim.tme ? 1u : 0u;
        s.abe = e.prim.abe ? 1u : 0u;
        s.alpha = e.ctx.alpha;
        s.tbp = e.ctx.tex0.tbp0;
        s.tpsm = e.ctx.tex0.psm;
        s.tfx = e.ctx.tex0.tfx;
        s.tex1 = e.ctx.tex1;
        s.x0 = e.v[0].x; s.y0 = e.v[0].y;
        s.x1 = e.v[1].x; s.y1 = e.v[1].y;
        s.u0 = e.v[0].u; s.v0 = e.v[0].v;
        s.u1 = e.v[1].u; s.v1 = e.v[1].v;
    }
    const size_t count = static_cast<size_t>(rowHi - rowLo + 1) * static_cast<size_t>(fbW);
    s.before.resize(count);
    s.gpu.resize(count);
    for (int y = rowLo; y <= rowHi; ++y)
        for (int x = 0; x < fbW; ++x)
        {
            const uint32_t off = GSPSMCT32::addrPSMCT32(
                s.fbBlock, fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            uint32_t px = 0;
            if (off + 4u <= vramSize)
                std::memcpy(&px, vram + off, 4);
            s.before[static_cast<size_t>(y - rowLo) * fbW + x] = px;
        }
    s.g570Depth = g570Depth && depthRowHi >= depthRowLo;
    s.depthRowLo = depthRowLo;
    s.depthRowHi = depthRowHi;
    if (s.g570Depth)
    {
        const size_t depthCount =
            static_cast<size_t>(depthRowHi - depthRowLo + 1) * static_cast<size_t>(fbW);
        s.depthBefore.resize(depthCount);
        s.depthGpu.resize(depthCount);
        for (int y = depthRowLo; y <= depthRowHi; ++y)
            for (int x = 0; x < fbW; ++x)
                s.depthBefore[static_cast<size_t>(y - depthRowLo) * fbW + x] =
                    g403DisplayZRead(x, y, fbp);
    }
}

static void g255StageGpuVerify(uint8_t *vram, uint32_t vramSize,
                               const std::vector<uint32_t> &readback, int fbH)
{
    G255VerifyState &s = g_g255Verify;
    if (!(g255VerifyOn() || (g286VerifyOn() && s.fbp == 0x13bu) ||
          (g583Verify13bOn() && s.fbp == 0x13bu) ||
          (g586Verify141On() && s.fbp == 0x141u) ||
          (g587Verify15dOn() && s.fbp == 0x15du) ||
          (g570Verify13dOn() && s.fbp == 0x13du) ||
          (g570Verify139On() && s.fbp == 0x139u) ||
          (g604VerifyDispOn() && g604DisplayFbp(s.fbp)) ||
          (g604VerifyDiscovOn() && s.g604IsDiscov)) ||
        !vram || s.before.empty() ||
        readback.size() != static_cast<size_t>(s.fbW) * static_cast<size_t>(fbH))
        return;
    for (int y = s.rowLo; y <= s.rowHi; ++y)
        for (int x = 0; x < s.fbW; ++x)
        {
            const size_t i = static_cast<size_t>(y - s.rowLo) * s.fbW + x;
            const size_t gpuI = static_cast<size_t>(fbH - 1 - y) * s.fbW + x;
            s.gpu[i] = readback[gpuI];
            const uint32_t off = GSPSMCT32::addrPSMCT32(
                s.fbBlock, s.fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            if (off + 4u <= vramSize)
                std::memcpy(vram + off, &s.before[i], 4);
        }
    s.pending = true;
}

static void g570StageDepthVerify()
{
    G255VerifyState &s = g_g255Verify;
    // G583/G586 ride this unchanged: their depth authority is the same G403/G411 private mirror,
    // so the snapshot/restore pair is identical for 0x13b, 0x13d and 0x141.
    // G604 rides this unchanged for the same reason: a display batch's live Z is the SAME
    // G403/G411 private mirror, so the snapshot/restore pair is identical for 0x0/0x68.
    if (!(g570Verify13dOn() || g583Verify13bOn() || g586Verify141On() ||
          g587Verify15dOn() || g604VerifyDispOn() || g604VerifyDiscovOn()) ||
        !s.pending || !s.g570Depth ||
        s.depthBefore.size() != s.depthGpu.size())
        return;
    for (int y = s.depthRowLo; y <= s.depthRowHi; ++y)
        for (int x = 0; x < s.fbW; ++x)
        {
            const size_t i = static_cast<size_t>(y - s.depthRowLo) * s.fbW + x;
            s.depthGpu[i] = g403DisplayZRead(x, y, s.fbp);
            g403DisplayZWrite(x, y, s.fbp, s.depthBefore[i]);
        }
}

static void g255CompareCpuVerify(uint8_t *vram, uint32_t vramSize)
{
    G255VerifyState &s = g_g255Verify;
    if (!s.pending || !vram)
        return;
    uint64_t bad = 0;
    uint64_t badRgb = 0;
    uint64_t badAlpha = 0;
    uint64_t coverage = 0;
    uint64_t depthBad = 0;
    uint64_t badBig = 0;      // G584: worst channel off by more than 1 LSB
    unsigned maxDelta = 0;    // G584: worst per-channel delta seen in this batch
    // G604: the same two numbers restricted to R/G/B. A display batch's ALPHA convention differs
    // between the two arms by construction (the GL path is not tagged rttRawAlpha, so it stores
    // 0..255 where the CPU replay stores PS2's raw 0..128) — an all-channel `big` on that
    // population measures that standing convention, not the admission under test.
    uint64_t badBigRgb = 0;
    unsigned maxDeltaRgb = 0;
    // G604: `firstBig` is dominated by the standing ALPHA convention (an exact 2x), so it never
    // shows the RGB defect. Sample the first pixel whose RGB is off by more than 1, and carry the
    // SIGNED mean RGB delta — a subtractive-blend error has a sign, rounding does not.
    int firstBigRgbX = -1, firstBigRgbY = -1;
    uint32_t firstBigRgbCpu = 0, firstBigRgbGpu = 0, firstBigRgbDst = 0;
    long long sumDeltaRgb = 0;
    // G604 attribution: same two counts, split by the mode-6 draws' footprint.
    const bool g604HaveBox = s.g604SubX1 >= s.g604SubX0 && s.g604SubY1 >= s.g604SubY0;
    uint64_t inBoxPx = 0, inBoxBig = 0, outBoxPx = 0, outBoxBig = 0;
    unsigned inBoxMaxD = 0;
    int firstBigX = -1, firstBigY = -1;
    uint32_t firstBigCpu = 0, firstBigGpu = 0;
    int firstX = -1, firstY = -1;
    uint32_t firstCpu = 0, firstGpu = 0;
    for (int y = s.rowLo; y <= s.rowHi; ++y)
        for (int x = 0; x < s.fbW; ++x)
        {
            const size_t i = static_cast<size_t>(y - s.rowLo) * s.fbW + x;
            const uint32_t off = GSPSMCT32::addrPSMCT32(
                s.fbBlock, s.fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            uint32_t cpu = 0;
            if (off + 4u <= vramSize)
                std::memcpy(&cpu, vram + off, 4);
            const bool g604InBox = g604HaveBox && x >= s.g604SubX0 && x <= s.g604SubX1 &&
                                   y >= s.g604SubY0 && y <= s.g604SubY1;
            if (g604HaveBox)
            {
                if (g604InBox) ++inBoxPx; else ++outBoxPx;
            }
            if (cpu != s.gpu[i])
            {
                if (bad == 0)
                {
                    firstX = x; firstY = y; firstCpu = cpu; firstGpu = s.gpu[i];
                }
                ++bad;
                if ((cpu & 0x00FFFFFFu) != (s.gpu[i] & 0x00FFFFFFu))
                    ++badRgb;
                if ((cpu & 0xFF000000u) != (s.gpu[i] & 0xFF000000u))
                    ++badAlpha;
                if (((cpu & 0x00FFFFFFu) == 0u) != ((s.gpu[i] & 0x00FFFFFFu) == 0u))
                    ++coverage;
                unsigned worst = 0u;
                unsigned worstRgb = 0u; // G604: channels 0..2 only
                for (int ch = 0; ch < 4; ++ch)
                {
                    const int ca = static_cast<int>((cpu >> (ch * 8)) & 0xFFu);
                    const int ga = static_cast<int>((s.gpu[i] >> (ch * 8)) & 0xFFu);
                    const unsigned d =
                        static_cast<unsigned>(ca > ga ? ca - ga : ga - ca);
                    if (d > worst)
                        worst = d;
                    if (ch < 3 && d > worstRgb)
                        worstRgb = d;
                }
                if (worst > maxDelta)
                    maxDelta = worst;
                if (worstRgb > maxDeltaRgb)
                    maxDeltaRgb = worstRgb;
                if (worstRgb > 1u)
                {
                    // The WORST pixel, not the first: scan order finds a 2-LSB pixel long before
                    // the one that carries the defect's magnitude.
                    if (worstRgb >= maxDeltaRgb)
                    {
                        firstBigRgbX = x; firstBigRgbY = y;
                        firstBigRgbCpu = cpu; firstBigRgbGpu = s.gpu[i];
                        // s.before IS the pre-batch destination (Cd). With it, each arm's actual
                        // subtraction is readable directly instead of inferred.
                        firstBigRgbDst = s.before[i];
                    }
                    ++badBigRgb;
                    for (int ch = 0; ch < 3; ++ch)
                        sumDeltaRgb += static_cast<int>((s.gpu[i] >> (ch * 8)) & 0xFFu) -
                                       static_cast<int>((cpu >> (ch * 8)) & 0xFFu);
                    if (g604HaveBox)
                    {
                        if (g604InBox)
                        {
                            ++inBoxBig;
                            if (worstRgb > inBoxMaxD)
                                inBoxMaxD = worstRgb;
                        }
                        else
                        {
                            ++outBoxBig;
                        }
                    }
                }
                if (worst > 1u)
                {
                    if (badBig == 0u)
                    {
                        firstBigX = x; firstBigY = y;
                        firstBigCpu = cpu; firstBigGpu = s.gpu[i];
                    }
                    ++badBig;
                }
            }
        }
    if (s.g570Depth)
        for (int y = s.depthRowLo; y <= s.depthRowHi; ++y)
            for (int x = 0; x < s.fbW; ++x)
            {
                const size_t i = static_cast<size_t>(y - s.depthRowLo) * s.fbW + x;
                if (g403DisplayZRead(x, y, s.fbp) != s.depthGpu[i])
                    ++depthBad;
            }
    const int ti = g248TargetIndex(s.fbp);
    if (g570Verify139On() && s.fbp == 0x139u)
    {
        const uint64_t n =
            g_g570Verify139Checks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad != 0u)
            g_g570Verify139Bad.fetch_add(bad, std::memory_order_relaxed);
        // ⛔⛔ G604 CORRECTION — DO NOT USE THIS AS A PROMOTION CONTROL.
        //
        // G584 described this as "the CONTROL … an ALREADY-PROMOTED, GPU-admitted, BLENDED
        // population … so its big/maxd is the port's standing float-vs-integer rounding rate", and
        // ROADMAP G604 Target 0 then made "no worse than the standing [G570:verify139] control" the
        // promotion gate for a different lever. **That is wrong and it would have licensed a false
        // promotion.** This arm only ever runs under g570Verify139On(), which IMPLIES
        // g570Private139On() — a lever that is DEFAULT-OFF because `plans/phase-G570-fix-log.md`
        // REJECTED it: "treating the large 0x139 batch as a normal G178 GPU batch with the private
        // depth mirror still produced hundreds of color/alpha/coverage mismatches per batch", with
        // an explicit Do-NOT-Carry-Forward entry against re-running it as if its oracle had passed.
        //
        // G604 re-measured it on `s03` and reproduced exactly that: 181.9 M pixels, 470,390 bad
        // (0.259%), 378,294 off by more than 1 LSB (0.208%), max channel delta 64 — i.e. ~590 bad
        // pixels per batch. That is a REJECTED lever's error rate, not a rounding floor.
        //
        // ⇒ This port has NO certified GPU-admitted blended population to gate against. Build a
        // same-RUN control instead (G604 §1.3: the same screen region in batches that do not carry
        // the admission under test).
        //
        // The counters stay — the number is still the right thing to compare a NEW admission's
        // order of magnitude to, and G583/G584's residue analysis is still readable against it.
        if (badBig != 0u)
            g_g584Verify139BadBig.fetch_add(badBig, std::memory_order_relaxed);
        g_g584Verify139Pixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
        if (bad != 0u || n <= 8u || (n % 30u) == 0u)
            std::fprintf(stderr,
                         "[G570:verify139] n=%llu pixels=%zu bad=%llu rgb=%llu a=%llu cov=%llu "
                         "big=%llu maxd=%u rows=%d..%d entries=%zu prim=%u tme=%u tbp=0x%x "
                         "psm=0x%x first=(%d,%d cpu=%08x gpu=%08x) "
                         "cumulative(px=%llu bad=%llu big=%llu)\n",
                         (unsigned long long)n, s.gpu.size(),
                         (unsigned long long)bad, (unsigned long long)badRgb,
                         (unsigned long long)badAlpha, (unsigned long long)coverage,
                         (unsigned long long)badBig, maxDelta,
                         s.rowLo, s.rowHi, s.entries, s.prim, s.tme, s.tbp, s.tpsm,
                         firstX, firstY, firstCpu, firstGpu,
                         (unsigned long long)g_g584Verify139Pixels.load(std::memory_order_relaxed),
                         (unsigned long long)g_g570Verify139Bad.load(std::memory_order_relaxed),
                         (unsigned long long)g_g584Verify139BadBig.load(std::memory_order_relaxed));
    }
    else if (g586Verify141On() && s.fbp == 0x141u)
    {
        const uint64_t n =
            g_g586VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g586VerifyPixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
        if (bad != 0u)
            g_g586VerifyBad.fetch_add(bad, std::memory_order_relaxed);
        if (depthBad != 0u)
            g_g586VerifyZBad.fetch_add(depthBad, std::memory_order_relaxed);
        if (badBig != 0u)
            g_g586VerifyBadBig.fetch_add(badBig, std::memory_order_relaxed);
        {
            uint64_t prev = g_g586VerifyMaxDelta.load(std::memory_order_relaxed);
            while (maxDelta > prev &&
                   !g_g586VerifyMaxDelta.compare_exchange_weak(prev, maxDelta,
                                                               std::memory_order_relaxed))
            {
            }
        }
        if (bad != 0u || depthBad != 0u || n <= 8u || (n % 30u) == 0u)
            std::fprintf(stderr,
                         "[G586:verify141] n=%llu pixels=%zu bad=%llu rgb=%llu a=%llu cov=%llu "
                         "big=%llu maxd=%u zbad=%llu zrows=%d..%d rows=%d..%d entries=%zu "
                         "prim=%u tme=%u first=(%d,%d cpu=%08x gpu=%08x) "
                         "firstBig=(%d,%d cpu=%08x gpu=%08x) "
                         "cumulative(px=%llu bad=%llu big=%llu maxd=%llu zbad=%llu)\n",
                         (unsigned long long)n, s.gpu.size(),
                         (unsigned long long)bad, (unsigned long long)badRgb,
                         (unsigned long long)badAlpha, (unsigned long long)coverage,
                         (unsigned long long)badBig, maxDelta,
                         (unsigned long long)depthBad, s.depthRowLo, s.depthRowHi,
                         s.rowLo, s.rowHi, s.entries, s.prim, s.tme,
                         firstX, firstY, firstCpu, firstGpu,
                         firstBigX, firstBigY, firstBigCpu, firstBigGpu,
                         (unsigned long long)g_g586VerifyPixels.load(std::memory_order_relaxed),
                         (unsigned long long)g_g586VerifyBad.load(std::memory_order_relaxed),
                         (unsigned long long)g_g586VerifyBadBig.load(std::memory_order_relaxed),
                         (unsigned long long)g_g586VerifyMaxDelta.load(std::memory_order_relaxed),
                         (unsigned long long)g_g586VerifyZBad.load(std::memory_order_relaxed));
    }
    else if (g587Verify15dOn() && s.fbp == 0x15du)
    {
        const uint64_t n =
            g_g587VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g587VerifyPixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
        if (bad != 0u)
            g_g587VerifyBad.fetch_add(bad, std::memory_order_relaxed);
        if (depthBad != 0u)
            g_g587VerifyZBad.fetch_add(depthBad, std::memory_order_relaxed);
        if (badBig != 0u)
            g_g587VerifyBadBig.fetch_add(badBig, std::memory_order_relaxed);
        {
            uint64_t prev = g_g587VerifyMaxDelta.load(std::memory_order_relaxed);
            while (maxDelta > prev &&
                   !g_g587VerifyMaxDelta.compare_exchange_weak(prev, maxDelta,
                                                               std::memory_order_relaxed))
            {
            }
        }
        if (bad != 0u || depthBad != 0u || n <= 8u || (n % 30u) == 0u)
            std::fprintf(stderr,
                         "[G587:verify15d] n=%llu pixels=%zu bad=%llu rgb=%llu a=%llu cov=%llu "
                         "big=%llu maxd=%u zbad=%llu zrows=%d..%d rows=%d..%d entries=%zu "
                         "prim=%u tme=%u first=(%d,%d cpu=%08x gpu=%08x) "
                         "firstBig=(%d,%d cpu=%08x gpu=%08x) "
                         "cumulative(px=%llu bad=%llu big=%llu maxd=%llu zbad=%llu)\n",
                         (unsigned long long)n, s.gpu.size(),
                         (unsigned long long)bad, (unsigned long long)badRgb,
                         (unsigned long long)badAlpha, (unsigned long long)coverage,
                         (unsigned long long)badBig, maxDelta,
                         (unsigned long long)depthBad, s.depthRowLo, s.depthRowHi,
                         s.rowLo, s.rowHi, s.entries, s.prim, s.tme,
                         firstX, firstY, firstCpu, firstGpu,
                         firstBigX, firstBigY, firstBigCpu, firstBigGpu,
                         (unsigned long long)g_g587VerifyPixels.load(std::memory_order_relaxed),
                         (unsigned long long)g_g587VerifyBad.load(std::memory_order_relaxed),
                         (unsigned long long)g_g587VerifyBadBig.load(std::memory_order_relaxed),
                         (unsigned long long)g_g587VerifyMaxDelta.load(std::memory_order_relaxed),
                         (unsigned long long)g_g587VerifyZBad.load(std::memory_order_relaxed));
    }
    else if (g604VerifyDiscovOn() && s.g604IsDiscov)
    {
        // ROADMAP Target 1's gate. Same fields as G586/G587 — those two are the precedent this
        // contract generalizes, so their numbers are the reference this one is read against.
        const uint64_t n = g_g604DscChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g604DscPixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
        g_g604DscBad.fetch_add(bad, std::memory_order_relaxed);
        g_g604DscZBad.fetch_add(depthBad, std::memory_order_relaxed);
        g_g604DscBadBig.fetch_add(badBig, std::memory_order_relaxed);
        g_g604DscBigRgb.fetch_add(badBigRgb, std::memory_order_relaxed);
        {
            uint64_t p = g_g604DscMaxDelta.load(std::memory_order_relaxed);
            while (maxDelta > p &&
                   !g_g604DscMaxDelta.compare_exchange_weak(p, maxDelta,
                                                            std::memory_order_relaxed))
            {
            }
            p = g_g604DscMaxDeltaRgb.load(std::memory_order_relaxed);
            while (maxDeltaRgb > p &&
                   !g_g604DscMaxDeltaRgb.compare_exchange_weak(p, maxDeltaRgb,
                                                               std::memory_order_relaxed))
            {
            }
        }
        if (bad != 0u || depthBad != 0u || n <= 8u || (n % 60u) == 0u)
        {
            const uint64_t px = g_g604DscPixels.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[G604:verifydiscov] n=%llu fbp=0x%x pixels=%zu bad=%llu rgb=%llu "
                         "a=%llu cov=%llu big=%llu bigRgb=%llu maxd=%u maxdRgb=%u zbad=%llu "
                         "zrows=%d..%d rows=%d..%d entries=%zu prim=%u tme=%u up=%u "
                         "worstRgbPx=(%d,%d dst=%08x cpu=%08x gpu=%08x) "
                         "cumulative(px=%llu bad=%llu %.5f%% big=%llu %.5f%% bigRgb=%llu %.5f%% "
                         "maxd=%llu maxdRgb=%llu zbad=%llu)\n",
                         (unsigned long long)n, s.fbp, s.gpu.size(),
                         (unsigned long long)bad, (unsigned long long)badRgb,
                         (unsigned long long)badAlpha, (unsigned long long)coverage,
                         (unsigned long long)badBig, (unsigned long long)badBigRgb,
                         maxDelta, maxDeltaRgb, (unsigned long long)depthBad,
                         s.depthRowLo, s.depthRowHi, s.rowLo, s.rowHi, s.entries,
                         s.prim, s.tme, s.g604Upload,
                         firstBigRgbX, firstBigRgbY, firstBigRgbDst,
                         firstBigRgbCpu, firstBigRgbGpu,
                         (unsigned long long)px,
                         (unsigned long long)g_g604DscBad.load(std::memory_order_relaxed),
                         px ? 100.0 * (double)g_g604DscBad.load(std::memory_order_relaxed) / (double)px : 0.0,
                         (unsigned long long)g_g604DscBadBig.load(std::memory_order_relaxed),
                         px ? 100.0 * (double)g_g604DscBadBig.load(std::memory_order_relaxed) / (double)px : 0.0,
                         (unsigned long long)g_g604DscBigRgb.load(std::memory_order_relaxed),
                         px ? 100.0 * (double)g_g604DscBigRgb.load(std::memory_order_relaxed) / (double)px : 0.0,
                         (unsigned long long)g_g604DscMaxDelta.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604DscMaxDeltaRgb.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604DscZBad.load(std::memory_order_relaxed));
        }
    }
    else if (g604VerifyDispOn() && g604DisplayFbp(s.fbp))
    {
        // The display population G603 changed. Same fields as G586/G587 plus the RGB-restricted
        // pair and the blend-mode-6 draw count that proves the two arms diverged.
        const uint64_t n =
            g_g604VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g604VerifyPixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
        if (bad != 0u)
            g_g604VerifyBad.fetch_add(bad, std::memory_order_relaxed);
        if (depthBad != 0u)
            g_g604VerifyZBad.fetch_add(depthBad, std::memory_order_relaxed);
        if (badBig != 0u)
            g_g604VerifyBadBig.fetch_add(badBig, std::memory_order_relaxed);
        if (badBigRgb != 0u)
            g_g604VerifyBigRgb.fetch_add(badBigRgb, std::memory_order_relaxed);
        // ⭐ The same-run split. `sub` = this batch carries a G603 blend-mode-6 draw and therefore
        // exists on the GPU arm ONLY because of the admission under test.
        if (s.g604SubDraws != 0u)
        {
            g_g604VerifySubBatches.fetch_add(1u, std::memory_order_relaxed);
            g_g604VerifySubDraws.fetch_add(s.g604SubDraws, std::memory_order_relaxed);
            g_g604SubPixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
            g_g604SubBad.fetch_add(bad, std::memory_order_relaxed);
            g_g604SubBigRgb.fetch_add(badBigRgb, std::memory_order_relaxed);
            g_g604SubZBad.fetch_add(depthBad, std::memory_order_relaxed);
            g_g604InBoxPixels.fetch_add(inBoxPx, std::memory_order_relaxed);
            g_g604InBoxBigRgb.fetch_add(inBoxBig, std::memory_order_relaxed);
            g_g604OutBoxPixels.fetch_add(outBoxPx, std::memory_order_relaxed);
            g_g604OutBoxBigRgb.fetch_add(outBoxBig, std::memory_order_relaxed);
            {
                uint64_t q = g_g604InBoxMaxDRgb.load(std::memory_order_relaxed);
                while (inBoxMaxD > q &&
                       !g_g604InBoxMaxDRgb.compare_exchange_weak(q, inBoxMaxD,
                                                                 std::memory_order_relaxed))
                {
                }
            }
            uint64_t p = g_g604SubMaxDeltaRgb.load(std::memory_order_relaxed);
            while (maxDeltaRgb > p &&
                   !g_g604SubMaxDeltaRgb.compare_exchange_weak(p, maxDeltaRgb,
                                                               std::memory_order_relaxed))
            {
            }
        }
        else
        {
            g_g604NoSubPixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
            g_g604NoSubBad.fetch_add(bad, std::memory_order_relaxed);
            g_g604NoSubBigRgb.fetch_add(badBigRgb, std::memory_order_relaxed);
            g_g604NoSubZBad.fetch_add(depthBad, std::memory_order_relaxed);
            // The region control: same screen rectangle, batches with no mode-6 draw in them.
            g_g604NoSubInBoxPixels.fetch_add(inBoxPx, std::memory_order_relaxed);
            g_g604NoSubInBoxBigRgb.fetch_add(inBoxBig, std::memory_order_relaxed);
            {
                uint64_t q = g_g604NoSubInBoxMaxDRgb.load(std::memory_order_relaxed);
                while (inBoxMaxD > q &&
                       !g_g604NoSubInBoxMaxDRgb.compare_exchange_weak(q, inBoxMaxD,
                                                                      std::memory_order_relaxed))
                {
                }
            }
            uint64_t p = g_g604NoSubMaxDeltaRgb.load(std::memory_order_relaxed);
            while (maxDeltaRgb > p &&
                   !g_g604NoSubMaxDeltaRgb.compare_exchange_weak(p, maxDeltaRgb,
                                                                 std::memory_order_relaxed))
            {
            }
        }
        // Second split, orthogonal to the first: does the oracle's floor come from FBO-sourced
        // sampling (residency skew between the two arms) or from ordinary rounding?
        if (s.g604FboSrcDraws != 0u)
        {
            g_g604FboSrcPixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
            g_g604FboSrcBigRgb.fetch_add(badBigRgb, std::memory_order_relaxed);
        }
        else
        {
            g_g604PlainPixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
            g_g604PlainBigRgb.fetch_add(badBigRgb, std::memory_order_relaxed);
        }
        // One compact, greppable gate line. Everything a promotion decision needs is on it, so a
        // timeout-killed run still yields the verdict without parsing the per-batch stream.
        if ((n % 200u) == 0u)
        {
            const auto rate = [](uint64_t num, uint64_t den) {
                return den ? (100.0 * static_cast<double>(num) / static_cast<double>(den)) : 0.0;
            };
            const uint64_t subPx = g_g604SubPixels.load(std::memory_order_relaxed);
            const uint64_t noSubPx = g_g604NoSubPixels.load(std::memory_order_relaxed);
            const uint64_t fboPx = g_g604FboSrcPixels.load(std::memory_order_relaxed);
            const uint64_t plainPx = g_g604PlainPixels.load(std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[G604:gate] chk=%llu subBatches=%llu subDraws=%llu | "
                         "SUB(px=%llu bigRgb=%llu %.4f%% bad=%.3f%% zbad=%llu maxdRgb=%llu) | "
                         "NOSUB(px=%llu bigRgb=%llu %.4f%% bad=%.3f%% zbad=%llu maxdRgb=%llu) | "
                         "FBOSRC(px=%llu %.4f%%) PLAIN(px=%llu %.4f%%) | "
                         "SUB6BOX(sub-in px=%llu %.4f%% maxd=%llu / sub-out px=%llu %.4f%%) | "
                         "REGIONCTL(nosub-in px=%llu %.4f%% maxd=%llu)\n",
                         (unsigned long long)n,
                         (unsigned long long)g_g604VerifySubBatches.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604VerifySubDraws.load(std::memory_order_relaxed),
                         (unsigned long long)subPx,
                         (unsigned long long)g_g604SubBigRgb.load(std::memory_order_relaxed),
                         rate(g_g604SubBigRgb.load(std::memory_order_relaxed), subPx),
                         rate(g_g604SubBad.load(std::memory_order_relaxed), subPx),
                         (unsigned long long)g_g604SubZBad.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604SubMaxDeltaRgb.load(std::memory_order_relaxed),
                         (unsigned long long)noSubPx,
                         (unsigned long long)g_g604NoSubBigRgb.load(std::memory_order_relaxed),
                         rate(g_g604NoSubBigRgb.load(std::memory_order_relaxed), noSubPx),
                         rate(g_g604NoSubBad.load(std::memory_order_relaxed), noSubPx),
                         (unsigned long long)g_g604NoSubZBad.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604NoSubMaxDeltaRgb.load(std::memory_order_relaxed),
                         (unsigned long long)fboPx,
                         rate(g_g604FboSrcBigRgb.load(std::memory_order_relaxed), fboPx),
                         (unsigned long long)plainPx,
                         rate(g_g604PlainBigRgb.load(std::memory_order_relaxed), plainPx),
                         (unsigned long long)g_g604InBoxPixels.load(std::memory_order_relaxed),
                         rate(g_g604InBoxBigRgb.load(std::memory_order_relaxed),
                              g_g604InBoxPixels.load(std::memory_order_relaxed)),
                         (unsigned long long)g_g604InBoxMaxDRgb.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604OutBoxPixels.load(std::memory_order_relaxed),
                         rate(g_g604OutBoxBigRgb.load(std::memory_order_relaxed),
                              g_g604OutBoxPixels.load(std::memory_order_relaxed)),
                         (unsigned long long)g_g604NoSubInBoxPixels.load(std::memory_order_relaxed),
                         rate(g_g604NoSubInBoxBigRgb.load(std::memory_order_relaxed),
                              g_g604NoSubInBoxPixels.load(std::memory_order_relaxed)),
                         (unsigned long long)g_g604NoSubInBoxMaxDRgb.load(std::memory_order_relaxed));
        }
        {
            uint64_t prev = g_g604VerifyMaxDelta.load(std::memory_order_relaxed);
            while (maxDelta > prev &&
                   !g_g604VerifyMaxDelta.compare_exchange_weak(prev, maxDelta,
                                                               std::memory_order_relaxed))
            {
            }
            prev = g_g604VerifyMaxDeltaRgb.load(std::memory_order_relaxed);
            while (maxDeltaRgb > prev &&
                   !g_g604VerifyMaxDeltaRgb.compare_exchange_weak(prev, maxDeltaRgb,
                                                                  std::memory_order_relaxed))
            {
            }
        }
        // Always print a SUB batch: there are only ~130 of them in a run and each one is a
        // datapoint about the admission under test.
        if (s.g604SubDraws != 0u || badBigRgb != 0u || depthBad != 0u || n <= 8u ||
            (n % 60u) == 0u)
            std::fprintf(stderr,
                         "[G604:verifydisp] n=%llu fbp=0x%x pixels=%zu bad=%llu rgb=%llu a=%llu "
                         "cov=%llu big=%llu maxd=%u bigRgb=%llu maxdRgb=%u sub=%u "
                         "zbad=%llu zrows=%d..%d rows=%d..%d entries=%zu prim=%u tme=%u "
                         "first=(%d,%d cpu=%08x gpu=%08x) firstBig=(%d,%d cpu=%08x gpu=%08x) "
                         "worstRgbPx=(%d,%d dst=%08x cpu=%08x gpu=%08x) meanDRgb=%.2f "
                         "sub6(verts=%u tme=%u tfx=%u tcc=%u bil=%u tpsm=0x%x texKey=%u "
                         "srcFbp=0x%x alpha=%016llx) "
                         "cumulative(chk=%llu px=%llu bad=%llu big=%llu bigRgb=%llu maxd=%llu "
                         "maxdRgb=%llu zbad=%llu subBatches=%llu subDraws=%llu)\n",
                         (unsigned long long)n, s.fbp, s.gpu.size(),
                         (unsigned long long)bad, (unsigned long long)badRgb,
                         (unsigned long long)badAlpha, (unsigned long long)coverage,
                         (unsigned long long)badBig, maxDelta,
                         (unsigned long long)badBigRgb, maxDeltaRgb, s.g604SubDraws,
                         (unsigned long long)depthBad, s.depthRowLo, s.depthRowHi,
                         s.rowLo, s.rowHi, s.entries, s.prim, s.tme,
                         firstX, firstY, firstCpu, firstGpu,
                         firstBigX, firstBigY, firstBigCpu, firstBigGpu,
                         firstBigRgbX, firstBigRgbY, firstBigRgbDst,
                         firstBigRgbCpu, firstBigRgbGpu,
                         badBigRgb ? (static_cast<double>(sumDeltaRgb) /
                                      (3.0 * static_cast<double>(badBigRgb)))
                                   : 0.0,
                         s.g604SubVerts, s.g604SubTme, s.g604SubTfx, s.g604SubTcc,
                         s.g604SubBilinear, s.g604SubTpsm, s.g604SubTexKey, s.g604SubSrcFbp,
                         (unsigned long long)s.g604SubAlpha,
                         (unsigned long long)n,
                         (unsigned long long)g_g604VerifyPixels.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604VerifyBad.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604VerifyBadBig.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604VerifyBigRgb.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604VerifyMaxDelta.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604VerifyMaxDeltaRgb.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604VerifyZBad.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604VerifySubBatches.load(std::memory_order_relaxed),
                         (unsigned long long)g_g604VerifySubDraws.load(std::memory_order_relaxed));
    }
    else if (ti >= 0)
    {
        const uint64_t n = g_g255Checks[ti].fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad != 0)
            g_g255Bad[ti].fetch_add(bad, std::memory_order_relaxed);
        if (bad != 0 || (n % 64u) == 0u)
            std::fprintf(stderr,
                         "[G255:verify] fbp=0x%x n=%llu rows=%d..%d pixels=%zu bad=%llu rgb=%llu a=%llu cov=%llu "
                         "batch(entries=%zu prim=%u tme=%u abe=%u alpha=%016llx tbp=0x%x psm=0x%x tfx=%u) "
                         "spr=(%.1f,%.1f u=%u,%u)-(%.1f,%.1f u=%u,%u) tex1=%llx "
                         "first=(%d,%d cpu=%08x gpu=%08x)\n",
                         s.fbp, static_cast<unsigned long long>(n), s.rowLo, s.rowHi,
                         s.gpu.size(), static_cast<unsigned long long>(bad),
                         static_cast<unsigned long long>(badRgb),
                         static_cast<unsigned long long>(badAlpha),
                         static_cast<unsigned long long>(coverage),
                         s.entries, s.prim, s.tme, s.abe,
                         static_cast<unsigned long long>(s.alpha), s.tbp, s.tpsm, s.tfx,
                         s.x0, s.y0, static_cast<uint32_t>(s.u0), static_cast<uint32_t>(s.v0),
                         s.x1, s.y1, static_cast<uint32_t>(s.u1), static_cast<uint32_t>(s.v1),
                         static_cast<unsigned long long>(s.tex1),
                         firstX, firstY, firstCpu, firstGpu);
    }
    // G583 before G286: the two 0x13b oracles are kept DISJOINT by `s.g570Depth`. G286's is gated
    // `!guestDepth` at its own trigger site and reports the depth-less sprite class; this one
    // reports exactly the depth-carrying class G286's oracle has never been able to see.
    else if (g583Verify13bOn() && s.fbp == 0x13bu && s.g570Depth)
    {
        const uint64_t n =
            g_g583VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        g_g583VerifyPixels.fetch_add(s.gpu.size(), std::memory_order_relaxed);
        if (bad != 0u)
            g_g583VerifyBad.fetch_add(bad, std::memory_order_relaxed);
        if (depthBad != 0u)
            g_g583VerifyZBad.fetch_add(depthBad, std::memory_order_relaxed);
        if (badBig != 0u)
            g_g584VerifyBadBig.fetch_add(badBig, std::memory_order_relaxed);
        {
            uint64_t prev = g_g584VerifyMaxDelta.load(std::memory_order_relaxed);
            while (maxDelta > prev &&
                   !g_g584VerifyMaxDelta.compare_exchange_weak(prev, maxDelta,
                                                               std::memory_order_relaxed))
            {
            }
        }
        if (bad != 0u || depthBad != 0u || n <= 8u || (n % 30u) == 0u)
            std::fprintf(stderr,
                         "[G583:verify13b] n=%llu pixels=%zu bad=%llu rgb=%llu a=%llu cov=%llu "
                         "big=%llu maxd=%u zbad=%llu zrows=%d..%d rows=%d..%d entries=%zu "
                         "prim=%u tme=%u tbp=0x%x psm=0x%x first=(%d,%d cpu=%08x gpu=%08x) "
                         "firstBig=(%d,%d cpu=%08x gpu=%08x) "
                         "cumulative(px=%llu bad=%llu big=%llu maxd=%llu zbad=%llu)\n",
                         (unsigned long long)n, s.gpu.size(),
                         (unsigned long long)bad, (unsigned long long)badRgb,
                         (unsigned long long)badAlpha, (unsigned long long)coverage,
                         (unsigned long long)badBig, maxDelta,
                         (unsigned long long)depthBad, s.depthRowLo, s.depthRowHi,
                         s.rowLo, s.rowHi, s.entries, s.prim, s.tme, s.tbp, s.tpsm,
                         firstX, firstY, firstCpu, firstGpu,
                         firstBigX, firstBigY, firstBigCpu, firstBigGpu,
                         (unsigned long long)g_g583VerifyPixels.load(std::memory_order_relaxed),
                         (unsigned long long)g_g583VerifyBad.load(std::memory_order_relaxed),
                         (unsigned long long)g_g584VerifyBadBig.load(std::memory_order_relaxed),
                         (unsigned long long)g_g584VerifyMaxDelta.load(std::memory_order_relaxed),
                         (unsigned long long)g_g583VerifyZBad.load(std::memory_order_relaxed));
    }
    else if (g286VerifyOn() && s.fbp == 0x13bu)
    {
        const uint64_t n =
            g_g286VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad != 0u)
            g_g286VerifyBad.fetch_add(bad, std::memory_order_relaxed);
        if (bad != 0u || n <= 8u || (n % 30u) == 0u)
            std::fprintf(stderr,
                         "[G286:verify] n=%llu pixels=%zu bad=%llu rgb=%llu a=%llu cov=%llu "
                         "rows=%d..%d entries=%zu prim=%u tme=%u tbp=0x%x psm=0x%x "
                         "first=(%d,%d cpu=%08x gpu=%08x) cumulativeBad=%llu\n",
                         (unsigned long long)n, s.gpu.size(),
                         (unsigned long long)bad, (unsigned long long)badRgb,
                         (unsigned long long)badAlpha, (unsigned long long)coverage,
                         s.rowLo, s.rowHi, s.entries, s.prim, s.tme, s.tbp, s.tpsm,
                         firstX, firstY, firstCpu, firstGpu,
                             (unsigned long long)g_g286VerifyBad.load(std::memory_order_relaxed));
    }
    else if (g570Verify13dOn() && s.fbp == 0x13du)
    {
        const uint64_t n =
            g_g570VerifyChecks.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (bad != 0u || depthBad != 0u)
            g_g570VerifyBad.fetch_add(bad + depthBad, std::memory_order_relaxed);
        if (bad != 0u || depthBad != 0u || n <= 8u || (n % 30u) == 0u)
            std::fprintf(stderr,
                         "[G570:verify13d] n=%llu pixels=%zu bad=%llu rgb=%llu a=%llu cov=%llu "
                         "zbad=%llu zrows=%d..%d rows=%d..%d entries=%zu prim=%u tme=%u tbp=0x%x psm=0x%x "
                         "first=(%d,%d cpu=%08x gpu=%08x) cumulativeBad=%llu\n",
                         (unsigned long long)n, s.gpu.size(),
                         (unsigned long long)bad, (unsigned long long)badRgb,
                         (unsigned long long)badAlpha, (unsigned long long)coverage,
                         (unsigned long long)depthBad, s.depthRowLo, s.depthRowHi,
                         s.rowLo, s.rowHi, s.entries, s.prim, s.tme, s.tbp, s.tpsm,
                         firstX, firstY, firstCpu, firstGpu,
                         (unsigned long long)g_g570VerifyBad.load(std::memory_order_relaxed));
    }
    s.pending = false;
    s.g570Depth = false;
}

// G253 (diagnostic only, default off): attribute G252's mid-frame drains without changing
// capture, replay, or barrier behavior.  The compact transition matrix answers whether the
// remaining cost is target switching (which needs a dependency-aware mixed-target design) or
// non-deferrable primitives (which may admit a narrower eligibility change).
static int g253BarrierFbpBucket(uint32_t fbp)
{
    if (fbp == 0x0u) return 0;
    if (fbp == 0x68u) return 1;
    const int rtt = g248TargetIndex(fbp);
    return rtt >= 0 ? (2 + rtt) : 7;
}

static void g253NoteMidBarrier(bool fbpSwitch, uint32_t fromFbp, uint32_t toFbp,
                               size_t pendingEntries, uint32_t primType, bool tme, bool abe)
{
    static const bool s_on = envFlagEnabled("DC2_G253_BARRIER_STAT");
    if (!s_on)
        return;

    static std::atomic<uint64_t> s_seq{0};
    static std::atomic<uint64_t> s_fbpSwitches{0};
    static std::atomic<uint64_t> s_nonDeferrable{0};
    static std::atomic<uint64_t> s_entries{0};
    static std::atomic<uint64_t> s_transition[8][8]{};
    static std::atomic<uint64_t> s_nonDefClass[8][4]{};
    static constexpr const char *kNames[8] = {
        "000", "068", "139", "13c", "143", "146", "155", "other"
    };

    if (fbpSwitch)
        s_fbpSwitches.fetch_add(1u, std::memory_order_relaxed);
    else
        s_nonDeferrable.fetch_add(1u, std::memory_order_relaxed);
    s_entries.fetch_add(static_cast<uint64_t>(pendingEntries), std::memory_order_relaxed);
    s_transition[g253BarrierFbpBucket(fromFbp)][g253BarrierFbpBucket(toFbp)]
        .fetch_add(1u, std::memory_order_relaxed);
    if (!fbpSwitch)
    {
        const uint32_t state = (tme ? 1u : 0u) | (abe ? 2u : 0u);
        s_nonDefClass[std::min<uint32_t>(primType, 7u)][state]
            .fetch_add(1u, std::memory_order_relaxed);
    }

    const uint64_t seq = s_seq.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((seq % 1024u) != 0u)
        return;

    const uint64_t switches = s_fbpSwitches.exchange(0u, std::memory_order_relaxed);
    const uint64_t nonDef = s_nonDeferrable.exchange(0u, std::memory_order_relaxed);
    const uint64_t entries = s_entries.exchange(0u, std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G253:barrier] n=%llu fbpSwitch=%llu nondef=%llu entries=%llu avgEntries=%.2f\n",
                 static_cast<unsigned long long>(seq),
                 static_cast<unsigned long long>(switches),
                 static_cast<unsigned long long>(nonDef),
                 static_cast<unsigned long long>(entries),
                 static_cast<double>(entries) / 1024.0);
    for (int from = 0; from < 8; ++from)
    {
        for (int to = 0; to < 8; ++to)
        {
            const uint64_t count = s_transition[from][to].exchange(0u, std::memory_order_relaxed);
            if (count != 0u)
                std::fprintf(stderr, "[G253:transition] %s->%s count=%llu\n",
                             kNames[from], kNames[to],
                             static_cast<unsigned long long>(count));
        }
    }
    for (int prim = 0; prim < 8; ++prim)
    {
        for (int state = 0; state < 4; ++state)
        {
            const uint64_t count = s_nonDefClass[prim][state].exchange(0u, std::memory_order_relaxed);
            if (count != 0u)
                std::fprintf(stderr, "[G253:nondef] prim=%d tme=%d abe=%d count=%llu\n",
                             prim, state & 1, (state >> 1) & 1,
                             static_cast<unsigned long long>(count));
        }
    }
}

class G248SpriteScope
{
public:
    G248SpriteScope(uint32_t fbp, bool tme, bool abe)
    {
        const int target = g_g248Stat ? g248TargetIndex(fbp) : -1;
        if (target < 0)
            return;
        m_bucket = target * static_cast<int>(kG248StateCount) +
                   (tme ? 1 : 0) + (abe ? 2 : 0);
        m_start = std::chrono::steady_clock::now();
    }

    ~G248SpriteScope()
    {
        if (m_bucket < 0)
            return;
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - m_start).count());
        g_g248SpriteCalls[m_bucket].fetch_add(1u, std::memory_order_relaxed);
        g_g248SpritePixels[m_bucket].fetch_add(m_pixels, std::memory_order_relaxed);
        g_g248SpriteNs[m_bucket].fetch_add(ns, std::memory_order_relaxed);
    }

    void setPixels(uint64_t pixels) { m_pixels = pixels; }

private:
    int m_bucket = -1;
    uint64_t m_pixels = 0u;
    std::chrono::steady_clock::time_point m_start{};
};

static void g248Report()
{
    if (!g_g248Stat)
        return;
    static uint64_t s_frame = 0u;
    static uint64_t s_lastCalls[kG248BucketCount]{};
    static uint64_t s_lastPixels[kG248BucketCount]{};
    static uint64_t s_lastNs[kG248BucketCount]{};
    if ((++s_frame % 30u) != 0u)
        return;

    static constexpr uint32_t kTargets[kG248TargetCount] = {
        0x139u, 0x13cu, 0x143u, 0x146u, 0x155u, 0x159u, 0x181u, 0x13au
    };
    uint64_t totalCalls = 0u, totalPixels = 0u, totalNs = 0u;
    for (uint32_t target = 0u; target < kG248TargetCount; ++target)
    {
        for (uint32_t state = 0u; state < kG248StateCount; ++state)
        {
            const uint32_t bucket = target * kG248StateCount + state;
            const uint64_t calls = g_g248SpriteCalls[bucket].load(std::memory_order_relaxed);
            const uint64_t pixels = g_g248SpritePixels[bucket].load(std::memory_order_relaxed);
            const uint64_t ns = g_g248SpriteNs[bucket].load(std::memory_order_relaxed);
            const uint64_t dc = calls - s_lastCalls[bucket];
            const uint64_t dp = pixels - s_lastPixels[bucket];
            const uint64_t dn = ns - s_lastNs[bucket];
            s_lastCalls[bucket] = calls;
            s_lastPixels[bucket] = pixels;
            s_lastNs[bucket] = ns;
            if (dc == 0u)
                continue;
            totalCalls += dc;
            totalPixels += dp;
            totalNs += dn;
            std::fprintf(stderr,
                         "[G248:rttspr] frame=%llu fbp=0x%x tme=%u abe=%u calls/f=%.2f "
                         "pixels/f=%.0f ms/f=%.3f ns/px=%.1f us/call=%.1f\n",
                         static_cast<unsigned long long>(s_frame), kTargets[target],
                         state & 1u, (state >> 1u) & 1u,
                         static_cast<double>(dc) / 30.0,
                         static_cast<double>(dp) / 30.0,
                         static_cast<double>(dn) / 30.0e6,
                         dp ? static_cast<double>(dn) / static_cast<double>(dp) : 0.0,
                         static_cast<double>(dn) / static_cast<double>(dc) / 1.0e3);
        }
    }
    std::fprintf(stderr,
                 "[G248:rttspr-total] frame=%llu calls/f=%.2f pixels/f=%.0f ms/f=%.3f "
                 "ns/px=%.1f us/call=%.1f\n",
                 static_cast<unsigned long long>(s_frame),
                 static_cast<double>(totalCalls) / 30.0,
                 static_cast<double>(totalPixels) / 30.0,
                 static_cast<double>(totalNs) / 30.0e6,
                 totalPixels ? static_cast<double>(totalNs) / static_cast<double>(totalPixels) : 0.0,
                 totalCalls ? static_cast<double>(totalNs) / static_cast<double>(totalCalls) / 1.0e3 : 0.0);
}
