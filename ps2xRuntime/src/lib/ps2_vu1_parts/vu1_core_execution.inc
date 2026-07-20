void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f; // VF0.w = 1.0
    m_state.q = 1.0f;
    s_vuPPending = 0.0f;
    s_vuPDelay = 0;
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return vf[bc & 3];
}

// G71: VU1 MAC flag computation. Until now m_state.mac was NEVER written, only read by
// FMAND/FMEQ/FMOR — so every MAC-flag-gated branch evaluated against a constant 0. The
// title-map transform builds its per-vertex draw/cull decision from an FMEQ chain that
// samples the MAC flags of the fog-clamp / winding ops (VU pc 0x1d20/0x1f00.. -> IBEQ
// 0x1d48/0x1f68/0x2128 that skips the ADC `+2048`). With MAC==0 the FMEQ never matched ->
// every kicking vertex got the +2048 -> ADC=1 -> the whole rocky cavern culled (flat blue).
//
// HW MAC layout (per ps2tek; same as F54 VU0 note): 16 bits, four nibbles O[15:12] U[11:8]
// S[7:4] Z[3:0]; within each nibble the lanes are X=bit3 Y=bit2 Z=bit1 W=bit0. Only the
// DEST lanes are flagged; non-dest lanes read 0. Z=result is zero, S=sign bit set (negative,
// incl -0), U=denormal, O=inf/exp-saturated.
static bool s_vu1MacDisabled = (std::getenv("DC2_NO_VU1_MAC") != nullptr);
static const bool s_g106ParallelFlags = (std::getenv("DC2_G106_PARALLEL_FLAGS") != nullptr);
static thread_local bool s_g106UseLowerFlagRead = false;
static thread_local uint32_t s_g106LowerMacRead = 0u;
static thread_local uint32_t s_g106LowerStatusRead = 0u;
// G71/H2 probe: count how often a VU1 FMAC result lane is a denormal (HW flushes to signed 0) or
// inf/NaN (HW clamps to 0x7f7fffff) — i.e. cases where the missing `vuDouble` clamp (PCSX2
// VUops.cpp) would change the value. If DC2 never hits these, H2 is moot. Env DC2_G71_CLAMPCOUNT.
static const bool s_g71clampcount = (std::getenv("DC2_G71_CLAMPCOUNT") != nullptr);
static std::atomic<uint64_t> s_g71denorm{0}, s_g71infnan{0}, s_g71total{0};
static inline uint32_t vu1MakeMac(const float *result, uint8_t dest)
{
    uint32_t mac = 0u;
    for (int i = 0; i < 4; ++i)
    {
        if (!(dest & (0x8u >> i)))
            continue;            // only destination lanes are flagged
        const int b = 3 - i;     // X=bit3 .. W=bit0 within the nibble
        uint32_t bits;
        std::memcpy(&bits, &result[i], 4);
        const uint32_t exp = (bits >> 23) & 0xFFu;
        const uint32_t man = bits & 0x7FFFFFu;
        const bool sign = (bits >> 31) & 1u;
        if (exp == 0u && man == 0u)
            mac |= (1u << (b + 0));      // Z: zero (either sign)
        else if (exp == 0u)
            mac |= (1u << (b + 8));      // U: denormal underflow
        else if (exp == 0xFFu)
            mac |= (1u << (b + 12));     // O: overflow / inf
        if (sign)
            mac |= (1u << (b + 4));      // S: negative
        if (s_g71clampcount)
        {
            s_g71total.fetch_add(1u, std::memory_order_relaxed);
            if (exp == 0u && man != 0u) s_g71denorm.fetch_add(1u, std::memory_order_relaxed);
            else if (exp == 0xFFu)      s_g71infnan.fetch_add(1u, std::memory_order_relaxed);
            if ((s_g71total.load(std::memory_order_relaxed) % 2000000u) == 0u)
                std::fprintf(stderr, "[G71:clampcount] total=%llu denorm=%llu infnan=%llu\n",
                             (unsigned long long)s_g71total.load(),
                             (unsigned long long)s_g71denorm.load(),
                             (unsigned long long)s_g71infnan.load());
        }
    }
    return mac;
}
// G71/H1: derive the VU STATUS flag register from the MAC flags after each FMAC op (PCSX2
// VU_STAT_UPDATE + the sticky accumulate of VUops.cpp). status[3:0]=current Z/S/U/O present,
// [5:4]=D/I (kept), [11:6]=sticky (OR-accumulated). Was a sibling of the MAC bug: m_state.status
// was only ever written by FSSET, so FSAND/FSEQ/FSOR read a stale/zero value. DC2's title VU
// program does not read STATUS, but other VU1 programs may; this keeps it HW-correct.
static inline void vu1UpdateStatus(uint32_t &statusReg, uint32_t mac)
{
    uint32_t cur = 0u;
    if (mac & 0x000Fu) cur |= 0x1u; // Z present in any lane
    if (mac & 0x00F0u) cur |= 0x2u; // S
    if (mac & 0x0F00u) cur |= 0x4u; // U
    if (mac & 0xF000u) cur |= 0x8u; // O
    statusReg = (statusReg & 0xFF0u) | cur | (cur << 6); // keep D/I + sticky, set current, accumulate sticky
}
// G71/H2: optional VU1 result clamp (PCSX2 vuDouble / MAC_UPDATE). Off by default (DC2 renders
// without it); DC2_VU1_CLAMP enables. Flags are taken from the ORIGINAL result (so U/O are set),
// then the stored value is clamped: denormal -> signed zero, inf/NaN/exp-saturated -> +-0x7f7fffff.
// Caller must run this BEFORE applyDest so the clamped value is what gets stored.
static const bool s_vu1Clamp = (std::getenv("DC2_VU1_CLAMP") != nullptr);
static inline void vu1UpdateMac(uint32_t &macReg, uint32_t &statusReg, float *result, uint8_t dest)
{
    if (!s_vu1MacDisabled)
    {
        macReg = vu1MakeMac(result, dest);
        vu1UpdateStatus(statusReg, macReg);
    }
    if (s_vu1Clamp)
    {
        for (int i = 0; i < 4; ++i)
        {
            if (!(dest & (0x8u >> i)))
                continue;
            uint32_t bits;
            std::memcpy(&bits, &result[i], 4);
            const uint32_t exp = (bits >> 23) & 0xFFu;
            if (exp == 0u)
                bits &= 0x80000000u;                 // denormal/zero -> signed zero
            else if (exp == 0xFFu)
                bits = (bits & 0x80000000u) | 0x7f7fffffu; // inf/NaN -> signed max
            else
                continue;
            std::memcpy(&result[i], &bits, 4);
        }
    }
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8)
        dst[0] = result[0]; // x
    if (dest & 0x4)
        dst[1] = result[1]; // y
    if (dest & 0x2)
        dst[2] = result[2]; // z
    if (dest & 0x1)
        dst[3] = result[3]; // w
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    // ACC-writing FMAC ops (ADDA/SUBA/MADDA/MSUBA/MULA/OPMULA/...) update MAC on HW too. Run the
    // MAC/clamp first so the (optional) clamped value is what lands in ACC.
    vu1UpdateMac(m_state.mac, m_state.status, const_cast<float *>(result), dest);
    applyDest(m_state.acc, result, dest);
}

// G65: current program startPC (single-threaded guest exec) so the XGKICK handler can
// attribute emitted prims to the program that built the packet. Bucketed under
// DC2_G65_KICKAGG to find which VU1 program emits the rock tristrips.
static uint32_t s_g65CurStartPC = 0u;

// G215: MAP-4 head/cap batch-cull discriminator. Per model-program execute (startPC=0x10)
// inside the character COutLineDraw window, count how much geometry the batch KICKS out vs how
// much vertex data was uploaded (XTOP window). Reset at execute() entry, bumped at each XGKICK,
// logged after run(). Single-threaded guest exec, same idiom as s_g65CurStartPC. Answers the
// G214 open question: does the head batch reach VU1 but get culled (kicks small/none while input
// present) or is it never submitted by the EE (no execute for it at all)? Default-off.
extern std::atomic<uint64_t> g_dc2PresentTick;        // ps2_runtime.cpp (host present tick mirror)
static const bool s_g215batch = (std::getenv("DC2_G215_BATCH") != nullptr);
static uint32_t s_g215KickCount = 0u;
static uint64_t s_g215KickBytes = 0ull;
static uint32_t s_g215EntryFbp0 = 0u;   // GS ctx0/ctx1 FRAME.fbp at execute entry — character RTT
static uint32_t s_g215EntryFbp1 = 0u;   // renders to fbp=0x139, so scope the batch trace on it

// G216: capture the projected vertex at the model program's final trifan clip gate.
static const bool s_g216clip = (std::getenv("DC2_G216_CLIP") != nullptr);
static const bool s_g216ForceGate = (std::getenv("DC2_G216_FORCE_GATE") != nullptr);
struct G216ClipSample
{
    float p[4];
    uint16_t vi1;
    uint16_t vi10;
};
struct G216ClipStats
{
    uint32_t count;
    uint32_t gateTaken;
    uint32_t gate1d48;
    uint32_t taken1d48;
    uint32_t gate1f68;
    uint32_t taken1f68;
    uint32_t nonFinite;
    uint32_t qNonPositive;
    uint32_t outsideGuard;
    float minv[4];
    float maxv[4];
    G216ClipSample samples[4];
    uint32_t sampleCount;
};
static bool s_g216CharExec = false;
static G216ClipStats s_g216Stats{};
static uint32_t s_g216Header[8]{};

// G217: join the character model's separate TEX0-state and geometry XGKICKs.  G214 proved
// that looking for TEX0 inside the geometry packet is insufficient: DC2 emits the bind and
// vertices in adjacent PATH1 packets.  This observer keeps the last PATH1 TEX0, then reports
// the exact page, packer PC, selector header, primitive, ADC distribution, and screen bounds
// for every kick in the fbp=0x139 model execute.  It never changes VU/GS state or packet bytes.
// Default off; DC2_G217_TICK_MIN/MAX bound the present-tick window and DC2_G217_LIMIT bounds log.
static const bool s_g217Join = (std::getenv("DC2_G217_JOIN") != nullptr);
static const uint64_t s_g217TickMin = g136_env_u32("DC2_G217_TICK_MIN", 0u);
static const uint64_t s_g217TickMax = g136_env_u32("DC2_G217_TICK_MAX", 0xFFFFFFFFu);
static const uint32_t s_g217Limit = g136_env_u32("DC2_G217_LIMIT", 4096u);
static bool s_g217CharExec = false;
static uint32_t s_g217ExecSeq = 0u;
static uint32_t s_g217KickSeq = 0u;
static uint64_t s_g217LastTex0 = 0u;
static uint32_t s_g217Header[8]{};
static const bool s_g217TagVu = (std::getenv("DC2_G217_TAG_VU") != nullptr);
static uint32_t s_g217ExactKind = 0u;
static std::atomic<uint32_t> s_g217Logged{0};

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t itop,
                             uint32_t maxCycles)
{
    s_g65CurStartPC = startPC;
    if (s_g215batch || s_g216clip || s_g216ForceGate || s_g217TagVu)
    {
        s_g215KickCount = 0u; s_g215KickBytes = 0ull;
        s_g215EntryFbp0 = gs.getContextFrame(0).fbp;
        s_g215EntryFbp1 = gs.getContextFrame(1).fbp;
    }
    if (s_g216clip || s_g216ForceGate)
    {
        const uint32_t fbp0 = gs.getContextFrame(0).fbp;
        const uint32_t fbp1 = gs.getContextFrame(1).fbp;
        s_g216CharExec = startPC == 0x10u && (fbp0 == 0x139u || fbp1 == 0x139u);
        s_g216Stats = {};
        std::memset(s_g216Header, 0, sizeof(s_g216Header));
        if (s_g216CharExec && memory)
        {
            const uint32_t top = memory->vif1_regs.top & 0x3FFu;
            for (uint32_t word = 0; word < 8u; ++word)
            {
                const uint32_t off = ((top * 16u) + word * 4u) & (PS2_VU1_DATA_SIZE - 1u);
                if (off + 4u <= dataSize)
                    std::memcpy(&s_g216Header[word], vuData + off, 4u);
            }
        }
    }
    if (s_g217Join)
    {
        const uint64_t tick = g_dc2PresentTick.load(std::memory_order_relaxed);
        const uint32_t fbp0 = gs.getContextFrame(0).fbp;
        const uint32_t fbp1 = gs.getContextFrame(1).fbp;
        s_g217CharExec = startPC == 0x10u &&
                         (fbp0 == 0x139u || fbp1 == 0x139u) &&
                         tick >= s_g217TickMin && tick <= s_g217TickMax;
        s_g217KickSeq = 0u;
        std::memset(s_g217Header, 0, sizeof(s_g217Header));
        if (s_g217CharExec)
        {
            ++s_g217ExecSeq;
            if (memory)
            {
                const uint32_t top = memory->vif1_regs.top & 0x3FFu;
                for (uint32_t word = 0; word < 8u; ++word)
                {
                    const uint32_t off = ((top * 16u) + word * 4u) & (PS2_VU1_DATA_SIZE - 1u);
                    if (off + 4u <= dataSize)
                        std::memcpy(&s_g217Header[word], vuData + off, 4u);
                }
            }
        }
    }
    s_g217ExactKind = 0u;
    if (s_g217TagVu && vuData && 38u * 16u + 4u <= dataSize)
    {
        uint32_t selector = 0u;
        std::memcpy(&selector, vuData + 38u * 16u, sizeof(selector));
        if ((selector & 0x40000000u) != 0u) s_g217ExactKind = 1u;
        else if ((selector & 0x20000000u) != 0u) s_g217ExactKind = 2u;
    }
    m_state.pc = startPC;
    m_state.ebit = false;
    m_state.itop = itop;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    if (vu1_trace_enabled())
    {
        const uint32_t n = s_vu1ExecCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 64u)
        {
            std::fprintf(stderr, "[F51.7:exec] n=%u startPc=0x%x itop=0x%x dataSize=0x%x\n",
                         n, startPC, itop, dataSize);
            // G28: the latched double-buffer DATA top the model transform reads via XTOP.
            std::fprintf(stderr, "[G28:top] n=%u startPc=0x%x top=0x%x itop=0x%x\n",
                         n, startPC, memory ? (memory->vif1_regs.top & 0x3FFu) : 0u, itop);
        }
    }
    // G27: one-shot dump of the runaway model-transform loop body (~0xe00..0xe50) +
    // the integer registers at kick, so the next phase can decode why the loop counter
    // never satisfies its IBNE exit (transform loop runs to maxcycles -> no valid XGKICK).
    if (vu1_trace_enabled() && startPC == 0x10u)
    {
        static std::atomic<uint32_t> s_g27dump{0};
        if (s_g27dump.fetch_add(1u, std::memory_order_relaxed) < 10u)
        {
            const uint32_t top = memory ? (memory->vif1_regs.top & 0x3FFu) : 0u;
            auto dumpQw = [&](const char *tag, uint32_t qw)
            {
                const uint32_t off = (qw * 16u) & (PS2_VU1_DATA_SIZE - 1u);
                if (off + 16u > dataSize) return;
                float f[4];
                for (int k = 0; k < 4; ++k) std::memcpy(&f[k], vuData + off + k * 4u, 4);
                std::fprintf(stderr, "[G28:in] %s qw=0x%x off=0x%x = % .4g % .4g % .4g % .4g\n",
                             tag, qw, off, f[0], f[1], f[2], f[3]);
            };
            std::fprintf(stderr, "[G28:in] --- model kick top=0x%x itop=0x%x ---\n", top, itop);
            // G38: VF/VI register state AT ENTRY (persisted from the prior kick). A matrix held
            // in VF that is valid for early kicks but zero for later ones => clobber/no-reload.
            {
                char vfbuf[1024]; int p = 0;
                for (int r = 0; r < 32; ++r)
                    p += std::snprintf(vfbuf + p, sizeof(vfbuf) - p, "VF%d(% .3g,% .3g,% .3g,% .3g) ",
                                       r, m_state.vf[r][0], m_state.vf[r][1], m_state.vf[r][2], m_state.vf[r][3]);
                std::fprintf(stderr, "[G38:vfentry] %s\n", vfbuf);
                char vibuf[512]; int q = 0;
                for (int r = 0; r < 16; ++r)
                    q += std::snprintf(vibuf + q, sizeof(vibuf) - q, "VI%d=0x%x ", r,
                                       (unsigned)(uint16_t)m_state.vi[r]);
                std::fprintf(stderr, "[G38:vientry] %s\n", vibuf);
            }
            // input vertex buffer the transform reads (XTOP): count nonzero qwords in a wide
            // window so we see whether later batches' vertex buffers are mostly zero.
            uint32_t nzqw = 0u, firstZero = 0xFFFFFFFFu;
            for (uint32_t qw = top; qw < top + 0x60u; ++qw)
            {
                const uint32_t off = (qw * 16u) & (PS2_VU1_DATA_SIZE - 1u);
                if (off + 16u > dataSize) break;
                bool nz = false;
                for (uint32_t b = 0; b < 16u; ++b) if (vuData[off + b]) { nz = true; break; }
                if (nz) ++nzqw; else if (firstZero == 0xFFFFFFFFu) firstZero = qw - top;
            }
            std::fprintf(stderr, "[G28:in] vtxwindow top=0x%x nonzeroQW=%u/0x60 firstZeroAt=+0x%x\n",
                         top, nzqw, firstZero);
            for (uint32_t qw = top; qw < top + 8u; ++qw) dumpQw("vtx", qw);
            // G39: the skinned path reads its bone-matrix palette from VU qw 0x3c (LQ 60(VIn)).
            // Dump qw 0x3c..0x47 (first 3 bone matrices) AT ENTRY to confirm whether the palette
            // is ever populated for the skinned (2nd+) kicks.
            for (uint32_t qw = 0x3cu; qw <= 0x47u; ++qw) dumpQw("pal", qw);
        }
    }
    // G38: one-shot dump of the MODEL program code (startPc=0x10) preamble + a bit of the
    // vertex loop, to see how the vertex address (XTOP -> VI -> LQ) and matrix are loaded.
    if (vu1_trace_enabled() && startPC == 0x10u)
    {
        static std::atomic<uint32_t> s_g38code{0};
        if (s_g38code.fetch_add(1u, std::memory_order_relaxed) == 0u)
        {
            // G39: dump the FULL resident program (setup 0x0 + model trampoline 0x10 jumps to
            // 0x320; transform divides at 0xc28/0xc68; GIF packing ~0x1b00) so it can be
            // disassembled offline to find the multi-kick vertex/matrix addressing bug.
            // G140: dump the WHOLE microcode (was 0x1c90 cap) — the trifan/clip emitters
            // live past 0x2180 and were never disassembled.
            for (uint32_t a = 0x0u; a + 8u <= codeSize; a += 8u)
            {
                uint32_t lo = 0u, up = 0u;
                std::memcpy(&lo, vuCode + a, 4);
                std::memcpy(&up, vuCode + a + 4, 4);
                std::fprintf(stderr, "[G39:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
            }
        }
    }
    // G234: dump the map program's INPUT vertex qwords (XTOP batch) at kick time, to
    // discriminate EE-side (VIF stream already carries the wrong constant color) from
    // VU-side (input varies, output collapses). Layout per the dungeon map program
    // (startPC=0x10, XGKICK@0x648): batch header at TOP+0/+1, vertices at TOP+2,
    // stride 5 qw (pos, st, color->FTOI0, x2 aux->FTOI4). Gated DC2_G234_VUIN +
    // the G138 deferred-start gate (DC2_G138_GSDUMP_AT_TICK) so it samples the
    // scene of interest, capped at 6 kicks.
    {
        static const bool s_g234vuin = (std::getenv("DC2_G234_VUIN") != nullptr);
        extern std::atomic<bool> g_dc2G138DumpGateOpen;
        if (s_g234vuin && startPC == 0x10u && vuData &&
            g_dc2G138DumpGateOpen.load(std::memory_order_relaxed))
        {
            static std::atomic<uint32_t> s_g234Kicks{0};
            const uint32_t kn = s_g234Kicks.fetch_add(1u, std::memory_order_relaxed);
            if (kn < 24u)
            {
                const uint32_t top = memory ? (memory->vif1_regs.top & 0x3FFu) : 0u;
                uint32_t code648 = 0u;
                if (0x648u + 4u <= codeSize)
                    std::memcpy(&code648, vuCode + 0x648u, 4);
                std::fprintf(stderr, "[G234:vuin] kick=%u top=0x%x itop=0x%x lo@648=0x%08x\n",
                             kn, top, itop, code648);
                // Constant region at VI0 base (qw 0x0..0x14): matrix rows, light/material
                // color constants. The map program stores VF6 (loaded from ~qw0x10) as the
                // per-vertex GS color, so the material tint lives here.
                // G235: extended to qw 0x3c to capture the light-colour matrix / ambient /
                // point-light constants (the map program reads these for its per-vertex tint,
                // not just the object matrix at 0x08-0x0b). Skip all-zero rows above 0x14.
                for (uint32_t q = 0x0u; q <= 0x3cu; ++q)
                {
                    const uint32_t base = q * 16u;
                    if (base + 16u > dataSize) break;
                    float f[4];
                    std::memcpy(f, vuData + base, 16);
                    if (q > 0x14u && f[0] == 0.f && f[1] == 0.f && f[2] == 0.f && f[3] == 0.f)
                        continue;
                    std::fprintf(stderr, "[G234:vuin]   K qw0x%02x %12.4f %12.4f %12.4f %12.4f\n",
                                 q, f[0], f[1], f[2], f[3]);
                }
                // G235: dump the per-vertex INPUT batch to discriminate EE-side (the vertex
                // stream already carries the wrong per-vertex colour) from VU-side (input
                // colour correct, VU over-brightens it). Layout per this program: header at
                // TOP+0/+1, vertices at TOP+2 stride 5 qw (pos, st, colour, aux, aux). The
                // colour qw is the 3rd of each vertex (TOP + 2 + v*5 + 2). Dump 8 verts.
                // G235: the map program uses THREE SEQUENTIAL input arrays (VI2 positions,
                // VI4=VI2+count colours, VI5=VI4+count st), NOT interleaved. Dump a raw window
                // of consecutive qw from TOP so the colour array (values in ~0..255 range,
                // FTOI0-passthrough to the GS RGBA) is visible after the position block.
                // G236: dump the VIF1 row/col/mask registers — if the map colour array is
                // VIF-UNPACKed in row-ADD (m-bit) mode, the per-frame STROW value is ADDED to
                // the baked base colour, which matches the additive-on-lit over-brightness.
                if (memory)
                {
                    std::fprintf(stderr,
                        "[G236:vif] mask=0x%08x row=[%d %d %d %d] col=[%d %d %d %d]\n",
                        memory->vif1_regs.mask,
                        (int)memory->vif1_regs.row[0], (int)memory->vif1_regs.row[1],
                        (int)memory->vif1_regs.row[2], (int)memory->vif1_regs.row[3],
                        (int)memory->vif1_regs.col[0], (int)memory->vif1_regs.col[1],
                        (int)memory->vif1_regs.col[2], (int)memory->vif1_regs.col[3]);
                }
                // G236: widen the window to 0x140 qw so the parallel colour array
                // (VI4 = positions + count, values in ~0..255 range -> FTOI0 -> GS RGBA) and
                // the st array are captured past the position block (which alone fills 0x60).
                // G236: SCAN THE ENTIRE VU data memory for colour-range float qwords. If the
                // per-vertex colour is a baked FTOI0-ready array (G235's passthrough claim), it
                // must appear somewhere as floats in ~[40,260]. If NO such qword exists anywhere,
                // the colour is VU-COMPUTED (G235 refuted / G234's VU-lighting math right).
                {
                    uint32_t nColourQw = 0u;
                    for (uint32_t qw = 0u; (qw + 1u) * 16u <= dataSize; ++qw)
                    {
                        float f[4];
                        std::memcpy(f, vuData + qw * 16u, 16);
                        int lanesInRange = 0;
                        for (int k = 0; k < 3; ++k)
                            if (f[k] >= 40.0f && f[k] <= 260.0f) ++lanesInRange;
                        if (lanesInRange >= 2)
                        {
                            uint32_t u[4];
                            std::memcpy(u, vuData + qw * 16u, 16);
                            if (nColourQw < 48u)
                                std::fprintf(stderr,
                                    "[G236:col] qw0x%03x %8.2f %8.2f %8.2f %8.2f | %08x %08x %08x %08x\n",
                                    qw, f[0], f[1], f[2], f[3], u[0], u[1], u[2], u[3]);
                            ++nColourQw;
                        }
                    }
                    std::fprintf(stderr, "[G236:col] kick=%u colourRangeQw=%u top=0x%x\n",
                                 kn, nColourQw, top);
                }
                std::fflush(stderr);
            }
        }
    }
    // G197: generic one-shot-per-distinct-startPC full microcode dump, unscoped (unlike G38's
    // startPC==0x10 title-model-only gate) -- needed to capture the map-piece/mgCVisualMDT
    // "VU program 0" microcode (G195/G196 block-trace) which uses a different startPC than the
    // title programs. Default-off (DC2_G197_VUDUMP); caps at 16 distinct startPC values so it
    // can't runaway across a long session.
    static const bool s_g197vudump = (std::getenv("DC2_G197_VUDUMP") != nullptr);
    if (s_g197vudump)
    {
        // G234: dedupe by (startPC, code checksum), not startPC alone — the game RELOADS
        // different microcode at the same startPC across scenes (title vs dungeon map),
        // and the old startPC-only key silently skipped every later program.
        uint32_t crc = 0x811C9DC5u;
        for (uint32_t a = 0; a + 4u <= codeSize; a += 64u)
        {
            uint32_t w = 0u; std::memcpy(&w, vuCode + a, 4);
            crc = (crc ^ w) * 0x01000193u;
        }
        static std::atomic<uint32_t> s_g197Seen[16];
        bool doDump = false;
        for (uint32_t i = 0; i < 16u; ++i)
        {
            uint32_t expected = 0u;
            const uint32_t tag = (startPC + 1u) ^ (crc | 1u);
            if (s_g197Seen[i].load(std::memory_order_relaxed) == tag) break;
            if (s_g197Seen[i].compare_exchange_strong(expected, tag, std::memory_order_relaxed))
            {
                doDump = true;
                break;
            }
            if (expected == tag) break;
        }
        if (doDump)
        {
            std::fprintf(stderr, "[G197:vudump] startPC=0x%x codeSize=0x%x\n", startPC, codeSize);
            for (uint32_t a = 0x0u; a + 8u <= codeSize; a += 8u)
            {
                uint32_t lo = 0u, up = 0u;
                std::memcpy(&lo, vuCode + a, 4);
                std::memcpy(&up, vuCode + a + 4, 4);
                std::fprintf(stderr, "[G39:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
            }
            std::fflush(stderr);
        }
    }
    // G198: the gate chain's ILW.x VI8,30(VI0) @0x2058 reads a fixed data-memory slot (qw 30,
    // byte 0x1E0) whose x-component IORs into VI10 (with the 0xD0 constant) before the FMAND
    // cascade. Dump it once per kick (capped) to check whether it's a resident constant (same
    // every kick, same title vs map) or something per-object/per-draw upload seeds differently --
    // a plain wrong-constant bug would need no VU1 interpreter change at all. Default-off
    // (DC2_G198_GATE_TRACE, shares the gate-trace env with the VI dump above).
    {
        static const bool s_g198qw30 = (std::getenv("DC2_G198_GATE_TRACE") != nullptr);
        if (s_g198qw30 && startPC == 0x10u && vuData && dataSize > 30u * 16u + 4u)
        {
            static std::atomic<uint32_t> s_g198KickN{0};
            const uint32_t kn = s_g198KickN.fetch_add(1u, std::memory_order_relaxed);
            if (kn < 200u)
            {
                uint32_t qw30x = 0u;
                std::memcpy(&qw30x, vuData + 30u * 16u, 4);
                std::fprintf(stderr, "[G198:qw30] kick=%u qw30.x=0x%08x\n", kn, qw30x);
            }
        }
    }
    // G61: one-shot dump of the TITLE MAP GIF-packing program (kick pc=0x2d88, uploaded to
    // 0x2b20). HW emits this geometry as PRIM=tristrip(3) but the runner emits trifan(5); we
    // need the microcode that builds the output GIFtag PRIM field. Gate on a dedicated env so
    // it never perturbs the costume/setup dumps. Dump the whole GIF-pack region 0x2a00..0x2e90.
    static const bool s_g61vudump = (std::getenv("DC2_G61_VUDUMP") != nullptr);
    if (s_g61vudump)
    {
        static std::atomic<uint32_t> s_g61{0};
        // Dump only when the map GIF-pack region is actually populated (XGKICK lower op at
        // pc=0x2d88), so we skip the setup/costume executes. XGKICK lower encoding low byte
        // pattern: the dispatcher matches 0x6C in the decoded op; detect it by the raw lower
        // word's top opcode bits == 0x6C (lower special op field bits[31:25]>>1 ... ) — to be
        // robust just require non-zero code there and dump up to 4 distinct executes.
        if (s_g61.load(std::memory_order_relaxed) < 4u && 0x2d88u + 8u <= codeSize)
        {
            uint32_t loK = 0u; std::memcpy(&loK, vuCode + 0x2d88u, 4);
            if (loK != 0u)
            {
                s_g61.fetch_add(1u, std::memory_order_relaxed);
                std::fprintf(stderr, "[G61:vudump] startPC=0x%x codeSize=0x%x lo@2d88=0x%08x\n",
                             startPC, codeSize, loK);
                for (uint32_t a = 0x2a00u; a <= 0x3300u && a + 8u <= codeSize; a += 8u)
                {
                    uint32_t lo = 0u, up = 0u;
                    std::memcpy(&lo, vuCode + a, 4);
                    std::memcpy(&up, vuCode + a + 4, 4);
                    std::fprintf(stderr, "[G39:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
                }
                std::fflush(stderr);
            }
        }
    }
    // G29: the setup kick (startPc=0x0) computes the projection-scale constants. Dump its
    // microcode + input qw0..0xc at entry, then qw0x4..0x6 after run, to find where the
    // poisoned ~-FLT_MAX scale comes from (model verts -> -inf).
    bool g29SetupDump = false;
    if (vu1_trace_enabled() && startPC == 0x0u)
    {
        static std::atomic<uint32_t> s_g29{0};
        if (s_g29.fetch_add(1u, std::memory_order_relaxed) < 2u)
        {
            g29SetupDump = true;
            std::fprintf(stderr, "[G29:setup] --- setup kick top=0x%x itop=0x%x ---\n",
                         memory ? (memory->vif1_regs.top & 0x3FFu) : 0u, itop);
            for (uint32_t a = 0x0u; a <= 0xd0u && a + 8u <= codeSize; a += 8u)
            {
                uint32_t lo = 0u, up = 0u;
                std::memcpy(&lo, vuCode + a, 4);
                std::memcpy(&up, vuCode + a + 4, 4);
                std::fprintf(stderr, "[G29:code] pc=0x%x lo=0x%08x up=0x%08x\n", a, lo, up);
            }
            for (uint32_t qw = 0u; qw < 13u; ++qw)
            {
                const uint32_t off = qw * 16u;
                if (off + 16u > dataSize) break;
                float f[4];
                for (int k = 0; k < 4; ++k) std::memcpy(&f[k], vuData + off + k * 4u, 4);
                std::fprintf(stderr, "[G29:in] qw=0x%x = % .4g % .4g % .4g % .4g\n",
                             qw, f[0], f[1], f[2], f[3]);
            }
        }
    }
    // G29: arm DIV/RSQRT tracing for the first 2 model kicks (find the -inf source).
    if (vu1_trace_enabled() && startPC == 0x10u)
    {
        static std::atomic<uint32_t> s_g29arm{0};
        const uint32_t kickIdx = s_g29arm.fetch_add(1u, std::memory_order_relaxed);
        s_g29DivTrace.store(kickIdx < 16u ? 200 : 0, std::memory_order_relaxed);
        // G39: arm the load trace for the first 6 model kicks (n=1 good, n=3+ bad). 120 load
        // logs per kick covers the preamble vertex/matrix pointer setup + first vertices.
        s_g39KickIdx.store(kickIdx, std::memory_order_relaxed);
        s_g39LoadTrace.store(kickIdx < 6u ? 120 : 0, std::memory_order_relaxed);
    }
    else
    {
        s_g29DivTrace.store(0, std::memory_order_relaxed);
        s_g39LoadTrace.store(0, std::memory_order_relaxed);
    }
    // G85: one-shot dump of the title-program (startPc=0x10) VU INPUT qwords 36..60 — the
    // light-colour matrix / ambient / light positions / fog. Maps the EE lightInfo (byte-
    // identical to HW, G84) to the VU qwords the lighting subs read (40/41/45-48) so the
    // R-vs-G/B channel routing of the directional term can be pinned. Env DC2_G85_IN, quiet.
    {
        static const bool s_g85in = (std::getenv("DC2_G85_IN") != nullptr);
        if (s_g85in && startPC == 0x10u && vuData)
        {
            static std::atomic<uint32_t> s_g85inN{0};
            const uint32_t kn = s_g85inN.fetch_add(1u, std::memory_order_relaxed);
            if (kn < 60u)
            {
                // selector at qw38.x (integer) + scan qw 8..60 for the light/colour data,
                // print only non-zero qwords so the rock's lighting signature is easy to spot.
                uint32_t sel = 0u;
                if (38u * 16u + 4u <= dataSize) std::memcpy(&sel, vuData + 38u * 16u, 4);
                std::fprintf(stderr, "[G85:in] kick=%u dataSize=0x%x sel(qw38)=0x%x\n", kn, dataSize, sel);
                for (uint32_t qw = 8u; qw <= 60u; ++qw)
                {
                    const uint32_t off = qw * 16u;
                    if (off + 16u > dataSize) break;
                    float f[4];
                    for (int k = 0; k < 4; ++k) std::memcpy(&f[k], vuData + off + k * 4u, 4);
                    if (f[0] == 0.f && f[1] == 0.f && f[2] == 0.f && f[3] == 0.f) continue;
                    std::fprintf(stderr, "[G85:in]   qw=%u = % .4g % .4g % .4g % .4g\n",
                                 qw, f[0], f[1], f[2], f[3]);
                }
                std::fflush(stderr);
            }
        }
    }
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    if (s_g217ExactKind != 0u)
    {
        uint32_t selector = 0u;
        if (vuData && 38u * 16u + 4u <= dataSize)
            std::memcpy(&selector, vuData + 38u * 16u, sizeof(selector));
        std::fprintf(stderr,
            "[G217:exactvu] tick=%llu kind=%u startPC=0x%x top=0x%x itop=0x%x selector=0x%08x kicks=%u bytes=%llu\n",
            static_cast<unsigned long long>(g_dc2PresentTick.load(std::memory_order_relaxed)),
            s_g217ExactKind, startPC, memory ? (memory->vif1_regs.top & 0x3ffu) : 0u,
            itop, selector, s_g215KickCount,
            static_cast<unsigned long long>(s_g215KickBytes));
        std::fflush(stderr);
    }
    if (s_g216CharExec)
    {
        const G216ClipStats &st = s_g216Stats;
        const G216ClipSample &a = st.samples[0];
        const G216ClipSample &b = st.samples[st.sampleCount > 1u ? 1u : 0u];
        static std::atomic<uint32_t> s_g216seq{0};
        const uint32_t seq = s_g216seq.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G216:clip] seq=%u tick=%llu hdr=%08x,%08x,%08x,%08x/%08x,%08x,%08x,%08x "
                     "g1d48=%u/%u g1f68=%u/%u g2128=%u/%u nonfin=%u qle0=%u outside=%u kicks=%u bytes=%llu "
                     "x=[%.3f,%.3f] y=[%.3f,%.3f] z=[%.3f,%.3f] q=[%.6g,%.6g] "
                     "s0=(%.3f,%.3f,%.3f,%.6g;%04x/%04x) s1=(%.3f,%.3f,%.3f,%.6g;%04x/%04x)\n",
                     seq, (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                     s_g216Header[0], s_g216Header[1], s_g216Header[2], s_g216Header[3],
                     s_g216Header[4], s_g216Header[5], s_g216Header[6], s_g216Header[7],
                     st.taken1d48, st.gate1d48, st.taken1f68, st.gate1f68,
                     st.gateTaken, st.count, st.nonFinite, st.qNonPositive, st.outsideGuard,
                     s_g215KickCount, (unsigned long long)s_g215KickBytes,
                     st.minv[0], st.maxv[0], st.minv[1], st.maxv[1],
                     st.minv[2], st.maxv[2], st.minv[3], st.maxv[3],
                     a.p[0], a.p[1], a.p[2], a.p[3], a.vi1, a.vi10,
                     b.p[0], b.p[1], b.p[2], b.p[3], b.vi1, b.vi10);
        std::fflush(stderr);
    }
    // G215: per-model-batch input-vs-kick tally for the character RTT (fbp=0x139). Distinguishes
    // an EE-skipped sub-mesh (no execute for it) from a VU1-culled one (input present, 0 kicks).
    if (s_g215batch && startPC == 0x10u &&
        (s_g215EntryFbp0 == 0x139u || s_g215EntryFbp1 == 0x139u))
    {
        const uint32_t top = memory ? (memory->vif1_regs.top & 0x3FFu) : 0u;
        uint32_t nzqw = 0u;
        for (uint32_t qw = top; qw < top + 0x200u; ++qw)
        {
            const uint32_t off = (qw * 16u) & (PS2_VU1_DATA_SIZE - 1u);
            if (off + 16u > dataSize) break;
            bool nz = false;
            for (uint32_t b = 0; b < 16u; ++b) if (vuData[off + b]) { nz = true; break; }
            if (nz) ++nzqw;
        }
        static std::atomic<uint32_t> s_g215seq{0};
        const uint32_t seq = s_g215seq.fetch_add(1u, std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[G215:batch] seq=%u tick=%llu fbp0=0x%x fbp1=0x%x top=0x%x itop=0x%x inNZqw=%u kicks=%u kickBytes=%llu\n",
                     seq, (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                     s_g215EntryFbp0, s_g215EntryFbp1, top, itop, nzqw,
                     s_g215KickCount, (unsigned long long)s_g215KickBytes);
        std::fflush(stderr);
    }
    if (g29SetupDump)
    {
        for (uint32_t qw = 4u; qw <= 6u; ++qw)
        {
            const uint32_t off = qw * 16u;
            if (off + 16u > dataSize) break;
            float f[4];
            for (int k = 0; k < 4; ++k) std::memcpy(&f[k], vuData + off + k * 4u, 4);
            std::fprintf(stderr, "[G29:out] qw=0x%x = % .4g % .4g % .4g % .4g\n",
                         qw, f[0], f[1], f[2], f[3]);
        }
    }
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t itop, uint32_t maxCycles)
{
    m_state.ebit = false;
    m_state.itop = itop;
    if (vu1_trace_enabled())
    {
        const uint32_t n = s_vu1ExecCount.fetch_add(1u, std::memory_order_relaxed);
        if (n < 64u)
            std::fprintf(stderr, "[F51.7:resume] n=%u pc=0x%x itop=0x%x\n", n, m_state.pc, itop);
    }
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

