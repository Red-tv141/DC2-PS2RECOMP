# Paletted Texture Decoding & Address Derivation Hoisting

> **Reusable PS2 Recompilation Knowledge** — Paletted texture decoding (PSMT8, PSMT4, PSMT4HL, PSMT4HH), address calculation hoisting, and CLUT lookup optimization.

---

## 1. The Paletted Texture Decoding Bottleneck

On the PlayStation 2 GS, paletted textures index into a Color Look-Up Table (CLUT) located in GS memory. In recompiled software rasterizers or texture upload paths, texture decoders convert raw VRAM pages into standard RGBA32 format.

### 1.1 The Address Derivation Bottleneck
A standard decoder invoking `PixelStorageTraits<psm>::Read(x, y)` on every texel pays heavy repeated arithmetic:
- `PageId(block, bw, x, y)` (involving multiple divides and multiplies).
- `block % 32`, `y % pageH`, `x % pageW`.
- Page offset computation: `page * PixelsPerPage()`.

All of these terms are **loop-invariant** once a texture row is sliced at physical page boundaries.

---

## 2. Why AVX2 Gather is the Wrong Lever (G650)

It is tempting to attempt vectorizing CLUT lookups via AVX2 gather intrinsics (`_mm256_i32gather_epi32`). However:
1. The 256-entry CLUT is pre-expanded once per decode into an `alignas(64)` 1 KiB table that resides permanently in L1 CPU cache.
2. An L1 scalar load (`clut[index]`) executes in 4–5 cycles with high throughput. Hardware gather instructions carry high setup latency and port contention.
3. The true CPU bottleneck is **not** the CLUT array lookup, but the **per-texel address calculation in PS2 VRAM page memory**.
4. **The Law**: *Hoist the address derivation out of the loop; do not attempt to SIMD-gather an L1-resident table.*

---

## 3. Specialized Hoisted Row Readers

By slicing the row at GS page boundaries, the base page pointer and row table offset are derived once. The inner loop specializes per format:

| Format | Format Code | Byte Address (`pa`) | Texel Index Extraction |
|---|---|---|---|
| **PSMT8** | 0x13 | `pa` | `data[pa]` |
| **PSMT4** | 0x14 | `pa >> 1` | `(data[pa >> 1] >> ((pa & 1) * 4)) & 0x0F` |
| **PSMT4HL** | 0x24 | `pa * 4 + 3` | `data[pa * 4 + 3] & 0x0F` |
| **PSMT4HH** | 0x2C | `pa * 4 + 3` | `(data[pa * 4 + 3] >> 4) & 0x0F` |

---

## 4. Live Page Table Verification vs Assumed Linearity

### 4.1 Swizzle Nonlinearity
While 32-bit textures (PSMCT32) exhibit linear column runs allowing 8-wide `memcpy` operations, paletted formats (PSMT8/PSMT4) utilize complex 2D column swizzles where texel memory addresses jump nonlinearly across X within a page.

### 4.2 Safe Verification Protocol
Never assume a page table row is contiguous:
```cpp
// Verify contiguous layout against the live page table before taking a block copy path:
bool isContiguous = true;
for (int k = 0; k < 8; ++k) {
    if (pageTableRow[group + k] != pageTableRow[group] + k) {
        isContiguous = false;
        break;
    }
}
if (isContiguous) {
    // Fast contiguous block path
} else {
    // Fall back to scalar hoisted address path
}
```

---

## 5. Verification Protocol

- **Oracle Comparison**: Re-read every produced texel using the baseline `PixelStorageTraits<psm>::Read` and verify zero mismatches (`bad == 0`).
- **Aggregate Profiling**: When decoding is parallelized across worker threads (e.g. `GSRowPool`), aggregate CPU time saved is proportional to thread count $\times$ GS own reduction.
