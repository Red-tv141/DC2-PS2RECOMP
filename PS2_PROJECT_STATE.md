# PS2 Recomp Project State — Dark Cloud 2

> **GENERAL PROJECT INFORMATION ONLY** — operating rules, workspace/build facts, durable runtime architecture, active session flags, and routing pointers. Specific phase investigation details belong in the matching fix logs; historical archives and solved issues belong in `plans/phase-history.md`.

---

## Where Everything Lives

| Need | File / Location |
|---|---|
| **What to do NEXT** (short-term goals & active phase targets) | [plans/ROADMAP.MD](file:///d:/ps2r/dc2/plans/ROADMAP.MD) |
| **Rules, paths, build, runtime architecture** | **this file** |
| **Current optimization-phase evidence** | [plans/phase-G654-fix-log.md](file:///d:/ps2r/dc2/plans/phase-G654-fix-log.md) (24-priority sweep) |
| **Previous shipped baseline evidence** | [plans/phase-G653-fix-log.md](file:///d:/ps2r/dc2/plans/phase-G653-fix-log.md) · [plans/phase-G652-fix-log.md](file:///d:/ps2r/dc2/plans/phase-G652-fix-log.md) · [plans/phase-G651-fix-log.md](file:///d:/ps2r/dc2/plans/phase-G651-fix-log.md) |
| **Closed phase history archive, NO-GO table, superseded numbers** | [plans/phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md) — *grep it; Gemini-exclusive under Rule 49* |
| **Full catalogue of `DC2_*` env flags** | [plans/env-flags.md](file:///d:/ps2r/dc2/plans/env-flags.md) |
| **How to drive a test** (graphics/audio routes & harness) | `skill/resources/appendix-dc2-test-routes.md` |
| **How to attribute a measured cost** (layer / worker / blocked half / surface) | `skill/resources/appendix-dc2-attribution-recipes.md` |
| **How to capture frames & gate a change on pixels** | `skill/resources/appendix-dc2-capture-and-gates.md` |
| **Performance profiler & telemetry architecture** | [PERFORMANCE_PROFILER_REPORT.md](file:///d:/ps2r/dc2/PERFORMANCE_PROFILER_REPORT.md) |
| **Debug logging & crash reporting architecture** | [DEBUG_AND_CRASH_REPORTING.md](file:///d:/ps2r/dc2/DEBUG_AND_CRASH_REPORTING.md) |
| **HD texture replacement & dumping architecture** | [TEXTURE_REPLACEMENT_REPORT.md](file:///d:/ps2r/dc2/TEXTURE_REPLACEMENT_REPORT.md) |
| **Runtime architecture, ABIs, double/float math semantics** | `skill/resources/appendix-dc2-runtime-architecture.md` |
| **Concrete paths, static export, harness wiring, PCSX2 A/B** | `skill/resources/appendix-dc2-project.md` |
| **Already-diagnosed graphics defects** | `skill/resources/appendix-dc2-graphics-facts.md` |
| **Project method & agent skills** | `skill/SKILL.md` and `skill/resources/` |

---

## Core Operating Rules & Architectural Laws

1. **NO PER-SCREEN FIXES (hard rule)**: Repair the root state/init/data path once, where the game itself sets it. Never patch a symptom by writing game state per-frame or per-draw scoped to one screen.
2. **Never clean the build**: Incremental builds only (`cmake --build <build_dir> --config Release --target <target> -- /m:1`). Never use `--clean-first` or delete build directories (full rebuild = 30+ hours).
3. **Untouchable code**: Never modify or create files in `runner/`, and never modify standard `.h` headers (use `.inc` and `.cpp` files to modularize logic).
4. **Git safety**: Never run destructive git commands (`checkout`, `clean`, `reset`, `stash`, `pull`).
5. **Toolchain environment**: Build only inside an x64 `vcvars64` environment.
6. **Performance uncapping**: Every performance run must set `DC2_PATCH_60FPS=1` (otherwise 33.33 ms is only the 30 FPS cap).
7. **Three-Thread Pole Model**: $\text{frame} \approx \max(\text{VU1 busy}, \text{GS own}, \text{EE cpu})$, where $\text{GS own} = \text{gsWorkerMs/f} - \text{gsStallMs/f}$ and $\text{EE cpu} = \text{[G182:ee] cpuMs/f}$ (**not** `busyMs/f`). Re-derive the EE term per route (`skill/resources/23-renderer-qualification-and-pole-injection.md`).
7b. **⛔⛔ The Vsync-Spin Absorber (G651)**: on a route **AT** the emulated 60 FPS cap, `[G182:ee] cpuMs/f` is **NOT** a work metric and is **inadmissible as an EE gate** — the guest busy-polls the emulated vblank counter (`WaitVSync__Fii` + `mgGetVSyncCount` = **17.4% of `dungeon6`'s EE thread**, 7.3% of `s05`'s), and `cpuMs/f` is a `GetThreadTimes` delta that counts the spin. Freed EE work is converted into spin, so the metric does not move. Gate EE levers on an **uncapped** EE-heavy route (`dungeon1`), or push the capped route off the cap with `DC2_G503_EE_SLOW_US` first. *(This supersedes the older rule that `cpuMs/f` is the admissible substitute for `frame` on `s05`/`dungeon6`.)*
7c. **Work-Only EE Counter (G652)**: `[G182:ee] workCpuMs/f` subtracts only generated vblank-wait scopes and is the admissible EE work metric on capped routes. Guest continuations can migrate between host threads, so cumulative `GetThreadTimes` baselines must be keyed by host thread ID and rebased on migration; never subtract clocks from two different threads.
8. **Derivative Injection Rule**: Census levels alone do not prove conversion. Always probe with `DC2_G431_GS_SLOW_US` / `DC2_G503_EE_SLOW_US` to measure injection sensitivity ($\Delta \text{frame} / \Delta \text{injected}$) before choosing a lever.
9. **Delete Work, Not Waiting (G493/G622)**: Removing a synchronization wait on a secondary thread does not recover frame time unless the underlying work shrinks. *(G637 exception: a wait on the pole itself when `gsStallMs/f = 0.00` is work).*
10. **Single-FIFO Backend Law (G640)**: The amount recoverable by scheduling on a single FIFO GL backend is `blocked − backendBusy`, and nothing more. Async submission migrates waits to the next synchronous edge (`skill/resources/27-async-gs-gl-and-gpu-command-graph.md`).
11. **Producer-Bounded Transfers (G641)**: Bound transfers to the producer's active bounding rect instead of transferring the entire VRAM, without changing memory authority (`skill/resources/32-gpu-ssbo-compute-and-shadow-chains.md`).
12. **Grow-Only Streaming GPU Buffers (G643)**: SSBO/VBO buffers must be grow-only (round-up high-water capacity + `glBufferSubData`). Reallocating `glBufferData` on size change causes driver allocation stalls.
12c. **⛔⛔ A `const bool` Guard Does Not Make Diagnostic Code Free (G653 §P25)**: rule 12b's strict
form, measured on a change with **no behavioural difference at all**. ~60 lines of never-executed,
`const bool`-guarded census/prototype code added to `ps2_gs_rasterizer.cpp` cost **+0.484 ms/f on
`dragon` / GS own +0.443**, both order blocks agreeing (+0.591 / +0.377), 2,700 matched presents per
arm, no drift. ⭐ **No env rollback can ever recover it** — every rollback restores behaviour and
none removes a byte. **Rule**: a diagnostic that must live in a hot TU is a COMPILE-TIME build mode
with its call sites `#if`-EXCLUDED (not merely predicated), and its reporter in a separate TU.
Precedents: `PS2X_G652_CRITICAL_TRACE`, `PS2X_G653_POOLDIAG`. It is not an L1I capacity effect —
nothing was near a cache budget; the mechanism is MSVC's placement of everything *else* in the TU.
12b. **⛔⛔ Translation-Unit Layout Budget (G651)**: a TU has a code-layout budget the way a function has an inline budget. Adding ~150 lines of COLD start-up code to `ps2_gif_arbiter.cpp` (the TU holding `processGIFPacket` and the GS worker loop) cost **+0.567 / +0.934 ms/f of `GS own`** on `dragon`/`dungeon1`; moving the identical code to a TU with no hot loop recovered **1.13 / 1.06 ms/f**. ⭐ **Diagnostic**: a TOTAL gate that regresses on ONE THREAD while every lever's own arm is neutral-or-negative means LAYOUT — ⛔ no env rollback can find it, because every rollback restores behaviour and none removes a byte. Put new code in a TU that carries no hot loop.
13. **Hot-Loop Invariant Hoisting (G645/G650)**: Select execution arms once outside hot loops. Never place rollback ternaries inside inner loops; execute single-form compiled paths (`skill/resources/29-vif1-gif-compiled-acceleration.md`).
14. **Exactness Oracles vs Mandatory Pixel Gates (G635)**: Exactness oracles test arithmetic on one path; they cannot see path selection or source staleness. For any path selection or residency change, pixels against a same-binary control (`tools/g635_bisect.py` on `ridepod` opening) are mandatory.
15. **Rule 49**: **ONLY GEMINI IS ALLOWED TO UPDATE `plans/phase-history.md`.** No other AI assistant or automated agent may modify or edit `plans/phase-history.md`.
16. **⭐⭐⭐ Compile Closed Prototypes OUT, Not Just Off (G654 §P5)**: rule 12b/12c measured with the
sign reversed. The G629–G636 persistent-GS-domain family is **closed negative on performance and
default-OFF**, so every one of its entry points is a constant at run time — and it was still
**121,285 bytes across 1,059 COMDATs = 8.52%** of `ps2_gs_rasterizer.cpp`'s machine code.
`#if`-excluding it (`PS2X_G654_GPUDOMAIN`, default OFF) removed **134,807 bytes (−9.47%)** and bought
**−1.082 ms/f on `dragon:tail`**, with **GS own −0.996, VU1 −0.792 and EE −0.355 — all three threads**,
which is the signature of code placement rather than a subsystem lever. ⭐ **Method**: price a TU by
family first with `tools/g654_tu_price.py <obj> --auto`, then exclude, then gate. Replace the family
with constant-folding stubs that return exactly what the real code returns when disarmed, annotate
each stub with the source line that establishes it, and keep the real code restorable by one CMake
option so the two builds can be A/B'd and pixel-gated.
17. **⛔⛔ `[G146:perf]`'s Four Layers Do Not Partition `GS own` (G654 §P17)**: `GIFsubmit` is
**nested inside** `VIF1` (`processVIF1Data` opens a `G146PerfScope` and reaches `submitGifPacket`,
which opens a second; `G146PerfScope` has no nesting guard), and all four accumulators are global
atomics **summed across threads** while `GS own` is one thread's occupancy. `fight:rain` therefore
shows a **negative 9.44 ms/f "residue"** against its own pole. Never subtract them from a pole; use
`[G654:layer]` (`-DPS2X_G654_DIAG=ON`) for an exclusive, thread-keyed split.
18. **Gate Through `tools/g654_gate.py` (G654 §P1–P3)**: an A B B A is not scored until three
pre-checks pass — a **named scene window** resolved to script frames on the control arm (no whole-run
averages, no capped scenes), **arm observability** (a mechanism counter, a log marker or a binary
hash proving the arms can differ), and **host-drift rejection** (control/candidate drift ≤ 1.5 ms/f,
no block-sign reversal, no monotone ramp, balanced present counts). Run the four arms with
`tools/g654_abba.ps1`, which burns one discarded warm-up first — the session's first run of a route
read 1.56 ms/f high.

---

## Standard Phase Checklist

1. Load `skill/SKILL.md`, this file, `plans/ROADMAP.MD`, and `PS2Recomp/AGENTS.md`.
2. Attribute the current resource pole before choosing a lever ($\text{frame} \approx \max(\text{VU1 busy}, \text{GS own}, \text{EE cpu})$).
3. Keep one architectural variable per timing arm and repair parity failures before promotion.
4. Build Release targets incrementally (`/m:1`); never clean.
5. Run the exact oracle, paired timing (`DC2_G419_AB`), and relevant graphics test routes.
5b. **Mandatory Pixel Gate**: Run `tools/g635_bisect.py` and `tools/g635_look.py` on `ridepod` opening (`sf 768..1400`) for any change altering draw paths or data authority.
5c. **Mandatory Regeneration Smoke**: After any recompiler/code-generation change, compare route arrival and progression against a saved pre-change runner before trusting timing. Runtime environment rollbacks cannot remove an emitted call-site shape.
6. Update roadmap, env-flags, and the matching fix log upon phase completion. `plans/phase-history.md` remains Gemini-exclusive under Rule 49.

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

| Flag | Category | Purpose |
|---|---|---|
| `DC2_DEBUG_MENU=1` + `DC2_PAD_INPUT=...` + `DC2_NO_XINPUT=1` | Test Automation | Route automation & deterministic pad input replay |
| `DC2_PATCH_60FPS=1` | Timing | Mandatory performance uncapper (`0x00376C50 = 1`) |
| `DC2_NO_PRESENT_SYNC=1` | Timing | Rollback for G616 RTSS presentation synchronization |
| `DC2_LOG_LEVEL=<lvl>` | Logging | Global logging threshold (`TRACE`, `DEBUG`, `INFO` [default], `WARN`, `ERROR`, `FATAL`) |
| `DC2_PROFILE=1` / `DC2_PROFILE_OVERLAY=1` | Profiling | Master toggle for unified telemetry / Real-time HUD overlay (F3) |
| `DC2_G419_AB=<lever>` | Benchmarking | Randomized within-process A/B timing harness |
| `DC2_G637_NO_COOP=1` | Rollback | Rollback for G637 cooperative work-stealing band replay (default-ON) |
| `DC2_G639_NO_PAYPAR=1` | Rollback | Rollback for G639 parallel 0x139 GPU payload build (default-ON) |
| `DC2_G641_NO_WIN=1` | Rollback | Rollback for G641 producer-scoped 0x139 bounding window (default-ON) |
| `DC2_G642_NO_DERIVESPAN=1` | Rollback | Rollback for G642 in-shader 0x139 span derivation (default-ON) |
| `DC2_G643_NO_SSBO=1` | Rollback | Rollback for G643 SSBO data path for 0x139 kernel (default-ON) |
| `DC2_G645_NO_EEPOOL=1` | Rollback | Rollback for G645 EE/VIF1 packet buffer pool (default-ON) |
| `DC2_G646_NO_DISPATCH=1` | Rollback | Rollback for G646 direct-mapped guest dispatch table (default-ON) |
| `DC2_G648_NO_LAZYSPAN=1` | Rollback | Rollback for G648 lazy CPU span array (default-ON) |
| `DC2_G649_NO_TRISPAR=1` | Rollback | Rollback for G649 parallel 0x139 triangle setup loop (default-ON) |
| `DC2_G649_NO_DISPATCHLEAN=1` | Rollback | Rollback for G649 dispatch diagnostic hoisting (default-ON) |
| `DC2_G650_NO_FASTUNPACK=1` | Rollback | Rollback for G650 compiled VIF1 UNPACK kernels (default-ON) |
| `DC2_G650_NO_ROWPAL=1` | Rollback | Rollback for G650 hoisted paletted row readers (default-ON) |
| `DC2_G650_NO_SCHED=1` | Rollback | Rollback for G650 contention-aware core scheduler (default-ON) |
| `DC2_G651_NO_ROWPAL2=1` | Rollback | Rollback for G651 widened paletted arms — P4HL/P4HH SSE2 word group + no-mask body (default-ON) |
| `DC2_G651_NO_VUCLIP=1` | Rollback | Rollback for G651 in-region FCGET emission in G612 region bodies (default-ON) |
| `DC2_G651_NO_TOPO=1` | Rollback | Rollback for G651 adaptive CPU topology → G650's static role→core map (default-ON) |
| `DC2_G651_NO_DISPTRACE=1` | Rollback | Drops the per-dispatch diagnostic ring in `lookupFunction` (ring default-ON) |
| `DC2_G651_ROWPAL_STAT=1` / `_DISP_STAT=1` | Census | `[G651:rowpal]` paletted arm split · `[G651:disp]` EE dispatch-family call counts |
| `DC2_G182_EE_STAT=1` | Profiling | Extended G652 output includes migration-safe `waitCpuMs/f`, `workCpuMs/f`, and `workOnCPU`; use work, not raw CPU, on capped routes |
| `DC2_G652_VU_LOI=1` / `_NO_VU_LOI=1` | Refuted prototype | Opt in to P3 LOI admission / force its legacy exit; default OFF after neutral-negative timing |
| `DC2_G652_NO_VU_SHORTENTRY=1` / `_NO_VU_HAZSTORE=1` | Rollback | Independently restore the two promoted P13 VU tail/exit arms |
| `DC2_G652_NO_RUNTIME_FASTDISPATCH=1` | Rollback | Disables promoted P6 runtime-only single-probe dispatch |
| `DC2_G652_CRIT_TRACE=1` / `_RINGBENCH=1` | Diagnostic / experiment | P15 critical-path timeline and P17 persistent command-ring proof; both default-OFF and invalid in timing arms |
| `PS2X_G654_GPUDOMAIN=ON` (CMake) | Build mode | Restores the G629–G636 persistent-GS-domain prototype. **Default OFF = compiled out** (G654 P5). Every `DC2_G629_*` / `DC2_G630_*` / `DC2_G632_*` / `DC2_G633_*` / `DC2_G634_*` / `DC2_G636_*` flag exists ONLY in this build |
| `PS2X_G654_DIAG=ON` (CMake) + `DC2_G654_LAYER=1` | Build mode / census | `[G654:layer]` exclusive per-thread layer split, `[G654:submit]` the four-seam submit split, `[G654:bindmemo]` the per-entry bind-decision repeat census. Never valid in a timing arm |
| `DC2_ACK_LAYOUT_GROWTH=<reason>` | Build guard | Acknowledges a hot-TU code/COMDAT growth in `tools/g652_layout_guard.ps1`; the reason belongs in the phase fix log beside the timing gate that justified it |
| `DC2_TEXTURE_REPLACEMENTS=1` / `_DUMP=1` | Textures | Enable HD texture replacement / dumping (`Mods/HD Texture`) |

*Full flag catalogue: [plans/env-flags.md](file:///d:/ps2r/dc2/plans/env-flags.md).*

---

## Durable Architecture Overview

### 1. The 5 GS Worker Layers (`processGIFPacket`)
1. **Layer 1 — Batch Execution & CPU Band Replay**: Replays rasterization for display and discovered targets where GPU contracts fallback (accelerated via G605/G608/G609 triangle span kernels, G530/G614/G615/G617 sprite and fill kernels). Dispatched via G637 cooperative work-stealing (`GSRowPool::runCoop`).
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
- **Status: CLOSED NEGATIVE ON PERFORMANCE — and since G654, COMPILED OUT.** `PS2X_G654_GPUDOMAIN`
  (CMake option, default **OFF**) selects constant-folding stubs
  (`ps2_gs_rasterizer_parts/g654_stub_gpudomain.inc`, `g654_stub_g629.inc`) instead of the family.
  Removing its 134,807 bytes from the hottest TU bought **−1.082 ms/f on `dragon:tail`** (rule 16).
  ⚠️ Every `DC2_G629_*`/`DC2_G630_*`/`DC2_G632_*`/`DC2_G633_*`/`DC2_G634_*`/`DC2_G636_*` env flag
  exists ONLY in a `-DPS2X_G654_GPUDOMAIN=ON` build; a census that needs one must be run there.
- G636 resolved all visual defects across the prototype family (100% bit-exact `0.00/0.00/0.00` on `ridepod` opening). However, end-to-end performance is +1.13 to +1.51 ms/f slower than control due to private depth fallback leaving 57% of batches on CPU. Kept default-OFF.

### 4. GPU-VU Architecture Status
- **Status: CLOSED ACROSS ALL SHAPES.**
- Scalar GPU-VU (G606), translated GPU-VU compute (G607), and program-point batching (G608) are closed due to synchronization, kick-chain dispatch floor, and inter-kick RAW dependencies. Retained on native CPU compilation backend.

### 5. Unified Performance Profiler & Telemetry (`dc2_profiler.inc`)
- Multi-Threaded Model: Main Thread (serial wall-clock: Upload, Present, WaitFrame, Input, DrawHUD); Concurrent Worker Threads (cumulative CPU: `EE_CPU`, `VU1`, `GS`). Guide: [`PERFORMANCE_PROFILER_REPORT.md`](file:///d:/ps2r/dc2/PERFORMANCE_PROFILER_REPORT.md).

### 6. Debug Logging & Crash Reporting (`dc2_logger.inc` & `dc2_crash_reporter.inc`)
- Structured logging across 18 subsystem categories; Windows SEH crash filter, guest symbol resolver (`DAC.csv`, 7,810 symbols), register snapshots, MiniDump (`crashes/`). Guide: [`DEBUG_AND_CRASH_REPORTING.md`](file:///d:/ps2r/dc2/DEBUG_AND_CRASH_REPORTING.md).

### 7. HD Texture Replacement & Dumping Prototype (`ps2_texture_replacements.inc`)
- Supports 6,577 PNG pack (`Mods/HD Texture`), scanning at startup in ~37ms. Guide: [`TEXTURE_REPLACEMENT_REPORT.md`](file:///d:/ps2r/dc2/TEXTURE_REPLACEMENT_REPORT.md).

---
## Known Issues & Prototype Status


  - **Objectives**: 60 FPS floor (no gameplay frame over 16.67 ms); long-term 60 FPS.
- **High-Resolution Prototype Issues**:
  - **FOV Stretch**: Setting `DC2_RESOLUTION_WIDTH` stretches 4:3 image rather than expanding FOV.
  - **Height Multiples**: `DC2_RESOLUTION_HEIGHT` requires integer multiples of 448 (e.g. 896).
  - **Culling Glitches**: Textures/UI occasionally vanish at high resolution due to guest bounding-box culling against unscaled native coordinates.
  - **Debug Mode Freeze**: Returning to debug menu from within a level via Start+Select freezes frame display.
