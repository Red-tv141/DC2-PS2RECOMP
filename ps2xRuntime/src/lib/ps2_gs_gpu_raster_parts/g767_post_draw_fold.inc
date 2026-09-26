// ===== G767: POST-DRAW FOLD — a transient target's rendered pages go GPU->GPU into residents =====
//
// The backend half of the TRANSIENT -> RESIDENT FOLD (front end:
// ps2_gs_rasterizer_parts/g767_transient_fold.inc). A transient contract (G286 0x13b, G570 0x13d,
// G586 0x141, G587 0x15d, G604 discovered targets) used to end every flush with a synchronous
// glReadPixels of its rendered rows, an unpack into guest VRAM, a page-generation bump, and a VRAM ->
// FBO mirror of the same rows into every dirty resident that aliases them: GPU -> CPU -> GPU for
// bytes no CPU observer asked for. Now the front end arms, per flush, a list of page-tile copies
// FROM the batch's own FBO INTO the resident FBOs that hold those pages; the render item applies them
// immediately AFTER its last draw, in the same worker item, and the batch reads nothing back.
//
// Same exactness argument as G766's pulls (g766_post_upload.inc): a CT32 page is a 64x32 rect whose
// within-page pixel order does not depend on FBW, every colour FBO carries the same integer scale S
// (G675), and transients and residents store the same raw-alpha RGBA8 representation (G255) — so a
// glCopyTexSubImage2D of a page rect (or of a row band of one) is bit-exact at every S.
//
// One slot, armed immediately before a SYNCHRONOUS submit. The render item compares its own FBO key
// with the armed key; a mismatch is counted and never applied. EVERY rect of EVERY destination is
// validated before the first copy is issued, so a refused fold lands NOTHING — the front end then
// sends the batch to its CPU fallback with every resident untouched.
struct G767PostDraw
{
    std::mutex mtx;
    bool armed = false;
    uint32_t srcKey = 0u;
    std::vector<int32_t> rects7; // {dstKey, sx, sy, dx, dy, w, h} x N, logical GL coordinates
    bool lastApplied = false;
    uint64_t armedN = 0u, appliedN = 0u, rectsN = 0u, mismatchN = 0u, failN = 0u;
};
static G767PostDraw g_g767Fold;
