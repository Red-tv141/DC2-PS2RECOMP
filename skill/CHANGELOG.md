# Changelog — ps2-recomp-Agent-SKILL

Every skill change, why it was made, and the evidence behind it. Newest first.

---

## 2026-07-10 — G191/G192/G193 lessons: route-complete invariant repairs, flat-screen camera triage, trace-registration eviction, decoy file errors

Driven by DC2 G191 (perf-lever promotion), G192 (MAP-0 repro + fioOpen decoy refutation,
`plans/phase-G192-fix-log.md`) and G193 (town flat-blue root-cause+fix — first town render,
`plans/phase-G193-fix-log.md`).

- **`10-agent-guardrails.md` §1 (3 new taxonomy rows)** — (a) repairing a GLOBAL boot invariant
  on only the route that crashed (F50.4 covered the dungeon loop-init; the town loop-init has
  the byte-identical dispatch and stayed flat-blue until G193 — audit every consumer route);
  (b) diagnostic `registerFunction` evicting the default table's repair wrapper (last-wins —
  a G58 canary bypassed the g67 distance-cull repair, so traced runs rendered differently than
  the run under debug); (c) chasing missing-file open errors that are retail-normal (verify the
  path against the game's own archive index parsed from the ISO before treating a fopen miss
  as a load failure).
- **`15-vu1-gs-debugging.md` (new triage blockquote)** — flat single-colour screen: check the
  CAMERA before VU/GS. Null scene camera → zero view matrix → 100% frustum cull; discriminator
  is draw-chain canaries firing while the visual-emit stage gets zero dispatches ("culled",
  not "unreached"); then A/B the scene camera words (count/active/slots) against PCSX2.
- **`16-runtime-concurrency-threading.md` §8 (new rule bullet)** — enter/exit trace pairs LIE
  under back-edge-preemption yield: the wrapper's "exit" line fires on first yield of a long
  body; inner calls print after it. Don't infer program order from wrapper-log interleaving
  without checking `ctx->pc`.
- **`appendix-dc2-project.md`** — operating snapshot refreshed to post-G193 (perf levers
  default-ON at ~17 fps, rendering arc reopened, town residuals listed); `LoopNo=1` legend
  entry added (town/edit loop, `EditMapJump@0x1AF4C0` route, `map_0.gs/.png` reference pair);
  §6 gained the full scene-camera chain fact (vtable slot, Initialize/AssignCamera offsets,
  healthy-state word values for A/B).

## 2026-07-09 — G186/G187 lessons: yield-safety of overrides, recovery-context noise, dungeon-soak snapshot

Driven by DC2 G186 (root-cause+fix of the `0xe3dc70` "some levels cannot be loaded" class —
evidence `plans/phase-G186-fix-log.md`) and G187 (first dungeon-3D soak,
`plans/phase-G187-fix-log.md`).

- **`16-runtime-concurrency-threading.md` §8 (NEW section)** — back-edge preemption & the
  pc-mismatch unwind: overrides that call recompiled bodies as plain C++ calls are NOT
  yield-safe (post-call fixup runs on yield; register sentinels like `$ra=0` leak; stack-scan
  recovery restores PC but not `$sp` → guest stack-frame corruption presenting as
  nondeterministic "Function at address <garbage> not found" far from the fault). Fix
  patterns (preempt-suppression scope / resume-aware loop) + the full diagnosis recipe
  (RAM dump at first-bad-pc, dispatch-boundary word-watch ring, sp-balance trampoline,
  per-label sp checks in the generated file).
- **`10-agent-guardrails.md` §3.4 (three new red flags)** — sentinel/fixup overrides without
  yield-safety; treating dispatcher-recovery contexts as fault locations; trusting a
  "deterministic" crash address as a meaningful pointer (G185→G186: the stable address was
  `&MainScene` read out of a smashed saved-`$ra` slot).
- **`appendix-dc2-project.md`** — §3 snapshot → post-G187 (G186 fix default-ON with
  kill-switch, dungeon soak clean, dungeon EE-bound fps, G178 1/3-fallback finding, promotion
  now sign-off-only); §4 raw-TCP quirks (256-byte read cap, single-client wedge); §7 recomp/
  provenance rule (`config_auto_recomp_F56baseline.toml`, root config.toml lists INERT) +
  instrumentation-placement rule (recomp/*.cpp directly, src/runner/ is ps2EntryRunner) +
  G186 diagnostics reference.

---

## 2026-07-09 — G178 lessons: LLE GPU rasterizer doctrine (CPU-parity contract, content-hash residency, readback-to-VRAM)

Driven by the DC2 bespoke LLE GPU-raster arc phase 1 (evidence `plans/phase-G178-fix-log.md`,
`plans/gpu-raster-arc-plan.md`; title 6.8→10.3 fps GPU alone, 16.8-17.7 with frame pipelining =
the measured EE-bound ceiling; GPU golden frames INSIDE the CPU golden band).

- **`17-performance-optimization.md` §3.2 (NEW section)** — the seven contracts for moving a
  software rasterizer to the GPU: census-before-shader (real workloads collapse to a tiny state
  matrix); CPU-PARITY over spec-correctness (match the shipped sampler's affine `interp(s/q)`
  convention — a "correct" perspective shader renders 3D near-black while 2D stays perfect);
  all-or-nothing per flush (never mix backends); readback-to-VRAM keeps the ENTIRE existing
  verification harness valid; content-hash revalidation on top of write-gens (games re-upload
  identical texture bytes every frame — gen-only invalidation measured hits≈0); release-before-
  share GL threading; soak detectors need a same-length control arm.
- **`15-vu1-gs-debugging.md` §3 (two new traps)** — (1) STQ is Q-premultiplied and a runner's
  CPU sampler may interpolate it AFFINELY; any second implementation must match that convention
  (tell: menus right, world wrong). (2) Runtime-STUB GS writes bypassing the GIF FIFO are a race
  class under threaded/pipelined GS (from G177): fix by synthesizing A+D packets through the
  arbiter; never gate per-frame stub writes (serializes) or defer a subset of a register's
  writers (breaks program order).
- **`appendix-dc2-project.md` §3 snapshot refreshed (was stale at G145) + §6 entries for
  G176/G177/G178** — current default stack + opt-in lever table with fps; G176's
  "frame dumps bypass UploadFrame" trap; G177's refuted reroute designs; G178's full lever
  reference (env vars, residency metric = `texHashHits` not `texHits`, fb-alpha caveat,
  title-only scope).

---

## 2026-07-09 — G175 lesson: presentation = frame-boundary snapshot only (per-tick live-VRAM latch is a race)

Driven by the DC2 default-path "garbage/flickering screen transitions" fix (evidence
`plans/phase-G175-fix-log.md`; repro captures `captures/g175_*`).

- **`appendix-dc2-project.md` §6 (new entry)** — the host present thread must never latch live GS
  VRAM per tick: at low guest fps the present loop ticks many times per guest frame, so most
  snapshots are mid-frame. Double-buffered scenes hide it (mid-frame latch still reads a complete
  front buffer); RTT-composited-into-display transitions expose it (latches just-cleared/partial
  buffers → full/black nonzero ping-pong, the diagnostic signature). Latch only at the guest frame
  boundary (mgEndFrame), in every threading mode; present consumes the published snapshot. The bug
  was PRE-EXISTING across all modes and had been misfiled as an "MTGS-era present-latch residual" —
  the serial-path repro is what killed that framing. Removing the per-tick latch also bought ~+10%
  fps (present-thread whole-VRAM copies + state-mutex contention against the GS worker).

---

## 2026-07-07 — G141–G144 lessons: GS rasterizer parallelism (row → tile-binning) + build-wrapper traps

Driven by the DC2 performance arc (title render complete, remaining blocker was speed ~3 f/s;
evidence `plans/phase-G14{1,2,3,4}-fix-log.md`). Title fps ~3.1 → ~4.6 available via opt-in levers,
graphic bit-clean throughout.

- **`17-performance-optimization.md` §3.1 (NEW section) + hotspot row #6 + §4** — the biggest
  generalizable lesson: the GS software rasterizer is embarrassingly parallel at the PIXEL level and
  is the ONE sanctioned place to multi-thread for speed (it never touches the guest lock). Documents
  the tier ladder (eliminate/hoist per-triangle invariants first → then parallelize), the key
  **reframe** that intra-primitive *row* parallelism plateaus ~1.2× (small tris + per-primitive
  barrier = Amdahl) while frame-level *tile/band binning across primitives* scales ~3× better, and
  the full **bit-exact-by-construction correctness contract**: disjoint pixels per thread, preserved
  submission order per region, `thread_local` per-primitive scratch, COMPLETE draw-state snapshot,
  and — the expensive-to-learn one — **any VRAM write between capture and replay (texture upload /
  BITBLT / RTT) is a flush barrier** (deceptive symptom: primitives whose texture got re-uploaded
  mid-frame corrupt while earlier ones look fine). Plus thread-context sensitivity (a pool safe from
  active-rendering code crashes from the frame-end/present-adjacent path) and the animate-scene
  verification rule (gate on nonzero COUNT + visual A/B, not a byte hash). Corrected §4's blanket
  "no threading for speed" to carve out this exception.
- **`appendix-dc2-project.md` §3** — two build-wrapper traps that cost real time: (1) `build_rt.bat`/
  `build_runner.bat` redirect to `build_out.txt`/`runner_out.txt` and end in `echo BUILD_EXIT:…`, so
  the batch's own exit code is the echo's (**always 0**) — a link failure reports `BUILD_EXIT:1` in
  the LOG and **deletes the exe**; never trust `$LASTEXITCODE` from the wrapper. (2) cross-TU `extern`
  declared inside `dc2_game_override.cpp`'s anonymous namespace binds to an internal-linkage symbol
  and fails to link — declare at global scope above the namespace. Also fixed the stale golden number
  (`633662` pre-G138 era → `211646` natural render; count-gate not byte-hash) and added the G141–G144
  perf-lever registry (`DC2_G14*` env knobs).

## 2026-07-06 — G140 lessons: VU clip-route decode, stale band-aid sweep, uncapped code dumps

Driven by the DC2 G140 root-cause (missing title water = the VU1 polygon clipper force-broken
by a 70-phases-old band-aid; evidence `plans/phase-G140-fix-log.md`):

- **`15-vu1-gs-debugging.md` §2** — new worked decode: **the VU CLIP route** (pre-dispatcher on
  an EE "needs clipping" selector bit → CLIP packers → CLIPw/FCAND/FCOR trivial tests →
  Sutherland–Hodgman clipper with per-edge FCGET inside/outside tests → clipped polys emitted
  as TRIFANS from a fixed-address template, empty-flush + real fan kicks). Signatures:
  alternating empty/real trifan giftags in a `.gs` = clipper output; "missing screen-edge
  geometry with tri/tstrip faithful" = the clip route dying, not a separate object/water path.
- **`15-vu1-gs-debugging.md` §5** — two new doctrine bullets: (1) after a ROOT fix, sweep ALL
  older pc-scoped interpreter patches (grep-able band-aid registry with original hypothesis;
  kill when the hypothesis is invalidated); (2) never cap a microcode dump — disassemble the
  whole program once and index its XGKICKs/branch targets (the clip subsystem sat past a
  0x1c90 dump cap for 20+ phases).
- **`appendix-dc2-project.md`** — G140 entry: G64 retired (re-enable
  `DC2_G64_FORCE_ENABLE_FIX=1`), census lever `DC2_G140_CLIP`, title render complete, G141=perf.

---

## 2026-07-06 — G139 lesson: same-pair upper→lower VF hazard

Driven by the DC2 G139 root-cause (beam-shard spanning triangles; evidence
`plans/phase-G139-fix-log.md`):

- **`15-vu1-gs-debugging.md` §2** — new checklist row **"SAME-PAIR upper→lower VF hazard
  (immediate upper commit)"**, placed with the Q-latency/flag-pipeline family: hand-scheduled
  microcode stores a register in the same pair that recomputes it (store-then-clobber, producer
  exactly 4 pairs back); recognizable signature = periodic vertex subset with garbage positions
  but coherent ADC/fog (`.w` survives a `.xyz` dest mask) and coords that decode as raw float
  bits. Fix pattern: snapshot upper's VF dest, expose the old value to the lower op, overlay the
  upper's masked lanes after.
- **`appendix-dc2-project.md`** — G139 entry (kill `DC2_VU1_NO_PAIRHAZ`), band-aid retirement
  levers, new golden smoke value, G140 blocker.

---

## 2026-07-05 — G138 lessons: opcode-table hazard, flag pipelining, packet-level GS A/B

Driven by the DC2 G138 root-cause (closed a ~70-phase graphics investigation in one day once
the right method was applied; evidence `plans/phase-G138-fix-log.md`):

- **`15-vu1-gs-debugging.md` §2** — two new checklist rows, ordered FIRST:
  **wrong opcode dispatch table** (the runner had VU lower ops FMEQ(0x18)/FMAND(0x1A) swapped
  + FMOR parked on FCGET's 0x1C slot; every disassembly "proved" a gate impossible because the
  disassembler mirrored the runtime's wrong labels — shared-tool-bug hazard) and
  **MAC/STATUS flags read un-pipelined** (real VU1 flags visible ~4 instruction pairs after the
  FMAC; microcode is hand-scheduled for exactly that distance; off-by-one kills gates). Softened
  the old "a branch can be structurally never-taken — verify the operands" caveat, which had
  actively reinforced the wrong conclusion.
- **`15-...md` §2.1** — authoritative `_LOWER_OPCODE` flag/branch block table (0x10–0x1C),
  flag-consumer pipeline rule, and a worked decode of a real per-vertex draw gate (FMAND mask
  cascade: guard-plane SUB S-flags `0xD0`, OPMSUB winding `0x20`, `qw30` winding-flip bit from
  the setup determinant).
- **`15-...md` §4.0 (new)** — packet-level GS-stream A/B method: dump the runner's GIF stream
  into a PCSX2-shaped `.gs` container (all existing parsers run on both sides), synthetic A+D
  NOP markers for per-XGKICK packer-PC attribution, per-(TBP×prim×strip) ADC-pattern census
  (ALLDRAW/PRIMED/MIXED/ALLNODRAW), fog-byte packer fingerprinting, geometry-join verdict
  tables. Rationale: aggregate stats matched for 20+ phases while per-strip patterns diverged.
- **`10-agent-guardrails.md` §1** — two taxonomy rows: "trusting the runtime's own labels/tables
  as ground truth" (verify against an EXTERNAL oracle) and "accepting an aggregate-statistics
  match as convergence" (compare at the finest granularity the reference allows).
- **`07-ps2-code-patterns.md` §5 (new)** — VU1 display-list microprogram anatomy (setup pc 0x0
  with one-time winding-flip flag → trampoline 0x10 → per-prim-class packers; XYZF2 word3
  dual-use fog/ADC; +2048 ADC idiom; double-buffered XGKICK loops) as a decompile-recognition
  pattern; old §5 threading renumbered §6.
- **`appendix-dc2-project.md` §6** — G138 entries (opcode fix + flag pipeline kill-switches,
  `DC2_G138_GSDUMP` harness + tools, G100 forced-draw retirement, G139 residuals).

## 2026-07-02 — Coverage pass: audio, peripherals/saves, FMV, host-crash decoding, code examples

Gap analysis found the skill deep on graphics/hangs/codegen but thin on the other subsystems
every port hits. Added:

- **NEW `resources/18-audio-spu2-iop-debugging.md`** — audio path mental model (EE → SIF RPC →
  IOP driver → SPU2), symptom triage table (silence / music-only / SFX-only / crackle / pitch /
  stall-on-sound), VAG/ADPCM data verification, the ENDX/completion contract (top recurring root
  cause — ties into file 16 §6 stalls), 4-tier stub strategy ("honest silence" first), host
  output sanity. *Why:* skill previously taught the agent to silence audio problems, not solve
  them.
- **NEW `resources/19-memcard-pads-fileio.md`** — the three SIF-RPC peripheral subsystems:
  libmc async contract (McSync completion — the infinite "checking memory card" class), save
  host-mapping + atomic writes; libpad data contract (active-LOW buttons, mode state machine,
  byte-swap trap proven in DC2); cdvdman file I/O (LSN×2048 ISO mapping as the faithful backend,
  path normalization, streaming API honesty).
- **NEW `resources/20-fmv-ipu-cutscenes.md`** — FMV as the most common early-boot blocker:
  detection, skip-first tier strategy with the safe-skip contract (return code + post-FMV state +
  stream cleanup), 3-leg hang triage (IPU / disc stream / audio), post-skip verification.
- **NEW `10-agent-guardrails.md` §3.8** — host-side crash decoding: `out_XXXXXXXX.cpp` filename =
  guest function address, `fault_ptr − rdram_base` → guest address, stack-overflow ≈ dispatch
  loop, trace-tail ≠ callstack.
- **NEW code examples:** `examples/syscall-implementation-template.cpp` (3 syscall shapes incl.
  blocking-with-guest-lock-release), `examples/distinct-lane-vu-test-template.cpp`
  (characterization harness for the §2.1 defect catalog), `examples/ab-trace-logging-template.cpp`
  (side-B probe with greppable one-line format + uncapped counter + fall-through rules).
- **NEW `scripts/pr-adoption-log-template.md`** — the §2 surgical-apply protocol as a fillable
  log (divergence table, per-hunk decisions, A/B verification); linked from `10` §2.
- Wiring: SKILL.md router/§8/trigger-table/§9 rows; `db-ps2-index.md` lookup + inventory;
  cross-refs 16→18, 07§4→19; file ranges bumped 01–17 → 01–20.

## 2026-07-02 — Consistency & portability pass

- **NEW `resources/17-performance-optimization.md`** — correctness-first performance doctrine:
  measure-before-touching, the known hotspot classes of a recompiled PS2 runtime (logging,
  guest-memory macros, dispatch lookup, VU1 interpreter, GS rasterizer, lock contention), and the
  behavior-identical verification rule. Routed from SKILL.md §1/§8 and the guardrails flowchart.
  *Why:* real ports hit "correct but slow" (e.g. a 30 s headless window reaching only a few hundred
  frames — see `appendix-dc2-project.md` §5) and the skill had no dedicated playbook for it.
- **NEW SKILL.md §1.5 Tool Adapter** — maps the Windsurf/Antigravity tool names used throughout the
  resource files (`find_by_name`, `view_file`, `command_status`, `WaitMsBeforeAsync`,
  `send_command_input`) to Claude Code and Cursor equivalents. *Why:* the skill is used from
  multiple agent environments; agents outside Windsurf had to guess the mapping.
- **ISO extraction how-to** added to `11-operational-phases.md` Phase 0 step 10 (7-Zip and
  PowerShell `Mount-DiskImage` paths, `SYSTEM.CNF`/`BOOT2` parsing). *Why:* Phase 0 required
  extraction but no file said how.
- **`skip` semantics fixed** in `examples/toml-config-template.toml` — it claimed the PC "steps
  over" skipped functions; per `03-ps2recomp-pipeline.md` §2 the recompiler actually emits a
  `ps2_stubs::TODO_NAMED` placeholder that misbehaves if reached. Comment now matches the pipeline
  doc.
- **Stale file-range references fixed** in SKILL.md ("files 01–15", "resource files (01–11)" →
  01–17). Prohibition #11 temp-dir wording made environment-neutral (scratchpad/`%TEMP%`/`/tmp`).
- **Removed `resources/images/image-catalog.md`** — stale raw-extraction duplicate listing 104
  images including 24 deleted from disk; the curated `IMAGE_CATALOG.md` (80 images, matches disk)
  is the referenced catalog everywhere.
- **Italian strings translated to English** in `README.md` troubleshooting and
  `05-ghidra-ghydramcp-guide.md` user prompts.
- **`db-ps2-index.md`** — routing rows and File Inventory entries added for files 12–17 and the
  project appendix (previously stopped at 11).
- **NEW `scripts/project-appendix-template.md`** — skeleton for `appendix-<game>-project.md`
  (workspace map, static export, build & smoke, PCSX2 A/B, input infra, proven facts, regen rules).
  `appendix-dc2-project.md` is marked as the filled worked example. *Why:* the appendix pattern was
  proven by DC2 but had no template, so new ports had to reverse-engineer the structure.
- **`scripts/run_game_agent_template.bat` redirection bug fixed** — `start /B prog > log` attaches
  the redirect to `start`, not the child, leaving the log empty; now wrapped in `cmd /c "... > log"`.
- **Game-override file location corrected** — `04-runtime-syscalls-stubs.md` §5 told the agent to
  create the per-game override file inside `ps2xRuntime/src/runner/` (the FORBIDDEN generated dir);
  corrected to `ps2xRuntime/src/` with a "varies per layout, record in state file" note.
  `13-decisional-brain.md` §4 heading aligned to the same convention.
- **`scripts/project-state-template.md`** — new `## Performance Baseline` section (golden output
  metric, FPS, profiled hot spots) so perf work has a measured before/after home.
- **README** — stale "Scenario C" reference → "Quick Resume"; prerequisites renumbered (two "3."
  items); multi-environment note added.
- **SKILL.md description** — added VU1/GS debugging, hang triage, PCSX2 A/B, performance,
  SCUS/SCES, DebugServer, static-export trigger keywords for better activation.
- **This file created** — README linked to a `CHANGELOG.md` that did not exist.

## Earlier

Pre-changelog history: see git history of the repository and the per-phase fix logs of the
reference project (`appendix-dc2-project.md`, `plans/phase-*-fix-log.md` in the DC2 workspace),
which are the evidence base for the hard-won rules in files 10 and 13–16.
