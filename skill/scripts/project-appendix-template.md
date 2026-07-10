# Appendix: {{GAME_TITLE}} — Project-Specific Reference

> **PROJECT-SPECIFIC.** Copy to `resources/appendix-<game>-project.md` when a port graduates from
> "experiment" to "ongoing project". This file pins the CONCRETE paths, tool names, and proven
> facts for THIS game. The generic skill files (`01`–`20`, `db-*`) stay game-agnostic; this
> appendix makes them actionable here. **Live, changing state lives in `PS2_PROJECT_STATE.md`**
> (current phase, open blockers, crash table) — read that for "what's happening now"; read this
> for "how this project is wired." Don't duplicate live state here.
>
> See `resources/appendix-dc2-project.md` for a fully-populated real example (Dark Cloud 2).
> Delete sections that don't apply; add sections this game needs. Every claim in here should be
> PROVEN (point at the fix-log or harness that proved it), not aspirational.

---

## §1 Workspace Map

| Thing | Path |
|-------|------|
| Game workspace (root) | {{...}} |
| PS2Recomp repo (LIVE) | {{...}} |
| Main ELF | {{path}} ({{region code, symbols yes/no/partial}}) |
| ISO | {{...}} |
| Generated runner output | {{...}} |
| Build dir | {{...}} |
| Game override file (most fixes) | {{...ps2xRuntime/src/<game>_game_override.cpp}} |
| Syscalls / stubs (LIVE defs) | {{...}} |
| Fix logs (1 per phase) | {{...plans/phase-XX-fix-log.md}} |

> Record any "editing the wrong twin file is inert" traps here (e.g. `.inl` vs `.cpp` duplicates).

## §2 Static Analysis Export (this project's `14-static-analysis-navigation.md` instance)

- Mode: {{STATIC EXPORT / LIVE GHIDRA / none}}
- Per-function files: {{path to ref/functions/, count}}
- Indexes: {{path to ref/index/*.json, sizes — flag any too big to Read whole}}
- Current ELF stamp: `elf_hash {{...}}`, `global_pointer {{...}}`, `text_range {{...}}`
- Regeneration command (when stale): {{...}}

## §3 Build & Smoke

- Generator + compiler: {{Ninja+clang-cl / VS+MSBuild — if VS, spell out the REQUIRED
  `--config Release --target <t>` commands; a bare build is a trap}}
- Exact build commands:
  ```powershell
  {{...}}
  ```
- Runner exe name (for taskkill): {{...}} (do NOT assume `ps2EntryRunner.exe`)
- Active run command: {{...}} (mirror of `PS2_PROJECT_STATE.md → Active Runner Command`)
- Golden smoke test + expected metric: {{harness path + the numeric pass band}}
- Benign build warnings to ignore: {{list them — anything else = real}}

## §4 PCSX2 A/B Setup

- MCP connection: {{port / build}} · Raw TCP details if used: {{host:port, protocol quirks}}
- Known constraints: {{e.g. EE RAM + scratchpad only; VU/GS reads return 0 → use .gs dumps}}
- Reference dumps / captures: {{paths}}
- Reusable A/B harnesses: {{tool paths}}
- Semantic oracle source tree (if present): {{e.g. local PCSX2 source path + the go-to files}}

## §5 Headless Input Injection / Test Infra

- How scripted input works here: {{env vars / harness + syntax}}
- Gotchas: {{byte-swaps, hooks that die in certain scenes, screens unidentifiable visually, …}}

## §6 Proven Facts (instances of the generic playbooks)

> One bullet per DURABLE proven fact: what was proven, the kill-switch/env-flag if any, and the
> fix-log that is its evidence. These are the facts a fresh session must not re-derive.
- {{...}}

## §7 Durable Regen / Allocator / Override Rules

- After any recompiler regen: {{fix-up scripts to re-apply, audits to re-run (allocator family!,
  stub collisions), in order}}
- Override dispatch rules confirmed for this game: {{direct-jal bypass instances, etc.}}

## §8 Upstream PR ↔ Gap Map (optional)

- {{Which upstream PS2Recomp PRs map to which local gaps; adopt-surgically notes per
  `10-agent-guardrails.md` §2}}
