# 17d — Hot-Loop, Interpreter & Codegen Laws

> **Generic (game-agnostic).** Laws about optimising a hot interpreter/rasteriser loop on MSVC x64,
> distilled from ~40 measured phases of a PS2 static-recompilation port. Every claim here was PAID
> FOR by a measurement; the DC2 instances that produced them are named so you can grep the fix logs.
>
> Load this AFTER `17a-perf-measurement.md` has named the pole. Companions:
> `17b-perf-levers.md` (hotspot classes), `17e-perf-measurement-traps.md` (instrument traps),
> `17c-perf-gs-pipeline.md` (GS-specific).

---

## §1 THE LIVE-VALUE LAW — the binding resource is register pressure, not instruction count

**In a register-starved hot loop, adding a live value costs more than the work it removes.**
Confirmed SIX times on the same loop, every time against an exact, correct, work-removing change:

| lever | what it removed | result |
|---|---|---|
| G427/G442 | one loop-carried local added | **+2.6…+2.8 ms/f** |
| G479 `qpreg` | two per-pair memory RMWs (Q/P delay counters → locals) | **+0.18**, and it gave back a −0.7 win it was stacked on |
| G479 pc-wrap mask | two compares folded to a mask | **+0.93** |
| G483 `pcreg` | a per-pair `m_state.pc` reload (kept the store) | **+0.47…+0.64** |
| G485 `hazreg` | genuine pointer aliasing on every VF access | **+0.232** (carries an index + two `__m128`) |
| G486 `widedest` | four single-lane stores → one wide store | **+0.597** (the wide form must READ the destination) |

**The mechanism (G483, from the disassembly):** the loop is already full of `[rbp±X]` reloads of
pointers hoisted before it, so a new register-resident value simply displaces another one into a
spill slot. The law is not "locals are expensive"; it is **"this loop's register file is full."**

**Corollaries.**
- The question is never *"does this remove work?"* but *"what does it hold while doing so?"*
- A change that REMOVES a hoisted pointer or a live range is the shape that can win; a change that
  adds one is the shape that has lost six times.
- Re-check the premise before applying it: G489 re-measured the same loop after two phases of call
  removal and found **81 stack refs in 1,420 instructions (5.7%)** — no longer spill-dominated. A
  "spill-bound" verdict expires when the loop's shape changes.

---

## §2 WHAT HAS ACTUALLY WON — three shapes

Everything promoted in this arc falls into exactly three classes.

### 2.1 Remove a CALL BOUNDARY
An MSVC interpreter dispatch function's prologue is far more expensive than its instruction count
suggests. Measured (G487) on a real `execLower`:

```
8 GPR pushes ; sub rsp,0x728 (1,832-byte frame)
TEN movaps spills of xmm6-xmm15
/GS security cookie
~40 instructions each way, before the opcode switch is even reached
```

**≈10 ns ≈ 34 cycles per call**, and prediction matched measurement — which is what makes it a
call-boundary result rather than a code-shape accident. The XMM half matters as much as the
instruction count: **a callee that saves all ten nonvolatile XMMs makes the caller's whole
nonvolatile XMM file dead across the call**, inside a register-starved loop.

**But the price belongs to the CASE, not only to the prologue (G488).** Moving three ops off
`execUpper` measured **~29 ns/call, ~3× the prologue-only prediction**, because 86% of that
population was one case that carried a function-local `static const bool` (a thread-safe-init magic
static, visible as an `_Init_thread_header` call in the map) plus six branchy scalar compares behind
a two-level switch. **Read what is INSIDE the case you are moving.**

### 2.2 Remove a STALL that is genuinely exposed
G485: four 4-byte scalar stores enforcing an invariant, immediately followed by a 16-byte
`_mm_loadu_ps` over the same address. **Narrow stores cannot be store-to-load forwarded into a wider
load**, so the load waits for them to retire to L1 (~12+ cycles). One `_mm_storeu_ps` of the same
constant bytes forwards: **−0.645 ms/f**. First lever in that loop to win by removing a stall rather
than work, and the only class that has beaten §1.

**Bounds on the class (G486):** it generalises only when the wide replacement is **INPUT-FREE**.
G485's win was a store of CONSTANTS — pure removal. Widening a single-lane write is a
load-modify-store: it puts a 16-byte load on the dependency chain and holds the old value live.
Before applying the class, ask **what the wide form must READ**.

**And check the dependency DISTANCE (G489).** A hot sample window does not mean its loads are
exposed. Removing a `m_state.pc` read-back on 90.4% of pairs — exact over 2.5 G pairs, in a window
carrying 24.8% of the profile — measured **−0.008 ms/f**. The value had been stored by the *previous
iteration's own tail*, so it was an L1 store-to-load forward with a whole loop body of independent
work in between; the loop's ILP already hid it.

### 2.1b ⭐⭐⭐ The prologue is set by the COLDEST code in the function (G602)

§2.1 prices the call boundary. **This is where that price comes from**, and it is usually not the
work. MSVC allocates the callee-saved register file — including the ten nonvolatile XMMs — and the
`/GS` cookie for the **whole function**, based on what any path in it needs. So a large,
**never-executed, default-off diagnostic block that formats floats** taxes every call:

| symbol (GS front end) | calls/f | before | after outlining the cold blocks |
|---|---:|---|---|
| vertex kick | 24,569 | 8 pushes + `sub rsp,0x148` + **NINE** `movaps` (xmm6..xmm14) | 3 pushes + `sub rsp,0x60` + **0** |
| packed-descriptor dispatch | 141,000 | 5 pushes + `sub rsp,0x100` + **4** `movaps` | 5 pushes + `sub rsp,0x90` + **0** |
| register write | 55,800 | 7 pushes + `sub rsp,0x130` + 2 `movaps` + **/GS cookie** | 7 pushes + `sub rsp,0x80` + **0**, no cookie |

Measured payoff, on the layer's own thread-local sub-timer: **−0.986 ms/f (−11.78%)**, 25/25 windows
in **both** order blocks, with the decode arithmetic completely unchanged.

- **The fix is `__declspec(noinline)` on the cold block, not a faster hot path.** Source stays
  single-copy, so the two cannot drift.
- **Two cheap co-conspirators, both worth finding with the same `/MAP` read:** a function-local
  `static const bool` in a hot path is an `_Init_thread_epoch` **TLS** compare per call (§2.1's G488
  again — one hot function tested the same accessor **three times** per call); and **any stack
  array** — even `uint8_t buf[8]` — buys the function a `/GS` cookie plus a
  `__security_check_cookie` call on every return. A 16-entry nibble table read straight out of the
  word it came from removed both the array and the cookie.
- **Corollary for the rollback:** see §4 — a `#define` that swaps `__declspec(noinline)` for plain
  `inline` does **not** restore the old shape, because MSVC declines the big ones.

### 2.3 Fuse what the FETCH READS (not what it computes)
G479: a loop's critical chain hung off one load and forked into TWO address computations and THREE
loads (two from the code array, one from a parallel predecode sidecar). Storing the 64-bit
instruction word **ADJACENT** to its 8-byte predecode makes it **one 16-byte record, one index, one
cache line**, and removes the code array from the loop's working set: **−0.5 ms/f, exact.**

---

## §3 INLINE BUDGET IS A CLIFF, NOT A SLOPE (G481)

Four small integer op bodies inlined into a hot interpreter loop: **−0.85 ms/f**.
**Twelve of the same class: +81 ms/f** — a 4× collapse of the thread, measured in-process against
the same binary. The loop crosses an MSVC inline/optimisation budget and the whole enclosing
function is regenerated in a far worse shape (the previously-inlined SIMD block being the obvious
casualty).

- **A body being SMALLER than the call it replaces says nothing about whether it is affordable**, and
  neither does a body of the same class already being affordable. Measure every added kind against
  its own paired arm.
- Inlining any **memory-touching** kind was negative in BOTH scalar and branchless-SSE form — the
  cost is text and XMM pressure, not the staging buffer.
- **REMOVING** two per-iteration compares by folding a dispatch chain into one switch also lost the
  whole win: a switch whose default arm is a call re-shapes a loop the compiler had already
  scheduled around predictable compares.

**Share an inlined body's TOKENS, don't transcribe it.** When the same operation must run both
inline in the hot loop and inside an out-of-line fallback, define the body ONCE as a macro in its
own `.inc` and expand it at both sites. The two paths then cannot drift by construction — and back
it with a shadow oracle that runs the out-of-line body from the identical pre-state and compares the
whole architectural state anyway.

### 3.1 The budget is the ENCLOSING FUNCTION's text — dead copies inside it are NOT free (G490)

The cliff in §3 is usually described as "don't inline too many bodies." It is broader than that: the
resource is the **total text of the function that contains the hot loop**, and code that can never
execute counts against it.

A phase built its lever as a new preprocessor COPY of the loop body (§6.4's technique) plus a second
copy carrying a per-iteration oracle. Both copies live inside the same `run()`; a given process
selects exactly one. The oracle copy is unreachable on any default run — its selecting flag is off.
Measured:

| binary | enclosing function | the SAME lever, paired A/B |
|---|---|---|
| candidate + oracle copies present | 113,580 B | **−0.560 ms/f** (mean of 4) |
| oracle copy compiled out (`#define … 0`) | **107,384 B** | **−1.913 ms/f** (mean of 2) |

**6,196 bytes that nothing ever branches to were worth 1.35 ms/f — two thirds of the lever.** Nothing
else changed; the candidate copy is source-identical across the two builds.

- **Refuting a lever is not the only reason to compile one out (§6.6). Promoting one is too**: after
  the arm wins, delete every copy the default path cannot select, keep a single `#define` to
  re-instantiate, and re-take the A/B — the lever you promote may be much bigger than the one you
  measured.
- Corollary for oracles: a per-iteration verifier that needs its own loop copy should be
  **compile-time gated**, not merely flag-gated. Run it during bring-up, record the pair count and
  `bad=0`, then compile it out.
- This also bounds §6.5: a code-SHAPE lever's size is good to a factor of ~3, and *this* is one of the
  mechanisms behind that spread. Quote the `/MAP` size of the enclosing function next to every
  shape-lever measurement so the two are comparable later.

---

## §4 `__forceinline` IS A REQUEST — READ THE LINKER `/MAP` (G480)

`g422FastLower` was declared `static __forceinline` and **MSVC declined it** (33 switch kinds),
emitting a real out-of-line body with its own unwind and `$chain$` records. An entire phase family's
founding premise — *"NOP → the call disappears entirely"* — was therefore false for years: ~554 k
five-argument calls per frame into a body that is `case NOP: return;`. Hoisting that one comparison
into the loop was **−0.440 ms/f**.

**Inlining is a property of ONE LINK.** G487 found that a later binary contained **zero** calls to
five bodies that had all been out-of-line before, with no source change at the call sites. Re-read
the map before building anything whose premise is "this is (not) a call".

### 4.1 The `/MAP` + raw-PC + capstone method (G480/G483, refines G471/G478)

A release PDB with public symbols only makes `SymFromAddr` attribute every anonymous-namespace
static to the preceding public symbol, and `SymGetLineFromAddr64` returns nothing. G478 concluded
per-callsite counters were the only route. **They are not.**

1. **Relink with `/MAP` through the `LINK` environment variable** — `$env:LINK = '/MAP:…\out.map'`
   before `cmake --build`. No project-file edit, no codegen change. The map names statics exactly
   (`?g422FastLower@?A0xb7488d15@@`).
   *Trap:* `cmake --build` on an up-to-date target does **not relink**, so the map is silently not
   produced while the build reports success. Force a recompile (touch a source file).
2. **Sample the SAME binary the map describes**, printing raw PCs in **preferred-base form**
   (`rip - runtimeBase + 0x140000000`), directly comparable to the map.
3. **Disassemble the exact bytes out of the exe** (capstone over the PE section mapping) for the
   symbol's extent, taken as map-address → next map-address.
4. **Locate the hot loop as the span of a backward branch** whose target the samples cluster around,
   then overlay the sample counts onto the instructions.

This is minutes of work against hours of counter plumbing, and it answers questions a profiler
cannot: how many `[rbp+X]` reloads the loop carries, which calls survive, how large the function is.

*Trap (G487):* `Select-Object -First N` over a `/MAP` grep truncates real symbols behind hundreds of
template instantiations, which reads as "the symbol is absent". **Filter, then cap.**

---

## §5 MEASURE THE LOOP'S COMPOSITION BEFORE PICKING A LEVER (G489)

Overlay the PC samples onto the disassembly and classify the loop's own instructions. On a mature
VU1 interpreter pair loop (1,420 instructions, ~63 cycles / ~75 retired instructions per iteration):

| region | share of thread samples | what it is |
|---|---|---|
| loop HEAD | **32.1%** | bounds test, index math, the fetch load-use chain |
| loop TAIL | **24.8%** | PC writeback, branch fold, loop condition |
| **envelope total** | **54.7%** | *not the semantics being interpreted* |
| bit-extraction | **20.7%** of samples, 343/1,420 insns | decoding fields out of words that never change for a given PC |
| all surviving calls but one | 0.64% | the dispatch-coverage vein really is closed |

**When >50% of a hot interpreter loop is envelope, the remaining wins are architectural, not
incremental.** A block translator pays the envelope once per BLOCK instead of once per iteration and
constant-folds the field extraction at translation time. Micro-levers cannot recover a 1.5×.

**The arithmetic that decides an arc:** if the last N promoted levers are all ≤1 ms and the target
needs 35%, more of the same will not get there. Say so instead of scheduling another 1% lever.

### 5.1 Collect the envelope with a RUN EXECUTOR before writing a translator (G490)

The §5 conclusion — "architectural, not incremental" — is right, but "architectural" does not have to
mean emitting native code. The envelope is per-ITERATION cost, so anything that executes **N
iterations per trip through the envelope** collects it, and the cheapest such thing is an ordinary
loop in an out-of-line function. Measured on the same interpreter: **77.0% of executed iterations ran
in runs of mean length 7.81, worth −1.91 ms/f (sign 6/6) and ≈−5.5 ms/f of the worker's busy time** —
with no code emission, no new invalidation machinery, and no new exactness surface beyond one
eligibility predicate.

Build it this way:

- **Eligibility is a per-slot property of the predecode cache, computed once**, exactly like any other
  cached decode (§6.1). Admit an iteration only when every piece of per-iteration machinery the loop
  performs *around* the real work is provably inert for it — enumerate those blocks from the loop
  source and turn each one's guard into an exclusion. What remains must be a short, fixed statement
  list you can write out and read back against the loop.
- **Store a maximal RUN LENGTH per slot**, rebuilt at the same two sites that invalidate the decode
  cache, so it can never describe stale code.
- **Fail closed** on anything that makes the next position non-derivable (a pending branch-delay slot,
  a latched end bit) and **truncate at the budget** rather than stepping past an architectural
  suspension edge.
- **Make the run body `noinline` and OUTSIDE the loop's enclosing function** (§3/§3.1) — a second
  inlined copy of the work bodies would be paid by every iteration to help a subset. Put the hook at
  the TOP of the iteration where no iteration-local value is live, so the call boundary costs argument
  setup only.
- **Go WIDE, not narrow.** An earlier attempt at the same idea covered only the iterations with *no*
  work in one slot — i.e. exactly the population where the envelope is the smallest fraction of the
  iteration. Cover every class the fast path already executes; that is also the class a translator
  would emit, so the number you get is the arc's real premise rather than its easiest corner.
- **Oracle the PREDICATE, not the arithmetic.** The op semantics are already covered by the existing
  per-op shadow oracles; what is new is the elision. Re-derive every excluded condition from the LIVE
  words for every iteration of every run, plus a re-read of the cached source so a missed invalidation
  inside a run cannot hide. Then compile the oracle's copy out (§3.1).

**And re-take the thread ranking immediately afterwards** — a lever this size is exactly the kind that
moves the pole out from under the arc that motivated it (`17a` §2 law 3, and the worked case in
`17e`).

### 5.2 KNOW WHEN THE RUN EXECUTOR IS DONE — price ONE ENTRY, then stop extending coverage (G502)

A run executor is a vein with a floor, and the floor is reachable in about four phases. Two numbers
end it, and both are cheap:

**(a) What does ONE trip round the enclosing loop's head cost?** Measure it with an inflation probe
(`17e` §3.4a): shorten every run so the same iterations execute in the same order with the same
coverage and only the number of entries changes, then divide the frame delta by the extra entries.
On one interpreter this was **1.89 ns — about seven host cycles for the whole loop head plus a
noinline 10-argument call**. Multiply by entries/frame and you have the vein's total size: 158 k ×
1.89 = **0.30 ms/f for deleting every entry in the frame**. That one number retires chaining, trace
formation, cheaper argument passing and minimum-run tuning at once.

**(b) A RUN DOES NOT SAVE THE LOOP ITERATION IT IS ENTERED FROM.** This is the law that decides every
coverage question, and it is easy to get backwards. Moving L iterations into an **existing** run is
free and saves `L × envelope`. Moving them into a **new** run saves `L × envelope` and costs one
entry — *and the entry is still one loop iteration*. So a new run of length L is worth
`L × envelope − entry`, which for the short runs that remain after the easy coverage is taken is
**≤ 0**.

The trap is pre-pricing (b) from (a)'s successful predecessor. One arc extended its runs to swallow
the control transfer that terminates them and measured **7.0 ns/iteration** — a huge win — then
pre-priced "admit a lone control transfer as a run of its own" from that same 7.0 and expected
−0.35 ms/f. It measured **+0.121 then +0.082 ms/f, slower 4/4 on each of two links**, while doing
exactly what it was designed to do (coverage 89.00% → 93.31%, ~70 k iterations/frame moved, control
flow provably identical). The 7.0 ns had been measured on iterations joining runs that were *already
being entered*; it never contained an entry. Same law as "an average marginal rate does not price a
selected population", firing in the pessimistic direction.

**Practical rule: once coverage is ~85–90%, stop. Coverage is not the vein.** The remaining
iterations are the ones whose runs are too short to pay for an entry, by construction. Move to the
per-iteration BODY.

---

## §6 DESIGNING AN A/B ARM THAT CANNOT BIAS ITSELF

### 6.1 Never select a cache-decoded property with an in-loop compare (G487)
When a lever's decision is baked into a predecode cache, selecting the arm **inside the hot loop**
puts an extra compare on the **CONTROL** arm — biasing the gate toward the candidate, the one
direction that must never be accepted. Instead:

- record the arm **in the cache struct**;
- sample it in exactly **one place, once per entry** to the hot function;
- force a **full re-decode** on a flip, before a single iteration of that run executes;
- have every other decode site read it back **from the cache**, so a mid-block write cannot mix two
  decodes.

The loop body then stays **byte-identical in both arms**.

### 6.2 Build the control arm from a SHADOW value (G488)
A free way to get a control arm with no in-loop test at all: decode the encoding to an index whose
descriptor entry is **zero**, so the existing `cls == 0` fall-through IS the control arm. Pick shadow
values the real decoder provably never produces.
A second free variant (G489): when the arm is the **value of one cache bit**, the control arm simply
sets the bit on every item — reproducing the pre-phase behaviour with identical loop text and no
shadow encoding at all.

### 6.3 A cache-property gate is STRUCTURALLY BLIND to added TEXT (G488)
Its virtue — both arms share the loop text — is also its limit: it prices the removed work and
**cannot see the added inlined footprint**, which is the resource §3's cliff binds on. **Pair every
such gate with a `/MAP` check** of the enclosing function's size and its call list.

### 6.4 Absence is a compile-time property of a loop COPY (G485, overturns an earlier "unmeasurable")
A lever whose premise is that a NAME is textually absent (so the aliasing it forces disappears)
cannot be tested with a runtime flag — the declarations still exist. But it is not unmeasurable: an
`#if`-guarded copy of the loop body puts **both arms in ONE binary**, selected by a bool read once
per entry. Before writing a vein off as unmeasurable, ask whether its premise is a compile-time
property that a copy can isolate.

#### 6.4a Once there are COPIES, the selector chain becomes a defect surface (G490)
The copy technique in §6.4 works, and it accumulates: one interpreter reached **eight** copies of one
loop body picked by an `if/else if` chain of run-scope predicates. Two failure modes follow, and
between them they cost that arc **four phases**:

1. **An arm below an always-true predicate can never be selected.** A run-executor lever's two arms
   sat below `else if (fixedLoop && leanFetch)`. `leanFetch` was default-OFF during that lever's
   bring-up and was **promoted default-ON two phases later** — silently killing the lever's arms.
   Arming it thereafter did not put the candidate in the promoted loop; it dropped the whole run onto
   a *different, older, slower* copy and measured that. The result — **"+0.34 ms/f, a run-executor
   LOSES"** — was published, entered the NO-GO table, and was made the gate on an entire architectural
   arc. Rebuilt correctly on the promoted body, the same mechanism was worth **−1.91 ms/f** and moved
   the pole off that thread. Two earlier levers in the same tree died the same way (their flags were
   conjuncts of the predicate selecting the fast copy).
2. **Every copy you leave behind is charged to the enclosing function** — §3.1.

**Protocol whenever a copy chain exists:**
- Before pricing ANY lever in the chain, **re-read the chain top to bottom** and confirm the new arm
  sits ABOVE every arm whose predicate does not exclude it. Simplest safe rule: **put the new arm
  first**, and make the promoted default the first arm after it.
- Make proof-of-selection (§7, G457) **mandatory in both arms of every paired run**, not just at
  bring-up. This is the only cheap check that catches a chain that has silently reordered under you.
- **Re-verify every existing opt-in lever's reachability whenever you promote a predicate**, because
  promoting one arm's condition to default-true is exactly what disables the arms below it. Pair this
  with the "re-price un-promoted opt-in flags after a pole flip" rule in `17a` §2 law 7d — the two
  together are why an opt-in lever's stale verdict is worth so little.

### 6.5 A code-SHAPE lever's SIZE is only good to a factor of ~3 (G436, re-measured G479)
The same lever read **−0.66 / −0.91 / −0.17 ms/f** on three successive relinks of an otherwise
identical source tree, because an in-binary selector re-lays-out the loop on every link.
**Trust the SIGN across binaries; quote the MEAN, never the best run.**

### 6.6 Refuting a lever: compile it OUT, don't ship it default-off (G489)
A null lever left in the source default-off still costs the default path whatever text and
always-taken tests it added — and §3 says text is what the budget binds on. Compile it out behind a
`#define` and keep the decode, the oracle and the arm, so ONE edit re-instantiates it. Record the
refutation **at the site**, not only in the fix log.

---

## §7 PROVING EXACTNESS

- **Shadow oracle**: run the fast body, snapshot everything it can write, restore the pre-state, run
  the reference body from the identical pre-state, compare the whole architectural state (registers,
  flags, and any memory the op can touch). Make it generic over op kinds so new kinds are covered
  for free. Quote the count: *"2.5 G pairs, `bad=0`"* is evidence; *"looks right"* is not.
- **Abort, don't log**, when a violated predicate would silently produce wrong output (a dropped
  branch, a mis-executed op). No route matrix can be trusted to catch that class.
- **A verifier must sit OUTSIDE a loop-COPY guard (G489).** If the verifier's own flag is a conjunct
  of the predicate that selects the fast copy, arming it drops the run onto a *different* copy — and
  a verifier written inside `#if <fast copy>` then silently never executes while its "armed" line
  still prints.
- **Prove the arm was live (G457).** Print a one-shot proof-of-selection line per arm. A paired run
  must show BOTH arms were really built/taken.
- **Derive predicates from the SOURCE, exhaustively.** G489's branch predicate was safe because every
  site that writes the PC was enumerated from the code (ten opcodes in two executors), not inferred
  from a profile.

---

## §8 DATA PLACEMENT AND ADDRESSING

- **`alignas(64)` on the hot state struct.** A struct with `alignof(4)` that is accessed with 16-byte
  SIMD lands wherever its owner puts it — measured at `base % 64 == 52`, so every vector access was
  misaligned, one register in four straddled a cache line, and a line-splitting store **cannot**
  store-to-load forward. Fixing the alignment alone was **−3.48 ms/f**. Print where hot data actually
  lives before theorising about it.
- **Runtime `div` by a power of two is not free.** When a dimension arrives as a runtime out-param of
  a `switch`, the compiler must emit a real `div` even though every value is a power of two. An exact
  shift-table form saved ~1.0–1.5 ms/f. Keep the shift table and the dimension table updated in the
  SAME edit, and guard with a verify flag.
- **Consolidating hoisted `thread_local` pointers into one struct is NOT automatically a win.**
  Merging nine TLS objects into one base + constant displacements changed the loop by **ONE
  instruction and ONE reload**: TLS pointers are already commoned or rematerialised and cost nothing
  to hold. Measure what the spill slots actually contain before merging anything.
- **Two allocation sources on ONE heap CONTEND.** A pool measured at +0.065 ms/f (neutral) read
  **−0.333 (5/5)** unchanged, once a second pool had taken ~1,950 buffers/frame off the same heap.
  When a pool measures neutral, check whether another source is still feeding the same heap before
  writing the vein off — and re-price it after the first one lands.
- **A pool cannot recycle a buffer whose OWNERSHIP transfers.** If the hot buffers are freed by a
  different thread than allocates them, the fix is an ownership change, not a pool.

---

## §9 EXACT SIMD RECIPES (portable, SSE2 baseline)

- **Signed `/255` for blending**: split positive/negative byte differences, multiply unsigned 16-bit
  magnitudes, apply `(x + 1 + (x >> 8)) >> 8`, then add/subtract. Preserves C++ truncation toward
  zero across the full 8-bit domain and avoids scalar division.
- **Interleaving two small bit-masks** has no cheap SSE2 form (no PDEP at this baseline) — one small
  `constexpr` lookup table is the right answer.
- **Hoist a swizzle ROW, then widen the STORE.** A per-pixel VRAM writer re-derives page, block,
  row and a 3-D subscript on every pixel and the compiler will not hoist any of it because `x + i`
  feeds the subscript. For a horizontal run at fixed `y`, the table row is CONSTANT and the page
  index is constant per page-aligned segment. Then read the column table to find the store width:
  one layout put consecutive x in adjacent PAIRS (four 8-byte stores at a 16-byte stride), another
  put x, x+8, x+16, x+24 in four CONSECUTIVE BYTES (one 32-bit masked RMW for four pixels).
  **Verify every layout claim from the table itself, once per `(block, row)` — never assume it** —
  and fall back through a data-driven pair loop to the exact single-pixel form.
  Result: 2,873 → 6,344 MiB/s and 189 → 377 MiB/s, −0.983 ms/f.
- **A flat first attempt is a claim about YOUR loop, not the machine.** The first quad-gather read
  only +9% and looked memory-bound; it was iterating all 128 columns to reach 32 anchors. Iterating
  the anchors directly took it to 349 MiB/s. **Read your own iteration count first.**

---

## §9a ⭐⭐⭐ THREE COSTS THAT ARE INVISIBLE IN THE SOURCE AND OWN THE LOOP ANYWAY

Each of these was found by a deletion/inlining probe after the arithmetic had already been ruled
out. None is visible by reading the loop body.

### 9a.1 A function-local `static` is a PER-ACCESS THREAD-SAFE-INIT GUARD, not a constant

`inline bool f() { static const bool v = getenv("X") != nullptr; return v; }` is the standard way to
cache a flag — and it is correct. But **every call re-checks the initialised-yet guard** (MSVC: a
TLS-indexed epoch load + compare + conditional branch). That is nothing at a per-draw site and it is
enormous at a per-pixel one.

One port's bilinear filter called three such accessors per channel: **twelve guard checks per
texel**, in the hottest leaf on the route. Inlining the body — arithmetic completely unchanged, the
identical statements — cut the whole pixel by a third.

- **Rule: read every such flag ONCE per draw/batch into a plain `const bool` hoisted above the loop
  nest, and never call one inside the loop.**
- A "default-off diagnostic" that reads its flag per iteration is the same defect wearing a
  disguise: the flag is off, the *check* is not free.
- Grep the hot leaf for `static const` / `static bool` / any `…On()` accessor before profiling it.

### 9a.2 A CALL BOUNDARY is a VEHICLE, not a price — inline the leaf, then re-measure

When a per-item stage lives in another translation unit, its cost is not "a call": it is everything
the boundary *forces* — the callee re-deriving arguments the caller already had, no hoisting of
loop-invariants across the boundary, and a register spill/reload at the seam. The same stage
implemented in the same TU can cost a fraction of it.

⭐⭐⭐ The consequence for RANKING: **a stage's share is a property of the LEAF THAT IMPLEMENTS IT and
of its hit rate, not of the stage.** One port measured the *same two stages* on two populations:

| stage | population A | population B | why |
|---|---:|---:|---|
| bilinear filter | **50.1%** | 40.1% | — |
| the four texture taps | **8.1%** | **35.7%** (4.4×) | B's tap leaves the TU through a cross-module reader; A's is inlined. And B's tap-quad memo hit rate is **zero** |

A had correctly optimised the filter and left the taps alone. Carrying that ranking to B would have
missed half the prize. **Re-run the stage decomposition per population; never carry another
population's ranking across.**

### 9a.3 A MEMO's HIT RATE is a LEVER, not a constant

`taps/px = 3.999` against a theoretical 4.0 looks like a null result — "the memo never hits, delete
it". It was the opposite: it *named* the fix. The memo was keyed on the whole quad, and the quad
never repeated; but when the source coordinate advances ~one unit per output unit, **this column's
LEFT pair is the previous column's RIGHT pair**. Re-keying on that halved the tap count at a 98.6%
hit rate.

**A near-zero hit rate is a question ("why does the key never repeat?"), not an answer.** The
re-keying is exact by construction — it reuses a value produced by the same function of the same
arguments — so it needs no numeric tolerance.

---

## §9b ⭐⭐⭐ A LOWERING IS EXACT BY CONSTRUCTION — AND THAT IS A DESIGN RULE, NOT A CLAIM

The safest large win available in a hot nest is not a better algorithm: it is **moving each
computation to the outermost loop level at which its inputs are constant**, evaluating the
*identical expression* there.

| level | what belongs there |
|---|---|
| per DRAW / batch | anything depending only on state: tables, gates as intervals, blend selectors, base addresses, every flag (§9a.1) |
| per ROW | anything depending only on the row index: row bases, the row's source coordinates, the vertical weight |
| per ITEM | only what genuinely varies: the taps, the filter, the commit |

Why it is *exact*: the expression is not re-derived, re-associated, or approximated — it is the same
source expression evaluated fewer times. **No rounding identity is assumed and no reciprocal is
substituted for a divide**, so there is no numeric argument to make. Contrast with per-item
interpolation or a DDA rewrite, which are *not* exact and are a different (much riskier) lever.

⚠️ The corollary that makes this cheap: because a lowering changes only *how often*, its oracle is
free — the existing per-item oracle already covers every item the lowered path produces.

⛔⛔ **DO NOT RANK AN INTERPRETER'S STAGES BY DELETING THEM.** The deletion-ranking method (`17e
§3.3a`) is for a *data-plane* loop, where a deleted stage changes only outputs. In an interpreter the
deleted slot writes the guest program's own **loop induction variables**: one port's "delete the
lower op" arm took work per call from **1,379 to 61,242 pairs (44×)**, and its "delete both" arm
deadlocked two worker threads. Price an interpreter lever by BUILDING a slice and gating it, with a
workload invariant (`pairs/call`) printed to prove the two arms did the same work.

## §10 CHECKLIST BEFORE BUILDING ANY HOT-LOOP LEVER

1. Has the pole been named by **occupancy** this session? (`17a`, `17e §1`)
2. Have you read the **`/MAP`** for the binary in hand — is the thing you assume is a call still a
   call? (§4)
3. Have you **priced the site's call/visit rate** with a counter? (`17e §3`)
4. Does the lever **add a live value**? If yes, expect to lose. (§1)
5. If it widens a store, is the wide form **input-free**? (§2.2)
6. If it removes a load, what is the **dependency distance**? (§2.2)
7. Is the arm a **cache property** with byte-identical loop text — and is it paired with a `/MAP`
   size check? (§6)
8. Is there a **shadow oracle** with a quoted count, and a **proof-of-selection** line? (§7)
9. Four paired runs minimum; quote **mean and sign count**, never the best run. (§6.5)

---

## §11 ⭐⭐⭐ WHEN N DIFFERENT IMPLEMENTATIONS COST THE SAME, THE COST IS THE ADMISSION TEST

The single highest-value diagnostic in a long specialization arc, and it is free — you already have
the numbers.

A port built three radically different exact executors for the same hot VU block:

| candidate | shape | result |
|---|---|---|
| generic 36-way unroll | large per-pair expansion, ~5.8 KiB | **+5.88 ms/f (+14.9%)** |
| compact interpreter | per-pair table, small text | **+5.6 ms/f** |
| …with full-vector loads/stores (a real codegen defect fixed) | — | **+5.5 ms/f** |
| narrow 3-segment native image | 4.5 KiB, **22% smaller than the first**, no dispatch table | **+5.5 ms/f** |

Three implementations with *nothing in common* — different text size, different dispatch shape,
different memory instructions — all landed within **0.4 ms of each other**. Arithmetic cannot be the
cost: the bodies do not resemble each other. **The invariant across all four arms is the extra
`is this the hot block?` test that every arm added to the block dispatcher, and that test runs on
EVERY block, including the ~99% that are not the target.**

**The law.** If candidate A and candidate B differ structurally and measure within noise of each
other, stop optimizing the body. Price the **admission conjunct** instead:
- count how many times the test runs vs how many times it *succeeds* (a 1:100 hit rate means you
  pay 100× the test to save 1× the body);
- build the arm that adds the test **and nothing else** (a no-op handler behind the same predicate)
  and measure that alone — this is an inflation probe (`17e §3.4a`) on the selector;
- only then decide whether a zero-extra-test dispatch (folding the decision into an existing
  switch, or a precomputed per-block kind byte) is available. If it is not, the whole class is dead.

Corroborating evidence from the same port: a separate one-byte static-fragment **selector** — no
handler at all — measured **+0.37 ms/f raw / +0.49 blocked** and was removed from the build. The
selector alone was a measurable loss. Four specialization attempts died to the same tax.

⚠️ The trap this closes: each individual refutation *looks* like "that particular implementation was
bad, try a better one", which licenses an unbounded sequence of rewrites. The identical-penalty
pattern is what proves the next rewrite is also dead.
