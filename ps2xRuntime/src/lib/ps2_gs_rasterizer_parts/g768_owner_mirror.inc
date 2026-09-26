// ===== G768: A G289 COPY-OWNER PUBLICATION IS A GUEST WRITE — mirror it, do not "escape" it =====
//
// G767 §6 blamed the T8 page view's regression on the inline CPU draw path (G582 bump source 6).
// ⛔ That reading was wrong: `[G570:bump-owner] src=6 bw=0 psm=ff` is g572BumpOwnedBlockRange's
// SENTINEL — "exact G289 owner publication, physical block interval" — which reuses source id 6.
// `[G768:inlesc]` (DC2_G768_INLESC=1) proves it on s05: 889 escapes, `inl=0` (the inline prologue
// never even ran), zero inline writes into any resident window.
//
// The mechanism. g345MaterializeSlot reads the G289 copy owner's FBO back and writes its owned
// blocks into guest VRAM, bumping their page generations. Those blocks lie inside 0x139's wide
// claimed window (pages 0x181.. / 0x19a.. on dungeon1 / s05). While something else published 0x139
// first (the 0x13c T8 texBind publication on dungeon1, the 0x147 mode-6 CPU replays on s05) the
// resident was clean and nothing was wrong. Remove that publication — the GPU paletted view, the
// mode-5/6 widening — and 0x139 is still DIRTY when the owner publishes: its next wave reads the
// bump as an escaped writer and DROPS RESIDENCY without publishing, losing its rendered rows and
// forcing a full re-seed (s05: 16.65 -> 31.24 ms/f, `inv=1629`).
//
// The bytes the owner publication writes are guest bytes exactly like a host->local IMAGE upload
// (G536 already treats them so at publication time: a moved page is guest-owned). So hand the
// written block interval to the SAME deferred upload mirror the transfer path uses (G264 CT32 map,
// G766 bit-exact mask mirror): every dirty resident that holds those words is patched VRAM->FBO at
// its next reader / wave edge and re-anchored there, and keeps its other rendered rows. A target
// the mirror cannot admit keeps the legacy behaviour (fail-closed).
//   DC2_G768_NO_OWNERMIR=1  rollback: the owner publication is invisible to residents (G767)
static bool g768OwnerMirOn()
{
    static const bool on = !envFlagEnabled("DC2_G768_NO_OWNERMIR");
    return on;
}
static uint64_t g_g768OwnerMirCalls = 0u, g_g768OwnerMirNotes = 0u, g_g768OwnerMirTaken = 0u;

static void g768NoteOwnerPublishMirror(uint32_t firstBlock, uint32_t lastBlock)
{
    if (!g768OwnerMirOn() || firstBlock > lastBlock || !g261WaveOn() || !g264On())
        return;
    const G144BlockRange pub{firstBlock, lastBlock, true};
    bool any = false;
    for (uint32_t t = 0u; t < kG248TargetCount && !any; ++t)
    {
        const G261Res &r = g_g261Res[t];
        any = r.dirty && r.range.valid && g144RangeOverlaps(r.range, pub);
    }
    if (!any)
        return;
    ++g_g768OwnerMirCalls;
    // One CT32 "upload" per shape: an 8x8 rect at base block b (dbw 1) is exactly block b; a
    // 64 x 32n rect at a page-aligned base (dbw 1) is exactly n consecutive pages.
    const auto note = [](uint32_t bp, uint32_t w, uint32_t h) {
        const uint32_t m264 = g766MaskMirActive()
                                  ? 0u
                                  : g264NoteUploadForMirror(bp, 1u, GS_PSM_CT32, 0u, 0u, w, h);
        const uint32_t m766 = g766NoteUploadMaskMirror(bp, 1u, GS_PSM_CT32, 0u, 0u, w, h, m264);
        ++g_g768OwnerMirNotes;
        if ((m264 | m766) != 0u)
            ++g_g768OwnerMirTaken;
    };
    uint32_t b = firstBlock;
    while (b <= lastBlock && (b & 31u) != 0u)
        note(b++, 8u, 8u);
    while (b <= lastBlock && lastBlock - b + 1u >= 32u)
    {
        const uint32_t pages = std::min<uint32_t>((lastBlock - b + 1u) / 32u, 64u);
        note(b, 64u, 32u * pages);
        b += pages * 32u;
    }
    while (b <= lastBlock)
        note(b++, 8u, 8u);
    if (g767StatOn())
    {
        static uint64_t s_n = 0u;
        if ((++s_n % 256u) == 1u)
            std::fprintf(stderr, "[G768:ownermir] calls=%llu notes=%llu taken=%llu\n",
                         (unsigned long long)g_g768OwnerMirCalls,
                         (unsigned long long)g_g768OwnerMirNotes,
                         (unsigned long long)g_g768OwnerMirTaken);
    }
}
