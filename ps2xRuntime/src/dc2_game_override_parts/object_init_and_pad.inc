static void dc2_write_u8(uint8_t *rdram, uint32_t addr, uint8_t value)
{
    uint8_t *dst = getMemPtr(rdram, addr);
    if (!dst)
        return;
    dst[0] = value;
}

static void dc2_write_u64(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    dc2_write_u32(rdram, addr, static_cast<uint32_t>(value));
    dc2_write_u32(rdram, addr + 4u, static_cast<uint32_t>(value >> 32u));
}

static void dc2_initialize_mg_texture(uint8_t *rdram, uint32_t tex)
{
    dc2_write_u8(rdram, tex + 0x08u, 0u);
    dc2_write_u16(rdram, tex + 0x00u, 0xFFFFu);
    for (uint32_t i = 0u; i < 4u; ++i)
        dc2_write_u32(rdram, tex + 0x50u + i * 4u, 0u);
    dc2_write_u32(rdram, tex + 0x60u, 0u);
    dc2_write_u64(rdram, tex + 0x48u, 0u);
    dc2_write_u64(rdram, tex + 0x40u, 0u);
    dc2_write_u64(rdram, tex + 0x38u, 0u);
    dc2_write_u8(rdram, tex + 0x48u, 0x05u);
    dc2_write_u16(rdram, tex + 0x06u, 0u);
    dc2_write_u16(rdram, tex + 0x04u, 0u);
    dc2_write_u16(rdram, tex + 0x02u, 0u);
    dc2_write_u32(rdram, tex + 0x64u, 0u);
    dc2_write_u32(rdram, tex + 0x28u, 0u);
    dc2_write_u32(rdram, tex + 0x2Cu, 0u);
    dc2_write_u32(rdram, tex + 0x30u, 0u);
    dc2_write_u32(rdram, tex + 0x68u, 0u);
}

static void dc2_initialize_mg_draw_env(uint8_t *rdram, uint32_t env, uint32_t mode)
{
    dc2_write_u64(rdram, env + 0x00u, 0u);
    dc2_write_u16(rdram, env + 0x00u, 0x8003u);
    dc2_write_u8(rdram, env + 0x07u, 0x10u);
    dc2_write_u8(rdram, env + 0x08u, 0x0Eu);
    dc2_write_u64(rdram, env + 0x10u, 0x0005000Bull);
    dc2_write_u64(rdram, env + 0x30u, 0x44ull);
    dc2_write_u64(rdram, env + 0x18u, mode == 0u ? 0x47ull : 0x48ull);
    dc2_write_u64(rdram, env + 0x28u, mode == 0u ? 0x4Eull : 0x4Full);
    dc2_write_u64(rdram, env + 0x38u, mode == 0u ? 0x42ull : 0x43ull);
}

static void dc2_zero_range(uint8_t *rdram, uint32_t addr, uint32_t size)
{
    uint8_t *dst = getMemPtr(rdram, addr);
    if (!dst)
        return;
    std::memset(dst, 0, size);
}

static void dc2_write_unit_matrix(uint8_t *rdram, uint32_t addr)
{
    for (uint32_t i = 0u; i < 16u; ++i)
        dc2_write_u32(rdram, addr + i * 4u, (i == 0u || i == 5u || i == 10u || i == 15u) ? 0x3F800000u : 0u);
}

static void dc2_initialize_mg_texture_block(uint8_t *rdram, uint32_t block)
{
    dc2_write_u32(rdram, block + 0x00u, 0u);
    dc2_write_u32(rdram, block + 0x04u, 0u);
    dc2_write_u32(rdram, block + 0x08u, 0u);
    dc2_write_u32(rdram, block + 0x0Cu, 0u);
}

static void dc2_initialize_mg_texture_anime(uint8_t *rdram, uint32_t anime)
{
    dc2_write_u32(rdram, anime + 0x00u, 0x18u);
    for (uint32_t i = 0u; i < 0x18u; ++i)
    {
        dc2_write_u32(rdram, anime + 0x004u + i * 4u, 0u);
        dc2_write_u32(rdram, anime + 0x064u + i * 4u, 0u);
        dc2_write_u32(rdram, anime + 0x0C4u + i * 4u, 0u);
        dc2_write_u32(rdram, anime + 0x124u + i * 4u, 0u);
        dc2_write_u32(rdram, anime + 0x184u + i * 4u, 0u);
    }
}

static void dc2_initialize_mg_texture_manager(uint8_t *rdram, uint32_t manager)
{
    dc2_initialize_mg_texture_block(rdram, manager + 0x14u);
    dc2_write_u32(rdram, manager + 0x1C0u, 0u);
    dc2_write_u32(rdram, manager + 0x1D0u, 0u);
    dc2_write_u32(rdram, manager + 0x00Cu, 0u);
    dc2_write_u32(rdram, manager + 0x010u, 0u);
}

static void dc2_initialize_mg_visual(uint8_t *rdram, uint32_t visual)
{
    dc2_write_u32(rdram, visual + 0x00u, 0u);
    dc2_write_u32(rdram, visual + 0x04u, 0u);
    dc2_write_u32(rdram, visual + 0x08u, 0u);
    dc2_write_u32(rdram, visual + 0x10u, 0u);
    dc2_write_u32(rdram, visual + 0x14u, 0u);
}

static void dc2_initialize_mg_3d_sprite(uint8_t *rdram, uint32_t sprite)
{
    dc2_initialize_mg_visual(rdram, sprite);
    dc2_write_u32(rdram, sprite + 0x20u, 0u);
    dc2_write_u32(rdram, sprite + 0x1Cu, 0x003750B0u);
}

static void dc2_initialize_mg_draw_manager(uint8_t *rdram, uint32_t manager)
{
    dc2_write_u32(rdram, manager + 0x68u, 0x40u);
    dc2_write_u32(rdram, manager + 0x6Cu, 0u);
    dc2_write_u32(rdram, manager + 0x70u, 0u);
}

static void dc2_initialize_mg_visual_attr(uint8_t *rdram, uint32_t attr)
{
    dc2_zero_range(rdram, attr, 0x18u);
    dc2_write_u32(rdram, attr + 0x00u, 0xFFFFFFFFu);
    dc2_write_u32(rdram, attr + 0x08u, 1u);
    dc2_write_u32(rdram, attr + 0x14u, 0xFFFFFFFFu);
}

static void dc2_initialize_mg_frame_attr(uint8_t *rdram, uint32_t attr)
{
    dc2_zero_range(rdram, attr, 0x90u);
    dc2_initialize_mg_visual_attr(rdram, attr);
    dc2_write_u32(rdram, attr + 0x70u, 0x43000000u);
    dc2_write_u32(rdram, attr + 0x74u, 0x43000000u);
    dc2_write_u32(rdram, attr + 0x78u, 0x43000000u);
    dc2_write_u32(rdram, attr + 0x7Cu, 0x43000000u);
    dc2_write_u32(rdram, attr + 0x00u, 0xFFFFFFFFu);
    dc2_write_u32(rdram, attr + 0x18u, 1u);
    dc2_write_u32(rdram, attr + 0x08u, 1u);
    dc2_write_u32(rdram, attr + 0x3Cu, 0u);
    dc2_write_u32(rdram, attr + 0x20u, 0x42C80000u);
    dc2_write_u32(rdram, attr + 0x30u, 1u);
    dc2_write_u32(rdram, attr + 0x44u, 0x3F800000u);
    dc2_write_u32(rdram, attr + 0x80u, 1u);
    dc2_write_u32(rdram, attr + 0x5Cu, 0u);
    dc2_write_u32(rdram, attr + 0x58u, 0u);
    dc2_write_u32(rdram, attr + 0x50u, 0u);
    dc2_write_u32(rdram, attr + 0x54u, 0x3F800000u);
    dc2_write_u32(rdram, attr + 0x8Cu, 0u);
    dc2_write_u32(rdram, attr + 0x84u, 0u);
}

static void dc2_initialize_mg_object(uint8_t *rdram, uint32_t object)
{
    dc2_write_u32(rdram, object + 0x10u, 0u);
    dc2_write_u32(rdram, object + 0x14u, 0u);
    dc2_write_u32(rdram, object + 0x18u, 0u);
    dc2_write_u32(rdram, object + 0x1Cu, 0x3F800000u);
    dc2_write_u32(rdram, object + 0x20u, 0u);
    dc2_write_u32(rdram, object + 0x24u, 0u);
    dc2_write_u32(rdram, object + 0x28u, 0u);
    dc2_write_u32(rdram, object + 0x2Cu, 0u);
    dc2_write_u32(rdram, object + 0x30u, 0x3F800000u);
    dc2_write_u32(rdram, object + 0x34u, 0x3F800000u);
    dc2_write_u32(rdram, object + 0x38u, 0x3F800000u);
    dc2_write_u32(rdram, object + 0x3Cu, 0u);
    dc2_write_u32(rdram, object + 0x40u, 1u);
    dc2_write_u32(rdram, object + 0x44u, 0u);
}

static void dc2_initialize_mg_frame(uint8_t *rdram, uint32_t frame)
{
    dc2_write_u32(rdram, frame + 0x60u, 0u);
    dc2_write_u32(rdram, frame + 0x5Cu, 0u);
    dc2_write_u32(rdram, frame + 0x58u, 0u);
    dc2_write_u32(rdram, frame + 0x54u, 0u);
    dc2_write_unit_matrix(rdram, frame + 0x70u);
    dc2_write_unit_matrix(rdram, frame + 0xB0u);
    dc2_write_u32(rdram, frame + 0x50u, 0u);
    dc2_write_u32(rdram, frame + 0x100u, 0u);
    dc2_write_u32(rdram, frame + 0xFCu, 0u);
    dc2_write_u32(rdram, frame + 0xF8u, 0u);
    dc2_write_u32(rdram, frame + 0xF4u, 0u);
    dc2_write_u32(rdram, frame + 0x64u, 0u);
    dc2_write_u32(rdram, frame + 0x68u, 0u);
    dc2_write_u32(rdram, frame + 0x6Cu, 0u);
    dc2_write_u32(rdram, frame + 0xF0u, 0u);
    dc2_initialize_mg_object(rdram, frame);
    dc2_write_u32(rdram, frame + 0x00u, 0x00374F50u);
}

static void dc2_initialize_base_menu_class(uint8_t *rdram, uint32_t self)
{
    // Mirrors __ct__14CBaseMenuClassFv (0x2370B0): fields + table defaults.
    dc2_write_u32(rdram, self + 0x10Cu, 0x00375FC0u);
    dc2_write_u32(rdram, self + 0x0E4u, 0xFFFFFFFFu);
    dc2_zero_range(rdram, self + 0x058u, 0x94u);
    dc2_write_u16(rdram, self + 0x0EEu, 0xFFFFu);
    dc2_write_u16(rdram, self + 0x0F0u, 0u);
    dc2_write_u16(rdram, self + 0x0F2u, 0xFFFFu);
    dc2_write_u16(rdram, self + 0x0ECu, 0u);

    dc2_zero_range(rdram, self, 0x110u);
    dc2_write_u8(rdram, self + 0x004u, 0u);
    dc2_write_u16(rdram, self + 0x000u, 1u);
    dc2_write_u16(rdram, self + 0x002u, 0u);
    dc2_write_u16(rdram, self + 0x006u, 0u);
    dc2_write_u32(rdram, self + 0x008u, 0u);
    dc2_write_u32(rdram, self + 0x00Cu, 0u);
    dc2_write_u32(rdram, self + 0x010u, 0x80u);
    dc2_write_u16(rdram, self + 0x014u, 0u);

    for (uint32_t i = 0u; i < 2u; ++i)
    {
        const uint32_t base = self + 0x018u + i * 0x20u;
        dc2_write_u32(rdram, base + 0x00u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, base + 0x04u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, base + 0x08u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, base + 0x0Cu, 0xFFFFFFFFu);
        dc2_write_u32(rdram, base + 0x10u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, base + 0x14u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, base + 0x18u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, base + 0x1Cu, 0xFFFFFFFFu);
    }

    dc2_write_u16(rdram, self + 0x0F4u, 0xFFFFu);
    dc2_write_u32(rdram, self + 0x0F8u, 0u);
    // SetAskParam(this, nullptr) path zeroes ask-mode struct.
    dc2_zero_range(rdram, self + 0x058u, 0x94u);
    dc2_zero_range(rdram, self + 0x0FCu, 0x10u);
}

static void dc2_initialize_mgccamerafollow(uint8_t *rdram,
                                            uint32_t camera,
                                            uint32_t distanceBits,
                                            uint32_t heightBits,
                                            uint32_t angleBits,
                                            uint32_t speedBits)
{
    // Base mgCCamera state.
    dc2_write_u32(rdram, camera + 0x00u, 0u);
    dc2_write_u32(rdram, camera + 0x04u, heightBits);
    dc2_write_u32(rdram, camera + 0x08u, distanceBits);
    dc2_write_u32(rdram, camera + 0x0Cu, 0x3F800000u);
    dc2_write_u32(rdram, camera + 0x10u, 0u);
    dc2_write_u32(rdram, camera + 0x14u, 0u);
    dc2_write_u32(rdram, camera + 0x18u, 0u);
    dc2_write_u32(rdram, camera + 0x1Cu, 0u);
    dc2_write_u32(rdram, camera + 0x20u, 0u);
    dc2_write_u32(rdram, camera + 0x24u, heightBits);
    dc2_write_u32(rdram, camera + 0x28u, distanceBits);
    dc2_write_u32(rdram, camera + 0x2Cu, 0x3F800000u);
    dc2_write_u32(rdram, camera + 0x30u, 0u);
    dc2_write_u32(rdram, camera + 0x34u, 0u);
    dc2_write_u32(rdram, camera + 0x38u, 0u);
    dc2_write_u32(rdram, camera + 0x3Cu, 0u);
    dc2_write_u32(rdram, camera + 0x40u, 0u);
    dc2_write_u32(rdram, camera + 0x44u, 0u);
    dc2_write_u32(rdram, camera + 0x48u, speedBits);
    dc2_write_u32(rdram, camera + 0x4Cu, speedBits);
    dc2_write_u32(rdram, camera + 0x58u, 0x3DCCCCCDu);
    dc2_write_u32(rdram, camera + 0x5Cu, 0u);

    // mgCCameraFollow vtable and follow fields.
    dc2_write_u32(rdram, camera + 0x60u, 0x00374E70u);
    dc2_write_u32(rdram, camera + 0x70u, 0u);
    dc2_write_u32(rdram, camera + 0x74u, 0u);
    dc2_write_u32(rdram, camera + 0x78u, 0u);
    dc2_write_u32(rdram, camera + 0x7Cu, 0x3F800000u);
    dc2_write_u32(rdram, camera + 0x80u, 0u);
    dc2_write_u32(rdram, camera + 0x84u, 0u);
    dc2_write_u32(rdram, camera + 0x88u, 0u);
    dc2_write_u32(rdram, camera + 0x8Cu, 0u);
    dc2_write_u32(rdram, camera + 0x90u, distanceBits);
    dc2_write_u32(rdram, camera + 0x94u, heightBits);
    dc2_write_u32(rdram, camera + 0x98u, angleBits);
    dc2_write_u32(rdram, camera + 0x9Cu, angleBits);
    dc2_write_u32(rdram, camera + 0xA0u, 1u);
    dc2_write_u32(rdram, camera + 0xB0u, 0u);
    dc2_write_u32(rdram, camera + 0xB4u, 0u);
    dc2_write_u32(rdram, camera + 0xB8u, 0u);
}

static void basemenuclass_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    if (self != 0u)
    {
        dc2_initialize_base_menu_class(rdram, self);
    }
    setReturnU32(ctx, self);
    ctx->pc = getRegU32(ctx, 31);
}

static void menucostumesel_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)runtime;
    const uint32_t self = getRegU32(ctx, 4);
    if (self == 0u)
    {
        setReturnU32(ctx, 0u);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }

    const uint32_t gp = getRegU32(ctx, 28);

    dc2_initialize_base_menu_class(rdram, self);
    dc2_write_u32(rdram, self + 0x10Cu, 0x00376270u); // __vt__15CMenuCostumeSel

    dc2_initialize_mgccamerafollow(
        rdram,
        self + 0x110u,
        0x42C80000u, // final distance after SetDistance(100.0f)
        0x40400000u, // final height after SetHeight(3.0f)
        0x00000000u, // angle
        0x40800000u  // final speed after SetSpeed(4.0f, -1.0f)
    );

    // Init__9mgCMemoryFv at +0x228
    dc2_zero_range(rdram, self + 0x228u, 0x2Cu);

    dc2_write_u32(rdram, self + 0x1D0u, 0u);
    if (gp != 0u)
    {
        // MenuCosutumeLoadPhase (gp-0x63dc)
        dc2_write_u16(rdram, gp + 0xFFFF9C24u, 0u);
    }
    dc2_write_u32(rdram, self + 0x224u, 0u);

    // GetCharaDataPtr(GetUserDataMan(), 0) is refreshed again in KeyStep; zero is safe bootstrap.
    dc2_write_u32(rdram, self + 0x2D0u, 0u);

    dc2_write_u16(rdram, self + 0x1DEu, 0u);
    dc2_write_u16(rdram, self + 0x1E0u, 0u);
    dc2_write_u16(rdram, self + 0x1E2u, 0u);
    dc2_write_u32(rdram, self + 0x29Cu, 0u);
    dc2_write_u32(rdram, self + 0x2A0u, 0u);
    dc2_write_u32(rdram, self + 0x2A4u, 0u);
    dc2_write_u32(rdram, self + 0x2A8u, 0u);
    dc2_write_u32(rdram, self + 0x2ACu, 0u);
    dc2_write_u32(rdram, self + 0x2B0u, 0u);
    dc2_write_u32(rdram, self + 0x2B4u, 0u);
    dc2_write_u32(rdram, self + 0x294u, 0u);
    dc2_write_u16(rdram, self + 0x258u, 0u);
    dc2_write_u32(rdram, self + 0x25Cu, 0u);
    dc2_write_u32(rdram, self + 0x2D4u, 0u);
    dc2_write_u32(rdram, self + 0x2D8u, 0u);

    dc2_write_u32(rdram, self + 0x260u, 0x41700000u);
    dc2_write_u32(rdram, self + 0x264u, 0xC1600000u);
    dc2_write_u32(rdram, self + 0x268u, 0x40800000u);
    dc2_write_u32(rdram, self + 0x26Cu, 0x3F800000u);
    dc2_write_u32(rdram, self + 0x270u, 0u);
    dc2_write_u32(rdram, self + 0x274u, 0x3DCCCCCDu);
    dc2_write_u32(rdram, self + 0x278u, 0u);
    dc2_write_u32(rdram, self + 0x27Cu, 0x3F800000u);

    dc2_zero_range(rdram, self + 0x1E4u, 0x30u);
    dc2_write_u32(rdram, self + 0x214u, self + 0x1F4u);
    dc2_write_u32(rdram, self + 0x218u, self + 0x1E4u);
    dc2_write_u32(rdram, self + 0x21Cu, self + 0x204u);
    dc2_write_u32(rdram, self + 0x220u, 0u);
    dc2_write_u32(rdram, self + 0x290u, 0u);
    dc2_write_u32(rdram, self + 0x298u, 0u);

    setReturnU32(ctx, self);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgctexture_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t tex = getRegU32(ctx, 4);
    dc2_initialize_mg_texture(rdram, tex);
    setReturnU32(ctx, tex);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgctextureblock_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t block = getRegU32(ctx, 4);
    dc2_initialize_mg_texture_block(rdram, block);
    setReturnU32(ctx, block);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgctexturemanager_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t manager = getRegU32(ctx, 4);
    dc2_initialize_mg_texture_manager(rdram, manager);
    setReturnU32(ctx, manager);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgctextureanime_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t anime = getRegU32(ctx, 4);
    dc2_initialize_mg_texture_anime(rdram, anime);
    setReturnU32(ctx, anime);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgc3dsprite_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t sprite = getRegU32(ctx, 4);
    dc2_initialize_mg_3d_sprite(rdram, sprite);
    setReturnU32(ctx, sprite);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgcdrawmanager_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t manager = getRegU32(ctx, 4);
    dc2_initialize_mg_draw_manager(rdram, manager);
    setReturnU32(ctx, manager);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgcframe_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t frame = getRegU32(ctx, 4);
    dc2_initialize_mg_frame(rdram, frame);
    setReturnU32(ctx, frame);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgcframeattr_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t attr = getRegU32(ctx, 4);
    dc2_initialize_mg_frame_attr(rdram, attr);
    setReturnU32(ctx, attr);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgcdrawenv_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t env = getRegU32(ctx, 4);
    dc2_initialize_mg_draw_env(rdram, env, 0u);
    setReturnU32(ctx, env);
    ctx->pc = getRegU32(ctx, 31);
}

static void mgcdrawprim_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t prim = getRegU32(ctx, 4);

    dc2_initialize_mg_draw_env(rdram, prim + 0x10u, 0u);
    dc2_initialize_mg_texture(rdram, prim + 0x58u);
    dc2_write_u32(rdram, prim + 0x04u, 0u);
    dc2_write_u32(rdram, prim + 0x08u, 0u);
    dc2_write_u32(rdram, prim + 0x00u, 0u);
    dc2_write_u32(rdram, prim + 0xD0u, 1u);
    dc2_write_u32(rdram, prim + 0xF8u, 0x3F800000u);
    dc2_write_u64(rdram, prim + 0x50u, 0x100ull);
    dc2_write_u32(rdram, prim + 0xC8u, 1u);
    dc2_write_u32(rdram, prim + 0xCCu, 1u);
    dc2_write_u32(rdram, prim + 0xFCu, 0u);

    const uint32_t n = ++calls;
    if (f33_trace_drawprim_enabled() && (n <= 64u || (n % 512u) == 0u))
    {
        std::fprintf(stderr,
                     "[F33:DrawPrim.ct] n=%u pc=0x%x ra=0x%x prim=0x%x\n",
                     n, ctx->pc, getRegU32(ctx, 31), prim);
    }

    setReturnU32(ctx, prim);
    ctx->pc = getRegU32(ctx, 31);
}

static void dc2_seed_title_mode(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu);
    if (titleInfo == 0u)
        return;

    const uint32_t mode = dc2_read_u32(rdram, titleInfo + 0u);
    const uint32_t sub = dc2_read_u32(rdram, titleInfo + 4u);
    if ((mode == 3u || mode == 0u) && sub == 0xFFFFFFFFu)
    {
        // F21 seed: the intro FMV (mode 0 RushMovie) / MC-check (mode 3) can't play
        // headless, so jump the title into TitleMode (mode 1) so the 3D background runs.
        dc2_write_u32(rdram, titleInfo + 0u, 1u);
        dc2_write_u32(rdram, titleInfo + 4u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, titleInfo + 8u, 0u);
        dc2_write_u32(rdram, titleInfo + 0x38u, 0u);
        // G127 (default: natural fade-in to PRESS-START, kill-switch DC2_G127_SEED_MENU=1
        // restores the old jump-straight-to-menu behavior). The original seed forced
        // TitlePhase=1 (the menu sub-state) + all alphas=128, so the New Game/Continue/Extras
        // /Options menu popped in instantly, skipping the real title sequence (cavern ->
        // "DARK CLOUD 2" logo fades in slowly -> PRESS START -> menu). TitleModeKey's phase
        // machine: phase -2 holds (TitleInfo+0x18 countdown, cavern only, logo invisible) ->
        // phase -1 ramps TitleInfo+0x1c 0->128 at 0.53/frame (the title fades in) -> phase 0
        // = full press-start (waits for Start to reach phase 1 = menu). Seed TitleModeInit's
        // own phase -2 + alpha-0 state so the natural fade runs, instead of phase 1/alpha 128.
        // (The G9 headless costume route, env-gated DC2_G9_COSTUME, still forces TitleModeKey
        // ret=5 to drive past press-start.)
        static const bool g127SeedMenu = dc2_env_flag_enabled("DC2_G127_SEED_MENU");
        if (g127SeedMenu)
        {
            dc2_write_u32(rdram, titleInfo + 0x14u, 0x43000000u);
            dc2_write_u32(rdram, titleInfo + 0x1Cu, 0x43000000u);
            dc2_write_u32(rdram, titleInfo + 0x20u, 0x43000000u);
            dc2_write_u32(rdram, titleInfo + 0x28u, 0x43000000u);
            dc2_write_u16(rdram, gp + 0xFFFF99A8u, 1u);  // TitlePhase = 1 (menu)
        }
        else
        {
            // == TitleModeInit's title-mode setup (the seed bypasses the TitleModeInit call):
            dc2_write_u32(rdram, titleInfo + 0x14u, 0u);    // logo alpha (CalcPushAlpha drives)
            dc2_write_u32(rdram, titleInfo + 0x18u, 0xF0u); // phase -2 hold countdown (240)
            dc2_write_u32(rdram, titleInfo + 0x1Cu, 0u);    // title/chronicle (fades in phase -1)
            dc2_write_u32(rdram, titleInfo + 0x20u, 0u);    // menu hidden
            dc2_write_u32(rdram, titleInfo + 0x24u, 0u);
            dc2_write_u32(rdram, titleInfo + 0x28u, 0u);    // cursor hidden
            dc2_write_u16(rdram, gp + 0xFFFF99A8u, 0xFFFEu); // TitlePhase = -2 (fade sequence)
        }
    }
}

// Phase F21: seed TitleLoop into title-input mode and own read_pad directly so
// CGamePad+0x04 gets an active-high mask every frame.
static void read_pad_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    dc2_seed_title_mode(rdram, ctx);

    const uint32_t statusAddr = getRegU32(ctx, 4);
    const int port = static_cast<int>(getRegU32(ctx, 5)) & 1;
    const int slot = static_cast<int>(getRegU32(ctx, 6));
    uint32_t mask = dc2_pad_mask(runtime, port, slot);
    // F66: fold in the in-dungeon free-roam buttons (the title clock that feeds
    // dc2_pad_mask freezes in-dungeon). dc2_write_pad_status emits the left stick.
    if (port == 0 && !g_pad_live_connected && g_f66_dungeon_active)
        mask |= g_f66_dungeon_buttons;
    // G10: inject the costume-prompt Cross confirm (set by the load-phase driver).
    if (port == 0)
        mask |= g_g9_confirm_mask.load(std::memory_order_relaxed);
    dc2_write_pad_status(rdram, statusAddr, mask);
    {
        static const bool trace = dc2_env_flag_enabled("DC2_TRACE_PAD_INPUT");
        if (trace && g_f66_dungeon_active)
        {
            static uint32_t c = 0u;
            if ((c++ % 60u) == 0u)
                std::fprintf(stderr, "[F66:readpad] port=%d mask=0x%x lx=0x%02x ly=0x%02x\n",
                             port, mask, g_f66_dungeon_lx, g_f66_dungeon_ly);
        }
    }

    setReturnU32(ctx, 1u);
    ctx->pc = getRegU32(ctx, 31);
}

// G7-fix: SIF IOP module-management stub returning 0. The recompiler emits a
// throwing TODO_NAMED body for these addresses, and the dc2_auto_stubs
// bindAddressHandler entries do NOT resolve (there is no ps2_stubs::sceSifSearch*
// /Unload/LoadStart* ג€” they are absent from Kernel/Stubs/SIF.cpp), so binding
// silently fails and the throwing body stays live. The IOP/module subsystem is
// faked in this port, so "module not present / no-op, return 0" is the correct
// behaviour (matches the auto-stub free functions). Registered below in
// applyDC2Phase9Stubs (last-write-wins) so it overrides the recompiled TODO body.
// Without this, simply playing into any code path that probes for an IOP module
// (e.g. sceSifSearchModuleByName) throws std::runtime_error and ends the process.
static void sif_module_return_zero_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)rdram;
    (void)runtime;
    setReturnS32(ctx, 0);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F21: pad_button_read shim. Decomp at 0x14a3d0 shows the real function:
//   1. scePadRead -> 32-byte buffer.
//   2. *param_1 = (mask_lo | mask_hi<<8) ^ 0xFFFF   // active-high mask at PAD_STATUS+0x00
//      param_1[4] = right-stick X, etc.
//   3. return high-nibble of byte[0x01]  -> pad type (e.g. 0x4 / 0x7).
// CGamePad accessors (On/On2/Down/Up at 0x14b3c0+) test *(CGamePad+0x04)&mask, where
// PAD_STATUS lives at CGamePad+0x04 ג€” so the button mask MUST be written into
// *param_1, not just returned in $v0. read_pad__FP10PAD_STATUSii (0x14a490) only
// commits the mask if the value is *stable across calls*; transient pulses get
// dropped. Hold each synthetic press steady for many calls.
static void pad_button_read_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t bufAddr = getRegU32(ctx, 4); // $a0 = PAD_STATUS pointer
    const int port = static_cast<int>(getRegU32(ctx, 5)) & 1;
    const int slot = static_cast<int>(getRegU32(ctx, 6));
    uint8_t buf[32] = {0};
    uint32_t mask = 0u;
    // PHASE F18: synthesise repeated button presses so the menu can advance without
    // a live pad. Pattern (per-port counter): pulse Start once, then alternate Cross
    // pulses every ~80 calls forever. This bombards the titleג†’main-menuג†’file-select
    // ג†’ level-select chain. Live pad backend used only on port 0 if it produced
    // a non-zero mask, otherwise the synthetic schedule wins.
    uint32_t n = 0u;
    if (!f40_script_input_requested())
    {
        static uint32_t call_count[2] = {0u, 0u};
        n = call_count[port]++;
    }

    if (f40_script_input_requested())
    {
        mask = (port == 0 && g_f40_active) ? static_cast<uint32_t>(g_f40_pressed) : 0u;
    }
    // Honour live input if the backend supplies any button.
    else if (runtime && runtime->padBackend().readState(port, slot, buf, sizeof(buf)))
    {
        const uint16_t activeLow = static_cast<uint16_t>(buf[2] | (buf[3] << 8));
        const uint32_t live = static_cast<uint32_t>(static_cast<uint16_t>(~activeLow));
        if (live != 0u)
            mask = live;
    }

    // PHASE F21 ג€” synthetic schedule. read_pad__FP10PAD_STATUSii (0x14a490) only
    // commits the mask into CGamePad+0x04 when it stays STABLE across calls; a
    // change resets the state machine, so each held button must persist for
    // many calls. The title-screen handler TitleModeKey__Fv (0x2A1220 case 0)
    // advances when MenuCheckPushButton__Fv (0x23E1B0) returns bit 0x10 set,
    // which only R1 (0x0800) decodes to directly. After R1 advances titleג†’menu,
    // hold Cross for the OK in MenuMainKey. Cycle (240 calls @ ~30 fps):
    //   [0..60)    R1   ג€” drive "PUSH START" ג†’ TitleModeKey case 0
    //   [60..120)  idle ג€” let read_pad observe release before next press
    //   [120..180) Cross ג€” confirm menu selection
    //   [180..240) idle
    if (!f40_script_input_requested() && mask == 0u)
    {
        // UpDate__8CGamePadFv (0x14a930) snapshots currג†’prev BEFORE read_pad,
        // so the Down() accessor's edge mask is `curr & ~prev`. A button held
        // continuously is curr==prev ג†’ edge=0. Drive distinct edges by holding
        // *idle* between presses long enough for prev to relax to 0, then
        // re-press briefly. Cycle (240 calls):
        //   [0..30)    idle  ג€” let prev settle to 0
        //   [30..40)   R1    ג€” 0ג†’0x800 rising edge on the first frame
        //   [40..120)  idle  ג€” relax + give game time to react
        //   [120..130) Cross ג€” confirm menu
        //   [130..240) idle  ג€” long relax before next R1
        const uint32_t cycle = n % 240u;
        if (cycle >= 30u && cycle < 40u)
            mask = 0x0800u; // R1
        else if (cycle >= 120u && cycle < 130u)
            mask = 0x4000u; // Cross
    }

    // F66: in-dungeon free-roam ג€” the present-loop driver owns the pad. Emit the
    // held buttons AND a deflected LEFT STICK (free-roam movement reads the analog
    // stick via Analog__11CPadControl, not the D-pad).
    uint8_t outLX = 0x80u, outLY = 0x80u; // PAD_STATUS layout: [1]=LY, [2]=LX, [3]=RY, [4]=RX
    if (g_f66_dungeon_active)
    {
        mask = g_f66_dungeon_buttons;
        outLX = g_f66_dungeon_lx;
        outLY = g_f66_dungeon_ly;
    }

    // Write the active-high mask into *param_1 (PAD_STATUS+0x00) where the
    // CGamePad accessors read it. Sticks default to 0x80 centre; F66 deflects the
    // left stick in-dungeon.
    if (bufAddr != 0u)
    {
        uint8_t *dst = getMemPtr(rdram, bufAddr);
        if (dst)
        {
            // Little-endian 32-bit store of the button mask.
            dst[0] = static_cast<uint8_t>(mask & 0xFFu);
            dst[1] = static_cast<uint8_t>((mask >> 8) & 0xFFu);
            dst[2] = 0u;
            dst[3] = 0u;
            // Stick ints at param_1[1..4] (PAD_STATUS+4/+8/+0xc/+0x10) = LY,LX,RY,RX.
            const uint8_t sticks[4] = { outLY, outLX, 0x80u, 0x80u };
            for (int i = 0; i < 4; ++i)
            {
                dst[4 + i * 4 + 0] = sticks[i];
                dst[4 + i * 4 + 1] = 0u;
                dst[4 + i * 4 + 2] = 0u;
                dst[4 + i * 4 + 3] = 0u;
            }
        }
    }

    // Return the pad-type nibble: 7 = DUALSHOCK analog (non-zero so read_pad's
    // iVar9-bookkeeping treats this as a successful read).
    setReturnU32(ctx, 0x7u);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F22: mgCCamera constructor at 0x131630. Auto-stub returns 0 so the
// camera object never receives its vtable pointer at +0x60. MenuCamInit
// @ 0x234540 then does `t9 = *(cam+0x60); t9 = *(t9+0x10); jr t9` which
// dispatches PC=0. Decompile + raw assembly at 0x131630 in ref/assembly.txt
// show:
//   *(this+0x60) = __vt__9mgCCamera (= 0x00374EA0)
//   *(this+0x48) = f12 (1.0f if f12 <= 0)
//   *(this+0x4c) = *(this+0x48)
//   *(this+0x40) = 0
//   *(this+0x44) = 0
//   *(this+0x58) = 0.1f (0x3dcccccd)
//   *(this+0x5c) = 0
//   return this
static void mgccamera_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t thisAddr = getRegU32(ctx, 4); // $a0
    float f12 = ctx->f[12];
    if (!(f12 > 0.0f))
        f12 = 1.0f;

    if (thisAddr != 0u)
    {
        uint8_t *p = getMemPtr(rdram, thisAddr);
        if (p)
        {
            uint32_t f12bits;
            std::memcpy(&f12bits, &f12, sizeof(f12bits));
            dc2_write_u32(rdram, thisAddr + 0x40u, 0u);
            dc2_write_u32(rdram, thisAddr + 0x44u, 0u);
            dc2_write_u32(rdram, thisAddr + 0x48u, f12bits);
            dc2_write_u32(rdram, thisAddr + 0x4Cu, f12bits);
            dc2_write_u32(rdram, thisAddr + 0x58u, 0x3DCCCCCDu);
            dc2_write_u32(rdram, thisAddr + 0x5Cu, 0u);
            dc2_write_u32(rdram, thisAddr + 0x60u, 0x00374EA0u);
        }
    }

    setReturnU32(ctx, thisAddr);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F42: mgCCameraFollow constructor at 0x131a90.
// Auto-stub returns 0, leaving this+0x60 (vtable) uninitialized.
// TitleMapDraw calls GetCamera ג†’ dispatches (**(code**)(puVar1[0x18]+8))(puVar1,1)
// which jumps to 0x33313237 (garbage) ג€” the pre-existing missing-function crash.
//
// Assembly at 0x131a90:
//   s0 = a0 (this)
//   jal 0x131630 (__ct__9mgCCameraFf), delay: f12 = f15 (param_4 = speed/fov)
//   sw __vt__15mgCCameraFollow=0x374E70, 0x60(this)
//   sw 0 ג†’ 0xb0,0xb4,0xb8
//   swc1 f14 (param_3=angle) ג†’ 0x98, 0x9c
//   swc1 f12 (param_1=dist)  ג†’ 0x90
//   swc1 f13 (param_2=ht)    ג†’ 0x94
//   jal 0x0012f230 (mgZeroVector, zeroes this+0x80..0x8f)
//   sw 1 ג†’ 0xa0; return this
static void mgccamerafollow_ct_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t thisAddr = getRegU32(ctx, 4); // $a0
    if (thisAddr == 0u)
    {
        setReturnU32(ctx, 0u);
        ctx->pc = getRegU32(ctx, 31);
        return;
    }

    // --- base mgCCamera ctor (0x131630) inlined: f15 is the speed float ---
    float f15 = ctx->f[15];
    float fov = (f15 > 0.0f) ? f15 : 1.0f;
    uint32_t fovBits;
    std::memcpy(&fovBits, &fov, sizeof(fovBits));
    dc2_write_u32(rdram, thisAddr + 0x40u, 0u);
    dc2_write_u32(rdram, thisAddr + 0x44u, 0u);
    dc2_write_u32(rdram, thisAddr + 0x48u, fovBits);
    dc2_write_u32(rdram, thisAddr + 0x4Cu, fovBits);
    dc2_write_u32(rdram, thisAddr + 0x58u, 0x3DCCCCCDu); // 0.1f
    dc2_write_u32(rdram, thisAddr + 0x5Cu, 0u);

    // --- mgCCameraFollow vtable (lui 0x37 / addiu 0x4e70 = 0x374E70) ---
    dc2_write_u32(rdram, thisAddr + 0x60u, 0x00374E70u);

    // --- mgZeroVector(this+0x80): zero the follow-target 4-vector ---
    dc2_write_u32(rdram, thisAddr + 0x80u, 0u);
    dc2_write_u32(rdram, thisAddr + 0x84u, 0u);
    dc2_write_u32(rdram, thisAddr + 0x88u, 0u);
    dc2_write_u32(rdram, thisAddr + 0x8Cu, 0u);

    // --- follow camera fields ---
    // f12=param_1 (distance 40.0f), f13=param_2 (height 30.0f), f14=param_3 (angle 0.0f)
    uint32_t p1bits, p2bits, p3bits;
    float f12 = ctx->f[12]; std::memcpy(&p1bits, &f12, sizeof(p1bits));
    float f13 = ctx->f[13]; std::memcpy(&p2bits, &f13, sizeof(p2bits));
    float f14 = ctx->f[14]; std::memcpy(&p3bits, &f14, sizeof(p3bits));
    dc2_write_u32(rdram, thisAddr + 0x90u, p1bits); // distance
    dc2_write_u32(rdram, thisAddr + 0x94u, p2bits); // height
    dc2_write_u32(rdram, thisAddr + 0x98u, p3bits); // angle
    dc2_write_u32(rdram, thisAddr + 0x9Cu, p3bits); // angle copy
    dc2_write_u32(rdram, thisAddr + 0xa0u, 1u);     // follow active
    dc2_write_u32(rdram, thisAddr + 0xb0u, 0u);     // target pos x
    dc2_write_u32(rdram, thisAddr + 0xb4u, 0u);     // target pos y
    dc2_write_u32(rdram, thisAddr + 0xb8u, 0u);     // target pos z

    setReturnU32(ctx, thisAddr);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F22: MenuCamInit @ 0x234540 raw MIPS:
//   v0 = 0x35<<16 + 0xA00 = 0x350A00
//   v1 = *(gp - 0x6B0C)            // camera ptr
//   lq  a0, 0x0(v0); sq a0, 0x80(v1)
//   v0 = 0x350A10
//   lq  v1, 0x0(v0); sq v1, 0x90(*(gp-0x6B0C))
//   swc1 f12, 0xA0(camera)
//   a0 = camera; t9 = *(a0+0x60); t9 = *(t9+0x10); jr t9
//
// The generated `jr t9` is a tail-call: the vfunc runs then returns to
// MenuMainInit's continuation at 0x232EE8. Dispatch loop can't look up
// 0x232EE8 (no function entry there). Override here writes the same
// fields and returns to $ra directly, skipping the second vtable->Init
// call (Init was already invoked at 0x232ED0 right after the ctor).
static void menucaminit_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t camera = dc2_read_u32(rdram, gp + 0xFFFF94F4u);
    if (camera != 0u)
    {
        const uint8_t *srcA = getConstMemPtr(rdram, 0x00350A00u);
        const uint8_t *srcB = getConstMemPtr(rdram, 0x00350A10u);
        uint8_t *dstA = getMemPtr(rdram, camera + 0x80u);
        uint8_t *dstB = getMemPtr(rdram, camera + 0x90u);
        if (srcA && dstA) std::memcpy(dstA, srcA, 16);
        if (srcB && dstB) std::memcpy(dstB, srcB, 16);

        float f12 = ctx->f[12];
        uint32_t f12bits;
        std::memcpy(&f12bits, &f12, sizeof(f12bits));
        dc2_write_u32(rdram, camera + 0xA0u, f12bits);
    }

    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F23c: libc 64-bit / FP conversion helpers.
//
// ABI verified against ref/assembly.txt for each address. Contrary to the
// classic libgcc o32 "two 32-bit registers per 64-bit value" pattern, the
// libgcc-ps2 build used by DC2 passes one full 64-bit value per GPR (the
// callee `dsra32`/`dsll32` against $a0 / $a1 only ג€” $a2 / $a3 are written,
// never read ג€” and the caller emits `ld a0,...` / `sd v0,...` for the full
// 64-bit transfer). Doubles are soft-float and therefore travel as raw
// 64-bit bits through the same GPRs (no $f12/$f13 use in __fixdfdi or
// __floatdidf prologues / epilogues).
//
// Helpers read $a0 / $a1 with `_mm_extract_epi64(..., 0)` (lower 64 of the
// 128-bit GPR), compute on host types, and write the full 64-bit result via
// `setReturnU64` (also mirrors the high 32 into $v1 for legacy callers).
// Division-by-zero is shimmed to a quiet zero result instead of the libgcc
// `break 0x7` trap ג€” the prompt requires that the shim must not halt.
static uint64_t dc2_get_reg_u64(const R5900Context *ctx, int reg)
{
    if (reg <= 0 || reg > 31)
        return 0u;
    return static_cast<uint64_t>(_mm_extract_epi64(ctx->r[reg], 0));
}

static void umoddi3_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint64_t a = dc2_get_reg_u64(ctx, 4);
    const uint64_t b = dc2_get_reg_u64(ctx, 5);

    uint64_t r;
    if (b == 0u)
    {
        static bool warned = false;
        if (!warned)
        {
            std::fprintf(stderr, "[dc2] __umoddi3 div-by-zero at pc=0x%x ra=0x%x\n",
                         ctx->pc, getRegU32(ctx, 31));
            warned = true;
        }
        r = 0u;
    }
    else
    {
        r = a % b;
    }

    setReturnU64(ctx, r);
    ctx->pc = getRegU32(ctx, 31);
}

static void moddi3_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int64_t a = static_cast<int64_t>(dc2_get_reg_u64(ctx, 4));
    const int64_t b = static_cast<int64_t>(dc2_get_reg_u64(ctx, 5));

    int64_t r;
    if (b == 0)
    {
        static bool warned = false;
        if (!warned)
        {
            std::fprintf(stderr, "[dc2] __moddi3 div-by-zero at pc=0x%x ra=0x%x\n",
                         ctx->pc, getRegU32(ctx, 31));
            warned = true;
        }
        r = 0;
    }
    else if (a == INT64_MIN && b == -1)
    {
        // INT64_MIN / -1 overflows; libgcc treats it as 0 mod and well-defined.
        r = 0;
    }
    else
    {
        r = a % b;
    }

    setReturnU64(ctx, static_cast<uint64_t>(r));
    ctx->pc = getRegU32(ctx, 31);
}

static void divdi3_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int64_t a = static_cast<int64_t>(dc2_get_reg_u64(ctx, 4));
    const int64_t b = static_cast<int64_t>(dc2_get_reg_u64(ctx, 5));

    int64_t r;
    if (b == 0)
    {
        static bool warned = false;
        if (!warned)
        {
            std::fprintf(stderr, "[dc2] __divdi3 div-by-zero at pc=0x%x ra=0x%x\n",
                         ctx->pc, getRegU32(ctx, 31));
            warned = true;
        }
        r = 0;
    }
    else if (a == INT64_MIN && b == -1)
    {
        r = INT64_MIN;
    }
    else
    {
        r = a / b;
    }

    setReturnU64(ctx, static_cast<uint64_t>(r));
    ctx->pc = getRegU32(ctx, 31);
}

static void udivdi3_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint64_t a = dc2_get_reg_u64(ctx, 4);
    const uint64_t b = dc2_get_reg_u64(ctx, 5);

    uint64_t r;
    if (b == 0u)
    {
        static bool warned = false;
        if (!warned)
        {
            std::fprintf(stderr, "[dc2] __udivdi3 div-by-zero at pc=0x%x ra=0x%x\n",
                         ctx->pc, getRegU32(ctx, 31));
            warned = true;
        }
        r = 0u;
    }
    else
    {
        r = a / b;
    }

    setReturnU64(ctx, r);
    ctx->pc = getRegU32(ctx, 31);
}

static void muldi3_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Signed and unsigned multiplication produce identical low 64 bits.
    const uint64_t a = dc2_get_reg_u64(ctx, 4);
    const uint64_t b = dc2_get_reg_u64(ctx, 5);

    const uint64_t r = a * b;

    setReturnU64(ctx, r);
    ctx->pc = getRegU32(ctx, 31);
}

static void fixdfdi_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // PS2 EE FPU is single-precision only; libgcc-ps2 keeps doubles as raw
    // 64-bit bits in GPRs. The input double bits arrive in $a0.
    const uint64_t bits = dc2_get_reg_u64(ctx, 4);

    double d;
    std::memcpy(&d, &bits, sizeof(d));

    int64_t r;
    if (!(d == d))
    {
        // NaN -> 0 (libgcc behaviour is undefined; pick the safe value).
        r = 0;
    }
    else if (d >= static_cast<double>(INT64_MAX))
    {
        r = INT64_MAX;
    }
    else if (d <= static_cast<double>(INT64_MIN))
    {
        r = INT64_MIN;
    }
    else
    {
        r = static_cast<int64_t>(d);
    }

    setReturnU64(ctx, static_cast<uint64_t>(r));
    ctx->pc = getRegU32(ctx, 31);
}

static void floatdidf_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Input: signed int64 in $a0. Output: double raw bits in $v0.
    const int64_t a = static_cast<int64_t>(dc2_get_reg_u64(ctx, 4));
    const double d = static_cast<double>(a);

    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));

    setReturnU64(ctx, bits);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F25: Silence the six cosmetic `fioOpen error: ... map\.{map,cfg,mpk,sky,ipk,efp}`
// boot lines. F24 established (via PC-histogram empty across all five 5 s windows
// post-ezMidi-burst) that the game thread is blocked in a silent host sema/sleep,
// not spinning in any map-load chain, so the lines are not a blocker ג€” only noise.
// F19 already established the misses are non-fatal (game falls through to
// `def.sky`).
//
// The F25-prompt Option 3 was to bind LoadMapFile_0x164480 and short-circuit on
// empty $a1. Tried first ג€” confirmed via a temporary `[F25:hit]` stderr
// breadcrumb that LoadMapFile is **never invoked** in the 30 s window. CreateMap
// (its only caller) is not reached either; the upstream LoadMapFromMemory path
// only fires once the title progresses, which is gated by the F24 thread-wait
// blocker. The errors therefore originate from a different chain that
// ultimately reaches `LoadFile2__FPcPvPii_0x149370` (F19: all six errors carry
// the same caller `ra=0x149688`, which is the post-fioOpen RA inside LoadFile2).
//
// Intercept LoadFile2 instead: when the path string contains the empty-stem map
// pattern `map[/\]\.<2-4 alpha ext>`, return 0 without calling the real
// LoadFile2. All other paths delegate to the original recomp via the C symbol,
// so menu / archive / cfg / .img / .snd / .pac opens still work and F6's
// DATA.DAT archive serving stays intact.
static void loadfile2_skip_empty_map_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t pathPtr = getRegU32(ctx, 4); // $a0 = path char*

    // PHASE F39: env-gated map-entry probe ג€” log every LoadFile2 call with
    // path stem, caller RA, mode, and the four arg pointers. Bounded to 64
    // calls so a long boot can't flood. Off in default smoke.
    if (f39_trace_map_entry_enabled())
    {
        static uint32_t f39_calls = 0u;
        if (f39_calls < 64u)
        {
            ++f39_calls;
            char pathBuf[80];
            std::memset(pathBuf, 0, sizeof(pathBuf));
            if (pathPtr != 0u)
            {
                const char *src = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathPtr));
                if (src)
                {
                    for (size_t i = 0; i < sizeof(pathBuf) - 1 && src[i] != 0; ++i)
                    {
                        const char c = src[i];
                        pathBuf[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
                    }
                }
            }
            // F50.5: when LoadFile2 is reached via LoadFile__FPcPvPi (ra=0x149338),
            // LoadFile saved its own caller's ra at its frame +0x10. At this point
            // ctx->sp is still LoadFile's sp (the stub replaced LoadFile2 wholesale,
            // before its prologue), so LoadFile's caller = *(sp+0x10). This pins who
            // issues the fatal empty-name load.
            const uint32_t lfRa = getRegU32(ctx, 31);
            uint32_t callerRa = 0u;
            if (lfRa == 0x149338u)
            {
                callerRa = dc2_read_u32(rdram, getRegU32(ctx, 29) + 0x10u);
            }
            std::fprintf(stderr,
                         "[F39:lf2] n=%u path='%s' a0=0x%x a1=0x%x a2=0x%x a3=%u ra=0x%x callerRa=0x%x gp=0x%x\n",
                         f39_calls, pathBuf,
                         pathPtr,
                         getRegU32(ctx, 5),
                         getRegU32(ctx, 6),
                         getRegU32(ctx, 7),
                         lfRa, callerRa,
                         getRegU32(ctx, 28));
        }
    }

    // F50.5: a fully-empty filename "" reaching LoadFile2 *via* LoadFile__FPcPvPi
    // (ra=0x149338) is fatal ג€” LoadFile aborts the whole game (`func 0x118fb0(0)`
    // -> _Exit) on any failed load. The empty name comes from SetupMainUnit@0x1E8F30
    // loading a character's equipped-item model: it calls GetItemFilePath__Fii on
    // an item id read from the user/character data (e.g. *(short)(charaData+0x322),
    // the main weapon), and GetItemFilePath returns "" when that id has no
    // CGameData entry. For our forced new-game path the equipment ids are not
    // populated, so the *unconditional* equipment loads (+0x322, +0x24a ג€” note the
    // game itself guards the +0x172/+0x1de loads with `if (id > 0)`) request empty
    // names. An empty path can never name a real file, so aborting is the wrong
    // behaviour for the headless port: treat an empty-name LoadFile as a no-op
    // success (return 1) so the load is skipped and InitDungeonMain proceeds. Only
    // applied on the LoadFile path (ra=0x149338), where 0 is fatal; direct
    // LoadFile2 callers that handle their own failure fall through unchanged.
    {
        bool emptyName = (pathPtr == 0u);
        if (!emptyName)
        {
            const char *p = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathPtr));
            emptyName = (p == nullptr) || (p[0] == '\0');
        }
        if (emptyName && getRegU32(ctx, 31) == 0x149338u)
        {
            setReturnU32(ctx, 1u); // non-zero => LoadFile treats as success, no abort
            ctx->pc = getRegU32(ctx, 31);
            return;
        }
    }

    if (pathPtr != 0u)
    {
        const char *p = reinterpret_cast<const char *>(getConstMemPtr(rdram, pathPtr));
        if (p)
        {
            // Bounded scan for the empty-stem map pattern. The boot path
            // produces "map//.ext" (double separator + dot) because the guest
            // sprintf template inserts the empty stem between two separators.
            // Accept any number of slashes/backslashes between "map" and ".".
            for (size_t i = 0; i < 252; ++i)
            {
                const char c0 = p[i];
                if (c0 == 0) break;
                if (c0 != 'm' && c0 != 'M') continue;
                const char c1 = p[i + 1];
                if (c1 != 'a' && c1 != 'A') continue;
                const char c2 = p[i + 2];
                if (c2 != 'p' && c2 != 'P') continue;
                size_t j = i + 3;
                if (p[j] != '/' && p[j] != '\\') continue;
                while (p[j] == '/' || p[j] == '\\') ++j;
                if (p[j] != '.') continue;
                const char e1 = p[j + 1];
                const bool extAlpha = (e1 >= 'a' && e1 <= 'z') || (e1 >= 'A' && e1 <= 'Z');
                if (!extAlpha) continue;
                // Matched empty-stem "map" + seps + ".ext". Skip the open
                // silently ג€” F19 established the misses are non-fatal (game
                // falls through to def.sky and proceeds to the title screen).
                setReturnU32(ctx, 0u);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
        }
    }

    LoadFile2__FPcPvPii_0x149370(rdram, ctx, runtime);
}

// =====================================================================================
// PHASE G15 ג€” deform-mesh build-path probes (env DC2_TRACE_G15, quiet/off by default).
// Pass-through wrappers that measure mgVif1Packet write-pointer growth per call, to
// localize where the costume/character model geometry stops being built in the runner.
// On real HW (PCSX2, costume list) mgDrawDirect@0x142fd0 is called repeatedly and the
// packet grows to ~0x2c20 bytes; the runner's front-end packet only reaches ~0x4f0.
// =====================================================================================
static bool g15_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G15");
    return enabled;
}

static std::atomic<uint32_t> g_g15_actor{0};
static std::atomic<uint32_t> g_g15_char{0};
static std::atomic<uint32_t> g_g15_mgdd{0};
static std::atomic<uint64_t> g_g15_mgdd_bytes{0};

static void g15_actor_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pkt = dc2_read_u32(rdram, gp - 0x788Cu); // mgVif1Packet
    const uint32_t before = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t self = getRegU32(ctx, 4);
    // G18: dump the CCharacter2::DrawDirect gate fields (+0x48 FarClip / +0x6c CheckDraw both
    // hinge on object[+0x64]; either returning 0 skips the whole mesh emit -> delta=0).
    const uint32_t f50 = self ? dc2_read_u32(rdram, self + 0x50u) : 0u;
    const uint32_t f54 = self ? dc2_read_u32(rdram, self + 0x54u) : 0u;
    const uint32_t f58 = self ? dc2_read_u32(rdram, self + 0x58u) : 0u;
    const uint32_t f60 = self ? dc2_read_u32(rdram, self + 0x60u) : 0u;
    const uint32_t f64 = self ? dc2_read_u32(rdram, self + 0x64u) : 0u;
    const uint32_t f68 = self ? dc2_read_u32(rdram, self + 0x68u) : 0u;
    const uint32_t f70 = self ? dc2_read_u32(rdram, self + 0x70u) : 0u;
    DrawDirect__12CActionCharaFv_0x16b940(rdram, ctx, runtime);
    const uint32_t after = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t n = g_g15_actor.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 24u || (n % 120u) == 0u)
        std::fprintf(stderr, "[G15:actorDraw] n=%u this=0x%x pkt=0x%x before=0x%x after=0x%x delta=0x%x"
                     " f50=0x%x f54=0x%x f58=0x%x f60=0x%x f64=0x%x f68=0x%x f70=0x%x\n",
                     n, self, pkt, before, after, after - before,
                     f50, f54, f58, f60, f64, f68, f70);
}

// =====================================================================================
// PHASE G226 -- CDynamicAnime (physics-driven accessory, e.g. Max's chest pendant) probe.
// Dumps the CCharacter2-chain DA fields (+0x120 flags, +0x12c count, +0x130 array) for the
// CActionChara head and every +0x678 linked part, before the raw DrawDirect body runs, to
// localize whether the pendant's dyn-anime data is loaded/populated in the runner at all.
// Ground truth (live PCSX2, empty-room repro): CCharacter2 flags120=0x0 daCount=1
// daArray!=0, DA[0] vertexCount=6 drawFrameCount=0. Read-only, no state mutation.
// Env: DC2_TRACE_G226 (quiet/off by default).
// =====================================================================================
static bool g226_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G226");
    return enabled;
}
static std::atomic<uint32_t> g_g226_actor{0};
// PHASE G228 (default-off, DC2_G228_DASCAN=1): dynamic-anime output-frame world-matrix
// degeneracy scanner. G227 refuted every node-level visibility layer for Max's pendant; the
// gem is skinned to a DA-driven bone (DrawSub self-draw is empty -- drawFrameCount=0 both HW
// and runner -- so the gem is body geometry attached to the 3 DA output frames FramePose
// poses each tick). G214 only flags an ALL-ZERO / non-finite world matrix; a FramePose basis
// that collapses to near-ZERO DETERMINANT (physics verts too close -> degenerate orthonormal
// basis) would zero-area the gem's tris while leaving translation intact -- invisible to G214.
// This captures the DA output-frame ptrs (from g226_dump_chain) and, in the g214 GetLWMatrix
// slot, dumps each DA frame's full world matrix + 3x3 basis determinant so a degenerate basis
// can be seen and A/B'd against live PCSX2.
static bool g228_dascan_enabled()
{
    static const bool on = dc2_env_flag_enabled("DC2_G228_DASCAN");
    return on;
}
static std::atomic<uint32_t> g_g228_daFrames[24];
static std::atomic<int> g_g228_daFrameCount{0};
static std::atomic<uint64_t> g_g228_logged{0};
static void g228_register_da_frame(uint32_t frm)
{
    if (frm < 0x80000u || frm >= 0x2000000u)
        return;
    int cnt = g_g228_daFrameCount.load(std::memory_order_relaxed);
    for (int i = 0; i < cnt; ++i)
        if (g_g228_daFrames[i].load(std::memory_order_relaxed) == frm)
            return;
    if (cnt < 24)
    {
        g_g228_daFrames[cnt].store(frm, std::memory_order_relaxed);
        g_g228_daFrameCount.store(cnt + 1, std::memory_order_relaxed);
    }
}
static bool g228_is_da_frame(uint32_t frm)
{
    int cnt = g_g228_daFrameCount.load(std::memory_order_relaxed);
    for (int i = 0; i < cnt; ++i)
        if (g_g228_daFrames[i].load(std::memory_order_relaxed) == frm)
            return true;
    return false;
}

// PHASE G229 (default-off, DC2_G229_DATREE=1): map every DA output frame's mgCFrame
// subtree and correlate visual-owning descendants with GetLWMatrix.  The authoritative
// mgCFrame layout is: +0x50 name, +0x54 parent, +0x58 first child, +0x5c next sibling,
// +0xf8 visual.  This closes G228's load-bearing unknown: which actual mesh is attached
// to the physics bones, and whether that mesh's own world matrix is ever fetched.
static bool g229_datree_enabled()
{
    static const bool on = dc2_env_flag_enabled("DC2_G229_DATREE");
    return on;
}
struct G229FrameSlot
{
    std::atomic<uint32_t> frame{0};
    std::atomic<uint32_t> root{0};
    std::atomic<uint32_t> depth{0};
    std::atomic<uint32_t> visual{0};
    std::atomic<uint64_t> lwCalls{0};
};
static G229FrameSlot g_g229_frames[256];
static std::atomic<uint64_t> g_g229_logged{0};
// G230: the visual pointer for the "atoramiria--m1-"/"miria" pendant mesh, latched once its
// name is resolved by the DA-tree walk below. Lets a later per-frame Draw__12mgCVisualMDT probe
// scope its logging to exactly this visual without re-walking the frame hierarchy every call.
static std::atomic<uint32_t> g_g230_pendant_visual{0};

static bool g229_valid_guest_ptr(uint32_t p)
{
    return p >= 0x80000u && p < 0x2000000u;
}

static void g229_frame_name(uint8_t *rdram, uint32_t frame, char (&out)[64])
{
    out[0] = '\0';
    const uint32_t name = g229_valid_guest_ptr(frame)
        ? dc2_read_u32(rdram, frame + 0x50u) : 0u;
    if (!g229_valid_guest_ptr(name))
        return;
    for (size_t i = 0; i + 1u < sizeof(out); ++i)
    {
        const char c = static_cast<char>(rdram[(name + static_cast<uint32_t>(i)) & 0x01FFFFFFu]);
        out[i] = c;
        out[i + 1u] = '\0';
        if (c == '\0')
            break;
    }
}

// PHASE G230 (default-off, DC2_G229_DATREE=1 shares the gate): dump the mgCVisualMDT material
// table for a newly-registered DA-tree frame -- count @+0x40, array @+0x44, stride 0x30 (two
// vec4 colors @+0x00/+0x10, resolved mgCTexture* @+0x20). Correlates each material with its
// TEX0 (tbp/tbw/psm/cbp/cpsm, mgCTexture+0x38) so the pendant's exact material/page identity can
// be matched byte-for-byte against a live-PCSX2 read of the same struct (see
// tools/run_g230_pendant_material.ps1), instead of comparing whole-dump GS totals.
static void g230_dump_materials(uint8_t *rdram, uint32_t frame, uint32_t visual)
{
    if (!g229_valid_guest_ptr(visual))
        return;
    const uint32_t matCount = dc2_read_u32(rdram, visual + 0x40u);
    const uint32_t matArray = dc2_read_u32(rdram, visual + 0x44u);
    if (!g229_valid_guest_ptr(matArray) || matCount == 0u || matCount >= 32u)
        return;
    for (uint32_t m = 0; m < matCount; ++m)
    {
        const uint32_t matOff = matArray + m * 0x30u;
        const float c0x = dc2_read_f32(rdram, matOff + 0x00u);
        const float c0y = dc2_read_f32(rdram, matOff + 0x04u);
        const float c0z = dc2_read_f32(rdram, matOff + 0x08u);
        const float c0w = dc2_read_f32(rdram, matOff + 0x0Cu);
        const float c1x = dc2_read_f32(rdram, matOff + 0x10u);
        const float c1y = dc2_read_f32(rdram, matOff + 0x14u);
        const float c1z = dc2_read_f32(rdram, matOff + 0x18u);
        const float c1w = dc2_read_f32(rdram, matOff + 0x1Cu);
        const uint32_t texPtr = dc2_read_u32(rdram, matOff + 0x20u);
        uint32_t tbp = 0, tbw = 0, psm = 0, cbp = 0, cpsm = 0;
        uint64_t tex0 = 0;
        if (g229_valid_guest_ptr(texPtr))
        {
            tex0 = dc2_read_u64(rdram, texPtr + 0x38u);
            f51_decode_tex0(tex0, &tbp, &tbw, &psm, &cbp, &cpsm);
        }
        std::fprintf(stderr,
                     "[G230:mat] frame=0x%08x visual=0x%08x mat=%u@0x%08x c0=(%g,%g,%g,%g) "
                     "c1=(%g,%g,%g,%g) texPtr=0x%08x tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x "
                     "cbp=0x%x cpsm=0x%x\n",
                     frame, visual, m, matOff,
                     (double)c0x, (double)c0y, (double)c0z, (double)c0w,
                     (double)c1x, (double)c1y, (double)c1z, (double)c1w,
                     texPtr, (unsigned long long)tex0, tbp, tbw, psm, cbp, cpsm);
    }
}

// PHASE G230b (same DC2_G229_DATREE gate): walk the mgCVisualMDT per-material face-group
// bucket list (visual+0x48 -> linked list of {materialIndex, mgCFace* head, next} 0x20-byte
// buckets; each mgCFace is {flags16, vertsPerFace16, materialIndex16, totalIndexCount16,
// faceCount16, pad16, indexArray32, nextFace32} per CreateFace__12mgCVisualMDTFP8FACES_ID
// (0x0013F010) decompilation). This is the load-time-built per-material geometry the draw
// path walks -- if the pendant's gem material (index 0, tbp=0x34e0) has an empty/absent
// bucket while the frame material (index 4, same page) does not, that pins the defect to
// geometry assignment rather than texture/material binding (already proven identical to
// live PCSX2 by g230_dump_materials).
static uint16_t g230_read_u16(uint8_t *rdram, uint32_t addr)
{
    return static_cast<uint16_t>(dc2_read_u32(rdram, addr) & 0xFFFFu);
}
static void g230_dump_face_buckets(uint8_t *rdram, uint32_t frame, uint32_t visual)
{
    if (!g229_valid_guest_ptr(visual))
        return;
    uint32_t bucket = dc2_read_u32(rdram, visual + 0x48u);
    for (uint32_t guard = 0; guard < 32u && g229_valid_guest_ptr(bucket); ++guard)
    {
        const uint32_t matIdx = dc2_read_u32(rdram, bucket + 0x00u);
        const uint32_t faceHead = dc2_read_u32(rdram, bucket + 0x04u);
        const uint32_t nextBucket = dc2_read_u32(rdram, bucket + 0x08u);
        uint32_t face = faceHead;
        uint32_t faceGuard = 0;
        uint32_t totalFaces = 0, totalIdx = 0;
        for (; faceGuard < 64u && g229_valid_guest_ptr(face); ++faceGuard)
        {
            const uint16_t flags = g230_read_u16(rdram, face + 0x00u);
            const uint16_t vertsPerFace = g230_read_u16(rdram, face + 0x02u);
            const uint16_t faceMatIdx = g230_read_u16(rdram, face + 0x04u);
            const uint16_t totalIdxCount = g230_read_u16(rdram, face + 0x06u);
            const uint16_t faceCount = g230_read_u16(rdram, face + 0x08u);
            const uint32_t idxArray = dc2_read_u32(rdram, face + 0x0Cu);
            const uint32_t nextFace = dc2_read_u32(rdram, face + 0x10u);
            std::fprintf(stderr,
                         "[G230:face] frame=0x%08x visual=0x%08x bucket=0x%08x matIdx=%u "
                         "face=0x%08x flags=0x%x vertsPerFace=%d faceMatIdx=%d totalIdxCount=%d "
                         "faceCount=%d idxArray=0x%08x next=0x%08x\n",
                         frame, visual, bucket, matIdx, face, flags,
                         (int16_t)vertsPerFace, (int16_t)faceMatIdx, (int16_t)totalIdxCount,
                         (int16_t)faceCount, idxArray, nextFace);
            totalFaces += (uint32_t)(int16_t)faceCount;
            totalIdx += (uint32_t)(int16_t)totalIdxCount;
            face = nextFace;
        }
        std::fprintf(stderr,
                     "[G230:bucket] frame=0x%08x visual=0x%08x bucket=0x%08x matIdx=%u "
                     "faceHead=0x%08x next=0x%08x faceLinks=%u totalFaces=%u totalIdx=%u\n",
                     frame, visual, bucket, matIdx, faceHead, nextBucket, faceGuard,
                     totalFaces, totalIdx);
        bucket = nextBucket;
    }
}

// PHASE G230c (same DC2_G229_DATREE gate): sample the per-material bucket's PACKET fields
// (+0x10 packet-start ptr, +0x14 quadword count -- written by
// CreatePacket__12mgCVisualMDTFP14mgCDrawManager@0x13F6A0, read back by
// Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager@0x13F4E0's own AddPacket loop) AFTER a real
// draw call for the latched pendant visual. g230_dump_face_buckets already proved the load-time
// FACE DATA for material 0 (the gem, tbp=0x34e0) is present and non-trivial (72 faces); this
// checks whether the SOLID-FILL packet-build step (CreatePacket, invoked once per draw from
// COutLineDraw's per-node chain) actually produced a nonzero-quadword GS packet for that same
// bucket, or silently emits qwc=0 for it while other materials get real packets.
static std::atomic<uint32_t> g_g230_mdt_calls{0};
static void g230_visualmdt_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(rdram, ctx, runtime);
    const uint32_t target = g_g230_pendant_visual.load(std::memory_order_relaxed);
    if (!g229_valid_guest_ptr(target) || self != target)
        return;
    const uint32_t n = g_g230_mdt_calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n > 6u) // static geometry -- a few post-warm-up samples are enough
        return;
    uint32_t bucket = dc2_read_u32(rdram, self + 0x48u);
    for (uint32_t guard = 0; guard < 32u && g229_valid_guest_ptr(bucket); ++guard)
    {
        const uint32_t matIdx = dc2_read_u32(rdram, bucket + 0x00u);
        const uint32_t nextBucket = dc2_read_u32(rdram, bucket + 0x08u);
        const uint32_t packetPtr = dc2_read_u32(rdram, bucket + 0x10u);
        const uint32_t qwc = dc2_read_u32(rdram, bucket + 0x14u);
        std::fprintf(stderr,
                     "[G230:pkt] n=%u self=0x%08x bucket=0x%08x matIdx=%u packetPtr=0x%08x qwc=%u\n",
                     n, self, bucket, matIdx, packetPtr, qwc);
        bucket = nextBucket;
    }
}

static int g229_register_frame(uint8_t *rdram, uint32_t frame, uint32_t root, uint32_t depth)
{
    if (!g229_valid_guest_ptr(frame))
        return -1;
    for (int i = 0; i < 256; ++i)
    {
        uint32_t cur = g_g229_frames[i].frame.load(std::memory_order_relaxed);
        if (cur == frame)
        {
            // Output frames can nest (atora is below himo_hone).  The later walk is the
            // more-specific DA root, so refresh the classification without duplicating it.
            g_g229_frames[i].root.store(root, std::memory_order_relaxed);
            g_g229_frames[i].depth.store(depth, std::memory_order_relaxed);
            return i;
        }
        if (cur == 0u && g_g229_frames[i].frame.compare_exchange_strong(
                cur, frame, std::memory_order_relaxed))
        {
            const uint32_t visual = dc2_read_u32(rdram, frame + 0xF8u);
            g_g229_frames[i].root.store(root, std::memory_order_relaxed);
            g_g229_frames[i].depth.store(depth, std::memory_order_relaxed);
            g_g229_frames[i].visual.store(visual, std::memory_order_relaxed);
            char name[64];
            g229_frame_name(rdram, frame, name);
            std::fprintf(stderr,
                         "[G229:tree] frame=0x%08x root=0x%08x depth=%u name=%s "
                         "parent=0x%08x child=0x%08x sibling=0x%08x visual=0x%08x\n",
                         frame, root, depth, name[0] ? name : "<none>",
                         dc2_read_u32(rdram, frame + 0x54u),
                         dc2_read_u32(rdram, frame + 0x58u),
                         dc2_read_u32(rdram, frame + 0x5Cu), visual);
            g230_dump_materials(rdram, frame, visual);
            g230_dump_face_buckets(rdram, frame, visual);
            if (g229_valid_guest_ptr(visual) && std::strstr(name, "miria") != nullptr)
                g_g230_pendant_visual.store(visual, std::memory_order_relaxed);
            return i;
        }
    }
    return -1;
}

static void g229_register_tree(uint8_t *rdram, uint32_t frame, uint32_t root,
                               uint32_t depth, uint32_t &budget)
{
    if (!g229_valid_guest_ptr(frame) || depth > 32u || budget >= 256u)
        return;
    ++budget;
    g229_register_frame(rdram, frame, root, depth);
    uint32_t child = dc2_read_u32(rdram, frame + 0x58u);
    for (uint32_t siblings = 0; g229_valid_guest_ptr(child) && siblings < 64u; ++siblings)
    {
        g229_register_tree(rdram, child, root, depth + 1u, budget);
        child = dc2_read_u32(rdram, child + 0x5Cu);
    }
}

static int g229_frame_slot(uint32_t frame)
{
    for (int i = 0; i < 256; ++i)
        if (g_g229_frames[i].frame.load(std::memory_order_relaxed) == frame)
            return i;
    return -1;
}

static void g226_dump_chain(uint8_t *rdram, uint32_t self, uint32_t n)
{
    int part = 0;
    while (self != 0 && part < 8)
    {
        const uint32_t flags120 = dc2_read_u32(rdram, self + 0x120u);
        const uint32_t daCount  = dc2_read_u32(rdram, self + 0x12Cu);
        const uint32_t daArray  = dc2_read_u32(rdram, self + 0x130u);
        // G227: +0x124 is the COutLineDraw-group head ptr DrawDirect__11CCharacter2Fv branches
        // on -- if null, the whole per-part COutLineDraw loop is skipped and the character is
        // drawn via a single direct mgDrawDirect(mainFrame) call instead (no per-node census).
        const uint32_t outlineGroup = dc2_read_u32(rdram, self + 0x124u);
        std::fprintf(stderr, "[G226:chain] n=%u part=%d this=0x%x flags120=0x%x daCount=%u daArray=0x%x outlineGroup(+0x124)=0x%x\n",
                     n, part, self, flags120, daCount, daArray, outlineGroup);
        if (daArray != 0 && daCount > 0 && daCount < 64)
        {
            for (uint32_t i = 0; i < daCount; ++i)
            {
                const uint32_t obj = daArray + i * 0x90u;
                const uint32_t vcount = dc2_read_u32(rdram, obj + 0x10u);
                const uint32_t drawCount = dc2_read_u32(rdram, obj + 0x30u);
                std::fprintf(stderr, "[G226:chain]   DA[%u]@0x%x vertexCount=%u drawFrameCount=%u\n",
                             i, obj, vcount, drawCount);
                // Full 0x90-byte struct dump (hex) so it can be diffed against a live-PCSX2
                // snapshot taken a few frames apart, to see which fields actually evolve
                // (proof of physics stepping) vs stay frozen in the runner.
                char hexbuf[0x90 * 2 + 1];
                for (uint32_t b = 0; b < 0x90u; ++b)
                    std::snprintf(hexbuf + b * 2, 3, "%02x", rdram[(obj + b) & 0x01FFFFFFu]);
                std::fprintf(stderr, "[G226:chain]   DA[%u]hex=%s\n", i, hexbuf);
                // +0x18 = "now position" vertex array ptr (vec4 per vertex, stride 0x10).
                // Dump vertex 0 and the last vertex (likely the pendant tip) to see whether
                // the physics chain is actually moving each tick.
                const uint32_t nowPosPtr = dc2_read_u32(rdram, obj + 0x18u);
                if (nowPosPtr != 0 && vcount > 0)
                {
                    const uint32_t v0 = 0u;
                    const uint32_t vN = vcount - 1u;
                    std::fprintf(stderr,
                        "[G226:chain]   DA[%u] nowPosPtr=0x%x v0=(%.3f,%.3f,%.3f,%.3f) v%u=(%.3f,%.3f,%.3f,%.3f)\n",
                        i, nowPosPtr,
                        dc2_read_f32(rdram, nowPosPtr + v0 * 0x10u + 0u),
                        dc2_read_f32(rdram, nowPosPtr + v0 * 0x10u + 4u),
                        dc2_read_f32(rdram, nowPosPtr + v0 * 0x10u + 8u),
                        dc2_read_f32(rdram, nowPosPtr + v0 * 0x10u + 12u),
                        vN,
                        dc2_read_f32(rdram, nowPosPtr + vN * 0x10u + 0u),
                        dc2_read_f32(rdram, nowPosPtr + vN * 0x10u + 4u),
                        dc2_read_f32(rdram, nowPosPtr + vN * 0x10u + 8u),
                        dc2_read_f32(rdram, nowPosPtr + vN * 0x10u + 12u));
                }
                // +0x04 = output frame count, +0x08 = resolved mgCFrame* table (one per
                // output frame FramePose() writes SetTransMatrix into). A null entry here
                // means the physics runs fine but its output frame was never resolved at
                // load time -- the bone FramePose should drive stays at bind pose/identity.
                const uint32_t frameCount = dc2_read_u32(rdram, obj + 0x04u);
                const uint32_t frameTbl = dc2_read_u32(rdram, obj + 0x08u);
                if (frameTbl != 0 && frameCount > 0 && frameCount < 16)
                {
                    for (uint32_t f = 0; f < frameCount; ++f)
                    {
                        const uint32_t frm = dc2_read_u32(rdram, frameTbl + f * 4u);
                        std::fprintf(stderr, "[G226:chain]   DA[%u] outFrame[%u]=0x%x\n", i, f, frm);
                        g228_register_da_frame(frm); // G228: track for the GetLWMatrix degeneracy scan
                        if (g229_datree_enabled())
                        {
                            uint32_t budget = 0;
                            g229_register_tree(rdram, frm, frm, 0u, budget);
                        }
                    }
                }
            }
        }
        self = dc2_read_u32(rdram, self + 0x678u);
        ++part;
    }
}
// G227: scope flag -- set while inside Max's DrawDirect__12CActionCharaFv call, so the
// per-outline-node census below (g227_node_probe/g227_getdrawrect_probe) can filter out
// every OTHER object's draws (geo-stones, npcs) without needing a self-pointer allowlist.
static std::atomic<int> g_g227_in_max_draw{0};
static void g226_actor_da_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t n = g_g226_actor.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 40u || (n % 240u) == 0u)
    {
        std::fprintf(stderr, "[G226:actorDraw] n=%u this=0x%x (before)\n", n, self);
        g226_dump_chain(rdram, self, n);
    }
    g_g227_in_max_draw.fetch_add(1, std::memory_order_relaxed);
    DrawDirect__12CActionCharaFv_0x16b940(rdram, ctx, runtime);
    g_g227_in_max_draw.fetch_sub(1, std::memory_order_relaxed);
}

// PHASE G227: per-outline-node census (read-only). G226 verified the DA physics data +
// stepping + frame-resolution are correct; G227 refuted the LOD-fade cull gate (f20 always
// 1.0), the GetLWMatrix collapse class (0 collapses/20000+ calls, DC2_G214_LWSCAN), and the
// VIF1 chain-tag truncation class (max tagsProcessed=1748 vs the 16384 cutoff) for this
// scene. What remains: does the pendant's outline-group node ever get built at all, and if
// so does ITS specific mgGetDrawRect call return ret=0 (a per-node AABB-collapse cull that
// is a DIFFERENT check than the world-matrix collapse G214 already scans)? This walks every
// node in CCharacter2+0x124's outline-group linked list each time DrawDirect__11CCharacter2Fv
// runs for Max (scoped via g_g227_in_max_draw), logging each node's identity fields and its
// GetDrawRect ret/box. Env: DC2_TRACE_G227 (shared with the LOD-fade/VIF1-tag probes above).
static bool g212_lwguard_disabled(); // forward decl (defined later; DC2_G212_NO_LWGUARD kill switch)
static std::atomic<uint32_t> g_g227_curNode{0};
static std::atomic<uint32_t> g_g227_nodeCalls{0};
static std::atomic<uint32_t> g_g229_outlineCalls{0};
static void g229_outline_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t node = getRegU32(ctx, 4);
    const uint32_t frame = dc2_read_u32(rdram, node + 0x34u);
    const int slot = g229_frame_slot(frame);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pkt = dc2_read_u32(rdram, gp - 0x788Cu);
    const uint32_t before = g229_valid_guest_ptr(pkt) ? dc2_read_u32(rdram, pkt) : 0u;
    Draw__12COutLineDrawFff_0x17c2d0(rdram, ctx, runtime);
    if (slot < 0)
        return;
    const uint32_t after = g229_valid_guest_ptr(pkt) ? dc2_read_u32(rdram, pkt) : before;
    const uint32_t visual = dc2_read_u32(rdram, frame + 0xF8u);
    // The cached world matrix at +0x70 is exactly what GetLWMatrixTopBottom refreshes for
    // rigid visuals before their vtable Draw call.  Read it after the outline body returns.
    auto rf = [&](uint32_t off) {
        const uint32_t u = dc2_read_u32(rdram, frame + 0x70u + off);
        float f; std::memcpy(&f, &u, sizeof(f)); return f;
    };
    const float r0x = rf(0x00u), r0y = rf(0x04u), r0z = rf(0x08u);
    const float r1x = rf(0x10u), r1y = rf(0x14u), r1z = rf(0x18u);
    const float r2x = rf(0x20u), r2y = rf(0x24u), r2z = rf(0x28u);
    const float tx = rf(0x30u), ty = rf(0x34u), tz = rf(0x38u);
    const float det = r0x * (r1y * r2z - r1z * r2y)
                    - r0y * (r1x * r2z - r1z * r2x)
                    + r0z * (r1x * r2y - r1y * r2x);
    const uint32_t n = g_g229_outlineCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 400u || (n % 120u) == 0u)
    {
        char name[64];
        g229_frame_name(rdram, frame, name);
        std::fprintf(stderr,
                     "[G229:outline] n=%u tick=%llu node=0x%08x frame=0x%08x name=%s "
                     "visual=0x%08x dirty=%u pkt=0x%08x before=0x%08x after=0x%08x "
                     "delta=0x%x ret=0x%x det=%g t=(%g,%g,%g)\n",
                     n, (unsigned long long)g_dc2PresentTick.load(std::memory_order_relaxed),
                     node, frame, name[0] ? name : "<none>", visual,
                     dc2_read_u32(rdram, frame + 0x40u), pkt, before, after, after - before,
                     getRegU32(ctx, 2), (double)det, (double)tx, (double)ty, (double)tz);
    }
}
// PHASE G233 (default-off, DC2_G233_COLPROBE=1): sample CDAColPipe::CheckHit@0x17C090 — the
// torso collider push-out the DA pendant chain relies on to stay in FRONT of the body (G232
// proved the chain settles BEHIND). Indirectly dispatched from Step__13CDynamicAnime's
// collision loop (vtable+0x8 at obj+0xC0), so registerFunction hooks it. Splits: never
// called / called-but-never-hits / hits-but-pushes-the-wrong-way. Direct body call is
// preempt-suppressed (G212 rule). Extern decl at file top (global scope).
static std::atomic<uint32_t> g_g233_calls{0}, g_g233_hits{0};
static void g233_colpipe_checkhit_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t obj = getRegU32(ctx, 4);
    const uint32_t vertPtr = getRegU32(ctx, 5);
    auto rdf = [&](uint32_t addr) {
        const uint32_t u = dc2_read_u32(rdram, addr);
        float f; std::memcpy(&f, &u, sizeof(f)); return f;
    };
    const float ix = rdf(vertPtr), iy = rdf(vertPtr + 4), iz = rdf(vertPtr + 8);
    g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    CheckHit__10CDAColPipeFPf_0x17c090(rdram, ctx, runtime);
    g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    const uint32_t ret = getRegU32(ctx, 2);
    const float ox = rdf(vertPtr), oy = rdf(vertPtr + 4), oz = rdf(vertPtr + 8);
    const uint32_t n = g_g233_calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (ret != 0u) g_g233_hits.fetch_add(1u, std::memory_order_relaxed);
    if (n <= 40u || (n % 500u) == 0u)
        std::fprintf(stderr,
                     "[G233:checkhit] n=%u hits=%u obj=0x%08x ret=%u in=(%.3f,%.3f,%.3f) "
                     "out=(%.3f,%.3f,%.3f) d=(%.3f,%.3f,%.3f) r=%.3f\n",
                     n, g_g233_hits.load(std::memory_order_relaxed), obj, ret,
                     ix, iy, iz, ox, oy, oz, ox - ix, oy - iy, oz - iz,
                     rdf(obj + 4));
}
static void g227_node_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t node = getRegU32(ctx, 4); // a0 = param_3 (the COutLineDraw node ptr)
    const uint32_t v30 = dc2_read_u32(rdram, node + 0x30u);
    const uint32_t v34 = dc2_read_u32(rdram, node + 0x34u);
    uint32_t v38b = dc2_read_u32(rdram, node + 0x38u);
    float v38; std::memcpy(&v38, &v38b, sizeof(v38));
    const uint32_t v3c = dc2_read_u32(rdram, node + 0x3Cu);
    const uint32_t v60 = dc2_read_u32(rdram, node + 0x60u);
    const uint32_t v64 = dc2_read_u32(rdram, node + 0x64u);
    g_g227_curNode.store(node, std::memory_order_relaxed);
    Draw__12COutLineDrawFff_0x17c2d0(rdram, ctx, runtime);
    g_g227_curNode.store(0, std::memory_order_relaxed);
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t n = g_g227_nodeCalls.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 400u || (n % 60u) == 0u)
        std::fprintf(stderr,
                     "[G227:node] n=%u node=0x%x +0x30=0x%x +0x34(frame)=0x%x +0x38(outlineW)=%.4f +0x3c=0x%x +0x60=0x%x +0x64=0x%x ret=%u\n",
                     n, node, v30, v34, static_cast<double>(v38), v3c, v60, v64, ret);
}
static void g227_getdrawrect_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t frame = getRegU32(ctx, 4); // capture BEFORE the call -- a0/a1 are not
    const uint32_t box = getRegU32(ctx, 5);   // preserved by the callee's own body.
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    mgGetDrawRect__FP8mgCFrameP9mgVu0FBOX_0x143160(rdram, ctx, runtime);
    if (!g212_lwguard_disabled())
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    const uint32_t retVal = getRegU32(ctx, 2);
    const uint32_t node = g_g227_curNode.load(std::memory_order_relaxed);
    float mx = 0, my = 0, nx = 0, ny = 0;
    if (box > 0x80000u && box < 0x2000000u)
    {
        auto rf = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, box + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
        mx = rf(0x00u); my = rf(0x04u); nx = rf(0x10u); ny = rf(0x14u);
    }
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 400u || (n % 60u) == 0u)
        std::fprintf(stderr,
                     "[G227:drawrect] n=%u node=0x%x frame=0x%x ret=%u box=(x:%.1f..%.1f y:%.1f..%.1f)\n",
                     n, node, frame, retVal, static_cast<double>(nx), static_cast<double>(mx),
                     static_cast<double>(ny), static_cast<double>(my));
}

// PHASE G205: is Draw__12CActionCharaFv (0x16b850, the non-Direct field-character group
// draw, walks the +0x678 linked list calling Draw__11CCharacter2Fv) even reached on the
// town route? DrawDirect__11CCharacter2Fv (costume path, G204's assumption) and
// Draw__11CCharacter2Fv both traced 0 hits at their raw-body entry on MAP-4 -- this probe
// checks the level above. Gated on DC2_G205_FVAR13_TRACE (shared with the recomp-file
// raw-body traces added this phase; see phase-G205-fix-log.md).
static bool g205_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G205_FVAR13_TRACE");
    return enabled;
}
static std::atomic<uint32_t> g_g205_actionchara{0};
static void g205_actionchara_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t n = g_g205_actionchara.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 40u || (n % 240u) == 0u)
        std::fprintf(stderr, "[G205:actionCharaDraw] n=%u this=0x%x\n", n, self);
    Draw__12CActionCharaFv_0x16b850(rdram, ctx, runtime);
}

static void g15_char_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pkt = dc2_read_u32(rdram, gp - 0x788Cu);
    const uint32_t before = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t self = getRegU32(ctx, 4);
    DrawDirect__11CCharacter2Fv_0x1731f0(rdram, ctx, runtime);
    const uint32_t after = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t n = g_g15_char.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 24u || (n % 120u) == 0u)
        std::fprintf(stderr, "[G15:charDraw] n=%u this=0x%x pkt=0x%x before=0x%x after=0x%x delta=0x%x\n",
                     n, self, pkt, before, after, after - before);
}

static void g15_mgdrawdirect_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pkt = dc2_read_u32(rdram, gp - 0x788Cu);
    const uint32_t before = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t frame = getRegU32(ctx, 4);
    // Draw__8mgCFrame@0x137e10 gates its build on frame+0xF8 (beqz -> skip). Dump the
    // mesh-data fields so we can tell "field not populated" from "build path broken".
    const uint32_t f0 = frame ? dc2_read_u32(rdram, frame + 0xF0u) : 0u;
    const uint32_t f4 = frame ? dc2_read_u32(rdram, frame + 0xF4u) : 0u;
    const uint32_t f8 = frame ? dc2_read_u32(rdram, frame + 0xF8u) : 0u;
    const uint32_t c58 = frame ? dc2_read_u32(rdram, frame + 0x58u) : 0u;
    mgDrawDirect__FP8mgCFrame_0x142fd0(rdram, ctx, runtime);
    const uint32_t after = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t delta = after - before;
    g_g15_mgdd_bytes.fetch_add(delta, std::memory_order_relaxed);
    const uint32_t n = g_g15_mgdd.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 40u || (n % 200u) == 0u)
        std::fprintf(stderr, "[G15:mgDrawDirect] n=%u frame=0x%x +0xF0=0x%x +0xF4=0x%x +0xF8=0x%x +0x58=0x%x delta=0x%x total=0x%llx\n",
                     n, frame, f0, f4, f8, c58, delta,
                     (unsigned long long)g_g15_mgdd_bytes.load(std::memory_order_relaxed));
}

static std::atomic<uint32_t> g_g15_clip{0};
static std::atomic<uint32_t> g_g15_clip_zero{0};

// G15: mgClipBoxW is the frustum-cull gate inside Draw__8mgCFrame (jal @0x137eec ->
// beqz $v0,+0x84). Log return value only for calls made FROM Draw__8mgCFrame (ra in its
// range) so we can tell whether the costume model is being culled (ret==0 => mesh skipped).
static void g15_clipbox_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const bool fromFrameDraw = (ra >= 0x00137E10u && ra <= 0x00138190u);
    const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5),
                   a2 = getRegU32(ctx, 6), a3 = getRegU32(ctx, 7);
    mgClipBoxW__FPfPfPfPf_0x12f2e0(rdram, ctx, runtime);
    if (!fromFrameDraw)
        return;
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t n = g_g15_clip.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (ret == 0u)
        g_g15_clip_zero.fetch_add(1u, std::memory_order_relaxed);
    if (n <= 40u || (n % 200u) == 0u)
        std::fprintf(stderr, "[G15:clipBoxW] n=%u ra=0x%x ret=%u zeroCount=%u a=(0x%x,0x%x,0x%x,0x%x)\n",
                     n, ra, ret, g_g15_clip_zero.load(std::memory_order_relaxed), a0, a1, a2, a3);
}

// G18: Draw__8mgCFrame@0x137e10 (frame vtable+0x44) returns the qword count mgDrawDirect
// reserves. delta=0 means this returns 0. Early geometry gate (decompiled 0x137e10:49243):
//   iVar4=*(frame+0xf4); if (iVar4 && (*(iVar4+0x18)&1) && *(frame+0xf8)) { ...emit... }
// Capture the gate inputs + return value to pin which condition fails for the model frame.
static std::atomic<uint32_t> g_g18_frame{0};
static void g18_frame_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t frame = getRegU32(ctx, 4);
    const uint32_t f0 = frame ? dc2_read_u32(rdram, frame + 0xF0u) : 0u;
    const uint32_t f4 = frame ? dc2_read_u32(rdram, frame + 0xF4u) : 0u;
    const uint32_t f8 = frame ? dc2_read_u32(rdram, frame + 0xF8u) : 0u;
    const uint32_t desc18 = f4 ? dc2_read_u32(rdram, f4 + 0x18u) : 0u;
    const uint32_t desc40 = f4 ? dc2_read_u32(rdram, f4 + 0x40u) : 0u;
    const uint32_t desc48 = f4 ? dc2_read_u32(rdram, f4 + 0x48u) : 0u; // 0 => clip-test branch (49244); !=0 => direct emit
    Draw__8mgCFrameFPUi_0x137e10(rdram, ctx, runtime);
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t n = g_g18_frame.fetch_add(1u, std::memory_order_relaxed) + 1u;
    // Log first 60, plus any call with f4!=0 returning 0 (the suspected model frame bail).
    if (n <= 60u || (f4 != 0u && ret == 0u))
        std::fprintf(stderr, "[G18:frameDraw] n=%u frame=0x%x f0=0x%x f4=0x%x desc+0x18=0x%x desc+0x40=0x%x desc+0x48=0x%x f8=0x%x ret=0x%x\n",
                     n, frame, f0, f4, desc18, desc40, desc48, f8, ret);
}

static std::atomic<uint32_t> g_g15_mdt{0};
static std::atomic<uint32_t> g_g15_svp{0};
static std::atomic<uint32_t> g_g15_svp_skip{0};

// G15: Draw__12mgCVisualMDT is the deform-mesh emit (loops mgSendVuProg + AddPacket).
static void g15_mdt_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pkt = dc2_read_u32(rdram, gp - 0x788Cu);
    const uint32_t before = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);   // param_2 (build buffer; 0 => AddPacket deferred path)
    const uint32_t ra = getRegU32(ctx, 31);  // caller: in 0x137e10..0x13815c => reached via Draw__8mgCFrame:49323
    Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(rdram, ctx, runtime);
    const uint32_t after = pkt ? dc2_read_u32(rdram, pkt) : 0u;
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t n = g_g15_mdt.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool fromFrameDraw = (ra >= 0x00137E10u && ra <= 0x0013815Cu);
    if (n <= 24u || (n % 200u) == 0u)
        std::fprintf(stderr, "[G15:visualMDT] n=%u mesh=0x%x param2=0x%x ra=0x%x fromFrameDraw=%u ret=0x%x pktDelta=0x%x\n",
                     n, a0, a1, ra, fromFrameDraw ? 1u : 0u, ret, after - before);
}

// G15: mgSendVuProg appends a DMA-call to the VU microprogram packet, UNLESS the requested
// program already equals the cached resident program at gp-0x7FE0 (then it early-returns,
// writing nothing). A stale resident-cache would suppress all MPG/MSCAL after the first.
static void g15_sendvuprog_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
    const uint32_t cacheBefore = dc2_read_u32(rdram, gp - 0x7FE0u);
    mgSendVuProg__FPUii_0x145e80(rdram, ctx, runtime);
    const uint32_t cacheAfter = dc2_read_u32(rdram, gp - 0x7FE0u);
    const uint32_t v0 = getRegU32(ctx, 2);
    const bool skipped = (v0 == 0u);
    if (skipped)
        g_g15_svp_skip.fetch_add(1u, std::memory_order_relaxed);
    const uint32_t n = g_g15_svp.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 40u || (n % 200u) == 0u)
        std::fprintf(stderr, "[G15:sendVuProg] n=%u a0=0x%x a1=0x%x cache=0x%x->0x%x v0=%u skipped=%u skipCount=%u\n",
                     n, a0, a1, cacheBefore, cacheAfter, v0, skipped ? 1u : 0u,
                     g_g15_svp_skip.load(std::memory_order_relaxed));
}

// =====================================================================================
// PHASE G16 ג€” CreateRenderInfoPacket@0x1404d0 return + renderInfo/descriptor flag A/B
// (env DC2_TRACE_G16, quiet/off by default). G15 localized the model-geometry deficit to
// this per-mesh state-packet builder. vtable[0x40] (the would-be geometry emit) is the ELF
// stub 0x140be0 (returns 0) on HW too, so the entire packet here is per-mesh STATE
// (transform matrices + lights + GS regs + drawenv). On HW the return is 0x20..0x51 qwords
// per mesh; this probe measures the runner's return + the descriptor flags that gate the
// emit blocks, to A/B against tools/g16_hw_createpacket.ps1.
// =====================================================================================
static bool g16_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G16");
    return enabled;
}

static bool g30_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G30");
    return enabled;
}

static bool g99_ri_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G99_RI");
    return enabled;
}

static bool g67_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G67");
    return enabled;
}

static std::atomic<uint32_t> g_g16_crip{0};
static std::atomic<uint32_t> g_g30_crip{0};
static std::atomic<uint32_t> g_g217HeadStatePacket{0u};
static std::atomic<uint32_t> g_g217CapStatePacket{0u};

// G217: runner-side counterpart to tools/g217_pcsx2_head_objects.ps1.  Observe both
// CreateRenderInfoPacket entry points, identify visuals by their material TEX0 pages, then
// tail-call the real body without any post-call fixup.  The non-static atomics arm a one-shot
// correlation in ps2_memory.cpp: SendDMA must copy this visual's scratch packet to a1 before
// another visual can reuse the packet slot.  Default off; registration itself is conditional
// on DC2_G217_EEOBJ.
static uint32_t g217_head_kind(uint8_t *rdram, uint32_t self)
{
    uint32_t matCount = self ? dc2_read_u32(rdram, self + 0x40u) : 0u;
    const uint32_t matBase = self ? dc2_read_u32(rdram, self + 0x44u) : 0u;
    if (matCount > 64u) matCount = 64u;
    uint32_t kind = 0u;
    for (uint32_t m = 0u; matBase != 0u && m < matCount; ++m)
    {
        const uint32_t texture = dc2_read_u32(rdram, matBase + m * 0x30u + 0x20u);
        if (texture == 0u) continue;
        const uint32_t tbp = dc2_read_u32(rdram, texture + 0x38u) & 0x3fffu;
        if (tbp == 0x3420u) kind |= 1u;
        if (tbp == 0x35a0u) kind |= 2u;
    }
    return kind;
}

static void g217_trace_eeobj(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t packet = getRegU32(ctx, 5) & 0x0fffffffu;
    const uint32_t matrix = getRegU32(ctx, 6);
    const uint32_t info = getRegU32(ctx, 7);
    const uint32_t desc = info ? dc2_read_u32(rdram, info + 0xfccu) : 0u;
    const uint32_t d2c = desc ? dc2_read_u32(rdram, desc + 0x2cu) : 0xffffffffu;
    uint32_t matCount = self ? dc2_read_u32(rdram, self + 0x40u) : 0u;
    const uint32_t matBase = self ? dc2_read_u32(rdram, self + 0x44u) : 0u;
    if (matCount > 64u) matCount = 64u;

    uint32_t pages[16]{};
    uint32_t pageCount = 0u;
    bool headPage = false;
    if (matBase != 0u)
    {
        for (uint32_t m = 0u; m < matCount; ++m)
        {
            const uint32_t material = matBase + m * 0x30u;
            const uint32_t texture = dc2_read_u32(rdram, material + 0x20u);
            if (texture == 0u) continue;
            const uint32_t tbp = dc2_read_u32(rdram, texture + 0x38u) & 0x3fffu;
            if (tbp == 0u) continue;
            bool duplicate = false;
            for (uint32_t i = 0u; i < pageCount; ++i)
                if (pages[i] == tbp) { duplicate = true; break; }
            if (!duplicate && pageCount < 16u) pages[pageCount++] = tbp;
            if (tbp == 0x3420u || tbp == 0x35a0u) headPage = true;
        }
    }

    if (headPage)
    {
        uint32_t headKind = 0u;
        for (uint32_t i = 0u; i < pageCount; ++i)
        {
            if (pages[i] == 0x3420u) headKind |= 1u;
            if (pages[i] == 0x35a0u) headKind |= 2u;
        }
        g_dc2G217HeadDmaSelf.store(self, std::memory_order_relaxed);
        g_dc2G217HeadDmaKind.store(headKind, std::memory_order_relaxed);
        g_dc2G217HeadDmaPacket.store(packet, std::memory_order_release);
        if ((headKind & 1u) != 0u)
            g_g217HeadStatePacket.store(packet, std::memory_order_release);
        if ((headKind & 2u) != 0u)
            g_g217CapStatePacket.store(packet, std::memory_order_release);

        static std::atomic<uint32_t> s_g217EeCount{0};
        const uint32_t n = s_g217EeCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 2048u)
        {
            char pageText[192]{};
            int pos = 0;
            for (uint32_t i = 0u; i < pageCount && pos < static_cast<int>(sizeof(pageText)); ++i)
                pos += std::snprintf(pageText + pos, sizeof(pageText) - static_cast<size_t>(pos),
                                     "%s0x%x", i ? "," : "", pages[i]);
            std::fprintf(stderr,
                "[G217:eeobj] n=%u tick=%llu self=0x%08x packet=0x%08x matrix=0x%08x info=0x%08x desc=0x%08x d2c=0x%x mats=%u pages=%s\n",
                n, static_cast<unsigned long long>(g_dc2PresentTick.load(std::memory_order_relaxed)),
                self, packet, matrix, info, desc, d2c, matCount, pageText);
        }
    }

}

static void g217_base_createpacket_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g217_trace_eeobj(rdram, ctx);
    // Deliberately no post-call code: if the recompiled body yields, its resume PC must pass
    // straight back to the dispatcher without a wrapper-side PC/SP fixup (G57/G186 rule).
    CreateRenderInfoPacket__12mgCVisualMDTFPUiPA4_fP13mgRENDER_INFO_0x1404d0(rdram, ctx, runtime);
}

static void g217_motion_createpacket_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g217_trace_eeobj(rdram, ctx);
    CreateRenderInfoPacket__18mgCVisualMotionMDTFPUiPA4_fP13mgRENDER_INFO_0x28a660(rdram, ctx, runtime);
}

// G234: dump each mgCVisualMDT's per-material color multipliers (+0x00 and +0x10, two vec4
// colors per G230) + texture page, at CreateRenderInfoPacket time. For the DngStatus=5 map
// pieces this reveals whether the material TINT is present (HW: bluish/tinted) or lost
// (runner suspected white/gray). Default off (DC2_G234_MAT). Tail-calls the real body.
static void g234_mat_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // Only dump in the DngStatus=5 map state (character draws in title/menu are all white
    // and would exhaust the window before the map renders).
    const int32_t dngStatus = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u);
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = (dngStatus == 5) ? s_n.fetch_add(1u, std::memory_order_relaxed) : 0xFFFFFFFFu;
    if (dngStatus == 5 && n < 120u)
    {
        const uint32_t self = getRegU32(ctx, 4);
        uint32_t matCount = self ? dc2_read_u32(rdram, self + 0x40u) : 0u;
        const uint32_t matBase = self ? dc2_read_u32(rdram, self + 0x44u) : 0u;
        if (matCount > 32u) matCount = 32u;
        auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
        for (uint32_t m = 0u; matBase != 0u && m < matCount; ++m)
        {
            const uint32_t mat = matBase + m * 0x30u;
            const uint32_t tex = dc2_read_u32(rdram, mat + 0x20u);
            const uint32_t tbp = tex ? (dc2_read_u32(rdram, tex + 0x38u) & 0x3fffu) : 0u;
            std::fprintf(stderr,
                "[G234:mat] n=%u self=0x%x m=%u tbp=0x%x c0=(%.3f,%.3f,%.3f,%.3f) c1=(%.3f,%.3f,%.3f,%.3f)\n",
                n, self, m, tbp,
                rf(mat), rf(mat + 4u), rf(mat + 8u), rf(mat + 0xcu),
                rf(mat + 0x10u), rf(mat + 0x14u), rf(mat + 0x18u), rf(mat + 0x1cu));
        }
        std::fflush(stderr);
    }
    CreateRenderInfoPacket__12mgCVisualMDTFPUiPA4_fP13mgRENDER_INFO_0x1404d0(rdram, ctx, runtime);
}

static void g217_facepacket_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t packet = getRegU32(ctx, 5) & 0x0fffffffu;
    const uint32_t face = getRegU32(ctx, 6);
    const uint32_t kind = g217_head_kind(rdram, self);
    if (kind != 0u)
    {
        g_dc2G217HeadDmaSelf.store(self, std::memory_order_relaxed);
        g_dc2G217HeadDmaKind.store(kind | 0x100u, std::memory_order_relaxed);
        g_dc2G217HeadDmaPacket.store(packet, std::memory_order_release);
        static std::atomic<uint32_t> s_g217FaceCount{0};
        const uint32_t n = s_g217FaceCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 4096u)
        {
            std::fprintf(stderr,
                "[G217:eeface] n=%u tick=%llu self=0x%08x kind=%u packet=0x%08x face=0x%08x count=%d data=0x%08x\n",
                n, static_cast<unsigned long long>(g_dc2PresentTick.load(std::memory_order_relaxed)),
                self, kind, packet, face,
                face ? static_cast<int16_t>(dc2_read_u32(rdram, face + 8u) & 0xffffu) : 0,
                face ? dc2_read_u32(rdram, face + 0xcu) : 0u);
        }
    }
    CreateFacePacket__12mgCVisualMDTFPUiP7mgCFace_0x13ff60(rdram, ctx, runtime);
}

static void g217_trace_createpacket(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t manager = getRegU32(ctx, 5);
    const uint32_t kind = g217_head_kind(rdram, self);
    if (kind != 0u)
    {
        uint32_t groups = 0u;
        uint32_t faces = 0u;
        uint32_t vertices = 0u;
        uint32_t materialSig = 2166136261u;
        uint32_t group = dc2_read_u32(rdram, self + 0x48u);
        while (group != 0u && groups < 64u)
        {
            const uint32_t material = dc2_read_u32(rdram, group + 0x00u);
            materialSig = (materialSig ^ material) * 16777619u;
            uint32_t face = dc2_read_u32(rdram, group + 0x04u);
            uint32_t groupFaces = 0u;
            while (face != 0u && groupFaces < 256u)
            {
                vertices += static_cast<uint16_t>(dc2_read_u32(rdram, face + 0x08u) & 0xffffu);
                face = dc2_read_u32(rdram, face + 0x10u);
                ++groupFaces;
            }
            faces += groupFaces;
            group = dc2_read_u32(rdram, group + 0x08u);
            ++groups;
        }
        static std::atomic<uint32_t> s_g217PacketCount{0};
        const uint32_t n = s_g217PacketCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 4096u)
        {
            std::fprintf(stderr,
                "[G217:eepacket] n=%u tick=%llu self=0x%08x kind=%u manager=0x%08x head=0x%08x groups=%u faces=%u vertices=%u materialSig=%08x\n",
                n, static_cast<unsigned long long>(g_dc2PresentTick.load(std::memory_order_relaxed)),
                self, kind, manager, dc2_read_u32(rdram, self + 0x48u),
                groups, faces, vertices, materialSig);
        }
    }
}

static void g217_createpacket_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g217_trace_createpacket(rdram, ctx);
    CreatePacket__12mgCVisualMDTFP14mgCDrawManager_0x13f6a0(rdram, ctx, runtime);
}

static void g217_fix_createpacket_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g217_trace_createpacket(rdram, ctx);
    CreatePacket__15mgCVisualFixMDTFP14mgCDrawManager_0x13f920(rdram, ctx, runtime);
}

static void g217_mdt_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t kind = g217_head_kind(rdram, self);
    if (kind != 0u)
    {
        const uint32_t direct = getRegU32(ctx, 5) & 0x0fffffffu;
        const uint32_t slot = g_dc2G217DirectPacketWrite.fetch_add(
            1u, std::memory_order_relaxed) % 128u;
        g_dc2G217DirectPackets[slot].store(direct, std::memory_order_release);
        static std::atomic<uint32_t> s_g217DrawCount{0u};
        const uint32_t n = s_g217DrawCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 512u || (n % 720u) == 0u)
        {
            std::fprintf(stderr,
                "[G217:eedraw] n=%u tick=%llu self=0x%08x kind=%u direct=0x%08x matrix=0x%08x manager=0x%08x ra=0x%08x\n",
                n, static_cast<unsigned long long>(g_dc2PresentTick.load(std::memory_order_relaxed)),
                self, kind, direct, getRegU32(ctx, 6),
                getRegU32(ctx, 7), getRegU32(ctx, 31));
            std::fflush(stderr);
        }
    }
    Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(rdram, ctx, runtime);
}

static void g16_createpacket_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4); // a0 = this (mgCVisualMDT)
    const uint32_t packet = getRegU32(ctx, 5); // a1 = scratch/output packet
    const uint32_t matrix = getRegU32(ctx, 6); // a2 = model matrix
    const uint32_t info = getRegU32(ctx, 7); // a3 = mgRENDER_INFO*
    // G120: UNGATED per-call desc+0x2c capture (no title-scope gate ג€” confirms the wrapper
    // fires and A/Bs against HW's 4 bit2=0 (drawing-route) objects 0x979710/0x9826f0/0x984990/
    // 0x9870d0). bit2 = (desc+0x2c != 0); bit2=0 => the copy/trifan DRAWING route. Env
    // DC2_G120_SEL; first 200 calls; quiet by default.
    if (std::getenv("DC2_G120_SEL") != nullptr && info)
    {
        static std::atomic<uint32_t> s_g120n{0};
        const uint32_t n = s_g120n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 200u)
        {
            const uint32_t desc = dc2_read_u32(rdram, info + 0xfccu);
            const uint32_t d2c = desc ? dc2_read_u32(rdram, desc + 0x2cu) : 0xffffffffu;
            std::fprintf(stderr, "[G120:sel] n=%u info=0x%x desc=0x%x d2c=0x%x bit2=%d\n",
                         n, info, desc, d2c, (desc && d2c != 0u) ? 1 : 0);
            std::fflush(stderr);
        }
    }
    // G115: per-batch qword38 SELECTOR capture. Recompute uVar8 EXACTLY as the decompiled
    // CreateRenderInfoPacket body does (asm 0x140924-0x1409dc) at ENTRY, so we see, per rock
    // batch, which VU packer it routes to (dispatcher 0x0708: bit2=tri/trifan vs bit2=0 -> bit0
    // tristrip0x1c50 / copy0x1b68) and WHICH render-state flag drives it. The fix target is the
    // field that makes the runner never produce bit2==0 && bit0==1 (tristrip). Title-scoped,
    // default-off (DC2_G115_SEL), first 48 calls. A/B vs HW (PCSX2 breakpoint @0x1404d0, read a3).
    if (std::getenv("DC2_G115_SEL") != nullptr && g67_title_scope(rdram, ctx))
    {
        static std::atomic<uint32_t> s_g115n{0};
        const uint32_t n = s_g115n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 48u && info)
        {
            const uint32_t fc0  = dc2_read_u32(rdram, info + 0xfc0u);
            const uint32_t fc4  = dc2_read_u32(rdram, info + 0xfc4u);
            const uint32_t fc8  = dc2_read_u32(rdram, info + 0xfc8u);
            const uint32_t i1010= dc2_read_u32(rdram, info + 0x1010u);
            const uint32_t fac  = dc2_read_u32(rdram, info + 0xfacu);
            const uint32_t desc = dc2_read_u32(rdram, info + 0xfccu);
            uint32_t d2c=0,d40=0,d4c=0,d60=0,d84=0;
            if (desc)
            {
                d2c = dc2_read_u32(rdram, desc + 0x2cu);
                d40 = dc2_read_u32(rdram, desc + 0x40u);
                d4c = dc2_read_u32(rdram, desc + 0x4cu);
                d60 = dc2_read_u32(rdram, desc + 0x60u);
                d84 = dc2_read_u32(rdram, desc + 0x84u);
            }
            uint32_t sel = (fc0 != 0u || fc4 != 0u) ? 1u : 0u;       // bit0
            if (fc4 != 0u)            sel |= 0x2u;                    // bit1
            if (d40 != 0u) { if (d40 & 1u) sel |= 0x8u; if (d40 & 2u) sel |= 0x100u; } // bit3/bit8
            if (d2c != 0u)            sel |= 0x4u;                    // bit2 (tri vs tristrip)
            if (fc8 != 0u)            sel |= 0x10u;                   // bit4
            if (i1010 != 0u)          sel |= 0x40u;                   // bit6
            if (d60 != 0u || fac == 0u || d4c != 0u) sel |= 0x20u;    // bit5
            if (d84 == 1u)            sel |= 0x80u;                   // bit7
            const char *route = (sel & 0x4u) ? "tri/trifan(0x1dc0/0x1ff0)"
                                : (sel & 0x1u) ? "TRISTRIP(0x1c50)" : "copy(0x1b68)";
            std::fprintf(stderr,
                "[G115:sel] n=%u self=0x%x info=0x%x desc=0x%x sel=0x%x route=%s | "
                "fc0=%u fc4=%u fc8=%u d2c=0x%x d40=0x%x d4c=0x%x d60=0x%x d84=0x%x fac=%u i1010=%u\n",
                n, self, info, desc, sel, route,
                fc0, fc4, fc8, d2c, d40, d4c, d60, d84, fac, i1010);
            std::fflush(stderr);
        }
    }
    if (g99_ri_trace_enabled() && g67_title_scope(rdram, ctx))
    {
        static std::atomic<uint32_t> s_g99N{0};
        const uint32_t gn = s_g99N.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const char *gf = std::getenv("DC2_G99_RI_FRAME");
        const uint32_t target = gf ? (uint32_t)std::atoi(gf) : 1u;
        static bool s_done = false;
        if (!s_done && gn >= target)
        {
            s_done = true;
            auto rf = [&](uint32_t a) {
                uint32_t u = dc2_read_u32(rdram, a);
                float f;
                std::memcpy(&f, &u, sizeof(f));
                return f;
            };
            auto dumpMat = [&](uint32_t base, uint32_t off, const char *label) {
                std::fprintf(stderr, "[G99:ri] --- %s info+0x%x ---\n", label, off);
                for (uint32_t r = 0u; r < 4u; ++r)
                {
                    std::fprintf(stderr, "[G99:ri]   %12.4f %12.4f %12.4f %12.4f\n",
                        rf(base + off + (r * 16u) + 0u),
                        rf(base + off + (r * 16u) + 4u),
                        rf(base + off + (r * 16u) + 8u),
                        rf(base + off + (r * 16u) + 12u));
                }
            };
            const uint32_t desc = info ? dc2_read_u32(rdram, info + 0xfccu) : 0u;
            const float branch = desc ? rf(desc + 0x8cu) : 0.0f;
            std::fprintf(stderr,
                "[G99:ri] n=%u this=0x%x packet=0x%x world=0x%x info=0x%x desc=0x%x "
                "desc+0x8c=%.4f branch=%s fc0=0x%x fc4=0x%x idx@+0x3f4=%u\n",
                gn, self, packet, matrix, info, desc, branch,
                branch > 1.0f ? "proj150" : "camera10",
                info ? dc2_read_u32(rdram, info + 0xfc0u) : 0u,
                info ? dc2_read_u32(rdram, info + 0xfc4u) : 0u,
                info ? dc2_read_u32(rdram, info + 0x3f4u) : 0u);
            if (info)
            {
                dumpMat(info, 0x10u, "camera/view-proj");
                dumpMat(info, 0x110u, "projection");
                dumpMat(info, 0x150u, "view*aspect");
            }
            if (matrix)
                dumpMat(matrix, 0x0u, "world");
            std::fflush(stderr);
        }
    }
    // G199: EE-side producer trace for VU1 data-memory quadword 30 (G198's map-only 0x0/0x20
    // divergence). Hypothesis from static analysis: CreateRenderInfoPacket's optional block
    // gated by info+0xfc4 uploads 8 quadwords at VU1 ADDR 25..32 (VIFcode 0x6c080019 at
    // asm 0x1409d8); the 6th quadword of that block (VU1 ADDR 30) is copied VERBATIM from
    // info+0x270..info+0x27c. Dump the gate flag + that raw quadword BEFORE the real call runs
    // (captures exactly what this call will upload, or would skip uploading if fc4==0 -- in
    // which case VU1 addr 30 keeps whatever a PRIOR object's kick left there). Default-off
    // (DC2_G199_QW30), unscoped (need both title and map data), first 300 + every 500th call.
    if (std::getenv("DC2_G199_QW30") != nullptr && info)
    {
        static std::atomic<uint32_t> s_g199n{0};
        const uint32_t n = s_g199n.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 300u || (n % 500u) == 0u)
        {
            const uint32_t fc4 = dc2_read_u32(rdram, info + 0xfc4u);
            const uint32_t desc = dc2_read_u32(rdram, info + 0xfccu);
            const uint32_t q30x = dc2_read_u32(rdram, info + 0x270u);
            const uint32_t q30y = dc2_read_u32(rdram, info + 0x274u);
            const uint32_t q30z = dc2_read_u32(rdram, info + 0x278u);
            const uint32_t q30w = dc2_read_u32(rdram, info + 0x27cu);
            std::fprintf(stderr,
                "[G199:qw30src] n=%u self=0x%x info=0x%x desc=0x%x fc4=%u blockEmitted=%d "
                "q30=0x%08x,0x%08x,0x%08x,0x%08x\n",
                n, self, info, desc, fc4, fc4 != 0u ? 1 : 0, q30x, q30y, q30z, q30w);
            std::fflush(stderr);
        }
    }
    CreateRenderInfoPacket__12mgCVisualMDTFPUiPA4_fP13mgRENDER_INFO_0x1404d0(rdram, ctx, runtime);
    const uint32_t ret = getRegU32(ctx, 2); // v0 = qword count
    // G93: steady-state LW (param_3 = a2 model matrix) + ViewProj (info+0x10) dump, title
    // scope, to localize why the transform path lands off-screen despite an HW-correct camera.
    if (std::getenv("DC2_G93_LW") != nullptr && g67_title_scope(rdram, ctx))
    {
        static std::atomic<uint32_t> s_lwN{0};
        const uint32_t ln = s_lwN.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const char *lf = std::getenv("DC2_G93_LW_FRAME");
        const uint32_t lt = lf ? (uint32_t)std::atoi(lf) : 3000u;
        if (ln >= lt && ln < lt + 6u) // a handful of consecutive visuals at steady state
        {
            auto f32 = [&](uint32_t a){ uint32_t u=dc2_read_u32(rdram,a); float f; std::memcpy(&f,&u,4); return f; };
            auto dm = [&](const char*tag,uint32_t b){ for(uint32_t r=0;r<4;++r) std::fprintf(stderr,
                "[G93:lw] %s r%u % .3f % .3f % .3f % .3f\n",tag,r,f32(b+r*16),f32(b+r*16+4),f32(b+r*16+8),f32(b+r*16+12)); };
            std::fprintf(stderr,"[G93:lw] ln=%u this=0x%x matrix(LW)=0x%x info=0x%x ret=0x%x\n",ln,self,matrix,info,ret);
            if (matrix) dm("LW",matrix);
            if (info)   dm("VP",info+0x10u);
            std::fflush(stderr);
        }
    }
    if (g30_trace_enabled())
    {
        const uint32_t n = g_g30_crip.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 12u)
        {
            auto f32 = [&](uint32_t addr) -> float {
                float v = 0.0f;
                const uint32_t raw = dc2_read_u32(rdram, addr);
                std::memcpy(&v, &raw, sizeof(v));
                return v;
            };
            auto dumpQw = [&](const char *tag, uint32_t addr) {
                std::fprintf(stderr, "[G30:%s] n=%u addr=0x%x = % .6g % .6g % .6g % .6g\n",
                    tag, n, addr, f32(addr), f32(addr + 4u), f32(addr + 8u), f32(addr + 12u));
            };
            std::fprintf(stderr,
                "[G30:crip] n=%u this=0x%x packet=0x%x matrix=0x%x info=0x%x ret=0x%x pc=0x%x\n",
                n, self, packet, matrix, info, ret, ctx ? ctx->pc : 0u);
            for (uint32_t q = 0; q < 12u; ++q)
                dumpQw("pkt", packet + (q * 16u));
            for (uint32_t q = 0; q < 4u; ++q)
                dumpQw("mat", matrix + (q * 16u));
            dumpQw("info_3a0", info + 0x3A0u);
            dumpQw("info_1000", info + 0x1000u);
        }
    }
    const uint32_t n = g_g16_crip.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n > 24u && (n % 200u) != 0u)
        return;
    const uint32_t desc = info ? dc2_read_u32(rdram, info + 0xFCCu) : 0u;
    auto R = [&](uint32_t base, uint32_t off) -> uint32_t {
        return base ? dc2_read_u32(rdram, base + off) : 0u;
    };
    std::fprintf(stderr,
        "[G16:crip] n=%u this=0x%x info=0x%x desc=0x%x ret=0x%x | "
        "s5:+fa4=0x%x +fac=0x%x +fc0=0x%x +fc4=0x%x +fc8=0x%x +100c=0x%x +1010=0x%x | "
        "desc:+2c=0x%x +40=0x%x +4c=0x%x +60=0x%x +84=0x%x +8c=0x%x +44=0x%x\n",
        n, self, info, desc, ret,
        R(info, 0xFA4u), R(info, 0xFACu), R(info, 0xFC0u), R(info, 0xFC4u), R(info, 0xFC8u),
        R(info, 0x100Cu), R(info, 0x1010u),
        R(desc, 0x2Cu), R(desc, 0x40u), R(desc, 0x4Cu), R(desc, 0x60u), R(desc, 0x84u),
        R(desc, 0x8Cu), R(desc, 0x44u));
}

static std::atomic<uint32_t> g_g16_mdt{0};
static std::atomic<uint32_t> g_g43_face_dump{0};
static std::atomic<uint32_t> g_g43_create_face_dump{0};
static std::atomic<uint32_t> g_g67_block{0};
static std::atomic<uint32_t> g_g67_mapdraw{0};
static std::atomic<uint32_t> g_g67_partsdraw{0};
static std::atomic<uint32_t> g_g67_enddraw{0};
static std::atomic<uint32_t> g_g67_frame{0};
static std::atomic<uint32_t> g_g67_test1{0};
static std::atomic<uint32_t> g_g67_clip{0};
static std::atomic<uint32_t> g_g67_mdt{0};
static std::atomic<uint32_t> g_g67_add{0};

struct G67FrameScope
{
    uint32_t seq;
    uint32_t frame;
    uint32_t f0;
    uint32_t f4;
    uint32_t f8;
    uint32_t desc18;
    uint32_t desc40;
    uint32_t desc48;
    uint32_t child;
    uint32_t before1;
    uint32_t before3;
    uint32_t distFix;
    uint32_t test1Seen;
    uint32_t test1Fixed;
    uint32_t clipSeen;
    uint32_t clipFixed;
    uint32_t clipRet;
    uint32_t clipA0;
    uint32_t clipA1;
    uint32_t clipA2;
    uint32_t clipA3;
    float test1Dist;
    float test1OriginalDist;
    float test1Limit;
};

static thread_local G67FrameScope g_g67_frame_stack[64];
static thread_local uint32_t g_g67_frame_depth = 0u;

static float g67_read_f32(uint8_t *rdram, uint32_t addr)
{
    const uint32_t bits = dc2_read_u32(rdram, addr);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

static uint32_t g67_f32_bits(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

static bool g67_title_dist_fix_enabled()
{
    static const bool enabled = !dc2_env_flag_enabled("DC2_G67_NO_TITLE_DIST_FIX");
    return enabled;
}

static G67FrameScope *g67_top_frame_scope()
{
    if (g_g67_frame_depth == 0u || g_g67_frame_depth > 64u)
        return nullptr;
    return &g_g67_frame_stack[g_g67_frame_depth - 1u];
}

static bool g67_focus_frame(uint32_t frame, uint32_t visual)
{
    return frame == 0x00981390u || visual == 0x00987230u ||
           (visual >= 0x00987000u && visual < 0x00989000u);
}

static bool g67_title_frame_target(uint32_t frame, uint32_t visual)
{
    return frame == 0x00981390u && visual == 0x00987230u;
}

static const char *g67_frame_reason(const G67FrameScope &s, uint32_t ret)
{
    if (s.f4 == 0u)
        return "no-desc";
    if ((s.desc18 & 1u) == 0u)
        return "desc18-no-draw";
    if (s.f8 == 0u)
        return "no-visual";
    if (s.desc48 != 0u || s.f0 == 0u)
        return ret ? "direct-drawn" : "direct-empty";
    if (s.test1Seen && !s.test1Fixed && s.test1Dist < s.test1Limit)
        return "dist-cull";
    if (s.clipSeen && !s.clipFixed && s.clipRet == 0u)
        return "clipbox-cull";
    if (!s.clipSeen)
        return ret ? "drawn-no-cliplog" : "preclip-cull";
    return ret ? "drawn" : "empty-after-clip";
}

// G16: walk the deform DMA-node list (this+0x48 chain) that Draw__12mgCVisualMDT turns into
// 0x5000 ref tags carrying the UNPACK+MSCAL vertex packets. A/B vs tools/g16_hw_deformlist.ps1.
static void g16_mdt_listwalk_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4); // a0 = this
    const uint32_t a1 = getRegU32(ctx, 5);
    const uint32_t a3 = getRegU32(ctx, 7);
    Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(rdram, ctx, runtime);
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t n = g_g16_mdt.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n > 16u)
        return;
    auto inRam = [](uint32_t a) { return a >= 0x10000u && a <= 0x1FFFFF0u; };
    auto R = [&](uint32_t a) -> uint32_t { return inRam(a) ? dc2_read_u32(rdram, a) : 0u; };
    const uint32_t head = R(self + 0x48u);
    const uint32_t f04 = R(self + 0x04u);
    char buf[1024];
    int p = std::snprintf(buf, sizeof(buf),
        "[G16:mdt] n=%u this=0x%x a1(out)=0x%x drawMgr=0x%x this+0x4=0x%x head=0x%x ret=0x%x",
        n, self, a1, a3, f04, head, ret);
    uint32_t node = head; int ni = 0;
    while (inRam(node) && ni < 8 && p < (int)sizeof(buf) - 96)
    {
        const uint32_t vprog = R(node + 0xCu), vptr = R(node + 0x10u), nxt = R(node + 0x8u);
        const uint32_t vtx0 = inRam(vptr) ? R(vptr) : 0u;
        p += std::snprintf(buf + p, sizeof(buf) - p,
            " | node%d=0x%x vprog=0x%x vtx=0x%x vtx[0]=0x%x next=0x%x",
            ni, node, vprog, vptr, vtx0, nxt);
        node = nxt; ni++;
    }
    std::fprintf(stderr, "%s  nodes=%d\n", buf, ni);
    // Dump node0's vtx sub-packet (node+0x10 target) as DMAtags to compare vs HW 0x4aeff0
    // (HW = REF qwc=7 / REF qwc=3 / REF qwc=191 -> the bulk vertex geometry).
    if (n <= 4 && inRam(head))
    {
        const uint32_t vptr = R(head + 0x10u);
        if (inRam(vptr))
        {
            char vb[640];
            int q = std::snprintf(vb, sizeof(vb), "[G16:vtxsub] n=%u node0=0x%x vtx=0x%x:", n, head, vptr);
            for (int i = 0; i < 6 && q < (int)sizeof(vb) - 64; ++i)
            {
                const uint32_t a = vptr + (uint32_t)(i * 16);
                const uint32_t w0 = R(a), w1 = R(a + 4);
                const uint32_t id = (w0 >> 28) & 0x7u, qwc = w0 & 0xFFFFu;
                q += std::snprintf(vb + q, sizeof(vb) - q, " [+0x%x w0=0x%x id=%u qwc=%u addr=0x%x]",
                                   i * 16, w0, id, qwc, w1 & 0x7FFFFFFFu);
            }
            std::fprintf(stderr, "%s\n", vb);
        }
    }
}

static bool g43_guest_ram_addr(uint32_t addr)
{
    const uint32_t a = addr & 0x1FFFFFFFu;
    return a >= 0x10000u && a <= 0x1FFFFF0u;
}

static uint32_t g43_guest_addr(uint32_t addr)
{
    return addr & 0x1FFFFFFFu;
}

static uint16_t g43_read_u16(uint8_t *rdram, uint32_t addr)
{
    return static_cast<uint16_t>(dc2_read_u32(rdram, addr) & 0xFFFFu);
}

struct G67TexHit
{
    uint32_t addr;
    uint32_t tbp;
    uint32_t psm;
    uint32_t cbp;
    uint64_t tex0;
};

#include "dc2_game_override_parts/title_render_probes_a.inc"

#include "dc2_game_override_parts/title_render_probes_b.inc"

#include "dc2_game_override_parts/ui_state_probes.inc"

// =====================================================================
// PHASE G34: costume 3D model RTT->display composite gap (continues G33).
// G33 proved the runner renders the model into the 0x2720 work buffer (CT32)
// but never composites it onto the display, because the per-character outline
// composite (Draw__12COutLineDraw -> DrawDivSprite4) is gated OFF.
// Decompiled DrawDirect__11CCharacter2@0x1731f0 (ref/decompiled.txt:95037):
//     fVar13 = *(this+0x100);                 // outline level (lwc1 0x100(a0))
//     fVar12 = vtable+0x44();                 // -> COutLineDraw param_1
//     vtable+0x48(fVar12, this, &fStack_4);   // writes fStack_4
//     fVar13 = fVar13 * fStack_4;             // == COutLineDraw param_2 (the gate)
//     if (1.0 <= fVar13) loop COutLineDraw(composite)  else  composite skipped
// And inside Draw__12COutLineDraw@0x17c2d0 (ref/decompiled.txt:102499) the
// DrawDivSprite4 RTT->display blit needs:  *(this+0x64)==0 && 1.0<=param_2 ...
// HW (G33 A/B): *(CCharacter2+0x100)=1.0, fStack_4=1.0 -> fVar13=1.0 -> fires.
// Runner: fVar13 < 1.0. These probes capture BOTH factors at draw time so we
// can tell whether the outline level (+0x100) or fStack_4 (vtable+0x48) is the
// divergence. DC2_G34_FIX restores the outline level to 1.0 (G13-class un-run
// init repair) to VERIFY that lifting fVar13>=1.0 makes the model visible.
// All gated to the costume route (g9) so default smoke is untouched.
// =====================================================================
#include "dc2_game_override_parts/layout_sprite_fix.inc"



// PHASE9: DC2 ג€” Apply all Phase 9.3 stub address bindings for Dark Cloud 2.
// PHASE G186: guest-sp desync probe for the 0xe3dc70 / level-load crash class.
// EditDraw's saved-ra stack slot is overwritten by ordinary mg-library callees
// (G186 watch-ring evidence), and the same route crashes with DIFFERENT sp values
// across runs — hypothesis: some callee leaves ctx->sp shifted, so subsequent
// callee frames overlap EditDraw's save area. Log sp/ra at EditLoop/EditDraw
// entry+exit each frame. Opt-in DC2_G186_SPTRACE=1.
#include "dc2_game_override_parts/level_load_debug_probes.inc"

#include "dc2_game_override_parts/apply_phase9_stubs.inc"

// PHASE F9: DC2 ג€” Apply ezMidi audio-compat layer (sister-fork port).
// rpcSid 0x00012346 is what DC2's EE-side ezMidi wrapper binds to.
#include "dc2_game_override_parts/apply_ezmidi_compat.inc"

} // namespace

// F66: present-loop entry point for in-dungeon free-roam pad input (external
// linkage; declared extern in ps2_runtime.cpp). Delegates to the anonymous-namespace
// parser/driver above, which are visible here within the same translation unit.
bool f66_drive_dungeon_pad(uint32_t dungeonFrame)
{
    const auto &events = f66_get_dungeon_events();
    if (events.empty()) { g_f66_dungeon_active = false; return false; }
    uint16_t pressed = 0u;
    for (const auto &e : events) if (e.first == dungeonFrame) pressed |= e.second;
    uint8_t lx = 0x80u, ly = 0x80u;
    if ((pressed & 0x10u) != 0u) ly = 0x00u; // Up
    if ((pressed & 0x40u) != 0u) ly = 0xFFu; // Down
    if ((pressed & 0x80u) != 0u) lx = 0x00u; // Left
    if ((pressed & 0x20u) != 0u) lx = 0xFFu; // Right
    // Publish for pad_button_read_stub (the live pad path). Active whenever the
    // dungeon script is configured, even on "no input" frames, so the stub owns the
    // stick instead of the title schedule.
    g_f66_dungeon_buttons = pressed;
    g_f66_dungeon_lx = lx;
    g_f66_dungeon_ly = ly;
    g_f66_dungeon_active = true;
    static const bool trace = dc2_env_flag_enabled("DC2_TRACE_PAD_INPUT");
    if (trace && pressed != 0u)
        std::fprintf(stderr, "[F66:dungeonPad] dframe=%u pressed=0x%04x lx=0x%02x ly=0x%02x\n",
                     dungeonFrame, pressed, lx, ly);
    return true;
}
