# VIF1 Zero-Copy DMA Unpacking & Persistent Packet Streaming

> **GENERIC & PROJECT REUSABLE.** Architecture for eliminating dynamic memory allocations during VIF1 DMA chain parsing, implementing zero-copy vector unpacking, and reusing packet staging buffers across recurring game frames.

---

## 1. The Defect Class: Heap Churn & Vector Resizing in VIF1 DMA Ingestion

In PS2 recompilation architectures, VIF1 handles DMA transfers from the Emotion Engine to VU1 and the GIF. The classic defect is **per-packet dynamic vector allocation and reallocation**:

```cpp
// ANTI-PATTERN: Dynamic allocations in high-frequency DMA loop
void PS2Memory::processVIF1Data(const uint8_t *data, size_t size) {
    std::vector<uint8_t> unpackBuffer; // Heap alloc every transfer!
    unpackBuffer.insert(unpackBuffer.end(), ...); // Triggers _Insert_counted_range growth
    std::vector<GifArbiterPacket> packets; // Heap alloc
}
```

### The Measured Penalty
- On EE-poled routes (`s05`), `PS2Memory::processVIF1Data` consumes **15.4% of total EE CPU (~2.4 ms/f)**.
- Sample profiling (`[G446:eeprofcaller]`) reveals 15% of out-of-executable CPU time sits under MSVC STL vector growth functions (`vector<GifArbiterPacket>::end` 10.5%, `vector<uint8_t>::_Insert_counted_range` 4.66%).
- These continuous allocations and deallocations cause heap lock contention, TLB misses, and memory fragmentation.

---

## 2. Zero-Copy VIF Unpack Architecture

### Principles
1. **Persistent Ring Buffers**: Allocate thread-local, pre-sized staging arenas (e.g. 2 MiB) at runtime initialization. Never call `std::vector::resize()` or `operator new` inside `processVIF1Data`.
2. **Direct Memory Mapping**: For UNPACK commands (V3-32, V4-32, V4-16, etc.), unpack source DMA bytes directly into the target VU1 Micro Memory or GS staging ring without intermediate heap buffers.
3. **Persistent Chain Descriptors**: Game engines frequently re-emit identical DMA tag sequences for static models and UI elements. Cache parsed chain metadata to fast-path recurrent DMA chains.

```
┌─────────────────────────┐
│     EE RDRAM Source     │
│   (VIF1 DMA Tag Stream) │
└───────────┬─────────────┘
            │ Direct zero-copy unpack
            ▼
┌─────────────────────────────────────────────────────────┐
│      Persistent Pre-Allocated Ring Arena (2 MiB)        │
│  [ Header ] [ Transformed Vertex Data ] [ Tags ] ...    │
└───────────┬─────────────────────────────────────────────┘
            │ Streamed directly to VU1 / GS
            ▼
┌─────────────────────────┐
│  VU1 Memory / GS Queue  │
└─────────────────────────┘
```

---

## 3. Implementation Patterns

### Pattern A: Reserve-and-Reuse Arena
```cpp
struct Vif1Arena {
    static constexpr size_t kCapacity = 4 * 1024 * 1024;
    uint8_t buffer[kCapacity];
    size_t cursor = 0;

    void reset() { cursor = 0; }
    uint8_t* allocate(size_t bytes) {
        size_t aligned = (bytes + 15) & ~15;
        if (cursor + aligned > kCapacity) {
            cursor = 0; // Wrap around for ring buffer
        }
        uint8_t* ptr = buffer + cursor;
        cursor += aligned;
        return ptr;
    }
};
```

### Pattern B: Fast-Path SIMD Unpacker
Lower standard VIF UNPACK formats (e.g. `V4_16` signed half-words to `V4_32` floats) into specialized AVX2 / SSE4.1 vectorized kernels:
- `_mm256_cvtepi16_epi32` + `_mm256_cvtepi32_ps`
- Direct streaming stores (`_mm256_stream_ps`) into VU1 memory.

---

## 4. Verification & Profiling

1. **Heap Allocation Counter**: Ensure zero `RtlAllocateHeap` / `ZwFreeVirtualMemory` calls are triggered during steady-state VIF DMA processing.
2. **EE Thread Timer**: Gate payoff on `s05` static tail using `DC2_G503_EEPROF=1` to confirm `PS2Memory::processVIF1Data` CPU drops below 5%.
