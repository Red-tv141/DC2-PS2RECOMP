# PS2 VU (Vector Unit) Instruction Set Reference
> **Sources**: VU User's Manual, VU Instruction Manual, ps2tek.md VU section
>
> **See also**: `db-isa.md` (COP2 macro-mode VU0 instructions), `db-ps2-architecture.md` §6 (VU pipeline + hazards), `db-registers.md` (VIF register addresses).
> The PS2 has two VUs: VU0 (4 KB micro/data mem, also COP2) and VU1 (16 KB micro/data mem, has EFU+XGKICK).
> Each VU executes **two instructions per cycle**: one **upper** (FMAC/float) and one **lower** (int/branch/load-store/special).
> A 64-bit doubleword = upper[63:32] | lower[31:0].

---

## Instruction Encoding — Upper Pipeline

```
 31 30 29 28 27  26                                0
 [ I| E| M| D| T|         instruction               ]
```

| Bit | Name | Description |
|-----|------|-------------|
| 31 | I | **I-bit**: Load lower 32 bits into I register (lower instr ignored) |
| 30 | E | **E-bit**: End microprogram (has delay slot like branches) |
| 29 | M | **M-bit**: (VU0 only) Release interlock on QMTC2.I / CTC2.I |
| 28 | D | **D-bit**: Debug break — halts VU, sends interrupt to EE |
| 27 | T | **T-bit**: Debug halt — similar to D |

### Upper Instruction Encoding Formats

**Standard (fd, fs, ft, dest)**
```
 26   21 20  16 15  11 10   6 5        0
 [  fd  | ft  | fs  | dest | opcode   ]
```

**BC (broadcast): operates on a single component of ft**
```
 26   21 20  16 15  11 10   6 5        0
 [  fd  | ft  | fs  | dest | bc | op  ]
```
bc: 00=x, 01=y, 10=z, 11=w

**dest field** (4 bits): x=bit 3, y=bit 2, z=bit 1, w=bit 0.
Controls which vector fields are written.

---

## Instruction Encoding — Lower Pipeline

Lower instructions have multiple formats depending on category:

**Register format**
```
 31       26 25  21 20  16 15  11 10   6 5        0
 [ special  | id  | it  | is  | dest | opcode   ]
```

**Immediate (11-bit)**
```
 31       26 25       21 20  16 15  11 10       0
 [ special  | imm11_hi | it  | is  | imm11_lo ]
```

**Branch (11-bit signed offset)**
```
 31       26 25  21 20  16 15  11 10       0
 [ special  |  —  | it  | is  | imm11    ]
```

---

## Upper Pipeline Instructions (Floating-Point / FMAC)

### Arithmetic — Standard (fd = fs OP ft)

| Mnemonic | Operation | Latency | Notes |
|----------|-----------|---------|-------|
| **VADDdest** | fd = fs + ft | 4 | Per-field with dest mask |
| **VSUBdest** | fd = fs − ft | 4 | |
| **VMULdest** | fd = fs × ft | 4 | |
| **VMAXdest** | fd = max(fs, ft) | 4 | Signed compare |
| **VMINIdest** | fd = min(fs, ft) | 4 | Signed compare |

### Arithmetic — Accumulator Destination (ACC = fs OP ft)

| Mnemonic | Operation | Latency | Notes |
|----------|-----------|---------|-------|
| **VADDAdest** | ACC = fs + ft | 4 | |
| **VSUBAdest** | ACC = fs − ft | 4 | |
| **VMULAdest** | ACC = fs × ft | 4 | |

### Multiply-Add / Multiply-Subtract

| Mnemonic | Operation | Latency | Notes |
|----------|-----------|---------|-------|
| **VMADDdest** | fd = ACC + fs × ft | 4 | |
| **VMSUBdest** | fd = ACC − fs × ft | 4 | |
| **VMADDAdest** | ACC = ACC + fs × ft | 4 | |
| **VMSUBAdest** | ACC = ACC − fs × ft | 4 | |

### Broadcast Variants (ft.bc)

All standard arithmetic ops have broadcast (BC) variants that use a **single component** of ft:

| Suffix | ft source | Example |
|--------|-----------|---------|
| **x** | ft.x broadcast to all | VADDx, VMULx, VMADDx, etc. |
| **y** | ft.y broadcast to all | VADDy, VMULy, VMADDy, etc. |
| **z** | ft.z broadcast to all | VADDz, VMULz, VMADDz, etc. |
| **w** | ft.w broadcast to all | VADDw, VMULw, VMADDw, etc. |

Broadcast applies to: VADD, VSUB, VMUL, VMADD, VMSUB, VMAX, VMINI, and their A-variants (VADDA, VSUBA, VMULA, VMADDA, VMSUBA).

> **Note**: VMAX and VMINI also have **I-register variants** (VMAXi, VMINIi) but do NOT have Q-register variants. Accumulator variants (A-suffix) do not apply to VMAX/VMINI.

### I Register Variants (ft = I)

| Mnemonic | Operation | Notes |
|----------|-----------|-------|
| **VADDidest** | fd = fs + I | I is 32-bit float loaded via I-bit |
| **VSUBidest** | fd = fs − I | |
| **VMULidest** | fd = fs × I | |
| **VMADDidest** | fd = ACC + fs × I | |
| **VMSUBidest** | fd = ACC − fs × I | |

And their A-variants: VADDAi, VSUBAi, VMULAi, VMADDAi, VMSUBAi.

### Q Register Variants (ft = Q)

| Mnemonic | Operation | Notes |
|----------|-----------|-------|
| **VADDqdest** | fd = fs + Q | Q from FDIV result |
| **VSUBqdest** | fd = fs − Q | |
| **VMULqdest** | fd = fs × Q | |
| **VMADDqdest** | fd = ACC + fs × Q | |
| **VMSUBqdest** | fd = ACC − fs × Q | |

And A-variants: VADDAq, VSUBAq, VMULAq, VMADDAq, VMSUBAq.

### Other Upper Instructions

| Mnemonic | Operation | Latency | Notes |
|----------|-----------|---------|-------|
| **VABSdest** | fd = abs(fs) | 4 | |
| **VFTOIdest** | fd = float_to_int(fs) | 4 | Truncation to 15.0 fixed |
| **VITOFdest** | fd = int_to_float(fs) | 4 | 15.0 fixed to float |
| **VFTOI0dest** | fd = float_to_int(fs × 1) | 4 | 12.4 → 15.0 |
| **VFTOI4dest** | fd = float_to_int(fs × 16) | 4 | Scale by 2^4 |
| **VFTOI12dest** | fd = float_to_int(fs × 4096) | 4 | Scale by 2^12 |
| **VFTOI15dest** | fd = float_to_int(fs × 32768) | 4 | Scale by 2^15 |
| **VITOF0dest** | fd = int_to_float(fs ÷ 1) | 4 | |
| **VITOF4dest** | fd = int_to_float(fs ÷ 16) | 4 | |
| **VITOF12dest** | fd = int_to_float(fs ÷ 4096) | 4 | |
| **VITOF15dest** | fd = int_to_float(fs ÷ 32768) | 4 | |
| **VCLIPw** | CLIP test fs.xyz against ±ft.w | 4 | Updates clip flags |
| **VNOP** | No operation (upper) | — | |
| **VOPMULA** | ACC = fs × ft (outer product partial) | 4 | Uses xyz cross-product pattern |
| **VOPMSUB** | fd = ACC − fs × ft (outer product) | 4 | Completes cross-product |

---

## Lower Pipeline Instructions

### Integer Arithmetic

| Mnemonic | Operation | Latency | Notes |
|----------|-----------|---------|-------|
| **VIADD** | id = is + it | 1* | 16-bit integer regs |
| **VIADDI** | id = is + imm5 | 1* | 5-bit signed immediate |
| **VIADDIU** | id = is + imm15 | 1* | 15-bit unsigned immediate |
| **VISUB** | id = is − it | 1* | |
| **VISUBIU** | id = is − imm15 | 1* | 15-bit unsigned immediate |
| **VIAND** | id = is & it | 1* | Bitwise AND |
| **VIOR** | id = is \| it | 1* | Bitwise OR |

> *Integer pipeline has 1-cycle latency due to bypass, despite 4-stage pipeline.

### Branching (11-bit signed offset, has delay slot)

| Mnemonic | Condition | Notes |
|----------|-----------|-------|
| **VB** | Always | Unconditional branch (PC-relative) |
| **VBAL** | Always | Branch and link (vi15 = return addr) |
| **VIBEQ** | is == it | Branch if equal |
| **VIBNE** | is != it | Branch if not equal |
| **VIBGEZ** | is ≥ 0 | Branch if ≥ 0 (signed) |
| **VIBGTZ** | is > 0 | Branch if > 0 (signed) |
| **VIBLEZ** | is ≤ 0 | Branch if ≤ 0 (signed) |
| **VIBLTZ** | is < 0 | Branch if < 0 (signed) |
| **VJALR** | Always | Jump and link register (vi15 = return, jump to it) |
| **VJR** | Always | Jump register (jump to it) |

> All branches have a **one-instruction delay slot**. Branch targets are word addresses (PC + offset × 8).

### Load / Store (VU Data Memory)

| Mnemonic | Operation | Pipeline | Notes |
|----------|-----------|----------|-------|
| **VLQ.dest** | fd = mem[is + imm11] | FMAC (4) | Load quadword (128-bit) |
| **VLQDdest** | fd = mem[--is] | FMAC (4) | Load quadword, pre-decrement |
| **VLQIdest** | fd = mem[is++] | FMAC (4) | Load quadword, post-increment |
| **VSQ.dest** | mem[it + imm11] = fs | 4 | Store quadword |
| **VSQDdest** | mem[it--] = fs | 4 | Store quadword, pre-decrement |
| **VSQIdest** | mem[it++] = fs | 4 | Store quadword, post-increment |
| **VILW.dest** | it = mem[is + imm11] | FMAC (4) | Load integer from vector field |
| **VISW.dest** | mem[is + imm11] = it | 4 | Store integer to vector field |
| **VILWR.dest** | it = mem[is] | FMAC (4) | Load integer, rotate-access |
| **VISWR.dest** | mem[is] = it | 4 | Store integer, rotate-access |
| **VILFP** | id = (flags) | — | Load flags from I register |

> Memory addresses for VU0: 0–1023 words (4 KB), VU1: 0–4095 words (16 KB).
> dest mask on loads/stores controls which 32-bit fields are accessed.

### FDIV Pipeline (Division/Square Root)

| Mnemonic | Operation | Latency | Result |
|----------|-----------|---------|--------|
| **VDIV** | Q = fs.fsf ÷ ft.ftf | 7 | Q register |
| **VSQRT** | Q = √(ft.ftf) | 7 | Q register |
| **VRSQRT** | Q = fs.fsf ÷ √(ft.ftf) | 13 | Q register |
| **VWAITQ** | Stall until Q ready | 0–12 | Sync point |

> fsf/ftf = 2-bit field selector (00=x, 01=y, 10=z, 11=w). Only one FDIV op at a time.

### EFU Pipeline (VU1 Only — Elementary Functions)

| Mnemonic | Operation | Latency | Result |
|----------|-----------|---------|--------|
| **VEATANXY** | P = atan(fs.x / fs.y) | 54 | P register |
| **VEATANXZ** | P = atan(fs.x / fs.z) | 54 | P register |
| **VEATAN** | P = atan(fs.fsf) | 54 | P register |
| **VEEXP** | P = exp(−fs.fsf) | 44 | P register |
| **VELENG** | P = √(fs.x² + fs.y² + fs.z²) | 18 | P register |
| **VERCPR** | P = 1 / fs.fsf | 12 | P register |
| **VERLENG** | P = 1 / √(fs.x² + fs.y² + fs.z²) | 24 | P register |
| **VERSADD** | P = 1 / (fs.x² + fs.y² + fs.z²) | 18 | P register |
| **VERSQRT** | P = 1 / √(fs.fsf) | 18 | P register |
| **VESADD** | P = fs.x² + fs.y² + fs.z² | 11 | P register |
| **VESIN** | P = sin(fs.fsf) | 29 | P register |
| **VESQRT** | P = √(fs.fsf) | 12 | P register |
| **VESUM** | P = fs.x + fs.y + fs.z + fs.w | 12 | P register |
| **VWAITP** | Stall until P ready | 0–53 | Sync point |

### Flag / Status Instructions

| Mnemonic | Operation | Notes |
|----------|-----------|-------|
| **VFCAND** | vi01 = (clip_flags & imm24) != 0 | Test clip flags with AND |
| **VFCEQ** | vi01 = (clip_flags == imm24) | Test clip flags equality |
| **VFCOR** | vi01 = (clip_flags \| imm24) == 0xFFFFFF | Test clip flags with OR |
| **VFCSET** | clip_flags = imm24 | Set clip flags |
| **VFCGET** | id = clip_flags | Get clip flags |
| **VFMAND** | id = (mac_flags & is) | Test MAC flags with AND |
| **VFMEQ** | id = (mac_flags == is) | Test MAC flags equality |
| **VFMOR** | id = (mac_flags \| is) | Test MAC flags with OR |
| **VFSAND** | id = (status_flags & imm12) | Test status flags with AND |
| **VFSEQ** | id = (status_flags == imm12) | Test status flags equality |
| **VFSOR** | id = (status_flags \| imm12) | Test status flags with OR |
| **VFSSET** | status_flags = imm12 | Set status flags |

### Data Move Instructions

| Mnemonic | Operation | Pipeline | Notes |
|----------|-----------|----------|-------|
| **VMOVE.dest** | fd = fs | FMAC (4) | Vector register copy |
| **VMR32.dest** | fd = fs rotated right 32 bits | FMAC (4) | {x,y,z,w} → {w,x,y,z} |
| **VMFIR.dest** | fd = sign_extend(is) | FMAC (4) | Int → float register (broadcast) |
| **VMTIR** | it = fs.fsf[15:0] | 1* | Float → int register |
| **VMFIRit** | fd.dest = sign_extend(is) | FMAC (4) | Alternate encoding |

### Register Transfer (COP2 / EE ↔ VU0 Macro Mode)

| Mnemonic | Operation | Notes |
|----------|-----------|-------|
| **QMFC2** | GPR = VF (COP2 move from) | EE macro mode only |
| **QMTC2** | VF = GPR (COP2 move to) | EE macro mode only |
| **CFC2** | GPR = VI (COP2 control from) | Access VU0 int reg via COP2 |
| **CTC2** | VI = GPR (COP2 control to) | Access VU0 int reg via COP2 |

### Special / Misc Lower Instructions

| Mnemonic | Operation | Notes |
|----------|-----------|-------|
| **VNOP** | No operation (lower) | |
| **VXITOP** | it = ITOP register | Read VIF ITOP value |
| **VXTOP** | it = TOP register | Read VIF TOP value (VU1 only) |
| **VXGKICK** | Start GIF PATH1 transfer at is | VU1 only, concurrent with VU |
| **VRGET.dest** | fd = R register | Read random number register |
| **VRINIT** | R = fs.fsf | Init random seed |
| **VRNDNEXT** | R = next_random(R) | Generate next random |
| **VRXOR** | R = R ^ fs.fsf | XOR random seed |
| **VMFP.dest** | fd = P | Move from P register (EFU result) |
| **VMTIR** | it = fs.fsf | Move to integer register |
| **VMFIR.dest** | fd = sign_extend(is) | Move from integer register |
| **VLOI** | I = imm32 (from upper I-bit) | Load immediate to I register |

---

## Pipeline Summary

| Pipeline | Used By | Read Stage | Write Stage | Max Stall |
|----------|---------|------------|-------------|-----------|
| **FMAC** | Upper arith, loads/stores | T (stage 2) | Z (stage 5) | 3 cycles |
| **Integer** | Lower int ops | T (stage 2) | T+bypass | 0 (1-cycle effective) |
| **FDIV** | DIV, SQRT | — | After 7/13 cycles | 6/12 cycles |
| **EFU** | Elementary funcs (VU1) | — | After N cycles | N−1 cycles |

### Hazard Notes
- RAW hazards on VF registers cause FMAC stalls (up to 3 cycles)
- **No hazard check** on: ACC, VF00, I, Q, P, R registers
- Hazard checks are **per-field**: writing vf01.y then reading vf01.x = no stall
- MAC flags are written in pipeline stage Z (writeback) = 4-cycle delay
- Clip flags are also pipelined and delayed
- Integer pipeline has bypass: 1-cycle effective latency

### VU Register Summary

| Register | Count | Width | Description |
|----------|-------|-------|-------------|
| VF00–VF31 | 32 | 128-bit (4×float32) | Vector float regs (VF00 = {0,0,0,1.0}) |
| VI00–VI15 | 16 | 16-bit | Integer regs (VI00 = 0, VI15 = link reg) |
| ACC | 1 | 128-bit | Accumulator (same format as VF) |
| Q | 1 | 32-bit float | FDIV result register |
| P | 1 | 32-bit float | EFU result register (VU1 only) |
| I | 1 | 32-bit float | Immediate value (loaded via I-bit) |
| R | 1 | 32-bit float | Random number register |
| MAC flags | 1 | 16-bit | {Ox,Oy,Oz,Ow,Ux,Uy,Uz,Uw,Sx,Sy,Sz,Sw,Zx,Zy,Zz,Zw} |
| Clip flags | 1 | 24-bit | 4 sets of {-z,+z,-y,+y,-x,+x} |
| Status flags | 1 | 12-bit | {DS,IS,OS,US,SS,ZS,D,I,O,U,S,Z} |
| TPC | 1 | — | Target program counter |
| CMSAR | 1 | — | Micro subroutine address |

### VU Memory

| | VU0 | VU1 |
|---|-----|-----|
| Micro memory | 4 KB (0x000–0xFFF) | 16 KB (0x000–0x3FFF) |
| Data memory | 4 KB (0x000–0xFFF) | 16 KB (0x000–0x3FFF) |
| Data mem base (EE map) | 0x11004000 | 0x1100C000 |
| Micro mem base (EE map) | 0x11000000 | 0x11008000 |

### VU0 Macro Mode (COP2)

When VU0 runs in macro mode, VU0 instructions are issued inline from the EE Core as COP2 instructions:
- Upper pipeline instructions use `COP2` opcode encoding
- Lower pipeline instructions are accessed through `COP2` special function codes
- VF registers accessed via `QMFC2`/`QMTC2`
- VI registers accessed via `CFC2`/`CTC2`
- Macro mode instructions **stall the EE pipeline** (no concurrent execution)
- Instructions that only exist in micro mode (branches, XGKICK, EFU) are not available
