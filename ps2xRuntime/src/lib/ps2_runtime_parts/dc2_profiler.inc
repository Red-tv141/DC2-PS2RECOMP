// dc2_profiler.inc — Unified performance profiler & telemetry for Dark Cloud 2 recompilation.
// Included from ps2_runtime.cpp. All code is file-scope static or in an anonymous namespace.
// No .h modifications (project rule: avoids 30+ hour full rebuild).
//
// Controls:
//   Compile-time: ENABLE_PROFILER (default 1). When 0, all macros compile to no-ops.
//   Runtime env vars (all read once at startup, cached):
//     DC2_PROFILE=1            — enable subsystem timing + periodic summary
//     DC2_PROFILE_OVERLAY=1    — enable on-screen overlay (also toggled by F3)
//     DC2_PROFILE_CSV=1        — enable CSV telemetry output
//     DC2_PROFILE_FUNCTIONS=1  — enable per-function profiling (high overhead!)
//     DC2_PROFILE_SLOW_MS=<ms> — slow-frame warning threshold (default 40)
//     DC2_PROFILE_WINDOW=<N>   — rolling window size (default 300)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>
#include <fstream>
#include <filesystem>
#include <ctime>
#include <iomanip>
#include <sstream>

// G652 P2 counter is implemented in ps2_runtime_cold.cpp. The generated RAII scopes around
// WaitVSync/mgGetVSyncCount feed it; the G182 reporter subtracts its delta from raw EE CPU.
extern uint64_t ps2EeWaitCpuNs();

#ifndef ENABLE_PROFILER
#define ENABLE_PROFILER 1
#endif

// ============================================================================
// Forward declarations for cross-thread timing (defined in other .inc files)
// ============================================================================
// G303: VU1 worker busy time (cumulative nanoseconds, relaxed atomic)
extern uint64_t g303_gs_worker_busy_ns();
// G332: GS worker total busy (cumulative nanoseconds, relaxed atomic)
extern uint64_t g332_gs_worker_total_ns();
// g297MtvuActive() is already visible from runtime_mtvu_and_env.inc

namespace dc2_profiler
{

// ============================================================================
// §1. Configuration — all env reads cached at first call
// ============================================================================

static bool profilerEnabled()
{
    static const bool on = [] {
        const char *v = std::getenv("DC2_PROFILE");
        return v && *v && *v != '0';
    }();
    return on;
}

static bool overlayEnabled()
{
    static const bool on = [] {
        const char *v = std::getenv("DC2_PROFILE_OVERLAY");
        return v && *v && *v != '0';
    }();
    return on;
}

static bool csvEnabled()
{
    static const bool on = [] {
        const char *v = std::getenv("DC2_PROFILE_CSV");
        return v && *v && *v != '0';
    }();
    return on;
}

static bool functionProfilingEnabled()
{
    static const bool on = [] {
        const char *v = std::getenv("DC2_PROFILE_FUNCTIONS");
        return v && *v && *v != '0';
    }();
    return on;
}

static double slowFrameThresholdMs()
{
    static const double ms = [] {
        const char *v = std::getenv("DC2_PROFILE_SLOW_MS");
        return v ? std::atof(v) : 40.0;
    }();
    return ms;
}

static uint32_t rollingWindowSize()
{
    static const uint32_t sz = [] {
        const char *v = std::getenv("DC2_PROFILE_WINDOW");
        const long val = v ? std::atol(v) : 300;
        return val > 0 ? static_cast<uint32_t>(val) : 300u;
    }();
    return sz;
}

// ============================================================================
// §2. Category identifiers
// ============================================================================

enum class Category : uint32_t
{
    Frame = 0,          // Total host frame time (main thread)
    Upload,             // UploadFrame — copy latched VRAM → GPU texture
    Present,            // BeginDrawing/EndDrawing — includes swap/VSync
    DrawHUD,            // HUD overlay rendering
    WaitFrame,          // Waiting for next guest frame latch
    Input,              // PollInputEvents + pad hook
    // Cross-thread categories (reported via atomics, not RAII scopes on main thread)
    EE_CPU,             // Recompiled EE execution (GameThread)
    VU1,                // VU1 microprogram execution (MTVU worker)
    GS,                 // GS/rasterizer (MTGS worker)
    COUNT
};

static const char *categoryName(Category c)
{
    switch (c)
    {
    case Category::Frame:     return "Frame";
    case Category::Upload:    return "Upload";
    case Category::Present:   return "Present";
    case Category::DrawHUD:   return "DrawHUD";
    case Category::WaitFrame: return "WaitFrame";
    case Category::Input:     return "Input";
    case Category::EE_CPU:    return "EE_CPU";
    case Category::VU1:       return "VU1";
    case Category::GS:        return "GS";
    default:                  return "Unknown";
    }
}

static const char *categoryDisplayName(Category c)
{
    switch (c)
    {
    case Category::Frame:     return "Frame";
    case Category::Upload:    return "Upload";
    case Category::Present:   return "Present";
    case Category::DrawHUD:   return "DrawHUD";
    case Category::WaitFrame: return "WaitFrame";
    case Category::Input:     return "Input";
    case Category::EE_CPU:    return "EE CPU";
    case Category::VU1:       return "VU1";
    case Category::GS:        return "GS";
    default:                  return "???";
    }
}

// ============================================================================
// §3. Counter identifiers
// ============================================================================

enum class Counter : uint32_t
{
    HostPresentTick = 0,
    VU1Kicks,
    GSPackets,
    RecompiledCalls,
    COUNT
};

static const char *counterName(Counter c)
{
    switch (c)
    {
    case Counter::HostPresentTick: return "host_ticks";
    case Counter::VU1Kicks:        return "vu1_kicks";
    case Counter::GSPackets:       return "gs_packets";
    case Counter::RecompiledCalls: return "recomp_calls";
    default:                       return "unknown";
    }
}

// ============================================================================
// §4. Per-frame data record
// ============================================================================

struct FrameRecord
{
    double categoryMs[static_cast<uint32_t>(Category::COUNT)] = {};
    uint64_t counterValues[static_cast<uint32_t>(Counter::COUNT)] = {};
    double frameMs = 0.0;
    double fps = 0.0;
};

// ============================================================================
// §5. Percentile computation
// ============================================================================

struct PercentileStats
{
    double avg = 0.0;
    double median = 0.0;
    double p95 = 0.0;
    double p99 = 0.0;
    double maxVal = 0.0;
    double minVal = 0.0;
};

static PercentileStats computePercentiles(const std::vector<double> &values)
{
    PercentileStats stats;
    if (values.empty())
        return stats;

    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());

    const size_t n = sorted.size();
    double sum = 0.0;
    for (double v : sorted)
        sum += v;

    stats.avg = sum / static_cast<double>(n);
    stats.minVal = sorted.front();
    stats.maxVal = sorted.back();
    stats.median = sorted[n / 2];

    auto percentile = [&](double p) -> double {
        const double idx = p * static_cast<double>(n - 1) / 100.0;
        const size_t lo = static_cast<size_t>(idx);
        const double frac = idx - static_cast<double>(lo);
        if (lo + 1 < n)
            return sorted[lo] * (1.0 - frac) + sorted[lo + 1] * frac;
        return sorted[lo];
    };

    stats.p95 = percentile(95.0);
    stats.p99 = percentile(99.0);
    return stats;
}

// ============================================================================
// §6. FunctionProfiler — optional per-recompiled-function telemetry
// ============================================================================

struct FunctionStats
{
    uint64_t callCount = 0;
    uint64_t totalNs = 0;
    uint64_t maxNs = 0;
};

class FunctionProfiler
{
public:
    static FunctionProfiler &instance()
    {
        static FunctionProfiler inst;
        return inst;
    }

    void recordCall(uint32_t pc, uint64_t elapsedNs)
    {
        auto &entry = m_stats[pc];
        ++entry.callCount;
        entry.totalNs += elapsedNs;
        if (elapsedNs > entry.maxNs)
            entry.maxNs = elapsedNs;
    }

    void loadFunctionNames(const std::string &indexPath)
    {
        uint32_t count = 0;

        // 1. Scan ref/functions/ directory
        std::error_code ec;
        if (std::filesystem::exists("ref/functions", ec))
        {
            for (const auto &entry : std::filesystem::directory_iterator("ref/functions", ec))
            {
                if (!entry.is_regular_file())
                    continue;
                const std::string filename = entry.path().filename().string();
                // Format: 0x00100190_mwInit.md
                if (filename.length() > 13 && filename.rfind("0x", 0) == 0 && filename.ends_with(".md"))
                {
                    const auto underscore = filename.find('_');
                    if (underscore != std::string::npos && underscore == 10)
                    {
                        const std::string addrStr = filename.substr(0, 10);
                        const std::string name = filename.substr(11, filename.length() - 14);
                        try
                        {
                            const uint32_t addr = static_cast<uint32_t>(std::stoul(addrStr, nullptr, 16));
                            if (!name.empty() && m_names.find(addr) == m_names.end())
                            {
                                m_names[addr] = name;
                                ++count;
                            }
                        }
                        catch (...) {}
                    }
                }
            }
        }

        // 2. Parse functions_index.json
        std::ifstream f(indexPath);
        if (f.is_open())
        {
            std::string line;
            while (std::getline(f, line))
            {
                // Pattern A: "address": "0x00100190", "name": "mwInit"
                // Pattern B: "name": "mwInit", "address": "0x00100190"
                const auto addrPos = line.find("\"address\": \"0x");
                const auto namePos = line.find("\"name\": \"");

                if (addrPos != std::string::npos && namePos != std::string::npos)
                {
                    const auto addrStart = addrPos + 12; // points to "0x..."
                    const auto addrEnd = line.find('"', addrStart);
                    const auto nameStart = namePos + 9;
                    const auto nameEnd = line.find('"', nameStart);

                    if (addrEnd != std::string::npos && nameEnd != std::string::npos)
                    {
                        const std::string addrStr = line.substr(addrStart, addrEnd - addrStart);
                        const std::string name = line.substr(nameStart, nameEnd - nameStart);
                        try
                        {
                            const uint32_t addr = static_cast<uint32_t>(std::stoul(addrStr, nullptr, 16));
                            if (!name.empty() && m_names.find(addr) == m_names.end())
                            {
                                m_names[addr] = name;
                                ++count;
                            }
                        }
                        catch (...) {}
                    }
                }
            }
        }

        std::fprintf(stderr, "[dc2_profiler] Loaded %u function names from index and ref/functions\n", count);
    }

    std::string getName(uint32_t pc) const
    {
        auto it = m_names.find(pc);
        if (it != m_names.end())
            return it->second;

        // Fallback: look for nearest preceding function base
        if (!m_names.empty())
        {
            auto upper = m_names.upper_bound(pc);
            if (upper != m_names.begin())
            {
                --upper;
                const uint32_t base = upper->first;
                const uint32_t offset = pc - base;
                if (offset < 0x4000u)
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%s+0x%X", upper->second.c_str(), offset);
                    return std::string(buf);
                }
            }
        }

        char buf[32];
        std::snprintf(buf, sizeof(buf), "Unknown_0x%08X", pc);
        return std::string(buf);
    }

    void printTopFunctions(uint32_t topN, uint64_t frameCount)
    {
        if (m_stats.empty() || frameCount == 0)
            return;

        // Sort by total time
        std::vector<std::pair<uint32_t, FunctionStats>> entries(m_stats.begin(), m_stats.end());
        std::sort(entries.begin(), entries.end(),
                  [](const auto &a, const auto &b) { return a.second.totalNs > b.second.totalNs; });

        std::fprintf(stderr, "\n=== TOP RECOMPILED FUNCTIONS BY TOTAL CPU TIME ===\n\n");
        std::fprintf(stderr, "%-4s %-12s %-40s %10s %10s %10s\n",
                     "#", "Address", "Name", "ms/frame", "calls/f", "max_us");

        const uint32_t limit = std::min(topN, static_cast<uint32_t>(entries.size()));
        const double fcount = static_cast<double>(frameCount);
        for (uint32_t i = 0; i < limit; ++i)
        {
            const auto &[pc, stats] = entries[i];
            const std::string name = getName(pc);
            char nameBuf[48];
            std::snprintf(nameBuf, sizeof(nameBuf), "%.40s", name.c_str());

            const double msPerFrame = static_cast<double>(stats.totalNs) / (1e6 * fcount);
            const double callsPerFrame = static_cast<double>(stats.callCount) / fcount;
            const double maxUs = static_cast<double>(stats.maxNs) / 1e3;

            std::fprintf(stderr, "%-4u 0x%08X  %-40s %10.2f %10.1f %10.1f\n",
                         i + 1, pc, nameBuf, msPerFrame, callsPerFrame, maxUs);
        }
        std::fprintf(stderr, "\n");
    }

    void resetStats()
    {
        m_stats.clear();
    }

private:
    FunctionProfiler() = default;
    std::unordered_map<uint32_t, FunctionStats> m_stats;
    std::map<uint32_t, std::string> m_names;
};

// ============================================================================
// §7. CSV Writer — buffered per-frame telemetry output
// ============================================================================

class CsvWriter
{
public:
    static CsvWriter &instance()
    {
        static CsvWriter inst;
        return inst;
    }

    void open()
    {
        if (m_file.is_open())
            return;

        // Create logs/ directory if it doesn't exist
        std::error_code ec;
        std::filesystem::create_directories("logs", ec);

        // Generate filename with timestamp
        const auto now = std::chrono::system_clock::now();
        const auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif

        char filename[128];
        std::snprintf(filename, sizeof(filename),
                      "logs/performance_%04d-%02d-%02d_%02d-%02d-%02d.csv",
                      1900 + tm.tm_year, 1 + tm.tm_mon, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec);

        m_file.open(filename, std::ios::out | std::ios::trunc);
        if (!m_file.is_open())
        {
            std::fprintf(stderr, "[dc2_profiler] Could not open CSV: %s\n", filename);
            return;
        }

        std::fprintf(stderr, "[dc2_profiler] CSV output: %s\n", filename);

        // Header
        m_file << "frame,frame_ms,fps";
        for (uint32_t i = 0; i < static_cast<uint32_t>(Category::COUNT); ++i)
        {
            if (static_cast<Category>(i) == Category::Frame)
                continue; // frame_ms already in header
            m_file << "," << categoryName(static_cast<Category>(i)) << "_ms";
        }
        for (uint32_t i = 0; i < static_cast<uint32_t>(Counter::COUNT); ++i)
            m_file << "," << counterName(static_cast<Counter>(i));
        m_file << "\n";
    }

    void writeRow(uint64_t frameNum, const FrameRecord &rec)
    {
        if (!m_file.is_open())
            return;

        // Buffer in memory, flush periodically
        char buf[512];
        int pos = std::snprintf(buf, sizeof(buf), "%llu,%.3f,%.1f",
                                static_cast<unsigned long long>(frameNum),
                                rec.frameMs, rec.fps);

        for (uint32_t i = 0; i < static_cast<uint32_t>(Category::COUNT); ++i)
        {
            if (static_cast<Category>(i) == Category::Frame)
                continue;
            pos += std::snprintf(buf + pos, sizeof(buf) - pos, ",%.3f", rec.categoryMs[i]);
        }
        for (uint32_t i = 0; i < static_cast<uint32_t>(Counter::COUNT); ++i)
            pos += std::snprintf(buf + pos, sizeof(buf) - pos, ",%llu",
                                 static_cast<unsigned long long>(rec.counterValues[i]));

        m_buffer.append(buf);
        m_buffer.push_back('\n');
        ++m_rowsSinceFlush;

        // Flush every 60 rows to avoid per-frame I/O
        if (m_rowsSinceFlush >= 60)
            flush();
    }

    void flush()
    {
        if (!m_file.is_open() || m_buffer.empty())
            return;
        m_file << m_buffer;
        m_file.flush();
        m_buffer.clear();
        m_rowsSinceFlush = 0;
    }

    void close()
    {
        flush();
        if (m_file.is_open())
            m_file.close();
    }

private:
    CsvWriter() = default;
    std::ofstream m_file;
    std::string m_buffer;
    uint32_t m_rowsSinceFlush = 0;
};

// ============================================================================
// §8. FrameProfiler — central aggregator
// ============================================================================

class FrameProfiler
{
public:
    static FrameProfiler &instance()
    {
        static FrameProfiler inst;
        return inst;
    }

    // --- Main-thread scope timing ---

    void addCategoryTime(Category cat, double ms)
    {
        m_currentFrame.categoryMs[static_cast<uint32_t>(cat)] += ms;
    }

    void incrementCounter(Counter ctr, uint64_t amount = 1)
    {
        m_currentFrame.counterValues[static_cast<uint32_t>(ctr)] += amount;
    }

    // --- Cross-thread timing collection ---
    // Worker threads write cumulative ns; main thread snapshots deltas at frame boundary.

    // Called by main thread at start of frame to snapshot cross-thread timing
    void snapshotCrossThreadTimingStart()
    {
        if (g297MtvuActive())
        {
            m_vu1BusyNsStart = G297Mtvu::instance().workerBusyNs();
            m_vu1KicksStart = G297Mtvu::instance().workerKicksRun();
        }
        m_gsBusyNsStart = g303_gs_worker_busy_ns();
    }

    // Called by main thread at end of frame to compute deltas
    void snapshotCrossThreadTimingEnd()
    {
        if (g297MtvuActive())
        {
            const uint64_t vu1Now = G297Mtvu::instance().workerBusyNs();
            const uint64_t vu1Delta = vu1Now - m_vu1BusyNsStart;
            m_currentFrame.categoryMs[static_cast<uint32_t>(Category::VU1)] =
                static_cast<double>(vu1Delta) / 1e6;

            const uint64_t kicksNow = G297Mtvu::instance().workerKicksRun();
            m_currentFrame.counterValues[static_cast<uint32_t>(Counter::VU1Kicks)] =
                kicksNow - m_vu1KicksStart;
        }

        const uint64_t gsNow = g303_gs_worker_busy_ns();
        const uint64_t gsDelta = gsNow - m_gsBusyNsStart;
        m_currentFrame.categoryMs[static_cast<uint32_t>(Category::GS)] =
            static_cast<double>(gsDelta) / 1e6;
    }

    // --- EE CPU timing (accumulated from dispatchLoop via atomic) ---
    std::atomic<uint64_t> eeCpuAccumNs{0};
    std::atomic<uint64_t> recompCallsAccum{0};

    void flushEeCpuTiming()
    {
        const uint64_t ns = eeCpuAccumNs.exchange(0, std::memory_order_relaxed);
        m_currentFrame.categoryMs[static_cast<uint32_t>(Category::EE_CPU)] =
            static_cast<double>(ns) / 1e6;
        m_currentFrame.counterValues[static_cast<uint32_t>(Counter::RecompiledCalls)] =
            recompCallsAccum.exchange(0, std::memory_order_relaxed);
    }

    // --- Frame lifecycle ---

    void beginFrame()
    {
        m_frameStart = std::chrono::steady_clock::now();
        std::memset(&m_currentFrame, 0, sizeof(m_currentFrame));
        snapshotCrossThreadTimingStart();
    }

    void endFrame()
    {
        const auto frameEnd = std::chrono::steady_clock::now();
        const double frameMs = std::chrono::duration<double, std::milli>(frameEnd - m_frameStart).count();
        m_currentFrame.frameMs = frameMs;
        m_currentFrame.fps = (frameMs > 0.0) ? (1000.0 / frameMs) : 0.0;
        m_currentFrame.categoryMs[static_cast<uint32_t>(Category::Frame)] = frameMs;

        // Flush cross-thread timing
        snapshotCrossThreadTimingEnd();
        flushEeCpuTiming();

        // Compute "Other" — derived, not a timed scope. It is the main-thread frame time
        // minus the main-thread scoped categories. Cross-thread times are NOT subtracted
        // because they run in parallel on other threads.
        // (We don't have an explicit "Other" category — it's computed in display/summary.)

        // Store in rolling window
        const uint32_t winSize = rollingWindowSize();
        if (m_history.size() >= winSize)
            m_history.erase(m_history.begin());
        m_history.push_back(m_currentFrame);
        ++m_totalFrames;
        ++m_windowFrames;

        // CSV output
        if (csvEnabled())
        {
            if (!m_csvOpened)
            {
                CsvWriter::instance().open();
                m_csvOpened = true;
            }
            CsvWriter::instance().writeRow(m_totalFrames, m_currentFrame);
        }

        // Slow-frame warning
        if (frameMs > slowFrameThresholdMs() && m_totalFrames > 60)
        {
            // Rate-limit: at most one warning per 30 frames
            if (m_totalFrames - m_lastSlowFrameReport >= 30)
            {
                m_lastSlowFrameReport = m_totalFrames;
                std::fprintf(stderr,
                             "\n[dc2_profiler] WARNING: slow frame %llu: %.1f ms\n",
                             static_cast<unsigned long long>(m_totalFrames), frameMs);
                printFrameBreakdown(m_currentFrame);
            }
        }

        // Periodic summary
        if (m_windowFrames >= winSize)
        {
            printSummary();
            m_windowFrames = 0;

            // Function profiler periodic report
            if (functionProfilingEnabled())
            {
                FunctionProfiler::instance().printTopFunctions(20, winSize);
                FunctionProfiler::instance().resetStats();
            }
        }
    }

    // --- Display helpers ---

    double getAverageFps() const
    {
        const double avgMs = getAverageFrameMs();
        return (avgMs > 0.0) ? (1000.0 / avgMs) : 0.0;
    }

    double getAverageFrameMs() const
    {
        if (m_history.empty())
            return 0.0;
        double sum = 0.0;
        for (const auto &r : m_history)
            sum += r.frameMs;
        return sum / static_cast<double>(m_history.size());
    }

    double getAverageCategoryMs(Category cat) const
    {
        if (m_history.empty())
            return 0.0;
        double sum = 0.0;
        for (const auto &r : m_history)
            sum += r.categoryMs[static_cast<uint32_t>(cat)];
        return sum / static_cast<double>(m_history.size());
    }

    PercentileStats getFramePercentiles() const
    {
        std::vector<double> values;
        values.reserve(m_history.size());
        for (const auto &r : m_history)
            values.push_back(r.frameMs);
        return computePercentiles(values);
    }

    const FrameRecord &currentFrame() const { return m_currentFrame; }
    uint64_t totalFrames() const { return m_totalFrames; }

    // Compute main-thread "other" time (frame minus sum of main-thread scoped categories)
    double computeOtherMs(const FrameRecord &rec) const
    {
        double mainThreadScoped = 0.0;
        // Main-thread categories: Upload, Present, DrawHUD, WaitFrame, Input
        mainThreadScoped += rec.categoryMs[static_cast<uint32_t>(Category::Upload)];
        mainThreadScoped += rec.categoryMs[static_cast<uint32_t>(Category::Present)];
        mainThreadScoped += rec.categoryMs[static_cast<uint32_t>(Category::DrawHUD)];
        mainThreadScoped += rec.categoryMs[static_cast<uint32_t>(Category::WaitFrame)];
        mainThreadScoped += rec.categoryMs[static_cast<uint32_t>(Category::Input)];
        const double other = rec.frameMs - mainThreadScoped;
        return other > 0.0 ? other : 0.0;
    }

    double computeAverageOtherMs() const
    {
        if (m_history.empty())
            return 0.0;
        double sum = 0.0;
        for (const auto &r : m_history)
            sum += computeOtherMs(r);
        return sum / static_cast<double>(m_history.size());
    }

    void printFrameBreakdown(const FrameRecord &rec)
    {
        // Main thread breakdown
        std::fprintf(stderr, "  Upload:    %6.1f ms\n", rec.categoryMs[static_cast<uint32_t>(Category::Upload)]);
        std::fprintf(stderr, "  Present:   %6.1f ms\n", rec.categoryMs[static_cast<uint32_t>(Category::Present)]);
        std::fprintf(stderr, "  WaitFrame: %6.1f ms\n", rec.categoryMs[static_cast<uint32_t>(Category::WaitFrame)]);
        std::fprintf(stderr, "  Input:     %6.1f ms\n", rec.categoryMs[static_cast<uint32_t>(Category::Input)]);
        std::fprintf(stderr, "  Other:     %6.1f ms  (main thread residual)\n", computeOtherMs(rec));
        // Cross-thread (concurrent, not additive with main thread)
        std::fprintf(stderr, "  --- concurrent worker threads ---\n");
        std::fprintf(stderr, "  EE CPU:    %6.1f ms  (GameThread)\n", rec.categoryMs[static_cast<uint32_t>(Category::EE_CPU)]);
        std::fprintf(stderr, "  VU1:       %6.1f ms  (MTVU worker)\n", rec.categoryMs[static_cast<uint32_t>(Category::VU1)]);
        std::fprintf(stderr, "  GS:        %6.1f ms  (MTGS worker)\n", rec.categoryMs[static_cast<uint32_t>(Category::GS)]);
    }

    void printSummary()
    {
        const uint32_t n = static_cast<uint32_t>(m_history.size());
        if (n == 0)
            return;

        const PercentileStats frameStats = getFramePercentiles();
        const double avgFps = getAverageFps();

        std::fprintf(stderr, "\n=== PERFORMANCE SUMMARY (dc2_profiler) ===\n\n");
        std::fprintf(stderr, "Frames: %u\n\n", n);

        std::fprintf(stderr, "Frame time:\n");
        std::fprintf(stderr, "  avg     %7.2f ms\n", frameStats.avg);
        std::fprintf(stderr, "  median  %7.2f ms\n", frameStats.median);
        std::fprintf(stderr, "  p95     %7.2f ms\n", frameStats.p95);
        std::fprintf(stderr, "  p99     %7.2f ms\n", frameStats.p99);
        std::fprintf(stderr, "  min     %7.2f ms\n", frameStats.minVal);
        std::fprintf(stderr, "  max     %7.2f ms\n", frameStats.maxVal);

        std::fprintf(stderr, "\nAverage main-thread breakdown:\n");
        const double avgFrame = frameStats.avg;

        auto printCat = [&](const char *label, Category cat) {
            const double ms = getAverageCategoryMs(cat);
            const double pct = (avgFrame > 0.0) ? (ms / avgFrame * 100.0) : 0.0;
            std::fprintf(stderr, "  %-12s %7.2f ms  %5.1f%%\n", label, ms, pct);
        };

        printCat("Upload", Category::Upload);
        printCat("Present", Category::Present);
        printCat("WaitFrame", Category::WaitFrame);
        printCat("Input", Category::Input);
        printCat("DrawHUD", Category::DrawHUD);
        {
            const double ms = computeAverageOtherMs();
            const double pct = (avgFrame > 0.0) ? (ms / avgFrame * 100.0) : 0.0;
            std::fprintf(stderr, "  %-12s %7.2f ms  %5.1f%%  (residual)\n", "Other", ms, pct);
        }

        std::fprintf(stderr, "\nConcurrent worker-thread averages:\n");
        printCat("EE CPU", Category::EE_CPU);
        printCat("VU1", Category::VU1);
        printCat("GS", Category::GS);

        std::fprintf(stderr, "\nAverage FPS: %.1f\n", avgFps);

        // Counter averages
        std::fprintf(stderr, "\nCounters (per frame avg):\n");
        for (uint32_t i = 0; i < static_cast<uint32_t>(Counter::COUNT); ++i)
        {
            double sum = 0.0;
            for (const auto &r : m_history)
                sum += static_cast<double>(r.counterValues[i]);
            std::fprintf(stderr, "  %-20s %10.1f\n", counterName(static_cast<Counter>(i)), sum / n);
        }

        std::fprintf(stderr, "\n===========================================\n\n");
        std::fflush(stderr);
    }

private:
    FrameProfiler() = default;

    std::chrono::steady_clock::time_point m_frameStart{};
    FrameRecord m_currentFrame{};
    std::vector<FrameRecord> m_history;
    uint64_t m_totalFrames = 0;
    uint32_t m_windowFrames = 0;
    uint64_t m_lastSlowFrameReport = 0;
    bool m_csvOpened = false;

    // Cross-thread snapshots (start-of-frame values)
    uint64_t m_vu1BusyNsStart = 0;
    uint64_t m_vu1KicksStart = 0;
    uint64_t m_gsBusyNsStart = 0;
};

// ============================================================================
// §9. ProfileScope — RAII scope timer
// ============================================================================

#if ENABLE_PROFILER

class ProfileScope
{
public:
    ProfileScope(Category cat) : m_cat(cat)
    {
        if (profilerEnabled())
            m_start = std::chrono::steady_clock::now();
    }

    ~ProfileScope()
    {
        if (profilerEnabled())
        {
            const auto end = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(end - m_start).count();
            FrameProfiler::instance().addCategoryTime(m_cat, ms);
        }
    }

    // Non-copyable
    ProfileScope(const ProfileScope &) = delete;
    ProfileScope &operator=(const ProfileScope &) = delete;

private:
    Category m_cat;
    std::chrono::steady_clock::time_point m_start{};
};

#define DC2_PROFILE_SCOPE(cat) \
    dc2_profiler::ProfileScope dc2_profiler_scope_##cat(dc2_profiler::Category::cat)

#define DC2_PROFILE_COUNT(ctr) \
    do { if (dc2_profiler::profilerEnabled()) \
        dc2_profiler::FrameProfiler::instance().incrementCounter(dc2_profiler::Counter::ctr); \
    } while (0)

#else // !ENABLE_PROFILER

#define DC2_PROFILE_SCOPE(cat) ((void)0)
#define DC2_PROFILE_COUNT(ctr) ((void)0)

#endif // ENABLE_PROFILER

// ============================================================================
// §10. Overlay renderer — uses Raylib DrawText (same as DC2_HUD)
// ============================================================================

static std::atomic<bool> g_overlayVisible{false};

static void toggleOverlay()
{
    g_overlayVisible.store(!g_overlayVisible.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
}

static bool isOverlayVisible()
{
    return overlayEnabled() || g_overlayVisible.load(std::memory_order_relaxed);
}

static void renderOverlay()
{
#if ENABLE_PROFILER
    if (!profilerEnabled() || !isOverlayVisible())
        return;

    const auto &prof = FrameProfiler::instance();
    const auto &rec = prof.currentFrame();

    const int x = 8;
    int y = 8;
    const int lineH = 16;
    const int fontSize = 14;

    // Background
    DrawRectangle(x - 4, y - 4, 310, 220, Fade(BLACK, 0.75f));

    char buf[128];

    // FPS and frame time
    std::snprintf(buf, sizeof(buf), "FPS: %.1f | Frame: %.2f ms",
                  prof.getAverageFps(), prof.getAverageFrameMs());
    DrawText(buf, x, y, fontSize, RAYWHITE);
    y += lineH + 2;

    // Separator
    DrawText("------------------------------", x, y, fontSize - 2, GRAY);
    y += lineH - 2;

    // Main thread breakdown
    auto drawRow = [&](const char *label, double ms, bool isCrossThread = false) {
        const double pct = (rec.frameMs > 0.0) ? (ms / rec.frameMs * 100.0) : 0.0;
        Color col = isCrossThread ? SKYBLUE : LIME;
        if (pct > 40.0) col = RED;
        else if (pct > 20.0) col = YELLOW;
        std::snprintf(buf, sizeof(buf), "%-10s %6.1f ms  %4.0f%%", label, ms, pct);
        DrawText(buf, x, y, fontSize, col);
        y += lineH;
    };

    drawRow("Upload", prof.getAverageCategoryMs(Category::Upload));
    drawRow("Present", prof.getAverageCategoryMs(Category::Present));
    drawRow("WaitFrame", prof.getAverageCategoryMs(Category::WaitFrame));
    drawRow("Input", prof.getAverageCategoryMs(Category::Input));
    drawRow("Other", prof.computeAverageOtherMs());

    y += 2;
    DrawText("--- workers (concurrent) ---", x, y, fontSize - 2, GRAY);
    y += lineH - 2;

    drawRow("EE CPU", prof.getAverageCategoryMs(Category::EE_CPU), true);
    drawRow("VU1", prof.getAverageCategoryMs(Category::VU1), true);
    drawRow("GS", prof.getAverageCategoryMs(Category::GS), true);

    // Percentiles
    y += 2;
    const auto pstats = prof.getFramePercentiles();
    std::snprintf(buf, sizeof(buf), "p50:%.1f p95:%.1f p99:%.1f max:%.1f",
                  pstats.median, pstats.p95, pstats.p99, pstats.maxVal);
    DrawText(buf, x, y, fontSize - 1, DARKGRAY);
    y += lineH;

#endif // ENABLE_PROFILER
}

// ============================================================================
// §11. EE CPU timing helper for dispatchLoop
// ============================================================================

#if ENABLE_PROFILER

// Lightweight per-call timer for dispatchLoop. Accumulates into FrameProfiler::eeCpuAccumNs.
class EeCallTimer
{
public:
    EeCallTimer()
    {
        m_start = std::chrono::steady_clock::now();
    }

    ~EeCallTimer()
    {
        const auto end = std::chrono::steady_clock::now();
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - m_start).count());
        FrameProfiler::instance().eeCpuAccumNs.fetch_add(ns, std::memory_order_relaxed);
        FrameProfiler::instance().recompCallsAccum.fetch_add(1, std::memory_order_relaxed);
    }

    EeCallTimer(const EeCallTimer &) = delete;
    EeCallTimer &operator=(const EeCallTimer &) = delete;

private:
    std::chrono::steady_clock::time_point m_start;
};

#endif // ENABLE_PROFILER

// ============================================================================
// §12. Function profiling timer for dispatchLoop (optional, high overhead)
// ============================================================================

#if ENABLE_PROFILER

class FunctionCallTimer
{
public:
    FunctionCallTimer(uint32_t pc) : m_pc(pc)
    {
        m_start = std::chrono::steady_clock::now();
    }

    ~FunctionCallTimer()
    {
        const auto end = std::chrono::steady_clock::now();
        const uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - m_start).count());
        FunctionProfiler::instance().recordCall(m_pc, ns);
    }

    FunctionCallTimer(const FunctionCallTimer &) = delete;
    FunctionCallTimer &operator=(const FunctionCallTimer &) = delete;

private:
    uint32_t m_pc;
    std::chrono::steady_clock::time_point m_start;
};

#endif // ENABLE_PROFILER

// ============================================================================
// §13. Initialization helper
// ============================================================================

static void initProfiler()
{
    if (!profilerEnabled())
    {
        std::fprintf(stderr, "[dc2_profiler] Profiler DISABLED (set DC2_PROFILE=1 to enable)\n");
        return;
    }

    std::fprintf(stderr, "[dc2_profiler] Profiler ENABLED\n");
    std::fprintf(stderr, "[dc2_profiler]   Window: %u frames\n", rollingWindowSize());
    std::fprintf(stderr, "[dc2_profiler]   Slow frame threshold: %.0f ms\n", slowFrameThresholdMs());
    std::fprintf(stderr, "[dc2_profiler]   Overlay: %s (toggle with F3)\n",
                 overlayEnabled() ? "ON" : "OFF (set DC2_PROFILE_OVERLAY=1 or press F3)");
    std::fprintf(stderr, "[dc2_profiler]   CSV: %s\n",
                 csvEnabled() ? "ON" : "OFF (set DC2_PROFILE_CSV=1)");
    std::fprintf(stderr, "[dc2_profiler]   Function profiling: %s\n",
                 functionProfilingEnabled() ? "ON (WARNING: high overhead!)"
                                            : "OFF (set DC2_PROFILE_FUNCTIONS=1)");

    if (functionProfilingEnabled())
    {
        // Try to load function names from the index
        // The index file is at ref/index/functions_index.json relative to CWD
        FunctionProfiler::instance().loadFunctionNames("ref/index/functions_index.json");
    }
}

} // namespace dc2_profiler
