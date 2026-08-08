# PS2 Recomp Project State — Dark Cloud 2

> **GENERAL PROJECT INFORMATION ONLY** — operating rules, workspace/build facts, and the list of open
> technical gaps. **No phase-specific numbers, no per-phase lessons, no route tables, no subsystem
> internals.** Those live in the files below. Read this file at session start; read the others on
> demand.

## Where everything lives

| You need | File |
|---|---|
| **What to do NEXT** — active phase, tasks, standing gate | [plans/ROADMAP.MD](file:///d:/ps2r/dc2/plans/ROADMAP.MD) |
| **Rules, paths, build, open gaps** | **this file** |
| **How to drive a test** — every graphics/audio route + harness | `skill/resources/appendix-dc2-test-routes.md` |
| **Any `DC2_*` env flag** | [plans/env-flags.md](file:///d:/ps2r/dc2/plans/env-flags.md) |
| **Why one phase did what it did** | `plans/phase-<ID>-fix-log.md` |
| **Closed veins, NO-GO table, superseded numbers, old rankings** | [plans/phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md) — **grep it, never read top-to-bottom** |
| **How this port's runtime works** (ABIs, addresses, data formats, contracts) | `skill/resources/appendix-dc2-runtime-architecture.md` |
| **Concrete paths, static export, harness wiring, PCSX2 A/B** | `skill/resources/appendix-dc2-project.md` |
| **Already-diagnosed graphics defects** | `skill/resources/appendix-dc2-graphics-facts.md` |
| **Generic PS2-recomp method** (not DC2-specific) | `skill/SKILL.md` → `skill/resources/` |
| **Native-renderer design** | [plans/arc-native-renderer.md](file:///d:/ps2r/dc2/plans/arc-native-renderer.md) · [plans/arc-total-closure.md](file:///d:/ps2r/dc2/plans/arc-total-closure.md) |

---

## Quick Rules

- **NO PER-SCREEN FIXES (hard rule).** Do not patch a symptom by writing game state per-frame or per-draw scoped to one screen (e.g. forcing `TitleProjection`/camera/renderinfo in the title scope). Diagnose to the ROOT and fix it at its source — the state machine / init / data path that is actually wrong — once, where the game itself would set it.
- **Never clean the build.** `cmake --build <build_dir>` only; no clean targets, no build-dir deletion.
- **Never modify or create files in `runner/`**, and never modify standard `.h` headers.
- **Never run destructive git commands.** Keep diagnostics env-gated and quiet by default.
- **Build via the PowerShell tool**, not Bash `cmd /c` (silently no-ops → stale exe).
- **Every performance run must set `DC2_PATCH_60FPS=1`.** A 33.33 ms/frame result is the 30 FPS cap.
- **Do not label `[G332:gsw] front` as front-thread CPU.** It is a wall interval that can include
  VU1 catch-up. Name a pole with a derivative probe, then optimize that resource.
- **Renderer promotion requires direct normal-output review** against available hardware references.
- **Write a `plans/phase-<ID>-fix-log.md` before ending each executable phase.**

### Standard Phase Checklist
- Use the local PS2Recomp skill at `D:/ps2r/dc2/skill/SKILL.md`. Obey this file.
- Performance phases follow the "Aggressive performance policy" below: keep one architectural variable per arm, repair discovered parity failures in the same phase, never promote incorrect output.
- Verify perf/render changes by FULL-FRAME DISTRIBUTION (multiple frames) + visual review.
- For anything touching threading/pipelining, soak with `DC2_FRAME_DUMP_EVERY=1`.

---

## Game / Workspace Overview

- **Title**: Dark Cloud 2 (NTSC-U). Main ELF `SCUS_972.13`. Partial recovered symbols.
- **Game workspace**: `D:/ps2r/dc2` · **ISO**: `D:/ps2r/dc2/Dark Cloud 2 (USA) (v2.00).iso`
- **PS2Recomp repo (LIVE)**: `D:/ps2r/dc2/PS2Recomp`
- **Generated output**: `D:/ps2r/dc2/recomp` · **Build dir**: `D:/ps2r/dc2/build64` (Visual Studio generator / MSBuild)
- **Runtime override — most fixes live here**: `PS2Recomp/ps2xRuntime/src/dc2_game_override.cpp`
- **Runtime syscalls/stubs**: `PS2Recomp/ps2xRuntime/src/lib/Kernel/{Syscalls,Stubs}/*.cpp` (the `*.inl` siblings are DEAD `/FORCE:MULTIPLE` duplicates)
- **Game data source**: ISO or extracted DATA folder mounted behind synthetic sector space (`lib/ps2_iso_mount.cpp`)
- **Launcher**: `D:/ps2r/dc2_Launcher` (WPF launcher, WPF published tree `publish/win-x64`)
- **Static analysis export**: `ref/functions/` + `ref/index/` — primary code-understanding tool

---

## Build & Smoke

```powershell
cmake --build D:\ps2r\dc2\build64 --config Release --target ps2_runtime -- /m:1
cmake --build D:\ps2r\dc2\build64 --config Release --target dc2_runner -- /m:1 /p:BuildProjectReferences=false
```

- **Benign link warnings only**: `LNK4006` (`getGameName`, `sceCdGetError`, …) + `LNK4088` (`/FORCE`).
- After editing `recomp/`, build `--target dc2_game -- /m:1` first.
- Smoke test gate: `frame_001500 PixelNonZero = 211646 ±4` (`skill/resources/appendix-dc2-test-routes.md`).

---

## Active Runner Command & Session-Critical Flags

**Active command**: `D:/ps2r/dc2/build64/Release/dc2_runner.exe D:/ps2r/dc2/SCUS_972.13`

**Full catalogue of flags**: [plans/env-flags.md](file:///d:/ps2r/dc2/plans/env-flags.md)

| Flag | Purpose |
|---|---|
| `DC2_DEBUG_MENU=1` + `DC2_PAD_INPUT='...'` + `DC2_NO_XINPUT=1` | Route automation |
| `DC2_PATCH_60FPS=1` | Mandatory performance uncapper |
| `DC2_FRAME_DUMP=1` / `DC2_FRAME_DUMP_EVERY=1` | Frame capture & soak |
| `DC2_G303_INSTR=1` + `DC2_G182_EE_STAT=1` + `DC2_G332_CENSUS=1` | Light occupancy census (names the pole) |
| `DC2_G419_AB=<lever>` | Randomized within-process A/B harness |
| `DC2_G26X_NO_NATIVE=1` | Master rollback for native-renderer stack |

---

## Open Technical Gaps & Known Issues (ACTIVE)

1. **Native Renderer Performance Floor (Thread Pole)**
   - **Current Baseline**: Final G533 paired candidates put lean MAP-0 near **19.1 ms/frame (≈52 FPS)** and heavy Georama near **31.8 ms/frame (≈31–32 FPS)** uncapped (`DC2_PATCH_60FPS=1`); Dungeon 6 remains content-dependent. Target: 16.67 ms / 60 FPS.
   - **Active Thread Pole Model**: Route-dependent. On the current G533 heavy-Georama binary, VU1 remains critical (+9.18 ms/f blocked derivative) while the GS-backend response is statistically null (+0.06 ms/f); band-replay lanes retain G530's ≈0.05× conversion. Re-run derivatives after every promotion.
   - **Phase Closure Index**: Full verbatim history of closed phases is archived in [plans/phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md). Milestone fix logs:
     - **G477–G486**: GS/VU1 levers ([G477](file:///d:/ps2r/dc2/plans/phase-G477-fix-log.md), [G479](file:///d:/ps2r/dc2/plans/phase-G479-fix-log.md), [G480](file:///d:/ps2r/dc2/plans/phase-G480-fix-log.md), [G481](file:///d:/ps2r/dc2/plans/phase-G481-fix-log.md), [G485](file:///d:/ps2r/dc2/plans/phase-G485-fix-log.md)).
     - **G487–G497**: Coverage & instrumentation ([G487](file:///d:/ps2r/dc2/plans/phase-G487-fix-log.md), [G488](file:///d:/ps2r/dc2/plans/phase-G488-fix-log.md), [G494](file:///d:/ps2r/dc2/plans/phase-G494-fix-log.md), [G496](file:///d:/ps2r/dc2/plans/phase-G496-fix-log.md)).
     - **G498–G506**: VU1 executor ([G498](file:///d:/ps2r/dc2/plans/phase-G498-fix-log.md), [G503](file:///d:/ps2r/dc2/plans/phase-G503-fix-log.md), [G505](file:///d:/ps2r/dc2/plans/phase-G505-fix-log.md), [G506](file:///d:/ps2r/dc2/plans/phase-G506-fix-log.md)).
     - **G507–G510**: GPU settlement & GS scopes ([G507](file:///d:/ps2r/dc2/plans/phase-G507-fix-log.md), [G508](file:///d:/ps2r/dc2/plans/phase-G508-fix-log.md), [G509](file:///d:/ps2r/dc2/plans/phase-G509-fix-log.md), [G510](file:///d:/ps2r/dc2/plans/phase-G510-fix-log.md)).
     - **G511–G516**: Tex/VU1 levers ([G512](file:///d:/ps2r/dc2/plans/phase-G512-fix-log.md), [G513](file:///d:/ps2r/dc2/plans/phase-G513-fix-log.md), [G514](file:///d:/ps2r/dc2/plans/phase-G514-fix-log.md), [G515](file:///d:/ps2r/dc2/plans/phase-G515-fix-log.md), [G516](file:///d:/ps2r/dc2/plans/phase-G516-fix-log.md)).
     - **G517–G521**: Pole inversions & GL worker scopes ([G517](file:///d:/ps2r/dc2/plans/phase-G517-fix-log.md), [G518](file:///d:/ps2r/dc2/plans/phase-G518-fix-log.md), [G519](file:///d:/ps2r/dc2/plans/phase-G519-fix-log.md), [G520](file:///d:/ps2r/dc2/plans/phase-G520-fix-log.md), [G521](file:///d:/ps2r/dc2/plans/phase-G521-fix-log.md)).
     - **G522–G524**: p2 & upload edge decomposition ([G522](file:///d:/ps2r/dc2/plans/phase-G522-fix-log.md), [G523](file:///d:/ps2r/dc2/plans/phase-G523-fix-log.md), [G524](file:///d:/ps2r/dc2/plans/phase-G524-fix-log.md)).
     - **G525–G533**: Heavy route survey, renderer overhaul, and VU1 re-pole ([G525](file:///d:/ps2r/dc2/plans/phase-G525-heavy-route-survey.md), [G526](file:///d:/ps2r/dc2/plans/phase-G526-fix-log.md), [G527](file:///d:/ps2r/dc2/plans/phase-G527-fix-log.md), [G528](file:///d:/ps2r/dc2/plans/phase-G528-fix-log.md), [G529](file:///d:/ps2r/dc2/plans/phase-G529-fix-log.md), [G530](file:///d:/ps2r/dc2/plans/phase-G530-fix-log.md), [G531](file:///d:/ps2r/dc2/plans/phase-G531-fix-log.md), [G532](file:///d:/ps2r/dc2/plans/phase-G532-fix-log.md), [G533](file:///d:/ps2r/dc2/plans/phase-G533-fix-log.md)).

2. **VU1 Interpreter FMAC Flag Pipeline (OPEN, G71)**
   - Delays MAC/STATUS/CLIP flags; revisit only if flag-gated VU1 programs misbehave.

3. **Sparse VU0 Helper Stubs (`Kernel/Stubs/VU.cpp`)**
   - Core matrix stubs implemented; audit remaining stubs as needed.

4. **VU0 Micro-Mode Execution Gap**
   - Micro-mode programs dormant in DC2 models; macro mode functional.

5. **Absent IOP R3000A Hardware Execution**
   - HLE layer handles PSS audio, SFX, BGM, WAV voices, and sound stops.

6. **Cooperative Thread Scheduler ABBA Safety**
   - Post-wake yields active in synchronization primitives.
