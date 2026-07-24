static bool g95_copy_tex0_enabled()
{
    static const bool enabled =
        dc2_env_flag_enabled("DC2_G95_COPY_TEX0") &&
        !dc2_env_flag_enabled("DC2_G95_NO_COPY_TEX0");
    return enabled;
}

static bool g95_replay_draw_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G95_REPLAY_DRAW");
    return enabled;
}

static bool g95_add_mini_tex0_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G95_ADD_MINI_TEX0");
    return enabled;
}

static bool g95_add_mini_all_tex0_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G95_MINI_ALL_TEX0");
    return enabled;
}

static bool g95_copy_tex0_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G95");
    return enabled;
}

static bool g135_src_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G135_SRC");
    return enabled;
}

static bool g135_src_trace_all_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G135_ALL");
    return enabled;
}

static uint32_t g135_src_trace_limit()
{
    static const uint32_t limit = []() {
        const char *env = std::getenv("DC2_G135_LIMIT");
        if (!env)
            return 180u;
        const int value = std::atoi(env);
        if (value <= 0)
            return 0u;
        return static_cast<uint32_t>(value);
    }();
    return limit;
}

static uint32_t g135_qword_limit()
{
    static const uint32_t limit = []() {
        const char *env = std::getenv("DC2_G135_QW");
        if (!env)
            return 6u;
        const int value = std::atoi(env);
        if (value <= 0)
            return 0u;
        return (value > 16) ? 16u : static_cast<uint32_t>(value);
    }();
    return limit;
}

static uint32_t g95_packet_terminate(uint8_t *rdram, uint32_t packet)
{
    uint32_t current = dc2_read_u32(rdram, packet + 0x00u);
    const uint32_t pending = dc2_read_u32(rdram, packet + 0x08u);
    while ((current & 0x0Cu) != 0u)
    {
        dc2_write_u32(rdram, current, 0u);
        current += 4u;
    }
    if (pending != 0u)
    {
        const uint32_t deltaQwc = ((current - pending) >> 4u) - 1u;
        dc2_write_u32(rdram, pending, dc2_read_u32(rdram, pending) + deltaQwc);
    }
    dc2_write_u32(rdram, packet + 0x08u, 0u);
    dc2_write_u32(rdram, packet + 0x00u, current);
    return current;
}

static uint32_t g95_call_mg_send_vu_prog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                         uint32_t out, uint32_t vuProg)
{
    const auto savedA0 = ctx->r[4];
    const auto savedA1 = ctx->r[5];
    const auto savedA2 = ctx->r[6];
    const auto savedA3 = ctx->r[7];
    const auto savedRa = ctx->r[31];
    const uint32_t savedPc = ctx->pc;

    f50_set_reg(ctx, 4, out);
    f50_set_reg(ctx, 5, vuProg);
    f50_set_reg(ctx, 31, 0u);
    mgSendVuProg__FPUii_0x145e80(rdram, ctx, runtime);
    const uint32_t words = getRegU32(ctx, 2);

    ctx->r[4] = savedA0;
    ctx->r[5] = savedA1;
    ctx->r[6] = savedA2;
    ctx->r[7] = savedA3;
    ctx->r[31] = savedRa;
    ctx->pc = savedPc;
    return words;
}

static uint32_t g95_write_call_tag(uint8_t *rdram, uint32_t dst, uint32_t target)
{
    dc2_write_u32(rdram, dst + 0x00u, 0x50000000u);
    dc2_write_u32(rdram, dst + 0x04u, target & 0x9FFFFFFFu);
    dc2_write_u32(rdram, dst + 0x08u, 0u);
    dc2_write_u32(rdram, dst + 0x0Cu, 0u);
    return dst + 0x10u;
}

static uint32_t g95_write_next_tag(uint8_t *rdram, uint32_t dst, uint32_t target)
{
    dc2_write_u32(rdram, dst + 0x00u, 0x20000000u);
    dc2_write_u32(rdram, dst + 0x04u, target & 0x9FFFFFFFu);
    dc2_write_u32(rdram, dst + 0x08u, 0u);
    dc2_write_u32(rdram, dst + 0x0Cu, 0u);
    return dst + 0x10u;
}

static uint64_t g95_giftag_aplusd(uint32_t nloop)
{
    return (static_cast<uint64_t>(nloop & 0x7FFFu) << 0u) |
           (static_cast<uint64_t>(1u) << 15u) |
           (static_cast<uint64_t>(1u) << 60u);
}

static uint32_t g95_write_copy_tex0_direct(uint8_t *rdram, uint32_t dst, uint64_t tex0)
{
    // VIF1 local DMA tags expose their high 64 bits as the first two VIF words.
    // Put DIRECT in tag word 3 so its GIF payload begins qword-aligned.
    dc2_write_u32(rdram, dst + 0x00u, 0x10000002u);
    dc2_write_u32(rdram, dst + 0x04u, 0u);
    dc2_write_u32(rdram, dst + 0x08u, 0u);
    dc2_write_u32(rdram, dst + 0x0Cu, 0x50000002u); // VIF DIRECT, two GIF qwords.

    dc2_write_u64(rdram, dst + 0x10u, g95_giftag_aplusd(1u));
    dc2_write_u64(rdram, dst + 0x18u, 0x0Eull); // packed A+D
    dc2_write_u64(rdram, dst + 0x20u, tex0);
    dc2_write_u64(rdram, dst + 0x28u, 0x06ull); // GS TEX0_1
    return dst + 0x30u;
}

static bool g95_state_tex0(uint8_t *rdram, uint32_t statePkt, uint64_t *tex0, uint32_t *tbp)
{
    G67TexHit hits[6]{};
    uint32_t hc = 0u;
    g67_scan_tex0_range_limited(rdram, statePkt, 0x400u, 0x400u, hits, &hc);
    if (hc == 0u)
        return false;
    *tex0 = hits[0].tex0;
    *tbp = hits[0].tbp;
    return true;
}

static uint32_t g95_alloc_draw_units(uint8_t *rdram, uint32_t mgr, uint32_t units)
{
    if (units == 0u || !g43_guest_ram_addr(mgr))
        return 0u;
    const uint32_t mem = dc2_read_u32(rdram, mgr + 0x54u);
    if (!g43_guest_ram_addr(mem) || dc2_read_u32(rdram, mem + 0x1Cu) != 0u)
        return 0u;

    const uint32_t base = dc2_read_u32(rdram, mem + 0x20u);
    const uint32_t cursor = dc2_read_u32(rdram, mem + 0x24u);
    const uint32_t limit = dc2_read_u32(rdram, mem + 0x28u);
    const uint64_t next = static_cast<uint64_t>(cursor) + static_cast<uint64_t>(units);
    if (!g43_guest_ram_addr(base) || next >= limit)
        return 0u;

    const uint64_t ptr64 = static_cast<uint64_t>(base) + static_cast<uint64_t>(cursor) * 0x10ull;
    const uint64_t end64 = ptr64 + static_cast<uint64_t>(units) * 0x10ull;
    if (ptr64 > 0x1FFFFFFFull || end64 > 0x2000000ull)
        return 0u;

    dc2_write_u32(rdram, mem + 0x24u, static_cast<uint32_t>(next));
    return static_cast<uint32_t>(ptr64);
}

static bool g95_select_addpacket_tex0(uint8_t *rdram, uint32_t texNode, uint32_t packet,
                                      uint64_t *tex0, uint32_t *tbp, const char **source)
{
    if (g43_guest_ram_addr(texNode))
    {
        G67TexHit hit[1]{};
        uint32_t count = 0u;
        g67_record_tex_hit(hit, &count, texNode + 0x38u, dc2_read_u64(rdram, texNode + 0x38u));
        if (count != 0u)
        {
            *tex0 = hit[0].tex0;
            *tbp = hit[0].tbp;
            *source = "texNode";
            return true;
        }
    }

    G67TexHit hits[6]{};
    uint32_t hitCount = 0u;
    g67_scan_packet_refs(rdram, packet, 0x40u, hits, &hitCount);
    if (hitCount == 0u)
        return false;
    *tex0 = hits[0].tex0;
    *tbp = hits[0].tbp;
    *source = "packet";
    return true;
}

static bool g95_mini_tbp_allowed(uint32_t tbp)
{
    if (g95_add_mini_all_tex0_enabled())
        return g67_title_tbp(tbp);
    return tbp == 0x2B20u || tbp == 0x2F20u || tbp == 0x3320u;
}

static uint32_t g95_write_tex0_next_packet(uint8_t *rdram, uint32_t mini, uint64_t tex0, uint32_t geomPkt)
{
    uint32_t cursor = g95_write_copy_tex0_direct(rdram, mini, tex0);
    cursor = g95_write_next_tag(rdram, cursor, geomPkt);
    return cursor;
}

static void g135_log_qwords(uint8_t *rdram, const char *kind, uint32_t n, uint32_t addr, uint32_t maxQw)
{
    const uint32_t phys = g43_guest_addr(addr);
    if (!g43_guest_ram_addr(phys))
    {
        std::fprintf(stderr, "[G135:%s] n=%u base=0x%x unreadable\n", kind, n, addr);
        return;
    }

    const uint32_t count = (maxQw > 16u) ? 16u : maxQw;
    for (uint32_t q = 0u; q < count; ++q)
    {
        const uint32_t a = phys + q * 16u;
        std::fprintf(stderr,
                     "[G135:%s] n=%u base=0x%x q=%u %08x %08x %08x %08x\n",
                     kind, n, phys, q,
                     dc2_read_u32(rdram, a + 0u),
                     dc2_read_u32(rdram, a + 4u),
                     dc2_read_u32(rdram, a + 8u),
                     dc2_read_u32(rdram, a + 12u));
    }
}

static void g135_log_refs(uint8_t *rdram, uint32_t n, const char *kind, uint32_t packet,
                          uint32_t packetQw, uint32_t refQw)
{
    const uint32_t phys = g43_guest_addr(packet);
    if (!g43_guest_ram_addr(phys))
        return;

    uint32_t refsLogged = 0u;
    const uint32_t qwords = (packetQw > 32u) ? 32u : packetQw;
    for (uint32_t q = 0u; q < qwords && refsLogged < 8u; ++q)
    {
        const uint32_t a = phys + q * 16u;
        const uint32_t w0 = dc2_read_u32(rdram, a + 0u);
        const uint32_t w1 = dc2_read_u32(rdram, a + 4u);
        const uint32_t tagClass = w0 & 0xF0000000u;
        const uint32_t tagId = (w0 >> 28u) & 0xFu;
        const uint32_t qwc = w0 & 0xFFFFu;
        const uint32_t ref = w1 & 0x1FFFFFFFu;
        if (qwc == 0u || qwc > 0x200u)
            continue;
        if (tagClass != 0x10000000u && tagClass != 0x30000000u && tagClass != 0x50000000u)
            continue;
        if (!g43_guest_ram_addr(ref))
            continue;

        G67TexHit hits[6]{};
        uint32_t hitCount = 0u;
        g67_scan_tex0_range_limited(rdram, ref, qwc * 16u, 0x400u, hits, &hitCount);

        char line[768];
        int p = std::snprintf(line, sizeof(line),
            "[G135:ref] n=%u src=%s q=%u tag=0x%x id=%u qwc=%u ref=0x%x",
            n, kind, q, w0, tagId, qwc, ref);
        if (hitCount != 0u)
        {
            p += std::snprintf(line + p, sizeof(line) - static_cast<size_t>(p), " ref");
            g67_append_tex_hits(line, sizeof(line), &p, hits, hitCount);
        }
        std::fprintf(stderr, "%s\n", line);

        const uint32_t dumpQw = (qwc < refQw) ? qwc : refQw;
        if (dumpQw != 0u)
            g135_log_qwords(rdram, "refqw", n, ref, dumpQw);
        ++refsLogged;
    }
}

static void g135_append_hit_text(char *line, size_t cap, int *p, const char *label,
                                const G67TexHit *hits, uint32_t count)
{
    if (*p <= 0 || static_cast<size_t>(*p) >= cap)
        return;
    *p += std::snprintf(line + *p, cap - static_cast<size_t>(*p), " %s", label);
    g67_append_tex_hits(line, cap, p, hits, count);
}

// G91: flush-side per-batch trace. Draw__14mgCDrawManager(mgr, blockArg, packet) walks the
// per-block batch array (built by PreEndDraw from the AddPacket linked list) and emits each
// batch's state packet (TEX0 carrier) + geometry packet to the DMA chain. The runner draws
// block-0's late textures 0x3720/0x3820/0x3920 but never the wall pages 0x2b20/0x2f20/0x3320
// that block 0 ALSO queued with valid TEX0 (G90). This dumps, per flushed batch, the state-packet
// TEX0 (scan) + the per-batch VU program id (batch+0xe) so we can tell whether the dropped wall
// batches use a different (transform/cull) VU program than the drawn ones. DC2_G91_FLUSH, title scope.
static void g91_drawmgr_flush_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t mgr = getRegU32(ctx, 4);
    const int32_t blockArg = static_cast<int32_t>(getRegU32(ctx, 5));
    const uint32_t packet = getRegU32(ctx, 6);
    const uint32_t ra = getRegU32(ctx, 31);
    const bool inTitle = g67_title_scope(rdram, ctx);
    const bool g91 = (std::getenv("DC2_G91_FLUSH") != nullptr) && inTitle;
    const bool g195Block = dc2_env_flag_enabled("DC2_G195_BLOCK_TRACE");

    if (g91 || g195Block)
    {
        // Replicate the read-only batch-array resolution from Draw__14mgCDrawManager@0x135720.
        const uint32_t texInfo = dc2_read_u32(rdram, mgr + 0x58u);
        const uint32_t maxBlocks = texInfo ? dc2_read_u32(rdram, texInfo + 0x0Cu) : 0u;
        int32_t block = blockArg;
        if (blockArg >= 0 && static_cast<uint32_t>(blockArg) < maxBlocks)
        {
            const uint32_t remapBase = dc2_read_u32(rdram, mgr + 0x00u);
            if (remapBase != 0u)
                block = static_cast<int32_t>(dc2_read_u32(rdram, dc2_read_u32(rdram, mgr + 0x04u) + (uint32_t)blockArg * 4u));
            const uint32_t head = (block >= 0) ? dc2_read_u32(rdram, dc2_read_u32(rdram, mgr + 0x10u) + (uint32_t)block * 4u) : 0u;
            const uint32_t cnt = (block >= 0) ? dc2_read_u32(rdram, dc2_read_u32(rdram, mgr + 0x18u) + (uint32_t)block * 4u) : 0u;
            static std::atomic<uint32_t> s_n{0};
            const uint32_t call = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (head != 0u && (g195Block || call <= 12u))
            {
                for (uint32_t i = 0u; i < cnt && i < 256u; ++i)
                {
                    const uint32_t batch = dc2_read_u32(rdram, head + i * 4u);
                    if (!g43_guest_ram_addr(batch))
                        continue;
                    const uint32_t statePkt = dc2_read_u32(rdram, batch + 0x00u);
                    const uint32_t geomPkt = dc2_read_u32(rdram, batch + 0x04u);
                    const int32_t blkIdx = static_cast<int16_t>(g67_read_u16(rdram, batch + 0x0Cu));
                    const uint32_t vuProg = g67_read_u16(rdram, batch + 0x0Eu);
                    G67TexHit hits[6]{};
                    uint32_t hc = 0u;
                    g67_scan_tex0_range_limited(rdram, statePkt, 0x400u, 0x400u, hits, &hc);
                    bool hit2a20 = false;
                    for (uint32_t h = 0u; h < hc; ++h)
                        hit2a20 |= (hits[h].tbp == 0x2a20u);
                    if (g195Block && hit2a20)
                    {
                        static std::atomic<uint32_t> s_g195DrawMgr{0};
                        const uint32_t n = s_g195DrawMgr.fetch_add(1u, std::memory_order_relaxed) + 1u;
                        if (n <= 256u || (n % 720u) == 0u)
                        {
                            char line[640];
                            int p = std::snprintf(line, sizeof(line),
                                "[G195:drawmgr] n=%u call=%u ra=0x%x mgr=0x%x blockArg=%d block=%d "
                                "i=%u/%u batch=0x%x blkIdx=%d vuProg=0x%x state=0x%x geom=0x%x stateTbp=",
                                n, call, ra, mgr, blockArg, block, i, cnt, batch, blkIdx, vuProg, statePkt, geomPkt);
                            g67_append_tex_hits(line, sizeof(line), &p, hits, hc);
                            std::fprintf(stderr, "%s\n", line);
                        }
                    }
                    if (!g91 || call > 12u)
                        continue;
                    char line[640];
                    int p = std::snprintf(line, sizeof(line),
                        "[G91:flush] call=%u blockArg=%d block=%d i=%u/%u batch=0x%x blkIdx=%d vuProg=0x%x state=0x%x geom=0x%x stateTbp=",
                        call, blockArg, block, i, cnt, batch, blkIdx, vuProg, statePkt, geomPkt);
                    if (hc == 0u)
                        p += std::snprintf(line + p, sizeof(line) - (size_t)p, "none");
                    for (uint32_t h = 0u; h < hc && p < (int)sizeof(line) - 24; ++h)
                        p += std::snprintf(line + p, sizeof(line) - (size_t)p, "%s0x%x", (h == 0u) ? "" : ",", hits[h].tbp);
                    // Dump the state packet's first qwords (GIFtag + A+D regs) for the first few
                    // batches so we can see whether it carries an active TEX0 (reg 0x06) write.
                    if (i < 6u && g43_guest_ram_addr(statePkt))
                    {
                        p += std::snprintf(line + p, sizeof(line) - (size_t)p, " stQw:");
                        for (uint32_t q = 0u; q < 5u && p < (int)sizeof(line) - 40; ++q)
                            p += std::snprintf(line + p, sizeof(line) - (size_t)p, " %016llx",
                                (unsigned long long)dc2_read_u64(rdram, statePkt + q * 8u));
                    }
                    std::fprintf(stderr, "%s\n", line);
                }
                std::fflush(stderr);
            }
        }
    }

    const bool g95CopyTex0 = g95_copy_tex0_enabled();
    const bool g95ReplayDraw = g95_replay_draw_enabled();
    if (!inTitle || (!g95CopyTex0 && !g95ReplayDraw))
    {
        Draw__14mgCDrawManagerFiP13sceVif1Packet_0x135720(rdram, ctx, runtime);
        return;
    }

    const uint32_t texInfo = mgr ? dc2_read_u32(rdram, mgr + 0x58u) : 0u;
    const uint32_t maxBlocks = texInfo ? dc2_read_u32(rdram, texInfo + 0x0Cu) : 0u;
    if (blockArg < 0 || static_cast<uint32_t>(blockArg) >= maxBlocks)
    {
        setReturnU32(ctx, 0u);
        ctx->pc = ra;
        return;
    }

    int32_t block = blockArg;
    if (dc2_read_u32(rdram, mgr + 0x00u) != 0u)
    {
        const uint32_t remap = dc2_read_u32(rdram, mgr + 0x04u);
        block = static_cast<int32_t>(dc2_read_u32(rdram, remap + static_cast<uint32_t>(blockArg) * 4u));
    }
    if (block < 0)
    {
        setReturnU32(ctx, 0u);
        ctx->pc = ra;
        return;
    }

    const uint32_t headBase = dc2_read_u32(rdram, mgr + 0x10u);
    const uint32_t countBase = dc2_read_u32(rdram, mgr + 0x18u);
    const uint32_t head = dc2_read_u32(rdram, headBase + static_cast<uint32_t>(block) * 4u);
    const uint32_t count = dc2_read_u32(rdram, countBase + static_cast<uint32_t>(block) * 4u);
    if (head == 0u)
    {
        setReturnU32(ctx, 1u);
        ctx->pc = ra;
        return;
    }

    uint32_t cursor = g95_packet_terminate(rdram, packet);
    const uint32_t start = cursor;
    uint32_t lastState = 0u;
    uint32_t inserted = 0u;
    uint32_t logged = 0u;
    uint32_t batchPtr = head + (count - 1u) * 4u;
    for (uint32_t i = 0u; i < count; ++i, batchPtr -= 4u)
    {
        const uint32_t batch = dc2_read_u32(rdram, batchPtr);
        if (batch == 0u)
            continue;

        const uint32_t vuProg = g67_read_u16(rdram, batch + 0x0Eu);
        cursor += g95_call_mg_send_vu_prog(rdram, ctx, runtime, cursor, vuProg) * 4u;

        const uint32_t statePkt = dc2_read_u32(rdram, batch + 0x00u);
        const uint32_t geomPkt = dc2_read_u32(rdram, batch + 0x04u);
        if (lastState != statePkt)
        {
            cursor = g95_write_call_tag(rdram, cursor, statePkt);
            lastState = statePkt;
        }

        uint64_t tex0 = 0u;
        uint32_t tbp = 0u;
        if (g95CopyTex0 && g95_state_tex0(rdram, statePkt, &tex0, &tbp))
        {
            cursor = g95_write_copy_tex0_direct(rdram, cursor, tex0);
            ++inserted;
            if (g95_copy_tex0_trace_enabled() && logged < 96u)
            {
                std::fprintf(stderr,
                             "[G95:copytex0] marker=DC2_G95_COPY_TEX0_REBIND blockArg=%d block=%d i=%u/%u "
                             "batch=0x%x state=0x%x geom=0x%x vuProg=0x%x tbp=0x%x tex0=0x%016llx\n",
                             blockArg, block, i, count, batch, statePkt, geomPkt, vuProg, tbp,
                             static_cast<unsigned long long>(tex0));
                ++logged;
            }
        }

        cursor = g95_write_call_tag(rdram, cursor, geomPkt);
    }

    dc2_write_u32(rdram, packet + 0x00u, cursor);
    if (g95_copy_tex0_trace_enabled())
    {
        std::fprintf(stderr,
                     "[G95:copytex0:sum] blockArg=%d block=%d batches=%u inserted=%u words=%u\n",
                     blockArg, block, count, inserted, (cursor - start) >> 2u);
        std::fflush(stderr);
    }
    setReturnU32(ctx, 1u);
    ctx->pc = ra;
}

static void g67_mdt_title_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled() && g67_title_scope(rdram, ctx);
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t outPacket = getRegU32(ctx, 5);
    uint32_t mgr = getRegU32(ctx, 7);
    if (mgr == 0u)
        mgr = 0x003820E0u;
    const uint32_t ra = getRegU32(ctx, 31);

    Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(rdram, ctx, runtime);

    if (!trace)
        return;

    const uint32_t n = g_g67_mdt.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t matBase = self ? dc2_read_u32(rdram, self + 0x44u) : 0u;
    const uint32_t head = self ? dc2_read_u32(rdram, self + 0x48u) : 0u;
    const uint32_t texMgr = self ? dc2_read_u32(rdram, self + 0x08u) : 0u;
    const bool logHeader = n <= 48u || (n % 240u) == 0u;

    if (logHeader)
    {
        std::fprintf(stderr,
                     "[G67:mdt] n=%u self=0x%x out=0x%x mgr=0x%x texMgr=0x%x ra=0x%x ret=0x%x "
                     "matBase=0x%x head=0x%x counts{0:%u 1:%u 3:%u 6:%u 7:%u}\n",
                     n, self, outPacket, mgr, texMgr, ra, ret, matBase, head,
                     g67_mgr_count(rdram, mgr, 0u), g67_mgr_count(rdram, mgr, 1u),
                     g67_mgr_count(rdram, mgr, 3u), g67_mgr_count(rdram, mgr, 6u),
                     g67_mgr_count(rdram, mgr, 7u));
    }

    uint32_t node = head;
    uint32_t ni = 0u;
    while (g43_guest_ram_addr(node) && ni < 16u)
    {
        const uint32_t matIndex = dc2_read_u32(rdram, node + 0x00u);
        const uint32_t faceList = dc2_read_u32(rdram, node + 0x04u);
        const uint32_t next = dc2_read_u32(rdram, node + 0x08u);
        const uint32_t vprog = dc2_read_u32(rdram, node + 0x0Cu);
        const uint32_t packet = dc2_read_u32(rdram, node + 0x10u);
        const uint32_t qwc = dc2_read_u32(rdram, node + 0x14u);
        const uint32_t mat = matBase ? (matBase + matIndex * 0x30u) : 0u;
        const uint32_t texNode = mat ? dc2_read_u32(rdram, mat + 0x20u) : 0u;
        const int32_t reqBlock = texNode ? static_cast<int16_t>(g67_read_u16(rdram, texNode)) : -1;
        const uint64_t tex0 = texNode ? dc2_read_u64(rdram, texNode + 0x38u) : 0u;
        uint32_t tbp = 0u, psm = 0u, cbp = 0u;
        f51_decode_tex0(tex0, &tbp, nullptr, &psm, &cbp, nullptr);

        G67TexHit hits[6]{};
        uint32_t hitCount = 0u;
        const bool hit3960 = g67_scan_packet_refs(rdram, packet, qwc, hits, &hitCount);
        const bool important = (tbp == 0x3960u) || hit3960 || (reqBlock == 0 && n <= 24u);
        if (logHeader || important)
        {
            char line[1024];
            int p = std::snprintf(line, sizeof(line),
                "[G67:node] n=%u node=%u @0x%x mat=%u matPtr=0x%x texNode=0x%x req=%d "
                "tex0(tbp=0x%x psm=0x%x cbp=0x%x) face=0x%x vprog=0x%x packet=0x%x qwc=%u next=0x%x",
                n, ni, node, matIndex, mat, texNode, reqBlock, tbp, psm, cbp,
                faceList, vprog, packet, qwc, next);
            g67_append_tex_hits(line, sizeof(line), &p, hits, hitCount);
            std::fprintf(stderr, "%s%s\n", line, hit3960 ? "  HIT3960" : "");
        }

        node = next;
        ++ni;
    }
    std::fflush(stderr);
}

static bool g67_addpacket_scope(uint8_t *rdram, R5900Context *ctx, uint32_t self, uint32_t state)
{
    if (g67_title_scope(rdram, ctx))
        return true;
    if (self >= 0x008F0000u && self < 0x009A0000u)
        return true;
    return (state >= 0x00520000u && state < 0x00630000u);
}

static void g67_addpacket_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled();
    const uint32_t mgr = getRegU32(ctx, 4);
    const int32_t reqArg = static_cast<int32_t>(getRegU32(ctx, 5));
    const uint32_t state = getRegU32(ctx, 6);
    const uint32_t packet = getRegU32(ctx, 7);
    const uint32_t vprog = getRegU32(ctx, 8);
    const uint32_t texNode = getRegU32(ctx, 2);
    const uint32_t self = getRegU32(ctx, 16);
    const uint32_t node = getRegU32(ctx, 17);
    uint32_t g217Kind = 0u;
    if (dc2_env_flag_enabled("DC2_G217_EEOBJ"))
    {
        const uint32_t statePhys = state & 0x0fffffffu;
        if (statePhys == g_g217HeadStatePacket.load(std::memory_order_acquire)) g217Kind |= 1u;
        if (statePhys == g_g217CapStatePacket.load(std::memory_order_acquire)) g217Kind |= 2u;
    }
    const uint32_t g217HeadPtr = (g217Kind != 0u && g43_guest_ram_addr(mgr))
        ? dc2_read_u32(rdram, mgr + 0x50u) : 0u;
    const uint32_t g217PreHead = g43_guest_ram_addr(g217HeadPtr)
        ? dc2_read_u32(rdram, g217HeadPtr) : 0u;
    const bool addScope = g67_addpacket_scope(rdram, ctx, self, state);
    const bool g135Src = g135_src_trace_enabled() && addScope;
    const bool g95AddMini = g95_add_mini_tex0_enabled() && g67_title_scope(rdram, ctx);
    const bool g195Block = dc2_env_flag_enabled("DC2_G195_BLOCK_TRACE");

    G67TexHit stateHits[6]{};
    G67TexHit packetHits[6]{};
    uint32_t stateHitCount = 0u;
    uint32_t packetHitCount = 0u;
    uint32_t tbp = 0u, psm = 0u, cbp = 0u;
    int32_t reqNode = -1;
    bool hit3960 = false;
    uint64_t g95Tex0 = 0u;
    uint32_t g95Tbp = 0u;
    const char *g95TexSource = "none";
    bool g95HaveTex0 = false;
    uint32_t g95ListHeadPtr = 0u;
    uint32_t g95PreHead = 0u;

    if (g95AddMini)
    {
        g95HaveTex0 = g95_select_addpacket_tex0(rdram, texNode, packet, &g95Tex0, &g95Tbp, &g95TexSource) &&
                      g95_mini_tbp_allowed(g95Tbp);
        g95ListHeadPtr = g43_guest_ram_addr(mgr) ? dc2_read_u32(rdram, mgr + 0x50u) : 0u;
        g95PreHead = g43_guest_ram_addr(g95ListHeadPtr) ? dc2_read_u32(rdram, g95ListHeadPtr) : 0u;
    }

    if (((trace || g135Src) && addScope) || g195Block)
    {
        const uint64_t tex0 = g43_guest_ram_addr(texNode) ? dc2_read_u64(rdram, texNode + 0x38u) : 0u;
        if (tex0 != 0u)
        {
            f51_decode_tex0(tex0, &tbp, nullptr, &psm, &cbp, nullptr);
            reqNode = static_cast<int16_t>(g67_read_u16(rdram, texNode));
        }
        hit3960 |= g67_scan_tex0_range_limited(rdram, state, 0x1000u, 0x1000u, stateHits, &stateHitCount);
        hit3960 |= g67_scan_packet_refs(rdram, packet, 0x40u, packetHits, &packetHitCount);
    }

    AddPacket__14mgCDrawManagerFiP1P1i_0x1359d0(rdram, ctx, runtime);

    if (g217Kind != 0u)
    {
        const uint32_t batch = g43_guest_ram_addr(g217HeadPtr)
            ? dc2_read_u32(rdram, g217HeadPtr) : 0u;
        const bool linked = g43_guest_ram_addr(batch) && batch != g217PreHead &&
            dc2_read_u32(rdram, batch + 0x00u) == state &&
            dc2_read_u32(rdram, batch + 0x04u) == packet;
        static std::atomic<uint32_t> s_g217AddPacket{0u};
        const uint32_t n = s_g217AddPacket.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 512u || (n % 720u) == 0u)
        {
            std::fprintf(stderr,
                "[G217:addpacket] n=%u tick=%llu kind=%u mgr=0x%08x req=%d state=0x%08x geom=0x%08x vprog=0x%08x headPtr=0x%08x pre=0x%08x post=0x%08x linked=%u block=%d batchVu=0x%x next=0x%08x\n",
                n, static_cast<unsigned long long>(g_dc2PresentTick.load(std::memory_order_relaxed)),
                g217Kind, mgr, reqArg, state, packet, vprog, g217HeadPtr,
                g217PreHead, batch, linked ? 1u : 0u,
                g43_guest_ram_addr(batch) ? static_cast<int32_t>(static_cast<int16_t>(g67_read_u16(rdram, batch + 0x0cu))) : -1,
                g43_guest_ram_addr(batch) ? static_cast<uint32_t>(g67_read_u16(rdram, batch + 0x0eu)) : 0u,
                g43_guest_ram_addr(batch) ? dc2_read_u32(rdram, batch + 0x08u) : 0u);
            std::fflush(stderr);
        }
    }

    if (g195Block)
    {
        bool hit2a20 = (tbp == 0x2a20u);
        for (uint32_t h = 0u; h < stateHitCount; ++h)
            hit2a20 |= (stateHits[h].tbp == 0x2a20u);
        for (uint32_t h = 0u; h < packetHitCount; ++h)
            hit2a20 |= (packetHits[h].tbp == 0x2a20u);

        if (hit2a20)
        {
            static std::atomic<uint32_t> s_g195Add{0};
            const uint32_t n = s_g195Add.fetch_add(1u, std::memory_order_relaxed) + 1u;
            if (n <= 256u || (n % 720u) == 0u)
            {
                const uint32_t listHeadPtr = g43_guest_ram_addr(mgr) ? dc2_read_u32(rdram, mgr + 0x50u) : 0u;
                const uint32_t batch = g43_guest_ram_addr(listHeadPtr) ? dc2_read_u32(rdram, listHeadPtr) : 0u;
                const bool batchOk = g43_guest_ram_addr(batch) &&
                    dc2_read_u32(rdram, batch + 0x00u) == state &&
                    dc2_read_u32(rdram, batch + 0x04u) == packet;
                char line[1024];
                int p = std::snprintf(line, sizeof(line),
                    "[G195:addpacket] n=%u ra=0x%x mgr=0x%x reqArg=%d state=0x%x packet=0x%x vprog=0x%x "
                    "self=0x%x node=0x%x texNode=0x%x reqNode=%d tex0(tbp=0x%x psm=0x%x cbp=0x%x) "
                    "batch=0x%x batchOk=%u batchBlk=%d batchVu=0x%x",
                    n, getRegU32(ctx, 31), mgr, reqArg, state, packet, vprog, self, node, texNode,
                    reqNode, tbp, psm, cbp, batch, batchOk ? 1u : 0u,
                    batchOk ? static_cast<int32_t>(static_cast<int16_t>(g67_read_u16(rdram, batch + 0x0Cu))) : -1,
                    batchOk ? static_cast<uint32_t>(g67_read_u16(rdram, batch + 0x0Eu)) : 0u);
                if (stateHitCount != 0u)
                    g135_append_hit_text(line, sizeof(line), &p, "state", stateHits, stateHitCount);
                if (packetHitCount != 0u)
                    g135_append_hit_text(line, sizeof(line), &p, "packet", packetHits, packetHitCount);
                std::fprintf(stderr, "%s\n", line);
                std::fflush(stderr);
            }
        }
    }

    if (g95AddMini && g95HaveTex0 && g43_guest_ram_addr(g95ListHeadPtr))
    {
        static std::atomic<uint32_t> s_miniLog{0};
        static std::atomic<uint32_t> s_miniFail{0};
        const uint32_t batch = dc2_read_u32(rdram, g95ListHeadPtr);
        if (g43_guest_ram_addr(batch) && batch != g95PreHead &&
            dc2_read_u32(rdram, batch + 0x00u) == state &&
            dc2_read_u32(rdram, batch + 0x04u) == packet)
        {
            const uint32_t mini = g95_alloc_draw_units(rdram, mgr, 4u);
            if (mini != 0u)
            {
                g95_write_tex0_next_packet(rdram, mini, g95Tex0, packet);
                dc2_write_u32(rdram, batch + 0x04u, mini);
                if (g95_copy_tex0_trace_enabled())
                {
                    const uint32_t n = s_miniLog.fetch_add(1u, std::memory_order_relaxed) + 1u;
                    if (n <= 160u || (n % 720u) == 0u)
                    {
                        std::fprintf(stderr,
                                     "[G95:addmini] marker=DC2_G95_ADD_MINI_TEX0 n=%u mgr=0x%x batch=0x%x "
                                     "state=0x%x geom=0x%x mini=0x%x tbp=0x%x source=%s tex0=0x%016llx\n",
                                     n, mgr, batch, state, packet, mini, g95Tbp, g95TexSource,
                                     static_cast<unsigned long long>(g95Tex0));
                        std::fflush(stderr);
                    }
                }
            }
            else if (g95_copy_tex0_trace_enabled())
            {
                const uint32_t n = s_miniFail.fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (n <= 32u || (n % 720u) == 0u)
                {
                    std::fprintf(stderr,
                                 "[G95:addmini:fail] marker=DC2_G95_ADD_MINI_TEX0 n=%u mgr=0x%x batch=0x%x "
                                 "geom=0x%x tbp=0x%x reason=alloc\n",
                                 n, mgr, batch, packet, g95Tbp);
                    std::fflush(stderr);
                }
            }
        }
    }

    if (g135Src)
    {
        static std::atomic<uint32_t> s_g135Add{0};
        const uint32_t n = s_g135Add.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint32_t listHeadPtr = g43_guest_ram_addr(mgr) ? dc2_read_u32(rdram, mgr + 0x50u) : 0u;
        const uint32_t batch = g43_guest_ram_addr(listHeadPtr) ? dc2_read_u32(rdram, listHeadPtr) : 0u;
        const bool batchOk = g43_guest_ram_addr(batch) &&
            dc2_read_u32(rdram, batch + 0x00u) == state &&
            dc2_read_u32(rdram, batch + 0x04u) == packet;
        const uint32_t limit = g135_src_trace_limit();
        const bool logLine = g135_src_trace_all_enabled() || (limit != 0u && n <= limit);

        if (logLine)
        {
            char line[1024];
            int p = std::snprintf(line, sizeof(line),
                "[G135:addsrc] n=%u mgr=0x%x reqArg=%d state=0x%x packet=0x%x vprog=0x%x "
                "self=0x%x node=0x%x texNode=0x%x reqNode=%d tex0(tbp=0x%x psm=0x%x cbp=0x%x) "
                "batch=0x%x batchOk=%u batchBlk=%d batchVu=0x%x",
                n, mgr, reqArg, state, packet, vprog, self, node, texNode, reqNode, tbp, psm, cbp,
                batch, batchOk ? 1u : 0u,
                batchOk ? static_cast<int32_t>(static_cast<int16_t>(g67_read_u16(rdram, batch + 0x0Cu))) : -1,
                batchOk ? static_cast<uint32_t>(g67_read_u16(rdram, batch + 0x0Eu)) : 0u);
            if (stateHitCount != 0u)
                g135_append_hit_text(line, sizeof(line), &p, "state", stateHits, stateHitCount);
            if (packetHitCount != 0u)
                g135_append_hit_text(line, sizeof(line), &p, "packet", packetHits, packetHitCount);
            std::fprintf(stderr, "%s\n", line);

            const uint32_t qw = g135_qword_limit();
            if (qw != 0u)
            {
                g135_log_qwords(rdram, "stateqw", n, state, qw);
                g135_log_qwords(rdram, "geomqw", n, packet, qw);
                g135_log_refs(rdram, n, "geom", packet, 32u, (qw < 4u) ? qw : 4u);
            }
            std::fflush(stderr);
        }
    }

    if (!trace || !addScope)
        return;

    const uint32_t n = g_g67_add.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool logLine = hit3960 || n <= 180u || (n % 360u) == 0u;
    if (!logLine)
        return;

    char line[1024];
    int p = std::snprintf(line, sizeof(line),
        "[G67:add] n=%u mgr=0x%x reqArg=%d state=0x%x packet=0x%x vprog=0x%x "
        "self=0x%x node=0x%x texNode=0x%x reqNode=%d tex0(tbp=0x%x psm=0x%x cbp=0x%x)",
        n, mgr, reqArg, state, packet, vprog, self, node, texNode, reqNode, tbp, psm, cbp);
    if (p > 0 && static_cast<size_t>(p) < sizeof(line))
    {
        p += std::snprintf(line + p, sizeof(line) - static_cast<size_t>(p), " state");
        g67_append_tex_hits(line, sizeof(line), &p, stateHits, stateHitCount);
    }
    if (p > 0 && static_cast<size_t>(p) < sizeof(line))
    {
        p += std::snprintf(line + p, sizeof(line) - static_cast<size_t>(p), " packet");
        g67_append_tex_hits(line, sizeof(line), &p, packetHits, packetHitCount);
    }
    std::fprintf(stderr, "%s%s\n", line, hit3960 ? "  HIT3960" : "");
    std::fflush(stderr);
}

static void g43_dump_qwords(uint8_t *rdram, const char *tag, uint32_t addr, uint32_t maxQw)
{
    addr = g43_guest_addr(addr);
    if (!g43_guest_ram_addr(addr))
        return;

    const uint32_t qwc = (maxQw > 24u) ? 24u : maxQw;
    for (uint32_t q = 0; q < qwc; ++q)
    {
        const uint32_t a = addr + q * 16u;
        std::fprintf(stderr,
                     "[G43:%s] addr=0x%x q=%u %08x %08x %08x %08x\n",
                     tag, addr, q,
                     dc2_read_u32(rdram, a + 0u),
                     dc2_read_u32(rdram, a + 4u),
                     dc2_read_u32(rdram, a + 8u),
                     dc2_read_u32(rdram, a + 12u));
    }
}

static void g43_dump_words(uint8_t *rdram, const char *tag, uint32_t addr, uint32_t maxWords,
                           uint32_t mdtN, uint32_t nodeI)
{
    addr = g43_guest_addr(addr);
    if (!g43_guest_ram_addr(addr))
        return;

    const uint32_t words = (maxWords > 36u) ? 36u : maxWords;
    for (uint32_t w = 0; w < words; w += 8u)
    {
        char line[512];
        int off = std::snprintf(line, sizeof(line),
                                "[G43:%s] n=%u i=%u addr=0x%x word=%u",
                                tag, mdtN, nodeI, addr, w);
        const uint32_t group = ((words - w) > 8u) ? 8u : (words - w);
        for (uint32_t j = 0; j < group && off > 0 && static_cast<size_t>(off) < sizeof(line); ++j)
        {
            off += std::snprintf(line + off, sizeof(line) - static_cast<size_t>(off),
                                 " %08x", dc2_read_u32(rdram, addr + (w + j) * 4u));
        }
        std::fprintf(stderr, "%s\n", line);
    }
}

static void g43_log_vec_qword(uint8_t *rdram, const char *tag, uint32_t base, uint32_t idx,
                              uint32_t mdtN, uint32_t nodeI, uint32_t rec)
{
    base = g43_guest_addr(base);
    const uint32_t addr = base + idx * 16u;
    if (!g43_guest_ram_addr(base))
        return;

    std::fprintf(stderr,
                 "[G43:%s] n=%u i=%u rec=%u base=0x%x idx=%u addr=0x%x eff=0x%x %08x %08x %08x %08x\n",
                 tag, mdtN, nodeI, rec, base, idx, addr, addr & PS2_RAM_MASK,
                 dc2_read_u32(rdram, addr + 0u),
                 dc2_read_u32(rdram, addr + 4u),
                 dc2_read_u32(rdram, addr + 8u),
                 dc2_read_u32(rdram, addr + 12u));
}

static void g43_log_face_inputs(uint8_t *rdram, uint32_t self, uint32_t face, uint32_t mdtN, uint32_t nodeI)
{
    if (!g43_guest_ram_addr(self) || !g43_guest_ram_addr(face))
        return;

    const uint32_t kind = g43_read_u16(rdram, face + 0u) & 0xffu;
    const uint32_t count = static_cast<uint32_t>(static_cast<uint16_t>(g43_read_u16(rdram, face + 8u)));
    const uint32_t src = dc2_read_u32(rdram, face + 0xcu);
    const uint32_t pos0 = dc2_read_u32(rdram, self + 0x30u);
    const uint32_t pos1 = dc2_read_u32(rdram, self + 0x34u);
    const uint32_t pos2 = dc2_read_u32(rdram, self + 0x38u);
    const uint32_t pos3 = dc2_read_u32(rdram, self + 0x3cu);

    uint32_t setDataMode = 0u;
    if ((g43_read_u16(rdram, face + 0u) & 0x100u) != 0u)
        setDataMode += 1u;
    if ((g43_read_u16(rdram, face + 0u) & 0x10u) != 0u)
        setDataMode += 2u;
    if ((g43_read_u16(rdram, face + 0u) & 0x200u) != 0u)
        setDataMode += 4u;

    uint32_t wordsPerRecord = 0u;
    if (setDataMode == 0u)
        wordsPerRecord = 3u;
    else if (setDataMode == 1u)
        wordsPerRecord = 2u;

    std::fprintf(stderr,
                 "[G43:srcmeta] n=%u i=%u kind=%u setDataMode=%u count=%u wordsPer=%u src=0x%x +30=0x%x +34=0x%x +38=0x%x +3c=0x%x\n",
                 mdtN, nodeI, kind, setDataMode, count, wordsPerRecord, src, pos0, pos1, pos2, pos3);

    if (wordsPerRecord == 0u || count == 0u || !g43_guest_ram_addr(src))
        return;

    g43_dump_words(rdram, "src", src, count * wordsPerRecord, mdtN, nodeI);

    const uint32_t records = (count > 12u) ? 12u : count;
    for (uint32_t rec = 0; rec < records; ++rec)
    {
        const uint32_t entry = src + rec * wordsPerRecord * 4u;
        if (!g43_guest_ram_addr(entry))
            break;

        if (setDataMode == 0u)
        {
            const uint32_t i0 = dc2_read_u32(rdram, entry + 0u);
            const uint32_t i1 = dc2_read_u32(rdram, entry + 4u);
            const uint32_t i2 = dc2_read_u32(rdram, entry + 8u);
            std::fprintf(stderr, "[G43:srcidx3] n=%u i=%u rec=%u idx=%u,%u,%u\n",
                         mdtN, nodeI, rec, i0, i1, i2);
            g43_log_vec_qword(rdram, "src30", pos0, i0, mdtN, nodeI, rec);
            g43_log_vec_qword(rdram, "src34", pos1, i1, mdtN, nodeI, rec);
            g43_log_vec_qword(rdram, "src3c", pos3, i2, mdtN, nodeI, rec);
        }
        else if (setDataMode == 1u)
        {
            const uint32_t i0 = dc2_read_u32(rdram, entry + 0u);
            const uint32_t i1 = dc2_read_u32(rdram, entry + 4u);
            std::fprintf(stderr, "[G43:srcidx4] n=%u i=%u rec=%u idx=%u,%u\n",
                         mdtN, nodeI, rec, i0, i1);
            g43_log_vec_qword(rdram, "src30", pos0, i0, mdtN, nodeI, rec);
            g43_log_vec_qword(rdram, "src3c", pos3, i1, mdtN, nodeI, rec);
        }
    }
}

static void g43_create_face_packet_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t packet = getRegU32(ctx, 5);
    const uint32_t face = getRegU32(ctx, 6);
    const uint32_t returnPc = getRegU32(ctx, 31);

    uint32_t n = 0u;
    const bool interesting =
        g43_guest_ram_addr(self) &&
        g43_guest_ram_addr(packet) &&
        g43_guest_ram_addr(face) &&
        static_cast<int16_t>(g43_read_u16(rdram, face + 8u)) > 0;
    if (interesting)
    {
        n = g_g43_create_face_dump.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const bool targetVisual = self >= 0x00f40000u && self < 0x00f50000u;
        if (n <= 24u || targetVisual)
        {
            std::fprintf(stderr,
                         "[G43:cfpre] n=%u this=0x%x packet=0x%x face=0x%x +14=0x%x h0=0x%x h2=%d h4=%d h6=%d count8=%d ptrC=0x%x\n",
                         n, self, packet, face,
                         dc2_read_u32(rdram, self + 0x14u),
                         g43_read_u16(rdram, face + 0u),
                         static_cast<int16_t>(g43_read_u16(rdram, face + 2u)),
                         static_cast<int16_t>(g43_read_u16(rdram, face + 4u)),
                         static_cast<int16_t>(g43_read_u16(rdram, face + 6u)),
                         static_cast<int16_t>(g43_read_u16(rdram, face + 8u)),
                         dc2_read_u32(rdram, face + 0xcu));
            g43_log_face_inputs(rdram, self, face, n, 0xffu);
        }
    }

    CreateFacePacket__12mgCVisualMDTFPUiP7mgCFace_0x13ff60(rdram, ctx, runtime);
    if ((ctx->pc & 0x1FFFFFFFu) != (returnPc & 0x1FFFFFFFu))
        return;

    if (n != 0u)
    {
        const uint32_t ret = getRegU32(ctx, 2);
        const bool targetVisual = self >= 0x00f40000u && self < 0x00f50000u;
        if (n <= 24u || targetVisual)
        {
            std::fprintf(stderr, "[G43:cfpost] n=%u ret=0x%x packet=0x%x target=%u\n",
                         n, ret, packet, targetVisual ? 1u : 0u);
            g43_dump_qwords(rdram, "cfpkt", packet, ret < 24u ? ret : 24u);
        }
    }
}

static void g43_log_face_reference_packets(uint8_t *rdram, uint32_t packet)
{
    packet = g43_guest_addr(packet);
    if (!g43_guest_ram_addr(packet))
        return;

    uint32_t refsLogged = 0u;
    for (uint32_t q = 0; q < 16u && refsLogged < 4u; ++q)
    {
        const uint32_t a = packet + q * 16u;
        const uint32_t w0 = dc2_read_u32(rdram, a + 0u);
        const uint32_t w1 = dc2_read_u32(rdram, a + 4u);
        const uint32_t id = (w0 >> 28u) & 0x7u;
        const uint32_t qwc = w0 & 0xFFFFu;
        if (id == 3u && qwc != 0u && g43_guest_ram_addr(w1))
        {
            std::fprintf(stderr,
                         "[G43:ref] packet=0x%x q=%u ref=0x%x qwc=0x%x\n",
                         packet, q, g43_guest_addr(w1), qwc);
            g43_dump_qwords(rdram, "refpkt", w1, qwc < 16u ? qwc : 16u);
            ++refsLogged;
        }
    }
}

static void g43_mdt_face_dump_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);
    const uint32_t a3 = getRegU32(ctx, 7);
    const uint32_t returnPc = getRegU32(ctx, 31);
    Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(rdram, ctx, runtime);
    if ((ctx->pc & 0x1FFFFFFFu) != (returnPc & 0x1FFFFFFFu))
        return;
    const uint32_t ret = getRegU32(ctx, 2);

    const uint32_t n = g_g43_face_dump.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (!g43_guest_ram_addr(self))
        return;

    const bool targetVisual = self >= 0x00f40000u && self < 0x00f50000u;
    if (n > 8u && !targetVisual)
        return;

    const uint32_t head = dc2_read_u32(rdram, self + 0x48u);
    std::fprintf(stderr,
                 "[G43:mdt] n=%u target=%u this=0x%x a1=0x%x drawMgr=0x%x ret=0x%x +14=0x%x +30=0x%x +34=0x%x +38=0x%x +3c=0x%x +44=0x%x +48=0x%x\n",
                 n, targetVisual ? 1u : 0u, self, a1, a3, ret,
                 dc2_read_u32(rdram, self + 0x14u),
                 dc2_read_u32(rdram, self + 0x30u),
                 dc2_read_u32(rdram, self + 0x34u),
                 dc2_read_u32(rdram, self + 0x38u),
                 dc2_read_u32(rdram, self + 0x3cu),
                 dc2_read_u32(rdram, self + 0x44u),
                 head);

    uint32_t node = head;
    for (uint32_t i = 0u; i < 4u && g43_guest_ram_addr(node); ++i)
    {
        const uint32_t mat = dc2_read_u32(rdram, node + 0x0u);
        const uint32_t face = dc2_read_u32(rdram, node + 0x4u);
        const uint32_t next = dc2_read_u32(rdram, node + 0x8u);
        const uint32_t prog = dc2_read_u32(rdram, node + 0xcu);
        const uint32_t packet = dc2_read_u32(rdram, node + 0x10u);
        const uint32_t qwc = dc2_read_u32(rdram, node + 0x14u);
        std::fprintf(stderr,
                     "[G43:node] n=%u i=%u node=0x%x mat=0x%x face=0x%x next=0x%x prog=0x%x packet=0x%x qwc=0x%x\n",
                     n, i, node, mat, face, next, prog, packet, qwc);
        if (g43_guest_ram_addr(face))
        {
            std::fprintf(stderr,
                         "[G43:face] n=%u i=%u h0=0x%x h2=%d h4=%d h6=%d count8=%d ptrC=0x%x next10=0x%x extra14=0x%x\n",
                         n, i,
                         g43_read_u16(rdram, face + 0u),
                         static_cast<int16_t>(g43_read_u16(rdram, face + 2u)),
                         static_cast<int16_t>(g43_read_u16(rdram, face + 4u)),
                         static_cast<int16_t>(g43_read_u16(rdram, face + 6u)),
                         static_cast<int16_t>(g43_read_u16(rdram, face + 8u)),
                         dc2_read_u32(rdram, face + 0xcu),
                         dc2_read_u32(rdram, face + 0x10u),
                         dc2_read_u32(rdram, face + 0x14u));
            g43_log_face_inputs(rdram, self, face, n, i);
        }
        g43_dump_qwords(rdram, "nodepkt", packet, qwc < 16u ? qwc : 16u);
        g43_log_face_reference_packets(rdram, packet);
        node = next;
    }
}
