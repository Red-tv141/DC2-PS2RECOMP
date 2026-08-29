// ===== G662 P4: normal-build interface for the CLOSED G590 surface-authority diagnostics ========
//
// Same mechanism and the same rule as the g655_stub_* family: a diagnostic that lives in a hot TU
// is a COMPILE-TIME build mode, not a runtime flag (Rule 12b/12c — G653 P25 measured ~60 lines of
// never-executed `const bool`-guarded code in `ps2_gs_rasterizer.cpp` at **+0.484 ms/f**, and G654
// P5 measured the same law with the sign reversed: deleting 134,807 bytes from that object bought
// **−1.082 ms/f on `dragon:tail`, with all three threads improving**).
//
// `g590_surface_authority.inc` is 821 lines / **9,430 code bytes / 34 COMDATs** in
// `ps2_gs_rasterizer.obj` (`tools/g654_tu_price.py`), and EVERY entry point it exports is gated on
// an env flag that is OFF in every default and every performance run:
//
//   DC2_G590_CENSUS · DC2_G590_SHADOW      -> g590Armed()       -> Note*/Shadow*/Report
//   DC2_G590_ZWHY                          -> g590ZWhyOn()      -> g590ZWhy
//   DC2_G590_ZVERIFY                       -> g590ZVerifyOn()   -> g590ZVerifySkip
//   DC2_G590_ZWIN (or ZVERIFY, or the `zwin` A/B arm) -> g590ZWinSelected() -> g590ZWinSkip/ZStamp
//
// So in a default run every one of these functions takes its first early-return, and the stubs
// below are BEHAVIOUR-IDENTICAL by construction, not merely "believed equivalent":
//   * `g590ZWinSkip` returns `false` exactly as `!g590ZWinOn()` does — no readback is skipped;
//   * `g590ClassifyColor` is a PURE function of its five arguments with no gate at all, so it is
//     reproduced verbatim rather than stubbed (its result feeds `g590NoteColor`, which is a
//     no-op here, but reproducing it keeps the call sites and their argument evaluation intact);
//   * everything else is a void accumulator behind `g590Armed()`.
//
// Restore the real family with `-DPS2X_G655_LEGACY_DIAG=ON` (the same switch the closed
// G214/G336/G369/G400/G526/G595 families already use — this is the same class of artefact and does
// not deserve a second build knob).

// The colour/depth cause codes are part of the CALL SITES, not of the census, so they must exist
// in both builds. Kept byte-identical to g590_surface_authority.inc:124-142.
enum
{
    kG590ColSkipRes = 0,
    kG590ColSkipSnap = 1,
    kG590ColSkipOwner = 2,
    kG590ColUpSnapInv = 3,
    kG590ColUpGenMoved = 4,
    kG590ColUpRowsOut = 5,
    kG590ColN = 6,
};

enum
{
    kG590ZSkipPend = 0,
    kG590ZSkipRes = 1,
    kG590ZUpFull = 2,
    kG590ZUpWin = 3,
    kG590ZUpSeed = 4,
    kG590ZN = 5,
};

static constexpr bool g590CensusOn() { return false; }
static constexpr bool g590ShadowOn() { return false; }
static constexpr bool g590Armed() { return false; }
static constexpr bool g590ZWhyOn() { return false; }
static constexpr bool g590ZVerifyOn() { return false; }
static constexpr bool g590ZWinSelected() { return false; }
static constexpr bool g590ZWinOn() { return false; }
static constexpr bool g590ZTracked() { return false; }

// PURE — no gate in the original. Reproduced verbatim so the call sites keep their exact shape.
static int g590ClassifyColor(bool uploadFb, bool forceNoUpload, bool consumeOwner, bool snapValid,
                             bool genSame)
{
    if (!uploadFb)
        return forceNoUpload ? kG590ColSkipRes
                             : (consumeOwner ? kG590ColSkipOwner : kG590ColSkipSnap);
    if (!snapValid)
        return kG590ColUpSnapInv;
    return genSame ? kG590ColUpRowsOut : kG590ColUpGenMoved;
}

static void g590NoteColor(uint32_t, int, int) {}
static void g590NoteDepth(uint32_t, int, int) {}
static void g590NoteReadback(uint32_t, int) {}
static void g590NoteMaterialize(uint32_t, int, int) {}
static void g590ShadowColor(uint32_t, uint32_t, int, int, int, int, int,
                            const std::vector<uint32_t> &) {}
static void g590ShadowDepth(uint32_t, uint64_t, int, int, int, int, int,
                            const std::vector<float> &) {}
static void g590ZStamp(uint64_t, uint32_t, uint32_t) {}
static void g590ZDropStamp(uint64_t) {}
static void g590ZWhy(uint64_t, uint32_t, uint32_t, int, int, bool, bool, bool, bool, bool, bool) {}
// Returns false for the same reason the real one does in a default run: `!g590ZWinOn()`.
static bool g590ZWinSkip(uint64_t, uint32_t, uint32_t, int, int) { return false; }
static void g590ZVerifySkip(uint64_t, int, int, int, int, const std::vector<float> &) {}
static void g590Report() {}
