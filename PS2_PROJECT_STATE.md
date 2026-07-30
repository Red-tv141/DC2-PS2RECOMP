# PS2 Recomp Project State — Dark Cloud 2

> **General, durable, forward-useful knowledge only** — operating rules, workspace/build facts,
> open technical gaps, graphic & audio test routes, and cross-cutting technical architecture.
> Active/next goals live in [ROADMAP.MD](file:///d:/ps2r/dc2/plans/ROADMAP.MD).

---

## Quick Rules
- **NO PER-SCREEN FIXES (hard rule).** Do not patch a symptom by writing game state per-frame/per-draw scoped to one screen (e.g. forcing `TitleProjection`/camera/renderinfo in the title scope). Such writes leak through shared globals into CONCURRENT screens. Diagnose to the ROOT and fix it at its source (the state machine / init / data path that is actually wrong), once, where the game itself would set it — not as a scoped band-aid. If a scoped lever is needed to PROVE a diagnosis, gate it opt-in (default-off) and never ship it as the fix.
- Build with `cmake --build <build_dir>` only; no clean targets, no build-dir deletion.
- Do NOT modify/create files in `runner/`, or modify standard headers. Split complex non-runner/override logic (e.g. `dc2_game_override.cpp`) into `.inc` files inside source subdirectories (e.g. `ps2xRuntime/src/`), never in the project root.
- Do NOT use destructive git commands. Keep diagnostics env-gated, quiet by default.
- Build via the **PowerShell tool**, not Bash `cmd /c` (silently no-ops → stale exe).
  Verify a change landed: `grep -c <marker> build64/Release/dc2_runner.exe`.
- Renderer promotion requires direct normal-output review against available hardware references. Internal oracles/counters do not prove downstream composition.
- Write a `plans/phase-<ID>-fix-log.md` before ending each executable phase — `plans/ROADMAP.MD` only ever gets a one-line "Active Phase" entry for the CURRENT phase.

### Standard Phase Checklist
- Use the local PS2Recomp skill at `D:/ps2r/dc2/skill/SKILL.md`. Obey this file.
- Performance phases follow the "Aggressive performance policy" below: the opt-in arm may render incorrectly during bring-up, but the default path must remain clean; keep one architectural variable per arm, repair discovered parity failures in the same phase, never promote incorrect output.
- Verify perf/render changes by FULL-FRAME DISTRIBUTION (multiple frames) + visual review, never a single golden sample alone.
- For anything touching threading/pipelining (MTGS worker, G157 pipeline, G144 band-replay), soak with `DC2_FRAME_DUMP_EVERY=1` (dense per-tick dumping).
- When bisecting a perf lever on a NON-title route, remember the frame-dump filename is the HOST TICK, not the guest scriptFrame — disabling GPU/tile-bin levers can make the CPU path several× slower in wall-clock time; budget accordingly and cross-check via state trace (`DC2_TRACE_F59`).

### Aggressive Performance Policy
The remaining gap to 60 FPS requires architectural experiments, not more low-yield barrier micro-tuning.
- The current default path is the immutable control arm. Risk applies only to a new default-off environment lever; never expose normal runs to an unfinished experiment.
- Change one architectural mechanism at a time. A phase may accumulate parity fixes for that one mechanism, but must not combine unrelated eligibility, ordering, depth, and timing experiments in one A/B arm.
- Do not stop at the first broken frame. Capture the earliest divergence, classify it as geometry, texture/CLUT, color/blend, depth, ownership/readback, or presentation, and fix its root while the phase remains active.
- Prefer same-run CPU shadow verification and exact dependency-boundary readback checks. Dense multi-frame review is mandatory.
- Measure performance throughout bring-up, but a timing result from incorrect output is not a win. Promotion requires the full route matrix to recover models, depth, lighting, text, textures, alpha cutouts, shared-page composition, and temporal stability.
- If parity cannot be restored or the architecture is neutral after parity, remove the behavior lever, retain useful diagnostics, and document in the phase fix log.

---

## Game / Workspace Overview

- **Title**: Dark Cloud 2 (NTSC-U). Main ELF: `SCUS_972.13`. Partial recovered symbols.
- **PS2Recomp repo (LIVE)**: `D:/ps2r/dc2/PS2Recomp`
- **Game workspace**: `D:/ps2r/dc2` · **ISO**: `D:/ps2r/dc2/Dark Cloud 2 (USA) (v2.00).iso`
- **Generated output**: `D:/ps2r/dc2/recomp` · **Build dir**: `D:/ps2r/dc2/build64`
- **Runtime override (most fixes live here)**: `PS2Recomp/ps2xRuntime/src/dc2_game_override.cpp`
- **Runtime syscalls/stubs**: `PS2Recomp/ps2xRuntime/src/lib/Kernel/{Syscalls,Stubs}/*.cpp` (`*.inl` are DEAD `/FORCE:MULTIPLE` dups — do not edit).

---

## Build & Smoke

```powershell
cmake --build D:\ps2r\dc2\build64 --config Release --target ps2_runtime -- /m:1
cmake --build D:\ps2r\dc2\build64 --config Release --target dc2_runner -- /m:1 /p:BuildProjectReferences=false
```
*Known-benign link warnings only: `LNK4006` (getGameName, `sceCdGetError`, …) + `LNK4088` (/FORCE).*
*G375: the runtime links FFmpeg. First `cmake` download prebuilt MSVC FFmpeg (`n7.1-241205`) into `build64/ThirdParty/`. Build offline / without it: `-DPS2X_ENABLE_FFMPEG=OFF`.*
*Note: After editing `recomp/`, build `dc2_game` target explicitly first (`cmake --build D:\ps2r\dc2\build64 --config Release --target dc2_game -- /m:1`).*

Default title smoke test (**G139 golden: held-menu frame_001500 `PixelNonZero=211646`**):
```powershell
powershell -ExecutionPolicy Bypass -File D:\ps2r\dc2\tools\run_30s_diagnose.ps1
```

---

## Active Runner Command & Key Environment Flags

**Active Command**: `D:/ps2r/dc2/build64/Release/dc2_runner.exe D:/ps2r/dc2/SCUS_972.13`

### Environment Flags Reference

#### General & Debug Flags
- `DC2_PATCH_60FPS=1`: Opt-in 60fps patch (`0x00376C50 = 1`) applied per frame via `mgEndFrame` hook.
- `DC2_DEBUG_MENU=1`: Enables the debug menu (`DebugFlag@0x00376FB8`).

#### Input & Test Scripting
- `DC2_PAD_INPUT='...'`: Scripted button inputs for deterministic tests.
- `DC2_DUNGEON_PAD='...'`: Scripted dungeon analog/button inputs (F66).
- `DC2_RSTICK='...'`: Scripted right-stick analog input (G49).
- `DC2_NO_XINPUT=1`: Disables live XInput controller polling (G7).

#### Frame Dumping & Diagnostics
- `DC2_FRAME_DUMP=1`: Dumps frames to `captures/frame_NNNNNN.ppm` every 60 ticks.
- `DC2_FRAME_DUMP_EVERY=1`: Dense per-tick frame dumping for soaking thread/pipeline changes.
- `DC2_G399_SURFDUMP=1`: G399 raw render-target probe (`DC2_G399_EVERY=<n>`, `DC2_G399_PPM=<n>`, `DC2_G399_FBPS=<hex,...>`).
- `DC2_G418_PROFILE=1`: Colour-readback edge split (`[G418:edge]` drain/transfer/kick). Inserts `glFinish`.
- `DC2_G419_AB=narrow|pack|lanes|all|zero|packlanes|pool|all420|narrowtouch`: Generic within-process randomized A/B instrument. `DC2_G419_PROFILE=1` for staging-pack census.
- `DC2_G303_INSTR=1` (+ `DC2_G182_EE_STAT=1`): **Thread attribution** — `[G303:vu1w]` VU1-worker / GS-worker busy ms/f + GS-collect stall, `[G182:ee]` EE cpu share. The premise-gate instrument: the frame is `max(threads)`, never `sum(buckets)`.
- `DC2_G421_CENSUS=1` (+ `DC2_G421_CENSUS_EVERY=<n>`): VU1 hot-loop dynamic census — `[G421:vu]` pairs/run + upper/lower execution shares, `[G421:mix]` upperKind/lowerOpHi/lowerS2 histograms, `[G421:cover]` inline-SIMD upper coverage, `[G422:cover]` inline lower coverage. Mix instrument only; its absolute frame is inflated. **`lowerS2` is an ALIAS histogram** — `S2_FUNC` is only meaningful when `funct >= 0x3C`, so the IADD/ISUB/IADDI/IAND/IOR mass appears under nonsense indices; use `[G422:cover]` + `lowerOpHi`.
- `DC2_G421_VERIFY=1`: Shadow-executes legacy `execUpper` against the G421 fast path and compares `vf`/`acc`/`mac`/`status`/`clip`, aborting on mismatch.
- `DC2_G422_VERIFY=1`: Shadow-executes legacy `execLower` against the G422 fast path and compares the whole `VU1State` **plus all 16 KB of VU data memory**, aborting on mismatch (~7× slower).
- `DC2_G332_CENSUS=1`: **GS-worker TOTAL busy** across window+boundary+apply (`[G332:gsw] totalMs/f`) plus the backend GL-busy-by-type split (render/readback/copy/other). **Mandatory alongside `DC2_G303_INSTR=1`** — `[G303:vu1w] gsWorkerMs/f` is window-only and under-reports the GS worker (G424). **But its readback bucket is inflated ~50% by its own timestamps (G425: 13.0 armed vs 7.9 measured independently); discount it before ranking GS against VU1.**
- `DC2_G425_PROFILE=1` (+ `DC2_G425_PROFILE_EVERY=<n>`): **VU1-worker internal attribution** — `[G425:vu1prof]` splits `VU1Interpreter::run()` into the instruction pair loop vs the XGKICK body (GIF tag walk + `g96Submit`), with bytes and µs per kick. Lean MAP-0: loop 97.6%, XGKICK 2.4%.
- `DC2_G425_RBCENSUS=1`: `[G425:rb]` GS colour-readback redundancy ceiling — re-reads of a window with a global mutation clock unchanged, with their measured ms.
- `DC2_G421_CENSUS=1` also prints `[G425:zero]`: zero-work pair (VNOP upper + NOP lower) run-length distribution.
- `DC2_G425_LEAN_PAIR=1` (opt-in, NOT promoted) / `DC2_G425_VERIFY=1`: G425 lean per-pair bookkeeping (skips `lastPc`/`g72_vi1_before`/the G369 guard and the VF0/VI0 re-pin on pairs that provably cannot name register 0). Exact but frame-neutral on this binary; the verifier asserts the VF0/VI0 invariant after every skipped re-pin.
- `DC2_G316_CENSUS=1`: GS-worker census by GIF path on sampled frames (`[G316:worker]` merge/sort/p1/p2/p3/boundary). MAP-0 is PATH2-dominated (~76%).
- `DC2_G313_CENSUS=1`: `GS::processImageData` segment split (`[G313:seg]` range/gen/bump/pre/writer/commit) plus per-PSM write rates and shapes (`[G313:psm]`). The instrument that sized G424.
- `DC2_G424_VERIFY=1` (+ `DC2_G424_VERIFY_EVERY=<n>`, default 64) / `DC2_G424_STAT=1`: G424 IMAGE-writer oracle — replays a sampled upload on both arms from an identical pre-state and compares all 4 MiB of GS memory plus the `m_hwregX/Y` transfer cursor, aborting on mismatch (`[G424:verify] ok=N bad=0`).
- `DC2_G326_VERIFY=1` (+ `DC2_G326_STAT=1`): independently rebuilds G264 mirror, G281 T8-coordinate, and G310 clean-page plans on every cache hit and compares them; `[G326:stat] ... bad=0` is G423's exact cache oracle.
- `DC2_G391_HD_DUMP=1` / `DC2_G391_MIX_DUMP=<path>`: Bounded `.HD` parameter hexdump / raw SFX mixer bus audio capture.

#### Promoted Performance Levers & Master Rollbacks
- `DC2_G410_NO_VU_COMPILED=1`: Rollback G410 compiled VU1 execution cache (default-ON).
- `DC2_G411_NO_GPU_SHARED_Z=1`: Rollback G411 private shared GPU-depth ownership (default-ON).
- `DC2_G412_NO_CROSS_FRAME=1`: Rollback G412 one-successor cross-frame host pipelining (default-ON).
- `DC2_G413_NO_DUAL_POLE=1`: Rollback G413 coupled VU1 MAC hot loop (default-ON).
- `DC2_G415_NO_FBO_RECYCLE=1`: Rollback G415 GPU-backend FBO geometry recycling (default-ON).
- `DC2_G418_NO_FAST_UNPACK=1`: Rollback G418 exact colour-readback VRAM unpack (default-ON).
- `DC2_G415_NO_NARROW_READBACK=1`: Rollback G419 promoted narrowed colour readback (default-ON).
- `DC2_G419_NO_FAST_PACK=1`: Rollback G419 promoted exact CT32 upload fast pack (default-ON).
- `DC2_G419_NO_PACK_LANE_GATE=1`: Rollback G419 promoted pixel-gated row-pool fan-out (default-ON).
- `DC2_G421_NO_FAST_UPPER=1`: Rollback G421 inline SIMD VU1 upper-slot execution (default-ON).
- `DC2_G422_NO_FAST_LOWER=1`: Rollback G422 inline VU1 lower-slot execution (default-ON).
- `DC2_G423_NO_PLAN_CACHE=1`: Rollback G423's exact G326 derived mapping-plan caches (default-ON); restores the original cold builders without changing native-renderer authority or ordering.
- `DC2_G424_NO_FAST_IMAGE=1`: Rollback G424's run-based host→local IMAGE upload writer (default-ON); restores the legacy per-pixel `GSMem::Write*` path.
- `DC2_G426_NO_PREDECODE=1`: Rollback G426's sidecar-resolved per-pair predicates (default-ON) — restores the per-pair G239 prestall compare chain, the per-pair G139 hazard index derivation, and the unconditional VF0/VI0 re-pin. Verify with `DC2_G426_VERIFY_PREDECODE=1` (+ `DC2_G425_VERIFY=1`).
- `DC2_G426_FUSED_UPPER=1`: **NO-GO, opt-in only.** G426's branchless fused upper FMAC dispatch. Exact (`DC2_G426_VERIFY=1` → `ok=450,000,001 bad=0`) but **+4.21 ms / +8.5%**. Retained as the arc's per-op cost instrument: it prices this loop at **~1 host cycle per SSE2 op**. Kill: `DC2_G426_NO_FUSED_UPPER=1`.
- `DC2_G26X_NO_NATIVE=1`: Master rollback disabling the entire native-renderer stack.

#### Audio Flags & Rollbacks
- `DC2_G384_MPEG_AUDIO_TRACE=1` / `DC2_G384_NO_MPEG_AUDIO=1`: FMV audio trace / rollback.
- `DC2_G393_PCM_ONLY=1` / `DC2_G393_MPEG_AUDIO_SELFTEST=1`: PSX-ADPCM playback rollback / selftest.
- `DC2_G385_AUDIO_TRACE=1` / `DC2_G385_NO_GAME_AUDIO=1`: Gameplay audio trace / rollback.
- `DC2_G386_VOICE_TRACE=1` / `DC2_G386_NO_VOICE=1`: Voice streaming trace / rollback.
- `DC2_G387_NO_BD_CHUNKS=1`: Rollback multi-chunk `.BD` bank concatenation.
- `DC2_G388_NO_BANK_ALIAS=1`: Rollback multi-port `SetHD` bank re-binding cache.
- `DC2_G389_LEGACY_MIX=1`: Rollback EZMIDI port volume scale divisor.
- `DC2_G390_LEGACY_SFX=1`: Rollback SFX voice-allocation & key-off fixes.
- `DC2_G391_LEGACY_SFX_PATH=1` / `DC2_G391_NO_ADSR=1` / `DC2_G391_NO_REVERB=1`: SFX 24-voice mixer rollbacks.
- `DC2_G392_LEGACY_SE_MIX=1` / `DC2_G392_NO_BGM_BUS=1`: Volume/pan swap & BGM mixer bus rollbacks.
- `DC2_G394_LEGACY_ASYNC_STOP=1`: Rollback title BGM early stop deferral.

#### Renderer Fix Rollbacks
- `DC2_G406_NO_ACCURATE_UV=1`: Rollback point-sprite 12.4 UV rounding to endpoint truncation.
- `DC2_G407_NARROW_UV=1`: Narrow point-sprite UV rounding predicate to within 1/64 texel of half-texels.
- `DC2_G408_LEGACY_SHADOW_COVERAGE=1`: Rollback scoped integer/top-left shadow triangle coverage.
- `DC2_G409_LEGACY_DUNGEON_MAP_LATCH=1`: Rollback Dungeon 6 `s19` world-origin publication.

*(Detailed benchmark numbers and phase logs for G400–G423 live in [phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md) and individual `plans/phase-GXX-fix-log.md` files).*

---

## Open Technical Gaps & Known Issues (ACTIVE)

1. **Native Renderer Performance Floor (VU1 IS THE POLE BY ~4 ms)**
   - Lean MAP-0 is **48.5 ms mean (20.6 FPS)** on a cold machine and drifts to ~51 ms across a long session; treat any figure below ~1.5 ms as unmeasurable without 4 pooled runs per arm in both orders.
   - Thread attribution (G425 re-measure, `DC2_G303_INSTR=1` + `DC2_G332_CENSUS=1` + `DC2_G425_PROFILE=1`), medians: **VU1 worker 47.6 ms/f** (of which `VU1Interpreter::run()` is 34.9 — **97.6% instruction pair loop**, 1.26 M pairs/f at **27.7 ns/pair** — 2.4% XGKICK, and ~12 ms/f of VIF/MTVU queue work outside `run()`); **GS worker ~43–44 ms/f of real work**; GS backend colour readback **7.9 ms/f over 4–7 calls**; EE **21 cpu-ms/f**. Convertible gap ≈ **4 ms**.
   - **Read the GS worker from `[G332:gsw] totalMs/f`, not `[G303:vu1w] gsWorkerMs/f`** — the latter is window-only (excludes the frame-boundary closure and apply branches) and under-reported the GS worker by ~6 ms in G424's premise gate. **And discount `[G332:gsw]`'s readback bucket**: its own timestamps inflate it ~50% (G425).
   - **G426 (2026-07-30) removed 1.65 ms (−3.1%) from the frame** by resolving three per-pair predicates in the G410 sidecar, and proved with an exact counter-lever that the pair loop is throughput-bound at ~1 cycle/SSE2-op. Roughly **2–2.5 ms of VU1-only headroom remains** before the VU1→GS crossover; re-run the G303/G332 gate before sizing the next VU1 lever.
   - G423's exact mapping-plan cache and G424's run-based IMAGE writer are paid and default-ON. Do not re-open G264/G281/G310 plan construction, G414–G420's exhausted GS front-end micro-vein, `DC2_G305_ASYNC` (structurally inert while G310 is default-on), or the `fbp=0x13b` readback drain as flush-recoverable (G418, net zero).
   - Master rollback: `DC2_G26X_NO_NATIVE=1`. Architecture & design: [arc-native-renderer.md](file:///d:/ps2r/dc2/plans/arc-native-renderer.md) and [arc-total-closure.md](file:///d:/ps2r/dc2/plans/arc-total-closure.md).

2. **VU1 Interpreter FMAC Flag Pipeline (OPEN, G71)**
   - PCSX2 delays MAC/STATUS/CLIP flags several instructions; `ps2_vu1.cpp` uses an immediate model. Revisit only if a flag-gated VU1 program (FMEQ/FMAND/FMOR/FCAND -> IBxx) misbehaves.
   - Optional VU1 float clamp `DC2_VU1_CLAMP` available but kept off pending broad testing.

3. **Sparse VU0 Helper Stubs (`Kernel/Stubs/VU.cpp`)**
   - `sceVu0InversMatrix`/`CameraMatrix`/`ScaleVectorXYZ`/`InterVectorXYZ` implemented; others stubbed. Audit implemented stubs against real VU0 microcode semantics (e.g. `sceVu0Normalize` dot-3 vs dot-4).

4. **Absent IOP R3000A Hardware Execution**
   - HLE layer handles PSS audio (G384/G393), HD/BD SFX and SQ BGM (G385), and WAV voices (G386). Upstream `ps2xIOP` remains HLE.

5. **Cooperative Thread Scheduler ABBA Safety**
   - Lightweight post-wake yields in SignalSema/SetEventFlag/WakeupThread/ReleaseWaitThread active. Full fiber scheduler deferred.

6. **VU0 Micro-Mode Execution Gap**
   - Inline COP2 macro mode works; VU0 micro-mode programs do not execute (dormant in DC2 skinned models).

---

## Routes for Graphic Test

Each row: reference dump → exact input route → harness → re-check triggers.

| Reference (`ref/dumps/`) | Route (debug-menu unless noted) | Harness | Re-check when touching |
|---|---|---|---|
| `correct_light.{gs,png}` | Debug menu → Down×2 → Circle → Square → Left → Cross (dungeon-0 map/light) | `tools/run_g237_capture.ps1` | **VU1 scalar-pipeline timing** |
| `dungeon_1_cutscene.{gs,png}` | Debug menu → Down×2 → Right → Circle → Cross×2 (Wood entrance) | `tools/run_g240_capture.ps1` | Alpha-test (`AFAIL`), blend, Z-write |
| `map_2_zoom.{gs,png}` | Debug menu → Down@0 → Right@15/30 → Circle@43 + zoom replay | `tools/run_g221_map2_zoom.ps1` | Rasterizer texture-interpolation (STQ) |
| `map_0.{gs,png}` | Debug menu → Down (~tick 15) → Circle (~tick 94) (town/MAP-0) | `tools/run_g194_map0.ps1` | Town geometry, Z, tile-bin/MTGS |
| `map_4.{gs,png}` | Debug menu → Down → Right×4 → Circle (town/MAP-4) | `tools/run_g204_map4.ps1` | VIF1 DMA chain-tag walker, shared-page clear |
| `map_4_zoom.{gs,png}` | Recorded route: Down@0 → Right@15/30/43/59 → Circle@77 | `tools/run_g218_map4_zoom.ps1` | Zoom-mode composite / RTT |
| `dungeon_0_cutscene.{gs,png}` | Debug menu → Down×2 → Circle → Cross×2 (dungeon-0 entrance cutscene) | `tools/run_g223_dungeon0_entrance.ps1`<br>`tools/run_g403_dungeon0_cutscene.ps1` | Cutscene camera/render path & shared ZBP character depth (G405) |
| `items.{gs,png}` | Debug menu → Down → Circle → Triangle → Cross | `tools/run_g361_items.ps1` | Boot static-inits (`DC2_G361_NO_SINIT`), 2D UV |
| `Inventory.{gs,png}` | Town → Down → Circle → Triangle×3 | `tools/run_g189_inventory_ab.ps1` | Sprite defer, pipelining, costume preview |
| `ttle.{gs,png}` | Held New-Game menu OR debug menu → Down×3 → Circle | `tools/run_g100_cap.ps1` | VU1 MAC/flags, culling, Z, presentation (`PixelNonZero=211646±4`) |
| `debug_menu.{gs,png}` | Debug menu → Down×2 → Down → Confirm (`DrawSub__8CEditMapFi@0x001B4130`) | `tools/run_30s_diagnose.ps1`<br>`DC2_DEBUG_MENU=1` | Preemption unwind (`g_dc2PreemptSuppressDepth`), vtable draw chain (G396) |

---

## Routes for Audio Test

Each row: subsystem / feature → exact trigger / route → harness & levers → verification & oracle checks.

| Subsystem / Feature | Replay Route / Trigger | Harness & Levers | Verification & Oracle Checks |
|---|---|---|---|
| **FMV / PSS PCM Audio (G384)** | Attract FMV playback (`RUSH.PSS` stream `0xBD` 48kHz stereo planar PCM) | `tools/run_g384_fmv_audio.ps1`<br>`DC2_G384_MPEG_AUDIO_TRACE=1`<br>`DC2_G384_NO_MPEG_AUDIO=1` | Demuxes planar PCM16LE (`SShd`/`SSbd`); streams via raylib without audio desync or silence. |
| **Sony HD/BD SFX & SQ BGM (G385, G391)** | Gameplay BGM & SFX triggers (`SE_Play@0x189C10`, MODMSIN messages) | `tools/run_g390_audio_route.ps1`<br>`DC2_G391_SELFTEST=1` (17/17)<br>`DC2_G385_AUDIO_TRACE=1` | Parses `SCEISequ` at 44.1kHz into 24-voice mixer (`dc2_g391_sfx_mixer.inc`); verifies `.HD` sample params (+18/+20 ADSR, +17 split pan) and fixed BGM bus gain (0.85). |
| **WAV Voice Streaming (G386)** | Spoken cutscene lines (EZBGM RPC SID `0x12345`) | `tools/run_g386_voice.ps1`<br>`DC2_G386_VOICE_TRACE=1`<br>`DC2_G386_NO_VOICE=1` | Indexes `SOUND.HD3` into `SOUND.DAT` for 48kHz mono WAV files (`0010665.wav`, `0010670.wav`); returns `0x1000` status until stream completion to unblock cutscene scripts. |
| **Multi-Chunk `.BD` Level BGM (G387)** | Level BGM load with multi-transfer SIF DMA `.BD` bank body | `DC2_G387_NO_BD_CHUNKS=1` | Concatenates multi-chunk SIF DMA `.BD` transfers so levels whose bank is split across transfers resolve all notes (was: 0/513 notes). |
| **Battle BGM Aliased Bank Re-binding (G388)** | Battle state trigger issuing second `SetHD` for committed bank | `tools/run_g388_battle_bgm.ps1`<br>`DC2_G388_NO_BANK_ALIAS=1` | Caches last committed bank by `SetHD` triple (`hdAddr`, `bdAddr`, `bodySize`) to re-bind aliased ports without note drops (was 0/2435 notes). |
| **EZMIDI Volume Scale & Pan Swap (G389, G392)** | Live `SE_SetVol` / `SE_SetPan` and EZMIDI port volume updates | `tools/run_g392_audio_route.ps1`<br>`DC2_G389_LEGACY_MIX=1`<br>`DC2_G392_LEGACY_SE_MIX=1` | Divides EZMIDI volume on 0..0x100 scale by `256.0f`; treats `F9 00` as channel volume and `F9 01` as channel pan (`0x40` = centre). |
| **SPU2 Reverb Topology & Retail Presets (G391, G392, G393)** | Reverb send (`SD_REV_MODE_*`) in dungeons and rooms | `tools/run_g392_audio_route.ps1`<br>`DC2_G391_SELFTEST=1` (17/17)<br>`DC2_G391_NO_REVERB=1`<br>`DC2_G392_NO_BGM_BUS=1` | Hardware-faithful 22,050Hz SPU2 reverb network using exact `LIBSD.IRX` `.data` preset tables (9 records). |
| **First-Visit Cold Title BGM Guard (G394)** | Cold title launch (first visit before attract FMV) | `tools/run_30s_diagnose.ps1`<br>`DC2_G394_LEGACY_ASYNC_STOP=1` | Defers zero-volume/stop pairs within 100ms of a new sequence Play to prevent stale EZMIDI stops from killing cold title BGM. |

---

## Reusable Technical Knowledge

### 1. Rendering Fixes & Sprite Sampling (G5, G6, G8, G362)
- **Split VIF1 IMAGE continuation (G6)**: VIF1 PATH2 IMAGE continuation qwords must be forwarded verbatim (`ps2_vif1_interpreter.cpp`) — never re-wrap in second IMAGE tag.
- **Debug-menu PSMT4HH font (G5)**: Packed 4-bit stream; stop at `TRXREG` rectangle, not oversized QWC request. CLAMP repeat semantics.
- **2D UI sprite point-sampling (G8, G362)**: DC2 UI text/icons are point-sampled (`tex1=0x201`, MMAG/MMIN=0). `drawSprite` FST bias is 0.0. `sampleExact` (G362) uses 12.4 fixed-point UV reconstruction (`lround(u*16)/16`) on `uMode` bit 12 to prevent float `floor()` rounding errors across atlas boundaries.
- **DISPFB Latch**: `f29_mgendframe_probe` DISPFB1/2 force-write is a presentation timing crutch for the half-rate title loop behind `DC2_FORCE_DRAW_BUFFER_LATCH=1`.

### 2. Pad Input Architecture (F66, G7, G49)
- **Live pad read is `read_pad_stub`** (`0x0014A490`) → `dc2_write_pad_status`. Writes buttons to `PAD_STATUS+0` and analog axes (`+4`=LY, `+8`=LX, `+0xc`=RY, `+0x10`=RX). `scePadRead` is DEAD. `CGamePad+0xc`=LX, `+0x8`=LY (`-0x80` centre, ±`0x32` deadzone).
- **Free-roam movement uses LEFT ANALOG STICK** (`Analog__11CPadControl@0x3d7b60`). Digital-only injector never moves player.
- **Host Present Loop Driving**: Headless pad script must drive off host present loop (`ps2_runtime.cpp`), as dungeon bypasses mgEndFrame hook.
- **XInput via raylib (G7)**: Polled by `g7_poll_live_pad()` in `ps2_pad.cpp`. Priority: `DC2_NO_XINPUT=1` → scripted `DC2_PAD_INPUT` → live gamepad/keyboard → default.
- **Scripted Right-Stick (`DC2_RSTICK`, G49)**: Controls costume preview rotation in headless tests.

### 3. Debug-Menu Dungeon Navigation & Harness
- `DC2_DEBUG_MENU=1` writes `DebugFlag@0x00376FB8`. Navigate with `DC2_PAD_INPUT='90..99:DebugDown;130..139:DebugDown;170..179:DebugConfirm'`.
- Frame dumps: `captures/frame_NNNNNN.ppm`. Filename is HOST TICK counter, NOT guest scriptFrame. Cross-check via `DC2_TRACE_F59=1`.

### 4. EE Software `double` Math (G373)
- R5900 FPU is single-precision only. Metrowerks implements `double` in software using 64-bit integer GPRs (`fptodp` @`0x002893C0`, `dpmul` @`0x00287FF8`, `dptofp` @`0x002887C8`).
- Double-precision libm entry points (`sin` `0x0011E1C0`, `cos` `0x0011DA28`, `atan` `0x0011D5D0`, `atan2` `0x0011EA80`, `pow` `0x0011EB98`, `sqrt` `0x0011EFC8`, `fabs` `0x0011DB30`, `floor` `0x0011DB88`): args in `$a0`/`$a1`, result in `$v0`. Each is a full 64-bit IEEE double in one GPR.
- Stubs using single-precision float ABI (`f12`/`f0`) return stale `$v0`, corrupting math without crashing. Use `ps2GetDoubleArg`/`ps2SetDoubleReturn`.

### 5. EE Float Arithmetic & Saturation (G371, G372)
- R5900 COP1 FPU has no Inf, no NaN, no denormals — saturates every result to `±0x7F7FFFFF`. `x/0` returns max finite value with operand signs; `sqrt` uses absolute value.
- NaNs/Infs in guest state indicate host IEEE overflow. Clamped via FPU macros (`FPU_ADD_S`, `FPU_SUB`, `FPU_MUL`, `FPU_DIV_S`, `FPU_SQRT_S`).
- COP2/VU0 FMAC ops are clamped by MAC-flag block. VU0 Q ops (`VDIV`/`VSQRT`/`VRSQRT`) and COP1 denormals are fixed via post-regen scripts.

### 6. Boot & Static Initializers (`__sinit_*`, G361)
- Metrowerks static initializers (`__sinit_<file>.cpp`) run via `.ctors` pointer table at `0x00374D80–0x00374E40` walked by `mwInit@0x00100190` called by `init__Fv@0x0015C160`.
- Nop'd `init__Fv` skipped all static initializers. `f55_boot_init_stub` runs the ctor table once before `InitCDFile`. Index 5 (`__sinit_mainloop.cpp`) stays excluded; `DC2_G361_SINIT_ALL=1` is diagnostic only.
- Diagnostic rule: zero state on a global struct with valid positions indicates BSS zero from un-run static ctor.

### 7. GS Transfers & BITBLTBUF (G383, G359, G358)
- Block pointers in `BITBLTBUF.SBP`/`DBP` are 14-bit fields in 256-byte block units. Guest TBP0 is ALREADY in block units — pass directly, do NOT scale by `*8`.
- MTGS readback contract: GS local→host readback (`sceGsExecStoreImage`) MUST call `g150_wait_idle()` before consuming bytes.

### 8. Recompiler & Codegen Technical Rules
- **`.inc` recompile trap (G359)**: MSBuild does not reliably rebuild when an included `.inc` changes. Always edit included `.cpp` (`GS.cpp`/`ps2_gs_gpu.cpp`) to force recompile.
- **Nested jump-table coverage (G395)**: `EventSelect__Fv@0x001923B0` uses the seven-entry inner table at `0x00365080`. Keep the full table explicit in `config_dc2_final.toml`; never stub its internal labels as standalone functions.
- **MSBuild SelectedFiles static-library trap (G395)**: building `dc2_game` with `/p:SelectedFiles=...` repacks `dc2_game.lib` with only the selected object. Build the explicit Release target normally.
- **`DIRECT_JAL_ONLY_TARGET` (G223)**: Cannot be traced via `registerFunction`; wrap jal target instead.
- **Back-edge preemption context safety (G186, G211, G212)**: Recompiled bodies unwound by preemption must suppress preemption during wrapper execution (`g_dc2PreemptSuppressDepth`).
- **VU1 scalar prestall (G239)**: Lower scalar stalls happen BEFORE either half of instruction pair executes; decode lower word first.
- **VU1 FDIV delay (G200)**: FDIV busy-stalls second DIV/SQRT/RSQRT until first result latches into Q.
- **Stack argument decoding (G194, G374, G378)**: R5900 EE ABI passes first 8 integer args in `$a0–$a3` and `$t0–$t3`. Stack args start at `sp + 0` in 8-byte slots.
- **Universal Z emulation (G203)**: Honor guest ZBUF/TEST universally, never whitelist per-screen.
- **`vf0` initialization (G40)**: Pin `vu0_vf[0] = (0,0,0,1)` after context memset.
- **VU1 Q latency (G87)**: Latches Q after fixed delay (7 cycles for DIV/SQRT, 13 for RSQRT).
- **Guest C heap location (F58)**: Guest C heap MUST sit ABOVE guest `_end` (`[F58:setupheap]` non-zero).

### 9. Off-EE-Thread Guest-Execution Lock (G377, G379)
- **Invariant**: Any host thread that runs recompiled guest code (e.g. INTC/DMAC IRQs, VSync callbacks, Alarm threads) MUST hold `PS2Runtime::GuestExecutionScope`.
- `m_guestExecutionMutex` is a recursive mutex and `g_guestExecutionDepths` is thread-local. Blocking syscalls release the lock via `GuestExecutionReleaseScope` (release-on-wait).

### 10. R5900 EABI Register & Stack Argument Layout (G374, G378)
- R5900 EABI passes first 8 integer arguments in `$a0–$a3` and **`$t0–$t3`**.
- There is NO 16-byte o32 stack save area. Stack arguments start at `sp + 0` in 8-byte slots.
- Software `double` math lives in a single 64-bit GPR (`$a0`/`$v0`) — never split across two 32-bit slots.

### 11. Recompiler Regeneration Procedure (G380, G381)
- **Authoritative Config**: Use `config_dc2_final.toml` in repo root (453 stub entries, verified 99.9% byte-identical regen).
- **Post-Regen Pipeline**:
  1. `ps2_recomp.exe config_dc2_final.toml`
  2. `python tools/fix_cop2_q_ps2_float.py recomp`
  3. `python tools/fix_cop1_ps2_float.py recomp`
- **STALE SCRIPT WARNING**: Do NOT run `tools/fix_cop2_destmask.py`. Modern `ps2_recomp.exe` emits correct dest masks; running `fix_cop2_destmask.py` corrupts the vector/clipper core.
- **Dead Stubs**: 0 dead `TODO_NAMED` process-abort stubs remain (`g381_dead_stub_repairs.inc` registers 35 handlers by guest address).

### 12. FMV/PSS Audio Architecture (G384/G393)
- DC2 PSS private-stream audio starts with substream selector (`ff a0 00 00`), then Sony `SShd`/`SSbd` ADS data.
- `RUSH.PSS` is codec 1 PCM16LE, 48 kHz stereo, with 0x200-byte planar blocks per channel. Deinterleave each channel block to LRLR samples before host playback.
- `MPEG.cpp` preserves guest callbacks while streaming supported PCM/ADPCM through raylib.

### 13. Gameplay SFX and Sequenced BGM Architecture (G385–G394)
- SIF DMA sends Sony `.HD` metadata and `.BD` VAG bodies; EZMIDI `SetHD` associates the pair with a sound port.
- Multi-chunk `.BD` transfers are concatenated before `SetHD` commit (G387). Aliased `SetHD` calls for committed banks are re-bound by HD/BD/size triple (G388).
- `SE_Play@0x189C10` emits program/volume/key-on messages through MODMSIN. Bank SFX run through a 24-voice 44.1 kHz stereo mixer (`dc2_g391_sfx_mixer.inc`) parsing `.HD` ADSR1/2 parameters (+18/+20) and equal-power split pan (+17).
- Sequenced BGM renders `SCEISequ` events against HD/BD instruments on the shared 44.1 kHz mixer bus through SPU2 hardware-preset reverb.
- Volume scale: EZMIDI port volume is 0..0x100 (divided by 256.0f). SE parameter stream order is `F9 00 <vol>`, `F9 01 <pan>`, `F9 02 <pitch>`.

### 14. Voice Streaming Architecture (G386)
- Voice filenames resolve through 16-byte `SOUND.HD3` entries into sector offsets within `SOUND.DAT`. Read only referenced 2048-byte sectors.
- Audio is standard 48 kHz mono 16-bit PCM WAV. `StreamOpenFast` reaches `ezBgm` SID `0x12345`; `sgCPlayVoice::Step` expects state `0x1000` while playing.

### 15. Performance Profiling & Sub-Millisecond Measurement Principles (G418–G420)
- **Within-Process Randomized A/B (`DC2_G419_AB=<lever>`)**: Sub-millisecond levers (~0.3 ms) cannot be measured by separate-process runs due to run-level offset noise (±1.5 ms). Switch the lever inside one process per frame using a fixed LCG sequence (`DC2_G418_ALTERNATE`), discarding warm-up frames (`DC2_G419_WARM`) and computing blocked window deltas.
- **Ballast Sensitivity Probe (`DC2_G418_ALT_BALLAST`)**: Multiply an isolated saving by the site's measured critical-path sensitivity before promising a frame-time payoff.
- **Measure Ceilings Before Building Levers**: Calculate absolute theoretical maximum time for an operation before building complex optimization levers (e.g. staging zero-fill is 4.6 MiB/frame but only 0.137 ms/frame at memory bandwidth limits).
- **Avoid GPU Resource Churn**: Attachment re-creation on target dimension changes (`fbp=0x139`) costs pure GL allocation time. Park outgoing FBO geometry by target size and revive it.
- **Thread Pool Dispatch Overhead**: Sizing the `lanes` parameter in `GSRowPool::run` splits rows but does not size thread wake-up convoy overhead (`m_threads.size()`). Gate parallel fan-out on total PIXELS (`DC2_G419_LANE_MIN_PX`), not row count.
- **Attribute to the THREAD before ranking any bucket (G421)**: the frame is `max(threads)`, not `sum(buckets)`. G414–G420 spent seven phases on 0.1–0.7 ms GS front-end slices while the VU1 worker sat at ~98% of the frame. `DC2_G303_INSTR=1` answers this in one 60-second run — do it first, every perf phase.
- **Size the claim to `pole − runnerUp`, not to the saving (G422)**: a lever on the pole converts to frame time only until that thread crosses under the second thread. G422 removed 11–13 ms from the VU1 worker and the frame moved 0.48 ms, because the crossover was 2.5–3 ms away. Compute the crossover distance in the premise run and state it as the phase's ceiling.
- **A thread-level win with a flat frame can still be worth promoting (G422)** — but only when it removes a *floor*. With G422 off, VU1 at ~60 ms would cap any future GS-side reduction at zero. Say this explicitly and never restate the neutral frame number as a win.
- **Re-price retained exact substrate after architecture changes (G423)**: G326's verified plan cache was worth only ~1–2 ms on its 2026-07-22 binary, but the unchanged mechanism became a **13.81 ms / 21.3%** frame win after G411–G422 changed ownership and worker costs. A prior no-go is evidence for that binary, not a permanent price. Preserve cold builders and exact verifiers so old substrate can be measured safely again.
- **Cache derived plans, never live renderer truth (G423)**: G264 address maps, G281 T8 coordinate maps, and G310 clean-page setup may be reused only because authority, eligibility, dirty/generation state, and publication order remain live checks. If a proposed cache includes those decisions, it is a different architectural mechanism and needs a new proof.
- **A refactor at a shared hot dispatch site changes the CONTROL ARM (G421)**: introducing a lambda/wrapper around `execUpper` added a call layer to the lever-OFF path and inflated the measured win from −8.4% to −9.8%. Same for an inlined census (~2 ms/f) and a second inlined fast-path copy at a 0.69% site. Use a local macro, keep the legacy call shape byte-identical, and re-measure the control against the pre-phase absolute.
- **Interpreter call boundaries are measurable and already priced (G421)**: G410's VNOP-skip bought ~8.6 ns per skipped `execUpper` call. Multiply that by the remaining call count to get a payoff ceiling *before* building anything — that is how G421's ~7 ms ceiling was known in advance.
- **This build has NO `/GL`/LTCG (G424)**: `CMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG`, so a small leaf function called from another translation unit is a **real call** that also blocks loop-invariant hoisting in the caller. G424's entire win was moving a per-pixel `GSMem::Write*` loop (~930k cross-TU calls/frame) into the TU that owns the swizzle tables — identical calls, identical arguments, identical order — for 2.97× on PSMCT32 and 1.85× on PSMT4. Before designing a cleverer algorithm, check whether the hot loop is simply paying a call boundary.
- **Size a lever's oracle to the part that is actually new (G424)**: when a fast path re-issues the *same* leaf calls in the same order, exactness is by construction and the only new logic is the run/loop decomposition. Verify *that* — replay both arms from an identical pre-state and compare the full destination buffer plus any resumable cursor (`DC2_G424_VERIFY`: 4 MiB of GS memory + `m_hwregX/Y`, 40,000+ comparisons at `bad=0`).
- **Price the site's marginal instruction before redesigning it (G426)**: the VU1 pair loop is THROUGHPUT-bound at **~1 host cycle per SSE2 op**, measured by building an exact fully-branchless upper dispatch and reading the cost of the ~20 ops it added (+4.21 ms / +8.5% over ~630 k executed uppers/frame). A hot interpreter loop is not automatically misprediction-bound — VU microprograms are loops, so their "data-dependent" jump tables are in fact well predicted. Removing branches at the cost of extra work is a *loss*; only a lower dynamic instruction count wins.
- **Per-pair decisions that are pure functions of the instruction encoding belong in the decode-time sidecar (G426)**: the G410 cache already stores and precisely invalidates the 64-bit pair source, so a predicate resolved there costs one bit test instead of being recomputed ~1.26 M times per frame. −1.65 ms / −3.1% for three of them. Corollary: **a sub-floor exact lever is a component, not a dead end** — G425 measured the VF0/VI0 re-pin elision as neutral on its own; bundled with two siblings of the same mechanism it cleared the noise floor.
- **Attribute INSIDE the pole thread, not just to it (G425)**: the VU1 worker also runs XGKICK (GIF tag walk + packet submission) and the VIF/MTVU queue. `DC2_G425_PROFILE=1` split it as 97.6% instruction pair loop / 2.4% XGKICK, with a further ~12 ms/f outside `run()` entirely — three different veins with three different levers, and only one of them was worth sizing.
- **A profiling census can inflate the very bucket it measures (G425)**: `DC2_G332_CENSUS` timestamps around each blocking backend GL item and reads colour readback at 13.0 ms/f where an independent clock in the same run reads 7.9. That 5 ms error is the difference between "the two workers are tied" and "VU1 leads by 4 ms". Cross-check any census bucket that wraps a blocking call with a second, independent clock.
- **Two batches that disagree in SIGN mean the effect is under the floor (G425)**: an exact, verified lever read −1.8 ms in arm order AB and +0.6 ms in order BA. Session-long thermal drift here is ~+3 ms, larger than a 1 ms lever. Report neutral; ship opt-in; do not average the two into a "win".
- **SSE2 is bit-exact for per-lane float work under MSVC `/O2` `/fp:precise` with no `/arch:AVX`**: `addps/subps/mulps` match scalar lane loops exactly and no FMA contraction is possible; `_mm_max_ps(a,b)` is `a>b?a:b` and `_mm_min_ps(a,b)` is `a<b?a:b`, matching the legacy ternaries operand-for-operand; `_mm_cvttps_epi32` matches MSVC x64's `(int32_t)(float)`. A masked bit-select store is value-identical to conditional per-lane stores.

---

## Game-Specific Bugs (Original-game defects — clamp at runtime boundary, never fix game-side)
- **4HH/4HL oversized transfer** (`mgLoadTextureZ@0x145400`): size `(w*h*bpp)/16` = 8× too large. Real HW survives the DMAC overrun; runtime consumes packed 4-bpp data and stops when `TRXREG` is full (G5). 4HH hides UI/font data in upper 8 bits of 24-bit Z-buffer, routed via GIF Path 3.
- **Intro movie `IsStarted` spin** (IPU/MPEG): bypassed headless.

---

## Recompiler Regen Caveat
- Allocator-family coherence is mandatory after TOML/stub changes. Do not split C/newlib allocation functions between runtime stubs and recompiled ELF bodies (`guestMalloc`, `memalign`, `free`, `realloc`, `calloc`).
- Stable texture corruption after allocator fixes usually indicates GS/texture state issues (CLUT upload/cache invalidation, TEX0/TEXA state, texture-page addressing, PSM/CPSM mismatch, TEXFLUSH, Z-write), not random memory noise.

---

## Technical Symbols & Upstream PR Cross-Reference Index
- **Boot C++ Runtime Inits**: `mwInit@0x00100190` walks `__initialize_cpp_rts` table (`0x00374D80–0x00374E40`).
- **GS Display Stride**: `copyDisplaySource` fallback reads `candidate.fbw = displayFrame.fbw` (512 vs context 640).
- **`ps2_stubs` & Memory Card Stubs**: Hand-written stubs in `Kernel/Stubs/` audited against R5900 EABI `$t0–$t3` integer arg layout.
- **Float Saturation**: Host float division replaced with `copysignf(INFINITY, ...)` in `tools/fix_cop1_ps2_float.py`.
- **Upstream PS2Recomp PR References**:
  - **PR #170**: `ps2xIOP` HLE refactor; no R3000A emulation or IRX loading/execution.
  - **PR #154**: SoundDriver RPC/status HLE parametrization; no sample renderer or IRX execution.
  - **PR #135**: Recompiler reachable-function generation.
  - **PR #120**: Cooperative thread scheduler ABBA-safe wait helper (`waitWithGuestExecutionReleasedUntilUnlocked`).
  - **PR #132**: GS VRAM addressing consolidation.
  - **PR #128**: Recompiler indirect-jump (JR/JALR) fallback codegen + Rabbitizer formatting.

---

*For full historical details of past phases, see [phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md).*
