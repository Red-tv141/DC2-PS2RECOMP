// G543 - exact, offline VU1 semantic replay shader.
//
// One invocation owns one complete G541 record. It starts from the captured pre-state, hidden
// scalar/MAC pipeline state, code RAM, and data RAM. It then executes the microprogram in program
// order and writes GENERATED post-state, post-pipeline, post-data, and ordered XGKICK bytes. The
// expected post-image is read only by the C++ oracle after the single bounded readback.
static const char *g543SemanticShaderSource()
{
    return R"G543A(#version 430 core
#pragma STDGL invariant(all)
layout(local_size_x=1, local_size_y=1, local_size_z=1) in;
layout(r32ui, binding=2) readonly uniform uimage2D gCommands;
layout(r32ui, binding=3) readonly uniform uimage2D gPayload;
layout(r32ui, binding=4) coherent uniform uimage2D gOutput;

const uint IMAGE_W = 4096u;
const uint ABI_MAGIC = 0x32435556u;
const uint STATUS_MAGIC = 0x4d455347u; // little-endian "GSEM"
const uint F_UNSUPPORTED_UPPER = 0x00000002u;
const uint F_UNSUPPORTED_LOWER = 0x00000004u;
const uint F_LAYOUT = 0x00000008u;
const uint F_RESULT = 0x00000010u;
const uint F_PACKET = 0x00000020u;
const uint F_PIPE = 0x00000040u;

ivec2 lc(uint i) { return ivec2(int(i & 4095u), int(i >> 12u)); }
uint cw(uint i) { return imageLoad(gCommands, lc(i)).r; }
uint pw(uint i) { return imageLoad(gPayload, lc(i)).r; }
uint ow(uint i) { return imageLoad(gOutput, lc(i)).r; }
void sw(uint i, uint v) { imageStore(gOutput, lc(i), uvec4(v, 0u, 0u, 0u)); }

uint gCmd;
uint gErrors;
uint gFailPc;
uint gFailWord;
uint gTop;
uint gCodeByte;
uint gPreDataByte;
uint gPostStateByte;
uint gPostPipeByte;
uint gPostDataByte;
uint gPacketDescByte;
uint gPacketBytesByte;
uint gExpectedPackets;
uint gExpectedPacketBytes;
uint gPacketCount;
uint gPacketBytes;

uvec4 Rvf[32];
int Rvi[16];
uvec4 Racc;
uint Rq;
uint Rp;
uint Ri;
uint Rpc;
uint Rmac;
uint Rclip;
uint Rstatus;
uint Rebit;
uint Ritop;
uint Rxitop;
uvec4 Rram[1024];

uint Qpending;
int Qdelay;
uint Ppending;
int Pdelay;
uint MacVisible;
uint StatusVisible;
uint G428Claim;

uvec4 Hvec[8];
uvec2 Hstamp[8];
uint Hmac[8];
uint Hdest[8];
uint Hkind[8];
int Hhead;
uvec2 PairBase;
bool HistArmed;

// G545 v27 default-off offline diagnostics: four visits to the Dungeon clipper's
// OPMSUB/FMAND sign-bit gate, plus a ring of the last eight VF25 changes before its first
// divergent SQI. Kept in invocation-private shader storage and published only in G543 status.
uint DebugWords[160];
uint DebugCount;
uint DebugVf25Count;
uint DebugOpmulaA;
uint DebugOpmulaB;

void fail(uint bit, uint word)
{
    if (gErrors == 0u) {
        gFailPc = Rpc;
        gFailWord = word;
    }
    gErrors |= bit;
}

int sx16(uint v) { return int(v << 16u) >> 16; }
int sx11(uint v) { return int(v << 21u) >> 21; }
uint u16(int v) { return uint(v) & 0xffffu; }
uint s2func(uint v) { return (((v >> 6u) & 31u) << 2u) | (v & 3u); }
bool laneOn(uint d, int lane) { return (d & (8u >> uint(lane))) != 0u; }

uvec2 add64s(uvec2 a, int delta)
{
    uint d = uint(delta);
    uint lo = a.x + d;
    uint carry = lo < a.x ? 1u : 0u;
    uint hi = a.y + (delta < 0 ? 0xffffffffu : 0u) + carry;
    return uvec2(lo, hi);
}

uvec2 add64u(uvec2 a, uint delta)
{
    uint lo = a.x + delta;
    return uvec2(lo, a.y + (lo < a.x ? 1u : 0u));
}

bool i64Less(uvec2 a, uvec2 b)
{
    bool an = (a.y & 0x80000000u) != 0u;
    bool bn = (b.y & 0x80000000u) != 0u;
    if (an != bn) return an;
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
}

bool i64LE(uvec2 a, uvec2 b) { return a == b || i64Less(a, b); }
bool i64Greater(uvec2 a, uvec2 b) { return i64Less(b, a); }

)G543A"
R"G543E(
precise float fadd(float a, float b) { precise float r = a + b; return r; }
precise float fsub(float a, float b) { precise float r = a - b; return r; }
precise float fmul(float a, float b) { precise float r = a * b; return r; }

// Match SSE2 CVTTPS2DQ, used by the authoritative CPU FTOI path. GLSL leaves
// out-of-range float-to-int conversion undefined; NVIDIA clamps positive overflow to INT_MAX,
// while CVTTPS2DQ returns its integer-indefinite value (INT_MIN) for either-sign overflow/NaN.
uint cvttSse(float value)
{
    uint bits = floatBitsToUint(value);
    if ((bits & 0x7fffffffu) >= 0x4f000000u) return 0x80000000u;
    return uint(int(value));
}
// NVIDIA's native float sqrt/div are allowed to differ from SSE sqrtss/divss by an ulp. A double
// intermediate has enough precision to reproduce the correctly-rounded binary32 result used by
// the CPU oracle. RSQRT still rounds the root to binary32 before its separate division.
precise float fdiv(float a, float b)
{
    precise double q = double(a) / double(b);
    precise float r = float(q);
    return r;
}
precise float fsqrt(float a)
{
    precise double q = sqrt(double(a));
    precise float r = float(q);
    return r;
}
precise float fmadd(float a, float b, float c)
{
    precise float p = a * b;
    precise float r = c + p;
    return r;
}
precise float fmsub(float a, float b, float c)
{
    precise float p = a * b;
    precise float r = c - p;
    return r;
}

uint f32Exp(uint bits) { return (bits >> 23u) & 255u; }
bool f32Subnormal(uint bits) { return f32Exp(bits) == 0u && (bits & 0x7fffffu) != 0u; }
bool f32NaN(uint bits) { return f32Exp(bits) == 255u && (bits & 0x7fffffu) != 0u; }
bool f32Inf(uint bits) { return (bits & 0x7fffffffu) == 0x7f800000u; }
bool f32Zero(uint bits) { return (bits & 0x7fffffffu) == 0u; }
uint quietSseNaN(uint bits) { return bits | 0x00400000u; }

// NVIDIA flushes binary32 denormals at arithmetic inputs and outputs. The CPU oracle uses SSE2
// with denormals preserved. Stay on native float for ordinary lanes; only the rare tiny lane takes
// this exact finite-bit -> double -> round-to-nearest-even binary32 repair path.
double f32FiniteToDouble(uint bits)
{
    uint exponent = f32Exp(bits);
    uint mantissa = bits & 0x7fffffu;
    double magnitude = exponent == 0u
        ? ldexp(double(mantissa), -149)
        : ldexp(double(0x800000u | mantissa), int(exponent) - 150);
    return (bits & 0x80000000u) != 0u ? -magnitude : magnitude;
}

uint roundEvenPositive(double scaled)
{
    double floored = floor(scaled);
    uint base = uint(floored);
    double fraction = scaled - floored;
    if (fraction > 0.5 || (fraction == 0.5 && (base & 1u) != 0u)) ++base;
    return base;
}

uint roundDoubleToF32(double value, uint nativeZero)
{
    if (value == 0.0) return nativeZero & 0x80000000u;
    bool negative = value < 0.0;
    double magnitude = abs(value);
    int exponent = 0;
    double fraction = frexp(magnitude, exponent);
    uint sign = negative ? 0x80000000u : 0u;
    if (exponent <= -126) {
        uint mantissa = roundEvenPositive(ldexp(magnitude, 149));
        if (mantissa == 0u) return sign;
        if (mantissa >= 0x800000u) return sign | 0x00800000u;
        return sign | mantissa;
    }
    uint significand = roundEvenPositive(ldexp(fraction, 24));
    int field = exponent + 126;
    if (significand >= 0x1000000u) {
        significand = 0x800000u;
        ++field;
    }
    if (field >= 255) return sign | 0x7f800000u;
    return sign | (uint(field) << 23u) | (significand & 0x7fffffu);
}

uint fmulBits(uint aBits, uint bBits)
{
    // SSE arithmetic returns the first NaN operand (quieted), preserving sign/payload. NVIDIA
    // GLSL canonicalizes these lanes to a driver-specific payload. Its invalid-operation NaN is
    // also different, so spell out the CPU oracle's 0xffc00000 result before native arithmetic.
    if (f32NaN(aBits)) return quietSseNaN(aBits);
    if (f32NaN(bBits)) return quietSseNaN(bBits);
    if ((f32Inf(aBits) && f32Zero(bBits)) || (f32Zero(aBits) && f32Inf(bBits)))
        return 0xffc00000u;
    uint native = floatBitsToUint(fmul(uintBitsToFloat(aBits), uintBitsToFloat(bBits)));
    if (f32Inf(aBits) || f32Inf(bBits)) return native;
    if (!f32Subnormal(aBits) && !f32Subnormal(bBits) && f32Exp(native) != 0u) return native;
    return roundDoubleToF32(f32FiniteToDouble(aBits) * f32FiniteToDouble(bBits), native);
}

uint faddBits(uint aBits, uint bBits)
{
    if (f32NaN(aBits)) return quietSseNaN(aBits);
    if (f32NaN(bBits)) return quietSseNaN(bBits);
    if (f32Inf(aBits) && f32Inf(bBits) && ((aBits ^ bBits) & 0x80000000u) != 0u)
        return 0xffc00000u;
    uint native = floatBitsToUint(fadd(uintBitsToFloat(aBits), uintBitsToFloat(bBits)));
    if (f32Inf(aBits) || f32Inf(bBits)) return native;
    if (!f32Subnormal(aBits) && !f32Subnormal(bBits) && f32Exp(native) != 0u) return native;
    return roundDoubleToF32(f32FiniteToDouble(aBits) + f32FiniteToDouble(bBits), native);
}

uint fsubBits(uint aBits, uint bBits)
{
    if (f32NaN(aBits)) return quietSseNaN(aBits);
    if (f32NaN(bBits)) return quietSseNaN(bBits);
    if (f32Inf(aBits) && f32Inf(bBits) && ((aBits ^ bBits) & 0x80000000u) == 0u)
        return 0xffc00000u;
    uint native = floatBitsToUint(fsub(uintBitsToFloat(aBits), uintBitsToFloat(bBits)));
    if (f32Inf(aBits) || f32Inf(bBits)) return native;
    if (!f32Subnormal(aBits) && !f32Subnormal(bBits) && f32Exp(native) != 0u) return native;
    return roundDoubleToF32(f32FiniteToDouble(aBits) - f32FiniteToDouble(bBits), native);
}

uint fmaddBits(uint aBits, uint bBits, uint cBits)
{
    return faddBits(cBits, fmulBits(aBits, bBits));
}

uint fmsubBits(uint aBits, uint bBits, uint cBits)
{
    return fsubBits(cBits, fmulBits(aBits, bBits));
}

void applyBits(inout uvec4 dst, uvec4 value, uint dest)
{
    for (int lane = 0; lane < 4; ++lane)
        if (laneOn(dest, lane)) dst[lane] = value[lane];
}

uint makeMac(uvec4 value, uint dest)
{
    uint m = 0u;
    for (int lane = 0; lane < 4; ++lane) {
        if (!laneOn(dest, lane)) continue;
        uint bits = value[lane];
        uint exp = (bits >> 23u) & 255u;
        uint man = bits & 0x7fffffu;
        uint b = uint(3 - lane);
        if (exp == 0u && man == 0u) m |= 1u << b;
        else if (exp == 0u) m |= 1u << (b + 8u);
        else if (exp == 255u) m |= 1u << (b + 12u);
        if ((bits & 0x80000000u) != 0u) m |= 1u << (b + 4u);
    }
    return m;
}

void updateStatus(uint mac)
{
    uint cur = 0u;
    if ((mac & 0x000fu) != 0u) cur |= 1u;
    if ((mac & 0x00f0u) != 0u) cur |= 2u;
    if ((mac & 0x0f00u) != 0u) cur |= 4u;
    if ((mac & 0xf000u) != 0u) cur |= 8u;
    Rstatus = (Rstatus & 0xff0u) | cur | (cur << 6u);
}

void pushPending(uvec2 stamp, uvec4 value, uint dest)
{
    int s = Hhead;
    Hvec[s] = value;
    Hstamp[s] = stamp;
    Hdest[s] = dest;
    Hkind[s] = 1u;
    Hhead = (s + 1) & 7;
}

void pushEager(uvec2 stamp, uint mac)
{
    int s = Hhead;
    Hstamp[s] = stamp;
    Hmac[s] = mac;
    Hkind[s] = 2u;
    Hhead = (s + 1) & 7;
}

uint resolveMac(uvec2 target)
{
    int best = -1;
    uvec2 bestStamp = uvec2(0u, 0x80000000u);
    for (int s = 0; s < 8; ++s) {
        if (i64LE(Hstamp[s], target) && i64Greater(Hstamp[s], bestStamp)) {
            bestStamp = Hstamp[s];
            best = s;
        }
    }
    if (best < 0) return 0u;
    if (Hkind[best] == 1u) return makeMac(Hvec[best], Hdest[best]);
    return Hmac[best];
}

uint pipeIn(uint word) { return pw((cw(gCmd + 17u) >> 2u) + word); }
void pipeOut(uint word, uint value) { sw((gPostPipeByte >> 2u) + word, value); }

void armHistory()
{
    if (HistArmed) return;
    uint deferredArmed = pipeIn(81u);
    if (deferredArmed != 0u) {
        fail(F_PIPE, deferredArmed);
        return;
    }

    // Default G428 transition performed by run() before G430 seeds its stamped history.
    for (uint s = 0u; s < 8u; ++s) pipeOut(65u + s, 2u);
    G428Claim = 0u;
    pipeOut(79u, 2u);
    pipeOut(80u, Rmac);
    pipeOut(81u, 1u);

    for (int s = 0; s < 8; ++s) Hstamp[s] = uvec2(0u, 0x80000000u);
    Hhead = 0;
    int legacyHead = int(pipeIn(24u));
    for (int age = 3; age >= 0; --age) {
        int slot = legacyHead + age;
        if (slot >= 4) slot -= 4;
        pushEager(add64s(PairBase, -1 - age), pipeIn(6u + uint(slot)));
    }
    HistArmed = true;
}

void finishFmac(uvec4 value, uint dest, bool toAcc, int vfDest, bool eager, uvec2 stamp)
{
    if (eager) {
        Rmac = makeMac(value, dest);
        updateStatus(Rmac);
    } else {
        pushPending(stamp, value, dest);
    }
    if (toAcc) applyBits(Racc, value, dest);
    else applyBits(Rvf[vfDest], value, dest);
}

uvec4 vecBinary(uvec4 aBits, uvec4 bBits, uint kind)
{
    uvec4 r;
    for (int n = 0; n < 4; ++n) {
        if (kind == 0u) r[n] = faddBits(aBits[n], bBits[n]);
        else if (kind == 1u) r[n] = fsubBits(aBits[n], bBits[n]);
        else if (kind == 2u) r[n] = fmulBits(aBits[n], bBits[n]);
        else {
            float a = uintBitsToFloat(aBits[n]);
            float b = uintBitsToFloat(bBits[n]);
            r[n] = kind == 3u ? (a > b ? aBits[n] : bBits[n])
                              : (a < b ? aBits[n] : bBits[n]);
        }
    }
    return r;
}

uvec4 vecScalar(uvec4 aBits, uint scalarBits, uint kind)
{
    uvec4 r;
    for (int n = 0; n < 4; ++n) {
        if (kind == 0u) r[n] = faddBits(aBits[n], scalarBits);
        else if (kind == 1u) r[n] = fsubBits(aBits[n], scalarBits);
        else if (kind == 2u) r[n] = fmulBits(aBits[n], scalarBits);
        else {
            float a = uintBitsToFloat(aBits[n]);
            float b = uintBitsToFloat(scalarBits);
            r[n] = kind == 3u ? (a > b ? aBits[n] : scalarBits)
                              : (a < b ? aBits[n] : scalarBits);
        }
    }
    return r;
}

uvec4 vecMadd(uvec4 accBits, uvec4 aBits, uvec4 bBits, bool sub)
{
    uvec4 r;
    for (int n = 0; n < 4; ++n)
        r[n] = sub ? fmsubBits(aBits[n], bBits[n], accBits[n])
                   : fmaddBits(aBits[n], bBits[n], accBits[n]);
    return r;
}

uvec4 vecMaddScalar(uvec4 accBits, uvec4 aBits, uint scalarBits, bool sub)
{
    uvec4 r;
    for (int n = 0; n < 4; ++n)
        r[n] = sub ? fmsubBits(aBits[n], scalarBits, accBits[n])
                   : fmaddBits(aBits[n], scalarBits, accBits[n]);
    return r;
}
)G543E"
R"G543B(

int upperVfDest(uint instr)
{
    uint op = instr & 63u;
    if (op <= 0x2fu) return int((instr >> 6u) & 31u);
    if (op >= 0x3cu) {
        uint fn = s2func(instr);
        if ((fn >= 0x10u && fn <= 0x17u) || fn == 0x1du || fn == 0x30u || fn == 0x31u)
            return int((instr >> 16u) & 31u);
    }
    return -1;
}

void execUpper(uint instr, bool eager, uvec2 stamp)
{
    uint dest = (instr >> 21u) & 15u;
    int ft = int((instr >> 16u) & 31u);
    int fs = int((instr >> 11u) & 31u);
    int fd = int((instr >> 6u) & 31u);
    uint op = instr & 63u;
    uvec4 value;

    if (op <= 3u) value = vecScalar(Rvf[fs], Rvf[ft][int(op)], 0u);
    else if (op <= 7u) value = vecScalar(Rvf[fs], Rvf[ft][int(op & 3u)], 1u);
    else if (op <= 0x0bu) value = vecMaddScalar(Racc, Rvf[fs], Rvf[ft][int(op & 3u)], false);
    else if (op <= 0x0fu) value = vecMaddScalar(Racc, Rvf[fs], Rvf[ft][int(op & 3u)], true);
    else if (op <= 0x13u) value = vecScalar(Rvf[fs], Rvf[ft][int(op & 3u)], 3u);
    else if (op <= 0x17u) value = vecScalar(Rvf[fs], Rvf[ft][int(op & 3u)], 4u);
    else if (op <= 0x1bu) value = vecScalar(Rvf[fs], Rvf[ft][int(op & 3u)], 2u);
    else if (op == 0x1cu) value = vecScalar(Rvf[fs], Rq, 2u);
    else if (op == 0x1du) value = vecScalar(Rvf[fs], Ri, 3u);
    else if (op == 0x1eu) value = vecScalar(Rvf[fs], Ri, 2u);
    else if (op == 0x1fu) value = vecScalar(Rvf[fs], Ri, 4u);
    else if (op == 0x20u) value = vecScalar(Rvf[fs], Rq, 0u);
    else if (op == 0x21u) value = vecMaddScalar(Racc, Rvf[fs], Rq, false);
    else if (op == 0x22u) value = vecScalar(Rvf[fs], Ri, 0u);
    else if (op == 0x23u) value = vecMaddScalar(Racc, Rvf[fs], Ri, false);
    else if (op == 0x24u) value = vecScalar(Rvf[fs], Rq, 1u);
    else if (op == 0x25u) value = vecMaddScalar(Racc, Rvf[fs], Rq, true);
    else if (op == 0x26u) value = vecScalar(Rvf[fs], Ri, 1u);
    else if (op == 0x27u) value = vecMaddScalar(Racc, Rvf[fs], Ri, true);
    else if (op == 0x28u) value = vecBinary(Rvf[fs], Rvf[ft], 0u);
    else if (op == 0x29u) value = vecMadd(Racc, Rvf[fs], Rvf[ft], false);
    else if (op == 0x2au) value = vecBinary(Rvf[fs], Rvf[ft], 2u);
    else if (op == 0x2bu) value = vecBinary(Rvf[fs], Rvf[ft], 3u);
    else if (op == 0x2cu) value = vecBinary(Rvf[fs], Rvf[ft], 1u);
    else if (op == 0x2du) value = vecMadd(Racc, Rvf[fs], Rvf[ft], true);
    else if (op == 0x2eu) {
        value.x = fmsubBits(Rvf[fs].y, Rvf[ft].z, Racc.x);
        value.y = fmsubBits(Rvf[fs].z, Rvf[ft].x, Racc.y);
        value.z = fmsubBits(Rvf[fs].x, Rvf[ft].y, Racc.z);
        value.w = 0u;
    }
    else if (op == 0x2fu) value = vecBinary(Rvf[fs], Rvf[ft], 4u);
    else if (op >= 0x3cu) {
        uint fn = s2func(instr);
        bool toAcc = false;
        bool haveFmac = false;
        if (fn <= 0x0fu) {
            uint group = fn >> 2u;
            uint scalar = Rvf[ft][int(fn & 3u)];
            if (group == 0u) value = vecScalar(Rvf[fs], scalar, 0u);
            else if (group == 1u) value = vecScalar(Rvf[fs], scalar, 1u);
            else if (group == 2u) value = vecMaddScalar(Racc, Rvf[fs], scalar, false);
            else value = vecMaddScalar(Racc, Rvf[fs], scalar, true);
            toAcc = true; haveFmac = true;
        }
        else if (fn >= 0x10u && fn <= 0x13u) {
            float scale = fn == 0x10u ? 1.0 : (fn == 0x11u ? (1.0 / 16.0) :
                          (fn == 0x12u ? (1.0 / 4096.0) : (1.0 / 32768.0)));
            vec4 r;
            for (int n = 0; n < 4; ++n) r[n] = fmul(float(int(Rvf[fs][n])), scale);
            applyBits(Rvf[ft], floatBitsToUint(r), dest);
            return;
        }
        else if (fn >= 0x14u && fn <= 0x17u) {
            float scale = fn == 0x14u ? 1.0 : (fn == 0x15u ? 16.0 :
                          (fn == 0x16u ? 4096.0 : 32768.0));
            vec4 a = uintBitsToFloat(Rvf[fs]);
            uvec4 r;
            for (int n = 0; n < 4; ++n) r[n] = cvttSse(fmul(a[n], scale));
            applyBits(Rvf[ft], r, dest);
            return;
        }
        else if (fn >= 0x18u && fn <= 0x1bu) {
            value = vecScalar(Rvf[fs], Rvf[ft][int(fn & 3u)], 2u);
            toAcc = true; haveFmac = true;
        }
        else if (fn == 0x1cu) { value = vecScalar(Rvf[fs], Rq, 2u); toAcc = true; haveFmac = true; }
        else if (fn == 0x1du) {
            uvec4 r = Rvf[fs] & uvec4(0x7fffffffu);
            applyBits(Rvf[ft], r, dest); return;
        }
        else if (fn == 0x1eu) { value = vecScalar(Rvf[fs], Ri, 2u); toAcc = true; haveFmac = true; }
        else if (fn == 0x1fu) {
            vec4 a = uintBitsToFloat(Rvf[fs]);
            float w = abs(uintBitsToFloat(Rvf[ft].w));
            uint f = 0u;
            if (a.x > w) f |= 1u; if (a.x < -w) f |= 2u;
            if (a.y > w) f |= 4u; if (a.y < -w) f |= 8u;
            if (a.z > w) f |= 16u; if (a.z < -w) f |= 32u;
            Rclip = (Rclip << 6u) | f; return;
        }
        else if (fn == 0x20u) { value = vecScalar(Rvf[fs], Rq, 0u); toAcc = true; haveFmac = true; }
        else if (fn == 0x21u) { value = vecMaddScalar(Racc, Rvf[fs], Rq, false); toAcc = true; haveFmac = true; }
        else if (fn == 0x22u) { value = vecScalar(Rvf[fs], Ri, 0u); toAcc = true; haveFmac = true; }
        else if (fn == 0x23u) { value = vecMaddScalar(Racc, Rvf[fs], Ri, false); toAcc = true; haveFmac = true; }
        else if (fn == 0x24u) { value = vecScalar(Rvf[fs], Rq, 1u); toAcc = true; haveFmac = true; }
        else if (fn == 0x25u) { value = vecMaddScalar(Racc, Rvf[fs], Rq, true); toAcc = true; haveFmac = true; }
        else if (fn == 0x26u) { value = vecScalar(Rvf[fs], Ri, 1u); toAcc = true; haveFmac = true; }
        else if (fn == 0x27u) { value = vecMaddScalar(Racc, Rvf[fs], Ri, true); toAcc = true; haveFmac = true; }
        else if (fn == 0x28u) { value = vecBinary(Rvf[fs], Rvf[ft], 0u); toAcc = true; haveFmac = true; }
        else if (fn == 0x29u) { value = vecMadd(Racc, Rvf[fs], Rvf[ft], false); toAcc = true; haveFmac = true; }
        else if (fn == 0x2au) { value = vecBinary(Rvf[fs], Rvf[ft], 2u); toAcc = true; haveFmac = true; }
        else if (fn == 0x2cu) { value = vecBinary(Rvf[fs], Rvf[ft], 1u); toAcc = true; haveFmac = true; }
        else if (fn == 0x2du) { value = vecMadd(Racc, Rvf[fs], Rvf[ft], true); toAcc = true; haveFmac = true; }
        else if (fn == 0x2eu) {
            value.x = fmulBits(Rvf[fs].y, Rvf[ft].z);
            value.y = fmulBits(Rvf[fs].z, Rvf[ft].x);
            value.z = fmulBits(Rvf[fs].x, Rvf[ft].y);
            value.w = Racc.w; toAcc = true; haveFmac = true;
        }
        else if (fn == 0x2fu) return;
        else if (fn == 0x30u) { applyBits(Rvf[ft], Rvf[fs], dest); return; }
        else if (fn == 0x31u) {
            uvec4 a = Rvf[fs]; applyBits(Rvf[ft], a.yzwx, dest); return;
        }
        else { fail(F_UNSUPPORTED_UPPER, instr); return; }
        if (haveFmac) { finishFmac(value, dest, toAcc, ft, eager, stamp); return; }
    }
    else { fail(F_UNSUPPORTED_UPPER, instr); return; }

    finishFmac(value, dest, false, fd, eager, stamp);
}

uint ramByte(uint byteAddr)
{
    uint a = byteAddr & 0x3fffu;
    uint q = a >> 4u;
    uint lane = (a >> 2u) & 3u;
    uint shift = (a & 3u) * 8u;
    return (Rram[q][int(lane)] >> shift) & 255u;
}

void storeOutByte(uint byteAddr, uint value)
{
    uint wi = byteAddr >> 2u;
    uint shift = (byteAddr & 3u) * 8u;
    uint mask = 255u << shift;
    sw(wi, (ow(wi) & ~mask) | ((value & 255u) << shift));
}

void storeOutU32(uint byteAddr, uint value)
{
    for (uint b = 0u; b < 4u; ++b) storeOutByte(byteAddr + b, value >> (b * 8u));
}

uint readRamU32(uint byteAddr)
{
    uint v = 0u;
    for (uint b = 0u; b < 4u; ++b) v |= ramByte(byteAddr + b) << (b * 8u);
    return v;
}

void emitKick(int isReg, uint instr)
{
    uint addr = (u16(Rvi[isReg]) * 16u) & 0x3fffu;
    uint off = addr;
    uint total = 0u;
    bool done = false;
    for (int safety = 0; safety < 256 && !done; ++safety) {
        uint lo = readRamU32(off);
        uint hi = readRamU32(off + 4u);
        uint nloop = lo & 0x7fffu;
        uint flg = (hi >> 26u) & 3u;
        uint nreg = (hi >> 28u) & 15u;
        if (nreg == 0u) nreg = 16u;
        uint size = 16u;
        if (flg == 0u) size += nloop * nreg * 16u;
        else if (flg == 1u) {
            uint regs = nloop * nreg;
            size += regs * 8u + ((regs & 1u) != 0u ? 8u : 0u);
        } else if (flg == 2u) size += nloop * 16u;
        total += size;
        off = (off + size) & 0x3fffu;
        done = ((lo >> 15u) & 1u) != 0u;
    }
    if (total == 0u) return;
    if (gPacketCount >= gExpectedPackets || total > gExpectedPacketBytes - min(gPacketBytes, gExpectedPacketBytes)) {
        fail(F_PACKET, instr);
        return;
    }
    storeOutU32(gPacketDescByte + gPacketCount * 8u, addr);
    storeOutU32(gPacketDescByte + gPacketCount * 8u + 4u, total);
    for (uint b = 0u; b < total; ++b)
        storeOutByte(gPacketBytesByte + gPacketBytes + b, ramByte(addr + b));
    ++gPacketCount;
    gPacketBytes += total;
}
)G543B"
R"G543C(

void stageP(uint result, int latency)
{
    if (Pdelay > 0) Rp = Ppending;
    Ppending = result;
    Pdelay = latency;
}

bool lowerReadsMac(uint op) { return op == 0x18u || op == 0x1au || op == 0x1bu; }
bool lowerReadsStatus(uint op) { return op == 0x14u || op == 0x16u || op == 0x17u; }

void execLower(uint instr)
{
    if (instr == 0u || instr == 0x8000033cu) return;
    uint op = (instr >> 25u) & 0x7fu;
    int it = int((instr >> 16u) & 31u);
    int isr = int((instr >> 11u) & 31u);
    int id = int((instr >> 6u) & 31u);
    uint dest = (instr >> 21u) & 15u;

    if (op == 0x00u || op == 0x01u || op == 0x04u || op == 0x05u) {
        int imm = sx11(instr);
        int baseReg = (op == 0x01u) ? it : isr;
        uint q = uint(Rvi[baseReg] + imm) & 0x3ffu;
        if (op == 0x00u) applyBits(Rvf[it], Rram[q], dest);
        else if (op == 0x01u) {
            uvec4 source = Rvf[isr];
            applyBits(Rram[q], source, dest);
        }
        else if (op == 0x04u) {
            int comp = (dest & 8u) != 0u ? 0 : ((dest & 4u) != 0u ? 1 : ((dest & 2u) != 0u ? 2 : 3));
            if (it != 0) Rvi[it] = sx16(Rram[q][comp]);
        } else {
            uint v = u16(Rvi[it]);
            for (int n = 0; n < 4; ++n) if (laneOn(dest, n)) Rram[q][n] = v;
        }
        return;
    }
    if (op == 0x08u || op == 0x09u) {
        int imm = sx16((instr & 0x7ffu) | ((instr >> 10u) & 0x7800u));
        if (it != 0) Rvi[it] = sx16(uint(op == 0x08u ? Rvi[isr] + imm : Rvi[isr] - imm));
        return;
    }
    if (op == 0x10u) { Rvi[1] = ((Rclip & 0xffffffu) == (instr & 0xffffffu)) ? 1 : 0; return; }
    if (op == 0x11u) { Rclip = instr & 0xffffffu; return; }
    if (op == 0x12u) { Rvi[1] = ((Rclip & (instr & 0xffffffu)) != 0u) ? 1 : 0; return; }
    if (op == 0x13u) { Rvi[1] = ((Rclip | (instr & 0xffffffu)) == 0xffffffu) ? 1 : 0; return; }
    if (op == 0x14u) { Rvi[1] = ((StatusVisible & 0xfffu) == (instr & 0xfffu)) ? 1 : 0; return; }
    if (op == 0x15u) { Rstatus = (instr >> 6u) & 0xfc0u; return; }
    if (op == 0x16u) { Rvi[1] = int(StatusVisible & (instr & 0xfffu)); return; }
    if (op == 0x17u) { Rvi[1] = ((StatusVisible | (instr & 0xfffu)) == 0xfffu) ? 1 : 0; return; }
    if (op == 0x18u) { if (it != 0) Rvi[it] = ((MacVisible & 0xffffu) == u16(Rvi[isr])) ? 1 : 0; return; }
    if (op == 0x1au) { if (it != 0) Rvi[it] = int(MacVisible & u16(Rvi[isr])); return; }
    if (op == 0x1bu) { if (it != 0) Rvi[it] = int(MacVisible | u16(Rvi[isr])); return; }
    if (op == 0x1cu) { if (it != 0) Rvi[it] = int(Rclip & 0xfffu); return; }

    if (op == 0x20u || op == 0x21u || op == 0x28u || op == 0x29u ||
        op == 0x2cu || op == 0x2du || op == 0x2eu || op == 0x2fu) {
        bool take = false;
        int imm = sx11(instr);
        if (op == 0x20u || op == 0x21u) take = true;
        else if (op == 0x28u) take = sx16(u16(Rvi[isr])) == sx16(u16(Rvi[it]));
        else if (op == 0x29u) take = sx16(u16(Rvi[isr])) != sx16(u16(Rvi[it]));
        else if (op == 0x2cu) take = sx16(u16(Rvi[isr])) < 0;
        else if (op == 0x2du) take = sx16(u16(Rvi[isr])) > 0;
        else if (op == 0x2eu) take = sx16(u16(Rvi[isr])) <= 0;
        else take = sx16(u16(Rvi[isr])) >= 0;
        if (op == 0x21u && it != 0) Rvi[it] = int((Rpc + 16u) / 8u);
        if (take) {
            uint target = (Rpc + 8u + uint(imm * 8)) & 0x3fffu;
            Rpc = target - 8u;
        }
        return;
    }
    if (op == 0x24u || op == 0x25u) {
        uint target = (u16(Rvi[isr]) * 8u) & 0x3fffu;
        if (op == 0x25u && it != 0) Rvi[it] = int((Rpc + 16u) / 8u);
        Rpc = target - 8u;
        return;
    }

    if (op != 0x40u) { fail(F_UNSUPPORTED_LOWER, instr); return; }
    uint fn6 = instr & 63u;
    if (fn6 == 0x30u) { if (id != 0) Rvi[id] = sx16(uint(Rvi[isr] + Rvi[it])); return; }
    if (fn6 == 0x31u) { if (id != 0) Rvi[id] = sx16(uint(Rvi[isr] - Rvi[it])); return; }
    if (fn6 == 0x32u) { int im = int(((instr >> 6u) & 31u) << 27u) >> 27; if (it != 0) Rvi[it] = sx16(uint(Rvi[isr] + im)); return; }
    if (fn6 == 0x34u) { if (id != 0) Rvi[id] = Rvi[isr] & Rvi[it]; return; }
    if (fn6 == 0x35u) { if (id != 0) Rvi[id] = Rvi[isr] | Rvi[it]; return; }
    if (fn6 < 0x3cu) { fail(F_UNSUPPORTED_LOWER, instr); return; }

    uint fn = s2func(instr);
    if (fn == 0x30u) { applyBits(Rvf[it], Rvf[isr], dest); return; }
    if (fn == 0x31u) { applyBits(Rvf[it], Rvf[isr].yzwx, dest); return; }
    if (fn >= 0x34u && fn <= 0x37u) {
        bool load = fn == 0x34u || fn == 0x36u;
        bool predec = fn == 0x36u || fn == 0x37u;
        int ar = load ? isr : it;
        if (predec && ar != 0) Rvi[ar] = sx16(uint(Rvi[ar] - 1));
        uint q = u16(Rvi[ar]) & 0x3ffu;
        if (load) applyBits(Rvf[it], Rram[q], dest);
        else {
            uvec4 source = Rvf[isr];
            applyBits(Rram[q], source, dest);
        }
        if (!predec && ar != 0) Rvi[ar] = sx16(uint(Rvi[ar] + 1));
        return;
    }
    if (fn == 0x38u || fn == 0x39u || fn == 0x3au) {
        int fsf = int((instr >> 21u) & 3u);
        int ftf = int((instr >> 23u) & 3u);
        float num = uintBitsToFloat(Rvf[isr][fsf]);
        float den = uintBitsToFloat(Rvf[it][ftf]);
        float outv;
        if (fn == 0x38u) outv = den != 0.0 ? fdiv(num, den) : (num >= 0.0 ? uintBitsToFloat(0x7f7fffffu) : uintBitsToFloat(0xff7fffffu));
        else if (fn == 0x39u) outv = fsqrt(abs(den));
        else {
            precise float root = fsqrt(abs(den));
            outv = root != 0.0 ? fdiv(num, root) : uintBitsToFloat(0x7f7fffffu);
        }
        Qpending = floatBitsToUint(outv);
        Qdelay = fn == 0x3au ? 13 : 7;
        return;
    }
    if (fn == 0x3bu) { if (Qdelay > 0) { Rq = Qpending; Qdelay = 0; } return; }
    if (fn == 0x3cu) { int comp = int((instr >> 21u) & 3u); if (it != 0) Rvi[it] = sx16(Rvf[isr][comp]); return; }
    if (fn == 0x3du) { uvec4 v = uvec4(uint(sx16(u16(Rvi[isr])))); applyBits(Rvf[it], v, dest); return; }
    if (fn == 0x3eu) {
        if (it != 0) {
            uint q = u16(Rvi[isr]) & 0x3ffu; uint v = 0u;
            for (int n = 0; n < 4; ++n) if (laneOn(dest, n)) v = Rram[q][n];
            Rvi[it] = sx16(v);
        }
        return;
    }
    if (fn == 0x3fu) {
        uint q = u16(Rvi[isr]) & 0x3ffu; uint v = u16(Rvi[it]);
        for (int n = 0; n < 4; ++n) if (laneOn(dest, n)) Rram[q][n] = v;
        return;
    }
    if (fn >= 0x40u && fn <= 0x43u) return;
    if (fn == 0x64u) { applyBits(Rvf[it], uvec4(Rp), dest); return; }
    if (fn == 0x68u) { if (it != 0) Rvi[it] = int(gTop & 0x3ffu); return; }
    if (fn == 0x69u) { if (it != 0) Rvi[it] = int(Ritop & 0x3ffu); return; }
    if (fn == 0x6cu) { emitKick(isr, instr); return; }

    vec4 a = uintBitsToFloat(Rvf[isr]);
    if (fn == 0x70u) {
        precise float s = fadd(fadd(fmul(a.x,a.x), fmul(a.y,a.y)), fmul(a.z,a.z));
        stageP(floatBitsToUint(s), 11); return;
    }
    if (fn == 0x71u) {
        precise float s = fadd(fadd(fmul(a.x,a.x), fmul(a.y,a.y)), fmul(a.z,a.z));
        stageP(floatBitsToUint(s != 0.0 ? fdiv(1.0,s) : 0.0), 18); return;
    }
    if (fn == 0x72u || fn == 0x73u) {
        precise float s = fadd(fadd(fmul(a.x,a.x), fmul(a.y,a.y)), fmul(a.z,a.z));
        precise float len = fsqrt(abs(s));
        stageP(floatBitsToUint(fn == 0x72u ? len : (len != 0.0 ? fdiv(1.0,len) : 0.0)), fn == 0x72u ? 18 : 24); return;
    }
    if (fn == 0x76u) {
        precise float s = fadd(fadd(fadd(a.x,a.y),a.z),a.w); stageP(floatBitsToUint(s),12); return;
    }
    if (fn == 0x78u || fn == 0x79u || fn == 0x7au) {
        int fsf = int((instr >> 21u) & 3u); precise float v = a[fsf]; precise float z;
        if (fn == 0x78u) z = fsqrt(abs(v));
        else if (fn == 0x79u) { precise float root=fsqrt(abs(v)); z=root!=0.0?fdiv(1.0,root):0.0; }
        else z=v!=0.0?fdiv(1.0,v):0.0;
        stageP(floatBitsToUint(z), fn == 0x79u ? 18 : 12); return;
    }
    if (fn == 0x7bu) { if (Pdelay > 0) { Rp = Ppending; Pdelay = 0; } return; }
    if (fn == 0x74u || fn == 0x75u || fn == 0x77u || fn == 0x7cu || fn == 0x7du || fn == 0x7eu) return;
    fail(F_UNSUPPORTED_LOWER, instr);
}

)G543C"
R"G543D(
void loadRecord()
{
    uint preState = cw(gCmd + 15u) >> 2u;
    for (int r = 0; r < 32; ++r)
        Rvf[r] = uvec4(pw(preState + uint(r * 4 + 0)), pw(preState + uint(r * 4 + 1)),
                       pw(preState + uint(r * 4 + 2)), pw(preState + uint(r * 4 + 3)));
    for (int r = 0; r < 16; ++r) Rvi[r] = int(pw(preState + 128u + uint(r)));
    Racc = uvec4(pw(preState + 144u), pw(preState + 145u), pw(preState + 146u), pw(preState + 147u));
    Rq = pw(preState + 148u); Rp = pw(preState + 149u); Ri = pw(preState + 150u);
    Rpc = pw(preState + 151u); Rmac = pw(preState + 152u); Rclip = pw(preState + 153u);
    Rstatus = pw(preState + 154u); Rebit = pw(preState + 155u); Ritop = pw(preState + 156u);
    Rxitop = pw(preState + 157u);

    uint prePipe = cw(gCmd + 17u) >> 2u;
    for (uint n = 0u; n < 161u; ++n) sw((gPostPipeByte >> 2u) + n, pw(prePipe + n));
    Qpending = pw(prePipe + 2u); Qdelay = int(pw(prePipe + 3u));
    Ppending = pw(prePipe + 4u); Pdelay = int(pw(prePipe + 5u));
    MacVisible = pw(prePipe + 22u); StatusVisible = pw(prePipe + 23u);
    G428Claim = pw(prePipe + 73u);
    for (int s = 0; s < 8; ++s) {
        Hvec[s] = uvec4(pw(prePipe + 82u + uint(s*4+0)), pw(prePipe + 82u + uint(s*4+1)),
                        pw(prePipe + 82u + uint(s*4+2)), pw(prePipe + 82u + uint(s*4+3)));
        Hstamp[s] = uvec2(pw(prePipe + 114u + uint(s*2)), pw(prePipe + 115u + uint(s*2)));
        Hmac[s] = pw(prePipe + 130u + uint(s));
        Hdest[s] = pw(prePipe + 138u + uint(s));
        Hkind[s] = pw(prePipe + 146u + uint(s));
    }
    Hhead = int(pw(prePipe + 154u));
    PairBase = uvec2(pw(prePipe + 155u), pw(prePipe + 156u));
    HistArmed = pw(prePipe + 157u) != 0u;

    uint preData = gPreDataByte >> 2u;
    for (int q = 0; q < 1024; ++q)
        Rram[q] = uvec4(pw(preData + uint(q*4+0)), pw(preData + uint(q*4+1)),
                        pw(preData + uint(q*4+2)), pw(preData + uint(q*4+3)));
}

void storeRecord()
{
    uint postState = gPostStateByte >> 2u;
    for (int r = 0; r < 32; ++r)
        for (int n = 0; n < 4; ++n) sw(postState + uint(r*4+n), Rvf[r][n]);
    for (int r = 0; r < 16; ++r) sw(postState + 128u + uint(r), uint(Rvi[r]));
    for (int n = 0; n < 4; ++n) sw(postState + 144u + uint(n), Racc[n]);
    sw(postState + 148u, Rq); sw(postState + 149u, Rp); sw(postState + 150u, Ri);
    sw(postState + 151u, Rpc); sw(postState + 152u, Rmac); sw(postState + 153u, Rclip);
    sw(postState + 154u, Rstatus); sw(postState + 155u, Rebit); sw(postState + 156u, Ritop);
    sw(postState + 157u, Rxitop);

    pipeOut(2u, Qpending); pipeOut(3u, uint(Qdelay));
    pipeOut(4u, Ppending); pipeOut(5u, uint(Pdelay));
    pipeOut(22u, MacVisible); pipeOut(23u, StatusVisible);
    pipeOut(73u, G428Claim);
    for (int s = 0; s < 8; ++s) {
        for (int n = 0; n < 4; ++n) pipeOut(82u + uint(s*4+n), Hvec[s][n]);
        pipeOut(114u + uint(s*2), Hstamp[s].x); pipeOut(115u + uint(s*2), Hstamp[s].y);
        pipeOut(130u + uint(s), Hmac[s]); pipeOut(138u + uint(s), Hdest[s]);
        pipeOut(146u + uint(s), Hkind[s]);
    }
    pipeOut(154u, uint(Hhead)); pipeOut(155u, PairBase.x); pipeOut(156u, PairBase.y);
    pipeOut(157u, HistArmed ? 1u : 0u);

    uint postData = gPostDataByte >> 2u;
    for (int q = 0; q < 1024; ++q)
        for (int n = 0; n < 4; ++n) sw(postData + uint(q*4+n), Rram[q][n]);
}

void main()
{
    uint rec = gl_GlobalInvocationID.x;
    gCmd = rec * 32u;
    gErrors = 0u; gFailPc = 0xffffffffu; gFailWord = 0u;
    gPacketCount = 0u; gPacketBytes = 0u;
    DebugCount = 0u;
    DebugVf25Count = 0u;
    for (int d = 0; d < 160; ++d) DebugWords[d] = 0xffffffffu;
    DebugOpmulaA = 0xffffffffu;
    DebugOpmulaB = 0xffffffffu;
    Rpc = 0xffffffffu;

    if (cw(gCmd) != ABI_MAGIC || cw(gCmd + 1u) != 1u || cw(gCmd + 2u) != 32u) gErrors |= 1u;
    gPostStateByte = cw(gCmd + 16u); gPostPipeByte = cw(gCmd + 18u);
    gCodeByte = cw(gCmd + 19u); gPreDataByte = cw(gCmd + 20u); gPostDataByte = cw(gCmd + 21u);
    gPacketDescByte = cw(gCmd + 26u); gExpectedPackets = cw(gCmd + 27u);
    gPacketBytesByte = cw(gCmd + 28u); gExpectedPacketBytes = cw(gCmd + 29u);
    gTop = cw(gCmd + 8u);
    if (gPostStateByte - cw(gCmd + 15u) != 632u || cw(gCmd + 17u) - gPostStateByte != 632u ||
        gPostPipeByte - cw(gCmd + 17u) != 644u || gCodeByte - gPostPipeByte != 644u ||
        gPreDataByte - gCodeByte != 16384u || gPostDataByte - gPreDataByte != 16384u)
        gErrors |= F_LAYOUT;

    loadRecord();
    armHistory();

    bool resume = (cw(gCmd + 3u) & 1u) != 0u;
    Rebit = 0u;
    Ritop = cw(gCmd + 7u);
    if (!resume) {
        Rpc = cw(gCmd + 6u);
        Rvf[0] = uvec4(0u, 0u, 0u, 0x3f800000u);
    }

    uint budget = cw(gCmd + 9u);
    uint cycle = 0u;
    uint stop = 0u;
    uint pendingBranch = 0xffffffffu;
    for (; cycle < budget; ++cycle) {
        uint currentPc = Rpc;
        uint debugSlot = 0xffffffffu;
        if (currentPc + 8u > 16384u) { stop = 1u; break; }
        uint codeWord = (gCodeByte + currentPc) >> 2u;
        uint lower = pw(codeWord);
        uint upper = pw(codeWord + 1u);
        bool eBit = ((upper >> 30u) & 1u) != 0u;
        bool iBit = ((upper >> 31u) & 1u) != 0u;

        if (Qdelay > 0 && --Qdelay == 0) Rq = Qpending;
        if (Pdelay > 0 && --Pdelay == 0) Rp = Ppending;
        if (!iBit && ((lower >> 25u) & 0x7fu) == 0x40u) {
            uint fn = s2func(lower);
            bool qProducer = fn >= 0x38u && fn <= 0x3au;
            if (Qdelay > 0 && (fn == 0x3bu || qProducer)) { Rq = Qpending; Qdelay = 0; }
            bool pProducer = fn == 0x70u || fn == 0x71u || fn == 0x72u || fn == 0x73u ||
                             fn == 0x76u || fn == 0x78u || fn == 0x79u || fn == 0x7au;
            if (Pdelay > 0 && (fn == 0x7bu || pProducer)) { Rp = Ppending; Pdelay = 0; }
        }

        uint lowerOp = (lower >> 25u) & 0x7fu;
        // G430 keeps STATUS on the legacy delayed pipeline whenever a microprogram reads it. The
        // current exact corpus has no STATUS readers; reject any future record until that pipeline
        // is migrated instead of silently accepting stale StatusVisible state.
        if (!iBit && lowerReadsStatus(lowerOp)) fail(F_PIPE, lower);
        if (!iBit && lowerReadsMac(lowerOp)) MacVisible = resolveMac(add64s(add64u(PairBase, cycle), -4));

        if (currentPc == 0x2f08u && DebugCount != 0u) {
            uint db = (DebugCount - 1u) * 12u;
            DebugWords[db + 5u] = MacVisible;
            DebugWords[db + 6u] = u16(Rvi[10]);
            DebugWords[db + 7u] = u16(Rvi[7]);
        }

        if (currentPc == 0x2ee0u) {
            DebugOpmulaA = Rvf[21].x;
            DebugOpmulaB = Rvf[20].y;
        }
        if (currentPc == 0x2ee8u && DebugCount < 4u) {
            debugSlot = DebugCount++;
            uint db = debugSlot * 12u;
            DebugWords[db + 0u] = cycle;
            DebugWords[db + 1u] = Racc.z;
            DebugWords[db + 2u] = fmulBits(Rvf[20].x, Rvf[21].y);
            DebugWords[db + 8u] = DebugOpmulaA;
            DebugWords[db + 9u] = DebugOpmulaB;
            DebugWords[db + 10u] = Rvf[20].x;
            DebugWords[db + 11u] = Rvf[21].y;
        }
        if (currentPc == 0x32b8u && DebugWords[144] == 0xffffffffu) {
            DebugWords[144] = cycle;
            DebugWords[145] = upper;
            DebugWords[146] = Rvf[21].x;
            DebugWords[147] = Rvf[21].y;
            DebugWords[148] = Rvf[21].z;
            DebugWords[149] = Rvf[21].w;
            DebugWords[150] = Rvf[17].x;
            DebugWords[151] = Rvf[17].y;
            DebugWords[152] = Rvf[17].z;
            DebugWords[153] = Rvf[17].w;
        }
        uvec4 debugVf25Before = Rvf[25];
        uvec2 stamp = add64u(PairBase, cycle);
        if (iBit) {
            Ri = lower;
            bool upperVnop = (upper & 63u) >= 0x3cu && s2func(upper) == 0x2fu;
            if (!upperVnop) {
                Rmac = resolveMac(stamp);
                execUpper(upper, true, stamp);
                G428Claim = 2u;
                if (gErrors == 0u) pushEager(stamp, Rmac);
            }
        } else {
            int hazard = upperVfDest(upper);
            uvec4 oldV = uvec4(0u), newV = uvec4(0u);
            if (hazard >= 0) oldV = Rvf[hazard];
            execUpper(upper, false, stamp);
            if (hazard >= 0) { newV = Rvf[hazard]; Rvf[hazard] = oldV; }
            if (gErrors == 0u) execLower(lower);
            if (hazard >= 0) applyBits(Rvf[hazard], newV, (upper >> 21u) & 15u);
        }
        if (cycle >= 800u && cycle <= 1246u && any(notEqual(debugVf25Before, Rvf[25]))) {
            uint db = 48u + (DebugVf25Count & 7u) * 12u;
            DebugWords[db + 0u] = cycle;
            DebugWords[db + 1u] = currentPc;
            DebugWords[db + 2u] = lower;
            DebugWords[db + 3u] = upper;
            DebugWords[db + 4u] = debugVf25Before.x;
            DebugWords[db + 5u] = debugVf25Before.y;
            DebugWords[db + 6u] = debugVf25Before.z;
            DebugWords[db + 7u] = debugVf25Before.w;
            DebugWords[db + 8u] = Rvf[25].x;
            DebugWords[db + 9u] = Rvf[25].y;
            DebugWords[db + 10u] = Rvf[25].z;
            DebugWords[db + 11u] = Rvf[25].w;
            ++DebugVf25Count;
        }
        if (currentPc == 0x32b8u && DebugWords[154] == 0xffffffffu) {
            DebugWords[154] = Rvf[25].x;
            DebugWords[155] = Rvf[25].y;
            DebugWords[156] = Rvf[25].z;
            DebugWords[157] = Rvf[25].w;
            DebugWords[158] = Racc.x;
            DebugWords[159] = (upper >> 21u) & 15u;
        }
        if (debugSlot != 0xffffffffu) {
            uint db = debugSlot * 12u;
            DebugWords[db + 3u] = Rvf[23].z;
            DebugWords[db + 4u] = makeMac(Rvf[23], 0xeu);
        }
        if (gErrors != 0u) { stop = 3u; break; }

        Rvf[0] = uvec4(0u, 0u, 0u, 0x3f800000u);
        Rvi[0] = 0;
        uint postExecPc = Rpc;
        uint sequential = currentPc + 8u;
        if (sequential >= 16384u) sequential = 0u;
        uint nextPc = sequential;
        if (pendingBranch != 0xffffffffu) {
            nextPc = pendingBranch;
            pendingBranch = 0xffffffffu;
        } else if (postExecPc != currentPc) {
            pendingBranch = postExecPc + 8u;
            if (pendingBranch >= 16384u) pendingBranch = 0u;
        }
        if (nextPc >= 16384u) nextPc = 0u;
        Rpc = nextPc;
        if (Rebit != 0u) { stop = 2u; break; }
        if (eBit) Rebit = 1u;
    }

    if (Pdelay > 0 && Rebit != 0u) { Rp = Ppending; Pdelay = 0; }
    Rmac = resolveMac(add64u(PairBase, cycle));
    PairBase = add64u(PairBase, cycle);
    if (cycle != cw(gCmd + 10u) || cw(gCmd + 11u) != 0u || stop != cw(gCmd + 12u)) gErrors |= F_RESULT;
    if (gPacketCount != gExpectedPackets || gPacketBytes != gExpectedPacketBytes) gErrors |= F_PACKET;

    storeRecord();
    uint statusAt = cw(gCmd + 31u);
    sw(statusAt + 0u, STATUS_MAGIC);
    sw(statusAt + 1u, gErrors);
    sw(statusAt + 2u, gFailPc);
    sw(statusAt + 3u, gFailWord);
    sw(statusAt + 4u, cycle);
    sw(statusAt + 5u, stop);
    sw(statusAt + 6u, gPacketCount);
    sw(statusAt + 7u, gPacketBytes);
    for (uint d = 0u; d < 160u; ++d) sw(statusAt + 8u + d, DebugWords[d]);
}
)G543D";
}
