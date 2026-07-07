#include "Common.h"
#include "Interrupt.h"
#include "ps2_log.h"
#include "Stubs/GS.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <set>
#include <atomic>

// F60 spin diagnostics (defined in ps2_runtime.cpp).
extern std::atomic<uint64_t> g_f60_waitVSyncTick;

namespace ps2_syscalls
{
    namespace interrupt_state
    {
        constexpr uint32_t kIntcVblankStart = 2u;
        constexpr uint32_t kIntcVblankEnd = 3u;
        constexpr auto kVblankPeriod = std::chrono::microseconds(16667);
        constexpr int kMaxCatchupTicks = 4;

        std::mutex g_irq_handler_mutex;
        std::mutex g_irq_worker_mutex;
        std::condition_variable g_irq_worker_cv;
        std::mutex g_vsync_flag_mutex;
        std::condition_variable g_vsync_cv;
        std::atomic<bool> g_irq_worker_stop{false};
        std::atomic<bool> g_irq_worker_running{false};
        uint32_t g_enabled_intc_mask = 0xFFFFFFFFu;
        uint32_t g_enabled_dmac_mask = 0xFFFFFFFFu;
        uint64_t g_vsync_tick_counter = 0u;
        VSyncFlagRegistration g_vsync_registration{};
    }

    using namespace interrupt_state;

    static bool traceIrqVsyncEnabled()
    {
        static const bool enabled = []() -> bool
        {
            const char *value = std::getenv("DC2_TRACE_IRQ_VSYNC");
            return value != nullptr &&
                   value[0] != '\0' &&
                   std::strcmp(value, "0") != 0 &&
                   std::strcmp(value, "false") != 0 &&
                   std::strcmp(value, "FALSE") != 0 &&
                   std::strcmp(value, "off") != 0 &&
                   std::strcmp(value, "OFF") != 0;
        }();
        return enabled;
    }

    // F49.5: diagnostics + watchdog for guest INTC handlers. A vblank INTC handler
    // dispatched on the IRQ worker thread runs in an unbounded loop here (it must be
    // able to preempt a guest thread spinning on vblank state). If a handler itself
    // never returns (runs away), the worker stops advancing the vsync tick and any
    // main-thread sceGsSyncV/WaitForNextVSyncTick deadlocks. DC2_TRACE_INTC enumerates
    // handlers + logs a runaway; DC2_INTC_STEP_MAX (default 0 = unbounded, original
    // behavior) sets a per-invocation outer-step cap that breaks a multi-step runaway.
    static bool traceIntcEnabled()
    {
        static const bool enabled = []() -> bool
        {
            const char *value = std::getenv("DC2_TRACE_INTC");
            return value != nullptr && value[0] != '\0' &&
                   std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    static uint64_t intcStepMax()
    {
        static const uint64_t limit = []() -> uint64_t
        {
            const char *value = std::getenv("DC2_INTC_STEP_MAX");
            if (value == nullptr || value[0] == '\0')
                return 0u;
            return std::strtoull(value, nullptr, 0);
        }();
        return limit;
    }

    static void writeGuestU32NoThrow(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        if (addr == 0u)
        {
            return;
        }

        uint8_t *dst = getMemPtr(rdram, addr);
        if (!dst)
        {
            return;
        }
        std::memcpy(dst, &value, sizeof(value));
    }

    static void writeGuestU64NoThrow(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        if (addr == 0u)
        {
            return;
        }

        uint8_t *dst = getMemPtr(rdram, addr);
        if (!dst)
        {
            return;
        }
        std::memcpy(dst, &value, sizeof(value));
    }

    static uint32_t readGuestU32NoThrow(uint8_t *rdram, uint32_t addr)
    {
        if (addr == 0u)
        {
            return 0u;
        }

        uint8_t *src = getMemPtr(rdram, addr);
        if (!src)
        {
            return 0u;
        }

        uint32_t value = 0u;
        std::memcpy(&value, src, sizeof(value));
        return value;
    }

    static uint32_t getAsyncHandlerStackTop(PS2Runtime *runtime)
    {
        constexpr uint32_t kAsyncHandlerStackSize = 0x4000u;
        thread_local PS2Runtime *s_cachedRuntime = nullptr;
        thread_local uint32_t s_cachedStackTop = 0u;

        if (runtime == nullptr)
        {
            return PS2_RAM_SIZE - 0x10u;
        }

        if (s_cachedRuntime != runtime || s_cachedStackTop == 0u)
        {
            s_cachedRuntime = runtime;
            s_cachedStackTop = runtime->reserveAsyncCallbackStack(kAsyncHandlerStackSize, 16u);
        }

        return (s_cachedStackTop != 0u) ? s_cachedStackTop : (PS2_RAM_SIZE - 0x10u);
    }

    static void dispatchIntcHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause)
    {
        if (!rdram || !runtime)
        {
            return;
        }

        std::vector<IrqHandlerInfo> handlers;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (cause < 32u && (g_enabled_intc_mask & (1u << cause)) == 0u)
            {
                return;
            }

            handlers.reserve(g_intcHandlers.size());
            for (const auto &[id, info] : g_intcHandlers)
            {
                (void)id;
                if (!info.enabled)
                {
                    continue;
                }
                if (info.cause != cause)
                {
                    continue;
                }
                if (info.handler == 0u)
                {
                    continue;
                }
                handlers.push_back(info);
            }
            std::sort(handlers.begin(), handlers.end(), [](const IrqHandlerInfo &a, const IrqHandlerInfo &b)
                      { return a.order < b.order; });
        }

        for (const IrqHandlerInfo &info : handlers)
        {
            if (!runtime->hasFunction(info.handler))
            {
                if (cause == kIntcVblankStart)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        static std::atomic<uint32_t> s_missingHandlerLogCount{0u};
                        const uint32_t logIndex = s_missingHandlerLogCount.fetch_add(1u, std::memory_order_relaxed);
                        if (logIndex < 32u)
                        {
                            auto flags = std::cout.flags();
                            std::cout << "[INTC:missing] cause=" << cause
                                      << " handler=0x" << std::hex << info.handler
                                      << std::dec
                                      << " id=" << info.id
                                      << std::endl;
                            std::cout.flags(flags);
                        }
                    });
                }
                continue;
            }

            try
            {
                R5900Context irqCtx{};
                SET_GPR_U32(&irqCtx, 28, info.gp);
                SET_GPR_U32(&irqCtx, 29, getAsyncHandlerStackTop(runtime));
                SET_GPR_U32(&irqCtx, 31, 0u);
                SET_GPR_U32(&irqCtx, 4, cause);
                SET_GPR_U32(&irqCtx, 5, info.arg);
                SET_GPR_U32(&irqCtx, 6, 0u);
                SET_GPR_U32(&irqCtx, 7, 0u);
                irqCtx.pc = info.handler;

                if (traceIntcEnabled())
                {
                    static std::mutex s_intcSeenMutex;
                    static std::set<uint32_t> s_intcSeen;
                    bool firstSeen = false;
                    {
                        std::lock_guard<std::mutex> lk(s_intcSeenMutex);
                        firstSeen = s_intcSeen.insert(info.handler).second;
                    }
                    if (firstSeen)
                    {
                        std::fprintf(stderr,
                                     "[F49.5:intc] dispatch handler=0x%x cause=%u order=%d arg=0x%x gp=0x%x\n",
                                     info.handler, cause, info.order, info.arg, info.gp);
                    }
                }

                const uint64_t stepMax = intcStepMax();
                uint64_t steps = 0u;
                uint32_t prevPc = irqCtx.pc;
                while (irqCtx.pc != 0u && runtime && !runtime->isStopRequested())
                {
                    PS2Runtime::RecompiledFunction step = runtime->lookupFunction(irqCtx.pc);
                    if (!step)
                    {
                        break;
                    }
                    // Interrupt handlers must be able to preempt a guest thread that is
                    // spinning on interrupt-produced state, such as a vblank counter.
                    prevPc = irqCtx.pc;
                    step(rdram, &irqCtx, runtime);
                    if (stepMax != 0u && ++steps >= stepMax)
                    {
                        if (traceIntcEnabled())
                        {
                            std::fprintf(stderr,
                                         "[F49.5:intc] RUNAWAY handler=0x%x cause=%u steps=%llu lastPc=0x%x nextPc=0x%x -- breaking\n",
                                         info.handler, cause,
                                         static_cast<unsigned long long>(steps),
                                         prevPc, irqCtx.pc);
                        }
                        break;
                    }
                }
            }
            catch (const ThreadExitException &)
            {
            }
            catch (const std::exception &e)
            {
                static uint32_t warnCount = 0;
                if (warnCount < 8u)
                {
                    std::cerr << "[INTC] handler 0x" << std::hex << info.handler
                              << " threw exception: " << e.what() << std::dec << std::endl;
                    ++warnCount;
                }
            }
        }
    }

    void dispatchDmacHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause)
    {
        if (!rdram || !runtime)
        {
            return;
        }

        std::vector<IrqHandlerInfo> handlers;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (cause < 32u && (g_enabled_dmac_mask & (1u << cause)) == 0u)
            {
                return;
            }

            handlers.reserve(g_dmacHandlers.size());
            for (const auto &[id, info] : g_dmacHandlers)
            {
                (void)id;
                if (!info.enabled)
                {
                    continue;
                }
                if (info.cause != cause)
                {
                    continue;
                }
                if (info.handler == 0u)
                {
                    continue;
                }
                handlers.push_back(info);
            }
            std::sort(handlers.begin(), handlers.end(), [](const IrqHandlerInfo &a, const IrqHandlerInfo &b)
                      { return a.order < b.order; });
        }

        for (const IrqHandlerInfo &info : handlers)
        {
            if (!runtime->hasFunction(info.handler))
            {
                continue;
            }

            try
            {
                R5900Context irqCtx{};
                SET_GPR_U32(&irqCtx, 28, info.gp);
                SET_GPR_U32(&irqCtx, 29, getAsyncHandlerStackTop(runtime));
                SET_GPR_U32(&irqCtx, 31, 0u);
                SET_GPR_U32(&irqCtx, 4, cause);
                SET_GPR_U32(&irqCtx, 5, info.arg);
                SET_GPR_U32(&irqCtx, 6, 0u);
                SET_GPR_U32(&irqCtx, 7, 0u);
                irqCtx.pc = info.handler;

                while (irqCtx.pc != 0u && runtime && !runtime->isStopRequested())
                {
                    PS2Runtime::RecompiledFunction step = runtime->lookupFunction(irqCtx.pc);
                    if (!step)
                    {
                        break;
                    }
                    step(rdram, &irqCtx, runtime);
                }
            }
            catch (const ThreadExitException &)
            {
            }
            catch (const std::exception &e)
            {
                static uint32_t warnCount = 0;
                if (warnCount < 8u)
                {
                    std::cerr << "[DMAC] handler 0x" << std::hex << info.handler
                              << " threw exception: " << e.what() << std::dec << std::endl;
                    ++warnCount;
                }
            }
        }
    }

    static uint64_t signalVSyncFlag(uint8_t *rdram)
    {
        VSyncFlagRegistration reg{};
        uint64_t tickValue = 0u;
        {
            std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
            reg = g_vsync_registration;
            tickValue = ++g_vsync_tick_counter;
        }

        g_vsync_cv.notify_all();

        if (reg.flagAddr != 0u)
        {
            writeGuestU32NoThrow(rdram, reg.flagAddr, 1u);
        }
        if (reg.tickAddr != 0u)
        {
            writeGuestU64NoThrow(rdram, reg.tickAddr, tickValue);
        }
        return tickValue;
    }

    static void interruptWorkerMain(uint8_t *rdram, PS2Runtime *runtime)
    {
        g_currentThreadId = -1;

        using clock = std::chrono::steady_clock;
        auto nextTick = clock::now() + kVblankPeriod;

        while (runtime != nullptr && !runtime->isStopRequested())
        {
            {
                std::unique_lock<std::mutex> lock(g_irq_worker_mutex);
                if (g_irq_worker_cv.wait_until(lock, nextTick, []()
                                               { return g_irq_worker_stop.load(std::memory_order_acquire); }))
                {
                    break;
                }
            }

            const auto now = clock::now();
            int ticksToProcess = 0;
            while (now >= nextTick && ticksToProcess < kMaxCatchupTicks)
            {
                ++ticksToProcess;
                nextTick += kVblankPeriod;
            }
            if (ticksToProcess == 0)
            {
                continue;
            }

            for (int i = 0; i < ticksToProcess; ++i)
            {
                const auto t0 = clock::now();
                const uint64_t tickValue = signalVSyncFlag(rdram);
                const auto t1 = clock::now();
                ps2_stubs::dispatchGsSyncVCallback(rdram, runtime, tickValue);
                const auto t2 = clock::now();
                dispatchIntcHandlersForCause(rdram, runtime, kIntcVblankStart);
                const auto t3 = clock::now();
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                dispatchIntcHandlersForCause(rdram, runtime, kIntcVblankEnd);
                const auto t4 = clock::now();

                if (traceIrqVsyncEnabled() && (tickValue <= 16u || (tickValue % 120u) == 0u))
                {
                    const auto usSignal = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                    const auto usCallback = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
                    const auto usIntcStart = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
                    const auto usIntcEnd = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t3).count();
                    const auto usTotal = std::chrono::duration_cast<std::chrono::microseconds>(t4 - t0).count();
                    std::fprintf(stderr,
                                 "[F49:irq] tick=%llu signalUs=%lld cbUs=%lld intcStartUs=%lld intcEndUs=%lld totalUs=%lld catchup=%d\n",
                                 static_cast<unsigned long long>(tickValue),
                                 static_cast<long long>(usSignal),
                                 static_cast<long long>(usCallback),
                                 static_cast<long long>(usIntcStart),
                                 static_cast<long long>(usIntcEnd),
                                 static_cast<long long>(usTotal),
                                 ticksToProcess);
                }
            }
        }

        g_irq_worker_running.store(false, std::memory_order_release);
        g_irq_worker_cv.notify_all();
    }

    static void ensureInterruptWorkerRunning(uint8_t *rdram, PS2Runtime *runtime)
    {
        if (!rdram || !runtime)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(g_irq_worker_mutex);
        if (g_irq_worker_running.load(std::memory_order_acquire))
        {
            return;
        }

        g_irq_worker_stop.store(false, std::memory_order_release);
        g_irq_worker_running.store(true, std::memory_order_release);
        try
        {
            std::thread(interruptWorkerMain, rdram, runtime).detach();
        }
        catch (...)
        {
            g_irq_worker_running.store(false, std::memory_order_release);
        }
    }

    void EnsureVSyncWorkerRunning(uint8_t *rdram, PS2Runtime *runtime)
    {
        ensureInterruptWorkerRunning(rdram, runtime);
    }

    uint64_t GetCurrentVSyncTick()
    {
        std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
        return g_vsync_tick_counter;
    }

    void stopInterruptWorker()
    {
        g_irq_worker_stop.store(true, std::memory_order_release);
        g_irq_worker_cv.notify_all();
        std::unique_lock<std::mutex> lock(g_irq_worker_mutex);
        g_irq_worker_cv.wait_for(lock, std::chrono::milliseconds(500), []()
                                 { return !g_irq_worker_running.load(std::memory_order_acquire); });
        g_vsync_cv.notify_all();
    }

    // F49.5: watchdog timeout (ms) for the vsync-tick wait. The MainLoop inter-loop
    // transition (menu->dungeon) calls sceGsSyncV while holding the guest-execution
    // lock with no per-frame yield; in the headless backend the IRQ worker stops
    // advancing the tick at that point and the wait below deadlocks forever (proven
    // F49.5: both main thread and IRQ worker freeze at the costume-confirm transition,
    // tick frozen ~1440). A bounded wait that synthesizes forward progress breaks the
    // deadlock. Legitimate waits are notified within one vblank (~17ms) and never reach
    // the timeout. Default 1000ms; DC2_VSYNC_WAIT_TIMEOUT_MS overrides (0 = original
    // unbounded blocking behavior).
    static uint32_t vsyncWaitTimeoutMs()
    {
        static const uint32_t ms = []() -> uint32_t
        {
            const char *v = std::getenv("DC2_VSYNC_WAIT_TIMEOUT_MS");
            if (v == nullptr || v[0] == '\0')
                return 0u; // default: original unbounded blocking behavior (no regression)
            return static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
        }();
        return ms;
    }

    uint64_t WaitForNextVSyncTick(uint8_t *rdram, PS2Runtime *runtime)
    {
        const bool trace = traceIrqVsyncEnabled();
        static std::atomic<uint32_t> s_wfntLogs{0u};
        const bool logThis = trace && s_wfntLogs.fetch_add(1u, std::memory_order_relaxed) < 64u;
        if (logThis)
            std::fprintf(stderr, "[F49.5:wfnt] enter\n");
        ensureInterruptWorkerRunning(rdram, runtime);
        const uint32_t timeoutMs = vsyncWaitTimeoutMs();

        // F49.5 deadlock fix: release the guest-execution lock BEFORE taking the
        // vsync flag mutex, and reacquire it AFTER releasing the flag mutex. The
        // previous code reacquired the guest lock (GuestExecutionReleaseScope dtor)
        // while still holding g_vsync_flag_mutex, which forms an ABBA cycle with
        // another guest thread (e.g. the GamePad thread) that holds the guest lock
        // and is entering its own WaitForNextVSyncTick (wanting g_vsync_flag_mutex),
        // while the IRQ worker blocks in signalVSyncFlag on g_vsync_flag_mutex and
        // stops advancing the tick. That froze the MainLoop menu->dungeon transition
        // sceGsSyncV permanently. Never hold both locks at once.
        uint64_t result;
        {
            // Release the guest-execution lock for the whole wait so the guest lock is
            // never held together with, or reacquired while holding, g_vsync_flag_mutex
            // (breaks the ABBA cycle described above; this alone revives the IRQ worker
            // which otherwise froze in signalVSyncFlag at the transition).
            PS2Runtime::GuestExecutionReleaseScope releaseGuestExecution(runtime);

            uint64_t current;
            {
                std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
                current = g_vsync_tick_counter;
            }
            if (logThis)
                std::fprintf(stderr, "[F49.5:wfnt] guest released, current=%llu polling\n",
                             static_cast<unsigned long long>(current));

            // Poll the worker-advanced tick instead of std::condition_variable::wait:
            // the cv wait was observed to never return at the MainLoop menu->dungeon
            // transition even with the worker ticking and notifying (relock stall),
            // permanently hanging the transition sceGsSyncV. Polling the counter under
            // the brief flag-mutex avoids that and is cheap (vblank is ~16ms).
            const auto t0 = std::chrono::steady_clock::now();
            for (;;)
            {
                uint64_t now;
                {
                    std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
                    now = g_vsync_tick_counter;
                }
                if (now > current || (runtime != nullptr && runtime->isStopRequested()))
                {
                    result = now;
                    break;
                }
                if (timeoutMs != 0u)
                {
                    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - t0)
                                               .count();
                    if (elapsedMs >= static_cast<long long>(timeoutMs))
                    {
                        // Watchdog: tick did not advance within the timeout. Synthesize
                        // forward progress so guest vsync waits never hang headless.
                        std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
                        g_vsync_tick_counter = (g_vsync_tick_counter > current)
                                                   ? g_vsync_tick_counter
                                                   : current + 1u;
                        result = g_vsync_tick_counter;
                        static std::atomic<uint32_t> s_watchdogLogs{0u};
                        if (s_watchdogLogs.fetch_add(1u, std::memory_order_relaxed) < 8u)
                        {
                            std::fprintf(stderr,
                                         "[F49.5:vsync-watchdog] tick stalled at %llu; forced after %ums\n",
                                         static_cast<unsigned long long>(current), timeoutMs);
                        }
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (logThis)
                std::fprintf(stderr, "[F49.5:wfnt] poll complete result=%llu, reacquiring guest lock\n",
                             static_cast<unsigned long long>(result));
        } // guest lock reacquired here (GuestExecutionReleaseScope dtor), holding no other lock

        if (logThis)
            std::fprintf(stderr, "[F49.5:wfnt] guest reacquired, return=%llu\n",
                         static_cast<unsigned long long>(result));
        return result;
    }

    void WaitVSyncTick(uint8_t *rdram, PS2Runtime *runtime)
    {
        g_f60_waitVSyncTick.fetch_add(1, std::memory_order_relaxed);
        (void)WaitForNextVSyncTick(rdram, runtime);
    }

    void SetVSyncFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t flagAddr = getRegU32(ctx, 4);
        const uint32_t tickAddr = getRegU32(ctx, 5);

        {
            std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
            g_vsync_registration.flagAddr = flagAddr;
            g_vsync_registration.tickAddr = tickAddr;
        }

        writeGuestU32NoThrow(rdram, flagAddr, 0u);
        writeGuestU64NoThrow(rdram, tickAddr, 0u);
        ensureInterruptWorkerRunning(rdram, runtime);
        setReturnS32(ctx, KE_OK);
    }

    void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            g_enabled_intc_mask |= (1u << cause);
        }
        if (cause == kIntcVblankStart || cause == kIntcVblankEnd)
        {
            PS2_IF_AGRESSIVE_LOGS({
                static std::atomic<uint32_t> s_enableLogCount{0u};
                const uint32_t logIndex = s_enableLogCount.fetch_add(1u, std::memory_order_relaxed);
                if (logIndex < 32u)
                {
                    RUNTIME_LOG("[EnableIntc] cause=" << cause);
                }
            });
        }
        setReturnS32(ctx, KE_OK);
    }

    void iEnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableIntc(rdram, ctx, runtime);
    }

    void DisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            g_enabled_intc_mask &= ~(1u << cause);
        }
        if (cause == kIntcVblankStart || cause == kIntcVblankEnd)
        {
            PS2_IF_AGRESSIVE_LOGS({
                static std::atomic<uint32_t> s_disableLogCount{0u};
                const uint32_t logIndex = s_disableLogCount.fetch_add(1u, std::memory_order_relaxed);
                if (logIndex < 32u)
                {
                    RUNTIME_LOG("[DisableIntc] cause=" << cause);
                }
            });
        }
        setReturnS32(ctx, KE_OK);
    }

    void iDisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableIntc(rdram, ctx, runtime);
    }

    void AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IrqHandlerInfo info{};
        info.cause = getRegU32(ctx, 4);
        info.handler = getRegU32(ctx, 5);
        uint32_t next = getRegU32(ctx, 6);
        info.arg = getRegU32(ctx, 7);
        info.gp = getRegU32(ctx, 28);
        info.sp = getRegU32(ctx, 29);
        info.enabled = true;

        int handlerId = 0;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            info.order = (next == 0) ? --g_intc_head_order : ++g_intc_tail_order;
            handlerId = g_nextIntcHandlerId++;
            info.id = handlerId;
            g_intcHandlers[handlerId] = info;
        }

        if (info.cause == kIntcVblankStart)
        {
            PS2_IF_AGRESSIVE_LOGS({
                static std::atomic<uint32_t> s_addHandlerLogCount{0u};
                const uint32_t logIndex = s_addHandlerLogCount.fetch_add(1u, std::memory_order_relaxed);
                if (logIndex < 32u)
                {
                    auto flags = std::cout.flags();
                    std::cout << "[AddIntcHandler] cause=" << info.cause
                              << " handler=0x" << std::hex << info.handler
                              << " arg=0x" << info.arg
                              << " gp=0x" << info.gp
                              << " sp=0x" << info.sp
                              << std::dec
                              << " id=" << handlerId
                              << std::endl;
                    std::cout.flags(flags);
                }
            });
        }

        ensureInterruptWorkerRunning(rdram, runtime);
        setReturnS32(ctx, handlerId);
    }

    void AddIntcHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddIntcHandler(rdram, ctx, runtime);
    }

    void RemoveIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        if (handlerId > 0)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            auto it = g_intcHandlers.find(handlerId);
            if (it != g_intcHandlers.end() && it->second.cause == cause)
            {
                g_intcHandlers.erase(it);
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void AddDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IrqHandlerInfo info{};
        info.cause = getRegU32(ctx, 4);
        info.handler = getRegU32(ctx, 5);
        uint32_t next = getRegU32(ctx, 6);
        info.arg = getRegU32(ctx, 7);
        info.gp = getRegU32(ctx, 28);
        info.sp = getRegU32(ctx, 29);
        info.enabled = true;

        int handlerId = 0;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            info.order = (next == 0) ? --g_dmac_head_order : ++g_dmac_tail_order;
            handlerId = g_nextDmacHandlerId++;
            info.id = handlerId;
            g_dmacHandlers[handlerId] = info;
        }
        setReturnS32(ctx, handlerId);
    }

    void AddDmacHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddDmacHandler(rdram, ctx, runtime);
    }

    void RemoveDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        if (handlerId > 0)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            auto it = g_dmacHandlers.find(handlerId);
            if (it != g_dmacHandlers.end() && it->second.cause == cause)
            {
                g_dmacHandlers.erase(it);
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_intcHandlers.find(handlerId); it != g_intcHandlers.end())
            {
                it->second.enabled = true;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_intcHandlers.find(handlerId); it != g_intcHandlers.end())
            {
                it->second.enabled = false;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_dmacHandlers.find(handlerId); it != g_dmacHandlers.end())
            {
                it->second.enabled = true;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_dmacHandlers.find(handlerId); it != g_dmacHandlers.end())
            {
                it->second.enabled = false;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void EnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            g_enabled_dmac_mask |= (1u << cause);
        }
        setReturnS32(ctx, KE_OK);
    }

    void iEnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableDmac(rdram, ctx, runtime);
    }

    void DisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            g_enabled_dmac_mask &= ~(1u << cause);
        }
        setReturnS32(ctx, KE_OK);
    }

    void iDisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableDmac(rdram, ctx, runtime);
    }
}
