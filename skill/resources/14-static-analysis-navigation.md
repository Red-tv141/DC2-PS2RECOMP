# Reference: Static Analysis Navigation — Function DB, Indexes, Graph

> **Load this whenever you need to UNDERSTAND a `sub_xxx` / function** (what it does, who
> calls it, what globals it touches, whether to stub or recompile). This is the **primary**
> "understand the original code" path. Live Ghidra (`05-ghidra-ghydramcp-guide.md`) is the
> FALLBACK, used only when no static export exists.

---

## §1 Two Analysis Modes — Detect First

There are two ways to read the original MIPS. **Detect which one this project uses BEFORE
reaching for Ghidra.**

| Mode | Evidence | Tooling |
|------|----------|---------|
| **STATIC EXPORT (preferred)** | A `ref/functions/` dir of per-function `.md` + a `ref/index/` dir of `*_index.json` | `Grep`, `Read`, `jq`/PowerShell on JSON — **no MCP needed** |
| **LIVE GHIDRA (fallback)** | A running Ghidra CodeBrowser + `mcp_ghydra_*` tools present | `05-ghidra-ghydramcp-guide.md` |

> **Rule:** If `ref/functions/` + `ref/index/` exist, USE THEM. They are faster than MCP, work
> offline, survive context resets (they're files you re-grep), and carry pre-computed triage
> the live tool doesn't have. Only fall back to live Ghidra for something the export lacks
> (e.g. re-decompiling with new types).

Record which mode the project uses in `PS2_PROJECT_STATE.md → Environment Setup`.

---

## §2 The Per-Function File (`ref/functions/0xADDR_name.md`)

One file per function. ~4 KB each — cheap to `Read` in full. Naming: `0x<ADDR>_<name>.md`
(e.g. `0x001000D0__dpfne.md`). To open a function you know the address of, glob
`ref/functions/0x001000D0*` — do NOT list the whole dir (thousands of files).

**Sections inside each file (in order):**

| Section | What it gives you |
|---------|-------------------|
| Header line | address · category · **disposition** · origin |
| `> schema_version … elf_hash … global_pointer … exported …` | **Staleness stamp** — see §6 |
| **Summary** table | category, **disposition**, size, mainloop/init/drawing chain depth, module_id, AI subsystem/role |
| **Recommendation (AI)** | `action` (force_recompile / stub / skip) + confidence + risk-if-stubbed vs risk-if-recompiled + reason |
| **Tags** | triage buckets this fn carries (e.g. `BUSY_WAIT`, `MMIO_ACCESS`, `A0_PASSTHROUGH_RETURNER`) |
| **Memory / Globals** | gp-relative globals read/written |
| **Calls** | **Callees** + **Callers** *with call-site addresses* (`name(fn@call_site)`) |
| **Related Function Files** | direct links to the caller/callee `.md` files — follow these to traverse |
| **Search Anchors** | grep tokens: `FUNC_0x…`, `NAME_…`, `CATEGORY_…`, `DISPOSITION_…`, `CALLS_…`, `CALLED_BY_…` |
| **Assembly** | raw MIPS disasm |
| **Decompiled** | C pseudocode |
| **Control flow** | basic-block graph + edge kinds (`UNCONDITIONAL_CALL`, branch, etc.) |

**The disposition is the cheat code.** Before writing any fix for a function, read its AI
`action`: `force_recompile` (translate the real body), `stub` (route to a runtime handler),
`skip` (placeholder). It already encodes the §3.1 fix-taxonomy choice + the risk of getting
it wrong. Treat it as a strong prior, not gospel — verify against the decompiled body.

---

## §3 The Index JSONs (`ref/index/*.json`)

Cross-function queries the per-file view can't answer. **These can be large (tens of MB) —
NEVER `Read` them whole.** Query with `Grep` (by address/name string) or a targeted
`jq`/PowerShell one-liner. Every index carries `elf_hash` + `global_pointer` for staleness.

| File | Purpose | Shape (per entry) |
|------|---------|-------------------|
| `functions_index.json` | Master record + project-wide statistics & hazard counts | `statistics{}`, `text_range`, `runtime_handler_roster`, per-fn records |
| `calls_index.json` | Forward + reverse call graph (jal edges) | `{address, name, callees[], callers[{address,name,call_site}]}` |
| `xrefs_index.json` | Xref counts + referenced globals + literal refs | `{address, name, xref_to_count, caller_count, referenced_globals[], literal_refs[{pc,mnem,base_reg,offset,dest_reg}]}` |
| `globals_index.json` | **Global token → who touches it** + init-order hazard | `"0xADDR": {readers[], writers[], sinit_writers[], touchers[], init_order_hazard}` |
| `tags_index.json` | Tag → functions carrying it (priority-ranked triage) | `"TAG": {count, priority, functions[{address,name}]}` |

**The `statistics{}` block in `functions_index.json`** is a project triage dashboard — read it
once at boot to know the shape of the work: `total_functions`, `busy_wait`, `mmio_access`,
`iop_rpc_dispatch`, `jump_tables`, `init_large_func`, `safe_leaf`, `orphan_code`,
`override_bound`, `no_runtime_handler_stubs`, etc. High `busy_wait`/`mmio_access` counts =
spinlock/hardware work ahead; high `iop_rpc_dispatch` = IOP/SIF surface.

---

## §4 Navigation Recipes

**Open a function by address** (cheapest):
```
Glob  ref/functions/0x001CEA00*        # find the file
Read  ref/functions/0x001CEA00_LoopDungeonMain.md
```

**Find a function by name / symbol** (grep the anchors, not the JSON):
```
Grep  "NAME_LoopDungeonMain"   path=ref/functions   output_mode=files_with_matches
Grep  "CreateRenderInfoPacket" path=ref/functions
```

**Who calls X? / What does X call?** — the per-function file's *Calls* section already lists
callers + callees with call-site addresses. For a programmatic / whole-graph query use
`calls_index.json`:
```
# callers of a function, with call sites:
jq '.functions[] | select(.name=="EventLoop") | .callers' ref/index/calls_index.json
```

**Who reads/writes a global?** (the killer query for "where is this state set?") —
`globals_index.json` maps the global address to readers / writers / `sinit_writers`:
```
jq '.globals["0x1ece40c"]' ref/index/globals_index.json
```
`sinit_writers` = static-init constructors (`__sinit_*`) that set it — critical for the
"global looks zeroed because its `__sinit` never ran" class of bug. `init_order_hazard: true`
flags init-order sensitivity.

**Triage by behavior** — `tags_index.json` is priority-ranked buckets. To find every
busy-wait spinlock, or every passthrough returner, pull the tag:
```
jq '.tags.BUSY_WAIT.functions' ref/index/tags_index.json
jq '.tags.MMIO_ACCESS.functions' ref/index/tags_index.json
```

**What literal addresses / MMIO does a function poke?** — `xrefs_index.json` `literal_refs`
(pc, mnem, base_reg, offset) + `referenced_globals`. Use it to spot a function hammering a
hardware register (`0x1000xxxx`/`0x12000000`) without opening the asm.

---

## §5 graphify Knowledge Graph (cross-module "how does X relate to Y")

Some projects also ship a **graphify** graph (look for `graphify-out/` with `GRAPH_REPORT.md`,
`graph.json`, and maybe `wiki/`). It clusters the codebase into communities and links
EXTRACTED + INFERRED edges. Use it for *architecture* questions that span modules, where the
per-function call graph is too local:
- Read `graphify-out/GRAPH_REPORT.md` first for god-nodes + community structure.
- If `graphify-out/wiki/index.md` exists, navigate it instead of raw files.
- Prefer `graphify query "<question>"`, `graphify path "<A>" "<B>"`, `graphify explain "<concept>"`
  over grep for "how does subsystem A relate to B" — they traverse the graph, not just files.
- `graphify update .` only after you change module structure (new files, moved functions).

---

## §6 Staleness — Trust But Verify the Export

Every function file and index stamps `elf_hash` + `global_pointer`. The export is a SNAPSHOT
of one ELF.

- **If the project's current ELF hash ≠ the stamped `elf_hash`, the export is STALE** — function
  bodies, addresses, and call edges may have moved. Re-run the project's enricher/exporter
  before trusting it (the function files say so themselves: *"If elf_hash / global_pointer
  differ from the current ELF, this doc is stale — re-run the enricher."*).
- `global_pointer` is the `$gp` value used to resolve gp-relative globals; a different `$gp`
  invalidates every `gp-0xNNNN` global address in the export.
- The export is a derived artifact — **never hand-edit `ref/functions/` or `ref/index/`**; fix
  the source and regenerate.

---

## §7 Where This Sits in the Diagnosis Ladder

This file IS Level 3 of the `13-decisional-brain.md` ladder when a static export exists:

```
L1 stdout/crash PC →  L2 subsystem map →  L3 UNDERSTAND THE ORIGINAL CODE
                                          └─ static export (THIS file)  ← preferred
                                          └─ live Ghidra MCP (05)        ← only if no export
                              →  L4 PCSX2 MCP A/B  →  L5 circuit breaker
```

Cross-refs: fix taxonomy `10-agent-guardrails.md §3.1`; reasoning loop `13-decisional-brain.md`;
runtime A/B `12-pcsx2-mcp-playbook.md`; live-Ghidra fallback `05-ghidra-ghydramcp-guide.md`.
For the concrete paths/commands of THIS project's export, see the project appendix
(`appendix-dc2-project.md` if present).
