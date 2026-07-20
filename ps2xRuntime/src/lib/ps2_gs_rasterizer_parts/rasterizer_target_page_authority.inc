// A resident target's FBO content changed over GS rows [rowLo..rowHi]. Bump the epoch of every
// physical CT32 page the target defines in those rows, so any overlay/fan-out copy of the SAME
// physical page made from a DIFFERENT alias is provably stale by epoch mismatch. Cheap and gated:
// authority-off returns immediately, and the measured family is at most 13x8 pages.
static void g283NoteTargetPages(int ti, int rowLo, int rowHi)
{
    if ((!g283AuthorityOn() && !g294OwnerTokenOn()) ||
        ti < 0 || ti >= static_cast<int>(kG248TargetCount))
        return;
    const bool g283On = g283AuthorityOn(); // epoch bumps only under G283; owner stamps under G294
    const G261Res &r = g_g261Res[ti];
    if (r.fbw == 0u || r.fbW <= 0)
        return;
    const uint32_t base = framePageBaseToBlock(kG261Fbp[ti]);
    const int pcols = r.fbW / 64;
    const int lo = std::max(0, rowLo);
    const int hi = std::min(511, rowHi);
    for (int y = (lo / 32) * 32; y <= hi; y += 32)
    {
        const uint32_t prow = static_cast<uint32_t>(y) / 32u;
        for (int col = 0; col < pcols; ++col)
        {
            const uint64_t first64 = static_cast<uint64_t>(base) +
                (static_cast<uint64_t>(prow) * r.fbw + static_cast<uint32_t>(col)) * 32u;
            if (first64 <= 0x3FFFu)
            {
                if (g283On)
                    g283BumpPageEpoch(static_cast<uint32_t>(first64));
                g294StampOwner(ti, static_cast<uint32_t>(first64)); // G294: fresh authority
            }
        }
    }
}

static bool g282StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G282_STAT");
    return s_on;
}

static bool g282VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G282_VERIFY");
    return s_on;
}

static bool g281StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G281_STAT");
    return s_on;
}

static bool g281VerifyOn()
{
    static const bool s_on = envFlagEnabled("DC2_G281_VERIFY");
    return s_on;
}

static bool g280StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G280_STAT");
    return s_on;
}