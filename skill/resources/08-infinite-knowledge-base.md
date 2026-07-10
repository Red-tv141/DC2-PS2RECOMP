# Reference: Infinite Knowledge Base (PS2docs)
> Use this when you encounter a highly specific hardware register, SCMD command, SIF RPC structure, VIF packet format, or undocumented PS2 behavior that is not covered in the other reference files.

## The Absolute Source of Truth

The user maintains a local repository of exhaustive PlayStation 2 documentation, including the legendary `ps2tek` (the reverse engineering holy grail), MIPS instruction set references, and Sony architectural analyses.

**Location of Absolute Truth:**
`resources/09-ps2tek.md` (Embedded locally)

If you ever find yourself stuck wondering:
- "What exactly does bit 5 of the GS `TEX0` register do?"
- "What is the exact memory layout of the SPU2 registers?"
- "How does the IPU DMA channel format its packets?"
- "What is the specific behavior of the `mfc0` instruction when reading the `Count` register?"

Do **NOT** guess. Do **NOT** hallucinate based on general MIPS knowledge.

## How to extract Infinite Knowledge

You have two native tools. No scripts needed.

### 1. Text & Tables — `grep_search` + `view_file`

The most important file is `resources/09-ps2tek.md` (230KB+ of pure markdown).

Use `grep_search` with highly specific queries (e.g., `TEX0`, `DMAC`, `SIFCMD`, `0x12000000`, `PMODE`) against this file. Once you find the right section, use `view_file` to read the surrounding context.

For PDF manuals in `resources/`, use `view_file` directly — it handles binary files including PDFs. Notable files:
- `EE_Users_Manual.pdf`, `EE_Core_Instruction_Set_Manual.pdf` (Emotion Engine)
- `GS_Users_Manual.pdf` (Graphics Synthesizer)
- `VU_Users_Manual.pdf` (Vector Units)
- `SPU2_Overview_Manual.pdf` (Sound)
- `mips_instruction_set_reference.pdf`
- `PlayStation 2 Architecture _ A Practical Analysis.pdf` (16MB detailed analysis)

> **Note:** All PDF text content has been extracted into the `db-*.md` knowledge bases. PDFs are kept as backup and for visual diagrams.

### 2. Visual Diagrams — Images & PDFs

For block diagrams, pipeline schematics, and architectural layouts:

1. **Classified images** (fastest): Check `resources/images/IMAGE_CATALOG.md` — it lists 80 extracted PNGs sorted by category (architecture, bus, pipeline, register, comparison). Each entry has filename, source PDF, and description.
2. **PDFs** (fallback): `view_file` on a PDF activates multimodal vision automatically. You can read diagrams directly — no extraction step needed.

## The Golden Rule of PS2 Porting

**"If it's hardware, it's mapped. If it's mapped, it's documented. If it's documented, it's in ps2tek."**

Never bypass an unknown hardware interaction by stubbing it with `ret0` without first checking the `ps2tek` to understand *why* the game is touching that memory. If you stub a DMAC completion flag with 0, you might cause an infinite spinlock. Read the docs!
