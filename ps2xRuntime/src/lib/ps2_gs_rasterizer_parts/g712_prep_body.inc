// ==============================================================================================
// G712 — PREP-AHEAD: the `class` pass, executed on a worker while the batch waits in the graph
// ==============================================================================================
//
// THE DEPENDENCY ARGUMENT, which is the whole justification:
//
//  1. `g178ClassifyEntry(e, st)` is a PURE FUNCTION OF `e`. Its body reads only the entry, the
//     `s_zTestDisabled` static env gate, a null-in-ship hook, and per-target predicates that are
//     themselves functions of `e.ctx.frame.fbp`. It touches no VRAM, no residency slot, no GL
//     object and no page generation. (`g178TexKey` — the one thing that looks like it might hash
//     memory — mixes only TEX0/TEXA/TEXCLUT register fields; the content hash lives in the later
//     `tex` pass, not here.) G510's memo already relies on exactly this purity to reuse entry
//     i-1's classification for entry i.
//
//  2. A batch is IMMUTABLE from `g260CloseOpenBatch()` until it executes. Its entries are moved
//     into `g_g260Batch::entries` and nothing writes them again: `g272PrepareBatch(const
//     G260Batch&)` and `g401NoteCompositeEntries` are read-only, and the vector is not touched
//     until `g_g144List.swap(b.entries)` inside the executor — after this record is consumed.
//     Moving a `G260Batch` (the graph's `erase` of an executed prefix) moves the vector HEADER,
//     not its heap buffer, so the pointer snapshotted here stays valid.
//
//  3. There is real waiting to overlap. `[G260:stat]` on `lavaboss`: **37.3 batch closes per frame
//     against 12.6 drains**, ~2.95 batches per drain, `maxGraph` 22. A batch sits in the graph for
//     the capture of the next target switch (~350 us) or for the execution of the batches ahead of
//     it in its drain (~240 us each), against a classify cost of ~36 us.
//
// ⛔ WHAT IS NOT CLAIMED. This does not make classification cheaper — it is the same code over the
// same entries. It moves it off the pole thread. Rule 88 does not apply (nothing is denied to a
// consumer); the G304 conservation law does not apply (no work is deleted and re-paid); and G701's
// refutation does not apply (no GL call, no submit and no blocking wait changes thread).
//
// FAIL-SAFE BY CONSTRUCTION. Every path that is not a clean, identity-checked, verdict-OK record
// falls through to the shipped inline loop unchanged:
//   * mechanism disarmed, pool not started, or queue full  -> inline
//   * the record does not describe THIS batch's buffer     -> inline
//   * the worker has not started the job                   -> the consumer CASes it and runs it
//                                                             inline, i.e. exactly the old code
//   * the batch classified to a REJECT of any kind         -> inline (so every reject side effect,
//                                                             counter, census and log is produced
//                                                             by the shipped code path verbatim)
//
// ORACLE `DC2_G712_VERIFY=1`: run the inline loop as well and `memcmp` its `s_states` and all
// eleven reductions against the prepared ones, reporting any divergence. Never a timing arm.
// ==============================================================================================

// ⚠️ SCOPE. g712_prep_types.inc is included INSIDE the anonymous namespace that
// rasterizer_draw_prim_probe.inc opens and rasterizer_rtt_census_and_waves.inc closes (it has to
// be: its consumer g260CloseOpenBatch lives in that namespace). A translation unit has exactly one
// unnamed namespace, so re-opening it here makes these definitions the SAME entities as those
// declarations. Defining them at global scope instead makes `G712Prep` ambiguous and leaves
// g712PrepOpen undefined — the G568 linkage lesson, in its usual shape.
namespace
{

// ---------------------------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------------------------

// `kFbH` is a function-local constant inside g178TryFlushGpu; mirror it here.
static constexpr int kG712FbH = 512;

enum : uint32_t
{
    kG712Pending = 0u,
    kG712Claimed = 1u,
    kG712Done = 2u,
};

enum : uint32_t
{
    kG712VerdictOk = 0u,
    kG712VerdictMixedTarget = 1u,
    kG712VerdictClassifyReject = 2u,
    kG712VerdictMixedZ = 3u,
};

struct G712Prep
{
    // ----- input snapshot (written by the producer before the post, read-only afterwards) -----
    const G144Entry *entries = nullptr;
    size_t n = 0;
    uint32_t fbp = 0u;
    uint32_t fbw = 0u;
    bool memo = false; // the G510 identity memo arm, latched with the rest of the input

    // ----- status -----
    std::atomic<uint32_t> state{kG712Pending};

    // ----- output -----
    std::vector<G178EntryState> states;
    uint32_t verdict = kG712VerdictOk;

    // The `class` loop's eleven reductions. The g282 family is accumulated UNCONDITIONALLY here —
    // in the shipped loop it is gated on per-flush candidate flags that are not knowable at batch
    // close, but the accumulated value is a pure function of the entries either way, so the flush
    // simply selects it or ignores it exactly as before.
    int rowLo = 0, rowHi = 0;
    int depthRowLo = 0, depthRowHi = 0;
    int tightRowLo = 0, tightRowHi = 0;   // g282RowLo / g282RowHi
    int tightDepthLo = 0, tightDepthHi = 0; // g282DepthLo / g282DepthHi
    int wrLo = 0, wrHi = 0;               // g621WrLo / g621WrHi
    bool wrUsable = true;
    bool guestDepth = false;
    bool guestDepthWrites = false;
    uint32_t guestZbpPage = 0u;
    uint32_t guestZpsm = 0u;
    uint64_t eligible = 0u; // G510 census: entries whose state matched their predecessor's

    // The four `std::all_of` shape scans the flush prologue runs (G586/G587/G604 admission). All
    // four ask the same question of the same list, so they collapse into one walk here.
    bool shapeUniformCt32Prim = false; // every entry: same fbp+fbw, PSM CT32, prim in {SPRITE,TRI,TRISTRIP}
    bool front0Ct32 = false;           // entries[0].ctx.frame.psm == GS_PSM_CT32
};

// ---------------------------------------------------------------------------------------------
// Free list. In the shipped (non-pipelined) configuration allocation and release both happen on
// the GS worker thread — open at batch close, release after the batch executed — so this list was
// written lock-free.
//
// ⚠️ G723 changes that premise for ONE configuration. `DC2_G722_PREP_AT_PARSE=1` opens the record
// on the PARSE thread (at `g713ParseFlushOpen`, which is ~10 ms of executor latency earlier than
// batch adoption) while `g712PrepRelease` still runs on the EXEC thread. Two threads then touch
// this list, so it takes a mutex. The lock is unconditional rather than armed: the cost is two
// uncontended acquisitions per batch — roughly 90 per frame against a 10,000/frame capture — and a
// conditionally-locked container is a far worse thing to own than that.
// ---------------------------------------------------------------------------------------------
static std::mutex g_g712FreeMtx;
static std::vector<G712Prep *> g_g712Free;
static uint64_t g_g712Posted = 0u, g_g712Served = 0u, g_g712Helped = 0u,
                g_g712Waited = 0u, g_g712Rejected = 0u, g_g712Mismatch = 0u,
                g_g712Entries = 0u;

// ===== The `verts` ELIGIBILITY census ==========================================================
//
// `verts` (0.83-1.34 ms/f, the largest CPU-only stage in the flush) is a pure function of
// (entry, state) EXCEPT for two per-entry tables the flush itself produces: `s_g261FboSrc` (the
// G261 bind pre-pass, i.e. `deps`) and `s_g281ViewKey` (the G622 content-variant tier, i.e. `tex`).
// A batch where NEITHER fired could have had its vertices and draw runs built at batch close,
// beside the `class` pass — but only if that population is actually large.
//
// ⛔ Rule 86 / Rule 85: do not design against an assumed population. Count it first. `vEligible` is
// the share of executed batches that reached the vertex stage with no FBO bind and no minted view
// key. Two plain counters on the GS worker, read once per flush, never per entry (Rule 63).
static uint64_t g_g712VertsFlushes = 0u, g_g712VertsEligible = 0u, g_g712VertsEligEntries = 0u;

static void g712NoteVertsEligibility(bool eligible, size_t entries)
{
    ++g_g712VertsFlushes;
    if (eligible)
    {
        ++g_g712VertsEligible;
        g_g712VertsEligEntries += entries;
    }
}

static bool g712VerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G712_VERIFY");
    return on;
}

// ⛔ THE ORACLE MUST BE FIELD-WISE, NOT `memcmp`. `G178EntryState` has interior padding (3 bytes
// before `residencyPlan`, and the trailing tail), and both the shipped loop and the prep pass build
// each state in a fresh stack local whose padding is indeterminate — so a byte compare reports a
// difference between two IDENTICAL classifications. The first run of this oracle did exactly that:
// every one of the eleven reductions agreed to the digit on every flush while `memcmp` disagreed on
// all of them. G679's rule in its general form: a comparison that is not the comparison the
// consumer performs is not an oracle. The consumer here is `g178StateEqual`, which is field-wise;
// this checks its fields plus the four the classifier fills that it does not read.
static const char *const kG712FieldName[] = {
    "texKey", "texW", "texH", "blend", "tfx", "tcc", "fge", "fogCol", "depthFunc", "depthWrite",
    "bilinear", "wrapU", "wrapV", "scX0", "scY0", "scX1", "scY1", "zbpPage", "zpsm", "guestDepth",
    "skip", "residencyPlan", "residencyBindMask", "residencyT8Mask"};

static uint32_t g712StateDiff(const G178EntryState &a, const G178EntryState &b)
{
    uint32_t m = 0u;
    if (a.texKey != b.texKey)                       m |= 1u << 0;
    if (a.texW != b.texW)                           m |= 1u << 1;
    if (a.texH != b.texH)                           m |= 1u << 2;
    if (a.blend != b.blend)                         m |= 1u << 3;
    if (a.tfx != b.tfx)                             m |= 1u << 4;
    if (a.tcc != b.tcc)                             m |= 1u << 5;
    if (a.fge != b.fge)                             m |= 1u << 6;
    if (a.fogCol != b.fogCol)                       m |= 1u << 7;
    if (a.depthFunc != b.depthFunc)                 m |= 1u << 8;
    if (a.depthWrite != b.depthWrite)               m |= 1u << 9;
    if (a.bilinear != b.bilinear)                   m |= 1u << 10;
    if (a.wrapU != b.wrapU)                         m |= 1u << 11;
    if (a.wrapV != b.wrapV)                         m |= 1u << 12;
    if (a.scX0 != b.scX0)                           m |= 1u << 13;
    if (a.scY0 != b.scY0)                           m |= 1u << 14;
    if (a.scX1 != b.scX1)                           m |= 1u << 15;
    if (a.scY1 != b.scY1)                           m |= 1u << 16;
    if (a.zbpPage != b.zbpPage)                     m |= 1u << 17;
    if (a.zpsm != b.zpsm)                           m |= 1u << 18;
    if (a.guestDepth != b.guestDepth)               m |= 1u << 19;
    if (a.skip != b.skip)                           m |= 1u << 20;
    if (a.residencyPlan != b.residencyPlan)         m |= 1u << 21;
    if (a.residencyBindMask != b.residencyBindMask) m |= 1u << 22;
    if (a.residencyT8Mask != b.residencyT8Mask)     m |= 1u << 23;
    return m;
}

// ---------------------------------------------------------------------------------------------
// The job body. Identical arithmetic to the shipped `class` loop in g178TryFlushGpu, over a batch
// that is not executing.
// ---------------------------------------------------------------------------------------------
static void g712PrepRun(G712Prep &p)
{
    p.states.clear();
    p.states.reserve(p.n);
    p.verdict = kG712VerdictOk;
    p.rowLo = kG712FbH;  p.rowHi = -1;
    p.depthRowLo = kG712FbH; p.depthRowHi = -1;
    p.tightRowLo = kG712FbH; p.tightRowHi = -1;
    p.tightDepthLo = kG712FbH; p.tightDepthHi = -1;
    p.wrLo = kG712FbH;   p.wrHi = -1;
    p.wrUsable = true;
    p.guestDepth = false;
    p.guestDepthWrites = false;
    p.guestZbpPage = 0u;
    p.guestZpsm = 0u;
    p.eligible = 0u;
    p.front0Ct32 = (p.n != 0u) && (p.entries[0].ctx.frame.psm == GS_PSM_CT32);
    p.shapeUniformCt32Prim = (p.n != 0u);

    for (size_t i = 0; i < p.n; ++i)
    {
        const G144Entry &e = p.entries[i];
        if (e.ctx.frame.fbp != p.fbp || e.ctx.frame.fbw != p.fbw)
        {
            p.verdict = kG712VerdictMixedTarget;
            return; // the flush re-derives this on the inline path, with all its diagnostics
        }
        // The collapsed G586/G587/G604 shape scan.
        if (p.shapeUniformCt32Prim &&
            !(e.ctx.frame.psm == GS_PSM_CT32 &&
              (e.prim.type == GS_PRIM_SPRITE || e.prim.type == GS_PRIM_TRIANGLE ||
               e.prim.type == GS_PRIM_TRISTRIP)))
            p.shapeUniformCt32Prim = false;

        G178EntryState st;
        const bool same =
            (p.memo && i > 0u &&
             std::memcmp(&e.ctx, &p.entries[i - 1u].ctx, sizeof(GSContext)) == 0 &&
             std::memcmp(&e.prim, &p.entries[i - 1u].prim, sizeof(GSPrimReg)) == 0 &&
             std::memcmp(&e.texa, &p.entries[i - 1u].texa, sizeof(GSTexaReg)) == 0 &&
             std::memcmp(&e.texclut, &p.entries[i - 1u].texclut, sizeof(GSTexClutReg)) == 0 &&
             e.pabe == p.entries[i - 1u].pabe &&
             e.fogcol == p.entries[i - 1u].fogcol &&
             e.titleRockScope == p.entries[i - 1u].titleRockScope &&
             e.townDepthScope == p.entries[i - 1u].townDepthScope);
        if (same)
        {
            ++p.eligible;
            st = p.states[i - 1u];
        }
        else if (!g178ClassifyEntry(e, st))
        {
            p.verdict = kG712VerdictClassifyReject;
            return; // ditto: the reject's counters, census and [G583] shape are the flush's job
        }

        const int entryLo = std::max(static_cast<int>(st.scY0), e.minY);
        const int entryHi = std::min(static_cast<int>(st.scY1), e.maxY);
        if (st.guestDepth)
        {
            if (!p.guestDepth)
            {
                p.guestDepth = true;
                p.guestZbpPage = st.zbpPage;
                p.guestZpsm = st.zpsm;
            }
            else if (p.guestZbpPage != st.zbpPage || p.guestZpsm != st.zpsm)
            {
                p.verdict = kG712VerdictMixedZ;
                return;
            }
            p.guestDepthWrites = p.guestDepthWrites || st.depthWrite;
            if (!st.skip)
            {
                p.depthRowLo = std::min(p.depthRowLo, static_cast<int>(st.scY0));
                p.depthRowHi = std::max(p.depthRowHi, static_cast<int>(st.scY1));
                if (entryHi >= entryLo)
                {
                    p.tightDepthLo = std::min(p.tightDepthLo, entryLo);
                    p.tightDepthHi = std::max(p.tightDepthHi, entryHi);
                }
            }
        }
        p.states.push_back(st);
        if (!st.skip)
        {
            p.rowLo = std::min(p.rowLo, static_cast<int>(st.scY0));
            p.rowHi = std::max(p.rowHi, static_cast<int>(st.scY1));
            if (entryHi >= entryLo)
            {
                p.wrLo = std::min(p.wrLo, entryLo);
                p.wrHi = std::max(p.wrHi, entryHi);
                p.tightRowLo = std::min(p.tightRowLo, entryLo);
                p.tightRowHi = std::max(p.tightRowHi, entryHi);
            }
            else
                p.wrUsable = false;
        }
    }
}

// The pool's entry point. Claims the job; a lost CAS means the consumer already took it inline.
static void g712PrepJobBody(void *job)
{
    G712Prep *p = static_cast<G712Prep *>(job);
    uint32_t expected = kG712Pending;
    if (!p->state.compare_exchange_strong(expected, kG712Claimed, std::memory_order_acq_rel,
                                          std::memory_order_relaxed))
        return;
    g712PrepRun(*p);
    p->state.store(kG712Done, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------
// Producer side (GS worker thread only)
// ---------------------------------------------------------------------------------------------
static G712Prep *g712PrepOpen(const G144Entry *entries, size_t n, uint32_t fbp, uint32_t fbw)
{
    if (!g712PrepAheadEnabled() || entries == nullptr || n == 0u)
        return nullptr;
    static const bool s_started = g712PrepPoolStart(&g712PrepJobBody);
    if (!s_started)
        return nullptr;
    // ⛔ Rule 63 / G510 exactness: the two classify censuses count SEEN entries, and the memo (and
    // therefore this pass) changes what they see. Stand down entirely whenever either is armed, so
    // a census run measures the shipped population.
    if (g262CensusOn() || g265CensusOn() || g510CensusOn())
        return nullptr;

    G712Prep *p = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_g712FreeMtx);
        if (!g_g712Free.empty())
        {
            p = g_g712Free.back();
            g_g712Free.pop_back();
        }
    }
    if (p == nullptr)
        p = new G712Prep();
    p->entries = entries;
    p->n = n;
    p->fbp = fbp;
    p->fbw = fbw;
    p->memo = g510ClsMemoActive();
    p->verdict = kG712VerdictOk;
    p->state.store(kG712Pending, std::memory_order_release);
    ++g_g712Posted;
    g712PrepPoolPost(p);
    return p;
}

static void g712PrepRelease(G712Prep *&p)
{
    if (p == nullptr)
        return;
    // ⛔ A record must never return to the free list while a worker is inside it. The consumer
    // normally leaves it DONE, but a flush can return false BEFORE reaching the `class` pass (the
    // transient/RTT/residency prologue has a dozen early exits), so this cannot assume it was
    // consumed. Claim it if it is still pending — retiring it unrun — and otherwise wait out the
    // worker. Both are bounded by one batch's classify time.
    uint32_t expected = kG712Pending;
    if (p->state.compare_exchange_strong(expected, kG712Claimed, std::memory_order_acq_rel,
                                         std::memory_order_relaxed))
    {
        p->verdict = kG712VerdictMixedTarget; // retired unrun: never usable
        p->state.store(kG712Done, std::memory_order_release);
    }
    else
    {
        uint32_t s = p->state.load(std::memory_order_acquire);
        while (s == kG712Claimed)
        {
            _mm_pause();
            s = p->state.load(std::memory_order_acquire);
        }
    }
    p->entries = nullptr;
    p->n = 0u;
    {
        std::lock_guard<std::mutex> lk(g_g712FreeMtx);
        g_g712Free.push_back(p);
    }
    p = nullptr;
}

// ---------------------------------------------------------------------------------------------
// Consumer side (the flush, on the GS worker thread)
//
// HELP-OR-WAIT. Try to claim the job; on success run it right here — that is byte-for-byte the
// shipped behaviour, just spelled differently. On a lost CAS the worker owns it, so spin briefly
// and then yield until it publishes. Returns true only for a complete, identity-checked, verdict-OK
// record; every false answer sends the caller to the untouched inline loop.
// ---------------------------------------------------------------------------------------------
static bool g712PrepConsume(G712Prep *p, const G144Entry *entries, size_t n,
                            uint32_t fbp, uint32_t fbw)
{
    if (p == nullptr)
        return false;
    if (p->entries != entries || p->n != n || p->fbp != fbp || p->fbw != fbw)
        return false; // not this batch — fail closed
    uint32_t expected = kG712Pending;
    if (p->state.compare_exchange_strong(expected, kG712Claimed, std::memory_order_acq_rel,
                                         std::memory_order_relaxed))
    {
        ++g_g712Helped;
        g712PrepRun(*p);
        p->state.store(kG712Done, std::memory_order_release);
    }
    else
    {
        uint32_t s = p->state.load(std::memory_order_acquire);
        if (s != kG712Done)
        {
            ++g_g712Waited;
            for (int spin = 0; spin < 4096 && s != kG712Done; ++spin)
            {
                _mm_pause();
                s = p->state.load(std::memory_order_acquire);
            }
            while (s != kG712Done)
            {
                std::this_thread::yield();
                s = p->state.load(std::memory_order_acquire);
            }
        }
    }
    if (p->verdict != kG712VerdictOk)
    {
        ++g_g712Rejected;
        return false;
    }
    ++g_g712Served;
    g_g712Entries += n;
    return true;
}

static void g712PrepReport()
{
    if (!g712PrepStatOn())
        return;
    static uint64_t s_next = 4000u;
    if (g_g712Posted < s_next)
        return;
    s_next = g_g712Posted + 4000u;
    const double posted = static_cast<double>(g_g712Posted ? g_g712Posted : 1u);
    std::fprintf(stderr,
                 "[G712:prep] posted=%llu served=%llu (%.1f%%) helped=%llu (%.1f%%) waited=%llu "
                 "(%.1f%%) reject=%llu mismatch=%llu entries/served=%.1f "
                 "vFlush=%llu vElig=%llu (%.1f%%) vEligEnt/f=%.1f\n",
                 static_cast<unsigned long long>(g_g712Posted),
                 static_cast<unsigned long long>(g_g712Served), 100.0 * g_g712Served / posted,
                 static_cast<unsigned long long>(g_g712Helped), 100.0 * g_g712Helped / posted,
                 static_cast<unsigned long long>(g_g712Waited), 100.0 * g_g712Waited / posted,
                 static_cast<unsigned long long>(g_g712Rejected),
                 static_cast<unsigned long long>(g_g712Mismatch),
                 g_g712Served ? static_cast<double>(g_g712Entries) /
                                    static_cast<double>(g_g712Served)
                              : 0.0,
                 static_cast<unsigned long long>(g_g712VertsFlushes),
                 static_cast<unsigned long long>(g_g712VertsEligible),
                 g_g712VertsFlushes ? 100.0 * static_cast<double>(g_g712VertsEligible) /
                                          static_cast<double>(g_g712VertsFlushes)
                                    : 0.0,
                 g_g712VertsEligible ? static_cast<double>(g_g712VertsEligEntries) /
                                           static_cast<double>(g_g712VertsEligible)
                                     : 0.0);
    std::fflush(stderr);
}

} // namespace
