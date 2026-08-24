// ===== G654 P16/P17: storage + reporter for the exclusive, thread-keyed layer timer ==========
//
// A COLD TU BY CONSTRUCTION (rule 12b). Everything here — the per-thread registry, the printf, the
// name tables — is out of every hot translation unit; the hot TUs see only the header's empty
// struct unless PS2X_G654_DIAG is defined, and even then only two out-of-line calls.
//
// Compiled ONLY when the option is on (see ps2xRuntime/CMakeLists.txt); the file is not in the
// default source list at all.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{

enum
{
    kLayerCount = 6
};

const char *kLayerName[kLayerCount] = {
    "VIF1", "GIFsubmit", "GSimage", "GSlocal", "GIFwalk", "drawPrim"
};

struct ThreadRow
{
    unsigned long tid = 0u;
    // Exclusive nanoseconds: time spent in this layer with no nested layer running.
    unsigned long long excl[kLayerCount] = {0};
    // Inclusive nanoseconds: the whole outermost scope, for comparison with [G146:perf].
    unsigned long long incl[kLayerCount] = {0};
    unsigned long long calls[kLayerCount] = {0};
    // How much of this layer's inclusive time was some OTHER layer nested inside it.
    unsigned long long nested[kLayerCount] = {0};
};

std::mutex g_mu;
std::vector<ThreadRow *> g_rows;

struct TlsState
{
    ThreadRow *row = nullptr;
    int depth[kLayerCount] = {0};
    // Stack of the currently open outermost layers, innermost last.
    int openStack[16] = {0};
    int openCount = 0;
};

thread_local TlsState t_state;

ThreadRow *rowForThisThread()
{
    if (t_state.row != nullptr)
        return t_state.row;
    ThreadRow *r = new ThreadRow();
#if defined(_WIN32)
    r->tid = GetCurrentThreadId();
#endif
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_rows.push_back(r);
    }
    t_state.row = r;
    return r;
}

} // namespace

bool g654LayerOn()
{
    static const bool on = std::getenv("DC2_G654_LAYER") != nullptr;
    return on;
}

// Returns true when this is the OUTERMOST scope of `layer` on this thread. Only that scope times.
void g654LayerEnter(int layer)
{
    if (layer < 0 || layer >= kLayerCount)
        return;
    TlsState &st = t_state;
    ++st.depth[layer];
    if (st.openCount < 16)
        st.openStack[st.openCount++] = layer;
}

void g654LayerExit(int layer, unsigned long long ns)
{
    if (layer < 0 || layer >= kLayerCount)
        return;
    TlsState &st = t_state;
    ThreadRow *row = rowForThisThread();
    if (st.openCount > 0)
        --st.openCount;
    if (st.depth[layer] > 0)
        --st.depth[layer];
    row->incl[layer] += ns;
    ++row->calls[layer];
    // Charge this scope's whole span to the enclosing layer's `nested` bucket, so the enclosing
    // layer's EXCLUSIVE time can be derived as incl - nested. That is the number `[G146:perf]`
    // cannot produce and the reason `fight:rain`'s residue is negative.
    if (st.openCount > 0)
    {
        const int parent = st.openStack[st.openCount - 1];
        if (parent >= 0 && parent < kLayerCount)
            row->nested[parent] += ns;
    }
}

// Called from the frame-end reporter (also #if-excluded there).
void g654LayerReport(unsigned int n, unsigned int window)
{
    if (!g654LayerOn() || window == 0u)
        return;
    std::vector<ThreadRow *> rows;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        rows = g_rows;
    }
    static std::vector<ThreadRow> s_prev;
    if (s_prev.size() < rows.size())
        s_prev.resize(rows.size());
    const double w = static_cast<double>(window);
    for (size_t i = 0; i < rows.size(); ++i)
    {
        ThreadRow cur = *rows[i];
        ThreadRow &prev = s_prev[i];
        char buf[512];
        int off = std::snprintf(buf, sizeof(buf), "[G654:layer] n=%u tid=%lu", n, cur.tid);
        bool any = false;
        for (int L = 0; L < kLayerCount; ++L)
        {
            const unsigned long long dIncl = cur.incl[L] - prev.incl[L];
            const unsigned long long dNest = cur.nested[L] - prev.nested[L];
            const unsigned long long dCall = cur.calls[L] - prev.calls[L];
            if (dIncl == 0ull && dCall == 0ull)
                continue;
            any = true;
            off += std::snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                                 " %s(incl=%.2f excl=%.2f n=%llu)", kLayerName[L],
                                 static_cast<double>(dIncl) / 1.0e6 / w,
                                 static_cast<double>(dIncl - dNest) / 1.0e6 / w,
                                 dCall / static_cast<unsigned long long>(window));
        }
        prev = cur;
        if (any)
            std::fprintf(stderr, "%s\n", buf);
    }
}
