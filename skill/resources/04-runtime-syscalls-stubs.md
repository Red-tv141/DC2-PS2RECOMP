# Reference: ps2xRuntime & C++ Implementation
> Use this when working in the `ps2xRuntime` directory, fixing syscalls, writing stubs, or writing Game Overrides.

The `ps2xRuntime` library is the environment executing the generated C++ code. It provides the memory model, CPU context, MMIO routing, and native SDK implementations.

## 1. The Core Loop
The execution of the game begins and remains in a very tight, highly optimized loop.
When `jal` instructions are encountered in MIPS, the generated code uses a table lookup to call the corresponding C++ function pointer. 

## 2. Memory Model
The PS2 has 32MB of main RAM starting at `0x00000000`.
In `ps2xRuntime`, memory is generally handled as a large flat `uint8_t` array.
*Crucial*: Because PS2 games assume physical memory maps, the runtime traps reads/writes to specific ranges and routes them. Let's look at MMIO routing:

### MMIO (Memory Mapped I/O)
When the game tries to read or write to addresses like `0x10000000` (IOP) or `0x12000000` (GS), normal memory access would segfault or return garbage.
The runtime handles these through explicit getters/setters in the `R5900Context` or macro-inlined memory accesses.

## 3. Syscalls (System Calls)
A `SYSCALL` instruction jumps to the BIOS exception handler. Sony provides hundreds of syscalls for threading, semaphores, interrupt handlers, and hardware initialization.
**File:** `ps2xRuntime/src/lib/ps2_syscalls.cpp`

If a game executes an unimplemented syscall, the runtime prints `[Syscall TODO]` and usually crashes. 
*Fixing it:*
1. Identify the Syscall ID from the log (e.g., `Syscall 0x02 executing`).
2. Search online (ps2dev documentation) or the hardware bible to see what Syscall `0x02` is (`GsPutDrawEnv`).
3. Add a case statement in `ps2_syscalls.cpp` handler switch.
4. Implement the logic, reading arguments from `ctx.gpr[4]` (a0), `ctx.gpr[5]` (a1), etc.

### Syscall Register Conventions & ABI Trap
- **The $v1 register is the canonical syscall-number carrier on the EE.** Negative syscall numbers (for interrupt-context helpers) are also passed in `$v1` as negative integers.
- **The $v0 register is the return value register.** Do NOT fall back to `$v0` to identify a syscall if `$v1` is invalid; `$v0` holds the return value of previous functions/syscalls. Falling back to `$v0` will coincidentally match stale return values to valid syscall numbers, causing silent mis-execution and masking missing syscalls.
- **Warning on Upstream Slices:** Some upstream PRs contain remapped syscall indices (e.g. remapping `0x5A` or `0x5B`). Always verify your game's boot requirements (e.g. `0x5A` = QueryBootMode, `0x5B` = GetThreadTLS) before accepting upstream remappings, as a wrong remap will break thread/TLS setup during boot.

## 4. Writing C++ Stubs
When you bind an address in `game.toml` to a `handler`, you must implement that handler in C++.

### The Triage Strategy
When reverse engineering stripped games, you'll encounter hundreds of `Warning: Unimplemented PS2 stub called`. 
Instead of writing real implementations immediately, we create "Triage Stubs":
```cpp
void ret0(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    setReturnU32(ctx, 0);
    ctx->pc = getRegU32(ctx, 31);
}
void ret1(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    setReturnU32(ctx, 1);
    ctx->pc = getRegU32(ctx, 31);
}
void reta0(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    setReturnU32(ctx, getRegU32(ctx, 4));
    ctx->pc = getRegU32(ctx, 31);
}
```
Try binding unknown functions to `ret0` or `ret1`. Does the game boot further? If so, you've bypassed a check. Figure out *what* check it was later using the static export (`14-static-analysis-navigation.md`) if present, with live Ghidra only as fallback.

### Writing Real Implementations (`sceCdRead` example)
When you know what a function does, emulate it natively. Example: intercepting a CD-ROM texture load.
```cpp
void my_sceCdRead(uint8_t* rdram, R5900Context* ctx, PS2Runtime* /*rt*/) {
    uint32_t lsn = getRegU32(ctx, 4); // a0: Logical Sector Number
    uint32_t sectors = getRegU32(ctx, 5); // a1: Number of sectors
    uint32_t buffer_ptr = getRegU32(ctx, 6); // a2: Destination address in EE RAM

    // Native C++ logic to read from PC file system instead of PS2 DVD...
    // MyFileSystem::Read(lsn, sectors, rdram + (buffer_ptr & 0x01FFFFFF));

    setReturnU32(ctx, 1); // Return 1 (success)
    ctx->pc = getRegU32(ctx, 31);
}
```

## 5. Game Overrides (`Game_Overrides.txt` concept)
You should keep game-specific hacks *out* of the core `ps2_syscalls.cpp` or generic SDK headers to avoid breaking other games.

Instead, create a C++ file for the specific game (e.g. `<game>_game_override.cpp` in `ps2xRuntime/src/` —
NOT inside the generated `src/runner/` dir; exact location varies per repo layout, record it in the
state file / project appendix). Register your overrides against the game's ELF metadata (basename,
entry, crc32).

**The API:**
```cpp
#include "game_overrides.h"
#include "ps2_runtime.h"

namespace {
    void applyMyGameOverrides(PS2Runtime &runtime) {
        // Direct bind to existing stub/handler
        ps2_game_overrides::bindAddressHandler(runtime, 0x00123456u, "sceCdRead");
        
        // Custom implementation wrapper
        runtime.registerFunction(0x001D9410u,
            [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt) {
                const uint32_t entryPc = ctx->pc;
                // do stuff
                ctx->gpr[2].words[0] = 0; // return 0
                
                // CRITICAL SAFETY FOR RAW WRAPPERS:
                if (ctx->pc == entryPc) {
                    ctx->pc = getRegU32(ctx, 31); // advance PC via ra
                }
            });
    }
}

PS2_REGISTER_GAME_OVERRIDE(
    "my-game-us",      // name
    "SLUS_XXXX.XX",    // elfName
    0x00100008u,       // entry point (0 avoids match)
    0u,                // crc32 (0 avoids match)
    applyMyGameOverrides
);
```
> **CRITICAL WARNING:** When using `bindAddressHandler(...)`, if the backend raw handler doesn't naturally advance `ctx->pc` (like many simple hooks), it will infinitely loop re-dispatching the exact same PC. If that happens, use `runtime.registerFunction` and advance the PC manually using `getRegU32(ctx, 31)` (the return address)!
>
> **CRITICAL RECOMPILER LIMITATION:** `runtime.registerFunction` and `bindAddressHandler` overrides ONLY fire when the recompiled code performs an *indirect* call (e.g. `jr $t9` or `jalr $t9`). Direct `jal <address>` calls are statically optimized by the recompiler into direct native C++ function calls (e.g., `sub_00109904(...)`). As a result, overriding `0x00109904` via `registerFunction` will NOT intercept any direct `jal sub_00109904` calls from other recompiled functions! If you need to intercept direct calls to a function, you must:
> 1. Mark that target function as a `stub` (not `skip` or `force_recompile`) in the `game.toml` configuration before running the recompiler. This forces the recompiler to generate a dynamic dispatch wrapper for it.
> 2. Or, write a C++ Game Override for the caller function itself.
> 3. Or, edit the recompiler configuration and regenerate the runner files.

## 6. Vectorization and SIMD Intrinsics
PS2 math relies heavily on 128-bit vectorization.
The runtime expects heavy use of SSE/AVX intrinsics (`_mm_add_epi32`, `_mm_mul_ps`) when manually replacing VU0/MMI geometry calculations. Do NOT write naive scalar loops for math-heavy stubs; it will destroy frame rates.

### 6.1 SDK VU0-helper stubs: match the MICROCODE semantics, not the C signature (DC2 G233)

When reimplementing a `sceVu0*` / libvu0-style helper as a runtime stub, derive the math from
the original VU0 macro-op body (disassembly / `ref/assembly.txt`), never from what the C
prototype "obviously means". The killer class is the **w lane**:

- `sceVu0Normalize(out, in)` is a **3-component** normalize: `ESADD P, vf` sums **x²+y²+z²
  only**, then `VMULq` scales all four lanes by `1/sqrt(P)`. A plausible 4-component
  reimplementation is silently wrong for any input whose `w != 0` — and inputs routinely carry
  `w=1` because they were produced by point subtraction (`P1 - P0` with both w=1... gives w=0,
  but `point - vec` or a matrix-transformed point gives w=1).
- Consequence example (DC2, missing chest gem, ~8 phases of triage): a capsule-collider
  push-out normalized a radial vector with leftover w=1; `len4 = sqrt(len3²+1) > 1`
  under-scaled the "unit" direction, so the push-OUT became a pull-IN — the physics chain
  converged INSIDE/behind the collider. The symptom surfaced three layers away as a Z-culled
  mesh.
- Audit heuristic: grep the original body for `ESADD`/`VOPMULA`/dot-3 idioms; any stub whose
  real body uses them must ignore w in the length/dot even though the memory operand is a vec4.
  Also mind that real `VMULq` scales the w lane too — write all four output lanes.
- A collision/constraint "push-out" that under-normalizes its direction becomes an
  **attractor** (pull-in). Signature in the data: free vertices stably offset toward the
  collider axis/backside, kinematically pinned vertices healthy, all struct constants
  byte-identical to the reference emulator.

#### 6.1b The stub's ABI is in the DISASSEMBLY, not in the C prototype (DC2 G373)

The worst stub bug is not wrong math — it is reading or writing the **wrong registers**, because
that fails completely silently.

DC2's double-precision libm stubs were written as `float arg = ctx->f[12]; ctx->f[0] = sinf(arg);`
— the obvious reading of `double sin(double)`. But the EE has no double precision: `double` is a
software type in the 64-bit integer GPRs, so the real convention is **argument in `$a0`, result in
`$v0`** (see `02-mips-r5900-isa.md`). The stub therefore never wrote `$v0`, and every caller
consumed whatever `$v0` still held — the output of the soft-float conversion immediately before it,
i.e. the *argument* rather than its sine. A thrown object's `(10·sin θ, 2.5, 10·cos θ)` became
`(10θ, 2.5, 10θ)`. All eight double libm entries had it; the whole game's double math was wrong.

Checklist for every hand-written stub:

1. Open the guest body and read the **prologue**. Which registers does it actually consume — `$a0..`
   or `$f12..`? Bit manipulation on `$a0` (`dsra32`, `and 0x7FFFFFFF`, clearing bit 63) means a
   software float/double in a GPR, never an FPU register.
2. Find the **return** path. Does the caller read `$v0` or `$f0`? A stub that writes only `f0`
   when the caller reads `$v0` returns a stale register, and nothing anywhere reports it.
3. Beware the half-fix: one of these stubs had already been "corrected" to read `$a0` — but as a
   32-bit *float* and returning through a sign-extending 32-bit helper. A partial ABI guess looks
   like due diligence and is still wrong.
4. Mechanical sweep for an existing port: every generated file that is only a `ps2_stubs::` forwarder
   is a stub whose ABI was chosen by hand. Diff each against its disassembly.

**When a computed value is wrong but nothing crashes and no NaN appears, suspect the ABI before the
math.**

#### 6.1a Audit the WHOLE family at once — the same bug is never in only one stub (DC2 G372)

G233 above ended with "AUDIT the other `sceVu0*` stubs". That audit sat undone for dozens of
phases; when finally run against the disassembly it found the *identical* w-lane bug in
`sceVu0InnerProduct` (a 4-lane dot where `vmul.xyz` + `vaddy.x` + `vaddz.x` is a **3**-lane dot).
Dot products are more load-bearing than normalize — trajectory, projection, reflection, angle
tests — and positions carry `w = 1.0`, so every position·position dot was off by a constant `+1`.
**When you fix one stub in a family, diff the entire family in the same session.** A one-line
follow-up note is not a fix.

Two more failure shapes the same pass found, both worth checking for directly:

- **Invented "missing argument" fallbacks.** `sceVu0ScaleVector` read the scale from `f12` and, if
  it was `0.0f`, fell back to reinterpreting `$a2`'s raw bits as a float and then as an int. The
  real body (`mfc1 t0,f12`) never reads `$a2`. `0.0` is a legitimate value — it is how you zero a
  velocity — so this corrupted every `v * 0`. If a stub contains a heuristic for "the caller
  probably meant something else", that heuristic is a bug: the ABI is in the disassembly.
- **`TODO`/unimplemented stubs that leave their OUTPUT untouched.** A warn-and-return-`-1` stub
  looks harmless in the log but hands the caller whatever was already in the destination buffer —
  a stale matrix, not an identity. Grep the TODO stubs for ones with real callers
  (`ref/functions/<addr>.md` → "Callers") and implement those; a stub with zero callers can stay.
