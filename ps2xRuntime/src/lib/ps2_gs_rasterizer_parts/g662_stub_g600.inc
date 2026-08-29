// ===== G662 P4: normal-build interface for the CLOSED G600 sky/texture probe ====================
//
// Same rule and the same mechanism as g662_stub_g590.inc and the g655_stub_* family.
//
// `g600_sky_probe.inc` is 252 lines / **6,897 code bytes / 24 COMDATs** in `ps2_gs_rasterizer.obj`
// (`tools/g654_tu_price.py`). Every entry point it exports is gated on an env flag that is OFF in
// every default and every performance run:
//
//   DC2_G600_TEX        -> g600TexOn()      -> g600NoteBigEntry / g600NoteTexHit
//   DC2_G600_TEXPAINT   -> g600TexPaintOn() -> the paint arm
//   DC2_G600_DIRECT     -> g600DirectOn()   -> g600NoteDisplayBatch
//   DC2_G600_TEXFILLTBP -> g600TexFillTbp() -> g600DumpDecoded's only effect
//
// ⚠️ SCOPE. `g600DirectOn`, `g600Seq`, `g600SeqOn` and `g600DisplayPubGenOn` are NOT in this file —
// they live in `rasterizer_gpu_alias_page_view.inc`, which is included EARLIER (ps2_gs_rasterizer.cpp
// line 407 vs 478) and is untouched by this stub. Only the sky/texture probe half is isolated.
//
// Restore with `-DPS2X_G655_LEGACY_DIAG=ON`.

static constexpr bool g600TexOn() { return false; }
static constexpr bool g600TexPaintOn() { return false; }
static void g600NoteDisplayBatch(uint32_t, uint32_t, int, int, bool, bool, bool, bool, bool) {}
static void g600NoteBigEntry(const G144Entry &, int, int, uint64_t, uint32_t, uint64_t, bool) {}
static void g600NoteTexHit(const G144Entry &, int, int, uint64_t, const char *) {}
static void g600DumpDecoded(const G144Entry &, int, int, uint64_t, std::vector<uint32_t> &) {}
