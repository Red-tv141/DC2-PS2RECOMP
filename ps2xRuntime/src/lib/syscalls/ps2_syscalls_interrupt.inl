// ===== G657 P1: 27 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
namespace
{
    constexpr uint32_t kIntcVblankStart = 2u;
    constexpr uint32_t kIntcVblankEnd = 3u;
    constexpr auto kVblankPeriod = std::chrono::microseconds(16667);
    constexpr int kMaxCatchupTicks = 4;

    struct VSyncFlagRegistration
    {
        uint32_t flagAddr = 0;
        uint32_t tickAddr = 0;
    };

    static std::mutex g_irq_handler_mutex;
    static std::mutex g_irq_worker_mutex;
    static std::condition_variable g_irq_worker_cv;
    static std::mutex g_vsync_flag_mutex;
    static std::condition_variable g_vsync_cv;
    static std::atomic<bool> g_irq_worker_stop{false};
    static std::atomic<bool> g_irq_worker_running{false};
    static uint32_t g_enabled_intc_mask = 0xFFFFFFFFu;
    static uint32_t g_enabled_dmac_mask = 0xFFFFFFFFu;
    static uint64_t g_vsync_tick_counter = 0u;
    static VSyncFlagRegistration g_vsync_registration{};
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
                // Interrupt handlers must be able to preempt a guest thread that is
                // spinning on interrupt-produced state, such as a vblank counter.
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
                std::cerr << "[INTC] handler 0x" << std::hex << info.handler
                          << " threw exception: " << e.what() << std::dec << std::endl;
                ++warnCount;
            }
        }
    }
}


// G375: pending DMAC completion causes (bitmask), drained on the vblank tick.
static std::mutex g_dmac_pending_mutex;
static uint32_t g_dmac_pending_causes = 0u;

// G375: deferred DMAC completion interrupts.
// libdma kicks a channel with TIE=1 and the runtime completes the transfer
// synchronously inside sceDmaSend. Dispatching the guest's DMAC handler right there
// is WRONG in ordering: DC2's FMV kicks the image DMA from vblankHandler and only
// ARMS its "image in flight" gate byte (gp-0x66EC) on the instruction AFTER the call
// returns, so an in-call handler always saw the gate clear and never ran
// voBufDecCount -> the movie's video-out ring never drained. On hardware the
// interrupt arrives later, so queue the cause here and dispatch it from the vblank
// interrupt worker, after the kicking guest code has returned.



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
            const uint64_t tickValue = signalVSyncFlag(rdram);
            ps2_stubs::dispatchGsSyncVCallback(rdram, runtime, tickValue);
            drainDmacCompletionIrqs(rdram, runtime); // G375
            dispatchIntcHandlersForCause(rdram, runtime, kIntcVblankStart);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            dispatchIntcHandlersForCause(rdram, runtime, kIntcVblankEnd);
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
























