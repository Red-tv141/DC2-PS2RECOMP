// ===== G424: run-based host->local IMAGE upload writer =====
//
// PREMISE (G424 thread gate, lean MAP-0, 2026-07-30). `[G332:gsw]` puts the GS worker's TOTAL busy
// time at 50.4-52.7 ms/f against a 50.3-52.9 ms frame and a 45.5-48.9 ms VU1 worker, so the GS
// worker is the pole and the convertible distance to the runner-up is about 3 ms. `[G316:worker]`
// attributes 38.5-40.8 ms of that to PATH2 (VIF1 DIRECT). Inside PATH2, `[G313:seg]` measures the
// serial IMAGE pixel writer at about 5.1 ms/f: PSMCT32 moves 3.7 MiB/f at 952 MiB/s and PSMT4
// 0.19 MiB/f at 102 MiB/s.
//
// ROOT. `GSMem::WriteCT32`/`WriteP8`/`WriteP4`/... are defined in ps2_gs_memory.cpp; the upload
// loop lives in this translation unit; the Release build is `/O2 /Ob2` with no `/GL`/LTCG. So the
// upload paid one REAL cross-TU call per pixel (~930k calls/frame) and none of the page/block/row
// address setup could be hoisted out of the pixel loop, even though a whole horizontal run shares
// a fixed y, a fixed page row, and a contiguous slice of the swizzle lookup table.
//
// MECHANISM. Hand the writer whole horizontal runs. `GSMem::WriteRun*` (ps2_gs_memory_parts/
// g424_image_run_writers.inc) performs, for pixel i of the run, exactly the same
// `PixelStorageTraits<psm>::Write(...)` call with exactly the same arguments in exactly the same
// order the per-pixel path made — same swizzle tables, same masking, same read-modify-write rules.
// Only the call boundary moves, so the written bytes are identical by construction; the win is
// inlining plus loop-invariant hoisting inside the owning TU.
//
// The run DECOMPOSITION is the only genuinely new logic, so that is what the oracle checks:
// `DC2_G424_VERIFY=1` replays a sampled upload on both paths from the same pre-state and compares
// all 4 MiB of GS memory plus the resumable `m_hwregX`/`m_hwregY` transfer cursor.
//
// RESULT (PROMOTED). Segment: `[G313:seg]` on one binary, both arms — 14.93 -> 6.14 us per IMAGE
// call (-58.8%), PSMCT32 910 -> 2707 MiB/s (2.97x), PSMT4 99.7 -> 184 MiB/s (1.85x), about
// -3.0 ms/f of serial GS-worker CPU. Frame: pooled 4v4 lean MAP-0 in both arm orders,
// 52.24 -> 49.79 ms/f (-2.45 ms, -4.69%; 19.14 -> 20.09 fps) with no overlap between the arms'
// per-run means. That converts almost exactly the pole-to-runner-up distance G424's premise gate
// predicted, so the GS worker now hands the pole to the VU1 worker.
//
// Default: ON.  Kill: DC2_G424_NO_FAST_IMAGE=1
// Verify: DC2_G424_VERIFY=1 (+ DC2_G424_VERIFY_EVERY)   Stats: DC2_G424_STAT=1
//
// NOT covered (left on the legacy per-pixel path): PSMT4HL/PSMT4HH, whose nibble-into-CT32 aliasing
// has its own byte-lane rules, and any unknown psm, which keeps the linear memcpy fallback. Both
// are negligible on this route (2 calls each across a 60 s run).

#include <vector>

namespace GSMem
{
    void WriteRunCT32(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                      const uint8_t *src, uint32_t count);
    void WriteRunZ32(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                     const uint8_t *src, uint32_t count);
    void WriteRunCT24(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                      const uint8_t *src, uint32_t count);
    void WriteRunCT16(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                      const uint8_t *src, uint32_t count);
    void WriteRunCT16S(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                       const uint8_t *src, uint32_t count);
    void WriteRunZ16(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                     const uint8_t *src, uint32_t count);
    void WriteRunZ16S(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                      const uint8_t *src, uint32_t count);
    void WriteRunP8(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                    const uint8_t *src, uint32_t count);
    void WriteRunP8H(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                     const uint8_t *src, uint32_t count);
    void WriteRunP4(uint8_t *data, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y,
                    const uint8_t *src, uint32_t nibble0, uint32_t count);
}

namespace
{
    // Default-on. The kill switch restores the byte-identical legacy per-pixel writer behind one
    // loop-invariant, perfectly-predicted branch, so the control arm stays unperturbed (G421
    // lesson). The former bring-up enable DC2_G424_FAST_IMAGE is accepted as a harmless no-op.
    bool g424FastImageOn()
    {
        static const bool v = !envFlagEnabled("DC2_G424_NO_FAST_IMAGE");
        return v;
    }

    bool g424VerifyOn()
    {
        static const bool v = envFlagEnabled("DC2_G424_VERIFY");
        return v;
    }

    bool g424StatOn()
    {
        static const bool v = envFlagEnabled("DC2_G424_STAT");
        return v;
    }

    uint32_t g424VerifyEvery()
    {
        static const uint32_t v = []() -> uint32_t {
            const char *e = std::getenv("DC2_G424_VERIFY_EVERY");
            const long n = e ? std::atol(e) : 0L;
            return (n > 0L) ? static_cast<uint32_t>(n) : 64u;
        }();
        return v;
    }

    // Everything the run decomposition needs. `hwX`/`hwY` are GS::m_hwregX/m_hwregY: the resumable
    // transfer cursor a chunked IMAGE keeps across packets, so it is part of the compared state.
    struct G424ImageCtx
    {
        uint8_t *vram;
        const uint8_t *data;
        uint32_t sizeBytes;
        uint32_t dbp;
        uint32_t dbw;      // already normalised to >= 1 by the caller
        uint32_t dpsm;
        uint32_t dsax;
        uint32_t dsay;
        uint32_t rrw;
        uint32_t rrh;
        uint32_t *hwX;
        uint32_t *hwY;
    };

    // Returns false when this psm is not covered; the caller then runs the untouched legacy tree.
    bool g424TryFastImageWrite(const G424ImageCtx &c)
    {
        const uint32_t dpsm = c.dpsm;

        // PSMT8 / PSMT4 address their pages 128 pixels wide, so GSMem takes dbw >> 1 — the same
        // halving the legacy callers applied.
        const uint32_t pageBwHalf = ((c.dbw >> 1u) != 0u) ? (c.dbw >> 1u) : 1u;

        if (dpsm == GS_PSM_T4)
        {
            uint32_t offset = 0u;
            while (offset < c.sizeBytes && *c.hwY < c.rrh)
            {
                // The legacy loop consumed ONE source byte per iteration and emitted its low
                // nibble, then its high nibble only when `xBefore + 1 < rrw`. So a row consumes
                // ceil(rowLeft / 2) bytes and the byte that lands the low nibble on the final
                // column discards its high nibble (rows are byte-aligned for odd rrw).
                const uint32_t rowLeft = c.rrw - *c.hwX;
                const uint32_t bytesForRow = (rowLeft + 1u) >> 1u;
                const uint32_t bytesAvail = c.sizeBytes - offset;
                const uint32_t bytesUse = (bytesForRow < bytesAvail) ? bytesForRow : bytesAvail;
                if (bytesUse == 0u)
                    break;
                uint32_t pixels = bytesUse * 2u;
                if (pixels > rowLeft)
                    pixels = rowLeft;

                GSMem::WriteRunP4(c.vram, c.dbp, pageBwHalf, c.dsax + *c.hwX, c.dsay + *c.hwY,
                                  c.data + offset, 0u, pixels);

                offset += bytesUse;
                *c.hwX += pixels;
                if (*c.hwX >= c.rrw)
                {
                    *c.hwX = 0u;
                    ++(*c.hwY);
                }
            }
            return true;
        }

        if (dpsm == GS_PSM_T8)
        {
            uint32_t offset = 0u;
            while (offset < c.sizeBytes && *c.hwY < c.rrh)
            {
                const uint32_t pixelsLeft = c.rrw - *c.hwX;
                const uint32_t bytesAvail = c.sizeBytes - offset;
                const uint32_t pixelsToCopy = (pixelsLeft < bytesAvail) ? pixelsLeft : bytesAvail;
                if (pixelsToCopy == 0u)
                    break;

                GSMem::WriteRunP8(c.vram, c.dbp, pageBwHalf, c.dsax + *c.hwX, c.dsay + *c.hwY,
                                  c.data + offset, pixelsToCopy);

                offset += pixelsToCopy;
                *c.hwX += pixelsToCopy;
                if (*c.hwX >= c.rrw)
                {
                    *c.hwX = 0u;
                    ++(*c.hwY);
                }
            }
            return true;
        }

        if (dpsm == GS_PSM_CT24 || dpsm == GS_PSM_Z24)
        {
            uint32_t offset = 0u;
            while (offset < c.sizeBytes && *c.hwY < c.rrh)
            {
                const uint32_t pixelsLeft = c.rrw - *c.hwX;
                const uint32_t bytesAvail = c.sizeBytes - offset;
                uint32_t pixelsToCopy = pixelsLeft;
                if (pixelsLeft * 3u > bytesAvail)
                    pixelsToCopy = bytesAvail / 3u;
                if (pixelsToCopy == 0u)
                    break;

                GSMem::WriteRunCT24(c.vram, c.dbp, c.dbw, c.dsax + *c.hwX, c.dsay + *c.hwY,
                                    c.data + offset, pixelsToCopy);

                offset += pixelsToCopy * 3u;
                *c.hwX += pixelsToCopy;
                if (*c.hwX >= c.rrw)
                {
                    *c.hwX = 0u;
                    ++(*c.hwY);
                }
            }
            return true;
        }

        uint32_t bytesPerPixel = 0u;
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
            bytesPerPixel = 4u;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            bytesPerPixel = 2u;
            break;
        case GS_PSM_T8H:
            bytesPerPixel = 1u;
            break;
        default:
            return false; // T4HL/T4HH and unknown psm keep the legacy path
        }

        uint32_t offset = 0u;
        while (offset < c.sizeBytes && *c.hwY < c.rrh)
        {
            const uint32_t pixelsLeft = c.rrw - *c.hwX;
            const uint32_t bytesAvail = c.sizeBytes - offset;
            uint32_t bytesLeft = pixelsLeft * bytesPerPixel;
            if (bytesLeft > bytesAvail)
                bytesLeft = (bytesAvail / bytesPerPixel) * bytesPerPixel;
            const uint32_t pixelsCopied = bytesLeft / bytesPerPixel;

            if (pixelsCopied != 0u)
            {
                const uint32_t vx = c.dsax + *c.hwX;
                const uint32_t vy = c.dsay + *c.hwY;
                const uint8_t *src = c.data + offset;
                switch (dpsm)
                {
                case GS_PSM_CT32:
                    GSMem::WriteRunCT32(c.vram, c.dbp, c.dbw, vx, vy, src, pixelsCopied);
                    break;
                case GS_PSM_Z32:
                    GSMem::WriteRunZ32(c.vram, c.dbp, c.dbw, vx, vy, src, pixelsCopied);
                    break;
                case GS_PSM_CT16:
                    GSMem::WriteRunCT16(c.vram, c.dbp, c.dbw, vx, vy, src, pixelsCopied);
                    break;
                case GS_PSM_CT16S:
                    GSMem::WriteRunCT16S(c.vram, c.dbp, c.dbw, vx, vy, src, pixelsCopied);
                    break;
                case GS_PSM_Z16:
                    GSMem::WriteRunZ16(c.vram, c.dbp, c.dbw, vx, vy, src, pixelsCopied);
                    break;
                case GS_PSM_Z16S:
                    GSMem::WriteRunZ16S(c.vram, c.dbp, c.dbw, vx, vy, src, pixelsCopied);
                    break;
                case GS_PSM_T8H:
                    GSMem::WriteRunP8H(c.vram, c.dbp, c.dbw, vx, vy, src, pixelsCopied);
                    break;
                default:
                    break;
                }
            }

            // The legacy loop advanced the cursor by `pixelsCopied` and consumed `bytesLeft` even
            // when the remainder held fewer than one whole pixel; a zero-length step would spin, so
            // it is the loop's terminator here exactly as `bytesLeft == 0 -> break` was there.
            offset += bytesLeft;
            *c.hwX += pixelsCopied;
            if (*c.hwX >= c.rrw)
            {
                *c.hwX = 0u;
                ++(*c.hwY);
            }
            if (bytesLeft == 0u)
                break;
        }
        return true;
    }

    // ---- DC2_G424_VERIFY oracle -------------------------------------------------------------
    // Replays one sampled upload on both paths from an identical pre-state and compares ALL of GS
    // memory plus the transfer cursor. Cost is confined to verify runs (three 4 MiB copies per
    // sampled call), so DC2_G424_VERIFY_EVERY defaults to one call in 64.

    thread_local std::vector<uint8_t> s_g424VerBefore;
    thread_local std::vector<uint8_t> s_g424VerFast;
    thread_local uint32_t s_g424VerX = 0u;      // cursor before either arm ran
    thread_local uint32_t s_g424VerY = 0u;
    thread_local uint32_t s_g424VerFastX = 0u;  // cursor the fast arm produced
    thread_local uint32_t s_g424VerFastY = 0u;
    thread_local bool s_g424VerPending = false;
    std::atomic<uint64_t> g_g424VerOk{0ull};
    std::atomic<uint64_t> g_g424VerBad{0ull};
    std::atomic<uint64_t> g_g424Calls{0ull};
    std::atomic<uint64_t> g_g424Fast{0ull};

    bool g424VerifySampleNow()
    {
        static thread_local uint32_t s_n = 0u;
        return (s_n++ % g424VerifyEvery()) == 0u;
    }

    void g424VerifyBegin(const uint8_t *vram, uint32_t vramSize, uint32_t hwX, uint32_t hwY)
    {
        s_g424VerBefore.assign(vram, vram + vramSize);
        s_g424VerX = hwX;
        s_g424VerY = hwY;
        s_g424VerPending = true;
    }

    // Captures the fast result, then rewinds VRAM and the cursor so the legacy tree runs from the
    // identical pre-state.
    void g424VerifyRewind(uint8_t *vram, uint32_t vramSize, uint32_t &hwX, uint32_t &hwY)
    {
        s_g424VerFast.assign(vram, vram + vramSize);
        s_g424VerFastX = hwX;
        s_g424VerFastY = hwY;
        std::memcpy(vram, s_g424VerBefore.data(), vramSize);
        hwX = s_g424VerX;
        hwY = s_g424VerY;
    }

    void g424VerifyCompare(const uint8_t *vram, uint32_t vramSize, uint32_t hwX, uint32_t hwY,
                           uint32_t dpsm, uint32_t dbp, uint32_t dbw,
                           uint32_t rrw, uint32_t rrh, uint32_t sizeBytes)
    {
        s_g424VerPending = false;
        const bool badMem = std::memcmp(vram, s_g424VerFast.data(), vramSize) != 0;
        const bool badCursor = (hwX != s_g424VerFastX) || (hwY != s_g424VerFastY);
        if (badMem || badCursor)
        {
            g_g424VerBad.fetch_add(1ull, std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[G424:verify] MISMATCH mem=%d cursor=%d psm=0x%x dbp=0x%x dbw=%u "
                         "rr=%ux%u bytes=%u fast=(%u,%u) legacy=(%u,%u)\n",
                         badMem ? 1 : 0, badCursor ? 1 : 0, dpsm, dbp, dbw, rrw, rrh, sizeBytes,
                         s_g424VerFastX, s_g424VerFastY, hwX, hwY);
            std::fflush(stderr);
            std::abort();
        }
        const uint64_t ok = g_g424VerOk.fetch_add(1ull, std::memory_order_relaxed) + 1ull;
        if ((ok % 1000ull) == 0ull)
            std::fprintf(stderr, "[G424:verify] ok=%llu bad=%llu\n",
                         (unsigned long long)ok,
                         (unsigned long long)g_g424VerBad.load(std::memory_order_relaxed));
    }
}
