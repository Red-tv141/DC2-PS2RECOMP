// G655 P17: normal-build contract for the G401 composite/surface probe.  Preserve the independent
// reserved-PSM causal switch exactly; it is behavior, while all composite sampling is diagnostic.
static constexpr bool g401CompositeOn() { return false; }

static bool g401IsDefinedTexturePsm(uint8_t psm)
{
    switch (psm & 0x3fu)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_T8:
    case GS_PSM_T4:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return true;
    default:
        return false;
    }
}

static bool g401SkipReservedTexturePsm(const GSPrimReg &prim, const GSContext &ctx)
{
    static const bool s_skip = (std::getenv("DC2_G401_SKIP_RESERVED_PSM") != nullptr);
    return s_skip && prim.tme && !g401IsDefinedTexturePsm(ctx.tex0.psm);
}

template <typename... T> static void g401NoteFrontDoor(T &&...) {}
template <typename... T> static void g401NoteCompositeDraw(T &&...) {}
template <typename... T> static void g401NoteCompositeEntries(T &&...) {}
template <typename... T> static void g401NoteDisplayFlush(T &&...) {}
