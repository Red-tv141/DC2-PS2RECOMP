// ===== G657 P1: 15 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
#include "ps2_syscalls.h"
#include "ps2_runtime.h"
#include "ps2_iop_audio.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/GS.h"
#include "ps2_iso_mount.h"
#include <iostream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <memory>
#include <string>

#ifndef _WIN32
#include <unistd.h>   // for unlink,rmdir,chdir
#include <sys/stat.h> // for mkdir
#endif
#include <ThreadNaming.h>

std::string translatePs2Path(const char *ps2Path);

#include "syscalls/helpers/ps2_syscalls_helpers_path.inl"
#include "syscalls/helpers/ps2_syscalls_helpers_state.inl"
#include "syscalls/helpers/ps2_syscalls_helpers_loader.inl"
#include "syscalls/helpers/ps2_syscalls_helpers_runtime.inl"

namespace ps2_syscalls
{
#include "syscalls/ps2_syscalls_interrupt.inl"
#include "syscalls/ps2_syscalls_system.inl"
    void iDeleteSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);


#include "syscalls/ps2_syscalls_thread.inl"
#include "syscalls/ps2_syscalls_flags.inl"
#include "syscalls/ps2_syscalls_rpc.inl"
#include "syscalls/ps2_syscalls_fileio.inl"




    static std::once_flag g_isoMountOpened;
    static void ensureIsoOpen()
    {
        // G449: one data-source resolution rule for every site (ps2_iso_mount.h).
        std::call_once(g_isoMountOpened, []() { (void)dc2OpenGameDataSource(); });
    }

    bool isoFindFileForFio(const char *path, uint32_t *lbaOut, uint32_t *sizeOut)
    {
        ensureIsoOpen();
        if (!getGlobalIsoMount().isOpen())
            return false;
        IsoFileInfo info{};
        if (!getGlobalIsoMount().findFile(path, info))
            return false;
        *lbaOut = info.lba;
        *sizeOut = info.size;
        return true;
    }

    bool isoReadSectorForFio(uint32_t lba, uint32_t count, void *dst)
    {
        ensureIsoOpen();
        return getGlobalIsoMount().readSector(lba, count, dst);
    }

}
