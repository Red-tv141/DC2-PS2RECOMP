# Reference: Operational Workflow — Phase 0 to Phase 5
> Load this at session start for fresh projects, or when transitioning between phases. The phase names match the `## Current Phase` field in `PS2_PROJECT_STATE.md`.

## Phase 0 — Setup (`PHASE_SETUP`)

1. **Discover both workspaces.** You need two paths:
   - **PS2Recomp Repo** — contains `ps2xRecomp/`, `ps2xRuntime/`, `build64/`
   - **Game Workspace** — contains extracted ISO, ELF files, `.toml` configs, recompiler `output/`
   
   These may be the same directory or siblings. Check for a `PS2_PROJECT_STATE.md` first — if it exists, read both paths from it. Otherwise, inspect the current directory and ask the user.

2. **Verify vcvars64 environment.** Test: `where cl` must return a path (NOT "not found"). If not, you MUST source it or wrap commands.

3. **Detect build directory** (in the PS2Recomp Repo). Look for `build64/` or `build/`. If found, read `CMakeCache.txt` and extract:
   - `CMAKE_GENERATOR` (Ninja? Visual Studio?)
   - `CMAKE_CXX_COMPILER` (clang-cl? cl.exe/MSVC?)
   - `CMAKE_BUILD_TYPE` (Release? Debug? empty?)

4. **Check runner code exists (SAFELY).** Do NOT list the runner directory! Use:
   ```powershell
   Test-Path ps2xRuntime/src/runner  # True/False
   (Get-ChildItem ps2xRuntime/src/runner -Filter *.cpp).Count  # e.g. 32000
   ```

5. **Inspect the Game Workspace.** Search for:
   - `SYSTEM.CNF` — if found, read it to discover the main executable name (`BOOT2 = ...`)
   - `.toml` configs (the recompiler config — may be one per binary)
   - PS2 binaries — look for `SL[EU]S_*`, `SC[EU]S_*`, `*.elf`, and files >2MB that could be MIPS code (like `COREC.BIN`)
   - `output/` directory with generated `.cpp` files
   - Extracted ISO folder structure

6. **Report & suggest build config:**

   | Config | Rating | Agent Action |
   |--------|--------|--------------|
   | clang-cl + Ninja + Release | ⚡ Optimal | Use as-is. Do NOT reconfigure. |
   | MSVC + Ninja | ⚠️ Acceptable | Suggest clang-cl upgrade (see `03-ps2recomp-pipeline.md` §4) |
   | MSVC + VS Solution | ❌ Critical | **STRONGLY** recommend switching to Ninja+clang-cl. 25h→1h difference. |
   | No build dir at all | 🆕 Fresh | Guide user through initial cmake configure (see pipeline reference). |
   
   **NEVER reconfigure or delete the build directory without explicit user approval.** Only suggest; let the user decide.

7. **Verify build health.** A build directory can exist but be incomplete (e.g., obj files were deleted). Check:
   ```powershell
   # Does the final executable exist?
   Test-Path build64/ps2xRuntime/ps2EntryRunner.exe          # True/False
   # Are there compiled objects? (spot-check one .obj file)
   (Get-ChildItem build64 -Recurse -Filter *.obj | Select-Object -First 1) -ne $null
   ```
   Report build status separately from config:

   | Build Health | Meaning | Agent Action |
   |--------------|---------|--------------|
   | ✅ Complete | exe exists + obj files present | Ready to run |
   | ⚠️ Needs rebuild | Config OK but exe or obj missing | Tell user: `cmake --build build64` needed |
   | 🆕 Never built | Build dir exists but empty/no obj | Tell user: full build required (~1h with clang-cl) |
   
   **Do NOT auto-rebuild.** Report the status and ask the user how to proceed.

8. **Record both paths** in `PS2_PROJECT_STATE.md` under `## Workspace Paths`.

9. Verify `ps2_analyzer` and `ps2_recomp` executables exist. Build if missing.

10. Extract main ELF from ISO (if not already extracted). Concrete Windows methods, in preference order:
    ```powershell
    # A) 7-Zip (handles ISO9660/UDF; quote paths with spaces):
    7z l "D:\path\game.iso"                          # list — find SYSTEM.CNF + SL[EU]S_*
    7z x "D:\path\game.iso" -o"D:\gamews\iso" SYSTEM.CNF SLES_531.55   # extract just what you need
    # B) PowerShell native mount (no 7z needed):
    Mount-DiskImage -ImagePath "D:\path\game.iso"    # → appears as a drive letter
    Get-Volume | Where-Object DriveType -eq 'CD-ROM' # find the letter, then Copy-Item the files
    Dismount-DiskImage -ImagePath "D:\path\game.iso" # ALWAYS dismount when done
    ```
    Read `SYSTEM.CNF` first — `BOOT2 = cdrom0:\SLES_XXX.XX;1` names the main executable (strip the
    `cdrom0:\` prefix and `;1` suffix). While the image is open, also note large `.BIN`/`.IRX` files
    for the multi-binary check (Phase 1) — see `db-overlay-patterns.md`.

**Exit:** Both workspaces identified, toolchain verified, build health reported, ELF located.

---

## Phase 1 — ELF Analysis (`PHASE_ELF_ANALYSIS`)

1. Run `ps2_analyzer` on the **main** ELF (from `SYSTEM.CNF` BOOT2 path) → generates `[game].toml`.
2. If stripped, export Ghidra function map.
3. **Multi-binary check:** Ask the user if there are additional binaries to recompile (e.g., `COREC.BIN`). These are discovered when:
   - The main binary references code at addresses outside its own range
   - Runtime crashes point to "loaded" overlays or modules
   - The user already knows from prior experience
4. If secondary binaries exist, run `ps2_analyzer` on each → generates a separate TOML per binary.

**Exit:** All TOMLs exist with analyzer data. State file records each binary path.

---

## Phase 2 — TOML Configuration (`PHASE_TOML_CONFIG`)

1. Map known addresses to `stubs` in TOML. See `examples/toml-config-template.toml` for syntax.
2. Map init code to `skip`. Apply `patches` for privileged instructions.

**Exit:** TOML has overrides to bypass setup loops.

---

## Phase 3 — Recompilation (`PHASE_RECOMPILATION`)

1. Run `ps2_recomp` with the TOML.
2. Check output for `// Unhandled opcode` comments.

**Exit:** `out_*.cpp` files generated without fatal panics.

---

## Phase 4 — Build & Runtime (`PHASE_RUNTIME_BUILD`)

1. Move generated files to `ps2xRuntime/src/runner/`.

2. **⛔ Execute BUILD GATE** before building:
   - INSPECT command: no `--clean-first`, no `--target clean`, no delete
   - VERIFY vcvars64 environment is active
   - VERIFY build dir name from `PS2_PROJECT_STATE.md` (could be `build64/`, `build/`, etc.)
   - VERIFY generator from `CMakeCache.txt`:
     - Ninja/single-config: `cmake --build <build_dir>` is correct; build type was set at configure.
     - Visual Studio/multi-config: a bare build defaults to Debug `ALL_BUILD`; use
       `cmake --build <build_dir> --config Release --target <specific_target> -- /m:1`.
   - READ: full build output, verify exit code 0

3. If build fails: read error, fix C++ code, rebuild via BUILD GATE again. Do not ask user to compile.

4. **🎮 Run the game — STANDARD TEST PROTOCOL (use EVERY time, no variations):**

   **Step A — Read the command.** Get the exact run command from `PS2_PROJECT_STATE.md` § Active Runner Command. **NEVER reconstruct it from memory.** Memory lies.

   **Step B — Run with SHORT timeout.**
   - Boot tests: 10-15 seconds max (`WaitMsBeforeAsync=15000`)
   - Menu tests: 30 seconds max (`WaitMsBeforeAsync=30000`)
   - The game will crash or hang — short timeouts prevent wasting time waiting.

   **Step C — Read output IMMEDIATELY.**
   - Use `command_status` with `OutputCharacterCount=5000` (max 5000 chars per read)
   - If output is larger, read the LAST 5000 chars (most recent = most relevant)
   - **NEVER** read more than 10K chars total from a single test run

   **Step D — Kill the process.**
   - Use `send_command_input` with `Terminate=true`
   - Don't leave game instances running — they consume resources and confuse future tests

   **Step E — Diagnose from the output you just read.**
   - Don't re-run hoping for different results — same code = same crash
   - Fix first, THEN test again
   - If you can't diagnose: follow the Decision Flowchart in `10-agent-guardrails.md` §3 + check db files

   **Step F — Record.** One line in PS2_PROJECT_STATE.md: date, what you tested, exact result.

   **Step G — PCSX2 MCP Escalation (if available).** If Steps A-F don't explain the crash:
   - Connect to PCSX2 running the SAME game (`pcsx2_connect()`)
   - Set breakpoint at crash address, read registers/memory = ground truth
   - Compare with recompiled behavior to find FIRST divergence
   - Full protocol: `12-pcsx2-mcp-playbook.md` §3

   **Step H — Graphics/performance promotion gate.** For GS, VU, threading, or native-renderer
   changes, do all of the following before promotion:
   - Prove the harness reached the intended scene and mode using an injection/state marker plus a
     mode-specific output signature. Input timing and dump-frame numbering may use different clocks.
   - Run internal CPU/GPU or texel oracles when available, then separately inspect normal downstream
     composition over multiple frames. Local oracle success does not prove temporal presentation.
   - Compare candidate/control on the same binary with repeated interleaved steady windows. Split
     inclusive timers into body/wait/flush children before naming an optimization target.
   - A new presentation regression blocks promotion. If its repair needs a new ownership mechanism,
     leave the arm default-off, retain diagnostics, and record a focused follow-up instead of widening.

5. Write game overrides following `examples/game-override-template.cpp`.

6. Address crashes using the Decision Flowchart in `10-agent-guardrails.md` §3.

**Exit:** Executable builds and advances past bootloader.

---

## Phase 5 — I/O & Menus (`PHASE_IO_MODULE`)

1. Implement CDVD reads, File I/O, SIF module loading.
2. Replace triage stubs with real implementations.

**Exit:** Game reads files and sends GIF/DMA packets.

---

## Decision Tree — Crashes in `runner/*.cpp`

When hitting a crash, null pointer, or infinite loop:

1. **Extract:** Find exact PC and caller (RA) from the log.
2. **Classify subsystem:** CDVD? SIF/RPC? GS? EE Timer? IOP audio? Thread?
3. **Pick fix type:**
   - System/env failure (missing syscall, CD read) → implement in `src/lib/`
   - Game-specific hardware wait (DMA, RPC) → game override via TOML
   - Recompiler bug → game override
4. **Verify:** Check for regressions against previous milestones. Update state file.

**NEVER patch `runner/*.cpp`. Always fix via runtime or override.**

> **If you find yourself stuck in the crash→fix→crash loop**, STOP and load `13-decisional-brain.md`. It contains the reasoning framework that breaks the loop.
