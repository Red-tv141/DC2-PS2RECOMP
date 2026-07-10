# Reference: Runtime Concurrency & Threading

> **Load this for any HANG, DEADLOCK, livelock, thread starvation, or "boots/runs then freezes at a
> handoff" bug.** A recompiled PS2 game runs the guest's threads/semaphores/event-flags on HOST
> threads, serialized by the runtime. Get the locking wrong and you get freezes that look like a
> render or I/O bug but are actually a scheduler/lock problem. These do not crash — they stop.

---

## §1 The Model — Guest Threads on Host Threads, One Lock

- The PS2 game uses Sony's kernel: `CreateThread`/`StartThread`, `CreateSema`/`WaitSema`/`SignalSema`,
  `CreateEventFlag`/`WaitEventFlag`/`SetEventFlag`, `RotateThreadReadyQueue`, `iWakeupThread`, etc.
  In the runtime these are **handwritten C++ syscalls** (`src/lib/Kernel/Syscalls/...`), not recompiled.
- Guest threads run on real host threads, but only **ONE guest thread executes guest code at a time**,
  serialized by a single **guest-execution mutex**. This preserves the PS2's effectively-cooperative
  EE scheduling and keeps the flat guest RAM coherent.
- A vsync/IRQ worker and the host present loop are *additional* threads that also touch shared guest
  state (vsync flag, DISPFB) under their own mutexes.

So: most "threading" bugs are about **how a blocking syscall hands the single guest lock around.**

---

## §2 Cardinal Rule — A Blocking/Yielding Syscall MUST Release the Guest Lock

Any syscall that **waits** (WaitSema on a taken sema, WaitEventFlag, sleep, join) or **yields**
(`RotateThreadReadyQueue` = syscall `0x2B`) must **release the guest-execution lock for the duration
of the wait**, then reacquire it before returning to guest code. Use a RAII release-scope
(e.g. `GuestExecutionReleaseScope`).

**The trap:** a wait/yield that keeps the lock held (a bare `std::this_thread::yield()` or
`sleep_for` without releasing) **starves every other guest thread** — none of them can run because
the waiter is sitting on the one lock they all need. This is the classic "menu→dungeon freezes at a
thread hand-off" class: the cooperative yield never actually yields. **Fix at the yield/wait site:
release the lock, then sleep/wait, then reacquire** (`RotateThreadReadyQueue`: release + a brief
`sleep_for(~200µs)` so the ready queue truly rotates).

---

## §3 ABBA Lock-Order Hazard — The Deadlock Maker

There is more than one mutex (guest-execution lock; vsync/IRQ mutex; a local wait mutex inside a
`cv.wait`). **All threads must acquire them in ONE consistent order.** The recurring hazard:

- A `cv.wait` site holds a **local wait mutex**, then its release-scope destructor **reacquires the
  guest lock** — while another thread (the IRQ/vsync worker) holds the guest lock and wants the
  vsync/local mutex. A→B vs B→A = **deadlock**.

**Rules:**
- **Never reacquire the guest-execution lock while still holding a local wait mutex or the vsync
  mutex.** Unlock the local mutex *before* the release-scope reacquires guest code.
- Pick a global lock order (e.g. guest-exec lock is always outermost) and never invert it.
- The principled fix is a wait helper that "keeps guest execution released for the whole host wait
  AND unlocks the local mutex before reacquiring guest code" (`waitWithGuestExecutionReleasedUntilUnlocked`
  pattern). Hand-patching individual `cv.wait` sites works but is error-prone — audit them all.

---

## §4 Wake Handoff — Waking ≠ Scheduling

`SignalSema`/`SetEventFlag`/`WakeupThread` mark a waiter runnable, but on a single-lock model the
**signaller can re-grab the guest lock and run on**, starving the just-woken thread indefinitely.
- **Minimum fix:** after waking a *genuine* waiter (only if `waiters > 0` actually fired), release the
  guest lock and `yield()` so the woken thread can run before you continue.
- **Stronger (deterministic handoff):** release and wait (≤~2 ms) on a handoff condvar/epoch until
  another thread *actually acquires* the guest lock — a true handoff, not a hopeful yield. Heavier;
  use only if post-wake yields don't resolve a wake-gated stall.

---

## §5 Diagnosis — Hang Triage

| Symptom | Likely cause | Next step |
|---------|-------------|-----------|
| Hang, **CPU pegged** (100%) | guest spinlock on a HW flag (DMA/vsync/CD) that never fires | not a thread bug — see `07-ps2-code-patterns.md` (DMA/V-SYNC patterns); fire the awaited event in runtime |
| Hang, **CPU idle** | a guest thread waiting on a wake that never comes, OR a deadlock | which thread holds the guest lock? is a waiter never woken (missing Signal)? is it ABBA (§3)? |
| **Boots/runs then freezes at a handoff** | starvation: a yield/wait that didn't release the lock (§2) or a wake that didn't schedule (§4) | release-scope at the wait/yield site; post-wake yield |
| Freeze only with audio/event scripts | an event script waiting on IOP/stream/scene-sound completion that never resolves (§7) | check stream/voice state; cooperative IOP or host-side event-skip |

**Ground truth = PCSX2 threads.** `pcsx2_get_threads()` lists EE/IOP thread states on the real
machine; `pcsx2_get_backtrace()` shows where a thread is stuck. Compare which thread *should* be
running vs which holds your lock. (See `12-pcsx2-mcp-playbook.md`.)

---

## §6 The IOP Side — Audio & RPC Stalls

The IOP (I/O processor) runs its own threads for audio (libsd/SPU2) and RPC. If the runtime **stubs
the IOP entirely**, RPC-completion and stream/voice-finished signals never arrive — so an event
script that waits on "scene sound finished" or "stream opened" **parks forever**. Symptoms look like
a stuck event/cutscene, not an audio bug. Two ways out:
- **Short term:** a host-side detector that reproduces the script's completion step when it's provably
  stalled (an "event-skip" — a band-aid; gate it and document it).
- **Real fix:** a **cooperative IOP scheduler** (an IOP CPU/kernel + module loader where RPC handlers
  spawn async workers), so the wait actually completes. Big, dedicated phase.

For the AUDIO-side completion semantics themselves (ENDX, voice-done, stream-drained — what the
waiting script is actually polling for), see `18-audio-spu2-iop-debugging.md` §4.

---

## §7 Where to Fix + ABI Notes

- Fix in the runtime sync syscalls (`src/lib/Kernel/Syscalls/...`) — **never** the runner.
- **5+-argument calls use MIPS EABI:** the 5th integer arg is in `$t0` (`$a4`), NOT the stack. Derive a
  helper's ABI from the actual `recomp/<caller>.cpp` `SET_GPR` sequence before the `jal`, don't assume.
- 64-bit/FP libgcc helpers use single-register-per-64-bit EABI (read `$a0/$a1` as full 64-bit; doubles
  are soft-float through GPRs).

Cross-refs: spinlock/HW-wait patterns `07-ps2-code-patterns.md`; thread inspection
`12-pcsx2-mcp-playbook.md`; fix taxonomy `10-agent-guardrails.md §3`; adopting an upstream
scheduler fix without clobbering local work `10-agent-guardrails.md §2` (surgical apply).

---

## §8 Back-Edge Preemption & the pc-Mismatch Unwind — Overrides Are Not Yield-Safe

Some runtimes preempt long-running guest code at loop back-edges (a counter in
`shouldPreemptGuestExecution()`-style hooks): the generated code sets `ctx->pc` to a
mid-function resume point and every generated frame unwinds via its
`if (ctx->pc != <expected-return>) return;` chain up to the dispatch loop, which later
re-enters the function through its `switch (ctx->pc)` header. Guest state (`ctx` registers,
guest stack) stays coherent BY CONSTRUCTION — as long as only generated code and the dispatch
loop are on the C++ call stack.

**The hazard: any OVERRIDE that calls a recompiled body as a plain C++ call sits inside that
unwind path and is NOT yield-safe by default.** Two real instances in one port (G57, G186):

1. **Post-call fixup runs early.** The override's code after `real(rdram, ctx, runtime)`
   executes on YIELD, not completion — restoring registers, reading "results", or doing more
   guest calls against half-executed state (G57: a draw-manager left half-initialized).
2. **Register-sentinel contracts break.** An override that emulates a call with a sentinel
   (e.g. `$ra=0` so the callee's final `jr ra` produces a recognizable `pc==0`) loses the
   sentinel's CATCHER when its frame unwinds: the resumed guest chain later reloads the fake
   `ra=0` from its own guest frame, the naked `pc=0` reaches the dispatcher's recovery, and —
   the killer — **stack-scan recovery restores PC but NOT `$sp`**. The resumed code then runs
   with `$sp` hundreds of bytes deep inside live frames; its ordinary sp-relative stores
   corrupt other functions' saved-`$ra` slots, and later returns dispatch garbage DATA values
   ("Function at address 0xNNN not found" where 0xNNN is an asset byte pattern, a global
   object's address, a stack pointer…). This presents as a **nondeterministic "missing
   function" / level-load crash far from the real fault** (G186: three separate crash
   signatures, one cause).

**Rules:**
- An override that delegates to a recompiled body must either (a) wrap the call in the
  runtime's preempt-suppression scope (`g_dc2PreemptSuppressDepth`-style ++/-- — the G57/G186
  fix pattern, cheap and proven), or (b) be resume-aware: loop re-dispatching `ctx->pc` until
  the real completion condition, treating any other pc as "yielded, come back".
- After the call returns, CHECK `ctx->pc` before trusting results — `pc != expected-return`
  means yield, not completion.
- Diagnosis recipe for the corruption class (all built in G186, reusable): full guest-RAM dump
  at first-bad-pc + offline scan for which SLOT holds the garbage value (it was a saved-`$ra`
  slot); a dispatch-boundary word-watch ring dumped at crash; an sp-balance trampoline around
  dispatched calls; per-label sp checks injected into the suspect function's generated file.
  Decode the victim's prologue (`addiu sp,-F; sd ra,X(sp)`) to locate its ra slot exactly.
- **Never read `[dispatch:recover-pc]` / stack-scan-recovery contexts as fault locations** —
  recovery resumes with a stale `$sp` and manufactures plausible-looking downstream crashes in
  unrelated subsystems.
- **Enter/exit trace pairs LIE under yield.** A probe wrapper that logs "enter", calls the
  recompiled body, then logs "exit" emits its "exit" line on the FIRST yield of a long body —
  the body then resumes and keeps running (its inner calls print AFTER the wrapper's "exit").
  Treat exit-side snapshots from such wrappers as mid-execution values, and don't infer
  program order from wrapper-log interleaving unless `ctx->pc` was checked at the exit
  (G193: EditInit/Initialize wrappers both printed early; the AssignCamera calls inside
  logged after the wrapper's own exit line).

