// ============================================================================
//  Syscall Implementation Template — patterns for ps2_syscalls.cpp
//
//  Shows the THREE syscall shapes you will implement:
//    (1) simple value-returning syscall
//    (2) syscall writing results through a guest pointer
//    (3) BLOCKING syscall — MUST release the guest-execution lock (16 §2)
//
//  Conventions (04-runtime-syscalls-stubs.md §3):
//   - $v1 carries the syscall NUMBER (negative numbers = interrupt-context
//     variants, still in $v1). NEVER identify a syscall from $v0 — it holds a
//     stale return value and will coincidentally match valid numbers.
//   - Args in $a0..$a3 (regs 4..7), 5th+ integer arg in $t0 (reg 8) per EABI.
//   - Return in $v0 via setReturnU32/setReturnS32.
//   - Guest pointer → host pointer: rdram + (addr & 0x01FFFFFF). ALWAYS mask.
//   - Exact helper/class names vary per runtime version — mirror the ones used
//     by the syscalls ALREADY in your ps2_syscalls.cpp, don't invent parallel ones.
// ============================================================================

#include "ps2_runtime.h"
#include <cstdio>
#include <cstring>

// ----------------------------------------------------------------------------
// (1) Simple value-returning syscall.
//     Example shape: GetOsdConfigParam-style "read a config word".
// ----------------------------------------------------------------------------
void sys_GetSomething(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/)
{
    // uint32_t a0 = getRegU32(ctx, 4);   // read args as needed
    setReturnU32(ctx, 0);                 // result in $v0
    // NOTE: inside the central syscall dispatcher the PC advance is usually
    // handled by the dispatcher itself (syscall returns to EPC+4). Only a RAW
    // registered handler must advance ctx->pc manually. Match your runtime.
}

// ----------------------------------------------------------------------------
// (2) Syscall writing through a guest pointer (out-param).
//     Example shape: RFU/Query calls filling a caller-provided struct.
// ----------------------------------------------------------------------------
void sys_QueryInfo(uint8_t* rdram, R5900Context* ctx, PS2Runtime* /*rt*/)
{
    const uint32_t outPtr = getRegU32(ctx, 4);            // a0 = guest dest
    if (outPtr != 0) {
        uint8_t* host = rdram + (outPtr & 0x01FFFFFF);    // MASK. ALWAYS.
        // Fill the struct with the layout the GAME expects — verify field
        // offsets from the decompiled caller (static export), not from memory.
        std::memset(host, 0, 16);
        *reinterpret_cast<uint32_t*>(host + 0) = 1;       // e.g. status = ok
    }
    setReturnU32(ctx, 0);
}

// ----------------------------------------------------------------------------
// (3) BLOCKING syscall — WaitSema-shaped. THE CARDINAL RULE (16 §2):
//     any syscall that waits/sleeps/yields must RELEASE the guest-execution
//     lock for the duration of the wait, then reacquire before returning to
//     guest code. Holding it starves every other guest thread → freeze.
// ----------------------------------------------------------------------------
void sys_WaitSema(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* rt)
{
    const int32_t semaId = static_cast<int32_t>(getRegU32(ctx, 4)); // a0

    // Pseudocode — use YOUR runtime's actual sema table + release-scope type.
    // Sema* s = rt->kernel().findSema(semaId);
    // if (!s) { setReturnS32(ctx, -1 /*ERROR_UNKNOWN_SEMID*/); return; }
    //
    // if (s->count > 0) { s->count--; setReturnS32(ctx, semaId); return; }
    //
    // s->waiters++;
    // {
    //     GuestExecutionReleaseScope release(rt);   // <-- THE critical line
    //     s->cv_wait_until_signaled();              // host wait, lock RELEASED
    //     // ABBA hazard (16 §3): unlock any LOCAL wait mutex BEFORE this
    //     // scope's destructor reacquires the guest-execution lock.
    // }
    // s->waiters--;
    // setReturnS32(ctx, semaId);
    (void)semaId; (void)rt; setReturnS32(ctx, 0); // placeholder for template
}

// ----------------------------------------------------------------------------
// Dispatcher wiring — a case in the existing switch (shape varies per runtime):
//
//   case 0x44: sys_WaitSema(rdram, ctx, rt); break;   // WaitSema
//
// Before adding a number: confirm it against db-syscalls.md AND the game's
// actual usage (some upstream slices remap indices — 04 §3 warning). Log every
// still-unknown number once: "[Syscall TODO 0x%02X]" so gaps surface loudly.
// ============================================================================
