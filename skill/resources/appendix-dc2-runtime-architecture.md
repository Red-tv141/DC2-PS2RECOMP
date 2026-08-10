# Appendix: Dark Cloud 2 — Runtime Architecture & Proven Technical Facts

> **PROJECT-SPECIFIC, LOOKUP ONLY.** Diagnosed, durable facts about how THIS port's runtime works —
> addresses, contracts, ABIs, data formats. Read a section when you touch that subsystem; never
> top-to-bottom.
>
> Companions: `appendix-dc2-project.md` (paths, build, harnesses, static export) ·
> `appendix-dc2-graphics-facts.md` (~50 diagnosed graphics defects + renderer snapshots) ·
> `appendix-dc2-test-routes.md` (route matrices) · `PS2_PROJECT_STATE.md` (live rules/gaps).
>
> Each heading names the phase(s) that established it — grep `plans/phase-<ID>-fix-log.md` for the
> full derivation.

---

## §1 Rendering Fixes & Sprite Sampling (G5, G6, G8, G362)

- **Split VIF1 IMAGE continuation (G6)**: VIF1 PATH2 IMAGE continuation qwords must be forwarded
  verbatim (`ps2_vif1_interpreter.cpp`) — never re-wrap in a second IMAGE tag.
- **Debug-menu PSMT4HH font (G5)**: packed 4-bit stream; stop at the `TRXREG` rectangle, not at the
  oversized QWC request. CLAMP repeat semantics.
- **2D UI sprite point-sampling (G8, G362)**: DC2 UI text/icons are point-sampled (`tex1=0x201`,
  MMAG/MMIN=0). `drawSprite` FST bias is 0.0. `sampleExact` (G362) uses 12.4 fixed-point UV
  reconstruction (`lround(u*16)/16`) on `uMode` bit 12 to prevent float `floor()` rounding errors
  across atlas boundaries.

---

## §2 Pad Input Architecture (F66, G7, G49)

- **Live pad read is `read_pad_stub`** (`0x0014A490`) → `dc2_write_pad_status`. Writes buttons to
  `PAD_STATUS+0` and analog axes (`+4`=LY, `+8`=LX, `+0xc`=RY, `+0x10`=RX). **`scePadRead` is DEAD.**
  `CGamePad+0xc`=LX, `+0x8`=LY (`-0x80` centre, ±`0x32` deadzone).
- **Free-roam movement uses the LEFT ANALOG STICK** (`Analog__11CPadControl@0x3d7b60`). A
  digital-only injector never moves the player.
- **Host present-loop driving**: a headless pad script must drive off the host present loop
  (`ps2_runtime.cpp`) — the dungeon bypasses the `mgEndFrame` hook.
- **XInput via raylib (G7)**: polled by `g7_poll_live_pad()` in `ps2_pad.cpp`. Priority:
  `DC2_NO_XINPUT=1` → scripted `DC2_PAD_INPUT` → live gamepad/keyboard → default.

## §3 Debug-Menu Navigation & Frame Dumps

- `DC2_DEBUG_MENU=1` writes `DebugFlag@0x00376FB8`. Navigate with
  `DC2_PAD_INPUT='90..99:DebugDown;130..139:DebugDown;170..179:DebugConfirm'`.
- Frame dumps land in `captures/frame_NNNNNN.ppm`. **The filename is the HOST TICK counter, NOT the
  guest scriptFrame.** Cross-check with `DC2_TRACE_F59=1`.
- The runner's menu rendering is too corrupted to identify screens visually — distinguish screens by
  STATE (`LoopNo`, `titleInfo`, `DngStatus`, `DAT_01ecd62c`), never by frame captures.

---

## §4 EE Software `double` Math (G373)

- The R5900 FPU is single-precision only. Metrowerks implements `double` **in software using 64-bit
  integer GPRs** (`fptodp` @`0x002893C0`, `dpmul` @`0x00287FF8`, `dptofp` @`0x002887C8`).
- Double-precision libm entry points — `sin` `0x0011E1C0`, `cos` `0x0011DA28`, `atan` `0x0011D5D0`,
  `atan2` `0x0011EA80`, `pow` `0x0011EB98`, `sqrt` `0x0011EFC8`, `fabs` `0x0011DB30`,
  `floor` `0x0011DB88`: **args in `$a0`/`$a1`, result in `$v0`**, each a full 64-bit IEEE double in
  ONE GPR. Use `ps2GetDoubleArg` / `ps2SetDoubleReturn`.
- A guessed ABI here fails SILENTLY — no crash, just wrong numbers.

## §5 EE Float Arithmetic & Saturation (G371, G372)

- R5900 COP1 has **no Inf, no NaN, no denormals** — it saturates every result to `±0x7F7FFFFF`.
  `x/0` returns the max finite value with the operand signs; `sqrt` uses the absolute value. Clamped
  via the FPU macros (`FPU_ADD_S`, `FPU_SUB`, `FPU_MUL`, `FPU_DIV_S`, `FPU_SQRT_S`).
- COP2/VU0 FMAC ops are clamped by the MAC-flag block.
- **Any NaN or Inf anywhere in guest state is OURS**, always — the hardware cannot produce either.

## §6 R5900 EABI Register & Stack Argument Layout (G194, G374, G378)

- The first **8** integer arguments go in `$a0–$a3` **and `$t0–$t3`**.
- There is **NO 16-byte o32 stack save area**. Stack arguments start at **`sp + 0`** in 8-byte slots.
- Software `double` math lives in a single 64-bit GPR — never split across two 32-bit slots.

---

## §7 Boot & Static Initializers (`__sinit_*`, G361)

- Metrowerks static initializers (`__sinit_<file>.cpp`) run via the `.ctors` pointer table at
  `0x00374D80–0x00374E40`, walked by `mwInit@0x00100190`, called by `init__Fv@0x0015C160`.
- A nop'd `init__Fv` skipped **all** static initializers. `f55_boot_init_stub` runs the ctor table
  once before `InitCDFile`. **Index 5 is EXCLUDED** (G361).

## §8 GS Transfers & BITBLTBUF (G358, G359, G383)

- Block pointers in `BITBLTBUF.SBP`/`DBP` are **14-bit fields in 256-byte block units**. The guest
  TBP0 is ALREADY in block units — pass it directly, do **NOT** scale by `*8`.
- MTGS readback contract: a GS local→host readback (`sceGsExecStoreImage`) MUST call
  `g150_wait_idle()` before consuming bytes.

## §9 Off-EE-Thread Guest-Execution Lock (G377, G379)

- **Invariant**: any host thread that runs recompiled guest code MUST hold
  `PS2Runtime::GuestExecutionScope`.
- `m_guestExecutionMutex` is **recursive**; `g_guestExecutionDepths` is **thread_local**. Blocking
  syscalls release the lock via `GuestExecutionReleaseScope`.
- An IRQ that runs guest code without the scope crashes FMV playback (G377) — IRQ delivery must be
  ASYNC (G375).

---

## §10 Recompiler & Codegen Rules (DC2 instances)

- **`.inc` recompile trap (G359)**: MSBuild does not reliably rebuild when an included `.inc`
  changes. Always content-edit the including `.cpp` (`GS.cpp` / `ps2_gs_gpu.cpp` / `ps2_vu1.cpp`) to
  force the recompile.
- **Nested jump-table coverage (G395)**: `EventSelect__Fv@0x001923B0` uses a seven-entry inner table
  at `0x00365080`. Keep the table explicit in `config_dc2_final.toml`.
- **MSBuild `SelectedFiles` static-library trap (G395)**: building `dc2_game` with
  `/p:SelectedFiles=...` repacks `dc2_game.lib` with only the selected object. Build the Release
  target normally.
- **`DIRECT_JAL_ONLY_TARGET` (G223)**: cannot be traced via `registerFunction` — wrap the jal target
  instead. More generally, `registerFunction` overrides only fire for indirect `jr $t9`/`jalr`.
- **Back-edge preemption context safety (G186, G211, G212)**: recompiled bodies unwound by preemption
  must suppress preemption during wrapper execution (`g_dc2PreemptSuppressDepth`). ANY new override
  that post-call-mutates pc/ra/output must apply this rule.
- **VU1 scalar prestall (G239)**: lower scalar stalls happen BEFORE either half of an instruction
  pair executes — decode the lower word first.
- **VU1 FDIV delay (G200)**: FDIV busy-stalls the second DIV/SQRT/RSQRT until the first result
  latches into Q.
- **VU1 immutable-fragment specialization (G533)**: build exact-source plan bits only during the
  existing microcode/cache rebuild. A static handler may run only when complete source words,
  block length, typed tail, data size, and required history storage match. Hashes are diagnostics,
  never admission. Sample the feature arm once per `run()`, call specialized code directly at the
  block site, and canonicalize every miss to the promoted generic body; do not wrap all blocks or
  generate machine code at runtime. Branch/E-bit, VF hazards, delayed flags, Q/P, and XGKICK remain
  hard plan boundaries.
- **VU1 GPU corpus boundary (G541)**: `G297KickItem` is the authoritative migration seam. A complete
  replay record includes public pre/post VU state, hidden delayed-pipeline state, full pre code/data
  RAM, ordered incoming writes, kick inputs, stop/cycle result, and ordered XGKICK addresses/bytes.
  The file/record schema is explicitly little-endian and padding-free; continuity across records is
  part of validation, not an assumption.
- **VU1→GS GPU ordering contract (G541)**: a GPU VU command must enter the shared GL FIFO before its
  dependent GS packet work becomes visible. Commands may coalesce only while still queued; after
  the GL worker pops a batch, later kicks form a successor. Keep persistent state and XGKICK output
  GPU-resident and order consumers with GPU barriers. Per-kick CPU fences, maps, waits, `glGet*`, or
  readbacks violate the architecture.
- **Universal Z emulation (G203)**: honor guest ZBUF/TEST universally, never whitelist per-screen.

## §11 Recompiler Regeneration Procedure (G380, G381)

**Authoritative config: `config_dc2_final.toml` in the repo root.**

```
1. ps2_recomp.exe config_dc2_final.toml
2. python tools/fix_cop2_q_ps2_float.py recomp
3. python tools/fix_cop1_ps2_float.py recomp
```

- **STALE SCRIPT WARNING: do NOT run `tools/fix_cop2_destmask.py`** — its defect class is fixed in the
  recompiler; re-running it corrupts output.
- After any regen, audit the **allocator family** for coherence (`malloc`/`_malloc_r`/`memalign`/
  `free`/`realloc`/`calloc` + `_r` variants must ALL route to the runtime allocator or ALL be
  recompiled — a split allocator silently corrupts texture/CLUT/menu memory), and validate VU0-helper
  un-stub collisions (`Kernel/Stubs/VU.cpp` vs recompiled bodies; `/FORCE` picks one winner).
- `ps2_recomp.exe`'s build cache hard-references the stale path `d:/ps2r/PS2Recomp` — fix and rebuild
  the recompiler before regenerating.

---

## §12 FMV / PSS Audio Architecture (G384, G393)

- DC2 PSS private-stream audio starts with the substream selector `ff a0 00 00`, then Sony
  `SShd`/`SSbd` ADS data.
- `RUSH.PSS` is **codec 1 PCM16LE, 48 kHz stereo, 0x200-byte planar blocks per channel**.
  Deinterleave channel blocks to LRLR samples before host playback.
- ADS codec 1 is planar PCM16LE; every non-1 value is Sony PSX ADPCM with per-channel predictor
  history. All 53 retail DC2 PSS entries census as codec 1 (G393).

## §13 Gameplay SFX and Sequenced BGM Architecture (G385–G394, G450, G451)

- SIF DMA sends Sony `.HD` metadata and `.BD` VAG bodies; EZMIDI `SetHD` associates the pair with a
  sound port.
- **Multi-chunk `.BD` transfers are concatenated before the `SetHD` commit** (G387). Aliased `SetHD`
  calls are re-bound by the HD/BD/size triple (G388).
- Bank SFX run through a 24-voice 44.1 kHz stereo mixer (`dc2_g391_sfx_mixer.inc`). `.HD` sample
  params: **+18/+20 ADSR, +17 split pan**. Fixed BGM bus gain **0.85**.
- Sequenced BGM parses `SCEISequ` events and renders through a hardware-faithful 22,050 Hz SPU2
  reverb network built from the exact `LIBSD.IRX` `.data` preset tables (9 records).
- **EZMIDI volume is 0..0x100 — divide by `256.0f`** (G389). `F9 00` is channel volume, `F9 01` is
  channel pan (`0x40` = centre) (G392).
- EZMIDI `Stop` stops all active voices on the targeted port (G450); `Quit` stops all.
  `TitleModeInit@0x2A1020` restarts environment BGM after the title load.
- **Cold-title BGM ordering (G394)**: `TitleLoop@0x29FFA0` queues an old-BGM zero-volume/Stop before
  the replacement Play, but host ordering can deliver that pair AFTER Play and kill the new
  generation before its first audible frame. Only the paired stop inside the first 100 ms is
  suppressed; a surviving standalone mute still applies.

## §14 Voice Streaming Architecture (G386)

- `StreamOpenFast@0x18AEF0` calls `ezBgm@0x28AE20`, private RPC SID `0x12345`.
- `SOUND.HD3` contains 16-byte entries `(nameOffset, sizeBytes, sectorOffset, sectorCount)` indexing
  into `SOUND.DAT`. Payloads are standard RIFF/BWF PCM, **mono, 48 kHz, 16-bit** — do NOT route them
  through the HD/BD VAG decoder.
- `sgCPlayVoice::Step@0x304730` treats exactly `0x1000` as "playing" and completes when the state
  clears. G386 derives the transition from decoded frame count / sample rate; a permanent zero or a
  permanent `0x1000` is wrong.
- Implementation: `lib/ps2_audio_parts/dc2_g386_voice_audio.inc`; SID routing in `lib/ps2_iop.cpp`.

---

## §15 Game-Specific Bugs (original-game defects — clamp at the runtime boundary, never fix game-side)

- **4HH/4HL oversized transfer** (`mgLoadTextureZ@0x145400`): size `(w*h*bpp)/16` is **8× too large**.
  Real hardware survives the DMAC overrun; the runtime consumes packed 4-bpp data and stops when
  `TRXREG` is full (G5).
- **Intro movie `IsStarted` spin** (IPU/MPEG): bypassed headless.

## §16 Technical Symbols & Upstream PR Cross-Reference

- **Boot C++ runtime inits**: `mwInit@0x00100190` walks the `__initialize_cpp_rts` table
  (`0x00374D80–0x00374E40`).
- **GS display stride**: `copyDisplaySource`'s fallback reads `candidate.fbw = displayFrame.fbw`
  (512 vs the context's 640).
- **Upstream PS2Recomp PRs** (a map of features DC2 lacks; adopt selectively, mine as design
  references — most are large rewrites):
  | PR | Subject | DC2 disposition |
  |---|---|---|
  | #170 | `ps2xIOP` HLE refactor | does NOT emulate the R3000A or load/execute IRX |
  | #154 | SoundDriver RPC/status HLE | parametrizes RPC without sample rendering; G384–G394 already cover the tracked audio routes |
  | #135 | Recompiler reachable-function generation | not an IOP CPU/scheduler |
  | #120 / #137 | Cooperative thread scheduler / ABBA-safe waits | menu→dungeon lock starvation; VU0 micro-mode; MPEG |
  | #132 | GS memory module | **do NOT splice** — would clobber the G3/G5 swizzle fixes |
  | #128 | JR/JALR fallback codegen | fold at the next regen |
  | #131 | More EE/IOP stubs | stub on demand instead |
