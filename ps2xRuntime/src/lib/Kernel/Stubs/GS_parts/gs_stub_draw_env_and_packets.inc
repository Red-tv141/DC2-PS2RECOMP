                auto step = runtime->lookupFunction(callbackCtx.pc);
                if (!step)
                {
                    break;
                }
                ++steps;
                step(rdram, &callbackCtx, runtime);
            }

            if (shouldLogDispatch)
            {
                RUNTIME_LOG("[sceGsSyncVCallback:return] cb=0x" << std::hex << callback
                                                                << " finalPc=0x" << callbackCtx.pc
                                                                << " ra=0x" << getRegU32(&callbackCtx, 31)
                                                                << " steps=0x" << steps
                                                                << std::dec << std::endl);
                ++s_dispatchLogCount;
            }
        }
        catch (const std::exception &e)
        {
            static uint32_t warnCount = 0u;
            if (warnCount < 8u)
            {
                std::cerr << "[sceGsSyncVCallback] callback exception: " << e.what() << std::endl;
                ++warnCount;
            }
        }
    }

    void sceGsExecLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        uint32_t srcAddr = getRegU32(ctx, 5);

        GsImageMem img{};
        if (!runtime || !runtime->syncCoreSubsystems() || !readGsImage(rdram, imgAddr, img))
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t rowBytes = bytesForPixels(img.psm, static_cast<uint32_t>(img.width));
        if (rowBytes == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t fbw = img.vram_width ? img.vram_width : std::max<uint32_t>(1, (img.width + 63) / 64);
        const uint32_t totalImageBytes = rowBytes * static_cast<uint32_t>(img.height);
        const uint32_t headerQwc = 6u;
        const uint32_t imageQwc = (totalImageBytes + 15u) / 16u;
        const uint32_t totalQwc = headerQwc + imageQwc;

        uint32_t pktAddr = runtime->guestMalloc(totalQwc * 16u, 16u);
        if (pktAddr == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint8_t *pkt = getMemPtr(rdram, pktAddr);
        const uint8_t *src = getConstMemPtr(rdram, srcAddr);
        if (!pkt || !src)
        {
            runtime->guestFree(pktAddr);
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t dbp = (static_cast<uint32_t>(img.vram_addr) * 2048u) / 256u;
        uint32_t dsax = static_cast<uint32_t>(img.x);
        uint32_t dsay = static_cast<uint32_t>(img.y);

        // Full messy
        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
        q[0] = makeGiftagAplusD(4u);
        q[1] = 0xEULL;
        q[2] = (static_cast<uint64_t>(img.psm & 0x3Fu) << 24) | (static_cast<uint64_t>(1u) << 16) |
               (static_cast<uint64_t>(dbp & 0x3FFFu) << 32) | (static_cast<uint64_t>(fbw & 0x3Fu) << 48) |
               (static_cast<uint64_t>(img.psm & 0x3Fu) << 56);
        q[3] = 0x50ULL;
        q[4] = (static_cast<uint64_t>(dsay & 0x7FFu) << 48) | (static_cast<uint64_t>(dsax & 0x7FFu) << 32);
        q[5] = 0x51ULL;
        q[6] = (static_cast<uint64_t>(img.height) << 32) | static_cast<uint64_t>(img.width);
        q[7] = 0x52ULL;
        q[8] = 0ULL;
        q[9] = 0x53ULL;
        q[10] = (static_cast<uint64_t>(2) << 58) | (static_cast<uint64_t>(imageQwc) & 0x7FFF) |
                (1ULL << 15);
        q[11] = 0ULL;

        std::memcpy(pkt + headerQwc * 16u, src, totalImageBytes);

        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
        auto &mem = runtime->memory();
        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
        mem.writeIORegister(GIF_CHANNEL + 0x20u, totalQwc & 0xFFFFu);
        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
        mem.processPendingTransfers();
        runtime->guestFree(pktAddr);

        setReturnS32(ctx, 0);
    }

    void sceGsExecStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        uint32_t dstAddr = getRegU32(ctx, 5);

        GsImageMem img{};
        if (!runtime || !runtime->syncCoreSubsystems() || !readGsImage(rdram, imgAddr, img))
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t rowBytes = bytesForPixels(img.psm, static_cast<uint32_t>(img.width));
        if (rowBytes == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t fbw = img.vram_width ? img.vram_width : std::max<uint32_t>(1, (img.width + 63) / 64);
        const uint32_t totalImageBytes = rowBytes * static_cast<uint32_t>(img.height);

        uint8_t *dst = getMemPtr(rdram, dstAddr);
        if (!dst)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t sbp = (static_cast<uint32_t>(img.vram_addr) * 2048u) / 256u;
        uint64_t bitbltbuf = (static_cast<uint64_t>(sbp & 0x3FFFu) << 0) |
                             (static_cast<uint64_t>(fbw & 0x3Fu) << 16) |
                             (static_cast<uint64_t>(img.psm & 0x3Fu) << 24) |
                             (static_cast<uint64_t>(0u) << 32) |
                             (static_cast<uint64_t>(1u) << 48) |
                             (static_cast<uint64_t>(0u) << 56);
        uint64_t trxpos = (static_cast<uint64_t>(img.x & 0x7FFu) << 0) |
                          (static_cast<uint64_t>(img.y & 0x7FFu) << 16) |
                          (static_cast<uint64_t>(0u) << 32) |
                          (static_cast<uint64_t>(0u) << 48);
        uint64_t trxreg = static_cast<uint64_t>(img.height) << 32 | static_cast<uint64_t>(img.width);

        uint32_t pktAddr = runtime->guestMalloc(80u, 16u);
        if (pktAddr == 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint8_t *pkt = getMemPtr(rdram, pktAddr);
        if (!pkt)
        {
            runtime->guestFree(pktAddr);
            setReturnS32(ctx, -1);
            return;
        }

        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
        q[0] = makeGiftagAplusD(4u);
        q[1] = 0xEULL;
        q[2] = bitbltbuf;
        q[3] = 0x50ULL;
        q[4] = trxpos;
        q[5] = 0x51ULL;
        q[6] = trxreg;
        q[7] = 0x52ULL;
        q[8] = 1ULL;
        q[9] = 0x53ULL;

        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
        auto &mem = runtime->memory();
        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
        mem.writeIORegister(GIF_CHANNEL + 0x20u, 5u);
        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
        mem.processPendingTransfers();

        runtime->gs().consumeLocalToHostBytes(dst, totalImageBytes);
        runtime->guestFree(pktAddr);

        setReturnS32(ctx, 0);
    }

    void sceGsGetGParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t addr = writeGsGParamToScratch(runtime);
        setReturnU32(ctx, addr);
    }

    void sceGsPutDispEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        GsDispEnvMem env{};
        if (!readGsDispEnv(rdram, envAddr, env))
        {
            setReturnS32(ctx, -1);
            return;
        }
        g177ApplyGsDispEnv(runtime, env);
        setReturnS32(ctx, 0);
    }

    void sceGsPutDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        GsRegPairMem pairs[8]{};
        if (!readGsRegPairs(rdram, envAddr, pairs, 8u))
        {
            setReturnS32(ctx, -1);
            return;
        }
        g177ApplyGsRegPairs(runtime, pairs, 8u);
        setReturnS32(ctx, 0);
    }

    void sceGsResetGraph(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t mode = getRegU32(ctx, 4);
        uint32_t interlace = getRegU32(ctx, 5);
        uint32_t omode = getRegU32(ctx, 6);
        uint32_t ffmode = getRegU32(ctx, 7);

        if (mode == 0)
        {
            if (runtime && !runtime->syncCoreSubsystems())
            {
                setReturnS32(ctx, -1);
                return;
            }

            g_gparam.interlace = static_cast<uint8_t>(interlace & 0x1);
            g_gparam.omode = static_cast<uint8_t>(omode & 0xFF);
            g_gparam.ffmode = static_cast<uint8_t>(ffmode & 0x1);
            writeGsGParamToScratch(runtime);
            resetGsSyncVState();

            uint64_t pmode = makePmode(1, 0, 0, 0, 0, 0x80);
            uint64_t smode2 = (interlace & 0x1) | ((ffmode & 0x1) << 1);
            uint64_t dispfb = makeDispFb(0, 10, 0, 0, 0);
            uint64_t display = makeDisplay(0, 0, 0, 0, 639, 447);
            uint64_t bgcolor = 0ULL;

            if (runtime)
            {
                uint32_t pktAddr = runtime->guestMalloc(128u, 16u);
                if (pktAddr != 0u)
                {
                    uint8_t *pkt = getMemPtr(rdram, pktAddr);
                    if (pkt)
                    {
                        uint64_t *q = reinterpret_cast<uint64_t *>(pkt);
                        q[0] = makeGiftagAplusD(7u);
                        q[1] = 0xEULL;
                        q[2] = pmode;
                        q[3] = 0x41ULL;
                        q[4] = smode2;
                        q[5] = 0x42ULL;
                        q[6] = dispfb;
                        q[7] = 0x59ULL;
                        q[8] = display;
                        q[9] = 0x5aULL;
                        q[10] = dispfb;
                        q[11] = 0x5bULL;
                        q[12] = display;
                        q[13] = 0x5cULL;
                        q[14] = bgcolor;
                        q[15] = 0x5fULL;
                        constexpr uint32_t GIF_CHANNEL = 0x1000A000;
                        constexpr uint32_t CHCR_STR_MODE0 = 0x101u;
                        auto &mem = runtime->memory();
                        mem.writeIORegister(GIF_CHANNEL + 0x10u, pktAddr);
                        mem.writeIORegister(GIF_CHANNEL + 0x20u, 8u);
                        mem.writeIORegister(GIF_CHANNEL + 0x00u, CHCR_STR_MODE0);
                        mem.processPendingTransfers();
                        runtime->guestFree(pktAddr);
                    }
                    else
                    {
                        runtime->guestFree(pktAddr);
                    }
                }
            }
        }

        setReturnS32(ctx, 0);
    }

    void sceGsResetPath(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefClear(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)ctx;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t psm = getRegU32(ctx, 5);
        uint32_t w = getRegU32(ctx, 6);
        uint32_t h = getRegU32(ctx, 7);
        const GsTrailingArgs3 trailing = decodeGsTrailingArgs3(rdram, ctx);
        const uint32_t ztest = trailing.arg0;
        const uint32_t zpsm = trailing.arg1;
        const uint32_t clear = trailing.arg2;

        if (w == 0u)
        {
            w = 640u;
        }
        if (h == 0u)
        {
            h = 448u;
        }

        const uint32_t fbw = std::max<uint32_t>(1u, (w + 63u) / 64u);
        const uint64_t pmode = makePmode(1u, 1u, 0u, 0u, 0u, 0x80u);
        const uint64_t smode2 =
            (static_cast<uint64_t>(g_gparam.interlace & 0x1u) << 0) |
            (static_cast<uint64_t>(g_gparam.ffmode & 0x1u) << 1);
        const uint64_t display = makeDisplay(636u, 32u, 0u, 0u, w - 1u, h - 1u);

        const int32_t drawWidth = static_cast<int32_t>(w);
        const int32_t drawHeight = static_cast<int32_t>(h);

        uint32_t zbufAddr = 0u;
        {
            R5900Context temp = *ctx;
            sceGszbufaddr(rdram, &temp, runtime);
            zbufAddr = getRegU32(&temp, 2);
        }

        const uint32_t fbp1 = zbufAddr;
        const uint64_t dispfb0 = makeDispFb(fbp1, fbw, psm, 0u, 0u);
        const uint64_t dispfb1 = makeDispFb(0u, fbw, psm, 0u, 0u);

        GsDBuffDcMem db{};
        db.disp[0].pmode = pmode;
        db.disp[0].smode2 = smode2;
        db.disp[0].dispfb = dispfb0;
        db.disp[0].display = display;
        db.disp[0].bgcolor = 0u;
        db.disp[1] = db.disp[0];
        db.disp[1].dispfb = dispfb1;

        const bool seedClear = clear != 0u;
        db.giftag0 = {makeGiftagAplusD(seedClear ? 22u : 16u), 0xEULL};
        seedGsDrawEnv1(db.draw01, drawWidth, drawHeight, 0u, fbw, psm, zbufAddr, zpsm, ztest, false);
        seedGsDrawEnv2(db.draw02, drawWidth, drawHeight, 0u, fbw, psm, zbufAddr, zpsm, ztest, false);
        db.giftag1 = db.giftag0;
        seedGsDrawEnv1(db.draw11, drawWidth, drawHeight, fbp1, fbw, psm, zbufAddr, zpsm, ztest, false);
        seedGsDrawEnv2(db.draw12, drawWidth, drawHeight, fbp1, fbw, psm, zbufAddr, zpsm, ztest, false);
        if (seedClear)
        {
            seedGsClearPacket(db.clear0, drawWidth, drawHeight, 0u, ztest, false);
            seedGsClearPacket(db.clear1, drawWidth, drawHeight, 0u, ztest, true);
        }

        if (!writeGsDBuffDc(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefDBuff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t psm = getRegU32(ctx, 5);
        uint32_t w = getRegU32(ctx, 6);
        uint32_t h = getRegU32(ctx, 7);
        // G194: DC2 is EABI — args 5..7 arrive in $t0/$t1/$t2, not the o32 stack
        // slots. The old readStackU32(16/20/24) reads picked up caller stack junk,
        // seeding zpsm=0 (Z32) into the dbuff struct where the game asked for
        // 0x31 (Z24) — the game re-reads this struct for every per-material ZBUF
        // write, so the whole town/field Z buffer ran in the wrong format.
        // decodeGsTrailingArgs3 is regs-first with a stack fallback (same as the
        // Dc variant above).
        const GsTrailingArgs3 trailing = decodeGsTrailingArgs3(rdram, ctx);
        const uint32_t ztest = trailing.arg0;
        const uint32_t zpsm = trailing.arg1;
        const uint32_t clear = trailing.arg2;
        (void)clear;

        if (w == 0u)
        {
            w = 640u;
        }
        if (h == 0u)
        {
            h = 448u;
        }

        const uint32_t fbw = std::max<uint32_t>(1u, (w + 63u) / 64u);
        const uint64_t pmode = makePmode(1u, 1u, 0u, 0u, 0u, 0x80u);
        const uint64_t smode2 =
            (static_cast<uint64_t>(g_gparam.interlace & 0x1u) << 0) |
            (static_cast<uint64_t>(g_gparam.ffmode & 0x1u) << 1);
        const uint64_t dispfb = makeDispFb(0u, fbw, psm, 0u, 0u);
        const uint64_t display = makeDisplay(636u, 32u, 0u, 0u, w - 1u, h - 1u);

        const int32_t drawWidth = static_cast<int32_t>(w);
        const int32_t drawHeight = static_cast<int32_t>(h);

        uint32_t zbufAddr = 0u;
        {
            R5900Context temp = *ctx;
            sceGszbufaddr(rdram, &temp, runtime);
            zbufAddr = getRegU32(&temp, 2);
        }

        GsDBuffMem db{};
        db.disp[0].pmode = pmode;
        db.disp[0].smode2 = smode2;
        db.disp[0].dispfb = dispfb;
        db.disp[0].display = display;
        db.disp[0].bgcolor = 0u;
        db.disp[1] = db.disp[0];

        db.giftag0 = {makeGiftagAplusD(14u), 0x0E0E0E0E0E0E0E0EULL};
        seedGsDrawEnv1(db.draw0, drawWidth, drawHeight, 0u, fbw, psm, zbufAddr, zpsm, ztest, false);
        db.giftag1 = db.giftag0;
        seedGsDrawEnv1(db.draw1, drawWidth, drawHeight, 0u, fbw, psm, zbufAddr, zpsm, ztest, false);

        if (!writeGsDBuff(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefDispEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t psm = getRegU32(ctx, 5);
        uint32_t w = getRegU32(ctx, 6);
        uint32_t h = getRegU32(ctx, 7);
        const GsTrailingArgs2 trailing = decodeGsTrailingArgs2(rdram, ctx);
        uint32_t dx = trailing.arg0;
        uint32_t dy = trailing.arg1;

        if (w == 0)
            w = 640;
        if (h == 0)
            h = 448;

        uint32_t fbw = (w + 63) / 64;
        uint64_t dispfb = makeDispFb(0, fbw, psm, 0, 0);
        uint64_t display = makeDisplay(dx, dy, 0, 0, w - 1, h - 1);

        writeGsDispEnv(rdram, envAddr, display, dispfb);
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t param_2 = getRegU32(ctx, 5);
        int32_t w = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFF));
        int32_t h = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFF));
        const GsTrailingArgs2 trailing = decodeGsTrailingArgs2(rdram, ctx);
        uint32_t param_5 = trailing.arg0;
        uint32_t param_6 = trailing.arg1;

        if (w <= 0)
            w = 640;
        if (h <= 0)
            h = 448;

        uint32_t psm = param_2 & 0xFU;
        uint32_t fbw = ((static_cast<uint32_t>(w) + 63u) >> 6) & 0x3FU;
        sceGszbufaddr(rdram, ctx, runtime);
        int32_t zbuf = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 2) & 0xFFFF));

        GsDrawEnv1Mem env{};
        seedGsDrawEnv1(env,
                       w,
                       h,
                       0u,
                       fbw,
                       psm,
                       static_cast<uint32_t>(zbuf),
                       param_6 & 0xFu,
                       param_5 & 0x3u,
                       (param_2 & 2u) != 0u);

        uint8_t *const ptr = getMemPtr(rdram, envAddr);
        if (!ptr)
        {
            setReturnS32(ctx, 8);
            return;
        }
        std::memcpy(ptr, &env, sizeof(env));

        setReturnS32(ctx, 8);
    }

    void sceGsSetDefDrawEnv2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t envAddr = getRegU32(ctx, 4);
        uint32_t param_2 = getRegU32(ctx, 5);
        int32_t w = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 6) & 0xFFFF));
        int32_t h = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 7) & 0xFFFF));
        const GsTrailingArgs2 trailing = decodeGsTrailingArgs2(rdram, ctx);
        uint32_t param_5 = trailing.arg0;
        uint32_t param_6 = trailing.arg1;

        if (w <= 0)
            w = 640;
        if (h <= 0)
            h = 448;

        uint32_t psm = param_2 & 0xFU;
        uint32_t fbw = ((static_cast<uint32_t>(w) + 63u) >> 6) & 0x3FU;
        sceGszbufaddr(rdram, ctx, runtime);
        int32_t zbuf = static_cast<int32_t>(static_cast<int16_t>(getRegU32(ctx, 2) & 0xFFFF));

        GsDrawEnv2Mem env{};
        seedGsDrawEnv2(env,
                       w,
                       h,
                       0u,
                       fbw,
                       psm,
                       static_cast<uint32_t>(zbuf),
                       param_6 & 0xFu,
                       param_5 & 0x3u,
                       (param_2 & 2u) != 0u);

        uint8_t *const ptr = getMemPtr(rdram, envAddr);
        if (!ptr)
        {
            setReturnS32(ctx, 8);
            return;
        }

        std::memcpy(ptr, &env, sizeof(env));
        setReturnS32(ctx, 8);
    }

    void sceGsSetDefLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t imgAddr = getRegU32(ctx, 4);
        const GsSetDefImageArgs args = decodeGsSetDefImageArgs(rdram, ctx);

        GsImageMem img{};
        img.x = static_cast<uint16_t>(args.x);
        img.y = static_cast<uint16_t>(args.y);
        img.width = static_cast<uint16_t>(args.width);
        img.height = static_cast<uint16_t>(args.height);
        img.vram_addr = static_cast<uint16_t>(args.vramAddr);
        img.vram_width = static_cast<uint8_t>(args.vramWidth);
        img.psm = static_cast<uint8_t>(args.psm);

        writeGsImage(rdram, imgAddr, img);
        setReturnS32(ctx, 0);
    }

    void sceGsSetDefStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceGsSetDefLoadImage(rdram, ctx, runtime);
    }

    void sceGsSwapDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t envAddr = getRegU32(ctx, 4);
        const uint32_t which = getRegU32(ctx, 5) & 1u;

        // PHASE F30: probe entry, retained only for explicit phase tracing.
        if (phaseDiagnosticsEnabled())
        {
            static std::atomic<uint32_t> s_f30SwapDc{0};
            const uint32_t n = s_f30SwapDc.fetch_add(1, std::memory_order_relaxed) + 1u;
            if (n <= 8u || (n % 60u) == 0u)
            {
                std::fprintf(stderr, "[F30:swapDc] n=%u envAddr=0x%x which=%u\n",
                             n, envAddr, which);
            }
        }

        GsDBuffDcMem db{};
        if (!runtime || !readGsDBuffDc(rdram, envAddr, db))
        {
            setReturnS32(ctx, -1);
            return;
        }
