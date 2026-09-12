// ==============================================================================================
// G737 — WHY THE DIRECT-FBO-BIND FAST PATH IS REFUSED, ON A SHIP-CONFIG BINARY
// ==============================================================================================
//
// ⛔⛔ THE DEFECT THIS CLOSES — A FIFTH DEAD INSTRUMENT, AND THE ONE THAT MATTERED MOST.
//
// `[G261:stat] mat(tex=7668)` is 31 % of all guest-VRAM publications on `dungeon1` and 1.59 M
// read-back rows. Each one is a textured entry that could NOT bind the producer FBO directly and
// therefore forced a blocking `glReadPixels` of the producer's whole dirty window. TWO censuses
// exist to say WHY, and NEITHER can run in a release build:
//
//     rasterizer_rtt_census_and_waves.inc:172   #if defined(PS2X_G684_HOT_DIAG)
//                                               static bool g262CensusOn() {...}
//                                               #else
//                                               static constexpr bool g262CensusOn(){return false;}
//     rasterizer_gpu_alias_page_view.inc:1166   the same shape for g599PubOn()
//
// Every release build sets `PS2X_G684_HOT_DIAG=OFF`, so `[G262:bindrej]` and `[G599:bind]` are
// compile-time `false` and emit nothing — verified by running `DC2_G262_CENSUS=1 DC2_G261_STAT=1`
// on `dc2_runner_g736ab.exe`: `mat(tex)=7741` and **zero** `[G262:bindrej]` lines.
//
// ⛔ AND THE REPLACEMENT G736 BUILT DOES NOT ANSWER IT EITHER. G736 §2 repaired
// `[G261:stat] fboRej` from a dead `g684DiagAdd` to an unconditional `fetch_add` — but that
// statement sits IMMEDIATELY BEFORE the materialize, so `fboRej == mat(tex)` by construction. The
// repaired run proves it: `fboRej=7741 … mat(tex=7741)`, identical in every window. It establishes
// that the population reaches the decision (refuting G735 §6.2d's "they were excluded upstream")
// and then says nothing about WHICH CONJUNCT refused.
//
// ⭐ WHAT THIS ADDS. The first failing conjunct of the bind predicate, as a HISTOGRAM weighted by
// the rows each refusal costs — because rows, not calls, are what a readback is priced in. Default
// OFF (`DC2_G737_TEXREJ=1`), out of line, in the COLD G713 pipeline TU, and reached ~1.3 times per
// frame, so it can be armed on a ship-config binary without Rule 63 applying: no lever changes the
// number of materializations, and the census is not in either arm of a timing gate.
//
// ⚠️ The classifier must mirror the predicate at `rasterizer_vram_materialization.inc:4178`
// exactly, IN ITS OWN ORDER. If that predicate is edited and this is not, the census names the
// wrong conjunct — so the source order is reproduced in the comment beside each test.
// ==============================================================================================

namespace
{

enum G737TexRejReason
{
    kG737RejPsm = 0,      // TEX0.PSM != CT32
    kG737RejBaseAlias,    // TBP0 != target base, but INSIDE the target's page range (a true alias)
    kG737RejBaseForeign,  // TBP0 != target base and outside its range (CLUT / neighbouring surface)
    kG737RejTbw,          // TBW != target FBW
    kG737RejFst,          // !PRIM.FST (STQ texcoords — no exact texel extent available)
    kG737RejPrim,         // primitive type outside {SPRITE, TRIANGLE, TRISTRIP, TRIFAN}
    kG737RejBilinNonSprite, // bilinear, and not the G263 sprite shape
    kG737RejG263,         // bilinear sprite on a non-resident destination: G263 tried and declined
    kG737RejUv,           // every cheap conjunct held; the FST texel extent left the FBO window
    kG737RejDisagree,     // ⛔ classifier says the bind should have been TAKEN — census is unsound
    kG737RejCount
};

const char *g737RejName(int r)
{
    switch (r)
    {
    case kG737RejPsm:            return "psm";
    case kG737RejBaseAlias:      return "baseAlias";
    case kG737RejBaseForeign:    return "baseForeign";
    case kG737RejTbw:            return "tbw";
    case kG737RejFst:            return "fst";
    case kG737RejPrim:           return "prim";
    case kG737RejBilinNonSprite: return "bilinNonSprite";
    case kG737RejG263:           return "g263declined";
    case kG737RejUv:             return "uvOutsideWindow";
    case kG737RejDisagree:       return "CLASSIFIER-DISAGREES";
    default:                     return "?";
    }
}

uint64_t g_g737RejN[kG737RejCount]{};
uint64_t g_g737RejRows[kG737RejCount]{};
uint64_t g_g737RejTotalN = 0;
uint64_t g_g737RejTotalRows = 0;

// ===== THE ROW-SCOPE CEILING, MEASURED BEFORE ANYTHING IS BUILT (G629's law) =====
//
// `g261Materialize` reads back `[dirtyLo .. dirtyHi]` — the target's WHOLE dirty window — whatever
// the consumer actually samples. G735 §2a.1 measured 267–444 rows per publication and concluded
// "the dirty span really is the whole target, so narrowing on the PRODUCER side has nothing to
// narrow; any row-scoping lever must come from the CONSUMER side", and noted that the mechanism for
// that (`g718RowScopeOn`) exists for G700 surfaces and has NO G261 equivalent.
//
// This prices the missing equivalent. For each refusal it intersects the consumer's own texture
// block range with the target's, converts the surviving blocks to a CT32 page-row band, and books
// three totals:
//
//   rows         what is read back today
//   rowsExact    |band ∩ dirty| — what a reader that fetched only its own rows would pay
//   rowsIval     the best SINGLE-INTERVAL publication, which is what is actually implementable:
//                `r.dirty*` is one interval, so a publication can only shrink it from one end.
//                Publish [dirtyLo..bandHi] and keep the suffix dirty, or [bandLo..dirtyHi] and keep
//                the prefix — take the cheaper. A middle band saves only the larger tail.
//
// ⚠️ `rowsIval` is the honest number. Quoting `rowsExact` would promise a two-interval dirty
// representation that does not exist, which is the shape of error G711 §3.2 prices at 5–20x.
uint64_t g_g737ScopeN = 0;
uint64_t g_g737ScopeRows = 0;
uint64_t g_g737ScopeExact = 0;
uint64_t g_g737ScopeIval = 0;
uint64_t g_g737ScopeNoBand = 0;   // range invalid / band empty ⇒ cannot scope, charged in full

// CT32 geometry: one GS page is 64x32 pixels and 32 blocks; pages advance along a page-row `fbw`
// at a time. So block b, relative to the target's base block, sits in page b/32, page-row
// (b/32)/fbw, i.e. pixel rows [pageRow*32, pageRow*32+31]. This is the inverse of the
// `g144RangeForRect(base, GS_PSM_CT32, fbw, 0, y, fbW, h)` the publication itself uses.
bool g737BandForBlocks(uint32_t blkLo, uint32_t blkHi, uint32_t baseBlk, uint32_t fbw,
                       int &rowLo, int &rowHi)
{
    if (fbw == 0u || blkHi < blkLo || blkHi < baseBlk)
        return false;
    const uint32_t relLo = (blkLo > baseBlk) ? (blkLo - baseBlk) : 0u;
    const uint32_t relHi = blkHi - baseBlk;
    const uint32_t pageRowLo = (relLo / 32u) / fbw;
    const uint32_t pageRowHi = (relHi / 32u) / fbw;
    rowLo = static_cast<int>(pageRowLo * 32u);
    rowHi = static_cast<int>(pageRowHi * 32u + 31u);
    return true;
}

void g737NoteRowScope(const G737TexRejectFacts &f, int rows)
{
    ++g_g737ScopeN;
    g_g737ScopeRows += static_cast<uint64_t>(rows);
    int bandLo = 0, bandHi = -1;
    const bool haveBand =
        f.texRangeValid && f.rangeValid &&
        g737BandForBlocks(f.texRangeLo > f.tgtRangeLo ? f.texRangeLo : f.tgtRangeLo,
                          f.texRangeHi < f.tgtRangeHi ? f.texRangeHi : f.tgtRangeHi,
                          f.tgtBlock, f.tgtFbw, bandLo, bandHi);
    if (!haveBand || bandHi < bandLo)
    {
        ++g_g737ScopeNoBand;
        g_g737ScopeExact += static_cast<uint64_t>(rows);
        g_g737ScopeIval += static_cast<uint64_t>(rows);
        return;
    }
    const int lo = (bandLo > f.dirtyLo) ? bandLo : f.dirtyLo;
    const int hi = (bandHi < f.dirtyHi) ? bandHi : f.dirtyHi;
    if (hi < lo)
    {
        // The consumer's band does not intersect the dirty rows at all — this whole publication
        // is servicing rows nobody in this entry reads.
        g_g737ScopeExact += 0ull;
        g_g737ScopeIval += 0ull;
        return;
    }
    g_g737ScopeExact += static_cast<uint64_t>(hi - lo + 1);
    const int prefix = hi - f.dirtyLo + 1;          // publish [dirtyLo..hi], keep the suffix dirty
    const int suffix = f.dirtyHi - lo + 1;          // publish [lo..dirtyHi], keep the prefix dirty
    g_g737ScopeIval += static_cast<uint64_t>(prefix < suffix ? prefix : suffix);
}

// Per-reason PSM histogram, so "baseAlias" can be read as "which format aliases the target".
// 64 PSM codes is the whole GS encoding space.
uint64_t g_g737RejPsmHist[kG737RejCount][64]{};

// One example per reason, kept verbatim so a row that selects a lever can be confirmed by reading
// the source at those exact values (G736-1: a classification is a hypothesis until it is checked).
G737TexRejectFacts g_g737RejExample[kG737RejCount]{};
bool g_g737RejHaveExample[kG737RejCount]{};

// ⭐ The UV-extent half of the predicate, reproduced. Source: the `!st.bilinear` arm at
// rasterizer_vram_materialization.inc:4196-4206. Sprites have exclusive right/bottom endpoints, so
// their maximum tap is v1-1; triangles interpolate up to the vertex texel itself.
bool g737UvInsideWindow(const G737TexRejectFacts &f, bool isSprite)
{
    if (isSprite)
        return f.u0 >= 0 && f.u1 <= f.tgtFbW && f.v0 >= f.covLo && f.v1 <= f.covHi + 1;
    return f.u0 >= 0 && f.u1 <= f.tgtFbW - 1 && f.v0 >= f.covLo && f.v1 <= f.covHi;
}

int g737ClassifyTexReject(const G737TexRejectFacts &f)
{
    constexpr uint32_t kPsmCt32 = 0u;      // GS_PSM_CT32
    constexpr uint32_t kPrimTriangle = 3u; // GS_PRIM_TRIANGLE
    constexpr uint32_t kPrimTriStrip = 4u; // GS_PRIM_TRISTRIP
    constexpr uint32_t kPrimTriFan = 5u;   // GS_PRIM_TRIFAN
    constexpr uint32_t kPrimSprite = 6u;   // GS_PRIM_SPRITE

    // Predicate order at :4178 — psm, base, tbw, fst, prim — then the bilinear split at :4183.
    if (f.texPsm != kPsmCt32)
        return kG737RejPsm;
    if (f.texTbp0 != f.tgtBlock)
        return (f.rangeValid && f.texTbp0 >= f.tgtRangeLo && f.texTbp0 <= f.tgtRangeHi)
                   ? kG737RejBaseAlias
                   : kG737RejBaseForeign;
    if (f.texTbw != f.tgtFbw)
        return kG737RejTbw;
    if (!f.fst)
        return kG737RejFst;
    const bool isSprite = (f.primType == kPrimSprite);
    if (!(isSprite || f.primType == kPrimTriangle || f.primType == kPrimTriStrip ||
          f.primType == kPrimTriFan))
        return kG737RejPrim;
    if (f.bilinear)
        return (isSprite && !f.dstIsResident) ? kG737RejG263 : kG737RejBilinNonSprite;
    // Cheap conjuncts all held and the read is NEAREST ⇒ the extent test is what refused. Assert
    // that, rather than assuming it: if the extent is INSIDE the window the classifier and the
    // predicate disagree and the census is unsound, which must be visible, not silent.
    return g737UvInsideWindow(f, isSprite) ? kG737RejDisagree : kG737RejUv;
}

} // namespace

bool g737TexRejectOn()
{
    static const bool on = (std::getenv("DC2_G737_TEXREJ") != nullptr);
    return on;
}

void g737NoteTexReject(const G737TexRejectFacts &f)
{
    if (!g737TexRejectOn())
        return;
    const int reason = g737ClassifyTexReject(f);
    if (reason < 0 || reason >= kG737RejCount)
        return;
    const int rows = (f.dirtyHi >= f.dirtyLo) ? (f.dirtyHi - f.dirtyLo + 1) : 0;
    ++g_g737RejN[reason];
    ++g_g737RejTotalN;
    g_g737RejRows[reason] += static_cast<uint64_t>(rows);
    g_g737RejTotalRows += static_cast<uint64_t>(rows);
    g737NoteRowScope(f, rows);
    ++g_g737RejPsmHist[reason][f.texPsm & 63u];
    if (!g_g737RejHaveExample[reason])
    {
        g_g737RejExample[reason] = f;
        g_g737RejHaveExample[reason] = true;
    }
}

namespace
{
const char *g737T8ReasonName(int r)
{
    switch (r)
    {
    case kG737T8ShapeBilinear: return "shape:NEAREST(bilinear-required)";
    case kG737T8ShapeOther:    return "shape:other";
    case kG737T8NoSrc:         return "noSrc";
    case kG737T8Clut:          return "clut";
    case kG737T8Map:           return "map(page-ownership)";
    case kG737T8Admit:         return "ADMIT";
    default:                   return "?";
    }
}
uint64_t g_g737T8N[kG737T8ReasonCount]{};
struct G737T8Example
{
    uint32_t dstFbp, tbp0, tbw, psm, prim;
    int texW, texH, fst, bilinear;
    bool have;
};
G737T8Example g_g737T8Eg[kG737T8ReasonCount]{};

uint64_t g_g737CpuScopeN = 0;
uint64_t g_g737CpuScopeRows = 0;
uint64_t g_g737CpuScopeExact = 0;
uint64_t g_g737CpuScopeIval = 0;
uint64_t g_g737CpuScopeNoBand = 0;
} // namespace

void g737NoteCpuScope(uint32_t baseBlk, uint32_t fbw, int dirtyLo, int dirtyHi,
                      uint32_t bandBlkLo, uint32_t bandBlkHi, bool bandValid)
{
    if (!g737TexRejectOn())
        return;
    const int rows = (dirtyHi >= dirtyLo) ? (dirtyHi - dirtyLo + 1) : 0;
    if (rows <= 0)
        return;
    ++g_g737CpuScopeN;
    g_g737CpuScopeRows += static_cast<uint64_t>(rows);
    int bandLo = 0, bandHi = -1;
    if (!bandValid || !g737BandForBlocks(bandBlkLo, bandBlkHi, baseBlk, fbw, bandLo, bandHi) ||
        bandHi < bandLo)
    {
        ++g_g737CpuScopeNoBand;
        g_g737CpuScopeExact += static_cast<uint64_t>(rows);
        g_g737CpuScopeIval += static_cast<uint64_t>(rows);
        return;
    }
    const int lo = (bandLo > dirtyLo) ? bandLo : dirtyLo;
    const int hi = (bandHi < dirtyHi) ? bandHi : dirtyHi;
    if (hi < lo)
        return; // band disjoint from the dirty rows: this publication serves nothing this entry reads
    g_g737CpuScopeExact += static_cast<uint64_t>(hi - lo + 1);
    const int prefix = hi - dirtyLo + 1;
    const int suffix = dirtyHi - lo + 1;
    g_g737CpuScopeIval += static_cast<uint64_t>(prefix < suffix ? prefix : suffix);
}

void g737NoteT8(int reason, uint32_t dstFbp, uint32_t tbp0, uint32_t tbw, uint32_t psm,
                int texW, int texH, uint32_t prim, int fst, int bilinear)
{
    if (!g737TexRejectOn() || reason < 0 || reason >= kG737T8ReasonCount)
        return;
    ++g_g737T8N[reason];
    G737T8Example &eg = g_g737T8Eg[reason];
    if (!eg.have)
    {
        eg.dstFbp = dstFbp; eg.tbp0 = tbp0; eg.tbw = tbw; eg.psm = psm; eg.prim = prim;
        eg.texW = texW; eg.texH = texH; eg.fst = fst; eg.bilinear = bilinear;
        eg.have = true;
    }
}

namespace
{
uint64_t g_g737ColN = 0, g_g737ColNarrow = 0, g_g737ColScissorOnly = 0;
uint64_t g_g737ColRows = 0, g_g737ColScissorOnlyRows = 0;
} // namespace

void g737NoteCpuColorNarrow(bool scissorHit, bool narrowHit, int rows)
{
    if (!g737TexRejectOn() || !scissorHit)
        return;
    ++g_g737ColN;
    g_g737ColRows += static_cast<uint64_t>(rows > 0 ? rows : 0);
    if (narrowHit)
    {
        ++g_g737ColNarrow;
    }
    else
    {
        ++g_g737ColScissorOnly;
        g_g737ColScissorOnlyRows += static_cast<uint64_t>(rows > 0 ? rows : 0);
    }
}

void g737ReportRouteCensus()
{
    if (!g737TexRejectOn())
        return;

    // ⭐ G623's blocked-reason histogram, rescued from a printer that a release build cannot reach.
    // Printed FIRST because it is one level UPSTREAM of the texrej table below: an entry only
    // reaches the `kG261MatTexAlias` materialize after G623 (and G630, G280, G289, G700) have each
    // declined it, so `[G737:g623]` names the refusal that happened first.
    {
        G737G623Snapshot s{};
        g737_g623_snapshot(s);
        if (s.n != 0ull)
        {
            std::fprintf(stderr,
                         "[G737:g623] n=%llu CLEAN=%llu (%.2f%%) binds=%llu unconsidered=%llu "
                         "| blocked:",
                         static_cast<unsigned long long>(s.n),
                         static_cast<unsigned long long>(s.clean),
                         100.0 * static_cast<double>(s.clean) / static_cast<double>(s.n),
                         static_cast<unsigned long long>(s.binds),
                         static_cast<unsigned long long>(s.unconsidered));
            for (int i = 0; i < s.blkCount; ++i)
                if (s.blk[i] != 0ull)
                    std::fprintf(stderr, " %s=%llu", g737_g623_block_name(i),
                                 static_cast<unsigned long long>(s.blk[i]));
            std::fprintf(stderr, "\n");
        }
        if (s.uvProved != 0ull || s.uvBuilt != 0ull || s.uvRefused != 0ull)
            std::fprintf(stderr,
                         "[G737:t8uv] rescuedAdmissions=%llu built=%llu refusedToFullProof=%llu "
                         "texels=%llu of fullTexels=%llu (%.2f%% of the walk the whole-texture "
                         "proof would do)\n",
                         static_cast<unsigned long long>(s.uvProved),
                         static_cast<unsigned long long>(s.uvBuilt),
                         static_cast<unsigned long long>(s.uvRefused),
                         static_cast<unsigned long long>(s.uvTexels),
                         static_cast<unsigned long long>(s.uvFullTexels),
                         s.uvFullTexels ? 100.0 * static_cast<double>(s.uvTexels) /
                                              static_cast<double>(s.uvFullTexels)
                                        : 0.0);
    }

    {
        uint64_t t8Total = 0ull;
        for (int i = 0; i < kG737T8ReasonCount; ++i)
            t8Total += g_g737T8N[i];
        if (t8Total != 0ull)
        {
            std::fprintf(stderr,
                         "[G737:t8any] n=%llu  (g598TryT8EntryAny outcome — the ship-config "
                         "replacement for `[G598:t8] rej(...)`, whose every field is why-gated)\n",
                         static_cast<unsigned long long>(t8Total));
            for (int i = 0; i < kG737T8ReasonCount; ++i)
            {
                if (g_g737T8N[i] == 0ull)
                    continue;
                const G737T8Example &eg = g_g737T8Eg[i];
                std::fprintf(stderr,
                             "[G737:t8any]   %-34s n=%llu (%5.1f%%)  eg: dst=0x%x tbp=0x%x tbw=%u "
                             "psm=0x%x %dx%d prim=%u fst=%d bilin=%d\n",
                             g737T8ReasonName(i),
                             static_cast<unsigned long long>(g_g737T8N[i]),
                             100.0 * static_cast<double>(g_g737T8N[i]) /
                                 static_cast<double>(t8Total),
                             eg.dstFbp, eg.tbp0, eg.tbw, eg.psm, eg.texW, eg.texH, eg.prim,
                             eg.fst, eg.bilinear);
            }
        }
    }

    if (g_g737ColN != 0ull)
        std::fprintf(stderr,
                     "[G737:cpucolor] colorHits=%llu narrowStillHits=%llu scissorONLY=%llu "
                     "(%.1f%% of hits, %llu of %llu rows = %.1f%%)  <= the prize a "
                     "primitive-extent colour range could remove\n",
                     static_cast<unsigned long long>(g_g737ColN),
                     static_cast<unsigned long long>(g_g737ColNarrow),
                     static_cast<unsigned long long>(g_g737ColScissorOnly),
                     100.0 * static_cast<double>(g_g737ColScissorOnly) /
                         static_cast<double>(g_g737ColN),
                     static_cast<unsigned long long>(g_g737ColScissorOnlyRows),
                     static_cast<unsigned long long>(g_g737ColRows),
                     g_g737ColRows ? 100.0 * static_cast<double>(g_g737ColScissorOnlyRows) /
                                         static_cast<double>(g_g737ColRows)
                                   : 0.0);

    if (g_g737CpuScopeN != 0ull && g_g737CpuScopeRows != 0ull)
        std::fprintf(stderr,
                     "[G737:cpuscope] n=%llu rows=%llu exact=%llu (%.1f%%) ival=%llu (%.1f%%) "
                     "noBand=%llu  (kG261MatCpuReplay — the widest cause)\n",
                     static_cast<unsigned long long>(g_g737CpuScopeN),
                     static_cast<unsigned long long>(g_g737CpuScopeRows),
                     static_cast<unsigned long long>(g_g737CpuScopeExact),
                     100.0 * static_cast<double>(g_g737CpuScopeExact) /
                         static_cast<double>(g_g737CpuScopeRows),
                     static_cast<unsigned long long>(g_g737CpuScopeIval),
                     100.0 * static_cast<double>(g_g737CpuScopeIval) /
                         static_cast<double>(g_g737CpuScopeRows),
                     static_cast<unsigned long long>(g_g737CpuScopeNoBand));

    if (g_g737RejTotalN == 0ull)
        return;
    std::fprintf(stderr,
                 "[G737:texrej] n=%llu rows=%llu  (first failing conjunct of the direct-FBO-bind "
                 "predicate, weighted by the rows each refusal reads back)\n",
                 static_cast<unsigned long long>(g_g737RejTotalN),
                 static_cast<unsigned long long>(g_g737RejTotalRows));
    if (g_g737ScopeN != 0ull && g_g737ScopeRows != 0ull)
        std::fprintf(stderr,
                     "[G737:rowscope] n=%llu rows=%llu exact=%llu (%.1f%%) ival=%llu (%.1f%%) "
                     "noBand=%llu  (ival is the implementable one: r.dirty* is ONE interval)\n",
                     static_cast<unsigned long long>(g_g737ScopeN),
                     static_cast<unsigned long long>(g_g737ScopeRows),
                     static_cast<unsigned long long>(g_g737ScopeExact),
                     100.0 * static_cast<double>(g_g737ScopeExact) /
                         static_cast<double>(g_g737ScopeRows),
                     static_cast<unsigned long long>(g_g737ScopeIval),
                     100.0 * static_cast<double>(g_g737ScopeIval) /
                         static_cast<double>(g_g737ScopeRows),
                     static_cast<unsigned long long>(g_g737ScopeNoBand));
    for (int i = 0; i < kG737RejCount; ++i)
    {
        if (g_g737RejN[i] == 0ull)
            continue;
        std::fprintf(stderr,
                     "[G737:texrej]   %-16s n=%llu (%5.1f%%) rows=%llu (%5.1f%%) rowsPerCall=%.1f\n",
                     g737RejName(i),
                     static_cast<unsigned long long>(g_g737RejN[i]),
                     100.0 * static_cast<double>(g_g737RejN[i]) /
                         static_cast<double>(g_g737RejTotalN),
                     static_cast<unsigned long long>(g_g737RejRows[i]),
                     g_g737RejTotalRows ? 100.0 * static_cast<double>(g_g737RejRows[i]) /
                                              static_cast<double>(g_g737RejTotalRows)
                                        : 0.0,
                     static_cast<double>(g_g737RejRows[i]) /
                         static_cast<double>(g_g737RejN[i]));
        // PSM spread for this reason — one line, only the codes that actually occurred.
        std::fprintf(stderr, "[G737:texrej]     psm:");
        for (int p = 0; p < 64; ++p)
            if (g_g737RejPsmHist[i][p] != 0ull)
                std::fprintf(stderr, " 0x%02x=%llu", p,
                             static_cast<unsigned long long>(g_g737RejPsmHist[i][p]));
        std::fprintf(stderr, "\n");
        const G737TexRejectFacts &e = g_g737RejExample[i];
        std::fprintf(stderr,
                     "[G737:texrej]     eg: dst=0x%x tgt=0x%x tbp=0x%x tgtBlk=0x%x tbw=%u fbw=%u "
                     "prim=%u fst=%u bilin=%u tex=%dx%d fbW=%d cov=%d..%d dirty=%d..%d "
                     "uv=%d..%d,%d..%d dstRes=%u\n",
                     e.dstFbp, e.tgtFbp, e.texTbp0, e.tgtBlock, e.texTbw, e.tgtFbw, e.primType,
                     e.fst, e.bilinear, e.texW, e.texH, e.tgtFbW, e.covLo, e.covHi,
                     e.dirtyLo, e.dirtyHi, e.u0, e.u1, e.v0, e.v1, e.dstIsResident);
    }
}
