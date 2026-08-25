// ===== G657 P1: 25 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
static bool looksLikeGuestPointerOrNull(uint32_t value)
{
    if (value == 0u)
    {
        return true;
    }
    const uint32_t normalized = value & 0x1FFFFFFFu;
    return normalized < PS2_RAM_SIZE;
}

static bool readGuestU32Safe(const uint8_t *rdram, uint32_t addr, uint32_t &out)
{
    const uint8_t *b0 = getConstMemPtr(rdram, addr + 0u);
    const uint8_t *b1 = getConstMemPtr(rdram, addr + 1u);
    const uint8_t *b2 = getConstMemPtr(rdram, addr + 2u);
    const uint8_t *b3 = getConstMemPtr(rdram, addr + 3u);
    if (!b0 || !b1 || !b2 || !b3)
    {
        out = 0u;
        return false;
    }

    out = static_cast<uint32_t>(*b0) |
          (static_cast<uint32_t>(*b1) << 8) |
          (static_cast<uint32_t>(*b2) << 16) |
          (static_cast<uint32_t>(*b3) << 24);
    return true;
}

struct DecodedSemaParams
{
    int init = 0;
    int max = 1;
    uint32_t attr = 0;
    uint32_t option = 0;
};

static DecodedSemaParams decodeCreateSemaParams(const uint32_t *param, uint32_t availableWords)
{
    DecodedSemaParams out{};
    if (!param || availableWords == 0u)
    {
        return out;
    }

    // EE layout (kernel.h):
    // [0]=count [1]=max_count [2]=init_count [3]=wait_threads [4]=attr [5]=option
    const bool hasEeLayout = availableWords >= 3u;
    const int eeMax = hasEeLayout ? static_cast<int>(param[1]) : 1;
    const int eeInit = hasEeLayout ? static_cast<int>(param[2]) : 0;
    const uint32_t eeAttr = (availableWords >= 5u) ? param[4] : 0u;
    const uint32_t eeOption = (availableWords >= 6u) ? param[5] : 0u;

    // Legacy layout (IOP-style):
    // [0]=attr [1]=option [2]=init [3]=max
    const bool hasLegacyLayout = availableWords >= 4u;
    const int legacyMax = hasLegacyLayout ? static_cast<int>(param[3]) : 1;
    const int legacyInit = hasLegacyLayout ? static_cast<int>(param[2]) : 0;
    const uint32_t legacyAttr = hasLegacyLayout ? param[0] : 0u;
    const uint32_t legacyOption = hasLegacyLayout ? param[1] : 0u;

    auto countLooksPlausible = [](int value) -> bool
    {
        return value > 0 && value <= 0x10000;
    };

    bool useLegacyLayout = hasLegacyLayout && !hasEeLayout;
    if (hasLegacyLayout && hasEeLayout && countLooksPlausible(legacyMax) && !countLooksPlausible(eeMax))
    {
        useLegacyLayout = true;
    }
    else if (hasLegacyLayout && hasEeLayout && countLooksPlausible(legacyMax) && countLooksPlausible(eeMax))
    {
        // If both max values look valid, prefer the layout whose option field
        // looks like a pointer/NULL payload.
        const bool eeOptionLooksValid = looksLikeGuestPointerOrNull(eeOption);
        const bool legacyOptionLooksValid = looksLikeGuestPointerOrNull(legacyOption);
        if (!eeOptionLooksValid && legacyOptionLooksValid)
        {
            useLegacyLayout = true;
        }
    }

    if (useLegacyLayout && hasLegacyLayout)
    {
        out.max = legacyMax;
        out.init = legacyInit;
        out.attr = legacyAttr;
        out.option = legacyOption;
    }
    else
    {
        if (!hasEeLayout)
        {
            return out;
        }
        out.max = eeMax;
        out.init = eeInit;
        out.attr = eeAttr;
        out.option = eeOption;
    }

    return out;
}











constexpr uint32_t WEF_OR = 1;
constexpr uint32_t WEF_CLEAR = 0x10;
constexpr uint32_t WEF_CLEAR_ALL = 0x20;
constexpr uint32_t WEF_MODE_MASK = WEF_OR | WEF_CLEAR | WEF_CLEAR_ALL;
constexpr uint32_t EA_MULTI = 0x2;















