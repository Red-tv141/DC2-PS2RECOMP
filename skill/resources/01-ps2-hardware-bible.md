# Reference: PS2 Hardware Bible
> Use this when you need knowledge of PS2 architecture, memory layout, EE registers, CP0, or binary formats.

## 1. Top-Level Overview
The PS2 architecture revolves around the Emotion Engine (EE) CPU and the Graphics Synthesizer (GS) GPU. They communicate directly via DMA and the VIF/GIF interfaces. The I/O Processor (IOP) handles legacy PS1 compatibility and peripheral (CD/DVD, USB, HDD) communication.

## 2. PS2 ELF Executable Format
- **Format**: Executable and Linking Format (ELF)
- **Extension**: Often stripped (`SLUS_XXX.XX`, `SLES_XXX.XX`) instead of `.elf`.
- **Key Sections**:
  - `.text`: Executable MIPS code. Often loaded at `0x00100000`.
  - `.data`: Initialized global data.
  - `.bss`: Uninitialized variable space (filled with 0 at runtime).
  - `.rodata`: Read-only constants, string literals.
- **Entry Point**: Look at the ELF header. Typically `0x00100008` (skips the crt0 `nop`).
- **Extraction**: Inside a PS2 ISO, the main ELF is listed in `SYSTEM.CNF` under `BOOT2`.

### The Multi-Binary Edge Case (.BIN files)
Not all games contain all MIPS code in a single ELF. Many large games (like Star Wars Episode III) use a tiny generic 'launcher' ELF containing only ~1 MB of code. The actual game engine is packed inside a massive `.BIN` file (e.g. `corec.bin`) containing raw, stripped MIPS bytecode which is loaded directly into RAM by the launcher.
**What to do:** When analyzing a game, if the main ELF yields very few functions, look for massive `.BIN` files in the ISO. These raw binaries must be converted into dummy `.ELF` files (giving them proper ELF headers and section mappings) so Ghidra and `ps2_analyzer` can process them independently!

## 3. Emotion Engine (EE) Architecture
- **Core**: Enhanced MIPS III/IV (R5900).
- **Registers**: 32 General Purpose Registers (GPRs).
  - **Crucially, GPRs on PS2 are 128-bit wide**. Lower 64 bits are standard MIPS; upper 64 bits are used by Multimedia Instructions (MMI).
  - Calling Convention uses `a0-a3` (and `t0-t3` for arg5-arg8) as arguments, `v0, v1` for returns.
- **Coprocessors**:
  - **COP0 (System Control)**: Exception handling, MMU/TLB, PRID.
  - **COP1 (FPU)**: Standard floating-point, plus specialized vector math.
  - **COP2 (VU0 Macro Mode)**: Vector Unit 0 closely tied to EE pipeline.

### EE Memory Map
| Address Range | Size | Description                                                              |
| ------------- | ---- | ------------------------------------------------------------------------ |
| `0x00000000`  | 32MB | Main RAM (RDRAM). Cached and accessible by EE. User memory mostly here.  |
| `0x10000000`  | 64KB | EE Hardware I/O Registers (Timers, INTC, DMAC, VIF, GIF, etc).           |
| `0x10008000`  | ...  | DMA Controller (DMAC) Registers (`0x10008000`–`0x1000E0FF`).             |
| `0x11000000`  | 16KB | VU0 Code Memory.                                                         |
| `0x11004000`  | 16KB | VU0 Data Memory.                                                         |
| `0x11008000`  | 16KB | VU1 Code Memory.                                                         |
| `0x1100C000`  | 16KB | VU1 Data Memory.                                                         |
| `0x12000000`  | 8KB  | GS Privileged Registers (Graphics Synthesizer mapping).                  |
| `0x1C000000`  | 2MB  | IOP RAM.                                                                 |
| `0x70000000`  | 16KB | Scratchpad RAM (fast uncached).                                          |
| `0xBFC00000`  | 512KB| Boot ROM (BIOS).                                                         |

*Note: Addresses above `0x20000000` mirror the lower 512MB space with different cache configurations (kseg0, kseg1).*

### The 10 DMA Channels (DMAC)
Games aggressively transfer data to avoid starving the EE. DMA channel registers span `0x10008000`–`0x1000D4FF`:
- `0x10008000` (CH0): VIF0 (To VU0)
- `0x10009000` (CH1): VIF1 (To VU1 / GS) -> *The main rendering pipeline*
- `0x1000A000` (CH2): GIF (To GS directly) -> *Textures and UI*
- `0x1000B000` (CH3): From IPU (MPEG Decoder)
- `0x1000B400` (CH4): To IPU
- `0x1000C000` (CH5): SIF0 (From IOP to EE)
- `0x1000C400` (CH6): SIF1 (From EE to IOP)
- `0x1000C800` (CH7): SIF2
- `0x1000D000` (CH8): From SPR (Scratchpad RAM)
- `0x1000D400` (CH9): To SPR (Scratchpad RAM)

## 4. Input/Output Processor (IOP) & SIF RPC
- **Core**: MIPS R3000A (PS1 CPU).
- **Role**: Handles CD/DVD drive, memory cards, controllers, sound (SPU2), USB.
- **Communication (SIF RPC)**: The EE talks to the IOP via the SIF (Sub-bus Interface). Games load "IRX modules" to the IOP to run specific drivers (e.g. `cdvdman.irx`, `mcman.irx`). 
  - *Fundamental Concept*: When the EE needs to save a file, it does not do it directly. It packs the filename and buffer into a struct, and sends an **RPC Command (Remote Procedure Call)** via SIF to the IOP, which executes the driver and sends the result back. This asynchronous bridge is why handling `sceCdRead` and memory card functions in PS2Recomp requires precise tracking of completion flags.

## 5. VU0, VU1, and the Graphics Synthesizer (GS)
- **VU0**: Generally used in "Macro Mode" directly extending the EE instruction set via COP2. Computes matrices, physics, skeleton animations.
- **VU1**: Generally used in "Micro Mode", acting autonomously. Connected directly to the GIF. The EE feeds it display lists via DMA (VIF1 channel); VU1 crunches the numbers and sends the vertex data to the GS.
- **GS Registers (`0x12000000` range)**: 
  - `PMODE` (`0x12000000`): Display modes (CRT/NTSC/PAL configuration).
  - `SMODE2` (`0x12000020`): Interlacing and sync modes.
  - `DISPFB1` (`0x12000070`): Display Frame Buffer settings (where in VRAM the visible screen starts).
  - `CSR` (`0x12001000`): System Status. *Critical:* Bit 3 of CSR is the V-BLANK sync flag. If a game spins in a while loop checking `0x12001000`, it's waiting for V-Sync.

## 6. Exceptions and Interrupts (COP0)
- **Cause Register**: Bits 2-6 hold the `ExcCode`.
  - `0x00`: Int (Interrupt)
  - `0x04`: AdEL (Address Error Load/Fetch)
  - `0x05`: AdES (Address Error Store)
  - `0x08`: Syscall (System Call)
- **Syscalls**: Executing the `SYSCALL` instruction jumps to the BIOS exception handler, which looks at mapping tables to invoke OS functions like `ExitThread`, `AddIntcHandler`, etc.
  - *In PS2Recomp*, these are trapped and routed to `ps2xRuntime`'s `ps2_syscalls.cpp`.

## 7. R5900 C++ Representation (`R5900Context`)
When working with overrides or debugging state, knowing the exact structure is vital.

```cpp
struct R5900Context {
    uint128_t gpr[32]; // 32 128-bit regs
    uint128_t lo, hi, lo1, hi1;
    uint32_t pc;
    // float fpu[32]; // Note: Implementation specific layout
    // uint32_t fcr31;
    // uint32_t fcr0;
    // ... COP0 registers ...
};
```
In runtime bindings, functions take a reference to this `ctx` to read `a0` (args) and write `v0` (return).

## 8. Subsystem Map — Runtime File Routing

When a crash involves PS2 hardware, use this table to find the responsible C++ file in `ps2xRuntime/src/lib/`:

| PS2 Subsystem | Address Range / Identifier | Runtime File(s) | Typical Symptoms |
|---------------|----------------------------|-----------------|------------------|
| **EE Core** (main CPU) | Recompiled code range | `ps2_runtime.cpp` + Runner code | Crashes in game logic |
| **GS** (Graphics) | `0x12000000-0x12001FFF` | `ps2_gs_gpu.cpp`, `ps2_gs_rasterizer.cpp` | Black screen, wrong rendering |
| **VU0/VU1** (Vector Units) | Inline in EE code | `ps2_vu1.cpp` + Runner (recompiled) | Wrong geometry, broken transforms |
| **VIF1** (VU Interface) | `0x10003C00-0x10003FFF` | `ps2_vif1_interpreter.cpp` | VU data not arriving, bad geometry |
| **GIF** (GS Interface) | `0x10003000-0x100037FF` | `ps2_gif_arbiter.cpp` | GS commands not reaching renderer |
| **SPU2** (Audio) | IOP side | `ps2_audio.cpp`, `ps2_audio_vag.cpp` | No sound, crashes on audio init |
| **IOP** (I/O Processor) | RPC calls, modules | `ps2_iop.cpp`, `ps2_iop_audio.cpp` | Hang during boot, module load fails |
| **Pad** (Controller) | `0x1F801xxx` | `ps2_pad.cpp` | No input, wrong buttons |
| **Syscalls** | `syscall` instruction | `ps2_syscalls.cpp` | Unimplemented syscall → crash |
| **Stubs** | Stubbed functions | `ps2_stubs.cpp` | Missing SDK function → log + return 0 |
| **Memory** | Kernel calls, TLB | `ps2_memory.cpp` | Segfault, invalid pointer |
| **Game Overrides** | Specific functions per-game | `game_overrides.cpp` | Recompiled function behaves wrong |
