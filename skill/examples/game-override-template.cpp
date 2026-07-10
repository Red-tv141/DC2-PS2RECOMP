// ============================================================================
//  Game Override Template — REAL ps2xRuntime API
//
//  A per-game C++ file that replaces or stubs recompiled functions WITHOUT
//  touching generated runner/*.cpp. Lives in ps2xRuntime/src/ (e.g.
//  ps2xRuntime/src/<game>_game_override.cpp). See 04-runtime-syscalls-stubs.md §5.
//
//  Handler signature is ALWAYS:  void(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime)
//  Read args with getRegU32(ctx, N):  a0=4 a1=5 a2=6 a3=7  (t0/a4=8 …)  ra=31
//  Return with setReturnU32(ctx, v) (== $v0).  Advance with ctx->pc = getRegU32(ctx, 31).
// ============================================================================

#include "game_overrides.h"
#include "ps2_runtime.h"
#include <cstdio>

namespace {

// ----------------------------------------------------------------------------
// Triage stubs — bypass an unknown/crashing/spinning sub_xxx.
// NOTE: a raw handler MUST advance the PC itself, or the dispatcher re-enters
// the same guest PC forever (see CRITICAL warning at the bottom).
// ----------------------------------------------------------------------------
void ret0_stub(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    setReturnU32(ctx, 0);                 // $v0 = 0
    ctx->pc = getRegU32(ctx, 31);         // return to caller via $ra
}
void ret1_stub(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    setReturnU32(ctx, 1);                 // $v0 = 1 (success)
    ctx->pc = getRegU32(ctx, 31);
}
void reta0_stub(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    setReturnU32(ctx, getRegU32(ctx, 4)); // $v0 = $a0 (passthrough)
    ctx->pc = getRegU32(ctx, 31);
}

// ----------------------------------------------------------------------------
// A function reversed via the static export / Ghidra and reimplemented natively.
// ----------------------------------------------------------------------------
void my_game_init(uint8_t* rdram, R5900Context* ctx, PS2Runtime* /*rt*/) {
    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t a1 = getRegU32(ctx, 5);
    std::printf("[Override] my_game_init a0=0x%08X a1=0x%08X\n", a0, a1);

    // ... native logic; touch guest RAM through rdram + a guest pointer ...
    // uint32_t* p = reinterpret_cast<uint32_t*>(rdram + (a0 & 0x01FFFFFF));

    setReturnU32(ctx, 1);
    ctx->pc = getRegU32(ctx, 31);
}

// ----------------------------------------------------------------------------
// Apply function — runs once when the runtime registers overrides for this ELF.
// ----------------------------------------------------------------------------
void applyMyGameOverrides(PS2Runtime& runtime) {
    // (a) Replace a recompiled function with a native C++ handler:
    runtime.registerFunction(0x00109904u, my_game_init);

    // (b) Triage bindings for infinite loops / crashing sub_xxx:
    runtime.registerFunction(0x001A24B0u, ret1_stub);
    runtime.registerFunction(0x001B0088u, ret0_stub);
    runtime.registerFunction(0x001C5530u, reta0_stub);

    // (c) Re-point a guest address at an EXISTING named runtime stub
    //     (ps2_stubs::sceCdRead etc.) — handles PC advance for you:
    ps2_game_overrides::bindAddressHandler(runtime, 0x00123456u, "sceCdRead");

    // (d) Inline lambda is fine too (same signature):
    runtime.registerFunction(0x0029FC78u,
        [](uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
            ctx->pc = getRegU32(ctx, 31);  // pure no-op skip
        });
}

} // namespace

// ----------------------------------------------------------------------------
// Registration — matches against the loaded ELF. Pass 0 for entry/crc32 to skip
// that match criterion (match by elfName only).
// ----------------------------------------------------------------------------
PS2_REGISTER_GAME_OVERRIDE(
    "my-game-us",      // human-readable name
    "SLUS_XXX.XX",     // elfName (basename of the main ELF, case-insensitive)
    0u,                // entry point (0 = don't match on entry)
    0u,                // crc32     (0 = don't match on crc32)
    &applyMyGameOverrides
);

// ============================================================================
//  CRITICAL: registerFunction overrides fire ONLY for indirect dispatch
//  (jr $t9 / jalr). A DIRECT `jal <addr>` is a direct C++ call and BYPASSES the
//  dispatcher. To intercept direct callers, mark the target as a TOML stub and
//  regenerate so calls dispatch dynamically, or override the caller itself.
//
//  CRITICAL: any raw handler that does not naturally advance ctx->pc MUST do
//  `ctx->pc = getRegU32(ctx, 31);` or the runtime re-dispatches the same PC in
//  an infinite loop. bindAddressHandler() to a named stub advances for you.
// ============================================================================
