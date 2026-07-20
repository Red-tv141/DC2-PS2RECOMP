                if (len == 0u || (offset + len) > sendSize)
                {
                    break;
                }

                applySoundDriverCommandUnlocked(rdram, packet + offset);
                offset += static_cast<uint32_t>(len);
            }
        }

        bool handleSoundDriverRpcServiceImpl(uint8_t *rdram, PS2Runtime *runtime,
                                             uint32_t sid, uint32_t rpcNum,
                                             uint32_t sendBuf, uint32_t sendSize,
                                             uint32_t recvBuf, uint32_t recvSize,
                                             uint32_t &resultPtr,
                                             bool &signalNowaitCompletion)
        {
            resultPtr = 0u;
            signalNowaitCompletion = false;

            if (!runtime || !rdram)
            {
                return false;
            }

            if (sid == IOP_SID_SNDDRV_COMMAND && rpcNum == IOP_RPC_SNDDRV_SUBMIT)
            {
                handleSoundDriverCommandBuffer(rdram, runtime, sendBuf, sendSize);
                return true;
            }

            if (sid == IOP_SID_SNDDRV_STATE &&
                (rpcNum == IOP_RPC_SNDDRV_GET_STATUS_ADDR || rpcNum == IOP_RPC_SNDDRV_GET_ADDR_TABLE))
            {
                uint32_t responseWord = 0u;
                {
                    std::lock_guard<std::mutex> lock(g_rpc_mutex);
                    if (!checkSoundDriverMemoryUnlocked(rdram, runtime))
                    {
                        return false;
                    }
                    responseWord =
                        (rpcNum == IOP_RPC_SNDDRV_GET_STATUS_ADDR) ? g_soundDriverRpcState.statusAddr
                                                                   : g_soundDriverRpcState.addrTableAddr;
                }

                if (recvBuf && recvSize >= sizeof(uint32_t))
                {
                    (void)writeGuestU32(rdram, recvBuf, responseWord);
                    if (recvSize > sizeof(uint32_t))
                    {
                        rpcZeroRdram(rdram, recvBuf + sizeof(uint32_t), recvSize - sizeof(uint32_t));
                    }
                    resultPtr = recvBuf;
                }

                signalNowaitCompletion = true;
                return true;
            }

            return false;
        }

    } // namespace

    bool handleSoundDriverRpcService(uint8_t *rdram, PS2Runtime *runtime,
                                     uint32_t sid, uint32_t rpcNum,
                                     uint32_t sendBuf, uint32_t sendSize,
                                     uint32_t recvBuf, uint32_t recvSize,
                                     uint32_t &resultPtr,
                                     bool &signalNowaitCompletion)
    {
        return handleSoundDriverRpcServiceImpl(rdram, runtime,
                                               sid, rpcNum,
                                               sendBuf, sendSize,
                                               recvBuf, recvSize,
                                               resultPtr,
                                               signalNowaitCompletion);
    }

    namespace
    {

        bool signalRpcCompletionSema(uint32_t semaId)
        {
            if (semaId == 0u || semaId > 0xFFFFu)
            {
                return false;
            }

            auto sema = lookupSemaInfo(static_cast<int>(semaId));
            if (!sema)
            {
                return false;
            }

            bool signaled = false;
            {
                std::lock_guard<std::mutex> lock(sema->m);
                if (!sema->deleted && sema->count < sema->maxCount)
                {
                    sema->count++;
                    signaled = true;
                }
            }

            if (signaled)
            {
                sema->cv.notify_one();
            }
            return signaled;
        }

        const char *dtxUrpcCommandName(uint32_t command)
        {
            switch (command)
            {
            case 0u:
                return "SJX_CREATE";
            case 1u:
                return "SJX_DESTROY";
            case 2u:
                return "SJX_RESET";
            case 3u:
                return "SJX_REINIT";
            case 8u:
                return "PS2RNA_CREATE";
            case 9u:
                return "PS2RNA_DESTROY";
            case 10u:
                return "PS2RNA_REINIT";
            case 32u:
                return "SJRMT_RBF_CREATE";
            case 33u:
                return "SJRMT_MEM_CREATE";
            case 34u:
                return "SJRMT_UNI_CREATE";
            case 35u:
                return "SJRMT_DESTROY";
            case 36u:
                return "SJRMT_GET_UUID";
            case 37u:
                return "SJRMT_RESET";
            case 38u:
                return "SJRMT_GET_CHUNK";
            case 39u:
                return "SJRMT_UNGET_CHUNK";
            case 40u:
                return "SJRMT_PUT_CHUNK";
            case 41u:
                return "SJRMT_GET_NUM_DATA";
            case 42u:
                return "SJRMT_IS_GET_CHUNK";
            case 43u:
                return "SJRMT_INIT";
            case 44u:
                return "SJRMT_FINISH";
            default:
                return "UNKNOWN";
            }
        }

        const char *dtxStreamName(uint32_t streamId)
        {
            switch (streamId)
            {
            case 0u:
                return "room";
            case 1u:
                return "data";
            default:
                return "other";
            }
        }

        bool dtxMatchesTransfer(const DtxTransferState &state, uint32_t srcAddr, uint32_t dstAddr, uint32_t sizeBytes)
        {
            (void)dstAddr;
            return state.eeWorkAddr != 0u &&
                   state.wkSize >= 64u &&
                   state.eeWorkAddr == srcAddr &&
                   state.wkSize == sizeBytes;
        }

        bool dtxCopyBytes(uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, uint32_t len)
        {
            for (uint32_t i = 0; i < len; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                uint8_t *dst = getMemPtr(rdram, dstAddr + i);
                if (!src || !dst)
                {
                    return false;
                }
                *dst = *src;
            }
            return true;
        }

        void dtxAppendToSjrmtData(uint8_t *rdram, DtxSjrmtState &state, uint32_t srcDataAddr, uint32_t requestedLen)
        {
            if (!rdram)
            {
                return;
            }

            const uint32_t cap = (state.wkSize == 0u) ? 0x4000u : state.wkSize;
            if (cap == 0u || state.wkAddr == 0u || state.roomBytes == 0u || requestedLen == 0u)
            {
                return;
            }

            const uint32_t requestedCopyLen = std::min(requestedLen, state.roomBytes);
            uint32_t copiedLen = 0u;
            for (uint32_t i = 0; i < requestedCopyLen; ++i)
            {
                const uint32_t writeOffset = (state.writePos + i) % cap;
                if (!dtxCopyBytes(rdram, state.wkAddr + writeOffset, srcDataAddr + i, 1u))
                {
                    break;
                }
                ++copiedLen;
            }

            state.writePos = (state.writePos + copiedLen) % cap;
            state.roomBytes -= copiedLen;
            state.dataBytes = std::min(cap, state.dataBytes + copiedLen);
        }

        void dtxConsumeSjrmtDataLocked(DtxSjrmtState &state)
        {
            const uint32_t cap = (state.wkSize == 0u) ? 0x4000u : state.wkSize;
            if (cap == 0u || state.dataBytes == 0u)
            {
                return;
            }

            const uint32_t consumed = std::min(cap, state.dataBytes);
            state.readPos = (state.readPos + consumed) % cap;
            state.dataBytes -= consumed;
            state.roomBytes = std::min(cap, state.roomBytes + consumed);
        }

        void dtxConsumeActivePs2RnaStreamsLocked()
        {
            for (const auto &entry : g_dtx_ps2rna_by_handle)
            {
                const DtxPs2RnaState &rna = entry.second;
                if (!rna.playEnabled)
                {
                    continue;
                }

                auto consumeHandle = [&](uint32_t sjHandle)
                {
                    if (sjHandle == 0u)
                    {
                        return;
                    }

                    auto it = g_dtx_sjrmt_by_handle.find(sjHandle);
                    if (it == g_dtx_sjrmt_by_handle.end())
                    {
                        return;
                    }

                    dtxConsumeSjrmtDataLocked(it->second);
                };

                consumeHandle(rna.sjHandle0);
                consumeHandle(rna.sjHandle1);
            }
        }

        void dtxApplySjxPayload(uint8_t *rdram, const DtxTransferState &transfer)
        {
            if (!rdram || transfer.eeWorkAddr == 0u || transfer.wkSize < 64u)
            {
                return;
            }

            uint32_t commandCount = 0u;
            if (!readGuestU32(rdram, transfer.eeWorkAddr, commandCount))
            {
                return;
            }

            commandCount = std::min(commandCount, 128u);
            if (commandCount == 0u)
            {
                return;
            }

            constexpr uint32_t kSjxHeaderSize = 16u;
            constexpr uint32_t kSjxCommandSize = 16u;

            std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
            for (uint32_t i = 0; i < commandCount; ++i)
            {
                const uint32_t cmdAddr = transfer.eeWorkAddr + kSjxHeaderSize + (i * kSjxCommandSize);
                uint8_t *cmdPtr = getMemPtr(rdram, cmdAddr);
                if (!cmdPtr)
                {
                    break;
                }

                const uint8_t cmdNo = cmdPtr[0];
                const uint8_t cmdLine = cmdPtr[1];
                uint16_t cmdXid = 0u;
                uint32_t sjxHandle = 0u;
                uint32_t chunkDataAddr = 0u;
                uint32_t chunkLen = 0u;
                std::memcpy(&cmdXid, cmdPtr + 2u, sizeof(cmdXid));
                std::memcpy(&sjxHandle, cmdPtr + 4u, sizeof(sjxHandle));
                std::memcpy(&chunkDataAddr, cmdPtr + 8u, sizeof(chunkDataAddr));
                std::memcpy(&chunkLen, cmdPtr + 12u, sizeof(chunkLen));

                if (cmdNo != 0u || chunkLen == 0u)
                {
                    continue;
                }

                auto sjxIt = g_dtx_sjx_by_handle.find(sjxHandle);
                if (sjxIt == g_dtx_sjx_by_handle.end())
                {
                    continue;
                }

                DtxSjxState &sjx = sjxIt->second;
                if (sjx.xid != 0u && sjx.xid != cmdXid)
                {
                    continue;
                }

                auto sjrmtIt = g_dtx_sjrmt_by_handle.find(sjx.dstSjHandle);
                if (sjrmtIt == g_dtx_sjrmt_by_handle.end())
                {
                    continue;
                }

                if (cmdLine == sjx.line)
                {
                    dtxAppendToSjrmtData(rdram, sjrmtIt->second, chunkDataAddr, chunkLen);

                    // The IOP side consumes data from the remote SJ stream and returns the
                    // chunk to the EE-side room queue. Rewriting the ack packet to line 0
                    // makes the EE sjx_rcvcbf recycle the chunk instead of requeueing it
                    // as playable data forever.
                    cmdPtr[1] = 0u;
                }
            }

            dtxConsumeActivePs2RnaStreamsLocked();
        }

        void dtxApplyPs2RnaPayload(uint8_t *rdram, const DtxTransferState &transfer)
        {
            if (!rdram || transfer.eeWorkAddr == 0u || transfer.wkSize < 64u)
            {
                return;
            }

            uint32_t commandCount = 0u;
            if (!readGuestU32(rdram, transfer.eeWorkAddr, commandCount))
            {
                return;
            }

            commandCount = std::min(commandCount, 128u);
            if (commandCount == 0u)
            {
                return;
            }

            constexpr uint32_t kPs2RnaHeaderSize = 16u;
            constexpr uint32_t kPs2RnaCommandSize = 16u;

            std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
            for (uint32_t i = 0; i < commandCount; ++i)
            {
                const uint32_t cmdAddr = transfer.eeWorkAddr + kPs2RnaHeaderSize + (i * kPs2RnaCommandSize);
                const uint8_t *cmdPtr = getConstMemPtr(rdram, cmdAddr);
                if (!cmdPtr)
                {
                    break;
                }

                uint16_t cmdNo = 0u;
                uint32_t rnaHandle = 0u;
                uint32_t arg1 = 0u;
                uint32_t arg2 = 0u;
                std::memcpy(&cmdNo, cmdPtr + 0u, sizeof(cmdNo));
                std::memcpy(&rnaHandle, cmdPtr + 4u, sizeof(rnaHandle));
                std::memcpy(&arg1, cmdPtr + 8u, sizeof(arg1));
                std::memcpy(&arg2, cmdPtr + 12u, sizeof(arg2));

                auto it = g_dtx_ps2rna_by_handle.find(rnaHandle);
                if (it == g_dtx_ps2rna_by_handle.end())
                {
                    continue;
                }

                DtxPs2RnaState &state = it->second;
                switch (cmdNo)
                {
                case 0u: // IOPRNA_CMD_START
                    state.playEnabled = true;
                    break;
                case 1u: // IOPRNA_CMD_STOP
                    state.playEnabled = false;
                    break;
                case 2u: // IOPRNA_CMD_SETPSW
                    state.playEnabled = (arg1 != 0u);
                    break;
                case 3u: // IOPRNA_CMD_SETNCH
                    state.channelCount = arg1;
                    break;
                case 4u: // IOPRNA_CMD_SETSFREQ
                    state.sampleFreq = arg1;
                    break;
                case 5u: // IOPRNA_CMD_SETVOL
                    state.volume = arg2;
                    break;
                default:
                    break;
                }
            }

            dtxConsumeActivePs2RnaStreamsLocked();
        }

    } // namespace

    void noteDtxSifDmaTransfer(uint8_t *rdram, uint32_t srcAddr, uint32_t dstAddr, uint32_t sizeBytes)
    {
        if (!rdram || sizeBytes < 64u)
        {
            return;
        }

        uint32_t normalizedSrc = 0u;
        uint32_t normalizedDst = 0u;
        if (!normalizeGuestLinearAddr(srcAddr, normalizedSrc) ||
            !normalizeGuestLinearAddr(dstAddr, normalizedDst))
        {
            return;
        }

        DtxTransferState matched{};
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
            if (!g_dtxCompatLayout.isConfigured())
            {
                return;
            }
            for (const auto &entry : g_dtx_transfer_by_id)
            {
                if (dtxMatchesTransfer(entry.second, normalizedSrc, normalizedDst, sizeBytes))
                {
                    matched = entry.second;
                    found = true;
                    break;
                }
            }
        }

        if (!found)
        {
            static uint32_t dtxMissLogCount = 0u;
            if (dtxMissLogCount < 64u)
            {
                uint32_t knownTransfers = 0u;
                uint32_t sampleDtxId = 0u;
                uint32_t sampleSrc = 0u;
                uint32_t sampleSize = 0u;
                {
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    knownTransfers = static_cast<uint32_t>(g_dtx_transfer_by_id.size());
                    if (!g_dtx_transfer_by_id.empty())
                    {
                        const auto &sample = *g_dtx_transfer_by_id.begin();
                        sampleDtxId = sample.second.dtxId;
                        sampleSrc = sample.second.eeWorkAddr;
                        sampleSize = sample.second.wkSize;
                    }
                }

                if (knownTransfers != 0u)
                {
                    RUNTIME_LOG("[sceSifSetDma:DTX_MISS] src=0x" << std::hex << normalizedSrc
                                                                 << " dst=0x" << normalizedDst
                                                                 << " size=0x" << sizeBytes
                                                                 << " known=" << std::dec << knownTransfers
                                                                 << " sampleDtxId=0x" << std::hex << sampleDtxId
                                                                 << " sampleSrc=0x" << sampleSrc
                                                                 << " sampleSize=0x" << sampleSize
                                                                 << std::dec << std::endl);
                    ++dtxMissLogCount;
                }
            }
            return;
        }

        const uint32_t footerTicketAddr = matched.eeWorkAddr + matched.wkSize - sizeof(uint32_t);
        uint32_t ticketNo = 0u;
        if (!readGuestU32(rdram, footerTicketAddr, ticketNo))
        {
            return;
        }

        (void)writeGuestU32(rdram, footerTicketAddr, ticketNo + 1u);

        if (matched.dtxId == 0u)
        {
            dtxApplySjxPayload(rdram, matched);
        }
        else if (matched.dtxId == 1u)
        {
            dtxApplyPs2RnaPayload(rdram, matched);
        }

        static uint32_t dtxAckLogCount = 0u;
        if (dtxAckLogCount < 32u)
        {
            RUNTIME_LOG("[sceSifSetDma:DTX_ACK] dtxId=0x" << std::hex << matched.dtxId
                                                          << " ee=0x" << matched.eeWorkAddr
                                                          << " iop=0x" << matched.iopWorkAddr
                                                          << " size=0x" << matched.wkSize
                                                          << " ticket=0x" << ticketNo
                                                          << "->0x" << (ticketNo + 1u));
            if (matched.dtxId == 0u)
            {
                uint32_t totalData = 0u;
                std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                for (const auto &entry : g_dtx_sjrmt_by_handle)
                {
                    totalData += entry.second.dataBytes;
                }
                RUNTIME_LOG(" sjrmtData=0x" << totalData);
            }
            RUNTIME_LOG(std::dec << std::endl);
            ++dtxAckLogCount;
        }
    }

    void resetSoundDriverRpcState()
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        resetSoundDriverRpcStateUnlocked();
    }

    void setSoundDriverCompatLayout(const PS2SoundDriverCompatLayout &layout)
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        g_soundDriverCompatLayout = layout;
    }

    void clearSoundDriverCompatLayout()
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        g_soundDriverCompatLayout = {};
    }

    void setDtxCompatLayout(const PS2DtxCompatLayout &layout)
    {
        std::scoped_lock lock(g_rpc_mutex, g_dtx_rpc_mutex);
        g_dtxCompatLayout = layout;
        resetDtxRpcStateUnlocked();
    }

    void clearDtxCompatLayout()
    {
        std::scoped_lock lock(g_rpc_mutex, g_dtx_rpc_mutex);
        g_dtxCompatLayout = {};
        resetDtxRpcStateUnlocked();
    }

    void prepareSoundDriverStatusTransfer(uint8_t *rdram, uint32_t srcAddr, uint32_t size)
    {
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        if (srcAddr != g_soundDriverRpcState.statusAddr || size != kSoundDriverStatusSize)
        {
            return;
        }

        backfillSoundDriverStatusFromCompatUnlocked(rdram);
    }

    void finalizeSoundDriverStatusTransfer(uint8_t *rdram, uint32_t srcAddr, uint32_t dstAddr, uint32_t size)
    {
        (void)rdram;
        (void)srcAddr;
        (void)dstAddr;
        (void)size;
    }

    void SifStopModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t moduleId = static_cast<int32_t>(getRegU32(ctx, 4)); // $a0
        const uint32_t resultAddr = getRegU32(ctx, 7);                    // $a3 (int* result, optional)

        uint32_t refsLeft = 0;
        const bool knownModule = trackSifModuleStop(moduleId, &refsLeft);
        const int32_t ret = knownModule ? 0 : -1;

        if (resultAddr != 0)
        {
            int32_t *hostResult = reinterpret_cast<int32_t *>(getMemPtr(rdram, resultAddr));
            if (hostResult)
            {
                *hostResult = knownModule ? 0 : -1;
            }
        }

        if (knownModule)
        {
            std::string modulePath;
            {
                std::lock_guard<std::mutex> lock(g_sif_module_mutex);
                auto it = g_sif_modules_by_id.find(moduleId);
                if (it != g_sif_modules_by_id.end())
                {
                    modulePath = it->second.path;
                }
            }
            logSifModuleAction("stop", moduleId, modulePath, refsLeft);
        }

        setReturnS32(ctx, ret);
    }

    void SifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pathAddr = getRegU32(ctx, 4); // $a0
        const std::string modulePath = readGuestCStringBounded(rdram, pathAddr, kMaxSifModulePathBytes);
        if (modulePath.empty())
        {
            setReturnS32(ctx, -1);
            return;
        }

        const int32_t moduleId = trackSifModuleLoad(modulePath);
        if (moduleId <= 0)
        {
            setReturnS32(ctx, -1);
            return;
        }

        uint32_t refs = 0;
