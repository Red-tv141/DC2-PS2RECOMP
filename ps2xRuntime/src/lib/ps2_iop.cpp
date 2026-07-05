#include "runtime/ps2_iop.h"
#include "runtime/ps2_iop_audio.h"
#include "runtime/ps2_iop_dbcman.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "Kernel/Syscalls/RPC.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
    constexpr uint32_t kEzMidiFamilyMask = 0xFFF0u;
    constexpr uint32_t kEzMidiPortMask = 0x000Fu;

    constexpr uint32_t kEzMidiCmdInit = 0x8010u;
    constexpr uint32_t kEzMidiCmdQuit = 0x80F0u;
    constexpr uint32_t kEzMidiCmdStart = 0x0000u;
    constexpr uint32_t kEzMidiCmdStop = 0x0020u;
    constexpr uint32_t kEzMidiCmdSeek = 0x0030u;
    constexpr uint32_t kEzMidiCmdSetSq = 0x0040u;
    constexpr uint32_t kEzMidiCmdSetHd = 0x9050u;
    constexpr uint32_t kEzMidiCmdTransBdPacket = 0x9060u;
    constexpr uint32_t kEzMidiCmdTransBd = 0x9070u;
    constexpr uint32_t kEzMidiCmdGetStatus = 0x8090u;
    constexpr uint32_t kEzMidiCmdSetPortAttr = 0x00A0u;
    constexpr uint32_t kEzMidiCmdSetPortVolume = 0x00B0u;
    constexpr uint32_t kEzMidiCmdSetStereoMode = 0x00C0u;
    constexpr uint32_t kEzMidiCmdGetUseVoiceNum = 0x80D0u;
    constexpr uint32_t kEzMidiCmdGetPortVolume = 0x80E0u;
    constexpr uint32_t kEzMidiCmdGetIopFileLength = 0x9080u;
    constexpr uint32_t kEzMidiCmdIop2Ee = 0x7000u;
    constexpr uint32_t kEzMidiCmdCheckIopMem = 0x8100u;

    constexpr uint32_t kEzMidiStatusStopped = 0u;
    constexpr uint32_t kEzMidiStatusReady = 1u;
    constexpr uint32_t kEzMidiStatusPlaying = 2u;

    bool envFlagEnabled(const char *name)
    {
        const char *value = std::getenv(name);
        return value != nullptr &&
               value[0] != '\0' &&
               std::strcmp(value, "0") != 0 &&
               std::strcmp(value, "false") != 0 &&
               std::strcmp(value, "FALSE") != 0 &&
               std::strcmp(value, "off") != 0 &&
               std::strcmp(value, "OFF") != 0;
    }

    bool phaseDiagnosticsEnabled()
    {
        static const bool enabled = envFlagEnabled("DC2_PHASE_TRACE");
        return enabled;
    }

    uint32_t readRpcWord(const uint8_t *sendPtr, uint32_t sendSize, size_t wordIndex)
    {
        const size_t byteOffset = wordIndex * sizeof(uint32_t);
        if (!sendPtr || sendSize < (byteOffset + sizeof(uint32_t)))
        {
            return 0u;
        }

        uint32_t value = 0u;
        std::memcpy(&value, sendPtr + byteOffset, sizeof(value));
        return value;
    }

    void writeRpcWord(uint8_t *recvPtr, uint32_t recvSize, size_t wordIndex, uint32_t value)
    {
        const size_t byteOffset = wordIndex * sizeof(uint32_t);
        if (!recvPtr || recvSize < (byteOffset + sizeof(uint32_t)))
        {
            return;
        }

        std::memcpy(recvPtr + byteOffset, &value, sizeof(value));
    }
}

ps2_iop::ps2_iop()
{
    reset();
}

void ps2_iop::init(uint8_t *rdram)
{
    m_rdram = rdram;
}

void ps2_iop::reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    resetEzMidiStateLocked();
}

void ps2_iop::setEzMidiCompatLayout(const PS2EzMidiCompatLayout &layout)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ezMidiCompat = layout;
    resetEzMidiStateLocked();
}

void ps2_iop::clearEzMidiCompatLayout()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ezMidiCompat = {};
    resetEzMidiStateLocked();
}

void ps2_iop::resetEzMidiStateLocked()
{
    m_ezMidiInitialized = false;
    m_ezMidiStereoMode = 0u;
    m_unknownEzMidiLogCount = 0u;

    const uint32_t defaultVolume =
        (m_ezMidiCompat.defaultPortVolume != 0u) ? m_ezMidiCompat.defaultPortVolume : 0x100u;
    for (EzMidiPortState &port : m_ezMidiPorts)
    {
        port = {};
        port.portVolume = defaultVolume;
    }
}

bool ps2_iop::handleEzMidiRpc(uint32_t sid, uint32_t rpcNum,
                               uint32_t sendBufAddr, uint32_t sendSize,
                               uint32_t recvBufAddr, uint32_t recvSize,
                               uint32_t &resultPtr)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ezMidiCompat.rpcSid == 0u || sid != m_ezMidiCompat.rpcSid)
    {
        return false;
    }

    const uint8_t *sendPtr = sendBufAddr ? getConstMemPtr(m_rdram, sendBufAddr) : nullptr;

    const uint32_t family = rpcNum & kEzMidiFamilyMask;
    const size_t portIndex = static_cast<size_t>(rpcNum & kEzMidiPortMask);
    EzMidiPortState &port = m_ezMidiPorts[portIndex];

    const uint32_t arg0 = readRpcWord(sendPtr, sendSize, 0u);
    const uint32_t arg1 = readRpcWord(sendPtr, sendSize, 1u);
    const uint32_t arg2 = readRpcWord(sendPtr, sendSize, 2u);
    const uint32_t arg3 = readRpcWord(sendPtr, sendSize, 3u);

    uint32_t replyBufAddr = recvBufAddr ? recvBufAddr : sendBufAddr;
    uint32_t replyCapacity = recvSize;
    if (replyBufAddr != 0u && replyCapacity == 0u)
    {
        // EzMidi EE wrapper reads scratch reply buffer even for rsize==0 calls.
        replyCapacity = std::max(sendSize, 0x10u);
    }

    uint8_t *replyPtr = replyBufAddr ? getMemPtr(m_rdram, replyBufAddr) : nullptr;
    if (replyPtr && replyCapacity > 0u)
    {
        std::memset(replyPtr, 0, replyCapacity);
    }

    switch (family)
    {
    case kEzMidiCmdInit:
        resetEzMidiStateLocked();
        m_ezMidiInitialized = true;
        // EE-side checks recvBuf[0] as iopMSINBuffAddr; must be non-zero or it prints AllocIopHeap Err.
        writeRpcWord(replyPtr, replyCapacity, 0u, 0x00100000u);
        if (phaseDiagnosticsEnabled())
        {
            std::cerr << "[F12:ezinit] replyBufAddr=0x" << std::hex << replyBufAddr
                      << " replyPtr=" << (void*)replyPtr
                      << " cap=" << std::dec << replyCapacity
                      << " wrote 0x100000 to word[0]" << std::endl;
        }
        break;

    case kEzMidiCmdQuit:
        resetEzMidiStateLocked();
        break;

    case kEzMidiCmdStart:
        if (m_ezMidiInitialized)
        {
            port.loaded = true;
            port.status = kEzMidiStatusPlaying;
        }
        break;

    case kEzMidiCmdStop:
        port.status = port.loaded ? kEzMidiStatusReady : kEzMidiStatusStopped;
        break;

    case kEzMidiCmdSeek:
        if (port.loaded && port.status == kEzMidiStatusStopped)
        {
            port.status = kEzMidiStatusReady;
        }
        break;

    case kEzMidiCmdSetSq:
        port.loaded = true;
        if (port.status == kEzMidiStatusStopped)
        {
            port.status = kEzMidiStatusReady;
        }
        break;

    case kEzMidiCmdSetHd:
        port.iopAddrA = arg1;
        port.iopAddrB = arg2;
        port.dataSize = arg3;
        port.loaded = (arg1 != 0u) || (arg2 != 0u) || (arg3 != 0u);
        port.status = port.loaded ? kEzMidiStatusReady : kEzMidiStatusStopped;
        writeRpcWord(replyPtr, replyCapacity, 1u, port.iopAddrA);
        writeRpcWord(replyPtr, replyCapacity, 2u, port.iopAddrB);
        writeRpcWord(replyPtr, replyCapacity, 3u, port.dataSize);
        break;

    case kEzMidiCmdTransBdPacket:
    case kEzMidiCmdTransBd:
        port.loaded = true;
        if (port.status == kEzMidiStatusStopped)
        {
            port.status = kEzMidiStatusReady;
        }
        break;

    case kEzMidiCmdGetStatus:
        writeRpcWord(replyPtr, replyCapacity, 0u, port.status);
        writeRpcWord(replyPtr, replyCapacity, 1u, port.loaded ? 1u : 0u);
        writeRpcWord(replyPtr, replyCapacity, 2u, port.dataSize);
        break;

    case kEzMidiCmdSetPortAttr:
        port.portAttr = arg0;
        if (!port.loaded && arg0 != 0u)
        {
            port.loaded = true;
            port.status = kEzMidiStatusReady;
        }
        break;

    case kEzMidiCmdSetPortVolume:
        port.portVolume = arg0;
        break;

    case kEzMidiCmdSetStereoMode:
        m_ezMidiStereoMode = arg0;
        break;

    case kEzMidiCmdGetUseVoiceNum:
        writeRpcWord(replyPtr, replyCapacity, 0u, m_ezMidiCompat.reportedVoiceCount);
        break;

    case kEzMidiCmdGetPortVolume:
        writeRpcWord(replyPtr, replyCapacity, 0u, port.portVolume);
        break;

    case kEzMidiCmdGetIopFileLength:
        writeRpcWord(replyPtr, replyCapacity, 0u, port.dataSize);
        break;

    case kEzMidiCmdIop2Ee:
        writeRpcWord(replyPtr, replyCapacity, 0u, 0u);
        break;

    case kEzMidiCmdCheckIopMem:
        writeRpcWord(replyPtr, replyCapacity, 0u, m_ezMidiCompat.reportedFreeIopBytes);
        writeRpcWord(replyPtr, replyCapacity, 1u, m_ezMidiCompat.reportedFreeIopBytes);
        break;

    default:
        if (m_unknownEzMidiLogCount < 32u)
        {
            std::cerr << "[ezMidi compat] unhandled rpc family=0x" << std::hex << family
                      << " rpc=0x" << rpcNum
                      << " send=0x" << sendBufAddr
                      << " recv=0x" << recvBufAddr
                      << std::dec << std::endl;
            ++m_unknownEzMidiLogCount;
        }
        break;
    }

    writeRpcWord(replyPtr, replyCapacity, 4u, m_ezMidiStereoMode);
    resultPtr = replyBufAddr;
    return true;
}

bool ps2_iop::handleRPC(PS2Runtime *runtime,
                        uint32_t sid, uint32_t rpcNum,
                        uint32_t sendBufAddr, uint32_t sendSize,
                        uint32_t recvBufAddr, uint32_t recvSize,
                        uint32_t &resultPtr,
                        bool &signalNowaitCompletion)
{
    resultPtr = 0u;
    signalNowaitCompletion = false;

    if (!runtime || !m_rdram)
    {
        return false;
    }

    if (ps2_syscalls::handleSoundDriverRpcService(m_rdram, runtime,
                                                  sid, rpcNum,
                                                  sendBufAddr, sendSize,
                                                  recvBufAddr, recvSize,
                                                  resultPtr,
                                                  signalNowaitCompletion))
    {
        return true;
    }

    // PR131: dbcman (database manager) RPC version handshake.
    if (ps2_iop_dbcman::handleDbcManRpc(m_rdram,
                                        sid, rpcNum,
                                        sendBufAddr, sendSize,
                                        recvBufAddr, recvSize,
                                        resultPtr))
    {
        return true;
    }

    if (phaseDiagnosticsEnabled())
    {
        static uint32_t dbgCount = 0u;
        if (dbgCount < 8u)
        {
            ++dbgCount;
            std::cerr << "[F12:handleRPC] sid=0x" << std::hex << sid
                      << " rpcNum=0x" << rpcNum
                      << " ezRpcSid=0x" << m_ezMidiCompat.rpcSid
                      << std::dec << std::endl;
        }
    }
    if (handleEzMidiRpc(sid, rpcNum,
                        sendBufAddr, sendSize,
                        recvBufAddr, recvSize,
                        resultPtr))
    {
        return true;
    }

    if (sid == IOP_SID_LIBSD)
    {
        const uint8_t *sendPtr = sendBufAddr ? getConstMemPtr(m_rdram, sendBufAddr) : nullptr;
        uint8_t *recvPtr = recvBufAddr ? getMemPtr(m_rdram, recvBufAddr) : nullptr;
        ps2_iop_audio::handleLibSdRpc(runtime, sid, rpcNum, sendPtr, sendSize, recvPtr, recvSize);
        resultPtr = recvBufAddr;
        return true;
    }

    return false;
}
