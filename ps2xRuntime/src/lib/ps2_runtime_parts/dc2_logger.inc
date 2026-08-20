// dc2_logger.inc — Structured runtime logging engine for Dark Cloud 2 recompilation.
// Included into ps2_runtime.cpp. All code is file-scope static or in anonymous namespace.
// No .h modifications (project rule: avoids 30+ hour full rebuild).

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>



#ifdef ERROR
#undef ERROR
#endif
#ifdef DEBUG
#undef DEBUG
#endif
#ifdef INFO
#undef INFO
#endif
#ifdef WARNING
#undef WARNING
#endif
#ifdef FATAL
#undef FATAL
#endif

namespace dc2_log
{

// ============================================================================
// §1. Log Levels & Subsystem Categories
// ============================================================================

enum class LogLevel : uint8_t
{
    TRACE = 0,
    DEBUG = 1,
    INFO = 2,
    WARNING = 3,
    ERROR = 4,
    FATAL = 5,
    COUNT
};

enum class LogCategory : uint8_t
{
    Runtime = 0,
    Recompiler,
    EE,
    VU0,
    VU1,
    GS,
    Renderer,
    MTGS,
    MTVU,
    Input,
    Audio,
    FileIO,
    MemoryCard,
    GameOverride,
    Performance,
    Launcher,
    Crash,
    General,
    TextureReplacement,
    COUNT
};

static constexpr const char *kLogLevelNames[] = {
    "TRACE",
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR",
    "FATAL"
};

static constexpr const char *kLogCategoryNames[] = {
    "Runtime",
    "Recompiler",
    "EE",
    "VU0",
    "VU1",
    "GS",
    "Renderer",
    "MTGS",
    "MTVU",
    "Input",
    "Audio",
    "FileIO",
    "MemoryCard",
    "GameOverride",
    "Performance",
    "Launcher",
    "Crash",
    "General",
    "TextureReplacement"
};

inline const char *levelToString(LogLevel level)
{
    const auto idx = static_cast<size_t>(level);
    return (idx < sizeof(kLogLevelNames) / sizeof(kLogLevelNames[0])) ? kLogLevelNames[idx] : "UNKNOWN";
}

inline const char *categoryToString(LogCategory category)
{
    const auto idx = static_cast<size_t>(category);
    return (idx < sizeof(kLogCategoryNames) / sizeof(kLogCategoryNames[0])) ? kLogCategoryNames[idx] : "Unknown";
}

inline LogLevel stringToLevel(std::string_view str)
{
    if (str == "TRACE" || str == "trace") return LogLevel::TRACE;
    if (str == "DEBUG" || str == "debug") return LogLevel::DEBUG;
    if (str == "INFO"  || str == "info")  return LogLevel::INFO;
    if (str == "WARN"  || str == "warn" || str == "WARNING" || str == "warning") return LogLevel::WARNING;
    if (str == "ERROR" || str == "error") return LogLevel::ERROR;
    if (str == "FATAL" || str == "fatal") return LogLevel::FATAL;
    return LogLevel::INFO;
}

inline LogCategory stringToCategory(std::string_view str)
{
    for (size_t i = 0; i < static_cast<size_t>(LogCategory::COUNT); ++i)
    {
        if (str == kLogCategoryNames[i])
            return static_cast<LogCategory>(i);
    }
    return LogCategory::General;
}

// ============================================================================
// §2. In-Memory Ring Buffer Structure
// ============================================================================

struct LogRecord
{
    uint64_t timestampUs; // Microseconds since logger initialization
    uint64_t frame;       // Frame number at time of log
    LogLevel level;
    LogCategory category;
    char threadName[24];
    char message[256];
};

static constexpr size_t kRingBufferSize = 512;

class RingBuffer
{
public:
    RingBuffer() : m_head(0), m_count(0) {}

    void push(const LogRecord &record)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_records[m_head] = record;
        m_head = (m_head + 1) % kRingBufferSize;
        if (m_count < kRingBufferSize)
            m_count++;
    }

    void copyTo(std::vector<LogRecord> &out) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        out.clear();
        out.reserve(m_count);
        if (m_count == 0) return;

        const size_t start = (m_count < kRingBufferSize) ? 0 : m_head;
        for (size_t i = 0; i < m_count; ++i)
        {
            out.push_back(m_records[(start + i) % kRingBufferSize]);
        }
    }

    // Unsynchronized copy for crash reporter in emergency mode
    size_t tryCopyUnsynchronized(LogRecord *dest, size_t maxRecords) const
    {
        const size_t count = (m_count < maxRecords) ? m_count : maxRecords;
        if (count == 0) return 0;
        const size_t start = (m_count < kRingBufferSize) ? 0 : m_head;
        for (size_t i = 0; i < count; ++i)
        {
            dest[i] = m_records[(start + i) % kRingBufferSize];
        }
        return count;
    }

private:
    mutable std::mutex m_mutex;
    LogRecord m_records[kRingBufferSize];
    size_t m_head;
    size_t m_count;
};

// ============================================================================
// §3. Thread Naming Integration
// ============================================================================

inline thread_local char t_threadName[24] = {0};

inline void setCurrentThreadName(const char *name)
{
    if (name && name[0] != '\0')
    {
        strncpy(t_threadName, name, sizeof(t_threadName) - 1);
        t_threadName[sizeof(t_threadName) - 1] = '\0';
    }
}

inline const char *getCurrentThreadName()
{
    if (t_threadName[0] != '\0')
    {
        return t_threadName;
    }
    return "Worker";
}

// ============================================================================
// §4. Global Logger State & Configuration
// ============================================================================

class Logger
{
public:
    static Logger &instance()
    {
        static Logger s_instance;
        return s_instance;
    }

    void init(const char *logDir = "logs")
    {
        std::lock_guard<std::mutex> lock(m_initMutex);
        if (m_initialized) return;

        m_startTime = std::chrono::steady_clock::now();
        m_logDir = logDir ? logDir : "logs";

        // Read environment variables
        parseEnvironmentConfig();

        // Setup log files
        if (m_fileOutputEnabled)
        {
            setupLogFiles();
        }

        m_initialized = true;
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        if (m_latestFile)
        {
            std::fflush(m_latestFile);
            std::fclose(m_latestFile);
            m_latestFile = nullptr;
        }
        if (m_sessionFile)
        {
            std::fflush(m_sessionFile);
            std::fclose(m_sessionFile);
            m_sessionFile = nullptr;
        }
    }

    bool isLevelEnabled(LogLevel level) const
    {
        return static_cast<uint8_t>(level) >= static_cast<uint8_t>(m_minLevel.load(std::memory_order_relaxed));
    }

    bool isCategoryEnabled(LogCategory cat) const
    {
        const auto idx = static_cast<size_t>(cat);
        if (idx >= static_cast<size_t>(LogCategory::COUNT)) return true;
        return m_categoryEnabled[idx].load(std::memory_order_relaxed);
    }

    void setMinLevel(LogLevel level)
    {
        m_minLevel.store(level, std::memory_order_relaxed);
    }

    LogLevel getMinLevel() const
    {
        return m_minLevel.load(std::memory_order_relaxed);
    }

    void setCategoryEnabled(LogCategory cat, bool enabled)
    {
        const auto idx = static_cast<size_t>(cat);
        if (idx < static_cast<size_t>(LogCategory::COUNT))
        {
            m_categoryEnabled[idx].store(enabled, std::memory_order_relaxed);
        }
    }

    void setFrameNumber(uint64_t frame)
    {
        m_currentFrame.store(frame, std::memory_order_relaxed);
    }

    uint64_t getFrameNumber() const
    {
        return m_currentFrame.load(std::memory_order_relaxed);
    }

    void log(LogLevel level, LogCategory cat, const char *fmt, va_list args)
    {
        if (!m_initialized)
        {
            init("logs");
        }

        // Fast-path filter check
        if (!isLevelEnabled(level) || !isCategoryEnabled(cat))
        {
            return;
        }

        // Format message
        char messageBuf[256];
        vsnprintf(messageBuf, sizeof(messageBuf), fmt, args);
        messageBuf[sizeof(messageBuf) - 1] = '\0';

        // Strip trailing newline if present for uniform formatting
        size_t len = strlen(messageBuf);
        while (len > 0 && (messageBuf[len - 1] == '\n' || messageBuf[len - 1] == '\r'))
        {
            messageBuf[--len] = '\0';
        }

        const uint64_t frame = m_currentFrame.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        const uint64_t elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(now - m_startTime).count();

        // Get thread name
        const char *tname = getCurrentThreadName();

        // Create log record
        LogRecord record;
        record.timestampUs = elapsedUs;
        record.frame = frame;
        record.level = level;
        record.category = cat;
        strncpy(record.threadName, tname ? tname : "Unknown", sizeof(record.threadName) - 1);
        record.threadName[sizeof(record.threadName) - 1] = '\0';
        strncpy(record.message, messageBuf, sizeof(record.message) - 1);
        record.message[sizeof(record.message) - 1] = '\0';

        // Push to in-memory ring buffer (always, for crash reporting)
        m_ringBuffer.push(record);

        // Format time string
        char timeStr[32];
        formatTimestamp(timeStr, sizeof(timeStr));

        // Format line: [HH:MM:SS.mmm][Frame N][Thread Name][LEVEL][Category] message\n
        char formattedLine[512];
        snprintf(formattedLine, sizeof(formattedLine),
                 "[%s][Frame %llu][%s][%s][%s] %s\n",
                 timeStr,
                 (unsigned long long)frame,
                 record.threadName,
                 levelToString(level),
                 categoryToString(cat),
                 record.message);

        // Write outputs with lock
        {
            std::lock_guard<std::mutex> lock(m_writeMutex);

            if (m_consoleOutputEnabled)
            {
                std::fputs(formattedLine, stderr);
                if (level >= LogLevel::ERROR)
                {
                    std::fflush(stderr);
                }
            }

            if (m_fileOutputEnabled)
            {
                writeToFileLocked(formattedLine);
            }
        }
    }

    void flush()
    {
        std::lock_guard<std::mutex> lock(m_writeMutex);
        if (m_consoleOutputEnabled)
        {
            std::fflush(stderr);
            std::fflush(stdout);
        }
        if (m_latestFile)
        {
            std::fflush(m_latestFile);
        }
        if (m_sessionFile)
        {
            std::fflush(m_sessionFile);
        }
    }

    const RingBuffer &getRingBuffer() const { return m_ringBuffer; }
    RingBuffer &getRingBuffer() { return m_ringBuffer; }

    const std::string &getLogDirectory() const { return m_logDir; }
    const std::string &getSessionLogPath() const { return m_sessionLogPath; }
    const std::string &getLatestLogPath() const { return m_latestLogPath; }

private:
    Logger()
        : m_initialized(false),
          m_minLevel(LogLevel::INFO),
          m_consoleOutputEnabled(true),
          m_fileOutputEnabled(true),
          m_currentFrame(0),
          m_latestFile(nullptr),
          m_sessionFile(nullptr),
          m_currentFileSize(0),
          m_maxFileSize(20 * 1024 * 1024) // 20 MB max before rotation
    {
        for (size_t i = 0; i < static_cast<size_t>(LogCategory::COUNT); ++i)
        {
            // By default all categories are enabled at minLevel except high-frequency tracing
            m_categoryEnabled[i].store(true, std::memory_order_relaxed);
        }
    }

    ~Logger()
    {
        shutdown();
    }

    void parseEnvironmentConfig()
    {
        // Log Level: DC2_LOG_LEVEL=TRACE|DEBUG|INFO|WARN|ERROR|FATAL
        if (const char *lvl = std::getenv("DC2_LOG_LEVEL"))
        {
            m_minLevel.store(stringToLevel(lvl), std::memory_order_relaxed);
        }

        // Console Output: DC2_LOG_CONSOLE=0|1
        if (const char *c = std::getenv("DC2_LOG_CONSOLE"))
        {
            m_consoleOutputEnabled = (std::atoi(c) != 0);
        }

        // File Output: DC2_LOG_FILE=0|1
        if (const char *f = std::getenv("DC2_LOG_FILE"))
        {
            m_fileOutputEnabled = (std::atoi(f) != 0);
        }

        // Category switches: DC2_LOG_<CATEGORY>=0|1
        for (size_t i = 0; i < static_cast<size_t>(LogCategory::COUNT); ++i)
        {
            std::string envName = "DC2_LOG_" + std::string(kLogCategoryNames[i]);
            for (char &ch : envName) ch = static_cast<char>(std::toupper(ch));
            if (const char *val = std::getenv(envName.c_str()))
            {
                m_categoryEnabled[i].store(std::atoi(val) != 0, std::memory_order_relaxed);
            }
        }
    }

    void setupLogFiles()
    {
        try
        {
            std::filesystem::create_directories(m_logDir);

            // Generate timestamped filename: logs/dc2_YYYY-MM-DD_HH-MM-SS.log
            const auto now = std::chrono::system_clock::now();
            const std::time_t timeNow = std::chrono::system_clock::to_time_t(now);
            std::tm tmBuf{};
#ifdef _WIN32
            localtime_s(&tmBuf, &timeNow);
#else
            localtime_r(&timeNow, &tmBuf);
#endif
            char filenameBuf[128];
            std::snprintf(filenameBuf, sizeof(filenameBuf), "dc2_%04d-%02d-%02d_%02d-%02d-%02d.log",
                          tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                          tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);

            m_sessionLogPath = (std::filesystem::path(m_logDir) / filenameBuf).string();
            m_latestLogPath = (std::filesystem::path(m_logDir) / "latest.log").string();

            m_sessionFile = std::fopen(m_sessionLogPath.c_str(), "w");
            m_latestFile = std::fopen(m_latestLogPath.c_str(), "w");

            if (m_latestFile)
            {
                m_currentFileSize = 0;
            }
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[Logger] Failed to create log directory '%s': %s\n", m_logDir.c_str(), e.what());
            m_fileOutputEnabled = false;
        }
    }

    void writeToFileLocked(const char *line)
    {
        const size_t lineLen = std::strlen(line);

        // Check for file rotation on latest.log
        if (m_latestFile && (m_currentFileSize + lineLen > m_maxFileSize))
        {
            rotateLatestLog();
        }

        if (m_latestFile)
        {
            std::fputs(line, m_latestFile);
            m_currentFileSize += lineLen;
        }

        if (m_sessionFile)
        {
            std::fputs(line, m_sessionFile);
        }
    }

    void rotateLatestLog()
    {
        if (m_latestFile)
        {
            std::fclose(m_latestFile);
            m_latestFile = nullptr;
        }

        try
        {
            const std::filesystem::path pLatest(m_latestLogPath);
            const std::filesystem::path p1 = std::filesystem::path(m_logDir) / "latest.1.log";
            const std::filesystem::path p2 = std::filesystem::path(m_logDir) / "latest.2.log";

            if (std::filesystem::exists(p1))
            {
                std::filesystem::rename(p1, p2);
            }
            if (std::filesystem::exists(pLatest))
            {
                std::filesystem::rename(pLatest, p1);
            }
        }
        catch (...)
        {
            // Ignore rotation errors and proceed
        }

        m_latestFile = std::fopen(m_latestLogPath.c_str(), "w");
        m_currentFileSize = 0;
    }

    void formatTimestamp(char *buf, size_t bufSize) const
    {
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        const std::time_t timeNow = std::chrono::system_clock::to_time_t(now);
        std::tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &timeNow);
#else
        localtime_r(&timeNow, &tmBuf);
#endif
        std::snprintf(buf, bufSize, "%02d:%02d:%02d.%03d",
                      tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec, static_cast<int>(ms.count()));
    }

    std::atomic<bool> m_initialized;
    std::atomic<LogLevel> m_minLevel;
    bool m_consoleOutputEnabled;
    bool m_fileOutputEnabled;
    std::atomic<uint64_t> m_currentFrame;
    std::atomic<bool> m_categoryEnabled[static_cast<size_t>(LogCategory::COUNT)];

    std::chrono::steady_clock::time_point m_startTime;
    std::string m_logDir;
    std::string m_sessionLogPath;
    std::string m_latestLogPath;
    FILE *m_latestFile;
    FILE *m_sessionFile;
    size_t m_currentFileSize;
    size_t m_maxFileSize;

    RingBuffer m_ringBuffer;
    std::mutex m_initMutex;
    std::mutex m_writeMutex;
};

// ============================================================================
// §5. Public Logging API & Helper Functions
// ============================================================================

inline void logMessage(LogLevel level, LogCategory cat, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Logger::instance().log(level, cat, fmt, args);
    va_end(args);
}

inline void init(const char *logDir = "logs")
{
    Logger::instance().init(logDir);
}

inline void shutdown()
{
    Logger::instance().shutdown();
}

inline void flush()
{
    Logger::instance().flush();
}

inline void setFrameNumber(uint64_t frame)
{
    Logger::instance().setFrameNumber(frame);
}

inline uint64_t getFrameNumber()
{
    return Logger::instance().getFrameNumber();
}

inline void setLogLevel(LogLevel level)
{
    Logger::instance().setMinLevel(level);
}

inline LogLevel getLogLevel()
{
    return Logger::instance().getMinLevel();
}

inline void setCategoryEnabled(LogCategory cat, bool enabled)
{
    Logger::instance().setCategoryEnabled(cat, enabled);
}

inline bool isCategoryEnabled(LogCategory cat)
{
    return Logger::instance().isCategoryEnabled(cat);
}

inline bool isLevelEnabled(LogLevel level)
{
    return Logger::instance().isLevelEnabled(level);
}

// ============================================================================
// §6. Startup Summary Diagnostics
// ============================================================================

struct StartupConfig
{
    const char *gameTitle = "Dark Cloud 2";
    const char *elfName = "SCUS_972.13";
    const char *buildType = "Release";
    const char *compiler = "MSVC x64";
    int resolutionW = 640;
    int resolutionH = 448;
    bool isFullscreen = false;
    bool isVSync = true;
    bool isMtvu = true;
    bool isMtgs = true;
    bool isProfiler = false;
    const char *dataPath = "";
    const char *gitCommit = nullptr;
};

inline void logStartupSummary(const StartupConfig &cfg)
{
    logMessage(LogLevel::INFO, LogCategory::Runtime, "========== DC2 RECOMP STARTUP ==========");
    logMessage(LogLevel::INFO, LogCategory::Runtime, "Game:        %s (%s)", cfg.gameTitle, cfg.elfName);
    logMessage(LogLevel::INFO, LogCategory::Runtime, "Build Type:  %s", cfg.buildType);
    logMessage(LogLevel::INFO, LogCategory::Runtime, "Compiler:    %s", cfg.compiler);
    if (cfg.gitCommit && cfg.gitCommit[0] != '\0')
    {
        logMessage(LogLevel::INFO, LogCategory::Runtime, "Git Commit:  %s", cfg.gitCommit);
    }
    logMessage(LogLevel::INFO, LogCategory::Runtime, "Resolution:  %dx%d (%s)", cfg.resolutionW, cfg.resolutionH,
               cfg.isFullscreen ? "Fullscreen" : "Windowed");
    logMessage(LogLevel::INFO, LogCategory::Runtime, "VSync:       %s (Target: 60 FPS)", cfg.isVSync ? "Enabled" : "Disabled");
    logMessage(LogLevel::INFO, LogCategory::Runtime, "MTVU (VU1):  %s", cfg.isMtvu ? "Enabled (Worker Active)" : "Disabled (Single-Threaded)");
    logMessage(LogLevel::INFO, LogCategory::Runtime, "MTGS (GS):   %s", cfg.isMtgs ? "Enabled (Worker Active)" : "Disabled (Single-Threaded)");
    logMessage(LogLevel::INFO, LogCategory::Runtime, "Profiler:    %s", cfg.isProfiler ? "Enabled" : "Disabled");
    if (cfg.dataPath && cfg.dataPath[0] != '\0')
    {
        logMessage(LogLevel::INFO, LogCategory::Runtime, "Game Data:   %s", cfg.dataPath);
    }
    logMessage(LogLevel::INFO, LogCategory::Runtime, "Log Sinks:   Console + %s", Logger::instance().getLatestLogPath().c_str());
    logMessage(LogLevel::INFO, LogCategory::Runtime, "========================================");
    flush();
}

// ============================================================================
// §7. Game Override Unresolved Handler Management
// ============================================================================

struct OverrideStats
{
    std::atomic<uint32_t> totalRegistered{0};
    std::atomic<uint32_t> totalResolved{0};
    std::atomic<uint32_t> totalUnresolved{0};
    std::mutex mutex;
    std::unordered_set<uint32_t> loggedUnresolvedAddresses;
    std::vector<std::pair<uint32_t, std::string>> unresolvedList;
};

inline OverrideStats &getOverrideStats()
{
    static OverrideStats s_stats;
    return s_stats;
}

inline void recordOverrideRegistration(uint32_t address, std::string_view handlerName, bool resolved)
{
    auto &stats = getOverrideStats();
    stats.totalRegistered.fetch_add(1, std::memory_order_relaxed);
    if (resolved)
    {
        stats.totalResolved.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        stats.totalUnresolved.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(stats.mutex);
        stats.unresolvedList.push_back({address, std::string(handlerName)});
    }
}

inline void logOverrideSummary()
{
    auto &stats = getOverrideStats();
    const uint32_t total = stats.totalRegistered.load(std::memory_order_relaxed);
    const uint32_t resolved = stats.totalResolved.load(std::memory_order_relaxed);
    const uint32_t unresolved = stats.totalUnresolved.load(std::memory_order_relaxed);

    logMessage(LogLevel::INFO, LogCategory::GameOverride,
               "Override resolution summary: %u total (%u resolved, %u unresolved)",
               total, resolved, unresolved);

    // List unresolved handlers ONCE at startup if verbose or debug logging is active
    if (unresolved > 0 && (isLevelEnabled(LogLevel::DEBUG) || std::getenv("DC2_TRACE_OVERRIDES")))
    {
        std::lock_guard<std::mutex> lock(stats.mutex);
        for (const auto &item : stats.unresolvedList)
        {
            logMessage(LogLevel::DEBUG, LogCategory::GameOverride,
                       "  Unresolved handler '%s' for address 0x%08X",
                       item.second.c_str(), item.first);
        }
    }
}

inline bool shouldLogUnresolvedCall(uint32_t address)
{
    auto &stats = getOverrideStats();
    std::lock_guard<std::mutex> lock(stats.mutex);
    return stats.loggedUnresolvedAddresses.insert(address).second;
}

} // namespace dc2_log

// ============================================================================
// §8. Logging Macros (Levels, Categories & Rate Limiting)
// ============================================================================

#define LOG_TRACE(cat, ...) do { \
    if (dc2_log::isLevelEnabled(dc2_log::LogLevel::TRACE) && dc2_log::isCategoryEnabled(dc2_log::LogCategory::cat)) \
        dc2_log::logMessage(dc2_log::LogLevel::TRACE, dc2_log::LogCategory::cat, __VA_ARGS__); \
} while(0)

#define LOG_DEBUG(cat, ...) do { \
    if (dc2_log::isLevelEnabled(dc2_log::LogLevel::DEBUG) && dc2_log::isCategoryEnabled(dc2_log::LogCategory::cat)) \
        dc2_log::logMessage(dc2_log::LogLevel::DEBUG, dc2_log::LogCategory::cat, __VA_ARGS__); \
} while(0)

#define LOG_INFO(cat, ...) do { \
    if (dc2_log::isLevelEnabled(dc2_log::LogLevel::INFO) && dc2_log::isCategoryEnabled(dc2_log::LogCategory::cat)) \
        dc2_log::logMessage(dc2_log::LogLevel::INFO, dc2_log::LogCategory::cat, __VA_ARGS__); \
} while(0)

#define LOG_WARNING(cat, ...) do { \
    if (dc2_log::isLevelEnabled(dc2_log::LogLevel::WARNING) && dc2_log::isCategoryEnabled(dc2_log::LogCategory::cat)) \
        dc2_log::logMessage(dc2_log::LogLevel::WARNING, dc2_log::LogCategory::cat, __VA_ARGS__); \
} while(0)

#define LOG_WARN(cat, ...) LOG_WARNING(cat, __VA_ARGS__)

#define LOG_ERROR(cat, ...) do { \
    if (dc2_log::isLevelEnabled(dc2_log::LogLevel::ERROR) && dc2_log::isCategoryEnabled(dc2_log::LogCategory::cat)) \
        dc2_log::logMessage(dc2_log::LogLevel::ERROR, dc2_log::LogCategory::cat, __VA_ARGS__); \
} while(0)

#define LOG_FATAL(cat, ...) do { \
    dc2_log::logMessage(dc2_log::LogLevel::FATAL, dc2_log::LogCategory::cat, __VA_ARGS__); \
    dc2_log::flush(); \
} while(0)

// Rate Limiting Macros
#define LOG_ONCE(cat, ...) do { \
    static std::atomic<bool> s_loggedOnce{false}; \
    bool expected = false; \
    if (s_loggedOnce.compare_exchange_strong(expected, true)) { \
        LOG_INFO(cat, __VA_ARGS__); \
    } \
} while(0)

#define LOG_WARN_ONCE(cat, ...) do { \
    static std::atomic<bool> s_loggedOnce{false}; \
    bool expected = false; \
    if (s_loggedOnce.compare_exchange_strong(expected, true)) { \
        LOG_WARN(cat, __VA_ARGS__); \
    } \
} while(0)

#define LOG_EVERY_N(n, cat, ...) do { \
    static std::atomic<uint64_t> s_logCounter{0}; \
    if ((s_logCounter.fetch_add(1, std::memory_order_relaxed) % (n)) == 0) { \
        LOG_INFO(cat, __VA_ARGS__); \
    } \
} while(0)

#define LOG_EVERY_N_FRAMES(n, cat, ...) do { \
    static uint64_t s_lastLogFrame = 0; \
    const uint64_t curFrame = dc2_log::getFrameNumber(); \
    if (curFrame >= s_lastLogFrame + (n)) { \
        s_lastLogFrame = curFrame; \
        LOG_INFO(cat, __VA_ARGS__); \
    } \
} while(0)

#define LOG_ONCE_KEY(key, cat, ...) do { \
    static std::mutex s_keyMtx; \
    static std::unordered_set<uint64_t> s_seenKeys; \
    bool isNew = false; \
    { \
        std::lock_guard<std::mutex> lock(s_keyMtx); \
        isNew = s_seenKeys.insert(static_cast<uint64_t>(key)).second; \
    } \
    if (isNew) { \
        LOG_INFO(cat, __VA_ARGS__); \
    } \
} while(0)

// ============================================================================
// §9. Crash Context Tracking & Assertion Hooks
// ============================================================================

struct R5900Context;

namespace dc2_crash
{
    inline std::atomic<uint32_t> g_activeGuestPc{0};
    inline std::atomic<uint32_t> g_activeGuestRa{0};
    inline std::atomic<uint32_t> g_activeGuestSp{0};
    inline std::atomic<uint32_t> g_activeGuestGp{0};
    inline R5900Context *g_activeR5900Context = nullptr;

    inline void updateActiveGuestContext(uint32_t pc, uint32_t ra, uint32_t sp, uint32_t gp, R5900Context *ctx)
    {
        g_activeGuestPc.store(pc, std::memory_order_relaxed);
        g_activeGuestRa.store(ra, std::memory_order_relaxed);
        g_activeGuestSp.store(sp, std::memory_order_relaxed);
        g_activeGuestGp.store(gp, std::memory_order_relaxed);
        if (ctx) g_activeR5900Context = ctx;
    }
}

#define DC2_ASSERT(condition) do { \
    if (!(condition)) { \
        dc2_crash::handleAssertionFailure(#condition, __FILE__, __LINE__); \
    } \
} while(0)

#define DC2_ASSERT_MSG(condition, msg) do { \
    if (!(condition)) { \
        dc2_crash::handleAssertionFailure(#condition, __FILE__, __LINE__, msg); \
    } \
} while(0)

#define DC2_FATAL(...) dc2_crash::fatalError(__VA_ARGS__)

