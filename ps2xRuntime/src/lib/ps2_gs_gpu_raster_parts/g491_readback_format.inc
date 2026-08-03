// ===== G491: native-format GS colour readback (exact by construction) ==========================
//
// PREMISE (measured on the CURRENT binary BEFORE a line was written — the G476 law).
//
//   [G447:edge] windowMs/f=23.50 | backend-render=3.48/34.0 backend-readcolor=3.39/6.0
//               backend-writecolor=0.37/11.0 backend-copy=0.10/2.0 backend-other=2.84/20.0
//               | blocked=10.75 cpu=12.75 (46% blocked)
//   [G446:gsprof] 19.68% NtWaitForAlertByThreadId  17.66% one PC inside g447WaitFuture
//   [G433:rb]     n=20992 usPerRead=698 nsPerPx=7.53           (lean: ~545 us/read)
//   [G425:rb]     redundant=0 (0.0%) distinctWindows=6         -> no coherence to cache
//   DC2_G419_AB=rb_all -> delta=-2.748 ms/f (-10.70%), blocked t=-10.15, 6/6 windows
//
// The GS worker's span IS the frame (23.5 vs 24.5 ms/f) and 46% of it is blocked on the GL
// backend. `backend-readcolor` is 3.2-3.4 ms/f over SIX calls, and the rb_all ceiling says 2.75 of
// those milliseconds are frame time — an 83% conversion, the highest of any edge in this arc.
//
// The two cheap ways out are already closed: G425 measures the readback redundancy at 0.0% (no
// temporal coherence to cache) and G433 measured the PBO transport SLOWER (22.79 vs 21.70 ns/px).
// What neither of them tested is the PIXEL FORMAT. 90,311 px = 361 KB in 545 us is ~660 MB/s,
// one to two orders below what an RGBA8 framebuffer read costs on this hardware, and the classic
// cause of exactly that number is a per-pixel channel swap inside the driver: `GL_RGBA` +
// `GL_UNSIGNED_BYTE` is the format the SPEC guarantees, not the one the framebuffer stores.
// GL exposes what it stores as GL_IMPLEMENTATION_COLOR_READ_FORMAT/_TYPE, and on every desktop
// driver of this generation that is GL_BGRA + GL_UNSIGNED_INT_8_8_8_8_REV.
//
// MECHANISM. Read in the implementation's own format and put the channels back on the CPU:
//   * `GL_BGRA` + `GL_UNSIGNED_INT_8_8_8_8_REV` yields, per pixel, the u32 0xAARRGGBB, i.e. the
//     memory bytes [B][G][R][A].
//   * The consumer contract (VRAM CT32 == RGBA8) wants [R][G][B][A], i.e. 0xAABBGGRR.
//   * So the fixup is one byte swap of lanes 0 and 2 per pixel — a single `_mm_shuffle_epi8` per
//     four pixels, ~30 us for the whole 361 KB window against a read that costs 545.
//
// EXACTNESS. The swap is a permutation of the same eight bits per channel; no value is rounded,
// clamped or reordered across pixels, and the tail below four pixels uses the identical scalar
// rotate. It is verified against the direct read in the same run by DC2_G491_RB_VERIFY=1, which
// performs BOTH reads into separate buffers and byte-compares them.
//
// FAILS CLOSED. If the driver does not report the packed BGRA pair, or `glReadPixels` sets an
// error with it, the arm latches OFF for the rest of the process and every later read takes the
// historical direct path. An unsupported driver is a perf non-event, never a correctness one.
//
// Levers: DC2_G419_AB=rbfmt (paired within-process gate), DC2_G491_RB_BGRA=1 (standalone arm),
// DC2_G491_NO_RB_BGRA=1 (rollback), DC2_G491_RB_VERIFY=1 (oracle),
// DC2_G491_RB_STAT=1 (`[G491:rbfmt]` — the probed format pair and the arm actually taken).

#include <emmintrin.h> // SSE2 only — see g491SwapBrInPlace

#ifndef GL491_BGRA
#define GL491_BGRA 0x80E1
#define GL491_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL491_IMPLEMENTATION_COLOR_READ_TYPE 0x8B9A
#define GL491_IMPLEMENTATION_COLOR_READ_FORMAT 0x8B9B
#endif

// NOTE: g491RbFmtArm() is defined at global scope in g419_ab_instrument.inc and declared in
// ps2_gs_gpu_raster.cpp ABOVE the anonymous namespace this part sits inside. Do not re-declare it
// here — a declaration inside the anon namespace binds to an internal-linkage symbol and fails
// (`C2129: declared but not defined`). That is the appendix's anonymous-namespace linkage trap.

static bool g491RbStatOn()
{
    static const bool on = (std::getenv("DC2_G491_RB_STAT") != nullptr);
    return on;
}

static bool g491RbVerifyOn()
{
    static const bool on = (std::getenv("DC2_G491_RB_VERIFY") != nullptr);
    return on;
}

// Standalone/default selector. The paired arm (DC2_G419_AB=rbfmt) overrides it when armed, exactly
// like g447SpinActive: the A/B owns the decision so the control arm is the shipped default.
//
// Default OFF during bring-up; promoted by the paired gate in §4 of the fix log.
static bool g491RbBgraRequested()
{
    static const bool on = (std::getenv("DC2_G491_RB_BGRA") != nullptr) &&
                           (std::getenv("DC2_G491_NO_RB_BGRA") == nullptr);
    return on;
}

// Latched OFF for the whole process by any driver-side refusal (see FAILS CLOSED above).
// GL-worker thread only, like every other resource in this backend.
static bool g_g491BgraUsable = true;

// One-shot probe of what this driver actually stores. Never gates behaviour on its own — the arm
// does — because a driver that reports something else may still accept the packed pair, and a
// driver that reports the pair may still be slow. It exists so the fix log can name the host.
static void g491ProbeReadFormat()
{
    static bool s_done = false;
    if (s_done)
        return;
    s_done = true;
    GLint fmt = 0, type = 0;
    glGetIntegerv(GL491_IMPLEMENTATION_COLOR_READ_FORMAT, &fmt);
    glGetIntegerv(GL491_IMPLEMENTATION_COLOR_READ_TYPE, &type);
    (void)glGetError(); // pre-3.0 drivers may reject the query; the arm does not depend on it
    if (g491RbStatOn())
        std::fprintf(stderr,
                     "[G491:rbfmt] implReadFormat=0x%x implReadType=0x%x (BGRA=0x%x REV=0x%x) "
                     "bgraArm=%d\n",
                     static_cast<unsigned>(fmt), static_cast<unsigned>(type),
                     (unsigned)GL491_BGRA, (unsigned)GL491_UNSIGNED_INT_8_8_8_8_REV,
                     g491RbBgraRequested() ? 1 : 0);
}

// [B][G][R][A] -> [R][G][B][A], in place. Lanes 1 and 3 are untouched, so this is a pure
// permutation of the bytes glReadPixels wrote.
//
// SSE2 only, on purpose: `_mm_shuffle_epi8` would do it in one instruction but needs SSSE3, and
// nothing else in this runtime requires above SSE2. The shift/mask form below is four pixels per
// six ops and is not the binding cost here (30 us against a 545 us read).
static void g491SwapBrInPlace(uint32_t *px, size_t n)
{
    size_t i = 0;
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
    const __m128i keepGA = _mm_set1_epi32(static_cast<int>(0xFF00FF00u));
    const __m128i lowB = _mm_set1_epi32(static_cast<int>(0x000000FFu));
    const __m128i hiB = _mm_set1_epi32(static_cast<int>(0x00FF0000u));
    for (; i + 4u <= n; i += 4u)
    {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(px + i));
        const __m128i ga = _mm_and_si128(v, keepGA);
        const __m128i r = _mm_and_si128(_mm_srli_epi32(v, 16), lowB);
        const __m128i b = _mm_and_si128(_mm_slli_epi32(v, 16), hiB);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(px + i),
                         _mm_or_si128(ga, _mm_or_si128(r, b)));
    }
#endif
    for (; i < n; ++i)
    {
        const uint32_t v = px[i];
        px[i] = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) | ((v & 0x000000FFu) << 16);
    }
}

// Read `rows` scanlines of the bound READ framebuffer in the driver's native packed format and
// restore RGBA byte order. `out` must already be sized to w*rows. Returns false without having
// written anything the caller can rely on ONLY when the driver refuses the format, in which case
// the arm is latched off and the caller falls back to the direct read.
static bool g491ReadPixelsNative(int glY, int w, int rows, uint32_t *out)
{
    if (!g_g491BgraUsable)
        return false;
    while (glGetError() != GL_NO_ERROR) {} // start from a clean error state (G257 house rule)
    glReadPixels(0, glY, w, rows, GL491_BGRA, GL491_UNSIGNED_INT_8_8_8_8_REV, out);
    if (glGetError() != GL_NO_ERROR)
    {
        g_g491BgraUsable = false;
        std::fprintf(stderr, "[G491:rbfmt] driver refused BGRA/8888_REV — latched OFF\n");
        return false;
    }
    g491SwapBrInPlace(out, static_cast<size_t>(w) * static_cast<size_t>(rows));
    return true;
}

// Same-run exact oracle (DC2_G491_RB_VERIFY=1): both reads, byte-compared. Halves readback
// throughput — bring-up only, never a timing arm.
static void g491VerifyNative(int glY, int w, int rows, const uint32_t *candidate)
{
    static std::vector<uint32_t> s_ref; // GL-worker thread only
    static uint64_t s_checked = 0ull, s_bad = 0ull, s_rep = 0ull;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(rows);
    s_ref.resize(n);
    glReadPixels(0, glY, w, rows, GL_RGBA, GL_UNSIGNED_BYTE, s_ref.data());
    if (std::memcmp(s_ref.data(), candidate, n * 4u) != 0)
    {
        if (++s_bad <= 8)
            std::fprintf(stderr, "[G491:rb-verify] MISMATCH glY=%d w=%d rows=%d bad=%llu\n",
                         glY, w, rows, static_cast<unsigned long long>(s_bad));
    }
    s_checked += n;
    if (++s_rep % 512u == 0u)
        std::fprintf(stderr, "[G491:rb-verify] checked=%llu px bad=%llu\n",
                     static_cast<unsigned long long>(s_checked),
                     static_cast<unsigned long long>(s_bad));
}

// The selector g433DoReadPixels calls. Paired arm wins when armed; otherwise the standalone flag.
static bool g491NativeReadActive()
{
    const int arm = g491RbFmtArm(); // -1 unless DC2_G419_AB=rbfmt
    if (arm >= 0)
        return arm == 1;
    return g491RbBgraRequested();
}
