// ===== G654 P16/P17: THE LAYER TIMER THAT IS EXCLUSIVE AND THREAD-KEYED ======================
//
// WHY THIS EXISTS. `[G146:perf]` prints four "inclusive" layer accumulators — VIF1, GIFsubmit,
// GSimage, GSlocal — and the G653 baseline table subtracts their sum from `GS own` and calls the
// remainder a "residue". On `fight:rain` that residue is **−8.71 ms/f**, i.e. the four layers
// claim 34.06 ms/f of a 27.48 ms/f pole. A negative residue is not a measurement, and two
// structural facts explain it, both visible in the source rather than in the numbers:
//
//  1. **GIFsubmit is NESTED INSIDE VIF1.** `PS2Memory::processVIF1Data` opens a `G146PerfScope`
//     over its whole body, and the VIF1 unpack reaches `PS2Memory::submitGifPacket`, which opens a
//     second `G146PerfScope` over ITS whole body. `G146PerfScope` has no nesting guard, so every
//     nanosecond of a GIF submit reached through VIF1 is counted twice — once in each bucket.
//  2. **The four buckets are not all on one thread.** `G146PerfScope` is a plain wall-clock
//     accumulator into a global atomic, summed across whatever thread runs it. VIF1/GIFsubmit run
//     on the guest EE side; GSimage/GSlocal run on the GS worker. Their sum is therefore not
//     comparable to `GS own`, which is one thread's occupancy.
//
// This adds an EXCLUSIVE, THREAD-KEYED companion: a thread-local nesting depth per layer means only
// the outermost scope of a layer accumulates, and time spent inside a NESTED DIFFERENT layer is
// subtracted from the enclosing one, so the buckets partition rather than overlap. Totals are kept
// per host thread and reported per (thread, layer).
//
// ⛔ RULE 12c. This is a COMPILE-TIME BUILD MODE (`-DPS2X_G654_DIAG=ON`), not a runtime flag. Its
// call sites are `#if`-EXCLUDED, not predicated: G653 P25 measured ~60 lines of never-executed,
// `const bool`-guarded diagnostic code in a hot TU at +0.484 ms/f with no behavioural difference at
// all. In the default build every symbol below is an empty struct whose constructor and destructor
// have no body, so no global load, no branch and no byte survives.
//
// Layer ids are stable and shared with the reporter in ps2_g654_layerdiag.cpp (a separate, cold TU
// — rule 12b: the reporter never lands in a TU that carries a hot loop).

enum G654Layer
{
    kG654LayerVif1 = 0,     // PS2Memory::processVIF1Data
    kG654LayerGifSubmit,    // PS2Memory::submitGifPacket
    kG654LayerGsImage,      // the deferred host->local image writer / g144 drain
    kG654LayerGsLocal,      // register-triggered local->local transfers
    kG654LayerGifWalk,      // GS::processGIFPacket - the tag walk (P16)
    kG654LayerDrawPrim,     // drawPrimitive, reached from the tag walk (P16)
    kG654LayerCount
};

#if defined(PS2X_G654_DIAG)

// Defined in ps2_g654_layerdiag.cpp.
void g654LayerEnter(int layer);
void g654LayerExit(int layer, unsigned long long ns);
bool g654LayerOn();

struct G654LayerScope
{
    int layer;
    bool outer;
    std::chrono::steady_clock::time_point t0;

    explicit G654LayerScope(int l) : layer(l), outer(false)
    {
        if (!g654LayerOn())
            return;
        g654LayerEnter(layer);
        outer = true;
        t0 = std::chrono::steady_clock::now();
    }

    ~G654LayerScope()
    {
        if (!outer)
            return;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count();
        g654LayerExit(layer, static_cast<unsigned long long>(ns));
    }
};

#else

// Default build: no storage, no calls, no bytes. An empty aggregate with a defaulted destructor is
// removed entirely by MSVC at /O2 - the object never reaches a register.
struct G654LayerScope
{
    explicit G654LayerScope(int) {}
};

#endif // PS2X_G654_DIAG
