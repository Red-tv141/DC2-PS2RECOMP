# Core Scheduling, Thread Affinity & Topology Optimization

> **Reusable PS2 Recompilation Knowledge** — Multi-threaded runtime scheduling, physical core mapping, Win32 topology enumeration, and SMT contention management.

---

## 1. Multi-Threaded PS2 Runtime Thread Model

A high-performance PS2 static recompilation runtime consists of several concurrent threads with distinct latency and throughput requirements:
1. **EE Thread (`GameThread`)**: Executes recompiled guest game logic and MIPS instructions.
2. **VU1 Worker Thread (MTVU)**: Executes vector unit microprograms.
3. **GS Worker Thread**: Executes GIF packet processing, state machine, and vertex submission.
4. **GL / GPU Backend Worker**: Executes driver OpenGL / Vulkan draw calls and context state changes.
5. **Worker Pool (`GSRowPool`)**: Multi-threaded cooperative lanes handling CPU rasterization and parallel task workloads.
6. **Main / Present Thread**: Handles window events, pad input, and frame presentation.
7. **IRQ Worker Thread**: Drives emulated hardware interrupts and vblank timing.

---

## 2. Windows CPU Topology & Contention Hazards

On modern multi-core processors with Simultaneous Multithreading (SMT / Hyper-Threading) or Hybrid Architectures (P-cores / E-cores):
- If the OS scheduler places two heavy worker threads (e.g. EE and GS) on the **two SMT siblings of a single physical core**, both threads compete for identical execution ports and L1/L2 caches, causing up to a 30% performance collapse.
- If worker threads are placed on low-power Efficiency cores (E-cores), frame timing becomes unstable.

---

## 3. Win32 Processor Topology Enumeration

To accurately identify physical cores and their logical SMT masks on Windows:

```cpp
// Use GetLogicalProcessorInformationEx with RelationProcessorCore:
DWORD len = 0;
GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
std::vector<uint8_t> buffer(len);
auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len);

uint8_t* ptr = buffer.data();
uint8_t* end = buffer.data() + len;

while (ptr < end) {
    auto* item = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(ptr);
    if (item->Relationship == RelationProcessorCore) {
        KAFFINITY coreMask = item->Processor.GroupMask[0].Mask;
        // Each coreMask contains all logical processor bits (SMT siblings) for ONE physical core.
        physicalCores.push_back(coreMask);
    }
    // CRITICAL: Advance by item->Size, NOT sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)!
    // SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX is a union whose max size exceeds RelationProcessorCore.
    ptr += item->Size;
}
```

> [!WARNING]
> Advancing by `sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)` will prematurely truncate the buffer and drop the last physical core on multi-core systems!

---

## 4. The Three Scheduling Slices

To optimize thread placement safely without regressing on diverse host hardware:

### 4.1 Slice 1: Soft Hints (`SetThreadIdealProcessor`)
- Sets the preferred logical processor for each hot thread.
- Non-exclusive: The Windows scheduler can migrate threads if needed.
- Completely safe on all architectures; does not restrict pool concurrency.

### 4.2 Slice 2: Hard Affinity Masks (`SetThreadAffinityMask`)
- Pins each hot worker thread to the mask of a single **physical core** (covering both its SMT siblings).
- Dedicated core mapping:
  - Core 0: Reserved for OS, display driver threads, and Main/Present thread.
  - Core 1: EE GameThread.
  - Core 2: VU1 Worker.
  - Core 3: GS Worker.
  - Core 4: GL / GPU Backend Worker.
  - Core 5+: Left completely unpinned for `GSRowPool` worker lanes.
- Prevents cross-thread SMT port contention on the primary pipeline.

### 4.3 Slice 3: Priority Elevating (`SetThreadPriority`)
- Elevate hot worker threads to `THREAD_PRIORITY_ABOVE_NORMAL`.
- Avoid `TIME_CRITICAL` or `REALTIME` — starving the GL backend worker or host display driver induces massive presentation stalls.

---

## 5. Host Re-Gating Rules

- **Host-Shaped Performance**: Core affinity gains are dependent on host CPU core counts.
- **Rule for Unknown Hosts**: Provide a clean rollback toggle (`DC2_G650_NO_SCHED=1`) so users on 2-core or 4-core systems can restore default OS scheduling if needed.
