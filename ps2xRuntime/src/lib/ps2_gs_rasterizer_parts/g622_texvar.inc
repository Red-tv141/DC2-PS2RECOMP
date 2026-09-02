// ===== G622: CONTENT-ADDRESSED TEXTURE VARIANTS =================================================
//
// WHAT WAS WRONG. The front-end texture registry (`g_g178TexReg`) and the backend texture cache
// (`m_texCache`) both hold exactly ONE image per `texKey`. `texKey` is FNV-1a over the texture
// DESCRIPTOR (tbp0/tbw/psm/tw/th/CLUT window — `g178TexKey`), not over the texture's bytes, so a
// source that alternates between a handful of images maps every one of them onto the same slot.
// Each alternation therefore evicts the previous image, re-decodes the source and re-uploads it.
//
// `[G620:texdec]`, shipped G621 binary, `dungeon1` (the census reports, per key, how many decodes
// happened and how many DISTINCT content hashes those decodes ever produced):
//
//   #01 key=9d0623..  512x256 psm=19 tbp=0x02720  dec= 7092  distinct=2  r(cycle= 7090)
//   #02 key=a7193f..  256x256 psm=19 tbp=0x02720  dec=13383  distinct=6  r(cycle=13377)
//   #09 key=dab517..  128x128 psm=19 tbp=0x02720  dec= 1005  hash=3333
//   #13 key=9ae579..  128x128 psm=19 tbp=0x03420  dec=   21  distinct=3  r(cycle=   18)
//   run total: reasonPx%(new=0.0 evict=0.0 fresh=72.3 cycle=27.6), decPxPerFlush=35004
//
// A `cycle` decode is one whose content hash THE SAME KEY has already produced before. Two distinct
// images decoded seven thousand times; six decoded thirteen thousand. **27.6% of every texel this
// route decodes is a cache slot being fought over**, and each of those decodes also re-uploads its
// texels to the GPU.
//
// WHY THIS SHAPE AND NOT ANOTHER. G493's law governs this arc: *a round trip removed from the pole
// thread's `blocked` column is not frame time unless the WORK also shrinks*. This lever deletes
// work — a decode and an upload — on the thread that leads the frame. It does not move a wait.
//
// THE FIX. Exactly one thing changes: the 64-bit key a decoded texture is uploaded under, and the
// key its draws reference. For a key that has been OBSERVED to cycle, that becomes a function of
// the source CONTENT as well as the descriptor, so the alternating images coexist on the GPU and
// the alternation HITS instead of re-decoding.
//
// SELF-LIMITING BY CONSTRUCTION. A variant key is minted only after a key returns to a content it
// produced before (`G622KeyState::cycling`). The single largest decoder on this route — `#00`,
// 512x512 CT32 at tbp=0x2760, `dec=15075 distinct=15075`, 59% of all decoded texels — never repeats
// a content, so it never mints a variant and its behaviour is byte-identical to the shipped path.
// The backend's own LRU (`evictIfNeeded`, `kG178TexCacheCapBytes`) bounds the resident set exactly
// as it does today; a variant that has been evicted simply misses and decodes.
//
// EXACTNESS. A variant hit is taken only when the source's content hash equals the hash recorded
// WITH that variant when it was uploaded, under the same hash body (`G178TexReg::hashVar`, G513).
// The decode is a pure function of the source bytes, the CLUT and the descriptor; the descriptor is
// in `texKey` and the bytes are in the hash, so equal (texKey, hashVar, hash) implies the resident
// texels ARE this decode. The only failure mode is a 64-bit content-hash collision — the same one
// the SHIPPED hash tier has relied on since G178, at the same width, on the same digest.
//
// KEY NAMESPACE. `g178TexKey` and `g281ViewKey` both force bit 63, and 0 is reserved for
// "untextured". A variant key is minted with **bit 63 clear and bit 62 set**, so it is disjoint from
// every existing key by construction and can never alias one.
//
// ARM SYMMETRY (G520's law). The ring bookkeeping (`g622NoteContent`) runs on BOTH arms — only the
// KEY CHOICE differs — so the candidate does not carry a bookkeeping cost the control does not.
// Switching arms mid-run costs one honest re-decode per key per flip (the other arm's key is not
// resident), which is symmetric and is why this arm takes the thread-level hold of 16.
//
// Rollback DC2_G622_NO_TEXVAR=1 · oracle DC2_G622_TEXVAR_VERIFY=1 · counters DC2_G622_TEXVAR_STAT=1
// · paired arm DC2_G419_AB=texvar.
// ==============================================================================================

static bool g622TexVarOn()
{
    static const bool on = !envFlagEnabled("DC2_G622_NO_TEXVAR") &&
                           !envFlagDisabled("DC2_G622_TEXVAR");
    return on;
}

#if defined(PS2X_G684_HOT_DIAG)
static bool g622TexVarStatOn()
{
    static const bool on = envFlagEnabled("DC2_G622_TEXVAR_STAT");
    return on;
}

// The oracle. Under it a would-be variant hit is RECORDED BUT NOT TAKEN: the decode runs anyway and
// its texels are compared against the digest stored when that variant was uploaded. So a verify run
// always ships the pre-G622 bytes — it can report a defect, never cause one (G621's discipline) —
// and it is strictly slower than either arm, so it is never valid in a timing run.
static bool g622TexVarVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G622_TEXVAR_VERIFY");
    return on;
}
#else
static constexpr bool g622TexVarStatOn() { return false; }
static constexpr bool g622TexVarVerifyOn() { return false; }
#endif

// Paired arm. GS-worker thread-level (the flush texture pass), hold 16 — registered in
// g419_ab_instrument.inc's block-hold predicate in this same edit (G506's law).
static bool g622TexVarActive()
{
    return g419AbArmFor(static_cast<int>(kG622LeverTexVar), g622TexVarOn());
}

// Six slots: `#02` above shows six distinct contents, the widest cycle this route produces.
static constexpr uint32_t kG622Ring = 6u;

struct G622KeyState
{
    uint64_t ring[kG622Ring] = {};
    uint8_t n = 0u;
    bool cycling = false;
};

// Flush-owner thread only, exactly like `g_g178TexReg` itself (an unsynchronised unordered_map that
// only the GS worker's flush pass ever walks).
static std::unordered_map<uint64_t, G622KeyState> g_g622Keys;

// Per-FLUSH source-key -> uploaded-key map. The texture pass mints the key; the draw pass, a
// separate walk over the same entries, reads it. Empty on every flush that mints nothing, so the
// draw-side lookup is one `empty()` test on both arms.
static std::unordered_map<uint64_t, uint64_t> g_g622FlushKey;

static std::atomic<uint64_t> g_g622Probes{0}, g_g622Hits{0}, g_g622HitPx{0};
static std::atomic<uint64_t> g_g622Mints{0}, g_g622CyclingKeys{0};
static std::atomic<uint64_t> g_g622VerifyChecks{0}, g_g622VerifyBad{0};

// varKey -> digest of the texels uploaded under it. ORACLE ONLY: populated and read only under
// DC2_G622_TEXVAR_VERIFY, so the shipped path never allocates it.
static std::unordered_map<uint64_t, uint64_t> g_g622Shadow;

static inline uint64_t g622Mix(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

// Records `hash` against `texKey` and reports whether this key has EVER returned to a content it
// produced before. Runs on BOTH arms (see ARM SYMMETRY above).
static bool g622NoteContent(uint64_t texKey, uint64_t hash)
{
    G622KeyState &s = g_g622Keys[texKey];
    for (uint32_t k = 0; k < s.n; ++k)
        if (s.ring[k] == hash)
        {
            if (!s.cycling)
            {
                s.cycling = true;
                g_g622CyclingKeys.fetch_add(1u, std::memory_order_relaxed);
            }
            return true;
        }
    if (s.n < kG622Ring)
    {
        s.ring[s.n++] = hash;
    }
    else
    {
        for (uint32_t k = 1; k < kG622Ring; ++k)
            s.ring[k - 1] = s.ring[k];
        s.ring[kG622Ring - 1] = hash;
    }
    return false;
}

static inline bool g622KeyCycles(uint64_t texKey)
{
    const auto it = g_g622Keys.find(texKey);
    return it != g_g622Keys.end() && it->second.cycling;
}

// Bit 63 clear + bit 62 set: disjoint from `g178TexKey`/`g281ViewKey` (both force bit 63) and from
// 0 (untextured) by construction.
static inline uint64_t g622VariantKey(uint64_t texKey, uint64_t hash, uint8_t hashVar)
{
    const uint64_t h = g622Mix(texKey ^ g622Mix(hash + hashVar + 1ull));
    return (h & 0x3FFFFFFFFFFFFFFFull) | (1ull << 62);
}

static inline void g622ResetFlush()
{
    if (!g_g622FlushKey.empty())
        g_g622FlushKey.clear();
}

static inline void g622NoteFlushKey(uint64_t texKey, uint64_t useKey)
{
    if (useKey != texKey && useKey != 0u)
        g_g622FlushKey[texKey] = useKey;
}

// Consulted by the DRAW pass. Identity whenever this flush minted nothing, which is every flush on
// the control arm and most flushes on the candidate.
static inline uint64_t g622DrawKey(uint64_t texKey)
{
    if (g_g622FlushKey.empty())
        return texKey;
    const auto it = g_g622FlushKey.find(texKey);
    return it == g_g622FlushKey.end() ? texKey : it->second;
}

static inline uint64_t g622ShadowDigest(const std::vector<uint32_t> &px)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t v : px)
    {
        h ^= v;
        h *= 1099511628211ull;
    }
    return h;
}

static void g622TexVarReport()
{
    if (!g622TexVarStatOn())
        return;
    static uint64_t s_next = 0u;
    const uint64_t probes = g_g622Probes.load(std::memory_order_relaxed);
    if (probes < s_next)
        return;
    s_next = probes + 20000u;
    const uint64_t hits = g_g622Hits.load(std::memory_order_relaxed);
    std::fprintf(stderr,
                 "[G622:texvar] probes=%llu mints=%llu cyclingKeys=%llu hits=%llu (%.1f%%) "
                 "hitMpx=%.2f keys=%zu | verify(checks=%llu bad=%llu)\n",
                 static_cast<unsigned long long>(probes),
                 static_cast<unsigned long long>(g_g622Mints.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_g622CyclingKeys.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(hits),
                 probes ? 100.0 * static_cast<double>(hits) / static_cast<double>(probes) : 0.0,
                 static_cast<double>(g_g622HitPx.load(std::memory_order_relaxed)) / 1.0e6,
                 g_g622Keys.size(),
                 static_cast<unsigned long long>(
                     g_g622VerifyChecks.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(
                     g_g622VerifyBad.load(std::memory_order_relaxed)));
}
