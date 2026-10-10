// ==============================================================================================
// G788 — WHOLE-WORLD NATIVE FixMDT: the cross-TU surface between the EE seam
// (dc2_game_override_parts/g788_world_native.inc), the GS parse thread (gpu_gif_and_registers.inc
// tag hook), the rasterizer (g788_world_native.inc: macro entries, CPU expansion) and the GL
// backend (g788_world_backend.inc: resident static source, GPU transform, draw). plans/phase-G788-ledger.md.
//
// The static world (mgCVisualFixMDT, vtable 0x375140) is drawn per frame as
//   mgCVisualMDT::Draw -> CreateRenderInfoPacket (matrices/lights + DIRECT GS state)
//                      -> CreatePacket (per bucket: REF material (TEX0/TEX1/CLAMP), REF PRMODE,
//                         one STATIC face geometry tag per face, TEXFLUSH, RET)
//                      -> AddPacket per bucket (texture-slot sorted node list, emitted by
//                         mgCDrawManager::EndDraw / Draw(slot)).
// The face geometry tag references the load-time face packet (UNPACK + MSCAL + MSCNT + FLUSHA): the
// only per-vertex work. G788 replaces exactly that tag with a REF to a 2-qword marker
// ([VIF NOP NOP NOP DIRECT 1] + a NLOOP=0 GIF tag whose REGS carry 0xC788 | job), so every GS state
// DIRECT of the legacy node still runs and only the face's VIF unpack / VU1 / PATH1 / parse work is
// deleted. The parse thread sees the marker at the face's exact stream position.
// ==============================================================================================
#ifndef DC2_G788_WORLD_API_INC
#define DC2_G788_WORLD_API_INC 1

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include "runtime/dc2_native_draw.h"
#include "runtime/dc2_g788_economics.h"

constexpr uint64_t kG788MarkerMagic = 0xC788ull;
constexpr uint32_t kG788DropJob = 0xFFFFFFFFu; // marker of a dropped face (ceiling probe)

// Process-wide mode (resolved once from DC2_G788_WORLD, read by every side):
//   0 off; 1 census (EE classification only, nothing substituted);
//   2 DROP  ⛔ corrupting ceiling probe: supported faces' geometry tags -> markers, nothing drawn;
//   3 native: markers -> ordered native entries -> GPU-transformed raster;
//   4 verify: legacy runs; a CALL'd marker before each face arms the CPU-reference oracle;
//   5 DROPALL ⛔ corrupting ceiling probe: the EE still builds every packet, but each supported node's
//     renderInfo AND chain are replaced by an empty RET (no VIF / VU1 / GS-state work at all).
extern int g_g788Mode;

// ---- immutable static geometry (built once per mesh on the EE, uploaded once by the GL worker) ----
// GPU source: per source triangle 13 vec4 = 3 corners x (position, normal, uv, colour) + info
// (localTri, batchFirstLocal, batchCount, topology) — local to the FACE. Triangles of a face are
// contiguous; faceTri[f] .. faceTri[f+1] is face f's range.
struct G788MeshGpu
{
    uint64_t identity = 0;
    std::shared_ptr<const dc2_native::Mesh> mesh;
    std::vector<dc2_native::Vector> source;
    std::vector<uint32_t> faceTri;
    uint32_t triangles = 0;
};
// One visual draw of one frame (EE capture; read by the parse thread, the executor and the GL worker).
struct G788DrawRec
{
    std::shared_ptr<const G788MeshGpu> mesh;
    dc2_native::DC2NativeDraw draw;                              // CPU reference input (geometry = mesh->mesh)
    std::vector<std::array<dc2_native::Vector, 21>> constants;   // per material: constants[0..20]
    uint32_t frame = 0;
    uint32_t clipMode = 3; // renderer flags bits 0/1 as the VU sees them (draw.renderFlags may be forced)
};
// One face of one draw. Ring slot = seq & (kG788Ring - 1); `seq` is published last (release).
constexpr uint32_t kG788Ring = 1u << 16;
struct G788Job
{
    std::shared_ptr<const G788DrawRec> rec;
    uint32_t face = 0;
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> verifyConsumed{0}; // end marker acknowledged; diagnostic arena may reuse this slot
};
extern G788Job g_g788Ring[kG788Ring];
// One native face of one GPU batch (built by the executor after the vertex build; owns the record).
struct G788BatchJob
{
    std::shared_ptr<const G788DrawRec> rec;
    uint32_t face = 0, triCount = 0, outBase = 0, flags = 0; // flags: 1 TME, 2 IIP
    uint16_t ofx = 0, ofy = 0;
};
inline bool g788Lookup(uint32_t seq, std::shared_ptr<const G788DrawRec> &rec, uint32_t &face)
{
    G788Job &j = g_g788Ring[seq & (kG788Ring - 1u)];
    if (j.seq.load(std::memory_order_acquire) != seq)
        return false;
    rec = j.rec;
    face = j.face;
    return j.seq.load(std::memory_order_acquire) == seq && rec != nullptr;
}

// Counters (monotonic, relaxed). Parse thread: markers / drop markers / orphans / macros / fallback
// kicks; exec: macro draws / CPU expansions / stale; GL: dispatches / jobs / triangles / uploads.
struct G788Counters
{
    std::atomic<uint64_t> markers{0}, dropMarkers{0}, orphans{0}, macros{0}, emptyFaces{0}, fallbackFaces{0},
        fallbackPrims{0}, refPrimFail{0}, lateMacros{0}, primerMacros{0}, primerReference{0}, primerDeclined{0};
    std::atomic<uint64_t> xMacros{0}, xCpuExpand{0}, xCpuFaces{0}, xStale{0}, xJobs{0}, xTris{0}, xBound{0}, xMerged{0}, xRuns{0};
    std::atomic<uint64_t> gDispatch{0}, gJobs{0}, gTris{0}, gMeshUploads{0}, gUploadBytes{0}, gDraws{0},
        gFail{0}, gArenaGrow{0};
    // VERIFY (mode 4): legacy runs; every pushed VU triangle of a face vs the per-face CPU reference.
    std::atomic<uint64_t> vFaces{0}, vOk{0}, vBad{0}, vZero{0}, vMissing{0}, vExtra{0};
    // per renderer clip mode (VU flags bits 0/1): faces, faces bad under the draw's own flags (A) and
    // under clipping forced on (B: renderFlags := 3)
    std::atomic<uint64_t> vmFaces[4]{}, vmBadA[4]{}, vmBadB[4]{};
    std::atomic<uint64_t> vmInput[4]{}, vmExpected[4]{}, vmCulled[4]{}, vmZero[4]{};
    std::atomic<uint64_t> vmCandidateA[4]{}, vmCandidateB[4]{}, vmFansA[4]{}, vmFansB[4]{};
    std::atomic<uint64_t> vmMissingA[4]{}, vmMissingB[4]{}, vmExtraA[4]{}, vmExtraB[4]{};
    std::atomic<uint64_t> vmAttrA[4]{}, vmAttrB[4]{};
    // GPU oracle (native + DC2_G788_GVERIFY=N): the compute output read back vs the CPU reference.
    std::atomic<uint64_t> gvBatches{0}, gvFaces{0}, gvTris{0}, gvBad{0}, gvMissing{0}, gvExtra{0}, gvShaderFail{0}, gvRefRefused{0};
};
extern G788Counters g_g788C;
// VERIFY bracket state, read by the GS kick path: 0 off, 1 inside a face's begin/end markers, 2 between faces.
extern std::atomic<int> g_g788VerifyArmed;
// GS vertexKick while a face is bracketed (1) / between faces (2): drawing kicks and ADC (XYZF3) vertices.
struct G788KickCounters { std::atomic<uint64_t> draw[3]{}, noDraw[3]{}; };
extern G788KickCounters g_g788K;

class GS;
void g788OnTagMarker(GS *gs, uint64_t tagHi); // processGIFPacket: NLOOP=0 tag, REGS magic 0xC788

// ---- per-face CPU reference (the exact legacy fallback, the CPU-replay expansion, the oracle) -----
// Same mathematics as dc2_native::referenceTransform + referencePrimitives, restricted to one face:
// per source batch, generated clipping fans first, then the unchanged triangles (stable). Returns
// false where the whole-draw reference would refuse (non-finite / unprojectable).
inline bool g788FacePrims(const G788DrawRec &rec, uint32_t faceIndex,
                          std::vector<dc2_native::ReferencePrimitive> &out, bool firstOnly, int forceFlags = -1)
{
    dc2_g788_econ::event(dc2_g788_econ::ReferenceExpansion);
    using namespace dc2_native;
    const DC2NativeDraw &draw = rec.draw;
    if (!draw.geometry || faceIndex >= draw.geometry->faces.size())
        return false;
    const Face &face = draw.geometry->faces[faceIndex];
    if (face.material >= draw.materials.size() || (face.topology != 3 && face.topology != 4))
        return false;
    const auto screenM = multiply(draw.cameraScreen, draw.world), clipM = multiply(draw.cameraClip, draw.world);
    std::array<Vector, 3> directions;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        directions[axis] = product(draw.lightDirections[0], splat(draw.world[axis][0]));
        directions[axis] = madd(directions[axis], draw.lightDirections[1], splat(draw.world[axis][1]));
        directions[axis] = madd(directions[axis], draw.lightDirections[2], splat(draw.world[axis][2]));
    }
    Vector lengths = product(directions[0], directions[0]);
    lengths = madd(lengths, directions[1], directions[1]);
    lengths = madd(lengths, directions[2], directions[2]);
    Vector reciprocal;
    for (uint32_t i = 0; i < 4; ++i)
        reciprocal[i] = lengths[i] == 0 ? 3.402823466e38f : 1.0f / std::sqrt(lengths[i]);
    for (auto &direction : directions)
        direction = product(direction, reciprocal);
    const auto &material = draw.materials[face.material];
    Matrix colours;
    for (uint32_t i = 0; i < 4; ++i)
        colours[i] = product(draw.lightColours[i], material.colour);
    const auto ambient = product(draw.ambient, material.colour);
    auto transformCorner = [&](uint32_t corner, ReferenceVertex &output) {
        dc2_g788_econ::event(dc2_g788_econ::SourceTransform);
        const auto &vertex = draw.geometry->vertices[face.indices[corner]];
        output.face = faceIndex;
        output.corner = corner;
        output.screen = transform(screenM, vertex.position);
        output.clip = transform(clipM, vertex.position);
        for (const auto *value : {&output.screen, &output.clip})
            for (float lane : *value)
                if (!std::isfinite(lane))
                    return false;
        Vector light = product(directions[0], splat(vertex.normal[0]));
        light = madd(light, directions[1], splat(vertex.normal[1]));
        light = madd(light, directions[2], splat(vertex.normal[2]));
        for (auto &value : light)
            value = std::max(0.0f, value);
        output.colour = ambient;
        for (uint32_t i = 0; i < 4; ++i)
            output.colour = madd(output.colour, colours[i], splat(light[i]));
        for (uint32_t i = 0; i < 4; ++i)
            output.colour[i] = std::min(output.colour[i], 255.0f);
        output.colour = product(output.colour, vertex.colour);
        for (uint32_t i = 0; i < 4; ++i)
            output.colour[i] = std::min(output.colour[i], 255.0f);
        output.uv = vertex.uv;
        return true;
    };
    const size_t n = face.indices.size();
    std::vector<ReferenceVertex> transformed(n);
    for (size_t c = 0; c < n; ++c)
    {
        if (face.indices[c] >= draw.geometry->vertices.size() || !transformCorner(uint32_t(c), transformed[c]))
            return false;
    }
    if (face.topology == 4 && face.batchCorners && n > face.batchCorners)
        return false;
    size_t batchStart = out.size();
    uint32_t currentBatch = ~0u;
    auto flushBatch = [&]() {
        std::stable_sort(out.begin() + batchStart, out.end(), [](const ReferencePrimitive &a, const ReferencePrimitive &b) {
            return a.batch != b.batch ? a.batch < b.batch : a.clipped && !b.clipped;
        });
    };
    for (size_t corner = 2; corner < n; ++corner)
    {
        if (face.topology == 3 && corner % 3 != 2)
            continue;
        const uint32_t batch = face.batchCorners ? uint32_t(corner / face.batchCorners) : 0u;
        if (batch != currentBatch)
        {
            if (currentBatch != ~0u)
            {
                flushBatch();
                if (firstOnly && out.size() > batchStart)
                {
                    out.resize(batchStart + 1);
                    return true;
                }
                batchStart = out.size();
            }
            currentBatch = batch;
        }
        std::array<size_t, 3> idx = {corner - 2, corner - 1, corner};
        if (face.topology == 4 && corner % 2)
            std::swap(idx[0], idx[2]);
        std::vector<ReferenceVertex> polygon;
        bool clipped = false;
        for (const auto i : idx)
        {
            polygon.push_back(transformed[i]);
            const auto &clip = polygon.back().clip;
            if ((forceFlags >= 0 ? uint32_t(forceFlags) : draw.renderFlags) & 2)
                for (uint32_t axis = 0; axis < 3; ++axis)
                    clipped |= std::fabs(clip[axis]) > std::fabs(clip[3]);
        }
        if (clipped)
        {
            for (const auto axis : {2u, 0u, 1u})
                for (const float sign : {1.0f, -1.0f})
                {
                    if (polygon.empty())
                        break;
                    std::vector<ReferenceVertex> output;
                    for (size_t edge = 0; edge < polygon.size(); ++edge)
                    {
                        const auto &previous = polygon[edge];
                        const auto &current = polygon[(edge + 1) % polygon.size()];
                        const bool before = std::fabs(previous.clip[3]) + sign * previous.clip[axis] >= 0;
                        const bool after = std::fabs(current.clip[3]) + sign * current.clip[axis] >= 0;
                        if (before)
                            output.push_back(previous);
                        if (before != after)
                        {
                            const float d0 = previous.clip[3] + sign * previous.clip[axis];
                            const float d1 = current.clip[3] + sign * current.clip[axis];
                            const float fraction = std::fabs(d0 / (d1 - d0));
                            if (!std::isfinite(fraction))
                                return false;
                            ReferenceVertex vertex = previous;
                            auto interpolate = [&](const Vector &a, const Vector &b) {
                                Vector delta;
                                for (uint32_t i = 0; i < 4; ++i)
                                    delta[i] = b[i] - a[i];
                                return madd(a, splat(fraction), delta);
                            };
                            vertex.clip = interpolate(previous.clip, current.clip);
                            vertex.screen = interpolate(previous.screen, current.screen);
                            vertex.colour = interpolate(previous.colour, current.colour);
                            vertex.uv = interpolate(previous.uv, current.uv);
                            output.push_back(vertex);
                        }
                    }
                    polygon = std::move(output);
                }
        }
        if (polygon.empty())
            continue;
        for (size_t c = 2; c < polygon.size(); ++c)
        {
            ReferencePrimitive primitive;
            primitive.face = faceIndex;
            primitive.material = face.material;
            primitive.sourceTopology = face.topology;
            primitive.clipped = clipped;
            primitive.bucket = face.bucket;
            primitive.sourceCorner = uint32_t(corner);
            primitive.batch = batch;
            const std::array<size_t, 3> fan = {0, c - 1, c};
            for (size_t at = 0; at < 3; ++at)
            {
                auto vertex = polygon[fan[at]];
                const auto screen = clipped ? transform(draw.cameraViewport, vertex.clip) : vertex.screen;
                if (!std::isfinite(screen[3]) || screen[3] == 0)
                    return false;
                vertex.q = 1.0f / screen[3];
                const auto projected = product(screen, splat(vertex.q));
                if (!std::isfinite(projected[0]) || !std::isfinite(projected[1]) || !std::isfinite(projected[2]) ||
                    std::fabs(projected[0]) > 1e7f || std::fabs(projected[1]) > 1e7f || std::fabs(projected[2]) > 1e9f)
                    return false;
                vertex.x = uint32_t(int32_t(projected[0] * 16.0f)) & 65535;
                vertex.y = uint32_t(int32_t(projected[1] * 16.0f)) & 65535;
                vertex.z = uint32_t(int32_t(projected[2])) & 0xffffff;
                for (uint32_t i = 0; i < 4; ++i)
                    vertex.rgba[i] = uint8_t(int32_t(vertex.colour[i]));
                vertex.s = vertex.uv[0] * vertex.q;
                vertex.t = vertex.uv[1] * vertex.q;
                const float fog = madd(splat(draw.fog[0]), splat(draw.fog[3]), splat(vertex.q))[0];
                vertex.fog = uint8_t(int32_t(std::max(draw.fog[2], std::min(draw.fog[1], fog))));
                primitive.vertices[at] = vertex;
            }
            const auto &a = primitive.vertices[0];
            const auto &b = primitive.vertices[1];
            const auto &cc = primitive.vertices[2];
            const int64_t area = (int64_t(b.x) - a.x) * (int64_t(cc.y) - a.y) - (int64_t(b.y) - a.y) * (int64_t(cc.x) - a.x);
            if (area)
                out.push_back(std::move(primitive));
        }
    }
    flushBatch();
    if (firstOnly && out.size() > batchStart)
        out.resize(batchStart + 1);
    return true;
}

#endif
