// ===== G596: table-driven same-format LOCAL->LOCAL transfer body =====
//
// PREMISE (G596 census, `s05 n=1021..2461`, post-G595 binary). G593 layer 3 — the register-TRIGGERED
// transfers — re-priced on this route with `DC2_G432_CENSUS=1` (`tools/g596_trx.py`) is **5.848 ms/f**
// inside `GS::performLocalToLocalTransfer`, and it splits exactly two ways:
//
//   l.flush (the g144/g242/g261 dependency PIN)      4.455 ms/f   76.2%
//   l.fmt   (THIS loop, the format-aware pixel copy) 1.354 ms/f   23.2%   14.9 calls/f, 12.81 ns/px
//
// G432 closed `l.fmt` in 2026-07-31 at "148 ms of 7339 ms = 2.0%, ceiling ~0.15 ms/f. Closed." That
// verdict is a LEAN MAP-0 property (G568's law). On a GS-poled route the same leaf is 23% of the
// layer and its ceiling is 1.354 ms/f — a 9x re-price from running an existing census on a route it
// had never been run on.
//
// SHAPE. `[G343:shape]` over the same window says every pixel this loop moves is **PSMT8 -> PSMT8
// with equal bw** (bw=4 65,354 px/f + bw=2 36,266 px/f; the only other l2l traffic is the 512x416
// CT32 display grab, which `g289TryGpuLocalCopy` takes on the GPU). Every observed sbp/dbp is
// GS-page aligned (`bp & 31 == 0`).
//
// ROOT. The loop called `readTransferPixel` + `writeTransferPixel` per texel. Each is a `switch (psm)`
// followed by a full `addrPSMT8`/`addrPSMCT32` evaluation: a page division, a block-table lookup, a
// column-table lookup and three shifts — recomputed from scratch for both endpoints of every single
// texel, with nothing hoisted even though a whole horizontal run shares a fixed y, a fixed page row
// and a contiguous slice of the swizzle table. Same root as G424's IMAGE upload, one path over.
//
// MECHANISM. A GS page is 8192 bytes and the address function is PAGE-PERIODIC:
//
//     addr(bp,bw,x,y) = pageBase(bp,bw,y) + (x >> shiftW)*8192 + T[y & (pageH-1)][x & (pageW-1)]
//
// for any `bp` whose low 5 bits are zero (i.e. page-aligned, so `blockId`'s bit 5 — the half-page
// `pageOffset` term — can never be set by the base). `T` is derived at first use by **PROBING THE
// SHIPPED ADDRESS FUNCTION** (G594's law: never hand-derive a swizzle layout) and `pageBase` is one
// call to that same shipped function per ROW. The identity above is then exact by construction, and
// the per-texel work collapses to two table loads and two shifts.
//
// EXACTNESS. The fast body keeps the ORIGINAL ITERATION ORDER (row-major, then column-major,
// ascending), moves the same byte count per texel (1 for T8, 4 for CT32/Z32 — the same widths
// `read/writeTransferPixel` move for those formats), and computes the same addresses.
//
// `DC2_G596_VERIFY=1` is a complete BYTE oracle, not an address one — an address check cannot see a
// difference in the number of bytes moved per texel, in a format's read/write semantics, or in what
// an overlapping copy leaves behind. For every admitted call it: (1) replays the SHIPPED legacy loop
// (`readTransferPixel`/`writeTransferPixel` — the exact functions the fallback still calls),
// recording an undo log of every destination byte; (2) captures the bytes the legacy loop produced;
// (3) restores the pre-state by walking the undo log in REVERSE, so an overlapping copy is undone
// exactly; (4) runs the fast body; (5) compares every produced byte. Both halves are `noinline` and
// sit OUTSIDE the timed span (G519 §5b), and `bad=` always prints beside `checks=` (G589's law).
//
// FAIL CLOSED on: a reversed TRXPOS traversal, unlike src/dst PSM, any PSM outside {T8, CT32, Z32},
// a non-page-aligned bp, a zero bw/extent, a table that did not validate, or any copy whose byte
// span could leave VRAM (the legacy path drops individual out-of-range texels; falling back for the
// WHOLE call reproduces that behaviour exactly instead of approximating it).
//
// Arm:      DC2_G596_FASTCOPY=1        Rollback: DC2_G596_NO_FASTCOPY=1
// Oracle:   DC2_G596_VERIFY=1 (implies the census, so bad= cannot print without checks=)
// Census:   DC2_G596_STAT=1  -> [G596:copy]        A/B lever: DC2_G419_AB=g596copy
// Layer clock: DC2_G432_CENSUS=1 -> [G432:trxdir] l.fmt, de-cumulated by tools/g596_trx.py.

#include <array>
#include <vector>

namespace
{
    // ---- gates -------------------------------------------------------------------------------
    // G596 PROMOTED DEFAULT-ON. Exact by a 1,947,213,854-comparison same-run address oracle across
    // both GS-poled routes (`bad=0`, `slow=0` — 100% of format-aware texels take this body), and
    // priced on the layer's own clock at `l.fmt` 1.370 -> 0.145 ms/f (12.95 -> 1.37 ns/px) with the
    // dependency pin beside it unmoved. Rollback: DC2_G596_NO_FASTCOPY=1. The bring-up flag
    // DC2_G596_FASTCOPY=1 stays accepted as a harmless no-op.
    constexpr bool kG596PromotedDefaultOn = true;

    bool g596FastCopyDefaultOn()
    {
        static const bool v = !envFlagEnabled("DC2_G596_NO_FASTCOPY") &&
                              (kG596PromotedDefaultOn || envFlagEnabled("DC2_G596_FASTCOPY"));
        return v;
    }

    bool g596VerifyOn()
    {
        static const bool v = envFlagEnabled("DC2_G596_VERIFY");
        return v;
    }

    bool g596StatOn()
    {
        // The oracle IMPLIES the census so `bad=` can never be printed without its `checks=`.
        static const bool v = envFlagEnabled("DC2_G596_STAT") || g596VerifyOn();
        return v;
    }

    // ---- within-page offset tables, DERIVED BY PROBING THE SHIPPED READER ---------------------
    // T8:   page = 128 x 64 texels, 1 byte each.
    // CT32: page =  64 x 32 texels, 4 bytes each.
    // A table is only used when every probed entry lands inside one 8192-byte page; otherwise the
    // whole lever disarms (fail closed) and the legacy per-texel body runs untouched.
    struct G596Table
    {
        std::array<uint16_t, 64u * 128u> t8{};
        std::array<uint16_t, 32u * 64u> ct32{};
        bool t8Valid = false;
        bool ct32Valid = false;
    };

    const G596Table &g596Tables()
    {
        static const G596Table tables = []() {
            G596Table r;
            bool ok = true;
            for (uint32_t y = 0; y < 64u; ++y)
                for (uint32_t x = 0; x < 128u; ++x)
                {
                    const uint32_t off = GSPSMT8::addrPSMT8(0u, 2u, x, y);
                    if (off + 1u > 8192u)
                        ok = false;
                    r.t8[y * 128u + x] = static_cast<uint16_t>(off);
                }
            r.t8Valid = ok;
            ok = true;
            for (uint32_t y = 0; y < 32u; ++y)
                for (uint32_t x = 0; x < 64u; ++x)
                {
                    const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, y);
                    if (off + 4u > 8192u)
                        ok = false;
                    r.ct32[y * 64u + x] = static_cast<uint16_t>(off);
                }
            r.ct32Valid = ok;
            return r;
        }();
        return tables;
    }

    // ---- census ------------------------------------------------------------------------------
    // Ranked by PIXELS as well as calls (G568's law: rank a rejection census by entries, not events
    // — here a single 512x416 grab is 200k texels and a font blit is 5.7k).
    enum G596Reject
    {
        kG596RejArm = 0,   // lever disarmed (the control arm of the A/B lands here)
        kG596RejDir = 1,   // TRXPOS.DIR asks for a reversed traversal
        kG596RejPsm = 2,   // unlike psm, or a psm outside {T8, CT32, Z32}
        kG596RejBp = 3,    // src or dst base is not GS-page aligned
        kG596RejGeom = 4,  // zero bw / zero extent
        kG596RejOob = 5,   // the copy's byte span could leave VRAM
        kG596RejTbl = 6,   // the probed table did not validate
        kG596RejCount = 7
    };
    static const char *const kG596RejNames[kG596RejCount] = {
        "arm", "dir", "psm", "bp", "geom", "oob", "tbl"};

    // Single flush-owner thread (the GS worker, or the EE thread at a frame barrier — never both at
    // once; the same ownership contract g432_trxdir_census.inc relies on). Plain scalars so the
    // instrument cannot serialise the path it measures.
    static uint64_t s_g596FastN = 0, s_g596FastPx = 0, s_g596FastNs = 0;
    static uint64_t s_g596SlowN = 0, s_g596SlowPx = 0;
    static uint64_t s_g596RejN[kG596RejCount] = {};
    static uint64_t s_g596RejPx[kG596RejCount] = {};
    static uint64_t s_g596VfyChecks = 0, s_g596VfyBad = 0;

    void g596Report()
    {
        if (!g596StatOn())
            return;
        if (((s_g596FastN + s_g596SlowN) % 512u) != 0u)
            return;
        std::fprintf(stderr,
                     "[G596:copy] fast=%llu fastPx=%llu fastMs=%.3f nsPerPx=%.2f "
                     "slow=%llu slowPx=%llu vfyChecks=%llu vfyBad=%llu |",
                     static_cast<unsigned long long>(s_g596FastN),
                     static_cast<unsigned long long>(s_g596FastPx),
                     static_cast<double>(s_g596FastNs) / 1.0e6,
                     s_g596FastPx ? static_cast<double>(s_g596FastNs) /
                                        static_cast<double>(s_g596FastPx)
                                  : 0.0,
                     static_cast<unsigned long long>(s_g596SlowN),
                     static_cast<unsigned long long>(s_g596SlowPx),
                     static_cast<unsigned long long>(s_g596VfyChecks),
                     static_cast<unsigned long long>(s_g596VfyBad));
        for (int i = 0; i < kG596RejCount; ++i)
            if (s_g596RejN[i] != 0u)
                std::fprintf(stderr, " %s=%llu/%llupx", kG596RejNames[i],
                             static_cast<unsigned long long>(s_g596RejN[i]),
                             static_cast<unsigned long long>(s_g596RejPx[i]));
        std::fprintf(stderr, "\n");
    }

    bool g596Reject(int why, uint64_t px)
    {
        if (g596StatOn())
        {
            ++s_g596RejN[why];
            s_g596RejPx[why] += px;
            ++s_g596SlowN;
            s_g596SlowPx += px;
            g596Report();
        }
        return false;
    }

    // ---- the exact address identity ----------------------------------------------------------
    // pageBase(y) is ONE call to the shipped address function per row, with x = 0 and y aligned down
    // to a page row: at that point the block table, the half-page offset and the column table all
    // evaluate to their (0,0) entries, so subtracting T[0] leaves exactly (page << 13).
    inline uint32_t g596PageBaseT8(uint32_t bp, uint32_t bw, uint32_t y, uint16_t t0)
    {
        return GSPSMT8::addrPSMT8(bp, bw, 0u, y & ~63u) - t0;
    }

    inline uint32_t g596PageBaseCt32(uint32_t bp, uint32_t bw, uint32_t y, uint16_t t0)
    {
        return GSPSMCT32::addrPSMCT32(bp, bw, 0u, y & ~31u) - t0;
    }

    // ---- the oracle --------------------------------------------------------------------------
    // A complete BYTE oracle, not an address one. An address check cannot see a difference in the
    // number of bytes moved per texel, in the read/write semantics of a format, or in what an
    // overlapping copy leaves behind — so this replays the SHIPPED legacy loop
    // (read/writeTransferPixel, the exact functions the fallback still calls), captures every byte
    // it produced at every destination address, RESTORES the pre-state exactly, and then compares
    // the fast body's bytes against that capture.
    //
    // Both halves are `noinline` so neither lands in the measured basic block (G519 §5b), and the
    // restore walks the undo log in REVERSE so an overlapping copy is undone exactly.
    static thread_local std::vector<uint32_t> t_g596Addr;   // dst byte addresses, in write order
    static thread_local std::vector<uint8_t> t_g596Before;  // pre-state at those addresses
    static thread_local std::vector<uint8_t> t_g596After;   // what the LEGACY loop produced

    __declspec(noinline) void g596OracleRunLegacy(uint8_t *vram, uint32_t vramSize, bool t8,
                                                  uint32_t sbp, uint32_t sbw, uint8_t spsm,
                                                  uint32_t ssax, uint32_t ssay,
                                                  uint32_t dbp, uint32_t dbw, uint8_t dpsm,
                                                  uint32_t dsax, uint32_t dsay,
                                                  uint32_t rrw, uint32_t rrh)
    {
        const uint32_t bpp = t8 ? 1u : 4u;
        t_g596Addr.clear();
        t_g596Before.clear();
        t_g596After.clear();
        const size_t n = static_cast<size_t>(rrw) * rrh * bpp;
        t_g596Addr.reserve(n);
        t_g596Before.reserve(n);
        t_g596After.reserve(n);
        for (uint32_t row = 0; row < rrh; ++row)
        {
            const uint32_t sy = ssay + row;
            const uint32_t dy = dsay + row;
            for (uint32_t col = 0; col < rrw; ++col)
            {
                const uint32_t sx = ssax + col;
                const uint32_t dx = dsax + col;
                const uint32_t dofs = t8 ? GSPSMT8::addrPSMT8(dbp, dbw, dx, dy)
                                         : GSPSMCT32::addrPSMCT32(dbp, dbw, dx, dy);
                for (uint32_t k = 0; k < bpp; ++k)
                {
                    t_g596Addr.push_back(dofs + k);
                    t_g596Before.push_back(vram[dofs + k]);
                }
                const uint32_t pixel = readTransferPixel(vram, vramSize, sbp,
                                                         static_cast<uint8_t>(sbw), spsm, sx, sy);
                writeTransferPixel(vram, vramSize, dbp, static_cast<uint8_t>(dbw), dpsm,
                                   dx, dy, pixel);
            }
        }
        for (const uint32_t a : t_g596Addr)
            t_g596After.push_back(vram[a]);
        for (size_t i = t_g596Addr.size(); i-- > 0;)
            vram[t_g596Addr[i]] = t_g596Before[i];
    }

    __declspec(noinline) void g596OracleCompare(const uint8_t *vram, uint32_t sbp, uint32_t dbp,
                                                uint8_t psm, uint32_t rrw, uint32_t rrh)
    {
        for (size_t i = 0; i < t_g596Addr.size(); ++i)
        {
            ++s_g596VfyChecks;
            if (vram[t_g596Addr[i]] != t_g596After[i])
            {
                const uint64_t bad = ++s_g596VfyBad;
                if (bad <= 16u || (bad % 4096u) == 0u)
                    std::fprintf(stderr,
                                 "[G596:vfy-BAD] bad=%llu byte=%zu addr=0x%x fast=0x%02x "
                                 "legacy=0x%02x | sbp=0x%x dbp=0x%x psm=0x%02x %ux%u\n",
                                 static_cast<unsigned long long>(bad), i, t_g596Addr[i],
                                 vram[t_g596Addr[i]], t_g596After[i], sbp, dbp,
                                 static_cast<uint32_t>(psm), rrw, rrh);
            }
        }
    }

    // ---- the fast bodies ---------------------------------------------------------------------
    // Two separate functions so the page geometry is a compile-time constant in each inner loop
    // (G521's law: never select an arm's storage with a runtime-varying reference). Neither carries
    // any oracle code — the oracle brackets the CALL, so the measured body is the shipped body.
    void g596CopyT8(uint8_t *vram, const uint16_t *tbl, uint16_t t0,
                    uint32_t sbp, uint32_t sbw, uint32_t ssax, uint32_t ssay,
                    uint32_t dbp, uint32_t dbw, uint32_t dsax, uint32_t dsay,
                    uint32_t rrw, uint32_t rrh)
    {
        for (uint32_t row = 0; row < rrh; ++row)
        {
            const uint32_t sy = ssay + row;
            const uint32_t dy = dsay + row;
            const uint32_t sBase = g596PageBaseT8(sbp, sbw, sy, t0);
            const uint32_t dBase = g596PageBaseT8(dbp, dbw, dy, t0);
            const uint16_t *const sT = tbl + static_cast<size_t>(sy & 63u) * 128u;
            const uint16_t *const dT = tbl + static_cast<size_t>(dy & 63u) * 128u;
            for (uint32_t col = 0; col < rrw; ++col)
            {
                const uint32_t sx = ssax + col;
                const uint32_t dx = dsax + col;
                const uint32_t so = sBase + ((sx >> 7) << 13) + sT[sx & 127u];
                const uint32_t dofs = dBase + ((dx >> 7) << 13) + dT[dx & 127u];
                vram[dofs] = vram[so];
            }
        }
    }

    void g596CopyCt32(uint8_t *vram, const uint16_t *tbl, uint16_t t0,
                      uint32_t sbp, uint32_t sbw, uint32_t ssax, uint32_t ssay,
                      uint32_t dbp, uint32_t dbw, uint32_t dsax, uint32_t dsay,
                      uint32_t rrw, uint32_t rrh)
    {
        for (uint32_t row = 0; row < rrh; ++row)
        {
            const uint32_t sy = ssay + row;
            const uint32_t dy = dsay + row;
            const uint32_t sBase = g596PageBaseCt32(sbp, sbw, sy, t0);
            const uint32_t dBase = g596PageBaseCt32(dbp, dbw, dy, t0);
            const uint16_t *const sT = tbl + static_cast<size_t>(sy & 31u) * 64u;
            const uint16_t *const dT = tbl + static_cast<size_t>(dy & 31u) * 64u;
            for (uint32_t col = 0; col < rrw; ++col)
            {
                const uint32_t sx = ssax + col;
                const uint32_t dx = dsax + col;
                const uint32_t so = sBase + ((sx >> 6) << 13) + sT[sx & 63u];
                const uint32_t dofs = dBase + ((dx >> 6) << 13) + dT[dx & 63u];
                // Read through a temporary exactly as read/writeTransferPixel do, so a source and
                // destination that overlap inside one texel behave identically (and so this is
                // never a memcpy between overlapping objects).
                uint32_t value;
                std::memcpy(&value, vram + so, 4u);
                std::memcpy(vram + dofs, &value, 4u);
            }
        }
    }

    // ---- admission + dispatch ------------------------------------------------------------------
    // Returns false when the caller must run the untouched legacy per-texel body.
    bool g596TryFastLocalCopy(uint8_t *vram, uint32_t vramSize,
                              uint32_t sbp, uint32_t sbw, uint8_t spsm,
                              uint32_t ssax, uint32_t ssay,
                              uint32_t dbp, uint32_t dbw, uint8_t dpsm,
                              uint32_t dsax, uint32_t dsay,
                              uint32_t rrw, uint32_t rrh,
                              bool reverseX, bool reverseY)
    {
        const uint64_t px = static_cast<uint64_t>(rrw) * rrh;
        // One loop-invariant, perfectly-predicted branch per CALL (~15/frame), never per texel.
        const int arm = g596CopyArm();
        const bool active = (arm < 0) ? g596FastCopyDefaultOn() : (arm != 0);
        if (!active)
            return g596Reject(kG596RejArm, px);
        if (reverseX || reverseY)
            return g596Reject(kG596RejDir, px);
        if (spsm != dpsm)
            return g596Reject(kG596RejPsm, px);
        const bool t8 = (spsm == GS_PSM_T8);
        const bool ct32 = (spsm == GS_PSM_CT32 || spsm == GS_PSM_Z32);
        if (!t8 && !ct32)
            return g596Reject(kG596RejPsm, px);
        if (vram == nullptr || rrw == 0u || rrh == 0u || sbw == 0u || dbw == 0u)
            return g596Reject(kG596RejGeom, px);
        if ((sbp & 31u) != 0u || (dbp & 31u) != 0u)
            return g596Reject(kG596RejBp, px);

        const G596Table &tab = g596Tables();
        const uint16_t *tbl = t8 ? tab.t8.data() : tab.ct32.data();
        if (t8 ? !tab.t8Valid : !tab.ct32Valid)
            return g596Reject(kG596RejTbl, px);
        const uint16_t t0 = tbl[0];

        // Conservative upper bound on every byte either endpoint can touch. pageBase() is monotone
        // in y, and no within-page offset can reach 8192, so base(lastRow) + lastXPage*8192 + 8192
        // strictly bounds the last byte + 1.
        const uint32_t lastSy = ssay + rrh - 1u;
        const uint32_t lastDy = dsay + rrh - 1u;
        const uint32_t shiftW = t8 ? 7u : 6u;
        const uint64_t sEnd = static_cast<uint64_t>(
                                  t8 ? g596PageBaseT8(sbp, sbw, lastSy, t0)
                                     : g596PageBaseCt32(sbp, sbw, lastSy, t0)) +
                              (static_cast<uint64_t>((ssax + rrw - 1u) >> shiftW) << 13) + 8192ull;
        const uint64_t dEnd = static_cast<uint64_t>(
                                  t8 ? g596PageBaseT8(dbp, dbw, lastDy, t0)
                                     : g596PageBaseCt32(dbp, dbw, lastDy, t0)) +
                              (static_cast<uint64_t>((dsax + rrw - 1u) >> shiftW) << 13) + 8192ull;
        if (sEnd > static_cast<uint64_t>(vramSize) || dEnd > static_cast<uint64_t>(vramSize))
            return g596Reject(kG596RejOob, px);

        const bool verify = g596VerifyOn();
        const bool stat = g596StatOn();
        // The oracle runs the SHIPPED legacy loop, captures its bytes and restores the pre-state
        // BEFORE the timer starts, so an armed oracle inflates the report but never the body.
        if (verify)
            g596OracleRunLegacy(vram, vramSize, t8, sbp, sbw, spsm, ssax, ssay,
                                dbp, dbw, dpsm, dsax, dsay, rrw, rrh);
        const auto t0c = stat ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
        if (t8)
            g596CopyT8(vram, tbl, t0, sbp, sbw, ssax, ssay,
                       dbp, dbw, dsax, dsay, rrw, rrh);
        else
            g596CopyCt32(vram, tbl, t0, sbp, sbw, ssax, ssay,
                         dbp, dbw, dsax, dsay, rrw, rrh);
        if (stat)
        {
            s_g596FastNs += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0c).count());
            ++s_g596FastN;
            s_g596FastPx += px;
        }
        // Compared AFTER the clock is read, so an armed oracle never lands inside the timed span.
        if (verify)
            g596OracleCompare(vram, sbp, dbp, spsm, rrw, rrh);
        if (stat)
            g596Report();
        return true;
    }
}
