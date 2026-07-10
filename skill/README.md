<p align="center">
<img width="512" height="512" alt="Ps2-Recomp-Agent-SKILL" src="https://github.com/user-attachments/assets/ef6a9300-ea65-4364-9a67-75602ebf59a5" />
</p>

Welcome to the **PS2 Recomp Mastery**: a complex, hyper-structured Operating System for LLM Agents (Claude Code, Antigravity, Cursor, …) that gives them architectural knowledge, procedural workflow and persistent memory required to autonomously reverse engineer and recompile PlayStation 2 games through PS2Recomp project (even if still in dev process).

> **Multi-environment:** the resource files use Windsurf-style tool names (`view_file`, `grep_search`, …). `SKILL.md §1.5 Tool Adapter` maps them to Claude Code / Cursor equivalents — the agent translates automatically.

This guide explains how *you*, the human driver, should use this skill to extract maximum performance from the AI.

> **What changed recently?** See [`CHANGELOG.md`](CHANGELOG.md) — every skill change, why it was made, and a redirect to the PCSX2 source line or the fix-log that is its evidence.

---

## ⚠️ How to Treat the Agent

Before using this, you must change how you interact with the LLM. 
**Do not treat it as a chatbot. Treat it as a Junior Reverse Engineer working on your machine.**

1. **It is Autonomous:** The agent runs cmake builds and the game executable directly. Let it work. Do not compile for it unless it specifically asks.
2. **It has Persistent Memory:** The agent will create a `PS2_PROJECT_STATE.md` file in your root directory. This is its "external hippocampus". It allows you to pause a session on Monday, start a new chat on Thursday, point to the state file, and the agent will resume exactly where it left off without forgetting what registers it was analyzing.
3. **It has Circuit Breakers:** If the agent gets stuck in a loop (failing the same crash 3 times), it is programmed to physically stop, read hardware documentation, or ask for your help. It will not burn your tokens infinitely.

---

## 🛠️ Prerequisites (Your PC Setup)

For the agent to work flawlessly, your machine must have the following ready:

1. **[Visual Studio](https://visualstudio.microsoft.com/) (C++ Desktop Workload)**: Required for the MSBuild native tools and vcvars64 environment.
   - **⚠️ CRITICAL: You MUST install Clang and Ninja.** Open the Visual Studio Installer, go to "Individual Components", and enable **"C++ Clang Compiler for Windows"** and **"C++ CMake tools for Windows"**. The agent will refuse to build without them.
2. **CMake**: Ensure CMake is installed and available in your PATH. (Usually bundled with the VS CMake tools above).
3. **Python 3.x**: Optional — only needed if using helper scripts.
4. [**Ghidra 11.4.2**](https://github.com/NationalSecurityAgency/ghidra/releases/tag/Ghidra_11.4.2_build):
   - Installed with the [EmotionEngine Reloaded Plugin](https://github.com/chaoticgd/ghidra-emotionengine-reloaded).
   - Installed with the [**GhydraMCP**](https://github.com/starsong-consulting/GhydraMCP) extension running on port 8192 (CodeBrowser must be open with the ELF).
   - **Crucial**: Ensure `mcp_config.json` inside your AI environment (Cursor/Antigravity) is configured to connect to the local GhydraMCP server. This allows the *Agent* to drive Ghidra, not you!
5. [**PCSX2-MCP**](https://github.com/hkmodd/PCSX2-MCP) (DebugServer build):
   - A custom PCSX2 build with an integrated DebugServer that exposes full debugging capabilities (128-bit registers, breakpoints, memory read/write, native disassembly) via MCP.
   - **Required for runtime debugging and A/B comparison** between real PS2 execution and recompiled output.
   - Ensure `mcp_config.json` in your AI environment points to the PCSX2 MCP server (default port: `21512` for DebugServer, `28011` for Pine IPC).
   - The agent will use this to set breakpoints, inspect registers, compare memory snapshots, and diagnose crashes that can't be solved from static analysis alone.
6. **Skill Placement**: Place the `ps2-recomp-Agent-SKILL/` folder inside your root PS2Recomp workspace, alongside `ps2xRecomp` and `ps2xRuntime`.

---

## 🚀 Start a Session

### The Universal Starter Prompt

Copy-paste this **entire block** into your AI IDE (Cursor, Antigravity, Claude Code, etc.) to activate the skill. It works whether you have a brand-new project, a half-done manual port, or need to resume from a previous session.

```text
Read the skill file `ps2-recomp-Agent-SKILL/SKILL.md` and BOTH boot sequence references
(`resources/03-ps2recomp-pipeline.md` and `resources/04-runtime-syscalls-stubs.md`).
Do NOT proceed until you have read all three files.

Then execute this startup sequence:

1. INSPECT my workspace. Look for:
   - Any `PS2_PROJECT_STATE.md` (persistent memory from a previous session)
   - Any build directory (`build64/`, `build/`, etc.)
   - Any `game.toml` or ELF files (may be in a SEPARATE game workspace folder!)
   ⚠️ DANGER: Do NOT list or search inside `runner/` directories!
   They contain 30,000+ files and WILL crash you. Instead use:
     Test-Path ps2xRuntime/src/runner          (True/False)
     (Get-ChildItem ps2xRuntime/src/runner -Filter *.cpp).Count  (number)
   
   NOTE: Game files (ISO, ELF, TOML, recomp output) are often in a sibling
   directory next to PS2Recomp, not inside it. If you can't find them here,
   ASK for the game workspace path.

2. If a build directory exists, READ `CMakeCache.txt` and report:
   - Generator (Ninja? Visual Studio?)
   - Compiler (clang-cl? MSVC cl.exe?)
   - Build type (Release? Debug?)
   Then rate the config (⚡ Optimal / ⚠️ Acceptable / ❌ Critical) and
   suggest improvements if needed. Do NOT change anything without my OK.

3. ASK me for anything you still don't know. Typical questions:
   - What game am I porting? (title + region code like SLES_531.55)
   - Where is the ISO? (absolute path)
   - What phase am I in? (or should you infer it from the workspace?)

4. REPORT: Tell me what you found, what phase I'm in, and what the
   next concrete step is. Then wait for my go-ahead.
```

> **Why this works:** The prompt forces the agent to *read first, detect second, ask third, act last*. It cannot skip the skill files, cannot assume your build config, and cannot start breaking things without your approval.

### Quick Resume (new chat, same project)
If you already used the starter prompt before and just need to resume (e.g., context degradation, new chat window):

```text
Read the skill `ps2-recomp-Agent-SKILL/SKILL.md` and the boot sequence references.
Then read `PS2_PROJECT_STATE.md` to recover our progress. Resume from there.
```

### Fresh Game (you know exactly what you want)
If you're starting a new game and want to skip the Q&A:

```text
Read the skill `ps2-recomp-Agent-SKILL/SKILL.md` and the boot sequence references.
I'm porting [GAME NAME] ([REGION CODE], e.g. SLES_531.55).
ISO is at: [ABSOLUTE PATH TO ISO]
PS2Recomp repo is at: [ABSOLUTE PATH TO PS2RECOMP]
Game workspace is at: [ABSOLUTE PATH TO GAME FOLDER]
(If game workspace = PS2Recomp, just say "same")
Start at Phase 0 — detect my build config and report before doing anything.
```

---

## 🤝 How to Collaborate (The Human-in-the-Loop)

While the agent is highly autonomous, PS2 reverse engineering requires your eyes:

- **Monitor `PS2_PROJECT_STATE.md`**: Open this file in split-screen. You will see the agent filling out tables of resolved stubs, triage attempts, and unhandled opcodes in real-time. If it hallucinated something, correct the Markdown file directly. The agent will read your correction on the next refresh.
- **Beware Context Degradation**: If you have been chatting for hours and the Agent asks stupid questions, its context window is full. Stop immediately. Open a new chat and use the **Quick Resume** prompt above. The Agent is programmed to warn you when it feels this happening.
- **Ensure Ghidra is Ready**: The agent drives Ghidra, but *you* must open Ghidra, perform the initial auto-analysis on the game's ELF, and leave the CodeBrowser window open with the GhydraMCP plugin running.
- **Do not interrupt builds**: When the agent runs `cmake --build`, let it finish. It might take 10-20 seconds. The agent will read the compiler output and fix syntax errors itself.

---

## 🚨 Troubleshooting

* **The Agent asks me to compile the game:** Tell it: "No, read your Skill. You must run `cmake --build build64` directly."
* **The Agent is guessing blindly and crashing:** Tell it: "You are violating the Circuit Breaker. Implement Dynamic Probing (empirical telemetry) as described in your Playbook to read the actual register values."
* **The Agent forgets an address:** Tell it: "Do a Context Refresh. Read `PS2_PROJECT_STATE.md`."
* **The game runs correctly but too slowly:** Tell it: "Load `17-performance-optimization.md`. Measure first — no speculative optimization."
---

## 🙏 Acknowledgements & Credits

This AI Skill directly leverages the revolutionary [**PS2Recomp**](https://github.com/ran-j/PS2Recomp) project created by **ran-j**. PS2Recomp is a monumental milestone in PlayStation 2 preservation and static recompilation. This AI workflow is built strictly *around* his open-source pipeline to automate the heavy lifting of reverse engineering. Huge thanks to ran-j and all contributors pushing the boundaries of the PS2 scene!

---

*Created by the Antigravity Deepmind System for flawless PS2 porting.*
