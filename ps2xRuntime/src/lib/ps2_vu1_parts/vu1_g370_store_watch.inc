// G370 — VU1 store-watch: which VU1 instruction writes the non-finite `q` that the cutscene
// gray-window GIF packets carry? (diagnostic, default-off: DC2_G370_VUSTORE=1)
//
// G370's packet probe proved the poison is delivered on PATH1 (VU1 XGKICK, 5.94M NaN-q vertex
// qwords; PATH2/PATH3 = 0), which REFUTES G369's "EE-built packet" inference and means G369's
// `DC2_G369_VUNAN=1` (zero non-finite `vf` lanes) was a false negative — it scanned the register
// file, not the store path, and under default-on MTVU the VU1 program runs on the worker thread.
//
// Every qword in a PATH1 packet is written to VU data memory by a store (SQ / SQI / SQD). This
// probe reports the first stores whose stored lanes are non-finite, with the VU pc, so the guilty
// instruction (and therefore the packer program and the op that produced the value) is named
// directly instead of inferred.

// File scope on purpose: inside an anonymous namespace this declaration would get INTERNAL
// linkage and fail to resolve against ps2_runtime.cpp's definition (the G214 trap).
extern std::atomic<uint64_t> g_dc2PresentTick;

namespace
{
    // File-scope const: these guards sit in per-store / per-load / per-divide VU1 hot paths,
    // where even a function-local static's guard check is a measurable per-op cost (G268).
    static const bool s_DC2_G370_VUSTORE = (std::getenv("DC2_G370_VUSTORE") != nullptr);
    inline bool g370StoreWatchEnabled() { return s_DC2_G370_VUSTORE; }

    // DIV denominator watch (DC2_G370_DIV0=1). The clamp arm (DC2_VU1_CLAMP=1) removes every NaN
    // from the PATH1 packets but the gray window survives, so the NaN was a SYMPTOM: the real
    // defect is upstream, where the perspective divide gets a zero/near-zero w. This reports the
    // divides that saturate, with both source vectors, so a zeroed transform matrix (all lanes 0)
    // is distinguishable from a single bad vertex.
    // File-scope const: these guards sit in per-store / per-load / per-divide VU1 hot paths,
    // where even a function-local static's guard check is a measurable per-op cost (G268).
    static const bool s_DC2_G370_DIV0 = (std::getenv("DC2_G370_DIV0") != nullptr);
    inline bool g370Div0Enabled() { return s_DC2_G370_DIV0; }

    // Present tick, so a dump can be placed inside or outside the gray window.
    static uint64_t g370PresentTick()
    {
        return g_dc2PresentTick.load(std::memory_order_relaxed);
    }

    void g370NoteVuDiv(uint32_t pc, float num, float den, const float *vfs, const float *vft,
                       const float (*vfAll)[4], uint32_t isReg, uint32_t itReg)
    {
        if (!g370Div0Enabled())
            return;
        const bool bad = (den == 0.0f) || !std::isfinite(den) || !std::isfinite(num);
        static std::atomic<uint64_t> s_total{0};
        static std::atomic<uint64_t> s_bad{0};
        s_total.fetch_add(1u, std::memory_order_relaxed);
        if (bad)
        {
            const uint64_t n = s_bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 24u || (n % 200000u) == 0u)
                std::fprintf(stderr,
                             "[G370:div0] n=%llu tick=%llu pc=0x%x vf%u/vf%u num=%g den=%g "
                             "vfs=(%g,%g,%g,%g) vft=(%g,%g,%g,%g)\n",
                             (unsigned long long)n, (unsigned long long)g370PresentTick(), pc,
                             isReg, itReg, num, den,
                             vfs[0], vfs[1], vfs[2], vfs[3], vft[0], vft[1], vft[2], vft[3]);
        }
        // One-shot full VF dump at the first saturating divide: distinguishes a ZEROED transform
        // matrix (matrix regs all zero) from a single zeroed source vertex.
        if (bad && vfAll != nullptr)
        {
            // One dump per present tick (capped): comparing a gray-window tick against a
            // healthy tick in the SAME run names the input that went zero.
            static std::atomic<uint64_t> s_lastTick{~0ull};
            static std::atomic<uint32_t> s_dumps{0};
            const uint64_t tk = g370PresentTick();
            uint64_t last = s_lastTick.load(std::memory_order_relaxed);
            if (tk != last && s_dumps.load(std::memory_order_relaxed) < 200u &&
                s_lastTick.compare_exchange_strong(last, tk, std::memory_order_relaxed))
            {
                s_dumps.fetch_add(1u, std::memory_order_relaxed);
                char b[2000];
                int p = 0;
                for (int r = 0; r < 32 && p < (int)sizeof(b) - 80; ++r)
                    p += std::snprintf(b + p, sizeof(b) - p, "VF%02d(%g,%g,%g,%g) ",
                                       r, vfAll[r][0], vfAll[r][1], vfAll[r][2], vfAll[r][3]);
                std::fprintf(stderr, "[G370:divvf] tick=%llu pc=0x%x %s\n",
                             (unsigned long long)tk, pc, b);
            }
        }
        static std::atomic<uint64_t> s_tick{0};
        if ((s_tick.fetch_add(1u, std::memory_order_relaxed) % 500000ull) == 0ull)
            std::fprintf(stderr, "[G370:divagg] bad=%llu/%llu\n",
                         (unsigned long long)s_bad.load(), (unsigned long long)s_total.load());
    }


    // Load-watch (DC2_G370_VULOAD=1). Splits the two possible origins of the saturated transform
    // matrix seen in the gray window: a value LOADED from VU data memory (so the poison was
    // delivered by the EE via VIF unpack / a previous packet) versus a value COMPUTED inside the
    // VU program (interpreter or genuine guest math). Flags |v| >= 1e38 or non-finite.
    // File-scope const: these guards sit in per-store / per-load / per-divide VU1 hot paths,
    // where even a function-local static's guard check is a measurable per-op cost (G268).
    static const bool s_DC2_G370_VULOAD = (std::getenv("DC2_G370_VULOAD") != nullptr);
    inline bool g370LoadWatchEnabled() { return s_DC2_G370_VULOAD; }

    void g370NoteVuLoad(uint32_t pc, uint32_t addr, const float lanes[4], uint32_t dest)
    {
        if (!g370LoadWatchEnabled())
            return;
        int bad = -1;
        for (int i = 0; i < 4; ++i)
        {
            if ((dest & (0x8u >> i)) == 0u)
                continue;
            const float v = lanes[i];
            if (!std::isfinite(v) || std::fabs(v) >= 1.0e38f)
            {
                bad = i;
                break;
            }
        }
        static std::atomic<uint64_t> s_hits{0};
        if (bad < 0)
            return;
        const uint64_t n = s_hits.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 24u || (n % 200000u) == 0u)
            std::fprintf(stderr,
                         "[G370:vuload] n=%llu tick=%llu pc=0x%x addr=0x%x lane=%c v=%g all=(%g,%g,%g,%g)\n",
                         (unsigned long long)n, (unsigned long long)g370PresentTick(), pc, addr,
                         "xyzw"[bad], lanes[bad], lanes[0], lanes[1], lanes[2], lanes[3]);
    }

    // op: 0x01 = SQ, 0x35/0x11 = SQI, 0x37/0x13 = SQD.
    void g370NoteVuStore(uint32_t pc, uint32_t op, uint32_t srcVf, uint32_t addr,
                         const float lanes[4], uint32_t dest)
    {
        if (!g370StoreWatchEnabled())
            return;
        int bad = -1;
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t mask = 0x8u >> i;
            if ((dest & mask) == 0u)
                continue;
            if (!std::isfinite(lanes[i]))
            {
                bad = i;
                break;
            }
        }
        if (bad < 0)
            return;
        static std::atomic<uint64_t> s_hits{0};
        const uint64_t n = s_hits.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 32u || (n % 100000u) == 0u)
        {
            uint32_t bits;
            std::memcpy(&bits, &lanes[bad], 4);
            std::fprintf(stderr,
                         "[G370:vustore] n=%llu pc=0x%x op=0x%02x vf%u dest=0x%x addr=0x%x lane=%c "
                         "bits=0x%08x val=%g all=(%g,%g,%g,%g)\n",
                         (unsigned long long)n, pc, op, srcVf, dest, addr, "xyzw"[bad], bits,
                         lanes[bad], lanes[0], lanes[1], lanes[2], lanes[3]);
        }
    }
}
