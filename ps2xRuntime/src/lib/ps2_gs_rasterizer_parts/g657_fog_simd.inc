// ===== G657 P2b: the three per-pixel FOG channel lerps, as ONE exact SSE2 vector ===============
//
// WHY. `[G605:shape]` on `lavaboss` (`DC2_G605_CYC=1`, final report) ranks the `fge=1` shapes at
// **90.71% of the CPU band replay's cycles** — rows #00..#05, #08 and #10, every one of them
// `disp tri-fast`, the G609 admitted-triangle kernel that G656 P20 already lowered. Every pixel of
// that leaf runs `g364ApplyFog`, which is three independent scalar lerps.
//
// ⭐ THE SAME ARGUMENT AS G656 P20, AND IT IS THE ONE G653 P26 DOES NOT REACH. G653 refuted SIMD
// for the span kernels on POPULATION — 10.1 px/call through a non-linear swizzle leaves lanes
// unfillable — but that is about vectorising across PIXELS. These are the three CHANNELS of ONE
// pixel: the vector is full by construction at every pixel, at any span length, with no gather.
//
// ⭐⭐ WHY 16-BIT LANES AND NOT 32. The obvious lowering puts each channel in a 32-bit lane, but
// SSE2 has no 32x32 low multiply (`_mm_mullo_epi32` is SSE4.1) and no integer divide, so `/255`
// would need two `_mm_mul_epu32` passes plus repacking — more instructions than the scalar form it
// replaces. In 16-bit lanes it is six instructions total, and it is EXACT because both products
// provably fit:
//
//     c*f + fc*(255-f)   with c, fc, f in [0,255]
//       <= 255*f + 255*(255-f) = 255*255 = 65025          (a convex combination scaled by 255)
//     + 127                    = 65152 <= 65535           so no 16-bit lane can overflow
//
// and the divide is the standard exact reciprocal for this range:
//
//     x/255 == (x * 32897) >> 23   for 0 <= x <= 65535
//           == ((x * 32897) >> 16) >> 7
//           == _mm_srli_epi16(_mm_mulhi_epu16(x, 32897), 7)
//
// `_mm_mulhi_epu16` yields exactly the high 16 bits of the 32-bit product, so the identity is
// reproduced bit-for-bit rather than approximated. Checked at the boundaries that matter:
// x=254 -> 0, x=255 -> 1, x=65024 -> 254, x=65025 -> 255, x=65152 -> 255.
//
// ⛔ NOT AN APPROXIMATION AND NOT A REASSOCIATION. Every lane computes the same integer expression,
// in the same order, as `g364FogLerp`. `DC2_G657_VERIFY_FOG=1` proves it per pixel rather than
// asserting it, and SHIPS THE SCALAR RESULT so a defect can never reach a pixel or a capture.

#pragma once

#include <emmintrin.h>

// Rollback for the promoted default (the project's standing rollback convention).
inline bool g657SimdFogOn()
{
    static const bool on = !envFlagEnabled("DC2_G657_NO_SIMDFOG");
    return on;
}

// Per-pixel shadow oracle. Runs BOTH forms and ships the SCALAR one.
inline bool g657VerifyFogOn()
{
    static const bool on = envFlagEnabled("DC2_G657_VERIFY_FOG");
    return on;
}

static std::atomic<uint64_t> g_g657FogChecks{0};
static std::atomic<uint64_t> g_g657FogBad{0};

// The three channels of one pixel, lerped towards FOGCOL by f, in one vector.
// Lane layout: [0]=r [1]=g [2]=b, lanes 3..7 are don't-care and are never read back.
// ⛔⛔ MEASURED NOTE — THE FIRST FORM OF THIS KERNEL WAS EXACT AND SLOWER, AND THE REASON IS THE
// POINT. It built the vector with `_mm_set_epi16(...)` from three separate `uint8_t&` and read it
// back with `_mm_store_si128` into a stack array plus three loads. On `lavaboss` at matched
// denominators (n=896,458 / px=1,103,963,280 in both arms) that cost **330.2 vs 328.9 cyc/inside,
// i.e. +0.40%** — every matched row slower.
//
// ⭐ THE SHARPENED LAW (refines PS2_PROJECT_STATE.md rule 18): vectorising ACROSS CHANNELS is only
// free when the channels are ALREADY PACKED. G656 P20's win came from the packing, not from the
// lane count — its four channels were already four byte lanes of the same 32-bit texel word, so the
// vector cost nothing to form. Here the three channels arrive as three separate byte references,
// and an 8-argument `_mm_set_epi16` lowers to a chain of inserts that costs more than the three
// scalar multiply-shift chains it replaces.
//
// So form the vector the cheap way instead: one 32-bit gather into a register
// (`_mm_cvtsi32_si128`), one `_mm_unpacklo_epi8` to widen to 16-bit lanes, and one
// `_mm_packus_epi16` + `_mm_cvtsi128_si32` to get back — no memory round trip, no insert chain.
inline void g657FogQuad3(uint32_t f, uint32_t fc, uint8_t &r, uint8_t &g, uint8_t &b)
{
    const __m128i zero = _mm_setzero_si128();
    const uint32_t packed = static_cast<uint32_t>(r) |
                            (static_cast<uint32_t>(g) << 8) |
                            (static_cast<uint32_t>(b) << 16);
    const __m128i c = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(packed)), zero);
    const __m128i fcv = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(fc & 0x00FFFFFFu)),
                                          zero);
    const __m128i fv = _mm_set1_epi16(static_cast<short>(f));
    const __m128i nfv = _mm_set1_epi16(static_cast<short>(255u - f));

    // c*f + fc*(255-f) + 127, all within 16 bits by the bound proved in the header.
    __m128i acc = _mm_add_epi16(_mm_mullo_epi16(c, fv), _mm_mullo_epi16(fcv, nfv));
    acc = _mm_add_epi16(acc, _mm_set1_epi16(127));
    // /255, exactly.
    const __m128i q =
        _mm_srli_epi16(_mm_mulhi_epu16(acc, _mm_set1_epi16(static_cast<short>(32897))), 7);

    const uint32_t out = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(q, zero)));
    r = static_cast<uint8_t>(out & 0xFFu);
    g = static_cast<uint8_t>((out >> 8) & 0xFFu);
    b = static_cast<uint8_t>((out >> 16) & 0xFFu);
}
