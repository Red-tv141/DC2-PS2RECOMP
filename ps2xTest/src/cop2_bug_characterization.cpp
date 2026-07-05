#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace ps2recomp;

namespace
{
int failures = 0;

void check(bool condition, const char *name, const char *detail)
{
    std::cout << (condition ? "[PASS]   " : "[FAILED] ") << name << ": " << detail << '\n';
    if (!condition)
        failures++;
}

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
}

int main()
{
    CodeGenerator gen({}, {});

    const std::string vsqd = gen.translateInstruction(makeVU0Special2(VU0_S2_VSQD, 0xC));
    check(vsqd.find("_mm_set_epi32(0, 0, -1, -1)") != std::string::npos,
          "VSQD.xy mask",
          "selects lanes X/Y and preserves Z/W");

    const std::string vmr32 = gen.translateInstruction(makeVU0Special2(VU0_S2_VMR32, 0xC));
    check(vmr32.find("_MM_SHUFFLE(0,3,2,1)") != std::string::npos &&
              vmr32.find("_mm_set_epi32(0, 0, -1, -1)") != std::string::npos &&
              vmr32.find("_mm_blendv_ps") != std::string::npos,
          "VMR32",
          "emits yzwx and honors the .xy destination mask");

    const std::string vclip = gen.translateInstruction(makeVU0Special2(VU0_S2_VCLIPw));
    check(vclip.find("_mm_extract_epi32(_mm_castps_si128(ctx->vu0_vf[7]), 3)") != std::string::npos &&
              vclip.find("0x7FFFFFFFu") != std::string::npos &&
              vclip.find("flags |= 0x01") != std::string::npos &&
              vclip.find("flags |= 0x02") != std::string::npos,
          "VCLIPw",
          "uses abs(ft.w), PCSX2 denormal handling, and architectural flag order");

    const std::string opmula = gen.translateInstruction(makeVU0Special2(VU0_S2_VOPMULA, 0xE));
    const std::string opmsub = gen.translateInstruction(makeVU0Special1(VU0_S1_VOPMSUB, 0xE));
    check(opmula.find("_MM_SHUFFLE(3,0,2,1)") != std::string::npos &&
              opmula.find("_MM_SHUFFLE(3,1,0,2)") != std::string::npos &&
              opmsub.find("_MM_SHUFFLE(3,0,2,1)") != std::string::npos &&
              opmsub.find("_MM_SHUFFLE(3,1,0,2)") != std::string::npos &&
              opmsub.find("ctx->vu0_acc = res") == std::string::npos,
          "VOPMULA/VOPMSUB",
          "emit outer-product pairing and VOPMSUB leaves ACC unchanged");

    const std::string vilwr = gen.translateInstruction(makeVU0Special2(VU0_S2_VILWR, 0x4));
    const std::string viswr = gen.translateInstruction(makeVU0Special2(VU0_S2_VISWR, 0x2));
    check(vilwr.find("(ctx->vi[11] & 0x3FF)) << 4") != std::string::npos &&
              vilwr.find("READ32(addr + 4)") != std::string::npos &&
              viswr.find("(ctx->vi[11] & 0x3FF)) << 4") != std::string::npos &&
              viswr.find("WRITE32(addr + 8") != std::string::npos,
          "VILWR/VISWR",
          "use qword addressing and selected component offsets");

    const std::vector<uint8_t> fmacSpecial1 = {
        VU0_S1_VADDx, VU0_S1_VSUBx, VU0_S1_VMADDx, VU0_S1_VMSUBx, VU0_S1_VMULx,
        VU0_S1_VMULq, VU0_S1_VMULi, VU0_S1_VADDq, VU0_S1_VMADDq, VU0_S1_VADDi,
        VU0_S1_VMADDi, VU0_S1_VSUBq, VU0_S1_VMSUBq, VU0_S1_VSUBi, VU0_S1_VMSUBi,
        VU0_S1_VADD, VU0_S1_VMADD, VU0_S1_VMUL, VU0_S1_VSUB, VU0_S1_VMSUB,
        VU0_S1_VOPMSUB};
    const std::vector<uint8_t> fmacSpecial2 = {
        VU0_S2_VADDAx, VU0_S2_VSUBAx, VU0_S2_VMADDAx, VU0_S2_VMSUBAx, VU0_S2_VMULAx,
        VU0_S2_VMULAq, VU0_S2_VMULAi, VU0_S2_VADDAq, VU0_S2_VMADDAq, VU0_S2_VADDAi,
        VU0_S2_VMADDAi, VU0_S2_VSUBAq, VU0_S2_VMSUBAq, VU0_S2_VSUBAi, VU0_S2_VMSUBAi,
        VU0_S2_VADDA, VU0_S2_VMADDA, VU0_S2_VMULA, VU0_S2_VSUBA, VU0_S2_VMSUBA,
        VU0_S2_VOPMULA};

    bool allFmacClassesUpdateFlags = true;
    for (uint8_t operation : fmacSpecial1)
    {
        const std::string code = gen.translateInstruction(makeVU0Special1(operation, 0xE));
        allFmacClassesUpdateFlags &=
            code.find("ctx->vu0_mac_flags") != std::string::npos &&
            code.find("ctx->vu0_status") != std::string::npos;
    }
    for (uint8_t operation : fmacSpecial2)
    {
        const std::string code = gen.translateInstruction(makeVU0Special2(operation, 0xE));
        allFmacClassesUpdateFlags &=
            code.find("ctx->vu0_mac_flags") != std::string::npos &&
            code.find("ctx->vu0_status") != std::string::npos;
    }
    check(allFmacClassesUpdateFlags,
          "MAC/status FMAC coverage",
          "all ADD/SUB/MUL/MADD/MSUB/OPM destination and accumulator forms emit flags");

    const std::string fmacLayout =
        gen.translateInstruction(makeVU0Special1(VU0_S1_VMUL, 0xF));
    check(fmacLayout.find("vu_reverse4") != std::string::npos &&
              fmacLayout.find("vu_under_mask") != std::string::npos &&
              fmacLayout.find("vu_over_mask") != std::string::npos &&
              fmacLayout.find("ctx->vu0_status & 0xFC0u") != std::string::npos,
          "MAC/status layout",
          "reverses host lane bits and updates current/sticky Z/S/U/O fields");

    const std::string vabs = gen.translateInstruction(makeVU0Special2(VU0_S2_VABS, 0xF));
    const std::string vmax = gen.translateInstruction(makeVU0Special1(VU0_S1_VMAX, 0xF));
    check(vabs.find("vu0_mac_flags") == std::string::npos &&
              vabs.find("vu0_status") == std::string::npos &&
              vmax.find("vu0_mac_flags") == std::string::npos &&
              vmax.find("vu0_status") == std::string::npos &&
              vmr32.find("vu0_mac_flags") == std::string::npos &&
              vmr32.find("vu0_status") == std::string::npos,
          "Non-FMAC flag isolation",
          "VABS, VMAX, and VMR32 do not produce arithmetic flags");

    Instruction cfc2{};
    cfc2.opcode = OPCODE_COP2;
    cfc2.rs = COP2_CFC2;
    cfc2.rt = 2;
    cfc2.rd = 16;
    const std::string status = gen.translateInstruction(cfc2);
    cfc2.rd = 17;
    const std::string mac = gen.translateInstruction(cfc2);
    cfc2.rd = 18;
    const std::string clip = gen.translateInstruction(cfc2);
    cfc2.rd = 22;
    const std::string q = gen.translateInstruction(cfc2);
    check(status.find("ctx->vu0_status") != std::string::npos &&
              mac.find("ctx->vu0_mac_flags") != std::string::npos &&
              clip.find("ctx->vu0_clip_flags") != std::string::npos &&
              q.find("ctx->vu0_q") != std::string::npos,
          "CFC2 control map",
          "architectural indices 16/17/18/22 map to STATUS/MAC/CLIP/Q");

    std::cout << "\nPassed " << (9 - failures) << "/9 COP2 generator checks.\n";
    return failures == 0 ? 0 : 1;
}
