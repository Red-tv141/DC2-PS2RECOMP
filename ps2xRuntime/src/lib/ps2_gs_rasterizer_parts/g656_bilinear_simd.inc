// ===== G656 P20: the four bilinear channel lerps, as ONE SSE2 vector =========================
//
// WHY. `[G605:leaf]` on `lavaboss` ranks `disp tri-fast` at **95.1% of the CPU band replay**
// (31.221 ms/f) at **1,270.9 px/call** and **836.7 cyc/inside-pixel**. The kernel's own ceiling
// probe `DC2_G609_CEIL=6`, which deletes exactly the four `g609Lerp8` calls and nothing else,
// reads **296.9 cyc/inside** — so those four calls are **539.8 cycles = 64.5% of the dominant
// pixel**, i.e. ~19.1 ms/f on `lavaboss:spikes` alone. The untouched control in the same run
// (`discov sprite`) moved 331.2 -> 333.8 (+0.8%), so the arms diverged only where intended.
//
// ⭐ WHY THIS ONE VECTORIZES WHEN G653 P26's SPAN SIMD DID NOT. G653 refuted SIMD for the span
// kernels on POPULATION: the dominant leaf committed 10.1 px/call through a non-linear swizzle
// (`ct[x & 7] * 4`), so the lanes could not be filled. That argument does not reach here. The four
// lerps are not four PIXELS — they are the four CHANNELS of ONE pixel, already sitting in four
// byte lanes of the same four 32-bit words. The vector is full by construction at every pixel, at
// any span length, with no gather and no swizzle. (The G653 re-open condition is met anyway:
// `replay` 31.221 > 8.0 and `pxPerCall` 1,270.9 > 32.)
//
// EXACTNESS. This is not an approximation and not a re-association. Each lane executes the SAME
// three IEEE-754 single-precision operations in the SAME order as the scalar body:
//
//     top    = c00 + (c10 - c00) * fx
//     bottom = c01 + (c11 - c01) * fx
//     v      = top + (bottom - top) * fy
//     n      = (int)v                              // truncate toward zero
//     out    = clampU8((v - (float)n >= 0.5f) ? n + 1 : n)
//
// `subps/mulps/addps` are the per-lane definitions of `subss/mulss/addss`; `cvttps_epi32` is the
// per-lane `cvttss2si`; `cvtdq2ps` is exact for 0..255 and for the small integers `n` can hold.
// Nothing is contracted into an FMA: the build is SSE2 baseline (no `/arch:AVX2`) under MSVC's
// default `/fp:precise`, which neither contracts nor reassociates. The build therefore CANNOT
// produce a different value in the two forms — and `DC2_G656_VERIFY_LERP=1` proves it per pixel
// rather than asserting it, shipping the SCALAR result so a defect can never reach a pixel.
//
// ⛔ The rounding step is `g529RoundHalfUpNonNeg`, not `std::lround` — G529/G609 already promoted
// that substitution and proved it exact for `v >= 0` on 1,677,721,600 roundings. This slice does
// not revisit it; it reproduces whatever `g609Lerp8` does today.

#pragma once

#include <emmintrin.h>

// Rollback for the promoted default (PS2_PROJECT_STATE.md's standing rollback convention).
inline bool g656SimdLerpOn()
{
    static const bool on = !envFlagEnabled("DC2_G656_NO_SIMDLERP");
    return on;
}

// Per-pixel shadow oracle. Runs BOTH forms and ships the SCALAR one, so an exactness defect is
// counted and reported but can never reach a pixel or a captured frame.
inline bool g656VerifyLerpOn()
{
    static const bool on = envFlagEnabled("DC2_G656_VERIFY_LERP");
    return on;
}

static std::atomic<uint64_t> g_g656LerpChecks{0};
static std::atomic<uint64_t> g_g656LerpBad{0};
static std::atomic<uint64_t> g_g656ShadeChecks{0};
static std::atomic<uint64_t> g_g656ShadeBad{0};
static std::atomic<uint64_t> g_g656ModChecks{0};
static std::atomic<uint64_t> g_g656ModBad{0};

// Expand the four bytes of an RGBA word into four int32 lanes, lane 0 = R .. lane 3 = A.
// SSE2 only: `_mm_cvtepu8_epi32` is SSE4.1 and this build's baseline is SSE2.
inline __m128 g656ExpandRgba(uint32_t c)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i x = _mm_cvtsi32_si128(static_cast<int>(c));
    x = _mm_unpacklo_epi8(x, zero);   // 4 x uint16
    x = _mm_unpacklo_epi16(x, zero);  // 4 x uint32
    return _mm_cvtepi32_ps(x);
}

// The whole bilinear blend of one texel quad, all four channels at once.
inline uint32_t g656LerpQuad4(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11,
                              float fx, float fy)
{
    const __m128 f00 = g656ExpandRgba(c00);
    const __m128 f10 = g656ExpandRgba(c10);
    const __m128 f01 = g656ExpandRgba(c01);
    const __m128 f11 = g656ExpandRgba(c11);
    const __m128 fxv = _mm_set1_ps(fx);
    const __m128 fyv = _mm_set1_ps(fy);

    const __m128 top = _mm_add_ps(f00, _mm_mul_ps(_mm_sub_ps(f10, f00), fxv));
    const __m128 bot = _mm_add_ps(f01, _mm_mul_ps(_mm_sub_ps(f11, f01), fxv));
    const __m128 v = _mm_add_ps(top, _mm_mul_ps(_mm_sub_ps(bot, top), fyv));

    // n = (int)v, then the half-up bump. `_mm_cmpge_ps` yields all-ones (= -1 as int32) where the
    // fraction reaches 0.5f, so subtracting the mask IS the `+1`.
    const __m128i n = _mm_cvttps_epi32(v);
    const __m128 frac = _mm_sub_ps(v, _mm_cvtepi32_ps(n));
    const __m128i bump = _mm_castps_si128(_mm_cmpge_ps(frac, _mm_set1_ps(0.5f)));
    __m128i r = _mm_sub_epi32(n, bump);

    // clampU8 for all four lanes: signed-saturate int32 -> int16, then unsigned-saturate -> uint8.
    // The composition is an exact clamp to [0,255] over the whole int32 range (anything above
    // 32767 saturates to 32767 and then to 255; anything below -32768 saturates to 0), and it
    // leaves the four bytes in lane order R,G,B,A — the layout the caller's word already uses.
    r = _mm_packs_epi32(r, r);
    r = _mm_packus_epi16(r, r);
    return static_cast<uint32_t>(_mm_cvtsi128_si32(r));
}

// ===== G656 P20 slice 2: the gouraud shade and the MODULATE combine ==========================
//
// Once the bilinear blend is a vector, the same shape is visible twice more in the same pixel, and
// both are exact for the same reason. `GSVertex` stores `r,g,b,a` as four CONTIGUOUS `uint8_t` at
// offset 12, so one 32-bit load per vertex feeds the identical expand used above.
//
//   scalar (rasterizer_draw_triangle.inc, reproduced verbatim in the G609 kernel):
//       shadeC = clampU8((int)(v0.C * w0 + v1.C * w1 + v2.C * w2))          for C in r,g,b,a
//       fC     = clampU8((tC * shadeC) >> 7)                                (MODULATE, tcc=1)
//
// `v0.C` is `uint8_t`, so `v0.C * w0` is an integral promotion to `int` and then a float
// conversion — exactly what `cvtdq2ps` on the expanded lane produces. The sum groups
// left-to-right, `((a*w0) + (b*w1)) + (c*w2)`, and the vector form uses that same association.

// Load the four contiguous colour bytes of a vertex as one word, without an aliasing violation.
inline uint32_t g656VertexRgba(const GSVertex &v)
{
    uint32_t w;
    std::memcpy(&w, &v.r, sizeof(w));
    return w;
}

// clampU8((int)(c0*w0 + c1*w1 + c2*w2)) for all four channels at once.
inline uint32_t g656ShadeQuad4(uint32_t c0, uint32_t c1, uint32_t c2,
                               float w0, float w1, float w2)
{
    const __m128 f0 = g656ExpandRgba(c0);
    const __m128 f1 = g656ExpandRgba(c1);
    const __m128 f2 = g656ExpandRgba(c2);
    const __m128 v = _mm_add_ps(_mm_add_ps(_mm_mul_ps(f0, _mm_set1_ps(w0)),
                                           _mm_mul_ps(f1, _mm_set1_ps(w1))),
                                _mm_mul_ps(f2, _mm_set1_ps(w2)));
    __m128i r = _mm_cvttps_epi32(v);
    r = _mm_packs_epi32(r, r);
    r = _mm_packus_epi16(r, r);
    return static_cast<uint32_t>(_mm_cvtsi128_si32(r));
}

// clampU8((t * s) >> 7) for all four channels at once.
//
// The product of two `uint8_t` is at most 255*255 = 65,025, which fits in 16 bits, so
// `_mm_mullo_epi16` (low half) IS the exact product and `_mm_srli_epi16` is the exact `>> 7` on a
// non-negative value. The result is at most 508, and `_mm_packus_epi16` saturates it to 255 —
// which is precisely what `clampU8` does.
inline uint32_t g656ModulateQuad4(uint32_t texel, uint32_t shade)
{
    const __m128i zero = _mm_setzero_si128();
    __m128i t = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(texel)), zero);
    __m128i s = _mm_unpacklo_epi8(_mm_cvtsi32_si128(static_cast<int>(shade)), zero);
    __m128i p = _mm_srli_epi16(_mm_mullo_epi16(t, s), 7);
    p = _mm_packus_epi16(p, p);
    return static_cast<uint32_t>(_mm_cvtsi128_si32(p));
}
