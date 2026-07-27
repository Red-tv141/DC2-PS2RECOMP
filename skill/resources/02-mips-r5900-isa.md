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
FPU registers are `f0 - f31`. The PS2 FPU operates in Single Precision (`.S`) exclusively.
*   `cvt.w.s` (Convert Float to Int)
*   `cvt.s.w` (Convert Int to Float)

#### There is NO double precision on the EE — `double` is a SOFTWARE type in the integer GPRs

The R5900 FPU is single-precision **only**: no double registers, no double instructions. Compilers
(Metrowerks, SN) implement `double` in software using the 64-bit integer GPRs. You can recognise it
in any disassembly by the helper calls wrapped around every double expression — in DC2 they are
`fptodp` (float→double), `dpmul`, `dptofp` (double→float); other builds name them similarly
(`__adddf3`-style soft-float helpers).

**The calling convention for software doubles is integer-register, not FPU-register:**

| | where |
|---|---|
| arguments | `$a0`, `$a1`, … — each a full 64-bit IEEE-754 **bit pattern in one GPR** |
| return | `$v0`, same encoding |

Recognise a double-precision libm entry from its prologue: it manipulates `$a0` as *bits*, e.g.
`move v0,a0 ; dsra32 a1,v0,0 ; and a1,a1,0x7FFFFFFF ; slt v0,0x3FE921FB,a1` — extracting the high
word of `$a0` and comparing it against the high word of the double π/4. A `fabs` that just clears
bit 63 of `$a0` is the same tell.

**Why this matters more than it looks.** Writing such a stub against the C prototype instead of the
disassembly gives you `float arg = ctx->f[12]; ctx->f[0] = sinf(arg);` — which does not merely lose
precision, it **never writes `$v0` at all**. The caller then consumes whatever `$v0` happened to
hold, which is typically the output of the soft-float conversion a few instructions earlier: *the
argument itself instead of the function of it*. In DC2 (G373) that turned a thrown object's
`vel = (10·sin θ, 2.5, 10·cos θ)` into `(10θ, 2.5, 10θ)` — both components equal, so every throw
flew the same diagonal. **No crash, no NaN, no log line.** All eight double-precision libm entry
points in that ELF had it, so every double-precision computation in the game was silently wrong.

Rule: **for any hand-written stub, read the guest prologue and confirm which registers it actually
reads and writes.** A stub that reads the wrong register file fails silently and surfaces layers
away. When a math result is wrong but nothing crashes, check the ABI before you check the math.

#### The R5900 FPU is NOT IEEE-754 — emit saturating arithmetic, never host `+`/`*`/`/`

This is the single highest-value COP1 fact and it has already cost one full project a
multi-phase misdiagnosis (DC2 G369→G370→G371, "cutscene turns gray").

The R5900 has **no Infinity, no NaN and no denormals**, on COP1 *and* on both VUs:

| op | hardware behaviour |
|---|---|
| `add.s` / `sub.s` / `mul.s` | result saturates to `±0x7F7FFFFF` (`1.7014118e38`), sets O flag |
| `div.s` with zero denominator | returns `±0x7F7FFFFF`, sign = XOR of operand signs (**including `0/0`**), sets D flag |
| `sqrt.s` | computes `sqrt(|fs|)`; a negative operand only raises the I flag |
| `rsqrt.s` | `fd = fs / sqrt(|ft|)` — a DIVIDE, not the MIPS-IV `1/sqrt(fs)` reciprocal |
| denormal operand/result | flushed to zero |

Guest code is written against that contract: it keeps computing on saturated-but-**finite**
values, and its own `if (len != 0.0f)` / `<` guards keep doing their job.

Emit host IEEE arithmetic and you get a two-step failure that surfaces far from its cause:

1. An operand already at the saturated magnitude (produced legitimately elsewhere) **overflows
   to `±inf`** on the next multiply or add, where hardware would have re-clamped it.
2. `inf - inf` (or `inf * 0`) is **NaN** — and NaN is the dangerous half, because `x != 0.0f` is
   TRUE for NaN. The guest's own degenerate-input guard is silently bypassed instead of taking
   the fallback branch its author wrote, so a zero-length vector produces a poisoned matrix
   rather than the identity row the game expects.

**Rules that follow:**
- **Any NaN or Inf anywhere in guest state is the port's bug, never the game's.** Do not chase it
  as game logic, bad assets, or a renderer defect.
- **Never write a diagnostic that filters only for "non-finite".** The upstream value is usually
  still finite when it is already wrong. Dump the actual struct and compare a healthy frame
  against a bad frame **within one run**.
- **A partial-lane poison names the plane, and a surviving finite lane pins the state.** "NaN in
  X and Z, Y intact" says horizontal/look-at math; matching that surviving Y against the same
  address on PCSX2 proves both sides are in the same guest state, which separates an arithmetic
  divergence from a route/logic divergence.
- Reference implementation: `ps2_runtime_macros.h` `ps2_fpu_finish()` / `ps2_fpu_div()` /
  `ps2_fpu_sqrt()`, plus `code_generator.cpp` `COP1_S_DIV` (must emit the macro, not an inline
  `copysignf(INFINITY, ...)`).

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
- **Same non-IEEE float rules as COP1 above** (no Inf, no NaN, saturate to `±0x7F7FFFFF`).
  Translating these to bare `_mm_add_ps` / `_mm_mul_ps` / `_mm_div_ps` reintroduces exactly the
  Inf→NaN chain described in the COP1 section.
- **But check WHERE the clamp lives before adding one.** If the generator emits a MAC-flag block
  after each FMAC op, that block very likely already re-encodes the result (`exp == 0x7F800000` →
  `sign|0x7F7FFFFF`, denormal → signed zero). Hardware derives the O/U/S/Z flags from the **raw**
  value and *then* saturates, so a clamp added inside the arithmetic macro computes the overflow
  flag from an already-clamped value and silently breaks MAC flags. Read the emitted code, not just
  the macro definition.
- **The lower-pipeline Q ops bypass the FMAC path entirely** and are the ones that actually get
  missed: `VDIV` (zero denominator ⇒ `±0x7F7FFFFF`, **not 0** — returning 0 collapses a normalize to
  the zero vector), `VSQRT` (`sqrt(|ft|)`), `VRSQRT` (`Q = fs / sqrt(|ft|)` — a divide that uses
  `fs`, not the MIPS-IV `1/sqrt(fs)`).
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
