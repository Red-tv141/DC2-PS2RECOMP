# Reference: VU1 Interpreter Correctness — When the GEOMETRY Is Wrong

> **Load this when the shape is wrong**: missing/exploded/streaking geometry, a model that renders
> at the wrong scale or position, lighting that is wrong per-vertex, culling that keeps or drops the
> wrong triangles, a flag-gated microprogram branching wrongly. These are **translation-class** bugs
> in the VU1 interpreter — the microcode is right and we execute it wrongly.
>
> **If the geometry is right but the PIXELS are wrong** (colour, texture, blend, depth, target,
> alias, presentation) it is a GS-state bug → `15b-gs-state-and-capture-ab.md`.
> That file also owns the packet-level `.gs` A/B method and the diagnostic-lever doctrine, which
> apply to both halves — read its §2 before building any probe.

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
| **Q-register pipeline latency** | A point-light/attenuation/perspective value is subtly wrong → e.g. neon-green lighting, slightly-off projection | HW latches `Q` **after a delay** (DIV/SQRT 7 cycles, RSQRT 13). Microcode doing `RSQRT/DIV … (no WAITQ) … MULq` at < latency distance wants the **OLD** pipelined Q. An immediate-write model feeds the fresh result. **Tell:** mixed `MULq|WAITQ` vs bare `MULq` in the same program. Stage Q into pending+delay, commit at 0; WAITQ publishes it at the lower-stall point. **AND model the FDIV busy-stall:** a second DIV/SQRT/RSQRT issued while a result is in flight must COMMIT the pending Q first (real HW stalls until the first latches — PCSX2 `_vuTestFDIVStalls`/`_vuFDIVflush`); a single-slot model that just overwrites pending silently DROPS the first result and leaves Q stale for the whole window. Symptom shape: geometry a real-HW `.gs` freeze marks 100% ADC=1 gets partially DRAWN with per-vertex coords matching HW within pixels (behind-camera q<0 verts passing a Q-derived cull/guard bound). Tell: back-to-back `DIV` pairs feeding `ADDq/MULq` ~7 pairs later. |
| **LOWER scalar stall applied after the paired upper** | A normalized lighting/transform basis suddenly reaches ~`FLT_MAX`; later colour clamps produce uniformly saturated strips even though the GS packer is faithful | Real VU1 tests the lower slot's FDIV/EFU wait or busy-pipe stall and flushes matured scalar results **before either slot executes**. Thus `MULq … | WAITQ` publishes pending Q before the upper MULq reads it; `WAITP` and a new producer issued into a busy FDIV/EFU pipe follow the same visibility ordering. An upper-then-lower interpreter consumes stale Q/P. **Fix with a pre-pair scalar stall/publication phase, then execute upper, then lower.** This is distinct from the same-pair VF hazard above: there the lower must see an old vector value; here the lower stall changes scalar visibility for the paired upper. Audit both whenever pair execution is hand-written. |
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

**Lower scalar-stall ordering** (`VU1microInterp.cpp` `_vu1Exec`, `VUops.cpp`
`_vuTestLowerStalls/_vuTestPipes`): before executing the instruction pair, PCSX2 decodes the
lower hazards, tests/advances the lower pipe, flushes completed results, then executes UPPER and
LOWER. A lower `WAITQ`, `WAITP`, or producer that encounters an already-busy FDIV/EFU pipe can
therefore publish the older Q/P **before the paired upper reads it**. Do not implement waits only
inside an upper-then-lower `execLower`; add a pre-pair stall/publication step.

**P-register / EFU latency** (`VUops.cpp` `_vuRegs*` table): P has its own pending result and
latency; `MFP` does not wait for a pending P, while `WAITP` and a subsequent busy EFU producer do.
For the commonly implemented subset, use **ESADD=11, ERSADD=18, ELENG=18, ERLENG=24, ESUM=12,
ERCPR=12, ESQRT=12, ERSQRT=18** cycles. PCSX2's `_vuTestEFUStalls` releases a producer stall one
cycle early but `_vuTestPipes` flushes the old P before the new result replaces it; preserve that
value visibility even in a simplified pair-level model.

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

> The GS swizzle/CLUT addressing authority is in `15b-gs-state-and-capture-ab.md` §1.1 (PCSX2 `GSLocalMemory`). The
> full per-PSM block-swizzle *tables* themselves live in `pcsx2/GS/GSBlock.cpp` /
> `GSLocalMemory.cpp` — transcribe a specific table only if a concrete addressing bug needs it.

---
