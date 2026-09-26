// =============================================================================================
// G757 — THE SCALAR (Q/P) PIPELINE, EXECUTED INSIDE THE NATIVE BODIES.
//
// ⭐⭐⭐ WHY. `dragon`'s hottest basic block is `0x1850..0x19e0` — 51 straight-line pairs with no
// internal branch, executed 281,874 times per profile window: **32.4% of every VU1 pair on the
// route**. Its listing (tools/g607_micro.py over a G541 corpus) contains SEVEN scalar-pipe ops:
//
//     0x1858 ERSQRT   0x1860 MFP   0x18d0 RSQRT   0x18e8 MFP
//     0x1940 ERSQRT   0x1948 RSQRT              0x19d8 RSQRT
//
// Every one of them is `G422_LK_NONE` (or carries `G426_PF_PRESTALL`), so `g490BlockPairEligible`
// refuses it, so it TERMINATES the native run, is interpreted one pair at a time through the
// 42 KB `execLower` switch, and then makes the NEXT region entry refuse on `[G612:why] skipQp`.
// One 51-pair loop is therefore executed as ~15 dispatch events instead of one.
//
// The census that prices it (plans/phase-G757-ledger.md §2–§3): the emitted arithmetic is
// **1.47 ns/pair** and 16% of VU1 CPU, while a dispatch-loop iteration is **24 ns** and there are
// 2.26 G of them. Scalar-pipe ops are ~123 k/frame against ~126 k block calls/frame — they are
// what creates the envelope.
//
// ⭐ WHAT MAKES THIS CHEAP: A PUBLICATION INVARIANT THE WRAPPER ESTABLISHES.
//
//     qDelay == 0  ⟹  st.q == g757QPend             (and the same for P)
//
// `G757Sync::enter` MAKES this true by loading the pending slot from the architectural register
// whenever nothing is in flight. Given it, **a Q producer's G239 pre-stall is an unconditional
// `st.q = g757QPend`** — no compare and no branch — and it is not merely an optimisation but the
// CORRECT semantics under lazy publication: at a producer the previous pending value must reach
// `st.q` whether its deadline has passed (the observation nobody performed yet) or is still due
// (the pre-stall proper). A conditional form would drop the first case and lose a publication.
// ⛔ Do NOT restate this as a property of `s_vuQPending`. It is not one; see G757_NEVER below for
// the oracle failure that proved it.
//
// ⭐ AND A LAZY DEADLINE REMOVES THE PER-PAIR COUNTER. Instead of decrementing a counter on every
// pair, the body carries the ABSOLUTE PAIR STAMP at which the pending value becomes architectural:
//
//     producer at pair j with latency L   ⟹   deadline = (stampBase + j) + L
//     entry with a countdown of qd > 0    ⟹   deadline = stampBase + qd - 1
//     published  ⟺  currentStamp >= deadline
//     on exit after n pairs               ⟹   qDelay = max(0, deadline - (stampBase + n - 1))
//
// R8 already carries `stampBase + pairsConsumed` at the head of every basic block and `j` is a
// translate-time constant inside it, so `lea rax, [r8 + j]` IS the current stamp and costs nothing
// on the pairs that do not observe the pipeline — which is all of them but the readers. Only a Q
// reader (an upper with `src == 1`) and MFP pay a guard, and that guard is a `cmov`, not a branch.
// This is the same "resolve only when observable" shape BT3's generator uses for Q/CLIP; what was
// missing here was not the idea but its application to the REGION ENTRY, which still refused
// outright on `slot.readsQ`.
//
// EXACTNESS. Every value below is transcribed from the `execLower` arm it replaces
// (vu1_dispatch_and_sync.inc) and from `g239_stage_p` / `g239_wait_p`
// (vu1_helpers_and_tables.inc), operation for operation and in the same evaluation order, so the
// float results are bit-identical and not merely equivalent:
//   * `(den != 0.0f) ? num/den : ±FLT_MAX` is a SELECT on the divide, not a branch around it, so
//     the taken and untaken arms compute the same bits the C++ does. `cmpeqss` against +0.0
//     answers `den == 0.0f` for -0.0 (true, same as C++) and for NaN (false, same as C++).
//   * `(num >= 0.0f)` is `cmpless` with the CONSTANT as the destination (`0 <= num`), so a NaN
//     numerator takes the `-FLT_MAX` arm exactly as the C++ comparison does.
//   * `std::sqrt(std::fabs(x))` is `andps` with `absMask` then `sqrtss`.
//   * the `? 1.0f/x : 0.0f` EFU forms need no second constant: the alternative is +0.0, so
//     `andnps(mask, recip)` IS the select.
// ⚠️ A divide or square root whose result is selected away still executes, so it can set an MXCSR
// STATUS bit (DE/ZE/IE) the interpreter would not have set. Nothing in this runtime reads MXCSR
// status, and the control word (rounding/DAZ/FTZ) is untouched. The whole-state oracles
// (`DC2_G610_VERIFY` / `DC2_G612_VERIFY`) compare VU1State, the G430 ring, VU RAM and both
// scalar-pipe counters, which is the contract this must hold to.
//
// ROLLBACK: `DC2_G757_NO_SCALARPIPE=1` — translation refuses every pair below, so the eligibility
// test, the emitted bodies and the deadline discipline all disappear and the backend is byte-for-
// byte the G756 one. Proof of arm: `DC2_G757_STAT=1` prints `[G757:pipe]`.
// =============================================================================================

namespace
{
    // -----------------------------------------------------------------------------------------
    // §1 THE PIPELINE BLOCK. It lives at the TAIL OF `G430HistBlock` for one reason: R9 already
    // points there in every emitted body (it is the G430 history ring's base), so the pipeline is
    // reachable as a constant displacement and the emitted code needs no extra register — and
    // there is none to give it (§2 of vu1_g610_native_jit.inc assigns all seven).
    // The offsets are asserted against the real type, so a field reorder is a BUILD ERROR.
    // -----------------------------------------------------------------------------------------
    constexpr int32_t G757_OFF_QPEND = 260;   // G430HistBlock::g757QPend
    constexpr int32_t G757_OFF_PPEND = 264;   // ::g757PPend
    constexpr int32_t G757_OFF_ARMED = 268;   // ::g757Armed  (bit 0 = Q armed, bit 1 = P armed)
    constexpr int32_t G757_OFF_QDEAD = 272;   // ::g757QDead
    constexpr int32_t G757_OFF_PDEAD = 280;   // ::g757PDead

    static_assert(offsetof(G430HistBlock, g757QPend) == (size_t)G757_OFF_QPEND, "G757: qPend");
    static_assert(offsetof(G430HistBlock, g757PPend) == (size_t)G757_OFF_PPEND, "G757: pPend");
    static_assert(offsetof(G430HistBlock, g757Armed) == (size_t)G757_OFF_ARMED, "G757: armed");
    static_assert(offsetof(G430HistBlock, g757QDead) == (size_t)G757_OFF_QDEAD, "G757: qDead");
    static_assert(offsetof(G430HistBlock, g757PDead) == (size_t)G757_OFF_PDEAD, "G757: pDead");

    constexpr int32_t G610_OFF_P = 596;       // VU1State::p, the sibling of G610_OFF_Q (592)
    static_assert(offsetof(VU1State, p) == (size_t)G610_OFF_P, "G757: p offset");

    // "Nothing is in flight." No real stamp ever reaches it, so an observation guard's `cmov`
    // never fires and `G757Sync::leave` can tell "nothing was pending and the body armed nothing"
    // apart from "a publication came due".
    //
    // ⛔⛔ THE FIRST REVISION USED THE OPPOSITE SENTINEL (INT64_MIN, "already published") AND THE
    // ORACLE REJECTED IT ON ITS FIRST RUN — `[G610:verify-BAD] q pc=0x0 count=30`, in a block that
    // contains no Q instruction at all. That form leaned on the global invariant
    // `qDelay == 0 ⟹ st.q == s_vuQPending`, which is true of every path the INTERPRETER takes but
    // is not a property anything enforces: a test that assigns `state().q` directly, a state
    // import, or a reset can break it, and then the exit wrote a stale pending value over a live
    // architectural register. The invariant is now ESTABLISHED at entry from the architectural
    // register (`enter` copies `st.q` into `g757QPend` when nothing is pending) instead of
    // assumed, which is what makes the unconditional pre-stall below sound.
    // ⚠️ NOT a sign-extended imm32. A stamp is a CUMULATIVE pair count over the life of the
    // thread — `dragon` runs ~2.9 M pairs per frame, so 2^31 is about eleven seconds away — and a
    // 32-bit "never" would become reachable and then start publishing stale values. WAITQ/WAITP
    // therefore arm it through `mov rax, imm64`, which is two instructions on 0.12% of pairs.
    constexpr int64_t G757_NEVER = INT64_MAX;

    // -----------------------------------------------------------------------------------------
    // §2 ENCODER ADDITIONS. Free functions over `G610Emit` rather than members, so the encoder in
    // vu1_g610_native_jit.inc §5 stays the fixed, audited object it is. Legacy SSE encodings
    // throughout: these ops are ~4.4% of pairs, the VEX form would save one byte, and a scalar
    // `divss`/`sqrtss` has no three-operand advantage to recover.
    // -----------------------------------------------------------------------------------------
    inline void g757Divss(G610Emit &e, unsigned xd, unsigned xs) { e.sseF3R(0x5E, xd, xs); }
    inline void g757Sqrtss(G610Emit &e, unsigned xd, unsigned xs) { e.sseF3R(0x51, xd, xs); }
    inline void g757Mulss(G610Emit &e, unsigned xd, unsigned xs) { e.sseF3R(0x59, xd, xs); }
    inline void g757Addss(G610Emit &e, unsigned xd, unsigned xs) { e.sseF3R(0x58, xd, xs); }
    // cmpss xmm, m32, imm8 — only lane 0 is compared and the upper lanes of `xd` are preserved,
    // which is why every consumer below reads lane 0 only.
    inline void g757CmpssM(G610Emit &e, unsigned xd, unsigned b, int32_t d, uint8_t imm)
    { e.sseF3M(0xC2, xd, b, d); e.u8(imm); }
    inline void g757CmpEqssM(G610Emit &e, unsigned xd, unsigned b, int32_t d)
    { g757CmpssM(e, xd, b, d, 0u); }                      // xd == [b+d]   (ordered; NaN -> false)
    inline void g757CmpLessM(G610Emit &e, unsigned xd, unsigned b, int32_t d)
    { g757CmpssM(e, xd, b, d, 2u); }                      // xd <= [b+d]   (ordered; NaN -> false)
    // andnps/andps/orps/xorps against a 16-byte-ALIGNED constant-pool slot.
    inline void g757AndnpsM(G610Emit &e, unsigned xd, unsigned b, int32_t d) { e.sseM(0x55, xd, b, d); }
    inline void g757AndpsM(G610Emit &e, unsigned xd, unsigned b, int32_t d) { e.sseM(0x54, xd, b, d); }
    inline void g757XorpsM(G610Emit &e, unsigned xd, unsigned b, int32_t d) { e.sseM(0x57, xd, b, d); }
    inline void g757AndnpsR(G610Emit &e, unsigned xd, unsigned xs) { e.sseR(0x55, xd, xs); }
    inline void g757AndpsR(G610Emit &e, unsigned xd, unsigned xs) { e.sseR(0x54, xd, xs); }
    inline void g757OrpsR(G610Emit &e, unsigned xd, unsigned xs) { e.sseR(0x56, xd, xs); }
    inline void g757XorpsR(G610Emit &e, unsigned xd, unsigned xs) { e.sseR(0x57, xd, xs); }
    // movaps xd, xs — a full 16-byte copy, used only to duplicate a compare MASK.
    inline void g757MovapsR(G610Emit &e, unsigned xd, unsigned xs) { e.sseR(0x28, xd, xs); }
    // cmp r64, [b+d]
    inline void g757CmpR64M(G610Emit &e, unsigned r, unsigned b, int32_t d)
    { e.rex(true, r, 0u, b); e.u8(0x3B); e.memD(r, b, d); }
    // mov [b+d], r64
    inline void g757MovSt64(G610Emit &e, unsigned r, unsigned b, int32_t d)
    { e.rex(true, r, 0u, b); e.u8(0x89); e.memD(r, b, d); }
    // mov qword [b+d], INT64_MAX — through RAX, because the value is not a sign-extended imm32
    // and a 32-bit "never" would be reached in seconds (see G757_NEVER).
    inline void g757MovStNever(G610Emit &e, unsigned b, int32_t d)
    { e.movImm64(G610_RAX, (uint64_t)G757_NEVER); g757MovSt64(e, G610_RAX, b, d); }
    // or dword [b+d], imm8  (83 /1 ib)
    inline void g757OrMemImm8(G610Emit &e, unsigned b, int32_t d, uint8_t v)
    { e.rex(false, 0u, 0u, b); e.u8(0x83); e.memD(1u, b, d); e.u8(v); }
    // cmovge r32, [b+d]
    inline void g757CmovgeM(G610Emit &e, unsigned r, unsigned b, int32_t d)
    { e.rex(false, r, 0u, b); e.u8(0x0F); e.u8(0x4D); e.memD(r, b, d); }

    // -----------------------------------------------------------------------------------------
    // §3 ARM + CENSUS
    // -----------------------------------------------------------------------------------------
    bool g757ScalarPipeOn()
    {
        // ⛔ `g200_qstall_enabled()` and `g87_q_latency_enabled()` / `g239_p_pipeline_enabled()`
        // are conjuncts, not tests inside the emitted code: the bodies below TRANSCRIBE the
        // default-on model, so a kill switch must refuse translation rather than change what an
        // already-emitted body means. `g443Fixed` already carries `s_qlatOn` and `s_ppipeOn`; this
        // adds the third, which it does not.
        static const bool s_on =
            !f50_12_vu_env_flag("DC2_G757_NO_SCALARPIPE") &&
            g87_q_latency_enabled() && g239_p_pipeline_enabled() && g200_qstall_enabled();
        return s_on;
    }
    bool g757StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G757_STAT");
        return s_on;
    }
    uint64_t s_g757Emitted = 0ull;      // scalar-pipe pairs compiled into a body
    uint64_t s_g757Bodies = 0ull;       // bodies that carry at least one
    uint64_t s_g757Guards = 0ull;       // Q/P read guards emitted
    uint64_t s_g757Entries = 0ull;      // runtime entries under the deadline discipline

    // ⭐ THE ADMISSION SET, in ONE place. `g490BlockPairEligible`'s test, the pre-pass that decides
    // a body's discipline, and `g757EmitScalarLower`'s switch must agree on every opcode: admit one
    // the emitter does not cover and the translator starts REFUSING blocks, and `[G610:jit]
    // refused` has been 0 since G611. It is exactly `g426LowerIsScalarPipe` (the G239 pre-stall
    // set: WAITQ, DIV/SQRT/RSQRT, WAITP and the eight EFU producers) plus MFP, which is NOT in that
    // set because it observes P without ever publishing early.
    // (XTOP/XITOP are deliberately absent — they read `memory->vif1_regs.top`, which an emitted
    // body has no pointer to, and they are 0.05% of pairs.)
    inline bool g757S2IsScalarPipe(uint8_t s2)
    {
        return s2 != 0xffu && (g426LowerIsScalarPipe(s2) || s2 == 0x64u);
    }
    inline bool g757PairIsScalarPipe(const G410CompiledUpper &pair)
    {
        return g757S2IsScalarPipe(pair.lowerS2);
    }
    inline bool g757IsScalarPipeLower(uint32_t lower)
    {
        if (((lower >> 25u) & 0x7fu) != 0x40u)
            return false;
        return g757S2IsScalarPipe(S2_FUNC(lower));
    }

    // The EFU latency table, transcribed from the `g239_stage_p(..., N)` call sites.
    inline int g757EfuLatency(uint8_t s2)
    {
        switch (s2)
        {
        case 0x70u: return 11;   // ESADD
        case 0x71u: return 18;   // ERSADD
        case 0x72u: return 18;   // ELENG
        case 0x73u: return 24;   // ERLENG
        case 0x76u: return 12;   // ESUM
        case 0x78u: return 12;   // ESQRT
        case 0x79u: return 18;   // ERSQRT
        case 0x7au: return 12;   // ERCPR
        default: return 0;
        }
    }

    // -----------------------------------------------------------------------------------------
    // §4 THE BODIES.
    //
    // Registers: RCX = VU1State*, R8 = stampBase for this basic block, R9 = G430HistBlock*,
    // R11 = constant pool, RAX = the one integer scratch. XMM3/4/5 are the lower slot's temporaries
    // — X0/X1 are the upper's operands and **X2 carries the G746 ACC residency across consecutive
    // FMACs**, so nothing here may touch it.
    // -----------------------------------------------------------------------------------------

    // `st.q = qPending` / `st.p = pPending`, unconditionally. See the publication invariant in the
    // file header: this is the G239 pre-stall, and it needs neither a test nor a branch.
    inline void g757EmitPrestallQRaw(G610Emit &e)
    {
        e.movLd32(G610_RAX, G610_R9, G757_OFF_QPEND);
        e.movSt32(G610_RAX, G610_RCX, G610_OFF_Q);
    }
    inline void g757EmitPrestallPRaw(G610Emit &e)
    {
        e.movLd32(G610_RAX, G610_R9, G757_OFF_PPEND);
        e.movSt32(G610_RAX, G610_RCX, G610_OFF_P);
    }

    // =========================================================================================
    // ⭐⭐⭐ G763 — THE PRE-STALL BELONGS BEFORE THE PARALLEL UPPER SLOT, NOT INSIDE THE LOWER.
    //
    // On VU1 the lower slot's FDIV/EFU stall resolves before EITHER half of the pair executes
    // (PCSX2: `_vuTestLowerStalls` + `_vuTestPipes` precede both), which is why both generic
    // executors run the G239 block ABOVE the upper: `vu1_g435_pair_loop.inc`'s prestall block
    // sits above `g421FastUpper`, and `g490BlockBody` calls `g757GenericPrestall` one statement
    // before it. The emitted bodies did not. §4 of this file is reached through
    // `g610EmitLower`, and EVERY pair site calls that AFTER `g610EmitUpper` — so for a pair whose
    // upper READS Q and whose lower publishes it, the emitted body used the PREVIOUS Q and the
    // publication landed one instruction too late.
    //
    // ⛔ Before G757 this could not happen: `G426_PF_PRESTALL` made the pair ineligible, it was
    // interpreted, and the interpreter's order is correct. Admitting these pairs to the native
    // bodies is what exposed the ordering, and no oracle caught it because
    // `DC2_G610_VERIFY`/`DC2_G612_VERIFY` were run on `ridepod`/`s03`/`georama`/`dragon`, none of
    // which executes a `<Q reader> | WAITQ` pair.
    //
    // MEASURED (G763, `map125`). The mg shadow-volume microprogram normalises its light basis:
    //     0021  nop                    | RSQRT Q, vf00w, vf31x
    //     0022  MULq.x vf02, vf02, Q   | WAITQ
    //     0023  MULq.x vf03, vf03, Q   | RSQRT Q, vf00w, vf31y
    //     0024  MULq.x vf04, vf04, Q   | nop
    // Shipped, `vf02.x` came out `0.721994936 * -0.000765812` — the stale Q left by the previous
    // program, bit for bit — instead of `* 0.891925763`. `vf03.x` and `vf04.x` are correct,
    // because by then the late WAITQ has published. The basis was therefore 0.765054 long instead
    // of 1 with its X component effectively deleted, the per-prism `dot(N, light) < 0` facing
    // test inverted 56 of 180 shadow prisms, and under `COLCLAMP` an inside-out prism's -1
    // cancelled a correct neighbour's +1 and the pixel went black. See plans/phase-G763-ledger.md.
    //
    // ROLLBACK `DC2_G763_NO_PRESTALL_ORDER=1` restores the G757 order exactly: the pre-upper
    // emitter goes silent and the in-switch calls below emit again, so the bytes are the G757
    // body's. It is the CONTROL ARM of this phase's gate.
    // =========================================================================================
    bool g763PrestallFirstOn()
    {
        static const bool s_on = !f50_12_vu_env_flag("DC2_G763_NO_PRESTALL_ORDER");
        return s_on;
    }

    // The forms the lower-slot switch calls. Under the fixed ordering the publication has already
    // been emitted above the upper, so these are silent; the raw emitters stay exactly as G757
    // wrote them and the rollback simply routes back to them.
    inline void g757EmitPrestallQ(G610Emit &e)
    {
        if (!g763PrestallFirstOn())
            g757EmitPrestallQRaw(e);
    }
    inline void g757EmitPrestallP(G610Emit &e)
    {
        if (!g763PrestallFirstOn())
            g757EmitPrestallPRaw(e);
    }

    // Emit this pair's G239 pre-stall, BEFORE its upper slot. The admission test is the same
    // conjunction `g610EmitLower` uses to route a lower into `g757EmitScalarLower`, spelled here
    // so a pair that will NOT be emitted as a scalar-pipe lower cannot publish anything: an
    // emitted publication with no matching lower would be a second defect, not a fix.
    // MFP (0x64) is deliberately absent — it OBSERVES P through its own guard and never publishes
    // early, which is why `g757S2IsScalarPipe` includes it and the G239 pre-stall set does not.
    inline void g763EmitPairPrestall(G610Emit &e, uint8_t kind, uint32_t lower)
    {
        if (!g763PrestallFirstOn() || !e.m_g757Pipe)
            return;
        if (kind != G422_LK_NONE && kind != G422_LK_DIV)
            return;
        if (!g757IsScalarPipeLower(lower))
            return;
        const uint8_t s2 = S2_FUNC(lower);
        if (s2 == 0x3bu || (s2 >= 0x38u && s2 <= 0x3au))
            g757EmitPrestallQRaw(e);                        // WAITQ / DIV / SQRT / RSQRT
        else if (s2 == 0x7bu || s2 == 0x70u || s2 == 0x71u || s2 == 0x72u || s2 == 0x73u ||
                 s2 == 0x76u || s2 == 0x78u || s2 == 0x79u || s2 == 0x7au)
            g757EmitPrestallPRaw(e);                        // WAITP / the eight EFU producers
    }

    // Arm a producer: publish the value and set its deadline to `stamp(j) + latency`.
    inline void g757EmitArmQ(G610Emit &e, unsigned xv, uint32_t j, int latency)
    {
        e.movssSt(xv, G610_R9, G757_OFF_QPEND);
        e.lea64(G610_RAX, G610_R8, (int32_t)j + latency);
        g757MovSt64(e, G610_RAX, G610_R9, G757_OFF_QDEAD);
        g757OrMemImm8(e, G610_R9, G757_OFF_ARMED, 1u);
    }
    inline void g757EmitArmP(G610Emit &e, unsigned xv, uint32_t j, int latency)
    {
        e.movssSt(xv, G610_R9, G757_OFF_PPEND);
        e.lea64(G610_RAX, G610_R8, (int32_t)j + latency);
        g757MovSt64(e, G610_RAX, G610_R9, G757_OFF_PDEAD);
        g757OrMemImm8(e, G610_R9, G757_OFF_ARMED, 2u);
    }

    // ⭐ THE OBSERVATION GUARD, and the only per-reader cost in the whole mechanism:
    //     if (stamp(j) >= deadline) architectural = pending;
    // written as a `cmov` so it is branchless and so repeated execution is idempotent (the value
    // is the same every time). `cmp` sets the flags; the `mov` between it and the `cmovge` does
    // not disturb them.
    inline void g757EmitObserve(G610Emit &e, uint32_t j, int32_t offArch, int32_t offPend,
                                int32_t offDead)
    {
        e.lea64(G610_RAX, G610_R8, (int32_t)j);
        g757CmpR64M(e, G610_RAX, G610_R9, offDead);
        e.movLd32(G610_RAX, G610_RCX, offArch);
        g757CmovgeM(e, G610_RAX, G610_R9, offPend);
        e.movSt32(G610_RAX, G610_RCX, offArch);
        if (g757StatOn())
            ++s_g757Guards;
    }
    inline void g757EmitObserveQ(G610Emit &e, uint32_t j)
    { g757EmitObserve(e, j, G610_OFF_Q, G757_OFF_QPEND, G757_OFF_QDEAD); }
    inline void g757EmitObserveP(G610Emit &e, uint32_t j)
    { g757EmitObserve(e, j, G610_OFF_P, G757_OFF_PPEND, G757_OFF_PDEAD); }

    // `x*x + y*y + z*z`, in the C++ source's own association: ((x*x + y*y) + z*z). Result in X3.
    inline void g757EmitSquaredLen3(G610Emit &e, unsigned is)
    {
        const int32_t base = G610_OFF_VF + (int32_t)(16u * is);
        e.movssLd(G610_X3, G610_RCX, base + 0);
        g757Mulss(e, G610_X3, G610_X3);
        e.movssLd(G610_X4, G610_RCX, base + 4);
        g757Mulss(e, G610_X4, G610_X4);
        g757Addss(e, G610_X3, G610_X4);
        e.movssLd(G610_X4, G610_RCX, base + 8);
        g757Mulss(e, G610_X4, G610_X4);
        g757Addss(e, G610_X3, G610_X4);
    }

    // `(v != 0.0f) ? 1.0f/v : 0.0f` for a value already in X3. The alternative is +0.0f, so the
    // select is ONE `andnps` — no second constant and no blend.
    inline void g757EmitReciprocalOrZero(G610Emit &e)
    {
        g757MovapsR(e, G610_X5, G610_X3);
        g757CmpEqssM(e, G610_X5, G610_R11, G610_C_ZERO);   // X5 = (v == 0) ? ~0 : 0
        e.movssLd(G610_X4, G610_R11, G610_C_ITOF);         // itof[0] is the splat 1.0f
        g757Divss(e, G610_X4, G610_X3);                    // 1.0f / v
        g757AndnpsR(e, G610_X5, G610_X4);                  // ~(v==0) & recip ; else +0.0
        g757MovapsR(e, G610_X3, G610_X5);
    }

    // Emit one scalar-pipe lower. `j` is the pair's index inside the basic block, so `R8 + j` is
    // its absolute stamp. Returns false (with `*why`) when the opcode is not one this covers.
    bool g757EmitScalarLower(G610Emit &e, uint32_t lower, uint32_t j, int *why)
    {
        if (g757StatOn())
            ++s_g757Emitted;
        const uint8_t s2 = S2_FUNC(lower);
        const unsigned it = LIT(lower), is = LIS(lower);
        const unsigned fsf = (unsigned)((lower >> 21) & 0x3u);
        const unsigned ftf = (unsigned)((lower >> 23) & 0x3u);
        const int32_t numOff = G610_OFF_VF + (int32_t)(16u * is) + (int32_t)(4u * fsf);
        const int32_t denOff = G610_OFF_VF + (int32_t)(16u * it) + (int32_t)(4u * ftf);

        switch (s2)
        {
        // ---- Q producers ------------------------------------------------------------------
        case 0x38u:   // DIV Q, VFs[fsf], VFt[ftf]
        {
            // q = (den != 0.0f) ? num/den : ((num >= 0.0f) ? +FLT_MAX : -FLT_MAX)
            g757EmitPrestallQ(e);
            e.movssLd(G610_X3, G610_RCX, numOff);          // num
            e.movssLd(G610_X4, G610_RCX, denOff);          // den
            g757MovapsR(e, G610_X5, G610_X4);
            g757CmpEqssM(e, G610_X5, G610_R11, G610_C_ZERO);   // X5 = (den == 0)
            g757Divss(e, G610_X3, G610_X4);                // X3 = num/den  (selected away if den==0)
            g757XorpsR(e, G610_X4, G610_X4);               // +0.0
            g757CmpLessM(e, G610_X4, G610_RCX, numOff);    // X4 = (0 <= num)  ordered, NaN -> false
            g757AndnpsM(e, G610_X4, G610_R11, G610_C_SIGN);// ~(0<=num) & signbit
            g757XorpsM(e, G610_X4, G610_R11, G610_C_FLTMAX);   // ±FLT_MAX
            g757AndpsR(e, G610_X4, G610_X5);               // alt & (den==0)
            g757AndnpsR(e, G610_X5, G610_X3);              // quotient & ~(den==0)
            g757OrpsR(e, G610_X4, G610_X5);
            g757EmitArmQ(e, G610_X4, j, 7);
            return true;
        }
        case 0x39u:   // SQRT Q, VFt[ftf]   —   q = sqrt(fabs(v))
            g757EmitPrestallQ(e);
            e.movssLd(G610_X3, G610_RCX, denOff);
            g757AndpsM(e, G610_X3, G610_R11, G610_C_ABS);
            g757Sqrtss(e, G610_X3, G610_X3);
            g757EmitArmQ(e, G610_X3, j, 7);
            return true;
        case 0x3au:   // RSQRT Q, VFs[fsf], VFt[ftf]
        {
            // den = sqrt(fabs(vt)); q = (den != 0.0f) ? num/den : +FLT_MAX   — note: no sign test,
            // the interpreter's arm is `std::numeric_limits<float>::max()` flat.
            g757EmitPrestallQ(e);
            e.movssLd(G610_X4, G610_RCX, denOff);
            g757AndpsM(e, G610_X4, G610_R11, G610_C_ABS);
            g757Sqrtss(e, G610_X4, G610_X4);               // den
            e.movssLd(G610_X3, G610_RCX, numOff);          // num
            g757MovapsR(e, G610_X5, G610_X4);
            g757CmpEqssM(e, G610_X5, G610_R11, G610_C_ZERO);   // X5 = (den == 0)
            g757Divss(e, G610_X3, G610_X4);                // X3 = num/den
            g757MovapsR(e, G610_X4, G610_X5);
            g757AndpsM(e, G610_X4, G610_R11, G610_C_FLTMAX);   // (den==0) & +FLT_MAX
            g757AndnpsR(e, G610_X5, G610_X3);              // ~(den==0) & quotient
            g757OrpsR(e, G610_X4, G610_X5);
            g757EmitArmQ(e, G610_X4, j, 13);
            return true;
        }
        case 0x3bu:   // WAITQ — publish and mark nothing in flight
            g757EmitPrestallQ(e);
            g757MovStNever(e, G610_R9, G757_OFF_QDEAD);
            return true;

        // ---- P consumers ------------------------------------------------------------------
        case 0x64u:   // MFP VFt[dest] <- P  (does NOT stall: it observes, it does not publish early)
        {
            const uint8_t dest = (uint8_t)((lower >> 21) & 0xFu);
            // ⚠️ `it == 0` IS REACHABLE and the interpreter does NOT skip it — `applyDest` writes
            // VF0. Emitting nothing is still exact, and the argument is two steps:
            //   * VF0: this pair's `lowerKind` is `G422_LK_NONE`, which falls into the `default`
            //     arm of the `touchesZero` classifier (vu1_g410_compiled_execution.inc), so
            //     `G425_PF_TOUCHES_ZERO` is SET and both paths end the pair by re-pinning VF0 to
            //     (0,0,0,1). The generic path's write is overwritten before anything can read it.
            //   * st.p: skipping the guard only defers a publication, and the only things that
            //     observe `st.p` are another MFP (which emits its own guard) and the region exit
            //     (where `G757Sync::leave` finishes it).
            if (it == 0u)
                return true;
            g757EmitObserveP(e, j);
            e.movssLd(G610_X3, G610_RCX, G610_OFF_P);
            e.shufps(G610_X3, G610_X3, 0x00u);             // splat P into all four lanes
            g730StoreVf(e, G610_X3, it, dest, G610_X4, G610_X5);
            return true;
        }
        case 0x7bu:   // WAITP
            g757EmitPrestallP(e);
            g757MovStNever(e, G610_R9, G757_OFF_PDEAD);
            return true;

        // ---- EFU (P) producers -------------------------------------------------------------
        case 0x70u:   // ESADD   : x*x + y*y + z*z
            g757EmitPrestallP(e);
            g757EmitSquaredLen3(e, is);
            g757EmitArmP(e, G610_X3, j, g757EfuLatency(s2));
            return true;
        case 0x71u:   // ERSADD  : s = |v|²;  (s != 0) ? 1/s : 0
            g757EmitPrestallP(e);
            g757EmitSquaredLen3(e, is);
            g757EmitReciprocalOrZero(e);
            g757EmitArmP(e, G610_X3, j, g757EfuLatency(s2));
            return true;
        case 0x72u:   // ELENG   : sqrt(fabs(s))
            g757EmitPrestallP(e);
            g757EmitSquaredLen3(e, is);
            g757AndpsM(e, G610_X3, G610_R11, G610_C_ABS);
            g757Sqrtss(e, G610_X3, G610_X3);
            g757EmitArmP(e, G610_X3, j, g757EfuLatency(s2));
            return true;
        case 0x73u:   // ERLENG  : len = sqrt(fabs(s)); (len != 0) ? 1/len : 0
            g757EmitPrestallP(e);
            g757EmitSquaredLen3(e, is);
            g757AndpsM(e, G610_X3, G610_R11, G610_C_ABS);
            g757Sqrtss(e, G610_X3, G610_X3);
            g757EmitReciprocalOrZero(e);
            g757EmitArmP(e, G610_X3, j, g757EfuLatency(s2));
            return true;
        case 0x76u:   // ESUM    : (((x + y) + z) + w)
        {
            const int32_t base = G610_OFF_VF + (int32_t)(16u * is);
            g757EmitPrestallP(e);
            e.movssLd(G610_X3, G610_RCX, base + 0);
            e.movssLd(G610_X4, G610_RCX, base + 4);
            g757Addss(e, G610_X3, G610_X4);
            e.movssLd(G610_X4, G610_RCX, base + 8);
            g757Addss(e, G610_X3, G610_X4);
            e.movssLd(G610_X4, G610_RCX, base + 12);
            g757Addss(e, G610_X3, G610_X4);
            g757EmitArmP(e, G610_X3, j, g757EfuLatency(s2));
            return true;
        }
        case 0x78u:   // ESQRT   : sqrt(fabs(VFs[fsf]))
            g757EmitPrestallP(e);
            e.movssLd(G610_X3, G610_RCX, numOff);
            g757AndpsM(e, G610_X3, G610_R11, G610_C_ABS);
            g757Sqrtss(e, G610_X3, G610_X3);
            g757EmitArmP(e, G610_X3, j, g757EfuLatency(s2));
            return true;
        case 0x79u:   // ERSQRT  : root = sqrt(fabs(VFs[fsf])); (root != 0) ? 1/root : 0
            g757EmitPrestallP(e);
            e.movssLd(G610_X3, G610_RCX, numOff);
            g757AndpsM(e, G610_X3, G610_R11, G610_C_ABS);
            g757Sqrtss(e, G610_X3, G610_X3);
            g757EmitReciprocalOrZero(e);
            g757EmitArmP(e, G610_X3, j, g757EfuLatency(s2));
            return true;
        case 0x7au:   // ERCPR   : (v != 0) ? 1/v : 0
            g757EmitPrestallP(e);
            e.movssLd(G610_X3, G610_RCX, numOff);
            g757EmitReciprocalOrZero(e);
            g757EmitArmP(e, G610_X3, j, g757EfuLatency(s2));
            return true;

        default:
            *why = G610_RF_LOWER_KIND;
            return false;
        }
    }

    // -----------------------------------------------------------------------------------------
    // §5 THE WRAPPER SYNC. A body that carries no scalar-pipe pair keeps the G610/G611/G612
    // discipline verbatim — this runs only for the ones that do, which is what makes the change
    // free for every other body.
    // -----------------------------------------------------------------------------------------
    struct G757Sync
    {
        // Entry: countdown -> deadline. `qd == 0` means "already published", and by the
        // publication invariant `st.q == s_vuQPending` there, so G757_PUBLISHED is exact and a
        // reader's `cmov` becomes a value-preserving rewrite.
        static inline void enter(G430HistBlock *hb, const VU1State &st, int qd, int pd,
                                 int64_t stampBase)
        {
            // ⭐ THE INVARIANT IS ESTABLISHED HERE, NOT ASSUMED. When nothing is in flight the
            // pending slot is loaded from the ARCHITECTURAL register, so the unconditional
            // pre-stall a producer emits rewrites the same bits and a stale `s_vuQPending` — from
            // a reset, a state import, or a test that assigned `state().q` directly — cannot leak
            // into `st.q`. That is the exact defect the oracle rejected on this mechanism's first
            // run (`[G610:verify-BAD] q`, in a block with no Q instruction in it).
            hb->g757QPend = (qd > 0) ? s_vuQPending : st.q;
            hb->g757PPend = (pd > 0) ? s_vuPPending : st.p;
            hb->g757QDead = (qd > 0) ? (stampBase + (int64_t)qd - 1) : G757_NEVER;
            hb->g757PDead = (pd > 0) ? (stampBase + (int64_t)pd - 1) : G757_NEVER;
            hb->g757Armed = 0u;
            if (g757StatOn())
                ++s_g757Entries;
        }
        // Exit after `n` pairs: deadline -> countdown, and finish any publication the body did not
        // have a reader for. `lastStamp` is the stamp of the final pair executed.
        static inline void leave(G430HistBlock *hb, VU1State &st, int *qDelay, int *pDelay,
                                 int64_t stampBase, uint32_t n)
        {
            const int64_t lastStamp = stampBase + (int64_t)n - 1;
            // `G757_NEVER` is reachable two ways — nothing was ever in flight, or a WAITQ/WAITP
            // emptied the pipe — and both mean the counter is zero. Writing `st.q = g757QPend`
            // when the deadline has passed is a no-op in the first case (`enter` loaded it FROM
            // `st.q`) and the publication in the second, which is why neither needs a flag.
            // ⛔ `s_vuQPending` DOES: the interpreter only changes it at a PRODUCER, and a body
            // that merely ran a pending publication down, or waited it out, must leave it alone.
            // `[G610:verify-BAD] qpDelay` on a WAITQ block is what proved that the deadline alone
            // cannot carry this.
            if (hb->g757QDead == G757_NEVER)               *qDelay = 0;
            else if (hb->g757QDead <= lastStamp)           { st.q = hb->g757QPend; *qDelay = 0; }
            else                                           *qDelay = (int)(hb->g757QDead - lastStamp);
            if (hb->g757PDead == G757_NEVER)               *pDelay = 0;
            else if (hb->g757PDead <= lastStamp)           { st.p = hb->g757PPend; *pDelay = 0; }
            else                                           *pDelay = (int)(hb->g757PDead - lastStamp);
            if ((hb->g757Armed & 1u) != 0u) s_vuQPending = hb->g757QPend;
            if ((hb->g757Armed & 2u) != 0u) s_vuPPending = hb->g757PPend;
        }
    };

    // -----------------------------------------------------------------------------------------
    // §6 THE GENERIC BODIES — for the BLOCK EXECUTOR, which is the oracle's reference and the
    // `DC2_G610_NO_JIT` rollback path.
    //
    // ⛔⛔⛔ THIS IS WHAT THE ORACLE CAUGHT SECOND, and it is the more dangerous of the two
    // defects. `g490BlockBody`'s lower dispatch ends in `g422FastLower(..., lowerKind, ...)`, and
    // for `G422_LK_NONE` that callee does NOTHING — the kind means "not covered, the pair loop
    // sends it to execLower instead". Before this phase no `G422_LK_NONE` pair could be inside a
    // block, so the omission was unreachable; admitting the scalar pipe made it reachable, and the
    // generic executor SILENTLY SKIPPED every RSQRT, MFP and EFU op. The emitted code was right and
    // the reference was wrong (`[G610:verify-BAD] vf at=6`).
    //
    // Both halves the generic path was missing are below: the G239 pre-stall that
    // `vu1_g435_pair_loop.inc` performs before the upper for these kinds, and the `execLower` arm
    // itself. Transcribed from those two sites statement for statement, including the evaluation
    // order of the EFU sums, so the reference is a reference and not a second implementation.
    // `g200_qstall_enabled()` / `g87_q_latency_enabled()` / `g239_p_pipeline_enabled()` are
    // admission conjuncts of `g757ScalarPipeOn()`, so the arms they gate are taken here
    // unconditionally — no pair reaches this code with any of them off.
    // -----------------------------------------------------------------------------------------
    inline void g757GenericPrestall(VU1State &st, uint8_t s2, int *qd, int *pd)
    {
        const bool waitQ = (s2 == 0x3bu);
        const bool fdivProducer = (s2 >= 0x38u && s2 <= 0x3au);
        if (*qd > 0 && (waitQ || fdivProducer))
        {
            st.q = s_vuQPending;
            *qd = 0;
        }
        const bool waitP = (s2 == 0x7bu);
        const bool efuProducer = (s2 == 0x70u || s2 == 0x71u || s2 == 0x72u || s2 == 0x73u ||
                                  s2 == 0x76u || s2 == 0x78u || s2 == 0x79u || s2 == 0x7au);
        if (*pd > 0 && (waitP || efuProducer))
        {
            st.p = s_vuPPending;
            *pd = 0;
        }
    }

    inline void g757GenericScalarLower(VU1State &st, uint32_t lower, int *qd, int *pd)
    {
        const uint8_t s2 = S2_FUNC(lower);
        const unsigned it = LIT(lower), is = LIS(lower);
        const unsigned fsf = (unsigned)((lower >> 21) & 0x3u);
        const unsigned ftf = (unsigned)((lower >> 23) & 0x3u);
        // `g239_stage_p` reads and writes `s_vuPDelay` directly; the block executor may be carrying
        // the counters in LOCALS (G499/G506), so the staging is spelled out here against `pd`
        // rather than called, and is otherwise line for line the helper in
        // vu1_helpers_and_tables.inc.
        auto stageP = [&](float result, int latency) {
            if (*pd > 0)
                st.p = s_vuPPending;
            s_vuPPending = result;
            *pd = latency;
        };
        switch (s2)
        {
        case 0x38u:   // DIV
        {
            const float num = st.vf[is][fsf];
            const float den = st.vf[it][ftf];
            const float qv = (den != 0.0f)
                ? (num / den)
                : ((num >= 0.0f) ? std::numeric_limits<float>::max()
                                 : -std::numeric_limits<float>::max());
            if (*qd > 0) st.q = s_vuQPending;
            s_vuQPending = qv;
            *qd = 7;
            return;
        }
        case 0x39u:   // SQRT
        {
            const float qv = std::sqrt(std::fabs(st.vf[it][ftf]));
            if (*qd > 0) st.q = s_vuQPending;
            s_vuQPending = qv;
            *qd = 7;
            return;
        }
        case 0x3au:   // RSQRT
        {
            const float num = st.vf[is][fsf];
            const float den = std::sqrt(std::fabs(st.vf[it][ftf]));
            const float qv = (den != 0.0f) ? (num / den) : std::numeric_limits<float>::max();
            if (*qd > 0) st.q = s_vuQPending;
            s_vuQPending = qv;
            *qd = 13;
            return;
        }
        case 0x3bu:   // WAITQ
            if (*qd > 0) { st.q = s_vuQPending; *qd = 0; }
            return;
        case 0x64u:   // MFP
        {
            const uint8_t dest = (uint8_t)((lower >> 21) & 0xFu);
            float result[4] = {st.p, st.p, st.p, st.p};
            for (unsigned lane = 0u; lane < 4u; ++lane)
                if (dest & (0x8u >> lane))
                    st.vf[it][lane] = result[lane];
            return;
        }
        case 0x7bu:   // WAITP
            if (*pd > 0) { st.p = s_vuPPending; *pd = 0; }
            return;
        case 0x70u:   // ESADD
            stageP(st.vf[is][0] * st.vf[is][0] + st.vf[is][1] * st.vf[is][1] +
                   st.vf[is][2] * st.vf[is][2], 11);
            return;
        case 0x71u:   // ERSADD
        {
            const float s = st.vf[is][0] * st.vf[is][0] + st.vf[is][1] * st.vf[is][1] +
                            st.vf[is][2] * st.vf[is][2];
            stageP((s != 0.0f) ? (1.0f / s) : 0.0f, 18);
            return;
        }
        case 0x72u:   // ELENG
        {
            const float s = st.vf[is][0] * st.vf[is][0] + st.vf[is][1] * st.vf[is][1] +
                            st.vf[is][2] * st.vf[is][2];
            stageP(std::sqrt(std::fabs(s)), 18);
            return;
        }
        case 0x73u:   // ERLENG
        {
            const float s = st.vf[is][0] * st.vf[is][0] + st.vf[is][1] * st.vf[is][1] +
                            st.vf[is][2] * st.vf[is][2];
            const float len = std::sqrt(std::fabs(s));
            stageP((len != 0.0f) ? (1.0f / len) : 0.0f, 24);
            return;
        }
        case 0x76u:   // ESUM
            stageP(st.vf[is][0] + st.vf[is][1] + st.vf[is][2] + st.vf[is][3], 12);
            return;
        case 0x78u:   // ESQRT
            stageP(std::sqrt(std::fabs(st.vf[is][fsf])), 12);
            return;
        case 0x79u:   // ERSQRT
        {
            const float root = std::sqrt(std::fabs(st.vf[is][fsf]));
            stageP((root != 0.0f) ? (1.0f / root) : 0.0f, 18);
            return;
        }
        case 0x7au:   // ERCPR
        {
            const float val = st.vf[is][fsf];
            stageP((val != 0.0f) ? (1.0f / val) : 0.0f, 12);
            return;
        }
        default:
            return;   // unreachable: admission and this switch name the same set
        }
    }

    // Does this run of pairs contain one? The generic block executor asks once, at entry, and uses
    // the answer to fall back to a ONE-PAIR Q/P segment (the per-pair form) for the whole block.
    inline bool g757RunHasScalarPipe(const G479FusedPair *fused, uint32_t firstIndex, uint32_t count)
    {
        if (!g757ScalarPipeOn())
            return false;
        for (uint32_t j = 0u; j < count; ++j)
            if (g757PairIsScalarPipe(fused[firstIndex + j].pair))
                return true;
        return false;
    }

    // Printed from the cold rebuild pass, and only when a counter MOVED. A route rebuilds the
    // decode cache thousands of times, so an unconditional line per rebuild would bury the census
    // in its own output — and a line emitted only on the FIRST rebuild reports the state before
    // any body has been translated, which is how this census first read a false `emitted=0`.
    uint64_t s_g757LastEmitted = ~0ull, s_g757LastBodies = ~0ull;
    void g757Report()
    {
        if (s_g757Emitted == s_g757LastEmitted && s_g757Bodies == s_g757LastBodies)
            return;
        s_g757LastEmitted = s_g757Emitted;
        s_g757LastBodies = s_g757Bodies;
        std::fprintf(stderr,
                     "[G757:pipe] emitted=%llu bodies=%llu guards=%llu entries=%llu\n",
                     (unsigned long long)s_g757Emitted, (unsigned long long)s_g757Bodies,
                     (unsigned long long)s_g757Guards, (unsigned long long)s_g757Entries);
        std::fflush(stderr);
    }

}
