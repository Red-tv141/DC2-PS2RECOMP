bool g178_backend_ready()
{
    return g178EnvEnabled() && G178Backend::instance().ready();
}

bool g178_backend_submit(G178Batch &batch)
{
    return G178Backend::instance().submit(batch);
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
    return G178Backend::instance().readDepth(depthKey, width, height,
                                             depthY, depthRows, depthReadback);
}

bool g178_backend_has_tex(uint64_t key)
{
    return G178Backend::instance().hasTex(key);
}

// G261: deferred RTT-wave materialization (front-end writes the rows back into guest VRAM).
bool g178_backend_read_color(uint32_t fbp, int width, int height, int glY, int rows,
                             std::vector<uint32_t> &out)
{
    return G178Backend::instance().readColor(fbp, width, height, glY, rows, out);
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

// G280: physical-alias page view resolve (GPU->GPU CT32 page-tile copies between target FBOs).
bool g280_backend_copy_color_rects(uint32_t srcFbp, uint32_t dstFbp,
                                   const std::vector<int32_t> &rects6)
{
    return G178Backend::instance().copyColor(srcFbp, dstFbp, rects6);
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

