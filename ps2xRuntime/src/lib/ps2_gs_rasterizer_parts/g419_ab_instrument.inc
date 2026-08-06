// G419 — generalized within-process randomized A/B instrument + the exact CT32 upload fast pack.
//
// WHY THIS EXISTS. G418 proved that separate-process lean pairs on this route carry a +-1.5 ms
// run-level offset, which cannot adjudicate a sub-millisecond mechanism: five alternating pairs
// reported the two arms of the SAME mechanism with opposite signs. That is what forced G417 to
// retire an exact optimization it could not separate, and what made G414/G415 shelve exact levers as
// "below the timing floor". G418's fix was an instrument bolted onto one call site. This part lifts
// it into a lever-agnostic instrument so any shelved lever can be re-adjudicated:
//
//   * ONE fixed sample point per frame. Exactly one colour-readback unpack happens per frame, so the
//     interval between consecutive unpacks IS a frame period sampled at a fixed point in the frame.
//     g419AbTick() is called there and nowhere else.
//   * THE ARM IS CHOSEN AT THE TICK, but CHARGED BY THE CONSUMER. A consumer records which arm it
//     actually used (g_g419AbUsed); the next tick charges the elapsed interval to that arm and
//     clears the record. An interval with no consumer, or with both arms observed, is dropped. This
//     makes attribution correct no matter which thread hosts the lever or how deep the host pipeline
//     is - the period is charged to the arm that demonstrably served it. G418's design charged the
//     arm it PICKED, which is only equivalent for a same-thread, zero-lag consumer.
//   * PER-FRAME RANDOMISATION, not block alternation. Fixed-parity block alternation aliases with the
//     route's own periodicity: G418 measured its promoted arm as 0.79 ms SLOWER at block=60 and the
//     sign flipped under parity inversion. The arm comes from a fixed LCG, which cannot alias.
//   * WARM-UP SKIP. Whichever bucket owns the first frames inherits the route's load-in cost.
//   * BLOCKED ESTIMATOR IN-PROCESS. Both arm means drift together, so the difference of per-window
//     means is far tighter than the pooled SE (G418: 0.077 vs 0.120 ms). G418 computed this by hand
//     from the periodic prints; it is now printed directly, with its t statistic.
//   * BALLAST. Both buckets run the CONTROL form and one bucket repeats it N extra times
//     (idempotent). measured_delta / known_added_cost is the site's critical-path sensitivity.
//     Multiply an isolated saving by it BEFORE predicting a payoff - this is the step G414's and
//     G417's inclusive-timer deletions were missing.
//
// Diagnostic-only: it deliberately runs both arms, so DC2_G419_AB is never a shipping mode.
//
// -----------------------------------------------------------------------------------------------
// THE EXACT CT32 UPLOAD FAST PACK (the lever this phase re-adjudicates on the upload side).
//
// The framebuffer-upload staging pack is the mirror image of G418's readback unpack: for every
// guest row it walks x with a full addrPSMCT32() call plus a 4-byte memcpy out of VRAM. G418's
// decomposition applies unchanged, because it is a property of addrPSMCT32 and not of a direction:
//
//     addr(x,y) == (((block>>5) + (y>>5)*width) << 13) + ((x>>6) << 13) + tbl[y&31][x&63]
//
// with tbl[yl][xl] := addrPSMCT32(block, width, xl, yl) - ((block>>5)<<13). The only x term above
// bit 5 is the linear (x>>6) page step; the only y terms are the linear (y>>5) page step and bits
// 0..4 feeding blockTable32/columnTable32, all of which the 32x64 table covers exhaustively. The
// table is literally the function restricted to one page, so nothing is approximated - including a
// non-zero (block&31), whose page carry is already baked into every table entry.
//
// PAIRED LOADS. columnTable32[y&7][x&7] for an even x and x+1 differ by exactly one word and share
// (x>>3)/(x>>6), so the two source pixels are contiguous in VRAM: an even-width row reads as 8-byte
// loads. The partial-bound cases reproduce the original per-pixel behaviour exactly (see below).
//
// WHY A MULTI-ENTRY TABLE CACHE. G418's single-entry cache is correct for the readback, which only
// ever sees fbp=0x13b at one width. Uploads visit fbp=0x139 at BOTH fbw=8 and fbw=2 plus 0x0, 0x68
// and several singletons in the same frame, so a single-entry cache would rebuild 2048 addresses per
// upload and eat the win. A small direct cache keyed by (block,width) makes every steady-state
// lookup a hit. Flush-owner thread only, exactly like G418's - the lanes only read it, after
// GSRowPool::run has published it through the dispatch.

// The shared arm register (g_g419AbArm / g_g419AbUsed) is defined in
// ps2_gs_gpu_parts/gpu_bridge_and_latch_helpers.inc and declared at global scope in
// rasterizer_headers_and_diagnostics.inc:
//   g_g419AbArm  : 0 = control, 1 = candidate. Written only by the tick.
//   g_g419AbUsed : bit0 = a consumer ran the control form, bit1 = the candidate form. Written by
//                  consumers, cleared by the tick.

enum G419Lever
{
    kG419LeverNone = 0,
    kG419LeverNarrow = 1, // DC2_G415_NARROW_READBACK  (GL backend, colour readback rows)
    kG419LeverPack = 2,   // the exact CT32 upload pack (GS front-end, this file)
    kG419LeverLanes = 3,  // pixel-gated row-pool fan-out for the upload pack (this file)
    kG419LeverAll = 4,    // G419's three together == the G419 shipping stack (its promotion gate)
    // G420 levers. `all420` is their combined arm; `all` deliberately still means G419's stack, so
    // the two phases' promotion gates never blur into one another.
    kG420LeverZero = 5,   // exact staging zero-fill elision (g420_pack_residue.inc)
    kG420LeverPLanes = 6, // pack row-pool LANE COUNT (alt vs G298's 8)
    kG420LeverPool = 7,   // GSRowPool dispatch narrowing (wait only for lanes that took work)
    kG420LeverAll = 8,    // the three G420 BEHAVIOUR levers (narrowtouch is diagnostic, excluded)
    // MECHANISM PROBE, never a behaviour lever and never in a combined arm: the candidate bucket
    // writes the complement rows the FULL readback would have written, into an inert scratch
    // buffer. It adds the full read's destination-side memory traffic WITHOUT its GL transfer, so
    // it measures the leading hypothesis for G419's unaccounted narrow-readback payoff directly.
    kG420LeverNTouch = 9,
    // G432: the exact G418 colour-readback unpack applied at the SECOND publication site
    // (g261Materialize, rasterizer_gpu_alias_page_view.inc). Sub-millisecond, so the within-process
    // randomized A/B is its only admissible gate. The site references this id by value because it is
    // compiled earlier in this TU than this enum (see kG432MatUnpackLeverId there).
    kG432LeverMatUnpack = 10,
    // G438: the two DERIVATIVE PROBES moved inside the within-process instrument. They are not
    // behaviour levers and can never be promoted — the candidate arm deliberately WASTES time on
    // one thread. The point is that G433's replication rule (the RUN is the unit, and the run-level
    // frame mean moves 44.8–48.6 ms) makes a cross-run derivative spin unusable on a quiet day:
    // G438 block A read VU1 +1.96 / GS −2.45 and block B read VU1 −0.83 / GS −0.27, i.e. both
    // probes disagreed with themselves in SIGN. Alternating the spin per FRAME inside one process
    // differences the drift away exactly as it does for a sub-ms behaviour lever, and turns the
    // pole ranking into a paired t statistic instead of a difference of two noisy run means.
    kG438LeverVu1Slow = 11, // spin DC2_G303_VU1_SLOW_US per kick on candidate frames only
    kG438LeverGsSlow = 12,  // spin DC2_G431_GS_SLOW_US per window on candidate frames only
    // G439: the WINDOW-COUNT ceiling probe. G434 priced every named block of work on the GS worker
    // at 2.83 ms/f while G438 measured the frame absorbing ~96% of any microsecond added PER WINDOW,
    // so what the thread transmits is the 1,596-windows/frame handoff chain, not the work inside it.
    // The candidate arm merges DC2_G439_COALESCE consecutive drain() windows into one queue item:
    // window count drops ~N x while kicks/frame and packets/frame are exactly conserved (the
    // workload invariant). Knowingly INCORRECT output — processWindow's stable path sort now spans
    // what were separate windows — so this is a ceiling probe and can never be promoted.
    kG439LeverCoalesce = 13,
    // G445: the G418 early command kick (one glFlush per skip-readback render batch), moved into the
    // within-process instrument. It is a real BEHAVIOUR lever and exact by construction — glFlush
    // changes no result, no GL state, no command order and no ownership — but its only prior gate was
    // a cross-run block taken while VU1 bound the frame, i.e. the G444 expiry law applies to it. On
    // this host the run-level frame mean drifts ~1.4x within a session (46.6 -> 33.3 on the base arm
    // in one G445 block), which is several times the lever, so a cross-run A/B cannot gate it.
    // Thread-level (it changes when the GL backend starts executing queued work, and the effect lands
    // on the NEXT batch's drain), so it inherits the block hold like the two spins and coalesce.
    kG445LeverEarlyKick = 14,
    // G445 extension probe: the same kick applied to the backend items G418 never covered — the
    // GPU->GPU alias copies and the decode/composite/upload class ("copy" and "other" in
    // [G332:gsw]). Those leave GL work queued and never fence either, so on the same argument they
    // should also be contributing to the readback's drain. Separate lever id because it is a second
    // architectural decision (WHICH items are kicked), not a re-measurement of the first.
    kG445LeverKickAll = 15,
    // G447: spin-then-block on the GL backend round trip. G447's edge census proved 100% of the
    // pole thread's 16.7 ms/f of blocked time is these ~74 promise/future round trips per frame, and
    // that the GL worker's own work only accounts for ~13 of it. Thread-level (it changes when the
    // GS worker resumes after a handoff), so it inherits the block hold.
    kG447LeverSpinWait = 16,
    // G447: the SUBMIT half of the same round trip — the GL backend worker spins on a submit
    // sequence counter before sleeping, so a queued item does not wait for that thread to be
    // scheduled back on. Separate id: it is a different thread burning the CPU.
    kG447LeverWSpin = 17,
    // G447 PROMOTION GATE: both halves of the round trip alternated together, i.e. exactly the
    // shipped default against exactly the double rollback. The two levers act on DIFFERENT threads
    // and each carries a CPU-burn cost, so only a combined arm answers "does the promoted pair pay".
    kG447LeverSpinBoth = 18,
    // G452: corrected one-slot G305 async break. G444 admitted G310 batches to async submission,
    // but its drain sites remained gated only by DC2_G305_ASYNC; the second submit therefore
    // failed and fell back to CPU replay. This arm keeps every consumer/frame drain live for both
    // buckets and alternates only admission of eligible G310 skip-readback batches.
    kG452LeverAsyncOne = 19,
    // G454: knowingly-wrong dependency-edge deletion probes. The candidate keeps the same
    // publication rows and unpack work but omits the synchronous FBO readback at one named
    // ownership boundary. These price an exact future ownership mechanism; they can never ship.
    kG454LeverReadbackAll = 20,
    kG454LeverReadbackL2l = 21,
    kG454LeverReadbackResync = 22,
    kG454LeverReadbackDisplay = 23,
    // G456: exact short-circuit form of G407's sprite-axis UV predicate. Both arms
    // return identical flags; this paired gate measures only the avoided gcd/divides.
    kG456LeverUvPredicate = 24,
    // G473: exact four-pixel SSE2 form of the G440 dual-CRT presentation blend. The scalar
    // control and SIMD candidate use the same signed-delta /255 truncation rule byte-for-byte.
    kG473LeverSimdMerge = 25,
    // G475: compact VU1 trace for sequential VNOP-upper + covered non-control lower pairs.
    // Thread-level code-shape lever; hold blocks prevent MTVU work leaking between arms.
    kG475LeverVnopTrace = 26,
    // G476: run-weighted page-generation walk. The +1 slack page makes the old loop visit every
    // interior page twice; the candidate adds 2 once instead of 1 twice, halving the `lock xadd`
    // count in g178BumpRectImpl. Identical final counters, so this gate measures only avoided work.
    kG476LeverPageBump = 27,
    // G477: swizzle-row hoisted IMAGE run writers (CT32/Z32 pair stores, P4 row hoist). Both arms
    // write byte-identical VRAM; the gate measures only the avoided per-pixel address derivation.
    kG477LeverRunWrite = 28,
    // G478: recycling pool for the per-kick G297KickItem. Both arms build a byte-identical item;
    // the gate measures only the avoided make_shared / vector regrow / cross-thread free.
    kG478LeverKickPool = 29,
    // G479: fused 16-byte VU1 pair record. Both arms decode the same two words from the same
    // precisely-invalidated cache; the gate measures only the removed second address chain and
    // the VU1 code store leaving the hot loop's working set. Thread-level code-SHAPE lever (it
    // selects a loop copy), so it inherits the G438 block hold like G475.
    kG479LeverLeanFetch = 30,
    // G480: cross-thread recycling of the per-XGKICK Path1 packet buffers. Both arms hand the GS
    // worker byte-identical packet bytes; the gate measures only the removed per-packet malloc on
    // the VU1 worker (the pole) and the cross-thread free on the GS worker.
    kG480LeverPktPool = 31,
    // G480: the COMBINED promotion arm — the per-XGKICK packet pool AND the G478 kick-item pool
    // switched together. G447's rule: two levers on the same serialized edge do not add, so the
    // promotion number must come from one arm that is exactly the shipped default against exactly
    // the full rollback. Both pools live on the same VU1-worker allocation edge.
    kG480LeverPoolBoth = 32,
    // G480: skip the out-of-line g422FastLower call on a NOP lower slot (39% of pairs). Both arms
    // leave VU1State and VU RAM identical — the NOP case of that function is literally `return;`.
    // Loop-invariant, read once per run(), so the arm is held for a G438 block like G475/G479.
    kG480LeverNopSkip = 33,
    // G481: execute the seven HOT G422 lower kinds (~42% of pairs) inline in the pair loop instead
    // of through the out-of-line g422FastLower call. Both arms expand the SAME body tokens
    // (vu1_g481_hot_lower.inc), so VU1State and VU RAM are identical and the gate measures only the
    // removed call. Loop-invariant code-SHAPE lever, so it inherits the G438 block hold.
    kG481LeverHotLower = 34,
    // G482: PRICE ONLY. The candidate arm computes the full intra-frame memo KEY (16 KB of
    // persistent VU data RAM + VU1State + code generation + kick params) once per geometry kick on
    // the VU1 worker and sinks it; the control arm does nothing. Neither arm changes a single byte
    // of guest state or output, so the gate measures exactly one thing: what the memo's key costs
    // the frame. Census before mechanism (G476) — if this exceeds the ~1.3 ms/f the memo could
    // return, the vein closes on arithmetic.
    kG482LeverMemoHash = 35,
    // G482: coalesce contiguous VU-RAM writes on the EE so the VU1 worker replays a handful of
    // long memcpys instead of ~59 short ones per kick. Both arms replay byte-identical content to
    // byte-identical offsets; the gate measures only the removed per-record loop work on the pole.
    kG482LeverWCoal = 36,
    // G482: skip the per-kick m_doneMtx acquire + notify_all when m_doneWaiters is zero. Both arms
    // publish the same `ready` flag with the same happens-before; the gate measures the lock only.
    kG482LeverNotifySkip = 37,
    // G482: RETIRED combined arm. `notifyskip` measured +0.528 ms/f SLOWER and is now default-off,
    // so the shipped default is `wcoal` alone and `wcoal` IS the promotion arm. Kept so an explicit
    // DC2_G419_AB=g482both still selects the notify lever only (its own record).
    kG482LeverBoth = 38,
    kG483LeverPcReg = 39,
    // G484: the pair loop addresses the nine scalar-pipe / flag-pipe thread_locals through ONE
    // hoisted base pointer instead of seven. Same storage, same bytes, same order — the arms
    // differ only in the addressing mode, so this measures register pressure and nothing else.
    kG484LeverWideVf0 = 40,
    // G485 REFUTED the register-form G139 hazard (`hazreg`, lever 41): mean +0.232 ms/f, sign 3/4
    // SLOWER. Removing the aliasing does not pay for the three loop-live values it adds. The lever
    // id is retired rather than reused so an old command line fails loudly instead of selecting
    // something else.
    // G486 REFUTED the wide single-lane FMAC destination store (`widedest`, lever 42): mean
    // +0.597 ms/f, sign 4/4 SLOWER. A single-lane write cannot be widened without READING the
    // destination's other three lanes, so the candidate turns a pure store into a
    // load-modify-store and puts a 16-byte load on the dependency chain. The lever id is retired
    // rather than reused so an old command line fails loudly instead of selecting something else.
    // G487: extend the G422 compiled lower to the four flag consumers (FMAND/FCOR/FCGET/FCAND,
    // 2.54% of pairs) so they stop crossing execLower's call boundary — 8 GPR pushes, ten xmm6-15
    // `movaps` spills, a 1832-byte frame and a /GS cookie, read off this binary's own /MAP. Both
    // arms execute the SAME bodies (the candidate's are a verbatim transcription of execLower's
    // cases) and the loop body is byte-identical; the arm changes only what
    // `g422DecodeLowerKind` writes into the G410 pair cache, which is re-decoded on every arm
    // flip. That is why this cannot be an in-loop compare: an extra per-pair test would sit on
    // the CONTROL arm on 59.9% of pairs and bias the gate in the candidate's favour (G486 §4).
    kG487LeverFlagLower = 43,
    // G488: extend the G421 compiled upper to OPMSUB/OPMULA/CLIPw (~1.08% of pairs, ~17.6 k/f) so
    // they stop crossing execUpper's call boundary — 8 GPR pushes, `sub rsp,0xa0`, four xmm6-9
    // `movaps` and a /GS cookie each way. Same shape as `flaglower`: both arms run the SAME loop
    // text and the arm changes only the `upperKind` byte the G410 pair cache is decoded with (the
    // control arm gets SHADOW kinds 0x72/0x73/0x74, whose G421 descriptor entries are zero), which
    // is re-decoded on every flip. An in-loop test is inadmissible for the same reason as G487's:
    // it would sit on the CONTROL arm on ~49% of pairs. NOTE what this gate does NOT cover — both
    // arms carry the added inline bodies in the loop's text, so it prices the removed call, not
    // the G481 inline-budget footprint; that is checked from the /MAP and the default-path level.
    kG488LeverFastOp = 44,
    // G489: the cached "this lower can write pc" bit. Also a cache-decode arm, so it must be held
    // for a G438 block. Unlike G487/G488 there is no shadow encoding — the CONTROL arm simply sets
    // the bit on every pair, which reproduces the pre-G489 tail exactly with identical loop text.
    // The control arm does carry one always-true byte test the old binary did not, so this gate
    // reads as a FLOOR on the lever.
    kG489LeverPcSkip = 45,
    // G490: the WIDENED BLOCK RUN — a whole run of eligible pairs executed in one noinline call,
    // paying the per-pair envelope once per RUN. Selects a LOOP COPY (like G483/G484/G485), so it
    // must be held for a G438 block: a flip mid-run() is impossible by construction (the arm is
    // read once per run()), but a per-frame flip would still put the ~20 us cache rebuild on one
    // arm's frames. The control arm is the SHIPPED default copy, textually identical apart from
    // the hook, so this gate prices the mechanism and not a refactor of the control.
    kG490LeverBlockRun = 46,
    // G491: the GS colour readback taken in the driver's NATIVE packed format (BGRA/8888_REV) with
    // the channel restore done on the CPU, instead of the spec-guaranteed GL_RGBA/UNSIGNED_BYTE.
    // Six reads per frame on the GS worker, ~545 us each; a per-read decision, so it does NOT need
    // a block hold for correctness — but it is held anyway (16) because the GL driver's readback
    // path is stateful enough that alternating it per frame would measure the transition.
    kG491LeverRbFmt = 47,
    // G491: drop the G447 spin on the readback edge only. Thread-level (it changes which thread
    // burns CPU), so it inherits the G438 block hold for the same reason the two G447 spins do.
    kG491LeverRbSpin = 48,
    // G492: the two unpriced blocking edges of the pole thread, deleted whole. `norender` drops the
    // backend-render round trip (3.5 ms/f over 34 items), `noother` the T8/CT24/composite round trip
    // (2.8 ms/f over 20). Knowingly-wrong output, built on the G454 rb_all pattern: every byte of
    // front-end preparation and downstream publication stays, only the queue push and the blocking
    // wait go. Ceiling probes — they can never be promoted. Thread-level (they change whether the GL
    // worker has anything queued at all), so both inherit the G438 block hold.
    kG492LeverNoRender = 49,
    kG492LeverNoOther = 50,
    // G492: skip the redundant glTexImage2D storage reallocation of the CT24 view's OUTPUT texture.
    // The build's full-screen draw rewrites every texel, so both arms produce a bit-identical view
    // and the gate measures only the removed allocation. One call per frame on the GL worker; held
    // for a G438 block anyway because GL driver allocation state is sticky across frames.
    kG492LeverCt24Reuse = 51,
    // G493: move the async break's drain from the TOP of g178TryFlushGpu to immediately before that
    // flush's own submit. G493 §1 measured the async break deleting 16 of 34 render waits
    // (blocked -1.1 ms/f) while `cpu` rose 1.6 ms/f and the frame did not move — the wait relocates
    // into the un-scoped drainAsync(). The candidate arm gives the pending packet the flush's whole
    // preparation to overlap with. Run it with DC2_G444_ASYNC=1 on BOTH arms: the only variable is
    // the drain POINT. Thread-level (it changes whether the GL worker runs concurrently with the
    // pole thread at all), so it inherits the G438 block hold.
    kG493LeverLateDrain = 52,
    // G494: elide the GL backend's per-item VALIDATION queries (glGetError, glCheckFramebufferStatus)
    // — the calls that force a rendezvous with the NVIDIA threaded driver's own worker and produce no
    // output byte. ~110 per frame against ~73 backend items. Read on the GL WORKER thread, several
    // times per item; thread-level (it changes how often that thread blocks on the driver), so it
    // inherits the G438 block hold like every other thread-level arm.
    kG494LeverNoSync = 53,
    // G496: the GPU DERIVATIVE PROBE — the arc's third derivative probe, and the first on the GPU.
    // The candidate arm re-issues each render batch's whole vertex range with an identity blend
    // (dst = 0*src + 1*dst), depth test off and depth writes off: every fragment is rasterized,
    // shaded, blended and written, and the byte written is the byte that was already there. Like
    // `gsslow` and `vu1slow` it deliberately WASTES time and can never be promoted. Thread-level in
    // the strongest sense (it changes how much work the GPU has queued when the next fence lands),
    // so it inherits the G438 block hold. Its injected cost is MEASURED, not assumed —
    // DC2_G496_FORCE=1 DC2_G496_CAL=1 prints `[G496:cal] injMs/f`.
    kG496LeverGpuSlow = 54,
    // G496b: redundant GL state elision in the render-batch draw loop. Both arms leave GL holding
    // the SAME state vector and render the same bytes; the arm decides only whether a call that
    // sets the value already there is issued at all. Read once per render batch on the GL worker,
    // and thread-level (it changes how much command translation that thread hands the driver), so
    // it takes the G438 block hold.
    kG496LeverGlState = 55,
    // G497: recycle the PER-WINDOW handoff buffers. MAP-0 seals ~1,560 windows/frame, and each one
    // (a) leaves the EE's `m_curBatch` with zero capacity after the move, so the next kick regrows
    // it from the heap, and (b) hands the GS worker a vector it then FREES — allocation on one
    // thread, free on another, 1,560x/frame. That is the same mechanism G478/G480 priced on the
    // 1,230 kicks/frame, one level up, and it was never touched. The candidate arm returns both
    // buffers to a bounded free list instead. Only CAPACITY survives a recycle; every consumer sees
    // size 0 and the kick shared_ptrs are released at exactly the same point as before, so the arms
    // are exact by construction. Thread-level (it changes the EE<->GS handoff cost), so it takes
    // the G438 block hold.
    kG497LeverWinBuf = 56,
    // G498: route the two OWNER-PUBLISH colour unpacks (g345MaterializeSlot 212,992 px/f and
    // g289Resolve14aBeforeDirect 49,152 px/f) through G418's promoted table/paired/lane arm on the
    // rows where the owner's per-pixel block filter is provably a no-op. Both arms write the same
    // bytes to the same VRAM addresses by construction (the filter decision is unchanged; only the
    // rows it cannot affect change their loop). Read once per publication on the GS worker, and it
    // dispatches row lanes, so it is thread-level and takes the G438 block hold.
    kG498LeverOwnPub = 57,
    // G499: give the G490 block executor the lower dispatch the pair loop has had since G480/G481
    // — skip the `g422FastLower` call outright for the 39.5% of pairs whose lower is NOP (that
    // case's whole body is `return;`) and run the three block-eligible hot INTEGER kinds
    // (IADDI 6.99% + IADDIU 3.07% + IAND 3.02%) inline. Both are promoted default-ON in
    // DC2_G422_RUN_LOWER and neither ever reached the block copy. Selects which of two noinline
    // block bodies run() calls, read once per run() on the VU1 worker — thread-level code shape,
    // so it takes the G438 block hold exactly like `blockrun`.
    kG499LeverBlkLower = 58,
    // G499: the Q/P delay counters as BLOCK LOCALS. `g490BlockBody` read-modify-writes both through
    // `int *` on every pair with an inlined `g422FastLower` (which stores into `vuData`) in between,
    // so MSVC must reload them from memory at the top of each iteration. Block eligibility excludes
    // every writer of those counters (`G426_PF_PRESTALL` + the uncovered lowers), so inside a block
    // they can only COUNT DOWN — a local copy written back once is exact. Control arm is the
    // `blklower` candidate, so this prices the residency alone. Thread-level, G438 block hold.
    kG499LeverQpBlock = 59,
    // G500: let a G490 block run END on the control transfer that terminates it, and swallow that
    // branch's delay slot. Control transfer is 8.88% of all pairs on this route and ~69% of all run
    // terminations, and a TAKEN branch's delay slot is excluded a second time over (the block hook
    // is gated on `pendingBranchTarget == NO_BRANCH`), so both populations pay the full per-pair
    // envelope today. The arm only EXTENDS a run that was already long enough to form a block, so
    // blocks/frame is invariant and the gate prices exactly the pairs it moves. Control arm is the
    // G499 shipped body. Selects which noinline block body run() calls, read once per run() on the
    // VU1 worker — thread-level code shape, so it takes the G438 block hold like `blklower`.
    kG500LeverBlkBranch = 60,
    // G502: the BLOCK ROUND-TRIP ceiling probe. `cover=88.99%` at `avg=9.15` is ~157 k block calls
    // per frame, and each one costs a full trip round the pair loop's top of iteration plus a
    // noinline 10-argument call before the next block's first pair runs. The candidate arm HALVES
    // every block whose clamped length is >= 2*MIN_RUN, which leaves the executed pairs, their
    // order and the coverage bit-identical and adds exactly ONE extra round trip per split block —
    // an INFLATION probe, so it carries none of G492's producer-edge deletion hazard. Frame delta
    // divided by `[G502:blk] split/f` IS the price of one round trip, i.e. the ceiling of any
    // chaining executor. Thread-level code shape, so it takes the G438 block hold.
    kG502LeverBlkSplit = 61,
    // G502: the lever that ceiling prices. A block may CONTINUE into the next eligible run without
    // returning to run() — after a sequential end, or across a control transfer whose delay slot
    // the block already swallowed. Control arm is the G500 shipped body. PRICED OUT and compiled
    // out — see DC2_G502_BLKCHAIN in vu1_g490_block_run.inc; the id is kept so a re-arm is one edit.
    kG502LeverBlkChain = 62,
    // G502: a block may consist of the CONTROL TRANSFER ALONE (plus the delay slot it swallows).
    // G500 admitted a tail only onto a run that already reached MIN_RUN, so a branch preceded by
    // fewer than two eligible pairs still pays the full per-pair envelope for itself AND for its
    // delay slot. `[G502:excl]` prices that remainder at 2.10% + 2.05% of ALL pairs, and G500's own
    // realised 7.0 ns/pair for exactly this population makes it ~0.47 ms/f gross. Control arm is
    // the G500 shipped admission test; only `g490Count + tail >= MIN_RUN` replaces
    // `g490Count >= MIN_RUN`. Thread-level code shape, so it takes the G438 block hold.
    kG502LeverBlkTail0 = 63,
    // G503: THE FOURTH DERIVATIVE PROBE — the EE (guest main) thread, the one unit this arc has
    // never injected into. G503's premise gate read VU1 0.04 / GS 0.27 / GPU ~0.00, a sensitivity
    // SUM of 0.31 against G502's 0.85 and G501's 1.10: two thirds of the frame's marginal cost was
    // in something unprobed. The occupancy census names the candidate — `[G182:ee] cpuMs/f` is a
    // GetThreadTimes delta (NOT the frame bracket `busyMs/f` is, G491's trap) and reads 16.7-19.3 ms
    // of real CPU per ~20 ms frame at onCPU 84-96%, while `[G146:perf]` reports VIF1 / GIFsubmit /
    // GSimage / GSlocal all 0.0, i.e. that CPU is recompiled GUEST code, not the EE-side graphics
    // path. Exact mirror of vu1slow/gsslow: busy-spin DC2_G503_EE_SLOW_US once per guest frame on
    // the EE thread, on candidate frames only. Injected magnitude is the spin length itself (ONE
    // spin per frame), so it needs no calibration run. Wastes time by construction — never
    // promotable. Thread-level, so it takes the G438 block hold.
    kG503LeverEeSlow = 64,
    // G503: recycled DMA-chain staging buffer. `PS2Memory::processTransferChain` built a FRESH
    // zero-capacity `std::vector<uint8_t>` per DMA chain and appended up to 16,384 tag payloads into
    // it, so every chain paid log2(N) geometric reallocations (each memmoving everything appended so
    // far) plus one operator new and one free. The caller-attributed `eeprof` puts that at 13.78% of
    // the EE thread in `_Insert_counted_range` with a 10.86% `memmove` leaf, beside 5.69%
    // ZwFreeVirtualMemory / 1.59% RtlAllocateHeap / 1.43% operator new. Both arms append the SAME
    // bytes in the same order and hand the consumer the same `size()`; only the buffer's CAPACITY on
    // entry differs, so there is no output to verify. Thread-level (it changes allocator traffic on
    // the pole thread), so it takes the G438 block hold.
    kG503LeverChainPool = 65,
    // G504: the G430 ring cursor as a block local. `[G483:rawpc]` resolved against a linker map put
    // the SINGLE HOTTEST instruction of the VU1 worker — the pole at sensitivity 1.03 this session —
    // inside `g430PushPending`, with the push at ~9.2% of the thread. Consecutive pushes are chained
    // through memory (`H.head` loaded, four stores indexed off it, stored back), so each waits on
    // the previous one's store to forward. Arm 6 carries that cursor in a block local; arm 3 (the
    // G500/G501 shipped body) is the control. The two arms execute the identical pairs in the
    // identical blocks and write the identical ring entries — only where the index lives differs.
    // Thread-level code shape, so it takes the G438 block hold.
    kG504LeverHistCur = 66,
    // G506: the per-pair Q/P delay advance, hoisted out of the block's pair loop by segmentation.
    // Selects block arm 7 against arm 3 (the G500/G501 shipped body) in g499BlkArmRun(). The two
    // arms execute the identical pairs in the identical blocks and publish `st.q`/`st.p` at the
    // identical pairs — only how often the two counters are TESTED differs. Thread-level code
    // shape, so it takes the G438 block hold.
    kG506LeverQpSeg = 67,
    // G508: skip the REDUNDANT recursive re-acquisition of GS::m_stateMutex. `GS::writeRegister`
    // locks that mutex on every call and `GS::processGIFPacket` — its only hot caller — already
    // holds it for the whole packet, so the GS worker pays ~55.8k already-owned re-acquisitions per
    // frame (`[G341:vifcensus]` 50,007 PATH1 + 5,827 PATH2 register writes). Priced BEFORE building
    // on this host at 5.97 ns per skipped call (7.25 recursive re-lock vs 1.29 thread_local depth
    // test) = ~0.33 ms/f.
    //   ARM DESIGN. The arm is read ONCE PER PACKET, in processGIFPacket's own guard — never in
    // writeRegister — so the instrument cannot pollute the 55.8k-call path it is measuring. The
    // candidate arm raises the thread-local depth so nested guards skip; the control arm leaves it
    // at 0 so every nested writeRegister locks for real, which is byte-for-byte the pre-G508 path.
    // Both arms hold the mutex across the identical region and write the identical bytes.
    // Thread-level (it changes the GS worker's per-frame CPU), so it takes the G438 block hold.
    kG508LeverLockSkip = 68,
    // G510: move the captured G144Entry into g_g144List instead of copying it. `G144Entry e` is a
    // loop-local, fully assigned and dead immediately after the push, so a move lands the identical
    // bytes in the identical slot and removes one whole ~150-byte struct copy plus the G149Buf
    // copy-construct/destroy pair, ~9,900 times per frame ON THE GS WORKER (the pole). Read once per
    // captured primitive through the standard per-frame tick-sequence memo, so the accessor is two
    // relaxed loads on both arms and cannot bias the delta. Thread-level (it changes the pole
    // thread's per-frame CPU), so it takes the G438 block hold.
    kG510LeverCapMove = 69,
    // G510: memoize g178ClassifyEntry across CONSECUTIVE graph entries with byte-identical state.
    // G178EntryState is a pure function of the entry's state registers (ctx/prim/texa/texclut/pabe/
    // fogcol/titleRockScope/townDepthScope) plus static env gates and per-fbp predicates that are
    // themselves functions of ctx.frame.fbp — a mesh's triangles differ only in their vertices. The
    // candidate arm compares that state against the previous entry's and reuses s_states[i-1] on a
    // match; the control arm always calls classify. The comparison is a byte compare, so it can only
    // produce false MISSES (padding), never a false hit. ~9,900 entries/frame on the pole thread.
    // Read once per GPU flush (33.4/frame), so the accessor cannot pollute the measured loop.
    // Thread-level, so it takes the G438 block hold.
    kG510LeverClsMemo = 70,
    // G510 PROMOTION GATE: both G510 levers together, i.e. exactly the shipped default against
    // exactly the full rollback. They sit on the same thread but on different edges (capture vs
    // flush), so G447's "two levers on one serialized edge do not add" does not apply — the combined
    // arm exists so the promotion number comes from the shipped pair, not from a sum of two singles.
    kG510LeverBoth = 71,
    // G511: run-length dedup of the per-entry `s_batchKeys` probe in the flush's TEXTURE pass.
    // That pass walks all ~9,900 graph entries and asks an unordered_set whether this entry's
    // texture key was already collected in this flush; G510 measured 94.4% of entries carrying
    // byte-identical state to their predecessor, so the key is almost always the SAME key the
    // previous iteration just proved present. Remembering that one key turns the hash probe into a
    // 64-bit compare for the repeat. Read once per GPU flush (33.4/frame) against the ~9,900 sites
    // it gates, so the accessor cannot pollute the loop it measures (G502).
    // Thread-level (it changes the pole thread's per-frame CPU), so it takes the G438 block hold.
    kG511LeverTexRun = 72,
    // G511 INFLATION probe (G502): the candidate arm performs ONE EXTRA `s_batchKeys.count()` per
    // probing entry and folds the result into a sink so the optimiser cannot delete it. The paired
    // delta divided by the census's probes/f is the price of ONE set probe on the real key
    // distribution — i.e. the exact quantity `texrun` removes. It is a MEASUREMENT arm, never a
    // promotion candidate: arm 1 is strictly slower than arm 0 by construction.
    kG511LeverTexProbe = 73,
    // G511 INFLATION probe #2 (G502): the candidate arm performs ONE EXTRA full state-identity test
    // — the four `memcmp`s plus four scalar compares G510 put on EVERY graph entry — in the flush's
    // Pass 1, folded into a sink. `class` reads 1.00 ms/f this session (§2) DESPITE the G510 memo
    // serving 94.4% of the classifications, so the memo's own comparison cost is the one per-entry
    // price in `pre` nobody has measured. The delta / 9,960 entries per frame is that price, and it
    // decides whether a cheap state-generation stamp is worth a phase.
    // Measurement arm only — never promoted, no env force flag.
    kG511LeverClsProbe = 74,
    // G512: the whole-texture DECODE inner loop. G511 priced `g178DecodeWhole` at 1.09 ms/f =
    // ~26.6 ns of CPU per texel (~80 host cycles) over 4 calls/frame and ~81,601 px each, against
    // 0.33 ns/px for merely HASHING the same VRAM. The G512 disassembly of `decodeRows`
    // (0x1403db080, 0x260 bytes) named the 80 cycles: per TEXEL the emitted body reloaded `psm`
    // through a closure pointer, re-ran a 6-way switch, made an out-of-line `call` into
    // ps2_gs_memory.obj for the swizzled read (LTCG did not inline it), made a SECOND out-of-line
    // call to g178LookupClutFixed for palettized formats — which re-derives a whole swizzled CLUT
    // address per texel — then reloaded `texW` through two more pointer chases and did a 64-bit
    // imul, twice. Candidate arm hoists every invariant into a VALUE, specialises the psm switch
    // ONCE outside the loop, materialises the 256-entry CLUT ONCE per decode, and batches the
    // swizzled read to one call per ROW. Read once per DECODE (4/frame) against ~326,000 texels,
    // so the accessor cannot pollute the loop it measures (G502).
    // Thread-level (it changes the pole thread's per-frame CPU), so it takes the G438 block hold.
    kG512LeverFastDec = 75,
};

// DC2_G419_AB=<lever> selects the ONE lever the instrument alternates. Exactly one at a time:
// a phase must keep one architectural variable per arm, and a shared arm register cannot attribute
// two levers from one period series.
// G447 HOST CHECK (DC2_G447_HOSTCHECK=1). One flag that turns a run on an UNFAMILIAR MACHINE into a
// copy-pasteable verdict on the two promoted G447 spins — the arc's only host-shaped levers, since
// they buy wall time by burning CPU on an otherwise-blocked thread. It selects the combined
// `spinboth` arm (the promotion gate: exactly the shipped default against exactly the full
// rollback), reports the host's core count, and prints a plain verdict beside every `[G419:ab]`
// window. An explicit DC2_G419_AB still wins, so this can never silently hijack another phase's A/B.
static bool g447HostCheckOn()
{
    static const bool on = (std::getenv("DC2_G447_HOSTCHECK") != nullptr);
    return on;
}

static int g419AbLever()
{
    static const int lever = [] {
        const char *value = std::getenv("DC2_G419_AB");
        if (value == nullptr || value[0] == '\0')
            return g447HostCheckOn() ? static_cast<int>(kG447LeverSpinBoth)
                                     : static_cast<int>(kG419LeverNone);
        if (std::strcmp(value, "narrow") == 0)
            return static_cast<int>(kG419LeverNarrow);
        if (std::strcmp(value, "pack") == 0)
            return static_cast<int>(kG419LeverPack);
        if (std::strcmp(value, "lanes") == 0)
            return static_cast<int>(kG419LeverLanes);
        if (std::strcmp(value, "all") == 0)
            return static_cast<int>(kG419LeverAll);
        if (std::strcmp(value, "zero") == 0)
            return static_cast<int>(kG420LeverZero);
        if (std::strcmp(value, "packlanes") == 0)
            return static_cast<int>(kG420LeverPLanes);
        if (std::strcmp(value, "pool") == 0)
            return static_cast<int>(kG420LeverPool);
        if (std::strcmp(value, "all420") == 0)
            return static_cast<int>(kG420LeverAll);
        if (std::strcmp(value, "narrowtouch") == 0)
            return static_cast<int>(kG420LeverNTouch);
        if (std::strcmp(value, "matunpack") == 0)
            return static_cast<int>(kG432LeverMatUnpack);
        if (std::strcmp(value, "vu1slow") == 0)
            return static_cast<int>(kG438LeverVu1Slow);
        if (std::strcmp(value, "gsslow") == 0)
            return static_cast<int>(kG438LeverGsSlow);
        if (std::strcmp(value, "coalesce") == 0)
            return static_cast<int>(kG439LeverCoalesce);
        if (std::strcmp(value, "earlykick") == 0)
            return static_cast<int>(kG445LeverEarlyKick);
        if (std::strcmp(value, "kickall") == 0)
            return static_cast<int>(kG445LeverKickAll);
        if (std::strcmp(value, "spinwait") == 0)
            return static_cast<int>(kG447LeverSpinWait);
        if (std::strcmp(value, "wspin") == 0)
            return static_cast<int>(kG447LeverWSpin);
        if (std::strcmp(value, "spinboth") == 0)
            return static_cast<int>(kG447LeverSpinBoth);
        if (std::strcmp(value, "async1") == 0)
            return static_cast<int>(kG452LeverAsyncOne);
        if (std::strcmp(value, "rb_all") == 0)
            return static_cast<int>(kG454LeverReadbackAll);
        if (std::strcmp(value, "rb_l2l") == 0)
            return static_cast<int>(kG454LeverReadbackL2l);
        if (std::strcmp(value, "rb_resync") == 0)
            return static_cast<int>(kG454LeverReadbackResync);
        if (std::strcmp(value, "rb_display") == 0)
            return static_cast<int>(kG454LeverReadbackDisplay);
        if (std::strcmp(value, "uvpred") == 0)
            return static_cast<int>(kG456LeverUvPredicate);
        if (std::strcmp(value, "simdmerge") == 0)
            return static_cast<int>(kG473LeverSimdMerge);
        if (std::strcmp(value, "vnoptrace") == 0)
            return static_cast<int>(kG475LeverVnopTrace);
        if (std::strcmp(value, "pagebump") == 0)
            return static_cast<int>(kG476LeverPageBump);
        if (std::strcmp(value, "runwrite") == 0)
            return static_cast<int>(kG477LeverRunWrite);
        if (std::strcmp(value, "kickpool") == 0)
            return static_cast<int>(kG478LeverKickPool);
        if (std::strcmp(value, "leanfetch") == 0)
            return static_cast<int>(kG479LeverLeanFetch);
        if (std::strcmp(value, "pktpool") == 0)
            return static_cast<int>(kG480LeverPktPool);
        if (std::strcmp(value, "poolboth") == 0)
            return static_cast<int>(kG480LeverPoolBoth);
        if (std::strcmp(value, "nopskip") == 0)
            return static_cast<int>(kG480LeverNopSkip);
        if (std::strcmp(value, "hotlower") == 0)
            return static_cast<int>(kG481LeverHotLower);
        if (std::strcmp(value, "memohash") == 0)
            return static_cast<int>(kG482LeverMemoHash);
        if (std::strcmp(value, "wcoal") == 0)
            return static_cast<int>(kG482LeverWCoal);
        if (std::strcmp(value, "notifyskip") == 0)
            return static_cast<int>(kG482LeverNotifySkip);
        if (std::strcmp(value, "g482both") == 0)
            return static_cast<int>(kG482LeverBoth);
        if (std::strcmp(value, "pcreg") == 0)
            return static_cast<int>(kG483LeverPcReg);
        if (std::strcmp(value, "widevf0") == 0)
            return static_cast<int>(kG484LeverWideVf0);
        if (std::strcmp(value, "flaglower") == 0)
            return static_cast<int>(kG487LeverFlagLower);
        if (std::strcmp(value, "fastop") == 0)
            return static_cast<int>(kG488LeverFastOp);
        if (std::strcmp(value, "pcskip") == 0)
            return static_cast<int>(kG489LeverPcSkip);
        if (std::strcmp(value, "blockrun") == 0)
            return static_cast<int>(kG490LeverBlockRun);
        if (std::strcmp(value, "rbfmt") == 0)
            return static_cast<int>(kG491LeverRbFmt);
        if (std::strcmp(value, "rbspin") == 0)
            return static_cast<int>(kG491LeverRbSpin);
        if (std::strcmp(value, "norender") == 0)
            return static_cast<int>(kG492LeverNoRender);
        if (std::strcmp(value, "noother") == 0)
            return static_cast<int>(kG492LeverNoOther);
        if (std::strcmp(value, "ct24reuse") == 0)
            return static_cast<int>(kG492LeverCt24Reuse);
        if (std::strcmp(value, "latedrain") == 0)
            return static_cast<int>(kG493LeverLateDrain);
        if (std::strcmp(value, "nosync") == 0)
            return static_cast<int>(kG494LeverNoSync);
        if (std::strcmp(value, "gpuslow") == 0)
            return static_cast<int>(kG496LeverGpuSlow);
        if (std::strcmp(value, "glstate") == 0)
            return static_cast<int>(kG496LeverGlState);
        if (std::strcmp(value, "winbuf") == 0)
            return static_cast<int>(kG497LeverWinBuf);
        if (std::strcmp(value, "ownpub") == 0)
            return static_cast<int>(kG498LeverOwnPub);
        if (std::strcmp(value, "blklower") == 0)
            return static_cast<int>(kG499LeverBlkLower);
        if (std::strcmp(value, "qpblock") == 0)
            return static_cast<int>(kG499LeverQpBlock);
        if (std::strcmp(value, "blkbranch") == 0)
            return static_cast<int>(kG500LeverBlkBranch);
        if (std::strcmp(value, "blksplit") == 0)
            return static_cast<int>(kG502LeverBlkSplit);
        if (std::strcmp(value, "blkchain") == 0)
            return static_cast<int>(kG502LeverBlkChain);
        if (std::strcmp(value, "blktail0") == 0)
            return static_cast<int>(kG502LeverBlkTail0);
        if (std::strcmp(value, "eeslow") == 0)
            return static_cast<int>(kG503LeverEeSlow);
        if (std::strcmp(value, "chainpool") == 0)
            return static_cast<int>(kG503LeverChainPool);
        if (std::strcmp(value, "histcur") == 0)
            return static_cast<int>(kG504LeverHistCur);
        if (std::strcmp(value, "qpseg") == 0)
            return static_cast<int>(kG506LeverQpSeg);
        if (std::strcmp(value, "lockskip") == 0)
            return static_cast<int>(kG508LeverLockSkip);
        if (std::strcmp(value, "capmove") == 0)
            return static_cast<int>(kG510LeverCapMove);
        if (std::strcmp(value, "clsmemo") == 0)
            return static_cast<int>(kG510LeverClsMemo);
        if (std::strcmp(value, "g510both") == 0)
            return static_cast<int>(kG510LeverBoth);
        if (std::strcmp(value, "texrun") == 0)
            return static_cast<int>(kG511LeverTexRun);
        if (std::strcmp(value, "texprobe") == 0)
            return static_cast<int>(kG511LeverTexProbe);
        if (std::strcmp(value, "clsprobe") == 0)
            return static_cast<int>(kG511LeverClsProbe);
        if (std::strcmp(value, "fastdec") == 0)
            return static_cast<int>(kG512LeverFastDec);
        std::fprintf(stderr,
                     "[G419:ab] unknown DC2_G419_AB='%s' (expected "
                     "narrow|pack|lanes|all|zero|packlanes|pool|all420|narrowtouch|matunpack|"
                     "vu1slow|gsslow|coalesce|earlykick|kickall|spinwait|wspin|spinboth|async1|"
                     "rb_all|rb_l2l|rb_resync|rb_display|uvpred|simdmerge|vnoptrace|pagebump|"
                     "leanfetch|pcreg|widevf0|flaglower|fastop|pcskip|blockrun|rbfmt|rbspin|"
                     "norender|noother|ct24reuse|latedrain|nosync|gpuslow|glstate|winbuf|ownpub|"
                     "blklower|qpblock|blkbranch|blksplit|blkchain|blktail0|eeslow|chainpool|"
                     "histcur|qpseg|lockskip|capmove|clsmemo|g510both|texrun|texprobe|clsprobe|"
                     "fastdec)\n",
                     value);
        return static_cast<int>(kG419LeverNone);
    }();
    return lever;
}

static const char *g419AbLeverName(int lever)
{
    switch (lever)
    {
    case kG419LeverNarrow: return "narrow";
    case kG419LeverPack: return "pack";
    case kG419LeverLanes: return "lanes";
    case kG419LeverAll: return "all";
    case kG420LeverZero: return "zero";
    case kG420LeverPLanes: return "packlanes";
    case kG420LeverPool: return "pool";
    case kG420LeverAll: return "all420";
    case kG420LeverNTouch: return "narrowtouch";
    case kG432LeverMatUnpack: return "matunpack";
    case kG438LeverVu1Slow: return "vu1slow";
    case kG438LeverGsSlow: return "gsslow";
    case kG439LeverCoalesce: return "coalesce";
    case kG445LeverEarlyKick: return "earlykick";
    case kG445LeverKickAll: return "kickall";
    case kG447LeverSpinWait: return "spinwait";
    case kG447LeverWSpin: return "wspin";
    case kG447LeverSpinBoth: return "spinboth";
    case kG452LeverAsyncOne: return "async1";
    case kG454LeverReadbackAll: return "rb_all";
    case kG454LeverReadbackL2l: return "rb_l2l";
    case kG454LeverReadbackResync: return "rb_resync";
    case kG454LeverReadbackDisplay: return "rb_display";
    case kG456LeverUvPredicate: return "uvpred";
    case kG473LeverSimdMerge: return "simdmerge";
    case kG475LeverVnopTrace: return "vnoptrace";
    case kG478LeverKickPool: return "kickpool";
    case kG476LeverPageBump: return "pagebump";
    case kG477LeverRunWrite: return "runwrite";
    case kG479LeverLeanFetch: return "leanfetch";
    case kG480LeverPktPool: return "pktpool";
    case kG480LeverPoolBoth: return "poolboth";
    case kG480LeverNopSkip: return "nopskip";
    case kG481LeverHotLower: return "hotlower";
    case kG482LeverMemoHash: return "memohash";
    case kG482LeverWCoal: return "wcoal";
    case kG482LeverNotifySkip: return "notifyskip";
    case kG482LeverBoth: return "g482both";
    case kG483LeverPcReg: return "pcreg";
    case kG484LeverWideVf0: return "widevf0";
    case kG487LeverFlagLower: return "flaglower";
    case kG488LeverFastOp: return "fastop";
    case kG489LeverPcSkip: return "pcskip";
    case kG490LeverBlockRun: return "blockrun";
    case kG491LeverRbFmt: return "rbfmt";
    case kG491LeverRbSpin: return "rbspin";
    case kG492LeverNoRender: return "norender";
    case kG492LeverNoOther: return "noother";
    case kG492LeverCt24Reuse: return "ct24reuse";
    case kG493LeverLateDrain: return "latedrain";
    case kG494LeverNoSync: return "nosync";
    case kG496LeverGpuSlow: return "gpuslow";
    case kG496LeverGlState: return "glstate";
    case kG497LeverWinBuf: return "winbuf";
    case kG498LeverOwnPub: return "ownpub";
    case kG499LeverBlkLower: return "blklower";
    case kG499LeverQpBlock: return "qpblock";
    case kG500LeverBlkBranch: return "blkbranch";
    case kG502LeverBlkSplit: return "blksplit";
    case kG502LeverBlkChain: return "blkchain";
    case kG502LeverBlkTail0: return "blktail0";
    case kG503LeverEeSlow: return "eeslow";
    case kG503LeverChainPool: return "chainpool";
    case kG504LeverHistCur: return "histcur";
    case kG506LeverQpSeg: return "qpseg";
    case kG508LeverLockSkip: return "lockskip";
    case kG510LeverCapMove: return "capmove";
    case kG510LeverClsMemo: return "clsmemo";
    case kG510LeverBoth: return "g510both";
    case kG511LeverTexRun: return "texrun";
    case kG511LeverTexProbe: return "texprobe";
    case kG511LeverClsProbe: return "clsprobe";
    case kG512LeverFastDec: return "fastdec";
    default: return "none";
    }
}

// DC2_G419_BALLAST=<repeats> — critical-path sensitivity probe. BOTH buckets run the control form;
// the candidate bucket additionally repeats the site's work <repeats> times. The repeat must be
// idempotent (it rewrites identical bytes), so the only thing measured is how much of a KNOWN added
// cost at that exact site reaches the frame.
static int g419AbBallast()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G419_BALLAST");
        if (value == nullptr)
            return 0;
        return std::max(0, std::min(16, std::atoi(value)));
    }();
    return n;
}

static uint64_t g419AbWarmFrames()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G419_WARM");
        if (value == nullptr)
            return 120;
        return std::max(0, std::min(6000, std::atoi(value)));
    }();
    return static_cast<uint64_t>(n);
}

// Blocked-estimator window length in ticks. Both arm means drift together over a run, so a window
// short enough that the drift is common to both buckets differences it away.
static uint64_t g419AbWindow()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G419_WINDOW");
        if (value == nullptr)
            return 600;
        return std::max(60, std::min(6000, std::atoi(value)));
    }();
    return static_cast<uint64_t>(n);
}

// Consumer-side arm selector. Returns the behaviour this call should use and records which bucket
// actually ran so the next tick can charge the interval to it.
//   * not the selected lever -> the promoted default, and nothing is recorded (zero instrument cost
//     on the shipping path beyond one cached-int compare);
//   * ballast mode -> the CONTROL form for both buckets (the extra work is added by the site).
// `all` alternates every lever at once, which is the promotion gate for the shipping stack: the three
// levers act on different threads, so only a combined arm answers "does the promoted default pay".
static bool g419AbLeverMatches(int lever)
{
    const int sel = g419AbLever();
    if (sel == lever)
        return true;
    // The two combined arms are each restricted to their OWN phase's levers. `all` must keep
    // meaning "the G419 shipping stack" so its promotion gate stays reproducible.
    if (sel == kG419LeverAll)
        return lever >= kG419LeverNarrow && lever <= kG419LeverLanes;
    if (sel == kG420LeverAll)
        return lever >= kG420LeverZero && lever <= kG420LeverPool;
    return false;
}

static bool g419AbArmFor(int lever, bool defaultOn)
{
    if (!g419AbLeverMatches(lever))
        return defaultOn;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    if (g419AbBallast() > 0)
        return false;
    return arm != 0u;
}

static bool g452AsyncSelected()
{
    return g419AbLever() == static_cast<int>(kG452LeverAsyncOne);
}

static bool g452AsyncArm()
{
    return g419AbArmFor(static_cast<int>(kG452LeverAsyncOne), false);
}

// G438: the derivative probes' arm selector, callable from the VU1-worker and GS-worker TUs (the
// rest of this instrument is static to the rasterizer TU). `which` is 0 for the VU1 spin, 1 for the
// GS spin. Returns -1 when that probe is not the selected lever (the caller then falls back to the
// legacy always-on DC2_G303_VU1_SLOW_US / DC2_G431_GS_SLOW_US behaviour), 0 on a control frame and
// 1 on a candidate frame.
//
// The `used` bit must reach the tick, but the spin sites run per-KICK and per-WINDOW (~1250 and
// ~1580 times/frame), and an unconditional atomic RMW on a line shared with the EE thread would be
// a per-call cross-core cost paid by BOTH arms — i.e. it would inflate the very baseline the probe
// is dividing by. The arm only changes once per frame, so a thread-local memo turns it into one
// atomic per thread per frame.
// G438: how many consecutive frames one arm is held for. 1 reproduces the original per-frame
// alternation exactly. DC2_G438_HOLD sets it; thread-level probes, including G456's GS-worker
// predicate, default to 16 so pipelined work does not leak into the opposite bucket.
static uint64_t g438ArmHoldFrames()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G438_HOLD");
        if (value != nullptr && value[0] != '\0')
            return static_cast<uint64_t>(std::max(1, std::min(600, std::atoi(value))));
        const int sel = g419AbLever();
        // G439's coalesce probe is a thread-level lever like the two spins (it changes the handoff
        // rate between the EE and the GS worker), so it inherits the block hold for the same
        // pipelined-runtime reason — see the g419AbTick comment on DC2_G438_HOLD.
        return (sel == kG438LeverVu1Slow || sel == kG438LeverGsSlow ||
                sel == kG456LeverUvPredicate || sel == kG473LeverSimdMerge ||
                sel == kG475LeverVnopTrace || sel == kG476LeverPageBump ||
                sel == kG477LeverRunWrite || sel == kG478LeverKickPool ||
                sel == kG487LeverFlagLower || sel == kG488LeverFastOp ||
                sel == kG489LeverPcSkip || sel == kG490LeverBlockRun ||
                sel == kG491LeverRbFmt || sel == kG491LeverRbSpin ||
                sel == kG492LeverNoRender || sel == kG492LeverNoOther ||
                sel == kG492LeverCt24Reuse || sel == kG493LeverLateDrain ||
                sel == kG494LeverNoSync || sel == kG496LeverGpuSlow ||
                sel == kG496LeverGlState || sel == kG497LeverWinBuf ||
                sel == kG498LeverOwnPub || sel == kG499LeverBlkLower ||
                sel == kG499LeverQpBlock || sel == kG500LeverBlkBranch ||
                sel == kG502LeverBlkSplit || sel == kG502LeverBlkChain ||
                sel == kG502LeverBlkTail0 || sel == kG503LeverEeSlow ||
                sel == kG503LeverChainPool || sel == kG504LeverHistCur ||
                sel == kG506LeverQpSeg || sel == kG508LeverLockSkip ||
                sel == kG510LeverCapMove || sel == kG510LeverClsMemo ||
                sel == kG510LeverBoth ||
                // G506's law: a new thread-level arm joins THIS predicate in the same edit that
                // adds it, or it silently alternates every frame while its peers hold 16.
                sel == kG511LeverTexRun || sel == kG511LeverTexProbe ||
                sel == kG511LeverClsProbe || sel == kG512LeverFastDec ||
                sel == kG439LeverCoalesce || sel == kG445LeverEarlyKick ||
                sel == kG445LeverKickAll || sel == kG447LeverSpinWait || sel == kG447LeverWSpin ||
                sel == kG447LeverSpinBoth || sel == kG452LeverAsyncOne ||
                (sel >= kG454LeverReadbackAll && sel <= kG454LeverReadbackDisplay))
                   ? 16ull
                   : 1ull;
    }();
    return n;
}

// Bumped once per frame by g419AbTick. The memo keys on this, NOT on the arm value: the LCG draws
// the same arm on consecutive frames about half the time, and keying on the value would then leave
// `used` clear and the tick would drop that frame as unattributable.
static std::atomic<uint64_t> s_g438TickSeq{0ull};

// G456 runs twice per sprite on the GS worker. Memo the attribution RMW once per
// frame; relaxed arm/sequence loads remain cheap and are paid by both paired arms.
int g456UvPredicateArm()
{
    if (g419AbLever() != static_cast<int>(kG456LeverUvPredicate))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G473 runs once in the GS worker's frame-boundary latch. A/B selection always forces the exact
// scalar/candidate pair. The exact SIMD path is promoted default-on; the rollback restores scalar.
int g473SimdMergeArm()
{
    if (g419AbLever() == static_cast<int>(kG473LeverSimdMerge))
        return g419AbArmFor(static_cast<int>(kG473LeverSimdMerge), false) ? 1 : 0;
    static const bool on = [] {
        if (std::getenv("DC2_G473_NO_SIMD_MERGE") != nullptr)
            return false;
        const char *value = std::getenv("DC2_G473_SIMD_MERGE");
        return value == nullptr || value[0] != '0';
    }();
    return on ? 1 : 0;
}

// G476: run-weighted page-generation walk. Returns 1 for the candidate (one weighted RMW per
// distinct page) and 0 for the control (the old per-px pair), or -1 when this lever is not selected
// so the sites fall through to their own default/rollback flag. Called ~10-20k/f on the GS worker,
// so it records the arm-used bit at most once per A/B tick like G475 rather than on every call.
int g476PageBumpArm()
{
    if (g419AbLever() != static_cast<int>(kG476LeverPageBump))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G477: swizzle-row hoisted IMAGE run writers. Returns 1 for the candidate (hoisted row + pair
// stores), 0 for the control (the G424 per-pixel run loop), or -1 when this lever is not selected
// so the writers fall through to their own DC2_G477_NO_FAST_RUN rollback. Called once per
// horizontal RUN on the GS worker, so the attribution RMW is recorded once per A/B tick.
int g477RunWriteArm()
{
    if (g419AbLever() != static_cast<int>(kG477LeverRunWrite))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G475 runs once per VU1 microprogram (~1,240/f). Memo the attribution RMW once per frame on
// the VU1 worker, mirroring G438, while returning the held arm for every run so one microprogram
// never changes loop copies mid-execution.
int g475VnopTraceArm()
{
    if (g419AbLever() != static_cast<int>(kG475LeverVnopTrace))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G478 runs once per VU1 KICK (~1,230/f) on the EE thread, which is where the item is acquired.
// Same once-per-frame memo as G475/G456: the arm is held for a block of frames, so an item
// acquired from the pool in a candidate frame is always released back into the same pool.
// G479 runs once per VU1 microprogram on the VU1 worker (~1,080/f). Same once-per-tick
// attribution memo as G475, and the arm is held for a block of frames so a run() never
// switches loop copies mid-execution.
int g479LeanFetchArm()
{
    if (g419AbLever() != static_cast<int>(kG479LeverLeanFetch))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G483 selects a different COPY of the pair loop (the one that carries the program counter in a
// loop-carried local instead of reloading `m_state.pc` through memory on every pair), so it obeys
// exactly the G479 rule above: read once per run(), never per pair, and held for a block of frames
// so a microprogram never switches loop copies mid-execution.
int g483PcRegArm()
{
    if (g419AbLever() != static_cast<int>(kG483LeverPcReg))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G484 selects the pair-loop copy that reaches the scalar-pipe / flag-pipe state through ONE
// hoisted base pointer instead of seven. Same rule as G479/G483: read once per run(), never per
// pair, and held for a block of frames so a microprogram never switches loop copies mid-execution.
int g484WideVf0Arm()
{
    if (g419AbLever() != static_cast<int>(kG484LeverWideVf0))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G480 is a TWO-THREAD lever: the VU1 worker acquires a recycled packet buffer (per XGKICK) and
// the GS worker returns it (per window). Both sides call this, and both must read the SAME arm for
// the whole hold block or a buffer acquired in one arm would be returned in the other — hence the
// G438 block hold (DC2_G438_HOLD=16) is mandatory for this gate, and the `used` attribution RMW is
// memoised once per A/B tick PER THREAD rather than taken on every packet.
int g480PktPoolArm()
{
    const int lever = g419AbLever();
    if (lever != static_cast<int>(kG480LeverPktPool) &&
        lever != static_cast<int>(kG480LeverPoolBoth))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G480 runs once per VU1 microprogram on the VU1 worker (~1,080/f) — the dispatch shape is chosen
// at run() entry, so the arm must be HELD for a block (G438) exactly like G475/G479.
int g480NopSkipArm()
{
    if (g419AbLever() != static_cast<int>(kG480LeverNopSkip))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G481 selects a loop dispatch shape at run() entry on the VU1 worker (~1,080 runs/f), exactly like
// G475/G479/G480's nopskip — the arm is HELD for a G438 block so no microprogram can change its
// dispatch mid-execution, and the attribution RMW is memoised once per A/B tick.
int g481HotLowerArm()
{
    if (g419AbLever() != static_cast<int>(kG481LeverHotLower))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G487 selects what `g422DecodeLowerKind` writes into the G410 pair cache, read once per run() on
// the VU1 worker (~1,230/f). An arm flip forces a full 2,048-slot re-decode of that cache, so the
// arm MUST be held for a G438 block (the default above adds this lever to the hold-16 set): at
// hold=1 the ~20 us rebuild would land on one arm's frame every frame and bias the gate.
int g487FlagLowerArm()
{
    if (g419AbLever() != static_cast<int>(kG487LeverFlagLower))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G488 selects which `upperKind` byte `g410SelectUpperKind` writes into the G410 pair cache, read
// once per run() on the VU1 worker (~1,230/f). Identical contract to g487FlagLowerArm: an arm flip
// forces a full 2,048-slot re-decode of that cache, so the arm MUST be held for a G438 block or
// the ~20 us rebuild would land on one arm's frame every frame and bias the gate.
int g488FastOpArm()
{
    if (g419AbLever() != static_cast<int>(kG488LeverFastOp))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G489 selects the VALUE of the `G489_PF_CANBRANCH` bit this cache's slots are decoded with, read
// once per run() on the VU1 worker (~1,230/f). Identical contract to g487FlagLowerArm /
// g488FastOpArm: a flip forces a full 2,048-slot re-decode, so the arm MUST be held for a G438
// block or the ~20 us rebuild would land on one arm's frame every frame and bias the gate.
int g489PcSkipArm()
{
    if (g419AbLever() != static_cast<int>(kG489LeverPcSkip))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G490 selects which LOOP COPY run() enters, read once per run() on the VU1 worker (~1,230/f).
// Same G438 block hold and the same per-frame-sequence memo as every other thread-level arm: the
// memo keys on the tick SEQUENCE, never on the arm value (the LCG draws the same arm on
// consecutive frames about half the time, and a value-keyed memo would stop marking the frame).
int g490BlockRunArm()
{
    if (g419AbLever() != static_cast<int>(kG490LeverBlockRun))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G499 selects which of the two noinline block bodies run() calls, read once per run() on the VU1
// worker (~1,238/f). Same G438 block hold and the same per-frame-sequence memo as g490BlockRunArm
// directly above: the arm must not flip inside a run(), and the memo keys on the tick SEQUENCE
// rather than on the arm value.
int g499BlkLowerArm()
{
    if (g419AbLever() != static_cast<int>(kG499LeverBlkLower))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G499 Q/P-residency arm. Same contract as g499BlkLowerArm directly above; its control arm is the
// `blklower` candidate, so the two levers of this phase are priced independently.
int g499QpBlockArm()
{
    if (g419AbLever() != static_cast<int>(kG499LeverQpBlock))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G500 control-transfer-tail arm. Same contract as the two directly above; its control arm is the
// `blklower` candidate (the shipped G499 body), so the tail is priced alone.
int g500BlkBranchArm()
{
    if (g419AbLever() != static_cast<int>(kG500LeverBlkBranch))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G502 block-round-trip CEILING PROBE arm. Same contract as the four directly above: read once per
// run() on the VU1 worker, per-frame-sequence memo, G438 block hold. The candidate arm halves every
// long block, so the arms differ only in how many times run()'s loop head is re-entered.
int g502BlkSplitArm()
{
    if (g419AbLever() != static_cast<int>(kG502LeverBlkSplit))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G502 block-CHAIN arm — the lever the probe above priced OUT (see the fix log §4). Kept as
// scaffolding beside a body that is compiled out, the DC2_G479_QPREG / DC2_G485_HAZREG convention.
int g502BlkChainArm()
{
    if (g419AbLever() != static_cast<int>(kG502LeverBlkChain))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G502 TAIL-0 admission arm. Same contract as the four above: read once per run() on the VU1
// worker, per-frame-sequence memo, G438 block hold.
int g502BlkTail0Arm()
{
    if (g419AbLever() != static_cast<int>(kG502LeverBlkTail0))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G504 history-CURSOR arm. Same contract as the five above: read once per run() on the VU1 worker
// (~1,236/frame, not per pair), per-frame-sequence memo, G438 block hold. Selects arm 6 against
// arm 3 in g499BlkArmRun().
int g504HistCurArm()
{
    if (g419AbLever() != static_cast<int>(kG504LeverHistCur))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G506 segmented Q/P-advance arm. Same contract as the six above: read once per run() on the VU1
// worker (~1,236/frame, not per pair), per-frame-sequence memo, G438 block hold. Selects arm 7
// against arm 3 in g499BlkArmRun().
int g506QpSegArm()
{
    if (g419AbLever() != static_cast<int>(kG506LeverQpSeg))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G491 readback-format arm, read once per colour readback on the GL WORKER thread (6/f). The
// per-frame memo below marks the frame from that thread — which is correct and necessary: the arm
// must be attributed on whichever thread consumes it, and 6 relaxed RMWs per frame is nothing
// (17a law 7c's instrument-cost corollary bites at thousands per frame, not six).
int g491RbFmtArm()
{
    if (g419AbLever() != static_cast<int>(kG491LeverRbFmt))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G491 readback-spin arm, read once per colour readback on the GS WORKER thread (6/f). Candidate
// arm (1) = do NOT spin on this edge.
int g491RbSpinArm()
{
    if (g419AbLever() != static_cast<int>(kG491LeverRbSpin))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G492 ceiling-probe arms, read once per backend item on the GS worker (~34 and ~20 per frame).
// Candidate arm (1) = DELETE the round trip at that edge. Same per-frame memo as every other
// thread-level arm; the memo keys on the tick SEQUENCE, never on the arm value.
int g492NoRenderArm()
{
    if (g419AbLever() != static_cast<int>(kG492LeverNoRender))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

int g492NoOtherArm()
{
    if (g419AbLever() != static_cast<int>(kG492LeverNoOther))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G492 CT24 output-storage reuse arm, read once per CT24 view build on the GL WORKER thread (1/f).
// Candidate arm (1) = REUSE the existing allocation. Both arms produce a bit-identical view.
int g492Ct24ReuseArm()
{
    if (g419AbLever() != static_cast<int>(kG492LeverCt24Reuse))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G493 late-drain arm, read on the GS-worker/front-end thread at the flush top and again at the
// flush's submit point (~34 + ~18 per frame). Candidate arm (1) = drain LATE, i.e. hand the pending
// async render the whole of this flush's preparation to overlap with. Same per-frame thread-local
// memo as every other thread-level arm; the memo keys on the tick SEQUENCE, never on the arm value.
int g493LateDrainArm()
{
    if (g419AbLever() != static_cast<int>(kG493LeverLateDrain))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G494 sync-query elision arm, read on the GL BACKEND WORKER thread several times per backend item
// (~110 reads/frame). Candidate arm (1) = elide the validation queries. Same per-frame thread-local
// tick-sequence memo as every other thread-level arm — never key the memo on the arm VALUE (G438).
int g494NoSyncArm()
{
    if (g419AbLever() != static_cast<int>(kG494LeverNoSync))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G496 GPU derivative-probe arm, read once per render batch on the GL BACKEND WORKER thread
// (~34 reads/frame). Candidate arm (1) = re-issue the batch's geometry with an identity blend, i.e.
// inject GPU work that changes no byte. Same per-frame thread-local tick-sequence memo as every
// other thread-level arm — never key the memo on the arm VALUE (G438).
int g496GpuSlowArm()
{
    if (g419AbLever() != static_cast<int>(kG496LeverGpuSlow))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G508 lock-skip arm, read once per GIF PACKET on the GS WORKER thread (~4,465 reads/frame against
// the ~55.8k register writes the lever acts on, so the accessor is ~1.3% of the sites it gates and
// cannot pollute the measured path — G502's per-item-census bias, avoided by construction).
// Candidate arm (1) = raise the thread-local depth so nested writeRegister guards skip the
// already-owned recursive re-acquisition. Control arm (0) = leave it at 0, i.e. the pre-G508 path.
// Same per-frame thread-local tick-sequence memo as every other thread-level arm — never key the
// memo on the arm VALUE (G438).
int g508LockSkipArm()
{
    if (g419AbLever() != static_cast<int>(kG508LeverLockSkip))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G510 capture-move arm, read once per CAPTURED PRIMITIVE on the GS WORKER thread (~9,900/frame).
// That is the same rate as the site it gates, so the accessor must be as close to free as an arm
// accessor can be: after the once-per-frame attribution RMW it is two relaxed loads, a compare and a
// predicted branch, and it sits on BOTH arms identically, so it adds a constant to each bucket and
// cannot bias the difference (G486 §4's asymmetry trap does not apply — no arm skips it).
// Candidate arm (1) = move the entry into g_g144List; control arm (0) = the pre-G510 copy.
// Never key the per-frame memo on the arm VALUE (G438/G501).
int g510CapMoveArm()
{
    const int sel = g419AbLever();
    if (sel != static_cast<int>(kG510LeverCapMove) && sel != static_cast<int>(kG510LeverBoth))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G510 classify-memo arm, read once per GPU FLUSH on the GS WORKER thread (~33.4/frame against the
// ~9,900 entries the lever acts on, so the accessor is 0.3% of the sites it gates and cannot pollute
// the measured loop — G502's per-item-census bias, avoided by construction).
// Candidate arm (1) = reuse the previous entry's classification when the state bytes match.
int g510ClsMemoArm()
{
    const int sel = g419AbLever();
    if (sel != static_cast<int>(kG510LeverClsMemo) && sel != static_cast<int>(kG510LeverBoth))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G510 shipped-default predicates. The A/B arm always wins when this phase's gate is armed, so the
// paired measurement is valid whichever way the default points; the env flags are the standalone
// rollback / force pair every promoted lever in this arc carries.
bool g510CapMoveActive()
{
    const int arm = g510CapMoveArm(); // -1 unless DC2_G419_AB=capmove|g510both
    if (arm >= 0)
        return arm == 1;
    static const bool off = (std::getenv("DC2_G510_NO_CAPMOVE") != nullptr);
    return !off;
}

bool g510ClsMemoActive()
{
    const int arm = g510ClsMemoArm(); // -1 unless DC2_G419_AB=clsmemo|g510both
    if (arm >= 0)
        return arm == 1;
    static const bool off = (std::getenv("DC2_G510_NO_CLSMEMO") != nullptr);
    return !off;
}

// G510 census (default OFF, DC2_G510_CENSUS=1): entries classified per frame and how many of them
// the memo could serve. Behaviour-pure — it counts on BOTH arms and on the default path, so the
// hit rate it reports is a property of the route, not of the lever.
bool g510CensusOn()
{
    static const bool on = (std::getenv("DC2_G510_CENSUS") != nullptr);
    return on;
}
std::atomic<uint64_t> g_g510ClsEntries{0};
std::atomic<uint64_t> g_g510ClsEligible{0};
std::atomic<uint64_t> g_g510ClsServed{0};
std::atomic<uint64_t> g_g510Flushes{0};

// G511 texture-pass run-length arm, read once per GPU FLUSH on the GS WORKER thread (~33.4/frame
// against the ~9,900 entries the lever acts on — 0.3% of the sites it gates, so the accessor cannot
// pollute the measured loop; G502's per-item-census bias, avoided by construction).
// Candidate arm (1) = skip the `s_batchKeys` hash probe when this entry's effective texture key is
// the one the previous iteration already proved present in the set.
int g511TexRunArm()
{
    if (g419AbLever() != static_cast<int>(kG511LeverTexRun))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G511 INFLATION probe arm — same rate and same accessor shape as above. Arm 1 adds one extra
// `s_batchKeys.count()` per probing entry; arm 0 is the untouched loop. This is a price, not a
// lever: it is never promoted and has no env force flag.
int g511TexProbeArm()
{
    if (g419AbLever() != static_cast<int>(kG511LeverTexProbe))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G511: NOT PROMOTED — default OFF, opt-in with DC2_G511_TEXRUN=1.
// The lever is exact and removes real work, but G511 §4 PRICED the work it removes with an
// inflation probe first: adding 5,264 extra s_batchKeys probes per frame moved the frame by
// -0.102 +/- 0.058 ms (t = -1.26, i.e. the arm doing MORE work measured FASTER), bounding one probe
// at <= 2.7 ns and the whole vein at <= 0.014 ms/f. Its own 4-run gate then read null in both
// estimators and sign-inconsistent 2/4, exactly as the probe predicted. A neutral change is not
// promoted: it would add a live value and a branch to the pole thread's hottest loop for a saving
// the instrument cannot see. It is kept, gated, so a future route (§8) can re-test it in one run.
bool g511TexRunActive()
{
    const int arm = g511TexRunArm(); // -1 unless DC2_G419_AB=texrun
    if (arm >= 0)
        return arm == 1;
    static const bool on = (std::getenv("DC2_G511_TEXRUN") != nullptr);
    return on;
}

// The inflation probe has NO default-on path — it only ever runs as the candidate arm of its own
// paired gate, so a leaked env flag cannot put it under a later measurement.
bool g511TexProbeActive()
{
    return g511TexProbeArm() == 1;
}

// G511 INFLATION probe #2 arm — read once per GPU FLUSH on the GS WORKER thread, same shape as the
// two above. Arm 1 repeats the G510 state-identity comparison once more per graph entry.
int g511ClsProbeArm()
{
    if (g419AbLever() != static_cast<int>(kG511LeverClsProbe))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

bool g511ClsProbeActive()
{
    return g511ClsProbeArm() == 1;
}

// G511 census (default OFF, DC2_G511_CENSUS=1): how many texture-pass iterations reach the set
// probe and how many of those repeat the key the previous iteration already resolved. Behaviour-
// pure — it counts on BOTH arms and on the default path, so the rate it reports is a property of
// the route, not of the lever. Its own sites are three relaxed adds per FLUSH, not per entry
// (G502's per-item-census bias: a per-entry counter here would inflate the very loop it prices).
bool g511CensusOn()
{
    static const bool on = (std::getenv("DC2_G511_CENSUS") != nullptr);
    return on;
}
std::atomic<uint64_t> g_g511TexEntries{0};  // iterations of the texture pass
std::atomic<uint64_t> g_g511TexProbes{0};   // of those, iterations reaching the set probe
std::atomic<uint64_t> g_g511TexRunRep{0};   // of those, iterations the run-length memo can serve
std::atomic<uint64_t> g_g511TexFlushes{0};

// G511 texture-pass DECOMPOSITION census (default OFF, DC2_G511_TEXSPLIT=1) → [G511:texsplit].
// §2 puts `tex` at 1.44 ms/f, the largest of the five pre-passes, and §4 proved the per-entry hash
// probe inside it is worth <= 0.014 ms/f — so the bucket's cost is somewhere else and nobody knows
// where. This splits it into the two HEAVY per-KEY sites (whole-texture decode, source-page hash)
// and leaves the rest as a stated residual.
// Clock pairs go ONLY on sites that run ~24x/frame, never on the ~9,900-iteration loop body: a
// per-entry clock pair would cost more than the thing it measures (G502).
bool g511TexSplitOn()
{
    static const bool on = (std::getenv("DC2_G511_TEXSPLIT") != nullptr);
    return on;
}
std::atomic<uint64_t> g_g511TexLoopNs{0};    // whole texture pass, one pair per flush
std::atomic<uint64_t> g_g511TexHashNs{0};    // g178HashPages
std::atomic<uint64_t> g_g511TexHashN{0};
std::atomic<uint64_t> g_g511TexDecodeNs{0};  // g178DecodeWhole (+ the upload buffer resize)
std::atomic<uint64_t> g_g511TexDecodeN{0};
std::atomic<uint64_t> g_g511TexGenNs{0};     // g178GenSumRect pair + registry lookup
std::atomic<uint64_t> g_g511TexGenN{0};
// Pixel counts for the two heavy sites. G494's law: a per-item cost that does NOT scale with the
// item's size is fixed OVERHEAD, not work — and the lever for overhead is different from the lever
// for work. ns/px separates them; nothing else in this arc can.
std::atomic<uint64_t> g_g511TexHashPx{0};
std::atomic<uint64_t> g_g511TexDecodePx{0};
std::atomic<uint64_t> g_g511TexSplitFlushes{0};
// Sink for the inflation probe. Written with a relaxed RMW once per FLUSH (never per entry) so the
// per-entry accumulation stays a register and the compiler still cannot prove the extra probes dead.
std::atomic<uint64_t> g_g511Sink{0};

// ---------------------------------------------------------------------------------------------
// G512 — fast whole-texture decode.
//
// Read ONCE per g178DecodeWhole call (4/frame) and passed down to the row-pool lanes as a plain
// bool, so the ~326,000 texels it gates never see the accessor (G502's per-item-census bias,
// avoided by construction). Same per-frame thread-local tick-sequence memo as every other
// thread-level arm.
int g512FastDecArm()
{
    if (g419AbLever() != static_cast<int>(kG512LeverFastDec))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G504's law: gate per-arm BEHAVIOUR on the arm VALUE, never on `arm != 0` alone — and keep the
// env rollback on the SAME accessor so a paired run and a forced run cannot disagree about which
// body ran. Default ON once promoted; DC2_G512_NO_FASTDEC=1 restores the pre-G512 per-texel body.
bool g512FastDecActive()
{
    const int arm = g512FastDecArm(); // -1 unless DC2_G419_AB=fastdec
    if (arm >= 0)
        return arm == 1;
    static const bool off = (std::getenv("DC2_G512_NO_FASTDEC") != nullptr);
    return !off;
}

// G512 exactness oracle (default OFF, DC2_G512_VERIFY=1). Decodes the texture BOTH ways into two
// buffers and counts mismatching texels. The reference is the pre-G512 per-texel body, kept
// verbatim in the same function — DC2_G298_VERIFY_TEXDECODE cannot serve here because it compares
// the parallel decode against the SAME body, so it would agree with a wrong fast path.
bool g512VerifyOn()
{
    static const bool on = (std::getenv("DC2_G512_VERIFY") != nullptr);
    return on;
}

// G512 census (default OFF, DC2_G512_PSM=1) -> [G512:psm]. Which pixel formats the 4 decodes/frame
// actually are, and how many texels each accounts for. G511 §9: `avgPx(decode=81601)` is a property
// of MAP-0's texture set, and this phase's price rides on the psm mix as well as the pixel count.
// Behaviour-pure: it counts on BOTH arms and on the default path.
bool g512PsmCensusOn()
{
    static const bool on = (std::getenv("DC2_G512_PSM") != nullptr);
    return on;
}
static std::atomic<uint64_t> s_g512PsmCalls[64] = {};
static std::atomic<uint64_t> s_g512PsmPx[64] = {};
static std::atomic<uint64_t> s_g512PsmFast{0};
static std::atomic<uint64_t> s_g512PsmSlow{0};

// Forward-declared in g512_fast_texdecode.inc (that file is included from
// rasterizer_command_graph.inc, ~50 lines earlier in this TU).
void g512PsmNote(uint8_t psm, uint64_t pixels, bool fast, int texW, int texH, int lanes)
{
    s_g512PsmCalls[psm & 63u].fetch_add(1u, std::memory_order_relaxed);
    s_g512PsmPx[psm & 63u].fetch_add(pixels, std::memory_order_relaxed);
    const uint64_t n =
        (fast ? s_g512PsmFast : s_g512PsmSlow).fetch_add(1u, std::memory_order_relaxed) + 1u;
    if ((n % 256u) != 0u)
        return;
    char buf[640];
    int off = std::snprintf(buf, sizeof(buf), "[G512:psm] fast=%llu ref=%llu",
                            static_cast<unsigned long long>(
                                s_g512PsmFast.load(std::memory_order_relaxed)),
                            static_cast<unsigned long long>(
                                s_g512PsmSlow.load(std::memory_order_relaxed)));
    for (uint32_t k = 0; k < 64u && off > 0 && off < static_cast<int>(sizeof(buf)) - 64; ++k)
    {
        const uint64_t c = s_g512PsmCalls[k].load(std::memory_order_relaxed);
        if (c == 0u)
            continue;
        off += std::snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                             " psm%02x(n=%llu px=%llu)", k, static_cast<unsigned long long>(c),
                             static_cast<unsigned long long>(
                                 s_g512PsmPx[k].load(std::memory_order_relaxed)));
    }
    std::fprintf(stderr, "%s last=%dx%d lanes=%d\n", buf, texW, texH, lanes);
}

// G496b GL-state-elision arm, read once per render batch on the GL BACKEND WORKER thread
// (~34 reads/frame). Candidate arm (1) = elide GL calls that set the value GL already holds. Same
// per-frame thread-local tick-sequence memo as every other thread-level arm.
int g496GlStateArm()
{
    if (g419AbLever() != static_cast<int>(kG496LeverGlState))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G492 WORKLOAD INVARIANT (17a law 22). A ceiling probe that deletes work in an incorrect-output
// arm is only a price if the GUEST ran the same amount of work on both arms. This counts, per arm
// and inside the same run, every entry into all five backend edges plus the render batches' draw
// count; `[G492:inv]` prints both arms beside every `[G419:ab]` window. The non-target edges, the
// draw count and the CPU-replay fallback count must MATCH across arms for the ceiling to be
// admissible. Called only while a G492 probe (or DC2_G492_INV=1) is armed — ~73 relaxed adds per
// frame then, and literally nothing on the shipping path.
// Bucket index follows the tick's convention: [0] = candidate (arm != 0), [1] = control.
static std::atomic<uint64_t> g_g492Edge[2][5];
static std::atomic<uint64_t> g_g492Skip[2][5];
static std::atomic<uint64_t> g_g492Draws[2];

void g492NoteEdge(int edge, unsigned long long draws, unsigned long long skipped)
{
    if (edge < 0 || edge >= 5)
        return;
    const int a = g_g419AbArm.load(std::memory_order_relaxed) != 0u ? 0 : 1;
    g_g492Edge[a][edge].fetch_add(1ull, std::memory_order_relaxed);
    if (skipped != 0ull)
        g_g492Skip[a][edge].fetch_add(skipped, std::memory_order_relaxed);
    if (draws != 0ull)
        g_g492Draws[a].fetch_add(draws, std::memory_order_relaxed);
}

// G482 price-only arm, read once per geometry kick on the VU1 worker (~1,230/f). Same G438 block
// hold as every other thread-level arm: the memo key is a per-kick cost, so the arm must not
// alternate inside a pipelined window or the injected work leaks across arms (G438).
int g482MemoHashArm()
{
    if (g419AbLever() != static_cast<int>(kG482LeverMemoHash))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G482 write-coalescing arm. Read on the EE thread once per noteVuWrite (~59/kick); the G438 block
// hold guarantees every write belonging to one kick sees the same arm, so a kick's replay is never
// half-coalesced.
int g482WriteCoalesceArm()
{
    const int lever = g419AbLever();
    if (lever != static_cast<int>(kG482LeverWCoal))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G482 publish-notify-skip arm, read once per kick on the VU1 worker.
int g482NotifySkipArm()
{
    const int lever = g419AbLever();
    if (lever != static_cast<int>(kG482LeverNotifySkip) && lever != static_cast<int>(kG482LeverBoth))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

int g478KickPoolArm()
{
    const int lever = g419AbLever();
    if (lever != static_cast<int>(kG478LeverKickPool) &&
        lever != static_cast<int>(kG480LeverPoolBoth))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G497 per-window handoff-buffer recycling arm. Read on BOTH sides of the handoff — the EE thread
// in onWindowHandoff and the GS worker in collectWindowPackets — so the `used` memo is per-thread
// (fetch_or from two threads is fine; each marks the frame once). The CONTROL arm is byte-identical
// to the pre-G497 path: it neither takes from the free list nor feeds it, so a candidate frame's
// recycled buffers can never subsidise a control frame.
int g497WinBufArm()
{
    if (g419AbLever() != static_cast<int>(kG497LeverWinBuf))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G498 owner-publish arm. Read once per publication (twice per frame) on the GS worker, so the
// `used` memo costs one relaxed load and, at most, one atomic per thread per frame. The CONTROL arm
// is the untouched per-pixel loop, so a candidate publication can never subsidise a control one:
// both write the same VRAM bytes and neither leaves state behind (G418's page table is keyed on
// (block,width) and is rebuilt on demand, so a control frame that skips it costs nothing later).
int g498OwnPubArm()
{
    if (g419AbLever() != static_cast<int>(kG498LeverOwnPub))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

bool g438SlowLeverSelected(int which);
int g438SlowArm(int which)
{
    const int want = (which == 0) ? static_cast<int>(kG438LeverVu1Slow)
                                  : static_cast<int>(kG438LeverGsSlow);
    if (g419AbLever() != want)
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G503: the EE derivative probe's arm selector, called from the guest-frame hook in
// dc2_game_override.cpp (the EE thread). Same contract as g438SlowArm: -1 when the probe is not the
// selected lever (the site then honours the always-on DC2_G503_EE_SLOW_US value), 0 on a control
// frame, 1 on a candidate frame. The site runs ONCE per guest frame, so unlike g439CoalesceArm the
// `used` RMW could have been unconditional; it keys on the tick sequence anyway so the two probes
// read identically.
int g503EeSlowArm()
{
    if (g419AbLever() != static_cast<int>(kG503LeverEeSlow))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G503: the recycled-chain-buffer arm selector, called from the memory TU on the EE thread. Same
// contract as g438SlowArm: -1 when not the selected lever (the site then honours the shipped default
// / DC2_G503_NO_CHAINPOOL), 0 on a control frame, 1 on a candidate frame. Called a handful of times
// per frame (once per DMA chain plus twice in processPendingTransfers), so the `used` RMW is memoed
// on the tick sequence like every other selector rather than run unconditionally.
int g503ChainPoolArm()
{
    if (g419AbLever() != static_cast<int>(kG503LeverChainPool))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

bool g438SlowLeverSelected(int which)
{
    const int want = (which == 0) ? static_cast<int>(kG438LeverVu1Slow)
                                  : static_cast<int>(kG438LeverGsSlow);
    return g419AbLever() == want;
}

// G439: the window-coalescing ceiling probe's arm selector, called from ps2_gif_arbiter.cpp on the
// EE thread. Same contract and same per-frame memo as g438SlowArm: -1 when the probe is not the
// selected lever (the site then honours the always-on DC2_G439_COALESCE value), 0 on a control
// frame, 1 on a candidate frame. The call site is GifArbiter::drain(), ~1,600 times per frame, so
// the `used` bit must not be an unconditional cross-core RMW.
int g439CoalesceArm()
{
    if (g419AbLever() != static_cast<int>(kG439LeverCoalesce))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G445: the G418 early-command-kick arm selector, called from the GL backend worker TU
// (lle_gpu_raster_backend.inc). Same contract and same per-frame memo as g438SlowArm: -1 when the
// probe is not the selected lever (the site then honours the always-on DC2_G418_EARLY_KICK value),
// 0 on a control frame, 1 on a candidate frame. The site runs once per skip-readback render batch
// (~33/frame), so the `used` bit must not be an unconditional cross-core RMW.
int g445KickArm()
{
    if (g419AbLever() != static_cast<int>(kG445LeverEarlyKick))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G447 spin-then-block arm selector. Same contract and same per-frame memo as g445KickArm; the site
// is every synchronous backend entry point on the GS worker (~74 items/frame).
int g447SpinArm()
{
    const int sel447 = g419AbLever();
    if (sel447 != static_cast<int>(kG447LeverSpinWait) &&
        sel447 != static_cast<int>(kG447LeverSpinBoth))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G447 worker-side spin arm selector. Same contract and same per-frame memo as g447SpinArm; the
// site is the GL backend worker's pre-sleep path (~74 sleeps/frame).
int g447WorkerSpinArm()
{
    const int sel447w = g419AbLever();
    if (sel447w != static_cast<int>(kG447LeverWSpin) &&
        sel447w != static_cast<int>(kG447LeverSpinBoth))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}

// G445 extension probe arm selector. Same contract and same per-frame memo as g445KickArm; the site
// is the GL backend worker loop, once per copy/composite/upload item (~33/frame).
int g445KickAllArm()
{
    if (g419AbLever() != static_cast<int>(kG445LeverKickAll))
        return -1;
    const uint32_t arm = g_g419AbArm.load(std::memory_order_relaxed);
    const uint64_t seq = s_g438TickSeq.load(std::memory_order_relaxed);
    static thread_local uint64_t s_lastSeq = 0xFFFFFFFFFFFFFFFFull;
    if (s_lastSeq != seq)
    {
        s_lastSeq = seq;
        g_g419AbUsed.fetch_or(arm != 0u ? 2u : 1u, std::memory_order_relaxed);
    }
    return arm != 0u ? 1 : 0;
}


// How many extra idempotent repetitions this frame's site should perform (ballast mode only).
static int g419AbBallastReps(int lever)
{
    const int n = g419AbBallast();
    if (n <= 0 || !g419AbLeverMatches(lever))
        return 0;
    return g_g419AbArm.load(std::memory_order_relaxed) != 0u ? n : 0;
}

// Frames observed at the instrument's sample point. Counted unconditionally (one increment per
// frame) so every G419 per-batch census can be normalised to a real per-frame figure. Guessing the
// divisor is how a per-batch cost turns into a wrong payoff prediction: DC2 issues ~1.7 uploads of
// fbp=0x139 per frame, not one, so a plausible-looking "one per frame" assumption was off by 70%.
static uint64_t s_g419FrameTicks = 0u;

static void g419NoteFrameTick()
{
    ++s_g419FrameTicks;
}

// Once-per-frame tick at the colour-readback unpack. Charges the elapsed interval to whichever arm a
// consumer reported, then draws the next frame's arm.
static void g419AbTick()
{
    if (g419AbLever() == kG419LeverNone)
        return;
    static std::chrono::steady_clock::time_point s_last{};
    static bool s_haveLast = false;
    static uint64_t s_frame = 0u;
    static uint64_t s_lcg = 0x9E3779B97F4A7C15ull;
    // [0] = candidate, [1] = control.
    static double s_sum[2]{}, s_sq[2]{};
    static uint64_t s_n[2]{};
    static double s_winSum[2]{};
    static uint64_t s_winN[2]{};
    static double s_blkSum = 0.0, s_blkSq = 0.0;
    static uint64_t s_blkN = 0u, s_blkFav = 0u, s_dropped = 0u;
    static uint64_t s_usedCounts[4]{};
    // G438: frames whose interval spans an arm CHANGE. See g438ArmHoldFrames below.
    static bool s_skipNext = false;
    static uint64_t s_transition = 0u;

    // G492 workload invariant: charge this frame (and the flush/fallback counters it produced) to
    // the arm it actually ran under. Read BEFORE the redraw below — g_g419AbArm still holds the arm
    // of the frame that just ended.
    static uint64_t s_g492Frames[2]{};
    static uint64_t s_g492Flush[2]{}, s_g492Fb[2]{};
    static uint64_t s_g492LastFlush = 0ull, s_g492LastFb = 0ull;
    {
        const int a492 = g_g419AbArm.load(std::memory_order_relaxed) != 0u ? 0 : 1;
        const uint64_t fl = g_g178Flushes.load(std::memory_order_relaxed);
        const uint64_t fb = g_g178Fallbacks.load(std::memory_order_relaxed);
        ++s_g492Frames[a492];
        s_g492Flush[a492] += fl - s_g492LastFlush;
        s_g492Fb[a492] += fb - s_g492LastFb;
        s_g492LastFlush = fl;
        s_g492LastFb = fb;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint32_t used = g_g419AbUsed.exchange(0u, std::memory_order_relaxed);
    ++s_usedCounts[used & 3u];
    const uint64_t idx = s_frame++;
    const bool skipThis = s_skipNext;
    s_skipNext = false;
    if (skipThis)
        ++s_transition;
    if (s_haveLast && !skipThis && idx >= g419AbWarmFrames())
    {
        const double ms = std::chrono::duration<double, std::milli>(now - s_last).count();
        // An interval with no consumer (used==0) or with both arms observed (used==3) cannot be
        // attributed. Frame periods are wall-clock, so clip the disc-read / transition outliers that
        // no mean survives (same 200 ms clip G418 used against this route's ~66 ms steady frame).
        if ((used == 1u || used == 2u) && ms < 200.0)
        {
            const int a = (used == 2u) ? 0 : 1;
            s_sum[a] += ms;
            s_sq[a] += ms * ms;
            ++s_n[a];
            s_winSum[a] += ms;
            ++s_winN[a];
        }
        else
        {
            ++s_dropped;
        }
    }
    s_last = now;
    s_haveLast = true;

    // G438: hold the arm for a block of frames instead of redrawing every frame. A per-frame draw
    // is right for a lever whose whole cost lands inside the frame that ran it, but this runtime
    // pipelines frames across threads (G412 one-successor cross-frame pipelining), so work injected
    // on a candidate frame partly lands in the NEXT frame period. With alternation every frame that
    // successor is a control frame half the time, which drains the measured difference towards zero:
    // the vu1slow probe read +0.21 ms for 6.25 ms/f of injected spin (sensitivity 0.03) against
    // 0.44-0.98 from the cross-run form of the same probe. Holding the arm for N frames confines
    // the smear to the ONE boundary frame per block, which is then dropped outright.
    const uint64_t hold = g438ArmHoldFrames();
    if (hold <= 1u || (idx % hold) == 0u)
    {
        s_lcg = s_lcg * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t next = ((s_lcg >> 33u) & 1ull) != 0ull ? 1u : 0u;
        if (hold > 1u && next != g_g419AbArm.load(std::memory_order_relaxed))
            s_skipNext = true;
        g_g419AbArm.store(next, std::memory_order_relaxed);
    }
    // Publish the frame identity AFTER the arm, so a worker that observes the new seq also observes
    // the arm that belongs to it.
    s_g438TickSeq.fetch_add(1ull, std::memory_order_relaxed);

    const uint64_t window = g419AbWindow();
    if (idx == 0u || (idx % window) != 0u)
        return;
    // Close the blocked window. Require a real paired sample count in both buckets so a window the
    // route spent loading or stalled in cannot contribute a spurious difference.
    if (s_winN[0] >= 8u && s_winN[1] >= 8u)
    {
        const double d = s_winSum[0] / static_cast<double>(s_winN[0]) -
                         s_winSum[1] / static_cast<double>(s_winN[1]);
        s_blkSum += d;
        s_blkSq += d * d;
        ++s_blkN;
        if (d < 0.0)
            ++s_blkFav;
    }
    s_winSum[0] = s_winSum[1] = 0.0;
    s_winN[0] = s_winN[1] = 0u;

    const auto mean = [](double sum, uint64_t n) {
        return n != 0u ? sum / static_cast<double>(n) : 0.0;
    };
    const double c = mean(s_sum[0], s_n[0]), k = mean(s_sum[1], s_n[1]);
    const auto sem = [&](int a, double m) {
        const uint64_t n = s_n[a];
        if (n < 2u)
            return 0.0;
        const double var =
            (s_sq[a] - static_cast<double>(n) * m * m) / static_cast<double>(n - 1u);
        return var > 0.0 ? std::sqrt(var / static_cast<double>(n)) : 0.0;
    };
    const double sc = sem(0, c), sk = sem(1, k);
    double blkMean = 0.0, blkSe = 0.0, blkT = 0.0;
    if (s_blkN >= 2u)
    {
        blkMean = s_blkSum / static_cast<double>(s_blkN);
        const double var = (s_blkSq - static_cast<double>(s_blkN) * blkMean * blkMean) /
                           static_cast<double>(s_blkN - 1u);
        blkSe = var > 0.0 ? std::sqrt(var / static_cast<double>(s_blkN)) : 0.0;
        blkT = blkSe > 0.0 ? blkMean / blkSe : 0.0;
    }
    std::fprintf(stderr,
                 "[G419:ab] lever=%s ballast=%d hold=%llu frames=%llu drop=%llu trans=%llu "
                 "used(0=%llu 1=%llu 2=%llu 3=%llu) "
                 "cand(n=%llu ms=%.3f se=%.3f) ctrl(n=%llu ms=%.3f se=%.3f) "
                 "delta=%.3f (%.2f%%) seDelta=%.3f "
                 "blocked(w=%llu mean=%.3f se=%.3f t=%.2f fav=%llu)\n",
                 g419AbLeverName(g419AbLever()),
                 g419AbBallast(),
                 static_cast<unsigned long long>(g438ArmHoldFrames()),
                 static_cast<unsigned long long>(idx),
                 static_cast<unsigned long long>(s_dropped),
                 static_cast<unsigned long long>(s_transition),
                 static_cast<unsigned long long>(s_usedCounts[0]),
                 static_cast<unsigned long long>(s_usedCounts[1]),
                 static_cast<unsigned long long>(s_usedCounts[2]),
                 static_cast<unsigned long long>(s_usedCounts[3]),
                 static_cast<unsigned long long>(s_n[0]), c, sc,
                 static_cast<unsigned long long>(s_n[1]), k, sk,
                 c - k, k != 0.0 ? 100.0 * (c - k) / k : 0.0,
                 std::sqrt(sc * sc + sk * sk),
                 static_cast<unsigned long long>(s_blkN), blkMean, blkSe, blkT,
                 static_cast<unsigned long long>(s_blkFav));

    // G492 workload-invariant report (17a law 22). Per-arm per-frame rates for all five backend
    // edges, the render draw count, the flush count and the CPU-replay fallback count. Read it
    // BEFORE quoting any G492 ceiling: the target edge's `skip` column is the lever, and EVERY
    // other column must match across arms or the arms ran different guest work.
    // G493: DC2_G492_INV=1 prints the invariant for ANY lever. g492ProbeArmed() already accepted
    // that flag for the COUNTING side, so the census was collectable but invisible for every lever
    // but the two G492 probes. A lever that changes when the GL worker runs — latedrain does — can
    // change how many items reach each edge, and the standing gate needs that visible.
    static const bool g493InvAlways = (std::getenv("DC2_G492_INV") != nullptr);
    if (g493InvAlways ||
        g419AbLever() == static_cast<int>(kG492LeverNoRender) ||
        g419AbLever() == static_cast<int>(kG492LeverNoOther))
    {
        char inv[2][320];
        for (int a = 0; a < 2; ++a)
        {
            const double f = s_g492Frames[a] != 0u ? static_cast<double>(s_g492Frames[a]) : 1.0;
            std::snprintf(inv[a], sizeof(inv[a]),
                          "f=%llu render=%.1f/%.1f rdcol=%.1f wrcol=%.1f copy=%.1f "
                          "other=%.1f/%.1f draws=%.0f flush=%.1f fallback=%.2f",
                          static_cast<unsigned long long>(s_g492Frames[a]),
                          g_g492Edge[a][0].load(std::memory_order_relaxed) / f,
                          g_g492Skip[a][0].load(std::memory_order_relaxed) / f,
                          g_g492Edge[a][1].load(std::memory_order_relaxed) / f,
                          g_g492Edge[a][2].load(std::memory_order_relaxed) / f,
                          g_g492Edge[a][3].load(std::memory_order_relaxed) / f,
                          g_g492Edge[a][4].load(std::memory_order_relaxed) / f,
                          g_g492Skip[a][4].load(std::memory_order_relaxed) / f,
                          g_g492Draws[a].load(std::memory_order_relaxed) / f,
                          static_cast<double>(s_g492Flush[a]) / f,
                          static_cast<double>(s_g492Fb[a]) / f);
        }
        std::fprintf(stderr, "[G492:inv] cand(%s) | ctrl(%s)\n", inv[0], inv[1]);
    }

    // G447 host check: turn the paired statistic into a verdict a reader who has never seen this
    // instrument can act on, and flag the two ways the run can measure NOTHING.
    if (g447HostCheckOn() && g419AbLever() == static_cast<int>(kG447LeverSpinBoth))
    {
        int logical = 0, physical = 0;
        g447HostShape(&logical, &physical);
        // G443: DC2 is a locked-30 title. A run whose work drops below 33.33 ms/f reads ~33.3 with
        // se ~0.01 on BOTH arms — that is the cap, not a result.
        const bool capped = (c > 33.0 && c < 33.8 && k > 33.0 && k < 33.8 && sc < 0.03 && sk < 0.03);
        const char *verdict;
        if (capped)
            verdict = "INVALID-FRAME-CAP: rerun with DC2_PATCH_60FPS=1 on BOTH arms (G443)";
        else if (s_blkN < 3u)
            verdict = "INCONCLUSIVE: too few windows, run longer (want w>=4)";
        else if (blkT <= -3.0 && s_blkFav * 2u >= s_blkN * 2u - 2u)
            verdict = "KEEP DEFAULT: the spins pay on this host";
        else if (blkT >= 3.0)
            verdict = "ROLL BACK: set DC2_G447_NO_SPIN=1 and DC2_G447_NO_WSPIN=1 on this host";
        else
            verdict = "NEUTRAL: no effect either way on this host; default is harmless";
        std::fprintf(stderr,
                     "[G447:host] cpus=%d logical / %d physical | runtime uses 4 busy threads + %d "
                     "spinners | spinMs/f=%.3f se=%.3f t=%.2f windows=%llu favouring=%llu | "
                     "reference host (6 physical/12 logical): -1.14 ms/f t=-7.19 4/4 | %s\n",
                     logical, physical, 2, blkMean, blkSe, blkT,
                     static_cast<unsigned long long>(s_blkN),
                     static_cast<unsigned long long>(s_blkFav), verdict);
    }
}

// ---------------------------------------------------------------------------------------------
// Exact CT32 upload fast pack.

// G419 PROMOTED to default-ON (2026-07-30). Exact by construction (the table IS addrPSMCT32
// restricted to one page, paired loads rest on the one-word columnTable32 adjacency) and by oracle
// (643,149,824 pixels, bad=0). Isolated cost 1.032 -> 0.767 ms/frame against G298's row-gated lanes
// and 0.730 -> 0.371 against the pixel gate; within-process randomized A/B over 6,000 MAP-0 frames
// measured -0.426 ms/frame (blocked -0.394, se 0.169, t = -2.33, 7/10 windows favouring).
// Kill with DC2_G419_NO_FAST_PACK=1 or DC2_G419_FAST_PACK=0.
static bool g419FastPackOn()
{
    static const bool on = !envFlagEnabled("DC2_G419_NO_FAST_PACK") &&
                           !envFlagDisabled("DC2_G419_FAST_PACK");
    return on;
}

// G419 pixel-gated row-pool fan-out for the upload pack. PROMOTED to default-ON (2026-07-30):
// isolated 1.032 -> 0.730 ms/frame against the legacy pack and 0.767 -> 0.371 against the fast pack,
// and within-process randomized A/B over 5,400 MAP-0 frames measured -0.320 ms/frame
// (blocked -0.303, se 0.137, t = -2.22, 6/9 windows favouring).
// Kill with DC2_G419_NO_PACK_LANE_GATE=1 or DC2_G419_PACK_LANE_GATE=0.
//
// G298 gates the pack's row-pool fan-out on `packRows >= 8`. Rows cannot express the work: DC2's
// small upload targets pack 128 rows of a 128-pixel target (16k pixels) or 64 rows of a 64-pixel one
// (4k pixels), which clear a row gate while carrying 1/13th to 1/53rd of a 416x512 pack. Measured on
// the current binary (DC2_G419_PROFILE, exact fast pack, same call counts):
//
//     target      geometry    8 lanes   1 lane
//     0x139/w8    448x512     0.102     0.152   <- fan-out pays
//     0x0/w8      416x512     0.102     0.134   <- pays
//     0x68/w8     416x512     0.096     0.137   <- pays
//     0x139/w2    128x128     0.083     0.012   <- fan-out costs 7x the whole loop
//     0x13c/w2    128x128     0.076     0.011
//     0x146/w2    128x128     0.077     0.013
//     0x143/w1     64x64      0.079     0.004
//     0x155/w1     64x64      0.076     0.004
//
// So GSRowPool dispatch is a fixed ~0.085 ms on this route (8 lane wake-ups + join). Break-even for
// an 8-lane split is therefore W > D*8/7, i.e. about 0.097 ms of serial work, which at the measured
// 0.63 ns/pixel is about 154k pixels. The DC2 population is bimodal (213k-229k vs 4k-16k), so any
// threshold in the gap is safe; 65,536 stays well clear of both clusters.
//
// EXACT BY CONSTRUCTION: lane count never changes results. G298's contract is that disjoint guest
// row ranges own disjoint VRAM/staging words, so one lane is simply the serial reference. Verified
// by DC2_G298_VERIFY_FBPACK / DC2_G419_VERIFY, which re-derive every packed pixel serially.
static bool g419PackLaneGateOn()
{
    static const bool on = !envFlagEnabled("DC2_G419_NO_PACK_LANE_GATE") &&
                           !envFlagDisabled("DC2_G419_PACK_LANE_GATE");
    return on;
}

static uint64_t g419PackLaneMinPixels()
{
    static const uint64_t n = [] {
        const char *value = std::getenv("DC2_G419_LANE_MIN_PX");
        if (value == nullptr)
            return 65536;
        return std::max(0, std::atoi(value));
    }();
    return n;
}

// `requested` is G298's existing lane count (already row-gated). Returns the lane count to dispatch.
static int g419PackLanes(int packRows, int fbW, int requested)
{
    if (requested <= 1 || packRows <= 0 || fbW <= 0)
        return requested;
    if (!g419AbArmFor(kG419LeverLanes, g419PackLaneGateOn()))
        return requested;
    const uint64_t pixels = static_cast<uint64_t>(packRows) * static_cast<uint64_t>(fbW);
    return pixels >= g419PackLaneMinPixels() ? requested : 1;
}

// Behaviour-pure isolated timer for the pack loop, keyed by target/width. Unlike DC2_G417_PROFILE
// this arms NOTHING in the GL backend, so it does not insert a finish and does not perturb
// scheduling - it is safe to read alongside a normal run.
static bool g419ProfileOn()
{
    static const bool on = envFlagEnabled("DC2_G419_PROFILE");
    return on;
}

struct G419Ct32Page
{
    uint32_t block = 0u;
    uint32_t width = 0u;
    bool valid = false;
    uint32_t off[32][64]{};
};

// Uploads visit several (block,width) pairs per frame (fbp=0x139 at fbw=8 AND fbw=2, plus 0x0,
// 0x68 and singletons), so a single-entry cache would rebuild 2048 addresses per upload. Twelve
// slots cover the observed DC2 population with round-robin replacement as the fail-safe.
static G419Ct32Page s_g419Ct32[12];
static uint32_t s_g419Ct32Next = 0u;

static const G419Ct32Page &g419Ct32Table(uint32_t block, uint32_t width)
{
    const uint32_t pagesPerRow = (width != 0u) ? width : 1u;
    for (G419Ct32Page &e : s_g419Ct32)
        if (e.valid && e.block == block && e.width == pagesPerRow)
            return e;
    G419Ct32Page &e = s_g419Ct32[s_g419Ct32Next % 12u];
    ++s_g419Ct32Next;
    const uint32_t base = (block >> 5u) << 13u;
    for (uint32_t y = 0; y < 32u; ++y)
        for (uint32_t x = 0; x < 64u; ++x)
            e.off[y][x] = GSPSMCT32::addrPSMCT32(block, pagesPerRow, x, y) - base;
    e.block = block;
    e.width = pagesPerRow;
    e.valid = true;
    return e;
}

// Pack guest rows [laneLo,laneHi] of a CT32 target out of VRAM into the bottom-first staging buffer.
// `legacy` runs the original serial per-pixel path verbatim as the control arm.
static void g419PackFbRows(uint32_t *dst, size_t dstCount,
                           const uint8_t *vram, uint32_t vramSize,
                           uint32_t fbBlock, uint32_t fbw, int fbW, int fbH,
                           int laneLo, int laneHi, bool legacy)
{
    if (dst == nullptr || vram == nullptr || laneHi < laneLo || fbW <= 0)
        return;
    if (legacy || (fbW & 1) != 0)
    {
        for (int y = laneLo; y <= laneHi; ++y)
        {
            const size_t dstRow = static_cast<size_t>(fbH - 1 - y) * fbW;
            if (dstRow + static_cast<size_t>(fbW) > dstCount)
                continue;
            for (int x = 0; x < fbW; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(fbBlock, fbw,
                                                           static_cast<uint32_t>(x),
                                                           static_cast<uint32_t>(y));
                uint32_t px = 0u;
                if (off + 4u <= vramSize)
                    std::memcpy(&px, vram + off, 4);
                dst[dstRow + x] = px;
            }
        }
        return;
    }
    const G419Ct32Page &tbl = g419Ct32Table(fbBlock, fbw);
    const uint32_t pagesPerRow = (fbw != 0u) ? fbw : 1u;
    const uint32_t basePage = fbBlock >> 5u;
    for (int y = laneLo; y <= laneHi; ++y)
    {
        const size_t dstRow = static_cast<size_t>(fbH - 1 - y) * fbW;
        if (dstRow + static_cast<size_t>(fbW) > dstCount)
            continue;
        const uint32_t *rowOff = tbl.off[static_cast<uint32_t>(y) & 31u];
        const uint32_t yBase =
            (basePage + (static_cast<uint32_t>(y) >> 5u) * pagesPerRow) << 13u;
        uint32_t *dstPx = dst + dstRow;
        for (int x = 0; x < fbW; x += 2)
        {
            const uint32_t off = yBase +
                                 (static_cast<uint32_t>(x >> 6) << 13u) +
                                 rowOff[static_cast<uint32_t>(x) & 63u];
            // Pixel x sits at off and pixel x+1 at off+4 (one-word columnTable32 adjacency), so the
            // original loop's per-pixel bound test maps onto exactly these three cases.
            if (off + 8u <= vramSize)
            {
                std::memcpy(dstPx + x, vram + off, 8);
            }
            else if (off + 4u <= vramSize)
            {
                std::memcpy(dstPx + x, vram + off, 4);
                dstPx[x + 1] = 0u; // the original wrote 0 when off+4 (here off+8) exceeded VRAM
            }
            else
            {
                dstPx[x] = 0u;
                dstPx[x + 1] = 0u;
            }
        }
    }
}

// DC2_G419_PROFILE aggregate for the isolated pack cost, keyed by target and staging geometry.
struct G419PackStat
{
    uint64_t n = 0, rows = 0, ns = 0;
};
static std::map<uint64_t, G419PackStat> s_g419Packs; // flush-owner thread only
static uint64_t s_g419PackTotal = 0u;

static void g419NotePack(uint32_t fbp, uint32_t fbw, int rows, uint64_t ns)
{
    const uint64_t key = (static_cast<uint64_t>(fbp) << 8) | (fbw & 0xffu);
    G419PackStat &s = s_g419Packs[key];
    ++s.n;
    s.rows += static_cast<uint64_t>(rows);
    s.ns += ns;
    if ((++s_g419PackTotal % 512u) != 0u)
        return;
    // `perF` is calls per FRAME and `msF` is this target's whole contribution to a frame. msF is the
    // only number a payoff prediction may use; per-call cpu is a mechanism figure.
    const double frames = s_g419FrameTicks != 0u ? static_cast<double>(s_g419FrameTicks) : 1.0;
    double sumMsF = 0.0;
    std::fprintf(stderr, "[G419:pack] total=%llu frames=%llu fast=%d",
                 static_cast<unsigned long long>(s_g419PackTotal),
                 static_cast<unsigned long long>(s_g419FrameTicks),
                 g419FastPackOn() ? 1 : 0);
    for (const auto &kv : s_g419Packs)
    {
        const G419PackStat &e = kv.second;
        if (e.n == 0u)
            continue;
        const double msF = static_cast<double>(e.ns) / 1.0e6 / frames;
        sumMsF += msF;
        std::fprintf(stderr, " fbp=0x%x/w%u(n=%llu perF=%.2f rows=%.1f cpu=%.3f msF=%.3f)",
                     static_cast<unsigned>(kv.first >> 8),
                     static_cast<unsigned>(kv.first & 0xffu),
                     static_cast<unsigned long long>(e.n),
                     static_cast<double>(e.n) / frames,
                     static_cast<double>(e.rows) / e.n,
                     static_cast<double>(e.ns) / 1.0e6 / e.n,
                     msF);
    }
    std::fprintf(stderr, " TOTmsF=%.3f\n", sumMsF);
}
