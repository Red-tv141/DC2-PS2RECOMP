# Reference: Agent Guardrails & Self-Correction Protocols
> Load this when you make a repeated mistake, hit the Circuit Breaker (3 Strike Rule), or need to self-diagnose behavioral drift.

## 1. Agent Mistake Taxonomy — Your Own Failure Modes

These are NOT PS2 bugs. These are mistakes YOU (the agent) make repeatedly. Learn them:

| Your Mistake | Why It Happens | Prevention |
|-------------|---------------|------------|
| Editing a `.h` file "just to add one field" | You forget the 30K recompilation cost | §PROHIBITION #3 — ALWAYS check. If in doubt, it's a .h and you can't touch it. |
| Creating temp scripts in project root | No cleanup protocol was encoded (now it is: prohibition #11) | Put ALL temp files in `/tmp/`. Clean up before ending. |
| Reading a huge log file in one shot | You forget context is finite (~200K tokens) | Max 200 lines per read. Use `OutputCharacterCount=5000`. |
| Appending to the same log file across runs | You forget previous runs accumulate | Overwrite every time. Better: read from stdout directly. |
| Not re-reading the state file | You trust your memory (your memory is HALLUCINATION-PRONE) | Follow the Mandatory Trigger table. |
| Confident without verification | Classic LLM hallucination pattern | Verify. Build. Run. Read output. THEN claim success. |
| Trying 3+ fixes for the same crash without diagnosis | Guessing instead of reasoning | 3 Strike Rule exists — USE it. Load the db file. |
| Leaving game processes running | Forgot to kill the game after testing | Always `Terminate` via `send_command_input` after reading output. |
| Writing a fix without reading the state file first | Context drift — you forgot the current state | Mandatory Trigger: "Before writing ANY C++ code" → re-read state file. |
| Trusting the runtime's OWN labels/tables as ground truth | The interpreter's case labels *look* authoritative, and your analysis tools were built to mirror them — so tool + runtime lie in unison (shared-bug hazard) | Verify opcode/dispatch mappings against an EXTERNAL oracle (PCSX2 dispatch tables, HW manuals), never against the code under suspicion. A real port "proved" a branch impossible for ~70 phases because FMEQ/FMAND were swapped in BOTH the interpreter and the disassembler. When disassembly says "impossible" but HW does it with byte-identical microcode, audit the opcode table + flag/pipeline timing FIRST. |
| Accepting an aggregate-statistics match as convergence | Totals (prim counts, ADC %, on-screen %) can match while per-item patterns are completely different | Compare at the finest granularity the reference allows (per-strip / per-vertex / per-packet). See `15-vu1-gs-debugging.md §4.0` packet-level A/B — build the runner dump in the SAME container as the HW reference so one parser serves both. |
| Repairing a GLOBAL boot invariant on only the route that crashed | The fix was written while debugging one route, so it got scoped to that route's wrapper | A restore-the-boot-invariant repair (un-run `__sinit_*` → null global vtable, unfunded heap, etc.) protects a SHARED object — audit EVERY consumer route. A real port repaired the main scene's vtable in the dungeon-loop init only; the town-loop init has the byte-identical `scene->Initialize()` dispatch and stayed broken (flat blue) for ~7 phases (F50.4 → G193). When you write such a repair, grep for every LoopInit/state-entry that dispatches through the same object and cover them all (or hoist the repair to one earlier common point). |
| Diagnostic registration silently REPLACING a repair wrapper | `registerFunction` is last-wins; a trace canary registered over an address evicts whatever repair/probe wrapper the default table installed there | Before registering a canary on an address, check what the default table installs — chain to THAT wrapper, not the raw recompiled body. A real port's `TRACE_G58` canary on `Draw__8mgCFrame` bypassed the distance-cull repair the default table had, so every traced run silently ran DIFFERENT rendering than the run being debugged (G193 finding). A traced run that behaves differently from the untraced run is the tell. |
| Chasing missing-file open errors that are retail-normal | fopen/fioOpen failure lines look like load failures, so they read as the cause of a missing scene | The original game probes optional files (`.sky`, `.cfg`, `.efp` variants) that are ABSENT on the retail disc; real HW gets the same misses and renders fine. Before chasing: look the exact path up in the game's own archive index (parse `DATA.HD2`-style tables from the ISO directly) — if the file isn't on the disc, the error is a decoy (G192). |

---

## 2. Upstream Awareness — PS2Recomp Is a Living Tool

PS2Recomp is under **active development**. It has open bugs. You WILL encounter situations where the tool itself produces incorrect output. This is normal — don't hack around it, handle it methodically.

**Known issue categories** (check `https://github.com/ran-j/PS2Recomp/issues`):
- **Codegen bugs** — Wrong C++ emitted for certain MIPS patterns (branch thunks, mixed VU0/MMI)
- **Missing syscalls** — Syscall numbers the recompiler doesn't know about (0x5b, 0x6, etc.)
- **Output bloat** — Functions generating far more C++ than expected
- **Missing stubs** — PS2 SDK functions with no default binding

**When to suspect a tool bug (not your code):**
1. The generated `out_*.cpp` has obviously wrong C++ (e.g., dead code loops, unreachable returns, wrong operand order)
2. A MIPS instruction gets translated to something that makes no architectural sense
3. The recompiler crashes or silently skips functions
4. The same pattern works for one function but fails for another similar function

**Symptom-shape triggers — these patterns scream "codegen bug," not runtime/game** (learned the
expensive way — a port burned ~50 phases at the wrong layer before checking the recompiler):
- **2D/UI/title works but 3D/geometry is broken.** Full-vector `.xyzw` ops have symmetric masks so
  they survive a lane/mask bug; partial-dest VU0/COP2 (`.xy`, `.z`, `.yzw`) corrupt. A whole-screen
  3D collapse with working 2D = suspect partial-dest codegen.
- **A whole CLASS of math is wrong** (every transform, every cross-product, every clip) rather than
  one function — points at the shared codegen for that op family, not 12 separate game bugs.
- **Symmetric test data passes, distinct data fails.** If you only ever fed `(1,1,1,1)` / all-ones,
  you can't see a shuffle/lane/mask reversal. **Always test VU/SIMD codegen with DISTINCT per-lane
  values** (e.g. `(100,80,200,1)`).
- A value reads back as the **raw IEEE-754 bit pattern** of an unconverted float, or a control-reg
  read returns an unrelated field — a lane/index mapping is reversed.

**How to confirm a codegen bug WITHOUT a full game route (the fast path):**
1. **Differential characterization harness.** Call the recompiler library's emit function (e.g.
   `CodeGenerator::translateInstruction`) directly in a tiny test, assert the emitted C++ against
   the **architectural truth**. Far faster than rebuild-run-eyeball, and it isolates codegen from
   runtime. (Real example: `cop2_bug_characterization.cpp` confirmed 6 COP2 defects + 1 control-reg
   defect in one run.)
2. **Use PCSX2 SOURCE as the semantic oracle** (not just the runtime A/B). For VU/COP2 semantics the
   authority is `pcsx2/VUops.cpp`, `pcsx2/VUflags.cpp`, `pcsx2/VU.h` — read the exact lane order,
   flag rules, outer-product pairing, control-reg indices, denormal/clamp handling there and match
   the codegen to it.
3. **Cross-check internal consistency** in ONE generated function: a partial-dest mask must agree
   with the `lqc2`/`sqc2` (`READ128`/`WRITE128`, X=lane0) and broadcast-shuffle lane order in the
   *same* file. Disagreement = the bug.
4. **AUDIT THE WHOLE CLASS, don't fix only the op that bit you.** When you find one codegen/interpreter
   defect, sweep every sibling op of the same family (all partial-dest, all control-reg maps, all
   outer-products) against the oracle — some defects are *dormant* (the game doesn't hit them yet)
   but will surface later. (F51.8 found 6 by auditing after fixing 1.)

**Protocol when you find an upstream issue:**

```
1. CONFIRM: Is this really a tool bug? Compare the Ghidra disassembly of the original
   MIPS with the generated C++. If the C++ doesn't match the MIPS semantics, it's the tool.

2. WORK AROUND CLEANLY: Don't patch the generated file. Instead:
   - TOML: stub or skip the broken function
   - Game Override: replace the broken function with a correct C++ implementation
   - TOML patch: NOP out the broken instruction(s)

3. DOCUMENT: Add a note to PS2_PROJECT_STATE.md under a "## Known Upstream Issues" header:
   - Which function / address is affected
   - What the recompiler generates vs. what the MIPS actually does
   - What workaround you applied
   - Link to the GitHub issue if one exists (or suggest opening one)
```

**Do NOT:** silently work around tool bugs without documenting them. The user may want to report them upstream, and future sessions need to know which workarounds are "permanent" vs "waiting for a tool fix."

**Adopting an upstream fix onto a customized fork — apply SURGICALLY, never `git merge`/cherry-pick.**
A working port's runtime is a **heavily-diverged fork** (hundreds–thousands of lines of local changes
in the very files a useful upstream PR touches: `ps2_runtime.cpp`, `Thread.cpp`, `Sync.cpp`,
`ps2_memory.h`, …). A blind merge/cherry-pick will clobber working local fixes and silently
regress validated routes.
- **Evaluate each hunk against local divergence.** Measure how far the touched files have drifted
  from the PR base. A 0-line-diverged header = clean apply; a 1000-line-diverged core file = port the
  *idea* by hand, adapted to local code.
- **Take the smallest correct form.** Prefer the lightweight version of a fix (e.g. a post-wake
  yield) over the heavy machinery (epoch-based handoff) unless the symptom demands it. Skip parts that
  pull new build dependencies (ffmpeg) or rewrite subsystems you've already validated (GS swizzle).
- **Don't adopt incomplete upstream work** (the author's own "missing X / basic" caveats) just because
  it "would likely be neutral" — neutral + incomplete + core-path rewire is not worth the risk without
  a route to A/B it.
- **A/B every applied hunk** against the golden baselines; record what was applied vs deferred vs
  skipped and WHY (a PR-adoption log), so a future rebase knows the state. Treat a large upstream PR as
  a **design reference for your own scoped fix**, not a drop-in.
- **Use the template:** copy `scripts/pr-adoption-log-template.md` → `plans/pr-adoption-<PR#>.md`
  BEFORE touching the first hunk. It walks the divergence assessment, per-hunk decisions, and the
  A/B verification in order.

### 2.1 High-Confidence Recompiler Codegen Defects & Test Recipes

During audits of the `ps2_recomp` code generator (`code_generator.cpp`), several critical component-order and lane-mapping bugs were identified. These bugs silently corrupt vector arithmetic, causing 3D geometry collapse while leaving symmetric 2D/UI code working. 

If you suspect a recompiler codegen issue, check if these common defects are present in the recompiler code generator:

1. **`VSQD` Partial-Destination Write Mask Bug:**
   - **Symptom:** Silent data corruption to VU memory on any partial-mask `VSQD` instruction.
   - **Defect:** In `CodeGenerator::translateVU_VSQD`, the blend mask is incorrectly built (e.g., using `(dest_mask & 0x2)` for lane Y instead of `0x4`, and `0x1` for lane X instead of `0x8`).
   - **Correct Form:** Mask arguments to `_mm_set_epi32` must map `X←0x8`, `Y←0x4`, `Z←0x2`, `W←0x1`.

2. **`VMR32` Rotation Shuffle Bug:**
   - **Symptom:** Compiles, but duplicates components into incorrect lanes (e.g. outputting `(y,x,x,x)` instead of rotating components `(y,z,w,x)`).
   - **Defect:** In `VU0_S2_VMR32`, the shuffle uses `_MM_SHUFFLE(0,0,0,1)` instead of `_MM_SHUFFLE(0,3,2,1)`.
   - **Correct Form:** Rotates the source components correctly to `(y,z,w,x)` and respects the destination field mask using blendv.

3. **`VCLIPw` Clip Flag Bit-Order & Absolute Value Swap:**
   - **Symptom:** Inverted clipping judgements (culling wrong faces/edges) in frustum and backface cull checks.
   - **Defect:** In `VU0_S2_VCLIPw`, the "+" and "−" comparison flags are transposed (e.g., bit 0 set to `x < -w` and bit 1 to `x > +w`, which is reversed from the PS2/PCSX2 hardware mapping). Furthermore, comparisons may check against raw `w` rather than absolute `|w|`.
   - **Correct Form:** Map `bit0/bit2/bit4` to `> +|w|` and `bit1/bit3/bit5` to `< -|w|`.

4. **`VOPMSUB` / `VOPMULA` Missing Cross-Product Swizzle:**
   - **Symptom:** Cross-product/plane-normal calculations (lighting, face normals) are completely wrong.
   - **Defect:** The generator performs a plain component-wise multiply (`fs * ft`) instead of the rotated component pairings required by the outer product (`fs.y*ft.z`, `fs.z*ft.x`, `fs.x*ft.y`).
   - **Correct Form:** Perform a shuffle-multiply pairing matching the mathematical cross-product.

#### Standard Verification Recipes (Distinct-Lane Testing)
Never test vector math with symmetric vectors like `(1,1,1,1)`. Always verify using **distinct-lane vectors**:
- **Float Vector V** = `X=1.0, Y=2.0, Z=4.0, W=8.0`
- **Integer Vector Vi** = `X=0x11111111, Y=0x22222222, Z=0x33333333, W=0x44444444`

Verify the instructions return the following exact lane layouts (`(lane0, lane1, lane2, lane3)` = `(X, Y, Z, W)`):
- **VSQD.xy**: Pre-fill memory with sentinel `(9,9,9,9)`. Store should yield `(1.0, 2.0, 9.0, 9.0)`.
- **VMR32**: Expect `(2.0, 4.0, 8.0, 1.0)`.
- **VCLIPw**: `VCLIPw V, 10.0` should yield `0x01` (X > +w). `VCLIPw V, -10.0` should yield `0x02` (X < -w, verifying absolute value).
- **VOPMULA**: Sourced from `fs=(1,2,3,_)` and `ft=(4,5,6,_)` should yield `ACC=(12, 12, 5, _)`.

---

## 3. Problem Resolution — The Core Reasoning Engine

> Every crash, every build error, every logic bug must pass through this decision framework.
> If you skip it, you WILL end up manually patching generated .cpp files — which is ALWAYS wrong.

### 3.1 The Fix Taxonomy — Your 4 Tools

You have exactly **4 tools** to fix anything. There is no 5th option. If you can't map a problem to one of these, you don't understand the problem yet.

| # | Tool | What it does | When to use | Files touched |
|---|------|-------------|-------------|---------------|
| 1 | **TOML Config** | Declarative: stubs, skips, patches, nops | Function should be skipped, stubbed (ret0/ret1), or nop'd out. No C++ needed. | `game.toml` |
| 2 | **Runtime C++** | Implements PS2 hardware in native code | Syscalls, DMA, GS, SPU, memory allocation, file I/O, timer, threading | `ps2xRuntime/src/lib/*.cpp` |
| 3 | **Game Override** | Per-game C++ function replacing recompiled code | A recompiled function produces wrong behavior that can't be fixed at the runtime layer. Registered via `PS2_REGISTER_GAME_OVERRIDE`. | `ps2xRuntime/src/lib/game_overrides.cpp` |
| 4 | **Re-run Recompiler** | Regenerate runner code from updated TOML | TOML stubs/patches changed, or new binary needs recompilation | `ps2_recomp` CLI → `output/*.cpp` |

**NEVER**: edit `runner/*.cpp`, write inline assembly hacks, or bypass the architecture. If none of the 4 tools fit, STOP and ask the user.

### 3.2 The Decision Flowchart

```
PROBLEM ENCOUNTERED
│
├─ BUILD ERROR (compilation/link fails)
│  ├─ Error is in runner/*.cpp?
│  │  ├─ Unhandled opcode → TOML patch (nop the instruction) or re-run recompiler
│  │  ├─ Missing symbol → Add stub in TOML, or implement in Runtime C++
│  │  └─ NEVER edit the runner file directly
│  ├─ Error is in src/lib/*.cpp?
│  │  └─ Fix in Runtime C++ (this is YOUR code)
│  └─ Linker error (undefined reference)?
│     ├─ It's a PS2 SDK function → Stub in TOML or implement in Runtime C++
│     └─ It's a Windows API → Fix includes/libs in CMake
│
├─ RUNTIME CRASH (exe crashes during execution)
│  ├─ Read the crash address/PC
│  ├─ Is the address inside the recompiled ELF range?
│  │  ├─ YES → Recompiled game code hit something unhandled
│  │  │  ├─ Unimplemented syscall → Implement in Runtime C++ (src/lib/)
│  │  │  ├─ Calls a stub that returns wrong value → Change TOML stub type or write Game Override
│  │  │  ├─ Hardware register access → Implement in Runtime C++ (GS/DMA/SPU layer)
│  │  │  └─ Infinite loop / setup code → TOML skip or patch
│  │  └─ NO → Address is OUTSIDE the recompiled range
│  │     ├─ It's a secondary binary → Recompile that binary (Phase 1 again)
│  │     ├─ It's a PS2 BIOS call → Runtime C++ syscall handler
│  │     └─ It's a wild pointer → Investigate the CALLER, not the target
│  └─ No crash address? (hang, infinite loop)
│     ├─ Attach debugger or add trace logging in Runtime C++
│     └─ Identify the loop → TOML skip/patch or Game Override
│
├─ WRONG BEHAVIOR (no crash, but game does wrong thing)
│  ├─ Graphics wrong → Runtime C++ GS implementation
│  ├─ Audio wrong → Runtime C++ SPU implementation
│  ├─ File not found → Runtime C++ file I/O path mapping
│  ├─ Game logic wrong → Game Override for the specific function
│  └─ Performance issue → Profile first, then optimize Runtime C++ (see 17-performance-optimization.md)
│
└─ UNKNOWN / CAN'T DIAGNOSE
   ├─ DON'T GUESS. Add trace logging to narrow the subsystem.
   ├─ Use Ghidra to understand what the original MIPS code was doing.
   └─ Ask the user for guidance.
```

### 3.3 Root Cause Protocol — 5 Questions Before Writing Code

Before writing ANY fix, answer these 5 questions **in order**. If you can't answer one, STOP — you need more information.

1. **WHAT failed?** (exact error, crash address, symptom)
2. **WHERE in the architecture?** (runner code? runtime layer? OS interface? game logic?)
3. **WHY did it fail?** (missing implementation? wrong assumption? unhandled case?)
4. **WHICH tool fixes this?** (TOML / Runtime C++ / Game Override / Recompiler — exactly ONE)
5. **WHAT could break?** (your fix affects what other systems? regression risk?)

If your answer to question 4 is "edit the runner .cpp" → **your answer to question 2 is WRONG.** Go back.

### 3.4 Red Flags — You're in the Wrong Layer

If you catch yourself doing any of these, STOP IMMEDIATELY:

| 🚩 Red Flag | Why it's wrong | Correct approach |
|-------------|----------------|------------------|
| Opening `runner/out_*.cpp` to edit it | Runner code is auto-generated. Your edit will be overwritten. | Fix via TOML stub, Runtime C++, or Game Override |
| Writing `#ifdef` in runner code | You're trying to conditionalize generated code | Write a Game Override that replaces the function entirely |
| Copy-pasting MIPS disassembly into C++ | You're reimplementing what the recompiler already did | Understand WHY the recompiled version doesn't work, fix the ROOT cause |
| Adding `if (address == 0xXXXXXX) return;` in the runtime | You're patching a symptom, not the cause | Use TOML to stub/skip the function, or implement the missing subsystem |
| Creating "adapter" functions between runner calls | You're fighting the calling convention | The recompiler handles calling conventions. If it's wrong, fix the TOML config. |
| Spending >10 minutes on a single crash without a diagnosis | You're guessing, not reasoning | Follow the Decision Flowchart. Use Ghidra for context. Ask the user. |
| Writing an override that calls a recompiled body directly, with post-call fixup or a register sentinel | Back-edge preemption yields unwind through your override — post-call code runs on YIELD, sentinels leak, sp desyncs (G57/G186 class) | Wrap the call in preempt suppression or make it resume-aware — `16-runtime-concurrency-threading.md §8` |
| Treating a dispatcher recovery context (`recover-pc`, stack-scan fallback) as the fault location | Recovery restores PC but not `$sp`; everything after it is manufactured noise in unrelated subsystems | Find the FIRST anomaly; instrument before it, not after |
| Trusting a "deterministic" crash address as a meaningful pointer | Corrupted-slot content can repeat run-to-run by allocation-layout luck (G185→G186: the "stable" address was `&MainScene` read from a smashed ra slot) | Check the value against the ELF code range AND live-HW RAM (A/B) before theorizing about who "wrote the pointer" |

### 3.5 Subsystem Map — Know Your Layers

When a crash involves PS2 hardware, you need to know which Runtime C++ file handles it.
**These are the REAL file names in `ps2xRuntime/src/lib/`:**

| PS2 Subsystem | Address Range / Identifier | Runtime File(s) | Typical Symptoms |
|---------------|----------------------------|-----------------|------------------|
| **EE Core** (main CPU) | Recompiled code range | `ps2_runtime.cpp` + Runner code | Crashes in game logic |
| **GS** (Graphics) | `0x12000000-0x12001FFF` | `ps2_gs_gpu.cpp`, `ps2_gs_rasterizer.cpp` | Black screen, wrong rendering |
| **VU0/VU1** (Vector Units) | Inline in EE code | `ps2_vu1.cpp` + Runner (recompiled) | Wrong geometry, broken transforms |
| **VIF1** (VU Interface) | `0x10003C00-0x10003FFF` | `ps2_vif1_interpreter.cpp` | VU data not arriving, bad geometry |
| **GIF** (GS Interface) | `0x10003000-0x100037FF` | `ps2_gif_arbiter.cpp` | GS commands not reaching renderer |
| **SPU2** (Audio) | IOP side | `ps2_audio.cpp`, `ps2_audio_vag.cpp` | No sound, crashes on audio init |
| **IOP** (I/O Processor) | RPC calls, modules | `ps2_iop.cpp`, `ps2_iop_audio.cpp` | Hang during boot, module load fails |
| **Pad** (Controller) | `0x1F801xxx` | `ps2_pad.cpp` | No input, wrong buttons |
| **Syscalls** | `syscall` instruction | `ps2_syscalls.cpp` | Unimplemented syscall → crash |
| **Stubs** | Stubbed functions | `ps2_stubs.cpp` | Missing SDK function → log + return 0 |
| **Memory** | Kernel calls, TLB | `ps2_memory.cpp` | Segfault, invalid pointer |
| **Game Overrides** | Specific functions per-game | `game_overrides.cpp` | Recompiled function behaves wrong |

### 3.6 Memory & Allocator Coherence Rules

When configuring the recompiler (TOML) or writing stubs/overrides, follow these strict rules to avoid catastrophic memory corruption:

1. **Keep the Allocator Family Coherent:**
   - **Rule:** Never split allocation/deallocation functions between runtime C++ stubs and recompiled MIPS code.
   - **Why:** If `malloc`/`_malloc_r` are stubbed to route to the native host allocator (`PS2Runtime::guestMalloc`), then `free`/`_free_r`, `realloc`/`_realloc_r`, `calloc`/`_calloc_r`, and `memalign`/`_memalign_r` **must also be stubbed to route to the native runtime allocator**.
   - **Hazard:** A "split allocator" (e.g., recompiled MIPS code uses a recompiled `free` on a pointer allocated via a host-runtime `malloc` stub) will silently corrupt the guest heap structure, leading to alignment bugs, random crashes, or severe texture/CLUT/menu corruption.
   - **Audit Checklist:** After any recompiler run (regen) or TOML update, check the TOML file, generated `recomp/*.cpp` stubs, and the symbol map to ensure that *either* all memory management functions route to the runtime, *or* all run as recompiled guest code.

2. **Align Host-side Allocations to PS2 Hardware Expectations:**
   - **Rule:** Ensure host-side stubs returning guest memory addresses adhere to alignment boundaries (e.g. 16-byte alignment).
   - **Why:** MIPS R5900 vector instructions (`lqc2`, `sqc2`, etc.) and DMA channels require strict alignment. Returns from custom allocators or memory initialization must not break alignment, or they will trigger hardware-emulation crashes or graphics corruption.

### 3.7 Silent Process Terminations (ThrowContext / bad_alloc)

When a recompiled runner shuts down silently (clean exit with no crash marker or exception dialog):

1. **Uncaught Exception Hazard:** The game may be hitting a C++ `std::bad_alloc` or other uncaught exception. In games compiled with CodeWarrior/SN systems, a throw goes to `__ThrowHandler` → `std::terminate` → `abort()` → `exit()`.
2. **Diagnosis Protocol:**
   - Check `SetupHeap` configuration. If the heap limit is set below the ELF's `_end`, any dynamic allocation (`operator new` / `malloc`) will fail immediately and throw `bad_alloc` or return null.
   - Walk the saved guest return addresses from the `exit` or `abort` stub call stack (read return addresses on the stack frame) to locate the throwing caller.
   - Do NOT assume a silent exit is a hang/freeze. Look at the stdout logs for syscall execution counts or trace the loop activity.

### 3.8 Host-Side Crash Decoding (x64 exception → guest location)

When the runner dies with a HOST exception (access violation `0xC0000005`, stack overflow
`0xC00000FD`, illegal instruction) instead of a clean guest-side error:

1. **Map the faulting module/file to a guest function.** A crash inside `out_XXXXXXXX.cpp` code —
   the address in the file name IS the guest function address. Open it in the static export
   (`14-static-analysis-navigation.md`), never the generated file itself.
2. **Convert the faulting HOST pointer back to a guest address.** Guest RAM is one flat host
   allocation: `guest_addr = fault_ptr − rdram_base`. Log `rdram_base` once at boot so this
   subtraction is always possible from a crash report. A result outside `0x00000000–0x01FFFFFF`
   (32 MB) means the access wasn't guest RAM at all — a wild HOST pointer (runtime bug), or an
   unmasked guest address (missing `& 0x01FFFFFF` somewhere in a handler you wrote).
3. **Stack overflow (`0xC00000FD`) in recompiled code ≈ infinite CALL loop**, usually a dispatch
   error: an override/stub that doesn't advance `ctx->pc`, or a direct-vs-indirect dispatch
   mismatch (Prohibition #13) re-entering the same function forever. Check the most recent
   override you registered before suspecting the game.
4. **The dispatcher `trace=` tail is NOT a callstack** — it's a recorded-PC window (see
   `13-decisional-brain.md` §5, last anti-pattern). Localize from the actual fault state (`a0`
   null? `sp` sane? `ra` plausible?), not the trace tail.
5. If stdout gives nothing, run once under a debugger (VS JIT / WinDbg) or enable minidumps —
   ONE crash with a real host stack beats ten blind reruns. Kill the process after, as always.

---

## 4. Adversarial Split + Verification-First — Mandatory for Code Changes

You are an LLM. You WILL hallucinate. You WILL confuse similar patterns. You WILL forget things from 50 tool calls ago. Accept this and COMPENSATE with structure:

### The 3 Rules of Epistemic Humility:

1. **Never claim without evidence.**
   - ❌ "This function returns 0" → Did you READ the code? Or are you remembering from 30 tool calls ago?
   - ✅ "Let me verify — `view_file` on the function → yes, line 47 returns 0"

2. **Never assume from pattern.**
   - ❌ "The other syscalls use this pattern, so this one must too"
   - ✅ "Let me check db-syscalls.md for this specific syscall number"

3. **If you're >80% sure without recent verification, you're probably wrong.**
   - High confidence without recent evidence = hallucination risk
   - Re-read the source. Re-read the reference. THEN be confident.

### Adversarial 3-Step Structure:

Before writing ANY C++ fix, override, or stub, you MUST execute:

1. **PROPOSE:** Draft your solution — which address to hook, what C++ logic, which file. State your hypothesis about *why* this fixes the crash.
2. **ATTACK:** Immediately switch stance. Try to destroy your own proposal:
   - Does the address exceed PS2's 32MB RDRAM (0x01FFFFFF)?
   - Are you suppressing a crash that will just cause silent corruption later?
   - Are you modifying a `runner/*.cpp` file instead of the runtime layer?
   - Does this break any previous milestone in the state file?
   - **Did you actually READ the relevant code, or are you assuming from memory?**
   - **Have you loaded the relevant db file for this subsystem?**
3. **EXECUTE:** Only after the attack finds no fatal flaws, output the final code and commands.

### Verification Ladder (EVERY fix must climb this):

1. ✍️ Write the fix
2. 🔨 BUILD it (read full output, verify exit code 0)
3. 🎮 RUN it (read stdout via `command_status`, verify behavior changed)
4. ✅ COMPARE to expected behavior (what SHOULD happen? does it?)
5. 📝 Only THEN update PS2_PROJECT_STATE.md with success/failure

**If you skip any step, your claim of "fixed" is a hallucination.** Build output or it didn't happen.

Skip this ONLY for trivial reads, greps, or state file updates. For **any code modification**, this structure is mandatory.

---

## 5. Circuit Breaker — 3 Strike Rule

If you attempt the same `compile → test → fail → guess → compile` loop **3 times** for the same crash:

1. **STOP.** Do not guess again.
2. Re-read `PS2_PROJECT_STATE.md`.
3. **LOAD the relevant knowledge database** using the Knowledge-Seeking Reflex table below. You MUST do this before strike 3 — never attempt a third fix without loading the reference.
4. Consult `resources/09-ps2tek.md` or use GhydraMCP.
5. **PCSX2 MCP A/B Comparison.** If PCSX2 is available, use `12-pcsx2-mcp-playbook.md` §3 to compare real PS2 state vs. recompiled output. The first register/memory divergence IS the root cause.
6. Search the web for community workarounds.
7. If still stuck: format a specific technical question and **ask the user**.

---

## 6. Knowledge-Seeking Reflex — When to Consult Documentation

You have 230+ KB of PS2 hardware documentation at your disposal. The boot loads pipeline and runtime references (03, 04). Everything else is **on-demand** — but you MUST know WHEN to reach for it.

**Trigger table — if you encounter X, LOAD Y:**

| Encounter | Load | Why |
|-----------|------|-----|
| Unknown syscall number | `db-syscalls.md` | Full syscall table with params |
| Unknown SDK function (sif*, sce*, etc.) | `db-sdk-functions.md` | SDK stub signatures |
| Hardware register address (0x1000xxxx) | `db-registers.md` | Register map by subsystem |
| Memory address confusion | `db-memory-map.md` | EE address space layout |
| Unknown MIPS instruction | `db-isa.md` | R5900 instruction encoding |
| VU0/VU1 instruction | `db-vu-instructions.md` | VU instruction reference |
| GS/DMA/VIF/GIF behavior | `resources/09-ps2tek.md` via `08` | Holy grail hardware doc |
| Need to understand a function (`sub_xxx`) | `14-static-analysis-navigation.md` | Static function DB + indexes (preferred over live Ghidra) |
| Graphics bug (geometry/colour/texture/VU1/GS, no crash) | `15-vu1-gs-debugging.md` | VU1 correctness + GS state checklist + capture A/B |
| Hang / deadlock / freeze / thread starvation (no crash) | `16-runtime-concurrency-threading.md` | Guest-lock model, release-on-wait, ABBA, wake handoff |
| Audio symptom (silence, missing SFX/music, crackle, pitch) | `18-audio-spu2-iop-debugging.md` | Audio-path triage + completion contract |
| Save/memcard hang, pad input dead, "file not found" | `19-memcard-pads-fileio.md` | libmc/libpad/cdvdman contracts |
| Stuck at intro video / cutscene (.PSS, IPU) | `20-fmv-ipu-cutscenes.md` | Skip tiers + 3-leg hang triage |
| Need architecture overview | `db-ps2-architecture.md` | Full PS2 system diagram |
| Need to find the RIGHT file | `db-ps2-index.md` | Master router |
| Need visual diagram | `resources/images/IMAGE_CATALOG.md` | 80 classified images |
| Multi-binary / overlay issue | `db-overlay-patterns.md` | Overlay detection & multi-TOML |
| Runtime crash (need register state) | `12-pcsx2-mcp-playbook.md` | PCSX2 breakpoints, A/B comparison |
| Stuck after 2 failed fixes | `13-decisional-brain.md` | Reasoning loop, anti-patterns |
| Need to compare real PS2 vs recomp | `12-pcsx2-mcp-playbook.md` §3 | A/B comparison workflow |

**The rule**: If you're about to write code that touches PS2 hardware and you haven't loaded the relevant db file THIS SESSION → **STOP and load it first**. Never implement from memory. Always verify against the reference.

**Circuit Breaker integration**: On strike 2 of the 3-strike rule, you **MUST** load the relevant db file before your third attempt. This is not optional.
