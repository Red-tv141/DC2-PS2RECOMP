// PHASE9: DC2 ג€” Game override: binds all 173 missing stub addresses to their handlers.
// Without this file the recomp wrappers call TODO_NAMED() (returns -1) instead of the
// actual stub, because the recompiler generated the wrappers before the stubs were added
// to PS2_STUB_LIST.  bindAddressHandler() re-points each guest address at the right
// ps2_stubs:: function at runtime, affecting only dc2.elf.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <atomic>
#include <functional>

extern void (*g_g7_poll_live_pad_hook)();
extern void (*g_g55_title_draw_probe_hook)(uint8_t *rdram);
extern bool (*g_f66_drive_dungeon_pad_hook)(uint32_t);

// G141: perf ns accumulators, defined in ps2_gs_rasterizer.cpp / ps2_vu1.cpp (external linkage).
extern std::atomic<uint64_t> g_g141GsRasterNs;
extern std::atomic<uint64_t> g_g141Vu1RunNs;
extern std::atomic<uint64_t> g_g146Vif1Ns;
extern std::atomic<uint64_t> g_g146GifSubmitNs;
extern std::atomic<uint64_t> g_g146GsImageNs;
extern std::atomic<uint64_t> g_g146GsLocalNs;
extern std::atomic<uint64_t> g_g146G144FlushMidNs;
extern std::atomic<uint64_t> g_g146G144FlushUploadNs;
extern std::atomic<uint64_t> g_g146G144FlushFrameNs;
extern std::atomic<uint64_t> g_g146G144FlushMidCount;
extern std::atomic<uint64_t> g_g146G144FlushUploadCount;
extern std::atomic<uint64_t> g_g146G144FlushFrameCount;
extern std::atomic<uint64_t> g_g147GsGifPacketNs;
extern std::atomic<uint64_t> g_g147GsGifPacketCount;
extern std::atomic<uint64_t> g_g147DrawPrimitiveNs;
extern std::atomic<uint64_t> g_g147DrawPrimitiveCount;
extern std::atomic<uint64_t> g_g147GifTags;
extern std::atomic<uint64_t> g_g147PackedRegs;
extern std::atomic<uint64_t> g_g147ReglistRegs;
extern std::atomic<uint64_t> g_g147ImageBytes;

void g7_poll_live_pad();
void g55_title_draw_probe(uint8_t *rdram);
bool f66_drive_dungeon_pad(uint32_t);

// PHASE F25: forward-decl for delegation when path is not the empty-stem map pattern.
// Defined in recomp/LoadFile2__FPcPvPii_0x149370.cpp.
extern void LoadFile2__FPcPvPii_0x149370(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleLoop__Fv_0x29ffa0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleModeInit__Fv_0x2a1020(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuInit__F13INIT_LOOP_ARG_0x191970(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuLoop__Fv_0x191c30(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuMainLoop__Fv_0x233fc0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void PauseLoop__Fv_0x309de0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleModeKey__Fv_0x2a1220(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleMCCheckKey__Fv_0x2a2ad0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Step__18CMemoryCardManagerFv_0x2f1fc0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuCheckPushButton__Fv_0x23e1b0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void ConvertCheckPushButton__Fi_0x23e2e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuMainInit__FP13MENU_INIT_ARG_0x232df0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuMainKey__Fv_0x233ff0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuMainDraw__Fv_0x234290(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuCostumeDraw__Fv_0x2be040(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void FinishForMC__18CMemoryCardManagerFv_0x2f19a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void InitSaveData__Fv_0x1908a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void LoadFileMenu__FPcP1i_0x251100(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void EnterIMGFile__17mgCTextureManagerFPUciP9mgCMemoryP15mgCEnterIMGInfo_0x12da90(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void ReloadTexture__17mgCTextureManagerFiP13sceVif1Packet_0x12e850(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void GetUserDataMan__Fv_0x196be0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetChrEquipDirect__16CUserDataManagerFii_0x19d560(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DebugGetItem__FP16CUserDataManageri_0x1a1b80(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void GetCharaDataPtr__16CUserDataManagerFi_0x19b490(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void GetItemFilePath__Fii_0x195d40(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetMenuLoadItemNo__Fi_0x2afdb0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void UpDate__8CGamePadFv_0x14a930(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Update__11CPadControlFP8CGamePad_0x2ed550(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void sndStep__Ff_0x18d650(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void StepSnd__6CSceneFv_0x2a7940(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleDraw__Fv_0x2a0ab0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleModeDraw__Fv_0x2a1b60(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleMapDraw__Fv_0x2a2280(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G57: scoped back-edge preemption suppression (defined in ps2_runtime.cpp). Held > 0
// while the title-frame draw runs so the recompiled deferred-draw bodies, which the
// title override invokes as direct C++ calls, run to completion instead of leaking a
// mid-loop preemption resume PC (which left BeginDraw's mgr[4]/mgr[6] null ג†’ flush crash).
extern std::atomic<int> g_dc2PreemptSuppressDepth;
// G56: main-title map geometry-submission chain (delegated by the G56 chain taps).
extern void Draw__9CMapPartsFv_0x15e3d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void PreDraw__9CMapPartsFv_0x166a00(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Draw__9CMapPieceFv_0x166e40(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void BeginDraw__14mgCDrawManagerFP9mgCMemoryPi_0x135230(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G57: DrawWater (last call in TitleMapDraw) ג€” wrapped by g57_drawwater_skip bisection tool.
extern void DrawWater__4CMapFP9mgCCameraP10mgCTextureP10mgCTexture_0x15e800(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G129: SPI map-config WATER_VERTEX command handler ג€” creates a CWaterFrame (CreateWaterFrame@0x185D40)
// and stores it into the global `cfgWater`. The title water arrays (CMap+0xcec/+0xcf0) are empty on
// the runner; this probe tells whether the title map's config script dispatches WATER_VERTEX at all.
extern void cfgWATER_VERTEX__FP9SPI_STACKi_0x1648f0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G58: remaining links of the title geometry-queue chain (wrapped by the g58 $ra-canary).
extern void DrawSub__8CEditMapFi_0x1b4130(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DrawSub__4CMapFi_0x15e250(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DrawSub__9CMapPartsFi_0x166a70(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Draw__12CObjectFrameFv_0x169fd0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G58: title camera-follow accessors (the crash trace ends AddHeight x3 -> 0x9f84a0).
extern void AddDistance__15mgCCameraFollowFf_0x131a20(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void GetDistance__15mgCCameraFollowFv_0x131a10(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void AddHeight__15mgCCameraFollowFf_0x131a50(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G58: title camera assignment (TitleInit calls this 2x to put the camera in scene slot 0/1).
extern void AssignCamera__6CSceneFiP9mgCCameraPc_0x283740(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G58: GetCamera__6CScene ג€” wrapped to assign the title camera on demand (ordering fix).
extern void GetCamera__6CSceneFi_0x2838c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G59: title MDS texture-block return probe.
extern void GetTextureBlockNo__11CMdsListSetFiPii_0x168fd0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgDraw__FP8mgCFrame_0x142f90(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuItemCharaDataLoad__FP9mgCMemoryiPP17MENU_BGREAD_INFO2i_0x2b90d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void ReadBGSync__Fv_0x148e70(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void ReadMainCharaBG__Fv_0x2bbc80(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void KeyMainCharaBG__Fv_0x2bc150(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void MenuCostumeKey__Fv_0x2be030(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void InitDungeonMain__F13INIT_LOOP_ARG_0x1cc040(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void LoopDungeonMain__Fv_0x1cea00(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Step__12CMenuTreeMapFv_0x1eff40(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleExit__Fv_0x29ff30(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CaptureEnd__8CGamePadFv_0x14b600(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void PrimQuad__FP11mgCDrawPrimff9mgRect_i__0x21fe60(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void PrimQuad__FP10mgCTextureff9mgRect_i_iiii_0x21ff30(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CreateMap__4CMapFP11CMdsListSetP9mgCMemory_0x1600d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void LoadMapFile__4CMapFPciP9mgCMemoryi_0x164480(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void LoadMapFromMemory__6CSceneFiP17SCN_LOADMAP_INFO2_0x285670(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void LoadMapFromMemory__6CSceneFiiP17SCN_LOADMAP_INFO2_0x2856f0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void LoadMap__6CSceneFiP17SCN_LOADMAP_INFO2i_0x285ce0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetScale__10CFuncPointFPf_0x1643a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetRotation__10CFuncPointFPf_0x1643c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetPosition__10CFuncPointFPf_0x1643e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetScale__9mgCObjectFPf_0x136340(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetRotation__9mgCObjectFPf_0x136270(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetPosition__9mgCObjectFPf_0x136190(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void ActiveLighting__13mgRENDER_INFOFii_0x139120(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Begin__11mgCDrawPrimFi_0x1344a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void End__11mgCDrawPrimFv_0x134690(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Color__11mgCDrawPrimFiiii_0x134c80(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Texture__11mgCDrawPrimFP10mgCTexture_0x134da0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DirectData__11mgCDrawPrimFi_0x134b00(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void EndPrim2__11mgCDrawPrimFv_0x134940(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgEndFrame__FP14mgCDrawManager_0x1425b0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// PHASE G15: costume/character deform-mesh build path (split VU1-dormant vs packet-not-built).
extern void mgDrawDirect__FP8mgCFrame_0x142fd0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DrawDirect__12CActionCharaFv_0x16b940(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DrawDirect__11CCharacter2Fv_0x1731f0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgGetDrawRect__FP8mgCFrameP9mgVu0FBOX_0x143160(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Draw__8mgCFrameFPUi_0x137e10(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetDeformMesh__11CCharacter2Fv_0x1730b0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// PHASE G34: costume model RTT->display composite (the outline/preview pass).
extern void Draw__12COutLineDrawFff_0x17c2d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DrawDivSprite4__FP11mgCDrawPrim9mgRect_i_P10mgCTexturePiii_0x17cb20(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgClipBoxW__FPfPfPfPf_0x12f2e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgClipInBoxW__FPfPfPfPf_0x12f380(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void test1__FPA4_fPA4_fPA4_fPfPf_0x135c70(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgSendVuProg__FPUii_0x145e80(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void AddPacket__14mgCDrawManagerFiP1P1i_0x1359d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CreateFacePacket__12mgCVisualMDTFPUiP7mgCFace_0x13ff60(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CreateRenderInfoPacket__12mgCVisualMDTFPUiPA4_fP13mgRENDER_INFO_0x1404d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CalcGlidPutPos__11CDngFreeMapFP9GLID_INFORfRfi_0x1ea890(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void InitEnd__12CMenuTreeMapFv_0x1ef9f0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetTextureInfo__11CDngFreeMapFv_0x1eabe0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void GetPackFile__FPUiPcPi_0x149cd0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void GetReadBGFile__Fi_0x148c70(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void LoadFileBG__FPcP1Pi_0x148930(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void InitReadBG__Fv_0x1488d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void StartReadBG__Fv_0x148cc0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void WaitVSync__Fii_0x1412a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void VSyncCallBack__Fi_0x141200(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void sceGsSyncVCallback_0x1041e8(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void sceGsSyncV_0x103300(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgEndDrawReloadTexture__FiP14mgCDrawManager_0x142560(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgEndDraw__FiP14mgCDrawManager_0x142580(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Draw__14mgCDrawManagerFiP13sceVif1Packet_0x135720(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// PHASE F50.9: dungeon CLUT/texture upload probe. mgLoadImage builds the VIF1
// BITBLTBUF+IMAGE packet for both texture pixels and (via ReloadCLUT) the palette.
extern void mgLoadImage__FPUiiiiP1iiiii_0x12e600(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void ReloadTexture__17mgCTextureManagerFiP13sceVif1Packet_0x12e850(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// PHASE F50.10: dungeon texture-manager reload. ReloadTexture rewrites each texture's
// sceGsTex0 (struct+0x38) TBP0/CBP to the current VRAM allocation, but only uploads
// pixels when the mip-0 data ptr (+0x50) is non-zero and the CLUT when (+0x60) is
// non-zero. We log the post-reload TEX0 vs the data/CLUT pointers to see whether the
// dungeon's sampled texture (tbp=0x2580/cbp=0x2980) has a null data/CLUT source.
extern void ReloadTexture__17mgCTextureManagerFiPUi_0x12e970(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// PHASE F50.11: mgCDrawPrim::Texture(drawprim, texEnv) copies the texEnv's sceGsTex0
// (texEnv+0) into the draw packet. This is where the dungeon geometry's TEX0 (tbp=0x2580
// cbp=0x2980 PSMCT16) is bound. We capture the source texEnv pointer + its TEX0 so we can
// identify which texture/manager owns the empty 0x2580 page.
extern void Texture__11mgCDrawPrimFP10mgCTexture_0x134da0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// PHASE F50.11: GetTexture(mgr, name, idx) returns an mgCTexture (sceGsTex0 at +0x38).
// Identifies which manager + texture name owns the empty 0x2580 page.
extern void GetTexture__17mgCTextureManagerFPci_0x12d050(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);

namespace ps2_stubs
{
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();
    void sceDevctl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime); // Kernel/Stubs/FileIO.cpp: no-HDD fail (-11)
}

// PHASE G34: set while inside DrawDivSprite4 (the model RTT->display composite) so the GS
// rasterizer can tag which 0x2720-sampling draws are the composite vs the brick background.
extern std::atomic<int> g_g34_in_divsprite;

// PHASE G37: set while inside Draw__12COutLineDraw (the costume preview outline+composite
// path) so the mgGetDrawRect wrapper only repairs the model bounding box for THIS draw,
// not for any other COutLineDraw user.
static std::atomic<int> g_g37_in_outline{0};

// G89: defined in ps2_gs_rasterizer.cpp; set by g67_title_scope each frame so the rasterizer's
// title-rock guard-band cull only fires in the title-map scene. Declared at global scope (external
// linkage) -- declaring it inside the anonymous namespace below would give it internal linkage.
extern std::atomic<bool> g_dc2TitleRockScope;
// G90: current logical title block flushing through mgEndDraw (rasterizer-side, [G88:geo] tag).
extern std::atomic<int> g_dc2TitleCurBlock;
// G144: defined in ps2_gs_rasterizer.cpp; drains trailing deferred tile-binning triangles at frame
// end. Declared at global scope (external linkage) — a local extern inside the anon namespace below
// would bind to an anon-namespace symbol (internal linkage) and fail to link.
extern void g144FlushPending();
// G150 MTGS (multi-threaded GS): defined in ps2_gif_arbiter.cpp. When DC2_G150_MTGS=1 the GS drain
// runs on a dedicated worker thread; the present latch must therefore run on that worker AFTER the
// frame's draws (g150_frame_barrier also applies the ≤1-frame pipeline backpressure). When MTGS is
// off, g150_frame_barrier runs the callback inline (byte-identical to the pre-G150 latch path).
extern bool g150_mtgs_enabled();
extern void g150_frame_barrier(std::function<void()> latch);
extern void g150_wait_idle();

namespace
{

static uint32_t dc2_read_u32(uint8_t *rdram, uint32_t addr);
static uint64_t dc2_read_u64(uint8_t *rdram, uint32_t addr);
static void dc2_write_u32(uint8_t *rdram, uint32_t addr, uint32_t value);
static void dc2_write_u16(uint8_t *rdram, uint32_t addr, uint16_t value);
static inline void f50_set_reg(R5900Context *ctx, int reg, uint32_t value);

static uint32_t g_f40_frame_counter = 0u;
static bool     g_f40_active        = false;
static uint16_t g_f40_pressed       = 0u;
// F66: in-dungeon free-roam input. The live pad path is pad_button_read_stub
// (override of 0x14a3d0), which the title input (g_f40_*) drives ג€” but that clock
// freezes in-dungeon and the stub hardcodes the analog sticks to centre. The
// present-loop driver f66_drive_dungeon_pad sets these so pad_button_read_stub can
// emit the held buttons + a deflected LEFT STICK (free-roam reads the analog stick).
static bool     g_f66_dungeon_active = false;
static uint16_t g_f66_dungeon_buttons= 0u;
static uint8_t  g_f66_dungeon_lx     = 0x80u;
static uint8_t  g_f66_dungeon_ly     = 0x80u;
// G7: live host controller (XInput via raylib). Snapshot published once per host
// present frame by g7_poll_live_pad (main thread); consumed by the guest pad read
// (read_pad_stub / dc2_pad_mask / dc2_write_pad_status) on the EE thread. When
// g_pad_live_connected, the live controller OWNS input (buttons + all four analog
// axes), overriding the scripted F40/F66 paths. Disabled when DC2_PAD_INPUT is set
// explicitly (deterministic tests) or DC2_NO_XINPUT=1. Marker: G7_XINPUT_LIVE.
static bool     g_pad_live_connected = false;
static uint16_t g_pad_live_mask      = 0u;     // active-high scePad bits
static uint8_t  g_pad_live_lx        = 0x80u;
static uint8_t  g_pad_live_ly        = 0x80u;
static uint8_t  g_pad_live_rx        = 0x80u;
static uint8_t  g_pad_live_ry        = 0x80u;
// G49: scripted RIGHT-STICK deflection for HEADLESS validation. The Select-Costume model
// preview rotates with the RIGHT analog stick (the right stick is hardcoded to centre 0x80
// in the headless pad path, so DC2_PAD_INPUT's digital buttons can navigate menus but cannot
// turn the model). Env DC2_RSTICK uses the same "start..end:Dir[+Dir];..." range syntax as
// DC2_PAD_INPUT, with Dir in {RLeft,RRight,RUp,RDown}; driven off the F40 scriptFrame path and
// applied in dc2_write_pad_status when no live pad is connected. Default 0x80 => inert.
static uint8_t  g_f40_rx             = 0x80u;
static uint8_t  g_f40_ry             = 0x80u;
static uint32_t g_f48_2_log_count   = 0u;
static uint32_t g_f48_3_log_count   = 0u;
static bool dc2_env_flag_enabled(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr &&
           value[0] != '\0' &&
           std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0 &&
           std::strcmp(value, "off") != 0 &&
           std::strcmp(value, "OFF") != 0;
}

static bool f33_trace_drawprim_enabled()
{
    static const bool enabled = []() {
        return dc2_env_flag_enabled("DC2_TRACE_DRAWPRIM");
    }();
    return enabled;
}

static bool dc2_phase_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_PHASE_TRACE");
    return enabled;
}

static bool f39_trace_map_entry_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_MAP_ENTRY");
    return enabled;
}

static bool f50_6_debug_menu_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_DEBUG_MENU");
    return enabled;
}

static bool f50_7_trace_selected_map_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_7");
    return enabled;
}

static bool f50_9_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_9");
    return enabled;
}

static bool f50_10_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_10");
    return enabled;
}

static bool f50_11_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_11");
    return enabled;
}

static bool f51_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_T8_UPLOAD");
    return enabled;
}

static bool f51_should_log()
{
    static uint32_t s_logs = 0u;
    if (s_logs >= 96u)
        return false;
    ++s_logs;
    return true;
}

static bool g59_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G59");
    return enabled;
}

static bool f48_trace_title_camera_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_TITLE_CAMERA");
    return enabled;
}

static bool f49_trace_waitvsync_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_WAITVSYNC");
    return enabled;
}

static bool f49_trace_vsync_callback_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_VSYNC_CB");
    return enabled;
}

static bool f49_trace_syncv_set_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_SYNCVCB_SET");
    return enabled;
}

static bool f49_trace_syncv_call_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_SYNCV_CALL");
    return enabled;
}

static bool f49_trace_loop_timing_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_LOOP_TIMING");
    return enabled;
}

static bool f49_trace_mainloop_hotspots_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_MAINLOOP_HOT");
    return enabled;
}

static bool f49_trace_title_path_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_TITLE_PATH");
    return enabled;
}

static bool f47_trace_menu_state_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_MENU_STATE");
    return enabled;
}

static bool f48_2_trace_charbg_enabled()
{
    static const bool enabled =
        dc2_env_flag_enabled("DC2_TRACE_F48_2") ||
        dc2_env_flag_enabled("DC2_TRACE_MENU_STATE");
    return enabled;
}

static bool f48_3_trace_costume_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F48_3");
    return enabled;
}

static bool f49_remap_title_ret5_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_F49_REMAP_RET5");
    return enabled;
}

static int32_t f49_waitvsync_env_max_count()
{
    static const int32_t parsed = []() -> int32_t
    {
        const char *value = std::getenv("DC2_WAITVSYNC_MAX_COUNT");
        if (value == nullptr || *value == '\0')
            return -1;

        char *end = nullptr;
        const long v = std::strtol(value, &end, 10);
        if (end == value || (end && *end != '\0') || v < 0L || v > 1024L)
            return -1;
        return static_cast<int32_t>(v);
    }();
    return parsed;
}

static bool f29_should_log(uint32_t n, uint32_t step = 60u)
{
    if (!dc2_phase_trace_enabled())
        return false;
    return n <= 12u || (step != 0u && (n % step) == 0u);
}

// PHASE9: DC2 ג€” placement operator new: return $a1 (the pointer argument)
static void reta1_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnU32(ctx, getRegU32(ctx, 5)); // $v0 = $a1
    ctx->pc = getRegU32(ctx, 31);         // return to caller
}

// PHASE9: DC2 ג€” safe no-op for unrecompiled game functions (return 0, advance PC)
static void nop_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnU32(ctx, 0u);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F3: DC2 ג€” SCElogoFade shim. Game polls "fade complete?" in MainLoop;
// boot path. Keep the decorative title map draw inert so the title handoff can
// proceed to menu initialization.
static void title_map_draw_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnU32(ctx, 0u);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F50.2: sndLoadSound @0x18DA30 headless stub. The real recompiled body
// (de-stubbed in the F50.2 auto-stub->real batch) issues a SIF RPC to the IOP
// sound module and busy-waits for completion; with no IOP/SPU2 the wait never
// returns, hanging InitDungeonMain (caller @0x1CDDF8) forever. This function was
// an auto-stub before F50.2 for every caller (title path included), so returning
// 0 here reverts to the known-good headless behavior and lets dungeon init proceed.
static uint32_t g_f50_2_snd_load_log = 0u;
static void f50_2_snd_load_sound_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f50_2_snd_load_log;
    if (dc2_env_flag_enabled("DC2_TRACE_F50_1") && n <= 8u)
    {
        std::fprintf(stderr,
                     "[F50.2:sndLoadSound] n=%u ra=0x%x a0=0x%x a1=0x%x a2=0x%x (headless no-op)\n",
                     n, getRegU32(ctx, 31), getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6));
    }
    setReturnU32(ctx, 0u);
    ctx->pc = getRegU32(ctx, 31);
}

static uint32_t f51_guest_nonzero_prefix(uint8_t *rdram, uint32_t addr, uint32_t limit)
{
    if (addr == 0u)
        return 0u;

    uint32_t nonzero = 0u;
    const uint32_t count = (limit > 256u) ? 256u : limit;
    for (uint32_t i = 0u; i < count; ++i)
    {
        if (rdram[(addr + i) & 0x1fffffffu] != 0u)
            ++nonzero;
    }
    return nonzero;
}

static void f51_decode_tex0(uint64_t tex0,
                            uint32_t *tbp,
                            uint32_t *tbw,
                            uint32_t *psm,
                            uint32_t *cbp,
                            uint32_t *cpsm)
{
    if (tbp)  *tbp  = static_cast<uint32_t>(tex0 & 0x3fffull);
    if (tbw)  *tbw  = static_cast<uint32_t>((tex0 >> 14) & 0x3full);
    if (psm)  *psm  = static_cast<uint32_t>((tex0 >> 20) & 0x3full);
    if (cbp)  *cbp  = static_cast<uint32_t>((tex0 >> 37) & 0x3fffull);
    if (cpsm) *cpsm = static_cast<uint32_t>((tex0 >> 51) & 0x0full);
}

static bool f51_target_like_tex0(uint64_t tex0)
{
    uint32_t tbp = 0u, psm = 0u, cbp = 0u;
    f51_decode_tex0(tex0, &tbp, nullptr, &psm, &cbp, nullptr);
    return tbp == 0x2720u || tbp == 0x2820u || tbp == 0x29a0u ||
           tbp == 0x2f20u || cbp == 0x3fbcu || psm == 0x13u;
}

static void f51_log_texture_chain(uint8_t *rdram, uint32_t call, const char *stage, uint32_t listHead)
{
    uint32_t node = listHead;
    uint32_t guard = 0u;
    while (node != 0u && guard < 16u)
    {
        const uint64_t tex0 = dc2_read_u64(rdram, node + 0x38u);
        uint32_t tbp = 0u, tbw = 0u, psm = 0u, cbp = 0u, cpsm = 0u;
        f51_decode_tex0(tex0, &tbp, &tbw, &psm, &cbp, &cpsm);
        const uint32_t dims = dc2_read_u32(rdram, node);
        const uint32_t width = (dims >> 16) & 0xffffu;
        const uint32_t height = dims & 0xffffu;
        const uint32_t bitdepth = dc2_read_u32(rdram, node + 0x04u) >> 16;
        const uint32_t areaBlocks = dc2_read_u32(rdram, node + 0x28u);
        const uint32_t pix0 = dc2_read_u32(rdram, node + 0x50u);
        const uint32_t pix1 = dc2_read_u32(rdram, node + 0x54u);
        const uint32_t clut = dc2_read_u32(rdram, node + 0x60u);
        const uint32_t next = dc2_read_u32(rdram, node + 0x68u);
        const bool target = f51_target_like_tex0(tex0) || pix0 != 0u || clut != 0u;
        if ((guard < 8u || target) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:tex] call=%u stage=%s node=0x%x guard=%u wh=%ux%u bitdepth=%u area=0x%x "
                         "tex0=0x%016llx tbp=0x%x tbw=%u psm=0x%x cbp=0x%x cpsm=0x%x pix0=0x%x pix1=0x%x clut=0x%x next=0x%x\n",
                         call, stage, node, guard, width, height, bitdepth, areaBlocks,
                         static_cast<unsigned long long>(tex0),
                         tbp, tbw, psm, cbp, cpsm, pix0, pix1, clut, next);
        }
        node = next;
        ++guard;
    }
}

static void f51_mgenddraw_reload_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f51_trace_enabled())
    {
        static uint32_t s_call = 0u;
        const uint32_t call = ++s_call;
        if (call <= 64u && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:enddraw] call=%u a0=0x%x a1=0x%x a2=0x%x a3=0x%x ra=0x%x\n",
                         call, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6),
                         getRegU32(ctx, 7), getRegU32(ctx, 31));
        }
    }
    mgEndDrawReloadTexture__FiP14mgCDrawManager_0x142560(rdram, ctx, runtime);
}

static void f51_reload_texture_packet_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f51_trace_enabled())
    {
        static uint32_t s_call = 0u;
        const uint32_t call = ++s_call;
        const uint32_t mgr = getRegU32(ctx, 4);
        const uint32_t setIdx = getRegU32(ctx, 5);
        const uint32_t packet = getRegU32(ctx, 6);
        const uint32_t curSet = mgr ? dc2_read_u32(rdram, mgr + 0x08u) : 0u;
        const uint32_t count = mgr ? dc2_read_u32(rdram, mgr + 0x0cu) : 0u;
        const uint32_t table = mgr ? dc2_read_u32(rdram, mgr + 0x10u) : 0u;
        const uint32_t listHead = (mgr && table != 0u && setIdx < count) ?
            dc2_read_u32(rdram, table + setIdx * 0x10u + 8u) : 0u;
        if ((call <= 96u || listHead != 0u) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:reloadpkt] call=%u mgr=0x%x set=%u packet=0x%x curset=%u count=%u base0=0x%x clutbase=0x%x table=0x%x list=0x%x ra=0x%x\n",
                         call, mgr, setIdx, packet, curSet, count,
                         mgr ? dc2_read_u32(rdram, mgr + 0x00u) : 0u,
                         mgr ? dc2_read_u32(rdram, mgr + 0x04u) : 0u,
                         table, listHead, getRegU32(ctx, 31));
        }
    }

    ReloadTexture__17mgCTextureManagerFiP13sceVif1Packet_0x12e850(rdram, ctx, runtime);
}

// PHASE F50.9: mgLoadImage probe. mgLoadImage@0x12e600 builds the VIF1
// BITBLTBUF+IMAGE upload packet for BOTH the texture pixels (large transfers)
// and, via ReloadCLUT@0x12ed90, the palette (CLUT) for paletted textures.
// EABI args: a0=dst($4) a1=DBP($5) a2=DPSM($6) a3=DBW($7) a4/$t0=src($8)
// a5/$t1=param6/qwc($9). The dungeon CLUT shows DBW=1, small qwc (4 or 0x40),
// DPSM=CPSM (PSMCT16=0x2), DBP=CBP (~0x2980). F50.8 found ZERO dpsm=0x2 reach GS,
// so this confirms whether the game even issues the dungeon CLUT upload.
// Always delegates to the real recomp body; logging is gated by DC2_TRACE_F50_9.
static void f50_9_mg_load_image_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f51_trace_enabled())
    {
        const uint32_t dst = getRegU32(ctx, 4);
        const uint32_t dbp = getRegU32(ctx, 5);
        const uint32_t dpsm = getRegU32(ctx, 6);
        const uint32_t dbw = getRegU32(ctx, 7);
        const uint32_t src = getRegU32(ctx, 8);
        const uint32_t qwc = getRegU32(ctx, 9);
        const uint32_t dsax = getRegU32(ctx, 10);
        const uint32_t dsay = getRegU32(ctx, 11);
        const uint32_t sp = getRegU32(ctx, 29);
        const uint32_t rrw = dc2_read_u32(rdram, sp + 0u);
        const uint32_t rrh = dc2_read_u32(rdram, sp + 8u);
        const bool hit = (dbp == 0x2720u) || (dpsm == 0x13u) ||
                         (dbp >= 0x2700u && dbp <= 0x3500u) ||
                         (dbp >= 0x3fb0u && dbp <= 0x3fe8u);
        static uint32_t s_call = 0u;
        const uint32_t call = ++s_call;
        if ((call <= 160u || hit) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:mgli] call=%u hit=%u dst=0x%x dbp=0x%x byte=0x%x dpsm=0x%x dbw=%u src=0x%x qwc=%u ds=(%u,%u) rr=(%u,%u) srcNz256=%u srcDw0=0x%x ra=0x%x\n",
                         call, hit ? 1u : 0u, dst, dbp, dbp * 256u, dpsm, dbw,
                         src, qwc, dsax, dsay, rrw, rrh,
                         f51_guest_nonzero_prefix(rdram, src, 256u),
                         src ? dc2_read_u32(rdram, src) : 0u,
                         getRegU32(ctx, 31));
        }
    }

    if (f50_9_trace_enabled())
    {
        const uint32_t dbp  = getRegU32(ctx, 5);
        const uint32_t dpsm = getRegU32(ctx, 6);
        const uint32_t dbw  = getRegU32(ctx, 7);
        const uint32_t src  = getRegU32(ctx, 8);
        const uint32_t qwc  = getRegU32(ctx, 9);
        const uint32_t ra   = getRegU32(ctx, 31);
        // CLUT-shaped: DBW==1 and small qwc. Also force-log any PSMCT16 dest.
        const bool clutShaped = (dbw == 1u) || (dpsm == 0x2u) || (dpsm == 0xau);
        static uint32_t s_all = 0u;
        static uint32_t s_clut = 0u;
        const uint32_t n = ++s_all;
        const uint32_t c = clutShaped ? (++s_clut) : 0u;
        if (n <= 96u || (clutShaped && c <= 64u) || (n % 512u) == 0u)
        {
            std::fprintf(stderr,
                         "[F509:mgli] n=%u %s dbp=0x%x(blk*256=0x%x) dpsm=0x%x dbw=%u qwc=%u src=0x%x ra=0x%x\n",
                         n, clutShaped ? "CLUT?" : "tex", dbp, dbp * 256u, dpsm, dbw, qwc, src, ra);
        }
    }
    mgLoadImage__FPUiiiiP1iiiii_0x12e600(rdram, ctx, runtime);
}

static void f50_10_reload_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t mgr    = getRegU32(ctx, 4); // param_1: mgCTextureManager*
    const uint32_t setIdx = getRegU32(ctx, 5); // param_2: texture-set index
    const uint32_t packet = getRegU32(ctx, 6); // param_3: output VIF/GIF packet ptr; 0 rewrites TEX0 only.
    bool willReload = false;
    uint32_t m0 = 0u, m1 = 0u, m2 = 0u, m3 = 0u, m4 = 0u;
    if ((f50_10_trace_enabled() || f51_trace_enabled()) && mgr != 0u)
    {
        m0 = dc2_read_u32(rdram, mgr + 0x00u);  // texture VRAM base (iVar2 seed)
        m1 = dc2_read_u32(rdram, mgr + 0x04u);  // CLUT VRAM base (iVar5 seed)
        m2 = dc2_read_u32(rdram, mgr + 0x08u);  // currently-loaded set
        m3 = dc2_read_u32(rdram, mgr + 0x0Cu);  // texture-set count
        m4 = dc2_read_u32(rdram, mgr + 0x10u);  // set table base
        willReload = (m2 != setIdx) && ((int32_t)setIdx >= 0) && ((int32_t)setIdx < (int32_t)m3);
    }

    uint32_t f51Call = 0u;
    uint32_t f51ListHead = 0u;
    if (f51_trace_enabled() && mgr != 0u)
    {
        static uint32_t s_call = 0u;
        f51Call = ++s_call;
        if (m4 != 0u && setIdx < m3)
            f51ListHead = dc2_read_u32(rdram, setIdx * 0x10u + m4 + 8u);
        if ((f51Call <= 96u || f51ListHead != 0u || setIdx == 1u) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:reloadraw-in] call=%u mgr=0x%x set=%u packet=0x%x curset=%u count=%u base0=0x%x clutbase=0x%x table=0x%x list=0x%x willReload=%u ra=0x%x\n",
                         f51Call, mgr, setIdx, packet, m2, m3, m0, m1, m4,
                         f51ListHead, willReload ? 1u : 0u, getRegU32(ctx, 31));
            if (f51ListHead != 0u)
                f51_log_texture_chain(rdram, f51Call, "pre", f51ListHead);
        }
    }

    ReloadTexture__17mgCTextureManagerFiPUi_0x12e970(rdram, ctx, runtime);

    if (f51_trace_enabled() && mgr != 0u && f51Call != 0u)
    {
        const uint32_t postCurSet = dc2_read_u32(rdram, mgr + 0x08u);
        if ((f51Call <= 96u || f51ListHead != 0u || setIdx == 1u) && f51_should_log())
        {
            std::fprintf(stderr,
                         "[F51:reloadraw-out] call=%u mgr=0x%x set=%u packet=0x%x postCurset=%u retWords=%u list=0x%x\n",
                         f51Call, mgr, setIdx, packet, postCurSet, getRegU32(ctx, 2), f51ListHead);
            if (f51ListHead != 0u)
                f51_log_texture_chain(rdram, f51Call, "post", f51ListHead);
        }
    }

    if (f50_10_trace_enabled() && mgr != 0u)
    {
        // Enumerate every DISTINCT manager instance (uncapped) so a 2nd map/model manager
        // with a base near 0x2580 is not hidden behind the per-call cap.
        static uint32_t s_mgrs[16] = {0};
        static uint32_t s_mgrCount = 0u;
        bool known = false;
        for (uint32_t i = 0u; i < s_mgrCount; ++i)
            if (s_mgrs[i] == mgr) { known = true; break; }
        if (!known && s_mgrCount < 16u)
        {
            s_mgrs[s_mgrCount++] = mgr;
            std::fprintf(stderr,
                         "[F510:mgr] NEW mgr=0x%x base0=0x%x clutbase=0x%x curset=%u count=%u settable=0x%x\n",
                         mgr, m0, m1, m2, m3, m4);
        }
        static uint32_t s_calls = 0u;
        const uint32_t call = ++s_calls;
        if (call <= 24u)
        {
            std::fprintf(stderr,
                         "[F510:reload] call=%u mgr=0x%x set=%u curset=%u count=%u base0=0x%x clutbase=0x%x willReload=%d\n",
                         call, mgr, setIdx, m2, m3, m0, m1, willReload ? 1 : 0);
            // Walk the texture linked-list for this set and dump each node's post-reload TEX0.
            const uint32_t listHead = dc2_read_u32(rdram, setIdx * 0x10u + m4 + 8u);
            uint32_t node = listHead;
            uint32_t guard = 0u;
            while (node != 0u && guard < 24u)
            {
                const uint64_t tex0 = dc2_read_u64(rdram, node + 0x38u);
                const uint32_t tbp  = (uint32_t)(tex0 & 0x3FFFull);
                const uint32_t psm  = (uint32_t)((tex0 >> 20) & 0x3Full);
                const uint32_t cbp  = (uint32_t)((tex0 >> 37) & 0x3FFFull);
                const uint32_t cpsm = (uint32_t)((tex0 >> 51) & 0xFull);
                const uint32_t bitd = dc2_read_u32(rdram, node + 0x04u) >> 16; // *(short*)(node+6)
                const uint32_t pix0 = dc2_read_u32(rdram, node + 0x50u);       // mip0 pixel ptr
                const uint32_t clut = dc2_read_u32(rdram, node + 0x60u);       // CLUT ptr
                std::fprintf(stderr,
                             "[F510:tex] call=%u node=0x%x tbp=0x%x psm=0x%x cbp=0x%x cpsm=0x%x bitdepth=%u pix0=0x%x clutptr=0x%x\n",
                             call, node, tbp, psm, cbp, cpsm, bitd, pix0, clut);
                node = dc2_read_u32(rdram, node + 0x68u);
                ++guard;
            }
        }
    }
}

static void f50_11_drawprim_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f50_11_trace_enabled())
    {
        const uint32_t env = getRegU32(ctx, 5); // param_2: texEnv (sceGsTex0 at +0)
        if (env != 0u)
        {
            const uint64_t tex0 = dc2_read_u64(rdram, env);
            const uint32_t tbp  = (uint32_t)(tex0 & 0x3FFFull);
            const uint32_t psm  = (uint32_t)((tex0 >> 20) & 0x3Full);
            const uint32_t cbp  = (uint32_t)((tex0 >> 37) & 0x3FFFull);
            const uint32_t cpsm = (uint32_t)((tex0 >> 51) & 0xFull);
            const bool paletted = (psm == 0x13u || psm == 0x14u || psm == 0x1Bu || psm == 0x24u || psm == 0x2Cu);
            static uint32_t s_all = 0u;
            const uint32_t n = ++s_all;
            // Histogram-friendly: log paletted binds (capped) + always when tbp==0x2580.
            static uint32_t s_pal = 0u;
            const bool hit2580 = (tbp == 0x2580u);
            if ((paletted && ++s_pal <= 120u) || (hit2580 && (n % 256u) == 1u))
            {
                std::fprintf(stderr,
                             "[F511:bind] n=%u env=0x%x tbp=0x%x psm=0x%x cbp=0x%x cpsm=0x%x ra=0x%x\n",
                             n, env, tbp, psm, cbp, cpsm, getRegU32(ctx, 31));
                if (hit2580)
                {
                    // Dump the texEnv neighbourhood to identify the owning struct.
                    std::fprintf(stderr,
                                 "[F511:env2580] env=0x%x  +0=0x%llx  +0x28=0x%x +0x40=0x%x +0x48=0x%x +0x50=0x%x +0x58=0x%x +0x60=0x%x  (env-0x38)+0x50=0x%x (env-0x38)+0x60=0x%x\n",
                                 env, (unsigned long long)tex0,
                                 dc2_read_u32(rdram, env + 0x28u), dc2_read_u32(rdram, env + 0x40u),
                                 dc2_read_u32(rdram, env + 0x48u), dc2_read_u32(rdram, env + 0x50u),
                                 dc2_read_u32(rdram, env + 0x58u), dc2_read_u32(rdram, env + 0x60u),
                                 dc2_read_u32(rdram, env - 0x38u + 0x50u), dc2_read_u32(rdram, env - 0x38u + 0x60u));
                }
            }
        }
    }
    Texture__11mgCDrawPrimFP10mgCTexture_0x134da0(rdram, ctx, runtime);
}

static void f50_11_get_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t mgr  = getRegU32(ctx, 4);
    const uint32_t name = getRegU32(ctx, 5);
    GetTexture__17mgCTextureManagerFPci_0x12d050(rdram, ctx, runtime);
    if (f50_11_trace_enabled())
    {
        const uint32_t tex = getRegU32(ctx, 2); // v0 return
        if (tex != 0u)
        {
            const uint64_t tex0 = dc2_read_u64(rdram, tex + 0x38u);
            const uint32_t tbp  = (uint32_t)(tex0 & 0x3FFFull);
            const uint32_t cbp  = (uint32_t)((tex0 >> 37) & 0x3FFFull);
            const uint32_t psm  = (uint32_t)((tex0 >> 20) & 0x3Full);
            char nm[28]; nm[0] = '\0';
            for (uint32_t i = 0u; i < 24u; ++i)
            {
                const uint8_t b = (uint8_t)(dc2_read_u32(rdram, name + i) & 0xFFu);
                nm[i] = (b >= 0x20u && b < 0x7Fu) ? (char)b : (b == 0u ? '\0' : '.');
                if (b == 0u) break;
            }
            nm[27] = '\0';
            static uint32_t s_n = 0u;
            const uint32_t n = ++s_n;
            const bool hit = (tbp == 0x2580u);
            if (n <= 120u || hit)
                std::fprintf(stderr,
                             "[F511:gettex] n=%u mgr=0x%x name='%s' tex=0x%x tbp=0x%x psm=0x%x cbp=0x%x pix0=0x%x clutptr=0x%x%s\n",
                             n, mgr, nm, tex, tbp, psm, cbp,
                             dc2_read_u32(rdram, tex + 0x50u), dc2_read_u32(rdram, tex + 0x60u),
                             hit ? "  <<< 0x2580" : "");
        }
    }
}

static bool f48_vtable_entry_is_callable(PS2Runtime *runtime, uint32_t fn)
{
    return runtime != nullptr && fn != 0u && runtime->hasFunction(fn);
}

static bool f48_title_camera_vtable_is_valid(uint8_t *rdram, PS2Runtime *runtime, uint32_t vtable)
{
    if (vtable == 0u)
        return false;

    const uint32_t initFn = dc2_read_u32(rdram, vtable + 0x08u);
    const uint32_t moveFn = dc2_read_u32(rdram, vtable + 0x18u);
    return f48_vtable_entry_is_callable(runtime, initFn) &&
           f48_vtable_entry_is_callable(runtime, moveFn);
}

static uint32_t f48_title_scene_camera(uint8_t *rdram, R5900Context *ctx, uint32_t *sceneOut, uint32_t *cameraIdOut, uint32_t *cameraCountOut)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t scene = dc2_read_u32(rdram, gp + 0xFFFF99ECu);
    const uint32_t cameraId = scene ? dc2_read_u32(rdram, scene + 0x2E54u) : 0xFFFFFFFFu;
    const uint32_t cameraCount = scene ? dc2_read_u32(rdram, scene + 0x2044u) : 0u;
    if (sceneOut)
        *sceneOut = scene;
    if (cameraIdOut)
        *cameraIdOut = cameraId;
    if (cameraCountOut)
        *cameraCountOut = cameraCount;

    if (scene == 0u)
        return 0u;

    if (cameraId >= cameraCount)
        return 0u;

    const uint32_t cameraEntry = scene + cameraId * 0x38u + 0x2048u;
    return dc2_read_u32(rdram, cameraEntry + 0x34u);
}

static bool f48_assign_title_scene_camera(uint8_t *rdram, uint32_t scene, uint32_t cameraId, uint32_t camera)
{
    if (scene == 0u || camera == 0u || cameraId > 1u)
        return false;

    const uint32_t cameraEntry = scene + cameraId * 0x38u + 0x2048u;
    const uint32_t minCount = cameraId + 1u;
    const uint32_t cameraCount = dc2_read_u32(rdram, scene + 0x2044u);
    if (cameraCount < minCount)
        dc2_write_u32(rdram, scene + 0x2044u, minCount);

    dc2_write_u32(rdram, cameraEntry + 0x00u, 4u);
    dc2_write_u32(rdram, cameraEntry + 0x08u, 0u);
    dc2_write_u32(rdram, cameraEntry + 0x34u, camera);
    return true;
}

static bool f48_repair_title_scene_cameras(uint8_t *rdram, R5900Context *ctx, uint32_t scene)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleCamera = dc2_read_u32(rdram, gp + 0xFFFF9948u);
    const uint32_t titleCamera2 = dc2_read_u32(rdram, gp + 0xFFFF994Cu);
    bool assigned = false;

    if (titleCamera != 0u)
    {
        const uint32_t slot0 = dc2_read_u32(rdram, scene + 0x2048u + 0x34u);
        if (slot0 == 0u)
            assigned = f48_assign_title_scene_camera(rdram, scene, 0u, titleCamera) || assigned;
    }

    if (titleCamera2 != 0u)
    {
        const uint32_t slot1 = dc2_read_u32(rdram, scene + 0x2048u + 0x38u + 0x34u);
        if (slot1 == 0u)
            assigned = f48_assign_title_scene_camera(rdram, scene, 1u, titleCamera2) || assigned;
    }

    return assigned;
}

// G58 ROOT FIX ג€” title-scene camera never lands in its scene slot, so GetCamera returns
// NULL and TitleMapDraw's camera-follow runs on a0=0, ending in a jr-to-data (0x9f84a0)
// that kills the title loop after one frame (black screen). ROOT (live-PCSX2 A/B + probes):
// TitleInit calls AssignCamera(scene,0,cam=0x785f70) and (scene,1,cam=0x786050) BEFORE
// CScene::Initialize@0x282ea0 has set the camera count (scene+0x2044), so GetSceneCamera
// returns 0 and the assign fails (ret=-1); Initialize sets count=8 only afterwards, leaving
// slot0/1 empty (HW: slot0.valid=0x4, slot0.obj=0x785f70). The existing F48 repair only runs
// on the indirect-dispatch title-draw path, which the dominant DIRECT path bypasses. This
// wraps GetCamera__6CScene so the title camera is (re)assigned on demand, on every path, just
// before it is used; self-heals if the late Initialize re-zeroes the slot. Scoped to the
// front-end (LoopNo==3) + the title scene (MainScene 0x1dd8260, shared with the dungeon) +
// an empty slot, so it never perturbs the dungeon. Kill switch DC2_G58_NO_CAMFIX.
static void g58_getcamera_title_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool disabled = dc2_env_flag_enabled("DC2_G58_NO_CAMFIX");
    if (!disabled)
    {
        const uint32_t scene = getRegU32(ctx, 4);
        const uint32_t id    = getRegU32(ctx, 5);
        const uint32_t gp    = getRegU32(ctx, 28);
        const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu); // 3 = front-end/title
        if (loopNo == 3u && scene == 0x01DD8260u && id <= 1u)
        {
            const uint32_t count = dc2_read_u32(rdram, scene + 0x2044u);
            if (count > id)
            {
                const uint32_t slot = scene + 0x2048u + 56u * id;
                if (dc2_read_u32(rdram, slot + 0x34u) == 0u)
                {
                    // titleCamera[id] = *(gp-0x66B8 + 4*id) (set by TitleInit's mgCCamera ctor)
                    const uint32_t cam = dc2_read_u32(rdram, gp + 0xFFFF9948u + 4u * id);
                    if (cam != 0u)
                    {
                        f48_assign_title_scene_camera(rdram, scene, id, cam);
                        static unsigned camFixLogs = 0u;
                        if (f48_trace_title_camera_enabled() && camFixLogs < 8u)
                        {
                            ++camFixLogs;
                            std::fprintf(stderr, "[G58:camfix] assigned scene=0x%x id=%u cam=0x%x (count=%u)\n",
                                         scene, id, cam, count);
                            std::fflush(stderr);
                        }
                    }
                }
            }
        }
    }
    GetCamera__6CSceneFi_0x2838c0(rdram, ctx, runtime);
}

static void g59_get_texture_block_no_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t set = getRegU32(ctx, 4);
    const uint32_t idx = getRegU32(ctx, 5);
    const uint32_t buf = getRegU32(ctx, 6);
    const uint32_t max = getRegU32(ctx, 7);
    const uint32_t ra = getRegU32(ctx, 31);

    GetTextureBlockNo__11CMdsListSetFiPii_0x168fd0(rdram, ctx, runtime);

    if (!g59_trace_enabled() || ctx->pc != ra)
        return;

    const bool titleSet = (set == 0x01DDA630u);
    const bool titleCaller = (ra == 0x002A2598u || ra == 0x002A2644u);
    if (!titleSet && !titleCaller)
        return;

    static uint32_t logs = 0u;
    if (logs >= 96u)
        return;
    ++logs;

    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t mdsCnt = set ? dc2_read_u32(rdram, set + 0x00u) : 0u;
    const uint32_t imgCnt = set ? dc2_read_u32(rdram, set + 0x90u) : 0u;
    const uint32_t mds7Name = set ? dc2_read_u32(rdram, set + 0x10u + 7u * 0x10u) : 0u;
    const uint32_t mds7Count = set ? dc2_read_u32(rdram, set + 0x14u + 7u * 0x10u) : 0u;
    const uint32_t mds7List = set ? dc2_read_u32(rdram, set + 0x18u + 7u * 0x10u) : 0u;
    const uint32_t img15Flag = set ? dc2_read_u32(rdram, set + 0x94u + 15u * 8u) : 0u;
    const uint32_t img15Tbl = set ? dc2_read_u32(rdram, set + 0x98u + 15u * 8u) : 0u;
    const uint32_t img15Base = (img15Tbl && idx < 32u) ? dc2_read_u32(rdram, img15Tbl + idx * 4u) : 0u;
    const uint32_t img15Count = (img15Tbl && idx < 32u) ? dc2_read_u32(rdram, img15Tbl + 0x80u + idx * 4u) : 0u;

    uint32_t foundEntry = 0xFFFFFFFFu;
    uint32_t foundFlag = 0u;
    uint32_t foundTbl = 0u;
    uint32_t foundBase = 0u;
    uint32_t foundCount = 0u;
    const uint32_t scanCount = (imgCnt < 32u) ? imgCnt : 32u;
    if (idx < 32u)
    {
        for (uint32_t i = 0u; i < scanCount; ++i)
        {
            const uint32_t flag = dc2_read_u32(rdram, set + 0x94u + i * 8u);
            const uint32_t tbl = dc2_read_u32(rdram, set + 0x98u + i * 8u);
            if (!flag || !tbl)
                continue;
            const uint32_t base = dc2_read_u32(rdram, tbl + idx * 4u);
            const uint32_t count = dc2_read_u32(rdram, tbl + 0x80u + idx * 4u);
            if (count != 0u || base != 0xFFFFFFFFu)
            {
                foundEntry = i;
                foundFlag = flag;
                foundTbl = tbl;
                foundBase = base;
                foundCount = count;
                break;
            }
        }
    }

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    std::fprintf(stderr,
                 "[G59:block] n=%u set=0x%x idx=%u max=%u ret=%d(0x%x) buf=0x%x "
                 "out=%x,%x,%x,%x ra=0x%x pc=0x%x loop=%u mdsCnt=%u imgCnt=%u "
                 "mds7(name=0x%x cnt=%u list=0x%x) found=%u flag=0x%x tbl=0x%x base=0x%x cnt=%u "
                 "img15(flag=0x%x tbl=0x%x base=0x%x cnt=%u)\n",
                 logs, set, idx, max, static_cast<int32_t>(ret), ret, buf,
                 dc2_read_u32(rdram, buf + 0u),
                 dc2_read_u32(rdram, buf + 4u),
                 dc2_read_u32(rdram, buf + 8u),
                 dc2_read_u32(rdram, buf + 12u),
                 ra, ctx->pc, loopNo, mdsCnt, imgCnt,
                 mds7Name, mds7Count, mds7List,
                 foundEntry, foundFlag, foundTbl, foundBase, foundCount,
                 img15Flag, img15Tbl, img15Base, img15Count);
    std::fflush(stderr);
}

static bool g59_title_scope(uint8_t *rdram, R5900Context *ctx, uint32_t *sceneOut = nullptr)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t scene = gp ? dc2_read_u32(rdram, gp + 0xFFFF99ECu) : 0u;
    if (sceneOut)
        *sceneOut = scene;
    return loopNo == 3u && scene == 0x01DD8260u;
}

static uint32_t g59_title_camera_ptr(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t scene = 0u;
    if (!g59_title_scope(rdram, ctx, &scene))
        return 0u;

    uint32_t id = scene ? dc2_read_u32(rdram, scene + 0x2E54u) : 0u;
    if (id > 1u)
        id = 0u;

    const uint32_t slot = scene + 0x2048u + 56u * id;
    uint32_t camera = dc2_read_u32(rdram, slot + 0x34u);
    if (!camera)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        camera = gp ? dc2_read_u32(rdram, gp + 0xFFFF9948u + 4u * id) : 0u;
    }
    return camera;
}

static void g59_repair_title_map_resume_frame(uint8_t *rdram, R5900Context *ctx, const char *tag)
{
    if (!g59_title_scope(rdram, ctx))
        return;

    static const uint32_t kExpectedTitleMapReturn = 0x002A1B88u;
    const uint32_t sp = getRegU32(ctx, 29);
    const bool titleMapFrame = (sp >= 0x001FFF700u && sp <= 0x001FFF800u);
    if (!titleMapFrame)
        return;

    const uint32_t raLo = dc2_read_u32(rdram, sp + 0xA0u);
    const uint32_t raHi = dc2_read_u32(rdram, sp + 0xA4u);
    const uint32_t camera = g59_title_camera_ptr(rdram, ctx);
    const uint32_t s7 = getRegU32(ctx, 23);
    bool fixedRa = false;
    bool fixedS7 = false;

    if (sp >= 0x00100000u && sp < 0x02000000u &&
        (raLo != kExpectedTitleMapReturn || raHi != 0u))
    {
        dc2_write_u32(rdram, sp + 0xA0u, kExpectedTitleMapReturn);
        dc2_write_u32(rdram, sp + 0xA4u, 0u);
        fixedRa = true;
    }

    if (s7 == 0u && camera != 0u)
    {
        f50_set_reg(ctx, 23, camera);
        fixedS7 = true;
    }

    if (g59_trace_enabled() && (fixedRa || fixedS7))
    {
        static uint32_t logs = 0u;
        if (logs < 32u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:statefix] tag=%s sp=0x%x raSlot=0x%x:%x s7=0x%x cam=0x%x fixedRa=%u fixedS7=%u pc=0x%x\n",
                         tag, sp, raHi, raLo, s7, camera, fixedRa ? 1u : 0u,
                         fixedS7 ? 1u : 0u, ctx->pc);
            std::fflush(stderr);
        }
    }
}

static void g59_repair_title_map_resume_sp(uint8_t *rdram, R5900Context *ctx, const char *tag)
{
    if (!g59_title_scope(rdram, ctx))
        return;

    const uint32_t sp = getRegU32(ctx, 29);
    if (sp < 0x001FFEF00u || sp > 0x001FFEFFFu)
        return;

    const uint32_t fixedSp = sp + 0x830u;
    f50_set_reg(ctx, 29, fixedSp);

    if (g59_trace_enabled())
    {
        static uint32_t logs = 0u;
        if (logs < 16u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:spfix] tag=%s sp=0x%x -> 0x%x pc=0x%x\n",
                         tag, sp, fixedSp, ctx->pc);
            std::fflush(stderr);
        }
    }
}

static bool g59_repair_title_camera_arg(uint8_t *rdram, R5900Context *ctx, const char *tag)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const bool titleCameraCaller =
        ra == 0x002A2734u || ra == 0x002A273Cu ||
        ra == 0x002A2774u || ra == 0x002A27A4u || ra == 0x002A27D4u ||
        ra == 0x002A284Cu || ra == 0x002A285Cu || ra == 0x002A2868u;
    if (!titleCameraCaller || getRegU32(ctx, 4) != 0u)
        return false;

    const uint32_t camera = g59_title_camera_ptr(rdram, ctx);
    if (!camera)
        return false;

    f50_set_reg(ctx, 4, camera);

    if (g59_trace_enabled())
    {
        static uint32_t logs = 0u;
        if (logs < 24u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:camarg] tag=%s ra=0x%x repaired a0=0x%x\n",
                         tag, ra, camera);
            std::fflush(stderr);
        }
    }
    return true;
}

static void g59_title_map_resume_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *tag)
{
    g59_repair_title_map_resume_sp(rdram, ctx, tag);
    g59_repair_title_map_resume_frame(rdram, ctx, tag);
    TitleMapDraw__Fv_0x2a2280(rdram, ctx, runtime);
}

static void g59_title_map_resume_2548(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g59_title_map_resume_fix(rdram, ctx, runtime, "TitleMapDraw@2a2548"); }

static void g59_title_map_resume_2644(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g59_title_map_resume_fix(rdram, ctx, runtime, "TitleMapDraw@2a2644"); }

static void g59_cam_adddistance_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g59_repair_title_camera_arg(rdram, ctx, "AddDistance");
    // G103 probe: measure the title-camera zoom rate PER AddDistance call. HW zooms at
    // -0.25/frame (verified live). If dist drops 0.25 per call here, the rate is correct and
    // any wall-clock framing lag is a runner frame-rate (perf) issue, not a camera bug.
    static const bool s_g103zoom = (std::getenv("DC2_G103_ZOOM") != nullptr);
    auto g103rf = [&](uint32_t a) -> float { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
    uint32_t g103cam = 0u;
    float g103before = 0.0f;
    if (s_g103zoom)
    {
        g103cam = getRegU32(ctx, 4);
        if (g103cam) g103before = g103rf(g103cam + 0x90u);
    }
    AddDistance__15mgCCameraFollowFf_0x131a20(rdram, ctx, runtime);
    if (s_g103zoom && g103cam)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        const float after = g103rf(g103cam + 0x90u);
        if (n < 8u || (n % 256u) == 0u)
            std::fprintf(stderr, "[G103:zoom] call=%u cam=0x%x ra=0x%x dist %.3f->%.3f (d=%.4f)\n",
                         n, g103cam, getRegU32(ctx, 31), g103before, after, after - g103before);
    }
    g59_repair_title_map_resume_frame(rdram, ctx, "AddDistance.post");
}

static void g59_cam_getdistance_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g59_repair_title_camera_arg(rdram, ctx, "GetDistance");
    GetDistance__15mgCCameraFollowFv_0x131a10(rdram, ctx, runtime);
    g59_repair_title_map_resume_frame(rdram, ctx, "GetDistance.post");
}

static void g59_cam_addheight_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g59_repair_title_camera_arg(rdram, ctx, "AddHeight");
    AddHeight__15mgCCameraFollowFf_0x131a50(rdram, ctx, runtime);
    g59_repair_title_map_resume_frame(rdram, ctx, "AddHeight.post");
}

static void g59_mgdraw_frame_tailfix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t entrySp = getRegU32(ctx, 29);
    const uint32_t entryRa = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t titleMap = gp ? dc2_read_u32(rdram, gp + 0xFFFF9944u) : 0u;
    const bool titleLike = (loopNo == 3u && titleMap == 0x008FC880u);
    const uint32_t frame = getRegU32(ctx, 4);
    const uint32_t vt = frame ? dc2_read_u32(rdram, frame + 0x00u) : 0u;
    const uint32_t drawSlot = vt ? dc2_read_u32(rdram, vt + 0x34u) : 0u;

    mgDraw__FP8mgCFrame_0x142f90(rdram, ctx, runtime);

    const uint32_t postPc = ctx->pc;
    const uint32_t postSp = getRegU32(ctx, 29);
    const uint32_t postRa = getRegU32(ctx, 31);
    bool repaired = false;
    uint32_t savedRa = 0u;
    uint32_t savedRaHi = 0u;

    if (titleLike &&
        entryRa == 0x00169FFCu &&
        drawSlot == 0x00137E10u &&
        postPc == drawSlot &&
        entrySp >= 0x10u &&
        postSp == entrySp - 0x10u)
    {
        savedRa = dc2_read_u32(rdram, postSp + 0x00u);
        savedRaHi = dc2_read_u32(rdram, postSp + 0x04u);
        if (savedRa == entryRa && savedRaHi == 0u)
        {
            f50_set_reg(ctx, 31, savedRa);
            f50_set_reg(ctx, 29, entrySp);
            ctx->pc = savedRa;
            repaired = true;
        }
    }

    if (g59_trace_enabled() && (titleLike || repaired || postPc == drawSlot))
    {
        static uint32_t logs = 0u;
        if (logs < 64u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:mgdrawfix] n=%u frame=0x%x slot=0x%x entry(sp=0x%x ra=0x%x) "
                         "post(pc=0x%x sp=0x%x ra=0x%x saved=0x%x:%x) loop=%u map=0x%x repaired=%u\n",
                         logs, frame, drawSlot, entrySp, entryRa,
                         postPc, postSp, postRa, savedRaHi, savedRa,
                         loopNo, titleMap, repaired ? 1u : 0u);
            std::fflush(stderr);
        }
    }
}

// Draw__8mgCFrameFv@0x1387f0 is a no-frame thunk:
//   a1 = 0; jr *(frame->vtable + 0x44)
// The generated body returns to the dispatcher with pc set to the target slot
// instead of completing the inherited tail-call. mgDraw then exits with its
// stack frame still active. Run the virtual target synchronously so it returns
// to mgDraw's real continuation (usually 0x142fac).
static void g59_frame_draw_tailcall_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t frame = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t sp = getRegU32(ctx, 29);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t titleMap = gp ? dc2_read_u32(rdram, gp + 0xFFFF9944u) : 0u;
    const bool titleLike = (loopNo == 3u && titleMap == 0x008FC880u);
    const uint32_t vt = frame ? dc2_read_u32(rdram, frame + 0x00u) : 0u;
    const uint32_t slot = vt ? dc2_read_u32(rdram, vt + 0x44u) : 0u;

    f50_set_reg(ctx, 5, 0u);
    f50_set_reg(ctx, 25, slot);

    bool called = false;
    bool normalized = false;
    uint32_t postPc = slot;
    uint32_t postSp = sp;
    uint32_t postRa = ra;

    if (slot != 0u && slot != 0x001387F0u && runtime->hasFunction(slot))
    {
        auto targetFn = runtime->lookupFunction(slot);
        ctx->pc = slot;
        targetFn(rdram, ctx, runtime);
        called = true;
        postPc = ctx->pc;
        postSp = getRegU32(ctx, 29);
        postRa = getRegU32(ctx, 31);

        if (postPc == slot || postPc == 0u)
        {
            ctx->pc = ra;
            postPc = ctx->pc;
            normalized = true;
        }
    }
    else
    {
        ctx->pc = slot;
    }

    if (g59_trace_enabled() && (titleLike || slot == 0x00137E10u || !called || normalized))
    {
        static uint32_t logs = 0u;
        if (logs < 96u)
        {
            ++logs;
            std::fprintf(stderr,
                         "[G59:framevfix] n=%u frame=0x%x vt=0x%x slot=0x%x ra=0x%x "
                         "sp=0x%x post(pc=0x%x sp=0x%x ra=0x%x) called=%u normalized=%u "
                         "loop=%u map=0x%x\n",
                         logs, frame, vt, slot, ra,
                         sp, postPc, postSp, postRa, called ? 1u : 0u, normalized ? 1u : 0u,
                         loopNo, titleMap);
            std::fflush(stderr);
        }
    }
}

static bool g53_trace_enabled(); // defined below; used by the title draw probe
static void f55_read_cstr(uint8_t *rdram, uint32_t addr, char *out, uint32_t cap); // fwd

// PHASE F48: keep real TitleMapDraw active, but repair the specific scene
// camera object it is about to virtual-dispatch through if the vtable pointer
// does not resolve to registered PS2 functions. This targets the observed
// bad-PC chain at TitleMapDraw+0x7c (ra=0x2a22fc) without broad dispatch policy.
static void f48_title_map_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static uint32_t fixes = 0u;
    static uint32_t logs = 0u;

    const uint32_t n = ++calls;
    uint32_t scene = 0u;
    uint32_t cameraId = 0xFFFFFFFFu;
    uint32_t cameraCount = 0u;
    uint32_t camera = f48_title_scene_camera(rdram, ctx, &scene, &cameraId, &cameraCount);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleCamera = dc2_read_u32(rdram, gp + 0xFFFF9948u);
    const uint32_t titleCamera2 = dc2_read_u32(rdram, gp + 0xFFFF994Cu);
    const bool assignedSceneCamera = (camera == 0u && scene != 0u) ?
        f48_repair_title_scene_cameras(rdram, ctx, scene) : false;
    if (assignedSceneCamera)
        camera = f48_title_scene_camera(rdram, ctx, &scene, &cameraId, &cameraCount);

    const uint32_t beforeVtable = camera ? dc2_read_u32(rdram, camera + 0x60u) : 0u;
    const uint32_t beforeFn8 = beforeVtable ? dc2_read_u32(rdram, beforeVtable + 0x08u) : 0u;
    const uint32_t beforeFn18 = beforeVtable ? dc2_read_u32(rdram, beforeVtable + 0x18u) : 0u;
    const bool validBefore = f48_title_camera_vtable_is_valid(rdram, runtime, beforeVtable);
    bool fixed = false;

    if (camera != 0u && !validBefore)
    {
        dc2_write_u32(rdram, camera + 0x60u, 0x00374E70u);
        fixed = true;
        ++fixes;
    }

    if (f48_trace_title_camera_enabled() && logs < 64u &&
        (fixed || n <= 12u || (n % 60u) == 0u))
    {
        const uint32_t afterVtable = camera ? dc2_read_u32(rdram, camera + 0x60u) : 0u;
        const uint32_t afterFn8 = afterVtable ? dc2_read_u32(rdram, afterVtable + 0x08u) : 0u;
        const uint32_t afterFn18 = afterVtable ? dc2_read_u32(rdram, afterVtable + 0x18u) : 0u;
        std::fprintf(stderr,
                     "[F48:titlecam] n=%u scene=0x%x count=%u camId=%u camera=0x%x titleCamera=0x%x titleCamera2=0x%x assigned=%u vt=0x%x fn8=0x%x fn18=0x%x fixed=%u fixes=%u afterVt=0x%x afterFn8=0x%x afterFn18=0x%x\n",
                     n, scene, cameraCount, cameraId, camera, titleCamera, titleCamera2,
                     assignedSceneCamera ? 1u : 0u, beforeVtable, beforeFn8, beforeFn18,
                     fixed ? 1u : 0u, fixes, afterVtable, afterFn8, afterFn18);
        ++logs;
    }

    // G53 (draw side): TitleMapDraw gates all 3D geometry on the global TitleMap
    // (= GetMap(TitleScene, scene[0xb97])). GetMap returns 0 unless map slot 0
    // (scene+0x27e4) has its active word (+0) != 0, and then returns the map ptr
    // (slot+0x34). HW: TitleMap=0x8fc880, slot active, ptr set. Dump both so we can
    // see whether the seeded/built map ever registers into the scene map slot.
    // Run the real draw FIRST so PreDraw__4CMap has rebuilt the per-frame visible draw
    // list (map+0x360) for THIS frame before we sample it.
    // G57: suppress back-edge preemption for the whole title draw. The draw flushes
    // through the deferred mgCDrawManager whose BeginDraw is reached via an override that
    // calls the recompiled body directly; a mid-loop preemption there leaks a resume PC
    // this path cannot re-enter, leaving the manager half-initialized (mgr[4]/mgr[6] null)
    // so the deferred flush dereferences null and aborts the title draw before any geometry
    // reaches the GS. Suppressing preemption makes the bodies run to completion in one call.
    // Kill switch DC2_G57_NO_SUPPRESS (default ON).
    // G57: when the title draw is reached through this (indirect-dispatch) override, it runs
    // TitleMapDraw as a direct C++ call. A mid-loop back-edge preemption inside the deferred
    // draw chain then returns a resume PC this path cannot re-enter, leaving e.g. BeginDraw's
    // mgr[4]/mgr[6] null -> deferred-flush null-deref. Suppress preemption for the duration so
    // the recompiled bodies run to completion in one call. Inert on the dominant DIRECT path
    // (TitleModeDraw -> jal TitleMapDraw bypasses this override), which the normal dispatch
    // loop already drives correctly. Kill switch DC2_G57_NO_SUPPRESS (default ON).
    // G127 (FIX, default-ON, kill-switch DC2_G127_NO_CAMID_FIX): the debug-menu "title"
    // entry reaches TitleMapDraw with the TitleScene active-camera index (scene+0x2e54) = -1
    // (HW = 0). On the normal boot TitleInit->AssignCamera sets it (AssignCamera@0x2837e0:
    // `if (scene+0x2e54 >= 0) skip; else scene+0x2e54 = assignedId`), but a debug re-entry
    // re-runs Initialize__6CScene (which resets active to -1) WITHOUT re-running AssignCamera
    // (the camera stays registered in slot 0). With active = -1, GetCamera(scene,-1) returns
    // NULL -> TitleMapDraw builds a ZERO view matrix -> mgSetViewMatrix writes 0 to
    // renderinfo +0x10/+0x90/+0x150 -> PreDraw__4CMap's mgInsideScreen frustum-culls all 6
    // parts -> map+0x360 draw list = 0 -> nothing reaches the VU -> flat blue.
    // Fix = restore the scene's OWN invariant using AssignCamera's OWN rule: when the active
    // index is -1 and a valid camera is registered in slot 0, select it. Touches only the
    // TitleScene object's field (NOT a shared global -> cannot leak into a concurrent screen,
    // unlike the rejected G93 TitleProjection/camera writes); idempotent (fires once, the
    // condition is then false); inert on the auto route (active already 0) and on title states
    // that don't draw the map (MenuMainDraw / costume / dungeon never reach TitleMapDraw).
    {
        static const bool g127NoCamIdFix = dc2_env_flag_enabled("DC2_G127_NO_CAMID_FIX");
        const uint32_t gpFix = getRegU32(ctx, 28);
        const uint32_t tsceneFix = gpFix ? dc2_read_u32(rdram, gpFix + 0xFFFF99ECu) : 0u;
        if (!g127NoCamIdFix && tsceneFix != 0u &&
            dc2_read_u32(rdram, tsceneFix + 0x2E54u) == 0xFFFFFFFFu &&
            dc2_read_u32(rdram, tsceneFix + 0x2044u) > 0u &&
            dc2_read_u32(rdram, tsceneFix + 0x2048u + 0x00u) != 0u &&
            dc2_read_u32(rdram, tsceneFix + 0x2048u + 0x34u) != 0u)
        {
            dc2_write_u32(rdram, tsceneFix + 0x2E54u, 0u);
        }
    }

    static const bool g57SuppressDisabled = dc2_env_flag_enabled("DC2_G57_NO_SUPPRESS");
    if (!g57SuppressDisabled)
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    TitleMapDraw__Fv_0x2a2280(rdram, ctx, runtime);
    if (!g57SuppressDisabled)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);


    // G65 (diagnosis): one-shot dump of the title model data region (frame tree +
    // mesh/strip data for s19_0.pcp; HW base 0x903000..0x912000) so it can be diffed
    // against the live-PCSX2 dump (captures/g65_hw_model.bin). If the runner region is
    // mostly zero / differs, the title geometry data never loaded (load gap); if it
    // matches HW, the 0-strip-count defect is in the render packet build, not the data.
    // Env-gated DC2_G65_MODELDUMP; fires once, after the model is loaded (n>=8).
    if (n >= 8u && std::getenv("DC2_G65_MODELDUMP") != nullptr)
    {
        static bool s_g65dumped = false;
        if (!s_g65dumped)
        {
            const uint32_t start = 0x00903000u, end = 0x00912000u;
            const uint8_t* src = getConstMemPtr(rdram, start);
            if (src)
            {
                if (FILE* f = std::fopen("D:/ps2r/dc2/captures/g65_runner_model.bin", "wb"))
                {
                    std::fwrite(src, 1, (size_t)(end - start), f);
                    std::fclose(f);
                    size_t nz = 0; for (uint32_t i = 0; i < (end - start); ++i) if (src[i]) ++nz;
                    std::fprintf(stderr, "[G65:modeldump] wrote 0x%x..0x%x bytes=%u nonzero=%zu\n",
                                 start, end, end - start, nz);
                }
            }
            s_g65dumped = true;
        }
    }

    static uint32_t g53DrawLogs = 0u;
    if (g53_trace_enabled() && scene != 0u && g53DrawLogs < 200u && (n <= 8u || (n % 120u) == 0u))
    {
        const uint32_t titleMap = dc2_read_u32(rdram, 0x00377E34u);
        const uint32_t nowMapNo = dc2_read_u32(rdram, scene + 0x2E5Cu);
        const uint32_t mapSlotCnt = dc2_read_u32(rdram, scene + 0x27E0u);
        const uint32_t slot0 = scene + 0x27E4u;
        const uint32_t slot0Active = dc2_read_u32(rdram, slot0 + 0x00u);
        const uint32_t slot0MapPtr = dc2_read_u32(rdram, slot0 + 0x34u);
        // CMap geometry: DrawSub__4CMapFi iterates *(map+0x360) parts from *(map+0x364);
        // each drawn via Draw__9CMapParts (vt+0x34). HW: cnt=6 parts@0x9f84a0 flags=0x161.
        uint32_t partsCnt = 0u, partsArr = 0u, mapFlags = 0u, p0 = 0u, p0vt = 0u;
        uint32_t mBase = 0u, mCnt = 0u, m0active = 0u, m0vis = 0u, m0vt = 0u;
        if (titleMap)
        {
            partsCnt = dc2_read_u32(rdram, titleMap + 0x360u);
            partsArr = dc2_read_u32(rdram, titleMap + 0x364u);
            mapFlags = dc2_read_u32(rdram, titleMap + 0xCB0u);
            if (partsArr) { p0 = dc2_read_u32(rdram, partsArr); if (p0) p0vt = dc2_read_u32(rdram, p0); }
            // Master parts (built by LoadMapFromMemory): base +0x32c, count +0x330,
            // stride 0x310. Per part: active +0x70 (byte), in-frustum/visible +0x2f8.
            mBase = dc2_read_u32(rdram, titleMap + 0x32Cu);
            mCnt  = dc2_read_u32(rdram, titleMap + 0x330u);
            if (mBase && mCnt) {
                m0active = dc2_read_u32(rdram, mBase + 0x70u) & 0xFFu;
                m0vis = dc2_read_u32(rdram, mBase + 0x2F8u);
                m0vt = dc2_read_u32(rdram, mBase);
            }
        }
        // Clip/view-proj matrix globals @0x380f50 used by mgInsideScreen (VU0 box proj).
        // HW row0 ~ [480,0,0,0]. part0+0x230 (=[0x8c]) must be !=0 (geometry present).
        auto rf = [&](uint32_t a){ uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f,&u,4); return f; };
        const uint32_t m0geom = mBase ? dc2_read_u32(rdram, mBase + 0x230u) : 0u;
        // part0+0xb0 = CMapPiece list head (HW=0x9fe340). part0+0x80 = active byte.
        // CreateBoundBox iterates this list to set +0x230. Empty list => +0x230 stays 0.
        const uint32_t m0pieces = mBase ? dc2_read_u32(rdram, mBase + 0xB0u) : 0u;
        uint32_t pc0visual = 0u;
        if (m0pieces) pc0visual = dc2_read_u32(rdram, m0pieces + 0x80u); // = piece+0x70 visual
        // MDS model table (map+0x100 = CMdsListSet, count = *CMdsListSet). HW: 0x1dda630, count=8.
        // SearchMDS uses this to resolve a piece's model; empty table => visual stays 0.
        const uint32_t mdsSet = titleMap ? dc2_read_u32(rdram, titleMap + 0x100u) : 0u;
        const uint32_t mdsCount = mdsSet ? dc2_read_u32(rdram, mdsSet) : 0u;
        // G55: how many of the mdsCount slots are FILLED (name word != 0). HW fills exactly 1
        // (s19_0.pcp in slot 7). 0 filled with count=8 => GetPCPName / GetPackFile failed.
        uint32_t mdsFilled = 0u, slotName0 = 0u;
        for (uint32_t i = 0u; i < mdsCount && i < 16u; ++i)
        {
            const uint32_t nm = dc2_read_u32(rdram, mdsSet + 0x10u + i * 0x10u);
            if (nm) { ++mdsFilled; if (!slotName0) slotName0 = nm; }
        }
        // CMapInfo PCP name list (parsed from s19.map): map+0x44 count, map+0x48 array.
        const uint32_t pcpCnt = titleMap ? dc2_read_u32(rdram, titleMap + 0x44u) : 0u;
        const uint32_t pcp0 = titleMap ? dc2_read_u32(rdram, titleMap + 0x48u) : 0u;
        char pcpName[24] = {0};
        if (pcp0) f55_read_cstr(rdram, pcp0, pcpName, sizeof(pcpName));
        // G55: the actual per-frame visible draw list count (map+0x360, rebuilt by
        // PreDraw__4CMap). HW=6 (all parts pass InsideScreen). 0 => parts still culled
        // despite bbox (VU0 box/clip projection rejects the box -> camera/clip issue).
        // Tally how many of the 6 master parts are flagged visible (+0x2f8) + have bbox.
        uint32_t visParts = 0u, bboxParts = 0u, visualParts = 0u;
        for (uint32_t i = 0u; i < mCnt && i < 6u; ++i)
        {
            const uint32_t p = mBase + i * 0x310u;
            if (dc2_read_u32(rdram, p + 0x2F8u)) ++visParts;
            if (dc2_read_u32(rdram, p + 0x230u)) ++bboxParts;
            const uint32_t pc = dc2_read_u32(rdram, p + 0xB0u);
            if (pc && dc2_read_u32(rdram, pc + 0x80u)) ++visualParts;
        }
        // G126: the debug-menu "title" route reaches this draw with drawList(map+0x360)=0 while
        // the auto-advance route gets 6. drawList is rebuilt per-frame by PreDraw__4CMap's
        // mgInsideScreen, which uses the clip/view-proj at 0x380f50 (HW row0.x ~480) + the title
        // camera. Dump loopNo (g67_title_scope gate), the title camera dist/phase, and clip row0.x
        // to see whether the debug route has the title camera/clip set up at all.
        const uint32_t gpG53 = getRegU32(ctx, 28);
        const uint32_t loopNoG53 = gpG53 ? dc2_read_u32(rdram, gpG53 + 0xFFFF8ADCu) : 0u;
        const uint32_t tcamG53 = dc2_read_u32(rdram, 0x00377E38u);
        const float camDistG53 = tcamG53 ? rf(tcamG53 + 0x90u) : 0.0f;
        const uint32_t camPhaseG53 = dc2_read_u32(rdram, 0x00377E44u);
        const float clip00G53 = rf(0x00380F50u);
        const float ri10G53  = rf(0x00380ED0u); // renderinfo +0x10 row0.x (View*Proj, HW 480)
        const float ri110G53 = rf(0x00380FD0u); // renderinfo +0x110 row0.x (proj, HW 480)
        const float ri150G53 = rf(0x00381010u); // renderinfo +0x150 row0.x (view*aspect, HW ~1)
        const uint32_t riIdxG53 = dc2_read_u32(rdram, 0x00380EC0u + 0x3F4u);
        std::fprintf(stderr,
                     "[G53:draw] n=%u loopNo=%u TitleMap=0x%x masterCnt=%u drawList(map+0x360)=%u visParts=%u/%u bboxParts=%u visualParts=%u camDist=%.1f camPhase=%u clip00=%.1f ri10=%.1f ri110=%.1f ri150=%.3f riIdx=%u | mdsCount=%u mdsFilled=%u pcpCnt=%u pcp0='%s'\n",
                     n, loopNoG53, titleMap, mCnt, partsCnt, visParts, mCnt, bboxParts, visualParts,
                     camDistG53, camPhaseG53, clip00G53, ri10G53, ri110G53, ri150G53, riIdxG53,
                     mdsCount, mdsFilled, pcpCnt, pcpName);
        // G127 diag: why are +0x10/+0x150 zero on the debug route? Dump the view matrix
        // (+0x1a0, HW row0.x=1 translate.x=70.1) + the title camera eye/ref (HW eye.x=-70.1
        // ref.x=-70.1 eye.z=-493 ref.z=775.25). If eye/ref are 0 -> TitleModeInit camera
        // block was skipped on the debug entry; if eye/ref set but view 0 -> GetCameraMatrix.
        const float riView00 = rf(0x00380EC0u + 0x1A0u);
        const float riViewTx = rf(0x00380EC0u + 0x1D0u);
        const float camEyeX  = tcamG53 ? rf(tcamG53 + 0x10u) : 0.0f;
        const float camEyeZ  = tcamG53 ? rf(tcamG53 + 0x18u) : 0.0f;
        const float camRefX  = tcamG53 ? rf(tcamG53 + 0x20u) : 0.0f;
        const float camRefZ  = tcamG53 ? rf(tcamG53 + 0x28u) : 0.0f;
        const uint32_t camVtG53 = tcamG53 ? dc2_read_u32(rdram, tcamG53 + 0x60u) : 0u;
        const uint32_t tsceneG53 = gpG53 ? dc2_read_u32(rdram, gpG53 + 0xFFFF99ECu) : 0u; // TitleScene (gp-0x6614)
        const uint32_t camIdG53 = tsceneG53 ? dc2_read_u32(rdram, tsceneG53 + 0x2E54u) : 0xFFFFFFFFu;
        const uint32_t camCntG53 = tsceneG53 ? dc2_read_u32(rdram, tsceneG53 + 0x2044u) : 0u;
        const uint32_t slot0ActG53 = tsceneG53 ? dc2_read_u32(rdram, tsceneG53 + 0x2048u + 0x00u) : 0u;
        const uint32_t slot0CamG53 = tsceneG53 ? dc2_read_u32(rdram, tsceneG53 + 0x2048u + 0x34u) : 0u;
        std::fprintf(stderr,
                     "[G127:cam] n=%u tcam=0x%x vt=0x%x TitleScene=0x%x camId=%d camCnt=%u slot0(act=%u cam=0x%x) view00=%.3f viewTx=%.2f eye=(%.1f,%.1f) ref=(%.1f,%.1f)\n",
                     n, tcamG53, camVtG53, tsceneG53, (int)camIdG53, camCntG53, slot0ActG53, slot0CamG53,
                     riView00, riViewTx, camEyeX, camEyeZ, camRefX, camRefZ);
        // G56: the title geometry flushes through the deferred mgCDrawManager (global
        // mgDrawManager @ 0x3820E0) keyed by the MDS set's IMG-block table. The flush
        // crashes (jalr -> data ptr) when G55 enables the draw, so dump the manager's
        // per-block tables + the one populated MDS IMG slot. HW: mgr[1]=0xd87340
        // mgr[3]=0x40 mgr[4]=0xd87620 mgr[6]=0xd87840 (all valid title-heap bufs);
        // MDS img[15] flag=0x903a20 tbl=0x903a30 (the rest 0).
        const uint32_t mgr = 0x003820E0u;
        const uint32_t m0 = dc2_read_u32(rdram, mgr + 0x00u);   // remap enable
        const uint32_t m1 = dc2_read_u32(rdram, mgr + 0x04u);   // remap table
        const uint32_t m3 = dc2_read_u32(rdram, mgr + 0x0Cu);   // block count
        const uint32_t m4 = dc2_read_u32(rdram, mgr + 0x10u);   // per-block packet-list array
        const uint32_t m6 = dc2_read_u32(rdram, mgr + 0x18u);   // per-block count array
        const uint32_t m16 = dc2_read_u32(rdram, mgr + 0x58u);  // bounds object
        const uint32_t m16c = m16 ? dc2_read_u32(rdram, m16 + 0x0Cu) : 0u; // max block
        const uint32_t imgCnt = mdsSet ? dc2_read_u32(rdram, mdsSet + 0x90u) : 0u;
        const uint32_t img15f = mdsSet ? dc2_read_u32(rdram, mdsSet + 0x94u + 15u * 8u) : 0u;
        const uint32_t img15t = mdsSet ? dc2_read_u32(rdram, mdsSet + 0x98u + 15u * 8u) : 0u;
        std::fprintf(stderr,
                     "[G56:mgr] mgDrawManager m0=0x%x m1=0x%x m3=%u m4=0x%x m6=0x%x m16=0x%x m16+c=%u | imgCnt=%u img15(flag=0x%x tbl=0x%x)\n",
                     m0, m1, m3, m4, m6, m16, m16c, imgCnt, img15f, img15t);
        std::fflush(stderr);
    }
}

// PHASE F29: Hold the title loop for several frames before reusing F21's forced
// "PUSH START -> menu" transition. Returning 5 immediately makes MainLoop break
// its inner frame loop after exactly one mgEndFrame.
static void f29_log_title_state(const char *tag, uint8_t *rdram, R5900Context *ctx, uint32_t n, uint32_t ret)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gp + 0xFFFF8AE0u);
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu);
    const uint32_t titleMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
    const uint32_t titleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 4u) : 0xFFFFFFFFu;
    const uint32_t titlePhaseRaw = dc2_read_u32(rdram, gp + 0xFFFF99A8u) & 0xFFFFu;
    const int32_t titlePhase = static_cast<int16_t>(titlePhaseRaw);
    const uint32_t alphaPush = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x14u) : 0u;
    const uint32_t alphaLogo = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x1Cu) : 0u;
    const uint32_t alphaMenu = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x20u) : 0u;
    const uint32_t alphaCursor = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x28u) : 0u;

    if (f29_should_log(n))
    {
        std::fprintf(stderr,
                     "[F29:%s] n=%u ret=%u pc=0x%x ra=0x%x loop=%u next=%u titleInfo=0x%x mode=%u sub=%u phase=%d alpha14=0x%x alpha1c=0x%x alpha20=0x%x alpha28=0x%x\n",
                     tag, n, ret, ctx->pc, getRegU32(ctx, 31), loopNo, nextLoopNo,
                     titleInfo, titleMode, titleSub, titlePhase,
                     alphaPush, alphaLogo, alphaMenu, alphaCursor);
    }
}

static void f29_log_counters(const char *tag, uint32_t n, PS2Runtime *runtime, R5900Context *ctx)
{
    if (!f29_should_log(n))
        return;

    const auto &memory = runtime->memory();
    std::fprintf(stderr,
                 "[F29:%s] n=%u pc=0x%x ra=0x%x dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                 tag, n, ctx->pc, getRegU32(ctx, 31),
                 static_cast<unsigned long long>(memory.dmaStartCount()),
                 static_cast<unsigned long long>(memory.gifCopyCount()),
                 static_cast<unsigned long long>(memory.gsWriteCount()),
                 static_cast<unsigned long long>(memory.vifWriteCount()));
}

static void f29_log_title_draw_state(const char *tag, uint32_t n, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!f29_should_log(n))
        return;

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu);
    const uint32_t titleMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
    const uint32_t titleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 4u) : 0xFFFFFFFFu;
    const uint32_t texChronicle = dc2_read_u32(rdram, gp + 0xFFFF99C4u);
    const uint32_t texLogo = dc2_read_u32(rdram, gp + 0xFFFF99C8u);
    const auto &memory = runtime->memory();

    std::fprintf(stderr,
                 "[F29:%s] n=%u pc=0x%x ra=0x%x titleInfo=0x%x mode=%u sub=%u texChronicle=0x%x texLogo=0x%x dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                 tag, n, ctx->pc, getRegU32(ctx, 31), titleInfo, titleMode, titleSub,
                 texChronicle, texLogo,
                 static_cast<unsigned long long>(memory.dmaStartCount()),
                 static_cast<unsigned long long>(memory.gifCopyCount()),
                 static_cast<unsigned long long>(memory.gsWriteCount()),
                 static_cast<unsigned long long>(memory.vifWriteCount()));
}

static void f29_log_packet_state(const char *tag, uint32_t n, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!f29_should_log(n))
        return;

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pkt = dc2_read_u32(rdram, gp + 0xFFFF8774u); // mgVif1Packet
    const uint32_t ptr = pkt ? dc2_read_u32(rdram, pkt + 0u) : 0u;
    const uint32_t buf = pkt ? dc2_read_u32(rdram, pkt + 4u) : 0u;
    const uint32_t dmatag = pkt ? dc2_read_u32(rdram, pkt + 8u) : 0u;
    const uint32_t direct = pkt ? dc2_read_u32(rdram, pkt + 12u) : 0u;
    const uint32_t gifptag = pkt ? dc2_read_u32(rdram, pkt + 20u) : 0u;
    const uint32_t tag0 = buf ? dc2_read_u32(rdram, buf + 0u) : 0u;
    const uint32_t tag1 = buf ? dc2_read_u32(rdram, buf + 4u) : 0u;
    const uint32_t tag2 = buf ? dc2_read_u32(rdram, buf + 8u) : 0u;
    const uint32_t tag3 = buf ? dc2_read_u32(rdram, buf + 12u) : 0u;
    const uint32_t next0 = buf ? dc2_read_u32(rdram, buf + 16u) : 0u;
    const uint32_t next1 = buf ? dc2_read_u32(rdram, buf + 20u) : 0u;
    auto &memory = runtime->memory();
    const uint32_t chcr = memory.readIORegister(0x10009000u);
    const uint32_t madr = memory.readIORegister(0x10009010u);
    const uint32_t qwc = memory.readIORegister(0x10009020u);
    const uint32_t tadr = memory.readIORegister(0x10009030u);

    std::fprintf(stderr,
                 "[F29:%s] n=%u pkt=0x%x ptr=0x%x buf=0x%x dmatag=0x%x direct=0x%x gifptag=0x%x tag=%08x,%08x,%08x,%08x next=%08x,%08x vif1[chcr=%08x madr=%08x qwc=%08x tadr=%08x]\n",
                 tag, n, pkt, ptr, buf, dmatag, direct, gifptag,
                 tag0, tag1, tag2, tag3, next0, next1,
                 chcr, madr, qwc, tadr);
}

static void f47_log_menu_state(uint8_t *rdram,
                               R5900Context *ctx,
                               uint32_t mgFrame,
                               uint32_t scriptFrame,
                               uint32_t loopNo,
                               uint32_t nextLoopNo)
{
    if (!f47_trace_menu_state_enabled())
        return;

    static uint32_t logs = 0u;
    static uint32_t lastLoop = 0xFFFFFFFFu;
    static uint32_t lastNextLoop = 0xFFFFFFFFu;
    static uint32_t lastMenuPtr = 0xFFFFFFFFu;
    static uint32_t lastMenuStep = 0xFFFFFFFFu;
    static uint32_t lastMenuSel = 0xFFFFFFFFu;
    static uint32_t lastTitlePhase = 0xFFFFFFFFu;

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu); // gp-0x6684
    const int32_t titlePhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF99A8u) & 0xFFFFu); // gp-0x6658
    const uint32_t titleMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x00u) : 0xFFFFFFFFu;
    const uint32_t titleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x04u) : 0xFFFFFFFFu;

    const uint32_t menuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6b08
    const uint32_t menuStep = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x54u) : 0xFFFFFFFFu;
    const int32_t menuSel = menuPtr ? static_cast<int16_t>(dc2_read_u32(rdram, menuPtr + 0x50u) & 0xFFFFu) : -1;
    const uint32_t menuFlags = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x00u) : 0u;

    const bool changed =
        loopNo != lastLoop ||
        nextLoopNo != lastNextLoop ||
        menuPtr != lastMenuPtr ||
        menuStep != lastMenuStep ||
        static_cast<uint32_t>(menuSel) != lastMenuSel ||
        static_cast<uint32_t>(titlePhase) != lastTitlePhase;

    if (logs < 128u && (logs < 16u || changed || (mgFrame % 30u) == 0u))
    {
        std::fprintf(stderr,
                     "[F47:menu] n=%u frame=%u loop=%u next=%u titlePhase=%d titleMode=%u titleSub=%u menu=0x%x step=%u sel=%d flags=0x%x ra=0x%x\n",
                     mgFrame, scriptFrame, loopNo, nextLoopNo,
                     titlePhase, titleMode, titleSub,
                     menuPtr, menuStep, menuSel, menuFlags, getRegU32(ctx, 31));
        ++logs;
    }

    lastLoop = loopNo;
    lastNextLoop = nextLoopNo;
    lastMenuPtr = menuPtr;
    lastMenuStep = menuStep;
    lastMenuSel = static_cast<uint32_t>(menuSel);
    lastTitlePhase = static_cast<uint32_t>(titlePhase);
}

// G9 costume-route state (driver bodies live near f49_menu_main_key_probe).
static bool g9_costume_enabled();
static bool g_g9_title_forced = false;
static void g94_observe_title_state1_entry(uint8_t *rdram, R5900Context *ctx, const char *site);

static void title_mode_key_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ret = (n < 180u) ? 0u : 5u;
    f29_log_title_state("TitleModeKey", rdram, ctx, n, ret);
    setReturnU32(ctx, ret);
    ctx->pc = getRegU32(ctx, 31);
}

static void f29_title_loop_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gp + 0xFFFF8AE0u);
    const uint32_t preVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu); // -0x6684
    const uint32_t preTitleState = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x0u) : 0xFFFFFFFFu;
    const uint32_t preTitleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x4u) : 0xFFFFFFFFu;
    // G9: the runner's press-start (state 1) never advances headlessly (real
    // TitleModeKey returns 0 with no input). Poke TitleInfo[1]=2 so TitleLoop's
    // own commit branch runs case 2 = MenuMainInit(&MenuArg) and *TitleInfo->2,
    // reaching the New-Game/Load menu where the MenuMainKey hook enters costume.
    // G9: the New-Game selector is forced in f49_title_mode_key_probe (TitleModeKey
    // ret=5) so InitSaveData/SetChrEquipDirect populate the costume list naturally.
    if (f49_trace_loop_timing_enabled() && (n <= 16u || (n % 60u) == 0u))
    {
        std::fprintf(stderr,
                     "[F49:loop-enter] tag=TitleLoop n=%u ra=0x%x loop=%u next=%u pre=%u state=%u sub=%u\n",
                     n, getRegU32(ctx, 31), loopNo, nextLoopNo, preVsync,
                     preTitleState, preTitleSub);
    }
    g94_observe_title_state1_entry(rdram, ctx, "TitleLoop.enter-state1");
    const auto t0 = std::chrono::steady_clock::now();
    TitleLoop__Fv_0x29ffa0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    g94_observe_title_state1_entry(rdram, ctx, "TitleLoop.exit-state1");
    if (f49_trace_loop_timing_enabled())
    {
        const uint32_t postVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t dtVsync = postVsync - preVsync;
        const uint32_t postTitleState = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x0u) : 0xFFFFFFFFu;
        const uint32_t postTitleSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 0x4u) : 0xFFFFFFFFu;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 16u || (n % 10u) == 0u || dtMs >= 50)
        {
            std::fprintf(stderr,
                         "[F49:loop] tag=TitleLoop n=%u ra=0x%x loop=%u next=%u ret=%u dtMs=%lld dv=%u state=%u->%u sub=%u->%u\n",
                         n, getRegU32(ctx, 31), loopNo, nextLoopNo, getRegU32(ctx, 2),
                         static_cast<long long>(dtMs), dtVsync,
                         preTitleState, postTitleState, preTitleSub, postTitleSub);
        }
    }
    f29_log_title_state("TitleLoop", rdram, ctx, n, getRegU32(ctx, 2));
}

static uint32_t g_f50_6_debug_log_count = 0u;
static bool g_f50_6_forced_menu_loop = false;

static void f50_6_enable_debug_menu(uint8_t *rdram, R5900Context *ctx, const char *site)
{
    if (!f50_6_debug_menu_enabled())
        return;

    constexpr uint32_t kDebugFlagAddr = 0x00376FB8u; // gp-0x7538
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopAddr = gp + 0xFFFF8ADCu;     // gp-0x7524 LoopNo
    const uint32_t nextLoopAddr = gp + 0xFFFF8AE0u; // gp-0x7520 NextLoopNo

    const uint32_t flagBefore = dc2_read_u32(rdram, kDebugFlagAddr);
    const uint32_t loopBefore = dc2_read_u32(rdram, loopAddr);
    const uint32_t nextBefore = dc2_read_u32(rdram, nextLoopAddr);

    dc2_write_u32(rdram, kDebugFlagAddr, 1u);

    bool forcedLoop = false;
    if (!g_f50_6_forced_menu_loop && loopBefore == 3u)
    {
        dc2_write_u32(rdram, loopAddr, 0u); // LoopNo 0 = MenuInit/MenuLoop debug menu.
        g_f50_6_forced_menu_loop = true;
        forcedLoop = true;
    }

    const uint32_t flagAfter = dc2_read_u32(rdram, kDebugFlagAddr);
    const uint32_t loopAfter = dc2_read_u32(rdram, loopAddr);
    if (g_f50_6_debug_log_count < 16u &&
        (forcedLoop || flagBefore != 1u || g_f50_6_debug_log_count < 4u))
    {
        ++g_f50_6_debug_log_count;
        std::fprintf(stderr,
                     "[F50.6:debug] site=%s flag=0x%x->0x%x loop=%u->%u next=%u forcedLoop=%u gp=0x%x\n",
                     site ? site : "?", flagBefore, flagAfter, loopBefore, loopAfter,
                     nextBefore, forcedLoop ? 1u : 0u, gp);
        std::fflush(stderr);
    }
}

static void f50_6_menu_init_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);

    if (f50_6_debug_menu_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F50.6:menu] tag=MenuInit.enter n=%u ra=0x%x flag=%u loop=%u next=%u a0=0x%x\n",
                     n, getRegU32(ctx, 31),
                     dc2_read_u32(rdram, 0x00376FB8u),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u),
                     getRegU32(ctx, 4));
    }

    MenuInit__F13INIT_LOOP_ARG_0x191970(rdram, ctx, runtime);

    if (f50_6_debug_menu_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F50.6:menu] tag=MenuInit.exit n=%u flag=%u loop=%u next=%u ret=%u\n",
                     n,
                     dc2_read_u32(rdram, 0x00376FB8u),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u),
                     getRegU32(ctx, 2));
    }
}

static void f49_menu_loop_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gp + 0xFFFF8AE0u);
    const uint32_t preVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const uint32_t preDebugFlag = dc2_read_u32(rdram, 0x00376FB8u);
    const uint32_t preMenuMode = dc2_read_u32(rdram, gp + 0xFFFF8B10u); // gp-0x74F0 menu_mode
    const uint32_t preSelect = dc2_read_u32(rdram, gp + 0xFFFF8B24u);  // gp-0x74DC select_1302
    const int32_t preArg = static_cast<int32_t>(dc2_read_u32(
        rdram, 0x00335430u + ((preSelect <= 13u) ? preSelect : 0u) * 4u)); // SelectArg[select]
    const uint32_t prePad = dc2_read_u32(rdram, 0x003D76E4u);          // CGamePad+0x04 current
    const uint32_t prePadPrev = dc2_read_u32(rdram, 0x003D777Cu);      // CGamePad+0x9C previous
    if (f50_6_debug_menu_enabled() && (n <= 64u || prePad != 0u))
    {
        std::fprintf(stderr,
                     "[F50.6:menu] tag=MenuLoop.enter n=%u frame=%u flag=%u loop=%u next=%u mode=%u select=%u arg=%d pad=0x%x down=0x%x\n",
                     n, g_f40_frame_counter, preDebugFlag, loopNo, nextLoopNo, preMenuMode,
                     preSelect, preArg, prePad, prePad & ~prePadPrev);
    }
    const auto t0 = std::chrono::steady_clock::now();
    MenuLoop__Fv_0x191c30(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f50_6_debug_menu_enabled() && (n <= 64u || getRegU32(ctx, 2) != 0u))
    {
        const uint32_t postSelect = dc2_read_u32(rdram, gp + 0xFFFF8B24u);
        const int32_t postArg = static_cast<int32_t>(dc2_read_u32(
            rdram, 0x00335430u + ((postSelect <= 13u) ? postSelect : 0u) * 4u));
        std::fprintf(stderr,
                     "[F50.6:menu] tag=MenuLoop.exit n=%u frame=%u ret=%u flag=%u loop=%u next=%u mode=%u select=%u arg=%d\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 2),
                     dc2_read_u32(rdram, 0x00376FB8u),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u),
                     dc2_read_u32(rdram, gp + 0xFFFF8B10u),
                     postSelect, postArg);
    }
    if (f49_trace_loop_timing_enabled())
    {
        const uint32_t postVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t dtVsync = postVsync - preVsync;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 16u || (n % 60u) == 0u || dtMs >= 50)
        {
            std::fprintf(stderr,
                         "[F49:loop] tag=MenuLoop n=%u ra=0x%x loop=%u next=%u ret=%u dtMs=%lld dv=%u\n",
                         n, getRegU32(ctx, 31), loopNo, nextLoopNo, getRegU32(ctx, 2),
                         static_cast<long long>(dtMs), dtVsync);
        }
    }
}

static void f49_menu_main_loop_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = dc2_read_u32(rdram, gp + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gp + 0xFFFF8AE0u);
    const uint32_t preVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const auto t0 = std::chrono::steady_clock::now();
    MenuMainLoop__Fv_0x233fc0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_loop_timing_enabled())
    {
        const uint32_t postVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t dtVsync = postVsync - preVsync;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 16u || (n % 60u) == 0u || dtMs >= 50)
        {
            std::fprintf(stderr,
                         "[F49:loop] tag=MenuMainLoop n=%u ra=0x%x loop=%u next=%u ret=%u dtMs=%lld dv=%u\n",
                         n, getRegU32(ctx, 31), loopNo, nextLoopNo, getRegU32(ctx, 2),
                         static_cast<long long>(dtMs), dtVsync);
        }
    }
}

static void f49_pause_loop_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pauseReqAddr = gp + 0xFFFFA1A8u; // -0x5E58
    const uint32_t pauseReqBefore = dc2_read_u32(rdram, pauseReqAddr);
    const auto t0 = std::chrono::steady_clock::now();
    PauseLoop__Fv_0x309de0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_loop_timing_enabled())
    {
        const uint32_t pauseReqAfter = dc2_read_u32(rdram, pauseReqAddr);
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const uint32_t ret = getRegU32(ctx, 2);
        if (n <= 16u || ret != 0u || (n % 120u) == 0u || dtMs >= 10)
        {
            std::fprintf(stderr,
                         "[F49:pause] n=%u ra=0x%x ret=%u pauseReq=%u->%u dtMs=%lld\n",
                         n, getRegU32(ctx, 31), ret, pauseReqBefore, pauseReqAfter,
                         static_cast<long long>(dtMs));
        }
    }
}

static void f49_update_gamepad_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const auto t0 = std::chrono::steady_clock::now();
    UpDate__8CGamePadFv_0x14a930(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f50_6_enable_debug_menu(rdram, ctx, "UpDate");
    if (f49_trace_mainloop_hotspots_enabled())
    {
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 8u || dtUs >= 1000 || (n % 600u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:hot] tag=UpDate n=%u ra=0x%x dtUs=%lld\n",
                         n, getRegU32(ctx, 31), static_cast<long long>(dtUs));
        }
    }
}

static void f49_update_pad_control_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const auto t0 = std::chrono::steady_clock::now();
    Update__11CPadControlFP8CGamePad_0x2ed550(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_mainloop_hotspots_enabled())
    {
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 8u || dtUs >= 1000 || (n % 600u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:hot] tag=PadControlUpdate n=%u ra=0x%x dtUs=%lld\n",
                         n, getRegU32(ctx, 31), static_cast<long long>(dtUs));
        }
    }
}

static void f49_snd_step_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const auto t0 = std::chrono::steady_clock::now();
    sndStep__Ff_0x18d650(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_mainloop_hotspots_enabled())
    {
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 8u || dtUs >= 1000 || (n % 600u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:hot] tag=sndStep n=%u ra=0x%x dtUs=%lld\n",
                         n, getRegU32(ctx, 31), static_cast<long long>(dtUs));
        }
    }
}

static void f49_scene_step_snd_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const auto t0 = std::chrono::steady_clock::now();
    StepSnd__6CSceneFv_0x2a7940(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_mainloop_hotspots_enabled())
    {
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 8u || dtUs >= 1000 || (n % 600u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:hot] tag=SceneStepSnd n=%u ra=0x%x dtUs=%lld\n",
                         n, getRegU32(ctx, 31), static_cast<long long>(dtUs));
        }
    }
}

static int32_t f49_title_phase(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t raw = dc2_read_u32(rdram, gp + 0xFFFF99A8u) & 0xFFFFu;
    return static_cast<int16_t>(raw);
}

static uint32_t f49_title_mode(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t titleInfo = dc2_read_u32(rdram, gp + 0xFFFF997Cu);
    return titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
}

static void f49_title_mode_key_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const int32_t phaseBefore = f49_title_phase(rdram, ctx);
    const uint32_t modeBefore = f49_title_mode(rdram, ctx);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=TitleModeKey.enter n=%u ra=0x%x phase=%d mode=%u\n",
                     n, ra, phaseBefore, modeBefore);
    }

    const auto t0 = std::chrono::steady_clock::now();
    TitleModeKey__Fv_0x2a1220(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    const uint32_t pcAfter = ctx->pc;
    const uint64_t rawRet64 = static_cast<uint64_t>(_mm_extract_epi64(ctx->r[2], 0));
    // Some recompiled callers compare full 64-bit GPRs. Normalize TitleModeKey's
    // return to canonical sign-extended 32-bit form.
    const int32_t rawRet32 = static_cast<int32_t>(getRegU32(ctx, 2));
    int32_t ret32 = rawRet32;
    if (ret32 == 5 && f49_remap_title_ret5_enabled())
        ret32 = 3;
    // G9: headless costume route. Force the New-Game return (5) once at press-start
    // (state 1). TitleLoop case-1 lVar10==5 runs InitSaveData + SetChrEquipDirect (so
    // the costume list is populated) + DAT_01ecd618=0x14 (the Select-Costume selector)
    // + TitleInfo[1]=2, whose commit calls MenuMainInit case 0x14 = MenuCostumeInit.
    if (g9_costume_enabled() && !g_g9_title_forced && modeBefore == 1u && n >= 120u)
    {
        ret32 = 5;
        g_g9_title_forced = true;
        std::fprintf(stderr, "[G9:costume] forced TitleModeKey ret=5 (New Game) at n=%u\n", n);
        std::fflush(stderr);
    }
    setReturnS32(ctx, ret32);

    if (f49_trace_title_path_enabled())
    {
        const int32_t phaseAfter = f49_title_phase(rdram, ctx);
        const uint32_t modeAfter = f49_title_mode(rdram, ctx);
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 256u || dtMs >= 10)
        {
            std::fprintf(stderr,
                         "[F49:title] tag=TitleModeKey.exit n=%u ra=0x%x pc=0x%x rawRet64=0x%llx rawRet=%u ret=%u dtMs=%lld phase=%d->%d mode=%u->%u\n",
                         n, ra, pcAfter, static_cast<unsigned long long>(rawRet64),
                         static_cast<uint32_t>(rawRet32), static_cast<uint32_t>(ret32), static_cast<long long>(dtMs),
                         phaseBefore, phaseAfter, modeBefore, modeAfter);
        }
    }
}

static void f48_log_map_chain(const char *tag, uint8_t *rdram, R5900Context *ctx, uint32_t n)
{
    if (!f39_trace_map_entry_enabled() || n > 128u)
        return;

    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);
    const uint32_t a2 = getRegU32(ctx, 6);
    const uint32_t a3 = getRegU32(ctx, 7);
    const uint32_t sp = getRegU32(ctx, 29);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t ra = getRegU32(ctx, 31);
    const char *path = reinterpret_cast<const char *>(getConstMemPtr(rdram, a1));
    char pathBuf[80] = {0};
    if (path != nullptr)
    {
        std::strncpy(pathBuf, path, sizeof(pathBuf) - 1u);
        pathBuf[sizeof(pathBuf) - 1u] = '\0';
    }

    const uint32_t arg5 = dc2_read_u32(rdram, sp + 0x10u);
    std::fprintf(stderr,
                 "[F48:map] tag=%s n=%u a0=0x%x a1=0x%x a2=0x%x a3=0x%x arg5=0x%x ra=0x%x gp=0x%x path='%s'\n",
                 tag, n, a0, a1, a2, a3, arg5, ra, gp, path ? pathBuf : "");
}

static void f48_log_loadmap_info(const char *tag, uint8_t *rdram, R5900Context *ctx,
                                 uint32_t n, uint32_t info)
{
    if (!f39_trace_map_entry_enabled() || n > 128u || info == 0u)
        return;
    if (getConstMemPtr(rdram, info) == nullptr)
        return;

    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t stack = dc2_read_u32(rdram, info + 0x1A4u);
    const uint32_t readDataGlobal = dc2_read_u32(rdram, gp + 0xFFFF8D74u); // gp-0x728c BuffReadData
    const uint32_t stackBase = stack ? dc2_read_u32(rdram, stack + 0x20u) : 0u;
    const uint32_t stackOff = stack ? dc2_read_u32(rdram, stack + 0x24u) : 0u;
    const uint32_t stackLimit = stack ? dc2_read_u32(rdram, stack + 0x28u) : 0u;

    std::fprintf(stderr,
                 "[F48:map-info] tag=%s n=%u info=0x%x w0=0x%x stackSel=%u data=0x%x misc3=0x%x misc4=0x%x flag67=0x%x map68=0x%x stack69=0x%x stackBase=0x%x stackOff=0x%x stackLimit=0x%x readData=0x%x\n",
                 tag, n, info,
                 dc2_read_u32(rdram, info + 0x00u),
                 dc2_read_u32(rdram, info + 0x04u),
                 dc2_read_u32(rdram, info + 0x08u),
                 dc2_read_u32(rdram, info + 0x0Cu),
                 dc2_read_u32(rdram, info + 0x10u),
                 dc2_read_u32(rdram, info + 0x19Cu),
                 dc2_read_u32(rdram, info + 0x1A0u),
                 stack, stackBase, stackOff, stackLimit, readDataGlobal);
    std::fprintf(stderr,
                 "[F48:map-info] tag=%s n=%u mainInfo=[0x%x,0x%x,0x%x,0x%x,0x%x,0x%x] editStack base=0x%x off=0x%x limit=0x%x raw70=0x%x raw74=0x%x raw78=0x%x\n",
                 tag, n,
                 dc2_read_u32(rdram, 0x01F58D50u),
                 dc2_read_u32(rdram, 0x01F58D54u),
                 dc2_read_u32(rdram, 0x01F58D58u),
                 dc2_read_u32(rdram, 0x01F58D5Cu),
                 dc2_read_u32(rdram, 0x01F58D60u),
                 dc2_read_u32(rdram, 0x01F58D64u),
                 dc2_read_u32(rdram, 0x01E9EA80u + 0x20u),
                 dc2_read_u32(rdram, 0x01E9EA80u + 0x24u),
                 dc2_read_u32(rdram, 0x01E9EA80u + 0x28u),
                 dc2_read_u32(rdram, 0x01E9EA70u),
                 dc2_read_u32(rdram, 0x01E9EA74u),
                 dc2_read_u32(rdram, 0x01E9EA78u));
    std::fprintf(stderr,
                 "[F48:map-info] tag=%s n=%u ptrs map=0x%x mapSize=0x%x cfg=0x%x cfgSize=0x%x mpk=0x%x ipk=0x%x efp=0x%x sky=0x%x total66=0x%x place64=0x%x add36=0x%x\n",
                 tag, n,
                 dc2_read_u32(rdram, info + 0x0B8u),
                 dc2_read_u32(rdram, info + 0x0BCu),
                 dc2_read_u32(rdram, info + 0x0C0u),
                 dc2_read_u32(rdram, info + 0x0C4u),
                 dc2_read_u32(rdram, info + 0x0C8u),
                 dc2_read_u32(rdram, info + 0x0CCu),
                 dc2_read_u32(rdram, info + 0x0D0u),
                 dc2_read_u32(rdram, info + 0x0D4u),
                 dc2_read_u32(rdram, info + 0x198u),
                 dc2_read_u32(rdram, info + 0x190u),
                 dc2_read_u32(rdram, info + 0x0D8u));
}

static void f48_log_map_chain_exit(const char *tag, uint8_t *rdram, R5900Context *ctx,
                                   uint32_t n, uint32_t ret, long long dtMs)
{
    if (!f39_trace_map_entry_enabled() || n > 128u)
        return;

    std::fprintf(stderr,
                 "[F48:map] tag=%s n=%u ret=0x%x pc=0x%x ra=0x%x dtMs=%lld gp=0x%x\n",
                 tag, n, ret, ctx->pc, getRegU32(ctx, 31), dtMs, getRegU32(ctx, 28));
}

static void f48_load_map_from_memory_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t info = getRegU32(ctx, 6);
    f48_log_map_chain("LoadMapFromMemory@285670.enter", rdram, ctx, n);
    f48_log_loadmap_info("LoadMapFromMemory@285670.enter", rdram, ctx, n, info);
    const auto t0 = std::chrono::steady_clock::now();
    LoadMapFromMemory__6CSceneFiP17SCN_LOADMAP_INFO2_0x285670(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f48_log_loadmap_info("LoadMapFromMemory@285670.exit", rdram, ctx, n, info);
    f48_log_map_chain_exit("LoadMapFromMemory@285670.exit", rdram, ctx, n, getRegU32(ctx, 2),
                           std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

static void f48_load_map_from_memory2_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t info = getRegU32(ctx, 7);
    f48_log_map_chain("LoadMapFromMemory@2856f0.enter", rdram, ctx, n);
    f48_log_loadmap_info("LoadMapFromMemory@2856f0.enter", rdram, ctx, n, info);
    const auto t0 = std::chrono::steady_clock::now();
    LoadMapFromMemory__6CSceneFiiP17SCN_LOADMAP_INFO2_0x2856f0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f48_log_loadmap_info("LoadMapFromMemory@2856f0.exit", rdram, ctx, n, info);
    f48_log_map_chain_exit("LoadMapFromMemory@2856f0.exit", rdram, ctx, n, getRegU32(ctx, 2),
                           std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

static void f50_7_seed_debug_map_stacks(uint8_t *rdram, uint32_t scene, uint32_t stackSel)
{
    if (!f50_6_debug_menu_enabled() || scene != 0x01DD8260u)
        return;

    static constexpr uint32_t kSceneStackCount = 0x0Cu;
    static constexpr uint32_t kStacks[] = {
        0x01E9EA50u, 0x01E9EA80u, 0x01E9EAB0u, 0x01E9EAE0u, 0x01E9EB10u,
        0x01E9EB40u, 0x01E9EB70u, 0x01E9EBA0u, 0x01E9EBD0u,
    };

    const uint32_t countBefore = dc2_read_u32(rdram, scene + 0x00u);
    const uint32_t mapCountBefore = dc2_read_u32(rdram, scene + 0x27E0u);
    const uint32_t requestedBefore =
        (stackSel < kSceneStackCount) ? dc2_read_u32(rdram, scene + 0x08u + stackSel * 4u) : 0u;
    uint32_t repaired = 0u;

    if (countBefore < kSceneStackCount)
        dc2_write_u32(rdram, scene + 0x00u, kSceneStackCount);

    for (uint32_t i = 0u; i < static_cast<uint32_t>(sizeof(kStacks) / sizeof(kStacks[0])); ++i)
    {
        const uint32_t slotAddr = scene + 0x08u + i * 4u;
        if (dc2_read_u32(rdram, slotAddr) != 0u)
            continue;
        const uint32_t stack = kStacks[i];
        if (dc2_read_u32(rdram, stack + 0x20u) == 0u || dc2_read_u32(rdram, stack + 0x28u) == 0u)
            continue;
        dc2_write_u32(rdram, slotAddr, stack);
        ++repaired;
    }

    if (mapCountBefore == 0u)
    {
        dc2_write_u32(rdram, scene + 0x0040u, 0x80u); // character slots
        dc2_write_u32(rdram, scene + 0x2044u, 8u);    // camera slots
        dc2_write_u32(rdram, scene + 0x2208u, 8u);    // message slots
        dc2_write_u32(rdram, scene + 0x27E0u, 4u);    // map slots
        dc2_write_u32(rdram, scene + 0x28C4u, 4u);    // sky slots
        dc2_write_u32(rdram, scene + 0x29A8u, 4u);    // game object slots
        dc2_write_u32(rdram, scene + 0x2AACu, 8u);    // effect slots
        dc2_write_u32(rdram, scene + 0x2E50u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, scene + 0x2E54u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, scene + 0x2E58u, 0xFFFFFFFFu);
        dc2_write_u32(rdram, scene + 0x2E5Cu, 0xFFFFFFFFu);

        if (uint8_t *mapSlots = getMemPtr(rdram, scene + 0x27E4u))
            std::memset(mapSlots, 0, 4u * 0x38u);
        if (uint8_t *skySlots = getMemPtr(rdram, scene + 0x28C8u))
            std::memset(skySlots, 0, 4u * 0x38u);
    }

    if (f50_7_trace_selected_map_enabled() && (repaired != 0u || mapCountBefore == 0u))
    {
        std::fprintf(stderr,
                     "[F50.7:map-stack] scene=0x%x count=%u->%u mapCount=%u->%u stackSel=%u slot=0x%x->0x%x repaired=%u e1.base=0x%x e1.off=0x%x e1.limit=0x%x\n",
                     scene, countBefore, dc2_read_u32(rdram, scene + 0x00u),
                     mapCountBefore, dc2_read_u32(rdram, scene + 0x27E0u),
                     stackSel, requestedBefore,
                     (stackSel < kSceneStackCount) ? dc2_read_u32(rdram, scene + 0x08u + stackSel * 4u) : 0u,
                     repaired,
                     dc2_read_u32(rdram, 0x01E9EA80u + 0x20u),
                     dc2_read_u32(rdram, 0x01E9EA80u + 0x24u),
                     dc2_read_u32(rdram, 0x01E9EA80u + 0x28u));
    }
}

static void f55_read_cstr(uint8_t *rdram, uint32_t addr, char *out, uint32_t cap); // fwd (defined below)

static bool g53_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G53");
    return enabled;
}

// G53: the main-title 3D background is map "s19", loaded by TitleInit@0x29f1f0 ->
// LoadMap@0x285ce0 (synchronous, last arg 0). LoadMap reads info+0x1a4 =
// GetStack(scene, info[1]); the required .ipk/.efp map files take their destination
// buffer from that stack (*(stack+0x20)+*(stack+0x24)*0x10). In the runner the title
// scene (MainScene @0x1dd8260) boots with its stack-slot table + slot counts ZERO (the
// scene's Initialize/__sinit does not fully run headless ג€” the same gap the debug-menu
// path patches via f50_7_seed_debug_map_stacks), so TitleInit's own
// SetStack(scene,1,0x1f060a0) no-ops (count==0) -> GetStack returns 0 -> the required
// .ipk load gets a NULL buffer -> LoadMapData fails -> the map is never built -> the
// title renders an empty (black) 3D scene. TitleInit DID allocate + fund the map stack
// at 0x1f060a0 (verified base!=0), so we just have to wire it into the scene's slot and
// give the scene non-zero slot counts so LoadMap/LoadMapFromMemory proceed. Self-gated on
// the slot being null (the broken title state) so the already-populated in-game dungeon
// scene is never touched. Kill switch DC2_G53_NO_SEED.
// G55: replicate Initialize__11CMdsListSetFv@0x1690d0 on the title scene's embedded
// CMdsListSet (scene+0x23d0 = 0x1dda630). The headless title scene's Initialize__6CScene
// does not fully run (the same gap G53 worked around for the map stack), so this set keeps
// count=0 (HW=8). With 0 slots, LoadPCPFile__11CMdsListSet cannot place the loaded s19_0.pcp
// model list -> SearchMDS fails -> map pieces get no visual -> all parts cull -> zero 3D
// geometry on the title. Sizing it to 8 empty slots (matching HW) lets LoadData's PCP loop
// fill slot for the title model pack. Gated on count==0 so a populated set is never wiped.
static void g55_init_title_mds_list_set(uint8_t *rdram, uint32_t scene)
{
    static const bool disabled = dc2_env_flag_enabled("DC2_G55_NO_MDS_INIT");
    if (disabled)
        return;
    const uint32_t set = scene + 0x23D0u; // MainScene 0x1dd8260 -> 0x1dda630
    if (dc2_read_u32(rdram, set + 0x00u) != 0u)
        return; // already initialized (HW count=8) ג€” do not clobber loaded lists

    dc2_write_u32(rdram, set + 0x00u, 8u); // 8 MDS list slots
    for (uint32_t i = 0u; i < 8u; ++i)
    {
        const uint32_t s = set + 0x10u + i * 0x10u;
        dc2_write_u32(rdram, s + 0x00u, 0u);
        dc2_write_u32(rdram, s + 0x04u, 0u);
        dc2_write_u32(rdram, s + 0x08u, 0u);
    }
    dc2_write_u32(rdram, set + 0x90u, 0x10u); // param_1[0x24] = 16 IMG-list slots
    for (uint32_t i = 0u; i < 0x10u; ++i)
    {
        const uint32_t s = set + 0x94u + i * 0x08u;
        dc2_write_u32(rdram, s + 0x00u, 0u);
        dc2_write_u32(rdram, s + 0x04u, 0u);
    }
    if (g53_trace_enabled())
        std::fprintf(stderr, "[G55:mdsinit] scene=0x%x set=0x%x sized to 8 slots\n", scene, set);
}

static void g53_seed_title_map_stack(uint8_t *rdram, uint32_t scene, uint32_t stackSel)
{
    static const bool disabled = dc2_env_flag_enabled("DC2_G53_NO_SEED");
    if (disabled || scene != 0x01DD8260u || stackSel >= 0x0Cu)
        return;

    // G55: size the title scene's MDS model table before the map loads (independent of the
    // stack-slot early-return below, which skips on re-entry once the slot is wired).
    g55_init_title_mds_list_set(rdram, scene);

    const uint32_t slotAddr = scene + 0x08u + stackSel * 4u;
    if (dc2_read_u32(rdram, slotAddr) != 0u)
        return; // slot already wired (dungeon / already-seeded) ג€” leave it alone

    // The map stack TitleInit prepared (SetStack(TitleScene,1,0x1f060a0)). Only adopt it
    // if it is actually funded (base ptr set), so we never point the slot at garbage.
    const uint32_t titleMapStack = 0x01F060A0u;
    if (dc2_read_u32(rdram, titleMapStack + 0x20u) == 0u)
        return;

    if (dc2_read_u32(rdram, scene + 0x00u) < 0x0Cu)
        dc2_write_u32(rdram, scene + 0x00u, 0x0Cu); // stack-slot count
    dc2_write_u32(rdram, slotAddr, titleMapStack);

    // Scene slot-count config: only when unset, so LoadMapFromMemory has map/sky/object/
    // effect slots to populate. NOTE: unlike the debug seed we do NOT touch the +0x2e50..5c
    // index fields or memset the slot arrays ג€” the title sets its own current-camera index
    // (TitleMapDraw reads *(scene+0x2e54) as the camera id; the title's slot 0 = TitleCamera),
    // and forcing those to 0xFFFFFFFF broke GetCamera and suppressed the map draw.
    if (dc2_read_u32(rdram, scene + 0x27E0u) == 0u)
    {
        dc2_write_u32(rdram, scene + 0x0040u, 0x80u); // character slots
        dc2_write_u32(rdram, scene + 0x2044u, 8u);    // camera slots
        dc2_write_u32(rdram, scene + 0x2208u, 8u);    // message slots
        dc2_write_u32(rdram, scene + 0x27E0u, 4u);    // map slots
        dc2_write_u32(rdram, scene + 0x28C4u, 4u);    // sky slots
        dc2_write_u32(rdram, scene + 0x29A8u, 4u);    // game object slots
        dc2_write_u32(rdram, scene + 0x2AACu, 8u);    // effect slots
    }

    if (g53_trace_enabled())
        std::fprintf(stderr, "[G53:seed] scene=0x%x slot=%u <- 0x%x (title map stack)\n",
                     scene, stackSel, titleMapStack);
}

static void f50_7_load_map_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t scene = getRegU32(ctx, 4);
    const uint32_t info = getRegU32(ctx, 6);
    const uint32_t stackSel = info ? dc2_read_u32(rdram, info + 0x04u) : 0xFFFFFFFFu;
    f50_7_seed_debug_map_stacks(rdram, scene, stackSel);
    g53_seed_title_map_stack(rdram, scene, stackSel);
    // G53: title 3D background = map "s19" loaded via TitleInit->LoadMap. Confirm the
    // title's LoadMap actually fires and what it leaves in the scene's map count.
    if (g53_trace_enabled())
    {
        const uint32_t mapId = getRegU32(ctx, 5);
        const uint32_t cntBefore = dc2_read_u32(rdram, scene + 0x00u);
        const uint32_t mapCntBefore = dc2_read_u32(rdram, scene + 0x27E0u);
        // SCN_LOADMAP_INFO2: +0x24 = first-entry "present" flag (LoadMapData breaks if 0),
        // +0x28 = dir string, +0x48 = base name. Empty +0x24 => GetLoadMapInfo got a bad
        // map number (SearchMapNo failed) => no map files requested.
        const uint32_t e0present = info ? dc2_read_u32(rdram, info + 0x24u) : 0xFFFFFFFFu;
        char dir[64] = {0}, base[64] = {0};
        if (info) { f55_read_cstr(rdram, info + 0x28u, dir, sizeof(dir));
                    f55_read_cstr(rdram, info + 0x48u, base, sizeof(base)); }
        LoadMap__6CSceneFiP17SCN_LOADMAP_INFO2i_0x285ce0(rdram, ctx, runtime);
        // info+0x1a4 (=info[0x69]) = LoadMap's GetStack(scene,stackSel); the .ipk/.efp dest
        // buffers derive from *(stackPtr+0x20)+*(stackPtr+0x24)*0x10. A null stackPtr or null
        // stack base => NULL .ipk buffer => required-file load fails => map never built.
        const uint32_t stackPtr = info ? dc2_read_u32(rdram, info + 0x1A4u) : 0u;
        const uint32_t stackBase = stackPtr ? dc2_read_u32(rdram, stackPtr + 0x20u) : 0u;
        const uint32_t stackCur = stackPtr ? dc2_read_u32(rdram, stackPtr + 0x24u) : 0u;
        const uint32_t sceneSlot = (stackSel < 8u) ? dc2_read_u32(rdram, scene + 0x08u + stackSel * 4u) : 0u;
        // Candidate title-scene stacks allocated by TitleInit (305249/305251) and their bases.
        const uint32_t t_a0_base = dc2_read_u32(rdram, 0x01F060A0u + 0x20u);
        const uint32_t t_a0_cur  = dc2_read_u32(rdram, 0x01F060A0u + 0x24u);
        const uint32_t t_d0_base = dc2_read_u32(rdram, 0x01F060D0u + 0x20u);
        const uint32_t t_70_base = dc2_read_u32(rdram, 0x01F06070u + 0x20u);
        std::fprintf(stderr,
                     "[G53:loadmap] scene=0x%x mapId=%u stackSel=%u ra=0x%x e0present=0x%x dir='%s' base='%s' cnt=%u->%u mapCnt=%u->%u stackPtr=0x%x stackBase=0x%x stackCur=0x%x sceneSlot=0x%x | a0base=0x%x a0cur=0x%x d0base=0x%x m70base=0x%x ret=0x%x\n",
                     scene, mapId, stackSel, getRegU32(ctx, 31), e0present, dir, base,
                     cntBefore, dc2_read_u32(rdram, scene + 0x00u),
                     mapCntBefore, dc2_read_u32(rdram, scene + 0x27E0u),
                     stackPtr, stackBase, stackCur, sceneSlot,
                     t_a0_base, t_a0_cur, t_d0_base, t_70_base,
                     getRegU32(ctx, 2));
        std::fflush(stderr);
        return;
    }
    LoadMap__6CSceneFiP17SCN_LOADMAP_INFO2i_0x285ce0(rdram, ctx, runtime);
}

static bool f50_7_is_mapfunc_pos_return(uint32_t ra)
{
    return ra == 0x00164188u || ra == 0x001641C0u || ra == 0x001641CCu ||
           ra == 0x00164300u || ra == 0x00164368u;
}

static void f50_7_copy_vec4(uint8_t *rdram, uint32_t dst, uint32_t src)
{
    for (uint32_t i = 0u; i < 4u; ++i)
        dc2_write_u32(rdram, dst + i * 4u, dc2_read_u32(rdram, src + i * 4u));
}

static void f50_7_funcpoint_setter_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                         const char *tag, uint32_t storeOff, uint32_t slotOff,
                                         PS2Runtime::RecompiledFunction originalFn,
                                         PS2Runtime::RecompiledFunction fallbackFn)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t vec = getRegU32(ctx, 5);
    const uint32_t ra = getRegU32(ctx, 31);

    if (!f50_6_debug_menu_enabled() || !f50_7_is_mapfunc_pos_return(ra) || self == 0u || vec == 0u)
    {
        originalFn(rdram, ctx, runtime);
        return;
    }

    const uint32_t frame = self + 0x70u;
    uint32_t vt = dc2_read_u32(rdram, frame);
    uint32_t slot = vt ? dc2_read_u32(rdram, vt + slotOff) : 0u;
    bool repairedVt = false;
    if (vt == 0u || slot == 0u)
    {
        dc2_write_u32(rdram, frame, 0x00374F50u);
        vt = dc2_read_u32(rdram, frame);
        slot = vt ? dc2_read_u32(rdram, vt + slotOff) : 0u;
        repairedVt = true;
    }

    if (f50_7_trace_selected_map_enabled() && (n <= 128u || repairedVt || slot == 0u))
    {
        std::fprintf(stderr,
                     "[F50.7:funcpoint] tag=%s n=%u self=0x%x frame=0x%x vec=0x%x ra=0x%x vt=0x%x slotOff=0x%x slot=0x%x repair=%u v=(0x%x,0x%x,0x%x,0x%x)\n",
                     tag, n, self, frame, vec, ra, vt, slotOff, slot, repairedVt ? 1u : 0u,
                     dc2_read_u32(rdram, vec + 0x00u),
                     dc2_read_u32(rdram, vec + 0x04u),
                     dc2_read_u32(rdram, vec + 0x08u),
                     dc2_read_u32(rdram, vec + 0x0Cu));
    }

    f50_7_copy_vec4(rdram, self + storeOff, vec);

    const auto savedRa = ctx->r[31];
    f50_set_reg(ctx, 4, frame);
    f50_set_reg(ctx, 5, vec);
    f50_set_reg(ctx, 31, 0u);

    PS2Runtime::RecompiledFunction targetFn = fallbackFn;
    if (slot != 0u && runtime->hasFunction(slot))
        targetFn = runtime->lookupFunction(slot);
    targetFn(rdram, ctx, runtime);

    ctx->r[31] = savedRa;
    ctx->pc = ra;
}

static void f50_7_funcpoint_set_scale_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_funcpoint_setter_probe(rdram, ctx, runtime, "SetScale@1643a0",
                                 0x1A0u, 0x28u,
                                 SetScale__10CFuncPointFPf_0x1643a0,
                                 SetScale__9mgCObjectFPf_0x136340);
}

static void f50_7_funcpoint_set_rotation_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_funcpoint_setter_probe(rdram, ctx, runtime, "SetRotation@1643c0",
                                 0x190u, 0x1Cu,
                                 SetRotation__10CFuncPointFPf_0x1643c0,
                                 SetRotation__9mgCObjectFPf_0x136270);
}

static void f50_7_funcpoint_set_position_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_funcpoint_setter_probe(rdram, ctx, runtime, "SetPosition@1643e0",
                                 0x180u, 0x10u,
                                 SetPosition__10CFuncPointFPf_0x1643e0,
                                 SetPosition__9mgCObjectFPf_0x136190);
}

static void f50_7_mg_active_lighting_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t lightNo = getRegU32(ctx, 4);
    const uint32_t active = getRegU32(ctx, 5);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);

    if (f50_7_trace_selected_map_enabled() && f50_6_debug_menu_enabled() && n <= 128u)
    {
        std::fprintf(stderr,
                     "[F50.7:tailfix] tag=mgActiveLighting@143720 n=%u light=%u active=%u ra=0x%x gp=0x%x\n",
                     n, lightNo, active, ra, gp);
    }

    dc2_write_u32(rdram, gp - 0x77E0u, 1u);

    const auto savedRa = ctx->r[31];
    f50_set_reg(ctx, 4, 0x00380EC0u);
    f50_set_reg(ctx, 5, lightNo);
    f50_set_reg(ctx, 6, active);
    f50_set_reg(ctx, 31, 0u);
    ActiveLighting__13mgRENDER_INFOFii_0x139120(rdram, ctx, runtime);

    ctx->r[31] = savedRa;
    ctx->pc = ra;
}

static void f50_7_map_draw_tailfix_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                         const char *tag, uint32_t directFlag)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t vt = self ? dc2_read_u32(rdram, self + 0x0D00u) : 0u;
    const uint32_t slot = vt ? dc2_read_u32(rdram, vt + 0x08u) : 0u;

    if (f50_7_trace_selected_map_enabled() && f50_6_debug_menu_enabled() && n <= 128u)
    {
        std::fprintf(stderr,
                     "[F50.7:tailfix] tag=%s n=%u self=0x%x direct=%u ra=0x%x vt=0x%x slot=0x%x\n",
                     tag, n, self, directFlag, ra, vt, slot);
    }

    if (slot == 0u || !runtime->hasFunction(slot))
    {
        ctx->pc = slot;
        return;
    }

    const auto savedRa = ctx->r[31];
    f50_set_reg(ctx, 4, self);
    f50_set_reg(ctx, 5, directFlag);
    f50_set_reg(ctx, 31, 0u);
    runtime->lookupFunction(slot)(rdram, ctx, runtime);

    const uint32_t postPc = ctx->pc;
    ctx->r[31] = savedRa;
    ctx->pc = (postPc == 0u) ? ra : postPc;
}

static void f50_7_map_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_map_draw_tailfix_probe(rdram, ctx, runtime, "Draw@160b10", 0u);
}

static void f50_7_map_draw_direct_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    f50_7_map_draw_tailfix_probe(rdram, ctx, runtime, "DrawDirect@160b30", 1u);
}

// ---------------------------------------------------------------------------
// PHASE G56: trace the main-title map per-part/per-piece geometry submission.
// The title 3D background (map s19) renders through the deferred mgCDrawManager
// queue. Chain (all indirect-dispatched, so registerFunction fires):
//   TitleMapDraw -> (TitleMap+0xd00 vt+0xc) Draw__4CMap@0x160b10 ->
//   DrawSub__8CEditMap@0x1b4130 -> DrawSub__4CMap@0x15e250 (iterates the 6-entry
//   visible draw list at map+0x364) -> part vt+0x34 Draw__9CMapParts@0x15e3d0 ->
//   DrawSub__9CMapParts@0x166a70 { gate part vt+0x40 = PreDraw__9CMapParts@0x166a00;
//   per-piece embedded vt+0x34 = Draw__9CMapPiece@0x166e40 -> Draw__12CObjectFrame
//   (mgCFrame/mgCVisualMDT geometry emit, queued into mgDrawManager) }.
// G55 made the draw list = 6 (HW-identical) yet [F31:prim] still shows 0 tri. These
// taps count entries + return value to pinpoint where the chain stops emitting
// geometry. Transparent (delegate to the real recompiled body); only the gated log
// changes anything. Gated DC2_TRACE_G56. HW dispatch reference (live PCSX2):
//   part vtable 0x3754d0: +0x34=0x15e3d0 +0x40=0x166a00; piece+0x80 visual=0x903ce0
//   (gate +0x54==0); piece embedded vtable 0x375570: +0x34=0x166e40.
// (externs for the recompiled bodies live in the global extern block near the top.)

static bool g56_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G56");
    return enabled;
}

static void g56_chain_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                          const char *tag,
                          void (*real)(uint8_t *, R5900Context *, PS2Runtime *),
                          unsigned &counter, unsigned &nonzero)
{
    const uint32_t a0 = getRegU32(ctx, 4);
    const unsigned n = ++counter;
    real(rdram, ctx, runtime);
    const uint32_t ret = getRegU32(ctx, 2);
    if (ret != 0u) ++nonzero;
    if (g56_trace_enabled() && (n <= 18u || (n % 1200u) == 0u))
    {
        std::fprintf(stderr, "[G56:%s] n=%u nz=%u a0=0x%x ret=0x%x\n",
                     tag, n, nonzero, a0, ret);
        std::fflush(stderr);
    }
}

static void g56_mapparts_draw_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static unsigned c = 0u, nz = 0u;
    g56_chain_tap(rdram, ctx, runtime, "parts.draw", Draw__9CMapPartsFv_0x15e3d0, c, nz);
}

static void g56_mapparts_predraw_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static unsigned c = 0u, nz = 0u;
    g56_chain_tap(rdram, ctx, runtime, "parts.predraw", PreDraw__9CMapPartsFv_0x166a00, c, nz);
}

static void g56_mappiece_draw_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static unsigned c = 0u, nz = 0u;
    g56_chain_tap(rdram, ctx, runtime, "piece.draw", Draw__9CMapPieceFv_0x166e40, c, nz);
}

// G56: tap BeginDraw__14mgCDrawManager (reached via mgBeginDraw's indirect tail-call).
// Captures the mempool + its base (mempool+0x20) on entry and the manager's per-block
// table pointers after init. The title passes mempool 0x1f060d0 (base HW=0xd87340).
// If poolBase==0 the title scene's mempool is unfunded (Initialize gap, like G53/G55);
// if poolBase is valid but m4/m6 stay 0, the Alloc path failed. Gated DC2_TRACE_G56.
static void g56_begindraw_tap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static unsigned c = 0u;
    const uint32_t mgr = getRegU32(ctx, 4);
    const uint32_t mempool = getRegU32(ctx, 5);
    const uint32_t blockArr = getRegU32(ctx, 6);
    const uint32_t poolBase = mempool ? dc2_read_u32(rdram, mempool + 0x20u) : 0u;
    const uint32_t poolEna  = mempool ? dc2_read_u32(rdram, mempool + 0x1Cu) : 0u;
    const uint32_t poolCur0 = mempool ? dc2_read_u32(rdram, mempool + 0x24u) : 0u;
    const uint32_t poolLim  = mempool ? dc2_read_u32(rdram, mempool + 0x28u) : 0u;
    const uint32_t m16in = dc2_read_u32(rdram, mgr + 0x58u);              // bounds object
    const uint32_t m17in = dc2_read_u32(rdram, mgr + 0x5Cu);              // fallback mempool
    const uint32_t m7in  = dc2_read_u32(rdram, mgr + 0x1Cu);              // table[0x14] size
    BeginDraw__14mgCDrawManagerFP9mgCMemoryPi_0x135230(rdram, ctx, runtime);
    const unsigned n = ++c;
    if (g56_trace_enabled() && (n <= 24u || (n % 600u) == 0u))
    {
        const uint32_t m1 = dc2_read_u32(rdram, mgr + 0x04u);
        const uint32_t m3 = dc2_read_u32(rdram, mgr + 0x0Cu);
        const uint32_t m4 = dc2_read_u32(rdram, mgr + 0x10u);
        const uint32_t m6 = dc2_read_u32(rdram, mgr + 0x18u);
        const uint32_t poolCur1 = mempool ? dc2_read_u32(rdram, mempool + 0x24u) : 0u;
        std::fprintf(stderr,
                     "[G56:begindraw] n=%u mgr=0x%x mempool=0x%x base=0x%x ena=%u cur=%u->%u lim=%u | m16=0x%x m17=0x%x m7=%u blockArr=0x%x -> m1=0x%x m3=%u m4=0x%x m6=0x%x\n",
                     n, mgr, mempool, poolBase, poolEna, poolCur0, poolCur1, poolLim,
                     m16in, m17in, m7in, blockArr, m1, m3, m4, m6);
        std::fflush(stderr);
    }
}

// G57 bisection tool (kept for G58): optionally SKIP DrawWater (env DC2_G57_SKIP_WATER)
// to test whether the title $ra corruption (TitleMapDraw_sp+0xA0 <- 0x9f84a0) originates in
// the water draw. RESULT: skipping water does NOT change the crash (same bad=0x9f84a0,
// sp=0x1fff760) -> the corruptor is the GEOMETRY-QUEUE chain, not the water. Default: run.
static void g57_drawwater_skip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool skip = dc2_env_flag_enabled("DC2_G57_SKIP_WATER");
    // G129: the title water pool (HW ~966-1012 trifans) renders 0 trifans on the runner at the
    // GS for the WHOLE frame (DC2_TRACE_G14 [G65:vk] t5=0). DrawWater@0x15e800 gates ALL water
    // drawing on `*(CMap+0xcec) > 0 && tex(a2) != 0 && *(CMap+0xcf4) > 0` (the CWaterFrame array
    // counts + the dest texture). Log the gate inputs so we can tell "water never configured /
    // count 0" (root = CMapWater init / map data) from "configured but trifans lost downstream"
    // (mgDrawDirect / CreatePacket__11CWaterFrameFv VU path). Opt-in, quiet. DC2_G129_WATER.
    static const bool g129water = dc2_env_flag_enabled("DC2_G129_WATER");
    if (g129water)
    {
        const uint32_t cmap = getRegU32(ctx, 4); // a0 = CMap*
        const uint32_t tex  = getRegU32(ctx, 6); // a2 = dest mgCTexture*
        int32_t cnt1 = 0, cnt2 = 0, arr1 = 0, arr2 = 0;
        if (cmap)
        {
            cnt1 = static_cast<int32_t>(dc2_read_u32(rdram, cmap + 0xcecu));
            arr1 = static_cast<int32_t>(dc2_read_u32(rdram, cmap + 0xcf0u));
            cnt2 = static_cast<int32_t>(dc2_read_u32(rdram, cmap + 0xcf4u));
            arr2 = static_cast<int32_t>(dc2_read_u32(rdram, cmap + 0xcf8u));
        }
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 80u)
            std::fprintf(stderr,
                "[G129:water] call=%u CMap=0x%x tex(a2)=0x%x cnt1(+0xcec)=%d arr1(+0xcf0)=0x%x cnt2(+0xcf4)=%d arr2(+0xcf8)=0x%x gate=%s\n",
                n, cmap, tex, cnt1, (unsigned)arr1, cnt2, (unsigned)arr2,
                (cnt1 > 0 && tex != 0 && cnt2 > 0) ? "ENTER" : "SKIP");
    }
    if (skip) return; // empty body => dispatch handler returns to caller (DrawWater skipped)
    DrawWater__4CMapFP9mgCCameraP10mgCTextureP10mgCTexture_0x15e800(rdram, ctx, runtime);
}

// G129: probe the SPI map-config WATER_VERTEX handler. If this fires during the title map load,
// the config script DOES request water (so the gap is downstream: cfgWater -> CMap+0xcf0 storage,
// or wrong CMap). If it never fires, the title map's SPI config never dispatches WATER_VERTEX
// (config not parsed/dispatched, or the title map data lacks it). Delegates to the real handler.
static void g129_cfg_water_vertex_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static const bool g129water = dc2_env_flag_enabled("DC2_G129_WATER");
    cfgWATER_VERTEX__FP9SPI_STACKi_0x1648f0(rdram, ctx, runtime);
    if (g129water)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed);
        if (n < 40u)
            std::fprintf(stderr, "[G129:cfgwater] FIRED call=%u cfgWater(ret v0)=0x%x ra=0x%x\n",
                         n, getRegU32(ctx, 2), getRegU32(ctx, 31));
    }
}

// ---------------------------------------------------------------------------
// PHASE G58: $ra-corruption canary for the title geometry-queue chain.
// TitleMapDraw@0x2a2280 saves $ra at sp+0xA0 = 0x1fff800 (entry $ra=0x2a1b88); the title
// loop dies after ONE frame because that slot is overwritten with 0x9f84a0 (= *(TitleMap
// +0x364), the visible-parts array = DrawSub__4CMap's s0) and the epilogue jr $ra jumps to
// data. G57 ruled DrawWater OUT; the corruptor is in the geometry-queue chain. This canary
// wraps every link, snapshots the fixed slot 0x1fff800 before/after the real body, and logs:
//   - the entry sp of each link (depth profile: a link whose sp >= 0x1fff760 overlaps
//     TitleMapDraw's own frame and can clobber the saved-$ra slot = sp imbalance upstream),
//   - the exact link across which 0x1fff800 flips (innermost flip = culprit boundary).
// The user's A/B (debug-menu + Circle x2 reaches the title WITHOUT this crash) shows the
// corruption is PATH-dependent => consistent with a back-edge-preemption sp leak rather than
// an always-on store-offset bug (the static sp-balance scan of every link found no overshoot
// store). Transparent: delegates to the real body; only the gated log changes anything.
// Gated DC2_TRACE_G58.
static const uint32_t kG58RaSlot = 0x1FFF800u; // TitleMapDraw sp+0xA0 (saved $ra)

static bool g58_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G58");
    return enabled;
}

static void g58_canary(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                       const char *tag,
                       void (*real)(uint8_t *, R5900Context *, PS2Runtime *))
{
    static unsigned s_logBudget = 0u;
    const uint32_t entrySp = getRegU32(ctx, 29);
    const uint32_t slotBefore = dc2_read_u32(rdram, kG58RaSlot);

    if (g58_trace_enabled() && s_logBudget < 4000u)
    {
        // First few entries per tag: capture the call-depth (sp) profile + the slot value.
        static const char *s_seenTags[16] = {nullptr};
        static unsigned s_tagCount[16] = {0u};
        unsigned ti = 0u;
        for (; ti < 16u && s_seenTags[ti]; ++ti)
            if (s_seenTags[ti] == tag) break;
        if (ti < 16u)
        {
            if (!s_seenTags[ti]) s_seenTags[ti] = tag;
            if (s_tagCount[ti] < 8u)
            {
                ++s_tagCount[ti];
                ++s_logBudget;
                std::fprintf(stderr, "[G58:enter] tag=%s entrySp=0x%x slot(0x1fff800)=0x%x%s\n",
                             tag, entrySp, slotBefore,
                             (entrySp >= 0x1FFF760u) ? "  <-- OVERLAPS TitleMapDraw FRAME" : "");
                std::fflush(stderr);
            }
        }
    }

    real(rdram, ctx, runtime);

    const uint32_t slotAfter = dc2_read_u32(rdram, kG58RaSlot);
    const uint32_t exitSp = getRegU32(ctx, 29);
    if (g58_trace_enabled() && slotBefore != slotAfter)
    {
        // Innermost link prints first (it returns first) => first [G58:slotflip] = culprit.
        std::fprintf(stderr,
                     "[G58:slotflip] tag=%s 0x1fff800: 0x%x -> 0x%x | entrySp=0x%x exitSp=0x%x pc=0x%x\n",
                     tag, slotBefore, slotAfter, entrySp, exitSp, ctx->pc);
        std::fflush(stderr);
    }
}

static void g58_canary_map_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__4CMap@160b10", f50_7_map_draw_probe); }
static void g58_canary_editmap_drawsub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "DrawSub__8CEditMap@1b4130", DrawSub__8CEditMapFi_0x1b4130); }
static void g58_canary_map_drawsub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "DrawSub__4CMap@15e250", DrawSub__4CMapFi_0x15e250); }
static void g58_canary_parts_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__9CMapParts@15e3d0", g56_mapparts_draw_tap); }
static void g58_canary_parts_drawsub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "DrawSub__9CMapParts@166a70", DrawSub__9CMapPartsFi_0x166a70); }
static void g58_canary_piece_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__9CMapPiece@166e40", g56_mappiece_draw_tap); }
static void g58_canary_objframe_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__12CObjectFrame@169fd0", Draw__12CObjectFrameFv_0x169fd0); }
static void g58_canary_mgframe_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__8mgCFrame@137e10", Draw__8mgCFrameFPUi_0x137e10); }
static void g58_canary_visualmdt_draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_canary(rdram, ctx, runtime, "Draw__12mgCVisualMDT@13f4e0", Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0); }

// G58: log r31 (return addr) + a0 (the mgCCameraFollow `this`) on entry to the title
// camera-follow accessors, and ctx->pc on exit. The crash trace ends with AddHeight x3 then
// a jump to 0x9f84a0; the 3 TitleMapDraw call sites set r31 to constants (0x2a2774/a4/d4), so
// an entry r31 of 0x9f84a0 (or an exit pc of 0x9f84a0) pins where the bad return addr enters.
static void g58_camera_accessor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                const char *tag,
                                void (*real)(uint8_t *, R5900Context *, PS2Runtime *))
{
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t a0Before = getRegU32(ctx, 4);
    const bool repaired = g59_repair_title_camera_arg(rdram, ctx, tag);
    const uint32_t a0 = getRegU32(ctx, 4);
    static unsigned budget = 0u;
    const bool flag = (a0Before == 0u) || repaired || (ra < 0x100000u) || (ra >= 0x300000u); // suspicious ra/this
    real(rdram, ctx, runtime);
    g59_repair_title_map_resume_frame(rdram, ctx, tag);
    const uint32_t outPc = ctx->pc;
    if (g58_trace_enabled() && (flag || outPc == 0x9f84a0u) && budget < 60u)
    {
        ++budget;
        const uint32_t sp = getRegU32(ctx, 29);
        const uint32_t slotLo = dc2_read_u32(rdram, sp + 0xA0u);
        const uint32_t slotHi = dc2_read_u32(rdram, sp + 0xA4u);
        std::fprintf(stderr, "[G58:cam] %s entry_ra=0x%x a0=0x%x usedA0=0x%x repaired=%u sp=0x%x slot=0x%x:%x -> exit_pc=0x%x%s\n",
                     tag, ra, a0Before, a0, repaired ? 1u : 0u,
                     sp, slotHi, slotLo, outPc,
                     (outPc == 0x9f84a0u) ? "  <-- BAD JUMP" : "");
        std::fflush(stderr);
    }
}
static void g58_cam_adddistance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_camera_accessor(rdram, ctx, runtime, "AddDistance@131a20", AddDistance__15mgCCameraFollowFf_0x131a20); }
static void g58_cam_getdistance(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_camera_accessor(rdram, ctx, runtime, "GetDistance@131a10", GetDistance__15mgCCameraFollowFv_0x131a10); }
static void g58_cam_addheight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{ g58_camera_accessor(rdram, ctx, runtime, "AddHeight@131a50", AddHeight__15mgCCameraFollowFf_0x131a50); }

// G58: log AssignCamera(scene,id,camera,name) args + the resulting slot.obj. TitleInit
// assigns the title camera into scene slot 0/1; the runner leaves slot0.obj=0 (HW=0x785f70).
static void g58_assign_camera_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t scene = getRegU32(ctx, 4);
    const uint32_t id    = getRegU32(ctx, 5);
    const uint32_t cam   = getRegU32(ctx, 6);
    const uint32_t name  = getRegU32(ctx, 7);
    const uint32_t ra    = getRegU32(ctx, 31);
    AssignCamera__6CSceneFiP9mgCCameraPc_0x283740(rdram, ctx, runtime);
    const uint32_t ret = getRegU32(ctx, 2);
    const uint32_t slotObj = (scene && id < 8u) ? dc2_read_u32(rdram, scene + 0x2048u + 56u*id + 0x34u) : 0xFFFFFFFFu;
    static unsigned n = 0u;
    if (g58_trace_enabled() && n < 40u)
    {
        ++n;
        std::fprintf(stderr, "[G58:assigncam] scene=0x%x id=%u cam=0x%x name=0x%x ra=0x%x -> ret=%d slot%u.obj=0x%x\n",
                     scene, id, cam, name, ra, (int)ret, id, slotObj);
        std::fflush(stderr);
    }
}

static void f48_load_map_file_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    f48_log_map_chain("LoadMapFile@164480.enter", rdram, ctx, n);
    const auto t0 = std::chrono::steady_clock::now();
    LoadMapFile__4CMapFPciP9mgCMemoryi_0x164480(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f48_log_map_chain_exit("LoadMapFile@164480.exit", rdram, ctx, n, getRegU32(ctx, 2),
                           std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

static void f48_create_map_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    f48_log_map_chain("CreateMap@1600d0.enter", rdram, ctx, n);
    const auto t0 = std::chrono::steady_clock::now();
    CreateMap__4CMapFP11CMdsListSetP9mgCMemory_0x1600d0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    f48_log_map_chain_exit("CreateMap@1600d0.exit", rdram, ctx, n, getRegU32(ctx, 2),
                           std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
}

static void f49_title_mccheck_key_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=TitleMCCheckKey.enter n=%u ra=0x%x\n",
                     n, ra);
    }

    const auto t0 = std::chrono::steady_clock::now();
    TitleMCCheckKey__Fv_0x2a2ad0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 256u || dtMs >= 10)
        {
            std::fprintf(stderr,
                         "[F49:title] tag=TitleMCCheckKey.exit n=%u ra=0x%x ret=%u dtMs=%lld\n",
                         n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs));
        }
    }
}

static void f49_mc_manager_step_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t self = getRegU32(ctx, 4);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MCStep.enter n=%u ra=0x%x self=0x%x\n",
                     n, ra, self);
    }

    const auto t0 = std::chrono::steady_clock::now();
    Step__18CMemoryCardManagerFv_0x2f1fc0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 256u || dtMs >= 10)
        {
            std::fprintf(stderr,
                         "[F49:title] tag=MCStep.exit n=%u ra=0x%x self=0x%x ret=%u dtMs=%lld\n",
                         n, ra, self, getRegU32(ctx, 2), static_cast<long long>(dtMs));
        }
    }
}

static void f49_menu_check_push_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    MenuCheckPushButton__Fv_0x23e1b0(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MenuCheckPush n=%u ra=0x%x ret=0x%x\n",
                     n, ra, getRegU32(ctx, 2));
    }
}

static void f49_convert_check_push_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t in = getRegU32(ctx, 4);
    ConvertCheckPushButton__Fi_0x23e2e0(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled() && n <= 256u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=ConvertPush n=%u ra=0x%x in=0x%x out=0x%x\n",
                     n, ra, in, getRegU32(ctx, 2));
    }
}

static void f49_menu_main_init_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t arg = getRegU32(ctx, 4);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MenuMainInit.enter n=%u ra=0x%x arg=0x%x\n",
                     n, ra, arg);
    }

    const auto t0 = std::chrono::steady_clock::now();
    MenuMainInit__FP13MENU_INIT_ARG_0x232df0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=MenuMainInit.exit n=%u ra=0x%x ret=0x%x dtMs=%lld\n",
                     n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs));
    }
}

// G9: headless Select-Costume route. Scripted pad input cannot pass MenuMainKey
// (route-B / F49 blocker), so we drive the MenuMain state machine directly. At the
// title New-Game/Load menu (LoopNo=3, TitleInfo[0]=2) we set MenuCommonInfo+0x50=0x14
// (the costume-select init selector) and invoke MenuMainInit, which runs case 0x14 =
// MenuCostumeInit and advances +0x54 to 0x17 (the costume KeyStep). From then on the
// game's own per-frame MenuMainKey runs the costume menu. Env-gated DC2_G9_COSTUME so
// the golden title/treemap smoke is never perturbed.
static bool g9_costume_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_G9_COSTUME");
    return e;
}

static bool g36_packet_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G36_PACKET");
    return enabled;
}

static bool g43_face_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G43_FACE");
    return enabled;
}
static bool g9_auto_confirm_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_G9_AUTO_CONFIRM");
    return enabled;
}
static int g_g9_phase = 0;     // 0=warmup, 1=armed, 2=done
static uint32_t g_g9_warm = 0u;
// G10: optional phase-4 Cross pulse for post-prompt experiments. Keep the default
// costume capture parked on the "Select Max's costume." prompt so it matches the
// hardware reference; set DC2_G9_AUTO_CONFIRM=1 to advance into the interactive list.
static std::atomic<uint32_t> g_g9_confirm_mask{0u};

static void f49_menu_main_key_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static uint32_t lastTarget = 0xFFFFFFFFu;
    static uint32_t lastStep = 0xFFFFFFFFu;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t preMenuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6B08
    const uint32_t preMenuStep = preMenuPtr ? dc2_read_u32(rdram, preMenuPtr + 0x54u) : 0xFFFFFFFFu;
    const uint32_t tableTarget = (preMenuStep < 128u) ? dc2_read_u32(rdram, 0x00350900u + preMenuStep * 4u) : 0u;
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MenuMainKey.enter n=%u ra=0x%x\n",
                     n, ra);
    }
    // G12 FIX: __sinit_menudraw.cpp@0x374310 (which Set's the front-end menu UV rects)
    // does not run headless, so menu_long_hand@0x1ECCE50 ג€” the long-hand cursor's atlas
    // UV rect ג€” stays (0,0,0,0). MenuCursorDraw@0x223a50 then samples a zero-size region
    // at texel (0,0) of the fukusel atlas (block 0x2c20), so the Select-Costume / menu
    // cursor renders as a solid black-transparent square instead of the hand pointer.
    // Restore the value __sinit_menudraw sets, (U,V,W,H)=(0x3e,1,0x28,0x18); verified
    // against live PCSX2 (menu_long_hand=(62,1,40,24)). Idempotent: only writes when the
    // rect is still zero. Same un-run-__sinit class as f50_4/f50_7. The cursor texObj
    // bind (tbp=0x2c20 cbp=0x3fd4) was already byte-identical to HW ג€” only the UV was bad.
    {
        constexpr uint32_t kMenuLongHand = 0x01ECCE50u;
        if (dc2_read_u32(rdram, kMenuLongHand + 0x0u) == 0u &&
            dc2_read_u32(rdram, kMenuLongHand + 0x8u) == 0u)
        {
            dc2_write_u32(rdram, kMenuLongHand + 0x0u, 0x3Eu); // U
            dc2_write_u32(rdram, kMenuLongHand + 0x4u, 0x01u); // V
            dc2_write_u32(rdram, kMenuLongHand + 0x8u, 0x28u); // W
            dc2_write_u32(rdram, kMenuLongHand + 0xCu, 0x18u); // H
        }
    }
    // G13 FIX (Known Issue #4 ג€” costume NAMES): the CMenuCostumeSel ctor (0x2bc500) caches
    // the player char-data ptr at MenuCosPtr+0x2d0 = GetCharaDataPtr(GetUserDataMan(),0)
    // (= ActiveSaveData+0x1d2a0+0x3f48). Draw@0x2bd640 only renders the three costume NAMES
    // (Hunting Cap / Denim Overalls / Leather Shoes, via GetName@0x197700) when that ptr is
    // non-zero. In the headless route the ctor runs before ActiveSaveData is funded, so it
    // cached 0 (verified: [G13:state] charData=0x0 while ActiveSaveData=0x1e01810 is valid and
    // byte-identical to HW). The name STRINGS are static ELF message tables (0x3361f0/0x365858),
    // already present ג€” only the cached ptr is wrong. Idempotently repair it (same un-run-init
    // / stale-cache class as the menu_long_hand fix above and f50_4/f50_7). Gated on the costume
    // being the active menu (MenuCosutumeLoadPhase 1..4) so a stale MenuCosPtr can't be touched.
    {
        const uint32_t phase = dc2_read_u32(rdram, 0x00378114u) & 0xFFFFu; // MenuCosutumeLoadPhase
        if (phase >= 1u && phase <= 4u)
        {
            const uint32_t mcp = dc2_read_u32(rdram, 0x00378120u); // MenuCosPtr (gp-0x63d0)
            if (mcp > 0x80000u && mcp < 0x2000000u &&
                dc2_read_u32(rdram, mcp + 0x2D0u) == 0u)
            {
                const uint32_t asd = dc2_read_u32(rdram, 0x00376FE4u); // ActiveSaveData (gp-0x750c)
                if (asd > 0x80000u && asd < 0x2000000u)
                    dc2_write_u32(rdram, mcp + 0x2D0u, asd + 0x1D2A0u + 0x3F48u);
            }
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    MenuMainKey__Fv_0x233ff0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=MenuMainKey.exit n=%u ra=0x%x ret=0x%x dtMs=%lld\n",
                     n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs));
    }
    if (f48_2_trace_charbg_enabled() && g_f48_2_log_count < 128u &&
        (n <= 16u || preMenuStep != lastStep || tableTarget != lastTarget || (n % 30u) == 0u))
    {
        const uint32_t postMenuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6B08
        const uint32_t postMenuStep = postMenuPtr ? dc2_read_u32(rdram, postMenuPtr + 0x54u) : 0xFFFFFFFFu;
        const int32_t postMenuSel = postMenuPtr ? static_cast<int16_t>(dc2_read_u32(rdram, postMenuPtr + 0x50u) & 0xFFFFu) : -1;
        const int32_t readNo = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF84ECu) & 0xFFFFu); // gp-0x7B14
        const int32_t statusBit = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C18u) & 0xFFFFu); // gp-0x63E8
        const int32_t moveX = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF9C0Cu)); // gp-0x63F4
        const int32_t movePhase = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF9C10u)); // gp-0x63F0
        const int32_t readPhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C00u) & 0xFFFFu); // gp-0x6400
        const int32_t screenWidth = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF8780u)); // gp-0x7880
        const uint32_t nowReadMainChara = dc2_read_u32(rdram, gp + 0xFFFF9C04u); // gp-0x63FC
        std::fprintf(stderr,
                     "[F48.2:charbg] tag=MenuMainKey.dispatch n=%u frame=%u ra=0x%x menu=0x%x step=%u->%u sel=%d target=0x%x ret=%u readNo=%d status=0x%x moveX=%d movePhase=%d readPhase=%d screenWidth=%d chara=0x%x block='%s'\n",
                     n, g_f40_frame_counter, ra, postMenuPtr, preMenuStep, postMenuStep,
                     postMenuSel, tableTarget, getRegU32(ctx, 2), readNo,
                     static_cast<uint32_t>(statusBit), moveX, movePhase,
                     readPhase, screenWidth, nowReadMainChara,
                     (tableTarget == 0x002BC150u) ? "dispatches_KeyMainCharaBG" : "runtime_target_not_KeyMainCharaBG");
        ++g_f48_2_log_count;
    }
    lastTarget = tableTarget;
    lastStep = preMenuStep;

    // G9: log once when the MenuMain state machine has entered costume-select
    // (the title case-2 commit ran MenuMainInit with selector 0x14). +0x50=0x14,
    // +0x54=0x17 (costume KeyStep) is the live PCSX2 signature.
    if (g9_costume_enabled() && g_g9_phase < 2)
    {
        const uint32_t menuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // MenuCommonInfo
        if (menuPtr != 0u)
        {
            const uint32_t sel = dc2_read_u32(rdram, menuPtr + 0x50u) & 0xFFFFu;
            const uint32_t step = dc2_read_u32(rdram, menuPtr + 0x54u);
            if (sel == 0x14u || step == 0x17u)
            {
                if (g_g9_phase == 0)
                    std::fprintf(stderr, "[G9:costume] in costume-select sel=0x%x step=0x%x menuPtr=0x%x\n",
                                 sel, step, menuPtr);
                g_g9_phase = 2;
                std::fflush(stderr);
            }
        }
    }
    // G10: trace the costume load-phase state machine (MenuCosutumeLoadPhase @ gp-0x63dc
    // = 0x378114, halfword). The "Select Max's costume." prompt persists until the
    // costume char/texture BG read completes (phase 2 -> 3 gated on ReadBGSync()==0),
    // then a 15-frame settle reaches phase 4 (interactive list). Find where it sticks.
    if (g9_costume_enabled() && g_g9_phase == 2)
    {
        static uint32_t s_last = 0xFFFFFFFFu;
        static uint32_t s_n = 0u;
        const uint32_t ph = dc2_read_u32(rdram, 0x00378114u) & 0xFFFFu;
        if (ph != s_last || (++s_n % 120u) == 0u)
        {
            std::fprintf(stderr, "[G10:loadphase] MenuCosutumeLoadPhase=%u\n", ph);
            std::fflush(stderr);
            s_last = ph;
        }
        // G11: one-shot dump of the costume sheet filename (LoadFileMenu arg @0x36f5a8).
        {
            static bool s_once = false;
            if (!s_once)
            {
                s_once = true;
                const char *fn = reinterpret_cast<const char *>(getConstMemPtr(rdram, 0x0036F5A8u));
                char nm[64] = {0};
                if (fn) { std::strncpy(nm, fn, sizeof(nm) - 1u); }
                std::fprintf(stderr, "[G11:sheet] LoadFileMenu name@0x36f5a8='%s'\n", nm);
                std::fflush(stderr);
            }
        }
        // G10: phase-4 frame counter for diagnostics, and optional prompt confirm
        // pulse. Default stays on the HW-reference prompt; DC2_G9_AUTO_CONFIRM=1
        // restores the old "advance into the list" behavior.
        if (ph >= 4u)
        {
            static uint32_t s_cf = 0u;
            const uint32_t phase4Frame = s_cf++;
            if (g9_auto_confirm_enabled())
            {
                const uint32_t cyc = phase4Frame % 48u;
                g_g9_confirm_mask.store(cyc < 6u ? 0x4000u : 0u, std::memory_order_relaxed);
            }
            else
            {
                g_g9_confirm_mask.store(0u, std::memory_order_relaxed);
            }

            // G12: A/B the costume texObj binding vs live HW. HW @ populated list:
            // mgr(0x381ef0) base=0x2720, costume mgCTexture (*(MenuCosPtr+0x2d4)) idx=0x54
            // tbp=0x2720; slot[0x54] = {0x2720/sz0x200, 0x2920/0x300, 0x2c20/0x100}.
            // If the runner binds the bars to 0x2aa0, this pins whether the mgr base,
            // the slot accumulation, or a stale +0x38 diverges. Throttled, bounded.
            // G12 verification aid (DC2_G12_FORCE_CURSOR, default off): the scripted
            // headless route never sets the cursor-active gate (MenuCosPtr+0x14a), so the
            // cursor does not draw and the menu_long_hand UV fix can't be seen in a frame.
            // Force the gate so a capture shows the hand cursor render. Test-only.
            {
                const uint32_t mcpF = dc2_read_u32(rdram, 0x00378120u);
                if (mcpF > 0x80000u && mcpF < 0x2000000u && dc2_env_flag_enabled("DC2_G12_FORCE_CURSOR"))
                    dc2_write_u32(rdram, mcpF + 0x294u, 1u);
            }

            static uint32_t s_g12n = 0u;
            const uint32_t g12c = s_cf;
            if (g12c == 6u || g12c == 90u)
            {
                ++s_g12n;
                const uint32_t mgr = 0x00381EF0u;
                const uint32_t base = dc2_read_u32(rdram, mgr + 0x00u) & 0x3FFFu;
                const uint32_t cacheIdx = dc2_read_u32(rdram, mgr + 0x08u);
                const uint32_t count = dc2_read_u32(rdram, mgr + 0x0Cu);
                const uint32_t slotArr = dc2_read_u32(rdram, mgr + 0x10u);
                const uint32_t mcp = dc2_read_u32(rdram, 0x00378120u); // MenuCosPtr (gp-0x63d0)
                std::fprintf(stderr, "[G12:mgr] base=0x%x cacheIdx=0x%x count=%u slotArr=0x%x MenuCosPtr=0x%x\n",
                             base, cacheIdx, count, slotArr, mcp);
                // G13 (Known Issue #4): costume NAMES + 3D Max model. A/B vs live HW
                // (populated list, phase 4): charData(MenuCosPtr+0x2d0)=0x1e229f8,
                // convtbl_5277@0x3769EC=[2,4,3], item entries type=4 sub=0x6f/0x104/0x75,
                // MenuActionChara@0x1f0caa0=0xebf9c0 vtable=0x3756f0, nametbl@0x1e9b060 has data.
                {
                    const uint32_t ph = dc2_read_u32(rdram, 0x00378114u) & 0xFFFFu;
                    const uint32_t charData = (mcp > 0x80000u && mcp < 0x2000000u)
                                                  ? dc2_read_u32(rdram, mcp + 0x2D0u) : 0u;
                    const uint32_t mac = dc2_read_u32(rdram, 0x01F0CAA0u); // MenuActionChara[0]
                    const uint32_t macVt = (mac > 0x80000u && mac < 0x2000000u)
                                               ? dc2_read_u32(rdram, mac) : 0u;
                    const uint32_t ntg = dc2_read_u32(rdram, 0x01E9B060u);
                    const uint32_t modelTexIdx = (mcp > 0x80000u && mcp < 0x2000000u)
                                                     ? dc2_read_u32(rdram, mcp + 0x24u) : 0u;
                    const uint32_t asd = dc2_read_u32(rdram, 0x00376FE4u); // ActiveSaveData (gp-0x750c)
                    const uint32_t udm = asd ? (asd + 0x1D2A0u) : 0u;
                    std::fprintf(stderr, "[G13:state] phase=%u charData=0x%x MenuActionChara=0x%x vtable=0x%x nametbl@1e9b060=0x%x modelTexIdx=0x%x ActiveSaveData=0x%x udm=0x%x udm+0x3f48=0x%x\n",
                                 ph, charData, mac, macVt, ntg, modelTexIdx, asd, udm, udm ? udm + 0x3F48u : 0u);
                    // G13 model scope: dump CActionChara model-related fields. HW (populated
                    // list) MenuActionChara[0] holds a built model (mgCFrame/mesh + "body").
                    if (mac > 0x80000u && mac < 0x2000000u)
                    {
                        std::fprintf(stderr, "[G13:model] mac+0x4..0x40:");
                        for (uint32_t o = 0x04u; o <= 0x40u; o += 4u)
                            std::fprintf(stderr, " %x:0x%x", o, dc2_read_u32(rdram, mac + o));
                        std::fprintf(stderr, "\n[G13:model] mac+0x110..0x140:");
                        for (uint32_t o = 0x110u; o <= 0x140u; o += 4u)
                            std::fprintf(stderr, " %x:0x%x", o, dc2_read_u32(rdram, mac + o));
                        std::fprintf(stderr, "\n");
                    }
                    if (charData > 0x80000u && charData < 0x2000000u)
                    {
                        const uint32_t convWord = dc2_read_u32(rdram, 0x003769ECu);
                        for (int i = 0; i < 3; ++i)
                        {
                            const uint32_t cv = (convWord >> (i * 8)) & 0xFFu;
                            const uint32_t entry = charData + cv * 0x6Cu + 0x170u;
                            const int32_t type = (int16_t)(dc2_read_u32(rdram, entry) & 0xFFFFu);
                            const int32_t sub = (int16_t)(dc2_read_u32(rdram, entry + 2u) & 0xFFFFu);
                            std::fprintf(stderr, "[G13:item] slot%d convtbl=%u entry=0x%x type=%d sub=%d(0x%x)\n",
                                         i, cv, entry, type, sub, (uint32_t)(sub & 0xFFFF));
                        }
                    }
                }
                if (mcp > 0x80000u && mcp < 0x2000000u && slotArr != 0u)
                {
                    const uint32_t P = dc2_read_u32(rdram, mcp + 0x2D4u); // costume mgCTexture
                    if (P > 0x80000u && P < 0x2000000u)
                    {
                        const uint32_t idx = dc2_read_u32(rdram, P) & 0xFFFFu;
                        const uint32_t tex0lo = dc2_read_u32(rdram, P + 0x38u);
                        std::fprintf(stderr, "[G12:texObj] P=0x%x idx=0x%x +0x38.tbp0=0x%x size=0x%x next=0x%x\n",
                                     P, idx, tex0lo & 0x3FFFu, dc2_read_u32(rdram, P + 0x28u),
                                     dc2_read_u32(rdram, P + 0x68u));
                    }
                    // G12: cursor texObj (MenuCosPtr+0x2d8 = +0x16c shorts) + UV rect
                    // menu_long_hand@0x1ecce50. HW: cursor tbp=0x2c20 cbp=0x3fd4 tbw=4 T8,
                    // UV=(62,1,40,24), enable(+0x294)=1. The recomp draws it as a black square.
                    {
                        const uint32_t cur = dc2_read_u32(rdram, mcp + 0x2D8u);
                        const uint32_t en = dc2_read_u32(rdram, mcp + 0x294u);
                        const uint32_t u = dc2_read_u32(rdram, 0x01ECCE50u);
                        const uint32_t v = dc2_read_u32(rdram, 0x01ECCE54u);
                        const uint32_t w = dc2_read_u32(rdram, 0x01ECCE58u);
                        const uint32_t h = dc2_read_u32(rdram, 0x01ECCE5Cu);
                        if (cur > 0x80000u && cur < 0x2000000u)
                        {
                            const uint32_t t0 = dc2_read_u32(rdram, cur + 0x38u);
                            const uint32_t t0h = dc2_read_u32(rdram, cur + 0x3Cu);
                            std::fprintf(stderr, "[G12:cursor] obj=0x%x idx=0x%x tbp=0x%x tbw=%u psm=0x%x cbp=0x%x size=0x%x src=0x%x enable=%u UV=(%d,%d,%d,%d)\n",
                                         cur, dc2_read_u32(rdram, cur) & 0xFFFFu, t0 & 0x3FFFu, (t0 >> 14) & 0x3Fu,
                                         (t0 >> 20) & 0x3Fu, (t0h >> 5) & 0x3FFFu, dc2_read_u32(rdram, cur + 0x28u),
                                         dc2_read_u32(rdram, cur + 0x50u), en, (int)u, (int)v, (int)w, (int)h);
                        }
                        else
                            std::fprintf(stderr, "[G12:cursor] obj=0x%x (null/oob) enable=%u UV=(%d,%d,%d,%d)\n",
                                         cur, en, (int)u, (int)v, (int)w, (int)h);
                    }
                    // G12: scan the slot array for the tbw=8 common-menu-frame texObjs (the
                    // costume row-bar atlas). HW slot[64] chains 0x2720->0x2aa0->... tbw=8 with
                    // valid src(+0x50) ptrs. Report tbp/tbw/psm + src so we can tell whether the
                    // runner's common frame is loaded (src!=0) and where it binds vs HW's 0x2aa0.
                    for (uint32_t s = 0; s < count && s < 130u; ++s)
                    {
                        uint32_t cur = dc2_read_u32(rdram, slotArr + s * 0x10u + 8u);
                        for (int d = 0; d < 4 && cur > 0x80000u && cur < 0x2000000u; ++d)
                        {
                            const uint32_t t0 = dc2_read_u32(rdram, cur + 0x38u);
                            const uint32_t tbp = t0 & 0x3FFFu;
                            const uint32_t tbw = (t0 >> 14) & 0x3Fu;
                            const uint32_t psm = (t0 >> 20) & 0x3Fu;
                            if (tbp == 0x2aa0u || (tbw == 8u && (tbp == 0x2720u || tbp == 0x2b20u || tbp == 0x2f20u)))
                                std::fprintf(stderr, "[G12:slot] s=%u d=%d obj=0x%x tbp=0x%x tbw=%u psm=0x%x size=0x%x src=0x%x\n",
                                             s, d, cur, tbp, tbw, psm, dc2_read_u32(rdram, cur + 0x28u),
                                             dc2_read_u32(rdram, cur + 0x50u));
                            cur = dc2_read_u32(rdram, cur + 0x68u);
                        }
                    }
                }
                std::fflush(stderr);
            }
        }
    }
}

// G11: trace the costume sheet load + texture-manager IMG entry. The bars sample
// the texture from fukusel.img (LoadFileMenu @0x36f5a8 -> EnterIMGFile -> GetTexture).
// If the load fails or the IMG enters at a block != the drawn 0x2720, the bars fall
// back to stale title-atlas data at 0x2aa0.
static void g11_loadfilemenu_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t a0 = getRegU32(ctx, 4);
    char nm[64] = {0};
    const char *fn = reinterpret_cast<const char *>(getConstMemPtr(rdram, a0));
    if (fn) std::strncpy(nm, fn, sizeof(nm) - 1u);
    LoadFileMenu__FPcP1i_0x251100(rdram, ctx, runtime);
    if (g9_costume_enabled())
    {
        std::fprintf(stderr, "[G11:LoadFileMenu] name='%s' ret=%d (size)\n",
                     nm, static_cast<int32_t>(getRegU32(ctx, 2)));
        std::fflush(stderr);
    }
}

static void g11_enterimg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t buf = getRegU32(ctx, 5);  // a1 = IMG buffer
    const uint32_t blk = getRegU32(ctx, 6);  // a2 = *param_3 (tex block)
    EnterIMGFile__17mgCTextureManagerFPUciP9mgCMemoryP15mgCEnterIMGInfo_0x12da90(rdram, ctx, runtime);
    if (g9_costume_enabled())
    {
        std::fprintf(stderr, "[G11:EnterIMG] buf=0x%x blockArg=0x%x ret=0x%x\n",
                     buf, blk, getRegU32(ctx, 2));
        std::fflush(stderr);
    }
}

// G11: log each ReloadTexture(texIndex) and the tbp it will bind (texture obj +0x38,
// low 14 bits), so we see which costume draw binds 0x2aa0 vs 0x2720. mgr=0x381ef0,
// table base=*(mgr+0x10); entry=texIndex*0x10+base; obj=*(entry+8); tbp=*(u16*)(obj+0x38)&0x3fff.
static void g11_reloadtex_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int32_t texIdx = static_cast<int32_t>(getRegU32(ctx, 5)); // a1
    uint32_t tbp = 0xFFFFu;
    if (g9_costume_enabled() && g_g9_phase == 2 && texIdx >= 0)
    {
        const uint32_t mgr = 0x00381EF0u;
        const uint32_t tblBase = dc2_read_u32(rdram, mgr + 0x10u);
        if (tblBase != 0u)
        {
            const uint32_t obj = dc2_read_u32(rdram, tblBase + static_cast<uint32_t>(texIdx) * 0x10u + 8u);
            if (obj != 0u)
                tbp = dc2_read_u32(rdram, obj + 0x38u) & 0x3FFFu;
        }
        static uint32_t c = 0u;
        if (c++ < 40u)
        {
            std::fprintf(stderr, "[G11:reload] texIdx=%d -> tbp=0x%x ra=0x%x\n",
                         texIdx, tbp, getRegU32(ctx, 31));
            std::fflush(stderr);
        }
    }
    ReloadTexture__17mgCTextureManagerFiP13sceVif1Packet_0x12e850(rdram, ctx, runtime);
}

static const char *f48_2_costume_block_reason(int32_t state,
                                               int32_t ret,
                                               int32_t loadPhase,
                                               int32_t loadTimer,
                                               uint32_t menuFlag1)
{
    if (ret == 1)
        return "none/MenuCostumeKey_returned_1";
    if (state == 2)
        return "0x2bd434 fade_or_close_not_complete_ret0";
    if (state == 1 && (loadTimer <= 15 || loadPhase <= 3))
        return "state1_wait_MenuCosutumeLoadPhase_gt3_and_timer_gt15";
    if (state == 0)
        return "state0_costume_select_loop_ret0";
    return menuFlag1 ? "menu_fade_flag_set_ret0" : "state_not_ready_ret0";
}

static const char *f48_3_costume_branch_reason(int32_t preState,
                                               int32_t postState,
                                               int32_t preSub,
                                               int32_t postSub,
                                               int32_t preCursor,
                                               int32_t postCursor,
                                               uint32_t postSelect,
                                               uint32_t postPush,
                                               int32_t ret)
{
    if (ret == 1)
        return "0x2bcdd4_return_1_after_state2_fade";
    if ((postPush & 2u) != 0u && postState == 2)
        return "0x2bd170_cancel_back_uVar8_bit2_sets_state2_fade";
    if ((postPush & 1u) != 0u && postState == 2 && postCursor == 3)
        return "0x2bd3d8_yes_confirm_cursor3_sets_state2_fade";
    if ((postPush & 1u) != 0u && postState == 2 && postCursor == 4)
        return "0x2bd2b8_yes_confirm_cursor4_sets_state2_fade";
    if ((postPush & 1u) != 0u && postCursor == 3 && postSub != preSub)
        return "0x2bd1c0_confirm_bit1_cursor3_opens_yesno";
    if ((postPush & 1u) != 0u && postCursor == 4 && postSub != preSub)
        return "0x2bd244_confirm_bit1_cursor4_opens_yesno";
    if ((postPush & 1u) != 0u)
        return "0x2bd1b8_confirm_bit1_ignored_cursor_not_3_or_4";
    if (postCursor != preCursor && (postSelect & 2u) != 0u)
        return "0x2bcfa0_select_bit2_cursor_increment";
    if (postCursor != preCursor && (postSelect & 1u) != 0u)
        return "0x2bcfa0_select_bit1_cursor_decrement";
    if (preState == 1 && postState == 0)
        return "0x2bccf0_loader_gate_complete_state0";
    if (postState == 2)
        return "state2_wait_fade_return1";
    if (postState == 0)
        return "state0_wait_select_or_push";
    return "state_not_ready";
}

static void f48_2_menu_costume_key_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static int32_t lastState = -9999;
    static int32_t lastLoadPhase = -9999;
    static int32_t lastRet = -9999;
    static int32_t lastF483State = -9999;
    static int32_t lastF483Sub = -9999;
    static int32_t lastF483Cursor = -9999;
    static uint32_t lastF483Select = 0xFFFFFFFFu;
    static uint32_t lastF483Push = 0xFFFFFFFFu;
    static uint32_t lastF483PadEdge = 0xFFFFFFFFu;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t menuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6B08
    const uint32_t costume = dc2_read_u32(rdram, gp + 0xFFFF9C30u); // gp-0x63D0
    const int32_t preState = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x00u) & 0xFFFFu) : -9999;
    const int32_t preSub = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x02u) & 0xFFFFu) : -9999;
    const int32_t preCursor = costume ? static_cast<int32_t>(dc2_read_u32(rdram, costume + 0x1D0u)) : -9999;
    const int32_t preChar = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x258u) & 0xFFFFu) : -9999;
    const int32_t preLoadTimer = costume ? static_cast<int32_t>(dc2_read_u32(rdram, costume + 0x2A8u)) : -9999;
    const int32_t preLoadPhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C24u) & 0xFFFFu); // gp-0x63DC
    const uint32_t preD62c = dc2_read_u32(rdram, 0x01ECD62Cu);
    const uint32_t preMenuFlag1 = menuPtr ? (dc2_read_u32(rdram, menuPtr + 0x00u) >> 8) & 0xFFu : 0u;
    const uint32_t preSelect = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x04u) : 0u;
    const uint32_t prePush = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x08u) : 0u;
    const uint32_t padCurr = dc2_read_u32(rdram, 0x003D76E4u); // CGamePad+0x04
    const uint32_t padPrev = dc2_read_u32(rdram, 0x003D777Cu); // CGamePad+0x9C
    const uint32_t padEdge = padCurr & ~padPrev;
    const int32_t languageCode = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF8AD0u)); // gp-0x7530
    const uint32_t padTableBase = (languageCode > 0) ? 0x00350C68u : 0x00350C60u;
    const uint32_t padTable0 = dc2_read_u32(rdram, padTableBase + 0u);
    const uint32_t padTable1 = dc2_read_u32(rdram, padTableBase + 4u);

    MenuCostumeKey__Fv_0x2be030(rdram, ctx, runtime);

    const int32_t ret = static_cast<int32_t>(getRegU32(ctx, 2));
    const int32_t postState = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x00u) & 0xFFFFu) : -9999;
    const int32_t postSub = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x02u) & 0xFFFFu) : -9999;
    const int32_t postCursor = costume ? static_cast<int32_t>(dc2_read_u32(rdram, costume + 0x1D0u)) : -9999;
    const int32_t postChar = costume ? static_cast<int16_t>(dc2_read_u32(rdram, costume + 0x258u) & 0xFFFFu) : -9999;
    const int32_t postLoadTimer = costume ? static_cast<int32_t>(dc2_read_u32(rdram, costume + 0x2A8u)) : -9999;
    const int32_t postLoadPhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C24u) & 0xFFFFu); // gp-0x63DC
    const uint32_t postD62c = dc2_read_u32(rdram, 0x01ECD62Cu);
    const uint32_t postMenuFlag1 = menuPtr ? (dc2_read_u32(rdram, menuPtr + 0x00u) >> 8) & 0xFFu : 0u;
    const uint32_t postSelect = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x04u) : 0u;
    const uint32_t postPush = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x08u) : 0u;

    const bool changed = postState != lastState || postLoadPhase != lastLoadPhase || ret != lastRet ||
                          preState != postState || preLoadPhase != postLoadPhase || preD62c != postD62c;
    if (f48_2_trace_charbg_enabled() && g_f48_2_log_count < 128u &&
        (n <= 24u || changed || ret != 0 || (n % 30u) == 0u))
    {
        std::fprintf(stderr,
                     "[F48.2:charbg] tag=MenuCostumeKey.exit n=%u frame=%u ra=0x%x menu=0x%x costume=0x%x state=%d->%d sub=%d->%d cursor=%d->%d char=%d->%d loadPhase=%d->%d loadTimer=%d->%d d62c=0x%x->0x%x menuFlag1=%u->%u ret=%d block='%s'\n",
                     n, g_f40_frame_counter, ra, menuPtr, costume,
                     preState, postState, preSub, postSub, preCursor, postCursor,
                     preChar, postChar, preLoadPhase, postLoadPhase, preLoadTimer, postLoadTimer,
                     preD62c, postD62c, preMenuFlag1, postMenuFlag1, ret,
                     f48_2_costume_block_reason(postState, ret, postLoadPhase, postLoadTimer, postMenuFlag1));
        ++g_f48_2_log_count;
    }

    const bool f483Changed =
        postState != lastF483State ||
        postSub != lastF483Sub ||
        postCursor != lastF483Cursor ||
        postSelect != lastF483Select ||
        postPush != lastF483Push ||
        padEdge != lastF483PadEdge ||
        preState != postState ||
        preSub != postSub ||
        preCursor != postCursor ||
        ret != 0;
    if (f48_3_trace_costume_enabled() && g_f48_3_log_count < 128u &&
        (n <= 24u || f483Changed || postPush != 0u || postSelect != 0u || (n % 30u) == 0u))
    {
        std::fprintf(stderr,
                     "[F48.3:costume] tag=MenuCostumeKey.exit n=%u frame=%u ra=0x%x script=0x%04x active=%u padCurr=0x%x padPrev=0x%x padEdge=0x%x lang=%d padtbl=%u/%u menu=0x%x costume=0x%x state=%d->%d sub=%d->%d cursor=%d->%d char=%d->%d select=0x%x->0x%x push=0x%x->0x%x loadPhase=%d->%d loadTimer=%d->%d ret=%d branch='%s'\n",
                     n, g_f40_frame_counter, ra,
                     static_cast<unsigned>(g_f40_pressed), g_f40_active ? 1u : 0u,
                     padCurr, padPrev, padEdge, languageCode, padTable0, padTable1,
                     menuPtr, costume, preState, postState, preSub, postSub,
                     preCursor, postCursor, preChar, postChar,
                     preSelect, postSelect, prePush, postPush,
                     preLoadPhase, postLoadPhase, preLoadTimer, postLoadTimer, ret,
                     f48_3_costume_branch_reason(preState, postState, preSub, postSub,
                                                 preCursor, postCursor, postSelect, postPush, ret));
        ++g_f48_3_log_count;
    }

    lastState = postState;
    lastLoadPhase = postLoadPhase;
    lastRet = ret;
    lastF483State = postState;
    lastF483Sub = postSub;
    lastF483Cursor = postCursor;
    lastF483Select = postSelect;
    lastF483Push = postPush;
    lastF483PadEdge = padEdge;
}

// PHASE F50.1: identify the dungeon scene-init sub-object whose auto-stub ctor
// leaves a garbage vtable (0x49497350) after Alloc@0x139d20 + placement
// new@0x1398e0. Activated only once InitDungeonMain @0x1CC040 is entered (set in
// f48_4_init_dungeon_main_probe below), so we capture only dungeon-path
// allocations. The last [F50.1:new] line before the dispatcher's bad=0x49497350
// warning is the caller doing the bad `new`; its $ra maps to the recomp function
// whose next `jal <ctor>` is the broken constructor.
// Behaviorally identical to reta1_stub (returns $a1) and quiet unless
// DC2_TRACE_F50_1 is set.
static bool f50_1_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F50_1");
    return enabled;
}

static bool g_f50_1_dungeon_active = false;
static uint32_t g_f50_1_new_log = 0u;

static void f50_1_placement_new_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ptr = getRegU32(ctx, 5); // $a1 (the pointer placement-new returns)
    if (f50_1_trace_enabled() && g_f50_1_dungeon_active && g_f50_1_new_log < 128u)
    {
        const uint32_t n = ++g_f50_1_new_log;
        std::fprintf(stderr,
                     "[F50.1:new] n=%u frame=%u ra=0x%x size=%u ptr=0x%x preVt=0x%x\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31),
                     getRegU32(ctx, 4), ptr,
                     ptr ? dc2_read_u32(rdram, ptr) : 0u);
    }
    setReturnU32(ctx, ptr);       // $v0 = $a1
    ctx->pc = getRegU32(ctx, 31); // return to caller
}

// PHASE F50.1: repair the auto-stubbed memoryInit__Fv @0x1CBD70.
// The generated recomp/memoryInit__Fv_0x1cbd70.cpp is a setReturnS32(ctx,0) auto-stub
// (it is commented out of [general].stubs in ref/config_auto_recomp.toml but the live
// recomp output is stale). InitDungeonMain @0x1CC094 calls it first thing; because it
// does nothing, the global mgCMemory pools are never created, so InitDungeonMain's
// Alloc() returns 0, every dungeon allocation gets ptr=0, and constructing on null
// yields the garbage vtable 0x49497350. This override faithfully replays the real
// memoryInit body (ref/assembly.txt @0x1CBD70) by calling the real recompiled helpers
// (all present). memoryInit runs only on dungeon entry, so the default-smoke (title)
// path is unaffected.
//
// Set an arbitrary GPR low word with R5900 sign-extension (mirrors setReturnU32).
static inline void f50_set_reg(R5900Context *ctx, int reg, uint32_t value)
{
    ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
}

// Complete a synchronous guest call even when regenerated loop code returns at
// a cooperative preemption point. The caller uses RA=0 as its return sentinel.
static uint32_t f50_run_guest_call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                   uint32_t addr)
{
    ctx->pc = addr;
    constexpr uint32_t kMaxResumeSlices = 1u << 20;
    for (uint32_t slice = 0; slice < kMaxResumeSlices; ++slice)
    {
        auto fn = runtime->lookupFunction(ctx->pc);
        if (!fn)
        {
            break;
        }

        fn(rdram, ctx, runtime);
        if (ctx->pc == 0u)
        {
            return getRegU32(ctx, 2);
        }
    }

    std::fprintf(stderr, "[F50.1:call] synchronous guest call did not complete addr=0x%x pc=0x%x\n",
                 addr, ctx->pc);
    return getRegU32(ctx, 2);
}

// Call a recompiled guest function by address with up to 4 args; return $v0.
static uint32_t f50_callg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                          uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    f50_set_reg(ctx, 4, a0);
    f50_set_reg(ctx, 5, a1);
    f50_set_reg(ctx, 6, a2);
    f50_set_reg(ctx, 7, a3);
    f50_set_reg(ctx, 31, 0u); // $ra sentinel
    return f50_run_guest_call(rdram, ctx, runtime, addr);
}

static uint32_t f50_callg_u64a0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                uint32_t addr, uint64_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(a0));
    f50_set_reg(ctx, 5, a1);
    f50_set_reg(ctx, 6, a2);
    f50_set_reg(ctx, 7, a3);
    f50_set_reg(ctx, 31, 0u);
    return f50_run_guest_call(rdram, ctx, runtime, addr);
}

static bool f55_trace_enabled();

// F55: init__Fv@0x15C160 stays nop'd for its IOP-module-load loops, but its
// InitCDFile@0x148F70 call is load-bearing: it fills the guest disc file-header
// table (header_buff@0x382680, header_num@gp-0x7764, data_sector@gp-0x775C) from
// cdrom0:\DATA.HD3;1. Without it every SearchFile-based load (LoadFileBG, the
// dungeon-map menu pack menu/N/dmapN.pac, ...) silently fails, which is the F53
// dungeon free-map black-rectangle root cause. Run the real generated body here.
static void f55_boot_init_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const auto savedRa = ctx->r[31];
    static bool s_initCdDone = false;
    if (!s_initCdDone)
    {
        s_initCdDone = true;
        const uint32_t ret = f50_callg(rdram, ctx, runtime, 0x148F70u, 0u, 0u, 0u, 0u);
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t headerNum = dc2_read_u32(rdram, gp - 0x7764u);
        const uint32_t dataSector = dc2_read_u32(rdram, gp - 0x775Cu);
        if (f55_trace_enabled() || headerNum == 0u)
        {
            std::fprintf(stderr,
                         "[F55:initcd] InitCDFile ret=0x%x header_num=%u data_sector=%u buff[0..3]=0x%x 0x%x 0x%x 0x%x\n",
                         ret, headerNum, dataSector,
                         dc2_read_u32(rdram, 0x382680u), dc2_read_u32(rdram, 0x382684u),
                         dc2_read_u32(rdram, 0x382688u), dc2_read_u32(rdram, 0x38268Cu));
        }
    }
    ctx->r[31] = savedRa;
    setReturnU32(ctx, 0u);
    ctx->pc = ra;
}

static void f50_1_memory_init(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t retAddr = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t pMainBuffer = gp - 0x7290u;  // global mgCMemory* MainBuffer
    const uint32_t pBuffReadData = gp - 0x728Cu; // global void* BuffReadData

    // MainBuffer = GetMainStack(); reset its stack offset (+0x24) and enable flag (+0x1c).
    const uint32_t mainBuf = f50_callg(rdram, ctx, runtime, 0x1908F0u, 0, 0, 0, 0);
    if (f50_1_trace_enabled())
        std::fprintf(stderr,
                     "[F50.1:meminit] mainBuf=0x%x pre f1c=0x%x base20=0x%x off24=0x%x lim28=0x%x\n",
                     mainBuf, dc2_read_u32(rdram, mainBuf + 0x1Cu), dc2_read_u32(rdram, mainBuf + 0x20u),
                     dc2_read_u32(rdram, mainBuf + 0x24u), dc2_read_u32(rdram, mainBuf + 0x28u));
    dc2_write_u32(rdram, pMainBuffer, mainBuf);
    dc2_write_u32(rdram, mainBuf + 0x24u, 0u);
    dc2_write_u32(rdram, mainBuf + 0x1Cu, 0u);

    // VIF1 packet double-buffer: two 10000-byte stack allocs -> mgInitVif1Packet(a,b,160000).
    const uint32_t vif0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 10000u, 0, 0);
    const uint32_t vif1 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 10000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x141E10u, vif0, vif1, 160000u, 0);
    if (f50_1_trace_enabled())
        std::fprintf(stderr, "[F50.1:meminit] vif0=0x%x vif1=0x%x\n", vif0, vif1);

    // Packet buffers 0x1e9f230 / 0x1e9f260 (20000 each) + mgSetPacketBuffer.
    uint32_t b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 20000u, 0, 0);
    uint32_t b1 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 20000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F230u, b0, 20000u, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F260u, b1, 20000u, 0);
    f50_callg(rdram, ctx, runtime, 0x141EF0u, 0x1E9F230u, 0x1E9F260u, 0, 0);

    // Data buffers 0x1e9f290 / 0x1e9f2c0 (60000 each) + mgSetDataBuffer(...,1).
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 60000u, 0, 0);
    b1 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 60000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F290u, b0, 60000u, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F2C0u, b1, 60000u, 0);
    f50_callg(rdram, ctx, runtime, 0x142040u, 0x1E9F290u, 0x1E9F2C0u, 1u, 0);

    // WorkData pool 0x1e9f3b0 (10000) + name (_1014 @0x366c78).
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 10000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F3B0u, b0, 10000u, 0);
    if (f50_callg(rdram, ctx, runtime, 0x129088u, 0x366C78u, 0, 0, 0) < 0x10u)
        f50_callg(rdram, ctx, runtime, 0x128F70u, 0x1E9F3B0u, 0x366C78u, 0, 0);
    dc2_write_u32(rdram, 0x1E9F3D4u, 0u);
    dc2_write_u32(rdram, 0x1E9F3CCu, 0u);

    // Texture manager: SetTableBuffer(table,0x140,0xaf,MainBuffer); Initialize(mgr,vramTop,-1).
    f50_callg(rdram, ctx, runtime, 0x12C830u, 0x381EF0u, 0x140u, 0xAFu, mainBuf);
    const uint32_t vramTop = f50_callg(rdram, ctx, runtime, 0x1908D0u, 0, 0, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x12CAD0u, 0x381EF0u, vramTop, 0xFFFFFFFFu, 0);

    // Character pool 0x1e9f3e0 (GetCharaMemAllocSize bytes) + name (_1015 @0x366c88).
    const uint32_t charaSize = f50_callg(rdram, ctx, runtime, 0x1E9B60u, 0, 0, 0, 0);
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, charaSize, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F3E0u, b0, charaSize, 0);
    if (f50_callg(rdram, ctx, runtime, 0x129088u, 0x366C88u, 0, 0, 0) < 0x10u)
        f50_callg(rdram, ctx, runtime, 0x128F70u, 0x1E9F3E0u, 0x366C88u, 0, 0);
    dc2_write_u32(rdram, 0x1E9F404u, 0u);
    dc2_write_u32(rdram, 0x1E9F3FCu, 0u);

    // ScriptData pool 0x1e9f560 (20000) + name (_1016 @0x366c98).
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 20000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139E70u, 0x1E9F560u, b0, 20000u, 0);
    if (f50_callg(rdram, ctx, runtime, 0x129088u, 0x366C98u, 0, 0, 0) < 0x10u)
        f50_callg(rdram, ctx, runtime, 0x128F70u, 0x1E9F560u, 0x366C98u, 0, 0);
    dc2_write_u32(rdram, 0x1E9F584u, 0u);
    dc2_write_u32(rdram, 0x1E9F57Cu, 0u);

    // EffectScriptData heap 0x1e9f590 (40000 via SetHeapMem) + name (_1017 @0x366cb0).
    b0 = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 40000u, 0, 0);
    f50_callg(rdram, ctx, runtime, 0x139930u, 0x1E9F590u, b0, 40000u, 0);
    if (f50_callg(rdram, ctx, runtime, 0x129088u, 0x366CB0u, 0, 0, 0) < 0x10u)
        f50_callg(rdram, ctx, runtime, 0x128F70u, 0x1E9F590u, 0x366CB0u, 0, 0);

    // BuffReadData = stAlloc64(MainBuffer, 200000).
    const uint32_t readData = f50_callg(rdram, ctx, runtime, 0x139C10u, mainBuf, 200000u, 0, 0);
    dc2_write_u32(rdram, pBuffReadData, readData);
    if (f50_1_trace_enabled())
        std::fprintf(stderr, "[F50.1:meminit] done charaSize=%u readData=0x%x off24=0x%x\n",
                     charaSize, readData, dc2_read_u32(rdram, mainBuf + 0x24u));

    setReturnS32(ctx, 0);
    ctx->pc = retAddr;
}

// PHASE F48.4: confirm the post-costume new-game loop transition.
// After MenuCostumeKey returns 1 with DAT_01ecd62c==0xc, TitleLoop calls
// NextLoop(2); MainLoop then switches LoopNo 3->2 and calls LoopInit[2] =
// InitDungeonMain @0x1CC040, followed each frame by LoopMain[2] =
// LoopDungeonMain @0x1CEA00. These wrappers log entry/exit so we can see
// whether the new-game dungeon init is reached and whether it returns.
// Bounded and quiet unless DC2_TRACE_F48_4 is set.
static bool f48_4_trace_postcostume_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F48_4");
    return enabled;
}

static uint32_t g_f48_4_init_log = 0u;
static uint32_t g_f48_4_main_log = 0u;

// F50.4 FIX ג€” restore MainScene's embedded CScene vtable pointer.
//
// Root cause: the static initializer __sinit_mainloop.cpp @0x373580 (real recomp
// body ps2___sinit_mainloop_cpp_0x373580) does not take effect in the headless
// run, so MainScene+0x10548 ג€” the CScene vtable pointer it writes with
// __vt__6CScene (0x375FE0) ג€” stays 0. InitDungeonMain's scene-entry dispatch
// (decompiled L158085 / asm 0x1CC1C4-0x1CC1D8:
//     lw t9,0x548(scene+0x10000) ; t9 = *(scene+0x10548)  -> 0 (null vtable)
//     lw t9,0x8(t9)              ; vtable slot +8 = Initialize__6CSceneFv@0x282EA0
//     jalr t9                    ; this = scene)
// is intended to call Initialize__6CSceneFv(MainScene), which sets the scene
// character count (scene+0x40) to 0x80 and initialises all 128 CSceneCharacter
// slots. With the vtable pointer null the jalr targets *(0+8); the dispatcher
// silently recovers to ra (no-op), so Initialize never runs, count stays 0, the
// 8 party AssignChara calls in InitDungeonMain all fail (GetSceneCharacter sees
// count 0), GetCharacter(DngMainScene,0) returns 0 -> MainChara=0, and the first
// LoopDungeonMain frame null-derefs it in DngStep@0x1D06C0 (bad=0x1 ra=0x1d0838).
//
// Fix: write the vtable pointer __sinit_mainloop should have set, before the
// scene-entry dispatch runs, so the game's own Initialize dispatch fires. The
// CScene vtable (0x375FE0) is static rodata and always loaded; verified at
// runtime that 0x375FE0+8 == 0x282EA0 (Initialize__6CSceneFv). Idempotent: only
// writes when the slot is still null. Both addresses are fixed ELF symbols,
// consistent with the other absolute-address game overrides in this file.
static void f50_4_repair_main_scene_vtable(uint8_t *rdram)
{
    constexpr uint32_t kMainScene = 0x01DD8260u; // &MainScene (GetMainScene__Fv @0x190870)
    constexpr uint32_t kSceneVt = 0x00375FE0u;   // __vt__6CScene
    if (dc2_read_u32(rdram, kMainScene + 0x10548u) == 0u)
    {
        dc2_write_u32(rdram, kMainScene + 0x10548u, kSceneVt);
    }
}

// F50.7: repair dungeon globals whose vtables are normally installed by
// __sinit_dng_main.cpp @0x373CA0. That initializer does not reliably take
// effect on the headless route, leaving CRandomCircle's embedded CCharacter2
// at 0x01EA4B60 null. InitDungeonMain later calls Initialize__13CRandomCircle,
// whose virtual CCharacter2 initialize dispatch silently no-ops when this
// vtable is zero; the first DngStep then tail-jumps through slot +0xD4 and
// hits pc-zero. Install only the missing final vtable pointers and the adjacent
// fields the static initializer clears, then let the game's own Initialize
// functions run.
static uint32_t f50_7_repair_dng_main_static_objects(uint8_t *rdram)
{
    constexpr uint32_t kRandomCircleChara = 0x01EA4B60u;
    constexpr uint32_t kGeoStone = 0x01EA51C0u;
    constexpr uint32_t kCharacter2Vt = 0x00375810u; // __vt__11CCharacter2
    constexpr uint32_t kGeoStoneVt = 0x00376040u;   // __vt__9CGeoStone
    uint32_t repaired = 0u;

    const uint32_t rcVt = dc2_read_u32(rdram, kRandomCircleChara);
    const uint32_t rcInit = rcVt ? dc2_read_u32(rdram, rcVt + 0x3Cu) : 0u;
    const uint32_t rcStep = rcVt ? dc2_read_u32(rdram, rcVt + 0xD4u) : 0u;
    if (rcVt == 0u || rcInit == 0u || rcStep == 0u)
    {
        dc2_write_u32(rdram, kRandomCircleChara, kCharacter2Vt);
        dc2_write_u32(rdram, kRandomCircleChara + 0x35Cu, 0u);
        dc2_write_u32(rdram, kRandomCircleChara + 0x360u, 0u);
        dc2_write_u32(rdram, kRandomCircleChara + 0x364u, 0u);
        repaired |= 1u;
    }

    const uint32_t geoVt = dc2_read_u32(rdram, kGeoStone);
    const uint32_t geoInit = geoVt ? dc2_read_u32(rdram, geoVt + 0x3Cu) : 0u;
    if (geoVt == 0u || geoInit == 0u)
    {
        dc2_write_u32(rdram, kGeoStone, kGeoStoneVt);
        dc2_write_u32(rdram, kGeoStone + 0x55Cu, 0u);
        dc2_write_u32(rdram, kGeoStone + 0x560u, 0u);
        dc2_write_u32(rdram, kGeoStone + 0x564u, 0u);
        repaired |= 2u;
    }

    if (f50_7_trace_selected_map_enabled() && repaired != 0u)
    {
        std::fprintf(stderr,
                     "[F50.7:dng-sinit] repaired=0x%x rcVt=0x%x->0x%x rcStep=0x%x geoVt=0x%x->0x%x\n",
                     repaired, rcVt, dc2_read_u32(rdram, kRandomCircleChara),
                     dc2_read_u32(rdram, dc2_read_u32(rdram, kRandomCircleChara) + 0xD4u),
                     geoVt, dc2_read_u32(rdram, kGeoStone));
    }

    return repaired;
}

static uint32_t f50_7_equip_id(uint8_t *rdram, uint32_t charaData, uint32_t offset)
{
    return charaData ? (dc2_read_u32(rdram, charaData + offset) & 0xFFFFu) : 0u;
}

static bool f50_7_has_missing_dungeon_equipment(uint8_t *rdram, uint32_t charaData)
{
    if (charaData == 0u)
        return true;
    return f50_7_equip_id(rdram, charaData, 0x322u) == 0u ||
           f50_7_equip_id(rdram, charaData, 0x24Au) == 0u ||
           f50_7_equip_id(rdram, charaData, 0x2B6u) == 0u;
}

static void f50_7_apply_direct_equips(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                      uint32_t udm, uint32_t slot, const uint32_t *items, uint32_t count)
{
    for (uint32_t i = 0u; i < count; ++i)
        f50_callg(rdram, ctx, runtime, 0x0019D560u, udm, slot, items[i], 0u);
}

static uint32_t f50_7_first_costume_id(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                       uint64_t costumeAttr, uint32_t costumeType, uint32_t scratch)
{
    if (uint8_t *buf = getMemPtr(rdram, scratch))
        std::memset(buf, 0, 0x20u);

    const uint32_t count =
        f50_callg_u64a0(rdram, ctx, runtime, 0x002F2B20u, costumeAttr, costumeType, scratch, 0u);
    const uint32_t first = dc2_read_u32(rdram, scratch) & 0xFFFFu;
    if (f50_7_trace_selected_map_enabled())
    {
        std::fprintf(stderr,
                     "[F50.7:costume-list] type=%u count=%u first=%u attr=0x%08x%08x scratch=0x%x\n",
                     costumeType, count, first,
                     static_cast<uint32_t>(costumeAttr >> 32u),
                     static_cast<uint32_t>(costumeAttr), scratch);
    }
    return (count != 0u && first != 0xFFFFu) ? first : 0u;
}

static bool f50_7_seed_used_data_id(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                    uint32_t udm, uint32_t charaData, uint32_t idOffset,
                                    uint32_t itemId)
{
    if (udm == 0u || charaData == 0u || itemId == 0u ||
        f50_7_equip_id(rdram, charaData, idOffset) != 0u)
        return false;

    const uint32_t usedData = charaData + idOffset - 2u;
    f50_callg(rdram, ctx, runtime, 0x0019E9E0u, udm, usedData, itemId, 0u);
    if (f50_7_equip_id(rdram, charaData, idOffset) == 0u)
    {
        dc2_write_u16(rdram, usedData + 0x00u, 1u);
        dc2_write_u16(rdram, usedData + 0x02u, static_cast<uint16_t>(itemId));
        dc2_write_u16(rdram, usedData + 0x10u, 1u);
    }
    return f50_7_equip_id(rdram, charaData, idOffset) != 0u;
}

static bool f50_7_seed_costume_slot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                    uint32_t udm, uint32_t charaData, uint32_t idOffset,
                                    uint64_t costumeAttr, uint32_t costumeType, uint32_t scratch)
{
    const uint32_t itemId =
        f50_7_first_costume_id(rdram, ctx, runtime, costumeAttr, costumeType, scratch);
    return f50_7_seed_used_data_id(rdram, ctx, runtime, udm, charaData, idOffset, itemId);
}

static void f50_7_repair_debug_dungeon_equipment(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static bool done = false;
    if (done || !f50_6_debug_menu_enabled())
        return;
    done = true;

    alignas(16) __m128i savedRegs[32];
    std::memcpy(savedRegs, ctx->r, sizeof(savedRegs));
    const uint32_t savedPc = ctx->pc;
    const uint32_t savedSp = getRegU32(ctx, 29);

    const uint32_t udm = f50_callg(rdram, ctx, runtime, 0x00196BE0u, 0u, 0u, 0u, 0u);
    uint32_t chara0 = udm ? f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 0u, 0u, 0u) : 0u;
    uint32_t chara1 = udm ? f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 1u, 0u, 0u) : 0u;
    const uint32_t activeBefore = udm ? f50_7_equip_id(rdram, udm, 0x44D96u) : 0xFFFFFFFFu;
    const uint32_t c0MainBefore = f50_7_equip_id(rdram, chara0, 0x322u);
    const uint32_t c0SideBefore = f50_7_equip_id(rdram, chara0, 0x24Au);
    const uint32_t c0SkinBefore = f50_7_equip_id(rdram, chara0, 0x2B6u);
    const uint32_t c1MainBefore = f50_7_equip_id(rdram, chara1, 0x322u);
    const uint32_t c1SideBefore = f50_7_equip_id(rdram, chara1, 0x24Au);
    const uint32_t c1SkinBefore = f50_7_equip_id(rdram, chara1, 0x2B6u);
    bool repaired = false;
    uint32_t costumeSeeds = 0u;

    if (udm != 0u &&
        (f50_7_has_missing_dungeon_equipment(rdram, chara0) ||
         f50_7_has_missing_dungeon_equipment(rdram, chara1)))
    {
        // Use the native debug dungeon preset first; direct equips below cover any
        // slots the preset leaves empty in this headless route.
        f50_callg(rdram, ctx, runtime, 0x001A1B80u, udm, 4u, 0u, 0u);
        repaired = true;

        chara0 = f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 0u, 0u, 0u);
        chara1 = f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 1u, 0u, 0u);
        if (f50_7_has_missing_dungeon_equipment(rdram, chara0))
        {
            static constexpr uint32_t kMaxEquips[] = {2u, 0x17u, 10u};
            f50_7_apply_direct_equips(rdram, ctx, runtime, udm, 0u, kMaxEquips,
                                      static_cast<uint32_t>(sizeof(kMaxEquips) / sizeof(kMaxEquips[0])));
        }
        if (f50_7_has_missing_dungeon_equipment(rdram, chara1))
        {
            static constexpr uint32_t kMonicaEquips[] = {0x7Fu, 0x85u, 0x10Au, 0x6Eu, 0x5Bu};
            f50_7_apply_direct_equips(rdram, ctx, runtime, udm, 1u, kMonicaEquips,
                                      static_cast<uint32_t>(sizeof(kMonicaEquips) / sizeof(kMonicaEquips[0])));
        }

        static constexpr uint64_t kDefaultCostumeAttr = 0x00000001274521CBull;
        const uint64_t savedCostumeAttr = dc2_read_u64(rdram, udm + 0x45598u);
        const uint64_t costumeAttr = savedCostumeAttr ? savedCostumeAttr : kDefaultCostumeAttr;
        if (savedCostumeAttr == 0u)
        {
            dc2_write_u32(rdram, udm + 0x45598u, static_cast<uint32_t>(kDefaultCostumeAttr));
            dc2_write_u32(rdram, udm + 0x4559Cu, static_cast<uint32_t>(kDefaultCostumeAttr >> 32u));
        }

        if (f50_7_trace_selected_map_enabled())
        {
            std::fprintf(stderr, "[F50.7:cosraw]");
            for (uint32_t i = 0u; i < 34u; ++i)
            {
                const uint32_t rec = dc2_read_u32(rdram, 0x0035CC40u + i * 4u);
                std::fprintf(stderr, " %u:%u/%u", i, rec & 0xFFFFu, (rec >> 16u) & 0xFFFFu);
            }
            std::fprintf(stderr, "\n");
        }

        const uint32_t scratch = savedSp - 0x200u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara0, 0x24Au,
                                                costumeAttr, 6u, scratch + 0x00u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara0, 0x322u,
                                                costumeAttr, 5u, scratch + 0x20u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara0, 0x2B6u,
                                                costumeAttr, 7u, scratch + 0x40u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara1, 0x24Au,
                                                costumeAttr, 9u, scratch + 0x60u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara1, 0x322u,
                                                costumeAttr, 8u, scratch + 0x80u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_costume_slot(rdram, ctx, runtime, udm, chara1, 0x2B6u,
                                                costumeAttr, 10u, scratch + 0xA0u) ? 1u : 0u;

        // If the early debug-menu route cannot query item types yet, use the
        // known default costume IDs from cosbit_table. The Monica IDs match the
        // title debug route's direct equips; the Max IDs are the corresponding
        // first entries for the type-5/6/7 ranges.
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara0, 0x322u, 111u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara0, 0x24Au, 117u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara0, 0x2B6u, 124u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara1, 0x322u, 127u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara1, 0x24Au, 133u) ? 1u : 0u;
        costumeSeeds += f50_7_seed_used_data_id(rdram, ctx, runtime, udm, chara1, 0x2B6u, 266u) ? 1u : 0u;
    }

    chara0 = udm ? f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 0u, 0u, 0u) : 0u;
    chara1 = udm ? f50_callg(rdram, ctx, runtime, 0x0019B490u, udm, 1u, 0u, 0u) : 0u;
    const uint32_t activeAfter = udm ? f50_7_equip_id(rdram, udm, 0x44D96u) : 0xFFFFFFFFu;

    if (f50_7_trace_selected_map_enabled())
    {
        std::fprintf(stderr,
                     "[F50.7:equip] udm=0x%x repaired=%u seeds=%u active=%u->%u c0=(%u,%u,%u)->(%u,%u,%u) c1=(%u,%u,%u)->(%u,%u,%u)\n",
                     udm, repaired ? 1u : 0u, costumeSeeds, activeBefore, activeAfter,
                     c0MainBefore, c0SideBefore, c0SkinBefore,
                     f50_7_equip_id(rdram, chara0, 0x322u),
                     f50_7_equip_id(rdram, chara0, 0x24Au),
                     f50_7_equip_id(rdram, chara0, 0x2B6u),
                     c1MainBefore, c1SideBefore, c1SkinBefore,
                     f50_7_equip_id(rdram, chara1, 0x322u),
                     f50_7_equip_id(rdram, chara1, 0x24Au),
                     f50_7_equip_id(rdram, chara1, 0x2B6u));
    }

    std::memcpy(ctx->r, savedRegs, sizeof(savedRegs));
    ctx->pc = savedPc;
}

static const char *f50_7_fallback_item_path(uint32_t itemId)
{
    switch (itemId)
    {
    case 111u: return "mainchr/ha001.chr";
    case 117u: return "mainchr/sh001.chr";
    case 124u: return "mainchr/ac002.chr";
    case 127u: return "mainchr/ac005.chr";
    case 133u: return "mainchr/bo005.chr";
    case 266u: return "mainchr/c02fuku4.chr";
    default: return nullptr;
    }
}

static bool f50_7_guest_cstr_empty(uint8_t *rdram, uint32_t addr)
{
    const char *p = reinterpret_cast<const char *>(getConstMemPtr(rdram, addr));
    return p == nullptr || p[0] == '\0';
}

static void f50_7_write_item_path(uint8_t *rdram, const char *path)
{
    constexpr uint32_t kItemFilePath = 0x01E74190u;
    constexpr size_t kItemFilePathSize = 128u;
    uint8_t *dst = getMemPtr(rdram, kItemFilePath);
    if (!dst || !path)
        return;

    const size_t n = std::strlen(path);
    const size_t copyLen = (n < kItemFilePathSize - 1u) ? n : (kItemFilePathSize - 1u);
    std::memcpy(dst, path, copyLen);
    dst[copyLen] = 0u;
}

static void f50_7_get_item_file_path_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t itemId = getRegU32(ctx, 4) & 0xFFFFu;
    GetItemFilePath__Fii_0x195d40(rdram, ctx, runtime);

    constexpr uint32_t kItemFilePath = 0x01E74190u;
    const char *fallback = f50_7_fallback_item_path(itemId);
    if (!f50_6_debug_menu_enabled() || fallback == nullptr || !f50_7_guest_cstr_empty(rdram, getRegU32(ctx, 2)))
        return;

    f50_7_write_item_path(rdram, fallback);
    setReturnU32(ctx, kItemFilePath);
    if (f50_7_trace_selected_map_enabled())
    {
        std::fprintf(stderr, "[F50.7:itempath] id=%u -> '%s'\n", itemId, fallback);
    }
}

static void f50_7_random_circle_step_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t chara = self + 0x40u;

    uint32_t vt = self ? dc2_read_u32(rdram, chara) : 0u;
    uint32_t slot = vt ? dc2_read_u32(rdram, vt + 0xD4u) : 0u;
    uint32_t repaired = 0u;
    if (self == 0x01EA4B20u && (vt == 0u || slot == 0u))
    {
        repaired = f50_7_repair_dng_main_static_objects(rdram);
        vt = dc2_read_u32(rdram, chara);
        slot = vt ? dc2_read_u32(rdram, vt + 0xD4u) : 0u;
    }

    if (f50_7_trace_selected_map_enabled() && (n <= 64u || repaired != 0u || slot == 0u))
    {
        std::fprintf(stderr,
                     "[F50.7:rcstep] n=%u self=0x%x chara=0x%x ra=0x%x vt=0x%x slot=0x%x repaired=0x%x\n",
                     n, self, chara, ra, vt, slot, repaired);
    }

    if (slot == 0u)
    {
        ctx->pc = ra;
        return;
    }
    if (!runtime->hasFunction(slot))
    {
        ctx->pc = slot;
        return;
    }

    const auto savedRa = ctx->r[31];
    f50_set_reg(ctx, 4, chara);
    f50_set_reg(ctx, 31, 0u);
    f50_run_guest_call(rdram, ctx, runtime, slot);

    const uint32_t postPc = ctx->pc;
    ctx->r[31] = savedRa;
    ctx->pc = postPc == 0u ? ra : postPc;
}

// F55: A/B probe for the dungeon free-map rectangle divergence (F53 blocker).
// Logs CalcGlidPutPos@0x1EA890 inputs (GLID grid shorts, view-offset floats at
// this+0x100/0x104) and outputs, to compare against live PCSX2 ground truth
// (view=(152,208), gridX 2..10, outputs on-screen 248..560 x 208..408).
static bool f55_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F55");
    return enabled;
}

static float f55_read_f32(uint8_t *rdram, uint32_t addr)
{
    const uint32_t bits = dc2_read_u32(rdram, addr);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

static void f55_calc_glid_put_pos_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    const uint32_t glid = getRegU32(ctx, 5);
    const uint32_t outX = getRegU32(ctx, 6);
    const uint32_t outY = getRegU32(ctx, 7);
    const uint32_t noView = getRegU32(ctx, 8); // 5th arg in $t0
    const uint32_t ra = getRegU32(ctx, 31);
    const bool log = f55_trace_enabled() && n <= 96u;

    int16_t type = 0, gx = 0, gy = 0, mode = 0;
    float viewX = 0.0f, viewY = 0.0f;
    uint32_t mgr = 0u, glidArr = 0u, glidCnt = 0u;
    uint32_t visible = 0u, glidA = 0u, glidB = 0u, texA = 0u, texB = 0u;
    float alpha = 0.0f, tgtX = 0.0f, tgtY = 0.0f;
    if (log)
    {
        if (glid != 0u)
        {
            type = static_cast<int16_t>(dc2_read_u32(rdram, glid) & 0xFFFFu);
            gx = static_cast<int16_t>((dc2_read_u32(rdram, glid) >> 16) & 0xFFFFu);
            gy = static_cast<int16_t>(dc2_read_u32(rdram, glid + 4u) & 0xFFFFu);
        }
        if (self != 0u)
        {
            mode = static_cast<int16_t>(dc2_read_u32(rdram, self + 0xCu) & 0xFFFFu);
            viewX = f55_read_f32(rdram, self + 0x100u);
            viewY = f55_read_f32(rdram, self + 0x104u);
            tgtX = f55_read_f32(rdram, self + 0x108u);
            tgtY = f55_read_f32(rdram, self + 0x10Cu);
            mgr = dc2_read_u32(rdram, self + 4u);
            visible = dc2_read_u32(rdram, self + 8u) & 0xFFu;   // gate 1 in CDngFreeMap::Draw
            alpha = f55_read_f32(rdram, self + 0xF0u);           // gate 2
            texB = dc2_read_u32(rdram, self + 0xD8u);            // gate 3 (map texture ptr)
            texA = dc2_read_u32(rdram, self + 0xD4u);
            glidA = dc2_read_u32(rdram, self + 0xC4u);           // player glid
            glidB = dc2_read_u32(rdram, self + 0xCCu);           // current-room glid
            if (mgr != 0u)
            {
                glidArr = dc2_read_u32(rdram, mgr + 4u);
                glidCnt = dc2_read_u32(rdram, mgr + 8u);
            }
        }
    }

    CalcGlidPutPos__11CDngFreeMapFP9GLID_INFORfRfi_0x1ea890(rdram, ctx, runtime);

    if (log)
    {
        const float ox = (outX != 0u) ? f55_read_f32(rdram, outX) : 0.0f;
        const float oy = (outY != 0u) ? f55_read_f32(rdram, outY) : 0.0f;
        std::fprintf(stderr,
                     "[F55:cgpp] n=%u ra=0x%x this=0x%x glid=0x%x noview=%u type=%d gx=%d gy=%d mode=%d "
                     "view=(%.2f,%.2f) tgt=(%.2f,%.2f) mgr=0x%x arr=0x%x cnt=%u "
                     "vis=%u alpha=%.2f texA=0x%x texB=0x%x glidA=0x%x glidB=0x%x -> out=(%.2f,%.2f)\n",
                     n, ra, self, glid, noView, type, gx, gy, mode,
                     viewX, viewY, tgtX, tgtY, mgr, glidArr, glidCnt,
                     visible, alpha, texA, texB, glidA, glidB, ox, oy);
    }
}

static void f55_read_cstr(uint8_t *rdram, uint32_t addr, char *out, uint32_t cap)
{
    out[0] = '\0';
    if (addr == 0u)
        return;
    uint32_t i = 0u;
    for (; i + 1u < cap; ++i)
    {
        const uint8_t b = (uint8_t)(dc2_read_u32(rdram, addr + i) & 0xFFu);
        if (b == 0u)
            break;
        out[i] = (b >= 0x20u && b < 0x7Fu) ? (char)b : '.';
    }
    out[i] = '\0';
}

static void f55_init_end_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f55_trace_enabled() && n <= 8u)
        std::fprintf(stderr, "[F55:initend] n=%u this=0x%x ra=0x%x\n",
                     n, getRegU32(ctx, 4), getRegU32(ctx, 31));
    InitEnd__12CMenuTreeMapFv_0x1ef9f0(rdram, ctx, runtime);
}

static void f55_set_texture_info_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t self = getRegU32(ctx, 4);
    SetTextureInfo__11CDngFreeMapFv_0x1eabe0(rdram, ctx, runtime);
    if (f55_trace_enabled() && n <= 8u)
        std::fprintf(stderr,
                     "[F55:settex] n=%u this=0x%x -> d4=0x%x d8=0x%x dc=0x%x e0=0x%x (post pc=0x%x)\n",
                     n, self,
                     dc2_read_u32(rdram, self + 0xD4u), dc2_read_u32(rdram, self + 0xD8u),
                     dc2_read_u32(rdram, self + 0xDCu), dc2_read_u32(rdram, self + 0xE0u),
                     ctx->pc);
}

static void f55_get_pack_file_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t pack = getRegU32(ctx, 4);
    const uint32_t namePtr = getRegU32(ctx, 5);
    const uint32_t ra = getRegU32(ctx, 31);
    GetPackFile__FPUiPcPi_0x149cd0(rdram, ctx, runtime);
    if (f55_trace_enabled() && n <= 96u)
    {
        char nm[40];
        f55_read_cstr(rdram, namePtr, nm, sizeof(nm));
        std::fprintf(stderr, "[F55:packfile] n=%u pack=0x%x name='%s' ra=0x%x -> v0=0x%x\n",
                     n, pack, nm, ra, getRegU32(ctx, 2));
    }
}

static void f55_get_read_bg_file_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t idx = getRegU32(ctx, 4);
    const uint32_t ra = getRegU32(ctx, 31);
    GetReadBGFile__Fi_0x148c70(rdram, ctx, runtime);
    if (f55_trace_enabled() && n <= 64u)
        std::fprintf(stderr, "[F55:readbg] n=%u idx=%u ra=0x%x -> v0=0x%x\n",
                     n, idx, ra, getRegU32(ctx, 2));
}

static void f55_load_file_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t namePtr = getRegU32(ctx, 4);
    const uint32_t buf = getRegU32(ctx, 5);
    const uint32_t ra = getRegU32(ctx, 31);
    char nm[64];
    char curdir[64];
    uint32_t headerNum = 0u, dataSector = 0u, foundIdx = 0xFFFFFFFFu;
    char e0[48], e1[48];
    e0[0] = e1[0] = '\0';
    const bool log = f55_trace_enabled() && n <= 64u;
    if (log)
    {
        f55_read_cstr(rdram, namePtr, nm, sizeof(nm));
        f55_read_cstr(rdram, 0x334390u, curdir, sizeof(curdir)); // CurrentDir
        headerNum = dc2_read_u32(rdram, 0x376D8Cu);              // header_num
        dataSector = dc2_read_u32(rdram, 0x376D94u);             // data_sector
        char full[128];
        std::snprintf(full, sizeof(full), "%s%s", curdir, nm);
        for (uint32_t i = 0u; i < headerNum && i < 16384u; ++i)
        {
            const uint32_t entryName = dc2_read_u32(rdram, 0x382680u + i * 16u);
            if (entryName == 0u)
                continue;
            char en[64];
            f55_read_cstr(rdram, entryName, en, sizeof(en));
            if (i == 0u)
                std::snprintf(e0, sizeof(e0), "%s", en);
            if (i == 1u)
                std::snprintf(e1, sizeof(e1), "%s", en);
            if (_stricmp(en, full) == 0)
            {
                foundIdx = i;
                break;
            }
        }
    }
    LoadFileBG__FPcP1Pi_0x148930(rdram, ctx, runtime);
    if (log)
    {
        std::fprintf(stderr,
                     "[F55:loadbg] n=%u name='%s' curdir='%s' headerNum=%u dataSector=%u foundIdx=%d "
                     "e0='%s' e1='%s' buf=0x%x ra=0x%x -> v0=0x%x (post pc=0x%x)\n",
                     n, nm, curdir, headerNum, dataSector, (int)foundIdx, e0, e1,
                     buf, ra, getRegU32(ctx, 2), ctx->pc);
    }
}

static void f55_init_read_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f55_trace_enabled() && n <= 32u)
        std::fprintf(stderr, "[F55:initreadbg] n=%u ra=0x%x\n", n, getRegU32(ctx, 31));
    InitReadBG__Fv_0x1488d0(rdram, ctx, runtime);
}

static void f55_start_read_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f55_trace_enabled() && n <= 32u)
        std::fprintf(stderr, "[F55:startreadbg] n=%u ra=0x%x\n", n, getRegU32(ctx, 31));
    StartReadBG__Fv_0x148cc0(rdram, ctx, runtime);
}

static void f48_4_init_dungeon_main_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_4_init_log;
    g_f50_1_dungeon_active = true; // F50.1: arm placement-new probe for the dungeon path
    const uint32_t gp = getRegU32(ctx, 28);
    // F50.4: at InitDungeonMain ENTRY (before any dungeon code), read MainScene's
    // char count (scene+0x40) and the embedded CScene-vtable pointer (scene+0x10548)
    // that __sinit_mainloop@0x373580 sets to 0x375fe0 (__vt__6CScene). If both are 0
    // here, the static initializer never ran. Also dump the static __vt__6CScene
    // (0x375fe0, always loaded rodata) slots 0..0x14 to locate Initialize@0x282ea0.
    if (f48_4_trace_postcostume_enabled() && n == 1u)
    {
        const uint32_t sceneAbs = 0x01DD8260u; // &MainScene (from L158066 GetMainScene)
        const uint32_t vt = 0x00375FE0u;       // __vt__6CScene
        std::fprintf(stderr,
                     "[F50.4:initentry] count=%u vptr@+0x10548=0x%x | vt0=0x%x vt4=0x%x vt8=0x%x vtC=0x%x vt10=0x%x vt14=0x%x\n",
                     dc2_read_u32(rdram, sceneAbs + 0x40u),
                     dc2_read_u32(rdram, sceneAbs + 0x10548u),
                     dc2_read_u32(rdram, vt + 0x00u), dc2_read_u32(rdram, vt + 0x04u),
                     dc2_read_u32(rdram, vt + 0x08u), dc2_read_u32(rdram, vt + 0x0Cu),
                     dc2_read_u32(rdram, vt + 0x10u), dc2_read_u32(rdram, vt + 0x14u));
    }
    if (f48_4_trace_postcostume_enabled() && n <= 8u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=InitDungeonMain.enter n=%u frame=%u ra=0x%x loop=%u next=%u a0=0x%x d62c=0x%x\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u),
                     getRegU32(ctx, 4), dc2_read_u32(rdram, 0x01ECD62Cu));
    }
    // F50.4 FIX (always on, not gated): restore the MainScene CScene vtable
    // pointer before InitDungeonMain's scene-entry Initialize dispatch runs.
    f50_4_repair_main_scene_vtable(rdram);
    f50_7_repair_dng_main_static_objects(rdram);
    f50_7_repair_debug_dungeon_equipment(rdram, ctx, runtime);
    InitDungeonMain__F13INIT_LOOP_ARG_0x1cc040(rdram, ctx, runtime);
    if (f48_4_trace_postcostume_enabled() && n <= 8u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=InitDungeonMain.exit n=%u frame=%u ret=%u\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 2));
    }
}

// F56: classify the in-dungeon state each frame. DngStatus selects the
// LoopDungeonMain branch: 0=3D world (DngMainDraw@0x1CF090), 1/4=menu
// (MenuMainDraw), 2=event, 3=event-edit, 5=exit. DngTreeMode is the
// floor-select treemap sub-state (0=display map, 1=transition/confirm).
// Addresses resolved from assembly:
//   DngStatus  : lui at,0x1ea; lw s1,-0x920(at)        -> 0x01E9F6E0 (word)
//   DngTreeMode: lh a0,-0x70fc(gp) (DngTreeMapDraw)     -> gp-0x70fc = 0x003773F4 (half)
// Quiet unless DC2_TRACE_F56. Bounded: logs on state change + every 60th
// frame, capped, so the floor-select->3D transition is visible compactly.
static bool f56_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F56");
    return enabled;
}

static void f56_log_dungeon_state(uint8_t *rdram, uint32_t n)
{
    if (!f56_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    static int32_t s_prevStatus = -1;
    static int32_t s_prevTree = -1;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u);          // DngStatus
    const int32_t tree = (int32_t)(dc2_read_u32(rdram, 0x003773F4u) & 0xFFFFu); // DngTreeMode (half)
    const bool changed = (status != s_prevStatus) || (tree != s_prevTree);
    if ((changed || (n % 60u == 0u)) && s_lines < 240u)
    {
        ++s_lines;
        const char *mode = (status == 0) ? "3D-DngMainDraw" : ((status == 1 || status == 4) ? "MENU-MainDraw" : ((status == 2) ? "EVENT" : ((status == 3) ? "EVENT-EDIT" : ((status == 5) ? "EXIT" : "?"))));
        std::fprintf(stderr,
                     "[F56:state] n=%u frame=%u DngStatus=%d(%s) DngTreeMode=%d%s\n",
                     n, g_f40_frame_counter, status, mode, tree,
                     changed ? " *CHANGED*" : "");
        s_prevStatus = status;
        s_prevTree = tree;
    }
}

// F57: trace the dungeon floor-entrance EVENT state machine. After the treemap
// confirm advances DngStatus 4->2 (EVENT), RunMainEvent@0x1D1360 sets DngStatus=0
// (free-roam) ONLY when EventLoop@0x2555E0 returns 1, which happens when the event
// CRunScript at 0x1ECE3D0 reports finished (its +0x3c flag, aliased DAT_01ece40c).
// Headlessly that never fires, so the event never terminates. This probe surfaces
// WHICH opcode/wait the script parks on.
//   EventLoop globals:
//     DAT_01ece40c (0x01ECE40C) = script-done flag (== CRunScript+0x3c). !=0 -> EdEventEnd, return 1.
//     DAT_01ece4fc (0x01ECE4FC) = pending return-action code (0xf=load map, 3/4/7/8/...).
//     DAT_01ece500 (0x01ECE500) = door/camera sub-mode (1/2/3/4 = special, 0 = run script).
//     DAT_01ece504 (0x01ECE504) = sound-fade state (1/2/3 = fade/stream skip path).
//     DAT_01ece508 (0x01ECE508) = skip-event arming value.
//   CRunScript@0x1ECE3D0 cursor (layout from resume/run/exe __10CRunScript):
//     +0x38 (0x01ECE408) = current vmcode_t* (resume runs exe while non-zero);
//                          *(vmcode) = opcode id (the exe switch value) = the parked command.
//     +0x40 (0x01ECE410) = skip flag, +0x50 (0x01ECE420) = wait/yield flag.
// Quiet unless DC2_TRACE_F57. Logs on any state change + every 60th event frame, capped.
static bool f57_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F57");
    return enabled;
}

static void f57_log_event_state(uint8_t *rdram, uint32_t n)
{
    if (!f57_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    static uint32_t s_prevSig = 0xFFFFFFFFu;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u); // DngStatus
    // Only meaningful during/after the entrance event (status 2 EVENT, or 0 once it
    // terminates). Skip the menu/treemap frames to keep the trace on the blocker.
    if (status != 2 && status != 0)
        return;
    const uint32_t done = dc2_read_u32(rdram, 0x01ECE40Cu);   // CRunScript+0x3c
    const uint32_t act = dc2_read_u32(rdram, 0x01ECE4FCu);    // DAT_01ece4fc
    const uint32_t door = dc2_read_u32(rdram, 0x01ECE500u);   // DAT_01ece500
    const uint32_t snd = dc2_read_u32(rdram, 0x01ECE504u);    // DAT_01ece504
    const uint32_t skip = dc2_read_u32(rdram, 0x01ECE508u);   // DAT_01ece508
    const uint32_t pc = dc2_read_u32(rdram, 0x01ECE408u);     // CRunScript+0x38 (vmcode_t*)
    const uint32_t skipf = dc2_read_u32(rdram, 0x01ECE410u);  // CRunScript+0x40
    const uint32_t yield = dc2_read_u32(rdram, 0x01ECE420u);  // CRunScript+0x50
    const uint32_t op = (pc && pc < 0x02000000u) ? dc2_read_u32(rdram, pc) : 0xFFFFFFFFu; // *(vmcode)=opcode
    const uint32_t sig = status ^ (done << 1) ^ (act << 4) ^ (door << 8) ^ (snd << 12) ^ (op << 16) ^ pc;
    if ((sig != s_prevSig || (n % 60u == 0u)) && s_lines < 300u)
    {
        ++s_lines;
        std::fprintf(stderr,
                     "[F57:event] n=%u frame=%u DngStatus=%d done=0x%x act=0x%x door=0x%x snd=0x%x skip=0x%x pc=0x%x op=0x%x skipf=0x%x yield=0x%x\n",
                     n, g_f40_frame_counter, status, done, act, door, snd, skip, pc, op, skipf, yield);
        s_prevSig = sig;
    }
}

// F59: free-roam (DngStatus==0) render brightness. The event scene (status 2)
// renders the full 3D world bright (~631830 NonZero); on entering free-roam the
// frame drops to ~50000. DngMainDraw@0x1CF090 is the SAME code for status {0,2,3},
// so dim free-roam must be DATA-driven. This reads the exact guest globals that
// function consumes:
//   DngMainMap      = *(gp-0x724c) @0x3772A4 (159632) -- gates the whole map +
//                     character geometry block at 159678; 0 => only sky+HUD draw.
//   BattleAreaScene = *(gp-0x7250) @0x3772A0
//      +0x6c (float) = light-color SCALE multiplied into EVERY light component
//                      (159639-159653); near-0 => unlit/black geometry.
//      +0x08 (uint)  = draw flags: &0x20 skip sky, &0x80 skip map+chara draw.
//   DngMainScene    = *(gp-0x7254) @0x37729C (+0x2e54 cam idx, +0x2e5c map idx).
// PCSX2 ground truth (status 2): DngMainMap=0x106a030, lightScale=1.0, flags=0x401,
// camIdx=1, mapIdx=0. Quiet unless DC2_TRACE_F59. Logs on change + every 60th frame.
static bool f59_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_F59");
    return enabled;
}

static void f59_log_freeroam_state(uint8_t *rdram, uint32_t n)
{
    if (!f59_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    static uint32_t s_prevSig = 0xFFFFFFFFu;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u);
    if (status != 0 && status != 2) // free-roam, plus event for A/B contrast
        return;
    const uint32_t dngMap = dc2_read_u32(rdram, 0x003772A4u);
    const uint32_t bas = dc2_read_u32(rdram, 0x003772A0u);
    const uint32_t scene = dc2_read_u32(rdram, 0x0037729Cu);
    const uint32_t scaleU = bas ? dc2_read_u32(rdram, bas + 0x6cu) : 0u;
    float scale = 0.0f;
    std::memcpy(&scale, &scaleU, sizeof(float));
    const uint32_t flags = bas ? dc2_read_u32(rdram, bas + 0x08u) : 0u;
    const uint32_t camIdx = scene ? dc2_read_u32(rdram, scene + 0x2e54u) : 0u;
    const uint32_t mapIdx = scene ? dc2_read_u32(rdram, scene + 0x2e5cu) : 0u;
    const uint32_t sig = (uint32_t)status ^ (dngMap << 1) ^ (scaleU << 3) ^ (flags << 7) ^ (camIdx << 11) ^ (mapIdx << 15);
    if ((sig != s_prevSig || (n % 60u == 0u)) && s_lines < 240u)
    {
        ++s_lines;
        std::fprintf(stderr,
                     "[F59:freeroam] n=%u frame=%u DngStatus=%d DngMainMap=0x%x BAS=0x%x lightScale=%.4f flags=0x%x camIdx=%u mapIdx=%u\n",
                     n, g_f40_frame_counter, status, dngMap, bas, scale, flags, camIdx, mapIdx);
        s_prevSig = sig;
    }
}

// F59: UNCONDITIONAL per-presented-frame logger (called from f29 hook, fires every
// frame regardless of active loop). Maps DngStatus + DngMainMap + lightScale across
// the bright->dim transition that f59_log_freeroam_state's sig-dedup hides. Every 30th.
static void f59_log_frame(uint8_t *rdram, uint32_t n, uint32_t loopNo)
{
    if (!f59_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    if (n % 30u != 0u || s_lines >= 400u)
        return;
    ++s_lines;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u);
    const uint32_t dngMap = dc2_read_u32(rdram, 0x003772A4u);
    const uint32_t bas = dc2_read_u32(rdram, 0x003772A0u);
    const uint32_t scene = dc2_read_u32(rdram, 0x0037729Cu);
    const uint32_t scaleU = bas ? dc2_read_u32(rdram, bas + 0x6cu) : 0u;
    float scale = 0.0f;
    std::memcpy(&scale, &scaleU, sizeof(float));
    const uint32_t flags = bas ? dc2_read_u32(rdram, bas + 0x08u) : 0u;
    const uint32_t mapIdx = scene ? dc2_read_u32(rdram, scene + 0x2e5cu) : 0xDEADu;
    std::fprintf(stderr,
                 "[F59:frame] n=%u frame=%u loopNo=%u DngStatus=%d DngMainMap=0x%x scale=%.3f flags=0x%x mapIdx=%d\n",
                 n, g_f40_frame_counter, loopNo, status, dngMap, scale, flags, (int32_t)mapIdx);
}

// F57: per-PRESENTED-FRAME logger (called from f29_mgendframe_probe, which fires
// every frame regardless of which loop function is active). Unlike f57_log_event_state
// (only hit while LoopDungeonMain@0x1CEA00 runs), this survives a loop swap, so it
// reveals what the game transitions to if the dungeon loop exits. Logs the current
// loop number (gp-0x7524) + next loop (gp-0x7520) alongside DngStatus + event globals.
static void f57_log_frame_loop(uint8_t *rdram, uint32_t n, uint32_t loopNo, uint32_t nextLoopNo)
{
    if (!f57_trace_enabled())
        return;
    static uint32_t s_lines = 0;
    static uint32_t s_prevSig = 0xFFFFFFFFu;
    const int32_t status = (int32_t)dc2_read_u32(rdram, 0x01E9F6E0u); // DngStatus
    const uint32_t done = dc2_read_u32(rdram, 0x01ECE40Cu);
    const uint32_t door = dc2_read_u32(rdram, 0x01ECE500u);
    const uint32_t snd = dc2_read_u32(rdram, 0x01ECE504u);
    const uint32_t pc = dc2_read_u32(rdram, 0x01ECE408u);
    const uint32_t op = (pc && pc < 0x02000000u) ? dc2_read_u32(rdram, pc) : 0xFFFFFFFFu;
    const uint32_t sig = loopNo ^ (nextLoopNo << 4) ^ ((uint32_t)status << 8) ^ (door << 12) ^ (snd << 16) ^ (op << 20);
    if ((sig != s_prevSig || (n % 120u == 0u)) && s_lines < 400u)
    {
        ++s_lines;
        std::fprintf(stderr,
                     "[F57:loop] n=%u loopNo=%u nextLoop=%u DngStatus=%d done=0x%x door=0x%x snd=0x%x op=0x%x\n",
                     n, loopNo, nextLoopNo, status, done, door, snd, op);
        s_prevSig = sig;
    }
}

// F56: trace the floor-select treemap state machine. param object (a0) layout
// (from Step__12CMenuTreeMapFv decompile): *(short)(this+0) = top menu state
// (0xc=jump/confirm-pending, 2=run jump script, 1=enter/fade, 0/else=interactive
// nav+button), *(short)(this+0x14) = interactive sub-state (psVar17[10]:
// 0=floor navigation, 1=enter/decide, 2=L/R page). Logs the trajectory + the
// active scripted pad so we can see whether confirm presses reach the menu and
// which sub-state gates the dungeon load. Quiet unless DC2_TRACE_F56.
static void f56_treemap_step_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (f56_trace_enabled())
    {
        static uint32_t s_calls = 0;
        static uint32_t s_lines = 0;
        static int32_t s_prevTop = -1;
        static int32_t s_prevSub = -1;
        const uint32_t self = getRegU32(ctx, 4);
        const int32_t top = self ? (int32_t)(dc2_read_u32(rdram, self + 0x00u) & 0xFFFFu) : -1;
        const int32_t sub = self ? (int32_t)(dc2_read_u32(rdram, self + 0x14u) & 0xFFFFu) : -1;
        // CGamePad singleton @0x3D76E0: +0x4 = current held buttons (CGamePad's
        // own bit layout, tested by Down__8CGamePad), +0x9c = previous frame.
        // Logging this while injecting a known scePad bit reveals the remap that
        // MenuCheckPushButton consumes (decide = Down(0x10) -> uVar7==4 -> top=0xc).
        const uint32_t cgcur = dc2_read_u32(rdram, 0x003D76E4u);
        // psVar17[0x90] (byte 0x120) = currently-selected room GLID_INFO* (the
        // floor the confirm would enter). psVar17[0x8c] (byte 0x118) = dungeon id.
        // If room==0 the confirm path beeps (MenuSePlay 5) instead of arming the
        // DngAskMessageDrawFlag "enter floor?" dialog -> 3D never loads.
        const uint32_t room = self ? dc2_read_u32(rdram, self + 0x120u) : 0u;
        const uint32_t dngId = self ? (dc2_read_u32(rdram, self + 0x118u) & 0xFFFFu) : 0u;
        // padtbl_3372 (US +8 = 0x350c68): MenuCheckPushButton maps RIGHT->*0x350c68,
        // DOWN->*0x350c6c. Whichever == 1 is the uVar7==1 "arm enter-dialog" button.
        const uint32_t padR = dc2_read_u32(rdram, 0x00350C68u);
        const uint32_t padD = dc2_read_u32(rdram, 0x00350C6Cu);
        const uint32_t c = ++s_calls;
        const bool changed = (top != s_prevTop) || (sub != s_prevSub);
        if ((changed || g_f40_pressed != 0u || (c % 60u == 0u)) && s_lines < 220u)
        {
            ++s_lines;
            std::fprintf(stderr,
                         "[F56:treemap] c=%u frame=%u this=0x%x top=%d sub=%d pad=0x%04x cgcur=0x%08x room=0x%x dng=%u padR=%u padD=%u%s\n",
                         c, g_f40_frame_counter, self, top, sub,
                         (unsigned)g_f40_pressed, cgcur, room, dngId, padR, padD, changed ? " *CHANGED*" : "");
            s_prevTop = top;
            s_prevSub = sub;
        }
    }
    Step__12CMenuTreeMapFv_0x1eff40(rdram, ctx, runtime);
}

static void f48_4_loop_dungeon_main_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_4_main_log;
    f56_log_dungeon_state(rdram, n);
    f57_log_event_state(rdram, n);
    f59_log_freeroam_state(rdram, n);
    if (f48_4_trace_postcostume_enabled() && n <= 6u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=LoopDungeonMain.enter n=%u frame=%u ra=0x%x\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31));
    }
    // F50.4: on the first dungeon frame, dump the player-character state that
    // DngStep@0x1D06C0 dereferences. The crash is a null-pointer vtable call:
    //   lw a0,-0x7228(gp) ; MainChara   -> a0=0x0 at [dispatch:first-bad-pc]
    //   lw t9,0x0(a0)     ; *MainChara  -> reads guest_ram[0]
    //   lw t9,0xdc(t9)    ; vtable+0xdc -> 0x1, jalr -> bad=0x1
    // MainChara = GetCharacter(DngMainScene,0) (InitDungeonMain @0x1cc3bc). If
    // it is 0, slot 0 of the scene character array was never populated. Dump the
    // scene char count (scene+0x40, set to 0x80 by Initialize__6CSceneFv), slot 0
    // active flag (slot+0x00) and object (slot+0x34) to localise the break.
    if (f48_4_trace_postcostume_enabled() && n == 1u)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t mainChara = dc2_read_u32(rdram, gp + 0xFFFF8DD8u); // gp-0x7228
        const uint32_t scene = dc2_read_u32(rdram, gp + 0xFFFF8DACu);     // gp-0x7254 DngMainScene
        const uint32_t count = scene ? dc2_read_u32(rdram, scene + 0x40u) : 0u;
        const uint32_t slot0 = scene ? (scene + 0x44u) : 0u;
        const uint32_t slot0Flag = slot0 ? dc2_read_u32(rdram, slot0 + 0x00u) : 0u;
        const uint32_t slot0Obj = slot0 ? dc2_read_u32(rdram, slot0 + 0x34u) : 0u;
        const uint32_t objVt = mainChara ? dc2_read_u32(rdram, mainChara + 0x00u) : 0u;
        const uint32_t vtDc = objVt ? dc2_read_u32(rdram, objVt + 0xDCu) : 0u;
        // The scene-entry init at InitDungeonMain L158085 dispatches through the
        // CScene vtable embedded at scene+0x10548, slot +0x8. Dump that vtable and
        // its slot-8 target so we can see whether it resolves to the real
        // Initialize/InitAllData (which sets scene[0x40]=0x80) or a stub.
        const uint32_t sceneVt = scene ? dc2_read_u32(rdram, scene + 0x10548u) : 0u;
        const uint32_t sceneVt8 = sceneVt ? dc2_read_u32(rdram, sceneVt + 0x8u) : 0u;
        const uint32_t sceneVt4 = sceneVt ? dc2_read_u32(rdram, sceneVt + 0x4u) : 0u;
        const uint32_t sceneVt0 = sceneVt ? dc2_read_u32(rdram, sceneVt + 0x0u) : 0u;
        std::fprintf(stderr,
                     "[F50.4:mainchara] MainChara=0x%x scene=0x%x count=%u slot0=0x%x flag=0x%x slot0obj=0x%x vt=0x%x vt+0xdc=0x%x\n",
                     mainChara, scene, count, slot0, slot0Flag, slot0Obj, objVt, vtDc);
        std::fprintf(stderr,
                     "[F50.4:scenevt] sceneVt=0x%x [+0]=0x%x [+4]=0x%x [+8]=0x%x\n",
                     sceneVt, sceneVt0, sceneVt4, sceneVt8);
    }
    // F64: the audio-gated entrance-event skip is driven from f29_mgendframe_probe
    // (mgEndFrame runs every frame inside LoopDungeonMain's internal loop; this
    // wrapper only fires once on entry).
    LoopDungeonMain__Fv_0x1cea00(rdram, ctx, runtime);
    if (f48_4_trace_postcostume_enabled() && n <= 6u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=LoopDungeonMain.exit n=%u frame=%u ret=%u\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 2));
    }
}

// F49.5: LoopExit[3] = TitleExit @0x29FF30 is dispatched via the MainLoop loop
// table when TitleLoop returns nonzero (NextLoop(2) set). Bracket it to see
// whether the loop-transition (break -> LoopExit -> LoopNo=2 -> sceGsSyncV ->
// LoopInit[2]) actually executes after the costume confirm. Quiet unless
// DC2_TRACE_F48_4 is set; bounded.
static uint32_t g_f48_4_titleexit_log = 0u;

static void f48_4_title_exit_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_4_titleexit_log;
    const uint32_t gp = getRegU32(ctx, 28);
    if (f48_4_trace_postcostume_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=TitleExit.enter n=%u frame=%u ra=0x%x loop=%u next=%u\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31),
                     dc2_read_u32(rdram, gp + 0xFFFF8ADCu),
                     dc2_read_u32(rdram, gp + 0xFFFF8AE0u));
    }
    TitleExit__Fv_0x29ff30(rdram, ctx, runtime);
    if (f48_4_trace_postcostume_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=TitleExit.exit n=%u frame=%u\n",
                     n, g_f40_frame_counter);
    }
}

// F49.5: CaptureEnd @0x14B600 is the last call in MainLoop's loop-transition
// (line ~118461) immediately before the inter-loop sceGsSyncV sequence. If this
// fires after the costume confirm but the transition sceGsSyncV (n>9) does not,
// the hang is in CaptureEnd or the sceGsSyncV itself; if it never fires, the
// hang is in the buffer-copy block before it. Quiet unless DC2_TRACE_F48_4.
static uint32_t g_f48_4_captureend_log = 0u;

static void f48_4_capture_end_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_4_captureend_log;
    if (f48_4_trace_postcostume_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=CaptureEnd.enter n=%u frame=%u ra=0x%x a0=0x%x\n",
                     n, g_f40_frame_counter, getRegU32(ctx, 31), getRegU32(ctx, 4));
    }
    CaptureEnd__8CGamePadFv_0x14b600(rdram, ctx, runtime);
    if (f48_4_trace_postcostume_enabled() && n <= 4u)
    {
        std::fprintf(stderr,
                     "[F48.4:postcostume] tag=CaptureEnd.exit n=%u frame=%u\n",
                     n, g_f40_frame_counter);
    }
}

static void f49_finish_for_mc_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t pcBefore = ctx->pc;
    const uint32_t s0Before = getRegU32(ctx, 16);
    const uint32_t s1Before = getRegU32(ctx, 17);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=FinishForMC.enter n=%u ra=0x%x pc=0x%x s0=0x%x s1=0x%x\n",
                     n, ra, pcBefore,
                     s0Before, s1Before);
    }
    const auto s0 = ctx->r[16];
    const auto s1 = ctx->r[17];
    const auto s2 = ctx->r[18];
    const auto s3 = ctx->r[19];
    const auto s4 = ctx->r[20];
    const auto s5 = ctx->r[21];
    const auto s6 = ctx->r[22];
    const auto s7 = ctx->r[23];
    const auto t0 = std::chrono::steady_clock::now();
    FinishForMC__18CMemoryCardManagerFv_0x2f19a0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    const uint32_t pcAfterRaw = ctx->pc;
    bool tailReturnFixed = false;
    // FinishForMC is a tail jump to sceMcEnd (0x122560). If sceMcEnd leaves
    // PC unchanged, normalize to the original caller continuation.
    if (pcAfterRaw == 0x00122560u)
    {
        ctx->pc = ra;
        tailReturnFixed = true;
    }
    ctx->r[16] = s0;
    ctx->r[17] = s1;
    ctx->r[18] = s2;
    ctx->r[19] = s3;
    ctx->r[20] = s4;
    ctx->r[21] = s5;
    ctx->r[22] = s6;
    ctx->r[23] = s7;
    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const uint32_t pcAfter = ctx->pc;
        const uint32_t raAfter = getRegU32(ctx, 31);
        const uint32_t s0After = getRegU32(ctx, 16);
        const uint32_t s1After = getRegU32(ctx, 17);
        std::fprintf(stderr,
                     "[F49:title] tag=FinishForMC.exit n=%u ra=0x%x ret=0x%x dtMs=%lld pcRaw=0x%x pc=0x%x tailFix=%u raAfter=0x%x s0=0x%x s1=0x%x\n",
                     n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs),
                     pcAfterRaw, pcAfter, tailReturnFixed ? 1u : 0u,
                     raAfter, s0After, s1After);
    }
}

static void f49_init_save_data_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr, "[F49:title] tag=InitSaveData.enter n=%u ra=0x%x\n", n, ra);
    }
    InitSaveData__Fv_0x1908a0(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr, "[F49:title] tag=InitSaveData.exit n=%u ra=0x%x ret=0x%x\n", n, ra, getRegU32(ctx, 2));
    }
}

static void f49_get_user_data_man_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    GetUserDataMan__Fv_0x196be0(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled() && n <= 64u)
    {
        std::fprintf(stderr, "[F49:title] tag=GetUserDataMan n=%u ra=0x%x ret=0x%x\n", n, ra, getRegU32(ctx, 2));
    }
}

static void f49_debug_get_item_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t udm = getRegU32(ctx, 4);
    const uint32_t mode = getRegU32(ctx, 5);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=DebugGetItem.enter n=%u ra=0x%x udm=0x%x mode=%u\n",
                     n, ra, udm, mode);
    }
    DebugGetItem__FP16CUserDataManageri_0x1a1b80(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=DebugGetItem.exit n=%u ra=0x%x ret=0x%x\n",
                     n, ra, getRegU32(ctx, 2));
    }
}

static void f49_set_chr_equip_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t udm = getRegU32(ctx, 4);
    const uint32_t slot = getRegU32(ctx, 5);
    const uint32_t equip = getRegU32(ctx, 6);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=SetChrEquip.enter n=%u ra=0x%x udm=0x%x slot=%u equip=%u\n",
                     n, ra, udm, slot, equip);
    }
    SetChrEquipDirect__16CUserDataManagerFii_0x19d560(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=SetChrEquip.exit n=%u ra=0x%x ret=0x%x\n",
                     n, ra, getRegU32(ctx, 2));
    }
}

static void f49_get_chara_data_ptr_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t udm = getRegU32(ctx, 4);
    const uint32_t idx = getRegU32(ctx, 5);
    if (f49_trace_title_path_enabled() && n <= 64u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=GetCharaDataPtr.enter n=%u ra=0x%x udm=0x%x idx=%u\n",
                     n, ra, udm, idx);
    }
    GetCharaDataPtr__16CUserDataManagerFi_0x19b490(rdram, ctx, runtime);
    if (f49_trace_title_path_enabled() && n <= 64u)
    {
        std::fprintf(stderr,
                     "[F49:title] tag=GetCharaDataPtr.exit n=%u ra=0x%x ret=0x%x pc=0x%x\n",
                     n, ra, getRegU32(ctx, 2), ctx->pc);
    }
}

static void f49_set_menu_load_item_no_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t arg = getRegU32(ctx, 4);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=SetMenuLoadItemNo.enter n=%u ra=0x%x arg=%u pc=0x%x\n",
                     n, ra, arg, ctx->pc);
    }
    const auto t0 = std::chrono::steady_clock::now();
    SetMenuLoadItemNo__Fi_0x2afdb0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=SetMenuLoadItemNo.exit n=%u ra=0x%x dtMs=%lld pc=0x%x\n",
                     n, ra, static_cast<long long>(dtMs), ctx->pc);
    }
}

static void f49_menu_item_chara_data_load_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t mem = getRegU32(ctx, 4);
    const uint32_t idx = getRegU32(ctx, 5);
    const uint32_t infos = getRegU32(ctx, 6);
    const uint32_t force = getRegU32(ctx, 7);
    if (f49_trace_title_path_enabled())
    {
        std::fprintf(stderr,
                     "[F49:title] tag=MenuItemCharaDataLoad.enter n=%u ra=0x%x mem=0x%x idx=%u infos=0x%x force=%u\n",
                     n, ra, mem, idx, infos, force);
    }
    const auto t0 = std::chrono::steady_clock::now();
    MenuItemCharaDataLoad__FP9mgCMemoryiPP17MENU_BGREAD_INFO2i_0x2b90d0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();
    if (f49_trace_title_path_enabled())
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=MenuItemCharaDataLoad.exit n=%u ra=0x%x dtMs=%lld pc=0x%x ret=0x%x\n",
                     n, ra, static_cast<long long>(dtMs), ctx->pc, getRegU32(ctx, 2));
    }
}

struct F48_2CharBgState
{
    int32_t readNo;
    int32_t monsterNo;
    int32_t statusBit;
    int32_t moveX;
    int32_t movePhase;
    int32_t readPhase;
    int32_t screenWidth;
    uint32_t nowReadMainChara;
    int32_t texMoveMode;
    uint32_t menuPtr;
    uint32_t menuStep;
    int32_t menuSel;
};

static uint32_t g_f48_2_read_call = 0u;
static uint32_t g_f48_2_last_read_call = 0u;
static int32_t g_f48_2_last_read_ret = -9999;

static F48_2CharBgState f48_2_read_charbg_state(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t menuPtr = dc2_read_u32(rdram, gp + 0xFFFF94F8u); // gp-0x6B08
    F48_2CharBgState s{};
    s.readNo = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF84ECu) & 0xFFFFu); // gp-0x7B14
    s.monsterNo = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF84F0u) & 0xFFFFu); // gp-0x7B10
    s.statusBit = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C18u) & 0xFFFFu); // gp-0x63E8
    s.moveX = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF9C0Cu)); // gp-0x63F4
    s.movePhase = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF9C10u)); // gp-0x63F0
    s.readPhase = static_cast<int16_t>(dc2_read_u32(rdram, gp + 0xFFFF9C00u) & 0xFFFFu); // gp-0x6400
    s.screenWidth = static_cast<int32_t>(dc2_read_u32(rdram, gp + 0xFFFF8780u)); // gp-0x7880
    s.nowReadMainChara = dc2_read_u32(rdram, gp + 0xFFFF9C04u); // gp-0x63FC
    s.texMoveMode = static_cast<int8_t>(dc2_read_u32(rdram, gp + 0xFFFF9B71u) & 0xFFu); // gp-0x648F
    s.menuPtr = menuPtr;
    s.menuStep = menuPtr ? dc2_read_u32(rdram, menuPtr + 0x54u) : 0xFFFFFFFFu;
    s.menuSel = menuPtr ? static_cast<int16_t>(dc2_read_u32(rdram, menuPtr + 0x50u) & 0xFFFFu) : -1;
    return s;
}

static const char *f48_2_read_block_reason(const F48_2CharBgState &pre,
                                           const F48_2CharBgState &post,
                                           int32_t ret)
{
    if (ret == 2)
        return "none/read_returned_2";
    if (pre.readNo < 0)
        return "0x2bbc8c NowReadMainCharaNo<0 early_ret0";
    if ((pre.statusBit & 2) == 0)
        return "0x2bbcb4 CheckPushButton_ret0";
    if (pre.readNo == 3 && pre.monsterNo < 0)
        return "unexpected/readNo3_monsterNo<0_should_ret2";
    if (pre.nowReadMainChara == 0u)
        return "0x2bbd04 NowReadMainChara_null_ret0";
    if (ret == 1 && post.readPhase == pre.readPhase)
        return "ReadBGSync_busy_or_phase_not_ready_ret1";
    if (ret == 1)
        return "ReadBG_state_machine_not_done_ret1";
    return "ret0_or_unknown";
}

static void f48_2_log_charbg(const char *tag,
                             uint32_t n,
                             uint32_t ra,
                             const F48_2CharBgState &pre,
                             const F48_2CharBgState &post,
                             int32_t ret,
                             const char *reason,
                             int32_t readRetForKey)
{
    if (!f48_2_trace_charbg_enabled() || g_f48_2_log_count >= 128u)
        return;

    const bool changed =
        pre.readNo != post.readNo ||
        pre.statusBit != post.statusBit ||
        pre.moveX != post.moveX ||
        pre.movePhase != post.movePhase ||
        pre.readPhase != post.readPhase ||
        pre.nowReadMainChara != post.nowReadMainChara ||
        pre.menuStep != post.menuStep;

    if (g_f48_2_log_count < 24u || changed || ret != 0 || (n % 30u) == 0u)
    {
        std::fprintf(stderr,
                     "[F48.2:charbg] tag=%s n=%u frame=%u ra=0x%x menu=0x%x step=%u sel=%d readNo=%d->%d monsterNo=%d status=0x%x->0x%x moveX=%d->%d movePhase=%d->%d readPhase=%d->%d screenWidth=%d chara=0x%x->0x%x texMode=%d ret=%d readRet=%d block='%s'\n",
                     tag, n, g_f40_frame_counter, ra,
                     post.menuPtr, post.menuStep, post.menuSel,
                     pre.readNo, post.readNo, pre.monsterNo,
                     static_cast<uint32_t>(pre.statusBit), static_cast<uint32_t>(post.statusBit),
                     pre.moveX, post.moveX, pre.movePhase, post.movePhase,
                     pre.readPhase, post.readPhase, post.screenWidth,
                     pre.nowReadMainChara, post.nowReadMainChara, post.texMoveMode,
                     ret, readRetForKey, reason ? reason : "");
        ++g_f48_2_log_count;
    }
}

static void f48_2_read_main_chara_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t n = ++g_f48_2_read_call;
    const uint32_t ra = getRegU32(ctx, 31);
    const F48_2CharBgState pre = f48_2_read_charbg_state(rdram, ctx);
    ReadMainCharaBG__Fv_0x2bbc80(rdram, ctx, runtime);
    const int32_t ret = static_cast<int32_t>(getRegU32(ctx, 2));
    const F48_2CharBgState post = f48_2_read_charbg_state(rdram, ctx);
    g_f48_2_last_read_call = n;
    g_f48_2_last_read_ret = ret;
    f48_2_log_charbg("ReadMainCharaBG.exit", n, ra, pre, post, ret,
                     f48_2_read_block_reason(pre, post, ret), ret);
}

static void f48_2_key_main_chara_bg_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t readCallBefore = g_f48_2_last_read_call;
    const F48_2CharBgState pre = f48_2_read_charbg_state(rdram, ctx);
    KeyMainCharaBG__Fv_0x2bc150(rdram, ctx, runtime);
    const int32_t ret = static_cast<int32_t>(getRegU32(ctx, 2));
    const F48_2CharBgState post = f48_2_read_charbg_state(rdram, ctx);
    const int32_t readRet = (g_f48_2_last_read_call != readCallBefore) ? g_f48_2_last_read_ret : -9999;
    const char *reason = "none/key_returned_1";
    if (readRet != 2)
        reason = "0x2bc1fc ReadMainCharaBG_ret_ne_2";
    else if (post.moveX < post.screenWidth)
        reason = "0x2bc210 NowMainCharaChngTexMoveX<mgScreenWidth";
    f48_2_log_charbg("KeyMainCharaBG.exit", n, ra, pre, post, ret, reason, readRet);
}

static void f49_read_bg_sync_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static uint32_t loadMenuSpinCount = 0u;
    static const uint32_t loadMenuSpinLimit = []() -> uint32_t
    {
        const char *value = std::getenv("DC2_READBGSYNC_SPIN_MAX");
        if (value == nullptr || *value == '\0')
            return 256u;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || (end && *end != '\0') || parsed < 1ul || parsed > 1000000ul)
            return 256u;
        return static_cast<uint32_t>(parsed);
    }();
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const bool logThis = f49_trace_title_path_enabled() && (n <= 32u || (n % 120u) == 0u);
    const auto t0 = std::chrono::steady_clock::now();
    ReadBGSync__Fv_0x148e70(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    // LoadMenuData(0x2BC8D0) polls ReadBGSync at 0x2BCC10. In runtime this path
    // can spin forever on ret=0. Bound the spin and force ready at that callsite.
    if (ra == 0x002BCC10u)
    {
        if (getRegU32(ctx, 2) == 0u)
        {
            ++loadMenuSpinCount;
            if (loadMenuSpinCount >= loadMenuSpinLimit)
            {
                setReturnU32(ctx, 1u);
                if (f49_trace_title_path_enabled())
                {
                    std::fprintf(stderr,
                                 "[F49:title] tag=ReadBGSync.force ra=0x%x spins=%u limit=%u ret=1\n",
                                 ra, loadMenuSpinCount, loadMenuSpinLimit);
                }
                loadMenuSpinCount = 0u;
            }
        }
        else
        {
            loadMenuSpinCount = 0u;
        }
    }
    else
    {
        loadMenuSpinCount = 0u;
    }

    if (logThis)
    {
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:title] tag=ReadBGSync n=%u ra=0x%x ret=0x%x dtMs=%lld pc=0x%x\n",
                     n, ra, getRegU32(ctx, 2), static_cast<long long>(dtMs), ctx->pc);
    }
}

// G93: forward decl (defined after g67_title_scope). Makes the title-rock renderinfo
// naturally correct for the NATIVE transform path: focal 480 + follow-camera eye, BEFORE
// TitleDraw's mgSetRenderInfo / TitleMapDraw's GetCameraMatrix run this frame.
static void g93_title_renderinfo_fix(uint8_t *rdram, R5900Context *ctx);
static bool g67_title_scope(uint8_t *rdram, R5900Context *ctx);

static void f29_title_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    // G93: correct the title-bg projection focal + follow-camera eye BEFORE the real
    // TitleDraw builds renderinfo (mgSetRenderInfo) and TitleMapDraw reads the camera
    // (GetCameraMatrix). This is the upstream Option-3 fix that lets the transform path
    // land on-screen (replacing the DC2_G92_FORCEMTX constant-injection lever).
    g93_title_renderinfo_fix(rdram, ctx);
    f29_log_title_draw_state("TitleDraw.enter", n, rdram, ctx, runtime);
    TitleDraw__Fv_0x2a0ab0(rdram, ctx, runtime);
    f29_log_title_draw_state("TitleDraw.exit", n, rdram, ctx, runtime);
}

static void f29_title_mode_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    f29_log_title_draw_state("TitleModeDraw.enter", n, rdram, ctx, runtime);
    // G59: the direct TitleModeDraw probe calls recompiled draw bodies directly.
    // Suppress back-edge yields for this title-frame draw so nested resume PCs do
    // not leak out through the probe path and unwind main after one frame.
    static const bool g57SuppressDisabled = dc2_env_flag_enabled("DC2_G57_NO_SUPPRESS");
    if (!g57SuppressDisabled)
        g_dc2PreemptSuppressDepth.fetch_add(1, std::memory_order_relaxed);
    TitleModeDraw__Fv_0x2a1b60(rdram, ctx, runtime);
    if (!g57SuppressDisabled)
        g_dc2PreemptSuppressDepth.fetch_sub(1, std::memory_order_relaxed);
    f29_log_title_draw_state("TitleModeDraw.exit", n, rdram, ctx, runtime);
}

static void f29_primquad_drawprim_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t prim = getRegU32(ctx, 4);
    const uint32_t rect = getRegU32(ctx, 5);
    const uint32_t x = dc2_read_u32(rdram, rect + 0u);
    const uint32_t y = dc2_read_u32(rdram, rect + 4u);
    const uint32_t w = dc2_read_u32(rdram, rect + 8u);
    const uint32_t h = dc2_read_u32(rdram, rect + 12u);

    if (f29_should_log(n, 120u))
    {
        const auto &memory = runtime->memory();
        std::fprintf(stderr,
                     "[F29:PrimQuadPrim.enter] n=%u pc=0x%x ra=0x%x prim=0x%x rect=0x%x xywh=%d,%d,%d,%d f12=%g f13=%g dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                     n, ctx->pc, getRegU32(ctx, 31), prim, rect,
                     static_cast<int32_t>(x), static_cast<int32_t>(y),
                     static_cast<int32_t>(w), static_cast<int32_t>(h),
                     static_cast<double>(ctx->f[12]), static_cast<double>(ctx->f[13]),
                     static_cast<unsigned long long>(memory.dmaStartCount()),
                     static_cast<unsigned long long>(memory.gifCopyCount()),
                     static_cast<unsigned long long>(memory.gsWriteCount()),
                     static_cast<unsigned long long>(memory.vifWriteCount()));
    }

    PrimQuad__FP11mgCDrawPrimff9mgRect_i__0x21fe60(rdram, ctx, runtime);
    f29_log_counters("PrimQuadPrim.exit", n, runtime, ctx);
}

static void f29_primquad_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t tex = getRegU32(ctx, 4);
    const uint32_t rect = getRegU32(ctx, 5);
    const uint32_t alpha = getRegU32(ctx, 6);
    const uint32_t red = getRegU32(ctx, 7);
    const uint32_t green = getRegU32(ctx, 8);
    const uint32_t blue = getRegU32(ctx, 9);
    const uint32_t x = dc2_read_u32(rdram, rect + 0u);
    const uint32_t y = dc2_read_u32(rdram, rect + 4u);
    const uint32_t w = dc2_read_u32(rdram, rect + 8u);
    const uint32_t h = dc2_read_u32(rdram, rect + 12u);

    if (f29_should_log(n, 120u))
    {
        const auto &memory = runtime->memory();
        std::fprintf(stderr,
                     "[F29:PrimQuadTex.enter] n=%u pc=0x%x ra=0x%x tex=0x%x rect=0x%x xywh=%d,%d,%d,%d rgba=%u,%u,%u,%u f12=%g f13=%g dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                     n, ctx->pc, getRegU32(ctx, 31), tex, rect,
                     static_cast<int32_t>(x), static_cast<int32_t>(y),
                     static_cast<int32_t>(w), static_cast<int32_t>(h),
                     red, green, blue, alpha,
                     static_cast<double>(ctx->f[12]), static_cast<double>(ctx->f[13]),
                     static_cast<unsigned long long>(memory.dmaStartCount()),
                     static_cast<unsigned long long>(memory.gifCopyCount()),
                     static_cast<unsigned long long>(memory.gsWriteCount()),
                     static_cast<unsigned long long>(memory.vifWriteCount()));
    }

    PrimQuad__FP10mgCTextureff9mgRect_i_iiii_0x21ff30(rdram, ctx, runtime);
    f29_log_counters("PrimQuadTex.exit", n, runtime, ctx);
}

static void f33_color_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f33_trace_drawprim_enabled() && (n <= 128u || (n % 1024u) == 0u))
    {
        const uint32_t prim = getRegU32(ctx, 4);
        const uint32_t out = dc2_read_u32(rdram, prim + 0xDCu);
        const uint32_t qBits = dc2_read_u32(rdram, prim + 0xF8u);
        std::fprintf(stderr,
                     "[F33:Color.call] n=%u pc=0x%x ra=0x%x prim=0x%x out=0x%x rgba=%u,%u,%u,%u q=0x%x\n",
                     n, ctx->pc, getRegU32(ctx, 31), prim, out,
                     getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), getRegU32(ctx, 8), qBits);
    }

    Color__11mgCDrawPrimFiiii_0x134c80(rdram, ctx, runtime);
}

static void f33_texture_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    if (f33_trace_drawprim_enabled() && (n <= 128u || (n % 1024u) == 0u))
    {
        const uint32_t prim = getRegU32(ctx, 4);
        const uint32_t tex = getRegU32(ctx, 5);
        const uint32_t out = dc2_read_u32(rdram, prim + 0xDCu);
        const uint64_t tex0 = tex ? dc2_read_u64(rdram, tex + 0x38u) : 0u;
        const uint64_t tex1 = tex ? dc2_read_u64(rdram, tex + 0x40u) : 0u;
        std::fprintf(stderr,
                     "[F33:Texture.call] n=%u pc=0x%x ra=0x%x prim=0x%x tex=0x%x out=0x%x tex0=0x%016llx tex1=0x%016llx dims=%u,%u,%u,%u\n",
                     n, ctx->pc, getRegU32(ctx, 31), prim, tex, out,
                     static_cast<unsigned long long>(tex0),
                     static_cast<unsigned long long>(tex1),
                     static_cast<uint32_t>(dc2_read_u32(rdram, tex + 0u) & 0xFFFFu),
                     static_cast<uint32_t>((dc2_read_u32(rdram, tex + 0u) >> 16u) & 0xFFFFu),
                     static_cast<uint32_t>(dc2_read_u32(rdram, tex + 4u) & 0xFFFFu),
                     static_cast<uint32_t>((dc2_read_u32(rdram, tex + 4u) >> 16u) & 0xFFFFu));
    }

    Texture__11mgCDrawPrimFP10mgCTexture_0x134da0(rdram, ctx, runtime);
}

static void f29_begin_prim_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;

    if (f29_should_log(n, 120u))
    {
        const auto &memory = runtime->memory();
        std::fprintf(stderr,
                     "[F29:BeginPrim.enter] n=%u pc=0x%x ra=0x%x prim=0x%x mode=%u dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                     n, ctx->pc, getRegU32(ctx, 31), getRegU32(ctx, 4), getRegU32(ctx, 5),
                     static_cast<unsigned long long>(memory.dmaStartCount()),
                     static_cast<unsigned long long>(memory.gifCopyCount()),
                     static_cast<unsigned long long>(memory.gsWriteCount()),
                     static_cast<unsigned long long>(memory.vifWriteCount()));
    }

    Begin__11mgCDrawPrimFi_0x1344a0(rdram, ctx, runtime);
}

static void f29_end_prim_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;

    if (f29_should_log(n, 120u))
    {
        const auto &memory = runtime->memory();
        std::fprintf(stderr,
                     "[F29:EndPrim.enter] n=%u pc=0x%x ra=0x%x prim=0x%x dma=%llu gif=%llu gsw=%llu vif=%llu\n",
                     n, ctx->pc, getRegU32(ctx, 31), getRegU32(ctx, 4),
                     static_cast<unsigned long long>(memory.dmaStartCount()),
                     static_cast<unsigned long long>(memory.gifCopyCount()),
                     static_cast<unsigned long long>(memory.gsWriteCount()),
                     static_cast<unsigned long long>(memory.vifWriteCount()));
    }

    End__11mgCDrawPrimFv_0x134690(rdram, ctx, runtime);
    f29_log_counters("EndPrim.exit", n, runtime, ctx);
}

// PHASE F40: scriptable headless pad-input injector.
static uint16_t f40_btn_bit(const char *name, size_t len)
{
    struct Entry { const char *name; uint16_t bit; };
    static const Entry kMap[] = {
        {"Select",   0x0001u}, {"L3",     0x0002u}, {"R3",     0x0004u},
        {"Start",    0x0008u}, {"Up",     0x0010u}, {"Right",  0x0020u},
        {"Down",     0x0040u}, {"Left",   0x0080u}, {"L2",     0x0100u},
        {"R2",       0x0200u}, {"L1",     0x0400u}, {"R1",     0x0800u},
        {"Triangle", 0x1000u}, {"Circle", 0x2000u},
        // DC2 menu checks the R1 bit (0x0800) for confirm via MenuCheckPushButton.
        // Keep scripted Cross compatible with canonical prompts by asserting both.
        {"Cross",    0x4800u},
        {"Square",   0x8000u},
        // Global debug MenuLoop uses raw CGamePad masks directly:
        // cursor down/up = 0x4000/0x1000, value +/- = 0x2000/0x8000,
        // confirm = 0x20. These aliases avoid overloading physical button names.
        {"DebugDown",     0x4000u}, {"DebugUp",       0x1000u},
        {"DebugInc",      0x2000u}, {"DebugDec",      0x8000u},
        {"DebugConfirm",  0x0020u}, {"DebugCancel",   0x0400u},
        {"DebugPlus10",   0x0008u}, {"DebugMinus10",  0x0004u},
        {"DebugPlus100",  0x0002u}, {"DebugMinus100", 0x0001u},
    };
    for (const auto &e : kMap)
        if (std::strlen(e.name) == len && std::strncmp(name, e.name, len) == 0)
            return e.bit;
    // Raw hex mask: "0x1234" ג€” lets replay_from_rec.py emit exact recorded bits
    // without aliasing through named buttons (e.g. Cross=0x4800 vs raw 0x4000).
    if (len > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
        return static_cast<uint16_t>(std::strtoul(name, nullptr, 16));
    return 0u;
}

static const std::vector<std::pair<uint32_t, uint16_t>> &f40_get_events()
{
    static std::vector<std::pair<uint32_t, uint16_t>> events = []() {
        std::vector<std::pair<uint32_t, uint16_t>> ev;
        const char *spec = std::getenv("DC2_PAD_INPUT");
        if (!spec || !*spec)
        {
            // Keep default smoke progression deterministic in headless runs.
            spec = "30..39:R1;120..129:Cross";
        }
        const char *p = spec;
        while (*p)
        {
            while (*p == ' ' || *p == '\t') ++p;
            char *end;
            const uint32_t firstFrame = static_cast<uint32_t>(std::strtoul(p, &end, 10));
            if (end == p) break;

            uint32_t rangeStart = firstFrame;
            uint32_t rangeEnd = firstFrame;
            if (end[0] == '.' && end[1] == '.')
            {
                p = end + 2;
                rangeEnd = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                if (end == p || *end != ':') break;
                if (rangeEnd < rangeStart)
                {
                    const uint32_t tmp = rangeStart;
                    rangeStart = rangeEnd;
                    rangeEnd = tmp;
                }
                p = end + 1;
            }
            else if (*end == ':')
            {
                p = end + 1;
            }
            else
            {
                break;
            }

            uint16_t pressed = 0u;
            while (*p && *p != ';')
            {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p || *p == ';') break;
                const char *sep = p;
                while (*sep && *sep != '+' && *sep != ';' && *sep != ' ' && *sep != '\t') ++sep;
                pressed |= f40_btn_bit(p, static_cast<size_t>(sep - p));
                while (*sep == ' ' || *sep == '\t') ++sep;
                p = (*sep == '+') ? sep + 1 : sep;
            }
            if (pressed != 0u)
            {
                for (uint32_t frame = rangeStart; frame <= rangeEnd; ++frame)
                {
                    bool merged = false;
                    for (auto &e : ev)
                        if (e.first == frame) { e.second |= pressed; merged = true; break; }
                    if (!merged) ev.emplace_back(frame, pressed);

                    if (frame == 0xFFFFFFFFu)
                        break;
                }
            }
            if (*p == ';') ++p;
        }
        return ev;
    }();
    return events;
}

// G49: parse DC2_RSTICK into per-frame right-stick (rx,ry) byte pairs. Dir tokens deflect
// the right stick: RRight rx=0xFF, RLeft rx=0x00, RUp ry=0x00, RDown ry=0xFF (0x80 centre).
struct F40RStick { uint32_t frame; uint8_t rx; uint8_t ry; };
static const std::vector<F40RStick> &f40_get_rstick_events()
{
    static std::vector<F40RStick> events = []() {
        std::vector<F40RStick> ev;
        const char *spec = std::getenv("DC2_RSTICK");
        if (!spec || !*spec) return ev;
        const char *p = spec;
        while (*p)
        {
            while (*p == ' ' || *p == '\t') ++p;
            char *end;
            const uint32_t firstFrame = static_cast<uint32_t>(std::strtoul(p, &end, 10));
            if (end == p) break;
            uint32_t rangeStart = firstFrame, rangeEnd = firstFrame;
            if (end[0] == '.' && end[1] == '.')
            {
                p = end + 2;
                rangeEnd = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                if (end == p || *end != ':') break;
                if (rangeEnd < rangeStart) std::swap(rangeStart, rangeEnd);
                p = end + 1;
            }
            else if (*end == ':') { p = end + 1; }
            else break;
            uint8_t rx = 0x80u, ry = 0x80u;
            while (*p && *p != ';')
            {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p || *p == ';') break;
                const char *sep = p;
                while (*sep && *sep != '+' && *sep != ';' && *sep != ' ' && *sep != '\t') ++sep;
                const size_t len = static_cast<size_t>(sep - p);
                if (len == 6 && std::strncmp(p, "RRight", 6) == 0) rx = 0xFFu;
                else if (len == 5 && std::strncmp(p, "RLeft", 5) == 0) rx = 0x00u;
                else if (len == 3 && std::strncmp(p, "RUp", 3) == 0)  ry = 0x00u;
                else if (len == 5 && std::strncmp(p, "RDown", 5) == 0) ry = 0xFFu;
                while (*sep == ' ' || *sep == '\t') ++sep;
                p = (*sep == '+') ? sep + 1 : sep;
            }
            for (uint32_t f = rangeStart; f <= rangeEnd; ++f)
            {
                ev.push_back({f, rx, ry});
                if (f == 0xFFFFFFFFu) break;
            }
            if (*p == ';') ++p;
        }
        return ev;
    }();
    return events;
}

static uint16_t f40_scripted_press_for_frame_range(uint32_t firstFrame, uint32_t lastFrame)
{
    if (firstFrame > lastFrame)
    {
        const uint32_t tmp = firstFrame;
        firstFrame = lastFrame;
        lastFrame = tmp;
    }

    uint16_t pressed = 0u;
    for (const auto &e : f40_get_events())
        if (e.first >= firstFrame && e.first <= lastFrame) pressed |= e.second;
    return pressed;
}

static bool f40_script_input_requested()
{
    return !f40_get_events().empty();
}

// G7: true only when DC2_PAD_INPUT was set EXPLICITLY (not the headless default
// smoke script). An explicit script suppresses the live controller so deterministic
// automation stays reproducible even with a pad plugged in.
static bool f40_explicit_script()
{
    static const bool e = []() {
        const char *s = std::getenv("DC2_PAD_INPUT");
        return s != nullptr && *s != '\0';
    }();
    return e;
}

// F66: in-dungeon free-roam pad driver. The title/menu input clock (scriptFrame,
// derived from the mgEndFrame OVERRIDE call count) FREEZES once in the dungeon,
// because the dungeon loop calls mgEndFrame through a direct jal that bypasses the
// registered override hook (so f40_drive_pad stops being called and no scripted
// input reaches free-roam). This driver runs off the host PRESENT loop instead
// (which ticks every frame in all game states) with its own counter that starts at
// free-roam entry, and reads a SEPARATE env DC2_DUNGEON_PAD (same
// "start..end:Alias[+Alias];..." range syntax) so it never replays the navigation
// frames. Default empty => inert. Movement uses the LEFT ANALOG STICK like the
// title path's F66 fix (free-roam reads Analog__11CPadControl, not the D-pad).
static const std::vector<std::pair<uint32_t, uint16_t>> &f66_get_dungeon_events()
{
    static std::vector<std::pair<uint32_t, uint16_t>> events = []() {
        std::vector<std::pair<uint32_t, uint16_t>> ev;
        const char *spec = std::getenv("DC2_DUNGEON_PAD");
        if (!spec || !*spec) return ev; // inert unless explicitly requested
        const char *p = spec;
        while (*p)
        {
            while (*p == ' ' || *p == '\t') ++p;
            char *end;
            const uint32_t firstFrame = static_cast<uint32_t>(std::strtoul(p, &end, 10));
            if (end == p) break;
            uint32_t rangeStart = firstFrame, rangeEnd = firstFrame;
            if (end[0] == '.' && end[1] == '.')
            {
                p = end + 2;
                rangeEnd = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                if (end == p || *end != ':') break;
                if (rangeEnd < rangeStart) { const uint32_t t = rangeStart; rangeStart = rangeEnd; rangeEnd = t; }
                p = end + 1;
            }
            else if (*end == ':') { p = end + 1; }
            else break;
            uint16_t pressed = 0u;
            while (*p && *p != ';')
            {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p || *p == ';') break;
                const char *sep = p;
                while (*sep && *sep != '+' && *sep != ';' && *sep != ' ' && *sep != '\t') ++sep;
                pressed |= f40_btn_bit(p, static_cast<size_t>(sep - p));
                while (*sep == ' ' || *sep == '\t') ++sep;
                p = (*sep == '+') ? sep + 1 : sep;
            }
            if (pressed != 0u)
                for (uint32_t frame = rangeStart; frame <= rangeEnd; ++frame)
                {
                    bool merged = false;
                    for (auto &e : ev) if (e.first == frame) { e.second |= pressed; merged = true; break; }
                    if (!merged) ev.emplace_back(frame, pressed);
                    if (frame == 0xFFFFFFFFu) break;
                }
            if (*p == ';') ++p;
        }
        return ev;
    }();
    return events;
}

// Definition of the present-loop entry point is at GLOBAL scope below the anonymous
// namespace (it must have external linkage so ps2_runtime.cpp can call it).

static void f40_log_inject(uint32_t frame, uint16_t pressed, const char *source)
{
    static const bool trace = dc2_env_flag_enabled("DC2_TRACE_PAD_INPUT");
    if (!trace || pressed == 0u)
        return;

    std::fprintf(stderr, "[F40:inject] frame=%u pressed=0x%04x activeLow=0x%04x source=%s\n",
                 frame, static_cast<unsigned>(pressed),
                 static_cast<unsigned>(~pressed & 0xFFFFu),
                 source ? source : "unknown");
    std::fflush(stderr);
}

static void f40_drive_pad(uint32_t frame, uint32_t loopNo)
{
    static bool havePrevFrame = false;
    static uint32_t prevFrame = 0u;
    static uint16_t deferredPress = 0u;

    // F64: correlate the input scriptFrame with the top-level LoopNo (program
    // state: title/menu/costume/cutscene) so route-B pad timing can be tuned to
    // the actual screen transitions. Logs on every LoopNo change + periodically.
    // Gated DC2_TRACE_F64.
    {
        static const bool s_f64 = dc2_env_flag_enabled("DC2_TRACE_F64");
        if (s_f64)
        {
            static uint32_t s_prevLoop = 0xFFFFFFFFu;
            static uint32_t s_lastLog = 0u;
            if (loopNo != s_prevLoop || frame - s_lastLog >= 15u)
            {
                std::fprintf(stderr, "[F64:loop] frame=%u LoopNo=%u%s\n",
                             frame, loopNo, (loopNo != s_prevLoop) ? "  <== CHANGE" : "");
                s_prevLoop = loopNo;
                s_lastLog = frame;
            }
        }
    }

    if (!f40_script_input_requested()) return;

    uint32_t rangeStart = frame;
    if (havePrevFrame)
    {
        if (frame >= prevFrame)
            rangeStart = prevFrame + 1u;
        else
            rangeStart = frame;
    }

    uint16_t effectivePressed = f40_scripted_press_for_frame_range(rangeStart, frame);

    // G49: latch the scripted right-stick deflection for this scriptFrame range (last match
    // wins). Held across frames with no button press, so the model can be turned for several
    // frames to validate the per-frame costume Z clear under rotation.
    {
        uint8_t rx = 0x80u, ry = 0x80u;
        bool found = false;
        for (const auto &e : f40_get_rstick_events())
            if (e.frame >= rangeStart && e.frame <= frame) { rx = e.rx; ry = e.ry; found = true; }
        if (found) { g_f40_rx = rx; g_f40_ry = ry; }
        else { g_f40_rx = 0x80u; g_f40_ry = 0x80u; }
    }

    // Keep Cross-window confirmations from being consumed by TitleModeKey.
    if ((effectivePressed & 0x4000u) != 0u && loopNo == 1u)
    {
        deferredPress |= effectivePressed;
        effectivePressed = 0u;
    }
    else if (deferredPress != 0u && loopNo != 1u)
    {
        effectivePressed |= deferredPress;
        deferredPress = 0u;
    }

    if (effectivePressed != 0u)
    {
        g_f40_active  = true;
        g_f40_pressed = effectivePressed;
        // F66: in-dungeon free-roam moves the player via the LEFT ANALOG STICK
        // (RunScript__12CActionChara reads Analog__11CPadControl(...,4/5)), NOT the
        // digital D-pad ג€” so a scripted "Up" with the stick centred (0x80) never
        // translated the player. Deflect the left stick to match any scripted
        // direction bit (kept ALONGSIDE the digital bit so menus that read D-pad
        // still navigate; menus ignore the analog stick). scePad bits: Up 0x10,
        // Right 0x20, Down 0x40, Left 0x80. Stick byte: centre 0x80, min 0x00,
        // max 0xFF; up = -Y = 0x00, left = -X = 0x00 (raylib axisToByte convention).
        uint8_t lx = 0x80u, ly = 0x80u;
        if ((effectivePressed & 0x10u) != 0u) ly = 0x00u; // Up
        if ((effectivePressed & 0x40u) != 0u) ly = 0xFFu; // Down
        if ((effectivePressed & 0x80u) != 0u) lx = 0x00u; // Left
        if ((effectivePressed & 0x20u) != 0u) lx = 0xFFu; // Right
        ps2_stubs::setPadOverrideState(static_cast<uint16_t>(~effectivePressed & 0xFFFFu),
                                       lx, ly, 0x80u, 0x80u);
        f40_log_inject(frame, effectivePressed, "mgEndFrameScaled");
    }
    else
    {
        g_f40_active  = false;
        g_f40_pressed = 0u;
        ps2_stubs::clearPadOverrideState();
    }

    havePrevFrame = true;
    prevFrame = frame;
}

static bool f30_valid_frame(const GSFrameReg &frame)
{
    return frame.fbp != 0u && frame.fbw != 0u;
}

// PHASE F49: instrument WaitVSync pacing and clamp anomalous mgEndFrame wait
// counts on the known callsite (ra=0x142A08). The game's frame-rate scalar at
// gp-0x78A0 is expected to be 1 or 2 in title/menu paths.
static void f49_waitvsync_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    static uint32_t clamps = 0u;
    const uint32_t n = ++calls;
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t preVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const uint32_t frameRate = dc2_read_u32(rdram, gp + 0xFFFF8760u); // -0x78A0
    const uint32_t startCount = getRegU32(ctx, 4);
    const uint32_t requested = getRegU32(ctx, 5);
    uint32_t effective = requested;
    bool clamped = false;

    if (ra == 0x142A08u && effective > 2u)
    {
        effective = 2u;
        ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(effective)));
        clamped = true;
        ++clamps;
    }

    const int32_t envMax = f49_waitvsync_env_max_count();
    if (envMax >= 0 && effective > static_cast<uint32_t>(envMax))
    {
        effective = static_cast<uint32_t>(envMax);
        ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(effective)));
        clamped = true;
        ++clamps;
    }

    const auto t0 = std::chrono::steady_clock::now();
    WaitVSync__Fii_0x1412a0(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_waitvsync_enabled() && (clamped || n <= 16u || (n % 60u) == 0u))
    {
        const uint32_t postVsync = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t deltaVsync = postVsync - preVsync;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[F49:wait] n=%u ra=0x%x frameRate=%u start=%u req=%u eff=%u dv=%u pre=%u post=%u dtMs=%lld clamped=%u clamps=%u\n",
                     n, ra, frameRate, startCount, requested, effective,
                     deltaVsync, preVsync, postVsync,
                     static_cast<long long>(dtMs),
                     clamped ? 1u : 0u, clamps);
    }
}

static void f49_vsync_callback_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t tickArg = getRegU32(ctx, 4);
    const uint32_t preCount = dc2_read_u32(rdram, gp + 0xFFFF8850u);   // -0x77B0
    const uint32_t preCb = dc2_read_u32(rdram, gp + 0xFFFF885Cu);      // -0x77A4
    const uint32_t preInCb = dc2_read_u32(rdram, gp + 0xFFFF8860u);    // -0x77A0
    const uint32_t preField = dc2_read_u32(rdram, gp + 0xFFFF87B8u);   // -0x7848
    const auto t0 = std::chrono::steady_clock::now();
    VSyncCallBack__Fi_0x141200(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_vsync_callback_enabled())
    {
        const uint32_t postCount = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t postCb = dc2_read_u32(rdram, gp + 0xFFFF885Cu);    // -0x77A4
        const uint32_t postInCb = dc2_read_u32(rdram, gp + 0xFFFF8860u);  // -0x77A0
        const uint32_t postField = dc2_read_u32(rdram, gp + 0xFFFF87B8u); // -0x7848
        const auto dtUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        if (n <= 16u || (n % 120u) == 0u || dtUs > 500)
        {
            std::fprintf(stderr,
                         "[F49:vsynccb] n=%u tick=%u ra=0x%x cb=0x%x->0x%x inCb=%u->%u count=%u->%u field=%u->%u dtUs=%lld\n",
                         n, tickArg, getRegU32(ctx, 31), preCb, postCb,
                         preInCb, postInCb, preCount, postCount, preField, postField,
                         static_cast<long long>(dtUs));
        }
    }
}

static void f49_syncv_callback_set_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t reqCb = getRegU32(ctx, 4);
    const uint32_t preCb = dc2_read_u32(rdram, gp + 0xFFFF885Cu); // -0x77A4
    sceGsSyncVCallback_0x1041e8(rdram, ctx, runtime);

    if (f49_trace_syncv_set_enabled())
    {
        const uint32_t postCb = dc2_read_u32(rdram, gp + 0xFFFF885Cu); // -0x77A4
        const uint32_t retPrev = getRegU32(ctx, 2);
        if (n <= 8u || reqCb == 0u || reqCb != preCb || postCb != preCb)
        {
            std::fprintf(stderr,
                         "[F49:syncvset] n=%u req=0x%x pre=0x%x post=0x%x retPrev=0x%x ra=0x%x\n",
                         n, reqCb, preCb, postCb, retPrev, getRegU32(ctx, 31));
        }
    }
}

static void f49_scegssyncv_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t ra = getRegU32(ctx, 31);
    const uint32_t preCount = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
    const auto t0 = std::chrono::steady_clock::now();
    sceGsSyncV_0x103300(rdram, ctx, runtime);
    const auto t1 = std::chrono::steady_clock::now();

    if (f49_trace_syncv_call_enabled())
    {
        const uint32_t postCount = dc2_read_u32(rdram, gp + 0xFFFF8850u); // -0x77B0
        const uint32_t dv = postCount - preCount;
        const auto dtMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (n <= 32u || dtMs >= 30 || (n % 240u) == 0u)
        {
            std::fprintf(stderr,
                         "[F49:syncv] n=%u ra=0x%x pre=%u post=%u dv=%u dtMs=%lld ret=%u\n",
                         n, ra, preCount, postCount, dv,
                         static_cast<long long>(dtMs), getRegU32(ctx, 2));
        }
    }
}

static void f29_mgendframe_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    // PHASE F30: green clear behind env-var gate. Default OFF so real title pixels survive.
    // Set $env:DC2_FORCE_VISIBLE='1' to restore F29 fallback for regression checks.
    static const bool forceVisible = []() {
        return dc2_env_flag_enabled("DC2_FORCE_VISIBLE");
    }();
    static const bool forceDrawBufferLatch = []() {
        return dc2_env_flag_enabled("DC2_FORCE_DRAW_BUFFER_LATCH");
    }();
    static uint32_t calls = 0u;
    const uint32_t n = ++calls;

    // G141: measure-first perf instrumentation (skill 17-performance-optimization.md).
    // This probe fires once per guest frame (wraps mgEndFrame), so the wall-clock delta
    // between successive entries is the whole guest frame time (game logic + VU1 draw-list
    // build + previous frame's GS submit, all of which run BEFORE the next mgEndFrame call);
    // the time spent inside mgEndFrame itself is the packet-flush/present-adjacent tail.
    // Env-gated, aggregate-only print (DC2_PERF=1), per the lever/instrumentation doctrine.
    static const bool s_g141PerfOn = dc2_env_flag_enabled("DC2_PERF");
    static const bool s_g147PerfOn = dc2_env_flag_enabled("DC2_G147_PERF");
    static const bool s_perfOn = s_g141PerfOn || dc2_env_flag_enabled("DC2_G146_PERF") || s_g147PerfOn;
    static std::chrono::steady_clock::time_point s_lastEntry{};
    static bool s_haveLast = false;
    static double s_sumFrameMs = 0.0;
    static double s_sumEndFrameMs = 0.0;
    static uint32_t s_perfWindow = 0u;
    // G141: read the global GS-raster + VU1-run ns accumulators (defined in
    // ps2_gs_rasterizer.cpp / ps2_vu1.cpp, declared at file scope below) to build a
    // NON-overlapping per-frame split. GS raster runs synchronously inside VU1 XGKICK, so
    // VU1-proper = vu1RunNs - gsRasterNs, and "other" (uploads/DMA/present/2D not nested in
    // VU1) = frame - vu1RunNs - (GS outside VU1).
    static uint64_t s_lastGsNs = 0ull, s_lastVu1Ns = 0ull;
    static uint64_t s_lastVif1Ns = 0ull, s_lastGifNs = 0ull, s_lastImageNs = 0ull, s_lastLocalNs = 0ull;
    static uint64_t s_lastG144MidNs = 0ull, s_lastG144UploadNs = 0ull, s_lastG144FrameNs = 0ull;
    static uint64_t s_lastG144MidCount = 0ull, s_lastG144UploadCount = 0ull, s_lastG144FrameCount = 0ull;
    static uint64_t s_lastG147PacketNs = 0ull, s_lastG147PacketCount = 0ull;
    static uint64_t s_lastG147DrawNs = 0ull, s_lastG147DrawCount = 0ull;
    static uint64_t s_lastG147Tags = 0ull, s_lastG147PackedRegs = 0ull, s_lastG147ReglistRegs = 0ull, s_lastG147ImageBytes = 0ull;
    static double s_sumGsMs = 0.0, s_sumVu1Ms = 0.0;
    static double s_sumVif1Ms = 0.0, s_sumGifMs = 0.0, s_sumImageMs = 0.0, s_sumLocalMs = 0.0;
    static double s_sumG144MidMs = 0.0, s_sumG144UploadMs = 0.0, s_sumG144FrameMs = 0.0;
    static uint64_t s_sumG144MidCount = 0ull, s_sumG144UploadCount = 0ull, s_sumG144FrameCount = 0ull;
    static double s_sumG147PacketMs = 0.0, s_sumG147DrawMs = 0.0;
    static uint64_t s_sumG147PacketCount = 0ull, s_sumG147DrawCount = 0ull;
    static uint64_t s_sumG147Tags = 0ull, s_sumG147PackedRegs = 0ull, s_sumG147ReglistRegs = 0ull, s_sumG147ImageBytes = 0ull;
    const auto perfEntryTs = std::chrono::steady_clock::now();
    if (s_perfOn)
    {
        if (s_haveLast)
        {
            s_sumFrameMs += std::chrono::duration<double, std::milli>(perfEntryTs - s_lastEntry).count();
            const uint64_t gsNow = g_g141GsRasterNs.load(std::memory_order_relaxed);
            const uint64_t vuNow = g_g141Vu1RunNs.load(std::memory_order_relaxed);
            const uint64_t vifNow = g_g146Vif1Ns.load(std::memory_order_relaxed);
            const uint64_t gifNow = g_g146GifSubmitNs.load(std::memory_order_relaxed);
            const uint64_t imageNow = g_g146GsImageNs.load(std::memory_order_relaxed);
            const uint64_t localNow = g_g146GsLocalNs.load(std::memory_order_relaxed);
            const uint64_t g144MidNow = g_g146G144FlushMidNs.load(std::memory_order_relaxed);
            const uint64_t g144UploadNow = g_g146G144FlushUploadNs.load(std::memory_order_relaxed);
            const uint64_t g144FrameNow = g_g146G144FlushFrameNs.load(std::memory_order_relaxed);
            const uint64_t g144MidCountNow = g_g146G144FlushMidCount.load(std::memory_order_relaxed);
            const uint64_t g144UploadCountNow = g_g146G144FlushUploadCount.load(std::memory_order_relaxed);
            const uint64_t g144FrameCountNow = g_g146G144FlushFrameCount.load(std::memory_order_relaxed);
            const uint64_t g147PacketNow = g_g147GsGifPacketNs.load(std::memory_order_relaxed);
            const uint64_t g147PacketCountNow = g_g147GsGifPacketCount.load(std::memory_order_relaxed);
            const uint64_t g147DrawNow = g_g147DrawPrimitiveNs.load(std::memory_order_relaxed);
            const uint64_t g147DrawCountNow = g_g147DrawPrimitiveCount.load(std::memory_order_relaxed);
            const uint64_t g147TagsNow = g_g147GifTags.load(std::memory_order_relaxed);
            const uint64_t g147PackedRegsNow = g_g147PackedRegs.load(std::memory_order_relaxed);
            const uint64_t g147ReglistRegsNow = g_g147ReglistRegs.load(std::memory_order_relaxed);
            const uint64_t g147ImageBytesNow = g_g147ImageBytes.load(std::memory_order_relaxed);
            s_sumGsMs += (gsNow - s_lastGsNs) / 1.0e6;
            s_sumVu1Ms += (vuNow - s_lastVu1Ns) / 1.0e6;
            s_sumVif1Ms += (vifNow - s_lastVif1Ns) / 1.0e6;
            s_sumGifMs += (gifNow - s_lastGifNs) / 1.0e6;
            s_sumImageMs += (imageNow - s_lastImageNs) / 1.0e6;
            s_sumLocalMs += (localNow - s_lastLocalNs) / 1.0e6;
            s_sumG144MidMs += (g144MidNow - s_lastG144MidNs) / 1.0e6;
            s_sumG144UploadMs += (g144UploadNow - s_lastG144UploadNs) / 1.0e6;
            s_sumG144FrameMs += (g144FrameNow - s_lastG144FrameNs) / 1.0e6;
            s_sumG144MidCount += g144MidCountNow - s_lastG144MidCount;
            s_sumG144UploadCount += g144UploadCountNow - s_lastG144UploadCount;
            s_sumG144FrameCount += g144FrameCountNow - s_lastG144FrameCount;
            s_sumG147PacketMs += (g147PacketNow - s_lastG147PacketNs) / 1.0e6;
            s_sumG147DrawMs += (g147DrawNow - s_lastG147DrawNs) / 1.0e6;
            s_sumG147PacketCount += g147PacketCountNow - s_lastG147PacketCount;
            s_sumG147DrawCount += g147DrawCountNow - s_lastG147DrawCount;
            s_sumG147Tags += g147TagsNow - s_lastG147Tags;
            s_sumG147PackedRegs += g147PackedRegsNow - s_lastG147PackedRegs;
            s_sumG147ReglistRegs += g147ReglistRegsNow - s_lastG147ReglistRegs;
            s_sumG147ImageBytes += g147ImageBytesNow - s_lastG147ImageBytes;
            s_lastGsNs = gsNow;
            s_lastVu1Ns = vuNow;
            s_lastVif1Ns = vifNow;
            s_lastGifNs = gifNow;
            s_lastImageNs = imageNow;
            s_lastLocalNs = localNow;
            s_lastG144MidNs = g144MidNow;
            s_lastG144UploadNs = g144UploadNow;
            s_lastG144FrameNs = g144FrameNow;
            s_lastG144MidCount = g144MidCountNow;
            s_lastG144UploadCount = g144UploadCountNow;
            s_lastG144FrameCount = g144FrameCountNow;
            s_lastG147PacketNs = g147PacketNow;
            s_lastG147PacketCount = g147PacketCountNow;
            s_lastG147DrawNs = g147DrawNow;
            s_lastG147DrawCount = g147DrawCountNow;
            s_lastG147Tags = g147TagsNow;
            s_lastG147PackedRegs = g147PackedRegsNow;
            s_lastG147ReglistRegs = g147ReglistRegsNow;
            s_lastG147ImageBytes = g147ImageBytesNow;
            ++s_perfWindow;
        }
        else
        {
            s_lastGsNs = g_g141GsRasterNs.load(std::memory_order_relaxed);
            s_lastVu1Ns = g_g141Vu1RunNs.load(std::memory_order_relaxed);
            s_lastVif1Ns = g_g146Vif1Ns.load(std::memory_order_relaxed);
            s_lastGifNs = g_g146GifSubmitNs.load(std::memory_order_relaxed);
            s_lastImageNs = g_g146GsImageNs.load(std::memory_order_relaxed);
            s_lastLocalNs = g_g146GsLocalNs.load(std::memory_order_relaxed);
            s_lastG144MidNs = g_g146G144FlushMidNs.load(std::memory_order_relaxed);
            s_lastG144UploadNs = g_g146G144FlushUploadNs.load(std::memory_order_relaxed);
            s_lastG144FrameNs = g_g146G144FlushFrameNs.load(std::memory_order_relaxed);
            s_lastG144MidCount = g_g146G144FlushMidCount.load(std::memory_order_relaxed);
            s_lastG144UploadCount = g_g146G144FlushUploadCount.load(std::memory_order_relaxed);
            s_lastG144FrameCount = g_g146G144FlushFrameCount.load(std::memory_order_relaxed);
            s_lastG147PacketNs = g_g147GsGifPacketNs.load(std::memory_order_relaxed);
            s_lastG147PacketCount = g_g147GsGifPacketCount.load(std::memory_order_relaxed);
            s_lastG147DrawNs = g_g147DrawPrimitiveNs.load(std::memory_order_relaxed);
            s_lastG147DrawCount = g_g147DrawPrimitiveCount.load(std::memory_order_relaxed);
            s_lastG147Tags = g_g147GifTags.load(std::memory_order_relaxed);
            s_lastG147PackedRegs = g_g147PackedRegs.load(std::memory_order_relaxed);
            s_lastG147ReglistRegs = g_g147ReglistRegs.load(std::memory_order_relaxed);
            s_lastG147ImageBytes = g_g147ImageBytes.load(std::memory_order_relaxed);
        }
        s_lastEntry = perfEntryTs;
        s_haveLast = true;
    }

    // PHASE F30: probe mgDBuffID at $gp-0x77E8 before / after mgEndFrame call.
    const uint32_t gpReg = getRegU32(ctx, 28);
    const uint32_t mgDBuffIDAddr = gpReg + 0xFFFF8818u;
    const uint32_t beforeDB = dc2_read_u32(rdram, mgDBuffIDAddr);
    f29_log_counters("mgEndFrame.enter", n, runtime, ctx);
    f29_log_packet_state("mgEndFrame.packet.enter", n, rdram, ctx, runtime);
    const auto perfEndFrameT0 = s_perfOn ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    mgEndFrame__FP14mgCDrawManager_0x1425b0(rdram, ctx, runtime);
    // G144: drain any trailing deferred triangles (frame-level tile-binning) now that this frame's
    // whole draw stream is complete, before the runtime presents/latches VRAM. Same (guest) thread
    // as capture, so no race on the deferred list. Sequential (no pool) — safe from this context.
    // Cheap no-op unless DC2_G144_TILEBIN>1 (list empty). (g144FlushPending declared at global scope.)
    // G150 MTGS: when the GS drain runs on the worker thread, g_g144List is populated ON that worker,
    // so the trailing flush must ALSO run there (folded into the frame-barrier callback below) — never
    // on this EE thread. Skip it here when MTGS is on.
    if (!g150_mtgs_enabled())
        g144FlushPending();
    if (s_perfOn)
    {
        s_sumEndFrameMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - perfEndFrameT0).count();
        if (s_perfWindow >= 30u)
        {
            const double w = static_cast<double>(s_perfWindow);
            const double avgFrameMs = s_sumFrameMs / w;
            const double avgEndFrameMs = s_sumEndFrameMs / w;
            const double avgGsMs = s_sumGsMs / w;
            const double avgVu1Ms = s_sumVu1Ms / w;
            const double avgVu1ProperMs = avgVu1Ms - avgGsMs; // VU1 minus its nested XGKICK raster
            const double avgOtherMs = avgFrameMs - avgVu1Ms;  // everything not in vu1.run (2D/upload/present)
            if (s_g141PerfOn)
            {
                std::fprintf(stderr,
                             "[G141:perf] n=%u window=%u avgFrameMs=%.2f fps=%.2f | GSraster=%.1f (%.0f%%) VU1proper=%.1f (%.0f%%) other=%.1f (%.0f%%) | mgEndFrame=%.1f\n",
                             n, s_perfWindow, avgFrameMs, 1000.0 / std::max(0.001, avgFrameMs),
                             avgGsMs, 100.0 * avgGsMs / std::max(0.001, avgFrameMs),
                             avgVu1ProperMs, 100.0 * avgVu1ProperMs / std::max(0.001, avgFrameMs),
                             avgOtherMs, 100.0 * avgOtherMs / std::max(0.001, avgFrameMs),
                             avgEndFrameMs);
            }
            else
            {
                std::fprintf(stderr,
                             "[G146:frame] n=%u window=%u avgFrameMs=%.2f fps=%.2f | mgEndFrame=%.1f\n",
                             n, s_perfWindow, avgFrameMs,
                             1000.0 / std::max(0.001, avgFrameMs),
                             avgEndFrameMs);
            }
            std::fprintf(stderr,
                         "[G146:perf] n=%u window=%u incl: VIF1=%.1f GIFsubmit=%.1f GSimage=%.1f GSlocal=%.1f\n",
                         n, s_perfWindow,
                         s_sumVif1Ms / w,
                         s_sumGifMs / w,
                         s_sumImageMs / w,
                         s_sumLocalMs / w);
            std::fprintf(stderr,
                         "[G146:g144] n=%u window=%u flush: mid=%.1fms/%.2fx upload=%.1fms/%.2fx frame=%.1fms/%.2fx\n",
                         n, s_perfWindow,
                         s_sumG144MidMs / w,
                         static_cast<double>(s_sumG144MidCount) / w,
                         s_sumG144UploadMs / w,
                         static_cast<double>(s_sumG144UploadCount) / w,
                         s_sumG144FrameMs / w,
                         static_cast<double>(s_sumG144FrameCount) / w);
            if (s_g147PerfOn)
            {
                const double packetMs = s_sumG147PacketMs / w;
                const double drawMs = s_sumG147DrawMs / w;
                const double imageMs = s_sumImageMs / w;
                const double parserOtherMs = packetMs - drawMs - imageMs;
                std::fprintf(stderr,
                             "[G147:gif] n=%u window=%u packet=%.1fms/%.2fx draw=%.1fms/%.2fx parserOther=%.1fms tags=%.1f packedRegs=%.1f reglistRegs=%.1f imageKB=%.1f\n",
                             n, s_perfWindow,
                             packetMs,
                             static_cast<double>(s_sumG147PacketCount) / w,
                             drawMs,
                             static_cast<double>(s_sumG147DrawCount) / w,
                             parserOtherMs,
                             static_cast<double>(s_sumG147Tags) / w,
                             static_cast<double>(s_sumG147PackedRegs) / w,
                             static_cast<double>(s_sumG147ReglistRegs) / w,
                             static_cast<double>(s_sumG147ImageBytes) / (w * 1024.0));
            }
            s_sumFrameMs = 0.0;
            s_sumEndFrameMs = 0.0;
            s_sumGsMs = 0.0;
            s_sumVu1Ms = 0.0;
            s_sumVif1Ms = 0.0;
            s_sumGifMs = 0.0;
            s_sumImageMs = 0.0;
            s_sumLocalMs = 0.0;
            s_sumG144MidMs = 0.0;
            s_sumG144UploadMs = 0.0;
            s_sumG144FrameMs = 0.0;
            s_sumG144MidCount = 0ull;
            s_sumG144UploadCount = 0ull;
            s_sumG144FrameCount = 0ull;
            s_sumG147PacketMs = 0.0;
            s_sumG147DrawMs = 0.0;
            s_sumG147PacketCount = 0ull;
            s_sumG147DrawCount = 0ull;
            s_sumG147Tags = 0ull;
            s_sumG147PackedRegs = 0ull;
            s_sumG147ReglistRegs = 0ull;
            s_sumG147ImageBytes = 0ull;
            s_perfWindow = 0u;
        }
    }
    const uint32_t afterDB = dc2_read_u32(rdram, mgDBuffIDAddr);
    if (f29_should_log(n))
    {
        std::fprintf(stderr,
                     "[F30:mgDBuffID] n=%u gp=0x%x addr=0x%x before=%u after=%u\n",
                     n, gpReg, mgDBuffIDAddr, beforeDB, afterDB);
    }
    if (forceVisible)
    {
        const bool clear0 = runtime->gs().clearFramebufferContext(0u, 0xFF00FF00u);
        const bool clear1 = runtime->gs().clearFramebufferContext(1u, 0xFF00FF00u);
        runtime->gs().latchHostPresentationFrame();
        if (f29_should_log(n))
        {
            std::fprintf(stderr,
                         "[F29:mgEndFrame.forceVisible] n=%u clear0=%u clear1=%u\n",
                         n, clear0 ? 1u : 0u, clear1 ? 1u : 0u);
        }
    }
    else
    {
        if (forceDrawBufferLatch)
        {
            // Legacy F30 presentation fallback: follow a live draw context
            // instead of the guest-selected display buffer.
            static GSFrameReg lastValidFrame{};
            static bool haveLastValidFrame = false;
            const auto &cf0 = runtime->gs().getContextFrame(0);
            const auto &cf1 = runtime->gs().getContextFrame(1);
            const bool cf0Valid = f30_valid_frame(cf0);
            const bool cf1Valid = f30_valid_frame(cf1);
            const bool currentValid = cf0Valid || cf1Valid;
            bool usedSticky = false;
            GSFrameReg chosen{};
            if (cf0Valid)
            {
                chosen = cf0;
            }
            else if (cf1Valid)
            {
                chosen = cf1;
            }
            else if (haveLastValidFrame)
            {
                chosen = lastValidFrame;
                usedSticky = true;
            }
            if (currentValid)
            {
                lastValidFrame = chosen;
                haveLastValidFrame = true;
            }
            if (f30_valid_frame(chosen))
            {
                const uint64_t baseEnc =
                    static_cast<uint64_t>(chosen.fbp & 0x1ffu)
                    | (static_cast<uint64_t>(chosen.fbw & 0x3fu) << 9)
                    | (static_cast<uint64_t>(chosen.psm & 0x3fu) << 15);
                runtime->gs().writeRegister(0x59u, baseEnc | 0x80000000000ull);
                runtime->gs().writeRegister(0x5bu, baseEnc);
            }
            if (f29_should_log(n))
            {
                std::fprintf(stderr,
                             "[F30:mgEndFrame.forceDrawLatch] n=%u currentValid=%u usedSticky=%u selected.fbp=0x%x fbw=%u psm=%u mgDBuffID=%u\n",
                             n, currentValid ? 1u : 0u, usedSticky ? 1u : 0u,
                             chosen.fbp, chosen.fbw, chosen.psm, afterDB);
            }
        }
        // G150 MTGS: g150_frame_barrier first blocks until the GS worker has finished this frame's
        // draws (frameDrain), then runs this closure on THIS (EE) thread: drain the trailing tile-bin
        // tris (safe — worker idle, sole toucher of g_g144List) and latch the present frame, reading
        // live display registers + vsync tick over complete VRAM exactly as the synchronous baseline.
        // When MTGS is off, the barrier runs the closure inline and the flush is skipped here (already
        // done on the EE thread above) → byte-identical to the pre-G150 path.
        g150_frame_barrier([runtime] {
            if (g150_mtgs_enabled())
                g144FlushPending();
            runtime->gs().latchHostPresentationFrame();
        });
        if (f29_should_log(n))
        {
            std::fprintf(stderr,
                         "[F52:mgEndFrame.guestLatch] n=%u forcedDrawLatch=%u mgDBuffID=%u\n",
                         n, forceDrawBufferLatch ? 1u : 0u, afterDB);
        }
    }
    f29_log_packet_state("mgEndFrame.packet.exit", n, rdram, ctx, runtime);
    f29_log_counters("mgEndFrame.exit", n, runtime, ctx);
    // Map sparse mgEndFrame probe hits onto a denser virtual frame clock that
    // matches the canonical scripted input windows used by F46/F49 prompts.
    uint32_t scriptFrame = static_cast<uint32_t>((static_cast<uint64_t>(n) * 31ull) / 14ull);
    if (scriptFrame > 16u)
        scriptFrame -= 16u;
    else
        scriptFrame = 0u;
    const uint32_t loopNo = dc2_read_u32(rdram, gpReg + 0xFFFF8ADCu);
    const uint32_t nextLoopNo = dc2_read_u32(rdram, gpReg + 0xFFFF8AE0u);
    f47_log_menu_state(rdram, ctx, n, scriptFrame, loopNo, nextLoopNo);
    f57_log_frame_loop(rdram, n, loopNo, nextLoopNo);
    f59_log_frame(rdram, n, loopNo);
    g_f40_frame_counter = scriptFrame;
    f40_drive_pad(scriptFrame, loopNo);
}

static void scelogofade_done_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    setReturnU32(ctx, 1u);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F5: DC2 ג€” GetFullPath corrected shim.
// Real convention: a0=input_filename, a1=output_buf (the auto-stub in dc2_auto_stubs.cpp
// had the args reversed and never prepended a drive prefix, so fioOpen got "").
// Bare filenames get "cdrom0:\" prepended; paths that already contain ':' pass through.
static void getfullpath_shim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t srcAddr  = getRegU32(ctx, 4); // $a0 = input filename
    const uint32_t destAddr = getRegU32(ctx, 5); // $a1 = output buffer

    const char *src = reinterpret_cast<const char *>(getConstMemPtr(rdram, srcAddr));
    char *dest      = reinterpret_cast<char *>(getMemPtr(rdram, destAddr));

    if (src && dest)
    {
        if (::strchr(src, ':'))
            ::snprintf(dest, 256, "%s", src);
        else
            ::snprintf(dest, 256, "cdrom0:\\%s", src);
    }

    setReturnU32(ctx, destAddr);
    ctx->pc = getRegU32(ctx, 31);
}

// PHASE F5: DC2 ג€” _Exit halt. The auto-stub returned 0 (nop), so Exit(-1) looped
// forever calling TerminateLibrary ג†’ InitTLB ג†’ _Exit ג†’ repeat. Terminate cleanly.
static void exit_halt_stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (dc2_env_flag_enabled("DC2_TRACE_EXIT"))
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t titleInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF996Cu) : 0u;
        const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
        const uint32_t nextLoopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8AE0u) : 0u;
        std::fprintf(stderr,
                     "[G59:exit] status=%d pc=0x%x ra=0x%x sp=0x%x a0=0x%x loop=%u next=%u titleState=%u titleSub=%u\n",
                     static_cast<int32_t>(getRegU32(ctx, 4)), ctx->pc, getRegU32(ctx, 31),
                     getRegU32(ctx, 29), getRegU32(ctx, 4), loopNo, nextLoopNo,
                     titleInfo ? dc2_read_u32(rdram, titleInfo + 0x00u) : 0u,
                     titleInfo ? dc2_read_u32(rdram, titleInfo + 0x04u) : 0u);
        std::fflush(stderr);
    }
    std::fprintf(stderr, "[dc2] _Exit called ג€” halting\n");
    std::exit(0);
}

static uint32_t dc2_pad_mask(PS2Runtime *runtime, int port, int slot)
{
    // G7: a live host controller (when connected and no explicit script) OWNS the
    // button mask, taking priority over the scripted F40 schedule. The analog sticks
    // flow separately through dc2_write_pad_status from the same snapshot.
    if (g_pad_live_connected)
        return (port == 0) ? static_cast<uint32_t>(g_pad_live_mask) : 0u;

    // PHASE F40: scripted reads consume only the mgEndFrame-driven state.
    if (f40_script_input_requested())
        return (port == 0 && g_f40_active) ? static_cast<uint32_t>(g_f40_pressed) : 0u;

    uint8_t buf[32] = {0};
    if (runtime && runtime->padBackend().readState(port, slot, buf, sizeof(buf)))
    {
        const uint16_t activeLow = static_cast<uint16_t>(buf[2] | (buf[3] << 8));
        const uint32_t live = static_cast<uint32_t>(static_cast<uint16_t>(~activeLow));
        if (live != 0u)
            return live;
    }

    if (port == 0)
    {
        static uint32_t call_count = 0u;
        const uint32_t cycle = call_count++ % 240u;
        if ((cycle >= 30u && cycle < 45u) || (cycle >= 120u && cycle < 135u))
            return 0x0800u;
    }

    return 0u;
}

static void dc2_write_pad_status(uint8_t *rdram, uint32_t statusAddr, uint32_t mask)
{
    if (statusAddr == 0u)
        return;

    uint8_t *dst = getMemPtr(rdram, statusAddr);
    if (!dst)
        return;

    dst[0] = static_cast<uint8_t>(mask & 0xFFu);
    dst[1] = static_cast<uint8_t>((mask >> 8) & 0xFFu);
    dst[2] = 0u;
    dst[3] = 0u;
    // PAD_STATUS stick ints at +4/+8/+0xc/+0x10 = LY,LX,RY,RX (each 0x80 centre).
    // F66: in-dungeon free-roam reads the LEFT analog stick for player movement, so
    // deflect it from the present-loop dungeon driver when active.
    uint8_t outLX = 0x80u, outLY = 0x80u, outRX = 0x80u, outRY = 0x80u;
    if (g_pad_live_connected)
    {
        // G7: live controller owns all four axes (both sticks).
        outLX = g_pad_live_lx; outLY = g_pad_live_ly;
        outRX = g_pad_live_rx; outRY = g_pad_live_ry;
    }
    else if (g_f66_dungeon_active)
    {
        outLX = g_f66_dungeon_lx; outLY = g_f66_dungeon_ly;
        outRX = g_f40_rx; outRY = g_f40_ry;
    }
    else
    {
        // G49: scripted right-stick deflection (DC2_RSTICK) for headless costume-rotation
        // validation; default 0x80/0x80 (centred) so normal scripted runs are unaffected.
        outRX = g_f40_rx; outRY = g_f40_ry;
    }
    const uint8_t sticks[4] = { outLY, outLX, outRY, outRX };
    for (int i = 0; i < 4; ++i)
    {
        dst[4 + i * 4 + 0] = sticks[i];
        dst[4 + i * 4 + 1] = 0u;
        dst[4 + i * 4 + 2] = 0u;
        dst[4 + i * 4 + 3] = 0u;
    }

    dst[20] = 99u; dst[21] = 0u; dst[22] = 0u; dst[23] = 0u;
    dst[24] = 6u;  dst[25] = 0u; dst[26] = 0u; dst[27] = 0u;
    dst[28] = 7u;  dst[29] = 0u; dst[30] = 0u; dst[31] = 0u;
    dst[32] = 7u;  dst[33] = 0u; dst[34] = 0u; dst[35] = 0u;
    dst[36] = 7u;  dst[37] = 0u; dst[38] = 0u; dst[39] = 0u;
}

static uint32_t dc2_read_u32(uint8_t *rdram, uint32_t addr)
{
    const uint8_t *src = getConstMemPtr(rdram, addr);
    if (!src)
        return 0u;
    return static_cast<uint32_t>(src[0]) |
        (static_cast<uint32_t>(src[1]) << 8) |
        (static_cast<uint32_t>(src[2]) << 16) |
        (static_cast<uint32_t>(src[3]) << 24);
}

static uint64_t dc2_read_u64(uint8_t *rdram, uint32_t addr)
{
    return static_cast<uint64_t>(dc2_read_u32(rdram, addr)) |
        (static_cast<uint64_t>(dc2_read_u32(rdram, addr + 4u)) << 32u);
}

static void dc2_write_u32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    uint8_t *dst = getMemPtr(rdram, addr);
    if (!dst)
        return;
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

static void dc2_write_u16(uint8_t *rdram, uint32_t addr, uint16_t value)
{
    uint8_t *dst = getMemPtr(rdram, addr);
    if (!dst)
        return;
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

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

static uint16_t g67_read_u16(uint8_t *rdram, uint32_t addr)
{
    const uint8_t *src = getConstMemPtr(rdram, addr);
    if (!src)
        return 0u;
    return static_cast<uint16_t>(src[0] | (static_cast<uint16_t>(src[1]) << 8u));
}

// G89: g67_title_scope sets g_dc2TitleRockScope (declared at global scope near the top of this
// file, defined in ps2_gs_rasterizer.cpp) each frame so the rasterizer's title-rock guard-band
// cull only fires in the title-map scene.
static bool g67_title_scope(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t titleMap = dc2_read_u32(rdram, 0x00377E34u);
    const bool inTitle = loopNo == 3u && titleMap == 0x008FC880u;
    g_dc2TitleRockScope.store(inTitle, std::memory_order_relaxed);
    // G121: A/B the per-object transform-vs-drawing selector field desc+0x2c, runner vs HW.
    // HW (live BP 0x1404d0) has 4 objects with desc+0x2c==0 (drawing/copy route): descs
    // 0x979710/0x9826f0/0x984990/0x9870d0; the 25 rock parts (e.g. 0x906b40) have +0x2c==1
    // (transform). Descriptors are same-address per G116. Read +0x2c for those fixed addrs
    // plus signature fields (+0x00=0xffffffff, +0x20=100.0, +0x70=128.0) to confirm the address
    // holds the SAME object on the runner. If the runner's +0x2c matches HW (0 for the 4,
    // 1 for rock) -> routing matches, the bug is downstream (copy walls on-screen but invisible).
    // If +0x2c differs -> the routing IS the bug -> trace the writer. Env DC2_G121_DESC; one-shot
    // (after title scope is live a few frames so the descriptors are populated); quiet by default.
    if (inTitle && std::getenv("DC2_G121_DESC") != nullptr)
    {
        static std::atomic<uint32_t> s_g121frames{0};
        static std::atomic<bool> s_g121done{false};
        const uint32_t f = s_g121frames.fetch_add(1u, std::memory_order_relaxed);
        if (f >= 30u && !s_g121done.exchange(true))
        {
            static const uint32_t addrs[5] = {0x979710u, 0x9826f0u, 0x984990u, 0x9870d0u, 0x906b40u};
            for (int i = 0; i < 5; ++i)
            {
                const uint32_t a = addrs[i];
                const uint32_t s0  = dc2_read_u32(rdram, a + 0x00u);
                const uint32_t s20 = dc2_read_u32(rdram, a + 0x20u);
                const uint32_t d2c = dc2_read_u32(rdram, a + 0x2cu);
                const uint32_t s70 = dc2_read_u32(rdram, a + 0x70u);
                const uint32_t d84 = dc2_read_u32(rdram, a + 0x84u);
                const bool sig = (s0 == 0xffffffffu && s20 == 0x42c80000u && s70 == 0x43000000u);
                std::fprintf(stderr,
                    "[G121:desc] addr=0x%x +0x2c=0x%x bit2=%d sigOK=%d (+0x00=0x%x +0x20=0x%x +0x70=0x%x +0x84=0x%x) HWexpect_bit2=%d\n",
                    a, d2c, (d2c != 0u) ? 1 : 0, sig ? 1 : 0, s0, s20, s70, d84, (i < 4) ? 0 : 1);
            }
            std::fflush(stderr);
        }
    }
    return inTitle;
}

// =====================================================================================
// PHASE G93 ג€” make the title-rock NATIVE TRANSFORM path render on-screen by fixing the
// renderinfo camera/projection at its SOURCE (Option 3, future-proof), replacing the
// G92 DC2_G92_FORCEMTX constant-injection lever.
//
// ROOT (G92C + G93 live A/B): the per-frame mgRENDER_INFO@0x380ec0 that the transform
// packers consume encodes the WRONG title camera:
//   (1) projection focal = 800 (state-2 TitleProjection) vs HW 480 (state-1);
//   (2) the follow-camera VIEW (renderinfo+0x1a0) has eye~origin (translate (0,0,6.25))
//       vs HW eye 661 back, because GetCameraMatrix__9mgCCamera@0x1314d0 builds the view
//       purely from camera pos(+0x00) -> ref(+0x10) (GetDir), and at TitleMapDraw time the
//       runner's follow camera pos is STALE (the G81 fix writes the camera fields only
//       LATER, inside the mgClipInBoxW geometry-flush hook, and writes a fixed ctor pos
//       (1.2,2.3,550) unsuitable for a follow camera). HW recomputes pos each frame from
//       ref + dist + height + angle.
//   (3) renderinfo lightInfo idx(+0x3f4) = 2 vs HW 0 (lighting variant).
//
// FIX (runs at TitleDraw entry, BEFORE mgSetRenderInfo + TitleMapDraw's GetCameraMatrix):
//   - TitleProjection(0x377E50) = 480 so SetRenderInfo builds the 480 projection.
//   - renderinfo idx(+0x3f4) = 0.
//   - recompute the follow-camera eye from its LIVE ref/dist/height/angle (the HW follow
//     formula eye = ref + (sin(a)*dist, height, cos(a)*dist)) and write camera pos(+0x00),
//     so the real GetCameraMatrix builds the HW view. Derived from live params (NOT a
//     constant matrix) -> future-proof and self-correcting as the camera zooms.
// Title-scoped (g67_title_scope: loopNo==3 && TitleMap==0x8FC880) => costume/dungeon safe.
// Opt-in proof lever only: DC2_G93_FIX=1. Sub-levers DC2_G93_NO_CAM / DC2_G93_NO_FOCAL for A/B.
// =====================================================================================
static void g93_title_renderinfo_fix(uint8_t *rdram, R5900Context *ctx)
{
    // OPT-IN diagnostic lever only (DC2_G93_FIX=1). NOT default-on: it writes GLOBAL state
    // (TitleProjection / renderinfo idx / the title camera object) in the front-end scope,
    // which bleeds into the CONCURRENT costume select (the G79 title<->costume concurrency)
    // and mislocates the Max model. The real fix is the front-end STATE (see G93 fix-log),
    // not this global write. Kept as the proof lever for the renderinfo-camera diagnosis.
    static const bool enabled = dc2_env_flag_enabled("DC2_G93_FIX");
    if (!enabled)
        return;
    if (!g67_title_scope(rdram, ctx))
        return;

    auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
    auto wf = [&](uint32_t a, float v) { uint32_t u; std::memcpy(&u, &v, 4); dc2_write_u32(rdram, a, u); };

    // (1) focal 480 (TitleProjection global) + (3) lightInfo idx 0.
    static const bool noFocal = dc2_env_flag_enabled("DC2_G93_NO_FOCAL");
    if (!noFocal)
    {
        wf(0x00377E50u, 480.0f);              // TitleProjection (state-1 focal)
        dc2_write_u32(rdram, 0x00380EC0u + 0x3F4u, 0u); // renderinfo lightInfo idx
    }

    // (2) follow-camera eye recompute.
    static const bool noCam = dc2_env_flag_enabled("DC2_G93_NO_CAM");
    if (!noCam)
    {
        const uint32_t cam = dc2_read_u32(rdram, 0x00377E38u); // TitleCamera
        if (cam != 0u)
        {
            const float dist = rf(cam + 0x90u);
            // Only once G81 has initialised the title framing (ctor dist=40); skip the
            // fresh ctor camera so we never write garbage from the default ref/origin.
            if (dist > 700.0f)
            {
                const float refx = rf(cam + 0x10u);
                const float refy = rf(cam + 0x14u);
                const float refz = rf(cam + 0x18u);
                const float height = rf(cam + 0x94u);
                const float angle = rf(cam + 0x9Cu); // current (interpolated) follow angle
                const float s = std::sin(angle);
                const float c = std::cos(angle);
                wf(cam + 0x00u, refx + s * dist); // eye.x
                wf(cam + 0x04u, refy + height);   // eye.y
                wf(cam + 0x08u, refz + c * dist); // eye.z
                wf(cam + 0x0Cu, 1.0f);
                if (std::getenv("DC2_G93_CAM") != nullptr)
                {
                    std::fprintf(stderr,
                        "[G93:cam] cam=0x%x dist=%.1f height=%.1f angle=%.4f eye=(%.1f,%.1f,%.1f) ref=(%.1f,%.1f,%.1f)\n",
                        cam, dist, height, angle, refx + s * dist, refy + height, refz + c * dist,
                        refx, refy, refz);
                    std::fflush(stderr);
                }
            }
        }
    }
}

static bool g67_title_tbp(uint32_t tbp)
{
    switch (tbp)
    {
    case 0x2720u: case 0x2760u: case 0x2820u: case 0x2860u:
    case 0x28A0u: case 0x28C0u: case 0x2B20u: case 0x2F20u:
    case 0x3320u: case 0x3720u: case 0x3820u: case 0x3920u:
    case 0x3960u:
        return true;
    default:
        return false;
    }
}

static bool g67_record_tex_hit(G67TexHit *hits, uint32_t *count, uint32_t addr, uint64_t tex0)
{
    uint32_t tbp = 0u, psm = 0u, cbp = 0u;
    f51_decode_tex0(tex0, &tbp, nullptr, &psm, &cbp, nullptr);
    if (!g67_title_tbp(tbp))
        return false;
    if (psm == 0u)
    {
        if (cbp != 0u && cbp < 0x3FC0u)
            return false;
    }
    else if (psm == 0x13u)
    {
        if (cbp < 0x3FC0u)
            return false;
    }
    else
    {
        return false;
    }

    bool hit3960 = (tbp == 0x3960u);
    for (uint32_t i = 0u; i < *count; ++i)
    {
        if (hits[i].addr == addr && hits[i].tex0 == tex0)
            return hit3960;
    }
    if (*count < 6u)
    {
        G67TexHit &h = hits[*count];
        h.addr = addr;
        h.tbp = tbp;
        h.psm = psm;
        h.cbp = cbp;
        h.tex0 = tex0;
        ++(*count);
    }
    return hit3960;
}

static bool g67_scan_tex0_range_limited(uint8_t *rdram, uint32_t base, uint32_t bytes,
                                        uint32_t limit, G67TexHit *hits, uint32_t *count)
{
    const uint32_t phys = base & 0x1FFFFFFFu;
    if (!g43_guest_ram_addr(phys))
        return false;
    const uint32_t capped = (bytes < limit) ? bytes : limit;
    bool hit3960 = false;
    for (uint32_t off = 0u; off + 8u <= capped; off += 8u)
        hit3960 |= g67_record_tex_hit(hits, count, phys + off, dc2_read_u64(rdram, phys + off));
    return hit3960;
}

static bool g67_scan_tex0_range(uint8_t *rdram, uint32_t base, uint32_t bytes,
                                G67TexHit *hits, uint32_t *count)
{
    return g67_scan_tex0_range_limited(rdram, base, bytes, 0x200u, hits, count);
}

static bool g67_scan_packet_refs(uint8_t *rdram, uint32_t packet, uint32_t qwc,
                                 G67TexHit *hits, uint32_t *count)
{
    const uint32_t phys = packet & 0x1FFFFFFFu;
    if (!g43_guest_ram_addr(phys))
        return false;
    const uint32_t packetBytes = ((qwc != 0u && qwc < 0x40u) ? qwc : 0x20u) * 16u;
    bool hit3960 = g67_scan_tex0_range(rdram, phys, packetBytes, hits, count);

    const uint32_t qwords = packetBytes / 16u;
    for (uint32_t q = 0u; q < qwords; ++q)
    {
        const uint32_t tag = dc2_read_u32(rdram, phys + q * 16u);
        const uint32_t ref = dc2_read_u32(rdram, phys + q * 16u + 4u) & 0x1FFFFFFFu;
        const uint32_t tagClass = tag & 0xF0000000u;
        const uint32_t refQwc = tag & 0xFFFFu;
        if (refQwc == 0u || refQwc > 0x80u)
            continue;
        if (tagClass != 0x10000000u && tagClass != 0x30000000u && tagClass != 0x50000000u)
            continue;
        if (!g43_guest_ram_addr(ref))
            continue;
        hit3960 |= g67_scan_tex0_range(rdram, ref, refQwc * 16u, hits, count);
    }
    return hit3960;
}

static void g67_append_tex_hits(char *buf, size_t cap, int *p, const G67TexHit *hits, uint32_t count)
{
    if (*p >= static_cast<int>(cap))
        return;
    *p += std::snprintf(buf + *p, cap - static_cast<size_t>(*p), " hits=");
    for (uint32_t i = 0u; i < count && *p < static_cast<int>(cap) - 80; ++i)
    {
        *p += std::snprintf(buf + *p, cap - static_cast<size_t>(*p),
            "%s0x%x:tbp=0x%x/psm=0x%x/cbp=0x%x",
            (i == 0u) ? "" : ",", hits[i].addr, hits[i].tbp, hits[i].psm, hits[i].cbp);
    }
}

static uint32_t g67_mgr_count(uint8_t *rdram, uint32_t mgr, uint32_t block)
{
    const uint32_t counts = mgr ? dc2_read_u32(rdram, mgr + 0x18u) : 0u;
    if (!counts || block >= 64u)
        return 0u;
    return dc2_read_u32(rdram, counts + block * 4u);
}

static uint32_t g67_mgr_max_block(uint8_t *rdram, uint32_t mgr)
{
    const uint32_t bounds = mgr ? dc2_read_u32(rdram, mgr + 0x58u) : 0u;
    const uint32_t count = bounds ? dc2_read_u32(rdram, bounds + 0x0Cu) : 0u;
    return count ? count : 64u;
}

static int32_t g67_resolve_mgr_block(uint8_t *rdram, uint32_t mgr, int32_t block)
{
    if (!mgr || block < 0)
        return -1;
    const uint32_t ublock = static_cast<uint32_t>(block);
    const uint32_t maxBlock = g67_mgr_max_block(rdram, mgr);
    if (ublock >= maxBlock)
        return -2;
    if (dc2_read_u32(rdram, mgr + 0x00u) != 0u)
    {
        const uint32_t remap = dc2_read_u32(rdram, mgr + 0x04u);
        if (!remap)
            return -3;
        return static_cast<int32_t>(dc2_read_u32(rdram, remap + ublock * 4u));
    }
    return block;
}

static uint32_t g67_mgr_head(uint8_t *rdram, uint32_t mgr, int32_t block)
{
    if (!mgr || block < 0)
        return 0u;
    const uint32_t heads = dc2_read_u32(rdram, mgr + 0x10u);
    const uint32_t ublock = static_cast<uint32_t>(block);
    if (!heads || ublock >= 64u)
        return 0u;
    return dc2_read_u32(rdram, heads + ublock * 4u);
}

static void g67_append_block_list(char *line, size_t cap, int *p,
                                  uint8_t *rdram, uint32_t buf, int32_t count, int32_t max)
{
    if (*p < 0 || static_cast<size_t>(*p) >= cap || !buf || count <= 0)
        return;
    const int32_t n = (count < max) ? count : max;
    *p += std::snprintf(line + *p, cap - static_cast<size_t>(*p), " out=");
    for (int32_t i = 0; i < n && *p < static_cast<int>(cap) - 24; ++i)
    {
        const int32_t v = static_cast<int32_t>(dc2_read_u32(rdram, buf + static_cast<uint32_t>(i) * 4u));
        *p += std::snprintf(line + *p, cap - static_cast<size_t>(*p),
                            "%s%d", (i == 0) ? "" : ",", v);
    }
}

static void g67_append_mgr_block_summary(char *line, size_t cap, int *p,
                                         uint8_t *rdram, uint32_t mgr, int32_t reqBlock)
{
    if (*p < 0 || static_cast<size_t>(*p) >= cap)
        return;
    const int32_t resolved = g67_resolve_mgr_block(rdram, mgr, reqBlock);
    const uint32_t count = (resolved >= 0) ? g67_mgr_count(rdram, mgr, static_cast<uint32_t>(resolved)) : 0u;
    const uint32_t array = (resolved >= 0) ? g67_mgr_head(rdram, mgr, resolved) : 0u;
    *p += std::snprintf(line + *p, cap - static_cast<size_t>(*p),
                        " req=%d->%d count=%u array=0x%x", reqBlock, resolved, count, array);
    for (uint32_t i = 0u; i < 4u && g43_guest_ram_addr(array) && i < count && *p < static_cast<int>(cap) - 180; ++i)
    {
        const uint32_t slot = count - 1u - i;
        const uint32_t node = dc2_read_u32(rdram, array + slot * 4u);
        if (!g43_guest_ram_addr(node))
            break;
        const uint32_t state = dc2_read_u32(rdram, node + 0x00u);
        const uint32_t packet = dc2_read_u32(rdram, node + 0x04u);
        const int32_t nodeBlock = static_cast<int16_t>(g67_read_u16(rdram, node + 0x0Cu));
        const uint32_t vprog = g67_read_u16(rdram, node + 0x0Eu);
        G67TexHit hits[6]{};
        uint32_t hitCount = 0u;
        const bool hit3960 = g67_scan_tex0_range_limited(rdram, state, 0x1000u, 0x1000u, hits, &hitCount);
        *p += std::snprintf(line + *p, cap - static_cast<size_t>(*p),
                            " | q%u[%u]=0x%x nb=%d vp=0x%x st=0x%x pk=0x%x%s",
                            i, slot, node, nodeBlock, vprog, state, packet, hit3960 ? "/3960" : "");
    }
}

static void g67_frame_test1_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled();
    const uint32_t ra = getRegU32(ctx, 31);
    const bool fromFrameDraw = (ra == 0x00137EC8u);
    const uint32_t outBox = getRegU32(ctx, 7);

    test1__FPA4_fPA4_fPA4_fPfPf_0x135c70(rdram, ctx, runtime);

    if (!fromFrameDraw)
        return;
    G67FrameScope *scope = g67_top_frame_scope();
    if (!scope)
        return;
    scope->test1Seen = 1u;
    scope->test1OriginalDist = g67_read_f32(rdram, outBox + 0x0Cu);
    scope->test1Dist = scope->test1OriginalDist;
    scope->test1Limit = g67_read_f32(rdram, 0x00381D48u);
    if (scope->distFix && scope->test1Dist < scope->test1Limit)
    {
        dc2_write_u32(rdram, outBox + 0x0Cu, g67_f32_bits(scope->test1Limit));
        scope->test1Dist = scope->test1Limit;
        scope->test1Fixed = 1u;
    }

    if (!trace)
        return;

    const uint32_t n = g_g67_test1.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool focus = g67_focus_frame(scope->frame, scope->f8);
    if (focus || n <= 80u || (n % 240u) == 0u)
    {
        std::fprintf(stderr,
                     "[G67:test1] n=%u frame=0x%x visual=0x%x out=0x%x dist=% .6g orig=% .6g "
                     "limit=% .6g fixed=%u culled=%u\n",
                     n, scope->frame, scope->f8, outBox, scope->test1Dist,
                     scope->test1OriginalDist, scope->test1Limit, scope->test1Fixed,
                     (!scope->test1Fixed && scope->test1Dist < scope->test1Limit) ? 1u : 0u);
        std::fflush(stderr);
    }
}

static void g67_frame_clipbox_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled();
    const uint32_t ra = getRegU32(ctx, 31);
    const bool fromFrameDraw = (ra == 0x00137EF4u);
    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);
    const uint32_t a2 = getRegU32(ctx, 6);
    const uint32_t a3 = getRegU32(ctx, 7);

    mgClipBoxW__FPfPfPfPf_0x12f2e0(rdram, ctx, runtime);

    if (!fromFrameDraw)
        return;
    G67FrameScope *scope = g67_top_frame_scope();
    uint32_t ret = getRegU32(ctx, 2);
    uint32_t fixed = 0u;
    if (scope && scope->distFix && ret == 0u)
    {
        setReturnU32(ctx, 1u);
        ret = 1u;
        fixed = 1u;
    }
    if (scope)
    {
        scope->clipSeen = 1u;
        scope->clipFixed = fixed;
        scope->clipRet = ret;
        scope->clipA0 = a0;
        scope->clipA1 = a1;
        scope->clipA2 = a2;
        scope->clipA3 = a3;
    }

    if (!trace)
        return;

    const uint32_t n = g_g67_clip.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool focus = scope && g67_focus_frame(scope->frame, scope->f8);
    if (focus || ret == 0u || n <= 80u || (n % 240u) == 0u)
    {
        std::fprintf(stderr,
                     "[G67:clip] n=%u frame=0x%x visual=0x%x ra=0x%x ret=%u fixed=%u "
                     "a=(0x%x,0x%x,0x%x,0x%x) min0=(% .6g,% .6g,% .6g,% .6g) max0=(% .6g,% .6g,% .6g,% .6g)\n",
                     n, scope ? scope->frame : 0u, scope ? scope->f8 : 0u, ra, ret, fixed,
                     a0, a1, a2, a3,
                     g67_read_f32(rdram, a0 + 0x00u), g67_read_f32(rdram, a0 + 0x04u),
                     g67_read_f32(rdram, a0 + 0x08u), g67_read_f32(rdram, a0 + 0x0Cu),
                     g67_read_f32(rdram, a1 + 0x00u), g67_read_f32(rdram, a1 + 0x04u),
                     g67_read_f32(rdram, a1 + 0x08u), g67_read_f32(rdram, a1 + 0x0Cu));
        std::fflush(stderr);
    }
}

// G78: mgClipInBoxW@0x12f380 is the SECOND clip test inside Draw__8mgCFrame (jal @0x137f04,
// ra=0x137f0c -> beq $v0,zero,0x137f20). Its return selects the frame's render-mode:
//   ret != 0  -> 0x137f14: fc0=0, fc4=0  => COPY render-mode (pre-transformed/screen-space)
//   ret == 0  -> 0x137f20: fc0=1 + fc4 set => TRANSFORM render-mode (re-projects + cull)
// CreateRenderInfoPacket builds VU qword 38 (the copy-vs-transform selector) from these flags:
// bit0=(fc0||fc4), bit1=fc4. The VU dispatcher routes a mesh to the copy packer 0x1b68 iff
// sel&5==0 (bit0 & bit2 clear). HW renders the title rock via copy meshes (selector 0x10/0x30,
// fc0=fc4=0; verified live on PCSX2 at CreateRenderInfoPacket@0x1404d0). The runner instead
// gets fc4!=0 for the rock meshes (selector 0x33 -> 0x17) because mgClipInBoxW returns 0 for
// the title rock frame, so every rock mesh routes to the +2048/ADC transform packers, whose
// per-vertex gate (IBEQ VI10(=208),VIxגˆˆ{0,1}) is structurally never taken => ADC=1 on every
// kicking vertex => no primitive ever draws => flat blue (p0x1b68=0). This mirrors the G67
// clip-box fix (which already forces the FIRST clip 0x12f2e0 for the same frame): for the title
// rock frame ONLY (scope->distFix, frame 0x981390/visual 0x987230), force the result to 1 so
// fc4=0 => copy render-mode => the rock is built + drawn WITH its texture (tbp=0x3960) and
// per-vertex ADC (unlike the G75 VU reroute, which forced transform-mode data through copy and
// rendered green/faceted). Kill-switch DC2_G78_NO_COPY_FIX. Trace DC2_TRACE_G67 -> [G78:clipinbox].
static std::atomic<uint32_t> g_g78_clipinbox{0};

// G79: costume->title round-trip leaves the title 3D bg flat-blue + a leaked model at the left.
// Probe whether the costume DRAW (MenuCostumeDraw@0x2be040) is still dispatched after exit (=
// front-end stuck on costume menu id 0x17) or the costume model object just persists/redraws
// independently. Logs the active menu id (MenuCommonInfo+0x54) + MenuActionChara[0]. DC2_TRACE_G79.
static std::atomic<uint32_t> g_g79_costdraw{0};
static std::atomic<uint32_t> g_g79_maindraw{0};
static bool g79_trace_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_TRACE_G79");
    return e;
}
static bool g79_leak_fix_enabled()
{
    // G92 Phase A (2026-06-26): DEFAULT-OFF (was default-on with kill DC2_G79_NO_LEAK_FIX).
    // The suppression gate below (loop==3 && titleMode==2 && menuId==0x17) was ASSUMED unique to
    // the costume->title round-trip leak, but a headless A/B (run_g34_costume / DC2_G9_COSTUME)
    // proved the LEGIT Select-Costume screen hits the exact same state (menuId=0x17 loop=3
    // titleMode=2, [G79:costdraw]) -> the fix wrongly skipped the real costume draw -> BLACK
    // screen (charDraw=0). Since the round-trip fix only ever removed a secondary MODEL leak (its
    // main symptom, the flat-blue bg, stayed DEFERRED) and it broke the PRIMARY costume baseline,
    // recover the baseline by defaulting OFF. Opt back in with DC2_G79_LEAK_FIX=1 for round-trip
    // experiments. The round-trip leak still needs a PROPER discriminator (the three gate globals
    // are identical between legit and leak; a costume Step-ran / exit-flag probe is required).
    static const bool e = dc2_env_flag_enabled("DC2_G79_LEAK_FIX");
    return e;
}
static void g79_costume_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t mcInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF94F8u) : 0u; // MenuCommonInfo (gp-0x6B08)
    const uint32_t menuId = mcInfo ? dc2_read_u32(rdram, mcInfo + 0x54u) : 0xFFFFFFFFu;
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0xFFFFFFFFu;
    const uint32_t titleInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF997Cu) : 0u;
    const uint32_t tMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
    const uint32_t mac0 = dc2_read_u32(rdram, 0x01F0CAA0u); // MenuActionChara[0]
    const uint32_t loadPhase = dc2_read_u32(rdram, 0x00378114u) & 0xFFFFu; // MenuCosutumeLoadPhase
    const uint32_t n = g_g79_costdraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
    // G79 FIX: the costume->title round-trip leaves the MenuMain state machine STUCK at the
    // costume menu id (0x17) while TitleLoop is back on its own New Game menu (loop=3,
    // titleMode=2). The two run CONCURRENTLY: the stuck costume draw leaks the Max model (at the
    // left) AND its RTT/GS state corrupts the title 3D background (flat blue). This inconsistent
    // state (loop=3 && titleMode==2 && menuId==0x17) never happens during a legitimate costume
    // select (there TitleLoop is NOT showing its menu), so suppressing the costume draw here
    // restores the title without affecting real costume select. Kill-switch DC2_G79_NO_LEAK_FIX.
    const bool inconsistent = (loopNo == 3u && tMode == 2u && menuId == 0x17u);
    if (g79_trace_enabled() && (n <= 20u || (n % 120u) == 0u))
        std::fprintf(stderr, "[G79:costdraw] n=%u menuId=0x%x loop=%u titleMode=%u mac0=0x%x loadPhase=%u suppress=%d\n",
                     n, menuId, loopNo, tMode, mac0, loadPhase, (int)(g79_leak_fix_enabled() && inconsistent));
    if (g79_leak_fix_enabled() && inconsistent)
        return; // skip the stuck costume draw -> title 3D bg + menu render cleanly
    MenuCostumeDraw__Fv_0x2be040(rdram, ctx, runtime);
}
static void g79_main_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t mcInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF94F8u) : 0u;
    const uint32_t menuId = mcInfo ? dc2_read_u32(rdram, mcInfo + 0x54u) : 0xFFFFFFFFu;
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0xFFFFFFFFu; // LoopNo (3=front-end)
    const uint32_t titleInfo = gp ? dc2_read_u32(rdram, gp + 0xFFFF997Cu) : 0u; // titleInfo (gp-0x6684)
    const uint32_t tMode = titleInfo ? dc2_read_u32(rdram, titleInfo + 0u) : 0xFFFFFFFFu;
    const uint32_t tSub = titleInfo ? dc2_read_u32(rdram, titleInfo + 4u) : 0xFFFFFFFFu;
    const uint32_t newGameSel = dc2_read_u32(rdram, 0x01ECD62Cu); // DAT_01ecd62c (0xc=New Game)
    const uint32_t n = g_g79_maindraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g79_trace_enabled() && (n <= 8u || (n % 60u) == 0u))
        std::fprintf(stderr, "[G79:maindraw] n=%u menuId=0x%x loop=%u titleMode=%u titleSub=%u newGameSel=0x%x\n",
                     n, menuId, loopNo, tMode, tSub, newGameSel);
    MenuMainDraw__Fv_0x234290(rdram, ctx, runtime);
}

static bool g78_copy_fix_enabled()
{
    static const bool enabled = !dc2_env_flag_enabled("DC2_G78_NO_COPY_FIX");
    return enabled;
}

// G80: the title rock cavern is logical texture block 0 (s19_01..s19_11a, VRAM 0x2720..0x3960,
// confirmed live on PCSX2 by walking texMgr@0x381ef0 block-0 chain) spread across MULTIPLE
// map-part visuals (0x92fb60/0x95b5a0/0x95eb20/0x9643b0, [G67:mdt] counts{0:7}). G78 only forced
// COPY render-mode for the block-1 (s19_05) visual 0x987230 -- the WRONG visual -- so the actual
// rock (block 0) stays TRANSFORM-routed -> the +2048/ADC gate culls every vertex -> flat/cyan
// ([G66:tbp] block-0 textures 0x2b20/0x2f20/0x3960 all nodraw). When enabled, extend the G78
// fc4=copy fix to EVERY title-map frame (g67_title_scope) instead of the single G78 target, so
// all block-0 rock meshes route to the copy packer 0x1b68 and sample their own reloaded textures.
// Default-ON (kill DC2_G80_NO_ROCK_COPY); title-scoped via g67_title_scope (loopNo==3 &&
// titleMap==0x8FC880) so it cannot touch the costume/menu (costume-safe by construction).
// Validated: brown rock formation + logo render (was flat cyan), golden smoke non-black, no
// costume regression. Residual (lower walls s19_02/04/06 still cull + camera) -> next phase.
// G100: master switch for the native forced-draw title-rock path (default-ON, kill
// DC2_G100_NO_FORCEDRAW). When ON, the title rock renders through the fixed transform packers
// (ps2_vu1.cpp::g100ForceTitleDraw) + the G89 guard band, matching the HW per-tbp on-screen
// distribution (0x2720 32%, walls 0x2b20/0x2f20/0x3320, no phantom pages). When reverted, the
// pre-G100 G78/G80 force-COPY path is restored.
static bool g100_native_title_enabled()
{
    static const bool enabled = !dc2_env_flag_enabled("DC2_G100_NO_FORCEDRAW");
    return enabled;
}

// G80 (RETIRED by G100 for the title): the force-COPY band-aid routed the block-0 rock to the
// copy packer 0x1b68. With the native forced-draw transform path active it would DOUBLE-DRAW
// 0x2720 (measured ~5x the HW vertex count, 11% on-screen) and bind the phantom pages
// 0x3820/0x3920 that HW never draws -> over-bright + blue triangular holes. So copy is OFF
// whenever G100 is enabled. Reverts with DC2_G100_NO_FORCEDRAW; DC2_G80_NO_ROCK_COPY still kills
// it explicitly on the reverted path.
static bool g80_rock_copy_enabled()
{
    static const bool enabled =
        !g100_native_title_enabled() && !dc2_env_flag_enabled("DC2_G80_NO_ROCK_COPY");
    return enabled;
}

// G81: the title camera is stuck at mgCCameraFollow CONSTRUCTOR defaults (distance=40, height=30,
// from __ct__15mgCCameraFollow(40,30,0,8) at TitleInit@0x2a0e..) because TitleModeInit@0x2a1020's
// camera-setup block (SetPos/SetRef/SetDistance(1296)/SetHeight(-214)/SetAngle(0)/FollowOn) ran
// while the TitleCamera global (0x00377E38) was 0 (the known headless title-camera init-ordering
// gap, cf. G58). With ctor defaults the camera looks at the origin from distance 40 instead of
// framing the rock cavern (HW ref (-70.1,280.8,-493.0) at distance ~1296->800 zoom-in), so the
// rock renders tiny/misplaced and the green map background fills the screen.
// FIX (default-ON, kill DC2_G81_NO_CAM_FIX): when title-scoped and the camera still holds the ctor
// defaults, replicate TitleModeInit's camera block by writing the fields directly + restart the
// phase-0 zoom-in. One-shot (after the fix distance=1296 and the phase-0 AddDistance(-0.25) drives
// it to 800, so the dist==40 guard never re-fires). HW A/B (PCSX2 New Game menu): phase 0,
// dist 1198.75, height -214, pos(-70.1,66.8,707.7), ref(-70.1,280.8,-493.0); init constants
// pos=data@0x354300=(1.2,2.3,550), ref=data@0x354310=(-70.1,280.8,-493).
static bool g81_cam_fix_enabled()
{
    static const bool enabled = !dc2_env_flag_enabled("DC2_G81_NO_CAM_FIX");
    return enabled;
}

struct G81TitleCameraRepairResult
{
    bool applied;
    uint32_t cam;
    float oldDist;
    float oldHeight;
};

static void g99_snap_title_follow_eye(uint8_t *rdram, uint32_t cam)
{
    auto wf = [&](uint32_t a, float v) {
        uint32_t u;
        std::memcpy(&u, &v, sizeof(u));
        dc2_write_u32(rdram, a, u);
    };
    const float refx = g67_read_f32(rdram, cam + 0x10u);
    const float refy = g67_read_f32(rdram, cam + 0x14u);
    const float refz = g67_read_f32(rdram, cam + 0x18u);
    const float dist = g67_read_f32(rdram, cam + 0x90u);
    const float height = g67_read_f32(rdram, cam + 0x94u);
    const float angle = g67_read_f32(rdram, cam + 0x9cu);
    const float eyeX = refx + std::sin(angle) * dist;
    const float eyeY = refy + height;
    const float eyeZ = refz + std::cos(angle) * dist;

    wf(cam + 0x00u, eyeX);
    wf(cam + 0x04u, eyeY);
    wf(cam + 0x08u, eyeZ);
    wf(cam + 0x0cu, 1.0f);
    wf(cam + 0x20u, eyeX);
    wf(cam + 0x24u, eyeY);
    wf(cam + 0x28u, eyeZ);
    wf(cam + 0x2cu, 1.0f);

    wf(cam + 0xb0u, refx);
    wf(cam + 0xb4u, refy);
    wf(cam + 0xb8u, refz);
    wf(cam + 0xbcu, 0.0f);
}

static G81TitleCameraRepairResult g81_apply_title_camera_setup(uint8_t *rdram)
{
    const uint32_t cam = dc2_read_u32(rdram, 0x00377E38u); // TitleCamera
    if (cam == 0u)
        return {false, 0u, 0.0f, 0.0f};
    const float dist = g67_read_f32(rdram, cam + 0x90u);
    const float height = g67_read_f32(rdram, cam + 0x94u);
    // Only the freshly-constructed, never-title-inited camera (ctor defaults 40.0 / 30.0).
    if (!(dist > 39.5f && dist < 40.5f && height > 29.5f && height < 30.5f))
        return {false, cam, dist, height};
    // pos = data@0x354300 (SetPos/SetNextPos); ref = data@0x354310 (SetRef = vtable+0x20).
    // CRITICAL: the follow camera (FollowOn) interpolates pos->nextpos(+0x20) and ref->nextref(+0x30,
    // the follow TARGET). Set BOTH current and "next" or the follow drifts ref to the origin (the
    // uninitialised target) and the camera looks away from the rock. HW has nextref(+0x30) and the
    // ref-copy(+0x70) all = the SetRef target (-70,281,-493).
    for (uint32_t i = 0; i < 4u; ++i)
    {
        const uint32_t pw = dc2_read_u32(rdram, 0x00354300u + i * 4u); // pos vector
        const uint32_t rw = dc2_read_u32(rdram, 0x00354310u + i * 4u); // ref/target vector
        dc2_write_u32(rdram, cam + 0x00u + i * 4u, pw); // pos
        dc2_write_u32(rdram, cam + 0x20u + i * 4u, pw); // nextpos
        dc2_write_u32(rdram, cam + 0x10u + i * 4u, rw); // ref
        dc2_write_u32(rdram, cam + 0x30u + i * 4u, rw); // nextref (follow target)
        dc2_write_u32(rdram, cam + 0x70u + i * 4u, rw); // ref-copy (matches HW)
    }
    float initDist = 1296.0f; // SetDistance(0x44a28000)
    if (const char *d = std::getenv("DC2_G81_DIST")) { float v = (float)atof(d); if (v > 1.0f) initDist = v; }
    dc2_write_u32(rdram, cam + 0x90u, g67_f32_bits(initDist));
    dc2_write_u32(rdram, cam + 0x94u, g67_f32_bits(-214.0f)); // SetHeight(0xc3560000)
    dc2_write_u32(rdram, cam + 0x98u, g67_f32_bits(0.0f));    // SetAngle(0)
    dc2_write_u32(rdram, cam + 0x9cu, g67_f32_bits(0.0f));    // current angle
    dc2_write_u32(rdram, cam + 0xA0u, 1u);   // FollowOn
    // TitleModeInit immediately steps the follow camera with a negative step, snapping
    // current eye to ref + distance/height. The repair must do the same or the title
    // view matrix yaws while interpolating from the static ctor/init position.
    g99_snap_title_follow_eye(rdram, cam);
    dc2_write_u32(rdram, 0x00377E44u, 0u);   // TitleCameraPhase = 0 (restart the zoom-in)
    return {true, cam, dist, height};
}

static void g81_fix_title_camera(uint8_t *rdram, R5900Context *ctx)
{
    if (!g81_cam_fix_enabled())
        return;
    if (!g67_title_scope(rdram, ctx))
        return;
    const G81TitleCameraRepairResult result = g81_apply_title_camera_setup(rdram);
    if (std::getenv("DC2_G81_CAM") != nullptr)
    {
        if (result.applied)
            std::fprintf(stderr, "[G81:camfix] applied cam=0x%x dist 40->1296 height 30->-214 ref(-70,281,-493) snapFollowEye=1\n", result.cam);
        std::fflush(stderr);
    }
}

// G94: state-1 title init repair. The real TitleModeInit writes TitleProjection=480 in
// a branch-delay slot, but the runner reaches steady state 1 with projection 800 after
// an init/order bounce. Apply the state-1 init postcondition once at state entry, before
// TitleDraw consumes it. This is deliberately not a per-draw renderinfo/global write.
static bool g94_title_state1_init_fix_enabled()
{
    static const bool enabled = !dc2_env_flag_enabled("DC2_G94_NO_INIT_FIX");
    return enabled;
}

static bool g94_trace_enabled()
{
    static const bool enabled = dc2_env_flag_enabled("DC2_TRACE_G94");
    return enabled;
}

static bool g94_frontend_title_state(uint8_t *rdram, R5900Context *ctx, uint32_t *titleInfo, uint32_t *state, uint32_t *sub)
{
    const uint32_t gp = getRegU32(ctx, 28);
    const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0u;
    const uint32_t info = gp ? dc2_read_u32(rdram, gp + 0xFFFF997Cu) : 0u;
    if (titleInfo) *titleInfo = info;
    if (state) *state = info ? dc2_read_u32(rdram, info + 0x00u) : 0xFFFFFFFFu;
    if (sub) *sub = info ? dc2_read_u32(rdram, info + 0x04u) : 0xFFFFFFFFu;
    return loopNo == 3u && info != 0u;
}

static void g94_apply_title_state1_init(uint8_t *rdram, R5900Context *ctx, const char *site)
{
    if (!g94_title_state1_init_fix_enabled())
        return;

    uint32_t titleInfo = 0u;
    uint32_t state = 0xFFFFFFFFu;
    uint32_t sub = 0xFFFFFFFFu;
    if (!g94_frontend_title_state(rdram, ctx, &titleInfo, &state, &sub))
        return;
    if (state != 1u && sub != 1u)
        return;

    const uint32_t beforeProj = dc2_read_u32(rdram, 0x00377E50u);
    dc2_write_u32(rdram, 0x00377E50u, g67_f32_bits(480.0f));
    const G81TitleCameraRepairResult cam = g81_apply_title_camera_setup(rdram);

    if (g94_trace_enabled())
    {
        const uint32_t afterProj = dc2_read_u32(rdram, 0x00377E50u);
        const float dist = cam.cam ? g67_read_f32(rdram, cam.cam + 0x90u) : 0.0f;
        const float height = cam.cam ? g67_read_f32(rdram, cam.cam + 0x94u) : 0.0f;
        std::fprintf(stderr,
                     "[G94:init] site=%s titleInfo=0x%x state=%u sub=%u proj=0x%08x->0x%08x cam=0x%x camApplied=%u dist=%.2f height=%.2f oldDist=%.2f oldHeight=%.2f pc=0x%x ra=0x%x\n",
                     site, titleInfo, state, sub, beforeProj, afterProj, cam.cam,
                     cam.applied ? 1u : 0u, dist, height, cam.oldDist, cam.oldHeight,
                     ctx->pc, getRegU32(ctx, 31));
        std::fflush(stderr);
    }
}

static void g94_title_mode_init_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t entryRa = getRegU32(ctx, 31);
    TitleModeInit__Fv_0x2a1020(rdram, ctx, runtime);
    if (ctx->pc == getRegU32(ctx, 31) || ctx->pc == entryRa)
        g94_apply_title_state1_init(rdram, ctx, "TitleModeInit");
}

static void g94_observe_title_state1_entry(uint8_t *rdram, R5900Context *ctx, const char *site)
{
    static uint32_t s_lastTitleInfo = 0u;
    static uint32_t s_lastState = 0xFFFFFFFFu;
    static bool s_state1Active = false;

    uint32_t titleInfo = 0u;
    uint32_t state = 0xFFFFFFFFu;
    uint32_t sub = 0xFFFFFFFFu;
    const bool inFrontEnd = g94_frontend_title_state(rdram, ctx, &titleInfo, &state, &sub);
    if (!inFrontEnd)
    {
        s_lastTitleInfo = titleInfo;
        s_lastState = state;
        s_state1Active = false;
        return;
    }

    const bool enteringState1 =
        state == 1u &&
        (!s_state1Active || s_lastTitleInfo != titleInfo || s_lastState != 1u);
    if (enteringState1)
    {
        g94_apply_title_state1_init(rdram, ctx, site);
        s_state1Active = true;
    }
    else if (state != 1u)
    {
        s_state1Active = false;
    }

    s_lastTitleInfo = titleInfo;
    s_lastState = state;
}

static void g78_frame_clipinbox_fix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = getRegU32(ctx, 31);
    const bool fromFrameDraw = (ra == 0x00137F0Cu);
    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);
    const uint32_t a2 = getRegU32(ctx, 6);
    const uint32_t a3 = getRegU32(ctx, 7);

    mgClipInBoxW__FPfPfPfPf_0x12f380(rdram, ctx, runtime);

    // G81: re-apply the title camera setup if it is stuck at ctor defaults (self-guarded, one-shot).
    g81_fix_title_camera(rdram, ctx);

    // G88: one-shot dump of the mgRenderInfo VIEWPORT/guard block (0x380ec0 + float idx 0x3a8..0x3c7)
    // to A/B the projection SCALE vs HW (HW vp[0x3b8]=(256,208), guard ֲ±2047). The title rock streaks
    // = geometry fanned to the guard band ֲ±2047 => the runner's NDC->screen scale or perspective
    // divide diverges from HW even though lightInfo (+0x400) is byte-identical. DC2_G88_VP, title scope.
    if (std::getenv("DC2_G88_VP") != nullptr && g67_title_scope(rdram, ctx))
    {
        static bool s_done = false;
        if (!s_done)
        {
            s_done = true;
            auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
            const uint32_t base = 0x00380ec0u;
            std::fprintf(stderr, "[G88:vp] idx@+0x3f4=%u\n", dc2_read_u32(rdram, base + 0x3f4u));
            for (uint32_t i = 0x3a8u; i <= 0x3c7u; i += 4u)
                std::fprintf(stderr, "[G88:vp]  vp[0x%x]: %12.4f %12.4f %12.4f %12.4f\n",
                    i, rf(base + i * 4u), rf(base + (i + 1) * 4u), rf(base + (i + 2) * 4u), rf(base + (i + 3) * 4u));
            std::fflush(stderr);
        }
    }

    // G92 Phase C: one-shot dump of the title transform projection matrix CHAIN in mgRENDER_INFO
    // (base 0x380ec0) so we can A/B it BYTE-for-byte against the live HW reference captured from
    // PCSX2 at the New Game title (phase-G92C-fix-log.md). These matrices are built by
    // SetRenderInfo__13mgRENDER_INFO@0x138b00 (real recompiled body) and consumed by
    // CreateRenderInfoPacket__12mgCVisualMDT@0x1404d0 to build the VU MVP = (View*Proj @ +0x10) * LW.
    // If any Y row diverges from HW => the EE matrix math (sceVu0* helpers / FPU) is the bug; if they
    // match => the divergence is in the per-draw LW combine / VU apply (transform packet). Byte offsets:
    //   +0x10  View*Proj combined (the MVP-no-world the else-branch multiplies by LW)
    //   +0x110 projection (focal 480)   +0x150 view*aspect   +0x1a0 view   +0x260 viewport scale
    // DC2_G92_MTX, title scope.
    if (std::getenv("DC2_G92_MTX") != nullptr && g67_title_scope(rdram, ctx))
    {
        // G93: dump at a LATER (steady-state) frame, not frame-1 (the title camera is
        // still at ctor defaults on frame 1). Fire once around the 400th title-clip call.
        static std::atomic<uint32_t> s_mtxN{0};
        const uint32_t mn = s_mtxN.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const char *mf = std::getenv("DC2_G92_MTX_FRAME");
        const uint32_t target = mf ? (uint32_t)std::atoi(mf) : 400u;
        static bool s_done = false;
        if (!s_done && mn >= target)
        {
            s_done = true;
            const uint32_t base = 0x00380ec0u;
            auto dumpMat = [&](uint32_t off, const char *label) {
                std::fprintf(stderr, "[G92:mtx] --- %s (base+0x%x) ---\n", label, off);
                for (uint32_t r = 0u; r < 4u; ++r)
                {
                    float f[4];
                    for (uint32_t c = 0u; c < 4u; ++c)
                    {
                        uint32_t u = dc2_read_u32(rdram, base + off + (r * 4u + c) * 4u);
                        std::memcpy(&f[c], &u, 4);
                    }
                    std::fprintf(stderr, "[G92:mtx]   %12.4f %12.4f %12.4f %12.4f\n", f[0], f[1], f[2], f[3]);
                }
            };
            std::fprintf(stderr, "[G92:mtx] fcc=0x%x fcc+0x8c=%.4f fc4=0x%x fc0=0x%x idx@+0x3f4=%u\n",
                         dc2_read_u32(rdram, base + 0xfccu),
                         [&]{ uint32_t u = dc2_read_u32(rdram, dc2_read_u32(rdram, base + 0xfccu) + 0x8cu); float f; std::memcpy(&f,&u,4); return f; }(),
                         dc2_read_u32(rdram, base + 0xfc4u), dc2_read_u32(rdram, base + 0xfc0u),
                         dc2_read_u32(rdram, base + 0x3f4u));
            dumpMat(0x10u,  "View*Proj combined +0x10");
            dumpMat(0x110u, "projection +0x110");
            dumpMat(0x150u, "view*aspect +0x150");
            dumpMat(0x1a0u, "view +0x1a0");
            dumpMat(0x260u, "viewport scale +0x260");
            std::fflush(stderr);
        }
    }

    // G92 Phase C VALIDATION lever (NOT a shipped fix): overwrite the title renderinfo projection/
    // view matrix chain with the live HW reference captured from PCSX2 at the New Game menu. Runs in
    // the title draw chain (mgClipInBoxW, after mgSetRenderInfo, before CreateRenderInfoPacket reads
    // renderinfo) so the transform packets are built with the HW camera. If the transform path's
    // off-screen (7.4%) coverage jumps toward HW (~48%) under this lever, it PROVES the off-screen
    // projection is the wrong renderinfo camera (runner focal 800 + camera at origin) vs HW
    // (focal 480 + camera 661 back), not a viewport/VU-apply bug. DC2_G92_FORCEMTX, title scope.
    if (std::getenv("DC2_G92_FORCEMTX") != nullptr && g67_title_scope(rdram, ctx))
    {
        const uint32_t base = 0x00380ec0u;
        auto wf = [&](uint32_t off, float v) { uint32_t u; std::memcpy(&u, &v, 4); dc2_write_u32(rdram, base + off, u); };
        auto wmat = [&](uint32_t off, const float m[16]) { for (uint32_t i = 0u; i < 16u; ++i) wf(off + i * 4u, m[i]); };
        static const float mvp[16] = {  // +0x10 View*Proj combined (HW)
            480.0f, 0.0f, 0.0f, 0.0f,
            366.2069f, -184.7677f, -298.6458f, 0.1788f,
            -2014.9930f, -2115.1277f, 1643.2494f, -0.9839f,
            1388447.375f, 1460146.625f, 49000152.0f, 661.5232f };
        static const float proj[16] = { // +0x110 projection (HW, focal 480)
            480.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 480.0f, 0.0f, 0.0f,
            2048.0f, 2048.0f, -1670.1670f, 1.0f,
            0.0f, 0.0f, 50105008.0f, 0.0f };
        static const float vasp[16] = { // +0x150 view*aspect (HW)
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, -1.1479f, 0.1788f, 0.0f,
            0.0f, -0.2086f, -0.9839f, 0.0f,
            70.1f, 219.4734f, 661.5232f, 1.0f };
        static const float view[16] = { // +0x1a0 view (HW, camera 661 back, pitch 10.3deg)
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, -0.9839f, 0.1788f, 0.0f,
            0.0f, -0.1788f, -0.9839f, 0.0f,
            70.1f, 188.1201f, 661.5232f, 1.0f };
        wmat(0x10u, mvp);
        wmat(0x110u, proj);
        wmat(0x150u, vasp);
        wmat(0x1a0u, view);
    }

    // G90: one-shot dump of the title texMgr (0x381ef0) block->texture chains for the drawn
    // blocks (0,1,3,6,7) so we can A/B block->VRAM tbp + texture sizes vs HW (tools/pcsx2tex.py).
    // HW cursor[0]=0x2720; each block reloads contiguously from there by texture size. The runner
    // misses HW's 0x2b20/0x2f20/0x3320 (the 1024-block big rock walls) -> cursor/size math diverges.
    if (std::getenv("DC2_G90_TEXMGR") != nullptr && g67_title_scope(rdram, ctx))
    {
        static bool s_done = false;
        if (!s_done)
        {
            s_done = true;
            const uint32_t TM = 0x00381ef0u;
            const uint32_t cursor = dc2_read_u32(rdram, TM + 0x00u);
            const uint32_t clut = dc2_read_u32(rdram, TM + 0x04u);
            const uint32_t last = dc2_read_u32(rdram, TM + 0x08u);
            const uint32_t cnt = dc2_read_u32(rdram, TM + 0x0Cu);
            const uint32_t arr = dc2_read_u32(rdram, TM + 0x10u);
            std::fprintf(stderr, "[G90:texmgr] cursor=0x%x clut=0x%x last=%d count=%u arr=0x%x\n",
                         cursor, clut, (int32_t)last, cnt, arr);
            const int blocks[5] = {0, 1, 3, 6, 7};
            for (int bi = 0; bi < 5; ++bi)
            {
                const int blk = blocks[bi];
                const uint32_t rec = arr + (uint32_t)blk * 0x10u;
                uint32_t t = dc2_read_u32(rdram, rec + 8u);
                std::fprintf(stderr, "[G90:texmgr] block %d rec=0x%x head=0x%x\n", blk, rec, t);
                for (int i = 0; t != 0u && i < 12; ++i)
                {
                    const int16_t w = (int16_t)(dc2_read_u32(rdram, t + 0x0u) >> 16); // +2 (hword in low word? read raw)
                    const uint32_t wh = dc2_read_u32(rdram, t + 0x00u); // [+0]=?,[+2]=w,[+4]=h... read +0x00 word then hwords
                    const uint16_t ww = (uint16_t)(dc2_read_u32(rdram, t + 0x00u) >> 16);
                    const uint16_t hh = (uint16_t)(dc2_read_u32(rdram, t + 0x04u) & 0xFFFFu);
                    const uint16_t fmt = (uint16_t)(dc2_read_u32(rdram, t + 0x04u) >> 16);
                    const uint32_t tex0lo = dc2_read_u32(rdram, t + 0x38u);
                    const uint32_t tbp = tex0lo & 0x3FFFu;
                    const uint32_t tbw = (tex0lo >> 14) & 0x3Fu;
                    const uint32_t psm = (tex0lo >> 20) & 0x3Fu;
                    const int32_t size = (int32_t)dc2_read_u32(rdram, t + 0x28u);
                    const uint32_t nxt = dc2_read_u32(rdram, t + 0x68u);
                    std::fprintf(stderr, "[G90:texmgr]   tex=0x%x wh=(%u,%u) fmt=%u tbp=0x%x tbw=%u psm=0x%x size=%d next=0x%x\n",
                                 t, ww, hh, fmt, tbp, tbw, psm, size, nxt);
                    (void)w; (void)wh;
                    t = nxt;
                }
            }
            std::fflush(stderr);
        }
    }

    // G81: title-camera A/B vs HW (read on the DOMINANT direct title-draw path; the f48
    // override only sees the early indirect path). HW ref @ New Game menu: phase 0, dist 1198.75,
    // height -214, pos(-70.1,66.8,707.7), ref(-70.1,280.8,-493.0). Reads the camera globals at
    // fixed addresses: TitleCamera ptr @0x377E38 -> distance(+0x90)/height(+0x94)/pos(+0)/ref(+0x10).
    if (std::getenv("DC2_G81_CAM") != nullptr && g67_title_scope(rdram, ctx))
    {
        static std::atomic<uint32_t> s_camN{0};
        const uint32_t cn = s_camN.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (cn <= 4u || (cn % 400u) == 0u)
        {
            const uint32_t cam = dc2_read_u32(rdram, 0x00377E38u);
            const uint32_t phase = dc2_read_u32(rdram, 0x00377E44u);
            const uint32_t counter = dc2_read_u32(rdram, 0x00377E48u);
            auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
            std::fprintf(stderr,
                         "[G81:cam] cn=%u cam=0x%x phase=%u counter=%u dist=%.2f height=%.2f pos=(%.1f,%.1f,%.1f) ref=(%.1f,%.1f,%.1f)\n",
                         cn, cam, phase, counter,
                         cam ? rf(cam + 0x90u) : 0.f, cam ? rf(cam + 0x94u) : 0.f,
                         cam ? rf(cam + 0x00u) : 0.f, cam ? rf(cam + 0x04u) : 0.f, cam ? rf(cam + 0x08u) : 0.f,
                         cam ? rf(cam + 0x10u) : 0.f, cam ? rf(cam + 0x14u) : 0.f, cam ? rf(cam + 0x18u) : 0.f);
            std::fflush(stderr);
        }
    }

    // G82: read the runner's title-map lighting (mgRenderInfo@0x380ec0). The rock renders green
    // because the per-vertex shade is wrong; HW's rock vertex colour == its AMBIENT (46.5,69,61.5)
    // (TitleMapDraw: GetLightInfo(TitleMap) -> *1.5 -> mgSetAmbient). lightInfo = 0x380ec0 +
    // idx*0x150 + 0x400 (idx=*(0x380ec0+0x3f4)); ambient=lightInfo+0x80, lightcol=+0x00, lightdir=+0x40.
    if (std::getenv("DC2_G82_LIGHT") != nullptr && g67_title_scope(rdram, ctx))
    {
        static std::atomic<uint32_t> s_litN{0};
        const uint32_t ln = s_litN.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (ln <= 3u || (ln % 400u) == 0u)
        {
            auto rf = [&](uint32_t a) { uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4); return f; };
            const uint32_t idx = dc2_read_u32(rdram, 0x00380EC0u + 0x3F4u);
            const uint32_t li = 0x00380EC0u + idx * 0x150u + 0x400u;
            std::fprintf(stderr,
                "[G82:light] ln=%u idx=%u amb=(%.1f,%.1f,%.1f,%.1f) col0=(%.3f,%.3f,%.3f) dir0=(%.1f,%.1f,%.1f) fog=(%.1f,%.1f,%.1f,%.1f)\n",
                ln, idx, rf(li + 0x80u), rf(li + 0x84u), rf(li + 0x88u), rf(li + 0x8Cu),
                rf(li + 0x00u), rf(li + 0x04u), rf(li + 0x08u),
                rf(li + 0x40u), rf(li + 0x44u), rf(li + 0x48u),
                rf(0x00381E9Cu), rf(0x00381EA0u), rf(0x00381EA4u), rf(0x00381EA8u));
            // G84: full light-COLOUR matrix (3 cols) + dir cols 1/2 to compare vs HW
            // (HW: col0=(-0.894,0,0) col1=(-0.447,0,0) col2=0 -> R-only). If the runner's
            // cols carry G/B the matrix is built wrong; if R-only the consumption transposes.
            std::fprintf(stderr,
                "[G84:lightmtx] colR=(%.3f,%.3f,%.3f) colG=(%.3f,%.3f,%.3f) colB=(%.3f,%.3f,%.3f) | c0=(%.3f,%.3f,%.3f) c1=(%.3f,%.3f,%.3f) c2=(%.3f,%.3f,%.3f) | d1=(%.1f,%.1f,%.1f) d2=(%.1f,%.1f,%.1f)\n",
                rf(li + 0x00u), rf(li + 0x10u), rf(li + 0x20u),
                rf(li + 0x04u), rf(li + 0x14u), rf(li + 0x24u),
                rf(li + 0x08u), rf(li + 0x18u), rf(li + 0x28u),
                rf(li + 0x00u), rf(li + 0x04u), rf(li + 0x08u),
                rf(li + 0x10u), rf(li + 0x14u), rf(li + 0x18u),
                rf(li + 0x20u), rf(li + 0x24u), rf(li + 0x28u),
                rf(li + 0x50u), rf(li + 0x54u), rf(li + 0x58u),
                rf(li + 0x60u), rf(li + 0x64u), rf(li + 0x68u));
            std::fflush(stderr);
        }
    }

    // G84: ambient-perturbation mechanism test (diagnostic; env-gated, off by default).
    // Scale the stored title-map ambient RGB (lightInfo+0x80) by DC2_G84_AMBSCALE just before
    // the per-part render packet is built (mgClipInBoxW runs in Draw__8mgCFrame, upstream of
    // Draw__12mgCVisualMDT/CreateRenderInfoPacket). Then watch [G83:rgba]: if the VU-output
    // colour scales with this -> the colour is (re)computed from this ambient downstream
    // (ambient double-apply / wrong consume); if it does NOT move -> colour is baked upstream.
    {
        static const char *s_ambEnv = std::getenv("DC2_G84_AMBSCALE");
        static const float s_ambScale = s_ambEnv ? std::strtof(s_ambEnv, nullptr) : 0.0f;
        if (s_ambScale > 0.0f && g67_title_scope(rdram, ctx))
        {
            const uint32_t idx = dc2_read_u32(rdram, 0x00380EC0u + 0x3F4u);
            const uint32_t li = 0x00380EC0u + idx * 0x150u + 0x400u;
            auto scaleF = [&](uint32_t a) {
                uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4);
                f *= s_ambScale; std::memcpy(&u, &f, 4); dc2_write_u32(rdram, a, u);
            };
            scaleF(li + 0x80u); scaleF(li + 0x84u); scaleF(li + 0x88u);
        }
    }

    // G84: directional-light isolation. Scale the whole light-COLOUR matrix (lightInfo +0x00..0x2F,
    // 3 columns) by DC2_G84_LITSCALE. LITSCALE=0 -> ambient-only; if the green G/B vanishes the
    // directional term is the inflation source, if it persists the per-vertex model colour is.
    {
        static const char *s_litEnv = std::getenv("DC2_G84_LITSCALE");
        static const bool s_litSet = (s_litEnv != nullptr);
        static const float s_litScale = s_litEnv ? std::strtof(s_litEnv, nullptr) : 1.0f;
        if (s_litSet && g67_title_scope(rdram, ctx))
        {
            const uint32_t idx = dc2_read_u32(rdram, 0x00380EC0u + 0x3F4u);
            const uint32_t li = 0x00380EC0u + idx * 0x150u + 0x400u;
            auto scaleF = [&](uint32_t a) {
                uint32_t u = dc2_read_u32(rdram, a); float f; std::memcpy(&f, &u, 4);
                f *= s_litScale; std::memcpy(&u, &f, 4); dc2_write_u32(rdram, a, u);
            };
            for (uint32_t o = 0u; o < 0x30u; o += 4u) scaleF(li + o);
        }
    }

    if (!fromFrameDraw)
        return;
    G67FrameScope *scope = g67_top_frame_scope();
    uint32_t ret = getRegU32(ctx, 2);
    uint32_t fixed = 0u;
    // G79 NOTE: broadening this from the single title rock frame (0x981390) to EVERY title map
    // frame (`|| g67_title_scope(rdram,ctx)`) was tried to fix the costume->title round-trip
    // flat-blue and did NOT help ג€” proving the round-trip regression is NOT this routing (the
    // title-map scene isn't restored at all post-costume: `g67_title_scope` is false because the
    // titleMap pointer 0x00377E34 != 0x008FC880). Reverted to the narrow, more-correct scope
    // (only the known-visible rock frame; broadening would also force genuinely off-screen parts
    // to draw, which HW culls). The round-trip needs a scene-restoration fix (see phase-G79).
    if (g78_copy_fix_enabled() && scope && scope->distFix && ret == 0u)
    {
        setReturnU32(ctx, 1u);
        ret = 1u;
        fixed = 1u;
    }
    if (!g67_trace_enabled())
        return;
    const uint32_t n = g_g78_clipinbox.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const bool focus = scope && g67_focus_frame(scope->frame, scope->f8);
    if (focus || ret == 0u || n <= 80u || (n % 240u) == 0u)
    {
        // G79: also log the front-end scene state (loopNo @ gp-0x7524, titleMap @ 0x00377E34)
        // so a post-costume capture shows whether the title-map scene is restored
        // (titleMap should be 0x008FC880; g67_title_scope true only then).
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t loopNo = gp ? dc2_read_u32(rdram, gp + 0xFFFF8ADCu) : 0xFFFFFFFFu;
        const uint32_t titleMap = dc2_read_u32(rdram, 0x00377E34u);
        std::fprintf(stderr,
                     "[G78:clipinbox] n=%u frame=0x%x visual=0x%x ra=0x%x ret=%u fixed=%u "
                     "loop=%u titleMap=0x%x a=(0x%x,0x%x,0x%x,0x%x)\n",
                     n, scope ? scope->frame : 0u, scope ? scope->f8 : 0u, ra, ret, fixed,
                     loopNo, titleMap, a0, a1, a2, a3);
        std::fflush(stderr);
    }
}

static void g67_frame_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled();
    const bool fixEnabled = g67_title_dist_fix_enabled();
    const bool title = (trace || fixEnabled) && g67_title_scope(rdram, ctx);
    const uint32_t frame = getRegU32(ctx, 4);
    const uint32_t f0 = frame ? dc2_read_u32(rdram, frame + 0xF0u) : 0u;
    const uint32_t f4 = frame ? dc2_read_u32(rdram, frame + 0xF4u) : 0u;
    const uint32_t f8 = frame ? dc2_read_u32(rdram, frame + 0xF8u) : 0u;
    const uint32_t desc18 = f4 ? dc2_read_u32(rdram, f4 + 0x18u) : 0u;
    const uint32_t desc40 = f4 ? dc2_read_u32(rdram, f4 + 0x40u) : 0u;
    const uint32_t desc48 = f4 ? dc2_read_u32(rdram, f4 + 0x48u) : 0u;
    const uint32_t child = frame ? dc2_read_u32(rdram, frame + 0x58u) : 0u;
    const bool focus = g67_focus_frame(frame, f8);
    const bool target = g67_title_frame_target(frame, f8);
    const bool traceTrack = trace && (title || focus);
    // G80: broaden the copy/uncull scope from the single G78 target frame to EVERY title-map
    // frame so the block-0 rock visuals (not just block-1 s19_05) route to copy. Opt-in.
    const bool fixTrack = fixEnabled && title && (target || g80_rock_copy_enabled());
    const bool track = traceTrack || fixTrack;
    const uint32_t before1 = track ? g67_mgr_count(rdram, 0x003820E0u, 1u) : 0u;
    const uint32_t before3 = track ? g67_mgr_count(rdram, 0x003820E0u, 3u) : 0u;
    const uint32_t seq = track ? (g_g67_frame.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;

    uint32_t stackIndex = 0xFFFFFFFFu;
    if (track && g_g67_frame_depth < 64u)
    {
        stackIndex = g_g67_frame_depth++;
        G67FrameScope &scope = g_g67_frame_stack[stackIndex];
        std::memset(&scope, 0, sizeof(scope));
        scope.seq = seq;
        scope.frame = frame;
        scope.f0 = f0;
        scope.f4 = f4;
        scope.f8 = f8;
        scope.desc18 = desc18;
        scope.desc40 = desc40;
        scope.desc48 = desc48;
        scope.child = child;
        scope.before1 = before1;
        scope.before3 = before3;
        scope.distFix = fixTrack ? 1u : 0u;
        scope.clipRet = 0xFFFFFFFFu;
    }

    Draw__8mgCFrameFPUi_0x137e10(rdram, ctx, runtime);

    const uint32_t ret = getRegU32(ctx, 2);
    if (!track)
        return;

    G67FrameScope scope{};
    if (stackIndex != 0xFFFFFFFFu && stackIndex < 64u)
    {
        scope = g_g67_frame_stack[stackIndex];
        g_g67_frame_depth = stackIndex;
    }
    else
    {
        scope.seq = seq;
        scope.frame = frame;
        scope.f0 = f0;
        scope.f4 = f4;
        scope.f8 = f8;
        scope.desc18 = desc18;
        scope.desc40 = desc40;
        scope.desc48 = desc48;
        scope.child = child;
        scope.before1 = before1;
        scope.before3 = before3;
    }

    if (!traceTrack)
        return;

    const uint32_t after1 = g67_mgr_count(rdram, 0x003820E0u, 1u);
    const uint32_t after3 = g67_mgr_count(rdram, 0x003820E0u, 3u);
    const bool queued = (after1 != scope.before1) || (after3 != scope.before3);
    const bool logLine = focus || queued || seq <= 180u || (ret == 0u && f4 != 0u);
    if (!logLine)
        return;

    std::fprintf(stderr,
                 "[G67:frame] n=%u depth=%u frame=0x%x child=0x%x f0=0x%x f4=0x%x "
                 "desc18=0x%x desc40=0x%x desc48=0x%x f8=0x%x ret=0x%x reason=%s "
                 "test1=%u fixed=%u dist=% .6g orig=% .6g limit=% .6g clip=%u/%u clipFixed=%u "
                 "counts1=%u->%u counts3=%u->%u\n",
                 seq, stackIndex == 0xFFFFFFFFu ? g_g67_frame_depth : stackIndex,
                 frame, child, f0, f4, desc18, desc40, desc48, f8, ret,
                 g67_frame_reason(scope, ret), scope.test1Seen, scope.test1Fixed,
                 scope.test1Dist, scope.test1OriginalDist, scope.test1Limit,
                 scope.clipSeen, scope.clipRet, scope.clipFixed,
                 scope.before1, after1, scope.before3, after3);
    std::fflush(stderr);
}

static void g67_get_texture_block_no_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t set = getRegU32(ctx, 4);
    const uint32_t idx = getRegU32(ctx, 5);
    const uint32_t buf = getRegU32(ctx, 6);
    const uint32_t max = getRegU32(ctx, 7);
    const uint32_t ra = getRegU32(ctx, 31);

    GetTextureBlockNo__11CMdsListSetFiPii_0x168fd0(rdram, ctx, runtime);

    if (!g67_trace_enabled())
        return;
    const bool titleSet = (set == 0x01DDA630u);
    const bool titleCaller = (ra == 0x002A2598u || ra == 0x002A2644u);
    if (!titleSet && !titleCaller && !g67_title_scope(rdram, ctx))
        return;

    const uint32_t n = g_g67_block.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n > 160u && (n % 240u) != 0u)
        return;

    const int32_t ret = static_cast<int32_t>(getRegU32(ctx, 2));
    const uint32_t imgCnt = set ? dc2_read_u32(rdram, set + 0x90u) : 0u;
    uint32_t foundEntry = 0xFFFFFFFFu;
    uint32_t foundBase = 0u;
    uint32_t foundCount = 0u;
    const uint32_t scanCount = (imgCnt < 32u) ? imgCnt : 32u;
    if (idx < 32u)
    {
        for (uint32_t i = 0u; i < scanCount; ++i)
        {
            const uint32_t flag = dc2_read_u32(rdram, set + 0x94u + i * 8u);
            const uint32_t tbl = dc2_read_u32(rdram, set + 0x98u + i * 8u);
            if (!flag || !tbl)
                continue;
            const uint32_t base = dc2_read_u32(rdram, tbl + idx * 4u);
            const uint32_t count = dc2_read_u32(rdram, tbl + 0x80u + idx * 4u);
            if (count != 0u || base != 0xFFFFFFFFu)
            {
                foundEntry = i;
                foundBase = base;
                foundCount = count;
                break;
            }
        }
    }

    char line[1024];
    int p = std::snprintf(line, sizeof(line),
        "[G67:block] n=%u set=0x%x idx=%u ret=%d max=%u buf=0x%x ra=0x%x pass=%s imgCnt=%u found=%u base=0x%x cnt=%u",
        n, set, idx, ret, max, buf, ra,
        (ra == 0x002A2598u) ? "0-5" : ((ra == 0x002A2644u) ? "6-15" : "other"),
        imgCnt, foundEntry, foundBase, foundCount);
    g67_append_block_list(line, sizeof(line), &p, rdram, buf, ret, 24);
    std::fprintf(stderr, "%s\n", line);
    std::fflush(stderr);
}

static void g67_map_drawsub_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled() && g67_title_scope(rdram, ctx);
    const uint32_t map = getRegU32(ctx, 4);
    const int32_t mode = static_cast<int32_t>(getRegU32(ctx, 5));
    const uint32_t n = trace ? (g_g67_mapdraw.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;
    if (trace && (n <= 16u || (n % 120u) == 0u))
    {
        const uint32_t drawCount = map ? dc2_read_u32(rdram, map + 0x360u) : 0u;
        const uint32_t drawList = map ? dc2_read_u32(rdram, map + 0x364u) : 0u;
        const uint32_t flags = map ? dc2_read_u32(rdram, map + 0xCB0u) : 0u;
        char line[1024];
        int p = std::snprintf(line, sizeof(line),
            "[G67:map.drawsub] n=%u map=0x%x mode=%d drawCount=%u drawList=0x%x flags=0x%x parts=",
            n, map, mode, drawCount, drawList, flags);
        const uint32_t lim = (drawCount < 10u) ? drawCount : 10u;
        for (uint32_t i = 0u; i < lim && p < static_cast<int>(sizeof(line)) - 80; ++i)
        {
            const uint32_t part = drawList ? dc2_read_u32(rdram, drawList + i * 4u) : 0u;
            const uint32_t vt = part ? dc2_read_u32(rdram, part) : 0u;
            const uint32_t pflags = part ? dc2_read_u32(rdram, part + 0x2B0u) : 0u;
            p += std::snprintf(line + p, sizeof(line) - static_cast<size_t>(p),
                               "%s0x%x(vt=0x%x fl=0x%x)", (i == 0u) ? "" : ",", part, vt, pflags);
        }
        std::fprintf(stderr, "%s\n", line);
        std::fflush(stderr);
    }

    DrawSub__4CMapFi_0x15e250(rdram, ctx, runtime);
}

static void g67_mapparts_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled() && g67_title_scope(rdram, ctx);
    const uint32_t part = getRegU32(ctx, 4);
    const uint32_t before1 = trace ? g67_mgr_count(rdram, 0x003820E0u, 1u) : 0u;
    const uint32_t before3 = trace ? g67_mgr_count(rdram, 0x003820E0u, 3u) : 0u;

    g56_mapparts_draw_tap(rdram, ctx, runtime);

    if (!trace)
        return;
    const uint32_t n = g_g67_partsdraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n > 80u && (n % 240u) != 0u)
        return;
    const uint32_t vt = part ? dc2_read_u32(rdram, part + 0x00u) : 0u;
    const uint32_t flags = part ? dc2_read_u32(rdram, part + 0x2B0u) : 0u;
    const uint32_t lightMtx = part ? dc2_read_u32(rdram, part + 0x2E4u) : 0u;
    const uint32_t plight = part ? dc2_read_u32(rdram, part + 0x2E8u) : 0u;
    const uint32_t lightInfo = part ? dc2_read_u32(rdram, part + 0x300u) : 0u;
    std::fprintf(stderr,
        "[G67:parts.draw] n=%u part=0x%x vt=0x%x flags=0x%x lightMtx=0x%x plight=0x%x lightInfo=0x%x ret=0x%x counts1=%u->%u counts3=%u->%u\n",
        n, part, vt, flags, lightMtx, plight, lightInfo, getRegU32(ctx, 2),
        before1, g67_mgr_count(rdram, 0x003820E0u, 1u),
        before3, g67_mgr_count(rdram, 0x003820E0u, 3u));
    std::fflush(stderr);
}

static void g67_mgenddraw_reload_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled() && g67_title_scope(rdram, ctx);
    const int32_t reqBlock = static_cast<int32_t>(getRegU32(ctx, 4));
    const uint32_t mgrArg = getRegU32(ctx, 5);
    const uint32_t mgr = mgrArg ? mgrArg : 0x003820E0u;
    const uint32_t ra = getRegU32(ctx, 31);

    if (trace)
    {
        const uint32_t n = g_g67_enddraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 200u || (n % 360u) == 0u)
        {
            char line[1400];
            int p = std::snprintf(line, sizeof(line),
                "[G67:end.reload] n=%u mgr=0x%x mgrArg=0x%x ra=0x%x", n, mgr, mgrArg, ra);
            g67_append_mgr_block_summary(line, sizeof(line), &p, rdram, mgr, reqBlock);
            std::fprintf(stderr, "%s\n", line);
            std::fflush(stderr);
        }
    }

    // G90: per-block reload diagnostics ג€” find why blocks 1/3/6/7 never get tbps written.
    // Capture mgr remap + texMgr cache/cursor BEFORE, the recompiled return AFTER.
    const bool g90 = (std::getenv("DC2_G90_RELOAD") != nullptr) && g67_title_scope(rdram, ctx);
    uint32_t mgr0 = 0u, mgr1 = 0u, remap = 0u, tmCache = 0u, tmCursor = 0u, tmCount = 0u, headTbp = 0u;
    if (g90)
    {
        mgr0 = dc2_read_u32(rdram, mgr + 0x00u);
        mgr1 = dc2_read_u32(rdram, mgr + 0x04u);
        const uint32_t texMgr = dc2_read_u32(rdram, mgr + 0x58u);
        tmCache = dc2_read_u32(rdram, texMgr + 0x08u);
        tmCursor = dc2_read_u32(rdram, texMgr + 0x00u);
        tmCount = dc2_read_u32(rdram, texMgr + 0x0Cu);
        if (mgr0 != 0u && mgrArg == 0u) {} // remap only consulted when mgr[0]!=0
        if (mgr0 != 0u && reqBlock >= 0)
            remap = dc2_read_u32(rdram, mgr1 + (uint32_t)reqBlock * 4u);
        else
            remap = (uint32_t)reqBlock;
        const uint32_t arr = dc2_read_u32(rdram, texMgr + 0x10u);
        const uint32_t head = (reqBlock >= 0) ? dc2_read_u32(rdram, arr + (uint32_t)reqBlock * 0x10u + 8u) : 0u;
        headTbp = head ? (dc2_read_u32(rdram, head + 0x38u) & 0x3FFFu) : 0xFFFFu;
    }

    mgEndDrawReloadTexture__FiP14mgCDrawManager_0x142560(rdram, ctx, runtime);

    if (g90)
    {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 40u)
        {
            const uint32_t texMgr = dc2_read_u32(rdram, mgr + 0x58u);
            const uint32_t arr = dc2_read_u32(rdram, texMgr + 0x10u);
            const uint32_t head = (reqBlock >= 0) ? dc2_read_u32(rdram, arr + (uint32_t)reqBlock * 0x10u + 8u) : 0u;
            const uint32_t postTbp = head ? (dc2_read_u32(rdram, head + 0x38u) & 0x3FFFu) : 0xFFFFu;
            std::fprintf(stderr,
                "[G90:reload] n=%u block=%d ra=0x%x mgr0=0x%x mgr1=0x%x remap=%d cache=%d cursor=0x%x count=%u headTbp:0x%x->0x%x ret=%u\n",
                n, reqBlock, ra, mgr0, mgr1, (int32_t)remap, (int32_t)tmCache, tmCursor, tmCount,
                headTbp, postTbp, getRegU32(ctx, 2));
            std::fflush(stderr);
        }
    }
}

static void g67_mgenddraw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool trace = g67_trace_enabled() && g67_title_scope(rdram, ctx);
    const int32_t reqBlock = static_cast<int32_t>(getRegU32(ctx, 4));
    const uint32_t mgrArg = getRegU32(ctx, 5);
    const uint32_t mgr = mgrArg ? mgrArg : 0x003820E0u;
    const uint32_t ra = getRegU32(ctx, 31);

    if (trace)
    {
        const uint32_t n = g_g67_enddraw.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 200u || (n % 360u) == 0u)
        {
            char line[1400];
            int p = std::snprintf(line, sizeof(line),
                "[G67:end.draw] n=%u mgr=0x%x mgrArg=0x%x ra=0x%x", n, mgr, mgrArg, ra);
            g67_append_mgr_block_summary(line, sizeof(line), &p, rdram, mgr, reqBlock);
            std::fprintf(stderr, "%s\n", line);
            std::fflush(stderr);
        }
    }

    // G90: tag the rasterizer with the block currently flushing so [G88:geo] can attribute
    // each drawn rock triangle to its source block.
    const bool inTitle = g67_title_scope(rdram, ctx);
    if (inTitle)
        g_dc2TitleCurBlock.store(reqBlock, std::memory_order_relaxed);
    mgEndDraw__FiP14mgCDrawManager_0x142580(rdram, ctx, runtime);
    if (inTitle)
        g_dc2TitleCurBlock.store(-1, std::memory_order_relaxed);
}

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

    if (g91)
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
            if (head != 0u && call <= 12u)
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
    const bool addScope = g67_addpacket_scope(rdram, ctx, self, state);
    const bool g135Src = g135_src_trace_enabled() && addScope;
    const bool g95AddMini = g95_add_mini_tex0_enabled() && g67_title_scope(rdram, ctx);

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

    if ((trace || g135Src) && addScope)
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
static bool g34_fix_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_G34_FIX");
    return e;
}

static bool g40_joints_enabled()
{
    static const bool e = dc2_env_flag_enabled("DC2_G40_JOINTS");
    return e;
}

static std::atomic<uint32_t> g_g34_char{0};
static std::atomic<uint32_t> g_g34_outline{0};

// Wrap DrawDirect__11CCharacter2@0x1731f0. a0 = CCharacter2 this; *(this+0x100)
// is the float outline level read at the function head (recomp 0x173214).
static void g34_char_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const bool valid = (self > 0x80000u && self < 0x2000000u);
    uint32_t outlineBits = valid ? dc2_read_u32(rdram, self + 0x100u) : 0u;
    float outline; std::memcpy(&outline, &outlineBits, sizeof(outline));
    bool fixed = false;
    if (g34_fix_enabled() && valid && !(outline >= 1.0f))
    {
        outlineBits = 0x3F800000u; // 1.0f
        dc2_write_u32(rdram, self + 0x100u, outlineBits);
        outline = 1.0f;
        fixed = true;
    }
    const uint32_t n = g_g34_char.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 16u || (n % 240u) == 0u)
    {
        std::fprintf(stderr, "[G34:charDraw] n=%u this=0x%x outlineLvl(+0x100)=0x%x(%g) fixed=%d\n",
                     n, self, outlineBits, static_cast<double>(outline), fixed ? 1 : 0);
        std::fflush(stderr);
    }
    DrawDirect__11CCharacter2Fv_0x1731f0(rdram, ctx, runtime);

    // G40: after the draw (which runs the deform), dump the skeleton joint LW matrices
    // (joint+0x70) + dirty flag (joint+0x40). HW A/B (PCSX2 @ costume) shows a fully
    // posed skeleton (root frame 0xee7410, joints 0xee7410..0xee8840, real rotation+
    // translation). G39 proved the runner's bone palette is ZERO. If these joints read
    // zero -> (A) skeleton unposed; if nonzero -> (B) deform/COP2 math zeroes the palette.
    if (g40_joints_enabled() && valid && (n <= 4u))
    {
        const uint32_t rootFrame = dc2_read_u32(rdram, self + 0x70u); // this[0x1c]
        std::fprintf(stderr, "[G40:joints] n=%u char=0x%x rootFrame(+0x70)=0x%x\n",
                     n, self, rootFrame);
        static const uint32_t hwJoints[] = {
            0xee7410u, 0xee82f0u, 0xee8400u, 0xee8510u, 0xee8620u, 0xee8730u, 0xee8840u};
        for (uint32_t j = 0; j < (sizeof(hwJoints) / sizeof(hwJoints[0])); ++j)
        {
            const uint32_t jp = hwJoints[j];
            const uint32_t dirty = dc2_read_u32(rdram, jp + 0x40u);
            const uint32_t force = dc2_read_u32(rdram, jp + 0xfcu);
            const uint32_t par = dc2_read_u32(rdram, jp + 0x54u);
            float m[16];
            for (int k = 0; k < 16; ++k)
            {
                uint32_t b = dc2_read_u32(rdram, jp + 0x70u + (uint32_t)(k * 4));
                std::memcpy(&m[k], &b, sizeof(float));
            }
            std::fprintf(stderr,
                "[G40:joint] j=%u addr=0x%x dirty=%u force=%u par=0x%x LW=[%.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f]\n",
                j, jp, dirty, force, par,
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
                m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
        // HW skinned MotionMDT visuals (PCSX2 A/B): 0xf48ce0, 0xfdebc0. Dump their
        // bone-binding inputs (+0x50 frameArr -> bone-frame ptrs, +0x54 count, +0x58
        // baseMtx, +0x80 bone-index list). HW: +0x50=0xeef7e0 -> [0xee7410,0xee7520..],
        // idx=[0x12,0x15,0x18,..]. If runner matches -> palette-build exec bug; else binding gap.
        static const uint32_t hwVisuals[] = {0xf48ce0u, 0xfdebc0u};
        for (uint32_t v = 0; v < 2; ++v)
        {
            const uint32_t vis = hwVisuals[v];
            const uint32_t f50 = dc2_read_u32(rdram, vis + 0x50u);
            const uint32_t f54 = dc2_read_u32(rdram, vis + 0x54u);
            const uint32_t f58 = dc2_read_u32(rdram, vis + 0x58u);
            std::fprintf(stderr, "[G40:visual] vis=0x%x +0x50(frameArr)=0x%x +0x54=0x%x +0x58(baseMtx)=0x%x idx[0..7]=",
                         vis, f50, f54, f58);
            for (int i = 0; i < 8; ++i)
                std::fprintf(stderr, "0x%x ", dc2_read_u32(rdram, vis + 0x80u + (uint32_t)(i * 4)));
            std::fprintf(stderr, "\n");
            if (f50 > 0x80000u && f50 < 0x2000000u)
            {
                std::fprintf(stderr, "[G40:framearr] vis=0x%x ptrs[0..5]=", vis);
                for (int i = 0; i < 6; ++i)
                    std::fprintf(stderr, "0x%x ", dc2_read_u32(rdram, f50 + (uint32_t)(i * 4)));
                std::fprintf(stderr, "\n");
            }
        }
        std::fflush(stderr);
    }
}

// Wrap Draw__12COutLineDraw::Draw@0x17c2d0. a0 = COutLineDraw this; param_1=f12,
// param_2=f13 (== fVar13, the composite gate value). +0x64 must be 0, +0x38 is
// outlineW (>0 selects the RTT-outline branch), +0x30/+0x34 are the frames.
static void g34_outline_draw_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4);
    const bool valid = (self > 0x80000u && self < 0x2000000u);
    const float p1 = ctx->f[12], p2 = ctx->f[13];
    const uint32_t v30 = valid ? dc2_read_u32(rdram, self + 0x30u) : 0u;
    const uint32_t v34 = valid ? dc2_read_u32(rdram, self + 0x34u) : 0u;
    uint32_t v38b = valid ? dc2_read_u32(rdram, self + 0x38u) : 0u;
    float v38; std::memcpy(&v38, &v38b, sizeof(v38));
    const uint32_t v60 = valid ? dc2_read_u32(rdram, self + 0x60u) : 0u;
    const uint32_t v64 = valid ? dc2_read_u32(rdram, self + 0x64u) : 0u;
    const uint32_t n = g_g34_outline.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 24u || (n % 240u) == 0u)
    {
        std::fprintf(stderr, "[G34:outline] n=%u this=0x%x p1=%g p2(fVar13)=%g p2ge1=%d +0x30=0x%x +0x34=0x%x +0x38(outlineW)=%g +0x60=0x%x +0x64=0x%x\n",
                     n, self, static_cast<double>(p1), static_cast<double>(p2),
                     (p2 >= 1.0f) ? 1 : 0, v30, v34, static_cast<double>(v38), v60, v64);
        std::fflush(stderr);
    }
    // G37: scope the mgGetDrawRect bbox repair to this preview-composite path only.
    g_g37_in_outline.fetch_add(1, std::memory_order_relaxed);
    Draw__12COutLineDrawFff_0x17c2d0(rdram, ctx, runtime);
    g_g37_in_outline.fetch_sub(1, std::memory_order_relaxed);
}

// PHASE G37: repair the collapsed costume-model bounding box at its SOURCE.
// On the runner the VU0 AABB->screen projection in GetDrawRect collapses every corner to
// the screen-center point, so Draw__12COutLineDraw computes a center-point rect for BOTH the
// black-fill clear of the RTT and the RTT->display composite. The collapsed clear leaves the
// rest of the 0x2720 work page holding the time-shared font/menu atlas, which the composite
// then samples as CT32 garbage (G37 RTT dump confirmed this). Rewriting the box to the
// PCSX2-verified preview extents fixes the clear and the composite together; the G36
// composite-rect patch then becomes a no-op backstop. mgVu0FBOX layout: +0x00 max.xyzw,
// +0x10 min.xyzw (GetDrawRect writes sceVu0CopyVector(box,max) + (box+4,min)).
static std::atomic<uint32_t> g_g37_boxfix{0};
static void g37_getdrawrect_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t box = getRegU32(ctx, 5); // a1 = mgVu0FBOX* output
    mgGetDrawRect__FP8mgCFrameP9mgVu0FBOX_0x143160(rdram, ctx, runtime);
    if (g_g37_in_outline.load(std::memory_order_relaxed) <= 0 ||
        getRegU32(ctx, 2) == 0u ||                 // ret==0: not visible, COutLineDraw bails anyway
        box <= 0x80000u || box >= 0x2000000u)
        return;

    auto readF = [&](uint32_t off) { uint32_t u = dc2_read_u32(rdram, box + off); float f; std::memcpy(&f, &u, sizeof(f)); return f; };
    const float maxX = readF(0x00u), maxY = readF(0x04u);
    const float minX = readF(0x10u), minY = readF(0x14u);
    // Collapsed iff the projected box has ~zero screen area.
    const float dx = (maxX > minX) ? (maxX - minX) : (minX - maxX);
    const float dy = (maxY > minY) ? (maxY - minY) : (minY - maxY);
    if (dx >= 2.0f || dy >= 2.0f)
        return;

    auto writeF = [&](uint32_t off, float f) { uint32_t u; std::memcpy(&u, &f, sizeof(u)); dc2_write_u32(rdram, box + off, u); };
    writeF(0x00u, 512.0f);     // max.x -> rect x1 0x2000
    writeF(0x04u, 380.875f);   // max.y -> rect y1 0x17ce
    writeF(0x10u, 223.9375f);  // min.x -> rect x0 0x0dff
    writeF(0x14u, 0.0f);       // min.y -> rect y0 0x0000

    const uint32_t n = g_g37_boxfix.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 8u || (n % 240u) == 0u)
    {
        std::fprintf(stderr,
                     "[G37:boxfix] n=%u box=0x%x oldmax=(%.2f,%.2f) oldmin=(%.2f,%.2f) -> max=(512,380.875) min=(223.9375,0)\n",
                     n, box, static_cast<double>(maxX), static_cast<double>(maxY),
                     static_cast<double>(minX), static_cast<double>(minY));
        std::fflush(stderr);
    }
}

static std::atomic<uint32_t> g_g34_divsprite{0};
static std::atomic<uint32_t> g_g36_packet_dump{0};
static std::atomic<uint32_t> g_g36_directdata{0};
static std::atomic<uint32_t> g_g36_endprim2{0};
static std::atomic<uint32_t> g_g36_rectfix{0};

static uint64_t g36_read_qword(uint8_t *rdram, uint32_t addr)
{
    return dc2_read_u64(rdram, addr & 0x1FFFFFFFu);
}

static const char *g36_gs_reg_name(uint8_t reg)
{
    switch (reg)
    {
    case 0x00: return "PRIM";
    case 0x01: return "RGBAQ";
    case 0x02: return "ST";
    case 0x03: return "UV";
    case 0x04: return "XYZF2";
    case 0x05: return "XYZ2";
    case 0x06: return "TEX0_1";
    case 0x07: return "TEX0_2";
    case 0x0E: return "A+D";
    case 0x0F: return "NOP";
    default: return "REG";
    }
}

static bool g36_parse_gif_tag(uint64_t tagLo,
                              uint64_t tagHi,
                              uint32_t remainingBytes,
                              uint32_t *outNloop,
                              uint32_t *outNreg,
                              uint8_t *outFlg,
                              uint32_t *outBytes)
{
    // Bits 16..45 are reserved in GIFtag words. The enclosing VIF/DMA tags in an
    // mgVif1Packet often look superficially GIF-like, but have these bits set.
    if ((tagLo & 0x00003FFFFFFF0000ull) != 0ull)
        return false;

    const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
    if (nreg == 0u)
        nreg = 16u;
    if (nloop == 0u || flg > 2u)
        return false;

    uint64_t payloadBytes = 0ull;
    if (flg == 0u)
        payloadBytes = static_cast<uint64_t>(nloop) * static_cast<uint64_t>(nreg) * 16ull;
    else if (flg == 1u)
        payloadBytes = static_cast<uint64_t>((nloop * nreg + 1u) / 2u) * 16ull;
    else
        payloadBytes = static_cast<uint64_t>(nloop) * 16ull;

    const uint64_t totalBytes = 16ull + payloadBytes;
    if (totalBytes > static_cast<uint64_t>(remainingBytes) || totalBytes > 0x20000ull)
        return false;

    (void)tagHi;
    *outNloop = nloop;
    *outNreg = nreg;
    *outFlg = flg;
    *outBytes = static_cast<uint32_t>(totalBytes);
    return true;
}

static void g36_decode_divsprite_packet(uint8_t *rdram,
                                        uint32_t dumpNo,
                                        uint32_t prim,
                                        uint32_t tex,
                                        uint32_t base,
                                        uint32_t end)
{
    const uint32_t physBase = base & 0x1FFFFFFFu;
    const uint32_t physEnd = end & 0x1FFFFFFFu;
    if (physBase < 0x80000u || physBase >= 0x2000000u ||
        physEnd <= physBase || physEnd > 0x2000000u)
    {
        std::fprintf(stderr,
                     "[G36:packet] dump=%u prim=0x%x tex=0x%x invalid base=0x%x end=0x%x physBase=0x%x physEnd=0x%x\n",
                     dumpNo, prim, tex, base, end, physBase, physEnd);
        return;
    }

    const uint32_t totalBytes = physEnd - physBase;
    std::fprintf(stderr,
                 "[G36:packet] dump=%u prim=0x%x tex=0x%x base=0x%x end=0x%x bytes=0x%x tex0=0x%016llx primState=0x%llx\n",
                 dumpNo, prim, tex, base, end, totalBytes,
                 static_cast<unsigned long long>((tex > 0x80000u && tex < 0x2000000u) ? dc2_read_u64(rdram, tex + 0x38u) : 0u),
                 static_cast<unsigned long long>(dc2_read_u64(rdram, prim + 0x50u)));

    const uint32_t rawBytes = (totalBytes < 0x180u) ? totalBytes : 0x180u;
    for (uint32_t raw = 0u; raw + 16u <= rawBytes; raw += 16u)
    {
        std::fprintf(stderr,
                     "[G36:raw] dump=%u off=0x%x q0=0x%016llx q1=0x%016llx\n",
                     dumpNo, raw,
                     static_cast<unsigned long long>(g36_read_qword(rdram, physBase + raw)),
                     static_cast<unsigned long long>(g36_read_qword(rdram, physBase + raw + 8u)));
    }

    uint32_t scan = 0u;
    uint32_t tag = 0u;
    uint32_t currentTbp = 0xFFFFFFFFu;
    uint32_t currentPsm = 0xFFFFFFFFu;
    uint32_t currentPrim = 0xFFFFFFFFu;
    uint32_t xyzLogged = 0u;

    while (scan + 16u <= totalBytes && tag < 64u)
    {
        const uint32_t tagAddr = physBase + scan;
        const uint64_t tagLo = g36_read_qword(rdram, tagAddr);
        const uint64_t tagHi = g36_read_qword(rdram, tagAddr + 8u);

        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t flg = 0u;
        uint32_t tagBytes = 0u;
        if (!g36_parse_gif_tag(tagLo, tagHi, totalBytes - scan, &nloop, &nreg, &flg, &tagBytes))
        {
            scan += 16u;
            continue;
        }

        uint32_t off = scan + 16u;
        const bool eop = ((tagLo >> 15u) & 1u) != 0u;
        const bool pre = ((tagLo >> 46u) & 1u) != 0u;
        if (pre)
            currentPrim = static_cast<uint32_t>((tagLo >> 47u) & 0x7FFu);

        uint8_t regs[16];
        for (uint32_t i = 0u; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4u)) & 0xFu);

        std::fprintf(stderr,
                     "[G36:tag] dump=%u tag=%u off=0x%x bytes=0x%x lo=0x%016llx hi=0x%016llx nloop=%u nreg=%u flg=%u pre=%u eop=%u prim=0x%x regs=",
                     dumpNo, tag, tagAddr - physBase, tagBytes,
                     static_cast<unsigned long long>(tagLo),
                     static_cast<unsigned long long>(tagHi),
                     nloop, nreg, static_cast<uint32_t>(flg), pre ? 1u : 0u, eop ? 1u : 0u, currentPrim);
        for (uint32_t i = 0u; i < nreg; ++i)
            std::fprintf(stderr, "%s%s", (i == 0u) ? "" : ",", g36_gs_reg_name(regs[i]));
        std::fprintf(stderr, "\n");

        if (flg == 0u)
        {
            for (uint32_t loop = 0u; loop < nloop; ++loop)
            {
                for (uint32_t r = 0u; r < nreg; ++r)
                {
                    if (off + 16u > totalBytes)
                        goto done;
                    const uint32_t itemAddr = physBase + off;
                    const uint64_t lo = g36_read_qword(rdram, itemAddr);
                    const uint64_t hi = g36_read_qword(rdram, itemAddr + 8u);
                    off += 16u;

                    uint8_t reg = regs[r];
                    uint64_t value = lo;
                    if (reg == 0x0Eu)
                        reg = static_cast<uint8_t>(hi & 0xFFu);

                    if (reg == 0x00u)
                    {
                        currentPrim = static_cast<uint32_t>(value & 0x7FFu);
                        std::fprintf(stderr,
                                     "[G36:item] dump=%u tag=%u loop=%u slot=%u off=0x%x reg=PRIM value=0x%llx hi=0x%llx type=%u tme=%u abe=%u fst=%u ctxt=%u\n",
                                     dumpNo, tag, loop, r, itemAddr - physBase,
                                     static_cast<unsigned long long>(value),
                                     static_cast<unsigned long long>(hi),
                                     currentPrim & 7u, (currentPrim >> 4u) & 1u, (currentPrim >> 6u) & 1u,
                                     (currentPrim >> 8u) & 1u, (currentPrim >> 9u) & 1u);
                    }
                    else if (reg == 0x06u || reg == 0x07u)
                    {
                        currentTbp = static_cast<uint32_t>(value & 0x3FFFu);
                        currentPsm = static_cast<uint32_t>((value >> 20u) & 0x3Fu);
                        std::fprintf(stderr,
                                     "[G36:item] dump=%u tag=%u loop=%u slot=%u off=0x%x reg=%s tex0=0x%016llx hi=0x%llx tbp=0x%x psm=0x%x tbw=%u\n",
                                     dumpNo, tag, loop, r, itemAddr - physBase,
                                     g36_gs_reg_name(reg),
                                     static_cast<unsigned long long>(value),
                                     static_cast<unsigned long long>(hi),
                                     currentTbp, currentPsm, static_cast<uint32_t>((value >> 14u) & 0x3Fu));
                    }
                    else if ((reg == 0x04u || reg == 0x05u) && xyzLogged < 32u)
                    {
                        const float x = static_cast<float>(value & 0xFFFFu) / 16.0f;
                        const float y = static_cast<float>((value >> 16u) & 0xFFFFu) / 16.0f;
                        std::fprintf(stderr,
                                     "[G36:item] dump=%u tag=%u loop=%u slot=%u off=0x%x reg=%s value=0x%llx hi=0x%llx xy=(%.1f,%.1f) prim=0x%x tex=0x%x/0x%x\n",
                                     dumpNo, tag, loop, r, itemAddr - physBase,
                                     g36_gs_reg_name(reg),
                                     static_cast<unsigned long long>(value),
                                     static_cast<unsigned long long>(hi),
                                     static_cast<double>(x), static_cast<double>(y),
                                     currentPrim, currentTbp, currentPsm);
                        ++xyzLogged;
                    }
                }
            }
        }
        else if (flg == 1u)
        {
            const uint32_t values = nloop * nreg;
            const uint32_t bytes = ((values + 1u) / 2u) * 16u;
            if (off + bytes > totalBytes)
                goto done;
            off += bytes;
        }
        else if (flg == 2u)
        {
            const uint64_t bytes = static_cast<uint64_t>(nloop) * 16ull;
            if (bytes > static_cast<uint64_t>(totalBytes - off))
                goto done;
            off += static_cast<uint32_t>(bytes);
        }
        else
        {
            break;
        }
        scan += tagBytes;
        ++tag;
    }
done:
    std::fflush(stderr);
}

static void g36_directdata_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool log = g36_packet_trace_enabled() &&
                     g_g34_in_divsprite.load(std::memory_order_relaxed) > 0;
    const uint32_t prim = getRegU32(ctx, 4);
    const uint32_t qwords = getRegU32(ctx, 5);
    const uint32_t before = (prim > 0x80000u && prim < 0x2000000u) ? dc2_read_u32(rdram, prim + 0xDCu) : 0u;
    DirectData__11mgCDrawPrimFi_0x134b00(rdram, ctx, runtime);
    if (log)
    {
        const uint32_t n = g_g36_directdata.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 16u)
        {
            const uint32_t after = (prim > 0x80000u && prim < 0x2000000u) ? dc2_read_u32(rdram, prim + 0xDCu) : 0u;
            std::fprintf(stderr,
                         "[G36:direct] n=%u prim=0x%x qwords=0x%x ret(v0)=0x%x before=0x%x after=0x%x delta=0x%x e0=0x%x e4=0x%x e8=0x%x ec=0x%x f0=0x%x\n",
                         n, prim, qwords, getRegU32(ctx, 2), before, after, after - before,
                         dc2_read_u32(rdram, prim + 0xE0u), dc2_read_u32(rdram, prim + 0xE4u),
                         dc2_read_u32(rdram, prim + 0xE8u), dc2_read_u32(rdram, prim + 0xECu),
                         dc2_read_u32(rdram, prim + 0xF0u));
            std::fflush(stderr);
        }
    }
}

static void g36_endprim2_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool log = g36_packet_trace_enabled() &&
                     g_g34_in_divsprite.load(std::memory_order_relaxed) > 0;
    const uint32_t prim = getRegU32(ctx, 4);
    const uint32_t n = log ? (g_g36_endprim2.fetch_add(1u, std::memory_order_relaxed) + 1u) : 0u;
    uint32_t beforeDc = 0u, e0 = 0u, e4 = 0u, e8 = 0u, ec = 0u, f0 = 0u, nreg = 0u;
    if (log && n <= 16u && prim > 0x80000u && prim < 0x2000000u)
    {
        beforeDc = dc2_read_u32(rdram, prim + 0xDCu);
        e0 = dc2_read_u32(rdram, prim + 0xE0u);
        e4 = dc2_read_u32(rdram, prim + 0xE4u);
        e8 = dc2_read_u32(rdram, prim + 0xE8u);
        ec = dc2_read_u32(rdram, prim + 0xECu);
        f0 = dc2_read_u32(rdram, prim + 0xF0u);
        nreg = dc2_read_u32(rdram, prim + 0x104u);
        std::fprintf(stderr,
                     "[G36:endprim.pre] n=%u prim=0x%x dc=0x%x e0=0x%x e4=0x%x e8=0x%x ec=0x%x f0=0x%x nreg=%u qwc(e8->dc)=0x%x\n",
                     n, prim, beforeDc, e0, e4, e8, ec, f0, nreg, (beforeDc - e8) >> 4u);
        if (e8 > 0x80000u && e8 < 0x2000000u)
        {
            for (uint32_t off = 0u; off < 0x90u && e8 + off + 8u < 0x2000000u; off += 16u)
            {
                std::fprintf(stderr,
                             "[G36:endraw] n=%u off=0x%x q0=0x%016llx q1=0x%016llx\n",
                             n, off,
                             static_cast<unsigned long long>(g36_read_qword(rdram, e8 + off)),
                             static_cast<unsigned long long>(g36_read_qword(rdram, e8 + off + 8u)));
            }
        }
    }

    EndPrim2__11mgCDrawPrimFv_0x134940(rdram, ctx, runtime);

    if (log && n <= 16u && prim > 0x80000u && prim < 0x2000000u)
    {
        const uint32_t afterDc = dc2_read_u32(rdram, prim + 0xDCu);
        std::fprintf(stderr,
                     "[G36:endprim.post] n=%u prim=0x%x dcBefore=0x%x dcAfter=0x%x e8TagLo=0x%016llx e8TagHi=0x%016llx ec=0x%x f0=0x%x\n",
                     n, prim, beforeDc, afterDc,
                     static_cast<unsigned long long>(g36_read_qword(rdram, e8)),
                     static_cast<unsigned long long>(g36_read_qword(rdram, e8 + 8u)),
                     ec ? dc2_read_u32(rdram, ec) : 0u,
                     f0 ? dc2_read_u32(rdram, f0) : 0u);
        std::fflush(stderr);
    }
}

static bool g36_repair_collapsed_divsprite_rect(uint8_t *rdram, uint32_t rect, uint32_t tex)
{
    if (rect <= 0x80000u || rect >= 0x2000000u ||
        tex <= 0x80000u || tex >= 0x2000000u)
    {
        return false;
    }

    const uint64_t tex0 = dc2_read_u64(rdram, tex + 0x38u);
    const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3FFFu);
    const uint32_t psm = static_cast<uint32_t>((tex0 >> 20u) & 0x3Fu);
    const uint32_t y0 = dc2_read_u32(rdram, rect + 0x0u);
    const uint32_t x0 = dc2_read_u32(rdram, rect + 0x4u);
    const uint32_t y1 = dc2_read_u32(rdram, rect + 0x8u);
    const uint32_t x1 = dc2_read_u32(rdram, rect + 0xCu);

    // G36: runner-only costume preview repair. The outline composite is reached with
    // the correct CT32 RTT texture (0x2720) but its screen-space bounds collapse to
    // the center point (0x1000,0x0d00), so DrawDivSprite4 never emits DirectData.
    // PCSX2 at the same "Select Max's Costume" call enters with these extents:
    // y0=0x0dff, x0=0x0000, y1=0x2000, x1=0x17ce.
    if (tbp != 0x2720u || psm != 0u ||
        y0 != 0x1000u || y1 != 0x1000u ||
        x0 != 0x0D00u || x1 != 0x0D00u)
    {
        return false;
    }

    dc2_write_u32(rdram, rect + 0x0u, 0x0DFFu);
    dc2_write_u32(rdram, rect + 0x4u, 0x0000u);
    dc2_write_u32(rdram, rect + 0x8u, 0x2000u);
    dc2_write_u32(rdram, rect + 0xCu, 0x17CEu);

    const uint32_t n = g_g36_rectfix.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (g36_packet_trace_enabled() && (n <= 16u || (n % 240u) == 0u))
    {
        std::fprintf(stderr,
                     "[G36:rectfix] n=%u rect=0x%x tex=0x%x tex0=0x%016llx old=(0x%x,0x%x,0x%x,0x%x) new=(0xdff,0x0,0x2000,0x17ce)\n",
                     n, rect, tex, static_cast<unsigned long long>(tex0), y0, x0, y1, x1);
        std::fflush(stderr);
    }
    return true;
}

// Wrap DrawDivSprite4@0x17cb20 = the RTT->display composite blit (the missing step on the
// runner per G33). a2 = mgCTexture* it samples (the 0x2720 framebuffer grabbed by
// mgGetFrameBuffer). Confirms the composite is actually reached and dumps the texture struct
// head so we can read its tbp0/psm (CT32 vs T8) wherever the RTT texture stores them.
static void g34_divsprite_probe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t prim = getRegU32(ctx, 4);  // a0 = mgCDrawPrim*
    const uint32_t rect = getRegU32(ctx, 5);  // a1 = mgRect<int>*
    const uint32_t tex = getRegU32(ctx, 6);   // a2 = mgCTexture*
    const uint32_t color = getRegU32(ctx, 7); // a3 = rgba/int vector
    const uint32_t expand = getRegU32(ctx, 8);
    const uint32_t arg9 = getRegU32(ctx, 9);
    const uint32_t gp = getRegU32(ctx, 28);
    g36_repair_collapsed_divsprite_rect(rdram, rect, tex);
    const uint32_t n = g_g34_divsprite.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (n <= 16u || (n % 240u) == 0u)
    {
        char buf[512];
        int q = std::snprintf(buf, sizeof(buf),
                              "[G34:divsprite] n=%u prim=0x%x rect=0x%x tex=0x%x color=0x%x expand=0x%x arg9=0x%x gpOff=(0x%x,0x%x) rectv:",
                              n, prim, rect, tex, color, expand, arg9,
                              dc2_read_u32(rdram, gp + 0xFFFF8798u), // gp-0x7868
                              dc2_read_u32(rdram, gp + 0xFFFF879Cu)); // gp-0x7864
        if (rect > 0x80000u && rect < 0x2000000u)
            for (uint32_t o = 0; o < 0x10u && q < (int)sizeof(buf) - 24; o += 4u)
                q += std::snprintf(buf + q, sizeof(buf) - q, " %x:0x%x", o, dc2_read_u32(rdram, rect + o));
        q += std::snprintf(buf + q, sizeof(buf) - q, " texhead:");
        if (tex > 0x80000u && tex < 0x2000000u)
            for (uint32_t o = 0; o <= 0x40u && q < (int)sizeof(buf) - 16; o += 4u)
                q += std::snprintf(buf + q, sizeof(buf) - q, " %x:0x%x", o, dc2_read_u32(rdram, tex + o));
        std::fprintf(stderr, "%s\n", buf);
        std::fflush(stderr);
    }
    g_g34_in_divsprite.fetch_add(1, std::memory_order_relaxed);
    DrawDivSprite4__FP11mgCDrawPrim9mgRect_i_P10mgCTexturePiii_0x17cb20(rdram, ctx, runtime);
    g_g34_in_divsprite.fetch_sub(1, std::memory_order_relaxed);

    if (g36_packet_trace_enabled())
    {
        const uint32_t dump = g_g36_packet_dump.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (dump <= 3u)
        {
            const uint32_t base = dc2_read_u32(rdram, prim + 0xD4u);
            const uint32_t end = dc2_read_u32(rdram, prim + 0xDCu);
            g36_decode_divsprite_packet(rdram, dump, prim, tex, base, end);
        }
    }
}



// PHASE9: DC2 ג€” Apply all Phase 9.3 stub address bindings for Dark Cloud 2.
void applyDC2Phase9Stubs(PS2Runtime &runtime)
{
    using namespace ps2_game_overrides;

    g_g7_poll_live_pad_hook = &g7_poll_live_pad;
    g_g55_title_draw_probe_hook = &g55_title_draw_probe;
    g_f66_drive_dungeon_pad_hook = &f66_drive_dungeon_pad;

    // -------------------------------------------------------------------------
    // PHASE9: DC2 ג€” Direct function overrides (not in PS2_STUB_LIST)
    // -------------------------------------------------------------------------
    // Placement operator new/new[] must return $a1 (the pointer), not -1.
    runtime.registerFunction(0x001398E0u, f50_1_placement_new_probe); // __nw__FUiP1 (reta1 + F50.1 ctor-id probe)
    runtime.registerFunction(0x001398F0u, reta1_stub); // __nwa__FUiP1

    // 0x29fc78: unrecompiled function in TitleInit range, called by printfג†’Alloc chain.
    // Without this, the runtime enters a crash loop on missing-function recovery.
    runtime.registerFunction(0x0029FC78u, nop_stub);

    // -------------------------------------------------------------------------
    // Group D ג€” Debug console (sceDevFont / sceDevCons): return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00104288u, "memclr");
    bindAddressHandler(runtime, 0x00104D58u, "sceDevFontDefault");
    bindAddressHandler(runtime, 0x00104DE0u, "sceDevFontIdle");
    bindAddressHandler(runtime, 0x00104E20u, "sceDevFontSetColor");
    bindAddressHandler(runtime, 0x00104E58u, "sceDevConsInit");
    bindAddressHandler(runtime, 0x00104E90u, "sceDevConsOpen");
    bindAddressHandler(runtime, 0x00104F90u, "sceDevConsClose");
    bindAddressHandler(runtime, 0x00104FC8u, "sceDevConsRef");
    bindAddressHandler(runtime, 0x00105068u, "sceDevConsDraw");
    bindAddressHandler(runtime, 0x00105230u, "sceDevConsDrawS");
    bindAddressHandler(runtime, 0x001053E0u, "sceDevConsClear");
    bindAddressHandler(runtime, 0x00105438u, "sceDevConsSetColor");
    bindAddressHandler(runtime, 0x00105450u, "sceDevConsPrintf");
    bindAddressHandler(runtime, 0x001056A8u, "sceDevConsLocate");
    bindAddressHandler(runtime, 0x001056D8u, "sceDevConsPut");
    bindAddressHandler(runtime, 0x001057A8u, "sceDevConsGet");
    bindAddressHandler(runtime, 0x00105820u, "sceDevConsAttribute");
    bindAddressHandler(runtime, 0x00105828u, "sceDevConsClearBox");
    bindAddressHandler(runtime, 0x001058F0u, "sceDevConsMove");
    bindAddressHandler(runtime, 0x00105A58u, "sceDevConsRollup");
    bindAddressHandler(runtime, 0x00105B08u, "sceDevConsMessage");
    bindAddressHandler(runtime, 0x00105C28u, "sceDevConsFrame");
    bindAddressHandler(runtime, 0x00105E18u, "sceDevFontRefDirectImage");
    bindAddressHandler(runtime, 0x00105FA0u, "sceDevFontRefStrN");
    bindAddressHandler(runtime, 0x00106100u, "sceDevConsPutc");
    bindAddressHandler(runtime, 0x00106150u, "sceDevConsGetc");
    bindAddressHandler(runtime, 0x001061A0u, "sceDevConsPiece");
    bindAddressHandler(runtime, 0x00106208u, "sceDevFontKnj2Chr");
    bindAddressHandler(runtime, 0x001064F0u, "sceDevFont");

    // -------------------------------------------------------------------------
    // Group D ג€” GIF packet builders (sceGifPk*): return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00106630u, "sceGifPkInit");
    bindAddressHandler(runtime, 0x00106640u, "sceGifPkReset");
    bindAddressHandler(runtime, 0x00106650u, "sceGifPkTerminate");
    bindAddressHandler(runtime, 0x001066A8u, "sceGifPkCnt");
    bindAddressHandler(runtime, 0x00106720u, "sceGifPkRef");
    bindAddressHandler(runtime, 0x001067B8u, "sceGifPkEnd");
    bindAddressHandler(runtime, 0x00106830u, "sceGifPkReserve");
    bindAddressHandler(runtime, 0x00106848u, "sceGifPkOpenGifTag");
    bindAddressHandler(runtime, 0x00106860u, "sceGifPkCloseGifTag");
    bindAddressHandler(runtime, 0x00106908u, "sceGifPkAddGsData");
    bindAddressHandler(runtime, 0x00106930u, "sceGifPkAddGsAD");
    bindAddressHandler(runtime, 0x00106958u, "sceGifPkRefLoadImage");

    // -------------------------------------------------------------------------
    // Group D ג€” VIF1 packet builders (sceVif1Pk*): return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00106B18u, "sceVif1PkInit");
    bindAddressHandler(runtime, 0x00106B28u, "sceVif1PkReset");
    bindAddressHandler(runtime, 0x00106B38u, "sceVif1PkTerminate");
    bindAddressHandler(runtime, 0x00106B90u, "sceVif1PkCnt");
    bindAddressHandler(runtime, 0x00106BE8u, "sceVif1PkCall");
    bindAddressHandler(runtime, 0x00106C58u, "sceVif1PkEnd");
    bindAddressHandler(runtime, 0x00106CB0u, "sceVif1PkOpenDirectCode");
    bindAddressHandler(runtime, 0x00106D08u, "sceVif1PkCloseDirectCode");
    bindAddressHandler(runtime, 0x00106D38u, "sceVif1PkOpenGifTag");
    bindAddressHandler(runtime, 0x00106D50u, "sceVif1PkCloseGifTag");
    bindAddressHandler(runtime, 0x00106DF8u, "sceVif1PkReserve");
    bindAddressHandler(runtime, 0x00106E10u, "sceVif1PkAlign");
    bindAddressHandler(runtime, 0x00106E90u, "sceVif1PkAddGsAD");

    // -------------------------------------------------------------------------
    // Group D ג€” Misc internal helpers
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x0010C6A8u, "dmaRefImage");
    bindAddressHandler(runtime, 0x0010DA28u, "_system_header");
    bindAddressHandler(runtime, 0x0010FE70u, "ReleaseAlarm");
    bindAddressHandler(runtime, 0x00110330u, "EnableCache");
    bindAddressHandler(runtime, 0x00110340u, "DisableCache");
    bindAddressHandler(runtime, 0x001104B0u, "isceSifSetDma");
    bindAddressHandler(runtime, 0x001104D0u, "isceSifSetDChain");
    bindAddressHandler(runtime, 0x00110550u, "_InitTLB");
    bindAddressHandler(runtime, 0x00110E48u, "InitAlarm");
    bindAddressHandler(runtime, 0x00111B30u, "printfloat");

    // -------------------------------------------------------------------------
    // Group D ג€” Filesystem internals (_sceFs*): return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x001139A0u, "_sceFsIobSemaMK");
    bindAddressHandler(runtime, 0x00113AF8u, "_sceFs_Rcv_Intr");
    bindAddressHandler(runtime, 0x00113F08u, "_sceFsWaitS");
    bindAddressHandler(runtime, 0x00113F48u, "scePowerOffHandler");
    bindAddressHandler(runtime, 0x00113FD8u, "_sceFs_Poff_Intr");

    // -------------------------------------------------------------------------
    // Group D ג€” Extended filesystem / I/O ops: return 0 is correct for PC
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00115158u, "sceIoctl2");
    bindAddressHandler(runtime, 0x00115338u, "_sceCallCode");
    bindAddressHandler(runtime, 0x001154E0u, "sceRemove");
    bindAddressHandler(runtime, 0x00115500u, "sceMkdir");
    bindAddressHandler(runtime, 0x001156B0u, "sceRmdir");
    bindAddressHandler(runtime, 0x001156D0u, "sceFormat");
    bindAddressHandler(runtime, 0x00115940u, "sceAddDrv");
    bindAddressHandler(runtime, 0x00115A60u, "sceDelDrv");
    bindAddressHandler(runtime, 0x00115A80u, "sceDopen");
    bindAddressHandler(runtime, 0x00115B48u, "sceDclose");
    bindAddressHandler(runtime, 0x00115CB0u, "sceDread");
    bindAddressHandler(runtime, 0x00115E08u, "sceGetstat");
    bindAddressHandler(runtime, 0x00115FA8u, "sceChstat");
    bindAddressHandler(runtime, 0x001161E8u, "sceRename");
    bindAddressHandler(runtime, 0x001163D8u, "sceChdir");
    bindAddressHandler(runtime, 0x001163F8u, "sceSync");
    bindAddressHandler(runtime, 0x00116590u, "sceMount");
    bindAddressHandler(runtime, 0x00116800u, "sceUmount");
    bindAddressHandler(runtime, 0x00116820u, "sceLseek64");
    // sceDevctl is only reached from HddConectCheck/CheckInstallSpace; the name-based bind
    // cannot resolve it (not in PS2_STUB_LIST), which left the recompiled TODO wrapper live
    // and fatally throwing "Unimplemented PS2 stub called: sceDevctl". Register the real
    // no-HDD stub directly so the HDD probe fails cleanly (-11 -> retail no-HDD path).
    runtime.registerFunction(0x00116A58u, &ps2_stubs::sceDevctl);
    bindAddressHandler(runtime, 0x00116C90u, "sceSymlink");
    bindAddressHandler(runtime, 0x00116E70u, "sceReadlink");

    // -------------------------------------------------------------------------
    // Group D ג€” SIF IOP memory / module management: return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00117130u, "sceSifAllocSysMemory");
    bindAddressHandler(runtime, 0x001171B0u, "sceSifFreeSysMemory");
    bindAddressHandler(runtime, 0x00117910u, "sceSifUnloadModule");
    bindAddressHandler(runtime, 0x001179A0u, "sceSifSearchModuleByName");
    bindAddressHandler(runtime, 0x00117A40u, "sceSifSearchModuleByAddress");
    bindAddressHandler(runtime, 0x00117AF0u, "sceSifLoadStartModuleBuffer");
    bindAddressHandler(runtime, 0x00117D58u, "sceSifLoadStartModule");

    // -------------------------------------------------------------------------
    // Group D ג€” TLB management: no-op for recompiled code
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00118570u, "_SetTLBEntry");
    bindAddressHandler(runtime, 0x00118580u, "SetTLBEntry");
    bindAddressHandler(runtime, 0x001185C0u, "GetTLBEntry");
    bindAddressHandler(runtime, 0x00118610u, "InitTLB");

    // -------------------------------------------------------------------------
    // Group D ג€” CD internal callbacks: return 0 is safe
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x0011F500u, "_sceCd_cd_callback");
    bindAddressHandler(runtime, 0x0011F738u, "_sceCd_cd_read_intr");
    bindAddressHandler(runtime, 0x0011F8F0u, "sceCdPOffCallback");
    bindAddressHandler(runtime, 0x0011F960u, "_sceCd_Poff_Intr");
    bindAddressHandler(runtime, 0x0011FD08u, "_sceCd_ncmd_prechk");
    bindAddressHandler(runtime, 0x00120020u, "_sceCd_scmd_prechk");

    // -------------------------------------------------------------------------
    // Group D ג€” Memory card internals: return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00122560u, "sceMcEnd");
    bindAddressHandler(runtime, 0x00122D78u, "mcHearAlarm");
    bindAddressHandler(runtime, 0x00122DA0u, "mcDelayThread");
    bindAddressHandler(runtime, 0x001230A8u, "sceMcUdCheckNewCard");

    // -------------------------------------------------------------------------
    // Group D ג€” MIDI/sound input: return 0 is correct
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00123C10u, "sceMSIn_Init");
    bindAddressHandler(runtime, 0x00123C90u, "sceMSIn_ATick");
    bindAddressHandler(runtime, 0x00123C98u, "sceMSIn_Load");
    bindAddressHandler(runtime, 0x00123D38u, "sceMSIn_PutMsg");
    bindAddressHandler(runtime, 0x00123DE8u, "sceMSIn_PutExcMsg");
    bindAddressHandler(runtime, 0x00123E60u, "sceMSIn_PutHsMsg");

    // -------------------------------------------------------------------------
    // Group B ג€” Memory/libc helpers (real implementations in dc2_auto_stubs.cpp)
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00123EA8u, "abort");
    bindAddressHandler(runtime, 0x001264D8u, "malloc_extend_top");
    bindAddressHandler(runtime, 0x001272D8u, "__malloc_lock");
    bindAddressHandler(runtime, 0x001272E0u, "__malloc_unlock");
    bindAddressHandler(runtime, 0x00127C48u, "__mcmp");
    bindAddressHandler(runtime, 0x0012A990u, "__sprint");
    bindAddressHandler(runtime, 0x0012A9D8u, "__sbprintf");

    // -------------------------------------------------------------------------
    // Group A ג€” Math functions (real implementations in dc2_auto_stubs.cpp)
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x0012A6A8u, "strtod");
    bindAddressHandler(runtime, 0x0012A6D8u, "strtodf");
    bindAddressHandler(runtime, 0x0012A700u, "_strtol_r");
    bindAddressHandler(runtime, 0x0012A938u, "strtol");
    bindAddressHandler(runtime, 0x0012A970u, "toupper");
    bindAddressHandler(runtime, 0x0012C1D0u, "exponent");
    bindAddressHandler(runtime, 0x001297A8u, "_strtod_r");
    bindAddressHandler(runtime, 0x00123EE0u, "atof");
    bindAddressHandler(runtime, 0x00123F00u, "atoi");
    bindAddressHandler(runtime, 0x0011E2B8u, "atanf");
    bindAddressHandler(runtime, 0x0011E590u, "cosf");
    bindAddressHandler(runtime, 0x0011E678u, "fabsf");
    bindAddressHandler(runtime, 0x0011E698u, "floorf");
    bindAddressHandler(runtime, 0x0011E908u, "sinf");
    bindAddressHandler(runtime, 0x0011E9F8u, "tanf");
    bindAddressHandler(runtime, 0x0011F1D8u, "atan2f");
    bindAddressHandler(runtime, 0x0011F300u, "sqrtf");
    bindAddressHandler(runtime, 0x00286078u, "__divdi3");

    // -------------------------------------------------------------------------
    // Group C ג€” Game-specific (real implementations in dc2_auto_stubs.cpp)
    // -------------------------------------------------------------------------
    bindAddressHandler(runtime, 0x00118E50u, "InitExecPS2");
    bindAddressHandler(runtime, 0x00149240u, "GetFullPath__FPcPc");
    // strFile* (0x0029AB10..0x0029AE80) removed ג€” Phase B: let recompiled strFile
    // code run so it can reach sceCdSearchFile (now ISO-backed).
    // init__Fv must stay bound: it is a TODO stub (not recompiled) and must return 0.
    bindAddressHandler(runtime, 0x0015C160u, "init__Fv");
    bindAddressHandler(runtime, 0x0014A3D0u, "pad_button_read__FP10PAD_STATUSii");
    bindAddressHandler(runtime, 0x00186BD0u, "divby0error__Fv");
    bindAddressHandler(runtime, 0x001CBD70u, "memoryInit__Fv");
    bindAddressHandler(runtime, 0x0029B4E0u, "iopGetArea__FPiPiPiPiP8AudioDeci");
    bindAddressHandler(runtime, 0x002F4BA0u, "McError__18CMemoryCardManagerFi");
    bindAddressHandler(runtime, 0x002F5390u, "McCheckMCPs2__FP12MC_CARD_INFO");
    bindAddressHandler(runtime, 0x002F53D0u, "McCheckMCPs2Boot__FP12MC_CARD_INFOi");
    bindAddressHandler(runtime, 0x0030A290u, "SCElogoFade__FiP9mgCMemory");
    bindAddressHandler(runtime, 0x0031CE80u, "random__Fv");
    bindAddressHandler(runtime, 0x0031D760u, "abs__Ff");
    bindAddressHandler(runtime, 0x00320680u, "rand_prob__Fi");

    // _ZERO_VECTOR appears at two distinct guest addresses, same handler
    bindAddressHandler(runtime, 0x002767B0u, "_ZERO_VECTOR__FP12RS_STACKDATAi");
    bindAddressHandler(runtime, 0x002E3390u, "_ZERO_VECTOR__FP12RS_STACKDATAi");

    // Pad auto-repeat helpers
    bindAddressHandler(runtime, 0x00278620u, "_PAD_AUTO_REPEAT_OFF__FP12RS_STACKDATAi");
    bindAddressHandler(runtime, 0x00278650u, "_PAD_SET_AUTO_REPEAT__FP12RS_STACKDATAi");

    // 64-bit integer division (GCC runtime helper)
    // __divdi3 already bound above in Group A

    // HDD/partition helpers: no HDD on PC, return 0
    bindAddressHandler(runtime, 0x0031B940u, "CreateDir__FPc");
    bindAddressHandler(runtime, 0x0031BAC0u, "CheckPartition__Fv");
    bindAddressHandler(runtime, 0x0031BBC0u, "CheckAppInstall__Fv");
    bindAddressHandler(runtime, 0x0031BD50u, "MountHDDFileSystem__Fv");
    bindAddressHandler(runtime, 0x0031BE30u, "UmountHDDFileSystem__Fv");
    bindAddressHandler(runtime, 0x0031BE60u, "CheckInstallSpace__Fv");
    bindAddressHandler(runtime, 0x0031BF60u, "UninstallApp__Fv");

    // -------------------------------------------------------------------------
    // PHASE F3 ג€” Three critical auto-stubs on the SCE-logo + main-menu code path.
    // These OVERRIDE the bindAddressHandler entries above (last write wins in the
    // function map). See plans/phase-F2-diagnosis.md and plans/phase-F3-fix-log.md.
    // -------------------------------------------------------------------------
    runtime.registerFunction(0x0015C160u, f55_boot_init_stub);       // init__Fv (nop'd, but F55 runs the real InitCDFile@0x148F70)
    runtime.registerFunction(0x0030A290u, scelogofade_done_stub);    // SCElogoFade
    runtime.registerFunction(0x0014A490u, read_pad_stub);            // read_pad
    runtime.registerFunction(0x0014A3D0u, pad_button_read_stub);     // pad_button_read
    // G7-fix: SIF IOP module-management funcs whose bindAddressHandler entries do
    // not resolve to a ps2_stubs impl (so the throwing recompiled TODO body stays
    // live). Override with a return-0 stub so probing for an IOP module no longer
    // ends the process (the IOP/module subsystem is faked in this port).
    runtime.registerFunction(0x00117910u, sif_module_return_zero_stub); // sceSifUnloadModule
    runtime.registerFunction(0x001179A0u, sif_module_return_zero_stub); // sceSifSearchModuleByName
    runtime.registerFunction(0x00117A40u, sif_module_return_zero_stub); // sceSifSearchModuleByAddress
    runtime.registerFunction(0x00117AF0u, sif_module_return_zero_stub); // sceSifLoadStartModuleBuffer
    runtime.registerFunction(0x00117D58u, sif_module_return_zero_stub); // sceSifLoadStartModule
    // PHASE F41: title_mode_key_stub retired. Real TitleModeKey__Fv at 0x002A1220
    // now runs via register_functions.cpp. F40 pad injection drives confirm via R1.
    runtime.registerFunction(0x001648F0u, g129_cfg_water_vertex_probe); // G129: SPI WATER_VERTEX map-config handler
    runtime.registerFunction(0x0029FFA0u, f29_title_loop_probe);     // TitleLoop
    runtime.registerFunction(0x002A1020u, g94_title_mode_init_probe); // G94: state-1 init postcondition (projection/camera)
    runtime.registerFunction(0x002A1220u, f49_title_mode_key_probe); // TitleModeKey timing/path probe
    runtime.registerFunction(0x002A2AD0u, f49_title_mccheck_key_probe); // TitleMCCheckKey timing/path probe
    runtime.registerFunction(0x002F1FC0u, f49_mc_manager_step_probe); // CMemoryCardManager::Step timing/path probe
    runtime.registerFunction(0x0023E1B0u, f49_menu_check_push_probe); // MenuCheckPushButton path probe
    runtime.registerFunction(0x0023E2E0u, f49_convert_check_push_probe); // ConvertCheckPushButton path probe
    runtime.registerFunction(0x00232DF0u, f49_menu_main_init_probe); // MenuMainInit timing/path probe
    runtime.registerFunction(0x00233FF0u, f49_menu_main_key_probe); // MenuMainKey timing/path probe
    runtime.registerFunction(0x00251100u, g11_loadfilemenu_probe); // G11 costume sheet load
    runtime.registerFunction(0x0012DA90u, g11_enterimg_probe);     // G11 IMG -> texture-mgr entry
    runtime.registerFunction(0x0012E850u, g11_reloadtex_probe);    // G11 ReloadTexture tbp bind
    runtime.registerFunction(0x002F19A0u, f49_finish_for_mc_probe); // FinishForMC timing/path probe
    runtime.registerFunction(0x001908A0u, f49_init_save_data_probe); // InitSaveData timing/path probe
    runtime.registerFunction(0x00196BE0u, f49_get_user_data_man_probe); // GetUserDataMan path probe
    runtime.registerFunction(0x00195D40u, f50_7_get_item_file_path_probe); // F50.7: fallback cfg7 costume model paths for debug dungeon route
    runtime.registerFunction(0x0019D560u, f49_set_chr_equip_probe); // SetChrEquipDirect path probe
    runtime.registerFunction(0x001A1B80u, f49_debug_get_item_probe); // DebugGetItem path probe
    runtime.registerFunction(0x0019B490u, f49_get_chara_data_ptr_probe); // GetCharaDataPtr path probe
    runtime.registerFunction(0x002AFDB0u, f49_set_menu_load_item_no_probe); // SetMenuLoadItemNo path probe
    runtime.registerFunction(0x002B90D0u, f49_menu_item_chara_data_load_probe); // MenuItemCharaDataLoad path probe
    runtime.registerFunction(0x00148E70u, f49_read_bg_sync_probe); // ReadBGSync polling probe
    runtime.registerFunction(0x002BBC80u, f48_2_read_main_chara_bg_probe); // ReadMainCharaBG step-23 audit probe
    runtime.registerFunction(0x002BC150u, f48_2_key_main_chara_bg_probe); // KeyMainCharaBG step-23 audit probe
    runtime.registerFunction(0x002BE030u, f48_2_menu_costume_key_probe); // MenuCostumeKey step-23 audit probe
    runtime.registerFunction(0x001CBD70u, f50_1_memory_init);             // F50.1 memoryInit (was auto-stub) ג€” create dungeon mgCMemory pools
    runtime.registerFunction(0x0012E600u, f50_9_mg_load_image_probe);     // F50.9 mgLoadImage CLUT/texture upload probe (DC2_TRACE_F50_9)
    runtime.registerFunction(0x0012E850u, f51_reload_texture_packet_probe); // F51 ReloadTexture packet-wrapper reach probe (DC2_TRACE_T8_UPLOAD)
    runtime.registerFunction(0x0012E970u, f50_10_reload_texture_probe);   // F50.10 ReloadTexture post-reload TEX0 vs data/CLUT ptr probe (DC2_TRACE_F50_10)
    runtime.registerFunction(0x00134DA0u, f50_11_drawprim_texture_probe); // F50.11 mgCDrawPrim::Texture bind-source probe (DC2_TRACE_F50_11)
    runtime.registerFunction(0x0012D050u, f50_11_get_texture_probe);      // F50.11 GetTexture owner/name probe (DC2_TRACE_F50_11)
    // F50.2: sndLoadSound stub REVERTED ג€” stubbing it broke the boot sound-init
    // vsync handshake (hung earlier at mgGetVSyncCount@0x141310). Real body needed.
    // runtime.registerFunction(0x0018DA30u, f50_2_snd_load_sound_stub);
    runtime.registerFunction(0x001CC040u, f48_4_init_dungeon_main_probe); // F48.4 InitDungeonMain post-costume new-game loop init probe
    runtime.registerFunction(0x0029FF30u, f48_4_title_exit_probe);        // F49.5 TitleExit (LoopExit[3]) loop-transition bracket
    runtime.registerFunction(0x0014B600u, f48_4_capture_end_probe);       // F49.5 CaptureEnd loop-transition bracket (last call before transition sceGsSyncV)
    runtime.registerFunction(0x001CEA00u, f48_4_loop_dungeon_main_probe); // F48.4 LoopDungeonMain post-costume new-game loop frame probe
    runtime.registerFunction(0x001EFF40u, f56_treemap_step_probe);        // F56 floor-select treemap state machine probe (DC2_TRACE_F56)
    runtime.registerFunction(0x002A0AB0u, f29_title_draw_probe);     // TitleDraw
    runtime.registerFunction(0x002A1B60u, f29_title_mode_draw_probe);// TitleModeDraw
    // PHASE F38: TitleMapDraw bypass removed ג€” real recomp registered by
    // recomp/register_functions.cpp is left in effect now that F33 fixed the
    // mgCDrawPrim ctor so draw primitives initialize correctly.
    // runtime.registerFunction(0x002A2280u, title_map_draw_stub);      // TitleMapDraw
    runtime.registerFunction(0x002A2280u, f48_title_map_draw_probe);  // TitleMapDraw camera-vtable fixup + delegate
    runtime.registerFunction(0x002838C0u, g58_getcamera_title_fix);   // G58: assign title camera on demand (ordering fix; kill DC2_G58_NO_CAMFIX)
    runtime.registerFunction(0x00168FD0u, g59_get_texture_block_no_probe); // G59 title texture-block probe (DC2_TRACE_G59)
    runtime.registerFunction(0x002A2548u, g59_title_map_resume_2548); // G59 title resume repair: stack + saved return + camera s7
    runtime.registerFunction(0x002A2644u, g59_title_map_resume_2644); // G59 title resume repair: MDS reload loop continuation
    runtime.registerFunction(0x00131A20u, g59_cam_adddistance_fix);   // G59 title camera arg repair for phase-0 follow
    runtime.registerFunction(0x00131A10u, g59_cam_getdistance_fix);   // G59 title camera arg repair for phase-0 follow
    runtime.registerFunction(0x00131A50u, g59_cam_addheight_fix);     // G59 title camera arg repair for phase-0 follow
    runtime.registerFunction(0x001387F0u, g59_frame_draw_tailcall_fix); // G59 Draw__8mgCFrameFv tail-call completion
    runtime.registerFunction(0x00137E10u, g67_frame_draw_probe);      // G67 title visual 0x987230 distance-cull repair
    runtime.registerFunction(0x00135C70u, g67_frame_test1_probe);     // G67 clamp only that title frame's test1 distance result
    runtime.registerFunction(0x0012F2E0u, g67_frame_clipbox_probe);    // G67 force only that title frame's clip-box result visible
    runtime.registerFunction(0x0012F380u, g78_frame_clipinbox_fix);    // G78 force only that title frame's clip-in-box -> copy render-mode (fc4=0); kill DC2_G78_NO_COPY_FIX
    runtime.registerFunction(0x00234290u, g79_main_draw_probe);        // G79 probe: per-frame active menu id (DC2_TRACE_G79)
    runtime.registerFunction(0x002BE040u, g79_costume_draw_probe);     // G79 probe: is costume DRAW still dispatched after exit? (DC2_TRACE_G79)
    runtime.registerFunction(0x00142F90u, g59_mgdraw_frame_tailfix);  // G59 title mgDraw trace around frame-tail return
    runtime.registerFunction(0x00285670u, f48_load_map_from_memory_probe); // CScene::LoadMapFromMemory(scene,map,info)
    runtime.registerFunction(0x002856F0u, f48_load_map_from_memory2_probe); // CScene::LoadMapFromMemory(scene,map,stage,info)
    runtime.registerFunction(0x00285CE0u, f50_7_load_map_probe); // F50.7: seed missing debug-menu map stack, then CScene::LoadMap
    runtime.registerFunction(0x00164480u, f48_load_map_file_probe); // CMap::LoadMapFile
    runtime.registerFunction(0x001600D0u, f48_create_map_probe); // CMap::CreateMap
    runtime.registerFunction(0x001643A0u, f50_7_funcpoint_set_scale_probe); // F50.7: keep map-script CFuncPoint setter tailcalls resumable
    runtime.registerFunction(0x001643C0u, f50_7_funcpoint_set_rotation_probe); // F50.7: keep map-script CFuncPoint setter tailcalls resumable
    runtime.registerFunction(0x001643E0u, f50_7_funcpoint_set_position_probe); // F50.7: keep map-script CFuncPoint setter tailcalls resumable
    runtime.registerFunction(0x00143720u, f50_7_mg_active_lighting_probe); // F50.7: keep mgActiveLighting tailcall resumable
    runtime.registerFunction(0x00160B10u, f50_7_map_draw_probe); // F50.7: keep CMap::Draw tailcall resumable
    runtime.registerFunction(0x00160B30u, f50_7_map_draw_direct_probe); // F50.7: keep CMap::DrawDirect tailcall resumable
    runtime.registerFunction(0x0015E3D0u, g56_mapparts_draw_tap);   // G56 Draw__9CMapParts (part vt+0x34) chain tap
    runtime.registerFunction(0x00166A00u, g56_mapparts_predraw_tap);// G56 PreDraw__9CMapParts (part vt+0x40 gate) chain tap
    runtime.registerFunction(0x00166E40u, g56_mappiece_draw_tap);   // G56 Draw__9CMapPiece (piece vt+0x34, geometry emit) chain tap
    runtime.registerFunction(0x00135230u, g56_begindraw_tap);       // G56 BeginDraw__14mgCDrawManager mempool/alloc tap
    // G57: confirmed via temporary detector taps that Draw__12mgCVisualMDT IS drawn 6x (the
    // title model geometry submits) and DrawWater returns clean. The DrawWater skip below
    // ruled the water draw OUT as the $ra-corruption source (see g57_drawwater_skip).
    runtime.registerFunction(0x0015E800u, g57_drawwater_skip);      // G57 DrawWater skip bisection (DC2_G57_SKIP_WATER)
    // G58: $ra-corruption canary over the title geometry-queue chain. Registered LAST so it
    // wins the function table over the G56/F50.7 taps it wraps. Only installed under
    // DC2_TRACE_G58 so the default dispatch table (and golden title smoke) is untouched.
    if (dc2_env_flag_enabled("DC2_TRACE_G58"))
    {
        runtime.registerFunction(0x00160B10u, g58_canary_map_draw);       // Draw__4CMap (wraps f50_7 tailfix)
        runtime.registerFunction(0x001B4130u, g58_canary_editmap_drawsub);// DrawSub__8CEditMap
        runtime.registerFunction(0x0015E250u, g58_canary_map_drawsub);    // DrawSub__4CMap (loads s0=*(map+0x364))
        runtime.registerFunction(0x0015E3D0u, g58_canary_parts_draw);     // Draw__9CMapParts (wraps g56 tap)
        runtime.registerFunction(0x00166A70u, g58_canary_parts_drawsub);  // DrawSub__9CMapParts
        runtime.registerFunction(0x00166E40u, g58_canary_piece_draw);     // Draw__9CMapPiece (wraps g56 tap)
        runtime.registerFunction(0x00169FD0u, g58_canary_objframe_draw);  // Draw__12CObjectFrame
        runtime.registerFunction(0x00137E10u, g58_canary_mgframe_draw);   // Draw__8mgCFrame
        runtime.registerFunction(0x0013F4E0u, g58_canary_visualmdt_draw); // Draw__12mgCVisualMDT
        runtime.registerFunction(0x00131A20u, g58_cam_adddistance);       // AddDistance (r31/a0 probe)
        runtime.registerFunction(0x00131A10u, g58_cam_getdistance);       // GetDistance (r31/a0 probe)
        runtime.registerFunction(0x00131A50u, g58_cam_addheight);         // AddHeight (r31/a0 probe)
        runtime.registerFunction(0x00283740u, g58_assign_camera_probe);   // AssignCamera args/result probe
    }
    runtime.registerFunction(0x0028BE40u, f50_7_random_circle_step_probe); // F50.7: repair CRandomCircle global vtable and resume tailcall
    runtime.registerFunction(0x001EA890u, f55_calc_glid_put_pos_probe); // F55: CDngFreeMap::CalcGlidPutPos A/B input/output probe (DC2_TRACE_F55)
    runtime.registerFunction(0x001EF9F0u, f55_init_end_probe);          // F55: CMenuTreeMap::InitEnd reach probe (DC2_TRACE_F55)
    runtime.registerFunction(0x001EABE0u, f55_set_texture_info_probe);  // F55: CDngFreeMap::SetTextureInfo result probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00149CD0u, f55_get_pack_file_probe);     // F55: GetPackFile name/result probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00148C70u, f55_get_read_bg_file_probe);  // F55: GetReadBGFile(i) result probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00148930u, f55_load_file_bg_probe);      // F55: LoadFileBG name/result probe (DC2_TRACE_F55)
    runtime.registerFunction(0x001488D0u, f55_init_read_bg_probe);      // F55: InitReadBG slot-clear probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00148CC0u, f55_start_read_bg_probe);     // F55: StartReadBG slot-clear probe (DC2_TRACE_F55)
    runtime.registerFunction(0x00191970u, f50_6_menu_init_probe);     // MenuInit debug-menu probe
    runtime.registerFunction(0x00191C30u, f49_menu_loop_probe);       // MenuLoop timing probe
    runtime.registerFunction(0x00233FC0u, f49_menu_main_loop_probe);  // MenuMainLoop timing probe
    runtime.registerFunction(0x00309DE0u, f49_pause_loop_probe);      // PauseLoop timing probe
    runtime.registerFunction(0x0014A930u, f49_update_gamepad_probe);  // UpDate hotspot probe
    runtime.registerFunction(0x002ED550u, f49_update_pad_control_probe); // CPadControl::Update hotspot probe
    runtime.registerFunction(0x0018D650u, f49_snd_step_probe);        // sndStep hotspot probe
    runtime.registerFunction(0x002A7940u, f49_scene_step_snd_probe);  // CScene::StepSnd hotspot probe
    runtime.registerFunction(0x0021FE60u, f29_primquad_drawprim_probe);
    runtime.registerFunction(0x0021FF30u, f29_primquad_texture_probe);
    runtime.registerFunction(0x0012C480u, mgctexture_ct_stub);       // mgCTexture ctor
    runtime.registerFunction(0x0012C6D0u, mgctextureblock_ct_stub);  // mgCTextureBlock ctor
    runtime.registerFunction(0x0012C7E0u, mgctexturemanager_ct_stub); // mgCTextureManager ctor
    runtime.registerFunction(0x002370B0u, basemenuclass_ct_stub);    // CBaseMenuClass ctor
    runtime.registerFunction(0x002BC500u, menucostumesel_ct_stub);   // CMenuCostumeSel ctor
    runtime.registerFunction(0x00138850u, mgcdrawenv_ct_stub);       // mgCDrawEnv ctor
    runtime.registerFunction(0x001343A0u, mgcdrawprim_ct_stub);      // mgCDrawPrim ctor
    runtime.registerFunction(0x00135180u, mgcdrawmanager_ct_stub);   // mgCDrawManager ctor
    runtime.registerFunction(0x00135B60u, mgcframeattr_ct_stub);     // mgCFrameAttr ctor
    runtime.registerFunction(0x00136490u, mgcframe_ct_stub);         // mgCFrame ctor
    runtime.registerFunction(0x0013D260u, mgctextureanime_ct_stub);  // mgCTextureAnime ctor
    runtime.registerFunction(0x0017D1F0u, mgc3dsprite_ct_stub);      // mgC3DSprite ctor
    runtime.registerFunction(0x001344A0u, f29_begin_prim_probe);     // Begin mgCDrawPrim
    runtime.registerFunction(0x00134690u, f29_end_prim_probe);       // End mgCDrawPrim
    runtime.registerFunction(0x00134C80u, f33_color_probe);          // Color mgCDrawPrim
    runtime.registerFunction(0x00134DA0u, f33_texture_probe);        // Texture mgCDrawPrim
    runtime.registerFunction(0x00142560u, f51_mgenddraw_reload_probe); // F51 mgEndDrawReloadTexture reach probe (DC2_TRACE_T8_UPLOAD)
    runtime.registerFunction(0x001425B0u, f29_mgendframe_probe);     // mgEndFrame
    runtime.registerFunction(0x00141200u, f49_vsync_callback_probe); // VSyncCallBack timing/counter probe
    runtime.registerFunction(0x001412A0u, f49_waitvsync_probe);      // WaitVSync (F49 pacing probe + clamp)
    runtime.registerFunction(0x00103300u, f49_scegssyncv_probe);     // sceGsSyncV wait/callsite probe
    runtime.registerFunction(0x001041E8u, f49_syncv_callback_set_probe); // sceGsSyncVCallback set/unset probe
    // PHASE G15: deform-mesh build-path probes (only when DC2_TRACE_G15 is set, so default
    // smoke is byte-identical). Pass-through wrappers measuring mgVif1Packet growth.
    if (g15_trace_enabled())
    {
        runtime.registerFunction(0x00137E10u, g18_frame_draw_probe);  // G18: Draw__8mgCFrame (mesh build gate)
        runtime.registerFunction(0x0016B940u, g15_actor_draw_probe);  // DrawDirect__12CActionChara
        runtime.registerFunction(0x001731F0u, g15_char_draw_probe);   // DrawDirect__11CCharacter2
        runtime.registerFunction(0x00142FD0u, g15_mgdrawdirect_probe);// mgDrawDirect__FP8mgCFrame
        runtime.registerFunction(0x0012F2E0u, g15_clipbox_probe);    // mgClipBoxW (cull gate)
        runtime.registerFunction(0x0013F4E0u, g15_mdt_probe);        // Draw__12mgCVisualMDT (mesh emit)
        runtime.registerFunction(0x00145E80u, g15_sendvuprog_probe); // mgSendVuProg (VU prog upload)
    }
    // PHASE G16/G30/G99: CreateRenderInfoPacket probes (quiet unless explicitly requested).
    if (g16_trace_enabled() || g30_trace_enabled() || g99_ri_trace_enabled())
    {
        runtime.registerFunction(0x001404D0u, g16_createpacket_probe);  // CreateRenderInfoPacket
    }
    if (g43_face_trace_enabled())
    {
        runtime.registerFunction(0x0013FF60u, g43_create_face_packet_probe); // G43 exact face-packet input/output dump
        runtime.registerFunction(0x0013F4E0u, g43_mdt_face_dump_probe); // G43 face-node/packet A/B dump
    }
    else if (g16_trace_enabled())
    {
        runtime.registerFunction(0x0013F4E0u, g16_mdt_listwalk_probe);  // Draw__12mgCVisualMDT deform-list walk
    }
    // G95: title copy-path TEX0 rebinding proof wrapper. Default behavior delegates
    // to the original draw loop; DC2_G95_COPY_TEX0/DC2_G95_REPLAY_DRAW enable A/B.
    runtime.registerFunction(0x00135720u, g91_drawmgr_flush_probe);
    if (g67_trace_enabled() || g95_add_mini_tex0_enabled() || g135_src_trace_enabled())
        runtime.registerFunction(0x001359D0u, g67_addpacket_probe);     // G67 trace / G95 mini-packet proof / G135 source-chain trace
    if (g67_trace_enabled())
    {
        runtime.registerFunction(0x00137E10u, g67_frame_draw_probe);  // G67 title mgCFrame cull/queue trace
        runtime.registerFunction(0x00135C70u, g67_frame_test1_probe);  // G67 title distance-cull trace
        runtime.registerFunction(0x0012F2E0u, g67_frame_clipbox_probe); // G67 title clip-box cull trace
        runtime.registerFunction(0x0015E250u, g67_map_drawsub_probe);   // G67 title CMap draw-list trace
        runtime.registerFunction(0x0015E3D0u, g67_mapparts_draw_probe);  // G67 title CMapParts queue-delta trace
        runtime.registerFunction(0x00168FD0u, g67_get_texture_block_no_probe); // G67 TitleMapDraw texture-block list trace
        runtime.registerFunction(0x0013F4E0u, g67_mdt_title_probe);     // G67 title VisualMDT material/TEX0 queue trace
        runtime.registerFunction(0x00142560u, g67_mgenddraw_reload_probe); // G67 title block reload trace
        runtime.registerFunction(0x00142580u, g67_mgenddraw_probe);     // G67 title block draw trace
    }
    // PHASE G34: costume model RTT->display composite gap. On the costume route,
    // wrap the character draw (reads/optionally restores the +0x100 outline level)
    // and the COutLineDraw composite gate (captures fVar13). Registered last so the
    // g34 wrapper wins 0x1731f0 over the g15 trace probe when both flags are set.
    if (g9_costume_enabled())
    {
        runtime.registerFunction(0x001731F0u, g34_char_draw_probe);     // DrawDirect__11CCharacter2
        runtime.registerFunction(0x0017C2D0u, g34_outline_draw_probe);  // Draw__12COutLineDraw::Draw
        runtime.registerFunction(0x00143160u, g37_getdrawrect_probe);   // mgGetDrawRect (G37 bbox repair)
        runtime.registerFunction(0x0017CB20u, g34_divsprite_probe);     // DrawDivSprite4 (RTT composite)
        if (g36_packet_trace_enabled())
        {
            runtime.registerFunction(0x00134B00u, g36_directdata_probe); // DirectData cursor trace
            runtime.registerFunction(0x00134940u, g36_endprim2_probe);   // EndPrim2 packet-finalize trace
        }
    }
    // PHASE F38: FinishForMC nop_stub removed ג€” real recomp
    // FinishForMC__18CMemoryCardManagerFv_0x2f19a0 registered by
    // recomp/register_functions.cpp is left in effect. Title->menu handoff
    // proceeds through the real memory-card finalize path.
    // runtime.registerFunction(0x002F19A0u, nop_stub);                 // FinishForMC

    // PHASE F22: mgCCamera ctor ג€” set vtable + scalar fields so MenuCamInit's
    // `*(*(cam+0x60)+0x10)` virtual dispatch resolves to a real PS2 address.
    runtime.registerFunction(0x00131630u, mgccamera_ct_stub);
    // PHASE F42: mgCCameraFollow ctor ג€” auto-stub left vtable at this+0x60 as 0,
    // causing TitleMapDraw's GetCamera virtual dispatch to jump to 0x33313237.
    runtime.registerFunction(0x00131A90u, mgccamerafollow_ct_stub);
    // MenuCamInit override skips the tail-call virtual dispatch that would
    // re-enter MenuMainInit at a non-entry PC (0x232EE8).
    runtime.registerFunction(0x00234540u, menucaminit_stub);

    // PHASE F23c ג€” libc 64-bit / FP conversion helpers. Each shim implements
    // the canonical math against the verified PS2 EE single-register-per-
    // 64-bit-value ABI. The __divdi3 binding overrides the prior Group-A
    // name binding above (last-write-wins).
    runtime.registerFunction(0x002875E8u, umoddi3_stub);
    runtime.registerFunction(0x00286950u, moddi3_stub);
    runtime.registerFunction(0x00286078u, divdi3_stub);
    runtime.registerFunction(0x00287018u, udivdi3_stub);
    runtime.registerFunction(0x00286FB8u, muldi3_stub);
    runtime.registerFunction(0x00286768u, fixdfdi_stub);
    runtime.registerFunction(0x002868B8u, floatdidf_stub);

    // PHASE F25: silence empty-stem map\.{map,cfg,mpk,sky,ipk,efp} boot errors.
    // Filter at LoadFile2 (the centralised file-open wrapper per F19's
    // ra=0x149688 finding) ג€” LoadMapFile_0x164480 is never reached in the
    // current 30 s boot window so a shim at that level wouldn't fire.
    //
    // PHASE F42: re-enabled for map-entry probe iteration. F42 fixes the
    // TitleMapDraw vtable crash so the title can now progress further; the
    // [F39:lf2] probe may fire. Reverted at F42 close if still unreachable,
    // or promoted to permanent if a non-empty stem is observed.
    runtime.registerFunction(0x00149370u, loadfile2_skip_empty_map_stub);

    // PHASE F14 ג€” WaitEnable__8CGamePadFv (0x14A830) busy-waits for pad-state
    // transition via scePadGetState/read_pad. With our headless backend the
    // loop has no source of motion (scePadGetState sticky-stable at 6,
    // read_pad returns 0). Return immediately so MainLoop can advance to
    // mgEndFrame. See plans/phase-F14-fix-log.md.
    // PHASE F15: dc2_game_override.cpp is now in the ps2_runtime CMake source list.
    // The WaitEnable early-return in recomp/WaitEnable__8CGamePadFv_0x14a830.cpp
    // is still the active F14 fix (kept for clarity; identical effect to nop_stub).

    // -------------------------------------------------------------------------
    // PHASE F5 ג€” DVD path prefix fix + Exit halt
    // -------------------------------------------------------------------------
    runtime.registerFunction(0x00149240u, getfullpath_shim);
    // Exit_0x118FB0 uses a tail-j to _Exit (0x10FD00) so 0x10FD00 is a direct C++ call
    // in the runner, not a dispatch-table lookup. Bind the Exit wrapper itself instead.
    runtime.registerFunction(0x00118FB0u, exit_halt_stub);
}

// PHASE F9: DC2 ג€” Apply ezMidi audio-compat layer (sister-fork port).
// rpcSid 0x00012346 is what DC2's EE-side ezMidi wrapper binds to.
void applyDC2EzMidiCompat(PS2Runtime &runtime)
{
    PS2EzMidiCompatLayout layout{};
    layout.rpcSid = 0x00012346u;
    layout.reportedFreeIopBytes = 0x00200000u;
    layout.defaultPortVolume = 0x00000100u;
    layout.reportedVoiceCount = 0x00000020u;
    runtime.iop().setEzMidiCompatLayout(layout);
}

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

// PHASE G7: poll the live host controller (XInput via raylib) once per host present
// frame and publish the snapshot the guest pad read consumes. Runs on the raylib/main
// thread (called from the ps2_runtime.cpp present loop) where raylib input state is
// valid. Global linkage so ps2_runtime.cpp can call it; reads the anonymous-namespace
// snapshot globals (same-TU visibility, like f66_drive_dungeon_pad). Marker:
// G7_XINPUT_LIVE.
extern "C" bool dc2_poll_host_pad(bool, uint16_t *, uint8_t *, uint8_t *,
                                  uint8_t *, uint8_t *);

// G55: per-present-frame title map draw-state probe (called from the host present loop in
// ps2_runtime.cpp). The TitleMapDraw@0x2a2280 override only fires on INDIRECT dispatch, so it
// cannot sample steady state. PreDraw__4CMap@0x15d7a0 gates the WHOLE draw-list rebuild on a
// map-level box test: it runs only if (map+0x334==0) OR mgInsideScreen(map+0x340). If that map
// box projects off-screen, PreDraw early-outs and the per-frame draw list (map+0x360) stays 0
// -> nothing draws even though every part has a visual + bbox. Gated DC2_TRACE_G53. Global
// linkage (outside the anonymous namespace) so ps2_runtime.cpp can call it.
void g55_title_draw_probe(uint8_t *rdram)
{
    if (!g53_trace_enabled())
        return;
    static uint32_t frames = 0u, logs = 0u;
    const uint32_t f = ++frames;
    if (logs >= 120u || (f % 120u) != 0u)
        return;
    const uint32_t titleMap = dc2_read_u32(rdram, 0x00377E34u);
    if (!titleMap)
        return;
    const uint32_t boxEnable = dc2_read_u32(rdram, titleMap + 0x334u);
    const uint32_t drawList = dc2_read_u32(rdram, titleMap + 0x360u);
    const uint32_t mBase = dc2_read_u32(rdram, titleMap + 0x32Cu);
    const uint32_t mCnt = dc2_read_u32(rdram, titleMap + 0x330u);
    auto rf = [&](uint32_t a){ uint32_t u = dc2_read_u32(rdram, a); float v; std::memcpy(&v,&u,4); return v; };
    uint32_t visParts = 0u;
    for (uint32_t i = 0u; i < mCnt && i < 6u; ++i)
        if (dc2_read_u32(rdram, mBase + i * 0x310u + 0x2F8u)) ++visParts;
    // map+0x340 = mgVu0FBOX: +0x00 max.xyzw, +0x10 min.xyzw (G37 convention).
    std::fprintf(stderr,
                 "[G55:titledraw] f=%u TitleMap=0x%x boxEnable(+0x334)=0x%x drawList(+0x360)=%u visParts=%u/%u box.max=(%.1f,%.1f,%.1f) box.min=(%.1f,%.1f,%.1f)\n",
                 f, titleMap, boxEnable, drawList, visParts, mCnt,
                 rf(titleMap + 0x340u), rf(titleMap + 0x344u), rf(titleMap + 0x348u),
                 rf(titleMap + 0x350u), rf(titleMap + 0x354u), rf(titleMap + 0x358u));
    std::fflush(stderr);
    ++logs;
}

void g7_poll_live_pad()
{
    static const bool noXInput  = dc2_env_flag_enabled("DC2_NO_XINPUT");
    static const bool allowKbd  = dc2_env_flag_enabled("DC2_KEYBOARD");
    static const bool explicitS = f40_explicit_script();
    if (noXInput || explicitS)
    {
        g_pad_live_connected = false;
        return;
    }

    uint16_t mask = 0u;
    uint8_t lx = 0x80u, ly = 0x80u, rx = 0x80u, ry = 0x80u;
    const bool connected = dc2_poll_host_pad(allowKbd, &mask, &lx, &ly, &rx, &ry);

    // rawMask = standard active-high scePad bits from dc2_poll_host_pad, BEFORE any
    // byte-swap correction. This is the value the scripted replay path uses (f40_btn_bit
    // maps the same standard scePad bit positions), so the recorder logs rawMask so that
    // replay_from_rec.py ג†’ DC2_PAD_INPUT produces bit-identical replay behaviour.
    const uint16_t rawMask = mask;

    // G7-fix: the live button word reaches the game byte-swapped relative to the
    // documented scePad layout (verified on real play: physical Cross 0x4000 acted as
    // D-pad Down 0x40, Triangle 0x1000 as Up 0x10, R1 0x800 as Start 0x8 ג€” a clean
    // high/low byte swap). Swap the two button bytes so the game reads the intended
    // scePad bits. Only the live path is corrected; the scripted F40/F66 paths (and
    // their trial-tuned aliases / golden smoke) are left untouched. Disable with
    // DC2_PAD_NOSWAP=1 if a controller is found that does not need it.
    static const bool noSwap = dc2_env_flag_enabled("DC2_PAD_NOSWAP");
    if (!noSwap)
        mask = static_cast<uint16_t>(((mask & 0x00FFu) << 8) | ((mask & 0xFF00u) >> 8));

    g_pad_live_mask = mask;
    g_pad_live_lx = lx; g_pad_live_ly = ly;
    g_pad_live_rx = rx; g_pad_live_ry = ry;
    g_pad_live_connected = connected;

    // Input recorder (DC2_REC_INPUT=1): log every live-pad CHANGE with the front-end
    // script-frame (g_f40_frame_counter, the same clock DC2_PAD_INPUT replays against), a
    // wall-clock ms stamp, the standard scePad active-high mask (rawMask, pre-swap), and the
    // four analog axes. Lets a live play session be replayed headless via DC2_PAD_INPUT.
    // File: DC2_REC_INPUT_FILE or default captures/input_rec.txt.
    // Decode bits: Select1 L3-2 R3-4 Start8 Up10 Right20 Down40 Left80
    // L2-100 R2-200 L1-400 R1-800 Tri-1000 Circle-2000 Cross-4000 Square-8000.
    if (connected)
    {
        static const bool recInput = dc2_env_flag_enabled("DC2_REC_INPUT");
        if (recInput)
        {
            static std::FILE *recF = nullptr;
            static auto recT0 = std::chrono::steady_clock::now();
            static uint16_t lastMask = 0xFFFFu;
            static uint8_t lastLx = 0x80u, lastLy = 0x80u, lastRx = 0x80u, lastRy = 0x80u;
            static bool recOpenTried = false;
            if (!recF && !recOpenTried)
            {
                recOpenTried = true;
                const char *path = std::getenv("DC2_REC_INPUT_FILE");
                recF = std::fopen(path ? path : "captures/input_rec.txt", "w");
                if (recF)
                    std::fprintf(recF, "# DC2 live input recording. f=scriptFrame ms=wallclock "
                                       "mask=scePad(active-high) lx ly rx ry. Replay via DC2_PAD_INPUT keyed on f.\n");
            }
            const bool changed = (rawMask != lastMask) || (lx != lastLx) || (ly != lastLy) ||
                                 (rx != lastRx) || (ry != lastRy);
            if (recF && changed)
            {
                const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - recT0).count();
                std::fprintf(recF, "f=%u ms=%lld mask=0x%04x lx=0x%02x ly=0x%02x rx=0x%02x ry=0x%02x\n",
                             g_f40_frame_counter, ms, static_cast<unsigned>(rawMask), lx, ly, rx, ry);
                std::fflush(recF);
                lastMask = rawMask; lastLx = lx; lastLy = ly; lastRx = rx; lastRy = ry;
            }
        }
    }

    static const bool trace = dc2_env_flag_enabled("DC2_TRACE_PAD_INPUT");
    if (trace && connected)
    {
        static uint32_t c = 0u;
        if ((mask != 0u) || (c++ % 120u) == 0u)
            std::fprintf(stderr,
                "[G7:live] connected=1 mask=0x%04x lx=0x%02x ly=0x%02x rx=0x%02x ry=0x%02x\n",
                static_cast<unsigned>(mask), lx, ly, rx, ry);
    }
}

// PHASE9: DC2 ג€” Register the Phase 9.3 stub override.
// The actual ELF filename on disc is SCUS_972.13 (not dc2.elf).
PS2_REGISTER_GAME_OVERRIDE(
    "DC2 Phase 9.3 stub address bindings",
    "SCUS_972.13",
    0u,
    0u,
    applyDC2Phase9Stubs)

// PHASE F9: DC2 ג€” Register ezMidi audio-compat override.
PS2_REGISTER_GAME_OVERRIDE(
    "Dark Cloud 2 ezMidi compat",
    "SCUS_972.13",
    0u,
    0u,
    applyDC2EzMidiCompat)
