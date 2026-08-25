// ===== G657 P1: 27 duplicate definitions removed ==========================
// Each collided with an identical symbol in another TU and was ALREADY discarded
// by the linker ("second definition ignored", LNK4006), so removal is
// behaviour-neutral by construction. Removed because under /GL /LTCG the same
// collision is fatal (LNK1179), which is what blocked the PGO pipeline (G652).
// Regenerate with tools/g657_dup_audit.py.
static void applySuspendStatusLocked(ThreadInfo &info)
{
    if (info.waitType != TSW_NONE)
    {
        info.status = THS_WAITSUSPEND;
    }
    else
    {
        info.status = THS_SUSPEND;
    }
}

static void runExitHandlersForThread(int tid, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!runtime || !ctx)
        return;

    std::vector<ExitHandlerEntry> handlers;
    {
        std::lock_guard<std::mutex> lock(g_exit_handler_mutex);
        auto it = g_exit_handlers.find(tid);
        if (it == g_exit_handlers.end())
            return;
        handlers = std::move(it->second);
        g_exit_handlers.erase(it);
    }

    for (const auto &handler : handlers)
    {
        if (!handler.func)
            continue;
        try
        {
            rpcInvokeFunction(rdram, ctx, runtime, handler.func, handler.arg, 0, 0, 0, nullptr);
        }
        catch (const ThreadExitException &)
        {
            // ignore
        }
        catch (const std::exception &)
        {
        }
    }
}








} // namespace ps2_syscalls
extern std::mutex g_guestThreadCtxMutex;
extern std::unordered_map<int, R5900Context *> g_guestThreadCtxs;
namespace ps2_syscalls {




















