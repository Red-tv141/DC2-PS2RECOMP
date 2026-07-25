// G371 — EE-side `mgRENDER_INFO` census (diagnostic, default-off: DC2_G371_RI=1).
//
// G370 proved the cutscene gray screen is a VU1-side collapse: every transformed vertex reaches
// the GS at (0,0) with a NaN `q`, the perspective divide runs on an ALL-ZERO transformed vertex,
// and the VU1 transform rows are fully `-FLT_MAX`. It also proved (DC2_G370_VULOAD, 38 hits, all
// re-loads of the VU's own output) that nothing non-finite is DELIVERED into VU data memory.
//
// That last result does NOT exonerate the EE: a **zero** matrix is perfectly finite and would slip
// past a non-finite filter while still producing exactly this chain (zero transform -> zero vertex
// -> 1/0 divide -> saturation -> NaN). The EE's projection/view matrices live in `mgRENDER_INFO`,
// whose layout is known from `SetRenderInfo__13mgRENDER_INFOFfiiffi` @ 0x00138B00 and
// `SetViewMatrix__13mgRENDER_INFOFPA4_fPf` @ 0x00138FC0:
//
//   +0x000  focal length (float)
//   +0x010  view * projection  (the matrix the VU1 low qwords are built from; G30's HW ref)
//   +0x050  secondary composite
//   +0x090  secondary composite
//   +0x0d0  screen/scale matrix
//   +0x110  projection matrix
//   +0x150  projection * camera
//   +0x1a0  CAMERA (view) matrix  <- INPUT: written by the caller BEFORE SetRenderInfo runs
//   +0x3a0  camera position vector
//
// `SetViewMatrix` copies +0x1a0 from its own argument and multiplies it into +0x10/+0x50/+0x90, so
// if the camera matrix at +0x1a0 goes degenerate (all-zero rows) the whole downstream chain does
// too — with no non-finite value anywhere on the EE side. This census dumps those fields once per
// present tick so a HEALTHY tick and a GRAY tick can be compared WITHIN ONE RUN (cross-run frame
// comparison is invalid here, see the G223/G241 host-tick-drift rule), and so the same guest
// addresses can be read on PCSX2 for a hardware A/B.
//
// `DC2_G371_RI_BASE=0x380ec0` overrides the instance address (G30's, the only one observed).

// File scope on purpose: inside an anonymous namespace this declaration would take INTERNAL
// linkage and fail to resolve against the definition in ps2_runtime.cpp (the G214 trap).
extern std::atomic<uint64_t> g_dc2PresentTick;

namespace
{
    static const bool s_DC2_G371_RI = (std::getenv("DC2_G371_RI") != nullptr);

    static uint32_t g371RenderInfoBase()
    {
        static const uint32_t s_base = []() -> uint32_t {
            const char *e = std::getenv("DC2_G371_RI_BASE");
            return (e != nullptr) ? (uint32_t)std::strtoul(e, nullptr, 0) : 0x00380EC0u;
        }();
        return s_base;
    }

    // A matrix is "degenerate" when any of its first three rows is entirely zero — that is exactly
    // the shape that turns a finite EE matrix into a zero transformed vertex on VU1.
    static int g371DegenRows(const float *m)
    {
        int rows = 0;
        for (int r = 0; r < 4; ++r)
        {
            bool allZero = true;
            for (int c = 0; c < 4; ++c)
                if (m[r * 4 + c] != 0.0f)
                    allZero = false;
            if (allZero)
                ++rows;
        }
        return rows;
    }

    static void g371DumpMatrix(const uint8_t *rdram, uint32_t base, uint32_t off, const char *tag,
                               uint64_t tick)
    {
        float m[16];
        std::memcpy(m, rdram + ((base + off) & 0x1FFFFFFFu), sizeof(m));
        std::fprintf(stderr,
                     "[G371:ri] tick=%llu %s@+0x%03x degen=%d "
                     "[% .6g % .6g % .6g % .6g | % .6g % .6g % .6g % .6g | "
                     "% .6g % .6g % .6g % .6g | % .6g % .6g % .6g % .6g]\n",
                     (unsigned long long)tick, tag, off, g371DegenRows(m),
                     m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                     m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    }

    // One dump per present tick, capped. Cheap enough to sit on the VIF1 stream entry: the tick
    // latch rejects every call but the first of each frame.
    void g371RenderInfoCensus(const uint8_t *rdram)
    {
        if (!s_DC2_G371_RI || rdram == nullptr)
            return;
        const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
        static std::atomic<uint64_t> s_lastTick{~0ull};
        static std::atomic<uint32_t> s_dumps{0};
        uint64_t last = s_lastTick.load(std::memory_order_relaxed);
        if (tick == last || s_dumps.load(std::memory_order_relaxed) >= 4000u)
            return;
        if (!s_lastTick.compare_exchange_strong(last, tick, std::memory_order_relaxed))
            return;
        s_dumps.fetch_add(1u, std::memory_order_relaxed);

        const uint32_t base = g371RenderInfoBase();
        float focal = 0.0f;
        std::memcpy(&focal, rdram + (base & 0x1FFFFFFFu), 4);
        float pos[4];
        std::memcpy(pos, rdram + ((base + 0x3A0u) & 0x1FFFFFFFu), sizeof(pos));
        std::fprintf(stderr, "[G371:ri] tick=%llu focal=% .6g campos=(% .6g % .6g % .6g % .6g)\n",
                     (unsigned long long)tick, focal, pos[0], pos[1], pos[2], pos[3]);
        g371DumpMatrix(rdram, base, 0x1A0u, "camera", tick);  // INPUT (written by the caller)
        g371DumpMatrix(rdram, base, 0x110u, "proj  ", tick);
        g371DumpMatrix(rdram, base, 0x150u, "projXc", tick);
        g371DumpMatrix(rdram, base, 0x010u, "final ", tick);  // what VU1 qw0..0xc is built from
    }
}
