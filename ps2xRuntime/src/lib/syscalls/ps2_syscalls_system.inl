// ===== G657 P1: 32 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
















static uint32_t computeBuiltinFindAddressResult(uint8_t *rdram,
                                                uint32_t originalStart,
                                                uint32_t originalEnd,
                                                uint32_t target);

static bool dispatchSyscallOverride(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    uint32_t handler = 0u;
    {
        std::lock_guard<std::mutex> lock(g_syscall_override_mutex);
        auto it = g_syscall_overrides.find(syscallNumber);
        if (it == g_syscall_overrides.end())
        {
            return false;
        }
        handler = it->second;
    }

    if (!runtime || !ctx || handler == 0u)
    {
        return false;
    }

    const uint32_t overrideA0 = getRegU32(ctx, 4);
    const uint32_t overrideA1 = getRegU32(ctx, 5);
    const uint32_t overrideA2 = getRegU32(ctx, 6);
    const uint32_t overrideA3 = getRegU32(ctx, 7);
    const uint32_t overridePc = ctx->pc;
    const uint32_t overrideRa = getRegU32(ctx, 31);

    thread_local std::vector<uint32_t> s_activeSyscallOverrides;
    if (std::find(s_activeSyscallOverrides.begin(), s_activeSyscallOverrides.end(), syscallNumber) != s_activeSyscallOverrides.end())
    {
        static std::atomic<uint32_t> s_reentrantLogs{0u};
        constexpr uint32_t kMaxReentrantLogs = 32u;
        const uint32_t logIndex = s_reentrantLogs.fetch_add(1u, std::memory_order_relaxed);
        if (logIndex < kMaxReentrantLogs)
        {
            std::cerr << "[SyscallOverride:reentrant]"
                      << " syscall=0x" << std::hex << syscallNumber
                      << " handler=0x" << handler
                      << " pc=0x" << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << std::dec << std::endl;
        }
        return false;
    }

    s_activeSyscallOverrides.push_back(syscallNumber);
    struct ScopedActiveOverride
    {
        std::vector<uint32_t> &active;
        ~ScopedActiveOverride()
        {
            if (!active.empty())
            {
                active.pop_back();
            }
        }
    } scopedActiveOverride{s_activeSyscallOverrides};

    uint32_t retV0 = 0u;
    const bool invoked = rpcInvokeFunction(rdram,
                                           ctx,
                                           runtime,
                                           handler,
                                           getRegU32(ctx, 4),
                                           getRegU32(ctx, 5),
                                           getRegU32(ctx, 6),
                                           getRegU32(ctx, 7),
                                           &retV0);

    if (syscallNumber == 0x83u)
    {
        const uint32_t builtinRet = computeBuiltinFindAddressResult(rdram, overrideA0, overrideA1, overrideA2);
        const bool mismatch = (retV0 != builtinRet);

        static std::atomic<uint32_t> s_findAddressOverrideLogs{0u};
        static std::atomic<uint32_t> s_findAddressOverrideMismatchLogs{0u};
        constexpr uint32_t kMaxFindAddressOverrideLogs = 64u;
        constexpr uint32_t kMaxFindAddressOverrideMismatchLogs = 128u;

        const uint32_t logIndex = s_findAddressOverrideLogs.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t mismatchIndex = mismatch
                                           ? s_findAddressOverrideMismatchLogs.fetch_add(1u, std::memory_order_relaxed)
                                           : 0u;
        if (logIndex < kMaxFindAddressOverrideLogs ||
            (mismatch && mismatchIndex < kMaxFindAddressOverrideMismatchLogs))
        {
            const uint32_t guestMinus20c = (retV0 != 0u) ? (retV0 - 0x20Cu) : 0u;
            const uint32_t guestMinus168 = (retV0 != 0u) ? (retV0 - 0x168u) : 0u;
            const uint32_t builtinMinus20c = (builtinRet != 0u) ? (builtinRet - 0x20Cu) : 0u;
            const uint32_t builtinMinus168 = (builtinRet != 0u) ? (builtinRet - 0x168u) : 0u;

            std::cerr << "[Syscall83:override]"
                      << " handler=0x" << std::hex << handler
                      << " invoked=" << (invoked ? "true" : "false")
                      << " pc=0x" << overridePc
                      << " ra=0x" << overrideRa
                      << " a0=0x" << overrideA0
                      << " a1=0x" << overrideA1
                      << " a2=0x" << overrideA2
                      << " a3=0x" << overrideA3
                      << " guestRet=0x" << retV0
                      << " builtinRet=0x" << builtinRet
                      << " guest-20c=0x" << guestMinus20c
                      << " builtin-20c=0x" << builtinMinus20c
                      << " guest-168=0x" << guestMinus168
                      << " builtin-168=0x" << builtinMinus168
                      << " match=" << (mismatch ? "false" : "true")
                      << std::dec << std::endl;
        }
    }

    if (!invoked)
    {
        static std::atomic<uint32_t> s_fallbackLogs{0u};
        constexpr uint32_t kMaxFallbackLogs = 64u;
        const uint32_t logIndex = s_fallbackLogs.fetch_add(1u, std::memory_order_relaxed);
        if (logIndex < kMaxFallbackLogs)
        {
            std::cerr << "[SyscallOverride:fallback]"
                      << " syscall=0x" << std::hex << syscallNumber
                      << " handler=0x" << handler
                      << " pc=0x" << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << std::dec << std::endl;
        }
        return false;
    }

    setReturnU32(ctx, retV0);
    return true;
}

static bool tryResolveGuestSyscallMirrorAddr(uint32_t syscallIndex, uint32_t &guestAddr)
{
    const int64_t offsetBytes =
        static_cast<int64_t>(static_cast<int32_t>(syscallIndex)) * static_cast<int64_t>(sizeof(uint32_t));
    const int64_t guestAddr64 = static_cast<int64_t>(kGuestSyscallTablePhysBase) + offsetBytes;
    if (guestAddr64 < 0 || (guestAddr64 + static_cast<int64_t>(sizeof(uint32_t))) > static_cast<int64_t>(kGuestSyscallMirrorLimit))
    {
        return false;
    }

    guestAddr = static_cast<uint32_t>(guestAddr64);
    return true;
}

static void writeGuestKernelWord(uint8_t *rdram, uint32_t guestAddr, uint32_t value)
{
    if (!rdram)
    {
        return;
    }

    if (uint8_t *ptr = getMemPtr(rdram, guestAddr))
    {
        std::memcpy(ptr, &value, sizeof(value));
    }
}

static void seedGuestSyscallTableProbeLocked(uint8_t *rdram)
{
    writeGuestKernelWord(rdram, kGuestSyscallTableProbeBase + 0u, kGuestSyscallTableGuestBase >> 16);
    writeGuestKernelWord(rdram, kGuestSyscallTableProbeBase + 8u, kGuestSyscallTableGuestBase & 0xFFFFu);
    g_syscall_mirror_addrs.insert(kGuestSyscallTableProbeBase + 0u);
    g_syscall_mirror_addrs.insert(kGuestSyscallTableProbeBase + 8u);
}

static void mirrorGuestSyscallEntryLocked(uint8_t *rdram, uint32_t syscallIndex, uint32_t handler)
{
    uint32_t guestAddr = 0u;
    if (!tryResolveGuestSyscallMirrorAddr(syscallIndex, guestAddr))
    {
        return;
    }

    writeGuestKernelWord(rdram, guestAddr, handler);
    if (handler == 0u)
    {
        g_syscall_mirror_addrs.erase(guestAddr);
        return;
    }

    g_syscall_mirror_addrs.insert(guestAddr);
}



// 0x3C SetupThread
// args: $a0 = gp, $a1 = stack, $a2 = stack_size, $a3 = args, $t0 = root_func

// 0x3D SetupHeap: returns heap base/start pointer

// 0x3E EndOfHeap: commonly returns current heap end; keep it stable for now.


static inline uint32_t normalizeKernelAlias(uint32_t addr)
{
    if (addr >= 0x80000000u && addr < 0xC0000000u)
    {
        return addr & 0x1FFFFFFFu;
    }
    return addr;
}

static uint32_t computeBuiltinFindAddressResult(uint8_t *rdram,
                                                uint32_t originalStart,
                                                uint32_t originalEnd,
                                                uint32_t target)
{
    uint32_t start = (originalStart + 3u) & ~0x3u;
    uint32_t end = originalEnd & ~0x3u;
    if (start >= end)
    {
        return 0u;
    }

    const uint32_t targetNorm = normalizeKernelAlias(target);
    for (uint32_t addr = start; addr < end; addr += sizeof(uint32_t))
    {
        const uint8_t *entryPtr = getConstMemPtr(rdram, addr);
        if (!entryPtr)
        {
            break;
        }

        uint32_t entry = 0u;
        std::memcpy(&entry, entryPtr, sizeof(entry));
        if (entry == target || normalizeKernelAlias(entry) == targetNorm)
        {
            return addr;
        }
    }

    return 0u;
}

struct FindAddressWordSample
{
    uint32_t addr = 0u;
    uint32_t value = 0u;
};

struct FindAddressMatchSample
{
    uint32_t addr = 0u;
    uint32_t value = 0u;
    bool aliasOnly = false;
};

static void logFindAddressDiagnostics(uint32_t callerPc,
                                      uint32_t originalStart,
                                      uint32_t originalEnd,
                                      uint32_t alignedStart,
                                      uint32_t alignedEnd,
                                      uint32_t target,
                                      uint32_t targetNorm,
                                      bool found,
                                      uint32_t resultAddr,
                                      uint32_t scannedWords,
                                      bool allZero,
                                      bool aborted,
                                      uint32_t abortedAddr,
                                      const FindAddressWordSample *firstWords,
                                      uint32_t firstWordCount,
                                      const FindAddressWordSample *nonZeroWords,
                                      uint32_t nonZeroWordCount,
                                      const FindAddressMatchSample *matches,
                                      uint32_t matchCount)
{
    static std::atomic<uint32_t> s_findAddressHitLogs{0u};
    static std::atomic<uint32_t> s_findAddressMissLogs{0u};
    constexpr uint32_t kMaxFindAddressHitLogs = 16u;
    constexpr uint32_t kMaxFindAddressMissLogs = 128u;

    std::atomic<uint32_t> &counter = found ? s_findAddressHitLogs : s_findAddressMissLogs;
    const uint32_t logIndex = counter.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t logLimit = found ? kMaxFindAddressHitLogs : kMaxFindAddressMissLogs;
    if (logIndex >= logLimit)
    {
        return;
    }

    std::cerr << "[FindAddress:" << (found ? "hit" : "miss") << "]"
              << " pc=0x" << std::hex << callerPc
              << " start=0x" << originalStart
              << " end=0x" << originalEnd
              << " alignedStart=0x" << alignedStart
              << " alignedEnd=0x" << alignedEnd
              << " target=0x" << target
              << " targetNorm=0x" << targetNorm
              << " result=0x" << resultAddr
              << std::dec
              << " scannedWords=" << scannedWords
              << " allZero=" << (allZero ? "true" : "false")
              << " aborted=" << (aborted ? "true" : "false");
    if (aborted)
    {
        std::cerr << " abortedAddr=0x" << std::hex << abortedAddr << std::dec;
    }
    std::cerr << std::endl;

    std::cerr << "  firstWords:";
    if (firstWordCount == 0u)
    {
        std::cerr << " none";
    }
    else
    {
        for (uint32_t i = 0; i < firstWordCount; ++i)
        {
            std::cerr << " [0x" << std::hex << firstWords[i].addr
                      << "]=0x" << firstWords[i].value;
        }
        std::cerr << std::dec;
    }
    std::cerr << std::endl;

    std::cerr << "  nonZeroSample:";
    if (nonZeroWordCount == 0u)
    {
        std::cerr << " none";
    }
    else
    {
        for (uint32_t i = 0; i < nonZeroWordCount; ++i)
        {
            std::cerr << " [0x" << std::hex << nonZeroWords[i].addr
                      << "]=0x" << nonZeroWords[i].value;
        }
        std::cerr << std::dec;
    }
    std::cerr << std::endl;

    std::cerr << "  matches:";
    if (matchCount == 0u)
    {
        std::cerr << " none";
    }
    else
    {
        for (uint32_t i = 0; i < matchCount; ++i)
        {
            std::cerr << " [0x" << std::hex << matches[i].addr
                      << "]=0x" << matches[i].value
                      << (matches[i].aliasOnly ? "(alias)" : "(exact)");
        }
        std::cerr << std::dec;
    }
    std::cerr << std::endl;
}

// 0x83 FindAddress:
// - a0: table start (inclusive)
// - a1: table end (exclusive)
// - a2: target address to locate inside the table (word entries)
// Returns the guest address of the matching word entry, or 0 if not found.


// 0x5A QueryBootMode (stub): return 0 for now

// 0x5B GetThreadTLS (stub): return 0

// 0x74 RegisterExitHandler (stub): return 0






