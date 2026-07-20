        }

        uint32_t terminatePacketBuilderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stateAddr = getRegU32(ctx, 4);
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return 0u;
            }

            const uint32_t zero = 0u;
            while ((currentAddr & 0xCu) != 0u)
            {
                writeGuestBytes(rdram,
                                runtime,
                                currentAddr,
                                reinterpret_cast<const uint8_t *>(&zero),
                                sizeof(zero));
                currentAddr += 4u;
            }

            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
            writeGuestBytes(rdram,
                            runtime,
                            stateAddr + 8u,
                            reinterpret_cast<const uint8_t *>(&zero),
                            sizeof(zero));
            return currentAddr;
        }

        void resetPacketBuilderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stateAddr = getRegU32(ctx, 4);
            uint32_t baseAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr + 4u, baseAddr))
            {
                setReturnU32(ctx, 0u);
                return;
            }

            const uint32_t words[4] = {baseAddr, baseAddr, 0u, 0u};
            writeGuestBytes(rdram,
                            runtime,
                            stateAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
            setReturnU32(ctx, baseAddr);
        }

        bool tryReadQwordFromGuest(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint64_t &outQword)
        {
            uint32_t low = 0u;
            uint32_t high = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, addr, low) ||
                !tryReadWordFromGuest(rdram, runtime, addr + 4u, high))
            {
                return false;
            }

            outQword = static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32u);
            return true;
        }

        void writeGuestU32(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint32_t value)
        {
            writeGuestBytes(rdram,
                            runtime,
                            addr,
                            reinterpret_cast<const uint8_t *>(&value),
                            sizeof(value));
        }

        void writeGuestU64(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, uint64_t value)
        {
            writeGuestBytes(rdram,
                            runtime,
                            addr,
                            reinterpret_cast<const uint8_t *>(&value),
                            sizeof(value));
        }

        void writeGuestVec128(uint8_t *rdram, PS2Runtime *runtime, uint32_t addr, __m128i value)
        {
            alignas(16) __m128i temp = value;
            writeGuestBytes(rdram,
                            runtime,
                            addr,
                            reinterpret_cast<const uint8_t *>(&temp),
                            sizeof(temp));
        }

        void refreshPacketBuilderPendingCount(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr)
        {
            uint32_t currentAddr = 0u;
            uint32_t pendingCountAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr) ||
                !tryReadWordFromGuest(rdram, runtime, stateAddr + 8u, pendingCountAddr) ||
                pendingCountAddr == 0u ||
                currentAddr <= pendingCountAddr)
            {
                return;
            }

            uint32_t countWord = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, pendingCountAddr, countWord))
            {
                return;
            }

            const uint32_t deltaBytes = currentAddr - pendingCountAddr;
            uint32_t deltaQwords = 0u;
            if (deltaBytes >= 16u)
            {
                deltaQwords = (deltaBytes >> 4u) - 1u;
            }

            countWord = (countWord & 0xFFFF0000u) | (deltaQwords & 0xFFFFu);
            writeGuestU32(rdram, runtime, pendingCountAddr, countWord);
        }

        void writePacketBuilderCurrent(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t currentAddr)
        {
            writeGuestU32(rdram, runtime, stateAddr, currentAddr);
            refreshPacketBuilderPendingCount(rdram, runtime, stateAddr);
        }

        uint32_t reservePacketBuilderWords(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t wordCount)
        {
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return 0u;
            }

            const uint32_t reservedAddr = currentAddr;
            currentAddr += wordCount * 4u;
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
            return reservedAddr;
        }

        void alignPacketBuilderState(uint8_t *rdram,
                                     PS2Runtime *runtime,
                                     uint32_t stateAddr,
                                     uint32_t alignMode,
                                     uint32_t reserveWords)
        {
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return;
            }

            const uint32_t adjusted = (alignMode + 2u) & 31u;
            const uint32_t shift = (32u - adjusted) & 31u;
            const uint32_t lowMask = 0xFFFFFFFFu >> shift;
            const uint32_t alignedBase = currentAddr & ~lowMask;
            uint32_t targetAddr = alignedBase + (reserveWords << 2u);
            if (targetAddr < currentAddr)
            {
                targetAddr = (targetAddr + 1u) + lowMask;
            }

            const uint32_t zero = 0u;
            while (currentAddr < targetAddr)
            {
                writeGuestU32(rdram, runtime, currentAddr, zero);
                currentAddr += 4u;
            }
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
        }

        void openPacketGifTag(uint8_t *rdram,
                              R5900Context *ctx,
                              PS2Runtime *runtime,
                              uint32_t stateAddr,
                              uint32_t openAddrOffset)
        {
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
            {
                return;
            }

            writeGuestVec128(rdram, runtime, currentAddr, GPR_VEC(ctx, 5));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
            writeGuestU32(rdram, runtime, stateAddr + openAddrOffset, currentAddr);
        }

        void closePacketGifTag(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t openAddrOffset)
        {
            uint32_t openAddr = 0u;
            uint32_t currentAddr = 0u;
            if (!tryReadWordFromGuest(rdram, runtime, stateAddr + openAddrOffset, openAddr) ||
                !tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr) ||
                openAddr == 0u)
            {
                return;
            }

            uint64_t tagValue = 0u;
            if (!tryReadQwordFromGuest(rdram, runtime, openAddr, tagValue))
            {
                return;
            }

            uint32_t packetQwords = ((currentAddr - openAddr) >> 3u) - 2u;
            const uint32_t flag = static_cast<uint32_t>((tagValue >> 58u) & 0x3u);
            if (flag != 1u)
            {
                packetQwords >>= 1u;
            }
            if (flag != 2u)
            {
                uint32_t nreg = static_cast<uint32_t>((tagValue >> 60u) & 0xFu);
                if (nreg == 0u)
                {
                    nreg = 16u;
                }
                packetQwords = (packetQwords + nreg - 1u) / nreg;
            }

            tagValue += static_cast<uint64_t>(packetQwords);
            writeGuestU32(rdram, runtime, stateAddr + openAddrOffset, 0u);
            writeGuestU64(rdram, runtime, openAddr, tagValue);

            while ((currentAddr & 0xCu) != 0u)
            {
                writeGuestU32(rdram, runtime, currentAddr, 0u);
                currentAddr += 4u;
            }
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr);
        }
    }

    void sceGifPkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        const uint64_t dataValue = GPR_U64(ctx, 6);
        const uint64_t regValue = static_cast<uint64_t>(getRegU32(ctx, 5));
        writeGuestU64(rdram, runtime, currentAddr, dataValue);
        writeGuestU64(rdram, runtime, currentAddr + 8u, regValue);
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
    }

    void sceGifPkAddGsData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        uint32_t currentAddr = 0u;
        if (!tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr))
        {
            return;
        }

        writeGuestU64(rdram, runtime, currentAddr, GPR_U64(ctx, 5));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 8u);
    }

    void sceGifPkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)ctx;
        closePacketGifTag(rdram, runtime, getRegU32(ctx, 4), 12u);
    }

    void sceGifPkCnt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t countValue = getRegU32(ctx, 5);
        const uint32_t extraValue = getRegU32(ctx, 6);
        const uint32_t tagWord = getRegU32(ctx, 7) | 0x10000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[4] = {tagWord, 0u, countValue, extraValue};
        const uint32_t nextAddr = packetAddr + 16u;

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, nextAddr);
    }

    void sceGifPkEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t countValue = getRegU32(ctx, 5);
        const uint32_t extraValue = getRegU32(ctx, 6);
        const uint32_t tagWord = getRegU32(ctx, 7) | 0x70000000u;
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[4] = {tagWord, countValue, extraValue, 0u};
        const uint32_t nextAddr = packetAddr + 16u;

        writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, nextAddr);
    }

    void sceGifPkInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        initPacketBuilderState(rdram, ctx, runtime);
    }

    void sceGifPkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        openPacketGifTag(rdram, ctx, runtime, getRegU32(ctx, 4), 12u);
    }

    void sceGifPkRef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t refAddr = getRegU32(ctx, 5) & 0x9FFFFFFFu;
        const uint32_t tagWord = getRegU32(ctx, 9) | getRegU32(ctx, 6) | 0x30000000u;
        const uint32_t extra0 = getRegU32(ctx, 7);
        const uint32_t extra1 = getRegU32(ctx, 8);
        const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
        const uint32_t words[4] = {tagWord, refAddr, extra0, extra1};

        writeGuestBytes(rdram,
                        runtime,
                        packetAddr,
                        reinterpret_cast<const uint8_t *>(words),
                        sizeof(words));
        writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 16u);
    }

    void sceGifPkRefLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t stateAddr = getRegU32(ctx, 4);
        const uint32_t dbp = getRegU32(ctx, 5) & 0xFFFFu;
        const uint32_t dpsm = getRegU32(ctx, 6) & 0xFFu;
        const uint32_t dbw = getRegU32(ctx, 7) & 0xFFFFu;
        uint32_t dataAddr = getRegU32(ctx, 8);
        uint32_t qwcRemaining = getRegU32(ctx, 9);
        const uint32_t dsax = getRegU32(ctx, 10);
        const uint32_t dsay = getRegU32(ctx, 11);
        const uint32_t width = readStackU32(rdram, ctx, 0);
        const uint32_t height = readStackU32(rdram, ctx, 8);

        if (phaseDiagnosticsEnabled())
        {
            static std::atomic<uint32_t> s_f31RefLoadCount{0};
            const uint32_t n = s_f31RefLoadCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u || (n % 256u) == 0u)
            {
                std::fprintf(stderr,
                             "[F31:refload-active] n=%u state=0x%08x dbp=0x%x dbw=%u dpsm=0x%x src=0x%08x qwc=%u ds=(%u,%u) rr=(%u,%u)\n",
                             n, stateAddr, dbp, dbw, dpsm, dataAddr, qwcRemaining,
                             dsax, dsay, width, height);
            }
        }

        // Open a 4-register A+D GIF tag and emit the GS load-image setup.
        {
            const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
            const uint32_t words[4] = {0x10000000u, 0u, 0u, 0u};
            writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
            writeGuestBytes(rdram,
                            runtime,
                            packetAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 16u);

            const uint64_t giftag[2] = {makeGiftagAplusD(0u), 0xEULL};
            uint32_t currentAddr = packetAddr + 16u;
            writeGuestBytes(rdram, runtime, currentAddr, reinterpret_cast<const uint8_t *>(giftag), sizeof(giftag));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, currentAddr + 16u);
            writeGuestU32(rdram, runtime, stateAddr + 12u, currentAddr);

            const uint64_t bitbltbuf =
                (static_cast<uint64_t>(dbp) << 32u) |
                (static_cast<uint64_t>(dbw & 0xFFu) << 48u) |
                (static_cast<uint64_t>(dpsm) << 56u);
            const uint64_t trxpos =
                (static_cast<uint64_t>(dsax) << 32u) |
                (static_cast<uint64_t>(dsay) << 48u);
            const uint64_t trxreg =
                static_cast<uint64_t>(width) |
                (static_cast<uint64_t>(height) << 32u);

            {
                uint32_t addr = 0u;
                if (!tryReadWordFromGuest(rdram, runtime, stateAddr, addr))
                {
                    return;
                }
                writeGuestU64(rdram, runtime, addr, bitbltbuf);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_BITBLTBUF));
                addr += 16u;
                writeGuestU64(rdram, runtime, addr, trxpos);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_TRXPOS));
                addr += 16u;
                writeGuestU64(rdram, runtime, addr, trxreg);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_TRXREG));
                addr += 16u;
                writeGuestU64(rdram, runtime, addr, 0u);
                writeGuestU64(rdram, runtime, addr + 8u, static_cast<uint64_t>(GS_REG_TRXDIR));
                addr += 16u;
                writePacketBuilderCurrent(rdram, runtime, stateAddr, addr);
                closePacketGifTag(rdram, runtime, stateAddr, 12u);
            }
        }

        while (qwcRemaining != 0u)
        {
            const uint32_t chunkQwc = std::min<uint32_t>(qwcRemaining, 32767u);

            const uint32_t packetAddr = terminatePacketBuilderState(rdram, ctx, runtime);
            const uint32_t words[4] = {0x10000000u, 0u, 0u, 0u};
            writeGuestU32(rdram, runtime, stateAddr + 8u, packetAddr);
            writeGuestBytes(rdram,
                            runtime,
                            packetAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, packetAddr + 16u);

            const uint32_t reservedAddr = reservePacketBuilderWords(rdram, runtime, stateAddr, 4u);
            const bool isLastChunk = (chunkQwc == qwcRemaining);
            const uint64_t gifTag =
                static_cast<uint64_t>(chunkQwc) |
                (isLastChunk ? 0x0800000000008000ULL : 0x0800000000000000ULL);
            writeGuestU64(rdram, runtime, reservedAddr, gifTag);
            writeGuestU64(rdram, runtime, reservedAddr + 8u, 0u);

            const uint32_t refPacketAddr = terminatePacketBuilderState(rdram, ctx, runtime);
            const uint32_t refWords[4] = {0x30000000u | chunkQwc, dataAddr & 0x9FFFFFFFu, 0u, 0u};
            writeGuestBytes(rdram,
                            runtime,
                            refPacketAddr,
                            reinterpret_cast<const uint8_t *>(refWords),
                            sizeof(refWords));
            writePacketBuilderCurrent(rdram, runtime, stateAddr, refPacketAddr + 16u);

            qwcRemaining -= chunkQwc;
            dataAddr += chunkQwc * 16u;
        }

        if (phaseDiagnosticsEnabled())
        {
            static std::atomic<uint32_t> s_f31RefLoadDoneCount{0};
            const uint32_t n = s_f31RefLoadDoneCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 32u || (n % 256u) == 0u)
            {
                uint32_t currentAddr = 0u;
                tryReadWordFromGuest(rdram, runtime, stateAddr, currentAddr);
                std::fprintf(stderr,
                             "[F31:refload-active.done] n=%u ptr=0x%08x\n",
                             n, currentAddr);
            }
        }
    }

    void sceGifPkReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        resetPacketBuilderState(rdram, ctx, runtime);
    }

    void sceGifPkReserve(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, reservePacketBuilderWords(rdram, runtime, getRegU32(ctx, 4), getRegU32(ctx, 5)));
    }

    void sceGifPkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, terminatePacketBuilderState(rdram, ctx, runtime));
    }

    static void resetGsSyncVState()
    {
        std::lock_guard<std::mutex> lock(g_gs_sync_v_mutex);
        g_gs_sync_v_base_tick = ps2_syscalls::GetCurrentVSyncTick();
    }

    static int32_t getGsSyncVFieldForTick(uint64_t tick)
    {
        std::lock_guard<std::mutex> lock(g_gs_sync_v_mutex);
        if (tick <= g_gs_sync_v_base_tick)
        {
            return 0;
        }

        return static_cast<int32_t>((tick - g_gs_sync_v_base_tick - 1u) & 1u);
    }

    void resetGsSyncVCallbackState()
    {
        {
            std::lock_guard<std::mutex> lock(g_gs_sync_v_callback_mutex);
            g_gs_sync_v_callback_func = 0u;
            g_gs_sync_v_callback_gp = 0u;
            g_gs_sync_v_callback_sp = 0u;
            g_gs_sync_v_callback_stack_base = 0u;
            g_gs_sync_v_callback_stack_top = 0u;
            g_gs_sync_v_callback_bad_pc_logs = 0u;
        }
        resetGsSyncVState();
    }

    void dispatchGsSyncVCallback(uint8_t *rdram, PS2Runtime *runtime, uint64_t tick)
    {
        if (!rdram || !runtime)
        {
            return;
        }

        uint32_t callback = 0u;
        uint32_t gp = 0u;
        uint32_t callbackStackTop = 0u;
        const uint64_t callbackTick = (tick != 0u) ? tick : ps2_syscalls::GetCurrentVSyncTick();
        {
            std::lock_guard<std::mutex> lock(g_gs_sync_v_callback_mutex);
            callback = g_gs_sync_v_callback_func;
            gp = g_gs_sync_v_callback_gp;
            callbackStackTop = g_gs_sync_v_callback_stack_top;
            if (callback == 0u)
            {
                return;
            }
        }

        if (!runtime->hasFunction(callback))
        {
            static uint32_t s_missingCallbackLogCount = 0u;
            if (s_missingCallbackLogCount < 32u)
            {
                std::cerr << "[sceGsSyncVCallback:missing] cb=0x" << std::hex << callback
                          << " gp=0x" << gp
                          << " tick=0x" << callbackTick
                          << std::dec << std::endl;
                ++s_missingCallbackLogCount;
            }
            return;
        }

        if (callbackStackTop == 0u)
        {
            constexpr uint32_t kCallbackStackSize = 0x4000u;
            const uint32_t stackTop = runtime->reserveAsyncCallbackStack(kCallbackStackSize, 16u);
            if (stackTop != 0u)
            {
                std::lock_guard<std::mutex> lock(g_gs_sync_v_callback_mutex);
                if (g_gs_sync_v_callback_stack_top == 0u)
                {
                    g_gs_sync_v_callback_stack_base = stackTop - (kCallbackStackSize - 0x10u);
                    g_gs_sync_v_callback_stack_top = stackTop;
                }
                callbackStackTop = g_gs_sync_v_callback_stack_top;
            }
        }

        try
        {
            R5900Context callbackCtx{};
            SET_GPR_U32(&callbackCtx, 28, gp);
            SET_GPR_U32(&callbackCtx, 29, (callbackStackTop != 0u) ? callbackStackTop : (PS2_RAM_SIZE - 0x10u));
            SET_GPR_U32(&callbackCtx, 31, 0u);
            SET_GPR_U32(&callbackCtx, 4, static_cast<uint32_t>(callbackTick));
            callbackCtx.pc = callback;

            static uint32_t s_dispatchLogCount = 0u;
            const bool shouldLogDispatch = (s_dispatchLogCount < 64u);
            if (shouldLogDispatch)
            {
                RUNTIME_LOG("[sceGsSyncVCallback:dispatch] cb=0x" << std::hex << callback
                                                                  << " gp=0x" << gp
                                                                  << " sp=0x" << getRegU32(&callbackCtx, 29)
                                                                  << " tick=0x" << callbackTick
                                                                  << std::dec << std::endl);
            }

            uint32_t steps = 0u;
            while (callbackCtx.pc != 0u && !runtime->isStopRequested() && steps < 1024u)
            {
                if (!runtime->hasFunction(callbackCtx.pc))
                {
                    if (g_gs_sync_v_callback_bad_pc_logs < 16u)
                    {
                        std::cerr << "[sceGsSyncVCallback:bad-pc] pc=0x" << std::hex << callbackCtx.pc
                                  << " ra=0x" << getRegU32(&callbackCtx, 31)
                                  << " sp=0x" << getRegU32(&callbackCtx, 29)
                                  << " gp=0x" << getRegU32(&callbackCtx, 28)
                                  << std::dec << std::endl;
                        ++g_gs_sync_v_callback_bad_pc_logs;
                    }
                    callbackCtx.pc = 0u;
                    break;
                }

