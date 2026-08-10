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
- **Do not label `[G332:gsw] front` as front-thread CPU.** It is a wall interval that can include VU1 catch-up. Name a pole with a derivative probe, then optimize that resource.
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
   - **Current Baseline**: route3 (palace battle + Palm Brinks rain) on the shipped default after
     **G569** is **30.55 ms/f mean, p50 28.58, p90 46.14, 32.9% of frames over the 33.33 ms
     budget** (pre-G568: 37.30 / 34.39 / 59.14 / 51.8%). Lean MAP-0 after G539 is **18.71
     ms/frame, 51–56 FPS** uncapped (`DC2_PATCH_60FPS=1`). **Immediate target: no gameplay frame
     over 33.33 ms — NOT yet met.** Long target: 16.67 ms / 60 FPS.
   - **GPU-residency table is now 14 slots** (`kG248TargetCount`, G569). Indices 0–7 unchanged;
     8–13 are route3's targets. Growing it is APPEND-ONLY — every residency array is declared
     `[kG248TargetCount]` and resizes automatically, but `kG261Fbp` must be extended in the same
     edit, and each new target needs its own depth/page-ownership audit (G534's law).
   - **Active Thread Pole Model — PER SCENE (G568 split the G541/G540 model).** Lean MAP-0/title:
     **VU1** is the pole (conversion ≈1.1, ~97–99% of frame), GS ~0.79 in series behind it, EE
     non-critical. route3 heavy bands: the **GS worker is the pole at 94% occupancy while VU1 runs
     at 47%**. Re-derive the pole on the route being optimised; do not carry one across scenes.
   - ⛔ **GPU VU1 Migration Gap — THE COMPUTE ARC IS REFUTED (Codex, 2026-08-10).** A scalar GPU-VU
     executor is **exact but takes 32–340 ms per 16-kick batch, and COMPUTE dominates — not the
     1 MiB copy**. The transport was never the problem, so the planned "price the round trip with a
     NULL kernel" step decides nothing. Three further exact-but-slower VU rewrites are recorded in
     `plans/phase-G569-fix-log.md` §"Cross-session merge". Any revival needs a different execution
     model. Historical scaffold below retained for reference only:
   - **GPU VU1 Migration Gap (historical)**: G541 provides a default-off exact kick corpus and asynchronous, coalesced, no-readback GPU transport. G542 adds a versioned padding-free GPU command ABI and transport gate. G543 adds exact offline GPU VU semantics, including route-specific Georama/Dungeon 6 corpora and GPU-denormal repair. G544 proves one-dispatch persistent state/pipeline/code/data plus bounded ordered XGKICK rings across all three corpora. G545 now has an incomplete default-off live seed/delta shadow: MAP-0 and Georama pass long exact streams, while Dungeon 6 fails closed at sequence 784. CPU VU1 and CPU packets remain authoritative. Work was stopped by the user; authority transfer, remaining routes, visual/audio soak, and promotion remain open.
   - **Phase Closure Index**: Full verbatim history of closed phases is archived in [plans/phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md). Milestone fix logs:
     - **G477–G486**: GS/VU1 levers ([G477](file:///d:/ps2r/dc2/plans/phase-G477-fix-log.md), [G479](file:///d:/ps2r/dc2/plans/phase-G479-fix-log.md), [G480](file:///d:/ps2r/dc2/plans/phase-G480-fix-log.md), [G481](file:///d:/ps2r/dc2/plans/phase-G481-fix-log.md), [G485](file:///d:/ps2r/dc2/plans/phase-G485-fix-log.md)).
     - **G487–G497**: Coverage & instrumentation ([G487](file:///d:/ps2r/dc2/plans/phase-G487-fix-log.md), [G488](file:///d:/ps2r/dc2/plans/phase-G488-fix-log.md), [G494](file:///d:/ps2r/dc2/plans/phase-G494-fix-log.md), [G496](file:///d:/ps2r/dc2/plans/phase-G496-fix-log.md)).
     - **G498–G506**: VU1 executor ([G498](file:///d:/ps2r/dc2/plans/phase-G498-fix-log.md), [G503](file:///d:/ps2r/dc2/plans/phase-G503-fix-log.md), [G505](file:///d:/ps2r/dc2/plans/phase-G505-fix-log.md), [G506](file:///d:/ps2r/dc2/plans/phase-G506-fix-log.md)).
     - **G507–G510**: GPU settlement & GS scopes ([G507](file:///d:/ps2r/dc2/plans/phase-G507-fix-log.md), [G508](file:///d:/ps2r/dc2/plans/phase-G508-fix-log.md), [G509](file:///d:/ps2r/dc2/plans/phase-G509-fix-log.md), [G510](file:///d:/ps2r/dc2/plans/phase-G510-fix-log.md)).
     - **G511–G516**: Tex/VU1 levers ([G512](file:///d:/ps2r/dc2/plans/phase-G512-fix-log.md), [G513](file:///d:/ps2r/dc2/plans/phase-G513-fix-log.md), [G514](file:///d:/ps2r/dc2/plans/phase-G514-fix-log.md), [G515](file:///d:/ps2r/dc2/plans/phase-G515-fix-log.md), [G516](file:///d:/ps2r/dc2/plans/phase-G516-fix-log.md)).
     - **G517–G521**: Pole inversions & GL worker scopes ([G517](file:///d:/ps2r/dc2/plans/phase-G517-fix-log.md), [G518](file:///d:/ps2r/dc2/plans/phase-G518-fix-log.md), [G519](file:///d:/ps2r/dc2/plans/phase-G519-fix-log.md), [G520](file:///d:/ps2r/dc2/plans/phase-G520-fix-log.md), [G521](file:///d:/ps2r/dc2/plans/phase-G521-fix-log.md)).
     - **G522–G524**: p2 & upload edge decomposition ([G522](file:///d:/ps2r/dc2/plans/phase-G522-fix-log.md), [G523](file:///d:/ps2r/dc2/plans/phase-G523-fix-log.md), [G524](file:///d:/ps2r/dc2/plans/phase-G524-fix-log.md)).
     - **G525–G530**: Heavy route survey & renderer overhaul ([G525](file:///d:/ps2r/dc2/plans/phase-G525-heavy-route-survey.md), [G526](file:///d:/ps2r/dc2/plans/phase-G526-fix-log.md), [G527](file:///d:/ps2r/dc2/plans/phase-G527-fix-log.md), [G528](file:///d:/ps2r/dc2/plans/phase-G528-fix-log.md), [G529](file:///d:/ps2r/dc2/plans/phase-G529-fix-log.md), [G530](file:///d:/ps2r/dc2/plans/phase-G530-fix-log.md)).
     - **G534–G545**: Correctness interrupts, Z-alias fix, level regression resolution, VU1 re-pole, and GPU-offload scaffold/ABI/exact persistent semantics ([G534](file:///d:/ps2r/dc2/plans/phase-G534-fix-log.md), [G535](file:///d:/ps2r/dc2/plans/phase-G535-fix-log.md), [G536](file:///d:/ps2r/dc2/plans/phase-G536-fix-log.md), [G538](file:///d:/ps2r/dc2/plans/phase-G538-fix-log.md), [G539](file:///d:/ps2r/dc2/plans/phase-G539-fix-log.md), [G540](file:///d:/ps2r/dc2/plans/phase-G540-fix-log.md), [G541](file:///d:/ps2r/dc2/plans/phase-G541-fix-log.md), [G542](file:///d:/ps2r/dc2/plans/phase-G542-fix-log.md), [G543](file:///d:/ps2r/dc2/plans/phase-G543-fix-log.md), [G544](file:///d:/ps2r/dc2/plans/phase-G544-fix-log.md), [G545](file:///d:/ps2r/dc2/plans/phase-G545-fix-log.md)).
     - **G546–G551**: Static VU1 fragment expansion (`imgspec_1be8` promoted [G550](file:///d:/ps2r/dc2/plans/phase-G550-fix-log.md), `imgspec_1c00` promoted [G551](file:///d:/ps2r/dc2/plans/phase-G551-fix-log.md), `imgspec_0900` refuted [G554](file:///d:/ps2r/dc2/plans/phase-G554-fix-log.md), `imgspec_1110` refuted [G555](file:///d:/ps2r/dc2/plans/phase-G555-fix-log.md)).
     - **G552–G560**: Cutscene `s05` profiling & GPU residency/compositor overhaul (`rtt181` promoted [G553](file:///d:/ps2r/dc2/plans/phase-G553-fix-log.md), `0x181` atlas compositor promoted [G557](file:///d:/ps2r/dc2/plans/phase-G557-fix-log.md), `rtt13b_solid` promoted [G558](file:///d:/ps2r/dc2/plans/phase-G558-fix-log.md), `dualsrc_blend` promoted [G560](file:///d:/ps2r/dc2/plans/phase-G560-fix-log.md)). `s05` frame time dropped **95.8 ms/f → 30.4 ms/f**.
     - **G569**: GPU-residency table grown 8 → 14 slots (inert). `rttwide` promoted then **REVERTED to default-OFF — user-verified graphical bug on every player jump/dash**; the `[G261:invariant] escaped writer` storm this phase booked as a *perf cost* was the defect. The four G553/G556/G557/G558 slots measured as a **bundle** (−22.0% on `s05`) and promoted — ⚠️ re-gate G558 alone, a parallel Codex run measured it slightly slower. User-reported **jump/dash spike class** characterized on route2, NOT root-caused. Codex cross-session refutations merged (GPU-VU arc closed) ([G569](file:///d:/ps2r/dc2/plans/phase-G569-fix-log.md)).
     - **G568**: Route3 frame-time spike census; `rtttri` discovered-target triangle deferral promoted default-ON (−5.82% paired, 4/4 both estimators); `bitalias` bit-field-disjoint texture/Z alias admission built, mechanism proven, payoff refused, left default-OFF ([G568](file:///d:/ps2r/dc2/plans/phase-G568-fix-log.md)).
     - **G561–G567**: Texture batching refutation (`tex_batch_wide` refuted [G561](file:///d:/ps2r/dc2/plans/phase-G561-fix-log.md)), static fragment selector tax refutations (`selector_1byte` refuted [G562](file:///d:/ps2r/dc2/plans/phase-G562-fix-log.md), narrow `0x0900` refuted [G564](file:///d:/ps2r/dc2/plans/phase-G564-fix-log.md)), persistent GPU-VU prototype (`g566_gpu_vu` [G566](file:///d:/ps2r/dc2/plans/phase-G566-fix-log.md)), and 190s full-stack soak test ([G567](file:///d:/ps2r/dc2/plans/phase-G567-fix-log.md)).

