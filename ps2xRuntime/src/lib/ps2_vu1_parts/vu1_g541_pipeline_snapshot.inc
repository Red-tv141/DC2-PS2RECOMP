// G541: stable, padding-free snapshot of the VU1 interpreter state that is not present in
// VU1State. Called only when DC2_G541_VU_CORPUS is armed. The stream is little-endian and
// versioned; keep append order stable because a future GPU executor consumes it as persistent
// cross-kick state.
void dc2_g541_vu_pipe_snapshot(std::vector<uint8_t> &out)
{
    out.clear();
    out.reserve(768u);
    const auto u32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8u));
        out.push_back(static_cast<uint8_t>(v >> 16u));
        out.push_back(static_cast<uint8_t>(v >> 24u));
    };
    const auto u64 = [&](uint64_t v) {
        for (unsigned i = 0; i < 8u; ++i)
            out.push_back(static_cast<uint8_t>(v >> (i * 8u)));
    };
    const auto f32 = [&](float v) {
        uint32_t bits = 0u;
        std::memcpy(&bits, &v, sizeof(bits));
        u32(bits);
    };

    u32(0x31505556u); // "VUP1"
    u32(1u);          // schema version

    f32(s_vuQPending);
    u32(static_cast<uint32_t>(s_vuQDelay));
    f32(s_vuPPending);
    u32(static_cast<uint32_t>(s_vuPDelay));

    for (uint32_t v : s_vuMacPipe) u32(v);
    for (uint32_t v : s_vuStatusPipe) u32(v);
    u32(s_vuMacVisible);
    u32(s_vuStatusVisible);
    u32(static_cast<uint32_t>(s_g328MacPipeHead));

    for (const auto &vec : s_g428PendVec)
        for (float v : vec) f32(v);
    for (uint8_t v : s_g428PendDest) u32(v);
    for (uint8_t v : s_g428Kind) u32(v);
    u32(s_g428Claim);
    for (float v : s_g428PreVec) f32(v);
    u32(s_g428PreDest);
    u32(s_g428PreKind);
    u32(s_g428PreMac);
    u32(s_g428Armed ? 1u : 0u);

    for (const auto &vec : s_g430HB.vec)
        for (float v : vec) f32(v);
    for (int64_t v : s_g430HB.stamp) u64(static_cast<uint64_t>(v));
    for (uint32_t v : s_g430HB.mac) u32(v);
    for (uint8_t v : s_g430HB.dest) u32(v);
    for (uint8_t v : s_g430HB.kind) u32(v);
    u32(static_cast<uint32_t>(s_g430HB.head));
    u64(static_cast<uint64_t>(s_g430PairBase));
    u32(s_g430HistArmed ? 1u : 0u);

    // Default-off parallel-flag path. Included so a corpus says exactly what the CPU saw.
    u32(s_g106UseLowerFlagRead ? 1u : 0u);
    u32(s_g106LowerMacRead);
    u32(s_g106LowerStatusRead);
}
