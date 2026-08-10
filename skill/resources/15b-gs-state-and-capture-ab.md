# Reference: GS State, Capture A/B, and Lever Doctrine — When the PIXELS Are Wrong

> **Load this when the geometry is in the right place but looks wrong**: black/blue/grey surfaces,
> wrong colours or blending, a texture that samples the wrong page or CLUT, depth that keeps the
> wrong fragment, a render target that composites stale bytes, presentation tearing/shear. Also
> load it for **any** graphics diagnosis method — §2 (packet-level `.gs` A/B) and §3 (lever
> doctrine) apply equally to VU1-class bugs.
>
> **If the SHAPE is wrong** (missing/exploded/mis-scaled geometry, per-vertex lighting, culling,
> flag-gated microprogram branches) it is a VU1 interpreter bug →
> `15-vu1-interpreter-correctness.md`.

---

## §1 GS State Checklist — Texture / Colour / Render-Target

> When geometry is positioned right but the *surface* is wrong (wrong texture, wrong palette,
> banding, black/blue regions, corrupt fonts), the bug is in GS state, not VU. **Log the full
> GS state for the offending draw and diff it against PCSX2.** The state that matters:

| Register / concept | Bug it causes |
|--------------------|---------------|
| **PSM / CPSM** (pixel + CLUT storage format) | Format mismatch → garbage texels / wrong palette. T8/T4 palette textures often uploaded **CT32-aliased** (DPSM=PSMCT32, TBW=2×DBW) and sampled as native T8 — don't "fix" by changing the format. |
| **TBP0 / TBW** (texture base ptr / buffer width) | Wrong texture page bound → wrong or missing texture. |
| **CBP / CSA** (CLUT base / start addr) | Wrong palette colours. |
| **TEX0 / TEXA** | Per-batch texture binding + alpha expansion for 16/24-bit. Per-strip TEX0 interleaving matters — collapsing many TEX0 into one bind de-interleaves texture from geometry. |
| **CLUT upload + TEXFLUSH** | Stale CLUT/texture cache → previous frame's palette. Always check CLUT invalidation. |
| **Swizzle** (PSMT8/PSMT4/PSMZ addressing) | Block/column swizzle wrong → scrambled texture. PSMT4HL/4HH host-to-local payloads are packed **4 bpp** even though the dest nibble aliases a CT32 word — consume low then high nibble, stop when `TRXREG` full; never infer bpp from VRAM storage width. |
| **CLAMP modes** | Wrong wrap → smeared/repeated edges. Apply per sampled coord incl. bilinear neighbours: REPEAT masks by size, CLAMP uses edge, REGION_CLAMP uses MIN/MAX, REGION_REPEAT = `(coord & MIN) \| MAX`. |
| **FRAME / RTT (fbp)** | Render-to-texture left bound → a later screen draws into the wrong buffer (flat-blue / wrong bg). Restore FRAME after RTT. |
| **ZBUF / TEST / scissor** | Z-write/compare or scissor wrong → missing or z-fighting polys; scissor clips to bands. |
| **ABE / alpha blend** | Wrong transparency; "black transparent square" cursors = untextured ABE highlight ALPHA/TEXA. |
| **MMAG/MMIN (filter)** | UI fonts are usually **point-sampled** (tex1 LSBs=0) at native res — don't "fix blur" by forcing bilinear; it's a no-op or makes it worse. |

> **Depth-as-texture trap:** a PS2 depth buffer viewed as RGB shows artificial colour banding
> (integer depth wraps across byte channels). Games sometimes sample a depth channel (often
> green) for fog/effects. Before treating an odd gradient as a colour-texture bug, verify the
> source isn't depth.

> **Flat single-colour screen triage — check the CAMERA before VU/GS:** a whole-screen flat
> colour (clear colour only) has three distinct causes; discriminate cheaply before deep work.
> (1) **Null/missing scene camera → zero view matrix → 100% frustum cull.** The game's own
> PreDraw culls every part, so the entire draw CHAIN still executes — the discriminator is
> canary overrides down the chain: map/parts/piece draws FIRE but the visual-emit stage
> (`mgCVisualMDT::Draw`-equivalent, the thing that builds VU packets) gets ZERO dispatches
> ⇒ "culled", not "unreached". Then read the scene's camera fields (count / active-index /
> slot objects) and A/B them against PCSX2 at the same point — a count of 0 or active −1
> usually traces to a scene-`Initialize`/`AssignCamera` init step that never ran (null-vtable
> dispatch no-op, or a debug re-entry that re-ran Initialize without re-assigning — the
> G127/G193 class). (2) **ADC all-cull** (VU flag table above). (3) **RTT FRAME left bound**
> (GS-state table above). Cause 1 is init-order, not rendering — no VU/GS work will fix it.

> **STQ is Q-premultiplied — know YOUR sampler's interpolation convention:** the GS receives
> `ST` premultiplied by Q and reconstructs the texture coordinate per pixel. A runtime's CPU
> rasterizer may instead interpolate the PRE-DIVIDED `s·1/|q|` affinely in screen space — visually
> fine at typical triangle sizes, and it becomes the project's verified baseline. Any second
> implementation (GPU port, fast path, SIMD rewrite) must match THAT convention, not the spec:
> reconstructing "correct" perspective against a premultiplied-affine baseline samples ~q×
> off-scale → wrap-noise / near-black 3D while FST/UV 2D stays perfect (the tell: menus right,
> world wrong).

> **Runtime-STUB GS writes bypass the FIFO — a whole race class:** any sceGs* stub (SwapDBuff /
> PutDrawEnv / PutDispEnv…) that applies GS registers *directly* on the EE thread injects state
> BETWEEN the worker's queued packets once the GS drain is threaded/pipelined. On real HW those
> land as GIF packets in stream order. Symptom: transient wrong-colour/state for a few frames,
> only under threading levers, invisible to sparse sampling. Fix at source: synthesize an A+D
> GIF packet and submit through the same arbiter path (never route per-frame stub writes through
> a frame-gate — measured pipeline serialization — and never defer a SUBSET of a register's
> writers — breaks EE program order). Audit stubs when a "only under pipelining" colour race
> appears; greps for the register struct miss writes through local references.

### §1.1 Authoritative GS memory geometry & swizzle (PCSX2 `GSLocalMemory.{cpp,h}`)

**Invariants (all PSM):** VRAM = **4 MB** (`m_vmsize`). **Page = 8 KB**, **block = 256 B**,
**column = 64 B**. A page is 32 blocks; the block-within-page order is itself **swizzled**, not
linear — never compute a texel address as `bp*256 + y*stride + x`. Use the per-PSM block-number
`bn(x,y,bp,bw)` / pixel-address `pa(x,y,bp,bw)` swizzle (PCSX2 `GSLocalMemory.h:407+`).

**Per-PSM geometry** (`GSLocalMemory.cpp:177–216`; pixel dims `W×H`):

| PSM | store bpp | **transfer bpp** | block (px) | page (px) | CLUT entries |
|-----|----------:|-----------------:|-----------|-----------|-------------:|
| PSMCT32 / PSMCT24 | 32 | 32/24 | 8×8 | 64×32 | — |
| PSMCT16 / PSMCT16S | 16 | 16 | 16×8 | 64×64 | — |
| PSMT8 | 8 | **8** | 16×16 | 128×64 | 256 |
| PSMT4 | 4 | **4** | 32×16 | 128×128 | 16 |
| **PSMT8H** | 32-aliased | **8** | (swizzle32) | (swizzle32) | 256 |
| **PSMT4HL / PSMT4HH** | 32-aliased | **4** | (swizzle32) | (swizzle32) | 16 |
| PSMZ32/24/16/16S | as CT | as CT | as CT | as CT | — |

**Swizzle table per PSM** (`GSLocalMemory.h:1131+`): CT32/CT24 → `swizzle32`; CT16 → `swizzle16`;
CT16S → `swizzle16S`; T8 → `swizzle8`; T4 → `swizzle4`; **T8H, T4HL, T4HH → `swizzle32`** (they live
in the high bits of a 32-bit word — this is why they read back "CT32-aliased"); Z formats use the
same tables with `m_blockAddressXor = 0x18` (Z swizzle XORs the block address). **trbpp ≠ store
width** for the H-formats: a T4HH IMAGE transfer is **4 bpp packed** even though each pixel aliases a
32-bit word — consume low-then-high nibble and stop when `TRXREG` is full (matches the real-port G5
finding; do NOT infer transfer bpp from VRAM storage width).

**CLUT (`GSClut`):** the palette is a **1 KB on-GS cache**, not sampled from VRAM each texel. TEX0
fields that drive it: `CBP` (CLUT block base in VRAM), `CPSM` (CLUT format — must be a *valid* CLUT
format: PSMCT32/16/16S, i.e. `0/2/0xa`; an invalid `cpsm` is a corrupt TEX0 setup, not a real
palette), `CSM` (1 = swizzled CSA layout, 2 = linear), `CSA` (start slot, 16-entry granularity for
32-bit), `CLD` (load-control: when/whether to reload the cache). A stale palette = missing CLUT-cache
invalidation on `CLD` change — check this before suspecting the texels.

**Alias-proof rule for GPU admission:** conservative GS overlap helpers may deliberately include an
extra page/block for unaligned bases or uncertain swizzles. That is a safety margin, not proof that
two aligned rectangles alias. If the guard dominates CPU fallback, enumerate the exact physical
page set for the measured `(bp,bw,psm,x,y,w,h)` tuples and compare sets. Remove slack only for the
proven aligned tuple; retain the conservative answer for unknown, unaligned, wrapping, or new
layouts. Never generalize one adjacent-page proof into a global weakening.

---


---

## §2 Methodology — How to Actually Diagnose a Graphics Bug

1. **Classify triangle coverage first.** A valid GS state histogram (FRAME/ALPHA/CLAMP/ZBUF/
   TEST/scissor/XYOFFSET/colour all expected) **cannot rescue bad guest geometry.** Bucket the
   emitted primitives (tri / tristrip / trifan counts, on-screen %, centroids) and compare to
   the reference. If coverage is wrong, the bug is upstream (VU/EE), not in GS state.
2. **Prove packet delivery before debugging packet contents.** If an entire sub-mesh/batch
   disappears, but the batches that do reach VU1 have hardware-matching inputs/outputs, trace the
   complete delivery chain: EE packet emission -> DMA tag/link -> chain-walker termination -> VIF
   unpack -> MSCAL/XGKICK. Log the total tag count, termination reason, and every hardcoded walker
   budget. Camera/pose/order sensitivity can move the same packet across a fixed cutoff and mimic
   a mathematical cull. Treat runaway guards as corruption detectors, not guessed workload limits:
   warn when approached, distinguish malformed cycles from valid long chains, and size them from
   observed workloads with headroom.
3. **Split geometry vs lighting/colour.** Probe VU-output XYZ separately from RGBA. Positions
   match HW but colour wrong → lighting consumption (§2 Q-latency, light-matrix channel order).
   Positions wrong → transform (§2 dest-mask, vf0, matrix).
3b. **"Draw happens but nothing appears" → per-draw PIXEL ACCOUNTING, not more state audits.**
   For the suspect draws only, count four stages in the rasterizer: bbox pixels iterated →
   inside (edge-test pass) → z-test pass → writePixel reached. One run pinpoints the killing
   stage (DC2 G232: 4511 inside → 4511 zfail → 0 writes = Z, after a full GS-state audit had
   "cleared" everything). Two traps this method retires:
   - **A global Z-disable lever (`NO_ZTEST`-style) is NOT a valid Z rule-out** for an element
     drawn BEFORE other geometry covering the same pixels — with Z off, painter's order lets
     the later draws overpaint the restored element, so it stays invisible and Z gets falsely
     exonerated. Only per-pixel `zi` vs `zdst` accounting on the exact draws rules Z in/out.
   - When Z IS the killer, dump the full **Z history at one watched pixel** (every draw
     touching it: source, zi, zdst, pass, zwrite) — the sequence names what owns the depth the
     element loses to, and comparing the same relationships in the HW `.gs` (order + z VALUES)
     splits "wrong GS state/order" from "wrong transformed Z", which points back to
     EE/VU/physics DATA. A "missing" element can be a physics/animation divergence wearing a
     Z-cull costume (DC2 gem: accessory-chain physics settled behind the torso).
4. **A/B against ground truth** (`12-pcsx2-mcp-playbook.md`): breakpoint the same draw in PCSX2,
   read the registers/memory, find the FIRST divergence. **Constraint:** the PCSX2 DebugServer
   maps EE RAM + scratchpad ONLY — it **cannot** read VU micro-mem (`0x1100xxxx`) or GS
   (`0x12000000`) (they read 0). For VU/GS internal state, use an **offline `.gs` GS dump**
   (capture from PCSX2, parse the packet stream + VRAM) as the reference instead.
5. **Probe with env-gated levers, never hard-edits** — see §5.
6. **Loop:** OBSERVE (which primitives/state) → LOCATE (VU? GS? VIF? EE upload?) → UNDERSTAND
   (what does HW emit here — from the `.gs` dump / PCSX2) → DECIDE (one fix tool) → VERIFY
   (re-capture, re-diff). Same loop as `13-decisional-brain.md`, with the *reference* being a
   captured frame, not stdout.
7. **Verify normal downstream composition.** An internal texel/batch oracle proves only its local
   boundary. GPU residency can still publish the wrong temporal version to a later CPU transfer,
   presentation latch, or composite. Inspect multi-frame output through the ordinary present/dump
   path: character body parts, terrain, shadows, overlays, and transition frames are pass/fail
   evidence. Any new regression blocks promotion even when the oracle is zero-bad and FPS improves.

### §2.0 Packet-level GS-stream A/B (the highest-leverage graphics method — use it EARLY)

Aggregate statistics (prim counts, ADC %, on-screen %) can MATCH the reference while the render
is still wrong — a real port matched per-prim ADC budgets for 20+ phases while the per-strip
patterns were completely different. The method that finally cracked it, reusable as-is:

1. **Dump the runner's GS stream in the SAME container the HW reference uses.** Hook the single
   GIF choke point (the `submitGifPacket`-equivalent that sees PATH1/2/3) and write a minimal
   PCSX2-v9-shaped `.gs` file: 44-byte header (8B magic + 9×u32 with `sho=36, shs=0, ss=0`) +
   0x2000 zero "priv regs" + type-0 transfer records `[u8 0][u8 path][i32 size][data]` (path:
   1=PATH2, 2=PATH3, 3=PATH1). **Every `.gs` parser you already wrote now runs on BOTH sides.**
2. **Tag PATH1 records with the VU packer PC** via a synthetic 32-byte A+D record writing GS
   reg 0x0F (NOP) — old parsers ignore it, your census tool decodes it → per-packer attribution
   on the runner side without touching parsers.
3. **Census per (TBP × prim × strip):** classify every tristrip GIFtag by its per-vertex ADC
   pattern — `ALLDRAW / PRIMED (leading ADC then drawn body) / MIXED (true restarts) /
   ALLNODRAW` — plus fog-byte fingerprint (XYZF2 word3 bits 4–11: fog-only ⇒ copy packer;
   fog+2048 band ⇒ transform packer), GIFtag structure (NREG/REGS), GS PATH byte, and
   per-record composition (one XGKICK = one record = one packer run).
4. **Geometry-JOIN strips across sides** (same TBP + vert count, nearest centroid): the verdict
   table `HWcategory → runner(category, packerPC)` names the diverging layer directly — e.g.
   "HW PRIMED strips are geometry-identical to runner ALLNODRAW strips from the transform
   packer" pins the defect to VU flag execution, exonerating routing/EE/copy in one shot.
   Matched strips give a **bit-exact per-vertex ADC oracle** for validating any fix.
5. **For a colour mismatch, fingerprint the exact packet before tracing arithmetic.** Match by
   GIFtag, loop count, semantic path, TBP/state, and geometry—not only by an aggregate texture-page
   colour census. Then walk the bad RGBA qword backward: packet record/provenance → VU data qword →
   last SQ/SQI writer → producing instruction. This cleanly distinguishes a faithful packer from
   already-corrupt lighting input. Prefer value-triggered traces (saturation/non-finite/range) over
   first-N logs; the failing batch may follow many correct batches.
6. Static scene + multi-frame dump → dedupe strips by content hash; camera drift makes borderline
   guard verts flip category across frames — treat small off-diagonals in the join as drift noise
   before suspecting the fix.
7. **`.gs` parser traps that manufacture false conclusions** (each cost a real port a phase):
   - GS IMAGE payload is stateful and can continue across transfer records. Track remaining IMAGE
     qwords **per semantic path** and treat them as raw texture bytes until exhausted; parsing a
     continuation qword as a new GIFtag manufactures fake draws and colour populations.
   - A transfer record's numeric path byte follows the dump-container convention, not the literal
     GS path number. In the v9-shaped format above, raw byte `3` is PATH1, not PATH3. Keep synthetic
     VU-PC marker state scoped to PATH1 records and never let it contaminate PATH2/PATH3.
   - Preserve RGBA provenance: record index, semantic path, explicit PACKED/A+D write versus
     inherited state. Do not merge populations whose colour origin is different.
   - **PACKED GIF descriptors 0x06/0x07 are direct TEX0_1/TEX0_2 writes** — a parser that only
     handles TEX0 via A+D misses every VU-emitted texture bind and reports one stale page as
     "a consolidated atlas".
   - Track **FRAME/TEST/ZBUF/TEX0 per context** and resolve the effective CTXT through
     PRMODE/PRMODECONT (PRMODECONT=0 ⇒ attributes come from PRMODE, not PRIM).
   - PACKED RGBAQ packs R@0 / **G@32 / B@64** / A@96 (a low-64-bit read fabricates "pure red"
     verts); PACKED ST carries **Q in bits [95:64]**; PACKED XYZF2 Z is [91:68] (not a plain
     hi-word mask); A+D kicks via addresses 0x04/0x05 use X[15:0] Y[31:16] Z[55:32]/[63:32].
   - When the repro is IN MOTION (character falling/walking), compare SIGNS and relationships
     (element-z minus body-z, pinned vs free vertices) across runner/reference — absolute
     values never match across poses; the mirror/ordering signature is pose-invariant.

### §2.1 Probe methodology (hard-won — a black texture is the canonical example)

- **Prove the data chain end-to-end; the DECISIVE probe is "does any write EVER target the exact
  thing the consumer reads."** For a black/wrong texture, don't assume "the upload was dropped."
  Probe in order: (1) is the upload fn called, with what `dpsm/dbp`? (2) is the pixel-copy
  (`processImageData`) called with NONZERO source bytes? (3) does data land in VRAM (per-region
  nonzero scan)? (4) **does any BITBLTBUF ever write the exact page the draw's TEX0 samples?** A
  real port spent a phase on a "dropped CLUT IMAGE" theory; (1)-(3) all passed, (4) was zero — the
  bug was an **upload-destination vs draw-reference address divergence** (game binds `tbp/cbp` the
  texture manager never uploads to), not a transfer drop.
- **An uncapped "does X ever happen?" counter beats a capped sample log.** Capped windows get
  swamped by boot/title traffic and miss the steady state. A single uncapped tally (`hitAddr=0`
  over the whole run) gives a clean yes/no. If a capped trace is load-bearing, print its saturation
  state and prove the interesting window occurred before the cap; silence after saturation is not
  a negative result.
- **A transition detector identifies an interval, not necessarily the writer.** A sentinel checked
  at draw/transfer choke points only proves that bytes changed since the previous check; another
  thread or a direct helper can write between visits. Keep a short ring of preceding events and
  pair it with source audit or a causal lever. Host page protection is page-granular: the trapped
  address is the first access to that page, not automatically the exact texel/qword that changed.
- **Find the WORKING control through the same machinery.** If one texture renders (e.g. the title
  font) and another doesn't, the IMAGE/sample path is sound — the fault is isolated to what differs
  (the failing path's address/descriptor setup). Diff the working vs broken bind.
- **Confirm the test actually REACHED the intended state/mode before concluding anything.** On a
  slow recompiled runner a 30 s headless window may only reach a few hundred frames; scripted input
  scheduled later simply never fires. Count the inject/marker, then assert a mode-specific visual or
  state signature (for example, a zoom camera may remove the player/HUD). "Input injected" is not
  enough if it fired during loading or used the wrong byte order. Prefer a live input recording,
  convert raw scePad bits through the project's replay convention, widen short pulses when the
  headless clock can skip them, and distinguish input `scriptFrame` from frame-dump tick numbering.
- **Reproduce suspicious frame evidence serially.** If an image viewer or batch loader shows a
  black/missing region, reopen that exact on-disk file alone and check simple pixel/region statistics
  before diagnosing renderer state. Tooling/concurrent-preview artifacts must not become regressions
  or fixes; persistent on-disk composed output is the evidence.

---


---

## §3 Diagnostic-Lever Doctrine (applies to ALL fixes, critical for graphics)

Graphics fixes are easy to "prove" by eye and wrong under the hood. Protect yourself:

- **Gate every behavioural change behind an env flag.**
  - A fix that ships ON → add a **kill-switch** (`<PREFIX>_NO_<NAME>` disables it) so a
    regression can be bisected without a rebuild.
  - A change that only PROVES a diagnosis → make it **opt-in, default-OFF**
    (`<PREFIX>_<NAME>=1`). Never ship a proof-lever as the fix.
  - **Read every gate ONCE into a `static const bool`** (magic statics are thread-safe) — never a
    per-call `getenv`/env-check in a per-vertex/per-draw/per-tag/per-packet path. Windows CRT
    `getenv` is µs-class (env lock + linear scan) and worker threads serialize on the lock; one
    uncached gate in a vertex-kick path cost 41% of the whole frame while masquerading as an
    architectural "parse/dispatch" cost for four phases (DC2 G268; see
    `17-performance-optimization.md` §3 hotspot #2).
- **NO PER-SCREEN FIXES (hard rule).** Never patch a symptom by writing game state
  (camera/projection/matrix/render-target) per-frame scoped to one screen. Shared globals leak
  the write into CONCURRENT screens (e.g. a title fix that moves a character model on another
  screen sharing the front-end state). **Diagnose to the ROOT and fix where the game itself
  would set the value, once.** If a scoped lever is needed to prove the diagnosis, gate it
  opt-in and don't ship it.
- **A gate/discriminator must be PROVEN to discriminate before you ship it.** A per-screen draw
  suppression gated on `loop==3 && titleMode==2 && menuId==0x17` (assumed "unique to the leak")
  black-screened the legit costume screen — because those exact globals were **identical** between
  the legit screen and the leak. A/B the two states and DIFF the gating field: if the field you're
  keying on reads the same in the case you want to keep and the case you want to suppress, the gate
  cannot work. Don't assume a state is unique — measure it.
- **GS-state predicates are not lifecycle predicates.** A FRAME/ZBUF/TEST tuple that correctly
  identifies the target draw in a steady scene can also appear during boot, loading, fades, menus,
  or render-target setup. Before promoting a GS/raster fix to default-on, pair the GS predicate with
  a guest-state readiness predicate when available (loop/state id, live scene pointer, valid
  camera/map/active indices). Then validate both the final target frame and the transition/loading
  frames that lead into it. If deferred or threaded replay is involved, capture the readiness scope
  per queued draw entry and replay that snapshot; do not read a later global scope from worker
  threads. If the fix improves the environment but leaves character/shadow/RTT defects, document it
  as a partial fix and split the remaining pass instead of widening the scope blindly.
- **Clamp original-game bugs at the runtime boundary, never "fix" game-side.** Some defects are
  in the original game (e.g. an oversized DMA transfer the real DMAC tolerates). Absorb them in
  the runtime (stop when the destination rectangle is full), don't alter recompiled game logic.
- **A GS page address is storage, not feature ownership.** Games aggressively alias the same VRAM
  pages across costumes, work buffers, depth pyramids, menus, and transitions. A synthetic clear or
  workaround keyed only by FBP/TBP/shape can silently hit an unrelated later lifecycle. After the
  source defect is repaired, A/B-retire the workaround on both its original route and every known
  aliasing route; do not preserve it by adding another per-screen exception.
- **A passing internal oracle never waives the presentation gate.** If a residency/readback arm
  produces missing character parts or terrain in normal composition, keep it default-off. Repair a
  bounded, evidence-proven ownership edge in-phase; if a new versioning/consumer mechanism is
  required, revert unsafe behavior, retain quiet diagnostics, document the exact blocker, and split
  a focused follow-up.
- **Don't delete a band-aid while a deeper blocker is unresolved** — it regresses to the
  earlier broken state. Mark band-aids with their kill-switch and the condition for removal.
- **After a ROOT fix lands, SWEEP ALL older band-aids — especially pc-scoped interpreter
  patches — not just the ones named in the current phase.** A real port's missing water was a
  70-phases-old "enable fix" that ORed bits into a VU register at three hardcoded microcode PCs:
  written when that subroutine was misread as a "batch enable gate", it was actually the
  CLIPPER's inside/outside test (the forced bits meant "outside" → the clipper emitted nothing,
  forever). The root fixes (opcode table + flag pipeline) had long since made it unnecessary —
  but it survived because band-aid sweeps only covered the band-aids the active phase knew
  about. Keep a grep-able registry: every pc-/address-scoped patch carries its env switch and
  the ORIGINAL hypothesis; when a root fix invalidates the hypothesis, kill the patch and A/B.
- **Never cap a microcode/code dump "to the interesting part".** The same defect went
  unattributed for 20+ phases because the VU program dump stopped at the last known packer —
  the entire clip subsystem (a third of the program) sat past the cap, undisassembled, and its
  work kept being attributed to routes that were actually visible. Dump 0..codeSize always;
  disassemble the WHOLE program once per program and index its XGKICKs/branch targets.

Cross-refs: runtime A/B + tool catalog `12-pcsx2-mcp-playbook.md`; original-code understanding
`14-static-analysis-navigation.md`; hardware register/format details `db-registers.md`,
`db-vu-instructions.md`, `08`→`09-ps2tek.md`; fix taxonomy `10-agent-guardrails.md §3`.
For THIS project's exact capture harnesses see `appendix-dc2-project.md` §4–§5; for already-diagnosed
DC2 graphics defects and proven addresses see `appendix-dc2-graphics-facts.md`.
