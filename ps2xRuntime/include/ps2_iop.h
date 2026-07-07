#ifndef PS2_IOP_H
#define PS2_IOP_H

#include <array>
#include <cstdint>
#include <mutex>

constexpr uint32_t IOP_SID_LIBSD = 0x80000701u;

struct PS2EzMidiCompatLayout
{
    uint32_t rpcSid = 0u;
    uint32_t reportedFreeIopBytes = 0x00200000u;
    uint32_t defaultPortVolume = 0x00000100u;
    uint32_t reportedVoiceCount = 0x00000020u;
};

class ps2_iop
{
public:
    ps2_iop();
    ~ps2_iop() = default;

    void init(uint8_t *rdram);
    void reset();
    void setEzMidiCompatLayout(const PS2EzMidiCompatLayout &layout);
    void clearEzMidiCompatLayout();

    bool handleRPC(uint32_t sid, uint32_t rpcNum,
                   uint32_t sendBufAddr, uint32_t sendSize,
                   uint32_t recvBufAddr, uint32_t recvSize,
                   uint32_t &resultPtr);

private:
    struct EzMidiPortState
    {
        bool loaded = false;
        uint32_t status = 0u;
        uint32_t portAttr = 0u;
        uint32_t portVolume = 0u;
        uint32_t iopAddrA = 0u;
        uint32_t iopAddrB = 0u;
        uint32_t dataSize = 0u;
    };

    void resetEzMidiStateLocked();
    bool handleEzMidiRpc(uint32_t sid, uint32_t rpcNum,
                         uint32_t sendBufAddr, uint32_t sendSize,
                         uint32_t recvBufAddr, uint32_t recvSize,
                         uint32_t &resultPtr);

    uint8_t *m_rdram = nullptr;
    mutable std::mutex m_mutex;
    PS2EzMidiCompatLayout m_ezMidiCompat{};
    bool m_ezMidiInitialized = false;
    uint32_t m_ezMidiStereoMode = 0u;
    std::array<EzMidiPortState, 16u> m_ezMidiPorts{};
    uint32_t m_unknownEzMidiLogCount = 0u;
};

#endif
