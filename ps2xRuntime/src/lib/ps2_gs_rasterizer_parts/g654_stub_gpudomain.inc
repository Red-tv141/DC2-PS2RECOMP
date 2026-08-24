// ===== G654 P5: THE G630 PERSISTENT-GS-DOMAIN FAMILY, COMPILED OUT ============================
//
// WHAT THIS REPLACES. `g630_gpu_domain.inc` + `g632_coherence_epoch.inc` +
// `g633_compiled_residency.inc` + `g634_raw_texture_view.inc` + `g636_authority_arbitration.inc` +
// `g636_arb_flags.inc` — the persistent GPU-resident GS domain prototype (G630–G636).
//
// WHY. PS2_PROJECT_STATE.md §3 records the family's disposition: **CLOSED NEGATIVE ON PERFORMANCE,
// DEFAULT-OFF.** G636 made it bit-exact and it is still +1.13 to +1.51 ms/f slower than control,
// so `g630DomainOn()` is false in every shipped run and every entry point below degenerates to a
// constant. But rule 12c (G653 §P25) measured that a runtime-`const bool`-guarded diagnostic
// **that never executes** still costs frame time, because MSVC's placement of everything ELSE in
// the translation unit changes: ~60 lines cost **+0.484 ms/f on `dragon`**, and no env rollback
// can recover it because no rollback removes a byte.
//
// `dumpbin /headers` on `ps2_gs_rasterizer.obj` prices this family at **121,285 bytes across 1,059
// COMDATs = 8.52% of the hot TU's machine code** (g630 47,937 · g633 27,137 · g629 25,394 ·
// g634 10,378 · g632 7,242 · g636 3,197). That is ~240x the size of the change that cost 0.484
// ms/f. Deleting the bytes is the only mechanism rule 12c leaves.
//
// EXACTNESS. Every stub returns exactly what the real function returns when the domain is off.
// Each is annotated with the line of the original that establishes it. The real family is restored
// verbatim by `-DPS2X_G654_GPUDOMAIN=ON`, which is how the two builds are A/B'd and pixel-gated.
//
//   real                                            domain-off value                (source)
//   g630DomainOn()                                  false                           g630:91  (needs DC2_G630_RESIDENT)
//   g630AnyDirty()                                  false                           g630:162 (g_g630GpuDirty never set)
//   g630MaterializeForRanges()                      returns immediately             g630:960 (!g630DomainOn())
//   g630PrepareCpuReplay()                          returns immediately             g630:990
//   g630MaterializeDepth()                          returns immediately             g630:767
//   g630NoteCpuReplay()                             no-op                           g630:1140
//   g630HaveSurfaces()                              false                           g630:1150 (g_g630Surfaces empty)
//   g630SurfaceFbW()                                0                               g630:1143
//   g630RangeHasGpuAuthority()                      false                           g630:1378
//   g630T8RejectMask()                              kG632T8Off                      g630:1411
//   g630TryBind()                                   0 (no bind)                     g630:1339 (G636 retired the mirror)
//   g630TryT8View()                                 0 (no view)                     g630:1435
//   g630BeginTexturePass()                          no-op                           g630:1369 (both callees gated)
//   g630CanPatchUpload()                            false                           g630:1069 -> mask kG632UpOff
//   g_g630UploadCompleteHook                        stays nullptr                   g630:1134 (installer excluded; the
//                                                                                   only call site null-checks it)
//   g632StatOn()/g632TexEpochOn()                   false                           g632:66-80 (env, both default-off)
//   g632NoteTexReject()/g632NoteG261InEpoch()       no-op                           g632:169, 268
//   G632EpochScope(on=false)                        inert                           g632:240
//   g633PlansOn()                                   false                           g633:25 (kG633PlansDefaultOn=false)
//   g633TexturePlanConst()                          nullptr                         g633:130 (plan vector empty)
//   g633TexturePlanHasAuthority()                   false                           g633:572
//   g633MaterializeTexturePlan()                    false                           g633:562 (no plan)
//   g633FinalizeTexturePass()                       no-op                           g633:1149
//   g_g633{GuestWrite,GuestCommit,AttachTexture}Hook stays nullptr                   g633:302 (installer is gated on
//                                                                                   s_g633Configured, false by default)
//   g634TryRawView()                                0                               g634:181 (!g634RawViewOn())
//   g634CanIngestUpload()                           false                           g634:386
//   g636SettleSurfaceBand()                         returns immediately             g630:797 (!g630DomainOn())
//   s_g636No*/s_g636Stat                            false                           g636_arb_flags (all env, default-off)
//
// The three `g_g633*Hook` pointers and `g_g630UploadCompleteHook` are DECLARED outside this family
// (rasterizer_command_graph.inc / rasterizer_headers_and_diagnostics.inc /
// rasterizer_rtt_census_and_waves.inc / ps2_gs_rasterizer.cpp) and only INSTALLED by it, so
// excluding the family simply leaves them null — which every call site already tests for.

// ---- the two reject-mask constants the surviving census sites still name ----------------------
enum : uint32_t
{
    kG632BindOff = 1u << 0,
    kG632T8Off   = 1u << 16,
};

// ---- G636 arbitration arms (all env, all default-off) -----------------------------------------
static constexpr bool s_g636NoFbSeed = false;
static constexpr bool s_g636NoMatPub = false;
static constexpr bool s_g636Stat = false;
static std::atomic<uint64_t> g_g636FbSeed{0u}, g_g636MatPub{0u};

// ---- G630 core --------------------------------------------------------------------------------
static constexpr bool g630DomainOn() { return false; }
static constexpr bool g630AnyDirty() { return false; }
static constexpr bool g630HaveSurfaces() { return false; }
static constexpr int g630SurfaceFbW(uint32_t) { return 0; }
static constexpr bool g630RangeHasGpuAuthority(const G144BlockRange &) { return false; }
static inline void g630MaterializeForRanges(const G144BlockRange &, const G144BlockRange &,
                                            bool, bool, int) {}
static inline void g630MaterializeDepth() {}
static inline void g630PrepareCpuReplay() {}
static inline void g630NoteCpuReplay() {}
static inline void g630BeginTexturePass() {}
static constexpr uint32_t g630T8RejectMask(const G144Entry &, const G178EntryState &,
                                           G144BlockRange *, G144BlockRange *)
{
    return kG632T8Off;
}
// ⚠️ The out-params matter for EXACTNESS even though nothing reads them in the default build: the
// masks are consumed only inside `if (g632StatOn())`, which is constexpr-false here. Write the same
// value the real gate writes anyway, so the stub is identical rather than merely equivalent —
// g630TryBind's `!s_g636LinView` early-out writes kG632BindOff (g630:1339) and g630TryT8View's
// reject writes the mask g630T8RejectMask returned, which with the domain off is kG632T8Off.
static inline uint32_t g630TryBind(uint32_t, const G144Entry &, const G178EntryState &,
                                   bool, uint32_t *outMask = nullptr)
{
    if (outMask) *outMask = kG632BindOff;
    return 0u;
}
static inline uint64_t g630TryT8View(const G144Entry &, const G178EntryState &,
                                     uint32_t *outMask = nullptr)
{
    if (outMask) *outMask = kG632T8Off;
    return 0ull;
}
static constexpr bool g630CanPatchUpload(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                         uint32_t, uint32_t, bool, bool census = false)
{
    return false;
}
static inline void g636SettleSurfaceBand(uint32_t, uint32_t, int, int, bool) {}

// ---- G632 coherence epochs --------------------------------------------------------------------
static constexpr bool g632StatOn() { return false; }
static constexpr bool g632TexEpochOn() { return false; }
static inline void g632NoteTexReject(uint32_t, uint32_t, bool, uint32_t) {}
static inline void g632NoteG261InEpoch(const G144BlockRange &) {}

// The real scope opens a page-union epoch when its first argument is true. It never is here, so
// the object is empty and both ctor and dtor fold away.
struct G632EpochScope
{
    G632EpochScope(bool, int) {}
};

// ---- G633 compiled residency ------------------------------------------------------------------
// The real plan carries the compiled texture/CLUT ranges. Nothing reaches a plan with the domain
// off (`g633TexturePlanConst` returns nullptr for every id), so the type only has to exist and
// carry the one member the surviving call site names.
struct G633TexturePlan
{
    G144BlockRange texRange{};
};
static constexpr bool g633PlansOn() { return false; }
static constexpr const G633TexturePlan *g633TexturePlanConst(uint32_t) { return nullptr; }
static constexpr bool g633TexturePlanHasAuthority(uint32_t) { return false; }
static constexpr bool g633MaterializeTexturePlan(uint32_t, int) { return false; }
static inline void g633FinalizeTexturePass() {}

// ---- G634 raw texture view --------------------------------------------------------------------
static constexpr uint32_t kG634Off = 0x0001u; // g634_raw_texture_view.inc:55
static inline uint64_t g634TryRawView(const G144Entry &, const G178EntryState &,
                                      uint32_t *outMask = nullptr)
{
    if (outMask) *outMask = kG634Off; // g634:183, the !g634RawViewOn() early-out
    return 0ull;
}
static constexpr bool g634CanIngestUpload(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                          uint32_t, uint32_t, bool)
{
    return false;
}
