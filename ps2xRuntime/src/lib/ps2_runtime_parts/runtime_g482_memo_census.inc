// ============================================================================
// G482 — PRICE the intra-frame kick memo's HASH before building the memo.
//
// The lead the ROADMAP carried out of G479/G481: `[G331:kicks]` reports dupIntra at a stable
// ~7.4% of geometry kicks. G331's identity is (startPC, itop, top, FNV(writeBytes)) — it IGNORES
// the persistent VU state the worker owns, so that 7.4% is an UPPER BOUND, not a hit rate.
//
// A real memo needs the FULL pre-state as its key:
//   - 16 KB of persistent VU DATA RAM (m_workerVuData),
//   - the persistent VU1State register file (vf/vi/acc/q/p/i/mac/clip/status/pc),
//   - the VU CODE RAM generation (MPG writes; tracked as a counter, not hashed — conservative),
//   - the kick parameters (startPC, itop, top, budget).
// A chained/history digest cannot substitute: two different write histories that arrive at the
// same VU RAM content are the SAME state for the purposes of a memo, and a history digest calls
// them different — which is exactly the case a multi-pass RTT+display kick pair is in.
//
// So the key costs ~16.6 KB of hashing per kick x ~1,230 kicks/frame ~= 20 MB/frame. G476's rule
// (census before mechanism) says price that FIRST: if the key alone costs more than the ~1.3 ms/f
// the memo could return, the vein closes on arithmetic and no mechanism gets built.
//
// This file answers both halves in one binary:
//   DC2_G482_MEMOCENSUS=1  -> full census on the VU1 worker: true full-identity intra-frame hit
//                            rate, the EXECUTE TIME those hits carry (which is what a memo saves),
//                            the restore payload a hit would have to copy back, and a SOUNDNESS
//                            oracle (do two kicks with an identical pre-key really produce an
//                            identical post-state?).
//   DC2_G419_AB=memohash   -> paired within-process arm that computes ONLY the key and sinks it.
//                            This is the PRICE of the key, measured on the pole thread, against a
//                            control arm in the same process (G418/G438).
// Both are default-off; the shipped path does one cached env read per worker start.
// ============================================================================

#include <cstddef> // offsetof

// SSE2-only xor/shift/add mixer. Four independent 128-bit chains so the 16 KB walk is bound by L1
// bandwidth, not by the mixer's dependency chain. This is a CENSUS hash: it must be fast and
// diffuse enough that ~1,230 keys/frame do not alias, not cryptographic. Its speed is the number
// this phase is measuring, so it is deliberately the same shape a real memo would ship.
static inline uint64_t g482HashBlock(const void *p, size_t bytes, uint64_t seed)
{
    const __m128i *q = reinterpret_cast<const __m128i *>(p);
    const size_t n16 = bytes >> 4;
    __m128i a0 = _mm_set_epi32((int)0x9E3779B1u, (int)(uint32_t)(seed >> 32), (int)0x85EBCA77u, (int)(uint32_t)seed);
    __m128i a1 = _mm_set_epi32((int)0xC2B2AE3Du, (int)0x27D4EB2Fu, (int)0x165667B1u, (int)0x1B873593u);
    __m128i a2 = _mm_set_epi32((int)0x85EBCA6Bu, (int)0xC2B2AE35u, (int)0x2545F491u, (int)0x9E3779B9u);
    __m128i a3 = _mm_set_epi32((int)0x7FEB352Du, (int)(uint32_t)seed, (int)0x846CA68Bu, (int)(uint32_t)(seed >> 32));
    size_t i = 0;
    for (; i + 4 <= n16; i += 4)
    {
        a0 = _mm_xor_si128(a0, _mm_loadu_si128(q + i + 0));
        a1 = _mm_xor_si128(a1, _mm_loadu_si128(q + i + 1));
        a2 = _mm_xor_si128(a2, _mm_loadu_si128(q + i + 2));
        a3 = _mm_xor_si128(a3, _mm_loadu_si128(q + i + 3));
        a0 = _mm_add_epi64(a0, _mm_slli_epi64(a0, 7));
        a1 = _mm_add_epi64(a1, _mm_slli_epi64(a1, 7));
        a2 = _mm_add_epi64(a2, _mm_slli_epi64(a2, 7));
        a3 = _mm_add_epi64(a3, _mm_slli_epi64(a3, 7));
        a0 = _mm_xor_si128(a0, _mm_srli_epi64(a0, 23));
        a1 = _mm_xor_si128(a1, _mm_srli_epi64(a1, 23));
        a2 = _mm_xor_si128(a2, _mm_srli_epi64(a2, 23));
        a3 = _mm_xor_si128(a3, _mm_srli_epi64(a3, 23));
    }
    for (; i < n16; ++i)
    {
        a0 = _mm_xor_si128(a0, _mm_loadu_si128(q + i));
        a0 = _mm_add_epi64(a0, _mm_slli_epi64(a0, 7));
        a0 = _mm_xor_si128(a0, _mm_srli_epi64(a0, 23));
    }
    // tail bytes (VU1State is not a multiple of 16)
    uint64_t h = 0ull;
    const uint8_t *tail = reinterpret_cast<const uint8_t *>(p) + (n16 << 4);
    for (size_t k = (n16 << 4); k < bytes; ++k)
    {
        h ^= tail[k - (n16 << 4)];
        h *= 1099511628211ull;
    }
    __m128i s = _mm_xor_si128(_mm_add_epi64(a0, a1), _mm_add_epi64(a2, a3));
    alignas(16) uint64_t lanes[2];
    _mm_store_si128(reinterpret_cast<__m128i *>(lanes), s);
    h ^= lanes[0] * 0x9E3779B97F4A7C15ull;
    h ^= lanes[1] + 0x165667B19E3779F9ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}

// G482 census/price gate. 0 = off (shipped default), 1 = census, and the paired price arm is
// selected independently through DC2_G419_AB=memohash.
static inline bool g482CensusOn()
{
    static const bool s_on = (std::getenv("DC2_G482_MEMOCENSUS") != nullptr);
    return s_on;
}

// Sink for the price arm: `volatile` so MSVC cannot delete the hash it is paying for.
static volatile uint64_t g_g482HashSink = 0ull;

// VU CODE RAM generation. MPG writes are explicit and rare relative to kicks, so the code half of
// the state is tracked as a monotonic counter instead of a second 16 KB hash. Conservative: two
// identical code images written twice get different generations and therefore MISS.
static uint64_t g_g482CodeGen = 0ull;

struct G482PostRec
{
    uint64_t postKey;   // full post-state key of the FIRST execution with this pre-key
    uint64_t execNs;    // its measured execute() time
    uint64_t pktBytes;  // its Path1 output bytes (what a memo replays)
    uint32_t count;
};

// Second, WEAKER identity — exactly G331's (startPC,itop,top,writeBytes) — so the census can say
// WHY the full key misses when G331's hits. For each G331-identity repeat we record whether the
// 16 KB VU data RAM matched, whether the register file matched, or both. A vein that dies because
// the DATA RAM always differs is structurally dead; one that dies only on registers might be
// recoverable with a narrower state slice.
struct G482WeakRec
{
    uint64_t dataKey;
    uint64_t regKey;
};

// One entry per distinct pre-key seen this frame.
static std::unordered_map<uint64_t, G482PostRec> &g482FrameMap()
{
    static std::unordered_map<uint64_t, G482PostRec> *m = new std::unordered_map<uint64_t, G482PostRec>();
    return *m;
}

static std::unordered_map<uint64_t, G482WeakRec> &g482WeakMap()
{
    static std::unordered_map<uint64_t, G482WeakRec> *m = new std::unordered_map<uint64_t, G482WeakRec>();
    return *m;
}

// Window accumulators (30 frames, matching [G331:kicks]).
struct G482Win
{
    uint32_t frames = 0u;
    uint64_t kicks = 0ull, resume = 0ull;
    uint64_t dupIntra = 0ull;      // full-identity intra-frame duplicates
    uint64_t dupUnsound = 0ull;    // duplicates whose POST state differed => key is not sufficient
    uint64_t execNsAll = 0ull;     // execute() ns over all geometry kicks
    uint64_t execNsDup = 0ull;     // execute() ns a memo would have skipped
    uint64_t hashNs = 0ull;        // measured cost of computing the pre-key + post-key
    uint64_t hashCalls = 0ull;
    uint64_t dupPktBytes = 0ull;   // Path1 bytes a memo hit must replay
    uint64_t dupStateBytes = 0ull; // post-state bytes a memo hit must restore (16 KB, coarse)
    // WHY the full key misses when G331's weak identity hits
    uint64_t weakDup = 0ull;       // G331-identity repeats (params + writeBytes only)
    uint64_t weakDataMatch = 0ull; // ... of which the 16 KB VU data RAM also matched
    uint64_t weakRegMatch = 0ull;  // ... of which the register file also matched
};
static G482Win &g482Win()
{
    static G482Win *w = new G482Win();
    return *w;
}

// The 16 KB persistent VU DATA RAM half of the key — the dominant cost.
static inline uint64_t g482DataKey(const uint8_t *vuData, uint32_t dataSize)
{
    return g482HashBlock(vuData, dataSize, 0x243F6A8885A308D3ull);
}

// The persistent REGISTER half. Inputs only (see g482StateKey's note on `pc`).
static inline uint64_t g482RegKey(const VU1State &st)
{
    uint64_t h = g482HashBlock(&st, offsetof(VU1State, pc), 0x13198A2E03707344ull);
    h ^= (uint64_t)st.mac * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)st.clip * 0xC2B2AE3D27D4EB4Full;
    h ^= (uint64_t)st.status * 0x165667B19E3779F9ull;
    return h;
}

// G331's WEAK identity, recomputed here so both identities come from one binary and one run.
static inline uint64_t g482ParamKey(uint32_t startPC, uint32_t itop, uint32_t top,
                                    const std::vector<uint8_t> &wb)
{
    uint64_t h = 1469598103934665603ull;
    const uint32_t hdr[3] = {startPC, itop, top};
    const uint8_t *b = reinterpret_cast<const uint8_t *>(hdr);
    for (size_t i = 0; i < sizeof(hdr); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    if (!wb.empty())
        h = g482HashBlock(wb.data(), wb.size(), h);
    return h;
}

// Compute the FULL memo key: persistent VU data RAM + VU1State + code generation + kick params.
static inline uint64_t g482StateKey(const uint8_t *vuData, uint32_t dataSize, const VU1State &st,
                                    uint32_t startPC, uint32_t itop, uint32_t top, uint32_t budget)
{
    uint64_t h = g482DataKey(vuData, dataSize);
    // Hash only the register fields that are INPUTS to a non-resume kick: vf / vi / acc / q / p / i
    // (one contiguous run ending at `pc`) plus the three flag words. `pc`, `ebit`, `itop` and
    // `xitop` are all overwritten by execute(startPC, itop) before the first instruction runs, so
    // including them would make two otherwise-identical kicks miss purely on their PREDECESSOR's
    // ending PC — an over-specific key that manufactures a false negative.
    h ^= g482RegKey(st) * 0x2545F4914F6CDD1Dull;
    h ^= g_g482CodeGen * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t)startPC * 0x100000001B3ull;
    h ^= (uint64_t)itop << 17;
    h ^= (uint64_t)top << 33;
    h ^= (uint64_t)budget * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 31;
    return h;
}

// Frame boundary + 30-frame report. Called from the worker before each geometry kick.
static inline void g482FrameTick()
{
    static uint64_t s_lastTick = ~0ull;
    const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
    if (tick == s_lastTick)
        return;
    s_lastTick = tick;
    g482FrameMap().clear();
    g482WeakMap().clear();
    G482Win &w = g482Win();
    ++w.frames;
    if (w.frames < 30u)
        return;
    const double f = (double)w.frames;
    const uint64_t geom = w.kicks - w.resume;
    std::fprintf(stderr,
                 "[G482:memo] frames=%u kicks/f=%.0f resume/f=%.0f dupIntraFULL/f=%.1f (%.2f%%) "
                 "unsound=%llu | execAll=%.2fms/f execDup=%.2fms/f (%.2f%%) | "
                 "hash=%.3fms/f (%.0f calls/f, %.0fns/call) | restore=%.0fKB/f pkt=%.0fKB/f | "
                 "NET(exec-hash)=%+.2fms/f | weakDup/f=%.1f (%.2f%%) ofWhich dataMatch=%.1f%% "
                 "regMatch=%.1f%%\n",
                 w.frames, (double)w.kicks / f, (double)w.resume / f,
                 (double)w.dupIntra / f,
                 geom ? 100.0 * (double)w.dupIntra / (double)geom : 0.0,
                 (unsigned long long)w.dupUnsound,
                 (double)w.execNsAll / f / 1e6, (double)w.execNsDup / f / 1e6,
                 w.execNsAll ? 100.0 * (double)w.execNsDup / (double)w.execNsAll : 0.0,
                 (double)w.hashNs / f / 1e6, (double)w.hashCalls / f,
                 w.hashCalls ? (double)w.hashNs / (double)w.hashCalls : 0.0,
                 (double)w.dupStateBytes / f / 1024.0, (double)w.dupPktBytes / f / 1024.0,
                 ((double)w.execNsDup - (double)w.hashNs) / f / 1e6,
                 (double)w.weakDup / f,
                 geom ? 100.0 * (double)w.weakDup / (double)geom : 0.0,
                 w.weakDup ? 100.0 * (double)w.weakDataMatch / (double)w.weakDup : 0.0,
                 w.weakDup ? 100.0 * (double)w.weakRegMatch / (double)w.weakDup : 0.0);
    w = G482Win();
}
