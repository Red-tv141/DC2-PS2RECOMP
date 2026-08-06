// ==============================================================================================
// G512 — the whole-texture DECODE inner loop.
//
// WHAT G511 LEFT.  `[G511:texsplit]` decomposed the flush's largest pre-pass and found that 95.5%
// of `tex` (1.48 ms/f) is 28 heavy events per frame, not 9,960 graph entries: the whole-texture
// decode is 1.09 ms/f over 4.0 calls at 32.6 us each, on an average 81,601-px texture — ~26.6 ns
// of CPU per texel (~80 host cycles) WITH 8 row-pool lanes already running. Hashing the same VRAM
// in the same swizzle costs 0.33 ns/px. The decode was 80x more expensive per pixel than reading
// the same bytes, and that gap is the whole phase.
//
// WHERE THE 80 CYCLES WENT (G505's law: read the emitted code, not the source).  Disassembly of
// `decodeRows` — the `<lambda_1>` of g178DecodeWhole, image 0x1403db080, 0x260 bytes — showed the
// PER-TEXEL body doing all of this:
//
//   +0x50  mov rax,[rbx+8] / movzx edx,[rax]   <- reload `psm` through the closure POINTER
//   +0x57..+0x88                               <- re-run the 6-way psm switch, every texel
//   +0xa4/+0xca/+0xf8/+0x163/+0x1c3/+0x1ed     <- out-of-line CALL for the swizzled read.
//                                                 /GL + /LTCG did NOT inline it.
//   +0x13d/+0x200                              <- SECOND out-of-line call, g178LookupClutFixed,
//                                                 which re-derives a whole swizzled CLUT address
//                                                 (resolveClutIndex + ReadCT32/24/16/16S) per texel
//   +0x209  mov rax,[rbx] / movsxd rcx,[rax]   <- reload `texW` through TWO pointer chases ...
//   +0x20f  imul rcx,r14                       <- ... and a 64-bit multiply for vv*texW ...
//   +0x21d  mov rax,[rbx] / cmp edi,[rax]      <- ... then reload it AGAIN for the loop condition
//
// The reloads are not a missed optimisation the compiler could have made: the lambda captures
// `texW`/`psm` BY REFERENCE, and it stores through `uint32_t *dst`, so MSVC cannot prove the store
// does not alias the captured locals and must re-read them after every store. The switch cannot be
// unswitched for the same reason.
//
// WHAT THIS DOES.  Four changes, none of which alters a single output byte:
//   1. Every loop invariant becomes a VALUE (texW, psm, tbp0, tbw, the texa/texclut fields), so no
//      store can force a reload.
//   2. The psm switch is resolved ONCE per lane, outside both loops — one specialised body each.
//   3. The CLUT is materialised ONCE per decode into a 256-entry table, by calling the SAME
//      `g178LookupClutFixed` with the SAME arguments the per-texel path used. For a 256x256 T8
//      texture that is 65,536 swizzled CLUT address derivations replaced by 256.
//   4. The swizzled source read is batched to ONE call per ROW (GSMem::G512ReadRow*, which is
//      literally `PixelStorageTraits<psm>::Read` in a loop inside ps2_gs_memory.obj, so the page
//      table, block id and page stride are loop-invariant there and hoist).
//
// EXACTNESS.  Every leaf expression is the one the reference body used, in the same order:
//   * CT32/CT24 read through GSMem::ReadCT32 — what `readTexelPSMCT32` does — and apply the
//     `tbw == 0 -> 1` rule the same way. CT32's applyTexa is the identity, so that case is a
//     straight row copy. `readTexelPSMCT32` reads `gs->m_vram`, this reads the `vram` argument;
//     they are the SAME pointer — g178TryFlushGpu has exactly two call sites
//     (rasterizer_tilebin_capture.inc:187 and :353) and both pass `g_g144LastGs->m_vram` with
//     `g_g144LastGs` as the `gs` argument. Using the argument also keeps this a free function:
//     `GS::m_vram` is private with `friend class GSRasterizer`, so nothing in this anonymous
//     namespace may name it.
//   * T8 reads GSMem::ReadP8 with the entry's own pageBwT8 halving; T4/T4HL/T4HH read
//     P4/P4HL/P4HH with the entry's own psm and bw rule — the G178 nibble-plane fix, preserved.
//     T8H and any Z/unknown psm follow the reference's `default:` arm into P4 + CLUT (see
//     g512PsmUsesClut).
//   * applyTexa/decodePSMCT16 are inlined verbatim, not re-derived.
//
// NOT COVERED — CT16/CT16S.  `readTexelPSMCT16` picks its reader from
// `gs->activeContext().tex0.psm` — the LIVE GS register, not the entry's psm — and it has Z16/Z16S
// arms as well as CT16/CT16S. `GS::activeContext()` is PRIVATE (friend: GSRasterizer only), so a
// free function in this anonymous namespace cannot read it, and exposing it would mean editing
// runtime/ps2_gs_gpu.h — a 30+ hour full rebuild. Those two formats therefore keep the reference
// body. GSMem::G512ReadRowCT16/CT16S/Z16/Z16S exist and are exact, so the day a legal accessor
// appears this is a three-line change. Run DC2_G512_PSM=1 first: on lean MAP-0 the census decides
// whether that arm is worth anything at all.
// The reference body stays in g178DecodeWhole and DC2_G512_VERIFY=1 decodes both ways and counts
// mismatching texels. DC2_G298_VERIFY_TEXDECODE cannot serve as the oracle here: it compares the
// parallel decode against the SAME body, so it would agree with a wrong fast path.
// ==============================================================================================

// The GSMem::G512ReadRow* declarations live at TRUE GLOBAL SCOPE in
// rasterizer_headers_and_diagnostics.inc, beside GSMem::AddressP8 — this file is included from
// inside the anonymous namespace, where `namespace GSMem` would name `(anonymous)::GSMem` and bind
// to nothing (appendix-dc2-project.md §3).
//
// The A/B arm accessors and the census sink are DEFINED with EXTERNAL linkage in
// g419_ab_instrument.inc, which this TU includes AFTER rasterizer_command_graph.inc. This file is
// pulled in from INSIDE that file's anonymous namespace, so their forward declarations cannot live
// here: a declaration inside the unnamed namespace names a different, never-defined symbol
// (`anonymous namespace'::g512FastDecActive → LNK2019, then C2129 once marked static). They go in
// the GLOBAL-scope gap in rasterizer_headers_and_diagnostics.inc instead — the same treatment
// rasterizer_command_graph.inc:30-38 gives `int g476PageBumpArm();`, with the comment that names
// this exact trap.

// The row scratch is a stack array, so it is per-INVOCATION and therefore per-lane without any TLS
// access in the loop. GS texture width tops out at 1<<10; anything wider falls back to the
// reference body rather than being clamped.
constexpr int kG512MaxRowW = 1024;

// Everything the specialised bodies need, derived ONCE per decode.
struct G512DecodePlan
{
    bool usable = false;
    uint8_t psm = 0;
    uint32_t tbp0 = 0;
    uint32_t bwDirect = 0;    // readTexelPSMCT32/16 rule: tbw == 0 -> 1
    uint32_t bwT8 = 0;        // (tbw >> 1) ?: 1
    uint32_t bwT4 = 0;        // (tbw >> 1) ?: 1, plain T4 only
    uint32_t bwT4High = 0;    // T4HH/T4HL address in the C32 layout with the raw tbw
    uint8_t ta0 = 0;
    bool aem = false;
    const uint32_t *clut = nullptr;
};

// WHICH psm values the fast path may claim. The reference switch has four DIRECT arms (CT32, CT24,
// CT16, CT16S) and a `default:` that covers everything else; the fast switch reproduces three of
// them and its own `default:` covers the same "everything else". CT16/CT16S must therefore be
// excluded EXPLICITLY — they are not in the fast switch, so without this they would fall into its
// `default:` and be decoded as T4, which is a wrong-pixels bug, not a slow path. See the
// NOT COVERED note in this file's header for why they cannot be implemented here today.
static inline bool g512PsmFastable(uint8_t psm)
{
    return psm != GS_PSM_CT16 && psm != GS_PSM_CT16S;
}

// Materialised once per decode. 256 entries covers T8 directly and T4 (whose reader yields 0..15),
// so no branch is needed on the index width in the inner loop.
static inline void g512BuildClut(uint8_t *vram, const GSTexaReg &texa, const GSTexClutReg &texclut,
                                 const GSContext &ctx, uint8_t psm, uint32_t *clut)
{
    for (uint32_t i = 0; i < 256u; ++i)
        clut[i] = g178LookupClutFixed(vram, texa, texclut, static_cast<uint8_t>(i), ctx.tex0.cbp,
                                      ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, psm);
}

// Mirror of the reference switch, NOT of the PS2 format taxonomy. The reference routes EVERYTHING
// that is not one of its four direct arms — T8, T4, T4HL, T4HH, and also T8H and any Z/unknown psm
// that reaches here — through a CLUT lookup, because the stragglers land in its `default:` T4 arm.
// Testing for "is a palette format" instead would leave plan.clut null on exactly those stragglers
// and the fast path would dereference it. Match the switch, not the taxonomy.
// (CT16/CT16S are screened off by g512PsmFastable before this is ever consulted.)
static inline bool g512PsmUsesClut(uint8_t psm)
{
    return psm != GS_PSM_CT32 && psm != GS_PSM_CT24 && psm != GS_PSM_CT16 && psm != GS_PSM_CT16S;
}

// applyTexa's CT24 arm, inlined.
static inline uint32_t g512TexaCt24(uint32_t texel, uint8_t ta0, bool aem)
{
    const bool rgbZero = (texel & 0x00FFFFFFu) == 0u;
    const uint8_t a = (aem && rgbZero) ? 0u : ta0;
    return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
}

// One lane's slice of the texture. `dst` is the WHOLE texture buffer and rows are written at their
// absolute index, exactly as the reference body does.
static void g512DecodeRowsFast(uint8_t *vram, const G512DecodePlan &p, int texW,
                               int rowLo, int rowHi, uint32_t *dst)
{
    const uint32_t w = static_cast<uint32_t>(texW);
    const uint32_t *const clut = p.clut;
    const uint8_t ta0 = p.ta0;
    const bool aem = p.aem;
    const uint32_t tbp0 = p.tbp0;

    alignas(64) uint8_t rowRaw[kG512MaxRowW];

    switch (p.psm)
    {
    case GS_PSM_CT32:
        // applyTexa is the identity for CT32 — read the row straight into the destination.
        for (int vv = rowLo; vv <= rowHi; ++vv)
            GSMem::G512ReadRowCT32(vram, tbp0, p.bwDirect, static_cast<uint32_t>(vv), w,
                                   dst + static_cast<size_t>(vv) * w);
        break;

    case GS_PSM_CT24:
    {
        for (int vv = rowLo; vv <= rowHi; ++vv)
        {
            uint32_t *const out = dst + static_cast<size_t>(vv) * w;
            GSMem::G512ReadRowCT32(vram, tbp0, p.bwDirect, static_cast<uint32_t>(vv), w, out);
            for (uint32_t x = 0; x < w; ++x)
                out[x] = g512TexaCt24(out[x], ta0, aem);
        }
        break;
    }

    case GS_PSM_T8:
    {
        uint8_t *const src = rowRaw;
        for (int vv = rowLo; vv <= rowHi; ++vv)
        {
            GSMem::G512ReadRowP8(vram, tbp0, p.bwT8, static_cast<uint32_t>(vv), w, src);
            uint32_t *const out = dst + static_cast<size_t>(vv) * w;
            for (uint32_t x = 0; x < w; ++x)
                out[x] = clut[src[x]];
        }
        break;
    }

    default: // T4 / T4HL / T4HH
    {
        uint8_t *const src = rowRaw;
        for (int vv = rowLo; vv <= rowHi; ++vv)
        {
            const uint32_t y = static_cast<uint32_t>(vv);
            if (p.psm == GS_PSM_T4HH)
                GSMem::G512ReadRowP4HH(vram, tbp0, p.bwT4High, y, w, src);
            else if (p.psm == GS_PSM_T4HL)
                GSMem::G512ReadRowP4HL(vram, tbp0, p.bwT4High, y, w, src);
            else
                GSMem::G512ReadRowP4(vram, tbp0, p.bwT4, y, w, src);
            uint32_t *const out = dst + static_cast<size_t>(vv) * w;
            for (uint32_t x = 0; x < w; ++x)
                out[x] = clut[src[x]];
        }
        break;
    }
    }
}
