// G597: DC2_G597_OWNCHK=1 -> [G597:own]. Counts EVERY conjunct of g536CollectGuestOwnedPages'
// fail-closed gate separately, plus pagesScanned/pagesMoved, because G596 measured that guard
// firing on 0.28-0.38% of publications while guest VRAM moved under 38-80% of them. Behaviour-pure.
// G597 FIX (DC2_G597_SCOPEDANCHOR=1, rollback DC2_G597_NO_SCOPEDANCHOR=1): g264FlushMirror re-anchors
// the PER-PAGE baseline only for the rows it actually mirrored. revision: 2
// G596: register-TRIGGERED transfers (G593 layer 3), re-priced at 5.848 ms/f on `s05` and split by
// [G432:trxdir] into the 4.455 ms/f dependency PIN and the 1.354 ms/f format-aware pixel copy. The
// lever itself lives in ps2_gs_gpu.cpp's parts (g596_fast_local_copy.inc); this TU only gains the
// `g596copy` A/B arm in g419_ab_instrument.inc, so this content edit forces MSBuild to consume it
// (G359). revision: 1
// G595: the drawPrimitive+capture LAYER (G593 layer 2), decomposed and attacked. All of it lives
// in .inc parts — g526_draw_census.inc (the [G595:pro] prologue split), rasterizer_command_graph.inc
// (the note memo + the arm-aware line accessor), rasterizer_setup_and_perf_census.inc and
// rasterizer_tilebin_capture.inc (the lap calls + the two call sites), rasterizer_rtt_census_and_waves.inc
// (the classifier's arm-aware conjunct) and g419_ab_instrument.inc (the arms) — so this content
// edit forces MSBuild to consume them (G359). revision: 3 — both slices PROMOTED DEFAULT-ON; the
// arm accessors hoisted behind one-shot cached bools AND the line predicate reordered to test the
// primitive TYPE before the accessor, after the first two shapes cost the SHIPPED prologue
// +0.15 ms/f (G502 applied to a lever's own selector, then to its conjunct order).
//   * DC2_G595_PRO=1 -> [G595:pro]. Splits `pro`, the layer's LARGEST leaf (1.633 ms/f of 5.50 on
//     `s05 n=1021..2461`), into front/zclr/pile/cull/body/elig. Its six laps sum to the `pro` lap
//     [G526:split] already prints, so the two instruments cross-check by construction.
//   * `g595line` (kG595LeverLineDefer) — G270's display-LINE deferral, refuted in 2026-07-16 on a
//     route that HAS NO LINES. [G526:draw] on `s05` prices the inline exit at 4.2 calls/frame x
//     242.4 us = 1.007 ms/f (18.3% of the layer), all LINESTRIP to fbp 0x0/0x68, tme=0 abe=0 —
//     G270's admission predicate verbatim. The cost is `drain` 51.8 us + `edge` 175.4 us per
//     primitive, not raster (15 us). Measured with the lever on: layer 5.498 -> 4.358 ms/f.
//     Arm DC2_G419_AB=g595line; opt-in DC2_G270_LINE_WAVE=1.
//   * `g595note` (kG595LeverNoteMemo) — a state-identity memo in front of g260NoteAppend, whose
//     four per-entry block ranges are a pure function of the GS state group that [G510:cls]
//     measures 92.80% identical to the predecessor. 1.369 ms/f pool. Exact because
//     G260RangeSet::add is idempotent for a repeated range and reset() (the only shrink) bumps a
//     seq the memo keys on. Arm DC2_G419_AB=g595note; opt-in DC2_G595_NOTEMEMO=1; rollback
//     DC2_G595_NO_NOTEMEMO=1; oracle DC2_G595_NOTEVFY=1; sub-timer DC2_G595_CENSUS=1.
//   * `g595both` is the promotion gate: exactly the shipped pair against exactly the rollback.
// G583: the whole-population CPU-fallback census + the 0x13b transient guest-depth admission, in
// G588: default-on exact CPU solid-sprite replay kernel + authoritative word oracle. Revision: 3.
// DC2_G586_TRANSIENT_141=1 arms only FBW=8 CT32 untextured sprite/tristrip batches with read-only
// ZBP=0xd0/Z24; DC2_G586_VERIFY_TRN141=1 implies it and leaves CPU replay authoritative. This
// content edit forces MSBuild to consume the G586 .inc changes (G359).
// rasterizer_rtt_census_and_waves.inc (both), rasterizer_vram_materialization.inc (the rejecter
// shape stash) and rasterizer_tilebin_capture.inc (the fallback hook) — content edit here forces
// MSBuild to consume them (G359). revision: 1
//   * DC2_G583_REJCENSUS=1 -> [G583:rejcensus] / [G583:rejshape]. Ranks every g178TryFlushGpu
//     fallback by REPLAYED ENTRIES for ANY fbp. G262's census is gated on the five named G248
//     targets and G265's on fbp 0/0x68, so fbp=0x13b — the largest fallback source on the Ridepod
//     boss — was invisible to both, and [G290:gpufail] prints entry 0 rather than the entry that
//     actually rejected.
//   * DC2_G583_TRN13B_Z=1 (default-ON since G585; rollback DC2_G583_NO_TRN13B_Z=1) admits G286's transient 0x13b target to
//     G262/G570's NON-PERSISTENT per-wave guest-depth contract and to the two additive-FIX blend
//     shapes, as one set. Nothing is added to g248TargetIndex and no residency survives the
//     flush, so this cannot reproduce G569's persistent-ownership escaped-writer defect.
//     Rollback DC2_G583_NO_TRN13B_Z=1; divergence counter DC2_G583_STAT=1 -> [G583:trn13bz].
//   * DC2_G583_VERIFY_TRN13B=1 -> [G583:verify13b]. Same-run exact oracle for that population,
//     reusing G570's 0x13d depth-carrying shape: snapshot colour + the G403/G411 private Z,
//     render the batch on the GPU, restore both authorities, replay on the CPU, compare. G286's
//     own 0x13b oracle is gated `!guestDepth` and has never fired here (G573 recorded zero checks
//     over 2.2 M primitives). The verifier IMPLIES the admission so it cannot compare the CPU
//     path against itself; it renders each admitted batch twice and is never valid for timing.
// G524: the transient-target materialize DELETION CEILING PROBE, in
// rasterizer_vram_materialization.inc — content edit here forces MSBuild to consume it (G359).
// revision: 1
//   * DC2_G524_NO_TRNMAT=1 (default-off, knowingly WRONG output) deletes the single
//     g261PrepareCpuReplay() the g286 transient-target admission runs in the GPU flush prologue.
//     That call is `pro0trn`, G523 §6b's named ⭐⭐ target: [G279:mat] c0 prices it at 1.031 ms per
//     materialize x ~1.0 per frame (416 rows of 0x139 read back into guest VRAM), and [G523:fit]
//     independently reads pro0trn = 0.0323 ms/flush x 33.4 flushes = 1.08 ms/f.
//     It is a CONSUMER edge (G492), so the deletion is a valid ceiling: the transient sprite's
//     texture decode then reads stale VRAM texels rather than crashing.
// G523: the FIXED-per-flush vs PER-ENTRY fit census for the GPU flush's `pre` passes, in
// rasterizer_vram_materialization.inc — content edit here forces MSBuild to consume it (G359).
// revision: 2 (adds the four prologue laps + the batch-merge headroom census)
//   * DC2_G523_MERGE=1 -> [G523:merge] (rasterizer_command_graph.inc). One GPU flush IS one closed
//     G260Batch and a batch closes only on an fbp switch, so ADJACENT batches never share a target
//     and no adjacent merge exists. This counts how many drained batches could legally coalesce
//     with an EARLIER same-(fbp,fbw) batch — legal only when every batch strictly between them is
//     independent of it (neither writes what the other reads or writes; unknown fails closed).
//     Read-only: nothing is reordered. Bounds the flush-count lever before anyone builds it.
//   * DC2_G523_FIT=1 -> [G523:fit]. Least-squares fit of each per-flush pass (clsPro/clsLoop/
//     clsEpi/class/fbz/deps/tex/verts/submit/post) against that flush's ENTRY count, plus a second
//     fit of `submit` against DRAW count. Intercept = the cost a flush pays carrying zero entries
//     (recoverable ONLY by having fewer flushes); slope = the marginal per-entry cost (conserved
//     under any re-batching). Behaviour-pure, implies DC2_G298_PROFILE's seven timestamps, adds
//     two laps inside `class` (33.4 flushes/frame, so ~67 extra clock reads/frame).
//     Answers the roadmap's standing "33.35 flushes/f x pre 0.140 ms = 4.67 ms/f, fixed share
//     never separated" for every pass at once.
// G521: the G144Entry ENTRY-SIZE INFLATION PROBE + the `capinplace` lever, in .inc parts (struct +
// reporter in rasterizer_tilebinning_and_probes.inc, arm/oracle in g419_ab_instrument.inc, the
// capture site in rasterizer_tilebin_capture.inc) — content edit here forces MSBuild to consume
// them (G359). revision: 2 (probe default ABSENT; capinplace REFUTED, compiled out)
//   * `capinplace` (kG521LeverCapInPlace) — build the captured G144Entry directly in g_g144List
//     instead of filling a stack local and moving it in, deleting one 256-byte write per captured
//     primitive (~9,927/frame on the pole thread). Exact: [G521:verify] entriesChecked=41000000
//     mismatches=0. REFUTED at mean raw -0.065 ms/f (-0.30%, 3/4) but blocked -0.011 (2/4,
//     SIGN-INCONSISTENT). Compiled out behind DC2_G521_INPLACEARM 0; the shipped capture is the
//     pre-G521 aggregate stack local + G510's move-push, byte for byte.
//     Rollback DC2_G521_NO_CAPINPLACE=1; oracle DC2_G521_VERIFY=1 (both inert at ARM 0).
//   * DC2_G521_PADBYTES (compile-time, default 0) appends dead payload to G144Entry so the
//     capture push and all five flush pre-passes stride over a larger entry with NO behaviour
//     change. [G298:profile]'s five per-pass clocks on a padded build vs an unpadded one give
//     the marginal ms/f PER BYTE of entry size — the ceiling for G510 §6's state-table refactor,
//     which is a ~580-site edit and was priced off a bucket total, not off count x cost.
//   * DC2_G521_SIZE=1 -> [G521:size], the one-shot measured layout (so the slope is divided by a
//     number read from the binary, not hand-computed).
// G520: the MAC-history resolve made branchless, in .inc parts (the arm/oracle accessors here in
// g419_ab_instrument.inc, the lever itself in ps2_vu1_parts/vu1_g421_fast_upper.inc) — content
// edit here forces MSBuild to consume them (G359). revision: 3 (REFUTED, compiled out)
//   * `macscan` (kG520LeverMacScan) — g430ResolveAt's 8-slot linear scan over the MAC history
//                ring becomes a branchless 8->4->2->1 max-reduction over an index-packed key.
//                [G478:vu1prof] + /MAP put 4.47% of the VU1 WORKER (~0.8 ms/f, the pole thread in
//                the LOW backend state) inside that 592-byte function, over ~33.4 k flag-reading
//                pairs/frame. Exact by re-association; ties (all stamps init to -1) resolve
//                identically via the `7 - i` low bits.
//                Rollback DC2_G520_NO_MACSCAN=1; oracle DC2_G520_VERIFY=1 -> [G520:verify].
// G519: delete the four CRT rounding calls in G458's per-draw scissor cull, all in .inc parts —
// content edit here forces MSBuild to consume them (G359). revision: 3 (PROMOTED default-ON)
//   * `cullcmp` (kG519LeverCullCmp) — rasterizer_tilebin_capture.inc decides `ceilf(a) < b` and
//                `floorf(a) > b` by comparison against b-1 / b+1 instead of by rounding. Every b is
//                a GSScissorReg uint16_t, so it is an exact integer and the substitution is exact
//                for every float a including NaN and +/-Inf. The cull runs on EVERY drawPrimitive
//                (14,600/f on lean MAP-0) and the G519 PC sampler measured ceilf 1.00% + floorf
//                0.82% = 1.82% of the pole thread (~0.40 ms/f), the four calls being the only
//                reason this path touches the CRT at all.
//                Rollback DC2_G519_NO_CULLCMP=1; standalone DC2_G519_CULLCMP=1;
//                decision oracle DC2_G519_VERIFY=1 -> [G519:verify].
// G511: two INFLATION prices on the flush's per-entry pre-work + one decomposition census, all in
// .inc parts — content edit here forces MSBuild to consume them (G359). revision: 2
//   * `texprobe` (kG511LeverTexProbe) — G502 INFLATION price: arm 1 adds ONE extra s_batchKeys probe
//                per probing entry. MEASURED -0.102 +/- 0.058 ms/f over 5,264 probes/f, i.e. the
//                probe costs <= 2.7 ns and the whole vein <= 0.014 ms/f. Measurement arm only.
//   * `texrun`   (kG511LeverTexRun) — the lever that price refutes: remember the one s_batchKeys key
//                proven resident and turn the repeat's probe into a 64-bit compare. Its own 4-run
//                gate read null in both estimators. **NOT PROMOTED — default OFF**, opt in with
//                DC2_G511_TEXRUN=1. Census DC2_G511_CENSUS=1 -> [G511:tex].
//   * `clsprobe` (kG511LeverClsProbe) — G502 INFLATION price #2: arm 1 repeats G510's state-identity
//                test (4 memcmps + 4 compares) once more per graph entry. Prices the memo's own
//                per-entry comparison cost. Measurement arm only.
//   * DC2_G511_TEXSPLIT=1 -> [G511:texsplit] — decomposes the `tex` pass (1.44 ms/f, the largest of
//                the five pre-passes) into gen / hash / decode and a stated residual.
// G510: two exact GS-worker levers + their paired gates, all in .inc parts — content edit here
// forces MSBuild to consume them (G359). revision: 1
//   * `capmove`  (kG510LeverCapMove) — rasterizer_tilebin_capture.inc moves the captured G144Entry
//                into g_g144List instead of copying it (~9,900 entries/frame on the pole thread).
//                Rollback DC2_G510_NO_CAPMOVE=1.
//   * `clsmemo`  (kG510LeverClsMemo) — rasterizer_vram_materialization.inc reuses the previous
//                entry's G178EntryState when the entry's state registers compare byte-equal.
//                Rollback DC2_G510_NO_CLSMEMO=1. Census DC2_G510_CENSUS=1 → [G510:cls].
//   * `g510both` (kG510LeverBoth) — the combined promotion arm.
// G508: `lockskip` lever + g508LockSkipArm() in g419_ab_instrument.inc.
// G588: promoted `solidsprite` replay commit + paired arm/oracle/depth-no-op proof + paired stores.
// G475: held paired arm for the VU1 compact VNOP trace; force-consume g419 instrument edit.
// G434: DC2_G434_NO_L2LPIN l2l-pin deletion ceiling probe in rasterizer_draw_sprite.inc ג€”
// content edit here forces MSBuild to consume it.
// G431: T8 resident-view rebuild redundancy census (DC2_G431_T8CENSUS=1) in
// rasterizer_vram_materialization.inc ג€” content edit here forces the .inc to be consumed.
// G411 promoted: force MSBuild to consume default shared GPU-depth ownership .inc changes.
// G415: publishes the exact colour-readback row window to the GPU backend (see
// rasterizer_vram_materialization.inc). Touching this file forces the .inc edit to be consumed.
// G417: diagnostic-only CPU framebuffer-pack split lives in that same part.
// G423: promotes G326's exact compiled mapping-plan cache; rollback DC2_G423_NO_PLAN_CACHE=1.
// G429: opt-in ceiling probe DC2_G429_NO_UPLOAD_EDGE=1 in rasterizer_draw_sprite.inc (default-off,
// knowingly incorrect) prices the upload-forced deferred-graph execute.
// G432: local->local flush-edge split census (DC2_G432_CENSUS=1) in rasterizer_draw_sprite.inc ג€”
// content edit here forces the .inc to be consumed. (rev 3: per-stage lap timers + the exact G418
// unpack at the SECOND publication site, g261Materialize; rollback DC2_G432_NO_MAT_UNPACK=1,
// A/B gate DC2_G419_AB=matunpack)
// G447: host core-count probe. Defined at global scope in ps2_gif_arbiter.cpp
// (g447_edge_census.inc), which is the TU that already pulls in <windows.h>. Declared here outside
// the parts' anonymous namespaces so DC2_G447_HOSTCHECK links against the real cross-TU symbol.
// G493: NEW part g493_late_drain.inc, included from rasterizer_vram_materialization.inc — moves the
// async break's drain from the flush TOP to just before that flush's submit, so the pending render
// overlaps the flush's own preparation instead of nothing (force recompile v1).
void g447HostShape(int *logicalOut, int *physicalOut);
int g456UvPredicateArm();
// G498: owner-publish unpack arm. Declared at global scope for the same reason as the line above —
// every part that reads it is inside an anonymous namespace, and the definition lives in
// g419_ab_instrument.inc far below.
int g498OwnPubArm();
// G589: the DRAIN TRANSACTION (GPU-VU command/authority redesign, slice 1). These five must be
// declared at GLOBAL scope here, before any part is included: g589BeginDrain / g589EndDrain are
// called from rasterizer_command_graph.inc (inside an anonymous namespace, and far earlier than
// the definitions), g589MemoEnsureAtlas from rasterizer_t8_maps_and_atlas.inc, g589NoteBatch from
// rasterizer_vram_materialization.inc and g589DisarmTransaction from rasterizer_tilebin_capture.inc.
// Declaring them inside any part's anonymous namespace would name a different, never-defined
// symbol (the G568 lesson). g589DrainMemoArm is the paired A/B accessor defined in
// g419_ab_instrument.inc, same rule as g498OwnPubArm above.
int g589DrainMemoArm();
// G590: window-scoped depth-authority paired arm, defined at global scope in
// g419_ab_instrument.inc and consumed by g590_surface_authority.inc (which is inside the
// anonymous namespace, so it must NOT redeclare it there — the G568 linkage lesson).
int g590ZWinArm();
// G591: the two window-scoped surface-authority paired arms (private-mirror depth epoch, colour
// window authority). Same global-linkage rule as g590ZWinArm above — g591_window_authority.inc
// lives inside the anonymous namespace and must bind to THESE symbols.
int g591PrivZArm();
int g591ColWinArm();
// G592: the private-mirror publication consumer test. Same global-linkage rule as above.
int g592PubConsArm();
// G594: the two logical/T8 atlas-refresh paired arms (owned-page enumeration instead of the
// 128-page x target winner walk; 4-lane guest-page hash instead of the byte-serial 8 KiB FNV).
// Same global-linkage rule as g590ZWinArm — rasterizer_t8_maps_and_atlas.inc is included at line
// 296, ~36 includes BEFORE g419_ab_instrument.inc defines them, and it lives inside the anonymous
// namespace, so it must bind to THESE global declarations (the G568 lesson).
int g594FastWalkArm();
int g594FastHashArm();
int g594FastDeswzArm();
void g589BeginDrain();
void g589EndDrain();
void g589NoteBatch();
void g589DisarmTransaction();
bool g589MemoEnsureAtlas(bool (*real)());
// G499: new A/B lever `blklower` (kG499LeverBlkLower = 58) + its arm accessor g499BlkLowerArm(),
// both defined in g419_ab_instrument.inc below. The consumer is the VU1 TU, which declares it
// `extern` at global scope in vu1_g490_block_run.inc (force recompile v1).
// G499 §2: second lever `qpblock` (kG499LeverQpBlock = 59) + g499QpBlockArm() (force recompile v2).
// G500: new A/B lever `blkbranch` (kG500LeverBlkBranch = 60) + g500BlkBranchArm(), same shape and
// the same G438 thread-level block hold. Consumed by the VU1 TU (force recompile v1).
// G512: DC2_G419_AB=fastdec lever (kG512LeverFastDec = 75) + g512FastDecActive/g512VerifyOn/
//       g512PsmCensusOn/g512PsmNote in g419_ab_instrument.inc, the specialised decode bodies in
//       ps2_gs_rasterizer_parts/g512_fast_texdecode.inc (new, included from
//       rasterizer_command_graph.inc), and the GSMem::G512ReadRow* global-scope declarations in
//       rasterizer_headers_and_diagnostics.inc (force recompile v1).
// G513: DC2_G419_AB=hashlane lever (kG513LeverHashLane = 76) + g513HashLaneArm/g513HashLaneActive/
//       g513VerifyOn and the four verify counters in g419_ab_instrument.inc, the two byte-loop
//       bodies (g513HashMarkedRef / g513HashMarkedFast) plus G178TexReg::hashVar in
//       rasterizer_command_graph.inc, the decision oracle and the ns/byte columns in
//       rasterizer_vram_materialization.inc, and the two global-scope declarations in
//       rasterizer_headers_and_diagnostics.inc (force recompile v1).
// G522: `l2lskip` (kG522LeverL2lSkip) — rasterizer_draw_sprite.inc elides the local->local edge's
// guest-VRAM publication (g285PrepareL2lConsume + g261MaterializeForRanges, 4.73 of that edge's
// 4.90 ms/f) whenever g289CanDeferLocalCopy proves the copy will be an FBO->FBO blit that never
// touches guest VRAM; arm/rollback/census + g144L2lLatePin's counter in g419_ab_instrument.inc.
// Content edit here forces MSBuild to consume both .inc files (G359). revision: 1

#include <deque>

// G599: the SCRIPT clock (defined in ps2_memory.cpp, written once per guest frame by the mgEndFrame
// override). The MAP-15 sky defect occupies scriptFrames ~1958..2440 and its consumer is ~0.06% of
// the T8 traffic, so the write-side census MUST window itself on this rather than on a first-N
// counter (G598 §6). Declared here, BEFORE the first part is included, because
// rasterizer_headers_and_diagnostics.inc opens an anonymous namespace that stays open through every
// later part — a declaration written after it names `(anonymous)::g_dc2ScriptFrame`, an
// internal-linkage variable that is never defined (C7631; the G568 linkage lesson).
#include <atomic>
extern std::atomic<uint32_t> g_dc2ScriptFrame;

#include "ps2_gs_rasterizer_parts/rasterizer_headers_and_diagnostics.inc"

#include "ps2_gs_rasterizer_parts/rasterizer_g214_cap_trace.inc"

    #include "ps2_gs_rasterizer_parts/rasterizer_clipping_and_tex_checks.inc"

    #include "ps2_gs_rasterizer_parts/rasterizer_row_pool.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_tilebinning_and_probes.inc"

// G527 revision: 1 — the runtime render-target registry. MUST follow tilebinning (it reads
// g248TargetIndex to keep the legacy five out of the wide set) and MUST precede the command graph,
// the VRAM materialization unit and the tile-bin capture, which hold the admission, GPU-routing and
// publication sites it re-keys. Content edit here forces MSBuild to consume the .inc files (G359).
#include "ps2_gs_rasterizer_parts/g527_target_registry.inc"

// G568: bit-field-disjoint texture/Z alias admission. It must sit HERE, beside g527, and not later:
// rasterizer_draw_prim_probe.inc below opens an anonymous namespace that stays open until
// rasterizer_rtt_census_and_waves.inc closes it, so an include placed between the two would put
// this file's forward declaration of g568BitAliasArm() in the anonymous namespace while
// g419_ab_instrument.inc defines it at GLOBAL scope — an unresolved external at exe link (the
// declaration of g552RttTriArm in g527_target_registry.inc is global for exactly this reason).
// It must still precede rasterizer_vram_materialization.inc, which holds the single `texAliasesZ`
// rejection site it re-keys. Content edit here forces MSBuild to consume the .inc file (G359).
#include "ps2_gs_rasterizer_parts/g568_bitalias_admission.inc"

// G400/G401: raw surface sampling must be visible at command-graph execution time. G400's
// draw-entry hook remains below; G401 calls the helper before the composite batch executes.
#include "ps2_gs_rasterizer_parts/rasterizer_g400_stage_probe.inc"
#include "ps2_gs_rasterizer_parts/rasterizer_g401_composite_probe.inc"

#include "ps2_gs_rasterizer_parts/rasterizer_draw_prim_probe.inc"

// G526 revision: 3 (PROMOTED default-ON) — the `anytgt` lever (g526AnyTargetSamplerOn / g526FastSpriteTarget in
// rasterizer_tilebinning_and_probes.inc, consumed at the g248FastSampleSprite predicate in
// rasterizer_draw_sprite.inc). Content edit here forces MSBuild to consume both .inc files (G359).
//   * DC2_G526_ANYTGT=1 (default-OFF) admits EVERY render target to the already-promoted G248 fast
//     sprite sampler, instead of only the five hardcoded addresses G248 measured on MAP-0. The
//     sampler reads tex.* and VRAM and never reads frame.fbp, so the target test was scope, not
//     safety. Rollback DC2_G526_NO_ANYTGT=1; per-pixel oracle DC2_G248_VERIFY=1 -> [G248:vfy].
// G526: per-primitive path attribution (DC2_G526_DRAW=1, default-off). Must precede
// rasterizer_setup_and_perf_census.inc and rasterizer_tilebin_capture.inc, which hold the four
// drawPrimitive exits it books against.
#include "ps2_gs_rasterizer_parts/g526_draw_census.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_command_graph.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_g336_runway.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_rtt_census_and_waves.inc"

#include "ps2_gs_rasterizer_parts/g345_closure.inc"

// G590 revision: 1 — SURFACE AUTHORITY census + generalized shadow oracle (both default-off,
// behaviour-pure except the shadow's blocking readback). MUST follow
// rasterizer_rtt_census_and_waves.inc (G261Res / kG261Fbp / G261MatCause / kG248TargetCount and
// the global g242_backend_read_depth declaration) and MUST precede
// rasterizer_gpu_alias_page_view.inc, whose g261Materialize calls g590NoteMaterialize, and
// rasterizer_vram_materialization.inc, which owns the colour/depth upload decisions and the pack
// site the shadow oracle hooks. Content edit here forces MSBuild to consume the .inc files (G359).
#include "ps2_gs_rasterizer_parts/g590_surface_authority.inc"

// G591 revision: 2 — WINDOW-SCOPED SURFACE AUTHORITY. Slice A (the G411 private depth mirror's
// WRITE EPOCH) is PROMOTED DEFAULT-ON; slice B (window-scoped colour authority) is REFUTED by its
// own census and stays default-off. Rollback DC2_G591_NO_PRIVZ=1.
// MUST follow g590_surface_authority.inc (it answers the question G590's shadow oracle opened and
// shares its page-generation vocabulary) and MUST precede rasterizer_gpu_alias_page_view.inc
// (g278FlushPendSlot stamps the private-mirror epoch and g261Report calls g591Report),
// rasterizer_vram_materialization.inc (the colour/depth upload decisions and the pack site), and
// rasterizer_draw_sprite.inc / rasterizer_draw_triangle.inc (the per-draw mirror-epoch bump).
// Content edit here forces MSBuild to consume the .inc files (G359).
#include "ps2_gs_rasterizer_parts/g591_window_authority.inc"

// G399: DC2_G399_SURFDUMP joins the raw-VRAM diag flag set in the part below.
// G535 revision: 3 — PROMOTES the consumer-edge publication to DEFAULT-ON (g535EdgePubOn):
// deferral is kept and the private-mirror owner is published in g528PublishScoped. +3.51% on lean
// MAP-0 vs the broken pre-G535 path, against +42.9% for the in-wave form kept as
// DC2_G535_NO_EDGEPUB=1.
// G535 revision: 1 — the G411 private-mirror depth owner no longer defers its publication
// (rasterizer_vram_materialization.inc: the g278OwnerEligible override and the g278DeferDepth
// predicate). CPU-rasterized draws consume that mirror through g403DisplayZRead/Write and no
// publication edge covered them, which is the game-wide missing-environment defect. Content edit
// here forces MSBuild to consume the .inc (G359). Diagnostic restore: DC2_G535_PRIVZ_DEFER=1.
#include "ps2_gs_rasterizer_parts/rasterizer_gpu_alias_page_view.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_alias_page_state.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_page_authority.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_page_source_maps.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_target_page_authority.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_t8_maps_and_atlas.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_t8_map_resolver.inc"

// G589 revision: 1 — the DRAIN TRANSACTION. MUST follow rasterizer_rtt_census_and_waves.inc
// (kG248TargetCount / g_g261Res), rasterizer_alias_page_state.inc (g_g280Ovl) and
// rasterizer_t8_maps_and_atlas.inc (g_g310ProducerN and the atlas lifecycle flags), because its
// memo key is computed over exactly those; and MUST precede rasterizer_vram_materialization.inc
// and rasterizer_tilebin_capture.inc, which call g589NoteBatch / g589DisarmTransaction. Its
// definitions are at global scope and are declared at the top of this file. Content edit here
// forces MSBuild to consume the .inc files (G359).
#include "ps2_gs_rasterizer_parts/g589_drain_transaction.inc"

// G342 arc#6 slice-1 premise-gate: per-consumer-shape TIME census of c5 tex-alias materialize.
// Included before vram_materialization.inc so g342NoteTexAlias is visible at the trigger site.
#include "ps2_gs_rasterizer_parts/g342_texalias_time_census.inc"

// G343: arc#4 slice-1 ג€” per-shape TIME census of the l2l consumer-floor edge (default-off)
#include "ps2_gs_rasterizer_parts/g343_l2l_floor_census.inc"

// G418: colour-readback VRAM unpack (CPU-copy component of the 0x13b readback edge) ג€” the exact
// table/paired/lane arm plus its profile and oracle. Included before vram_materialization.inc so
// g418UnpackColorRows is visible at the publication site.
#include "ps2_gs_rasterizer_parts/g418_readback_unpack.inc"

// G498: the two OWNER-PUBLISH colour unpacks G418 never reached (g345MaterializeSlot and
// g289Resolve14aBeforeDirect, 262,144 px/frame of per-pixel addrPSMCT32 scatter on the pole
// thread). Included after g418_readback_unpack.inc because it dispatches to G418's promoted arm on
// the rows where the owner's per-pixel block filter is provably a no-op; the two call sites, which
// sit far earlier in this TU, forward-declare g498PublishOwnedRows.
#include "ps2_gs_rasterizer_parts/g498_owner_publish.inc"

// G419: the generalized within-process randomized A/B instrument (DC2_G419_AB) plus the exact CT32
// upload fast pack. Included before vram_materialization.inc so g419AbTick/g419PackFbRows are
// visible at the once-per-frame tick point and at the framebuffer staging pack.
#include "ps2_gs_rasterizer_parts/g419_ab_instrument.inc"

// G420: the two staging-pack residues G419 left untested ג€” the exact zero-fill elision and the
// row-pool lane count ג€” plus the GSRowPool dispatch narrowing the lane-count mechanism turned out
// to require. Included before vram_materialization.inc so g420PrepStaging/g420PackLaneCount are
// visible at the framebuffer staging pack; rasterizer_row_pool.inc forward-declares
// g420PoolNarrowArm because it is included far earlier in this TU.
#include "ps2_gs_rasterizer_parts/g420_pack_residue.inc"

// G600: the MAP-15 backdrop probe (default OFF). Needs g599EnvU32/g_dc2ScriptFrame from
// rasterizer_gpu_alias_page_view.inc above, and must precede vram_materialization.inc, whose
// texture loop carries all three call sites (source decision, registry hit, decode).
#include "ps2_gs_rasterizer_parts/g600_sky_probe.inc"

#include "ps2_gs_rasterizer_parts/rasterizer_vram_materialization.inc"


// G592 revision: 1 — the private-mirror PUBLICATION's consumer test (the READ half of the
// ownership record G591 opened). MUST follow rasterizer_gpu_alias_page_view.inc (G278PendDepth /
// s_g278PendTab / g278FlushPendSlot), rasterizer_tilebinning_and_probes.inc (g_g144List /
// t_g144InReplay), rasterizer_clipping_and_tex_checks.inc (g203UniversalZEnabled /
// g404SharedZScope) and g419_ab_instrument.inc (g592PubConsArm, defined at GLOBAL scope — this
// part is inside an anonymous namespace and must NOT redeclare it: the G568 linkage lesson).
// MUST precede g528_flush_publish.inc, whose G535 edge is its only publication call site, and
// rasterizer_draw_sprite.inc / rasterizer_draw_triangle.inc, which carry the invariant probe.
// Content edit here forces MSBuild to consume the .inc files (G359).
#include "ps2_gs_rasterizer_parts/g592_publish_consumer.inc"

// G528 revision: 1 — the flush prologue, scoped to the executing batch's own page ranges.
// MUST follow rasterizer_gpu_alias_page_view.inc / rasterizer_vram_materialization.inc (it calls
// g278FlushPendingDepth[ForRanges], g289MaterializeOwner / g289MaterializeForRanges,
// g272MaterializeAll / g272MaterializeForRanges, g276FlushPendingDisplay[ForRanges],
// g261PrepareCpuReplay and g242PrepareVramReadAll) and rasterizer_command_graph.inc (g_g528Wr /
// g_g528Rd, G260RangeSet), and MUST precede rasterizer_tilebin_capture.inc, whose two flush
// closures are its only call sites. ⚠️ It must also sit BEFORE rasterizer_setup_and_perf_census.inc:
// that part and tilebin_capture are FRAGMENTS OF GSRasterizer::drawPrimitive's BODY, and a
// namespace cannot be opened inside a function. Content edit forces MSBuild to consume it (G359).
#include "ps2_gs_rasterizer_parts/g528_flush_publish.inc"

// G529 revision: 1 — the CPU band replay's dispatch census + per-batch lane-count policy. MUST
// follow g528_flush_publish.inc (it reuses g528ClassifyFbp so the two populations G527 created are
// never averaged) and rasterizer_row_pool.inc (GSRowPool), and MUST precede
// rasterizer_tilebin_capture.inc, whose parallel flush closure is its only call site. Same
// namespace constraint as G528: it must sit BEFORE rasterizer_setup_and_perf_census.inc, which is
// a FRAGMENT OF GSRasterizer::drawPrimitive's BODY. Content edit forces MSBuild to consume it (G359).
#include "ps2_gs_rasterizer_parts/g529_replay_dispatch.inc"

#include "ps2_gs_rasterizer_parts/rasterizer_setup_and_perf_census.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_tilebin_capture.inc"


// G369 cutscene gray-screen census (default-off: DC2_G369_CENSUS=1). Must sit AFTER the
// anonymous namespace opened in rasterizer_headers_and_diagnostics.inc has closed, because it
// declares the externally-linked g_dc2PresentTick from ps2_runtime.cpp.
#include "ps2_gs_rasterizer_parts/rasterizer_g369_scene_census.inc"

#include "ps2_gs_rasterizer_parts/rasterizer_write_pixel.inc"


// G530 revision: 1 — the replayed sprite pixel, decomposed (ceiling probes) and then rewritten as a
// span kernel. MUST follow rasterizer_write_pixel.inc (it reuses writePixel's exact commit
// semantics) and MUST precede rasterizer_draw_sprite.inc, which is a FRAGMENT OF
// GSRasterizer::drawSprite's BODY and therefore cannot open a namespace of its own.
#include "ps2_gs_rasterizer_parts/g530_sprite_span.inc"


// G570: exact CPU leaf kernel for the combat FBP 0x139 replay.  It reuses G530's CT32 row/group
// swizzle helpers and therefore must follow g530_sprite_span.inc; drawTriangle is its only caller.
#include "ps2_gs_rasterizer_parts/g570_cpu_triangle.inc"

// G580 revision: 2 - exact Z24/CT32 leaves plus a candidate-only batch-shared T8 CLUT for the
// replayed Ridepod 0x13b triangle state. It reuses G530's CT32 row helper, so it must follow
// G530/G570 and precede drawTriangle; forward declarations let the replay dispatcher bind it.
// This content edit also forces MSBuild to consume later g580_cpu_triangle.inc edits (G359).
#include "ps2_gs_rasterizer_parts/g580_cpu_triangle.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_draw_sprite.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_draw_triangle.inc"


#include "ps2_gs_rasterizer_parts/rasterizer_draw_line.inc"
// G401: composite probe also hooks direct drawSprite execution (force recompile).
// G438: DC2_G419_AB=vu1slow|gsslow derivative probes + DC2_G438_HOLD block-held arms (force recompile v3, DC2_G447_HOSTCHECK).
// G439: DC2_G419_AB=coalesce window-count ceiling probe, g439CoalesceArm() (force recompile v2).
// G447: DC2_G419_AB=spinwait lever + kG447LeverSpinWait in g419_ab_instrument.inc (force recompile v1).
// G481: DC2_G419_AB=hotlower lever + g481HotLowerArm in g419_ab_instrument.inc (force recompile v1).
// G482: DC2_G419_AB=memohash lever + g482MemoHashArm in g419_ab_instrument.inc (force recompile v1).
// G484: DC2_G419_AB=tlsblock lever + g484TlsBlockArm in g419_ab_instrument.inc (force recompile v1).
// G488: DC2_G419_AB=fastop lever + g488FastOpArm in g419_ab_instrument.inc; added to the hold-16
//       default set because an arm flip re-decodes the whole G410 pair cache (force recompile v1).
// G454: readback dependency-edge A/B probes + callsite selection (force recompile v1).
// G455: DC2_G455_RUNWAY last-writer/readback runway census (force recompile v4).
// G456: paired exact G407 UV-predicate short circuit + shadow verifier (force recompile v4, promoted).
// G457: rejected narrow-boundary fast path removed; A/B used-state census retained (force recompile v4).
// G458: exact fully-scissor-clipped primitive rejection promoted default-on (force recompile v2).
// G459: rejected triangle/scissor behavior removed; diagnostic census retained (force recompile v2).
// G471: rejected spinclock/dirty-count levers removed; exact page-shift walks + bitset page-overlap
//       + G369 call-site gate hoist retained (force recompile v5).
// G473: DC2_G419_AB=simdmerge paired presentation-latch gate, promoted (force recompile v2).
// G474: frame-boundary g144FlushPending six-stage census (force recompile v1).
// G471: exact power-of-two SHIFT form of the page-range walks (g178PageShifts / g144PsmPageShifts)
//       in rasterizer_command_graph.inc + rasterizer_draw_prim_probe.inc (force recompile v2).
// G444: DC2_G444_ASYNC=1 ceiling probe ג€” admits G310 flushes to the G305 async slot (force recompile v1).
// G445: DC2_G419_AB=earlykick behaviour lever, g445KickArm() (force recompile v1).
// G445: DC2_G419_AB=kickall extension probe, g445KickAllArm() (force recompile v2).
// G446: cache g294OwnerTokenRequested / g283AuthorityRequested (per-page getenv on the pole thread) (force recompile v1).
// G446: cache DC2_DUMP_FONT in the per-texel T4HH sampler (force recompile v2).
// G452: DC2_G419_AB=async1 corrected one-slot G305/G310 async gate (force recompile v1).
// G452: rejected deeper FIFO removed; corrected one-slot diagnostic retained (force recompile v4).
// G453: rejected behavior levers removed (force recompile v6).
// G476: exact run-weighted page-generation walk (g178BumpRectImpl / g178GenSumRect) + visit-ceiling
//       census. Verified exact but PRICED OUT (~940 atomic RMWs/f, ~6 us/f); kept DEFAULT-OFF via
//       DC2_G476_PAGERUN=1, oracle DC2_G476_VERIFY=1, paired gate DC2_G419_AB=pagebump; the
//       lever-selected test is hoisted to a static so the default path pays no per-call query
//       (force recompile v6).
// G477: DC2_G419_AB=runwrite paired gate for the swizzle-row hoisted IMAGE run writers
//       (GSMem::WriteRunCT32/Z32/P4 in ps2_gs_memory.cpp) (force recompile v1).
// G533: DC2_G419_AB=imgspec VU1 immutable-fragment specialization arm (force recompile v1).
// G547: DC2_G419_AB=hot0c70 isolates the second exact VU1 fragment (force recompile v1).
// G548: DC2_G419_AB=hot0e00 isolates the third exact VU1 fragment (force recompile v1).
// G549: DC2_G419_AB=hot30d8 isolates the fourth exact VU1 fragment (force recompile v1).
// G550: DC2_G419_AB=hot1be8 isolates the fifth exact VU1 fragment (force recompile v1).
// G551: DC2_G419_AB=hot1c00 isolates the sixth exact VU1 fragment (force recompile v1).
// G552: DC2_G419_AB=rtttri isolates discovered-target triangle deferral (force recompile v1).
// G553: DC2_G553_RTT159=1 gives s05 target 0x159 a sixth GPU-residency slot; the
//       DC2_G553_VERIFY_HEAVY=1 oracle narrows G255 verification to late-route 0x159 batches
//       after attempt 10,000 while ordinary batches retain resident waves (force recompile v5).
// G556: DC2_G556_RTT181=1 gives Georama target 0x181 a seventh GPU-residency slot
//       (force recompile v1).
// G536: page-ownership-aware residency publication (default-ON, kill DC2_G536_NO_PAGEOWN=1) plus
//       the DC2_G536_MATCHK=1 probe that proved the invariant is violated at publication time.
//       g261Materialize must not overwrite pages the guest wrote since the residency anchored.
//       (force recompile v1)
