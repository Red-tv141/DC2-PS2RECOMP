# PS2 Recomp Project State — Dark Cloud 2

> **General, durable, forward-useful knowledge only** — operating rules, workspace/build facts,
> currently-open issues (summarized), graphic test routes, and cross-cutting technical knowledge
> likely needed again regardless of which phase is active (pad-input protocol, PCSX2 A/B protocol,
> regen caveats, generalized "lessons learned").
> Active/next goals live in [ROADMAP.MD](file:///d:/ps2r/dc2/plans/ROADMAP.MD).
> Detailed historical phase narratives and closed perf-arc logs live in [phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md) or specific `plans/phase-GXX-fix-log.md` files.

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
*G375: the runtime now links FFmpeg. The first `cmake -S . -B build64` after G375 downloads a
prebuilt MSVC FFmpeg (`n7.1-241205`) via `ExternalProject` into `build64/ThirdParty/`, and
`ps2x_stage_ffmpeg_runtime_dlls(dc2_runner)` copies `av*/sw*` DLLs beside `dc2_runner.exe`.
Build offline / without it: `-DPS2X_ENABLE_FFMPEG=OFF` (MPEG video falls back to stub frames).*
*Note: After editing `recomp/`, build `dc2_game` target explicitly first (`cmake --build D:\ps2r\dc2\build64 --config Release --target dc2_game -- /m:1`).*

Default title smoke test (**G139 golden: held-menu frame_001500 `PixelNonZero=211646`**):
```powershell
powershell -ExecutionPolicy Bypass -File D:\ps2r\dc2\tools\run_30s_diagnose.ps1
```

---

## Active Runner Command & Key Environment Flags

**Active Command**: `D:/ps2r/dc2/build64/Release/dc2_runner.exe D:/ps2r/dc2/SCUS_972.13`

### Common Environment Flags
- `DC2_PATCH_60FPS=1`: Opt-in 60fps patch (`0x00376C50 = 1`) applied per frame via `mgEndFrame` hook. Accelerates dungeon/town routes (native 30fps).
- `DC2_DEBUG_MENU=1`: Enables the debug menu (`DebugFlag@0x00376FB8`).
- `DC2_PAD_INPUT='...'`: Scripted button inputs for deterministic tests.
- `DC2_DUNGEON_PAD='...'`: Scripted dungeon analog/button inputs (F66).
- `DC2_RSTICK='...'`: Scripted right-stick analog input (G49).
- `DC2_NO_XINPUT=1`: Disables live XInput controller polling (G7).
- `DC2_FRAME_DUMP=1`: Dumps frames to `captures/frame_NNNNNN.ppm` every 60 ticks.
- `DC2_FRAME_DUMP_EVERY=1`: Dense per-tick frame dumping for soaking thread/pipeline changes.
- `DC2_G384_MPEG_AUDIO_TRACE=1`: Bounded FMV ADS/PCM header, playback, and progress trace.
- `DC2_G384_NO_MPEG_AUDIO=1`: Same-binary rollback for G384 host FMV PCM playback.
- `DC2_G393_PCM_ONLY=1`: Same-binary rollback for G393 Sony PSX-ADPCM PSS audio decoding.
- `DC2_G393_MPEG_AUDIO_SELFTEST=1`: G393 PSX-ADPCM decoder/interleave self-test (expect 4/4).
- `DC2_G385_AUDIO_TRACE=1`: Bounded gameplay-audio DMA/RPC/bank/BGM/SFX/voice-open trace.
- `DC2_G385_NO_GAME_AUDIO=1`: Same-binary rollback for G385 Sony HD/BD SFX and SQ BGM playback.
- `DC2_G386_VOICE_TRACE=1`: Bounded EZBGM SID `0x12345` voice open/play/state/completion trace.
- `DC2_G386_NO_VOICE=1`: Same-binary rollback for G386 WAV voice streaming.
- `DC2_G387_NO_BD_CHUNKS=1`: Same-binary rollback for G387 multi-transfer `.BD` bank reassembly.
- `DC2_G388_NO_BANK_ALIAS=1`: Same-binary rollback for G388 multi-port re-bind of one `SetHD` bank.
- `DC2_G389_LEGACY_MIX=1`: Same-binary rollback for the G389 port-volume scale/SFX attenuation.
- `DC2_G390_LEGACY_SFX=1`: Same-binary rollback for the G390 SFX voice-allocation fixes.
- `DC2_G391_LEGACY_SFX_PATH=1`: Rollback to the G390 fire-and-forget one-shot path (no G391 mixer).
- `DC2_G391_NO_ADSR=1`: Rollback the HD ADSR envelope (SFX + BGM) to the fixed attack/release.
- `DC2_G391_NO_REVERB=1`: Disable the G391 SPU2 reverb bus.
- `DC2_G391_LEGACY_BGM_NORM=1`: Restore per-sequence BGM peak normalisation.
- `DC2_G392_LEGACY_SE_MIX=1`: Restore the G385 reading of the SE stream (`F9 01` as volume).
- `DC2_G392_NO_BGM_BUS=1`: Put sequenced BGM back on private per-port streams (no reverb send).
- `DC2_G394_LEGACY_ASYNC_STOP=1`: Restore literal early zero-volume/Stop handling; reproduces
  first-title silence before the attract FMV.
- `DC2_G399_SURFDUMP=1`: G399 raw render-target probe. At each present latch, reads the dungeon
  RTT chain straight out of VRAM as PSMCT32 512x416 and prints `[G399:surf] n=… fbp=… meanL/maxL/
  meanA/nz`. `DC2_G399_EVERY=<n>` report interval (default 60), `DC2_G399_PPM=<n>` also write
  `captures/g399_surf_<fbp>_<n>.ppm` (default 0 = stats only), `DC2_G399_FBPS=<hex,…>` surface list
  (default `139,68,0`). Diagnostic only; also joins `g289RawVramDiagOn()` so VRAM stays
  authoritative under the G178 GPU raster path.
- `DC2_G400_NO_RESIDENT_139=1` / `DC2_G400_NO_RESIDENT=<hex,…>`: G400 ownership A/B. Drops the
  named `fbp`(s) from the native renderer's resident-target set (`g248TargetIndex`), so the whole
  Dungeon 6 physical-alias family shares the CPU/VRAM owner. Diagnostic; restores the `0x139`
  alpha channel but does NOT fix the darkening.
- `DC2_G400_STAGE=1`: G400 in-frame `0x139`/`0x068` sampling at FRAME-target transitions
  (`DC2_G400_FROM`/`DC2_G400_TO`/`DC2_G400_TICKS`/`DC2_G400_STEP`). **Weak instrument** — hooks the
  CPU draw entry points, which this route mostly bypasses (see G400 §5).
- `DC2_G391_HD_DUMP=1` / `DC2_G391_MIX_DUMP=<path>`: Bounded Sony `.HD` parameter hexdump / raw
  s16le 44.1 kHz stereo capture of the SFX mixer bus.

---

## Known Issues (ACTIVE)

1. **Low performance due to incomplete native renderer (CLOSED PERF ARC)**
   - **Status**: Native-renderer stack (G260–G298) is DEFAULT-ON (~13.5 fps / ~74 ms MAP-0 floor). Total closure perf arc CLOSED at G352 (accept-floor). Master rollback: `DC2_G26X_NO_NATIVE=1`.
   - **Current Thread Pole**: MAP-0 frame ~74 ms/13.5 fps. VU1 worker (~74, true wall ~66–68, MTVU default-on since G302) and GS worker (~68 incl 5ms collect-stall) are **co-balanced ~74 ms** (G344 re-profile); EE idle (~24% busy).
   - **Milestone 3 Status (Pillar-4 Rearch)**: G310 logical atlas DEFAULT-ON (`DC2_G310_LOGICAL=1`). **G338 resident CT24-alpha view PROMOTED DEFAULT-ON at G352** (`DC2_G338_CT24_VIEW`; kill `DC2_G338_NO_CT24_VIEW=1`; bit-exact, GS worker −3.8 ms/f, frame −2.6% VU1-capped).
   - **G178 GPU Depth Parity Stepping-Stone**: `DC2_G242_GPU_DEPTH=1` persistent GPU-depth bridge is opt-in only; native-renderer stack G260–G352 covers rendering performance and depth authority.
   - **Arc Total-Closure Findings (G340–G352)**:
     - G340 lazy-VRAM: NO-GO (0.0% skippable).
     - G341 parallel-VIF DMA chain pre-decode: NO-GO (GS front-end pole is Path2 DIRECT = 53 ms 96% IMAGE upload bytes, not parallelizable geometry decode).
     - G342 shader-side deswizzle / resident views: NO-GO (relocated readback row-exact to T8 consumer).
     - G343 L2L exact consume-floor slice: NO-GO (readback-upload conservation, frame regressed +8%).
     - G344 MTVU window-coalescing: NO-GO (handoff overhead is constant 0.64 ms/f; 93% of `collectMs` is VU1 wait stall).
     - G349 persist-one-FBO & G351 surface-split: NO-GO (`uploadFb` gates on shared physical-page gens).
   - **Post-arc presentation & stage fixes (G353–G368)**:
     - G353 (WEAVE present): Interlace field bob removed (`DC2_G353_FIELD_BOB=1`).
     - G358 / G359 (Sindain circle): MTGS readback sync (`g150_wait_idle()`, G358) + `BITBLTBUF.SBP` 14-bit block units (`sbp = img.vram_addr`, G359) FIXED Known Issue #2.
     - G361 (Static inits): `mwInit` run from boot (`f55_boot_init_stub`) FIXED un-run `__sinit_*` root.
     - G362 (Point sampling): `sampleExact` 12.4 integer UV reconstruction FIXED Items-menu cuts/notches.
     - G363 (Spheda HUD): `GetFullPath` shim preserves `CurrentDir` prepend FIXED missing HUD textures.
     - G364 (Distance fog): GS per-pixel FOG stage (`GS_REG_FOGCOL`, `PRIM.FGE`) applied in all draw paths.
     - G368 (MAP-160 tear): DISPFB stride fallback fix (`candidate.fbw = displayFrame.fbw`) FIXED page-row band tearing; G365 sprite coverage rule promoted back default-ON.
     - G383 (Minor residuals): `sceMcSetFileInfo` now honors `$a3` info + `$t0` flags; LoadImage `BITBLTBUF.DBP` now uses direct 256-byte block units. G194 DOF/TexAnime visual claims were not reproducible and are closed.
     - G396 (Georama retest): the documented floor-select route executes
       `DrawSub__8CEditMapFi@0x001B4130` repeatedly with zero PC-zero, recovery, stack-desync,
       or bad-dispatch events. The old `ra=0` issue is confirmed fixed by G186.
     - G397 (Dungeon 6 long route): Established Zelmite Mine free-roam benchmark; proved map/train/HUD/lighting match HW, texture/CLUT uploads match 100%, GS draw states match. Added `tools/g397_draw_state.py`. **Its "ticks 1,740–7,620 are intro frames" claim is WRONG — corrected by G398.**
     - G398 (Dungeon 6 onset): Break pinned to host tick **~1,770**, the FIRST free-roam frame after the `FLOOR EPISODE` banner clears — tick 1,750 is pixel-correct vs `ref/dumps/dungeon_6.png`. Guest state is identical across the transition and identical to PCSX2 (`DngStatus=0`, `lightScale=1.0`, `mapIdx=0`). What changes is that DC2 turns on its second render target: pre-break frames draw only `fbp=0x139`; free-roam frames add the `fbp=0x068`/`fbp=0x000` post-process + composite that reads `0x139` back as `tbp=0x2720 tpsm=PSMCT32`. GS command-stream parity with `ref/dumps/dungeon_6.gs` proven down to vertex colours (both `fbp=0x139` blend-volume passes `ALPHA=0x…62` subtract / `0x…68` add carry `RGBA=(1,1,1,64)` on HW and runner; composite sprites `0x503c005d`/`0x503c0061`). Refuted `DC2_G364_NO_FOG`, `DC2_G203_LEGACY_Z`, `DC2_G144_NO_TILEBIN`. Added `tools/run_g398_dungeon6.ps1` + `tools/g398_frame_census.py`.
     - G399 (Dungeon 6 stage split): added the `DC2_G399_SURFDUMP` raw render-target probe
       (`ps2_gs_gpu_parts/gpu_g399_surface_probe.inc`, default-off) and measured each stage
       separately. **G398's either/or is wrong — both stages lose.** At the break latch the scene
       RTT `fbp=0x139` itself drops meanL 39.2 -> 21.9 with meanA 178.8 -> 32.6, *and* the
       `0x068`/`0x000` composite ratio falls 0.97 -> 0.13 with `0x068` maxL hard-capping at 97
       while `0x139` still reaches 255. Structural find: DC2's free-roam post-process renders to
       `fbp=0x13b/0x13c/0x14b` (blocks `0x2760`/`0x2780`/`0x2960`) and reads back
       `tbp=0x2820/0x2920/0x2d20/0x2e20` — all **inside** the `0x139` footprint
       (`0x2720`–`0x3420`), i.e. the G304 hard multi-owner physical-alias class, switched on by
       exactly the frame that breaks. Refuted: GS `COLCLAMP=0` wrap (DC2 uses `COLCLAMP=1` here,
       matching the runtime's unconditional clamp) and draw-ORDER divergence in the `0x139`
       light-volume prologue. Added `tools/g399_draw_order.py`.
     - G400 (Dungeon 6 ownership A/B): the `fbp=0x139` **alpha** collapse is rooted — it is the
       residency split (`DC2_G400_NO_RESIDENT_139=1` restores meanA 29.9 → 160.3 vs 178.8
       correct); excluding the other four resident targets as well changes nothing. But the
       **luminance/darkening is NOT the native renderer**: `DC2_G26X_NO_NATIVE=1` (whole stack
       off) is *darker* than the default (display meanL 1.83 vs 2.55; hardware 34.3), so no
       residency/FBO/page-authority experiment can close it. **CORRECTS G399 §3.1**: at the break
       latch `0x139` holds no scene at all (black + Max only), so present-latch `0x139` stats are
       a latch-phase artifact and not a valid stage-1 oracle — only `0x068`/display numbers are
       phase-safe. Refuted: G287 transient target `0x13b`, G338 CT24-alpha view, and any
       composite-command-stream divergence (identical states/texture bases at full-frame scale).
       Next: measure `0x139` at COMPOSITE time, then audit T8 decode/CLUT for the world's PSMT8
       pages `0x3420`–`0x3660`.
     - **TRAP (G400): `captures/g398_bad.gs` is TRUNCATED** (1,706 batches vs ~9,500 for a real
       free-roam frame) and stops before the composite sprites — it reads as "the runner never
       emits the composite". Use `captures/g398_bad2.gs` (27,765 batches) and always check the
       total batch count before believing a "missing draw class" result.
     - **TRAP (G399): `tools/g398_frame_census.py` had a shifted GS register map** — `DIMX` is
       `0x44`, `DTHE` is `0x45`, `COLCLAMP` is `0x46`; it read `COLCLAMP` from `0x1F`, which is not
       a GS register, so every census before G399 reported `colclamp=0` for every dump. Fixed. Do
       not cite `colclamp`/`dimx`/`dthe` from any pre-G399 census output.
     - **TRAP (G398): on this route compare arms by GUEST STATE (`[F59:dump] DngStatus=0` + settled free-roam), never by host tick.** Disabling a perf lever changes pacing several×; `DC2_G144_NO_TILEBIN=1` read as a "fix" at ticks 2,400/6,000 while still in the map screen / entrance cutscene, and renders mean L = 0.02 (fully black) once a 420 s soak actually reaches free-roam.
   - Detailed specs & fix-logs: [arc-native-renderer.md](file:///d:/ps2r/dc2/plans/arc-native-renderer.md), [arc-total-closure.md](file:///d:/ps2r/dc2/plans/arc-total-closure.md), [phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md), and [phase-G352-fix-log.md](file:///d:/ps2r/dc2/plans/phase-G352-fix-log.md).

---

## Routes for Graphic Test

Each row: reference dump → exact input route → harness → re-check triggers. exact input route → harness → re-check triggers.

| Reference (`ref/dumps/`) | Route (debug-menu unless noted) | Harness | Re-check when touching |
|---|---|---|---|
| `correct_light.{gs,png}` | Debug menu → Down×2 → Circle → Square → Left → Cross (dungeon-0 map/light) | `tools/run_g237_capture.ps1` | **VU1 scalar-pipeline timing** |
| `dungeon_1_cutscene.{gs,png}` | Debug menu → Down×2 → Right → Circle → Cross×2 (Wood entrance) | `tools/run_g240_capture.ps1` | Alpha-test (`AFAIL`), blend, Z-write |
| `map_2_zoom.{gs,png}` | Debug menu → Down@0 → Right@15/30 → Circle@43 + zoom replay | `tools/run_g221_map2_zoom.ps1` | Rasterizer texture-interpolation (STQ) |
| `map_0.{gs,png}` | Debug menu → Down (~tick 15) → Circle (~tick 94) (town/MAP-0) | `tools/run_g194_map0.ps1` | Town geometry, Z, tile-bin/MTGS |
| `map_4.{gs,png}` | Debug menu → Down → Right×4 → Circle (town/MAP-4) | `tools/run_g204_map4.ps1` | VIF1 DMA chain-tag walker, shared-page clear |
| `map_4_zoom.{gs,png}` | Recorded route: Down@0 → Right@15/30/43/59 → Circle@77 | `tools/run_g218_map4_zoom.ps1` | Zoom-mode composite / RTT |
| `dungeon_0_cutscene.{gs,png}` | Debug menu → Down×2 → Circle → Cross×2 (dungeon-0 entrance cutscene) | `tools/run_g223_dungeon0_entrance.ps1` | Cutscene camera/render path |
| `dungeon_0_gameplay.{gs,png}` | Same route, post DngStatus 2→0 (free-roam) | `tools/run_g223_dungeon0_entrance.ps1` | Free-roam camera / lighting |
| `emptey_room.{gs,png}` | Debug menu → Down → Left → Circle×3 (empty room, Max falling) | `tools/run_g226_emptyroom.ps1` | VU0 stubs, DA/accessory physics |
| `items.{gs,png}` | Debug menu → Down → Circle → Triangle → Cross | `tools/run_g361_items.ps1` | Boot static-inits (`DC2_G361_NO_SINIT`), 2D UV |
| `Inventory.{gs,png}` | Town → Down → Circle → Triangle×3 | `tools/run_g189_inventory_ab.ps1` | Sprite defer, pipelining |
| `ttle.{gs,png}` | Held New-Game menu OR debug menu → Down×3 → Circle | `tools/run_g100_cap.ps1` | VU1 MAC/flags, culling, Z, presentation |

---

## Reusable Technical Knowledge

### 1. Rendering Fixes & Sprite Sampling (G5, G6, G8, G362)
- **Split VIF1 IMAGE continuation (G6)**: VIF1 PATH2 IMAGE continuation qwords must be forwarded verbatim (`ps2_vif1_interpreter.cpp`) — never re-wrap in second IMAGE tag.
- **Debug-menu PSMT4HH font (G5)**: Packed 4-bit stream; stop at `TRXREG` rectangle, not oversized QWC request. CLAMP repeat semantics.
- **2D UI sprite point-sampling (G8, G362)**: DC2 UI text/icons are point-sampled (`tex1=0x201`, MMAG/MMIN=0). `drawSprite` FST bias is 0.0. `sampleExact` (G362) uses 12.4 fixed-point UV reconstruction (`lround(u*16)/16`) on `uMode` bit 12 to prevent float `floor()` rounding errors across atlas boundaries.
- **DISPFB Latch (Kept by Design)**: `f29_mgendframe_probe` DISPFB1/2 force-write is a presentation timing crutch for the half-rate title loop behind `DC2_FORCE_DRAW_BUFFER_LATCH=1`.

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
- Stubs using single-precision float ABI (`f12`/`f0`) return stale `$v0`, corrupting math without crashing (e.g. pot velocity flying along `x==z` diagonal). Use `ps2GetDoubleArg`/`ps2SetDoubleReturn`.

### 5. EE Float Arithmetic & Saturation (G371, G372)
- R5900 COP1 FPU has no Inf, no NaN, no denormals — saturates every result to `±0x7F7FFFFF`. `x/0` returns max finite value with operand signs; `sqrt` uses absolute value.
- NaNs/Infs in guest state indicate host IEEE overflow. Clamped via FPU macros (`FPU_ADD_S`, `FPU_SUB`, `FPU_MUL`, `FPU_DIV_S`, `FPU_SQRT_S`).
- COP2/VU0 FMAC ops are clamped by MAC-flag block. VU0 Q ops (`VDIV`/`VSQRT`/`VRSQRT`) and COP1 denormals are fixed via post-regen scripts.

### 6. Boot & Static Initializers (`__sinit_*`, G361)
- Metrowerks static initializers (`__sinit_<file>.cpp`) run via `.ctors` pointer table at `0x00374D80–0x00374E40` walked by `mwInit@0x00100190` called by `init__Fv@0x0015C160`.
- Nop'd `init__Fv` skipped all static initializers. `f55_boot_init_stub` runs the ctor table once before `InitCDFile`. Index 5 (`__sinit_mainloop.cpp`) stays excluded; `DC2_G361_SINIT_ALL=1` is diagnostic only.
- G382 closed the proposed missing User-Data/New-Game follow-up: `MainLoop@0x190CB0` calls `CSaveData::Initialize`, `InitSaveData@0x1908A0` repeats it, and TitleLoop's New Game return-5 branch calls `InitSaveData` again. At MapJump, index-5 A/B hashes are identical across the active-save/CUserDataManager pages.
- Full index 5 is unsafe in this manual boot position: it changes surviving scene/villager placement seed state and moves deterministic MAP-0 from `(362.53,12.68,960.28)` to origin. Keep the narrow F50.4 MainScene-vtable repair; do not carry forward the false assumption that save initialization is missing.
- Diagnostic rule: zero state on a global struct with valid positions indicates BSS zero from un-run static ctor.

### 7. GS Transfers & BITBLTBUF (G383, G359, G358)
- Block pointers in `BITBLTBUF.SBP`/`DBP` are 14-bit fields in 256-byte block units. Guest TBP0 is ALREADY in block units — pass directly, do NOT scale by `*8`.
- MTGS readback contract: GS local→host readback (`sceGsExecStoreImage`) MUST call `g150_wait_idle()` before consuming bytes.

### 8. Recompiler & Codegen Technical Rules
- **`.inc` recompile trap (G359)**: MSBuild does not reliably rebuild when an included `.inc` changes. Always edit included `.cpp` (`GS.cpp`/`ps2_gs_gpu.cpp`) to force recompile.
- **Nested jump-table coverage (G395)**: `EventSelect__Fv@0x001923B0` uses the seven-entry
  inner table at `0x00365080`. Automatic inference omitted targets 2-6 and crashed when selector
  2 reached the valid internal label `0x001929C0`. Keep the full table explicit in
  `config_dc2_final.toml`; never stub its internal labels as standalone functions.
- **MSBuild SelectedFiles static-library trap (G395)**: building `dc2_game` with
  `/p:SelectedFiles=...` repacks `dc2_game.lib` with only the selected object. Do not use this as
  a normal incremental static-library build. Build the explicit Release target normally; if a
  surgical object compile is unavoidable, repack and verify the complete object set before
  linking the runner.
- **`DIRECT_JAL_ONLY_TARGET` (G223)**: Cannot be traced via `registerFunction`; wrap jal target instead.
- **Back-edge preemption context safety (G186, G211, G212)**: Recompiled bodies unwound by preemption must suppress preemption during wrapper execution (`g_dc2PreemptSuppressDepth`).
- **VU1 scalar prestall (G239)**: Lower scalar stalls happen BEFORE either half of instruction pair executes; decode lower word first.
- **VU1 FDIV delay (G200)**: FDIV busy-stalls second DIV/SQRT/RSQRT until first result latches into Q.
- **Stack argument decoding (G194, G374, G378)**: R5900 EE ABI passes first 8 integer args in `$a0–$a3` and `$t0–$t3`. Stack reads (`readStackU32(16)`) for 5th fixed arg are wrong; stack args start at `sp + 0` in 8-byte slots.
- **Universal Z emulation (G203)**: Honor guest ZBUF/TEST universally, never whitelist per-screen.
- **`vf0` initialization (G40)**: Pin `vu0_vf[0] = (0,0,0,1)` after context memset.
- **VU1 Q latency (G87)**: Latches Q after fixed delay (7 cycles for DIV/SQRT, 13 for RSQRT).
- **G193 instrumentation caveat**: `DC2_TRACE_G58=1` re-registers `Draw__8mgCFrame@0x137E10` bypassing `g67_frame_draw_probe` distance cull repair. Trace runs lack G67 repair. Long recompiled bodies print exit trace early on unwinding back-edge preemption.
- **Guest C heap location (F58)**: Guest C heap MUST sit ABOVE guest `_end` (`[F58:setupheap]` non-zero).

### 9. Off-EE-Thread Guest-Execution Lock (G377, G379)
- **Invariant**: Any host thread that runs recompiled guest code (e.g. INTC/DMAC IRQs, VSync callbacks, Alarm threads) MUST hold `PS2Runtime::GuestExecutionScope`.
- `m_guestExecutionMutex` is a recursive mutex and `g_guestExecutionDepths` is thread-local, so taking the scope defensively on a thread that already holds it is free. Blocking syscalls release the lock via `GuestExecutionReleaseScope` (release-on-wait).

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

### 12. FMV/PSS Audio (G384/G393)
- DC2 PSS private-stream audio starts with a four-byte substream selector (`ff a0 00 00`), then Sony `SShd`/`SSbd` ADS data.
- `RUSH.PSS` is codec 1 PCM16LE, 48 kHz stereo, with 0x200-byte planar blocks per channel. Deinterleave each channel block to LRLR samples before host playback.
- G393 implements the ADS/SS2 format rule used by FFmpeg: codec 1 is planar PCM16LE; every
  non-1 codec value is planar-interleaved Sony PSX ADPCM with predictor history per channel.
  A direct ISO census found all 53 retail DC2 PSS entries use codec 1 (48 kHz, stereo,
  0x200-byte channel interleave), so PSX ADPCM is generic completeness rather than a changed
  DC2 retail route. Kill only G393 ADPCM with `DC2_G393_PCM_ONLY=1`; decoder proof:
  `DC2_G393_MPEG_AUDIO_SELFTEST=1` (4/4).
- `MPEG.cpp` preserves guest callbacks while streaming supported PCM/ADPCM through raylib.
  Kill all host FMV audio with `DC2_G384_NO_MPEG_AUDIO=1`; trace with
  `DC2_G384_MPEG_AUDIO_TRACE=1`.

### 13. Gameplay SFX and Sequenced BGM (G385)
- DC2 SIF DMA sends Sony `.HD` metadata and `.BD` VAG bodies; EZMIDI `SetHD` associates the pair with a sound port.
- `SE_Play@0x189C10` emits program/volume/key-on messages through MODMSIN. Its generated handlers were dead return-zero stubs; G385 resolves their program/key split against the captured bank and submits decoded VAG audio.
- BGM `SetSq` payloads start with `SCEISequ` and use variable-delta MIDI-like channel events plus Sony loop markers. G385 renders the captured sequence against its HD/BD instruments and loops the PCM through one host stream per port.
- Kill with `DC2_G385_NO_GAME_AUDIO=1`; trace with `DC2_G385_AUDIO_TRACE=1`.
- Voice uses a separate `ezBgm@0x28AE20` RPC service (SID `0x12345`); G386 implements that path.
- **A bank body is NOT one DMA transfer (G387).** Large `.BD` payloads arrive as several
  consecutive transfers on the audio DMA descriptor. Concatenate them and let `SetHD`'s requested
  `bodySize` select the chunk-aligned run; capturing only the first transfer silently loses the
  whole bank and yields a BGM stream that plays perfect silence. Diagnostic:
  `[G385:bank] commit-miss` with `requested` > `pending`. Kill `DC2_G387_NO_BD_CHUNKS=1`.
- **Silent-BGM oracle:** `[G385:bgm] committed ... notes=N misses=N` (all notes missed) means the
  bank at that port is missing/empty — look upstream at bank capture, never at the renderer.
- **SFX voice allocation (G390).** A `FD 10` MODMSIN message with velocity 0 is a **key-off** — do
  not play it (it was 98% of all `[G385:sfx] resolve-miss` lines). `PS2AudioBackend` allows 24
  concurrent sounds (SPU2 per-core voices), lets a bank sample retrigger over itself, and never
  applies the duration-based "this is BGM, stop everything" branch to bank samples (key
  `0x01000000 | slot<<16 | vagIndex`). Kill `DC2_G390_LEGACY_SFX=1`.
- **Audio volume scales (G389).** EZMIDI port volume (`ezMidi(port|0xb0, v)`) is **0..0x100**:
  `sndSetPortVol__Fif@0x18D420` computes `f*127`, then `SetVol__6CSoundFii@0x18A2C0` multiplies by
  `256/127` unless the value is the `0x100` unity token. Divide by 256, never 127. That port gain
  attenuates SFX one-shots as well as the sequenced stream. Voice is a separate scale:
  `sndStreamSetVol__Fff@0x190410` packs `f*32767` per channel into `ezBgm(port|0x80, L<<16|R)`.
  Kill `DC2_G389_LEGACY_MIX=1`.
- **One bank can serve several ports (G388).** `SetHD` carries `(hdAddr=arg1, bdAddr=arg2,
  size=arg3)`; DC2 repeats the same triple for a second port (SFX port + battle-BGM port) without
  re-uploading. The last commit is cached by that triple and re-bound (`commit-alias`).
  `commit-miss pending=0x0` ⇒ unknown identity (G388 shape); `commit-miss pending>0 chunks>1` ⇒
  capture gap (G387 shape). Kill `DC2_G388_NO_BANK_ALIAS=1`.
- **A one-shot needs a VOICE, not a `Sound` (G391).** Envelope, loop, pan and key-off were four
  symptoms of one cause: `PS2AudioBackend::play()` is fire-and-forget. DC2 bank SFX now run through
  a 24-voice 44.1 kHz mixer (`ps2_audio_parts/dc2_g391_sfx_mixer.inc`).
  - **Sony `.HD` sample parameter is 42 bytes**: `+0` vagIndex(u16), `+2/+4` velocity range,
    `+11` baseNote, `+12` detune, `+16` volume, **`+18` ADSR1**, **`+20` ADSR2** (LE, SPU2
    semantics). Pan is `split+17` and `programParam+7` (`0x40` = centre). Verify with
    `DC2_G391_HD_DUMP=1`; reading ADSR one byte lower yields `Ar=0x7f` for everything (the tell).
  - **MODMSIN hs message = `FD <sub> 00 <key> <id> <value> <extra>`**: sub `0x10` play
    (`value` = velocity; `value == 0` is `SE_Stop`'s key-off), `0x00` SetVol, `0x01` SetPan,
    `0x02` SetPitch (14-bit `value | extra<<7`). `F9 01 <v>` is the channel volume; **`F9 00 <v>`
    is still unidentified and is NOT pan** (0..0x48, never 0x40).
  - **Reverb is guest-driven**: `SetReverb__6CSoundFiii@0x188940` → `sceSdRemote` `0x8130`
    (`mode|0x100` at attr+4), `0x8070` core|2 (enable), `0x8010` core|0xb80/0xc80 (echo volume =
    `depth<<8`). Captured in `Kernel/Stubs/Audio.cpp`. The tail network is an approximation; the
    routing is not. BGM does not pass through the mixer, so it takes no reverb send.
  - **Never peak-normalise a rendered sequence** — relative track loudness is composed data. One
    fixed bus gain (0.85) + soft knee.
- **SE parameter stream order (G392, corrects G385/G391).** `sndSePlaySeID(id, seNo, prio, vol,
  pan=0x40, pitch=0x2000, ...)` -> `sndSePlayPrKr` -> `sndSePlayPBPrKr@0x18F350` -> `SE_Play`,
  which emits `F9 00 <vol>`, `F9 01 <pan>`, `F9 02 <pitch14>`, then the `FD 10` key-on. **`F9 01`
  is the PAN, not the volume** — G385 had them swapped, so every bank SFX was scaled by its pan
  (~0.5) and its real volume was dropped. Oracle: the `F9 01` stream is 0x40-heavy (66/131 exactly
  centre); `F9 00` never hits 0x40. Kill `DC2_G392_LEGACY_SE_MIX=1`.
- **BGM shares the SFX bus (G392).** Sequenced BGM is a source inside the G391 mixer (it renders at
  44.1 kHz = bus rate), so it takes the same reverb send the hardware would give it and the whole
  mix passes one soft limiter. Kill `DC2_G392_NO_BGM_BUS=1`.
- **Cold-title BGM ordering (G394).** `TitleLoop@0x29FFA0` stops the old BGM before loading and
  playing its replacement, but the older zero-volume/Stop pair can arrive after the new Play in
  the host queue. Suppress only that paired stop inside the replacement's first 100 ms; a mute
  which survives the window still applies. Roll back with `DC2_G394_LEGACY_ASYNC_STOP=1`.
- **Reverb is the real SPU network (G392/G393).** Same/different-side IIR + 4-tap comb + 2 all-pass at
  22050 Hz over a work area; `m*`/`d*` registers are addresses in 8-byte units (= 4 samples).
  G393 recovered DC2's exact nine register sets and work-area spans from retail
  `MODULES/LIBSD.IRX` `.data`: ROOM at file offset `0x4278`, then 0x44-byte records through PIPE
  at `0x4498`. This corrected ROOM taps, the PS2 ECHO/DELAY layouts, and PIPE mapping. These are
  exact preset inputs; the host floating-point network is still not claimed waveform bit-exact.

### 14. Voice Streaming (G386)
- Voice filenames such as `0010665.wav` resolve through 16-byte `SOUND.HD3` entries: name offset, byte size, `SOUND.DAT` sector offset, and sector count. Read only the referenced 2048-byte sectors; do not extract or scan the full 970 MB archive.
- Exercised assets are standard RIFF/BWF PCM WAV: mono, 48 kHz, 16-bit. They are not VPK/VAG and do not use the G385 HD/BD decoder.
- `StreamOpenFast@0x18AEF0` reaches `ezBgm@0x28AE20` SID `0x12345`. Captured command families cover open (`0x8020`), standby/mode/attribute, play (`0x50`), volume (`0x80`), state (`0x80b0`), level (`0x80e0`), stop, and close.
- `sgCPlayVoice::Step@0x304730` requires state exactly `0x1000` while playing, then a non-playing value to complete. G386 derives that transition from decoded frame count/sample rate.
- Repro: `tools/run_g386_voice.ps1`; add `-Control` for the same-binary `DC2_G386_NO_VOICE=1` arm.
- Kill with `DC2_G386_NO_VOICE=1`; trace with `DC2_G386_VOICE_TRACE=1`.

---



## Game-Specific Bugs (original-game defects — clamp at the runtime boundary, never fix game-side)
- **4HH/4HL oversized transfer** (`mgLoadTextureZ@0x145400`): size `(w*h*bpp)/16` = 8× too large.
  Real HW survives the DMAC overrun; the runtime consumes packed 4-bpp data and stops when
  `TRXREG` is full (G5). 4HH hides UI/font data in the upper 8 bits of the 24-bit Z-buffer,
  routed via GIF Path 3.
- **Intro movie `IsStarted` spin** (IPU/MPEG): bypassed headless (IOP/audio/MPEG dead).

## Runtime Gaps (PS2Recomp missing features for this game)
- **VU1 interpreter FMAC flag PIPELINE not modelled (OPEN, G71).** PCSX2 delays the MAC/STATUS/CLIP flags
  several instructions (an FMxx flag-read sees the result of an op a few cycles earlier); `ps2_vu1.cpp` uses
  an IMMEDIATE model (the lower op of a pair reads the same pair's upper-op flags). Moot for the title
  (its cull is a structurally never-taken branch, not flag-dependent) so not yet fixed. **Revisit only if a
  flag-gated VU1 program (FMEQ/FMAND/FMOR/FCAND -> IBxx) misbehaves in a way the immediate-timing model
  explains.** Related OPEN: optional VU1 float clamp `DC2_VU1_CLAMP` (denormal/overflow, see
  `plans/Possibles_bugs.md` B2) — correct but kept off pending broad testing.
- VU0 helpers in `Kernel/Stubs/VU.cpp`: `sceVu0InversMatrix`/`CameraMatrix`/`ScaleVectorXYZ`/
  `InterVectorXYZ` IMPLEMENTED; `sceVu0ecossin`/`InterVector`/`LightColorMatrix`/`MulVector` … still
  `TODO_NAMED` — implement from `ref/assembly.txt` if a route hits one (matrices row-major float[16]).
  **G233 (2026-07-12): audit every implemented `sceVu0*` stub against the real VU0 microcode
  semantics, not the C signature** — `sceVu0Normalize` is a 3-component ESADD normalize (its
  4-len reimplementation flipped the DA collision push-out → missing chest gem); any stub whose
  real body uses ESADD/dot-3 idioms can carry the same w-lane bug.
- BIOS pseudo-files such as `rom0:ROMVER` should not be treated as normal host files; missing support can produce noisy fopen/fio errors, but do not prioritize it unless the game actually consumes the result or blocks on it.
- **No R3000A/IRX execution / cooperative IOP scheduler** remains as a general architecture gap.
  G385 bypasses the absent
  EZMIDI/MODMIDI modules for captured Sony HD/BD SFX and SQ BGM, and G386 bypasses EZBGM
  for captured WAV voices and their completion state. Current upstream `ps2xIOP` is itself HLE:
  its README explicitly says it does not emulate the R3000A, load IRX files, or execute IRX
  code. Upstream PR #154 parametrizes SoundDriver RPC/status HLE; it does not add guest IOP
  execution or sample rendering. Therefore merging it cannot close this architectural gap.
  All tracked Audio routes are nevertheless implemented: G384/G393 handle PSS audio, G385/G386
  handle gameplay/voice protocols, G393 statically recovered retail libsd reverb tables, and
  G394 protects replacement BGM from an older queued zero-volume/Stop pair.
  Do not carry this general runtime limitation as an open Audio issue.
- **No cooperative thread scheduler ABBA-safe wait helper** → threading deadlock/starvation at
  thread hand-offs (F49.5/F50 class). Partial fix APPLIED (lightweight post-wake yields in
  SignalSema/SetEventFlag/WakeupThread/ReleaseWaitThread). Fix reference: upstream #120
  `waitWithGuestExecutionReleasedUntilUnlocked` + full #137 fiber scheduler (high-risk, would
  obliterate MTGS — deferred).
- **VU0 micro-mode programs do not execute** (only inline COP2 macro mode works) — dormant today
  (DC2's skinned models use recompiler-inline COP2 macro ops, not VU0 micro-mode) but would surface
  as wrong physics/lighting on a VU0-micro route. Fix reference: upstream #120 (incomplete upstream,
  don't adopt yet).
- **IPU/MPEG movie decode/output**: **FIXED (G375/G376/G384/G393)** — FFmpeg-backed video plus
  host-streamed PSS PCM/PSX-ADPCM audio; `RUSH.PSS` video and sound verified against
  `ref/dumps/fmv.png` and `ref/audio/fmv0.webm`.
- **GS VRAM addressing is bespoke per-fix, not consolidated** — not a defect (DC2's GS is correct,
  heavily validated G2-G52) but a maintenance divergence from upstream #132. Do NOT splice #132
  wholesale (would overwrite G3/G5 swizzle/CLUT/RTT/Z/costume fixes); mine only a specific proven
  hunk if a matching bug appears.
- **Sparse EE library-function / IOP module coverage** — stub on demand when a route faults
  (cheaper/safer than importing upstream #131 wholesale).
- **Recompiler indirect-jump (JR/JALR) fallback gaps** — recompiler-side, matters at next regen.
  #128 (fallback codegen + Rabbitizer formatting) ALREADY ADOPTED into PS2Recomp source (G152) but
  NOT regenerated into live `recomp/` (a temp regen showed massive churn, didn't recover `0xe3dc70`,
  rejected). #150 (jal-only entry discovery) may help the `0xe3dc70` blocker at a future regen.
- Full per-PR adoption table + rationale (2026-06-22, re-surveyed 2026-07-07): `plans/pr_change.md`
  and the archived detail in `plans/phase-history.md`. **Net as of 2026-07-11: no upstream PR
  merged into live behavior beyond the pre-existing #120+#136.**

## Recompiler regen caveat
- Allocator-family coherence is mandatory after TOML/stub changes.
  Do not split C/newlib allocation functions between runtime stubs and recompiled ELF bodies. 
  If `malloc`/`_malloc_r` route to `PS2Runtime::guestMalloc`, then `memalign`/`_memalign_r` and the matching `free`/`_free_r`/`realloc`/`_realloc_r`/`calloc`/`_calloc_r` family must be runtime-backed or explicitly proven compatible. 
  A mixed allocator can allocate from one heap and free/reuse through another, causing silent alignment/memory corruption bugs — especially texture/CLUT/menu corruption. 
  After any regen or TOML edit, audit the full allocator family in the TOML, generated stubs, and Ghidra symbol map before trusting graphical symptoms.
- Stable texture corruption after allocator fixes usually means "next-layer GS/texture bug," not random memory noise. 
  If a screen changes from noisy/unstable corruption to repeatable texture blocks, wrong palettes, menu artifacts, or large blue/black regions after allocator alignment fixes, treat the allocator issue as likely reduced but not proof that rendering is correct. 
  Next suspects should be CLUT upload/cache invalidation, TEX0/TEXA state, texture-page addressing, PSM/CPSM mismatch, TEXFLUSH, and Z-write/background-clear behavior. 
  For menu-heavy routes, compare against PCSX2 and log PSM/CPSM/CBP/CSA/TBP0/TBW/TEXFLUSH/ZBUF/TEST before chasing geometry or game state.
- Do not merge a regenerated TOML until allocator-family routing is coherent. 
  Either all allocation entry points route to the runtime allocator, or the full newlib allocator path is intentionally recompiled and proven compatible. 
  Avoid half-runtime / half-recompiled allocation paths; they can cause silent alignment bugs, CLUT/menu texture corruption, heap metadata corruption, or non-deterministic crashes

## Technical Symbols & Upstream PR Cross-Reference Index
- **Boot C++ Runtime Inits**: `mwInit@0x00100190` walks `__initialize_cpp_rts` table (`0x00374D80–0x00374E40`).
- **GS Display Stride**: `copyDisplaySource` fallback reads `candidate.fbw = displayFrame.fbw` (512 vs context 640).
- **`ps2_stubs` & Memory Card Stubs**: Hand-written stubs (`sceMcGetInfo`, `sceMcGetDir`) in `Kernel/Stubs/` audited against R5900 EABI `$t0–$t3` integer arg layout.
- **Float Saturation**: Host float division replaced with `copysignf(INFINITY, ...)` in `tools/fix_cop1_ps2_float.py`.
- **Upstream PS2Recomp PR References**:
  - **PR #170**: `ps2xIOP` HLE refactor; explicitly no R3000A emulation or IRX loading/execution.
  - **PR #154**: SoundDriver RPC/status HLE parametrization; no sample renderer or IRX execution.
  - **PR #135**: Recompiler reachable-function generation; not an IOP CPU/scheduler.
  - **PR #120**: Cooperative thread scheduler ABBA-safe wait helper (`waitWithGuestExecutionReleasedUntilUnlocked`).
  - **PR #132**: GS VRAM addressing consolidation.
  - **PR #128**: Recompiler indirect-jump (JR/JALR) fallback codegen + Rabbitizer formatting.
- **Allocator Coherence**: `guestMalloc` / `memalign` / `free` / `realloc` / `calloc` family routing integrity.

---

*For full historical details of past phases, see [phase-history.md](file:///d:/ps2r/dc2/plans/phase-history.md).*
