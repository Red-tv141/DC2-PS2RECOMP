// dc2_crash_reporter.inc — Automatic crash reporting system for Dark Cloud 2 recompilation.
// Included into ps2_runtime.cpp. All code is file-scope static or in anonymous namespace.
// No .h modifications (project rule: avoids 30+ hour full rebuild).

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>

#include <smmintrin.h>
#include "ps2_runtime.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace dc2_crash
{

// ============================================================================
// §1. Guest Symbol Table (Parsed from DAC.csv with O(log N) Binary Search)
// ============================================================================

struct GuestSymbol
{
    uint32_t start;
    uint32_t end;
    uint32_t size;
    std::string name;
};

class GuestSymbolTable
{
public:
    static GuestSymbolTable &instance()
    {
        static GuestSymbolTable s_table;
        return s_table;
    }

    void load(const char *csvPath = nullptr)
    {
        if (m_loaded) return;

        std::vector<std::string> candidates;
        if (csvPath && csvPath[0] != '\0')
        {
            candidates.push_back(csvPath);
        }
        candidates.push_back("DAC.csv");
        candidates.push_back("ref/DAC.csv");
        candidates.push_back("../DAC.csv");
        candidates.push_back("D:/ps2r/dc2/DAC.csv");
        candidates.push_back("D:/ps2r/dc2/ref/DAC.csv");

        std::string foundPath;
        for (const auto &path : candidates)
        {
            if (std::filesystem::exists(path))
            {
                foundPath = path;
                break;
            }
        }

        if (foundPath.empty())
        {
            return;
        }

        std::ifstream file(foundPath);
        if (!file.is_open())
        {
            return;
        }

        std::string line;
        // Skip header if present
        if (std::getline(file, line))
        {
            if (line.find("Name,Start,End,Size") == std::string::npos)
            {
                // Not a header, parse first line
                parseLine(line);
            }
        }

        while (std::getline(file, line))
        {
            parseLine(line);
        }

        std::sort(m_symbols.begin(), m_symbols.end(),
                  [](const GuestSymbol &a, const GuestSymbol &b) { return a.start < b.start; });

        m_loaded = true;
    }

    bool resolve(uint32_t pc, std::string &outName, uint32_t &outStart, uint32_t &outOffset) const
    {
        if (m_symbols.empty()) return false;

        // Binary search for symbol containing PC
        auto it = std::upper_bound(m_symbols.begin(), m_symbols.end(), pc,
                                   [](uint32_t val, const GuestSymbol &sym) {
                                       return val < sym.start;
                                   });

        if (it != m_symbols.begin())
        {
            --it;
            if (pc >= it->start && (it->end == 0 || pc < it->end || (pc - it->start) < 0x8000))
            {
                outName = it->name;
                outStart = it->start;
                outOffset = pc - it->start;
                return true;
            }
        }

        return false;
    }

    size_t count() const { return m_symbols.size(); }

private:
    GuestSymbolTable() : m_loaded(false) {}

    void parseLine(const std::string &line)
    {
        if (line.empty()) return;

        // Expected format: Name,Start,End,Size
        std::stringstream ss(line);
        std::string nameStr, startStr, endStr, sizeStr;

        if (std::getline(ss, nameStr, ',') &&
            std::getline(ss, startStr, ',') &&
            std::getline(ss, endStr, ',') &&
            std::getline(ss, sizeStr, ','))
        {
            GuestSymbol sym;
            sym.name = nameStr;
            sym.start = static_cast<uint32_t>(std::strtoul(startStr.c_str(), nullptr, 0));
            sym.end = static_cast<uint32_t>(std::strtoul(endStr.c_str(), nullptr, 0));
            sym.size = static_cast<uint32_t>(std::strtoul(sizeStr.c_str(), nullptr, 0));
            if (sym.start != 0)
            {
                m_symbols.push_back(sym);
            }
        }
    }

    std::vector<GuestSymbol> m_symbols;
    bool m_loaded;
};

// ============================================================================
// §2. Subsystem State Snapshot Hooks & Context Capture
// ============================================================================

struct GuestExecutionContext
{
    uint32_t pc = 0;
    uint32_t ra = 0;
    uint32_t sp = 0;
    uint32_t gp = 0;
    uint32_t gpr[32] = {0};
    uint32_t lo = 0;
    uint32_t hi = 0;
    uint32_t sa = 0;
    bool valid = false;
};

struct GsCrashSnapshot
{
    uint32_t fbp = 0;
    uint32_t fbw = 0;
    uint32_t psm = 0;
    uint32_t zbp = 0;
    uint32_t zpsm = 0;
    uint32_t primType = 0;
    uint32_t vertexCount = 0;
    uint64_t drawCount = 0;
    uint64_t batchNumber = 0;
    bool mtgsActive = false;
    bool valid = false;
};

struct Vu1CrashSnapshot
{
    uint32_t microPc = 0;
    uint32_t microProgStart = 0;
    uint32_t macFlags = 0;
    uint32_t clipFlags = 0;
    uint32_t statusFlags = 0;
    uint64_t kickCount = 0;
    bool mtvuActive = false;
    bool valid = false;
};

struct PerfCrashSnapshot
{
    double avgFrameMs = 0.0;
    double fps = 0.0;
    uint64_t frame = 0;
    double uploadMs = 0.0;
    double presentMs = 0.0;
    double waitFrameMs = 0.0;
    double eeCpuMs = 0.0;
    double vu1Ms = 0.0;
    double gsMs = 0.0;
    bool valid = false;
};

inline GsCrashSnapshot g_lastGsSnapshot{};
inline Vu1CrashSnapshot g_lastVu1Snapshot{};
inline PerfCrashSnapshot g_lastPerfSnapshot{};
inline std::mutex g_snapshotMutex;

inline void updateGsSnapshot(const GsCrashSnapshot &snap)
{
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    g_lastGsSnapshot = snap;
    g_lastGsSnapshot.valid = true;
}

inline void updateVu1Snapshot(const Vu1CrashSnapshot &snap)
{
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    g_lastVu1Snapshot = snap;
    g_lastVu1Snapshot.valid = true;
}

inline void updatePerfSnapshot(const PerfCrashSnapshot &snap)
{
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    g_lastPerfSnapshot = snap;
    g_lastPerfSnapshot.valid = true;
}

// ============================================================================
// §3. Crash Handler & Report Generation
// ============================================================================

static std::atomic<bool> s_crashInProgress{false};

inline const char *getExceptionName(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:         return "Access Violation (0xC0000005)";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "Array Bounds Exceeded (0xC000008C)";
    case EXCEPTION_BREAKPOINT:               return "Breakpoint (0x80000003)";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "Datatype Misalignment (0x80000002)";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "Float Denormal Operand (0xC000008D)";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "Float Divide by Zero (0xC000008E)";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "Float Inexact Result (0xC000008F)";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "Float Invalid Operation (0xC0000090)";
    case EXCEPTION_FLT_OVERFLOW:             return "Float Overflow (0xC0000091)";
    case EXCEPTION_FLT_STACK_CHECK:          return "Float Stack Check (0xC0000092)";
    case EXCEPTION_FLT_UNDERFLOW:            return "Float Underflow (0xC0000093)";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "Illegal Instruction (0xC000001D)";
    case EXCEPTION_IN_PAGE_ERROR:            return "In Page Error (0xC0000006)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "Integer Divide by Zero (0xC0000094)";
    case EXCEPTION_INT_OVERFLOW:             return "Integer Overflow (0xC0000095)";
    case EXCEPTION_INVALID_DISPOSITION:      return "Invalid Disposition (0xC0000026)";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "Noncontinuable Exception (0xC0000025)";
    case EXCEPTION_PRIV_INSTRUCTION:         return "Privileged Instruction (0xC0000096)";
    case EXCEPTION_SINGLE_STEP:              return "Single Step (0x80000004)";
    case EXCEPTION_STACK_OVERFLOW:           return "Stack Overflow (0xC00000FD)";
    case 0xE06D7363:                         return "Microsoft C++ Exception (0xE06D7363)";
    default:                                 return "Unknown Exception";
    }
}

inline void writeMiniDump(EXCEPTION_POINTERS *pExceptionPointers, const std::string &dumpPath)
{
#ifdef _WIN32
    HANDLE hFile = CreateFileA(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mdei{};
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = pExceptionPointers;
        mdei.ClientPointers = FALSE;

        const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal |
            MiniDumpWithDataSegs |
            MiniDumpWithUnloadedModules |
            MiniDumpWithProcessThreadData);

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, dumpType,
                          pExceptionPointers ? &mdei : nullptr, nullptr, nullptr);
        CloseHandle(hFile);
    }
#endif
}

inline void writeCrashReport(EXCEPTION_POINTERS *pExceptionInfo, const char *fatalMsg = nullptr)
{
    // Try to ensure symbol table is loaded
    GuestSymbolTable::instance().load();

    // Create crashes directory
    try
    {
        std::filesystem::create_directories("crashes");
    }
    catch (...) {}

    // Generate crash timestamp
    const auto now = std::chrono::system_clock::now();
    const std::time_t timeNow = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
#ifdef _WIN32
    localtime_s(&tmBuf, &timeNow);
#else
    localtime_r(&timeNow, &tmBuf);
#endif

    char filenameBuf[128];
    std::snprintf(filenameBuf, sizeof(filenameBuf), "crashes/crash_%04d-%02d-%02d_%02d-%02d-%02d.txt",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
    char dumpnameBuf[128];
    std::snprintf(dumpnameBuf, sizeof(dumpnameBuf), "crashes/crash_%04d-%02d-%02d_%02d-%02d-%02d.dmp",
                  tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                  tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);

    std::string reportPath = filenameBuf;
    std::string dumpPath = dumpnameBuf;

    // Write Windows MiniDump
    if (pExceptionInfo)
    {
        writeMiniDump(pExceptionInfo, dumpPath);
    }

    FILE *fp = std::fopen(reportPath.c_str(), "w");
    if (!fp)
    {
        fp = stderr;
    }

    const uint64_t frame = dc2_log::getFrameNumber();
    const char *threadName = dc2_log::getCurrentThreadName();

    // ------------------------------------------------------------------------
    // Section 1: Header / Crash Summary
    // ------------------------------------------------------------------------
    std::fprintf(fp, "========== DARK CLOUD 2 RECOMP CRASH REPORT ==========\n\n");
    std::fprintf(fp, "Timestamp:        %04d-%02d-%02d %02d:%02d:%02d\n",
                 tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
                 tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
    std::fprintf(fp, "Executable:       dc2_runner.exe (Dark Cloud 2 SCUS_972.13)\n");
    std::fprintf(fp, "Host Frame:       %llu\n", (unsigned long long)frame);
#ifdef _WIN32
    std::fprintf(fp, "Crashing Thread:  %s (TID %lu, 0x%lX)\n",
                 threadName ? threadName : "Unknown",
                 (unsigned long)GetCurrentThreadId(),
                 (unsigned long)GetCurrentThreadId());
#endif

    const EXCEPTION_RECORD *rec = pExceptionInfo ? pExceptionInfo->ExceptionRecord : nullptr;
    const CONTEXT *ctx = pExceptionInfo ? pExceptionInfo->ContextRecord : nullptr;

    if (fatalMsg)
    {
        std::fprintf(fp, "Termination Type: FATAL ERROR / ASSERTION FAILURE\n");
        std::fprintf(fp, "Fatal Message:    %s\n", fatalMsg);
    }
    else if (rec)
    {
        std::fprintf(fp, "Exception Type:   %s\n", getExceptionName(rec->ExceptionCode));
        std::fprintf(fp, "Exception Code:   0x%08lX\n", (unsigned long)rec->ExceptionCode);
        std::fprintf(fp, "Faulting Address: 0x%p\n", rec->ExceptionAddress);

        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        {
            const ULONG_PTR opType = rec->ExceptionInformation[0];
            const ULONG_PTR targetAddr = rec->ExceptionInformation[1];
            const char *opStr = (opType == 0) ? "READ" : (opType == 1 ? "WRITE" : "EXECUTE");
            std::fprintf(fp, "Access Violation: %s at target memory address 0x%016llX\n",
                         opStr, (unsigned long long)targetAddr);
        }
    }
    std::fprintf(fp, "\n========================================================\n\n");

    // ------------------------------------------------------------------------
    // Section 2: Host Native CPU Registers (x86-64)
    // ------------------------------------------------------------------------
#ifdef _WIN32
    if (ctx)
    {
        std::fprintf(fp, "---------- HOST NATIVE REGISTERS (x86-64) ----------\n\n");
        std::fprintf(fp, "  RIP: 0x%016llX   RSP: 0x%016llX   RBP: 0x%016llX\n",
                     (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rbp);
        std::fprintf(fp, "  RAX: 0x%016llX   RBX: 0x%016llX   RCX: 0x%016llX\n",
                     (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx, (unsigned long long)ctx->Rcx);
        std::fprintf(fp, "  RDX: 0x%016llX   RSI: 0x%016llX   RDI: 0x%016llX\n",
                     (unsigned long long)ctx->Rdx, (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi);
        std::fprintf(fp, "  R8:  0x%016llX   R9:  0x%016llX   R10: 0x%016llX\n",
                     (unsigned long long)ctx->R8,  (unsigned long long)ctx->R9,  (unsigned long long)ctx->R10);
        std::fprintf(fp, "  R11: 0x%016llX   R12: 0x%016llX   R13: 0x%016llX\n",
                     (unsigned long long)ctx->R11, (unsigned long long)ctx->R12, (unsigned long long)ctx->R13);
        std::fprintf(fp, "  R14: 0x%016llX   R15: 0x%016llX   EFL: 0x%08lX\n",
                     (unsigned long long)ctx->R14, (unsigned long long)ctx->R15, (unsigned long)ctx->EFlags);
        std::fprintf(fp, "\n");
    }
#endif

    // ------------------------------------------------------------------------
    // Section 3: Host Native Call Stack
    // ------------------------------------------------------------------------
#ifdef _WIN32
    std::fprintf(fp, "---------- HOST NATIVE CALL STACK ----------\n\n");
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    CONTEXT walkCtx{};
    if (ctx)
    {
        walkCtx = *ctx;
    }
    else
    {
        RtlCaptureContext(&walkCtx);
    }

    STACKFRAME64 frame64{};
    frame64.AddrPC.Offset = walkCtx.Rip;
    frame64.AddrPC.Mode = AddrModeFlat;
    frame64.AddrFrame.Offset = walkCtx.Rbp;
    frame64.AddrFrame.Mode = AddrModeFlat;
    frame64.AddrStack.Offset = walkCtx.Rsp;
    frame64.AddrStack.Mode = AddrModeFlat;

    int frameIndex = 0;
    while (StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(),
                       &frame64, &walkCtx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
    {
        if (frame64.AddrPC.Offset == 0) break;

        char symBuf[sizeof(SYMBOL_INFO) + 512] = {0};
        SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;
        const bool haveSym = SymFromAddr(GetCurrentProcess(), frame64.AddrPC.Offset, &disp, sym) != FALSE;

        IMAGEHLP_LINE64 line64{};
        line64.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisp = 0;
        const bool haveLine = SymGetLineFromAddr64(GetCurrentProcess(), frame64.AddrPC.Offset, &lineDisp, &line64) != FALSE;

        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(frame64.AddrPC.Offset), &mod);
        char modName[MAX_PATH] = {0};
        if (mod) GetModuleFileNameA(mod, modName, sizeof(modName) - 1);
        const char *modLeaf = modName[0] ? (std::strrchr(modName, '\\') ? std::strrchr(modName, '\\') + 1 : modName) : "unknown";

        if (haveSym && haveLine)
        {
            std::fprintf(fp, "  #%02d 0x%016llX [%s] %s + 0x%llX (%s:%lu)\n",
                         frameIndex++, (unsigned long long)frame64.AddrPC.Offset, modLeaf,
                         sym->Name, (unsigned long long)disp, line64.FileName, (unsigned long)line64.LineNumber);
        }
        else if (haveSym)
        {
            std::fprintf(fp, "  #%02d 0x%016llX [%s] %s + 0x%llX\n",
                         frameIndex++, (unsigned long long)frame64.AddrPC.Offset, modLeaf,
                         sym->Name, (unsigned long long)disp);
        }
        else
        {
            std::fprintf(fp, "  #%02d 0x%016llX [%s] (No symbol)\n",
                         frameIndex++, (unsigned long long)frame64.AddrPC.Offset, modLeaf);
        }

        if (frameIndex >= 64) break;
    }
    std::fprintf(fp, "\n");
#endif

    // ------------------------------------------------------------------------
    // Section 4: Guest PS2 Execution Context & Function Symbol Mapping
    // ------------------------------------------------------------------------
    std::fprintf(fp, "---------- GUEST PS2 EXECUTION CONTEXT ----------\n\n");
    const uint32_t guestPc = g_activeGuestPc.load(std::memory_order_relaxed);
    const uint32_t guestRa = g_activeGuestRa.load(std::memory_order_relaxed);
    const uint32_t guestSp = g_activeGuestSp.load(std::memory_order_relaxed);
    const uint32_t guestGp = g_activeGuestGp.load(std::memory_order_relaxed);

    std::string pcSymName = "Unknown / Dynamic";
    uint32_t pcStart = 0, pcOff = 0;
    const bool havePcSym = GuestSymbolTable::instance().resolve(guestPc, pcSymName, pcStart, pcOff);

    std::string raSymName = "Unknown";
    uint32_t raStart = 0, raOff = 0;
    const bool haveRaSym = GuestSymbolTable::instance().resolve(guestRa, raSymName, raStart, raOff);

    std::fprintf(fp, "  Guest PC:        0x%08X", guestPc);
    if (havePcSym)
    {
        std::fprintf(fp, "  -->  %s (Start: 0x%08X + 0x%X)\n", pcSymName.c_str(), pcStart, pcOff);
    }
    else
    {
        std::fprintf(fp, "  -->  (Unmapped Guest Address)\n");
    }

    std::fprintf(fp, "  Guest RA ($31):  0x%08X", guestRa);
    if (haveRaSym)
    {
        std::fprintf(fp, "  -->  %s (Start: 0x%08X + 0x%X)\n", raSymName.c_str(), raStart, raOff);
    }
    else
    {
        std::fprintf(fp, "\n");
    }

    std::fprintf(fp, "  Guest SP ($29):  0x%08X\n", guestSp);
    std::fprintf(fp, "  Guest GP ($28):  0x%08X\n", guestGp);
    std::fprintf(fp, "\n");

    // ------------------------------------------------------------------------
    // Section 5: PS2 EE General Purpose Registers ($0 - $31)
    // ------------------------------------------------------------------------
    if (g_activeR5900Context)
    {
        std::fprintf(fp, "---------- PS2 EE REGISTER SNAPSHOT ----------\n\n");
        static const char *kGprNames[32] = {
            "$zero", "$at", "$v0", "$v1", "$a0", "$a1", "$a2", "$a3",
            "$t0",   "$t1", "$t2", "$t3", "$t4", "$t5", "$t6", "$t7",
            "$s0",   "$s1", "$s2", "$s3", "$s4", "$s5", "$s6", "$s7",
            "$t8",   "$t9", "$k0", "$k1", "$gp", "$sp", "$fp", "$ra"
        };

        for (int i = 0; i < 32; i += 4)
        {
            std::fprintf(fp, "  %-5s: 0x%08X   %-5s: 0x%08X   %-5s: 0x%08X   %-5s: 0x%08X\n",
                         kGprNames[i],   static_cast<uint32_t>(_mm_extract_epi32(g_activeR5900Context->r[i], 0)),
                         kGprNames[i+1], static_cast<uint32_t>(_mm_extract_epi32(g_activeR5900Context->r[i+1], 0)),
                         kGprNames[i+2], static_cast<uint32_t>(_mm_extract_epi32(g_activeR5900Context->r[i+2], 0)),
                         kGprNames[i+3], static_cast<uint32_t>(_mm_extract_epi32(g_activeR5900Context->r[i+3], 0)));
        }
        std::fprintf(fp, "  LO:    0x%016llX   HI:    0x%016llX   SA:    0x%08X\n",
                     (unsigned long long)g_activeR5900Context->lo,
                     (unsigned long long)g_activeR5900Context->hi,
                     g_activeR5900Context->sa);
        std::fprintf(fp, "\n");
    }

    // ------------------------------------------------------------------------
    // Section 6: Subsystem States (GS, VU1, MTVU, MTGS)
    // ------------------------------------------------------------------------
    std::fprintf(fp, "---------- SUBSYSTEM STATUS ----------\n\n");
    {
        std::lock_guard<std::mutex> lock(g_snapshotMutex);
        if (g_lastGsSnapshot.valid)
        {
            std::fprintf(fp, "  [GS / Graphics]\n");
            std::fprintf(fp, "    MTGS Worker:      %s\n", g_lastGsSnapshot.mtgsActive ? "Active" : "Disabled");
            std::fprintf(fp, "    Color FBP:        0x%03X (FBW: %u, PSM: 0x%02X)\n",
                         g_lastGsSnapshot.fbp, g_lastGsSnapshot.fbw, g_lastGsSnapshot.psm);
            std::fprintf(fp, "    Depth ZBP:        0x%03X (ZPSM: 0x%02X)\n",
                         g_lastGsSnapshot.zbp, g_lastGsSnapshot.zpsm);
            std::fprintf(fp, "    Active Primitive: Type %u (Vertices: %u)\n",
                         g_lastGsSnapshot.primType, g_lastGsSnapshot.vertexCount);
            std::fprintf(fp, "    Execution Batch:  #%llu (Draw #%llu)\n\n",
                         (unsigned long long)g_lastGsSnapshot.batchNumber,
                         (unsigned long long)g_lastGsSnapshot.drawCount);
        }

        if (g_lastVu1Snapshot.valid)
        {
            std::fprintf(fp, "  [VU1 / Vector Unit]\n");
            std::fprintf(fp, "    MTVU Worker:      %s\n", g_lastVu1Snapshot.mtvuActive ? "Active" : "Disabled");
            std::fprintf(fp, "    Micro PC:         0x%04X (Program Start: 0x%04X)\n",
                         g_lastVu1Snapshot.microPc, g_lastVu1Snapshot.microProgStart);
            std::fprintf(fp, "    Flags:            MAC=0x%X  CLIP=0x%X  STATUS=0x%X\n",
                         g_lastVu1Snapshot.macFlags, g_lastVu1Snapshot.clipFlags, g_lastVu1Snapshot.statusFlags);
            std::fprintf(fp, "    Total Kicks:      %llu\n\n", (unsigned long long)g_lastVu1Snapshot.kickCount);
        }

        if (g_lastPerfSnapshot.valid)
        {
            std::fprintf(fp, "  [Recent Performance]\n");
            std::fprintf(fp, "    Average Frame:    %.2f ms (%.2f FPS)\n",
                         g_lastPerfSnapshot.avgFrameMs, g_lastPerfSnapshot.fps);
            std::fprintf(fp, "    Breakdown:        Upload: %.2f ms | Present: %.2f ms | WaitFrame: %.2f ms\n",
                         g_lastPerfSnapshot.uploadMs, g_lastPerfSnapshot.presentMs, g_lastPerfSnapshot.waitFrameMs);
            std::fprintf(fp, "    Worker CPU:       EE: %.2f ms | VU1: %.2f ms | GS: %.2f ms\n\n",
                         g_lastPerfSnapshot.eeCpuMs, g_lastPerfSnapshot.vu1Ms, g_lastPerfSnapshot.gsMs);
        }
    }

    // ------------------------------------------------------------------------
    // Section 7: Active Environment Flags
    // ------------------------------------------------------------------------
    std::fprintf(fp, "---------- ACTIVE CONFIGURATION & ENV FLAGS ----------\n\n");
    static const char *kFlagsToCheck[] = {
        "DC2_PATCH_60FPS", "DC2_DEBUG_MENU", "DC2_NO_PRESENT_SYNC",
        "DC2_PROFILE", "DC2_PROFILE_OVERLAY", "DC2_PROFILE_CSV", "DC2_PROFILE_FUNCTIONS",
        "DC2_FRAME_DUMP", "DC2_G26X_NO_NATIVE", "DC2_G605_NO_TRI_SPAN",
        "DC2_G608_NO_TRI_FST", "DC2_G609_NO_TRI_SCAN", "DC2_G610_NO_JIT",
        "DC2_G612_NO_REGION", "DC2_G614_NO_SPRITE_FAST", "DC2_G615_NO_PRIVZ_SPAN",
        "DC2_G617_NO_BLEND_FILL", "DC2_G380_NO_EZMIDI", "DC2_G385_NO_GAME_AUDIO",
        "DC2_PAD_INPUT", "DC2_DATA_DIR", "DC2_ISO_PATH", "DC2_LOG_LEVEL"
    };

    for (const char *flagName : kFlagsToCheck)
    {
        if (const char *val = std::getenv(flagName))
        {
            std::fprintf(fp, "  %s = %s\n", flagName, val);
        }
    }
    std::fprintf(fp, "\n");

    // ------------------------------------------------------------------------
    // Section 8: In-Memory Ring Buffer (Recent Log History)
    // ------------------------------------------------------------------------
    std::fprintf(fp, "---------- RECENT LOG HISTORY (LAST 512 EVENTS) ----------\n\n");
    std::vector<dc2_log::LogRecord> logs;
    dc2_log::Logger::instance().getRingBuffer().copyTo(logs);

    if (logs.empty())
    {
        // Try unsynchronized copy if lock failed or empty
        dc2_log::LogRecord tempLogs[dc2_log::kRingBufferSize];
        const size_t got = dc2_log::Logger::instance().getRingBuffer().tryCopyUnsynchronized(tempLogs, dc2_log::kRingBufferSize);
        for (size_t i = 0; i < got; ++i)
        {
            logs.push_back(tempLogs[i]);
        }
    }

    for (const auto &recEntry : logs)
    {
        const uint32_t sec = static_cast<uint32_t>(recEntry.timestampUs / 1000000);
        const uint32_t ms = static_cast<uint32_t>((recEntry.timestampUs % 1000000) / 1000);
        std::fprintf(fp, "  [%02u:%02u:%02u.%03u][Frame %llu][%s][%s][%s] %s\n",
                     (sec / 3600) % 24, (sec / 60) % 60, sec % 60, ms,
                     (unsigned long long)recEntry.frame,
                     recEntry.threadName,
                     dc2_log::levelToString(recEntry.level),
                     dc2_log::categoryToString(recEntry.category),
                     recEntry.message);
    }

    std::fprintf(fp, "\n========== END OF CRASH REPORT ==========\n");

    if (fp != stderr)
    {
        std::fflush(fp);
        std::fclose(fp);
    }

    // Also write concise notification to stderr
    std::fprintf(stderr, "\n********************************************************\n");
    std::fprintf(stderr, "  DARK CLOUD 2 RECOMP CRASH DETECTED\n");
    std::fprintf(stderr, "  Crash report written to: %s\n", reportPath.c_str());
    if (pExceptionInfo)
    {
        std::fprintf(stderr, "  MiniDump written to:     %s\n", dumpPath.c_str());
    }
    std::fprintf(stderr, "********************************************************\n\n");
    std::fflush(stderr);
}

// Windows Unhandled Exception Filter
#ifdef _WIN32
static LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *pExceptionInfo)
{
    bool expected = false;
    if (!s_crashInProgress.compare_exchange_strong(expected, true))
    {
        // Recursive crash detected!
        std::fprintf(stderr, "[CRASH] Recursive crash inside crash reporter. Immediate termination.\n");
        return EXCEPTION_EXECUTE_HANDLER;
    }

    dc2_log::flush();
    writeCrashReport(pExceptionInfo, nullptr);

    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

inline void installCrashReporter()
{
    // Load guest symbol table in background
    GuestSymbolTable::instance().load();

#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif

    std::set_terminate([]()
    {
        bool expected = false;
        if (s_crashInProgress.compare_exchange_strong(expected, true))
        {
            dc2_log::flush();
            writeCrashReport(nullptr, "std::terminate invoked (unhandled C++ exception)");
        }
        std::abort();
    });
}

// ============================================================================
// §4. Assertions & Fatal Errors
// ============================================================================

inline void handleAssertionFailure(const char *expr, const char *file, int line, const char *msg = nullptr)
{
    char buf[512];
    if (msg && msg[0] != '\0')
    {
        std::snprintf(buf, sizeof(buf), "Assertion failed: '%s' (%s) at %s:%d", expr, msg, file, line);
    }
    else
    {
        std::snprintf(buf, sizeof(buf), "Assertion failed: '%s' at %s:%d", expr, file, line);
    }

    LOG_FATAL(Crash, "%s", buf);

    bool expected = false;
    if (s_crashInProgress.compare_exchange_strong(expected, true))
    {
        writeCrashReport(nullptr, buf);
    }

    dc2_log::flush();
    std::abort();
}

inline void fatalError(const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    LOG_FATAL(Crash, "FATAL: %s", buf);

    bool expected = false;
    if (s_crashInProgress.compare_exchange_strong(expected, true))
    {
        writeCrashReport(nullptr, buf);
    }

    dc2_log::flush();
    std::exit(1);
}

// ============================================================================
// §5. Controlled Crash Testing Trigger
// ============================================================================

inline void checkTestCrashTrigger()
{
    const char *testEnv = std::getenv("DC2_TEST_CRASH");
    if (!testEnv || testEnv[0] == '\0' || testEnv[0] == '0')
    {
        return;
    }

    const int mode = std::atoi(testEnv);
    LOG_WARNING(Crash, "DC2_TEST_CRASH=%d detected — triggering deliberate test crash...", mode);
    dc2_log::flush();

    switch (mode)
    {
    case 1: // Access violation
    {
        volatile int *badPtr = reinterpret_cast<volatile int *>(0x42);
        *badPtr = 0xDEADBEEF;
        break;
    }
    case 2: // Assertion failure
    {
        handleAssertionFailure("test_condition == true", __FILE__, __LINE__, "Deliberate test assertion failure");
        break;
    }
    case 3: // Fatal error
    {
        fatalError("Deliberate fatal error triggered via DC2_TEST_CRASH=3");
        break;
    }
    default:
        LOG_WARNING(Crash, "Unknown DC2_TEST_CRASH mode %d", mode);
        break;
    }
}

} // namespace dc2_crash

// Centralized Assert Macros
#define DC2_ASSERT(condition) do { \
    if (!(condition)) { \
        dc2_crash::handleAssertionFailure(#condition, __FILE__, __LINE__); \
    } \
} while(0)

#define DC2_ASSERT_MSG(condition, msg) do { \
    if (!(condition)) { \
        dc2_crash::handleAssertionFailure(#condition, __FILE__, __LINE__, msg); \
    } \
} while(0)

#define DC2_FATAL(...) dc2_crash::fatalError(__VA_ARGS__)
