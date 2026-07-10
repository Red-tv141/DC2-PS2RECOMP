# PS2 EE Syscall Database
> Machine-readable reference for the PS2Recomp agent — maps EE kernel syscall numbers to runtime implementations and stub safety.
> Ground truth: **ps2tek** (official PS2 hardware docs), cross-referenced with `ps2_syscalls.cpp` source.
>
> **See also**: `db-sdk-functions.md` (SDK library stubs that wrap these syscalls), `db-ps2-architecture.md` §9 (exception vectors + boot flow), `db-memory-map.md` (kernel memory areas).

## Lookup Protocol
1. Find the syscall `$v1` number in the table below
2. Check **Runtime Status** — `impl` = fully implemented in `ps2_syscalls.cpp`, `stub-safe` = safe to return 0
3. Parameters follow the MIPS o32 calling convention: `$a0`–`$a3` → `ctx->regs[4..7]`
4. Negative syscall numbers (prefixed `-`) are interrupt-context variants (no thread reschedule)

## Syscall Table

> [!IMPORTANT]
> Numbers verified against ps2tek §BIOS EE Syscalls and PS2Recomp `ps2_syscalls.cpp` switch statement.

| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x01 | ResetEE | `a0`=flags | void | stub-safe | Resets DMAC/VU/VIF/GIF/IPU per flag bits — no-op in recomp |
| 0x02 | SetGsCrt | `a0`=interlace, `a1`=mode, `a2`=field | void | impl | Initializes PCRTC display mode |
| 0x03 | SetGsCrtKn | same as SetGsCrt | void | impl | Korean variant |
| 0x04 | Exit | `a0`=status | noreturn | impl | ps2tek: returns to OSDSYS. Recomp: calls `std::exit(0)` |
| 0x05 | _ExceptionEpilogue | — | void | stub-safe | Internal kernel use — returns to user from exception |
| 0x06 | LoadExecPS2 | `a0`=filename, `a1`=argc, `a2`=argv | noreturn | stub-safe | Loads ELF and executes — not needed in static recomp |
| 0x07 | ExecPS2 | `a0`=entry, `a1`=gp, `a2`=argc, `a3`=argv | noreturn | stub-safe | Clears kernel state, creates main thread at entry |
| 0x0B | AddSbusIntcHandler | `a0`=cause, `a1`=handler | `$v0`=id | stub-safe | IOP SBUS — not needed in recomp |
| 0x0C | RemoveSbusIntcHandler | `a0`=cause | `$v0`=status | stub-safe | |

### Interrupts (0x10–0x17)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x10 | AddIntcHandler | `a0`=cause, `a1`=handler, `a2`=next, `a3`=arg | `$v0`=handler_id | impl | Registers INTC interrupt handler |
| 0x11 | RemoveIntcHandler | `a0`=cause, `a1`=handler_id | `$v0`=status | impl | |
| 0x12 | AddDmacHandler | `a0`=channel, `a1`=handler, `a2`=next, `a3`=arg | `$v0`=handler_id | impl | Registers DMAC interrupt handler |
| 0x13 | RemoveDmacHandler | `a0`=channel, `a1`=handler_id | `$v0`=status | impl | |
| 0x14 | _EnableIntc | `a0`=cause_bit | `$v0`=was_enabled | impl | Enables INTC_MASK bit |
| 0x15 | _DisableIntc | `a0`=cause_bit | `$v0`=was_enabled | impl | |
| 0x16 | _EnableDmac | `a0`=cause_bit | `$v0`=was_enabled | impl | Enables D_STAT mask bit |
| 0x17 | _DisableDmac | `a0`=cause_bit | `$v0`=was_enabled | impl | |

### Alarms (0x18–0x19, patched 0xFC–0xFF)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x18 | SetAlarm | `a0`=ticks_lo, `a1`=ticks_hi, `a2`=handler, `a3`=common | `$v0`=alarm_id | impl | Timer alarm with chrono callback |
| 0x19 | ReleaseAlarm | `a0`=alarm_id | `$v0`=status | impl | Cancels alarm |
| -0x1E / 0xFC | iSetAlarm | same as SetAlarm | same | impl | Patched via SetSyscall — ps2tek §EE Patches |
| 0xFE | CancelAlarm | `a0`=alarm_id | `$v0`=status | impl | Alias for ReleaseAlarm (source: case 0xFE) |
| -0x1F / 0xFD | iReleaseAlarm | same as ReleaseAlarm | same | impl | |
| 0xFF | iCancelAlarm | same | same | impl | |

### Threading (0x20–0x3B)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x20 | CreateThread | `a0`=ThreadParam* | `$v0`=thread_id | impl | Allocates thread ID, stores entry/priority/stack |
| 0x21 | DeleteThread | `a0`=thread_id | `$v0`=status | impl | Thread must be DORMANT |
| 0x22 | StartThread | `a0`=thread_id, `a1`=args | `$v0`=status | impl | Sets DORMANT→READY, reschedules |
| 0x23 | ExitThread | — | void | impl | Current thread→DORMANT, reschedules |
| 0x24 | ExitDeleteThread | — | void | impl | Current thread deleted, reschedules |
| 0x25 | TerminateThread | `a0`=thread_id | `$v0`=status | impl | Forces thread→DORMANT |
| 0x26 / -0x26 | iTerminateThread | same | same | impl | No reschedule variant |
| 0x29 | ChangeThreadPriority | `a0`=tid, `a1`=priority | `$v0`=status | impl | tid=0 → current thread |
| 0x2A / -0x2A | iChangeThreadPriority | same | `$v0`=old_priority | impl | |
| 0x2B | RotateThreadReadyQueue | `a0`=priority | `$v0`=status | stub-safe | Rotates same-priority thread list |
| 0x2C / -0x2C | _iRotateThreadReadyQueue | same | same | stub-safe | |
| 0x2D | ReleaseWaitThread | `a0`=tid | `$v0`=status | impl | WAIT→READY or WAITSUSPEND→SUSPEND |
| 0x2E / -0x2E | iReleaseWaitThread | same | same | impl | |
| 0x2F | GetThreadId | — | `$v0`=tid | impl | Returns current thread ID |
| 0x30 | ReferThreadStatus | `a0`=tid, `a1`=ThreadParam* | `$v0`=status | impl | Fills status struct |
| 0x31 / -0x31 | iReferThreadStatus | same | same | impl | |
| 0x32 | SleepThread | — | `$v0`=status | impl | Cooperative yield — decrements wakeup_count or goes WAIT |
| 0x33 | WakeupThread | `a0`=tid | `$v0`=status | impl | WAIT→READY, reschedules |
| 0x34 / -0x34 | iWakeupThread | same | same | impl | |
| 0x35 | CancelWakeupThread | `a0`=tid | `$v0`=old_count | impl | Resets wakeup_count to 0 |
| 0x36 / -0x36 | iCancelWakeupThread | same | same | impl | |
| 0x37 | SuspendThread | `a0`=tid | `$v0`=status | impl | READY→SUSPEND, WAIT→WAITSUSPEND |
| 0x38 / -0x38 | iSuspendThread | same | same | impl | ⚠️ ps2tek BUG: no reschedule on current thread! |
| 0x39 | ResumeThread | `a0`=tid | `$v0`=status | impl | SUSPEND→READY, WAITSUSPEND→WAIT |
| 0x3A / -0x3A | iResumeThread | same | same | impl | |
| 0x3B | JoinThread | — | void | stub-safe | ps2tek: listed but undocumented |

### Memory / Heap (0x3C–0x3F)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x3C | InitMainThread | `a0`=gp, `a1`=stack, `a2`=stack_size, `a3`=args | `$v0`=stack_ptr | impl | ps2tek: stack=-1 → end of RDRAM |
| 0x3D | InitHeap | `a0`=heap, `a1`=heap_size | `$v0`=heap_end | impl | heap=-1 → starts at stack pointer |
| 0x3E | EndOfHeap | — | `$v0`=addr | impl | Returns heap base |
| 0x3F | GetHeapEnd | — | `$v0`=addr | impl | Alias for EndOfHeap |

### Semaphores (0x40–0x49)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x40 | CreateSema | `a0`=SemaParam* | `$v0`=sema_id | impl | Uses init_count, max_count |
| 0x41 | DeleteSema | `a0`=sema_id | `$v0`=status | impl | Releases waiting threads |
| 0x42 | SignalSema | `a0`=sema_id | `$v0`=status | impl | count++ or wakes waiter, reschedules |
| 0x43 / -0x43 | iSignalSema | same | `$v0`=status | impl | Returns -2 if thread released |
| 0x44 | WaitSema | `a0`=sema_id | `$v0`=status | impl | Blocking: count>0→count--, else WAIT |
| 0x45 | PollSema | `a0`=sema_id | `$v0`=status | impl | Non-blocking: count-- or -1 |
| 0x46 / -0x46 | iPollSema | same | same | impl | |
| 0x47 | ReferSemaStatus | `a0`=sema_id, `a1`=status_buf | `$v0`=status | impl | |
| 0x48 / -0x48 | iReferSemaStatus | same | same | impl | |
| -0x49 | iDeleteSema | `a0`=sema_id | `$v0`=status | impl | Interrupt-context DeleteSema |

### OSD Config Aliases (0x4A–0x4B)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x4A | SetOsdConfigParam | `a0`=config | void | impl | Alias — same handler as 0x7E |
| 0x4B | GetOsdConfigParam | `a0`=buf | void | impl | Alias — same handler as 0x79 |

### Event Flags (0x50–0x5A)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x50 | CreateEventFlag | `a0`=flag_desc_ptr | `$v0`=flag_id | impl | Creates event flag object |
| 0x51 | DeleteEventFlag | `a0`=flag_id | `$v0`=status | impl | |
| 0x52 | SetEventFlag | `a0`=flag_id, `a1`=bits | `$v0`=status | impl | Sets bit pattern |
| -0x53 | iSetEventFlag | same | same | impl | |
| 0x54 | ClearEventFlag | `a0`=flag_id, `a1`=bits | `$v0`=status | impl | Clears bit pattern (AND mask) |
| -0x55 | iClearEventFlag | same | same | impl | |
| 0x56 | WaitEventFlag | `a0`=flag_id, `a1`=bits, `a2`=mode, `a3`=result_ptr | `$v0`=status | impl | Blocking wait on bit pattern |
| 0x57 | PollEventFlag | same | same | impl | Non-blocking poll |
| -0x58 | iPollEventFlag | same | same | impl | |
| 0x59 | ReferEventFlagStatus | `a0`=flag_id, `a1`=status_buf | `$v0`=status | impl | |
| -0x5A | iReferEventFlagStatus | same | same | impl | |
| 0x5A | QueryBootMode | `a0`=param | `$v0`=value | impl | Returns boot mode parameter |

### Handler Enable/Disable (0x5B–0x5F)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x5B | GetThreadTLS | `a0`=tid | `$v0`=tls_ptr | impl | Returns thread-local storage pointer |
| 0x5C / -0x5C | EnableIntcHandler | `a0`=handler_id | `$v0`=status | impl | |
| 0x5D / -0x5D | DisableIntcHandler | `a0`=handler_id | `$v0`=status | impl | |
| 0x5E / -0x5E | EnableDmacHandler | `a0`=handler_id | `$v0`=status | impl | |
| 0x5F / -0x5F | DisableDmacHandler | `a0`=handler_id | `$v0`=status | impl | |

### Cache (0x64)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x64 | FlushCache | `a0`=mode | void | impl | 0=flush data, 1=invalidate data, 2=invalidate insn. No-op in recomp |

### GS Interface (0x70–0x73)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x70 / -0x70 | GsGetIMR | — | `$v0`=imr | impl | Returns GS_IMR |
| 0x71 / -0x71 | GsPutIMR | `a0`=imr_val | void | impl | Sets GS_IMR |
| 0x73 | SetVSyncFlag | `a0`=vsync_ptr, `a1`=csr_ptr | void | impl | Sets kernel vsync pointers — **NOT SetGsVParam** |

### SIF & System (0x74–0x85)
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x74 | SetSyscall | `a0`=index, `a1`=address | void | impl | ps2tek name. PS2Recomp: RegisterExitHandler. Replaces syscall table entry |
| 0x76 / -0x76 | SifDmaStat | `a0`=dma_id | `$v0`=status | impl | +value=queued, 0=in-progress, -value=complete |
| 0x77 / -0x77 | SifSetDma | `a0`=SifDmaTransfer*, `a1`=len | `$v0`=transfer_id | impl | Low-level SIF1 transfer |
| 0x78 / -0x78 | SifSetDChain | — | void | impl | Initializes SIF0 channel (CHCR=184h) |
| 0x79 | GetOsdConfigParam | `a0`=buf | void | impl | OSD config (language, screen, etc.) |
| 0x7A | GetOsdConfigParam2 | `a0`=buf | void | impl | Extended OSD config |
| 0x7B | ExecOSD | `a0`=argc, `a1`=argv | noreturn | stub-safe | Shorthand for LoadExecPS2("rom0:OSDSYS") |
| 0x7E | SetOsdConfigParam | `a0`=config | void | impl | |
| 0x7F | SetOsdConfigParam2 | `a0`=config | void | impl | ps2tek: MachineType/GetMemorySize nearby |
| 0x83 | FindAddress | `a0`=id | `$v0`=addr | impl | |
| 0x85 | SetMemoryMode | `a0`=mode | `$v0`=prev_mode | impl | |

### Misc / Extended
| # | Name | Params | Return | Runtime Status | Notes |
|---|------|--------|--------|---------------|-------|
| 0x100 | SetVTLBRefillHandler | `a0`=handler | void | stub-safe | TLB refill — not needed in recomp |
| 0xFC | _kprintf | `a0`=fmt_ptr | void | impl | Debug print from RDRAM |

## Quick Lookup by Category

### Threading (0x20–0x3B)
CreateThread(0x20) → DeleteThread(0x21) → StartThread(0x22) → ExitThread(0x23) → ExitDeleteThread(0x24) → TerminateThread(0x25/0x26) → ChangeThreadPriority(0x29/0x2A) → RotateThreadReadyQueue(0x2B/0x2C) → ReleaseWaitThread(0x2D/0x2E) → GetThreadId(0x2F) → ReferThreadStatus(0x30/0x31) → SleepThread(0x32) → WakeupThread(0x33/0x34) → CancelWakeupThread(0x35/0x36) → SuspendThread(0x37/0x38) → ResumeThread(0x39/0x3A) → JoinThread(0x3B) → GetThreadTLS(0x5B)

### Semaphores (0x40–0x49)
CreateSema(0x40) → DeleteSema(0x41/-0x49) → SignalSema(0x42/0x43) → WaitSema(0x44) → PollSema(0x45/0x46) → ReferSemaStatus(0x47/0x48)

### Event Flags (0x50–0x5A)
CreateEventFlag(0x50) → DeleteEventFlag(0x51) → SetEventFlag(0x52/-0x53) → ClearEventFlag(0x54/-0x55) → WaitEventFlag(0x56) → PollEventFlag(0x57/-0x58) → ReferEventFlagStatus(0x59/-0x5A)

### Interrupts (0x10–0x17, 0x5C–0x5F)
AddIntcHandler(0x10) → RemoveIntcHandler(0x11) → AddDmacHandler(0x12) → RemoveDmacHandler(0x13) → _EnableIntc(0x14) → _DisableIntc(0x15) → _EnableDmac(0x16) → _DisableDmac(0x17) → EnableIntcHandler(0x5C) → DisableIntcHandler(0x5D) → EnableDmacHandler(0x5E) → DisableDmacHandler(0x5F)

### GS Interface (0x70–0x73)
GsGetIMR(0x70) → GsPutIMR(0x71) → SetVSyncFlag(0x73) → SetGsCrt(0x02)

### SIF Interface (0x76–0x78)
SifDmaStat(0x76) → SifSetDma(0x77) → SifSetDChain(0x78)

### Memory / Cache (0x3C–0x3F, 0x64, 0x83–0x85)
InitMainThread(0x3C) → InitHeap(0x3D) → EndOfHeap(0x3E) → GetHeapEnd(0x3F) → FlushCache(0x64) → FindAddress(0x83) → SetMemoryMode(0x85)

### Alarms (0x18–0x19, 0xFC–0xFF)
SetAlarm(0x18) → iSetAlarm(0xFC/-0x1E) → ReleaseAlarm(0x19) → CancelAlarm(0xFE) → iReleaseAlarm(0xFD/-0x1F) → iCancelAlarm(0xFF)

### System / Boot
Exit(0x04) → LoadExecPS2(0x06) → ExecPS2(0x07) → ExecOSD(0x7B) → QueryBootMode(0x5A) → ResetEE(0x01) → SetSyscall(0x74) → GetOsdConfigParam(0x79/0x4B) → SetOsdConfigParam(0x7E/0x4A)

---

## Stub Development Patterns

### Triage Strategy for Unknown Functions
When reverse engineering stripped games, hundreds of `Warning: Unimplemented PS2 stub called` messages appear. Instead of real implementations immediately, use triage stubs:

```cpp
void ret0(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    setReturnU32(ctx, 0);
    ctx->pc = getRegU32(ctx, 31);
}
void ret1(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    setReturnU32(ctx, 1);
    ctx->pc = getRegU32(ctx, 31);
}
void reta0(uint8_t* /*rdram*/, R5900Context* ctx, PS2Runtime* /*rt*/) {
    setReturnU32(ctx, getRegU32(ctx, 4));
    ctx->pc = getRegU32(ctx, 31);
}
```

Try binding unknown functions to `ret0` or `ret1`. Does the game boot further? If yes, you bypassed a check. Identify what check it was later using the static export (`14-static-analysis-navigation.md`) if present, with live Ghidra only as fallback.

### Syscall Debugging Workflow
1. Identify the Syscall ID from runtime log (e.g., `Syscall 0x02 executing`)
2. Look up in the table above or ps2dev docs to find the name (e.g., `GsPutDrawEnv`)
3. Add a case statement in `ps2_syscalls.cpp` handler switch
4. Implement logic, reading args from `ctx.gpr[4]` (a0), `ctx.gpr[5]` (a1), ...
5. Return value goes in `ctx.gpr[2]` (v0)

### Writing Real Implementations
```cpp
void my_sceCdRead(uint8_t* rdram, R5900Context* ctx, PS2Runtime* /*rt*/) {
    uint32_t lsn = getRegU32(ctx, 4);        // a0: Logical Sector Number
    uint32_t sectors = getRegU32(ctx, 5);    // a1: Number of sectors
    uint32_t buffer_ptr = getRegU32(ctx, 6); // a2: Destination in EE RAM
    // Native C++ logic to read from PC filesystem
    // MyFileSystem::Read(lsn, sectors, rdram + (buffer_ptr & 0x01FFFFFF));
    setReturnU32(ctx, 1); // Return 1 (success)
    ctx->pc = getRegU32(ctx, 31);
}
```
