// ===== G656 P6/P9: the two lap timers this phase needs, as a COMPILE-TIME build mode ==========
//
// WHY THIS FILE EXISTS. Two G656 priorities need a split that no existing instrument provides:
//
//   P9  `[G313:seg]` proved that 95.9% of the `lavaboss` host->local IMAGE layer is `range`, i.e.
//       `g144FlushPendingUploadRange`. That is an INCLUSIVE timer around a FLUSH, so by G429's law
//       it measures the work it DRAINS, not the upload. Deciding whether any of it is deletable
//       needs the edge split into its named stages: the deferred command graph (`exec`, which is
//       CONSERVED draw work and therefore not a lever) versus the materialization round trips and
//       the residue (which are).
//
//   P6  `[G323:census]` prices the per-entry `bind` loop at 70-71% of `deps`, but as ONE number
//       over 324-362 entries per flush. Which of its ~11 candidate-source gates owns that number
//       is not observable, and G655 P8/P21 already refuted memoizing the loop wholesale, so the
//       only remaining question is whether ONE gate is expensive and independently optimizable.
//
// ⛔ RULE 12c. Both live in `ps2_gs_rasterizer.cpp`, the hottest TU in the build. G653 P25 measured
// ~60 lines of never-executed `const bool`-guarded diagnostic code there at +0.484 ms/f with NO
// behavioural difference at all, and G654/G655 measured the same law with the sign reversed. So
// this is a build mode (`-DPS2X_G656_DIAG=ON`) whose call sites are `#if`-EXCLUDED, not predicated.
// In the default build every symbol below is either absent or an empty inline that MSVC deletes.
//
// Storage and the reporter live in ps2_g656_diag.cpp — a cold TU (rule 12b), never one with a hot
// loop.
//
// TWO CLOCKS ON PURPOSE. The upload edge runs ~11 times per frame, so a `steady_clock` lap there is
// free and gives directly comparable milliseconds. The bind loop runs 41-68 MILLION times per run,
// where a `steady_clock` pair would be the census that moves what it measures
// (`17e-perf-measurement-traps.md`), so it laps with `__rdtsc()` and reports cycles and shares.

#pragma once

enum G656Slot
{
    // --- P9: g144FlushPendingUploadRange, in call order (steady_clock, nanoseconds) ---
    kG656UpG289Mat = 0,   // g289PrepareUploadOverwrite + g289MaterializeForRanges — ownership pin
    kG656UpExec,          // g178CensusScan + g260ExecuteAll / g271ExecuteConflictPrefix
    kG656UpG264,          // g264NoteUploadForMirror + the g630/g634 patch predicates
    kG656UpG278Dep,       // g278FlushPendingDepthForRanges
    kG656UpG261Mat,       // g261MaterializeForRanges — the GPU->VRAM readback
    kG656UpG278Disp,      // g278FlushPendingDepthForDisplayOwners
    kG656UpG272Mat,       // g272MaterializeForRanges
    kG656UpG276Disp,      // g276FlushPendingDisplayForRanges
    kG656UpTail,          // g277CensusRangeEdge + g314FinishEdge + the return path
    kG656UpNsCount,

    // --- P6: the per-entry bind loop, in call order (__rdtsc, cycles) ---
    kG656BindG289 = kG656UpNsCount,  // g289 direct-display and owner-view source
    kG656BindG310,        // g310 page-view candidate
    kG656BindG309,        // g309 composite candidate
    kG656BindEarly,       // the `texKey==0 || skip || !tme` early-out test itself
    kG656BindT8,          // g281 / g570 / g598 T8 view selection
    kG656BindG267,        // g267 bilinear-triangle footprint + alias rejection
    kG656BindG280,        // g280 alias entry
    kG656BindProducer,    // g623 / g630 producer-surface direct consumption
    kG656BindG634,        // g634 GPU-native raw texture view
    kG656BindTail,        // everything after the last gate to the end of the iteration
    kG656SlotCount
};

#if defined(PS2X_G656_DIAG)

#include <chrono>
#include <intrin.h>

// Defined in ps2_g656_diag.cpp.
bool g656DiagOn();
bool g656BindOn();
void g656Add(int slot, unsigned long long units, unsigned long long calls);
void g656ReportUpload();
void g656ReportBind();
void g656NoteBindEntry(unsigned exitStage);

// Sequential lap over the upload edge: one clock read per stage boundary, charged to the stage
// that just finished. `armed` is read once per edge, never per stage.
struct G656UpLap
{
    bool on;
    std::chrono::steady_clock::time_point t;

    explicit G656UpLap(bool armed) : on(armed)
    {
        if (on)
            t = std::chrono::steady_clock::now();
    }

    void lap(int slot)
    {
        if (!on)
            return;
        const auto now = std::chrono::steady_clock::now();
        g656Add(slot,
                static_cast<unsigned long long>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count()),
                1ull);
        t = now;
    }
};

// Sequential lap over one bind-loop iteration. `__rdtsc` is ~25 cycles and needs no serializing
// instruction here: we are measuring a share, and any skid is charged to the same slot in both
// directions across millions of entries.
//
// ⚠️ THE LOOP BODY IS FULL OF `continue`. A plain lap sequence would silently drop the tail of
// every entry that exits early — and early exit is the COMMON case, so the drop would be the
// measurement. This object therefore tracks which stage is currently open and charges the
// remainder in its DESTRUCTOR, which runs on `continue`, on the normal path and on a `break`
// alike. That also yields the exit-stage population for free, and the population is what
// distinguishes "this stage is expensive" from "every entry walks through this stage".
struct G656BindLap
{
    bool on;
    int next;
    unsigned long long t;

    explicit G656BindLap(bool armed) : on(armed), next(kG656BindG289), t(0ull)
    {
        if (on)
            t = __rdtsc();
    }

    void lap(int slot)
    {
        if (!on)
            return;
        const unsigned long long now = __rdtsc();
        g656Add(slot, now - t, 1ull);
        t = now;
        next = slot + 1;
    }

    ~G656BindLap()
    {
        if (!on)
            return;
        const int slot = (next < kG656SlotCount) ? next : (kG656SlotCount - 1);
        g656Add(slot, __rdtsc() - t, 1ull);
        g656NoteBindEntry(static_cast<unsigned>(slot - kG656BindG289));
    }
};

#define G656_UP_LAP(slot) g656Up.lap(slot)
#define G656_BIND_LAP(slot) g656Bind.lap(slot)

#else

// Default build: no storage, no calls, no bytes. The two arm predicates are `constexpr false`, so
// the call sites below stay readable while MSVC folds each one to nothing and constructs an empty
// aggregate that never reaches a register — the same shape as G654LayerScope.
constexpr bool g656DiagOn() { return false; }
constexpr bool g656BindOn() { return false; }

struct G656UpLap
{
    explicit G656UpLap(bool) {}
};
struct G656BindLap
{
    explicit G656BindLap(bool) {}
};

#define G656_UP_LAP(slot) ((void)0)
#define G656_BIND_LAP(slot) ((void)0)

#endif
