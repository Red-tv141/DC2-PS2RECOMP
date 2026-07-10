# Decisional Brain — The Reasoning Engine

> **Purpose:** This document teaches you HOW TO THINK, not what to do. The SKILL has procedures, flowcharts, and fix taxonomies. This document is the **conscious reasoning loop** that connects them — the "why" behind every action.

---

## §1 The Reasoning Loop

Execute this loop for EVERY problem. No shortcuts, no skipping steps.

```
┌─────────────────────────────────────────────────────────────┐
│                    THE LOOP                                  │
│                                                             │
│   ┌──────────┐                                              │
│   │ OBSERVE  │ ← What is the symptom?                       │
│   └────┬─────┘   (exact error, crash PC, visual glitch,     │
│        │          hang, wrong output, missing feature)       │
│        ▼                                                    │
│   ┌──────────┐                                              │
│   │  LOCATE  │ ← Where in the architecture?                 │
│   └────┬─────┘   (which layer: MIPS translation? Runtime?   │
│        │          PS2 hardware? Host OS? Overlay loading?)   │
│        ▼                                                    │
│   ┌──────────────┐                                          │
│   │  UNDERSTAND  │ ← Why does the ORIGINAL PS2 code work?   │
│   └────┬─────────┘   (Ghidra: decompile the MIPS.           │
│        │              PCSX2 MCP: see the running state.      │
│        │              What does this function ACTUALLY do?)  │
│        ▼                                                    │
│   ┌──────────┐                                              │
│   │  DECIDE  │ ← Which ONE of the 4 tools fixes this?       │
│   └────┬─────┘   TOML / Runtime C++ / Override / Recompiler │
│        │         If you can't pick ONE → you didn't finish   │
│        │         UNDERSTAND. Go back.                        │
│        ▼                                                    │
│   ┌──────────┐                                              │
│   │  VERIFY  │ ← Did the fix work?                          │
│   └────┬─────┘   Build → Run → Compare.                     │
│        │         If still broken → back to OBSERVE           │
│        │         with NEW DATA, not the same guess.          │
│        │                                                    │
│        └─── If verified → update PS2_PROJECT_STATE.md       │
│             with Learned Pattern: "X causes Y, fix with Z"  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### The Golden Rule

**If you cannot explain WHY your fix is correct in one sentence, you are guessing.** Go back to UNDERSTAND.

---

## §2 The "Why" Checklist

Before writing ANY fix, answer these four questions. If you can't answer all four, you are NOT ready to write code.

| # | Question | Bad Answer | Good Answer |
|---|----------|-----------|-------------|
| 1 | **Why does this crash/bug happen?** | "There's a null pointer" | "Function `sub_1E25A0` reads from address stored in `a0`, but the recomp passes `0` because the calling convention for 128-bit args isn't handled" |
| 2 | **What did the original PS2 code expect here?** | "I don't know, I'll stub it" | "GhydraMCP shows the MIPS loads a pointer from `gp+0x3A40` which is initialized by `sub_100200` during boot — the runtime never calls that init function" |
| 3 | **Why does my fix address the root cause?** | "It stops the crash" | "Adding the init call in `ps2Init()` populates the global pointer table, so `sub_1E25A0` gets the correct address instead of null" |
| 4 | **What will break if my fix is wrong?** | "Nothing" | "If the offset is wrong, every function using `gp+0x3A40` will read garbage — that's ~15 functions in the subsystem. Watch for cascading crashes." |

---

## §3 Diagnosis Escalation Ladder

When facing a bug, escalate through these levels IN ORDER. Each level gives you more information. Most bugs are solved at levels 1-2. Complex bugs need levels 3-4.

```
Level 1: STDOUT/STDERR
   │  Read the crash output. Look for:
   │  • Crash address → map to function (Ghidra)
   │  • Error message → tells you what's missing
   │  • Assertion text → tells you what went wrong
   │  • Stack trace → tells you the call chain
   │
   │  Solved? → Fix and verify.
   │  Not enough info? → Level 2.
   ▼
Level 2: SUBSYSTEM MAP
   │  From 10-agent-guardrails.md §3.4:
   │  Which Runtime file handles this address range?
   │  Which PS2 subsystem is involved? (DMA? GS? VIF? SPU2?)
   │  Read the relevant source file → understand current implementation.
   │
   │  Solved? → Fix and verify.
   │  Know WHAT but not WHY? → Level 3.
   ▼
Level 3: UNDERSTAND THE ORIGINAL CODE (Static Analysis)
   │  PREFERRED — static export (see 14-static-analysis-navigation.md):
   │    Read ref/functions/0x<addr>_*.md → decompiled C + asm + AI
   │      stub-vs-recompile disposition + callers/callees + globals.
   │    globals_index.json → who reads/writes the state this fn uses.
   │    calls_index.json / the file's Calls section → who calls this?
   │  FALLBACK — live Ghidra MCP (only if no static export):
   │    mcp_ghydra_functions_decompile(address) / _xrefs_list(to_addr)
   │  Ask: what does it actually do? what mem/regs? what does it call?
   │
   │  Solved? → Fix and verify.
   │  Know the code but not the runtime state? → Level 4.
   ▼
Level 4: PCSX2 MCP (Runtime A/B Comparison)
   │  See 12-pcsx2-mcp-playbook.md §3 for the full workflow.
   │  1. Breakpoint in PCSX2 at the problem address
   │  2. Read registers + memory = ground truth
   │  3. Compare with recomp behavior
   │  4. First divergence = root cause
   │
   │  Solved? → Fix and verify.
   │  STILL stuck after all 4 levels? → Circuit Breaker.
   ▼
Level 5: CIRCUIT BREAKER
   Load the relevant db-*.md file (ask db-ps2-index.md to route).
   If still stuck after loading reference material → ASK THE USER.
   Present what you know, what you tried, what you suspect.
   Do NOT keep looping without new information.
```

---

## §4 Decision Patterns — When to Use Each Tool

### TOML (`game.toml`)

**Use when:** The problem is in how the recompiler TRANSLATES the function, not in what the function DOES.

| Signal | TOML Action |
|--------|------------|
| Function crashes immediately on entry | `stub` — it's likely init code that's not needed in recomp |
| Function is an infinite polling loop | `skip` or `nop` — the PS2 hardware being polled doesn't exist |
| Function does direct hardware I/O that's handled elsewhere | `nop` — already covered by Runtime |
| Syscall vector is wrong | `patch` — fix the address |

**Anti-signal:** If the function does GAMEPLAY LOGIC (not hardware), stubbing is hiding a bug.

### Runtime C++ (`src/lib/*.cpp`)

**Use when:** The recompiled code is calling PS2 hardware that doesn't exist on the host.

| Signal | Runtime Action |
|--------|---------------|
| Crash in DMA/VIF/GIF/GS code | Implement the hardware interface in Runtime |
| Missing syscall | Add to syscall table |
| Function expects PS2 memory layout | Implement memory-mapped I/O handler |
| Audio/input/filesystem access | Implement host-side translation |

**Anti-signal:** If the crash is in game LOGIC (math, AI, physics), the Runtime doesn't help.

### Game Override (`ps2xRuntime/src/<game>_game_override.cpp` — exact path varies; check state file/appendix)

**Use when:** A specific function's RECOMPILED C++ is semantically wrong or can't work on x64.

| Signal | Override Action |
|--------|----------------|
| Function uses 128-bit registers that the recompiler can't handle | Write C++ equivalent using the original MIPS semantics |
| Function has self-modifying code | Reimplement the computed behavior |
| Function relies on precise PS2 timing | Write host-equivalent with different timing |
| Decompiled MIPS shows clear purpose but recompiled C++ is garbled | Clean reimplementation |

**Anti-signal:** If you're overriding more than ~5 functions for the same subsystem, you probably need a Runtime implementation instead.

### Recompiler (`ps2_recomp`)

**Use when:** The recompiler itself needs to re-process the ELF.

| Signal | Recompiler Action |
|--------|-------------------|
| New ELF/overlay discovered | Recompile with updated TOML |
| TOML changed (stubs, patches) | Rerun to regenerate runners |
| Address ranges updated | Recompile affected ranges |

**Anti-signal:** Never rerun the recompiler "just to see if it helps." It takes a full rebuild after.

---

## §5 Anti-Patterns — Thinking Mistakes

These are the cognitive traps that waste hours. Recognize them, stop, correct.

### "This looks like the same bug as before"

**WRONG.** Every crash is unique until you VERIFY it's the same root cause. Same crash address ≠ same bug. Different call chains can reach the same instruction with different state.

**Fix:** Read the registers/stack. If the state is different from last time, it's a new bug.

### "The fix worked for another function, so I'll apply it here too"

**WRONG.** Each function has different semantics. A stub that's correct for a boot-time init function is WRONG for a gameplay update function.

**Fix:** Go through the reasoning loop for THIS function. UNDERSTAND what it does before choosing the fix.

### "I'll just stub this and move on"

**WRONG** (unless it's genuinely unreachable code). Stubbing gameplay code hides bugs that will resurface later as mysterious glitches, crashes in unrelated areas, or missing game features.

**Fix:** Before stubbing, verify with Ghidra what the function does. If it's called during gameplay, it matters. Find the real fix.

### "It compiles, so it works"

**WRONG.** Compiling proves syntax. Running proves behavior. COMPARING proves correctness.

**Fix:** Build → Run → Read output → Compare with expected behavior. Only THEN claim it works.

### "I'll add more logging and try again"

**WRONG** (after the first round). If one round of logging didn't diagnose it, more logging won't either. You need a different diagnostic approach.

**Fix:** Escalate to the next level in the Diagnosis Ladder. Level 1 (stdout) failed? Use Level 2 (subsystem map). Level 2 failed? Use Level 3 (Ghidra). Level 3 failed? Use Level 4 (PCSX2 MCP A/B comparison).

### "I'm confident I know the answer without checking"

**THE MOST DANGEROUS TRAP.** Confidence without verification = hallucination risk. The SKILL says this explicitly: *"When confident without verification → Re-read source."*

**Fix:** Check. Always check. Read the actual code, run the actual binary, compare the actual state. Then be confident.

### "I'll build a lever to flip this branch / force this value and see what changes"

**WRONG until you prove the path is REACHABLE and the branch is SATISFIABLE.** A real port burned
THREE phases perturbing a "+2048 cull gate," trying to find the input that opens it — then someone
finally read the operands: the gate was `IBEQ VI10, VI7` with `VI10 = 208` (a constant) and
`VI7 ∈ {0,1}` (a boolean). `208 ≠ {0,1}` → the branch is **structurally never-taken by any value**.
No lever could ever have opened it. All three phases were chasing a dead path.

**Fix:** Before building a probe/lever to change a branch outcome or "force" a code path, READ the
operands and constants feeding the condition. Confirm the branch *can* flip and the path *is*
reached (count actual executions). A never-taken branch, an unreachable block, or a value pinned by
a constant is a trap — perturbing its inputs proves nothing and wastes phases.

### "I established / refuted this N phases ago, so it still holds"

**WRONG — findings have a shelf life.** A diagnosis is only valid for the **build state it was
measured on.** A verdict of "the transform packer never draws" went STALE the moment two unrelated
VU fixes (MAC flags, Q-latency) landed — the packer then *did* draw, and the old verdict sent the
next phase down a refuted path. Conversely, the same refuted hypotheses get re-litigated across
phases because nobody wrote down "this is NOT the bug, here's the proof."

**Fix:** (1) When you fix something that touches a path, RE-VERIFY any prior finding about that path
before relying on it. (2) Record every rejected hypothesis with its proof in the phase fix-log's
"Rejected hypotheses / Do-NOT-carry-forward" section (see `10-agent-guardrails.md`) so future phases
don't re-chase it — and tag each finding with the build/condition it was measured under.

### "The crash trace shows it's in function X, so X is the bug"

**WRONG — the dispatch `trace=` tail is NOT a literal call stack.** It's a recorded-PC window; it can
cross registered wrapper hooks that merely fall through to the original. A real crash that traced
through the title's clip hooks looked like a title regression — it was an independent dungeon bug;
the hooks were just in the recorded window. Near-null vtable dispatch (`bad=0x1`/small) is often a
NULL `this`, not a garbage vtable.

**Fix:** Localize by reading the actual state at the fault — the call's `a0` (is `this` null?), `sp`,
`ra` — not by trusting the trace tail. Bisect by killing all band-aids/levers: if the symptom is
byte-identical with every recent fix disabled, those fixes are not the cause.
