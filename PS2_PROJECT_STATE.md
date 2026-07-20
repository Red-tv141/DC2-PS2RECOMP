# PS2 Recomp Project State — Dark Cloud 2

> **General, durable, forward-useful knowledge only** — operating rules, workspace/build facts,
> currently-open issues (as short facts, not phase narrative), and cross-cutting technical
> knowledge likely needed again regardless of which phase is active (pad-input protocol, PCSX2 A/B
> protocol, regen caveats, generalized "lessons learned"). **This file does NOT contain:** active/
> next-phase status (`plans/ROADMAP.MD`), native-renderer architecture (`plans/arc-native-renderer.md`),
> per-lever measurement tables, refuted-hypothesis lists, closed-arc digests, or any other
> per-phase-specific detail — that all lives in `plans/phase-history.md` (grep it) or the specific
> `plans/phase-GXX-fix-log.md`. **If you need to know what a specific lever does, what was tried
> and refuted, or the history of a closed arc — check phase-history.md or the fix-log, don't expect
> it here.** When an issue is resolved, remove it from "Known Issues" entirely (the fix-log is the
> permanent record; do not maintain a "Resolved" index in this file). Last restructured 2026-07-18
> (2nd pass): moved the Resolved index, Current Levers table, Refuted/Do-Not-Re-chase list,
> Title-3D-arc digest, and single-investigation technical dumps (CDynamicAnime struct offsets,
> dungeon/event state machines, front-end state machine, PCSX2 cutscene pause point, C++ exception
> model, floor-select input) to `plans/phase-history.md`. 

## Quick Rules
- **NO PER-SCREEN FIXES (hard rule).** Do not patch a symptom by writing
  game state per-frame/per-draw scoped to one screen (e.g. forcing `TitleProjection`/camera/renderinfo in
  the title scope). Such writes leak through shared globals into CONCURRENT screens (the G93 title fix
  mislocated the costume Max model — front-end title+costume share state, G79). Diagnose to the ROOT and
  fix it at its source (the state machine / init / data path that is actually wrong), once, where the game
  itself would set it — not as a scoped band-aid. If a scoped lever is needed to PROVE a diagnosis, gate it
  opt-in (default-off) and never ship it as the fix.
- Build with `cmake --build <build_dir>` only; no clean targets, no build-dir deletion.
- Do NOT modify/create files in `runner/`, or modify standard headers. Split complex non-runner/override logic (e.g. `dc2_game_override.cpp`) into `.inc` files inside source subdirectories (e.g. `ps2xRuntime/src/`), never in the project root.
- Do NOT use destructive git commands. Keep diagnostics env-gated, quiet by default.
- Build via the **PowerShell tool**, not Bash `cmd /c` (silently no-ops → stale exe).
  Verify a change landed: `grep -c <marker> build64/Release/dc2_runner.exe`.
- Renderer promotion requires direct normal-output review against available hardware references.
  Internal oracles/counters do not prove downstream composition; MAP-2 left-cliff loss passed
  G284's internal range census until `ref/dumps/map_2_zoom.png` was compared side-by-side in G287.
- Write a `plans/phase-<ID>-fix-log.md` before ending each executable phase.

### G-series phase scoping
A G phase may include discovery, but only bounded discovery tied to ONE foundation defect. Start
each with: (1) the exact defect / rule-out question, (2) the smallest known repro, (3) the
PCSX2-vs-runner comparison point if needed, (4) clear stop conditions. No new gameplay/route
phases unless they reduce uncertainty for the active foundation defect. Write a
`plans/phase-GXX-fix-log.md` before ending each executable phase — `plans/ROADMAP.MD` only ever
gets a one-line "Active Phase" entry for the CURRENT phase, never a duplicated narrative. When a
phase closes, move its narrative to `plans/phase-history.md` in the same session (or the next
session's first act) and compress its `PS2_PROJECT_STATE.md` "Resolved" entry to one line.

### Standard phase checklist
- Use the local PS2Recomp skill at `D:/ps2r/dc2/skill/SKILL.md`. Obey this file.
- Performance phases follow the "Aggressive performance policy" below: the opt-in arm may render incorrectly during bring-up, but the default path must remain clean; keep one architectural variable per arm, repair discovered parity failures in the same phase, never promote incorrect output.
- Verify perf/render changes by FULL-FRAME DISTRIBUTION (multiple frames) + visual review, never a single golden sample alone (G150, G168, G174 all caught real bugs a single sample missed).
- For anything touching threading/pipelining (MTGS worker, G157 pipeline, G144 band-replay), soak with `DC2_FRAME_DUMP_EVERY=1` (dense per-tick dumping) — the default 60-tick cadence is too coarse to catch transient (few-frame) races (G174).
- When bisecting a perf lever (G144/G178/G150/G157/G172) on a NON-title route, remember the frame-dump filename is the HOST TICK, not the guest scriptFrame — disabling GPU/tile-bin levers can make the CPU path several× slower in wall-clock time, so a fixed `-Seconds` budget from a title-only harness may not reach the same guest state; increase it and cross-check via a state trace (e.g. `DC2_TRACE_F59`) (G223).

### Aggressive performance policy (user-approved 2026-07-15)
The remaining gap to 60 FPS requires architectural experiments, not more low-yield barrier
micro-tuning. Performance phases may deliberately push an opt-in path until graphics diverge, then
diagnose and repair that divergence inside the same phase/arc.
- The current default path is the immutable control arm. Risk applies only to a new default-off
  environment lever; never expose normal runs to an unfinished experiment.
- Change one architectural mechanism at a time. A phase may accumulate parity fixes for that one
  mechanism, but must not combine unrelated eligibility, ordering, depth, and timing experiments
  in one A/B arm.
- Do not stop at the first broken frame. Capture the earliest divergence, classify it as geometry,
  texture/CLUT, color/blend, depth, ownership/readback, or presentation, and fix its root while
  the phase remains active. The normal three-strike circuit breaker still applies.
- Prefer same-run CPU shadow verification and exact dependency-boundary readback checks. Dense
  multi-frame review is mandatory because single goldens missed G249/G254 temporal failures.
- Measure performance throughout bring-up, but a timing result from incorrect output is not a win.
  Promotion requires the full route matrix to recover models, depth, lighting, text, textures,
  alpha cutouts, shared-page composition, and temporal stability.
- If parity cannot be restored or the architecture is neutral after parity, remove the behavior
  lever, retain only useful diagnostics, and document the rejected design in the phase fix log.

## Game / Workspace
- Title: Dark Cloud 2 (NTSC-U). Main ELF: `SCUS_972.13`. Partial recovered symbols.
- PS2Recomp repo (LIVE): `D:/ps2r/dc2/PS2Recomp`
- Game workspace: `D:/ps2r/dc2` · ISO: `D:/ps2r/dc2/Dark Cloud 2 (USA) (v2.00).iso`
- Generated output: `D:/ps2r/dc2/recomp` · Build dir: `D:/ps2r/dc2/build64`
- Runtime override (most fixes live here): `PS2Recomp/ps2xRuntime/src/dc2_game_override.cpp`
- Runtime syscalls/stubs: `PS2Recomp/ps2xRuntime/src/lib/Kernel/{Syscalls,Stubs}/*.cpp`
  (the LIVE defs; `*.inl` are /FORCE:MULTIPLE-ignored DEAD dups — do not edit those).

## Build & Smoke
```powershell
cmake --build D:\ps2r\dc2\build64 --config Release --target ps2_runtime -- /m:1
cmake --build D:\ps2r\dc2\build64 --config Release --target dc2_runner -- /m:1 /p:BuildProjectReferences=false
```
Known-benign link warnings only: `LNK4006` (getGameName) + `LNK4088` (/FORCE).
Default title smoke (**G139 2026-07-06 golden: held-menu frame_001500 `PixelNonZero=211646`**;
G138 measured ≈211650 pre-G139, pre-G138 forced-draw era was `≈633662-634384`; reproduce the old
forced render with `DC2_G100_FORCE_DRAW=1 DC2_VU1_NO_FMSWAPFIX=1 DC2_VU1_NO_MACPIPE=1`, and the
G139 beam shards with `DC2_VU1_NO_PAIRHAZ=1`):
```powershell
powershell -ExecutionPolicy Bypass -File D:\ps2r\dc2\tools\run_30s_diagnose.ps1
```

## Active Runner Command
`D:/ps2r/dc2/build64/Release/dc2_runner.exe D:/ps2r/dc2/SCUS_972.13`
(workspace `D:/ps2r/dc2`; smoke via `tools/run_30s_diagnose.ps1`.)

## 60fps Patch (perf-arc test accelerant, NOT default-on)
Raw cheat-device code `20376C50 00000001` (32-bit write of `0x00000001` to guest address
`0x00376C50`) is wired as an opt-in env-gated patch, reapplied every guest frame (matches a real
cheat-device "extended" write surviving the game's own resets of the value): set
`DC2_PATCH_60FPS=1` before launching `dc2_runner.exe`. Implementation:
`dc2_apply_60fps_patch()` in `PS2Recomp/ps2xRuntime/src/dc2_game_override.cpp`, called from the
per-frame `mgEndFrame` hook. Do all perf-arc timing comparisons with this patch consistently
ON or OFF (not mixed) — the title loop is already unthrottled/60fps by default so the patch is a
no-op there; use it on dungeon/town routes (native 30fps) for faster iteration. See "Routes for
Graphic Test" below for which route to use it on.

## Known Issues (ACTIVE)

1. Low performance due to an incomplete native renderer. Native-renderer stack (G260-G298) is
   DEFAULT-ON; CPU-raster arc CLOSED at G259. Master rollback `DC2_G26X_NO_NATIVE=1`; per-lever
   kill switches are in `plans/phase-history.md` or each phase's own `plans/phase-GXX-fix-log.md`
   — do not hand-maintain a duplicate lever list here.
   **Current pole (G310 final direct per-thread measurement): the GS worker's TOTAL cost**
   (84.40 ms mean default vs 94.93 ms G310-killed), of which `GSimage` is a subset
   (54.83 vs 63.80 ms). VU1 runs on a dedicated worker thread by default since G302 (kill
   `DC2_G297_NO_MTVU=1`), remains below the GS worker at roughly 68-73 ms, and records
   `gsStall=0`; the EE is mostly idle.
   **Milestone-2 status (G310, 2026-07-19): the first logical-vs-physical framebuffer slice is
   DEFAULT-ON.** One private 512x512 CT32 logical atlas now represents publication-order authority
   across the five physical RTT/work layouts. CT32 display consumers bind it directly; a bounded
   overwritten-alias retirement and exact resident PSMT8 views close the measured downstream
   chain. The observed `TA0=48, AEM=1` CT24 family remains on the legacy path. Exact coverage:
   251.66M warmed + 83.89M rebuilt-final pixels `bad=0`; full hardware-reference route matrix,
   996-frame dense title, and 651-frame dense Max-body gate passed. Final same-executable means:
   92.61 ms default vs 104.39 ms kill (-11.29%, +12.7% FPS), with run-median ranges separated.
   Rollback `DC2_G310_NO_LOGICAL=1` or `DC2_G310_LOGICAL=0`; exact verifier
   `DC2_G310_VERIFY=1`; stats `DC2_G310_STAT=1`. G309 remains a default-off oracle/substrate.
   **G311 (2026-07-20): batched page-delta atlas maintenance is a NO-GO** (premise gate). The
   composite is already cheap (~230.8 µs) and there is no page-level temporal coherence (~107/128
   pages change every refresh, `noop=0`), so the incremental path — built + proven bit-exact
   (`bad=0`/334.8M px) — is neutral-to-regressive (306 µs/refresh, fragments into ~5-20 round-trips).
   Kept default-off substrate `DC2_G311_INCREMENTAL=1` (kill `DC2_G311_NO_INCREMENTAL=1`); census
   `DC2_G311_CENSUS=1`, oracle `DC2_G311_VERIFY=1`. The G310 default is unchanged (title `211650`,
   MAP-0 full Max verified). **Next is G312:** bounded re-profile of the GS-worker's own compute
   buckets (register replay vs raster vs CT32 pack) to find a new bounded sub-term or confirm the
   remaining entry is a re-architecture; all bounded composite/async/pipeline slices are exhausted.
   Architecture design + full pillar/lever history: `plans/arc-native-renderer.md`. Latest fix-log:
   `plans/phase-G311-fix-log.md`. **Active/next phase status is tracked only in `plans/ROADMAP.MD`.**
2. Active pre-native-renderer graphical issues
- **Sindain inventory circular viewport** shows colored noise instead of a live 3D character
   portrait (`ref/dumps/inventory.png` vs `captures/g189_inventory.png`). Reachable via both debug
   shortcut and normal gameplay. Dispatch lead: `MenuMainDraw__Fv@0x234290` → `menu_drawfunctbl`
   (indexed by `MenuCommonInfo+0x54`) → per-page draw fn, not yet identified. Harness:
   `tools/run_g189_inventory_ab.ps1` / `tools/run_g189b_present_ab.ps1`. **PARKED by user during
   the performance arc (2026-07-15); regression route only, do not investigate or fix now.**
3. **Audio absent** — no music/SFX/voice; event scripts can WAIT on sound completion and stall
   (check `StreamOpenState`/voice/stream flags first). libsd/IOP path unimplemented. See Runtime
   Gaps #1 below for the upstream IOP-subsystem fix reference.
4. **Memory-card save/load** — not implemented; blocks New Game/Load + long-form testing.
5. **Interlace / field presentation jitter** — DISPFB/SMODE2/PMODE/CSR; presentation, not
   geometry; compare PCSX2 deinterlacing first.
6. **FMV / movie playback** — IPU/MPEG/IOP; only prioritize if a movie wait blocks progression.
7. **(RETEST after G186)** georama `DrawSub__8CEditMapFi` "null-vtable" crash
   (`[dispatch:pc-zero] from=0x1b4340 ra=0x0`) — G186 showed this `ra=0x0` signature is the
   F50.7 sentinel leak, NOT a null vtable; likely fixed by the G186 preempt-suppression, but
   never explicitly retested. May be entangled with item 1 (same georama/EditMap family).
8. **G193 instrumentation caveat (durable):** `DC2_TRACE_G58=1` re-registers `Draw__8mgCFrame
   @0x137E10` with a canary calling the RAW recompiled body — bypassing `g67_frame_draw_probe`'s
   distance-cull repair from the default table. G58-traced runs lack the G67 repair. Also:
   wrappers around LONG recompiled bodies (EditInit, Initialize__6CScene) print their ".exit"
   trace EARLY when back-edge preemption unwinds them mid-body (G186 mechanism) — treat exit
   snapshots as mid-execution values.
9. **G194 town DOF wedge** — dark triangular wedges from `DepthOfField@0x17E320`; bisect-proven
   (`DC2_G194_SKIP_DOF=1` removes them) but final root (raster-semantics/pyramid-content level)
   never found. Low priority.
10. **G194 TexAnime L→L rect-width divergence** (`1×k` vs real HW's `64×k`+`64×(64-k)` wrap
    pairs) — documented, unfixed, low priority.
11. **Regen caveat (durable):** see Reusable Knowledge → "Recompiler regen caveat" (COP2 dest-mask
    reapply + `ps2_recomp` stale cache + VU0 un-stub collisions) before any future regen.
12. **DISPFB latch kept BY DESIGN (F52, not a bug):** the `f29_mgendframe_probe` DISPFB1/2 force-write
    is a headless presentation-timing crutch for the half-rate title loop; do NOT diagnose a black
    frame as a swap/buffering bug. Legacy mode behind `DC2_FORCE_DRAW_BUFFER_LATCH=1`.
13. **Guest C heap must sit ABOVE guest `_end` (F58, RESOLVED, verify if `bad_alloc` reappears):**
    check `[F58:setupheap]` non-zero.
14. **Max foot shadow** missing (MAP-0/MAP-4, G202-era residual, never resolved).
15. **G178 GPU depth parity** — `DC2_G242_GPU_DEPTH=1` persistent GPU-depth bridge is opt-in only
    (`PS2_PROJECT_STATE.md`/`plans/arc-native-renderer.md` GPU stepping-stones): its dirty
    ownership invariant fails on the title (G249), blocked on finding the missing CPU-write
    barrier. Not on the critical path (native-renderer stack G260-G289 covers perf instead).


## Routes for Graphic Test

**Rule for the upcoming perf arc: accuracy beats speed.** Any change to VU1 timing, the GS
rasterizer (CPU or GPU path), tile-binning, MTGS/threading, or universal-Z/alpha-test semantics
must be re-verified against the routes below before being called done — pixel-count golden alone
is NOT sufficient (see "Golden pixel-count alone as a promotion gate" in ROADMAP's Refuted list).

**The 60fps patch (`DC2_PATCH_60FPS=1`, see Build & Smoke) is a TEST ACCELERANT, not a target
state.** The title loop already runs unthrottled/60fps by default — the patch is a no-op there.
Dungeon/town routes (MAP-0 etc.) run at the game's native 30fps without it, so use the patch on
those routes when you want faster wall-clock iteration; always spot-check at least one route with
the patch OFF too, since it writes a live guest word every frame and must not be assumed neutral
for a route that hasn't been checked.

Each row: reference dump → exact input route → harness → which planned perf-lever changes should
re-trigger a re-check of that route.

| Reference (`ref/dumps/`) | Route (debug-menu unless noted) | Harness | Re-check when touching |
|---|---|---|---|
| `correct_light.{gs,png}` | Debug menu → Down×2 → Circle → Square → Left → Cross (dungeon-0 map/light view) | `tools/run_g237_capture.ps1`, `tools/run_g239...` A/B via `run_g237_vugs_ab.ps1` | **VU1 interpreter timing/scalar-pipeline changes — HIGHEST PRIORITY**, this route is what caught the G239 scalar-prestall bug |
| `dungeon_1_cutscene.{gs,png}` | Debug menu → Down×2 → Right → Circle → Cross×2 (Rainbow Butterfly Wood entrance cutscene) | `tools/run_g240_capture.ps1` | Alpha-test (`AFAIL`)/blend/Z-write changes, G242 universal-Z GPU-depth bridge |
| `map_2_zoom.{gs,png}` | Debug menu → Down@0 → Right@15/30 → Circle@43, then hold byte-swapped replay R2=`0x0002` from scriptFrame 212 (real zoom appears near dump tick 1200) | `tools/run_g221_map2_zoom.ps1`, `tools/run_g222_map2_kink.ps1` | CPU/GPU rasterizer texture-interpolation (STQ) changes — the G222 hyperbolic-vs-affine bug only showed on this route's large deep triangles |
| `map_0.{gs,png}` | Debug menu → Down (~tick 15) → Circle (~tick 94) (town/MAP-0) | `tools/run_g194_map0.ps1` | VU1 Q-pipe/FDIV timing (G200), universal guest-Z (G203), any tile-bin/MTGS/GPU-raster lever — cheapest non-title 3D route, and the one that's genuinely 30fps without the patch (good for A/B'ing the patch itself) |
| `map_4.{gs,png}` | Debug menu → Down → Right×4 → Circle (town/MAP-4) | `tools/run_g204_map4.ps1` | VIF1 DMA chain-tag walker (G217), shared-page-clear regressions (G220) |
| `map_4_zoom.{gs,png}` | Recorded route: Down@0 → Right@15/30/43/59 → Circle@77, then hold byte-swapped replay R2=`0x0002` from scriptFrame 212 (real zoom near dump tick 1200) | `tools/run_g218_map4_zoom.ps1` | Same as `map_4`, plus zoom-mode composite/RTT changes; verify Max/HUD disappear or the route did not enter zoom |
| `dungeon_0_cutscene.{gs,png}` | Debug menu → Down×2 → Circle → Cross×2 (dungeon-0 entrance cutscene, mid-cutscene frame) | `tools/run_g223_dungeon0_entrance.ps1` (default `-Sweep` covers this) | Cutscene camera/render path, DngStatus=2 draw scope |
| `dungeon_0_cutscene_end.{gs,png}` | Same route, final shot ≈ frame_016020 (need `-Seconds`/sweep extended past the script default) | `tools/run_g223_dungeon0_entrance.ps1` with a longer `-Seconds` | Event-completion timing (G225), cutscene-to-freeroam handoff |
| `dungeon_0_gameplay.{gs,png}` | Same route, after DngStatus 2→0 (free-roam begins) | `tools/run_g223_dungeon0_entrance.ps1` (extended) | Free-roam camera/lighting (G224), general dungeon 3D render |
| `emptey_room.{gs,png}` | Debug menu → Down → Left → Circle×3 (empty room, Max falling — DA physics/pendant repro) | `tools/run_g226_emptyroom.ps1` | VU0 stub changes (`sceVu0*` family), DA/accessory physics — lower relevance to the GS/tile-bin perf arc unless it touches VU0 |
| `Inventory.{gs,png}` | Town → Down → Circle (open menu) → Triangle×3 (inventory tabs, scriptFrame~172/198/232) | `tools/run_g189_inventory_ab.ps1` | Sprite-defer (G172)/pipelining (G157) levers — this route already has a KNOWN bug (G241, circular viewport noise); use it for regression-only checks on the rest of the menu, not as a pass/fail gate for G241 itself |
| `ttle.{gs,png}` (title cavern) | Either the non-debug golden route (held New-Game menu) OR debug menu → Down×3 → Circle | `tools/run_g100_cap.ps1`; gross golden `PixelNonZero=211646+/-4` at `frame_001500`; **dense temporal gate from f60 onward** (G249) | Any VU1 MAC/flag-pipeline, clipper, draw-list/culling, deferred-order, depth-ownership, or presentation change. One golden frame is insufficient; G249 caught a G242 regression only through the multi-frame gate. |

**Suggested check order for a perf-arc change that touches VU1 timing/GS raster/threading broadly:**
1. `ttle` golden pixel count (cheap, catches gross regressions).
2. `correct_light` (VU1 scalar-timing canary).
3. `dungeon_1_cutscene` (alpha-test/blend canary).
4. `map_2_zoom` (texture-interpolation canary).
5. `map_0` / `map_4` / `map_4_zoom` (general town/dungeon geometry + Z + chain-tag).
6. `dungeon_0_cutscene` / `_cutscene_end` / `_gameplay` (general dungeon render + camera).
7. `emptey_room`, `Inventory` (lower priority — VU0/known-broken respectively).

## Reusable Knowledge (verified this phase unless noted)

### Rendering fixes DONE (G5/G6/G8) — durable facts only; full narrative in `plans/phase-history.md`
- **Split VIF1 IMAGE continuation (G6, FIXED):** VIF1 PATH2 IMAGE continuation qwords must be
  forwarded verbatim (`ps2_vif1_interpreter.cpp`) — never re-wrap them in a second IMAGE tag.
- **Debug-menu PSMT4HH font (G5, FIXED):** `tbp=0x1a00 tbw=8 psm=0x2c cbp=0x3fe0` is a packed
  4-bit host-to-local stream (low nibble then high nibble); stop at the `TRXREG` rectangle, not
  the oversized `0xD000` QWC request. Sampler honors GS `CLAMP`/`REGION_CLAMP`/`REGION_REPEAT`
  (the debug font needs `CLAMP=0` repeat, its U coords exceed the declared 512 width).
- **2D UI sprite sampling (G8):** DC2 menu/HUD/debug fonts are POINT-SAMPLED
  (`[G8:sprite] tex1=0x201`, MMAG/MMIN=0) — never add bilinear to "fix" UI blur. `drawSprite`'s
  FST sampling bias is 0.0 (left-edge), not +0.5. Residual UI softness vs a real PS2's upscaler
  is a known cosmetic gap (would need an internal 2D supersample pass), not a bug to chase.
- DC2 menu/HUD text+icons are PSMT8 palette textures uploaded via a CT32-aliased BITBLT
  (`DPSM=PSMCT32`, `TBW=2×DBW`, CLUTs in CT32) and sampled as native T8 — do NOT re-touch
  `GSMem`/`addrPSMT8`/`addrPSMCT32`/CLUT for text without re-reading the full G5/G6/G8 history.

### Pad input — the LIVE path and the ANALOG stick (F66)
- **The live pad read is `read_pad_stub`** (registered at `0x0014A490`, overrides
  `read_pad__FP10PAD_STATUS`) → `dc2_write_pad_status` (`dc2_game_override.cpp`). It
  writes the button mask to `PAD_STATUS+0` and the four analog ints
  (`+4`=LY,`+8`=LX,`+0xc`=RY,`+0x10`=RX). **`scePadRead` (Pad.cpp AND
  ps2_stubs_misc.inl) is DEAD — the game never calls it** (`[F66:padcall]`=0); editing
  it / `setPadOverrideState` is inert for input. `PAD_STATUS` = `CGamePad+0x04`
  (`UpDate__8CGamePad@0x14a930` calls `read_pad(CGamePad+4)`), so `CGamePad+0xc`=LX,
  `+0x8`=LY (`GetLX/GetLY`: `-0x80` centre, ±`0x32` deadzone).
- **In-dungeon free-roam movement is the LEFT ANALOG STICK**, not the D-pad:
  `RunScript__12CActionChara`→`Analog__11CPadControl(0x3d7b60, 4/5)`. A digital-only
  injector navigates menus but never moves the player.
- **Headless input clock dies in-dungeon:** `f40_drive_pad`'s `scriptFrame` comes from
  the **mgEndFrame OVERRIDE** call count, but the dungeon calls mgEndFrame via a direct
  `jal` that bypasses the hook → input stops at `scriptFrame≈697`. For in-game input,
  drive from the **host present loop** (`ps2_runtime.cpp`), not mgEndFrame.
- **`DC2_DUNGEON_PAD`** (F66): same `start..end:Alias[+Alias];…` syntax as
  `DC2_PAD_INPUT`, evaluated by `f66_drive_dungeon_pad` off the present loop with a
  counter that starts at free-roam entry; emits buttons + a deflected left stick
  (Up `0x10`→ly`0x00`, Down→ly`0xFF`, Left→lx`0x00`, Right→lx`0xFF`).
- Addresses: `MainChara` ptr @ **`0x003772C8`** (gp-0x7228, gp=`0x0037E4F0`), player
  world pos @ `MainChara+0xe0`, vtable `0x3756f0`. `DebugPause` @ **`0x00377288`**
  (gp-0x7268). Movement gate (`DngMainKey` exe `0x1d1d…`): `DAT_01e9f6e8==0 &&
  DebugPause==0 && (BattleAreaScene+8 & 4)==0`. **G223 note: `DAT_01e9f6e8` is also
  the sentinel `InitEyeCamera__FP12CActionChara@0x1D3D10` unconditionally sets to 1 near
  its end — reading it as 0 throughout free-roam is proof InitEyeCamera never ran.**

### Real controller input — XInput via raylib (G7)
- A connected host gamepad now drives the game live (buttons + both analog sticks).
  Backend = raylib's gamepad layer (XInput on Windows via GLFW), exposed by the free
  fn `dc2_poll_host_pad(allowKbd, mask, lx,ly,rx,ry)` in `src/lib/ps2_pad.cpp`
  (active-high scePad mask + 0x80-centred axes, radial deadzone ~0.20; triggers also
  assert L2/R2). Polled once per present frame by `g7_poll_live_pad()` (global scope in
  `dc2_game_override.cpp`, called from the `ps2_runtime.cpp` present loop) → publishes
  the snapshot `g_pad_live_*` consumed by `read_pad_stub`/`dc2_pad_mask`/
  `dc2_write_pad_status`.
- **Input source priority:** `DC2_NO_XINPUT=1` (off) → explicit `DC2_PAD_INPUT`
  (scripted, deterministic tests — suppresses live) → connected gamepad or
  `DC2_KEYBOARD=1` (live OWNS buttons + all four axes) → F40/F66 scripted default.
- `run_30s_diagnose.ps1` now sets `DC2_PAD_INPUT` explicitly so a plugged pad can't
  perturb golden smoke. Trace: `DC2_TRACE_PAD_INPUT` → `[G7:live]`.
  cross-thread snapshot reads are unsynchronised scalars (≤1 frame stale, like F66).
  Rumble not wired (raylib gamepad has no rumble API). See `plans/phase-G7-fix-log.md`.
- **`DC2_RSTICK` (G49) — scripted RIGHT-STICK for headless tests.** `DC2_PAD_INPUT` only injects
  the 16-bit button mask (both analog sticks stay centred), so it cannot rotate the Select-Costume
  model preview (which turns with the RIGHT analog stick). `DC2_RSTICK` uses the same
  `start..end:Dir[+Dir];...` range syntax, `Dir` ∈ {`RRight`,`RLeft`,`RUp`,`RDown`}; driven off the
  F40 scriptFrame path, applied in `dc2_write_pad_status` when no live pad. Default centred (inert).
  Harness `tools/run_g49_rstick.ps1`. (`f40_get_rstick_events`/`g_f40_rx,ry` in `dc2_game_override.cpp`.)

### Debug-menu dungeon route
`DC2_DEBUG_MENU=1` writes `DebugFlag@0x00376FB8` (gp-0x7538); navigate with
`DC2_PAD_INPUT='90..99:DebugDown;130..139:DebugDown;170..179:DebugConfirm'` → reaches the
floor-select treemap (`DngStatus=4`). Add `;260..266:Down;300..306:Down` to confirm a
floor and reach the 3D draw. Frame capture: `DC2_FRAME_DUMP=1` → `captures/frame_NNNNNN.ppm`
every 60 ticks (`ps2_frame_dump.cpp`). **The dump filename is the HOST PRESENT-LOOP `tick`
counter, NOT the guest scriptFrame** (G223, 2026-07-11 correction) — cross-check which tick is
post-transition via `DC2_TRACE_F59=1`'s `[F59:dump] tick=... DngStatus=...` line before trusting
a sweep frame number, especially across different perf-lever configs (wall-clock/scriptFrame
ratio is NOT constant — disabling G144/G178 can make the CPU path several× slower in real time).
Harness: `tools/run_f56_dungeon3d.ps1` (older, coarse) or `tools/run_g223_dungeon0_entrance.ps1`
(newer, includes the `DC2_TRACE_F56/F57/F59` state traces by default).

### Recompiler regen caveat (before any future regen, e.g. with the trimmed toml)
- Configs are inputs to a MANUAL `ps2_recomp` run, NOT referenced by the runtime build.
  `ref/config_auto_recomp.toml` = trimmed v11 (54 stubs + 10 skips removed → recompile);
  `ref/config_auto_recomp_F56baseline.toml` = v9, closest to the CURRENT working binary
  (rollback insurance); `patch/config_auto_recomp.toml` = older v3.
- `ps2_recomp.exe` build cache hard-references the stale path `d:/ps2r/PS2Recomp` — fix +
  rebuild the recompiler before regenerating.
- **After any regen, re-apply the F51.8 COP2 dest-mask reversal** (`tools/fix_cop2_destmask.py`)
  to the regenerated files, or all dungeon 3D perspective transforms regress to degenerate.
- Un-stubbing VU0 helpers (e.g. `sceVu0InversMatrix`) collides with runtime impls in
  `Kernel/Stubs/VU.cpp` (/FORCE picks one) — validate the correct winner per function.

### PCSX2 A/B (DebugServer raw TCP)
`127.0.0.1:21512`, newline-delimited JSON. cmds: status / evaluate{expression} /
read_memory{address:int,size} / set_breakpoint{address} / resume / clear_breakpoints;
poll `status.data.paused` after resume. Reusable: `tools/run_f56_pcsx2_ab.ps1`,
`tools/run_f55_pcsx2_ab.ps1`. Convention: leave PCSX2 paused, breakpoints cleared.


## Learned Patterns (durable cross-phase wisdom)

Full mechanism/evidence for any entry below: `plans/phase-history.md` "ARCHIVED
PS2_PROJECT_STATE.md 'Learned Patterns' DETAIL as of 2026-07-14", or the phase's own fix-log.

**Threading / diagnostics**
- **G281:** removing one exact read-side publication does not pay when a later overlapping
  writer still needs guest-authoritative physical pages: `mat(tex) -205` became
  `mat(cpu) +205` exactly. Count total publication causes, not one bucket. A read view and a
  write view are different ownership mechanisms; never "classifier-widen" a write whose
  destination pages overlap several dirty FBOs without ordered fan-out/page authority.
- **G281:** PS2 formats can share physical pages without sharing within-page geometry.
  The safe CT32-to-T8 view begins with the real `AddressP8`, inverts CT32 word layout, selects
  the exact byte lane, and applies the live CLUT. Do not carry CT32 64x32 tile equivalence into
  T8/T4/Z. Also, retained shorthand said TRI_STRIP while the live shape was SPRITE: current
  consumer-edge census outranks phase prose.
- **G267:** a 2-D sampled page set cannot be safely collapsed to one min/max GS block interval
  when that interval grants GPU residency permission. A narrow texture column spanning many page
  rows made the generic range falsely include every intervening CT32 page column (`alias=0x1c`);
  enumerating actual sampled pages found the one real alias (`0x146`, page `0x2920`). Use coarse
  ranges for fail-closed barriers/census, exact page/address sets for admission.
- **G267:** eliminating a named consumer drain does not imply less synchronization. The correct
  TRISTRIP direct-bind prototype had to publish one aliased sibling and then publish the source at
  its lifecycle boundary; total `mat(tex)` stayed at control cadence. Count the whole downstream
  materialization graph after every ownership repair, and remove a behavior arm that only relabels
  the work even when its final composition is correct.
- **G265:** exactness work can erase an architectural win even when it fixes the right contract.
  The first 8-bit alpha repair ran integer TFX arithmetic on every GPU fragment and collapsed the
  matched frame-time gain to a 1.45% median. Keep exact post-TFX alpha, but branch the integer work
  on the uniform alpha-test mode; the final 3x3 recovered an 8.59% median reduction. Rebenchmark
  after every parity repair, and discard timings from any superseded executable.
- **G265:** a short dense control can miss legitimate animated title states. Candidate `211640`
  frames initially looked unique; an extended same-binary predecessor control reached the same
  value in the same two intervals and the same edge/overlay pixels. Compare complete temporal
  windows and inspect the presented frames; do not relax a gate merely because a difference is
  small, and do not call a control-established timing phase a regression.
- **G264:** GS page aliasing means a source upload rectangle is not generally a target-surface
  rectangle. Map physical byte addresses through both CT32 layouts and retain an exact target-pixel
  mask. Equal DBW, aligned bases, and row bounding boxes are unsafe assumptions; coalescing gaps can
  overwrite dirty FBO pixels from stale VRAM. On patch failure, keep the newer VRAM upload and drop
  residency without a readback.
- **G260**: deferring a formerly-INLINE draw class silently drops every side-channel the inline
  path performed — here the drawPrimitive-end `g178NoteVramWriteRect` page-generation bump, whose
  absence froze GPU-cached textures of RTT pages into a persistent missing-body-parts dropout.
  Before deferring/recording any draw class, enumerate what its inline path does BESIDES
  rasterize (gen bumps, scope latches, presentation hints) and replicate them at the equivalent
  execution point — scoped to what consumers actually key on: over-scoping the replicated bump
  (all pages instead of RTT-family) churned the fb-snapshot generations and gave back an entire
  measured win (459 ms vs 448 control).
- **G260**: a faster wrong arm can masquerade as an architecture win — the pre-fix graph arm was
  +15% purely from stale-texture cache hits (frozen generations skipped fb re-uploads/decodes).
  Treat any perf delta measured before parity as an upper bound contaminated by skipped work.
- **G260 (anon-namespace linkage, 3rd+4th instances)**: an `extern` declared INSIDE an anonymous
  namespace (even inside a function body) binds to a new internal symbol — LNK2019 with an
  `anonymous namespace` mangled name, or C2668 ambiguity once a matching global decl exists.
  Cross-TU symbols in `ps2_gs_rasterizer.cpp` need a global-scope bridge/fwd-decl OUTSIDE the
  file's large anonymous namespaces (which wrap most of its interior, including the G178 section).
- **G258**: CPU `std::lround` and GPU interpolated `floor(x + 0.5)` can disagree at a mathematically
  exact fixed-point half tie because raster interpolation arrives a few ULPs low. Stabilize the
  conversion boundary narrowly and verify dense output; do not change the later integer sampler or
  blend without first proving it diverges. Also, a subtarget shadow oracle does not prove final
  consumer composition: inspect non-verification output, and completely remove shader-resource
  probes that perturb graphics even when their runtime branch is false.
- **G257**: a newly created level-0-only OpenGL texture is incomplete under the default mipmapped
  min filter; even an explicit `texelFetch` then returns the incomplete-texture value `(0,0,0,1)`.
  Set a non-mipmapped min filter before sampling. To distinguish source-sampler failure from an
  image-store failure, independently prove scratch coverage, direct texture mutation, and
  direct-vs-FBO readback agreement before changing shader arithmetic or synchronization.
- **G223**: a symptom identical with two independent rendering backends disabled
  (`DC2_G144_NO_TILEBIN=1` + `DC2_G178_NO_GPU=1`) is a cheap signal the bug is GUEST-CODE state, not
  the render pipeline — A/B the big backend switches before building new GS-level probes.
- **G223**: frame-dump filename is the HOST PRESENT-LOOP tick, not guest scriptFrame — not
  comparable across runs with different perf levers; cross-check via `DC2_TRACE_F59`, and budget
  much longer wall-clock time for any A/B that disables a major perf lever.
- **G189**: `fprintf`/stderr tracing on a hot multi-threaded path can HIDE the bug it's meant to
  catch (I/O cost shifts thread timing out of the starvation window). Use relaxed `std::atomic`
  breadcrumbs on the hot path + a separate watchdog thread that only prints once staleness is
  detected. If adding a trace makes a reproducible bug stop reproducing, that's itself a signal of
  timing-sensitivity (contention/starvation), not a plain logic error.
- **G189**: an unthrottled producer/consumer ping-pong between two threads can starve a THIRD
  thread's access to a shared lock with zero deadlock (MTGS's per-frame EE↔GS-worker handoff
  starved the present thread's `GS::m_stateMutex` acquisition on cheap/idle screens — classic lock
  convoy). Soak-test any frame-pipelining change on an IDLE screen, not just content-heavy ones.
- **G189 fix**: when rarely-written/read state shares a lock with a hot high-frequency-write path,
  give it its OWN dedicated mutex instead of throttling the hot path (the host-presentation
  snapshot didn't need `GS::m_stateMutex`'s protection against `writeRegister()`'s per-register
  calls, it just happened to share the lock).
- **G206**: an old per-draw instrumentation technique (VIF1-packet-qword-delta) can silently stop
  discriminating once a later architectural change (G144/G157/G178 deferred pipeline) decouples
  "call happened" from "effect landed synchronously" — re-validate any old probe against one
  known-good/known-bad case under the CURRENT default pipeline before trusting it.

**Recompiler / codegen**
- **G221**: `cmake --build ... --target dc2_runner -- /m:1 /p:BuildProjectReferences=false` (this
  file's own prescribed recipe) can SILENTLY skip recompiling/relinking after a `recomp/*.cpp` edit
  (that dir builds into `dc2_game.lib`, a project reference the flag skips checking) — MSBuild still
  prints success while linking a STALE lib. **After editing `recomp/`, build `dc2_game` as its own
  explicit target FIRST** (look for a real "Compiling..." line) before building `dc2_runner`, and
  drop `/p:BuildProjectReferences=false` that session. Distinct from (but can combine with) the
  `runner/` shadow-duplicate trap (`ps2recomp_runner_glob_shadow_trap`, see memory).
- **G223**: a `DIRECT_JAL_ONLY_TARGET` function can't be traced via `registerFunction` (bypasses
  dispatch) AND may have a `runner/` shadow blocking a `recomp/` edit — check both before planning
  an in-function probe; prefer reading state the function WRITES from an already-hookable caller.
- **G211/G212**: the G57/G186 swallowed-resume bug class, generalized. Any wrapper that calls a
  recompiled body/vtable slot DIRECTLY and afterwards mutates `ctx->pc`/`ctx->r[31]`/guest memory
  breaks under back-edge preemption (~25% of recompiled bodies have a resume checkpoint) — output
  never written (G211: `GetLWMatrix` all-zero ~25% of frames) or a swallowed resume (G186). Fix:
  preempt-suppress the call window (`g_dc2PreemptSuppressDepth`), check `ctx->pc` before post-work,
  or re-drive to completion (`f50_run_guest_call`, gold standard). AUDIT ANY NEW OVERRIDE against
  this rule; full 133-wrapper audit: `plans/phase-G212-fix-log.md`.
- **G239**: VU1 lower scalar stalls happen BEFORE either half of the instruction pair executes
  (e.g. `MULq VF2.x,VF2,Q | WAITQ` observes the Q published by WAITQ; an upper-then-lower
  interpreter feeds it stale Q instead). Same rule for WAITP and busy FDIV/EFU producers. Fixed
  default-on in `ps2_vu1.cpp`; kill `DC2_VU1_NO_SCALAR_PRESTALL` / `DC2_VU1_NO_PPIPE`. Durable
  rule: decode the LOWER word and model its stall/pipeline publication before reasoning about
  what the paired upper reads.
- **G200**: a single-slot "pending + delay" model of a pipelined unit DROPS results on
  back-to-back ops — real VU1 FDIV busy-stalls the second DIV/SQRT/RSQRT until the first result
  latches into Q. Fix: commit the in-flight pending value the moment a new producer issues. Kill
  `DC2_VU1_NO_QSTALL`. Discriminator worth reusing: when the runner draws HW-marked-ADC=1
  geometry, compare PER-VERTEX OUTPUT COORDS first — matching coords rules out matrix/EE-data
  instantly and pins the divergence to gate-flag evaluation instead. Also documented-but-unfixed:
  DC2's interpreter wrongly updates MAC flags on MINI/MAX and wrongly skips them on ACC-writing
  ops — fix only against a concrete repro (G200 fix-log has the pair-by-pair audit).
- **G194**: a runtime stub reading args 5+ from the o32 STACK is silently wrong on DC2 (EABI: args
  5-7 in `$t0/$t1/$t2`) — `sceGsSetDefDBuff` seeded wrong Z format for months with no crash. Grep
  stubs for `readStackU32(16` as the bug signature; fix via regs-first/stack-fallback decode.
- **G203**: emulate Z like HW — honor guest ZBUF/TEST UNIVERSALLY, never whitelist per-screen. DC2
  clears Z every frame via a full-screen sprite; the runner's `drawSprite` had no Z code so the
  clear never populated VRAM Z, and each screen-scoped Z whitelist that papered over this leaked
  into other screens (g202's town scope stomped the inventory's font staging). Fix: add guest Z to
  `drawSprite` + honor guest Z universally. **"Wrong depth on ONE screen" is almost never a new Z
  scope — it's a missing piece of universal GS emulation.**
- **G242**: GPU depth must remain coherent with authoritative guest VRAM across mixed CPU/GPU flushes. Validate one common ZBUF configuration, upload/read back every touched row union, preserve PSMZ24 packing through ReadZ24/WriteZ24, invert guest/GL rows, and keep unsupported formats or alpha-write semantics on CPU replay.
- **G240**: alpha-test failure controls framebuffer and Z writes INDEPENDENTLY per `AFAIL` mode
  (KEEP/FB_ONLY/ZB_ONLY/RGB_ONLY) — classify the final fragment alpha once and gate both
  sprite/triangle outputs from that result; don't let a color discard hide the decision from a
  later unconditional Z store.
- **G186**: an override calling a recompiled body directly around a register-sentinel/post-call
  fixup breaks under back-edge preemption (sentinel leaks into the resumed chain, pc-zero recovery
  restores PC but not `$sp` → stack corruption). See ROADMAP's Refuted list "Preempt pattern" /
  "Recovery context" entries for the durable rule.
- **G140**: a STALE BAND-AID can be the "missing feature" — the title's missing water pool was the
  VU1 clipper force-broken by the old G64 "enable fix" (an IAND ORed the plane mask into what's
  actually FCGET clip flags, emptying every Sutherland-Hodgman pass). When retiring a root fix's
  band-aids, sweep OLD pc-scoped interpreter patches too — G64 predated its root fix (G138) by 70
  phases. `plans/phase-G140-fix-log.md`.
- **G139**: VU1 SAME-PAIR upper→lower VF hazard — a lower op can NEVER see its same-pair upper op's
  result on real VU1 (~4-cycle FMAC latency); the interpreter's immediate-commit model broke a
  store-then-clobber idiom (title tri packer) into "beam shard" garbage. Fix (default-on): snapshot
  the upper's dest, expose the OLD value to execLower, overlay masked lanes after. Any "positions
  garbage but ADC/fog fine" signature = look for this pattern.
- **G138**: the VU1 LOWER-OPCODE TABLE was wrong (FMEQ/FMAND swapped) and MAC/STATUS flags were
  read un-pipelined (real visibility ~4 pairs after the FMAC) — root of the entire title blue-void
  line; a branch that disassembly "proves" unreachable yet HW visibly takes is a sign to distrust
  the interpreter's opcode table/flag timing before the guest logic.
- VU0/VU1 `vf0` is HW-hardwired `(x,y,z,w)=(0,0,0,1)` and recompiled COP2 macro ops read
  `ctx->vu0_vf[0]` as that constant. The runtime `memset`s the context to zero and NOTHING writes vf0,
  so it stayed `(0,0,0,0)` → every matrix INVERSE (`mgInversMatrix`: `Q=vf0.w/det`, then `vmulq`)
  came out all-zero → skinned bone palettes zero → all skinned characters collapsed (G40). Plain
  multiplies (`mgMulMatrix`) and the VIF1-DIRECT map path don't read vf0, so they masked it for 50+
  phases. FIX: pin `vu0_vf[0]=(0,0,0,1)` after the context memset; re-assert after any context reset.
- VU1 MAC flags were NEVER computed (G71): `m_state.mac` in `ps2_vu1.cpp` was only ever READ (FMAND/FMEQ/
  FMOR) and never written, so every MAC-flag-gated VU1 branch evaluated against constant 0. Fix = compute
  the 16-bit MAC after each FMAC op: nibbles O[15:12] U[11:8] S[7:4] Z[3:0]; lanes X/Y/Z/W=bit3/2/1/0 (same
  as the F54 VU0 note); only DEST lanes flagged. When a VU program does FMEQ/FMAND `→ IBxx` and the branch
  never flips, suspect the flag register isn't maintained. (Caveat: a flag-gated branch can still be
  structurally never-taken for OTHER reasons — e.g. the title's `IBEQ VI10(208),VI7(0/1)` — so verify the
  operands before assuming flags are the cause.) Kill-switch `DC2_NO_VU1_MAC`.
- VU1 Q-register PIPELINE LATENCY was not modelled (G87): DIV/SQRT/RSQRT wrote `m_state.q` IMMEDIATELY, but
  real VU1 latches Q after a fixed delay (DIV/SQRT 7 cycles, RSQRT 13). Microcode that reads Q (MULq/ADDq/SUBq)
  before the latch, with NO WAITQ between, expects the PREVIOUS (pipelined) Q — the immediate model gave it the
  fresh result. This silently broke the title rock point-light attenuation (`MULq @vu 0x1850` one instr after
  `RSQRT @0x1840`) → point light ≈0 → neon-green rock. FIX: stage DIV/SQRT/RSQRT into `s_vuQPending`+`s_vuQDelay`
  (7/13), tick the delay down one per instruction word in the run loop and commit to `m_state.q` at 0, WAITQ
  commits immediately (`ps2_vu1.cpp`, both lower-op dispatch tables). Kill `DC2_VU1_NO_QLATENCY`. DURABLE: when a
  VU program does `RSQRT/DIV … (no WAITQ) … MULq` at < latency distance, it wants the OLD Q — an immediate model
  is wrong. The mixed use of `MULq|WAITQ` vs bare `MULq` in the same program is the tell.
- VU1 sibling-bug audit vs PCSX2 (`D:\ps2r\pcsx2-master`; `plans/Possibles_bugs.md`): **STATUS** register had
  the same gap (only FSSET wrote it) — FIXED (derive from MAC, coupled to `DC2_NO_VU1_MAC`). **Float clamp**
  (`vuDouble`: denormal→signed0, inf/NaN→±0x7f7fffff) is missing and DC2 DOES hit it (costume VU1 ≈0.3% of
  result lanes denormal/inf-NaN) yet renders fine; implemented opt-in `DC2_VU1_CLAMP`, no observed
  regression, kept OFF pending broad testing. **VU0 macro/COP2** flags are NOT a runtime bug — the
  recompiler emits MAC/STATUS/CLIP updates inline in generated code (`code_generator.cpp`; committed
  `recomp/GetLWMatrix…` contain the writes). Only the hand-written VU1 interpreter lacked flag upkeep.
- COP2 partial-dest dest-mask was lane-reversed (F51.8) — THE 50-phase dungeon-black cause.
  In SIMD codegen tests use DISTINCT per-lane source values; symmetric/all-ones vectors hide
  shuffle/mask defects. VU0 MAC nibble is X/Y/Z/W = bits 3/2/1/0, opposite `_mm_movemask_ps`
  (bits 0/1/2/3) — reverse before building Z/S/U/O (F54).
- VU outer-product (`VOPMULA/VOPMSUB`) rotates source pairing — NOT component-wise multiply;
  `mgPlaneNormal` component-wise `A*B−B*A`≡0 is the decisive local invariant (F51.8).
- `CFC2/CTC2` use architectural macro control indices (STATUS/MAC/CLIP=16/17/18, Q=22) — verify
  numeric instruction fields against the HW register table, not enum order (F51.8).
- After ANY regen, re-apply `tools/fix_cop2_destmask.py` or dungeon 3D transforms regress.
- `registerFunction` overrides are consulted ONLY for indirect `jr $t9`/`jalr`. A direct
  `jal <addr>` is a direct C++ call → bypasses dispatch; wrap the jal TARGET instead.
- Auto-stub ctors (`setReturnS32(ctx,0)` for a `__ct__`) silently break virtual dispatch (null
  vtable → `jr 0` pc-zero) AND non-virtual stack objects (garbage `this+0`, e.g. `mgCDrawPrim`
  → black render). Repair in `dc2_game_override.cpp` from `ref/assembly.txt`; never patch the
  generated callee. Stale committed `recomp/` can predate a toml stub-list edit — a fresh regen
  emits real bodies (same symbol → no register/header edits).
- Generated `jr $t9` tail-call can't return into a parent's mid-PC; if invoked via `jal`, the
  dispatcher prints "Function at address 0xN not found". Override to skip the trailing `jr $t9`.
- VS generator: a BARE `cmake --build build64` defaults to Debug `ALL_BUILD` → huge rebuild.
  Always `--config Release --target <ps2_runtime|dc2_runner>`. Build via PowerShell tool, not
  Bash `cmd /c` (silent no-op). Verify: `grep -c <marker> dc2_runner.exe`.

**Depth Buffer**
- A PS2 depth buffer viewed as RGB may show artificial color banding because the
  integer depth value wraps between byte channels. Some games intentionally sample
  a specific channel, often green, from packed depth data for fog/depth effects.
  When debugging odd color gradients, fog, masks, or banding, verify whether the
  source is depth-as-texture before treating it as a normal color texture bug.

**Diagnosis**
- A valid GS histogram (FRAME/ALPHA/CLAMP/ZBUF/TEST/scissor/XYOFFSET/color all expected) can't
  rescue bad guest geometry — classify triangle coverage; bad XYZ comes from upstream (F53).
- Near-null vtable dispatch (`bad=0x1`/small) can be a NULL `this`, not a garbage vtable — READ
  the call's `a0`; if 0, chase why the pointer is 0. Don't trust the dispatch `trace=` tail as a
  literal call stack (F50.4).
- An un-run `__sinit_*` leaves a GLOBAL object's embedded vtable ptr null → its virtual init
  silently no-ops. Fix by writing the vtable ptr the `__sinit` should have set (idempotent),
  so the game's own Initialize runs. Suspect whenever a global's fields look fresh/zeroed (F50.4).
- An auto-stubbed memory-pool init masquerades as a downstream ctor/vtable crash — first check
  whether the ALLOCATOR returns 0 (probe placement-new return), not the ctor (F50.1).
- "Bad geometry" can be uninitialized-data laundering — probe the FIRST producer's INPUTS before
  suspecting arithmetic (F55). A probe firing 0 times is itself a finding (F50.11).
- A host-side workaround (DISPFB rewrite, host fallback) can MASK a missing guest subsystem until
  the one uncovered path fails — verify it's still needed on the current build before preserving.
- Prove a host→guest write LANDS via the consumer's OWN address fn / passthrough readback, not a
  linear scan (misses swizzled writes) or an assumed mapping (F50.10, F56).
- A SILENT, marker-less process termination (clean raylib teardown, no crash/`_Exit` log) is a
  guest `exit()`/`abort()` — log the `exit` stub's caller `ra`+`a0` and WALK the saved guest
  return addresses up the stack to recover the abort/terminate/throw chain (F57). `a0=1` via
  `abort@0x100EA8`←`terminate`←`__ThrowHandler` = an **uncaught C++ exception**; the bad_alloc
  on the stack (`__dt__Q23std9bad_alloc@0x100560`) named it an OOM. Don't assume "never returns
  1" means an infinite wait — the loop may be aborting first.
- "X won't terminate" can be "X aborts before terminating" — verify the loop is still RUNNING
  (per-frame loop-number probe that survives loop swaps) before theorising about a stuck wait
  condition (F57: `loopNo` stayed 2, then the process exited).
- An OOM (`operator new`→NULL→`bad_alloc`) is "bogus size" OR "exhaustion" OR "the heap was
  never funded" — dump base/end/limit + per-block free/used at the failure, not just the size
  (F58: `base=end=limit=0` revealed an EMPTY window, not a too-big request). A guest that
  "mostly works" can have a dead `malloc` if its bulk data uses its own static pools — only a
  checked `operator new` exposes it.
- When the recompiled allocator (`malloc`/`_malloc_r`) is a runtime STUB routing to a host
  allocator, the guest's newlib internals (`sbrk`/bins/`malloc_extend_top`) are DEAD CODE —
  zero `sbrk` calls is the tell. Don't chase newlib arena logic; the heap is whatever the
  host window (sized by `SetupHeap`) provides (F58).

**GS / rendering**
- For asynchronous native rendering, measure packet ownership before copying anything. G299 found
  97% of MAP-0 packets were already readback-free but owned 293-517 KiB of vector data; deep copies
  would move 9-15 MiB/frame. Transfer/recycle vector allocations instead. Completion also owns the
  failure contract: retain the original command list for delayed CPU replay and publish VRAM,
  snapshots, residency, and owner tokens only through an ordered completion commit.
- G254: dependency-aware mixed-target CPU batching is not a viable current MAP-0 lever. The refined
  census predicted 93-94/512 safe, but isolated crossing kept total drain cadence/performance flat
  and produced large black regions; the triangle-widened arm caused a temporal Max dropout. Retain
  only `DC2_G254_DEP_STAT=1`, preserve all barriers, and keep
  `DC2_G252_GPU_RTT` off until single-target residency/readback ownership is proven.
- G252 disjoint-row CPU replay of the five measured RTT sprite targets is **default-on since G259**
  (kill `DC2_G252_NO_RTT_DEFER=1`): G259 revalidated it on current source at `1.957 -> 2.297 fps`
  MAP-0 (+17.4%, outside variance), re-passed the G253 matrix (`199,229,440` texels `bad=0`), and
  passed golden 211646 + an 896-frame dense title gate. All upload/FBP/local-copy drains are
  preserved. `DC2_G252_GPU_RTT` remains unproven and off. Opt-in census: `DC2_G253_BARRIER_STAT=1`
  (G252/G253/G259).
- **G259 (CPU-raster arc close):** promoting a perf lever requires a matched current-source A/B
  (multiple reps/arm to show the win is outside run-to-run variance — the G252 arm span was ±0.2%,
  control ±1.5%, and the arms did not overlap), the full graphics route matrix with the same-run
  `DC2_G248_VERIFY` texel oracle (`bad=0`), AND a dense per-tick title gate. Do NOT trust a
  single-cadence dumped frame across runs: G259's apparent title "regression" (frames ~211546-574)
  was stale Select-Costume frames a prior harness left behind because its sweep did not clear
  `frame_*.ppm` — always clear the capture dir before a title gate and confirm the frame content is
  actually the title cavern, not a leftover screen. Once profiling shows the frame is bound by
  register dispatch + `XYZ2` + flush barriers (far above 16.67 ms), further narrow CPU fast paths are
  not the primary route — that is the native-host-renderer objective.
- The `fbp=0x139/0x13c/0x143/0x146/0x155` RTT sprite family is texture-sampling/fill-bound, not
  dispatch-bound. Hoisting fixed-UV CT32/T8 sampler state plus per-draw CLUT/per-row texel-quad
  caches cuts its current MAP-0 body `132.9->96.9 ms/f`; shipped default-on in G250 with strict
  fallback and `DC2_G248_NO_FASTSPRITE=1` rollback. Re-run `DC2_G248_VERIFY=1` after any sampler,
  CLUT, TEXA, swizzle, filter, wrap/clamp, target-page, or RTT-ordering change (G248/G250).
- A host workaround keyed only by GS page/state can outlive its original route and corrupt a later
  feature that aliases the same VRAM. G37's synthetic `fbp=0x139/tbp=0x2720` costume clear caused
  MAP-4 zoom's deferred composite to sample zeros; retire redundant workarounds after their source
  repair and verify the original route, rather than adding a second per-screen exception (G220).
- T4HL/T4HH host-to-local IMAGE payloads are packed 4 bpp even though the destination nibble
  aliases a CT32 word. Consume low nibble then high nibble and stop when `TRXREG` is full;
  never infer transfer bpp from unpacked VRAM storage width (G5).
- Apply GS `CLAMP` per sampled coordinate, including bilinear neighbors: repeat masks by
  texture size, clamp uses the texture edge, region clamp uses MIN/MAX, and region repeat is
  `(coord & MIN) | MAX` (G5).
- For PCSX2 v9 `.gs` freezes, do not assume VRAM is the final 4MB of the state blob: 84 bytes
  of GIF-path/Q state follow it. The packet stream also begins after a 0x2000-byte private
  register snapshot (G5).
- **G222 (2026-07-11): a CPU rasterizer that divides S/T by Q PER VERTEX then re-multiplies
  PER PIXEL is mathematically AFFINE (the divide/multiply cancel exactly), not perspective-correct
  — even though it "looks like" a per-pixel divide is happening.** Real GS/PS2 texture mapping is
  hyperbolic: interpolate raw S/T/Q linearly across the triangle and divide ONCE per pixel
  (`u=lerp(S)/lerp(Q)`). The error is invisible on small/dense/near-planar geometry (where per-
  triangle Q barely varies) and only surfaces as visible kinking on large, deep triangles with high
  Q spread (e.g. a big floor trifan). DURABLE: any custom rasterizer's texture-coordinate math
  should be audited for this exact affine-disguised-as-perspective pattern if a similar "texture
  seam bends at a triangle/quad edge on large geometry only" symptom appears elsewhere (GPU path
  too — parity-match both sampler implementations).

**Runtime / threading / ABI**
- DC2 `LoadFile__FPcPvPi@0x149320` aborts the WHOLE game (`0x118FB0(0)`→`_Exit`) on ANY failed
  load → an empty/garbage filename is fatal. Find a `LoadFile2` caller via `*(ctx->sp+0x10)`.
  Empty names come from uninitialised game data (e.g. `GetItemFilePath` returns "" for invalid
  item id) (F50.5).
- Cooperative thread-yield syscalls (`RotateThreadReadyQueue`, syscall 0x2B) MUST release the
  guest-execution lock (`GuestExecutionReleaseScope`); a bare `std::this_thread::yield()` keeps
  `m_guestExecutionMutex` and starves other guest threads (F50/F49.5). Never reacquire the guest
  lock while holding `g_vsync_flag_mutex` (ABBA with the IRQ worker).
- DC2 uses MIPS EABI for 5+-arg calls: 5th int arg in `$t0` (`$a4`), not the stack. Derive a
  helper's ABI from the actual `recomp/<caller>.cpp` SET_GPR before the `jal` (F50.1).
- libgcc-ps2 64-bit/FP helpers use single-register-per-64-bit EABI (read `$a0/$a1` as full 64
  via `_mm_extract_epi64`, return `setReturnU64`); doubles are soft-float through GPRs. `$a2/$a3`
  in stub dumps are caller leftovers.
- LIVE syscall/stub defs are `Kernel/{Syscalls,Stubs}/*.cpp`; `*.inl` are /FORCE:MULTIPLE DEAD dups.
- CGamePad `+0x45C==0` enables On/Down accessors (not the pressed-this-frame field).

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
- **No real IOP execution / cooperative IOP scheduler** → blocks audio + audio-gated event stalls
  (today masked by the host-side `DC2_DISABLE_EVENT_SKIP` event-skip). Fix reference: upstream PR
  #135 (new IOP CPU/kernel/loader + cooperative scheduler). Needs a dedicated phase; also pulls
  recomp-side midasm hooks → a regen. Full upstream-PR survey (2026-07-07, decision: "map only,
  merge nothing" while GS/threading churns) archived in `plans/phase-history.md`.
- **No cooperative thread scheduler ABBA-safe wait helper** → threading deadlock/starvation at
  thread hand-offs (F49.5/F50 class). Partial fix APPLIED (lightweight post-wake yields in
  SignalSema/SetEventFlag/WakeupThread/ReleaseWaitThread). Fix reference: upstream #120
  `waitWithGuestExecutionReleasedUntilUnlocked` + full #137 fiber scheduler (high-risk, would
  obliterate MTGS — deferred).
- **VU0 micro-mode programs do not execute** (only inline COP2 macro mode works) — dormant today
  (DC2's skinned models use recompiler-inline COP2 macro ops, not VU0 micro-mode) but would surface
  as wrong physics/lighting on a VU0-micro route. Fix reference: upstream #120 (incomplete upstream,
  don't adopt yet).
- **No IPU/MPEG movie decode** → FMV blocker; bypassed headless. Fix reference: #120 MPEG decoder
  (ffmpeg-backed). Lowest priority.
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
