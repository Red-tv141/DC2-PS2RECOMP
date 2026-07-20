// ===== G280 bodies (state + forward decls beside the G264 telemetry above) =====
static bool g280AliasOn()
{
    // Bring-up default: OFF. Enable with DC2_G280_ALIAS_VIEW=1; DC2_G280_NO_ALIAS_VIEW=1 (or
    // =0) always disables. Rides the wave stack, so every established oracle/diagnostic decline
    // in g261WaveOn applies; additionally declines under both consumer-edge censuses so their
    // measurements stay behavior-pure (the G267/G277 census must observe the legacy cadence).
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G280_NO_ALIAS_VIEW") || envFlagDisabled("DC2_G280_ALIAS_VIEW"))
            return false;
        // G281 is a paying child configuration of the G280 page-view mechanism. Requesting it
        // implies G280 so a same-binary A/B needs one behavior lever, while the G280 kill still
        // tears down the complete view stack.
        const bool g282Requested =
            envFlagEnabled("DC2_G282_TIGHT_ROWS") &&
            !envFlagEnabled("DC2_G282_NO_TIGHT_ROWS") &&
            !envFlagDisabled("DC2_G282_TIGHT_ROWS");
        // G283 authority is a paying child of the whole view/fan-out stack: requesting it implies
        // G282 (and thus G281/G280) so a same-binary A/B needs one behavior lever.
        const bool g283Requested =
            envFlagEnabled("DC2_G283_AUTHORITY") &&
            !envFlagEnabled("DC2_G283_NO_AUTHORITY") &&
            !envFlagDisabled("DC2_G283_AUTHORITY");
        return envFlagEnabled("DC2_G280_ALIAS_VIEW") ||
               envFlagEnabled("DC2_G281_T8_VIEW") ||
               g282Requested || g283Requested || g294OwnerTokenRequested();
    }();
    return s_on && g261WaveOn() && !g267CensusOn() && !g277CensusOn();
}

static bool g281ViewOn()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G281_NO_T8_VIEW") ||
            envFlagDisabled("DC2_G281_T8_VIEW"))
            return false;
        const bool g282Requested =
            envFlagEnabled("DC2_G282_TIGHT_ROWS") &&
            !envFlagEnabled("DC2_G282_NO_TIGHT_ROWS") &&
            !envFlagDisabled("DC2_G282_TIGHT_ROWS");
        const bool g283Requested =
            envFlagEnabled("DC2_G283_AUTHORITY") &&
            !envFlagEnabled("DC2_G283_NO_AUTHORITY") &&
            !envFlagDisabled("DC2_G283_AUTHORITY");
        return envFlagEnabled("DC2_G281_T8_VIEW") ||
               g282Requested || g283Requested;
    }();
    return s_on && g280AliasOn();
}

static bool g282TightRowsOn()
{
    static const bool s_on = []() {
        if (envFlagEnabled("DC2_G282_NO_TIGHT_ROWS") ||
            envFlagDisabled("DC2_G282_TIGHT_ROWS"))
            return false;
        const bool g283Requested =
            envFlagEnabled("DC2_G283_AUTHORITY") &&
            !envFlagEnabled("DC2_G283_NO_AUTHORITY") &&
            !envFlagDisabled("DC2_G283_AUTHORITY");
        return envFlagEnabled("DC2_G282_TIGHT_ROWS") || g283Requested;
    }();
    return s_on && g281ViewOn();
}

// G290 (bring-up opt-in DC2_G290_EXACT_ROWS=1; kills DC2_G290_NO_EXACT_ROWS=1 /
// DC2_G290_EXACT_ROWS=0): exact vertex-bound rows for the measured fbp=0x139/fbw=8 CT32 batch
// with READ-ONLY zbp=0xd0 Z24 depth. The legacy scissor row union (448 rows) makes the
// conservative color/Z page test a FALSE alias (color pages 313..319 vs Z page row 13); the
// per-entry capture bounds prove rows 0..415 → color pages 313..416 and Z pages 208..311 are
// physically disjoint (same false-alias class G273 removed for the 0x68 display tuple).
// Independent of the default-off G280/G281/G282 alias-view stack.
// Every 64x32 page tile of resident target `fbp` whose block interval overlaps `rd` must be
// fully upload-re-covered (all 2048 pixel bits set in g291Cov) for the read to skip
// materialization. Fail-closed on any layout mismatch, invalid range, or partial coverage.
static bool g291PagesCurrent(const G261Res &r, uint32_t fbp, const G144BlockRange &rd)
{
    if (!rd.valid || r.g291CovFbw == 0u || r.g291CovFbw != r.fbw ||
        r.fbw > 8u || r.fbW != static_cast<int>(r.fbw) * 64)
    {
        static std::atomic<uint32_t> s_shapeLog{0};
        if (g291StatOn() && s_shapeLog.fetch_add(1u, std::memory_order_relaxed) < 8u)
            std::fprintf(stderr,
                         "[G291:fail-shape] t=%03x valid=%d covFbw=%u fbw=%u fbW=%d\n",
                         fbp, rd.valid ? 1 : 0, r.g291CovFbw, r.fbw, r.fbW);
        return false;
    }
    const uint32_t base = framePageBaseToBlock(fbp);
    const uint32_t pages = 16u * r.fbw; // 512 rows / 32 per page-row
    for (uint32_t p = 0; p < pages; ++p)
    {
        const uint32_t b0 = base + p * 32u;
        const uint32_t b1 = b0 + 31u;
        if (b1 < rd.first || b0 > rd.last)
            continue; // page outside the read
        const uint32_t py = p / r.fbw;
        const uint32_t px = p % r.fbw;
        const uint32_t rowLo = py * 32u;
        const uint32_t word = px; // 64 pixels per uint64 word, page cols are word-aligned
        for (uint32_t y = rowLo; y < rowLo + 32u; ++y)
            if (r.g291Cov[y][word] != ~uint64_t{0})
            {
                static std::atomic<uint32_t> s_pageLog{0};
                if (g291StatOn() && fbp == 0x139u &&
                    s_pageLog.fetch_add(1u, std::memory_order_relaxed) < 24u)
                    std::fprintf(stderr,
                                 "[G291:fail-page] t=%03x rd=0x%x..0x%x page=%u py=%u px=%u "
                                 "y=%u cov=0x%016llx\n",
                                 fbp, rd.first, rd.last, p, py, px, y,
                                 (unsigned long long)r.g291Cov[y][word]);
                return false;
            }
    }
    return true;
}
static bool g290ExactRowsOn()
{
    static const bool s_on = (envFlagEnabled("DC2_G290_EXACT_ROWS") || g291AtlasOn()) &&
                             !envFlagEnabled("DC2_G290_NO_EXACT_ROWS") &&
                             !envFlagDisabled("DC2_G290_EXACT_ROWS");
    return s_on;
}
static bool g290StatOn()
{
    static const bool s_on = envFlagEnabled("DC2_G290_STAT");
    return s_on;
}
static std::atomic<uint64_t> g_g290ExactAdmit{0}, g_g290ExactRejOverlap{0}, g_g290ExactRejShape{0};

// G308 retained diagnosis (default-off DC2_G308_UPLOAD_CENSUS=1): report how many CT32 rows the
// exact front-end staging window contains versus today's frozen full-surface backend upload.
// The compact behavior prototype was exact but economically neutral and was removed.
static bool g308UploadCensusOn()
{
    static const bool s_on = envFlagEnabled("DC2_G308_UPLOAD_CENSUS");
    return s_on;
}
static std::atomic<uint64_t> g_g308Uploads{0}, g_g308Rows{0}, g_g308FullRows{0};