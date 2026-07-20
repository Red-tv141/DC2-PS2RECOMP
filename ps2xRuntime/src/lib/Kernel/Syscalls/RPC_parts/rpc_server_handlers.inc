        {
            std::lock_guard<std::mutex> lock(g_sif_module_mutex);
            auto it = g_sif_modules_by_id.find(moduleId);
            if (it != g_sif_modules_by_id.end())
            {
                refs = it->second.refCount;
            }
        }
        logSifModuleAction("load", moduleId, modulePath, refs);

        setReturnS32(ctx, moduleId);
    }

    void SifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::scoped_lock lock(g_rpc_mutex, g_dtx_rpc_mutex);
        if (runtime)
        {
            runtime->iop().init(rdram);
        }
        if (!g_rpc_initialized)
        {
            g_rpc_servers.clear();
            g_rpc_clients.clear();
            g_rpc_next_id = 1;
            g_rpc_packet_index = 0;
            g_rpc_server_index = 0;
            g_rpc_active_queue = 0;
            resetDtxRpcStateUnlocked();
            g_soundDriverRpcState.initialized = false;
            g_rpc_initialized = true;
            RUNTIME_LOG("[SifInitRpc] Initialized");
        }
        else
        {
            resetDtxRpcStateUnlocked();
            g_soundDriverRpcState.initialized = false;
        }
        setReturnS32(ctx, 0);
    }

    void SifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clientPtr = getRegU32(ctx, 4);
        uint32_t rpcId = getRegU32(ctx, 5);
        uint32_t mode = getRegU32(ctx, 6);

        t_SifRpcClientData *client = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, clientPtr));

        if (!client)
        {
            setReturnS32(ctx, -1);
            return;
        }

        client->command = 0;
        client->buf = 0;
        client->cbuf = 0;
        client->end_function = 0;
        client->end_param = 0;
        client->server = 0;
        client->hdr.pkt_addr = 0;
        client->hdr.sema_id = -1;
        client->hdr.mode = mode;

        uint32_t serverPtr = 0;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            client->hdr.rpc_id = g_rpc_next_id++;
            auto it = g_rpc_servers.find(rpcId);
            if (it != g_rpc_servers.end())
            {
                serverPtr = it->second.sd_ptr;
            }
            g_rpc_clients[clientPtr] = {};
            g_rpc_clients[clientPtr].sid = rpcId;
        }

        if (!serverPtr)
        {
            // Allocate a dummy server so bind loops can proceed.
            serverPtr = rpcAllocServerAddr(rdram);
            if (serverPtr)
            {
                t_SifRpcServerData *dummy = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr));
                if (dummy)
                {
                    std::memset(dummy, 0, sizeof(*dummy));
                    dummy->sid = static_cast<int>(rpcId);
                }
                std::lock_guard<std::mutex> lock(g_rpc_mutex);
                g_rpc_servers[rpcId] = {rpcId, serverPtr};
            }
        }

        if (serverPtr)
        {
            t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr));
            client->server = serverPtr;
            client->buf = sd ? sd->buf : 0;
            client->cbuf = sd ? sd->cbuf : 0;
        }
        else
        {
            client->server = 0;
            client->buf = 0;
            client->cbuf = 0;
        }

        setReturnS32(ctx, 0);
    }

    void SifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::recursive_mutex> rpcCallLock(g_sif_call_rpc_mutex);

        uint32_t clientPtr = getRegU32(ctx, 4);
        uint32_t rpcNum = getRegU32(ctx, 5);
        uint32_t mode = getRegU32(ctx, 6);
        uint32_t sendBuf = getRegU32(ctx, 7);
        uint32_t sendSize = 0;
        uint32_t recvBuf = 0;
        uint32_t recvSize = 0;
        uint32_t endFunc = 0;
        uint32_t endParam = 0;

        // Decode both extended-reg convention (EE default) and standard O32 stack convention,
        // picking REG whenever plausible, to avoid zero-collision on the stack.
        uint32_t sp = getRegU32(ctx, 29);

        uint32_t sendSizeReg = getRegU32(ctx, 8);
        uint32_t recvBufReg = getRegU32(ctx, 9);
        uint32_t recvSizeReg = getRegU32(ctx, 10);
        uint32_t endFuncReg = getRegU32(ctx, 11);
        uint32_t endParamReg = 0;
        (void)readStackU32(rdram, sp, 0x0, endParamReg);

        uint32_t sendSizeStk = 0;
        uint32_t recvBufStk = 0;
        uint32_t recvSizeStk = 0;
        uint32_t endFuncStk = 0;
        uint32_t endParamStk = 0;
        (void)readStackU32(rdram, sp, 0x10, sendSizeStk);
        (void)readStackU32(rdram, sp, 0x14, recvBufStk);
        (void)readStackU32(rdram, sp, 0x18, recvSizeStk);
        (void)readStackU32(rdram, sp, 0x1C, endFuncStk);
        (void)readStackU32(rdram, sp, 0x20, endParamStk);

        auto looksLikeGuestPtr = [&](uint32_t v) -> bool
        {
            if (v == 0)
                return true;
            const uint32_t norm = v & 0x1FFFFFFFu;
            return norm >= 0x10000u && norm < PS2_RAM_SIZE;
        };

        auto looksLikeSize = [&](uint32_t v) -> bool
        {
            return v <= 0x2000000u;
        };

        auto looksLikeFunc = [&](uint32_t v) -> bool
        {
            return v == 0 || looksLikeGuestPtr(v);
        };

        auto plausiblePack = [&](uint32_t sendSz, uint32_t rbuf, uint32_t rsz, uint32_t endFn) -> bool
        {
            return looksLikeSize(sendSz) && looksLikeGuestPtr(rbuf) && looksLikeSize(rsz) && looksLikeFunc(endFn);
        };

        const bool regPackPlausible = plausiblePack(sendSizeReg, recvBufReg, recvSizeReg, endFuncReg);
        const bool stackPackPlausible = plausiblePack(sendSizeStk, recvBufStk, recvSizeStk, endFuncStk);

        uint32_t boundSidHint = 0u;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            auto it = g_rpc_clients.find(clientPtr);
            if (it != g_rpc_clients.end())
            {
                boundSidHint = it->second.sid;
            }
        }
        PS2DtxCompatLayout dtxCompat{};
        {
            std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
            dtxCompat = g_dtxCompatLayout;
        }

        auto looksLikeDtxCreatePack = [&](uint32_t sendSz, uint32_t rbuf, uint32_t rsz) -> bool
        {
            return rbuf != 0u && rsz >= 4u && rsz <= 0x40u &&
                   sendSz >= 12u && sendSz <= 0x1000u;
        };

        const bool isDtxCreate34Call = dtxCompat.isConfigured() &&
                                       (boundSidHint == dtxCompat.rpcSid) &&
                                       (rpcNum == 0x422u);
        const bool forceStackForDtxCreate34 =
            isDtxCreate34Call &&
            stackPackPlausible &&
            looksLikeDtxCreatePack(sendSizeStk, recvBufStk, recvSizeStk) &&
            !looksLikeDtxCreatePack(sendSizeReg, recvBufReg, recvSizeReg);

        bool useRegConvention = true;
        if (forceStackForDtxCreate34)
        {
            useRegConvention = false;
        }
        else if (!regPackPlausible && stackPackPlausible)
        {
            const bool regHasValidCallback = (endFuncReg != 0u) && looksLikeFunc(endFuncReg);
            const bool stackHasValidCallback = (endFuncStk != 0u) && looksLikeFunc(endFuncStk);
            if (!(regHasValidCallback && !stackHasValidCallback))
            {
                useRegConvention = false;
            }
        }

        sendSize = useRegConvention ? sendSizeReg : sendSizeStk;
        recvBuf = useRegConvention ? recvBufReg : recvBufStk;
        recvSize = useRegConvention ? recvSizeReg : recvSizeStk;
        endFunc = useRegConvention ? endFuncReg : endFuncStk;
        endParam = useRegConvention ? endParamReg : endParamStk;

        const bool isDtxLikeRpc = dtxCompat.isConfigured() &&
                                  ((boundSidHint == dtxCompat.rpcSid) || ((rpcNum & 0xFF00u) == 0x0400u));
        static uint32_t dtxAbiLogCount = 0u;
        if (isDtxLikeRpc && dtxAbiLogCount < 96u)
        {
            RUNTIME_LOG("[SifCallRpc:ABI] client=0x" << std::hex << clientPtr
                                                     << " rpc=0x" << rpcNum
                                                     << " sidHint=0x" << boundSidHint
                                                     << " useReg=" << (useRegConvention ? 1 : 0)
                                                     << " reg=(" << sendSizeReg << "," << recvBufReg << "," << recvSizeReg << "," << endFuncReg << "," << endParamReg << ")"
                                                     << " stk=(" << sendSizeStk << "," << recvBufStk << "," << recvSizeStk << "," << endFuncStk << "," << endParamStk << ")"
                                                     << " plausible=(" << (regPackPlausible ? 1 : 0) << "," << (stackPackPlausible ? 1 : 0) << ")"
                                                     << " force34=" << (forceStackForDtxCreate34 ? 1 : 0)
                                                     << std::dec << std::endl);
            ++dtxAbiLogCount;
        }

        t_SifRpcClientData *client = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, clientPtr));

        if (!client)
        {
            setReturnS32(ctx, -1);
            return;
        }

        client->command = rpcNum;
        client->end_function = endFunc;
        client->end_param = endParam;
        client->hdr.mode = mode;

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            g_rpc_clients[clientPtr].busy = true;
            g_rpc_clients[clientPtr].last_rpc = rpcNum;
            uint32_t sid = g_rpc_clients[clientPtr].sid;
            if (sid)
            {
                auto it = g_rpc_servers.find(sid);
                if (it != g_rpc_servers.end())
                {
                    uint32_t mappedServer = it->second.sd_ptr;
                    if (mappedServer && client->server != mappedServer)
                    {
                        client->server = mappedServer;
                    }
                }
            }
        }

        uint32_t sid = 0;
        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            auto it = g_rpc_clients.find(clientPtr);
            if (it != g_rpc_clients.end())
            {
                sid = it->second.sid;
            }
        }

        uint32_t serverPtr = client->server;
        t_SifRpcServerData *sd = serverPtr ? reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverPtr)) : nullptr;

        if (sd)
        {
            sd->client = clientPtr;
            sd->pkt_addr = client->hdr.pkt_addr;
            sd->rpc_number = rpcNum;
            sd->size = static_cast<int>(sendSize);
            sd->recvbuf = recvBuf;
            sd->rsize = static_cast<int>(recvSize);
            sd->rmode = ((mode & kSifRpcModeNowait) && endFunc == 0) ? 0 : 1;
            sd->rid = 0;
        }

        if (sd && sd->buf && sendBuf && sendSize > 0)
        {
            rpcCopyToRdram(rdram, sd->buf, sendBuf, sendSize);
        }

        uint32_t resultPtr = 0;
        bool handled = false;

        auto readRpcU32 = [&](uint32_t addr, uint32_t &out) -> bool
        {
            if (!addr)
            {
                return false;
            }
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(&out, ptr, sizeof(out));
            return true;
        };

        auto writeRpcU32 = [&](uint32_t addr, uint32_t value) -> bool
        {
            if (!addr)
            {
                return false;
            }
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(ptr, &value, sizeof(value));
            return true;
        };

        if (!handled && runtime)
        {
            runtime->iop().init(rdram);
            uint32_t iopResultPtr = 0u;
            bool iopSignalNowaitCompletion = false;
            if (runtime->iop().handleRPC(runtime,
                                         sid, rpcNum,
                                         sendBuf, sendSize,
                                         recvBuf, recvSize,
                                         iopResultPtr,
                                         iopSignalNowaitCompletion))
            {
                handled = true;
                resultPtr = iopResultPtr;
                if (iopSignalNowaitCompletion && (mode & kSifRpcModeNowait) != 0u)
                {
                    uint32_t semaId = static_cast<uint32_t>(client->hdr.sema_id);
                    if (semaId == 0xFFFFFFFFu || semaId == 0u)
                    {
                        semaId = endParam;
                    }
                    (void)signalRpcCompletionSema(semaId);
                }
            }
        }

        const bool isDtxUrpc = dtxCompat.isUrpcRpc(sid, rpcNum);
        uint32_t dtxUrpcCommand = isDtxUrpc ? (rpcNum & 0xFFu) : 0u;
        uint32_t dtxUrpcFn = 0;
        uint32_t dtxUrpcObj = 0;
        uint32_t dtxUrpcSend0 = 0;
        bool dtxUrpcDispatchAttempted = false;
        bool dtxUrpcFallbackEmulated = false;
        bool dtxUrpcFallbackCreate34 = false;
        bool hasUrpcHandler = false;
        if (isDtxUrpc)
        {
            if (sendBuf && sendSize >= sizeof(uint32_t))
            {
                (void)readRpcU32(sendBuf, dtxUrpcSend0);
            }
            if (dtxCompat.hasUrpcTables() && dtxUrpcCommand < 64u)
            {
                (void)readRpcU32(dtxCompat.urpcFnTableBase + (dtxUrpcCommand * 4u), dtxUrpcFn);
                (void)readRpcU32(dtxCompat.urpcObjTableBase + (dtxUrpcCommand * 4u), dtxUrpcObj);
            }
            hasUrpcHandler = (dtxUrpcCommand < 64u) && (dtxUrpcFn != 0u);
        }
        const bool allowServerDispatch = !isDtxUrpc || hasUrpcHandler;
        const bool isDtxSid = dtxCompat.isConfigured() && sid == dtxCompat.rpcSid;

        if (sd && sd->func && (!isDtxSid || isDtxUrpc) && allowServerDispatch)
        {
            dtxUrpcDispatchAttempted = dtxUrpcDispatchAttempted || isDtxUrpc;
            handled = rpcInvokeFunction(rdram, ctx, runtime, sd->func, rpcNum, sd->buf, sendSize, 0, &resultPtr);
            if (handled && resultPtr == 0 && sd->buf)
            {
                resultPtr = sd->buf;
            }
            if (handled && resultPtr == 0 && recvBuf)
            {
                resultPtr = recvBuf;
            }
        }

        if (!handled && isDtxUrpc && sendBuf && sendSize > 0)
        {
            // Only dispatch through dtx_rpc_func when a URPC handler is registered in the table.
            // If the slot is empty, defer to the fallback emulation below.
            if (hasUrpcHandler && dtxCompat.dispatcherFuncAddr != 0u)
            {
                dtxUrpcDispatchAttempted = true;
                handled = rpcInvokeFunction(rdram, ctx, runtime, dtxCompat.dispatcherFuncAddr, rpcNum, sendBuf, sendSize, 0, &resultPtr);
                if (handled && resultPtr == 0)
                {
                    resultPtr = sendBuf;
                }
            }
        }

        if (!handled && isDtxSid)
        {
            if (rpcNum == 2 && recvBuf && recvSize >= sizeof(uint32_t))
            {
                uint32_t dtxId = 0;
                uint32_t eeWorkAddr = 0u;
                uint32_t iopWorkAddr = 0u;
                uint32_t wkSize = 0u;
                if (sendBuf && sendSize >= sizeof(uint32_t))
                {
                    (void)readRpcU32(sendBuf + 0u, dtxId);
                }
                if (sendBuf && sendSize >= (4u * sizeof(uint32_t)))
                {
                    (void)readRpcU32(sendBuf + 4u, eeWorkAddr);
                    (void)readRpcU32(sendBuf + 8u, iopWorkAddr);
                    (void)readRpcU32(sendBuf + 12u, wkSize);
                }

                uint32_t normalizedEeWorkAddr = 0u;
                uint32_t normalizedIopWorkAddr = 0u;
                (void)normalizeGuestLinearAddr(eeWorkAddr, normalizedEeWorkAddr);
                (void)normalizeGuestLinearAddr(iopWorkAddr, normalizedIopWorkAddr);

                uint32_t remoteHandle = 0;
                {
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_remote_by_id.find(dtxId);
                    if (it != g_dtx_remote_by_id.end())
                    {
                        remoteHandle = it->second;
                    }
                    if (!remoteHandle)
                    {
                        remoteHandle = rpcAllocServerAddr(rdram);
                        if (!remoteHandle)
                        {
                            remoteHandle = rpcAllocPacketAddr(rdram);
                        }
                        if (!remoteHandle)
                        {
                            remoteHandle = kRpcServerPoolBase + ((dtxId & 0xFFu) * kRpcServerStride);
                        }
                        g_dtx_remote_by_id[dtxId] = remoteHandle;
                    }

                    DtxTransferState state{};
                    state.dtxId = dtxId;
                    state.remoteHandle = remoteHandle;
                    state.eeWorkAddr = normalizedEeWorkAddr;
                    state.iopWorkAddr = normalizedIopWorkAddr;
                    state.wkSize = wkSize;
                    g_dtx_transfer_by_id[dtxId] = state;
                }

                (void)writeRpcU32(recvBuf, remoteHandle);
                if (recvSize > sizeof(uint32_t))
                {
                    rpcZeroRdram(rdram, recvBuf + sizeof(uint32_t), recvSize - sizeof(uint32_t));
                }
                static uint32_t dtxCreateLogCount = 0;
                if (dtxCreateLogCount < 64u)
                {
                    RUNTIME_LOG("[SifCallRpc:DTX_CREATE] dtxId=0x" << std::hex << dtxId
                                                                   << " remote=0x" << remoteHandle
                                                                   << " recvBuf=0x" << recvBuf
                                                                   << " recvSize=0x" << recvSize
                                                                   << std::dec << std::endl);
                    ++dtxCreateLogCount;
                }
                handled = true;
                resultPtr = recvBuf;
            }
            else if (rpcNum == 3)
            {
                uint32_t remoteHandle = 0;
                if (sendBuf && sendSize >= sizeof(uint32_t) && readRpcU32(sendBuf, remoteHandle) && remoteHandle)
                {
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    for (auto it = g_dtx_remote_by_id.begin(); it != g_dtx_remote_by_id.end(); ++it)
                    {
                        if (it->second == remoteHandle)
                        {
                            g_dtx_transfer_by_id.erase(it->first);
                            g_dtx_remote_by_id.erase(it);
                            break;
                        }
                    }
                }
                if (recvBuf && recvSize > 0)
                {
                    rpcZeroRdram(rdram, recvBuf, recvSize);
                }
                handled = true;
                resultPtr = recvBuf;
            }
            else if (rpcNum >= 0x400 && rpcNum < 0x500)
            {
                dtxUrpcFallbackEmulated = true;
                const uint32_t urpcCommand = rpcNum & 0xFFu;
                uint32_t outWords[4] = {1u, 0u, 0u, 0u};
                uint32_t outWordCount = 1u;

                auto readSendWord = [&](uint32_t index, uint32_t &out) -> bool
                {
                    const uint64_t byteOffset = static_cast<uint64_t>(index) * sizeof(uint32_t);
                    if (!sendBuf || sendSize < (byteOffset + sizeof(uint32_t)))
                    {
                        return false;
                    }
                    return readRpcU32(sendBuf + static_cast<uint32_t>(byteOffset), out);
                };

                switch (urpcCommand)
                {
                case 0u: // SJX_CREATE
                {
                    uint32_t srcSjHandle = 0u;
                    uint32_t dstSjHandle = 0u;
                    uint32_t line = 0u;
                    uint32_t eeObjAddr = 0u;
                    (void)readSendWord(0u, srcSjHandle);
                    (void)readSendWord(1u, dstSjHandle);
                    (void)readSendWord(2u, line);
                    (void)readSendWord(3u, eeObjAddr);

                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    const uint32_t handle = dtxAllocUrpcHandleLocked();
                    DtxSjxState state{};
                    state.handle = handle;
                    state.srcSjHandle = srcSjHandle;
                    state.dstSjHandle = dstSjHandle;
                    state.line = line;
                    state.eeObjAddr = eeObjAddr;
                    g_dtx_sjx_by_handle[handle] = state;
                    outWords[0] = handle ? handle : 1u;
                    outWordCount = 1u;
                    break;
                }
                case 1u: // SJX_DESTROY
                {
                    uint32_t handle = 0u;
                    (void)readSendWord(0u, handle);
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    g_dtx_sjx_by_handle.erase(handle);
                    outWords[0] = 1u;
                    outWordCount = 1u;
                    break;
                }
                case 2u: // SJX_RESET
                {
                    uint32_t handle = 0u;
                    uint32_t xid = 0u;
                    (void)readSendWord(0u, handle);
                    (void)readSendWord(1u, xid);
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_sjx_by_handle.find(handle);
                    if (it != g_dtx_sjx_by_handle.end())
                    {
                        it->second.xid = static_cast<uint16_t>(xid & 0xFFFFu);
                    }
                    outWords[0] = 1u;
                    outWordCount = 1u;
                    break;
                }
                case 8u: // PS2RNA_CREATE
                {
                    uint32_t maxChannels = 0u;
                    uint32_t sjHandle0 = 0u;
                    uint32_t sjHandle1 = 0u;
                    (void)readSendWord(0u, maxChannels);
                    (void)readSendWord(2u, sjHandle0);
                    (void)readSendWord(3u, sjHandle1);

                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    const uint32_t handle = dtxAllocUrpcHandleLocked();
                    DtxPs2RnaState state{};
                    state.handle = handle;
                    state.maxChannels = maxChannels;
                    state.sjHandle0 = sjHandle0;
                    state.sjHandle1 = sjHandle1;
                    state.channelCount = maxChannels;
                    g_dtx_ps2rna_by_handle[handle] = state;
                    outWords[0] = handle ? handle : 1u;
                    outWordCount = 1u;
                    break;
                }
                case 9u: // PS2RNA_DESTROY
                {
                    uint32_t handle = 0u;
                    (void)readSendWord(0u, handle);
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    g_dtx_ps2rna_by_handle.erase(handle);
                    outWords[0] = 1u;
                    outWordCount = 1u;
                    break;
                }
                case 32u: // SJRMT_RBF_CREATE
                case 33u: // SJRMT_MEM_CREATE
                case 34u: // SJRMT_UNI_CREATE
                {
                    uint32_t arg0 = 0;
                    uint32_t arg1 = 0;
                    uint32_t arg2 = 0;
                    (void)readSendWord(0u, arg0);
                    (void)readSendWord(1u, arg1);
                    (void)readSendWord(2u, arg2);

                    uint32_t mode = 0;
                    uint32_t wkAddr = 0;
                    uint32_t wkSize = 0;
                    if (urpcCommand == 34u)
                    {
                        mode = arg0;
                        wkAddr = arg1;
                        wkSize = arg2;
                        dtxUrpcFallbackCreate34 = true;
                    }
                    else if (urpcCommand == 33u)
                    {
                        wkAddr = arg0;
                        wkSize = arg1;
                    }
                    else
                    {
                        wkAddr = arg0;
                        wkSize = (arg1 != 0u) ? arg1 : arg2;
                    }

                    wkSize = dtxNormalizeSjrmtCapacity(wkSize);

                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    const uint32_t handle = dtxAllocUrpcHandleLocked();
                    DtxSjrmtState state{};
                    state.handle = handle;
                    state.mode = mode;
                    state.wkAddr = wkAddr;
                    state.wkSize = wkSize;
                    state.readPos = 0u;
                    state.writePos = 0u;
                    state.roomBytes = wkSize;
                    state.dataBytes = 0u;
                    state.uuid0 = 0x53524D54u; // "SRMT"
                    state.uuid1 = handle;
                    state.uuid2 = wkAddr;
                    state.uuid3 = wkSize;
                    g_dtx_sjrmt_by_handle[handle] = state;

                    outWords[0] = handle ? handle : 1u;
                    outWordCount = 1u;
                    break;
                }
                case 35u: // SJRMT_DESTROY
                {
                    uint32_t handle = 0;
                    (void)readSendWord(0u, handle);
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    g_dtx_sjrmt_by_handle.erase(handle);
                    outWords[0] = 1u;
                    outWordCount = 1u;
                    break;
                }
                case 36u: // SJRMT_GET_UUID
                {
                    uint32_t handle = 0;
                    (void)readSendWord(0u, handle);
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_sjrmt_by_handle.find(handle);
                    if (it != g_dtx_sjrmt_by_handle.end())
                    {
                        outWords[0] = it->second.uuid0;
                        outWords[1] = it->second.uuid1;
                        outWords[2] = it->second.uuid2;
                        outWords[3] = it->second.uuid3;
                    }
                    else
                    {
                        outWords[0] = 0u;
                        outWords[1] = 0u;
                        outWords[2] = 0u;
                        outWords[3] = 0u;
                    }
                    outWordCount = 4u;
                    break;
                }
                case 37u: // SJRMT_RESET
                {
                    uint32_t handle = 0;
                    (void)readSendWord(0u, handle);
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_sjrmt_by_handle.find(handle);
                    if (it != g_dtx_sjrmt_by_handle.end())
                    {
                        const uint32_t cap = (it->second.wkSize == 0u) ? 0x4000u : it->second.wkSize;
                        it->second.readPos = 0u;
                        it->second.writePos = 0u;
                        it->second.roomBytes = cap;
                        it->second.dataBytes = 0u;
                    }
                    outWords[0] = 1u;
                    outWordCount = 1u;
                    break;
                }
                case 38u: // SJRMT_GET_CHUNK
                {
                    uint32_t handle = 0;
                    uint32_t streamId = 0;
                    uint32_t nbyte = 0;
                    (void)readSendWord(0u, handle);
                    (void)readSendWord(1u, streamId);
                    (void)readSendWord(2u, nbyte);

                    uint32_t ptr = 0u;
                    uint32_t len = 0u;

                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_sjrmt_by_handle.find(handle);
                    if (it != g_dtx_sjrmt_by_handle.end())
                    {
                        DtxSjrmtState &state = it->second;
                        const uint32_t cap = (state.wkSize == 0u) ? 0x4000u : state.wkSize;

                        if (streamId == 0u)
                        {
                            len = std::min(nbyte, state.roomBytes);
                            ptr = state.wkAddr + (cap ? (state.writePos % cap) : 0u);
                            if (cap != 0u)
                            {
                                state.writePos = (state.writePos + len) % cap;
                            }
                            state.roomBytes -= len;
                        }
                        else if (streamId == 1u)
