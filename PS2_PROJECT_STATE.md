# PS2 Recomp Project State — Dark Cloud 2

> **GENERAL PROJECT INFORMATION ONLY** — operating rules, workspace/build facts, durable runtime architecture, and active environment flags. Specific phase investigation details, detailed benchmark tables, and superseded numbers belong in the appropriate fix logs and phase history.

---

## Where Everything Lives

| Need | File |
|---|---|
| **What to do NEXT** (short-term goals & active phase targets) | [plans/ROADMAP.MD](file:///d:/ps2r/dc2/plans/ROADMAP.MD) |
| **Rules, paths, build, runtime architecture** | **this file** |
| **How to drive a test** (graphics/audio routes & harness) | `skill/resources/appendix-dc2-test-routes.md` |
| **How to attribute a measured cost** (layer / worker / blocked half / surface / sub-noise lever) | `skill/resources/appendix-dc2-attribution-recipes.md` |
| **How to capture frames & gate a change on pixels** | `skill/resources/appendix-dc2-capture-and-gates.md` |
| **Full catalogue of `DC2_*` env flags** | [plans/env-flags.md](file:///d:/ps2r/dc2/plans/env-flags.md) |
| **Performance profiler & telemetry architecture** | [PERFORMANCE_PROFILER_REPORT.md](file:///d:/ps2r/dc2/PERFORMANCE_PROFILER_REPORT.md) |
| **Debug logging & crash reporting architecture** | [DEBUG_AND_CRASH_REPORTING.md](file:///d:/ps2r/dc2/DEBUG_AND_CRASH_REPORTING.md) |
| **HD texture replacement & dumping architecture** | [TEXTURE_REPLACEMENT_REPORT.md](file:///d:/ps2r/dc2/TEXTURE_REPLACEMENT_REPORT.md) |
| **Why one phase did what it did** | `plans/phase-<ID>-fix-log.md` |
| **Closed phase history archive, NO-GO table, superseded numbers** | [plans/phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md) — *grep it; never read top-to-bottom* |
| **Runtime architecture, ABIs, double/float math semantics** | `skill/resources/appendix-dc2-runtime-architecture.md` |
| **Concrete paths, static export, harness wiring, PCSX2 A/B** | `skill/resources/appendix-dc2-project.md` |
| **Already-diagnosed graphics defects** | `skill/resources/appendix-dc2-graphics-facts.md` |
| **Project method & agent skill** | `skill/SKILL.md` and `skill/resources/` |
| **Native-renderer design** | `plans/arc-native-renderer.md` · `plans/arc-total-closure.md` |

---

## Core Operating Rules

- **NO PER-SCREEN FIXES (hard rule).** Repair the root state/init/data path once, where the game itself sets it. Never patch a symptom by writing game state per-frame or per-draw scoped to one screen.
- **Never clean the build.** `cmake --build <build_dir>` only; no clean targets, no build-dir deletion (full rebuild = 30+ hours).
- **Never modify or create files in `runner/`**, and never modify standard `.h` headers (only `.inc` and `.cpp` files).
- **Never run destructive git commands.** Preserve unrelated worktree changes.
- **Build only inside an x64 `vcvars64` environment.**
- **Every performance run must set `DC2_PATCH_60FPS=1`.** Otherwise 33.33 ms is only the 30 FPS cap.
- **Three-Thread Pole Model**: $\text{frame} \approx \max(\text{VU1 busy}, \text{GS own}, \text{EE cpu})$, where $\text{GS own} = \text{gsWorkerMs/f} - \text{gsStallMs/f}$ and $\text{EE cpu} = \text{[G182:ee] cpuMs/f}$ (**not** `busyMs/f`). Re-derive the EE term per route (`skill/resources/23-renderer-qualification-and-pole-injection.md`).
- **Derivative Injection Rule**: Census levels alone do not prove conversion. Always probe with `DC2_G431_GS_SLOW_US` / `DC2_G503_EE_SLOW_US` to measure injection sensitivity ($\Delta \text{frame} / \Delta \text{injected}$) before choosing a lever. A level pole (e.g. `dungeon1` GS 0.69 / EE 0.14) is a closure for single-thread levers.
- **Delete Work, Not Waiting (G493/G622)**: Removing a synchronization wait on a secondary thread does not recover frame time unless the underlying work shrinks. **⭐ G637 EXCEPTION — a wait ON THE POLE ITSELF is work.** When the blocking thread is the pole and `gsStallMs/f = 0.00`, the wait is inside `GS own` and deleting it converts at the route's injection sensitivity. Always ask WHICH thread waits before applying this rule.
- **A PC Sampler Cannot Name a Function in a Symbol-Less Module (G639)**: `[G446:eeprof]` read `getenv 2.07%` of the EE thread; `DC2_G446_ENVCENSUS=1` then printed **nothing** over a full 190 s run (< 27 hooked env reads/frame). The samples are in `ucrtbase.dll`, which ships no PDB, so `SymFromAddr` resolves to the nearest **exported** symbol. **Read such a row as "time in this MODULE" and get the function from a counter you own.** This refuted a roadmap candidate three phases old.
- **Grep a Helper's DEFINITION, Not Its Calls (G639)**: `envFlagEnabled` is **five separate anonymous-namespace copies** (GS stub, GPU bridge, rasterizer, IOP, memory) plus `dc2_env_flag_enabled` in the override TU. A memo wrapping one cannot move the others' traffic.
- **A Reference Arm Must Differ ONLY in the Thing Under Test (G639)**: G624's proposed `DC2_G261_NO_WAVE=1` reference sits **1.53** from both candidates, which differ from each other by **0.15** — it cannot adjudicate. Measure the reference's own divergence FIRST and require it to be smaller than the effect. Also **size a slow arm's budget from its own measured rate** — that arm is 3.4× slower and its first capture dumped **0 frames**.
- **Read `common window:` and per-arm `pres` Before Quoting an A B B A (G639)**: one arm exiting early silently shrinks the gate (2,040 → 360 presents) rather than failing it.
- **Identical Register COUNTS Are Not Identical Register VALUES (G638)**: `[G147:gif]`'s `tags` / `packedRegs` / `imageKB` were byte-identical across every window of `dragon`'s static tail, and a payload memo built on that premise measured **0 hits in 1,200 batches** — the geometry is re-emitted with different coordinates while the screen holds still. **Gate a caching/memo lever on a census of the VALUES it memoizes, never on the stability of their shape.**
- **A GS Pole Is Not Automatically Rasterization (G638)**: on `dragon`, 44% of a 33.8 ms/f GS pole is one target's shadow-compute preparation and band round trip (`[G638:prep] g570gpubatch` 5.263 + `g570prep` 4.735 ms/f) while the CPU band replay every phase since G529 has optimised is **1.611**. Split the drain (`tools/g638_drain.py`) and the dispatch window (`DC2_G638_PREP=1`) before choosing a lever.
- **Bracket the Barrier, Don't Infer It (G638)**: ⛔ `[G529:disp] wall` is **NOT** `GSRowPool::run` despite its header comment — `g529T` spans the whole `if (y1 >= y0)` body, a **7× overstatement** on `dragon` (9,957 vs 134 µs/dispatch). G637 §1's `rt = wall − lane0 = "GS-thread SLEEP"` inherits it. Quote `[G638:pool]` (`DC2_G638_POOL=1`), which brackets `run()` itself and splits all seven call sites by return address.
- **Rank a Fork/Join Census by WAIT, Not WALL (G638)**: `wall` contains the caller's own band — real work no scheduling lever can delete. The recoverable term is `wall − lane`.
- **Over-Decomposition Subsumes a Static Work Estimator (G637)**: For a fork/join over a partitionable range, cut it into more chunks than workers and let every participant — **including the caller** — claim dynamically. Do NOT also build a work-estimating partition: it is redundant (the scheduler balances) and harmful (its fences cluster where the work is dense, so more items straddle a boundary and repeat their per-item setup). Measured: +14% aggregate CPU for a `max` the scheduler did not need. (`skill/resources/26-cooperative-replay-scheduling.md`)
- **A Fork/Join Dispatch Is Either Tail-Bound or Aggregate-Bound (G637)**: Split its wall into `sum` (aggregate participant time) and `max` (worst chunk) before choosing a lever. `max ≈ wall` ⇒ imbalance, fix the schedule. `sum/workers > max` ⇒ aggregate-bound, and no scheduling change can help — only deleting work can.
- **Synthetic Address Space Isolation (G625)**: A synthetic "other processor" address space must NEVER be carved out of guest RAM (`kIopHeapBase`). Dedicated host backing stores (`g_g625IopShadow`) are mandatory (`skill/resources/21-synthetic-address-spaces-and-iop-heap.md`).
- **Coordinate Transforms & Quantization (G624)**: Flipping a sampling coordinate inverts quantization/rounding tie-breaks. Evaluate rules in original space and map results back (`uUvFlipV`, `skill/resources/22-gs-uv-transforms-and-quantization.md`).
- **PSMT8-in-CT32 Sub-Word Boundary Updates (G627)**: Sub-word writes cannot be reconstructed in guest VRAM, but are expressible directly on resident GPU FBOs using `glColorMask` + per-texel fragment lane discard (`DC2_G627_LANEMIR=1`).
- **Dead Stage Elision & Proofs (G628)**: When state makes a stage dead (e.g. `ZTST=ALWAYS` + `ZMSK=1`), omit it entirely in lowered templates and pass null buffers. Admission must prove dead-stage conditions fail closed.
- **A Resident GPU Authority Must Be a Superset of Every Writer (G636)**: Enumerate all memory writers first (including native GL FBO residency `m_fbos[fbp]` and CPU private depth mirror `g403DisplayZBuf()`). Missing any writer produces silent corruption across authority boundaries.
- **A Second Authority with Unmergeable Conflicts Must Be Deleted (G636)**: When both authorities perform concurrent writes, detection alone does not solve the merge; the secondary authority must be retired and refused back to the primary.
- **A Deterministic Pixel Window Is Deterministic AT FIXED FRAME RATE ONLY (G637)**: `ridepod`'s opening scores a 0.00 floor between two same-speed runs, but sprites that advance on **host presents** (the dialogue cursor, particles) land on different animation sub-phases when an arm's speed changes, and the ±2-key temporal minimum cannot collapse a sub-key offset. **A speed-changing candidate must be gated against a speed-perturbed same-binary control (`DC2_G431_GS_SLOW_US`)** — otherwise its own payoff convicts it. Every pre-G637 prototype was SLOWER than control, which is why this never surfaced.
- **Exactness Oracles vs Mandatory Pixel Gates (G635)**: Exactness oracles test arithmetic on one path; they cannot see path selection, source staleness, or execution ordering. For any path-selection, source-authority, or residency change, **pixels against a same-binary control are the only admissible gate** (`tools/g635_bisect.py`, `tools/g635_look.py`, `ridepod` opening sf 768..1392 floor 0.00).
- **Flag Exclusivity (G636)**: `DC2_FRAME_DUMP=1` (host tick) and `DC2_G598_DUMP_SF` (script clock) are **mutually exclusive**. Never arm both.
- **Diagnostic Hygiene in Hot Loops**: Cache all environment flag reads at namespace scope. Function-local `static const bool` is an MSVC per-access TLS guard (`_Init_global_epoch`).
- **Rollback Arm Fidelity**: A rollback arm must restore the original construct verbatim, not an older or alternative mechanism.
- **Script Clock Keying**: Key transient and soak captures on script clock (`scriptFrame`, `[G154:perf] frame=`), never host-present tick `n`.
- **Subsystem Wall Ceiling Check**: Total subsystem wall time is the hard ceiling on all optimizations within it.
- **Unconditional A/B Sampling**: Sample A/B arms on paths that 100% of the target population executes, not inside conditional branches.
- **Valid Gate Evaluation**: Treat `[G419:ab]` runs with $w < 10$ as unmeasured; never draw conclusions from early in-progress runs inside entry spikes.
- **Renderer Promotion Requirements**: Paired within-process payoff (`DC2_G419_AB`), exactness oracle verification (`bad=0`), rollback flag registration, and direct normal-output review on the relevant graphics route.
- **Harness Maintenance**: Add every new `DC2_*` flag to `tools/run_g477_perf.ps1`'s `$clear` array in the same edit that creates it.
- **Fix Log Requirement**: Write `plans/phase-<ID>-fix-log.md` before ending each executable phase.
- **Rule 49**: **ONLY GEMINI IS ALLOWED TO UPDATE `plans/phase-history.md`.** No other AI assistant or automated agent may modify or edit `plans/phase-history.md`.

---

## Standard Phase Checklist

1. Load `skill/SKILL.md`, this file, `plans/ROADMAP.MD`, and `PS2Recomp/AGENTS.md`.
2. Attribute the current resource pole before choosing a lever ($\text{frame} \approx \max(\text{VU1 busy}, \text{GS own}, \text{EE cpu})$).
3. Keep one architectural variable per timing arm and repair parity failures in the same phase before promotion.
4. Build Release targets incrementally (`/m:1`); never clean.
5. Run the exact oracle, paired timing, and relevant graphics test routes.
5b. ⛔⛔⛔ **MANDATORY PIXEL GATE for ANY change that alters WHICH path serves a draw** (a new view, a new admission, a residency/coherence change). The gate is: `tools/g635_bisect.py <ctlA> <ctlB> <arm>… --lo 768 --hi 1400` on `ridepod`'s deterministic opening with a second control `ctlB` as noise floor, plus `tools/g635_look.py <ctl> <arm> <worstKey>` visual PNG inspection.
6. Update roadmap, env-flags, fix log, and phase history upon phase completion.

---

## Game & Workspace Overview

- **Title**: Dark Cloud 2 (NTSC-U), Main ELF `SCUS_972.13`.
- **Workspace Root**: `D:/ps2r/dc2`
- **Game Data**: `D:/ps2r/dc2/Dark Cloud 2 (USA) (v2.00).iso` or extracted DATA behind synthetic sector mount (`ps2_iso_mount.cpp`).
- **Live Repository**: `D:/ps2r/dc2/PS2Recomp`
- **Generated Output**: `D:/ps2r/dc2/recomp`
- **Build Directory**: `D:/ps2r/dc2/build64` (Visual Studio generator / MSBuild)
- **Runtime Overrides**: `PS2Recomp/ps2xRuntime/src/dc2_game_override.cpp` and split runtime parts (`src/lib/ps2_runtime_parts/`, `src/lib/ps2_gs_gpu_parts/`).
- **Kernel Syscalls / Stubs**: `PS2Recomp/ps2xRuntime/src/lib/Kernel/{Syscalls,Stubs}/*.cpp` (`.inl` files are dead duplicates).
- **Static Analysis Export**: `ref/functions/` and `ref/index/`.

---

## Build & Smoke Commands

Run through `vcvars64.bat`:

```powershell
# Build ps2_runtime library:
cmake --build D:\ps2r\dc2\build64 --config Release --target ps2_runtime -- /m:1

# Build executable runner:
cmake --build D:\ps2r\dc2\build64 --config Release --target dc2_runner -- /m:1 /p:BuildProjectReferences=false

# If recomp/ was edited, build game target first:
cmake --build D:\ps2r\dc2\build64 --config Release --target dc2_game -- /m:1
```

- **Runner Command**: `D:/ps2r/dc2/build64/Release/dc2_runner.exe D:/ps2r/dc2/SCUS_972.13`
- **Smoke Gate**: Assert route arrival by game state, review full-frame distribution. Do not gate on title host tick.

---

## Active Session-Critical Flags

| Flag | Purpose |
|---|---|
| `DC2_DEBUG_MENU=1` + `DC2_PAD_INPUT=...` + `DC2_NO_XINPUT=1` | Route automation & deterministic pad input |
| `DC2_PATCH_60FPS=1` | Mandatory performance uncapper (`0x00376C50 = 1`) |
| `DC2_NO_PRESENT_SYNC=1` | Rollback for G616 RTSS presentation synchronization (restores free-running 60 Hz swaps) |
| `DC2_LOG_LEVEL=<lvl>` | Global logging threshold (`TRACE`, `DEBUG`, `INFO` [default], `WARN`, `ERROR`, `FATAL`) |
| `DC2_PROFILE=1` | Master toggle for unified performance profiler + telemetry |
| `DC2_PROFILE_OVERLAY=1` | Real-time on-screen HUD profiler overlay (toggleable live via F3) |
| `DC2_G419_AB=<lever>` | Randomized within-process A/B timing harness |
| `DC2_G26X_NO_NATIVE=1` | Master rollback for native-renderer stack |
| `DC2_G605_NO_TRI_SPAN=1` | Rollback for G605 exact accelerated replayed-triangle span kernel |
| `DC2_G608_NO_TRI_FST=1` | Rollback for G608 `fst=1` sampler leaf of that kernel |
| `DC2_G609_NO_TRI_SCAN=1` / `_NO_SAMP_INLINE=1` | Rollbacks for G609 outlined tight triangle scanline & inlined sampler |
| `DC2_G610_NO_JIT=1` / `DC2_G612_NO_REGION=1` | Rollbacks for G610/G612 native VU1 JIT and REGION backends |
| `DC2_G614_NO_SPRITE_FAST=1` | Rollback for G614 inlined replayed-sprite leaves & column slide |
| `DC2_G615_NO_PRIVZ_SPAN=1` | Rollback for G615 private-Z display-sprite span admission |
| `DC2_G617_NO_BLEND_FILL=1` / `_NO_T4HH=1` | Rollbacks for G617 untextured blended fill & PSMT4HH tap |
| `DC2_G618_NO_NEGCACHE=1` / `_NO_FAST_T8MAP=1` | Rollbacks for G618 PSMT8 refusal ring & lowered map builder |
| `DC2_G619_LIVE_ENVREAD=1` / `_NO_DEPTHSLOTS=1` | Rollbacks for G619 EE env-flag cache & guest execution depth slots |
| `DC2_G620_NO_ZCLEARSKIP=1` / `_NO_PXPOOL=1` | Rollbacks for G620 private-Z dirty bit skip & decoded-texel pool |
| `DC2_G620_LIVE_TEXREPL=1` / `_NO_FASTROW=1` | Rollbacks for G620 HD-replacement probe early-out & fast row reader |
| `DC2_G621_NO_WRSPAN=1` / `_WRSPAN_VERIFY=1` | Rollback & oracle for G621 producer-scoped colour readback window |
| `DC2_G622_NO_TEXVAR=1` / `_TEXVAR_VERIFY=1` | Rollback & oracle for G622 content-addressed texture variants |
| `DC2_G623_NO_PRODSRC=1` / `_VERIFY=1` | Rollback & oracle for G623 producer-surface direct consumption (promoted by G624) |
| `DC2_G624_NO_UVFLIP=1` / `_UVFLIP_STAT=1` | Rollback & census for G624 flip-aware G406/G407 UV rounding |
| `DC2_G625_LEGACY_IOPHEAP=1` | Rollback for G625 host shadow IOP heap (restores EE RAM aliasing) |
| `DC2_G626_LIVE_PIXFLAG=1` | Rollback for G626 per-pixel diagnostic predicate cache (measured NULL) |
| `DC2_G627_NO_LANEMIR=1` / `_VERIFY=1` | Rollback & oracle for G627 PSMT8-in-CT32 local transfer lane mirror |
| `DC2_G628_NO_TRI_UNTEX=1` / `_STAT=1` | Rollback & census for G628 untextured (`tme=0`) triangle span-kernel admission |
| `DC2_G629_GPU=1` / `_NO_GPU=1` / `_VERIFY=1` | G629 exact GPU compute band replay (**default-OFF, measured +1.13 ms/f slower**). 100% bit-exact since G636 |
| `DC2_G630_RESIDENT=1` / `_NO_RESIDENT=1` / `_STAT=1` | G630 persistent raw-VRAM GPU authority prototype (**default-OFF, measured +1.38 ms/f slower**). 100% bit-exact since G636 |
| `DC2_G633_PLANS=1` / `_NO_PLANS=1` | G633 event-driven compiled residency & invalidation plans inside G630 prototype (**default-OFF**) |
| `DC2_G634_RAWVIEW=1` / `_NO_RAWVIEW=1` | G634 raw-VRAM CT32/CT24 texture view addressed by TEX0 (**default-OFF**) |
| `DC2_G634_RAWUP=1` / `_NO_RAWUP=1` | G634b format-agnostic Host→Local IMAGE ingestion (**default-OFF**) |
| `DC2_G636_NO_ARBITRATE=1` / `_NO_BANDPATCH=1` / `_NO_FBSEED=1` / `_NO_MATPUB=1` / `_NO_UPSYNCFIX=1` | G636 rollbacks for authority-arbitration repairs inside prototype |
| `DC2_G636_LINVIEW=1` / `_RESIDENT_Z=1` | ⛔⛔ Re-arms of the two RETIRED mechanisms (resident linear FBO mirror and persistent GPU depth mirror) |
| `DC2_G637_NO_COOP=1` | 🛡️ Rollback for G637 cooperative over-decomposed work-stealing band replay — ⭐ **PROMOTED DEFAULT-ON by G638** (A B B A on `ridepod`, both order blocks agree: **−1.404 ms/f pooled, −5.95%**; GS own 21.08 → 19.74). `DC2_G637_COOP=1` remains accepted as the bring-up spelling |
| `DC2_G637_OVER=<n>` / `_MINCHUNK=<n>` | G637 chunk sizing (defaults 4 chunks per lane, 2 rows minimum) |
| `DC2_G637_BALANCE=1` / `_INDEX=1` | ⛔ G637's two REFUTED / measured-NULL slices, retained as their own probes |
| `DC2_G637_BAL=1` / `_VERIFY=1` | G637 dispatch balance census (`[G637:bal]`) & partition oracle |
| `DC2_G638_POOL=1` / `DC2_G638_PREP=1` | 📊 G638 censuses — the legacy `GSRowPool::run` barrier **by call site** (`[G638:pool]`, resolve with `tools/g638_pool.py`) and the CPU-fallback dispatch window **by phase** (`[G638:prep]`). Never valid in a timing run |
| `DC2_G638_PAYCACHE=1` / `_NO_PAYCACHE=1` / `_VERIFY=1` / `_STAT=1` | ⛔ G638 0x139 shadow-payload memo — **REFUTED (0 hits in 1,200 batches)**, retained as its own probe plus `[G638:keydiff]` |
| `DC2_G639_NO_PAYPAR=1` | 🛡️ Rollback for the ⭐ **G639 PARALLEL 0x139 GPU payload build (default-ON)** — oracle `bad=0` on 2,400 batches, A B B A on `dragon` **−2.882 ms/f (−8.24%)**. `_VERIFY=1` / `_STAT=1` are its oracle and census |
| `DC2_G639_ENVMEMO=1` / `_NO_ENVMEMO=1` | ⛔ G639 `dc2_env_flag_enabled` memo — **measured NULL**, retained as its own probe |
| `DC2_TEXTURE_REPLACEMENTS=1` / `_DUMP=1` | Enable HD texture replacement / dumping (`Mods/HD Texture`) |

*Full flag catalogue: [plans/env-flags.md](file:///d:/ps2r/dc2/plans/env-flags.md).*

---

## Durable Architecture Overview

### 1. The 5 GS Worker Layers (`processGIFPacket`)
1. **Layer 1 — Batch Execution & CPU Band Replay**: Replays rasterization for display and discovered targets where GPU contracts fallback (accelerated via G605/G608/G609 triangle span kernels, G530/G614/G615/G617 sprite and fill kernels). **Dispatch (G637, `DC2_G637_COOP=1`, default-OFF)**: the row range is cut into ~4× more contiguous chunks than lanes and every participant — *including the GS thread itself* — claims chunks from one lock-free generation-tagged cursor until the batch is exhausted (`GSRowPool::runCoop`, `g637_coop_replay.inc`). Exact by the same argument as the legacy equal-band split: contiguous disjoint row ranges, whole entry list replayed in submission order clipped to each. The legacy `GSRowPool::run` barrier remains and is what `DC2_G637_NO_COOP=1` restores verbatim.
2. **Layer 2 — `drawPrimitive` & Tile-Bin Capture**: Fixed front-end tile binning and winner search (`g310EnsureCurrent`).
3. **Layer 3 — Register-Triggered TRXDIR Transfers**: Table-driven same-format local-to-local VRAM transfers (`g596copy`).
4. **Layer 4 — GIF Tag Walk & Register Replay**: Vertex assembly and non-vertex register state machine with outlined prologues (`g602_gif_cold_outline.inc`).
5. **Layer 5 — Host→Local Image Writer**: VRAM texture and image data streaming.

### 2. The 5-Level VU1 Execution Stack
1. **Level 1 — Native Compiled Regions (`vu1_g612_native_region.inc`, G612)**: Compiles microprogram CFGs into single x86-64 executable bodies.
2. **Level 2 — Native Compiled Blocks (`vu1_g610_native_jit.inc`, G610/G611)**: Straight-line x86-64 machine code emission for admitted basic blocks.
3. **Level 3 — Hand-Written Fragment Handlers (G533/G547–G551)**: Specialized exact-source handlers for recurring entry points.
4. **Level 4 — Generic Block Executor (`g490BlockBody`, G490)**: Interprets runs of eligible pairs.
5. **Level 5 — Interpreter Pair Loop (`VU1Interpreter::run()`)**: Fallback generic interpreter loop for uncompiled / hazard pairs.

### 3. GPU Band Replay & Persistent GS Domain Prototype Status (G629–G636)
- **Status: CLOSED NEGATIVE ON PERFORMANCE (DEFAULT-OFF).**
- G636 resolved all visual defects across the prototype family (now 100% bit-exact `0.00/0.00/0.00` on 53 keys of `ridepod` opening sf 768..1392). However, end-to-end performance is +1.13 to +1.51 ms/f slower than control because refusing private depth leaves 57% of batches on CPU replay, so addressable wait savings (~1.2 ms/f) sit below added resident costs (~1.8 ms/f). Kept default-OFF. Full autopsy: `plans/phase-history.md` & `phase-G636-fix-log.md`.

### 4. GPU-VU Architecture Status
- **Closed Across All Shapes**: Scalar GPU-VU interpreter (G606), translated GPU-VU compute (G607), and program-point batching (G608) are all closed due to synchronization, kick-chain dispatch floor, and inter-kick RAW dependencies. All VU1 execution is retained on native CPU compilation backend.

### 5. Unified Performance Profiler & Telemetry (`dc2_profiler.inc`)
- **Multi-Threaded Model**: Main Thread (serial wall-clock: Upload, Present, WaitFrame, Input, DrawHUD); Concurrent Worker Threads (cumulative CPU: `EE_CPU`, `VU1`, `GS`).
- **Overhead**: Subsystem timing (`DC2_PROFILE=1`) < 0.1 ms/frame; function profiling (`DC2_PROFILE_FUNCTIONS=1`) ~0.40 ms/frame. Live overlay via F3 toggle. Guide: [`PERFORMANCE_PROFILER_REPORT.md`](file:///d:/ps2r/dc2/PERFORMANCE_PROFILER_REPORT.md).

### 6. Debug Logging & Crash Reporting (`dc2_logger.inc` & `dc2_crash_reporter.inc`)
- **Structured Logging**: 6 log levels across 18 subsystem categories, dual sinks (ANSI console + rotating `logs/latest.log`), zero-allocation in-memory crash ring buffer.
- **Crash Reporting**: Windows SEH filter, guest function symbol resolver (`DAC.csv`, 7,810 functions), register snapshots, MiniDump (`crashes/`). Guide: [`DEBUG_AND_CRASH_REPORTING.md`](file:///d:/ps2r/dc2/DEBUG_AND_CRASH_REPORTING.md).

### 7. HD Texture Replacement & Dumping Prototype (`ps2_texture_replacements.inc`)
- **Status**: Supports 6,577 PNG pack (`Mods/HD Texture`), scanning at startup in ~37ms. Guide: [`TEXTURE_REPLACEMENT_REPORT.md`](file:///d:/ps2r/dc2/TEXTURE_REPLACEMENT_REPORT.md).

---

## Known Issues & Prototype Status

- **Correctness Status**: ✅ **NO KNOWN OPEN CORRECTNESS DEFECT** across the shipped default path and prototype arms (all 3 defects eliminated and bit-exact since G636).
- **Performance Targets & Active Qualification**:
  - **Objectives**: 30 FPS floor (no gameplay frame over 33.33 ms); long-term 60 FPS (16.67 ms).
  - ⭐⭐⭐ **Worst-Case GS Route (G638, re-taken G639)**: `dragon` static tail — after G637+G639 promotion frame **≈31.8–32.4 ms**, GS own **≈29.7–30.4**, VU1 ≈23.4–23.9, EE cpu ≈14.5–15.1. **Sole GS pole, headroom ≈+6.4 ms/f.** (Pre-promotion it was 34.4–35.8 / 33.2–33.8 / +9.2.) ⛔ Its pole is **not** rasterization: 44% is the G570 0x139 shadow-compute preparation + band round trip, and the whole CPU band replay is 1.6 ms/f. See `plans/phase-G638-fix-log.md` §2 and `appendix-dc2-test-routes.md` §1.3p.
  - **Band-Replay Gate Route**: `ridepod` — post-G637 baseline frame **22.21 ms**, GS own **19.74**, VU1 17.65, EE cpu 14.13, headroom +2.1. ⭐ **Re-derived injection sensitivity (G638): GS 0.971 / VU1 −0.212** — still a **SOLE GS POLE**; VU1 has real slack even at 19 ms/f. ⛔ `dragon` **cannot** gate a band-replay lever — it is 1.6 ms/f there and G637's A B B A reads NULL (blocks −0.197 / +0.026). **Gate a lever where its MECHANISM dominates, not where the POLE is largest.**
  - **Active EE Route**: `s05` (frame 16.5–17.2 ms, EE cpu **14.6–15.1 ms/f** — EE poled, −2.5 ms/f needed for 60 FPS gate).
- **High-Resolution Prototype Issues**:
  - **FOV Stretch**: Setting `DC2_RESOLUTION_WIDTH` stretches 4:3 image rather than expanding FOV.
  - **Height Multiples**: `DC2_RESOLUTION_HEIGHT` requires integer multiples of 448 (e.g. 896).
  - **Culling Glitches**: Textures/UI occasionally vanish at high resolution due to guest bounding-box culling against unscaled native coordinates.
  - **Debug Mode Freeze**: Returning to debug menu from within a level via Start+Select freezes frame display.
