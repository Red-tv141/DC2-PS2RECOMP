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
// G490: after vu1_g421_fast_upper.inc — its eligibility test reads `g421DescTableConst`, and its
// block body calls the inlined g421FastUpper / g422FastLower directly.
#include "ps2_vu1_parts/vu1_g490_block_run.inc"
#include "ps2_vu1_parts/vu1_g483_run_cycles.inc"
#include "ps2_vu1_parts/vu1_upper_opcodes.inc"
#include "ps2_vu1_parts/vu1_lower_opcodes.inc"
#include "ps2_vu1_parts/vu1_dispatch_and_sync.inc"
// G360: item-slot UV-loss XGKICK probe added in vu1_dispatch_and_sync.inc (force recompile v2).
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
