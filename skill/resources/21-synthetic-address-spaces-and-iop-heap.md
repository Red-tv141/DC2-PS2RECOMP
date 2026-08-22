# Synthetic Address Spaces, IOP Heap Isolation & SIF Memory Safety

> **Core Law**: A synthetic "other processor" address space must NEVER be carved out of guest RAM. Any host-invented address range handed back to guest code (IOP heap, VU scratchpad, host callback tables) requires its own dedicated host backing store or mathematical proof that the guest allocator cannot touch it.

---

## 1. The Synthetic IOP Heap Aliasing Defect (Phase G625)

### 1.1 Architecture Background
On the physical PlayStation 2, the Emotion Engine (EE) has 32 MB of RDRAM (`0x00000000..0x01FFFFFF`), while the Input/Output Processor (IOP) has its own physically separate 2 MB RAM. The Sub-System Interface (SIF) DMA transfers data between EE and IOP address spaces.

In `ps2xRuntime` (`lib/Kernel/Stubs/Helpers/Support.h`), `sceSifAllocIopHeap` returned synthetic IOP addresses in the range:
```cpp
constexpr uint32_t kIopHeapBase  = 0x01A00000;
constexpr uint32_t kIopHeapLimit = 0x01F00000;
```

### 1.2 The Failure Mechanism
1. Dark Cloud 2's own EE memory allocator (`mgCMemory::Alloc`) dynamically allocates game structures, monster tables, and effect scripts across `0x01A00000..0x01FFFFFF`.
2. When the game loads sound banks or audio streams, it calls `sceSifSetDma` to transfer up to **246 KiB** (`0x3C4F0` bytes) from EE staging buffers to `dest=0x01A00000`.
3. The runtime executed `copyGuestByteRange(rdram, xfer.dest, ...)` directly into `rdram`, physically overwriting active game structures allocated at `0x01A00000+`.

### 1.3 The Multi-Subsystem Blast Radius
A memory corruption bug in a synthetic address space can manifest in completely unrelated subsystems:
- **Monster AI & Player Damage**: At guest frame 228 on `dungeon1`, sound-bank transfers clobbered monster effect-script base headers (specifically base 44 for poison spit at `0x1a192c0`). `CreateEffSpt` failed, returning `inst=0x0`. The script never ran `_COLPRIM_CREATE`, leaving 0 active collision primitives (`CColPrim`), making the player 100% immune to all enemy attacks.
- **Dungeon 6 Free-Roam Darkening**: Clut/texture structures trampled by SIF audio transfers caused screen darkening (investigated across G397–G400 before G625 identified the root cause).
- **Title Screen BGM**: Initialization data trampled during sound transfers silenced title music (G450).

---

## 2. Host-Side Shadow Buffer Implementation

To isolate synthetic IOP memory, allocate a dedicated host-side shadow buffer:

```cpp
// SIF.cpp
static std::vector<uint8_t> g_g625IopShadow;

static uint8_t* g625IopShadowPtr(uint32_t addr, uint32_t size) {
    if (addr >= kIopHeapBase && (addr + size) <= kIopHeapLimit) {
        if (g_g625IopShadow.empty()) {
            g_g625IopShadow.resize(kIopHeapLimit - kIopHeapBase, 0);
        }
        return &g_g625IopShadow[addr - kIopHeapBase];
    }
    return nullptr;
}
```

In `copyGuestByteRange`:
```cpp
uint8_t* dstPtr = g625IopShadowPtr(xfer.dest, xfer.size);
const uint8_t* srcPtr = g625IopShadowPtr(xfer.src, xfer.size);

if (!dstPtr) dstPtr = &rdram[xfer.dest & 0x01FFFFFF];
if (!srcPtr) srcPtr = &rdram[xfer.src & 0x01FFFFFF];

std::memcpy(dstPtr, srcPtr, xfer.size);
```

### 2.1 Audio SIF DMA Contract
Always verify which endpoint audio consumers read. In `ps2xRuntime`, audio consumers (`g385TraceAndDumpAudioDma`, `dc2G385CaptureIopDma`, `noteDtxSifDmaTransfer`) inspect **`xfer.src`** (the EE buffer) rather than the IOP destination, so shadow buffer isolation remains 100% coherent with existing audio pipelines.

---

## 3. Investigation & Diagnostic Laws

### 3.1 Direct JAL vs Indirect VM Census
- **Direct JAL Calls**: Recompiled directly into static C++ function calls. A `lookupFunction` census will report **0 calls** even when the function executes millions of times. **A zero in `lookupFunction` is WORTHLESS for a direct JAL target.**
- **Indirect VM Tables**: Effect scripts, monster scripts, and vtables dispatch through pointer arrays (`RS_STACKDATA`, function tables). A `lookupFunction` census on these ranges is **strong evidence** because they cannot be reached via direct JAL.

### 3.2 Preemption Suppression Around Wrapped `$v0` (G224's Law)
When probing or hooking a wrapped recompiled function, reading `$v0` without holding preemption suppression returns a mid-call or uncommitted value:
```cpp
// CORRECT: Hold preemption suppression when evaluating return value
g_dc2PreemptSuppressDepth++;
uint32_t ret = targetFunction(args);
g_dc2PreemptSuppressDepth--;
```

### 3.3 Structural Integrity Oracles
When debugging suspected data corruption:
1. Capture `(pointer, expected_id)` at creation/build time.
2. Re-read the memory locations once per frame.
3. Print the exact guest frame and replaced byte payload when integrity check fails.
This pinpoints corrupting memory writes in a single run without guessing game logic.
