// ===== G496: the GPU DERIVATIVE PROBE ==========================================================
//
// WHY THIS PART EXISTS.
//
// The arc has CPU-side derivative probes for both worker threads — `gsslow` (DC2_G431_GS_SLOW_US)
// and `vu1slow` (DC2_G303_VU1_SLOW_US) — and has used them for five phases to name the pole. It has
// never had one for the GPU. After G495 every remaining hypothesis in the arc depends on the answer:
//
//   * `[G447:edge]` splits the pole thread into cpu=12.6-13.4 + blocked=9.5-10.0 ms/f, and their sum
//     IS the frame, i.e. the GS worker's CPU and the GPU barely overlap (seven fences per frame
//     drain the pipeline to empty).
//   * G495 §3a proved the readback drain is the GPU catching up on submitted work (0 draws queued
//     ahead -> 0.33 ms drain; 410 draws ahead -> 1.65 ms), not a removable per-fence overhead.
//   * But `rb_all` deleting five of the six fences returns only 38% of what they were charged, so
//     the FENCES are not the cost. The GPU work behind them is the candidate — and `[G495:gpu]` can
//     only bound it from above, because a per-item GL_TIME_ELAPSED span contains idle time waiting
//     for commands to arrive (it reports rdcol=1.76 ms/item against a ~1.0 ms CPU-side wait for the
//     same item, which is impossible for true busy time).
//
// A derivative probe does not care about any of that. Inject a KNOWN quantity of extra GPU work into
// the same command stream, measure what the frame does, and divide. **Sensitivity ~1 means the GPU
// is the pole and the arc's target is fragment work; sensitivity ~0 means there is GPU slack and the
// target is the serialization instead.**
//
// THE BALLAST SHAPE — and why it is NOT the colour-mask form the G495 roadmap proposed.
//
// The roadmap's proposal was to re-issue the batch with glColorMask(0,0,0,0) + glDepthMask(FALSE)
// and depth test disabled. This backend already carries the counter-evidence, in the comment above
// the exact program's scratch attachment (lle_gpu_raster_backend.inc, `setupExactProgram` call
// site): *"Some Windows GL drivers elide a fragment invocation whose only framebuffer output is
// fully masked."* A ballast the driver is free to discard would inject nothing, and the probe would
// read sensitivity 0 — indistinguishable from a real "the GPU has slack" verdict, which is exactly
// the conclusion the phase is trying to reach. That is an inadmissible instrument.
//
// This ballast instead re-issues the batch's whole vertex range with an IDENTITY BLEND:
//
//     glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE)   =>   dst = 0*src + 1*dst = dst
//
// with depth test disabled and glDepthMask(GL_FALSE). Every fragment is rasterized, shaded, blended
// and WRITTEN — the driver cannot elide a real framebuffer write — and the byte written is the byte
// that was already there (dst*1 is exact for an 8-bit UNORM target, and no fragment reaches the
// depth buffer at all). One glDrawArrays over the whole VBO covers the batch's entire geometry, so
// the CPU-side cost added to the GL worker is one draw call per batch, not one per draw.
//
// EXCLUSION: batches on the G256 exact path are skipped outright. That program's fragment shader
// does an imageStore into the guest colour texture, which is a side effect NO blend mode can undo.
// (g256ExactRttOn() needs two env flags and is off on every default and every perf run, so this
// costs the probe nothing — it is a correctness guard, not a coverage compromise.)
//
// CALIBRATION (DC2_G496_CAL=1). The injected cost is measured, never assumed: a GL_TIME_ELAPSED
// query brackets the ballast draws and nothing else, harvested a full ring later so the read never
// waits on the GPU. The per-frame divisor is the GL worker's own frame clock — the ONE render batch
// per frame that reads back (G495 §2 established `rb/frames = 0.99`) — so the report is a true
// ms/frame, never cumulative-over-run ÷ frames (the G492 `fallback` trap).
//
// USAGE
//   calibration : DC2_G496_FORCE=1 DC2_G496_CAL=1   -> [G496:cal] injMs/f=<X>
//   sensitivity : DC2_G419_AB=gpuslow DC2_G438_HOLD=16, no other G496 flag
//                 sensitivity = -[G419:ab] blocked mean / X   (delta is +ve: the candidate is slower)
//   magnitude   : DC2_G496_N=<repeats, default 1>  DC2_G496_MOD=<ballast every Kth batch, default 1>
//
// DIAGNOSTIC ONLY. `gpuslow` deliberately wastes GPU time on the candidate arm; like `gsslow` and
// `vu1slow` it can never be promoted, and DC2_G496_FORCE is a calibration mode, never a shipping
// path.
// ==============================================================================================

// WHICH GL work class the ballast injects into. The two classes need different exactness arguments
// and answer different questions, so they are selected, calibrated and reported separately:
//
//   rend (default) — the 34 render batches / 849 draws. Needs the identity blend above, because the
//                    guest geometry is drawn INTO a live target with per-draw blend state.
//   pass           — the 19 full-screen 512x512 passes (9 G309 composites + 10 G281 T8 views + 1
//                    G338 CT24 view). These need no trick at all: each is a deterministic function
//                    of its bound sources rendered with GL_BLEND disabled into its own output, so
//                    re-issuing the SAME draw writes the SAME bytes. Idempotent by construction.
enum
{
    kG496SiteRend = 1,
    kG496SitePass = 2
};

static int g496SiteMask()
{
    static const int mask = [] {
        const char *value = std::getenv("DC2_G496_SITE");
        if (value == nullptr || value[0] == '\0' || std::strcmp(value, "rend") == 0)
            return static_cast<int>(kG496SiteRend);
        if (std::strcmp(value, "pass") == 0)
            return static_cast<int>(kG496SitePass);
        if (std::strcmp(value, "both") == 0)
            return static_cast<int>(kG496SiteRend | kG496SitePass);
        std::fprintf(stderr, "[G496] unknown DC2_G496_SITE='%s' (expected rend|pass|both)\n", value);
        return static_cast<int>(kG496SiteRend);
    }();
    return mask;
}

// Repeats of the batch's vertex range per ballasted batch.
static int g496BallastReps()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G496_N");
        if (value == nullptr || value[0] == '\0')
            return 1;
        return std::max(0, std::min(64, std::atoi(value)));
    }();
    return n;
}

// Ballast only every Kth render batch. The frame issues ~34, so this is the fine-grained magnitude
// control: a probe that injects far more than the frame's own budget measures a different regime.
static int g496BallastMod()
{
    static const int n = [] {
        const char *value = std::getenv("DC2_G496_MOD");
        if (value == nullptr || value[0] == '\0')
            return 1;
        return std::max(1, std::min(64, std::atoi(value)));
    }();
    return n;
}

// Calibration mode: run the ballast on EVERY frame regardless of the A/B arm, so the timer query
// below measures the per-candidate-frame injection without the arm halving it.
static bool g496ForceOn()
{
    static const bool on = (std::getenv("DC2_G496_FORCE") != nullptr);
    return on;
}

static bool g496CalOn()
{
    static const bool on = (std::getenv("DC2_G496_CAL") != nullptr);
    return on;
}

// The A/B arm (DC2_G419_AB=gpuslow). Defined at global scope in g419_ab_instrument.inc and declared
// in ps2_gs_gpu_raster.cpp above this TU's anonymous namespace (the appendix's linkage trap).
// Returns -1 when the lever is not selected, else 0 (control) / 1 (candidate).
static inline bool g496BallastArmed()
{
    if (g496ForceOn())
        return true;
    return g496GpuSlowArm() == 1;
}

// Repeats for THIS batch, or 0. `batchCounter` supplies the modulus so the ballasted batches are a
// deterministic subset rather than whichever ones happen to be large.
static inline int g496BallastForBatch(uint64_t batchCounter)
{
    const int reps = g496BallastReps();
    if (reps <= 0 || (g496SiteMask() & kG496SiteRend) == 0)
        return 0;
    if (!g496BallastArmed())
        return 0;
    const int mod = g496BallastMod();
    if (mod > 1 && (batchCounter % static_cast<uint64_t>(mod)) != 0ull)
        return 0;
    return reps;
}

// --- calibration timer -------------------------------------------------------------------------
#ifndef GL496_TIME_ELAPSED
#define GL496_TIME_ELAPSED 0x88BF
#define GL496_QUERY_RESULT 0x8866
#endif

typedef void(APIENTRY *PFN496GENQUERIES)(GLsizei, GLuint *);
typedef void(APIENTRY *PFN496BEGINQUERY)(GLenum, GLuint);
typedef void(APIENTRY *PFN496ENDQUERY)(GLenum);
typedef void(APIENTRY *PFN496GETQUERYOBJECTUI64V)(GLuint, GLenum, uint64_t *);

static constexpr int kG496Ring = 128;

struct G496Cal
{
    PFN496GENQUERIES glGenQueries_ = nullptr;
    PFN496BEGINQUERY glBeginQuery_ = nullptr;
    PFN496ENDQUERY glEndQuery_ = nullptr;
    PFN496GETQUERYOBJECTUI64V glGetQueryObjectui64v_ = nullptr;
    GLuint q[kG496Ring] = {};
    bool armed[kG496Ring] = {};
    bool tried = false, ok = false;
    int cursor = 0;
    bool active = false;
    uint64_t gpuNs = 0, batches = 0, drawsIssued = 0, vertsIssued = 0;
    uint64_t lastGpuNs = 0, lastBatches = 0, lastDraws = 0, lastVerts = 0;
    uint64_t frames = 0, lastFrames = 0;
};

static G496Cal g_g496Cal;

static bool g496CalReady()
{
    G496Cal &g = g_g496Cal;
    if (!g.tried)
    {
        g.tried = true;
        g.ok = loadProc(g.glGenQueries_, "glGenQueries") &&
               loadProc(g.glBeginQuery_, "glBeginQuery") &&
               loadProc(g.glEndQuery_, "glEndQuery") &&
               loadProc(g.glGetQueryObjectui64v_, "glGetQueryObjectui64v");
        if (g.ok)
            g.glGenQueries_(kG496Ring, g.q);
        std::fprintf(stderr, "[G496:cal] timer queries %s (reps=%d mod=%d force=%d)\n",
                     g.ok ? "loaded" : "UNAVAILABLE", g496BallastReps(), g496BallastMod(),
                     g496ForceOn() ? 1 : 0);
    }
    return g.ok;
}

static inline void g496CalBegin()
{
    if (!g496CalOn() || !g496CalReady())
        return;
    G496Cal &g = g_g496Cal;
    const int i = g.cursor;
    if (g.armed[i])
    {
        uint64_t ns = 0ull;
        g.glGetQueryObjectui64v_(g.q[i], GL496_QUERY_RESULT, &ns);
        g.gpuNs += ns;
        ++g.batches;
        g.armed[i] = false;
    }
    g.glBeginQuery_(GL496_TIME_ELAPSED, g.q[i]);
    g.active = true;
}

static inline void g496CalEnd(uint64_t draws, uint64_t verts)
{
    G496Cal &g = g_g496Cal;
    if (!g.active)
        return;
    g.glEndQuery_(GL496_TIME_ELAPSED);
    g.armed[g.cursor] = true;
    g.active = false;
    g.cursor = (g.cursor + 1) % kG496Ring;
    g.drawsIssued += draws;
    g.vertsIssued += verts;
}

// The full-screen-pass ballast. Called immediately after the pass's own glDrawArrays, with every
// piece of GL state (program, FBO, viewport, bound sources, uniforms) still exactly as that draw
// left it — so the repeat is the SAME draw and writes the SAME bytes. No state is touched and none
// needs restoring. Its own GL-worker-private sequence counter supplies the modulus, so
// DC2_G496_MOD selects a deterministic subset of the frame's 19 passes.
static uint64_t g_g496PassSeq = 0ull;

static inline void g496PassBallast()
{
    const int reps = g496BallastReps();
    if (reps <= 0 || (g496SiteMask() & kG496SitePass) == 0)
        return;
    const uint64_t seq = g_g496PassSeq++;
    if (!g496BallastArmed())
        return;
    const int mod = g496BallastMod();
    if (mod > 1 && (seq % static_cast<uint64_t>(mod)) != 0ull)
        return;
    g496CalBegin();
    for (int rep = 0; rep < reps; ++rep)
        glDrawArrays(GL_TRIANGLES, 0, 3);
    g496CalEnd(static_cast<uint64_t>(reps), 3ull * static_cast<uint64_t>(reps));
}

// GL-worker frame clock: the ONE render batch per frame that reads back (G495 §2). Every figure the
// report prints is a WINDOW DELTA against the previous report.
static inline void g496NoteFrame()
{
    if (!g496CalOn())
        return;
    G496Cal &g = g_g496Cal;
    ++g.frames;
    if ((g.frames % 600ull) != 0ull)
        return;
    const double frames = static_cast<double>(g.frames - g.lastFrames);
    const double harvested = static_cast<double>(g.batches - g.lastBatches);
    if (frames <= 0.0)
        return;
    const double injMs = static_cast<double>(g.gpuNs - g.lastGpuNs) / 1.0e6 / frames;
    std::fprintf(stderr,
                 "[G496:cal] win frames=%.0f injMs/f=%.3f ballast(batches/f=%.1f draws/f=%.1f "
                 "verts/f=%.0f) gpuMs/batch=%.3f\n",
                 frames, injMs, harvested / frames,
                 static_cast<double>(g.drawsIssued - g.lastDraws) / frames,
                 static_cast<double>(g.vertsIssued - g.lastVerts) / frames,
                 harvested > 0.0
                     ? static_cast<double>(g.gpuNs - g.lastGpuNs) / 1.0e6 / harvested
                     : 0.0);
    g.lastFrames = g.frames;
    g.lastBatches = g.batches;
    g.lastGpuNs = g.gpuNs;
    g.lastDraws = g.drawsIssued;
    g.lastVerts = g.vertsIssued;
}
