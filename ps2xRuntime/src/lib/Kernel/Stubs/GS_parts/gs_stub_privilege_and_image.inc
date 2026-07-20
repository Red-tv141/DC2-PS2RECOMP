
        const bool hasClearPacket = (which == 0u) ? hasSeededGsClearPacket(db.clear0)
                                                  : hasSeededGsClearPacket(db.clear1);
        const uint64_t debugDrawFrameReg = (which == 0u) ? db.draw01.frame1.value
                                                         : db.draw11.frame1.value;

        g177ApplyGsDispEnv(runtime, db.disp[which]);
        static uint32_t s_swapDbuffLogCount = 0u;
        if (s_swapDbuffLogCount < 32u)
        {
            const uint32_t dispFbp = static_cast<uint32_t>(db.disp[which].dispfb & 0x1FFu);
            const uint32_t clearContext = (which == 0u)
                                              ? static_cast<uint32_t>((db.clear0.prim.value >> 9) & 0x1u)
                                              : static_cast<uint32_t>((db.clear1.prim.value >> 9) & 0x1u);
            RUNTIME_LOG("[gs:swapdbuff] which=" << which
                                                << " env=0x" << std::hex << envAddr
                                                << " dispfb=0x" << db.disp[which].dispfb
                                                << " display=0x" << db.disp[which].display
                                                << " pmode=0x" << db.disp[which].pmode
                                                << " dispFbp=" << dispFbp
                                                << " clearCtxt=" << clearContext
                                                << std::dec << std::endl);
            ++s_swapDbuffLogCount;
        }
        logSwapProbeStage(runtime, "swap-pre", which, debugDrawFrameReg, db.disp[which].dispfb, hasClearPacket);
        if (which == 0u)
        {
            g177ApplyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw01), 8u);
            g177ApplyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw02), 8u);
            if (hasSeededGsClearPacket(db.clear0))
            {
                const uint32_t clearContext = static_cast<uint32_t>((db.clear0.prim.value >> 9) & 0x1u);
                // G219: queued through the arbiter FIFO (was: immediate EE-thread clear — see
                // g219ApplyGsClearPacketQueued).
                g219ApplyGsClearPacketQueued(runtime, db.clear0, clearContext);
                logSwapProbeStage(runtime, "swap-post-clear", which, db.draw01.frame1.value, db.disp[which].dispfb, true);
            }
            logSwapProbeStage(runtime, "swap-post", which, db.draw01.frame1.value, db.disp[which].dispfb, hasClearPacket);
        }
        else
        {
            g177ApplyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw11), 8u);
            g177ApplyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw12), 8u);
            if (hasSeededGsClearPacket(db.clear1))
            {
                const uint32_t clearContext = static_cast<uint32_t>((db.clear1.prim.value >> 9) & 0x1u);
                // G219: queued through the arbiter FIFO (was: immediate EE-thread clear — see
                // g219ApplyGsClearPacketQueued).
                g219ApplyGsClearPacketQueued(runtime, db.clear1, clearContext);
                logSwapProbeStage(runtime, "swap-post-clear", which, db.draw11.frame1.value, db.disp[which].dispfb, true);
            }
            logSwapProbeStage(runtime, "swap-post", which, db.draw11.frame1.value, db.disp[which].dispfb, hasClearPacket);
        }

        setReturnS32(ctx, static_cast<int32_t>(which ^ 1u));
    }

    void sceGsSwapDBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        const uint32_t which = getRegU32(ctx, 5) & 1u;

        // PHASE F30: probe entry + cross-check mgDBuffID, retained only for explicit phase tracing.
        if (phaseDiagnosticsEnabled())
        {
            static std::atomic<uint32_t> s_f30Swap{0};
            const uint32_t n = s_f30Swap.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 8u || (n % 60u) == 0u)
            {
                const uint32_t gpReg = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
                const uint32_t dbAddr = gpReg + 0xFFFF8818u;
                uint32_t memDB = 0u;
                if (rdram)
                {
                    std::memcpy(&memDB, rdram + (dbAddr & 0x01FFFFFFu), sizeof(memDB));
                }
                std::fprintf(stderr, "[F30:swap] n=%u envAddr=0x%x which=%u gp=0x%x dbAddr=0x%x memDB=%u\n",
                             n, envAddr, which, gpReg, dbAddr, memDB);
            }
        }

        GsDBuffMem db{};
        if (!runtime || !readGsDBuff(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }

        g177ApplyGsDispEnv(runtime, db.disp[which]);
        if (which == 0u)
        {
            g177ApplyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw0), 8u);
        }
        else
        {
            g177ApplyGsRegPairs(runtime, reinterpret_cast<const GsRegPairMem *>(&db.draw1), 8u);
        }

        setReturnS32(ctx, static_cast<int32_t>(which ^ 1u));
    }

    void sceGsSyncPath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int32_t mode = static_cast<int32_t>(getRegU32(ctx, 4));
        auto &mem = runtime->memory();

        if (mode == 0)
        {
            mem.processPendingTransfers();

            uint32_t count = 0;
            constexpr uint32_t kTimeout = 0x1000000;

            while ((mem.readIORegister(0x10009000) & 0x100) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            while ((mem.readIORegister(0x1000A000) & 0x100) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            while ((mem.readIORegister(0x10003C00) & 0x1F000003) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            while ((mem.readIORegister(0x10003020) & 0xC00) != 0)
            {
                if (++count > kTimeout)
                {
                    setReturnS32(ctx, -1);
                    return;
                }
            }

            setReturnS32(ctx, 0);
        }
        else
        {
            uint32_t result = 0;

            if ((mem.readIORegister(0x10009000) & 0x100) != 0)
                result |= 1;
            if ((mem.readIORegister(0x1000A000) & 0x100) != 0)
                result |= 2;
            if ((mem.readIORegister(0x10003C00) & 0x1F000003) != 0)
                result |= 4;
            if ((mem.readIORegister(0x10003020) & 0xC00) != 0)
                result |= 0x10;

            setReturnS32(ctx, result);
        }
    }

    void sceGsSyncV(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ::g_f60_gsSyncV.fetch_add(1, std::memory_order_relaxed);
        const uint64_t tick = ps2_syscalls::WaitForNextVSyncTick(rdram, runtime);
        if (g_gparam.interlace != 0u)
        {
            setReturnS32(ctx, getGsSyncVFieldForTick(tick));
            return;
        }

        setReturnS32(ctx, 1);
    }

    void sceGsSyncVCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newCallback = getRegU32(ctx, 4);
        const uint32_t callerPc = ctx ? ctx->pc : 0u;
        const uint32_t callerRa = ctx ? getRegU32(ctx, 31) : 0u;
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t sp = getRegU32(ctx, 29);

        uint32_t oldCallback = 0u;
        {
            std::lock_guard<std::mutex> lock(g_gs_sync_v_callback_mutex);
            oldCallback = g_gs_sync_v_callback_func;
            g_gs_sync_v_callback_func = newCallback;
            if (newCallback != 0u)
            {
                g_gs_sync_v_callback_gp = gp;
                g_gs_sync_v_callback_sp = sp;
            }
        }

        static uint32_t s_syncVCallbackLogCount = 0u;
        if (s_syncVCallbackLogCount < 128u)
        {
            RUNTIME_LOG("[sceGsSyncVCallback:set] new=0x" << std::hex << newCallback
                                                          << " old=0x" << oldCallback
                                                          << " callerPc=0x" << callerPc
                                                          << " callerRa=0x" << callerRa
                                                          << " gp=0x" << gp
                                                          << " sp=0x" << sp
                                                          << std::dec << std::endl);
            ++s_syncVCallbackLogCount;
        }

        if (newCallback != 0u)
        {
            ps2_syscalls::EnsureVSyncWorkerRunning(rdram, runtime);
        }

        setReturnU32(ctx, oldCallback);
    }

    void sceGszbufaddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        uint32_t param_1 = getRegU32(ctx, 4);
        int32_t w = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFF));
        int32_t h = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFF));

        int32_t width_blocks = (w + 63) >> 6;
        if (w + 63 < 0)
            width_blocks = (w + 126) >> 6;

        int32_t height_blocks;
        if ((param_1 & 2) != 0)
        {
            int32_t v = (h + 63) >> 6;
            if (h + 63 < 0)
                v = (h + 126) >> 6;
            height_blocks = v;
        }
        else
        {
            int32_t v = (h + 31) >> 5;
            if (h + 31 < 0)
                v = (h + 62) >> 5;
            height_blocks = v;
        }

        int32_t product = width_blocks * height_blocks;

        uint64_t gparam_val = 0;
        if (runtime)
        {
            uint8_t *scratch = runtime->memory().getScratchpad();
            if (scratch)
            {
                std::memcpy(&gparam_val, scratch + 0x100, sizeof(gparam_val));
            }
        }
        if ((gparam_val & 0xFFFF0000FFFFULL) == 1ULL)
            product = (product * 0x10000) >> 16;
        else
            product = (product * 0x20000) >> 16;

        setReturnS32(ctx, product);
    }

    void Ps2SwapDBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int logCount = 0;
        if (logCount < 8)
        {
            RUNTIME_LOG("ps2_stub Ps2SwapDBuff");
            ++logCount;
        }
        setReturnS32(ctx, 0);
    }

    void sceVif1PkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        const uint64_t dataValue = GPR_U64(ctx, 6);
        const uint32_t words[4] = {
            static_cast<uint32_t>(dataValue & 0xFFFFFFFFu),
            static_cast<uint32_t>(dataValue >> 32u),
            getRegU32(ctx, 5),
            0u,
        };
        writeGuestBytes(rdram,
                        runtime,
                        currentAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
    }

    void sceVif1PkAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        alignPacketBuilderState(rdram,
                                runtime,
                                getRegU32(ctx, 4),
                                getRegU32(ctx, 5),
                                getRegU32(ctx, 6));
    }

    void sceVif1PkCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t refAddr = getRegU32(ctx, 5) & 0x9FFFFFFFu;
        const uint32_t tagWord = getRegU32(ctx, 6) | 0x50000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[2] = {tagWord, refAddr};

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 8u);
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
    }

    void sceVif1PkCloseDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        uint32_t openAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr) ||
            !tryReadWordFromGuest(rdram, runtime, stateAddr + 12u, openAddr) ||
            openAddr == 0u)
        {
            return;
        }

        const uint32_t currentMinusTag = currentAddr - 4u;
        const uint32_t wordCount = (currentMinusTag - openAddr) >> 2u;
        const uint32_t qwordCount = wordCount >> 2u;
        uint32_t tagWord = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, openAddr, tagWord))
        {
            return;
        }

        logVif1PacketStateOp("close-direct", ctx, stateAddr, currentAddr, openAddr, qwordCount);

        tagWord += qwordCount;
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
        writeGuestU32(rdram, runtime, openAddr, tagWord);
    }

    void sceVif1PkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)ctx;
        closePacketGifTag(rdram, runtime, getRegU32(ctx, 4), 20u);
    }

    void sceVif1PkCnt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t tagWord = getRegU32(ctx, 5) | 0x10000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[2] = {tagWord, 0u};

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 8u);
    }

    void sceVif1PkEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t tagWord = getRegU32(ctx, 5) | 0x70000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[2] = {tagWord, 0u};

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writeGuestU32(rdram, runtime, stateAddr + 12u, 0u);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 8u);
    }

    void sceVif1PkInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        initPacketBuilderState(rdram, ctx, runtime);
        writeGuestU32(rdram, runtime, getRegU32(ctx, 4) + 20u, 0u);
    }

    void sceVif1PkOpenDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        alignPacketBuilderState(rdram, runtime, stateAddr, 2u, 3u);

        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        const uint32_t tagWord = (getRegU32(ctx, 5) != 0u) ? 0xD0000000u : 0x50000000u;
        writeGuestU32(rdram, runtime, currentAddr, tagWord);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 4u);
        writeGuestU32(rdram, runtime, stateAddr + 12u, currentAddr);
        logVif1PacketStateOp("open-direct", ctx, stateAddr, currentAddr + 4u, currentAddr, tagWord);
    }

    void sceVif1PkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        openPacketGifTag(rdram, ctx, runtime, getRegU32(ctx, 4), 20u);
    }

    void sceVif1PkReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        resetPacketBuilderState(rdram, ctx, runtime);
        writeGuestU32(rdram, runtime, getRegU32(ctx, 4) + 20u, 0u);
    }

    void sceVif1PkReserve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t wordCount = getRegU32(ctx, 5);
        uint32_t currentAddr = 0u;
        tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr);
        const uint32_t reservedAddr = reservePacketBuilderWords(rdram, runtime, stateAddr, wordCount);
        logVif1PacketStateOp("reserve", ctx, stateAddr, currentAddr, reservedAddr, wordCount);
        setReturnU32(ctx, reservedAddr);
    }

    void sceVif1PkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, terminatePacketBuilderState(rdram, ctx, runtime));
    }
}
