# GPU Compute Pipelines, SSBO Data Paths & Shadow Chains

> **Reusable PS2 Recompilation Knowledge** — Exact GLSL compute shaders, SSBO memory management, in-shader span derivation, and dual-fence readback profiling.

---

## 1. High-Performance GPU Compute for GS Acceleration

Complex PS2 GS render passes (such as real-time shadow generation at address `0x139` or specialized multi-pass rasterization) can be offloaded from CPU rasterizers to dedicated OpenGL / Vulkan compute shaders.

---

## 2. SSBO Memory Management & The Grow-Only Law (G643)

Streaming dynamic per-frame vertex and primitive data into Shader Storage Buffer Objects (SSBOs) requires strict memory management:

### 2.1 The Allocation Stall Trap
- Calling `glBufferData(GL_SHADER_STORAGE_BUFFER, newSize, ...)` whenever the size fluctuates forces the GPU driver to destroy and reallocate backing GPU pages every frame.
- On Windows drivers, this introduces multi-millisecond CPU stalls and thread starvation across worker threads.

### 2.2 The Grow-Only High-Water Pattern
Always maintain an allocated capacity that only expands when a new maximum size is encountered:
```cpp
// Maintain high-water mark capacity:
if (requiredBytes > currentCapacity) {
    currentCapacity = (requiredBytes * 3) / 2; // 1.5x exponential growth
    glBufferData(GL_SHADER_STORAGE_BUFFER, currentCapacity, nullptr, GL_DYNAMIC_DRAW);
}
// Always upload data via sub-buffer streaming:
glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, requiredBytes, hostData);
```

---

## 3. In-Shader Affine Span Derivation (G642)

### 3.1 Eliminating CPU Span Tables
Traditional CPU software rasterizers generate extensive per-row span structures (e.g., 5+ MiB per batch) describing start/end X coordinates, depth, and barycentric steps for each horizontal scanline. Uploading these arrays across PCIe saturates host-GPU bandwidth.

### 3.2 Mathematical Derivation in Shader
Because edge equations $e_k(x, y) = e_{k0} + a_k \cdot x + b_k \cdot y$ are **affine functions** of $(x, y)$, their extrema across any rectangular tile occur strictly at the four corners.
- Rather than uploading scanline tables, upload only the 6–8 scalar coefficients per primitive (`rowStep[3]`, `edgeBase[3]`, stride).
- The compute shader computes the per-row span limits directly in parallel workgroups using arithmetic shifts and bitcasts.

---

## 4. Producer-Scoped Bounding Windowing (G641)

When a compute pass modifies a subset of the PS2 GS VRAM:
1. **Never read back the entire VRAM**: Bound the readback and upload rectangles to the active bounding box ($Y_{\min} \dots Y_{\max}$) of the primitives contained in the batch.
2. **Authority Integrity**: Narrowing the transfer range does not require complex multi-writer tracking or changing memory ownership; the kernel's bounding box proves outside pixels are unaltered.
3. Use `GL_UNPACK_ROW_LENGTH` and `GL_PACK_ROW_LENGTH` to transfer sub-rectangles directly without CPU-side buffer re-packing.

---

## 5. Dual-Fence Profiling Protocol (G641)

Synchronous readbacks (`glGetTexImage` or `glReadPixels`) often report massive wall-clock times that are mistakenly attributed to PCIe transfer bottlenecks.

To isolate the actual cost:
```
glFinish();               // 1. Drain all prior queued GL render work (Lap 1: backlog drain)
auto t0 = now();
glDispatchCompute(...);
glMemoryBarrier(...);
glFinish();               // 2. Wait for compute kernel execution (Lap 2: GPU compute)
auto t1 = now();
glReadPixels(...);        // 3. Perform the actual PCIe transfer (Lap 3: data transfer)
auto t2 = now();
```

- **Lap 1 (`pre`)**: GPU executing backlog draw calls previously submitted by the pipeline.
- **Lap 2 (`comp`)**: Real execution duration of the compute shader on hardware.
- **Lap 3 (`xfer`)**: True PCIe transfer duration.

Without a fence on both sides, Lap 1 is incorrectly billed to Lap 2 or Lap 3.
