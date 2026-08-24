// G490 - WIDENED BLOCK RUN EXECUTOR, on the PROMOTED (fixed + fused + wide-VF0) loop body.
//
// WHY THIS EXISTS, and why it is not G475.
//
// G489's `/MAP` + PC-sample overlay measured the pair loop at ~63 cycles / ~75 retired
// instructions per pair, of which **54.7% is the per-pair ENVELOPE** (pc-oob test, index math,
// record load, upper/lower split, flag extract, sequential-PC / branch fold / pc store / ebit /
// loop condition) and 20.7% is field extraction from words that never change for a given PC.
// 60 FPS needs 1.55x on this thread. The structural answer is to pay the envelope ONCE PER RUN
// of covered pairs instead of once per pair — which is the premise the whole block-translation
// arc rests on. This file prices that premise at the SOURCE level before anything is emitted.
//
// G475 already built a run executor and G478 measured it +0.34 ms/f SLOWER, which the ROADMAP
// asked G490 to understand first. Two things were found:
//
//   1. **The G475 trace copy is STRUCTURALLY UNREACHABLE on the current binary.** Its two
//      selector arms in vu1_upper_opcodes.inc sit BELOW `else if (g443Fixed && g479LeanFetch)`,
//      and `g479LeanFetch` has been default-ON since G479 was promoted. Arming
//      `DC2_G419_AB=vnoptrace` today cannot put the trace into the promoted loop; it can only
//      drop the whole run onto a different, slower copy. This is exactly the G482/G484
//      "structurally unmeasurable lever" class, one arc later.
//   2. Therefore G478's `+0.34` describes a PRE-G479, PRE-G485 body: a non-fused split fetch and
//      four narrow VF0 stores, in both arms. It is not evidence about today's loop.
//
// So this is the re-price the ROADMAP asked for, done the only way it can be done — as a copy of
// the SHIPPED body (DC2_G484_WIDEVF0 1 + DC2_G479_FUSED 1) with the run hook added, so the
// control arm is the promoted default and nothing else differs.
//
// WHAT IS WIDER THAN G475. G475 covered `upperKind == 0x6f` (VNOP) only — 34.5% of MAP-0 pairs,
// and the half of the stream that carries no upper work at all, i.e. the half where the envelope
// is the SMALLEST fraction of the pair. G490 covers **every upper class `g421FastUpper` executes**
// (all `cls != 0`), because the run body can call the same inlined SIMD upper with the same G430
// stamped-history push that the pair loop passes it. That is the class a block translator would
// emit, so this measures the arc's real premise instead of its easiest corner.
//
// EXACTNESS — what the elision rests on. A pair is eligible only when the per-pair machinery this
// loop performs around the two slots is PROVABLY INERT for it (g490BlockPairEligible):
//   * `pairFlags == 0`     -> no E-bit (no latch, no drain) and no I-bit (the lower word is not a
//                             float immediate), so the LOI arm and the ebit test are no-ops.
//   * `!G426_PF_PRESTALL`  -> the G239 scalar-prestall block is guarded by this bit; clear means
//                             the lower is neither a FDIV/EFU producer nor WAITQ/WAITP.
//   * `!G427_PF_FLAGREAD`  -> under the promoted `g430Hist` model the MAC/STATUS line is NOT
//                             advanced per pair; the only per-pair work is `g430ResolveAt` for a
//                             consumer pair. Clear means there is none.
//   * `!G426_PF_HAZ`       -> the G139 pair-hazard save/restore/overlay is guarded by this bit.
//   * lower is a covered, NON-CONTROL, NON-FLAG kind -> `g422FastLower` executes it, and G489
//                             established from the complete `m_state.pc =` set in the source that
//                             only the ten branch/jump lowers can write pc. So `postExecPc ==
//                             currentPc`, the branch fold is dead, and the PC is sequential.
//   * upper is VNOP (the loop skips it) or `cls != 0` (`g421FastUpper` returns true).
// What remains per pair is then exactly: Q advance, P advance, upper, lower, VF0/VI0 re-pin,
// pc += 8 — which is what the block below executes, in that order.
//
// Verify: DC2_G490_VERIFY=1 re-derives every one of those predicates from the LIVE code words for
//         every pair of every executed block and aborts on the first disagreement (it also
//         re-reads the cached source, so a missed invalidation inside a block cannot hide).
// Stats:  DC2_G490_STAT=1 -> `[G490:block] runs= pairs= avg= cover=`.
// Arm:    DC2_G419_AB=blockrun (paired, hold 16 — it is a cache-decode arm) or DC2_G490_BLOCK=1.
extern int g490BlockRunArm();

// G499 - THE BLOCK EXECUTOR'S LOWER DISPATCH, brought level with the pair loop's.
//
// WHY. `g490ExecuteBlock` below calls `g422FastLower` for EVERY pair of every run. The pair loop
// has not done that since G480/G481: `DC2_G422_RUN_LOWER` (vu1_upper_opcodes.inc) skips the call
// outright when the kind is NOP — the callee's whole body for that case is `return;` — and runs
// the hot INTEGER kinds inline. Both are promoted default-ON, and neither ever reached this copy;
// G490 built the block around the ENVELOPE and inherited the pre-G480 dispatch for the two slots.
//
// SIZE (this session's `DC2_G421_CENSUS=1` on MAP-0, 1.18 G pairs):
//     lowerNop        = 39.5% of all pairs          <- pays a 5-argument call to do nothing
//     IADDI/IADDIU/IAND = 6.99 + 3.07 + 3.02 = 13.1% <- pays a call for three integer instructions
//     [G490:block] cover = 77.0%, avg run 7.81
// Every one of those kinds is block-eligible (NOP is covered, names no register, cannot set the
// hazard/prestall/flagread bits), so essentially all of that population runs THROUGH this
// function. G481's own measurement is why only the three INTEGER kinds are inlined here and the
// memory kinds (LQ/LQI/SQI, 16-byte staging + two memcpys) are left behind the call: inlining all
// seven read +0.50 ms/f in the pair loop. The same law applies to this body.
//
// EXACTNESS. `G481_LOWER_BODY_*` are the SAME tokens `g422FastLower`'s own cases expand
// (vu1_g422_fast_lower.inc:368/449/452), so an inlined kind executes the identical statements in
// the identical order; and skipping the call for NOP is skipping a function whose body is
// `case G422_LK_NOP: return;`. Nothing is re-ordered and no state is touched that was not touched
// before. The two arms are ONE template body, so they cannot drift.
//
// Arm:    DC2_G419_AB=blklower (paired, hold 16 — thread-level code shape, like blockrun)
// Kill:   DC2_G499_NO_BLKLOWER=1
// Stats:  DC2_G499_STAT=1 -> `[G499:blk] arm=<0|1|2>` one-shot proof of selection per arm.
//
// The SECOND mechanism this phase adds to the same body (the Q/P counters as block locals) is
// gated separately — DC2_G419_AB=qpblock, kill DC2_G499_NO_QPLOCAL=1 — so each is priced alone.
// Its exactness argument is at the template body below.
extern int g499BlkLowerArm();
extern int g499QpBlockArm();

// G500 - LET A BLOCK RUN END ON A CONTROL TRANSFER (and swallow the delay slot).
//
// WHY. `g490BlockPairEligible` excludes the ten branch/jump lowers outright, so on this route
// **8.88% of all pairs** (B 0.38 + BAL 0.82 + JR 0.86 + IBEQ 2.40 + IBNE 4.41 + IBLEZ 0.01, re-taken
// this session and identical to G499's) are executed by the pair loop at the full per-pair envelope
// AND terminate the run they end — G499 §2 put control transfer at ~69% of all run terminations,
// which is why the average run is 7.81 and not longer. The delay slot of a TAKEN branch is excluded
// too, for a second reason: the block hook is guarded by `pendingBranchTarget == NO_BRANCH`, so the
// one pair that consumes a pending target can never start a block however eligible it is.
//
// WHAT THE ARM DOES. A run that would ALREADY have formed a block may execute up to two more pairs:
// the control transfer that ended it, and (when that slot is itself block-eligible) its delay slot.
// Nothing else changes — the arm never CREATES a block, never merges two runs and never lengthens
// the plain part, so the number of block calls per frame is invariant and the gate prices exactly
// one thing: pairs moved out of the pair loop into a block that was going to run anyway.
//
// EXACTNESS. The tail pair executes the same statements in the same order as a plain block pair
// (Q advance, P advance, upper, lower, VF0/VI0 re-pin) with two additions, both of which reproduce
// the pair loop's own protocol verbatim:
//   * `st.pc` is written to the tail pair's own PC before its lower runs, because the branch bodies
//     derive their target from it and BAL/JALR write `(st.pc + 16) / 8` into VI. The pair loop has
//     that value live for every pair; the block writes it once.
//   * the branch OUTCOME is read back exactly as the pair loop's tail reads it — `st.pc` different
//     from the tail pair's PC means taken, and the target is `st.pc + 8` with the same wrap. That
//     is the identical test, so even the degenerate `B 0` encoding (target == the delay slot, which
//     the pair loop treats as not-taken) behaves the same in both arms.
// The delay slot, when it is taken into the block, is a plain eligible pair by construction, and
// nothing in `g421FastUpper` or the non-branch `g422FastLower` kinds reads `st.pc`, so the branch's
// write to it cannot be observed before the caller consumes it.
//
// Arm:    DC2_G419_AB=blkbranch (paired, hold 16 — thread-level code shape, like blockrun/blklower)
// Kill:   DC2_G500_NO_BLKBRANCH=1
// Stats:  DC2_G500_STAT=1 -> `[G500:inv] arm=<0|1>` per-arm workload invariant (blocks/f, pairs,
//         avg run, tail1/tail2/taken counts) printed from the SAME run, plus `[G499:blk] arm=3`.
// Verify: DC2_G490_VERIFY=1 re-derives the tail predicate from the live code words as well.
extern int g500BlkBranchArm();

// G502 - WHAT A BLOCK COSTS TO ENTER, AND WHAT IS LEFT OUTSIDE IT.
//
// Two questions this arc has never answered, both about the SAME loop, both answered by the census
// and the ceiling probe below BEFORE any lever is written (G497 §2 / G500 §8):
//
//  1. THE ROUND TRIP. `cover=88.99%` at `avg=9.15` means ~157 k block calls per frame, and every one
//     of them costs a full trip round the pair loop's top of iteration (pc-oob test, fused record
//     load, the pending/ebit gate, the run-length load, the budget clamp, the MIN_RUN test, the
//     G500 tail lookup, the noinline 10-argument call, the next-pc fold, the cycle update) before
//     the first pair of the next block executes. Nobody has priced ONE such trip, so nobody knows
//     what a lever that CHAINS blocks (executes the next run without returning to run()) could be
//     worth. `DC2_G419_AB=blksplit` prices it directly: the candidate arm HALVES every block whose
//     budget-clamped length is >= 4, which leaves the executed pairs, their order and the coverage
//     bit-identical and adds exactly ONE extra round trip per split block. Frame delta divided by
//     the census's `split/f` IS the price of a round trip. It is an INFLATION probe, so it carries
//     none of G492's producer-edge deletion hazard: nothing downstream can derive different work
//     from it, and `[G500:inv] pairs/run` + `cover` are the per-arm invariant.
//
//  2. THE REMAINDER. The roadmap's task 2 — the ~9.4% of pairs that are neither block-eligible nor
//     control transfers — has been "unpriced because nobody has counted it" for two phases. A
//     STATIC slot tally in g490RebuildRunLengths would not answer it (the hot loop's slots execute
//     orders of magnitude more often than cold ones), so this census counts the reason DYNAMICALLY,
//     once per pair the PAIR LOOP executes, in the order g490BlockPairEligible tests them.
//
// Both halves are split by KICK KIND (G501 §3b: resume kicks are 29% of kicks but 58% of all pairs
// and their pairs cost 25% less), which is the roadmap's task 3.
//
// Census: DC2_G502_CENSUS=1 -> `[G502:blk]` / `[G502:excl]`. Never a timing arm — it puts one
//         out-of-line call on every plain pair.
// Probe:  DC2_G419_AB=blksplit (paired, hold 16), or DC2_G502_SPLIT=1 standalone.
// G502 §5 — WHAT THE CENSUS AND THE PRICE TOGETHER SELECTED: `blktail0`.
//
// `[G502:excl]` splits the remainder that has been called "the ~9.4% G139-hazard / short-run
// bucket" for two phases. It is not that. Weighted by each population's share of ALL pairs
// (geometry 41%, resume 59%):
//     branch   2.10%   control transfers the pair loop still executes
//     pending  2.05%   their delay slots
//     short    1.43%   an eligible run of exactly ONE pair
//     G139 haz 0.49%   <- a FIFTH of what the framing implied
//     eibit    0.32%   prestall 0.06%   ebit 0.005%   budget 0.00%
// (plus lknone 2.08% and flaglow 2.54%, both already priced and both NO-GO.)
//
// G500 admits a control transfer as a block TAIL only when the run before it already reached
// MIN_RUN, so `branch` + `pending` = **4.15% of all pairs, ~67 k/frame**, are exactly the
// population G500 measured at **7.0 ns/pair** (its −1.464 ms/f over 208.6 k tail pairs/frame) and
// they still pay the full per-pair envelope. `blktail0` admits a block on `count + tail` instead of
// on `count`, so a lone control transfer — with no eligible pair in front of it at all — forms a
// block of its own and swallows its delay slot. Ceiling: ~0.35 ms/f gross against ~34 k extra
// round trips at 1.89 ns = 0.064 ms/f, i.e. **~0.29 ms/f net**. Nothing else on the board prices
// above the floor.
//
// EXACTNESS: none. There is no new code in the block body and no new predicate — `g500TailEligible`
// already decides admissibility, `g490BlockBody` already runs `for (j = 0; j < count; ++j)` (which
// is simply empty when count is 0) and the caller's branch-outcome fold already handles
// `g500BPc == currentPc`. The oracle that verified G500's tails verifies these unchanged.
//
// Arm:    DC2_G419_AB=blktail0 (paired, hold 16)
// Kill:   DC2_G502_NO_BLKTAIL0=1
extern int g502BlkSplitArm();
extern int g502BlkChainArm();
extern int g502BlkTail0Arm();

// G504: the ring-cursor arm (DC2_G419_AB=histcur, rollback DC2_G504_NO_HISTCUR=1).
extern int g504HistCurArm();

// G506: the segmented Q/P-advance arm (DC2_G419_AB=qpseg, rollback DC2_G506_NO_QPSEG=1).
extern int g506QpSegArm();

// G514: the whole-vector VU-data-memory store arm (DC2_G419_AB=vstore, rollback
// DC2_G514_NO_VSTORE=1, oracle DC2_G514_VERIFY=1) and its LOAD-side sibling
// (DC2_G419_AB=vload, rollback DC2_G514_NO_VLOAD=1, same oracle).
extern int g514VStoreArm();
extern int g514VLoadArm();
// G514 promotion gate: arm 10 (shipped) vs arm 7 (pre-G514). DC2_G419_AB=vmem.
extern int g514VMemArm();

// G515: the NOP peel arm (DC2_G419_AB=nopdisp, rollback DC2_G515_NO_NOPPEEL=1). Arm 12 against the
// G514 shipped arm 10.
extern int g515NopPeelArm();

// G516: the three decode-time-dispatch arms (DC2_G419_AB=updesc / lowdense / dispdense, rollback
// DC2_G516_NO_DISPDENSE=1). Arms 13 / 14 / 15 against the G515 shipped arm 12.
extern int g516UpDescArm();
extern int g516LowDenseArm();
extern int g516DispDenseArm();
extern int g516Peel4Arm();
extern int g516DispArm();

// G517: the pair-loop strength-reduction arms (DC2_G419_AB=fpind / stampind / g517ind, rollback
// DC2_G517_NO_IND=1). Arms 19 / 20 / 21 against the G516 shipped arm 18.
extern int g517PtrIndArm();
extern int g517StampIndArm();
extern int g517IndArm();
extern int g531FlagTailArm();
extern int g533ImageSpecArm();
extern int g547Hot0c70Arm();
extern int g548Hot0e00Arm();
extern int g549Hot30d8Arm();
extern int g550Hot1be8Arm();
extern int g551Hot1c00Arm();
extern int g554Hot0900Arm();
extern int g555Hot1110Arm();
#ifndef DC2_G532_FLAGHEAD
// G532a REFUTED: four Georama gates were 4/4 slower. Keep the source for audit/reproduction,
// but compile every arm, oracle, wrapper, and pair-loop hook out of the shipping binary.
#define DC2_G532_FLAGHEAD 0
#endif
#if DC2_G532_FLAGHEAD
extern int g532FlagHeadArm();
#endif

// G516 3 — THE UPPER-DESCRIPTOR FAMILY IS REFUTED AND COMPILED OUT (`DC2_G516_UPDESC 0`).
//
// `updesc` resolved `g421DescTableConst.e[upperKind]` at decode time into a per-pair array so the
// descriptor's address depended only on the pair index. Four paired A/Bs against the G515 shipped
// body: **+1.980 ms/f (+9.9%), 0/4**, and `dispdense` (updesc + lowdense together) **+1.737, 0/4**
// — bigger alone than combined, because the two shared one 8-byte record load.
//
// A load whose target is a SMALL PERMANENTLY-RESIDENT CONSTANT TABLE is two L1 hits, not a
// dependency chain: `g421DescTableConst` is 1 KB of .rdata read by every non-VNOP block pair.
// Replacing it with a per-item array trades that hot table for a cold stream on a working set
// (`fused`, 16 B x 2,048 = 32 KB) that is already the whole of L1D. Kept, guarded, never
// instantiated, so the measurement is reproducible by setting this to 1.
#ifndef DC2_G516_UPDESC
#define DC2_G516_UPDESC 0
#endif

// G517 — THE PAIR-LOOP STRENGTH REDUCTION IS REFUTED AND COMPILED OUT (`DC2_G517_INDARM 0`).
//
// The re-taken attribution put 9.4% of the VU1 worker on address arithmetic that is a pure function
// of `j`, with every input SPILLED (29 stack references in the leaf's first 0x700). Iterating
// pointers and a running stamp deleted ~7 instructions per pair and 5 of those 29 references — and
// bought NOTHING, measured with the pole forced onto the VU1 worker so the conversion was ~1.0:
//
//   fpind    (arm 19, the two pointers)  -0.111 / -0.153 / -0.279 / +0.159 / -0.018 / +0.093
//                                        => mean -0.052, 4/6, SIGN-INCONSISTENT = under the floor
//   stampind (arm 20, the running stamp) +0.151 / +0.399 / +0.683 => +0.411, 0/3, MONOTONE WORSE
//   g517ind  (arm 21, both)              +0.261 / +0.005 / +0.286 / +0.128 => +0.170, 0/4
//
// ⭐ A SPILL RELOAD IS NOT A LIVE VALUE. G483 named live-value pressure as this loop's binding
// resource and every win since has come from reducing it; a *reload* of a spilled value is an L1
// hit the out-of-order core hides, exactly like G516's `updesc` constant table. Deleting the
// reloads while adding an induction variable is a wash at best, and the STAMP lost outright
// because MSVC spilled it to `[rbp-0x71]` and wrote it back on every pair.
//
// Kept, guarded, never instantiated, so the shipped `g490BlockBody` is byte-identical to G516's and
// the measurement reproduces by setting this to 1.
#ifndef DC2_G517_INDARM
#define DC2_G517_INDARM 0
#endif

// G499 §2 REFUTED and COMPILED OUT (`DC2_G499_QPLOCAL 0`). Four paired A/Bs on the same binary,
// both arms confirmed built on every run (`[G499:blk] arm=1` and `arm=2`), read
// **-0.050 / +0.191 / -0.057 / -0.031 ms/f: mean +0.013, signs disagree, |t| <= 1.14, blocked
// -0.010 / +0.186 / -0.059 / -0.017.** That is law 14's neutral, not a small win. The two memory
// operands it deletes per pair are L1-resident and the branch on them is perfectly predicted, so
// the out-of-order engine was already hiding them; the two extra loop-live ints are not free even
// in a leaf function with its own register file. **This is the SIXTH confirmation of the
// G427/G442/G479/G483/G485/G486 live-value law, and the first one taken OUTSIDE run()'s pair
// loop** — which is what makes it worth keeping: G479's `QPREG` refutation could be dismissed as a
// property of a loop already at ~1.2 IPC with 36 spills, and this one cannot.
//
// The scaffolding is kept (the template parameter, the arm, the flags) so ONE edit re-instantiates
// it, exactly like DC2_G485_HAZREG / DC2_G489_PCSKIP. Set the #define to 1 to re-arm; do not do so
// until something has changed what brackets those two counters.
#ifndef DC2_G499_QPLOCAL
#define DC2_G499_QPLOCAL 0
#endif

// G500: the control-transfer tail body. Built (a fourth instantiation would be a THIRD live one, so
// `qpblock` stays compiled out beside it — G499 §5c measured an identical source at -0.145 on a link
// carrying one extra 4.9 KB instantiation and -0.515 on the shipped one).
#ifndef DC2_G500_BLKBRANCH
#define DC2_G500_BLKBRANCH 1
#endif

// G502 §4 — THE CHAINED BLOCK BODY IS PRICED OUT AND COMPILED OUT, BY MEASUREMENT.
//
// `[G502:blk]` says 44.30% (geo) / 40.86% (res) of blocks are immediately followed by another one,
// i.e. ~66.8 k of the ~158 k block round trips per frame buy nothing. The `blksplit` ceiling probe
// then priced ONE round trip: **+0.360 ms/f mean over four paired runs, sign 4/4, for 190,653 extra
// round trips per frame ⇒ 1.89 ns each** — about seven host cycles for run()'s whole top of
// iteration PLUS a noinline 10-argument call. So the chain's ceiling is
// **66,800 × 1.89 ns = 0.126 ms/f**, and the chain does not even delete a whole round trip (it
// still evaluates the next-pc fold, the run-length load and the tail lookup inside the leaf), so
// the realistic figure is under 0.09. G501's own `blkpin` read −0.167 and was called "AT the
// instrument floor". **Below the floor: do not build it.**
//
// The scaffolding stays — the template parameter, the chain step, the wrapper, the arm accessor and
// the lever id — so ONE edit re-arms it, exactly like DC2_G479_QPREG / DC2_G485_HAZREG /
// DC2_G489_PCSKIP / DC2_G499_QPLOCAL. Do not set this to 1 until something has changed what a block
// round trip costs.
#ifndef DC2_G502_BLKCHAIN
#define DC2_G502_BLKCHAIN 0
#endif

// G504 — THE G430 RING CURSOR AS A BLOCK LOCAL.
//
// The first instruction-level attribution of this thread in the whole arc (G504 §3: `[G483:rawpc]`
// resolved against a linker map this phase made every link emit) puts the SINGLE HOTTEST
// instruction of the VU1 worker inside `g430PushPending`, and the push as a whole at ~9.2% of the
// thread. The mechanism is not instruction count — it is that consecutive pushes are chained
// through MEMORY (`H.head` is loaded, four stores are indexed off it, and it is stored back), so
// each push waits on the previous one's store to forward. Full argument and the exactness proof at
// `g430PushPendingAt` in vu1_g421_fast_upper.inc.
//
// G504 §4d — REFUTED over four paired runs, and COMPILED OUT. See the fix log for the numbers.
//
// The compiler did most of what was asked: `dumpbin` on the candidate shows the `hb` reload and the
// `H.head` load GONE, the ring index carried in `r15`, and the four store addresses no longer
// waiting on a load. It then **mirrored the cursor to a stack slot anyway** (`mov ecx,[rbp-0x60]` /
// `mov [rbp-0x60],edx` on every push), computing the wrap twice — so the store→load round trip
// survived, relocated from the TLS block to the stack frame, and the leaf's frame grew from
// `sub rsp,0x70` to `sub rsp,0x80`.
//
// SEVENTH confirmation of the live-value law (G427/G442/G479/G483/G485/G486), and the first one
// against a mechanism predicted to ESCAPE it: a loop-carried store→load dependence is the one class
// an out-of-order core cannot hide, and G485's `widevf0` — the only previous lever of that class
// here — won. It does not generalise. In a leaf that already spills six loop-invariant values per
// pair because it CALLS `g422FastLower`, taking one value out of memory only moves the spill.
//
// The scaffolding stays — the template parameter, `g430PushPendingAt`, `g504ExecuteBlockTailCur`,
// arm 6 and the lever id — so ONE edit re-arms it, exactly like DC2_G499_QPLOCAL / DC2_G502_BLKCHAIN
// / DC2_G502_BLKTAIL0. Do not set this to 1 again until something has changed what forces the spill
// (i.e. until the block stops calling out of line).
#ifndef DC2_G504_HISTCUR
#define DC2_G504_HISTCUR 0
#endif

// G504: the ring-cursor ORACLE, compiled out by default and re-armed with ONE edit — the
// `DC2_G490_VERIFY_BLOCK` convention, not a runtime flag, and for a measured reason. Holding the
// entry cursor live across the whole block for a cold comparison is an extra LIVE VALUE in the one
// loop this arc has proved binds on exactly that (G483): with the check compiled in, MSVC grew the
// leaf's frame from `sub rsp,0x70` to `sub rsp,0xa0` and kept the cursor in BOTH a register and a
// stack slot. It has already answered — **zero `[G504:verify-BAD]` over 513,437,981 block runs and
// 4.7 G pairs**, with `[G490:block] cover=88.99% avg=9.15` unchanged — so the shipped body carries
// none of it. Set to 1 (and rebuild) to re-verify after any change to what a block may execute.
#ifndef DC2_G504_VERIFY_CUR
#define DC2_G504_VERIFY_CUR 0
#endif

// G506 — THE Q/P DELAY ADVANCE, HOISTED OUT OF THE PER-PAIR LOOP BY SEGMENTATION.
//
// WHAT IS BEING REMOVED. The shipped body (`<1,0,1,1,0,0>`, i.e. G499QpLocal FALSE) runs this on
// EVERY pair, through the two POINTER parameters:
//
//     if (*qDelay > 0 && --(*qDelay) == 0) st.q = s_vuQPending;
//     if (*pDelay > 0 && --(*pDelay) == 0) st.p = s_vuPPending;
//
// `tools/g504_disasm.py` on the shipped leaf shows what that costs at the top of the loop
// (+0xc0..+0xf0): the `qDelay` pointer is RELOADED from its stack home on every iteration
// (`mov r8, qword ptr [rbp+0x68]`, because r8 is needed for the fused base first), then a load, a
// test, a branch, a decrement, a store and a second branch — twice. Two read-modify-writes to the
// same two words on every one of ~1.45 M block pairs per frame, plus a register the loop body then
// does not have.
//
// WHY IT IS PROVABLY DEAD WORK INSIDE A BLOCK. Both counters are ARMED only by scalar-pipe lowers:
// `s_vuQDelay` by DIV / SQRT / RSQRT (7/7/13) and WAITQ, in `execLower`; `s_vuPDelay` by
// `g239_stage_p` (the eight EFU producers) and `g239_wait_p` (WAITP), and by nothing else in the
// runtime — the complete writer set is enumerable and was enumerated (`s_vuQDelay =` / `s_vuPDelay =`
// have exactly these call sites). `g426LowerIsScalarPipe` marks precisely that opcode set
// (0x38-0x3a, 0x3b, 0x70-0x73, 0x76, 0x78-0x7a, 0x7b) as `G426_PF_PRESTALL`, and block eligibility
// REQUIRES that bit clear. No upper slot can reach either counter. Therefore, inside a block, both
// counters can only COUNT DOWN — the sequence of values a pair sees is a pure function of the entry
// value and the pair index, and the only observable effect is the single publication of `st.q` /
// `st.p` at the pair where the counter reaches zero.
//
// THE TRANSFORMATION. Split the pair run at those (at most two) publication points instead of
// testing for them per pair. Each segment executes with NO counter code at all; the publication
// happens once, between segments. The pair body appears ONCE in the source, so this adds an outer
// `while` and no second copy of the 13 KB leaf. A counter with value v at pair j publishes before
// pair j+v-1, which is exactly what the loop below computes.
//
// It also subsumes G499's `qpblock` (arm 2, refuted at +0.013 and compiled out): the counters must
// live in block locals for the segmentation to be expressible at all. That refutation is not
// evidence against this one — `qpblock` kept the per-pair decrement and only moved WHERE it read
// from, which is the live-value law's own subject. This deletes the per-pair work outright.
//
// Arm:    DC2_G419_AB=qpseg (paired, hold 16 — thread-level code shape). Control arm is 3, the
//         G500/G501 shipped body, so both arms execute the identical pairs in the identical blocks.
// Kill:   DC2_G506_NO_QPSEG=1
// Oracle: DC2_G506_VERIFY=1 — caller-side, once per BLOCK: after the block returns, `*qDelay` and
//         `*pDelay` must equal `max(entry - pairsConsumed, 0)`. A lower that re-armed a counter
//         inside the block would leave 7, 13 or 18 there instead, and an arithmetic error in the
//         segmentation would leave anything else. Costs nothing on the default path (one memoised
//         bool tested once per block) and nothing inside the leaf.
#ifndef DC2_G506_QPSEG
#define DC2_G506_QPSEG 1
#endif

// G502 §6 — `blktail0` IS REFUTED, over EIGHT consecutive paired runs on two links.
//
// The lever: admit a block on `count + tail` instead of on `count`, so a control transfer with
// fewer than MIN_RUN eligible pairs in front of it forms a block of its own and swallows its delay
// slot. §2 prices that population at 4.15% of all pairs and G500 measured exactly this population
// at 7.0 ns/pair, so it was pre-priced at −0.35 ms/f. It measured
// **+0.121 (4/4 slower) on the first link and +0.082 (4/4 slower) on the second** — the second
// carrying the pre-joined `g502TailAt` sidecar, which cut the penalty by 0.039 and did not flip it.
// Coverage did what it was supposed to (89.00% → 93.31%, +70 k pairs/frame off the pair loop) and
// the frame still got worse.
//
// WHY, and this is the durable half. **A block does not save the pair-loop iteration it is ENTERED
// from.** Moving L pairs into a NEW block saves `L × (per-pair envelope)` and costs one round trip;
// moving them into an EXISTING one costs nothing. G500's 7.0 ns/pair was measured on tail pairs
// that joined blocks **already being entered** — it never included an entry. Here every 2-pair
// block is a new entry, so the sum is `2 × envelope − 1.89 ns`, and the measurement says that is
// **≤ 0**: a branch pair executed by the pair loop costs about what one executed inside a block
// costs. Same law as G500 §8 (an average marginal rate does not price a selected population),
// firing in the pessimistic direction for the first time.
//
// Scaffolding kept, compiled out — DC2_G479_QPREG / DC2_G485_HAZREG / DC2_G489_PCSKIP convention.
#ifndef DC2_G502_BLKTAIL0
#define DC2_G502_BLKTAIL0 0
#endif

// G502 §3 — the round-trip ceiling probe has ANSWERED (1.89 ns per block round trip) and is
// compiled out with it: a default-off lever still costs the default path a test on every hook
// entry (Aggressive Performance Policy). Re-arm with one edit when something changes what a block
// entry costs.
#ifndef DC2_G502_SPLIT_PROBE
#define DC2_G502_SPLIT_PROBE 0
#endif

// G610: the three stage-deletion ceiling instantiations of the block leaf.
//
// ⛔⛔ COMPILED OUT, AND THE PROBE IS STRUCTURALLY INVALID — not merely spent. Deleting a slot from
// an interpreter pair changes the microprogram's own trip counts, because the deleted slot writes
// its loop induction variables: the no-lower arm took `[G483:runprof] pairs/call` from **1379 to
// 61242** and the no-both arm deadlocked the two worker threads (`gsStallMs/f = 682.2` of a 693.4
// "busy"). It is kept in source as the RECORD of that finding and must never be quoted as a price.
// Compiling it out also removes its three tests from `run()`'s hook, and `run()`'s text is a shared
// resource (G490 §5 measured 6,196 B of never-selected copy at 1.35 ms/f).
#ifndef DC2_G610_CEIL_ARMS
#define DC2_G610_CEIL_ARMS 0
#endif

namespace
{
    // G610: `DC2_G610_CEIL` = 1 (no lower) / 2 (no upper) / 3 (neither). Read ONCE, at the first
    // call, and held for the process — it selects a whole leaf copy, never a per-pair test.
    int g610CeilMode()
    {
#if DC2_G610_CEIL_ARMS
        static const int s_mode = [] {
            const char *v = std::getenv("DC2_G610_CEIL");
            if (v == nullptr || v[0] < '1' || v[0] > '3')
                return 0;
            return static_cast<int>(v[0] - '0');
        }();
        return s_mode;
#else
        return 0;
#endif
    }

    // Minimum run length worth the call boundary. 2 mirrors G475 so the two are comparable.
    constexpr uint32_t G490_MIN_RUN = 2u;
#if DC2_G610_JIT_BACKEND
    // G610: defined below, beside the compiled-block entry; called from the END of
    // g490RebuildRunLengths() so a compiled body is discarded and re-emitted by the SAME event that
    // rewrites the pair it was compiled from.
    void g610CompileAll(uint32_t dataSize);
#endif
    // G531 reuses the existing one-byte branch-tail sidecar instead of growing the hot
    // thread_local compiled cache. Values 0/1/2 retain their G500 meaning; 3 is one compiled
    // flag-consumer pair. Control arms sanitize 3 back to zero before entering a block.
    constexpr uint8_t G531_FLAG_TAIL = 3u;

    bool g490BlockRequested()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "blockrun") == 0;
        }();
        if (s_paired)
            return g490BlockRunArm() > 0;
        // PROMOTED default-on (G490): mean -0.560 ms/f raw / -0.523 blocked over four paired
        // A/Bs, sign 4/4, blocked t -2.0..-4.8, both arms confirmed built on every run.
        // Rollback: DC2_G490_NO_BLOCK=1 restores the G485 promoted copy exactly.
        static const bool s_on = !f50_12_vu_env_flag("DC2_G490_NO_BLOCK");
        return s_on;
    }

    bool g490VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G490_VERIFY");
        return s_on;
    }

#if DC2_G506_QPSEG
    // G506 oracle, caller side. Memoised like every other arm accessor (G500 §4: an uncached
    // getenv on a path reached ~158 k times per frame is 8 ms/f).
    bool g506VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G506_VERIFY");
        return s_on;
    }

    // The premise, checked directly: inside a block both scalar-pipe counters can only COUNT DOWN,
    // so after `used` pairs each must hold `max(entry - used, 0)`. Reports both arms, so a
    // disagreement can be attributed to the segmentation (arm 7) or to the premise itself (arm 3,
    // which runs the untouched per-pair form and must satisfy the same identity).
    void g506VerifyBlockQp(int qEntry, int pEntry, uint32_t used, int qNow, int pNow, int arm)
    {
        const int wantQ = static_cast<int>(used) >= qEntry ? 0 : qEntry - static_cast<int>(used);
        const int wantP = static_cast<int>(used) >= pEntry ? 0 : pEntry - static_cast<int>(used);
        static std::atomic<uint64_t> s_blocks{0};
        s_blocks.fetch_add(1u, std::memory_order_relaxed);
        if (qNow == wantQ && pNow == wantP)
            return;
        static std::atomic<uint32_t> s_bad{0};
        const uint32_t n = s_bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 32u)
            std::fprintf(stderr,
                         "[G506:verify-BAD] arm=%d used=%u q(entry=%d now=%d want=%d) "
                         "p(entry=%d now=%d want=%d) n=%u blocks=%llu\n",
                         arm, used, qEntry, qNow, wantQ, pEntry, pNow, wantP, n,
                         static_cast<unsigned long long>(
                             s_blocks.load(std::memory_order_relaxed)));
    }
#endif

    bool g490StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G490_STAT");
        return s_on;
    }

    // G499: which block body run() calls. ONE value, read ONCE per run() (G479's rule — a
    // microprogram must never change dispatch shape mid-execution), so the pair loop carries a
    // single live int instead of one bool per mechanism.
    //   0 = pre-G499 body            (DC2_G499_NO_BLKLOWER=1, and the `blklower` control arm)
    //   1 = + G480/G481 lower dispatch (the `blklower` candidate / the `blkbranch` CONTROL)
    //   2 = + Q/P counters as block locals (REFUTED G499 §5b, compiled out)
    //   3 = + G500 control-transfer TAIL (the `blkbranch` candidate). G501's pinned addressing
    //       is UNCONDITIONAL inside this body, not a fourth arm — it deletes code `g443Fixed`
    //       proves unreachable, so it has no control arm to be compared against.
    //   4 = reserved for the `blksplit` CEILING PROBE, which changes no body at all (it only
    //       shortens the run the pair loop hands to arm 3) and therefore has no body of its own.
    //   5 = + G502 CHAIN (the `blkchain` candidate). Control arm is 3, the G500 shipped body.
    //   6 = + G504 HISTORY CURSOR (the `histcur` candidate): arm 3's body with the G430 ring cursor
    //       carried in a block local instead of in `H.head`. Control arm is 3, so the two arms
    //       execute the identical pairs in the identical blocks and differ only in where four
    //       stores get their index from.
    //  23 = + G531 FLAG TAIL: arm 18's shipped body absorbs one compiled flag consumer after an
    //       existing ordinary run. 24 is its standalone oracle copy.
    //  25 = + G532 FLAG HEAD: arm 23 also absorbs a flag consumer immediately before an existing
    //       ordinary block. 26 is its standalone oracle copy.
    //  27 = + G533 immutable-image specialization: exact-source static handler for one recurring
    //       nine-pair block plus its typed flag tail. 28 is its full-state/history oracle copy;
    //       29 is the isolated occupancy-census copy.
    int g499BlkArmRun()
    {
        static const int s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            if (ab == nullptr)
                return 0;
            if (std::strcmp(ab, "blklower") == 0)
                return 1;
            if (std::strcmp(ab, "qpblock") == 0)
                return 2;
            if (std::strcmp(ab, "blkbranch") == 0)
                return 3;
            if (std::strcmp(ab, "blkchain") == 0)
                return 5;
            if (std::strcmp(ab, "histcur") == 0)
                return 6;
            if (std::strcmp(ab, "qpseg") == 0)
                return 7;
            if (std::strcmp(ab, "vstore") == 0)
                return 8;
            if (std::strcmp(ab, "vload") == 0)
                return 10;
            if (std::strcmp(ab, "vmem") == 0)
                return 11;
            if (std::strcmp(ab, "nopdisp") == 0)
                return 12;
#if DC2_G516_UPDESC
            if (std::strcmp(ab, "updesc") == 0)
                return 13;
            if (std::strcmp(ab, "dispdense") == 0)
                return 15;
#endif
            if (std::strcmp(ab, "lowdense") == 0)
                return 14;
            if (std::strcmp(ab, "peel4") == 0)
                return 17;
            if (std::strcmp(ab, "g516disp") == 0)
                return 18;
            if (std::strcmp(ab, "flagtail") == 0)
                return 23;
            if (std::strcmp(ab, "imgspec") == 0)
                return 27;
            if (std::strcmp(ab, "hot0c70") == 0)
                return 30;
            if (std::strcmp(ab, "hot0e00") == 0)
                return 31;
            if (std::strcmp(ab, "hot30d8") == 0)
                return 32;
            if (std::strcmp(ab, "hot1be8") == 0)
                return 33;
            if (std::strcmp(ab, "hot1c00") == 0)
                return 34;
            if (std::strcmp(ab, "hot0900") == 0)
                return 35;
            if (std::strcmp(ab, "hot1110") == 0)
                return 36;
#if DC2_G532_FLAGHEAD
            if (std::strcmp(ab, "flaghead") == 0)
                return 25;
#endif
#if DC2_G517_INDARM
            if (std::strcmp(ab, "fpind") == 0)
                return 19;
            if (std::strcmp(ab, "stampind") == 0)
                return 20;
            if (std::strcmp(ab, "g517ind") == 0)
                return 21;
#endif
            return 0;
        }();
        if (s_paired == 1)
            return g499BlkLowerArm() > 0 ? 1 : 0;   // 0 vs 1: prices the lower dispatch alone
#if DC2_G499_QPLOCAL
        if (s_paired == 2)
            return g499QpBlockArm() > 0 ? 2 : 1;    // 1 vs 2: prices the Q/P residency alone
#endif
#if DC2_G500_BLKBRANCH
        // 1 vs 3: prices the control-transfer tail alone. The control arm is the G499 shipped body,
        // so nothing but the tail differs.
        if (s_paired == 3)
            return g500BlkBranchArm() > 0 ? 3 : 1;
#endif
#if DC2_G502_BLKCHAIN
        // 3 vs 5: prices the chain alone. The control arm is the G500 shipped body, and the two
        // arms execute the identical pairs in the identical blocks — only the return to run()
        // between two consecutive blocks differs, which `[G500:inv]`'s `calls=` column isolates.
        if (s_paired == 5)
            return g502BlkChainArm() > 0 ? 5 : 3;
#endif
#if DC2_G504_HISTCUR
        // 3 vs 6: prices the ring cursor's residency alone. Control is the G500/G501 shipped body.
        if (s_paired == 6)
            return g504HistCurArm() > 0 ? 6 : 3;
#endif
#if DC2_G506_QPSEG
        // 3 vs 7: prices the per-pair Q/P advance alone. Control is the G500/G501 shipped body, so
        // both arms execute the identical pairs in the identical blocks and differ only in whether
        // two counters are tested once per PAIR or once per SEGMENT.
        if (s_paired == 7)
            return g506QpSegArm() > 0 ? 7 : 3;
        // G514. 7 vs 8: prices the store staging alone. Control is the G506 shipped body, so both
        // arms execute the identical pairs in the identical blocks and differ only in how the
        // SQ/SQI/SQD bodies put their 16 bytes into VU RAM.
        if (s_paired == 8)
            return g514VStoreArm() > 0 ? 8 : 7;
        // G514b. 8 vs 10: prices the LOAD side alone, on top of the store side.
        if (s_paired == 10)
            return g514VLoadArm() > 0 ? 10 : 8;
        // G514 PROMOTION GATE. 7 vs 10: the shipped default against the pre-G514 body.
        if (s_paired == 11)
            return g514VMemArm() > 0 ? 10 : 7;
        // G515. 10 vs 12: prices the NOP peel alone. Control is the G514 shipped body, so both arms
        // execute the identical pairs in the identical blocks and differ only in whether the most
        // common lower kind reaches its (empty) body through the two-level jump table or through
        // one compare and one direct branch.
        if (s_paired == 12)
            return g515NopPeelArm() > 0 ? 12 : 10;
        // G516. 12 vs 13 / 14 / 15: prices the upper chain alone, the lower chain alone, and both
        // together. Control is the G515 shipped body in every case, so all three arms execute the
        // identical pairs in the identical blocks and differ only in where the dispatch inputs are
        // read from.
#if DC2_G516_UPDESC
        if (s_paired == 13)
            return g516UpDescArm() > 0 ? 13 : 12;
        if (s_paired == 15)
            return g516DispDenseArm() > 0 ? 15 : 12;
#endif
        if (s_paired == 14)
            return g516LowDenseArm() > 0 ? 14 : 12;
        // G516 PROMOTION GATE. 12 vs 18: the shipped default against the G515 body. Measured
        // rather than added — overlap does not compose (G493), and `peel4` and `lowdense` both
        // touch the same dispatch.
        if (s_paired == 18)
            return g516DispArm() > 0 ? 18 : 12;
        // G531. 18 vs 23: same shipped block body; candidate alone consumes typed sidecar value 3
        // and publishes the delayed MAC line before running the G487 fast lower.
        if (s_paired == 23)
            return g531FlagTailArm() > 0 ? 23 : 18;
        // G533. 23 vs 27: both arms keep the promoted typed flag tail. The candidate substitutes
        // one exact-source static handler at one immutable block identity; every other block uses
        // the same generic G531 body.
        if (s_paired == 27)
            return g533ImageSpecArm() > 0 ? 27 : 23;
        // G547. 27 vs 30: both arms retain G533's exact 0x3088 specialization; candidate alone
        // admits and executes the exact 0x0c70 fragment.
        if (s_paired == 30)
            return g547Hot0c70Arm() > 0 ? 30 : 27;
        // G548. 30 vs 31: candidate adds only the exact 0x0e00 fragment.
        if (s_paired == 31)
            return g548Hot0e00Arm() > 0 ? 31 : 30;
        // G549. 31 vs 32: candidate adds only exact 0x30d8.
        if (s_paired == 32)
            return g549Hot30d8Arm() > 0 ? 32 : 31;
        // G550. 32 vs 33: candidate adds only exact 0x1be8.
        if (s_paired == 33)
            return g550Hot1be8Arm() > 0 ? 33 : 32;
        // G551. 33 vs 34: candidate adds only exact 0x1c00.
        if (s_paired == 34)
            return g551Hot1c00Arm() > 0 ? 34 : 33;
        // G554. 34 vs 35: candidate adds only exact 0x0900.
        if (s_paired == 35)
            return g554Hot0900Arm() > 0 ? 35 : 34;
        // G555. G554 was refuted, so control is the shipped G551 set (34); candidate adds only
        // exact 0x1110 and deliberately does not inherit arm 35's 0x0900 handler.
        if (s_paired == 36)
            return g555Hot1110Arm() > 0 ? 36 : 34;
        // G532. 23 vs 25: both arms keep the promoted typed tail; the candidate additionally
        // consumes a typed flag prologue before the next already-existing ordinary block.
#if DC2_G532_FLAGHEAD
        if (s_paired == 25)
            return g532FlagHeadArm() > 0 ? 25 : 23;
#endif
        // G516b. 12 vs 17: prices peeling the four biggest remaining indirect-branch destinations,
        // with no new array and no change to any load — the isolated branch variable.
        if (s_paired == 17)
            return g516Peel4Arm() > 0 ? 17 : 12;
#if DC2_G517_INDARM
        // G517. 18 vs 19 / 20 / 21: prices the two induction POINTERS alone, the running STAMP
        // alone, and both. Control is the G516 shipped body in every case, so all three arms
        // execute the identical pairs in the identical blocks and differ only in how the pair
        // address, the class-byte address and the history stamp are formed. ALL THREE REFUTED —
        // see the `DC2_G517_INDARM` note at the head of this file.
        if (s_paired == 19)
            return g517PtrIndArm() > 0 ? 19 : 18;
        if (s_paired == 20)
            return g517StampIndArm() > 0 ? 20 : 18;
        if (s_paired == 21)
            return g517IndArm() > 0 ? 21 : 18;
#endif
#endif
        // G500 §4 — THE ROLLBACK FLAGS ARE READ ONCE, NOT ONCE PER run().
        //
        // `f50_12_vu_env_flag` is an UNCACHED `std::getenv` (vu1_helpers_and_tables.inc), and this
        // function is called once per run() — ~1,236 times per FRAME on this route. G499 put one
        // such call on the default path here and G500 briefly added a second; every other arm
        // accessor in this file (g490BlockRequested, g490StatOn, g499StatOn, …) already memoises in
        // a function-local `static const`, which is why nobody had noticed.
        //
        // It also made the two ESTIMATORS DISAGREE IN SIGN. The paired A/B returns at the
        // `s_paired` branches above and never reaches these calls, so it prices the block bodies
        // cleanly; a lean run pays them, and a getenv MISS scans the whole environment block while
        // a HIT stops at the match — so `DC2_G500_NO_BLKBRANCH=1` made its own rollback arm cheaper
        // than the default it was supposed to be compared against. Six interleaved lean runs read
        // the candidate 2.1 ms/f SLOWER while six paired runs read it 1.45 ms/f FASTER, purely
        // because of that asymmetry. Memoising removes both the cost and the bias.
        static const int s_default = [] {
            if (f50_12_vu_env_flag("DC2_G499_NO_BLKLOWER"))
                return 0;
#if DC2_G506_QPSEG
            // G514: the whole-vector store rides on the G506 body, so its rollback falls back to
            // arm 7 exactly as G506's did to arm 3. The ORACLE copy (arm 9) is selected by its own
            // flag and by nothing else, so it can never be reached by a timing run.
            if (!f50_12_vu_env_flag("DC2_G500_NO_BLKBRANCH") &&
                !f50_12_vu_env_flag("DC2_G506_NO_QPSEG") &&
                !f50_12_vu_env_flag("DC2_G514_NO_VSTORE"))
            {
                if (f50_12_vu_env_flag("DC2_G514_VERIFY"))
                    return 9;
                if (f50_12_vu_env_flag("DC2_G514_NO_VLOAD"))
                    return 8;
                // G515: the NOP peel rides on the G514 body, so its rollback falls back to arm 10
                // exactly as G514's did to arm 7.
                if (f50_12_vu_env_flag("DC2_G515_NO_NOPPEEL"))
                    return 10;
                // G516: the decode-time dispatch rides on the G515 body, so its rollback falls
                // back to arm 12 exactly as G515's did to arm 10.
                // G516 BRING-UP: default is still 12 until the gate has run. `DC2_G516_DISPDENSE=1`
                // selects arm 15 standalone for the oracle / disassembly checks; the promotion edit
                // is the one line below.
                if (f50_12_vu_env_flag("DC2_G516_VERIFY"))
                    return 16;
                // G516: the dispatch pair rides on the G515 body, so its rollback falls back to
                // arm 12 exactly as G515's did to arm 10.
                if (f50_12_vu_env_flag("DC2_G516_NO_DISP"))
                    return 12;
                // G531 promoted default-on after four Georama paired wins. Its oracle has a
                // separate instantiation; the one rollback restores arm 18 exactly.
                if (f50_12_vu_env_flag("DC2_G531_VERIFY"))
                    return 24;
                if (f50_12_vu_env_flag("DC2_G531_NO_FLAGTAIL"))
                    return 18;
                if (f50_12_vu_env_flag("DC2_G533_VERIFY"))
                    return 28;
                if (f50_12_vu_env_flag("DC2_G533_STAT"))
                    return 29;
#if DC2_G532_FLAGHEAD
                // G532 rides on G531. Its verifier is a separate noinline body; rollback restores
                // arm 23 exactly, while G531's rollback above still restores arm 18.
                if (f50_12_vu_env_flag("DC2_G532_VERIFY"))
                    return 26;
                if (f50_12_vu_env_flag("DC2_G532_NO_FLAGHEAD"))
                    return 23;
#endif
#if DC2_G517_INDARM
                // G517: REFUTED. The default stays 18; these two only exist so the refutation
                // reproduces when `DC2_G517_INDARM` is set to 1.
                if (f50_12_vu_env_flag("DC2_G517_VERIFY"))
                    return 22;
                if (f50_12_vu_env_flag("DC2_G517_IND"))
                    return 21;
#endif
                // G533 promoted default-on after the refined exact-source handler won four of
                // four Georama paired trials. One rollback restores G531 arm 23 exactly.
                if (f50_12_vu_env_flag("DC2_G533_NO_STATIC"))
                    return 23;
                // G547 extends that immutable set by one exact-source fragment. Its narrow
                // rollback keeps G533's 0x3088 handler and removes only 0x0c70.
                if (f50_12_vu_env_flag("DC2_G547_NO_0C70"))
                    return 27;
                if (f50_12_vu_env_flag("DC2_G548_NO_0E00"))
                    return 30;
                if (f50_12_vu_env_flag("DC2_G549_NO_30D8"))
                    return 31;
                if (f50_12_vu_env_flag("DC2_G550_NO_1BE8"))
                    return 32;
                if (f50_12_vu_env_flag("DC2_G551_NO_1C00"))
                    return 33;
                // G554: REFUTED. The 36-pair 0x0900 unroll is exact but expands the VU hot loop
                // enough to lose ~5.9 ms/f on s05. Keep arm 34 shipped; opt-in arm 35 only for
                // reproducing the oracle/gate, with the narrow rollback taking precedence.
                if (f50_12_vu_env_flag("DC2_G554_0900") &&
                    !f50_12_vu_env_flag("DC2_G554_NO_0900"))
                    return 35;
                if (f50_12_vu_env_flag("DC2_G555_1110") &&
                    !f50_12_vu_env_flag("DC2_G555_NO_1110"))
                    return 36;
                return 34;
            }
            // G506: the segmented Q/P advance rides on the G500 tail body, so its rollback falls
            // back to arm 3 exactly as `histcur`'s did.
            if (!f50_12_vu_env_flag("DC2_G500_NO_BLKBRANCH") &&
                !f50_12_vu_env_flag("DC2_G506_NO_QPSEG"))
                return 7;
#endif
#if DC2_G504_HISTCUR
            if (!f50_12_vu_env_flag("DC2_G500_NO_BLKBRANCH") &&
                !f50_12_vu_env_flag("DC2_G504_NO_HISTCUR"))
                return 6;
#endif
#if DC2_G502_BLKCHAIN
            if (!f50_12_vu_env_flag("DC2_G500_NO_BLKBRANCH") &&
                !f50_12_vu_env_flag("DC2_G502_NO_BLKCHAIN"))
                return 5;
#endif
#if DC2_G500_BLKBRANCH
            if (!f50_12_vu_env_flag("DC2_G500_NO_BLKBRANCH"))
                return 3;
#endif
#if DC2_G499_QPLOCAL
            return f50_12_vu_env_flag("DC2_G499_NO_QPLOCAL") ? 1 : 2;
#else
            return 1; // arm 2 is compiled out; see the DC2_G499_QPLOCAL note at the head of the file
#endif
        }();
        return s_default;
    }

    bool g499StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G499_STAT");
        return s_on;
    }

    // G504: the ring-cursor oracle. Memoised in a function-local `static const` (G500's law — an
    // uncached getenv reachable from the block leaf is the exact shape that cost ~8 ms/f).
    bool g504VerifyOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G504_VERIFY");
        return s_on;
    }

    // ---------------------------------------------------------------------------------------
    // Eligibility + the per-slot maximal run length, maintained beside G410's precisely
    // invalidated pair cache (same contract as G475's table: rebuilt on a full cache build and on
    // every MPG invalidation, so it can never describe a stale encoding).
    // ---------------------------------------------------------------------------------------
    bool g490BlockPairEligible(const G410CompiledUpper &pair)
    {
        if (pair.pairFlags != 0u)
            return false;                                   // E-bit or LOI
        if (pair.lowerKind == G422_LK_NONE)
            return false;                                   // needs the legacy execLower
        if (pair.lowerKind >= G422_LK_B && pair.lowerKind <= G422_LK_IBGEZ)
            return false;                                   // branch / jump: writes pc
        if (pair.lowerKind >= G422_LK_FIRST_FLAG)
            return false;                                   // reads the MAC/STATUS line
        if ((pair.g425Flags &
             (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u)
            return false;                                   // per-pair machinery is live
        if (pair.upperKind == 0x6fu)
            return true;                                    // VNOP: the loop skips the upper
        return g421DescTableConst.e[pair.upperKind].cls != 0u;
    }

    // G531: a flag consumer may terminate an EXISTING block. It cannot be an ordinary member,
    // because the generic pair loop publishes the delayed MAC line before executing either slot.
    // The typed tail reproduces that publication at its exact stamp, then executes the already-
    // verified G487 fast lower. Every other per-pair mechanism remains required to be inert.
    bool g531FlagTailEligible(const G410CompiledUpper &pair)
    {
        if (pair.pairFlags != 0u)
            return false;
        if (pair.lowerKind < G422_LK_FIRST_FLAG || pair.lowerKind >= G422_LK_COUNT)
            return false;
        if ((pair.g425Flags & G427_PF_FLAGREAD) == 0u)
            return false;
        if ((pair.g425Flags & (G426_PF_PRESTALL | G426_PF_HAZ)) != 0u)
            return false;
        if (pair.upperKind == 0x6fu)
            return true;
        return g421DescTableConst.e[pair.upperKind].cls != 0u;
    }

    // G500: the CONTROL-TRANSFER TAIL predicate. Identical to g490BlockPairEligible except that the
    // lower is required to BE a branch/jump instead of required not to be one. Every other clause is
    // the same, and for the same reason:
    //   * `pairFlags == 0`   -> no E-bit (no latch, no drain) and no I-bit.
    //   * `!G426_PF_PRESTALL` / `!G426_PF_HAZ` / `!G427_PF_FLAGREAD` -> the three per-pair blocks
    //     the pair loop runs around the slots are provably inert for this pair. (A branch lower can
    //     never set PRESTALL, and reads no flag line, but HAZ is a pure function of the two
    //     encodings and is honoured rather than reasoned away — same conservatism as G489's I-bit.)
    //   * upper is VNOP (skipped) or `cls != 0` (`g421FastUpper` executes it).
    // What the tail pair additionally needs, and what the block body supplies, is the architectural
    // PC: `G481_LOWER_BODY_B/BAL/JR/IBEQ/...` compute their target from `st.pc`, and BAL/JALR write
    // `(st.pc + 16) / 8` into VI. The block writes `st.pc` exactly once, immediately before the tail
    // lower, which is the same value the pair loop would have had live there.
    bool g500BranchTailEligible(const G410CompiledUpper &pair)
    {
        if (pair.pairFlags != 0u)
            return false;                                   // E-bit or LOI
        if (pair.lowerKind < G422_LK_B || pair.lowerKind > G422_LK_IBGEZ)
            return false;                                   // not a control transfer
        if ((pair.g425Flags &
             (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u)
            return false;                                   // per-pair machinery is live
        if (pair.upperKind == 0x6fu)
            return true;                                    // VNOP: the loop skips the upper
        return g421DescTableConst.e[pair.upperKind].cls != 0u;
    }

    // G516: the DENSE renumbering of the ten lower kinds the block leaf's switch names. Values are
    // 1..10 with no gaps and each maps to its own case body, which is the condition under which
    // MSVC emits a ONE-level jump table; the shipped switch is keyed on `lowerKind` (values
    // 1,2,3,6,20,21,25,26,27,28 spread over a 28-wide range with 18 of the 28 sharing `default`),
    // which is why MSVC compresses it into a byte-index table plus a rel32 table — two dependent
    // loads where one would do.
    //
    // 0 means "none of the ten": the leaf's `default:` calls g422FastLower with the ORIGINAL
    // `lowerKind`, read from the fused record on that path only.
    enum : uint8_t
    {
        G516_BC_OTHER = 0u,
        G516_BC_NOP = 1u,
        G516_BC_IADDI = 2u,
        G516_BC_IADDIU = 3u,
        G516_BC_IAND = 4u,
        G516_BC_SQ = 5u,
        G516_BC_SQI = 6u,
        G516_BC_SQD = 7u,
        G516_BC_LQ = 8u,
        G516_BC_LQI = 9u,
        G516_BC_LQD = 10u,
        G516_BC_COUNT = 11u,
    };

    uint8_t g516LowClass(uint8_t lowerKind)
    {
        switch (lowerKind)
        {
        case G422_LK_NOP:    return G516_BC_NOP;
        case G422_LK_IADDI:  return G516_BC_IADDI;
        case G422_LK_IADDIU: return G516_BC_IADDIU;
        case G422_LK_IAND:   return G516_BC_IAND;
        case G422_LK_SQ:     return G516_BC_SQ;
        case G422_LK_SQI:    return G516_BC_SQI;
        case G422_LK_SQD:    return G516_BC_SQD;
        case G422_LK_LQ:     return G516_BC_LQ;
        case G422_LK_LQI:    return G516_BC_LQI;
        case G422_LK_LQD:    return G516_BC_LQD;
        default:             return G516_BC_OTHER;
        }
    }

    // G516 ORACLE (DC2_G516_VERIFY=1, arm 16, default OFF).
    //
    // The lever is exact by construction — `g516Blk[i]` is filled from the SAME link-time-constant
    // table at the SAME index as the code it replaces, and `g490RebuildRunLengths()` is called on
    // BOTH paths that can change `uppers[]` (the full prepare and the precise MPG invalidation),
    // before any pair of the new encoding executes. What this oracle adds is the EXECUTION-TIME
    // half of that argument: it re-derives the record from the fused sidecar for every pair of
    // every executed block and aborts on the first disagreement. Combined with DC2_G490_VERIFY,
    // which re-derives the sidecar itself from the LIVE code words, the chain
    //     live code word -> fused sidecar -> G516 record -> dispatch
    // is checked end to end.
    thread_local uint64_t s_g516Verified = 0u;
    thread_local uint64_t s_g516LastReport = 0u;

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g516VerifyFail(uint8_t gotLow, const G410CompiledUpper &pair)
    {
        std::fprintf(stderr,
                     "[G516:verify] MISMATCH upperKind=0x%02x lowerKind=%u | got(low=%u)"
                     " want(low=%u) after %llu ok\n",
                     (unsigned)pair.upperKind, (unsigned)pair.lowerKind, (unsigned)gotLow,
                     (unsigned)g516LowClass(pair.lowerKind),
                     (unsigned long long)s_g516Verified);
        std::fflush(stderr);
        std::abort();
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g516VerifyReport()
    {
        std::fprintf(stderr, "[G516:verify] pairsChecked=%llu mismatches=0\n",
                     (unsigned long long)s_g516Verified);
        std::fflush(stderr);
    }

    static inline void g516CheckLow(uint8_t gotLow, const G410CompiledUpper &pair)
    {
        if (gotLow != g516LowClass(pair.lowerKind))
            g516VerifyFail(gotLow, pair);
        if (++s_g516Verified - s_g516LastReport >= 100000000ull)
        {
            s_g516LastReport = s_g516Verified;
            g516VerifyReport();
        }
    }

#if DC2_G517_INDARM
    // ---------------------------------------------------------------------------------------
    // G517: the induction-variable oracle (arm 22, DC2_G517_VERIFY=1).
    //
    // The lever replaces `fused[firstIndex + j]` with a pointer advanced once per pair and
    // `stampBase + j` with a counter advanced once per pair. Both are identities the compiler
    // cannot check for us and that a future `continue` inside the pair body would silently break —
    // so re-derive both the OLD way on every pair of every executed block and compare. Lives in a
    // copy its own flag selects, exactly like G516's (G489 §6).
    // ---------------------------------------------------------------------------------------
    thread_local uint64_t s_g517Verified = 0u;
    thread_local uint64_t s_g517LastReport = 0u;

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g517VerifyFail(const void *gotFp, const void *wantFp, int64_t gotStamp, int64_t wantStamp)
    {
        std::fprintf(stderr,
                     "[G517:verify] MISMATCH fp got=%p want=%p | stamp got=%lld want=%lld"
                     " after %llu ok\n",
                     gotFp, wantFp, (long long)gotStamp, (long long)wantStamp,
                     (unsigned long long)s_g517Verified);
        std::fflush(stderr);
        std::abort();
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g517VerifyReport()
    {
        std::fprintf(stderr, "[G517:verify] pairsChecked=%llu mismatches=0\n",
                     (unsigned long long)s_g517Verified);
        std::fflush(stderr);
    }

    static inline void g517CheckInd(const G479FusedPair *gotFp, const G479FusedPair *wantFp,
                                    int64_t gotStamp, int64_t wantStamp)
    {
        if (gotFp != wantFp || gotStamp != wantStamp)
            g517VerifyFail(gotFp, wantFp, gotStamp, wantStamp);
        if (++s_g517Verified - s_g517LastReport >= 100000000ull)
        {
            s_g517LastReport = s_g517Verified;
            g517VerifyReport();
        }
    }
#endif // DC2_G517_INDARM

    // G533 immutable plan. This is rebuilt through the same precise MPG/code-write funnel as every
    // G410 sidecar. The source words are compared exactly (not sampled and not hash-only), so a
    // static handler is admitted only for the byte-identical fragment it implements.
    constexpr uint32_t G533_HOT_3088_INDEX = 0x3088u / 8u;
    constexpr uint32_t G533_HOT_3088_COUNT = 9u;
    constexpr uint32_t G533_HOT_3088_TAIL = G531_FLAG_TAIL;
    constexpr uint64_t G533_HOT_3088_SOURCE[] = {
        0x01e0ac5b81f5437cull, 0x01e0b49b81f6437cull, 0x01e0bcdb81f7437cull,
        0x000002ff8000033cull, 0x01d189ff8000033cull, 0x01d5a9ff8000033cull,
        0x000002ff8000033cull, 0x000002ff8000033cull, 0x000002ff8000033cull,
        0x000002ff38010000ull,
    };
    // G547: first census-selected expansion. This exact six-pair fragment is shared by the
    // Georama and Dungeon-6 images (6.0% and 10.2% of all executed blocks respectively).
    constexpr uint32_t G547_HOT_0C70_INDEX = 0x0c70u / 8u;
    constexpr uint32_t G547_HOT_0C70_COUNT = 4u;
    constexpr uint32_t G547_HOT_0C70_TAIL = 2u;
    constexpr uint64_t G547_HOT_0C70_SOURCE[] = {
        0x01c0a55c01f43801ull, 0x01f058bd81d38b3cull, 0x01f060be81e8937dull,
        0x01f06c4b01f04002ull, 0x000002ff520907f9ull, 0x000002ff81e7ab7dull,
    };
    // G548: second shared census-selected expansion (4.8% Georama, 7.8% Dungeon-6).
    constexpr uint32_t G548_HOT_0E00_INDEX = 0x0e00u / 8u;
    constexpr uint32_t G548_HOT_0E00_COUNT = 8u;
    constexpr uint32_t G548_HOT_0E00_TAIL = 2u;
    constexpr uint64_t G548_HOT_0E00_SOURCE[] = {
        0x01f638be8000033cull, 0x01f645cb8000033cull, 0x01e0ad908000033cull,
        0x01f411bc8000033cull, 0x01f418bd8000033cull, 0x01eebe1580094ff2ull,
        0x01f4254a01f44003ull, 0x01e049bf8000033cull, 0x01f628bc520907f7ull,
        0x01f630bd81e8c37dull,
    };
    // G549: tiny shared loop fragment (8.7% Georama, 3.1% Dungeon-6).
    constexpr uint32_t G549_HOT_30D8_INDEX = 0x30d8u / 8u;
    constexpr uint32_t G549_HOT_30D8_COUNT = 2u;
    constexpr uint32_t G549_HOT_30D8_TAIL = 2u;
    constexpr uint64_t G549_HOT_30D8_SOURCE[] = {
        0x000002ff80050934ull, 0x000002ff8000033cull,
        0x000002ff5004000full, 0x000002ff8000033cull,
    };
    // G550: two-pair shared store loop (2.3% Georama, 6.6% Dungeon-6).
    constexpr uint32_t G550_HOT_1BE8_INDEX = 0x1be8u / 8u;
    constexpr uint32_t G550_HOT_1BE8_COUNT = 2u;
    constexpr uint32_t G550_HOT_1BE8_TAIL = 0u;
    constexpr uint64_t G550_HOT_1BE8_SOURCE[] = {
        0x003dde9280094ff2ull, 0x0030ef0b81e6cb7dull,
    };
    // G551: six-pair copy/pack loop shared by Dungeon-6 (7.4%) and Georama (2.3%).
    constexpr uint32_t G551_HOT_1C00_INDEX = 0x1c00u / 8u;
    constexpr uint32_t G551_HOT_1C00_COUNT = 4u;
    constexpr uint32_t G551_HOT_1C00_TAIL = 2u;
    constexpr uint64_t G551_HOT_1C00_SOURCE[] = {
        0x000002ff81f4237cull, 0x01f1d17d81f92b7cull, 0x003de6d581da833cull,
        0x003d01bc81f0137cull, 0x000002ff520907f8ull, 0x000002ff81e68b7dull,
    };
    // G554: s05's next census-selected fragment. This 36-pair matrix/load/store block is 3.39%
    // of all full block entries but carries far more pair work than every remaining candidate.
    constexpr uint32_t G554_HOT_0900_INDEX = 0x0900u / 8u;
    constexpr uint32_t G554_HOT_0900_COUNT = 34u;
    constexpr uint32_t G554_HOT_0900_TAIL = 2u;
    constexpr uint64_t G554_HOT_0900_SOURCE[] = {
        0x01f19eda01e3383eull, 0x01f195db01e4383full, 0x01f19f1b01e5403cull,
        0x01f409bc01e6403dull, 0x01f410bd01e7403eull, 0x01f418be01e8403full,
        0x01f420bf01e9483cull, 0x01f528bc01ea483dull, 0x01f530bd01eb483eull,
        0x01f538be01ec483full, 0x01f540bf01ed503cull, 0x01f648bc01ee503dull,
        0x01f650bd01ef503eull, 0x01f658be01f0503full, 0x01f660bf8000033cull,
        0x01f768bc800318b2ull, 0x01f770bd8000033cull, 0x01f778be01f11801ull,
        0x01f7878b01f21001ull, 0x01f909bc01f32001ull, 0x01f910bd8000033cull,
        0x01f918be81071bfeull, 0x01fa28bc80881bfeull, 0x01fa30bd80491bfeull,
        0x01fa38be802a1bfeull, 0x01fb48bc01e1383cull, 0x01fb50bd01e2383dull,
        0x01fb58be8000033cull, 0x01fc68bc8000033cull, 0x01fc70bd8000033cull,
        0x01fc77ca8000033cull, 0x01f1951881e2f37dull, 0x01f19e58800b5ff2ull,
        0x01f195598000033cull, 0x01f19e99520b07ddull, 0x01f1959a81e4fb7dull,
    };
    // G555: s05's next compact candidate after G554's oversized unroll was refuted.
    constexpr uint32_t G555_HOT_1110_INDEX = 0x1110u / 8u;
    constexpr uint32_t G555_HOT_1110_COUNT = 4u;
    constexpr uint32_t G555_HOT_1110_TAIL = 2u;
    constexpr uint64_t G555_HOT_1110_SOURCE[] = {
        0x000002ff01f93801ull, 0x01eead9501f44001ull,
        0x000002ff80094ff2ull, 0x000002ff80073872ull,
        0x000002ff520907fbull, 0x01f9a56a81e8b37dull,
    };
    uint8_t s_g533Hot3088Plan = 0u;
    uint8_t s_g547Hot0c70Plan = 0u;
    uint8_t s_g548Hot0e00Plan = 0u;
    uint8_t s_g549Hot30d8Plan = 0u;
    uint8_t s_g550Hot1be8Plan = 0u;
    uint8_t s_g551Hot1c00Plan = 0u;
    uint8_t s_g554Hot0900Plan = 0u;
    uint8_t s_g555Hot1110Plan = 0u;
    void g547RefreshSignatures();

    void g490RebuildRunLengths()
    {
        uint32_t run = 0u;
        for (size_t i = s_g410Cache.uppers.size(); i-- > 0u;)
        {
            // G516: resolve this slot's dispatch inputs. Unconditional and present on EVERY arm —
            // it runs once per slot per code write (2,048 slots, ~1 rebuild per program), never
            // per pair, so it cannot bias a gate. `g421DescTableConst` is 0x100 entries with a
            // zero tail, so 0x6f (VNOP) and 0xff (uncovered) both yield cls 0 without a test.
            {
                const G410CompiledUpper &g516p = s_g410Cache.uppers[i];
#if DC2_G516_UPDESC
                const G421UpperDesc g516d = g421DescTableConst.e[g516p.upperKind];
                uint32_t g516bits = 0u;
                std::memcpy(&g516bits, &g516d, sizeof(g516bits));
                s_g410Cache.g516UpDesc[i] = g516bits;
#endif
                s_g410Cache.g516LowCls[i] = g516LowClass(g516p.lowerKind);
            }
            if (g490BlockPairEligible(s_g410Cache.uppers[i]))
            {
                if (run < 255u)
                    ++run;
                s_g410Cache.g490RunLen[i] = static_cast<uint8_t>(run);
            }
            else
            {
                run = 0u;
                s_g410Cache.g490RunLen[i] = 0u;
            }
            // G500: 0 / 1 / 2 extra pairs a block may execute as a control-transfer TAIL at this
            // slot. `g490RunLen[i + 1]` has already been written by this same backward pass, and it
            // is non-zero exactly when slot i+1 is g490BlockPairEligible — which is the delay-slot
            // condition, so no second eligibility call is needed.
            uint8_t tail = 0u;
            if (g500BranchTailEligible(s_g410Cache.uppers[i]))
            {
                tail = 1u;
                if (i + 1u < s_g410Cache.g490RunLen.size() &&
                    s_g410Cache.g490RunLen[i + 1u] != 0u)
                    tail = 2u;
            }
            else if (g531FlagTailEligible(s_g410Cache.uppers[i]))
                tail = G531_FLAG_TAIL;
            s_g410Cache.g500Tail[i] = tail;
            // G502: pre-join the tail the block STARTING at this slot would find, so the hook is a
            // single byte load at the slot it is already indexing. `i + g490RunLen[i] > i`, so the
            // backward pass has already written that entry.
            const size_t g502At = i + static_cast<size_t>(s_g410Cache.g490RunLen[i]);
            s_g410Cache.g502TailAt[i] =
                (g502At < s_g410Cache.g500Tail.size()) ? s_g410Cache.g500Tail[g502At] : 0u;
        }

        bool g533Match =
            s_g410Cache.g490RunLen[G533_HOT_3088_INDEX] == G533_HOT_3088_COUNT &&
            s_g410Cache.g502TailAt[G533_HOT_3088_INDEX] == G533_HOT_3088_TAIL &&
            g531FlagTailEligible(
                s_g410Cache.uppers[G533_HOT_3088_INDEX + G533_HOT_3088_COUNT]);
        for (size_t i = 0u; i < sizeof(G533_HOT_3088_SOURCE) /
                                      sizeof(G533_HOT_3088_SOURCE[0]); ++i)
            g533Match = g533Match &&
                s_g410Cache.sources[G533_HOT_3088_INDEX + i] == G533_HOT_3088_SOURCE[i];
        s_g533Hot3088Plan = g533Match ? 1u : 0u;

        bool g547Match =
            s_g410Cache.g490RunLen[G547_HOT_0C70_INDEX] == G547_HOT_0C70_COUNT &&
            s_g410Cache.g502TailAt[G547_HOT_0C70_INDEX] == G547_HOT_0C70_TAIL &&
            g500BranchTailEligible(
                s_g410Cache.uppers[G547_HOT_0C70_INDEX + G547_HOT_0C70_COUNT]);
        for (size_t i = 0u; i < sizeof(G547_HOT_0C70_SOURCE) /
                                      sizeof(G547_HOT_0C70_SOURCE[0]); ++i)
            g547Match = g547Match &&
                s_g410Cache.sources[G547_HOT_0C70_INDEX + i] == G547_HOT_0C70_SOURCE[i];
        s_g547Hot0c70Plan = g547Match ? 1u : 0u;

        bool g548Match =
            s_g410Cache.g490RunLen[G548_HOT_0E00_INDEX] == G548_HOT_0E00_COUNT &&
            s_g410Cache.g502TailAt[G548_HOT_0E00_INDEX] == G548_HOT_0E00_TAIL &&
            g500BranchTailEligible(
                s_g410Cache.uppers[G548_HOT_0E00_INDEX + G548_HOT_0E00_COUNT]);
        for (size_t i = 0u; i < sizeof(G548_HOT_0E00_SOURCE) /
                                      sizeof(G548_HOT_0E00_SOURCE[0]); ++i)
            g548Match = g548Match &&
                s_g410Cache.sources[G548_HOT_0E00_INDEX + i] == G548_HOT_0E00_SOURCE[i];
        s_g548Hot0e00Plan = g548Match ? 1u : 0u;

        bool g549Match =
            s_g410Cache.g490RunLen[G549_HOT_30D8_INDEX] == G549_HOT_30D8_COUNT &&
            s_g410Cache.g502TailAt[G549_HOT_30D8_INDEX] == G549_HOT_30D8_TAIL &&
            g500BranchTailEligible(
                s_g410Cache.uppers[G549_HOT_30D8_INDEX + G549_HOT_30D8_COUNT]);
        for (size_t i = 0u; i < sizeof(G549_HOT_30D8_SOURCE) /
                                      sizeof(G549_HOT_30D8_SOURCE[0]); ++i)
            g549Match = g549Match &&
                s_g410Cache.sources[G549_HOT_30D8_INDEX + i] == G549_HOT_30D8_SOURCE[i];
        s_g549Hot30d8Plan = g549Match ? 1u : 0u;

        bool g550Match =
            s_g410Cache.g490RunLen[G550_HOT_1BE8_INDEX] == G550_HOT_1BE8_COUNT &&
            s_g410Cache.g502TailAt[G550_HOT_1BE8_INDEX] == G550_HOT_1BE8_TAIL;
        for (size_t i = 0u; i < sizeof(G550_HOT_1BE8_SOURCE) /
                                      sizeof(G550_HOT_1BE8_SOURCE[0]); ++i)
            g550Match = g550Match &&
                s_g410Cache.sources[G550_HOT_1BE8_INDEX + i] == G550_HOT_1BE8_SOURCE[i];
        s_g550Hot1be8Plan = g550Match ? 1u : 0u;

        bool g551Match =
            s_g410Cache.g490RunLen[G551_HOT_1C00_INDEX] == G551_HOT_1C00_COUNT &&
            s_g410Cache.g502TailAt[G551_HOT_1C00_INDEX] == G551_HOT_1C00_TAIL &&
            g500BranchTailEligible(
                s_g410Cache.uppers[G551_HOT_1C00_INDEX + G551_HOT_1C00_COUNT]) &&
            !f50_12_vu_env_flag("DC2_G133_COPY_QSKIP");
        for (size_t i = 0u; i < sizeof(G551_HOT_1C00_SOURCE) /
                                      sizeof(G551_HOT_1C00_SOURCE[0]); ++i)
            g551Match = g551Match &&
                s_g410Cache.sources[G551_HOT_1C00_INDEX + i] == G551_HOT_1C00_SOURCE[i];
        // G133's opt-in near-plane diagnostic observes PC 0x1c10 inside execLower. Keep that
        // diagnostic on the generic executor; the default graphics path uses this exact handler.
        s_g551Hot1c00Plan = g551Match ? 1u : 0u;

        bool g554Match =
            s_g410Cache.g490RunLen[G554_HOT_0900_INDEX] == G554_HOT_0900_COUNT &&
            s_g410Cache.g502TailAt[G554_HOT_0900_INDEX] == G554_HOT_0900_TAIL &&
            g500BranchTailEligible(
                s_g410Cache.uppers[G554_HOT_0900_INDEX + G554_HOT_0900_COUNT]);
        for (size_t i = 0u; i < sizeof(G554_HOT_0900_SOURCE) /
                                      sizeof(G554_HOT_0900_SOURCE[0]); ++i)
            g554Match = g554Match &&
                s_g410Cache.sources[G554_HOT_0900_INDEX + i] == G554_HOT_0900_SOURCE[i];
        s_g554Hot0900Plan = g554Match ? 1u : 0u;

        bool g555Match =
            s_g410Cache.g490RunLen[G555_HOT_1110_INDEX] == G555_HOT_1110_COUNT &&
            s_g410Cache.g502TailAt[G555_HOT_1110_INDEX] == G555_HOT_1110_TAIL &&
            g500BranchTailEligible(
                s_g410Cache.uppers[G555_HOT_1110_INDEX + G555_HOT_1110_COUNT]);
        for (size_t i = 0u; i < sizeof(G555_HOT_1110_SOURCE) /
                                      sizeof(G555_HOT_1110_SOURCE[0]); ++i)
            g555Match = g555Match &&
                s_g410Cache.sources[G555_HOT_1110_INDEX + i] == G555_HOT_1110_SOURCE[i];
        s_g555Hot1110Plan = g555Match ? 1u : 0u;

        // G533: price static microprogram-fragment specialization from immutable rebuild state,
        // never from the hot pair loop. This diagnostic emits exact source identity, decoded
        // operands, and block/tail boundaries for the shared Georama fragment. It is quiet by
        // default and runs only when a newly observed fragment is installed.
        static const bool s_g533PlanDump = f50_12_vu_env_flag("DC2_G533_PLAN_DUMP");
        if (s_g533PlanDump)
        {
            constexpr size_t kFirst = 0x3000u / 8u;
            constexpr size_t kLast = 0x3300u / 8u;
            uint64_t hash = 1469598103934665603ull;
            for (size_t i = kFirst; i < kLast; ++i)
            {
                const uint64_t source = s_g410Cache.sources[i];
                for (unsigned byte = 0u; byte < 8u; ++byte)
                {
                    hash ^= static_cast<uint8_t>(source >> (byte * 8u));
                    hash *= 1099511628211ull;
                }
            }

            static thread_local uint64_t s_g533LastHash = 0u;
            if (hash != s_g533LastHash)
            {
                s_g533LastHash = hash;
                std::fprintf(stderr,
                             "[G533:plan] region=3000-32ff fnv64=%016llx hot3088=%u\n",
                             static_cast<unsigned long long>(hash),
                             static_cast<unsigned>(s_g533Hot3088Plan));
                for (size_t i = kFirst; i < kLast; ++i)
                {
                    const G410CompiledUpper &p = s_g410Cache.uppers[i];
                    const uint64_t source = s_g410Cache.sources[i];
                    std::fprintf(stderr,
                                 "[G533:slot] pc=%04x lo=%08x up=%08x uk=%02x lk=%02x"
                                 " fs=%02x ft=%02x s2=%02x pf=%02x g425=%02x run=%u tail=%u\n",
                                 static_cast<unsigned>(i * 8u),
                                 static_cast<unsigned>(source),
                                 static_cast<unsigned>(source >> 32u),
                                 static_cast<unsigned>(p.upperKind),
                                 static_cast<unsigned>(p.lowerKind),
                                 static_cast<unsigned>(p.lowerFs),
                                 static_cast<unsigned>(p.lowerFt),
                                 static_cast<unsigned>(p.lowerS2),
                                 static_cast<unsigned>(p.pairFlags),
                                 static_cast<unsigned>(p.g425Flags),
                                 static_cast<unsigned>(s_g410Cache.g490RunLen[i]),
                                 static_cast<unsigned>(s_g410Cache.g502TailAt[i]));
                }
                std::fflush(stderr);
            }
        }
        g547RefreshSignatures();
#if DC2_G610_JIT_BACKEND
        // G610: LAST in this pass, so the translator reads the run lengths, the G516 sidecars and
        // the fused records this pass has just rewritten — never a mixture. VU1 data RAM is a fixed
        // 16 KB (`PS2Memory` allocates exactly PS2_VU1_DATA_SIZE), and the compiled-block entry
        // re-checks the caller's `dataSize` against the value baked here before executing anything.
        g610CompileAll(PS2_VU1_DATA_SIZE);
#if DC2_G612_REGION_BACKEND
        // G612: LAST of all, so the CFG walk reads the run lengths, the G500 tail codes and the
        // fused records this pass has just rewritten — never a mixture, and never a state G610's
        // own pass could still be changing.
        g612CompileAll(PS2_VU1_DATA_SIZE, PS2_VU1_CODE_SIZE);
#endif
#endif
    }

    // ---------------------------------------------------------------------------------------
    // Stats. Out of line and behind a cached bool, so the promoted copy carries one test and a
    // never-taken call (G421 law 17: never inline a census into this loop).
    // ---------------------------------------------------------------------------------------
    thread_local uint64_t s_g490Runs = 0u;
    thread_local uint64_t s_g490Pairs = 0u;
    thread_local uint64_t s_g490TotalPairs = 0u;
    thread_local uint64_t s_g490LastReport = 0u;

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g490NoteBlock(uint32_t count)
    {
        ++s_g490Runs;
        s_g490Pairs += count;
        if (s_g490Pairs - s_g490LastReport >= 50000000u)
        {
            s_g490LastReport = s_g490Pairs;
            std::fprintf(stderr,
                         "[G490:block] runs=%llu pairs=%llu avg=%.2f cover=%.2f%%\n",
                         static_cast<unsigned long long>(s_g490Runs),
                         static_cast<unsigned long long>(s_g490Pairs),
                         static_cast<double>(s_g490Pairs) /
                             static_cast<double>(s_g490Runs ? s_g490Runs : 1u),
                         s_g490TotalPairs
                             ? 100.0 * static_cast<double>(s_g490Pairs) /
                                   static_cast<double>(s_g490TotalPairs)
                             : 0.0);
        }
    }

    // G502: the same counter for a CHAIN, which ran `runs` blocks in one entry. Keeps `avg=` and
    // `cover=` meaning exactly what they meant before the chain existed.
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g490NoteBlocks(uint32_t runs, uint32_t count)
    {
        s_g490Runs += runs;
        s_g490Pairs += count;
        if (s_g490Pairs - s_g490LastReport >= 50000000u)
        {
            s_g490LastReport = s_g490Pairs;
            std::fprintf(stderr,
                         "[G490:block] runs=%llu pairs=%llu avg=%.2f cover=%.2f%%\n",
                         static_cast<unsigned long long>(s_g490Runs),
                         static_cast<unsigned long long>(s_g490Pairs),
                         static_cast<double>(s_g490Pairs) /
                             static_cast<double>(s_g490Runs ? s_g490Runs : 1u),
                         s_g490TotalPairs
                             ? 100.0 * static_cast<double>(s_g490Pairs) /
                                   static_cast<double>(s_g490TotalPairs)
                             : 0.0);
        }
    }

    // Called ONCE per run() (~1,230/frame), never per pair, so `cover=` is exact without putting
    // a counter in the loop.
    void g490NoteTotalPairs(uint32_t cycles)
    {
        s_g490TotalPairs += cycles;
    }

    // -------------------------------------------------------------------------------------------
    // G500: the PER-ARM WORKLOAD INVARIANT. A deletion/relocation lever must prove that both arms
    // did the same work (G492's -24.2 ms/f artefact was a candidate arm building 35 draws against
    // 829), and a coverage lever must prove the coverage actually moved. Both arms of `blkbranch`
    // execute the identical microprogram, so kicks/f, pairs/kick and blocks/f must be INVARIANT
    // while tailPairs and cover must differ — that pair of statements is the whole check.
    //
    // Off by default (DC2_G500_STAT=1), out of line, behind a bool cached once, and called once per
    // BLOCK and once per run() — never per pair (G421 law 17).
    // -------------------------------------------------------------------------------------------
    bool g500StatOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G500_STAT");
        return s_on;
    }

    struct G500Inv
    {
        uint64_t runs = 0u;
        uint64_t totalPairs = 0u;
        uint64_t blocks = 0u;
        uint64_t plainPairs = 0u;
        uint64_t tail1 = 0u;
        uint64_t tail2 = 0u;
        uint64_t taken = 0u;
        // G502: how many times run() ENTERED the block executor. Identical to `blocks` on every arm
        // before this phase; on the chain arm it is `blocks` minus the round trips the chain
        // deleted, which is the ONE column the two arms of `blkchain` are allowed to differ in.
        uint64_t calls = 0u;
        uint64_t lastReport = 0u;
    };
    // G501: indexed by the ACTUAL arm value, not by `arm != 0`. `blkbranch` alternates arms 1
    // and 3, but `blkpin` alternates 3 and 4 — with a two-bucket array keyed on `arm >= 3` both of
    // its arms collapsed into the same bucket and the per-arm invariant, whose entire job is to
    // prove the two arms did the SAME work, could not have shown a difference. Same defect class
    // as g499ReportArmed's `s_once[3]`, and the same fix.
    // G502: eight slots, so arm 5 (the chain candidate) has one of its own. Same reason G501 went
    // from 3 to 5 — a per-arm instrument that folds two arms into one bucket cannot show a
    // difference, which is the entire job of a workload invariant.
    thread_local G500Inv s_g500Inv[8];

    inline int g500InvSlot(int arm) { return (arm < 0 || arm > 7) ? 0 : arm; }

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g500NoteBlock(int arm, uint32_t plain, uint32_t tail, bool taken)
    {
        G500Inv &v = s_g500Inv[g500InvSlot(arm)];
        ++v.blocks;
        ++v.calls;
        v.plainPairs += plain;
        if (tail == 1u)
            ++v.tail1;
        else if (tail == 2u)
            ++v.tail2;
        if (taken)
            ++v.taken;
    }

    // G502: the same invariant for ONE CHAIN — a single entry into the block executor that ran
    // `segments` blocks. `blocks`, `plainPairs`, `tail1`, `tail2` and `taken` accumulate exactly as
    // they would have without the chain (that is what makes them the invariant); only `calls` sees
    // one instead of `segments`.
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g500NoteChain(int arm, uint32_t segments, uint32_t plain, uint32_t tail1,
                       uint32_t tail2, uint32_t taken)
    {
        G500Inv &v = s_g500Inv[g500InvSlot(arm)];
        v.blocks += segments;
        ++v.calls;
        v.plainPairs += plain;
        v.tail1 += tail1;
        v.tail2 += tail2;
        v.taken += taken;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g500NoteRun(int arm, uint32_t cycles)
    {
        G500Inv &v = s_g500Inv[g500InvSlot(arm)];
        ++v.runs;
        v.totalPairs += cycles;
        if (v.totalPairs - v.lastReport < 50000000u)
            return;
        v.lastReport = v.totalPairs;
        const double blocks = static_cast<double>(v.blocks ? v.blocks : 1u);
        const uint64_t tailPairs = v.tail1 + 2u * v.tail2;
        std::fprintf(stderr,
                     "[G500:inv] arm=%d runs=%llu pairs/run=%.1f blocks=%llu calls=%llu "
                     "pairs/blk=%.2f tail1=%llu tail2=%llu taken=%llu cover=%.2f%%\n",
                     g500InvSlot(arm),
                     static_cast<unsigned long long>(v.runs),
                     static_cast<double>(v.totalPairs) /
                         static_cast<double>(v.runs ? v.runs : 1u),
                     static_cast<unsigned long long>(v.blocks),
                     static_cast<unsigned long long>(v.calls),
                     (static_cast<double>(v.plainPairs) + static_cast<double>(tailPairs)) / blocks,
                     static_cast<unsigned long long>(v.tail1),
                     static_cast<unsigned long long>(v.tail2),
                     static_cast<unsigned long long>(v.taken),
                     v.totalPairs
                         ? 100.0 * (static_cast<double>(v.plainPairs) +
                                    static_cast<double>(tailPairs)) /
                               static_cast<double>(v.totalPairs)
                         : 0.0);
        std::fflush(stderr);
    }

    // -------------------------------------------------------------------------------------------
    // G502 - THE BLOCK-ENTRY CENSUS AND THE ROUND-TRIP CEILING PROBE.
    // -------------------------------------------------------------------------------------------
    bool g502CensusOn()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G502_CENSUS");
        return s_on;
    }

    // The split arm. Read ONCE per run() like g499BlkArmRun (G479's rule: a microprogram must not
    // change dispatch shape mid-execution), and memoised behind a function-local `static const` on
    // the default path — G500 §4's blind spot is exactly an arm accessor whose default path reaches
    // an uncached getenv.
#if DC2_G502_SPLIT_PROBE
    int g502SplitArmRun()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "blksplit") == 0;
        }();
        if (s_paired)
            return g502BlkSplitArm() > 0 ? 1 : 0;
        static const int s_default = f50_12_vu_env_flag("DC2_G502_SPLIT") ? 1 : 0;
        return s_default;
    }
#endif // DC2_G502_SPLIT_PROBE

    // The tail-0 admission. Same contract: read once per run(), memoised on the default path.
#if DC2_G502_BLKTAIL0
    int g502Tail0ArmRun()
    {
        static const bool s_paired = [] {
            const char *ab = std::getenv("DC2_G419_AB");
            return ab != nullptr && std::strcmp(ab, "blktail0") == 0;
        }();
        if (s_paired)
            return g502BlkTail0Arm() > 0 ? 1 : 0;
        static const int s_default = f50_12_vu_env_flag("DC2_G502_NO_BLKTAIL0") ? 0 : 1;
        return s_default;
    }
#endif // DC2_G502_BLKTAIL0

    // Why a pair the PAIR LOOP executed was not in a block. Tested in g490BlockPairEligible's own
    // order, so the first matching reason is the one that actually decided.
    enum G502Excl : int
    {
        G502_X_PENDING = 0,   // branch delay slot: the hook is gated on pendingBranchTarget
        G502_X_EBIT,          // E-bit latched: the hook fails closed
        G502_X_BUDGET,        // eligible run present but maxCycles - cycle < MIN_RUN
        G502_X_SHORT,         // eligible, but the maximal run from here is 1 pair
        G502_X_EIBIT,         // pairFlags != 0 (E-bit or LOI)
        G502_X_LKNONE,        // lower needs the legacy execLower (DIV/XGKICK/...)
        G502_X_BRANCH,        // control transfer (G500 takes these as TAILS, not as run members)
        G502_X_FLAGLOW,       // flag lower (FCAND/FCOR/FMAND/FCGET)
        G502_X_PRESTALL,      // G239 scalar prestall
        G502_X_HAZ,           // G139 pair hazard
        G502_X_FLAGREAD,      // G427 MAC/STATUS consumer
        G502_X_UPPER0,        // upper class 0 (execUpper fallback)
        G502_X_COUNT
    };

    static const char *const kG502ExclName[G502_X_COUNT] = {
        "pending", "ebit", "budget", "short", "eibit", "lknone",
        "branch", "flaglow", "prestall", "haz", "flagread", "upper0"};

    struct G502Win
    {
        uint64_t blocks = 0u;       // blocks formed
        uint64_t blkPairs = 0u;     // plain + tail pairs executed inside them
        uint64_t chain = 0u;        // blocks whose SUCCESSOR would immediately form another block
        uint64_t extraTrips = 0u;   // EXTRA round trips the blksplit probe adds at this block
        uint64_t tail1 = 0u;
        uint64_t tail2 = 0u;
        uint64_t taken = 0u;
        uint64_t plain = 0u;        // pairs executed by the pair loop
        uint64_t excl[G502_X_COUNT] = {};
        // G532 premise census: a typed flag consumer at the current pair followed by an ordinary
        // run can join that ALREADY-EXISTING block call. This is deliberately separate from
        // `flaglow`: the latter includes consumers with no following run, which would require the
        // singleton block G502 proved slower.
        uint64_t flagHead = 0u;
        uint64_t flagHeadKind[G422_LK_COUNT - G422_LK_FIRST_FLAG] = {};
    };
    // [0] = geometry kick, [1] = resume kick. Never keyed on a BOOLEAN OF AN ARM (G501 §6) — this
    // index is the KICK KIND, which is what the split is about.
    thread_local G502Win s_g502Win[2];
    thread_local uint64_t s_g502Report = 0u;

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g502Report()
    {
        for (int k = 0; k < 2; ++k)
        {
            const G502Win &w = s_g502Win[k];
            const double blocks = static_cast<double>(w.blocks ? w.blocks : 1u);
            const uint64_t total = w.blkPairs + w.plain;
            std::fprintf(stderr,
                         "[G502:blk] kind=%s blocks=%llu pairs/blk=%.2f cover=%.2f%% "
                         "chain=%.2f%% split=%.2f%% tail1=%llu tail2=%llu taken=%.1f%% plain=%llu\n",
                         k ? "res" : "geo",
                         static_cast<unsigned long long>(w.blocks),
                         static_cast<double>(w.blkPairs) / blocks,
                         total ? 100.0 * static_cast<double>(w.blkPairs) /
                                     static_cast<double>(total) : 0.0,
                         100.0 * static_cast<double>(w.chain) / blocks,
                         100.0 * static_cast<double>(w.extraTrips) / blocks,
                         static_cast<unsigned long long>(w.tail1),
                         static_cast<unsigned long long>(w.tail2),
                         100.0 * static_cast<double>(w.taken) /
                             static_cast<double>((w.tail1 + w.tail2) ? (w.tail1 + w.tail2) : 1u),
                         static_cast<unsigned long long>(w.plain));
            const double pl = static_cast<double>(w.plain ? w.plain : 1u);
            const double tot = static_cast<double>(total ? total : 1u);
            std::fprintf(stderr, "[G502:excl] kind=%s", k ? "res" : "geo");
            for (int r = 0; r < G502_X_COUNT; ++r)
                std::fprintf(stderr, " %s=%.2f%%/%.2f%%", kG502ExclName[r],
                             100.0 * static_cast<double>(w.excl[r]) / pl,
                             100.0 * static_cast<double>(w.excl[r]) / tot);
            std::fprintf(stderr, "\n");
            std::fprintf(stderr,
                         "[G502:join] kind=%s flaghead=%llu plain=%.2f%% all=%.2f%% "
                         "fcand=%llu fcor=%llu fmand=%llu fcget=%llu\n",
                         k ? "res" : "geo",
                         static_cast<unsigned long long>(w.flagHead),
                         100.0 * static_cast<double>(w.flagHead) / pl,
                         100.0 * static_cast<double>(w.flagHead) / tot,
                         static_cast<unsigned long long>(w.flagHeadKind[0]),
                         static_cast<unsigned long long>(w.flagHeadKind[1]),
                         static_cast<unsigned long long>(w.flagHeadKind[2]),
                         static_cast<unsigned long long>(w.flagHeadKind[3]));
        }
        std::fflush(stderr);
    }

    // Once per BLOCK (~157 k/frame), out of line, behind a bool cached once — never per pair.
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g502NoteBlock(uint32_t plain, uint32_t tail, bool taken, uint32_t clamped,
                       uint32_t nextPc, uint32_t remaining, bool pending, uint32_t codeSize)
    {
        G502Win &w = s_g502Win[s_g502Resume ? 1 : 0];
        ++w.blocks;
        w.blkPairs += plain + tail;
        if (tail == 1u)
            ++w.tail1;
        else if (tail == 2u)
            ++w.tail2;
        if (taken)
            ++w.taken;
        // How many EXTRA round trips would the blksplit probe add here? The halving is applied
        // again to every remainder that still reaches 2*MIN_RUN (the pair loop cannot know it is
        // looking at a remainder), so a run of 9 becomes 5+2+2 — TWO extra trips, not one. Counted
        // exactly rather than closed-form, because this is the denominator the probe's frame delta
        // is divided by and a wrong denominator is a wrong price.
        for (uint32_t n = clamped; n >= 2u * G490_MIN_RUN; n -= (n + 1u) / 2u)
            ++w.extraTrips;
        // Would a CHAINING executor have kept going instead of returning to run()? Exactly the
        // conditions the pair loop's own hook re-tests one iteration later.
        if (!pending && remaining >= G490_MIN_RUN && nextPc + 8u <= codeSize)
        {
            const uint32_t avail =
                static_cast<uint32_t>(s_g410Cache.g490RunLen[nextPc >> 3u]);
            if (avail >= G490_MIN_RUN)
                ++w.chain;
        }
    }

    // Once per pair the PAIR LOOP executes (~11% of all pairs, ~178 k/frame). Out of line so the
    // promoted copy carries one test and a never-taken call (G421 law 17).
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g502NotePlain(const G410CompiledUpper *p, uint32_t avail, uint32_t nextAvail,
                       uint32_t budget, bool pending, bool ebit)
    {
        G502Win &w = s_g502Win[s_g502Resume ? 1 : 0];
        ++w.plain;
        if (!pending && !ebit && budget > G490_MIN_RUN && nextAvail >= G490_MIN_RUN &&
            g531FlagTailEligible(*p))
        {
            ++w.flagHead;
            ++w.flagHeadKind[p->lowerKind - G422_LK_FIRST_FLAG];
        }
        int r;
        if (pending)
            r = G502_X_PENDING;
        else if (ebit)
            r = G502_X_EBIT;
        else if (avail >= G490_MIN_RUN)
            r = G502_X_BUDGET;               // an eligible run was there; the budget clamped it
        else if (avail != 0u)
            r = G502_X_SHORT;                // eligible, but a run of exactly one
        else if (p->pairFlags != 0u)
            r = G502_X_EIBIT;
        else if (p->lowerKind == G422_LK_NONE)
            r = G502_X_LKNONE;
        else if (p->lowerKind >= G422_LK_B && p->lowerKind <= G422_LK_IBGEZ)
            r = G502_X_BRANCH;
        else if (p->lowerKind >= G422_LK_FIRST_FLAG)
            r = G502_X_FLAGLOW;
        else if ((p->g425Flags & G426_PF_PRESTALL) != 0u)
            r = G502_X_PRESTALL;
        else if ((p->g425Flags & G426_PF_HAZ) != 0u)
            r = G502_X_HAZ;
        else if ((p->g425Flags & G427_PF_FLAGREAD) != 0u)
            r = G502_X_FLAGREAD;
        else
            r = G502_X_UPPER0;
        ++w.excl[r];
        if (++s_g502Report >= 5000000u)
        {
            s_g502Report = 0u;
            g502Report();
        }
    }

    // ---------------------------------------------------------------------------------------
    // Oracle. Re-derives, from the LIVE code words, every predicate the elision rests on. This is
    // the risk that is actually new in G490 (a wrong eligibility decode silently drops a hazard,
    // a prestall or a flag read); the arithmetic of the two slots is already covered by G421's
    // and G422's own shadow oracles, and the compact-loop SHAPE by G475's 40 M-pair state oracle.
    // ---------------------------------------------------------------------------------------
    std::atomic<uint64_t> s_g490Verified{0u};

    [[noreturn]] void g490VerifyFail(const char *what, uint32_t pc, uint32_t lower, uint32_t upper)
    {
        std::fprintf(stderr,
                     "[G490:verify-BAD] %s pc=0x%x lower=%08x upper=%08x\n",
                     what, pc, lower, upper);
        std::fflush(stderr);
        std::abort();
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g490VerifyBlock(const uint8_t *vuCode, uint32_t firstIndex, uint32_t count)
    {
        for (uint32_t j = 0u; j < count; ++j)
        {
            const size_t index = static_cast<size_t>(firstIndex + j);
            const uint32_t pc = static_cast<uint32_t>(index * 8u);
            uint64_t live = 0u;
            std::memcpy(&live, vuCode + pc, sizeof(live));
            const G410CompiledUpper &p = s_g410Cache.uppers[index];
            if (live != s_g410Cache.sources[index] || live != s_g410Cache.fused[index].source)
                g490VerifyFail("stale-source", pc, (uint32_t)live, (uint32_t)(live >> 32));

            const uint32_t lower = static_cast<uint32_t>(live);
            const uint32_t upper = static_cast<uint32_t>(live >> 32);

            // E-bit / I-bit straight off the raw upper word.
            if (((upper >> 30u) & 1u) != 0u || ((upper >> 31u) & 1u) != 0u)
                g490VerifyFail("ei-bit", pc, lower, upper);
            // G239 scalar prestall.
            const uint8_t liveS2 =
                ((lower >> 25u) & 0x7fu) == 0x40u ? S2_FUNC(lower) : 0xffu;
            if (g426LowerIsScalarPipe(liveS2))
                g490VerifyFail("prestall", pc, lower, upper);
            // G427 MAC/STATUS consumer.
            if (g427LowerReadsFlags(lower))
                g490VerifyFail("flagread", pc, lower, upper);
            // G489: only the ten branch/jump lowers write pc.
            if (g489LowerWritesPc(lower))
                g490VerifyFail("writes-pc", pc, lower, upper);
            // G139 pair hazard, re-derived exactly as the G426 predecode verifier does.
            uint32_t liveIdx = 0xffffffffu;
            const uint8_t liveOp = static_cast<uint8_t>(upper & 0x3Fu);
            if (liveOp < 0x3Cu)
                liveIdx = (upper >> 6) & 0x1Fu;
            else
            {
                const uint8_t liveS2u = S2_FUNC(upper);
                if ((liveS2u >= 0x10u && liveS2u <= 0x17u) || liveS2u == 0x1Du)
                    liveIdx = (upper >> 16) & 0x1Fu;
            }
            if (liveIdx != 0xffffffffu &&
                ((((lower >> 11u) & 0x1Fu) == liveIdx) || (((lower >> 16u) & 0x1Fu) == liveIdx)))
                g490VerifyFail("pairhaz", pc, lower, upper);
            // And the cached flags this block's eligibility was actually taken from.
            if ((p.g425Flags & (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u ||
                p.pairFlags != 0u || !g490BlockPairEligible(p))
                g490VerifyFail("cached-flags", pc, lower, upper);
        }
        const uint64_t old = s_g490Verified.fetch_add(count, std::memory_order_relaxed);
        if (old == 0u || old / 100000000u != (old + count) / 100000000u)
            std::fprintf(stderr, "[G490:verify] pairs=%llu bad=0\n",
                         static_cast<unsigned long long>(old + count));
    }

    // G500: the same oracle for the control-transfer TAIL. Everything g490VerifyBlock asserts,
    // except that the terminator's lower MUST be one of the ten pc-writers rather than must not be,
    // and the delay slot (when the tail is 2) is put through the plain check.
#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g500VerifyTail(const uint8_t *vuCode, uint32_t branchIndex, uint32_t tailCount)
    {
        const uint32_t pc = branchIndex * 8u;
        uint64_t live = 0u;
        std::memcpy(&live, vuCode + pc, sizeof(live));
        const G410CompiledUpper &p = s_g410Cache.uppers[branchIndex];
        if (live != s_g410Cache.sources[branchIndex] ||
            live != s_g410Cache.fused[branchIndex].source)
            g490VerifyFail("tail-stale-source", pc, (uint32_t)live, (uint32_t)(live >> 32));

        const uint32_t lower = static_cast<uint32_t>(live);
        const uint32_t upper = static_cast<uint32_t>(live >> 32);
        if (((upper >> 30u) & 1u) != 0u || ((upper >> 31u) & 1u) != 0u)
            g490VerifyFail("tail-ei-bit", pc, lower, upper);
        const uint8_t liveS2 = ((lower >> 25u) & 0x7fu) == 0x40u ? S2_FUNC(lower) : 0xffu;
        if (g426LowerIsScalarPipe(liveS2))
            g490VerifyFail("tail-prestall", pc, lower, upper);
        if (g427LowerReadsFlags(lower))
            g490VerifyFail("tail-flagread", pc, lower, upper);
        // The terminator MUST be a pc writer — that is what makes it a tail rather than a plain
        // pair, and it is the one clause inverted against g490VerifyBlock.
        if (!g489LowerWritesPc(lower))
            g490VerifyFail("tail-not-branch", pc, lower, upper);
        uint32_t liveIdx = 0xffffffffu;
        const uint8_t liveOp = static_cast<uint8_t>(upper & 0x3Fu);
        if (liveOp < 0x3Cu)
            liveIdx = (upper >> 6) & 0x1Fu;
        else
        {
            const uint8_t liveS2u = S2_FUNC(upper);
            if ((liveS2u >= 0x10u && liveS2u <= 0x17u) || liveS2u == 0x1Du)
                liveIdx = (upper >> 16) & 0x1Fu;
        }
        if (liveIdx != 0xffffffffu &&
            ((((lower >> 11u) & 0x1Fu) == liveIdx) || (((lower >> 16u) & 0x1Fu) == liveIdx)))
            g490VerifyFail("tail-pairhaz", pc, lower, upper);
        if ((p.g425Flags & (G426_PF_PRESTALL | G426_PF_HAZ | G427_PF_FLAGREAD)) != 0u ||
            p.pairFlags != 0u || !g500BranchTailEligible(p))
            g490VerifyFail("tail-cached-flags", pc, lower, upper);
        if (tailCount == 2u)
            g490VerifyBlock(vuCode, branchIndex + 1u, 1u);
    }

    // ---------------------------------------------------------------------------------------
    // THE BLOCK. `noinline` on purpose: G481 measured this loop's budget binding on `run()`'s
    // instruction FOOTPRINT, so a second inlined copy of the SIMD upper + fast lower inside
    // run() would be paid by every pair to help a subset. Non-volatile registers survive the
    // call, and the hook sits at the TOP of the iteration where no pair-local value is live yet,
    // so the boundary costs the argument setup and nothing that the pair body holds.
    //
    // Order is the pair loop's order, statement for statement, for the eligible class:
    //   Q advance -> P advance -> upper -> lower -> VF0/VI0 re-pin.
    //
    // G499: ONE body, three instantiations. `G499FastLower` and `G499QpLocal` are compile-time
    // constants, so the control arm's text is byte-identical to what it was before this phase and
    // each candidate's extra code exists only in that candidate — an in-body runtime selector
    // would sit on BOTH arms and bias the gate (G436).
    //
    // G499 §2 — THE Q/P COUNTERS AS BLOCK LOCALS (`G499QpLocal`).
    //
    // Both counters are read-modify-written through POINTERS on every pair, and the loop calls
    // `g422FastLower` in between — which stores into `vuData` (SQ/SQI/SQD). MSVC cannot prove
    // `int *qDelay` does not alias that array, so it must RELOAD both from memory at the top of
    // every iteration, and that load sits in the dependency chain of the iteration's first branch.
    //
    // Hoisting them is EXACT here in a way it is not in the pair loop. Block eligibility excludes
    // `G426_PF_PRESTALL` (every FDIV/EFU producer and WAITQ/WAITP) and every lower `g422FastLower`
    // does not cover, and G479 established that the ONLY writer of these two counters outside the
    // loop body is `execLower`, which a block can never reach. So inside a block the counters can
    // only COUNT DOWN, never be re-armed: a local copy written back once at the end is the same
    // value the pointer form would have left, pair for pair, and `st.q`/`st.p` are published at
    // exactly the same pair.
    //
    // This is NOT G479's refuted `DC2_G479_QPREG`. That added two loop-carried locals to `run()`'s
    // pair loop, which G427/G442/G483 had already measured at ~1.2 IPC with 36 spills — the live
    // value cost more than the loads it deleted. This body is a 4.7 KB leaf function with its own
    // register file, entered ~160 k times/frame, and it is the ONE place the counters are provably
    // inert.
    // ---------------------------------------------------------------------------------------
    // G501 — PIN THE BLOCK'S LOOP-INVARIANT ADDRESSING PREDICATES (`G501Pin`).
    //
    // PREMISE, read off `dumpbin /DISASM` of the SHIPPED `g490BlockBody<1,0,1>` (0x3538 = 13,624
    // bytes): the leaf reloads `hb` from its STACK SLOT `[rbp+80h]` eleven times, loads the global
    // `s_g428DestStore` six times, and carries ELEVEN dead calls — `g421Desc()` ×3 and
    // `g421MaskRows()` ×8 — on arms that can never be taken. Three of the `hb` tests and one
    // `s_g428DestStore` test sit on the MAIN per-pair path (0x119 / 0x26A / 0x27B / 0x2D1), i.e.
    // on ~1.44 M pairs/frame.
    //
    // WHY THEY SURVIVED. Inside the pair loop `g436Lean` is a `constexpr` shadow (G443), so
    // `g421FastUpper` folds. The block body is a separate `noinline` leaf that takes `hb` as a
    // RUNTIME POINTER PARAMETER, so every fold G443 bought is lost at the call boundary — the same
    // class as G499's out-of-line lower dispatch, one level up.
    //
    // EXACTNESS. The block copy is selected only by `g490BlockRun = g484WideVf0 && …`, and
    // `g484WideVf0` is `g443Fixed && …`. `g443Fixed` lists **`g436Lean && g436Hb != nullptr`**
    // among its conjuncts, so `hb` is provably non-null here; G501 adds `s_g428DestStore` to that
    // same conjunction (the established convention for a promoted representation with a rollback
    // flag — `DC2_G428_NO_DEST_STORE=1` now drops to the legacy copy exactly as
    // `DC2_G436_LEGACY_ADDR=1` already did). Inside the block each pinned name therefore already
    // held the constant it is replaced by, which is G443's own argument applied to the leaf.
    //
    // This is a code-SHAPE change, so it is a separate INSTANTIATION rather than a runtime `if`
    // (G436): a selector inside the body would sit on both arms and bias the gate.
    // ---------------------------------------------------------------------------------------
    // G502 — LET A BLOCK CHAIN INTO THE NEXT ONE WITHOUT RETURNING TO run() (`G502Chain`).
    //
    // PREMISE, measured not assumed. `[G502:blk]` (this phase's census) reads **44.30% (geometry) /
    // 40.86% (resume) of blocks are immediately followed by another block** — i.e. the pair loop
    // leaves the block, walks its whole top-of-iteration envelope (pc-oob test, fused record load,
    // the pending/ebit gate, the run-length load, the budget clamp, the MIN_RUN test, the G500 tail
    // lookup, the noinline 10-argument call, the next-pc fold, the cycle update) and immediately
    // forms another block at the very next slot. At ~158 k blocks/frame that is ~67 k round trips
    // per frame that buy nothing. `DC2_G419_AB=blksplit` prices ONE such trip directly (it halves
    // every long block and adds a known number of them), so this lever is built only after its
    // ceiling is on the page — G497 §2 / G500 §8.
    //
    // WHAT THE ARM DOES. Nothing to any pair. The SAME pairs execute in the SAME order, in the same
    // blocks, with the same tails: only the return to run() between two consecutive blocks is
    // elided. `[G500:inv]` therefore has an unusually strong invariant available — `pairs/run`,
    // `cover`, `blocks` (segments), `tail1`, `tail2` and `taken` must ALL be identical across the
    // two arms, and only the new `calls=` column may differ. That is the G500 §5c control-flow
    // oracle with five extra columns.
    //
    // EXACTNESS. The chain step re-evaluates, inside the leaf, exactly the predicates run()'s hook
    // would have evaluated one iteration later, in the same order and from the same tables:
    //   * the next PC comes from the pair loop's own fold — sequential when there was no tail,
    //     and otherwise `st.pc != branchPc` for taken with the same `+8` / `+16` / wrap arithmetic.
    //   * a tail of 1 that was TAKEN leaves a pending branch target, and the pair loop is the only
    //     thing allowed to consume a delay slot, so the chain STOPS there and hands the target back.
    //     (A tail of 1 that was not taken lands on a slot whose run length is 0 by construction —
    //     that is why the tail was 1 and not 2 — so the MIN_RUN test below stops it anyway.)
    //   * `m_state.ebit` cannot have been set: every pair a block executes has `pairFlags == 0`,
    //     which is the E bit of the raw upper word, and nothing else in the leaf writes it. The
    //     hook's `!m_state.ebit` guard is therefore still true at every chain step.
    //   * `maxCycles` is an architectural suspension edge, so the remaining budget is carried and
    //     each segment is clamped to it exactly as the hook clamps the first.
    //   * `pc + 8 > codeSize` is the hook's own pc-oob test; the chain declines and lets the pair
    //     loop take its `break` with the right `stopReason`.
    // The G430 stamp base advances by the pairs each segment consumed, so every pair's stamp is the
    // one the un-chained arm would have given it.
    //
    // Arm:    DC2_G419_AB=blkchain (paired, hold 16 — thread-level code shape)
    // Kill:   DC2_G502_NO_BLKCHAIN=1
    // Stats:  DC2_G500_STAT=1 -> `[G500:inv] arm=5` with `calls=` beside `blocks=`.
    struct G502ChainOut
    {
        uint32_t consumed;        // pairs executed by the whole chain (plain + tail)
        uint32_t plain;           // plain pairs only
        uint32_t tail1;
        uint32_t tail2;
        uint32_t taken;
        uint32_t segments;        // blocks the chain absorbed (1 = no chaining happened)
        uint32_t nextPc;          // what run() must store into m_state.pc
        uint32_t pendingTarget;   // valid only when pendingValid
        bool pendingValid;
    };

    // ------------------------------------------------------------------------------------------
    // G514 ORACLE + POPULATION CENSUS.
    //
    // Lives ONLY in the oracle instantiation (`G514Verify`), which `DC2_G514_VERIFY=1` selects —
    // the G490 §oracle-copy convention, so the verifier can never end up in a copy its own flag
    // deselects, and the shipped body carries none of its live values (G504's lesson).
    //
    // `g514MakeRef` re-derives the address INDEPENDENTLY (it does not read anything the fast body
    // computed) and reproduces the SCALAR staging body's 16 bytes from the pre-store state. That
    // is the body being REPLACED, which is what G512 §5 requires of an oracle: a verifier that
    // compares the fast path against itself agrees with a wrong fast path.
    struct G514Ref
    {
        uint32_t addr = 0u;
        bool guarded = false;
        uint8_t dest = 0u;
        uint8_t vfIdx = 0u; // load side: the VF register the reference was built for
        float ref[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };

    static G514Ref g514MakeRef(const VU1State &st, uint32_t instr, uint8_t kind,
                               const uint8_t *vuData, uint32_t dataSize)
    {
        G514Ref r;
        const uint8_t it = LIT(instr);
        const uint8_t is = LIS(instr);
        r.dest = static_cast<uint8_t>((instr >> 21) & 0xFu);
        uint32_t addr;
        if (kind == G422_LK_SQ)
        {
            const int16_t imm = IMM11(instr);
            addr = static_cast<uint32_t>(static_cast<int32_t>(st.vi[it] + imm)) * 16u;
        }
        else if (kind == G422_LK_SQI)
        {
            addr = static_cast<uint32_t>(static_cast<uint16_t>(st.vi[it])) * 16u;
        }
        else // SQD decrements BEFORE the address is formed, and VI0 is never written
        {
            int32_t v = st.vi[it];
            if (it != 0)
                v = static_cast<int16_t>(v - 1);
            addr = static_cast<uint32_t>(static_cast<uint16_t>(v)) * 16u;
        }
        addr &= (dataSize - 1u);
        r.addr = addr;
        r.guarded = (addr + 16u <= dataSize);
        if (r.guarded)
        {
            std::memcpy(r.ref, vuData + addr, 16);
            if (r.dest & 0x8u)
                r.ref[0] = st.vf[is][0];
            if (r.dest & 0x4u)
                r.ref[1] = st.vf[is][1];
            if (r.dest & 0x2u)
                r.ref[2] = st.vf[is][2];
            if (r.dest & 0x1u)
                r.ref[3] = st.vf[is][3];
        }
        return r;
    }

    // G514b LOAD-side reference: the 16 bytes the SCALAR body would have left in `st.vf[it]`.
    // Built from a snapshot of the register taken BEFORE the fast body, so the lanes `dest` does
    // not select carry their own pre-existing value — which is exactly what the whole-vector write
    // must reproduce. The address is re-derived independently, including LQI's post-increment and
    // LQD's PRE-decrement.
    static G514Ref g514MakeLoadRef(const VU1State &st, uint32_t instr, uint8_t kind,
                                   const uint8_t *vuData, uint32_t dataSize)
    {
        G514Ref r;
        const uint8_t it = LIT(instr);
        const uint8_t is = LIS(instr);
        r.dest = static_cast<uint8_t>((instr >> 21) & 0xFu);
        r.vfIdx = it;
        uint32_t addr;
        if (kind == G422_LK_LQ)
        {
            const int16_t imm = IMM11(instr);
            addr = static_cast<uint32_t>(static_cast<int32_t>(st.vi[is] + imm)) * 16u;
        }
        else if (kind == G422_LK_LQI)
        {
            addr = static_cast<uint32_t>(static_cast<uint16_t>(st.vi[is])) * 16u;
        }
        else // LQD decrements BEFORE the address is formed, and VI0 is never written
        {
            int32_t v = st.vi[is];
            if (is != 0)
                v = static_cast<int16_t>(v - 1);
            addr = static_cast<uint32_t>(static_cast<uint16_t>(v)) * 16u;
        }
        addr &= (dataSize - 1u);
        r.addr = addr;
        r.guarded = (addr + 16u <= dataSize);
        std::memcpy(r.ref, st.vf[it], 16); // untouched lanes must survive
        if (r.guarded)
        {
            float tmp[4];
            std::memcpy(tmp, vuData + addr, 16);
            if (r.dest & 0x8u)
                r.ref[0] = tmp[0];
            if (r.dest & 0x4u)
                r.ref[1] = tmp[1];
            if (r.dest & 0x2u)
                r.ref[2] = tmp[2];
            if (r.dest & 0x1u)
                r.ref[3] = tmp[3];
        }
        return r;
    }

    // Population census + BYTE comparison. Counted per kind and per dest so the gate can be read
    // against the population it actually moved (G511's law: decompose before pricing).
    static uint64_t s_g514Checked = 0ull, s_g514Bad = 0ull, s_g514Skipped = 0ull;
    static uint64_t s_g514ByKind[3] = {0ull, 0ull, 0ull};   // SQ, SQI, SQD
    static uint64_t s_g514ByDest[16] = {};

    static void g514Check(const G514Ref &r, uint8_t kind, uint32_t instr, const uint8_t *vuData)
    {
        (void)instr;
        ++s_g514ByKind[kind == G422_LK_SQ ? 0 : (kind == G422_LK_SQI ? 1 : 2)];
        ++s_g514ByDest[r.dest & 0xFu];
        if (!r.guarded)
        {
            ++s_g514Skipped;
            return;
        }
        ++s_g514Checked;
        if (std::memcmp(vuData + r.addr, r.ref, 16) != 0)
        {
            if (++s_g514Bad <= 8ull)
                std::fprintf(stderr,
                             "[G514:verify-BAD] kind=%u dest=0x%x addr=0x%x\n",
                             (unsigned)kind, (unsigned)r.dest, (unsigned)r.addr);
        }
        if ((s_g514Checked % 20000000ull) == 0ull)
        {
            std::fprintf(stderr,
                         "[G514:verify] checked=%llu bad=%llu skipped=%llu | sq=%llu sqi=%llu "
                         "sqd=%llu | dest 0f=%.1f%% 01=%.1f%% 0e=%.1f%% 0d=%.1f%% 0c=%.1f%% "
                         "other=%.1f%%\n",
                         (unsigned long long)s_g514Checked, (unsigned long long)s_g514Bad,
                         (unsigned long long)s_g514Skipped,
                         (unsigned long long)s_g514ByKind[0], (unsigned long long)s_g514ByKind[1],
                         (unsigned long long)s_g514ByKind[2],
                         100.0 * (double)s_g514ByDest[0xF] / (double)s_g514Checked,
                         100.0 * (double)s_g514ByDest[0x1] / (double)s_g514Checked,
                         100.0 * (double)s_g514ByDest[0xE] / (double)s_g514Checked,
                         100.0 * (double)s_g514ByDest[0xD] / (double)s_g514Checked,
                         100.0 * (double)s_g514ByDest[0xC] / (double)s_g514Checked,
                         100.0 * (double)(s_g514Checked - s_g514ByDest[0xF] - s_g514ByDest[0x1] -
                                          s_g514ByDest[0xE] - s_g514ByDest[0xD] -
                                          s_g514ByDest[0xC]) / (double)s_g514Checked);
            std::fflush(stderr);
        }
    }

    // Load-side comparison. `st.vf[r.vfIdx]` is compared as BYTES, so a lane the dest mask did not
    // select must still equal the value it held before the pair ran.
    static uint64_t s_g514LdChecked = 0ull, s_g514LdBad = 0ull;
    static uint64_t s_g514LdByKind[3] = {0ull, 0ull, 0ull}; // LQ, LQI, LQD
    static uint64_t s_g514LdByDest[16] = {};

    static void g514CheckLoad(const G514Ref &r, uint8_t kind, const VU1State &st)
    {
        ++s_g514LdByKind[kind == G422_LK_LQ ? 0 : (kind == G422_LK_LQI ? 1 : 2)];
        ++s_g514LdByDest[r.dest & 0xFu];
        ++s_g514LdChecked;
        if (std::memcmp(st.vf[r.vfIdx], r.ref, 16) != 0)
        {
            if (++s_g514LdBad <= 8ull)
                std::fprintf(stderr,
                             "[G514:verify-BAD-load] kind=%u dest=0x%x vf=%u addr=0x%x\n",
                             (unsigned)kind, (unsigned)r.dest, (unsigned)r.vfIdx,
                             (unsigned)r.addr);
        }
        if ((s_g514LdChecked % 20000000ull) == 0ull)
        {
            std::fprintf(stderr,
                         "[G514:verify-load] checked=%llu bad=%llu | lq=%llu lqi=%llu lqd=%llu "
                         "| dest 0f=%.1f%% 01=%.1f%% 0e=%.1f%% 08=%.1f%% other=%.1f%%\n",
                         (unsigned long long)s_g514LdChecked, (unsigned long long)s_g514LdBad,
                         (unsigned long long)s_g514LdByKind[0],
                         (unsigned long long)s_g514LdByKind[1],
                         (unsigned long long)s_g514LdByKind[2],
                         100.0 * (double)s_g514LdByDest[0xF] / (double)s_g514LdChecked,
                         100.0 * (double)s_g514LdByDest[0x1] / (double)s_g514LdChecked,
                         100.0 * (double)s_g514LdByDest[0xE] / (double)s_g514LdChecked,
                         100.0 * (double)s_g514LdByDest[0x8] / (double)s_g514LdChecked,
                         100.0 * (double)(s_g514LdChecked - s_g514LdByDest[0xF] -
                                          s_g514LdByDest[0x1] - s_g514LdByDest[0xE] -
                                          s_g514LdByDest[0x8]) / (double)s_g514LdChecked);
            std::fflush(stderr);
        }
    }

    std::atomic<uint64_t> s_g531Verified{0u};

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g531VerifyFail(uint32_t pc, uint8_t cachedKind, uint8_t liveKind, uint32_t mac)
    {
        std::fprintf(stderr,
                     "[G531:verify-BAD] pc=0x%x cached=%u live=%u mac=%08x after=%llu\n",
                     pc, static_cast<unsigned>(cachedKind), static_cast<unsigned>(liveKind), mac,
                     static_cast<unsigned long long>(s_g531Verified.load(std::memory_order_relaxed)));
        std::fflush(stderr);
        std::abort();
    }

    void g531VerifyPass()
    {
        const uint64_t n = s_g531Verified.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n == 1u || (n % 10000000u) == 0u)
        {
            std::fprintf(stderr, "[G531:verify] tails=%llu bad=0\n",
                         static_cast<unsigned long long>(n));
            std::fflush(stderr);
        }
    }

#if DC2_G532_FLAGHEAD
    // G532's oracle is deliberately separate from G531's. The new claim is not the flag leaf;
    // G531 already proved that. It is that the same pair can execute one stamp before an ordinary
    // block without changing the live decode, delayed MAC value, or any VI result.
    std::atomic<uint64_t> s_g532Verified{0u};

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g532VerifyFail(uint32_t pc, uint8_t cachedKind, uint8_t liveKind, uint32_t mac)
    {
        std::fprintf(stderr,
                     "[G532:verify-BAD] pc=0x%x cached=%u live=%u mac=%08x after=%llu\n",
                     pc, static_cast<unsigned>(cachedKind), static_cast<unsigned>(liveKind), mac,
                     static_cast<unsigned long long>(s_g532Verified.load(std::memory_order_relaxed)));
        std::fflush(stderr);
        std::abort();
    }

    void g532VerifyPass()
    {
        const uint64_t n = s_g532Verified.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n == 1u || (n % 10000000u) == 0u)
        {
            std::fprintf(stderr, "[G532:verify] heads=%llu bad=0\n",
                         static_cast<unsigned long long>(n));
            std::fflush(stderr);
        }
    }
#endif

    template <bool G499FastLower, bool G499QpLocal, bool G500Tail = false, bool G501Pin = false,
              bool G502Chain = false, bool G504Cursor = false, bool G506QpSeg = false,
              bool G514VStore = false, bool G514VLoad = false, bool G514Verify = false,
              bool G515NopPeel = false, bool G516Up = false, bool G516Low = false,
              bool G516Verify = false, bool G516Peel4 = false, bool G517PtrInd = false,
              bool G517StampInd = false, bool G517Verify = false,
              bool G531FlagTail = false, bool G531Verify = false,
              bool G532FlagHead = false, bool G532Verify = false,
              // G610 CEILING PROBE. 0 = the shipped body, byte-identical (every guard below folds
              // to `true` and emits nothing). 1 = delete the LOWER slot, 2 = delete the UPPER slot
              // (and with it the G430 history push), 3 = delete BOTH, leaving only the loop, the
              // fused fetch, the Q/P segmentation and the G425 zero re-pin. Renders GARBAGE — it
              // exists only to bound what a compiled backend could recover. Never shipped ON.
              int G610Ceil = 0>
    void g490BlockBody(VU1State &st, uint32_t firstIndex, uint32_t count,
                              uint8_t *vuData, uint32_t dataSize,
                              int *qDelay, int *pDelay,
                              int64_t stampBase, G430HistBlock *hb,
                              uint32_t g500TailCount = 0u,
                              G502ChainOut *g502Out = nullptr,
                              uint32_t g502CodeSize = 0u, uint32_t g502Budget = 0u)
    {
        const G479FusedPair *const fused = s_g410Cache.fused.data();
        // G516: the pre-resolved dispatch record, hoisted exactly as `fused` is. Null and unread on
        // every pre-existing instantiation, so their bodies are unchanged.
        [[maybe_unused]] const uint32_t *g516UpBase = nullptr;
        [[maybe_unused]] const uint8_t *g516LowBase = nullptr;
        if constexpr (G516Up)
            g516UpBase = s_g410Cache.g516UpDesc.data();
        if constexpr (G516Low || G516Verify)
            g516LowBase = s_g410Cache.g516LowCls.data();
        // G516: the switch below is ONE body whose case labels are the arm's own encoding. The
        // shipped arm keys on `lowerKind` (values 1,2,3,6,20,21,25,26,27,28 in a 28-wide range, 18
        // of whose slots share `default`) and MSVC compresses that into a byte-index table feeding
        // a rel32 table — two DEPENDENT loads. The G516 arm keys on the dense 1..10 class, whose
        // ten targets are all distinct, so there is nothing to compress and one table remains.
        constexpr uint8_t G516K_NOP =
            G516Low ? static_cast<uint8_t>(G516_BC_NOP) : static_cast<uint8_t>(G422_LK_NOP);
        constexpr uint8_t G516K_IADDI =
            G516Low ? static_cast<uint8_t>(G516_BC_IADDI) : static_cast<uint8_t>(G422_LK_IADDI);
        constexpr uint8_t G516K_IADDIU =
            G516Low ? static_cast<uint8_t>(G516_BC_IADDIU) : static_cast<uint8_t>(G422_LK_IADDIU);
        constexpr uint8_t G516K_IAND =
            G516Low ? static_cast<uint8_t>(G516_BC_IAND) : static_cast<uint8_t>(G422_LK_IAND);
        constexpr uint8_t G516K_SQ =
            G516Low ? static_cast<uint8_t>(G516_BC_SQ) : static_cast<uint8_t>(G422_LK_SQ);
        constexpr uint8_t G516K_SQI =
            G516Low ? static_cast<uint8_t>(G516_BC_SQI) : static_cast<uint8_t>(G422_LK_SQI);
        constexpr uint8_t G516K_SQD =
            G516Low ? static_cast<uint8_t>(G516_BC_SQD) : static_cast<uint8_t>(G422_LK_SQD);
        constexpr uint8_t G516K_LQ =
            G516Low ? static_cast<uint8_t>(G516_BC_LQ) : static_cast<uint8_t>(G422_LK_LQ);
        constexpr uint8_t G516K_LQI =
            G516Low ? static_cast<uint8_t>(G516_BC_LQI) : static_cast<uint8_t>(G422_LK_LQI);
        constexpr uint8_t G516K_LQD =
            G516Low ? static_cast<uint8_t>(G516_BC_LQD) : static_cast<uint8_t>(G422_LK_LQD);
        // G506: the segmented form needs the two counters in block locals for the same reason
        // G499's arm 2 did — the segmentation is a statement about their values across the whole
        // run, not about one pair. Both arms write back exactly once, at the single exit.
        constexpr bool G506Local = G499QpLocal || G506QpSeg;
        [[maybe_unused]] int g499Qd = 0, g499Pd = 0;
        if constexpr (G506Local)
        {
            g499Qd = *qDelay;
            g499Pd = *pDelay;
        }
        // G504: the G430 ring cursor, carried in a local for the whole block. `hb` is non-null
        // here by construction (`G501Pin` asserts it, and `g443Fixed` proves it for every caller of
        // this copy). See the exactness note at `g430PushPendingAt`.
        [[maybe_unused]] int g504Slot = 0;
#if DC2_G504_VERIFY_CUR
        [[maybe_unused]] int g504Entry = 0;
#endif
        if constexpr (G504Cursor)
        {
            g504Slot = hb->head;
#if DC2_G504_VERIFY_CUR
            g504Entry = g504Slot;
#endif
        }
#if DC2_G532_FLAGHEAD
        if constexpr (G532FlagHead)
        {
            // G532: execute the typed flag consumer at `firstIndex - 1` as a PROLOGUE. The caller
            // passes the following ordinary run as `firstIndex/count`, so that run keeps exactly
            // the same noinline call count, address expressions, and branch/flag tail it had under
            // G531. Only this one generic pair-loop iteration disappears.
            const uint32_t g532Idx = firstIndex - 1u;
            const G479FusedPair &g532p = fused[g532Idx];
            const uint64_t g532src = g532p.source;
            const uint32_t g532lower = static_cast<uint32_t>(g532src);
            const uint32_t g532upper = static_cast<uint32_t>(g532src >> 32u);

            // Generic-loop order from `vu1_g435_pair_loop.inc`: Q advance, P advance, delayed-MAC
            // publication, upper, lower, zero-register re-pin. The ordinary run begins at the next
            // stamp and observes the counters/history left here.
            if constexpr (G506Local)
            {
                if (g499Qd > 0 && --g499Qd == 0)
                    st.q = s_vuQPending;
                if (g499Pd > 0 && --g499Pd == 0)
                    st.p = s_vuPPending;
            }
            else
            {
                if (*qDelay > 0 && --(*qDelay) == 0)
                    st.q = s_vuQPending;
                if (*pDelay > 0 && --(*pDelay) == 0)
                    st.p = s_vuPPending;
            }

            const int64_t g532Stamp = stampBase - 1;
            const uint32_t g532Mac = g430ResolveAt(g532Stamp - 4);
            s_vuMacVisible = g532Mac;

            if (g532p.pair.upperKind != 0x6fu)
                g421FastUpper<G501Pin, G504Cursor>(st, g532upper, g532p.pair.upperKind,
                                                   true, false, true, g532Stamp, hb, g504Slot);

            int32_t g532Expected[16] = {};
            if constexpr (G532Verify)
            {
                const uint8_t g532LiveKind = g422DecodeLowerKind(
                    g532lower, true, (s_g410Cache.decodeArmState & 2u) != 0u);
                if (g532LiveKind != g532p.pair.lowerKind ||
                    !g531FlagTailEligible(g532p.pair))
                    g532VerifyFail(g532Idx * 8u, g532p.pair.lowerKind, g532LiveKind, g532Mac);
                std::memcpy(g532Expected, st.vi, sizeof(g532Expected));
                switch (g532p.pair.lowerKind)
                {
                case G422_LK_FCAND:
                    g532Expected[1] = ((st.clip & (g532lower & 0xFFFFFFu)) != 0u) ? 1 : 0;
                    break;
                case G422_LK_FCOR:
                    g532Expected[1] =
                        ((st.clip | (g532lower & 0xFFFFFFu)) == 0xFFFFFFu) ? 1 : 0;
                    break;
                case G422_LK_FMAND:
                {
                    const uint8_t it = LIT(g532lower);
                    const uint8_t is = LIS(g532lower);
                    if (it != 0u)
                        g532Expected[it] = static_cast<int32_t>(
                            g532Mac & static_cast<uint32_t>(
                                static_cast<uint16_t>(g532Expected[is])));
                    break;
                }
                case G422_LK_FCGET:
                {
                    const uint8_t it = LIT(g532lower);
                    if (it != 0u)
                        g532Expected[it] = static_cast<int32_t>(st.clip & 0xFFFu);
                    break;
                }
                default:
                    g532VerifyFail(g532Idx * 8u, g532p.pair.lowerKind,
                                   g532LiveKind, g532Mac);
                }
            }

            g422FastLower(st, g532lower, g532p.pair.lowerKind, vuData, dataSize);

            if constexpr (G532Verify)
            {
                if (std::memcmp(g532Expected, st.vi, sizeof(g532Expected)) != 0)
                    g532VerifyFail(g532Idx * 8u, g532p.pair.lowerKind,
                                   g532p.pair.lowerKind, g532Mac);
                g532VerifyPass();
            }

            if ((g532p.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
            {
                _mm_storeu_ps(st.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
                st.vi[0] = 0;
            }
        }
#endif
        // G502: exactly ONE iteration unless `G502Chain`, in which case the `break` below is
        // replaced by the chain step. For every pre-existing instantiation this is a `for(;;)`
        // whose body ends in an unconditional `break`, which MSVC folds away entirely — the
        // shipped `<1,0,1,1>` body is verified byte-for-byte unchanged by `dumpbin /DISASM`.
        [[maybe_unused]] uint32_t g502Consumed = 0u;
        for (;;)
        {
        // G506: the segment loop. For every pre-existing instantiation `G506QpSeg` is false, so
        // `g506SegEnd` is `count`, the outer `while` runs once and MSVC folds it into the original
        // `for (j = 0; j < count; ++j)` — the shipped `<1,0,1,1,0,0>` body is unchanged.
        uint32_t j = 0u;
        for (;;)
        {
        if constexpr (G506QpSeg)
        {
            if (j >= count)
                break;
        }
        uint32_t g506SegEnd = count;
        if constexpr (G506QpSeg)
        {
            // A counter holding v at pair j publishes BEFORE pair j+v-1 (the per-pair form
            // decrements first and tests for zero, so the pair at which it reaches zero is the
            // first that sees the new value). v == 1 therefore publishes at THIS pair.
            if (g499Qd == 1)
            {
                g499Qd = 0;
                st.q = s_vuQPending;
            }
            if (g499Pd == 1)
            {
                g499Pd = 0;
                st.p = s_vuPPending;
            }
            uint32_t g506Run = count - j;
            if (g499Qd > 1 && static_cast<uint32_t>(g499Qd - 1) < g506Run)
                g506Run = static_cast<uint32_t>(g499Qd - 1);
            if (g499Pd > 1 && static_cast<uint32_t>(g499Pd - 1) < g506Run)
                g506Run = static_cast<uint32_t>(g499Pd - 1);
            // `g506Run >= 1` always: `j < count` gives at least one pair, and a counter above 1
            // contributes at least 1. Progress is therefore guaranteed and the loop terminates.
            g506SegEnd = j + g506Run;
            if (g499Qd > 1)
                g499Qd -= static_cast<int>(g506Run);
            if (g499Pd > 1)
                g499Pd -= static_cast<int>(g506Run);
        }
        // G517 — STRENGTH-REDUCE THE THREE INDEX EXPRESSIONS THE INNER LOOP RE-FORMS EVERY PAIR.
        //
        // The re-taken attribution (G517 §1) puts 9.4% of the whole VU1 worker on address
        // arithmetic that is a pure function of the loop counter, and every one of its inputs is on
        // the STACK because the leaf is out of registers (G483's binding resource, 29 stack
        // references in the shipped body's first 0x700):
        //
        //   +0x174  mov  r14d,eax / shl r14,4 / add r14,r9   ; &fused[firstIndex+j]   4.03%
        //   +0x59c  mov  eax,0x8340 / add rax,[rbp-0x51]     ; &g516LowCls[...] base  1.14%
        //   +0x1540 mov  esi,[rbp+0x4f] / mov r9,[rbp-0x59]  ; RELOAD firstIndex, fused  2.46%
        //   +0x4b1  mov  ecx,r15d / add rcx,[rbp+0x7f]       ; stampBase + j          4.27%
        //
        // Iterating POINTERS and a running stamp trades five live values (`firstIndex`, `fused`,
        // the cache base behind `g516LowBase`, `stampBase`, and the `firstIndex+j` scratch) for
        // three, so the arithmetic disappears AND the pressure that spilled its inputs drops.
        //
        // EXACT BY CONSTRUCTION. `*g517Fp` IS `fused[firstIndex+j]` and `g517Stamp` IS
        // `stampBase + j` at every iteration: all three are re-derived from `firstIndex`/`j` at
        // each SEGMENT entry (so the G506 segment loop and the G502 chain restart cannot desync
        // them) and advanced exactly once per pair at the bottom of a body that contains no
        // `continue` and no jump out of the loop. Nothing else moves.
        // ⚠️ NO INITIALISERS, and every use below goes through an `if constexpr` rather than a
        // ternary. The first version of this hunk wrote `= nullptr` / `= 0` and read them through
        // `G517PtrInd ? *g517Fp : …`; MSVC kept stack slots for all three on the CONTROL
        // instantiation, whose frame grew `sub rsp,0x90` → `0xa0` and whose register allocation
        // shuffled. That is G421's law — a change at a shared site is a change to the CONTROL ARM,
        // and the A/B cannot see it. Written this way the discarded `if constexpr` branch is never
        // instantiated and the control's disassembly is byte-identical to the shipped body.
#if DC2_G517_INDARM
        [[maybe_unused]] const G479FusedPair *g517Fp;
        [[maybe_unused]] const uint8_t *g517Low;
        [[maybe_unused]] int64_t g517Stamp;
        if constexpr (G517PtrInd)
        {
            g517Fp = fused + firstIndex + j;
            if constexpr (G516Low || G516Verify)
                g517Low = g516LowBase + firstIndex + j;
        }
        if constexpr (G517StampInd)
            g517Stamp = stampBase + static_cast<int64_t>(j);
#endif
        for (; j < g506SegEnd; ++j)
        {
            // G517 ORACLE (arm 22, DC2_G517_VERIFY=1). The lever's ONLY failure mode is the
            // advance running a different number of times than `++j` — an identity that a future
            // `continue` in this body would silently break. Check it where it can be caught, on
            // every pair of every executed block, and let the arm's own flag select the copy that
            // carries it (G489 §6).
#if DC2_G517_INDARM
            if constexpr (G517Verify)
                g517CheckInd(g517Fp, fused + firstIndex + j, g517Stamp,
                             stampBase + static_cast<int64_t>(j));
            const G479FusedPair *g517P;
            if constexpr (G517PtrInd)
                g517P = g517Fp;
            else
                g517P = fused + firstIndex + j;
            const G479FusedPair &fp = *g517P;
#else
            const G479FusedPair &fp = fused[firstIndex + j];
#endif
            const uint64_t src = fp.source;
            const uint32_t lower = static_cast<uint32_t>(src);
            const uint32_t upper = static_cast<uint32_t>(src >> 32);

            if constexpr (G506QpSeg)
            {
                // Nothing: the segment boundary above already published everything this run of
                // pairs can observe, and no block-eligible pair can re-arm either counter.
            }
            else if constexpr (G499QpLocal)
            {
                if (g499Qd > 0 && --g499Qd == 0)
                    st.q = s_vuQPending;
                if (g499Pd > 0 && --g499Pd == 0)
                    st.p = s_vuPPending;
            }
            else
            {
                if (*qDelay > 0 && --(*qDelay) == 0)
                    st.q = s_vuQPending;
                if (*pDelay > 0 && --(*pDelay) == 0)
                    st.p = s_vuPPending;
            }

            // `g443Fixed` pins the four trailing arguments the pair loop passes: g428DeferPark
            // true, shadow false, g430Hist true, and the hoisted history block. Eligibility
            // guarantees `cls != 0`, so the return value is always true and the legacy
            // execUpper fallback the macro carries is unreachable here.
            if constexpr (G516Verify)
            {
#if DC2_G517_INDARM
                if constexpr (G517PtrInd)
                    g516CheckLow(*g517Low, fp.pair);
                else
#endif
                    g516CheckLow(g516LowBase[firstIndex + j], fp.pair);
            }
            if constexpr (G610Ceil == 2 || G610Ceil == 3)
            {
                // G610 ceiling: the UPPER slot and its G430 history push are deleted.
            }
            else if constexpr (G516Up)
            {
                // ONE 32-bit load whose address is the PAIR INDEX. `cls == 0` is `upperKind ==
                // 0x6f` for a block-eligible pair (eligibility admits VNOP or `cls != 0`, and
                // `e[0x6f]` is the zero entry), so this is the same predicate on the same pairs —
                // with the descriptor already in a register instead of two dependent loads away.
                const uint32_t g516Bits = g516UpBase[firstIndex + j];
                if (static_cast<uint8_t>(g516Bits) != 0u)
                    g421FastUpper<G501Pin, G504Cursor, true>(
                        st, upper, 0u, true, false, true,
                        stampBase + static_cast<int64_t>(j), hb, g504Slot, g516Bits);
            }
#if DC2_G517_INDARM
            // G517: the ONE per-pair use of the stamp. Written as `if constexpr` and not as a
            // ternary so the control instantiation never mentions `g517Stamp` (see the frame note
            // at its declaration).
            else if (fp.pair.upperKind != 0x6fu)
            {
                if constexpr (G517StampInd)
                    g421FastUpper<G501Pin, G504Cursor>(st, upper, fp.pair.upperKind, true, false,
                                                       true, g517Stamp, hb, g504Slot);
                else
                    g421FastUpper<G501Pin, G504Cursor>(st, upper, fp.pair.upperKind, true, false,
                                                       true,
                                                       stampBase + static_cast<int64_t>(j), hb,
                                                       g504Slot);
            }
#else
            else if (fp.pair.upperKind != 0x6fu)
                g421FastUpper<G501Pin, G504Cursor>(st, upper, fp.pair.upperKind, true, false, true,
                                                   stampBase + static_cast<int64_t>(j), hb,
                                                   g504Slot);
#endif

            if constexpr (G610Ceil == 1 || G610Ceil == 3)
            {
                // G610 ceiling: the LOWER slot is deleted.
            }
            else if constexpr (G499FastLower)
            {
                // G480: g422FastLower's NOP case is exactly `return;` — do not call it.
                // G481: the three block-eligible hot integer kinds run inline; no call at all.
                // (IBNE is the fourth kind the pair loop inlines and is a branch, i.e. never
                // block-eligible, so it cannot appear here.) Same tokens as the callee's cases.
                // G516: the selector is the arm's own encoding of the same ten kinds — the raw
                // `lowerKind` for every pre-existing instantiation, the dense class for the G516
                // arm. Read AT THE POINT OF USE, never hoisted: v1 copied an 8-byte record into a
                // local at the pair fetch and held it to here, ~1,100 bytes of code later, and
                // lost 8.26% to the extra live value (G483's law, measured).
                uint8_t g499kind;
#if DC2_G517_INDARM
                if constexpr (G516Low && G517PtrInd)
                    g499kind = *g517Low;
                else
#endif
                if constexpr (G516Low)
                    g499kind = g516LowBase[firstIndex + j];
                else
                    g499kind = fp.pair.lowerKind;
                // G515: PEEL THE DOMINANT KIND OUT OF THE INDIRECT DISPATCH.
                //
                // MSVC compiles the switch below to a TWO-LEVEL jump table, and the shipped leaf's
                // annotated disassembly (`tools/g504_disasm.py`, +0x57d..+0x5a3) puts 16.89% of the
                // WHOLE VU1 worker on its seven instructions:
                //
                //   movzx edx,[rsi+0xe]                      ; fp.pair.lowerKind
                //   lea   eax,[rdx-1] / cmp eax,0x1b / ja    ; kinds 1..28 only
                //   movzx eax,[r8+rax+0x31014c]              ; table 1: index byte   <- depends on ^
                //   mov   ecx,[r8+rax*4+0x310120]            ; table 2: rel32 target <- depends on ^
                //   add   rcx,r8                             ; 7.17% of the thread
                //   jmp   rcx                                ; 28-way indirect
                //
                // Three dependent L1 loads feeding a 28-way indirect branch, and `[G479:lk]` says
                // the single most common destination — 40.2% of ALL pairs, ~44% of block-eligible
                // ones once the never-eligible control transfers are excluded — is `G422_LK_NOP`,
                // whose body is `break`. Roughly 630 k pairs per frame walk the whole chain and
                // take the indirect jump to reach nothing.
                //
                // The peel replaces that, for those pairs, with one compare and one DIRECT branch
                // off a byte the loop has already loaded. It also removes the loop's most frequent
                // target from the indirect predictor's stream, which is why it can help the other
                // 56% as well.
                //
                // EXACT BY CONSTRUCTION, no oracle needed: the guarded statement set for
                // `g499kind == G422_LK_NOP` is `case G422_LK_NOP: break;`, i.e. empty. Nothing
                // before the switch and nothing after it (the G425 zero-register re-pin) moves.
                // `case G422_LK_NOP` deliberately STAYS in the switch, so the control arm's jump
                // table is bit-identical to the shipped one and only the guard differs.
                //
                // The condition is written `!G515NopPeel || ...` so the control instantiation
                // folds it to `true` and emits no test at all (G436: a selector inside the leaf
                // would sit on both arms and bias the gate).
                if (!G515NopPeel || g499kind != G516K_NOP)
                {
                // G516 `peel4` — THE DIRECT TEST OF WHAT G515 ACTUALLY WON.
                //
                // G515's account of its own win was "three dependent L1 loads feeding an indirect
                // branch". G516's `lowdense` arm deleted one of those load levels and measured
                // NOTHING, and `updesc` deleted the other chain outright and measured +9.9% — so
                // the loads were never the mechanism. What is left is the 28-way INDIRECT BRANCH:
                // G515 took 40.2% of all pairs out of it and gave them a direct, perfectly
                // predicted one, and that is where its 2.0 host cycles per peeled pair came from.
                //
                // If that reading is right, peeling MORE of the branch must pay again. These are
                // the four biggest remaining destinations by `[G479:lk]` share of ALL pairs —
                // LQ 9.37 + SQI 8.16 + IADDI 6.99 + LQI 6.78 = **31.3%, ~507 k pairs/frame** —
                // and at G515's realised rate that is worth ~0.22 ms/f.
                //
                // EXACT BY CONSTRUCTION, and with NO DUPLICATED BODY: each test branches to a
                // label placed on the switch's own `case`, so the statements executed are the same
                // statements in the same order, and `break` still leaves the switch. (G514's
                // case-duplication trap is avoided for the same reason its `goto g514Slow` avoided
                // it.) The tests are inside the `if constexpr` arm, so the control instantiation
                // does not contain them at all.
                if constexpr (G516Peel4)
                {
                    if (g499kind == G516K_LQ)
                        goto g516PeelLQ;
                    if (g499kind == G516K_SQI)
                        goto g516PeelSQI;
                    if (g499kind == G516K_IADDI)
                        goto g516PeelIADDI;
                    if (g499kind == G516K_LQI)
                        goto g516PeelLQI;
                }
                switch (g499kind)
                {
                case G516K_NOP:
                    break;
                case G516K_IADDI:
                g516PeelIADDI:
                    G481_LOWER_BODY_IADDI(st, lower);
                    break;
                case G516K_IADDIU:
                    G481_LOWER_BODY_IADDIU(st, lower);
                    break;
                case G516K_IAND:
                    G481_LOWER_BODY_IAND(st, lower);
                    break;
                // G514: the three VU-data-memory STORE kinds. When the arm is off these three
                // labels fall into exactly the `default:` statement below, so the control arm's
                // jump table entries are unchanged and MSVC folds the labels away — the shipped
                // and control instantiations are byte-identical (verified by /MAP size).
                case G516K_SQ:
                    if constexpr (G514Verify)
                    {
                        const G514Ref g514r = g514MakeRef(st, lower, G422_LK_SQ, vuData, dataSize);
                        G514_LOWER_BODY_SQ(st, lower, vuData, dataSize);
                        g514Check(g514r, G422_LK_SQ, lower, vuData);
                        break;
                    }
                    else if constexpr (G514VStore)
                    {
                        G514_LOWER_BODY_SQ(st, lower, vuData, dataSize);
                        break;
                    }
                    else
                        goto g514Slow;
                case G516K_SQI:
                g516PeelSQI:
                    if constexpr (G514Verify)
                    {
                        const G514Ref g514r = g514MakeRef(st, lower, G422_LK_SQI, vuData, dataSize);
                        G514_LOWER_BODY_SQI(st, lower, vuData, dataSize);
                        g514Check(g514r, G422_LK_SQI, lower, vuData);
                        break;
                    }
                    else if constexpr (G514VStore)
                    {
                        G514_LOWER_BODY_SQI(st, lower, vuData, dataSize);
                        break;
                    }
                    else
                        goto g514Slow;
                case G516K_SQD:
                    if constexpr (G514Verify)
                    {
                        const G514Ref g514r = g514MakeRef(st, lower, G422_LK_SQD, vuData, dataSize);
                        G514_LOWER_BODY_SQD(st, lower, vuData, dataSize);
                        g514Check(g514r, G422_LK_SQD, lower, vuData);
                        break;
                    }
                    else if constexpr (G514VStore)
                    {
                        G514_LOWER_BODY_SQD(st, lower, vuData, dataSize);
                        break;
                    }
                    else
                        goto g514Slow;
                case G516K_LQ:
                g516PeelLQ:
                    if constexpr (G514Verify)
                    {
                        const G514Ref g514l =
                            g514MakeLoadRef(st, lower, G422_LK_LQ, vuData, dataSize);
                        G514_LOWER_BODY_LQ(st, lower, vuData, dataSize);
                        g514CheckLoad(g514l, G422_LK_LQ, st);
                        break;
                    }
                    else if constexpr (G514VLoad)
                    {
                        G514_LOWER_BODY_LQ(st, lower, vuData, dataSize);
                        break;
                    }
                    else
                        goto g514Slow;
                case G516K_LQI:
                g516PeelLQI:
                    if constexpr (G514Verify)
                    {
                        const G514Ref g514l =
                            g514MakeLoadRef(st, lower, G422_LK_LQI, vuData, dataSize);
                        G514_LOWER_BODY_LQI(st, lower, vuData, dataSize);
                        g514CheckLoad(g514l, G422_LK_LQI, st);
                        break;
                    }
                    else if constexpr (G514VLoad)
                    {
                        G514_LOWER_BODY_LQI(st, lower, vuData, dataSize);
                        break;
                    }
                    else
                        goto g514Slow;
                case G516K_LQD:
                    if constexpr (G514Verify)
                    {
                        const G514Ref g514l =
                            g514MakeLoadRef(st, lower, G422_LK_LQD, vuData, dataSize);
                        G514_LOWER_BODY_LQD(st, lower, vuData, dataSize);
                        g514CheckLoad(g514l, G422_LK_LQD, st);
                        break;
                    }
                    else if constexpr (G514VLoad)
                    {
                        G514_LOWER_BODY_LQD(st, lower, vuData, dataSize);
                        break;
                    }
                    else
                        goto g514Slow;
                // G514: the six labels above must share THIS tail, not get a copy of it each.
                // With `g422FastLower(...); break;` written out three times MSVC emitted three
                // copies and the CONTROL arm (G514VStore false) grew 1,728 bytes over the
                // pre-G514 shipped body — 1,728 bytes of duplicated slow tail sitting on exactly
                // the three paths the gate is measuring. The `goto` restores a single tail and
                // makes the control instantiation byte-size-identical to what shipped before.
                default:
                g514Slow:
                    // G516: the callee needs the REAL kind, and on this arm `g499kind` is the
                    // dense class. The fused byte is loaded here and only here — on the ~24% of
                    // block pairs that were going to pay an out-of-line call anyway, where one
                    // more L1 hit in front of a `call` is not on any critical path.
                    if constexpr (G516Low)
                        g422FastLower(st, lower, fp.pair.lowerKind, vuData, dataSize);
                    else
                        g422FastLower(st, lower, g499kind, vuData, dataSize);
                    break;
                }
                } // G515: closes the NOP peel guard above.
            }
            else
                g422FastLower(st, lower, fp.pair.lowerKind, vuData, dataSize);

            if ((fp.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
            {
                // G484's promoted wide form, byte-for-byte with the pair loop's re-pin.
                _mm_storeu_ps(st.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
                st.vi[0] = 0;
            }
#if DC2_G517_INDARM
            // G517: the induction step. Placed at the bottom rather than in the `for`'s iteration
            // expression only because the latter would have to write `+= 0` on the control arm; the
            // body has no `continue` and no jump that leaves it, so this runs exactly once per `++j`
            // — the invariant the whole lever rests on. Absent from every other instantiation.
            if constexpr (G517PtrInd)
            {
                ++g517Fp;
                if constexpr (G516Low || G516Verify)
                    ++g517Low;
            }
            if constexpr (G517StampInd)
                ++g517Stamp;
#endif
        }
        // G506: for every non-segmented instantiation this is an unconditional `break` closing a
        // `for(;;)`, which MSVC folds away entirely — the same shape, and the same guarantee, the
        // G502 chain loop above already relies on.
        if constexpr (!G506QpSeg)
            break;
        }
        if constexpr (G531FlagTail)
        {
            // G531: typed tail 3 is one of G487's four compiled flag consumers. The generic loop
            // advances Q/P, publishes the delayed MAC line, executes upper, then lower. Reproduce
            // that order exactly; ordinary block pairs and G500 branch tails keep their old body.
            if (g500TailCount == G531_FLAG_TAIL)
            {
                const uint32_t g531Idx = firstIndex + count;
                const G479FusedPair &g531p = fused[g531Idx];
                const uint64_t g531src = g531p.source;
                const uint32_t g531lower = static_cast<uint32_t>(g531src);
                const uint32_t g531upper = static_cast<uint32_t>(g531src >> 32u);

                if constexpr (G506Local)
                {
                    if (g499Qd > 0 && --g499Qd == 0)
                        st.q = s_vuQPending;
                    if (g499Pd > 0 && --g499Pd == 0)
                        st.p = s_vuPPending;
                }
                else
                {
                    if (*qDelay > 0 && --(*qDelay) == 0)
                        st.q = s_vuQPending;
                    if (*pDelay > 0 && --(*pDelay) == 0)
                        st.p = s_vuPPending;
                }

                // This executor is reachable only through g443Fixed, whose gate proves the
                // promoted MAC-history depth is four. Use the TLS object directly: adding a
                // pointer/depth argument would grow every block call for a one-pair tail.
                const uint32_t g531Mac =
                    g430ResolveAt(stampBase + static_cast<int64_t>(count) - 4);
                s_vuMacVisible = g531Mac;

                if (g531p.pair.upperKind != 0x6fu)
                    g421FastUpper<G501Pin, G504Cursor>(
                        st, g531upper, g531p.pair.upperKind, true, false, true,
                        stampBase + static_cast<int64_t>(count), hb, g504Slot);

                int32_t g531Expected[16] = {};
                if constexpr (G531Verify)
                {
                    const uint8_t g531LiveKind = g422DecodeLowerKind(
                        g531lower, true, (s_g410Cache.decodeArmState & 2u) != 0u);
                    if (g531LiveKind != g531p.pair.lowerKind ||
                        !g531FlagTailEligible(g531p.pair))
                        g531VerifyFail(g531Idx * 8u, g531p.pair.lowerKind, g531LiveKind, g531Mac);
                    std::memcpy(g531Expected, st.vi, sizeof(g531Expected));
                    switch (g531p.pair.lowerKind)
                    {
                    case G422_LK_FCAND:
                        g531Expected[1] = ((st.clip & (g531lower & 0xFFFFFFu)) != 0u) ? 1 : 0;
                        break;
                    case G422_LK_FCOR:
                        g531Expected[1] =
                            ((st.clip | (g531lower & 0xFFFFFFu)) == 0xFFFFFFu) ? 1 : 0;
                        break;
                    case G422_LK_FMAND:
                    {
                        const uint8_t it = LIT(g531lower);
                        const uint8_t is = LIS(g531lower);
                        if (it != 0u)
                            g531Expected[it] = static_cast<int32_t>(
                                g531Mac & static_cast<uint32_t>(
                                    static_cast<uint16_t>(g531Expected[is])));
                        break;
                    }
                    case G422_LK_FCGET:
                    {
                        const uint8_t it = LIT(g531lower);
                        if (it != 0u)
                            g531Expected[it] = static_cast<int32_t>(st.clip & 0xFFFu);
                        break;
                    }
                    default:
                        g531VerifyFail(g531Idx * 8u, g531p.pair.lowerKind, g531LiveKind, g531Mac);
                    }
                }

                g422FastLower(st, g531lower, g531p.pair.lowerKind, vuData, dataSize);

                if constexpr (G531Verify)
                {
                    if (std::memcmp(g531Expected, st.vi, sizeof(g531Expected)) != 0)
                        g531VerifyFail(g531Idx * 8u, g531p.pair.lowerKind,
                                       g531p.pair.lowerKind, g531Mac);
                    g531VerifyPass();
                }

                if ((g531p.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
                {
                    _mm_storeu_ps(st.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
                    st.vi[0] = 0;
                }
            }
        }
        if constexpr (G500Tail)
        {
            // G500. `g500TailCount` is 0, 1 or 2 and the caller has already proved, from the
            // precisely invalidated sidecar, that slot `firstIndex + count` is an admissible
            // control transfer and (for 2) that slot `firstIndex + count + 1` is a plain eligible
            // pair. Statement order is the plain pair's, and the two additions are the pair loop's
            // own branch protocol — see the exactness note at the head of this file.
            if (g500TailCount != 0u && (!G531FlagTail || g500TailCount != G531_FLAG_TAIL))
            {
                const uint32_t g500Idx = firstIndex + count;
                const G479FusedPair &bp = fused[g500Idx];
                const uint64_t bsrc = bp.source;
                const uint32_t blower = static_cast<uint32_t>(bsrc);
                const uint32_t bupper = static_cast<uint32_t>(bsrc >> 32);

                // G506: the tail is at most two pairs, so it keeps the per-pair form — segmenting
                // it would buy nothing and the branch pair is the one place `st.pc` is written.
                // It reads the SEGMENTED arm's locals, which the loop above left exact.
                if constexpr (G506Local)
                {
                    if (g499Qd > 0 && --g499Qd == 0)
                        st.q = s_vuQPending;
                    if (g499Pd > 0 && --g499Pd == 0)
                        st.p = s_vuPPending;
                }
                else
                {
                    if (*qDelay > 0 && --(*qDelay) == 0)
                        st.q = s_vuQPending;
                    if (*pDelay > 0 && --(*pDelay) == 0)
                        st.p = s_vuPPending;
                }

                if (bp.pair.upperKind != 0x6fu)
                    g421FastUpper<G501Pin, G504Cursor>(st, bupper, bp.pair.upperKind, true, false,
                                                       true,
                                                       stampBase + static_cast<int64_t>(count), hb,
                                                       g504Slot);

                // The architectural PC of THIS pair, which every branch body reads (and BAL/JALR
                // write back as `(st.pc + 16) / 8`). One store per block, not per pair.
                st.pc = g500Idx * 8u;
                // IBNE is 4.41% of all pairs — half of every control transfer on this route — and
                // the pair loop has run it inline since G481. Same tokens as g422FastLower's own
                // case (vu1_g481_hot_lower.inc), so the executed statements are identical.
                // (The G481 bodies are brace blocks, not do/while macros — hence the explicit
                // braces on both arms.)
                if (bp.pair.lowerKind == G422_LK_IBNE)
                {
                    G481_LOWER_BODY_IBNE(st, blower);
                }
                else
                {
                    g422FastLower(st, blower, bp.pair.lowerKind, vuData, dataSize);
                }

                if ((bp.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
                {
                    _mm_storeu_ps(st.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
                    st.vi[0] = 0;
                }

                if (g500TailCount == 2u)
                {
                    // The DELAY SLOT. A plain eligible pair by construction; nothing it can
                    // execute reads `st.pc`, so the branch's write above is invisible to it and
                    // the caller still finds the branch outcome there.
                    const G479FusedPair &dp = fused[g500Idx + 1u];
                    const uint64_t dsrc = dp.source;
                    const uint32_t dlower = static_cast<uint32_t>(dsrc);
                    const uint32_t dupper = static_cast<uint32_t>(dsrc >> 32);

                    if constexpr (G506Local)
                    {
                        if (g499Qd > 0 && --g499Qd == 0)
                            st.q = s_vuQPending;
                        if (g499Pd > 0 && --g499Pd == 0)
                            st.p = s_vuPPending;
                    }
                    else
                    {
                        if (*qDelay > 0 && --(*qDelay) == 0)
                            st.q = s_vuQPending;
                        if (*pDelay > 0 && --(*pDelay) == 0)
                            st.p = s_vuPPending;
                    }

                    if (dp.pair.upperKind != 0x6fu)
                        g421FastUpper<G501Pin, G504Cursor>(
                            st, dupper, dp.pair.upperKind, true, false, true,
                            stampBase + static_cast<int64_t>(count) + 1, hb, g504Slot);

                    if constexpr (G499FastLower)
                    {
                        const uint8_t g500kind = dp.pair.lowerKind;
                        switch (g500kind)
                        {
                        case G422_LK_NOP:
                            break;
                        case G422_LK_IADDI:
                            G481_LOWER_BODY_IADDI(st, dlower);
                            break;
                        case G422_LK_IADDIU:
                            G481_LOWER_BODY_IADDIU(st, dlower);
                            break;
                        case G422_LK_IAND:
                            G481_LOWER_BODY_IAND(st, dlower);
                            break;
                        default:
                            g422FastLower(st, dlower, g500kind, vuData, dataSize);
                            break;
                        }
                    }
                    else
                        g422FastLower(st, dlower, dp.pair.lowerKind, vuData, dataSize);

                    if ((dp.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
                    {
                        _mm_storeu_ps(st.vf[0], _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f));
                        st.vi[0] = 0;
                    }
                }
            }
        }
        if constexpr (!G502Chain)
            break;
        else
        {
            // ------------------------------------------------------------------------------
            // G502 CHAIN STEP. Everything here is the pair loop's own next-iteration logic,
            // evaluated once per SEGMENT instead of once per block round trip. See the exactness
            // note at the head of this template.
            // ------------------------------------------------------------------------------
            g502Out->segments += 1u;
            g502Out->plain += count;
            if (g500TailCount == 1u)
                ++g502Out->tail1;
            else if (g500TailCount == 2u)
                ++g502Out->tail2;
            g502Consumed += count + g500TailCount;

            // The pair loop's own next-pc fold, with the same taken test and the same wrap.
            uint32_t g502Next;
            bool g502Pending = false;
            uint32_t g502Target = 0u;
            if (g500TailCount == 0u)
                g502Next = (firstIndex + count) * 8u;
            else
            {
                const uint32_t g502BPc = (firstIndex + count) * 8u;
                const bool g502Taken = (st.pc != g502BPc);
                if (g502Taken)
                    ++g502Out->taken;
                if (g500TailCount == 2u)
                    g502Next = g502Taken ? (st.pc + 8u) : (g502BPc + 16u);
                else
                {
                    if (g502Taken)
                    {
                        g502Target = st.pc + 8u;
                        if (g502Target >= g502CodeSize)
                            g502Target = 0u;
                        g502Pending = true;
                    }
                    g502Next = g502BPc + 8u;
                }
            }
            if (g502Next >= g502CodeSize)
                g502Next = 0u;

            // Can this chain continue? Exactly run()'s hook, one iteration later.
            if (!g502Pending && g502Consumed < g502Budget && g502Next + 8u <= g502CodeSize)
            {
                const uint32_t g502Rem = g502Budget - g502Consumed;
                const uint32_t g502Avail =
                    static_cast<uint32_t>(s_g410Cache.g490RunLen[g502Next >> 3u]);
                const uint32_t g502Count = g502Avail < g502Rem ? g502Avail : g502Rem;
                if (g502Count >= G490_MIN_RUN)
                {
                    uint32_t g502T = 0u;
                    if (g502Count == g502Avail)
                    {
                        const uint32_t g502TPc = g502Next + 8u * g502Avail;
                        if (g502TPc + 8u <= g502CodeSize)
                        {
                            g502T = static_cast<uint32_t>(
                                s_g410Cache.g500Tail[g502TPc >> 3u]);
                            // G531's typed tail is supported only by arms 23/24. The retired G502
                            // chain body is arm 5 and interprets nonzero values as branch tails.
                            if (g502T == G531_FLAG_TAIL)
                                g502T = 0u;
                            if (g502T == 2u && g502TPc + 16u > g502CodeSize)
                                g502T = 1u;
                            if (g502Avail + g502T > g502Rem)
                                g502T = 0u;
                        }
                    }
                    stampBase += static_cast<int64_t>(count) +
                                 static_cast<int64_t>(g500TailCount);
                    firstIndex = g502Next >> 3u;
                    count = g502Count;
                    g500TailCount = g502T;
                    continue;
                }
            }
            g502Out->consumed = g502Consumed;
            g502Out->nextPc = g502Next;
            g502Out->pendingValid = g502Pending;
            g502Out->pendingTarget = g502Target;
            break;
        }
        }
        if constexpr (G506Local)
        {
            // Exactly the values the pointer form would have left: neither counter can be
            // RE-ARMED inside a block (see the eligibility argument above), so a single write-back
            // is equivalent to the per-pair stores, and every consumer of `*qDelay`/`*pDelay`
            // lives outside this call.
            *qDelay = g499Qd;
            *pDelay = g499Pd;
        }
        // G504: the ring cursor's single write-back. This is the ONLY exit of the function — the
        // `for(;;)` above is either folded away entirely (every non-chain instantiation ends its
        // body in an unconditional `break`) or left through the chain step's `break`, and there is
        // no `return` anywhere in the body. Every reader of `H.head` outside this call
        // (`g430PushPending` in the pair loop, `g430PushEager`, run()'s prologue) therefore sees
        // exactly the cursor the per-push form would have left.
        if constexpr (G504Cursor)
        {
            // ORACLE (DC2_G504_VERIFY=1, default off). The whole lever rests on one claim: NOTHING
            // else writes `H.head` while a block runs. That claim is checkable directly — if it
            // holds, `hb->head` is still the value read at entry. This catches the failure the
            // structural argument could get wrong (a lower kind that reaches the history, a future
            // caller that pushes from inside the leaf) far more sharply than any output compare.
            //
            // It cannot be checked with `DC2_G430_VERIFY=1` instead: that flag is a conjunct of
            // `g443Fixed` (vu1_upper_opcodes.inc), so arming it drops the whole run onto a loop copy
            // that HAS NO BLOCK HOOK — the G500 §2 / G501 §9 "the census flag selects a different
            // loop copy" trap. One branch per BLOCK (~158 k/frame, never taken), inside a leaf whose
            // entry already costs 1.89 ns, so the gate is not measurably biased by it — and what
            // bias there is runs AGAINST the candidate, since only this arm carries the test.
#if DC2_G504_VERIFY_CUR
            if (g504VerifyOn() && hb->head != g504Entry)
            {
                static std::atomic<uint32_t> s_bad{0};
                const uint32_t n = s_bad.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 32u)
                    std::fprintf(stderr,
                                 "[G504:verify-BAD] head moved under a block: entry=%d now=%d "
                                 "local=%d firstIndex=%u count=%u n=%u\n",
                                 g504Entry, hb->head, g504Slot, firstIndex, count, n);
            }
#endif
            hb->head = g504Slot;
        }
    }

    // The three instantiations. `noinline` for the reason recorded above the body: a second
    // inlined copy of the SIMD upper + fast lower inside run() would be paid by every pair to help
    // a subset. `<true,true>` is the shipped default; `<true,false>` is the `qpblock` gate's
    // control arm; `<false,false>` is the pre-G499 body and the `DC2_G499_NO_BLKLOWER` rollback.
    // The fourth combination `<false,true>` is deliberately NOT instantiated — no gate needs it,
    // and a copy nothing selects is text nothing pays for (G490).
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g490ExecuteBlock(VU1State &st, uint32_t firstIndex, uint32_t count,
                          uint8_t *vuData, uint32_t dataSize,
                          int *qDelay, int *pDelay,
                          int64_t stampBase, G430HistBlock *hb)
    {
        g490BlockBody<false, false>(st, firstIndex, count, vuData, dataSize, qDelay, pDelay,
                                    stampBase, hb);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g499ExecuteBlockFast(VU1State &st, uint32_t firstIndex, uint32_t count,
                              uint8_t *vuData, uint32_t dataSize,
                              int *qDelay, int *pDelay,
                              int64_t stampBase, G430HistBlock *hb)
    {
        g490BlockBody<true, false>(st, firstIndex, count, vuData, dataSize, qDelay, pDelay,
                                   stampBase, hb);
    }

#if DC2_G500_BLKBRANCH
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g500ExecuteBlockTail(VU1State &st, uint32_t firstIndex, uint32_t count,
                              uint8_t *vuData, uint32_t dataSize,
                              int *qDelay, int *pDelay,
                              int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        // G501: `G501Pin` is UNCONDITIONAL here — see the note at `g490BlockBody`. It is not an
        // arm: it deletes code `g443Fixed` proves unreachable, so there is nothing for a control
        // arm to be different about, and the pinned instantiation is strictly smaller
        // (0x3364 vs 0x3538) with eleven fewer calls. The rollback is the one that already
        // existed — `DC2_G436_LEGACY_ADDR=1` or `DC2_G428_NO_DEST_STORE=1` falsifies `g443Fixed`
        // and drops the whole run onto the legacy loop copy, which has no block hook at all.
        g490BlockBody<true, false, true, true>(st, firstIndex, count, vuData, dataSize, qDelay,
                                               pDelay, stampBase, hb, tailCount);
    }

#if DC2_G504_HISTCUR
    // G504: arm 3's body with the G430 ring cursor in a block local. A separate INSTANTIATION rather
    // than a runtime `if` for the same reason as every other arm in this file (G436): a selector
    // inside the leaf would sit on BOTH arms and bias the gate. The wrapper is `noinline` so run()'s
    // own text is byte-identical between the two arms — only which leaf is called differs.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g504ExecuteBlockTailCur(VU1State &st, uint32_t firstIndex, uint32_t count,
                                 uint8_t *vuData, uint32_t dataSize,
                                 int *qDelay, int *pDelay,
                                 int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, true>(st, firstIndex, count, vuData, dataSize,
                                                            qDelay, pDelay, stampBase, hb,
                                                            tailCount);
    }
#endif // DC2_G504_HISTCUR

#if DC2_G506_QPSEG
    // G506: arm 3's body with the Q/P delay advance hoisted out of the per-pair loop by
    // segmentation. A separate INSTANTIATION rather than a runtime `if` for the same reason as every
    // other arm in this file (G436): a selector inside the leaf would sit on BOTH arms and bias the
    // gate. The wrapper is `noinline` so run()'s own text is byte-identical between the two arms —
    // only which leaf is called differs.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g506ExecuteBlockTailQpSeg(VU1State &st, uint32_t firstIndex, uint32_t count,
                                   uint8_t *vuData, uint32_t dataSize,
                                   int *qDelay, int *pDelay,
                                   int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

    // G514: arm 7's body with the three VU-data-memory STORE kinds writing 16 bytes in one store
    // instead of staging them through a `tmp[4]` stack buffer. A separate INSTANTIATION rather
    // than a runtime `if` for the same reason as every other arm in this file (G436): a selector
    // inside the leaf would sit on BOTH arms and bias the gate. The wrapper is `noinline` so
    // run()'s own text is byte-identical between the two arms.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g514ExecuteBlockTailVStore(VU1State &st, uint32_t firstIndex, uint32_t count,
                                    uint8_t *vuData, uint32_t dataSize,
                                    int *qDelay, int *pDelay,
                                    int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, false>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

    // G514b: arm 8's body with the three VU-data-memory LOAD kinds writing `st.vf[it]` as ONE
    // 16-byte store instead of up to four `movss` lane stores. Control arm is 8, so the load side
    // is priced alone.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g514ExecuteBlockTailVMem(VU1State &st, uint32_t firstIndex, uint32_t count,
                                  uint8_t *vuData, uint32_t dataSize,
                                  int *qDelay, int *pDelay,
                                  int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

    // G514 ORACLE COPY — the candidate body with the per-store reference comparison
    // (DC2_G514_VERIFY=1). Never a timing arm: its flag is what selects it, so it cannot end up in
    // a copy the flag deselects (G489 §6), and the shipped body carries none of its live values.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g514ExecuteBlockTailVerify(VU1State &st, uint32_t firstIndex, uint32_t count,
                                    uint8_t *vuData, uint32_t dataSize,
                                    int *qDelay, int *pDelay,
                                    int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

    // G515: arm 12 — arm 10's body (the G514 shipped default) with `G422_LK_NOP` peeled out of the
    // two-level jump table into one compare and one direct branch. Control arm is 10, so the two
    // arms execute the identical pairs in the identical blocks and differ only in HOW the loop
    // reaches the empty body of the single most common lower kind. Separate INSTANTIATION rather
    // than a runtime `if` (G436), and a `noinline` wrapper so run()'s own text is byte-identical
    // between the arms (G481's inline-budget cliff binds on run(), not on a leaf).
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g515ExecuteBlockTailNopPeel(VU1State &st, uint32_t firstIndex, uint32_t count,
                                     uint8_t *vuData, uint32_t dataSize,
                                     int *qDelay, int *pDelay,
                                     int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

    // G516: arms 13 / 14 / 15 — arm 12's body with the UPPER descriptor, the LOWER dense class, or
    // both, read out of the decode-time G516 arrays instead of chased through a
    // dependent table lookup. Separate INSTANTIATIONS rather than runtime `if`s for the same reason
    // as every other arm in this file (G436), and `noinline` wrappers so run()'s own text is
    // byte-identical between the arms.
#if DC2_G516_UPDESC
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g516ExecuteBlockTailUpDesc(VU1State &st, uint32_t firstIndex, uint32_t count,
                                    uint8_t *vuData, uint32_t dataSize,
                                    int *qDelay, int *pDelay,
                                    int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      true, false>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }
#endif // DC2_G516_UPDESC

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g516ExecuteBlockTailLowDense(VU1State &st, uint32_t firstIndex, uint32_t count,
                                      uint8_t *vuData, uint32_t dataSize,
                                      int *qDelay, int *pDelay,
                                      int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

    // G516 PROMOTION CANDIDATE — arm 18 = arm 12's body with BOTH surviving mechanisms: the dense
    // 1..10 lower class (one jump-table level fewer) and the four peeled indirect-branch
    // destinations. Exact by construction on both halves; oracle DC2_G516_VERIFY=1 (arm 16).
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g516ExecuteBlockTailDisp(VU1State &st, uint32_t firstIndex, uint32_t count,
                                  uint8_t *vuData, uint32_t dataSize,
                                  int *qDelay, int *pDelay,
                                  int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

    // G531 arm 23: the G516 shipped body plus one typed flag-consumer tail. Arm 24 is the same
    // executor with an independent live-word decode and VI-state oracle; it is never a timing arm.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g531ExecuteBlockTailFlag(VU1State &st, uint32_t firstIndex, uint32_t count,
                                  uint8_t *vuData, uint32_t dataSize,
                                  int *qDelay, int *pDelay,
                                  int64_t stampBase, G430HistBlock *hb, uint32_t tailCode)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, false, false, false, true, false>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCode);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g531ExecuteBlockTailVerify(VU1State &st, uint32_t firstIndex, uint32_t count,
                                    uint8_t *vuData, uint32_t dataSize,
                                    int *qDelay, int *pDelay,
                                    int64_t stampBase, G430HistBlock *hb, uint32_t tailCode)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, false, false, false, true, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCode);
    }

    // =========================================================================================
    // G610 — THE COMPILED-BLOCK ENTRY.
    //
    // Lives here, beside `g490ExecuteBlock`, because the oracle has to be able to call the shipped
    // GENERIC executor; the encoder and translator live in vu1_g610_native_jit.inc, which is
    // included earlier (it needs only the G410 cache and the G421 descriptor table).
    //
    // EVERYTHING IN THIS FUNCTION IS PER BLOCK, NEVER PER PAIR. It is `noinline` for the same
    // reason every other block entry is (G481's inline-budget cliff binds `run()`'s text), and the
    // hook only calls it when the slot ALREADY holds a compiled body — so the fallback population
    // pays one array load and one test, not a call.
    //
    // Preconditions 2 and 3 of the header are enforced here:
    //   * `tailCode != 0` -> the generic body owns branch and flag-consumer tails (tested by the
    //     caller, which has `g500Tail` live).
    //   * a Q/P segment split inside the block -> `st.q`/`st.p` would be republished mid-block and
    //     the compiled body has them baked as "constant across the block". Tested BEFORE anything
    //     is mutated, so a refusal leaves the caller's state exactly as it found it.
    // =========================================================================================
#if DC2_G610_JIT_BACKEND
    // The Q/P segment arithmetic of `g490BlockBody`'s G506QpSeg arm, folded to `j == 0` and
    // `g506Run == count`.
    //
    // ⭐ AN UNOBSERVED PUBLICATION DOES NOT NEED A SEGMENT. The first revision refused any block in
    // which either counter would publish mid-block, and the census read `fbSeg = 2,402,989` against
    // 5.26 M native blocks — 31% of every block entry, thrown away. But the publication is only
    // OBSERVABLE if a pair between the block entry and the publishing pair reads the register, and:
    //
    //   * `st.p` is read by NOTHING G490 can admit — MFP decodes to `G422_LK_NONE`, so a pair that
    //     reads P is never block-eligible in the first place. P's timing is never observable.
    //   * `st.q` is read only by an upper with descriptor `src == 1` (MULq/ADDq/MADDq/SUBq/MSUBq/
    //     MULAq/...). The translator records whether ANY pair of the block is one, once, at
    //     translation time (`readsQ`).
    //
    // When the publication is unobservable, publishing it AT ENTRY leaves exactly the state the
    // segmented generic loop leaves: `st.q = s_vuQPending` and the counter at 0 (once a counter
    // reaches 0 the generic loop's `if (qd > 1) qd -= run` never touches it again). So the only
    // remaining refusal is a mid-block Q publication in a block that actually reads Q.
    // ⭐⭐ G611 REFINES THE OBSERVABILITY TEST FROM "DOES THIS BLOCK READ Q" TO "DOES IT READ Q
    // BEFORE THE PUBLICATION". A counter holding v publishes BEFORE pair v-1, so pairs 0..v-2 see
    // the OLD q and pairs v-1..count-1 see the new one. Publishing at ENTRY is therefore exact
    // whenever no pair in [0, v-2] reads Q — which `qMask` (bit j = pair j reads Q, indices >= 63
    // folded into bit 63) answers with one AND. G610 refused on `readsQ` alone, i.e. on the
    // existence of a reader anywhere in the block, including readers AFTER the publication that see
    // exactly the right value either way.
    //
    // Returns 0 = single segment, state updated; 1 = genuinely observable, state UNTOUCHED and the
    // caller may split at `*splitN1 = v - 1`; the caller falls back if it cannot.
    static DC2_G330_FORCEINLINE int g610QpSingleSegment(VU1State &st, uint32_t count,
                                                        int *qDelay, int *pDelay, bool readsQ,
                                                        uint64_t qMask, uint32_t *splitN1)
    {
        int qd = *qDelay;
        int pd = *pDelay;
        // A counter holding v publishes BEFORE pair v-1; `v-1 < count` means that pair is inside.
        const bool qIn = (qd > 1 && (uint32_t)(qd - 1) < count);
        const bool pIn = (pd > 1 && (uint32_t)(pd - 1) < count);
        if (qIn && readsQ)
        {
            const uint32_t n1 = (uint32_t)(qd - 1);
            const uint64_t prefix = (n1 >= 64u) ? ~0ull : ((1ull << n1) - 1ull);
            if ((qMask & prefix) != 0ull)
            {
                *splitN1 = n1;
                return 1;                     // genuinely observable
            }
        }
        if (qd == 1) { qd = 0; st.q = s_vuQPending; }
        if (pd == 1) { pd = 0; st.p = s_vuPPending; }
        if (qIn)      { qd = 0; st.q = s_vuQPending; }
        else if (qd > 1) qd -= (int)count;
        if (pIn)      { pd = 0; st.p = s_vuPPending; }
        else if (pd > 1) pd -= (int)count;
        *qDelay = qd;
        *pDelay = pd;
        return 0;
    }

    // G611 — the Q-OBSERVABLE SPLIT. When a publication really is observed inside the block, the
    // block is executed as TWO compiled sub-bodies with the publication between them, which is
    // exactly what the segmented generic loop does — only compiled.
    //
    // Both sub-bodies are acquired BEFORE any state is mutated, and the acquisition passes
    // `allowReset = false` so a translation triggered here can never unbind the very slot the
    // caller is standing on.
    static bool g611AcquireSplit(uint32_t firstIndex, uint32_t count, uint32_t n1,
                                 uint32_t dataSize, G610SlotRec &s)
    {
        if (n1 == 0u || n1 >= count)
            return false;
        if (s.splitN1 == (uint16_t)n1 && s.splitFn1 != nullptr && s.splitFn2 != nullptr)
            return true;
        if (s.splitN1 == (uint16_t)n1)
            return false;                      // memoised NEGATIVE for this split point
        const G610CacheEnt *e1 = g610CacheLookup(firstIndex, n1, dataSize, false);
        const G610CacheEnt *e2 = g610CacheLookup(firstIndex + n1, count - n1, dataSize, false);
        s.splitN1 = (uint16_t)n1;
        s.splitFn1 = (e1 != nullptr) ? e1->fn : nullptr;
        s.splitFn2 = (e2 != nullptr) ? e2->fn : nullptr;
        return s.splitFn1 != nullptr && s.splitFn2 != nullptr;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    bool g610RunNativeBlock(VU1State &st, uint32_t firstIndex, uint32_t count,
                            uint8_t *vuData, uint32_t dataSize,
                            int *qDelay, int *pDelay, int64_t stampBase, G430HistBlock *hb,
                            bool g611QSplit)
    {
        G610SlotRec &s = s_g610.slot[firstIndex];
        // The body was emitted for one exact pair count. The hook usually asks for exactly that
        // (`g490Count == g490Avail`), but a cycle-budget clamp can shorten a block, and a body that
        // executes 8 pairs must never be asked for 5.
        if (s.count != (uint16_t)count)
        {
            ++s_g610FbCount;
            return false;
        }
        // The baked `& (dataSize - 1)` address masks belong to one VU RAM size.
        if (dataSize != s_g610.dataSize)
        {
            ++s_g610FbNone;
            return false;
        }

        const bool verify = g610VerifyOn();
        if (verify)
        {
            s_g610SnapPre.st = st;
            s_g610SnapPre.hb = *hb;
            s_g610SnapPre.qd = *qDelay;
            s_g610SnapPre.pd = *pDelay;
            s_g610SnapPre.ram.assign(vuData, vuData + dataSize);
        }

        uint32_t g611N1 = 0u;
        bool g611Split = false;
        if (g610QpSingleSegment(st, count, qDelay, pDelay, s.readsQ != 0u, s.qMask, &g611N1) != 0)
        {
            // The publication is genuinely observed inside this block. G611 splits it in two;
            // G610's behaviour (and the `vuqsplit` control arm) is to hand it to the generic body.
            if (!g611QSplit || !g611AcquireSplit(firstIndex, count, g611N1, dataSize, s))
            {
                ++s_g610FbSeg;
                s_g610FbSegPairs += count;
                if (g611QSplit)
                    ++s_g611SplitFail;
                return false;
            }
            g611Split = true;
            // Everything the single-segment path would have done EXCEPT the Q publication, which
            // now happens between the two sub-bodies. P is never read by anything G490 admits, so
            // its publication stays at entry (G610 §3.6).
            int pd = *pDelay;
            const bool pIn = (pd > 1 && (uint32_t)(pd - 1) < count);
            if (pd == 1) { pd = 0; st.p = s_vuPPending; }
            if (pIn)      { pd = 0; st.p = s_vuPPending; }
            else if (pd > 1) pd -= (int)count;
            *pDelay = pd;
        }

        if (g611Split)
        {
            s.splitFn1(&st, vuData, stampBase, hb);
            st.q = s_vuQPending;
            *qDelay = 0;
            s.splitFn2(&st, vuData, stampBase + (int64_t)g611N1, hb);
            ++s_g611SplitBlocks;
            s_g611SplitPairs += count;
        }
        else
        {
            s.fn(&st, vuData, stampBase, hb);
        }
        if (verify)
        {
            s_g610SnapJit.st = st;
            s_g610SnapJit.hb = *hb;
            s_g610SnapJit.qd = *qDelay;
            s_g610SnapJit.pd = *pDelay;
            s_g610SnapJit.ram.assign(vuData, vuData + dataSize);

            // Restore the true pre-state and run the AUTHORITATIVE shipped generic executor.
            st = s_g610SnapPre.st;
            *hb = s_g610SnapPre.hb;
            *qDelay = s_g610SnapPre.qd;
            *pDelay = s_g610SnapPre.pd;
            std::memcpy(vuData, s_g610SnapPre.ram.data(), dataSize);
            g531ExecuteBlockTailFlag(st, firstIndex, count, vuData, dataSize, qDelay, pDelay,
                                     stampBase, hb, 0u);

            const char *what = "?";
            long at = -1;
            if (!g610StatesEqual(s_g610SnapJit.st, st, s_g610SnapJit.hb, *hb,
                                 s_g610SnapJit.ram.data(), vuData, dataSize, &what, &at))
            {
                // G611: print the two values side by side before aborting. `hist.vec` is the one
                // field where "wrong" has several possible mechanisms (a wrong result, a wrong ring
                // slot, or G546's canonicalisation of the masked-off lanes), and the RAW BITS
                // separate them at a glance.
                if (at >= 0 && at < G430_HIST &&
                    (std::strcmp(what, "hist.vec") == 0))
                    std::fprintf(stderr,
                                 "[G610:verify-BAD] hist.vec[%ld] jit=%08x %08x %08x %08x "
                                 "gen=%08x %08x %08x %08x dest=%u kind=%u head jit=%d gen=%d\n",
                                 at,
                                 *(const uint32_t *)&s_g610SnapJit.hb.vec[at][0],
                                 *(const uint32_t *)&s_g610SnapJit.hb.vec[at][1],
                                 *(const uint32_t *)&s_g610SnapJit.hb.vec[at][2],
                                 *(const uint32_t *)&s_g610SnapJit.hb.vec[at][3],
                                 *(const uint32_t *)&hb->vec[at][0],
                                 *(const uint32_t *)&hb->vec[at][1],
                                 *(const uint32_t *)&hb->vec[at][2],
                                 *(const uint32_t *)&hb->vec[at][3],
                                 (unsigned)hb->dest[at], (unsigned)hb->kind[at],
                                 (int)s_g610SnapJit.hb.head, (int)hb->head);
                g610VerifyFail(what, firstIndex, count, at);
            }
            if (s_g610SnapJit.qd != *qDelay || s_g610SnapJit.pd != *pDelay)
                g610VerifyFail("qpDelay", firstIndex, count, -1);
            g610VerifyPass();
        }

        ++s_g610Blocks;
        s_g610Pairs += count;
        if (g610StatOn())
        {
            // G611's coverage census rides the ONE `g610StatOn()` read this function already made,
            // so a stat-off run pays exactly what G610 shipped.
            if (s.g611 != 0u)
            {
                ++s_g611OpsBlocks;
                s_g611OpsPairs += count;
            }
            if ((s_g610Blocks & 0x3FFFFFull) == 0ull)
                g610Report();
        }
        return true;
    }

#if DC2_G612_REGION_BACKEND
    // ===========================================================================================
    // G612 — THE REGION ENTRY, ITS ORACLE, AND THE TWO CONSTANTS THE TRANSLATOR MIRRORS
    // ===========================================================================================
    //
    // `vu1_g612_native_region.inc` is included BEFORE this file (it needs G610's emitter, which is
    // included before it), so it cannot see `G490_MIN_RUN` or `G531_FLAG_TAIL`. It re-declares both
    // and they are tied together HERE, beside the definitions, so a change to either is a build
    // error rather than a silently divergent admission rule.
    static_assert(G612_MIN_ENTRY == G490_MIN_RUN, "G612: entry threshold must track G490_MIN_RUN");
    static_assert(G612_FLAG_TAIL == G531_FLAG_TAIL, "G612: flag-tail code must track G531");

    // -------------------------------------------------------------------------------------------
    // THE ORACLE'S REFERENCE STEPPER (DC2_G612_VERIFY=1).
    //
    // ⭐ WHAT MAKES THIS AN ORACLE AND NOT A SECOND IMPLEMENTATION. It executes pairs ONLY through
    // `g531ExecuteBlockTailFlag` — the shipped generic block body, the same one G610's oracle
    // treats as authoritative — and it derives its own control flow from the shipped sidecars
    // (`g490RunLen`, `g502TailAt`) plus the pair loop's own branch protocol, copied statement for
    // statement from the hook in vu1_g435_pair_loop.inc. It never reads the region's CFG. So a
    // region that mis-computes a branch condition, mis-orders a delay slot, links a back-edge to
    // the wrong block, or emits one wrong byte anywhere lands in a different architectural state
    // than this does, and the whole-state comparison says so.
    //
    // It is driven to EXACTLY the pair count the region consumed. That is deliberate: where the
    // region STOPS is a coverage question (measured by `[G612:exit]`), not a correctness one, and
    // an oracle that also had to predict the stopping point would have to import the CFG and stop
    // being independent. What it does check, and what a wrong stop point would break, is that the
    // resulting `st.pc` agrees — a region that ran the right number of pairs down the wrong path
    // cannot land on the right pc.
    // -------------------------------------------------------------------------------------------
    // G652: authoritative one-pair oracle bridge for the two admissions that deliberately sit
    // outside G490's block sidecar. It calls the shipped G421/G422 leaves and reproduces the
    // generic loop's same-pair overlay literally; it does not consult the region CFG or emitter.
    bool g652RefSpecialPair(VU1State &st, uint32_t idx, uint8_t *vuData, uint32_t dataSize,
                            int64_t stamp, G430HistBlock *hb)
    {
        if (idx >= s_g410Cache.fused.size())
            return false;
        const G479FusedPair &fp = s_g410Cache.fused[idx];
        const uint32_t lower = static_cast<uint32_t>(fp.source);
        const uint32_t upper = static_cast<uint32_t>(fp.source >> 32u);
        const bool loi = g652VuLoiOn() && fp.pair.pairFlags == G410_PAIR_I_BIT;
        const bool hazardStore = g652RegionHazStorePairOk(idx);
        if (!loi && !hazardStore)
            return false;

        __m128 oldV = _mm_setzero_ps();
        __m128 newV = _mm_setzero_ps();
        const int hazardDest = hazardStore ? static_cast<int>(fp.pair.upperVfDest) : -1;
        if (hazardDest >= 0)
            oldV = _mm_loadu_ps(st.vf[hazardDest]);
        if (loi)
            std::memcpy(&st.i, &lower, sizeof(lower));
        if (fp.pair.upperKind != 0x6fu)
            g421FastUpper<true, false>(st, upper, fp.pair.upperKind, true, false, true,
                                       stamp, hb);
        if (hazardDest >= 0)
        {
            newV = _mm_loadu_ps(st.vf[hazardDest]);
            _mm_storeu_ps(st.vf[hazardDest], oldV);
            g422FastLower(st, lower, fp.pair.lowerKind, vuData, dataSize);
            const uint8_t mask = static_cast<uint8_t>((upper >> 21u) & 0xFu);
            alignas(16) uint32_t newBits[4];
            std::memcpy(newBits, &newV, sizeof(newBits));
            for (unsigned lane = 0u; lane < 4u; ++lane)
                if ((mask & (8u >> lane)) != 0u)
                    std::memcpy(&st.vf[hazardDest][lane], &newBits[lane], sizeof(uint32_t));
        }
        if ((fp.pair.g425Flags & G425_PF_TOUCHES_ZERO) != 0u)
        {
            const __m128 vf0 = _mm_castsi128_ps(_mm_set_epi32(0x3f800000, 0, 0, 0));
            _mm_storeu_ps(st.vf[0], vf0);
            st.vi[0] = 0;
        }
        return true;
    }

    uint32_t g612RefRun(VU1State &st, uint32_t startSlot, uint8_t *vuData, uint32_t dataSize,
                        int *qDelay, int *pDelay, int64_t stampBase, G430HistBlock *hb,
                        uint32_t wantPairs, uint32_t codeSize)
    {
        uint32_t pc = startSlot * 8u;
        uint32_t consumed = 0u;
        while (consumed < wantPairs)
        {
            const uint32_t idx = pc >> 3u;
            if (idx >= s_g410Cache.g490RunLen.size())
                break;
            const uint32_t left = wantPairs - consumed;
            const uint32_t avail = (uint32_t)s_g410Cache.g490RunLen[idx];
            uint32_t count = avail < left ? avail : left;
            uint32_t tail = 0u;
            uint32_t bpc = 0u;
            // ⚠️ NO `count >= G490_MIN_RUN` CONJUNCT HERE, and that is not an oversight. The pair
            // loop's hook needs it because a one-pair block does not pay for its round trip; the
            // REGION has no round trip to pay for, so it emits a branch after a single plain pair
            // and after none at all. The first oracle run said so in one line — `pairs jit=22
            // ref=1 pc=0x1c8`, a slot whose `g490RunLen` is 1 — and a reference that cannot
            // reproduce the region's own admission rule reports a defect that is entirely its own.
            // `g531ExecuteBlockTailFlag` instantiates `G506QpSeg = true`, whose segment loop opens
            // with `if (j >= count) break;`, so `count == 0` with a tail is well-defined.
            if (count == avail)
            {
                uint32_t t = (uint32_t)s_g410Cache.g502TailAt[idx];
                // ⭐ G651: a region now DOES cover one flag-consumer tail — FCGET, whose emission is
                // argued beside `g651VuClipOn` in vu1_g612_native_region.inc. Under that arm the
                // reference must be allowed to execute it through `g531ExecuteBlockTailFlag` (the
                // shipped generic block body, which publishes the MAC line at the exact stamp and
                // then runs the already-verified G487 fast lower), or the oracle would report a
                // `pairs jit=N ref=M` defect that is entirely its own. Every OTHER flag consumer is
                // still forced to 0, because a region still exits at those.
                // ⛔ THE TAIL SLOT IS `idx + avail`, NOT `idx`. `g502TailAt[i]` is pre-joined as
                // `g500Tail[i + g490RunLen[i]]`, so the pair the tail describes sits at the END of
                // the run. Testing `uppers[idx]` reads the run's FIRST pair and lets every flag tail
                // through — the first oracle run said so in one line, `pairs jit=87 ref=77 pc=0x2740`.
                const size_t g651TailSlot = (size_t)idx + (size_t)avail;
                if (t == G531_FLAG_TAIL &&
                    !(g651VuClipOn() && g651TailSlot < s_g410Cache.uppers.size() &&
                      s_g410Cache.uppers[g651TailSlot].lowerKind == G422_LK_FCGET))
                    t = 0u;                       // a region never covers any OTHER flag tail
                if (t != 0u)
                {
                    bpc = pc + 8u * avail;
                    // ⚠️ `G531_FLAG_TAIL` is a TYPE CODE (3), not a pair count — the flag tail is
                    // ONE pair. Every arm in vu1_g435_pair_loop.inc spells this
                    // `(tail == G531_FLAG_TAIL) ? 1u : tail`; do the same for the budget test and
                    // for `consumed` below, or the reference over-counts by two per FCGET.
                    const uint32_t tPairs = (t == G531_FLAG_TAIL) ? 1u : t;
                    if (bpc + 8u > codeSize)
                        t = 0u;
                    else
                    {
                        if (t == 2u && bpc + 16u > codeSize)
                            t = 1u;
                        if (avail + tPairs > left)
                            t = 0u;
                    }
                    tail = t;
                }
            }
            const uint32_t tailPairs = (tail == G531_FLAG_TAIL) ? 1u : tail;
            if (count == 0u && tail == 0u)
            {
                const bool special =
                    (g652VuLoiOn() &&
                     s_g410Cache.uppers[idx].pairFlags == G410_PAIR_I_BIT) ||
                    g652RegionHazStorePairOk(idx);
                if (!special)
                    break;
                // The generic pair loop advances scalar pipelines before either issue slot.
                if (*qDelay > 0 && --(*qDelay) == 0)
                    st.q = s_vuQPending;
                if (*pDelay > 0 && --(*pDelay) == 0)
                    st.p = s_vuPPending;
                if (!g652RefSpecialPair(st, idx, vuData, dataSize,
                                        stampBase + static_cast<int64_t>(consumed), hb))
                    break;
                ++consumed;
                pc = (pc + 8u >= codeSize) ? 0u : pc + 8u;
                continue;
            }
            g531ExecuteBlockTailFlag(st, idx, count, vuData, dataSize, qDelay, pDelay,
                                     stampBase + (int64_t)consumed, hb, tail);
            consumed += count + tailPairs;
            uint32_t next;
            if (tail == 0u)
                next = pc + 8u * count;
            else if (tail == G531_FLAG_TAIL)
            {
                // ⭐ G651: a flag consumer writes no pc. Control falls through to the next slot,
                // exactly as the pair loop's own `g500Tail == G531_FLAG_TAIL` arm does.
                next = bpc + 8u;
            }
            else
            {
                const bool taken = (st.pc != bpc);
                if (tail == 2u)
                    next = taken ? (st.pc + 8u) : (bpc + 16u);
                else
                {
                    // A `tail == 1` leaves a PENDING branch target, which a region can never do
                    // (precondition 2). Stop rather than model it.
                    next = bpc + 8u;
                    pc = (next >= codeSize) ? 0u : next;
                    break;
                }
            }
            pc = (next >= codeSize) ? 0u : next;
        }
        st.pc = pc;
        return consumed;
    }

    thread_local G610Snap s_g612SnapPre, s_g612SnapJit;

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    uint32_t g612RunRegion(VU1State &st, uint32_t firstIndex, uint8_t *vuData, uint32_t dataSize,
                           uint32_t codeSize, int *qDelay, int *pDelay, int64_t stampBase,
                           G430HistBlock *hb, uint32_t budget)
    {
        const G612SlotRec &s = s_g612.slot[firstIndex];
        // Precondition 4. The baked `& (dataSize - 1)` address masks and every emitted PC constant
        // belong to one pair of sizes.
        if (dataSize != s_g612.dataSize || codeSize != s_g612.codeSize)
        {
            ++s_g612SkipSize;
            return 0u;
        }
        // Precondition 6, caller half: the ENTRY block must fit, so a region always makes progress
        // and the pair loop can never spin on a zero-pair return.
        if (budget < (uint32_t)s.pairs)
        {
            ++s_g612SkipBudget;
            return 0u;
        }

        const bool verify = g612VerifyOn();
        const int qEntry = *qDelay;
        const int pEntry = *pDelay;
        if (verify)
        {
            s_g612SnapPre.st = st;
            s_g612SnapPre.hb = *hb;
            s_g612SnapPre.qd = qEntry;
            s_g612SnapPre.pd = pEntry;
            s_g612SnapPre.ram.assign(vuData, vuData + dataSize);
        }

        const uint32_t n = s.fn(&st, vuData, stampBase, hb, budget);
        if (n == 0u)
            return 0u;                            // structurally impossible; fail closed anyway

        // ⭐ THE DEFERRED SCALAR-PIPE PUBLICATION (precondition 3, relaxed exactly as far as it is
        // provable). The generic per-pair form is `if (d > 0 && --d == 0) publish`, so over `n`
        // pairs a counter holding `v` publishes iff `v <= n` and otherwise ends at `v - n`. That is
        // what these two lines compute. Doing it at the EXIT rather than at pair `v` is
        // unobservable because:
        //   * NOTHING G490 admits reads `st.p` (MFP decodes to G422_LK_NONE), so P always defers;
        //   * Q defers only when the region contains no `src == 1` upper anywhere — the caller's
        //     `readsQ` test, which is G611 §3's prefix argument taken over the whole body.
        // Nothing a region emits can ARM either counter, so neither can be re-armed in between.
        int qd = qEntry, pd = pEntry;
        if (pd > 0)
        {
            if ((uint32_t)pd <= n) { pd = 0; st.p = s_vuPPending; }
            else                    pd -= (int)n;
            *pDelay = pd;
        }
        if (qd > 0)
        {
            if ((uint32_t)qd <= n) { qd = 0; st.q = s_vuQPending; }
            else                    qd -= (int)n;
            *qDelay = qd;
        }

        if (verify)
        {
            s_g612SnapJit.st = st;
            s_g612SnapJit.hb = *hb;
            s_g612SnapJit.qd = qd;
            s_g612SnapJit.pd = pd;
            s_g612SnapJit.ram.assign(vuData, vuData + dataSize);

            st = s_g612SnapPre.st;
            *hb = s_g612SnapPre.hb;
            *qDelay = s_g612SnapPre.qd;
            *pDelay = s_g612SnapPre.pd;
            std::memcpy(vuData, s_g612SnapPre.ram.data(), dataSize);
            const uint32_t nRef = g612RefRun(st, firstIndex, vuData, dataSize, qDelay, pDelay,
                                             stampBase, hb, n, codeSize);

            const char *what = "?";
            long at = -1;
            if (nRef != n)
            {
                std::fprintf(stderr, "[G612:verify-BAD] pairs jit=%u ref=%u pc=0x%x\n", n, nRef,
                             firstIndex * 8u);
                g610VerifyFail("pairs", firstIndex, n, -1);
            }
            if (!g610StatesEqual(s_g612SnapJit.st, st, s_g612SnapJit.hb, *hb,
                                 s_g612SnapJit.ram.data(), vuData, dataSize, &what, &at))
            {
                std::fprintf(stderr,
                             "[G612:verify-BAD] %s at=%ld pc=0x%x pairs=%u pcJit=0x%x pcRef=0x%x "
                             "afterOk=%llu\n",
                             what, at, firstIndex * 8u, n, s_g612SnapJit.st.pc, st.pc,
                             (unsigned long long)s_g612Verified);
                g610VerifyFail(what, firstIndex, n, at);
            }
            // Precondition 3, CHECKED rather than asserted. The reference advanced both counters
            // pair by pair inside the shipped block body and published at the exact pair; the
            // region deferred both to the exit. If the deferral were ever observable the two would
            // disagree here — and this is also what proves no emitted pair re-armed either.
            if (s_g612SnapJit.qd != *qDelay || s_g612SnapJit.pd != *pDelay)
            {
                std::fprintf(stderr,
                             "[G612:verify-BAD] qp jit=(%d,%d) ref=(%d,%d) entry=(%d,%d) "
                             "pc=0x%x pairs=%u\n",
                             s_g612SnapJit.qd, s_g612SnapJit.pd, *qDelay, *pDelay, qEntry, pEntry,
                             firstIndex * 8u, n);
                g610VerifyFail("qp", firstIndex, n, -1);
            }
            const uint64_t v = ++s_g612Verified;
            if (v == 1u || (v % 2000000u) == 0u)
            {
                std::fprintf(stderr, "[G612:verify] regions=%llu bad=0 pairs=%llu\n",
                             (unsigned long long)v, (unsigned long long)s_g612Pairs);
                std::fflush(stderr);
            }
        }

        ++s_g612Runs;
        s_g612Pairs += n;
        if (g612StatOn())
        {
            if ((uint64_t)n > s_g612MaxPairs)
                s_g612MaxPairs = (uint64_t)n;
            const uint32_t ex = st.pc >> 3u;
            if (ex < s_g612ExitAt.size())
            {
                ++s_g612ExitAt[ex];
                const uint8_t flags = s_g410Cache.uppers[ex].pairFlags;
                if ((flags & G410_PAIR_I_BIT) != 0u)
                    ++s_g652ExitIAt[ex];
                if ((flags & G410_PAIR_E_BIT) != 0u)
                    ++s_g652ExitEAt[ex];
                int g652FmandState = -1;
                if (s_g410Cache.uppers[ex].lowerKind == G422_LK_FMAND)
                {
                    const int64_t target = stampBase + (int64_t)n - 4;
                    if (g652MacHistoryPendingAt(*hb, target))
                    {
                        ++s_g652FmandPendingAt[ex];
                        g652FmandState = 1;
                    }
                    else
                    {
                        ++s_g652FmandMaterialAt[ex];
                        g652FmandState = 0;
                    }
                }
                g652ExitIdentityNote(ex, g652FmandState);
            }
            if ((s_g612Runs & 0x3FFFFFull) == 0ull)
                g612Report();
        }
        return n;
    }
#endif // DC2_G612_REGION_BACKEND

    // Eager translation, run at the END of the cold rebuild pass — the same pass, on the same two
    // invalidation paths, that rewrites g490RunLen / g500Tail / g516*. A compiled body therefore
    // can never describe a stale encoding: it is discarded and re-emitted by the same event that
    // rewrote the pair it came from. (Lazy compilation would need a per-block-entry `tried` test on
    // the hot path; this needs none, so the hook is one array load.)
    void g610CompileAll(uint32_t dataSize)
    {
        // PROCESS-level, never the per-run() arm: both arms of `DC2_G419_AB=vujit` must find the
        // same populated slot table.
        if (!g610BackendEnabled())
            return;
        g610CheckMaskTable();
        g610UnbindAll(dataSize);
        ++s_g610Rebuilds;
        for (size_t i = 0u; i < s_g410Cache.g490RunLen.size(); ++i)
        {
            const uint32_t run = (uint32_t)s_g410Cache.g490RunLen[i];
            if (run < G490_MIN_RUN)
                continue;
            // ⚠️ Slots whose run ends in a branch/flag TAIL are bound too, even though the hook
            // currently refuses them (precondition 3). Binding them is what makes `[G610:why]
            // fbTail` a MEASUREMENT of the tail population instead of a structural zero — and a
            // loop body always ends in its own back-edge, so this is where the coverage is.

            // Code cache: hit on the exact source words and this body is already emitted.
            // NEGATIVE entries (fn == nullptr) are cached too. Without them a refused block is
            // re-translated on every rebuild — 362,117 wasted attempts over 2,103 rebuilds in the
            // run that measured this — and each one walks its pairs until it hits the unsupported
            // one. G611 moved the probe/insert into `g610CacheLookup` so the Q-split path can use
            // the same cache for its SUB-BLOCK bodies.
            const G610CacheEnt *ce = g610CacheLookup((uint32_t)i, run, dataSize, true);
            if (ce != nullptr && ce->fn != nullptr)
            {
                s_g610.slot[i].fn = ce->fn;
                s_g610.slot[i].count = ce->count;
                s_g610.slot[i].readsQ = ce->readsQ;
                s_g610.slot[i].g611 = ce->g611;
                s_g610.slot[i].qMask = ce->qMask;
            }
        }
        // One flush for the whole newly-emitted region rather than one per body: x86-64 keeps
        // instruction and data caches coherent for same-thread write-then-execute (the `call` into
        // the body is the serialising branch), so this is belt-and-braces, not a requirement.
#if defined(_WIN32)
        if (s_g610.arena != nullptr && s_g610.used != 0u)
            FlushInstructionCache(GetCurrentProcess(), s_g610.arena, s_g610.used);
#endif
        if (g610StatOn() && (s_g610Rebuilds % 4096u) == 1u)
            g610Report();
    }
#endif // DC2_G610_JIT_BACKEND

    // G610 CEILING PROBE ARMS (`DC2_G610_CEIL=1|2|3`).
    //
    // WHY THESE EXIST. ROADMAP Target A asks for a VU1 microprogram translator, and G607 priced its
    // headroom as "the interpreter's own overhead, 13.2-14.4 ns/pair ~= 53 cycles per VU pair" — a
    // WHOLE-PROGRAM average taken before G490's block executor, G515/G516's dispatch levers and the
    // G533/G547-G551 static handlers were all in the shipped path. Before emitting a single byte of
    // native code, RANK THE PAIR'S STAGES BY DELETING THEM (the G609 law): run the promoted block
    // leaf with the lower slot gone, with the upper slot gone, and with both gone, against the same
    // route and the same denominators. What survives at `Ceil=3` is the part a translator CANNOT
    // remove (the loop, the fused fetch, the Q/P segmentation, the G425 re-pin); the difference is
    // the part it can.
    //
    // Each arm is a SEPARATE INSTANTIATION of the shipped arm-23 body (G436/G593: a default-off
    // diagnostic must never branch inside the loop it observes), reached through a loop-invariant
    // arm read at the caller, exactly like every other arm in this file.
    //
    // ⛔ RENDERS GARBAGE. Never a correctness run, never quoted as a lever.
#if DC2_G610_CEIL_ARMS
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g610ExecuteBlockCeilNoLower(VU1State &st, uint32_t firstIndex, uint32_t count,
                                     uint8_t *vuData, uint32_t dataSize,
                                     int *qDelay, int *pDelay,
                                     int64_t stampBase, G430HistBlock *hb, uint32_t tailCode)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, false, false, false, true, false, false, false,
                      1>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCode);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g610ExecuteBlockCeilNoUpper(VU1State &st, uint32_t firstIndex, uint32_t count,
                                     uint8_t *vuData, uint32_t dataSize,
                                     int *qDelay, int *pDelay,
                                     int64_t stampBase, G430HistBlock *hb, uint32_t tailCode)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, false, false, false, true, false, false, false,
                      2>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCode);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g610ExecuteBlockCeilEmpty(VU1State &st, uint32_t firstIndex, uint32_t count,
                                   uint8_t *vuData, uint32_t dataSize,
                                   int *qDelay, int *pDelay,
                                   int64_t stampBase, G430HistBlock *hb, uint32_t tailCode)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, false, false, false, true, false, false, false,
                      3>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCode);
    }
#endif // DC2_G610_CEIL_ARMS

    // G533 arms 27/28: immutable-image specialization. Admission is one rebuild-time exact-source
    // plan bit plus the block identity already live at this call site. No per-pair plan stream, hash,
    // function pointer, or runtime code generation enters the hot loop; all non-matching code uses
    // G531 unchanged.
    static DC2_G330_FORCEINLINE bool g533Hot3088Match(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode)
    {
        // Test the caller-live identity first. Ordinary blocks then avoid even loading the plan bit.
        return firstIndex == G533_HOT_3088_INDEX && count == G533_HOT_3088_COUNT &&
               tailCode == G533_HOT_3088_TAIL && s_g533Hot3088Plan != 0u;
    }

    static DC2_G330_FORCEINLINE bool g533Hot3088Runnable(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode,
        const uint8_t *vuData, uint32_t dataSize, const G430HistBlock *hb)
    {
        return g533Hot3088Match(firstIndex, count, tailCode) && vuData != nullptr &&
               dataSize == PS2_VU1_DATA_SIZE && hb != nullptr;
    }

    static DC2_G330_FORCEINLINE bool g547Hot0c70Match(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode)
    {
        return firstIndex == G547_HOT_0C70_INDEX && count == G547_HOT_0C70_COUNT &&
               tailCode == G547_HOT_0C70_TAIL && s_g547Hot0c70Plan != 0u;
    }

    static DC2_G330_FORCEINLINE bool g547Hot0c70Runnable(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode,
        const uint8_t *vuData, uint32_t dataSize, const G430HistBlock *hb)
    {
        return g547Hot0c70Match(firstIndex, count, tailCode) && vuData != nullptr &&
               dataSize == PS2_VU1_DATA_SIZE && hb != nullptr;
    }

    static DC2_G330_FORCEINLINE bool g548Hot0e00Match(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode)
    {
        return firstIndex == G548_HOT_0E00_INDEX && count == G548_HOT_0E00_COUNT &&
               tailCode == G548_HOT_0E00_TAIL && s_g548Hot0e00Plan != 0u;
    }

    static DC2_G330_FORCEINLINE bool g548Hot0e00Runnable(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode,
        const uint8_t *vuData, uint32_t dataSize, const G430HistBlock *hb)
    {
        return g548Hot0e00Match(firstIndex, count, tailCode) && vuData != nullptr &&
               dataSize == PS2_VU1_DATA_SIZE && hb != nullptr;
    }

    static DC2_G330_FORCEINLINE bool g549Hot30d8Match(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode)
    {
        return firstIndex == G549_HOT_30D8_INDEX && count == G549_HOT_30D8_COUNT &&
               tailCode == G549_HOT_30D8_TAIL && s_g549Hot30d8Plan != 0u;
    }

    static DC2_G330_FORCEINLINE bool g549Hot30d8Runnable(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode,
        const uint8_t *vuData, uint32_t dataSize, const G430HistBlock *hb)
    {
        return g549Hot30d8Match(firstIndex, count, tailCode) && vuData != nullptr &&
               dataSize == PS2_VU1_DATA_SIZE && hb != nullptr;
    }

    static DC2_G330_FORCEINLINE bool g550Hot1be8Match(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode)
    {
        return firstIndex == G550_HOT_1BE8_INDEX && count == G550_HOT_1BE8_COUNT &&
               tailCode == G550_HOT_1BE8_TAIL && s_g550Hot1be8Plan != 0u;
    }

    static DC2_G330_FORCEINLINE bool g550Hot1be8Runnable(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode,
        const uint8_t *vuData, uint32_t dataSize, const G430HistBlock *hb)
    {
        return g550Hot1be8Match(firstIndex, count, tailCode) && vuData != nullptr &&
               dataSize == PS2_VU1_DATA_SIZE && hb != nullptr;
    }

    static DC2_G330_FORCEINLINE bool g551Hot1c00Match(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode)
    {
        return firstIndex == G551_HOT_1C00_INDEX && count == G551_HOT_1C00_COUNT &&
               tailCode == G551_HOT_1C00_TAIL && s_g551Hot1c00Plan != 0u;
    }

    static DC2_G330_FORCEINLINE bool g551Hot1c00Runnable(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode,
        const uint8_t *vuData, uint32_t dataSize, const G430HistBlock *hb)
    {
        return g551Hot1c00Match(firstIndex, count, tailCode) && vuData != nullptr &&
               dataSize == PS2_VU1_DATA_SIZE && hb != nullptr;
    }

    static DC2_G330_FORCEINLINE bool g554Hot0900Match(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode)
    {
        return firstIndex == G554_HOT_0900_INDEX && count == G554_HOT_0900_COUNT &&
               tailCode == G554_HOT_0900_TAIL && s_g554Hot0900Plan != 0u;
    }

    static DC2_G330_FORCEINLINE bool g554Hot0900Runnable(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode,
        const uint8_t *vuData, uint32_t dataSize, const G430HistBlock *hb)
    {
        return g554Hot0900Match(firstIndex, count, tailCode) && vuData != nullptr &&
               dataSize == PS2_VU1_DATA_SIZE && hb != nullptr;
    }

    static DC2_G330_FORCEINLINE bool g555Hot1110Match(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode)
    {
        return firstIndex == G555_HOT_1110_INDEX && count == G555_HOT_1110_COUNT &&
               tailCode == G555_HOT_1110_TAIL && s_g555Hot1110Plan != 0u;
    }

    static DC2_G330_FORCEINLINE bool g555Hot1110Runnable(
        uint32_t firstIndex, uint32_t count, uint32_t tailCode,
        const uint8_t *vuData, uint32_t dataSize, const G430HistBlock *hb)
    {
        return g555Hot1110Match(firstIndex, count, tailCode) && vuData != nullptr &&
               dataSize == PS2_VU1_DATA_SIZE && hb != nullptr;
    }

    template <unsigned Vf>
    static DC2_G330_FORCEINLINE void g533LqiFullVi8(VU1State &st, const uint8_t *vuData)
    {
        static_assert(Vf < 32u);
        const uint32_t addr =
            (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[8])) * 16u) &
            (PS2_VU1_DATA_SIZE - 1u);
        _mm_storeu_ps(st.vf[Vf], _mm_loadu_ps(reinterpret_cast<const float *>(vuData + addr)));
        st.vi[8] = static_cast<int16_t>(st.vi[8] + 1);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g533ExecuteHot3088(VU1State &st, uint8_t *vuData,
                            int *qDelay, int *pDelay, int64_t stampBase,
                            G430HistBlock *hb)
    {
        int qd = *qDelay;
        int pd = *pDelay;

        // 0x3088..0x3098: three fixed FMAC uppers paired with three full-vector LQI loads.
        // Literal words make descriptor class, operands, masks and lower dispatch compile-time
        // constants while preserving upper-before-lower issue order and one history stamp per pair.
        g421FastUpper<true, false, true>(st, 0x01e0ac5bu, 0x1bu, true, false, true,
                                         stampBase + 0, hb, g504UnusedSlot, 0x03000401u);
        g533LqiFullVi8<21u>(st, vuData);

        g421FastUpper<true, false, true>(st, 0x01e0b49bu, 0x1bu, true, false, true,
                                         stampBase + 1, hb, g504UnusedSlot, 0x03000401u);
        g533LqiFullVi8<22u>(st, vuData);

        g421FastUpper<true, false, true>(st, 0x01e0bcdbu, 0x1bu, true, false, true,
                                         stampBase + 2, hb, g504UnusedSlot, 0x03000401u);
        g533LqiFullVi8<23u>(st, vuData);

        // 0x30a0..0x30c8: fixed NOP lowers, with only the two non-VNOP uppers retained.
        g421FastUpper<true, false, true>(st, 0x01d189ffu, 0x5fu, true, false, true,
                                         stampBase + 4, hb, g504UnusedSlot, 0x00000008u);
        g421FastUpper<true, false, true>(st, 0x01d5a9ffu, 0x5fu, true, false, true,
                                         stampBase + 5, hb, g504UnusedSlot, 0x00000008u);

        // 0x30d0 typed tail: publish the exact four-cycle MAC history point before fixed FCGET.
        s_vuMacVisible = g430ResolveAt(stampBase + 5);
        st.vi[1] = static_cast<int32_t>(st.clip & 0xFFFu);

        // All ten exact pairs are scalar-pipe-free and none reads Q/P. Collapse the same G506
        // countdown contract to one span update; publication timing inside this fragment is
        // unobservable, while the architectural value and remaining delay at exit stay exact.
        if (qd > 0)
        {
            if (qd <= 10)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 10;
        }
        if (pd > 0)
        {
            if (pd <= 10)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 10;
        }

        *qDelay = qd;
        *pDelay = pd;
    }

    template <unsigned Vf, unsigned Vi, int Imm>
    static DC2_G330_FORCEINLINE void g547LqFull(VU1State &st, const uint8_t *vuData)
    {
        static_assert(Vf < 32u && Vi < 16u);
        const uint32_t addr =
            (static_cast<uint32_t>(static_cast<int32_t>(st.vi[Vi] + Imm)) * 16u) &
            (PS2_VU1_DATA_SIZE - 1u);
        _mm_storeu_ps(st.vf[Vf],
                      _mm_loadu_ps(reinterpret_cast<const float *>(vuData + addr)));
    }

    template <unsigned Vf, unsigned Vi>
    static DC2_G330_FORCEINLINE void g547SqiFull(VU1State &st, uint8_t *vuData)
    {
        static_assert(Vf < 32u && Vi < 16u && Vi != 0u);
        const uint32_t addr =
            (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[Vi])) * 16u) &
            (PS2_VU1_DATA_SIZE - 1u);
        _mm_storeu_ps(reinterpret_cast<float *>(vuData + addr), _mm_loadu_ps(st.vf[Vf]));
        st.vi[Vi] = static_cast<int16_t>(st.vi[Vi] + 1);
    }

    template <unsigned Vf, unsigned Vi>
    static DC2_G330_FORCEINLINE void g551LqiFull(VU1State &st, const uint8_t *vuData)
    {
        static_assert(Vf < 32u && Vi < 16u && Vi != 0u);
        const uint32_t addr =
            (static_cast<uint32_t>(static_cast<uint16_t>(st.vi[Vi])) * 16u) &
            (PS2_VU1_DATA_SIZE - 1u);
        _mm_storeu_ps(st.vf[Vf], _mm_loadu_ps(reinterpret_cast<const float *>(vuData + addr)));
        st.vi[Vi] = static_cast<int16_t>(st.vi[Vi] + 1);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g547ExecuteHot0c70(VU1State &st, uint8_t *vuData,
                            int *qDelay, int *pDelay, int64_t stampBase,
                            G430HistBlock *hb)
    {
        int qd = *qDelay;
        int pd = *pDelay;

        // Q is observed by the first upper. Publish its pending value at the exact pair boundary;
        // P and the remaining five Q boundaries are unobserved inside this immutable fragment.
        if (qd > 0 && --qd == 0)
            st.q = s_vuQPending;
        if (pd > 0 && --pd == 0)
            st.p = s_vuPPending;

        // 0x0c70: MULq.xyzw vf20, vf20 followed by full LQ vf20, 1(vi7).
        g421FastUpper<true, false, true>(st, 0x01c0a55cu, 0x1cu, true, false, true,
                                         stampBase + 0, hb, g504UnusedSlot, 0x00010401u);
        g547LqFull<20u, 7u, 1>(st, vuData);

        // 0x0c78: MADDAy.xyzw, then MOVE.xyz vf19, vf17 (w remains untouched).
        g421FastUpper<true, false, true>(st, 0x01f058bdu, 0x49u, true, false, true,
                                         stampBase + 1, hb, g504UnusedSlot, 0x01000202u);
        g421StoreMasked(st.vf[19], _mm_loadu_ps(st.vf[17]), G481_MASKROW(0xEu));

        // 0x0c80: MADDAz.xyzw, then full SQI vf18, (vi8++).
        g421FastUpper<true, false, true>(st, 0x01f060beu, 0x4au, true, false, true,
                                         stampBase + 2, hb, g504UnusedSlot, 0x02000202u);
        g547SqiFull<18u, 8u>(st, vuData);

        // 0x0c88: MADDw.xyzw followed by full LQ vf16, 2(vi8).
        g421FastUpper<true, false, true>(st, 0x01f06c4bu, 0x0bu, true, false, true,
                                         stampBase + 3, hb, g504UnusedSlot, 0x03000201u);
        g547LqFull<16u, 8u, 2>(st, vuData);

        // 0x0c90 branch and 0x0c98 delay slot. The generic block tail publishes this pair PC
        // before IBNE; its macro leaves that value in place when the branch is not taken.
        st.pc = 0x0c90u;
        G481_LOWER_BODY_IBNE(st, 0x520907f9u);
        g547SqiFull<21u, 7u>(st, vuData);

        // Advance the five unobserved boundaries exactly. No instruction above produces Q/P.
        if (qd > 0)
        {
            if (qd <= 5)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 5;
        }
        if (pd > 0)
        {
            if (pd <= 5)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 5;
        }

        *qDelay = qd;
        *pDelay = pd;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g548ExecuteHot0e00(VU1State &st, uint8_t *vuData,
                            int *qDelay, int *pDelay, int64_t stampBase,
                            G430HistBlock *hb)
    {
        // Eight ordinary pairs. Fixed NOP lowers disappear; the one IADDI and one full LQ retain
        // upper-before-lower issue order and their original history stamps.
        g421FastUpper<true, false, true>(st, 0x01f638beu, 0x4au, true, false, true,
                                         stampBase + 0, hb, g504UnusedSlot, 0x02000202u);
        g421FastUpper<true, false, true>(st, 0x01f645cbu, 0x0bu, true, false, true,
                                         stampBase + 1, hb, g504UnusedSlot, 0x03000201u);
        g421FastUpper<true, false, true>(st, 0x01e0ad90u, 0x10u, true, false, true,
                                         stampBase + 2, hb, g504UnusedSlot, 0x00000501u);
        g421FastUpper<true, false, true>(st, 0x01f411bcu, 0x58u, true, false, true,
                                         stampBase + 3, hb, g504UnusedSlot, 0x00000402u);
        g421FastUpper<true, false, true>(st, 0x01f418bdu, 0x49u, true, false, true,
                                         stampBase + 4, hb, g504UnusedSlot, 0x01000202u);
        g421FastUpper<true, false, true>(st, 0x01eebe15u, 0x15u, true, false, true,
                                         stampBase + 5, hb, g504UnusedSlot, 0x01000601u);
        G481_LOWER_BODY_IADDI(st, 0x80094ff2u);
        g421FastUpper<true, false, true>(st, 0x01f4254au, 0x0au, true, false, true,
                                         stampBase + 6, hb, g504UnusedSlot, 0x02000201u);
        g547LqFull<20u, 8u, 3>(st, vuData);
        g421FastUpper<true, false, true>(st, 0x01e049bfu, 0x5bu, true, false, true,
                                         stampBase + 7, hb, g504UnusedSlot, 0x03000402u);

        // Fixed IBNE tail and its fixed full-SQI delay slot, both with live FMAC uppers.
        g421FastUpper<true, false, true>(st, 0x01f628bcu, 0x48u, true, false, true,
                                         stampBase + 8, hb, g504UnusedSlot, 0x00000202u);
        st.pc = 0x0e40u;
        G481_LOWER_BODY_IBNE(st, 0x520907f7u);
        g421FastUpper<true, false, true>(st, 0x01f630bdu, 0x49u, true, false, true,
                                         stampBase + 9, hb, g504UnusedSlot, 0x01000202u);
        g547SqiFull<24u, 8u>(st, vuData);

        // No instruction in the exact ten-pair span reads or produces Q/P, so only exit state is
        // observable and the ten per-pair decrements collapse to one exact span update.
        int qd = *qDelay;
        int pd = *pDelay;
        if (qd > 0)
        {
            if (qd <= 10)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 10;
        }
        if (pd > 0)
        {
            if (pd <= 10)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 10;
        }
        *qDelay = qd;
        *pDelay = pd;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g549ExecuteHot30d8(VU1State &st, uint8_t *vuData,
                            int *qDelay, int *pDelay, int64_t stampBase,
                            G430HistBlock *hb)
    {
        (void)vuData;
        (void)stampBase;
        (void)hb;

        // Four VNOP uppers: IAND, NOP, fixed IBEQ tail, NOP delay slot.
        G481_LOWER_BODY_IAND(st, 0x80050934u);
        st.pc = 0x30e8u;
        G481_LOWER_BODY_IBEQ(st, 0x5004000fu);

        int qd = *qDelay;
        int pd = *pDelay;
        if (qd > 0)
        {
            if (qd <= 4)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 4;
        }
        if (pd > 0)
        {
            if (pd <= 4)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 4;
        }
        *qDelay = qd;
        *pDelay = pd;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g550ExecuteHot1be8(VU1State &st, uint8_t *vuData,
                            int *qDelay, int *pDelay, int64_t stampBase,
                            G430HistBlock *hb)
    {
        g421FastUpper<true, false, true>(st, 0x003dde92u, 0x12u, true, false, true,
                                         stampBase + 0, hb, g504UnusedSlot, 0x02000501u);
        G481_LOWER_BODY_IADDI(st, 0x80094ff2u);
        g421FastUpper<true, false, true>(st, 0x0030ef0bu, 0x0bu, true, false, true,
                                         stampBase + 1, hb, g504UnusedSlot, 0x03000201u);
        g547SqiFull<25u, 6u>(st, vuData);

        int qd = *qDelay;
        int pd = *pDelay;
        if (qd > 0)
        {
            if (qd <= 2)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 2;
        }
        if (pd > 0)
        {
            if (pd <= 2)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 2;
        }
        *qDelay = qd;
        *pDelay = pd;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g551ExecuteHot1c00(VU1State &st, uint8_t *vuData,
                            int *qDelay, int *pDelay, int64_t stampBase,
                            G430HistBlock *hb)
    {
        // 0x1c00: VNOP + full LQI vf20,(vi4++).
        g551LqiFull<20u, 4u>(st, vuData);

        // 0x1c08: FTOI4.xyzw vf17,vf26 + full LQI vf25,(vi5++).
        g421FastUpper<true, false, true>(st, 0x01f1d17du, 0x55u, true, false, true,
                                         stampBase + 1, hb, g504UnusedSlot, 0x01000004u);
        g551LqiFull<25u, 5u>(st, vuData);

        // 0x1c10: MINIy.w vf27,vf28,vf29 + MOVE.xyz vf26,vf16.
        g421FastUpper<true, false, true>(st, 0x003de6d5u, 0x15u, true, false, true,
                                         stampBase + 2, hb, g504UnusedSlot, 0x01000601u);
        g421StoreMasked(st.vf[26], _mm_loadu_ps(st.vf[16]), G481_MASKROW(0xEu));

        // 0x1c18: MULAx.w ACC,vf0,vf29 + full LQI vf16,(vi2++).
        g421FastUpper<true, false, true>(st, 0x003d01bcu, 0x58u, true, false, true,
                                         stampBase + 3, hb, g504UnusedSlot, 0x00000402u);
        g551LqiFull<16u, 2u>(st, vuData);

        // Fixed IBNE tail and full SQI delay slot.
        st.pc = 0x1c20u;
        G481_LOWER_BODY_IBNE(st, 0x520907f8u);
        g547SqiFull<17u, 6u>(st, vuData);

        // None of the six exact pairs reads or produces Q/P.
        int qd = *qDelay;
        int pd = *pDelay;
        if (qd > 0)
        {
            if (qd <= 6)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 6;
        }
        if (pd > 0)
        {
            if (pd <= 6)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 6;
        }
        *qDelay = qd;
        *pDelay = pd;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g554ExecuteHot0900(VU1State &st, uint8_t *vuData,
                            int *qDelay, int *pDelay, int64_t stampBase,
                            G430HistBlock *hb)
    {
        // Literal source words keep every descriptor, operand, mask and lower kind compile-time
        // constant. The sequence is still upper-before-lower for each of the exact 36 pairs.
#define G554_UP(n, word, kind, desc)                                                       \
        g421FastUpper<true, false, true>(st, (word), (kind), true, false, true,            \
                                         stampBase + (n), hb, g504UnusedSlot, (desc))

        G554_UP(0,  0x01f19edau, 0x1au, 0x02000401u);
        G481_LOWER_BODY_LQ(st, 0x01e3383eu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(1,  0x01f195dbu, 0x1bu, 0x03000401u);
        G481_LOWER_BODY_LQ(st, 0x01e4383fu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(2,  0x01f19f1bu, 0x1bu, 0x03000401u);
        G481_LOWER_BODY_LQ(st, 0x01e5403cu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(3,  0x01f409bcu, 0x58u, 0x00000402u);
        G481_LOWER_BODY_LQ(st, 0x01e6403du, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(4,  0x01f410bdu, 0x49u, 0x01000202u);
        G481_LOWER_BODY_LQ(st, 0x01e7403eu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(5,  0x01f418beu, 0x4au, 0x02000202u);
        G481_LOWER_BODY_LQ(st, 0x01e8403fu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(6,  0x01f420bfu, 0x4bu, 0x03000202u);
        G481_LOWER_BODY_LQ(st, 0x01e9483cu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(7,  0x01f528bcu, 0x48u, 0x00000202u);
        G481_LOWER_BODY_LQ(st, 0x01ea483du, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(8,  0x01f530bdu, 0x49u, 0x01000202u);
        G481_LOWER_BODY_LQ(st, 0x01eb483eu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(9,  0x01f538beu, 0x4au, 0x02000202u);
        G481_LOWER_BODY_LQ(st, 0x01ec483fu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(10, 0x01f540bfu, 0x4bu, 0x03000202u);
        G481_LOWER_BODY_LQ(st, 0x01ed503cu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(11, 0x01f648bcu, 0x48u, 0x00000202u);
        G481_LOWER_BODY_LQ(st, 0x01ee503du, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(12, 0x01f650bdu, 0x49u, 0x01000202u);
        G481_LOWER_BODY_LQ(st, 0x01ef503eu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(13, 0x01f658beu, 0x4au, 0x02000202u);
        G481_LOWER_BODY_LQ(st, 0x01f0503fu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(14, 0x01f660bfu, 0x4bu, 0x03000202u);

        G554_UP(15, 0x01f768bcu, 0x48u, 0x00000202u);
        G481_LOWER_BODY_IADDI(st, 0x800318b2u);
        G554_UP(16, 0x01f770bdu, 0x49u, 0x01000202u);
        G554_UP(17, 0x01f778beu, 0x4au, 0x02000202u);
        G481_LOWER_BODY_LQ(st, 0x01f11801u, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(18, 0x01f7878bu, 0x0bu, 0x03000201u);
        G481_LOWER_BODY_LQ(st, 0x01f21001u, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(19, 0x01f909bcu, 0x58u, 0x00000402u);
        G481_LOWER_BODY_LQ(st, 0x01f32001u, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(20, 0x01f910bdu, 0x49u, 0x01000202u);

        G554_UP(21, 0x01f918beu, 0x4au, 0x02000202u);
        g422FastLower(st, 0x81071bfeu, G422_LK_ILWR, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(22, 0x01fa28bcu, 0x48u, 0x00000202u);
        g422FastLower(st, 0x80881bfeu, G422_LK_ILWR, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(23, 0x01fa30bdu, 0x49u, 0x01000202u);
        g422FastLower(st, 0x80491bfeu, G422_LK_ILWR, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(24, 0x01fa38beu, 0x4au, 0x02000202u);
        g422FastLower(st, 0x802a1bfeu, G422_LK_ILWR, vuData, PS2_VU1_DATA_SIZE);

        G554_UP(25, 0x01fb48bcu, 0x48u, 0x00000202u);
        G481_LOWER_BODY_LQ(st, 0x01e1383cu, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(26, 0x01fb50bdu, 0x49u, 0x01000202u);
        G481_LOWER_BODY_LQ(st, 0x01e2383du, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(27, 0x01fb58beu, 0x4au, 0x02000202u);
        G554_UP(28, 0x01fc68bcu, 0x48u, 0x00000202u);
        G554_UP(29, 0x01fc70bdu, 0x49u, 0x01000202u);
        G554_UP(30, 0x01fc77cau, 0x0au, 0x02000201u);

        G554_UP(31, 0x01f19518u, 0x18u, 0x00000401u);
        G481_LOWER_BODY_SQI(st, 0x81e2f37du, vuData, PS2_VU1_DATA_SIZE);
        G554_UP(32, 0x01f19e58u, 0x18u, 0x00000401u);
        G481_LOWER_BODY_IADDI(st, 0x800b5ff2u);
        G554_UP(33, 0x01f19559u, 0x19u, 0x01000401u);

        G554_UP(34, 0x01f19e99u, 0x19u, 0x01000401u);
        st.pc = 0x0a10u;
        G481_LOWER_BODY_IBNE(st, 0x520b07ddu);
        G554_UP(35, 0x01f1959au, 0x1au, 0x02000401u);
        G481_LOWER_BODY_SQI(st, 0x81e4fb7du, vuData, PS2_VU1_DATA_SIZE);

#undef G554_UP

        // No exact instruction in this span reads or produces Q/P. Preserve the generic block's
        // externally observable countdown/publication result with one 36-pair update.
        int qd = *qDelay;
        int pd = *pDelay;
        if (qd > 0)
        {
            if (qd <= 36)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 36;
        }
        if (pd > 0)
        {
            if (pd <= 36)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 36;
        }
        *qDelay = qd;
        *pDelay = pd;
    }

#if 0 // G563/G564 REFUTED: exact, but the added all-block admission cost is ~+5.5 ms/f on s05.
    // G563: compact exact executor for G554's 0x0900 image. G554 proved that eliminating the
    // generic dispatch is useful work but its 36 literal g421FastUpper expansions made the hot
    // text too large. This record keeps only the fields the exact image can observe: every upper
    // is a full-destination FMAC, every source is VF, and the only arithmetic forms are MUL and
    // MADD. The high nibble selects one of the six exact lower bodies; the low nibble is
    // bc[1:0] | vf-destination[2] | madd[3]. One eight-byte record replaces the fused source,
    // upper descriptor and lower-class streams used by the generic block body.
    enum : uint8_t
    {
        G563_LO_NOP = 0u,
        G563_LO_LQ = 1u,
        G563_LO_IADDI = 2u,
        G563_LO_ILWR = 3u,
        G563_LO_SQI = 4u,
        G563_LO_IBNE = 5u,
    };

    struct G563CompactPair
    {
        uint32_t lower;
        uint8_t fs;
        uint8_t ft;
        uint8_t fd;
        uint8_t ops;
    };
    static_assert(sizeof(G563CompactPair) == 8u);

    constexpr G563CompactPair g563MakeCompactPair(size_t index, uint8_t upperOps,
                                                   uint8_t lowerOp)
    {
        const uint64_t source = G554_HOT_0900_SOURCE[index];
        const uint32_t upper = static_cast<uint32_t>(source >> 32u);
        return {
            static_cast<uint32_t>(source),
            static_cast<uint8_t>((upper >> 11u) & 31u),
            static_cast<uint8_t>((upper >> 16u) & 31u),
            static_cast<uint8_t>((upper >> 6u) & 31u),
            static_cast<uint8_t>(upperOps | (lowerOp << 4u)),
        };
    }

    // Upper low nibble: bc | VF-destination | MADD. The exact-source rebuild predicate above is
    // the authority for every word; this table is only a compact execution form of those words.
    constexpr G563CompactPair G563_HOT_0900_PAIRS[] = {
        g563MakeCompactPair(0,  0x6u, G563_LO_LQ),
        g563MakeCompactPair(1,  0x7u, G563_LO_LQ),
        g563MakeCompactPair(2,  0x7u, G563_LO_LQ),
        g563MakeCompactPair(3,  0x0u, G563_LO_LQ),
        g563MakeCompactPair(4,  0x9u, G563_LO_LQ),
        g563MakeCompactPair(5,  0xau, G563_LO_LQ),
        g563MakeCompactPair(6,  0xbu, G563_LO_LQ),
        g563MakeCompactPair(7,  0x8u, G563_LO_LQ),
        g563MakeCompactPair(8,  0x9u, G563_LO_LQ),
        g563MakeCompactPair(9,  0xau, G563_LO_LQ),
        g563MakeCompactPair(10, 0xbu, G563_LO_LQ),
        g563MakeCompactPair(11, 0x8u, G563_LO_LQ),
        g563MakeCompactPair(12, 0x9u, G563_LO_LQ),
        g563MakeCompactPair(13, 0xau, G563_LO_LQ),
        g563MakeCompactPair(14, 0xbu, G563_LO_NOP),
        g563MakeCompactPair(15, 0x8u, G563_LO_IADDI),
        g563MakeCompactPair(16, 0x9u, G563_LO_NOP),
        g563MakeCompactPair(17, 0xau, G563_LO_LQ),
        g563MakeCompactPair(18, 0xfu, G563_LO_LQ),
        g563MakeCompactPair(19, 0x0u, G563_LO_LQ),
        g563MakeCompactPair(20, 0x9u, G563_LO_NOP),
        g563MakeCompactPair(21, 0xau, G563_LO_ILWR),
        g563MakeCompactPair(22, 0x8u, G563_LO_ILWR),
        g563MakeCompactPair(23, 0x9u, G563_LO_ILWR),
        g563MakeCompactPair(24, 0xau, G563_LO_ILWR),
        g563MakeCompactPair(25, 0x8u, G563_LO_LQ),
        g563MakeCompactPair(26, 0x9u, G563_LO_LQ),
        g563MakeCompactPair(27, 0xau, G563_LO_NOP),
        g563MakeCompactPair(28, 0x8u, G563_LO_NOP),
        g563MakeCompactPair(29, 0x9u, G563_LO_NOP),
        g563MakeCompactPair(30, 0xeu, G563_LO_NOP),
        g563MakeCompactPair(31, 0x4u, G563_LO_SQI),
        g563MakeCompactPair(32, 0x4u, G563_LO_IADDI),
        g563MakeCompactPair(33, 0x5u, G563_LO_NOP),
        g563MakeCompactPair(34, 0x5u, G563_LO_IBNE),
        g563MakeCompactPair(35, 0x6u, G563_LO_SQI),
    };
    static_assert(sizeof(G563_HOT_0900_PAIRS) / sizeof(G563_HOT_0900_PAIRS[0]) == 36u);

    // Volatile blocks LTCG from turning the compact loop back into G554's refuted full unroll.
    // It is read once per matching block, never per pair.
    volatile uint32_t s_g563Hot0900PairCount = 36u;

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g563ExecuteHot0900Compact(VU1State &st, uint8_t *vuData,
                                   int *qDelay, int *pDelay, int64_t stampBase,
                                   G430HistBlock *hb)
    {
        int histSlot = hb->head;
        // No pre-tail instruction in this exact image reads PC. Establishing the branch pair's
        // architectural PC here keeps the loop single-copy; IBNE at pair 34 observes 0x0a10 just
        // as it does in the generic and G554 bodies.
        st.pc = 0x0a10u;

        const uint32_t pairCount = s_g563Hot0900PairCount;
        for (uint32_t n = 0u; n < pairCount; ++n)
        {
            const G563CompactPair &pair = G563_HOT_0900_PAIRS[n];
            const uint8_t upperOps = static_cast<uint8_t>(pair.ops & 0x0fu);
            const __m128 vs = _mm_loadu_ps(st.vf[pair.fs]);
            const __m128 vt = _mm_loadu_ps(st.vf[pair.ft]);
            __m128 b;
            switch (upperOps & 3u)
            {
            case 0u: b = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(0, 0, 0, 0)); break;
            case 1u: b = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(1, 1, 1, 1)); break;
            case 2u: b = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(2, 2, 2, 2)); break;
            default: b = _mm_shuffle_ps(vt, vt, _MM_SHUFFLE(3, 3, 3, 3)); break;
            }
            __m128 r = _mm_mul_ps(vs, b);
            if ((upperOps & 0x8u) != 0u)
                r = _mm_add_ps(_mm_loadu_ps(st.acc), r);

            // No lower in this image can inspect G430 history. Carrying the cursor therefore
            // leaves every entry byte-identical while removing 35 load/store-forwarded head hops.
            g430PushPendingAt<true>(hb, histSlot, stampBase + static_cast<int64_t>(n), r, 0x0fu);
            if ((upperOps & 0x4u) != 0u)
                _mm_storeu_ps(st.vf[pair.fd], r);
            else
                _mm_storeu_ps(st.acc, r);

            switch (pair.ops >> 4u)
            {
            case G563_LO_NOP:
                break;
            case G563_LO_LQ:
            {
                // Every exact LQ has dest=0xf. Use G514's promoted whole-vector transfer without
                // retaining the generic masked-lane arm that can never be reached for this image.
                const uint8_t it = LIT(pair.lower);
                const uint8_t is = LIS(pair.lower);
                const int16_t imm = IMM11(pair.lower);
                uint32_t addr =
                    static_cast<uint32_t>(static_cast<int32_t>(st.vi[is] + imm)) * 16u;
                addr &= (PS2_VU1_DATA_SIZE - 1u);
                if (addr + 16u <= PS2_VU1_DATA_SIZE)
                    _mm_storeu_ps(st.vf[it],
                                  _mm_loadu_ps(reinterpret_cast<const float *>(vuData + addr)));
                break;
            }
            case G563_LO_IADDI:
                G481_LOWER_BODY_IADDI(st, pair.lower);
                break;
            case G563_LO_ILWR:
                g422FastLower(st, pair.lower, G422_LK_ILWR, vuData, PS2_VU1_DATA_SIZE);
                break;
            case G563_LO_SQI:
            {
                // Both exact SQIs also have dest=0xf.
                const uint8_t it = LIT(pair.lower);
                const uint8_t is = LIS(pair.lower);
                uint32_t addr = static_cast<uint32_t>(static_cast<uint16_t>(st.vi[it])) * 16u;
                addr &= (PS2_VU1_DATA_SIZE - 1u);
                if (addr + 16u <= PS2_VU1_DATA_SIZE)
                    _mm_storeu_ps(reinterpret_cast<float *>(vuData + addr),
                                  _mm_loadu_ps(st.vf[is]));
                if (it != 0u)
                    st.vi[it] = static_cast<int16_t>(st.vi[it] + 1);
                break;
            }
            case G563_LO_IBNE:
                G481_LOWER_BODY_IBNE(st, pair.lower);
                break;
            default:
                break;
            }
        }
        hb->head = histSlot;

        int qd = *qDelay;
        int pd = *pDelay;
        if (qd > 0)
        {
            if (qd <= 36)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 36;
        }
        if (pd > 0)
        {
            if (pd <= 36)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 36;
        }
        *qDelay = qd;
        *pDelay = pd;
    }

    // G564: the middle shape between G554's 5.8 KiB expansion and G563's refuted compact
    // interpreter. Every operand, broadcast, destination and lower word is a native constant,
    // but the image is split into three noinline segments so the compiler sees only the narrow
    // MUL/MADD + full-destination contract rather than expanding g421FastUpper's generic body 36
    // times. Each segment carries G430's cursor locally and commits it before the next segment.
#define G564_UP(n, fs, ft, fd, bc, vfDest, madd)                                          \
        {                                                                                 \
            const __m128 g564Vs = _mm_loadu_ps(st.vf[(fs)]);                              \
            const __m128 g564Vt = _mm_loadu_ps(st.vf[(ft)]);                              \
            const __m128 g564B = _mm_shuffle_ps(                                          \
                g564Vt, g564Vt, _MM_SHUFFLE((bc), (bc), (bc), (bc)));                    \
            __m128 g564R = _mm_mul_ps(g564Vs, g564B);                                     \
            if constexpr ((madd) != 0)                                                    \
                g564R = _mm_add_ps(_mm_loadu_ps(st.acc), g564R);                          \
            g430PushPendingAt<true>(hb, histSlot, stampBase + (n), g564R, 0x0fu);          \
            if constexpr ((vfDest) != 0)                                                  \
                _mm_storeu_ps(st.vf[(fd)], g564R);                                        \
            else                                                                          \
                _mm_storeu_ps(st.acc, g564R);                                             \
        }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g564ExecuteHot0900Segment0(VU1State &st, uint8_t *vuData, int64_t stampBase,
                                    G430HistBlock *hb)
    {
        int histSlot = hb->head;
        G564_UP(0, 19, 17, 27, 2, 1, 0);
        G514_LOWER_BODY_LQ(st, 0x01e3383eu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(1, 18, 17, 23, 3, 1, 0);
        G514_LOWER_BODY_LQ(st, 0x01e4383fu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(2, 19, 17, 28, 3, 1, 0);
        G514_LOWER_BODY_LQ(st, 0x01e5403cu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(3, 1, 20, 0, 0, 0, 0);
        G514_LOWER_BODY_LQ(st, 0x01e6403du, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(4, 2, 20, 0, 1, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01e7403eu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(5, 3, 20, 0, 2, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01e8403fu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(6, 4, 20, 0, 3, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01e9483cu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(7, 5, 21, 0, 0, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01ea483du, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(8, 6, 21, 0, 1, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01eb483eu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(9, 7, 21, 0, 2, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01ec483fu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(10, 8, 21, 0, 3, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01ed503cu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(11, 9, 22, 0, 0, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01ee503du, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(12, 10, 22, 0, 1, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01ef503eu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(13, 11, 22, 0, 2, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01f0503fu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(14, 12, 22, 0, 3, 0, 1);
        hb->head = histSlot;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g564ExecuteHot0900Segment1(VU1State &st, uint8_t *vuData, int64_t stampBase,
                                    G430HistBlock *hb)
    {
        int histSlot = hb->head;
        G564_UP(15, 13, 23, 0, 0, 0, 1);
        G481_LOWER_BODY_IADDI(st, 0x800318b2u);
        G564_UP(16, 14, 23, 0, 1, 0, 1);
        G564_UP(17, 15, 23, 0, 2, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01f11801u, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(18, 16, 23, 30, 3, 1, 1);
        G514_LOWER_BODY_LQ(st, 0x01f21001u, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(19, 1, 25, 0, 0, 0, 0);
        G514_LOWER_BODY_LQ(st, 0x01f32001u, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(20, 2, 25, 0, 1, 0, 1);
        G564_UP(21, 3, 25, 0, 2, 0, 1);
        g422FastLower(st, 0x81071bfeu, G422_LK_ILWR, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(22, 5, 26, 0, 0, 0, 1);
        g422FastLower(st, 0x80881bfeu, G422_LK_ILWR, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(23, 6, 26, 0, 1, 0, 1);
        g422FastLower(st, 0x80491bfeu, G422_LK_ILWR, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(24, 7, 26, 0, 2, 0, 1);
        g422FastLower(st, 0x802a1bfeu, G422_LK_ILWR, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(25, 9, 27, 0, 0, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01e1383cu, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(26, 10, 27, 0, 1, 0, 1);
        G514_LOWER_BODY_LQ(st, 0x01e2383du, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(27, 11, 27, 0, 2, 0, 1);
        G564_UP(28, 13, 28, 0, 0, 0, 1);
        G564_UP(29, 14, 28, 0, 1, 0, 1);
        G564_UP(30, 14, 28, 31, 2, 1, 1);
        hb->head = histSlot;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g564ExecuteHot0900Segment2(VU1State &st, uint8_t *vuData, int64_t stampBase,
                                    G430HistBlock *hb)
    {
        int histSlot = hb->head;
        G564_UP(31, 18, 17, 20, 0, 1, 0);
        G514_LOWER_BODY_SQI(st, 0x81e2f37du, vuData, PS2_VU1_DATA_SIZE);
        G564_UP(32, 19, 17, 25, 0, 1, 0);
        G481_LOWER_BODY_IADDI(st, 0x800b5ff2u);
        G564_UP(33, 18, 17, 21, 1, 1, 0);
        G564_UP(34, 19, 17, 26, 1, 1, 0);
        st.pc = 0x0a10u;
        G481_LOWER_BODY_IBNE(st, 0x520b07ddu);
        G564_UP(35, 18, 17, 22, 2, 1, 0);
        G514_LOWER_BODY_SQI(st, 0x81e4fb7du, vuData, PS2_VU1_DATA_SIZE);
        hb->head = histSlot;
    }

#undef G564_UP

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g564ExecuteHot0900Narrow(VU1State &st, uint8_t *vuData,
                                  int *qDelay, int *pDelay, int64_t stampBase,
                                  G430HistBlock *hb)
    {
        g564ExecuteHot0900Segment0(st, vuData, stampBase, hb);
        g564ExecuteHot0900Segment1(st, vuData, stampBase, hb);
        g564ExecuteHot0900Segment2(st, vuData, stampBase, hb);

        int qd = *qDelay;
        int pd = *pDelay;
        if (qd > 0)
        {
            if (qd <= 36)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 36;
        }
        if (pd > 0)
        {
            if (pd <= 36)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 36;
        }
        *qDelay = qd;
        *pDelay = pd;
    }

#endif

// G555's compact six-pair exact fragment. Keep it out of line: unlike G554, this adds only two
// live upper bodies and four small lower bodies to the selected path.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g555ExecuteHot1110(VU1State &st, uint8_t *vuData,
                            int *qDelay, int *pDelay, int64_t stampBase,
                            G430HistBlock *hb)
    {
        // 0x1110: VNOP + full LQ vf25, 1(vi7).
        G481_LOWER_BODY_LQ(st, 0x01f93801u, vuData, PS2_VU1_DATA_SIZE);

        // 0x1118: MINIy.xyzw vf18,vf21,vf26 + full LQ vf20, 1(vi8).
        g421FastUpper<true, false, true>(st, 0x01eead95u, 0x15u, true, false, true,
                                         stampBase + 1, hb, g504UnusedSlot, 0x01000601u);
        G481_LOWER_BODY_LQ(st, 0x01f44001u, vuData, PS2_VU1_DATA_SIZE);

        // 0x1120/0x1128: two self IADDIs; both uppers are VNOP.
        G481_LOWER_BODY_IADDI(st, 0x80094ff2u);
        G481_LOWER_BODY_IADDI(st, 0x80073872u);

        // Fixed IBNE tail and its live-upper full-SQI delay slot.
        st.pc = 0x1130u;
        G481_LOWER_BODY_IBNE(st, 0x520907fbu);
        g421FastUpper<true, false, true>(st, 0x01f9a56au, 0x2au, true, false, true,
                                         stampBase + 5, hb, g504UnusedSlot, 0x00030401u);
        G481_LOWER_BODY_SQI(st, 0x81e8b37du, vuData, PS2_VU1_DATA_SIZE);

        // This exact span neither reads nor produces Q/P.
        int qd = *qDelay;
        int pd = *pDelay;
        if (qd > 0)
        {
            if (qd <= 6)
            {
                qd = 0;
                st.q = s_vuQPending;
            }
            else
                qd -= 6;
        }
        if (pd > 0)
        {
            if (pd <= 6)
            {
                pd = 0;
                st.p = s_vuPPending;
            }
            else
                pd -= 6;
        }
        *qDelay = qd;
        *pDelay = pd;
    }

    // G547: dynamic exact-fragment census, carried only by G533's isolated stat arm (29). The
    // shipped arm never calls this code. Count full structural hits separately: only those can be
    // replaced by a fixed static handler without changing block boundaries or budget behavior.
    struct G547BlockRow
    {
        uint64_t blocks = 0u;
        uint64_t full = 0u;
        uint64_t pairs = 0u;
        uint32_t count = 0u;
        uint32_t tail = 0u;
        uint32_t mixed = 0u;
        uint64_t signature = 0u;
        uint64_t sourceMixed = 0u;
    };
    thread_local std::array<G547BlockRow, PS2_VU1_CODE_SIZE / 8u> s_g547Blocks{};
    thread_local std::array<uint64_t, PS2_VU1_CODE_SIZE / 8u> s_g547Signatures{};
    thread_local uint64_t s_g547TotalBlocks = 0u;
    thread_local uint64_t s_g547LastReport = 0u;

    void g547RefreshSignatures()
    {
        static const bool s_on = f50_12_vu_env_flag("DC2_G533_STAT");
        if (!s_on)
            return;
        for (size_t index = 0u; index < s_g547Signatures.size(); ++index)
        {
            const uint32_t count = s_g410Cache.g490RunLen[index];
            const uint32_t tail = s_g410Cache.g502TailAt[index];
            const uint32_t tailPairs = tail == G531_FLAG_TAIL ? 1u : tail;
            const uint32_t span = count + tailPairs;
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t n = 0u; n < span && index + n < s_g410Cache.sources.size(); ++n)
            {
                const uint64_t source = s_g410Cache.sources[index + n];
                for (unsigned byte = 0u; byte < 8u; ++byte)
                {
                    hash ^= static_cast<uint8_t>(source >> (byte * 8u));
                    hash *= 1099511628211ull;
                }
            }
            s_g547Signatures[index] = hash;
        }
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g547NoteBlock(uint32_t firstIndex, uint32_t count, uint32_t tailCode)
    {
        if (firstIndex >= s_g547Blocks.size())
            return;
        G547BlockRow &row = s_g547Blocks[firstIndex];
        const uint32_t tailPairs = tailCode == G531_FLAG_TAIL ? 1u : tailCode;
        ++row.blocks;
        row.pairs += count + tailPairs;
        if (row.blocks == 1u)
        {
            row.count = count;
            row.tail = tailCode;
            row.signature = s_g547Signatures[firstIndex];
        }
        else if (row.count != count || row.tail != tailCode)
            ++row.mixed;
        if (row.signature != s_g547Signatures[firstIndex])
            ++row.sourceMixed;

        const uint32_t plannedCount = s_g410Cache.g490RunLen[firstIndex];
        const uint32_t plannedTail = s_g410Cache.g502TailAt[firstIndex];
        if (count == plannedCount && tailCode == plannedTail)
            ++row.full;

        ++s_g547TotalBlocks;
        if (s_g547TotalBlocks - s_g547LastReport < 10000000ull)
            return;
        s_g547LastReport = s_g547TotalBlocks;

        std::array<uint16_t, PS2_VU1_CODE_SIZE / 8u> order{};
        for (size_t i = 0u; i < order.size(); ++i)
            order[i] = static_cast<uint16_t>(i);
        std::partial_sort(order.begin(), order.begin() + 16u, order.end(),
            [](uint16_t a, uint16_t b) {
                return s_g547Blocks[a].full > s_g547Blocks[b].full;
            });
        std::fprintf(stderr, "[G547:block] total=%llu top=16 exactSource=1\n",
                     static_cast<unsigned long long>(s_g547TotalBlocks));
        for (size_t rank = 0u; rank < 16u; ++rank)
        {
            const uint32_t index = order[rank];
            const G547BlockRow &r = s_g547Blocks[index];
            if (r.blocks == 0u)
                break;
            const uint32_t plannedCount = s_g410Cache.g490RunLen[index];
            const uint32_t plannedTail = s_g410Cache.g502TailAt[index];
            const uint32_t plannedTailPairs = plannedTail == G531_FLAG_TAIL ? 1u : plannedTail;
            const uint32_t span = plannedCount + plannedTailPairs;
            uint64_t hash = 1469598103934665603ull;
            for (uint32_t n = 0u; n < span && index + n < s_g410Cache.sources.size(); ++n)
            {
                const uint64_t source = s_g410Cache.sources[index + n];
                for (unsigned byte = 0u; byte < 8u; ++byte)
                {
                    hash ^= static_cast<uint8_t>(source >> (byte * 8u));
                    hash *= 1099511628211ull;
                }
            }
            std::fprintf(stderr,
                         "[G547:rank] n=%zu pc=%04x blocks=%llu full=%llu fullPct=%.3f"
                         " pairs=%llu shape=%u+%u mixed=%u srcMix=%llu fnv=%016llx src=",
                         rank + 1u, index * 8u,
                         static_cast<unsigned long long>(r.blocks),
                         static_cast<unsigned long long>(r.full),
                         100.0 * static_cast<double>(r.full) /
                             static_cast<double>(s_g547TotalBlocks ? s_g547TotalBlocks : 1u),
                         static_cast<unsigned long long>(r.pairs), plannedCount, plannedTail,
                         r.mixed, static_cast<unsigned long long>(r.sourceMixed),
                         static_cast<unsigned long long>(hash));
            for (uint32_t n = 0u; n < span && index + n < s_g410Cache.sources.size(); ++n)
            {
                const size_t slot = index + n;
                const G410CompiledUpper &pair = s_g410Cache.uppers[slot];
                const G421UpperDesc desc = g421DescTableConst.e[pair.upperKind];
                std::fprintf(stderr,
                             "%s%04x:%016llx/u%02x/l%02x/d%u.%u.%u.%u/p%02x/g%02x",
                             n == 0u ? "" : ",", static_cast<unsigned>(slot * 8u),
                             static_cast<unsigned long long>(s_g410Cache.sources[slot]),
                             static_cast<unsigned>(pair.upperKind),
                             static_cast<unsigned>(pair.lowerKind),
                             static_cast<unsigned>(desc.cls),
                             static_cast<unsigned>(desc.op),
                             static_cast<unsigned>(desc.src),
                             static_cast<unsigned>(desc.bc),
                             static_cast<unsigned>(pair.pairFlags),
                             static_cast<unsigned>(pair.g425Flags));
            }
            std::fprintf(stderr, "\n");
        }
        std::fflush(stderr);
    }

    thread_local uint64_t s_g533Hits = 0u;
    thread_local uint64_t s_g533Fallbacks = 0u;
    thread_local uint64_t s_g533HitPairs = 0u;
    thread_local uint64_t s_g533LastReport = 0u;

#if defined(_MSC_VER)
    __declspec(noinline)
#endif
    void g533NotePlan(bool hit, uint32_t pairs)
    {
        if (hit)
        {
            ++s_g533Hits;
            s_g533HitPairs += pairs;
        }
        else
            ++s_g533Fallbacks;
        const uint64_t total = s_g533Hits + s_g533Fallbacks;
        if (total - s_g533LastReport >= 10000000ull)
        {
            s_g533LastReport = total;
            std::fprintf(stderr,
                         "[G533:stat] blocks=%llu hits=%llu fallback=%llu hitPct=%.3f pairs=%llu\n",
                         static_cast<unsigned long long>(total),
                         static_cast<unsigned long long>(s_g533Hits),
                         static_cast<unsigned long long>(s_g533Fallbacks),
                         total != 0u ? 100.0 * static_cast<double>(s_g533Hits) /
                                           static_cast<double>(total) : 0.0,
                         static_cast<unsigned long long>(s_g533HitPairs));
            std::fflush(stderr);
        }
    }

    // Census is a separate arm: the candidate priced by paired A/B never pays counters,
    // report-threshold checks, or a runtime flag branch in its per-block leaf.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g533ExecuteBlockTailStat(VU1State &st, uint32_t firstIndex, uint32_t count,
                                  uint8_t *vuData, uint32_t dataSize,
                                  int *qDelay, int *pDelay,
                                  int64_t stampBase, G430HistBlock *hb, uint32_t tailCode)
    {
        g547NoteBlock(firstIndex, count, tailCode);
        const bool hot3088 = g533Hot3088Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot0c70 = g547Hot0c70Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot0e00 = g548Hot0e00Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot30d8 = g549Hot30d8Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot1be8 = g550Hot1be8Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot1c00 = g551Hot1c00Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot0900 = g554Hot0900Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot1110 = g555Hot1110Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const uint32_t hotPairs = hot0900 ? 36u : hot1110 ? 6u :
            hot3088 ? 10u : hot0c70 ? 6u : hot0e00 ? 10u :
            hot30d8 ? 4u : hot1be8 ? 2u : hot1c00 ? 6u : 0u;
        g533NotePlan(hot3088 || hot0c70 || hot0e00 || hot30d8 || hot1be8 || hot1c00 ||
                         hot0900 || hot1110,
                     hotPairs);
        if (hot3088)
        {
            g533ExecuteHot3088(st, vuData, qDelay, pDelay, stampBase, hb);
            return;
        }
        if (hot0c70)
        {
            g547ExecuteHot0c70(st, vuData, qDelay, pDelay, stampBase, hb);
            return;
        }
        if (hot0e00)
        {
            g548ExecuteHot0e00(st, vuData, qDelay, pDelay, stampBase, hb);
            return;
        }
        if (hot30d8)
        {
            g549ExecuteHot30d8(st, vuData, qDelay, pDelay, stampBase, hb);
            return;
        }
        if (hot1be8)
        {
            g550ExecuteHot1be8(st, vuData, qDelay, pDelay, stampBase, hb);
            return;
        }
        if (hot1c00)
        {
            g551ExecuteHot1c00(st, vuData, qDelay, pDelay, stampBase, hb);
            return;
        }
        if (hot0900)
        {
            g554ExecuteHot0900(st, vuData, qDelay, pDelay, stampBase, hb);
            return;
        }
        if (hot1110)
        {
            g555ExecuteHot1110(st, vuData, qDelay, pDelay, stampBase, hb);
            return;
        }
        g531ExecuteBlockTailFlag(st, firstIndex, count, vuData, dataSize,
                                 qDelay, pDelay, stampBase, hb, tailCode);
    }

    std::atomic<uint64_t> s_g533Verified{0u};

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g533ExecuteBlockTailVerify(VU1State &st, uint32_t firstIndex, uint32_t count,
                                    uint8_t *vuData, uint32_t dataSize,
                                    int *qDelay, int *pDelay,
                                    int64_t stampBase, G430HistBlock *hb, uint32_t tailCode)
    {
        const bool hot3088 = g533Hot3088Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot0c70 = g547Hot0c70Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot0e00 = g548Hot0e00Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot30d8 = g549Hot30d8Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot1be8 = g550Hot1be8Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot1c00 = g551Hot1c00Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot0900 = g554Hot0900Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        const bool hot1110 = g555Hot1110Runnable(
            firstIndex, count, tailCode, vuData, dataSize, hb);
        if (!hot3088 && !hot0c70 && !hot0e00 && !hot30d8 && !hot1be8 && !hot1c00 &&
            !hot0900 && !hot1110)
        {
            g531ExecuteBlockTailFlag(st, firstIndex, count, vuData, dataSize,
                                     qDelay, pDelay, stampBase, hb, tailCode);
            return;
        }

        const VU1State before = st;
        const G430HistBlock histBefore = *hb;
        const int qBefore = *qDelay;
        const int pBefore = *pDelay;
        const uint32_t macBefore = s_vuMacVisible;
        // Register/history/pipeline state is shadowed on every hit. Sample RAM every 1,024 hits;
        // 0x3088 only loads; 0x0c70, 0x1c00, 0x0900, and 0x1110 also contain fixed stores.
        const uint64_t verifyOrdinal = s_g533Verified.load(std::memory_order_relaxed) / 6u;
        const bool hasMem = vuData != nullptr && dataSize <= PS2_VU1_DATA_SIZE &&
                            (verifyOrdinal & 1023u) == 0u;
        // G554/G555 SQIs can alias an earlier LQ source. Even on non-full-RAM samples, restore
        // their exact aligned quads between generic and static execution so both start from
        // byte-identical RAM. No preceding lower instruction writes these entry base registers.
        const uint32_t targetMemCount = vuData == nullptr ? 0u :
            (hot0900 ? 2u : (hot1110 ? 1u : 0u));
        uint32_t targetMemAddr[2] = {};
        uint8_t targetMemBefore[2][16] = {};
        uint8_t targetMemExpected[2][16] = {};
        if (hot0900 && targetMemCount != 0u)
        {
            targetMemAddr[0] =
                (static_cast<uint32_t>(static_cast<uint16_t>(before.vi[2])) * 16u) &
                (PS2_VU1_DATA_SIZE - 1u);
            targetMemAddr[1] =
                (static_cast<uint32_t>(static_cast<uint16_t>(before.vi[4])) * 16u) &
                (PS2_VU1_DATA_SIZE - 1u);
        }
        else if (hot1110 && targetMemCount != 0u)
            targetMemAddr[0] =
                (static_cast<uint32_t>(static_cast<uint16_t>(before.vi[8])) * 16u) &
                (PS2_VU1_DATA_SIZE - 1u);
        for (uint32_t i = 0u; i < targetMemCount; ++i)
            std::memcpy(targetMemBefore[i], vuData + targetMemAddr[i], 16u);
        if (hasMem)
            std::memcpy(s_g422VerMemBefore, vuData, dataSize);

        g531ExecuteBlockTailFlag(st, firstIndex, count, vuData, dataSize,
                                 qDelay, pDelay, stampBase, hb, tailCode);
        const VU1State expected = st;
        const G430HistBlock histExpected = *hb;
        const int qExpected = *qDelay;
        const int pExpected = *pDelay;
        const uint32_t macExpected = s_vuMacVisible;
        for (uint32_t i = 0u; i < targetMemCount; ++i)
            std::memcpy(targetMemExpected[i], vuData + targetMemAddr[i], 16u);
        if (hasMem)
            std::memcpy(s_g422VerMemFast, vuData, dataSize);

        st = before;
        *hb = histBefore;
        *qDelay = qBefore;
        *pDelay = pBefore;
        s_vuMacVisible = macBefore;
        if (hasMem)
            std::memcpy(vuData, s_g422VerMemBefore, dataSize);
        else
            for (uint32_t i = 0u; i < targetMemCount; ++i)
                std::memcpy(vuData + targetMemAddr[i], targetMemBefore[i], 16u);

        if (hot3088)
            g533ExecuteHot3088(st, vuData, qDelay, pDelay, stampBase, hb);
        else if (hot0c70)
            g547ExecuteHot0c70(st, vuData, qDelay, pDelay, stampBase, hb);
        else if (hot0e00)
            g548ExecuteHot0e00(st, vuData, qDelay, pDelay, stampBase, hb);
        else if (hot30d8)
            g549ExecuteHot30d8(st, vuData, qDelay, pDelay, stampBase, hb);
        else if (hot1be8)
            g550ExecuteHot1be8(st, vuData, qDelay, pDelay, stampBase, hb);
        else if (hot1c00)
            g551ExecuteHot1c00(st, vuData, qDelay, pDelay, stampBase, hb);
        else if (hot0900)
            g554ExecuteHot0900(st, vuData, qDelay, pDelay, stampBase, hb);
        else
            g555ExecuteHot1110(st, vuData, qDelay, pDelay, stampBase, hb);

        const bool badState = !g422StatesEqual(expected, st);
        const bool badHist = std::memcmp(&histExpected, hb, sizeof(histExpected)) != 0;
        const bool badPipe = qExpected != *qDelay || pExpected != *pDelay;
        const bool badMac = macExpected != s_vuMacVisible;
        bool badTargetMem = false;
        for (uint32_t i = 0u; i < targetMemCount; ++i)
            badTargetMem = badTargetMem ||
                std::memcmp(targetMemExpected[i], vuData + targetMemAddr[i], 16u) != 0;
        const bool badMem = badTargetMem ||
            (hasMem && std::memcmp(s_g422VerMemFast, vuData, dataSize) != 0);
        if (badState || badHist || badPipe || badMac || badMem)
        {
            int badVf = -1, badVfLane = -1, badVi = -1, badAcc = -1;
            for (int vf = 0; vf < 32 && badVf < 0; ++vf)
                for (int lane = 0; lane < 4; ++lane)
                    if (std::memcmp(&expected.vf[vf][lane], &st.vf[vf][lane],
                                    sizeof(float)) != 0)
                    {
                        badVf = vf;
                        badVfLane = lane;
                        break;
                    }
            for (int vi = 0; vi < 16 && badVi < 0; ++vi)
                if (expected.vi[vi] != st.vi[vi])
                    badVi = vi;
            for (int lane = 0; lane < 4 && badAcc < 0; ++lane)
                if (std::memcmp(&expected.acc[lane], &st.acc[lane], sizeof(float)) != 0)
                    badAcc = lane;
            auto fbits = [](float v) {
                uint32_t bits = 0u;
                std::memcpy(&bits, &v, sizeof(bits));
                return bits;
            };
            std::fprintf(stderr,
                         "[G533:verify-BAD] state=%d hist=%d pipe=%d mac=%d mem=%d"
                         " q=%d/%d p=%d/%d\n"
                         "[G533:verify-state] vf=%d.%d:%08x/%08x vi=%d:%08x/%08x"
                         " acc=%d:%08x/%08x q=%08x/%08x p=%08x/%08x i=%08x/%08x"
                         " pc=%04x/%04x mac=%08x/%08x clip=%08x/%08x status=%08x/%08x"
                         " ebit=%d/%d itop=%08x/%08x xitop=%08x/%08x\n",
                         static_cast<int>(badState), static_cast<int>(badHist),
                         static_cast<int>(badPipe), static_cast<int>(badMac),
                         static_cast<int>(badMem), qExpected, *qDelay, pExpected, *pDelay,
                         badVf, badVfLane,
                         badVf >= 0 ? fbits(expected.vf[badVf][badVfLane]) : 0u,
                         badVf >= 0 ? fbits(st.vf[badVf][badVfLane]) : 0u,
                         badVi, badVi >= 0 ? static_cast<uint32_t>(expected.vi[badVi]) : 0u,
                         badVi >= 0 ? static_cast<uint32_t>(st.vi[badVi]) : 0u,
                         badAcc, badAcc >= 0 ? fbits(expected.acc[badAcc]) : 0u,
                         badAcc >= 0 ? fbits(st.acc[badAcc]) : 0u,
                         fbits(expected.q), fbits(st.q), fbits(expected.p), fbits(st.p),
                         fbits(expected.i), fbits(st.i), expected.pc, st.pc,
                         expected.mac, st.mac, expected.clip, st.clip,
                         expected.status, st.status, static_cast<int>(expected.ebit),
                         static_cast<int>(st.ebit), expected.itop, st.itop,
                         expected.xitop, st.xitop);
            std::fflush(stderr);
            std::abort();
        }

        const uint64_t pairCount = hot0900 ? 36u : hot1110 ? 6u :
            hot1be8 ? 2u : hot30d8 ? 4u :
            (hot0c70 || hot1c00) ? 6u : 10u;
        const uint64_t n =
            s_g533Verified.fetch_add(pairCount, std::memory_order_relaxed) + pairCount;
        if (n == pairCount || n / 10000000u != (n - pairCount) / 10000000u)
        {
            std::fprintf(stderr, "[G533:verify] pairs=%llu bad=0\n",
                         static_cast<unsigned long long>(n));
            std::fflush(stderr);
        }
    }

#if DC2_G532_FLAGHEAD
    // G532 arms 25/26: the G531 body with one typed flag-consumer prologue. `firstIndex` names the
    // ordinary run, while the prologue is `firstIndex - 1`; the caller advances stampBase by one.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g532ExecuteBlockHeadFlag(VU1State &st, uint32_t firstIndex, uint32_t count,
                                  uint8_t *vuData, uint32_t dataSize,
                                  int *qDelay, int *pDelay,
                                  int64_t stampBase, G430HistBlock *hb, uint32_t tailCode)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, false, false, false, true, false, true, false>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCode);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g532ExecuteBlockHeadVerify(VU1State &st, uint32_t firstIndex, uint32_t count,
                                    uint8_t *vuData, uint32_t dataSize,
                                    int *qDelay, int *pDelay,
                                    int64_t stampBase, G430HistBlock *hb, uint32_t tailCode)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, false, false, false, true, true, true, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCode);
    }
#endif

#if DC2_G516_UPDESC
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g516ExecuteBlockTailDispDense(VU1State &st, uint32_t firstIndex, uint32_t count,
                                       uint8_t *vuData, uint32_t dataSize,
                                       int *qDelay, int *pDelay,
                                       int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      true, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

    // G516 ORACLE COPY — arm 15's body with the per-pair record comparison (DC2_G516_VERIFY=1).
    // Never a timing arm: its flag is what selects it, so it cannot end up in a copy the flag
    // deselects (G489 6), and the shipped body carries none of its live values.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
#endif // DC2_G516_UPDESC

    // G516 ORACLE COPY — arm 18's body with the per-pair record comparison (DC2_G516_VERIFY=1).
    // Never a timing arm: its flag is what selects it, so it cannot end up in a copy the flag
    // deselects (G489 6), and the shipped body carries none of its live values.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g516ExecuteBlockTailVerify(VU1State &st, uint32_t firstIndex, uint32_t count,
                                    uint8_t *vuData, uint32_t dataSize,
                                    int *qDelay, int *pDelay,
                                    int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, true, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

    // G516b: arm 17 — arm 12's body with the four biggest remaining indirect-branch destinations
    // peeled into direct compares. No new array, no load removed: the isolated BRANCH variable.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g516ExecuteBlockTailPeel4(VU1State &st, uint32_t firstIndex, uint32_t count,
                                   uint8_t *vuData, uint32_t dataSize,
                                   int *qDelay, int *pDelay,
                                   int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, false, false, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

#if DC2_G517_INDARM
    // G517: arms 19 / 20 / 21 / 22 — arm 18's body with the pair loop's index expressions
    // strength-reduced. One architectural variable per arm (G436, and G516 §3a's lesson that the
    // halves of a combined lever must be priced alone before the pair is believed):
    //   19 `fpind`    — the two POINTERS (`&fused[firstIndex+j]`, `&g516LowCls[firstIndex+j]`).
    //   20 `stampind` — the running STAMP (`stampBase + j`).
    //   21 `g517ind`  — both; the promotion gate. Overlap does not compose (G493): both mechanisms
    //                   free registers in the same leaf, so the pair is measured, never summed.
    //   22           — arm 21's body plus the induction oracle, selected only by DC2_G517_VERIFY=1.
    // Control arm for all four is 18, the G516 shipped body, so every arm executes the identical
    // pairs in the identical blocks and differs only in how three addresses are formed.
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g517ExecuteBlockTailPtrInd(VU1State &st, uint32_t firstIndex, uint32_t count,
                                    uint8_t *vuData, uint32_t dataSize,
                                    int *qDelay, int *pDelay,
                                    int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, true, false>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g517ExecuteBlockTailStampInd(VU1State &st, uint32_t firstIndex, uint32_t count,
                                      uint8_t *vuData, uint32_t dataSize,
                                      int *qDelay, int *pDelay,
                                      int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, false, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g517ExecuteBlockTailInd(VU1State &st, uint32_t firstIndex, uint32_t count,
                                 uint8_t *vuData, uint32_t dataSize,
                                 int *qDelay, int *pDelay,
                                 int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, true, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g517ExecuteBlockTailVerify(VU1State &st, uint32_t firstIndex, uint32_t count,
                                    uint8_t *vuData, uint32_t dataSize,
                                    int *qDelay, int *pDelay,
                                    int64_t stampBase, G430HistBlock *hb, uint32_t tailCount)
    {
        g490BlockBody<true, false, true, true, false, false, true, true, true, false, true,
                      false, true, false, true, true, true, true>(
            st, firstIndex, count, vuData, dataSize, qDelay, pDelay, stampBase, hb, tailCount);
    }
#endif // DC2_G517_INDARM
#endif // DC2_G506_QPSEG

#if DC2_G502_BLKCHAIN
    // G502: the same body with the chain step compiled in. A separate INSTANTIATION rather than a
    // runtime `if` (G436) — a selector inside the leaf would sit on both arms and bias the gate —
    // and a separate `noinline` wrapper so run()'s own text is unchanged (G481's inline-budget
    // cliff binds on run(), not on a leaf).
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g502ExecuteBlockChain(VU1State &st, uint32_t firstIndex, uint32_t count,
                               uint8_t *vuData, uint32_t dataSize,
                               int *qDelay, int *pDelay,
                               int64_t stampBase, G430HistBlock *hb, uint32_t tailCount,
                               G502ChainOut &out, uint32_t codeSize, uint32_t budget)
    {
        g490BlockBody<true, false, true, true, true>(st, firstIndex, count, vuData, dataSize,
                                                     qDelay, pDelay, stampBase, hb, tailCount,
                                                     &out, codeSize, budget);
    }
#endif // DC2_G502_BLKCHAIN
#endif // DC2_G500_BLKBRANCH

#if DC2_G499_QPLOCAL
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    void g499ExecuteBlockFastQp(VU1State &st, uint32_t firstIndex, uint32_t count,
                                uint8_t *vuData, uint32_t dataSize,
                                int *qDelay, int *pDelay,
                                int64_t stampBase, G430HistBlock *hb)
    {
        g490BlockBody<true, true>(st, firstIndex, count, vuData, dataSize, qDelay, pDelay,
                                  stampBase, hb);
    }
#endif // DC2_G499_QPLOCAL

    // One-shot proof of selection (G457): a copy that was never selected proves nothing, and a
    // run-length table that covers nothing is a null with no information in it.
    void g490ReportArmed(bool armed)
    {
        static const bool s_show = g490StatOn() || (std::getenv("DC2_G419_AB") != nullptr);
        if (!s_show)
            return;
        static std::atomic<uint32_t> s_once[2] = {};
        const int slot = armed ? 1 : 0;
        if (s_once[slot].fetch_add(1u, std::memory_order_relaxed) != 0u)
            return;
        uint32_t eligible = 0u;
        uint32_t runs = 0u;
        for (size_t i = 0u; i < s_g410Cache.g490RunLen.size(); ++i)
        {
            if (s_g410Cache.g490RunLen[i] != 0u)
                ++eligible;
            if (s_g410Cache.g490RunLen[i] >= G490_MIN_RUN &&
                (i == 0u || s_g410Cache.g490RunLen[i - 1u] == 0u))
                ++runs;
        }
        std::fprintf(stderr,
                     "[G490:armed] block=%d eligibleSlots=%u (%.2f%%) runStarts=%u\n",
                     armed ? 1 : 0, eligible,
                     100.0 * static_cast<double>(eligible) /
                         static_cast<double>(s_g410Cache.g490RunLen.size()),
                     runs);
        std::fflush(stderr);
    }

    // G499: the same one-shot proof of selection, per arm (G457). A paired A/B in which only one
    // arm was ever built measures nothing, and this phase's whole premise is that a copy can be
    // present in the source and unreachable on the shipped path (G490's own lesson).
    void g499ReportArmed(int arm)
    {
        static const bool s_show = g499StatOn() || (std::getenv("DC2_G419_AB") != nullptr);
        if (!s_show)
            return;
        // G501: five slots, not three. With `s_once[3]` and `arm > 2 -> slot 0`, the `blkpin`
        // gate's two arms (3 and 4) BOTH folded onto slot 0, so exactly one `[G499:blk]` line
        // would print and the run could not prove both bodies were built — the G457 failure this
        // helper exists to prevent. (G500's arms happened to be 1 and 3, which landed on different
        // slots by luck.)
        // G516: SIXTEEN slots, not eight. With `s_once[8]` every arm above 7 folded onto slot 0, so
        // a `vmem` / `nopdisp` / `dispdense` gate could print at most ONE `[G499:blk]` line and
        // could not prove both bodies were built — the same G457 failure G501 fixed here by going
        // from three slots to eight, one arc later and for the same reason.
        // G517: TWENTY-FOUR slots, not sixteen. Arms 19–22 would all fold onto slot 0 and a
        // `g517ind` gate could print at most ONE `[G499:blk]` line — the same G457 failure G501 and
        // G516 already fixed here, third firing. Widen this in the SAME edit that adds an arm.
        // G555: THIRTY-SEVEN slots. Arm 36 is the 0x1110 immutable fragment expansion; folding it
        // onto slot 0 would hide proof that both paired bodies were selected.
        static std::atomic<uint32_t> s_once[37] = {};
        const int slot = (arm < 0 || arm > 36) ? 0 : arm;
        if (s_once[slot].fetch_add(1u, std::memory_order_relaxed) != 0u)
            return;
        std::fprintf(stderr, "[G499:blk] arm=%d\n", arm);
        std::fflush(stderr);
    }
} // namespace
