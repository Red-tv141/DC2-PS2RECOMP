// G665 - TRANSLATED GPU VU1 BACKEND: whole-microprogram VU1 -> GLSL translation.
//
// ============================================================================================
// WHY THIS EXISTS, AND WHY IT IS NOT G607
// ============================================================================================
// G606 closed the INTERPRETER-shaped GPU VU1 executor and named translation as the only remaining
// shape. G607 then built a *ceiling bench* for translation and measured it fast - but that bench
// deliberately omits (its own header, tools/g607_emit.py:19-28) the Q/P pipeline latencies, the
// MAC/status/clip flag writes, control flow, and XGKICK. It is a rate probe, not a backend, and it
// says so.
//
// G665 measured, on real corpora, the two numbers that decide the shape:
//   * exact GPU INTERPRETER: 1.98-3.69 M pair-steps/s, and WIDTH MAKES IT SLOWER (12.4x divergence
//     penalty) - dead, because even its unattainable zero-divergence ceiling (31.3 M) is 2.3x
//     slower than ONE 73 M CPU core;
//   * exact TRANSLATION: 387.57 M pair-steps/s at width 1 (5.3x one core), 2,571 M at width 5.
// So the executor, not the dispatch and not the arithmetic, was always the blocker.
//
// ============================================================================================
// THE CENTRAL DESIGN DECISION: SPECIALIZE BY CONSTANT-FOLDING, NOT BY REIMPLEMENTING
// ============================================================================================
// The obvious way to write a translator is to decode each VU1 pair in C++ and emit hand-written
// GLSL for it. That reimplements, in a second language, every exactness property G543 spent a phase
// proving: MSVC Release SSE2 destination-NaN selection, CVTTPS2DQ integer-indefinite overflow,
// correctly-rounded binary32 div/sqrt through double, NVIDIA denormal repair, the Q(7)/P(13) delay
// counters, and the MAC/STATUS delay line. Every one of those is a chance to diverge silently, and
// `bad=0` on the shipped oracle is exactly what we cannot afford to lose.
//
// So this translator emits calls to G543's OWN functions with COMPILE-TIME-CONSTANT instruction
// words, and lets the GLSL compiler specialize them. `execUpper(0x12345678u, false, stamp)` folds:
// the opcode switch collapses to its single taken case, `(instr>>21)&15u` becomes a literal, and -
// the reason this is worth doing at all - `Rvf[upperVfDest(instr)]` becomes `Rvf[<constant>]`.
//
// That last point is the whole performance argument. G543's interpreter indexes `uvec4 Rvf[32]`,
// `int Rvi[16]` and `uvec4 Rram[1024]` with RUNTIME values, and a dynamically-indexed array in GLSL
// cannot live in registers - NVIDIA spills it to local memory, so every register read is a memory
// round trip. Baking the index to a constant is what moves the executor from 3.7 M to ~388 M
// pair-steps/s. It also deletes the two `pw()` image loads per pair (the code fetch) and the entire
// decode+dispatch chain.
//
// The translated program is therefore EXACT BY CONSTRUCTION: it is the same code, specialized.
//
// ============================================================================================
// CONTROL FLOW, INCLUDING INDIRECT BRANCHES
// ============================================================================================
// Each pair becomes one `case` of a `switch (Rpc >> 3u)` inside the ordinary PC-driven loop. This
// is deliberately NOT a basic-block CFG lowering:
//   * it needs no reachability analysis, so it cannot be wrong about what is reachable;
//   * it handles JR/JALR and every computed target natively, because the switch dispatches on the
//     RUNTIME PC - G607's static walk left 4 indirect JRs unresolved and this construction simply
//     does not have that failure mode;
//   * the branch-delay-slot machinery (`pendingBranch`) is G543's, unchanged.
// The cost is one indirect jump per pair instead of per block. That is the conservative starting
// point; block-chaining straight-line runs is a later optimization with its own gate.
//
// `default:` is the IN-SHADER INTERPRETER FALLBACK - it fetches and executes through G543's generic
// path exactly as before. So a PC with no translated case still runs, correctly, on the GPU. That
// is what makes "no workload is unable to use the translated backend" true by construction rather
// than by census: an untranslated pair degrades to interpreted speed, never to a wrong answer and
// never to a bail-out.
//
// ⛔ `break` INSIDE A `switch` BINDS TO THE SWITCH, NOT THE LOOP. G543's loop body uses bare
// `break` for its three exit paths (budget/pc-range, gErrors, E-bit). Emitting that text unchanged
// inside a case would silently turn "stop the microprogram" into "advance to the next pair" - a
// wrong-answer bug that the oracle would catch but that is worth naming here. Every exit is routed
// through the `brk` flag below instead.
//
// This file is compiled only under -DPS2X_G665_GPUVU=ON. `ps2_gs_gpu_raster.cpp` is on
// `tools/g652_layout_guard.ps1` with a growth budget of ZERO (Rule 12b), so the default image must
// not carry one byte of it.

#if defined(PS2X_G665_GPUVU)

// ⛔ NO `#include` HERE. This file is pulled in from inside an anonymous namespace, so a standard
// header included at this point declares a NESTED `std` and every `std::string` in the file then
// resolves against `'anonymous-namespace'::std`. Everything used below (`std::string`,
// `std::snprintf`, `std::vector`) is already visible from the prelude's own includes.

constexpr uint32_t G665_CODE_WORDS = 4096u;  // 16 KiB VU1 code image
constexpr uint32_t G665_PAIRS = G665_CODE_WORDS / 2u;

struct G665TranslateStats
{
    uint32_t emitted = 0u;      // pairs given a specialized case
    uint32_t skipped = 0u;      // pairs left to the in-shader interpreter default
    size_t sourceBytes = 0u;
};

static uint64_t g665CodeImageKey(const uint32_t *codeWords)
{
    uint64_t h = 14695981039346656037ull;
    for (uint32_t i = 0u; i < G665_CODE_WORDS; ++i)
    {
        h ^= codeWords[i];
        h *= 1099511628211ull;
    }
    return h;
}

// A pair is worth specializing when it is not the all-zero fill. VU1's all-zero pair decodes to
// LOWER=0 (an `LQ` form) / UPPER=0, which never appears in real microcode at an executed PC, so
// emitting a case for every zero slot only inflates compile time. Anything non-zero gets a case
// whether or not a reachability analysis would call it live - being wrong about reachability is
// exactly the failure mode this design refuses to have (Rule 33's principle: a static ranking
// cannot see reachability).
static bool g665PairWorthEmitting(uint32_t lower, uint32_t upper)
{
    return (lower | upper) != 0u;
}

// The per-pair body. This is G543's own loop body (its `main`, the `for (; cycle < budget; ++cycle)`
// block) with three mechanical changes and NOTHING else:
//   1. `lower` / `upper` become literals, so every decode expression folds;
//   2. `currentPc` becomes a literal, so the sequential-PC arithmetic folds;
//   3. the three bare `break`s become `brk = true` (see the warning above).
// Keeping it textually close to the original is deliberate: the two must be diffable by eye,
// because any drift between them is a silent exactness bug.
static void g665EmitPairBody(std::string &out, uint32_t pc, uint32_t lower, uint32_t upper)
{
    // Fixed-arity emitters instead of a varargs helper: `<cstdarg>` cannot be included here (see the
    // note at the top of this file), and the argument counts are known at every call site anyway.
    char buf[1024];
    const auto emit1 = [&](const char *fmt, uint32_t a) {
        std::snprintf(buf, sizeof(buf), fmt, a);
        out += buf;
    };
    const auto emit2 = [&](const char *fmt, uint32_t a, uint32_t b) {
        std::snprintf(buf, sizeof(buf), fmt, a, b);
        out += buf;
    };
    const auto emit3 = [&](const char *fmt, uint32_t a, uint32_t b, uint32_t c) {
        std::snprintf(buf, sizeof(buf), fmt, a, b, c);
        out += buf;
    };
    const auto emit4 = [&](const char *fmt, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        std::snprintf(buf, sizeof(buf), fmt, a, b, c, d);
        out += buf;
    };

    emit1("case %uu: { ++gSpecPairs;\n", pc >> 3u);
    // Q/P delay counters tick every pair regardless of opcode - these stay dynamic.
    out += "if (Qdelay > 0 && --Qdelay == 0) Rq = Qpending;\n"
           "if (Pdelay > 0 && --Pdelay == 0) Rp = Ppending;\n";
    // The Q/P early-wake block. Written with literal operands so the whole predicate folds away on
    // the ~97% of pairs whose lower slot is not a special-function op.
    emit3("if (!(%uu != 0u) && ((%uu >> 25u) & 0x7fu) == 0x40u) {\n"
          "  uint fn = s2func(%uu);\n"
          "  bool qProducer = fn >= 0x38u && fn <= 0x3au;\n"
          "  if (Qdelay > 0 && (fn == 0x3bu || qProducer)) { Rq = Qpending; Qdelay = 0; }\n"
          "  bool pProducer = fn == 0x70u || fn == 0x71u || fn == 0x72u || fn == 0x73u ||\n"
          "                   fn == 0x76u || fn == 0x78u || fn == 0x79u || fn == 0x7au;\n"
          "  if (Pdelay > 0 && (fn == 0x7bu || pProducer)) { Rp = Ppending; Pdelay = 0; }\n"
          "}\n",
          (upper >> 31u) & 1u, lower, lower);
    emit3("if (!(%uu != 0u) && lowerReadsStatus((%uu >> 25u) & 0x7fu)) fail(F_PIPE, %uu);\n",
          (upper >> 31u) & 1u, lower, lower);
    emit2("if (!(%uu != 0u) && lowerReadsMac((%uu >> 25u) & 0x7fu))\n"
          "  MacVisible = resolveMac(add64s(add64u(PairBase, cycle), -4));\n",
          (upper >> 31u) & 1u, lower);
    out += "uvec2 stamp = add64u(PairBase, cycle);\n";

    if (((upper >> 31u) & 1u) != 0u)
    {
        // I-bit pair: the lower word is a 32-bit immediate, and the upper runs "eager".
        emit4("Ri = %uu;\n"
              "if (!((%uu & 63u) >= 0x3cu && s2func(%uu) == 0x2fu)) {\n"
              "  Rmac = resolveMac(stamp);\n"
              "  execUpper(%uu, true, stamp);\n"
              "  G428Claim = 2u;\n"
              "  if (gErrors == 0u) pushEager(stamp, Rmac);\n"
              "}\n",
              lower, upper, upper, upper);
    }
    else
    {
        // Normal pair. `upperVfDest` folds to a constant, so the VF hazard save/restore either
        // disappears entirely or becomes a direct, constant-indexed register swap.
        emit4("int hazard = upperVfDest(%uu);\n"
              "uvec4 oldV = uvec4(0u), newV = uvec4(0u);\n"
              "if (hazard >= 0) oldV = Rvf[hazard];\n"
              "execUpper(%uu, false, stamp);\n"
              "if (hazard >= 0) { newV = Rvf[hazard]; Rvf[hazard] = oldV; }\n"
              "if (gErrors == 0u) execLower(%uu);\n"
              "if (hazard >= 0) applyBits(Rvf[hazard], newV, (%uu >> 21u) & 15u);\n",
              upper, upper, lower, upper);
    }

    // Tail. `break` -> `brk` (see the warning at the top of this file).
    emit3("if (gErrors != 0u) { stop = 3u; brk = true; }\n"
          "else {\n"
         "  Rvf[0] = uvec4(0u, 0u, 0u, 0x3f800000u);\n"
         "  Rvi[0] = 0;\n"
         "  uint postExecPc = Rpc;\n"
         "  uint sequential = %uu;\n"
         "  uint nextPc = sequential;\n"
         "  if (pendingBranch != 0xffffffffu) { nextPc = pendingBranch; pendingBranch = 0xffffffffu; }\n"
         "  else if (postExecPc != %uu) {\n"
         "    pendingBranch = postExecPc + 8u;\n"
         "    if (pendingBranch >= 16384u) pendingBranch = 0u;\n"
         "  }\n"
         "  if (nextPc >= 16384u) nextPc = 0u;\n"
         "  Rpc = nextPc;\n"
         "  if (Rebit != 0u) { stop = 2u; brk = true; }\n"
         "  else if (%uu != 0u) Rebit = 1u;\n"
         "}\n",
         (pc + 8u) >= 16384u ? 0u : (pc + 8u), pc, (upper >> 30u) & 1u);
    out += "} break;\n";
}

// The in-shader interpreter fallback, used for any PC without a specialized case. This is G543's
// body verbatim (fetching `lower`/`upper` through `pw()`), so an untranslated pair is executed
// correctly at interpreted speed rather than refused. It is what makes the backend total.
static void g665EmitInterpretedDefault(std::string &out)
{
    out += "default: { ++gInterpPairs;\n"
           "G665_PROFILE_HOOK\n"
           "uint codeWord = (gCodeByte + currentPc) >> 2u;\n"
           "uint lower = pw(codeWord);\n"
           "uint upper = pw(codeWord + 1u);\n"
           "bool eBit = ((upper >> 30u) & 1u) != 0u;\n"
           "bool iBit = ((upper >> 31u) & 1u) != 0u;\n"
           "if (Qdelay > 0 && --Qdelay == 0) Rq = Qpending;\n"
           "if (Pdelay > 0 && --Pdelay == 0) Rp = Ppending;\n"
           "if (!iBit && ((lower >> 25u) & 0x7fu) == 0x40u) {\n"
           "  uint fn = s2func(lower);\n"
           "  bool qProducer = fn >= 0x38u && fn <= 0x3au;\n"
           "  if (Qdelay > 0 && (fn == 0x3bu || qProducer)) { Rq = Qpending; Qdelay = 0; }\n"
           "  bool pProducer = fn == 0x70u || fn == 0x71u || fn == 0x72u || fn == 0x73u ||\n"
           "                   fn == 0x76u || fn == 0x78u || fn == 0x79u || fn == 0x7au;\n"
           "  if (Pdelay > 0 && (fn == 0x7bu || pProducer)) { Rp = Ppending; Pdelay = 0; }\n"
           "}\n"
           "uint lowerOp = (lower >> 25u) & 0x7fu;\n"
           "if (!iBit && lowerReadsStatus(lowerOp)) fail(F_PIPE, lower);\n"
           "if (!iBit && lowerReadsMac(lowerOp))\n"
           "  MacVisible = resolveMac(add64s(add64u(PairBase, cycle), -4));\n"
           "uvec2 stamp = add64u(PairBase, cycle);\n"
           "if (iBit) {\n"
           "  Ri = lower;\n"
           "  bool upperVnop = (upper & 63u) >= 0x3cu && s2func(upper) == 0x2fu;\n"
           "  if (!upperVnop) {\n"
           "    Rmac = resolveMac(stamp);\n"
           "    execUpper(upper, true, stamp);\n"
           "    G428Claim = 2u;\n"
           "    if (gErrors == 0u) pushEager(stamp, Rmac);\n"
           "  }\n"
           "} else {\n"
           "  int hazard = upperVfDest(upper);\n"
           "  uvec4 oldV = uvec4(0u), newV = uvec4(0u);\n"
           "  if (hazard >= 0) oldV = Rvf[hazard];\n"
           "  execUpper(upper, false, stamp);\n"
           "  if (hazard >= 0) { newV = Rvf[hazard]; Rvf[hazard] = oldV; }\n"
           "  if (gErrors == 0u) execLower(lower);\n"
           "  if (hazard >= 0) applyBits(Rvf[hazard], newV, (upper >> 21u) & 15u);\n"
           "}\n"
           "if (gErrors != 0u) { stop = 3u; brk = true; }\n"
           "else {\n"
           "  Rvf[0] = uvec4(0u, 0u, 0u, 0x3f800000u);\n"
           "  Rvi[0] = 0;\n"
           "  uint postExecPc = Rpc;\n"
           "  uint sequential = currentPc + 8u;\n"
           "  if (sequential >= 16384u) sequential = 0u;\n"
           "  uint nextPc = sequential;\n"
           "  if (pendingBranch != 0xffffffffu) { nextPc = pendingBranch; pendingBranch = 0xffffffffu; }\n"
           "  else if (postExecPc != currentPc) {\n"
           "    pendingBranch = postExecPc + 8u;\n"
           "    if (pendingBranch >= 16384u) pendingBranch = 0u;\n"
           "  }\n"
           "  if (nextPc >= 16384u) nextPc = 0u;\n"
           "  Rpc = nextPc;\n"
           "  if (Rebit != 0u) { stop = 2u; brk = true; }\n"
           "  else if (eBit) Rebit = 1u;\n"
           "}\n"
           "} break;\n";
}

// Build a complete compute shader specialized to one VU1 code image.
//
// Everything above `void main()` in G543's source is reused VERBATIM - the state declarations, the
// exact float helpers, `execUpper`, `emitKick`, `execLower`, `loadRecord`, `storeRecord`. Only
// `main` is replaced. That is why this cannot drift from the interpreter's semantics: it shares
// every line that computes a value.
//
// ⛔⛔ COMPILE TIME IS THE BINDING CONSTRAINT, NOT CORRECTNESS (measured, G665).
// Specializing ALL 1,620 non-zero pairs of the `fight` dominant image produces 2.66 MB of GLSL and
// the NVIDIA driver had **not finished linking it after 420 seconds**. Constant-folding works by
// INLINING `execUpper`/`execLower` - each a large opcode switch - once per pair, so the IR the
// optimizer sees before folding grows as (pairs x switch size). Translation itself is trivial
// (13.5 ms); the driver's optimizer is the wall.
//
// `maxPairs` bounds the specialized set. Pairs beyond the bound fall to the in-shader interpreter
// `default:`, so the program stays TOTAL and EXACT at any bound - the bound trades throughput for
// compile time and nothing else. That is what makes a partial translation a legitimate product
// rather than a broken one, and it is why the fallback was built before the fast path.
//
// ⛔⛔ `bad=0` IS VACUOUS UNLESS THE SPECIALIZED PATH ACTUALLY RAN (Rule 25 / G657 law 10).
// `maxPairs` selects the first N non-zero pairs in PC ORDER, and there is no reason those are the
// pairs a kick executes - a microprogram entered at PC 0x1850 may never touch pair 0. So the shader
// counts, per record, how many pairs went through a SPECIALIZED case (`gSpecPairs`) versus the
// interpreted `default:` (`gInterpPairs`), and the bench reports both. Without that denominator a
// clean oracle would only prove the interpreter still works.
//
// The counters live in a per-record 2-word block appended AFTER the status area, because status
// slots are packed exactly 8 apart (`cmd[31] = payloadWords + rec*8`) and have no spare room.
//
// ⛔⛔ SELECTING THE SPECIALIZED SET BY PC ORDER IS USELESS, MEASURED. The first N non-zero pairs of
// an image are not the pairs a kick runs: at maxPairs=64 the counters read **spec=4,940 of
// 1,386,253 executed pairs - 0.36%**, so 64 specialized cases cost 36% of throughput while running
// almost never. Selection must be PROFILE-GUIDED, which is why `hotPairs` exists: pass 1 runs the
// pure interpreter with `profile=true` to build a per-PC execution histogram, and pass 2 specializes
// the pairs that histogram says are actually hot.
static std::string g665TranslateProgram(const uint32_t *codeWords, uint32_t local,
                                        uint32_t maxPairs, bool fallback,
                                        uint32_t payloadWords, uint32_t records,
                                        bool profile, const std::vector<uint32_t> *hotPairs,
                                        G665TranslateStats &st, std::string &why)
{
    const std::string base(g543SemanticShaderSource());
    const size_t mainAt = base.find("void main()");
    if (mainAt == std::string::npos)
    {
        why = "G543 source has no main() to replace";
        return std::string();
    }

    std::string out;
    out.reserve(4u * 1024u * 1024u);
    out.append(base, 0, mainAt);

    // Workgroup width. ⚠️ G665 measured that widening the INTERPRETER makes it slower (12.4x
    // divergence penalty) because lanes land on different VU PCs. A translated program has the same
    // property when one lane is one KICK, so width>1 is exposed for measurement, not assumed good.
    if (local != 1u)
    {
        const std::string oldDecl = "layout(local_size_x=1, local_size_y=1, local_size_z=1) in;";
        const size_t at = out.find(oldDecl);
        if (at == std::string::npos)
        {
            why = "G543 source has no workgroup declaration to widen";
            return std::string();
        }
        char decl[128];
        std::snprintf(decl, sizeof(decl),
                      "layout(local_size_x=%u, local_size_y=1, local_size_z=1) in;", local);
        out.replace(at, oldDecl.size(), decl);
    }

    out += "\nuint gSpecPairs;\nuint gInterpPairs;\n"
           "\nvoid main()\n{\n"
           "    gSpecPairs = 0u; gInterpPairs = 0u;\n"
           "    uint rec = gl_GlobalInvocationID.x;\n"
           "    gCmd = rec * 32u;\n"
           "    gErrors = 0u; gFailPc = 0xffffffffu; gFailWord = 0u;\n"
           "    gPacketCount = 0u; gPacketBytes = 0u;\n"
           "    Rpc = 0xffffffffu;\n"
           "    if (cw(gCmd) != ABI_MAGIC || cw(gCmd + 1u) != 1u || cw(gCmd + 2u) != 32u) gErrors |= 1u;\n"
           "    gPostStateByte = cw(gCmd + 16u); gPostPipeByte = cw(gCmd + 18u);\n"
           "    gCodeByte = cw(gCmd + 19u); gPreDataByte = cw(gCmd + 20u); gPostDataByte = cw(gCmd + 21u);\n"
           "    gPacketDescByte = cw(gCmd + 26u); gExpectedPackets = cw(gCmd + 27u);\n"
           "    gPacketBytesByte = cw(gCmd + 28u); gExpectedPacketBytes = cw(gCmd + 29u);\n"
           "    gTop = cw(gCmd + 8u);\n"
           "    if (gPostStateByte - cw(gCmd + 15u) != 632u || cw(gCmd + 17u) - gPostStateByte != 632u ||\n"
           "        gPostPipeByte - cw(gCmd + 17u) != 644u || gCodeByte - gPostPipeByte != 644u ||\n"
           "        gPreDataByte - gCodeByte != 16384u || gPostDataByte - gPreDataByte != 16384u)\n"
           "        gErrors |= F_LAYOUT;\n"
           "    loadRecord();\n"
           "    armHistory();\n"
           "    bool resume = (cw(gCmd + 3u) & 1u) != 0u;\n"
           "    Rebit = 0u;\n"
           "    Ritop = cw(gCmd + 7u);\n"
           "    if (!resume) {\n"
           "        Rpc = cw(gCmd + 6u);\n"
           "        Rvf[0] = uvec4(0u, 0u, 0u, 0x3f800000u);\n"
           "    }\n"
           "    uint budget = cw(gCmd + 9u);\n"
           "    uint cycle = 0u;\n"
           "    uint stop = 0u;\n"
           "    uint pendingBranch = 0xffffffffu;\n"
           "    for (; cycle < budget; ++cycle) {\n"
           "        uint currentPc = Rpc;\n"
           "        if (currentPc + 8u > 16384u) { stop = 1u; break; }\n"
           "        bool brk = false;\n"
           "        switch (currentPc >> 3u) {\n";

    if (hotPairs != nullptr)
    {
        // Profile-guided: `hotPairs` is already ranked by measured execution count.
        for (uint32_t pair : *hotPairs)
        {
            if (st.emitted >= maxPairs || pair >= G665_PAIRS)
                break;
            const uint32_t lower = codeWords[pair * 2u];
            const uint32_t upper = codeWords[pair * 2u + 1u];
            if (!g665PairWorthEmitting(lower, upper))
                continue;
            g665EmitPairBody(out, pair * 8u, lower, upper);
            ++st.emitted;
        }
        st.skipped = G665_PAIRS - st.emitted;
    }
    else
    {
        for (uint32_t pair = 0u; pair < G665_PAIRS; ++pair)
        {
            const uint32_t lower = codeWords[pair * 2u];
            const uint32_t upper = codeWords[pair * 2u + 1u];
            if (!g665PairWorthEmitting(lower, upper) || st.emitted >= maxPairs)
            {
                ++st.skipped;
                continue;
            }
            g665EmitPairBody(out, pair * 8u, lower, upper);
            ++st.emitted;
        }
    }
    if (fallback)
    {
        const size_t before = out.size();
        g665EmitInterpretedDefault(out);
        // Profile pass: count executions per PC into a shared 2048-word histogram. `imageAtomicAdd`
        // because every record is its own invocation. Only ever emitted in pass 1 - it is a probe,
        // never part of a measured arm (G657 law: an instrument that changes what it measures).
        const std::string hook = "G665_PROFILE_HOOK";
        const size_t at = out.find(hook, before);
        if (at != std::string::npos)
        {
            std::string repl;
            if (profile)
            {
                char b[192];
                std::snprintf(b, sizeof(b),
                              "imageAtomicAdd(gOutput, lc(%uu + (currentPc >> 3u)), 1u);",
                              payloadWords + records * 8u + records * 2u);
                repl = b;
            }
            out.replace(at, hook.size(), repl);
        }
    }
    else
        // PURE-TRANSLATED arm (DC2_G665_NOFALLBACK=1). The interpreter is REMOVED from the program
        // rather than merely unused, which is the only way to test whether its presence is what
        // costs the throughput: NVIDIA sizes occupancy from the whole shader's worst-case register
        // demand, so `execUpper`/`execLower` living in the same program can throttle every lane
        // even on pairs that never reach them. An unspecialized PC stops the record (`stop=1`)
        // instead of executing, so this arm is only VALID when coverage is complete - the bench's
        // `interp=` counter is what proves that.
        out += "default: { stop = 1u; brk = true; } break;\n";

    out += "        }\n"
           "        if (brk) break;\n"
           "    }\n"
           "    if (Pdelay > 0 && Rebit != 0u) { Rp = Ppending; Pdelay = 0; }\n"
           "    Rmac = resolveMac(add64u(PairBase, cycle));\n"
           "    PairBase = add64u(PairBase, cycle);\n"
           "    if (cycle != cw(gCmd + 10u) || cw(gCmd + 11u) != 0u || stop != cw(gCmd + 12u)) gErrors |= F_RESULT;\n"
           "    if (gPacketCount != gExpectedPackets || gPacketBytes != gExpectedPacketBytes) gErrors |= F_PACKET;\n"
           "    storeRecord();\n"
           "    uint statusAt = cw(gCmd + 31u);\n"
           "    sw(statusAt + 0u, STATUS_MAGIC);\n"
           "    sw(statusAt + 1u, gErrors);\n"
           "    sw(statusAt + 2u, gFailPc);\n"
           "    sw(statusAt + 3u, gFailWord);\n"
           "    sw(statusAt + 4u, cycle);\n"
           "    sw(statusAt + 5u, stop);\n"
           "    sw(statusAt + 6u, gPacketCount);\n"
           "    sw(statusAt + 7u, gPacketBytes);\n";
    {
        char tail[256];
        std::snprintf(tail, sizeof(tail),
                      "    uint g665Counters = %uu + %uu * 8u + rec * 2u;\n"
                      "    sw(g665Counters + 0u, gSpecPairs);\n"
                      "    sw(g665Counters + 1u, gInterpPairs);\n"
                      "}\n",
                      payloadWords, records);
        out += tail;
    }

    st.sourceBytes = out.size();
    return out;
}

#endif // PS2X_G665_GPUVU
