# Reference: The Game Porting Playbook (Boot to Menu)
> Use this when actively trying to get a game to boot, diagnosing crashes, or escaping infinite loops.

If a game is stripped (no symbols), the path from "first compile" to "seeing the menu" is a predictable loop of fixing specific roadblocks.

**CRITICAL MENTAL MODEL: Address-Based Dispatch**
Runtime dispatch is purely address-based (`0xADDR` -> function pointer), NOT name-based (`sub_xxx`). Renaming generated C++ functions does not fix execution. If `0x00123456` should behave like `sceCdRead`, you MUST map that address to the handler in TOML or via a Game Override.

## The Iteration Loop
1. Run `ps2xRuntime` built with the game.
2. See where it crashes or hangs.
3. Diagnose the blocker (Missing function, Unimplemented stub, Syscall, Hang).
4. Update `game.toml` or C++ Overrides to bypass/fix it.
5. Rebuild and repeat.

> **THE RULE OF 3 (CIRCUIT BREAKER)**
> If you execute steps 1-5 three times on the EXACT SAME crash address and it still fails, you are using flawed logic. **STOP.** Do not guess a fourth time. Read the static export (`14-static-analysis-navigation.md`) if present, otherwise open Ghidra; then read `ps2tek` / the relevant `db-*` file or ask the user. You must break the loop.

---

## Standard Blockers & Solutions

### Blocker 1: "Function Not Found" at Boot
*Symptom:* The game immediately aborts with `Function not found for address 0xXXXXXX`.
*Cause:* The analyzer missed a valid execution path.
*Solution A:* If it's a known OS function address (check online documentation), map it as a stub in `game.toml`.
*Solution B:* If a static export exists, check whether the target address is present/stale using `14-static-analysis-navigation.md`. If no export exists or it lacks the code, export the function map from Ghidra and re-run `ps2_analyzer`.

### Blocker 2: "Unimplemented PS2 stub called: sub_XXXXXX"
*Symptom:* Execution hits an unknown function that you've told the TOML to `stub`, but you didn't write a C++ handler.
*The Dynamic Probing Solution (Empirical Telemetry):*
Do NOT just blindly return 0 for everything; returning 0 on a function that allocates memory will create Zombie Threads and unresolved BSS-TRAPS later.
Instead of guessing via static assembly, **instrument the function** to dump its arguments to the console and return zero *only* temporarily, or proxy to an implementation if possible:
```cpp
void probe_sub_XXXXXX(uint8_t* rdram, R5900Context* ctx, PS2Runtime* /*rt*/) {
    uint32_t a0 = getRegU32(ctx, 4);
    uint32_t a1 = getRegU32(ctx, 5);
    printf("[PROBE] sub_XXXXXX called. a0: 0x%08X, a1: 0x%08X\n", a0, a1);
    
    // If a0 looks like a pointer (e.g. 0x00A00000 range), dump the memory it points to!
    if (a0 > 0x00100000 && a0 < 0x02000000) {
        printf("  DUMP: [0x%08X] = 0x%08X\n", a0, *reinterpret_cast<uint32_t*>(rdram + (a0 & 0x01FFFFFF)));
    }
    
    setReturnU32(ctx, 0);        // Temporary bypass
    ctx->pc = getRegU32(ctx, 31);
}
```
Watch the console output. If the values predictably increment or look like known enums, you can deduce the struct shape empirically.

### Blocker 3: The Infinite Loop (Spinlock/Deadlock)
*Symptom:* The game freezes. The log stops printing, but the CPU core goes to 100%. If you pause in a debugger, it's stuck in a loop inside a `sub_XXXXXX`.
*Cause:* The game is waiting for a hardware event (like V-Sync, CD/DVD interrupt, or DMA completion) that we haven't properly emulated.
*Diagnosis (static export first, Ghidra fallback):*
1. Get the address where the PC is stuck (from your C++ debugger).
2. Read the static function file (`ref/functions/0xADDR_*.md`) if present; otherwise use Ghidra decompilation. You will see something like:
   `while ( *(uint32_t*)0x12000000 == 0 ) { ... }`
3. Identify the hardware register. If it's `0x10000000` range, it's IOP/CD. If `0x12000000`, it's GS.
*The Triage Solution:*
Add a patch in TOML to replace the branch instruction that forms the loop with a `nop` (0x00000000). Or write a C++ game override that forcefully advances the state the game is waiting for.

### Blocker 4: Missing Syscalls (`[Syscall TODO]`)
*Symptom:* `ps2xRuntime` prints `[Syscall TODO: 0x?? executing]`.
*Cause:* The game needs threading, sync, or HW access provided by the BIOS.
*Solution:* Implement it in `ps2_syscalls.cpp`.

---

## The Path to the Main Menu (Milestones)

1. **Boot and Memory Init:** Early code initializes stacks, zeros BSS, sets up DMA channels.
2. **Library Loading (`SifLoadModule`):** The EE loads drivers into the IOP. You will see calls to `sub_xxx` passing string arguments like `rom0:SIO2MAN` or `cdrom0:\MODULE\CDVDMAN.IRX`.
   *Action:* Identify the `SifLoadModule` function and stub it perfectly. It must return success (`1`).
3. **CDVD Reads (`sceCdRead`, `sceCdSync`):** The game tries to read files from the disc.
   *Action:* Emulate the CD-ROM reader in C++ reading from the PC filesystem.
4. **GS Initialization:** The game kicks off GIF tags and configures the GS for drawing.
   *Action:* The runtime should handle standard GS routing, but verify no custom setup loops are hanging.
5. **The First Frame:** If the game doesn't crash after setting up display memory and reading an asset file, you will likely see a splash screen or intro video!

## Final Checklist Before Asking "Why Is It Stuck?"
If you are stuck in a game loop, verify:
1. `registerAllFunctions(runtime)` is non-empty and actually called before `loadELF`.
2. The Entry point function is registered.
3. The first unresolved function addresses are mapped or implemented.
4. The first unknown syscalls are handled.
5. There is no obvious same-PC infinite loop caused by direct raw handler binding in Game Overrides.
6. Temporary return stubs (`ret0`/`ret1`) are limited and documented (if you left too many, the game logic will eventually fail randomly).
If all 6 are true, you are in real behavior emulation territory, not plumbing failure.
