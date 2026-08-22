// ===== G634: THE GPU-NATIVE TEXTURE READ — CT32/CT24 DECODED OUT OF RAW VRAM ==================
//
// WHAT THIS REPLACES. In the G630/G633 persistent domain a textured draw whose texture pages are
// GPU-authoritative had exactly two ways to be served without a guest-VRAM round trip:
//   * `g630TryBind`, which binds the resident CT32 *surface FBO* directly. It requires a resident
//     surface at that base/width, `psm == CT32`, `fst == 1`, and a per-draw proof that THIS draw's
//     UV window lies inside the surface's covered rows.
//   * `g630TryT8View`, which decodes a PSMT8 texture out of the raw image on the GPU.
// Everything else fell through to `g630MaterializeForRanges` — a synchronous GPU->CPU publication
// of the texture's pages, i.e. a guest-VRAM authority transition on the pole thread.
//
// `[G632:texbits]` on `ridepod` (16,207 real `tex` breaks, `rows=14 overflow=0`, so the histogram
// is complete) says those fallbacks are TWO shapes and nothing else:
//     bind=0x0010 psm=01 (PSMCT24, no resident view exists for it at all)   7,579   46.8%
//     bind=0x0020 psm=00 (CT32 but NOT FST — no per-draw UV window derivable) 7,531  46.5%
//     bind=0x0040 psm=00 (no resident surface owns that tbp0/tbw)              885   5.5%
//     bind=0x1800 psm=00 (window not provably inside the surface)              203   1.3%
// and `tex` is 12,509.9 ms of the 15,519.3 ms of ALL publication in that run — 80.6%.
//
// THE OBSERVATION. Every one of those four blockers is a property of the SURFACE-MIRROR contract,
// not of GS memory. Row p of the persistent raw image IS guest page p, so the texture the draw
// wants is already inside the GPU domain; what is missing is a way to read it AS A TEXTURE.
// Decoding it with TEX0 addressing into an ordinary texW x texH texture object removes all four at
// once, because that view:
//   * needs no resident surface (it addresses `tbp0`/`tbw` straight),
//   * needs no UV window and therefore no `fst` (it covers the whole declared texture, so any
//     sample of it — projective, bilinear, wrapped — is inside by construction), and
//   * carries the format in the shader, so PSMCT24 is a TEXA substitution rather than a refusal.
// It also removes the per-draw residency reconstruction those proofs required: the view is keyed
// by `texKey` and built at most once per texture pass, exactly like G630's PSMT8 view.
//
// EXACTNESS. `ct32Word` in the backend program is COPIED VERBATIM from the shipped G630 CT32 view
// program (G629's "do not restate a shipped predicate" law); it is the expression G630/G631 verified
// at `bad=0` over 430,952,960 words. The CT24 alpha arm is `applyTexa`'s CT24 case verbatim
// (`a = (AEM && rgb==0) ? 0 : TA0`). `DC2_G634_VERIFY=1` re-checks both against a raw-image readback
// and — for textures whose pages are NOT GPU-authoritative, where guest VRAM is provably current —
// against the shipped `GSMem::G512ReadRowCT32` decode of the same texture.
//
// Arm DC2_G634_RAWVIEW=1 (inside DC2_G630_RESIDENT=1). Rollback DC2_G634_NO_RAWVIEW=1.
// Census DC2_G634_STAT=1. Oracle DC2_G634_VERIFY=1.

// Namespace-scope caches: PS2_PROJECT_STATE.md's diagnostic-hygiene rule — a function-local
// `static const bool` is an MSVC per-access TLS guard, and this predicate sits on the texture pass.
static const bool s_g634RawView = envFlagEnabled("DC2_G634_RAWVIEW") &&
                                  !envFlagEnabled("DC2_G634_NO_RAWVIEW");
static const bool s_g634RawUp = envFlagEnabled("DC2_G634_RAWUP") &&
                                !envFlagEnabled("DC2_G634_NO_RAWUP");
static const bool s_g634Stat = envFlagEnabled("DC2_G634_STAT");
static const bool s_g634Verify = envFlagEnabled("DC2_G634_VERIFY");

static inline bool g634RawViewOn() { return s_g634RawView && g630DomainOn(); }
static inline bool g634RawUpOn() { return s_g634RawUp && g630DomainOn(); }

// Reject bitmask, in source order, one bit per conjunct (G628's law: census the CONJUNCTS).
static constexpr uint32_t kG634Off = 0x0001u;
static constexpr uint32_t kG634Uninit = 0x0002u;
static constexpr uint32_t kG634NoTme = 0x0004u;
static constexpr uint32_t kG634Skip = 0x0008u;
static constexpr uint32_t kG634Psm = 0x0010u;
static constexpr uint32_t kG634NoKey = 0x0020u;
static constexpr uint32_t kG634Dim = 0x0040u;
static constexpr uint32_t kG634Range = 0x0080u;
static constexpr uint32_t kG634NoAuthority = 0x0100u;
static constexpr uint32_t kG634Submit = 0x0200u;

static std::unordered_set<uint64_t> g_g634Built;
static std::atomic<uint64_t> g_g634Views{0u}, g_g634Fast{0u}, g_g634Rejects{0u};
static std::atomic<uint64_t> g_g634Fail{0u}, g_g634Texels{0u}, g_g634PatchPages{0u};
static std::array<std::atomic<uint64_t>, 16> g_g634RejectBits{};
// G634b counters (declared here so the shared census below can read them).
static std::atomic<uint64_t> g_g634Ups{0u}, g_g634UpOps{0u}, g_g634UpDeclined{0u};
static std::atomic<uint64_t> g_g634UpFail{0u};

static uint64_t g634ViewKey(uint64_t base)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(base);
    mix(0x5634333652415756ull); // "G634RAWV": a namespace of its own, never a G281/G338 key
    return h | (1ull << 63);
}

static void g634BeginTexturePass() { g_g634Built.clear(); }

static void g634Report()
{
    if (!s_g634Stat) return;
    std::fprintf(stderr,
                 "[G634:raw] on=%u views=%llu fast=%llu build=%llu fail=%llu rej=%llu "
                 "Mpx=%.1f patchPages=%llu\n",
                 g634RawViewOn() ? 1u : 0u,
                 (unsigned long long)(g_g634Views.load(std::memory_order_relaxed) +
                                      g_g634Fast.load(std::memory_order_relaxed)),
                 (unsigned long long)g_g634Fast.load(std::memory_order_relaxed),
                 (unsigned long long)g_g634Views.load(std::memory_order_relaxed),
                 (unsigned long long)g_g634Fail.load(std::memory_order_relaxed),
                 (unsigned long long)g_g634Rejects.load(std::memory_order_relaxed),
                 (double)g_g634Texels.load(std::memory_order_relaxed) / 1.0e6,
                 (unsigned long long)g_g634PatchPages.load(std::memory_order_relaxed));
    std::fprintf(stderr,
                 "[G634:rawup] on=%u ups=%llu ops=%llu declined=%llu fail=%llu\n",
                 g634RawUpOn() ? 1u : 0u,
                 (unsigned long long)g_g634Ups.load(std::memory_order_relaxed),
                 (unsigned long long)g_g634UpOps.load(std::memory_order_relaxed),
                 (unsigned long long)g_g634UpDeclined.load(std::memory_order_relaxed),
                 (unsigned long long)g_g634UpFail.load(std::memory_order_relaxed));
    std::fprintf(stderr, "[G634:rejbits]");
    static const char *kNames[10] = {"off", "uninit", "notme", "skip", "psm",
                                     "nokey", "dim", "range", "noauth", "submit"};
    for (uint32_t b = 0u; b < 10u; ++b)
    {
        const uint64_t n = g_g634RejectBits[b].load(std::memory_order_relaxed);
        if (n != 0u)
            std::fprintf(stderr, " %s=%llu", kNames[b], (unsigned long long)n);
    }
    std::fprintf(stderr, "\n");
}

// The admission. Every CHEAP conjunct unconditionally; the block range and the authority scan only
// for draws that survived them, so a run with the prototype disarmed pays exactly one predicate.
static uint32_t g634RejectMask(const G144Entry &e, const G178EntryState &st,
                               G144BlockRange *outTex)
{
    if (!g634RawViewOn()) return kG634Off;
    uint32_t m = 0u;
    if (!g_g630Initialized) m |= kG634Uninit;
    if (!e.prim.tme) m |= kG634NoTme;
    if (st.skip) m |= kG634Skip;
    const uint32_t psm = static_cast<uint32_t>(e.ctx.tex0.psm);
    if (psm != GS_PSM_CT32 && psm != GS_PSM_CT24) m |= kG634Psm;
    if (st.texKey == 0u) m |= kG634NoKey;
    if (st.texW <= 0 || st.texH <= 0 || st.texW > 1024 || st.texH > 1024) m |= kG634Dim;
    if (m != 0u) return m;
    const G144BlockRange tex = g144TextureRange(e);
    if (outTex) *outTex = tex;
    if (!tex.valid) return m | kG634Range;
    // Only take this path when guest VRAM is actually behind. When it is not, the shipped decode
    // (or the cheaper direct FBO bind above it) is already free of a round trip, and claiming the
    // draw here would only add a GPU dispatch. `DC2_G634_VERIFY` deliberately admits the clean
    // population too — that is the only population whose CPU reference is trustworthy.
    if (!g630RangeHasGpuAuthority(tex) && !s_g634Verify) m |= kG634NoAuthority;
    return m;
}

static void g634NoteReject(uint32_t mask)
{
    ++g_g634Rejects;
    if (!s_g634Stat) return;
    for (uint32_t b = 0u; b < 16u; ++b)
        if ((mask & (1u << b)) != 0u)
            g_g634RejectBits[b].fetch_add(1u, std::memory_order_relaxed);
}

// Guest pages under `r` that are NOT GPU-authoritative, appended once each. Same contract as
// `g630AppendGuestRangeUnique` — that helper is typed to G630T8Packet, so this is the identical
// body over the G634 packet rather than a second policy.
static void g634AppendGuestRangeUnique(const G144BlockRange &r, G634RawViewPacket &packet)
{
    if (!r.valid) return;
    std::vector<uint32_t> pages, words;
    g630AppendGuestPages(r.first >> 5u, (r.last >> 5u) - (r.first >> 5u) + 1u, pages, words);
    for (size_t i = 0; i < pages.size(); ++i)
    {
        if (std::find(packet.patchPages.begin(), packet.patchPages.end(), pages[i]) !=
            packet.patchPages.end())
            continue;
        packet.patchPages.push_back(pages[i]);
        packet.patchWords.insert(packet.patchWords.end(),
                                 words.begin() + i * kG630WordsPerPage,
                                 words.begin() + (i + 1u) * kG630WordsPerPage);
    }
}

static uint64_t g634TryRawView(const G144Entry &e, const G178EntryState &st,
                               uint32_t *outMask = nullptr)
{
    // SHIPPED-PATH GUARD. This call sits in the texture pre-pass, which runs whenever the G261
    // residency wave has dirty targets — i.e. it executes ~16.5 M times per `ridepod` run with the
    // prototype disarmed. Returning here keeps that cost at exactly one predicate: even the
    // reject COUNTER below is an atomic add, and 16.5 M of them are not free.
    if (!g634RawViewOn())
    {
        if (outMask) *outMask = kG634Off;
        return 0u;
    }
    G144BlockRange tex{};
    const uint32_t m = g634RejectMask(e, st, &tex);
    if (m != 0u)
    {
        if (outMask) *outMask = m;
        g634NoteReject(m);
        return 0u;
    }
    // ⭐⭐⭐ G636 DEFECT B. This call `continue`s out of the texture pre-pass, skipping the
    // per-target loop that materialises a dirty G261 resident target overlapping the read. If one
    // does overlap, the live pixels are in that FBO and neither the raw image nor guest VRAM has
    // them — decode them and the environment renders from the stale copy (measured 0.63-0.79x on
    // `ridepod`). Decline and let the shipped path publish it first.
    if (s_g636NoRawView || g636G261DirtyOverlaps(tex))
    {
        g_g636ArbRaw.fetch_add(1u, std::memory_order_relaxed);
        if (outMask) *outMask = kG634Submit;
        return 0u;
    }
    const uint64_t key = g634ViewKey(st.texKey);
    if (g_g634Built.count(key) != 0u)
    {
        if (outMask) *outMask = 0u;
        g_g634Fast.fetch_add(1u, std::memory_order_relaxed);
        return key;
    }

    auto packet = std::make_shared<G634RawViewPacket>();
    packet->texKey = key;
    packet->tbp = static_cast<uint32_t>(e.ctx.tex0.tbp0);
    // `readTexelPSMCT32`'s rule: TBW==0 addresses as 1. The shader multiplies by this directly, so
    // the substitution has to happen HERE or a TBW==0 texture would decode from block 0 onward.
    packet->tbw = std::max(1u, static_cast<uint32_t>(e.ctx.tex0.tbw));
    packet->psm = static_cast<uint32_t>(e.ctx.tex0.psm);
    packet->ta0 = static_cast<uint32_t>(e.texa.ta0);
    packet->aem = e.texa.aem ? 1u : 0u;
    packet->width = st.texW;
    packet->height = st.texH;
    // The CT32 page word map is a property of the PSMCT32 page layout, not of a consumer. G630's
    // view path owns the same map; G634 sends its own copy once so the oracle has a CPU-side copy
    // regardless of which path armed the texture first.
    static bool s_g634MapSent = false;
    if (!s_g634MapSent)
    {
        packet->pageWords.resize(64u * 32u);
        for (uint32_t y = 0u; y < 32u; ++y)
            for (uint32_t x = 0u; x < 64u; ++x)
            {
                const uint32_t word = GSPSMCT32::addrPSMCT32(0u, 1u, x, y) >> 2u;
                if (word >= 2048u) return 0u;
                packet->pageWords[y * 64u + x] = word;
            }
    }
    g634AppendGuestRangeUnique(tex, *packet);

    if (s_g634Verify && g_g630Vram != nullptr && !g630RangeHasGpuAuthority(tex))
    {
        // Guest VRAM is provably current for this texture, so the shipped row reader is a valid
        // independent reference for the whole TEX0 -> address -> TEXA chain.
        packet->expected.assign(static_cast<size_t>(st.texW) * st.texH, 0u);
        const uint32_t w = static_cast<uint32_t>(st.texW);
        for (int v = 0; v < st.texH; ++v)
        {
            uint32_t *const row = packet->expected.data() + static_cast<size_t>(v) * w;
            GSMem::G512ReadRowCT32(g_g630Vram, packet->tbp, packet->tbw,
                                   static_cast<uint32_t>(v), w, row);
            if (packet->psm == GS_PSM_CT24)
                for (uint32_t x = 0u; x < w; ++x)
                {
                    const bool rgbZero = (row[x] & 0x00FFFFFFu) == 0u;
                    const uint32_t a = (packet->aem != 0u && rgbZero)
                                           ? 0u : (packet->ta0 & 0xFFu);
                    row[x] = (row[x] & 0x00FFFFFFu) | (a << 24u);
                }
        }
    }
    packet->verify = s_g634Verify;

    G630Pending pending;
    pending.view = true;
    if (!g634_backend_prepare_raw_view(packet))
    {
        g_g634Fail.fetch_add(1u, std::memory_order_relaxed);
        g630RecoverFailure(pending);
        if (outMask) *outMask = kG634Submit;
        return 0u;
    }
    s_g634MapSent = true;
    g_g634PatchPages.fetch_add(packet->patchPages.size(), std::memory_order_relaxed);
    g630CommitSyncedPages(packet->patchPages);
    g_g630Pending.push_back(std::move(pending));
    g_g634Built.insert(key);
    g_g634Views.fetch_add(1u, std::memory_order_relaxed);
    g_g634Texels.fetch_add(static_cast<uint64_t>(st.texW) * st.texH, std::memory_order_relaxed);
    if (outMask) *outMask = 0u;
    return key;
}

// ===== G634b: HOST DMA / IMAGE INGESTION FOR ANY PIXEL FORMAT =================================
//
// `[G632:up]` on `ridepod` is ONE shape with ONE blocker: **2,885 of 2,885** declined uploads fail
// solely on `psm != CT32` (`mask=0x0010`). The shipped GPU patch (`g630UploadCompleteInternal`)
// re-reads the completed rect with `GSMem::ReadCT32` and scatters CT32 texels, so every other
// pixel format concedes a publication before the CPU may write guest VRAM.
//
// The generalisation does NOT need a shader per format. An IMAGE transfer's whole effect on GS
// memory is a set of BIT RANGES inside 32-bit words, and after the CPU has written guest VRAM the
// merged word VALUE is already correct there. So the exact GPU-side expression is one masked
// read-modify-write per distinct word: `dst = (dst & ~mask) | (value & mask)`. The front end
// computes each texel's bit range with the SAME per-PSM address helpers the transfer writer used
// (`readTransferPixel`'s switch, arm for arm), which is why sub-word formats — PSMT8's byte,
// PSMT4's nibble, PSMCT24's low three bytes, PSMT4HL/HH's high nibbles — are exact rather than
// approximated by a whole-page replace.
//
// Arm DC2_G634_RAWUP=1. Rollback DC2_G634_NO_RAWUP=1.

static constexpr uint32_t kG634MaxUpPages = 64u;     // 512 KiB of raw VRAM per transfer
static constexpr size_t kG634MaxUpWords = 131072u;   // hard cap on ops (3 words each)

static std::vector<uint32_t> g_g634UpMask;    // word -> accumulated bit mask, page-span local
static std::vector<uint32_t> g_g634UpTouched; // local word indices, in first-touch order

static bool g634UploadPsmAddressable(uint32_t psm)
{
    switch (psm)
    {
    case GS_PSM_CT32: case GS_PSM_CT24: case GS_PSM_CT16: case GS_PSM_CT16S:
    case GS_PSM_T8: case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH:
    case GS_PSM_Z32: case GS_PSM_Z24: case GS_PSM_Z16: case GS_PSM_Z16S:
        return true;
    default:
        return false;
    }
}

// The bit range one texel occupies, as (absolute 32-bit word index, mask). Mirrors
// `readTransferPixel`'s switch arm for arm — same helper, same `width` convention, same
// nibble/byte selection — so a format this returns `false` for is one the transfer writer itself
// does not handle either.
static bool g634UploadTexelBits(uint32_t block, uint32_t width, uint32_t psm,
                                uint32_t X, uint32_t Y, uint32_t &word, uint32_t &mask)
{
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_Z32:
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(block, width, X, Y);
        word = off >> 2u; mask = 0xFFFFFFFFu; return (off & 3u) == 0u;
    }
    case GS_PSM_CT24:
    case GS_PSM_Z24:
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(block, width, X, Y);
        word = off >> 2u; mask = 0x00FFFFFFu; return (off & 3u) == 0u;
    }
    case GS_PSM_CT16:
    case GS_PSM_Z16:
    {
        const uint32_t off = GSPSMCT16::addrPSMCT16(block, width, X, Y);
        if ((off & 1u) != 0u) return false;
        word = off >> 2u; mask = 0x0000FFFFu << ((off & 2u) * 8u); return true;
    }
    case GS_PSM_CT16S:
    case GS_PSM_Z16S:
    {
        const uint32_t off = GSPSMCT16::addrPSMCT16S(block, width, X, Y);
        if ((off & 1u) != 0u) return false;
        word = off >> 2u; mask = 0x0000FFFFu << ((off & 2u) * 8u); return true;
    }
    case GS_PSM_T8:
    {
        const uint32_t off = GSPSMT8::addrPSMT8(block, width, X, Y);
        word = off >> 2u; mask = 0xFFu << ((off & 3u) * 8u); return true;
    }
    case GS_PSM_T4HL:
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(block, width, X, Y);
        word = off >> 2u; mask = 0x0F000000u; return (off & 3u) == 0u;
    }
    case GS_PSM_T4HH:
    {
        const uint32_t off = GSPSMCT32::addrPSMCT32(block, width, X, Y);
        word = off >> 2u; mask = 0xF0000000u; return (off & 3u) == 0u;
    }
    case GS_PSM_T4:
    {
        const uint32_t nib = GSPSMT4::addrPSMT4(block, width, X, Y);
        const uint32_t byteOff = nib >> 1u;
        word = byteOff >> 2u;
        mask = 0x0Fu << (((byteOff & 3u) * 8u) + ((nib & 1u) * 4u));
        return true;
    }
    default:
        return false;
    }
}

static bool g634CanIngestUpload(uint32_t block, uint32_t fbw, uint32_t psm,
                                uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                bool unknown)
{
    if (!g634RawUpOn() || !g_g630Initialized) return false;
    if (unknown || fbw == 0u || width == 0u || height == 0u) return false;
    // CT32 keeps the shipped scatter — it is already inside the GPU domain and cheaper.
    if (psm == GS_PSM_CT32 || !g634UploadPsmAddressable(psm)) return false;
    // Worst case one distinct word per texel, so bounding TEXELS bounds the op list and the
    // builder below cannot run out of room after the caller has already skipped its publication.
    const uint64_t texels = static_cast<uint64_t>(width) * height;
    if (texels == 0u || texels > static_cast<uint64_t>(kG634MaxUpWords)) return false;
    const G144BlockRange r = g144RangeForRect(block, psm, fbw, x, y, width, height);
    if (!r.valid) return false;
    const uint32_t pageLo = r.first >> 5u;
    const uint32_t pageHi = std::min(kG630Pages - 1u, r.last >> 5u);
    if (pageHi < pageLo || (pageHi - pageLo + 1u) > kG634MaxUpPages) return false;
    // Only an upload that would actually BLOCK is worth GPU work: with no GPU-authoritative page
    // under the rect there is no publication to delete, and guest VRAM stays the authority.
    bool dirty = false;
    for (uint32_t p = pageLo; p <= pageHi; ++p)
        if (g_g630GpuDirty[p] != 0u) { dirty = true; break; }
    return dirty;
}

// Called from the upload-complete hook, i.e. AFTER the CPU wrote the whole rect into guest VRAM.
// Returns false without side effects when the transfer is not ingestible; the caller then behaves
// exactly as before this phase.
static bool g634IngestUpload(uint8_t *vram, uint32_t block, uint32_t fbw, uint32_t psm,
                             uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (vram == nullptr ||
        !g634CanIngestUpload(block, fbw, psm, x, y, width, height, false))
        return false;
    const G144BlockRange r = g144RangeForRect(block, psm, fbw, x, y, width, height);
    const uint32_t pageLo = r.first >> 5u;
    const uint32_t pageHi = std::min(kG630Pages - 1u, r.last >> 5u);
    const size_t baseWord = static_cast<size_t>(pageLo) * kG630WordsPerPage;
    const size_t span = static_cast<size_t>(pageHi - pageLo + 1u) * kG630WordsPerPage;
    if (g_g634UpMask.size() < span) g_g634UpMask.assign(span, 0u);
    g_g634UpTouched.clear();

    // `readTransferPixel`'s width convention: DBW == 0 addresses as 1.
    const uint32_t bw = (fbw != 0u) ? fbw : 1u;
    bool ok = true;
    for (uint32_t py = 0u; py < height && ok; ++py)
        for (uint32_t px = 0u; px < width; ++px)
        {
            uint32_t word = 0u, mask = 0u;
            if (!g634UploadTexelBits(block, bw, psm, x + px, y + py, word, mask))
            { ok = false; break; }
            if (word < baseWord || word >= baseWord + span ||
                g_g634UpTouched.size() >= kG634MaxUpWords)
            { ok = false; break; }
            const size_t local = static_cast<size_t>(word) - baseWord;
            if (g_g634UpMask[local] == 0u) g_g634UpTouched.push_back(static_cast<uint32_t>(local));
            g_g634UpMask[local] |= mask;
        }

    if (!ok)
    {
        // ⚠️ FAIL CLOSED. The publication site consulted the SAME `g634CanIngestUpload` and has
        // already skipped its materialize for this edge, so a silent decline here would leave the
        // guest's IMAGE write invisible to the GPU authority. `g634CanIngestUpload` is built so
        // this is unreachable (the texel cap bounds the op list and every admitted PSM's address is
        // naturally aligned); if it ever fires, republish everything and disarm the domain rather
        // than continue with a hole in it.
        for (uint32_t local : g_g634UpTouched) g_g634UpMask[local] = 0u;
        g_g634UpTouched.clear();
        g_g634UpDeclined.fetch_add(1u, std::memory_order_relaxed);
        G630Pending recover;
        recover.view = true;
        g630RecoverFailure(recover);
        return false;
    }

    auto packet = std::make_shared<G634RawUploadPacket>();
    packet->psm = psm;
    packet->verify = s_g634Verify;
    packet->ops.reserve(g_g634UpTouched.size() * 3u);
    const uint32_t *const vwords = reinterpret_cast<const uint32_t *>(vram);
    for (uint32_t local : g_g634UpTouched)
    {
        const uint32_t word = static_cast<uint32_t>(baseWord + local);
        packet->ops.push_back(word);
        packet->ops.push_back(vwords[word]);
        packet->ops.push_back(g_g634UpMask[local]);
        g_g634UpMask[local] = 0u;
    }
    g_g634UpTouched.clear();

    G630Pending pending;
    pending.view = true;
    if (!g634_backend_submit_raw_upload(packet))
    {
        g_g634UpFail.fetch_add(1u, std::memory_order_relaxed);
        g630RecoverFailure(pending);
        return false;
    }
    g_g630Pending.push_back(std::move(pending));
    g_g634Ups.fetch_add(1u, std::memory_order_relaxed);
    g_g634UpOps.fetch_add(packet->ops.size() / 3u, std::memory_order_relaxed);
    // ⛔ G636: the same defect the CT32 scatter had — the ops cover only the transferred TEXELS, not
    // the whole page, so stamping the page synced permanently excluded the rest of it from future
    // patches. `DC2_G636_NO_UPSYNCFIX=1` restores the original stamp.
    if (s_g636NoUpSyncFix)
        for (uint32_t page = pageLo; page <= pageHi; ++page)
            if (g_g630GpuDirty[page] == 0u)
                g_g630SyncedGen[page] = g_g178PageGen[page].load(std::memory_order_relaxed);
    return true;
}
