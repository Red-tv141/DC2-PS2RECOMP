// ===== G736: the A/B arm selectors are COMPILE-TIME CONSTANTS in a shipping build =====
//
// THE DEFECT. `DC2_G419_AB=<lever>` needs `-DPS2X_G419_DIAG=ON`; without it `g419AbLever()` is
// `static constexpr int g419AbLever() { return kG419LeverNone; }` (g419_ab_instrument.inc), so the
// FIRST statement of every arm selector -- `if (g419AbLever() != kSomeLever) return -1;` -- is a
// compile-time true and the whole function collapses to `return -1`.
//
// It collapsed, and then it stayed. Disassembling the promoted deliverable (SHA 4872FAF9...,
// 98,357,760 B) at each `*Arm@@` address in its own `.map`:
//
//     ?g477RunWriteArm@@YAHXZ    83 c8 ff c3        or eax,-1 ; ret
//     ?g672RowPairArm@@YAHXZ     83 c8 ff c3        or eax,-1 ; ret
//     ... 100 of them, each at its OWN address ...
//
// `/OPT:ICF` cannot fold them (the link is `/DEBUG`, which defaults `/OPT` to `NOREF,NOICF`), so
// the image carries 100 four-byte functions that can never do anything.
//
// HOW BIG IS IT, HONESTLY. Scanning every `E8`/`E9` rel32 in `.text` for each stub's address in the
// shipped binary: **98 of the 100 have ZERO callers** -- LTCG did inline them -- and the two that
// remain are `g477RunWriteArm`'s call sites inside `GSMem::WriteRunCT32` / `WriteRunP4`, i.e. once
// per horizontal IMAGE run. So this is NOT a multi-millisecond lever, and the 4.62% that G735
// sec.6.0's PC sampler attributes to `g477RunWriteArm` is a SYMBOL FOLD, not that function (see
// g446_gsworker_pcsample.inc's G736 note: the row belongs to `GSMem::g477RunWriteP4`, which begins
// 4 bytes later). What this change buys is exact and small: two cross-TU calls removed from the
// IMAGE run writers, and 100 dead functions removed from a binary whose hot TU is already the one
// G710 measured at +0.362 ms/f for compile-time-DEAD text.
//
// THE FIX, AND WHY IT IS EXACT. When `PS2X_G419_DIAG` is not defined this header supplies each
// selector as an `inline` function returning the SAME constant the out-of-line body provably
// returns, and it is included at the top of every consuming TU so the definition precedes every
// call site. `g419_ab_instrument.inc` then drops its own out-of-line copies (guarded on
// `DC2_G736_INLINE_ARMS`), so exactly one definition exists per build configuration.
//
//   * `PS2X_G419_DIAG=ON`  - this header is empty, the real selectors are compiled and exported,
//                            every `DC2_G419_AB` lever behaves exactly as before.
//   * `PS2X_G419_DIAG=OFF` - every call site folds to `-1` and the branch that consumes it folds
//                            to the shipped default. No behaviour can change: the shipping value
//                            was already `-1` on every one of these calls, on every frame.
//
// NOT HERE, DELIBERATELY: `g473SimdMergeArm` and `g420NarrowTouchArm`. Both have a real env-var
// fallback BELOW the lever test, so they do not collapse to a constant -- the disassembly shows
// real prologues (`40 53 48 83 ...`) -- and folding them would change behaviour.
//
// Rollback / gate A arm: `-DPS2X_G736_LEGACY=ON` restores every pre-G736 hot-path code shape (these
// stubs and the G623 producer-surface walk), so ONE source tree builds both arms of the
// cross-binary A/B/B/A that Rule 46 requires for a code-layout change.

#ifndef PS2X_G736_AB_ARM_STUBS_INCLUDED
#define PS2X_G736_AB_ARM_STUBS_INCLUDED

#if !defined(PS2X_G419_DIAG) && !defined(PS2X_G736_LEGACY)

// Tells g419_ab_instrument.inc that this TU already owns the definitions.
#define DC2_G736_INLINE_ARMS 1

inline int g456UvPredicateArm() { return -1; }
inline int g476PageBumpArm() { return -1; }
inline int g477RunWriteArm() { return -1; }
inline int g652SsboRingArm() { return -1; }
inline int g652Gif412Arm() { return -1; }
inline int g475VnopTraceArm() { return -1; }
inline int g479LeanFetchArm() { return -1; }
inline int g483PcRegArm() { return -1; }
inline int g484WideVf0Arm() { return -1; }
inline int g480PktPoolArm() { return -1; }
inline int g480NopSkipArm() { return -1; }
inline int g481HotLowerArm() { return -1; }
inline int g487FlagLowerArm() { return -1; }
inline int g488FastOpArm() { return -1; }
inline int g489PcSkipArm() { return -1; }
inline int g490BlockRunArm() { return -1; }
inline int g610JitArm() { return -1; }
inline int g611OpsArm() { return -1; }
inline int g612RegionArm() { return -1; }
inline int g611QSplitArm() { return -1; }
inline int g499BlkLowerArm() { return -1; }
inline int g499QpBlockArm() { return -1; }
inline int g500BlkBranchArm() { return -1; }
inline int g502BlkSplitArm() { return -1; }
inline int g502BlkChainArm() { return -1; }
inline int g502BlkTail0Arm() { return -1; }
inline int g504HistCurArm() { return -1; }
inline int g506QpSegArm() { return -1; }
inline int g514VStoreArm() { return -1; }
inline int g514VLoadArm() { return -1; }
inline int g514VMemArm() { return -1; }
inline int g515NopPeelArm() { return -1; }
inline int g531FlagTailArm() { return -1; }
inline int g532FlagHeadArm() { return -1; }
inline int g532DivLowerArm() { return -1; }
inline int g533ImageSpecArm() { return -1; }
inline int g547Hot0c70Arm() { return -1; }
inline int g548Hot0e00Arm() { return -1; }
inline int g549Hot30d8Arm() { return -1; }
inline int g550Hot1be8Arm() { return -1; }
inline int g551Hot1c00Arm() { return -1; }
inline int g554Hot0900Arm() { return -1; }
inline int g555Hot1110Arm() { return -1; }
inline int g568BitAliasArm() { return -1; }
inline int g573Tri13bArm() { return -1; }
inline int g580Tri13bFastArm() { return -1; }
inline int g588SolidSpriteArm() { return -1; }
inline int g614SprFastArm() { return -1; }
inline int g615PrivZArm() { return -1; }
inline int g617BlendFillArm() { return -1; }
inline int g617T4hhArm() { return -1; }
inline int g590ZWinArm() { return -1; }
inline int g591PrivZArm() { return -1; }
inline int g594FastWalkArm() { return -1; }
inline int g594FastDeswzArm() { return -1; }
inline int g594FastHashArm() { return -1; }
inline int g595LineArm() { return -1; }
inline int g595NoteMemoArm() { return -1; }
inline int g596CopyArm() { return -1; }
inline int g592PubConsArm() { return -1; }
inline int g591ColWinArm() { return -1; }
inline int g589DrainMemoArm() { return -1; }
inline int g552RttTriArm() { return -1; }
inline int g541VuNullArm() { return -1; }
inline int g519CullCmpArm() { return -1; }
inline int g520MacScanArm() { return -1; }
inline int g491RbFmtArm() { return -1; }
inline int g491RbSpinArm() { return -1; }
inline int g492NoRenderArm() { return -1; }
inline int g492NoOtherArm() { return -1; }
inline int g492Ct24ReuseArm() { return -1; }
inline int g493LateDrainArm() { return -1; }
inline int g494NoSyncArm() { return -1; }
inline int g496GpuSlowArm() { return -1; }
inline int g508LockSkipArm() { return -1; }
inline int g510CapMoveArm() { return -1; }
inline int g510ClsMemoArm() { return -1; }
inline int g522L2lSkipArm() { return -1; }
inline int g521CapInPlaceArm() { return -1; }
inline int g511TexRunArm() { return -1; }
inline int g511TexProbeArm() { return -1; }
inline int g511ClsProbeArm() { return -1; }
inline int g512FastDecArm() { return -1; }
inline int g513HashLaneArm() { return -1; }
inline int g496GlStateArm() { return -1; }
inline int g482MemoHashArm() { return -1; }
inline int g482WriteCoalesceArm() { return -1; }
inline int g482NotifySkipArm() { return -1; }
inline int g478KickPoolArm() { return -1; }
inline int g497WinBufArm() { return -1; }
inline int g498OwnPubArm() { return -1; }
inline int g672RowPairArm() { return -1; }
inline int g438SlowArm(int) { return -1; }
inline int g503EeSlowArm() { return -1; }
inline int g503ChainPoolArm() { return -1; }
inline int g439CoalesceArm() { return -1; }
inline int g445KickArm() { return -1; }
inline int g447SpinArm() { return -1; }
inline int g447WorkerSpinArm() { return -1; }
inline int g445KickAllArm() { return -1; }

#endif // !PS2X_G419_DIAG && !PS2X_G736_LEGACY
#endif // PS2X_G736_AB_ARM_STUBS_INCLUDED
