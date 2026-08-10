// ===== G568: BIT-FIELD-DISJOINT TEXTURE/Z ALIAS ADMISSION =====
//
// THE DEFECT. `g178TryFlushGpu` rejects a whole flush with `kG266FrTexAliasZ` whenever a sampled
// texture's VRAM rect overlaps the live guest-Z window (rasterizer_vram_materialization.inc, the
// `texAliasesZ` site). The test is a PAGE/RECT overlap: it asks "do these two surfaces touch the
// same bytes?" and nothing more. On route3 that single conjunct is the largest CPU-fallback class
// in the port — `[G290:gpufail]` sampled 1-in-64 puts `texaliasz` at 11.7% of failed flushes but
// **90.1% of all CPU-replayed entries** (fbp=0x000/0x068 — the MAIN display batch, ~259 entries
// each), i.e. the whole main scene falls off the GPU onto the 8-lane CPU band replay.
//
// THE MEASURED ALIAS. `[G242:alias]` on route3 reports the identical shape every time:
//
//   texture-depth tbp=0x1a00 psm=0x2c cbp=0x3fe0 zbp=0xd0 zRows=0..415 tex=512x512
//                 texHit=1 clutHit=0
//
// ZBP 0xd0 pages << 5 = 0x1a00 blocks = TBP0 exactly. The game has packed a 512x512 **PSMT4HH**
// texture into the same VRAM as its **PSMZ24** depth buffer. That is not a collision, it is the
// standard PS2 idiom: PSMZ24 stores depth in bits 0..23 of a 32-bit word and leaves bits 24..31
// permanently free, and the PSMT8H / PSMT4HL / PSMT4HH formats exist precisely to address those
// spare high bits. The two surfaces share pages and share words but **never share a bit**.
//
// WHY THE EXEMPTION IS SOUND END TO END. The rejection exists because the GPU owns the Z window,
// so the guest-VRAM copy of those pages is stale. That is true only of the bits the Z path writes:
//
//   * CPU raster    -> GSMem::WriteZ24 -> PixelStorageTraits<Z24>::Write, which is a READ-MODIFY-
//                      WRITE: `value = (old & 0xFF000000) | (value & 0x00FFFFFF)`
//                      (ps2xRuntime/include/runtime/ps2_gs_memory.h). The high byte is preserved.
//   * GPU depth publication -> g242SyncDepthState -> the same GSMem::WriteZ24 per pixel
//                      (rasterizer_rtt_census_and_waves.inc). The high byte is preserved.
//
// No Z path in this port writes bits 24..31 of a PSMZ24 surface, so guest VRAM's high byte is
// never stale with respect to depth — it still holds exactly what the guest's own image upload put
// there. A T8H/T4HL/T4HH sampler reads only that byte. The batch is therefore admissible.
// (g242SyncDepthState already calls g149BumpVramGen with the comment "A Z page may also be texture
// or CLUT storage" — the codebase had observed the aliasing; it just had no bit-width test.)
//
// SCOPE — three conjuncts, all required, none of them widened:
//   1. the guest Z PSM is PSMZ24 (ZBUF.PSM field == 1). PSMZ32 writes all 32 bits; PSMZ16/Z16S
//      have a different word layout entirely. Both keep the old rejection.
//   2. the texture PSM is T8H (bits 24..31), T4HL (24..27) or T4HH (28..31). Every other PSM —
//      including plain T8/T4, CT32 and CT24 — reads bits the Z path writes, and keeps it.
//   3. the CLUT does NOT alias Z. `clutAliasesZ` is evaluated and rejected exactly as before: a
//      CLUT is CT32, so it is never bit-disjoint from anything. (The measured batch reports
//      clutHit=0, cbp=0x3fe0 — well clear of the Z window.)
//
// This does not touch `g242SyncOverlappingDepth` (the second kG266FrTexAliasZ site below it). That
// one is a SYNC FAILURE, not an overlap verdict, and a failed sync must still fail closed.
//
// Levers:
//   DC2_G419_AB=bitalias   paired gate (hold 16, GS-worker lever)
//   DC2_G568_BITALIAS=1    arm standalone (bring-up default: OFF)
//   DC2_G568_NO_BITALIAS=1 rollback once promoted default-ON
//   DC2_G568_STAT=1        [G568:bitalias] census — admits vs the shapes still rejected
//
// ⚠️ THE COUNTER IS NOT OPTIONAL (g538_null_ab_needs_a_path_counter). A null paired reading on an
// admission lever is worthless unless something proves the arms took different paths, and G534
// retired a lever worth 2.7x on exactly that mistake. `g_g568Admits` is bumped on the exemption
// itself, so a run whose delta is null can be read against a non-zero admit count.

int g568BitAliasArm();

static bool g568BitAliasOn()
{
    static const bool paired = []()
    {
        const char *value = std::getenv("DC2_G419_AB");
        return value != nullptr && std::strcmp(value, "bitalias") == 0;
    }();
    if (paired)
        return g568BitAliasArm() > 0;
    // Bring-up default: OFF. Flip to `!envFlagEnabled("DC2_G568_NO_BITALIAS") &&
    // !envFlagDisabled("DC2_G568_BITALIAS")` on promotion.
    static const bool on = envFlagEnabled("DC2_G568_BITALIAS") &&
                           !envFlagEnabled("DC2_G568_NO_BITALIAS");
    return on;
}

static bool g568StatOn()
{
    static const bool on = envFlagEnabled("DC2_G568_STAT");
    return on;
}

static std::atomic<uint64_t> g_g568Admits{0};   // flushes kept on the GPU by the exemption
static std::atomic<uint64_t> g_g568RejPsm{0};   // texture aliased Z but its PSM reads Z bits
static std::atomic<uint64_t> g_g568RejZPsm{0};  // texture PSM was high-bit but Z was not PSMZ24
static std::atomic<uint64_t> g_g568RejClut{0};  // CLUT aliased Z — never bit-disjoint

// Does this texture format read ONLY bits 24..31 of the 32-bit word it addresses?
static inline bool g568HighBitTexPsm(uint32_t psm)
{
    return psm == GS_PSM_T8H ||   // bits 24..31
           psm == GS_PSM_T4HL ||  // bits 24..27
           psm == GS_PSM_T4HH;    // bits 28..31
}

// The ZBUF.PSM field value for PSMZ24 (the register stores the low nibble of the PSM code).
static constexpr uint32_t kG568ZbufPsmZ24 = 1u;

// The whole verdict, in one call, at the single `texAliasesZ` site. Returns true when the flush may
// stay on the GPU despite the page overlap. `texAliases`/`clutAliases` are the caller's already
// computed g242RectsOverlap results, so no rect math is repeated and the disarmed path is one
// predictable branch.
static inline bool g568TexAliasIsBitDisjoint(bool texAliases, bool clutAliases,
                                             uint32_t texPsm, uint32_t guestZpsm)
{
    if (!texAliases || !g568BitAliasOn())
        return false;
    if (clutAliases)
    {
        if (g568StatOn())
            g_g568RejClut.fetch_add(1u, std::memory_order_relaxed);
        return false; // a CT32 CLUT shares every bit — fail closed
    }
    if (!g568HighBitTexPsm(texPsm))
    {
        if (g568StatOn())
            g_g568RejPsm.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    if (guestZpsm != kG568ZbufPsmZ24)
    {
        if (g568StatOn())
            g_g568RejZPsm.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    const uint64_t n = g_g568Admits.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g568StatOn() && (n <= 8u || (n % 4096u) == 0u))
        std::fprintf(stderr,
                     "[G568:bitalias] admit=%llu texPsm=0x%x zpsm=%u | rej(psm=%llu zpsm=%llu "
                     "clut=%llu)\n",
                     static_cast<unsigned long long>(n), texPsm, guestZpsm,
                     static_cast<unsigned long long>(g_g568RejPsm.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g568RejZPsm.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_g568RejClut.load(std::memory_order_relaxed)));
    return true;
}
