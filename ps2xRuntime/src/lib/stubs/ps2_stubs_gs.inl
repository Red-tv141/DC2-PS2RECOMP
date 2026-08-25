// ===== G657 P1: 23 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
namespace
{
    std::mutex g_gs_sync_v_mutex;
    uint64_t g_gs_sync_v_base_tick = 0u;
    std::mutex g_gs_sync_v_callback_mutex;
    uint32_t g_gs_sync_v_callback_func = 0u;
    uint32_t g_gs_sync_v_callback_gp = 0u;
    uint32_t g_gs_sync_v_callback_sp = 0u;
    uint32_t g_gs_sync_v_callback_stack_base = 0u;
    uint32_t g_gs_sync_v_callback_stack_top = 0u;
    uint32_t g_gs_sync_v_callback_bad_pc_logs = 0u;
}

static void resetGsSyncVState()
{
    std::lock_guard<std::mutex> lock(g_gs_sync_v_mutex);
    g_gs_sync_v_base_tick = ps2_syscalls::GetCurrentVSyncTick();
}

static int32_t getGsSyncVFieldForTick(uint64_t tick)
{
    std::lock_guard<std::mutex> lock(g_gs_sync_v_mutex);
    if (tick <= g_gs_sync_v_base_tick)
    {
        return 0;
    }

    return static_cast<int32_t>((tick - g_gs_sync_v_base_tick - 1u) & 1u);
}























