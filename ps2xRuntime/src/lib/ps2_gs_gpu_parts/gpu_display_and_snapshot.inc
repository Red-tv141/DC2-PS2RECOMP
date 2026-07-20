void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_vram = vram;
    m_vramSize = vramSize;
    m_privRegs = privRegs;
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    std::memset(m_ctx, 0, sizeof(m_ctx));
    m_prim = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    m_hwregX = 0;
    m_hwregY = 0;
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    // G189: the host-presentation snapshot fields are guarded by the dedicated
    // s_g189HostPresentMutex, not m_stateMutex (already held above) — nest the lock rather
    // than reordering, since this is the only site that ever takes both (safe: every other
    // site takes at most one of the two, so there is no lock-order-inversion risk).
    {
        std::lock_guard<std::mutex> presentLock(s_g189HostPresentMutex);
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
    }
    // G176: reset clears the snapshot — advance the generation so present re-reads it.
    s_g176LatchGeneration.fetch_add(1ull, std::memory_order_release);
    s_pendingImageBytes = 0u;
    s_f31UploadPresentationCandidate = {};

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

GSContext &GS::activeContext()
{
    return m_ctx[m_prim.ctxt ? 1 : 0];
}

void GS::snapshotVRAM()
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    if (!m_vram || m_vramSize == 0)
        return;
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.resize(m_vramSize);
    std::memcpy(m_displaySnapshot.data(), m_vram, m_vramSize);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

bool GS::copyFrameToHostRgbaUnlocked(const GSFrameReg &frame,
                                     uint32_t width,
                                     uint32_t height,
                                     std::vector<uint8_t> &outPixels,
                                     bool preserveAlpha,
                                     bool useLocalMemoryLayout,
                                     bool frameBaseIsPages,
                                     uint32_t sourceOriginX,
                                     uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
    {
        return false;
    }

    static const bool s_g289PresentationAudit =
        !envFlagEnabled("DC2_G289_NO_GPU_LOCAL_COPY") &&
        (std::getenv("DC2_G289_GPU_LOCAL_COPY") == nullptr ||
         envFlagEnabled("DC2_G289_GPU_LOCAL_COPY"));
    if (s_g289PresentationAudit)
        g289_prepare_presentation_read(
            m_vram, frame.fbp, frame.fbw, frame.psm,
            sourceOriginX, sourceOriginY, width, height,
            useLocalMemoryLayout, frameBaseIsPages);

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);

    const uint32_t baseBytes = frameBaseIsPages ? (frame.fbp * 8192u) : (frame.fbp * 256u);
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbwBlocks = frame.fbw ? frame.fbw : (kHostFrameWidth / 64u);
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t strideBytes = fbwBlocks * 64u * bytesPerPixel;

    if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
    {
        const uint32_t srcPixelBytes = (frame.psm == GS_PSM_CT24) ? 3u : 4u;
        if (useLocalMemoryLayout)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = outPixels.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    const uint32_t srcY = sourceOriginY + y;
                    const uint32_t srcOff = GSPSMCT32::addrPSMCT32(basePtr, fbwBlocks, srcX, srcY);
                    if (srcOff + srcPixelBytes > m_vramSize)
                    {
                        return false;
                    }

                    dstRow[x * 4u + 0u] = m_vram[srcOff + 0u];
                    dstRow[x * 4u + 1u] = m_vram[srcOff + 1u];
                    dstRow[x * 4u + 2u] = m_vram[srcOff + 2u];
                    dstRow[x * 4u + 3u] =
                        (preserveAlpha && frame.psm != GS_PSM_CT24) ? m_vram[srcOff + 3u] : 255u;
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dstRow = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * srcPixelBytes);
                if (srcOff + srcPixelBytes > m_vramSize)
                {
                    return false;
                }

                dstRow[x * 4u + 0u] = m_vram[srcOff + 0u];
                dstRow[x * 4u + 1u] = m_vram[srcOff + 1u];
                dstRow[x * 4u + 2u] = m_vram[srcOff + 2u];
                dstRow[x * 4u + 3u] =
                    (preserveAlpha && frame.psm != GS_PSM_CT24) ? m_vram[srcOff + 3u] : 255u;
            }
        }
        return true;
    }

    if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
    {
        if (useLocalMemoryLayout)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint32_t dstOff = y * kHostFrameWidth * 4u;
                uint8_t *dst = outPixels.data() + dstOff;
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    const uint32_t srcY = sourceOriginY + y;
                    const uint32_t srcOff = addrPSMCT16Family(basePtr, fbwBlocks, frame.psm, srcX, srcY);
                    if (srcOff + sizeof(uint16_t) > m_vramSize)
                    {
                        return false;
                    }

                    uint16_t pixel = 0u;
                    std::memcpy(&pixel, m_vram + srcOff, sizeof(pixel));
                    const uint32_t r = pixel & 31u;
                    const uint32_t g = (pixel >> 5) & 31u;
                    const uint32_t b = (pixel >> 10) & 31u;
                    dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                    dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                    dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                    dst[x * 4u + 3u] = preserveAlpha ? ((pixel & 0x8000u) ? 0x80u : 0x00u) : 255u;
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dst = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * 2u);
                if (srcOff + sizeof(uint16_t) > m_vramSize)
                {
                    return false;
                }

                uint16_t pixel = 0u;
                std::memcpy(&pixel, m_vram + srcOff, sizeof(pixel));
                const uint32_t r = pixel & 31u;
                const uint32_t g = (pixel >> 5) & 31u;
                const uint32_t b = (pixel >> 10) & 31u;
                dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                dst[x * 4u + 3u] = preserveAlpha ? ((pixel & 0x8000u) ? 0x80u : 0x00u) : 255u;
            }
        }
        return true;
    }

    return false;
}

void GS::latchHostPresentationFrame()
{
    // G189: dedicated mutex, not m_stateMutex — see s_g189HostPresentMutex's comment above.
    std::lock_guard<std::mutex> lock(s_g189HostPresentMutex);
    // Bump on EVERY exit path (success, dual-merge, clear-on-invalid) — any call can
    // change the snapshot. Destructs before the lock_guard releases s_g189HostPresentMutex.
    struct G176LatchBump
    {
        ~G176LatchBump() { s_g176LatchGeneration.fetch_add(1ull, std::memory_order_release); }
    } g176Bump;
    (void)g176Bump;

    // PHASE F30: latch state probe, retained only for explicit phase tracing.
    if (phaseDiagnosticsEnabled())
    {
        static std::atomic<uint32_t> s_f30Latch{0};
        const uint32_t n = s_f30Latch.fetch_add(1, std::memory_order_relaxed) + 1u;
        if (n <= 6u || (n % 60u) == 0u)
        {
            const uint64_t dispfb1 = m_privRegs ? m_privRegs->dispfb1 : 0ull;
            const uint64_t dispfb2 = m_privRegs ? m_privRegs->dispfb2 : 0ull;
            const uint64_t pmode   = m_privRegs ? m_privRegs->pmode   : 0ull;
            std::fprintf(stderr,
                         "[F30:latch] n=%u pmode=0x%llx dispfb1=0x%llx dispfb2=0x%llx "
                         "ctx0.frame.fbp=0x%x fbw=%u psm=%u "
                         "ctx1.frame.fbp=0x%x fbw=%u psm=%u\n",
                         n,
                         static_cast<unsigned long long>(pmode),
                         static_cast<unsigned long long>(dispfb1),
                         static_cast<unsigned long long>(dispfb2),
                         m_ctx[0].frame.fbp, m_ctx[0].frame.fbw, m_ctx[0].frame.psm,
                         m_ctx[1].frame.fbp, m_ctx[1].frame.fbw, m_ctx[1].frame.psm);
        }
    }

    if (!m_privRegs || !m_vram || m_vramSize == 0u)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    const GSPmodeState pmode = decodePmode(m_privRegs->pmode);
    const GSSmode2State smode2 = decodeSMode2(m_privRegs->smode2);
    const bool applyFieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (ps2_syscalls::GetCurrentVSyncTick() & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(m_privRegs->dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(m_privRegs->dispfb2);
    const GSDisplayReadOrigin displayOrigin1 = decodeDisplayReadOrigin(m_privRegs->dispfb1);
    const GSDisplayReadOrigin displayOrigin2 = decodeDisplayReadOrigin(m_privRegs->dispfb2);

    uint32_t width1 = 0u;
    uint32_t height1 = 0u;
    uint32_t width2 = 0u;
    uint32_t height2 = 0u;
    decodeDisplaySize(m_privRegs->display1, width1, height1);
    decodeDisplaySize(m_privRegs->display2, width2, height2);

    const bool validCrt1 = pmode.enableCrt1 && hasDisplaySetup(m_privRegs->display1, displayFrame1);
    const bool validCrt2 = pmode.enableCrt2 && hasDisplaySetup(m_privRegs->display2, displayFrame2);
    uint32_t f50_12_latch_n = 0u;
    if (f50_12_trace_enabled())
    {
        static std::atomic<uint32_t> s_f50_12_latch{0};
        f50_12_latch_n = s_f50_12_latch.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (f50_12_latch_should_log(f50_12_latch_n))
        {
            std::fprintf(stderr,
                         "[F512:latch] n=%u valid1=%u valid2=%u pmode=0x%llx smode2=0x%llx "
                         "disp1(fbp=0x%x fbw=%u psm=0x%x org=%u,%u size=%ux%u) "
                         "disp2(fbp=0x%x fbw=%u psm=0x%x org=%u,%u size=%ux%u) "
                         "ctx0(fbp=0x%x fbw=%u psm=0x%x) ctx1(fbp=0x%x fbw=%u psm=0x%x) "
                         "preferred=%u prefSrc(fbp=0x%x fbw=%u psm=0x%x) prefDst=0x%x\n",
                         f50_12_latch_n,
                         validCrt1 ? 1u : 0u,
                         validCrt2 ? 1u : 0u,
                         static_cast<unsigned long long>(m_privRegs->pmode),
                         static_cast<unsigned long long>(m_privRegs->smode2),
                         displayFrame1.fbp, displayFrame1.fbw, static_cast<uint32_t>(displayFrame1.psm),
                         displayOrigin1.x, displayOrigin1.y, width1, height1,
                         displayFrame2.fbp, displayFrame2.fbw, static_cast<uint32_t>(displayFrame2.psm),
                         displayOrigin2.x, displayOrigin2.y, width2, height2,
                         m_ctx[0].frame.fbp, m_ctx[0].frame.fbw, static_cast<uint32_t>(m_ctx[0].frame.psm),
                         m_ctx[1].frame.fbp, m_ctx[1].frame.fbw, static_cast<uint32_t>(m_ctx[1].frame.psm),
                         m_hasPreferredDisplaySource ? 1u : 0u,
                         m_preferredDisplaySourceFrame.fbp,
                         m_preferredDisplaySourceFrame.fbw,
                         static_cast<uint32_t>(m_preferredDisplaySourceFrame.psm),
                         m_preferredDisplayDestFbp);
        }
    }

    auto copyDisplaySource = [&](const GSFrameReg &displayFrame,
                                 const GSDisplayReadOrigin &displayOrigin,
                                 uint32_t width,
                                 uint32_t height,
                                 bool allowPreferred,
                                 bool preserveAlpha,
                                 GSFrameReg &selectedFrame,
                                 std::vector<uint8_t> &scratch,
                                 bool &usedPreferred) -> bool
    {
        selectedFrame = displayFrame;
        scratch.clear();
        usedPreferred = false;

        if (allowPreferred &&
            m_hasPreferredDisplaySource &&
            m_preferredDisplayDestFbp == displayFrame.fbp &&
            (m_preferredDisplaySourceFrame.fbw != 0u || m_preferredDisplaySourceFrame.fbp != displayFrame.fbp))
        {
            if (copyFrameToHostRgbaUnlocked(m_preferredDisplaySourceFrame,
                                            width,
                                            height,
                                            scratch,
                                            preserveAlpha,
                                            true,
                                            false,
                                            0u,
                                            0u))
            {
                selectedFrame = m_preferredDisplaySourceFrame;
                usedPreferred = true;
                if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
                {
                    std::fprintf(stderr,
                                 "[F512:latchcopy] n=%u stage=preferred displayFbp=0x%x sourceFbp=0x%x fbw=%u psm=0x%x size=%ux%u nonblack=%u\n",
                                 f50_12_latch_n,
                                 displayFrame.fbp,
                                 selectedFrame.fbp,
                                 selectedFrame.fbw,
                                 static_cast<uint32_t>(selectedFrame.psm),
                                 width,
                                 height,
                                 countNonBlackPixels(scratch, width, height));
                }
            }
            else if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
            {
                std::fprintf(stderr,
                             "[F512:latchcopy] n=%u stage=preferred-fail displayFbp=0x%x sourceFbp=0x%x fbw=%u psm=0x%x size=%ux%u\n",
                             f50_12_latch_n,
                             displayFrame.fbp,
                             m_preferredDisplaySourceFrame.fbp,
                             m_preferredDisplaySourceFrame.fbw,
                             static_cast<uint32_t>(m_preferredDisplaySourceFrame.psm),
                             width,
                             height);
            }
        }

        if (scratch.empty())
        {
            if (!copyFrameToHostRgbaUnlocked(displayFrame,
                                             width,
                                             height,
                                             scratch,
                                             preserveAlpha,
                                             true,
                                             true,
                                             displayOrigin.x,
                                             displayOrigin.y))
            {
                if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
                {
                    std::fprintf(stderr,
                                 "[F512:latchcopy] n=%u stage=display-fail displayFbp=0x%x fbw=%u psm=0x%x org=%u,%u size=%ux%u\n",
                                 f50_12_latch_n,
                                 displayFrame.fbp,
                                 displayFrame.fbw,
                                 static_cast<uint32_t>(displayFrame.psm),
                                 displayOrigin.x,
                                 displayOrigin.y,
                                 width,
                                 height);
                }
                return false;
            }
            if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
            {
                std::fprintf(stderr,
                             "[F512:latchcopy] n=%u stage=display displayFbp=0x%x sourceFbp=0x%x fbw=%u psm=0x%x org=%u,%u size=%ux%u nonblack=%u\n",
                             f50_12_latch_n,
                             displayFrame.fbp,
                             displayFrame.fbp,
                             displayFrame.fbw,
                             static_cast<uint32_t>(displayFrame.psm),
                             displayOrigin.x,
                             displayOrigin.y,
                             width,
                             height,
                             countNonBlackPixels(scratch, width, height));
            }
        }

        if (!usedPreferred && countNonBlackPixels(scratch, width, height) == 0u)
        {
            for (int contextIndex = 0; contextIndex < 2; ++contextIndex)
            {
                const GSFrameReg &candidate = m_ctx[contextIndex].frame;
                if (candidate.fbp == selectedFrame.fbp &&
                    candidate.fbw == selectedFrame.fbw &&
                    candidate.psm == selectedFrame.psm)
                {
                    continue;
                }

                std::vector<uint8_t> candidatePixels;
                if (!copyFrameToHostRgbaUnlocked(candidate,
                                                 width,
                                                 height,
                                                 candidatePixels,
                                                 preserveAlpha,
                                                 true,
                                                 true,
                                                 0u,
                                                 0u))
                {
                    continue;
                }

                const uint32_t candidateNonBlack = countNonBlackPixels(candidatePixels, width, height);
                if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
                {
                    std::fprintf(stderr,
                                 "[F512:latchcopy] n=%u stage=context%d displayFbp=0x%x sourceFbp=0x%x fbw=%u psm=0x%x size=%ux%u nonblack=%u\n",
                                 f50_12_latch_n,
                                 contextIndex,
                                 displayFrame.fbp,
                                 candidate.fbp,
                                 candidate.fbw,
                                 static_cast<uint32_t>(candidate.psm),
                                 width,
                                 height,
                                 candidateNonBlack);
                }

                if (candidateNonBlack == 0u)
                {
                    continue;
                }

                selectedFrame = candidate;
                scratch.swap(candidatePixels);
                break;
            }

            if (uploadPresentationFallbackEnabled() &&
                countNonBlackPixels(scratch, width, height) == 0u &&
                s_f31UploadPresentationCandidate.valid)
            {
                const F31UploadPresentationCandidate &candidate = s_f31UploadPresentationCandidate;
                const uint32_t candidateWidth = std::min<uint32_t>(width, candidate.width);
                const uint32_t candidateHeight = std::min<uint32_t>(height, candidate.height);
                if (candidateWidth != 0u &&
                    candidateHeight != 0u &&
                    !candidate.pixels.empty() &&
                    countNonBlackPixels(candidate.pixels, candidateWidth, candidateHeight) != 0u)
                {
                    if (phaseDiagnosticsEnabled())
                    {
                        static std::atomic<uint32_t> s_f31UploadPresentCount{0};
                        const uint32_t n = s_f31UploadPresentCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 12u || (n % 60u) == 0u)
                        {
                            std::fprintf(stderr,
                                         "[F31:upload-present] n=%u displayFbp=0x%x srcDbp=0x%x dbw=%u psm=0x%x size=(%u,%u) nonzero=%u score=%llu\n",
                                         n,
                                         displayFrame.fbp,
                                         candidate.frame.fbp,
                                         candidate.frame.fbw,
                                         static_cast<uint32_t>(candidate.frame.psm),
                                         candidate.width,
                                         candidate.height,
                                         candidate.nonzeroBytes,
                                         static_cast<unsigned long long>(candidate.score));
                        }
                    }
                    selectedFrame = candidate.frame;
                    scratch = candidate.pixels;
                    usedPreferred = true;
                }
            }
        }

        return true;
    };

    if (!validCrt1 && !validCrt2)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    if (validCrt1 && validCrt2)
    {
        GSFrameReg selectedFrame1{};
        GSFrameReg selectedFrame2{};
        std::vector<uint8_t> rc1;
        std::vector<uint8_t> rc2;
        bool usedPreferred1 = false;
        bool usedPreferred2 = false;

        const bool copiedCrt1 = copyDisplaySource(displayFrame1, displayOrigin1, width1, height1, false, true, selectedFrame1, rc1, usedPreferred1);
        const bool copiedCrt2 = copyDisplaySource(displayFrame2, displayOrigin2, width2, height2, false, true, selectedFrame2, rc2, usedPreferred2);

        if (copiedCrt1 && copiedCrt2)
        {
            const G195PresentMode g195Mode = g195PresentationMode();
            if (g195Mode == G195PresentMode::Crt1Only || g195Mode == G195PresentMode::Crt2Only)
            {
                const bool useCrt1 = (g195Mode == G195PresentMode::Crt1Only);
                std::vector<uint8_t> selected = useCrt1 ? rc1 : rc2;
                const uint32_t width = useCrt1 ? width1 : width2;
                const uint32_t height = useCrt1 ? height1 : height2;
                if (applyFieldMode)
                {
                    applyFieldPresentation(selected, width, height, oddField);
                }
                normalizePresentationAlpha(selected, width, height);

                m_hostPresentationFrame.swap(selected);
                m_hostPresentationWidth = width;
                m_hostPresentationHeight = height;
                m_hostPresentationDisplayFbp = useCrt1 ? displayFrame1.fbp : displayFrame2.fbp;
                m_hostPresentationSourceFbp = useCrt1 ? selectedFrame1.fbp : selectedFrame2.fbp;
                m_hostPresentationUsedPreferred = false;
                m_hasHostPresentationFrame = true;
                static std::atomic<uint32_t> s_g195PresentLog{0};
                const uint32_t pn = s_g195PresentLog.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (pn <= 16u || (pn % 60u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[G195:present] n=%u mode=%s applyField=%u odd=%u "
                                 "dispFbp=0x%x sourceFbp=0x%x size=%ux%u nonblack=%u\n",
                                 pn,
                                 g195PresentationModeName(g195Mode),
                                 applyFieldMode ? 1u : 0u,
                                 oddField ? 1u : 0u,
                                 m_hostPresentationDisplayFbp,
                                 m_hostPresentationSourceFbp,
                                 m_hostPresentationWidth,
                                 m_hostPresentationHeight,
                                 countNonBlackPixels(m_hostPresentationFrame,
                                                     m_hostPresentationWidth,
                                                     m_hostPresentationHeight));
                }
                return;
            }

            if (g195Mode == G195PresentMode::FieldPairCrt2Even ||
                g195Mode == G195PresentMode::FieldPairCrt1Even)
            {
                const uint32_t width = std::max(width1, width2);
                const uint32_t height = std::max(height1, height2);
                const uint8_t bgR = static_cast<uint8_t>(m_privRegs->bgcolor & 0xFFu);
                const uint8_t bgG = static_cast<uint8_t>((m_privRegs->bgcolor >> 8) & 0xFFu);
                const uint8_t bgB = static_cast<uint8_t>((m_privRegs->bgcolor >> 16) & 0xFFu);

                std::vector<uint8_t> merged(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
                for (uint32_t y = 0; y < height; ++y)
                {
                    const bool evenLine = (y & 1u) == 0u;
                    const bool useCrt1 = (g195Mode == G195PresentMode::FieldPairCrt1Even)
                                             ? evenLine
                                             : !evenLine;
                    const std::vector<uint8_t> &src = useCrt1 ? rc1 : rc2;
                    const uint32_t srcWidth = useCrt1 ? width1 : width2;
                    const uint32_t srcHeight = useCrt1 ? height1 : height2;
                    uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        dstRow[x * 4u + 0u] = bgR;
                        dstRow[x * 4u + 1u] = bgG;
                        dstRow[x * 4u + 2u] = bgB;
                        dstRow[x * 4u + 3u] = 255u;
                    }

                    if (srcWidth == 0u || srcHeight == 0u || src.empty())
                    {
                        continue;
                    }

                    uint32_t sourceY = (y >> 1u) << 1u;
                    if (sourceY >= srcHeight)
                    {
                        sourceY = srcHeight - 1u;
                    }

                    const uint8_t *srcRow = src.data() + (sourceY * kHostFrameWidth * 4u);
                    const uint32_t copyWidth = std::min(width, srcWidth);
                    for (uint32_t x = 0; x < copyWidth; ++x)
                    {
                        dstRow[x * 4u + 0u] = srcRow[x * 4u + 0u];
                        dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u];
                        dstRow[x * 4u + 2u] = srcRow[x * 4u + 2u];
                        dstRow[x * 4u + 3u] = 255u;
                    }
                }

                m_hostPresentationFrame.swap(merged);
                m_hostPresentationWidth = width;
                m_hostPresentationHeight = height;
                m_hostPresentationDisplayFbp = displayFrame1.fbp;
                m_hostPresentationSourceFbp = selectedFrame1.fbp;
                m_hostPresentationUsedPreferred = false;
                m_hasHostPresentationFrame = true;
                static std::atomic<uint32_t> s_g195PresentLog{0};
                const uint32_t pn = s_g195PresentLog.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (pn <= 16u || (pn % 60u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[G195:present] n=%u mode=%s applyField=%u odd=%u "
                                 "disp1Org=%u,%u disp2Org=%u,%u size=%ux%u nonblack=%u\n",
                                 pn,
                                 g195PresentationModeName(g195Mode),
                                 applyFieldMode ? 1u : 0u,
                                 oddField ? 1u : 0u,
                                 displayOrigin1.x,
                                 displayOrigin1.y,
                                 displayOrigin2.x,
                                 displayOrigin2.y,
                                 m_hostPresentationWidth,
                                 m_hostPresentationHeight,
                                 countNonBlackPixels(m_hostPresentationFrame,
                                                     m_hostPresentationWidth,
                                                     m_hostPresentationHeight));
                }
                return;
            }

            const uint32_t width = std::max(width1, width2);
            const uint32_t height = std::max(height1, height2);
            const uint8_t bgR = static_cast<uint8_t>(m_privRegs->bgcolor & 0xFFu);
            const uint8_t bgG = static_cast<uint8_t>((m_privRegs->bgcolor >> 8) & 0xFFu);
            const uint8_t bgB = static_cast<uint8_t>((m_privRegs->bgcolor >> 16) & 0xFFu);
            const uint8_t bgA = pmode.alp;

            std::vector<uint8_t> merged(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    dstRow[x * 4u + 0u] = bgR;
                    dstRow[x * 4u + 1u] = bgG;
                    dstRow[x * 4u + 2u] = bgB;
                    dstRow[x * 4u + 3u] = bgA;
                }
            }

            if (!pmode.slbg)
            {
                for (uint32_t y = 0; y < height2; ++y)
                {
                    const uint8_t *srcRow = rc2.data() + (y * kHostFrameWidth * 4u);
                    uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0; x < width2; ++x)
                    {
                        dstRow[x * 4u + 0u] = srcRow[x * 4u + 0u];
                        dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u];
                        dstRow[x * 4u + 2u] = srcRow[x * 4u + 2u];
                        dstRow[x * 4u + 3u] = srcRow[x * 4u + 3u];
                    }
                }
            }

            for (uint32_t y = 0; y < height1; ++y)
            {
                const uint8_t *srcRow = rc1.data() + (y * kHostFrameWidth * 4u);
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t srcR = srcRow[x * 4u + 0u];
                    const uint8_t srcG = srcRow[x * 4u + 1u];
                    const uint8_t srcB = srcRow[x * 4u + 2u];
                    const uint8_t srcA = srcRow[x * 4u + 3u];
                    const uint8_t dstR = dstRow[x * 4u + 0u];
                    const uint8_t dstG = dstRow[x * 4u + 1u];
                    const uint8_t dstB = dstRow[x * 4u + 2u];
                    const uint8_t dstA = dstRow[x * 4u + 3u];
                    const uint32_t factor = pmode.mmod
                                                ? static_cast<uint32_t>(pmode.alp)
                                                : std::min<uint32_t>(255u, static_cast<uint32_t>(srcA) * 2u);

                    dstRow[x * 4u + 0u] = blendPresentationChannel(srcR, dstR, factor);
                    dstRow[x * 4u + 1u] = blendPresentationChannel(srcG, dstG, factor);
                    dstRow[x * 4u + 2u] = blendPresentationChannel(srcB, dstB, factor);
                    dstRow[x * 4u + 3u] = pmode.amod ? dstA : srcA;
                }
            }

            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *row = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    row[x * 4u + 3u] = 255u;
                }
            }

            if (applyFieldMode)
            {
                applyFieldPresentation(merged, width, height, oddField);
            }

            m_hostPresentationFrame.swap(merged);
            m_hostPresentationWidth = width;
            m_hostPresentationHeight = height;
            m_hostPresentationDisplayFbp = displayFrame1.fbp;
            m_hostPresentationSourceFbp = selectedFrame1.fbp;
            m_hostPresentationUsedPreferred = false;
            m_hasHostPresentationFrame = true;
            if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
            {
                std::fprintf(stderr,
                             "[F512:latchdone] n=%u mode=dual displayFbp=0x%x sourceFbp=0x%x usedPreferred=0 size=%ux%u nonblack=%u\n",
                             f50_12_latch_n,
                             m_hostPresentationDisplayFbp,
                             m_hostPresentationSourceFbp,
                             m_hostPresentationWidth,
                             m_hostPresentationHeight,
                             countNonBlackPixels(m_hostPresentationFrame, m_hostPresentationWidth, m_hostPresentationHeight));
            }
            return;
        }
    }

    const GSFrameReg &displayFrame = validCrt1 ? displayFrame1 : displayFrame2;
    const uint32_t width = validCrt1 ? width1 : width2;
    const uint32_t height = validCrt1 ? height1 : height2;

    GSFrameReg selectedFrame = displayFrame;
    std::vector<uint8_t> scratch;
    bool usedPreferred = false;
    const GSDisplayReadOrigin &displayOrigin = validCrt1 ? displayOrigin1 : displayOrigin2;
    if (!copyDisplaySource(displayFrame, displayOrigin, width, height, true, false, selectedFrame, scratch, usedPreferred))
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = displayFrame.fbp;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    if (applyFieldMode)
    {
        applyFieldPresentation(scratch, width, height, oddField);
    }

    normalizePresentationAlpha(scratch, width, height);

    m_hostPresentationFrame.swap(scratch);
    m_hostPresentationWidth = width;
    m_hostPresentationHeight = height;
    m_hostPresentationDisplayFbp = displayFrame.fbp;
    m_hostPresentationSourceFbp = selectedFrame.fbp;
    m_hostPresentationUsedPreferred = usedPreferred;
    m_hasHostPresentationFrame = true;
    if (f50_12_trace_enabled() && f50_12_latch_should_log(f50_12_latch_n))
    {
        std::fprintf(stderr,
                     "[F512:latchdone] n=%u mode=single displayFbp=0x%x sourceFbp=0x%x usedPreferred=%u size=%ux%u nonblack=%u\n",
                     f50_12_latch_n,
                     m_hostPresentationDisplayFbp,
                     m_hostPresentationSourceFbp,
                     m_hostPresentationUsedPreferred ? 1u : 0u,
                     m_hostPresentationWidth,
                     m_hostPresentationHeight,
                     countNonBlackPixels(m_hostPresentationFrame, m_hostPresentationWidth, m_hostPresentationHeight));
    }
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    // G189: dedicated mutex, not m_stateMutex — see s_g189HostPresentMutex's comment above.
    std::lock_guard<std::mutex> lock(s_g189HostPresentMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.assign(packedRowBytes * static_cast<size_t>(outHeight), 0u);
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

