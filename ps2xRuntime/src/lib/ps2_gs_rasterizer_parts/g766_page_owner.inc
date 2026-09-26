// ===== G766: GPU PAGE-OWNER COHERENCE BETWEEN RESIDENT TARGET FBOS =====
//
// THE ARCHITECTURE DEFECT. Dark Cloud 2 renders the same physical GS pages through several resident
// targets (0x139 at three widths, 0x159, 0x15b, 0x13a, 0x13c ... all inside one 4 MiB family). The
// G261 residency model gives each target its own FBO and records only "this target's FBO is newer
// than guest VRAM over rows lo..hi". It has NO notion of which FBO is newest when two of them hold
// the same page. So every internal transition that needs another target's pixels — a wave that
// seeds its FBO from guest VRAM, a texture bind, a width flip — resolves the question by publishing
// FBOs to guest VRAM (a blocking glReadPixels that drains the whole GL queue) and reading the bytes
// back up. On `dragon` that is 1.42 CPU-replay + 0.75 l2l + 0.44 texture + 0.72 resync publications
// per frame, every one of them GPU -> CPU -> GPU, and none of them feeding a real CPU observer.
//
// THE MODEL. Per GS page (512 x 8 KiB) record the resident slot whose FBO holds the NEWEST bytes:
//
//   owner[p] = {slot, slotEpoch, stamp}   set when `slot`'s wave renders rows that contain p
//
// An owner is VALID only while its slot is still dirty in the same dirty epoch and p lies in its
// dirty rows — so every one of the seven sites that clear `dirty` (publication, drops, flips, ...)
// retires ownership implicitly and no clear hook can be missed. Each slot also remembers, per page,
// which {stamp, pageGen} its OWN FBO copy reflects (`seen`). A slot's copy of p is current iff it is
// the owner, or it has seen the owner's current stamp at the current guest write generation (any
// guest VRAM write to p moves g_g178PageGen[p], so a mirrored guest upload forces a re-pull).
//
// Internal transitions then stay on the GPU:
//   * a wave into slot T pulls every page of its window whose valid owner is another slot, as
//     page-tile FBO->FBO copies applied right after its VRAM seed upload (g766_post_upload.inc);
//   * a publication of T first pulls the pages T does not own, so the bytes it writes to guest VRAM
//     are the newest bytes whichever FBO produced them (this also closes a latent multi-writer
//     ordering hazard: publication order no longer decides which alias wins);
//   * an FBO-direct texture bind pulls into the bound FBO instead of publishing every OTHER dirty
//     target whose range overlaps the texture;
//   * G310's logical atlas takes a page from its valid owner instead of from the lowest slot index.
//
// Guest VRAM is written only at real CPU observers, exactly as before.
//
// Exact by construction: a CT32 page is a 64x32 rect whose within-page pixel order does not depend
// on FBW, every colour FBO carries the same integer scale S, and every resident slot stores the same
// raw-alpha RGBA8 representation (G255), so a page copy is a bit-exact glCopyTexSubImage2D.
//
//   DC2_G766_NO_OWNER=1    rollback: no ownership, no pulls (legacy publication everywhere)
//   DC2_G766_STAT=1        [G766:own] census
//   DC2_G766_PULL_VERIFY=1 oracle: after every wave pull, read the pulled pages back from BOTH FBOs
//                          and compare bytewise (perturbing; never timed)

static bool g766OwnerOn()
{
    static const bool on = !envFlagEnabled("DC2_G766_NO_OWNER") &&
                           !envFlagDisabled("DC2_G766_OWNER");
    return on;
}

// Pulls are armed into a synchronous batch; every fire-and-forget arm disables them (the
// fail-closed fallback then publishes the owner, which is the legacy behaviour).
static bool g766PullsAllowed()
{
    static const bool g701 = envFlagEnabled("DC2_G701") && !envFlagEnabled("DC2_G701_NO_ASYNC");
    // G351's split routes 0x139 across two FBO keys; page positions assume one key per slot.
    return g766OwnerOn() && !g701 && !g351SplitOn();
}

// ===== 0x139 width views =====
// 0x139 is rendered at fbw=8 (the screen grab + post-process surface) and at fbw=1/2 (character
// work) every frame. One residency record per fbp made every such alternation a WIDTH FLIP: the
// old width's dirty rows were published to guest VRAM (a 448-row blocking readback on every route
// measured) and the new width re-seeded from VRAM. G766 gives the narrow widths their own slot
// (15) and their own FBO (G349's de-collided key), so the two views coexist as ordinary aliases and
// the page-owner protocol keeps them coherent on the GPU. Rollback DC2_G766_NO_NARROW139=1.
static constexpr uint32_t kG766NarrowSlot = 15u;
static constexpr uint32_t kG766Narrow139Key = 0xfffffff3u; // == kG349Narrow139 in the backend TU
static bool g766NarrowOn()
{
    static const bool on = !envFlagEnabled("DC2_G766_NO_NARROW139") &&
                           !envFlagDisabled("DC2_G766_NARROW139") &&
                           !envFlagEnabled("DC2_G351_SPLIT");
    return on;
}
// The residency slot a BATCH renders into: the width is part of 0x139's identity.
static int g766SlotForBatch(uint32_t fbp, uint32_t fbw)
{
    if (fbp == 0x139u && fbw != 8u && g766NarrowOn())
        return static_cast<int>(kG766NarrowSlot);
    return g248TargetIndex(fbp);
}
// Backend FBO key of a slot's surface (for calls that look the FBO up by key alone).
static inline uint32_t g766SlotKey(uint32_t t)
{
    return (t == kG766NarrowSlot) ? kG766Narrow139Key : kG261Fbp[t];
}
// Snapshot / window-authority key of a slot (g_g178FbSnap, G591), distinct per surface.
static inline uint32_t g766SlotSnapKey(uint32_t t)
{
    return (t == kG766NarrowSlot) ? kG766Narrow139Key : kG261Fbp[t];
}
static inline uint32_t g766SnapKeyFor(uint32_t fbp, uint32_t fbw)
{
    return (fbp == 0x139u && fbw != 8u && g766NarrowOn()) ? kG766Narrow139Key : fbp;
}

struct G766PageOwn
{
    int8_t slot = -1;
    uint32_t epoch = 0u;
    uint64_t stamp = 0u;
};
static G766PageOwn g_g766Own[kG178PageCount];
static uint32_t g_g766Epoch[kG248TargetCount] = {};
static uint64_t g_g766SeenStamp[kG248TargetCount][kG178PageCount] = {};
static uint64_t g_g766SeenGen[kG248TargetCount][kG178PageCount] = {};
static uint64_t g_g766Ctr = 0u;
static uint64_t g_g766Stamps = 0u, g_g766WavePulls = 0u, g_g766WavePullPages = 0u,
    g_g766PubPulls = 0u, g_g766PubPullPages = 0u, g_g766BindPulls = 0u, g_g766BindPullPages = 0u,
    g_g766BindSkips = 0u, g_g766Fallbacks = 0u, g_g766PullFail = 0u, g_g766VerifyBad = 0u,
    g_g766VerifyPages = 0u, g_g766G310Owner = 0u;

extern bool g766_backend_arm_pulls(uint32_t dstKey, const std::vector<int32_t> &rects7);
extern bool g766_backend_last_pulls_applied();
extern void g766_backend_report();
static void g766Report();

static inline uint64_t g766PageGen(uint32_t page)
{
    return g_g178PageGen[page & (kG178PageCount - 1u)].load(std::memory_order_relaxed);
}

// Page `page`'s position in slot t's CURRENT layout (page row / page column), or false when the page
// is not one of the pages that slot's FBO represents.
static inline bool g766SlotPagePos(uint32_t t, uint32_t page, uint32_t &row, uint32_t &col)
{
    const G261Res &r = g_g261Res[t];
    if (r.fbw == 0u || r.fbw > 8u || r.fbW <= 0)
        return false;
    const uint32_t base = kG261Fbp[t];
    if (page < base)
        return false;
    const uint32_t off = page - base;
    row = off / r.fbw;
    col = off % r.fbw;
    return row < 16u && static_cast<int>((col + 1u) * 64u) <= r.fbW;
}

static bool g536GuestOwnsTargetPage(const G261Res &r, uint32_t fbp, uint32_t pageRow,
                                    uint32_t pageCol);

static void g766MirrorFlushOrDrop(int ti);
static inline int g766ValidOwner(uint32_t page)
{
    if (page >= kG178PageCount)
        return -1;
    const G766PageOwn &o = g_g766Own[page];
    if (o.slot < 0 || static_cast<uint32_t>(o.slot) >= kG248TargetCount)
        return -1;
    const uint32_t t = static_cast<uint32_t>(o.slot);
    const G261Res &r = g_g261Res[t];
    if (!r.dirty || g_g766Epoch[t] != o.epoch)
        return -1;
    uint32_t row = 0u, col = 0u;
    if (!g766SlotPagePos(t, page, row, col))
        return -1;
    const int y0 = static_cast<int>(row) * 32;
    if (y0 + 31 < r.dirtyLo || y0 > r.dirtyHi)
        return -1;
    // A guest write that reached VRAM without being mirrored into this FBO (G536's "guest owns
    // the page") makes guest VRAM the newer copy: the FBO is no longer an authority for p.
    if (g536GuestOwnsTargetPage(r, kG261Fbp[t], row, col))
    {
        // ⭐ G769: ...unless the write IS mirrored and only waits in the lazy mirror. A PARTIAL
        // write (PSMT8 / PSMT4 / PSMCT16 into this CT32 page) leaves the page's other bytes newest
        // in this FBO, so "guest VRAM is newer" is false for them. Apply the mirror (a queued GPU
        // patch, no wait) and ask again: every owner query then describes patched truth.
        // Rollback DC2_G769_NO_OWNERMIRFLUSH=1.
        static const bool s_flush = !envFlagEnabled("DC2_G769_NO_OWNERMIRFLUSH");
        if (!s_flush || !r.mirPending || r.mirHi < r.mirLo || y0 + 31 < r.mirLo || y0 > r.mirHi)
            return -1;
        g766MirrorFlushOrDrop(static_cast<int>(t));
        const G261Res &r2 = g_g261Res[t];
        if (!r2.dirty || g_g766Epoch[t] != o.epoch || y0 + 31 < r2.dirtyLo || y0 > r2.dirtyHi ||
            g536GuestOwnsTargetPage(r2, kG261Fbp[t], row, col))
            return -1;
    }
    return static_cast<int>(t);
}

static inline bool g766SeenCurrent(uint32_t t, uint32_t page)
{
    return g_g766SeenStamp[t][page] == g_g766Own[page].stamp &&
           g_g766SeenGen[t][page] == g766PageGen(page);
}

static inline void g766MarkSeen(uint32_t t, uint32_t page)
{
    g_g766SeenStamp[t][page] = g_g766Own[page].stamp;
    g_g766SeenGen[t][page] = g766PageGen(page);
}

// A wave of slot t rendered GS rows [rowLo, rowHi] (layout already updated). `wasDirty` is the dirty
// flag BEFORE this wave: a clean -> dirty transition opens a new ownership epoch.
static void g766NoteWaveRender(int ti, int rowLo, int rowHi, bool wasDirty)
{
    if (!g766OwnerOn() || ti < 0 || static_cast<uint32_t>(ti) >= kG248TargetCount)
        return;
    const uint32_t t = static_cast<uint32_t>(ti);
    if (!wasDirty)
        ++g_g766Epoch[t];
    const G261Res &r = g_g261Res[t];
    if (r.fbw == 0u || r.fbw > 8u || r.fbW <= 0)
        return;
    const int lo = std::max(0, rowLo) / 32;
    const int hi = std::min(511, rowHi) / 32;
    const uint32_t cols = static_cast<uint32_t>(r.fbW) / 64u;
    for (int pr = lo; pr <= hi; ++pr)
        for (uint32_t pc = 0u; pc < cols && pc < r.fbw; ++pc)
        {
            const uint32_t page = kG261Fbp[t] + static_cast<uint32_t>(pr) * r.fbw + pc;
            if (page >= kG178PageCount)
                continue;
            G766PageOwn &o = g_g766Own[page];
            o.slot = static_cast<int8_t>(t);
            o.epoch = g_g766Epoch[t];
            o.stamp = ++g_g766Ctr;
            g766MarkSeen(t, page);
            ++g_g766Stamps;
        }
    g766Report();
}

// Append one page-tile copy {srcKey, sx, sy, dx, dy, 64, 32}, merging with the previous rect when
// both the source and the destination continue the same run horizontally.
static inline void g766AppendRect(std::vector<int32_t> &rects7, uint32_t srcKey, uint32_t srow,
                                  uint32_t scol, uint32_t drow, uint32_t dcol)
{
    const int32_t sx = static_cast<int32_t>(scol * 64u);
    const int32_t sy = 512 - static_cast<int32_t>((srow + 1u) * 32u);
    const int32_t dx = static_cast<int32_t>(dcol * 64u);
    const int32_t dy = 512 - static_cast<int32_t>((drow + 1u) * 32u);
    const size_t n = rects7.size();
    if (n >= 7u && static_cast<uint32_t>(rects7[n - 7u]) == srcKey &&
        rects7[n - 5u] == sy && rects7[n - 3u] == dy &&
        rects7[n - 6u] + rects7[n - 2u] == sx && rects7[n - 4u] + rects7[n - 2u] == dx)
    {
        rects7[n - 2u] += 64;
        return;
    }
    rects7.insert(rects7.end(), {static_cast<int32_t>(srcKey), sx, sy, dx, dy, 64, 32});
}

// Collect the page pulls slot t needs over its GS rows [lo, hi]. `forceLo..forceHi` (may be empty)
// are rows whose FBO copy was just replaced (a VRAM seed upload), so every owned-elsewhere page
// there is pulled regardless of `seen`. Pages are marked seen as they are collected; the caller
// must either apply the pulls or call g766ForgetSeen for the same set.
static uint32_t g766CollectPulls(uint32_t t, int lo, int hi, int forceLo, int forceHi,
                                 std::vector<int32_t> &rects7, std::vector<uint32_t> *pages,
                                 uint32_t *ownerMask)
{
    const G261Res &r = g_g261Res[t];
    if (r.fbw == 0u || r.fbw > 8u || r.fbW <= 0 || hi < lo)
        return 0u;
    const uint32_t cols = static_cast<uint32_t>(r.fbW) / 64u;
    uint32_t pulled = 0u;
    const int prLo = std::max(0, lo) / 32;
    const int prHi = std::min(511, hi) / 32;
    for (int pr = prLo; pr <= prHi; ++pr)
    {
        const bool forced = forceHi >= forceLo && pr * 32 + 31 >= forceLo && pr * 32 <= forceHi;
        for (uint32_t pc = 0u; pc < cols && pc < r.fbw; ++pc)
        {
            const uint32_t page = kG261Fbp[t] + static_cast<uint32_t>(pr) * r.fbw + pc;
            if (page >= kG178PageCount)
                continue;
            const int o = g766ValidOwner(page);
            if (o < 0 || static_cast<uint32_t>(o) == t)
                continue;
            if (!forced && g766SeenCurrent(t, page))
                continue;
            uint32_t srow = 0u, scol = 0u;
            if (!g766SlotPagePos(static_cast<uint32_t>(o), page, srow, scol))
                continue; // cannot happen for a valid owner; never pull what we cannot locate
            g766AppendRect(rects7, g766SlotKey(static_cast<uint32_t>(o)), srow, scol, static_cast<uint32_t>(pr), pc);
            g766MarkSeen(t, page);
            if (pages != nullptr)
                pages->push_back(page);
            if (ownerMask != nullptr)
                *ownerMask |= 1u << static_cast<uint32_t>(o);
            ++pulled;
        }
    }
    return pulled;
}

static void g766ForgetSeen(uint32_t t, const std::vector<uint32_t> &pages)
{
    for (const uint32_t p : pages)
    {
        g_g766SeenStamp[t][p] = 0u;
        g_g766SeenGen[t][p] = ~0ull;
    }
}

static bool g264FlushMirror(int ti);
// G766 sub-word write mirror (g766_mask_mirror.inc), reconciled inside g264FlushMirror.
static bool g766FlushMaskMirror(int ti, bool &any, int &lo, int &hi);
static void g766DropMaskMirror(G261Res &r);
// G766 GPU-seeded transient contracts (g766_mask_mirror.inc), driven from g178TryFlushGpu.
static bool g766TransientSeedAdmit(uint32_t fbp);
static bool g766TransientArmPulls(uint32_t fbp, uint32_t fboKey, int rowLo, int rowHi);
static bool g766TransientConfirm();
static void g766TransientWriteback(uint32_t fbp, uint32_t fbBlock, uint32_t fbw, int fbW, int lo,
                                   int hi);
static void g766TransientReset();
static bool g766TransientArmed(uint32_t fbp);
// G767 transient -> resident fold (g767_transient_fold.inc), driven from g178TryFlushGpu.
static uint64_t g_g767FlushSerial = 0u; // advanced at the top of every GPU flush (g766TransientReset)
static bool g767FoldArm(uint32_t fbp, uint32_t fbw, uint32_t fboKey, int lo, int hi);
static bool g767FoldConfirmCommit();
static void g767FoldReset();
static bool g767FoldArmed(uint32_t fbp);
static void g767RetireView(G178FbSnap &snap);
static bool g767NarrowTransientOn();
static bool g767FoldDepthOn();
// G768 (g768_page_view.inc): the general paletted page view with GPU CLUT.
static uint64_t g768TryView(const G144Entry &e, int texW, int texH,
                            std::unordered_set<uint64_t> &builtThisFlush);
// G768: the same system for direct-colour CT32 / CT24 reads (after the direct binds had their turn).
static uint64_t g768TryDirectView(const G144Entry &e, int texW, int texH,
                                  std::unordered_set<uint64_t> &builtThisFlush);
static uint64_t g767TryT8View(const G144Entry &e, int texW, int texH,
                              std::unordered_set<uint64_t> &builtThisFlush);
static bool g767GpuSeedCovers(uint32_t fbp, uint32_t fbw, int rowLo, int rowHi);
static uint64_t g767TryPageView(const G144Entry &e, int texW, int texH,
                                std::unordered_set<uint64_t> &builtThisFlush);

// ===== G766: LAZY (CONSUMER-SCOPED) UPLOAD MIRRORS =====
// G264 applied every target's pending VRAM->FBO mirror at the start of EVERY GPU flush, whatever
// the flush touched. That was cheap while an upload found its targets clean (the preceding
// publication had just cleaned them); with G766 the residents stay dirty through the guest's
// texture stream, which re-uploads the same pages of the 0x139 family many times a frame, and
// the eager flush mirrored each version separately: ~1.3 M texels a frame on MAP-15 / lavaboss
// for bytes no FBO reader ever sampled. A pending mirror is only OBSERVABLE through the target's
// FBO, so it is now applied when the FBO is about to be observed or written by anything else:
//   * the target's own wave (render / seed / escaped-writer check),
//   * the bind pass binding it (direct FBO source, G281/G338 views) — flushed before the texture
//     pass queues any view build and before the batch is submitted,
//   * the G310 logical / G309 composite and G282 fan-out builders, which read or write resident
//     FBOs directly,
//   * a GPU page pull INTO it (a later-rendered owner's bytes must land AFTER the older mirror),
//   * its publication and its owner pulls (already: g261Materialize, g766FlushOwnerMirrors).
// Between those points repeated uploads to the same texels coalesce into one patch.
//   DC2_G766_NO_LAZYMIR=1  rollback: flush every target's mirror at every GPU flush (G264)
static bool g766LazyMirOn()
{
    static const bool on = !envFlagEnabled("DC2_G766_NO_LAZYMIR") &&
                           !envFlagDisabled("DC2_G766_LAZYMIR");
    return on;
}
static void g766MirrorFlushOrDrop(int ti);
static void g766MirrorFlushAll();
static void g766MirrorFlushKey(uint32_t key);
static void g261Materialize(int ti, int cause);
static bool g305DrainRequired();

// Any owner that is about to be COPIED FROM must first absorb its own deferred upload mirror.
static void g766FlushOwnerMirrors(uint32_t ownerMask)
{
    for (uint32_t o = 0u; o < kG248TargetCount; ++o)
        if ((ownerMask & (1u << o)) != 0u && g_g261Res[o].mirPending)
            (void)g264FlushMirror(static_cast<int>(o));
}

static bool g766PullVerifyOn()
{
    static const bool on = envFlagEnabled("DC2_G766_PULL_VERIFY");
    return on;
}

// Width of the FBO behind a slot key (the source of a pull is always a resident slot).
static int g766KeyWidth(uint32_t key)
{
    for (uint32_t u = 0u; u < kG248TargetCount; ++u)
        if (g766SlotKey(u) == key)
            return g_g261Res[u].fbW;
    return 0;
}
// A width-mapped (fbp, width) pair for a backend READ of the surface behind `key`.
static uint32_t g766KeyReadFbp(uint32_t key)
{
    return key == kG766Narrow139Key ? 0x139u : key;
}

// ORACLE (perturbing, never timed): read back each copied rect from both FBOs and compare.
static void g766VerifyPulls(uint32_t t, uint32_t src, const std::vector<int32_t> &r6)
{
    static std::vector<uint32_t> s_a, s_b;
    const int sw = g766KeyWidth(src);
    const int dw = g_g261Res[t].fbW;
    for (size_t k = 0u; k + 6u <= r6.size(); k += 6u)
    {
        const int sx = r6[k], sy = r6[k + 1], dx = r6[k + 2], dy = r6[k + 3];
        const int w = r6[k + 4], h = r6[k + 5];
        if (sw <= 0 || dw <= 0 ||
            !g178_backend_read_color(g766KeyReadFbp(src), sw, 512, sy, h, s_a) ||
            !g178_backend_read_color(kG261Fbp[t], dw, 512, dy, h, s_b)) // width-mapped key
        {
            ++g_g766VerifyBad;
            continue;
        }
        uint64_t bad = 0u;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const size_t ia = static_cast<size_t>(y) * sw + static_cast<size_t>(sx + x);
                const size_t ib = static_cast<size_t>(y) * dw + static_cast<size_t>(dx + x);
                if (ia >= s_a.size() || ib >= s_b.size() || s_a[ia] != s_b[ib])
                    ++bad;
            }
        g_g766VerifyPages += static_cast<uint64_t>(w / 64);
        if (bad != 0u)
        {
            g_g766VerifyBad += bad;
            static uint32_t s_log = 0u;
            if (s_log++ < 16u)
                std::fprintf(stderr,
                             "[G766:verify-BAD] src=0x%x dst=0x%x rect=(%d,%d)->(%d,%d) %dx%d "
                             "bad=%llu\n",
                             src, kG261Fbp[t], sx, sy, dx, dy, w, h,
                             static_cast<unsigned long long>(bad));
        }
    }
}

// Issue page pulls NOW as standalone backend copies (FBO -> FBO, no readback). The destination FBO
// must already exist in slot t's current geometry (true for any dirty slot).
static bool g766PullNow(uint32_t t, const std::vector<int32_t> &rects7)
{
    if (rects7.empty())
        return true;
    // G766 lazy mirrors: an older pending guest write must land in t's FBO BEFORE a later
    // renderer's bytes are copied over it, never after.
    if (g_g261Res[t].mirPending && !g264FlushMirror(static_cast<int>(t)))
        return false;
    std::vector<int32_t> r6;
    r6.reserve(rects7.size());
    size_t k = 0u;
    bool ok = true;
    while (k + 7u <= rects7.size())
    {
        const uint32_t src = static_cast<uint32_t>(rects7[k]);
        r6.clear();
        while (k + 7u <= rects7.size() && static_cast<uint32_t>(rects7[k]) == src)
        {
            r6.insert(r6.end(), rects7.begin() + static_cast<std::ptrdiff_t>(k + 1u),
                      rects7.begin() + static_cast<std::ptrdiff_t>(k + 7u));
            k += 7u;
        }
        const bool copied = g280_backend_copy_color_rects(src, g766SlotKey(t), r6);
        if (copied && g766PullVerifyOn())
            g766VerifyPulls(t, src, r6);
        ok = copied && ok;
    }
    return ok;
}

static void g766Report();

// Before slot t publishes rows [dirtyLo, dirtyHi] to guest VRAM: pull every page of those rows whose
// newest bytes live in another resident FBO, so the publication writes the newest bytes.
static void g766PullBeforePublish(int ti)
{
    if (!g766OwnerOn() || ti < 0 || static_cast<uint32_t>(ti) >= kG248TargetCount)
        return;
    const uint32_t t = static_cast<uint32_t>(ti);
    const G261Res &r = g_g261Res[t];
    if (!r.dirty || r.dirtyHi < r.dirtyLo)
        return;
    static std::vector<int32_t> s_rects; // flush-owner thread only
    static std::vector<uint32_t> s_pages;
    s_rects.clear();
    s_pages.clear();
    uint32_t owners = 0u;
    const uint32_t n = g766CollectPulls(t, r.dirtyLo, r.dirtyHi, 0, -1, s_rects, &s_pages, &owners);
    if (n == 0u)
        return;
    g766FlushOwnerMirrors(owners);
    if (!g766PullNow(t, s_rects))
    {
        // The copy did not land: the publication below would write stale bytes for these pages.
        // Publish their owners AFTER this target so the newest bytes win in guest VRAM.
        ++g_g766PullFail;
        g766ForgetSeen(t, s_pages);
        return;
    }
    ++g_g766PubPulls;
    g_g766PubPullPages += n;
    g766Report();
}

// ===== Source reads of a dirty target that guest VRAM already satisfies =====
// A consumer that READS guest VRAM over `rd` (a transfer source, a CPU replay's texture/CLUT, an
// inline draw's texture) forces the legacy edges to publish every dirty target whose window
// overlaps `rd` — the whole window, 448 rows for 0x139. But a page of that window which is outside
// the dirty rows, or whose every pixel a guest upload rewrote after the last render (G291 coverage,
// mirrored into the FBO by G264), already holds the newest bytes in guest VRAM. When EVERY page the
// read overlaps is such a page, the publication is the identity for this reader and is skipped.
// Only the READ role qualifies: a write into a dirty target's pages keeps the legacy publication
// (the target's FBO would otherwise be stale where the write lands).
static uint64_t g_g766SrcSkips = 0u;
static bool g767OwnerVramOn()
{
    static const bool on = !envFlagEnabled("DC2_G767_NO_OWNERVRAM") &&
                           !envFlagDisabled("DC2_G767_OWNERVRAM");
    return on;
}
static bool g766SrcPagesCurrent(uint32_t t, const G144BlockRange &rd)
{
    if (!g766OwnerOn() || !rd.valid || t >= kG248TargetCount)
        return false;
    // ⭐ G769: a guest write still sitting in t's LAZY mirror makes its pages read as G536
    // guest-owned (g766ValidOwner -> -1), i.e. "newest in VRAM" — but a PARTIAL write (a PSMT8 /
    // PSMT4 upload into a CT32 page, `[G769:verify-BAD]` 1,024 of 2,048 words on fight) leaves the
    // page's other bytes newest in t's FBO. Apply the mirror first: the page is then owned again
    // and the test below sees the truth. Rollback DC2_G769_NO_SRCMIRFLUSH=1.
    static const bool s_g769Flush = !envFlagEnabled("DC2_G769_NO_SRCMIRFLUSH");
    if (s_g769Flush && g_g261Res[t].dirty && g_g261Res[t].mirPending)
    {
        // Only when the pending rows reach a page of the read inside the dirty interval.
        const G261Res &m = g_g261Res[t];
        bool touches = false;
        if (m.fbw != 0u && m.fbw <= 8u && m.mirHi >= m.mirLo)
        {
            const uint32_t base = framePageBaseToBlock(kG261Fbp[t]);
            const int prLo = std::max(std::max(0, m.dirtyLo), m.mirLo) / 32;
            const int prHi = std::min(std::min(511, m.dirtyHi), m.mirHi) / 32;
            for (int pr = prLo; pr <= prHi && !touches; ++pr)
                for (uint32_t pc = 0u; pc < m.fbw && !touches; ++pc)
                {
                    const uint32_t b0 = base + (static_cast<uint32_t>(pr) * m.fbw + pc) * 32u;
                    touches = !(b0 + 31u < rd.first || b0 > rd.last);
                }
        }
        else
            touches = true;
        if (touches)
            g766MirrorFlushOrDrop(static_cast<int>(t));
    }
    const G261Res &r = g_g261Res[t];
    if (!r.dirty)
        return true;
    if (r.fbw == 0u || r.fbw > 8u || r.fbW != static_cast<int>(r.fbw) * 64 ||
        r.dirtyHi < r.dirtyLo)
        return false;
    const bool cov = r.g291CovFbw == r.fbw;
    const uint32_t base = framePageBaseToBlock(kG261Fbp[t]);
    const int prLo = std::max(0, r.dirtyLo) / 32;
    const int prHi = std::min(511, r.dirtyHi) / 32;
    for (int pr = prLo; pr <= prHi; ++pr)
        for (uint32_t pc = 0u; pc < r.fbw; ++pc)
        {
            const uint32_t b0 = base + (static_cast<uint32_t>(pr) * r.fbw + pc) * 32u;
            if (b0 + 31u < rd.first || b0 > rd.last)
                continue; // the read does not touch this page
            // ⭐ G767: a page of the dirty INTERVAL that no FBO owns has its newest bytes in guest
            // VRAM. Every writer that makes a resident FBO newer than VRAM stamps ownership (a wave
            // stamps every page of its rows, a G767 fold its folded pages); pulls, mirrors and alias
            // copies only replicate an owner's or VRAM's bytes. So a dirty slot's rows between its
            // rendered bands — and every page another alias rendered and then published — no
            // longer force a publication for a READ. Rollback DC2_G767_NO_OWNERVRAM=1.
            if (g767OwnerVramOn() && g766ValidOwner(kG261Fbp[t] + static_cast<uint32_t>(pr) * r.fbw +
                                                    pc) < 0)
                continue;
            if (!cov)
                return false;
            for (int y = pr * 32; y < pr * 32 + 32; ++y)
                if (r.g291Cov[static_cast<size_t>(y)][pc] != ~uint64_t{0})
                    return false; // FBO-only bytes the read needs
        }
    return true;
}

// ⭐ G767: a page with NO valid FBO owner has its newest bytes in guest VRAM, and a DIRTY slot's copy
// of it (inside the slot's FBO-defined rows) equals guest VRAM once the slot's deferred mirror is
// applied: every guest write into a dirty slot's pages is mirrored into its FBO (G264/G766 mask
// mirror), a write that could not be is G536's guest-owned page (refused here), and a publication
// by another alias pulls this slot current first (g766CoherePublish). It is the same invariant
// g766CollectPulls relies on when it pulls only OWNED-elsewhere pages into a wave.
// The legacy answer was "not current", so every T8 view over two dirty aliases of one page (0x139
// wide + narrow) fell back to publishing the whole 448-row window. Rollback DC2_G767_NO_UNOWNED=1.
static bool g767UnownedCurrentOn()
{
    static const bool on = !envFlagEnabled("DC2_G767_NO_UNOWNED") &&
                           !envFlagDisabled("DC2_G767_UNOWNED");
    return on;
}
static uint64_t g_g767UnownedOk = 0u, g_g767UnownedRef = 0u;
static bool g767UnownedPageCurrent(uint32_t t, uint32_t page)
{
    if (!g767UnownedCurrentOn() || t >= kG248TargetCount)
        return false;
    const G261Res &r = g_g261Res[t];
    uint32_t row = 0u, col = 0u;
    if (!r.dirty || !g766SlotPagePos(t, page, row, col))
    {
        ++g_g767UnownedRef;
        return false;
    }
    const int y0 = static_cast<int>(row) * 32;
    if (r.covHi < r.covLo || y0 < r.covLo || y0 + 31 > r.covHi)
    {
        ++g_g767UnownedRef;
        return false;
    }
    if (r.mirPending)
    {
        g766MirrorFlushOrDrop(static_cast<int>(t));
        if (!g_g261Res[t].dirty)
        {
            ++g_g767UnownedRef;
            return false;
        }
    }
    if (g536GuestOwnsTargetPage(g_g261Res[t], kG261Fbp[t], row, col))
    {
        ++g_g767UnownedRef;
        return false;
    }
    ++g_g767UnownedOk;
    return true;
}

// ⭐ G767: the CLUT half of every T8 view admission (G281 / G310 / G598) refused whenever ANY dirty
// target's claimed WINDOW overlapped the CLUT — 0x13c's 512-row window swallows most of the family,
// so once 0x13c stayed dirty (G767 no longer publishes it on a CPU replay) every T8 view fell back.
// What the CLUT decode needs is that guest VRAM is current for the CLUT's own pages: exactly
// g766SrcPagesCurrent. Rollback DC2_G767_NO_CLUTPAGE=1 (whole-window overlap refuses).
static bool g767ClutPageOn()
{
    static const bool on = !envFlagEnabled("DC2_G767_NO_CLUTPAGE") &&
                           !envFlagDisabled("DC2_G767_CLUTPAGE");
    return on;
}
static bool g767ClutBlockedBy(uint32_t t, const G144BlockRange &clut)
{
    const G261Res &r = g_g261Res[t];
    if (!r.dirty)
        return false;
    if (!r.range.valid)
        return true;
    if (!g144RangeOverlaps(clut, r.range))
        return false;
    return !(g767ClutPageOn() && g766SrcPagesCurrent(t, clut));
}

// A view built from slot t's FBO needs page `page` current in t. True when t owns it, when t has
// seen the owner's current bytes, or after a GPU pull from the valid owner succeeds.
static bool g766SourceCurrentOrPulled(uint32_t t, uint32_t page)
{
    if (!g766OwnerOn() || t >= kG248TargetCount || page >= kG178PageCount)
        return false;
    const int o = g766ValidOwner(page);
    if (o < 0)
        return g767UnownedPageCurrent(t, page); // G767 (legacy: false, the foreign-owner rule)
    if (static_cast<uint32_t>(o) == t || g766SeenCurrent(t, page))
        return true;
    uint32_t srow = 0u, scol = 0u, drow = 0u, dcol = 0u;
    if (!g766SlotPagePos(static_cast<uint32_t>(o), page, srow, scol) ||
        !g766SlotPagePos(t, page, drow, dcol))
        return false;
    static std::vector<int32_t> s_rects;
    s_rects.clear();
    g766AppendRect(s_rects, g766SlotKey(static_cast<uint32_t>(o)), srow, scol, drow, dcol);
    g766FlushOwnerMirrors(1u << static_cast<uint32_t>(o));
    if (!g766PullNow(t, s_rects))
    {
        ++g_g766PullFail;
        return false;
    }
    g766MarkSeen(t, page);
    ++g_g766BindPulls;
    ++g_g766BindPullPages;
    return true;
}

// Publication trim (see g261Materialize): rows of a dirty window whose pages the guest fully
// rewrote since the last wave render need no readback.
static bool g766TrimOn()
{
    static const bool on = !envFlagEnabled("DC2_G766_NO_TRIM") && !envFlagDisabled("DC2_G766_TRIM");
    return on;
}
static uint64_t g_g766TrimCalls = 0u, g_g766TrimRows = 0u, g_g766TrimWhole = 0u;
static void g766NoteTrim(int ti, int lo0, int hi0, int lo, int hi)
{
    ++g_g766TrimCalls;
    const int kept = hi >= lo ? hi - lo + 1 : 0;
    g_g766TrimRows += static_cast<uint64_t>((hi0 - lo0 + 1) - kept);
    if (kept == 0)
        ++g_g766TrimWhole;
    if (g766StatOn())
    {
        static uint32_t s_n = 0u;
        if (s_n++ < 12u)
            std::fprintf(stderr, "[G766:trim] fbp=0x%03x rows=%d..%d -> %d..%d\n",
                         ti >= 0 ? kG261Fbp[ti] : 0u, lo0, hi0, lo, hi);
    }
}

// ===== Coherent publication across aliases =====
// Publishing slot T writes guest VRAM over its dirty rows and bumps those pages' generations. Any
// OTHER dirty slot U whose window covers one of those pages would then read the bump as an ESCAPED
// guest writer at its next wave (`genAtSkip != winNow` -> residency dropped loudly, U's own rendered
// rows lost) — which is why the legacy model could only ever publish overlapping aliases together.
// G766 instead (1) brings U's copy of every such page current on the GPU (a pull from the page's
// valid owner, when U is not already current) and (2) after the publication, excuses exactly those
// page bumps in U's baselines (genAtSkip is an exact per-page sum for page-aligned slots, G572), so
// U stays resident and correct. A page with NO valid FBO owner is never excused: that is a guest
// byte the legacy invariant must still see.
struct G766Excuse
{
    uint8_t slot;
    uint16_t page;
    uint64_t oldGen;
};
static std::vector<G766Excuse> s_g766Excuse; // flush-owner thread only
static uint64_t g_g766CoherePulls = 0u, g_g766CoherePages = 0u, g_g766Excused = 0u;

static void g766CoherePublish(int ti)
{
    s_g766Excuse.clear();
    if (!g766OwnerOn() || ti < 0 || static_cast<uint32_t>(ti) >= kG248TargetCount)
        return;
    const uint32_t T = static_cast<uint32_t>(ti);
    const G261Res &rt = g_g261Res[T];
    if (!rt.dirty || rt.dirtyHi < rt.dirtyLo || rt.fbw == 0u || rt.fbw > 8u || rt.fbW <= 0)
        return;
    const uint32_t cols = static_cast<uint32_t>(rt.fbW) / 64u;
    const int prLo = std::max(0, rt.dirtyLo) / 32;
    const int prHi = std::min(511, rt.dirtyHi) / 32;
    static std::vector<int32_t> s_rects;
    static std::vector<uint32_t> s_pages;
    for (uint32_t U = 0u; U < kG248TargetCount; ++U)
    {
        if (U == T)
            continue;
        const G261Res &ru = g_g261Res[U];
        if (!ru.dirty || ru.winHi < ru.winLo || ru.fbw == 0u || ru.fbw > 8u || ru.fbW <= 0)
            continue;
        s_rects.clear();
        s_pages.clear();
        uint32_t owners = 0u;
        const size_t excuseBase = s_g766Excuse.size();
        for (int pr = prLo; pr <= prHi; ++pr)
            for (uint32_t pc = 0u; pc < cols && pc < rt.fbw; ++pc)
            {
                const uint32_t page = kG261Fbp[T] + static_cast<uint32_t>(pr) * rt.fbw + pc;
                if (page >= kG178PageCount)
                    continue;
                uint32_t urow = 0u, ucol = 0u;
                if (!g766SlotPagePos(U, page, urow, ucol))
                    continue;
                const int uy0 = static_cast<int>(urow) * 32;
                if (uy0 + 31 < ru.winLo || uy0 > ru.winHi)
                    continue;
                const int o = g766ValidOwner(page);
                if (o < 0)
                {
                    // ⭐ G767: a page NO FBO owns is published with T's copy, which equals guest
                    // VRAM (T's lazy mirror is applied before its publication) — the write changes
                    // no byte, only the generation. When U's copy is current too (not guest-owned
                    // in either slot), excuse that bump in U instead of letting U's next wave read it
                    // as an escaped writer and drop U's residency (and its rendered rows). A page
                    // G536 calls guest-owned in T or U stays unexcused: that is a real guest byte.
                    // Rollback DC2_G767_NO_UNOWNEDEXCUSE=1.
                    static const bool s_g767Excuse = !envFlagEnabled("DC2_G767_NO_UNOWNEDEXCUSE");
                    uint32_t trow = 0u, tcol = 0u;
                    if (s_g767Excuse && g766SlotPagePos(T, page, trow, tcol) &&
                        !g536GuestOwnsTargetPage(rt, kG261Fbp[T], trow, tcol) &&
                        !g536GuestOwnsTargetPage(ru, kG261Fbp[U], urow, ucol))
                        s_g766Excuse.push_back(G766Excuse{static_cast<uint8_t>(U),
                                                          static_cast<uint16_t>(page),
                                                          g766PageGen(page)});
                    continue;
                }
                if (static_cast<uint32_t>(o) != U && !g766SeenCurrent(U, page))
                {
                    uint32_t srow = 0u, scol = 0u;
                    if (!g766SlotPagePos(static_cast<uint32_t>(o), page, srow, scol))
                        continue;
                    g766AppendRect(s_rects, g766SlotKey(static_cast<uint32_t>(o)), srow, scol, urow, ucol);
                    g766MarkSeen(U, page);
                    s_pages.push_back(page);
                    owners |= 1u << static_cast<uint32_t>(o);
                }
                s_g766Excuse.push_back(G766Excuse{static_cast<uint8_t>(U),
                                                  static_cast<uint16_t>(page), g766PageGen(page)});
            }
        if (!s_rects.empty())
        {
            g766FlushOwnerMirrors(owners);
            if (!g766PullNow(U, s_rects))
            {
                // U could not be made current: do not excuse its pages (the legacy invariant then
                // treats the publication as it always did).
                ++g_g766PullFail;
                g766ForgetSeen(U, s_pages);
                s_g766Excuse.resize(excuseBase);
                continue;
            }
            ++g_g766CoherePulls;
            g_g766CoherePages += s_pages.size();
        }
    }
}

// After the publication's generation bump: re-anchor exactly the excused pages in each U.
static void g766ApplyExcuses()
{
    for (const G766Excuse &e : s_g766Excuse)
    {
        const uint64_t now = g766PageGen(e.page);
        if (now == e.oldGen)
            continue;
        G261Res &ru = g_g261Res[e.slot];
        if (!ru.dirty)
            continue;
        uint32_t row = 0u, col = 0u;
        if (!g766SlotPagePos(e.slot, e.page, row, col))
            continue;
        ru.genAtSkip += now - e.oldGen;
        const uint32_t idx = row * ru.fbw + col;
        if (ru.g536PageGenFbw == ru.fbw && idx < ru.g536PageGen.size() &&
            ru.g536PageGen[idx] == e.oldGen)
            ru.g536PageGen[idx] = now;
        if (g_g766SeenGen[e.slot][e.page] == e.oldGen)
            g_g766SeenGen[e.slot][e.page] = now;
        ++g_g766Excused;
    }
    s_g766Excuse.clear();
}

// Before an FBO-direct bind of slot t over GS rows [lo, hi]: pull every page of those rows that is
// newer in another FBO. Returns false if a needed pull could not be issued (caller falls back).
static bool g766PullForBind(uint32_t t, int lo, int hi)
{
    if (!g766OwnerOn() || !g766PullsAllowed())
        return false;
    static std::vector<int32_t> s_rects;
    static std::vector<uint32_t> s_pages;
    s_rects.clear();
    s_pages.clear();
    uint32_t owners = 0u;
    const uint32_t n = g766CollectPulls(t, lo, hi, 0, -1, s_rects, &s_pages, &owners);
    if (n == 0u)
        return true;
    g766FlushOwnerMirrors(owners);
    if (!g766PullNow(t, s_rects))
    {
        ++g_g766PullFail;
        g766ForgetSeen(t, s_pages);
        return false;
    }
    ++g_g766BindPulls;
    g_g766BindPullPages += n;
    return true;
}

// Wave-start plan for slot t, called BEFORE the upload decision: if pulls cannot be armed for this
// batch, publish every other owner of a page in the window now (the legacy behaviour), so the
// upload decision below sees the bytes in guest VRAM.
static void g766PrepareWave(int ti, int lo, int hi)
{
    if (!g766OwnerOn() || ti < 0 || static_cast<uint32_t>(ti) >= kG248TargetCount)
        return;
    if (g766PullsAllowed() && !g305DrainRequired())
        return; // pulls will be armed after the upload decision
    const uint32_t t = static_cast<uint32_t>(ti);
    const G261Res &r = g_g261Res[t];
    if (r.fbw == 0u || r.fbw > 8u || r.fbW <= 0 || hi < lo)
        return;
    const uint32_t cols = static_cast<uint32_t>(r.fbW) / 64u;
    for (int pr = std::max(0, lo) / 32; pr <= std::min(511, hi) / 32; ++pr)
        for (uint32_t pc = 0u; pc < cols && pc < r.fbw; ++pc)
        {
            const uint32_t page = kG261Fbp[t] + static_cast<uint32_t>(pr) * r.fbw + pc;
            if (page >= kG178PageCount)
                continue;
            const int o = g766ValidOwner(page);
            if (o >= 0 && static_cast<uint32_t>(o) != t && !g766SeenCurrent(t, page))
            {
                ++g_g766Fallbacks;
                G766MatSite g766s(kG766SiteOwnerFallback);
                g261Materialize(o, kG261MatResync);
            }
        }
}

// Wave-start pulls, called after the upload decision and immediately before a SYNCHRONOUS submit.
// `win` is the FBO-defined row window after this batch, `up` the rows the VRAM seed replaces
// (empty when no upload). Returns the number of pages armed.
static uint32_t g766ArmWavePulls(int ti, uint32_t fboKey, int winLo, int winHi, bool uploadFb,
                                 int upLo, int upHi, std::vector<uint32_t> &pagesOut)
{
    pagesOut.clear();
    if (!g766OwnerOn() || !g766PullsAllowed() || ti < 0 ||
        static_cast<uint32_t>(ti) >= kG248TargetCount)
        return 0u;
    const uint32_t t = static_cast<uint32_t>(ti);
    static std::vector<int32_t> s_rects;
    s_rects.clear();
    uint32_t owners = 0u;
    const uint32_t n = g766CollectPulls(t, winLo, winHi, uploadFb ? upLo : 0, uploadFb ? upHi : -1,
                                        s_rects, &pagesOut, &owners);
    if (n == 0u)
        return 0u;
    g766FlushOwnerMirrors(owners);
    if (!g766_backend_arm_pulls(fboKey, s_rects))
    {
        g766ForgetSeen(t, pagesOut);
        pagesOut.clear();
        ++g_g766PullFail;
        return 0u;
    }
    ++g_g766WavePulls;
    g_g766WavePullPages += n;
    return n;
}

// After the submit returns: if the armed pulls did not land, the FBO is stale for those pages —
// forget them so the next consumer pulls again, and count it loudly.
static void g766ConfirmWavePulls(int ti, const std::vector<uint32_t> &pages)
{
    if (pages.empty())
        return;
    if (g766_backend_last_pulls_applied())
        return;
    ++g_g766PullFail;
    if (ti >= 0 && static_cast<uint32_t>(ti) < kG248TargetCount)
        g766ForgetSeen(static_cast<uint32_t>(ti), pages);
    static uint32_t s_n = 0u;
    if (s_n++ < 8u)
        std::fprintf(stderr, "[G766:pullfail] wave pulls did not land ti=%d pages=%zu\n", ti,
                     pages.size());
}

static void g766Report()
{
    if (!g766StatOn())
        return;
    static uint64_t s_n = 0u;
    if ((++s_n % 512u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G766:own] stamps=%llu wavePulls=%llu/%llu pubPulls=%llu/%llu "
                 "bindPulls=%llu/%llu bindSkips=%llu fallbacks=%llu fail=%llu g310owner=%llu "
                 "cohere=%llu/%llu excused=%llu trim(calls=%llu rows=%llu whole=%llu) "
                 "verify(pages=%llu bad=%llu)\n",
                 (unsigned long long)g_g766Stamps, (unsigned long long)g_g766WavePulls,
                 (unsigned long long)g_g766WavePullPages, (unsigned long long)g_g766PubPulls,
                 (unsigned long long)g_g766PubPullPages, (unsigned long long)g_g766BindPulls,
                 (unsigned long long)g_g766BindPullPages, (unsigned long long)g_g766BindSkips,
                 (unsigned long long)g_g766Fallbacks, (unsigned long long)g_g766PullFail,
                 (unsigned long long)g_g766G310Owner, (unsigned long long)g_g766CoherePulls,
                 (unsigned long long)g_g766CoherePages, (unsigned long long)g_g766Excused,
                 (unsigned long long)g_g766TrimCalls, (unsigned long long)g_g766TrimRows,
                 (unsigned long long)g_g766TrimWhole, (unsigned long long)g_g766VerifyPages,
                 (unsigned long long)g_g766VerifyBad);
    g766_backend_report();
}

// ===== G766: THE TEXEL FOOTPRINT OF A READ, NOT ITS DECLARED TEXTURE =====
// `g144TextureRange` is TEX0's whole declared rectangle (1<<TW by 1<<TH). The overlap test that
// decides whether a texture read forces a dirty resident's publication used it, so a 24x14-texel
// HUD sprite reading a 512x512 descriptor at 0x2860 published 0x15b's whole 416-row window
// (`[G737:texrej] baseForeign` = 27 % of dragon's texture readback rows) although not one sampled
// texel lies in 0x15b's pages. For an FST primitive the sampled texels are bounded exactly: every
// interpolated UV lies in the hull of the vertex UVs, a filter or boundary-rounding tap reaches at
// most one texel further, and GS CLAMP folds taps past an edge onto the edge texel. A REPEAT read
// that would cross an edge (and every STQ read) keeps the declared rectangle.
// Safe for the texture cache: a decode made while an unsampled page was stale is only ever reused
// by a draw whose own footprint avoids that page, because any draw that samples it runs this same
// test, publishes the owner, and moves the page generations the registry re-validates on.
//   DC2_G766_NO_TEXFOOT=1  rollback: every read tests the declared rectangle
static bool g766TexFootprintOn()
{
    static const bool on = !envFlagEnabled("DC2_G766_NO_TEXFOOT") &&
                           !envFlagDisabled("DC2_G766_TEXFOOT");
    return on;
}
static uint64_t g_g766FootNarrow = 0u, g_g766FootFull = 0u;
static bool g766TexFootprint(const G144Entry &e, G144BlockRange &out)
{
    if (!g766TexFootprintOn() || !e.prim.tme || !e.prim.fst)
        return false;
    const bool sprite = e.prim.type == GS_PRIM_SPRITE;
    if (!sprite && e.prim.type != GS_PRIM_TRIANGLE && e.prim.type != GS_PRIM_TRISTRIP &&
        e.prim.type != GS_PRIM_TRIFAN)
        return false;
    const GSTex0Reg &tex = e.ctx.tex0;
    const int texW = 1 << std::min<uint8_t>(tex.tw, 15u);
    const int texH = 1 << std::min<uint8_t>(tex.th, 15u);
    const int nv = sprite ? 2 : 3;
    int u0 = INT32_MAX, u1 = INT32_MIN, v0 = INT32_MAX, v1 = INT32_MIN;
    for (int k = 0; k < nv; ++k)
    {
        const int uu = static_cast<int>(e.v[k].u >> 4);
        const int vv = static_cast<int>(e.v[k].v >> 4);
        u0 = std::min(u0, uu); u1 = std::max(u1, uu);
        v0 = std::min(v0, vv); v1 = std::max(v1, vv);
    }
    u0 -= 1; v0 -= 1; u1 += 1; v1 += 1; // one filter / rounding tap on every side
    const uint32_t wms = e.ctx.clamp & 3u, wmt = (e.ctx.clamp >> 2) & 3u;
    const auto axis = [](int &lo, int &hi, int n, uint32_t wrap) -> bool {
        if (lo >= 0 && hi <= n - 1)
            return true;       // no wrap or clamp reached
        if (wrap != 1u)
            return false;      // REPEAT / REGION: the declared rectangle
        lo = std::max(lo, 0);  // GS CLAMP folds the taps onto the edge texels
        hi = std::min(hi, n - 1);
        return lo <= hi;
    };
    if (!axis(u0, u1, texW, wms) || !axis(v0, v1, texH, wmt))
    {
        ++g_g766FootFull;
        return false;
    }
    out = g144RangeForRect(tex.tbp0, tex.psm, tex.tbw, static_cast<uint32_t>(u0),
                           static_cast<uint32_t>(v0), static_cast<uint32_t>(u1 - u0 + 1),
                           static_cast<uint32_t>(v1 - v0 + 1));
    if (!out.valid)
        return false;
    ++g_g766FootNarrow;
    return true;
}
