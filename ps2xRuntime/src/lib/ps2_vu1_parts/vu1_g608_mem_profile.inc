// =====================================================================================
// G608 — VU1 PER-PROGRAM-POINT VU-RAM FOOTPRINT (default OFF, DC2_G608_MEMPROF=1)
//
// WHY THIS EXISTS — it is the ONE precondition G607 parked the surviving GPU-VU idea behind.
// -----------------------------------------------------------------------------------
// G606 closed the interpreted GPU-VU shape on divergence and G607 closed the TRANSLATED shape on
// SYNCHRONIZATION: 44.1% of kicks read state the immediately preceding kick wrote, so kicks cannot
// be merged, so a GPU backend pays 1431 serialized dispatches per `ridepod` frame at a measured
// GPU-resident chained floor of 5.17 us = 7.40 ms/f — against a re-measured usable headroom of
// 3.75 ms/f.
//
// Exactly one idea survived that: batch by PROGRAM POINT rather than by KICK. Gather every pending
// invocation of hot loop L across many kicks into ONE wide dispatch. That would raise the width
// above G607's p50 = 5 AND collapse the dispatch count at the same time. G607 wrote its precondition
// down and refused to build anything until it was tested:
//
//   > prove that the *inputs of the hot loops themselves* do not intersect that carried set —
//   > G606's census was over WHOLE-KICK footprints and cannot answer it. If the loops do touch the
//   > carried set, Goal 3 is closed outright.
//
// G606's carried conflict set is CONCENTRATED, not spread: 121 quadwords at 21, 22, 30 and a run at
// 781..808 (0x30D0..0x3230), consistent with a setup kick publishing derived constants that later
// draw kicks consume. If the hot loops read only from streams their OWN kick produced, those loops
// are batchable across kicks even though whole kicks are not.
//
// WHAT THIS RECORDS
// -----------------
// Per sampled kick, per PC BUCKET (the five hot loops of G607 §2 plus a catch-all), a 1024-bit
// bitmap of VU data-RAM QUADWORDS READ and a second of quadwords WRITTEN, plus the bucket's pair
// count. Offline (`tools/g608_memdep.py`) that answers the precondition three ways:
//   1. does R(L) intersect G606's carried set {21,22,30} u [781,808]?
//   2. re-derive the carried set from THIS corpus (qw read by kick k, written by some kick j<k and
//      not rewritten inside k first) instead of trusting G606's — the honest control;
//   3. per loop, split R(L) into qw the SAME kick wrote before the loop ran (private, batchable)
//      vs qw no earlier bucket of this kick wrote (imported, i.e. cross-kick carried).
//
// EXACTNESS OF THE FOOTPRINT
// --------------------------
// Every address here is computed by the SAME expression the shipped body uses — the field
// accessors are `LIS`/`LIT`/`IMM11`/`S2_FUNC` and the address forms are copied from
// `G481_LOWER_BODY_LQ/LQI/SQI` and `vu1_dispatch_and_sync.inc`'s ISW/ILWR/ISWR cases:
//   loads  LQ/ILW  (VI[is] + imm11)*16   LQI/ILWR  VI[is]*16   LQD  (VI[is]-1)*16
//   stores SQ      (VI[it] + imm11)*16   SQI       VI[it]*16   SQD  (VI[it]-1)*16
//          ISW     (VI[is] + imm11)*16   ISWR      VI[is]*16
// (integer stores take the ADDRESS from `is` and the VALUE from `it`; quadword stores take the
// address from `it`.) The hook runs in G295 diagnostic region B, which is post-fetch and
// PRE-EXECUTE, so `m_state.vi` still holds the pre-increment value LQI/SQI actually address with.
//
// COST / SAFETY — it is an OBSERVER. It reads `lower`, `currentPc` and `m_state.vi`, and writes
// only its own bitmaps, so an armed run produces byte-identical guest output. It lives only inside
// the `DC2_VU1_LOOP_DIAG` copy of the pair loop and `DC2_G608_MEMPROF` is registered in
// `s_g295AnyProbeEnv`, so arming it routes the run onto that copy and the shipped body never grows
// a branch for it (the G593 law — duplicate the loop, do not branch inside it).
//
// ⛔ NEVER QUOTE FRAME TIME FROM AN ARMED RUN. Same reason as DC2_G607_PROF: the diagnostic loop
// copy is a different, slower body. This instrument measures DATA FLOW, not time.
//
// FLAGS
//   DC2_G608_MEMPROF=1          arm
//   DC2_G608_MEM_KICKS=<n>      kicks to record (default 1024)
//   DC2_G608_MEM_EVERY=<n>      record 1 kick in every n (default 1500, i.e. G607's stride, so the
//                               sample spans the whole route rather than one scene)
//   DC2_G608_MEM_PATH=<file>    output (default captures/g608_memdep.bin)
//   DC2_G608_MEM_RANGES=lo-hi,... PC ranges in HEX BYTES, overriding the five G607 loops
//
// Reader: tools/g608_memdep.py
// =====================================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

namespace
{

constexpr uint32_t kG608MemWords = 16u;   // 1024 quadwords / 64 bits
constexpr uint32_t kG608MaxBuckets = 16u; // 15 ranges + the catch-all

struct G608MemBucket
{
    uint64_t readMap[kG608MemWords] = {};
    uint64_t writeMap[kG608MemWords] = {};
    uint64_t pairs = 0;
    uint64_t reads = 0;
    uint64_t writes = 0;
};

struct G608MemProfiler
{
    bool armed = false;
    uint64_t budget = 0;
    uint64_t every = 1500;
    uint64_t seen = 0;
    std::FILE *out = nullptr;
    bool failed = false;

    bool recording = false;
    uint32_t nRanges = 0;                 // buckets 0..nRanges-1 are ranges; nRanges is "other"
    uint32_t rangeLo[kG608MaxBuckets] = {};
    uint32_t rangeHi[kG608MaxBuckets] = {};
    G608MemBucket bucket[kG608MaxBuckets + 1];
    uint64_t pairs = 0;
    uint32_t startPC = 0;
    uint64_t codeHash = 0;

    // G607's §2 table, ranked by exclusive pairs. The first five are 51.65% of ALL executed pairs
    // and contain ZERO conditional branches in the body — they are the only loops a program-point
    // batcher would ever gather, so they are the only loops whose inputs this has to classify.
    void defaultRanges()
    {
        static const uint32_t lo[5] = {0x1850u, 0x0900u, 0x0c60u, 0x1be8u, 0x0e00u};
        static const uint32_t hi[5] = {0x19e0u, 0x0a10u, 0x0c90u, 0x1c20u, 0x0e40u};
        for (uint32_t i = 0; i < 5u; ++i)
        {
            rangeLo[i] = lo[i];
            rangeHi[i] = hi[i];
        }
        nRanges = 5u;
    }

    void parseRanges(const char *spec)
    {
        nRanges = 0;
        const char *p = spec;
        while (*p != '\0' && nRanges < kG608MaxBuckets)
        {
            char *end = nullptr;
            const unsigned long a = std::strtoul(p, &end, 16);
            if (end == p)
                break;
            if (*end != '-')
                break;
            p = end + 1;
            const unsigned long b = std::strtoul(p, &end, 16);
            if (end == p)
                break;
            rangeLo[nRanges] = static_cast<uint32_t>(a);
            rangeHi[nRanges] = static_cast<uint32_t>(b);
            ++nRanges;
            p = end;
            while (*p == ',' || *p == ' ')
                ++p;
        }
        if (nRanges == 0)
            defaultRanges();
    }

    G608MemProfiler()
    {
        armed = std::getenv("DC2_G608_MEMPROF") != nullptr;
        if (!armed)
            return;
        const char *k = std::getenv("DC2_G608_MEM_KICKS");
        budget = (k != nullptr && *k != '\0') ? std::strtoull(k, nullptr, 0) : 1024ull;
        const char *e = std::getenv("DC2_G608_MEM_EVERY");
        every = (e != nullptr && *e != '\0') ? std::strtoull(e, nullptr, 0) : 1500ull;
        if (every == 0ull)
            every = 1ull;
        const char *r = std::getenv("DC2_G608_MEM_RANGES");
        if (r != nullptr && *r != '\0')
            parseRanges(r);
        else
            defaultRanges();
        std::fprintf(stderr, "[G608:mem] armed kicks=%llu every=%llu ranges=%u\n",
                     (unsigned long long)budget, (unsigned long long)every, nRanges);
        for (uint32_t i = 0; i < nRanges; ++i)
            std::fprintf(stderr, "[G608:mem]   L%u = 0x%04x..0x%04x\n", i, rangeLo[i], rangeHi[i]);
    }

    ~G608MemProfiler()
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
        const char *p = std::getenv("DC2_G608_MEM_PATH");
        const char *path = (p != nullptr && *p != '\0') ? p : "captures/g608_memdep.bin";
        out = std::fopen(path, "wb");
        if (out == nullptr)
        {
            failed = true;
            std::fprintf(stderr, "[G608:mem] FAILED to open %s\n", path);
            return false;
        }
        std::fwrite("DC2G608M", 1, 8, out);
        const uint32_t version = 1u;
        const uint32_t words = kG608MemWords;
        const uint32_t buckets = nRanges + 1u;
        const uint32_t pad0 = 0u;
        std::fwrite(&version, 4, 1, out);
        std::fwrite(&words, 4, 1, out);
        std::fwrite(&buckets, 4, 1, out);
        std::fwrite(&pad0, 4, 1, out);
        for (uint32_t i = 0; i < nRanges; ++i)
        {
            std::fwrite(&rangeLo[i], 4, 1, out);
            std::fwrite(&rangeHi[i], 4, 1, out);
        }
        std::fprintf(stderr, "[G608:mem] writing %s\n", path);
        return true;
    }
};

static G608MemProfiler s_g608Mem;

#if defined(_MSC_VER)
#define G608_NOINLINE __declspec(noinline)
#else
#define G608_NOINLINE __attribute__((noinline))
#endif

static G608_NOINLINE uint64_t g608MemHashCode(const uint8_t *code, uint32_t size)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < size; ++i)
    {
        h ^= static_cast<uint64_t>(code[i]);
        h *= 1099511628211ull;
    }
    return h;
}

static G608_NOINLINE bool g608MemBeginKickCold(const uint8_t *code, uint32_t codeSize,
                                               uint32_t startPC)
{
    G608MemProfiler &P = s_g608Mem;
    const uint64_t n = P.seen++;
    if (P.budget == 0 || P.failed)
        return false;
    if (P.every > 1ull && (n % P.every) != 0ull)
        return false;
    if (!P.openOut())
        return false;
    P.recording = true;
    P.pairs = 0;
    P.startPC = startPC;
    P.codeHash = (code != nullptr) ? g608MemHashCode(code, codeSize) : 0ull;
    for (uint32_t b = 0; b <= P.nRanges; ++b)
        P.bucket[b] = G608MemBucket{};
    return true;
}

static inline bool g608MemBeginKick(const uint8_t *code, uint32_t codeSize, uint32_t startPC)
{
    if (!s_g608Mem.armed)
        return false;
    return g608MemBeginKickCold(code, codeSize, startPC);
}

// The per-pair observer. Called from the DIAGNOSTIC loop copy only, post-fetch / pre-execute.
static inline void g608MemProfPair(uint32_t pc, uint32_t lower, const int32_t *vi)
{
    G608MemProfiler &P = s_g608Mem;
    uint32_t b = P.nRanges; // the catch-all
    for (uint32_t i = 0; i < P.nRanges; ++i)
        if (pc >= P.rangeLo[i] && pc <= P.rangeHi[i])
        {
            b = i;
            break;
        }
    G608MemBucket &B = P.bucket[b];
    ++B.pairs;
    ++P.pairs;

    const uint32_t opHi = (lower >> 25u) & 0x7Fu;
    // The shipped bodies index `vi` with the raw 5-bit field; VU1State.vi is int32_t[16] and the
    // microprogram only ever names VI0..VI15, so the mask is defensive only — it can differ from
    // the shipped address exclusively where the shipped body would already be reading out of
    // bounds, which is not a case this observer should follow.
    const uint8_t is = static_cast<uint8_t>(LIS(lower) & 0xFu);
    const uint8_t it = static_cast<uint8_t>(LIT(lower) & 0xFu);
    uint32_t addr = 0u;
    int mode = 0; // 0 none, 1 read, 2 write
    switch (opHi)
    {
    case 0x00u: // LQ
        addr = static_cast<uint32_t>(static_cast<int32_t>(vi[is] + IMM11(lower))) * 16u;
        mode = 1;
        break;
    case 0x04u: // ILW
        addr = static_cast<uint32_t>(static_cast<int32_t>(vi[is] + IMM11(lower))) * 16u;
        mode = 1;
        break;
    case 0x01u: // SQ
        addr = static_cast<uint32_t>(static_cast<int32_t>(vi[it] + IMM11(lower))) * 16u;
        mode = 2;
        break;
    case 0x05u: // ISW — address from `is`, value from `it`
        addr = static_cast<uint32_t>(static_cast<int32_t>(vi[is] + IMM11(lower))) * 16u;
        mode = 2;
        break;
    case 0x40u:
        switch (S2_FUNC(lower))
        {
        case 0x34u: // LQI
            addr = static_cast<uint32_t>(static_cast<uint16_t>(vi[is])) * 16u;
            mode = 1;
            break;
        case 0x36u: // LQD (pre-decrement)
            addr = static_cast<uint32_t>(static_cast<uint16_t>(vi[is] - 1)) * 16u;
            mode = 1;
            break;
        case 0x3Eu: // ILWR
            addr = static_cast<uint32_t>(static_cast<uint16_t>(vi[is])) * 16u;
            mode = 1;
            break;
        case 0x35u: // SQI
            addr = static_cast<uint32_t>(static_cast<uint16_t>(vi[it])) * 16u;
            mode = 2;
            break;
        case 0x37u: // SQD (pre-decrement)
            addr = static_cast<uint32_t>(static_cast<uint16_t>(vi[it] - 1)) * 16u;
            mode = 2;
            break;
        case 0x3Fu: // ISWR — address from `is`
            addr = static_cast<uint32_t>(static_cast<uint16_t>(vi[is])) * 16u;
            mode = 2;
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
    if (mode == 0)
        return;
    addr &= (PS2_VU1_DATA_SIZE - 1u);
    const uint32_t qw = addr >> 4u; // 0..1023
    const uint32_t w = qw >> 6u;
    const uint64_t bit = 1ull << (qw & 63u);
    if (mode == 1)
    {
        B.readMap[w] |= bit;
        ++B.reads;
    }
    else
    {
        B.writeMap[w] |= bit;
        ++B.writes;
    }
}

static G608_NOINLINE void g608MemEndKickCold()
{
    G608MemProfiler &P = s_g608Mem;
    P.recording = false;
    if (P.out == nullptr)
        return;
    std::fwrite("G608KICK", 1, 8, P.out);
    const uint64_t idx = P.seen - 1ull;
    std::fwrite(&idx, 8, 1, P.out);
    std::fwrite(&P.startPC, 4, 1, P.out);
    const uint32_t pad = 0u;
    std::fwrite(&pad, 4, 1, P.out);
    std::fwrite(&P.pairs, 8, 1, P.out);
    std::fwrite(&P.codeHash, 8, 1, P.out);
    for (uint32_t b = 0; b <= P.nRanges; ++b)
    {
        const G608MemBucket &B = P.bucket[b];
        std::fwrite(B.readMap, 8, kG608MemWords, P.out);
        std::fwrite(B.writeMap, 8, kG608MemWords, P.out);
        std::fwrite(&B.pairs, 8, 1, P.out);
        std::fwrite(&B.reads, 8, 1, P.out);
        std::fwrite(&B.writes, 8, 1, P.out);
    }
    if (--P.budget == 0)
    {
        std::fflush(P.out);
        std::fclose(P.out);
        P.out = nullptr;
        std::fprintf(stderr, "[G608:mem] budget exhausted after %llu kicks -- file closed\n",
                     (unsigned long long)P.seen);
    }
}

static inline void g608MemEndKick()
{
    if (!s_g608Mem.recording)
        return;
    g608MemEndKickCold();
}

} // namespace
