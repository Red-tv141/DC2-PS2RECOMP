// ===== G765: CARRY THE COALESCED DISPLAY PUBLICATION ACROSS THE FRAME BOUNDARY =====
//
// THE ROUND TRIP THIS DELETES. G276 coalesces a frame's display-colour readbacks into one pending
// record and the frame boundary publishes it unconditionally (rasterizer_draw_sprite.inc, cause 5):
// a synchronous FBO->guest-VRAM readback of the whole display rectangle, once per frame. At S = 3
// the readback waits for the ENTIRE frame's queued GPU work, so it serialises the GS executor
// behind the GPU. G765's stack profile of the promoted G764 image (dragon, 1600x900) books
// `g276FlushPendingDisplay <- g144FlushPending <- mgEndFrame` at 15.0 % of the executor's wall.
//
// WHY IT IS NOT NEEDED. The boundary was a hard consumer edge because "presentation, frame dumps
// and the next frame's texture decodes read guest VRAM" (the comment at that site). Since G675 the
// high-resolution latch reads the display FBO on the GPU, since G764 the host presents a shared GPU
// snapshot, and frame dumps copy the latched presentation — none of them read guest VRAM. Every
// other reader already has an edge: ranged transfer edges and the CPU-replay prologue call
// `g276FlushPendingDisplayForRanges`, un-ranged edges call `g276FlushPendingDisplay`.
//
// THE MODEL. At the boundary the pending record moves into a CARRY slot instead of being read back.
// A carried record is exactly a G276 pending record that survived a boundary: the FBO holds the
// newer bytes for its rows, guest VRAM holds the bytes of its last publication, and no generation
// has moved (so the texture registry and the FBO snapshot are exactly as a publication-free
// same-target continuation leaves them). It is published only by a real consumer:
//
//   * every unconditional G276 flush               -> all carries          (g276FlushPendingDisplay)
//   * every ranged transfer / CPU-replay edge      -> overlapping carries  (…ForRanges)
//   * a GPU batch that reads its pages (texture, CLUT, depth, or colour through ANOTHER frame
//     base), or renders into it while a different display target is pending -> that carry
//   * a guest-VRAM presentation read (CPU latch fallback, logical presentation) -> overlapping
//   * a third display target needing a slot         -> the oldest carry
// A GPU batch that renders INTO the carried surface reclaims it as the live pending record — the
// same state a same-target continuation inside one frame has always had.
//
// Fails closed everywhere: an unknown range publishes. G288 (the GPU-batch bridge for the LIVE
// record) is default-OFF, so the carry audit does not depend on it.
//
// LEVERS
//   DC2_G765_NO_DISPLAY_CARRY=1   rollback: publish at the boundary exactly as before
//   DC2_G765_CARRY_POISON=1       ORACLE: write magenta over a carried record's guest-VRAM rows and
//                                 bump their generations, so any reader that skipped an edge shows
//                                 magenta (and a registry re-decode picks it up). Never timed.
//   DC2_G765_STAT=1               [G765:carry] census

static constexpr int kG765CarrySlots = 2; // double-buffered display: one live + two carried
static G276PendDisplay s_g765Carry[kG765CarrySlots];
static uint64_t s_g765CarryAge[kG765CarrySlots] = {};
static uint64_t s_g765CarryClock = 0u;
static std::atomic<uint64_t> g_g765Swapped{0}, g_g765Discarded{0};
static std::atomic<uint64_t> g_g765Carried{0}, g_g765Reclaimed{0}, g_g765Merged{0},
    g_g765PubAll{0}, g_g765PubRanged{0}, g_g765PubGpu{0}, g_g765PubPresent{0},
    g_g765PubEvict{0}, g_g765PubRows{0}, g_g765Poisoned{0};

static bool g765StatOn()
{
    static const bool on = envFlagEnabled("DC2_G765_STAT");
    return on;
}

static bool g765CarryPoisonOn()
{
    static const bool on = envFlagEnabled("DC2_G765_CARRY_POISON");
    return on;
}

static bool g765CarryOn()
{
    // The presentation-read audit (g289_prepare_presentation_read) is the edge that publishes a
    // carry before a CPU latch reads guest VRAM. It runs only when copyFrameToHostRgbaUnlocked's
    // own G289 predicate holds, so the carry requires the same predicate.
    static const bool on =
        !envFlagEnabled("DC2_G765_NO_DISPLAY_CARRY") &&
        !envFlagDisabled("DC2_G765_DISPLAY_CARRY") &&
        !envFlagEnabled("DC2_G289_NO_GPU_LOCAL_COPY") &&
        (std::getenv("DC2_G289_GPU_LOCAL_COPY") == nullptr ||
         envFlagEnabled("DC2_G289_GPU_LOCAL_COPY")) &&
        g276DisplayRbOn();
    return on;
}

static bool g765DiscardOn()
{
    static const bool on = !envFlagEnabled("DC2_G765_NO_CARRY_DISCARD");
    return on;
}

static bool g765AnyCarry()
{
    for (int i = 0; i < kG765CarrySlots; ++i)
        if (s_g765Carry[i].active)
            return true;
    return false;
}

static G144BlockRange g765CarryRange(const G276PendDisplay &p)
{
    if (!p.active || p.fbW <= 0 || p.rowHi < p.rowLo)
        return {};
    return g144RangeForRect(p.fbBlock, GS_PSM_CT32, p.fbw, 0u, static_cast<uint32_t>(p.rowLo),
                            static_cast<uint32_t>(p.fbW),
                            static_cast<uint32_t>(p.rowHi - p.rowLo + 1));
}

// G768: carried-record publications by CAUSE (the edge that forced each one).
static std::atomic<uint64_t> g_g768CarryPubCause[16];
static void g765Report()
{
    if (!g765StatOn())
        return;
    static uint64_t s_n = 0u; // flush-owner thread only
    if ((++s_n % 600u) != 1u)
        return;
    std::fprintf(stderr,
                 "[G765:carry] carried=%llu reclaimed=%llu swapped=%llu discarded=%llu merged=%llu pub(all=%llu ranged=%llu "
                 "gpu=%llu present=%llu evict=%llu rows=%llu) poisoned=%llu live=%d/%d\n",
                 (unsigned long long)g_g765Carried.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765Reclaimed.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765Swapped.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765Discarded.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765Merged.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765PubAll.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765PubRanged.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765PubGpu.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765PubPresent.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765PubEvict.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765PubRows.load(std::memory_order_relaxed),
                 (unsigned long long)g_g765Poisoned.load(std::memory_order_relaxed),
                 s_g765Carry[0].active ? 1 : 0, s_g765Carry[1].active ? 1 : 0);
    std::fprintf(stderr, "[G768:carrycause]");
    for (int c = 0; c < 16; ++c)
    {
        const uint64_t n = g_g768CarryPubCause[c].load(std::memory_order_relaxed);
        if (n != 0u)
            std::fprintf(stderr, " c%d=%llu", c, (unsigned long long)n);
    }
    std::fprintf(stderr, "\n");
}

// Publish one carried record through the unchanged G276 publication body: swap it into the live
// slot, run the core, restore the live record. The core reads and clears only `s_g276Pend`, so the
// live record is untouched by construction.
static void g765PublishCarry(int slot, int cause, std::atomic<uint64_t> &counter)
{
    if (slot < 0 || slot >= kG765CarrySlots || !s_g765Carry[slot].active)
        return;
    const G276PendDisplay carried = s_g765Carry[slot];
    s_g765Carry[slot] = G276PendDisplay{};
    const G276PendDisplay live = s_g276Pend;
    s_g276Pend = carried;
    g276FlushPendingDisplayCore(cause);
    // A failed core publication leaves s_g276Pend cleared and invalidates the FBO snapshot, i.e.
    // the next batch re-uploads from VRAM — the same bounded fallback the live record has.
    s_g276Pend = live;
    counter.fetch_add(1u, std::memory_order_relaxed);
    g_g768CarryPubCause[cause & 15].fetch_add(1u, std::memory_order_relaxed);
    g_g765PubRows.fetch_add(static_cast<uint64_t>(carried.rowHi - carried.rowLo + 1),
                            std::memory_order_relaxed);
}

static void g765PublishAllCarries(int cause)
{
    for (int i = 0; i < kG765CarrySlots; ++i)
        if (s_g765Carry[i].active)
            g765PublishCarry(i, cause, g_g765PubAll);
}

static void g765PublishCarriesForRanges(int cause, const G144BlockRange &src,
                                        const G144BlockRange &dst, bool srcUnknown,
                                        bool dstUnknown)
{
    for (int i = 0; i < kG765CarrySlots; ++i)
    {
        if (!s_g765Carry[i].active)
            continue;
        const G144BlockRange c = g765CarryRange(s_g765Carry[i]);
        const bool hit = srcUnknown || dstUnknown || !c.valid ||
                         (src.valid && g144RangeOverlaps(c, src)) ||
                         (dst.valid && g144RangeOverlaps(c, dst));
        if (hit)
            g765PublishCarry(i, cause, g_g765PubRanged);
    }
}

// Presentation reads guest VRAM directly (CPU latch path, logical presentation authority).
static void g765PublishCarriesForPresentationRead(const G144BlockRange &read, bool unknown)
{
    for (int i = 0; i < kG765CarrySlots; ++i)
    {
        if (!s_g765Carry[i].active)
            continue;
        const G144BlockRange c = g765CarryRange(s_g765Carry[i]);
        if (unknown || !c.valid || !read.valid || g144RangeOverlaps(c, read))
            g765PublishCarry(i, 13, g_g765PubPresent);
    }
}

static bool g765CarryLive(int evictCause);
static void g765PoisonCarried(const G276PendDisplay &p);

// The texels a GS-coordinate (FST) primitive can actually sample: the vertices' UV bounding box,
// widened by one texel on every side for bilinear taps. When that box lies inside the texture no
// wrap mode can reach any other texel, so the block range of the box is exact-or-wider. Anything
// else (STQ coordinates, mipmapping, a box that leaves the texture) takes TEX0's declared range.
static G144BlockRange g765SampledTextureRange(const G144Entry &e)
{
    const GSTex0Reg &tex = e.ctx.tex0;
    const bool mip = ((e.ctx.tex1 >> 2) & 7ull) != 0ull;
    const int nv = e.prim.type == GS_PRIM_SPRITE ? 2
                 : (e.prim.type == GS_PRIM_TRIANGLE || e.prim.type == GS_PRIM_TRISTRIP ||
                    e.prim.type == GS_PRIM_TRIFAN) ? 3 : 0;
    if (!e.prim.fst || mip || nv == 0)
        return g144TextureRange(e);
    const int texW = 1 << std::min<uint8_t>(tex.tw, 15u);
    const int texH = 1 << std::min<uint8_t>(tex.th, 15u);
    int uMin = 1 << 30, uMax = -1, vMin = 1 << 30, vMax = -1;
    for (int i = 0; i < nv; ++i)
    {
        uMin = std::min(uMin, static_cast<int>(e.v[i].u) >> 4);
        uMax = std::max(uMax, (static_cast<int>(e.v[i].u) + 15) >> 4);
        vMin = std::min(vMin, static_cast<int>(e.v[i].v) >> 4);
        vMax = std::max(vMax, (static_cast<int>(e.v[i].v) + 15) >> 4);
    }
    uMin -= 1; vMin -= 1; uMax += 1; vMax += 1;
    // CLAMP / REGION_CLAMP pin every tap inside the texture (or the region), so the box is clamped
    // there; REPEAT / REGION_REPEAT can wrap an out-of-range tap anywhere, so those fall back.
    const uint64_t clampReg = e.ctx.clamp;
    const uint32_t wms = static_cast<uint32_t>(clampReg & 3ull);
    const uint32_t wmt = static_cast<uint32_t>((clampReg >> 2) & 3ull);
    auto clampAxis = [](uint32_t wm, uint64_t reg, int shiftMin, int shiftMax, int size,
                        int &lo, int &hi) -> bool {
        if (lo >= 0 && hi < size)
            return true;
        if (wm == 1u)
        {
            lo = std::max(lo, 0);
            hi = std::min(hi, size - 1);
            return lo <= hi;
        }
        if (wm == 2u)
        {
            const int rmin = static_cast<int>((reg >> shiftMin) & 0x3FFull);
            const int rmax = static_cast<int>((reg >> shiftMax) & 0x3FFull);
            lo = std::max(lo, std::min(rmin, size - 1));
            hi = std::min(hi, std::min(rmax, size - 1));
            return lo <= hi;
        }
        return false;
    };
    if (!clampAxis(wms, clampReg, 4, 14, texW, uMin, uMax) ||
        !clampAxis(wmt, clampReg, 24, 34, texH, vMin, vMax))
        return g144TextureRange(e);
    return g144RangeForRect(tex.tbp0, tex.psm, tex.tbw, static_cast<uint32_t>(uMin),
                            static_cast<uint32_t>(vMin), static_cast<uint32_t>(uMax - uMin + 1),
                            static_cast<uint32_t>(vMax - vMin + 1));
}
// A SPRITE that writes every pixel of rows [rowLo, rowHi] x [0, fbW) of its own frame without
// reading the destination: no blending, no antialiasing, full FBMSK, an alpha test that cannot fail
// (off or ALWAYS — texel alpha varies), no destination-alpha test, depth test off or ALWAYS, and the
// exact drawSprite ceil-rule rectangle clipped to the scissor covering the rows. Texturing and fog
// only change WHAT is written, never WHETHER.
static bool g765SpriteOverwritesRows(const G144Entry &e, int fbW, int rowLo, int rowHi)
{
    const GSContext &c = e.ctx;
    if (e.prim.type != GS_PRIM_SPRITE || e.prim.abe || e.prim.aa1 || c.frame.fbmsk != 0u)
        return false;
    const uint64_t test = c.test;
    const bool ate = (test & 1ull) != 0ull;
    const uint32_t atst = static_cast<uint32_t>((test >> 1) & 7ull);
    const bool date = ((test >> 14) & 1ull) != 0ull;
    const bool zte = ((test >> 16) & 1ull) != 0ull;
    const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3ull);
    if ((ate && atst != 1u) || date || (zte && ztst != 1u))
        return false;
    if (envFlagEnabled("DC2_G365_NO_SPRITE_EDGE"))
        return false; // the truncate rule's rectangle is not modelled here
    const int ofx = static_cast<int>(c.xyoffset.ofx >> 4);
    const int ofy = static_cast<int>(c.xyoffset.ofy >> 4);
    float fx0 = e.v[0].x - static_cast<float>(ofx), fx1 = e.v[1].x - static_cast<float>(ofx);
    float fy0 = e.v[0].y - static_cast<float>(ofy), fy1 = e.v[1].y - static_cast<float>(ofy);
    if (fx0 > fx1)
        std::swap(fx0, fx1);
    if (fy0 > fy1)
        std::swap(fy0, fy1);
    const int x0 = std::max<int>(static_cast<int>(std::ceil(fx0)), c.scissor.x0);
    const int y0 = std::max<int>(static_cast<int>(std::ceil(fy0)), c.scissor.y0);
    const int x1 = std::min<int>(static_cast<int>(std::ceil(fx1)) - 1, c.scissor.x1);
    const int y1 = std::min<int>(static_cast<int>(std::ceil(fy1)) - 1, c.scissor.y1);
    return x0 <= 0 && x1 >= fbW - 1 && y0 <= rowLo && y1 >= rowHi;
}

// GPU batch consumer audit, run before the batch uploads its framebuffer, decodes textures/CLUTs or
// sets up depth (i.e. beside the G288 bridge). Same per-entry predicate as G288, applied to every
// carried record, plus the reclaim decision for entries that render into the carried surface.
static void g765AuditCarriesForGpuBatch(uint32_t directDisplaySrc)
{
    if (g_g144List.empty())
        return;
    for (int slot = 0; slot < kG765CarrySlots; ++slot)
    {
        G276PendDisplay &c = s_g765Carry[slot];
        if (!c.active)
            continue;
        const G144BlockRange carried = g765CarryRange(c);
        if (!carried.valid)
        {
            g765PublishCarry(slot, 0, g_g765PubGpu);
            continue;
        }
        if (g735BatchCannotTouch(carried))
            continue; // O(intervals) proof: no entry reads or writes the carried pages
        bool publish = false;
        bool rendersInto = false;
        bool firstOverwrites = false;
        for (size_t i = 0; i < g_g144List.size() && !publish; ++i)
        {
            const G144Entry &e = g_g144List[i];
            const bool sameSurface = e.ctx.frame.fbp == c.fbp && e.ctx.frame.fbw == c.fbw &&
                                     e.ctx.frame.psm == GS_PSM_CT32;
            if (sameSurface)
            {
                if (!rendersInto)
                {
                    firstOverwrites = g765DiscardOn() &&
                                      g765SpriteOverwritesRows(e, c.fbW, c.rowLo, c.rowHi);
                    if (g765StatOn())
                    {
                        static uint32_t s_n = 0u; // flush-owner thread only
                        if (s_n++ < 8u)
                            std::fprintf(stderr,
                                         "[G765:first] carry(fbp=0x%x rows=%d..%d) prim=%u tme=%u "
                                         "abe=%u aa1=%u fbmsk=0x%x test=0x%llx v=(%.2f,%.2f)-(%.2f,%.2f) "
                                         "of=(%u,%u) sc=%u,%u..%u,%u overwrites=%u\n",
                                         c.fbp, c.rowLo, c.rowHi, static_cast<unsigned>(e.prim.type),
                                         e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u,
                                         e.prim.aa1 ? 1u : 0u, e.ctx.frame.fbmsk,
                                         static_cast<unsigned long long>(e.ctx.test), e.v[0].x,
                                         e.v[0].y, e.v[1].x, e.v[1].y, e.ctx.xyoffset.ofx >> 4,
                                         e.ctx.xyoffset.ofy >> 4, e.ctx.scissor.x0,
                                         e.ctx.scissor.y0, e.ctx.scissor.x1, e.ctx.scissor.y1,
                                         firstOverwrites ? 1u : 0u);
                    }
                }
                rendersInto = true;
            }
            else
            {
                const G144BlockRange color = g144TargetRange(e);
                const bool nonEmpty = e.ctx.scissor.x1 >= e.ctx.scissor.x0 &&
                                      e.ctx.scissor.y1 >= e.ctx.scissor.y0;
                if ((nonEmpty && !color.valid) ||
                    (color.valid && g144RangeOverlaps(carried, color)))
                    publish = true;
            }
            const G254Surface depth = g254DepthSurface(e);
            if (depth.active && (!depth.range.valid || g144RangeOverlaps(carried, depth.range)))
                publish = true;
            // Entry 0 of a G289 direct-display batch samples the display FBO itself
            // (`s_g261FboSrc[0] = token` in g178TryFlushGpu), never guest VRAM: its texture range
            // cannot observe a carried record's stale bytes.
            const bool fboTexture = directDisplaySrc != 0u && i == 0u;
            if (e.prim.tme && !fboTexture)
            {
                const G144BlockRange tex = g765SampledTextureRange(e);
                if (!tex.valid || g144RangeOverlaps(carried, tex))
                    publish = true;
                if (g144IsPalettedPsm(e.ctx.tex0.psm))
                {
                    const G144BlockRange clut = g144ClutRange(e);
                    if (!clut.valid || g144RangeOverlaps(carried, clut))
                        publish = true;
                }
            }
        }
        bool otherLive = false;
        if (publish && g765StatOn())
        {
            static uint32_t s_n = 0u; // flush-owner thread only
            if (s_n++ < 16u)
            {
                const G144Entry &e0 = g_g144List.front();
                const G144BlockRange cr = g765CarryRange(c);
                const G144BlockRange tr = e0.prim.tme ? g765SampledTextureRange(e0) : G144BlockRange{};
                std::fprintf(stderr,
                             "[G765:carrypub+] carry=%x..%x tex=%x..%x tbw=%u tw=%u th=%u fst=%u "
                             "uv=(%u,%u)-(%u,%u) tex1=0x%llx\n",
                             cr.first, cr.last, tr.first, tr.last,
                             static_cast<unsigned>(e0.ctx.tex0.tbw),
                             static_cast<unsigned>(e0.ctx.tex0.tw),
                             static_cast<unsigned>(e0.ctx.tex0.th), e0.prim.fst ? 1u : 0u,
                             static_cast<unsigned>(e0.v[0].u), static_cast<unsigned>(e0.v[0].v),
                             static_cast<unsigned>(e0.v[1].u), static_cast<unsigned>(e0.v[1].v),
                             static_cast<unsigned long long>(e0.ctx.tex1));
                std::fprintf(stderr,
                             "[G765:carrypub] carry(fbp=0x%x fbw=%u rows=%d..%d) otherLive=%u "
                             "batch(fbp=0x%x fbw=%u n=%zu tme=%u tbp=0x%x psm=0x%x zbp=0x%x)\n",
                             c.fbp, c.fbw, c.rowLo, c.rowHi, otherLive ? 1u : 0u,
                             e0.ctx.frame.fbp, e0.ctx.frame.fbw, g_g144List.size(),
                             e0.prim.tme ? 1u : 0u, e0.ctx.tex0.tbp0, e0.ctx.tex0.psm,
                             static_cast<uint32_t>(e0.ctx.zbuf & 0x1FFu));
            }
        }
        if (publish)
        {
            g765PublishCarry(slot, 0, g_g765PubGpu);
            continue;
        }
        if (!rendersInto)
            continue;
        if (firstOverwrites)
        {
            // WRITE-AFTER-WRITE: the batch's first write to this surface replaces every carried row
            // before anything can read them (every read of the carried pages by this batch was
            // tested above and would have published). The carried bytes are dead: drop the record.
            c = G276PendDisplay{};
            g_g765Discarded.fetch_add(1u, std::memory_order_relaxed);
            continue;
        }
        // Reclaim: the batch continues the carried surface in its persistent FBO. Its rows join
        // the live record, exactly the state an intra-frame same-target continuation holds. When a
        // DIFFERENT display target is live, the two trade places (the live one is carried).
        const G276PendDisplay reclaimed = c;
        c = G276PendDisplay{};
        if (s_g276Pend.active &&
            (s_g276Pend.fbp != reclaimed.fbp || s_g276Pend.fbw != reclaimed.fbw ||
             s_g276Pend.fbW != reclaimed.fbW))
        {
            if (!g765CarryLive(0))
                g276FlushPendingDisplayCore(0);
            g_g765Swapped.fetch_add(1u, std::memory_order_relaxed);
        }
        if (!s_g276Pend.active)
        {
            s_g276Pend = reclaimed;
            g_g765Reclaimed.fetch_add(1u, std::memory_order_relaxed);
        }
        else
        {
            s_g276Pend.rowLo = std::min(s_g276Pend.rowLo, reclaimed.rowLo);
            s_g276Pend.rowHi = std::max(s_g276Pend.rowHi, reclaimed.rowHi);
            g_g765Merged.fetch_add(1u, std::memory_order_relaxed);
        }
    }
}

// ORACLE: poison a carried record's guest-VRAM rows. A reader that skips every edge now sees magenta
// (bytes), and the generation bump forces any cached texture decode of these pages to re-read them.
static void g765PoisonCarried(const G276PendDisplay &p)
{
    uint8_t *vram = g_g144LastVram;
    const uint32_t vramSize = g_g261VramSize;
    const int rows = p.rowHi - p.rowLo + 1;
    if (vram == nullptr || vramSize == 0u || p.fbW <= 0 || rows <= 0)
        return;
    static std::vector<uint32_t> s_px; // flush-owner thread only
    s_px.assign(static_cast<size_t>(p.fbW) * rows, 0xFFFF00FFu);
    g498PublishOwnedRows(0, vram, vramSize, s_px, p.fbBlock, p.fbw, p.fbW, p.rowHi + 1,
                         p.rowLo, p.rowHi, 0u, 0xFFFFFFFFu);
    g178BumpRectImpl(p.fbBlock, p.fbw, GS_PSM_CT32, 0u, static_cast<uint32_t>(p.rowLo),
                     static_cast<uint32_t>(p.fbW), static_cast<uint32_t>(rows), 2u | (9u << 8u));
    g_g765Poisoned.fetch_add(1u, std::memory_order_relaxed);
}

// Move the live record into a carry slot (merging with a same-surface carry, evicting the oldest
// carry by publication when both slots are taken). The live record's FBO keeps the newer bytes, so
// a carried record is exactly as valid as a live one; only its publication point moves.
static bool g765CarryLive(int evictCause)
{
    if (!s_g276Pend.active || s_g276Pend.fbW <= 0 || s_g276Pend.rowHi < s_g276Pend.rowLo)
        return false;
    int slot = -1;
    for (int i = 0; i < kG765CarrySlots && slot < 0; ++i)
        if (s_g765Carry[i].active && s_g765Carry[i].fbp == s_g276Pend.fbp &&
            s_g765Carry[i].fbw == s_g276Pend.fbw && s_g765Carry[i].fbW == s_g276Pend.fbW)
            slot = i;
    if (slot >= 0)
    {
        // Defensive: a same-surface carry should have been reclaimed by the batch that rendered.
        s_g276Pend.rowLo = std::min(s_g276Pend.rowLo, s_g765Carry[slot].rowLo);
        s_g276Pend.rowHi = std::max(s_g276Pend.rowHi, s_g765Carry[slot].rowHi);
        g_g765Merged.fetch_add(1u, std::memory_order_relaxed);
    }
    for (int i = 0; i < kG765CarrySlots && slot < 0; ++i)
        if (!s_g765Carry[i].active)
            slot = i;
    if (slot < 0)
    {
        int oldest = 0;
        for (int i = 1; i < kG765CarrySlots; ++i)
            if (s_g765CarryAge[i] < s_g765CarryAge[oldest])
                oldest = i;
        g765PublishCarry(oldest, evictCause, g_g765PubEvict);
        slot = oldest;
    }
    s_g765Carry[slot] = s_g276Pend;
    s_g765CarryAge[slot] = ++s_g765CarryClock;
    s_g276Pend.active = false;
    s_g276Pend.rowLo = 1 << 30;
    s_g276Pend.rowHi = -1;
    g_g765Carried.fetch_add(1u, std::memory_order_relaxed);
    if (g765CarryPoisonOn())
        g765PoisonCarried(s_g765Carry[slot]);
    return true;
}

// The frame-boundary site (cause 5). Returns having either carried or published the live record.
// ⭐ G768: a boundary with NOTHING LIVE to carry used to fall through to the unconditional flush,
// which publishes every CARRIED record too — `[G768:carrycause] c5` = 245 of s05's 257 whole-carry
// publications / 60 s, each a synchronous display readback. A carried record's publication point is
// its next real consumer (every reader has an edge, see the header); an empty boundary is not one.
// Rollback DC2_G768_NO_CARRYKEEP=1.
static bool g768CarryKeepOn()
{
    static const bool on = !envFlagEnabled("DC2_G768_NO_CARRYKEEP");
    return on;
}
static void g765CarryOrPublishAtBoundary()
{
    if (!g765CarryOn())
        g276FlushPendingDisplay(5);
    else if (!g765CarryLive(5))
    {
        if (g768CarryKeepOn())
            g276FlushPendingDisplayCore(5); // a degenerate live record only; carries stay carried
        else
            g276FlushPendingDisplay(5);
    }
    g765Report();
}

// A coalescing display batch for a DIFFERENT surface than the live record (G276's "next different
// target" edge). The live record is carried, not read back: a different display target is not a
// reader of the old one's pages (anything that is, is an edge of its own).
static void g765SwitchDisplayTarget(int cause)
{
    if (!g765CarryOn() || !g765CarryLive(cause))
        g276FlushPendingDisplayCore(cause);
}
