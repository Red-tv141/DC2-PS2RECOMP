static void f29_mgendframe_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // PHASE F30: green clear behind env-var gate. Default OFF so real title pixels survive.
    // Set $env:DC2_FORCE_VISIBLE='1' to restore F29 fallback for regression checks.
    static const bool forceVisible = []() {
        return dc2_env_flag_enabled("DC2_FORCE_VISIBLE");
    }();
    static const bool forceDrawBufferLatch = []() {
        return dc2_env_flag_enabled("DC2_FORCE_DRAW_BUFFER_LATCH");
    }();
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;

    // G141: measure-first perf instrumentation (skill 17-performance-optimization.md).
    // This probe fires once per guest frame (wraps mgEndFrame), so the wall-clock delta
    // between successive entries is the whole guest frame time (game logic + VU1 draw-list
    // build + previous frame's GS submit, all of which run BEFORE the next mgEndFrame call);
    // the time spent inside mgEndFrame itself is the packet-flush/present-adjacent tail.
    // Env-gated, aggregate-only print (DC2_PERF=1), per the lever/instrumentation doctrine.
    static const bool s_g141PerfOn = dc2_env_flag_enabled("DC2_PERF");
    static const bool s_g147PerfOn = dc2_env_flag_enabled("DC2_G147_PERF");
    static const bool s_g171PerfOn = dc2_env_flag_enabled("DC2_G171_STAT");
    // G182: EE thread onCPU%/frame -- is the measured VIF1(EE) wall time real compute or stall?
    static const bool s_g182PerfOn = dc2_env_flag_enabled("DC2_G182_EE_STAT");
    static const bool s_perfOn = s_g141PerfOn || dc2_env_flag_enabled("DC2_G146_PERF") || s_g147PerfOn || s_g171PerfOn || s_g182PerfOn;
    static std::chrono::steady_clock::time_point s_lastEntry{};
    static bool s_haveLast = false;
    static double s_sumFrameMs = 0.0;
    static double s_sumEndFrameMs = 0.0;
    static uint64_t s_lastEeCpuNs = 0ull;
    static double s_sumEeCpuMs = 0.0;
    static uint32_t s_perfWindow = 0u;
    // G141: read the global GS-raster + VU1-run ns accumulators (defined in
    // ps2_gs_rasterizer.cpp / ps2_vu1.cpp, declared at file scope below) to build a
    // NON-overlapping per-frame split. GS raster runs synchronously inside VU1 XGKICK, so
    // VU1-proper = vu1RunNs - gsRasterNs, and "other" (uploads/DMA/present/2D not nested in
    // VU1) = frame - vu1RunNs - (GS outside VU1).
    static uint64_t s_lastGsNs = 0ull, s_lastVu1Ns = 0ull;
    static uint64_t s_lastVif1Ns = 0ull, s_lastGifNs = 0ull, s_lastImageNs = 0ull, s_lastLocalNs = 0ull;
    static uint64_t s_lastG144MidNs = 0ull, s_lastG144UploadNs = 0ull, s_lastG144FrameNs = 0ull;
    static uint64_t s_lastG144MidCount = 0ull, s_lastG144UploadCount = 0ull, s_lastG144FrameCount = 0ull;
    static uint64_t s_lastG147PacketNs = 0ull, s_lastG147PacketCount = 0ull;
    static uint64_t s_lastG147DrawNs = 0ull, s_lastG147DrawCount = 0ull;
    static uint64_t s_lastG147Tags = 0ull, s_lastG147PackedRegs = 0ull, s_lastG147ReglistRegs = 0ull, s_lastG147ImageBytes = 0ull;
    static uint64_t s_lastG171RegNs[16] = {0};
    static uint64_t s_lastG171RegCount[16] = {0};
    static uint64_t s_lastG171AdNs[256] = {0};
    static uint64_t s_lastG171AdCount[256] = {0};
    static double s_sumGsMs = 0.0, s_sumVu1Ms = 0.0;
    static double s_sumVif1Ms = 0.0, s_sumGifMs = 0.0, s_sumImageMs = 0.0, s_sumLocalMs = 0.0;
    static double s_sumG144MidMs = 0.0, s_sumG144UploadMs = 0.0, s_sumG144FrameMs = 0.0;
    static uint64_t s_sumG144MidCount = 0ull, s_sumG144UploadCount = 0ull, s_sumG144FrameCount = 0ull;
    static double s_sumG147PacketMs = 0.0, s_sumG147DrawMs = 0.0;
    static uint64_t s_sumG147PacketCount = 0ull, s_sumG147DrawCount = 0ull;
    static uint64_t s_sumG147Tags = 0ull, s_sumG147PackedRegs = 0ull, s_sumG147ReglistRegs = 0ull, s_sumG147ImageBytes = 0ull;
    static uint64_t s_sumG171RegNs[16] = {0};
    static uint64_t s_sumG171RegCount[16] = {0};
    static uint64_t s_sumG171AdNs[256] = {0};
    static uint64_t s_sumG171AdCount[256] = {0};
    const auto perfEntryTs = std::chrono::steady_clock::now();
    if (s_perfOn)
    {
        if (s_haveLast)
        {
            s_sumFrameMs += std::chrono::duration<double, std::milli>(perfEntryTs - s_lastEntry).count();
            if (s_g182PerfOn)
            {
                const uint64_t eeCpuNow = g182ThreadCpuNs();
                s_sumEeCpuMs += (eeCpuNow - s_lastEeCpuNs) / 1.0e6;
                s_lastEeCpuNs = eeCpuNow;
            }
            const uint64_t gsNow = g_g141GsRasterNs.load(std::memory_order_relaxed);
            const uint64_t vuNow = g_g141Vu1RunNs.load(std::memory_order_relaxed);
            const uint64_t vifNow = g_g146Vif1Ns.load(std::memory_order_relaxed);
            const uint64_t gifNow = g_g146GifSubmitNs.load(std::memory_order_relaxed);
            const uint64_t imageNow = g_g146GsImageNs.load(std::memory_order_relaxed);
            const uint64_t localNow = g_g146GsLocalNs.load(std::memory_order_relaxed);
            const uint64_t g144MidNow = g_g146G144FlushMidNs.load(std::memory_order_relaxed);
            const uint64_t g144UploadNow = g_g146G144FlushUploadNs.load(std::memory_order_relaxed);
            const uint64_t g144FrameNow = g_g146G144FlushFrameNs.load(std::memory_order_relaxed);
            const uint64_t g144MidCountNow = g_g146G144FlushMidCount.load(std::memory_order_relaxed);
            const uint64_t g144UploadCountNow = g_g146G144FlushUploadCount.load(std::memory_order_relaxed);
            const uint64_t g144FrameCountNow = g_g146G144FlushFrameCount.load(std::memory_order_relaxed);
            const uint64_t g147PacketNow = g_g147GsGifPacketNs.load(std::memory_order_relaxed);
            const uint64_t g147PacketCountNow = g_g147GsGifPacketCount.load(std::memory_order_relaxed);
            const uint64_t g147DrawNow = g_g147DrawPrimitiveNs.load(std::memory_order_relaxed);
            const uint64_t g147DrawCountNow = g_g147DrawPrimitiveCount.load(std::memory_order_relaxed);
            const uint64_t g147TagsNow = g_g147GifTags.load(std::memory_order_relaxed);
            const uint64_t g147PackedRegsNow = g_g147PackedRegs.load(std::memory_order_relaxed);
            const uint64_t g147ReglistRegsNow = g_g147ReglistRegs.load(std::memory_order_relaxed);
            const uint64_t g147ImageBytesNow = g_g147ImageBytes.load(std::memory_order_relaxed);
            s_sumGsMs += (gsNow - s_lastGsNs) / 1.0e6;
            s_sumVu1Ms += (vuNow - s_lastVu1Ns) / 1.0e6;
            s_sumVif1Ms += (vifNow - s_lastVif1Ns) / 1.0e6;
            s_sumGifMs += (gifNow - s_lastGifNs) / 1.0e6;
            s_sumImageMs += (imageNow - s_lastImageNs) / 1.0e6;
            s_sumLocalMs += (localNow - s_lastLocalNs) / 1.0e6;
            s_sumG144MidMs += (g144MidNow - s_lastG144MidNs) / 1.0e6;
            s_sumG144UploadMs += (g144UploadNow - s_lastG144UploadNs) / 1.0e6;
            s_sumG144FrameMs += (g144FrameNow - s_lastG144FrameNs) / 1.0e6;
            s_sumG144MidCount += g144MidCountNow - s_lastG144MidCount;
            s_sumG144UploadCount += g144UploadCountNow - s_lastG144UploadCount;
            s_sumG144FrameCount += g144FrameCountNow - s_lastG144FrameCount;
            s_sumG147PacketMs += (g147PacketNow - s_lastG147PacketNs) / 1.0e6;
            s_sumG147DrawMs += (g147DrawNow - s_lastG147DrawNs) / 1.0e6;
            s_sumG147PacketCount += g147PacketCountNow - s_lastG147PacketCount;
            s_sumG147DrawCount += g147DrawCountNow - s_lastG147DrawCount;
            s_sumG147Tags += g147TagsNow - s_lastG147Tags;
            s_sumG147PackedRegs += g147PackedRegsNow - s_lastG147PackedRegs;
            s_sumG147ReglistRegs += g147ReglistRegsNow - s_lastG147ReglistRegs;
            s_sumG147ImageBytes += g147ImageBytesNow - s_lastG147ImageBytes;
            if (s_g171PerfOn)
            {
                for (int i = 0; i < 16; ++i)
                {
                    const uint64_t nsNow = g_g171RegNs[i].load(std::memory_order_relaxed);
                    const uint64_t cNow = g_g171RegCount[i].load(std::memory_order_relaxed);
                    s_sumG171RegNs[i] += nsNow - s_lastG171RegNs[i];
                    s_sumG171RegCount[i] += cNow - s_lastG171RegCount[i];
                    s_lastG171RegNs[i] = nsNow;
                    s_lastG171RegCount[i] = cNow;
                }
                for (int i = 0; i < 256; ++i)
                {
                    const uint64_t nsNow = g_g171AdNs[i].load(std::memory_order_relaxed);
                    const uint64_t cNow = g_g171AdCount[i].load(std::memory_order_relaxed);
                    s_sumG171AdNs[i] += nsNow - s_lastG171AdNs[i];
                    s_sumG171AdCount[i] += cNow - s_lastG171AdCount[i];
                    s_lastG171AdNs[i] = nsNow;
                    s_lastG171AdCount[i] = cNow;
                }
            }
            s_lastGsNs = gsNow;
            s_lastVu1Ns = vuNow;
            s_lastVif1Ns = vifNow;
            s_lastGifNs = gifNow;
            s_lastImageNs = imageNow;
            s_lastLocalNs = localNow;
            s_lastG144MidNs = g144MidNow;
            s_lastG144UploadNs = g144UploadNow;
            s_lastG144FrameNs = g144FrameNow;
            s_lastG144MidCount = g144MidCountNow;
            s_lastG144UploadCount = g144UploadCountNow;
            s_lastG144FrameCount = g144FrameCountNow;
            s_lastG147PacketNs = g147PacketNow;
            s_lastG147PacketCount = g147PacketCountNow;
            s_lastG147DrawNs = g147DrawNow;
            s_lastG147DrawCount = g147DrawCountNow;
            s_lastG147Tags = g147TagsNow;
            s_lastG147PackedRegs = g147PackedRegsNow;
            s_lastG147ReglistRegs = g147ReglistRegsNow;
            s_lastG147ImageBytes = g147ImageBytesNow;
            ++s_perfWindow;
        }
        else
        {
            if (s_g182PerfOn)
                s_lastEeCpuNs = g182ThreadCpuNs();
            s_lastGsNs = g_g141GsRasterNs.load(std::memory_order_relaxed);
            s_lastVu1Ns = g_g141Vu1RunNs.load(std::memory_order_relaxed);
            s_lastVif1Ns = g_g146Vif1Ns.load(std::memory_order_relaxed);
            s_lastGifNs = g_g146GifSubmitNs.load(std::memory_order_relaxed);
            s_lastImageNs = g_g146GsImageNs.load(std::memory_order_relaxed);
            s_lastLocalNs = g_g146GsLocalNs.load(std::memory_order_relaxed);
            s_lastG144MidNs = g_g146G144FlushMidNs.load(std::memory_order_relaxed);
            s_lastG144UploadNs = g_g146G144FlushUploadNs.load(std::memory_order_relaxed);
            s_lastG144FrameNs = g_g146G144FlushFrameNs.load(std::memory_order_relaxed);
            s_lastG144MidCount = g_g146G144FlushMidCount.load(std::memory_order_relaxed);
            s_lastG144UploadCount = g_g146G144FlushUploadCount.load(std::memory_order_relaxed);
            s_lastG144FrameCount = g_g146G144FlushFrameCount.load(std::memory_order_relaxed);
            s_lastG147PacketNs = g_g147GsGifPacketNs.load(std::memory_order_relaxed);
            s_lastG147PacketCount = g_g147GsGifPacketCount.load(std::memory_order_relaxed);
            s_lastG147DrawNs = g_g147DrawPrimitiveNs.load(std::memory_order_relaxed);
            s_lastG147DrawCount = g_g147DrawPrimitiveCount.load(std::memory_order_relaxed);
            s_lastG147Tags = g_g147GifTags.load(std::memory_order_relaxed);
            s_lastG147PackedRegs = g_g147PackedRegs.load(std::memory_order_relaxed);
            s_lastG147ReglistRegs = g_g147ReglistRegs.load(std::memory_order_relaxed);
            s_lastG147ImageBytes = g_g147ImageBytes.load(std::memory_order_relaxed);
            if (s_g171PerfOn)
            {
                for (int i = 0; i < 16; ++i)
                {
                    s_lastG171RegNs[i] = g_g171RegNs[i].load(std::memory_order_relaxed);
                    s_lastG171RegCount[i] = g_g171RegCount[i].load(std::memory_order_relaxed);
                }
                for (int i = 0; i < 256; ++i)
                {
                    s_lastG171AdNs[i] = g_g171AdNs[i].load(std::memory_order_relaxed);
                    s_lastG171AdCount[i] = g_g171AdCount[i].load(std::memory_order_relaxed);
                }
            }
        }
        s_lastEntry = perfEntryTs;
        s_haveLast = true;
    }

    // PHASE F30: probe mgDBuffID at $gp-0x77E8 before / after mgEndFrame call.
    const uint32_t gpReg = getRegU32(ctx, 28);
    const uint32_t mgDBuffIDAddr = gpReg + 0xFFFF8818u;
    const uint32_t beforeDB = dc2_read_u32(rdram, mgDBuffIDAddr);
    f29_log_counters("mgEndFrame.enter", n, runtime, ctx);
    f29_log_packet_state("mgEndFrame.packet.enter", n, rdram, ctx, runtime);
    const auto perfEndFrameT0 = s_perfOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // G183: bracket the PC sampler around exactly the call G182 found dominant (92-93% of
    // EE on-CPU time) so the histogram localizes hot addresses INSIDE mgEndFrame, not the
    // ~7-8% of EE time between calls.
    static const bool s_g183On = dc2_env_flag_enabled("DC2_G183_PCSAMPLE");
    if (s_g183On)
    {
        static std::thread s_g183Thread(g183SamplerThreadMain);
        static const bool s_g183Detached = [](){ s_g183Thread.detach(); return true; }();
        (void)s_g183Detached;
        g_g183Ctx.store(ctx, std::memory_order_relaxed);
        g_g183Sampling.store(true, std::memory_order_relaxed);
    }
    mgEndFrame__FP14mgCDrawManager_0x1425b0(rdram, ctx, runtime);
    if (s_g183On)
        g_g183Sampling.store(false, std::memory_order_relaxed);
    // G144: drain any trailing deferred triangles (frame-level tile-binning) now that this frame's
    // whole draw stream is complete, before the runtime presents/latches VRAM. Same (guest) thread
    // as capture, so no race on the deferred list. Sequential (no pool) — safe from this context.
    // Cheap no-op unless DC2_G144_TILEBIN>1 (list empty). (g144FlushPending declared at global scope.)
    // G150 MTGS: when the GS drain runs on the worker thread, g_g144List is populated ON that worker,
    // so the trailing flush must ALSO run there (folded into the frame-barrier callback below) — never
    // on this EE thread. Skip it here when MTGS is on.
    if (!g150_mtgs_enabled())
        g144FlushPending();
    if (s_perfOn)
    {
        s_sumEndFrameMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - perfEndFrameT0).count();
        if (s_perfWindow >= 30u)
        {
            const double w = static_cast<double>(s_perfWindow);
            const double avgFrameMs = s_sumFrameMs / w;
            const double avgEndFrameMs = s_sumEndFrameMs / w;
            const double avgGsMs = s_sumGsMs / w;
            const double avgVu1Ms = s_sumVu1Ms / w;
            const double avgVu1ProperMs = avgVu1Ms - avgGsMs; // VU1 minus its nested XGKICK raster
            const double avgOtherMs = avgFrameMs - avgVu1Ms;  // everything not in vu1.run (2D/upload/present)
            if (s_g141PerfOn)
            {
                std::fprintf(stderr,
                             "[G141:perf] n=%u window=%u avgFrameMs=%.2f fps=%.2f | GSraster=%.1f (%.0f%%) VU1proper=%.1f (%.0f%%) other=%.1f (%.0f%%) | mgEndFrame=%.1f\n",
                             n, s_perfWindow, avgFrameMs, 1000.0 / std::max(0.001, avgFrameMs),
                             avgGsMs, 100.0 * avgGsMs / std::max(0.001, avgFrameMs),
                             avgVu1ProperMs, 100.0 * avgVu1ProperMs / std::max(0.001, avgFrameMs),
                             avgOtherMs, 100.0 * avgOtherMs / std::max(0.001, avgFrameMs),
                             avgEndFrameMs);
            }
            else
            {
                std::fprintf(stderr,
                             "[G146:frame] n=%u window=%u avgFrameMs=%.2f fps=%.2f | mgEndFrame=%.1f\n",
                             n, s_perfWindow, avgFrameMs,
                             1000.0 / std::max(0.001, avgFrameMs),
                             avgEndFrameMs);
            }
            std::fprintf(stderr,
                         "[G146:perf] n=%u window=%u incl: VIF1=%.1f GIFsubmit=%.1f GSimage=%.1f GSlocal=%.1f\n",
                         n, s_perfWindow,
                         s_sumVif1Ms / w,
                         s_sumGifMs / w,
                         s_sumImageMs / w,
                         s_sumLocalMs / w);
            // G303: VU1-worker busy time per frame (delta since last window / window frames). Placed
            // alongside GSimage so the pole can be attributed by THREAD (EE / VU1 worker / GS worker),
            // not by a per-bucket table (thread_attribution_trap). Only meaningful when MTVU is on.
            {
                static uint64_t s_lastVuwNs = g297WorkerBusyNs();
                static uint64_t s_lastVuwKicks = g297WorkerKicksRun();
                const uint64_t vuwNs = g297WorkerBusyNs();
                const uint64_t vuwKicks = g297WorkerKicksRun();
                const double vuwMsPerF = (double)(vuwNs - s_lastVuwNs) / 1e6 / w;
                const double vuwKicksPerF = (double)(vuwKicks - s_lastVuwKicks) / w;
                s_lastVuwNs = vuwNs;
                s_lastVuwKicks = vuwKicks;
                static uint64_t s_lastStallNs = g297GsCollectStallNs();
                static uint64_t s_lastStalls = g297GsCollectStalls();
                const uint64_t stallNs = g297GsCollectStallNs();
                const uint64_t stalls = g297GsCollectStalls();
                const double stallMsPerF = (double)(stallNs - s_lastStallNs) / 1e6 / w;
                const double stallsPerF = (double)(stalls - s_lastStalls) / w;
                s_lastStallNs = stallNs;
                s_lastStalls = stalls;
                static uint64_t s_lastGsBusyNs = g303_gs_worker_busy_ns();
                const uint64_t gsBusyNs = g303_gs_worker_busy_ns();
                const double gsBusyMsPerF = (double)(gsBusyNs - s_lastGsBusyNs) / 1e6 / w;
                s_lastGsBusyNs = gsBusyNs;
                if (vuwKicks != 0ull)
                    std::fprintf(stderr,
                                 "[G303:vu1w] n=%u window=%u busyMs/f=%.1f kicks/f=%.0f gsStallMs/f=%.1f gsStalls/f=%.0f gsWorkerMs/f=%.1f\n",
                                 n, s_perfWindow, vuwMsPerF, vuwKicksPerF, stallMsPerF, stallsPerF, gsBusyMsPerF);
            }
            if (s_g182PerfOn)
            {
                const double eeCpuMs = s_sumEeCpuMs / w;
                std::fprintf(stderr,
                             "[G182:ee] n=%u window=%u busyMs/f=%.2f cpuMs/f=%.2f onCPU=%.0f%%\n",
                             n, s_perfWindow, avgFrameMs, eeCpuMs,
                             100.0 * eeCpuMs / std::max(0.001, avgFrameMs));
            }
            std::fprintf(stderr,
                         "[G146:g144] n=%u window=%u flush: mid=%.1fms/%.2fx upload=%.1fms/%.2fx frame=%.1fms/%.2fx\n",
                         n, s_perfWindow,
                         s_sumG144MidMs / w,
                         static_cast<double>(s_sumG144MidCount) / w,
                         s_sumG144UploadMs / w,
                         static_cast<double>(s_sumG144UploadCount) / w,
                         s_sumG144FrameMs / w,
                         static_cast<double>(s_sumG144FrameCount) / w);
            if (s_g147PerfOn)
            {
                const double packetMs = s_sumG147PacketMs / w;
                const double drawMs = s_sumG147DrawMs / w;
                const double imageMs = s_sumImageMs / w;
                const double parserOtherMs = packetMs - drawMs - imageMs;
                std::fprintf(stderr,
                             "[G147:gif] n=%u window=%u packet=%.1fms/%.2fx draw=%.1fms/%.2fx parserOther=%.1fms tags=%.1f packedRegs=%.1f reglistRegs=%.1f imageKB=%.1f\n",
                             n, s_perfWindow,
                             packetMs,
                             static_cast<double>(s_sumG147PacketCount) / w,
                             drawMs,
                             static_cast<double>(s_sumG147DrawCount) / w,
                             parserOtherMs,
                             static_cast<double>(s_sumG147Tags) / w,
                             static_cast<double>(s_sumG147PackedRegs) / w,
                             static_cast<double>(s_sumG147ReglistRegs) / w,
                             static_cast<double>(s_sumG147ImageBytes) / (w * 1024.0));
            }
            if (s_g171PerfOn)
            {
                // Packed-register descriptor names (GS PACKED format table).
                static const char *kRegNames[16] = {
                    "PRIM", "RGBAQ", "ST", "UV", "XYZF2", "XYZ2", "TEX0_1", "TEX0_2",
                    "CLAMP_1", "CLAMP_2", "FOG", "resv0x0B", "XYZ3", "XYZF3", "A+D", "NOP"};
                double totalMs = 0.0;
                for (int i = 0; i < 16; ++i)
                    totalMs += s_sumG171RegNs[i] / 1.0e6;
                std::fprintf(stderr, "[G171:regstat] n=%u window=%u totalMs=%.2f/f\n", n, s_perfWindow, totalMs / w);
                for (int i = 0; i < 16; ++i)
                {
                    const double ms = s_sumG171RegNs[i] / 1.0e6;
                    const double cnt = static_cast<double>(s_sumG171RegCount[i]);
                    if (cnt <= 0.0)
                        continue;
                    std::fprintf(stderr,
                                 "[G171:regstat]   desc=0x%02X %-9s ms/f=%.3f (%.0f%%) count/f=%.1f ns/call=%.1f\n",
                                 i, kRegNames[i], ms / w,
                                 100.0 * ms / std::max(0.001, totalMs),
                                 cnt / w,
                                 (s_sumG171RegNs[i]) / std::max(1.0, cnt));
                }
                // Top A+D (desc 0x0E) register addresses by total time this window.
                int adIdx[256];
                int adN = 0;
                for (int i = 0; i < 256; ++i)
                    if (s_sumG171AdCount[i] > 0)
                        adIdx[adN++] = i;
                std::sort(adIdx, adIdx + adN, [&](int a, int b) {
                    return s_sumG171AdNs[a] > s_sumG171AdNs[b];
                });
                const int adShow = std::min(adN, 8);
                for (int k = 0; k < adShow; ++k)
                {
                    const int i = adIdx[k];
                    const double ms = s_sumG171AdNs[i] / 1.0e6;
                    const double cnt = static_cast<double>(s_sumG171AdCount[i]);
                    std::fprintf(stderr,
                                 "[G171:regstat]     A+D addr=0x%02X ms/f=%.3f count/f=%.1f ns/call=%.1f\n",
                                 i, ms / w, cnt / w, (s_sumG171AdNs[i]) / std::max(1.0, cnt));
                }
            }
            s_sumFrameMs = 0.0;
            s_sumEndFrameMs = 0.0;
            s_sumEeCpuMs = 0.0;
            s_sumGsMs = 0.0;
            s_sumVu1Ms = 0.0;
            s_sumVif1Ms = 0.0;
            s_sumGifMs = 0.0;
            s_sumImageMs = 0.0;
            s_sumLocalMs = 0.0;
            s_sumG144MidMs = 0.0;
            s_sumG144UploadMs = 0.0;
            s_sumG144FrameMs = 0.0;
            s_sumG144MidCount = 0ull;
            s_sumG144UploadCount = 0ull;
            s_sumG144FrameCount = 0ull;
            s_sumG147PacketMs = 0.0;
            s_sumG147DrawMs = 0.0;
            s_sumG147PacketCount = 0ull;
            s_sumG147DrawCount = 0ull;
            s_sumG147Tags = 0ull;
            s_sumG147PackedRegs = 0ull;
            s_sumG147ReglistRegs = 0ull;
            s_sumG147ImageBytes = 0ull;
            if (s_g171PerfOn)
            {
                for (int i = 0; i < 16; ++i)
                {
                    s_sumG171RegNs[i] = 0ull;
                    s_sumG171RegCount[i] = 0ull;
                }
                for (int i = 0; i < 256; ++i)
                {
                    s_sumG171AdNs[i] = 0ull;
                    s_sumG171AdCount[i] = 0ull;
                }
            }
            s_perfWindow = 0u;
        }
    }
    const uint32_t afterDB = dc2_read_u32(rdram, mgDBuffIDAddr);
    if (f29_should_log(n))
    {
        std::fprintf(stderr,
                     "[F30:mgDBuffID] n=%u gp=0x%x addr=0x%x before=%u after=%u\n",
                     n, gpReg, mgDBuffIDAddr, beforeDB, afterDB);
    }
    if (forceVisible)
    {
        const bool clear0 = runtime->gs().clearFramebufferContext(0u, 0xFF00FF00u);
        const bool clear1 = runtime->gs().clearFramebufferContext(1u, 0xFF00FF00u);
        runtime->gs().latchHostPresentationFrame();
        if (f29_should_log(n))
        {
            std::fprintf(stderr,
                         "[F29:mgEndFrame.forceVisible] n=%u clear0=%u clear1=%u\n",
                         n, clear0 ? 1u : 0u, clear1 ? 1u : 0u);
        }
    }
    else
    {
        if (forceDrawBufferLatch)
        {
            // Legacy F30 presentation fallback: follow a live draw context
            // instead of the guest-selected display buffer.
            static GSFrameReg lastValidFrame{};
            static bool haveLastValidFrame = false;
            const auto &cf0 = runtime->gs().getContextFrame(0);
            const auto &cf1 = runtime->gs().getContextFrame(1);
            const bool cf0Valid = f30_valid_frame(cf0);
            const bool cf1Valid = f30_valid_frame(cf1);
            const bool currentValid = cf0Valid || cf1Valid;
            bool usedSticky = false;
            GSFrameReg chosen{};
            if (cf0Valid)
            {
                chosen = cf0;
            }
            else if (cf1Valid)
            {
                chosen = cf1;
            }
            else if (haveLastValidFrame)
            {
                chosen = lastValidFrame;
                usedSticky = true;
            }
            if (currentValid)
            {
                lastValidFrame = chosen;
                haveLastValidFrame = true;
            }
            if (f30_valid_frame(chosen))
            {
                const uint64_t baseEnc =
                    static_cast<uint64_t>(chosen.fbp & 0x1ffu)
                    | (static_cast<uint64_t>(chosen.fbw & 0x3fu) << 9)
                    | (static_cast<uint64_t>(chosen.psm & 0x3fu) << 15);
                runtime->gs().writeRegister(0x59u, baseEnc | 0x80000000000ull);
                runtime->gs().writeRegister(0x5bu, baseEnc);
            }
            if (f29_should_log(n))
            {
                std::fprintf(stderr,
                             "[F30:mgEndFrame.forceDrawLatch] n=%u currentValid=%u usedSticky=%u selected.fbp=0x%x fbw=%u psm=%u mgDBuffID=%u\n",
                             n, currentValid ? 1u : 0u, usedSticky ? 1u : 0u,
                             chosen.fbp, chosen.fbw, chosen.psm, afterDB);
            }
        }
        // G150 MTGS: g150_frame_barrier first blocks until the GS worker has finished this frame's
        // draws (frameDrain), then runs this closure on THIS (EE) thread: drain the trailing tile-bin
        // tris (safe — worker idle, sole toucher of g_g144List) and latch the present frame, reading
        // live display registers + vsync tick over complete VRAM exactly as the synchronous baseline.
        // When MTGS is off, the barrier runs the closure inline and the flush is skipped here (already
        // done on the EE thread above) → byte-identical to the pre-G150 path.
        g150_frame_barrier([runtime, n] {
            // G189 (diagnostic watchdog, DEFAULT OFF, DC2_G189_WATCHDOG=1): cheap atomic-only
            // breadcrumbs (no I/O in the hot path -- an earlier fprintf-based version of this
            // trace perturbed thread timing enough to hide the bug it was meant to catch) so a
            // hang under DC2_G157_PIPELINE=1 can be isolated to g144FlushPending() vs
            // latchHostPresentationFrame() (both run on the GS worker thread under pipelining,
            // per g150_frame_barrier). See plans/phase-G189-fix-log.md.
            extern void g189_set_closure_stage(int stage, uint32_t n);
            g189_set_closure_stage(1, n);
            if (g150_mtgs_enabled())
                g144FlushPending();
            g189_set_closure_stage(2, n);
            runtime->gs().latchHostPresentationFrame();
            g189_set_closure_stage(3, n);
        });
        if (f29_should_log(n))
        {
            std::fprintf(stderr,
                         "[F52:mgEndFrame.guestLatch] n=%u forcedDrawLatch=%u mgDBuffID=%u\n",
                         n, forceDrawBufferLatch ? 1u : 0u, afterDB);
        }
    }
    f29_log_packet_state("mgEndFrame.packet.exit", n, rdram, ctx, runtime);
    f29_log_counters("mgEndFrame.exit", n, runtime, ctx);
    // Map sparse mgEndFrame probe hits onto a denser virtual frame clock that
    // matches the canonical scripted input windows used by F46/F49 prompts.
    uint32_t scriptFrame = static_cast<uint32_t>((static_cast<uint64_t>(n) * 31ull) / 14ull);
    if (scriptFrame > 16u)
        scriptFrame -= 16u;
    else
        scriptFrame = 0u;
    const uint32_t loopNo = dc2_read_u32(rdram, gpReg + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gpReg + 0xFFFF8AE0u);
    f47_log_menu_state(rdram, ctx, n, scriptFrame, loopNo, nextLoopNo);
    f57_log_frame_loop(rdram, n, loopNo, nextLoopNo);
    f59_log_frame(rdram, n, loopNo);
    g195_log_camera_state(rdram, n, loopNo);
    g154_observe_frame(rdram, n, scriptFrame, loopNo, nextLoopNo);
    g202_update_town_depth_scope(rdram, n, scriptFrame, loopNo);
    dc2_apply_60fps_patch(rdram);
    g_f40_frame_counter = scriptFrame;
    f40_drive_pad(scriptFrame, loopNo);
}

static void scelogofade_done_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnU32(ctx, 1u);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F5: DC2 ג€” GetFullPath corrected shim.
// Real convention: a0=input_filename, a1=output_buf (the auto-stub in dc2_auto_stubs.cpp
// had the args reversed and never prepended a drive prefix, so fioOpen got "").
// Bare filenames get "cdrom0:\" prepended; paths that already contain ':' pass through.
static void getfullpath_shim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t srcAddr  = getRegU32(ctx, 4); // $a0 = input filename
    const uint32_t destAddr = getRegU32(ctx, 5); // $a1 = output buffer

    const char *src = reinterpret_cast<const char *>(getConstMemPtr(rdram, srcAddr));
    char *dest      = reinterpret_cast<char *>(getMemPtr(rdram, destAddr));

    if (src && dest)
    {
        if (::strchr(src, ':'))
            ::snprintf(dest, 256, "%s", src);
        else
            ::snprintf(dest, 256, "cdrom0:\\%s", src);
    }

    setReturnU32(ctx, destAddr);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F5: DC2 ג€” _Exit halt. The auto-stub returned 0 (nop), so Exit(-1) looped
// forever calling TerminateLibrary ג†’ InitTLB ג†’ _Exit ג†’ repeat. Terminate cleanly.
static void exit_halt_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (dc2_env_flag_enabled("DC2_TRACE_EXIT"))
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t titleInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF996Cu) : 0u;
        const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
        const uint32_t nextLoopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8AE0u) : 0u;
        std::fprintf(stderr,
                     "[G59:exit] status=%d pc=0x%x ra=0x%x sp=0x%x a0=0x%x loop=%u next=%u titleState=%u titleSub=%u\n",
                     static_cast<int32_t>(getRegU32(ctx, 4)), ctx->pc, getRegU32(ctx, 31),
                     getRegU32(ctx, 29), getRegU32(ctx, 4), loopNo, nextLoopNo,
                     titleInfo ? dc2_read_u32(rdram, titleInfo + 0x00u) : 0u,
                     titleInfo ? dc2_read_u32(rdram, titleInfo + 0x04u) : 0u);
        std::fflush(stderr);
    }
    std::fprintf(stderr, "[dc2] _Exit called ג€” halting\n");
    std::exit(0);
}

static uint32_t dc2_pad_mask(PS2Runtime *runtime, int port, int slot)
{
    // G7: a live host controller (when connected and no explicit script) OWNS the
    // button mask, taking priority over the scripted F40 schedule. The analog sticks
    // flow separately through dc2_write_pad_status from the same snapshot.
    if (g_pad_live_connected)
        return (port == 0) ? static_cast<uint32_t>(g_pad_live_mask) : 0u;

    // PHASE F40: scripted reads consume only the mgEndFrame-driven state.
    if (f40_script_input_requested())
        return (port == 0 && g_f40_active) ? static_cast<uint32_t>(g_f40_pressed) : 0u;

    uint8_t buf[32] = {0};
    if (runtime && runtime->padBackend().readState(port, slot, buf, sizeof(buf)))
    {
        const uint16_t activeLow = static_cast<uint16_t>(buf[2] | (buf[3] << 8));
        const uint32_t live = static_cast<uint32_t>(static_cast<uint16_t>(~activeLow));
        if (live != 0u)
            return live;
    }

    if (port == 0)
    {
        static uint32_t call_count = 0u;
        const uint32_t cycle = call_count++ % 240u;
        if ((cycle >= 30u && cycle < 45u) || (cycle >= 120u && cycle < 135u))
            return 0x0800u;
    }

    return 0u;
}

static void dc2_write_pad_status(uint8_t *rdram, uint32_t statusAddr, uint32_t mask)
{
    if (statusAddr == 0u)
        return;

    uint8_t *dst = getMemPtr(rdram, statusAddr);
    if (!dst)
        return;

    dst[0] = static_cast<uint8_t>(mask & 0xFFu);
    dst[1] = static_cast<uint8_t>((mask >> 8) & 0xFFu);
    dst[2] = 0u;
    dst[3] = 0u;
    // PAD_STATUS stick ints at +4/+8/+0xc/+0x10 = LY,LX,RY,RX (each 0x80 centre).
    // F66: in-dungeon free-roam reads the LEFT analog stick for player movement, so
    // deflect it from the present-loop dungeon driver when active.
    uint8_t outLX = 0x80u, outLY = 0x80u, outRX = 0x80u, outRY = 0x80u;
    if (g_pad_live_connected)
    {
        // G7: live controller owns all four axes (both sticks).
        outLX = g_pad_live_lx; outLY = g_pad_live_ly;
        outRX = g_pad_live_rx; outRY = g_pad_live_ry;
    }
    else if (g_f66_dungeon_active)
    {
        outLX = g_f66_dungeon_lx; outLY = g_f66_dungeon_ly;
        outRX = g_f40_rx; outRY = g_f40_ry;
    }
    else
    {
        // G49: scripted right-stick deflection (DC2_RSTICK) for headless costume-rotation
        // validation; default 0x80/0x80 (centred) so normal scripted runs are unaffected.
        outRX = g_f40_rx; outRY = g_f40_ry;
    }
    const uint8_t sticks[4] = { outLY, outLX, outRY, outRX };
    for (int i = 0; i < 4; ++i)
    {
        dst[4 + i * 4 + 0] = sticks[i];
        dst[4 + i * 4 + 1] = 0u;
        dst[4 + i * 4 + 2] = 0u;
        dst[4 + i * 4 + 3] = 0u;
    }

    dst[20] = 99u; dst[21] = 0u; dst[22] = 0u; dst[23] = 0u;
    dst[24] = 6u;  dst[25] = 0u; dst[26] = 0u; dst[27] = 0u;
    dst[28] = 7u;  dst[29] = 0u; dst[30] = 0u; dst[31] = 0u;
    dst[32] = 7u;  dst[33] = 0u; dst[34] = 0u; dst[35] = 0u;
    dst[36] = 7u;  dst[37] = 0u; dst[38] = 0u; dst[39] = 0u;
}

static uint32_t dc2_read_u32(uint8_t *rdram, uint32_t addr)
{
    const uint8_t *src = getConstMemPtr(rdram, addr);
    if (!src)
        return 0u;
    return static_cast<uint32_t>(src[0]) |
        (static_cast<uint32_t>(src[1]) << 8) |
        (static_cast<uint32_t>(src[2]) << 16) |
        (static_cast<uint32_t>(src[3]) << 24);
}

static uint64_t dc2_read_u64(uint8_t *rdram, uint32_t addr)
{
    return static_cast<uint64_t>(dc2_read_u32(rdram, addr)) |
        (static_cast<uint64_t>(dc2_read_u32(rdram, addr + 4u)) << 32u);
}

static void dc2_write_u32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    uint8_t *dst = getMemPtr(rdram, addr);
    if (!dst)
        return;
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

static void dc2_write_u16(uint8_t *rdram, uint32_t addr, uint16_t value)
{
    uint8_t *dst = getMemPtr(rdram, addr);
    if (!dst)
        return;
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}
