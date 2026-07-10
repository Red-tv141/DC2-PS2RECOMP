# Knowledge Base: Overlay & Multi-Binary Patterns
> Load this when a PS2 game loads code at runtime (overlays, modules, secondary ELFs), or when the main binary references addresses outside its own range.

## 1. What Are Overlays?

Many PS2 games don't run from a single monolithic ELF. Instead, they load additional code segments at runtime:

- **Code overlays** — portions of the game's own code loaded into fixed memory regions on demand (e.g., level-specific logic loaded over audio code when changing levels)
- **Secondary ELFs** — completely separate executables loaded via `LoadExecPS2()` (e.g., the game's "core engine" `COREC.BIN` vs. the boot ELF `SLUS_xxx.xx`)
- **IOP modules** — `.irx` files sent to the I/O processor via `SifLoadModule()` (audio drivers, pad drivers, CD drivers — these run on the IOP, not the EE)

## 2. Detection Patterns

### Signs Your Game Uses Overlays

| Evidence | What It Means |
|----------|---------------|
| Runtime crash at an address OUTSIDE the main ELF's `.text` range | The game jumped to overlay code that was never recompiled |
| `Function not found for address 0xXXXXXX` where the address is outside the main ELF | Missing overlay binary in the recompilation pipeline |
| Strings like `COREC.BIN`, `BATTLE.OVL`, `MENU.BIN` in the ISO | Likely secondary code binaries |
| `LoadExecPS2()` or `ExecPS2()` calls (by Ghidra name or pattern) | The game explicitly loads a new ELF |
| `sceCdRead` targeting an address range that overlaps existing code | Code overlay being loaded into a reusable memory region |
| Multiple `.text` sections, or a `.text` section that's suspiciously small | The ELF is a boot shim that loads the real code dynamically |

### How to Confirm via Static Export or Ghidra

1. If `ref/index/functions_index.json` exists, read its `text_range`/statistics (targeted `jq`, not a whole-file read) and compare the crash/call address to that range.
2. Search `ref/functions/` or `ref/index/xrefs_index.json` for strings/literals like `.elf`, `.bin`, `EXEC`, `LoadExecPS2`, or out-of-range call targets.
3. If no static export exists, use Ghidra: `mcp_ghydra_segments_list()` for `.text`, `mcp_ghydra_xrefs_list()` for out-of-range targets, and `mcp_ghydra_data_list_strings()` for loader strings.

## 3. Multi-TOML Configuration

Each binary needs its own TOML and its own `ps2_analyzer` + `ps2_recomp` run.

### Example: Boot ELF + Core Engine

```
Game/
├── SLUS_123.45          ← Boot ELF (small, sets up hardware, loads COREC)
├── COREC.BIN            ← Main game engine (large, contains all gameplay)
├── slus_12345.toml      ← TOML for boot ELF
└── corec.toml           ← TOML for core engine
```

**Pipeline:**
```bash
# 1. Analyze both
ps2_analyzer SLUS_123.45 -o slus_12345.toml
ps2_analyzer COREC.BIN -o corec.toml -base_address 0x00200000

# 2. Configure both TOMLs (stubs, patches, skips)

# 3. Recompile both
ps2_recomp slus_12345.toml -o output/boot/
ps2_recomp corec.toml -o output/core/

# 4. Copy ALL generated .cpp files to runner/
```

> **Critical:** The `-base_address` flag tells the recompiler where the binary is loaded in memory. For the main ELF this is usually `0x00100000`. For overlays, you must determine the load address from either:
> - The `LoadExecPS2()` call arguments
> - The memory address where `sceCdRead` deposits the overlay
> - The ELF header of the overlay binary itself

### Address Space Conflicts

If two binaries share the same address range (because overlays are loaded INTO the same memory):
- They CANNOT both be active at the same time
- The runtime must track which overlay is currently loaded
- Register functions from the currently-active overlay only
- This is advanced and may require runtime-level overlay management (not yet standardized in PS2Recomp)

## 4. IOP Modules (`.irx`) — Different Category

IOP modules run on the I/O Processor, NOT the Emotion Engine. They are **never recompiled** by `ps2_recomp`. Instead:

- `SifLoadModule("rom0:SIO2MAN")` → Stub to return success (module ID > 0)
- `SifLoadModule("cdrom0:\\MODULE\\CDVDMAN.IRX")` → Stub to return success
- The functionality these modules provide (pad input, CD reads, audio) must be reimplemented in the `ps2xRuntime` C++ layer

See `resources/04-runtime-syscalls-stubs.md` for stub patterns and `resources/07-ps2-code-patterns.md` §3 for the SifLoadModule pattern.

## 5. State File Integration

When working with multi-binary games, the `PS2_PROJECT_STATE.md` should track:

```markdown
## Binaries
| Binary | Type | Base Address | TOML | Status |
|--------|------|-------------|------|--------|
| SLUS_123.45 | Boot ELF | 0x00100000 | slus_12345.toml | ✅ Recompiled |
| COREC.BIN | Core Engine | 0x00200000 | corec.toml | 🔄 In progress |
| MODULE/SOUND.IRX | IOP Module | N/A (IOP) | N/A | Stubbed |
```
