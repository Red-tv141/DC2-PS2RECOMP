# Reference: Audio Debugging — SPU2, IOP Sound Path, VAG/ADPCM

> **Load this for any AUDIO symptom** — total silence, music-but-no-SFX, SFX-but-no-music,
> crackling/popping, wrong pitch, audio that cuts out, or a game that STALLS waiting on sound
> completion (that last one overlaps `16-runtime-concurrency-threading.md` §6 — read both).
> Audio bugs don't crash and don't show on screen; you diagnose them by tracing the COMMAND PATH
> (EE → SIF RPC → IOP → SPU2) and verifying the DATA (VAG payloads) at each hop.
>
> Register-level truth: `09-ps2tek.md` (SPU2 section) via `08`; SDK signatures: `db-sdk-functions.md`.

---

## §1 Mental Model — The Audio Path

```
EE (recompiled game code)
  │  sound library calls (libsdr / custom sound engine)
  ▼
SIF RPC (EE→IOP)  ─ command buffers: "key voice N", "set pitch", "stream this"
  │
  ▼
IOP sound driver  ─ libsd (rom0/sdk) OR the GAME'S OWN sound IRX (very common!)
  │  writes SPU2 registers, feeds AutoDMA
  ▼
SPU2 — 2 cores × 24 voices = 48 voices. Each voice plays Sony ADPCM (VAG blocks).
  ├─ Keyed voices (KON/KOFF): SFX, jingles — sample fully in SPU2 RAM (2MB)
  └─ AutoDMA (ADMA): streamed PCM/BGM — IOP continuously refills a small double buffer
  ▼
Mix → host audio output (runtime: ps2_audio.cpp / ps2_audio_vag.cpp / ps2_iop_audio.cpp)
```

**The recomp reality:** only the EE is recompiled. Everything below "SIF RPC" is runtime C++.
The runtime intercepts either (a) the EE-side library functions (stub level — easier), or
(b) the RPC traffic itself (protocol level — needed when the game ships its own IOP driver).

**First question, always: WHICH driver?** Grep the ISO/IOP module list for the game's sound IRX.
- Standard `libsd`-family RPC → command codes are documented; runtime can interpret generically.
- **Custom sound IRX** (very common in big titles) → RPC payload is a private protocol. Generic
  interpretation impossible; you must reverse the EE-side wrapper functions (static export) and
  stub at THAT level, or implement a cooperative IOP (`16` §6) and run the IRX for real.

---

## §2 Symptom Triage Table

| Symptom | Most likely layer | First check |
|---------|------------------|-------------|
| **Total silence, game runs fine** | EE-side sound init stubbed to `ret0` and game gave up; or host audio device never opened | Log: does ANY sound RPC/stub fire per frame? (uncapped counter). Is host audio backend initialized (device open success log)? |
| **Music (streamed) but no SFX** | Keyed-voice path dead: KON never processed, VAG upload to SPU2 RAM dropped, or ENDX never set so game thinks all voices busy | Count KON events + voice-upload transfers. Check ENDX behavior (§4). |
| **SFX but no music** | AutoDMA/stream path dead: stream refill RPC stalls, file streaming reads fail | Trace the stream-open call → does the refill loop run? Ties to CD streaming (`19`). |
| **Crackle / pops** | Host buffer underrun (audio callback starved — often the guest-execution lock held too long), sample-rate mismatch, or ADPCM decode losing history across blocks | Measure callback timing; verify 48000 Hz end-to-end; check decoder keeps s1/s2 history per voice (§3). |
| **Wrong pitch (chipmunk/slow)** | PITCH conversion wrong | `pitch = (sampleRate << 12) / 48000`; `0x1000` = native 48 kHz. Verify VAG header sample rate vs applied pitch. |
| **Sound cuts out after N sounds** | Voice recycling broken — ENDX never set → game's voice allocator sees 48 busy voices forever | §4 ENDX. |
| **Game STALLS at cutscene/event waiting for sound** | completion signal never arrives (stream-done, voice-done) | This is the `16` §6 class. Fix completion semantics or gated event-skip. |
| **Volume zero / one channel only** | VOL registers written in sweep mode (bit 15 set) interpreted as raw volume, or L/R swap | Log VOLL/VOLR writes; bit 15 = sweep mode, not amplitude. |

---

## §3 VAG / ADPCM Data Verification

Sony ADPCM: **16-byte blocks → 28 PCM samples.** Byte 0 = predictor/shift nibbles, byte 1 = flags
(loop start / loop region / loop end / mute), bytes 2–15 = nibble data. Standalone `.VAG` files
carry a 48-byte header, magic `VAGp`, big-endian sample rate at offset 0x10 — but in-game banks
often strip headers and store raw blocks + a separate table.

**Checks when audio is WRONG (not absent):**
1. **Magic scan:** does the buffer the game uploads actually contain plausible ADPCM? (`VAGp` for
   files; for raw banks: flags bytes ∈ small set, not random.) Garbage in = wrong file/offset read
   (→ `19` file-I/O layer), not an SPU2 bug.
2. **Decoder history:** ADPCM is predictive — the decoder MUST carry the two previous samples
   (s1/s2) per voice ACROSS blocks. Resetting history each block = harsh buzz/crackle.
3. **Loop flags:** loop-end without loop-start set → one-shot; wrong loop handling = stuck droning
   note or SFX that never terminates (and never sets ENDX).
4. **Distinct-data rule (audio edition):** test with a real asset that has a KNOWN sound, not a
   sine you generated — you're testing the decode chain, not the mixer.

---

## §4 The ENDX / Completion Contract (top recurring root cause)

Games poll for "voice finished" to recycle voices and to gate script events:
- **ENDX register bit per voice** — set by hardware when a voice hits its loop-end/mute block.
- Sound engines also track "stream drained" for BGM and "all SFX done" for scene transitions.

**If your runtime plays audio but never reports completion**, the game leaks voices until the
allocator is exhausted (sound cuts out after N plays) or an event script parks forever (`16` §6).

**Contract to implement, even in the CHEAPEST stub tier:**
- KON → voice marked playing; after its computed duration (or immediately, in triage tier), set
  its ENDX bit and any engine-visible "done" status the EE-side wrapper reads.
- KOFF → enter release; ENDX per hardware semantics (verify in ps2tek).
- Reading ENDX clears it (read-acknowledge) in real HW — match what the game's polling loop
  expects (check the decompiled poll site in the static export before assuming).

---

## §5 Diagnosis Ladder (audio instance of `13-decisional-brain.md` §3)

```
L1  COMMAND FLOW — env-gated log of every audio stub/RPC hit: fn, command code, voice, addr, size.
    Uncapped counters per command type (does KON EVER fire? does stream-refill EVER fire?).
    Silence + zero commands = EE-side init failed earlier (find the ret0 that lied).
    Silence + commands flowing = runtime playback layer broken.
L2  DATA — dump ONE uploaded sample payload to a file; check VAG plausibility (§3).
L3  STATE — voice table snapshot on interval: playing?, pitch, volume, ENDX. Compare against what
    the game's allocator believes (read its voice-state globals via static export + memory read).
L4  REFERENCE — PCSX2: the DebugServer maps EE RAM only (no SPU2 regs), so A/B the EE-SIDE state:
    the sound engine's own globals/queues at the same moment. For ground-truth output, record
    PCSX2's audio (or use its SPU2 register/trace logging build) and compare event timing, not
    waveforms.
L5  CIRCUIT BREAKER — 09-ps2tek SPU2 section for exact register semantics; then ask user.
```

**Stub-tier strategy (correct order of ambition):**
1. **Tier 0 — honest silence:** all audio calls succeed, completion reported instantly (ENDX set,
   stream-done immediate). Game logic fully unblocked, zero sound. SHIP THIS FIRST — it converts
   audio from a blocker into a feature.
2. **Tier 1 — keyed SFX:** VAG decode + mix keyed voices on the host. Most audible win per effort.
3. **Tier 2 — streams/ADMA:** BGM streaming with real refill pacing + completion.
4. **Tier 3 — cooperative IOP running the real driver IRX** (`16` §6) — only if the custom-driver
   protocol is too hairy to stub. Big dedicated phase.

Every tier: env-gated (`<PREFIX>_NO_AUDIO`, `<PREFIX>_AUDIO_TIER=n`) per the lever doctrine
(`15` §5) so regressions bisect without rebuilds.

---

## §6 Host Output Sanity (before blaming the PS2 side)

- Output format: 48000 Hz stereo — resample ONCE at the mixer edge, nowhere else.
- Callback starvation: the host audio callback must NEVER wait on the guest-execution lock —
  buffer between guest-side producer and audio thread. Crackle correlated with heavy scenes =
  starvation, not decode.
- Volume: apply MVOL master + per-voice VOLL/VOLR; remember sweep-mode bit (§2 table).
- Start-up race: opening the device after the game already keyed voices = first sounds lost —
  harmless, don't chase it as a bug.

Cross-refs: stalls-on-sound `16-runtime-concurrency-threading.md` §6; file/stream reads
`19-memcard-pads-fileio.md`; FMV audio `20-fmv-ipu-cutscenes.md`; register truth `09-ps2tek.md`;
lever doctrine `15-vu1-gs-debugging.md` §5.
