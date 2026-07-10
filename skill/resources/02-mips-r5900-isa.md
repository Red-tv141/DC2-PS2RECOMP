# Reference: MIPS R5900 ISA & C++ Translation
> Use this when converting MIPS assembly to C++ by hand, handling `Unhandled opcode` errors, or deciphering Ghidra decompilation.

## 1. MIPS Calling Convention (O32 variations for PS2)

### Registers
| Register | Name        | Usage                                    |
| -------- | ----------- | ---------------------------------------- |
| 0        | `$zero`     | Always 0                                 |
| 1        | `$at`       | Assembler temporary                      |
| 2-3      | `$v0, $v1`  | **Return Values**                        |
| 4-7      | `$a0 - $a3` | **Arguments 1-4**                        |
| 8-11     | `$t0 - $t3` | **Arguments 5-8** (PS2 extension to O32) |
| 12-15    | `$t4 - $t7` | Temporaries                              |
| 16-23    | `$s0 - $s7` | Saved/Callee-saved (must be preserved)   |
| 24-25    | `$t8, $t9`  | Temporaries                              |
| 28       | `$gp`       | Global pointer                           |
| 29       | `$sp`       | Stack pointer                            |
| 30       | `$fp`       | Frame pointer (rarely used, often `$s8`) |
| 31       | `$ra`       | Return address                           |

### Standard C++ Translation of Arguments
When writing a game override or raw runtime handler:
```cpp
void myFunction(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    uint32_t arg0 = getRegU32(ctx, 4); // a0
    uint32_t arg1 = getRegU32(ctx, 5); // a1
    uint32_t arg2 = getRegU32(ctx, 6); // a2

    // DO WORK

    setReturnU32(ctx, result);        // v0
    ctx->pc = getRegU32(ctx, 31);     // return via ra if this handler consumes the call
}
```

## 2. Dealing with "Unhandled Opcode"
Sometimes `ps2_recomp` cannot translate an instruction. You must manually implement its equivalent in the generated C++ file or a patch.

### COP0 (System Control)
Instructions like `MFC0` (Move From Coprocessor 0) and `MTC0` (Move To Coprocessor 0) manage exceptions, TLB, and status.
*C++ Equivalents:*
- **Status Register**: Often handles interrupts. In C++, might translate to checking a flag.
- **Cause Register**: Used to determine exception type.

### COP1 (FPU - Floating Point)
Standard MIPS floating point. FPU registers are `f0 - f31`.
PS2 FPU operates in Single Precision (`.S`) exclusively.
*   `cvt.w.s` (Convert Float to Int)
*   `cvt.s.w` (Convert Int to Float)

### MMI (Multimedia Instructions)
The R5900 extends standard 64-bit MMI with 128-bit operations.
These are critical for geometry and matrix math. If the recompiler fails on these, it's often due to 128-bit vector alignment issues.
*Common MMI opcodes:*
- `LQ / SQ` (Load/Store Quadword) - 128-bit memory operations! **Must be 16-byte aligned.**
  - C++ Note: `*(uint128_t*)(memory + address) = ctx.gpr[rx];`
- `PADDW`: Parallel Add Word (adds four 32-bit words simultaneously).
  - C++ Note: Can be simulated with `_mm_add_epi32` (SSE2).

### COP2 / VU0 Macro Instructions
When the EE issues instructions to VU0 in Macro Mode.
- `CTC2` / `CFC2`: Move control registers.
- `VADD`, `VMUL`, `VMADD`: Vector math.
*C++ Translation:* Extremely difficult to port manually line-by-line. Usually requires figuring out the high-level matrix/vector operation being performed and writing the equivalent C++ `glM` matrix multiplication or custom SIMD function.

## 3. Emulating Branch Delay Slots
MIPS has a **Branch Delay Slot**. The instruction immediately following a jump/branch is executed *before* the branch actually takes effect.
*Assembly:*
```assembly
jal     sub_123456
addiu   $a0, $zero, 1  // THIS HAPPENS BEFORE sub_123456 executes!
```
*C++ Translation:*
```cpp
ctx.gpr[4].words[0] = 1; // Handled FIRST
sub_123456(ctx);
```
Be hyper-aware of this when translating raw assembly. The PS2Recomp tool handles this automatically, but if you are patching raw ASM, you must remember it.

## 4. Ghidra Decompiler Artifacts
Ghidra's pseudocode will often look messy due to 128-bit registers and delay slots.
- **`afff()` / `qword`**: Usually indicates a 128-bit MMI register access or `lq`/`sq`.
- **`v0 = sub_123456(...)`**: Ghidra tries to infer arguments, but often gets it wrong if normal calling conventions aren't used. Always double-check actual `a0-a3` usage in the assembly view.
