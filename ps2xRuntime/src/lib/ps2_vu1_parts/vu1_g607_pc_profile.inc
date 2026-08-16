// =====================================================================================
// G607 — VU1 per-kick CONTROL-FLOW profile (default OFF, DC2_G607_PROF=1)
//
// WHY THIS EXISTS
// ---------------
// G606 closed the interpreter-shaped GPU VU1 executor and named the mechanism: a warp whose
// lanes sit on different opcodes pays the union of those opcodes, and removing that divergence
// (LOCKSTEP) was worth 10.3x.  The only escape is to stop interpreting and TRANSLATE the
// dominant microprogram, mapping a lane to a DATA ITEM inside one program.
//
// Before any of that can be built, the ROADMAP's Target 0(a) has to be priced, and it needs
// exactly one thing this port has never recorded: WHERE INSIDE THE MICROPROGRAM THE PAIRS GO,
// per kick.  `tools/g607_micro.py` already recovers the static CFG from the corpus (1553
// reachable pairs, 26 natural loops, 21 subroutines, 6 XGKICK sites) but static structure
// cannot say which loop carries the work, how many items one kick has to spread across lanes,
// or how often a data-dependent guard is actually taken.
//
// This profiler answers all three at once by recording, per kick, the FULL per-PC execution
// histogram.  Joined offline against the static CFG that is:
//   * loop WEIGHT      -- pairs spent inside each natural loop  (which loop to translate)
//   * loop TRIP COUNT  -- visits to each loop head, per kick    (the available LANE WIDTH)
//   * branch BIAS      -- visits to each successor of a branch  (predicate vs. real divergence)
//
// EXACTNESS / COST
// ----------------
// This is an OBSERVER: it reads `currentPc` and writes only its own counters.  It never touches
// guest state, VU registers, VU RAM, or the GS packet stream, so an armed run produces
// byte-identical output to an unarmed one.
//
// It lives inside the `DC2_VU1_LOOP_DIAG` copy of the pair loop, which the preprocessor deletes
// from the shipped body entirely (`vu1_upper_opcodes.inc` builds several copies of
// `vu1_g435_pair_loop.inc` and selects one per run).  `DC2_G607_PROF` is registered in
// `s_g295AnyProbeEnv` so arming it routes the run onto that diagnostic copy, exactly as the
// comment there requires of "any future per-cycle probe added to run()".  Nothing is added to
// the hot loop — the G593 law (a default-off diagnostic must not branch inside the loop it
// observes; duplicate it instead) is satisfied by construction.
//
// The timing of an ARMED run is meaningless and must never be quoted: the diagnostic loop copy
// is a different, slower body.  This instrument measures CONTROL FLOW, not time.
//
// FLAGS
//   DC2_G607_PROF=1            arm
//   DC2_G607_PROF_KICKS=<n>    kicks to record (default 512, matching the G606 corpus)
//   DC2_G607_PROF_SKIP=<n>     kicks to skip first (default 0; use ~1.5M to land in the
//                              `ridepod` boss battle the way DC2_G541_VU_CORPUS did)
//   DC2_G607_PROF_EVERY=<n>    record 1 kick in every n (default 1). The diagnostic loop copy is
//                              slower than the shipped one, so a fixed SKIP tuned on a normal run
//                              can fail to reach the intended scene inside the route's wall-clock
//                              budget. A stride covers the whole route instead, and every record
//                              carries its own code-image hash so the offline join can bucket by
//                              microprogram rather than trusting where the sample landed.
//   DC2_G607_PROF_PATH=<file>  output (default captures/g607_pcprof.bin)
//
// Reader: tools/g607_micro.py --prof <file>
// =====================================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

namespace
{

constexpr uint32_t kG607PairSlots = 2048u;   // 16 KiB of VU1 micro memory / 8 B per pair

static uint64_t g607EnvU64(const char *name, uint64_t dflt)
{
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0')
        return dflt;
    return std::strtoull(v, nullptr, 0);
}

struct G607Profiler
{
    bool armed = false;
    uint64_t budget = 0;      // kicks still to record
    uint64_t skip = 0;        // kicks still to skip
    uint64_t every = 1;       // record 1 kick in every `every`
    uint64_t seen = 0;        // kicks observed (skipped + recorded)
    std::FILE *out = nullptr;
    bool failed = false;

    // per-kick accumulation
    bool recording = false;
    uint32_t hist[kG607PairSlots] = {};
    std::vector<uint32_t> touched;   // slots with a nonzero count, so the reset is O(touched)
    uint64_t pairs = 0;
    uint32_t startPC = 0, itop = 0, top = 0, isResume = 0;
    uint64_t codeHash = 0;

    G607Profiler()
    {
        armed = std::getenv("DC2_G607_PROF") != nullptr;
        if (!armed)
            return;
        budget = g607EnvU64("DC2_G607_PROF_KICKS", 512ull);
        skip = g607EnvU64("DC2_G607_PROF_SKIP", 0ull);
        every = g607EnvU64("DC2_G607_PROF_EVERY", 1ull);
        if (every == 0ull)
            every = 1ull;
        touched.reserve(kG607PairSlots);
        std::fprintf(stderr, "[G607:prof] armed kicks=%llu skip=%llu every=%llu\n",
                     (unsigned long long)budget, (unsigned long long)skip,
                     (unsigned long long)every);
    }

    ~G607Profiler()
    {
        if (out != nullptr)
        {
            std::fclose(out);
            out = nullptr;
        }
    }

    bool openOut()
    {
        if (out != nullptr || failed)
            return out != nullptr;
        const char *p = std::getenv("DC2_G607_PROF_PATH");
        const char *path = (p != nullptr && *p != '\0') ? p : "captures/g607_pcprof.bin";
        out = std::fopen(path, "wb");
        if (out == nullptr)
        {
            failed = true;
            std::fprintf(stderr, "[G607:prof] FAILED to open %s\n", path);
            return false;
        }
        // File header. There is no kick count here on purpose -- the reader walks to EOF, so a
        // run that is killed mid-route still leaves a readable file.
        std::fwrite("DC2G607P", 1, 8, out);
        const uint32_t version = 1u;
        const uint32_t slots = kG607PairSlots;
        std::fwrite(&version, 4, 1, out);
        std::fwrite(&slots, 4, 1, out);
        const uint64_t pad[2] = {0ull, 0ull};
        std::fwrite(pad, 8, 2, out);
        std::fprintf(stderr, "[G607:prof] writing %s\n", path);
        return true;
    }
};

static G607Profiler s_g607Prof;

// G602's law applies to the bracket below: `run()` is called ~1431x/frame and MSVC sets a
// function's prologue from its COLDEST code, so a default-off diagnostic that inlines a 16 KiB
// hash loop and a pile of fwrite() calls into run() would tax every kick in the shipped binary
// through spills it never executes. Everything cold is therefore explicitly OUTLINED, and the
// only thing that stays inline at the call site is one bool test.
#if defined(_MSC_VER)
#define G607_NOINLINE __declspec(noinline)
#else
#define G607_NOINLINE __attribute__((noinline))
#endif

// FNV-1a over the whole 16 KiB code image, so a recorded kick can be joined to the image the
// corpus/static analysis is built from. ~512 kicks x 16 KiB is nothing for a diagnostic.
static G607_NOINLINE uint64_t g607HashCode(const uint8_t *code, uint32_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < size; ++i)
    {
        h ^= static_cast<uint64_t>(code[i]);
        h *= 1099511628211ull;
    }
    return h;
}

static G607_NOINLINE bool g607ProfBeginKickCold(const uint8_t *code, uint32_t codeSize,
                                                uint32_t startPC, uint32_t itop, uint32_t top,
                                                uint32_t isResume)
{
    G607Profiler &P = s_g607Prof;
    const uint64_t n = P.seen++;
    if (P.skip > 0)
    {
        --P.skip;
        return false;
    }
    if (P.budget == 0 || P.failed)
        return false;
    if (P.every > 1ull && (n % P.every) != 0ull)
        return false;
    if (!P.openOut())
        return false;
    P.recording = true;
    P.pairs = 0;
    P.startPC = startPC;
    P.itop = itop;
    P.top = top;
    P.isResume = isResume;
    P.codeHash = (code != nullptr) ? g607HashCode(code, codeSize) : 0ull;
    return true;
}

// Called once per run() entry, BEFORE the pair loop. Default path: one bool test, nothing else.
static inline bool g607ProfBeginKick(const uint8_t *code, uint32_t codeSize,
                                     uint32_t startPC, uint32_t itop, uint32_t top,
                                     uint32_t isResume)
{
    if (!s_g607Prof.armed)
        return false;
    return g607ProfBeginKickCold(code, codeSize, startPC, itop, top, isResume);
}

// Called once per executed pair from the DIAGNOSTIC loop copy only.
static inline void g607ProfPair(uint32_t pc)
{
    G607Profiler &P = s_g607Prof;
    const uint32_t slot = (pc >> 3) & (kG607PairSlots - 1u);
    if (P.hist[slot]++ == 0u)
        P.touched.push_back(slot);
    ++P.pairs;
}

static G607_NOINLINE void g607ProfEndKickCold()
{
    G607Profiler &P = s_g607Prof;
    P.recording = false;
    if (P.out != nullptr)
    {
        std::fwrite("G607KICK", 1, 8, P.out);
        const uint64_t idx = P.seen - 1ull;
        std::fwrite(&idx, 8, 1, P.out);
        std::fwrite(&P.startPC, 4, 1, P.out);
        std::fwrite(&P.itop, 4, 1, P.out);
        std::fwrite(&P.top, 4, 1, P.out);
        std::fwrite(&P.isResume, 4, 1, P.out);
        std::fwrite(&P.pairs, 8, 1, P.out);
        std::fwrite(&P.codeHash, 8, 1, P.out);
        const uint32_t nEntries = static_cast<uint32_t>(P.touched.size());
        const uint32_t pad = 0u;
        std::fwrite(&nEntries, 4, 1, P.out);
        std::fwrite(&pad, 4, 1, P.out);
        for (const uint32_t slot : P.touched)
        {
            const uint32_t pcByte = slot * 8u;
            const uint32_t count = P.hist[slot];
            std::fwrite(&pcByte, 4, 1, P.out);
            std::fwrite(&count, 4, 1, P.out);
        }
        if (--P.budget == 0)
        {
            std::fflush(P.out);
            std::fclose(P.out);
            P.out = nullptr;
            std::fprintf(stderr, "[G607:prof] budget exhausted after %llu kicks -- file closed\n",
                         (unsigned long long)P.seen);
        }
    }
    for (const uint32_t slot : P.touched)
        P.hist[slot] = 0u;
    P.touched.clear();
}

// Called once per run() exit. Default path: one bool test, nothing else.
static inline void g607ProfEndKick()
{
    if (!s_g607Prof.recording)
        return;
    g607ProfEndKickCold();
}

static inline bool g607ProfArmed()
{
    return s_g607Prof.armed;
}

} // namespace
