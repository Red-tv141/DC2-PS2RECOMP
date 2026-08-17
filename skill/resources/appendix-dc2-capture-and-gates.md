# Appendix: Dark Cloud 2 — CAPTURE & GATE DISCIPLINE (how to prove a change did no harm)

> **PROJECT-SPECIFIC, LOOKUP ONLY.** Split out of `appendix-dc2-test-routes.md` (2026-08-17, G617):
> a route says *what to run*, this says *how to capture the result and what makes the comparison
> legitimate*. Section IDs (`§3.2a`…`§3.2d`, `§G610`) are unchanged.
>
> ⛔⛔ **THE ONE-LINE SUMMARY OF EVERY SECTION BELOW: A PIXEL COMPARISON IS ONLY READABLE AGAINST A
> SAME-BINARY CONTROL.** Two runs of one unmodified binary do NOT agree on this game — not on
> `correct_light` (frame-rate-dependent), not on `dungeon1` (frame-rate-driven monster AI, ~4%
> floor). Quote the PAIRED EXCESS (G515), never the identical count.
>
> | you need to | read |
> |---|---|
> | know why a sky/defect detector said "clean" | `g536_detect.py` is not a sky gate |
> | dump frames for a cross-arm comparison | **§3.2a** (one dump path per tag) + **§3.2b** (never key on the host tick) |
> | catch a ONE-FRAME or ALTERNATING defect | **§3.2c** — `-SfDump` structurally cannot see it |
> | gate a lever with no runtime arm | **§3.2d** — cross-binary A B B A |
> | gate a VU1 EXECUTION lever | **§G610** |
>
> Companions: `appendix-dc2-test-routes.md` (routes) · `appendix-dc2-attribution-recipes.md`
> (attribution) · `appendix-dc2-graphics-facts.md` (already-diagnosed defects).

---

### ⛔⛔ `tools/g536_detect.py` IS NOT A SKY GATE ON ITS OWN (G596 §6)

Its predicate fires only when a straight seam separates a dark achromatic block from a block that is
**still bright and chromatic**:

```python
if sd < DARK_SAT and md < DARK_MEAN and ss > SKY_SAT:   # ss = the OTHER block
```

A sky that has been replaced **ENTIRELY** has no bright chromatic block left, so the detector is
silent — and `has_sky()`, its **denominator**, uses the same bright-chromatic test, so such a frame
is not even counted. **`defectPerSky=0.0%` is therefore compatible with every sky in the run being
destroyed**, and G596 quoted exactly that number twice as a clean-sky proof while a real defect was
present on ~15% of exterior frames.

> ⭐⭐⭐ **A DETECTOR THAT NEEDS THE HEALTHY TISSUE STILL PRESENT CANNOT SEE A TOTAL FAILURE — and if
> its denominator uses the same test, the rate reads 0.0% rather than undefined. Identify the scene
> by a feature INDEPENDENT of the thing being judged.**

Run these beside it:

| Tool | What it does |
|---|---|
| `tools/g596_skybroken.py <tag>...` | Identifies a MAP-15 **exterior** by its GREEN HILLS (mid band) — independent of the sky — then judges the sky band separately. Prints every broken tick. |
| `tools/g596_skyband.py <tag>... [--list]` | Full per-frame sky-band brightness/saturation **distribution**, not a verdict (G535: an alternating defect is invisible to a golden single-frame gate). |

Current baseline on the shipped binary: **`skyBroken` = 2 of 19 exteriors on the SCRIPT clock**
(scriptFrames ~1958–2440, ~5.4 s), an OPEN pre-existing defect caused by **0x139 residency**
(`PS2_PROJECT_STATE.md` gap 2, `plans/phase-G598-fix-log.md`), not a regression. A gate run must
compare against that, not against zero. ⚠️ The older `1/20` and `3/20` figures were sampling
artefacts — do not quote them.

### ⛔⛔ 3.2a ARM EXACTLY ONE DUMP PATH PER TAG (G603)

`DC2_FRAME_DUMP=1` and `DC2_G598_DUMP_SF=<every>` are **two writers of the same
`captures/frame_<tag>_<N>.ppm` namespace**, and `N` means something different in each:

| path | `N` is | honours `DC2_G598_SF_LO`/`_HI`? |
|---|---|---|
| `DC2_FRAME_DUMP=1` (+ `_EVERY`) | the **host present tick** | no |
| `DC2_G598_DUMP_SF=<every>` | the **`scriptFrame` bucket** | yes |

Setting both gives one directory in which some files are tick-keyed and some are script-keyed, and
**the two key spaces overlap**, so no post-hoc filter separates them. A cross-arm diff over that set
compares different scenes and reads as a catastrophic pixel regression: G603's first pair scored
median `mean|A−B|` = 8.20 with 10% of frames identical, and re-running with the script path ALONE
was the whole fix.

- `::dc2::dumpFramePpm` does **not** consult `DC2_FRAME_DUMP`, so `DC2_G598_DUMP_SF` needs no
  companion flag — pass it on its own.
- The tell: a key **above** your `DC2_G598_SF_HI`. Only the unbounded tick path can produce one.
- Generic script-clock scorer: `python tools/g603_sfdiff.py <tagA> <tagB>` — intersects the two key
  sets and reports the whole-frame `mean|A−B|` distribution (median / p90 / max / %identical), so a
  rate is never quoted over a denominator one arm never reached.

### 3.2b ⛔ NEVER COMPARE TWO ARMS' CAPTURES ON THE HOST TICK (G598)

`frame_<tag>_<NNNNNN>.ppm` is named for the **host present tick**, which the present loop increments
at ~58 Hz whether the guest advanced or not. The route — pad script, cutscene camera, scene — runs
on `scriptFrame = f(n)`. So two arms with different frame times write **the same file names for
different halves of the script**: on this route the default arm reached scriptFrame 10216 in 120 s
and `DC2_G400_NO_RESIDENT_139=1` (2× the frame time) only 6097, yet both produced 116 files named
`000060..006960`. A defect rate read off those two sets compares different scenes. Even the *same*
binary with the *same* flags reads `1/20` and `2/19` on two runs, purely from sampling phase.

| Do this instead | |
|---|---|
| `tools\run_g536_map15.ps1 -Tag <t> -SfDump 88` | whole script, ~1 s buckets, keyed on `scriptFrame` |
| `… -SfDump 2 -SfLo <lo> -SfHi <hi>` | dense capture over a window (per-guest-frame at `every=2`) |
| `… -GsAtSf <sf>` | packet capture gated on the **script**, not the tick (`-GsAtTick` has the same trap) |
| `… -Map` | `[G598:map] tick=… n=… sf=…` per guest frame, to audit an existing tick-keyed set |
| `python tools/g598_pair.py <tagA> <tagB>` | scores both arms **only on shared keys**, reports each arm's unshared coverage, and prints the whole-frame `mean\|A−B\|` distribution |
| `python tools/g598_bisect.py <ctl> <arm>…` | n-arm table: `ext` / `broken` / `dCtl.mean` / `dCtl.max` |
| `python tools/g598_window.py <tag> [lo hi]` | per-key sky profile + `dPrevTop` vs `dPrevAll` (static garbage vs live FBO data) |
| `python tools/g598_tbp.py <a.gs> [<b.gs>]` | `(FRAME.fbp, TEX0.tbp, TEX0.psm)` drawn-vertex census / two-arm diff |

### 3.2c ⛔⛔ `-SfDump` CANNOT SEE A ONE-FRAME DEFECT — AND NEITHER CAN A WHOLE-FRAME METRIC (G600)

3.2b's rule is about comparing two ARMS. It does not make the script clock a general capture: it
fires **at most once per GUEST frame**, while the host loop presents several times per guest frame
in a dumping run, so it samples **one arbitrary present of each frame**. A 471-frame script-keyed
capture of the `s05` hair-band flicker — a defect the user can see instantly — contained **not one
bad frame**.

And the obvious detector fails too. That defect changes ~2% of the frame by ~25 luminance, i.e.
**~0.5 in the whole-frame mean**, which is *below* the frame-to-frame mean of the scene's own idle
animation: a whole-frame spike test reports **0 spikes** on a capture where it is obvious by eye.

| For a one-frame / alternating defect, do this | |
|---|---|
| `DC2_FRAME_DUMP=1 DC2_FRAME_DUMP_EVERY=1 DC2_G600_TICK_SF_LO=<sf> [_TICK_SF_HI=<sf>]` | one PPM per **HOST PRESENT**, bounded to a script-clock window so it stays affordable |
| `python tools/g601_flicker.py <tag> [--tile 8]` | **FINDER.** Per-**8×8-tile** spike `min(\|i,i-1\|,\|i,i+1\|) − \|i+1,i-1\|`: animation drifts (≤0), a one-frame defect returns (≫0). Same frames score **140–147** where the whole-frame test scored 0 |
| `python tools/g601_band.py <tag> [<tag2> …] [--x0 … --y1 …]` | **GATE.** Once the defect's tiles are known, score only those: region mean median/min, per-tile spike, and the count of dropped presents. ⛔ Do **not** gate with the finder — its whole-frame ranking mixes the defect with whatever else moves fast (Monica's hair ornament scores 40–70), and the two arms of an A/B never present on the same host ticks, so their top rows are different moments |
| ⭐ a decode/probe count is often a better gate than pixels | For the hair band, `[G600:tex]` firing (a guest-VRAM decode of `tbp=0x2740`) is **one-for-one** with a dropped present: 5 decodes → 5 bad guest frames → 8 bad presents. A mechanism counter needs no tick alignment at all |
| `… -Set @('DC2_G598_MAP=1')` beside it | lets you tell a bad NEW frame from a repeat present — on `s05`, 0 of 26 repeat presents were bad and 53 of 98 new-frame presents were, which is what excluded a latch/tear cause |
| `DC2_G600_TEXPAINT=1` | paints every texture a colour keyed on its base, so one bad frame names the texture base of every painted pixel (it found `tbp=0x2740`). ⛔⛔ **It only paints VRAM-DECODED textures.** An unpainted region therefore means *the FBO-bind path ran there*, **not** *that base did not draw there* — G600 §14 read the good/bad colour flip as "a different base COVERS the band" when it was reporting **which path ran**. The band was not covered; it was missing (G601 §4). |
| `DC2_G601_REJ=1` / `DC2_G601_MAT=1` | once a bad frame is isolated: **why** a bilinear FBO bind was refused, and what a `g261Materialize` actually published (`guarded=` pages withheld, plus the mean of the region of interest in the FBO buffer). Uncapped and script-clock windowed, unlike `[G263:rej]`/`[G262:bindrej]` which cap at 16/24 lines **from process start** |

⭐ **The whole-frame delta on the NON-defective keys is the alignment proof.** Two correctly aligned
arms read `median 0.06` there even at different frame rates; a large baseline delta means the runs
are not aligned and no verdict from them means anything. Quote it beside every rate.
⚠️ `-SfDump` writes a 637 KB PPM on the GS worker (24.8 → 28.9 ms/f at `every=88`) — **never quote a
frame time from a run that had it set.**

### 3.2d ⭐⭐⭐ GATING A **COMPILE-TIME** LEVER — the cross-binary A B B A (G602)

Some levers have no runtime arm at all. A pure code-SHAPE change (outlining, inlining, frame/spill
layout) cannot be selected by an env var, because the second arm would be a second COPY of the hot
code inside the same function — and 17d §3.1 measured that at **1.35 ms/f for 6,196 bytes nothing
ever branches to**. So the arms are two BINARIES.

```powershell
# 1. build candidate, save it INTO build64/Release (it resolves ~40 DLLs from its own directory)
& 'D:\ps2r\dc2\build_rt.bat'; & 'D:\ps2r\dc2\tools\build_runner_norefs.bat'
Copy-Item build64\Release\dc2_runner.exe build64\Release\dc2_runner_<tag>cand.exe -Force
Copy-Item build64\Release\dc2_runner.map build64\Release\dc2_runner_<tag>cand.map -Force
# 2. flip the #define, rebuild, save as ...ctl.exe, then flip back and rebuild
# 3. order-balanced A B B A, same route, same window, workload invariant on BOTH arms
tools\g525_route.ps1 -Route s05 -Tag <t>a1 -Exe <...>cand.exe -Set @('DC2_G434_INV=1')
tools\g525_route.ps1 -Route s05 -Tag <t>b1 -Exe <...>ctl.exe  -Set @('DC2_G434_INV=1')
tools\g525_route.ps1 -Route s05 -Tag <t>b2 -Exe <...>ctl.exe  -Set @('DC2_G434_INV=1')
tools\g525_route.ps1 -Route s05 -Tag <t>a2 -Exe <...>cand.exe -Set @('DC2_G434_INV=1')
python tools\g602_pair.py --a <t>a1 <t>a2 --b <t>b1 <t>b2 --n0 1021 --n1 2461 [--tag G147:gif]
```

Five rules, each bought by a G602 failure:

1. ⛔⛔ **DO NOT GATE IT ON `avgFrameMs`.** Window pairing removes the SCENE variance (the route's
   frame time ranges 18→40 ms and the scene is a fixed function of `n`) but **not the SESSION
   drift**, because the arms are two processes at different times — which is exactly what
   `DC2_G419_AB` exists to randomise away *inside* one process. G602's two order blocks read
   **+0.063** and **−1.224 ms/f** on the same pair of binaries with `kicks/f` agreeing to 0.002%.
   Gate on a **thread-local sub-timer** instead: `[G147:gif] parserOther` read **−0.924 / −1.048**
   with **25/25 windows in both blocks** (se 0.051, so it resolves a 0.15 ms lever).
2. ⛔ **Require BOTH blocks to agree in sign.** A pooled `t` across two disagreeing blocks is an
   artefact of pooling. Report it as inconclusive, not as the payoff.
3. ⛔ **Do not re-bracket the window after the pre-registered one fails.** G602's `ridepod` boss
   window (`n=1441..1861`, the one the G525 survey names) read +0.314 with blocks disagreeing;
   widening to `n=1441..3661` makes both blocks agree at −0.28. That is a garden of forking paths —
   record it as context only.
4. ⭐ **Verify BOTH binaries' shape against their own `/MAP`**, not just the candidate's. G602's first
   control used plain `inline` for the rollback and MSVC **declined** it, leaving an intermediate
   shape with 4 of `vertexKick`'s 9 XMM spills — which would have measured the phase short.
   `__forceinline` in the rollback branch restored 9/2/4 exactly.
5. ⭐ **Quote the workload denominator in the direction it moved.** G602's candidate decoded
   **+0.18% MORE** descriptors in the same scene windows, making −11.78% a lower bound.

**Exactness gate for a semantics-neutral lever is PIXEL IDENTITY, not a defect rate:**
`tools\run_g536_map15.ps1 -Tag <t> -SfDump 88 -KeepFrames -Exe <...>` on each binary, then
`python tools\g598_pair.py <ctlTag> <candTag>`. G602 read **median = p90 = 0.00** whole-frame
`mean|A−B|` over 106 shared script keys — *below* the **0.06** that tool reads for two arms that are
merely both correct. Its single 6.79 outlier was `-SfDump` catching a different host present of the
same guest frame (§3.2c), identifiable because both arms' band metrics matched to 3 decimal places.

---

## §G610 — gating a VU1 EXECUTION lever (the native block compiler and anything like it)

⛔⛔ **`s03` CANNOT GATE A LEVER THAT CHANGES SPEED.** Two independent reasons, both measured in G610:
its headroom mean is **−0.19 ms/f** (below the ~1 ms/f a whole-route frame gate resolves), and its
scripted replay **reaches different content at different speeds** — two runs of one binary differing
only in one env flag executed **1396 vs 954 pairs/kick**. Always check `[G483:runprof] pairs/call` on
both arms before quoting anything from `s03`.

> ⛔⛔ **THE `ridepod` ROW BELOW IS SUPERSEDED (G612).** G611 removed 1.8 ms/f of VU1 pair-loop time
> and **the pole crossed**: re-derived on the shipped G611 binary over 354 windows, `ridepod` is
> **87.6% GS-poled**, GS own 22.60 against VU1 busy **20.55**, headroom **+2.05 ms/f**, 6 of its 7
> windows over 33.33 ms GS-poled. **It can no longer gate a VU1 lever at all** — the frame follows
> GS there now. Use it as a stable-workload MECHANISM route (`pairs/call` ≈ 1350) and nothing else.
> Full table: `plans/phase-G612-fix-log.md` §1.

| purpose | route | why |
|---|---|---|
| **the frame gate (post-G612)** | **`s05`** | The only route where VU1 still leads: **79.5% VU1-poled**, GS own 17.61 vs VU1 busy 18.43, headroom **−0.83 mean / −2.97 in the VU1-poled windows**. ⚠️ **Blocked estimator only** — its raw estimator suffers a ~10% arm imbalance on a phase-varying cutscene, and in G610 the two disagreed in SIGN (raw +0.063 vs blocked −0.524 ± 0.221, t = −2.37, `fav` 56/69). Read `fav`. |
| the biggest remaining VU1 PRIZE (not a gate) | `s03` | 78.3% VU1-poled, VU1 busy **32.56** — the largest VU1 number anywhere — headroom −3.83 mean / **−7.35 in the VU1-poled windows**, and **54 of its 87 windows over 33.33 ms are VU1-poled**. ⛔ Still cannot gate a speed-changing lever (below). |
| ⛔ superseded | `ridepod` | Was the frame gate from G610 to G611. **Now 87.6% GS-poled with POSITIVE headroom.** |
| **the MECHANISM sub-timer** | any | `DC2_G483_RUNPROF=1` → `[G483:runprof] loopNsPerPair`, a per-pair rate from three clock reads per kick INSIDE `run()`, immune to GS stall. ⚠️ **`pairs/call` is the workload invariant — if it differs between arms the comparison is void.** |
| ⛔ never | `[G303:vu1w] busyMs/f` | a WALL interval. G610's garbage-rendering arm read 693.4 ms/f of which **682.2 was `gsStall`** — two worker threads ping-ponging, not VU1 work. |
| **the graphics gate** | **`correct_light`** (`tools/run_g237_capture.ps1`) | The selection table's row for **VU1 MAC / flags / culling / clip** AND for the **VU1 scalar pipeline / Q latency**. Assert arrival by `DngStatus=5` + `[F59:frame] n=180 frame=380` + the six `[G197:vudump]` headers + `[G234:vuin] kicks=24`. |

### ⚠️ `correct_light` IS NOT DETERMINISTIC ACROSS FRAME RATES — IT NEEDS SAME-BINARY CONTROLS

Capture with `DC2_FRAME_DUMP=1 DC2_G598_DUMP_SF=30` (script-clock keyed; a host-tick pair is
meaningless when the arms differ in speed). Then run **five** arms, not two — G610's measurement:

| pair | what differs | byte-identical script frames |
|---|---|---:|
| `on` vs `on2` | nothing (same exe) | 116 / 160 |
| `off` vs `off2` | nothing (same exe) | 111 / 160 |
| **`off` vs `off3`** | **SPEED ONLY** (`DC2_G303_VU1_SLOW_US=2`, lever OFF) | **12 / 160** |
| `on` vs `off` | the lever | 118 / 160 |

**The verdict is the PAIRED EXCESS, never the raw identical count.** A lever passes when its agreement
sits inside the same-binary controls' own range and every key where both controls agree exactly is
reproduced by the speed-only control. Also read the diff MAGNITUDE: G610's worst surviving frame was
77% of bytes differing but **95% of them at |Δ| ≤ 3** — a one-step global fade, not a rendering
difference.

⚠️ **THE ROUTE'S BASELINE AGREEMENT IS NOT A CONSTANT — RE-MEASURE THE CONTROLS EVERY TIME.** G610
read 116/160 and 111/160 for its two same-binary controls; G612, on the same route with
`DC2_G598_DUMP_SF=30` and 184 shared keys, read **4/183 and 2/184**. Anyone who reused G610's numbers
as the expected range would have failed a perfectly clean lever. **Use `tools/g612_framepair.py`**
(route-agnostic: byte-identical count + whole-frame `mean|A−B|` median/p90/max + per-frame
`%bytesDiff` / `p95|Δ|` / `max|Δ|`) and compare the LEVER pair against the CONTROLS taken in the
same session. G612's reading:

| pair | differs | identical | median mean\|Δ\| | p90 | max |
|---|---|---:|---:|---:|---:|
| `on` vs `on2` | nothing | 4/183 | 0.011 | 0.122 | 3.918 |
| `off` vs `off2` | nothing | 2/184 | 0.020 | 0.133 | 6.345 |
| **`on` vs `off`** | **the lever** | 2/184 | **0.011** | **0.089** | 4.159 |
| **`on` vs `off2`** | **the lever** | 5/184 | **0.011** | **0.076** | 4.091 |
