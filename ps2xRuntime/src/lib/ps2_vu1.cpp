// G520: `macscan` — g430ResolveAt (vu1_g421_fast_upper.inc) resolves the architectural MAC with a
// branchless tree reduction instead of a serial branchy scan of the 8-slot history ring. Content
// edit here forces MSBuild to consume the .inc (G359). revision: 28 (G612 native region backend)
// G652: native-region LOI admission + flag-materialization census (force rebuild v1).
#include "ps2_vu1_parts/vu1_helpers_and_tables.inc"
#include "ps2_vu1_parts/vu1_g370_store_watch.inc"
#include "ps2_g480_packet_pool.inc"
#include "ps2_vu1_parts/vu1_core_execution.inc"
#include "ps2_vu1_parts/vu1_g481_hot_lower.inc"
#include "ps2_vu1_parts/vu1_g422_fast_lower.inc"
#include "ps2_vu1_parts/vu1_g410_compiled_execution.inc"
#include "ps2_vu1_parts/vu1_g421_fast_upper.inc"
#include "ps2_vu1_parts/vu1_g426_fused_upper.inc"
#include "ps2_vu1_parts/vu1_g421_census.inc"
#include "ps2_vu1_parts/vu1_g475_vnop_trace.inc"
// G610: the NATIVE VU1 BLOCK COMPILATION BACKEND — x86-64 emitter + translator. Must come AFTER
// vu1_g421_fast_upper.inc (it reads `g421DescTableConst` and `g421MaskTableConst`) and BEFORE
// vu1_g490_block_run.inc, which holds the compiled-block ENTRY and calls `g610CompileAll` from the
// same cold rebuild pass that maintains g490RunLen.
#include "ps2_vu1_parts/vu1_g610_native_jit.inc"
// G612: the NATIVE VU1 REGION backend — the same emitter, given a control-flow graph. Must come
// AFTER vu1_g610_native_jit.inc (it CALLS `g610EmitUpper` / `g610EmitLower` rather than
// re-deriving one line of arithmetic) and BEFORE vu1_g490_block_run.inc, which holds the region
// ENTRY, the oracle, and the `g612CompileAll` call in the same cold rebuild pass.
#include "ps2_vu1_parts/vu1_g612_native_region.inc"
// G490: after vu1_g421_fast_upper.inc — its eligibility test reads `g421DescTableConst`, and its
// block body calls the inlined g421FastUpper / g422FastLower directly.
#include "ps2_vu1_parts/vu1_g490_block_run.inc"
// G533: immutable microprogram-fragment plans and static exact-source specialization
//       (v8 promoted default-on; direct exact-block gate with G531-canonical fallback).
#include "ps2_vu1_parts/vu1_g483_run_cycles.inc"
// G607: per-kick VU1 control-flow profile (default OFF, DC2_G607_PROF=1). Observer only; lives
// in the DC2_VU1_LOOP_DIAG copy of the pair loop, so the shipped body is untouched. Must precede
// vu1_upper_opcodes.inc, which arms it at run() entry.
#include "ps2_vu1_parts/vu1_g607_pc_profile.inc"
// G608: per-PROGRAM-POINT VU-RAM read/write footprint (default OFF, DC2_G608_MEMPROF=1). The one
// precondition G607 parked program-point GPU batching behind. Observer only, same DIAG-copy
// discipline as G607, and likewise must precede vu1_upper_opcodes.inc which arms it at run() entry.
#include "ps2_vu1_parts/vu1_g608_mem_profile.inc"
#include "ps2_vu1_parts/vu1_upper_opcodes.inc"
#include "ps2_vu1_parts/vu1_lower_opcodes.inc"
#include "ps2_vu1_parts/vu1_dispatch_and_sync.inc"
#include "ps2_vu1_parts/vu1_g541_pipeline_snapshot.inc"
// G665: the INVERSE of the G541 snapshot plus the G566 state encoding - VU1 backend state IMPORT.
// Must come AFTER vu1_g541_pipeline_snapshot.inc: its self-test calls the writer it inverts, and
// exactness of that round trip is the whole point. Marshalling only; nothing calls it unless a
// GPU->CPU migration actually happens, so the default path pays nothing (Rule 12c).
#include "ps2_vu1_parts/vu1_g665_backend_migration.inc"
// G360: item-slot UV-loss XGKICK probe added in vu1_dispatch_and_sync.inc (force recompile v2).
// G541: stable hidden-pipeline snapshots + ordered XGKICK addresses for the VU1 GPU corpus (v1).
// G370: VU1 store-watch probe added (force recompile).
// G410: default-on cached compiled VU1 execution (force recompile v16).
// G413: default-on G328 circular MAC delay + G330 fused MAC classifier (force recompile).
// G421: default-on inline SIMD upper slot + hot-loop census (kill DC2_G421_NO_FAST_UPPER, v3).
// G422: default-on inline lower slot (kill DC2_G422_NO_FAST_LOWER, force recompile v1).
// G425: zero-work pair run census + run-collapsing lever (force recompile v1).
// G426: branchless fused upper FMAC dispatch (DC2_G426_FUSED_UPPER, force recompile v1).
// G475: compact VNOP-upper trace runway census + trace arm (force recompile v2).
// G427: default-on lazy MAC/STATUS delay-line publication (kill DC2_G427_NO_LAZY_FLAGS, v1).
// G428: fast-upper SIMD-op census + dest-shape store + deferred MAC classification (v2).
// G430: DC2_G430_SKIP ceiling probe + default-on stamped MAC history (force recompile v2).
// G435: pair loop split into vu1_g435_pair_loop.inc (included twice); three OPT-IN levers
//       (DC2_G435_LEAN_LOOP / _WIDE_VF0 / _HAZ_REG) + the DC2_G435_SKIP ceiling probe (v5).
// G436: default-on lean hot-path addressing — constexpr G421 desc/mask tables (no init guard, so
//       no `call g421Desc` per covered upper), one G430HistBlock thread_local hoisted once per
//       run(), and the per-pair s_g106UseLowerFlagRead store gated on G106 parallel flags.
//       Exact and frame-neutral; kill DC2_G436_LEGACY_ADDR=1 (force recompile v1).
// G437: VU1 register-file alignment premise probe DC2_G437_ALIGN=1 (force recompile v1).
// G438: whole-tree alignment sweep probe DC2_G438_ALIGN=1 (force recompile v1).
// G442: subnormal census (compile-time DC2_G442_DENORM_CENSUS), rejected register-resident PC,
//       and the branch-delay loop-carried-local fold (force recompile v3).

// G481: the seven HOT G422 lower kinds inlined into the pair loop; bodies shared token-for-token
//       with g422FastLower via vu1_g481_hot_lower.inc (kill DC2_G481_NO_HOT_LOWER, force
//       recompile v1).
// G479: census + FUSED pair record + Q/P delay loop locals REJECTED, masked PC wrap REJECTED, fused record PROMOTED (force recompile v8).
// G484: the nine scalar-pipe / flag-pipe thread_locals folded into ONE alignas(64) G484VuBlock, so
//       the pair loop hoists ONE base pointer instead of seven (DC2_G419_AB=tlsblock,
//       DC2_G484_TLSBLOCK=1, force recompile v1).
// G485: the wide VF0 re-pin PROMOTED default-on — one 16-byte store of (0,0,0,1) replaces four
//       scalar stores at the bottom of every pair (mean -0.619 ms/f, sign 4/4; rollback
//       DC2_G485_NO_WIDE_VF0=1, proof-of-selection DC2_G485_STAT=1, force recompile v1).
// G485: register-form G139 pair hazard measured for the first time (compile-time copy property
//       DC2_G485_HAZREG) and REFUTED at mean +0.232 ms/f, sign 3/4 slower; copy removed, #if
//       scaffolding kept in vu1_g435_pair_loop.inc (force recompile v3).
// G486: the wide single-lane FMAC destination store was built as a loop copy and REFUTED
//       (mean +0.597 ms/f, sign 4/4 slower); all its plumbing was removed, so the default pair
//       loop and g428StoreDest are source-identical to G485's. See the refutation at
//       g428StoreDest in vu1_g421_fast_upper.inc.
// G486: [G486:uncov] names the lower opcodes that fall through g422FastLower into the full
//       execLower switch (DC2_G421_CENSUS=1, diagnostic only, non-TLS storage, force recompile v3).
// G487: audit relink to obtain a linker /MAP for THIS binary — which of g421FastUpper /
//       g422FastLower / g481 hot bodies MSVC actually inlined, and what execLower/execUpper
//       really cost at their call boundary (G480/G483 technique, force recompile v1).
// G487: COMPILED FLAG-CONSUMER LOWERS — FCAND/FCOR/FMAND/FCGET (2.54% of pairs, ~41 k/f) leave
//       execLower for g422FastLower, which this binary's /MAP proves is fully inlined into run().
//       The arm is a property of the G410 pair CACHE (re-decoded on every flip), never an in-loop
//       compare. PROMOTED default-on: mean −0.55 ms/f, sign 4/4. Gate DC2_G419_AB=flaglower,
//       ROLLBACK DC2_G487_NO_FLAG_LOWER=1, proof DC2_G487_STAT=1, oracle DC2_G422_VERIFY=1
//       (force recompile v3).
// G488: COMPILED CROSS-LANE / FLAG-ONLY UPPERS — OPMSUB (0x2E), OPMULA (0x6E) and CLIPw (0x5F),
//       the whole `[G421:cover] uncovered` population on this route (~1.08% of pairs, ~17.6 k/f),
//       leave execUpper for the inlined g421FastUpper. OPMSUB/OPMULA reuse G421's FMAC tail
//       verbatim (MAC/STATUS, G428 park, G430 stamped history, g428StoreDest) and differ only in
//       one outer-product permutation of the operands; CLIPw joins the cls>=3 group and writes
//       only m_state.clip. The arm is a property of the G410 pair CACHE (control decodes SHADOW
//       kinds 0x72/0x73/0x74 whose descriptor entries are zero), so the loop body is byte-identical
//       in both arms. Also closes a latent out-of-bounds read: G421DescTable/G426CtlTable were
//       indexed with the raw 0xff fallback kind. Gate DC2_G419_AB=fastop, ROLLBACK
//       DC2_G488_NO_FAST_OP=1, proof DC2_G488_STAT=1, oracle DC2_G421_VERIFY=1
//       (force recompile v2 — v2 is the /MAP audit relink: the paired gate cannot see loop TEXT
//       growth because both arms carry the new bodies, so the G481 inline-budget question is
//       answered from the map instead).
// G489: /MAP audit relink to re-take G484 §2's stack-traffic census on the CURRENT loop. G484
//       measured `[rbp+0x118]` (&m_state) at 24 reloads/pair and attributed them to the
//       out-of-line calls in the body; G487/G488 have since removed the frequent execLower /
//       execUpper calls from the covered population, so that premise has to be re-measured
//       before any lever is built on it (force recompile v1).
// G489: default-on cached "this lower can write pc" bit (G489_PF_CANBRANCH) so the pair loop's
//       TAIL stops reading m_state.pc back on every pair to ask whether the lower slot branched.
//       Gate DC2_G419_AB=pcskip, ROLLBACK DC2_G489_NO_PCSKIP=1, proof DC2_G489_STAT=1,
//       oracle DC2_G489_VERIFY=1 (force recompile v2).
// G490: WIDENED BLOCK RUN — a whole run of eligible pairs executed in one noinline call
//       (vu1_g490_block_run.inc), paying the per-pair ENVELOPE that G489 measured at 54.7% of all
//       VU1 time ONCE per run instead of once per pair. Covers every upper class g421FastUpper
//       executes, not only VNOP, so it prices the block-translation arc's real premise. Also
//       records that G475's trace copy has been STRUCTURALLY UNREACHABLE since G479 was promoted
//       (its selector arms sit below `else if (g443Fixed && g479LeanFetch)`), so G478's +0.34 ms/f
//       verdict describes a pre-G479/pre-G485 body in both arms. Gate DC2_G419_AB=blockrun,
//       standalone DC2_G490_BLOCK=1, oracle DC2_G490_VERIFY=1, stats DC2_G490_STAT=1
//       (force recompile v1).
// G490 map relink v1.
// G490 oracle copy compiled out (force recompile v2).
// G490 map relink v2.
// G499: the G490 block executor's LOWER DISPATCH, brought level with the pair loop's. The block
//       body called g422FastLower for every pair — it never got G480's NOP skip (the callee's whole
//       body for that kind is `return;`, and NOP is 39.5% of all pairs) nor G481's inline hot
//       INTEGER kinds (IADDI 6.99% + IADDIU 3.07% + IAND 3.02%), both promoted default-ON in
//       DC2_G422_RUN_LOWER since G481. One template body, two noinline instantiations, so the
//       control arm's text is unchanged. Gate DC2_G419_AB=blklower, ROLLBACK DC2_G499_NO_BLKLOWER=1,
//       proof DC2_G499_STAT=1 (force recompile v1).
// G499 §2: the Q/P delay counters as BLOCK LOCALS. The same body read-modify-wrote both through
//       `int *` on every pair with an inlined g422FastLower (which stores into vuData) in between,
//       so MSVC had to reload them from memory at the top of every iteration. Block eligibility
//       excludes every writer of those counters, so inside a block they can only count down and a
//       local written back once is exact. NOT G479's refuted QPREG — that was run()'s
//       register-starved pair loop; this is a 4.7 KB leaf. Gate DC2_G419_AB=qpblock (control = the
//       blklower candidate), ROLLBACK DC2_G499_NO_QPLOCAL=1 (force recompile v2).
//       REFUTED and COMPILED OUT (DC2_G499_QPLOCAL 0): -0.050/+0.191/-0.057/-0.031 ms/f, mean
//       +0.013, signs disagree. Sixth confirmation of the live-value law and the FIRST taken
//       outside run()'s pair loop (force recompile v3).
// G500: let a block run END on the CONTROL TRANSFER that terminates it, and swallow that branch's
//       delay slot. Control transfer is 8.88% of all pairs and ~69% of all run terminations (why
//       the average run is 7.81), and a TAKEN branch's delay slot is excluded twice over because
//       the block hook is gated on `pendingBranchTarget == NO_BRANCH`. The tail only EXTENDS a run
//       that already formed a block, so blocks/frame is invariant across the arms. New sidecar
//       table g500Tail[] (0/1/2), built by the same g490RebuildRunLengths pass. Gate
//       DC2_G419_AB=blkbranch (control = the G499 shipped body), ROLLBACK DC2_G500_NO_BLKBRANCH=1,
//       per-arm invariant DC2_G500_STAT=1, oracle DC2_G490_VERIFY=1 (force recompile v1).
// G501: PIN the block executor's loop-invariant ADDRESSING predicates. `dumpbin /DISASM` of the
//       G500 block body g490BlockBody<1,0,1> (0x3538 bytes) showed `hb` reloaded from its STACK
//       SLOT [rbp+80h] TWELVE times, the global s_g428DestStore loaded SIX times, and ELEVEN dead
//       calls (g421Desc x3, g421MaskRows x8) on arms `g443Fixed` already proves unreachable —
//       three of the `hb` tests on the MAIN per-pair path. The pair loop folds all of this through
//       G443's constexpr shadows; the block leaf lost it at the call boundary because `hb` is a
//       runtime pointer parameter. `G501Pin` restores the fold inside the leaf: 12 -> 4 stack
//       refs, 6 -> 0 global loads, 11 -> 0 calls, 0x3538 -> 0x3364 bytes.
//       UNCONDITIONAL, not an arm — it deletes provably-unreachable code, so there is nothing for
//       a control arm to differ about. Rollback is the pre-existing one: DC2_G436_LEGACY_ADDR=1 or
//       DC2_G428_NO_DEST_STORE=1 falsifies g443Fixed and drops the run onto the legacy loop copy.
//       Gated as DC2_G419_AB=blkpin while it was being measured: -0.167 ms/f mean over 4 paired
//       runs on the shipped link (-0.266 / +0.033 / -0.346 / -0.088, 3/4 negative, 20/32 windows)
//       against -0.755 over 3 runs on the previous link. AT THE INSTRUMENT FLOOR — the scaffolding
//       was removed and the fold kept (force recompile v3).
// G531: typed flag-consumer block tail promoted default-on (force recompile v2).
