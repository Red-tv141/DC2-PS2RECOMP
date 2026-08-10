# Reference: Performance LEVERS — What To Actually Build

> **Load this AFTER `17a-perf-measurement.md` has named the pole thread and the target site.**
> It answers *given a measured target, what mechanism do I build, and what will it really cost?*
> Building from this file without the measurement file is how phases optimize the wrong thread.
> GS-rasterizer/LLE/native-renderer playbooks live in `17c-perf-gs-pipeline.md`.

---

## §1 Known Hotspot Classes in a Recompiled PS2 Runtime

Check these IN ORDER — the cheap wins come first. Confirm each with the profiler before acting.

| # | Hotspot | Symptom / check | Fix direction |
|---|---------|-----------------|---------------|
| 1 | **Leftover diagnostic logging** | `printf`/`fprintf`/`std::cout` in a per-frame, per-draw, per-call path; console I/O shows in profile | Delete or env-gate. Format+flush per call is brutally slow. An uncapped *counter* is fine; a per-hit *printf* is not. **Also: DISABLED probes are not free at scale** — dozens of cached-bool-gated probe blocks accumulated over many debug phases in one hot loop still pay their pc/state compares and branches every iteration. Wrap the whole pile behind ONE master "any diagnostic active this run" flag (probe envs OR'd once at entry) with the legacy interleaved body kept byte-identical as the kill-switch arm; require new probes to register in the master list. DC2 G295: gating ~20 dormant probe regions in the VU1 interpreter loop (+ skipping a redundant save/restore) cut the interpreter ~28% and the frame −12%. |
| 2 | **Per-call `getenv()` in env-gated diagnostics** | An env-gated probe (`if (envFlagEnabled("X_TRACE")) …`) sits in a per-vertex/per-draw/per-tag path WITHOUT a `static const bool` cache. Does NOT show as I/O — the µs-class `getenv` (env lock + linear scan on Windows CRT) hides inside the caller's inclusive time, and worker/replay threads SERIALIZE on the CRT env lock. Check: grep hot files for `getenv`/`envFlagEnabled` calls not feeding a `static const` initializer; census per-callsite ns (a cheap handler at 25 ns vs a sibling at 5,000 ns whose only extra feature is an uncached env check = the tell). | Read once into `static const bool` (magic statics are thread-safe); keep an opt-in lever that restores per-call reads as the same-binary A/B control. Real-world cost: ONE uncached line in a per-vertex kick path cost **41% of the whole frame** and masqueraded for four phases as an architectural "parse/dispatch" bucket (DC2 G268) — profile-bucket names lie; census per-descriptor/per-callsite before designing an architectural fix for a bucket. |
| 3 | **Debug/unoptimized build** | You're not on `Release`; iterators/asserts in profile | Verify `CMAKE_BUILD_TYPE=Release` (Ninja: baked at configure; VS generator: `--config Release`). Never "fix" perf while accidentally profiling Debug. |
| 4 | **Guest memory access macros** | `READ32`/`WRITE32`/`READ128` etc. dominate samples — every guest access masks + bounds-checks + MMIO-routes | Fast-path the common case (plain RDRAM range) before the MMIO check; keep the MMIO route for `0x10000000+`/`0x12000000` only. Behavior-identical by construction — still A/B it. |
| 5 | **Function-pointer dispatch lookup** | The indirect-call resolver (address → handler map) hot in profile | Cache lookups; use a flat table indexed by (addr − code_base)/4 rather than a hash map, if the runtime doesn't already. |
| 6 | **VU1 interpreter inner loop** | `ps2_vu1.cpp` dominates; heavy per-instruction decode | Decode-once/cache per microprogram; keep flag/Q-latency semantics EXACTLY (the correctness rows in `15-vu1-interpreter-correctness.md` §2 are non-negotiable — re-run distinct-lane tests after). **Then inline the operation itself.** Census the dynamic mix first (pairs/run × runs/frame ⇒ ns/pair; a predecoded step much over ~15-30 host cycles means the *step* is the target, not the guest workload). If the hot slots leave the loop through a non-inlined `execUpper`/`execLower` with a wide jump table, execute the dominant families **inline in registers** from a descriptor table indexed by the existing predecode, and fall back for cross-lane/flag-only ops — a families-based table hits ~98% of executions with no cliff. See `17c-perf-gs-pipeline.md` §3 for the bit-exactness rules that make the SIMD form provably identical. (DC2 G421: −8.4% frame, 600 M shadow-verified ops, `bad=0`.) |
| 7 | **GS software rasterizer** | `ps2_gs_rasterizer.cpp` per-pixel loop dominates (usually the #1 cost) | FIRST hoist per-triangle invariants out of the per-pixel path (sampler setup, CLUT decode → memoize per-triangle, swizzle-address base, alpha/blend decode) + scanline-narrow the bbox scan; THEN parallelize across disjoint pixels — see **`17c-perf-gs-pipeline.md` §1** (the biggest lever). Do NOT change rounding/blend/sample semantics (verify vs `.gs` capture + same-run per-pixel A/B). |
| 8 | **Guest-execution lock contention / sleeps** | Cores idle, FPS low, threads ping-ponging | See `16-runtime-concurrency-threading.md` — wrong wait granularity (e.g. a 200 µs sleep in a hot yield) caps FPS. Tune wait sites, keep the release-on-wait rule intact. |
| 9 | **Scalar loops in math-heavy stubs/overrides** | Your own handwritten override shows hot | Vectorize with SSE intrinsics (`04-runtime-syscalls-stubs.md` §6). Test with DISTINCT per-lane values after (`10-agent-guardrails.md` §2.1) — vectorizing is exactly where lane bugs are born. |
| 10 | **Per-call allocations / copies in handlers** | `malloc`/`memcpy` hot inside a stub called per frame | Preallocate/reuse buffers. Respect allocator-family coherence (§3.6 of `10-agent-guardrails.md`) — never introduce a second allocator path. |

---


---

## §2 Choosing what to build — laws from a long perf arc

> Same source arc as `17a-perf-measurement.md` §2 (DC2 G418–G431); numbering continues
> from it, so law 20 here follows law 19 there.

20. **Measure the CEILING before building the lever.** A byte count is not a time: a "1 MiB"
    per-frame zero-fill was 0.137 ms total, of which 0.089 was elidable — under the instrument's own
    floor, i.e. unpromotable no matter how exact. One 60-second premise run decides this.
21. **A measured per-op price belongs to a RESOURCE, not to "instructions".** ~1 host cycle per
    SSE2 op is the price of a loop's *SIMD dependency chain*; deleting ~4 scalar L1-resident memory
    ops per iteration over 1.26 M iterations/frame — nominally ~1.5 ms at that price — was worth
    **+0.12 ms**, because scalar loads/stores issue on otherwise-idle ports. Ask which port or
    resource the loop actually saturates before extrapolating a price to another instruction class.
22. **Count SIMD→GPR domain crossings, not just SIMD ops.** Removing a ~10-op classifier from a hot
    SIMD path was worth 2.6× what the per-op price predicted, because four `movmskps` per result
    hung a SIMD→scalar→SIMD round trip off it. An op that leaves the vector domain is a *latency*
    coupling, worth more than its op count.
23. **A hot interpreter loop is not automatically misprediction-bound.** VU-style microprograms are
    loops, so their "data-dependent" jump tables are in fact well predicted. An exact fully
    branchless dispatch that added ~20 ops per iteration cost **+8.5%**. Only a lower *dynamic
    instruction count* wins; removing branches at the cost of extra work is a loss.
24. **Prefer the hoisted-pointer idiom to a loop-carried local.** Adding ONE extra live value across
    a register-pressure-sensitive loop body cost **+2.8 ms / +5.5%** even though it removed work.
24a. **Do not add a live local to save MEMORY TRAFFIC in such a loop — including the interpreter's
    own PC.** "Keep the PC in a register" is a textbook interpreter optimisation; built exactly
    (the memory store kept and only the reads moved) and verified pair-by-pair, it cost **+2.56 ms**
    in the same loop, reproducing law 24 to within 10%. The store-to-load forward it removed was
    never *exposed*: the address chain was `pc + 8` on ~90% of iterations, so the out-of-order
    engine had already hidden it, while the extra live value was paid every iteration. Before
    optimising a loop-carried memory round trip, ask whether its next value is PREDICTABLE — if it
    is, the round trip is not on the critical path.
24b. **The price is per-iteration READ FREQUENCY, not the count of locals — the inverse of 24 does
    not hold.** Removing a loop-carried local that only ~10% of iterations touch was frame-neutral.
    Target values read on EVERY iteration (e.g. a pile of loop-invariant lever booleans: pack them
    into one mask, or template-specialise the loop on the default configuration so they fold away).
24c. **Check MXCSR before theorising about float latency, then PRICE it.** A runtime that never sets
    FTZ/DAZ can pay an x86 subnormal microcode assist on every subnormal operand or result, and an
    emulated FPU that has no subnormals at all (PS2 VU/COP1 flush them) makes every one of them pure
    loss. Census first — 5.56% of FMACs carried a subnormal in one arc — then price it with an
    FTZ|DAZ ceiling probe on the pole thread ALONE (one `_mm_setcsr` at thread entry cannot reach
    another thread's arithmetic). There it was worth only **−0.4 ms**: a famous microarchitectural
    penalty is not automatically a penalty on the machine in front of you. Note the exactness catch
    if it ever does pay — hardware flush-to-zero usually still raises the underflow flag, so an
    exact lever must reproduce that separately.
25. **Per-iteration decisions that are pure functions of the instruction encoding belong in the
    decode-time sidecar.** If the decode cache already stores and precisely invalidates the source
    bytes, a predicate resolved there costs one bit test instead of ~1.26 M recomputations/frame.
26. **A sub-floor exact lever is a COMPONENT, not a dead end.** One predicate measured neutral
    alone; bundled with two siblings of the same mechanism the group cleared the noise floor.
27. **A delay line with SPARSE producers is a stamped history, not a shift register.** Ask which
    instructions can actually change the state and how many read it. If both are sparse, record
    `(stamp, value)` at the producer and search `newest stamp <= N - DEPTH` at the consumer:
    `DEPTH+1` entries are provably enough, and the loop's existing iteration counter is a free
    timestamp. Worth −9.3% in one arc where the old line touched every iteration.
28. **Prove a side effect DEAD from the DECODE CACHE, not from a census.** "The census never saw one
    execute" is a probability; "the decode cache, which precisely invalidates the whole code store,
    contains zero readers of this state" is a proof — and it self-disarms the moment new code is
    uploaded. When a lever needs some state to be dead, ask what the decode cache can prove.
29. **A gate that can flip needs a CONVERTER in both directions.** If a lever changes the
    representation of some state, arming must seed the new form and disarming must materialise the
    old one — an `if` is not enough. Related: capture an evicted ring entry at the point the item is
    *fetched*, not where it is *pushed*; the two capture points are not interchangeable.
30. **A per-flush "already did it" set is not a cache, and a whole-object counter cannot detect
    redundancy.** Before making such a set survive its scope, census the redundancy with the
    FINEST-grained invalidation signal available (per-page generations, not a per-object write
    counter — the coarse one reports 100% "necessary" by construction and would give the right
    answer for the wrong reason). One arc's answer was still 0 of 8000 rebuilds redundant.
31. **A delta/incremental replacement of a batched op must be censused for changed-fraction AND
    round-trip count first.** With no temporal coherence, the incremental path does the same work
    while fragmenting one batched job into many synchronous round-trips — exact but slower.
32. **Re-price retained exact substrate after architecture changes.** A verified cache worth only
    ~1–2 ms on one binary became a **13.81 ms / 21.3%** win after later phases changed ownership and
    worker costs. A prior no-go is evidence for THAT binary, not a permanent price — so preserve the
    cold builders and the exact verifiers, and keep dead levers opt-in so they can be re-measured.
33. **Cache derived PLANS, never live renderer truth.** Address maps, coordinate maps and setup may
    be reused only because authority, eligibility, dirty/generation state and publication order
    remain live checks. A cache that swallows those decisions is a different mechanism needing a
    new proof.
34. **Interpreter call boundaries are measurable and can be priced in advance.** One arc measured
    ~8.6 ns per eliminated interpreter call; multiplying by the remaining call count gave the next
    phase's ~7 ms ceiling *before* anything was built.
35. **Size a lever's oracle to the part that is actually NEW.** When a fast path re-issues the same
    leaf calls in the same order, exactness is by construction and only the loop/run decomposition
    is new — verify *that*, by replaying both arms from an identical pre-state and comparing the
    whole destination buffer plus any resumable cursor.
36. **Avoid GPU resource churn**: re-creating attachments on target-dimension changes is pure
    allocation time. Park outgoing FBO geometry by size and revive it. And sizing a thread pool's
    `lanes` splits the work but does not size the wake-up convoy — gate parallel fan-out on total
    PIXELS, not on row count.
37. **SSE2 is bit-exact for per-lane float work under MSVC `/O2 /fp:precise` with no `/arch:AVX`**:
    `addps/subps/mulps` match scalar lane loops exactly and no FMA contraction is possible;
    `_mm_max_ps(a,b)` is `a>b?a:b`, `_mm_min_ps(a,b)` is `a<b?a:b` (matching the legacy ternaries
    operand-for-operand); `_mm_cvttps_epi32` matches MSVC x64's `(int32_t)(float)`. A masked
    bit-select store is value-identical to conditional per-lane stores.

38. **Before pricing what a hot loop EXECUTES, print where its DATA LIVES.** Four consecutive
    phases correctly priced an interpreter loop's instruction mix, footprint, store width, pointer
    aliasing and addressing at ~zero and concluded "the residual is stall". All four were reasoning
    about a register-file struct with **`alignof 4`** — its widest member was `float` and it had no
    `alignas`, so it landed wherever its owner (a member of a stack-local runtime object) put it:
    base **≡ 52 mod 64**. A quarter of the vector registers, and *all* of the accumulator, split a
    64-byte cache line on every access. **A line-splitting store can never store-to-load forward**,
    so every dependent step of a `MULA→MADDA→MADDA→MADD` chain paid a store-buffer drain
    (~15–20 cycles) where an aligned base pays ~5. One `alignas(64)` was worth **7% of the frame**.
39. **`alignof` is not a detail on any struct accessed with `_mm_loadu_ps` / `_mm_storeu_ps`.**
    The unaligned intrinsics make misalignment *correct*, which is exactly why the defect is
    invisible: no crash, no wrong pixel, no failing oracle to find it by — only stall. Never infer
    alignment from the declaration; a struct of `float`s has alignment 4 however vector-shaped it
    looks. Print `reinterpret_cast<uintptr_t>(&x) & 63` once at a hot entry, default-OFF behind an
    env flag, and keep the probe. Sweep **every** SIMD-accessed struct in the tree once; the cost
    is three lines each.
40. **A struct's alignment is a compile-time SHAPE change, so it has no in-binary control arm.**
    A/B it across BINARIES against a saved pre-phase executable (see `17a` on the in-binary-lever
    trap). And if the struct is read by more than one thread's work, the rule "subtract the peer
    thread's stall from this thread's busy time" **stops holding** — it will subtract part of the
    effect. One such lever moved worker-A busy −3.64, worker-B −3.63 and the stall −3.61, so the
    net-of-stall column read 0.00 while the frame moved 3.48. **The FRAME is the result**; the
    thread table only corroborates that idle time did not merely relocate.
41. **The LAST unscoped consumer edge is a standing lever, and refactors create new ones.**
    A long-lived engine converts its dependency edges from "publish everything" to "publish what
    overlaps" one edge at a time, and each conversion is justified by the batch sizes *at the time*.
    An edge left whole-state because "there was one batch per drain anyway" becomes a defect the
    moment an earlier phase multiplies the batch count — and it will not show up as a hot function,
    because the cost is one fixed prologue paid per batch. **After any phase that changes how many
    units flow through a pipeline, re-ask which edges still publish whole-state.** DC2 G528: five of
    seven calls in a flush prologue took no ranges; every *other* edge in the same engine had been
    converted across nine earlier phases; the exact ranges were already computed and attached to the
    batch, and the closure threw them away. Two calls were 99.9% of a 2.9 ms/f bucket.
    ⭐ The cheap tell: grep the codebase for `*ForRanges` / `*ForRect` siblings and find the call
    sites that use the whole-state form.
42. **When a design question has a shipped census, RUN IT before designing — including on a route it
    has never seen.** A census answers in one run what a build answers in an hour, and its verdict on
    a *new population* is a genuinely new result, not a repeat. DC2 G528's obvious lever ("coalesce
    the five tiny batches") was killed by `DC2_G523_MERGE=1` reading `mergeable=0` of 23,530 batches
    on a route that census had never been pointed at — the second refutation of batch coalescing,
    on a different route and a different batch population, for zero build time.
43. **Split the bucket before designing against it, even when the mechanism seems obvious.** G528
    began building against the two materialize calls in a publication prologue on the reasoning that
    materializing is what publication costs. The split read **0.0003 and 0.0006 ms/f** for those two
    and **1.581 + 1.321** for the two nobody suspected. The design survived only because the split
    ran first; it would otherwise have scoped the two calls that cost nothing and gated at zero.

---


---

## §3 What NOT to Do

- **No "optimization" that skips guest work** (dropping draws, skipping VU programs, frame-skipping)
  as a default. If used as a stopgap, it's a band-aid: env-gate it, default OFF, document in the
  state file with its removal condition.
- **No threading of GUEST EXECUTION for speed** without re-reading `16-runtime-concurrency-threading.md`
  — breaking the single-guest-lock model is a rewrite, not an optimization. **The ONE sanctioned
  exception is parallelizing the GS *rasterizer* across disjoint pixels (`17c-perf-gs-pipeline.md` §1)** — it never touches
  the guest lock and is bit-exact by construction. Keep it default-OFF until soaked across all scenes.
- **No toolchain flag roulette.** `clang-cl + Ninja + Release` is already the sanctioned optimum
  (`03-ps2recomp-pipeline.md` §4). Changing global flags forces a mass rebuild — Build Gate applies.

---
