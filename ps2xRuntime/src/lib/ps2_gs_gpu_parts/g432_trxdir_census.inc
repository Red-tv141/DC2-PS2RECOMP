// ===== G432: TRXDIR cost census (behaviour-pure, default-off) =====
//
// G431 handed G432 the GS worker as the pole (frame sensitivity 1.05 vs VU1 0.49) and named
// `[G171:regstat] desc=0x0E addr=0x53` — the A+D write of TRXDIR — as 8.35-8.70 ms/f over ~199
// calls at 43.7 us each. But addr=0x53 is a SINGLE register that dispatches TWO very different
// transfers: TRXDIR=2 runs `performLocalToLocalTransfer` and TRXDIR=1 runs
// `performLocalToHostToBuffer`. One aggregate number cannot say which, nor say how much of it is
// the g242 depth-materialisation barrier (a blocking GPU round-trip) versus the serial per-pixel
// CPU gather out of swizzled VRAM.
//
// This census splits the bucket four ways with an independent clock (G425: `[G332:gsw]` inflates
// its own readback bucket ~50%, so the pre-existing instrument may not be used to size this):
//
//   dir=1 prep    : g242PrepareVramReadRect inside performLocalToHostToBuffer
//   dir=1 gather  : the per-pixel swizzled read loop that fills m_localToHostBuffer
//   dir=2         : performLocalToLocalTransfer, whole call
//   other         : a TRXDIR write that dispatched nothing
//
// Each bucket also carries pixels so a per-pixel rate can be compared against G424's measured
// PSMCT32 write rate, and dir=1 is additionally keyed by source PSM because the four gather
// branches (T4 / T8 / CT24-Z24 / CT32-else) have very different shapes.
//
// DC2_G432_CENSUS=1 arms it. Unarmed, every hook is one `static const bool` test (G268 rule: no
// per-call getenv on a path that runs ~200x/frame).

static bool g432CensusOn()
{
    static const bool on = envFlagEnabled("DC2_G432_CENSUS");
    return on;
}

struct G432Bucket
{
    uint64_t n = 0u;
    uint64_t ns = 0u;
    uint64_t px = 0u;
};

// TRXDIR dispatch happens on the GS worker (or the EE thread at a frame barrier) but never on both
// concurrently — same ownership contract the G261/G242 residency state already relies on. Plain
// scalars, no atomics, so the instrument cannot itself serialise the path it measures.
static G432Bucket s_g432Prep;                 // dir=1 g242 barrier
static G432Bucket s_g432Gather[5];            // dir=1 gather, keyed by g432PsmClass
static G432Bucket s_g432L2L;                  // dir=2 whole call
static G432Bucket s_g432Other;                // TRXDIR write that dispatched nothing
static uint64_t s_g432Reports = 0u;

// dir=2 internal split. The l2l call is not one thing: before any pixel moves it runs a G144
// deferred-draw flush, a G242 depth materialisation, an optional G289 GPU copy, and the G178
// write-ownership hook — each of which can block on the GPU worker. Only `body` is the CPU pixel
// loop that a G424-style run writer could attack.
static G432Bucket s_g432LFlush;   // g144FlushPendingUpload / range flush
static G432Bucket s_g432LPrep;    // g242PrepareVramReadRect
static G432Bucket s_g432LGpu;     // g289TryGpuLocalCopy
static G432Bucket s_g432LHook;    // g149BumpVramGen + g178NoteVramWriteRect
static G432Bucket s_g432LBody[3]; // 0 = G285 page memcpy, 1 = formatAware per-pixel, 2 = raw
static uint64_t s_g432LGpuTaken = 0u; // calls that returned early on the G289 GPU copy

static int g432PsmClass(uint32_t spsm)
{
    switch (spsm)
    {
    case GS_PSM_T4: case GS_PSM_T4HL: case GS_PSM_T4HH: return 0;
    case GS_PSM_T8:                                     return 1;
    case GS_PSM_CT24: case GS_PSM_Z24:                  return 2;
    case GS_PSM_CT32: case GS_PSM_Z32:                  return 3;
    default:                                            return 4;
    }
}

static const char *g432PsmClassName(int c)
{
    static const char *names[5] = {"t4", "t8", "ct24", "ct32", "other"};
    return names[c];
}

static void g432Report()
{
    if ((++s_g432Reports % 600u) != 0u)
        return;
    const auto emit = [](const char *tag, const G432Bucket &b) {
        if (b.n == 0u)
            return;
        std::fprintf(stderr, " %s(n=%llu ms=%.3f px=%.0f nsPerPx=%.2f)",
                     tag,
                     static_cast<unsigned long long>(b.n),
                     static_cast<double>(b.ns) / 1.0e6,
                     static_cast<double>(b.px),
                     b.px ? static_cast<double>(b.ns) / static_cast<double>(b.px) : 0.0);
    };
    std::fprintf(stderr, "[G432:trxdir] reports=%llu",
                 static_cast<unsigned long long>(s_g432Reports));
    emit("prep", s_g432Prep);
    for (int c = 0; c < 5; ++c)
        emit(g432PsmClassName(c), s_g432Gather[c]);
    emit("l2l", s_g432L2L);
    emit("l.flush", s_g432LFlush);
    emit("l.prep", s_g432LPrep);
    emit("l.gpu", s_g432LGpu);
    emit("l.hook", s_g432LHook);
    emit("l.page", s_g432LBody[0]);
    emit("l.fmt", s_g432LBody[1]);
    emit("l.raw", s_g432LBody[2]);
    emit("none", s_g432Other);
    std::fprintf(stderr, " gpuTaken=%llu\n",
                 static_cast<unsigned long long>(s_g432LGpuTaken));
}

// Scoped timer: one steady_clock pair, charged to `bucket` with `pixels` on destruction.
struct G432Scope
{
    G432Bucket *bucket;
    uint64_t pixels;
    std::chrono::steady_clock::time_point t0;
    explicit G432Scope(G432Bucket *b, uint64_t px)
        : bucket(b), pixels(px), t0(std::chrono::steady_clock::now())
    {
    }
    ~G432Scope()
    {
        if (bucket == nullptr)
            return;
        bucket->ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count());
        ++bucket->n;
        bucket->px += pixels;
    }
};
