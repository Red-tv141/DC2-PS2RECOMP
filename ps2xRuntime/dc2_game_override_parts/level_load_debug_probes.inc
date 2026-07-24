static bool g186_sptrace_enabled()
{
    static const bool e = (std::getenv("DC2_G186_SPTRACE") != nullptr);
    return e;
}
static void g186_editdraw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t s_n = 0u;
    const uint32_t spIn = getRegU32(ctx, 29), raIn = getRegU32(ctx, 31);
    ++s_n;
    if (s_n <= 512u)
        std::fprintf(stderr, "[G186:editdraw-enter] n=%u spIn=0x%x raIn=0x%x\n", s_n, spIn, raIn);
    g_g186SpBalArmed = true;
    EditDraw__Fv_0x1ae3d0(rdram, ctx, runtime);
    g_g186SpBalArmed = false;
    const uint32_t spOut = getRegU32(ctx, 29);
    if (s_n <= 512u || spIn != spOut)
        std::fprintf(stderr, "[G186:editdraw] n=%u spIn=0x%x raIn=0x%x spOut=0x%x%s\n",
                     s_n, spIn, raIn, spOut, (spIn != spOut) ? " SP-DESYNC" : "");
}
static void g186_editloop_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t s_n = 0u;
    const uint32_t spIn = getRegU32(ctx, 29), raIn = getRegU32(ctx, 31);
    EditLoop__Fv_0x1abcf0(rdram, ctx, runtime);
    const uint32_t spOut = getRegU32(ctx, 29);
    ++s_n;
    if (s_n <= 512u || spIn != spOut)
        std::fprintf(stderr, "[G186:editloop] n=%u spIn=0x%x raIn=0x%x spOut=0x%x%s\n",
                     s_n, spIn, raIn, spOut, (spIn != spOut) ? " SP-DESYNC" : "");
}
