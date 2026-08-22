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
| `debug_menu.{gs,png}` | Debug menu → Down×2 → Down → Confirm (`DrawSub__8CEditMapFi@0x001B4130`) | `tools/run_30s_diagnose.ps1`<br>`DC2_DEBUG_MENU=1` | Preemption unwind (`g_dc2PreemptSuppressDepth`), vtable draw chain (G396) |
| `map_15.{gs,png}` | Down@57 → R1@114 → Right×5 @187/243/296/351/404 → Circle@444 → MAP 15, then a scripted **moving-camera cutscene** (~f=449–2129) | `tools/run_g536_map15.ps1`<br>gate `tools/g596_skybroken.py <tag>` (⛔ **not** `g536_detect.py` alone)<br>cross-arm: **`-SfDump`** + `tools/g598_pair.py` (§3.2b, `appendix-dc2-capture-and-gates.md`) | **GPU-resident RTT publication ownership** (`g261Materialize`, `genAtSkip`, G264 mirror), texture-in-RTT-window aliasing (G536), 0x139-residency sky defect (G598) |

| *(no reference dump yet)* | **`route13-heavy-new`** — recorded by the user 2026-08-22. Sleeping-dragon tent cutscene: `Down ×5 → Circle → Right ×9 → Down ×5 → Circle` into the map, then ~180 Cross taps through a long dialogue; 4,417 scriptFrames, then a **STATIC tail** | `tools/g525_route.ps1 -Route dragon` | ⭐⭐⭐ **THE LARGEST GS POLE IN THE SET** (GS own 33.2–33.8 vs VU1 24.0–25.5, headroom **+9.2 ms/f**) and the only content-static steady state. ⛔ Replay NEITHER stick. ⛔ Do NOT bring a CPU-band-replay lever here — see §1.3p |

| *(no reference dump yet)* | **`route11-s03-cutscene`** — recorded by the user 2026-08-16. Stage `map/s/s03`, the Ch.1 forest cutscene (Flotsam's clown boss + Linda + the train wreck): `Circle → Down×5 → Circle → Right×3 → Circle` into the map, then ~30 Cross taps through the dialogue; 4,919 scriptFrames | `tools/g525_route.ps1 -Route s03` | **CPU BAND REPLAY / GPU admission on the DISPLAY buffer.** The only surveyed route whose display batches fall back in bulk (1,062 batches / 18.6 s of replay wall), and the route G603's blend mode 6 was measured on. ⛔ Do NOT replay its left stick (constant keyboard neutral `0x68,0x68`). ⚠️ The live `record_err.txt` is 30 FPS-capped — `run_record.bat` sets no `DC2_PATCH_60FPS` |
| `s05.{gs,png}` | **`route10-s05-bandage`** — recorded 2026-08-15; last Cross at scriptFrame 6863, after which the cutscene parks on *"Right. Thanks, Master Utan"*, which IS the reference frame | `tools/run_g477_perf.ps1 -PadInput <replay string from plans/g525-routes/route10-s05-bandage.replay.txt>`<br>capture **per PRESENT**: `DC2_FRAME_DUMP=1 DC2_FRAME_DUMP_EVERY=1 DC2_G600_TICK_SF_LO=6900`<br>gate `python tools/g601_band.py <tag>` (find: `g601_flicker.py`) | **ONE-FRAME / ALTERNATING defects** (Monica's hair band, ✅ FIXED by G601; rollback `DC2_G601_NO_EMPTYBIND=1` reproduces it). ⛔ Do NOT replay its left stick. ⛔ `-SfDump` at the default cadence cannot see this class — see §3.2c in `appendix-dc2-capture-and-gates.md` |

⚠️ **The MAP-15 route's recorded LEFT STICK is a constant keyboard non-centred neutral**
(`0x9c/0x9d,0x80`) and must NOT be replayed — replaying it walks the player. Recording preserved at
`plans/g525-routes/route7-map15-sky.{rec,replay}.txt`.
`g536_detect.py` reports **`defectPerSky`** — defects over *sky-visible* frames, not over dumped
frames. A slower arm reaches fewer guest frames in a fixed wall-clock window, so a raw rate silently
rewards any arm that never reached the scene (`DC2_G26X_NO_NATIVE=1` dumped 126 frames with only 15
containing sky).

### §1.1 Required graphics smoke

Do not gate changes on `frame_001500` or a title nonzero count. At that host tick the game may still
be playing FMV, so the capture does not prove title or GS gameplay correctness. Use the §3
change-to-route matrix to choose **exactly one graphics route: the route most likely to break from
the changed mechanism**. Do not stack generic graphics routes. Assert route arrival by game state,
then review its full-frame distribution plus representative gameplay frames. Keep
`tools/run_g100_cap.ps1` only as an optional title-specific diagnostic when title behavior itself is
under investigation.

**Invoke harnesses in-process (`& 'path\script.ps1'`)**, never `powershell -File` — a nested shell
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
`g525_route.ps1` now forwards all three (G613); `LStick` / `RStick` are **opt-in per route**, because
a recorded channel that is a constant neutral must not be replayed (§1.3n rule 1).

⚠️ **CORRECTED (G613): `run_record.bat` DOES set `DC2_PATCH_60FPS=1`.** A live recording is
**uncapped** and its `record_err.txt` frame times are real. The old rule ("live recordings are 30 FPS
capped, every ~33 ms window is the cap") is stale — but recordings taken *before* the flag was added
are still capped, so check the batch file rather than the doc.

### §1.3p ⭐⭐⭐ `dragon` — the biggest GS pole in the set, and the only STATIC steady state (added G638)

```powershell
tools\g525_route.ps1 -Route dragon -Census -Tag <tag>      # three-thread pole
tools\g525_route.ps1 -Route dragon -Tag <tag> -Set @('DC2_G638_PREP=1')   # what the pole IS
```

User recording 2026-08-22, preserved as `plans/g525-routes/route13-heavy-new.*`. Debug menu
`Down ×5 → Circle → Right ×9 → Down ×5 → Circle`, then ~180 `Cross` taps through a long dialogue
(405 input changes, scriptFrame 0..4417). Scene: a tent interior with a **sleeping blue dragon**.
⛔ **Replay NEITHER stick** — both channels are dead-centre `0x80,0x80` for the whole recording.

| window | frame | GS own | VU1 | EE cpu | verdict |
|---|---:|---:|---:|---:|---|
| static tail (sf ≳ 4600, host tick ≳ 2071) | 34.4–35.8 | ⭐ **33.2–33.8** | 24.0–25.5 | 15.6–17.7 | **SOLE GS POLE, headroom +9.2** |

⭐ **Its tail is a STATIC screen** — after the last input the route parks on a dialogue frame. Nothing
in the content depends on frame rate, so **a speed-changing candidate cannot move it**: this is the
one route where G637's speed-perturbed control is unnecessary. Best **timing** gate in the set.

⛔⛔ **It is NOT memoizable, and the reason is a trap.** `[G147:gif]` reports `tags` / `packedRegs` /
`imageKB` **byte-identical** across every window of the tail — but those are register COUNTS, not
VALUES. G638's payload memo measured **0 hits in 1,200 consecutive shadow batches**: the 0x139
shadow geometry is re-emitted with different coordinates every frame while the screen holds still.

⛔⛔⛔ **DO NOT BRING A CPU-BAND-REPLAY LEVER HERE.** `[G638:prep]` on the tail:
`g570gpubatch` **5.263** + `g570prep` **4.735** vs `poolreplay` **1.611** ms/f — i.e. **~10 ms/f of
the pole is the G570 0x139 shadow-compute preparation and band round trip**, and the replay every
phase since G529 has been optimising is 1.6. `[G638:pool]` puts the whole `GSRowPool::run` barrier
across all seven call sites at **1.84 ms/f**. The GS thread's real wait is the **GL backend future at
≈9.9 ms/f** (`[G290:probe] gpuOk`, 46.9 synchronous submits/f @ 212 µs, `[G299:backend] q = 8–12 µs`
so it is GL work, not handoff). ⛔ G637's payoff gate reads **NULL** here (blocks −0.197 / +0.026).

⚠️ **8,044 GIF packets/frame** against lean MAP-0's 14 — this route stresses per-packet cost, not
per-pixel cost. `DC2_G570_NO_GPU_139=1` costs **+2 ms/f**, so the GPU compute is paying for itself.

### §1.3n ⭐⭐⭐ `dungeon1` — the GS gate route (added G613)

```powershell
tools\g525_route.ps1 -Route dungeon1 -Census -Tag <tag>   # then tools/g605_pole.py on the _err.txt
```

`map/d/d02/f01` via debug-menu dungeon **index 1**, then a ~37 s analog run. Recording + full
evidence: `plans/g525-routes/README.md` and `plans/phase-G613-fix-log.md`.

⛔⛔⛔ **SUPERSEDED BY G624 + G626 — `dungeon1` IS NO LONGER THE GS GATE.** It is CLOSED to
single-thread levers (its `GS own` and `EE cpu` are LEVEL; injection **GS 0.69 / EE 0.14**,
≈+0.3 ms/f left). ⭐⭐⭐ **The GS gate is now `ridepod`**, which G626 measured as a **SOLE** pole:
`GS own` **21.14** vs VU1 17.72 vs EE 14.26, headroom **+3.42**, and **injection sensitivity 1.05**
(a GS cut converts ~1:1 over multiple ms).

⭐⭐⭐ **PIXEL GATE SCOPE (G635/G636): `ridepod`'s OPENING (sf 768..1392) IS A BIT-DETERMINISTIC PIXEL GATE (floor 0.00).**
The older note ("`ridepod` grades TIMING ONLY") applied **only to its late gameplay / boss window (sf 2088+)**, where route motion drifts. The first ~10 seconds is a deterministic cutscene. When evaluated with `tools/g635_bisect.py` (temporal-min per-tile estimator, excluding the host-present opening fade at sf < 768), control-vs-control scores **p50 0.00 / p90 0.00 / max 0.00** across 53 shared keys. It is the premier pixel gate for GS rasterization, resident renderer, and triangle span kernel changes.

⛔⛔ **G637 CORRECTION — THAT 0.00 FLOOR IS CONDITIONAL ON THE TWO ARMS RUNNING AT THE SAME SPEED.**
The dialogue cursor and the particle sprites advance on **host presents**, so a FASTER arm lands on a
different animation sub-phase inside the same script bucket and scores ~1.2–1.6 against a 0.00 floor
with no rendering defect at all. Every prototype gated here before G637 was *slower* than control,
which is why this never surfaced. A speed-changing candidate needs a speed-perturbed same-binary
control (`DC2_G431_GS_SLOW_US=<n>`, ≈ +1.83 ms/f per unit) as a second axis — recipe and the three
readings that settle it: `appendix-dc2-capture-and-gates.md` §3.2f.

⚠️ **Do NOT set `DC2_FRAME_DUMP=1` when using `DC2_G598_DUMP_SF`**: the host-tick dumper silently preempts the script-clock dumper, producing incomparable host-tick files. Evidence: `plans/phase-G635-fix-log.md` and `plans/phase-G636-fix-log.md`.

### ⚠️⚠️ `ridepod` — LIMITS THE NEXT PHASE MUST KNOW (added G628, refined G635/G636)

**1. IT TERMINATES NON-DETERMINISTICALLY IN WHOLE RUNS, so a whole-run gate silently loses its window.**
Eight identical 280 s runs ended at scriptFrame **25494 / 22173 / 13537 / 14201 / 9153 / 25893 /
25627 / 23501**. Four hit the harness timeout; four exited on their own (`Window closed
successfully`, `crashLines=0`), and the early exits hit BOTH arms of a paired gate (2 control,
2 candidate) — it is route noise, not a lever. Consequence: **no arm-common script window reaches
the boss window (sf 18000..22173)**, so `tools/g627_abba.py` (which intersects to
`hi = min(ends)`) scores the *pre-boss* segment unless you notice. Pass `--lo` deliberately and
read the printed `common window:` line before quoting anything.

**2. IT IS THE ONLY ROUTE THAT EXERCISES THE LOWERED TRIANGLE SPAN KERNELS.** With
`DC2_G609_VERIFY=1` armed, rows offered to `g609TriScanRowT`:

| route | rows offered | verdict |
|---|---:|---|
| `ridepod` | **331,600,000** | the only usable gate |
| `dungeon6v` | 4 | smoke only |
| MAP-15 | **0** — `[G609:scan]` never printed | **zero coverage** |

So a clean pixel comparison or a `bad=0` on MAP-15 says **nothing** about a G605/G608/G609/G628
change (`g589_oracle_with_zero_checks` at route level). MAP-15 remains worth running as a
*collateral-damage* check — just never quote it as coverage. Always confirm the census printed a
non-trivial denominator before calling a result clean.

⭐⭐⭐ **[HISTORICAL] This was the route to gate a GS lever on.** Measured on the shipped G612 binary, 108 windows:

| | `dungeon1` | `ridepod` | `s05` | `s03` |
|---|---:|---:|---:|---:|
| VU1-poled | **0.0%** | 12.4% | 79.5% | 78.3% |
| GS own mean / max | **31.97 / 72.10** | 22.60 | 17.61 | 28.73 |
| VU1 busy mean | **13.90** | 20.55 | 18.43 | 32.56 |
| **headroom** | **+18.07** | +2.05 | −0.83 | −3.83 |
| > 33.33 ms | 24 (all GS) | 7 | 40 | 87 |

⭐ **Re-derived on the shipped G615 binary:** `dungeon1` **112/112 GS-poled**, GS own 30.70 / max 72.60
vs VU1 13.48, headroom **+17.23**; and **`ridepod` is now 0/231 VU1-poled with headroom +6.96** (was
12.4% / +2.05), so a GS lever has two viable gates. ⚠️ Those two rows summarise different window
populations from G612's, so only the within-run derivatives are quotable.

- ⛔ **It cannot gate a VU1 lever** — zero VU1-poled windows; VU1 leads nothing anywhere on it.
- ⛔ **`gsStallMs/f` is 0.00 in the tail**, so the attribution does not rest on the stall subtraction.
- ⛔⛔ **ITS MONSTER AI IS DRIVEN BY THE FRAME RATE, so ANY speed-changing lever moves the content.**
  A pixel comparison across two arms therefore has a **~1–4% content-divergence floor** that no amount
  of extra frames reduces: G615 measured a **same-binary ON-vs-ON′ control at 96.0% byte-identical**
  (and OFF-vs-OFF′ at 99.0%, with one key at `mean|Δ| = 32.378`). Consequences:
  - **Always four arms** (ON, ON′, OFF, OFF′) and read the **paired EXCESS** (G515), never the
    identical count. ⭐ A lever pair that lands *above* both controls (G615's did: 99.2% / median
    `mean|Δ|` 0.142) is a strong pass, since a speed change should diverge *more*, not less.
  - A defect confined to a few frames **can hide inside that floor**, so a stage-level same-run oracle
    is the primary exactness evidence on this route and the pixel pair is the composition check.
- ⚠️ **`DC2_G598_DUMP_SF` alone dumps nothing comparable — you must also set `DC2_FRAME_DUMP_TAG=<arm>`.**
  `tools/g525_route.ps1 -Tag` does **not** set it (only `run_g536_map15.ps1 -SfDump` does), so the PPMs
  land as untagged `frame_NNNNNN.ppm`, `tools/g612_framepair.py` globs `frame_<tag>_*.ppm` and prints
  `shared=0 → NO SHARED SCRIPT FRAMES` — a null that looks like a measurement. Pass it in `-Set`.
- **Two separable populations, not one.** The spikes (n=241..1051) are the *display* batch falling
  back to CPU replay — same packets at **3.2× unit price** with `imageKB` flat. The 28.7 ms/f steady
  floor (n≥1081) is **97.9%** one `discov sprite` shape family (Target L, cut by G614).
  ⛔⛔ **The old "and 38.8% of it prologue at 50.79 µs/call (Target K)" line is REFUTED** — that was
  `[G605:leaf]` booking untextured sprites' whole fill loops as prologue (G615 §1). Post-fix the same
  route reads `pro% = 1.1` at 1.00 µs/call, and the real cost was the `disp sprite` **loop** at 2.17×
  the `discov` leaf, which G615 cut to 302.8 cyc/inside.
- **Its replay is 99.8% sprites.** Triangles are 0.2% — G605/G608/G609's kernels are inert here, as
  on `s05`. Three routes, three different replay machines (G609's law).
- ⛔⛔⛔ **G617: THE WHOLE CPU BAND REPLAY ON THIS ROUTE IS ~1.1 ms/f OF WALL, SO IT IS NOT WHERE THE
  FRAME IS.** G614, G615 and G617 optimised most of that subsystem — the four biggest leaves, by
  −33%, −58%, −86% and −21% `cyc/inside`, every one exact — and **every frame gate after G615 read
  NULL**, because their effects share a ~1.1 ms/f budget inside a 34 ms frame. `GS own` is 30.70 and
  `frontMs/f` is 21.4: **something other than band replay owns this route** and G613's nesting
  question is now the only thing worth measuring on it. ⛔ Do not build another replay-pixel lever
  here expecting a frame.
- ⛔⛔ **AN EARLY `[G419:ab]` PRINT ON THIS ROUTE IS NOT A MEASUREMENT.** The first windows are the
  entry spike (n=241..1051, 63–74 ms/f), and whichever arm lands there reads catastrophically:
  G617's legs printed **+30.9% (t = 16.53)** and **+40.4% (t = 138.82)** at `w = 2` and converged to
  −0.4/−0.6 by `w = 28`. At `w = 2` the blocked `se` comes from one pair difference and can be
  arbitrarily small by chance. **Read only the LAST print of a COMPLETED leg; treat `w < 10` as no
  result.**
- ⭐⭐ **Read `[G605:leaf]`'s `spr-slow` row and `[G605:shape]`'s `tme=` column** (both added by
  G617). Before them, `disp sprite` averaged the span kernel's admitted and refused draws — 94%
  refused by draw — and the shape rows summed to 1.9× the leaf's own pixel count.
- ⚠️ **Replay its LEFT stick** (the route is the walk) but only via the masked `.lstick.txt` sidecar —
  the recorded neutral is `0x91,0x63` until scriptFrame 579. Its camera channel is constant and is
  not replayed.

### §1.3a ✅ CURRENT PROFILE — post-G585 whole pipeline + G588 closeout (2026-08-14)

The full matrix was measured after G585 on Release `ps2_runtime` / `dc2_runner`,
`DC2_PATCH_60FPS=1`, GPU **P0 / 1620 MHz**. Each recording had a lean level pass and a light
occupancy pass. `mean` / `worst` are lean 60-frame windows.

⚠️ Raw `[G332:gsw]` includes time the GS worker spends waiting for VU1 catch-up. Resource ownership
is `GS own = gsWorker - gsStall`, compared against **total VU1 busy**. Do not compare against
`VU1 busy - stall` or label the whole `front` residual CPU work.

| Window (`n` range) | mean | worst | VU1 busy | GS stall | GS own | Resource pole |
|---|---:|---:|---:|---:|---:|---|
| `ridepod` boss `1441..1861` (post-G588) | **35.53** | 41.86 | 25.69 | 0.01 | **35.44** | GS |
| `fight` Palace `781..1741` | 32.26 | 38.33 | 23.82 | 0.72 | **30.29** | GS |
| `s05` forest `1021..2461` | **35.96** | 38.14 | 25.24 | 0.00 | **36.20** | GS |
| `combat` action `1621..2101` | **33.79** | 45.35 | 29.58 | 1.00 | **32.71** | GS |
| `ridepod` workshop `2701..3661` | 31.12 | 31.96 | 29.14 | 0.02 | **31.05** | GS |
| `combat` static `2401..6301` | 31.53 | 32.57 | **30.76** | 1.15 | 29.79 | **VU1** |
| `georama` fire-rain `421..721` | **33.59** | 35.24 | **30.62** | 12.54 | 19.25 | **VU1** |
| `georama` placement `1101..3541` | 33.01 | 56.49 | **32.27** | 18.00 | 15.18 | **VU1** |
| `fight` Palm Brinks rain `3151..6301` | 33.08 | 66.63 | 27.12 | 4.33 | 27.40 | tie / slight GS |
| `dungeon6v` free-roam `3181..6301` | 21.31 | 30.75 | 12.35 | 0.00 | **21.19** | GS |
| `map125` `901..4801` | 20.57 | 24.05 | **19.45** | 1.12 | 18.08 | **VU1** |
| lean `MAP-0` `1201..4441` | 19.74 | 23.03 | **18.33** | 0.17 | 17.92 | **VU1** |
| `dungeon6` free-roam `1621..3541` | 16.67 | 16.73 | 5.24 | 1.10 | **13.77** | GS |
| `dungeon6` Transform `721..1201` | 16.67 | 16.72 | 0.32 | 0.00 | 8.11 | EE/present exception |

Verdict: **4/14** exceed 33.33 ms by mean, **7/14** by worst window, and **0/14** are strictly below
16.67 ms. Five gameplay windows are VU1-resource-dominant, Palm Brinks rain is tied, seven remain
GS-resource-dominant. Transform is not a GPU/VU control.

G588 then promoted the exact solid-sprite replay commit. Ridepod boss improved 4/4 paired runs,
mean `-1.169 ms/frame (-2.86%)`, with 272,449,924 oracle pixels and `bad=0`; the table's Ridepod row
is the rebuilt post-G588 lean/census landing. It does **not** alter the
architecture verdict: the complete S05 breadth run selected zero G588 frames, so S05 retains its
approximately 11 ms GS-resource lead. Next work is the integrated GPU-VU command/authority redesign,
not another isolated transient admission.

Evidence: `captures/g477_g585_post_*_err.txt` (use `fight_*2`),
`captures/g477_g588_solid_*_err.txt`, and `plans/phase-G588-fix-log.md`.

### §1.3d–§1.3l — the ATTRIBUTION RECIPES → **moved to `appendix-dc2-attribution-recipes.md`**

⭐ Ten "how to attribute X" recipes (G583–G595) used to sit here. They are method, not routes, and
they were the bulk of this file. Section IDs are unchanged. **Load that file when you have a number
and need to know what owns it**; load this one when you need to drive something.

⭐⭐⭐ **Start with §1.3j (price a whole-layer bypass BEFORE building), then §1.3h (the GS worker's
blocked half) before §1.3f (the worker itself).** Several recipes exist to correct the one before.


### §1.3b ⛔ SUPERSEDED — the G525 survey table

⛔⛔ **EVERY NUMBER BELOW IS STALE — twice over.** (1) G540: the survey was taken before G534, or
with G534's `s_g273ExactAlias` retirement present, i.e. on a binary carrying **32.7 ms/f of CPU band
replay** that G539 removed. (2) G582: G581 + G582 are now default-ON. Kept only as the historical
per-draw/per-vertex analysis, which §1.3a does not repeat. **Never rank off this table** — use
§1.3a.

| Route | Measured (STALE) | Analysis |
|---|---|---|
| **`s05` forest cutscene** (Ch.2, 3 close-up characters) | 90.4 ms/f; Mode A: 5,160 GPU draws/f at 11.4 verts | `plans/phase-G525-heavy-route-survey.md` §1–§4 |
| **Palace prologue combat**, 4 Griffon Soldiers (`DngStatus=2`) | 87.0 ms/f (35–111); Mode A: 2,789–4,768 draws at 10.3–12.2 verts; `l2l` **0.00** | same file §7, §8a |
| **Palace prologue static screen** | 54.6 ms/f on a byte-identical frame — the zero-coherence proof | same file §8 |
| **Palm Brinks, night + rain** (`DngStatus=5`) | 91.7 ms/f; Mode B: 26.2 v/draw but **22–25 readbacks/f = 38.8 ms/f**, `tex` 8.00 | same file §8b |
| **Ridepod boss battle vs Linda** | 98.3 ms/f (77–128) at **0.58× MAP-0's vertices**; 8.5 v/draw, **7.46 µs per GIF draw** | same file §8c |
| **Ridepod static workshop screen** | 67.6 ms/f on a frozen frame — second zero-coherence proof | same file §8d |
| **Dungeon 6 free-roam** as Sewer Rat (`d07/f01`) | 50.5 ms/f at **0.25× MAP-0's GIF draws / 0.35× verts / 0.41× kicks** — 8.90 µs/draw | same file §8e |
| **Monster Transform menu** | 16.6 ms/f, GS worker 78% IDLE on 434 draws + 1 readback. ⚠️ backdrop is a cached image (`kicks/f = 0`) | same file §8f |
| **Georama placement mode** — best subject for per-draw attribution | 45.5 ms/f with **488 GPU draws / 5 readbacks**, FEWER than lean MAP-0's 829/6; flush only 6.00 ms/f | same file §8g.3 |
| **Fire-rain field** | 55.6 ms/f at 1.0003× MAP-0's vertex count — the matched pair. ⛔ does NOT reproduce Mode B | same file §8g.1–2 |
| lean MAP-0 (baseline) | 19.4–22.5 ms/f; 829 GPU draws/f at 37.6 verts; **0.24 µs/draw**; 6 readbacks/f | §1.2 |

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
- **`fight`** — after the recorded Circle, `g525_route.ps1` removes the old Cross ranges and emits a
  one-script-frame Cross tap every five frames from 1196 through 5200. The four-frame release gap is
  required: combat consumes button-down edges, so a held or merged Cross loses the battle. The
  corrected route wins and reaches canonical Palm Brinks rain; use `fight_*2` for the G585 matrix.

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

For graphics correctness, run **one row and one route only**: choose the route whose exercised
mechanism is closest to the change and therefore most likely to regress. A separate performance
route may still be used to measure speed; it is not an additional graphics gate.

| You touched | Run |
|---|---|
| VU1 MAC / flags / culling / clip | dungeon-0 light (`correct_light`) |
| VU1 scalar pipeline / Q latency | `correct_light` |
| Cross-lane VU1 geometry (OPMSUB/OPMULA) | MAP-4 |
| Rasterizer STQ / texture interpolation | `map_2_zoom` |
| Alpha test / blend / Z-write | `dungeon_1_cutscene` |
| GPU **admission** on the DISPLAY buffer (blend modes, `g178ClassifyEntry`, `g265DisplayZWave`) | `s03` — the only route where the display batch falls back in bulk, so it is the only one where an admission change moves anything. Pixel-gate it with `DC2_G598_DUMP_SF=<every>` on both arms + `tools/g603_sfdiff.py <tagA> <tagB>` (script-clock keyed; a host-tick pair is meaningless when the arms differ in speed) |
| Tile-bin / MTGS / pipelining | MAP-0 (use dense capture on this same route if needed) |
| VIF1 chain-tag walker / shared-page clear | MAP-4 |
| RTT / composite / zoom ownership | `map_4_zoom` |
| GPU-residency publication / `g261Materialize` / page ownership | `map_15` — ⛔ **and NOT with `g536_detect.py` alone**, see below |
| A local→local / host→local VRAM TRANSFER's bytes (swizzle, PSM, run/table writers) | `map_15` + a `DC2_G596_VERIFY`-class byte oracle (an address oracle is not enough) |
| Boot static-inits / 2D UV | `items` |
| Sprite defer / costume preview | `Inventory` |
| Preemption / vtable draw chain | `debug_menu` |
| Any perf lever | lean MAP-0 for timing, plus the single most-at-risk graphics route from this table |
| Audio mixer / bank / reverb | the matching §2 row + `DC2_G391_SELFTEST=1` |

### §3.2 CAPTURE & GATE DISCIPLINE → **moved to `appendix-dc2-capture-and-gates.md`**

⭐ `g536_detect.py`'s blind spot, §3.2a (one dump path per tag), §3.2b (never key on the host tick),
§3.2c (a one-frame defect is invisible to `-SfDump`), §3.2d (cross-binary A B B A) and §G610 (gating
a VU1 execution lever) moved there. Section IDs unchanged.

⛔⛔ **The rule that governs all of them: a pixel comparison is only readable against a SAME-BINARY
control.** Two runs of one unmodified binary do not agree on this game.

