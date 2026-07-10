# Reference: FMV / Cutscene Playback — IPU, .PSS Streams, Skip Strategy

> **Load this when the game hangs, black-screens, or crashes at a PRE-RENDERED VIDEO** — company
> logos, intro movie, cutscenes. FMV is the single most common early-boot blocker after module
> loading: many games play a video BEFORE the title screen, so "stuck at black screen after boot"
> is often FMV, not graphics. Distinguish first: real-time cutscenes (engine renders → `15`) vs
> pre-rendered video files (this file).

---

## §1 Mental Model — The FMV Path

```
.PSS/.PS2/.MPG file on disc  (MPEG-2 Program Stream w/ Sony private streams)
  │  sceCdStRead streaming (19 §3) into a ring buffer
  ▼
EE demux (libmpeg / sceMpeg*): splits video PES ↔ audio PES
  ├─ video → IPU (DMA ch4 toIPU: compressed) → IPU decodes macroblocks
  │          → (DMA ch3 fromIPU: RGB/indexed) → EE builds GS texture upload → screen
  └─ audio → ADPCM/PCM → SPU2 AutoDMA stream (18)
  Sync: frame pacing against vblank + audio clock
```

Three subsystems must cooperate (disc streaming, IPU video, audio stream) — an FMV hang is
usually ONE of them never completing, and the wait site tells you which.

**Detection — is this FMV?** Filenames `.pss`/`.ps2`/`.ipu`/`.mpg` in the ISO; `sceMpeg` /
`sceIpu` names or strings in the static export; DMA on channels 3/4 (`0x1000B000`/`0x1000B400`);
IPU register access (`0x10002000` range, IPU_CMD/IPU_CTRL/IPU_BP).

---

## §2 Strategy Tiers — Skip First, Decode Later

**Tier 0 — SKIP (do this first, always).** Real IPU decode is a huge subsystem; a port blocked at
the intro video needs the video GONE, not decoded:

- Preferred skip point: the game's own "play movie" WRAPPER function (find via the `.pss`
  filename string → xref in the static export). Override it to return "movie finished" —
  one clean cut above all three subsystems.
- Alternative: stub the `sceMpeg*` layer — init/create succeed, decode-frame reports
  end-of-stream immediately, delete/destroy succeed.
- **Contract for a safe skip:** the wrapper must still (a) return the success/finished code the
  caller checks, (b) leave any "movie done" flags/globals in the state the post-FMV code expects
  (read the caller's decompile — some set a mode variable after the call), (c) not leave the
  streaming API mid-stream (close/cancel the sceCdSt stream if the wrapper opened it, or the next
  file read inherits a busy stream → `19` §3 stall).
- Gate it: `<PREFIX>_SKIP_FMV=1` default ON during bring-up, and log which file was skipped.
  Document per the lever doctrine (`15` §5) — it's a band-aid with a removal condition (Tier 2).

**Tier 1 — AUDIO-ONLY (optional middle).** Some games carry dialogue in FMV audio; skipping loses
plot. Demux only the audio PES → `18` stream path, present black/static frames at the right
pacing so duration matches (scripts that time on video frames still work).

**Tier 2 — REAL DECODE.** MPEG-2 video decode (host library or software decoder) fed by the
demuxed PES, output converted to the GS upload the game expects — OR full IPU emulation if the
game feeds the IPU directly with its own demuxer. Only worth it once gameplay is solid. Check
upstream PS2Recomp PRs for an existing IPU/MPEG module before writing one (`10` §2 surgical-apply
rules — do not blind-merge).

---

## §3 Hang Triage — Which of the Three Legs Is Stuck?

| Wait site (from the stuck PC / decompile) | Leg | Fix direction |
|-------------------------------------------|-----|---------------|
| Polling IPU_CTRL / waiting DMA ch3 (fromIPU) completion | IPU video | Tier 0 skip at wrapper; if committed to decode: complete the fromIPU transfer with plausible data |
| Waiting on sceCdStRead ring level / stream sector count | disc streaming | `19` §3 — StRead must deliver + report honestly |
| Waiting "audio stream drained/finished" | audio | `18` §4 completion contract; `16` §6 if it's an IOP-signal park |
| Waiting on a frame-count vs vblank pacing loop | sync | ensure vsync events advance during FMV (present loop still ticking) |
| Crash (not hang) inside sceMpeg demux right after open | data | the stream read gave wrong bytes — `19` §3 LSN math / stream state, not an FMV bug |

Uncapped counters beat log spam here too (`15` §4.1): "toIPU qwords total, fromIPU qwords total,
StRead sectors total, audio samples total" — four numbers localize the dead leg in one run.

---

## §4 Post-Skip Verification

After ANY skip/stub, verify the handoff, not just "no hang":
1. Game reaches the NEXT scene (title/menu) — the golden smoke metric advances.
2. No stream left open (next file I/O works — load a save, enter a level).
3. Audio still alive after the skipped video (first menu SFX plays / audio counters move).
4. Record in the state file: which file(s) skipped, which tier, kill-switch name, evidence.

Cross-refs: disc streaming `19-memcard-pads-fileio.md` §3; audio stream + completion
`18-audio-spu2-iop-debugging.md`; hang model `16-runtime-concurrency-threading.md`; DMA channels
`01-ps2-hardware-bible.md`; IPU registers `db-registers.md` + `09-ps2tek.md`; lever doctrine
`15-vu1-gs-debugging.md` §5.
