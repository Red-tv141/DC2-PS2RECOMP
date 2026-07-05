#include "Common.h"
#include "System.h"

namespace ps2_stubs
{
    void builtin_set_imask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int logCount = 0;
        if (logCount < 8)
        {
            RUNTIME_LOG("ps2_stub builtin_set_imask");
            ++logCount;
        }
        setReturnS32(ctx, 0);
    }

    void sceIDC(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSDC(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void exit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // F57: surface WHO terminated the program. The dungeon floor-entrance event
        // ends the process via a guest exit()/_Exit; logging the caller (ra) + arg
        // localises whether it is the CRunScript VM malformed-operand exit(-1)
        // (exe__10CRunScript, ~0x18xxxx) or the LoadFile fatal abort (0x118FB0).
        // Gated on DC2_TRACE_F57 so default smoke stays quiet.
        static const bool s_trace = []() {
            const char *e = std::getenv("DC2_TRACE_F57");
            return e && e[0] && e[0] != '0';
        }();
        if (s_trace)
        {
            // ra=0x123ec0 means the caller is abort()@0x123ea8 (raise(6); _exit(1)).
            // abort's prologue does `sd ra,0x0(sp)`, so the REAL culprit (abort's
            // own caller) is the word at sp+0. Dump a small stack window of saved
            // return addresses to localise the fatal site in the event/floor load.
            const uint32_t ra = getRegU32(ctx, 31);
            const uint32_t sp = getRegU32(ctx, 29);
            std::fprintf(stderr, "[F57:exit] guest exit() ra=0x%x a0=0x%x sp=0x%x\n",
                         ra, getRegU32(ctx, 4), sp);
            if (rdram && sp && sp < 0x02000000u)
            {
                std::fprintf(stderr, "[F57:exit] stack@sp:");
                for (uint32_t off = 0; off <= 0x180u; off += 4u)
                {
                    const uint32_t w = *reinterpret_cast<const uint32_t *>(rdram + ((sp + off) & 0x01FFFFFFu));
                    if (w >= 0x00100000u && w < 0x002e0000u) // plausible guest text addr (DC2 .text)
                        std::fprintf(stderr, " +%x=0x%x", off, w);
                }
                std::fprintf(stderr, "\n");
            }
        }
        if (runtime)
        {
            runtime->requestStop();
        }
        setReturnS32(ctx, 0);
    }

    void getpid(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSetBrokenLink(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSetBrokenLink", rdram, ctx, runtime);
    }

    void sceSetPtm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSetPtm", rdram, ctx, runtime);
    }

    void sceDevVif0Reset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceDevVu0Reset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

}
