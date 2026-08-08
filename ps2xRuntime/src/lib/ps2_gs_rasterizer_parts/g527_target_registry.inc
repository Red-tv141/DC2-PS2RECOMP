// ===== G527: RUNTIME RENDER-TARGET REGISTRY — admission by PROPERTY, not by ADDRESS =====
//
// THE DEFECT (G526 §3). Fast-path eligibility in this port is expressed as a hardcoded list of
// five FBP addresses, `g248TargetIndex`'s switch {0x139, 0x13c, 0x143, 0x146, 0x155} — the render
// targets G248 happened to measure on the MAP-0/title route. THREE independent fast paths gate on
// that one list, and the game has 210 maps. A scene whose maps render anywhere else falls onto the
// slowest path in all three at once:
//
//   * G248 fast sprite sampler          -> widened by G526's `anytgt` (shipped, -18.47% Georama)
//   * deferral admission (g252RttTarget / g260RttTri) -> WIDENED HERE
//   * the G260/G261 GPU-resident wave machinery       -> deliberately NOT widened (see below)
//
// The measured residue after G526, Georama, `[G526:draw]` + `[G526:inl]` de-cumulated over a
// 40,000-draw window (5.22 frames):
//
//   exit      draws/f   ms/f    | inline population           n/f    ms/f
//   cull      3,978     0.27    | SPRITE 0x181 (two states)   9.2    9.81
//   defer     3,660     1.21    | SPRITE 0x1d7                2.3    4.52
//   inline    24.1     26.4     | SPRITE 0x19f                2.3    4.10
//                               | SPRITE 0x18d                4.6    3.74
//   raster    21.4 ms/f         | SPRITE 0x1b7                1.1    0.98
//   edge       4.3 ms/f         | LINESTRIP 0x0/0x68          4.6    3.24
//   drain      0.7 ms/f         |
//
// 23.15 ms/f of a 27.8 ms/f drawPrimitive path is SPRITES to five off-whitelist render targets,
// rasterized scalar and single-threaded on the GS worker, each one additionally paying a mid-frame
// graph drain (53.6 us) and the full ownership/materialize edge (158 us) because a non-deferrable
// primitive forces both. Nothing about those sprites is special: `deferrable144` would accept every
// one of them if `g248TargetIndex` named their address.
//
// THE FIX. Discover a render target the first time the guest draws to a non-display FBP and record
// it in a 512-entry direct map (FBP is 9 bits, so the map is the complete key space — O(1), one
// byte load, no branch chain, no allocation, no sizing work). Admission then asks a PROPERTY
// ("has the guest ever rendered to this address?") instead of consulting a list.
//
// ⭐ THE ARCHITECTURAL POINT — G526 §4's blocker was that ONE predicate gated two unrelated things.
// This file splits it in two, which is what makes the widening safe:
//
//   g248TargetIndex(fbp)   NARROW, UNCHANGED. Owns GPU RESIDENCY: g_g261Res, g_g280ContentGen,
//                          g_g310TargetPageGen, g_g261MatByTarget, kG261Fbp, the T8 atlas/resolver
//                          — every one of them sized kG248TargetCount. Those five keep their slots
//                          and their exact behaviour; not one array changes size.
//   g527DeferTarget(fbp)   WIDE, NEW. Owns DEFERRAL + WRITE-GENERATION PUBLICATION, neither of
//                          which needs a slot, an FBO, or a generation table.
//
// ⛔ The ROADMAP's NO-GO ("do not widen deferral admission without pairing it with write-generation
// publication — the three publication sites filter on the same index that gates admission") is
// honoured by CONSTRUCTION: all three publication sites (rasterizer_command_graph.inc x2 and
// g261NoteCpuRttWrites) take the SAME g527DeferTarget predicate as the admission site, in the same
// edit. A discovered target can never be admitted to deferral without its writes being published.
// Today an inline draw publishes via the unconditional g178 bbox bump at the end of drawPrimitive;
// after this lever the deferred flush publishes the same rect (scissor-bounded, i.e. a superset of
// the bbox — conservative in the safe direction).
//
// ⛔ And a discovered target must never reach the GPU flush path: g255Rtt is
// `g255GpuRttOn() && g248TargetIndex(fbp) >= 0`, so a batch whose fbp is discovered-but-not-legacy
// would be false for BOTH g255Rtt and the kG266FrCpuOnlyRtt early-return, and would fall through
// g178TryFlushGpu as if it were a DISPLAY batch — uploading and reading back an arbitrary address
// through logic tuned for 0x0/0x68. g178TryFlushGpu gets an explicit CPU-only return for exactly
// this class. The win being bought here is the 8-lane band-parallel CPU replay, not the GPU.
//
// EXCLUSIONS (never registered):
//   * 0x0 / 0x68  — the display buffers (g272DisplayIndex's whole domain; hardcoded identically at
//                   every deferrable144 site already).
//   * 0x13b       — G286's transient target, which HAS a GPU path (g286TransientTarget) and its own
//                   publication (g286NoteCpuTransientWrites). Registering it would force its
//                   batches onto the CPU and change a proven path.
//   * the legacy five — already handled by g248TargetIndex; g527DeferTarget is only consulted as
//                   `legacy || discovered`, so registering them would be redundant, and leaving
//                   them out keeps the two predicates disjoint and the census readable.
//
// Levers:
//   DC2_G527_TGTREG=1     arm the registry (bring-up default: OFF)
//   DC2_G527_NO_TGTREG=1  rollback once promoted default-ON
//   DC2_G527_NO_TRI=1     bisect: admit discovered SPRITES only, leave triangles inline
//   DC2_G527_STAT=1       [G527:reg] census — which addresses were discovered, and when

static constexpr uint32_t kG527FbpSlots = 512u; // FRAME.FBP is 9 bits: the complete key space

// One byte per FBP. 0 = not a discovered render target (display, excluded, or never drawn to);
// 1 = discovered. Written once per address on the first draw to it, read on every draw and at
// every publication site. Byte load/store on x86; std::atomic only to keep the data race
// well-defined (the GS worker under MTGS and the EE thread at the frame barrier both reach it).
static std::atomic<uint8_t> g_g527Reg[kG527FbpSlots];
static std::atomic<uint32_t> g_g527Discovered{0};
// NOTE: there is deliberately no "deferred draws" counter here. Incrementing one would put an
// atomic on the per-draw path, and the number is already available without cost — `[G526:draw]`'s
// `defer` / `inline` columns are exactly it, split by exit. A counter that is declared and never
// incremented is worse than none (g494_counter_subject_removed_and_normalisation).

// PROMOTED DEFAULT-ON (G527 §6). Georama 38.385 -> 34.950 ms/f, mean -3.435 ms/f = -8.95%,
// 4/4 paired runs negative; Dungeon 6 free-roam 2/2; golden title 211650 with the same-binary
// rollback control returning the identical 211650. Rollback: DC2_G527_NO_TGTREG=1.
static bool g527RegistryOn()
{
    static const bool on = !envFlagEnabled("DC2_G527_NO_TGTREG") &&
                           !envFlagDisabled("DC2_G527_TGTREG");
    return on;
}

// Triangle admission is OPT-IN inside the lever, deliberately. A captured TRIANGLE pre-decodes its
// texture at capture time (`e.decoded = isTri144 ? g149BuildDecoded(...) : G149Buf{}` in
// rasterizer_tilebin_capture.inc) whereas a captured SPRITE samples guest VRAM at replay time; the
// decode therefore reads VRAM at a point where an earlier pending batch may not have executed.
// That contract already governs the legacy five and is unchanged by this phase — but the measured
// inline population on Georama is 100% sprites + linestrips, so admitting triangles buys nothing
// on the route the payoff is claimed from while widening the class of entries subject to it.
// DC2_G527_TRI=1 turns it on for a future phase that prices it with its own gate.
static bool g527TriOn()
{
    static const bool on = envFlagEnabled("DC2_G527_TRI") && !envFlagEnabled("DC2_G527_NO_TRI");
    return on;
}

static bool g527StatOn()
{
    static const bool on = envFlagEnabled("DC2_G527_STAT");
    return on;
}

// Is this FBP the guest's own display buffer, or an address with an existing owner that already
// has a proven path? Never register those.
static inline bool g527ExcludedFbp(uint32_t fbp)
{
    return fbp == 0x0u || fbp == 0x68u || fbp == 0x13bu;
}

static void g527Report(); // defined below

// Called from the drawPrimitive prologue, once per draw. One relaxed byte load on the hot path.
static inline void g527NoteTargetDraw(uint32_t fbp)
{
    if (g527StatOn())
    {
        static std::atomic<uint64_t> s_tick{0};
        if ((s_tick.fetch_add(1u, std::memory_order_relaxed) % 200000ull) == 0ull)
            g527Report();
    }
    if (fbp >= kG527FbpSlots || g527ExcludedFbp(fbp))
        return;
    if (g_g527Reg[fbp].load(std::memory_order_relaxed) != 0u)
        return;
    if (g248TargetIndex(fbp) >= 0)
        return; // legacy five: owned by the residency table, kept out of the wide set
    g_g527Reg[fbp].store(1u, std::memory_order_relaxed);
    const uint32_t n = g_g527Discovered.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g527StatOn())
        std::fprintf(stderr, "[G527:reg] discovered fbp=0x%x (target %u)\n",
                     static_cast<unsigned>(fbp), static_cast<unsigned>(n));
}

// The WIDE predicate. True for a render target this run has discovered — deferral admission and
// write-generation publication both key on exactly this, and nothing else does.
static inline bool g527DiscoveredTarget(uint32_t fbp)
{
    return g527RegistryOn() && fbp < kG527FbpSlots &&
           g_g527Reg[fbp].load(std::memory_order_relaxed) != 0u;
}

// `legacy five OR discovered` — the admission/publication predicate that replaces the bare
// `g248TargetIndex(fbp) >= 0` test at the sites listed in this file's header.
static inline bool g527DeferTarget(uint32_t fbp)
{
    return g248TargetIndex(fbp) >= 0 || g527DiscoveredTarget(fbp);
}

static void g527Report()
{
    if (!g527StatOn())
        return;
    std::fprintf(stderr, "[G527:reg] on=%u discovered=%u fbps=",
                 g527RegistryOn() ? 1u : 0u,
                 g_g527Discovered.load(std::memory_order_relaxed));
    for (uint32_t f = 0; f < kG527FbpSlots; ++f)
        if (g_g527Reg[f].load(std::memory_order_relaxed) != 0u)
            std::fprintf(stderr, "%03x,", static_cast<unsigned>(f));
    std::fprintf(stderr, "\n");
}
