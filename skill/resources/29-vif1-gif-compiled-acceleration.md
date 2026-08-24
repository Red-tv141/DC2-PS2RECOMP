# VIF1 & GIF Compiled Acceleration and Invariant Hoisting

> **Reusable PS2 Recompilation Knowledge** — Hot loop compilation, single-form dispatch, and DMA unpacking acceleration.

---

## 1. The Core Principle: Hoist Invariants Outside the Loop

In high-throughput PS2 emulation and recompilation loops (such as VIF1 UNPACK or GIF packet decoding), executing tens of millions of iterations per second means every in-loop branch or redundant operation creates heavy performance penalties.

### 1.1 The In-Loop Branch & Rollback Penalty (G593 / G645)
- An in-loop ternary (`condition ? fast_path : slow_path`) or rollback branch forces the compiler to keep variables from both paths live across the loop body.
- This creates severe register pressure, causing spills to stack, defeating vectorization, and increasing loop-carried dependency chains (see `17d-hot-loop-and-codegen-laws.md`).
- **The Law**: *Select the execution arm ONCE outside the loop, or compile the loop as a single-form body.*

---

## 2. Compiled VIF1 UNPACK Acceleration (G650)

### 2.1 The Problem in Legacy Interpretation
A standard VIF1 UNPACK interpreter re-evaluates numerous properties per write-vector:
- Format decoding (`vl`, `vn`, component count).
- Destination load before store (to preserve masked lanes).
- Per-lane mask evaluation.
- Origin tracking and destination bounds checking.

### 2.2 The Six Admission Conjuncts
When all properties are invariant across an entire UNPACK command, the execution path can be selected once before entering the vector loop:
1. `!maskEnable` $\Rightarrow$ All `maskSpec` entries are 0 (no lane masking).
2. `(vif1_regs.mode & 3) == 0` $\Rightarrow$ Mode 0 (no row addition, no row write-back).
3. `cl == wl` $\Rightarrow$ `g645Blocked` holds, source and destination progress contiguously without skipping vectors.
4. `vuAddr + writeVectorCount <= 0x400` $\Rightarrow$ Destination lies completely within VU1 data memory (`0x400` 16-byte quadwords) with no modulo wrap.
5. Format is directly handled (`vl != 3 || vn == 3`) $\Rightarrow$ Format is supported without raw fallback.
6. `!g137Active` $\Rightarrow$ Diagnostic vector origin tracing is disabled.

### 2.3 Single-Form Direct Memory Copy
In the dominant PS2 3D geometry pipeline pattern (**V4-32, unmasked, mode 0, `cl == wl`**), the entire multi-vector UNPACK operation is mathematically equivalent to copying contiguous bytes directly from the DMA buffer into VU1 data memory:
```cpp
// Collapses multi-million iteration per-vector loop into a single optimized block copy:
std::memcpy(m_vu1Data + (vuAddr * 16), srcPtr, writeVectorCount * 16);
```

### 2.4 Snapshot-and-Replay Exactness Oracle
To guarantee bit-level exactness without maintaining duplicate arithmetic logic:
1. Snapshot the destination VU1 memory region.
2. Execute the accelerated/compiled kernel.
3. Save the accelerated output to a comparison buffer.
4. Restore the original destination snapshot.
5. Execute the reference legacy loop on the identical pre-state.
6. Compare byte-for-byte (`std::memcmp`). Count any discrepancy as `bad++`.

---

## 3. Compiled GIF PACKED Descriptor Inlining (G650)

### 3.1 Call Boundaries vs Large Switches
GIF PACKED packets contain sequences of 64-bit register descriptors. In standard runtimes, each descriptor invokes an out-of-line function (e.g., `GS::writeRegisterPacked`) containing a large switch statement:
- Out-of-line call overhead (~10 ns/call).
- Compilers refuse to inline large multi-case switch statements into complex packet loops.
- Re-evaluating the switch per descriptor even though vertex descriptor sequences are static within a tag.

### 3.2 Macro-Unified Vertex Dispatch
To inline vertex descriptors without duplicating arithmetic logic across codebases:
1. Define vertex attribute operations in shared macros (expanding identically in both the inline loop and the legacy fallback switch):
   - `RGBAQ` (0x01): Handles color, fog, and the zero-RGBAQ hold rule (`G52`).
   - `ST` (0x02): Handles texture coordinates $(S, T)$ with $Q$ scaling fallback (`m_curQ == 0.0f => 1.0f`).
   - `UV` (0x03): Handles integer texel coordinates $(U, V)$.
   - `XYZF2` (0x04) / `XYZ2` (0x05): Handles vertex coordinates, fog extraction from high bits (`hi >> 36`), synthetic ADC guards (`g105Restart`), and draw trigger dispatch.
   - `FOG` (0x0A), `NOP` (0x0F).
2. Non-vertex and stateful register descriptors (`A+D` 0x0E, `PRIM` 0x00, `XYZ3` 0x0C, `XYZF3` 0x0D) remain on the out-of-line dispatch to preserve register state machine complexity without bloating the hot loop.

---

## 4. Key Rules for Recompilation Engines

1. **Never Branch Inside Hot Unpack Loops**: If a feature is only used rarely (e.g., masking or mode 1/2), create a dedicated fallback loop branch at the command level rather than evaluating it inside the inner loop.
2. **Oracle is the Legacy Code**: Never write a third comparison reference when verifying optimized kernels; use the original known-good loop as the ground truth oracle.
3. **Verify Volume vs CPU Share**: When evaluating optimizations across different game routes, verify both the operation volume (e.g., vector counts) and the actual CPU share on the target worker thread. A high-volume operation on a thread with low utilization will not yield frame-time improvements.
