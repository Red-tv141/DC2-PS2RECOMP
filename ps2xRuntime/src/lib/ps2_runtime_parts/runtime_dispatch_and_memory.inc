void PS2Runtime::dispatchLoop(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t lastPc = std::numeric_limits<uint32_t>::max();
    uint32_t samePcCount = 0;
    constexpr uint32_t kSamePcYieldInterval = 0x4000u;

    g_g58Rdram = rdram; // G58: enable the 0x1fff800 saved-$ra watch in lookupFunction.

    while (!isStopRequested())
    {
        const uint32_t pc = ctx->pc;

        if (pc == lastPc)
        {
            ++samePcCount;
            if ((samePcCount % kSamePcYieldInterval) == 0u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    RUNTIME_LOG("CPU is doing some work at PC 0x" << std::hex << pc << ". PC not updating.");
                });
                std::this_thread::yield();
            }
        }
        else
        {
            samePcCount = 0;
            lastPc = pc;
        }

        m_debugPc.store(pc, std::memory_order_relaxed);
        m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)), std::memory_order_relaxed);
        m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)), std::memory_order_relaxed);
        m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)), std::memory_order_relaxed);

        RecompiledFunction fn = lookupFunction(pc);
        const uint32_t dispatchedPc = pc;
        const uint32_t dispatchedRa = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));

        {
            GuestExecutionScope guestExecution(this);
            fn(rdram, ctx, this);
        }

        // G58: capture the function whose execution left pc=0x9f84a0 (the robust title crash).
        static const bool s_g58bad = (std::getenv("DC2_TRACE_G58") != nullptr);
        if (s_g58bad && ctx->pc == 0x009F84A0u)
        {
            static int s_g58BadHits = 0;
            if (s_g58BadHits < 4)
            {
                ++s_g58BadHits;
                const uint32_t r31 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
                const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
                std::cerr << "[G58:badjump] fn@0x" << std::hex << dispatchedPc
                          << " (enteredRa=0x" << dispatchedRa << ") left pc=0x9f84a0"
                          << " r31=0x" << r31 << " sp=0x" << sp
                          << " *(sp+0xA0)=0x" << readGuestU32Wrapped(rdram, sp + 0xA0u)
                          << std::dec << std::endl;
            }
        }

        if (ctx->pc == 0u)
        {
            const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
            const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
            const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
            std::cerr << "[dispatch:pc-zero] from=0x" << std::hex << dispatchedPc
                      << " fromRa=0x" << dispatchedRa
                      << " ra=0x" << ra
                      << " sp=0x" << sp
                      << " gp=0x" << gp
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;

            // PC=0 means this guest thread returned (usually via jr $ra with RA=0).
            // Do not request a global runtime stop here: other guest threads may still run.
            break;
        }
    }
}

void PS2Runtime::enterGuestExecution()
{
    m_guestExecutionWaiters.fetch_add(1u, std::memory_order_acq_rel);
    m_guestExecutionMutex.lock();
    m_guestExecutionWaiters.fetch_sub(1u, std::memory_order_acq_rel);
    ++g_guestExecutionDepths[this];
}

void PS2Runtime::leaveGuestExecution()
{
    auto it = g_guestExecutionDepths.find(this);
    if (it == g_guestExecutionDepths.end() || it->second == 0u)
    {
        return;
    }

    --it->second;
    m_guestExecutionMutex.unlock();
    if (it->second == 0u)
    {
        g_guestExecutionDepths.erase(it);
    }
}

uint32_t PS2Runtime::releaseGuestExecution()
{
    auto it = g_guestExecutionDepths.find(this);
    if (it == g_guestExecutionDepths.end() || it->second == 0u)
    {
        return 0u;
    }

    const uint32_t depth = it->second;
    for (uint32_t i = 0; i < depth; ++i)
    {
        m_guestExecutionMutex.unlock();
    }
    g_guestExecutionDepths.erase(it);
    return depth;
}

void PS2Runtime::reacquireGuestExecution(uint32_t depth)
{
    if (depth == 0u)
    {
        return;
    }

    uint32_t &heldDepth = g_guestExecutionDepths[this];
    for (uint32_t i = 0; i < depth; ++i)
    {
        m_guestExecutionWaiters.fetch_add(1u, std::memory_order_acq_rel);
        m_guestExecutionMutex.lock();
        m_guestExecutionWaiters.fetch_sub(1u, std::memory_order_acq_rel);
        ++heldDepth;
    }
}

// G57: scoped back-edge preemption suppression. The title-frame draw runs through
// override functions (f48_title_map_draw_probe / g56_begindraw_tap) that call the
// recompiled bodies DIRECTLY. When a recompiled loop yields mid-iteration via
// shouldPreemptGuestExecution() it returns a mid-function resume PC; the normal
// dispatch loop re-enters via the registered resume-PC table, but an override that
// invokes the body as a plain C++ call swallows that resume — leaving e.g.
// BeginDraw__14mgCDrawManager half-initialized (mgr[4]/mgr[6] null) so the deferred
// flush dereferences null and crashes. Suppressing preemption around the title draw
// makes those bodies run to completion in a single call. Scoped (RAII inc/dec around
// the title draw only), so other guest threads are unaffected outside that window.
std::atomic<int> g_dc2PreemptSuppressDepth{0};

// PHASE G214: extern-visible mirror of the host present-loop tick (g_g189PresentTick lives
// in an anonymous namespace and cannot be externed). Lets a game-override diagnostic probe
// correlate an event (e.g. a collapsed skin matrix) with the dumped frame_NNNNNN.ppm number.
std::atomic<uint64_t> g_dc2PresentTick{0};

bool PS2Runtime::shouldPreemptGuestExecution()
{
    // G57 experiment: allow disabling back-edge preemption entirely to confirm
    // a mid-loop preemption return is the corruption source. Env-gated.
    static const bool s_noPreempt = [] {
        const char *v = std::getenv("DC2_G57_NO_PREEMPT");
        return v && v[0] && v[0] != '0';
    }();
    if (s_noPreempt)
        return false;

    // G57 fix: scoped suppression set by the title-draw override.
    if (g_dc2PreemptSuppressDepth.load(std::memory_order_relaxed) > 0)
        return false;

    thread_local uint32_t s_backEdgeYieldCounter = 0u;
    const uint32_t waiterCount = m_guestExecutionWaiters.load(std::memory_order_acquire);
    const uint32_t yieldInterval = (waiterCount != 0u) ? 64u : 100u;
    if (++s_backEdgeYieldCounter < yieldInterval)
    {
        return false;
    }

    s_backEdgeYieldCounter = 0u;
    return true;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read32(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read64(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    try
    {
        m_memory.write32(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    ps2_syscalls::notifyRuntimeStop();
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

// F50.3: registry of live guest worker-thread contexts (external linkage; no header
// edit). Worker contexts are stack-locals inside StartThread's lambda, so the host
// hang watchdog cannot otherwise see them. StartThread registers/unregisters each
// worker here; the DC2_TRACE_HANG watchdog in run() samples every thread's live
// per-instruction PC to pinpoint an intra-function (no-call) spin.
std::mutex g_guestThreadCtxMutex;
std::unordered_map<int, R5900Context *> g_guestThreadCtxs;

void PS2Runtime::run()
{
    m_stopRequested.store(false, std::memory_order_relaxed);
    ps2_stubs::resetSifState();
    ps2_syscalls::resetSoundDriverRpcState();
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetGsSyncVCallbackState();
    ps2_stubs::resetMpegStubState();
    ps2_syscalls::initializeGuestKernelState(m_memory.getRDRAM());
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    g_activeThreads.store(1, std::memory_order_relaxed);
    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            dispatchLoop(m_memory.getRDRAM(), &m_cpuContext);
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        g_activeThreads.fetch_sub(1, std::memory_order_relaxed);
        gameThreadFinished.store(true, std::memory_order_release); });

    ps2_syscalls::EnsureVSyncWorkerRunning(m_memory.getRDRAM(), this);

    g189StartWatchdogOnce();
    uint64_t tick = 0;
    while (!isStopRequested() && g_activeThreads.load(std::memory_order_relaxed) > 0)
    {
        tick++;
        g_g189PresentTick.store(tick, std::memory_order_relaxed);
        g_dc2PresentTick.store(tick, std::memory_order_relaxed);
        g_g189PresentStage.store(0, std::memory_order_relaxed);
        PS2_IF_AGRESSIVE_LOGS({
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const int activeThreads = g_activeThreads.load(std::memory_order_relaxed);

                std::cout << "[run:tick] tick=" << tick
                          << " pc=0x" << std::hex << dbgPc
                          << " ra=0x" << dbgRa
                          << " sp=0x" << dbgSp
                          << " gp=0x" << dbgGp
                          << " dispfb1=0x" << gs.dispfb1
                          << " display1=0x" << gs.display1
                          << std::dec
                          << " activeThreads=" << activeThreads
                          << " dma=" << curDma
                          << " gif=" << curGif
                          << " gsw=" << curGs
                          << " vif=" << curVif
                          << std::endl;
            }
        });
        // F50.2: env-gated hang watchdog. The game thread runs dispatchLoop; when a
        // recompiled function spins on an internal backward branch (no sub-calls), it
        // never returns to dispatchLoop, so m_debugPc freezes at that function's entry
        // PC. Sample it here on the host present-thread to surface the spinning function.
        {
            static const bool s_hangWatch = (std::getenv("DC2_TRACE_HANG") != nullptr);
            if (s_hangWatch && (tick % 240) == 0)
            {
                static uint32_t s_lastPc = 0xFFFFFFFFu;
                static uint32_t s_freeze = 0u;
                // Sample the LIVE per-instruction PC (recomp sets ctx->pc before every
                // instruction). m_debugPc only updates at dispatchLoop granularity, which
                // is useless here because recomp functions nest inline (the whole game
                // runs as one deep C++ call tree from the entry dispatch). Reading the
                // live context PC pinpoints an intra-function (no-call) spin exactly.
                const uint32_t livePc = m_cpuContext.pc;
                const uint32_t wpc = m_debugPc.load(std::memory_order_relaxed);
                if (livePc == s_lastPc) { ++s_freeze; } else { s_freeze = 0u; s_lastPc = livePc; }
                std::cerr << "[F50.2:watch] tick=" << std::dec << tick
                          << " livePc=0x" << std::hex << livePc
                          << " dispPc=0x" << wpc
                          << " ra=0x" << m_debugRa.load(std::memory_order_relaxed)
                          << " sp=0x" << m_debugSp.load(std::memory_order_relaxed)
                          << " gp=0x" << m_debugGp.load(std::memory_order_relaxed)
                          << std::dec << " freezeN=" << s_freeze << std::endl;

                // F50.3: also sample every live guest worker thread's per-instruction PC.
                // The spinning worker (sound/ADX thread) holds a stack-local context that
                // m_cpuContext/m_debugPc never see; this exposes its exact spin instruction.
                std::lock_guard<std::mutex> wlock(g_guestThreadCtxMutex);
                for (auto &kv : g_guestThreadCtxs)
                {
                    if (kv.second == nullptr) continue;
                    std::cerr << "[F50.3:thread] tick=" << std::dec << tick
                              << " tid=" << kv.first
                              << " pc=0x" << std::hex << kv.second->pc
                              << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(kv.second->r[31], 0))
                              << " sp=0x" << static_cast<uint32_t>(_mm_extract_epi32(kv.second->r[29], 0))
                              << " a0=0x" << static_cast<uint32_t>(_mm_extract_epi32(kv.second->r[4], 0))
                              << " a1=0x" << static_cast<uint32_t>(_mm_extract_epi32(kv.second->r[5], 0))
                              << std::dec << std::endl;
                }
            }
        }
        // G167 (MEASURE ONLY, DC2_G167_STARVE_MAIN=1, default off): skip the main thread's own
        // GPU work (UploadFrame's UpdateTexture + DrawTexturePro) to test whether G165's
        // cross-thread-GPU-contention hypothesis for the T8 GPU-decoder thread is correct -- if
        // the decoder's draw/readback times (DC2_G164_PROFILE) drop when the main thread stops
        // touching the GPU, the hypothesis is confirmed; if unchanged, something else dominates.
        // Diagnostic only, never shipped as a real presentation path -- window goes black/frozen
        // while set. See plans/phase-G167-fix-log.md.
        static const bool s_g167StarveMain = (std::getenv("DC2_G167_STARVE_MAIN") != nullptr);
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        g_g189PresentStage.store(1, std::memory_order_relaxed);
        if (!s_g167StarveMain)
            UploadFrame(frameTex, this, presentWidth, presentHeight);
        g_g189PresentStage.store(2, std::memory_order_relaxed);

        BeginDrawing();
        ClearBackground(BLACK);
        g_g189PresentStage.store(3, std::memory_order_relaxed);
        if (!s_g167StarveMain)
        {
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
        const float dstWidth = srcWidth * scale;
        const float dstHeight = srcHeight * scale;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        }

        if (dc2HudEnabled())
        {
            const int hudX = 8;
            const int hudY = 8;
            DrawRectangle(hudX - 4, hudY - 4, 320, 70, Fade(BLACK, 0.55f));

            char line1[64];
            char line2[128];
            char line3[64];
            std::snprintf(line1, sizeof(line1), "tick=%llu",
                          static_cast<unsigned long long>(tick));
            std::snprintf(line2, sizeof(line2),
                          "dma=%llu gif=%llu gsw=%llu vif=%llu",
                          static_cast<unsigned long long>(m_memory.dmaStartCount()),
                          static_cast<unsigned long long>(m_memory.gifCopyCount()),
                          static_cast<unsigned long long>(m_memory.gsWriteCount()),
                          static_cast<unsigned long long>(m_memory.vifWriteCount()));
            std::snprintf(line3, sizeof(line3), "threads=%d",
                          g_activeThreads.load(std::memory_order_relaxed));

            DrawText(line1, hudX, hudY, 20, RAYWHITE);
            DrawText(line2, hudX, hudY + 20, 18, LIME);
            DrawText(line3, hudX, hudY + 42, 18, SKYBLUE);
        }

        static const bool s_scpathSummary = (std::getenv("DC2_TRACE_SYSCALL_PATH") != nullptr);
        if (s_scpathSummary && (tick != 0) && (tick % 120 == 0)) {
            std::cerr << "[SCPATH:summary] tick=" << tick
                      << " enc=" << g_scpath_enc.load(std::memory_order_relaxed)
                      << " v1=" << g_scpath_v1.load(std::memory_order_relaxed)
                      << " todo=" << g_scpath_todo.load(std::memory_order_relaxed)
                      << std::endl;
        }

        static bool s_forceBlank = false;
        if (IsKeyPressed(KEY_F11)) {
            s_forceBlank = !s_forceBlank;
        }
        static const uint64_t s_frameDumpEvery = []() -> uint64_t {
            const char *e = std::getenv("DC2_FRAME_DUMP_EVERY");
            const long v = e ? std::strtol(e, nullptr, 10) : 60;
            return v > 0 ? static_cast<uint64_t>(v) : 60ull;
        }();
        const bool periodic = dc2FrameDumpEnabled() && (tick != 0) && (tick % s_frameDumpEvery == 0);
        const bool manual = IsKeyPressed(KEY_F12);
        if (periodic || manual) {
            dc2::dumpFramePpm(tick, s_forceBlank || manual, this);
            // F59: correlate each dumped frame with the dungeon render state so the
            // bright(event)->dim(post-event) capture transition can be tied to
            // DngStatus / DngMainMap / lightScale. Gated on DC2_TRACE_F59.
            static const bool s_f59 = (std::getenv("DC2_TRACE_F59") != nullptr);
            if (s_f59) {
                uint8_t *rd = m_memory.getRDRAM();
                auto rd32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                const int32_t status = (int32_t)rd32(0x01E9F6E0u);
                const uint32_t dngMap = rd32(0x003772A4u);
                const uint32_t bas = rd32(0x003772A0u);
                const uint32_t scene = rd32(0x0037729Cu);
                const uint32_t scaleU = bas ? rd32(bas + 0x6cu) : 0u;
                float scale; std::memcpy(&scale, &scaleU, 4);
                const uint32_t flags = bas ? rd32(bas + 0x08u) : 0u;
                const int32_t mapIdx = scene ? (int32_t)rd32(scene + 0x2e5cu) : 0xDEAD;
                std::fprintf(stderr,
                    "[F59:dump] tick=%llu DngStatus=%d DngMainMap=0x%x scale=%.3f flags=0x%x mapIdx=%d\n",
                    (unsigned long long)tick, status, dngMap, scale, flags, mapIdx);
            }
            // G234: deferred GS-dump start — close the g138 dump gate at boot when
            // DC2_G138_GSDUMP_AT_TICK is set, open it once the present tick reaches
            // the requested value (dump then runs until DC2_G138_GSDUMP_MAX records).
            {
                extern std::atomic<bool> g_dc2G138DumpGateOpen;
                static const long s_gsdumpAtTick = []() -> long {
                    const char *e = std::getenv("DC2_G138_GSDUMP_AT_TICK");
                    const long v = e ? std::strtol(e, nullptr, 10) : -1;
                    if (v >= 0)
                        g_dc2G138DumpGateOpen.store(false, std::memory_order_relaxed);
                    return v;
                }();
                if (s_gsdumpAtTick >= 0 && (long)tick >= s_gsdumpAtTick &&
                    !g_dc2G138DumpGateOpen.load(std::memory_order_relaxed)) {
                    g_dc2G138DumpGateOpen.store(true, std::memory_order_relaxed);
                    std::fprintf(stderr, "[G234:gsdump] gate OPEN at tick=%llu\n",
                                 (unsigned long long)tick);
                }
            }
            // G234: dump the mgRenderInfo active light set (dir/color matrices, ambient,
            // point lights) on each frame-dump tick for A/B vs a paused PCSX2 at the same
            // scene. Gated on DC2_G234_LIGHT.
            static const bool s_g234 = (std::getenv("DC2_G234_LIGHT") != nullptr);
            if (s_g234) {
                uint8_t *rd = m_memory.getRDRAM();
                auto rd32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                auto rdf = [&](uint32_t a){ uint32_t u = rd32(a); float f; std::memcpy(&f, &u, 4); return f; };
                const uint32_t ri = rd32(0x00382144u);
                if (ri != 0) {
                    const uint32_t idx = rd32(ri + 0x3F4u);
                    std::fprintf(stderr, "[G234:light] tick=%llu ri=0x%x activeSet=%u\n",
                                 (unsigned long long)tick, ri, idx);
                    for (uint32_t s = 0; s < 8u; ++s) {
                        const uint32_t b = ri + 0x400u + s * 0x150u;
                        std::fprintf(stderr,
                            "[G234:light]   set %u dir0=(%.2f,%.2f,%.2f,%.2f) col0=(%.2f,%.2f,%.2f,%.2f) amb=(%.2f,%.2f,%.2f,%.2f) pl0col=(%.2f,%.2f,%.2f,%.2f) pl0pos=(%.2f,%.2f,%.2f,%.2f)\n",
                            s,
                            rdf(b), rdf(b+4u), rdf(b+8u), rdf(b+12u),
                            rdf(b+0x40u), rdf(b+0x44u), rdf(b+0x48u), rdf(b+0x4Cu),
                            rdf(b+0x80u), rdf(b+0x84u), rdf(b+0x88u), rdf(b+0x8Cu),
                            rdf(b+0x90u), rdf(b+0x94u), rdf(b+0x98u), rdf(b+0x9Cu),
                            rdf(b+0xA0u), rdf(b+0xA4u), rdf(b+0xA8u), rdf(b+0xACu));
                    }
                    std::fflush(stderr);
                }
            }
        }

        // F60: sample the poll/wait-primitive counters from the host present
        // loop (which keeps running even while the guest EE thread is stalled in
        // a busy-wait). Whichever delta explodes identifies the spin. Throttled
        // to ~every 30 present frames (~0.5s). Gated DC2_TRACE_F60.
        {
            static const bool s_f60 = (std::getenv("DC2_TRACE_F60") != nullptr);
            if (s_f60) {
                static uint64_t s_f60LastTick = 0;
                static uint64_t s_f60Prev[5] = {0, 0, 0, 0, 0};
                if (tick - s_f60LastTick >= 30) {
                    s_f60LastTick = tick;
                    const uint64_t cur[5] = {
                        g_f60_checkStatRpc.load(std::memory_order_relaxed),
                        g_f60_pollSema.load(std::memory_order_relaxed),
                        g_f60_dmaStat.load(std::memory_order_relaxed),
                        g_f60_gsSyncV.load(std::memory_order_relaxed),
                        g_f60_waitVSyncTick.load(std::memory_order_relaxed),
                    };
                    uint8_t *rd = m_memory.getRDRAM();
                    int32_t status;
                    std::memcpy(&status, rd + (0x01E9F6E0u & 0x01FFFFFFu), 4);
                    // Sample the EE context PC twice ~1ms apart: if it does not
                    // move while the render is frozen, the guest EE thread is
                    // pinned in a pure-memory busy-wait at this PC (no syscall);
                    // if it moves, the loop is calling something each iteration.
                    const uint32_t pcA = m_cpuContext.pc;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    const uint32_t pcB = m_cpuContext.pc;
                    // CAutoMapGen@0x1ea0480 procedural-map state. If tmplCnt==0 /
                    // template[0].parts==0, CreatRoom can never succeed and
                    // RandomMapMainProc's do/while(iVar5==0) spins forever.
                    auto rd32b = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                    const uint32_t tmplCnt = rd32b(0x1ea0480u + 0x1c8u);
                    // newlib rand seed lives at *(_impure_ptr)+0x58; _impure_ptr
                    // global = 0x333b84. If the seed does not advance between two
                    // host samples while the generator spins, rand() is stuck
                    // (constant) => iRand constant => every room places at the same
                    // cell => CreatRoom overlap-fails forever.
                    const uint32_t impurePtr = rd32b(0x333b84u);
                    const uint32_t seedA = impurePtr ? rd32b(impurePtr + 0x58u) : 0xDEADu;
                    // (the 1ms sleep already happened between pcA and pcB above)
                    const uint32_t seedB = impurePtr ? rd32b(impurePtr + 0x58u) : 0xDEADu;
                    std::fprintf(stderr,
                        "[F60:spin] tick=%llu status=%d pcA=0x%x pcB=0x%x moved=%d | dCheckRpc=%lld dPollSema=%lld dDmaStat=%lld dGsSyncV=%lld dWaitVS=%lld | tmplCnt=%u seedMoved=%d\n",
                        (unsigned long long)tick, status, pcA, pcB, (pcA != pcB) ? 1 : 0,
                        (long long)(cur[0]-s_f60Prev[0]), (long long)(cur[1]-s_f60Prev[1]),
                        (long long)(cur[2]-s_f60Prev[2]), (long long)(cur[3]-s_f60Prev[3]),
                        (long long)(cur[4]-s_f60Prev[4]),
                        tmplCnt, (seedA != seedB) ? 1 : 0);
                    for (int i = 0; i < 5; ++i) s_f60Prev[i] = cur[i];
                }
            }
        }

        // F64: watch the costume-select load phase (KeyStep@0x2bcc60 gates input on
        // MenuCosutumeLoadPhase; must reach its settled value before the menu accepts
        // a press, and resets to 1 on each costume reload). Gated DC2_TRACE_F64.
        {
            static const bool s_f64c = (std::getenv("DC2_TRACE_F64") != nullptr);
            if (s_f64c) {
                static uint64_t s_f64cLast = 0;
                static uint32_t s_f64cPrev = 0xFFFFFFFFu;
                uint8_t *rd = m_memory.getRDRAM();
                auto c32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                const uint32_t ph = c32(0x01ecd62cu);
                if (ph != s_f64cPrev || tick - s_f64cLast >= 30) {
                    s_f64cLast = tick; s_f64cPrev = ph;
                    std::fprintf(stderr, "[F64:costume] tick=%llu ph62c=%u ph630=%u ph628=%u ca5c=%u\n",
                                 (unsigned long long)tick, ph, c32(0x01ecd630u), c32(0x01ecd628u), c32(0x01ecca5cu));
                }
            }
        }

        // F64 FIX: the dungeon-entrance EVENT (DngStatus==2) never completes headless,
        // G225 (2026-07-12) CORRECTION of the F64 story: the entrance script's generic
        // wait sub-function (strBase+0x378) is NOT an infinite "wait until the director
        // skips me" loop — it is a plain per-frame COUNTDOWN (op-0x7 SUB: var0 -= 1,
        // exit when var0 < 0; decoded live from PCSX2 guest memory). The whole entrance
        // cutscene runs to its final dialogue box naturally with no audio at all
        // (verified: forge-off runner frame matches ref/dumps/dungeon_0_cutscene_end.png
        // pixel-for-pixel in composition). F64's 120-iteration (~2s) done==0 detector
        // fired long before any multi-thousand-tick countdown could finish, which is what
        // truncated the cutscene and dropped the player at the staging position (G224
        // post-fix symptom). The only genuinely indefinite park is a message-window /
        // end-of-script wait (real PCSX2 parks at the same vmcode offset +0x40E0 with
        // done==0 until player input).
        //
        // So the stall detector is now PROGRESS-AWARE: it only counts consecutive present
        // iterations where the script's current vmcode_t* (+0x38) has NOT moved. Any
        // advance resets the counter. Threshold default 10800 iterations (longest
        // legitimate frozen-vmcode park measured on this route is ~3600, the countdown);
        // override via DC2_F64_STALL_TICKS (e.g. =120 for the old fast-skip behavior in
        // headless harnesses that only need free-roam). The forge action itself is
        // unchanged (halt VM + done=1 + RunMainEvent completion block — see F64 fix-log
        // for why the subtler alternatives failed).
        // Default ON; set DC2_DISABLE_EVENT_SKIP=1 to A/B the raw behavior.
        {
            static const bool s_f64disabled = (std::getenv("DC2_DISABLE_EVENT_SKIP") != nullptr);
            static const bool s_f64log = (std::getenv("DC2_TRACE_F64") != nullptr);
            if (!s_f64disabled) {
                uint8_t *rd = m_memory.getRDRAM();
                auto rr = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                auto wr = [&](uint32_t a, uint32_t v){ std::memcpy(rd + (a & 0x01FFFFFFu), &v, 4); };
                const int32_t status = (int32_t)rr(0x01E9F6E0u);
                const uint32_t script = 0x01ece3d0u;
                const uint32_t done   = rr(script + 0x3cu);     // DAT_01ece40c
                const uint32_t dngMap = rr(0x003772A4u);        // DngMainMap (floor loaded)
                // Gate to the DUNGEON ENTRANCE: map loaded (DngMainMap != 0). status==2
                // alone also matches the intro/menu events that run BEFORE the map loads.
                const bool stuckEvent = (status == 2 && dngMap != 0u && done == 0u);
                static uint32_t s_f64stuck = 0u;
                static bool s_f64forced = false;
                static uint32_t s_g225LastVm = 0u;
                static const uint32_t s_g225Threshold = [](){
                    const char *e = std::getenv("DC2_F64_STALL_TICKS");
                    if (e) { long v = std::strtol(e, nullptr, 10); if (v > 0) return (uint32_t)v; }
                    return 10800u; }();
                const uint32_t vm = rr(script + 0x38u);
                if (stuckEvent) {
                    // G225: count only while the script makes NO progress (vmcode frozen).
                    if (vm == s_g225LastVm) ++s_f64stuck; else s_f64stuck = 0u;
                    s_g225LastVm = vm;
                } else { s_f64stuck = 0u; s_g225LastVm = 0u; s_f64forced = false; }
                if (stuckEvent && s_f64stuck >= s_g225Threshold && !s_f64forced) {
                    s_f64forced = true;
                    // Halt the event script VM the way its own op-0xf end opcode does
                    // (current vmcode_t* +0x38 = 0, done +0x3c = 1, return latch = 0)...
                    wr(script + 0x38u, 0u);   // VM halts (resume__10CRunScript no-ops)
                    wr(script + 0x3cu, 1u);   // DAT_01ece40c = done
                    wr(0x01ece4fcu, 0u);      // DAT_01ece4fc return latch clear
                    // ...then reproduce RunMainEvent@0x1D1360's "EventLoop returned 1"
                    // completion block directly (DngStatus=0 + scene reset), since with
                    // the script halted EventLoop should return 1 but does not settle
                    // here headless. This is the documented free-roam-entry state.
                    wr(0x01E9F6E0u, 0u);      // DngStatus = 0 (free-roam)
                    const uint32_t bas = rr(0x003772A0u); // BattleAreaScene
                    if (bas) {
                        std::memcpy(rd + ((bas + 0x46u) & 0x01FFFFFFu), "\0\0", 2); // +0x46 (u16) = 0
                        const uint32_t f = rr(bas + 0x08u) & 0xFFFFFBFFu;           // clear bit 0x400
                        wr(bas + 0x08u, f);
                    }
                    const uint32_t dms = rr(0x0037729Cu); // DngMainScene
                    if (dms) wr(dms + 0x2e54u, 0u);       // cam idx = 0
                    if (s_f64log)
                        std::fprintf(stderr, "[F64:done][G225] forcing event end (frozen=%u thr=%u vm=0x%x) bas=0x%x dms=0x%x -> DngStatus=0\n",
                                     s_f64stuck, s_g225Threshold, vm, bas, dms);
                }
            }
        }

        // F65: dim free-roam render. After F64 forces the entrance event to complete,
        // DngStatus reaches 0 but the frame is dim (~50000 nz vs the event's ~630000).
        // DngMainDraw@0x1CF090 draws the 3D world for DngStatus in {0,2,3} gated on
        // (DngMainMap!=0 && (BattleAreaScene+8 & 0x80)==0); sky on (& 0x20)==0; lighting
        // scaled by BattleAreaScene+0x6c; camera = GetCamera(DngMainScene, +0x2e54);
        // map = GetMap(DngMainScene, +0x2e5c). Real-HW free-roam (PCSX2): flags=0x0,
        // light=1.0, cam=0, mapIdx=0. Dump the runner's values at status 0 to A/B.
        // Gated DC2_TRACE_F65.
        {
            static const bool s_f65 = (std::getenv("DC2_TRACE_F65") != nullptr);
            if (s_f65) {
                static uint64_t s_f65Last = 0;
                uint8_t *rd = m_memory.getRDRAM();
                auto g32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                const int32_t status = (int32_t)g32(0x01E9F6E0u);
                if (status == 0 && tick - s_f65Last >= 30) {
                    s_f65Last = tick;
                    const uint32_t bas = g32(0x003772A0u);
                    const uint32_t dms = g32(0x0037729Cu);
                    const uint32_t dmap = g32(0x003772A4u);
                    const uint32_t flags = bas ? g32(bas + 0x08u) : 0u;
                    const uint32_t f46 = bas ? (g32(bas + 0x44u) >> 16) : 0u;
                    uint32_t lu = bas ? g32(bas + 0x6cu) : 0u; float light; std::memcpy(&light, &lu, 4);
                    const int32_t cam = dms ? (int32_t)g32(dms + 0x2e54u) : -1;
                    const int32_t mapIdx = dms ? (int32_t)g32(dms + 0x2e5cu) : -1;
                    const uint32_t mapDrawn = dmap ? g32(dmap + 0xc4u) : 0u;
                    std::fprintf(stderr,
                        "[F65:dim] tick=%llu DngStatus=0 DngMainMap=0x%x flags=0x%x +0x46=0x%x light=%.3f cam=%d mapIdx=%d mapDrawn=%u\n",
                        (unsigned long long)tick, dmap, flags, f46, light, cam, mapIdx, mapDrawn);
                }
            }
        }

        // F66: free-roam player movement. After F65 the dungeon free-roam renders,
        // but injected movement input did not translate the player/camera. Real-HW
        // PCSX2 free-roam: MainChara(*0x003772C8) position @ +0xe0 = (785,12,3200),
        // vtable=0x3756f0; gates DAT_01e9f6e8=0, BattleAreaScene+8=0, +0x54=0.
        // Movement runs via RunScript__12CActionChara@DngMainKey gated on
        // (DAT_01e9f6e8==0 && DebugPause==0 && (BattleAreaScene+8 & 4)==0). Dump the
        // runner's MainChara ptr/pos + gates at status 0 and track the position delta
        // so we can tell "player frozen" from "player moves, camera doesn't follow".
        // Gated DC2_TRACE_F66.
        {
            static const bool s_f66 = (std::getenv("DC2_TRACE_F66") != nullptr);
            if (s_f66) {
                static uint64_t s_f66Last = 0;
                uint8_t *rd = m_memory.getRDRAM();
                auto g32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                auto gf  = [&](uint32_t a){ float f; std::memcpy(&f, rd + (a & 0x01FFFFFFu), 4); return f; };
                const int32_t status = (int32_t)g32(0x01E9F6E0u);
                if (status == 0 && tick - s_f66Last >= 15) {
                    s_f66Last = tick;
                    const uint32_t mc  = g32(0x003772C8u);   // MainChara
                    const uint32_t bas = g32(0x003772A0u);
                    const uint32_t e8  = g32(0x01E9F6E8u);
                    const uint32_t dbgP= g32(0x00377288u);   // DebugPause (gp-0x7268, gate @0x1d1444)
                    const uint32_t vt  = mc ? g32(mc + 0x00u) : 0u;
                    const float px = mc ? gf(mc + 0xe0u) : 0.f;
                    const float py = mc ? gf(mc + 0xe4u) : 0.f;
                    const float pz = mc ? gf(mc + 0xe8u) : 0.f;
                    const uint32_t flags = bas ? g32(bas + 0x08u) : 0u;
                    const uint32_t b54   = bas ? g32(bas + 0x54u) : 0u;
                    static float s_px=0,s_py=0,s_pz=0; static bool s_have=false;
                    float dx=px-s_px, dy=py-s_py, dz=pz-s_pz;
                    float dmag = s_have ? std::sqrt(dx*dx+dy*dy+dz*dz) : 0.f;
                    s_px=px; s_py=py; s_pz=pz; s_have=true;
                    // CGamePad@0x3d76e0 analog: GetLX reads +0xc, GetLY +0x10 (raw
                    // stick byte, centre 0x80). Confirms our scePad override reaches
                    // the object the player movement reads from.
                    const int32_t cgLX = (int32_t)g32(0x3D76E0u + 0xcu); // LX (PAD_STATUS+8)
                    const int32_t cgLY = (int32_t)g32(0x3D76E0u + 0x8u); // LY (PAD_STATUS+4)
                    std::fprintf(stderr,
                        "[F66:move] tick=%llu mc=0x%x vt=0x%x pos=(%.2f,%.2f,%.2f) dmove=%.3f | e8=0x%x dbgPause=0x%x flags=0x%x +0x54=0x%x cgLX=%d cgLY=%d\n",
                        (unsigned long long)tick, mc, vt, px, py, pz, dmag, e8, dbgP, flags, b54, cgLX, cgLY);
                }
            }
        }

        // F66: drive in-dungeon free-roam pad input from the present loop. The
        // title/menu input clock (mgEndFrame-override-derived scriptFrame) freezes
        // in-dungeon, so DC2_PAD_INPUT cannot reach free-roam; this additive path
        // uses DC2_DUNGEON_PAD with its own counter that starts at free-roam entry
        // (DngStatus==0 with the floor actually loaded) and ticks every present
        // frame. Inert unless DC2_DUNGEON_PAD is set.
        // G7: poll the live host controller (XInput via raylib) once per present
        // frame on this (raylib/main) thread and publish the snapshot the guest pad
        // read consumes. Inert (live disabled) when DC2_PAD_INPUT is set explicitly
        // or DC2_NO_XINPUT=1, so deterministic automation is unaffected.
        {
            if (g_g7_poll_live_pad_hook)
                g_g7_poll_live_pad_hook();
        }

        // G55: observe the title map's steady-state draw state from the present loop
        // (the TitleMapDraw@0x2a2280 override is only consulted on INDIRECT dispatch, so
        // it cannot sample per-frame). Reads TitleMap draw-list count + map-level box gate.
        // Gated DC2_TRACE_G53; inert otherwise.
        {
            if (g_g55_title_draw_probe_hook)
                g_g55_title_draw_probe_hook(m_memory.getRDRAM());
        }

        {
            uint8_t *rd = m_memory.getRDRAM();
            auto g32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
            const int32_t status = (int32_t)g32(0x01E9F6E0u);
            const uint32_t mc   = g32(0x003772C8u); // MainChara
            const uint32_t dmap = g32(0x003772A4u); // DngMainMap
            static uint32_t s_dframe = 0u;
            static bool s_freeroamSeen = false;
            const bool freeroam = (status == 0 && mc != 0u && dmap != 0u);
            if (freeroam)
            {
                if (!s_freeroamSeen) { s_freeroamSeen = true; s_dframe = 0u; }
                if (g_f66_drive_dungeon_pad_hook)
                    g_f66_drive_dungeon_pad_hook(s_dframe++);
            }
            else
            {
                s_freeroamSeen = false;
            }
        }

        // F63: dump the dungeon-entrance EVENT state machine during the freeze.
        // EventLoop@0x2555E0 parks on DAT_01ece504 (3=StreamOpenState/SIF sound
        // wait, 2=fade, 1=skip) and advances the script via resume__10CRunScript
        // gated by DAT_01ece500; the CRunScript done flag is DAT_01ece40c (+0x3c).
        // Gated DC2_TRACE_F63.
        {
            static const bool s_f63e = (std::getenv("DC2_TRACE_F63") != nullptr);
            if (s_f63e) {
                static uint64_t s_f63eLast = 0;
                if (tick - s_f63eLast >= 30) {
                    s_f63eLast = tick;
                    uint8_t *rd = m_memory.getRDRAM();
                    auto r32 = [&](uint32_t a){ uint32_t v; std::memcpy(&v, rd + (a & 0x01FFFFFFu), 4); return v; };
                    const int32_t status = (int32_t)r32(0x01E9F6E0u);
                    const uint32_t d500 = r32(0x01ece500u); // event sub-state
                    const uint32_t d504 = r32(0x01ece504u); // stream/fade wait selector
                    const uint32_t d508 = r32(0x01ece508u);
                    const uint32_t d40c = r32(0x01ece40cu); // CRunScript done flag (+0x3c)
                    const uint32_t d4fc = r32(0x01ece4fcu); // EventLoop return code latch
                    const uint32_t script = 0x01ece3d0u;
                    const uint32_t vmcode = r32(script + 0x38u); // current vmcode_t* (exe runs while != 0)
                    const uint32_t yield  = r32(script + 0x50u);
                    const uint32_t op0 = vmcode ? r32(vmcode + 0u) : 0u; // opcode
                    const uint32_t op1 = vmcode ? r32(vmcode + 4u) : 0u;
                    const uint32_t op2 = vmcode ? r32(vmcode + 8u) : 0u;
                    std::fprintf(stderr,
                        "[F63:evt] tick=%llu DngStatus=%d d500=%u d504=%u d508=0x%x done(d40c)=%u retLatch(d4fc)=0x%x vmcode=0x%x yield=0x%x op=[0x%x 0x%x 0x%x]\n",
                        (unsigned long long)tick, status, d500, d504, d508, d40c, d4fc, vmcode, yield, op0, op1, op2);
                    // One-shot script disassembly around the parked vmcode: each
                    // instruction is 0xc bytes [op,arg1,arg2]. Resolve opcode-0x13
                    // (call native) to a name via strBase=+0x48 (string pool).
                    static uint32_t s_f63PrevVm = 0;
                    static uint32_t s_f63Stable = 0;
                    static uint32_t s_f63DumpedVm = 0;
                    static uint32_t s_f63DumpCount = 0;
                    if (vmcode != 0 && vmcode == s_f63PrevVm && d40c == 0)
                        ++s_f63Stable;
                    else
                        s_f63Stable = 0;
                    s_f63PrevVm = vmcode;
                    // Dump once per NEWLY-stable park location (stable >= 6 samples
                    // == ~180 ticks of an unchanging script pc == the real stall, not
                    // a transient). Re-fires if the park moves, capped at 4 dumps.
                    const bool parked = (s_f63Stable >= 6u);
                    if (parked && vmcode != s_f63DumpedVm && status == 2 && tick > 600 && s_f63DumpCount < 4u) {
                        s_f63DumpedVm = vmcode;
                        ++s_f63DumpCount;
                        const uint32_t strBase = r32(script + 0x48u);
                        const uint32_t cmdTab  = r32(script + 0x08u); // ext-command fn table
                        std::fprintf(stderr, "[F63:script] vmcode=0x%x strBase=0x%x funcTab=0x%x cmdTab=0x%x\n",
                                     vmcode, strBase, r32(script + 0x34u), cmdTab);
                        // Track the first int pushed in the current arg group so an
                        // op-0x15 (ext) can be resolved to its command id (= first
                        // pushed int) and native fn (= cmdTab[id]). F64.
                        uint32_t groupFirstInt = 0xffffffffu;
                        bool haveGroupFirst = false;
                        for (uint32_t i = 0; i < 48u; ++i) {
                            const uint32_t a = vmcode + i * 0xcu;
                            const uint32_t o = r32(a), q1 = r32(a + 4u), q2 = r32(a + 8u);
                            char nm[64]; nm[0] = 0;
                            // op3/a1==3 = push_str(strBase+arg2).
                            if (o == 0x3u && q1 == 3u && strBase) {
                                const uint32_t fd = strBase + q2;
                                for (uint32_t k = 0; k < 32u; ++k) {
                                    uint8_t ch = rd[(fd + k) & 0x01FFFFFFu];
                                    if (ch < 0x20 || ch > 0x7e) { nm[k] = 0; break; }
                                    nm[k] = (char)ch; nm[k+1] = 0;
                                }
                            }
                            // op3/a1==1 = push_int; remember first of the group.
                            if (o == 0x3u && q1 == 1u && !haveGroupFirst) {
                                groupFirstInt = q2; haveGroupFirst = true;
                            }
                            // op-0x15 = ext: id = first pushed int, fn = cmdTab[id].
                            if (o == 0x15u) {
                                const uint32_t id = haveGroupFirst ? groupFirstInt : 0xffffffffu;
                                const uint32_t fn = (cmdTab && id != 0xffffffffu) ? r32(cmdTab + id * 4u) : 0u;
                                std::snprintf(nm, sizeof(nm), "EXT id=0x%x fn=0x%x", id, fn);
                                haveGroupFirst = false; groupFirstInt = 0xffffffffu;
                            }
                            std::fprintf(stderr, "[F63:script]  +0x%02x a=0x%x op=0x%x a1=0x%x a2=0x%x %s\n",
                                         i * 0xcu, a, o, q1, q2, nm);
                        }
                        // F64: also dump the generic WAIT sub-function at
                        // strBase+0x378 (0x8ec4a8..0x8ec538, an op-0x10 back-jump
                        // loop) where the event blocks. Resolve its op-0x15 ext
                        // commands (the audio/condition query) and op-0x11/0x12
                        // conditional exit.
                        {
                            const uint32_t wbase = strBase + 0x378u;
                            std::fprintf(stderr, "[F64:wait] dumping wait loop @0x%x\n", wbase);
                            uint32_t gFirst = 0xffffffffu; bool haveG = false;
                            for (uint32_t i = 0; i < 24u; ++i) {
                                const uint32_t a = wbase + i * 0xcu;
                                const uint32_t o = r32(a), q1 = r32(a + 4u), q2 = r32(a + 8u);
                                char nm[64]; nm[0] = 0;
                                if (o == 0x3u && q1 == 3u && strBase) {
                                    const uint32_t fd = strBase + q2;
                                    for (uint32_t k = 0; k < 32u; ++k) {
                                        uint8_t ch = rd[(fd + k) & 0x01FFFFFFu];
                                        if (ch < 0x20 || ch > 0x7e) { nm[k] = 0; break; }
                                        nm[k] = (char)ch; nm[k+1] = 0;
                                    }
                                }
                                if (o == 0x3u && q1 == 1u && !haveG) { gFirst = q2; haveG = true; }
                                if (o == 0x15u) {
                                    const uint32_t id = haveG ? gFirst : 0xffffffffu;
                                    const uint32_t fn = (cmdTab && id != 0xffffffffu) ? r32(cmdTab + id * 4u) : 0u;
                                    std::snprintf(nm, sizeof(nm), "EXT id=0x%x fn=0x%x", id, fn);
                                    haveG = false; gFirst = 0xffffffffu;
                                }
                                if (o == 0x13u) { // script call: target = strBase + funcdata[0]
                                    const uint32_t fdp = strBase + q2;
                                    const uint32_t tgt = strBase + r32(fdp);
                                    std::snprintf(nm, sizeof(nm), "CALL ->0x%x", tgt);
                                }
                                std::fprintf(stderr, "[F64:wait]  +0x%02x a=0x%x op=0x%x a1=0x%x a2=0x%x %s\n",
                                             i * 0xcu, a, o, q1, q2, nm);
                            }
                        }
                    }
                }
            }
        }

        EndDrawing();

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();

    // G150 MTGS: drain + join the GS worker thread BEFORE the game thread join / GS teardown. Also
    // unblocks the game thread if it is parked in the frame-barrier backpressure wait. No-op unless
    // DC2_G150_MTGS=1. Declared here (external linkage, defined in ps2_gif_arbiter.cpp).
    extern void g150_shutdown();
    g150_shutdown();

    const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gameThreadFinished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < joinDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (gameThread.joinable())
    {
        if (gameThreadFinished.load(std::memory_order_acquire))
        {
            gameThread.join();
        }
        else
        {
            std::cerr << "[run] game thread did not stop within timeout; detaching" << std::endl;
            gameThread.detach();
        }
    }

    const auto workerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (g_activeThreads.load(std::memory_order_relaxed) > 0 &&
           std::chrono::steady_clock::now() < workerDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (g_activeThreads.load(std::memory_order_relaxed) > 0)
    {
        requestStop();
        const auto finalWorkerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (g_activeThreads.load(std::memory_order_relaxed) > 0 &&
               std::chrono::steady_clock::now() < finalWorkerDeadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (g_activeThreads.load(std::memory_order_relaxed) == 0)
    {
        ps2_syscalls::joinAllGuestHostThreads();
    }
    else
    {
        std::cerr << "[run] guest host threads did not stop within timeout; detaching remaining worker threads"
                  << std::endl;
        ps2_syscalls::detachAllGuestHostThreads();
    }

    UnloadTexture(frameTex);
    CloseWindow();

    if (std::getenv("DC2_TRACE_SYSCALL_PATH") != nullptr)
    {
        std::cerr << "[SCPATH:summary]"
                  << " enc=" << g_scpath_enc.load(std::memory_order_relaxed)
                  << " v1=" << g_scpath_v1.load(std::memory_order_relaxed)
                  << " todo=" << g_scpath_todo.load(std::memory_order_relaxed)
                  << std::endl;
    }

    const int remainingThreads = g_activeThreads.load(std::memory_order_relaxed);
    RUNTIME_LOG("[run] exiting loop, activeThreads=" << remainingThreads);
    if (remainingThreads > 0)
    {
        std::cerr << "[run] warning: " << remainingThreads
                  << " guest worker thread(s) still active during shutdown." << std::endl;
    }
}
