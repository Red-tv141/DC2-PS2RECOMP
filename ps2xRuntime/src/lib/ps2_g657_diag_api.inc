// ===== G657 P3/P4/P7: rank the CPU-fallback population by WALL, not by ENTRIES ================
//
// WHY THIS FILE EXISTS.
//
//   P3  `[G583:rejcensus]` ranks every `g178TryFlushGpu` fallback by REPLAYED ENTRIES, and that is
//       the ranking G568 deliberately chose over an event ranking. But the roadmap asks for the
//       "replay wall-time ceiling" of each reject family, and on `lavaboss` the two rankings
//       DISAGREE BY A FACTOR OF SIXTEEN:
//
//         - by entries: `rej=colorzalias fbp=0x139` is 2,649,339 of 2,843,566 = 93.2%
//         - by cycles : G656 P12 puts the `disp` CLASS at 92.6% of the same route's replay, and
//                       every `disp` fallback here is `texaliasz`/`classify:blend`, together only
//                       5.9% of the ENTRIES.
//
//       Both are true. An entry is not a unit of work: a `fbp=0x139` untextured `tme=0 abe=0`
//       shadow sprite and a `disp` bilinear MODULATE triangle differ by orders of magnitude in
//       cycles per entry. ⛔ RANKING A LEVER OFF THE ENTRY CENSUS PICKS THE WRONG FAMILY. This
//       instrument closes the gap by bracketing the CPU band replay itself and charging its wall
//       to the reject family that sent the batch there.
//
//   P4  The same bracket, plus the GPU-success branch beside it, IS the GPU-native GS coverage
//       metric: for every batch class, how many batches/entries were admitted to the accepted GPU
//       path versus replayed on the CPU, and what each cost in wall time. Coverage expressed only
//       as a batch count is not an architectural metric — a route can be 95% GPU by batch count
//       and spend all of its frame in the other 5%.
//
// ⛔ RULE 12c. The call sites are inside the flush closure in `rasterizer_tilebin_capture.inc`,
// which is compiled into `ps2_gs_rasterizer.cpp` — the hottest TU in the build, where G653 P25
// measured ~60 lines of NEVER-EXECUTED `const bool`-guarded code at +0.484 ms/f. So this is a
// compile-time build mode (`-DPS2X_G657_DIAG=ON`) with `#if`-EXCLUDED call sites, exactly like
// `PS2X_G656_DIAG`. In the default build every symbol below is an empty inline or absent.
//
// Storage and the reporter live in `ps2_g657_diag.cpp`, a cold TU (rule 12b).
//
// ⚠️ ONE CLOCK PER BATCH, NOT PER ENTRY. The fallback fires ~9 times per frame on the boss window
// and the bracket encloses a fork/join over the whole row pool, so a `steady_clock` pair is far
// below the noise of what it measures. It is NOT an `__rdtsc` census: the replay migrates across
// worker threads, so a TSC delta would not be a single thread's clock.

#pragma once

#if defined(PS2X_G657_DIAG)

#include <chrono>

// Defined in ps2_g657_diag.cpp.
bool g657WallOn();
void g657NoteFallbackWall(unsigned rej, unsigned site, unsigned cls, unsigned fbp,
                          unsigned long long entries, unsigned long long ns);
void g657NoteGpuOk(unsigned cls, unsigned fbp, unsigned long long entries,
                   unsigned long long ns);
void g657ReportWall();

// Brackets the CPU band replay for ONE fallback batch. Constructed after the reject reason is
// known and destroyed after the replay's last write, so the reason and the wall it caused are
// recorded as one fact rather than joined after the run.
struct G657WallLap
{
    bool on;
    unsigned rej, site, cls, fbp;
    unsigned long long entries;
    std::chrono::steady_clock::time_point t;

    G657WallLap(bool armed, unsigned rej_, unsigned site_, unsigned cls_, unsigned fbp_,
                unsigned long long entries_)
        : on(armed), rej(rej_), site(site_), cls(cls_), fbp(fbp_), entries(entries_)
    {
        if (on)
            t = std::chrono::steady_clock::now();
    }

    ~G657WallLap()
    {
        if (!on)
            return;
        g657NoteFallbackWall(rej, site, cls, fbp, entries,
                             static_cast<unsigned long long>(
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - t).count()));
    }
};

#else

// Default build: no storage, no calls, no bytes.
constexpr bool g657WallOn() { return false; }
struct G657WallLap
{
    G657WallLap(bool, unsigned, unsigned, unsigned, unsigned, unsigned long long) {}
};
inline void g657NoteGpuOk(unsigned, unsigned, unsigned long long, unsigned long long) {}
inline void g657ReportWall() {}

#endif
