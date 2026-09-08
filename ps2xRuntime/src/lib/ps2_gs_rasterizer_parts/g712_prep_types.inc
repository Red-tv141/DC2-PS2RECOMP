// ==============================================================================================
// G712 — PREP-AHEAD: the declarations the command graph needs, ahead of the implementation
// ==============================================================================================
//
// Split out of g712_prep_body.inc purely because of include order: `g260CloseOpenBatch` (the POST
// site) lives in rasterizer_command_graph.inc, which is included BEFORE
// rasterizer_rtt_census_and_waves.inc — and the job body cannot be written until
// `g178ClassifyEntry` exists. So the graph sees only an opaque handle and three calls; everything
// with a body is in g712_prep_body.inc, after the classifier.
//
// ⛔ RULE 12b BUDGET. Everything below is a declaration; no code is emitted here. The pool itself
// (threads, queue, env parsing, reporter) lives in the COLD TU ps2_g712_pipeline.cpp, because G710
// measured +0.362 ms/f from 6 KB of compile-time-DEAD text in this TU and G651 measured +0.567 /
// +0.934 ms/f from ~150 lines of cold start-up code in the hot GIF TU.
// ==============================================================================================

// ⚠️ The four pool entry points live in the COLD TU ps2_g712_pipeline.cpp and are declared at
// GLOBAL scope in ps2_gs_rasterizer.cpp, above the anonymous namespace this file lands in — the
// G568 linkage lesson: a declaration inside the unnamed namespace names a DIFFERENT, never-defined
// symbol, and the link fails with four unresolved `anonymous namespace'::g712Prep* externals.
// (`g712PrepAheadEnabled`, `g712PrepStatOn`, `g712PrepPoolStart`, `g712PrepPoolPost`.)

// The per-batch preparation record. Opaque to the command graph: it stores the handle, hands it to
// the flush, and releases it. Defined in g712_prep_body.inc.
struct G712Prep;

// Open a preparation record for a batch that has just been CLOSED (and is therefore immutable
// until it executes), and post it to the pool. `entries` must remain valid and unmodified until
// the matching g712PrepRelease. Returns nullptr when the mechanism is disarmed — every call site
// must treat that as "run the shipped inline path".
static G712Prep *g712PrepOpen(const G144Entry *entries, size_t n, uint32_t fbp, uint32_t fbw);

// Return the record to the free list. Safe on nullptr. Must not be called while the record is
// CLAIMED by a worker — every consumer path runs g712PrepConsume (help-or-wait) first, and the
// release sites below only run after the batch has executed.
static void g712PrepRelease(G712Prep *&p);

// The record belonging to the batch currently executing, published by g260ExecutePrefix /
// g260ExecuteAll around their `fn()` call exactly like g_g528Wr / g_g528Rd. nullptr means "no
// prepared state — classify inline", which is the shipped behaviour.
static G712Prep *g_g712CurPrep = nullptr;
