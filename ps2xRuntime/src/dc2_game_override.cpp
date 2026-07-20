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
#include <string>
#include <algorithm>
#include <thread>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// G182: EE thread CPU-time (user+kernel), same technique as G151/G156's worker-side
// GetThreadTimes probe -- called from f29_mgendframe_probe, which runs ON the EE thread, so
// GetCurrentThread() here is the EE thread itself. Lets us tell whether the measured VIF1(EE)
// wall-time (G146:perf) is real CPU work or scheduler/cache stall, before chasing a dispatch-
// overhead lever inside the interpreter.
static uint64_t g182ThreadCpuNs()
{
    FILETIME cr, ex, kt, ut;
    if (!GetThreadTimes(GetCurrentThread(), &cr, &ex, &kt, &ut))
        return 0ull;
    const uint64_t k = ((uint64_t)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
    const uint64_t u = ((uint64_t)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
    return (k + u) * 100ull; // FILETIME is 100 ns units -> ns
}
#else
static uint64_t g182ThreadCpuNs() { return 0ull; }
#endif

// G183: statistical PC-sampling profiler. G182 found EE is 90-99% on-CPU with 92-93% of
// that time inside the single mgEndFrame call -- but mgEndFrame is translated GUEST code
// (a deep inlined C++ call tree with no returns to instrument), so there are no free
// per-region timers the way there are for runtime code. Instead, sample the LIVE
// per-instruction ctx->pc (same technique F50.2's hang-watch used, and the same cross-
// thread unsynchronised-scalar-read precedent as F50.2/F66/G151/G156) from a dedicated
// host thread at high frequency WHILE mgEndFrame executes on the EE thread, and histogram
// hot addresses. Symbolize offline against ref/functions + ref/index -- this profiler
// only needs to name the addresses, not embed a symbolizer. Default-off (DC2_G183_PCSAMPLE=1),
// zero cost when unset (thread never spawned).
static std::atomic<bool> g_g183Sampling{false};
static std::atomic<R5900Context *> g_g183Ctx{nullptr};
static std::mutex g_g183HistMutex;
static std::unordered_map<uint32_t, uint64_t> g_g183Hist;

static void g183DumpAndReset(uint64_t total)
{
    std::vector<std::pair<uint32_t, uint64_t>> sorted;
    {
        std::lock_guard<std::mutex> lock(g_g183HistMutex);
        sorted.assign(g_g183Hist.begin(), g_g183Hist.end());
        g_g183Hist.clear();
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    std::fprintf(stderr, "[G183:pcsample] total=%llu distinctPc=%zu (top 20)\n",
                 (unsigned long long)total, sorted.size());
    for (size_t i = 0; i < sorted.size() && i < 20; ++i)
    {
        std::fprintf(stderr, "[G183:pcsample]   pc=0x%08x count=%llu (%.1f%%)\n",
                     sorted[i].first, (unsigned long long)sorted[i].second,
                     100.0 * (double)sorted[i].second / (double)std::max<uint64_t>(1, total));
    }
    // Full histogram, overwritten each flush (never appended) so it always reflects the
    // most recent window -- consumed offline against ref/functions/ref/index.
    if (FILE *f = std::fopen("captures/g183_pcsample.csv", "w"))
    {
        std::fprintf(f, "pc,count\n");
        for (const auto &kv : sorted)
            std::fprintf(f, "0x%08x,%llu\n", kv.first, (unsigned long long)kv.second);
        std::fclose(f);
    }
}

static void g183SamplerThreadMain()
{
    std::unordered_map<uint32_t, uint64_t> local;
    uint64_t localCount = 0;
    uint64_t windowTotal = 0;
    auto lastFlush = std::chrono::steady_clock::now();
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        if (g_g183Sampling.load(std::memory_order_relaxed))
        {
            R5900Context *ctx = g_g183Ctx.load(std::memory_order_relaxed);
            if (ctx != nullptr)
            {
                ++local[ctx->pc];
                ++localCount;
            }
        }
        if (localCount >= 64)
        {
            std::lock_guard<std::mutex> lock(g_g183HistMutex);
            for (auto &kv : local)
                g_g183Hist[kv.first] += kv.second;
            windowTotal += localCount;
            local.clear();
            localCount = 0;
        }
        const auto now = std::chrono::steady_clock::now();
        if (windowTotal > 0 &&
            std::chrono::duration<double>(now - lastFlush).count() >= 5.0)
        {
            g183DumpAndReset(windowTotal);
            windowTotal = 0;
            lastFlush = now;
        }
    }
}

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
extern std::atomic<uint64_t> g_g171RegNs[16];
extern std::atomic<uint64_t> g_g171RegCount[16];
extern std::atomic<uint64_t> g_g171AdNs[256];
extern std::atomic<uint64_t> g_g171AdCount[256];

void g7_poll_live_pad();
void g55_title_draw_probe(uint8_t *rdram);
bool f66_drive_dungeon_pad(uint32_t);

// PHASE F25: forward-decl for delegation when path is not the empty-stem map pattern.
// Defined in recomp/LoadFile2__FPcPvPii_0x149370.cpp.
extern void LoadFile2__FPcPvPii_0x149370(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void EditDraw__Fv_0x1ae3d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void EditLoop__Fv_0x1abcf0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern bool g_g186SpBalArmed; // ps2_runtime.cpp — G186 sp-balance logging armed while inside EditDraw
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
extern void GetPoly__8CEditMapFiP6CCPolyR9mgVu0FBOXi_0x1b0780(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Step__14CCameraControlFi_0x2ec110(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetFollow__15mgCCameraFollowFfff_0x131990(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DngMainDraw__Fv_0x1cf090(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleModeDraw__Fv_0x2a1b60(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TitleMapDraw__Fv_0x2a2280(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G57: scoped back-edge preemption suppression (defined in ps2_runtime.cpp). Held > 0
// while the title-frame draw runs so the recompiled deferred-draw bodies, which the
// title override invokes as direct C++ calls, run to completion instead of leaking a
// mid-loop preemption resume PC (which left BeginDraw's mgr[4]/mgr[6] null ג†’ flush crash).
extern std::atomic<int> g_dc2PreemptSuppressDepth;
// G214: extern-visible host present-loop tick (defined in ps2_runtime.cpp) so the skin-matrix
// collapse scanner can print the frame_NNNNNN.ppm number an event lands on.
extern std::atomic<uint64_t> g_dc2PresentTick;
// G56: main-title map geometry-submission chain (delegated by the G56 chain taps).
extern void Draw__9CMapPartsFv_0x15e3d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void PreDraw__9CMapPartsFv_0x166a00(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Draw__9CMapPieceFv_0x166e40(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void BeginDraw__14mgCDrawManagerFP9mgCMemoryPi_0x135230(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G57: DrawWater (last call in TitleMapDraw) ג€” wrapped by g57_drawwater_skip bisection tool.
extern void DrawWater__4CMapFP9mgCCameraP10mgCTextureP10mgCTexture_0x15e800(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// G194: town-scene effect-pass bisection targets (giant dark overlay tris sampling 0x28c0/T8).
extern void DrawEffect__6CSceneFi_0x2c8820(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DepthOfField__FiPfP10mgCTexturef_0x17e320(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void TexAnime__15mgCTextureAnimeFiP13sceVif1Packet_0x13bd90(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgSetPkMoveImage__FP10mgCTexture9mgRect_i_P10mgCTextureiii_0x144560(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DrawEffect__8CEditMapFv_0x29c1c0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DrawEffect__4CMapFv_0x15e3f0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
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
// G193: town/edit-map camera diagnosis (scene->Initialize ordering vs EditInit's AssignCamera).
extern void Initialize__6CSceneFv_0x282ea0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void EditInit__F13INIT_LOOP_ARG_0x1a9f40(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
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
// PHASE G205: field-character (non-"Direct") draw entry, town Max candidate.
extern void Draw__12CActionCharaFv_0x16b850(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DrawDirect__11CCharacter2Fv_0x1731f0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgGetDrawRect__FP8mgCFrameP9mgVu0FBOX_0x143160(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Draw__8mgCFrameFPUi_0x137e10(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void SetDeformMesh__11CCharacter2Fv_0x1730b0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// PHASE G209: per-frame world (skin) matrix builder, test of G207/G208's skin-matrix hypothesis.
extern void GetLWMatrix__8mgCFrameFPA4_f_0x137030(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
// PHASE G34: costume model RTT->display composite (the outline/preview pass).
extern void Draw__12COutLineDrawFff_0x17c2d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CheckHit__10CDAColPipeFPf_0x17c090(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void DrawDivSprite4__FP11mgCDrawPrim9mgRect_i_P10mgCTexturePiii_0x17cb20(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgClipBoxW__FPfPfPfPf_0x12f2e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgClipInBoxW__FPfPfPfPf_0x12f380(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void test1__FPA4_fPA4_fPA4_fPfPf_0x135c70(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void mgSendVuProg__FPUii_0x145e80(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void AddPacket__14mgCDrawManagerFiP1P1i_0x1359d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CreateFacePacket__12mgCVisualMDTFPUiP7mgCFace_0x13ff60(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CreatePacket__12mgCVisualMDTFP14mgCDrawManager_0x13f6a0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CreatePacket__15mgCVisualFixMDTFP14mgCDrawManager_0x13f920(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CreateRenderInfoPacket__12mgCVisualMDTFPUiPA4_fP13mgRENDER_INFO_0x1404d0(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
extern void CreateRenderInfoPacket__18mgCVisualMotionMDTFPUiPA4_fP13mgRENDER_INFO_0x28a660(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime);
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

// G215: expose the COutLineDraw character-draw window to the VU1 interpreter so its
// per-model-batch cull discriminator (DC2_G215_BATCH) can scope model executes to the
// character pass. Only meaningful when the outline wrapper is registered (DC2_G205_FVAR13_TRACE
// or DC2_G9_COSTUME); otherwise g_g37_in_outline stays 0 and the probe emits nothing.
bool dc2_g215_in_char_outline() { return g_g37_in_outline.load(std::memory_order_relaxed) > 0; }

// G89: defined in ps2_gs_rasterizer.cpp; set by g67_title_scope each frame so the rasterizer's
// title-rock guard-band cull only fires in the title-map scene. Declared at global scope (external
// linkage) -- declaring it inside the anonymous namespace below would give it internal linkage.
extern std::atomic<bool> g_dc2TitleRockScope;
extern std::atomic<bool> g_dc2TownDepthScope;
// G90: current logical title block flushing through mgEndDraw (rasterizer-side, [G88:geo] tag).
extern std::atomic<int> g_dc2TitleCurBlock;
// G144: defined in ps2_gs_rasterizer.cpp; drains trailing deferred tile-binning triangles at frame
// end. Declared at global scope (external linkage) — a local extern inside the anon namespace below
// would bind to an anon-namespace symbol (internal linkage) and fail to link.
extern void g144FlushPending();
// G150 MTGS (multi-threaded GS): defined in ps2_gif_arbiter.cpp. When DC2_G150_MTGS=1 the GS drain
// runs on a dedicated worker thread. g150_frame_barrier runs this closure (G144 flush + present
// latch) correctly either way: with DC2_G157_PIPELINE=1 it hands the closure to the worker as a
// frame-boundary marker and the EE returns immediately (bounded ≤1-frame pipeline overlap,
// register-write races against the worker's deferred latch closed by ps2_memory.cpp's
// g150_pipeline_wait_register_slot() gate); otherwise it blocks the EE until the worker has
// finished this frame's draws and then runs the closure here, on the EE thread (G150-v2). When
// MTGS is off, g150_frame_barrier runs the closure inline (byte-identical to the pre-G150 path).
extern bool g150_mtgs_enabled();
extern void g150_frame_barrier(std::function<void()> latch);
extern void g150_wait_idle();

// G303: VU1-worker (MTVU) busy-time attribution — snapshotted per perf window in the G146 block
// to place the VU1 worker on the same footing as GSimage/EE for pole attribution.
extern uint64_t g297WorkerBusyNs();
extern uint64_t g297WorkerKicksRun();
extern uint64_t g297GsCollectStallNs();
extern uint64_t g297GsCollectStalls();
extern uint64_t g303_gs_worker_busy_ns();

// G217: one-shot exact head-object packet correlation consumed by ps2_memory.cpp.
std::atomic<uint32_t> g_dc2G217HeadDmaPacket{0u};
std::atomic<uint32_t> g_dc2G217HeadDmaSelf{0u};
std::atomic<uint32_t> g_dc2G217HeadDmaKind{0u};
std::atomic<uint32_t> g_dc2G217DirectPackets[128]{};
std::atomic<uint32_t> g_dc2G217DirectPacketWrite{0u};

namespace
{
#include "dc2_game_override_parts/common_state.inc"
#include "dc2_game_override_parts/texture_probes.inc"
#include "dc2_game_override_parts/title_camera_and_map.inc"
#include "dc2_game_override_parts/frontend_loops.inc"
#include "dc2_game_override_parts/title_map_pipeline.inc"
#include "dc2_game_override_parts/frontend_costume_camera.inc"
#include "dc2_game_override_parts/dungeon_init.inc"

// F56: classify the in-dungeon state each frame. DngStatus selects the
// LoopDungeonMain branch: 0=3D world (DngMainDraw@0x1CF090), 1/4=menu
// (MenuMainDraw), 2=event, 3=event-edit, 5=exit. DngTreeMode is the
// floor-select treemap sub-state (0=display map, 1=transition/confirm).
// Addresses resolved from assembly:
//   DngStatus  : lui at,0x1ea; lw s1,-0x920(at)        -> 0x01E9F6E0 (word)
//   DngTreeMode: lh a0,-0x70fc(gp) (DngTreeMapDraw)     -> gp-0x70fc = 0x003773F4 (half)
// Quiet unless DC2_TRACE_F56. Bounded: logs on state change + every 60th
// frame, capped, so the floor-select->3D transition is visible compactly.
#include "dc2_game_override_parts/dungeon_runtime.inc"

#include "dc2_game_override_parts/title_draw_runtime.inc"
#include "dc2_game_override_parts/frame_end_and_core_helpers.inc"
#include "dc2_game_override_parts/object_init_and_pad.inc"

#include "dc2_game_override_parts/live_input_and_stubs.inc"

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
