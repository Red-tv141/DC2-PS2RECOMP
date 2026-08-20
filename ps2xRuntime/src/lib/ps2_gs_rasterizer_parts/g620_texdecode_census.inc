// ===== G620: WHY DOES A TEXTURE RE-DECODE? (diagnostic, default OFF) =====
//
// WHAT THIS ANSWERS.  `[G511:texsplit]` on `dungeon1` prices the flush's texture pass at
// `decode = 0.0549 ms/flush x 45 flushes/f = 2.47 ms/f` over `0.27 decodes/flush = 12.2 whole-
// texture decodes per FRAME`, each ~126 k texels at 1.59 ns/px. That is the largest single item in
// the GS front-end preparation (G618 s2a: `pre` 8.79 ms/f against the whole GL backend's 5.13).
//
// The pass already has a two-level cross-frame cache (`g_g178TexReg`): a page-generation sum, then
// a content hash of the source pages. So the 12 decodes/frame are one of four different things,
// and they license completely different levers:
//
//   new    the descriptor key was never seen        -> nothing to cache; real first-sight work
//   evict  the key is known but the BACKEND dropped it -> a residency/capacity problem
//   fresh  content genuinely changed to a value this key has never carried -> real streaming work
//   cycle  content changed to a value this key ALREADY carried recently  -> a cache that keeps ONE
//          decoded result per key is thrashing between N alternating contents; keeping N would
//          delete the decode outright
//
// G618's law applies to the denominator: this counts what the tex pass VISITED (every key that
// reached the gen test), not only what it decoded, so a rate here has the work-done denominator.
//
// The ring is per key and holds the last kG620Ring content hashes THAT WERE DECODED. A `cycle`
// verdict is therefore a statement about this cache's own history, which is exactly the property a
// larger cache would exploit.
//
// COST / PLACEMENT.  Every site is on the once-per-distinct-key path of the tex pass (36 gen tests,
// 33 hashes, 12 decodes per frame on `dungeon1`) - never inside the per-texel decode loop, and
// never inside the per-entry walk. The arm is read ONCE per flush into a local `bool` and passed
// down (G511's rule, and G619's: a function-local `static const bool` is a per-access thread-safe-
// static guard on MSVC, so it must not be re-tested on a hot path).
//
// THREADING.  This runs on the GS worker (flush owner) only - the same thread that owns
// `g_g178TexReg`, which is itself an unsynchronised `std::unordered_map`. No synchronisation is
// needed here for the same reason.

static bool g620TexDecOn()
{
    static const bool on = envFlagEnabled("DC2_G620_TEXDEC");
    return on;
}

static uint32_t g620TexDecEvery()
{
    static const uint32_t v = g599EnvU32("DC2_G620_TEXDEC_EVERY", 2000u);
    return v;
}

enum G620DecReason : uint32_t
{
    kG620DecNew = 0u,   // no registry entry for this key
    kG620DecEvict = 1u, // registry entry present, backend does not have the texture
    kG620DecFresh = 2u, // content hash changed to a value this key has not carried recently
    kG620DecCycle = 3u, // content hash changed BACK to a value already in this key's ring
    kG620DecCount = 4u
};

static const char *const kG620DecName[kG620DecCount] = {"new", "evict", "fresh", "cycle"};

constexpr uint32_t kG620Ring = 8u;

struct G620TexStat
{
    uint64_t decodes = 0;
    uint64_t genHits = 0;
    uint64_t hashHits = 0;
    uint64_t px = 0;                 // texels decoded, summed
    uint64_t reason[kG620DecCount] = {0, 0, 0, 0};
    uint64_t ring[kG620Ring] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t ringN = 0;              // how many slots are populated
    uint32_t ringW = 0;              // write cursor
    uint32_t distinct = 0;           // distinct content hashes ever seen (saturates at 0xFFFFFFFF)
    int w = 0, h = 0;
    uint32_t tbp0 = 0, cbp = 0, psm = 0, tbw = 0;
};

static std::unordered_map<uint64_t, G620TexStat> g_g620TexStat;
static uint64_t g_g620Visits = 0;    // keys that reached the gen test (the work-done denominator)
static uint64_t g_g620Flushes = 0;
static uint64_t g_g620ReasonTotal[kG620DecCount] = {0, 0, 0, 0};
static uint64_t g_g620ReasonPx[kG620DecCount] = {0, 0, 0, 0};

static void g620NoteTexVisit(bool on)
{
    if (on)
        ++g_g620Visits;
}

static void g620NoteTexHit(bool on, uint64_t key, bool viaHash)
{
    if (!on)
        return;
    G620TexStat &s = g_g620TexStat[key];
    if (viaHash)
        ++s.hashHits;
    else
        ++s.genHits;
}

// Called immediately before the decode, while `regHas`/`backendHas`/`hashNow` are still the values
// the decision was made from.
static void g620NoteTexDecode(bool on, uint64_t key, bool regHas, bool backendHas,
                              uint64_t hashNow, int w, int h,
                              uint32_t tbp0, uint32_t cbp, uint32_t psm, uint32_t tbw)
{
    if (!on)
        return;
    G620TexStat &s = g_g620TexStat[key];
    s.w = w;
    s.h = h;
    s.tbp0 = tbp0;
    s.cbp = cbp;
    s.psm = psm;
    s.tbw = tbw;
    ++s.decodes;
    s.px += static_cast<uint64_t>(w) * static_cast<uint64_t>(h);

    bool inRing = false;
    for (uint32_t k = 0; k < s.ringN; ++k)
        if (s.ring[k] == hashNow)
        {
            inRing = true;
            break;
        }

    uint32_t reason;
    if (!regHas)
        reason = kG620DecNew;
    else if (!backendHas)
        reason = kG620DecEvict;
    else if (inRing)
        reason = kG620DecCycle;
    else
        reason = kG620DecFresh;

    ++s.reason[reason];
    ++g_g620ReasonTotal[reason];
    g_g620ReasonPx[reason] += static_cast<uint64_t>(w) * static_cast<uint64_t>(h);

    if (!inRing)
    {
        if (s.distinct != 0xFFFFFFFFu)
            ++s.distinct;
        s.ring[s.ringW] = hashNow;
        s.ringW = (s.ringW + 1u) % kG620Ring;
        if (s.ringN < kG620Ring)
            ++s.ringN;
    }
}

// The PSMCT32 group row reader's exactness oracle (DC2_G620_VERIFY=1). ⛔ `bad=0` is only a result
// when `checked` is large — a zero denominator is a FAILED RUN, not a clean one (G589).
static void g620RowVerifyReport()
{
    static const bool on = envFlagEnabled("DC2_G620_VERIFY");
    if (!on)
        return;
    static uint64_t s_n = 0u;
    if ((++s_n % 2000u) != 0u)
        return;
    std::fprintf(stderr, "[G620:rowverify] flushes=%llu checked=%llu bad=%llu\n",
                 static_cast<unsigned long long>(s_n),
                 static_cast<unsigned long long>(
                     GSMem::g_g620RowVerifyN.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     GSMem::g_g620RowVerifyBad.load(std::memory_order_relaxed)));
}

// Decode wall time bucketed by SOURCE FORMAT. The census's pixel column ranks by texels, but the
// decode's per-texel price is format-dependent (a CT32 row copy against a T8 CLUT gather), so a
// share of the PIXELS is not a share of the TIME — the two must be read side by side before any
// format's kernel is worth specialising.
static uint64_t g_g620PsmNs[64] = {};
static uint64_t g_g620PsmPx[64] = {};
static uint64_t g_g620PsmN[64] = {};

static void g620NoteTexDecodeNs(bool on, uint32_t psm, uint64_t ns, uint64_t px)
{
    if (!on)
        return;
    const uint32_t k = psm & 63u;
    g_g620PsmNs[k] += ns;
    g_g620PsmPx[k] += px;
    ++g_g620PsmN[k];
}

// The decode BRACKET is not only the decode: it is `up.px.resize()` (a fresh 1 MiB allocation whose
// elements std::vector VALUE-INITIALISES, i.e. a zero-fill of every texel that is then overwritten),
// then `g178DecodeWhole`, then the HD texture-replacement probe. Split it — a stage that turns out
// to be a pure zero-fill is deleted, not optimised.
static uint64_t g_g620StageNs[3] = {}; // 0 = resize, 1 = decode, 2 = replace
static void g620NoteStage(bool on, uint32_t stage, uint64_t ns)
{
    if (on)
        g_g620StageNs[stage & 3u] += ns;
}

static void g620TexDecReport(bool on)
{
    if (!on)
        return;
    const uint64_t nf = ++g_g620Flushes;
    const uint32_t every = g620TexDecEvery();
    if (every == 0u || (nf % every) != 0u)
        return;

    const double n = static_cast<double>(nf);
    uint64_t decTot = 0, genTot = 0, hashTot = 0, pxTot = 0;
    for (const auto &kv : g_g620TexStat)
    {
        decTot += kv.second.decodes;
        genTot += kv.second.genHits;
        hashTot += kv.second.hashHits;
        pxTot += kv.second.px;
    }
    std::fprintf(stderr,
                 "[G620:texdec] flushes=%llu keys=%zu perFlush(visit=%.2f gen=%.2f hash=%.2f "
                 "dec=%.2f) decPxPerFlush=%.0f | reason perFlush(new=%.3f evict=%.3f fresh=%.3f "
                 "cycle=%.3f) reasonPx%%(new=%.1f evict=%.1f fresh=%.1f cycle=%.1f)\n",
                 static_cast<unsigned long long>(nf), g_g620TexStat.size(),
                 static_cast<double>(g_g620Visits) / n,
                 static_cast<double>(genTot) / n, static_cast<double>(hashTot) / n,
                 static_cast<double>(decTot) / n, static_cast<double>(pxTot) / n,
                 static_cast<double>(g_g620ReasonTotal[0]) / n,
                 static_cast<double>(g_g620ReasonTotal[1]) / n,
                 static_cast<double>(g_g620ReasonTotal[2]) / n,
                 static_cast<double>(g_g620ReasonTotal[3]) / n,
                 100.0 * static_cast<double>(g_g620ReasonPx[0]) / std::max(1.0, static_cast<double>(pxTot)),
                 100.0 * static_cast<double>(g_g620ReasonPx[1]) / std::max(1.0, static_cast<double>(pxTot)),
                 100.0 * static_cast<double>(g_g620ReasonPx[2]) / std::max(1.0, static_cast<double>(pxTot)),
                 100.0 * static_cast<double>(g_g620ReasonPx[3]) / std::max(1.0, static_cast<double>(pxTot)));

    for (uint32_t k = 0; k < 64u; ++k)
    {
        if (g_g620PsmN[k] == 0u)
            continue;
        std::fprintf(stderr,
                     "[G620:texpsm]  psm=%2u calls=%llu msPerFlush=%.4f px%%=%.1f ms%%=%.1f "
                     "nsPerPx=%.2f avgPx=%.0f\n",
                     k, static_cast<unsigned long long>(g_g620PsmN[k]),
                     static_cast<double>(g_g620PsmNs[k]) / n / 1.0e6,
                     100.0 * static_cast<double>(g_g620PsmPx[k]) / std::max(1.0, static_cast<double>(pxTot)),
                     100.0 * static_cast<double>(g_g620PsmNs[k]) /
                         std::max(1.0, static_cast<double>([]() {
                             uint64_t t = 0;
                             for (uint32_t q = 0; q < 64u; ++q)
                                 t += g_g620PsmNs[q];
                             return t;
                         }())),
                     static_cast<double>(g_g620PsmNs[k]) /
                         std::max(1.0, static_cast<double>(g_g620PsmPx[k])),
                     static_cast<double>(g_g620PsmPx[k]) /
                         std::max(1.0, static_cast<double>(g_g620PsmN[k])));
    }
    std::fprintf(stderr,
                 "[G620:decstage] msPerFlush(resize=%.4f decode=%.4f replace=%.4f) "
                 "ms%%(resize=%.1f decode=%.1f replace=%.1f)\n",
                 static_cast<double>(g_g620StageNs[0]) / n / 1.0e6,
                 static_cast<double>(g_g620StageNs[1]) / n / 1.0e6,
                 static_cast<double>(g_g620StageNs[2]) / n / 1.0e6,
                 100.0 * static_cast<double>(g_g620StageNs[0]) /
                     std::max(1.0, static_cast<double>(g_g620StageNs[0] + g_g620StageNs[1] +
                                                       g_g620StageNs[2])),
                 100.0 * static_cast<double>(g_g620StageNs[1]) /
                     std::max(1.0, static_cast<double>(g_g620StageNs[0] + g_g620StageNs[1] +
                                                       g_g620StageNs[2])),
                 100.0 * static_cast<double>(g_g620StageNs[2]) /
                     std::max(1.0, static_cast<double>(g_g620StageNs[0] + g_g620StageNs[1] +
                                                       g_g620StageNs[2])));
    std::fprintf(stderr,
                 "[G620:pxpool] acquire(exact=%llu grow=%llu fresh=%llu) held=%zu | "
                 "poison(checked=%llu left=%llu)\n",
                 static_cast<unsigned long long>(g_g620PxExact),
                 static_cast<unsigned long long>(g_g620PxGrow),
                 static_cast<unsigned long long>(g_g620PxFresh),
                 g_g620PxPool.size(),
                 static_cast<unsigned long long>(g_g620PoisonChecked),
                 static_cast<unsigned long long>(g_g620PoisonLeft));
    std::fprintf(stderr, "[G620:zclear] calls=%llu fills=%llu perFlush(calls=%.3f fills=%.3f)\n",
                 static_cast<unsigned long long>(
                     g_g620ZClearCalls.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_g620ZClearFills.load(std::memory_order_relaxed)),
                 static_cast<double>(g_g620ZClearCalls.load(std::memory_order_relaxed)) / n,
                 static_cast<double>(g_g620ZClearFills.load(std::memory_order_relaxed)) / n);

    // Top keys by DECODED PIXELS - the decode's cost is per texel, so ranking by decode COUNT would
    // rank a 32x32 above a 512x512 (G494: rank by the unit the work is actually paid in).
    std::vector<std::pair<uint64_t, const G620TexStat *>> rows;
    rows.reserve(g_g620TexStat.size());
    for (const auto &kv : g_g620TexStat)
        rows.emplace_back(kv.first, &kv.second);
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<uint64_t, const G620TexStat *> &a,
                 const std::pair<uint64_t, const G620TexStat *> &b) {
                  return a.second->px > b.second->px;
              });
    const size_t top = rows.size() < 14u ? rows.size() : 14u;
    for (size_t r = 0; r < top; ++r)
    {
        const G620TexStat &s = *rows[r].second;
        std::fprintf(stderr,
                     "[G620:texdec]  #%02zu key=%016llx %4dx%-4d psm=%2u tbp=0x%05x tbw=%2u "
                     "cbp=0x%05x | dec=%llu gen=%llu hash=%llu distinct=%u "
                     "r(new=%llu evict=%llu fresh=%llu cycle=%llu) pxPerFlush=%.0f\n",
                     r, static_cast<unsigned long long>(rows[r].first), s.w, s.h, s.psm,
                     s.tbp0, s.tbw, s.cbp,
                     static_cast<unsigned long long>(s.decodes),
                     static_cast<unsigned long long>(s.genHits),
                     static_cast<unsigned long long>(s.hashHits), s.distinct,
                     static_cast<unsigned long long>(s.reason[0]),
                     static_cast<unsigned long long>(s.reason[1]),
                     static_cast<unsigned long long>(s.reason[2]),
                     static_cast<unsigned long long>(s.reason[3]),
                     static_cast<double>(s.px) / n);
    }
}
