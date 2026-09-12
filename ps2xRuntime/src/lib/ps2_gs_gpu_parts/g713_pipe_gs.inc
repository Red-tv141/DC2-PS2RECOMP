// ==============================================================================================
// G713 — PIPELINE, the GS TRANSFER/IMAGE producer's state and its post helpers.
//
// Included by ps2_gs_gpu.cpp BEFORE gpu_gif_and_registers.inc, which holds the four producer sites
// (the resolved `TRXDIR` seam and the three IMAGE calls) and the
// one-shot hook installation.
//
// ⚠️ WHY THE EXEC BODIES ARE NOT IN THIS FILE. Every member the transfer path touches — `m_vram`,
// `m_bitbltbuf`, `m_trxpos`, `m_trxreg`, `writeRegister`, `processImageData`,
// `performLocalToHostToBuffer`, `m_lastDisplayBaseBytes` — is PRIVATE, and `ps2_gs_gpu.h` is frozen
// (it is pulled into the 30,000-TU generated target, so adding a friend or a member would cost a
// 30-hour rebuild). The exec bodies are therefore CAPTURELESS LAMBDAS declared inside
// `GS::processGIFPacket`, which is a member and so has the access; a captureless lambda converts to
// a plain function pointer, which is exactly what the ring stores. Nothing here touches a GS.
//
// G721: nodes carry decoded transfer fields from the parser. The executor applies
// them to a private transfer context, without replaying register side effects.
// Existing transfer kernels share the real VRAM and retain private m_hwregX/Y
// across chunks; context construction cannot reset the parser's GIF continuation.
//
// ⛔ LOCAL->HOST DOES NOT MOVE. `performLocalToHostToBuffer` fills `m_localToHostBuffer`, which the
// EE reads back through the real GS object — a genuinely CPU-visible result. It takes the blocking
// bypass instead, and is recorded as this path's one unavoidable serialisation edge.
// ==============================================================================================

#include "../ps2_g713_pipeline_api.inc"

namespace
{

// The exec thread's scratch GS, and the real one. Both are set from inside `GS::processGIFPacket`.
GS *g_g713ExecGs = nullptr;
GS *g_g713RealGs = nullptr;

// The parse thread's live snapshot of the four transfer registers. `g_g713SnapReset` is consumed by
// the first IMAGE chunk after a TRXDIR, so the cursor reset happens exactly once per transfer.
G713XferSnap g_g713Snap{};
bool g_g713SnapReset = false;

// Executor-only application of the immutable decoded command, installed by GS.
GS *(*g_g721PrepareTransfer)(const G713XferSnap *) = nullptr;

// Installed once from inside `GS::processGIFPacket` (see the header note).
void (*g_g713L2hThunk)(void *) = nullptr;

// ---------------------------------------------------------------------------------------------
// PARSE: post one host->local IMAGE chunk. Takes plain integers so it needs no GS access; the
// three call sites are all inside GS members and pass their own registers.
//
// The ordering obligation is the g260 upload edge: entries already captured against pages this
// upload overwrites must execute BEFORE it. The shipped path answers that inside
// `g144FlushPendingUploadRange` by draining the whole graph; here every CLOSED batch is already
// ahead in the ring, so only the OPEN batch can be out of order — and only when it truly conflicts.
// ---------------------------------------------------------------------------------------------
// ⚠️ `[G713:dbg]` (DC2_G713_DBG=1, default OFF). The first SERIAL bring-up derailed the GIF parse
// permanently at the first real IMAGE transfer — `[G147:gif] tags=0 packedRegs=0 draw=0.00x` from
// n≈331 onward — and no amount of reading the diff explained it. This prints the first N posts with
// the snapshot the exec side will actually apply, so a zero/stale BITBLTBUF or a missing
// `resetProgress` names itself instead of being inferred.
bool g713DbgOn()
{
    static const bool on = [] {
        const char *v = std::getenv("DC2_G713_DBG");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    return on;
}
uint64_t g_g713DbgN = 0;

void g713PostImageFromGs(uint32_t dbp, uint32_t dbw, uint32_t dpsm,
                         uint32_t dsax, uint32_t dsay, uint32_t rrw, uint32_t rrh,
                         const uint8_t *data, uint32_t bytes)
{
    // ⛔⛔⛔ A ZERO-BYTE POST MUST NOT CONSUME THE CURSOR RESET.
    //
    // This one line cost the whole first SERIAL bring-up. `g713PostImage` returns early when
    // `bytes == 0`, but the flag was cleared unconditionally below — so the FIRST transfer's
    // `resetProgress` was eaten by a 0-byte call and every later chunk carried `reset=0`. The
    // scratch GS therefore never received BITBLTBUF/TRXPOS/TRXREG at all: its `m_trxreg` stayed
    // {0,0}, `processImageData` wrote nothing, no texture or CLUT ever reached guest VRAM, and the
    // guest's GIF stream degenerated into an endless image continuation
    // (`[G147:gif] tags=0 packedRegs=0 draw=0.00x` from n≈331). `[G713:dbg]` named it in one run:
    //   img n=1 bytes=0 reset=1 ... | img n=2 bytes=1024 reset=0 ...
    //
    // ⭐ The rule this buys: a flag that arms a piece of STATE for the next consumer may only be
    // cleared on the path that actually reaches that consumer.
    if (bytes == 0u || data == nullptr)
        return;
    if (g713RasterCloseForUpload(dbp, dbw, dpsm, dsax, dsay, rrw, rrh))
        g713RasterFlushOpen();
    // Capture current destination even when registers changed without TRXDIR.
    g_g713Snap.dbp = dbp; g_g713Snap.dbw = dbw; g_g713Snap.dpsm = dpsm;
    g_g713Snap.dsax = dsax; g_g713Snap.dsay = dsay;
    g_g713Snap.rrw = rrw; g_g713Snap.rrh = rrh;
    g_g713Snap.resetProgress = g_g713SnapReset ? 1u : 0u;
    if (g713DbgOn() && ++g_g713DbgN <= 24u)
    {
        std::fprintf(stderr,
                     "[G713:dbg] img n=%llu bytes=%u reset=%u | live(dbp=0x%x dbw=%u dpsm=0x%x "
                     "ds=%u,%u rr=%u,%u) | resolved(trxdir=%u)\n",
                     static_cast<unsigned long long>(g_g713DbgN), bytes, g_g713Snap.resetProgress,
                     dbp, dbw, dpsm, dsax, dsay, rrw, rrh, g_g713Snap.trxdir);
        std::fflush(stderr);
    }
    g713PostImage(g_g713Snap, data, bytes);
    g_g713SnapReset = false;
}

// ==============================================================================================
// G713 — THE LOCAL→LOCAL DIRECTION: its census and its ceiling probe.
//
// WHY, IN ONE PARAGRAPH. `GS::performLocalToLocalTransfer` already calls `g289TryGpuLocalCopy`
// first and only falls to the CPU copy `if (!g289GpuCopy)` — this tree HAS PCSX2's `Move()` fast
// path and it is default-ON. But two things stop it delivering PCSX2's benefit:
//
//   1. Its admission is ONE HARDCODED SHAPE (`g289CanDeferLocalCopy`): source must be the pending
//      display readback, destination must be literally 0x13b or 0x14a, rect exactly 512x416 at the
//      origin. Everything else escapes to the CPU copy.
//   2. ⭐ EVEN WHEN IT IS TAKEN, the source has ALREADY been materialised: the range barrier and
//      `g242PrepareVramReadRect(m_vram, sbp, ...)` run BEFORE the GPU attempt, unconditionally.
//      PCSX2's whole point is "no need to update local memory" — this path still updates it.
//
// So the direction's real prize is the CPU copy PLUS the source materialisation it forces, and the
// probe below prices them TOGETHER, because a widened admission would have to remove both to be
// worth anything. Pricing before building is G629's law, and G711 §3.2's 5-20x discount is exactly
// what happens to anyone who skips it.
//
//   DC2_G713_L2L_STAT=1   `[G713:l2l]` — calls, GPU taken, escaped, and the escape SHAPE
//                         histogram (psm pair / rect class / destination), so a widening can be
//                         designed from the census rather than from one observed transfer.
//   DC2_G713_L2L_CEIL=1   ⛔⛔⛔ KNOWINGLY WRONG OUTPUT. Skips the range barrier, the source
//                         materialisation and the CPU copy for EVERY local→local transfer, while
//                         still bumping the destination's write generations so the publication
//                         bookkeeping both arms do is identical. Measurement only, never promote.
// ==============================================================================================

bool g713L2lStatOn()
{
    static const bool on = [] {
        const char *v = std::getenv("DC2_G713_L2L_STAT");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    return on;
}

bool g713L2lCeilOn()
{
    static const bool on = [] {
        const char *v = std::getenv("DC2_G713_L2L_CEIL");
        return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
    }();
    return on;
}

struct G713L2lStat
{
    uint64_t calls = 0, gpu = 0, esc = 0;
    uint64_t escPx = 0;             // escaped pixels — the term that scales with the CPU copy
    uint64_t sameCt32 = 0;          // escaped and spsm == dpsm == CT32 (the widenable shape)
    uint64_t samePsmOther = 0;      // escaped, formats equal, not CT32
    uint64_t mixedPsm = 0;          // escaped, formats differ (needs conversion, not a blit)
    uint64_t bigRect = 0;           // escaped, >= 64x64 (worth a GPU dispatch at all)
    uint64_t dstHist[8] = {};       // escaped destinations, by dbp>>5 bucketed into 8 lanes
};
G713L2lStat g_g713L2l;

void g713NoteL2l(bool gpuTaken, uint32_t sbp, uint32_t spsm, uint32_t dbp, uint32_t dpsm,
                 uint32_t rrw, uint32_t rrh)
{
    (void)sbp;
    G713L2lStat &s = g_g713L2l;
    ++s.calls;
    if (gpuTaken)
    {
        ++s.gpu;
    }
    else
    {
        ++s.esc;
        s.escPx += static_cast<uint64_t>(rrw) * rrh;
        if (spsm == dpsm)
        {
            if (spsm == GS_PSM_CT32)
                ++s.sameCt32;
            else
                ++s.samePsmOther;
        }
        else
        {
            ++s.mixedPsm;
        }
        if (rrw >= 64u && rrh >= 64u)
            ++s.bigRect;
        s.dstHist[(dbp >> 5u) & 7u]++;
    }
    if ((s.calls % 20000ull) != 0ull)
        return;
    const double c = static_cast<double>(s.calls);
    std::fprintf(stderr,
                 "[G713:l2l] calls=%llu gpu=%llu (%.2f%%) esc=%llu (%.2f%%) escMpx=%.1f | "
                 "escShape(sameCT32=%.1f%% samePsmOther=%.1f%% mixedPsm=%.1f%% big64=%.1f%%) | "
                 "dst=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
                 (unsigned long long)s.calls, (unsigned long long)s.gpu, 100.0 * s.gpu / c,
                 (unsigned long long)s.esc, 100.0 * s.esc / c,
                 static_cast<double>(s.escPx) / 1.0e6,
                 s.esc ? 100.0 * s.sameCt32 / (double)s.esc : 0.0,
                 s.esc ? 100.0 * s.samePsmOther / (double)s.esc : 0.0,
                 s.esc ? 100.0 * s.mixedPsm / (double)s.esc : 0.0,
                 s.esc ? 100.0 * s.bigRect / (double)s.esc : 0.0,
                 (unsigned long long)s.dstHist[0], (unsigned long long)s.dstHist[1],
                 (unsigned long long)s.dstHist[2], (unsigned long long)s.dstHist[3],
                 (unsigned long long)s.dstHist[4], (unsigned long long)s.dstHist[5],
                 (unsigned long long)s.dstHist[6], (unsigned long long)s.dstHist[7]);
    std::fflush(stderr);
}

} // namespace
