// ==============================================================================================
// G713 — PIPELINE, PART 1 of 2 (the parse/exec split of the open capture batch).
//
// Included BEFORE rasterizer_command_graph.inc, because the graph, the capture site and the flush
// all name the two indirections below.
//
// ⭐ THE ONE IDEA. `g_g144List` is BOTH the open capture buffer (appended by the GIF parse) and the
// flush's working buffer — `g260ExecutePrefix` does `g_g144List.swap(b.entries)`, and 144 sites in
// rasterizer_vram_materialization.inc read it as "the batch being flushed". Those are exactly the
// two halves this phase puts on different threads, so they cannot be the same vector.
//
// `g_g144Open` is that separation and nothing else. In every shipped run it points AT
// `g_g144List` and `g_g144OpenFbp` at `g_g144BlockFbp`, so the capture path is behaviourally
// identical to today. Under `DC2_G713_PIPE` they are re-pointed once, at arm time, to parse-thread
// storage — and `g_g144List` becomes exec-private for the whole run.
//
// ⚠️ COST: one pointer load per captured primitive (~10k/f). Deliberately NOT applied to the ~40
// field writes that fill the entry — those still target a stack local, because G521 §6b measured
// that turning them into indirect stores through a runtime-varying base is what killed `capinplace`.
// ==============================================================================================

// ⛔ ps2_g713_pipeline_api.inc is NOT included here. This file is pulled in AFTER
// rasterizer_draw_prim_probe.inc has opened the anonymous namespace that stays open until
// rasterizer_rtt_census_and_waves.inc, so an include here would give every pipeline declaration
// INTERNAL linkage and produce eight LNK2001s against the cold TU's global-scope definitions. The
// header is included at global scope near the top of ps2_gs_rasterizer.cpp instead. (Same trap
// ps2_gs_rasterizer.cpp:333 already records for g568BitAliasArm.)

// Parse-thread storage. Untouched — and unallocated — unless the pipeline arms.
static std::vector<G144Entry> g_g713ParseList;
static uint32_t g_g713ParseFbp = 0xFFFFFFFFu;

static std::vector<G144Entry> *g_g144Open = &g_g144List;
static uint32_t *g_g144OpenFbp = &g_g144BlockFbp;

// A plain TU-local bool, so the capture predicate reads a static rather than calling across a TU
// boundary ~10,000 times a frame. Set exactly once, by g713ArmIfRequested, before any post.
static bool g_g713On = false;

// True only while the exec thread is inside the re-entered non-deferrable tail of drawPrimitive.
// The deferral predicate is then bypassed, so it is evaluated exactly once per primitive (on the
// parse thread) — the address registries it mutates (g527NoteTargetDraw) must never be re-noted.
static thread_local bool t_g713ExecInline = false;

// Defined in g713_pipe_parse.inc; forward-declared here for the capture site and the graph.
static void g713ParseFlushOpen();
static void g713ArmIfRequested(GSRasterizer *self);
