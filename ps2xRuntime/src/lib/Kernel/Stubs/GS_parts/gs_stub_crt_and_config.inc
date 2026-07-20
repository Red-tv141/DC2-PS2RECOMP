#include "Common.h"
#include "GS.h"
#include "ps2_log.h"
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_psmct16.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// F60 spin diagnostics (defined in ps2_runtime.cpp).
extern std::atomic<uint64_t> g_f60_gsSyncV;

// G177 (defined in ps2_gif_arbiter.cpp): ordered-closure hand-off to the GS worker FIFO.
// Returns false when MTGS is off / worker not running — caller applies inline.
#include <functional>
extern bool g150_enqueue_apply(std::function<void()> fn);

namespace ps2_stubs
{
    namespace
    {
        bool envFlagEnabled(const char *name)
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

        bool phaseDiagnosticsEnabled()
        {
            static const bool enabled = envFlagEnabled("DC2_PHASE_TRACE");
            return enabled;
        }

        std::mutex g_gs_sync_v_mutex;
        uint64_t g_gs_sync_v_base_tick = 0u;
        std::mutex g_gs_sync_v_callback_mutex;
        uint32_t g_gs_sync_v_callback_func = 0u;
        uint32_t g_gs_sync_v_callback_gp = 0u;
        uint32_t g_gs_sync_v_callback_sp = 0u;
        uint32_t g_gs_sync_v_callback_stack_base = 0u;
        uint32_t g_gs_sync_v_callback_stack_top = 0u;
        uint32_t g_gs_sync_v_callback_bad_pc_logs = 0u;
        struct GsDebugProbePoint
        {
            uint32_t x;
            uint32_t y;
        };

        constexpr GsDebugProbePoint kGhostProbePoints[] = {
            {220u, 176u},
            {260u, 208u},
            {320u, 208u},
            {260u, 240u},
            {320u, 240u},
            {260u, 272u},
            {320u, 272u},
        };

        uint64_t makeClearPrim(bool useContext2)
        {
            return static_cast<uint64_t>(GS_PRIM_SPRITE) |
                   (static_cast<uint64_t>(useContext2 ? 1u : 0u) << 9);
        }

        uint64_t makeClearRgbaq(uint32_t rgba)
        {
            return static_cast<uint64_t>(rgba);
        }

        uint64_t makeClearXyz(int32_t x, int32_t y)
        {
            return static_cast<uint64_t>(static_cast<uint16_t>(x << 4)) |
                   (static_cast<uint64_t>(static_cast<uint16_t>(y << 4)) << 16);
        }

        void seedGsClearPacket(GsClearMem &clear,
                               int32_t width,
                               int32_t height,
                               uint32_t rgba,
                               uint32_t ztest,
                               bool useContext2)
        {
            const int32_t offX = 0x800 - (width >> 1);
            const int32_t offY = 0x800 - (height >> 1);
            const uint64_t clearTest = makeTest(0u);
            const uint64_t restoreTest = makeTest(ztest);
            const uint64_t prim = makeClearPrim(useContext2);
            const uint64_t rgbaq = makeClearRgbaq(rgba);
            const uint64_t xyz0 = makeClearXyz(offX, offY);
            const uint64_t xyz1 = makeClearXyz(offX + width, offY + height);
            const uint64_t testReg = useContext2 ? GS_REG_TEST_2 : GS_REG_TEST_1;

            clear.testa = {clearTest, testReg};
            clear.prim = {prim, GS_REG_PRIM};
            clear.rgbaq = {rgbaq, GS_REG_RGBAQ};
            clear.xyz2a = {xyz0, GS_REG_XYZ2};
            clear.xyz2b = {xyz1, GS_REG_XYZ2};
            clear.testb = {restoreTest, testReg};
        }

        bool hasSeededGsClearPacket(const GsClearMem &clear)
        {
            return clear.rgbaq.reg == GS_REG_RGBAQ &&
                   clear.xyz2a.reg == GS_REG_XYZ2 &&
                   clear.xyz2b.reg == GS_REG_XYZ2;
        }

        struct GsTrailingArgs2
        {
            uint32_t arg0 = 0u;
            uint32_t arg1 = 0u;
        };

        struct GsTrailingArgs3
        {
            uint32_t arg0 = 0u;
            uint32_t arg1 = 0u;
            uint32_t arg2 = 0u;
        };

        GsTrailingArgs2 decodeGsTrailingArgs2(uint8_t *rdram, R5900Context *ctx)
        {
            const uint32_t reg8 = getRegU32(ctx, 8);
            const uint32_t reg9 = getRegU32(ctx, 9);
            const uint32_t stack0 = readStackU32(rdram, ctx, 16);
            const uint32_t stack1 = readStackU32(rdram, ctx, 20);

            const bool hasRegArgs = (reg8 != 0u || reg9 != 0u);
            const bool hasStackArgs = (stack0 != 0u || stack1 != 0u);
            if (hasRegArgs || !hasStackArgs)
            {
                return {reg8, reg9};
            }

            return {stack0, stack1};
        }

        GsTrailingArgs3 decodeGsTrailingArgs3(uint8_t *rdram, R5900Context *ctx)
        {
            const uint32_t reg8 = getRegU32(ctx, 8);
            const uint32_t reg9 = getRegU32(ctx, 9);
            const uint32_t reg10 = getRegU32(ctx, 10);
            const uint32_t stack0 = readStackU32(rdram, ctx, 16);
            const uint32_t stack1 = readStackU32(rdram, ctx, 20);
            const uint32_t stack2 = readStackU32(rdram, ctx, 24);

            const bool hasRegArgs = (reg8 != 0u || reg9 != 0u || reg10 != 0u);
            const bool hasStackArgs = (stack0 != 0u || stack1 != 0u || stack2 != 0u);
            if (hasRegArgs || !hasStackArgs)
            {
                return {reg8, reg9, reg10};
            }

            return {stack0, stack1, stack2};
        }

        bool sampleFramebufferPixel(uint8_t *vram,
                                    uint32_t vramSize,
                                    uint32_t fbp,
                                    uint32_t fbw,
                                    uint32_t psm,
                                    uint32_t x,
                                    uint32_t y,
                                    uint32_t &outPixel)
        {
            if (!vram || fbw == 0u)
            {
                return false;
            }

            const uint32_t bytesPerPixel =
                (psm == GS_PSM_CT16 || psm == GS_PSM_CT16S) ? 2u : (psm == GS_PSM_CT32 || psm == GS_PSM_CT24) ? 4u
                                                                                                              : 0u;
            if (bytesPerPixel == 0u)
            {
                return false;
            }

            const uint32_t widthBlocks = (fbw != 0u) ? fbw : 1u;
            const uint32_t strideBytes = fbw * 64u * bytesPerPixel;
            const uint32_t baseBytes = fbp * 8192u;
            uint32_t offset = baseBytes + (y * strideBytes) + (x * bytesPerPixel);
            if (psm == GS_PSM_CT16)
            {
                offset = GSPSMCT16::addrPSMCT16(GSInternal::framePageBaseToBlock(fbp), widthBlocks, x, y);
            }
            else if (psm == GS_PSM_CT16S)
            {
                offset = GSPSMCT16::addrPSMCT16S(GSInternal::framePageBaseToBlock(fbp), widthBlocks, x, y);
            }
            if (offset + bytesPerPixel > vramSize)
            {
                return false;
            }

            if (bytesPerPixel == 4u)
            {
                std::memcpy(&outPixel, vram + offset, sizeof(outPixel));
                if (psm == GS_PSM_CT24)
                {
                    outPixel |= 0xFF000000u;
                }
                return true;
            }

            uint16_t packed = 0u;
            std::memcpy(&packed, vram + offset, sizeof(packed));
            const uint32_t r = ((packed >> 0) & 0x1Fu) << 3;
            const uint32_t g = ((packed >> 5) & 0x1Fu) << 3;
            const uint32_t b = ((packed >> 10) & 0x1Fu) << 3;
            const uint32_t a = (packed & 0x8000u) ? 0x80u : 0x00u;
            outPixel = r | (g << 8) | (b << 16) | (a << 24);
            return true;
        }

        bool sampleFrameRegPixel(PS2Runtime *runtime,
                                 uint64_t frameReg,
                                 uint32_t x,
                                 uint32_t y,
                                 uint32_t &outPixel)
        {
            if (!runtime)
            {
                return false;
            }

            const uint32_t fbp = static_cast<uint32_t>(frameReg & 0x1FFu);
            const uint32_t fbw = static_cast<uint32_t>((frameReg >> 16) & 0x3Fu);
            const uint32_t psm = static_cast<uint32_t>((frameReg >> 24) & 0x3Fu);
            return sampleFramebufferPixel(runtime->memory().getGSVRAM(), PS2_GS_VRAM_SIZE, fbp, fbw, psm, x, y, outPixel);
        }

        bool sampleDispFbPixel(PS2Runtime *runtime,
                               uint64_t dispfb,
                               uint32_t x,
                               uint32_t y,
                               uint32_t &outPixel)
        {
            if (!runtime)
            {
                return false;
            }

            const uint32_t fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
            const uint32_t fbw = static_cast<uint32_t>((dispfb >> 9) & 0x3Fu);
            const uint32_t psm = static_cast<uint32_t>((dispfb >> 15) & 0x1Fu);
            return sampleFramebufferPixel(runtime->memory().getGSVRAM(), PS2_GS_VRAM_SIZE, fbp, fbw, psm, x, y, outPixel);
        }

        void logSwapProbeStage(PS2Runtime *runtime,
                               const char *stage,
                               uint32_t which,
                               uint64_t drawFrameReg,
                               uint64_t dispfb,
                               bool hasClearPacket)
        {
            static uint32_t s_swapProbeCount = 0u;
            if (!runtime || s_swapProbeCount >= 24u)
            {
                return;
            }

            PS2_IF_AGRESSIVE_LOGS({
                RUNTIME_LOG("[gs:probe] stage=" << stage
                                                << " which=" << which
                                                << " clear=" << static_cast<uint32_t>(hasClearPacket ? 1u : 0u));

                for (const auto &probe : kGhostProbePoints)
                {
                    uint32_t page0Pixel = 0u;
                    uint32_t page150Pixel = 0u;
                    const bool havePage0 = sampleFrameRegPixel(runtime, drawFrameReg, probe.x, probe.y, page0Pixel);
                    const bool havePage150 = sampleDispFbPixel(runtime, dispfb, probe.x, probe.y, page150Pixel);
                    if (havePage0)
                    {
                        RUNTIME_LOG(" p0[" << probe.x << "," << probe.y << "]=0x"
                                           << std::hex << page0Pixel << std::dec);
                    }
                    if (havePage150)
                    {
                        RUNTIME_LOG(" p150[" << probe.x << "," << probe.y << "]=0x"
                                             << std::hex << page150Pixel << std::dec);
                    }
                }
                RUNTIME_LOG(std::endl);
                ++s_swapProbeCount;
            });
        }

        void applyGsClearPacket(PS2Runtime *runtime, const GsClearMem &clear)
        {
            if (!runtime || !runtime->syncCoreSubsystems() || !hasSeededGsClearPacket(clear))
            {
                return;
            }

            runtime->gs().writeRegister(static_cast<uint8_t>(clear.testa.reg & 0xFFu), clear.testa.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.prim.reg & 0xFFu), clear.prim.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.rgbaq.reg & 0xFFu), clear.rgbaq.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.xyz2a.reg & 0xFFu), clear.xyz2a.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.xyz2b.reg & 0xFFu), clear.xyz2b.value);
            runtime->gs().writeRegister(static_cast<uint8_t>(clear.testb.reg & 0xFFu), clear.testb.value);
        }

        // G219: sceGsSwapDBuffDc's swap-time framebuffer clear used to be applied IMMEDIATELY on
        // the EE thread — clearFramebufferContext() rect-filled VRAM through the LIVE GS context,
        // then applyGsClearPacket() drove the clear sprite via direct writeRegister calls. Both are
        // the exact G177 FIFO-bypass class (one site G177's audit missed): under the default
        // MTGS+G144 pipeline the worker still holds this frame's DEFERRED draw list unflushed, and
        // m_ctx still holds whatever FRAME the last WORKER-processed packet bound (e.g. the
        // fbp=0x139 character-RTT pages = tbp 0x2720 as a texture), so the EE-side clear stomped
        // texture pages that deferred triangles (the town sky/feedback pass) sample at flush time —
        // the G218 MAP-4 zoom top-right black region (pixel-proven: the same feedback triangle
        // samples sky-blue inline but black under deferral, [G219:skycol]). Fix: submit the seeded
        // 6-register clear packet (TEST/PRIM/RGBAQ/XYZ2/XYZ2/TEST — GsClearMem is 6 contiguous
        // GsRegPairMem, already in A+D qword layout) through the GifArbiter FIFO exactly like the
        // G177 drawenv pairs, so the worker executes it in program order and the XYZ2 kicks travel
        // the normal drawPrimitive/deferral path. Kill switch DC2_G219_DIRECT_SWAPCLEAR=1 restores
        // the old immediate clear.
        void g219ApplyGsClearPacketQueued(PS2Runtime *runtime,
                                          const GsClearMem &clear,
                                          uint32_t clearContext)
        {
            if (!runtime || !runtime->syncCoreSubsystems() || !hasSeededGsClearPacket(clear))
            {
                return;
            }
            static const bool s_direct = envFlagEnabled("DC2_G219_DIRECT_SWAPCLEAR");
            if (s_direct)
            {
                runtime->gs().clearFramebufferContext(clearContext,
                                                      static_cast<uint32_t>(clear.rgbaq.value));
                applyGsClearPacket(runtime, clear);
                return;
            }
            uint8_t buf[16u + 6u * 16u];
            const uint64_t tagLo = makeGiftagAplusD(6u);
            const uint64_t tagHi = 0xEull; // REGS[0] = A+D
            std::memcpy(buf, &tagLo, sizeof(tagLo));
            std::memcpy(buf + 8, &tagHi, sizeof(tagHi));
            std::memcpy(buf + 16, &clear, 6u * sizeof(GsRegPairMem));
            runtime->memory().submitGifPacket(GifPathId::Path3, buf, sizeof(buf), true);
        }

        // G177: sceGsSwapDBuff / sceGsPutDrawEnv / sceGsPutDispEnv are STUBBED (DC2 calls
        // sceGsSwapDBuff from mgEndFrame EVERY frame, alternating buffers), and their register
        // writes used to be applied DIRECTLY from the EE thread (Support.h applyGsDispEnv /
        // applyGsRegPairs): the 8 drawenv general registers (FRAME_1/ZBUF_1/XYOFFSET_1/SCISSOR_1/
        // PRMODECONT/COLCLAMP/DTHE/TEST_1) poked the GS state machine BETWEEN the worker's queued
        // packets (out of FIFO order), and the dispenv privileged registers mutated the gs_regs
        // fields directly — bypassing BOTH the GifArbiter FIFO and the G157 pipeline
        // register-slot gate in PS2Memory::writeIORegister (G157's whole-tree audit missed this
        // site: it writes through a local `regs.` reference, not `gs_regs.`). Under
        // DC2_G157_PIPELINE=1 the EE runs frame N+1's mgEndFrame while the worker is still
        // drawing frame N, so the direct writes clobber frame N's draw state mid-stream — the
        // G174 title-rock wrong-color race. Fix: route the drawenv registers through the arbiter
        // as a synthesized A+D GIF packet (what the real sceGsPutDrawEnv does via GIF DMA), and
        // the dispenv privileged registers through PS2Memory::writeIORegister (which carries the
        // G157 gate). Kill-switch DC2_G177_DIRECT_DRAWENV=1 restores the old direct writes.
        bool g177DirectDrawEnv()
        {
            static const bool enabled = envFlagEnabled("DC2_G177_DIRECT_DRAWENV");
            return enabled;
        }

        void g177ApplyGsDispEnv(PS2Runtime *runtime, const GsDispEnvMem &env)
        {
            // DISPENV stays IMMEDIATE-on-EE (the exact pre-G177 behavior) in every mode. Two
            // rerouting designs were built and REFUTED by measurement:
            //  (1) via PS2Memory::writeIORegister (G157 gate) — ordering-correct but the
            //      per-frame gate wait serialized the G157 pipeline, 9.3 → 6.8 fps;
            //  (2) via the worker FIFO (g150_enqueue_apply) — no stall, but deferral breaks EE
            //      PROGRAM ORDER against the guest's own later writes of the same privileged
            //      registers: golden sweep went h=415 → h=416 with 27/28 frames out of band.
            // The G174 rock-color race is the DRAWENV clobber (g177ApplyGsRegPairs below), not
            // dispenv: dispenv feeds only the presentation latch. The residual (dispenv can
            // reach the latch one frame early under DC2_G157_PIPELINE=1) is a bounded,
            // presentation-only hazard that predates G177 — fixing it requires deferring ALL
            // EE writers of these registers uniformly (a G157-gate redesign, its own phase).
            applyGsDispEnv(runtime, env);
        }

        void g177ApplyGsRegPairs(PS2Runtime *runtime, const GsRegPairMem *pairs, size_t pairCount)
        {
            if (!runtime || !pairs || !runtime->syncCoreSubsystems())
                return;
            if (g177DirectDrawEnv())
            {
                applyGsRegPairs(runtime, pairs, pairCount);
                return;
            }
            // A+D packed GIF packet: giftag, then one {data,reg} qword per register —
            // GsRegPairMem already has exactly the A+D qword layout.
            uint8_t buf[16u + 16u * 16u];
            const size_t count = std::min<size_t>(pairCount, 16u);
            const uint64_t tagLo = makeGiftagAplusD(static_cast<uint32_t>(count));
            const uint64_t tagHi = 0xEull; // REGS[0] = A+D
            std::memcpy(buf, &tagLo, sizeof(tagLo));
            std::memcpy(buf + 8, &tagHi, sizeof(tagHi));
            std::memcpy(buf + 16, pairs, count * sizeof(GsRegPairMem));
            runtime->memory().submitGifPacket(GifPathId::Path3,
                                              buf,
                                              static_cast<uint32_t>(16u + count * sizeof(GsRegPairMem)),
                                              true);
        }

        void refreshPacketBuilderPendingCount(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr);
        void writePacketBuilderCurrent(uint8_t *rdram, PS2Runtime *runtime, uint32_t stateAddr, uint32_t currentAddr);
        void logVif1PacketStateOp(const char *op,
                                  R5900Context *ctx,
                                  uint32_t stateAddr,
                                  uint32_t currentAddr,
                                  uint32_t aux0,
                                  uint32_t aux1)
        {
            static uint32_t s_vif1PacketOpLogCount = 0u;
            if (s_vif1PacketOpLogCount >= 96u)
            {
                return;
            }

            RUNTIME_LOG("[vif1:packet] idx=" << s_vif1PacketOpLogCount
                                             << " op=" << op
                                             << " pc=0x" << std::hex << ctx->pc
                                             << " ra=0x" << getRegU32(ctx, 31)
                                             << " state=0x" << stateAddr
                                             << " current=0x" << currentAddr
                                             << " aux0=0x" << aux0
                                             << " aux1=0x" << aux1
                                             << std::dec << std::endl);
            ++s_vif1PacketOpLogCount;
        }

        void initPacketBuilderState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stateAddr = getRegU32(ctx, 4);
            const uint32_t baseAddr = getRegU32(ctx, 5);
            const uint32_t words[4] = {baseAddr, baseAddr, 0u, 0u};
            writeGuestBytes(rdram,
                            runtime,
                            stateAddr,
                            reinterpret_cast<const uint8_t *>(words),
                            sizeof(words));
