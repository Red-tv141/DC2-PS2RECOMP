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
