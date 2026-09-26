// G764: upload the native snapshot once; perform the exact integer nearest
// resample into the existing presentation texture on the GPU. The second sampler
// (DrawTexturePro to the window) remains unchanged, including its rounding.
namespace g764_present
{
static Shader shader{};
static Texture2D source{};
static unsigned int framebuffer = 0;
static bool tried = false;
static int sizeLocation = -1;
static int opaqueLocation = -1;
static bool enabled()
{
    static const bool on = (runtimeEnvFlagEnabled("DC2_G764_GPU_PRESENT_SCALE") ||
                           dc2::gpu_present::enabled()) &&
                           !runtimeEnvFlagEnabled("DC2_G764_NO_PRESENT_SCALE");
    return on;
}
static bool setup()
{
    if (!enabled()) return false;
    if (!tried)
    {
        tried = true;
        static const char* fs =
            "#version 330\n"
            "uniform sampler2D texture0;\n"
            "uniform ivec2 uOutputSize;\n"
            "uniform int uOpaque;\n"
            "out vec4 finalColor;\n"
            "void main() {\n"
            "  ivec2 d = ivec2(gl_FragCoord.xy);\n"
            "  ivec2 src = (d * textureSize(texture0, 0)) / uOutputSize;\n"
            "  finalColor = texelFetch(texture0, src, 0);\n"
            "  if (uOpaque != 0) finalColor.a = 1.0;\n"
            "}\n";
        shader = LoadShaderFromMemory(nullptr, fs);
        sizeLocation = GetShaderLocation(shader, "uOutputSize");
        opaqueLocation = GetShaderLocation(shader, "uOpaque");
        if (sizeLocation >= 0) framebuffer = rlLoadFramebuffer();
        std::fprintf(stderr, "[G764:present] GPU integer resample %s\n",
                     sizeLocation >= 0 && framebuffer ? "ready" : "unavailable; CPU fallback");
    }
    return sizeLocation >= 0 && framebuffer != 0;
}
static bool draw(Texture2D& target, Texture2D input, int virtualW, int virtualH, bool opaque)
{
    if (target.width != virtualW || target.height != virtualH || !setup()) return false;
    rlFramebufferAttach(framebuffer, target.id, RL_ATTACHMENT_COLOR_CHANNEL0,
                        RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(framebuffer)) return false;
    const int outputSize[2] = {virtualW, virtualH};
    const int normalizeAlpha = opaque ? 1 : 0;
    SetShaderValue(shader, sizeLocation, outputSize, SHADER_UNIFORM_IVEC2);
    SetShaderValue(shader, opaqueLocation, &normalizeAlpha, SHADER_UNIFORM_INT);
    RenderTexture2D destination{};
    destination.id = framebuffer; destination.texture = target;
    BeginTextureMode(destination);
    BeginShaderMode(shader);
    rlDisableColorBlend();
    DrawTexturePro(input, Rectangle{0, 0, float(input.width), float(input.height)},
                   Rectangle{0, 0, float(virtualW), float(virtualH)}, Vector2{0, 0}, 0, WHITE);
    EndShaderMode();
    rlEnableColorBlend();
    EndTextureMode();
    return true;
}
static bool uploadShared(Texture2D& target, const dc2::gpu_present::FramePtr& frame,
                         int virtualW, int virtualH)
{
    if (!frame || !setup() || !dc2::gpu_present::waitReady(frame)) return false;
    Texture2D input{frame->texture, int(frame->width), int(frame->height), 1,
                    PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    const bool ok = draw(target, input, virtualW, virtualH, true);
    // Same-frame, end-to-end graphical oracle. Script-frame comparisons on a
    // faster MAP-15 build can capture different camera poses. Here both paths
    // consume the *same* immutable snapshot, so a mismatch is real. Diagnostic
    // only: this deliberately brings back the readback avoided by this path.
    static const bool verify = runtimeEnvFlagEnabled("DC2_G764_SHARED_FINAL_VERIFY");
    if (ok && verify)
    {
        static uint64_t checked = 0, badTotal = 0;
        std::vector<uint8_t> source;
        Image actual = LoadImageFromTexture(target);
        uint64_t bad = 0;
        if (!dc2::gpu_present::readPixels(frame, source) || !actual.data ||
            actual.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 ||
            actual.width != virtualW || actual.height != virtualH ||
            source.size() != size_t(frame->width) * frame->height * 4u)
            bad = uint64_t(virtualW) * virtualH;
        else
        {
            const auto* got = static_cast<const uint32_t*>(actual.data);
            const auto* src = reinterpret_cast<const uint32_t*>(source.data());
            for (int y = 0; y < virtualH; ++y)
                for (int x = 0; x < virtualW; ++x)
                {
                    const uint32_t sy = uint32_t(y) * frame->height / uint32_t(virtualH);
                    const uint32_t sx = uint32_t(x) * frame->width / uint32_t(virtualW);
                    const auto pixel = got[size_t(y)*virtualW+x];
                    // Both readPixels and LoadImageFromTexture expose this FBO
                    // in bottom-origin order. Do not flip either operand.
                    bad += pixel != src[size_t(sy)*frame->width+sx];
                }
        }
        UnloadImage(actual);
        ++checked; badTotal += bad;
        if (bad || checked <= 8 || checked % 128 == 0)
            std::fprintf(stderr, "[G764:shared-final-verify] checks=%llu bad=%llu last=%llu\n",
                         (unsigned long long)checked, (unsigned long long)badTotal,
                         (unsigned long long)bad);
    }
    dc2::gpu_present::finishRead(frame);
    return ok;
}
static bool upload(Texture2D& target, const std::vector<uint8_t>& pixels,
                   uint32_t width, uint32_t height, int virtualW, int virtualH)
{
    if (!width || !height || width > 8192 || height > 8192 ||
        pixels.size() != size_t(width) * height * 4 || !setup()) return false;
    if (target.width != virtualW || target.height != virtualH) return false;
    if (source.width != int(width) || source.height != int(height))
    {
        Image image{const_cast<uint8_t*>(pixels.data()), int(width), int(height), 1,
                    PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        Texture2D replacement = LoadTextureFromImage(image);
        if (!replacement.id) return false;
        if (source.id) UnloadTexture(source);
        source = replacement;
        SetTextureFilter(source, TEXTURE_FILTER_POINT);
    }
    else UpdateTexture(source, pixels.data());
    if (!draw(target, source, virtualW, virtualH, false)) return false;
    static const bool verify = runtimeEnvFlagEnabled("DC2_G764_PRESENT_VERIFY");
    if (verify)
    {
        static uint64_t calls = 0, checked = 0, badTotal = 0;
        if (++calls <= 8 || calls % 128 == 0)
        {
            Image actual = LoadImageFromTexture(target);
            uint64_t bad = 0;
            if (actual.data && actual.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 &&
                actual.width == virtualW && actual.height == virtualH)
            {
                const auto* rgba = static_cast<const uint32_t*>(actual.data);
                const auto* original = reinterpret_cast<const uint32_t*>(pixels.data());
                for (int y = 0; y < virtualH; ++y) for (int x = 0; x < virtualW; ++x)
                    bad += rgba[size_t(y)*virtualW+x] !=
                        original[size_t(uint32_t(y)*height/virtualH)*width+uint32_t(x)*width/virtualW];
            }
            else bad = uint64_t(virtualW)*virtualH;
            UnloadImage(actual); ++checked; badTotal += bad;
            std::fprintf(stderr, "[G764:present-verify] checks=%llu bad=%llu last=%llu src=%ux%u dst=%dx%d\n",
                (unsigned long long)checked, (unsigned long long)badTotal,
                (unsigned long long)bad, width, height, virtualW, virtualH);
            if (bad) return false;
        }
    }
    return true;
}
static void capture(uint32_t scriptFrame)
{
    static const char* tag = std::getenv("DC2_G764_SCREEN_TAG");
    if (!tag || !*tag) return;
    static bool safe = [] { for (const char* p = tag; *p; ++p)
        if (!std::isalnum(static_cast<unsigned char>(*p)) && *p != '_') return false; return true; }();
    if (!safe || scriptFrame < 600 || scriptFrame > 4000) return;
    const uint32_t bucket = scriptFrame / 80;
    static uint32_t previous = 0;
    if (previous == bucket) return;
    previous = bucket;
    rlDrawRenderBatchActive();
    Image screen = LoadImageFromScreen();
    char path[256];
    std::snprintf(path, sizeof(path), "captures/screen_%s_%06u.png", tag, bucket*80);
    ExportImage(screen, path); UnloadImage(screen);
}
static void shutdown()
{
    if (source.id) UnloadTexture(source);
    if (framebuffer) rlUnloadFramebuffer(framebuffer);
    if (sizeLocation >= 0) UnloadShader(shader);
    source = {}; shader = {}; framebuffer = 0; tried = false; sizeLocation = opaqueLocation = -1;
}
}
