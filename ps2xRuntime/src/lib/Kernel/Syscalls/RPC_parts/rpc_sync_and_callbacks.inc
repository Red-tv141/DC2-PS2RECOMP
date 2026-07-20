                        {
                            len = std::min(nbyte, state.dataBytes);
                            ptr = state.wkAddr + (cap ? (state.readPos % cap) : 0u);
                            if (cap != 0u)
                            {
                                state.readPos = (state.readPos + len) % cap;
                            }
                            state.dataBytes -= len;
                        }
                    }

                    outWords[0] = ptr;
                    outWords[1] = len;
                    outWordCount = 2u;
                    break;
                }
                case 39u: // SJRMT_UNGET_CHUNK
                {
                    uint32_t handle = 0;
                    uint32_t streamId = 0;
                    uint32_t len = 0;
                    (void)readSendWord(0u, handle);
                    (void)readSendWord(1u, streamId);
                    (void)readSendWord(3u, len);

                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_sjrmt_by_handle.find(handle);
                    if (it != g_dtx_sjrmt_by_handle.end())
                    {
                        DtxSjrmtState &state = it->second;
                        const uint32_t cap = (state.wkSize == 0u) ? 0x4000u : state.wkSize;
                        if (streamId == 0u)
                        {
                            const uint32_t delta = (cap == 0u) ? 0u : (len % cap);
                            if (cap != 0u)
                            {
                                state.writePos = (state.writePos + cap - delta) % cap;
                            }
                            state.roomBytes = std::min(cap, state.roomBytes + len);
                        }
                        else if (streamId == 1u)
                        {
                            const uint32_t delta = (cap == 0u) ? 0u : (len % cap);
                            if (cap != 0u)
                            {
                                state.readPos = (state.readPos + cap - delta) % cap;
                            }
                            state.dataBytes = std::min(cap, state.dataBytes + len);
                        }
                    }

                    outWords[0] = 1u;
                    outWordCount = 1u;
                    break;
                }
                case 40u: // SJRMT_PUT_CHUNK
                {
                    uint32_t handle = 0;
                    uint32_t streamId = 0;
                    uint32_t len = 0;
                    (void)readSendWord(0u, handle);
                    (void)readSendWord(1u, streamId);
                    (void)readSendWord(3u, len);

                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_sjrmt_by_handle.find(handle);
                    if (it != g_dtx_sjrmt_by_handle.end())
                    {
                        DtxSjrmtState &state = it->second;
                        const uint32_t cap = (state.wkSize == 0u) ? 0x4000u : state.wkSize;
                        if (streamId == 0u)
                        {
                            state.roomBytes = std::min(cap, state.roomBytes + len);
                        }
                        else if (streamId == 1u)
                        {
                            state.dataBytes = std::min(cap, state.dataBytes + len);
                        }
                    }

                    outWords[0] = 1u;
                    outWordCount = 1u;
                    break;
                }
                case 41u: // SJRMT_GET_NUM_DATA
                {
                    uint32_t handle = 0;
                    uint32_t streamId = 0;
                    (void)readSendWord(0u, handle);
                    (void)readSendWord(1u, streamId);

                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_sjrmt_by_handle.find(handle);
                    if (it != g_dtx_sjrmt_by_handle.end())
                    {
                        outWords[0] = (streamId == 0u) ? it->second.roomBytes : it->second.dataBytes;
                    }
                    else
                    {
                        outWords[0] = 0u;
                    }
                    outWordCount = 1u;
                    break;
                }
                case 42u: // SJRMT_IS_GET_CHUNK
                {
                    uint32_t handle = 0;
                    uint32_t streamId = 0;
                    uint32_t nbyte = 0;
                    (void)readSendWord(0u, handle);
                    (void)readSendWord(1u, streamId);
                    (void)readSendWord(2u, nbyte);

                    uint32_t available = 0u;
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_sjrmt_by_handle.find(handle);
                    if (it != g_dtx_sjrmt_by_handle.end())
                    {
                        available = (streamId == 0u) ? it->second.roomBytes : it->second.dataBytes;
                    }
                    outWords[0] = (available >= nbyte) ? 1u : 0u;
                    outWords[1] = available;
                    outWordCount = 2u;
                    break;
                }
                case 43u: // SJRMT_INIT
                case 44u: // SJRMT_FINISH
                {
                    outWords[0] = 1u;
                    outWordCount = 1u;
                    break;
                }
                default:
                {
                    uint32_t urpcRet = 1u;
                    if (sendBuf && sendSize >= sizeof(uint32_t))
                    {
                        (void)readRpcU32(sendBuf, urpcRet);
                    }
                    if (urpcCommand == 0u)
                    {
                        std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                        urpcRet = dtxAllocUrpcHandleLocked();
                    }
                    if (urpcRet == 0u)
                    {
                        urpcRet = 1u;
                    }
                    outWords[0] = urpcRet;
                    outWordCount = 1u;
                    break;
                }
                }

                if (recvBuf && recvSize > 0u)
                {
                    const uint32_t recvWordCapacity = static_cast<uint32_t>(recvSize / sizeof(uint32_t));
                    const uint32_t wordsToWrite = std::min(outWordCount, recvWordCapacity);
                    for (uint32_t i = 0; i < wordsToWrite; ++i)
                    {
                        (void)writeRpcU32(recvBuf + (i * sizeof(uint32_t)), outWords[i]);
                    }

                    // SJRMT_IsGetChunk callers read rbuf[1] even when nout==1.
                    if (urpcCommand == 42u && outWordCount > 1u)
                    {
                        (void)writeRpcU32(recvBuf + sizeof(uint32_t), outWords[1]);
                    }

                    if (recvSize > (wordsToWrite * sizeof(uint32_t)))
                    {
                        rpcZeroRdram(rdram, recvBuf + (wordsToWrite * sizeof(uint32_t)),
                                     recvSize - (wordsToWrite * sizeof(uint32_t)));
                    }
                }

                handled = true;
                resultPtr = recvBuf;
            }
        }

        if (recvBuf && recvSize > 0)
        {
            if (handled && resultPtr && resultPtr != recvBuf)
            {
                rpcCopyToRdram(rdram, recvBuf, resultPtr, recvSize);
            }
            else if (!handled && sendBuf && sendSize > 0 && sendBuf != recvBuf)
            {
                size_t copySize = (sendSize < recvSize) ? sendSize : recvSize;
                rpcCopyToRdram(rdram, recvBuf, sendBuf, copySize);
            }
            else if (!handled)
            {
                rpcZeroRdram(rdram, recvBuf, recvSize);
            }
        }

        if (isDtxUrpc)
        {
            static int dtxUrpcLogCount = 0;
            if (dtxUrpcLogCount < 64)
            {
                uint32_t dtxUrpcRecv0 = 0;
                if (recvBuf && recvSize >= sizeof(uint32_t))
                {
                    (void)readRpcU32(recvBuf, dtxUrpcRecv0);
                }
                RUNTIME_LOG("[SifCallRpc:DTX] rpcNum=0x" << std::hex << rpcNum
                                                         << " cmd=0x" << dtxUrpcCommand
                                                         << " name=" << dtxUrpcCommandName(dtxUrpcCommand)
                                                         << " fn=0x" << dtxUrpcFn
                                                         << " obj=0x" << dtxUrpcObj
                                                         << " send0=0x" << dtxUrpcSend0
                                                         << " recv0=0x" << dtxUrpcRecv0
                                                         << " resultPtr=0x" << resultPtr
                                                         << " handled=" << std::dec << (handled ? 1 : 0)
                                                         << " dispatch=" << (dtxUrpcDispatchAttempted ? 1 : 0)
                                                         << " emu=" << (dtxUrpcFallbackEmulated ? 1 : 0)
                                                         << " emu34=" << (dtxUrpcFallbackCreate34 ? 1 : 0)
                                                         << std::endl);

                if (dtxUrpcCommand >= 37u && dtxUrpcCommand <= 42u)
                {
                    std::lock_guard<std::mutex> lock(g_dtx_rpc_mutex);
                    auto it = g_dtx_sjrmt_by_handle.find(dtxUrpcSend0);
                    if (it != g_dtx_sjrmt_by_handle.end())
                    {
                        const DtxSjrmtState &state = it->second;
                        RUNTIME_LOG("[SifCallRpc:DTX_SJRMT] handle=0x" << std::hex << dtxUrpcSend0
                                                                       << " cmd=" << dtxUrpcCommandName(dtxUrpcCommand)
                                                                       << " wk=0x" << state.wkAddr
                                                                       << " size=0x" << state.wkSize
                                                                       << " read=0x" << state.readPos
                                                                       << " write=0x" << state.writePos
                                                                       << std::dec
                                                                       << " room=" << state.roomBytes
                                                                       << " data=" << state.dataBytes);
                        if ((dtxUrpcCommand == 38u || dtxUrpcCommand == 42u) && sendBuf && sendSize >= 12u)
                        {
                            uint32_t streamId = 0u;
                            uint32_t nbyte = 0u;
                            (void)readRpcU32(sendBuf + 4u, streamId);
                            (void)readRpcU32(sendBuf + 8u, nbyte);
                            RUNTIME_LOG(" stream=" << dtxStreamName(streamId)
                                                   << " req=" << nbyte);
                        }
                        RUNTIME_LOG(std::endl);
                    }
                }
                ++dtxUrpcLogCount;
            }
        }

        if (endFunc)
        {
            bool callbackInvoked = rpcInvokeFunction(rdram, ctx, runtime, endFunc, endParam, 0, 0, 0, nullptr);

            if (!callbackInvoked && endFunc >= 0x10000u)
            {
                const uint32_t normalizedEndFunc = endFunc - 0x10000u;
                if (runtime->hasFunction(normalizedEndFunc))
                {
                    callbackInvoked = rpcInvokeFunction(rdram, ctx, runtime, normalizedEndFunc, endParam, 0, 0, 0, nullptr);
                }
            }

            PS2SoundDriverCompatLayout soundCompat{};
            {
                std::lock_guard<std::mutex> lock(g_rpc_mutex);
                soundCompat = g_soundDriverCompatLayout;
            }

            if (soundCompat.matchesCompletionCallback(endFunc))
            {
                uint32_t semaId = static_cast<uint32_t>(client->hdr.sema_id);
                if (semaId == 0xFFFFFFFFu || semaId == 0u)
                {
                    semaId = endParam;
                }
                (void)signalRpcCompletionSema(semaId);
                if (rdram && soundCompat.busyFlagAddr != 0u && soundCompat.matchesClearBusyCallback(endFunc))
                {
                    if (uint32_t *busy = reinterpret_cast<uint32_t *>(getMemPtr(rdram, soundCompat.busyFlagAddr)))
                    {
                        *busy = 0u;
                    }
                }
            }

            if (!callbackInvoked)
            {
                uint32_t semaId = static_cast<uint32_t>(client->hdr.sema_id);
                if (semaId == 0xFFFFFFFFu || semaId == 0u)
                {
                    semaId = endParam;
                }
                const bool fallbackSignaledSema = signalRpcCompletionSema(semaId);

                static uint32_t unresolvedEndFuncWarnCount = 0;
                if (unresolvedEndFuncWarnCount < 32u)
                {
                    std::cerr << "[SifCallRpc] unresolved end callback endFunc=0x" << std::hex << endFunc
                              << " semaId=0x" << semaId
                              << " fallbackSignal=" << std::dec << (fallbackSignaledSema ? 1 : 0)
                              << std::endl;
                    ++unresolvedEndFuncWarnCount;
                }
            }
        }

        static int logCount = 0;
        if (logCount < 10)
        {
            RUNTIME_LOG("[SifCallRpc] client=0x" << std::hex << clientPtr
                                                 << " sid=0x" << sid
                                                 << " rpcNum=0x" << rpcNum
                                                 << " mode=0x" << mode
                                                 << " sendBuf=0x" << sendBuf
                                                 << " recvBuf=0x" << recvBuf
                                                 << " recvSize=0x" << recvSize
                                                 << " size=" << std::dec << sendSize << std::endl);
            ++logCount;
        }

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            g_rpc_clients[clientPtr].busy = false;
        }

        setReturnS32(ctx, 0);
    }

    void SifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t sdPtr = getRegU32(ctx, 4);
        uint32_t sid = getRegU32(ctx, 5);
        uint32_t func = getRegU32(ctx, 6);
        uint32_t buf = getRegU32(ctx, 7);
        // stack args: cfunc, cbuf, qd...
        uint32_t sp = getRegU32(ctx, 29);
        uint32_t cfunc = 0;
        uint32_t cbuf = 0;
        uint32_t qd = 0;
        readStackU32(rdram, sp, 0x10, cfunc);
        readStackU32(rdram, sp, 0x14, cbuf);
        readStackU32(rdram, sp, 0x18, qd);

        t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
        if (!sd)
        {
            setReturnS32(ctx, -1);
            return;
        }

        sd->sid = static_cast<int>(sid);
        sd->func = func;
        sd->buf = buf;
        sd->size = 0;
        sd->cfunc = cfunc;
        sd->cbuf = cbuf;
        sd->size2 = 0;
        sd->client = 0;
        sd->pkt_addr = 0;
        sd->rpc_number = 0;
        sd->recvbuf = 0;
        sd->rsize = 0;
        sd->rmode = 0;
        sd->rid = 0;
        sd->base = qd;
        sd->link = 0;
        sd->next = 0;

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);

            if (qd)
            {
                t_SifRpcDataQueue *queue = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qd));
                if (queue)
                {
                    if (!queue->link)
                    {
                        queue->link = sdPtr;
                    }
                    else
                    {
                        uint32_t curPtr = queue->link;
                        for (int guard = 0; guard < 1024 && curPtr; ++guard)
                        {
                            t_SifRpcServerData *cur = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, curPtr));
                            if (!cur)
                                break;
                            if (!cur->link)
                            {
                                cur->link = sdPtr;
                                break;
                            }
                            if (cur->link == sdPtr)
                                break;
                            curPtr = cur->link;
                        }
                    }
                }
            }

            g_rpc_servers[sid] = {sid, sdPtr};
            for (auto &entry : g_rpc_clients)
            {
                if (entry.second.sid == sid)
                {
                    t_SifRpcClientData *cd = reinterpret_cast<t_SifRpcClientData *>(getMemPtr(rdram, entry.first));
                    if (cd)
                    {
                        cd->server = sdPtr;
                        cd->buf = sd->buf;
                        cd->cbuf = sd->cbuf;
                    }
                }
            }
        }

        RUNTIME_LOG("[SifRegisterRpc] sid=0x" << std::hex << sid << " sd=0x" << sdPtr << std::dec);
        setReturnS32(ctx, 0);
    }

    void SifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t clientPtr = getRegU32(ctx, 4);
        g_f60_checkStatRpc.fetch_add(1, std::memory_order_relaxed);
        g_f60_checkStatRpc_ra.store(getRegU32(ctx, 31), std::memory_order_relaxed);
        g_f60_checkStatRpc_client.store(clientPtr, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        auto it = g_rpc_clients.find(clientPtr);
        if (it == g_rpc_clients.end())
        {
            setReturnS32(ctx, 0);
            return;
        }
        setReturnS32(ctx, it->second.busy ? 1 : 0);
    }

    void SifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t qdPtr = getRegU32(ctx, 4);
        int threadId = static_cast<int>(getRegU32(ctx, 5));

        t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
        if (!qd)
        {
            setReturnS32(ctx, -1);
            return;
        }

        qd->thread_id = threadId;
        qd->active = 0;
        qd->link = 0;
        qd->start = 0;
        qd->end = 0;
        qd->next = 0;

        {
            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            if (!g_rpc_active_queue)
            {
                g_rpc_active_queue = qdPtr;
            }
            else
            {
                uint32_t curPtr = g_rpc_active_queue;
                for (int guard = 0; guard < 1024 && curPtr; ++guard)
                {
                    if (curPtr == qdPtr)
                        break;
                    t_SifRpcDataQueue *cur = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, curPtr));
                    if (!cur)
                        break;
                    if (!cur->next)
                    {
                        cur->next = qdPtr;
                        break;
                    }
                    curPtr = cur->next;
                }
            }
        }

        setReturnS32(ctx, 0);
    }

    void SifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t qdPtr = getRegU32(ctx, 4);
        if (!qdPtr)
        {
            setReturnU32(ctx, 0);
            return;
        }

        std::lock_guard<std::mutex> lock(g_rpc_mutex);
        if (!g_rpc_active_queue)
        {
            setReturnU32(ctx, 0);
            return;
        }

        if (g_rpc_active_queue == qdPtr)
        {
            t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
            g_rpc_active_queue = qd ? qd->next : 0;
            setReturnU32(ctx, qdPtr);
            return;
        }

        uint32_t curPtr = g_rpc_active_queue;
        for (int guard = 0; guard < 1024 && curPtr; ++guard)
        {
            t_SifRpcDataQueue *cur = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, curPtr));
            if (!cur)
                break;
            if (cur->next == qdPtr)
            {
                t_SifRpcDataQueue *rem = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
                cur->next = rem ? rem->next : 0;
                setReturnU32(ctx, qdPtr);
                return;
            }
            curPtr = cur->next;
        }

        setReturnU32(ctx, 0);
    }

    void SifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t sdPtr = getRegU32(ctx, 4);
        uint32_t qdPtr = getRegU32(ctx, 5);

        t_SifRpcDataQueue *qd = reinterpret_cast<t_SifRpcDataQueue *>(getMemPtr(rdram, qdPtr));
        if (!qd || !sdPtr)
        {
            setReturnU32(ctx, 0);
            return;
        }

        std::lock_guard<std::mutex> lock(g_rpc_mutex);

        if (qd->link == sdPtr)
        {
            t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
            qd->link = sd ? sd->link : 0;
            if (sd)
                sd->link = 0;
            setReturnU32(ctx, sdPtr);
            return;
        }

        uint32_t curPtr = qd->link;
        for (int guard = 0; guard < 1024 && curPtr; ++guard)
        {
            t_SifRpcServerData *cur = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, curPtr));
            if (!cur)
                break;
            if (cur->link == sdPtr)
            {
                t_SifRpcServerData *sd = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, sdPtr));
                cur->link = sd ? sd->link : 0;
                if (sd)
                    sd->link = 0;
                setReturnU32(ctx, sdPtr);
                return;
            }
            curPtr = cur->link;
        }

        setReturnU32(ctx, 0);
    }

    void sceSifCallRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SifCallRpc(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t cid = getRegU32(ctx, 4);
        uint32_t packetAddr = getRegU32(ctx, 5);
        uint32_t packetSize = getRegU32(ctx, 6);
        uint32_t srcExtra = getRegU32(ctx, 7);

        uint32_t sp = getRegU32(ctx, 29);
        uint32_t destExtra = 0;
        uint32_t sizeExtra = 0;
        readStackU32(rdram, sp, 0x10, destExtra);
        readStackU32(rdram, sp, 0x14, sizeExtra);

        if (sizeExtra > 0 && srcExtra && destExtra)
        {
            rpcCopyToRdram(rdram, destExtra, srcExtra, sizeExtra);
        }

        static int logCount = 0;
        if (logCount < 5)
        {
            RUNTIME_LOG("[sceSifSendCmd] cid=0x" << std::hex << cid
                                                 << " packet=0x" << packetAddr
                                                 << " psize=0x" << packetSize
                                                 << " extra=0x" << destExtra << std::dec << std::endl);
            ++logCount;
        }

        // Return non-zero on success.
        setReturnS32(ctx, 1);
    }

    void sceRpcGetPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t queuePtr = getRegU32(ctx, 4);
        setReturnS32(ctx, static_cast<int32_t>(queuePtr));
    }
}
