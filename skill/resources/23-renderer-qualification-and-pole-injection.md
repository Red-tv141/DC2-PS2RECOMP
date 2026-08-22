# Renderer Qualification, Pole Models & Derivative Injection Probes

> **Core Law**: Census ms levels alone do not prove a thread will convert optimizations into frame time. A sole pole and a level pole look identical in census tables. ONLY a derivative injection probe (`DC2_G431_GS_SLOW_US`, `DC2_G503_EE_SLOW_US`) measures true conversion rate ($\Delta \text{frame} / \Delta \text{injected}$).

---

## 1. The Three-Thread Pole Model (G618/G619/G624/G626)

On modern multi-core hosts, the PS2 recompiled runtime executes concurrently across three major threads:
$$\text{Frame Time} \approx \max(\text{VU1 busy}, \text{GS own}, \text{EE cpu})$$

Where:
- $\text{GS own} = \text{gsWorkerMs/f} - \text{gsStallMs/f}$
- $\text{EE cpu} = \text{cpuMs/f}$ from `[G182:ee]` (**not** `busyMs/f`, which includes synchronization waits)
- $\text{VU1 busy} = \text{worker busy duration}$

---

## 2. Sole Pole vs. Level Pole

### 2.1 Sole Pole (`ridepod` qualification in G626)
- **Characteristic**: One thread is significantly higher than all others, with substantial headroom.
- **GS Worker Own**: 21.14 ms/f
- **VU1 Busy**: 17.72 ms/f (+3.42 ms/f gap)
- **EE CPU**: 14.26 ms/f (60% onCPU)
- **Derivative Probe**: Injected +2.41 ms/f via `DC2_G431_GS_SLOW_US=1` $\rightarrow$ Frame moved +2.53 ms/f.
- **Injection Sensitivity**:
  $$\text{Sensitivity} = \frac{\Delta \text{Frame}}{\Delta \text{Injected}} = \frac{+2.53}{+2.41} = \mathbf{1.05}$$
- **Verdict**: A GS optimization converts **~1:1 into frame time** over multiple milliseconds.

### 2.2 Level Pole (`dungeon1` closure in G624)
- **Characteristic**: Two or three threads sit at approximately the same execution time.
- **GS Worker Own**: 21.5–21.8 ms/f
- **EE CPU**: 19.8–21.9 ms/f
- **Derivative Probe**:
  - GS Injection (`DC2_G431_GS_SLOW_US=1`): +3.1 ms injected $\rightarrow$ +2.14 frame ($\text{Sensitivity} = 0.69$).
  - EE Injection (`DC2_G503_EE_SLOW_US=3000`): +2.09 ms injected $\rightarrow$ +0.29 frame ($\text{Sensitivity} = 0.14$).
- **Verdict**: **CLOSED to single-thread levers.** A GS cut converts only until GS meets EE (~0.3 ms/f), then reads null. Further gains require simultaneous, balanced reductions on both threads.

---

## 3. Methodological Rules

### 3.1 Script Clock vs Host Present Clock
- `[G154:perf] n=` is a **host-present counter**. A faster binary presents more frames in the same real-world time, shifting scene instants.
- Always key windows on **script clock** (`scriptFrame`, `[G154:perf] frame=`) and denominate on rendered frames.

### 3.2 Headroom Staleness Law
- Headroom figures expire rapidly upon phase promotions.
- On `dungeon1`, GS headroom was +3.7 ms/f post-G622, but collapsed to +0.3 ms/f after G623/G624 promoted.
- Re-derive the pole and injection sensitivities whenever a major lever is promoted.
