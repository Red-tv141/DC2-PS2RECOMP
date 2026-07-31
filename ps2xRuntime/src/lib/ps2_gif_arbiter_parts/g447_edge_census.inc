// ============================================================================
// G447 — GS-worker BLOCKING-EDGE census (default OFF, DC2_G447_EDGE=1).
//
// PREMISE. G446's host PC sampler proved that 51-61% of the GS worker's
// window-processing span is a KERNEL WAIT (~19-21 ms/f on a ~37 ms frame), and
// that the whole named exe content of the pole thread is only ~15 ms/f spread
// over a dozen 1-4% rows. A PC sampler names the wait PRIMITIVE
// (NtWaitForAlertByThreadId 47.4%) but not the EDGE that entered it. This
// census puts a wall clock on every edge at which the GS worker can block, so
// the residue is either named or proven to be somewhere else.
//
// Edges (all timed on the GS worker thread ONLY — the EE thread reaches several
// of the same functions at the frame barrier and must not be pooled in):
//   0 backend-render     G178Backend::submitImpl fut.get()  (renderBatch / depth)
//   1 backend-readcolor  readColor fut.get()                (blocking glReadPixels)
//   2 backend-writecolor writeColor fut.get()               (upload)
//   3 backend-copy       copyColor / copyColorEnsureDest    (GPU->GPU alias copy)
//   4 backend-other      T8 view / CT24 view / composites
//   5 vu1-collect        g297CollectWindowPackets           (MTVU Path1 merge)
//   6 arbiter-relock     lk.lock() re-acquire after a window
//
// The report is emitted from the frame-boundary branch every 30 boundaries and
// is deliberately expressed against the SAME window-span accumulator the rest
// of the arc uses (g_g151WorkerBusyNs), so `cpu` below is exactly
//   window span - (sum of the timed blocking edges inside that span).
//
// Self-cost: two steady_clock::now() per blocking edge. There are ~74 backend
// items + ~1560 windows per frame, i.e. ~1.6 k timed edges/f — the same order as
// [G316:worker], which the arc already treats as admissible for attribution but
// NOT for an absolute frame number. Never quote a frame time taken with this on.
// ============================================================================

namespace
{
    bool g447EdgeOnImpl()
    {
        static const bool on = (std::getenv("DC2_G447_EDGE") != nullptr);
        return on;
    }

    constexpr int kG447Edges = 7;
    const char *g447EdgeName(int e)
    {
        switch (e)
        {
        case 0: return "backend-render";
        case 1: return "backend-readcolor";
        case 2: return "backend-writecolor";
        case 3: return "backend-copy";
        case 4: return "backend-other";
        case 5: return "vu1-collect";
        case 6: return "arbiter-relock";
        default: return "?";
        }
    }

    // GS-worker-thread private (no atomics: only that thread writes, and only the
    // boundary branch — which runs on the same thread — reads).
    thread_local bool t_g447IsGsWorker = false;
    uint64_t g_g447Ns[kG447Edges] = {};
    uint64_t g_g447N[kG447Edges] = {};
    uint64_t g_g447LastNs[kG447Edges] = {};
    uint64_t g_g447LastN[kG447Edges] = {};
    uint64_t g_g447Boundaries = 0ull;
    uint64_t g_g447LastWindowNs = 0ull;
} // namespace

// --- cross-TU surface (global linkage; the GL backend lives in another TU whose
// --- parts are compiled inside an anonymous namespace — see the G445 lesson).
bool g447EdgeOn() { return g447EdgeOnImpl(); }
bool g447OnGsWorker() { return t_g447IsGsWorker; }
void g447MarkGsWorkerThread() { t_g447IsGsWorker = true; }

// G447 HOST SHAPE. Both promoted G447 spins buy wall time by burning CPU on a thread that is
// otherwise blocked, so their verdict is a property of the MACHINE, not only of the code: free where
// there are spare cores, potentially a regression where there are not. This reports the shape so a
// result from an unfamiliar host can be read correctly. Physical-core count comes from
// GetLogicalProcessorInformation (RelationProcessorCore); on failure physical is reported as 0.
void g447HostShape(int *logicalOut, int *physicalOut)
{
    static int s_logical = 0, s_physical = 0;
    if (s_logical == 0)
    {
        s_logical = static_cast<int>(std::thread::hardware_concurrency());
        DWORD len = 0;
        GetLogicalProcessorInformation(nullptr, &len);
        if (len != 0 && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buf(
                len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
            if (GetLogicalProcessorInformation(buf.data(), &len))
                for (const auto &e : buf)
                    if (e.Relationship == RelationProcessorCore)
                        ++s_physical;
        }
    }
    if (logicalOut != nullptr)
        *logicalOut = s_logical;
    if (physicalOut != nullptr)
        *physicalOut = s_physical;
}

void g447NoteWait(int edge, unsigned long long ns)
{
    if (edge < 0 || edge >= kG447Edges)
        return;
    g_g447Ns[edge] += static_cast<uint64_t>(ns);
    ++g_g447N[edge];
}

// Called from the GS worker's frame-boundary branch. windowNs is the running
// g_g151WorkerBusyNs total (window-branch wall time only), so the delta over the
// report period is the span the G446 sampler cross-tabbed as "window-popped".
void g447ReportBoundary(unsigned long long windowNsTotal)
{
    if (!g447EdgeOnImpl())
        return;
    if ((++g_g447Boundaries % 30ull) != 0ull)
        return;
    const double f = 30.0;
    const double windowMs =
        static_cast<double>(windowNsTotal - g_g447LastWindowNs) / 1.0e6 / f;
    double blockedMs = 0.0;
    char line[512];
    int off = std::snprintf(line, sizeof(line), "[G447:edge] frames=30 windowMs/f=%.2f |", windowMs);
    for (int e = 0; e < kG447Edges; ++e)
    {
        const double ms = static_cast<double>(g_g447Ns[e] - g_g447LastNs[e]) / 1.0e6 / f;
        const double n = static_cast<double>(g_g447N[e] - g_g447LastN[e]) / f;
        // Edges 0..4 and 5 sit inside the window span; 6 is the tail re-lock, also
        // inside it (it is taken before m_busy is cleared).
        blockedMs += ms;
        if (off < static_cast<int>(sizeof(line)) - 1)
            off += std::snprintf(line + off, sizeof(line) - off, " %s=%.2f/%.1f",
                                 g447EdgeName(e), ms, n);
        g_g447LastNs[e] = g_g447Ns[e];
        g_g447LastN[e] = g_g447N[e];
    }
    g_g447LastWindowNs = windowNsTotal;
    std::fprintf(stderr, "%s | blocked=%.2f cpu=%.2f (%.0f%% blocked)\n",
                 line, blockedMs, windowMs - blockedMs,
                 windowMs > 0.0 ? 100.0 * blockedMs / windowMs : 0.0);
}
