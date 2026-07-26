// =============================================================================
// PHASE G381 — retire the `TODO_NAMED` process-abort class.
//
// G380 pinned `config_dc2_final.toml` as THE recompiler config and enumerated its one
// residual defect: 36 of the 453 stub addresses resolve in NEITHER `PS2_STUB_LIST` nor
// `PS2_SYSCALL_LIST` (`ps2_call_list.h`), so `PS2Recompiler::isStubFunction` emitted
// `ps2_stubs::TODO_NAMED("<name>")`, which THROWS `std::runtime_error` and ends the
// process. `bindAddressHandler(runtime, addr, "<name>")` in apply_phase9_stubs.inc cannot
// repair them either: it resolves through the same two tables and only prints
// "[game_overrides] unresolved handler", leaving the throwing body live.
//
// G380 fixed exactly one member of the class by hand (`sceDevctl`). This file closes the
// class: every remaining DEAD address gets a non-throwing handler with the semantics the
// real library would produce on THIS host (no dev9/HDD, no IOP kernel, no TLB, host
// allocator). Registration is by guest address, last-write-wins, so it survives a regen —
// per the project rule that a runtime override outlives a generated tree.
//
// Reachability audit (ref/index/calls_index.json, callers → main):
//   * HDD/host-fs family (sceMount/sceChdir/sceDopen/sceDread/sceGetstat/...) is reached
//     from `HddConectCheck__FPi` / `CheckInstallSpace__Fv` / `CheckAppInstall__Fv` /
//     `MountHDDFileSystem__Fv`, i.e. from `TitleInit`/`TitleLoop`/`HDDMenuLoop` —
//     LIVE menu paths. They must FAIL (-11, the same "fs RPC endpoint absent" code
//     G380 gave `sceDevctl`), never succeed: a fake success walks the guest deeper into
//     an HDD install that does not exist.
//   * `sceGetstat` is additionally a callee of `LoadFile2__FPcPvPii` (126 callers), but
//     only inside its `dev == 3` (host/HDD) branch; DC2 runs `DefaultFileDev == 1` (CD),
//     so that edge is latent — still fatal if ever taken.
//   * `sbrk` (← `_sbrk_r` ← `malloc_extend_top`/`_malloc_trim_r`) and `SyncDCache`
//     (← `_send_to_iop` ← `scePadSetActDirect`, and ← `scePadGetDmaStr`) are unreachable
//     today only because their callers are themselves stubbed out. Repaired anyway.
//   * The remainder (TLB, exception vectors, power-off callbacks, `iSuspendThread`,
//     `CB_DelayTh`, `sceMcUdCheckNewCard`, `_ch3dmaCSC`, `sceSifStopModule`) have no
//     guest callers in the export; they are pure abort-surface.
//
// Rollback lever: DC2_G381_NO_DEAD_STUBS=1 restores the throwing TODO_NAMED bodies.
// =============================================================================

// Error code convention: PS2 libc file I/O returns a negative errno. -11 is what G380's
// `ps2_stubs::sceDevctl` returns for "no HDD / fs RPC endpoint absent"; reuse it so every
// no-device answer the guest sees on this path is identical.
static constexpr int32_t kG381NoDevice = -11;

static void g381_log_once(const char *name, int32_t rv)
{
    static const bool quiet = !dc2_env_flag_enabled("DC2_G381_TRACE");
    if (quiet)
        return;
    std::fprintf(stderr, "[G381:deadstub] %s -> %d\n", name, static_cast<int>(rv));
}

// Returns 0: the operation is a genuine no-op on this host (cache maintenance, TLB,
// interrupt-context queries, power-off/alarm callbacks, IOP module teardown).
#define DC2_G381_DEFINE_ZERO_STUB(fn, label)                                             \
    static void fn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)               \
    {                                                                                    \
        (void)rdram;                                                                     \
        (void)runtime;                                                                   \
        g381_log_once(label, 0);                                                         \
        setReturnS32(ctx, 0);                                                            \
        ctx->pc = getRegU32(ctx, 31);                                                    \
    }

// Returns -11: the device/filesystem the guest is asking for does not exist here.
#define DC2_G381_DEFINE_NODEV_STUB(fn, label)                                            \
    static void fn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)               \
    {                                                                                    \
        (void)rdram;                                                                     \
        (void)runtime;                                                                   \
        g381_log_once(label, kG381NoDevice);                                             \
        setReturnS32(ctx, kG381NoDevice);                                                \
        ctx->pc = getRegU32(ctx, 31);                                                    \
    }

// --- cache / DMA / interrupt-context / TLB / callback no-ops -------------------
DC2_G381_DEFINE_ZERO_STUB(g381_ch3dmacsc_stub, "_ch3dmaCSC")
DC2_G381_DEFINE_ZERO_STUB(g381_syncdcache_stub, "SyncDCache")
DC2_G381_DEFINE_ZERO_STUB(g381_invaliddcache_stub, "InvalidDCache")
DC2_G381_DEFINE_ZERO_STUB(g381_queryintrcontext_stub, "QueryIntrContext")
DC2_G381_DEFINE_ZERO_STUB(g381_isuspendthread_stub, "iSuspendThread")
DC2_G381_DEFINE_ZERO_STUB(g381_powerooffhandler_stub, "scePowerOffHandler")
DC2_G381_DEFINE_ZERO_STUB(g381_settlbentry_stub, "SetTLBEntry")
DC2_G381_DEFINE_ZERO_STUB(g381_gettlbentry_stub, "GetTLBEntry")
DC2_G381_DEFINE_ZERO_STUB(g381_ktlbexception_stub, "_kTLBException")
DC2_G381_DEFINE_ZERO_STUB(g381_kdebugexception_stub, "_kDebugException")
DC2_G381_DEFINE_ZERO_STUB(g381_cb_delayth_stub, "CB_DelayTh")
DC2_G381_DEFINE_ZERO_STUB(g381_cdpoffcallback_stub, "sceCdPOffCallback")
DC2_G381_DEFINE_ZERO_STUB(g381_sifstopmodule_stub, "sceSifStopModule")
// sceMcUdCheckNewCard: "has a new card been inserted since the last check" — 0 = no.
DC2_G381_DEFINE_ZERO_STUB(g381_mcudchecknewcard_stub, "sceMcUdCheckNewCard")
// sceDclose / sceSync close-and-flush semantics: idempotent success. The guest can only
// reach these with an fd sceDopen already refused, so 0 keeps its cleanup path quiet
// instead of feeding a second error into `error_cb`.
DC2_G381_DEFINE_ZERO_STUB(g381_dclose_stub, "sceDclose")
DC2_G381_DEFINE_ZERO_STUB(g381_sync_stub, "sceSync")

// --- HDD / host filesystem: must fail, never fake success ----------------------
DC2_G381_DEFINE_NODEV_STUB(g381_ioctl2_stub, "sceIoctl2")
DC2_G381_DEFINE_NODEV_STUB(g381_remove_stub, "sceRemove")
DC2_G381_DEFINE_NODEV_STUB(g381_mkdir_stub, "sceMkdir")
DC2_G381_DEFINE_NODEV_STUB(g381_rmdir_stub, "sceRmdir")
DC2_G381_DEFINE_NODEV_STUB(g381_format_stub, "sceFormat")
DC2_G381_DEFINE_NODEV_STUB(g381_adddrv_stub, "sceAddDrv")
DC2_G381_DEFINE_NODEV_STUB(g381_deldrv_stub, "sceDelDrv")
DC2_G381_DEFINE_NODEV_STUB(g381_dopen_stub, "sceDopen")
DC2_G381_DEFINE_NODEV_STUB(g381_dread_stub, "sceDread")
DC2_G381_DEFINE_NODEV_STUB(g381_getstat_stub, "sceGetstat")
DC2_G381_DEFINE_NODEV_STUB(g381_chstat_stub, "sceChstat")
DC2_G381_DEFINE_NODEV_STUB(g381_rename_stub, "sceRename")
DC2_G381_DEFINE_NODEV_STUB(g381_chdir_stub, "sceChdir")
DC2_G381_DEFINE_NODEV_STUB(g381_mount_stub, "sceMount")
DC2_G381_DEFINE_NODEV_STUB(g381_umount_stub, "sceUmount")
DC2_G381_DEFINE_NODEV_STUB(g381_lseek64_stub, "sceLseek64")
DC2_G381_DEFINE_NODEV_STUB(g381_symlink_stub, "sceSymlink")
DC2_G381_DEFINE_NODEV_STUB(g381_readlink_stub, "sceReadlink")

// sbrk: newlib's "extend the break" primitive. The guest C heap is runtime-backed
// (`PS2Runtime::guestMalloc`, and the whole malloc family is stubbed in the TOML), so no
// legitimate caller survives; the correct answer for a break that cannot grow is
// (void *)-1 = ENOMEM, which makes newlib's malloc return NULL rather than hand out a
// bogus pointer above `_end` (F58 heap rule).
static void g381_sbrk_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    (void)rdram;
    (void)runtime;
    g381_log_once("sbrk", -1);
    setReturnS32(ctx, -1);
    ctx->pc = getRegU32(ctx, 31);
}

#undef DC2_G381_DEFINE_ZERO_STUB
#undef DC2_G381_DEFINE_NODEV_STUB

// Addresses from config_dc2_final.toml (`!! DEAD` block). Registered last so they win
// over the unresolved bindAddressHandler attempts earlier in applyDC2Phase9Stubs.
static void applyDC2G381DeadStubRepairs(PS2Runtime &runtime)
{
    static const bool disabled = dc2_env_flag_enabled("DC2_G381_NO_DEAD_STUBS");
    if (disabled)
        return;

    if (dc2_env_flag_enabled("DC2_G381_TRACE"))
        std::fprintf(stderr, "[G381:deadstub] registering 35 dead-stub repairs "
                             "(sceDevctl@0x116A58 handled by G380)\n");

    runtime.registerFunction(0x0010C988u, g381_ch3dmacsc_stub);        // _ch3dmaCSC
    runtime.registerFunction(0x001107E0u, g381_sbrk_stub);             // sbrk
    runtime.registerFunction(0x001109D8u, g381_syncdcache_stub);       // SyncDCache
    runtime.registerFunction(0x00110B18u, g381_invaliddcache_stub);    // InvalidDCache
    runtime.registerFunction(0x00110BB0u, g381_queryintrcontext_stub); // QueryIntrContext
    runtime.registerFunction(0x001111E8u, g381_isuspendthread_stub);   // iSuspendThread
    runtime.registerFunction(0x00113F48u, g381_powerooffhandler_stub); // scePowerOffHandler
    runtime.registerFunction(0x00115158u, g381_ioctl2_stub);           // sceIoctl2
    runtime.registerFunction(0x001154E0u, g381_remove_stub);           // sceRemove
    runtime.registerFunction(0x00115500u, g381_mkdir_stub);            // sceMkdir
    runtime.registerFunction(0x001156B0u, g381_rmdir_stub);            // sceRmdir
    runtime.registerFunction(0x001156D0u, g381_format_stub);           // sceFormat
    runtime.registerFunction(0x00115940u, g381_adddrv_stub);           // sceAddDrv
    runtime.registerFunction(0x00115A60u, g381_deldrv_stub);           // sceDelDrv
    runtime.registerFunction(0x00115A80u, g381_dopen_stub);            // sceDopen
    runtime.registerFunction(0x00115B48u, g381_dclose_stub);           // sceDclose
    runtime.registerFunction(0x00115CB0u, g381_dread_stub);            // sceDread
    runtime.registerFunction(0x00115E08u, g381_getstat_stub);          // sceGetstat
    runtime.registerFunction(0x00115FA8u, g381_chstat_stub);           // sceChstat
    runtime.registerFunction(0x001161E8u, g381_rename_stub);           // sceRename
    runtime.registerFunction(0x001163D8u, g381_chdir_stub);            // sceChdir
    runtime.registerFunction(0x001163F8u, g381_sync_stub);             // sceSync
    runtime.registerFunction(0x00116590u, g381_mount_stub);            // sceMount
    runtime.registerFunction(0x00116800u, g381_umount_stub);           // sceUmount
    runtime.registerFunction(0x00116820u, g381_lseek64_stub);          // sceLseek64
    runtime.registerFunction(0x00116C90u, g381_symlink_stub);          // sceSymlink
    runtime.registerFunction(0x00116E70u, g381_readlink_stub);         // sceReadlink
    runtime.registerFunction(0x00117708u, g381_sifstopmodule_stub);    // sceSifStopModule
    runtime.registerFunction(0x00118580u, g381_settlbentry_stub);      // SetTLBEntry
    runtime.registerFunction(0x001185C0u, g381_gettlbentry_stub);      // GetTLBEntry
    runtime.registerFunction(0x00118880u, g381_ktlbexception_stub);    // _kTLBException
    runtime.registerFunction(0x00118AC0u, g381_kdebugexception_stub);  // _kDebugException
    runtime.registerFunction(0x0011F418u, g381_cb_delayth_stub);       // CB_DelayTh
    runtime.registerFunction(0x0011F8F0u, g381_cdpoffcallback_stub);   // sceCdPOffCallback
    runtime.registerFunction(0x001230A8u, g381_mcudchecknewcard_stub); // sceMcUdCheckNewCard
    // Opt-in verification (DC2_G381_SELFTEST=1): the class is LATENT on every Graphic-Test
    // route (the HDD probe is gated behind sceDevctl's -11 and the rest have no live
    // callers), so "no crash" proves nothing by itself. Read the function map back and
    // assert each DEAD address now resolves to a handler that is NOT the recompiled
    // TODO_NAMED body — i.e. that the registration, not luck, removed the abort surface.
    if (dc2_env_flag_enabled("DC2_G381_SELFTEST"))
    {
        struct Entry { uint32_t addr; PS2Runtime::RecompiledFunction fn; const char *name; };
        static const Entry kEntries[] = {
            {0x0010C988u, g381_ch3dmacsc_stub, "_ch3dmaCSC"},
            {0x001107E0u, g381_sbrk_stub, "sbrk"},
            {0x001109D8u, g381_syncdcache_stub, "SyncDCache"},
            {0x00110B18u, g381_invaliddcache_stub, "InvalidDCache"},
            {0x00110BB0u, g381_queryintrcontext_stub, "QueryIntrContext"},
            {0x001111E8u, g381_isuspendthread_stub, "iSuspendThread"},
            {0x00113F48u, g381_powerooffhandler_stub, "scePowerOffHandler"},
            {0x00115158u, g381_ioctl2_stub, "sceIoctl2"},
            {0x001154E0u, g381_remove_stub, "sceRemove"},
            {0x00115500u, g381_mkdir_stub, "sceMkdir"},
            {0x001156B0u, g381_rmdir_stub, "sceRmdir"},
            {0x001156D0u, g381_format_stub, "sceFormat"},
            {0x00115940u, g381_adddrv_stub, "sceAddDrv"},
            {0x00115A60u, g381_deldrv_stub, "sceDelDrv"},
            {0x00115A80u, g381_dopen_stub, "sceDopen"},
            {0x00115B48u, g381_dclose_stub, "sceDclose"},
            {0x00115CB0u, g381_dread_stub, "sceDread"},
            {0x00115E08u, g381_getstat_stub, "sceGetstat"},
            {0x00115FA8u, g381_chstat_stub, "sceChstat"},
            {0x001161E8u, g381_rename_stub, "sceRename"},
            {0x001163D8u, g381_chdir_stub, "sceChdir"},
            {0x001163F8u, g381_sync_stub, "sceSync"},
            {0x00116590u, g381_mount_stub, "sceMount"},
            {0x00116800u, g381_umount_stub, "sceUmount"},
            {0x00116820u, g381_lseek64_stub, "sceLseek64"},
            {0x00116C90u, g381_symlink_stub, "sceSymlink"},
            {0x00116E70u, g381_readlink_stub, "sceReadlink"},
            {0x00117708u, g381_sifstopmodule_stub, "sceSifStopModule"},
            {0x00118580u, g381_settlbentry_stub, "SetTLBEntry"},
            {0x001185C0u, g381_gettlbentry_stub, "GetTLBEntry"},
            {0x00118880u, g381_ktlbexception_stub, "_kTLBException"},
            {0x00118AC0u, g381_kdebugexception_stub, "_kDebugException"},
            {0x0011F418u, g381_cb_delayth_stub, "CB_DelayTh"},
            {0x0011F8F0u, g381_cdpoffcallback_stub, "sceCdPOffCallback"},
            {0x001230A8u, g381_mcudchecknewcard_stub, "sceMcUdCheckNewCard"},
        };
        size_t bound = 0u;
        for (const auto &e : kEntries)
        {
            const bool present = runtime.hasFunction(e.addr);
            const bool match = present && runtime.lookupFunction(e.addr) == e.fn;
            if (match)
                ++bound;
            else
                std::fprintf(stderr, "[G381:selftest] FAIL %s@0x%08X present=%d\n",
                             e.name, e.addr, present ? 1 : 0);
        }
        std::fprintf(stderr, "[G381:selftest] %zu/%zu dead addresses bound to G381 handlers\n",
                     bound, sizeof(kEntries) / sizeof(kEntries[0]));
    }

    // sceDevctl@0x00116A58 is already repaired by the G380 registration in
    // apply_phase9_stubs.inc (ps2_stubs::sceDevctl, same -11 answer) — not duplicated here
    // so its DC2_G380_NO_DEVCTL lever keeps working independently.
}
