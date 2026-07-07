#include "ps2_syscalls.h"
#include "ps2_runtime.h"
#include "runtime/ps2_iop_audio.h"
#include "ps2_runtime_macros.h"
#include "ps2_stubs.h"
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
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <ThreadNaming.h>

std::string translatePs2Path(const char *ps2Path);

#include "Helpers/Path.h"
#include "Helpers/State.h"
#include "Helpers/Loader.h"
#include "Helpers/Runtime.h"

// PS2Recomp PR #120 (3fa5bd3) — anti-starvation helper. After a syscall wakes a
// guest waiter, release the global guest-execution lock and yield so the woken
// thread can actually run before the signalling thread re-acquires it. Reuses
// DC2's existing GuestExecutionReleaseScope; no behavior change unless a waiter
// was actually woken (callers gate on `wokeWaiter`/`hadWaiters`).
namespace ps2_syscalls
{
    inline void yieldGuestExecutionAfterWake(PS2Runtime *runtime)
    {
        PS2Runtime::GuestExecutionReleaseScope releaseGuestExecution(runtime);
        std::this_thread::yield();
    }
}
