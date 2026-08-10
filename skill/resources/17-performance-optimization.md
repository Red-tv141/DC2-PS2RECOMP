# Reference: Performance Optimization — Correct First, Fast Second

> **Load this when the game runs CORRECTLY but too slowly** — low FPS, stutter, a headless test
> window that only reaches a few hundred frames in 30 s, input scripts that never fire because the
> target frame is never reached. Do NOT load this while a correctness bug is open: optimizing a
> wrong pipeline wastes the work and destroys your A/B baselines.
>
> **This is the ENTRY file: doctrine + a router.** The substance lives in five siblings —
> `17a-perf-measurement.md`, `17b-perf-levers.md`, `17c-perf-gs-pipeline.md`,
> `17d-hot-loop-and-codegen-laws.md`, `17e-perf-measurement-traps.md`. Load one or two of those
> per §2 below; never all of them.

---

## §1 Doctrine — The Five Laws

0. ⭐⭐⭐ **A LEVER THAT CHANGES *WHO OWNS* PIXELS MUST BE GATED ON SCENES WHERE THE GUEST *WRITES*
   PIXELS.** Residency, ownership, page-authority and cache-admission levers do not change *how
   fast* a frame draws, they change *which writer wins* — so they only misbehave where two writers
   race, i.e. under **player-controlled motion, jumps, dashes and animation transitions**. A gate
   set of scripted-camera cutscenes, static screens and golden title frames **cannot see that
   class at all**.
   *Real cost:* a phase promoted a residency widening past four green gates (golden title exact,
   sky-defect detector 0 defects, two dungeon/town routes pixel-identical) and shipped a defect the
   user hit **every time the player jumped**. Every gate passed; none contained a jump.
   → **Add one player-motion route to the gate set of any ownership lever, and require the user to
   drive it if it cannot be scripted.**
0a. ⭐⭐⭐ **AN OWNERSHIP-INVARIANT COUNTER IS A CORRECTNESS GATE THAT MUST READ ZERO — NEVER A COST
   TO TRADE AGAINST ms/f.** In the same phase the runtime printed 2,505 `escaped writer` lines per
   run (0 with the rollback). They were tabulated as *the mechanism of a +3.4% slowdown* and the
   lever shipped. An "escaped writer" is two writers disagreeing about who owns a page; the
   materialize the port performs afterwards is its **recovery**, not the event. If a counter's name
   contains *invariant*, *escaped*, *conflict*, *stale* or *fallback-for-safety*, a non-zero value
   is a **failed gate**, and its rate is only interesting for locating the bug.
1. **Correctness before speed.** Never optimize a subsystem with an open correctness bug in it.
   An optimization changes timing and code shape — it will smear the evidence you need for the bug.
2. **Measure before touching.** No optimization without a profile or counter proving WHERE the time
   goes. "The rasterizer is probably slow" is a hypothesis, not a diagnosis. The hot spot in a
   recompiled port is frequently NOT where intuition says (it's often logging, dispatch, or a mutex —
   not the math).
3. **Behavior-identical, verified.** After every optimization, re-run the golden baseline
   (e.g. the title-screen pixel metric, a `.gs` capture diff, the phase regression checks). An
   optimization that changes ANY output is a correctness change in disguise — revert or gate it.
4. **Re-profile the current executable.** Every promoted phase changes the cost graph. Treat an
   older phase's "next hotspot" as a stale hypothesis until the current Release binary reproduces
   it. Measure event frequency and exclusive cost, then compute the maximum plausible payoff before
   designing an architectural mechanism.

Every performance change follows the same Verification Ladder as a fix (`10-agent-guardrails.md` §4):
write → build → run → **compare output metric AND time metric** → record in the project state file.

---

## §2 Router — which perf file answers your question

**Read this table before loading anything else.** The files are split by the QUESTION you are
asking, not by subsystem, because at the start of a phase you do not yet know which subsystem is
the pole — and picking a subsystem file on a guess is the single most expensive mistake in perf
work (one real arc spent seven consecutive phases optimizing the wrong thread that way).

| Your question right now | Load |
|---|---|
| "Where does the time actually go? Which thread is the pole? Did my change help?" | **`17a-perf-measurement.md`** — instruments, the premise gate, thread attribution, derivative probes, ceiling probes, powering an A/B. **Always first.** |
| "Measurement named a target — what mechanism do I build, and what will it really cost?" | **`17b-perf-levers.md`** — hotspot classes in a recompiled runtime, hot-loop/interpreter levers, cache & residency design rules, what NOT to do. |
| "Measurement says the GS path is the pole." | **`17c-perf-gs-pipeline.md`** — parallel software rasterizer, GPU LLE, native-renderer admission/aliasing/presentation, each with its correctness contract. |
| "The pole is a hot INTERPRETER / rasteriser loop — what shape of change actually wins?" | **`17d-hot-loop-and-codegen-laws.md`** — the live-value law, the inline-budget cliff, call-boundary pricing, store-forwarding bounds, the `/MAP`+capstone method, and how to design an A/B arm that cannot bias itself. |
| "My measurement contradicts another measurement / a correct lever measured backwards." | **`17e-perf-measurement-traps.md`** — pole naming, censuses that move what they measure, pricing before building, running the A/B, harness hygiene. |
| "Is this a *correctness* bug rather than a slow one?" | Stop. `15-vu1-interpreter-correctness.md` or `15b-gs-state-and-capture-ab.md`. Never optimize around an open correctness bug. |
| "The threads deadlock / starve rather than run slowly." | `16-runtime-concurrency-threading.md`. |

### The 60-second phase opening

1. `17a` §1 — attribute every bucket to its THREAD. The frame is `max(threads)`, not `sum(buckets)`.
2. `17a` §2 law A1–A3 — rank the threads with a **derivative probe** (inject a known busy-spin into
   each candidate worker and read the frame). A ranking from a previous phase is stale evidence.
3. **Cross-check the ranking against OCCUPANCY** (busy ms/f per thread vs the frame) using a census
   light enough that it cannot re-order the threads — `17e` §1.3 and §2.2. When two probes read
   within noise of each other the threads are TIED, and re-taking the probe forever is a coin toss.
4. Only then open `17b` / `17c` (mechanism) or `17d` (hot-loop shape) to choose a lever.

---

## §3 Record It

For every accepted optimization, one line in the project state file's `Learned Patterns`:
`<hotspot> cost <N>% frame time, fixed with <what>, FPS <before> → <after>, baseline metric unchanged`.
If it changed an output metric even slightly: it is NOT an optimization — reclassify as a behavior
change and route it through the normal fix taxonomy.

For every rejected premise, record the current-binary profile, payoff ceiling, mechanism counters,
reverse-order A/B distribution, normal-presentation evidence, prototype disposition, and the focused
next mechanism in the phase fix log. Never summarize an internal counter reduction as a win.
**Diagnostics-only closure is a valid phase result.**

Cross-refs: verification ladder `10-agent-guardrails.md` §4; lever/kill-switch doctrine
`15b-gs-state-and-capture-ab.md`; lock model `16-runtime-concurrency-threading.md`; SIMD note
`04-runtime-syscalls-stubs.md` §6; build gate `SKILL.md` §4.
