// ============================================================================
//  Distinct-Lane VU/SIMD Characterization Test — template
//
//  Purpose: prove or disprove a suspected recompiler-codegen or VU-interpreter
//  lane/mask/shuffle defect WITHOUT a full game route. This is the harness
//  pattern that settled the COP2 dest-mask lane reversal (the "50 phases of
//  dungeon black" bug) in one run. See 10-agent-guardrails.md §2.1 for the
//  defect catalog these tests target.
//
//  THE ONE RULE: never test vector ops with symmetric data. (1,1,1,1) survives
//  any shuffle/mask reversal and proves nothing. DISTINCT PER-LANE VALUES ONLY.
//
//  Two ways to wire it (pick per what you're auditing):
//   A) CODEGEN audit  — link the recompiler lib, call its emit function
//      (e.g. CodeGenerator::translateInstruction) per instruction word and
//      assert on the EMITTED C++ TEXT vs the architectural truth.
//   B) INTERPRETER audit — link the runtime, execute the VU1 interpreter op
//      on a prepared context and assert on the RESULT LANES.
//  Build as a tiny standalone exe (own CMake target); never inside the runner.
// ============================================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>

// --- Canonical distinct-lane inputs (10 §2.1) --------------------------------
static const float  VF[4]  = { 1.0f, 2.0f, 4.0f, 8.0f };          // X Y Z W
static const uint32_t VI[4] = { 0x11111111u, 0x22222222u,
                                0x33333333u, 0x44444444u };
static const float  SENT[4] = { 9.0f, 9.0f, 9.0f, 9.0f };          // memory sentinel

static int g_failures = 0;

static void expect_lanes(const char* name, const float got[4], const float want[4])
{
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(got[i] - want[i]) > 1e-6f) {
            std::printf("FAIL %-10s lane %c: got %g want %g\n",
                        name, "XYZW"[i], got[i], want[i]);
            ++g_failures;
            return;
        }
    }
    std::printf("ok   %s\n", name);
}

// --- Oracle expectations (architectural truth; PCSX2 VUops.cpp is the source) --
// Lane order everywhere: (lane0,lane1,lane2,lane3) = (X,Y,Z,W).
// Dest-mask bit order: X=8, Y=4, Z=2, W=1  (OPPOSITE of _mm_movemask_ps!).

static void test_vmr32(/* your op-under-test hook here */)
{
    // VMR32 rotates (x,y,z,w) -> (y,z,w,x)
    const float want[4] = { 2.0f, 4.0f, 8.0f, 1.0f };
    float got[4];
    // got = run_interpreter_VMR32(VF)  /  or assert emitted C++ contains the
    //       correct _MM_SHUFFLE(0,3,2,1), not (0,0,0,1).
    std::memcpy(got, want, sizeof got);            // placeholder — wire your call
    expect_lanes("VMR32", got, want);
}

static void test_vsqd_xy(/* hook */)
{
    // VSQD.xy: store to memory pre-filled with sentinel must yield (1,2,9,9).
    // Catches the mask-bit reversal (X<-8 not 1, Y<-4 not 2).
    const float want[4] = { 1.0f, 2.0f, 9.0f, 9.0f };
    float mem[4]; std::memcpy(mem, SENT, sizeof mem);
    // run_interpreter_VSQD_xy(VF, mem);
    std::memcpy(mem, want, sizeof mem);            // placeholder — wire your call
    expect_lanes("VSQD.xy", mem, want);
}

static void test_vopmula(/* hook */)
{
    // Outer product uses ROTATED pairing, not component-wise multiply:
    //   ACC.x = Fs.y*Ft.z, ACC.y = Fs.z*Ft.x, ACC.z = Fs.x*Ft.y
    // fs=(1,2,3,_), ft=(4,5,6,_)  =>  ACC=(12, 12, 5, _)
    const float want[4] = { 12.0f, 12.0f, 5.0f, 0.0f };
    float acc[4];
    std::memcpy(acc, want, sizeof acc);            // placeholder — wire your call
    expect_lanes("VOPMULA", acc, want);
}

static void test_vclipw(/* hook */)
{
    // VCLIPw judgement bits: bit0/2/4 = coord > +|w| ; bit1/3/5 = coord < -|w|.
    // V=(1,2,4,8) vs w=10  -> 0x00 ; vs w=... craft cases where exactly one
    // side trips, and a NEGATIVE w case to prove |w| (absolute) handling:
    //   VCLIPw(V, +10) => 0x00,  VCLIPw(V,  -10) must equal VCLIPw(V, +10).
    // uint32_t flags = run_interpreter_VCLIPw(VF, -10.0f);
    std::printf("ok   VCLIPw (wire the two-sided + negative-w cases)\n");
}

int main()
{
    test_vmr32();
    test_vsqd_xy();
    test_vopmula();
    test_vclipw();

    std::printf("\n%s (%d failures)\n", g_failures ? "DEFECTS FOUND" : "ALL PASS",
                g_failures);
    // AUDIT THE WHOLE CLASS (10 §2.1 rule 4): when one op fails, add every
    // sibling of its family (all partial-dest masks, all shuffles, all
    // control-reg indices) before declaring the audit done — dormant defects
    // surface later otherwise.
    return g_failures ? 1 : 0;
}
