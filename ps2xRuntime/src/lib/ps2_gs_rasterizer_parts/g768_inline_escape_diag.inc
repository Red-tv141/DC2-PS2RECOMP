// ===== G768 (diagnostic, DC2_G768_INLESC=1): WHY does an inline CPU draw escape a dirty resident? =====
//
// G767 §6 and the g767o s05 census: every lever that removes a publication which used to precede an
// inline CPU draw (the paletted page view's 0x13c publication on dungeon1, the mode-5/6 widening's
// 0x147 CPU replays on s05) turns the same inline draw into `[G261:invariant] escaped writer 0x139`
// (bump source 6 = inline-draw bbox colour bump, 8 pages of 0x139's wide window each time). The
// inline prologue (g261PrepareInline) is supposed to publish every dirty resident whose claimed
// window the primitive's frame / Z / texture / CLUT pages touch. This tracer records what that
// prologue saw and decided, and after the draw names every resident whose window the bump landed in.
// Behaviour-pure; one cached bool per inline primitive when off.
static bool g768InlEscOn()
{
    static const bool on = envFlagEnabled("DC2_G768_INLESC");
    return on;
}
struct G768InlTrace
{
    G144BlockRange color{};
    uint32_t dirtyMask = 0u; // residents dirty when the prologue ran
    uint32_t matMask = 0u;   // residents it published
    uint32_t skipMask = 0u;  // residents it excused (G766 read-only skip)
    uint64_t serial = 0u;
    bool ran = false;
    bool unknown = false;
};
static thread_local G768InlTrace t_g768Inl;
static uint64_t g_g768InlSerial = 0u;

static void g768InlEscAfterDraw(const GSContext &ctx, const GSPrimReg &prim, int bx0, int by0,
                                int bx1, int by1)
{
    if (!g768InlEscOn() || bx1 < bx0 || by1 < by0)
        return;
    const G144BlockRange w = g144RangeForRect(
        framePageBaseToBlock(ctx.frame.fbp), ctx.frame.psm, ctx.frame.fbw,
        static_cast<uint32_t>(std::max(bx0, 0)), static_cast<uint32_t>(std::max(by0, 0)),
        static_cast<uint32_t>(bx1 - std::max(bx0, 0) + 1),
        static_cast<uint32_t>(by1 - std::max(by0, 0) + 1));
    static uint64_t s_n = 0u;
    for (uint32_t t = 0u; t < kG248TargetCount; ++t)
    {
        const G261Res &r = g_g261Res[t];
        if (!r.range.valid || !w.valid || !g144RangeOverlaps(w, r.range))
            continue;
        ++s_n;
        if (s_n > 96u && (s_n % 512u) != 0u)
            continue;
        std::fprintf(stderr,
                     "[G768:inlesc] n=%llu slot=%u fbp=0x%03x dirty=%d rfbw=%u win=%d..%d "
                     "drows=%d..%d cov=%d..%d range=%x..%x anchor=%llu | frame=%03x/%u/%x "
                     "sc=%d,%d..%d,%d box=%d,%d..%d,%d bump=%x..%x prim=%u tme=%u | pro(ran=%d "
                     "ser=%llu/%llu unk=%d color=%x..%x dirty=%x mat=%x skip=%x)\n",
                     (unsigned long long)s_n, t, kG261Fbp[t], r.dirty ? 1 : 0, r.fbw, r.winLo,
                     r.winHi, r.dirtyLo, r.dirtyHi, r.covLo, r.covHi, r.range.first, r.range.last,
                     (unsigned long long)r.g768AnchorSeq, ctx.frame.fbp,
                     static_cast<unsigned>(ctx.frame.fbw), ctx.frame.psm, ctx.scissor.x0,
                     ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1, bx0, by0, bx1, by1, w.first,
                     w.last, static_cast<unsigned>(prim.type), prim.tme ? 1u : 0u,
                     t_g768Inl.ran ? 1 : 0, (unsigned long long)t_g768Inl.serial,
                     (unsigned long long)g_g768InlSerial, t_g768Inl.unknown ? 1 : 0,
                     t_g768Inl.color.valid ? t_g768Inl.color.first : 0u,
                     t_g768Inl.color.valid ? t_g768Inl.color.last : 0u, t_g768Inl.dirtyMask,
                     t_g768Inl.matMask, t_g768Inl.skipMask);
    }
    t_g768Inl.ran = false;
}

// At the escaped-writer invariant: the residency record's geometry and its last anchor.
static void g768InlEscAtInvariant(int ti, const G261Res &r, uint32_t fbp, uint32_t batchFbw,
                                  int rowLo, int rowHi)
{
    if (!g768InlEscOn())
        return;
    static uint64_t s_n = 0u;
    if (++s_n > 48u && (s_n % 256u) != 0u)
        return;
    std::fprintf(stderr,
                 "[G768:inv] n=%llu slot=%d fbp=0x%03x rfbw=%u batchFbw=%u rows=%d..%d win=%d..%d "
                 "drows=%d..%d cov=%d..%d range=%x..%x anchor=%llu anchorNow=%llu inl=%llu\n",
                 (unsigned long long)s_n, ti, fbp, r.fbw, batchFbw, rowLo, rowHi, r.winLo, r.winHi,
                 r.dirtyLo, r.dirtyHi, r.covLo, r.covHi, r.range.first, r.range.last,
                 (unsigned long long)r.g768AnchorSeq, (unsigned long long)g_g768AnchorSeq,
                 (unsigned long long)g_g768InlSerial);
}
