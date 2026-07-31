#ifndef PS2_VU1_H
#define PS2_VU1_H

#include <cstdint>

class GS;
class PS2Memory;

// G437: the VU1 register file MUST be cache-line aligned.
//
// The G421/G422/G426/G428 fast paths access `vf[n]` and `acc` as 16-byte SSE vectors
// (`_mm_loadu_ps` / `_mm_storeu_ps`). Without an explicit alignment this struct has alignof 4 and
// lands wherever its owner (`PS2Runtime::m_vu1`, a stack local in main) puts it — measured at
// `base % 64 == 52`, so EVERY vector access was 4-byte misaligned, one VU register in four
// straddled a 64-byte cache line, and `acc` (offset 576) straddled one on every single access.
// A line-splitting store cannot store-to-load forward, so each dependent FMAC in an
// MULA->MADDA->MADDA->MADD chain paid a store-buffer drain (~15-20 cycles) instead of ~5-cycle
// forwarding. alignas(64) makes vf[n] (16n) and acc (576 = 9*64) split-free for all n.
// Purely an address change: no field, size-in-fields, order or value semantics move.
struct alignas(64) VU1State
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    bool ebit;
    uint32_t itop;
    uint32_t xitop;
};

class VU1Interpreter
{
public:
    VU1Interpreter();

    void reset();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t itop = 0, uint32_t maxCycles = 65536);

    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }

private:
    VU1State m_state;

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);

    void execUpper(uint32_t instr);
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);

    void applyDest(float *dst, const float *result, uint8_t dest);
    void applyDestAcc(const float *result, uint8_t dest);
    float broadcast(const float *vf, uint8_t bc);
};

#endif
