# PS2 Recomp Project State — Dark Cloud 2

> **GENERAL PROJECT INFORMATION ONLY** — operating rules, workspace/build facts, open technical gaps, and essential environment flags. **No phase-specific numbers, per-phase post-mortems, or route tables.** Those live in the linked files below.

## Where Everything Lives

| Need | File |
|---|---|
| **What to do NEXT** (short-term goals & active phase targets) | [plans/ROADMAP.MD](file:///d:/ps2r/dc2/plans/ROADMAP.MD) |
| **Rules, paths, build, open gaps** | **this file** |
| **How to drive a test** (graphics/audio routes & harness) | `skill/resources/appendix-dc2-test-routes.md` |
| **Full catalogue of `DC2_*` env flags** | [plans/env-flags.md](file:///d:/ps2r/dc2/plans/env-flags.md) |
| **Why one phase did what it did** | `plans/phase-<ID>-fix-log.md` |
| **Closed phase history archive, NO-GO table, superseded numbers** | [plans/phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md) — *grep it; never read top-to-bottom* |
| **Runtime architecture, ABIs, double/float math semantics** | `skill/resources/appendix-dc2-runtime-architecture.md` |
| **Concrete paths, static export, harness wiring, PCSX2 A/B** | `skill/resources/appendix-dc2-project.md` |
| **Already-diagnosed graphics defects** | `skill/resources/appendix-dc2-graphics-facts.md` |
| **Project method & agent skill** | `skill/SKILL.md` and `skill/resources/` |
| **Native-renderer design** | `plans/arc-native-renderer.md` · `plans/arc-total-closure.md` |

---

## Quick Rules

- **NO PER-SCREEN FIXES (hard rule).** Repair the root state/init/data path once, where the game itself would set it. Do not patch a symptom by writing game state per-frame or per-draw scoped to one screen.
- **Never clean the build.** `cmake --build <build_dir>` only; no clean targets, no build-dir deletion.
- **Never modify or create files in `runner/`**, and never modify standard `.h` headers.
- **Never run destructive git commands.** Preserve unrelated worktree changes.
- **Build only inside an x64 `vcvars64` environment.** Build via PowerShell, not Bash `cmd /c`.
- **Every performance run must set `DC2_PATCH_60FPS=1`.** Otherwise 33.33 ms is only the 30 FPS cap.
- **Do not label `[G332:gsw] front` as front-thread CPU.** It is a wall interval that can include VU1 catch-up stall. Name a pole with a derivative probe, then optimize that resource.
- **Renderer promotion requires paired payoff, exactness evidence, rollback, and direct normal-output review** on exactly one mechanism-specific graphics route.
- **Review the selected route's complete full-frame distribution** plus representative full frames.
- ⛔ **A DEFECT DETECTOR THAT NEEDS THE HEALTHY TISSUE STILL PRESENT CANNOT SEE A TOTAL FAILURE.** `tools/g536_detect.py` fires only when a straight seam separates a dark block from a bright chromatic block. Run `tools/g596_skybroken.py` beside it and read `tools/g596_skyband.py`'s distribution.
- ⛔ **RANK A CPU-REPLAY POPULATION BY CLASS AND BY PRIMITIVE LEAF.** Rank by lane cycles (`[G605:leaf]` / `[G605:shape]`), reading `pro%` beside `cyc/inside`.
- ⛔ **A CONTROL MUST BE A PROMOTED POPULATION.** Always verify that a baseline or control path's flag is default-ON in the shipped binary. Build a same-run region control when no certified GPU-admitted population exists.
- ⛔ **A PERF HARNESS RESTORES ONLY WHAT IS IN ITS `$clear` LIST.** Add every new `DC2_*` flag to `tools/run_g477_perf.ps1`'s `$clear` array in the same edit that creates it.
- **Write `plans/phase-<ID>-fix-log.md` before ending each executable phase.**
- **ONLY GEMINI IS ALLOWED TO UPDATE `plans/phase-history.md`.** No other AI assistant or automated agent may modify or edit `plans/phase-history.md`.

### Standard Phase Checklist

1. Load `skill/SKILL.md`, this file, `plans/ROADMAP.MD`, and `PS2Recomp/AGENTS.md`.
2. Attribute the current resource pole before choosing a lever (`GS own = gsWorkerMs/f - gsStallMs/f`).
3. Keep one architectural variable per timing arm and repair parity failures in the same phase before promotion.
4. Build Release targets incrementally (`/m:1`); never clean.
5. Run the exact oracle, paired timing, and exactly one highest-risk graphics route (`tools/run_g536_map15.ps1`).
6. Update state, roadmap, flags, route appendix, and fix log upon phase completion.

---

## Game and Workspace Overview

- **Title**: Dark Cloud 2 (NTSC-U), Main ELF `SCUS_972.13`. Partial recovered symbols.
- **Workspace**: `D:/ps2r/dc2`
- **Game data**: `D:/ps2r/dc2/Dark Cloud 2 (USA) (v2.00).iso` or extracted DATA behind synthetic sector mount (`ps2_iso_mount.cpp`).
- **Live repository**: `D:/ps2r/dc2/PS2Recomp`
- **Generated output**: `D:/ps2r/dc2/recomp`
- **Build directory**: `D:/ps2r/dc2/build64` (Visual Studio generator / MSBuild)
- **Runtime overrides**: `PS2Recomp/ps2xRuntime/src/dc2_game_override.cpp` and split runtime parts.
- **Kernel syscalls/stubs**: `PS2Recomp/ps2xRuntime/src/lib/Kernel/{Syscalls,Stubs}/*.cpp` (siblings in `.inl` are dead `/FORCE:MULTIPLE` duplicates).
- **Static analysis export**: `ref/functions/` and `ref/index/`.

---

## Build & Smoke Commands

Run through `vcvars64.bat`:

```powershell
cmake --build D:\ps2r\dc2\build64 --config Release --target ps2_runtime -- /m:1
cmake --build D:\ps2r\dc2\build64 --config Release --target dc2_runner -- /m:1 /p:BuildProjectReferences=false
```

- **Link warnings**: Benign established `LNK4006` duplicates and `LNK4088` from `/FORCE`.
- **Game edits**: After editing `recomp/`, build `--target dc2_game -- /m:1` first.
- **Runner command**: `D:/ps2r/dc2/build64/Release/dc2_runner.exe D:/ps2r/dc2/SCUS_972.13`
- **Smoke gate**: Assert route arrival by game state, review full-frame distribution. Do not gate on title host tick (may be FMV).

---

## Active Session-Critical Flags

| Flag | Purpose |
|---|---|
| `DC2_DEBUG_MENU=1` + `DC2_PAD_INPUT=...` + `DC2_NO_XINPUT=1` | Route automation |
| `DC2_PATCH_60FPS=1` | Mandatory performance uncapper |
| `DC2_FRAME_DUMP=1` / `DC2_FRAME_DUMP_EVERY=1` | Frame capture / dense soak |
| `DC2_G303_INSTR=1` + `DC2_G182_EE_STAT=1` + `DC2_G332_CENSUS=1` | Light occupancy census (names the pole) |
| `DC2_G419_AB=<lever>` | Randomized within-process A/B harness |
| `DC2_G26X_NO_NATIVE=1` | Master rollback for native-renderer stack |
| `DC2_G591_NO_PRIVZ=1` | Rollback for promoted default-ON private-mirror depth epoch |
| `DC2_G592_NO_CONSUMER=1` | Rollback for promoted default-ON publication consumer test |
| `DC2_G594_NO_FASTHASH=1` / `DC2_G594_NO_FASTDESWZ=1` | Rollbacks for promoted default-ON logical-atlas guest-page hash and deswizzle |
| `DC2_G595_NO_LINEDEFER=1` / `DC2_G595_NO_NOTEMEMO=1` | Rollbacks for promoted default-ON display-line deferral and per-entry page-range memo |
| `DC2_G596_NO_FASTCOPY=1` | Rollback for promoted default-ON table-driven same-format local→local transfer body |
| `DC2_G600_NO_DISPLAY_PUB_GEN=1` | Rollback for promoted default-ON display-publication page-generation bump & snapshot re-anchor |
| `DC2_G601_NO_EMPTYBIND=1` | Rollback for promoted default-ON empty-footprint bilinear sprite bind admission |
| `#define DC2_G602_FASTDECODE 0` | Rollback for promoted default-ON Layer 4 GIF decode MSVC call-prologue outlining (compile-time) |
| `DC2_G603_SUBBLEND=1` / `DC2_G603_NO_SUBBLEND=1` | G603 display-buffer subtractive blend (GPU mode 6, HELD default-OFF / refuted by G604) |
| `DC2_G604_DISCOV_TRANSIENT=1` | G604 discovered targets transient contract (HELD default-OFF / refuted on exactness) |
| `DC2_G605_NO_TRI_SPAN=1` | Rollback for promoted default-ON exact accelerated replayed-triangle span kernel |
| `DC2_G608_NO_TRI_FST=1` | Rollback for promoted default-ON `fst=1` sampler leaf of that kernel |
| `DC2_G608_MEMPROF=1` (+ `_MEM_KICKS`/`_MEM_EVERY`/`_MEM_PATH`/`_MEM_RANGES`) | Per-program-point VU1 data-RAM read/write footprint. ⛔ Registered in `s_g295AnyProbeEnv` — selects the SLOW diagnostic pair-loop copy; **never quote frame time from an armed run** |
| `DC2_G606_VUBENCH=1` | Wide offline GPU-VU bench instrument (HELD default-OFF / scalar GPU-VU interpreter refuted) |
| `DC2_G607_PROF=1` (+ `_KICKS`/`_SKIP`/`_EVERY`/`_PATH`) | Per-kick VU1 control-flow profile. ⛔ Selects the SLOW diagnostic pair-loop copy — **never quote frame time from an armed run** |
| `DC2_G607_TBENCH=1` (+ `_TWIDTH`/`_TGROUPS`/`_TCHAIN`/`_TFILL`/`_TITERS`/`_TEXACT`) | Translated GPU-VU kernel throughput CEILING bench (HELD default-OFF / translated GPU-VU refuted). ⛔ `_TREPEAT` is NOT a throughput axis — the compiler hoists across repeats |
| `DC2_G434_NO_DRAW=1` + `DC2_G434_INV=1` | ⛔ **CEILING PROBE** (deletes `drawPrimitive` at `vertexKick` boundary) |
| `DC2_G447_EDGE=1` | Wall clock on every GS-worker blocking edge |
| `DC2_G598_DUMP_SF=<every>` (+ `_SF_LO` / `_SF_HI`) | Keys frame dump on `scriptFrame` instead of host present tick |

Full flag catalogue: [plans/env-flags.md](file:///d:/ps2r/dc2/plans/env-flags.md).

---

## Primary Open Technical Gap — GPU-VU Command / Authority Architecture & GS Worker Layers

### 1. Performance Target & Pole Attribution
- **Objectives**: 30 FPS floor (**no gameplay frame over 33.33 ms**) across all 12 game routes; long-term target **16.67 ms (60 FPS)**.
- **Pole Attribution Rule**: `frame ≈ max(VU1 busy, GS-worker own)`, where `GS own = gsWorkerMs/f - gsStallMs/f`.
- **Bypass Value**: Ceiling probe `DC2_G434_NO_DRAW=1` proves whole-GS-worker bypass value: **−27.9% on `s05` (36.13 → 26.04 ms/f)** and **−32.3% on `ridepod` boss (37.74 → 25.55 ms/f)**.
- **Current Route Poles**:
  - `s05` (forest cutscene): GS worker-bound (**6.48 ms/f headroom**).
  - `ridepod` (boss battle): **80.7% VU1-poled** — re-measured post-G605 by G607 over 300 windows
    (`-Census` + `tools/g605_pole.py`): GS own mean **25.34**, VU1 busy mean **27.01**, headroom
    mean **−1.67 ms/f** (**−3.75** in the VU1-poled windows), frame mean 30.07, `kicks/f`
    1125–1989. ⚠️ **Supersedes the pre-G605 "VU1 busy ~25.7, headroom 8.40 ms/f".** The prize for
    any VU1 lever is the HEADROOM (3.75 ms/f), not VU1 busy — below GS own it buys nothing.
  - `s03` (cutscene): **post-G608, 152 windows** — GS-poled 52 (34.2%), VU1-poled 100 (65.8%); GS own mean 31.77, VU1 busy mean 32.55, headroom mean **−0.77**. ⭐ **GS STILL OWNS THE HEAVY WINDOWS**: the top nine by GS own are all GS-poled (114.0 / 109.9 / 104.7 / 80.1 / 61.9 / 61.4 / 59.2 / 55.5 / 50.1 against VU1 31.7–51.2 in the same windows); 82/152 windows exceed 33.33 ms and 46 of those are GS-poled. Max GS own 185.70 (pre-G605) → 155.00 (post-G605) → **114.00** (post-G608).

### 2. Breakdown of the 5 GS Worker Layers (`processGIFPacket` ~34.8 ms/f baseline on `s05`)
1. **Layer 1 — Batch Execution & CPU Band Replay (~18.5 ms/f on `s05`)**:
   - On `s03`, Layer 1 graph bookkeeping (`prep+resolve+note`) is tiny (**0.005–0.27 ms/f**), while **CPU band replay** drives spikes up to 156 ms/f (`disp` 37.8%, `discov` 47.8%, `legacy` 0x139 14.4% of replay wall).
   - **G605 Promoted Triangle Span Kernel (`g605_tri_span.inc`, `DC2_G605_NO_TRI_SPAN=1` rollback)**: Lowered covered triangle pixels (80.4% of replay CPU, 865–873 cyc/inside vs 59.7 for G530 sprites) for CT32/T8-MODULATE/bilinear/Z24 private-Z shape family (`ALPHA ∈ {0x42,0x44,0x48}`). Hoisted batch-shared CLUT, private Z mirror loads/stores, and blend commit. Reduced triangle loop wall −21.1%, p95 99.32 → 59.22 ms/f on `s03`, verified **`bad=0` on 251.8 M pixels**.
   - **G608 Promoted `fst=1` Sampler Leaf (`g608SampleSharedT8Uv`, `DC2_G608_NO_TRI_FST=1` rollback)**: the same kernel with a second sampler — G605's is an STQ sampler, this one is `g141FastSample`'s own `u/16, v/16` arm, everything downstream shared verbatim. The four `disp tristrip fst=1 tbp=0x2720` shape rows go **847–877 → 590–604 cyc/inside (−31.0%)** over 91.4 M inside px; `disp tri-fast` LOOP wall −13.6%; **`bad=0` on 88.5 M pixels**; **−0.710 / −0.704 ms/f** on two estimators, windows >100 ms **3 → 0**.
   - ⭐⭐⭐ **Post-G608 leaf ranking on `s03`** (`DC2_G605_CYC=1`): `discov` tri-fast **43.6%** (651.4 cyc/inside, pro% 12.9) · `disp` tri-fast **34.7%** (621.6, pro% 0.4) · `disp` sprite **10.8%** (150.7, **pro% 67.8**) · `discov` sprite **10.7%** (60.7). Triangles are **78.3%** of replay CPU, and **the LOWERED triangle pixel still costs 10.2× the LOWERED sprite pixel**. The residue is per-pixel interpolation (⛔ a DDA rewrite is NOT exact), two per-pixel call boundaries, and ~7 default-off diagnostic branches (⭐ exact by construction — G602's promoted technique, one subsystem over).
   - Generic GPU admission attempts (G603 mode 6 blend, G604 discovered transient contract) are **HELD default-OFF** due to exactness refutations (28.21% and 2.044% error rates on same-run oracles).
2. **Layer 2 — `drawPrimitive` & Tile-Bin Capture (5.44 ms/f → 3.79 ms/f, CLOSED by G595)**:
   - Optimized per-flush fixed front-end (`g310EnsureCurrent`) via G595 (separated winner search from page-hash evaluation, eliminated `std::array` zero-init).
3. **Layer 3 — Register-Triggered TRXDIR Transfers (4.89 ms/f → 4.57 ms/f, CLOSED by G596)**:
   - Decomposed in G596 (`l.flush` pin 4.47 ms/f, `l.fmt` copy 1.37 ms/f). Replaced format-aware per-texel transfer body with `g596copy` table-driven same-format PSMT8/PSMCT32 local copy body (reduced `l.fmt` 1.370 → 0.146 ms/f, −89.4%).
4. **Layer 4 — GIF Tag Walk & Register Replay (4.87 ms/f → 2.60 ms/f, CLOSED by G602)**:
   - Decomposed in G602 (2.79 ms/f vertex tag walk at 114 ns/kick + 2.08 ms/f non-vertex register replay at 17.9 ns).
   - Root cause identified as MSVC call prologue XMM callee saves (9 spills in `vertexKick`, 4 in `writeRegisterPacked`, 2 in `writeRegister`, 4 in `processGIFPacket` + /GS cookies) due to default-OFF diagnostic float printing.
   - Outlined cold diagnostic helpers (`g602_gif_cold_outline.inc`), hoisted static guards to namespace scope. MSVC prologues went to 0 XMM spills across all 4 hot functions. `parserOther` reduced −0.986 ms/f (−11.78%) on `s05`.
5. **Layer 5 — Host→Local Image Writer (1.09 ms/f)**:
   - Stable; low contribution.

### 3. GPU-VU Architecture Status & Refutations — ⛔⛔ **THE WHOLE ARC IS CLOSED (G606 + G607 + G608)**

- **Translated GPU-VU Design CLOSED on SYNCHRONIZATION (G607)** — *not* divergence, *not* arithmetic:
  - **Premise CONFIRMED.** Static CFG of the dominant image (recovered with VI constant propagation:
    1553 of 2048 pairs reachable, 21 subroutines, 6 XGKICK sites) covers **100.0000% of 1,187,573
    executed pairs** over 1024 kicks sampled across the live `ridepod` route. The five largest
    per-item loops — **51.65% of all pairs** — contain **zero conditional branches**, so G606's
    divergence mechanism does not reappear in a translated kernel.
  - **Width is p50 = 5** iterations per loop entry (mean 10.7, max 57; 0.00% of loop-body pairs at
    width ≥ 64) ⇒ 28.4% warp-32 occupancy. Costly but not fatal.
  - **Arithmetic is not the constraint.** Translated straight-line GLSL of the top loop measures
    **4.25 G pair-steps/s at width 5** (58× one CPU core at 73 M) and 120.9 G saturated. Exact FP64
    RSQRT costs only **2.45–3.08×**.
  - **⛔ The kick chain closes it.** 44.1% of kicks read the previous kick's writes (0 clean batches
    at window ≥ 4), so kicks cannot be merged: **1431 serialized dispatches per `ridepod` frame**.
    Measured GPU-resident chained-dispatch floor (no CPU sync, memory barrier only) = **5.17 µs**
    ⇒ **7.40 ms/f of pure ordering overhead** (5.82–10.28 across the route's real `kicks/f` range)
    against a re-measured usable prize of **3.75 ms/f** — **1.6×–2.7× the whole prize**, measured
    with every choice favouring the GPU (native float, one loop entry per kick, no XGKICK, no
    transport, no GS contention, best-of-8).
  - **⛔ DO NOT build a GPU VU1 backend of any shape that takes one device dispatch per kick.**
- **⛔⛔ Program-Point Batching REFUTED (G608) — the LAST surviving GPU-VU idea is struck.**
  G607 parked "gather every pending invocation of loop L across many kicks into one wide dispatch"
  behind one precondition: prove the hot loops' own inputs miss the carried conflict set.
  `DC2_G608_MEMPROF=1` (per-kick, per-PC-bucket 1024-bit VU-RAM read/write bitmaps, shipped bodies'
  own address expressions) + `tools/g608_memdep.py`, **1024 kicks sampled 1-in-1500 over 1,534,501
  kicks of the live `ridepod` route**, dominant image 98.8%:
  - **5 of 5** branch-free hot loops (51.65% of all executed pairs) **import quadwords a previous
    kick wrote**. Imported:private = **4.4× / 14.0× / 8.4× / 2.4× / 39.7×**.
  - **L0 (0x1850, 27.00% of all pairs) imports qw 800–808 — inside G606's own published carried run
    781–808**; L3 (the output-assembly loop before `XGKICK`) imports 802–809.
  - ⭐⭐⭐ The corpus-derived carried set is **574 qw**, not G606's 121 (a whole-kick measurement).
    Testing only against the published constant would have reported **3 of 5 loops CLEAN**.
  - ⛔ **GOAL 3 IS NOW CLOSED IN EVERY SHAPE**: interpreted (G606, divergence), translated (G607,
    synchronization), program-point-batched (G608, cross-kick data flow). Details:
    `plans/phase-G608-fix-log.md` §8.
- **Scalar GPU-VU Interpreter Design CLOSED (G606)**:
  - Re-measured scalar GPU-VU executor (`g566_gpu_vu` / G543) wide using offline bench (`g606_wide_vu.inc`, `DC2_G606_VUBENCH=1`).
  - Proved original refutations were based on a **1-lane-wide shader** (only 1 thread per workgroup active).
  - Widening `local_size_x` (1 → 64) improved throughput from 2.31M to 3.09M pair-steps/s (saturated at 512 threads).
  - Proved primary bottleneck is **SIMD DIVERGENCE** (warp lanes executing different opcodes). Forcing lockstep (`DC2_G606_LOCKSTEP`) increased throughput 10.3× to 31.82M pair-steps/s (59.86M pair-steps/s ceiling on long kicks).
  - Zero-divergence ceiling (59.9M pair-steps/s = 38.0 ms/f on `ridepod`) is **slower than 1 CPU core** (~73M pair-steps/s = 27.0 ms/f). Real divergent GPU execution is 3.09M pair-steps/s (**638 ms/f**, 23.6× short of CPU).
  - Kick independence sweep (`DC2_G606_TRACE=1`): **44.1% of kicks read state written by immediately preceding kick at window 2**; 0 clean batches at window ≥ 4.
  - ⛔ **Its "active direction" (microprogram TRANSLATION) was then priced and refuted by G607 above.**
    Do not restart it from this section.

### 4. Promoted Default-ON Performance Levers
- **G591**: Private depth mirror write epoch (`DC2_G591_NO_PRIVZ=1` rollback).
- **G592**: Publication consumer test (`DC2_G592_NO_CONSUMER=1` rollback).
- **G594**: Logical atlas fast page hash & deswizzle (`DC2_G594_NO_FASTHASH=1` / `DC2_G594_NO_FASTDESWZ=1` rollbacks).
- **G595**: Display-line deferral & per-entry page-range memo (`DC2_G595_NO_LINEDEFER=1` / `DC2_G595_NO_NOTEMEMO=1` rollbacks).
- **G596**: Fast table-driven local→local transfer body (`DC2_G596_NO_FASTCOPY=1` rollback).
- **G600**: Display publication page-generation bump & snapshot re-anchor (`DC2_G600_NO_DISPLAY_PUB_GEN=1` rollback).
- **G601**: Empty-footprint bilinear sprite bind admission (`DC2_G601_NO_EMPTYBIND=1` rollback).
- **G602**: GIF decode MSVC call-prologue outlining (`#define DC2_G602_FASTDECODE 0` compile-time rollback).
- **G605**: Exact accelerated replayed-triangle span kernel (`DC2_G605_NO_TRI_SPAN=1` rollback).
- **G608**: The `fst=1` sampler leaf of that kernel (`DC2_G608_NO_TRI_FST=1` rollback).
