#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include "ps2_runtime_parts/dc2_logger.inc"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib> // [G380] DC2_G380_NO_EZMIDI
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

namespace
{
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<ps2_game_overrides::Descriptor> &descriptorRegistry()
    {
        static std::vector<ps2_game_overrides::Descriptor> registry;
        return registry;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(lhs[i]);
            const auto r = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(l) != std::tolower(r))
            {
                return false;
            }
        }

        return true;
    }

    std::string basenameFromPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        const std::filesystem::path leaf = fsPath.filename();
        if (leaf.empty())
        {
            return path;
        }
        return leaf.string();
    }

    uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t size)
    {
        static std::array<uint32_t, 256> table = []()
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t c = i;
                for (int bit = 0; bit < 8; ++bit)
                {
                    c = (c & 1u) ? (0xEDB88320u ^ (c >> 1u)) : (c >> 1u);
                }
                values[i] = c;
            }
            return values;
        }();

        uint32_t out = crc;
        for (size_t i = 0; i < size; ++i)
        {
            out = table[(out ^ data[i]) & 0xFFu] ^ (out >> 8u);
        }
        return out;
    }

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        std::array<uint8_t, 4096> chunk{};
        uint32_t crc = 0xFFFFFFFFu;

        while (file.good())
        {
            file.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
            const std::streamsize got = file.gcount();
            if (got <= 0)
            {
                break;
            }
            crc = crc32Update(crc, chunk.data(), static_cast<size_t>(got));
        }

        crcOut = ~crc;
        return true;
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandlerByName(std::string_view handlerName)
    {
        const std::string_view resolvedSyscall = ps2_runtime_calls::resolveSyscallName(handlerName);
        if (!resolvedSyscall.empty())
        {
#define PS2_RESOLVE_SYSCALL(name)                   \
    if (resolvedSyscall == std::string_view{#name}) \
    {                                               \
        return &ps2_syscalls::name;                 \
    }
            PS2_SYSCALL_LIST(PS2_RESOLVE_SYSCALL)
#undef PS2_RESOLVE_SYSCALL
        }

        const std::string_view resolvedStub = ps2_runtime_calls::resolveStubName(handlerName);
        if (!resolvedStub.empty())
        {
#define PS2_RESOLVE_STUB(name)                   \
    if (resolvedStub == std::string_view{#name}) \
    {                                            \
        return &ps2_stubs::name;                 \
    }
            PS2_STUB_LIST(PS2_RESOLVE_STUB)
#undef PS2_RESOLVE_STUB
        }

        return std::nullopt;
    }
}

namespace ps2_game_overrides
{
    AutoRegister::AutoRegister(const Descriptor &descriptor)
    {
        registerDescriptor(descriptor);
    }

    void registerDescriptor(const Descriptor &descriptor)
    {
        if (!descriptor.apply)
        {
            LOG_WARN(GameOverride, "Ignoring descriptor with null apply callback.");
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        descriptorRegistry().push_back(descriptor);
    }

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address, std::string_view handlerName)
    {
        const auto resolved = resolveHandlerByName(handlerName);
        if (!resolved.has_value())
        {
            dc2_log::recordOverrideRegistration(address, handlerName, false);
            if (dc2_log::shouldLogUnresolvedCall(address))
            {
                LOG_DEBUG(GameOverride, "Unresolved handler '%.*s' for address 0x%08X",
                          static_cast<int>(handlerName.size()), handlerName.data(), address);
            }
            return false;
        }

        dc2_log::recordOverrideRegistration(address, handlerName, true);
        runtime.registerFunction(address, resolved.value());
        return true;
    }

    void applyMatching(PS2Runtime &runtime, const std::string &elfPath, uint32_t entry)
    {
        ps2_syscalls::clearSoundDriverCompatLayout();
        ps2_syscalls::clearDtxCompatLayout();
        ps2_stubs::clearMpegCompatLayout();
        runtime.iop().clearEzMidiCompatLayout(); // [G380] restored from patch/darkCloud2.patch

        std::vector<Descriptor> descriptors;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            descriptors = descriptorRegistry();
        }

        if (descriptors.empty())
        {
            return;
        }

        const std::string elfName = basenameFromPath(elfPath);
        uint32_t fileCrc32 = 0u;
        bool fileCrcComputed = false;
        bool fileCrcValid = false;

        size_t appliedCount = 0;
        for (const Descriptor &descriptor : descriptors)
        {
            if (!descriptor.apply)
            {
                continue;
            }

            if (descriptor.elfName && descriptor.elfName[0] != '\0')
            {
                if (!equalsIgnoreCaseAscii(descriptor.elfName, elfName))
                {
                    continue;
                }
            }

            if (descriptor.entry != 0u && descriptor.entry != entry)
            {
                continue;
            }

            if (descriptor.crc32 != 0u)
            {
                if (!fileCrcComputed)
                {
                    fileCrcComputed = true;
                    fileCrcValid = computeFileCrc32(elfPath, fileCrc32);
                    if (!fileCrcValid)
                    {
                        LOG_WARN(GameOverride, "Failed to compute CRC32 for '%s'", elfPath.c_str());
                    }
                }

                if (!fileCrcValid || fileCrc32 != descriptor.crc32)
                {
                    continue;
                }
            }

            const char *name = (descriptor.name && descriptor.name[0] != '\0')
                                   ? descriptor.name
                                   : "unnamed";
            LOG_INFO(GameOverride, "Applying '%s'", name);
            descriptor.apply(runtime);
            ++appliedCount;
        }

        if (appliedCount > 0)
        {
            LOG_INFO(GameOverride, "Applied %zu matching override(s).", appliedCount);
            dc2_log::logOverrideSummary();
        }
    }
}

namespace
{
    void applyRecvxSoundDriverCompat(PS2Runtime &runtime)
    {
        (void)runtime;

        // Trying to explain a bit of Resident Evil Code: Veronica X sound-driver guest globals.
        // Update these guest addresses/callback PCs when porting the override to another build:
        // - checksum tables back the SE/MIDI status values mirrored through the snddrv RPC stubs
        // - busyFlagAddr is the guest-side "work in progress" word cleared on completion
        // - completion/clearBusy callbacks are guest PCs reached when async snddrv work finishes
        PS2SoundDriverCompatLayout layout{};
        layout.primarySeCheckAddr = 0x01E0EF10u;
        layout.primaryMidiCheckAddr = 0x01E0EF20u;
        layout.fallbackSeCheckAddr = 0x01E1EF10u;
        layout.fallbackMidiCheckAddr = 0x01E1EF20u;
        layout.busyFlagAddr = 0x01E212C8u;
        layout.completionCallbacks = {0x002EAC20u, 0x002EAC30u, 0x002FAC20u, 0x002FAC30u};
        layout.clearBusyCallbacks = {0x002EAC30u, 0x002FAC30u};
        ps2_syscalls::setSoundDriverCompatLayout(layout);
    }

    void applyRecvxDtxCompat(PS2Runtime &runtime)
    {
        (void)runtime;

        // Trying to explain abit of Resident Evil Code: Veronica X DTX guest layout.
        // Update these guest values when porting the middleware override to another build:
        // - rpcSid identifies the DTX RPC service the guest binds/registers
        // - urpc object/table addresses back the SJX/PS2RNA/SJRMT command tables
        // - dispatcherFuncAddr is the guest-side DTX RPC handler used for URPC dispatch
        PS2DtxCompatLayout layout{};
        layout.rpcSid = 0x7D000000u;
        layout.urpcObjBase = 0x01F18000u;
        layout.urpcObjLimit = 0x01F1FF00u;
        layout.urpcObjStride = 0x20u;
        layout.urpcFnTableBase = 0x0034FED0u;
        layout.urpcObjTableBase = 0x0034FFD0u;
        layout.dispatcherFuncAddr = 0x002FABC0u;
        ps2_syscalls::setDtxCompatLayout(layout);
    }

    void applyRecvxMpegCompat(PS2Runtime &runtime)
    {
        (void)runtime;

        // this is temporary so ignore for now
        PS2MpegCompatLayout layout{};
        layout.mpegObjectAddr = 0x01E27140u;
        layout.videoStateAddr = 0x01E271E8u;
        layout.movieStateAddr = 0x01E21914u;
        layout.syntheticFramesBeforeEnd = 1u;
        layout.finishedVideoStateValue = 3u;
        layout.finishedMovieStateValue = 3u;
        ps2_stubs::setMpegCompatLayout(layout);
    }

    // [G380] Restored from `patch/darkCloud2.patch` — this registration was dropped during the
    // fork's PR port while the ~300 lines of ezMidi RPC handling it arms (`ps2_iop.cpp`,
    // `handleEzMidiRpc`) came across intact. `handleEzMidiRpc` bails on
    // `m_ezMidiCompat.rpcSid == 0u`, so without this override every ezMidi RPC fell through
    // unhandled and the whole ezMidi layer was dead code.
    // Rollback: DC2_G380_NO_EZMIDI=1.
    void applyDarkCloud2EzMidiCompat(PS2Runtime &runtime)
    {
        static const bool s_disabled = (std::getenv("DC2_G380_NO_EZMIDI") != nullptr);
        if (s_disabled)
        {
            runtime.iop().clearEzMidiCompatLayout();
            return;
        }

        PS2EzMidiCompatLayout layout{};
        layout.rpcSid = 0x00012346u;
        layout.reportedFreeIopBytes = 0x00200000u;
        layout.defaultPortVolume = 0x00000100u;
        layout.reportedVoiceCount = 0x00000020u;
        runtime.iop().setEzMidiCompatLayout(layout);

        // One line, always on: RUNTIME_LOG is compiled out in Release, and a silently
        // unregistered override is exactly the failure this fix is repairing.
        std::cerr << "[game_overrides] DC2 ezMidi compat armed: rpcSid=0x"
                  << std::hex << layout.rpcSid << std::dec << std::endl;
    }

    PS2_REGISTER_GAME_OVERRIDE("RECVX sound-driver compat", "slus_201.84", 0u, 0u, &applyRecvxSoundDriverCompat);
    PS2_REGISTER_GAME_OVERRIDE("RECVX DTX compat", "slus_201.84", 0u, 0u, &applyRecvxDtxCompat);
    PS2_REGISTER_GAME_OVERRIDE("RECVX MPEG compat", "slus_201.84", 0u, 0u, &applyRecvxMpegCompat);
    PS2_REGISTER_GAME_OVERRIDE("Dark Cloud 2 ezMidi compat", "scus_972.13", 0u, 0u, &applyDarkCloud2EzMidiCompat);
}
