static bool g34_fix_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_G34_FIX");
    return e;
}

static bool g40_joints_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_G40_JOINTS");
    return e;
}

static std::atomic<uint32_t> g_g34_char{0};
static std::atomic<uint32_t> g_g34_outline{0};

// Wrap DrawDirect__11CCharacter2@0x1731f0. a0 = CCharacter2 this; *(this+0x100)
// is the float outline level read at the function head (recomp 0x173214).
static void g34_char_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const bool valid = (self > 0x80000u && self < 0x2000000u);
    uint32_t outlineBits = valid ? dc2_read_u32(rdram, self + 0x100u) : 0u;
    float outline; std::memcpy(&outline, &outlineBits, sizeof(outline));
    bool fixed = false;
    if (g34_fix_enabled() && valid && !(outline >= 1.0f))
    {
        outlineBits = 0x3F800000u; // 1.0f
        dc2_write_u32(rdram, self + 0x100u, outlineBits);
        outline = 1.0f;
        fixed = true;
    }
    const uint32_t n = g_g34_char.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 16u || (n % 240u) == 0u)
    {
        std::fprintf(stderr, "[G34:charDraw] n=%u this=0x%x outlineLvl(+0x100)=0x%x(%g) fixed=%d\n",
                     n, self, outlineBits, static_cast<double>(outline), fixed ? 1 : 0);
        std::fflush(stderr);
    }
    DrawDirect__11CCharacter2Fv_0x1731f0(rdram, ctx, runtime);

    // G40: after the draw (which runs the deform), dump the skeleton joint LW matrices
    // (joint+0x70) + dirty flag (joint+0x40). HW A/B (PCSX2 @ costume) shows a fully
    // posed skeleton (root frame 0xee7410, joints 0xee7410..0xee8840, real rotation+
    // translation). G39 proved the runner's bone palette is ZERO. If these joints read
    // zero -> (A) skeleton unposed; if nonzero -> (B) deform/COP2 math zeroes the palette.
    if (g40_joints_enabled() && valid && (n <= 4u))
    {
        const uint32_t rootFrame = dc2_read_u32(rdram, self + 0x70u); // this[0x1c]
        std::fprintf(stderr, "[G40:joints] n=%u char=0x%x rootFrame(+0x70)=0x%x\n",
                     n, self, rootFrame);
        static const uint32_t hwJoints[] = {
            0xee7410u, 0xee82f0u, 0xee8400u, 0xee8510u, 0xee8620u, 0xee8730u, 0xee8840u};
        for (uint32_t j = 0; j < (sizeof(hwJoints) / sizeof(hwJoints[0])); ++j)
        {
            const uint32_t jp = hwJoints[j];
            const uint32_t dirty = dc2_read_u32(rdram, jp + 0x40u);
            const uint32_t force = dc2_read_u32(rdram, jp + 0xfcu);
            const uint32_t par = dc2_read_u32(rdram, jp + 0x54u);
            float m[16];
            for (int k = 0; k < 16; ++k)
            {
                uint32_t b = dc2_read_u32(rdram, jp + 0x70u + (uint32_t)(k * 4));
                std::memcpy(&m[k], &b, sizeof(float));
            }
            std::fprintf(stderr,
                "[G40:joint] j=%u addr=0x%x dirty=%u force=%u par=0x%x LW=[%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n",
                j, jp, dirty, force, par,
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
        // HW skinned MotionMDT visuals (PCSX2 A/B): 0xf48ce0, 0xfdebc0. Dump their
        // bone-binding inputs (+0x50 frameArr -> bone-frame ptrs, +0x54 count, +0x58
        // baseMtx, +0x80 bone-index list). HW: +0x50=0xeef7e0 -> [0xee7410,0xee7520..],
        // idx=[0x12,0x15,0x18,..]. If runner matches -> palette-build exec bug; else binding gap.
        static const uint32_t hwVisuals[] = {0xf48ce0u, 0xfdebc0u};
        for (uint32_t v = 0; v < 2; ++v)
        {
            const uint32_t vis = hwVisuals[v];
            const uint32_t f50 = dc2_read_u32(rdram, vis + 0x50u);
            const uint32_t f54 = dc2_read_u32(rdram, vis + 0x54u);
            const uint32_t f58 = dc2_read_u32(rdram, vis + 0x58u);
            std::fprintf(stderr, "[G40:visual] vis=0x%x +0x50(frameArr)=0x%x +0x54=0x%x +0x58(baseMtx)=0x%x idx[0..7]=",
                         vis, f50, f54, f58);
            for (int i = 0; i < 8; ++i)
                std::fprintf(stderr, "0x%x ", dc2_read_u32(rdram, vis + 0x80u + (uint32_t)(i * 4)));
            std::fprintf(stderr, "\n");
            if (f50 > 0x80000u && f50 < 0x2000000u)
            {
                std::fprintf(stderr, "[G40:framearr] vis=0x%x ptrs[0..5]=", vis);
                for (int i = 0; i < 6; ++i)
                    std::fprintf(stderr, "0x%x ", dc2_read_u32(rdram, f50 + (uint32_t)(i * 4)));
                std::fprintf(stderr, "\n");
            }
        }
        std::fflush(stderr);
    }
}

// =====================================================================
// PHASE G204: character LOD / part-count trace. DrawDirect__11CCharacter2
// (0x1731f0) picks an LOD index from camera distance (threshold array at
// this+0x350, count this+0x34c), calls ChangeLOD which stores the chosen
// index at this+0x354 and the selected LOD data at this+0x358, then walks
// the mesh frame list drawing each part. On MAP-4 the costume-menu Max (LOD 0,
// close camera) renders the cap correctly but the town Max (farther camera)
// drops the cap. This probe logs LOD count + chosen index (before AND after
// the draw) + the dynamic-anime sub count (this+0x12c) so we can tell whether
// town selects a higher, cap-less LOD than the costume route. Gated behind
// DC2_G204_LOD_TRACE=1; registered on ANY route so town is observable.
// =====================================================================
static bool g204_lod_trace_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_G204_LOD_TRACE");
    return e;
}
static std::atomic<uint32_t> g_g204_char{0};
static void g204_lod_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const bool valid = (self > 0x80000u && self < 0x2000000u);
    const uint32_t lodCount = valid ? dc2_read_u32(rdram, self + 0x34cu) : 0u;
    const uint32_t lodBefore = valid ? dc2_read_u32(rdram, self + 0x354u) : 0u;
    const uint32_t dynCount = valid ? dc2_read_u32(rdram, self + 0x12cu) : 0u;
    const uint32_t outBits = valid ? dc2_read_u32(rdram, self + 0x100u) : 0u;
    float outline; std::memcpy(&outline, &outBits, sizeof(outline));
    // Object frame translation proxy (row2 of the this+0x10 transform block).
    auto rf = [&](uint32_t off) { uint32_t b = valid ? dc2_read_u32(rdram, self + off) : 0u; float f; std::memcpy(&f, &b, sizeof(f)); return f; };
    const float px = rf(0x30u), py = rf(0x34u), pz = rf(0x38u);
    DrawDirect__11CCharacter2Fv_0x1731f0(rdram, ctx, runtime);
    const uint32_t retCount = getRegU32(ctx, 2); // v0 = accumulated draw count
    const uint32_t lodAfter = valid ? dc2_read_u32(rdram, self + 0x354u) : 0u;
    const uint32_t lodData = valid ? dc2_read_u32(rdram, self + 0x358u) : 0u;
    const uint32_t n = g_g204_char.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 24u || (n % 240u) == 0u)
    {
        std::fprintf(stderr,
            "[G204:lod] n=%u this=0x%x ret=%u out(+0x100)=%g pos=(%.1f,%.1f,%.1f) lodCount=%d lodIdx %d->%d dynSub=%d\n",
            n, self, retCount, (double)outline, (double)px, (double)py, (double)pz,
            (int)lodCount, (int)lodBefore, (int)lodAfter, (int)dynCount);
        std::fflush(stderr);
    }
}

// Wrap Draw__12COutLineDraw::Draw@0x17c2d0. a0 = COutLineDraw this; param_1=f12,
// param_2=f13 (== fVar13, the composite gate value). +0x64 must be 0, +0x38 is
// outlineW (>0 selects the RTT-outline branch), +0x30/+0x34 are the frames.
static void g34_outline_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const bool valid = (self > 0x80000u && self < 0x2000000u);
    const float p1 = ctx->f[12], p2 = ctx->f[13];
    const uint32_t v30 = valid ? dc2_read_u32(rdram, self + 0x30u) : 0u;
    const uint32_t v34 = valid ? dc2_read_u32(rdram, self + 0x34u) : 0u;
    uint32_t v38b = valid ? dc2_read_u32(rdram, self + 0x38u) : 0u;
    float v38; std::memcpy(&v38, &v38b, sizeof(v38));
    const uint32_t v60 = valid ? dc2_read_u32(rdram, self + 0x60u) : 0u;
    const uint32_t v64 = valid ? dc2_read_u32(rdram, self + 0x64u) : 0u;
    const uint32_t n = g_g34_outline.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 56u || (n % 240u) == 0u)
    {
        std::fprintf(stderr, "[G34:outline] n=%u this=0x%x p1=%g p2(fVar13)=%g p2ge1=%d +0x30=0x%x +0x34=0x%x +0x38(outlineW)=%g +0x60=0x%x +0x64=0x%x\n",
                     n, self, static_cast<double>(p1), static_cast<double>(p2),
                     (p2 >= 1.0f) ? 1 : 0, v30, v34, static_cast<double>(v38), v60, v64);
        std::fflush(stderr);
    }
    // G37: scope the mgGetDrawRect bbox repair to this preview-composite path only.
    g_g37_in_outline.fetch_add(1, std::memory_order_relaxed);
    Draw__12COutLineDrawFff_0x17c2d0(rdram, ctx, runtime);
    g_g37_in_outline.fetch_sub(1, std::memory_order_relaxed);
}

// PHASE G37: repair the collapsed costume-model bounding box at its SOURCE.
// On the runner the VU0 AABB->screen projection in GetDrawRect collapses every corner to
// the screen-center point, so Draw__12COutLineDraw computes a center-point rect for BOTH the
// black-fill clear of the RTT and the RTT->display composite. The collapsed clear leaves the
// rest of the 0x2720 work page holding the time-shared font/menu atlas, which the composite
// then samples as CT32 garbage (G37 RTT dump confirmed this). Rewriting the box to the
// PCSX2-verified preview extents fixes the clear and the composite together; the G36
// composite-rect patch then becomes a no-op backstop. mgVu0FBOX layout: +0x00 max.xyzw,
// +0x10 min.xyzw (GetDrawRect writes sceVu0CopyVector(box,max) + (box+4,min)).
static bool g206_bbox_trace_enabled();
static bool g212_lwguard_disabled(); // G212: preempt-suppress guard kill switch (defined below)
static std::atomic<uint32_t> g_g37_boxfix{0};
static std::atomic<uint32_t> g_g206_bbox_costume{0};
static void g37_getdrawrect_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t frame = getRegU32(ctx, 4); // a0 = mgCFrame* (G206 identity trace)
    const uint32_t box = getRegU32(ctx, 5); // a1 = mgVu0FBOX* output
    // G212: this probe replaces the default g212_getdrawrect_guard in the function table,
    // so it must reproduce the guard's preempt suppression (a yield escaping through this
    // probe's direct C++ call is the exact G211 swallow; the probe would also read/REWRITE
    // the box before the body logically finished).
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    mgGetDrawRect__FP8mgCFrameP9mgVu0FBOX_0x143160(rdram, ctx, runtime);
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    const uint32_t retVal = getRegU32(ctx, 2);
    // G206: read-only bbox trace on the COSTUME route too (same window/identity scheme
    // as g206_getdrawrect_probe, but this route already has the box-fix wrapper on
    // 0x143160 so we log inline here instead of double-registering).
    if (g206_bbox_trace_enabled() && g_g37_in_outline.load(std::memory_order_relaxed) > 0)
    {
        float mx = 0, my = 0, nx = 0, ny = 0;
        if (box > 0x80000u && box < 0x2000000u)
        {
            auto rf = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, box + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
            mx = rf(0x00u); my = rf(0x04u); nx = rf(0x10u); ny = rf(0x14u);
        }
        const uint32_t n = g_g206_bbox_costume.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 56u || (n % 240u) == 0u)
        {
            std::fprintf(stderr,
                         "[G206:bboxCostume] n=%u frame=0x%x ret=%u box=(x:%.1f..%.1f y:%.1f..%.1f)\n",
                         n, frame, retVal, static_cast<double>(nx), static_cast<double>(mx),
                         static_cast<double>(ny), static_cast<double>(my));
            std::fflush(stderr);
        }
    }
    if (g_g37_in_outline.load(std::memory_order_relaxed) <= 0 ||
        retVal == 0u ||                            // ret==0: not visible, COutLineDraw bails anyway
        box <= 0x80000u || box >= 0x2000000u)
        return;

    auto readF = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, box + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
    const float maxX = readF(0x00u), maxY = readF(0x04u);
    const float minX = readF(0x10u), minY = readF(0x14u);
    // Collapsed iff the projected box has ~zero screen area.
    const float dx = (maxX > minX) ? (maxX - minX) : (minX - maxX);
    const float dy = (maxY > minY) ? (maxY - minY) : (minY - maxY);
    if (dx >= 2.0f || dy >= 2.0f)
        return;

    auto writeF = [&](uint32_t off, float f) { uint32_t u; std::memcpy(&u, &f, sizeof(u)); dc2_write_u32(rdram, box + off, u); };
    writeF(0x00u, 512.0f);     // max.x -> rect x1 0x2000
    writeF(0x04u, 380.875f);   // max.y -> rect y1 0x17ce
    writeF(0x10u, 223.9375f);  // min.x -> rect x0 0x0dff
    writeF(0x14u, 0.0f);       // min.y -> rect y0 0x0000

    const uint32_t n = g_g37_boxfix.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 8u || (n % 240u) == 0u)
    {
        std::fprintf(stderr,
                     "[G37:boxfix] n=%u box=0x%x oldmax=(%.2f,%.2f) oldmin=(%.2f,%.2f) -> max=(512,380.875) min=(223.9375,0)\n",
                     n, box, static_cast<double>(maxX), static_cast<double>(maxY),
                     static_cast<double>(minX), static_cast<double>(minY));
        std::fflush(stderr);
    }
}

// PHASE G206: read-only screen-bbox probe for mgGetDrawRect, scoped to the same
// Draw__12COutLineDraw call window as g34/g37 (g_g37_in_outline) but WITHOUT the G37
// costume-preview box rewrite -- town/field parts must not be stomped with the costume
// RTT's hardcoded extents. Logs a0 (mgCFrame*, expected == a part's self+0x34 captured
// by g34_outline_draw_probe) + the projected screen rect, to identify which of the 7
// outline parts is the cap by screen position (see phase-G205-fix-log.md "Next").
static bool g206_bbox_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G206_BBOX_TRACE");
    return enabled;
}
static std::atomic<uint32_t> g_g206_bbox{0};
// PHASE G207: dense per-part visibility tabulation, layered on the G206 probe. A first
// attempt keyed the tally by POSITIONAL index ((n-1)%8) assuming a fixed 8-call/frame
// cycle starting at n=1 -- WRONG: the very first sampled call (n=1) is a one-off warm-up
// outlier (different frame identity than the steady-state cycle), which phase-shifts every
// later positional bucket by one and silently corrupts the whole tally (see fix-log
// "Finding 0"). Key by the mgCFrame* IDENTITY itself instead (small fixed-size linear-probe
// table; ~8 distinct parts expected) so the tally is immune to any positional drift.
struct G207Slot {
    std::atomic<uint32_t> addr{0}; std::atomic<uint32_t> total{0}; std::atomic<uint32_t> visible{0};
    // PHASE G209: latest GetLWMatrix world-matrix snapshot for this identity (translation
    // row + basis-X magnitude as a cheap degenerate-scale check), for skin-matrix hypothesis testing.
    std::atomic<uint32_t> lwCalls{0};
    std::atomic<uint32_t> tx{0}, ty{0}, tz{0}, scale0{0}; // float bits
};
static G207Slot g_g207_slots[16];
// Find (or create) this identity's slot in the shared G207 table. Used by both the
// GetDrawRect visibility probe and the G209 GetLWMatrix probe so results correlate.
static uint32_t g207_slot_for(uint32_t frame)
{
    uint32_t slotIdx = 16;
    for (uint32_t i = 0; i < 16; ++i)
    {
        uint32_t cur = g_g207_slots[i].addr.load(std::memory_order_relaxed);
        if (cur == frame) { slotIdx = i; break; }
        if (cur == 0u)
        {
            uint32_t expected = 0u;
            if (g_g207_slots[i].addr.compare_exchange_strong(expected, frame, std::memory_order_relaxed) ||
                expected == frame)
            {
                slotIdx = i;
                break;
            }
        }
    }
    return slotIdx;
}
static void g206_getdrawrect_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t frame = getRegU32(ctx, 4); // a0 = mgCFrame*
    const uint32_t box = getRegU32(ctx, 5);   // a1 = mgVu0FBOX* output
    // G212: reproduce the default guard's preempt suppression (see g37_getdrawrect_probe).
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    mgGetDrawRect__FP8mgCFrameP9mgVu0FBOX_0x143160(rdram, ctx, runtime);
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    const uint32_t ret = getRegU32(ctx, 2);
    if (g_g37_in_outline.load(std::memory_order_relaxed) <= 0 || box <= 0x80000u || box >= 0x2000000u)
        return;
    auto readF = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, box + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
    const float maxX = readF(0x00u), maxY = readF(0x04u);
    const float minX = readF(0x10u), minY = readF(0x14u);
    const uint32_t n = g_g206_bbox.fetch_add(1u, std::memory_order_relaxed) + 1u;
    uint32_t slotIdx = g207_slot_for(frame);
    if (slotIdx < 16)
    {
        g_g207_slots[slotIdx].total.fetch_add(1u, std::memory_order_relaxed);
        if (ret != 0u)
            g_g207_slots[slotIdx].visible.fetch_add(1u, std::memory_order_relaxed);
    }
    if (n <= 56u || (n % 240u) == 0u)
    {
        float tx = 0, ty = 0, tz = 0, s0 = 0; uint32_t lw = 0;
        if (slotIdx < 16)
        {
            uint32_t b;
            b = g_g207_slots[slotIdx].tx.load(std::memory_order_relaxed); std::memcpy(&tx, &b, sizeof(tx));
            b = g_g207_slots[slotIdx].ty.load(std::memory_order_relaxed); std::memcpy(&ty, &b, sizeof(ty));
            b = g_g207_slots[slotIdx].tz.load(std::memory_order_relaxed); std::memcpy(&tz, &b, sizeof(tz));
            b = g_g207_slots[slotIdx].scale0.load(std::memory_order_relaxed); std::memcpy(&s0, &b, sizeof(s0));
            lw = g_g207_slots[slotIdx].lwCalls.load(std::memory_order_relaxed);
        }
        std::fprintf(stderr,
                     "[G206:bbox] n=%u frame=0x%x ret=%u box=(x:%.1f..%.1f y:%.1f..%.1f) lwPos=(%.2f,%.2f,%.2f) lwScale0=%.3f lwCalls=%u\n",
                     n, frame, ret, static_cast<double>(minX), static_cast<double>(maxX),
                     static_cast<double>(minY), static_cast<double>(maxY),
                     static_cast<double>(tx), static_cast<double>(ty), static_cast<double>(tz),
                     static_cast<double>(s0), lw);
        std::fflush(stderr);
    }
    if ((n % 800u) == 0u)
    {
        std::fprintf(stderr, "[G207:stat] n=%u", n);
        for (uint32_t i = 0; i < 16; ++i)
        {
            const uint32_t a = g_g207_slots[i].addr.load(std::memory_order_relaxed);
            if (a == 0u)
                continue;
            const uint32_t tot = g_g207_slots[i].total.load(std::memory_order_relaxed);
            const uint32_t vis = g_g207_slots[i].visible.load(std::memory_order_relaxed);
            std::fprintf(stderr, " f=0x%x:%u/%u", a, vis, tot);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
}

// PHASE G209: test G207/G208's skin-matrix hypothesis for the MAP-4 cap-mesh flicker
// (identity 0x8638b0 this session) directly. Wraps GetLWMatrix__8mgCFrame -- the world
// (local-to-world joint chain) matrix builder GetDrawRect's AABB projection consumes --
// and, scoped to the same outline-draw window as G206/G207, stashes the latest translation
// (row3 xyz) + basis-X magnitude (cheap degenerate-scale check) into the SAME identity-keyed
// g_g207_slots table, so g206_getdrawrect_probe's [G206:bbox] line can print the matrix that
// produced each ret=0/ret=1 sample for direct correlation (no separate log to cross-reference).
// A wildly different/garbage lwPos or lwScale0 on a ret=0 sample vs a neighboring ret=1 sample
// for the SAME identity confirms the hypothesis; a near-identical matrix refutes it.
static std::atomic<uint32_t> g_g209_lw{0};
// PHASE G210: extend G209's probe to capture PRE-call producer state -- G209 proved
// GetLWMatrix returns an exact-zero world matrix on bad frames but left open WHICH branch
// produced it (decompiled 0x137030: cache branch = plain sceVu0CopyMatrix of this+0x70 when
// this+0x40==0 AND the whole this+0x54 parent-dirty chain is clean; recompute branch =
// ClearChildFlag + GetLocalMatrix + optional parent recurse + multiply, always ends with
// this+0x40=0). Reading this+0x40 (dirty)/this+0x54 (parent)/this+0x70 (cached matrix, same
// 4x4 layout as the output) and the parent's own +0x40 BEFORE the call distinguishes: (a)
// dirtyPre==0 + cache already zero => something zeroed the this+0x70 cache slot directly
// without marking dirty (the field-level producer bug G209 named); (b) dirtyPre==1 (recompute
// ran) but output still zero => GetLocalMatrix or the parent's own GetLWMatrix fed it zero
// upstream instead.
static void g209_lwmatrix_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4); // a0 = mgCFrame*
    const uint32_t outp = getRegU32(ctx, 5); // a1 = undefined1 (*)[16][4] world matrix out

    const bool inWindowPre = g_g37_in_outline.load(std::memory_order_relaxed) > 0;
    uint32_t fcFlagPre = 0, dirtyPre = 0, parentPre = 0, parentDirtyPre = 0, parentFcPre = 0;
    float cacheR0xPre = 0.0f, cacheTxPre = 0.0f, cacheTyPre = 0.0f, cacheTzPre = 0.0f;
    float parentCacheR0xPre = 0.0f, parentCacheTxPre = 0.0f, parentCacheTyPre = 0.0f, parentCacheTzPre = 0.0f;
    // G210: local's own scale vector (self+0x30) -- GetLocalMatrix scales the bind matrix
    // rows 0-2 by this vector; an all-zero scale collapses local's basis to zero even with
    // a perfectly healthy parent, which would also present as a fully-zero world matrix.
    float scaleVecXPre = 0.0f, scaleVecYPre = 0.0f, scaleVecZPre = 0.0f;
    // G211: the rest of GetLocalMatrix's inputs G210 left unread -- translate (self+0x10),
    // Euler angles (self+0x20/0x24/0x28), and the two mode/branch-select flags (self+0x44
    // local-dirty, self+0x100 rotation/translate-mode bitmask) that gate which of
    // GetLocalMatrix's optional blocks execute.
    uint32_t localFlag44Pre = 0, modeFlag100Pre = 0;
    float txInPre = 0.0f, tyInPre = 0.0f, tzInPre = 0.0f;
    float rotXPre = 0.0f, rotYPre = 0.0f, rotZPre = 0.0f;
    float bindR0xPre = 0.0f, bindR0yPre = 0.0f, bindR0zPre = 0.0f;
    float bindTxPre = 0.0f, bindTyPre = 0.0f, bindTzPre = 0.0f;
    const bool selfInRange = self > 0x80000u && self < 0x2000000u;
    if (inWindowPre && selfInRange)
    {
        fcFlagPre = dc2_read_u32(rdram, self + 0xfcu);
        dirtyPre = dc2_read_u32(rdram, self + 0x40u);
        parentPre = dc2_read_u32(rdram, self + 0x54u);
        if (parentPre > 0x80000u && parentPre < 0x2000000u)
        {
            parentDirtyPre = dc2_read_u32(rdram, parentPre + 0x40u);
            parentFcPre = dc2_read_u32(rdram, parentPre + 0xfcu);
            auto rfp = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, parentPre + 0x70u + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
            parentCacheR0xPre = rfp(0x00u);
            parentCacheTxPre = rfp(0x30u); parentCacheTyPre = rfp(0x34u); parentCacheTzPre = rfp(0x38u);
        }
        auto rfc = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, self + 0x70u + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
        cacheR0xPre = rfc(0x00u);
        cacheTxPre = rfc(0x30u); cacheTyPre = rfc(0x34u); cacheTzPre = rfc(0x38u);
        auto rfs = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, self + 0x30u + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
        scaleVecXPre = rfs(0x00u); scaleVecYPre = rfs(0x04u); scaleVecZPre = rfs(0x08u);
        localFlag44Pre = dc2_read_u32(rdram, self + 0x44u);
        modeFlag100Pre = dc2_read_u32(rdram, self + 0x100u);
        auto rft = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, self + 0x10u + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
        txInPre = rft(0x00u); tyInPre = rft(0x04u); tzInPre = rft(0x08u);
        auto rfr = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, self + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
        rotXPre = rfr(0x20u); rotYPre = rfr(0x24u); rotZPre = rfr(0x28u);
        // G212: flag44==0 takes GetLocalMatrix's TRIVIAL branch (sceVu0CopyMatrix straight
        // from self+0xb0) -- scale/translate/rot above are dead code on that path. self+0xb0
        // (the bind local matrix) is the actual local input in play; read it directly.
        auto rfb = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, self + 0xb0u + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
        bindR0xPre = rfb(0x00u); bindR0yPre = rfb(0x04u); bindR0zPre = rfb(0x08u);
        bindTxPre = rfb(0x30u); bindTyPre = rfb(0x34u); bindTzPre = rfb(0x38u);
    }

    // G212: reproduce the default guard's preempt suppression (this probe replaces
    // g212_lwmatrix_guard in the function table when the trace is enabled).
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    GetLWMatrix__8mgCFrameFPA4_f_0x137030(rdram, ctx, runtime);
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    if (!inWindowPre || outp <= 0x80000u || outp >= 0x2000000u)
        return;
    auto rf = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, outp + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
    // row0 = basis-X (world scale/orientation proxy), row3 = translation.
    const float r0x = rf(0x00u), r0y = rf(0x04u), r0z = rf(0x08u);
    const float tx = rf(0x30u), ty = rf(0x34u), tz = rf(0x38u);
    const float scale0 = std::sqrt(r0x * r0x + r0y * r0y + r0z * r0z);
    const bool bad = !std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz) || !std::isfinite(scale0);
    const bool zeroMat = scale0 == 0.0f && tx == 0.0f && ty == 0.0f && tz == 0.0f;
    const uint32_t slotIdx = g207_slot_for(self);
    if (slotIdx < 16)
    {
        g_g207_slots[slotIdx].lwCalls.fetch_add(1u, std::memory_order_relaxed);
        uint32_t txb, tyb, tzb, s0b;
        std::memcpy(&txb, &tx, sizeof(txb)); std::memcpy(&tyb, &ty, sizeof(tyb));
        std::memcpy(&tzb, &tz, sizeof(tzb)); std::memcpy(&s0b, &scale0, sizeof(s0b));
        g_g207_slots[slotIdx].tx.store(txb, std::memory_order_relaxed);
        g_g207_slots[slotIdx].ty.store(tyb, std::memory_order_relaxed);
        g_g207_slots[slotIdx].tz.store(tzb, std::memory_order_relaxed);
        g_g207_slots[slotIdx].scale0.store(s0b, std::memory_order_relaxed);
    }
    const uint32_t n = g_g209_lw.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (bad || zeroMat || n <= 40u || (n % 800u) == 0u)
    {
        std::fprintf(stderr,
                     "[G209:lwmat] n=%u self=0x%x pos=(%.2f,%.2f,%.2f) scale0=%.3f%s"
                     " pre[fc=%u dirty=%u flag44=%u mode100=0x%x scaleVec=(%.3f,%.3f,%.3f)"
                     " translateIn=(%.3f,%.3f,%.3f) rotIn=(%.4f,%.4f,%.4f)"
                     " bindR0=(%.3f,%.3f,%.3f) bindPos=(%.2f,%.2f,%.2f)"
                     " parent=0x%x parentDirty=%u parentFc=%u"
                     " parentCacheR0x=%.3f parentCachePos=(%.2f,%.2f,%.2f) cacheR0x=%.3f cachePos=(%.2f,%.2f,%.2f)]\n",
                     n, self, static_cast<double>(tx), static_cast<double>(ty), static_cast<double>(tz),
                     static_cast<double>(scale0), bad ? " BAD" : (zeroMat ? " ZERO" : ""),
                     fcFlagPre, dirtyPre, localFlag44Pre, modeFlag100Pre,
                     static_cast<double>(scaleVecXPre), static_cast<double>(scaleVecYPre), static_cast<double>(scaleVecZPre),
                     static_cast<double>(txInPre), static_cast<double>(tyInPre), static_cast<double>(tzInPre),
                     static_cast<double>(rotXPre), static_cast<double>(rotYPre), static_cast<double>(rotZPre),
                     static_cast<double>(bindR0xPre), static_cast<double>(bindR0yPre), static_cast<double>(bindR0zPre),
                     static_cast<double>(bindTxPre), static_cast<double>(bindTyPre), static_cast<double>(bindTzPre),
                     parentPre, parentDirtyPre, parentFcPre,
                     static_cast<double>(parentCacheR0xPre), static_cast<double>(parentCacheTxPre),
                     static_cast<double>(parentCacheTyPre), static_cast<double>(parentCacheTzPre),
                     static_cast<double>(cacheR0xPre), static_cast<double>(cacheTxPre),
                     static_cast<double>(cacheTyPre), static_cast<double>(cacheTzPre));
        std::fflush(stderr);
    }
}

// PHASE G212 FIX (default-ON, kill DC2_G212_NO_LWGUARD=1): scoped back-edge preemption
// suppression for the joint-matrix / draw-rect computation window. G211 proved the MAP-4
// cap/head-mesh flicker root: shouldPreemptGuestExecution() can fire on a backward branch
// inside GetLWMatrix@0x137030 (its ancestor-dirty-chain walk), ClearChildFlag@0x136C80
// (sibling walk, called from GetLWMatrix's recompute), or GetDrawRect@0x1381C0 (under
// mgGetDrawRect@0x143160) -- the yield unwinds the whole recompiled call chain, and if any
// interposed override swallows the resume (calls another body before the dispatcher can
// re-drive the yielded one -- the G57/G186 bug class), the world matrix / screen rect is
// never written and the consumer reads zeros => the part is culled for that frame
// (GetDrawRect ret=0, screen box collapsed). Decisive A/B in phase-G211-fix-log.md:
// DC2_G57_NO_PREEMPT=1 eliminated 100% of the zero-matrix samples; these guards reproduce
// that atomicity ONLY for the two proven windows (a few bone-chain recursions + VU0 math,
// microseconds) instead of globally (global no-preempt has its own regressions, G211).
// Same fix pattern as G57 (title draw) and G186 (Draw__4CMap): depth ++/-- around the call.
static bool g212_lwguard_disabled()
{
    static const bool disabled = dc2_env_flag_enabled("DC2_G212_NO_LWGUARD");
    return disabled;
}
static void g212_lwmatrix_guard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g212_lwguard_disabled())
    {
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
        GetLWMatrix__8mgCFrameFPA4_f_0x137030(rdram, ctx, runtime);
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    GetLWMatrix__8mgCFrameFPA4_f_0x137030(rdram, ctx, runtime);
}
static void g212_getdrawrect_guard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!g212_lwguard_disabled())
    {
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
        mgGetDrawRect__FP8mgCFrameP9mgVu0FBOX_0x143160(rdram, ctx, runtime);
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    mgGetDrawRect__FP8mgCFrameP9mgVu0FBOX_0x143160(rdram, ctx, runtime);
}

// PHASE G214 (default-off, DC2_G214_LWSCAN=1): skin-matrix collapse scanner. G213 localized the
// MAP-4 head/cap whole-batch drop to the cap sub-mesh (tbp=0x35a0) + face (tbp=0x3420) emitting
// ZERO triangles on back-view frames (upstream of the GS) -- the signature of those verts being
// collapsed/off-screen by a bad per-bone skin matrix so VU1 clips the entire batch. G209/G212's
// probe only samples GetLWMatrix calls INSIDE the outline-draw window (g_g37_in_outline > 0), i.e.
// the mgGetDrawRect AABB phase -- it tracked the outline frame 0x8638b0, which G213 proved is a RED
// HERRING (valid matrix, ret=1, head still gone). The SKIN matrices are built EARLIER, during
// SetDeformMesh->DeformMesh->MotionProc2->GetLWMatrix, OUTSIDE that window. This scanner inspects
// EVERY GetLWMatrix output (a1) and reports any collapsed (row0 magnitude ~0 & zero translation, or
// non-finite) world matrix with the identity + present tick, so a missing frame (e.g. 900/1200) can
// be checked for a collapsing bone the old probe never sampled. Replaces g212_lwmatrix_guard in the
// table when enabled and reproduces its preempt suppression, so default behavior is unchanged.
static bool g214_lwscan_enabled()
{
    static const bool on = dc2_env_flag_enabled("DC2_G214_LWSCAN");
    return on;
}
static std::atomic<uint64_t> g_g214_calls{0};
static std::atomic<uint64_t> g_g214_collapses{0};
static std::atomic<uint64_t> g_g214_logged{0};
static void g214_lwscan_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4); // a0 = mgCFrame*
    const uint32_t outp = getRegU32(ctx, 5); // a1 = float[4][4] world matrix out
    const bool inOutline = g_g37_in_outline.load(std::memory_order_relaxed) > 0;

    // Reproduce g212_lwmatrix_guard's scoped preempt suppression (this probe owns the table slot).
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    GetLWMatrix__8mgCFrameFPA4_f_0x137030(rdram, ctx, runtime);
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);

    const uint64_t nCalls = g_g214_calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (outp <= 0x80000u || outp >= 0x2000000u)
        return;
    auto rf = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, outp + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
    const float r0x = rf(0x00u), r0y = rf(0x04u), r0z = rf(0x08u);
    const float tx = rf(0x30u), ty = rf(0x34u), tz = rf(0x38u);
    const float scale0 = std::sqrt(r0x * r0x + r0y * r0y + r0z * r0z);
    const bool nonFinite = !std::isfinite(scale0) || !std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(tz);
    const bool zeroMat = scale0 == 0.0f && tx == 0.0f && ty == 0.0f && tz == 0.0f;
    // G228: for the DA physics output frames (the gem's bone), dump the full basis + 3x3
    // determinant regardless of zero/degenerate -- a near-zero-det (collapsed) basis vanishes
    // the gem without tripping the all-zero check above.
    if (g228_dascan_enabled() && g228_is_da_frame(self))
    {
        const float r1x = rf(0x10u), r1y = rf(0x14u), r1z = rf(0x18u);
        const float r2x = rf(0x20u), r2y = rf(0x24u), r2z = rf(0x28u);
        const float det = r0x * (r1y * r2z - r1z * r2y)
                        - r0y * (r1x * r2z - r1z * r2x)
                        + r0z * (r1x * r2y - r1y * r2x);
        const uint64_t nl = g_g228_logged.fetch_add(1u, std::memory_order_relaxed);
        if (nl < 400u)
            std::fprintf(stderr,
                         "[G228:daMat] tick=%llu frame=0x%08x det=%g scale0=%g t=(%g,%g,%g) "
                         "r0=(%g,%g,%g) r1=(%g,%g,%g) r2=(%g,%g,%g)\n",
                         (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                         self, (double)det, (double)scale0, (double)tx, (double)ty, (double)tz,
                         (double)r0x, (double)r0y, (double)r0z,
                         (double)r1x, (double)r1y, (double)r1z,
                         (double)r2x, (double)r2y, (double)r2z);
    }
    // G229: the actual pendant visual can be below (rather than equal to) a DA output
    // frame.  Count every registered descendant, but log only DA roots and frames that
    // own a visual so the whole character skeleton cannot saturate the useful window.
    if (g229_datree_enabled())
    {
        const int slot = g229_frame_slot(self);
        if (slot >= 0)
        {
            const uint64_t slotCalls = g_g229_frames[slot].lwCalls.fetch_add(
                1u, std::memory_order_relaxed) + 1u;
            const uint32_t root = g_g229_frames[slot].root.load(std::memory_order_relaxed);
            const uint32_t visual = g_g229_frames[slot].visual.load(std::memory_order_relaxed);
            // Roots are extremely hot (hundreds of identical reads per second).  Sample
            // them sparsely so they cannot consume the global cap before a visual owner
            // such as atoramiria--m1- is reached.
            const bool sampledRoot = self == root && (slotCalls <= 8u || (slotCalls % 200u) == 0u);
            if (visual != 0u || sampledRoot)
            {
                const float r1x = rf(0x10u), r1y = rf(0x14u), r1z = rf(0x18u);
                const float r2x = rf(0x20u), r2y = rf(0x24u), r2z = rf(0x28u);
                const float det = r0x * (r1y * r2z - r1z * r2y)
                                - r0y * (r1x * r2z - r1z * r2x)
                                + r0z * (r1x * r2y - r1y * r2x);
                const uint64_t nl = g_g229_logged.fetch_add(1u, std::memory_order_relaxed);
                if (nl < 800u)
                {
                    char name[64];
                    g229_frame_name(rdram, self, name);
                    std::fprintf(stderr,
                                 "[G229:lw] tick=%llu self=0x%08x root=0x%08x name=%s "
                                 "visual=0x%08x calls=%llu det=%g scale0=%g t=(%g,%g,%g)\n",
                                 (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                                 self, root, name[0] ? name : "<none>", visual,
                                 (unsigned long long)slotCalls,
                                 (double)det, (double)scale0,
                                 (double)tx, (double)ty, (double)tz);
                }
            }
        }
    }
    if (nonFinite || zeroMat)
    {
        const uint64_t nc = g_g214_collapses.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint64_t nl = g_g214_logged.load(std::memory_order_relaxed);
        // Log the first 200 collapses in full, then 1-in-64, to keep the identity/tick set
        // visible without flooding (collapses should be rare -- one bone per bad frame).
        if (nl < 200u || (nc % 64u) == 0u)
        {
            g_g214_logged.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[G214:collapse] tick=%llu self=0x%08x inOutline=%d %s scale0=%g t=(%g,%g,%g)\n",
                         (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                         self, inOutline ? 1 : 0, nonFinite ? "NONFIN" : "ZERO",
                         (double)scale0, (double)tx, (double)ty, (double)tz);
        }
    }
    if ((nCalls % 20000ull) == 0ull)
    {
        std::fprintf(stderr, "[G214:scan] tick=%llu calls=%llu collapses=%llu\n",
                     (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                     (unsigned long long)nCalls,
                     (unsigned long long)g_g214_collapses.load(std::memory_order_relaxed));
    }
}

// PHASE G206b: does the mesh-emit (Draw__12mgCVisualMDT) actually grow the mgVif1Packet
// for a part whose mgGetDrawRect bbox/ret says "visible"? Bbox-visible-but-zero-pktDelta
// would mean the geometry collapses INSIDE the mesh emit (skin matrix / cull), not at the
// GetDrawRect gate. Scoped to the same g_g37_in_outline window as G206's bbox probe so
// call order lines up 1:1 with the 8 [G34:outline]/[G206:bbox] part entries per frame.
static std::atomic<uint32_t> g_g206_mdt{0};
static void g206_mdt_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pkt = dc2_read_u32(rdram, gp - 0x788Cu);
    const uint32_t before = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t mesh = getRegU32(ctx, 4);
    const bool inOutline = g_g37_in_outline.load(std::memory_order_relaxed) > 0;
    Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(rdram, ctx, runtime);
    if (!inOutline)
        return;
    const uint32_t after = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t n = g_g206_mdt.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 56u || (n % 240u) == 0u)
    {
        std::fprintf(stderr, "[G206:mdt] n=%u mesh=0x%x ret=0x%x pktDelta=0x%x\n",
                     n, mesh, ret, after - before);
        std::fflush(stderr);
    }
}

static std::atomic<uint32_t> g_g34_divsprite{0};
static std::atomic<uint32_t> g_g36_packet_dump{0};
static std::atomic<uint32_t> g_g36_directdata{0};
static std::atomic<uint32_t> g_g36_endprim2{0};
static std::atomic<uint32_t> g_g36_rectfix{0};

static uint64_t g36_read_qword(uint8_t *rdram, uint32_t addr)
{
    return dc2_read_u64(rdram, addr & 0x1FFFFFFFu);
}

static const char *g36_gs_reg_name(uint8_t reg)
{
    switch (reg)
    {
    case 0x00: return "PRIM";
    case 0x01: return "RGBAQ";
    case 0x02: return "ST";
    case 0x03: return "UV";
    case 0x04: return "XYZF2";
    case 0x05: return "XYZ2";
    case 0x06: return "TEX0_1";
    case 0x07: return "TEX0_2";
    case 0x0E: return "A+D";
    case 0x0F: return "NOP";
    default: return "REG";
    }
}

static bool g36_parse_gif_tag(uint64_t tagLo,
                              uint64_t tagHi,
                              uint32_t remainingBytes,
                              uint32_t *outNloop,
                              uint32_t *outNreg,
                              uint8_t *outFlg,
                              uint32_t *outBytes)
{
    // Bits 16..45 are reserved in GIFtag words. The enclosing VIF/DMA tags in an
    // mgVif1Packet often look superficially GIF-like, but have these bits set.
    if ((tagLo & 0x00003FFFFFFF0000ull) != 0ull)
        return false;

    const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
    if (nreg == 0u)
        nreg = 16u;
    if (nloop == 0u || flg > 2u)
        return false;

    uint64_t payloadBytes = 0ull;
    if (flg == 0u)
        payloadBytes = static_cast<uint64_t>(nloop) * static_cast<uint64_t>(nreg) * 16ull;
    else if (flg == 1u)
        payloadBytes = static_cast<uint64_t>((nloop * nreg + 1u) / 2u) * 16ull;
    else
        payloadBytes = static_cast<uint64_t>(nloop) * 16ull;

    const uint64_t totalBytes = 16ull + payloadBytes;
    if (totalBytes > static_cast<uint64_t>(remainingBytes) || totalBytes > 0x20000ull)
        return false;

    (void)tagHi;
    *outNloop = nloop;
    *outNreg = nreg;
    *outFlg = flg;
    *outBytes = static_cast<uint32_t>(totalBytes);
    return true;
}

static void g36_decode_divsprite_packet(uint8_t *rdram,
                                        uint32_t dumpNo,
                                        uint32_t prim,
                                        uint32_t tex,
                                        uint32_t base,
                                        uint32_t end)
{
    const uint32_t physBase = base & 0x1FFFFFFFu;
    const uint32_t physEnd = end & 0x1FFFFFFFu;
    if (physBase < 0x80000u || physBase >= 0x2000000u ||
        physEnd <= physBase || physEnd > 0x2000000u)
    {
        std::fprintf(stderr,
                     "[G36:packet] dump=%u prim=0x%x tex=0x%x invalid base=0x%x end=0x%x physBase=0x%x physEnd=0x%x\n",
                     dumpNo, prim, tex, base, end, physBase, physEnd);
        return;
    }

    const uint32_t totalBytes = physEnd - physBase;
    std::fprintf(stderr,
                 "[G36:packet] dump=%u prim=0x%x tex=0x%x base=0x%x end=0x%x bytes=0x%x tex0=0x%016llx primState=0x%llx\n",
                 dumpNo, prim, tex, base, end, totalBytes,
                 static_cast<unsigned long long>((tex > 0x80000u && tex < 0x2000000u) ? dc2_read_u64(rdram, tex + 0x38u) : 0u),
                 static_cast<unsigned long long>(dc2_read_u64(rdram, prim + 0x50u)));

    const uint32_t rawBytes = (totalBytes < 0x180u) ? totalBytes : 0x180u;
    for (uint32_t raw = 0u; raw + 16u <= rawBytes; raw += 16u)
    {
        std::fprintf(stderr,
                     "[G36:raw] dump=%u off=0x%x q0=0x%016llx q1=0x%016llx\n",
                     dumpNo, raw,
                     static_cast<unsigned long long>(g36_read_qword(rdram, physBase + raw)),
                     static_cast<unsigned long long>(g36_read_qword(rdram, physBase + raw + 8u)));
    }

    uint32_t scan = 0u;
    uint32_t tag = 0u;
    uint32_t currentTbp = 0xFFFFFFFFu;
    uint32_t currentPsm = 0xFFFFFFFFu;
    uint32_t currentPrim = 0xFFFFFFFFu;
    uint32_t xyzLogged = 0u;

    while (scan + 16u <= totalBytes && tag < 64u)
    {
        const uint32_t tagAddr = physBase + scan;
        const uint64_t tagLo = g36_read_qword(rdram, tagAddr);
        const uint64_t tagHi = g36_read_qword(rdram, tagAddr + 8u);

        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t flg = 0u;
        uint32_t tagBytes = 0u;
        if (!g36_parse_gif_tag(tagLo, tagHi, totalBytes - scan, &nloop, &nreg, &flg, &tagBytes))
        {
            scan += 16u;
            continue;
        }

        uint32_t off = scan + 16u;
        const bool eop = ((tagLo >> 15u) & 1u) != 0u;
        const bool pre = ((tagLo >> 46u) & 1u) != 0u;
        if (pre)
            currentPrim = static_cast<uint32_t>((tagLo >> 47u) & 0x7FFu);

        uint8_t regs[16];
        for (uint32_t i = 0u; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 0xFu);

        std::fprintf(stderr,
                     "[G36:tag] dump=%u tag=%u off=0x%x bytes=0x%x lo=0x%016llx hi=0x%016llx nloop=%u nreg=%u flg=%u pre=%u eop=%u prim=0x%x regs=",
                     dumpNo, tag, tagAddr - physBase, tagBytes,
                     static_cast<unsigned long long>(tagLo),
                     static_cast<unsigned long long>(tagHi),
                     nloop, nreg, static_cast<uint32_t>(flg), pre ? 1u : 0u, eop ? 1u : 0u, currentPrim);
        for (uint32_t i = 0u; i < nreg; ++i)
            std::fprintf(stderr, "%s%s", (i == 0u) ? "" : ",", g36_gs_reg_name(regs[i]));
        std::fprintf(stderr, "\n");

        if (flg == 0u)
        {
            for (uint32_t loop = 0u; loop < nloop; ++loop)
            {
                for (uint32_t r = 0u; r < nreg; ++r)
                {
                    if (off + 16u > totalBytes)
                        goto done;
                    const uint32_t itemAddr = physBase + off;
                    const uint64_t lo = g36_read_qword(rdram, itemAddr);
                    const uint64_t hi = g36_read_qword(rdram, itemAddr + 8u);
                    off += 16u;

                    uint8_t reg = regs[r];
                    uint64_t value = lo;
                    if (reg == 0x0Eu)
                        reg = static_cast<uint8_t>(hi & 0xFFu);

                    if (reg == 0x00u)
                    {
                        currentPrim = static_cast<uint32_t>(value & 0x7FFu);
                        std::fprintf(stderr,
                                     "[G36:item] dump=%u tag=%u loop=%u slot=%u off=0x%x reg=PRIM value=0x%llx hi=0x%llx type=%u tme=%u abe=%u fst=%u ctxt=%u\n",
                                     dumpNo, tag, loop, r, itemAddr - physBase,
                                     static_cast<unsigned long long>(value),
                                     static_cast<unsigned long long>(hi),
                                     currentPrim & 7u, (currentPrim >> 4u) & 1u, (currentPrim >> 6u) & 1u,
                                     (currentPrim >> 8u) & 1u, (currentPrim >> 9u) & 1u);
                    }
                    else if (reg == 0x06u || reg == 0x07u)
                    {
                        currentTbp = static_cast<uint32_t>(value & 0x3FFFu);
                        currentPsm = static_cast<uint32_t>((value >> 20u) & 0x3Fu);
                        std::fprintf(stderr,
                                     "[G36:item] dump=%u tag=%u loop=%u slot=%u off=0x%x reg=%s tex0=0x%016llx hi=0x%llx tbp=0x%x psm=0x%x tbw=%u\n",
                                     dumpNo, tag, loop, r, itemAddr - physBase,
                                     g36_gs_reg_name(reg),
                                     static_cast<unsigned long long>(value),
                                     static_cast<unsigned long long>(hi),
                                     currentTbp, currentPsm, static_cast<uint32_t>((value >> 14u) & 0x3Fu));
                    }
                    else if ((reg == 0x04u || reg == 0x05u) && xyzLogged < 32u)
                    {
                        const float x = static_cast<float>(value & 0xFFFFu) / 16.0f;
                        const float y = static_cast<float>((value >> 16u) & 0xFFFFu) / 16.0f;
                        std::fprintf(stderr,
                                     "[G36:item] dump=%u tag=%u loop=%u slot=%u off=0x%x reg=%s value=0x%llx hi=0x%llx xy=(%.1f,%.1f) prim=0x%x tex=0x%x/0x%x\n",
                                     dumpNo, tag, loop, r, itemAddr - physBase,
                                     g36_gs_reg_name(reg),
                                     static_cast<unsigned long long>(value),
                                     static_cast<unsigned long long>(hi),
                                     static_cast<double>(x), static_cast<double>(y),
                                     currentPrim, currentTbp, currentPsm);
                        ++xyzLogged;
                    }
                }
            }
        }
        else if (flg == 1u)
        {
            const uint32_t values = nloop * nreg;
            const uint32_t bytes = ((values + 1u) / 2u) * 16u;
            if (off + bytes > totalBytes)
                goto done;
            off += bytes;
        }
        else if (flg == 2u)
        {
            const uint64_t bytes = static_cast<uint64_t>(nloop) * 16ull;
            if (bytes > static_cast<uint64_t>(totalBytes - off))
                goto done;
            off += static_cast<uint32_t>(bytes);
        }
        else
        {
            break;
        }
        scan += tagBytes;
        ++tag;
    }
done:
    std::fflush(stderr);
}

static void g36_directdata_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool log = g36_packet_trace_enabled() &&
                     g_g34_in_divsprite.load(std::memory_order_relaxed) > 0;
    const uint32_t prim = getRegU32(ctx, 4);
    const uint32_t qwords = getRegU32(ctx, 5);
    const uint32_t before = (prim > 0x80000u && prim < 0x2000000u) ? dc2_read_u32(rdram, prim + 0xDCu) : 0u;
    DirectData__11mgCDrawPrimFi_0x134b00(rdram, ctx, runtime);
    if (log)
    {
        const uint32_t n = g_g36_directdata.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 16u)
        {
            const uint32_t after = (prim > 0x80000u && prim < 0x2000000u) ? dc2_read_u32(rdram, prim + 0xDCu) : 0u;
            std::fprintf(stderr,
                         "[G36:direct] n=%u prim=0x%x qwords=0x%x ret(v0)=0x%x before=0x%x after=0x%x delta=0x%x e0=0x%x e4=0x%x e8=0x%x ec=0x%x f0=0x%x\n",
                         n, prim, qwords, getRegU32(ctx, 2), before, after, after - before,
                         dc2_read_u32(rdram, prim + 0xE0u), dc2_read_u32(rdram, prim + 0xE4u),
                         dc2_read_u32(rdram, prim + 0xE8u), dc2_read_u32(rdram, prim + 0xECu),
                         dc2_read_u32(rdram, prim + 0xF0u));
            std::fflush(stderr);
        }
    }
}

static void g36_endprim2_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool log = g36_packet_trace_enabled() &&
                     g_g34_in_divsprite.load(std::memory_order_relaxed) > 0;
    const uint32_t prim = getRegU32(ctx, 4);
    const uint32_t n = log ? (g_g36_endprim2.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;
    uint32_t beforeDc = 0u, e0 = 0u, e4 = 0u, e8 = 0u, ec = 0u, f0 = 0u, nreg = 0u;
    if (log && n <= 16u && prim > 0x80000u && prim < 0x2000000u)
    {
        beforeDc = dc2_read_u32(rdram, prim + 0xDCu);
        e0 = dc2_read_u32(rdram, prim + 0xE0u);
        e4 = dc2_read_u32(rdram, prim + 0xE4u);
        e8 = dc2_read_u32(rdram, prim + 0xE8u);
        ec = dc2_read_u32(rdram, prim + 0xECu);
        f0 = dc2_read_u32(rdram, prim + 0xF0u);
        nreg = dc2_read_u32(rdram, prim + 0x104u);
        std::fprintf(stderr,
                     "[G36:endprim.pre] n=%u prim=0x%x dc=0x%x e0=0x%x e4=0x%x e8=0x%x ec=0x%x f0=0x%x nreg=%u qwc(e8->dc)=0x%x\n",
                     n, prim, beforeDc, e0, e4, e8, ec, f0, nreg, (beforeDc - e8) >> 4u);
        if (e8 > 0x80000u && e8 < 0x2000000u)
        {
            for (uint32_t off = 0u; off < 0x90u && e8 + off + 8u < 0x2000000u; off += 16u)
            {
                std::fprintf(stderr,
                             "[G36:endraw] n=%u off=0x%x q0=0x%016llx q1=0x%016llx\n",
                             n, off,
                             static_cast<unsigned long long>(g36_read_qword(rdram, e8 + off)),
                             static_cast<unsigned long long>(g36_read_qword(rdram, e8 + off + 8u)));
            }
        }
    }

    EndPrim2__11mgCDrawPrimFv_0x134940(rdram, ctx, runtime);

    if (log && n <= 16u && prim > 0x80000u && prim < 0x2000000u)
    {
        const uint32_t afterDc = dc2_read_u32(rdram, prim + 0xDCu);
        std::fprintf(stderr,
                     "[G36:endprim.post] n=%u prim=0x%x dcBefore=0x%x dcAfter=0x%x e8TagLo=0x%016llx e8TagHi=0x%016llx ec=0x%x f0=0x%x\n",
                     n, prim, beforeDc, afterDc,
                     static_cast<unsigned long long>(g36_read_qword(rdram, e8)),
                     static_cast<unsigned long long>(g36_read_qword(rdram, e8 + 8u)),
                     ec ? dc2_read_u32(rdram, ec) : 0u,
                     f0 ? dc2_read_u32(rdram, f0) : 0u);
        std::fflush(stderr);
    }
}

static bool g36_repair_collapsed_divsprite_rect(uint8_t *rdram, uint32_t rect, uint32_t tex)
{
    if (rect <= 0x80000u || rect >= 0x2000000u ||
        tex <= 0x80000u || tex >= 0x2000000u)
    {
        return false;
    }

    const uint64_t tex0 = dc2_read_u64(rdram, tex + 0x38u);
    const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu);
    const uint32_t psm = static_cast<uint32_t>((tex0 >> 20u) & 0x3Fu);
    const uint32_t y0 = dc2_read_u32(rdram, rect + 0x0u);
    const uint32_t x0 = dc2_read_u32(rdram, rect + 0x4u);
    const uint32_t y1 = dc2_read_u32(rdram, rect + 0x8u);
    const uint32_t x1 = dc2_read_u32(rdram, rect + 0xCu);

    // G36: runner-only costume preview repair. The outline composite is reached with
    // the correct CT32 RTT texture (0x2720) but its screen-space bounds collapse to
    // the center point (0x1000,0x0d00), so DrawDivSprite4 never emits DirectData.
    // PCSX2 at the same "Select Max's Costume" call enters with these extents:
    // y0=0x0dff, x0=0x0000, y1=0x2000, x1=0x17ce.
    if (tbp != 0x2720u || psm != 0u ||
        y0 != 0x1000u || y1 != 0x1000u ||
        x0 != 0x0D00u || x1 != 0x0D00u)
    {
        return false;
    }

    dc2_write_u32(rdram, rect + 0x0u, 0x0DFFu);
    dc2_write_u32(rdram, rect + 0x4u, 0x0000u);
    dc2_write_u32(rdram, rect + 0x8u, 0x2000u);
    dc2_write_u32(rdram, rect + 0xCu, 0x17CEu);

    const uint32_t n = g_g36_rectfix.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g36_packet_trace_enabled() && (n <= 16u || (n % 240u) == 0u))
    {
        std::fprintf(stderr,
                     "[G36:rectfix] n=%u rect=0x%x tex=0x%x tex0=0x%016llx old=(0x%x,0x%x,0x%x,0x%x) new=(0xdff,0x0,0x2000,0x17ce)\n",
                     n, rect, tex, static_cast<unsigned long long>(tex0), y0, x0, y1, x1);
        std::fflush(stderr);
    }
    return true;
}

// Wrap DrawDivSprite4@0x17cb20 = the RTT->display composite blit (the missing step on the
// runner per G33). a2 = mgCTexture* it samples (the 0x2720 framebuffer grabbed by
// mgGetFrameBuffer). Confirms the composite is actually reached and dumps the texture struct
// head so we can read its tbp0/psm (CT32 vs T8) wherever the RTT texture stores them.
static void g34_divsprite_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t prim = getRegU32(ctx, 4);  // a0 = mgCDrawPrim*
    const uint32_t rect = getRegU32(ctx, 5);  // a1 = mgRect<int>*
    const uint32_t tex = getRegU32(ctx, 6);   // a2 = mgCTexture*
    const uint32_t color = getRegU32(ctx, 7); // a3 = rgba/int vector
    const uint32_t expand = getRegU32(ctx, 8);
    const uint32_t arg9 = getRegU32(ctx, 9);
    const uint32_t gp = getRegU32(ctx, 28);
    g36_repair_collapsed_divsprite_rect(rdram, rect, tex);
    const uint32_t n = g_g34_divsprite.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 16u || (n % 240u) == 0u)
    {
        char buf[512];
        int q = std::snprintf(buf, sizeof(buf),
                              "[G34:divsprite] n=%u prim=0x%x rect=0x%x tex=0x%x color=0x%x expand=0x%x arg9=0x%x gpOff=(0x%x,0x%x) rectv:",
                              n, prim, rect, tex, color, expand, arg9,
                              dc2_read_u32(rdram, gp + 0xFFFF8798u), // gp-0x7868
                              dc2_read_u32(rdram, gp + 0xFFFF879Cu)); // gp-0x7864
        if (rect > 0x80000u && rect < 0x2000000u)
            for (uint32_t o = 0; o < 0x10u && q < (int)sizeof(buf) - 24; o += 4u)
                q += std::snprintf(buf + q, sizeof(buf) - q, " %x:0x%x", o, dc2_read_u32(rdram, rect + o));
        q += std::snprintf(buf + q, sizeof(buf) - q, " texhead:");
        if (tex > 0x80000u && tex < 0x2000000u)
            for (uint32_t o = 0; o <= 0x40u && q < (int)sizeof(buf) - 16; o += 4u)
                q += std::snprintf(buf + q, sizeof(buf) - q, " %x:0x%x", o, dc2_read_u32(rdram, tex + o));
        std::fprintf(stderr, "%s\n", buf);
        std::fflush(stderr);
    }
    g_g34_in_divsprite.fetch_add(1, std::memory_order_relaxed);
    DrawDivSprite4__FP11mgCDrawPrim9mgRect_i_P10mgCTexturePiii_0x17cb20(rdram, ctx, runtime);
    g_g34_in_divsprite.fetch_sub(1, std::memory_order_relaxed);

    if (g36_packet_trace_enabled())
    {
        const uint32_t dump = g_g36_packet_dump.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (dump <= 3u)
        {
            const uint32_t base = dc2_read_u32(rdram, prim + 0xD4u);
            const uint32_t end = dc2_read_u32(rdram, prim + 0xDCu);
            g36_decode_divsprite_packet(rdram, dump, prim, tex, base, end);
        }
    }
}
