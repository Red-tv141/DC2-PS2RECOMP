// G665 - BIDIRECTIONAL VU1 BACKEND STATE MIGRATION, the CPU half.
//
// WHY THIS EXISTS. G566 makes the GPU authoritative for VU1: it seeds state+pipe+code+data once,
// then ships only the ordered VIF writes per kick, and `runtime_mtvu_and_env.inc` stops running the
// CPU interpreter entirely. That is a ONE-WAY door today. Nothing can bring VU1 state back to the
// CPU, so:
//   * a GPU failure is fail-closed and NOT fail-back - `g566PublishBatchFailure` publishes ok=false
//     with no packets and the frame silently loses that geometry, and
//   * a per-workload backend SELECTOR is impossible, because selecting the CPU for the next kick
//     requires the CPU to hold the state the GPU just produced.
// Both of those are exit-condition items. The missing primitive is the same one: import.
//
// The export direction already exists as `G566VuAuthority::encodeState` + `dc2_g541_vu_pipe_snapshot`
// (CPU -> GPU seed). This file is the inverse (GPU -> CPU), and it must be EXACT: the two encodings
// are the persistent cross-kick state, so any field that does not survive a round trip is a silent
// divergence in the next kick's arithmetic, not a crash.
//
// WHAT MAKES A BATCH RECOVERABLE. `g545_vu_gpu_live_shader.inc` calls `storePersistentWorkspace`
// only at line 334, AFTER its whole record loop; every error path returns at :303 / :327 without
// storing, and the architectural registers live in shader locals loaded once by `loadLiveWorkspace`
// at :253. So a failed batch leaves the persistent workspace holding its exact PRE-BATCH state.
// Batches are atomic. That is what lets the CPU re-run a failed batch from its first kick and get
// bit-identical results, rather than resuming from a half-written state.
//
// LAYOUT. Fixed by the shader's own constants (`g545_vu_gpu_live_shader.inc:149-152`) and by the
// two encoders this file inverts:
//     word   16 .. 173   VU1State           158 words / 632 B   (LIVE_STATE_WORD)
//     word  174 .. 334   hidden pipeline     161 words / 644 B   (LIVE_PIPE_WORD)
//     word  335 .. 4430  VU1 code image     4096 words /  16 KB  (LIVE_CODE_WORD)
//     word 4431 .. 8526  VU1 data RAM       4096 words /  16 KB  (LIVE_DATA_WORD)
// Cross-checked against `canonicalizeSeedHistoryPipe` (runtime_g566_vu_authority.inc:163-181),
// which addresses s_g430HB.vec at word 82 and s_g430HB.dest at word 138 of the PIPE section - both
// land exactly where the field walk below puts them.
//
// This file is pure state marshalling. It executes no VU instruction and reads no env flag.
//
// ⛔⛔ WHY THE WHOLE FILE IS `#if PS2X_G665_DIAG` (Rule 31, and Rule 12b's price).
// These two functions are ~4.3 KB of machine code, and `ps2_vu1.obj` is the project's SECOND
// HOTTEST translation unit - it is on `tools/g652_layout_guard.ps1` with a growth budget of ZERO.
// Compiled in unconditionally they moved it 630,896 -> 635,527 bytes (+4,631) and the guard FAILED.
// G662 P4 priced 21,652 bytes of that TU class at -0.448 ms/f and G653 P25 priced ~60 inert lines
// at +0.484 ms/f, so hot-TU code that nothing calls is NOT free.
//
// As of this phase there is no SHIPPED caller: the translated GPU backend that would migrate state
// back to the CPU is not built. Rule 31 says a lever with no promoted caller comes out of the
// default image and keeps its census behind a compile-time option. So this stays diagnostic-gated,
// where it backs the `[G665:exact]` both-backend oracle and costs the deliverable nothing.
//
// ⭐ RE-OPEN CONDITION, written at the call site as Rule 31 requires: make this unconditional in
// the same edit that lands a shipped caller - i.e. when the GPU backend can hand VU1 state back to
// the CPU (the fail-back path in `runtime_mtvu_and_env.inc:929`, or the per-workload selector).
// At that point it is load-bearing rather than inert, and its +4,631 bytes get acknowledged with
// `DC2_ACK_LAYOUT_GROWTH` against a measured frame gate - never assumed free.

#if defined(PS2X_G665_DIAG)

constexpr uint32_t G665_PIPE_MAGIC = 0x31505556u; // little-endian "VUP1"
constexpr uint32_t G665_PIPE_VERSION = 1u;
constexpr size_t G665_PIPE_BYTES = 644u;
constexpr size_t G665_STATE_BYTES = 632u;

namespace
{
    // Little-endian symmetric reader. Deliberately mirrors the writer in
    // vu1_g541_pipeline_snapshot.inc statement for statement, so the two can be diffed by eye.
    struct G665PipeReader
    {
        const uint8_t *p;
        size_t left;
        bool ok = true;

        uint32_t u32()
        {
            if (left < 4u)
            {
                ok = false;
                return 0u;
            }
            const uint32_t v = static_cast<uint32_t>(p[0]) |
                               (static_cast<uint32_t>(p[1]) << 8u) |
                               (static_cast<uint32_t>(p[2]) << 16u) |
                               (static_cast<uint32_t>(p[3]) << 24u);
            p += 4u;
            left -= 4u;
            return v;
        }
        uint64_t u64()
        {
            const uint64_t lo = u32();
            const uint64_t hi = u32();
            return lo | (hi << 32u);
        }
        float f32()
        {
            const uint32_t bits = u32();
            float v = 0.0f;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }
    };
}

// Inverse of dc2_g541_vu_pipe_snapshot.
//
// ⭐ WHY THIS WRITES THE LIVE PIPELINE DIRECTLY, with no staging copy. The obvious defensive shape
// is to decode into locals and commit only on success, so a short stream cannot half-apply. Here
// that is provably dead weight: the writer emits a FIXED 644 bytes, the field walk below consumes
// exactly 644 (8 + 16 + 64 + 12 + 128 + 32 + 32 + 4 + 16 + 16 + 128 + 64 + 32 + 32 + 32 + 4 + 8 + 4
// + 12 = 644), and `bytes != G665_PIPE_BYTES` is rejected before the first read. So once the length
// and magic checks pass, `r.ok` cannot go false and there is no partial-write case to defend
// against. The staged version cost 3,088 bytes of machine code in the project's second-hottest TU
// (Rule 12b) to defend a state the arithmetic excludes.
//
// The 644 is asserted at the bottom rather than assumed, so a future field added to the writer
// without a matching read here fails loudly instead of silently importing garbage.
__declspec(noinline) bool dc2_g665_vu_pipe_restore(const uint8_t *in, size_t bytes)
{
    if (in == nullptr || bytes != G665_PIPE_BYTES)
        return false;

    G665PipeReader r{in, bytes};
    if (r.u32() != G665_PIPE_MAGIC || r.u32() != G665_PIPE_VERSION)
        return false;

    s_vuQPending = r.f32();
    s_vuQDelay = static_cast<int>(r.u32());
    s_vuPPending = r.f32();
    s_vuPDelay = static_cast<int>(r.u32());

    for (uint32_t &v : s_vuMacPipe) v = r.u32();
    for (uint32_t &v : s_vuStatusPipe) v = r.u32();
    s_vuMacVisible = r.u32();
    s_vuStatusVisible = r.u32();
    s_g328MacPipeHead = static_cast<int>(r.u32());

    for (auto &vec : s_g428PendVec)
        for (float &v : vec) v = r.f32();
    for (uint8_t &v : s_g428PendDest) v = static_cast<uint8_t>(r.u32());
    for (uint8_t &v : s_g428Kind) v = static_cast<uint8_t>(r.u32());
    s_g428Claim = static_cast<uint8_t>(r.u32());
    for (float &v : s_g428PreVec) v = r.f32();
    s_g428PreDest = static_cast<uint8_t>(r.u32());
    s_g428PreKind = static_cast<uint8_t>(r.u32());
    s_g428PreMac = r.u32();
    s_g428Armed = r.u32() != 0u;

    for (auto &vec : s_g430HB.vec)
        for (float &v : vec) v = r.f32();
    for (int64_t &v : s_g430HB.stamp) v = static_cast<int64_t>(r.u64());
    for (uint32_t &v : s_g430HB.mac) v = r.u32();
    for (uint8_t &v : s_g430HB.dest) v = static_cast<uint8_t>(r.u32());
    for (uint8_t &v : s_g430HB.kind) v = static_cast<uint8_t>(r.u32());
    s_g430HB.head = static_cast<int>(r.u32());
    s_g430PairBase = static_cast<int64_t>(r.u64());
    s_g430HistArmed = r.u32() != 0u;

    s_g106UseLowerFlagRead = r.u32() != 0u;
    s_g106LowerMacRead = r.u32();
    s_g106LowerStatusRead = r.u32();

    // Schema drift guard, not an underrun guard: both must hold by construction above.
    return r.ok && r.left == 0u;
}

// Decode the 632-byte VU1State encoding produced by G566VuAuthority::encodeState. Kept HERE, beside
// the pipe restore, so the two halves of one migration are read together and cannot drift apart.
__declspec(noinline) bool dc2_g665_vu_state_restore(const uint8_t *in, size_t bytes, VU1State &out)
{
    if (in == nullptr || bytes != G665_STATE_BYTES)
        return false;
    G665PipeReader r{in, bytes};
    for (auto &vf : out.vf)
        for (float &v : vf) v = r.f32();
    for (int32_t &v : out.vi) v = static_cast<int32_t>(r.u32());
    for (float &v : out.acc) v = r.f32();
    out.q = r.f32();
    out.p = r.f32();
    out.i = r.f32();
    out.pc = r.u32();
    out.mac = r.u32();
    out.clip = r.u32();
    out.status = r.u32();
    out.ebit = r.u32() != 0u;
    out.itop = r.u32();
    out.xitop = r.u32();
    return r.ok && r.left == 0u;
}

// Round-trip self-test for the migration primitives, printed once at first call.
//
// This is the gate that makes an import trustworthy. A field the WRITER emits and the READER skips
// produces a stream that still validates (the length check passes, because both halves are wrong by
// the same amount only if the drift is at the tail) - so length alone is not proof. Snapshotting the
// LIVE pipeline, restoring it, and re-snapshotting compares every byte of the actual representation.
//
// It runs against whatever the calling thread's pipeline currently holds, so call it after real work
// rather than at startup: an all-zero pipeline would pass a broken reader for any field whose
// default is zero.
void dc2_g665_migration_selftest(const char *where)
{
    static std::atomic<uint32_t> s_runs{0u};
    const uint32_t n = s_runs.fetch_add(1u, std::memory_order_relaxed) + 1u;

    std::vector<uint8_t> before, after;
    dc2_g541_vu_pipe_snapshot(before);
    const bool sized = before.size() == G665_PIPE_BYTES;
    const bool restored = sized && dc2_g665_vu_pipe_restore(before.data(), before.size());
    if (restored)
        dc2_g541_vu_pipe_snapshot(after);

    size_t firstBad = SIZE_MAX;
    if (restored && after.size() == before.size())
    {
        for (size_t i = 0u; i < before.size(); ++i)
            if (before[i] != after[i])
            {
                firstBad = i;
                break;
            }
    }
    else
        firstBad = 0u;

    // Print the DENOMINATOR unconditionally (G657 measurement law 10 / Rule 25): a self-test that
    // only prints on failure is indistinguishable from one that never ran.
    std::fprintf(stderr,
                 "[G665:selftest] n=%u where=%s pipeBytes=%zu sized=%d restored=%d "
                 "roundTrip=%s firstBadByte=%lld\n",
                 n, where ? where : "?", before.size(), sized ? 1 : 0, restored ? 1 : 0,
                 firstBad == SIZE_MAX ? "OK" : "MISMATCH",
                 firstBad == SIZE_MAX ? -1LL : static_cast<long long>(firstBad));
}

#endif // PS2X_G665_DIAG
