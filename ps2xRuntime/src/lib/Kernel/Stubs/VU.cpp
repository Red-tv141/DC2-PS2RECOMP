#include "Common.h"
#include "VU.h"
//TODO use glm

namespace ps2_stubs
{
    void sceVu0ecossin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceVu0ecossin", rdram, ctx, runtime);
    }

    namespace
    {
        // G371 (diagnostic, default-off: DC2_G371_VU0NAN=1) — non-finite watch on the VU0-macro
        // math stubs. The cutscene gray screen is an EE-side poison: the camera matrix in
        // mgRENDER_INFO (+0x1a0) and the camera position (+0x3a0) go NaN mid-cutscene while the
        // projection stays clean. Every sceVu0* stub reads and writes guest vectors through these
        // two helpers, so watching them names the guest ADDRESS that first holds a NaN — which is
        // then resolvable to an owning object via ref/index/globals_index.json.
        // Reads and writes are reported separately: a NaN arriving on a READ means the poison was
        // already in guest RAM (an EE FPU computation upstream), a NaN leaving on a WRITE whose
        // inputs were clean means the stub itself manufactured it.
        static const bool s_DC2_G371_VU0NAN = (std::getenv("DC2_G371_VU0NAN") != nullptr);

        void g371NoteVuNan(const char *dir, uint32_t addr, const float *v, int n)
        {
            if (!s_DC2_G371_VU0NAN)
                return;
            bool bad = false;
            for (int i = 0; i < n; ++i)
                if (!std::isfinite(v[i]))
                    bad = true;
            if (!bad)
                return;
            // Dedup by address so one poisoned object does not drown the log.
            static std::atomic<uint32_t> s_seen[64]{};
            static std::atomic<uint32_t> s_nSeen{0};
            const uint32_t have = s_nSeen.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < have && i < 64u; ++i)
                if (s_seen[i].load(std::memory_order_relaxed) == addr)
                    return;
            if (have < 64u)
                s_seen[have].store(addr, std::memory_order_relaxed), s_nSeen.fetch_add(1u, std::memory_order_relaxed);
            std::fprintf(stderr, "[G371:vu0nan] %s addr=0x%08x n=%d v=(% .6g % .6g % .6g % .6g)\n",
                         dir, addr, n, v[0], n > 1 ? v[1] : 0.0f, n > 2 ? v[2] : 0.0f,
                         n > 3 ? v[3] : 0.0f);
        }

        bool readVuVec4f(uint8_t *rdram, uint32_t addr, float (&out)[4])
        {
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(out, ptr, sizeof(out));
            g371NoteVuNan("read4", addr, out, 4);
            return true;
        }

        bool writeVuVec4f(uint8_t *rdram, uint32_t addr, const float (&in)[4])
        {
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            g371NoteVuNan("writ4", addr, in, 4);
            std::memcpy(ptr, in, sizeof(in));
            return true;
        }

        bool readVuVec4i(uint8_t *rdram, uint32_t addr, int32_t (&out)[4])
        {
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(out, ptr, sizeof(out));
            return true;
        }

        bool writeVuVec4i(uint8_t *rdram, uint32_t addr, const int32_t (&in)[4])
        {
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(ptr, in, sizeof(in));
            return true;
        }

        bool readVuMatrix4f(uint8_t *rdram, uint32_t addr, float (&out)[16])
        {
            const uint8_t *ptr = getConstMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            std::memcpy(out, ptr, sizeof(out));
            g371NoteVuNan("read16", addr, out, 16);
            return true;
        }

        bool writeVuMatrix4f(uint8_t *rdram, uint32_t addr, const float (&in)[16])
        {
            uint8_t *ptr = getMemPtr(rdram, addr);
            if (!ptr)
            {
                return false;
            }
            g371NoteVuNan("writ16", addr, in, 16);
            std::memcpy(ptr, in, sizeof(in));
            return true;
        }

        void mulVuMatrix(const float (&lhs)[16], const float (&rhs)[16], float (&out)[16])
        {
            std::fill(std::begin(out), std::end(out), 0.0f);
            for (int i = 0; i < 4; ++i)
            {
                for (int j = 0; j < 4; ++j)
                {
                    for (int k = 0; k < 4; ++k)
                    {
                        out[4 * i + j] += rhs[4 * k + j] * lhs[4 * i + k];
                    }
                }
            }
        }

        void makeIdentityMatrix(float (&out)[16])
        {
            std::fill(std::begin(out), std::end(out), 0.0f);
            out[0] = 1.0f;
            out[5] = 1.0f;
            out[10] = 1.0f;
            out[15] = 1.0f;
        }

        // G372: single source of truth for the three single-axis rotation stubs, so that
        // sceVu0RotMatrix (which the hardware implements purely by chaining Z -> Y -> X) cannot
        // drift from sceVu0RotMatrix{X,Y,Z}. axis: 0 = X, 1 = Y, 2 = Z.
        void vu0RotateAxis(uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, float angle, int axis)
        {
            float src[16]{}, rot[16]{}, out[16]{};
            if (!readVuMatrix4f(rdram, srcAddr, src))
                return;
            makeIdentityMatrix(rot);
            const float cs = std::cos(angle);
            const float sn = std::sin(angle);
            switch (axis)
            {
            case 0: rot[5] = cs;  rot[6] = sn;  rot[9] = -sn; rot[10] = cs; break;
            case 1: rot[0] = cs;  rot[2] = -sn; rot[8] = sn;  rot[10] = cs; break;
            default: rot[0] = cs; rot[1] = sn;  rot[4] = -sn; rot[5] = cs;  break;
            }
            mulVuMatrix(src, rot, out);
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }

        // G373 (diagnostic, default-off: DC2_G373_THROW=1) — instrument the item-throw vector.
        //
        // `ThrowItemObject__12CActionCharaFv` @0x0016AF40 builds the thrown object's target as:
        //     dir  = copy of  this+0x690            (sceVu0CopyVector,  ra=0x16AF80)
        //     pos  = (*vtable[0x18])(this)          (writes sp+0x30)
        //     dir  = dir * 120.0f                   (sceVu0ScaleVector, ra=0x16AFA8, f12=0x42F00000)
        //     out  = dir + pos                      (sceVu0AddVector,   ra=0x16AFB8)
        //     SetScriptVect1(effectMan, out, ...)
        // so a wrong throw direction is either a wrong `this+0x690` direction vector or a wrong
        // vtable[0x18] position — and this prints both, plus the result, without needing a hook on
        // the recompiled function itself.
        //
        // The recompiled body writes real guest return addresses into $ra before each stub call,
        // so gating on `ra` inside the function's extent isolates THIS call site from the hundreds
        // of other sceVu0* callers per frame.
        static const bool s_DC2_G373_THROW = (std::getenv("DC2_G373_THROW") != nullptr);
        constexpr uint32_t kG373ThrowLo = 0x0016AF40u;
        constexpr uint32_t kG373ThrowHi = 0x0016B020u;

        bool g373InThrow(const R5900Context *ctx)
        {
            if (!s_DC2_G373_THROW || ctx == nullptr)
                return false;
            const uint32_t ra = getRegU32(ctx, 31);
            return ra >= kG373ThrowLo && ra < kG373ThrowHi;
        }

        void g373NoteThrow(const char *what, const R5900Context *ctx, uint32_t addr,
                           const float *v, float scale)
        {
            std::fprintf(stderr, "[G373:throw] %-5s ra=0x%06x addr=0x%08x v=(% .4f % .4f % .4f % .4f)",
                         what, getRegU32(ctx, 31), addr, v[0], v[1], v[2], v[3]);
            if (scale == scale && scale != 0.0f)
                std::fprintf(stderr, " f12=% .4f", scale);
            std::fprintf(stderr, "\n");
        }

        float dotVuVec3(const float (&lhs)[4], const float (&rhs)[4])
        {
            return (lhs[0] * rhs[0]) + (lhs[1] * rhs[1]) + (lhs[2] * rhs[2]);
        }

        void crossVuVec3(const float (&lhs)[4], const float (&rhs)[4], float (&out)[4])
        {
            out[0] = (lhs[1] * rhs[2]) - (lhs[2] * rhs[1]);
            out[1] = (lhs[2] * rhs[0]) - (lhs[0] * rhs[2]);
            out[2] = (lhs[0] * rhs[1]) - (lhs[1] * rhs[0]);
            out[3] = 0.0f;
        }

        bool normalizeVuVec3(float (&vec)[4])
        {
            const float lenSq = dotVuVec3(vec, vec);
            if (lenSq <= 1.0e-12f)
            {
                return false;
            }
            const float invLen = 1.0f / std::sqrt(lenSq);
            vec[0] *= invLen;
            vec[1] *= invLen;
            vec[2] *= invLen;
            vec[3] = 0.0f;
            return true;
        }
    }

    void sceVpu0Reset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceVu0AddVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t lhsAddr = getRegU32(ctx, 5);
        const uint32_t rhsAddr = getRegU32(ctx, 6);
        float lhs[4]{}, rhs[4]{}, out[4]{};
        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = lhs[i] + rhs[i];
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
            if (g373InThrow(ctx))
            {
                g373NoteThrow("velo", ctx, lhsAddr, lhs, 0.0f); // dir * 120
                g373NoteThrow("pos ", ctx, rhsAddr, rhs, 0.0f); // vtable[0x18] output
                g373NoteThrow("targ", ctx, dstAddr, out, 0.0f); // what SetScriptVect1 receives
            }
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ApplyMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t matrixAddr = getRegU32(ctx, 5);
        const uint32_t srcAddr = getRegU32(ctx, 6);
        float matrix[16]{};
        float src[4]{};
        float out[4]{};
        if (readVuMatrix4f(rdram, matrixAddr, matrix) && readVuVec4f(rdram, srcAddr, src))
        {
            // Match libvux VuxApplyMatrix math while honoring the imported EE ABI:
            // a0=out, a1=matrix, a2=vector.
            out[0] = (matrix[0] * src[0]) + (matrix[4] * src[1]) + (matrix[8] * src[2]) + (matrix[12] * src[3]);
            out[1] = (matrix[1] * src[0]) + (matrix[5] * src[1]) + (matrix[9] * src[2]) + (matrix[13] * src[3]);
            out[2] = (matrix[2] * src[0]) + (matrix[6] * src[1]) + (matrix[10] * src[2]) + (matrix[14] * src[3]);
            out[3] = (matrix[3] * src[0]) + (matrix[7] * src[1]) + (matrix[11] * src[2]) + (matrix[15] * src[3]);
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0CameraMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t positionAddr = getRegU32(ctx, 5);
        const uint32_t directionAddr = getRegU32(ctx, 6);
        const uint32_t upAddr = getRegU32(ctx, 7);

        float position[4]{};
        float direction[4]{};
        float up[4]{};
        float view[16]{};
        makeIdentityMatrix(view);

        if (readVuVec4f(rdram, positionAddr, position) &&
            readVuVec4f(rdram, directionAddr, direction) &&
            readVuVec4f(rdram, upAddr, up))
        {
            float zAxis[4] = {direction[0], direction[1], direction[2], 0.0f};
            if (!normalizeVuVec3(zAxis))
            {
                zAxis[2] = 1.0f;
            }

            float upAxis[4] = {up[0], up[1], up[2], 0.0f};
            if (!normalizeVuVec3(upAxis))
            {
                upAxis[1] = 1.0f;
            }

            float xAxis[4]{};
            crossVuVec3(upAxis, zAxis, xAxis);
            if (!normalizeVuVec3(xAxis))
            {
                const float fallbackUp[4] = {0.0f, 1.0f, 0.0f, 0.0f};
                const float fallbackSide[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                crossVuVec3(fallbackUp, zAxis, xAxis);
                if (!normalizeVuVec3(xAxis))
                {
                    crossVuVec3(fallbackSide, zAxis, xAxis);
                    (void)normalizeVuVec3(xAxis);
                }
            }

            float yAxis[4]{};
            crossVuVec3(zAxis, xAxis, yAxis);
            if (!normalizeVuVec3(yAxis))
            {
                yAxis[1] = 1.0f;
            }

            view[0] = xAxis[0];
            view[4] = xAxis[1];
            view[8] = xAxis[2];
            view[12] = -dotVuVec3(xAxis, position);

            view[1] = yAxis[0];
            view[5] = yAxis[1];
            view[9] = yAxis[2];
            view[13] = -dotVuVec3(yAxis, position);

            view[2] = zAxis[0];
            view[6] = zAxis[1];
            view[10] = zAxis[2];
            view[14] = -dotVuVec3(zAxis, position);

            view[3] = 0.0f;
            view[7] = 0.0f;
            view[11] = 0.0f;
            view[15] = 1.0f;
        }

        (void)writeVuMatrix4f(rdram, dstAddr, view);
        setReturnS32(ctx, 0);
    }

    void sceVu0ClampVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceVu0ClampVector", rdram, ctx, runtime);
    }

    void sceVu0ClipAll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceVu0ClipAll", rdram, ctx, runtime);
    }

    void sceVu0ClipScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceVu0ClipScreen", rdram, ctx, runtime);
    }

    void sceVu0ClipScreen3(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceVu0ClipScreen3", rdram, ctx, runtime);
    }

    void sceVu0CopyMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        uint8_t *dst = getMemPtr(rdram, dstAddr);
        const uint8_t *src = getConstMemPtr(rdram, srcAddr);
        if (dst && src)
        {
            std::memcpy(dst, src, sizeof(float) * 16u);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0CopyVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        uint8_t *dst = getMemPtr(rdram, dstAddr);
        const uint8_t *src = getConstMemPtr(rdram, srcAddr);
        if (dst && src)
        {
            std::memcpy(dst, src, sizeof(float) * 4u);
            if (g373InThrow(ctx))
            {
                float v[4];
                std::memcpy(v, src, sizeof(v));
                g373NoteThrow("dir", ctx, srcAddr, v, 0.0f); // this+0x690, the facing vector
            }
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0CopyVectorXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        uint8_t *dst = getMemPtr(rdram, dstAddr);
        const uint8_t *src = getConstMemPtr(rdram, srcAddr);
        if (dst && src)
        {
            std::memcpy(dst, src, sizeof(float) * 3u);
        }
        setReturnS32(ctx, 0);
    }

    // Faithful reimplementation of sceVu0DivVector @0x107078 (ref/assembly.txt):
    //   lqc2 vf4,(a1); qmtc2 f12,vf5; vdiv Q,vf0w,vf5x; vmulq.xyzw vf4,vf4,Q; sqc2 vf4,(a0)
    // i.e. out[a0].xyzw = in[a1].xyzw * (1.0 / scalar_f12). vf0.w == 1.0.
    // F65: this was a TODO stub that THREW; the dungeon free-roam camera
    // (Quake2__12CSceneCmrSeq@0x25a6e0) calls it, killing the EE thread → black/frozen
    // free-roam after the entrance event completes.
    void sceVu0DivVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float scale = ctx ? ctx->f[12] : 0.0f;
        // G372: the VU has no Infinity. VDIV with a zero denominator returns +/-0x7F7FFFFF
        // (sign = XOR of the operand signs), not inf — the comment this replaces had the
        // hardware rule backwards, and the inf it produced became a NaN one multiply later.
        const float q = ps2_fpu_div(1.0f, scale);
        float v[4]{};
        if (readVuVec4f(rdram, srcAddr, v))
        {
            for (int i = 0; i < 4; ++i)
                v[i] *= q;
            (void)writeVuVec4f(rdram, dstAddr, v);
        }
        setReturnS32(ctx, 0);
    }

    // sceVu0DivVectorXYZ @0x107098: identical but vmulq.xyz (W preserved).
    void sceVu0DivVectorXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float scale = ctx ? ctx->f[12] : 0.0f;
        const float q = ps2_fpu_div(1.0f, scale); // G372: VU saturation, never inf
        float v[4]{};
        if (readVuVec4f(rdram, srcAddr, v))
        {
            for (int i = 0; i < 3; ++i)
                v[i] *= q;
            (void)writeVuVec4f(rdram, dstAddr, v);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0DropShadowMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceVu0DropShadowMatrix", rdram, ctx, runtime);
    }

    void sceVu0FTOI0Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[4]{};
        int32_t out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<int32_t>(src[i]);
            }
            (void)writeVuVec4i(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0FTOI4Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[4]{};
        int32_t out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<int32_t>(src[i] * 16.0f);
            }
            (void)writeVuVec4i(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    // G372: the dot product is THREE-component, not four. Real sceVu0InnerProduct @0x00106F58:
    //   vmul.xyz vf5,vf4,vf5 ; vaddy.x vf5,vf5,vf5 ; vaddz.x vf5,vf5,vf5 ; qmfc2 v0,vf5
    // i.e. x*x' + y*y' + z*z' — the W lane is multiplied by nothing and never summed.
    // Including W (the pre-G372 code) silently corrupted every dot product taken against a
    // POSITION vector, which carries w=1.0 by convention: `dot += 1*1` added a constant 1 to
    // distances, projections, reflections and angle tests. Same defect class as G233
    // (sceVu0Normalize summing 4 lanes instead of the VU0 ESADD's 3) — this is that audit's
    // second hit; the callers most affected are physics/trajectory, not rendering.
    void sceVu0InnerProduct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t lhsAddr = getRegU32(ctx, 4);
        const uint32_t rhsAddr = getRegU32(ctx, 5);
        float lhs[4]{}, rhs[4]{};
        float dot = 0.0f;
        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            dot = (lhs[0] * rhs[0]) + (lhs[1] * rhs[1]) + (lhs[2] * rhs[2]);
        }

        if (ctx)
        {
            ctx->f[0] = dot;
        }
        uint32_t raw = 0u;
        std::memcpy(&raw, &dot, sizeof(raw));
        setReturnU32(ctx, raw);
    }

    // sceVu0InterVector @0x1070b8 (ref/assembly.txt): linear interpolate.
    //   out[a0].xyzw = v1[a1]*t + v2[a2]*(1-t), t = f12. (vf0.w == 1.0)
    void sceVu0InterVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t v1Addr = getRegU32(ctx, 5);
        const uint32_t v2Addr = getRegU32(ctx, 6);
        const float t = ctx ? ctx->f[12] : 0.0f;
        const float it = 1.0f - t;
        float v1[4]{}, v2[4]{}, out[4]{};
        if (readVuVec4f(rdram, v1Addr, v1) && readVuVec4f(rdram, v2Addr, v2))
        {
            for (int i = 0; i < 4; ++i)
                out[i] = v1[i] * t + v2[i] * it;
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0InterVectorXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t lhsAddr = getRegU32(ctx, 5);
        const uint32_t rhsAddr = getRegU32(ctx, 6);
        const float factor = ctx ? ctx->f[12] : 0.0f;
        float lhs[4]{}, rhs[4]{}, out[4]{};

        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            const float inverseFactor = 1.0f - factor;
            for (int i = 0; i < 3; ++i)
            {
                out[i] = (lhs[i] * factor) + (rhs[i] * inverseFactor);
            }
            out[3] = lhs[3];
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0InversMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // Faithful reimplementation of sceVu0InversMatrix @0x107008 (ref/assembly.txt).
        // ABI: a0=dst matrix, a1=src matrix. Computes the inverse of an affine
        // transform whose 3x3 part is orthonormal (rotation): the rotation block is
        // transposed (R^-1 = R^T) and the translation row becomes -(R^T * t). The
        // input W row scalar (m[15]) is preserved; the rotation rows' W is zeroed.
        // Memory layout is row-major float[16] matching readVuMatrix4f (row r = m[4r..4r+3]).
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float m[16]{};
        float out[16]{};
        if (readVuMatrix4f(rdram, srcAddr, m))
        {
            // Transpose the 3x3 rotation block, W of each rotation row = 0.
            out[0] = m[0];  out[1] = m[4];  out[2] = m[8];   out[3] = 0.0f;
            out[4] = m[1];  out[5] = m[5];  out[6] = m[9];   out[7] = 0.0f;
            out[8] = m[2];  out[9] = m[6];  out[10] = m[10]; out[11] = 0.0f;

            // Translation row = -(R^T * t), where t = (m[12], m[13], m[14]).
            out[12] = -((m[0] * m[12]) + (m[1] * m[13]) + (m[2] * m[14]));
            out[13] = -((m[4] * m[12]) + (m[5] * m[13]) + (m[6] * m[14]));
            out[14] = -((m[8] * m[12]) + (m[9] * m[13]) + (m[10] * m[14]));
            out[15] = m[15];

            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ITOF0Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        int32_t src[4]{};
        float out[4]{};
        if (readVuVec4i(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<float>(src[i]);
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ITOF12Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        int32_t src[4]{};
        float out[4]{};
        if (readVuVec4i(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<float>(src[i]) / 4096.0f;
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ITOF4Vector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        int32_t src[4]{};
        float out[4]{};
        if (readVuVec4i(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = static_cast<float>(src[i]) / 16.0f;
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0LightColorMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool s_g238 = (std::getenv("DC2_G238_LIGHTMTX") != nullptr);
        if (s_g238) {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t nn = s_n.fetch_add(1u, std::memory_order_relaxed);
            if (nn < 8u)
                std::fprintf(stderr, "[G238:LightColorMatrix] call n=%u a0=0x%x a1=0x%x a2=0x%x a3=0x%x t0=0x%x\n",
                             nn, getRegU32(ctx,4), getRegU32(ctx,5), getRegU32(ctx,6), getRegU32(ctx,7), getRegU32(ctx,8));
        }
        TODO_NAMED("sceVu0LightColorMatrix", rdram, ctx, runtime);
    }

    void sceVu0MulMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t lhsAddr = getRegU32(ctx, 5);
        const uint32_t rhsAddr = getRegU32(ctx, 6);
        float lhs[16]{};
        float rhs[16]{};
        float out[16]{};
        if (readVuMatrix4f(rdram, lhsAddr, lhs) && readVuMatrix4f(rdram, rhsAddr, rhs))
        {
            mulVuMatrix(lhs, rhs, out);
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    // sceVu0MulVector @0x107110 (ref/assembly.txt): component-wise multiply.
    //   out[a0].xyzw = a[a1] * b[a2]
    void sceVu0MulVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t aAddr = getRegU32(ctx, 5);
        const uint32_t bAddr = getRegU32(ctx, 6);
        float a[4]{}, b[4]{}, out[4]{};
        if (readVuVec4f(rdram, aAddr, a) && readVuVec4f(rdram, bAddr, b))
        {
            for (int i = 0; i < 4; ++i)
                out[i] = a[i] * b[i];
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0Normalize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[4]{}, out[4]{};
        if (readVuVec4f(rdram, srcAddr, src))
        {
            // Real sceVu0Normalize sums x,y,z ONLY (VU0 ESADD P,vf12), then VMULq scales all
            // four lanes by 1/sqrt(P). Including w in the length (G233 bug) under-scaled every
            // direction whose w!=0 — CDAColPipe::CheckHit's push-out became a pull-IN and the
            // pendant chain collapsed behind the torso (the missing chest gem).
            const float len = std::sqrt((src[0] * src[0]) + (src[1] * src[1]) + (src[2] * src[2]));
            if (len > 1.0e-6f)
            {
                const float invLen = 1.0f / len;
                for (int i = 0; i < 4; ++i)
                {
                    out[i] = src[i] * invLen;
                }
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0NormalLightMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool s_g238 = (std::getenv("DC2_G238_LIGHTMTX") != nullptr);
        if (s_g238) {
            static std::atomic<uint32_t> s_n{0};
            const uint32_t nn = s_n.fetch_add(1u, std::memory_order_relaxed);
            if (nn < 8u)
                std::fprintf(stderr, "[G238:NormalLightMatrix] call n=%u a0=0x%x a1=0x%x a2=0x%x a3=0x%x t0=0x%x\n",
                             nn, getRegU32(ctx,4), getRegU32(ctx,5), getRegU32(ctx,6), getRegU32(ctx,7), getRegU32(ctx,8));
        }
        TODO_NAMED("sceVu0NormalLightMatrix", rdram, ctx, runtime);
    }

    void sceVu0OuterProduct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t lhsAddr = getRegU32(ctx, 5);
        const uint32_t rhsAddr = getRegU32(ctx, 6);
        float lhs[4]{}, rhs[4]{}, out[4]{};
        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            out[0] = (lhs[1] * rhs[2]) - (lhs[2] * rhs[1]);
            out[1] = (lhs[2] * rhs[0]) - (lhs[0] * rhs[2]);
            out[2] = (lhs[0] * rhs[1]) - (lhs[1] * rhs[0]);
            out[3] = 0.0f;
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    // G372: was an unimplemented TODO that left the destination matrix STALE while returning -1.
    // Reachable from `DrawEffect__11CCharacter2Fv` @0x00177CB0 and `LightingEdit__FP6CScene`
    // @0x001A8440. Real sceVu0RotMatrix @0x00107480 is pure composition, in this order:
    //   RotMatrixZ(dst, src, angles[2]) -> RotMatrixY(dst, dst, angles[1])
    //                                   -> RotMatrixX(dst, dst, angles[0])   (tail call)
    // a0=dst, a1=src matrix, a2=pointer to the three angles.
    void sceVu0RotMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t angleAddr = getRegU32(ctx, 6);
        float angles[4]{};
        if (!readVuVec4f(rdram, angleAddr, angles))
        {
            setReturnS32(ctx, 0);
            return;
        }

        // Chain the same single-axis helper the X/Y/Z stubs use, in the real function's order.
        // The first pass reads `src`; the following two read back `dst`. Guest registers are
        // left alone (the real chain reloads a0/a1 itself, so nothing observable depends on it).
        vu0RotateAxis(rdram, dstAddr, srcAddr, angles[2], 2);
        vu0RotateAxis(rdram, dstAddr, dstAddr, angles[1], 1);
        vu0RotateAxis(rdram, dstAddr, dstAddr, angles[0], 0);
        setReturnS32(ctx, 0);
    }

    void sceVu0RotMatrixX(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        // G372: shared with sceVu0RotMatrix, which the hardware builds by chaining these.
        vu0RotateAxis(rdram, dstAddr, srcAddr, ctx ? ctx->f[12] : 0.0f, 0);
        setReturnS32(ctx, 0);
    }

    void sceVu0RotMatrixY(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        // G372: shared with sceVu0RotMatrix, which the hardware builds by chaining these.
        vu0RotateAxis(rdram, dstAddr, srcAddr, ctx ? ctx->f[12] : 0.0f, 1);
        setReturnS32(ctx, 0);
    }

    void sceVu0RotMatrixZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        // G372: shared with sceVu0RotMatrix, which the hardware builds by chaining these.
        vu0RotateAxis(rdram, dstAddr, srcAddr, ctx ? ctx->f[12] : 0.0f, 2);
        setReturnS32(ctx, 0);
    }

    void sceVu0RotTransPers(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceVu0RotTransPers", rdram, ctx, runtime);
    }

    void sceVu0RotTransPersN(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceVu0RotTransPersN", rdram, ctx, runtime);
    }

    void sceVu0ScaleVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[4]{}, out[4]{};
        // G372: the scale is `f12` and ONLY `f12`. Real sceVu0ScaleVector @0x00107128:
        //   lqc2 vf4,(a1) ; mfc1 t0,f12 ; qmtc2 t0,vf5 ; vmulx.xyzw vf6,vf4,vf5 ; sqc2 vf6,(a0)
        // — $a2 is not read at all. The pre-G372 code treated `f12 == 0` as "argument missing"
        // and substituted $a2's raw bits reinterpreted as a float (then as an integer), so a
        // LEGITIMATE scale of 0.0 — the ordinary way to zero a velocity or a delta — multiplied
        // the vector by an arbitrary leftover register value instead of by zero.
        const float scale = ctx ? ctx->f[12] : 0.0f;

        if (readVuVec4f(rdram, srcAddr, src))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = src[i] * scale;
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
            if (g373InThrow(ctx))
            {
                g373NoteThrow("scl<", ctx, srcAddr, src, scale);
                g373NoteThrow("scl>", ctx, dstAddr, out, 0.0f);
            }
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0ScaleVectorXYZ(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const float scale = ctx ? ctx->f[12] : 0.0f;
        float out[4]{};

        if (readVuVec4f(rdram, srcAddr, out))
        {
            for (int i = 0; i < 3; ++i)
            {
                out[i] *= scale;
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0SubVector(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t lhsAddr = getRegU32(ctx, 5);
        const uint32_t rhsAddr = getRegU32(ctx, 6);
        float lhs[4]{}, rhs[4]{}, out[4]{};
        if (readVuVec4f(rdram, lhsAddr, lhs) && readVuVec4f(rdram, rhsAddr, rhs))
        {
            for (int i = 0; i < 4; ++i)
            {
                out[i] = lhs[i] - rhs[i];
            }
            (void)writeVuVec4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    // G372: was an unimplemented TODO that left the destination matrix STALE while returning -1.
    // It is reachable — `Draw Effect__11CCharacter2Fv` @0x00177CB0 calls it twice. Real
    // sceVu0TransMatrix @0x00107140:
    //   lqc2 vf4,(a2) ; lqc2 vf5,0x30(a1) ; lq a3,(a1) ; lq t0,0x10(a1) ; lq t1,0x20(a1)
    //   vadd.xyz vf5,vf5,vf4 ; sq a3,(a0) ; sq t0,0x10(a0) ; sq t1,0x20(a0) ; sqc2 vf5,0x30(a0)
    // i.e. rows 0..2 copied verbatim, row 3 (the translation) gets the a2 vector ADDED in XYZ
    // with W preserved. a0=dst, a1=src matrix, a2=translation.
    void sceVu0TransMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t transAddr = getRegU32(ctx, 6);
        float m[16]{}, t[4]{};
        if (readVuMatrix4f(rdram, srcAddr, m) && readVuVec4f(rdram, transAddr, t))
        {
            m[12] += t[0];
            m[13] += t[1];
            m[14] += t[2];
            // m[15] (W) deliberately untouched: `vadd.xyz` leaves the W lane alone.
            (void)writeVuMatrix4f(rdram, dstAddr, m);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0TransposeMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        float src[16]{};
        float out[16]{};
        if (readVuMatrix4f(rdram, srcAddr, src))
        {
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                {
                    out[4 * row + col] = src[4 * col + row];
                }
            }
            (void)writeVuMatrix4f(rdram, dstAddr, out);
        }
        setReturnS32(ctx, 0);
    }

    void sceVu0UnitMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dstAddr = getRegU32(ctx, 4); // sceVu0FMATRIX dst
        alignas(16) const float identity[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};

        if (!writeGuestBytes(rdram, runtime, dstAddr, reinterpret_cast<const uint8_t *>(identity), sizeof(identity)))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 8)
            {
                std::cerr << "sceVu0UnitMatrix: failed to write matrix at 0x"
                          << std::hex << dstAddr << std::dec << std::endl;
                ++warnCount;
            }
        }

        setReturnS32(ctx, 0);
    }

    void sceVu0ViewScreenMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceVu0ViewScreenMatrix", rdram, ctx, runtime);
    }
}
