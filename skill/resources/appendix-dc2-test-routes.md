# Appendix: Dark Cloud 2 — Test Routes (graphics & audio verification matrices)

> **PROJECT-SPECIFIC, LOOKUP ONLY.** Every scripted route this port can drive headless, what it
> proves, and which harness runs it. Read the row you need; never top-to-bottom.
>
> Companions: `appendix-dc2-project.md` (paths, build, static export, input-injection mechanics §5) ·
> `appendix-dc2-runtime-architecture.md` (the runtime contracts each route exercises) ·
> `appendix-dc2-graphics-facts.md` (already-diagnosed defects).
> Live state: `PS2_PROJECT_STATE.md` (rules/build/gaps) · `plans/env-flags.md` (every `DC2_*` flag) ·
> `plans/ROADMAP.MD` (what to do next).

**Every scripted route needs three flags:** `DC2_DEBUG_MENU=1` + `DC2_PAD_INPUT='...'` +
`DC2_NO_XINPUT=1`. Every *performance* run additionally needs `DC2_PATCH_60FPS=1`.

**The runner's menu rendering is too corrupted to identify screens visually** — assert the route
reached the intended mode by STATE (`LoopNo`, `titleInfo`, `DngStatus`, `DAT_01ecd62c`), not by
looking at a frame. Frame-dump filenames are HOST TICKS, not guest scriptFrames.

---

## §1 Graphics routes

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
| `map_15.{gs,png}` | Down@57 → R1@114 → Right×5 @187/243/296/351/404 → Circle@444 → MAP 15, then a scripted **moving-camera cutscene** (~f=449–2129) | `tools/run_g536_map15.ps1`<br>gate `tools/g536_detect.py <tag>` | **GPU-resident RTT publication ownership** (`g261Materialize`, `genAtSkip`, G264 mirror), texture-in-RTT-window aliasing (G536) |

⚠️ **The MAP-15 route's recorded LEFT STICK is a constant keyboard non-centred neutral**
(`0x9c/0x9d,0x80`) and must NOT be replayed — replaying it walks the player. Recording preserved at
`plans/g525-routes/route7-map15-sky.{rec,replay}.txt`.
`g536_detect.py` reports **`defectPerSky`** — defects over *sky-visible* frames, not over dumped
frames. A slower arm reaches fewer guest frames in a fixed wall-clock window, so a raw rate silently
rewards any arm that never reached the scene (`DC2_G26X_NO_NATIVE=1` dumped 126 frames with only 15
containing sky).

### §1.1 The golden smoke test

```powershell
& 'D:\ps2r\dc2\tools\run_30s_diagnose.ps1'
python 'D:\ps2r\dc2\skill\scripts\ppm_nonzero.py' 'D:\ps2r\dc2\captures\frame_001500.ppm'
```
**Gate: `frame_001500 PixelNonZero = 211646 ±4`** (post-G139 natural render). The COUNT is the gate,
not a byte hash — the title pans/animates, so per-pixel content differs run to run.

- `run_30s_diagnose.ps1` presses Cross around frame 120 and navigates INTO a map. For a stable held
  TITLE frame use `tools/run_g100_cap.ps1 -PadInput '99000..99001:Up'` (clears frame dumps first).
- The pre-G138 forced-draw era's `633662–634384` band reproduces with
  `DC2_G100_FORCE_DRAW=1 DC2_VU1_NO_FMSWAPFIX=1 DC2_VU1_NO_MACPIPE=1`.
- **Invoke harnesses in-process (`& 'path\script.ps1'`)**, never `powershell -File` — a nested shell
  flattens array arguments and silently drops flags.

### §1.2 The lean MAP-0 performance route

Every perf phase since G414 uses the same route, via `tools/run_g477_perf.ps1`:
`Down@15 → Circle@94 → MAP-0 (DngStatus=0)`, uncapped, warm 120 frames, 600-frame windows.
The script owns the environment clear-list — **add every new census/verify/proof flag to `$clear` in
the same edit that adds the flag**, or it leaks into every later run of the session.

### §1.3 Heavy-route perf survey (G525)

**All six recordings are stored in `plans/g525-routes/` — replay one by name:**

```powershell
tools\g525_route.ps1 -List                      # routes + their measured n= windows
tools\g525_route.ps1 -Route georama -Census     # light occupancy census
tools\g525_route.ps1 -Route ridepod  -Flush     # flush decomposition + [G279:mat]
```

⚠️ **`run_record.bat` OVERWRITES `captures/input_rec.txt` every time.** Copy a new recording into
`plans/g525-routes/` before recording again, or it is lost. See that directory's `README.md`.

`tools/run_g477_perf.ps1` takes `-PadInput` / `-LStick` / `-RStick` directly; default is still the
lean MAP-0 route. Convert a raw recording with `python tools/replay_from_rec.py`.

| Route | Measured | Analysis |
|---|---|---|
| **`s05` forest cutscene** (Ch.2, 3 close-up characters) | **90.4 ms/f, 11 FPS**; Mode A: 5,160 GPU draws/f at 11.4 verts | `plans/phase-G525-heavy-route-survey.md` §1–§4 |
| **Palace prologue combat**, 4 Griffon Soldiers (`DngStatus=2`) | **87.0 ms/f (35–111), 9–13 FPS**; Mode A: 2,789–4,768 draws at 10.3–12.2 verts; `l2l` **0.00** | same file §7, §8a |
| **Palace prologue static screen** ("Monica was incapacitated") | **54.6 ms/f, 18.3 FPS on a byte-identical frame** — the zero-coherence proof | same file §8 |
| **Palm Brinks, night + rain** (Max's intro, `DngStatus=5`) | **91.7 ms/f, 10.9 FPS**; Mode B: packing fine (26.2 v/draw) but **22–25 readbacks/f = 38.8 ms/f**, `tex` 8.00, EE 38% on-CPU | same file §8b |
| ⭐⭐ **Ridepod boss battle vs Linda** (+ its ~58 s cutscene) | **98.3 ms/f (77–128), slowest in the survey** — with **0.58× MAP-0's vertices**; 8.5 v/draw, **7.46 µs per GIF draw** | same file §8c |
| **Ridepod static workshop screen** (tutorial box) | **67.6 ms/f on a frozen frame** — second zero-coherence proof | same file §8d |
| ⭐⭐ **Dungeon 6 free-roam** as Sewer Rat (map `d07/f01`) | **50.5 ms/f** with **0.25× MAP-0's GIF draws / 0.35× its verts / 0.41× its VU1 kicks** — **8.90 µs per draw, worst in the survey** | same file §8e |
| ✅ **Monster Transform menu** (same recording) | **16.6 ms/f = 60 FPS, GS worker 78% IDLE** on 434 draws + 1 readback — **the control**. ⚠️ its dungeon backdrop is a cached image (`kicks/f = 0`) | same file §8f |
| ⭐⭐⭐ **Georama placement mode** — **best subject for per-draw attribution** | **45.5 ms/f** with **488 GPU draws and 5 readbacks — FEWER than lean MAP-0's 829/6** — flush only 6.00 ms/f, GPU idle; **31.2 ms/f front residual = 69% of the frame** | same file §8g.3 |
| ⭐⭐ **Fire-rain field** (falling meteors, free-roam) | **55.6 ms/f at 1.0003× MAP-0's vertex count** — the matched pair. ⛔ **does NOT reproduce Mode B** (13.2 ms/f / 8 reads vs Palm Brinks' 38.8 / 22) | same file §8g.1–2 |
| lean MAP-0 (baseline) | 19.4–22.5 ms/f; 829 GPU draws/f at 37.6 verts; **0.24 µs/draw**; 6 readbacks/f | §1.2 |
| **`dungeon6v`** — Dungeon 6 verification (user recording 2026-08-09) | G540 re-measure | `plans/g525-routes/route8-dungeon6-verify.*` |
| **`map125`** — MAP-125 via debug menu (authored, not recorded) | G540 re-measure | `plans/g525-routes/route9-map125.replay.txt` |

⛔⛔ **EVERY NUMBER IN THIS TABLE IS STALE (G540).** The whole survey was taken either before G534 or
with G534's `s_g273ExactAlias` retirement present — i.e. on a binary carrying **32.7 ms/f of CPU band
replay** that G539 removed. Lean MAP-0 alone went 50.32 → **18.71 ms/f** when the retirement was
reversed. Re-measure any route before ranking off it; the *ratios between* routes are also not safe,
because the regression sat on the display batch and its share differs per scene.

### §1.2b The two user-owned verification routes (added G540)

```powershell
tools\g525_route.ps1 -Route dungeon6v -Tag mytag    # Dungeon 6, user recording
tools\g525_route.ps1 -Route map125    -Tag mytag    # MAP-125, authored from the button sequence
```

- **`dungeon6v`** — `Down ×2 → Right ×6 → Circle@480 → Cross@641/692`, then free-roam. ⚠️ Its
  recorded LEFT STICK is the usual **constant non-centred keyboard neutral `0x98,0x63`** until
  scriptFrame **812**, where the user genuinely starts walking. Replaying the raw channel walks the
  player through the menu route, so a **masked sidecar** (`route8-dungeon6-verify.lstick.txt`) forces
  the prefix to centred `0x80,0x80` and replays only the real movement. `g525_route.ps1` prefers a
  `.lstick.txt` sidecar over the raw channel whenever one exists.
- **`map125`** — `Down → R2 → R1 ×2 → Right ×5 → Circle`, authored from the user's sequence rather
  than recorded, so its **timing is modelled on `route7-map15-sky`** (~55 scriptFrames apart, held
  ~16). Game-space masks: `Down 0x4000 · Right 0x2000 · Circle 0x0020 · Cross 0x0040 ·
  Triangle 0x0010 · R1 0x0008 · R2 0x0002` (raw scePad bit with the two bytes exchanged).

⚠️ **The Ridepod recording carries REAL analog input** (driving) — it must be replayed with
`-LStick`. Resting value `0x69,0x73` is pad drift inside the deadzone; genuine pushes reach
`0x90,0x01`. First route where dropping the stick channel changes what is measured.

⚠️ **Flushes/f is a per-route constant** — 33.3 (MAP-0) · 40.0 (`s05`) · 50.0 (combat) · 32.5 (static).
Every `[G273]`/`[G298]` per-flush→per-frame conversion depends on it; re-derive, never carry over.
⚠️ **`[G273]`/`[G298]`/`[G279]` are CUMULATIVE means** — de-cumulate a transient window with
`(N₂·avg₂ − N₁·avg₁)/(N₂ − N₁)`.
⚠️ Do NOT replay a recording's `DC2_LSTICK` when it is a constant (e.g. `0x96,0x6c`) — that is the
keyboard path's non-centred neutral, not movement, and it walks the player.
⚠️ **`run_record.bat` does not set `DC2_PATCH_60FPS=1`** — live recordings are 30 FPS-capped, so every
33.3 ms window in `record_err.txt` is the cap, not a measurement. Headless replay is unaffected.
⚠️ A replay presses only what was recorded. The combat recording has **no** combat input, so the
player dies ~150 frames in; everything after is the static screen.

---

## §2 Audio routes

Each row: subsystem / feature → exact trigger / route → harness & levers → verification & oracle
checks. Format/contract detail lives in `appendix-dc2-runtime-architecture.md` §12–§14.

| Subsystem / Feature | Replay route / trigger | Harness & levers | Verification & oracle checks |
|---|---|---|---|
| **FMV / PSS PCM audio (G384)** | Attract FMV playback (`RUSH.PSS` stream `0xBD`, 48 kHz stereo planar PCM) | `tools/run_g384_fmv_audio.ps1`<br>`DC2_G384_MPEG_AUDIO_TRACE=1`<br>`DC2_G384_NO_MPEG_AUDIO=1` | Demuxes planar PCM16LE (`SShd`/`SSbd`); streams via raylib without desync or silence. |
| **Sony HD/BD SFX & SQ BGM (G385, G391)** | Gameplay BGM & SFX triggers (`SE_Play@0x189C10`, MODMSIN messages) | `tools/run_g390_audio_route.ps1`<br>`DC2_G391_SELFTEST=1` (17/17)<br>`DC2_G385_AUDIO_TRACE=1` | Parses `SCEISequ` at 44.1 kHz into the 24-voice mixer (`dc2_g391_sfx_mixer.inc`); verifies `.HD` sample params (+18/+20 ADSR, +17 split pan) and fixed BGM bus gain (0.85). |
| **BGM/SFX stop authority (G450, G451)** | Recorded title route `captures/input_rec.txt` (Circle ×2, Start ×3) | `DC2_G450_BGM_TRACE=1`<br>`DC2_G451_LEGACY_EARLY_STOP_GUARD=1` | Guest 3 → 4 → 1 transition calls `TitleModeInit@0x2A1020` and restarts BGM after `TitleInit`'s Stop. EZMIDI `Stop` stops all port voices, `Quit` stops all. |
| **WAV voice streaming (G386)** | Spoken cutscene lines (EZBGM RPC SID `0x12345`) | `tools/run_g386_voice.ps1`<br>`DC2_G386_VOICE_TRACE=1`<br>`DC2_G386_NO_VOICE=1` | Indexes `SOUND.HD3` into `SOUND.DAT` for 48 kHz mono WAV; returns `0x1000` until stream completion to unblock cutscene scripts. |
| **Multi-chunk `.BD` level BGM (G387)** | Level BGM load with a multi-transfer SIF DMA `.BD` bank body | `DC2_G387_NO_BD_CHUNKS=1` | Concatenates multi-chunk SIF DMA `.BD` transfers so levels resolve all notes. |
| **Battle BGM aliased bank re-binding (G388)** | Battle state trigger issuing a second `SetHD` for a committed bank | `tools/run_g388_battle_bgm.ps1`<br>`DC2_G388_NO_BANK_ALIAS=1` | Caches the last committed bank by `SetHD` triple to re-bind aliased ports without note drops. |
| **EZMIDI volume scale & pan swap (G389, G392)** | Live `SE_SetVol` / `SE_SetPan` and EZMIDI port volume updates | `tools/run_g392_audio_route.ps1`<br>`DC2_G389_LEGACY_MIX=1`<br>`DC2_G392_LEGACY_SE_MIX=1` | Divides EZMIDI volume by `256.0f`; treats `F9 00` as channel volume and `F9 01` as channel pan (`0x40` = centre). |
| **SPU2 reverb topology & retail presets (G391, G392, G393)** | Reverb send (`SD_REV_MODE_*`) in dungeons and rooms | `tools/run_g392_audio_route.ps1`<br>`DC2_G391_SELFTEST=1` (17/17) | Hardware-faithful 22,050 Hz SPU2 reverb network using the exact `LIBSD.IRX` `.data` preset tables (9 records). |

---

## §3 Route selection — what to run after touching what

| You touched | Run |
|---|---|
| VU1 MAC / flags / culling / clip | title golden (`ttle`) **+** dungeon-0 light (`correct_light`) |
| VU1 scalar pipeline / Q latency | `correct_light` |
| Cross-lane VU1 geometry (OPMSUB/OPMULA) | MAP-0 **+** MAP-4 |
| Rasterizer STQ / texture interpolation | `map_2_zoom` |
| Alpha test / blend / Z-write | `dungeon_1_cutscene` |
| Tile-bin / MTGS / pipelining | MAP-0, then soak with `DC2_FRAME_DUMP_EVERY=1` |
| VIF1 chain-tag walker / shared-page clear | MAP-4 |
| RTT / composite / zoom ownership | `map_4_zoom` |
| GPU-residency publication / `g261Materialize` / page ownership | `map_15` **+** `map_4_zoom` **+** Inventory (character RTT) |
| Boot static-inits / 2D UV | `items` |
| Sprite defer / costume preview | `Inventory` |
| Preemption / vtable draw chain | `debug_menu` |
| Any perf lever | lean MAP-0 (`run_g477_perf.ps1`) + the two routes for whatever class it could break |
| Audio mixer / bank / reverb | the matching §2 row + `DC2_G391_SELFTEST=1` |
