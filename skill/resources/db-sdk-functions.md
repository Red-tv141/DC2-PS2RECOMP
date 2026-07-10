# PS2 SDK Function Stub Database
> Cross-reference of PS2 SDK library functions to their ps2xRuntime stub implementations.
>
> **See also**: `db-syscalls.md` (kernel-level syscalls these SDK functions call), `db-registers.md` (DMA/GIF/VIF registers these functions configure), `db-ps2-architecture.md` (DMA/GS/VU subsystem context).

## Lookup Protocol
1. Match the imported function name from the ELF symbol table
2. Check **File** for the implementation source
3. **Status**: `impl` = real logic, `nop` = safe return-0, `log-only` = prints then returns 0
4. All stubs use signature: `void funcName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)`

---

## CD/DVD Subsystem (`libcdvd`)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceCdRead | impl | 1=ok, 0=fail | ps2_stubs_ps2.inl | LBN-based sector read with auto-recovery for swapped args |
| sceCdSync | impl | 0 | ps2_stubs_ps2.inl | Always returns "not busy" |
| sceCdGetError | impl | error_code | ps2_stubs_ps2.inl | Returns `g_lastCdError` |
| sceCdRI | nop | 0 | ps2_stubs_misc.inl | Read disc ID — stubbed |
| sceCdRM | nop | 0 | ps2_stubs_misc.inl | Read media type — stubbed |
| sceCdApplyNCmd | nop | 0 | ps2_stubs_misc.inl | |
| sceCdBreak | nop | 0 | ps2_stubs_misc.inl | |
| sceCdCallback | nop | 0 | ps2_stubs_misc.inl | |
| sceCdChangeThreadPriority | nop | 0 | ps2_stubs_misc.inl | |
| sceCdDelayThread | nop | 0 | ps2_stubs_misc.inl | |
| sceCdDiskReady | nop | 2 | ps2_stubs_misc.inl | Returns "ready" |
| sceCdGetDiskType | nop | 0x14 | ps2_stubs_misc.inl | PS2CD type |
| sceCdGetReadPos | nop | 0 | ps2_stubs_misc.inl | |
| sceCdGetToc | impl | 1 | ps2_stubs_misc.inl | Fills TOC buffer from ISO |
| sceCdInit | impl | 1 | ps2_stubs_misc.inl | |
| sceCdInitEeCB | nop | 0 | ps2_stubs_misc.inl | |
| sceCdIntToPos | impl | v0=result | ps2_stubs_misc.inl | LBA ↔ MSF conversion |
| sceCdMmode | nop | 0 | ps2_stubs_misc.inl | |
| sceCdNcmdDiskReady | nop | 0 | ps2_stubs_misc.inl | |
| sceCdPause | nop | 1 | ps2_stubs_misc.inl | |
| sceCdPosToInt | impl | v0=LBA | ps2_stubs_misc.inl | MSF → LBA conversion |
| sceCdReadChain | impl | 1 | ps2_stubs_misc.inl | Chain-read from sector list |
| sceCdReadClock | impl | 1 | ps2_stubs_misc.inl | Returns real clock via chrono |
| sceCdReadIOPm | nop | 0 | ps2_stubs_misc.inl | |
| sceCdSearchFile | impl | 1 | ps2_stubs_misc.inl | ISO9660 file search |
| sceCdSeek | nop | 1 | ps2_stubs_misc.inl | |
| sceCdStandby | nop | 1 | ps2_stubs_misc.inl | |
| sceCdStatus | nop | 0x0A | ps2_stubs_misc.inl | "Open tray" status |
| sceCdStInit | nop | 1 | ps2_stubs_misc.inl | |
| sceCdStop | nop | 1 | ps2_stubs_misc.inl | |
| sceCdStPause | nop | 1 | ps2_stubs_misc.inl | |
| sceCdStRead | impl | v0 | ps2_stubs_misc.inl | Streaming read from LBN |
| sceCdStream | nop | 0 | ps2_stubs_misc.inl | |
| sceCdStResume | nop | 1 | ps2_stubs_misc.inl | |
| sceCdStSeek | nop | 1 | ps2_stubs_misc.inl | |

---

## GS/Graphics Subsystem

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| GsSyncV | impl | field_count | ps2_stubs_gs.inl | VSync wait with actual frame timing |
| GsSyncPath | impl | 0 | ps2_stubs_gs.inl | Waits for GIF/VIF/GS idle |
| GsSetCRTCMode | impl | — | ps2_stubs_gs.inl | Sets CRTC interlace/field mode |
| GsClearDispMask | impl | — | ps2_stubs_gs.inl | |
| GsSetDefDispEnv | impl | — | ps2_stubs_gs.inl | Initializes display environment struct |
| GsSetDefDrawEnv | impl | — | ps2_stubs_gs.inl | Initializes draw environment struct |
| GsPutDispEnv | impl | — | ps2_stubs_gs.inl | Sends display env to GS |
| GsPutDrawEnv | impl | — | ps2_stubs_gs.inl | Sends draw env to GS |
| GsLoadImage | impl | — | ps2_stubs_gs.inl | GIF-DMA image upload |
| GsSetDefaultPrimEnvInit | impl | — | ps2_stubs_gs.inl | |
| GsResetGraph | impl | — | ps2_stubs_gs.inl | Full GS reset sequence |
| qwordCopy | impl | — | ps2_stubs_gs.inl | Quad-word memcpy for GS packets |

---

## SIF/IOP Communication

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceSifCmdIntrHdlr | nop | 0 | ps2_stubs_misc.inl | |
| sceSifLoadModule | nop | ≥0 | ps2_stubs_misc.inl | Returns fake module ID |
| sceSifSendCmd | nop | 0 | ps2_stubs_misc.inl | |
| sceRpcFreePacket | nop | 0 | ps2_stubs_misc.inl | |
| sceRpcGetFPacket | nop | 0 | ps2_stubs_misc.inl | |
| sceRpcGetFPacket2 | nop | 0 | ps2_stubs_misc.inl | |

---

## Math / VU0 Helpers

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceVu0ecossin | impl | — | ps2_stubs_misc.inl | VU0 extended-precision sin |
| sceVu0ecoscos | impl | — | ps2_stubs_misc.inl | VU0 extended-precision cos |
| sceVu0eatan2 | impl | — | ps2_stubs_misc.inl | VU0 atan2 |
| sceVu0esqrt | impl | — | ps2_stubs_misc.inl | VU0 sqrt |

---

## Filesystem

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceFsDbChk | nop | 0 | ps2_stubs_misc.inl | |
| sceFsIntrSigSema | nop | 0 | ps2_stubs_misc.inl | |
| sceFsSemExit | nop | 0 | ps2_stubs_misc.inl | |
| sceFsSemInit | nop | 0 | ps2_stubs_misc.inl | |
| sceFsSigSema | nop | 0 | ps2_stubs_misc.inl | |

---

## Pad/Input

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| scePadInit | impl | 1 | ps2_stubs_misc.inl | Initializes pad subsystem |
| scePadPortOpen | impl | ≥0 | ps2_stubs_misc.inl | Opens pad port, returns handle |
| scePadRead | impl | — | ps2_stubs_misc.inl | Reads pad state → writes to buffer |
| scePadSetMainMode | nop | 1 | ps2_stubs_misc.inl | |
| scePadGetState | nop | 6 | ps2_stubs_misc.inl | Returns "stable" |
| scePadSetActDirect | nop | 1 | ps2_stubs_misc.inl | |
| scePadSetActAlign | nop | 1 | ps2_stubs_misc.inl | |
| scePadInfoPressMode | nop | 0 | ps2_stubs_misc.inl | |
| scePadEnterPressMode | nop | 1 | ps2_stubs_misc.inl | |

---

## DMA Helpers

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| FlushToDmac | impl | — | ps2_stubs_misc.inl | Submits DMA transfer to memory |
| SendDmaTag | impl | — | ps2_stubs_misc.inl | Enqueues DMA chain tag |
| VifSendPacket | impl | — | ps2_stubs_misc.inl | VIF1 packet submission |
| vifSendPacket | impl | — | ps2_stubs_misc.inl | |

---

## Misc / System

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| InitThread | log-only | 1 | ps2_stubs_ps2.inl | |
| builtin_set_imask | log-only | 0 | ps2_stubs_ps2.inl | |
| sceIDC | nop | 0 | ps2_stubs_misc.inl | |
| sceSDC | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegFlush | nop | 0 | ps2_stubs_misc.inl | |

---

## Resident Evil CV Specific (game override)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| syRtcInit | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| syFree | impl | — | ps2_stubs_residentEvilCV.inl | Custom allocator free |
| syMalloc | impl | addr | ps2_stubs_residentEvilCV.inl | Custom allocator malloc |
| syMallocInit | impl | — | ps2_stubs_residentEvilCV.inl | |
| syHwInit | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| syHwInit2 | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| InitGdSystemEx | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| pdInitPeripheral | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| pdGetPeripheral | impl | — | ps2_stubs_residentEvilCV.inl | Returns pad state for RE:CV |
| Ps2SwapDBuff | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| Ps2_pad_actuater | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| InitReadKeyEx | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| SetRepeatKeyTimer | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| sdDrvInit | nop | 0 | ps2_stubs_residentEvilCV.inl | Sound driver init |
| sdSndStopAll | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| sdSysFinish | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| ADXF_LoadPartitionNw | nop | 0 | ps2_stubs_residentEvilCV.inl | CRI ADX stub |
| ADXT_Init | nop | 0 | ps2_stubs_residentEvilCV.inl | |
| ADXT_SetNumRetry | nop | 0 | ps2_stubs_residentEvilCV.inl | |

---

## C Library Reimplementations (`libc`)

> Full libc replacements — these intercept standard C calls compiled into the PS2 ELF.
> All are **impl** with real logic, not no-ops.

### Memory Allocation

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| malloc | impl | addr | ps2_stubs_libc.inl | Guest heap allocator via PS2Runtime |
| free | impl | — | ps2_stubs_libc.inl | Guest heap free |
| calloc | impl | addr | ps2_stubs_libc.inl | malloc + zero-fill |
| realloc | impl | addr | ps2_stubs_libc.inl | Resize allocation |

### Memory Operations

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| memcpy | impl | v0=dest | ps2_stubs_libc.inl | Host-side std::memcpy on guest pointers |
| memset | impl | v0=dest | ps2_stubs_libc.inl | Host-side std::memset |
| memmove | impl | v0=dest | ps2_stubs_libc.inl | Overlap-safe copy |
| memcmp | impl | v0=cmp | ps2_stubs_libc.inl | Byte-by-byte comparison |

### String Operations

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| strcpy | impl | v0=dest | ps2_stubs_libc.inl | |
| strncpy | impl | v0=dest | ps2_stubs_libc.inl | |
| strlen | impl | v0=len | ps2_stubs_libc.inl | |
| strcmp | impl | v0=cmp | ps2_stubs_libc.inl | |
| strncmp | impl | v0=cmp | ps2_stubs_libc.inl | |
| strcat | impl | v0=dest | ps2_stubs_libc.inl | |
| strncat | impl | v0=dest | ps2_stubs_libc.inl | |
| strchr | impl | v0=ptr | ps2_stubs_libc.inl | Returns guest pointer or 0 |
| strrchr | impl | v0=ptr | ps2_stubs_libc.inl | Reverse search |
| strstr | impl | v0=ptr | ps2_stubs_libc.inl | Substring search |

### Formatted I/O

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| printf | impl | v0=chars | ps2_stubs_libc.inl | Reads fmt from guest, prints to stdout |
| sprintf | impl | v0=chars | ps2_stubs_libc.inl | Writes formatted string to guest buffer |
| snprintf | impl | v0=chars | ps2_stubs_libc.inl | Size-bounded variant |
| fprintf | impl | v0=chars | ps2_stubs_libc.inl | Writes to host FILE* from guest handle |
| puts | impl | v0=status | ps2_stubs_libc.inl | Reads guest string, prints + newline |

### File I/O

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| fopen | impl | v0=handle | ps2_stubs_libc.inl | Opens host file, returns guest handle |
| fclose | impl | v0=status | ps2_stubs_libc.inl | Closes host FILE*, removes handle |
| fread | impl | v0=items | ps2_stubs_libc.inl | Reads into guest buffer |
| fwrite | impl | v0=items | ps2_stubs_libc.inl | Writes from guest buffer |
| fseek | impl | v0=status | ps2_stubs_libc.inl | Seeks on host FILE* |
| ftell | impl | v0=offset | ps2_stubs_libc.inl | Returns host file position |
| fflush | impl | v0=status | ps2_stubs_libc.inl | Flushes host FILE* |

### Math

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sqrt | impl | f0 | ps2_stubs_libc.inl | Reads f12, returns via FPR |
| sin | impl | f0 | ps2_stubs_libc.inl | |
| cos | impl | f0 | ps2_stubs_libc.inl | |
| tan | impl | f0 | ps2_stubs_libc.inl | |
| atan2 | impl | f0 | ps2_stubs_libc.inl | atan2(f12, f13) |
| pow | impl | f0 | ps2_stubs_libc.inl | pow(f12, f13) |
| exp | impl | f0 | ps2_stubs_libc.inl | |
| log | impl | f0 | ps2_stubs_libc.inl | Natural log |
| log10 | impl | f0 | ps2_stubs_libc.inl | Base-10 log |
| ceil | impl | f0 | ps2_stubs_libc.inl | |
| floor | impl | f0 | ps2_stubs_libc.inl | |
| fabs | impl | f0 | ps2_stubs_libc.inl | |

### Internal Math Helpers

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| __kernel_sinf | impl | f0 | ps2_stubs_libc.inl | newlib kernel sin |
| __kernel_cosf | impl | f0 | ps2_stubs_libc.inl | newlib kernel cos |
| __ieee754_rem_pio2f | impl | v0=quadrant | ps2_stubs_libc.inl | Argument reduction for trig |

---

## SIF/IOP Communication (extended)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceSifInitRpc | nop | 0 | ps2_stubs_misc.inl | |
| sceSifExitRpc | nop | 0 | ps2_stubs_misc.inl | |
| sceSifInitCmd | nop | 0 | ps2_stubs_misc.inl | |
| sceSifExitCmd | nop | 0 | ps2_stubs_misc.inl | |
| sceSifBindRpc | nop | 0 | ps2_stubs_misc.inl | |
| sceSifRegisterRpc | nop | 0 | ps2_stubs_misc.inl | |
| sceSifRemoveRpc | nop | 0 | ps2_stubs_misc.inl | |
| sceSifRemoveRpcQueue | nop | 0 | ps2_stubs_misc.inl | |
| sceSifSetRpcQueue | nop | 0 | ps2_stubs_misc.inl | |
| sceSifRpcLoop | nop | 0 | ps2_stubs_misc.inl | |
| sceSifExecRequest | nop | 0 | ps2_stubs_misc.inl | |
| sceSifCheckStatRpc | nop | 0 | ps2_stubs_misc.inl | |
| sceSifGetNextRequest | nop | 0 | ps2_stubs_misc.inl | |
| sceSifGetOtherData | nop | 0 | ps2_stubs_misc.inl | |
| sceSifSetCmdBuffer | nop | 0 | ps2_stubs_misc.inl | |
| sceSifSetSysCmdBuffer | nop | 0 | ps2_stubs_misc.inl | |
| sceSifAddCmdHandler | nop | 0 | ps2_stubs_misc.inl | |
| sceSifRemoveCmdHandler | nop | 0 | ps2_stubs_misc.inl | |
| sceSifDmaStat | nop | 0 | ps2_stubs_misc.inl | |
| sceSifSetDma | nop | 0 | ps2_stubs_misc.inl | |
| sceSifSetDChain | nop | 0 | ps2_stubs_misc.inl | |
| sceSifStopDma | nop | 0 | ps2_stubs_misc.inl | |
| sceSifGetDataTable | nop | 0 | ps2_stubs_misc.inl | |
| sceSifGetIopAddr | nop | 0 | ps2_stubs_misc.inl | |
| sceSifSetIopAddr | nop | 0 | ps2_stubs_misc.inl | |
| sceSifGetReg | nop | 0 | ps2_stubs_misc.inl | |
| sceSifSetReg | nop | 0 | ps2_stubs_misc.inl | |
| sceSifGetSreg | nop | 0 | ps2_stubs_misc.inl | |
| sceSifSetSreg | nop | 0 | ps2_stubs_misc.inl | |
| sceSifLoadModule | nop | ≥0 | ps2_stubs_misc.inl | Returns fake module ID |
| sceSifLoadModuleBuffer | nop | ≥0 | ps2_stubs_misc.inl | |
| sceSifLoadElf | nop | 0 | ps2_stubs_misc.inl | |
| sceSifLoadElfPart | nop | 0 | ps2_stubs_misc.inl | |
| sceSifLoadFileReset | nop | 0 | ps2_stubs_misc.inl | |
| sceSifAllocIopHeap | nop | 0 | ps2_stubs_misc.inl | |
| sceSifFreeIopHeap | nop | 0 | ps2_stubs_misc.inl | |
| sceSifLoadIopHeap | nop | 0 | ps2_stubs_misc.inl | |
| sceSifWriteBackDCache | nop | 0 | ps2_stubs_misc.inl | |
| sceSifRebootIop | nop | 0 | ps2_stubs_misc.inl | |
| sceSifResetIop | nop | 0 | ps2_stubs_misc.inl | |
| sceSifSyncIop | nop | 0 | ps2_stubs_misc.inl | |
| sceSifIsAliveIop | nop | 1 | ps2_stubs_misc.inl | Returns "alive" |

---

## DMA Subsystem (`libdma`)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceDmaReset | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaGetChan | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaGetEnv | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaPutEnv | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaSend | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaSendI | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaSendM | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaSendN | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaRecv | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaRecvI | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaRecvN | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaSync | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaSyncN | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaCallback | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaPause | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaRestart | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaPutStallAddr | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaWatch | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaLastSyncTime | nop | 0 | ps2_stubs_misc.inl | |
| sceDmaDebug | nop | 0 | ps2_stubs_misc.inl | |
| DmaAddr | nop | 0 | ps2_stubs_misc.inl | |

---

## Memory Card (`libmc`)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceMcInit | nop | 0 | ps2_stubs_misc.inl | |
| sceMcOpen | nop | 0 | ps2_stubs_misc.inl | |
| sceMcClose | nop | 0 | ps2_stubs_misc.inl | |
| sceMcRead | nop | 0 | ps2_stubs_misc.inl | |
| sceMcWrite | nop | 0 | ps2_stubs_misc.inl | |
| sceMcSeek | nop | 0 | ps2_stubs_misc.inl | |
| sceMcSync | nop | 0 | ps2_stubs_misc.inl | |
| sceMcGetDir | nop | 0 | ps2_stubs_misc.inl | |
| sceMcGetInfo | nop | 0 | ps2_stubs_misc.inl | |
| sceMcGetSlotMax | nop | 0 | ps2_stubs_misc.inl | |
| sceMcGetEntSpace | nop | 0 | ps2_stubs_misc.inl | |
| sceMcMkdir | nop | 0 | ps2_stubs_misc.inl | |
| sceMcChdir | nop | 0 | ps2_stubs_misc.inl | |
| sceMcDelete | nop | 0 | ps2_stubs_misc.inl | |
| sceMcRename | nop | 0 | ps2_stubs_misc.inl | |
| sceMcFormat | nop | 0 | ps2_stubs_misc.inl | |
| sceMcUnformat | nop | 0 | ps2_stubs_misc.inl | |
| sceMcFlush | nop | 0 | ps2_stubs_misc.inl | |
| sceMcSetFileInfo | nop | 0 | ps2_stubs_misc.inl | |
| sceMcChangeThreadPriority | nop | 0 | ps2_stubs_misc.inl | |

### Memory Card UI Helpers (game-specific)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| mcSelectFileInfoInit | nop | 0 | ps2_stubs_misc.inl | |
| mcSelectSaveFileCheck | nop | 0 | ps2_stubs_misc.inl | |
| mcDisplaySelectFileInfo | nop | 0 | ps2_stubs_misc.inl | |
| mcDisplaySelectFileInfoMesCount | nop | 0 | ps2_stubs_misc.inl | |
| mcDispWindowCurSol | nop | 0 | ps2_stubs_misc.inl | |
| mcDispWindowFoundtion | nop | 0 | ps2_stubs_misc.inl | |
| mceGetInfoApdx | nop | 0 | ps2_stubs_misc.inl | |
| mceIntrReadFixAlign | nop | 0 | ps2_stubs_misc.inl | |
| mceStorePwd | nop | 0 | ps2_stubs_misc.inl | |
| mcGetConfigCapacitySize | nop | 0 | ps2_stubs_misc.inl | |
| mcGetFileSelectWindowCursol | nop | 0 | ps2_stubs_misc.inl | |
| mcGetFreeCapacitySize | nop | 0 | ps2_stubs_misc.inl | |
| mcGetIconCapacitySize | nop | 0 | ps2_stubs_misc.inl | |
| mcGetIconFileCapacitySize | nop | 0 | ps2_stubs_misc.inl | |
| mcGetPortSelectDirInfo | nop | 0 | ps2_stubs_misc.inl | |
| mcGetSaveFileCapacitySize | nop | 0 | ps2_stubs_misc.inl | |
| mcGetStringEnd | nop | 0 | ps2_stubs_misc.inl | |
| mcMoveFileSelectWindowCursor | nop | 0 | ps2_stubs_misc.inl | |
| mcNewCreateConfigFile | nop | 0 | ps2_stubs_misc.inl | |
| mcNewCreateIcon | nop | 0 | ps2_stubs_misc.inl | |
| mcNewCreateSaveFile | nop | 0 | ps2_stubs_misc.inl | |
| mcReadIconData | nop | 0 | ps2_stubs_misc.inl | |
| mcReadStartConfigFile | nop | 0 | ps2_stubs_misc.inl | |
| mcReadStartSaveFile | nop | 0 | ps2_stubs_misc.inl | |
| mcSetFileSelectWindowCursol | nop | 0 | ps2_stubs_misc.inl | |
| mcSetFileSelectWindowCursolInit | nop | 0 | ps2_stubs_misc.inl | |
| mcSetStringSaveFile | nop | 0 | ps2_stubs_misc.inl | |
| mcSetTyepWriteMode | nop | 0 | ps2_stubs_misc.inl | |
| mcWriteIconData | nop | 0 | ps2_stubs_misc.inl | |
| mcWriteStartConfigFile | nop | 0 | ps2_stubs_misc.inl | |
| mcWriteStartSaveFile | nop | 0 | ps2_stubs_misc.inl | |

---

## MPEG Video (`libmpeg`)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceMpegInit | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegCreate | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegDelete | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegReset | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegAddBs | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegAddCallback | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegAddStrCallback | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegClearRefBuff | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegDemuxPss | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegDemuxPssRing | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegGetPicture | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegGetPictureRAW8 | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegGetPictureRAW8xy | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegGetDecodeMode | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegSetDecodeMode | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegSetDefaultPtsGap | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegResetDefaultPtsGap | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegSetImageBuff | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegIsEnd | nop | 1 | ps2_stubs_misc.inl | Returns "ended" |
| sceMpegIsRefBuffEmpty | nop | 1 | ps2_stubs_misc.inl | Returns "empty" |
| sceMpegDispWidth | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegDispHeight | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegDispCenterOffX | nop | 0 | ps2_stubs_misc.inl | |
| sceMpegDispCenterOffY | nop | 0 | ps2_stubs_misc.inl | |

---

## GS Extended (`libgraph`)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceGsSyncV | impl | field_count | ps2_stubs_misc.inl | VSync wait |
| sceGsSyncVCallback | impl | — | ps2_stubs_misc.inl | |
| resetGsSyncVCallbackState | impl | — | ps2_stubs_misc.inl | |
| sceGsSyncPath | impl | 0 | ps2_stubs_misc.inl | |
| sceGsResetGraph | impl | — | ps2_stubs_misc.inl | |
| sceGsResetPath | nop | 0 | ps2_stubs_misc.inl | |
| sceGsGetGParam | nop | 0 | ps2_stubs_misc.inl | |
| sceGsSetDefDispEnv | impl | — | ps2_stubs_misc.inl | |
| sceGsSetDefDrawEnv | impl | — | ps2_stubs_misc.inl | |
| sceGsSetDefDrawEnv2 | impl | — | ps2_stubs_misc.inl | |
| sceGsPutDispEnv | impl | — | ps2_stubs_misc.inl | |
| sceGsPutDrawEnv | impl | — | ps2_stubs_misc.inl | |
| sceGsExecLoadImage | impl | — | ps2_stubs_misc.inl | |
| sceGsExecStoreImage | nop | 0 | ps2_stubs_misc.inl | |
| sceGsSetDefLoadImage | nop | 0 | ps2_stubs_misc.inl | |
| sceGsSetDefStoreImage | nop | 0 | ps2_stubs_misc.inl | |
| sceGsSetDefDBuffDc | nop | 0 | ps2_stubs_misc.inl | |
| sceGsSwapDBuffDc | nop | 0 | ps2_stubs_misc.inl | |
| sceGsSetDefClear | nop | 0 | ps2_stubs_misc.inl | |
| sceGszbufaddr | nop | 0 | ps2_stubs_misc.inl | |

---

## Sound / Synthesizer (`libsd`, `libssyn`, `libsynth`)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceSdRemoteInit | nop | 0 | ps2_stubs_misc.inl | |
| sceSdRemote | nop | 0 | ps2_stubs_misc.inl | |
| sceSdCallBack | nop | 0 | ps2_stubs_misc.inl | |
| sceSdTransToIOP | nop | 0 | ps2_stubs_misc.inl | |
| sndr_trans_func | nop | 0 | ps2_stubs_misc.inl | |
| StopFxProgram | nop | 0 | ps2_stubs_misc.inl | |

> ~60 `sceSSyn_*` and `sceSynthesizer*` functions are also stubbed as nop in ps2_stubs_misc.inl.
> They cover MIDI synthesizer operations (NoteOn/Off, volume, panpot, LFO, envelope, DMA).
> All return 0. See source for complete list.

---

## VU0 Math (`libvu0`)

> ~40 `sceVu0*` functions are stubbed. Key ones:

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceVu0ecossin | impl | — | ps2_stubs_misc.inl | Extended-precision sin |
| sceVu0AddVector | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0ApplyMatrix | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0CameraMatrix | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0CopyMatrix | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0CopyVector | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0InnerProduct | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0InversMatrix | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0MulMatrix | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0MulVector | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0Normalize | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0RotMatrix | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0RotTransPers | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0TransMatrix | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0UnitMatrix | nop | 0 | ps2_stubs_misc.inl | |
| sceVu0ViewScreenMatrix | nop | 0 | ps2_stubs_misc.inl | |
| sceVpu0Reset | nop | 0 | ps2_stubs_misc.inl | |

> Full list: sceVu0{ClampVector,ClipAll,ClipScreen,ClipScreen3,DivVector,DivVectorXYZ,DropShadowMatrix,FTOI0Vector,FTOI4Vector,InterVector,InterVectorXYZ,ITOF0Vector,ITOF4Vector,ITOF12Vector,LightColorMatrix,NormalLightMatrix,OuterProduct,RotMatrixX,RotMatrixY,RotMatrixZ,RotTransPersN,ScaleVector,ScaleVectorXYZ,SubVector,TransposeMatrix,CopyVectorXYZ}

---

## Pad/Input (extended)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| scePadInit2 | nop | 1 | ps2_stubs_misc.inl | |
| scePadEnd | nop | 1 | ps2_stubs_misc.inl | |
| scePadPortClose | nop | 1 | ps2_stubs_misc.inl | |
| scePadExitPressMode | nop | 0 | ps2_stubs_misc.inl | |
| scePadGetPortMax | nop | 2 | ps2_stubs_misc.inl | |
| scePadGetSlotMax | nop | 1 | ps2_stubs_misc.inl | |
| scePadGetModVersion | nop | 0 | ps2_stubs_misc.inl | |
| scePadGetFrameCount | nop | 0 | ps2_stubs_misc.inl | |
| scePadGetDmaStr | nop | 0 | ps2_stubs_misc.inl | |
| scePadGetReqState | nop | 0 | ps2_stubs_misc.inl | |
| scePadSetReqState | nop | 0 | ps2_stubs_misc.inl | |
| scePadGetButtonMask | nop | 0 | ps2_stubs_misc.inl | |
| scePadSetButtonInfo | nop | 0 | ps2_stubs_misc.inl | |
| scePadInfoAct | nop | 0 | ps2_stubs_misc.inl | |
| scePadInfoComb | nop | 0 | ps2_stubs_misc.inl | |
| scePadInfoMode | nop | 0 | ps2_stubs_misc.inl | |
| scePadReqIntToStr | nop | 0 | ps2_stubs_misc.inl | |
| scePadStateIntToStr | nop | 0 | ps2_stubs_misc.inl | |
| scePadSetVrefParam | nop | 0 | ps2_stubs_misc.inl | |
| scePadSetWarningLevel | nop | 0 | ps2_stubs_misc.inl | |
| Pad_init | nop | 0 | ps2_stubs_misc.inl | Game-specific variant |
| Pad_set | nop | 0 | ps2_stubs_misc.inl | Game-specific variant |

### Pad SPI Wire Protocol (DualShock)
> Source: padspecs.txt — low-level SPI command/response format for scePadRead implementation.

**Device Modes** (modeId high nibble):

| Hi-nibble | Mode | Name |
|-----------|------|------|
| 4 | `0x41` | Digital (standard) |
| 7 | `0x73`/`0x79` | Analog (DualShock) |
| F | `0xF3` | Config mode |

**Key SPI Commands** (sent byte 2):

| Cmd | Name | Purpose |
|-----|------|---------|
| `0x42` | READ_DATA | Read button/stick state (main polling command) |
| `0x43` | ENTER/EXIT_CONFIG | `0x01`=enter config, `0x00`=exit |
| `0x44` | SET_MAIN_MODE | Set digital/analog mode + lock |
| `0x45` | QUERY_MODEL | Returns model (01=DS1, 03=DS2), mode count |
| `0x46` | QUERY_ACT | Query actuator (motor) capabilities |
| `0x4D` | SET_ACT_ALIGN | Enable vibration motors |
| `0x4F` | SET_BUTTON_INFO | Configure pressure-sensitive button mask |

**READ_DATA (0x42) response format**:
- Byte 0–1: `0xFF` + modeId
- Byte 2: `0x5A` (constant)
- Byte 3–4: Button status (1=released, 0=pressed)
- Byte 5–8: Analog sticks (analog mode only)
- Byte 9–20: Pressure values (pressure mode, 12 buttons)

---

## Font (`libfont`)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceeFontInit | nop | 0 | ps2_stubs_misc.inl | |
| sceeFontClose | nop | 0 | ps2_stubs_misc.inl | |
| sceeFontLoadFont | nop | 0 | ps2_stubs_misc.inl | |
| sceeFontSetFont | nop | 0 | ps2_stubs_misc.inl | |
| sceeFontSetScale | nop | 0 | ps2_stubs_misc.inl | |
| sceeFontSetColour | nop | 0 | ps2_stubs_misc.inl | |
| sceeFontSetMode | nop | 0 | ps2_stubs_misc.inl | |
| sceeFontPrintfAt | nop | 0 | ps2_stubs_misc.inl | |
| sceeFontPrintfAt2 | nop | 0 | ps2_stubs_misc.inl | |
| sceeFontGenerateString | nop | 0 | ps2_stubs_misc.inl | |

---

## IPU (`libipu`)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceIpuInit | nop | 0 | ps2_stubs_misc.inl | |
| sceIpuSync | nop | 0 | ps2_stubs_misc.inl | |
| sceIpuStopDMA | nop | 0 | ps2_stubs_misc.inl | |
| sceIpuRestartDMA | nop | 0 | ps2_stubs_misc.inl | |

---

## TTY / Debug

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceTtyInit | nop | 0 | ps2_stubs_misc.inl | |
| sceTtyHandler | nop | 0 | ps2_stubs_misc.inl | |
| sceTtyRead | nop | 0 | ps2_stubs_misc.inl | |
| sceTtyWrite | nop | 0 | ps2_stubs_misc.inl | |
| sceResetttyinit | nop | 0 | ps2_stubs_misc.inl | |
| sceDeci2Open | nop | 0 | ps2_stubs_misc.inl | DECI2 debug protocol |
| sceDeci2Close | nop | 0 | ps2_stubs_misc.inl | |
| sceDeci2Poll | nop | 0 | ps2_stubs_misc.inl | |
| sceDeci2ReqSend | nop | 0 | ps2_stubs_misc.inl | |
| sceDeci2ExLock | nop | 0 | ps2_stubs_misc.inl | |
| sceDeci2ExUnLock | nop | 0 | ps2_stubs_misc.inl | |
| sceDeci2ExRecv | nop | 0 | ps2_stubs_misc.inl | |
| sceDeci2ExReqSend | nop | 0 | ps2_stubs_misc.inl | |
| sceDeci2ExSend | nop | 0 | ps2_stubs_misc.inl | |
| scePrintf | impl | — | ps2_stubs_misc.inl | Debug print |

---

## POSIX / File I/O

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceOpen | nop | 0 | ps2_stubs_misc.inl | |
| sceClose | nop | 0 | ps2_stubs_misc.inl | |
| sceRead | nop | 0 | ps2_stubs_misc.inl | |
| sceWrite | nop | 0 | ps2_stubs_misc.inl | |
| sceLseek | nop | 0 | ps2_stubs_misc.inl | |
| sceIoctl | nop | 0 | ps2_stubs_misc.inl | |
| open | nop | 0 | ps2_stubs_misc.inl | POSIX open |
| close | nop | 0 | ps2_stubs_misc.inl | |
| read | nop | 0 | ps2_stubs_misc.inl | |
| write | nop | 0 | ps2_stubs_misc.inl | |
| lseek | nop | 0 | ps2_stubs_misc.inl | |
| stat | nop | 0 | ps2_stubs_misc.inl | |
| fstat | nop | 0 | ps2_stubs_misc.inl | |
| getpid | nop | 1 | ps2_stubs_misc.inl | |

---

## Additional libc / Misc

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| abs | impl | v0 | ps2_stubs_misc.inl | Integer absolute value |
| atan | impl | f0 | ps2_stubs_misc.inl | |
| rand | impl | v0 | ps2_stubs_misc.inl | |
| srand | impl | — | ps2_stubs_misc.inl | |
| pow | impl | f0 | ps2_stubs_misc.inl | |
| exit | impl | noreturn | ps2_stubs_misc.inl | Calls std::exit |
| memchr | impl | v0=ptr | ps2_stubs_misc.inl | |
| memcmp | impl | v0=cmp | ps2_stubs_misc.inl | |
| strcasecmp | impl | v0=cmp | ps2_stubs_misc.inl | Case-insensitive compare |
| vfprintf | impl | v0=chars | ps2_stubs_misc.inl | |
| vsprintf | impl | v0=chars | ps2_stubs_misc.inl | |
| iopGetArea | nop | 0 | ps2_stubs_misc.inl | |
| sceSetBrokenLink | nop | 0 | ps2_stubs_misc.inl | |
| sceSetPtm | nop | 0 | ps2_stubs_misc.inl | |
| sceFsInit | nop | 0 | ps2_stubs_misc.inl | |
| sceFsReset | nop | 0 | ps2_stubs_misc.inl | |

---

## Generic Stubs (reusable)

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| ret0 | nop | 0 | ps2_stubs_misc.inl | Generic return-0 stub |
| ret1 | nop | 1 | ps2_stubs_misc.inl | Generic return-1 stub |
| reta0 | nop | a0 | ps2_stubs_misc.inl | Generic return-first-arg stub |

---

## Newlib / Reentrant Wrappers

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| malloc_r | impl | addr | ps2_stubs_misc.inl | Reentrant wrapper for malloc |
| free_r | impl | — | ps2_stubs_misc.inl | |
| calloc_r | impl | addr | ps2_stubs_misc.inl | |
| malloc_trim_r | nop | 0 | ps2_stubs_misc.inl | |
| mbtowc_r | impl | v0 | ps2_stubs_misc.inl | Multi-byte to wide char |
| printf_r | impl | v0=chars | ps2_stubs_misc.inl | |

---

## CD/DVD Extended

| Function | Status | Return | File | Notes |
|----------|--------|--------|------|-------|
| sceCdStSeekF | nop | 1 | ps2_stubs_misc.inl | |
| sceCdStStart | nop | 1 | ps2_stubs_misc.inl | |
| sceCdStStat | nop | 0 | ps2_stubs_misc.inl | |
| sceCdStStop | nop | 1 | ps2_stubs_misc.inl | |
| sceCdSyncS | nop | 0 | ps2_stubs_misc.inl | |
| sceCdTrayReq | nop | 1 | ps2_stubs_misc.inl | |

---

When a game calls an unimplemented SDK function:
1. **Check this table first** — the function may already be stubbed
2. **If not listed**, add a new stub in `ps2_stubs_misc.inl` using the pattern:
   ```cpp
   void funcName(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
   {
       setReturnS32(ctx, 0); // or appropriate default
   }
   ```
3. **Register it** in `ps2_stubs.cpp` dispatch table under the correct game section
4. **For game-specific stubs**, create a new `ps2_stubs_<game>.inl` file

---

## PS2 Controller (PAD) Protocol Specs

> Source: [RO]man / Florin Sasu (v1.0, 2004). Low-level SPI protocol for DualShock/DualShock2 controllers.

### Device Modes (modeId)

| Hi | Size | Lo | ModeId | Device |
|----|------|----|--------|--------|
| 4 | 5 | 1 | 0x41 | STANDARD (DIGITAL) |
| 7 | 9,21 | 3/9 | 0x73/0x79 | ANALOG (DualShock) |
| 1 | 7 | 2 | 0x12 | MOUSE |
| 2 | 9 | 3 | 0x23 | NEGI-CON |
| 5 | 9 | 3 | 0x53 | JOY STICK |
| 3 | 5 | 1 | 0x31 | KONAMI-GUN |
| 6 | 9 | 3 | 0x63 | NAMCO-GUN |
| E | 9,13 | 3/5 | 0xE3/0xE5 | JOGCON |
| F | 9 | 3 | 0xF3 | CONFIG |
| 8 | — | — | 0x80 | MULTITAP |

### Key SPI Commands

| Cmd | Name | Description |
|-----|------|-------------|
| 0x40 | SET_VREF_PARAM | Set reference parameters (indexed) |
| 0x41 | QUERY_BUTTON_MASK | Get button capability mask (4 bytes) |
| 0x42 | READ_DATA | Read button + analog state |
| 0x43 | ENTER/EXIT_CONFIG | 0x00=exit config, 0x01=enter config mode |
| 0x44 | SET_MAIN_MODE | Set digital/analog mode + lock |
| 0x45 | QUERY_MODEL | Query controller model/mode info |
| 0x46 | QUERY_ACT | Query actuator (vibration motor) specs |
| 0x47 | QUERY_COMB | Query actuator combination info |
| 0x4C | QUERY_MODE | Query available modes |
| 0x4D | SET_ACT_ALIGN | Set vibration motor alignment (enable rumble) |
| 0x4F | SET_BUTTON_INFO | Set pressure-sensitive button mask |

### Button Status Bits (cmd 0x42 response bytes 3–4)

Bits are **active-low** (1=released, 0=pressed):

| Byte 3 | Bit | Button | Byte 4 | Bit | Button |
|--------|-----|--------|--------|-----|--------|
| 3 | 0 | SELECT | 4 | 0 | L2 |
| 3 | 1 | L3/LSTICK | 4 | 1 | R2 |
| 3 | 2 | R3/RSTICK | 4 | 2 | L1 |
| 3 | 3 | START | 4 | 3 | R1 |
| 3 | 4 | UP | 4 | 4 | TRIANGLE |
| 3 | 5 | RIGHT | 4 | 5 | CIRCLE |
| 3 | 6 | DOWN | 4 | 6 | CROSS |
| 3 | 7 | LEFT | 4 | 7 | SQUARE |

### QUERY_MODEL Response (cmd 0x45)

```
Byte 3: model  — 0x01=DualShock, 0x03=DualShock2
Byte 4: noOfModes — 0x02
Byte 5: modeCurOffs — 0x00=digital, 0x01=analog
Byte 6: noOfAct — 0x02 (DualShock), 0x01 (Joystick)
Byte 7: noOfComb — 0x01
```

### Actuator Specs (cmd 0x46)

```
Idx 0 (small motor): function=1, subfunction=1, length=1, current=100mA
Idx 1 (big motor):   function=1, subfunction=1, length=1, current=200mA
```

### SET_ACT_ALIGN (cmd 0x4D)
`01,4D,00,00,01,FF,FF,FF,FF` — Maps act[0]=small motor (on/off), act[1]=big motor (variable speed).

