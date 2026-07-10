# PS2 Knowledge Base — Master Index
> **The LLM looks here FIRST.** Every PS2 topic maps to a file and section.
>
> Files live in `resources/` directory. Numbered files (01–20) are narrative references & playbooks; `db-*.md` are structured knowledge bases; `appendix-*.md` are project-specific.

---

## Quick Lookup

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **System overview / block diagram** | `db-ps2-architecture.md` | §1 System-Level Block Diagram |
| **Bus bandwidths** | `db-ps2-architecture.md` | §1 Key Bandwidths |
| **EE Core pipeline / dual-issue** | `db-ps2-architecture.md` | §2 EE Core Pipeline |
| **Branch prediction / delay slots** | `db-ps2-architecture.md` | §2 Branch Prediction |
| **Cache / Scratchpad** | `db-ps2-architecture.md` | §2 Cache Architecture |
| **DMA channels map** | `db-ps2-architecture.md` | §3 DMA Controller |
| **DMA tag format** | `db-ps2-architecture.md` | §3 DMA Tag Format |
| **DMA transfer modes** | `db-ps2-architecture.md` | §3 DMA Transfer Modes |
| **Interrupts / INTC** | `db-ps2-architecture.md` | §4 Interrupt Controller |
| **GS rendering pipeline** | `db-ps2-architecture.md` | §5 GS Rendering Pipeline |
| **GS pixel formats (PSM)** | `db-ps2-architecture.md` | §5 GS Pixel Storage Formats |
| **VRAM layout / swizzle** | `db-ps2-architecture.md` | §5 VRAM Block Layout |
| **GIF paths (PATH1/2/3)** | `db-ps2-architecture.md` | §5 GIF Data Paths |
| **GS drawing primitives** | `db-ps2-architecture.md` | §5 GS Drawing Primitives |
| **VU0 vs VU1** | `db-ps2-architecture.md` | §6 VU Architecture |
| **VU micro pipeline + hazards** | `db-ps2-architecture.md` | §6 VU Micro Pipeline |
| **VIF UNPACK formats** | `db-ps2-architecture.md` | §6 VIF Unpack Formats |
| **SPU2 / sound** | `db-ps2-architecture.md` | §7 SPU2 |
| **ADPCM format** | `db-ps2-architecture.md` | §7 SPU2 ADPCM Format |
| **SPU2 KEY ON/OFF, ENDX, AutoDMA** | `db-ps2-architecture.md` | §7 SPU2 Voice Control & DMA |
| **SPU2 effect area / reverb** | `db-ps2-architecture.md` | §7 SPU2 Voice Control & DMA |
| **IPU (MPEG decoder)** | `db-ps2-architecture.md` | §13 IPU |
| **IOP subsystem** | `db-ps2-architecture.md` | §8 IOP Subsystem |
| **SIF (EE↔IOP)** | `db-ps2-architecture.md` | §8 SIF |
| **Exception vectors** | `db-ps2-architecture.md` | §9 Exception Vectors |
| **Boot sequence** | `db-ps2-architecture.md` | §9 Boot Flow |
| **FPU quirks (non-IEEE)** | `db-ps2-architecture.md` | §10 FP Quirks |
| **Normal mapping technique** | `db-ps2-architecture.md` | §11 Normal Mapping |
| **Optimization patterns** | `db-ps2-architecture.md` | §12 Optimization Patterns |
| **Double buffering** | `db-ps2-architecture.md` | §12 DMA Double-Buffering |
| **Typical frame data flow** | `db-ps2-architecture.md` | §14 Game Data Flow |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **R5900 instructions** | `db-isa.md` | Full instruction tables |
| **Integer ALU instructions** | `db-isa.md` | CPU Integer Instructions |
| **MMI 128-bit SIMD** | `db-isa.md` | R5900 128-bit (MMI) Extensions |
| **Branch / jump instructions** | `db-isa.md` | Branch / Jump Instructions |
| **Load / store instructions** | `db-isa.md` | Load / Store Instructions |
| **COP0 instructions** | `db-isa.md` | COP0 Instructions |
| **COP1 (FPU) instructions** | `db-isa.md` | COP1 Instructions |
| **COP2 (VU0 macro) instructions** | `db-isa.md` | COP2 Instructions |
| **MIPS calling convention** | `db-isa.md` | MIPS Calling Convention |
| **SA register / QFSRV** | `db-isa.md` | SA Register |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **VU upper instructions (FMAC)** | `db-vu-instructions.md` | Upper Instructions |
| **VU lower instructions** | `db-vu-instructions.md` | Lower Instructions |
| **VU branch / flow** | `db-vu-instructions.md` | Branch Instructions |
| **VU flags (MAC, Status, Clip)** | `db-vu-instructions.md` | Flag Operations |
| **VU special regs (I, Q, P, ACC)** | `db-vu-instructions.md` | Special Registers |
| **XGKICK** | `db-vu-instructions.md` | XGKICK |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **Hardware register addresses** | `db-registers.md` | All register map tables |
| **Timer registers** | `db-registers.md` | Timer section |
| **DMA registers** | `db-registers.md` | DMA section |
| **GIF registers** | `db-registers.md` | GIF section |
| **VIF registers** | `db-registers.md` | VIF section |
| **IPU registers** | `db-registers.md` | IPU section |
| **GS privileged registers** | `db-registers.md` | GS Privileged section |
| **GS general registers bit-fields** | `db-registers.md` | GS General Registers |
| **COP0 register bit-fields** | `db-registers.md` | COP0 Registers section |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **Memory map / address ranges** | `db-memory-map.md` | Full address map |
| **KSEG0 / KSEG1 / KUSEG** | `db-memory-map.md` | Segment mapping |
| **ELF loading / entry point** | `db-memory-map.md` | ELF Layout |
| **Kernel memory areas** | `db-memory-map.md` | Kernel Areas |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **Syscall numbers + handler** | `db-syscalls.md` | Syscall Table |
| **IOP RPC mechanism** | `db-syscalls.md` | IOP RPC |
| **Stub linking (IMPORTS/EXPORTS)** | `db-syscalls.md` | Stub Patterns |
| **Exception dispatching** | `db-syscalls.md` | Exception Handling |
| **Thread management syscalls** | `db-syscalls.md` | Thread Syscalls |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **sceDma* functions** | `db-sdk-functions.md` | DMA Functions |
| **sceGs* functions** | `db-sdk-functions.md` | GS Functions |
| **scePad* functions** | `db-sdk-functions.md` | Pad Functions |
| **Controller SPI protocol** | `db-sdk-functions.md` | PAD SPI Protocol |
| **sceVif* / sceVu* functions** | `db-sdk-functions.md` | VIF/VU Functions |
| **File I/O (sceOpen, etc.)** | `db-sdk-functions.md` | File I/O |
| **Memory allocation (malloc)** | `db-sdk-functions.md` | Memory Management |
| **sceGsSyncV / sceGsSyncPath** | `db-sdk-functions.md` | GS Sync Functions |
| **printf / sceWrite** | `db-sdk-functions.md` | Debug / Output |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **EE/IOP architecture narrative** | `01-ps2-hardware-bible.md` | Full narrative reference |
| **Memory maps (narrative)** | `01-ps2-hardware-bible.md` | Memory Maps |
| **I/O register overview** | `01-ps2-hardware-bible.md` | I/O Registers |
| **Subsystem→file routing** | `01-ps2-hardware-bible.md` | Subsystem Routing |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **MIPS→C++ translation patterns** | `02-mips-r5900-isa.md` | Translation Rules |
| **MMI translation to C++** | `02-mips-r5900-isa.md` | MMI Translation |
| **COP0 translation to C++** | `02-mips-r5900-isa.md` | COP0 Translation |
| **FPU translation to C++** | `02-mips-r5900-isa.md` | FPU Translation |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **PS2Recomp pipeline / TOML** | `03-ps2recomp-pipeline.md` | Full pipeline |
| **TOML configuration** | `03-ps2recomp-pipeline.md` | Configuration |
| **Override system (*.cpp)** | `03-ps2recomp-pipeline.md` | Override System |
| **Build process (cmake)** | `03-ps2recomp-pipeline.md` | Build Process |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **Runtime syscall implementation** | `04-runtime-syscalls-stubs.md` | Syscall Implementation |
| **Stub patterns / signatures** | `04-runtime-syscalls-stubs.md` | Stub Patterns |
| **Runtime structure (src/lib/)** | `04-runtime-syscalls-stubs.md` | Runtime Architecture |
| **Game override C++ patterns** | `04-runtime-syscalls-stubs.md` | Override Patterns |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **Ghidra analysis workflow** | `05-ghidra-ghydramcp-guide.md` | Analysis |
| **GhydraMCP tools list** | `05-ghidra-ghydramcp-guide.md` | MCP Tools |
| **Function naming conventions** | `05-ghidra-ghydramcp-guide.md` | Naming |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **Game porting methodology** | `06-game-porting-playbook.md` | Step-by-step |
| **Common porting blockers** | `06-game-porting-playbook.md` | Blockers |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **Decompiled code patterns** | `07-ps2-code-patterns.md` | Pattern library |
| **DMA setup patterns** | `07-ps2-code-patterns.md` | DMA Patterns |
| **VU program loading patterns** | `07-ps2-code-patterns.md` | VU Load Patterns |
| **GS init patterns** | `07-ps2-code-patterns.md` | GS Init Patterns |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **How to search ps2tek.md** | `08-infinite-knowledge-base.md` | Search Instructions |
| **ps2tek section map** | `08-infinite-knowledge-base.md` | Section Index |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **Deep hardware reference** | `09-ps2tek.md` | Everything (232K) |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **ELF overlay detection** | `db-overlay-patterns.md` | §1 What Are Overlays |
| **Multi-binary porting** | `db-overlay-patterns.md` | §2 Detection Checklist |
| **COREC.BIN / dynamic modules** | `db-overlay-patterns.md` | §3 Discovery Protocol |
| **Overlay TOML config** | `db-overlay-patterns.md` | §4 TOML Integration |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **All prohibitions in one place** | `10-agent-guardrails.md` | §1-§2 Mistake Taxonomy |
| **Agent failure modes** | `10-agent-guardrails.md` | §1 Agent Mistake Taxonomy |
| **Problem Resolution / crash triage** | `10-agent-guardrails.md` | §3 Problem Resolution |
| **Fix Taxonomy (4 Tools)** | `10-agent-guardrails.md` | §3.1 Fix Taxonomy |
| **Root Cause Protocol** | `10-agent-guardrails.md` | §3.3 Root Cause Protocol |
| **Subsystem identification** | `10-agent-guardrails.md` | §3.5 Subsystem Map |
| **Adversarial Split / verification** | `10-agent-guardrails.md` | §4 Adversarial Split |
| **Circuit Breaker / 3-strike rule** | `10-agent-guardrails.md` | §5 Circuit Breaker |
| **Porting phase workflow details** | `11-operational-phases.md` | Phase 0–5 Deep Dives |
| **Phase entry/exit criteria** | `11-operational-phases.md` | Per-phase checklists |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **PCSX2 DebugServer tools** | `12-pcsx2-mcp-playbook.md` | §2 Tool Catalog |
| **A/B comparison (real PS2 vs recomp)** | `12-pcsx2-mcp-playbook.md` | §3 A/B Workflow |
| **PCSX2 debugging recipes** | `12-pcsx2-mcp-playbook.md` | §4 Common Recipes |
| **Reasoning loop / how to think** | `13-decisional-brain.md` | §1 The Reasoning Loop |
| **Diagnosis escalation ladder** | `13-decisional-brain.md` | §3 Escalation Ladder |
| **Thinking anti-patterns** | `13-decisional-brain.md` | §5 Anti-Patterns |
| **Static function DB (`ref/functions/`)** | `14-static-analysis-navigation.md` | §2 Per-Function Files |
| **Index JSONs / call graph / globals** | `14-static-analysis-navigation.md` | §3–§4 Indexes & Recipes |
| **Export staleness (`elf_hash`)** | `14-static-analysis-navigation.md` | §6 Staleness |
| **VU1 interpreter correctness** | `15-vu1-gs-debugging.md` | §2 VU1 Checklist |
| **GS state / texture / CLUT bugs** | `15-vu1-gs-debugging.md` | §3 GS State Checklist |
| **Diagnostic-lever / kill-switch doctrine** | `15-vu1-gs-debugging.md` | §5 Lever Doctrine |
| **Guest-execution lock model** | `16-runtime-concurrency-threading.md` | §1–§2 |
| **ABBA deadlock / wake handoff** | `16-runtime-concurrency-threading.md` | §3–§4 |
| **Hang triage table** | `16-runtime-concurrency-threading.md` | §5 Diagnosis |
| **Performance / low FPS / profiling** | `17-performance-optimization.md` | §2–§3 Measure & Hotspots |
| **Audio triage (silence/SFX/crackle/pitch)** | `18-audio-spu2-iop-debugging.md` | §2 Symptom Triage |
| **VAG/ADPCM verification** | `18-audio-spu2-iop-debugging.md` | §3 Data Verification |
| **ENDX / sound completion contract** | `18-audio-spu2-iop-debugging.md` | §4 |
| **Memory card / save implementation** | `19-memcard-pads-fileio.md` | §1 libmc |
| **Pad input contract & triage** | `19-memcard-pads-fileio.md` | §2 libpad |
| **Disc file I/O / LSN mapping / streaming** | `19-memcard-pads-fileio.md` | §3 cdvdman |
| **FMV / .PSS / IPU / cutscene skip** | `20-fmv-ipu-cutscenes.md` | §2 Strategy Tiers |
| **This game's concrete paths/facts** | `appendix-<game>-project.md` | e.g. `appendix-dc2-project.md` |

---

| I need to know about... | Go to | Section |
|------------------------|-------|---------|
| **Hardware diagrams / images** | `images/IMAGE_CATALOG.md` | 80 classified diagrams |
| **PS2 controller pad spec** | `padspecs.txt` | Controller SPI protocol |

## File Inventory

| File | Size | Last Updated | Content Type |
|------|------|-------------|-------------|
| `01-ps2-hardware-bible.md` | ~15 KB | 2026-03-06 | EE/IOP architecture narrative, memory maps, I/O registers |
| `02-mips-r5900-isa.md` | ~12 KB | 2026-03-06 | MIPS→C++ translation (MMI, COP0, FPU) |
| `03-ps2recomp-pipeline.md` | ~9 KB | 2026-03-13 | PS2Recomp tool pipeline + TOML config |
| `04-runtime-syscalls-stubs.md` | ~10 KB | 2026-03-06 | Syscall implementation, stubs, runtime structure |
| `05-ghidra-ghydramcp-guide.md` | ~5 KB | 2026-03-04 | Ghidra/GhydraMCP analysis workflow |
| `06-game-porting-playbook.md` | ~6 KB | 2026-03-05 | Game porting methodology |
| `07-ps2-code-patterns.md` | ~4 KB | 2026-03-03 | Decompiled code pattern recognition |
| `08-infinite-knowledge-base.md` | ~3 KB | 2026-03-06 | How to search & navigate 09-ps2tek.md |
| `09-ps2tek.md` | ~232 KB | 2026-02-18 | Deep hardware reference (all registers) |
| `10-agent-guardrails.md` | ~26 KB | 2026-07-02 | Guardrails + problem resolution + adversarial split + codegen defects |
| `11-operational-phases.md` | ~9 KB | 2026-07-02 | Phase 0–5 deep workflow with entry/exit criteria + ISO extraction |
| `12-pcsx2-mcp-playbook.md` | ~12 KB | 2026-07-02 | PCSX2 DebugServer tools, A/B comparison, recipes |
| `13-decisional-brain.md` | ~14 KB | 2026-07-02 | Reasoning loop, diagnosis escalation, anti-patterns |
| `14-static-analysis-navigation.md` | ~9 KB | 2026-07-02 | Static function DB + index JSONs + graphify navigation |
| `15-vu1-gs-debugging.md` | ~20 KB | 2026-07-02 | VU1 correctness checklist, GS state checklist, lever doctrine |
| `16-runtime-concurrency-threading.md` | ~7 KB | 2026-07-02 | Guest-lock model, ABBA, wake handoff, hang triage |
| `17-performance-optimization.md` | ~7 KB | 2026-07-02 | Correctness-first perf doctrine, hotspot classes, verification |
| `18-audio-spu2-iop-debugging.md` | ~8 KB | 2026-07-02 | Audio path triage, VAG/ADPCM, ENDX contract, stub tiers |
| `19-memcard-pads-fileio.md` | ~8 KB | 2026-07-02 | Memory cards/saves, pad input, disc file I/O (LSN/ISO) |
| `20-fmv-ipu-cutscenes.md` | ~5 KB | 2026-07-02 | FMV/.PSS/IPU playback, skip strategy, cutscene hangs |
| `appendix-dc2-project.md` | ~11 KB | 2026-07-02 | EXAMPLE project appendix — Dark Cloud 2 concrete paths/facts |
| `db-ps2-architecture.md` | ~40 KB | 2026-03-13 | System diagrams (mermaid), pipelines, data flow |
| `db-ps2-index.md` | ~12 KB | 2026-03-21 | THIS FILE — master topic → file+section map |
| `db-isa.md` | ~24 KB | 2026-03-13 | R5900 instruction tables (630 lines) |
| `db-vu-instructions.md` | ~15 KB | 2026-03-13 | VU0/VU1 micro instruction tables |
| `db-registers.md` | ~53 KB | 2026-03-13 | All HW register addresses + bit-fields |
| `db-sdk-functions.md` | ~34 KB | 2026-03-13 | SDK function signatures + PAD protocol |
| `db-syscalls.md` | ~15 KB | 2026-03-13 | Syscall table, stubs, RPC, exception handling |
| `db-memory-map.md` | ~7 KB | 2026-03-13 | Address space map, ELF layout |
| `db-overlay-patterns.md` | ~8 KB | 2026-07-18 | ELF overlay detection, multi-binary porting |
| `images/IMAGE_CATALOG.md` | ~5 KB | 2026-03-06 | 80 classified hardware diagrams from PS2 PDFs |
| `padspecs.txt` | ~2 KB | 2026-03-06 | PS2 controller SPI protocol specification |

**Total knowledge base**: ~620 KB of structured, searchable PS2 knowledge.
