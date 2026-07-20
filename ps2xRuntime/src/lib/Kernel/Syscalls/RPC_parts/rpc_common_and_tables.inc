#include "Common.h"
#include "RPC.h"
#include <atomic>

// F60 spin diagnostics (defined in ps2_runtime.cpp).
extern std::atomic<uint64_t> g_f60_checkStatRpc;
extern std::atomic<uint32_t> g_f60_checkStatRpc_ra;
extern std::atomic<uint32_t> g_f60_checkStatRpc_client;

namespace ps2_syscalls
{
    namespace
    {
        constexpr uint32_t kSoundDriverStatusSize = 0x42u;
        constexpr uint32_t kSoundDriverSeInfoOffset = 0x00u;
        constexpr uint32_t kSoundDriverMidiInfoOffset = 0x0Cu;
        constexpr uint32_t kSoundDriverMidiSumOffset = 0x1Eu;
        constexpr uint32_t kSoundDriverSeSumOffset = 0x26u;
        constexpr uint32_t kSoundDriverAddrTableEntries = 16u;
        constexpr uint32_t kSoundDriverHdRegionSize = 0x4000u;
        constexpr uint32_t kSoundDriverSqRegionSize = 0x18000u;
        constexpr uint32_t kSoundDriverDataRegionSize = 0x40000u;
        constexpr uint32_t kSoundDriverStatusAlignment = 0x100u;
        constexpr uint32_t kSoundDriverAddrTableAlignment = 0x100u;
        constexpr uint32_t kSoundDriverStorageAlignment = 0x1000u;
        constexpr uint32_t kSoundDriverGuestPoolBase = 0x00120000u;
        constexpr uint32_t kSoundDriverGuestPoolLimit = 0x00200000u;

        void resetDtxRpcStateUnlocked()
        {
            g_dtx_remote_by_id.clear();
            g_dtx_transfer_by_id.clear();
            g_dtx_sjx_by_handle.clear();
            g_dtx_ps2rna_by_handle.clear();
            g_dtx_sjrmt_by_handle.clear();
            g_dtx_next_urpc_obj = g_dtxCompatLayout.urpcObjBase;
        }

        bool readGuestU16(const uint8_t *rdram, uint32_t addr, uint16_t &out)
        {
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                out = 0u;
                return false;
            }
            std::memcpy(&out, ptr, sizeof(out));
            return true;
        }

        bool readGuestS16(const uint8_t *rdram, uint32_t addr, int16_t &out)
        {
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                out = 0;
                return false;
            }
            std::memcpy(&out, ptr, sizeof(out));
            return true;
        }

        bool writeGuestU16(uint8_t *rdram, uint32_t addr, uint16_t value)
        {
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(ptr, &value, sizeof(value));
            return true;
        }

        bool writeGuestS16(uint8_t *rdram, uint32_t addr, int16_t value)
        {
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(ptr, &value, sizeof(value));
            return true;
        }

        bool readGuestU32(const uint8_t *rdram, uint32_t addr, uint32_t &out)
        {
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                out = 0u;
                return false;
            }
            std::memcpy(&out, ptr, sizeof(out));
            return true;
        }

        bool writeGuestU32(uint8_t *rdram, uint32_t addr, uint32_t value)
        {
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(ptr, &value, sizeof(value));
            return true;
        }

        bool normalizeGuestLinearAddr(uint32_t addr, uint32_t &out)
        {
            uint32_t offset = 0u;
            bool scratch = false;
            if (!ps2ResolveGuestPointer(addr, offset, scratch) || scratch)
            {
                out = 0u;
                return false;
            }
            out = offset;
            return true;
        }

        bool hasAnyNonZero(const uint8_t *ptr, size_t bytes)
        {
            if (!ptr)
            {
                return false;
            }
            for (size_t i = 0; i < bytes; ++i)
            {
                if (ptr[i] != 0u)
                {
                    return true;
                }
            }
            return false;
        }

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            if (alignment == 0u)
            {
                return value;
            }
            return (value + (alignment - 1u)) & ~(alignment - 1u);
        }

        void resetSoundDriverRpcStateUnlocked()
        {
            const PS2SoundDriverCompatLayout compat = g_soundDriverCompatLayout;
            g_soundDriverRpcState = {};
            g_soundDriverCompatLayout = compat;
        }

        void adoptSoundDriverRuntimeUnlocked(PS2Runtime *runtime)
        {
            const uintptr_t runtimeCookie = reinterpret_cast<uintptr_t>(runtime);
            if (g_soundDriverRpcState.ownerRuntime != runtimeCookie)
            {
                resetSoundDriverRpcStateUnlocked();
                g_soundDriverRpcState.ownerRuntime = runtimeCookie;
            }
        }

        bool checkSoundDriverMemoryUnlocked(uint8_t *rdram, PS2Runtime *runtime)
        {
            if (!rdram || !runtime)
            {
                return false;
            }

            adoptSoundDriverRuntimeUnlocked(runtime);

            if (g_soundDriverRpcState.statusAddr == 0u)
            {
                const uint32_t statusAddr = alignUp(kSoundDriverGuestPoolBase, kSoundDriverStatusAlignment);
                const uint32_t addrTableAddr =
                    alignUp(statusAddr + kSoundDriverStatusSize, kSoundDriverAddrTableAlignment);
                const uint32_t hdBaseAddr =
                    alignUp(addrTableAddr + (kSoundDriverAddrTableEntries * sizeof(uint32_t)), kSoundDriverStorageAlignment);
                const uint32_t sqBaseAddr = alignUp(hdBaseAddr + kSoundDriverHdRegionSize, kSoundDriverStorageAlignment);
                const uint32_t dataBaseAddr = alignUp(sqBaseAddr + kSoundDriverSqRegionSize, kSoundDriverStorageAlignment);
                const uint32_t storageEnd = dataBaseAddr + kSoundDriverDataRegionSize;
                if (storageEnd > kSoundDriverGuestPoolLimit)
                {
                    return false;
                }

                g_soundDriverRpcState.statusAddr = statusAddr;
                g_soundDriverRpcState.addrTableAddr = addrTableAddr;
                g_soundDriverRpcState.hdBaseAddr = hdBaseAddr;
                g_soundDriverRpcState.sqBaseAddr = sqBaseAddr;
                g_soundDriverRpcState.dataBaseAddr = dataBaseAddr;
                g_soundDriverRpcState.storageBaseAddr = hdBaseAddr;
                g_soundDriverRpcState.storageSize =
                    (dataBaseAddr + kSoundDriverDataRegionSize) - hdBaseAddr;
            }

            if (g_soundDriverRpcState.statusAddr == 0u ||
                g_soundDriverRpcState.addrTableAddr == 0u ||
                g_soundDriverRpcState.storageBaseAddr == 0u)
            {
                return false;
            }

            if (!g_soundDriverRpcState.initialized)
            {
                rpcZeroRdram(rdram, g_soundDriverRpcState.statusAddr, kSoundDriverStatusSize);
                rpcZeroRdram(rdram,
                             g_soundDriverRpcState.addrTableAddr,
                             kSoundDriverAddrTableEntries * sizeof(uint32_t));
                rpcZeroRdram(rdram, g_soundDriverRpcState.storageBaseAddr, g_soundDriverRpcState.storageSize);
                (void)writeGuestU32(rdram, g_soundDriverRpcState.addrTableAddr + (0u * sizeof(uint32_t)), g_soundDriverRpcState.hdBaseAddr);
                (void)writeGuestU32(rdram, g_soundDriverRpcState.addrTableAddr + (1u * sizeof(uint32_t)), g_soundDriverRpcState.sqBaseAddr);
                (void)writeGuestU32(rdram, g_soundDriverRpcState.addrTableAddr + (2u * sizeof(uint32_t)), g_soundDriverRpcState.dataBaseAddr);
                g_soundDriverRpcState.initialized = true;
            }

            return true;
        }

        int16_t soundDriverCheckValue(const uint8_t *rdram, uint32_t primaryBase, uint32_t fallbackBase, uint32_t index, uint32_t count)
        {
            if (index >= count)
            {
                return 0;
            }

            int16_t value = 0;
            if (readGuestS16(rdram, primaryBase + (index * sizeof(int16_t)), value) && value != 0)
            {
                return value;
            }

            (void)readGuestS16(rdram, fallbackBase + (index * sizeof(int16_t)), value);
            return value;
        }

        bool selectSoundDriverCompatChecks(const uint8_t *rdram, uint32_t &seBase, uint32_t &midiBase)
        {
            const PS2SoundDriverCompatLayout &compat = g_soundDriverCompatLayout;
            if (!compat.hasChecksumTables())
            {
                return false;
            }

            seBase = compat.primarySeCheckAddr;
            midiBase = compat.primaryMidiCheckAddr;

            const uint8_t *selectedSe = getConstMemPtr(rdram, seBase);
            const uint8_t *selectedMidi = getConstMemPtr(rdram, midiBase);
            const bool primaryLooksLive =
                hasAnyNonZero(selectedSe, 5u * sizeof(int16_t)) ||
                hasAnyNonZero(selectedMidi, 4u * sizeof(int16_t));

            if ((!selectedSe || !selectedMidi) || !primaryLooksLive)
            {
                const uint8_t *fallbackSe = getConstMemPtr(rdram, compat.fallbackSeCheckAddr);
                const uint8_t *fallbackMidi = getConstMemPtr(rdram, compat.fallbackMidiCheckAddr);
                const bool fallbackLooksLive =
                    hasAnyNonZero(fallbackSe, 5u * sizeof(int16_t)) ||
                    hasAnyNonZero(fallbackMidi, 4u * sizeof(int16_t));
                if (fallbackLooksLive)
                {
                    seBase = compat.fallbackSeCheckAddr;
                    midiBase = compat.fallbackMidiCheckAddr;
                    return true;
                }
            }

            return selectedSe != nullptr && selectedMidi != nullptr;
        }

        void backfillSoundDriverStatusFromCompatUnlocked(uint8_t *rdram)
        {
            if (!rdram || g_soundDriverRpcState.statusAddr == 0u || !g_soundDriverCompatLayout.hasChecksumTables())
            {
                return;
            }

            uint32_t seBase = 0u;
            uint32_t midiBase = 0u;
            if (!selectSoundDriverCompatChecks(rdram, seBase, midiBase))
            {
                return;
            }

            const uint8_t *selectedSe = getConstMemPtr(rdram, seBase);
            const uint8_t *selectedMidi = getConstMemPtr(rdram, midiBase);
            uint8_t *status = getMemPtr(rdram, g_soundDriverRpcState.statusAddr);
            if (!selectedSe || !selectedMidi || !status)
            {
                return;
            }

            auto backfillZeroS16Slots = [&](uint32_t statusOffset, const uint8_t *compatBase, uint32_t slotCount)
            {
                for (uint32_t slot = 0u; slot < slotCount; ++slot)
                {
                    int16_t liveValue = 0;
                    if (!readGuestS16(rdram,
                                      g_soundDriverRpcState.statusAddr + statusOffset + (slot * sizeof(int16_t)),
                                      liveValue))
                    {
                        continue;
                    }

                    if (liveValue != 0)
                    {
                        continue;
                    }

                    int16_t compatValue = 0;
                    std::memcpy(&compatValue, compatBase + (slot * sizeof(int16_t)), sizeof(compatValue));
                    if (compatValue == 0)
                    {
                        continue;
                    }

                    (void)writeGuestS16(rdram,
                                        g_soundDriverRpcState.statusAddr + statusOffset + (slot * sizeof(int16_t)),
                                        compatValue);
                }
            };

            backfillZeroS16Slots(kSoundDriverSeSumOffset, selectedSe, 5u);
            backfillZeroS16Slots(kSoundDriverMidiSumOffset, selectedMidi, 4u);
        }

        size_t soundDriverCommandLength(uint8_t command)
        {
            const uint8_t hi = static_cast<uint8_t>(command & 0xF0u);
            switch (hi)
            {
            case 0x00u:
            {
                size_t len = 4u;
                if ((command & 0x01u) != 0u)
                {
                    ++len;
                }
                if ((command & 0x02u) != 0u)
                {
                    ++len;
                }
                if ((command & 0x04u) != 0u)
                {
                    len += 2u;
                }
                return len;
            }
            case 0x10u:
                return (command == 0x11u) ? 3u : 1u;
            case 0x20u:
                if (command == 0x22u || command == 0x23u || command == 0x24u || command == 0x25u)
                {
                    return 3u;
                }
                if (command == 0x26u)
                {
                    return 4u;
                }
                if (command == 0x20u)
                {
                    return 5u;
                }
                if (command == 0x27u || command == 0x28u || command == 0x29u || command == 0x2Cu || command == 0x2Du)
                {
                    return 8u;
                }
                return 2u;
            case 0x40u:
                if (command == 0x47u || command == 0x48u || command == 0x49u || command == 0x4Au ||
                    command == 0x41u || command == 0x42u)
                {
                    return 2u;
                }
                if (command == 0x4Bu)
                {
                    return 3u;
                }
                if (command == 0x45u || command == 0x4Cu)
                {
                    return 4u;
                }
                if (command == 0x44u)
                {
                    return 6u;
                }
                if (command == 0x4Du || command == 0x4Eu)
                {
                    return 3u;
                }
                if (command == 0x4Fu)
                {
                    return 6u;
                }
                return 1u;
            case 0x50u:
            case 0x60u:
                if (command == 0x51u || command == 0x52u || command == 0x53u || command == 0x54u)
                {
                    return 8u;
                }
                return 2u;
            default:
                return 0u;
            }
        }

        void applySoundDriverCommandUnlocked(uint8_t *rdram, const uint8_t *cmd)
        {
            if (!rdram || !cmd || g_soundDriverRpcState.statusAddr == 0u)
            {
                return;
            }

            const uint8_t op = cmd[0];
            switch (op)
            {
            case 0x20u: // SdrBgmReq
            {
                const uint32_t port = cmd[1] & 0x0Fu;
                uint16_t midiInfo = 0u;
                (void)readGuestU16(rdram, g_soundDriverRpcState.statusAddr + kSoundDriverMidiInfoOffset, midiInfo);
                midiInfo = static_cast<uint16_t>(midiInfo | static_cast<uint16_t>(1u << port));
                (void)writeGuestU16(rdram, g_soundDriverRpcState.statusAddr + kSoundDriverMidiInfoOffset, midiInfo);
                break;
            }
            case 0x21u: // SdrBgmStop
            {
                const uint32_t port = cmd[1] & 0x0Fu;
                uint16_t midiInfo = 0u;
                (void)readGuestU16(rdram, g_soundDriverRpcState.statusAddr + kSoundDriverMidiInfoOffset, midiInfo);
                midiInfo = static_cast<uint16_t>(midiInfo & ~static_cast<uint16_t>(1u << port));
                (void)writeGuestU16(rdram, g_soundDriverRpcState.statusAddr + kSoundDriverMidiInfoOffset, midiInfo);
                break;
            }
            case 0x28u: // SdrHDDataSet
            {
                const uint32_t port = cmd[1] & 0x0Fu;
                const PS2SoundDriverCompatLayout &compat = g_soundDriverCompatLayout;
                if (compat.primaryMidiCheckAddr != 0u || compat.fallbackMidiCheckAddr != 0u)
                {
                    const int16_t checksum =
                        soundDriverCheckValue(rdram, compat.primaryMidiCheckAddr, compat.fallbackMidiCheckAddr, port, 4u);
                    (void)writeGuestS16(rdram,
                                        g_soundDriverRpcState.statusAddr + kSoundDriverMidiSumOffset + (port * sizeof(int16_t)),
                                        checksum);
                }
                break;
            }
            case 0x29u: // SdrHDDataSet2
            {
                const uint32_t port = cmd[1] & 0x0Fu;
                const PS2SoundDriverCompatLayout &compat = g_soundDriverCompatLayout;
                if (compat.primarySeCheckAddr != 0u || compat.fallbackSeCheckAddr != 0u)
                {
                    const int16_t checksum =
                        soundDriverCheckValue(rdram, compat.primarySeCheckAddr, compat.fallbackSeCheckAddr, port, 5u);
                    (void)writeGuestS16(rdram,
                                        g_soundDriverRpcState.statusAddr + kSoundDriverSeSumOffset + (port * sizeof(int16_t)),
                                        checksum);
                }
                break;
            }
            case 0x10u: // SdrSeAllStop
                rpcZeroRdram(rdram, g_soundDriverRpcState.statusAddr + kSoundDriverSeInfoOffset, 6u * sizeof(uint16_t));
                break;
            default:
                break;
            }
        }

        void handleSoundDriverCommandBuffer(uint8_t *rdram, PS2Runtime *runtime, uint32_t sendBuf, uint32_t sendSize)
        {
            if (!rdram || !runtime || !sendBuf || sendSize == 0u)
            {
                return;
            }

            std::lock_guard<std::mutex> lock(g_rpc_mutex);
            if (!checkSoundDriverMemoryUnlocked(rdram, runtime))
            {
                return;
            }

            const uint8_t *packet = getConstMemPtr(rdram, sendBuf);
            if (!packet)
            {
                return;
            }

            for (uint32_t offset = 0u; offset < sendSize;)
            {
                const uint8_t op = packet[offset];
                if (op == 0xFFu)
                {
                    break;
                }

                const size_t len = soundDriverCommandLength(op);
