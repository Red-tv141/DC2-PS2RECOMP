// ===== G766: POST-UPLOAD PAGE PULLS — the backend half of GPU page-owner coherence =====
//
// The front end (ps2_gs_rasterizer_parts/g766_page_owner.inc) tracks, per GS page, which resident
// target FBO holds the newest bytes. When a resident target's wave is about to render and some page
// of its window is newer in ANOTHER target's FBO, the old architecture published that other target
// to guest VRAM (a blocking readback) so the wave's VRAM seed could see it. G766 instead arms a list
// of page-tile copies here; the batch's render item applies them immediately AFTER its VRAM seed
// upload and BEFORE its first draw, so the FBO is exactly current without any GPU->CPU crossing.
//
// A CT32 page is a 64x32 rect whose within-page pixel order is layout independent (only page
// SELECTION depends on FBW), and every colour FBO is scaled by the same integer S (G675), so a
// glCopyTexSubImage2D of the page rect is bit-exact.
//
// One slot, armed by the front end immediately before a SYNCHRONOUS submit (the front end refuses to
// arm under the fire-and-forget G305/G701 paths). The render item compares its own FBO key with the
// armed key; a mismatch is counted and reported, never applied to the wrong surface. The front end
// reads `lastApplied` after the submit returns and fails closed if the pulls did not land.
struct G766PostUpload
{
    std::mutex mtx;
    bool armed = false;
    uint32_t dstKey = 0u;
    std::vector<int32_t> rects7; // {srcKey, sx, sy, dx, dy, w, h} x N, logical GL coordinates
    bool lastApplied = false;
    uint64_t armedN = 0u, appliedN = 0u, rectsN = 0u, mismatchN = 0u, failN = 0u;
};
static G766PostUpload g_g766Post;
