# Reference: Peripherals & Storage — Memory Cards / Saves, Pad Input, Disc File I/O

> **Load this for:** infinite "checking memory card" screens, save/load failures or corrupted
> saves, no controller input / wrong buttons / dead analog sticks, "file not found", wrong data
> loaded from disc, streaming stalls. These subsystems share one property: the EE never touches
> them directly — everything goes through **SIF RPC to the IOP**, and the runtime replaces the IOP
> side with host implementations. Get the COMPLETION SEMANTICS wrong and games hang; get the DATA
> LAYOUT wrong and games corrupt.
>
> SDK signatures: `db-sdk-functions.md`. Pad wire protocol: `padspecs.txt`. Register truth: `09-ps2tek.md`.

---

## §1 Memory Cards & Saves (libmc → mcserv)

### The async contract — where every hang comes from

EVERY libmc call is **asynchronous**: `sceMcXxxx(...)` merely queues the request; the game then
polls `sceMcSync(mode, &cmd, &result)` until completion. The runtime MUST honor this shape:

- `sceMcSync` returns "still executing" vs "done + here's the result of command X".
- A runtime that does the work synchronously inside `sceMcXxxx` but never lets `McSync` report
  completion → **infinite "checking memory card" screen**.
- A runtime that reports done but returns the WRONG result code → game concludes "no card" /
  "unformatted" and either loops a format prompt or disables saving.

### The boot handshake (get this right first)

At boot virtually every game runs: `McInit → McGetInfo(port,slot) → [format decision] → McOpen…`.
`McGetInfo` result semantics (verify exact values against `db-sdk-functions.md` / the decompiled
caller in the static export — do NOT trust memory): 0 = same card as last check, negative codes
distinguish "card switched", "not formatted", "no card". **Easiest stable stub: always report a
formatted card, present, unchanged.** The first `McGetInfo` after boot may legitimately need the
"card changed" code once — read the game's own check loop in the static export to see which
sequence it accepts, then lock that sequence in.

### Host mapping

- Map each card to a host dir: `saves/mc0/`, `saves/mc1/`. Saves are directories
  (`BESLES-XXXXX.../`) containing the data file(s) + `icon.sys` + `.ico` icons — mirror that tree
  1:1 on the host; don't invent a container format.
- `McGetDir` fills an array of dir-entry structs (name, size, attributes, timestamps). **Struct
  layout errors here silently corrupt the save-select screen** — verify field offsets from the
  decompiled consumer, not from documentation guesses.
- Free-space queries: return a large-but-plausible free cluster count; 8 MB card = 8000 KB-ish.
  Returning 0 free space disables saving; returning absurd values can overflow game UI math.
- **Writes must be durable + atomic-ish:** write to temp then rename, so a crash mid-save doesn't
  leave a half-file that later loads as a corrupt save.

### Triage checklist for save bugs

1. Log every libmc entry point with args + every McSync poll with returned (cmd, result).
2. Hang at card screen → McSync never reports completion (async contract broken).
3. "No card / format?" prompt → McGetInfo result codes wrong for the game's expected sequence.
4. Corrupt save-list UI → McGetDir struct layout.
5. Load succeeds but game state garbage → McRead size/offset handling; compare file bytes
   host-side vs what lands in guest RAM (memory_diff around the read).

---

## §2 Pad / Controller Input (libpad → padman)

### The data contract

`scePadRead(port, slot, buf)` fills a buffer the GAME parses: `buf[0]` = status, `buf[1]` = mode
ID (high nibble = mode: 0x4 digital, 0x7 analog(s); low nibble = payload size in half-words —
0x41 digital, 0x73/0x79 analog family), `buf[2..3]` = **digital buttons, ACTIVE-LOW** (0xFFFF =
nothing pressed), `buf[4..7]` = right/left stick axes (0x80 = center) in analog modes. Full wire
protocol: `padspecs.txt`.

### The state machine — games gate on it

Games poll `scePadGetState` until the pad reports STABLE before reading, and drive mode switches
(`scePadSetMainMode` for analog) expecting the state to cycle through EXECCMD back to STABLE.
**A runtime that reports STABLE instantly but ignores mode switches** leaves games stuck digital
(analog stick dead) or stuck waiting for the mode-change round-trip.

### Triage table

| Symptom | Cause | Check |
|---------|-------|-------|
| No input at all | game still waiting for STABLE state, or reads a different port/slot than you feed | log scePadGetState polls + which (port,slot) scePadRead hits |
| Everything pressed at once / inverted | active-LOW forgotten (0x0000 = ALL buttons down) | buf[2..3] must idle at 0xFFFF |
| Buttons scrambled | byte order — some engines read the mask byte-swapped (real case: DC2 needed swap16 on scripted masks) | compare one known button bit end-to-end |
| Analog sticks dead | mode still 0x41 digital; game's SetMainMode round-trip never completed | log mode-change requests; return analog mode ID after them |
| Camera/movement drifts | axes not centered at 0x80 when idle | idle dump of buf[4..7] |
| Works then stops after scene change | game re-runs pad init and a second init path isn't handled | log scePadPortOpen/Close lifecycle |

---

## §3 Disc File I/O (cdvdman: sceCdSearchFile / sceCdRead / sceCdSync / sceCdStRead)

### Two access styles — support BOTH

1. **By filename:** `sceCdSearchFile("\\DATA\\FILE.BIN;1", &fp)` → returns the file's **LSN**
   (logical sector number) + size; then the game reads by LSN.
2. **By raw LSN:** `sceCdRead(lsn, nsectors, buf, mode)` straight into pack files (`DATA.BND`)
   whose internal offsets the game computed itself.

Because of style 2, **the faithful host mapping is the ISO itself, not an extracted tree**:
`host_offset = LSN * 2048`, read `nsectors * 2048` bytes, done. An extracted-files mapping breaks
the moment a game does arithmetic on LSNs (reading a pack file at `base_lsn + internal_offset`).
Keep the ISO open read-only and serve every sceCdRead from it.

**Path normalization for SearchFile:** uppercase, `\` separators, strip `cdrom0:\` prefix and the
ISO9660 `;1` version suffix. Build the filename→(LSN,size) table ONCE from the ISO's directory
records at boot.

### Async + streaming semantics

- `sceCdRead` KICKS a read; `sceCdSync(0)` blocks until done, `sceCdSync(1)` polls. Synchronous
  host implementation is fine — but `sceCdSync` must then report "done", and any registered
  callback (`sceCdCallback`) must fire, or the game's I/O state machine parks (`16` hang class).
- **Streaming API (`sceCdStStart/StRead/StSeek`)** feeds FMV and audio: a ring buffer the game
  drains while the drive fills. Implement StRead honestly (deliver requested sectors, report the
  count) — under-reporting stalls streams, over-reporting corrupts them. Symptoms land in `18`
  (music stops) or `20` (FMV freezes) but the fault is here.
- `sceCdDiskReady`, `sceCdGetDiskType` → always ready, correct disk type (PS2 DVD vs CD matters:
  some engines pick sector layout by type).
- **Speed emulation: DON'T.** Serve reads instantly. (Only pathological cases rely on seek
  timing; cross that bridge only with evidence.)

### Triage checklist

1. "file not found" → log the EXACT string reaching SearchFile before normalization; 9/10 it's a
   prefix/suffix/case mismatch, not a missing file.
2. Wrong data loaded → verify `LSN*2048` math and mode (2048 data vs 2340/2352 raw modes — games
   using raw CDDA/XA reads need the sector's raw layout; check the `mode` arg you're ignoring).
3. Stream stutters/stalls → count StRead served vs requested; check the completion/callback path.
4. Boot hangs after module loads → often the FIRST SearchFile of the boot chain failed silently
   and the game retries forever; the log from (1) shows it.

Cross-refs: RPC/IOP model `01-ps2-hardware-bible.md` §4; hang semantics
`16-runtime-concurrency-threading.md`; audio streams `18-audio-spu2-iop-debugging.md`; FMV
`20-fmv-ipu-cutscenes.md`; SDK signatures `db-sdk-functions.md`; pad protocol `padspecs.txt`.
