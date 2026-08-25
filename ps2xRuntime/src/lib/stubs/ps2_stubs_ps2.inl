// ===== G657 P1: 4 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.




/*
void InitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static int logCount = 0;
    if (logCount < 8)
    {
        std::cout << "ps2_stub InitThread" << std::endl;
        ++logCount;
    }
    setReturnS32(ctx, 1); // success
}
*/