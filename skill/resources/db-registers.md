# PS2 EE Hardware Register Map
> Maps PS2 hardware register addresses to names, subsystems, and runtime handling.
> **Source**: EE User's Manual Version 6.0 (SCE Confidential), GS User's Manual
>
> **See also**: `db-memory-map.md` (address ranges these registers live in), `db-ps2-architecture.md` §3–§5 (DMA/GIF/GS subsystem context), `db-vu-instructions.md` (VU register usage).

## Lookup Protocol
1. Match the address from decompiled code or IO write trace
2. Check **Subsystem** to understand context
3. **Runtime Handling**: `impl` = has write handler in `ps2_memory.cpp`, `store-only` = stored but not acted on

## Address Ranges

| Range | Subsystem | Description |
|-------|-----------|-------------|
| `0x10000000–0x100000FF` | TIMER0 | Timer 0 registers |
| `0x10000800–0x100008FF` | TIMER1 | Timer 1 registers (same layout) |
| `0x10001000–0x100010FF` | TIMER2 | Timer 2 registers (no HOLD) |
| `0x10001800–0x100018FF` | TIMER3 | Timer 3 registers (no HOLD) |
| `0x10002000–0x10002030` | IPU | Image Processing Unit |
| `0x10003800–0x100039FF` | VIF0 | Vector Interface 0 |
| `0x10003C00–0x10003DFF` | VIF1 | Vector Interface 1 |
| `0x10008000–0x100080FF` | DMA Chan 0 | VIF0 DMA |
| `0x10009000–0x100090FF` | DMA Chan 1 | VIF1 DMA |
| `0x1000A000–0x1000A0FF` | DMA Chan 2 | GIF DMA |
| `0x1000B000–0x1000B0FF` | DMA Chan 3 | fromIPU DMA |
| `0x1000B400–0x1000B4FF` | DMA Chan 4 | toIPU DMA |
| `0x1000C000–0x1000C0FF` | DMA Chan 5 | SIF0 DMA |
| `0x1000C400–0x1000C4FF` | DMA Chan 6 | SIF1 DMA |
| `0x1000C800–0x1000C8FF` | DMA Chan 7 | SIF2 DMA |
| `0x1000D000–0x1000D0FF` | DMA Chan 8 | fromSPR DMA |
| `0x1000D400–0x1000D4FF` | DMA Chan 9 | toSPR DMA |
| `0x1000E000–0x1000E0FF` | DMAC | DMA controller global registers |
| `0x1000F000–0x1000F010` | INTC | Interrupt controller (STAT, MASK) |
| `0x1000F200–0x1000F260` | SIF | SIF control registers |
| `0x1000F520` | D_ENABLER | DMAC hold state (read) |
| `0x1000F590` | D_ENABLEW | DMAC hold control (write) |
| `0x10007010` | IPU_IN_FIFO | IPU input FIFO |
| `0x12000000–0x12002000` | GS Privileged | GS privileged registers (PMODE, CSR, etc.) |

---

## Timer Registers (Tn base addresses)

All 4 timers share the same register layout. Timer 0/1 have an additional HOLD register.

| Timer | Base Address | INTC bit |
|-------|-------------|----------|
| 0 | `0x10000000` | TIM0 (bit 4) |
| 1 | `0x10000800` | TIM1 (bit 5) |
| 2 | `0x10001000` | TIM2 (bit 6) |
| 3 | `0x10001800` | TIM3 (bit 7) |

| Offset | Name | R/W | Handling | Notes |
|--------|------|-----|----------|-------|
| `+0x00` | Tn_COUNT | RW | impl | Counter value (16-bit, upper bits fixed 0) — reads return 0 in runtime |
| `+0x10` | Tn_MODE | RW | store-only | Timer mode (clock source, gate, etc.) |
| `+0x20` | Tn_COMP | RW | store-only | Compare register (16-bit) |
| `+0x30` | Tn_HOLD | RW | store-only | Hold register (**Timer 0/1 only**) |

> Runtime: all Timer reads return 0 — timers are not emulated.
>
> **Note**: `ps2_memory.cpp` handles `0x10000200–0x10000300` as a separate fallback (returns 0 / accepts writes silently). This range falls in reserved space between Timer 0 and Timer 1 in the official address map — it's a game-specific access pattern, not the real Timer 1 base.

### Tn_MODE Bit-Fields

| Bit(s) | Name | Description |
|--------|------|-------------|
| 1:0 | CLKS | Clock selection: 00=BUSCLK (147.456MHz), 01=1/16 BUSCLK, 10=1/256 BUSCLK, 11=H-BLNK |
| 2 | GATE | Gate function enable (0=disabled, 1=enabled) |
| 3 | GATS | Gate selection: 0=H-BLNK (disabled when CLKS=11), 1=V-BLNK |
| 5:4 | GATM | Gate mode: 00=count while low, 01=reset+count on rising, 10=on falling, 11=on both edges |
| 6 | ZRET | Zero return: 0=keep counting past reference, 1=clear counter on match |
| 7 | CUE | Count up enable: 0=stop, 1=start/restart counting |
| 8 | CMPE | Compare-interrupt enable |
| 9 | OVFE | Overflow-interrupt enable |
| 10 | EQUF | Equal flag — set on compare-interrupt, **W1C** |
| 11 | OVFF | Overflow flag — set on overflow-interrupt, **W1C** |

---

## IPU Registers (0x10002000)

| Address | Name | R/W | Handling | Notes |
|---------|------|-----|----------|-------|
| `0x10002000` | IPU_CMD | RW | impl | Command register — reset on CTRL.RST |
| `0x10002010` | IPU_CTRL | RW | impl | Control — bit30 write triggers full IPU reset |
| `0x10002020` | IPU_BP | RW | impl | Bit position — reset on CTRL.RST |
| `0x10002030` | IPU_TOP | R | impl | Top of bitstream |
| `0x10007010` | IPU_IN_FIFO | W | store-only | Input FIFO write |

---

## GIF Registers (0x10003000)

| Address | Name | R/W | Notes |
|---------|------|-----|-------|
| `0x10003000` | GIF_CTRL | W | Control — bit0=RST (reset GIF), bit3=PSE (pause transfer) |
| `0x10003010` | GIF_MODE | W | Mode — bit0=M3R (mask PATH3), bit2=IMT (intermittent transfer) |
| `0x10003020` | GIF_STAT | R | Status — OPH, APTS, P3Q, P2Q, P1Q, IP3, PSE, FQC[4:0] |
| `0x10003040` | GIF_TAG0 | R | GIFtag save — NLOOP[14:0], EOP, id[1:0], PRE, PRIM |
| `0x10003050` | GIF_TAG1 | R | GIFtag save — TAG[63:32] (REGS field) |
| `0x10003060` | GIF_TAG2 | R | GIFtag save — TAG[95:64] (REGS continued) |
| `0x10003070` | GIF_TAG3 | R | GIFtag save — TAG[127:96] (REGS continued) |
| `0x10003080` | GIF_CNT | R | Transfer count — LOOPCNT[14:0], REGCNT[3:0] |
| `0x10003090` | GIF_P3CNT | R | PATH3 remaining transfer count |
| `0x100030A0` | GIF_P3TAG | R | PATH3 tag value (LOOPCNT, EOP) |

### GIF_STAT Bit-Fields

| Bit(s) | Name | Description |
|--------|------|-------------|
| 0 | M3R | Mask PATH3 (reflects GIF_MODE.M3R) |
| 1 | M3P | PATH3 masked by VIF1 (via MSKPATH3 VIFcode) |
| 2 | IMT | Intermittent transfer mode |
| 3 | PSE | Temporary pause (reflects GIF_CTRL.PSE) |
| 5 | IP3 | PATH3 interrupted (idle while M3R or M3P set) |
| 6 | P3Q | PATH3 request queued |
| 7 | P2Q | PATH2 request queued |
| 8 | P1Q | PATH1 request queued |
| 9 | OPH | Output path active (data being sent to GS) |
| 11:10 | APTS | Active PATH status: 00=idle, 01=PATH1, 10=PATH2, 11=PATH3 |
| 13:12 | DIR | Transfer direction: 00=idle |
| 28:24 | FQC | FIFO queue count (0–16 qwords in GIF FIFO) |

---

## VIF0 Registers (0x10003800)

| Address | Name | R/W | Handling | Notes |
|---------|------|-----|----------|-------|
| `0x10003800` | VIF0_STAT | R | impl | Status register — VPS, VEW, VIS, INT, ER0, ER1 |
| `0x10003810` | VIF0_FBRST | W | impl | Force break/reset — bit0=RST (reset VIF0), bit1=FBK (force break), bit2=STP (stop), bit3=STC (stall clear) |
| `0x10003820` | VIF0_ERR | W | impl | Error mask — bit0=MII (mask interrupt on INT), bit1=ME0 (mask DMAtag mismatch), bit2=ME1 (mask VIFcode mismatch) |
| `0x10003830` | VIF0_MARK | RW | impl | Mark register, clears MRK flag on write |
| `0x10003840` | VIF0_CYCLE | RW | impl | Cycle register (CL/WL) for UNPACK |
| `0x10003850` | VIF0_MODE | RW | impl | Mode (0–3): 0=normal, 1=offset, 2=difference |
| `0x10003860` | VIF0_NUM | RW | impl | Number of data remaining to process |
| `0x10003870` | VIF0_MASK | RW | impl | Write mask for UNPACK data |
| `0x10003880` | VIF0_CODE | RW | impl | Current VIFcode being executed |
| `0x10003890` | VIF0_ITOPS | RW | impl | Interrupt TOP register (set value) |
| `0x100038A0` | VIF0_R0 | RW | impl | Column/row register R0 (filling mask) |
| `0x100038B0` | VIF0_R1 | RW | impl | Column/row register R1 |
| `0x100038C0` | VIF0_R2 | RW | impl | Column/row register R2 |
| `0x100038D0` | VIF0_R3 | RW | impl | Column/row register R3 |
| `0x10003900` | VIF0_C0 | R | impl | Column register C0 (read-only) |
| `0x10003910` | VIF0_C1 | R | impl | Column register C1 |
| `0x10003920` | VIF0_C2 | R | impl | Column register C2 |
| `0x10003930` | VIF0_C3 | R | impl | Column register C3 |
| `0x10003940` | VIF0_ITOP | R | impl | Interrupt TOP actual value |

> **Note**: VIF0 does NOT have BASE/OFST/TOPS/TOP registers — double-buffering is only available on VIF1. VIF0 feeds VU0 (macro mode) data via DMA channel 0.
> R0–R3 are the filling registers used in STROW VIFcode. C0–C3 are the column registers used in STCOL VIFcode.

---

## VIF1 Registers (0x10003C00)

| Address | Name | R/W | Handling | Notes |
|---------|------|-----|----------|-------|
| `0x10003C00` | VIF1_STAT | R | impl | Status register — read in sync loops |
| `0x10003C10` | VIF1_FBRST | W | impl | Force break/reset — bit0=RST, bit3=STC |
| `0x10003C20` | VIF1_ERR | W | impl | Error mask — bit0=MII, bit1=ME0, bit2=ME1 |
| `0x10003C30` | VIF1_MARK | RW | impl | Mark register, clears MRK flag on write |
| `0x10003C40` | VIF1_CYCLE | RW | impl | Cycle register (CL/WL) |
| `0x10003C50` | VIF1_MODE | RW | impl | Mode (0–3) |
| `0x10003C60` | VIF1_NUM | RW | impl | Number of data to process |
| `0x10003C70` | VIF1_MASK | RW | impl | Write mask |
| `0x10003C80` | VIF1_CODE | RW | impl | Current VIFcode |
| `0x10003C90` | VIF1_ITOPS | RW | impl | Interrupt TOP register |
| `0x10003CA0` | VIF1_BASE | RW | impl | Double-buffer base |
| `0x10003CB0` | VIF1_OFST | RW | impl | Double-buffer offset |
| `0x10003CC0` | VIF1_TOPS | RW | impl | TOP register |
| `0x10003CD0` | VIF1_ITOP | RW | impl | Interrupt TOP actual |
| `0x10003CE0` | VIF1_TOP | RW | impl | TOP actual |
| `0x10003D00` | VIF1_R0 | RW | impl | Row register R0 (filling mask) |
| `0x10003D10` | VIF1_R1 | RW | impl | Row register R1 |
| `0x10003D20` | VIF1_R2 | RW | impl | Row register R2 |
| `0x10003D30` | VIF1_R3 | RW | impl | Row register R3 |
| `0x10003D40` | VIF1_C0 | R | impl | Column register C0 (read-only) |
| `0x10003D50` | VIF1_C1 | R | impl | Column register C1 |
| `0x10003D60` | VIF1_C2 | R | impl | Column register C2 |
| `0x10003D70` | VIF1_C3 | R | impl | Column register C3 |

---

## DMA Channel Layout (per-channel, base+offset)

Each DMA channel at base address `0x10008000 + N*0x1000` (or `0x1000B400` etc.):

| Offset | Name | Description |
|--------|------|-------------|
| `+0x00` | Dn_CHCR | Channel control (bit8=STR start) |
| `+0x10` | Dn_MADR | Memory address |
| `+0x20` | Dn_QWC | Quadword count |
| `+0x30` | Dn_TADR | Tag address (chain mode) |
| `+0x40` | Dn_ASR0 | Address stack 0 (call/ret chains) |
| `+0x50` | Dn_ASR1 | Address stack 1 |
| `+0x80` | Dn_SADR | SPR address (**channels 8/9 only** — fromSPR/toSPR) |

**DMA Channel Base Addresses:**

| Channel | Base | Purpose |
|---------|------|---------|
| 0 | `0x10008000` | VIF0 |
| 1 | `0x10009000` | VIF1 |
| 2 | `0x1000A000` | GIF |
| 3 | `0x1000B000` | fromIPU |
| 4 | `0x1000B400` | toIPU |
| 5 | `0x1000C000` | SIF0 (IOP→EE) |
| 6 | `0x1000C400` | SIF1 (EE→IOP) |
| 7 | `0x1000C800` | SIF2 |
| 8 | `0x1000D000` | fromSPR |
| 9 | `0x1000D400` | toSPR |

---

## DMAC Global Registers (0x1000E000)

| Address | Name | R/W | Handling | Notes |
|---------|------|-----|----------|-------|
| `0x1000E000` | D_CTRL | RW | impl | DMAC master enable (bit0) |
| `0x1000E010` | D_STAT | RW | impl | Status — W1C status + toggle mask (see bit-fields) |
| `0x1000E020` | D_PCR | RW | store-only | Priority control |
| `0x1000E030` | D_SQWC | RW | store-only | Interleave size (SQWC skip + TQWC transfer) |
| `0x1000E040` | D_RBSR | RW | store-only | Ring buffer size mask |
| `0x1000E050` | D_RBOR | RW | store-only | Ring buffer offset (aligned to RBSR) |
| `0x1000E060` | D_STADR | RW | store-only | Stall address (source channel MADR copy) |
| `0x1000F520` | D_ENABLER | R | impl | DMAC hold state (read-only mirror of D_ENABLEW) |
| `0x1000F590` | D_ENABLEW | W | impl | DMAC hold control — bit16 CPND: 1=suspend, 0=restart |

### D_CTRL Bit-Fields

| Bit(s) | Name | Description |
|--------|------|-------------|
| 0 | DMAE | DMA enable: 0=disable all DMAs, 1=enable all DMAs |
| 1 | RELE | Release signal (cycle stealing): 0=off, 1=on |
| 3:2 | MFD | Memory FIFO drain: 00=none, 01=reserved, 10=VIF1(ch1), 11=GIF(ch2) |
| 5:4 | STS | Stall source: 00=none, 01=SIF0(ch5), 10=fromSPR(ch8), 11=fromIPU(ch3) |
| 7:6 | STD | Stall drain: 00=none, 01=VIF1(ch1), 10=GIF(ch2), 11=SIF1(ch6) |
| 10:8 | RCYC | Release cycle: 000=8, 001=16, 010=32, 011=64, 100=128, 101=256 |

### D_STAT Bit-Fields

| Bit(s) | Name | Description |
|--------|------|-------------|
| 9:0 | CIS0–CIS9 | Channel interrupt status (per-channel), **W1C** |
| 13 | SIS | Stall interrupt status, **W1C** |
| 14 | MEIS | MFIFO empty interrupt status, **W1C** |
| 15 | BEIS | BUSERR interrupt status, **W1C** |
| 25:16 | CIM0–CIM9 | Channel interrupt mask, **toggle on W1** |
| 29 | SIM | Stall interrupt mask, **toggle on W1** |
| 30 | MEIM | MFIFO empty interrupt mask, **toggle on W1** |

> INT1 = (CIS0&&CIM0) || ... || (CIS9&&CIM9) || (SIS&&SIM) || (MEIS&&MEIM) || BEIS

---

## GS Privileged Registers (0x12000000)

| Address | Offset | Name | R/W | Notes |
|---------|--------|------|-----|-------|
| `0x12000000` | 0x0000 | PMODE | RW | CRT mode control |
| `0x12000010` | 0x0010 | SMODE1 | RW | Sync mode 1 (H/V sync parameters) |
| `0x12000020` | 0x0020 | SMODE2 | RW | Sync mode 2 (interlace/frame mode) |
| `0x12000030` | 0x0030 | SRFSH | RW | DRAM refresh rate |
| `0x12000040` | 0x0040 | SYNCH1 | RW | H-sync timing 1 (front/back porch, horizontal sync width) |
| `0x12000050` | 0x0050 | SYNCH2 | RW | H-sync timing 2 (additional horizontal sync parameters) |
| `0x12000060` | 0x0060 | SYNCV | RW | V-sync timing (vertical blanking, front/back porch) |
| `0x12000070` | 0x0070 | DISPFB1 | RW | Display framebuffer 1 |
| `0x12000080` | 0x0080 | DISPLAY1 | RW | Display output 1 |
| `0x12000090` | 0x0090 | DISPFB2 | RW | Display framebuffer 2 |
| `0x120000A0` | 0x00A0 | DISPLAY2 | RW | Display output 2 |
| `0x120000B0` | 0x00B0 | EXTBUF | RW | External buffer (feedback write) |
| `0x120000C0` | 0x00C0 | EXTDATA | RW | External data (feedback area) |
| `0x120000D0` | 0x00D0 | EXTWRITE | RW | External write trigger |
| `0x120000E0` | 0x00E0 | BGCOLOR | RW | Background color |
| `0x12001000` | 0x1000 | CSR | RW | System status — bits0–1 W1C |
| `0x12001010` | 0x1010 | IMR | RW | Interrupt mask |
| `0x12001040` | 0x1040 | BUSDIR | RW | Bus direction |
| `0x12001080` | 0x1080 | SIGLBLID | RW | Signal/label ID |

---

## SIF Registers (0x1000F200)

| Address | Name | R/W | Handling | Notes |
|---------|------|-----|----------|-------|
| `0x1000F200` | SIF_MSCOM | RW | store-only | SIF main→sub communication |
| `0x1000F210` | SIF_SMCOM | RW | store-only | SIF sub→main communication |
| `0x1000F220` | SIF_MSFLG | RW | store-only | Main→sub flag |
| `0x1000F230` | SIF_SMFLG | R | impl | Sub→main flag — **reads return `0x60000`** |
| `0x1000F240` | SIF_CTRL | R | impl | Control — **reads return `0xF0000002`** |
| `0x1000F260` | SIF_BD6 | RW | store-only | Unknown |

> Runtime: `SIF_SMFLG` returns `0x60000` and `SIF_CTRL` returns `0xF0000002` — hardcoded to satisfy initialization checks.

---

## INTC Registers (0x1000F000)

| Address | Name | R/W | Handling | Notes |
|---------|------|-----|----------|-------|
| `0x1000F000` | I_STAT | RW | store-only | Interrupt status (W1C) |
| `0x1000F010` | I_MASK | RW | store-only | Interrupt mask (toggle on W1) |

### I_STAT / I_MASK Bit-Fields

| Bit | Name | Interrupt Source |
|-----|------|-----------------|
| 0 | GS | Graphics Synthesizer |
| 1 | SBUS | SBus (IOP) |
| 2 | VBON | V-Blank start (begin of blanking) |
| 3 | VBOF | V-Blank end (end of blanking) |
| 4 | TIM0 | Timer 0 |
| 5 | TIM1 | Timer 1 |
| 6 | TIM2 | Timer 2 |
| 7 | TIM3 | Timer 3 |
| 8 | SFIFO | SFIFO |
| 9 | VU0WD | VU0 Watchdog |
| 10 | VIF0 | VIF0 |
| 11 | VIF1 | VIF1 |
| 12 | VU0 | VU0 |
| 13 | VU1 | VU1 |
| 14 | IPU | IPU |

> INT0 = OR of (I_STAT[n] AND I_MASK[n]) for all n

---

## DMA Chain Tag Format

```
Bits [15:0]  = QWC  (quadword count for this transfer)
Bits [28:26] = ID   (tag type: 0=REFE, 1=CNT, 2=NEXT, 3=REF, 5=CALL, 6=RET, 7=END)
Bit  [31]    = IRQ  (interrupt request)
Bits [62:32] = ADDR (31-bit address for REF/NEXT/CALL tags)
```

**Tag IDs:**
- `0` REFE — transfer data at ADDR, end chain
- `1` CNT — transfer data following tag, continue at end of data
- `2` NEXT — transfer data following tag, next tag at ADDR
- `3`/`4` REF — transfer data at ADDR, next tag follows this tag
- `5` CALL — like NEXT but pushes return address to ASR stack
- `6` RET — pops return address from ASR stack
- `7` END — transfer data following tag, end chain

---

## COP0 (System Control Coprocessor) Registers
> Source: EE Core User's Manual Version 6.0

### Register Map

| # | Name | Description | Purpose |
|---|------|-------------|---------|
| 0 | Index | TLB entry index for read/write | MMU |
| 1 | Random | Pseudo-random TLB index (range: Wired..47) | MMU |
| 2 | EntryLo0 | Low TLB entry (even PFN) — has S bit for SPR | MMU |
| 3 | EntryLo1 | Low TLB entry (odd PFN) | MMU |
| 4 | Context | Page table pointer + BadVPN2 | MMU |
| 5 | PageMask | TLB page size mask (4K–16M) | MMU |
| 6 | Wired | Number of wired TLB entries | MMU |
| 7 | — | Reserved | — |
| 8 | BadVAddr | Virtual address that caused exception | Exception |
| 9 | Count | Timer counter (increments every CPU clock) | Exception |
| 10 | EntryHi | VPN2 + ASID for TLB | MMU |
| 11 | Compare | Timer compare value (IP[7] set when Count=Compare) | Exception |
| 12 | **Status** | Processor status / interrupt control | Exception |
| 13 | **Cause** | Exception cause / pending interrupts | Exception |
| 14 | EPC | Exception Program Counter | Exception |
| 15 | PRId | Processor ID (Imp=0x2E for EE Core) | MMU |
| 16 | **Config** | Cache/pipeline configuration | MMU |
| 17–22 | — | Reserved | — |
| 23 | BadPAddr | Physical address that caused bus error | Exception |
| 24 | Debug | Debug registers (7 sub-registers via MFC0/MTC0) | Debug |
| 25 | Perf | Performance counter + control (3 sub-registers) | Exception |
| 26–27 | — | Reserved | — |
| 28 | TagLo | Cache tag low bits | Cache |
| 29 | TagHi | Cache tag high bits | Cache |
| 30 | ErrorEPC | Error exception program counter | Exception |
| 31 | — | Reserved | — |

### Status Register — CPR[0,12]

| Bits | Name | Description |
|------|------|-------------|
| 31:28 | CU[3:0] | Coprocessor usability (0=unusable, 1=usable). COP0 always usable in Kernel. COP3 always unusable |
| 23 | DEV | Debug/PerfC vector select (1=bootstrap @0xBFC00280/300) |
| 22 | BEV | TLB Refill/General vector select (1=bootstrap @0xBFC00200/380) |
| 18 | CH | Cache Hit status of last CACHE instruction (0=miss, 1=hit) |
| 17 | EDI | EI/DI instruction enable (0=Kernel only, 1=all modes) |
| 16 | EIE | Enable IE bit (0=all interrupts disabled regardless of IE) |
| 15 | IM[7] | Interrupt mask: internal timer (Count/Compare) |
| 11:10 | IM[3:2] | Interrupt mask: Int[1:0] signals |
| 12 | BEM | Bus Error Mask (0=report, 1=suppress) |
| 4:3 | KSU | Mode (00=Kernel, 01=Supervisor, 10=User) |
| 2 | ERL | Error Level (set on Reset/NMI/PerfC/Debug) — init: **1** |
| 1 | EXL | Exception Level (set on all other exceptions) |
| 0 | IE | Interrupt Enable (both IE=1 AND EIE=1 needed, plus ERL=EXL=0) |

### Cause Register — CPR[0,13]

| Bits | Name | Description |
|------|------|-------------|
| 31 | BD | Branch Delay — exception occurred in branch delay slot (L1) |
| 30 | BD2 | Branch Delay 2 — NMI/PerfC/Debug in delay slot (L2) |
| 29:28 | CE | Coprocessor number for CpU exception |
| 18:16 | EXC2 | Level 2 exception code (0=Reset, 1=NMI, 2=PerfC, 3=Debug) |
| 15 | IP[7] | Timer interrupt pending |
| 11 | IP[3] | Int[0] interrupt pending |
| 10 | IP[2] | Int[1] interrupt pending |
| 6:2 | ExcCode | Level 1 exception code (see table below) |

### ExcCode Values

| Code | Name | Description |
|------|------|-------------|
| 0 | Int | Interrupt |
| 1 | Mod | TLB Modified |
| 2 | TLBL | TLB Refill (fetch/load) |
| 3 | TLBS | TLB Refill (store) |
| 4 | AdEL | Address Error (fetch/load) |
| 5 | AdES | Address Error (store) |
| 6 | IBE | Bus Error (instruction) |
| 7 | DBE | Bus Error (data) |
| 8 | Sys | System Call |
| 9 | Bp | Breakpoint |
| 10 | RI | Reserved Instruction |
| 11 | CpU | Coprocessor Unusable |
| 12 | Ov | Overflow |
| 13 | Tr | Trap |

### Config Register — CPR[0,16]

| Bits | Name | Description | Init |
|------|------|-------------|------|
| 30:28 | EC | Bus clock ratio (000 = CPU/2) | 0 |
| 18 | DIE | Dual-issue enable (1=parallel pipeline) | 0 |
| 17 | ICE | Instruction cache enable | 0 |
| 16 | DCE | Data cache enable | 0 |
| 13 | NBE | Non-blocking load enable (hit-under-miss) | 0 |
| 12 | BPE | Branch prediction enable | 0 |
| 11:9 | IC | I-cache size (010=16KB) | 010 |
| 8:6 | DC | D-cache size (001=8KB) | 001 |
| 2:0 | K0 | kseg0 cache mode (010=uncached, 011=cached WB, 111=uncached accel) | Undef |

### Exception Vectors

| Vector | Condition | Normal Address | Bootstrap Address |
|--------|-----------|---------------|-------------------|
| V_RESET_NMI | Reset / NMI | 0xBFC00000 | 0xBFC00000 |
| V_TLB_REFILL | TLB Refill (EXL=0) | 0x80000000 | 0xBFC00200 |
| V_COUNTER | Performance Counter | 0x80000080 | 0xBFC00280 |
| V_DEBUG | Debug | 0x80000100 | 0xBFC00300 |
| V_COMMON | All other L1 exceptions | 0x80000180 | 0xBFC00380 |
| V_INTERRUPT | Interrupt | 0x80000200 | 0xBFC00400 |

> Bootstrap addresses used when BEV=1 (TLB Refill/Common/Interrupt) or DEV=1 (Counter/Debug).

### TLB EntryLo0/Lo1 Cache Modes

| C bits | Mode |
|--------|------|
| 010 (2) | Uncached |
| 011 (3) | Cached, write-back with write allocate |
| 111 (7) | Uncached Accelerated |
| others | Reserved |

> EntryLo0 bit 31 (S) selects Scratchpad RAM when set to 1.

### Index Register — CPR[0,0]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31 | P | 1 = TLBP probe failure (no match) | r/- | Undef |
| 5:0 | Index | TLB entry index for TLBR/TLBWI (0–47) | r/w | Undef |

### Random Register — CPR[0,1]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 5:0 | Random | Pseudo-random TLB index (decrements Wired→47) | r/- | **47** |

> Random counts down each instruction. On wrap, resets to 47. Range: Wired ≤ Random ≤ 47.

### EntryLo0 / EntryLo1 — CPR[0,2] / CPR[0,3]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31 | S | **Scratchpad** — SPR access (EntryLo0 only, replaces physical page) | r/w | Undef |
| 25:6 | PFN | Page Frame Number (physical address bits 31:12) | r/w | Undef |
| 5:3 | C | Cache coherency (see table below) | r/w | Undef |
| 2 | D | Dirty — page writable when set | r/w | Undef |
| 1 | V | Valid — TLB entry usable when set | r/w | Undef |
| 0 | G | Global — when set in BOTH EntryLo0 AND EntryLo1, ignores ASID match | r/w | Undef |

### EntryHi — CPR[0,10]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31:13 | VPN2 | Virtual Page Number / 2 (maps 2 consecutive pages) | r/w | Undef |
| 7:0 | ASID | Address Space ID (8-bit process tag) | r/w | Undef |

### Context — CPR[0,4]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31:23 | PTEBase | Page Table Entry base address (software-defined) | r/w | Undef |
| 22:4 | BadVPN2 | VPN2 of most recent TLB exception (auto-set) | r/- | Undef |

### PageMask — CPR[0,5]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 24:13 | MASK | Page size mask | r/w | Undef |

| MASK value | Page size |
|-----------|-----------|
| 0x000 | 4 KB |
| 0x003 | 16 KB |
| 0x00F | 64 KB |
| 0x03F | 256 KB |
| 0x0FF | 1 MB |
| 0x3FF | 4 MB |
| 0xFFF | 16 MB |

### Wired — CPR[0,6]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 5:0 | Wired | Lower bound of Random register range. Entries 0..Wired-1 are never replaced by TLBWR | r/w | 0 |

### BadVAddr — CPR[0,8]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31:0 | BadVAddr | Virtual address that caused TLB/address exception | r/- | Undef |

### Count — CPR[0,9]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31:0 | Count | Free-running timer, increments every CPU clock cycle | r/w | Undef |

> When Count == Compare, IP[7] (timer interrupt) is set in Cause register.

### Compare — CPR[0,11]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31:0 | Compare | Timer reference. Writing clears IP[7] pending bit | r/w | Undef |

### EPC — CPR[0,14]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31:0 | EPC | Return address for Level 1 exceptions. Points to faulting instruction or preceding branch (if BD=1) | r/w | Undef |

> Not updated when Status.EXL = 1 (double exception).

### PRId — CPR[0,15]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 15:8 | Imp | Implementation ID = **0x2E** (EE Core) | r/- | 0x2E |
| 7:0 | Rev | Revision number (major[7:4].minor[3:0]) | r/- | chip-specific |

### BadPAddr — CPR[0,23]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31:4 | BadPAddr | Physical address of most recent bus error (only updated when Status.BEM=0) | r/w | Undef |

### ErrorEPC — CCR[0,30]

| Bits | Name | Description | R/W | Init |
|------|------|-------------|-----|------|
| 31:0 | ErrorEPC | Return address for Level 2 exceptions (Reset/NMI/PerfC/Debug). Points to faulting instruction or preceding branch (if Cause.BD2=1) | r/w | Undef |

### BPC (Breakpoint Control) — CCR[0,24]-0

| Bits | Name | Description | R/W |
|------|------|-------------|-----|
| 31 | IAE | Instruction Address breakpoint Enable | r/w |
| 30 | DRE | Data Read breakpoint Enable | r/w |
| 29 | DWE | Data Write breakpoint Enable | r/w |
| 28 | DVE | Data Value breakpoint Enable | r/w |
| 26 | IUE | Instr breakpoint — User mode Enable | r/w |
| 25 | ISE | Instr breakpoint — Supervisor mode Enable | r/w |
| 24 | IKE | Instr breakpoint — Kernel mode Enable | r/w |
| 23 | IXE | Instr breakpoint — EXL mode Enable | r/w |
| 21 | DUE | Data breakpoint — User mode Enable | r/w |
| 20 | DSE | Data breakpoint — Supervisor mode Enable | r/w |
| 19 | DKE | Data breakpoint — Kernel mode Enable | r/w |
| 18 | DXE | Data breakpoint — EXL mode Enable | r/w |
| 17 | ITE | Instr breakpoint — Trigger generation Enable | r/w |
| 16 | DTE | Data breakpoint — Trigger generation Enable | r/w |
| 15 | BED | Breakpoint Exception Disable | r/w |
| 2 | DWB | Data Write Breakpoint flag | r/w |
| 1 | DRB | Data Read Breakpoint flag | r/w |
| 0 | IAB | Instruction Address Breakpoint flag | r/w |

> Sub-registers IAB, IABM, DAB, DABM, DVB, DVBM accessed via MFIAB/MTIAB etc. (not MFC0/MTC0).

### PCCR (Performance Counter Control) — CCR[0,25]

| Bits | Name | Description | R/W |
|------|------|-------------|-----|
| 31 | CTE | Counter enable (0=disabled, 1=counting) | r/w |
| 19:15 | EVENT1 | Event selector for CTR1 (see table) | r/w |
| 14 | U1 | CTR1 counts in User mode | r/w |
| 13 | S1 | CTR1 counts in Supervisor mode | r/w |
| 12 | K1 | CTR1 counts in Kernel mode | r/w |
| 11 | EXL1 | CTR1 counts in L1 exception handler | r/w |
| 9:5 | EVENT0 | Event selector for CTR0 (see table) | r/w |
| 4 | U0 | CTR0 counts in User mode | r/w |
| 3 | S0 | CTR0 counts in Supervisor mode | r/w |
| 2 | K0 | CTR0 counts in Kernel mode | r/w |
| 1 | EXL0 | CTR0 counts in L1 exception handler | r/w |

### Performance Counter Events (EVENT0/EVENT1 field)

| Code | CTR0 Event | CTR1 Event |
|------|-----------|-----------|
| 0 | (reserved) | Low-order branch issues |
| 1 | Processor cycle | Processor cycle |
| 2 | Single instruction issue | Double instruction issue |
| 3 | Branch issue | Branch prediction miss |
| 4 | BTAC miss | TLB miss |
| 5 | TLB miss | DTLB miss |
| 6 | I-cache miss | D-cache miss |
| 7 | DTLB access | WBB single request unusable |
| 8 | Non-blocking load | WBB burst request unusable |
| 9 | WBB single request | WBB burst request almost full |
| 10 | WBB burst request | WBB burst request full |
| 11 | CPU address bus busy | CPU data bus busy |
| 12 | Instruction complete | Instruction complete |
| 13 | Non-BDS instruction complete | Non-BDS instruction complete |
| 14 | COP2 instruction complete | COP1 instruction complete |
| 15 | Load complete | Store complete |
| 16 | No event | No event |

### PCR0 / PCR1 (Performance Counters) — CCR[0,25]

| Bits | Name | Description | R/W |
|------|------|-------------|-----|
| 31 | OVFL | Overflow flag (triggers Debug exception when CTE=1) | r/w |
| 30:0 | VALUE | 31-bit counter value | r/w |

### TagLo — CCR[0,28] (Cache Tag)

| Bits | Name | Description |
|------|------|-------------|
| 31:12 | PTagLo | Physical address tag |
| 6 | D | Dirty bit |
| 5 | V | Valid bit |
| 4 | R | LRF (Least Recently Filled) bit |
| 3 | L | Lock bit |

> Used with CACHE instructions (DXLTG/DXSTG, IXLTG/IXSTG). TagHi[31:2] holds BTAC target address.

---

## GS (Graphics Synthesizer) Registers
> **Source**: GS User's Manual Version 6.0, Chapter 7 — Register Descriptions
>
> General-purpose registers are write-only, addressed via GIF packets.
> Privileged registers live at `0x12000000–0x12002000` and some support read access.

### Pixel Storage Formats (used across many GS registers)

| Value (bin) | Name | Bits/pixel | Description |
|-------------|------|-----------|-------------|
| 000000 | PSMCT32 | 32 | RGBA 8:8:8:8 |
| 000001 | PSMCT24 | 24 | RGB 8:8:8 (A unused) |
| 000010 | PSMCT16 | 16 | RGBA 5:5:5:1 |
| 001010 | PSMCT16S | 16 | RGBA 5:5:5:1 (swizzled) |
| 010011 | PSMT8 | 8 | 8-bit indexed |
| 010100 | PSMT4 | 4 | 4-bit indexed |
| 011011 | PSMT8H | 8 | 8-bit indexed (stored in high bits of 32-bit) |
| 100100 | PSMT4HL | 4 | 4-bit indexed (bits 24–27 of 32-bit) |
| 101100 | PSMT4HH | 4 | 4-bit indexed (bits 28–31 of 32-bit) |
| 110000 | PSMZ32 | 32 | Z buffer 32-bit |
| 110001 | PSMZ24 | 24 | Z buffer 24-bit |
| 110010 | PSMZ16 | 16 | Z buffer 16-bit |
| 111010 | PSMZ16S | 16 | Z buffer 16-bit (swizzled) |

---

### General-Purpose Register Address List

| Addr | Register | Description |
|------|----------|-------------|
| 0x00 | PRIM | Drawing primitive setting |
| 0x01 | RGBAQ | Vertex color + Q |
| 0x02 | ST | Vertex texture coords (S,T) |
| 0x03 | UV | Vertex texel coords (U,V) |
| 0x04 | XYZF2 | Vertex coord + fog (drawing kick) |
| 0x05 | XYZ2 | Vertex coord (drawing kick) |
| 0x06 | TEX0_1 | Texture info (Context 1) |
| 0x07 | TEX0_2 | Texture info (Context 2) |
| 0x08 | CLAMP_1 | Texture wrap mode (Context 1) |
| 0x09 | CLAMP_2 | Texture wrap mode (Context 2) |
| 0x0a | FOG | Vertex fog value |
| 0x0c | XYZF3 | Vertex coord + fog (no kick) |
| 0x0d | XYZ3 | Vertex coord (no kick) |
| 0x14 | TEX1_1 | Texture sampling (Context 1) |
| 0x15 | TEX1_2 | Texture sampling (Context 2) |
| 0x16 | TEX2_1 | Texture info subset (Context 1) |
| 0x17 | TEX2_2 | Texture info subset (Context 2) |
| 0x18 | XYOFFSET_1 | Coordinate offset (Context 1) |
| 0x19 | XYOFFSET_2 | Coordinate offset (Context 2) |
| 0x1a | PRMODECONT | Prim attribute source select |
| 0x1b | PRMODE | Drawing prim attributes |
| 0x1c | TEXCLUT | CLUT position (CSM2 mode) |
| 0x22 | SCANMSK | Raster address mask |
| 0x34 | MIPTBP1_1 | MIPMAP level 1–3 (Context 1) |
| 0x35 | MIPTBP1_2 | MIPMAP level 1–3 (Context 2) |
| 0x36 | MIPTBP2_1 | MIPMAP level 4–6 (Context 1) |
| 0x37 | MIPTBP2_2 | MIPMAP level 4–6 (Context 2) |
| 0x3b | TEXA | Texture alpha value |
| 0x3d | FOGCOL | Distant fog color |
| 0x3f | TEXFLUSH | Texture page buffer disable |
| 0x40 | SCISSOR_1 | Scissoring area (Context 1) |
| 0x41 | SCISSOR_2 | Scissoring area (Context 2) |
| 0x42 | ALPHA_1 | Alpha blending (Context 1) |
| 0x43 | ALPHA_2 | Alpha blending (Context 2) |
| 0x44 | DIMX | Dither matrix |
| 0x45 | DTHE | Dither control |
| 0x46 | COLCLAMP | Color clamp control |
| 0x47 | TEST_1 | Pixel test (Context 1) |
| 0x48 | TEST_2 | Pixel test (Context 2) |
| 0x49 | PABE | Per-pixel alpha blend enable |
| 0x4a | FBA_1 | Alpha correction (Context 1) |
| 0x4b | FBA_2 | Alpha correction (Context 2) |
| 0x4c | FRAME_1 | Frame buffer (Context 1) |
| 0x4d | FRAME_2 | Frame buffer (Context 2) |
| 0x4e | ZBUF_1 | Z buffer (Context 1) |
| 0x4f | ZBUF_2 | Z buffer (Context 2) |
| 0x50 | BITBLTBUF | Buffer transfer setting |
| 0x51 | TRXPOS | Transfer area position |
| 0x52 | TRXREG | Transfer area size |
| 0x53 | TRXDIR | Transfer activation |
| 0x54 | HWREG | Transfer data port |
| 0x60 | SIGNAL | SIGNAL event request |
| 0x61 | FINISH | FINISH event request |
| 0x62 | LABEL | LABEL event request |

---

### PRIM — Drawing Primitive Setting (0x00)

| Bits | Name | Description |
|------|------|-------------|
| 2:0 | PRIM | Primitive type: 000=Point, 001=Line, 010=LineStrip, 011=Triangle, 100=TriStrip, 101=TriFan, 110=Sprite |
| 3 | IIP | Shading: 0=Flat, 1=Gouraud |
| 4 | TME | Texture mapping: 0=OFF, 1=ON |
| 5 | FGE | Fog: 0=OFF, 1=ON |
| 6 | ABE | Alpha blending: 0=OFF, 1=ON |
| 7 | AA1 | 1-pass antialiasing: 0=OFF, 1=ON |
| 8 | FST | Texture coords: 0=STQ (float), 1=UV (fixed) |
| 9 | CTXT | Context: 0=Context 1, 1=Context 2 |
| 10 | FIX | Fragment value control: 0=unfixed, 1=fixed

> PRMODE (0x1b) has the same layout at bits 10:3 (no PRIM field).
> PRMODECONT (0x1a): bit 0 = AC; 0=use PRMODE, 1=use PRIM register.

### RGBAQ — Vertex Color + Q (0x01)

| Bits | Name | Format | Description |
|------|------|--------|-------------|
| 7:0 | R | u8 | Red (0–0xFF) |
| 15:8 | G | u8 | Green |
| 23:16 | B | u8 | Blue |
| 31:24 | A | u8 | Alpha (0x80 = fully opaque) |
| 63:32 | Q | float32 | Normalised texture coord Q |

### ST — Vertex Texture Coordinates (0x02)

| Bits | Name | Format | Description |
|------|------|--------|-------------|
| 31:0 | S | float32 | Texture coordinate S |
| 63:32 | T | float32 | Texture coordinate T |

### UV — Vertex Texel Coordinates (0x03)

| Bits | Name | Format | Description |
|------|------|--------|-------------|
| 13:0 | U | 10.4 fixed | Texel coordinate U |
| 29:16 | V | 10.4 fixed | Texel coordinate V |

### XYZF2 — Vertex Coord + Fog, Drawing Kick (0x04)

| Bits | Name | Format | Description |
|------|------|--------|-------------|
| 15:0 | X | 12.4 fixed | Vertex X (prim coord) |
| 31:16 | Y | 12.4 fixed | Vertex Y (prim coord) |
| 55:32 | Z | u24 | Vertex Z |
| 63:56 | F | u8 | Fog coefficient |

> XYZ2 (0x05) is the same but Z is full 32 bits [63:32] and no fog field.
> XYZF3 (0x0c) / XYZ3 (0x0d) are identical but do NOT trigger drawing kick.

### FOG — Vertex Fog Value (0x0a)

| Bits | Name | Description |
|------|------|-------------|
| 63:56 | F | Fog coefficient (u8) |

### TEX0_1 / TEX0_2 — Texture Information (0x06 / 0x07)

| Bits | Name | Description |
|------|------|-------------|
| 13:0 | TBP0 | Texture base pointer (word addr / 64) |
| 19:14 | TBW | Texture buffer width (texels / 64) |
| 25:20 | PSM | Texture pixel storage format (see PSM table) |
| 29:26 | TW | Texture width = 2^TW (max 2^10) |
| 33:30 | TH | Texture height = 2^TH (max 2^10) |
| 34 | TCC | Texture color component: 0=RGB, 1=RGBA |
| 36:35 | TFX | Texture function: 00=MODULATE, 01=DECAL, 10=HIGHLIGHT, 11=HIGHLIGHT2 |
| 50:37 | CBP | CLUT base pointer (word addr / 64) |
| 54:51 | CPSM | CLUT pixel format: 0000=PSMCT32, 0010=PSMCT16, 1010=PSMCT16S |
| 55 | CSM | CLUT storage mode: 0=CSM1, 1=CSM2 |
| 60:56 | CSA | CLUT entry offset (offset / 16) |
| 63:61 | CLD | CLUT load control (000=no change, 001=load, 010–101=conditional load with CBP0/CBP1) |

### TEX1_1 / TEX1_2 — Texture Sampling (0x14 / 0x15)

| Bits | Name | Description |
|------|------|-------------|
| 0 | LCM | LOD calc method: 0=formula, 1=fixed LOD=K |
| 4:2 | MXL | Max MIP level (0–6) |
| 5 | MMAG | Mag filter (LOD<0): 0=NEAREST, 1=LINEAR |
| 8:6 | MMIN | Min filter (LOD≥0): 000=NEAREST, 001=LINEAR, 010=NEAREST_MIPMAP_NEAREST, 011=NEAREST_MIPMAP_LINEAR, 100=LINEAR_MIPMAP_NEAREST, 101=LINEAR_MIPMAP_LINEAR |
| 9 | MTBA | MIP base addr: 0=MIPTBP1/2 values, 1=auto |
| 20:19 | L | LOD parameter L |
| 43:32 | K | LOD parameter K (signed 7.4 fixed) |

### TEX2_1 / TEX2_2 — Texture Info Subset (0x16 / 0x17)

Same as TEX0 but only fields PSM[25:20], CBP[50:37], CPSM[54:51], CSM[55], CSA[60:56], CLD[63:61].

### CLAMP_1 / CLAMP_2 — Texture Wrap Mode (0x08 / 0x09)

| Bits | Name | Description |
|------|------|-------------|
| 1:0 | WMS | Wrap mode S: 00=REPEAT, 01=CLAMP, 10=REGION_CLAMP, 11=REGION_REPEAT |
| 3:2 | WMT | Wrap mode T: same values |
| 13:4 | MINU | Min U clamp value |
| 23:14 | MAXU | Max U clamp value |
| 33:24 | MINV | Min V clamp value |
| 43:34 | MAXV | Max V clamp value |

### MIPTBP1_1 / MIPTBP1_2 — MIP Level 1–3 (0x34 / 0x35)

| Bits | Name | Description |
|------|------|-------------|
| 13:0 | TBP1 | Level 1 base pointer (addr/64) |
| 19:14 | TBW1 | Level 1 buffer width (texels/64) |
| 33:20 | TBP2 | Level 2 base pointer |
| 39:34 | TBW2 | Level 2 buffer width |
| 53:40 | TBP3 | Level 3 base pointer |
| 59:54 | TBW3 | Level 3 buffer width |

### MIPTBP2_1 / MIPTBP2_2 — MIP Level 4–6 (0x36 / 0x37)

Same layout as MIPTBP1 but for levels 4, 5, 6.

### TEXCLUT — CLUT Position (CSM2 Mode) (0x1c)

| Bits | Name | Description |
|------|------|-------------|
| 5:0 | CBW | CLUT buffer width (pixels / 64) |
| 11:6 | COU | CLUT offset U (pixels / 16) |
| 21:12 | COV | CLUT offset V (pixels) |

### TEXA — Texture Alpha Value (0x3b)

| Bits | Name | Description |
|------|------|-------------|
| 7:0 | TA0 | Alpha when A-field=0 in RGBA16 or RGB24 |
| 15 | AEM | 0=normal, 1=transparent (A=0) when R=G=B=0 |
| 39:32 | TA1 | Alpha when A-field=1 in RGBA16 |

### FOGCOL — Distant Fog Color (0x3d)

| Bits | Name | Description |
|------|------|-------------|
| 7:0 | FCR | Fog color Red |
| 15:8 | FCG | Fog color Green |
| 23:16 | FCB | Fog color Blue |

### XYOFFSET_1 / XYOFFSET_2 — Coordinate Offset (0x18 / 0x19)

| Bits | Name | Format | Description |
|------|------|--------|-------------|
| 15:0 | OFX | 12.4 fixed | Offset X (prim → window) |
| 47:32 | OFY | 12.4 fixed | Offset Y (prim → window) |

### SCISSOR_1 / SCISSOR_2 — Scissoring Area (0x40 / 0x41)

| Bits | Name | Description |
|------|------|-------------|
| 10:0 | SCAX0 | X upper-left (window coords) |
| 26:16 | SCAX1 | X lower-right |
| 42:32 | SCAY0 | Y upper-left |
| 58:48 | SCAY1 | Y lower-right |

### ALPHA_1 / ALPHA_2 — Alpha Blending (0x42 / 0x43)

Formula: `Cv = (A - B) * C >> 7 + D`
Where A/B/D select color source and C selects alpha source.

| Bits | Name | Values |
|------|------|--------|
| 1:0 | A | 00=Cs (source), 01=Cd (dest), 10=0 |
| 3:2 | B | 00=Cs, 01=Cd, 10=0 |
| 5:4 | C | 00=As (src alpha), 01=Ad (dst alpha), 10=FIX |
| 7:6 | D | 00=Cs, 01=Cd, 10=0 |
| 39:32 | FIX | Fixed alpha value (used when C=FIX) |

### DIMX — Dither Matrix (0x44)

| Bits | Name | Description |
|------|------|-------------|
| 2:0 | DM00 | Matrix[0][0] (signed 3-bit, –4 to +3) |
| 6:4 | DM01 | Matrix[0][1] |
| 10:8 | DM02 | Matrix[0][2] |
| 14:12 | DM03 | Matrix[0][3] |
| 18:16 | DM10 | Matrix[1][0] |
| 22:20 | DM11 | Matrix[1][1] |
| 26:24 | DM12 | Matrix[1][2] |
| 30:28 | DM13 | Matrix[1][3] |
| 34:32 | DM20 | Matrix[2][0] |
| 38:36 | DM21 | Matrix[2][1] |
| 42:40 | DM22 | Matrix[2][2] |
| 46:44 | DM23 | Matrix[2][3] |
| 50:48 | DM30 | Matrix[3][0] |
| 54:52 | DM31 | Matrix[3][1] |
| 58:56 | DM32 | Matrix[3][2] |
| 62:60 | DM33 | Matrix[3][3] |

### DTHE — Dither Control (0x45)

| Bits | Name | Description |
|------|------|-------------|
| 0 | DTHE | 0=Dither OFF, 1=Dither ON |

### COLCLAMP — Color Clamp Control (0x46)

| Bits | Name | Description |
|------|------|-------------|
| 0 | CLAMP | 0=Mask (lower 8 bits), 1=Clamp (0–255) |

### TEST_1 / TEST_2 — Pixel Test Control (0x47 / 0x48)

| Bits | Name | Description |
|------|------|-------------|
| 0 | ATE | Alpha test: 0=OFF, 1=ON |
| 3:1 | ATST | Alpha test method: 000=NEVER, 001=ALWAYS, 010=LESS, 011=LEQUAL, 100=EQUAL, 101=GEQUAL, 110=GREATER, 111=NOTEQUAL |
| 11:4 | AREF | Alpha reference value (u8) |
| 13:12 | AFAIL | Alpha fail action: 00=KEEP, 01=FB_ONLY, 10=ZB_ONLY, 11=RGB_ONLY |
| 14 | DATE | Destination alpha test: 0=OFF, 1=ON |
| 15 | DATM | Dest alpha test mode: 0=pass if dst_alpha==0, 1=pass if dst_alpha==1 |
| 16 | ZTE | Depth test: 0=OFF (not allowed), 1=ON |
| 18:17 | ZTST | Depth test method: 00=NEVER, 01=ALWAYS, 10=GEQUAL, 11=GREATER |

### PABE — Per-Pixel Alpha Blend Enable (0x49)

| Bits | Name | Description |
|------|------|-------------|
| 0 | PABE | 0=disabled, 1=alpha blending only for pixels with MSB(A)==1 |

### FBA_1 / FBA_2 — Alpha Correction (0x4a / 0x4b)

| Bits | Name | Description |
|------|------|-------------|
| 0 | FBA | Value OR'd into MSB of alpha after alpha test |

### FRAME_1 / FRAME_2 — Frame Buffer Setting (0x4c / 0x4d)

| Bits | Name | Description |
|------|------|-------------|
| 8:0 | FBP | Frame buffer base pointer (word addr / 2048) |
| 21:16 | FBW | Frame buffer width (pixels / 64) |
| 27:24 | PSM | Pixel storage format: 0000=PSMCT32, 0001=PSMCT24, 0010=PSMCT16, 1010=PSMCT16S |
| 63:32 | FBMSK | Frame buffer write mask (1=masked/protected) |

### ZBUF_1 / ZBUF_2 — Z Buffer Setting (0x4e / 0x4f)

| Bits | Name | Description |
|------|------|-------------|
| 8:0 | ZBP | Z buffer base pointer (word addr / 2048) |
| 27:24 | PSM | Z format: 0000=PSMZ32, 0001=PSMZ24, 0010=PSMZ16, 1010=PSMZ16S |
| 32 | ZMSK | Z write mask: 0=update Z, 1=don't update Z |

> Z buffer width is inherited from the FRAME register (same size).

### SCANMSK — Raster Address Mask (0x22)

| Bits | Name | Values |
|------|------|--------|
| 1:0 | MSK | 00=Normal, 10=Prohibit even-Y drawing, 11=Prohibit odd-Y drawing |

### BITBLTBUF — Buffer Transfer Setting (0x50)

| Bits | Name | Description |
|------|------|-------------|
| 13:0 | SBP | Source base pointer (word addr / 64) |
| 19:16 | SBW | Source buffer width (pixels / 64) |
| 29:24 | SPSM | Source pixel format |
| 45:32 | DBP | Destination base pointer (word addr / 64) |
| 51:48 | DBW | Destination buffer width (pixels / 64) |
| 61:56 | DPSM | Destination pixel format |

### TRXPOS — Transfer Area Position (0x51)

| Bits | Name | Description |
|------|------|-------------|
| 10:0 | SSAX | Source X (upper-left) |
| 26:16 | SSAY | Source Y (upper-left) |
| 42:32 | DSAX | Destination X (upper-left) |
| 58:48 | DSAY | Destination Y (upper-left) |
| 60:59 | DIR | Pixel order (local→local only): 00=UL→LR, 01=LL→UR, 10=UR→LL, 11=LR→UL |

### TRXREG — Transfer Area Size (0x52)

| Bits | Name | Description |
|------|------|-------------|
| 11:0 | RRW | Width of transmission area |
| 43:32 | RRH | Height of transmission area |

### TRXDIR — Transfer Activation (0x53)

| Bits | Name | Values |
|------|------|--------|
| 1:0 | XDIR | 00=Host→Local, 01=Local→Host, 10=Local→Local, 11=Deactivated |

### SIGNAL — SIGNAL Event (0x60)

| Bits | Name | Description |
|------|------|-------------|
| 31:0 | ID | Value written to SIGLBLID register |
| 63:32 | IDMSK | Bit mask (1=update corresponding SIGLBLID bit) |

### FINISH — FINISH Event (0x61)

No fields — writing any value triggers FINISH event.

### LABEL — LABEL Event (0x62)

| Bits | Name | Description |
|------|------|-------------|
| 31:0 | ID | Value written to SIGLBLID register (LBLID portion) |
| 63:32 | IDMSK | Bit mask (1=update corresponding SIGLBLID bit) |

---

### Privileged Register Address List (at GS base `0x12000000`)

| Addr | Register | Access | Description |
|------|----------|--------|-------------|
| 0x00 | PMODE | w | PCRTC mode setting |
| 0x02 | SMODE2 | w | Video sync mode |
| 0x07 | DISPFB1 | w | Read output circuit 1 buffer |
| 0x08 | DISPLAY1 | w | Read output circuit 1 display |
| 0x09 | DISPFB2 | w | Read output circuit 2 buffer |
| 0x0a | DISPLAY2 | w | Read output circuit 2 display |
| 0x0b | EXTBUF | w | Feedback write buffer |
| 0x0c | EXTDATA | w | Feedback write setting |
| 0x0d | EXTWRITE | w | Feedback write control |
| 0x0e | BGCOLOR | w | Background color |
| 0x40 | CSR | r/w | System status |
| 0x41 | IMR | w | Interrupt mask control |
| 0x44 | BUSDIR | w | Host interface bus direction |
| 0x48 | SIGLBLID | r/w | Signal/Label ID values |

### PMODE — PCRTC Mode Setting (Priv 0x00)

| Bits | Name | Description |
|------|------|-------------|
| 0 | EN1 | Read circuit 1: 0=OFF, 1=ON |
| 1 | EN2 | Read circuit 2: 0=OFF, 1=ON |
| 4:2 | CRTMD | CRT output switching (always 001) |
| 5 | MMOD | Alpha select: 0=Circuit 1 alpha, 1=ALP field value |
| 6 | AMOD | OUT1 alpha output: 0=Circuit 1, 1=Circuit 2 |
| 7 | SLBG | Blend method: 0=with Circuit 2, 1=with background color |
| 15:8 | ALP | Fixed alpha value (0xFF = 1.0) |

### SMODE2 — Video Sync Mode (Priv 0x02)

| Bits | Name | Description |
|------|------|-------------|
| 0 | INT | 0=Non-interlace, 1=Interlace |
| 1 | FFMD | Interlace mode: 0=FIELD (every-other), 1=FRAME (every line) |
| 3:2 | DPMS | VESA DPMS: 00=On, 01=Standby, 10=Suspend, 11=Off |

### DISPFB1 / DISPFB2 — Read Output Circuit Buffer (Priv 0x07 / 0x09)

| Bits | Name | Description |
|------|------|-------------|
| 8:0 | FBP | Base pointer (addr / 2048) |
| 14:9 | FBW | Buffer width (pixels / 64) |
| 19:15 | PSM | Pixel format (5-bit: 00000=CT32, 00001=CT24, 00010=CT16, 01010=CT16S) |
| 42:32 | DBX | X position of display rect upper-left |
| 53:43 | DBY | Y position of display rect upper-left |

### DISPLAY1 / DISPLAY2 — Read Output Circuit Display (Priv 0x08 / 0x0a)

| Bits | Name | Description |
|------|------|-------------|
| 11:0 | DX | Display X position (VCK units) |
| 22:12 | DY | Display Y position (raster units) |
| 26:23 | MAGH | Horizontal magnification (0=×1 … 15=×16) |
| 28:27 | MAGV | Vertical magnification (0=×1 … 3=×4) |
| 43:32 | DW | Display width − 1 (VCK units) |
| 54:44 | DH | Display height − 1 (pixel units) |

### CSR — System Status (Priv 0x40, r/w)

| Bits | Name | W-Meaning | R-Meaning |
|------|------|-----------|-----------|
| 0 | SIGNAL | 1=clear & enable | 1=event occurred |
| 1 | FINISH | 1=enable | 1=event occurred |
| 2 | HSINT | 1=enable HSync int | 1=HSync int occurred |
| 3 | VSINT | 1=enable VSync int | 1=VSync int occurred |
| 4 | EDWINT | 1=enable rect-write int | 1=rect-write int occurred |
| 8 | FLUSH | 1=flush drawing + clear FIFO | — |
| 9 | RESET | 1=reset GS | — |
| 12 | NFIELD | — | Output value of NFIELD |
| 13 | FIELD | — | 0=EVEN, 1=ODD |
| 15:14 | FIFO | — | 00=normal, 01=empty, 10=almost full |
| 23:16 | REV | — | GS revision number |
| 31:24 | ID | — | GS ID |

### IMR — Interrupt Mask (Priv 0x41)

| Bits | Name | Description |
|------|------|-------------|
| 8 | SIGMSK | SIGNAL interrupt mask: 0=unmasked, 1=masked |
| 9 | FINISHMSK | FINISH interrupt mask |
| 10 | HSMSK | HSync interrupt mask |
| 11 | VSMSK | VSync interrupt mask |
| 12 | EDWMSK | Rect-write termination interrupt mask |

> All initially masked (=1) after reset. Undefined bits should be set to 1.

### BGCOLOR — Background Color (Priv 0x0e)

| Bits | Name | Description |
|------|------|-------------|
| 7:0 | R | Background red |
| 15:8 | G | Background green |
| 23:16 | B | Background blue |

### BUSDIR — Host Interface Bus Direction (Priv 0x44)

| Bits | Name | Description |
|------|------|-------------|
| 0 | DIR | 0=Host→Local (normal), 1=Local→Host |

### SIGLBLID — Signal/Label ID Values (Priv 0x48, r/w)

| Bits | Name | Description |
|------|------|-------------|
| 31:0 | SIGID | ID set by SIGNAL register |
| 63:32 | LBLID | ID set by LABEL register |

### EXTBUF — Feedback Write Buffer (Priv 0x0b)

| Bits | Name | Description |
|------|------|-------------|
| 13:0 | EXBP | Base pointer (word addr / 64) |
| 19:14 | EXBW | Buffer width (pixels / 64) |
| 21:20 | FBIN | Input source: 00=OUT1, 01=OUT2 |
| 22 | WFFMD | Interlace: 0=FIELD (every other), 1=FRAME (every line) |
| 24:23 | EMODA | Alpha processing: 00=as-is, 01=Y from RGB, 10=Y/2, 11=always 0 |
| 26:25 | EMODC | Color processing: 00=as-is, 01=Y to RGB, 10=YCbCr, 11=A to RGB |
| 42:32 | WDX | Write dest X |
| 53:43 | WDY | Write dest Y |

### EXTDATA — Feedback Write Setting (Priv 0x0c)

| Bits | Name | Description |
|------|------|-------------|
| 11:0 | SX | Source X (VCK units) |
| 22:12 | SY | Source Y (pixel units) |
| 26:23 | SMPH | H sampling rate (0=every VCK … 15=every 16th) |
| 28:27 | SMPV | V sampling rate (0=every HSync … 3=every 4th) |
| 43:32 | WW | Width − 1 |
| 54:44 | WH | Height − 1 |

### EXTWRITE — Feedback Write Control (Priv 0x0d)

| Bits | Name | Description |
|------|------|-------------|
| 0 | WRITE | 0=complete in current frame, 1=start from next frame |

---

## COP0 (System Control) Registers

> R5900 COP0 register bit-fields. Source: EE Core User's Manual, TX79 Architecture Manual.

### COP0 Register Map

| Reg | Sel | Name | Description |
|-----|-----|------|-------------|
| 0 | 0 | Index | TLB entry index |
| 2 | 0 | EntryLo0 | TLB even page entry |
| 3 | 0 | EntryLo1 | TLB odd page entry |
| 4 | 0 | Context | TLB context (BadVPN2 + PTEBase) |
| 5 | 0 | PageMask | TLB page size mask |
| 6 | 0 | Wired | Number of wired TLB entries |
| 8 | 0 | BadVAddr | Bad virtual address |
| 9 | 0 | Count | Timer tick count (halfclock) |
| 10 | 0 | EntryHi | TLB entry ASID + VPN2 |
| 11 | 0 | Compare | Timer compare (interrupt when Count matches) |
| 12 | 0 | **Status** | Processor status & enable bits |
| 13 | 0 | **Cause** | Exception cause |
| 14 | 0 | **EPC** | Exception Program Counter |
| 15 | 0 | PRId | Processor Revision Identifier |
| 16 | 0 | Config | System configuration |
| 23 | 0 | BadPAddr | Bad physical address (R5900 specific) |
| 24 | 0 | Debug | Breakpoint debug register |
| 25 | 0 | Perf | Performance counter control |
| 28 | 0 | TagLo | Cache tag (low) |
| 28 | 1 | TagHi | Cache tag (high) |
| 30 | 0 | ErrorEPC | Error exception PC |

### Status Register (COP0 reg 12) — Bit Fields

| Bits | Name | Description |
|------|------|-------------|
| 0 | IE | Interrupt Enable (global) |
| 1 | EXL | Exception Level (set on exception entry) |
| 2 | ERL | Error Level (set on reset/NMI) |
| 3:4 | KSU | Kernel/Supervisor/User mode (00=kernel, 10=user) |
| 10 | IM[0] | Interrupt Mask — Software interrupt 0 |
| 11 | IM[1] | Interrupt Mask — Software interrupt 1 |
| 12 | BEM | Bus Error Mask |
| 15:13 | IM[7:2] | Interrupt Mask — Hardware interrupts |
| 16 | EIE | EI/DI instruction Enable (R5900 extension) |
| 17 | EDI | EI/DI valid in user mode |
| 18 | CH | Cache Hit indicator |
| 22 | BEV | Bootstrap Exception Vectors (1=ROM vectors) |
| 23 | DEV | Debug Exception Vectors |
| 28 | CU[0] | COP0 Usable |
| 29 | CU[1] | COP1 (FPU) Usable |
| 30 | CU[2] | COP2 (VU0 Macro) Usable |
| 31 | CU[3] | Reserved |

### Cause Register (COP0 reg 13) — Bit Fields

| Bits | Name | Description |
|------|------|-------------|
| 6:2 | ExcCode | Exception code (see table below) |
| 8 | IP[0] | Software interrupt 0 pending |
| 9 | IP[1] | Software interrupt 1 pending |
| 10 | EXC2 | Level 2 exception (R5900) |
| 12:11 | — | Reserved |
| 15:13 | IP[7:2] | Hardware interrupt pending |
| 28 | CE[0] | Coprocessor number in COP unusable exception |
| 29 | CE[1] | |
| 30 | BD2 | Level 2 branch delay (R5900) |
| 31 | BD | Exception in branch delay slot |

### Exception Codes (ExcCode field)

| Code | Name | Description |
|------|------|-------------|
| 0x00 | Int | Interrupt |
| 0x01 | Mod | TLB Modified |
| 0x02 | TLBL | TLB Refill (Load/Fetch) |
| 0x03 | TLBS | TLB Refill (Store) |
| 0x04 | AdEL | Address Error (Load/Fetch) |
| 0x05 | AdES | Address Error (Store) |
| 0x06 | IBE | Bus Error (Instruction Fetch) |
| 0x07 | DBE | Bus Error (Data Load/Store) |
| 0x08 | Sys | Syscall |
| 0x09 | Bp | Breakpoint |
| 0x0A | RI | Reserved Instruction |
| 0x0B | CpU | Coprocessor Unusable |
| 0x0C | Ov | Arithmetic Overflow |
| 0x0D | Tr | Trap |
| 0x0E | — | Reserved |
| 0x0F | — | Reserved |
| 0x12 | RESET | Reset (R5900 only) |
| 0x13 | NMI | Non-Maskable Interrupt |
| 0x14 | PERF | Performance Counter |
| 0x15 | DBG | Debug |

### PRId Register (COP0 reg 15)

| Bits | Name | Value |
|------|------|-------|
| 7:0 | Rev | Revision (varies) |
| 15:8 | Imp | Implementation: `0x2E` = R5900 (EE) |
| 23:16 | — | Reserved |
| 31:24 | — | Company: `0x00` |

### Config Register (COP0 reg 16)

| Bits | Name | Description |
|------|------|-------------|
| 2:0 | K0 | KSEG0 coherency (2=uncached, 3=cacheable writeback) |
| 6:4 | DC | Data cache size (= 2^(12+DC) bytes, typically 6=8KB) |
| 9:7 | IC | Instruction cache size (= 2^(12+IC) bytes, typically 6=16KB) |
| 12 | BPE | Branch prediction enable |
| 13 | NBE | Non-blocking enable |
| 16 | DIE | Dual-Issue Enable (R5900 superscalar) |
| 17 | ICE | I-cache enable |
| 18 | DCE | D-cache enable |
| 28 | EC | Endian Config (0=little endian) |

