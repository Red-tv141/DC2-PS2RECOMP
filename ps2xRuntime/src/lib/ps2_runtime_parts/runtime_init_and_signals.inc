bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }

#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#endif
        // G158a (default OFF, DC2_G158_GPURASTER=1): one-shot synthetic FBO/shader/readback
        // round-trip proof on the just-created GL context. No effect on real rendering; see
        // plans/phase-G158-fix-log.md. A no-op single boolean read when unset.
        // G159: capture this (main-thread) HGLRC/HDC BEFORE the MTGS worker thread can exist, so
        // g158RunWorkerContextTest() (ps2_gif_arbiter.cpp, runs later on the worker thread) can
        // create a shared second context against it. Also a no-op when the env var is unset.
        extern void g158CaptureMainContext();
        extern void g158RunSelfTest();
        g158CaptureMainContext();
        g158RunSelfTest();
        // G160 (default OFF, same DC2_G158_GPURASTER switch): G159 found sharing fails while the
        // main context is current elsewhere; this releases it, shares+tests on a fresh dedicated
        // GPU thread, then restores it here before raylib's own frame loop starts. Blocking
        // (one-time startup stall) and a no-op when unset. See plans/phase-G160-fix-log.md.
        extern void g158StartDedicatedGpuThread();
        g158StartDedicatedGpuThread();
        // G162 (default OFF, needs DC2_G158_GPURASTER=1 AND DC2_G162_GPU_DECODE=1): starts a
        // SECOND, PERSISTENT dedicated GPU thread (same release-before-share pattern G160 just
        // validated above) that stays alive for the process lifetime, draining PSMT8 texture
        // de-swizzle+CLUT decode jobs submitted from G149's texture-cache build site. A no-op
        // when either env var is unset. See plans/phase-G162-fix-log.md.
        extern void g162StartPersistentDecoder();
        g162StartPersistentDecoder();
        // G178 (default ON since 2026-07-10, kill DC2_G178_NO_GPU=1): starts the persistent LLE
        // GPU rasterizer backend thread (same release-before-share pattern as G160/G162 above;
        // the main context capture above also fires for this lever). See
        // plans/gpu-raster-arc-plan.md and plans/phase-G178-fix-log.md.
        extern void g178StartPersistentBackend();
        g178StartPersistentBackend();

        SetTargetFPS(60);

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    if (std::getenv("DC2_TRACE_F58") != nullptr)
    {
        std::fprintf(stderr,
                     "[F58:elfheap] maxLoadedRdramEnd=0x%x paddedEnd=0x%x suggestedHeapBase=0x%x "
                     "suggestedBase=0x%x hardLimit=0x%x RAM=0x%x\n",
                     maxLoadedRdramEnd, paddedEnd, suggestedHeapBase, m_guestHeapSuggestedBase,
                     (unsigned)std::min(kGuestHeapHardLimit, PS2_RAM_SIZE), (unsigned)PS2_RAM_SIZE);
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    ps2_game_overrides::applyMatching(*this, elfPath, m_cpuContext.pc);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

void PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    m_functionTable[address] = func;
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    auto it = m_functionTable.find(address);
    if (it != m_functionTable.end())
    {
        return true;
    }

    return false;
}

// G186 watch state (see lookupFunction / the first-bad-pc handler)
struct G186WatchChange
{
    uint32_t oldVal, newVal, prevAddr, nextAddr;
};
static G186WatchChange g_g186Ring[64];
static uint32_t g_g186RingN = 0u;
static const uint32_t g_g186WatchAddr = []() -> uint32_t {
    const char *e = std::getenv("DC2_G186_WATCH");
    return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
}();

// G186 sp-balance trampoline (opt-in DC2_G186_SPBAL=1): every dispatched call is
// wrapped so a callee that returns with a shifted guest $sp is named directly.
// (Guest thread switches / longjmp-class flows legitimately change sp — read the
// log with that in mind; the leak signature is a REPEATED same-address entry.)
static const bool g_g186SpBal = (std::getenv("DC2_G186_SPBAL") != nullptr);
bool g_g186SpBalArmed = false; // set by the G186 EditDraw probe (dc2_game_override.cpp)
struct G186SpEnt
{
    PS2Runtime::RecompiledFunction fn;
    uint32_t addr;
};
static thread_local std::vector<G186SpEnt> g_g186SpStack;
static void g186SpBalTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    G186SpEnt e = g_g186SpStack.back();
    g_g186SpStack.pop_back();
    const uint32_t spIn = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
    e.fn(rdram, ctx, runtime);
    const uint32_t spOut = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
    if (spOut != spIn)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = ++s_n;
        if (g_g186SpBalArmed && n <= 8192u)
            std::fprintf(stderr, "[G186:spbal] fn=0x%x spIn=0x%x spOut=0x%x delta=%d n=%u\n",
                         e.addr, spIn, spOut, static_cast<int32_t>(spOut - spIn), n);
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    // G58: detect the function that clobbers TitleMapDraw's saved-$ra slot (0x1fff800).
    static const bool s_g58 = (std::getenv("DC2_TRACE_G58") != nullptr);
    if (s_g58 && g_g58Rdram && address == 0x002A2280u)
    {
        // One-shot title-scene camera-state dump on TitleMapDraw entry. HW (live): scene
        // 0x1dd8260, count=8, slot0 +0x00=0x4 +0x34=0x785f70 (camera obj, vt 0x374e70).
        static int s_camDumps = 0;
        if (s_camDumps < 6)
        {
            ++s_camDumps;
            const uint32_t scene = readGuestU32Wrapped(g_g58Rdram, 0x00377EDCu);
            const uint32_t count = scene ? readGuestU32Wrapped(g_g58Rdram, scene + 0x2044u) : 0u;
            const uint32_t camId = scene ? readGuestU32Wrapped(g_g58Rdram, scene + 0x2E54u) : 0u;
            const uint32_t slot0v = scene ? readGuestU32Wrapped(g_g58Rdram, scene + 0x2048u) : 0u;
            const uint32_t slot0o = scene ? readGuestU32Wrapped(g_g58Rdram, scene + 0x2048u + 0x34u) : 0u;
            const uint32_t camObjVt = readGuestU32Wrapped(g_g58Rdram, 0x00785F70u + 0x60u);
            const uint32_t camObjW0 = readGuestU32Wrapped(g_g58Rdram, 0x00785F70u);
            std::cerr << "[G58:camstate] scene=0x" << std::hex << scene
                      << " count=" << std::dec << count
                      << " camId=" << camId
                      << " slot0.valid=0x" << std::hex << slot0v
                      << " slot0.obj=0x" << slot0o
                      << " | 0x785f70[0]=0x" << camObjW0
                      << " 0x785f70+0x60(vt)=0x" << camObjVt
                      << std::dec << std::endl;
        }
    }
    if (s_g58 && g_g58Rdram)
    {
        const uint32_t slot = readGuestU32Wrapped(g_g58Rdram, 0x01FFF800u);
        if (slot != g_g58LastSlot)
        {
            std::cerr << "[G58:write] 0x1fff800 0x" << std::hex << g_g58LastSlot
                      << " -> 0x" << slot
                      << " after fn@0x" << g_g58PrevPc
                      << " (next=0x" << address << ")" << std::dec << std::endl;
            g_g58LastSlot = slot;
        }
        g_g58PrevPc = address;
    }

    // F64 DISCOVERY: capture the set of INDIRECTLY-called guest functions while the
    // dungeon-entrance event is parked (DngStatus==2, CRunScript done==0). The
    // audio-gated wait command (op-0x15 ext native fn) is reached via the indirect
    // call in ext__10CRunScript@0x1870bc, so it shows up here. Cross-ref the printed
    // addresses against ref/decompiled.txt to name the sound-state query. Gated
    // DC2_TRACE_F64 (default OFF, quiet otherwise).
    {
        static const bool s_f64disc = (std::getenv("DC2_TRACE_F64") != nullptr);
        if (s_f64disc)
        {
            uint8_t *rd = m_memory.getRDRAM();
            if (rd)
            {
                int32_t status;
                uint32_t done, dngMap;
                std::memcpy(&status, rd + (0x01E9F6E0u & 0x01FFFFFFu), 4);
                std::memcpy(&done, rd + (0x01ece40cu & 0x01FFFFFFu), 4);
                std::memcpy(&dngMap, rd + (0x003772A4u & 0x01FFFFFFu), 4);
                if (status == 2 && done == 0u && dngMap != 0u)
                {
                    static uint32_t s_seen[128] = {0};
                    static uint32_t s_n = 0u;
                    bool found = false;
                    for (uint32_t i = 0; i < s_n; ++i)
                        if (s_seen[i] == address) { found = true; break; }
                    if (!found && s_n < 128u)
                    {
                        s_seen[s_n++] = address;
                        std::fprintf(stderr, "[F64:indcall] addr=0x%x distinct=%u\n", address, s_n);
                    }
                }
            }
        }
    }

    // G186 (opt-in DC2_G186_WATCH=<hex guest addr>): watch one guest word at every
    // dispatch boundary, keep a RING of the most recent changes, and dump it from the
    // first-bad-pc handler. Same technique as the G58 saved-$ra clobber hunt, but
    // address-configurable and crash-anchored (streaming hit its cap long before the
    // crash; only the last few changes before first-bad-pc matter).
    if (g_g186WatchAddr)
    {
        static uint32_t s_lastVal = 0u;
        static uint32_t s_prevAddr = 0u;
        uint8_t *rd = m_memory.getRDRAM();
        if (rd)
        {
            const uint32_t v = readGuestU32Wrapped(rd, g_g186WatchAddr);
            if (v != s_lastVal)
            {
                G186WatchChange &c = g_g186Ring[g_g186RingN++ & 63u];
                c.oldVal = s_lastVal;
                c.newVal = v;
                c.prevAddr = s_prevAddr;
                c.nextAddr = address;
                if (g_g186RingN <= 32u)
                    std::fprintf(stderr, "[G186:watch] 0x%x: 0x%x -> 0x%x after=0x%x next=0x%x\n",
                                 g_g186WatchAddr, s_lastVal, v, s_prevAddr, address);
                s_lastVal = v;
            }
        }
        // Sentinel ring entries mark scope: EditDraw / DngMainDraw / EditLoop entries.
        if (address == 0x001AE3D0u || address == 0x001CF090u || address == 0x001ABCF0u)
        {
            G186WatchChange &c = g_g186Ring[g_g186RingN++ & 63u];
            c.oldVal = 0xEDEDEDEDu;
            c.newVal = 0xEDEDEDEDu;
            c.prevAddr = s_prevAddr;
            c.nextAddr = address;
        }
        s_prevAddr = address;
    }

    auto it = m_functionTable.find(address);
    if (it != m_functionTable.end())
    {
        if (g_g186SpBal)
        {
            g_g186SpStack.push_back({it->second, address});
            return g186SpBalTrampoline;
        }
        return it->second;
    }

    std::cerr << "Warning: Function at address 0x" << std::hex << address << std::dec << " not found" << std::endl;

    static RecompiledFunction defaultFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
        const uint32_t sp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u;
        const uint32_t gp = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0)) : 0u;
        const uint32_t a0 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0)) : 0u;
        const uint32_t a1 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0)) : 0u;
        const uint32_t v0 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0)) : 0u;
        const uint32_t v1 = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0)) : 0u;

        if (ctx && runtime)
        {
            thread_local uint32_t s_recoverCount = 0u;
            thread_local bool s_loggedContext = false;
            const uint32_t pc = ctx->pc;
            const bool hasPcFunction = runtime->hasFunction(pc);

            if (!hasPcFunction && s_recoverCount < 8192u)
            {
                if (!s_loggedContext)
                {
                    std::ostringstream stackDump;
                    if (rdram)
                    {
                        stackDump << " [stack]";
                        for (uint32_t off = 0u; off < 0x40u; off += 4u)
                        {
                            const uint32_t slot = readGuestU32Wrapped(rdram, sp + off);
                            stackDump << " +" << std::hex << off << "=0x" << slot;
                        }
                    }
                    std::cerr << "[dispatch:first-bad-pc] bad=0x" << std::hex << pc
                              << " ra=0x" << ra
                              << " sp=0x" << sp
                              << " gp=0x" << gp
                              << " v0=0x" << v0
                              << " v1=0x" << v1
                              << " a0=0x" << a0
                              << " a1=0x" << a1
                              << " trace=" << formatDispatchHistory()
                              << stackDump.str()
                              << std::dec << std::endl;
                    s_loggedContext = true;

                    // G186: dump the watch ring (most recent changes of the watched word)
                    if (g_g186WatchAddr)
                    {
                        const uint32_t n = g_g186RingN < 64u ? g_g186RingN : 64u;
                        for (uint32_t i = 0; i < n; ++i)
                        {
                            const G186WatchChange &c = g_g186Ring[(g_g186RingN - n + i) & 63u];
                            std::fprintf(stderr, "[G186:ring] %u/%u 0x%x: 0x%x -> 0x%x after=0x%x next=0x%x\n",
                                         i + 1, n, g_g186WatchAddr, c.oldVal, c.newVal, c.prevAddr, c.nextAddr);
                        }
                    }

                    // G186 (opt-in DC2_G186_BADPTR_DUMP=1): dump the entire guest RAM at the
                    // moment of the FIRST bad indirect-call target so offline tooling can
                    // locate which structure held the garbage pointer and A/B the same slots
                    // against a paused PCSX2. One-shot per process.
                    static const bool s_g186dump = (std::getenv("DC2_G186_BADPTR_DUMP") != nullptr);
                    if (s_g186dump && rdram)
                    {
                        static std::atomic<bool> s_dumped{false};
                        bool expected = false;
                        if (s_dumped.compare_exchange_strong(expected, true))
                        {
                            const uint32_t t9 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[25], 0));
                            if (FILE *fp = std::fopen("captures/g186_badpc_ram.bin", "wb"))
                            {
                                std::fwrite(rdram, 1u, 0x02000000u, fp);
                                std::fclose(fp);
                                std::fprintf(stderr, "[G186:dump] wrote captures/g186_badpc_ram.bin bad=0x%x t9=0x%x\n", pc, t9);
                            }
                            else
                            {
                                std::fprintf(stderr, "[G186:dump] FAILED to open captures/g186_badpc_ram.bin (t9=0x%x)\n", t9);
                            }
                        }
                    }
                }

                uint32_t recoveryPc = 0u;
                if (ra != 0u && runtime->hasFunction(ra))
                {
                    recoveryPc = ra;
                }

                if (recoveryPc == 0u)
                {
                    recoveryPc = selectStackRecoveryPc(rdram, ctx, runtime);
                }

                if (recoveryPc == 0u)
                {
                    recoveryPc = selectDispatchRecoveryPc(runtime);
                }

                if (recoveryPc != 0u && recoveryPc != pc)
                {
                    if (s_recoverCount < 256u)
                    {
                        std::cerr << "[dispatch:recover-pc] bad=0x" << std::hex << pc
                                  << " ra=0x" << ra
                                  << " fallback=0x" << recoveryPc
                                  << " sp=0x" << sp
                                  << std::dec << std::endl;
                    }
                    ++s_recoverCount;
                    ctx->pc = recoveryPc;
                    return;
                }
            }

            if (hasPcFunction)
            {
                s_recoverCount = 0u;
                s_loggedContext = false;
            }
            else if (pc < 0x00100000u && ra == pc && s_recoverCount < 4096u)
            {
                uint32_t recoveryPc = selectStackRecoveryPc(rdram, ctx, runtime);
                if (recoveryPc == 0u)
                {
                    recoveryPc = selectDispatchRecoveryPc(runtime);
                }
                if (recoveryPc != 0u && recoveryPc != pc)
                {
                    if (s_recoverCount < 128u)
                    {
                        std::cerr << "[dispatch:recover-low-pc] bad=0x" << std::hex << pc
                                  << " ra=0x" << ra
                                  << " fallback=0x" << recoveryPc
                                  << " sp=0x" << sp
                                  << std::dec << std::endl;
                    }
                    ++s_recoverCount;
                    ctx->pc = recoveryPc;
                    return;
                }
            }
        }

        std::ostringstream oss;
        oss << "Error: Called unimplemented function at address 0x" << std::hex << (ctx ? ctx->pc : 0u)
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " hostTid=" << std::this_thread::get_id()
            << " pcTrace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_defaultFnLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_defaultFnLogMutex);
            std::cerr << oss.str() << std::endl;
        }

        runtime->requestStop();
    };

    return defaultFunction;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    static std::unordered_map<uint32_t, int> seen;
    int &count = seen[address];
    if (count < 3)
    {
        RUNTIME_LOG("[VU0] microprogram @0x" << std::hex << address
                                             << " pc=0x" << ctx->pc
                                             << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0))
                                             << std::dec << std::endl);
    }
    ++count;

    // Seed status so dependent code sees success.
    ctx->vu0_clip_flags = 0;
    ctx->vu0_clip_flags2 = 0;
    ctx->vu0_mac_flags = 0;
    ctx->vu0_status = 0;
    ctx->vu0_q = 1.0f;
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    static const bool s_scpath = (std::getenv("DC2_TRACE_SYSCALL_PATH") != nullptr);

    // EE syscall number = the syscall instruction's encoded immediate when present,
    // else $v1 ($3), the canonical EE kernel syscall-number register. The legacy
    // $v0 ($2) fallback was REMOVED in F64.5 (upstream PR #131 alignment): $v0 is the
    // EE return-value register, never the number, so the fallback could only fire on a
    // stale-$v0 collision and silently run the WRONG syscall, masking a missing one.
    // Measured 0 hits for DC2 (probe DC2_TRACE_SYSCALL_PATH). A genuinely missing
    // syscall now lands in [Syscall TODO] (which logs both $v1 and $v0) — fix at root.
    // NOTE: do NOT also take PR #131's 0x5A/0x5B remap; DC2 calls QueryBootMode (0x5A)
    // and GetThreadTLS (0x5B) — see plans/syscall-v0-fallback-investigation.md.
    const bool fromEncoded = (encodedSyscallId != 0u);
    const uint32_t syscallId = fromEncoded ? encodedSyscallId : getRegU32(ctx, 3); // $v1

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        if (s_scpath)
        {
            const uint64_t n = (fromEncoded ? g_scpath_enc : g_scpath_v1)
                                   .fetch_add(1, std::memory_order_relaxed);
            if (n < 80u)
            {
                std::cerr << "[SCPATH:" << (fromEncoded ? "enc" : "v1") << "] num=0x"
                          << std::hex << syscallId
                          << " ra=0x" << getRegU32(ctx, 31)
                          << " pc=0x" << ctx->pc << std::dec << std::endl;
            }
        }
        return;
    }

    if (s_scpath)
    {
        const uint64_t n = g_scpath_todo.fetch_add(1, std::memory_order_relaxed);
        if (n < 200u)
        {
            std::cerr << "[SCPATH:todo]"
                      << " v1=0x" << std::hex << getRegU32(ctx, 3)
                      << " v0=0x" << getRegU32(ctx, 2)
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " pc=0x" << ctx->pc
                      << std::dec << std::endl;
        }
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

