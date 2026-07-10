# R5900 (EE Core) Instruction Set Reference
> Complete opcode encoding tables for the PS2 Emotion Engine CPU.
> **Sources**: EE Core Instruction Set Manual, TX79 Architecture Manual, `instructions.h` enum values.
>
> **See also**: `db-vu-instructions.md` (VU0/VU1 micro instruction set), `db-ps2-architecture.md` §2 (EE Core pipeline + dual-issue rules), `db-registers.md` (COP0 register bit-fields).

## Lookup Protocol
1. Extract opcode bits [31:26] → match in **Primary Opcode Table**
2. If SPECIAL (0x00) → bits [5:0] → **SPECIAL Function Table**
3. If REGIMM (0x01) → bits [20:16] → **REGIMM RT Table**
4. If MMI (0x1C) → bits [5:0] → **MMI Function Table** (may cascade to MMI0/1/2/3)
5. If COP0/COP1/COP2 → bits [25:21] → **COPx Format Table** (may cascade further)

## Instruction Formats

```
R-Type:  [opcode:6][rs:5][rt:5][rd:5][sa:5][function:6]
I-Type:  [opcode:6][rs:5][rt:5][immediate:16]
J-Type:  [opcode:6][target:26]
```

### Decoding Macros (from instructions.h)
| Macro | Expression | Purpose |
|-------|-----------|---------|
| `OPCODE(i)` | `(i >> 26) & 0x3F` | Primary opcode bits [31:26] |
| `RS(i)` | `(i >> 21) & 0x1F` | Source register / COP format |
| `RT(i)` | `(i >> 16) & 0x1F` | Target register / REGIMM code |
| `RD(i)` | `(i >> 11) & 0x1F` | Destination register |
| `SA(i)` | `(i >> 6) & 0x1F` | Shift amount |
| `FUNCTION(i)` | `i & 0x3F` | Function field bits [5:0] |
| `IMMEDIATE(i)` | `i & 0xFFFF` | Unsigned 16-bit immediate |
| `SIMMEDIATE(i)` | `(int16_t)(i & 0xFFFF)` | Sign-extended 16-bit immediate |
| `TARGET(i)` | `i & 0x3FFFFFF` | 26-bit jump target |
| `FT(i)` | `RT(i)` | FPU target register |
| `FS(i)` | `RD(i)` | FPU source register |
| `FD(i)` | `SA(i)` | FPU destination register |

---

## 1. Primary Opcode Table — bits [31:26]

| Hex | Name | Type | Description | PS2? |
|-----|------|------|-------------|------|
| 0x00 | SPECIAL | R | → SPECIAL function table (bits [5:0]) | |
| 0x01 | REGIMM | I | → REGIMM RT table (bits [20:16]) | |
| 0x02 | J | J | Jump | |
| 0x03 | JAL | J | Jump and Link | |
| 0x04 | BEQ | I | Branch on Equal | |
| 0x05 | BNE | I | Branch on Not Equal | |
| 0x06 | BLEZ | I | Branch on ≤ Zero | |
| 0x07 | BGTZ | I | Branch on > Zero | |
| 0x08 | ADDI | I | Add Immediate Word | |
| 0x09 | ADDIU | I | Add Immediate Unsigned Word | |
| 0x0A | SLTI | I | Set on Less Than Immediate | |
| 0x0B | SLTIU | I | Set on Less Than Immediate Unsigned | |
| 0x0C | ANDI | I | AND Immediate | |
| 0x0D | ORI | I | OR Immediate | |
| 0x0E | XORI | I | XOR Immediate | |
| 0x0F | LUI | I | Load Upper Immediate | |
| 0x10 | COP0 | — | → COP0 format table (bits [25:21]) | |
| 0x11 | COP1 | — | → COP1/FPU format table (bits [25:21]) | |
| 0x12 | COP2 | — | → COP2/VU0 macro format table (bits [25:21]) | |
| 0x14 | BEQL | I | Branch on Equal Likely | |
| 0x15 | BNEL | I | Branch on Not Equal Likely | |
| 0x16 | BLEZL | I | Branch on ≤ Zero Likely | |
| 0x17 | BGTZL | I | Branch on > Zero Likely | |
| 0x18 | DADDI | I | Doubleword Add Immediate | |
| 0x19 | DADDIU | I | Doubleword Add Immediate Unsigned | |
| 0x1A | LDL | I | Load Doubleword Left | |
| 0x1B | LDR | I | Load Doubleword Right | |
| **0x1C** | **MMI** | R | **→ MMI function table (bits [5:0])** | **PS2** |
| **0x1E** | **LQ** | I | **Load Quadword (128-bit, 16-byte aligned)** | **PS2** |
| **0x1F** | **SQ** | I | **Store Quadword (128-bit, 16-byte aligned)** | **PS2** |
| 0x20 | LB | I | Load Byte | |
| 0x21 | LH | I | Load Halfword | |
| 0x22 | LWL | I | Load Word Left | |
| 0x23 | LW | I | Load Word | |
| 0x24 | LBU | I | Load Byte Unsigned | |
| 0x25 | LHU | I | Load Halfword Unsigned | |
| 0x26 | LWR | I | Load Word Right | |
| 0x27 | LWU | I | Load Word Unsigned | |
| 0x28 | SB | I | Store Byte | |
| 0x29 | SH | I | Store Halfword | |
| 0x2A | SWL | I | Store Word Left | |
| 0x2B | SW | I | Store Word | |
| 0x2C | SDL | I | Store Doubleword Left | |
| 0x2D | SDR | I | Store Doubleword Right | |
| 0x2E | SWR | I | Store Word Right | |
| 0x2F | CACHE | I | Cache Operation | |
| 0x31 | LWC1 | I | Load Word to FPU | |
| 0x33 | PREF | I | Prefetch | |
| **0x36** | **LQC2** | I | **Load Quadword to VU0 (128-bit)** | **PS2** |
| 0x37 | LD | I | Load Doubleword | |
| 0x39 | SWC1 | I | Store Word from FPU | |
| **0x3E** | **SQC2** | I | **Store Quadword from VU0 (128-bit)** | **PS2** |
| 0x3F | SD | I | Store Doubleword | |

> [!IMPORTANT]
> PS2-only opcodes: `LQ` (0x1E), `SQ` (0x1F), `MMI` (0x1C), `LQC2` (0x36), `SQC2` (0x3E).
> `LQ`/`SQ` require **16-byte alignment** or SIGBUS.

---

## 2. SPECIAL Function Table — opcode=0x00, bits [5:0]

| Hex | Name | Description | PS2? |
|-----|------|-------------|------|
| 0x00 | SLL | Shift Word Left Logical | |
| 0x02 | SRL | Shift Word Right Logical | |
| 0x03 | SRA | Shift Word Right Arithmetic | |
| 0x04 | SLLV | Shift Word Left Logical Variable | |
| 0x06 | SRLV | Shift Word Right Logical Variable | |
| 0x07 | SRAV | Shift Word Right Arithmetic Variable | |
| 0x08 | JR | Jump Register | |
| 0x09 | JALR | Jump and Link Register | |
| **0x0A** | **MOVZ** | **Move Conditional on Zero** | **PS2** |
| **0x0B** | **MOVN** | **Move Conditional on Not Zero** | **PS2** |
| 0x0C | SYSCALL | System Call | |
| 0x0D | BREAK | Breakpoint | |
| 0x0F | SYNC | Synchronize Shared Memory | |
| 0x10 | MFHI | Move From HI Register | |
| 0x11 | MTHI | Move To HI Register | |
| 0x12 | MFLO | Move From LO Register | |
| 0x13 | MTLO | Move To LO Register | |
| 0x14 | DSLLV | Doubleword Shift Left Logical Variable | |
| 0x16 | DSRLV | Doubleword Shift Right Logical Variable | |
| 0x17 | DSRAV | Doubleword Shift Right Arithmetic Variable | |
| 0x18 | MULT | Multiply Word | |
| 0x19 | MULTU | Multiply Unsigned Word | |
| 0x1A | DIV | Divide Word | |
| 0x1B | DIVU | Divide Unsigned Word | |
| 0x20 | ADD | Add Word | |
| 0x21 | ADDU | Add Unsigned Word | |
| 0x22 | SUB | Subtract Word | |
| 0x23 | SUBU | Subtract Unsigned Word | |
| 0x24 | AND | AND | |
| 0x25 | OR | OR | |
| 0x26 | XOR | XOR | |
| 0x27 | NOR | NOR | |
| **0x28** | **MFSA** | **Move From SA Register** | **PS2** |
| **0x29** | **MTSA** | **Move To SA Register** | **PS2** |
| 0x2A | SLT | Set on Less Than | |
| 0x2B | SLTU | Set on Less Than Unsigned | |
| 0x2C | DADD | Doubleword Add | |
| 0x2D | DADDU | Doubleword Add Unsigned | |
| 0x2E | DSUB | Doubleword Subtract | |
| 0x2F | DSUBU | Doubleword Subtract Unsigned | |
| 0x30 | TGE | Trap if ≥ | |
| 0x31 | TGEU | Trap if ≥ Unsigned | |
| 0x32 | TLT | Trap if < | |
| 0x33 | TLTU | Trap if < Unsigned | |
| 0x34 | TEQ | Trap if Equal | |
| 0x36 | TNE | Trap if Not Equal | |
| 0x38 | DSLL | Doubleword Shift Left Logical | |
| 0x3A | DSRL | Doubleword Shift Right Logical | |
| 0x3B | DSRA | Doubleword Shift Right Arithmetic | |
| 0x3C | DSLL32 | Doubleword Shift Left Logical +32 | |
| 0x3E | DSRL32 | Doubleword Shift Right Logical +32 | |
| 0x3F | DSRA32 | Doubleword Shift Right Arithmetic +32 | |

---

## 3. REGIMM RT Table — opcode=0x01, bits [20:16]

| Hex | Name | Description | PS2? |
|-----|------|-------------|------|
| 0x00 | BLTZ | Branch on < Zero | |
| 0x01 | BGEZ | Branch on ≥ Zero | |
| 0x02 | BLTZL | Branch on < Zero Likely | |
| 0x03 | BGEZL | Branch on ≥ Zero Likely | |
| 0x08 | TGEI | Trap if ≥ Immediate | |
| 0x09 | TGEIU | Trap if ≥ Immediate Unsigned | |
| 0x0A | TLTI | Trap if < Immediate | |
| 0x0B | TLTIU | Trap if < Immediate Unsigned | |
| 0x0C | TEQI | Trap if Equal Immediate | |
| 0x0E | TNEI | Trap if Not Equal Immediate | |
| 0x10 | BLTZAL | Branch on < Zero and Link | |
| 0x11 | BGEZAL | Branch on ≥ Zero and Link | |
| 0x12 | BLTZALL | Branch on < Zero and Link Likely | |
| 0x13 | BGEZALL | Branch on ≥ Zero and Link Likely | |
| **0x18** | **MTSAB** | **Move To SA Register Byte** | **PS2** |
| **0x19** | **MTSAH** | **Move To SA Register Halfword** | **PS2** |

---

## 4. MMI Function Table — opcode=0x1C, bits [5:0] (ALL PS2-SPECIFIC)

| Hex | Name | Description |
|-----|------|-------------|
| 0x00 | MADD | Multiply-Add Word (to LO/HI) |
| 0x01 | MADDU | Multiply-Add Unsigned Word |
| 0x02 | MSUB | Multiply-Subtract Word |
| 0x03 | MSUBU | Multiply-Subtract Unsigned Word |
| 0x04 | PLZCW | Parallel Leading Zero/One Count Word |
| 0x08 | MMI0 | → MMI0 sub-table (bits [10:6]) |
| 0x09 | MMI2 | → MMI2 sub-table (bits [10:6]) |
| 0x10 | MFHI1 | Move From HI1 Register |
| 0x11 | MTHI1 | Move To HI1 Register |
| 0x12 | MFLO1 | Move From LO1 Register |
| 0x13 | MTLO1 | Move To LO1 Register |
| 0x18 | MULT1 | Multiply Word → LO1/HI1 |
| 0x19 | MULTU1 | Multiply Unsigned Word → LO1/HI1 |
| 0x1A | DIV1 | Divide Word using LO1/HI1 |
| 0x1B | DIVU1 | Divide Unsigned Word using LO1/HI1 |
| 0x20 | MADD1 | Multiply-Add Word → LO1/HI1 |
| 0x21 | MADDU1 | Multiply-Add Unsigned Word → LO1/HI1 |
| 0x28 | MMI1 | → MMI1 sub-table (bits [10:6]) |
| 0x29 | MMI3 | → MMI3 sub-table (bits [10:6]) |
| 0x30 | PMFHL | Parallel Move From HI/LO (sa field selects variant) |
| 0x31 | PMTHL | Parallel Move To HI/LO |
| 0x34 | PSLLH | Parallel Shift Left Logical Halfword |
| 0x36 | PSRLH | Parallel Shift Right Logical Halfword |
| 0x37 | PSRAH | Parallel Shift Right Arithmetic Halfword |
| 0x3C | PSLLW | Parallel Shift Left Logical Word |
| 0x3E | PSRLW | Parallel Shift Right Logical Word |
| 0x3F | PSRAW | Parallel Shift Right Arithmetic Word |

### PMFHL Variants — sa field bits [10:6]
| sa | Name | Description |
|----|------|-------------|
| 0x00 | PMFHL.LW | Lower Word |
| 0x01 | PMFHL.UW | Upper Word |
| 0x02 | PMFHL.SLW | Signed Lower Word |
| 0x03 | PMFHL.LH | Lower Halfword |
| 0x04 | PMFHL.SH | Signed Halfword |

---

### 4a. MMI0 Sub-Table — MMI function=0x08, bits [10:6]

| Hex | Name | Description |
|-----|------|-------------|
| 0x00 | PADDW | Parallel Add Word |
| 0x01 | PSUBW | Parallel Subtract Word |
| 0x02 | PCGTW | Parallel Compare Greater Than Word |
| 0x03 | PMAXW | Parallel Maximum Word |
| 0x04 | PADDH | Parallel Add Halfword |
| 0x05 | PSUBH | Parallel Subtract Halfword |
| 0x06 | PCGTH | Parallel Compare Greater Than Halfword |
| 0x07 | PMAXH | Parallel Maximum Halfword |
| 0x08 | PADDB | Parallel Add Byte |
| 0x09 | PSUBB | Parallel Subtract Byte |
| 0x0A | PCGTB | Parallel Compare Greater Than Byte |
| 0x10 | PADDSW | Parallel Add with Signed Saturation Word |
| 0x11 | PSUBSW | Parallel Subtract with Signed Saturation Word |
| 0x12 | PEXTLW | Parallel Extend Lower Word |
| 0x13 | PPACW | Parallel Pack Word |
| 0x14 | PADDSH | Parallel Add with Signed Saturation Halfword |
| 0x15 | PSUBSH | Parallel Subtract with Signed Saturation Halfword |
| 0x16 | PEXTLH | Parallel Extend Lower Halfword |
| 0x17 | PPACH | Parallel Pack Halfword |
| 0x18 | PADDSB | Parallel Add with Signed Saturation Byte |
| 0x19 | PSUBSB | Parallel Subtract with Signed Saturation Byte |
| 0x1A | PEXTLB | Parallel Extend Lower Byte |
| 0x1B | PPACB | Parallel Pack Byte |
| 0x1E | PEXT5 | Parallel Extend from 5-bit |
| 0x1F | PPAC5 | Parallel Pack to 5-bit |

### 4b. MMI1 Sub-Table — MMI function=0x28, bits [10:6]

| Hex | Name | Description |
|-----|------|-------------|
| 0x01 | PABSW | Parallel Absolute Word |
| 0x02 | PCEQW | Parallel Compare Equal Word |
| 0x03 | PMINW | Parallel Minimum Word |
| 0x04 | PADSBH | Parallel Add/Subtract Halfword |
| 0x05 | PABSH | Parallel Absolute Halfword |
| 0x06 | PCEQH | Parallel Compare Equal Halfword |
| 0x07 | PMINH | Parallel Minimum Halfword |
| 0x0A | PCEQB | Parallel Compare Equal Byte |
| 0x10 | PADDUW | Parallel Add Unsigned Word |
| 0x11 | PSUBUW | Parallel Subtract Unsigned Word |
| 0x12 | PEXTUW | Parallel Extend Upper Word |
| 0x14 | PADDUH | Parallel Add Unsigned Halfword |
| 0x15 | PSUBUH | Parallel Subtract Unsigned Halfword |
| 0x16 | PEXTUH | Parallel Extend Upper Halfword |
| 0x18 | PADDUB | Parallel Add Unsigned Byte |
| 0x19 | PSUBUB | Parallel Subtract Unsigned Byte |
| 0x1A | PEXTUB | Parallel Extend Upper Byte |
| 0x1B | QFSRV | Quadword Funnel Shift Right Variable |

### 4c. MMI2 Sub-Table — MMI function=0x09, bits [10:6]

| Hex | Name | Description |
|-----|------|-------------|
| 0x00 | PMADDW | Parallel Multiply-Add Word |
| 0x02 | PSLLVW | Parallel Shift Left Logical Variable Word |
| 0x03 | PSRLVW | Parallel Shift Right Logical Variable Word |
| 0x04 | PMSUBW | Parallel Multiply-Subtract Word |
| 0x08 | PMFHI | Parallel Move From HI |
| 0x09 | PMFLO | Parallel Move From LO |
| 0x0A | PINTH | Parallel Interleave Halfword |
| 0x0C | PMULTW | Parallel Multiply Word |
| 0x0D | PDIVW | Parallel Divide Word |
| 0x0E | PCPYLD | Parallel Copy Lower Doubleword |
| 0x12 | PAND | Parallel AND |
| 0x13 | PXOR | Parallel XOR |
| 0x14 | PMADDH | Parallel Multiply-Add Halfword |
| 0x15 | PHMADH | Parallel Horizontal Multiply-Add Halfword |
| 0x18 | PMSUBH | Parallel Multiply-Subtract Halfword |
| 0x19 | PHMSBH | Parallel Horizontal Multiply-Subtract Halfword |
| 0x1A | PEXEH | Parallel Exchange Even Halfword |
| 0x1B | PREVH | Parallel Reverse Halfword |
| 0x1C | PMULTH | Parallel Multiply Halfword |
| 0x1D | PDIVBW | Parallel Divide Broadcast Word |
| 0x1E | PEXEW | Parallel Exchange Even Word |
| 0x1F | PROT3W | Parallel Rotate 3 Words |

### 4d. MMI3 Sub-Table — MMI function=0x29, bits [10:6]

| Hex | Name | Description |
|-----|------|-------------|
| 0x00 | PMADDUW | Parallel Multiply-Add Unsigned Word |
| 0x03 | PSRAVW | Parallel Shift Right Arithmetic Variable Word |
| 0x08 | PMTHI | Parallel Move To HI |
| 0x09 | PMTLO | Parallel Move To LO |
| 0x0A | PINTEH | Parallel Interleave Even Halfword |
| 0x0C | PMULTUW | Parallel Multiply Unsigned Word |
| 0x0D | PDIVUW | Parallel Divide Unsigned Word |
| 0x0E | PCPYUD | Parallel Copy Upper Doubleword |
| 0x12 | POR | Parallel OR |
| 0x13 | PNOR | Parallel NOR |
| 0x1A | PEXCH | Parallel Exchange Center Halfword |
| 0x1B | PCPYH | Parallel Copy Halfword |
| 0x1E | PEXCW | Parallel Exchange Center Word |

---

## 5. COP0 (System Control) — opcode=0x10

### Format Field — bits [25:21]
| Hex | Name | Next decode |
|-----|------|-------------|
| 0x00 | MFC0 | Move From COP0: `rd` = COP0 register |
| 0x04 | MTC0 | Move To COP0: `rd` = COP0 register |
| 0x08 | BC0 | → BC condition (bits [20:16]) |
| 0x10 | CO | → CO function (bits [5:0]) |

### BC0 Conditions — bits [20:16]
| Hex | Name |
|-----|------|
| 0x00 | BC0F (False) |
| 0x01 | BC0T (True) |
| 0x02 | BC0FL (False Likely) |
| 0x03 | BC0TL (True Likely) |

### CO Functions — bits [5:0]
| Hex | Name | Description |
|-----|------|-------------|
| 0x01 | TLBR | TLB Read |
| 0x02 | TLBWI | TLB Write Indexed |
| 0x06 | TLBWR | TLB Write Random |
| 0x08 | TLBP | TLB Probe |
| 0x18 | ERET | Return from Exception |
| **0x38** | **EI** | **Enable Interrupts (PS2)** |
| **0x39** | **DI** | **Disable Interrupts (PS2)** |

### COP0 Register Numbers (for MFC0/MTC0 `rd` field)
| # | Name | Description |
|---|------|-------------|
| 0 | Index | TLB array index |
| 1 | Random | Random TLB index |
| 2 | EntryLo0 | TLB entry low (even pages) |
| 3 | EntryLo1 | TLB entry low (odd pages) |
| 4 | Context | Kernel virtual address recovery |
| 5 | PageMask | TLB page size mask |
| 6 | Wired | TLB wired boundary |
| 8 | BadVAddr | Bad virtual address |
| 9 | Count | Timer count |
| 10 | EntryHi | TLB entry high |
| 11 | Compare | Timer compare / interrupt |
| 12 | Status | Processor status and control |
| 13 | Cause | Exception cause |
| 14 | EPC | Exception program counter |
| 15 | PRId | Processor revision ID (R5900 = 0x2E20) |
| 16 | Config | Configuration |
| 23 | BadPAddr | Bad physical address (PS2) |
| 24 | Debug | Debug register (PS2) |
| 25 | Perf | Performance counter (PS2) |
| 28 | TagLo | Cache TagLo |
| 29 | TagHi | Cache TagHi |
| 30 | ErrorEPC | Error exception program counter |

---

## 6. COP1 (FPU) — opcode=0x11

> [!NOTE]
> PS2 FPU is **single-precision only**. Double-precision ops (D format, L format) are defined but unused.

### Format Field — bits [25:21]
| Hex | Name | Next decode |
|-----|------|-------------|
| 0x00 | MFC1 | Move From FPU Register |
| 0x02 | CFC1 | Move From FPU Control Register |
| 0x04 | MTC1 | Move To FPU Register |
| 0x06 | CTC1 | Move To FPU Control Register |
| 0x08 | BC1 | → BC condition (bits [20:16]) |
| 0x10 | S | → Single-Precision function (bits [5:0]) |
| 0x14 | W | → Word/Integer function (bits [5:0]) |

### BC1 Conditions — bits [20:16]
| Hex | Name |
|-----|------|
| 0x00 | BC1F (False) |
| 0x01 | BC1T (True) |
| 0x02 | BC1FL (False Likely) |
| 0x03 | BC1TL (True Likely) |

### S Format Functions — bits [5:0]
| Hex | Name | Description | PS2? |
|-----|------|-------------|------|
| 0x00 | ADD.S | Add | |
| 0x01 | SUB.S | Subtract | |
| 0x02 | MUL.S | Multiply | |
| 0x03 | DIV.S | Divide | |
| 0x04 | SQRT.S | Square Root | |
| 0x05 | ABS.S | Absolute Value | |
| 0x06 | MOV.S | Move | |
| 0x07 | NEG.S | Negate | |
| 0x0C | ROUND.W.S | Round to Word | |
| 0x0D | TRUNC.W.S | Truncate to Word | |
| 0x0E | CEIL.W.S | Ceiling to Word | |
| 0x0F | FLOOR.W.S | Floor to Word | |
| **0x16** | **RSQRT.S** | **Reciprocal Square Root** | **PS2** |
| **0x18** | **ADDA.S** | **Add to Accumulator** | **PS2** |
| **0x19** | **SUBA.S** | **Subtract from Accumulator** | **PS2** |
| **0x1A** | **MULA.S** | **Multiply to Accumulator** | **PS2** |
| **0x1C** | **MADD.S** | **Multiply-Add** | **PS2** |
| **0x1D** | **MSUB.S** | **Multiply-Subtract** | **PS2** |
| **0x1E** | **MADDA.S** | **Multiply-Add to Accumulator** | **PS2** |
| **0x1F** | **MSUBA.S** | **Multiply-Subtract from Accumulator** | **PS2** |
| 0x24 | CVT.W.S | Convert Float to Word Integer | |
| **0x28** | **MAX.S** | **Maximum** | **PS2** |
| **0x29** | **MIN.S** | **Minimum** | **PS2** |
| 0x30 | C.F.S | Compare False | |
| 0x32 | C.EQ.S | Compare Equal | |
| 0x34 | C.OLT.S | Compare Ordered Less Than | |
| 0x36 | C.OLE.S | Compare Ordered Less Equal | |
| 0x3C | C.LT.S | Compare Less Than (signaling) | |
| 0x3E | C.LE.S | Compare Less Equal (signaling) | |

### W Format Functions — bits [5:0]
| Hex | Name | Description |
|-----|------|-------------|
| 0x20 | CVT.S.W | Convert Word Integer to Float |

---

## 7. COP2 (VU0 Macro Mode) — opcode=0x12

### Format Field — bits [25:21]
| Hex | Name | Description |
|-----|------|-------------|
| 0x01 | QMFC2 | Move Quadword From VU0 Register |
| 0x02 | CFC2 | Move From VU0 Control Register |
| 0x05 | QMTC2 | Move Quadword To VU0 Register |
| 0x06 | CTC2 | Move To VU0 Control Register |
| 0x08 | BC2 | → BC2 condition (bits [20:16]) |
| 0x10+ | CO | → VU0 macro operations (bit 25=1) |

### BC2 Conditions — bits [20:16]
| Hex | Name |
|-----|------|
| 0x00 | BC2F |
| 0x01 | BC2T |
| 0x02 | BC2FL |
| 0x03 | BC2TL |

### VU0 Macro Special1 — CO format, bits [5:0] (when bit 0 of opcode2 = 0)

| Hex | Name | Hex | Name |
|-----|------|-----|------|
| 0x00 | VADDx | 0x01 | VADDy |
| 0x02 | VADDz | 0x03 | VADDw |
| 0x04 | VSUBx | 0x05 | VSUBy |
| 0x06 | VSUBz | 0x07 | VSUBw |
| 0x08 | VMADDx | 0x09 | VMADDy |
| 0x0A | VMADDz | 0x0B | VMADDw |
| 0x0C | VMSUBx | 0x0D | VMSUBy |
| 0x0E | VMSUBz | 0x0F | VMSUBw |
| 0x10 | VMAXx | 0x11 | VMAXy |
| 0x12 | VMAXz | 0x13 | VMAXw |
| 0x14 | VMINIx | 0x15 | VMINIy |
| 0x16 | VMINIz | 0x17 | VMINIw |
| 0x18 | VMULx | 0x19 | VMULy |
| 0x1A | VMULz | 0x1B | VMULw |
| 0x1C | VMULq | 0x1D | VMAXi |
| 0x1E | VMULi | 0x1F | VMINIi |
| 0x20 | VADDq | 0x21 | VMADDq |
| 0x22 | VADDi | 0x23 | VMADDi |
| 0x24 | VSUBq | 0x25 | VMSUBq |
| 0x26 | VSUBi | 0x27 | VMSUBi |
| 0x28 | VADD | 0x29 | VMADD |
| 0x2A | VMUL | 0x2B | VMAX |
| 0x2C | VSUB | 0x2D | VMSUB |
| 0x2E | VOPMSUB | 0x2F | VMINI |
| 0x30 | VIADD | 0x31 | VISUB |
| 0x32 | VIADDI | 0x34 | VIAND |
| 0x35 | VIOR | 0x38 | VCALLMS |
| 0x39 | VCALLMSR | | |

### VU0 Macro Special2 — CO format, bits [5:0] (when bit 0 of opcode2 = 1)

| Hex | Name | Hex | Name |
|-----|------|-----|------|
| 0x00 | VADDAx | 0x01 | VADDAy |
| 0x02 | VADDAz | 0x03 | VADDAw |
| 0x04 | VSUBAx | 0x05 | VSUBAy |
| 0x06 | VSUBAz | 0x07 | VSUBAw |
| 0x08 | VMADDAx | 0x09 | VMADDAy |
| 0x0A | VMADDAz | 0x0B | VMADDAw |
| 0x0C | VMSUBAx | 0x0D | VMSUBAy |
| 0x0E | VMSUBAz | 0x0F | VMSUBAw |
| 0x10 | VITOF0 | 0x11 | VITOF4 |
| 0x12 | VITOF12 | 0x13 | VITOF15 |
| 0x14 | VFTOI0 | 0x15 | VFTOI4 |
| 0x16 | VFTOI12 | 0x17 | VFTOI15 |
| 0x18 | VMULAx | 0x19 | VMULAy |
| 0x1A | VMULAz | 0x1B | VMULAw |
| 0x1C | VMULAq | 0x1D | VABS |
| 0x1E | VMULAi | 0x1F | VCLIPw |
| 0x20 | VADDAq | 0x21 | VMADDAq |
| 0x22 | VADDAi | 0x23 | VMADDAi |
| 0x24 | VSUBAq | 0x25 | VMSUBAq |
| 0x26 | VSUBAi | 0x27 | VMSUBAi |
| 0x28 | VADDA | 0x29 | VMADDA |
| 0x2A | VMULA | 0x2C | VSUBA |
| 0x2D | VMSUBA | 0x2E | VOPMULA |
| 0x2F | VNOP | 0x30 | VMOVE |
| 0x31 | VMR32 | 0x34 | VLQI |
| 0x35 | VSQI | 0x36 | VLQD |
| 0x37 | VSQD | 0x38 | VDIV |
| 0x39 | VSQRT | 0x3A | VRSQRT |
| 0x3B | VWAITQ | 0x3C | VMTIR |
| 0x3D | VMFIR | 0x3E | VILWR |
| 0x3F | VISWR | 0x40 | VRNEXT |
| 0x41 | VRGET | 0x42 | VRINIT |
| 0x43 | VRXOR | | |

### VU0 Control Registers (for CFC2/CTC2)
| # | Name | Description |
|---|------|-------------|
| 0 | Status | Status/Control flags |
| 1 | MAC | MAC flags (OxOyOzOw UxUyUzUw SxSySzSw ZxZyZzZw) |
| 2 | VPU-STAT | VU pipeline status |
| 3 | R | Random number register |
| 4 | I | Immediate value register |
| 5 | Clip | Clipping flags |
| 6 | TPC | Program counter |
| 7 | CMSAR0 | Call/return address |
| 8 | FBRST | VIF/VU reset |
| 20 | ACC | Accumulator |
| 26 | P | P register |
| 27 | XITOP | XITOP register |
| 28 | ITOP | ITOP register |
| 29 | TOP | TOP register |

### VU0 Instruction Field Decoding
| Macro | Expression | Purpose |
|-------|-----------|---------|
| `VU_DEST(i)` | `(i >> 16) & 0xF` | Destination field mask (xyzw) |
| `VU_FD(i)` | `(i >> 11) & 0x1F` | Destination vector register |
| `VU_FS(i)` | `(i >> 6) & 0x1F` | Source vector register |
| `VU_FT(i)` | `i & 0x1F` | Target vector register |
| `VU_FSF(i)` | `(i >> 10) & 0x3` | Source field selector (x=0,y=1,z=2,w=3) |
| `VU_FTF(i)` | `(i >> 8) & 0x3` | Target field selector |
| `VU_IS(i)` | `RT(i)` | Source integer register |
| `VU_IT(i)` | `RD(i)` | Target integer register |
| `VU_ID(i)` | `SA(i)` | Destination integer register |
| `VU_IMM11(i)` | `i & 0x7FF` | 11-bit immediate |
| `VU_IMM15(i)` | `i & 0x7FFF` | 15-bit immediate |

---

## 8. GPR Register Map (128-bit wide on PS2)

| # | Name | Convention | Preserved? |
|---|------|-----------|-----------|
| 0 | $zero | Hard-wired zero | — |
| 1 | $at | Assembler temporary | No |
| 2–3 | $v0–$v1 | Return values | No |
| 4–7 | $a0–$a3 | Arguments 1–4 | No |
| 8–11 | $t0–$t3 | Arguments 5–8 (PS2 extension) | No |
| 12–15 | $t4–$t7 | Temporaries | No |
| 16–23 | $s0–$s7 | Callee-saved | **Yes** |
| 24–25 | $t8–$t9 | Temporaries | No |
| 26–27 | $k0–$k1 | Kernel reserved | — |
| 28 | $gp | Global pointer | **Yes** |
| 29 | $sp | Stack pointer | **Yes** |
| 30 | $fp/$s8 | Frame pointer | **Yes** |
| 31 | $ra | Return address | No |

> [!NOTE]
> All GPRs are **128-bit wide**. Lower 64 bits are standard MIPS III; upper 64 bits used by MMI/parallel instructions.
> PS2 uses $t0–$t3 as extra argument registers (args 5–8), unlike standard MIPS O32.
> Runtime access: `ctx.gpr[N].words[0]` for 32-bit word, `ctx.gpr[N]` for full 128-bit.

---

## 9. PS2-Specific Instruction Summary

All instructions unique to the R5900 that do NOT exist in standard MIPS III/IV:

### Core Extensions
- **LQ/SQ** (0x1E/0x1F) — 128-bit load/store, 16-byte aligned
- **LQC2/SQC2** (0x36/0x3E) — 128-bit load/store to/from VU0
- **MOVZ/MOVN** — Conditional moves (SPECIAL 0x0A/0x0B)
- **MFSA/MTSA** — Shift Amount register (SPECIAL 0x28/0x29)
- **MTSAB/MTSAH** — SA register byte/halfword (REGIMM 0x18/0x19)
- **EI/DI** — Enable/Disable Interrupts (COP0 CO 0x38/0x39)

### Second Pipeline (HI1/LO1)
- **MULT1/MULTU1/DIV1/DIVU1** — duplicate mul/div using HI1/LO1
- **MADD1/MADDU1** — multiply-add to HI1/LO1
- **MFHI1/MTHI1/MFLO1/MTLO1** — HI1/LO1 register moves

### FPU Extensions (COP1 S format)
- **RSQRT.S, ADDA.S, SUBA.S, MULA.S** — accumulator ops
- **MADD.S, MSUB.S, MADDA.S, MSUBA.S** — fused multiply-add
- **MAX.S, MIN.S** — float min/max

### Full MMI Set (opcode 0x1C)
All 80+ parallel SIMD instructions operating on 128-bit registers — see sections 4, 4a–4d above.

### VU0 Macro Mode (COP2)
All vector operations executing on VU0 through the EE pipeline — see section 7 above.
