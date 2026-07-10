# Reference: PS2 Code & Hardware Patterns
> Use this when reading decompiled Ghidra output to recognize what a mysterious `sub_xxx` is actually doing based on memory addresses and loops.

If you don't know what a function does, look at the physical addresses it reads or writes. This is the biggest cheat code in PS2 reverse engineering.

## 1. The "Wait for DMA" Pattern
Games aggressively use the DMA Controller (DMAC) to send data from EE RAM to the GS or IOP.
If you see a loop waiting on a DMAC channel `CHCR` address, first identify the channel from the
base address. `CHCR` is at base+`0x00`; `MADR/QWC/TADR` are at base+`0x10/+0x20/+0x30`.

| Channel | CHCR base | Purpose |
|---------|-----------|---------|
| 0 | `0x10008000` | VIF0 (to VU0) |
| 1 | `0x10009000` | VIF1 (to VU1 / main graphics path) |
| 2 | `0x1000A000` | GIF (direct to GS; textures/UI) |
| 3 | `0x1000B000` | fromIPU |
| 4 | `0x1000B400` | toIPU |
| 5 | `0x1000C000` | SIF0 (IOP to EE) |
| 6 | `0x1000C400` | SIF1 (EE to IOP) |
| 7 | `0x1000C800` | SIF2 |
| 8 | `0x1000D000` | fromSPR |
| 9 | `0x1000D400` | toSPR |

**The Pattern:**
```cpp
// Start DMA transfer
*(uint32_t*)0x10009000 = ...; // VIF1 CHCR

// Spin until DMA CHCR bit 8 (START) clears (becomes 0)
while ( (*(uint32_t*)0x10009000) & 0x100 ) {
    // wait
}
```
If your recompilation hangs here, the runtime hasn't properly fired the DMA completion event.

**Source-chain TTE trap:** In source-chain mode, `CHCR` bit 6 is `TTE` (Tag Transfer Enable) and bit
7 is `TIE` (tag IRQ). When `TTE=1`, the DMAC also transfers the upper 64 bits of each DMAtag into
the channel stream. VIF1 chains commonly store two VIFcodes there (for example `NOP` + `MSCAL`). If a
VIF1 stream has valid tags but no expected `MSCAL`, inspect the tag high qword and `CHCR.TTE`
handling before blaming the VU program.

## 2. The GS (Graphics Synthesizer) Kickoff Pattern
Addresses in the `0x12000000` range mean the EE is talking directly to the GS.
- `0x12000000`: GS_PMODE
- `0x12000080`: GS_DISPFB1
- `0x12001000`: GS_CSR (System Status - extremely common in loops)

**The V-SYNC Loop Pattern:**
Games wait for the V-BLANK (vertical sync) to swap buffers.
```cpp
// Spinning on GS_CSR waiting for the V-Sync flag
while ( (*(uint32_t*)0x12001000 & 8) == 0 ) {}
*(uint32_t*)0x12001000 = 8; // Clear the flag
```

## 3. Library / Module Loading (`SifLoadModule`)
The PS2 OS is distributed. The IOP handles peripherals. Games load `.irx` modules from the CD to the IOP.
When you see string constants like:
- `"rom0:SIO2MAN"` (Serial I/O Manager - Gamepads/Memcards)
- `"rom0:MCMAN"` (Memory Card Manager)
- `"cdrom0:\MODULE\IOPRP.IRX"`

And these strings are passed into a function inside a loop... that function is `SifLoadModule` or `SifLoadModuleBuffer`. This function uses the `SIF` (System Interface) to send the file to the IOP and execute it.
*PS2Recomp Strategy:* You MUST stub this function to return success (`1` or `>0`). Do NOT let the native game logic try to execute IOP modules in the recompiler; it will fail.

## 4. CD/DVD I/O Patterns
If you see a function taking strings like `\\SLUS_xxx.xx;1` or `DATA.BND;1`.
That function is calling `sceCdSearchFile` or `sceCdRead`.
You will recognize CD logic because it often involves the `cdvdman` RPC commands.
Usually, a CD read involves:
1. `sceCdSearchFile(name, &toc_entry)` → Gets sector start
2. `sceCdRead(lsn, sectors, ptr, mode)` → Kicks off background read
3. `sceCdSync(0)` → Waits for read to finish.

If the game hangs after calling a `sub_xxx` with a filename, it's stuck in `sceCdSync`. You must implement a C++ stub that synchronously reads the file from the PC and returns success, bypassing the hang.

> Full CD/file-I/O implementation playbook (LSN→ISO offset mapping, path normalization, streaming
> API, async/callback semantics): `19-memcard-pads-fileio.md` §3.

## 5. The VU1 Display-List Microprogram Anatomy (recognize it before disassembling blind)

Retail engines ship ONE resident VU1 microprogram for world/map geometry with a fixed shape.
Knowing the shape turns a 2000-line VU disasm into a 20-minute read:

- **Setup program at pc `0x0`** — runs once per block (`MSCAL 0x0`): computes projection/guard
  constants from uploaded state qwords, and often ONE-TIME flags. Example: a **winding-flip
  bit** = sign of the view-matrix determinant (`OPMSUB` cross + dot + `FMAND mac,0x80`), stored
  to a low data-mem qword (e.g. `qw30`) that every packer later ORs into its expected cull mask.
  If ALL faces render inverted, audit THIS, not the per-vertex gate.
- **Trampoline at pc `0x10`** (`MSCAL 0x10` per batch) — jumps into a **dispatcher** that reads a
  per-batch selector qword (EE-authored) and branches to a **prim-class packer**: separate loops
  for tri / tristrip / trifan / "copy" (EE-pre-projected passthrough).
- **Packer loops** — per-vertex: load pos/uv/rgba (`LQI`), transform, perspective `DIV`+`MULq`,
  fog term `clamp(a + b·w)`, then write the GS vertex with `FTOI4`. **The XYZF2 word3 is
  dual-use: fog byte in bits 4–11, ADC (no-draw/strip-restart) in bit 15** — the transform
  packers add `+2048` to the fog float to set ADC (out-of-guard / backface / behind-plane);
  the copy packer writes fog only (its ADC is structurally 0). A per-vertex **draw gate** is
  typically an FMAND mask cascade over guard-plane `SUB` MAC flags + an `OPMSUB` winding bit,
  compared with `IBEQ` against the expected mask — scheduled exactly 4 instruction pairs after
  each producing FMAC (see `15-vu1-gs-debugging.md §2/§2.1` for the full decoded example and
  the interpreter hazards that silently break it).
- **Buffering:** packers write to double-buffered output at `VI` base pointers and `XGKICK` per
  batch; `ISWR`/`ILWR` around the kick save/restore loop registers to a tiny stack at `VI14`.

Recognition payoff: if a GS dump shows one prim class 100 %-culled (all ADC=1) or 100 %-drawn
while others look right, the defect is almost never the microcode (it's byte-identical to what
real HW runs) — it's the interpreter executing the gate wrong (opcode table, flag pipeline,
`vf0`, Q latency — the `15-...md §2` checklist, in that order).

## 6. Threading and Semaphores
Sony's SDK provides primitives for threading.
If you see a function taking strings like `"Main Thread"` or `"Load Thread"` and a priority number (usually around `64`), it's `CreateThread`.
If you see a structure initialized with an initial value of `1` and max `1`, it's `CreateSema` (Semaphore).
*PS2Recomp Strategy:* The `ps2xRuntime` hooks `SYSCALL` for `WaitSema` and `SignalSema`. If the game has wrapper functions around these syscalls (it usually does), you don't necessarily need to stub them, as the recompiler will successfully translate the `syscall` instruction inside the wrapper.
