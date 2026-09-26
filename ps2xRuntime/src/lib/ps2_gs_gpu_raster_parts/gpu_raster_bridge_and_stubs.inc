bool g178_backend_ready()
{
    if (!g178EnvEnabled())
        return false;
    G178Backend &backend = G178Backend::instance();
    // G705: this function has exactly one call site — the top of g178TryFlushGpu — so the thread
    // that reaches it first IS the GS front end, with or without MTGS. In fused mode that thread
    // adopts the GL context here, once. After adoption `fusedNeedsAdopt()` is one relaxed load.
    if (backend.fusedNeedsAdopt())
        backend.adoptFusedContext();
    return backend.ready();
}

// G705: fused-transport arm report, printed from the [G154:perf] window under DC2_G705_STAT=1.
void g705_backend_report()
{
    G178Backend::instance().g705Report();
}

bool g178_backend_submit(G178Batch &batch)
{
    return G178Backend::instance().submit(batch);
}

// G629: the exact CPU-band-replay compute job. Private .cpp-to-.cpp bridge, exactly like G570's —
// the frozen runtime headers stay untouched.
bool g629_backend_replay(const std::vector<uint32_t> &recs,
                         const std::vector<uint32_t> &texDesc,
                         const std::vector<uint32_t> &texData,
                         const std::vector<uint32_t> &tileOff,
                         const std::vector<uint32_t> &tileRef,
                         std::vector<uint32_t> &color,
                         std::vector<uint32_t> &depth,
                         int width, int height, int tilesX,
                         int block, int fbw, int firstPage,
                         bool wantDepth)
{
    return G178Backend::instance().submitG629(recs, texDesc, texData, tileOff, tileRef,
                                              color, depth, width, height, tilesX,
                                              block, fbw, firstPage, wantDepth);
}

bool g630_backend_submit(const std::shared_ptr<G630ReplayPacket> &packet)
{
    return G178Backend::instance().submitG630(packet);
}

bool g630_backend_prepare_ct32_view(const std::shared_ptr<G630ViewPacket> &packet)
{
    return G178Backend::instance().prepareG630Ct32View(packet);
}

bool g630_backend_submit_upload(const std::shared_ptr<G630UploadPacket> &packet)
{
    return G178Backend::instance().submitG630Upload(packet);
}

bool g630_backend_prepare_t8_view(const std::shared_ptr<G630T8Packet> &packet)
{
    return G178Backend::instance().prepareG630T8View(packet);
}

bool g634_backend_prepare_raw_view(const std::shared_ptr<G634RawViewPacket> &packet)
{
    return G178Backend::instance().prepareG634RawView(packet);
}

bool g634_backend_submit_raw_upload(const std::shared_ptr<G634RawUploadPacket> &packet)
{
    return G178Backend::instance().prepareG634RawUpload(packet);
}

// G714: one resolved same-format local->local transfer, executed against raw GS VRAM on the GPU.
bool g714_backend_submit_l2l(const std::shared_ptr<G714L2lPacket> &packet)
{
    return G178Backend::instance().prepareG714L2l(packet);
}

// G715 VRAMDST: the same submit, but the caller needs `packet->dstOut` filled before it continues.
bool g714_backend_submit_l2l_sync(const std::shared_ptr<G714L2lPacket> &packet)
{
    return G178Backend::instance().prepareG714L2lSync(packet);
}

bool g714_backend_available()
{
    return G178Backend::instance().g714Available();
}

bool g630_backend_poll(bool wait, std::vector<uint8_t> &results)
{
    return G178Backend::instance().pollG630(wait, results);
}

bool g630_backend_read_vram(std::vector<uint32_t> &out)
{
    return G178Backend::instance().readG630(false, out);
}

// G631: read only rows [rowLo, rowLo+rowCount) of the raw-VRAM image (row p == guest page p).
// The caller MUST size-check `out`: a backend that could not take the band path fills the whole
// 2048x512 image instead, and `out.size()` is the only signal of which one it got.
bool g630_backend_read_vram_rows(uint32_t rowLo, uint32_t rowCount, std::vector<uint32_t> &out)
{
    return G178Backend::instance().readG630(false, out, rowLo, rowCount);
}

bool g630_backend_read_depth(std::vector<uint32_t> &out)
{
    return G178Backend::instance().readG630(true, out);
}

bool g570_backend_shadow139(const std::vector<uint32_t> &color,
                            const std::vector<uint32_t> &depth,
                            const std::vector<uint32_t> &triWords,
                            const std::vector<uint32_t> &spanWords,
                            const std::vector<uint32_t> &tileOffsets,
                            const std::vector<uint32_t> &tileTris,
                            std::vector<uint32_t> &out,
                            int rectX, int rectY, int rectW, int rectH, int deriveSpan)
{
    return G178Backend::instance().submitG570Shadow139(
        color, depth, triWords, spanWords, tileOffsets, tileTris, out,
        rectX, rectY, rectW, rectH, deriveSpan);
}

// G541: asynchronous, no-readback VU1 transport probe. Private cpp-to-cpp bridge keeps the frozen
// runtime headers untouched.
bool g541_backend_null_compute()
{
    return G178Backend::instance().submitG541NullCompute();
}

// G545: move-owned live VU1 shadow command. Private bridge preserves frozen public headers.
bool g545_backend_live_submit(std::vector<uint8_t> &&command)
{
    return G178Backend::instance().submitG545Live(std::move(command));
}

bool g566_backend_authority_submit(
    std::vector<uint8_t> &&command, const std::shared_ptr<G566VuAuthorityResult> &result)
{
    return G178Backend::instance().submitG566Authority(std::move(command), result);
}

// G701: fire-and-forget submission of a readback-free batch. Returns false when the caller must
// take the blocking path. See the G701 block in lle_gpu_raster_backend.inc for why FIFO ordering
// on the single worker thread makes the wait unnecessary rather than merely optional.
bool g701_backend_submit_nowait(G178Batch &batch, uint64_t depthKey,
                                const std::vector<float> *depthUpload, int depthY, int depthRows)
{
    return G178Backend::instance().submitNoWait(batch, depthKey, depthUpload, depthY, depthRows);
}

// Latched failure from any earlier fire-and-forget submit; consumed and cleared.
bool g701_backend_take_failure()
{
    return G178Backend::instance().g701TakeFailure();
}

void g701_backend_report()
{
    G178Backend::instance().g701Report();
}

// G305: one-slot move-owned async submit / drain (default-off). See G178Backend::submitAsync.
bool g178_backend_submit_async(G178Batch &batch)
{
    return G178Backend::instance().submitAsync(batch);
}
bool g178_backend_drain_async(bool &ok)
{
    return G178Backend::instance().drainAsync(ok);
}

// G415 private .cpp-to-.cpp side channel. Publishes the exact GL row window that the front-end
// will consume out of the NEXT synchronous submit's colour readback. Consumed-and-cleared by that
// submit, so an unpublished submit keeps the previous full-height behaviour.
void g415_backend_set_color_window(int glY, int rows)
{
    g_g415PendWindow.valid = rows > 0 && glY >= 0;
    g_g415PendWindow.glY = glY;
    g_g415PendWindow.rows = rows;
}

// G242 private .cpp-to-.cpp side channel; intentionally not added to a header.
bool g242_backend_submit_depth(G178Batch &batch, uint64_t depthKey,
                               const std::vector<float> *depthUpload,
                               int depthY, int depthRows)
{
    return G178Backend::instance().submitDepth(batch, depthKey, depthUpload, depthY, depthRows);
}

bool g275_backend_submit_depth_readback(G178Batch &batch, uint64_t depthKey,
                                        const std::vector<float> *depthUpload,
                                        int depthY, int depthRows,
                                        int readY, int readRows,
                                        std::vector<float> &depthReadback)
{
    return G178Backend::instance().submitDepthReadback(
        batch, depthKey, depthUpload, depthY, depthRows,
        readY, readRows, depthReadback);
}

bool g242_backend_read_depth(uint64_t depthKey, int width, int height,
                             int depthY, int depthRows, std::vector<float> &depthReadback)
{
    const uint64_t g770T0 = g770XTraceActive() ? g770XTraceNow() : 0u; // G770 trace
    const bool ok = G178Backend::instance().readDepth(depthKey, width, height,
                                                      depthY, depthRows, depthReadback);
    if (g770T0 != 0u)
        g770XTrace('R', 1u, static_cast<uint32_t>(depthKey), static_cast<uint32_t>(depthRows), g770T0);
    return ok;
}

bool g178_backend_has_tex(uint64_t key)
{
    return G178Backend::instance().hasTex(key);
}

// G261: deferred RTT-wave materialization (front-end writes the rows back into guest VRAM).
bool g178_backend_read_color(uint32_t fbp, int width, int height, int glY, int rows,
                             std::vector<uint32_t> &out)
{
    const uint64_t g770T0 = g770XTraceActive() ? g770XTraceNow() : 0u; // G770 trace
    const bool ok = G178Backend::instance().readColor(fbp, width, height, glY, rows, out);
    if (g770T0 != 0u)
        g770XTrace('R', 0u, fbp, static_cast<uint32_t>(rows), g770T0);
    return ok;
}

// G264: upload-into-FBO mirror (front-end pushes guest upload bytes into the resident FBO).
bool g178_backend_write_color(uint32_t fbp, int width, int height, int glY, int rows,
                              const std::vector<uint32_t> &in)
{
    return G178Backend::instance().writeColor(fbp, width, height, 0, glY, width, rows, in);
}

// G264 alias-aware partial mirror. Kept as a private .cpp-to-.cpp bridge so the runtime's
// generated-target headers remain untouched.
bool g264_backend_write_color_rect(uint32_t fbp, int texWidth, int texHeight,
                                   int glX, int glY, int width, int rows,
                                   const std::vector<uint32_t> &in)
{
    return G178Backend::instance().writeColor(fbp, texWidth, texHeight,
                                               glX, glY, width, rows, in);
}

// G627: per-byte-lane masked VRAM->FBO patch. `mask`'s channels select which bytes of each CT32
// texel the patch replaces; the rest keep the FBO's value, so a PSMT8 copy that only partially
// covers its boundary CT32 words needs no destination readback.
bool g627_backend_write_color_rect_masked(uint32_t fbp, int texWidth, int texHeight,
                                          int glX, int glY, int width, int rows,
                                          const std::vector<uint32_t> &in,
                                          const std::vector<uint32_t> &mask)
{
    return G178Backend::instance().writeColorMasked(fbp, texWidth, texHeight,
                                                    glX, glY, width, rows, in, mask);
}

// G766: per-BIT masked VRAM->FBO patch: texel = (old & ~mask) | (in & mask). The general form of
// G627's lane patch for every sub-word guest write (PSMT8/PSMT4/PSMT4HL/PSMT4HH/PSMCT16).
bool g766_backend_write_color_rect_bits(uint32_t fbp, int texWidth, int texHeight,
                                        int glX, int glY, int width, int rows,
                                        const std::vector<uint32_t> &in,
                                        const std::vector<uint32_t> &mask)
{
    return G178Backend::instance().writeColorBits(fbp, texWidth, texHeight,
                                                  glX, glY, width, rows, in, mask);
}

// G769: fire-and-forget forms of the two patch writes above (the item owns the buffers; FIFO order
// is the only ordering any consumer of the patched FBO needs).
bool g769_backend_write_color_rect_async(uint32_t fbp, int texWidth, int texHeight, int glX,
                                         int glY, int width, int rows, std::vector<uint32_t> &&in)
{
    return G178Backend::instance().writeColorAsync(fbp, texWidth, texHeight, glX, glY, width, rows,
                                                   std::move(in), std::vector<uint32_t>{}, false);
}
bool g769_backend_write_color_rect_bits_async(uint32_t fbp, int texWidth, int texHeight, int glX,
                                              int glY, int width, int rows,
                                              std::vector<uint32_t> &&in,
                                              std::vector<uint32_t> &&mask)
{
    return G178Backend::instance().writeColorAsync(fbp, texWidth, texHeight, glX, glY, width, rows,
                                                   std::move(in), std::move(mask), true);
}
uint64_t g769_backend_async_write_fails()
{
    return G178Backend::instance().m_g769AsyncWriteFails.load(std::memory_order_relaxed);
}

// G280: physical-alias page view resolve (GPU->GPU CT32 page-tile copies between target FBOs).
bool g280_backend_copy_color_rects(uint32_t srcFbp, uint32_t dstFbp,
                                   const std::vector<int32_t> &rects6)
{
    return G178Backend::instance().copyColor(srcFbp, dstFbp, rects6);
}

// G766: arm post-upload page pulls for the NEXT synchronous batch whose FBO key is `dstKey`.
// `rects7` packs {srcKey, srcX, srcGlY, dstX, dstGlY, w, h} per page run, logical coordinates.
bool g766_backend_arm_pulls(uint32_t dstKey, const std::vector<int32_t> &rects7)
{
    if (rects7.empty() || (rects7.size() % 7u) != 0u)
        return false;
    std::lock_guard<std::mutex> lk(g_g766Post.mtx);
    g_g766Post.armed = true;
    g_g766Post.dstKey = dstKey;
    g_g766Post.rects7 = rects7;
    g_g766Post.lastApplied = false;
    ++g_g766Post.armedN;
    return true;
}

// True when the most recently armed pulls were applied by their batch.
bool g766_backend_last_pulls_applied()
{
    std::lock_guard<std::mutex> lk(g_g766Post.mtx);
    const bool ok = g_g766Post.lastApplied && !g_g766Post.armed;
    g_g766Post.armed = false; // never let an unconsumed arm leak into a later batch
    return ok;
}

void g766_backend_report()
{
    std::lock_guard<std::mutex> lk(g_g766Post.mtx);
    std::fprintf(stderr,
                 "[G766:post] armed=%llu applied=%llu rects=%llu mismatch=%llu fail=%llu\n",
                 static_cast<unsigned long long>(g_g766Post.armedN),
                 static_cast<unsigned long long>(g_g766Post.appliedN),
                 static_cast<unsigned long long>(g_g766Post.rectsN),
                 static_cast<unsigned long long>(g_g766Post.mismatchN),
                 static_cast<unsigned long long>(g_g766Post.failN));
}

// G767: arm the post-draw fold for the NEXT synchronous batch whose FBO key is `srcKey`.
// `rects7` packs {dstKey, sx, sy, dx, dy, w, h} per run, logical GL coordinates.
bool g767_backend_arm_folds(uint32_t srcKey, const std::vector<int32_t> &rects7)
{
    if (rects7.empty() || (rects7.size() % 7u) != 0u)
        return false;
    std::lock_guard<std::mutex> lk(g_g767Fold.mtx);
    g_g767Fold.armed = true;
    g_g767Fold.srcKey = srcKey;
    g_g767Fold.rects7 = rects7;
    g_g767Fold.lastApplied = false;
    ++g_g767Fold.armedN;
    return true;
}

// True when the most recently armed fold was applied (all of it) by its batch.
bool g767_backend_last_folds_applied()
{
    std::lock_guard<std::mutex> lk(g_g767Fold.mtx);
    const bool ok = g_g767Fold.lastApplied && !g_g767Fold.armed;
    g_g767Fold.armed = false; // never let an unconsumed arm leak into a later batch
    g_g767Fold.lastApplied = false;
    return ok;
}

// G767: CT32/CT24 page view — rects7 {srcKey, srcGsX, srcGsY, dstX, dstY, w, h}.
bool g767_backend_prepare_ct32_view(uint64_t texKey, int texW, int texH, bool ct24, int ta0,
                                    int aem, const std::vector<int32_t> &rects7,
                                    const std::vector<uint32_t> &vramPixels)
{
    return G178Backend::instance().prepareCt32PageView(texKey, texW, texH, ct24, ta0, aem, rects7,
                                                       vramPixels);
}

// G767: paletted page view — rects7 {srcKey|0, srcGsX|stageIdx, srcGsY, dstX, dstY, w, h}.
bool g767_backend_prepare_t8_view(uint64_t texKey, int texW, int texH, int kind,
                                  const std::vector<int32_t> &rects7,
                                  const std::vector<uint32_t> &vramPages,
                                  const std::vector<uint32_t> &clut256,
                                  const std::vector<uint32_t> &lut)
{
    return G178Backend::instance().prepareT8PageView(texKey, texW, texH, kind, rects7, vramPages,
                                                     clut256, lut);
}

// G767: fire-and-forget page views (inputs moved into a packet the worker owns).
bool g767_backend_queue_view(uint64_t texKey, int texW, int texH, bool ct24, int ta0, int aem,
                             int t8Kind, std::vector<int32_t> &&rects7,
                             std::vector<uint32_t> &&pixels, std::vector<uint32_t> &&clut,
                             const std::vector<uint32_t> *lut)
{
    return G178Backend::instance().prepareViewAsync(texKey, texW, texH, ct24, ta0, aem, t8Kind,
                                                    std::move(rects7), std::move(pixels),
                                                    std::move(clut), lut);
}

// G768: general page view (g768_page_view_members.inc) — fire-and-forget, FIFO-ordered.
bool g768_backend_queue_view(std::shared_ptr<G768ViewPacket> pk)
{
    return G178Backend::instance().queueG768View(std::move(pk));
}
uint64_t g768_backend_mirror_epoch()
{
    return G178Backend::instance().g768MirrorEpoch();
}
uint64_t g768_backend_exec_serial()
{
    return G178Backend::instance().g768ExecSerial();
}
bool g768_backend_read_view(uint64_t key, int w, int h, std::vector<uint32_t> &out)
{
    return G178Backend::instance().g768ReadViewSync(key, w, h, out);
}
void g768_backend_report()
{
    G178Backend::instance().g768Report();
}

// G767: GPU local->local byte move (see g767_l2l_members.inc).
bool g767_backend_l2l(const std::vector<int32_t> &gather, const std::vector<uint32_t> &map,
                      const std::vector<int32_t> &destTiles, const std::vector<int32_t> &scatter)
{
    return G178Backend::instance().runL2lMove(gather, map, destTiles, scatter);
}

void g767_backend_report()
{
    std::lock_guard<std::mutex> lk(g_g767Fold.mtx);
    std::fprintf(stderr,
                 "[G767:post] armed=%llu applied=%llu rects=%llu mismatch=%llu fail=%llu\n",
                 static_cast<unsigned long long>(g_g767Fold.armedN),
                 static_cast<unsigned long long>(g_g767Fold.appliedN),
                 static_cast<unsigned long long>(g_g767Fold.rectsN),
                 static_cast<unsigned long long>(g_g767Fold.mismatchN),
                 static_cast<unsigned long long>(g_g767Fold.failN));
}

// G675: the rect-aware form. `rect*` is the PCRTC read rectangle in LOGICAL GS coordinates
// (DISPFB DBX/DBY + DISPLAY DW/DH); the backend multiplies it by the native scale. A zero
// width/height means "whole surface".
bool g675_backend_read_display_rect(uint32_t fbp, uint32_t fallbackFbp,
                                    int32_t rectX, int32_t rectY, int32_t rectW, int32_t rectH,
                                    uint32_t &outW, uint32_t &outH,
                                    std::vector<uint8_t> &outRgba,
                                    dc2::gpu_present::FramePtr* gpuFrame = nullptr)
{
    return G178Backend::instance().readDisplayFbo(fbp, fallbackFbp, rectX, rectY, rectW, rectH,
                                                  outW, outH, outRgba, gpuFrame);
}

bool g178_backend_read_display_fbo(uint32_t fbp, uint32_t fallbackFbp, uint32_t &outW, uint32_t &outH,
                                   std::vector<uint8_t> &outRgba)
{
    // Rect-less legacy entry point (declared in ps2_gs_gpu_lle.h): whole display surface.
    return g675_backend_read_display_rect(fbp, fallbackFbp, 0, 0, 0, 0, outW, outH, outRgba);
}

bool g289_backend_copy_display_to_work(uint32_t srcFbp, uint32_t dstFbp)
{
    static const std::vector<int32_t> s_rect = {
        0, 96, 0, 96, 512, 416
    };
    return G178Backend::instance().copyColorEnsureDest(
        srcFbp, dstFbp, 512, 512, s_rect);
}

bool g289_backend_build_work_alias_view()
{
    // Physical base 0x2720 is two CT32 pages before owner base 0x2760. Remap the owner's
    // 8x13 page rectangle into that shifted logical view with two contiguous copies per row.
    // The private synthetic destination avoids contaminating the real 0x139 target FBO.
    static const std::vector<int32_t> s_rects = [] {
        std::vector<int32_t> r;
        r.reserve(27u * 6u);
        const auto add = [&](int sx, int sy, int dx, int dy, int w, int h) {
            r.push_back(sx); r.push_back(sy); r.push_back(dx);
            r.push_back(dy); r.push_back(w); r.push_back(h);
        };
        add(0, 480, 128, 480, 384, 32);
        for (int dstRow = 1; dstRow <= 12; ++dstRow)
        {
            add(384, 480 - (dstRow - 1) * 32,
                0, 480 - dstRow * 32, 128, 32);
            add(0, 480 - dstRow * 32,
                128, 480 - dstRow * 32, 384, 32);
        }
        add(384, 96, 0, 64, 128, 32);
        return r;
    }();
    return G178Backend::instance().copyColorEnsureDest(
        0x13bu, kG289AliasFbp, 512, 512, s_rects);
}

bool g281_backend_prepare_t8_view(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                                  int texW, int texH,
                                  const std::vector<uint32_t> &mapRgba,
                                  const std::vector<uint32_t> &clutRgba,
                                  bool verify, uint64_t &verifyBad)
{
    return G178Backend::instance().prepareT8View(
        texKey, srcFbp, srcFbW, texW, texH, mapRgba, clutRgba, verify, verifyBad);
}

bool g338_backend_prepare_ct24_view(uint64_t texKey, uint32_t srcFbp, int srcFbW,
                                    int texW, int texH, int ta0, int aem,
                                    bool verify, uint64_t &verifyBad)
{
    return G178Backend::instance().prepareCt24View(
        texKey, srcFbp, srcFbW, texW, texH, ta0, aem, verify, verifyBad);
}

bool g309_backend_build_authoritative_composite(
    const std::vector<uint32_t> &pageMap, const std::vector<uint32_t> &basePixels,
    bool readback,
    std::vector<uint32_t> &out)
{
    return G178Backend::instance().buildAuthoritativeComposite(
        pageMap, basePixels, readback, out);
}

bool g310_backend_init_logical(uint32_t seedFbp,
                               const std::vector<uint32_t> &guestPixels)
{
    // copyColorEnsureDest is the existing worker-owned allocator. The seed texel is immediately
    // overwritten by the full guest upload; requiring a live producer also proves the G310
    // admission has reached the native RTT path before creating private state.
    static const std::vector<int32_t> s_seedRect = {0, 0, 0, 0, 1, 1};
    G178Backend &backend = G178Backend::instance();
    return guestPixels.size() == 512u * 512u &&
           backend.copyColorEnsureDest(
               seedFbp, kG310LogicalFbp, 512, 512, s_seedRect) &&
           backend.writeColor(
               kG310LogicalFbp, 512, 512, 0, 0, 512, 512, guestPixels);
}

bool g310_backend_copy_logical_pages(uint32_t srcFbp,
                                     const std::vector<int32_t> &rects6)
{
    return G178Backend::instance().copyColor(
        srcFbp, kG310LogicalFbp, rects6);
}

bool g310_backend_build_logical_composite(
    const std::vector<uint32_t> &pageMap,
    const std::vector<uint32_t> &basePixels)
{
    return G178Backend::instance().buildLogicalComposite(pageMap, basePixels);
}

// Called once from PS2Runtime::initialize() (after g158CaptureMainContext). No-op unless
// DC2_G178_GPU=1.
void g178StartPersistentBackend()
{
    if (!g178EnvEnabled())
        return;
    G178Backend::instance().startEager();
    if (G178Backend::instance().ready())
        std::fprintf(stderr, "[G178:backend] persistent LLE GPU raster backend started\n");
}

// ⭐⭐⭐ G738: GL COMMAND-STREAM CENSUS BRIDGE.
//
// Defined at GLOBAL scope on purpose. The counters it reads are anonymous-namespace statics in
// g496_gl_state_cache.inc, which unqualified lookup reaches from here because this file is part of
// the same translation unit and is included after the namespace closes. The FUNCTION, however, must
// have external linkage: its caller is the cold reporter in ps2_g713_pipeline.cpp, and it is read
// on the EE thread once per `[G154:perf]` window.
//
// ⛔ This is the half of the fix that matters. The old reporter divided by `g_g496GlFrames`, which
// `renderBatch` incremented once per readback batch — measured 8.12 rendered frames on the shipped
// G732 image — and printed `frames=600` next to the result. No denominator is decided here; the
// caller supplies the window's own rendered-frame count.
void g738_glcall_snapshot(G738GlSnapshot &out)
{
    for (int i = 0; i < kG738GlClsCount; ++i)
    {
        out.issued[i] = g_g496GlIssued[i].load(std::memory_order_relaxed);
        out.elided[i] = g_g496GlElided[i].load(std::memory_order_relaxed);
    }
    out.drawBallastCalls = g_g738DrawBallastCalls.load(std::memory_order_relaxed);
    out.stateBallastCalls = g_g738StateBallastCalls.load(std::memory_order_relaxed);
    out.batches = g_g738Batches.load(std::memory_order_relaxed);
    out.drawsSeen = g_g738DrawsSeen.load(std::memory_order_relaxed);
}
