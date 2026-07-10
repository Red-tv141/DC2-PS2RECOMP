# Reference: VU1 & GS Rendering Debug Playbook

> **Load this for any GRAPHICS bug** — wrong geometry, wrong colour, missing/extra polygons,
> wrong texture, black/blue screen, streaks, swimming verts. These bugs do **not** print a
> crash address. stdout is useless here. You diagnose them by **comparing state against a
> reference (PCSX2 / a GS dump)** and by **auditing the hand-written VU1 interpreter and GS
> emulation for silent divergences from real hardware.**
>
> This is the deepest and most time-consuming class of recomp work. Budget for it.

---

## §1 Mental Model — The Graphics Path

```
EE (recompiled game code)
  builds a DMA packet (VIF/GIF tags + data) in RAM
        │  DMA ch.1 (VIF1)            │  DMA ch.2 (GIF / PATH3)
        ▼                             ▼
   VIF1 unpacks → VU1 micro-mem    GIF ──────────────► GS (textures via BITBLT/IMAGE)
        │  MSCAL runs VU1 microprogram
        ▼  XGKICK emits GIF primitives
       GIF ──────────────────────────► GS (rasterizer) ──► framebuffer
```

Two ways geometry reaches the GS — **know which one a given draw uses**:
- **Transform path:** VU1 microprogram transforms verts (matrix multiply, perspective divide,
  lighting, clip/ADC) then `XGKICK`s primitives. Bugs here = the VU1 interpreter.
- **Copy/passthrough path:** EE already computed screen-space verts; the VU program just copies
  them out (no divide/transform). Bugs here are upstream (EE) or in the copy packer.

### VIF1-to-GS DIRECT Transfer Split Hazard
- **Mechanism:** When the game uploads textures via VIF1 PATH2 using a `DIRECT` VIF1 command, the transfer may be split across multiple DMA packets.
- **Hazard:** If the VIF1 interpreter wraps continuation pixels in a new/extra `IMAGE` tag when the GS is already in pending-image mode, the GS will consume that tag's header qword as raw pixel data. This shifts the uploaded texture buffer by one qword.
- **Symptom:** T8/T4 HUD text and textures appear scrambled, misaligned, or offset by 16 bytes (one qword).
- **Check:** Ensure the VIF1 interpreter passes pending PATH2 image qwords through verbatim without prepending tags once the transfer has entered continuation.

If a draw renders wrong, **first establish which path it takes** (count XGKICKs, check whether
the VU program does a perspective divide). Chasing the VU transform when the draw is actually a
copy wastes phases.

Runtime files (from `10-agent-guardrails.md §3.5`): `ps2_vu1.cpp` (interpreter),
`ps2_vif1_interpreter.cpp`, `ps2_gif_arbiter.cpp`, `ps2_gs_gpu.cpp`, `ps2_gs_rasterizer.cpp`.

---

## §2 VU1 Interpreter Correctness Checklist

> A hand-written VU1 interpreter accumulates **silent** divergences: it computes a slightly
> wrong value, no crash, geometry/colour comes out subtly (or catastrophically) wrong. These
> are the highest-recurrence root causes. When VU1 output is wrong, walk this list against the
> reference VU (PCSX2's `VU1` / the `vuDouble`/flag code) **before** assuming the microprogram
> or the data is wrong.

| Hazard | Symptom when broken | Mechanism / check |
|--------|--------------------|-------------------|
| **WRONG OPCODE DISPATCH TABLE** (check FIRST — poisons everything below) | A flag/branch gate is "provably never takeable" by disassembly, yet real HW visibly takes it with byte-identical microcode; whole prim classes 100%-culled or 100%-drawn | The interpreter's own case labels can mis-map opcodes — a real port shipped for ~70 phases with **FMEQ(0x18) and FMAND(0x1A) SWAPPED** (+FMOR parked on 0x1C which is really FCGET). Every disassembly "proved" an FMEQ 0/1 cascade could never equal a mask like `0xD0` — the ops were FMANDs accumulating that mask. **Verify every lower/upper opcode slot against PCSX2's dispatch tables (`VUops.cpp` `_LOWER_OPCODE[128]`/`_UPPER_OPCODE`), never against the runner's case labels. Shared-tool-bug hazard: if your disassembler mirrors the runtime's mapping, it inherits the same lie — derive tool tables from PCSX2 too.** |
| **`vf0` not hardwired** | Matrix INVERSES come out all-zero → skinned characters collapse; plain multiplies look fine (mask it for many phases) | HW `vf0` is constant `(x,y,z,w)=(0,0,0,1)`. If the context is `memset` to 0 and nothing writes `vf0`, it stays `(0,0,0,0)`. Any op reading `vf0.w` (e.g. `Q=vf0.w/det` in matrix inverse) breaks. **Pin `vf0=(0,0,0,1)` after every context reset.** |
| **MAC flags never computed** | `FMEQ`/`FMAND`/`FMOR → IBxx` branches evaluate against constant 0 → a VU branch never flips | Compute the 16-bit MAC after each FMAC op: nibbles O[15:12] U[11:8] S[7:4] Z[3:0]; **lanes X/Y/Z/W = bits 3/2/1/0** (DEST lanes only). If a flag-gated branch never flips, suspect (in order): the OPCODE TABLE row above, the flag PIPELINE row below, then the flag computation itself. (A branch that still looks "structurally never-takeable" after those three checks is usually another interpreter lie, not game logic.) |
| **MAC/STATUS flags read UN-PIPELINED** | Gates fire never/always or on the WRONG op's result; per-vertex cull/draw masks garbage; symptoms survive a correct opcode table | Real VU1 makes an FMAC's flags visible **~4 instruction pairs after issue**; hand-scheduled microcode reads them at exactly that distance (interleaving unrelated FMACs in between, NOP pairs propagate the last value). An immediate-write model hands the consumer the *interleaved* op's flags. **Model a 4-deep (mac,status) shift register advanced once per executed pair; FMEQ/FMAND/FMOR/FSAND/FSEQ/FSOR read the 4-old snapshot.** Same defect class as Q latency — audit them together. Validate the depth empirically: off-by-one (3 or 5) visibly kills the gate. |
| **STATUS flags derived from nothing** | Same family as MAC | STATUS must be derived from the MAC; if only `FSSET` writes it, it's wrong. |
| **CLIP flags** | Clip-gated VU branches wrong | Maintain the 24-bit clipping flag register from the clip judgements. |
| **ADC / Strip Restart flags** | Triangles are completely culled (flat blue/black screen) OR giant swimming sheets/overdraw polygons cover the frame | The GS uses the MSB of the vertex coordinates (ADC bit) to flag "no-draw / strip-restart". If the VU/COP2 output fails to set/clear the ADC bit (often mapping the `.w` float field to the integer MSB via FTOI4/FTOI0), you get all-cull (all-ADC=1) or no-cull (all-ADC=0). **Verify selective ADC ratio against PCSX2/GS-dump (e.g., ~60% ADC=1 on real HW).** |
| **SAME-PAIR upper→lower VF hazard (immediate upper commit)** | POSITIONS of a periodic vertex subset explode into screen-spanning textured "beam" tris while ADC/fog/draw% still match HW (the `.w` lane survives a `.xyz` dest mask); garbage coords decode as raw FLOAT BITS (x/y = low16 of a float, z24 = bits 4..27) | A lower op (SQ/SQI/SQD, DIV/RSQRT, MTIR, MOVE…) can **never** see its same-pair upper's result on real VU1 (FMAC latency ~4 cycles). Hand-scheduled microcode exploits this with a store-then-clobber idiom: `SUB VF24.xyz,VF17,VF16 \| SQ VF24 -> 5(VI6)` — the SQ stores the OLD VF24 (produced by an FTOI4 exactly 4 pairs earlier) while the upper computes the NEXT value in the same slot. An interpreter that runs upper-first with immediate commit stores the upper's fresh result instead. **Fix pattern: snapshot the upper op's VF dest (fd for main-space ops; ft for FTOI/ITOF/ABS; none for ACC/CLIP writers) before execUpper, expose the OLD value to execLower, then overlay the upper's dest-masked lanes afterwards (upper wins its lanes even if the lower wrote the reg).** Same family as Q latency and the flag pipeline — audit all three together. |
| **Q-register pipeline latency** | A point-light/attenuation/perspective value is subtly wrong → e.g. neon-green lighting, slightly-off projection | HW latches `Q` **after a delay** (DIV/SQRT 7 cycles, RSQRT 13). Microcode doing `RSQRT/DIV … (no WAITQ) … MULq` at < latency distance wants the **OLD** pipelined Q. An immediate-write model feeds the fresh result. **Tell:** mixed `MULq|WAITQ` vs bare `MULq` in the same program. Stage Q into pending+delay, commit at 0, WAITQ commits immediately. |
| **Float clamp (`vuDouble`)** | Rare denormal/inf/NaN lanes corrupt a result | HW VU clamps: denormal→signed 0, inf/NaN→±`0x7f7fffff`. Often tolerable (games rarely depend on it) — implement opt-in, validate before defaulting on. |
| **Dest-mask lane order reversed** | Partial-dest writes hit the wrong lanes → degenerate transforms (THE 50-phase dungeon-black class) | VU/COP2 lane order is X/Y/Z/W = bits 3/2/1/0 — **opposite** `_mm_movemask_ps` (0/1/2/3). Reverse before building masks. In SIMD tests use DISTINCT per-lane values; symmetric/all-ones vectors HIDE shuffle/mask defects. |
| **Outer-product `VOPMULA/VOPMSUB`** | Cross-product / plane-normal math wrong | Rotates the source pairing — NOT component-wise multiply. Local invariant: `mgPlaneNormal` component-wise `A*B−B*A ≡ 0`. |
| **`CFC2/CTC2` control indices** | Reads wrong control reg | Architectural macro indices: STATUS/MAC/CLIP = 16/17/18, Q = 22. Verify numeric instruction fields against the HW register table, not enum order. |

> **Where to look in the recompiler vs runtime:** VU0 macro/COP2 ops are emitted **inline** by
> the recompiler (flag updates in generated code) — usually NOT a runtime bug. Only the
> hand-written **VU1 interpreter** lacks flag/latency upkeep. Don't audit recompiled COP2 for a
> flag bug; audit `ps2_vu1.cpp`.

### §2.1 Authoritative PCSX2 values (the oracle — verified against `D:\ps2r\pcsx2-master\pcsx2`)

When you implement/audit any row above, match these EXACT rules from PCSX2 (cite them in the fix log):

**Q-register latency** (`VUops.cpp` `_vuRegsDIV/_vuRegsSQRT/_vuRegsRSQRT`, ~line 2326): the FDIV
pipe writes `REG_Q` after **DIV = 7, SQRT = 7, RSQRT = 13** cycles. So microcode doing
`DIV/RSQRT … (no WAITQ) … MULq/ADDq` within that distance reads the OLD Q. (EFU ops ESUM/ERSQRT/
EEXP use a SEPARATE pipe — don't lump them with Q.)

**`vuDouble` operand/result conditioning** (`VUops.cpp:440`) — applied to every FMAC operand+result:
- exponent field `== 0` (denormal/zero): return `f & 0x80000000` → **flush to signed zero** (UNCONDITIONAL).
- exponent field `== 0xFF` (inf/NaN): if `CHECK_VU_OVERFLOW` → `(sign) | 0x7f7fffff` (max normal); else passthrough.
- This is why a "missing float clamp" is usually tolerable until a specific operand goes denormal/inf.

**MAC flag** (`VUflags.cpp:15` `VU_MAC_UPDATE`, per lane `shift`: X=3 Y=2 Z=1 W=0; only DEST lanes):
- 16-bit layout `O[15:12] U[11:8] S[7:4] Z[3:0]`; per lane bits S=`0x10<<shift` Z=`0x01<<shift` U=`0x100<<shift` O=`0x1000<<shift`.
- Sign(S) = result sign bit, always set/cleared. Then by exponent: `f==0` → set Z, clear O+U;
  denormal(exp 0) → set U+Z, clear O (result flushed to signed 0); inf(exp 255) → set O, clear Z+U;
  normal → clear O+U+Z.
- **STATUS** (`VUflags.cpp:89` `VU_STAT_UPDATE`) = OR-reduce each MAC nibble: bit0=Z(`mac&0xF`),
  bit1=S(`&0xF0`), bit2=U(`&0xF00`), bit3=O(`&0xF000`), plus sticky bits. FTOI/ITOF/MOVE/MR32/ABS do **not** update MAC/STATUS.

**Outer product** (`VUops.cpp` VOPMULA ~843): `ACC.x = Fs.y*Ft.z, ACC.y = Fs.z*Ft.x, ACC.z = Fs.x*Ft.y`
(rotated pairing); VOPMSUB subtracts the rotated product and **leaves ACC unchanged**.

**COP2 control-reg macro indices** (`VU.h` / confirmed by the F51.8 audit): STATUS=16, MAC=17,
CLIP=18, R=20, I=21, Q=22, P=23, TPC=26, CMSAR0=27, FBRST=28, VPU_STAT=29, CMSAR1=31.

**Lower-opcode dispatch, flag/branch block** (`VUops.cpp` `_LOWER_OPCODE[128]`, index = bits
25–31 of the lower word) — transcribe into BOTH the interpreter and any disassembly tool:

| op | real VU | op | real VU |
|----|---------|----|---------|
| 0x10 | FCEQ | 0x14 | FSEQ |
| 0x11 | FCSET | 0x15 | FSSET |
| 0x12 | FCAND | 0x16 | FSAND |
| 0x13 | FCOR | 0x17 | FSOR |
| 0x18 | **FMEQ** | 0x1A | **FMAND** |
| 0x19 | (none) | 0x1B | **FMOR** |
| 0x1C | **FCGET** (`VIt = clip & 0xFFF`) | | |

(The 0x18/0x1A pair is the one a real port had swapped for its whole graphics-debug arc; the
mistake also parked FMOR on 0x1C, silently NOP-ing real FMORs and mis-executing real FCGETs.)

**Flag-consumer pipeline** (real-HW behavior; PCSX2 microVU models it via 4 "flag instances"):
MAC/STATUS values readable by FMEQ/FMAND/FMOR/FSxx at pair N are those produced by the FMAC at
pair **N−4**; pairs whose upper op is not an FMAC propagate the previous value. Game microcode
is hand-scheduled against this (guard-plane SUB exactly 4 pairs before its FMAND, unrelated
fog/pos FMACs interleaved in the gap). CLIP flags have analogous delayed visibility — audit if a
clip-gated branch misbehaves after MAC/STATUS are pipelined.

**A worked decode of a real per-vertex draw gate** (title-map transform packer — reusable
pattern, other games ship similar mask cascades): guard SUBs `upperBound−pos` / `pos−lowerBound`
on `.xyw` → S-flags `0xD0` = "inside in x,y,w"; `FMAND VIn, 0xD0|qw30` then FMAND-cascade ANDs
each vert's mask; `OPMSUB` cross product → S.z (`0x20`) = backface; `IOR` merges it;
`IBEQ VIexp, VIn` draws (skips the `+2048` ADC add) only when mask == expected. `qw30` is a
**winding-flip bit (0/0x20)** the setup program computes ONCE from the view-matrix determinant
sign (`FMAND VI, 0x80` on the determinant's S.x) — if all faces render inverted, check that
setup path before touching the per-vertex gate.

**A worked decode of a real VU CLIP route** (title-map program — the second reusable pattern;
found only after 20+ phases because the microcode dump was capped short, see §4.1): a
PRE-dispatcher ahead of the known prim dispatcher tests a selector bit meaning "EE said this
frame straddles the clip box" (set from an EE `mgClipInBoxW`-style test, NOT per-vertex) and
routes those batches to separate CLIP-transform packers. Each triangle gets `CLIPw` ×3 →
`FCAND 0x3ffff` (touches any plane?) → six `FCOR` masks (trivially reject when all 3 verts are
outside ONE plane, mask = `~(bit<<0|bit<<6|bit<<12)`) → a **Sutherland–Hodgman clipper**: 6
plane passes over a vertex ring, per-edge subroutine doing `CLIPw A; CLIPw B; FCGET; IAND
VIt,VI1,planeBitA; IBEQ…` (FCGET low 12 bits = the TWO most recent CLIPw results, A<<6|B; a SET
bit = vertex OUTSIDE), intersection via `DIV Q + WAITQ` + `MR32` axis rotation. The clipped
polygon is emitted as a **TRIFAN** from a template giftag the program keeps at a fixed VU
address (kicked twice: an empty `nloop=0|EOP` flush tag then the real `nloop=N` fan).
**Signatures:** a `.gs` dump with alternating empty/real trifan giftags = clipper output, not an
"object route"; "missing screen-edge geometry" (water strips, floor edges, bottom bands) with
tri/tstrip otherwise faithful = the clip route is dying, not a missing model or a separate
water/object path. Instrument the clipper entry, each plane-pass output count, and the fan
XGKICK — the stage whose count is 0 names the defect.

> The GS swizzle/CLUT addressing authority is captured in §3.1 below (PCSX2 `GSLocalMemory`). The
> full per-PSM block-swizzle *tables* themselves live in `pcsx2/GS/GSBlock.cpp` /
> `GSLocalMemory.cpp` — transcribe a specific table only if a concrete addressing bug needs it.

---

## §3 GS State Checklist — Texture / Colour / Render-Target

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

### §3.1 Authoritative GS memory geometry & swizzle (PCSX2 `GSLocalMemory.{cpp,h}`)

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

---

## §4 Methodology — How to Actually Diagnose a Graphics Bug

1. **Classify triangle coverage first.** A valid GS state histogram (FRAME/ALPHA/CLAMP/ZBUF/
   TEST/scissor/XYOFFSET/colour all expected) **cannot rescue bad guest geometry.** Bucket the
   emitted primitives (tri / tristrip / trifan counts, on-screen %, centroids) and compare to
   the reference. If coverage is wrong, the bug is upstream (VU/EE), not in GS state.
2. **Split geometry vs lighting/colour.** Probe VU-output XYZ separately from RGBA. Positions
   match HW but colour wrong → lighting consumption (§2 Q-latency, light-matrix channel order).
   Positions wrong → transform (§2 dest-mask, vf0, matrix).
3. **A/B against ground truth** (`12-pcsx2-mcp-playbook.md`): breakpoint the same draw in PCSX2,
   read the registers/memory, find the FIRST divergence. **Constraint:** the PCSX2 DebugServer
   maps EE RAM + scratchpad ONLY — it **cannot** read VU micro-mem (`0x1100xxxx`) or GS
   (`0x12000000`) (they read 0). For VU/GS internal state, use an **offline `.gs` GS dump**
   (capture from PCSX2, parse the packet stream + VRAM) as the reference instead.
4. **Probe with env-gated levers, never hard-edits** — see §5.
5. **Loop:** OBSERVE (which primitives/state) → LOCATE (VU? GS? VIF? EE upload?) → UNDERSTAND
   (what does HW emit here — from the `.gs` dump / PCSX2) → DECIDE (one fix tool) → VERIFY
   (re-capture, re-diff). Same loop as `13-decisional-brain.md`, with the *reference* being a
   captured frame, not stdout.

### §4.0 Packet-level GS-stream A/B (the highest-leverage graphics method — use it EARLY)

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
5. Static scene + multi-frame dump → dedupe strips by content hash; camera drift makes borderline
   guard verts flip category across frames — treat small off-diagonals in the join as drift noise
   before suspecting the fix.

### §4.1 Probe methodology (hard-won — a black texture is the canonical example)

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
  over the whole run) gives a clean yes/no.
- **Find the WORKING control through the same machinery.** If one texture renders (e.g. the title
  font) and another doesn't, the IMAGE/sample path is sound — the fault is isolated to what differs
  (the failing path's address/descriptor setup). Diff the working vs broken bind.
- **Confirm the test actually REACHED the state before concluding a feature is broken.** On a slow
  recompiled runner a 30 s headless window may only reach a few hundred frames; scripted input
  scheduled at a later frame simply never fires. Count the inject/marker — "0 injections" means the
  test never ran, not that the feature failed. (Sibling of the §13 "prove reachability" rule.)

---

## §5 Diagnostic-Lever Doctrine (applies to ALL fixes, critical for graphics)

Graphics fixes are easy to "prove" by eye and wrong under the hood. Protect yourself:

- **Gate every behavioural change behind an env flag.**
  - A fix that ships ON → add a **kill-switch** (`<PREFIX>_NO_<NAME>` disables it) so a
    regression can be bisected without a rebuild.
  - A change that only PROVES a diagnosis → make it **opt-in, default-OFF**
    (`<PREFIX>_<NAME>=1`). Never ship a proof-lever as the fix.
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
- **Clamp original-game bugs at the runtime boundary, never "fix" game-side.** Some defects are
  in the original game (e.g. an oversized DMA transfer the real DMAC tolerates). Absorb them in
  the runtime (stop when the destination rectangle is full), don't alter recompiled game logic.
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
For THIS project's exact capture harnesses + proven addresses, see `appendix-dc2-project.md`.
