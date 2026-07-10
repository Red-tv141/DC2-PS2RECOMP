<!-- ⛔ RULES — Re-read this section EVERY TIME you open this file. -->
<!-- These rules also appear in the SKILL.md. Redundancy is intentional. -->

# ⛔ QUICK RULES (mandatory re-read)

1. **Build:** `cmake --build <build_dir>` — NEVER add `--clean-first`, `--target clean`, or delete the build directory. (Ninja: bare build OK. **Visual Studio generator: bare build = Debug ALL_BUILD = huge rebuild — MUST add `--config Release --target <target>`.** Record exact build command below.)
2. **Files:** NEVER modify `runner/*.cpp`. Fix in `src/lib/` or game overrides.
3. **Headers:** NEVER modify `.h` without asking user. Triggers mass rebuild.
4. **Git:** NEVER use destructive git commands (`checkout`, `clean`, `reset`, `stash`, `pull`).
5. **Verify:** NEVER assume file names/paths. Use tools (`list_dir`, `find_by_name`, `grep_search`).

---

# PS2 Recomp — Project State
> Auto-maintained by agent. DO NOT DELETE. Read at session start, update after every major action.

## Boot Status
- [ ] Loaded `03-ps2recomp-pipeline.md`
- [ ] Loaded `04-runtime-syscalls-stubs.md`
- [ ] Verified comprehension (3 questions answered)

## Game Info
- **Title**: <!-- e.g. Star Wars Episode III -->
- **Region**: <!-- NTSC-U / PAL / NTSC-J -->
- **Has Symbols**: <!-- yes/no/partial — affects analysis strategy -->

## Workspace Paths
<!-- CRITICAL: These are the two root paths the agent needs. 
     They may be the same directory or separate siblings.
     Phase 0 discovers these; resume sessions read them. -->
- **PS2Recomp Repo**: <!-- absolute path to cloned PS2Recomp repo (contains ps2xRecomp/, ps2xRuntime/, build64/) -->
- **Game Workspace**: <!-- absolute path to game-specific dir (contains ISO, ELF, .toml, output/) -->
- **ISO Path**: <!-- absolute path to ISO file or extracted ISO directory -->
- **Output Dir**: <!-- path to recompiler output .cpp files (often game_workspace/output/) -->

## Binaries
<!-- PS2 games can have MULTIPLE binaries that need recompilation.
     The main executable is identified from SYSTEM.CNF (BOOT2 = ...).
     Secondary binaries are discovered during analysis or at runtime.
     IMPORTANT: SLES/SLUS files have NO .elf extension! COREC.BIN is MIPS code too! -->

| Role | File Name | Type | Path | TOML Config | Status |
|------|-----------|------|------|-------------|--------|
| Main | <!-- e.g. SLES_531.55 --> | <!-- ELF / BIN / other --> | <!-- absolute path --> | <!-- path to .toml --> | <!-- analyzed / recompiled / not started --> |
| <!-- Secondary? --> | <!-- e.g. COREC.BIN --> | <!-- BIN --> | <!-- absolute path --> | <!-- path to .toml --> | <!-- status --> |

## Environment Setup
- **Ghidra Install Path**: <!-- The folder containing ghidraRun.bat (e.g., C:\ghidra_11.4.2_PUBLIC) -->

## Current Phase
<!-- Update this to reflect exactly where the project stands.
     Valid phases in order:
     PHASE_SETUP          — PS2Recomp repo not yet cloned/configured
     PHASE_ISO_EXTRACT    — Need to extract ELF from ISO
     PHASE_ELF_ANALYSIS   — Running ps2_analyzer or Ghidra analysis
     PHASE_TOML_CONFIG    — Editing TOML config (stubs, skip, patches)
     PHASE_RECOMPILATION  — Running ps2_recomp to generate C++
     PHASE_CPP_REVIEW     — Reviewing generated C++ for unhandled opcodes
     PHASE_RUNTIME_BUILD  — Compiling ps2xRuntime and headless testing
     PHASE_IO_MODULE      — Game trying to load assets, CD/file I/O
     PHASE_MENU_REACH     — Reaching main menu
     PHASE_GAMEPLAY       — In-game behavior and rendering
-->
PHASE_SETUP

## Build Configuration
- **CMake Generator**: <!-- e.g. Ninja (preferred) or Visual Studio 17 2022 -->
- **C++ Compiler**: <!-- clang-cl (preferred) or MSVC cl -->
- **Build Type**: <!-- Debug / Release / RelWithDebInfo -->
- **Ghidra CSV Path**: <!-- path to exported function map, if any -->
- **single_file_output**: <!-- true/false -->

## PCSX2 MCP Status
<!-- Updated at session start if PCSX2 is available.
     If PCSX2 is not running or not needed, set Status to "Not Connected".
     This section lets the agent know whether A/B comparison is available. -->
- **Status**: <!-- Connected (DebugServer) / Connected (Pine only) / Not Connected -->
- **Game Loaded**: <!-- title + region from pcsx2_game_info(), or "N/A" -->
- **Match**: <!-- Does the PCSX2 game match the recomp target? yes/no/N/A -->

## Active Runner Command
<!-- AGENT: Once absolute paths are established, write the exact run command here.
     Never guess or reconstruct this command from memory. Read it and execute it verbatim.
     Example (Windows):  "E:\PS2Recomp\build\ps2xRuntime\ps2xRuntime.exe" "E:\games\game.iso"
     Example (Linux/Mac): "/home/user/ps2recomp/build/ps2xRuntime" "/home/user/games/game.iso"
     Use run_command with a timeout (WaitMsBeforeAsync). Kill via send_command_input if it hangs.
-->
<!-- ACTIVE RUNNER COMMAND: -->

## Unique Crashes & Subsystem Map
<!-- MANDATORY STRUCTURED TABLE. Agent must populate and update this after every crash/fix cycle.
     Cluster similar errors. Never duplicate rows — update existing ones with new findings.
     Proposed Fix Type: "Runtime C++" | "Game Override (TOML)" | "TOML Patch (nop)" | "Syscall Stub"
     Regression Status: "Untested" | "OK" | "REGRESSED" | "N/A"
-->
| Crash Address/PC | Subsystem | Callstack/Context | Proposed Fix Type | Regression Status | Resolution |
| ---------------- | --------- | ----------------- | ----------------- | ----------------- | ---------- |
|                  |           |                   |                   |                   |            |

## Resolved Stubs
<!-- Track every stub binding. Leave rows empty if none. -->
| Address | Handler | Binding Method | Status | Notes |
| ------- | ------- | -------------- | ------ | ----- |
|         |         |                |        |       |

## Resolved Syscalls
<!-- Track every syscall implementation -->
| Syscall ID | Implementation | File | Status |
| ---------- | -------------- | ---- | ------ |
|            |                |      |        |

## Temporary Triage Stubs
<!-- ret0/ret1/reta0 stubs that still need real implementation -->
| Address | Triage Type | Caller Context | Priority | Notes |
| ------- | ----------- | -------------- | -------- | ----- |
|         |             |                |          |       |

## Unhandled Opcodes
<!-- Instructions the recompiler could not translate -->
| Address | Opcode | Type | Resolution |
| ------- | ------ | ---- | ---------- |
|         |        |      |            |

## Performance Baseline
<!-- Fill once the game runs correctly; update after any accepted optimization.
     See 17-performance-optimization.md — no optimization without a measured before/after,
     and the golden output metric must stay unchanged. -->
- **Golden output metric** (e.g. title-screen PixelNonZero band): <!-- value + how measured -->
- **FPS / frames reached in 30s** (headless smoke): <!-- value + date -->
- **Known hot spots (profiled, not guessed)**: <!-- subsystem: % frame time -->

## Known Issues
- [ ] <!-- Active issue description -->

## Known Upstream Issues
<!-- Document PS2Recomp tool bugs found during development.
     These are NOT game bugs — they're recompiler/toolchain bugs.
     See 10-agent-guardrails.md §2 for the full protocol. -->
| Affected Function/Address | What Recompiler Generates | What MIPS Actually Does | Workaround Applied | GitHub Issue |
| ------------------------- | ------------------------- | ----------------------- | ------------------ | ----------- |
|                           |                           |                         |                    |             |

## Learned Patterns (Auto-growing)
<!-- After each session, synthesize key discoveries here. These compound across sessions.
     Write PATTERNS not events: "X causes Y, fix with Z" > "X happened".
     This section is read at boot — it makes the next session smarter. -->

## Session Log
<!-- Append new entries at the top. Each entry: date, actions, discoveries, current blocker -->
### <!-- YYYY-MM-DD -->
- **Actions**: 
- **Discovered**: 
- **Current Blocker**: 
