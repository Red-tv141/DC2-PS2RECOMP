#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

using namespace ps2recomp;

namespace
{
Instruction makeVU0Special2(uint8_t operation, uint8_t destMask = 0xF)
{
    Instruction inst{};
    inst.opcode = OPCODE_COP2;
    inst.rs = COP2_CO | destMask;
    inst.rt = 7;
    inst.rd = 11;
    inst.sa = 3;
    inst.function = 0x3C;
    inst.vectorInfo.vectorField = destMask;
    inst.raw = (((operation >> 2) & 0x1F) << 6) | (operation & 0x3);
    return inst;
}

Instruction makeVU0Special1(uint8_t operation, uint8_t destMask = 0xF)
{
    Instruction inst{};
    inst.opcode = OPCODE_COP2;
    inst.rs = COP2_CO | destMask;
    inst.rt = 7;
    inst.rd = 11;
    inst.sa = 3;
    inst.function = operation;
    inst.vectorInfo.vectorField = destMask;
    return inst;
}

Instruction makeControlTransfer(bool write, uint8_t rt, uint8_t rd)
{
    Instruction inst{};
    inst.opcode = OPCODE_COP2;
    inst.rs = write ? COP2_CTC2 : COP2_CFC2;
    inst.rt = rt;
    inst.rd = rd;
    return inst;
}

void emitFunction(std::ofstream &output, const char *name, const std::string &body)
{
    output << "static void " << name << "(R5900Context* ctx)\n{\n    "
           << body << "\n}\n\n";
}
}

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: cop2_mac_status_codegen <output.cpp>\n";
        return 1;
    }

    CodeGenerator gen({}, {});
    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    if (!output)
    {
        std::cerr << "failed to open generated test source\n";
        return 1;
    }

    output << R"CPP(#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <bit>
#include <cstdint>
#include <iostream>

static void setVector(R5900Context& ctx, uint8_t index, float x, float y, float z, float w)
{
    ctx.vu0_vf[index] = _mm_set_ps(w, z, y, x);
}

static uint32_t laneBits(__m128 value, int lane)
{
    return static_cast<uint32_t>(Ps2ExtractEpi32(_mm_castps_si128(value), lane));
}

static bool expect(bool condition, const char* name)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    return condition;
}

)CPP";

    emitFunction(output, "execVmul",
                 gen.translateInstruction(makeVU0Special1(VU0_S1_VMUL)));
    emitFunction(output, "execVadd",
                 gen.translateInstruction(makeVU0Special1(VU0_S1_VADD)));
    emitFunction(output, "execVaddX",
                 gen.translateInstruction(makeVU0Special1(VU0_S1_VADD, 0x8)));
    emitFunction(output, "execVabs",
                 gen.translateInstruction(makeVU0Special2(VU0_S2_VABS)));
    emitFunction(output, "execVmax",
                 gen.translateInstruction(makeVU0Special1(VU0_S1_VMAX)));
    emitFunction(output, "execVmr32",
                 gen.translateInstruction(makeVU0Special2(VU0_S2_VMR32)));
    emitFunction(output, "execVopmula",
                 gen.translateInstruction(makeVU0Special2(VU0_S2_VOPMULA, 0xE)));
    emitFunction(output, "execCtcStatus",
                 gen.translateInstruction(makeControlTransfer(true, 2, 16)));
    emitFunction(output, "execCfcStatus",
                 gen.translateInstruction(makeControlTransfer(false, 4, 16)));
    emitFunction(output, "execCfcMac",
                 gen.translateInstruction(makeControlTransfer(false, 5, 17)));

    output << R"CPP(int main()
{
    int failures = 0;
    R5900Context ctx{};

    ctx.r[2] = _mm_setzero_si128();
    ctx.vu0_status = 0xFFFF;
    execCtcStatus(&ctx);
    failures += !expect(ctx.vu0_status == 0, "CTC2 clears STATUS before arithmetic");

    setVector(ctx, 11, 0.0f, -2.0f, std::bit_cast<float>(0x00800000u),
              std::bit_cast<float>(0x7F7FFFFFu));
    setVector(ctx, 7, 3.0f, 3.0f, 0.5f, 2.0f);
    execVmul(&ctx);
    failures += !expect(ctx.vu0_mac_flags == 0x124Au,
                        "MAC Z/S/U/O bits use architectural X/Y/Z/W order");
    failures += !expect(ctx.vu0_status == 0x03CFu,
                        "STATUS current and sticky Z/S/U/O bits are set");
    failures += !expect(laneBits(ctx.vu0_vf[3], 0) == 0x00000000u &&
                            laneBits(ctx.vu0_vf[3], 2) == 0x00000000u &&
                            laneBits(ctx.vu0_vf[3], 3) == 0x7F7FFFFFu,
                        "underflow flushes to zero and overflow clamps to max finite");

    execCfcStatus(&ctx);
    execCfcMac(&ctx);
    R5900Context* ctxPointer = &ctx;
    failures += !expect(GPR_U32(ctxPointer, 4) == 0x03CFu && GPR_U32(ctxPointer, 5) == 0x124Au,
                        "CFC2 reads STATUS and MAC after FMAC arithmetic");

    setVector(ctx, 11, 1.0f, 2.0f, 3.0f, 4.0f);
    setVector(ctx, 7, 4.0f, 3.0f, 2.0f, 1.0f);
    execVadd(&ctx);
    failures += !expect(ctx.vu0_mac_flags == 0 && ctx.vu0_status == 0x03C0u,
                        "positive result clears current flags and preserves sticky flags");

    ctx.vu0_status = 0;
    ctx.vu0_mac_flags = 0xFFFFu;
    ctx.vu0_vf[3] = _mm_set_ps(40.0f, 30.0f, 20.0f, 10.0f);
    setVector(ctx, 11, -1.0f, std::bit_cast<float>(0x7F7FFFFFu),
              std::bit_cast<float>(0x7F7FFFFFu), std::bit_cast<float>(0x7F7FFFFFu));
    setVector(ctx, 7, -2.0f, std::bit_cast<float>(0x7F7FFFFFu),
              std::bit_cast<float>(0x7F7FFFFFu), std::bit_cast<float>(0x7F7FFFFFu));
    execVaddX(&ctx);
    failures += !expect(ctx.vu0_mac_flags == 0x0080u && ctx.vu0_status == 0x0082u,
                        "partial destination clears disabled MAC components");
    failures += !expect(laneBits(ctx.vu0_vf[3], 0) == std::bit_cast<uint32_t>(-3.0f) &&
                            laneBits(ctx.vu0_vf[3], 1) == std::bit_cast<uint32_t>(20.0f) &&
                            laneBits(ctx.vu0_vf[3], 2) == std::bit_cast<uint32_t>(30.0f) &&
                            laneBits(ctx.vu0_vf[3], 3) == std::bit_cast<uint32_t>(40.0f),
                        "partial destination writes only the selected SIMD lane");

    ctx.vu0_status = 0x0A55u;
    ctx.vu0_mac_flags = 0x5678u;
    execVabs(&ctx);
    execVmax(&ctx);
    execVmr32(&ctx);
    failures += !expect(ctx.vu0_status == 0x0A55u && ctx.vu0_mac_flags == 0x5678u,
                        "VABS, VMAX, and VMR32 do not produce FMAC flags");

    ctx.vu0_status = 0;
    ctx.vu0_mac_flags = 0x1111u;
    setVector(ctx, 11, 1.0f, 2.0f, 3.0f, 4.0f);
    setVector(ctx, 7, 5.0f, 6.0f, 7.0f, 8.0f);
    execVopmula(&ctx);
    failures += !expect(ctx.vu0_mac_flags == 0x1111u && ctx.vu0_status == 0x03CFu,
                        "VOPMULA updates XYZ and preserves the prior W MAC component");

    if (failures == 0)
        std::cout << "\nAll generated MAC/status checks passed.\n";
    return failures == 0 ? 0 : 1;
}
)CPP";

    return output ? 0 : 1;
}
