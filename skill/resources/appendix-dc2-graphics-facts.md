# Appendix: Dark Cloud 2 — Proven Graphics Facts & Historical Renderer Snapshots

> **Load this ONLY when a DC2 graphics symptom matches one of the rows below**, or when you need the
> historical renderer state to interpret an old fix log. It is a lookup table of already-diagnosed
> DC2 defects, not a playbook — the method lives in `15-vu1-interpreter-correctness.md` (geometry)
> and `15b-gs-state-and-capture-ab.md` (pixels).
>
> Always-read DC2 facts (paths, build, harnesses, PCSX2 wiring, regen rules) stay in
> `appendix-dc2-project.md`. Live status is in `plans/ROADMAP.MD`.
>
> **These are point-in-time findings.** Each names its phase; verify against current code before
> asserting one as fact.

---

## §1 DC2-Proven Graphics Facts

> Instances of `15-vu1-interpreter-correctness.md` (geometry-class) and
> `15b-gs-state-and-capture-ab.md` (pixel-class).

Proven fixes and operating facts (some unconditional, some retired, others kill-switch gated):
- **G629–G636 GPU-Resident Renderer Arc & Defect Diagnoses (G636, 2026-08-22):**
  - **Defect A: G630 `ridepod` Chassis Black / Character Polygon Loss:**
    - *Root Cause:* The CPU rasterizer wrote the host private depth mirror `g403DisplayZBuf()` outside of band replays (e.g. between resident replays, ~12,000 words in rows 161..401 matching the ridepod silhouette). The resident GPU replay then performed depth testing against stale Z and rejected valid character polygons. Additionally, the resident replay linear mirror stored unconditionally across its whole dispatch grid into `m_fbos[fbp]`, erasing native draws in shared FBOs.
    - *Repair:* Removed the persistent GPU depth mirror (batches requiring depth take the standard CPU band replay) and retired the linear FBO mirror.
  - **Defect B: G634 Environment Dim / Stale Textures (0.77x brightness ratio):**
    - *Root Cause:* `g634TryRawView` skipped the texture pre-pass without checking if dirty G261 resident FBO targets overlapped the sampled memory. Live texels remained in the native FBO while the raw image decoded stale data.
    - *Repair:* `g636G261DirtyOverlaps()` arbitration predicate wired into `g634TryRawView`, `g630TryT8View`, `g630PrepareView`, and G633 equivalents to fail-closed and force materialization.
  - **Defect C: G629 "Flashlight Flicker":**
    - *Root Cause:* Also caused by the CPU private depth mirror in the non-resident compute replay arm (`DC2_G629_GPU=1`), causing occasional invalid depth tests and additive light blooms on the ridepod chassis.
    - *Repair:* Unconditional refusal of depth batches in `g629_tri_replay`.
  - **Final Disposition:** All three defects eliminated (**100% bit-exact 0.00/0.00/0.00** on 53 keys of `ridepod` opening sf 768..1392). However, end-to-end performance closed negative (+1.13 to +1.51 ms/f slower than control) because refusing private depth removed 57% of batches, leaving addressable wait savings (~1.2 ms/f) below added resident costs (~1.8 ms/f). **Prototype remains DEFAULT-OFF for measured performance reasons.**
- **`s05` Monica's hair-band flicker fixed (G601, 2026-08-16):** an **EMPTY FOOTPRINT SATISFIES A
  "FOR ALL TAPS" ADMISSION CRITERION — IT DOES NOT FAIL IT.** The band is a two-pass offscreen
  effect: the guest clears `x=403..492 y=125..211` of `FRAME fbp=0x13a fbw=8 CT32` to
  **`RGBA=(0,0,0,0)`** (so the RTT's ALPHA is the mask), draws the band mesh into it with
  `TEX0 tbp=0x2720/T8/tbw1 64x64`, then composites it onto the display with **51 sprites** reading
  `TEX0 tbp=0x2740 tbw=8 CT32 512x512 TCC=1 TFX=0`, `ALPHA=(Cs−Cd)*As+Cd`, `TEX1=0x261` (bilinear),
  `CLAMP=0x5`, `fst=1`, `iip=0`. Two vertex colours only: **3 sprites at `(128,128,128,128)`** — a
  native-colour copy, i.e. the band itself — and **48 at `(80,60,0,128)`** — the gold rim. The band
  is **not** in the `0x139` scene RTT, so losing that read deletes it outright.
  ⭐ On every 4th guest frame one composite sprite is sub-pixel on one axis and covers **no GL pixel
  centre**, so it has no nonzero-weight tap; `g263BilinearSpriteBind` returned false
  (*"nothing provable"*), its fallback ran `g261Materialize(0x13a)`, the G536 page-ownership guard
  withheld **73 pages** (exactly `0x13a`'s guest-VRAM overlap with sibling `0x159`, published two
  targets earlier in the SAME per-entry pass) — and `dirty` was cleared anyway, so the other 50
  sprites hit `if (!r.dirty) continue;` and decoded **black**. Fix: admit the empty primitive; a
  tap-bounds QUERY still fails closed. Rollback `DC2_G601_NO_EMPTYBIND=1`.
  ⚠️ **Still true and still armed:** `g261Materialize` clears `dirty` even for guard-withheld pages,
  and `0x13a`/`0x159` overlap by 73 pages while both hold full-height 512×416 residency windows
  (the guest only renders rows ~125..211 of `0x13a`). On hardware there is no conflict — the guest
  consumes `0x159` before it writes `0x13a`.
- **Map 125 shadow-edge residue fixed (G408, 2026-07-29):** treat a textured alias pass as
  a consumer until the source bytes are proven clean. Here, the visible T8 pass
  (`TBP=0x2720`) reads the same live memory as shadow target `FBP=0x139`
  (`0x139 << 5 = 0x2720`). Paired untextured `TRIFAN` shadow volumes use add/subtract
  `ALPHA=...68/...62`, `FIX=0x80`, and source RGB `(1,1,1)`. Inclusive half-pixel
  barycentric coverage left uncancelled RGB=1 shared-edge samples; T8/CLUT decode merely
  exposed them as the diagonal dark trail beside Max. Fix only this exact class with integer
  GS sample positions plus 12.4 fixed-point top-left half-open edge ownership. Roll back with
  `DC2_G408_LEGACY_SHADOW_COVERAGE=1`.
  - Use `tools/run_g407_render.ps1`; pass `-G408Rollback` for the same-binary causal arm.
    Compare against `ref/dumps/map_125.{gs,png}`. The existing `.gs` dump is sufficient and
    includes a clean reference screenshot; do not request a new dump unless that artifact is
    missing or corrupt.
  - Preserve the claim boundary: full G406 rollback and G407 legacy-wide UV arms retain the
    same Map 125 dots. Therefore G406 is innocent **of this defect**, and G407 did **not**
    improve these dots. G407's debug-text/HUD correction remains valid.
  - Before changing STQ, bilinear sampling, or CLUT decode for an aliased texture artifact,
    dump/trace the producer target first. If the bad byte already exists in the RTT, repair
    producer ordering, blending, or coverage rather than the downstream sampler.
  - See `plans/phase-G408-fix-log.md`; final release A/B artifacts are
    `captures/g407_g408_release_{candidate,rollback}` and
    `captures/g408_release_verified_ab.png`.
- **Scene-camera chain (G193, the town flat-blue root):** every loop-init (TitleInit /
  InitDungeonMain / EditInit / sgInitGyoRace) does `scene->Initialize()` via the CScene vtable
  (`*(scene+0x10548)`, `__vt__6CScene=0x375FE0`, slot +8 = `Initialize__6CSceneFv@0x282EA0`)
  then `AssignCamera@0x283740` per camera id. Initialize sets camera count `scene+0x2044`=8,
  active `scene+0x2E54`=-1, clears the 0x38-stride slot array at `scene+0x2048` (obj at +0x34);
  AssignCamera bounds-checks id against count, populates the slot, first success sets active.
  Null vtable ⇒ the whole chain silently degrades to "count 0, everything culled". Repair =
  `f50_4_repair_main_scene_vtable` at BOTH InitDungeonMain (F50.4) and EditInit (G193) entry.
  Healthy state on real HW at a town: count=8, active=0, slot objs non-null — A/B these five
  words first for any flat scene.
- **`vf0=(0,0,0,1)` pinned** after context memset (G40) — else matrix inverses zero → skinned
  characters collapse.
- **VU1 MAC flags computed** per FMAC (G71); **STATUS** derived from MAC. Kill `DC2_NO_VU1_MAC`.
- **VU1 Q pipeline latency modelled** (G87, DIV/SQRT 7, RSQRT 13; WAITQ commits). Kill
  `DC2_VU1_NO_QLATENCY`. Was the title-rock neon-green (point-light attenuation read stale Q).
- **VU1 FDIV busy-stall / Q commit-before-rearm modelled** (G200). A second DIV/SQRT/RSQRT
  issued while Q is pending must commit the in-flight result first; otherwise behind-camera
  MAP-0 geometry that HW 100%-skips partly draws as a foreground sheet. Kill `DC2_VU1_NO_QSTALL=1`.
- **VU1 lower scalar-stall ordering + P/EFU pipeline fixed** (G239). Repro:
  `Debug menu -> Down x2 -> Circle -> Square -> Left -> Cross`; reference
  `ref/dumps/correct_light.{png,gs}`. The decisive pair is `RSQRT` followed by
  `MULq VF2.x,VF2,Q | WAITQ`: real VU1 tests the lower wait/busy pipe before the paired upper,
  while the old upper-first interpreter let MULq consume stale Q, exploded the normalized
  lighting basis toward `FLT_MAX`, and produced flat `(255,255,255,128)` strips. The runtime now
  pre-publishes pending Q/P for `WAITQ`/`WAITP` and busy scalar producers; causal kill
  `DC2_VU1_NO_SCALAR_PRESTALL=1`. A separate delayed-P model implements ESADD/ERSADD/ELENG/
  ERLENG/ESUM/ESQRT/ERSQRT/ERCPR/WAITP (kill `DC2_VU1_NO_PPIPE=1`); disabling only P latency
  leaves the route correct, so it is a sibling semantic fix, not the lighting cause. Exact packet
  `302dc00000008039:0000000000000412` has 57 packed ST/RGBAQ/XYZF2 loops; scalar-prestall ON/OFF
  yields 0/64 saturated lighting stores, and the fixed `tbp=0x2820` mean RGBA `(50.6,69.8,64.1,127.0)`
  closely matches HW `(52.6,72.3,67.1,127.1)`. Harnesses `tools/run_g237_capture.ps1` and
  `tools/run_g237_vugs_ab.ps1`; parsers `tools/g234_map_rgbaq.py` and
  `tools/g239_dump_gs_records.py`; final artifacts `captures/g239_final_001800.png` and
  `captures/g237_g239_fixed.gs`. Title golden remains exactly 211646 and MAP-4 stayed healthy.
  See `plans/phase-G239-fix-log.md`.
- **VU1 float clamp** (`vuDouble`) implemented opt-in `DC2_VU1_CLAMP` (kept OFF, validated no
  regression but unproven broadly).
- **COP2 dest-mask lane reversal** (F51.8) — the 50-phase dungeon-black root. After ANY regen,
  re-apply `tools/fix_cop2_destmask.py` or dungeon 3D transforms regress.
- **Split VIF1 IMAGE continuation** (G6), **T4HL/T4HH packed 4-bpp** (G5), **CLAMP per-coord**
  (G5), **CT32-aliased PSMT8 fonts** — see `PS2_PROJECT_STATE.md` "GS / rendering" + the
  `plans/phase-G{5,6}-fix-log.md`.
- **VU1 lower-opcode table corrected** (G138): real mapping `0x18=FMEQ, 0x1A=FMAND, 0x1B=FMOR,
  0x1C=FCGET` — the runner had FMEQ/FMAND SWAPPED for the entire G70–G137 title arc. Kill
  `DC2_VU1_NO_FMSWAPFIX`. **MAC/STATUS flag pipeline** (4 instruction pairs) modelled; kill
  `DC2_VU1_NO_MACPIPE`, depth `DC2_VU1_MACPIPE_DEPTH=4`. Together they made the title transform
  packers' per-vertex ADC **bit-exact vs the HW dump**; the `DC2_G100` forced-draw band-aid is
  retired by default (re-enable `DC2_G100_FORCE_DRAW=1`). See `plans/phase-G138-fix-log.md`.
- **Packet-level GS A/B harness** (G138, = `15b-gs-state-and-capture-ab.md §2.0`): `DC2_G138_GSDUMP=<path>`
  (+`DC2_G138_GSDUMP_MAX`) dumps the runner's GIF stream as a PCSX2-shaped `.gs` with per-XGKICK
  packer-PC markers (`ps2_memory.cpp submitGifPacket`); census `tools/g138_hw_slice.py`
  (`--dedupe --strips out.csv --seq N`), geometry-join `tools/g138_join.py`. Reference dump
  `ref/dumps/new_game_via_debug.gs`+`.png`.
- **VIF1 chain-walker cutoff fixed (G217):** MAP-4's character RTT legitimately exceeds 4096
  nested/local DMA tags. The old `kMaxChainTags=4096` stopped before the static-view head/cap
  packets; camera rotation moved them to indices ~3988-4071 and made the defect look like culling.
  Runtime now uses 16384. If a whole batch is angle/order-dependent while arriving batches are
  VU-byte-identical to HW, check chain-tag headroom before VU/skin theories. Diagnostics:
  `DC2_G217_EEOBJ=1`, `DC2_G217_TAG_VU=1`, `tools/g217_pcsx2_head_objects.ps1`.
- **MAP-4 zoom shared-page ordering fixed (G219/G220):** the zoom cycle copies the display into
  work page `fbp=0x139/tbp=0x2720`, defers a display composite, then clears the work page. G219
  routed `sceGsSwapDBuffDc` clear packets through the GIF arbiter FIFO (kill
  `DC2_G219_DIRECT_SWAPCLEAR=1`) and proved capture-time blue became replay-time zero. G220 found
  the remaining writer: G37's host-only costume clear directly bulk-zeroed the same aliased page.
  That workaround is retired default-OFF; `DC2_G37_FORCE_CLEAR=1` reproduces the old boundary.
  Repro/reference: `tools/run_g218_map4_zoom.ps1`, `tools/g219_black_region.py`,
  `ref/dumps/map_4_zoom.gs` + `.png`. `DC2_G219_GUARD` is page-granular, so use its first-access
  RIP only with a source audit and causal lever; it is not an exact-byte watchpoint.
- **Missing environment game-wide = MIXED DEPTH AUTHORITY on ZBP `0xd0` (G534, 2026-08-09):**
  environment surfaces are replaced by whatever was drawn behind them (on `map_4_zoom` the near
  ground becomes the sky; in interiors the floor/walls become unrelated surfaces). Cause: G273
  relaxed G242's colour/Z alias reject for `fbp=0x68`, which put BOTH display buffers on the GPU
  wave path at once. The depth authority they then share is the G403/G411 private PSMZ24 mirror,
  which CPU-rasterized draws for the same ZBP also write (`g403DisplayZ` is gated on
  `g203Uni && g404SharedZScope`, **independently of G411**). Mixed CPU/GPU depth lets late sky
  geometry win the depth test. G534 retired the G273 admission (`s_g273ExactAlias` default-OFF, cost
  null: lean MAP-0 51.82 vs 51.89 ms/f) — ⛔ **but that did NOT fix the defect.** G535 (2026-08-09)
  re-measured it at **24.8% of `map_4` frames** on the shipped G534 binary, and both `G273` arms are
  broken (39.0% on, 29.3% off), so G273 was never the cause. **The real cause and fix are in G535
  below.** G534's gate missed it because three hand-picked `map_4_zoom` ticks are not a distribution
  — the defect ALTERNATES, so any single frame is ~70% likely to look clean.
  ⚠️ The page-disjointness arithmetic
  (`g273AlignedRectsOverlapExact`, `[G273:alias] rows(c=0..415 z=0..415)`) is **correct** — do not
  re-audit it. Probes: `DC2_G534_ZSEQ=1` (which targets share a depth key, and whether a wave
  uploads or rides the retained texture), `DC2_G534_ZDIFF=1`, `DC2_G534_ZSRC_VRAM=1`,
  `DC2_G534_FULLZ=1`, `DC2_G534_NOSKIP=1`. Repro `tools/run_g218_map4_zoom.ps1` vs
  `ref/dumps/map_4_zoom.png`. See `plans/phase-G534-fix-log.md`.
- **⭐⭐⭐ Missing environment game-wide, THE ACTUAL FIX = the private-mirror depth deferral
  (G535, 2026-08-09):** `g411PrivateDepth` forced `g278OwnerEligible = true` unconditionally, so the
  ZBP `0xd0` depth publication was always deferrable. That justification covers only the colour/Z
  page-alias race. The mirror's OTHER consumer is the **CPU rasterizer** (`g403DisplayZRead` /
  `g403DisplayZWrite`, gated on `g203Uni && g404SharedZScope`, independent of G411), and every
  ranged publication path skips `privateMirror` owners ("no VRAM transfer consumer" —
  `g278FlushPendingDepthForRanges`, `g278FlushPendingDepthForDisplayOwners`, `g528VerifyResidual`).
  The INLINE edge (`g278FlushPendingDepthForInline`) already published them; the **batched CPU band
  replay was the last uncovered consumer**. Fix: `g528PublishScoped` publishes any active
  private-mirror owner — default-ON, **+3.51%** on lean MAP-0. Rollback `DC2_G535_NO_EDGEPUB=1`
  (in-wave, +42.9%); ⛔ `DC2_G535_PRIVZ_DEFER=1` reproduces the defect.
  ⚠️ **Do NOT remove the other three `privateMirror` skips by symmetry** — those edges really are
  VRAM-transfer/display consumers, and "fixing" them reintroduces the +42.9%.
  ⚠️ **`DC2_G411_NO_GPU_SHARED_Z=1` also removes the defect but costs +126.6%** — G411's headline
  SURVIVED where G273's decayed to null, so it was never a candidate for retirement.
  ⭐ **Gate a defect like this on the ADJACENT-FRAME delta of a static route, never on one frame:**
  it alternates, so any single frame is ~70% likely to look clean, which is exactly how G534 passed
  three hand-picked ticks while a quarter of all frames were broken.
  See `plans/phase-G535-fix-log.md`.
- **⭐⭐⭐ Garbage blocks in the sky / any texture that lives inside a resident RTT's window
  (G536, 2026-08-09):** on MAP-15's cutscene the sky carries **256×64 axis-aligned blocks of dark,
  achromatic, vertically-streaked data**, seams pinned to screen `y=62/126/190` and `x=256` while
  the camera pans, content changing every frame. **One bad block = one PSMT8 page (128×64 texels)**
  of the cloud texture at `TBP=0x2720` — and `0x2720 = 0x139 << 5`, i.e. that texture lives inside
  GPU-resident render target `0x139` (`kG261Fbp[0]`), which is held at `fbw=8`, `cov=0..447`, so its
  claim spans ~blocks `0x2720..0x3520` and also swallows `0x2820/0x2860/0x34xx` streaming pages.
  **Root cause:** `G261Res::genAtSkip` is compared ONLY where a new wave decides `uploadFb`
  (`rasterizer_vram_materialization.inc`), never at `g261Materialize`, which publishes
  `dirtyLo..dirtyHi` **at full width** from the FBO into guest VRAM. Any guest write the G264 CT32
  upload mirror did not absorb is therefore overwritten by render-target pixels, which the T8 cloud
  draw then decodes as palette indices. Probe `DC2_G536_MATCHK=1` measured **5120/15115 (33.9 %)**
  publications writing over moved VRAM. **Fix:** page-ownership-aware publication — `G261Res`
  carries `g536PageGen[]` (per-page twin of `genAtSkip`, anchored in `g261UpdateWindow`) and any
  page whose gen moved is preserved across `g418UnpackColorRows`. Default-ON, null cost; rollback
  `DC2_G536_NO_PAGEOWN=1` ⛔ reproduces the defect. Route `tools/run_g536_map15.ps1`, gate
  `tools/g536_detect.py` (0/89 sky frames vs 13.3–13.6 %), reference `ref/dumps/map_15.{gs,png}`.
  ⚠️ **`[G261:invariant]` silence does NOT exonerate a publication** — that check never runs at the
  publication edge. ⛔ Do not re-chase G530/G526/G291/G310/G264/G522/G289 for this class; each was
  refuted by its own arm. See `plans/phase-G536-fix-log.md`.
- **⭐⭐⭐ MAP-15 TOTAL background replacement — a SECOND, distinct defect at the same address; it is
  a DEPTH defect (G598 diagnosis REFUTED by G599, 2026-08-15):** for scriptFrames **~1958–2440** (~5.4 s,
  script-deterministic) the whole MAP-15 exterior background — sky *and* distant hills — is replaced
  by a smooth stretched stone/crack texture with a soft dark block. **This is not G536's variant:**
  G536's was 256×64 page blocks whose content **changed every frame**; this one's sky-band
  frame-to-frame delta is **0.00** while the camera pans (whole-frame delta 4.7), i.e. a **stale
  read**, not live FBO data.
  **The guest is innocent (G598, stands):** both arms gated at the same script instant
  (`DC2_G598_GS_AT_SF`, `sf=1981 n=902`), `(fbp,tbp,psm)` census reads **`fbp=0x13b tbp=0x2720
  psm=19 (PSMT8)` = 14,352 in both, delta 0**. Whatever is wrong is entirely in the port.
  ⭐⭐⭐ **CAUSE (G599, 2026-08-15): the shared ZBP `0xd0` GPU DEPTH — NOT the VRAM content.**
  Five arms take it **8/28 → 0/28**: `DC2_G411_NO_GPU_SHARED_Z=1` (80.8 ms/f),
  `DC2_G265_NO_DISPLAY_ZWAVE=1` (76.6), `DC2_G262_NO_WIDE=1`, `DC2_G203_LEGACY_Z=1` (69.5),
  `DC2_G400_NO_RESIDENT_139=1` (45.6). All are 1.8–3.3× too slow to ship. The broken band is **near
  cliff geometry winning the depth test**: `DC2_G534_NOSKIP=1` (re-upload the CPU mirror over the
  retained GPU depth) turns the **whole frame** into that same rock, which also proves the retained
  texture is the current copy and the mirror the stale one.
  ⛔ **G598's cause is REFUTED.** `[G599:who]` fingerprints the pages the sky's PSMT8 view occupies:
  **one distinct hash per page, identical in the 8/28 arm and the 0/28 arm**, 550 reads each.
  Deleting the ENTIRE guest-VRAM write of every 0x139 publication (`DC2_G599_NOPUB=139`, **at the
  control's own 25.4 vs 24.8 ms/f**) reads 8/28, as does forcing the VRAM→FBO upload
  (`DC2_G599_FORCEUP=139`). Denying every FBO texture source (`DC2_G599_NOBIND=1`) makes it **worse**
  (15/28) — the bind is protective.
  ⛔ **REFUTED at 8/28, do not re-chase:** `G536_NO_PAGEOWN`, `G528_NO_PUB`, `G310_NO_T8_VIEW`,
  `G310_NO_LOGICAL`, `G310_NO_ALIAS_RETIRE`, `G260_NO_SKIP`, `G283_NO_AUTHORITY`, `G264_NO_UP_FBO`,
  `G288_NO_GPU_CONSUMER`, the whole READ side (`DC2_G598_T8ANY=1`, which reads `dirty=0 anyDirty=0`
  at the 128×128 consumer), `G591_NO_PRIVZ`, `G535_NO_EDGEPUB`, `G534_ZSRC_VRAM`,
  `G588_NO_SOLID_SPRITE`, `G534_FULLZ` (9/28), and the 512-wide T8 view `DC2_G599_T8WIDE=1` (built,
  **admitted** — `[G598:t8why] ADMIT … 512x512` — still broken).
  ⛔⛔ **`DC2_G400_NO_RESIDENT=13b` IS AN INERT ARM** — `g400NoResident()` switches only on
  `{0x139,0x13c,0x143,0x146,0x155}`. G598 quoted it as the control that killed the frame-rate
  confound; it measured the control binary. The confound is instead killed by `DC2_G599_T8WIDE=1`
  being **broken at 135.6 ms/f**, slower than every fixing arm.
  ⚠️ **The control flow does NOT discriminate the window** — `[G599:flush]`/`[G599:zseq]` are
  identical clean vs broken (same targets, counts, depth keys, `skip=1 pend=1 eligible=1`), so the
  next phase must compare depth **content**. Note the wide 0x139 scene batch reads `guestDepth=0
  zbp=0x0` although G404's header says the guest renders it against ZBP `0xd0`.
  See `plans/phase-G599-fix-log.md` (and `phase-G598-fix-log.md` for the script clock).
- **MAP-0 environment Z promoted with lifecycle guard** (G202): real HW z-tests town display
  geometry with `ZBUF_1 zbp=0xd0`, `PSMZ24`, `ZTST=GEQUAL`. The runtime honors that only for
  ready town/edit frames (`LoopNo==1`, live scene/camera/map) and clears Z24 page `0xd0` to
  PS2-far (`0`) on display-target transitions. Kill `DC2_G202_NO_TOWN_Z=1`, trace
  `DC2_G202_TOWN_Z_STAT=1`. Do not key this on GS state alone; loading/transition frames bind
  the same fbp/zbuf shape.
- **One ZBP has one owner across color FBPs (G405):** DC2 clears PSMZ24 `ZBP=0xd0` while display
  FBP 0/0x68 is bound, then renders the character RTT at `FBP=0x139` against that same depth
  surface. A private mirror scoped only to display FBP splits one hardware buffer into cleared and
  stale owners, so Max disappears before the final `ZTST=ALWAYS` composite. Key the mirror by
  `(ZBP=0xd0, PSMZ24, FBW=8)`, route every color target through it, and reject those entries from
  GPU waves unless the GPU shares the same cross-target owner. Rollback:
  `DC2_G404_LEGACY_DISPLAY_Z_SCOPE=1`. Reference: `ref/dumps/background.{gs,png}`.
- **VU1 same-pair upper→lower VF hazard fixed** (G139): a lower op never sees its same-pair
  upper's result on real VU1; the immediate-commit model broke the tri packer's
  store-then-clobber idiom (`SUB VF24.xyz,VF17,VF16 | SQ VF24`) → every tri's middle vertex
  stored raw edge-vector float bits = the "beam shard" spanning triangles. Kill
  `DC2_VU1_NO_PAIRHAZ`. Rasterizer band-aids retired default-off: G89 guard
  (`DC2_G89_FORCE_GUARD_CULL=1`), G104/G125 near-plane tri clip (`DC2_G104_FORCE_TRI_CLIP=1`),
  G128 behind-drop (`DC2_G128_FORCE_BEHIND_DROP=1`); the G125 title Z stays default-ON. The
  historical G139 title capture measured `PixelNonZero=211646`; it is not a current required gate.
  See `plans/phase-G139-fix-log.md`.
- **Title water / clipped geometry restored — G64 band-aid RETIRED** (G140): the HW dump's
  1012 "trifan" verts are the **VU1 polygon CLIPPER's output**, not a `0x1c50` object route.
  Title VU program layout (decoded G140): PRE-dispatcher at VU `0x5e0` routes selector-bit1
  (=`fc4` "needs clip", from `mgClipInBoxW@0x12f380`) batches to CLIP packers
  `0x21b0`(tri)/`0x23e8`(tstrip) → Sutherland–Hodgman clipper `0x2740..0x2f48` (edge sub
  `0x3088`) → fans kicked from the template at VU 780 (`0x2d88` empty flush + `0x2f38` real).
  The June G64 "enable fix" (IAND patch at pc 0x30d8/0x30f8/0x3168) had inverted the clipper's
  inside/outside test — retired default-OFF, re-enable `DC2_G64_FORCE_ENABLE_FIX=1`. Census
  lever `DC2_G140_CLIP`; the `[G39:code]` microcode dump now covers the FULL program. Golden
  historical capture count stayed `211646`. **Title render is COMPLETE vs
  `ref/dumps/new_game_via_debug.png`.** This is historical evidence, not a required smoke.
  See `plans/phase-G140-fix-log.md`.
- **GS rasterizer PERFORMANCE (G141-G145, the DC2 instance of `17-performance-optimization.md`
  `17c-perf-gs-pipeline.md` §1)** — measured: the GS software rasterizer is ~53% of the title frame; `DC2_PERF=1` prints the
  `[G141:perf]` frame split + `[G141:cov]` coverage. Shipped levers (title fps ~3.1 → ~4.6 available):
  - **Sampler eliminate-work (default-ON, bit-exact):** inlined per-triangle fast-path
    (`DC2_G141_NO_FASTSAMPLE` kills), per-row x-span (`DC2_G141_NO_XSPAN`), per-triangle CLUT-decode
    cache + texel-quad cache (`DC2_G141_NO_CLUTCACHE`/`DC2_G141_NO_TEXQUAD`). Bit-exactness PROVEN
    per-pixel by `DC2_G141_SAMPLE_VERIFY=1` (`[G141:vfy] bad=0`). Falls back to `sampleTexture` for the
    costume RTT (`fbp=0x139`)/Z-tex/non-default T8. **Finding: the sampler is `ReadP8`-bound, NOT
    CLUT-decode-bound** — micro-caching plateaus.
  - **Row-parallel raster (G143, DEFAULT-OFF `DC2_G143_THREADS=N`):** `GSRowPool` splits ONE triangle's
    scanlines across threads. +11% at N=8, then plateaus (~1.2×) — most title tris are small.
    Thresholds `DC2_G143_MINROWS`/`DC2_G143_MINAREA`.
  - **Frame-level tile-binning (G144, DEFAULT-OFF `DC2_G144_TILEBIN=N`):** defers textured tris (fbp
    0x0/0x68) into a display list, replays per-thread screen BANDS at flush. **+32–35% at N=8 (frame
    ~294→~220ms), ~3× G143.** THE bug that cost a debug cycle: mid-frame texture uploads
    (`processImageData`/`performLocalToLocalTransfer`) clobber VRAM deferred tris sample → they are now
    flush barriers. The parallel flush CRASHES from the `mgEndFrame` context (present-thread race) →
    frame-end drain is sequential. Levers `DC2_G144_SEQ`/`DC2_G144_SEQUPLOAD`.
  - **Dirty-region upload flush gate (G145, EXPERIMENTAL DEFAULT-OFF `DC2_G145_DIRTY_UPLOAD=1`):**
    replaces G144's "flush before every upload" rule with a conservative VRAM block-range overlap
    test for upload destination/source, pending texture, CLUT, and render target ranges. Unknown PSM
    or range cases flush, never skip. Kill `DC2_G145_NO_DIRTY_UPLOAD=1`. Verified title counts:
    default 211644, G144 tilebin 211650, G145 opt-in 211646, kill-switch fallback 211644. Dirty stats
    proved the lever is active (`flush=545 skip=479` sampled), but perf is modest/noisy and dungeon
    3D soak is still required before promotion.
  - **Historical title counts are not a current required gate:** a host-tick capture can still be
    FMV. Gate a perf win on the single gameplay route most likely to break from its mechanism,
    state-confirmed arrival, that route's full-frame distribution, and visual review. Open:
    dungeon-3D tile-binned soak before promoting any
    default-OFF lever to default-ON.
    Detail `plans/phase-G14{1,2,3,4,5}-fix-log.md`.
- **Presentation model = frame-boundary snapshot ONLY (G175):** the host present thread must never
  call `latchHostPresentationFrame()` per tick — that snapshots live VRAM mid-frame (~10×/guest
  frame at DC2's fps) and, on RTT-composited transitions (costume↔menu), latches
  just-cleared/partial buffers → the transition "ping-pong" (full `212480` and black `0` nonzero
  counts alternating). Double-buffered scenes HIDE this race; RTT-composite-into-display exposes
  it. The sole valid latch point is the guest frame boundary (`g150_frame_barrier` at the
  mgEndFrame hook, all modes — serial, MTGS, pipelined); present consumes
  `copyLatchedHostPresentationFrame()`. Boot fallback: per-tick latch allowed only until the
  first boundary latch (`g175_frame_boundary_count()==0`). Kill `DC2_G175_TICK_RELATCH=1`.
  Removing the per-tick latch was also worth ~+10% default fps (present-thread VRAM copies +
  `m_stateMutex` contention). See `plans/phase-G175-fix-log.md`.
- **Present upload is latch-generation gated (G176, default-ON):** `UploadFrame` skips the
  snapshot copy + whole-frame `UpdateTexture` when the presentation snapshot generation is
  unchanged (~2/3 of ticks at low guest fps). Kill `DC2_G176_TICK_UPLOAD=1`; counters
  `DC2_G176_STAT=1`. **Trap: frame dumps bypass `UploadFrame`** — dump cleanliness proves
  nothing about the WINDOW for present-path changes; use `[G176:stat]` upload-vs-generation
  tracking or eyes. Any NEW writer of `m_hostPresentationFrame` must bump
  `s_g176LatchGeneration` or the window shows a stale frame while dumps look correct.
- **`sceGsSwapDBuff` stub drawenv writes routed through the arbiter (G177, default-ON):** the
  stub used to apply 8 DRAWENV registers directly on the EE thread — a GifArbiter FIFO bypass
  that raced the worker's stream under `DC2_G157_PIPELINE=1` (transient wrong-colour rock,
  found only by dense per-tick sampling + the chroma-grid detector). Fixed by synthesizing an
  A+D GIF packet via `submitGifPacket(Path3)`. Kill `DC2_G177_DIRECT_DRAWENV=1`. REFUTED
  designs (do not retry): routing per-frame stub writes through the G157 register-slot gate
  (serializes the pipeline 9.4→6.8fps); deferring a SUBSET of a shared register's writers onto
  the worker FIFO (breaks EE program order; h=416 golden regression = the signature). The
  DISPENV privileged writes remain immediate — a bounded presentation-only residual, documented
  in `plans/phase-G177-fix-log.md`.
- **LLE GPU rasterizer, arc phase 1 (G178, OPT-IN `DC2_G178_GPU=1`)** — the DC2 instance of
  `17c-perf-gs-pipeline.md` §2. G144 deferred list → persistent GPU thread
  (release-before-share context; backend in `ps2_gs_gpu_raster.cpp`, front-end in
  `ps2_gs_rasterizer.cpp`, private interface `src/lib/ps2_gs_gpu_lle.h` — kept OUT of
  `include/runtime/` so the generated target never sees it) → FBO per display fbp →
  state-batched draws → ONE `glReadPixels`/flush written back into guest VRAM (present/dumps/
  golden gates unchanged). Whole-flush CPU fallback on any unsupported entry (title:
  `fallback=0`). Texture cache: per-page write gens (bumped by uploads, local transfers, inline
  CPU draw footprints) + source-page CONTENT-HASH revalidation — DC2 re-uploads identical
  texture bytes every frame, so gen-only invalidation thrashes (residency metric =
  `texHashHits`, NOT `texHits`). **Bring-up root-cause: the CPU sampler is AFFINE in s/q
  (GSVertex.s is Q-premultiplied; drawTriangle samples `interp_screen(s·1/|q|)·texW`)** — a
  perspective-correct shader renders the cavern near-black; all shader varyings are
  `noperspective` and the STQ attribute is pre-divided. Title-Z replicated as scoped
  (title-rock tris only, clear on fbp change); fb ALPHA stored ≠ CPU (blend-factor encoding) —
  needs dual-source blend before any RTT-alpha route. Measured: default 6.8-6.95 → 10.2-10.4
  fps alone; **16.8-17.7 fps with `DC2_G157_PIPELINE=1`** (≈ the G151 EE-bound ceiling).
  Verified TITLE ONLY. Diagnostics: `DC2_G178_STAT=1`, `DC2_G178_CENSUS=1` (state-combo
  census — run it before extending the shader to a new route). See
  `plans/phase-G178-fix-log.md` + `plans/gpu-raster-arc-plan.md`.
- **Missing chest gem arc (G226-G233, CLOSED 2026-07-12) — the DC2 instance of 15-vu1-gs
  §4.3b pixel accounting + 04-runtime §6.1 stub-semantics audit.** Final root:
  `sceVu0Normalize` stub used 4-component length (real VU0 `ESADD` = xyz only) →
  `CDAColPipe::CheckHit`'s push-out (radial vector with leftover w=1) became a pull-in → the
  DA pendant chain settled behind the torso → gem Z-culled behind the chest. Reusable DC2
  diagnostics (all env-gated, default-off): `DC2_G232_RTTDUMP=1` (RTT scans, per-draw
  bbox/inside/zfail/write accounting, watched-pixel Z history in `ps2_gs_rasterizer.cpp`),
  `DC2_G233_COLPROBE=1` (CheckHit in/out/delta), `DC2_G229_DATREE=1` (DA frame tree +
  pendant matrix), `DC2_TRACE_G226` (DA now-positions). Tools: `tools/g232_gs_zorder.py`
  (dual-context `.gs` z/order parser — handles PACKED TEX0 descriptors 0x06/0x07,
  PRMODE/PRMODECONT, per-context FRAME/TEST/ZBUF), `tools/g232_pcsx2_pendant_mtx.ps1` (live
  pendant-chain matrices + DA bind/now arrays over DebugServer). Two invalidated rule-outs to
  never repeat: `DC2_NO_ZTEST=1` cannot exonerate Z for early-drawn elements (painter's-order
  overpaint), and "physics byte-matches PCSX2" must compare the FREE chain vertices, not just
  the pinned anchor. `plans/phase-G232-fix-log.md`, `plans/phase-G233-fix-log.md`.

- **GPU render-target residency lessons (G260/G261, 2026-07-16, durable for any port):**
  (1) Before building ANY skip-readback/GPU-residency mechanism for a render target, first
  measure who CONSUMES its output per frame — if the consumers (later draws sampling it, CPU
  fallback rasterization, guest re-uploads into the same pages) still execute on the CPU, the
  round-trip merely relocates to the consumer edges and the win is small (G261: +4.5% despite a
  flawless ownership model; 1836/frame-window classifier rejects + 2.2 uploads/frame were the
  real cap). Instrument consumer-edge counters FIRST, build residency SECOND. (2) Residency
  overlap footprints must be scoped to the actually-rendered row window, never the whole
  512-row target — GS work blocks pack targets pages apart and texture-upload pages right
  behind them. (3) Guest "transfer is cheaper than space" engines re-upload scratch-page
  content every frame: cross-frame FBO residency for such targets requires routing the upload
  INTO the FBO, not materializing around it. (4) When a gen-based ownership invariant guards a
  resident surface, scope the gen sum to the same row window as the residency claim, or
  unrelated same-page-range traffic trips it falsely. `plans/phase-G261-fix-log.md`.
- **Native display admission/presentation lessons (G269–G273):** split inclusive upload/draw
  timers before optimizing; deferred graph work is charged to the barrier that pays it. Relax a
  conservative VRAM alias only with exact PSM page sets and only for the measured aligned tuple.
  Internal batch/oracle success does not prove final composition: G272 was faster but produced a
  head-only Max through the normal downstream path. G273 promoted only after dense title plus
  normal foliage/light/costume/dungeon and recorded real-zoom MAP2/MAP4 presentation all passed.
- **Current-binary premise + retirement discipline (G279, durable):** re-profile after every
  promoted phase, then bound the candidate with `events/frame × exclusive cost/event`. A large
  internal counter reduction is mechanism evidence only. Require separated same-executable,
  reverse-order end-to-end A/B plus normal composed-frame inspection. If arms overlap, order changes
  the sign, or any presentation route regresses, remove the behavior path completely; keep only
  cached default-off diagnostics, record the blocker, and open the smallest new ownership mechanism.
- **Presentation-gate blindness + gen-sum slack conventions (G277, durable):** PixelNonZero (and
  therefore the historical dense-title check) CANNOT see a character dropout over an opaque scene — MAP-0's
  count stayed 211650 while Max's body toggled on half the frames. Any display-color/residency
  lever must gate on a dense MAP-0 body-presence/content-diff check (frame-to-frame changed-pixel
  bbox, or a body-box non-grass classifier). Also: `g178BumpRectImpl`/`g178GenSumRect` both use a
  +1 slack-page convention (write AND read side) — an "exact" gen window is only exact with a
  slack-free aligned sum, and culled inline prims must not bump (clamped bbox ∩ scissor, G277).
- **Reference-backed composition + first-use state (G287, durable):** an internal exact oracle can
  prove the admitted batch and still miss a later temporal consumer; G284's range census passed
  while MAP-2 lost the whole left cliff. Bind promotion to the exact reference route/tick and check
  screen edges/background, then bisect candidate/stack/family/slice kills and re-test the rebuilt
  default. Separately verify the first eligible batch in a fresh process: G286's special exact
  branch inherited a stale texture/TFX/TCC state, and its first repaired batch stayed black until
  the level-0-only GL texture's sampler completeness was set explicitly.
- **Logical/physical framebuffer split + count-is-not-cost (G310/G311, durable):** G310 promoted a
  persistent 512x512 CT32 logical atlas (display consumers bind one texture; publication-order
  authority reconstructed by the G309 one-pass compositor) for **−11.29% frame** — the win came from
  deleting the per-target readback/materialize/T8-publication chain, not from the composite itself.
  G311 then tried to batch the "576 full composites / 1,024 waves" into page-delta updates and it was
  a **premise-gate No-Go**: a high per-frame COUNT is not a cost lever until you measure per-event
  exclusive TIME × changed-fraction. The composite was already **~230 µs** each, and the atlas has
  **no page-level temporal coherence** (RTT/work sources regenerate every frame → ~107/128 pages
  change every rebuild, `noop=0`), so the pre-built incremental page-delta path — though **bit-exact**
  (`DC2_G311_VERIFY` bad=0 / 334.8M px) — fragmented one composite-shader pass into ~5-20 synchronous
  `future.get()` round-trips and was **slower** (306 µs vs 230 µs), overlapping A/B. Kept default-off
  substrate `DC2_G311_INCREMENTAL=1`. Durable rule: when the whole surface must be rebuilt every
  frame, one composite pass beats N page copies; only revive incremental if the sources gain
  coherence or async submit removes the per-round-trip cost. `plans/phase-G31{0,1}-fix-log.md`.
- **DC2_PERF self-cost + lean draw path (G312, durable):** the G141 per-draw counter block costs
  ~855 ns/draw under `DC2_PERF=1` (~12.6 ms/f of GS-worker in every `run_g304_prof.ps1` run);
  without it the whole drawPrimitive entry→capture window is ~119 ns/draw. Never bucket-hunt
  inside a `DC2_PERF` run (arm-vs-arm A/B stays valid); use `[G312:seg]` (`DC2_G312_STAT=1`)
  without `DC2_PERF` for absolute prologue attribution. The drawPrimitive F31..G34 diagnostic
  pile costs ~17 ns/draw — do not re-attempt a skip-branch lever. Fresh bounded GS-worker term:
  serial per-pixel IMAGE deswizzle in `GS::processImageData` (~12 ms/f, ~3.9 MB/f) → G313.
  `plans/phase-G312-fix-log.md`.

---


---

## §2 Historical DC2 operating snapshot (2026-07-18, post-G287 — superseded numbers; mechanisms still valid)

**G343 durable lesson (2026-07-23):** a materialize READBACK can double as the FBO⇄VRAM snapshot
publication. Eliding an "overwritten anyway" readback (exact, row-verified, visually identical)
can still REGRESS the frame because the next GPU wave must re-establish the invalidated snapshot
from VRAM (pack + upload of ~the same rows) — readback⇄upload conservation. Before eliding any
publication edge, count BOTH sides of FBO⇄VRAM coherence: who reads the published bytes AND who
depends on the snapshot staying valid. Census: `DC2_G343_CENSUS` (l2l per-shape TIME census with
payer target `hit=`); lever kept default-off: `DC2_G343_L2L_EXACT`.

**G291 durable lessons (2026-07-18):** (1) When a residency lever is neutral, attribute the
relocation to NAMED consumer shapes (`[G279:mat]` cause + `[G266:texShape]` tuple census)
before designing the next slice — G290's "synchronous transfers" framing was wrong in detail;
the cost was ~89% tex-alias materializations with three exact shapes. (2) A read-triggered
materialize can be exactly skipped for pages FULLY re-covered by noted uploads since the last
wave render (VRAM >= FBO by construction; the round-trip is identity) — but the retire only
pays if EVERY per-frame publishing consumer of the target is retired; one whole-window
publisher collapses it (skip merely reorders who pays). (3) Early-capped bring-up prints
(`n<=8`) are NOT evidence a mechanism is inert/active overall — add a periodic aggregate
counter line before concluding.

**G290 durable lessons (2026-07-18):** (1) `DC2_G290_PROBE=1` is the first per-batch drain-
execution decomposition (prep/resolve/gpuOk/gpuFail/publish/replay/note + `[G290:gpufail]`
failing-shape census) — use it before selecting any drain-side perf slice. (2) Eliminating a CPU
replay bucket is NOT automatically a wall-time win: the admitting GPU wave pays synchronous
Z-pack/fb-upload/submit that can equal the replay (G290 three arms neutral ~1.0%). Measure the
wave's own transfer cost before promising the replay's cost as the payoff ceiling. (3) A real
physical color/Z page alias can still be admissible when the batch structurally proves the
aliased bytes (leading opaque clear + read-only ZMSK=1 depth → seeded Z window). (4) MAP-2
tick-2100 hardware comparison is mandatory after ANY native-preparation change (G287/G289 rule,
re-followed here).

**G287 is default-on:** the recurring exact transient `fbp=0x13b`, `fbw=8` work-page batch now
binds its decoded PSMT8+CLUT texture, carries TFX/TCC mode, and makes the level-0-only GL sampler
complete from first use. Final exact oracle: 120/120 zero-error batches (25,559,040 pixels; earlier
210/210). Corrected-default MAP-0 is 107.71 ms versus 169.24 ms with
`DC2_G287_NO_TRANSIENT_TARGET=1` (−36.35% frame / +57.1% FPS). Diag
`DC2_G287_STAT=1`; retained oracle `DC2_G286_VERIFY=1`.

**G284 is retired/default-off:** direct comparison to `ref/dumps/map_2_zoom.png` found its
upload-edge display-readback coalescing removed the continuous left cliff while its internal range
counters passed. G287 kill reproduced the defect; `DC2_G284_NO_UPLOAD_COALESCE=1` alone restored
the cliff. Keep the behavior explicit opt-in (`DC2_G284_UPLOAD_COALESCE=1`) until a later
consumer/authority mechanism is proven. See `plans/phase-G287-fix-log.md`.

**G280 is built/validated but DEFAULT-OFF (`DC2_G280_ALIAS_VIEW=1`; diag `DC2_G280_STAT` /
`DC2_G280_VERIFY`):** the backend-native GPU physical-alias page surface/view (GPU→GPU CT32
64x32 page-tile copies between target FBOs, contentGen versioning, overlay
restore-before-publish) is exact — 2,328 copies `vfyBad=0`, zero failures — but enabling it
REGRESSES MAP-0 (3v3 median 177.0 vs 158.9 ms; `mat(cpu) 0→1164`). **Durable lesson: a lever
that keeps RTT targets dirty longer must first close EVERY same-frame VRAM consumer of those
pages, or deferred materializations reappear multiplied at worse edges — CPU-vs-GPU resolve
cost is irrelevant (closes the G267 question). Alias sets must be re-censused per binary
(0x13c newly live vs G267's 0x146-only era). CT32 tile equivalence must NOT be extended to
T8/T4/Z without a per-PSM within-page order proof.** Next: G281 T8 swizzle-view of the
resident CT32 FBO for the censused `0x143←0x13c` consumer. `plans/phase-G280-fix-log.md`.

**G278 is default-on:** compatible same-key G262 waves coalesce Z readback/publication within a
drain while tracking exact dirty/whole row unions and stale rows. Final MAP-0 improved
`193.19→174.10 ms` (`-9.88%` frame / `+10.97%` FPS), with `saved=1899 uploadSkip=2373 fail=0
inv=0`; the full downstream route matrix passed. `plans/phase-G278-fix-log.md`.

**G279 is diagnosis/closed; no behavior remains:** always re-profile after the preceding phase.
Post-G278 backend submit was only `0.306 ms/submit`, about 17 submits or `~5.2 ms/frame`; the planned
async-submit slice could not deliver a major win. `DC2_G279_PROFILE=1` instead pinned remaining
materialization cost to G261/G276 color publication. A range-exact display prototype reduced G276
flushes `1408→587`, but reverse-order A/B overlapped and one ordering regressed, so every behavior
flag/helper was removed and only the diagnostic was retained. **G280 targets a backend-native GPU
physical-alias page surface/view**, first the measured `0x139 + 0x146` CT32 family. Do not revive a
queue-only `future.get()` removal or the G267 CPU publish/patch experiment.
`plans/phase-G279-fix-log.md`.

**G273 is default-on:** exact page enumeration for the sole measured aligned display tuple proves
FBP 0x68 CT32 color pages 104..207 disjoint from ZBP 0xd0 Z24 pages 208+. This removes a false
`colorzalias` whole-CPU fallback only for `(fbBlock=0xd00,zBlock=0x1a00,fbw=8)`; every unknown,
unaligned, or different tuple retains the conservative guard. Interleaved MAP-0 medians
`240.75→220.80 ms` (−8.29% frame / ~+9% FPS), fallback ~17–20%→~4–5%. Phase kills:
`DC2_G273_NO_EXACT_COLORZ_ALIAS=1` or `DC2_G273_EXACT_COLORZ_ALIAS=0`; stack kill
`DC2_G26X_NO_NATIVE=1`. Final default title and normal MAP-0/foliage/correct-light/costume/
dungeon/corrected real-zoom MAP2+MAP4 presentation passed. `plans/phase-G273-fix-log.md`.

**Do not re-chase the post-G268 profile:** G269 split the inclusive ~155 ms image timer into
<=10 ms CT32 writing and ~145 ms pending graph execution; its compact writer regressed and was
removed. G270 found zero steady MAP-0 line-wave coverage; the ~61.5 ms A+D number was deferred
drain attribution. G271's conflict-prefix barrier retained only ~0.5 suffix batch and stayed
noise-scale. G272 display-color residency measured ~6% but lost Max's body in normal composition,
so it remains default-off (`DC2_G272_DISPLAY_COLOR_WAVE=1`) pending a separate temporal ownership
mechanism. See `plans/phase-G269-fix-log.md` through `plans/phase-G272-fix-log.md`.

G268's durable rule still applies: all env checks in per-vertex/per-draw/per-tag/per-flush paths
must be `static const bool`; one uncached Windows CRT `getenv` cost 41% of the frame.

### §2.1 Prior snapshot (post-G266 — kept for context)

Read `plans/ROADMAP.MD` first for live status — its Current Status + levers table supersede
this snapshot when they disagree. Pre-G222 narrative (town/MAP arcs, G191 perf arc) is
archived in `plans/phase-history.md`; do not act on old snapshots of it.

- **Arc state: CPU-raster perf arc CLOSED (G259); NATIVE-RENDERER STACK G260–G266 PROMOTED
  DEFAULT-ON at G266 (2026-07-16).** Master rollback `DC2_G26X_NO_NATIVE=1`; per-slice kills
  `DC2_G261_NO_WAVE`/`DC2_G262_NO_WIDE`/`DC2_G263_NO_BILIN_BIND`/`DC2_G264_NO_UP_FBO`/
  `DC2_G265_NO_DISPLAY_ZWAVE`/`DC2_G266_NO_FBW_SPLIT`. Waves auto-disengage under
  `DC2_G255_VERIFY`/`DC2_G248_VERIFY`/`DC2_G242_GPU_DEPTH`/`DC2_G256_EXACT_RTT`/
  `DC2_G149_TEXCACHE`/`DC2_G38_VRAMDUMP` (oracles/dumps keep the per-flush readback contract).
  Post-build matched MAP-0: default 408.90 ms/2.45 fps vs rollback 443.04 ms/2.26 fps.
  First-read for any renderer/perf phase: `plans/arc-native-renderer.md`
  (objective, MAP-0 profile, stepping-stones table, G266-revised consumer attack order).
- **Default-on levers (kills):** G144 tile-bin (`DC2_G144_NO_TILEBIN=1`), G150 MTGS
  (`DC2_G150_NO_MTGS=1`), G157 pipelining (`DC2_G157_NO_PIPELINE=1`), G172 sprite-defer
  (`DC2_G172_NO_SPRITE_DEFER=1`), G178 LLE GPU raster (`DC2_G178_NO_GPU=1`), G203 universal
  guest-Z (`DC2_G203_LEGACY_Z=1`), G222 hyperbolic STQ (`DC2_G222_AFFINE_TEX=1`), G240 AFAIL
  FB/Z gating, G248/G250 RTT fast sampler (`DC2_G248_NO_FASTSPRITE=1`), G252 CPU RTT band
  replay (`DC2_G252_NO_RTT_DEFER=1`).
- **Validated default-OFF substrates (the native-renderer stepping stones — promote only with
  a paying consumer):** G260 frame-scope command graph (`DC2_G260_NR=1`, +2.0% alone) and
  G261 GPU-resident RTT waves (`DC2_G261_WAVE=1`, implies NR, +4.5% alone; zero
  ownership-invariant failures). Verifier-only/opt-in experiments: G242 GPU depth
  (`DC2_G242_GPU_DEPTH=1`), G255/G256 exact GPU RTT (requires `DC2_G255_VERIFY=1`).
- **Current binding profile (G260 decomposition of the ~435-450 ms MAP-0 frame):** ~170 ms
  banded CPU raster work + ~160 ms GIF parse/register dispatch (82k `writeRegisterPacked`/f)
  + ~100 ms host→local deswizzle. Refuted as costs: drain count/fragmentation (G253+G260),
  per-flush readback skip alone (G261, +4.5%).
- **Next executable priority (G267 candidates, census-pinned in `plans/phase-G266-fix-log.md`):**
  (a) extend G263's exact tap-footprint FBO-direct bind from sprites to display TRISTRIPs
  (bounded); (b) l2l display→0x2760 dst mirrors (G264-style); (c) T8-alias consumer 0x143←0x13c
  (decode-from-FBO); (d) fbp=0x68 color/Z-alias strip batch (display rows 416..447 alias
  zbp=0xd0 — honest, UNBOUNDED, own phase); (e) pillar-1 compact GIF parse (~160 ms, largest
  untouched bucket). Do NOT widen the classifier on drain evidence (G266 refuted) or re-chase
  the fbw split as a ms win (proven neutral; it is GPU-coverage substrate).
- Do not require the historical held-menu `frame_001500` title count: host tick 1500 can still be
  FMV. Use the single state-confirmed gameplay route most likely to break, FULL-FRAME DISTRIBUTION
  + visual review on that route, never one sample. For threading/pipelining/deferral levers, dense
  soak on that route with `DC2_FRAME_DUMP_EVERY=1`
  (G174/G249: 60-tick cadence misses transient races and temporal dropouts).
- Perf A/B point: `tools/run_g194_map0.ps1` steady DngStatus=0 windows (`[G154:perf]`), ≥3
  reps/arm, claim only non-overlapping arms. 60fps accelerant for 30fps routes:
  `DC2_PATCH_60FPS=1` (opt-in, no-op on title).
- Smoke: select exactly one gameplay route from `appendix-dc2-test-routes.md`—the route most likely
  to break from the change; assert arrival by game state and validate its captured distribution
  with `skill/scripts/ppm_nonzero.py`.
- Use the build wrappers (`build_rt.bat`/`build_runner.bat`) and read `BUILD_EXIT` from
  `build_out.txt` / `runner_out.txt`; the wrapper process exit code is not reliable.
- Loop-state legend (`LoopNo@0x00376FCC`): 0=boot, **1=town/edit loop (EditInit@0x1A9F40 /
  EditLoop@0x1ABCF0 — the debug-menu "MAP N" route lands here via EditMapJump@0x1AF4C0)**,
  2=dungeon, 3=front-end. Real-HW reference pair for the MAP-0 town route:
  `ref/dumps/map_0.gs` / `map_0.png`.
- Parked by user (regression-route only, do NOT investigate during perf phases): Sindain
  inventory circular-viewport noise. Other open residuals: Max foot shadow, G194 DOF wedge,
  TexAnime L→L width, and memcard. Audio/FMVs are restored through G386.

---
