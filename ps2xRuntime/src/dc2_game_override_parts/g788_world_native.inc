// G788 — WHOLE-WORLD NATIVE FixMDT, EE side (plans/phase-G788-ledger.md, ps2_g788_world_api.inc).
//
// Seams (all wrappers chain to whatever is registered at the address, so G775/G776/G785 still run):
//   0x13f360 DataAssignMDT__15mgCVisualFixMDT   entry: a FixMDT (re)assignment starts -> forget self
//   0x13ff60 CreateFacePacket__12mgCVisualMDT   entry: freeze the bucket topology + source streams
//                                               (FIX loading recycles temporary face/index storage
//                                               between buckets: G776 topology lifetime correction)
//   0x13f4e0 Draw__12mgCVisualMDT (dest form)    entry: classify + capture one FixMDT draw
//   0x1359d0 AddPacket__14mgCDrawManager         entry, called from Draw's bucket loop: substitute
//                                               the bucket chain's face geometry tags
//   0x135720 Draw__14mgCDrawManager (one slot)   entry: ordered-run census of the slot's nodes
// Every post-call point is avoided: the generated code may be preempted inside a guest loop and
// resume mid-function, so only ENTRY code is guaranteed to run exactly once per call (G787 §6).
#if defined(PS2X_G777_NATIVE_GL)
namespace g788 {
using RecompiledFunction = PS2Runtime::RecompiledFunction;
using dc2_native::Vector;
using dc2_native::Matrix;

constexpr uint32_t kFixVtable = 0x375140u;
constexpr uint32_t kDrawSeam = 0x13f4e0u, kAddPacket = 0x1359d0u, kMgrDraw = 0x135720u;
constexpr uint32_t kFixAssign = 0x13f360u, kFacePacket = 0x13ff60u;
constexpr uint32_t kRaAdd0 = 0x13f644u, kRaAdd1 = 0x13f668u; // Draw's two AddPacket call sites

static RecompiledFunction prevDraw = nullptr, prevAdd = nullptr, prevMgr = nullptr, prevAssign = nullptr,
                          prevFace = nullptr;

static inline bool ok(uint32_t a, size_t n) { return a >= 0x10000u && a < 0x02000000u && n <= 0x02000000u - a; }
static inline uint32_t rd(const uint8_t *ram, uint32_t a) { uint32_t v; std::memcpy(&v, ram + a, 4); return v; }
static inline uint16_t rh(const uint8_t *ram, uint32_t a) { uint16_t v; std::memcpy(&v, ram + a, 2); return v; }
static inline Vector vecAt(const uint8_t *ram, uint32_t a) { Vector v{}; if (ok(a, 16)) std::memcpy(v.data(), ram + a, 16); return v; }
static inline Matrix matAt(const uint8_t *ram, uint32_t a) { Matrix m{}; for (uint32_t i = 0; i < 4; ++i) m[i] = vecAt(ram, a + i * 16); return m; }
static inline uint32_t sf() { return g_dc2ScriptFrame.load(std::memory_order_relaxed); }

// ---- load-time structured registry --------------------------------------------------------------
struct FrozenFace { uint32_t address = 0; std::array<uint8_t, 0x18> header{}; std::vector<uint8_t> indices; };
struct FrozenBucket { std::array<uint8_t, 0x20> header{}; std::vector<FrozenFace> faces; };
struct Source { bool have = false; std::array<uint32_t, 4> counts{}; std::array<std::vector<uint8_t>, 4> arrays; };
struct Entry
{
    uint64_t version = 0;
    Source source;
    std::unordered_map<uint32_t, FrozenBucket> buckets; // by bucket address
    std::shared_ptr<const dc2_native::Mesh> mesh;       // built at the first draw
    std::shared_ptr<const G788MeshGpu> gpu;             // immutable GPU source of `mesh`
    std::unordered_map<uint32_t, uint32_t> bucketFaceBase; // bucket address -> first face index
    std::vector<uint8_t> faceHold;                      // strip face with an all-zero vertex colour (G52 hold risk)
    uint32_t buildFail = 0;                             // nonzero: refusal reason of the build
};
static std::unordered_map<uint32_t, Entry> g_registry;
static uint64_t g_version = 0;

// ---- census ----------------------------------------------------------------------------------------
enum Reason : uint32_t
{
    kRDest = 0, kRRenderer, kRAlpha, kRFlags, kRFog, kRLights, kRMaterials, kRNoMesh, kRMeshBuild, kRNonFinite,
    kRBucketShape, kRClipMode, kRLightingMode, kRProjection, kRCount
};
static const char *const kReasonName[kRCount] = {"dest", "renderer", "alpha", "flags", "fog", "lights",
                                                 "materials", "nomesh", "build", "nonfinite", "bucket", "clipmode", "lightingmode", "projection"};
struct Census
{
    uint64_t seam = 0, fix = 0, elig = 0, refused[kRCount]{};
    uint64_t buckets = 0, faces = 0, corners = 0, tris = 0;
    uint64_t nodes = 0, chainOk = 0, chainBad = 0, faceSubst = 0, blobQw = 0, allocFail = 0;
    uint64_t assigns = 0, frozen = 0, frozenBad = 0, built = 0;
    uint64_t slots = 0, slotNodes = 0, slotSubst = 0, runs = 0, maxRun = 0, runNodes = 0, holdLegacy = 0, longStrip = 0,
             alreadyNative = 0;
    uint32_t lastSf = 0, frames = 0;
};
static Census g_c;
static std::map<uint32_t, uint64_t> g_progHist, g_hdrHist, g_clipHist;
static std::unordered_set<uint32_t> g_substChains; // this frame: bucket chains whose faces were substituted
static uint32_t g_substFrame = 0;
static uint32_t g_tagLogs = 0;
static uint32_t g_seq = 0; // native face job sequence (EE thread)

// DC2_G788_CLIPANY=1 (native): every clip mode drawn with the flags-3 clipping semantics. Only with a
// verify-mode proof that the VU program clips identically in that mode (plans/phase-G788-ledger.md).
static bool clipAny()
{
    static const bool v = [] { const char *e = std::getenv("DC2_G788_CLIPANY"); return e && *e && std::strcmp(e, "0") != 0; }();
    return v;
}
// DC2_G788_CLIPMODES=<mask> (native): the VU clip modes drawn natively with the draw's own flags
// (bit m = mode m). Default mode0: N independently proved the actual legacy front door.
static uint32_t clipMask()
{
    static const uint32_t v = [] { const char *e = std::getenv("DC2_G788_CLIPMODES"); return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0x1u; }();
    return v;
}
static bool censusOn()
{
    static const bool v = [] { const char *e = std::getenv("DC2_G788_CENSUS"); return e && *e && std::strcmp(e, "0") != 0; }();
    return v;
}

static void report()
{
    const uint32_t f = sf();
    if (f == g_c.lastSf)
        return;
    if (g_c.lastSf != 0)
        ++g_c.frames;
    g_c.lastSf = f;
    static uint32_t lastBucket = 0;
    if (!censusOn() || f / 600u == lastBucket)
        return;
    lastBucket = f / 600u;
    char reasons[512];
    int at = 0;
    for (uint32_t r = 0; r < kRCount && at < 480; ++r)
        at += std::snprintf(reasons + at, sizeof(reasons) - at, " %s=%llu", kReasonName[r],
                            static_cast<unsigned long long>(g_c.refused[r]));
    std::fprintf(stderr,
                 "[G788:census] sf=%u frames=%u seam=%llu fix=%llu elig=%llu buckets=%llu faces=%llu corners=%llu "
                 "tris=%llu nodes=%llu chainOk=%llu chainBad=%llu faceSubst=%llu blobQw=%llu allocFail=%llu "
                 "assigns=%llu frozen=%llu frozenBad=%llu built=%llu slots=%llu slotNodes=%llu slotSubst=%llu "
                 "runs=%llu runNodes=%llu maxRun=%llu | refused:%s\n",
                 f, g_c.frames, (unsigned long long)g_c.seam, (unsigned long long)g_c.fix, (unsigned long long)g_c.elig,
                 (unsigned long long)g_c.buckets, (unsigned long long)g_c.faces, (unsigned long long)g_c.corners,
                 (unsigned long long)g_c.tris, (unsigned long long)g_c.nodes, (unsigned long long)g_c.chainOk,
                 (unsigned long long)g_c.chainBad, (unsigned long long)g_c.faceSubst, (unsigned long long)g_c.blobQw,
                 (unsigned long long)g_c.allocFail, (unsigned long long)g_c.assigns, (unsigned long long)g_c.frozen,
                 (unsigned long long)g_c.frozenBad, (unsigned long long)g_c.built, (unsigned long long)g_c.slots,
                 (unsigned long long)g_c.slotNodes, (unsigned long long)g_c.slotSubst, (unsigned long long)g_c.runs,
                 (unsigned long long)g_c.runNodes, (unsigned long long)g_c.maxRun, reasons);
    std::string prog, hdr;
    for (const auto &p : g_progHist) { char b[40]; std::snprintf(b, sizeof(b), " %x:%llu", p.first, (unsigned long long)p.second); prog += b; }
    for (const auto &p : g_hdrHist) { char b[40]; std::snprintf(b, sizeof(b), " %x:%llu", p.first, (unsigned long long)p.second); hdr += b; }
    auto L = [](const std::atomic<uint64_t> &a) { return (unsigned long long)a.load(std::memory_order_relaxed); };
    std::string clip;
    for (const auto &p : g_clipHist) { char b[40]; std::snprintf(b, sizeof(b), " %x:%llu", p.first, (unsigned long long)p.second); clip += b; }
    std::fprintf(stderr, "[G788:hist] sf=%u vuprog{%s } facehdr0{%s } clipmode{%s } holdLegacy=%llu longStrip=%llu alreadyNative=%llu\n", f,
                 prog.c_str(), hdr.c_str(), clip.c_str(), (unsigned long long)g_c.holdLegacy, (unsigned long long)g_c.longStrip,
                 (unsigned long long)g_c.alreadyNative);
    const G788Counters &c = g_g788C;
    std::fprintf(stderr, "[G788:runs] sf=%u runs=%llu mergedFaces=%llu\n", f, L(c.xRuns), L(c.xMerged));
    std::fprintf(stderr, "[G788:primer] sf=%u macros=%llu referenceFaces=%llu declined=%llu\n", f,
                 L(c.primerMacros), L(c.primerReference), L(c.primerDeclined));
    std::fprintf(stderr,
                 "[G788:native] sf=%u parse(markers=%llu drop=%llu orphans=%llu macros=%llu empty=%llu fallbackFaces=%llu "
                 "fallbackPrims=%llu refFail=%llu) exec(macros=%llu cpuExpand=%llu cpuFaces=%llu stale=%llu jobs=%llu tris=%llu "
                 "bound=%llu) gl(dispatch=%llu jobs=%llu tris=%llu meshUploads=%llu uploadBytes=%llu draws=%llu fail=%llu "
                 "arenaGrow=%llu) latchConsumed=%llu\n",
                 f, L(c.markers), L(c.dropMarkers), L(c.orphans), L(c.macros), L(c.emptyFaces), L(c.fallbackFaces),
                 L(c.fallbackPrims), L(c.refPrimFail), L(c.xMacros), L(c.xCpuExpand), L(c.xCpuFaces), L(c.xStale), L(c.xJobs),
                 L(c.xTris), L(c.xBound), L(c.gDispatch), L(c.gJobs), L(c.gTris), L(c.gMeshUploads), L(c.gUploadBytes),
                 L(c.gDraws), L(c.gFail), L(c.gArenaGrow), L(dc2_g778::g788LatchConsumed));
    std::fprintf(stderr,
                 "[G788:verify] sf=%u vu(faces=%llu ok=%llu bad=%llu zero=%llu missing=%llu extra=%llu) "
                 "gpu(batches=%llu faces=%llu tris=%llu bad=%llu missing=%llu extra=%llu shaderFail=%llu refRefused=%llu)\n",
                 f, L(c.vFaces), L(c.vOk), L(c.vBad), L(c.vZero), L(c.vMissing), L(c.vExtra), L(c.gvBatches), L(c.gvFaces),
                 L(c.gvTris), L(c.gvBad), L(c.gvMissing), L(c.gvExtra), L(c.gvShaderFail), L(c.gvRefRefused));
    std::fprintf(stderr,
                 "[G788:vmode] sf=%u faces/badA/badB clip0=%llu/%llu/%llu clip1=%llu/%llu/%llu clip2=%llu/%llu/%llu "
                 "clip3=%llu/%llu/%llu\n",
                 f, L(c.vmFaces[0]), L(c.vmBadA[0]), L(c.vmBadB[0]), L(c.vmFaces[1]), L(c.vmBadA[1]), L(c.vmBadB[1]),
                 L(c.vmFaces[2]), L(c.vmBadA[2]), L(c.vmBadB[2]), L(c.vmFaces[3]), L(c.vmBadA[3]), L(c.vmBadB[3]));
    std::fprintf(stderr, "[G788:vkick] sf=%u inFace(draw=%llu adc=%llu) between(draw=%llu adc=%llu)\n", f,
                 L(g_g788K.draw[1]), L(g_g788K.noDraw[1]), L(g_g788K.draw[2]), L(g_g788K.noDraw[2]));
    for (uint32_t mode : {0u, 1u, 3u})
        std::fprintf(stderr,
                     "[G788:faceoracle] sf=%u mode=%u faces=%llu input=%llu expected=%llu zero=%llu g458=%llu "
                     "A(candidate=%llu fans=%llu missing=%llu extra=%llu attributes=%llu badFaces=%llu) "
                     "B(candidate=%llu fans=%llu missing=%llu extra=%llu attributes=%llu badFaces=%llu)\n",
                     f, mode, L(c.vmFaces[mode]), L(c.vmInput[mode]), L(c.vmExpected[mode]), L(c.vmZero[mode]),
                     L(c.vmCulled[mode]), L(c.vmCandidateA[mode]), L(c.vmFansA[mode]), L(c.vmMissingA[mode]),
                     L(c.vmExtraA[mode]), L(c.vmAttrA[mode]), L(c.vmBadA[mode]), L(c.vmCandidateB[mode]),
                     L(c.vmFansB[mode]), L(c.vmMissingB[mode]), L(c.vmExtraB[mode]), L(c.vmAttrB[mode]), L(c.vmBadB[mode]));
}

// ---- 0x13f360: a FixMDT (re)assignment starts --------------------------------------------------
static void assignWrapper(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4) & 0x1fffffffu;
    g_registry.erase(self);
    Entry &e = g_registry[self];
    e.version = ++g_version;
    ++g_c.assigns;
    prevAssign(ram, ctx, runtime);
}

// ---- 0x13ff60: freeze bucket topology (G776 rememberLoadedBuckets, any FixMDT) + source streams --
static void facePacketWrapper(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = getRegU32(ctx, 4) & 0x1fffffffu, firstFace = getRegU32(ctx, 6) & 0x1fffffffu;
    auto it = g_registry.find(self);
    if (it != g_registry.end() && ok(self, 0x50) && ok(firstFace, 0x18) && rd(ram, self + 0x1c) == kFixVtable)
    {
        Entry &e = it->second;
        if (!e.source.have)
        {
            bool good = true;
            for (uint32_t i = 0; i < 4 && good; ++i)
            {
                const uint32_t count = rd(ram, self + 0x20 + i * 4), address = rd(ram, self + 0x30 + i * 4) & 0x1fffffffu;
                if (count > 65536u || (count && !ok(address, size_t(count) * 16)))
                    good = false;
                else
                {
                    e.source.counts[i] = count;
                    if (count)
                        e.source.arrays[i].assign(ram + address, ram + address + size_t(count) * 16);
                }
            }
            e.source.have = good;
            if (!good)
                e.buildFail = 1;
        }
        const uint16_t material = rh(ram, firstFace + 4);
        uint32_t bucket = rd(ram, self + 0x48) & 0x1fffffffu;
        for (uint32_t b = 0; b < 256 && ok(bucket, 0x20); ++b)
        {
            bool contains = false;
            if (rd(ram, bucket) == material)
            {
                uint32_t p = rd(ram, bucket + 4) & 0x1fffffffu;
                for (uint32_t n = 0; n < 256 && ok(p, 0x18); ++n)
                {
                    if (p == firstFace) { contains = true; break; }
                    p = rd(ram, p + 0x10) & 0x1fffffffu;
                }
            }
            if (contains)
            {
                FrozenBucket copy;
                std::memcpy(copy.header.data(), ram + bucket, copy.header.size());
                uint32_t face = rd(ram, bucket + 4) & 0x1fffffffu;
                bool complete = true;
                const auto prior = e.buckets.find(bucket);
                for (uint32_t n = 0; face && n < 256; ++n)
                {
                    if (!ok(face, 0x18)) { complete = false; break; }
                    FrozenFace f;
                    f.address = face;
                    std::memcpy(f.header.data(), ram + face, f.header.size());
                    const uint16_t words = rh(ram, face + 6);
                    const uint32_t address = rd(ram, face + 12) & 0x1fffffffu;
                    if (!ok(address, size_t(words) * 4)) { complete = false; break; }
                    const FrozenFace *cached = nullptr;
                    if (prior != e.buckets.end())
                        for (const auto &previous : prior->second.faces)
                            if (previous.address == face) { cached = &previous; break; }
                    if (cached)
                    {
                        if (cached->indices.size() != size_t(words) * 4 || std::memcmp(cached->header.data(), f.header.data(), 12))
                        { complete = false; break; }
                        f.indices = cached->indices;
                    }
                    else
                        f.indices.assign(ram + address, ram + address + size_t(words) * 4);
                    copy.faces.push_back(std::move(f));
                    face = rd(ram, face + 0x10) & 0x1fffffffu;
                }
                if (face)
                    complete = false;
                if (complete && !copy.faces.empty())
                {
                    e.buckets[bucket] = std::move(copy);
                    ++g_c.frozen;
                }
                else
                    ++g_c.frozenBad;
            }
            bucket = rd(ram, bucket + 8) & 0x1fffffffu;
        }
    }
    prevFace(ram, ctx, runtime);
}

// ---- mesh build (first draw of a registered FixMDT; G778 capture generalised to any mesh) --------
// Returns 0 or a refusal code. The frozen topology is checked against the LIVE bucket list (same
// buckets in the same order, same face count, same 12 header bytes per face).
static uint32_t buildMesh(const uint8_t *ram, uint32_t self, Entry &e)
{
    dc2_g788_econ::event(dc2_g788_econ::TopologyBuild);
    dc2_g788_econ::Scope construction(dc2_g788_econ::TopologyConstruction);
    if (!e.source.have)
        return 1;
    auto mesh = std::make_shared<dc2_native::Mesh>();
    auto sourceVector = [&](uint32_t stream, uint32_t index, Vector &value) {
        if (index >= e.source.counts[stream] || size_t(index) * 16 + 16 > e.source.arrays[stream].size())
            return false;
        std::memcpy(value.data(), e.source.arrays[stream].data() + size_t(index) * 16, 16);
        return true;
    };
    uint32_t bucket = rd(ram, self + 0x48) & 0x1fffffffu;
    for (uint32_t b = 0; b < 256 && ok(bucket, 0x20); ++b)
    {
        const auto retained = e.buckets.find(bucket);
        if (retained == e.buckets.end())
            return 2;
        const uint32_t material = rd(ram, bucket);
        e.bucketFaceBase[bucket] = uint32_t(mesh->faces.size());
        for (const auto &face : retained->second.faces)
        {
            uint16_t header[6];
            std::memcpy(header, face.header.data(), sizeof(header));
            ++g_hdrHist[header[0]];
            const bool colour = (header[0] & 0x100) != 0;
            if ((header[0] & ~0x107u) || ((header[0] & 7) != 3 && (header[0] & 7) != 4) || header[1] != 3 + colour ||
                face.indices.size() != size_t(header[3]) * 4 || header[3] != header[1] * header[4])
                return 3;
            dc2_native::Face output;
            output.topology = header[0] & 7;
            output.material = material;
            output.bucket = bucket;
            output.batchCorners = ((rd(ram, self + 0x14) - 2) / header[1] / 3) * 3;
            if (output.batchCorners < 3)
                return 4;
            for (uint32_t v = 0; v < header[4]; ++v)
            {
                uint32_t index[4]{};
                std::memcpy(index, face.indices.data() + size_t(v) * header[1] * 4, header[1] * 4);
                dc2_native::Vertex vertex;
                vertex.colour = {1, 1, 1, 1};
                if (!sourceVector(0, index[0], vertex.position) || !sourceVector(1, index[1], vertex.normal) ||
                    !sourceVector(3, index[2], vertex.uv) || (colour && !sourceVector(2, index[3], vertex.colour)))
                    return 5;
                if (vertex.uv[2] != 1.0f)
                    return 6;
                for (const auto *values : {&vertex.position, &vertex.normal, &vertex.uv, &vertex.colour})
                    for (const auto value : *values)
                        if (!std::isfinite(value))
                            return 7;
                output.indices.push_back(uint32_t(mesh->vertices.size()));
                mesh->vertices.push_back(vertex);
            }
            mesh->faces.push_back(std::move(output));
        }
        bucket = rd(ram, bucket + 8) & 0x1fffffffu;
    }
    if (mesh->faces.empty())
        return 8;
    uint64_t h = 14695981039346656037ull;
    auto hash = [&](const void *p, size_t n) { const auto *q = static_cast<const uint8_t *>(p); for (size_t i = 0; i < n; ++i) h = (h ^ q[i]) * 1099511628211ull; };
    hash(mesh->vertices.data(), mesh->vertices.size() * sizeof(dc2_native::Vertex));
    for (const auto &face : mesh->faces)
    {
        hash(&face.topology, sizeof(face.topology));
        hash(&face.material, sizeof(face.material));
        hash(&face.bucket, sizeof(face.bucket));
        hash(&face.batchCorners, sizeof(face.batchCorners));
        hash(face.indices.data(), face.indices.size() * sizeof(uint32_t));
    }
    mesh->identity = h;
    // GPU source (G777 layout + per-face batch info), in the reference's emission source order.
    auto gpu = std::make_shared<G788MeshGpu>();
    gpu->identity = h;
    e.faceHold.assign(mesh->faces.size(), 0u);
    for (uint32_t f = 0; f < mesh->faces.size(); ++f)
    {
        const auto &face = mesh->faces[f];
        gpu->faceTri.push_back(gpu->triangles);
        std::vector<std::array<uint32_t, 3>> tris;
        std::vector<uint32_t> batch;
        for (uint32_t corner = 2; corner < face.indices.size(); ++corner)
        {
            if (face.topology == 3 && corner % 3 != 2)
                continue;
            std::array<uint32_t, 3> t = {corner - 2, corner - 1, corner};
            if (face.topology == 4 && (corner & 1))
                std::swap(t[0], t[2]);
            tris.push_back(t);
            batch.push_back(face.batchCorners ? corner / face.batchCorners : 0u);
        }
        for (uint32_t i = 0; i < tris.size(); ++i)
        {
            uint32_t first = i, count = 0;
            while (first > 0 && batch[first - 1] == batch[i])
                --first;
            while (first + count < tris.size() && batch[first + count] == batch[i])
                ++count;
            for (const uint32_t c : tris[i])
            {
                const auto &v = mesh->vertices[face.indices[c]];
                gpu->source.push_back(v.position);
                gpu->source.push_back(v.normal);
                gpu->source.push_back(v.uv);
                gpu->source.push_back(v.colour);
            }
            gpu->source.push_back({float(i), float(first), float(count), float(face.topology)});
        }
        gpu->triangles += uint32_t(tris.size());
        if (face.topology == 4)
            for (const uint32_t vi : face.indices)
            {
                const auto &c = mesh->vertices[vi].colour;
                if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0)
                    e.faceHold[f] = 1u;
            }
    }
    gpu->faceTri.push_back(gpu->triangles);
    gpu->mesh = mesh;
    // DIAGNOSTIC mesh fixture (DC2_G788_MESHFIX=N): the first N meshes: per face the frozen header, the
    // live geometry tag + its static face packet (the VU INPUT) and the structured corners -> mesh.bin.
    static const uint32_t meshFix = [] { const char *v = std::getenv("DC2_G788_MESHFIX"); return v ? uint32_t(std::strtoul(v, nullptr, 0)) : 0u; }();
    static uint32_t meshFixed = 0;
    if (meshFix && meshFixed < meshFix)
    {
        ++meshFixed;
        static FILE *mf = std::fopen("logs/g788/mesh.bin", "wb");
        if (mf)
        {
            uint32_t bucket = rd(ram, self + 0x48) & 0x1fffffffu, fi = 0;
            for (uint32_t b = 0; b < 256 && ok(bucket, 0x20); ++b)
            {
                uint32_t face = rd(ram, bucket + 4) & 0x1fffffffu;
                for (uint32_t n = 0; face && n < 256 && ok(face, 0x30) && fi < mesh->faces.size(); ++n, ++fi)
                {
                    const auto &mfc = mesh->faces[fi];
                    uint32_t tag[4];
                    std::memcpy(tag, ram + face + 0x20, 16);
                    const uint32_t qwc = tag[0] & 0xffffu, addr = tag[1] & 0x1fffffffu;
                    const uint32_t take = (ok(addr, size_t(qwc) * 16) && qwc <= 8192u) ? qwc : 0u;
                    const uint32_t hdr[8] = {0x4853454Du, self, fi, uint32_t(mfc.indices.size()), take, mfc.topology, mfc.batchCorners,
                                             rd(ram, self + 0x14)};
                    std::fwrite(hdr, sizeof(hdr), 1, mf);
                    std::fwrite(ram + face, 0x18, 1, mf);
                    std::fwrite(tag, 16, 1, mf);
                    if (take)
                        std::fwrite(ram + addr, size_t(take) * 16, 1, mf);
                    for (const uint32_t vi : mfc.indices)
                    {
                        const auto &v = mesh->vertices[vi];
                        std::fwrite(v.position.data(), 16, 1, mf);
                        std::fwrite(v.normal.data(), 16, 1, mf);
                        std::fwrite(v.uv.data(), 16, 1, mf);
                        std::fwrite(v.colour.data(), 16, 1, mf);
                    }
                    face = rd(ram, face + 0x10) & 0x1fffffffu;
                }
                bucket = rd(ram, bucket + 8) & 0x1fffffffu;
            }
            std::fflush(mf);
        }
    }
    e.gpu = std::move(gpu);
    e.mesh = std::move(mesh);
    ++g_c.built;
    return 0;
}

// ---- per-draw record --------------------------------------------------------------------------
struct Pending
{
    uint32_t renderInfo = 0, self = 0, frame = 0, memory = 0;
    uint32_t worldPtr = 0, manager = 0, clipMode = 0; // fixture dump
    bool dumped = false;
    std::shared_ptr<const dc2_native::Mesh> mesh;
    std::shared_ptr<const G788DrawRec> rec; // native mode: the draw's immutable record
    const Entry *entry = nullptr;
    bool eligible = false;
    bool shadowDrop = false; // G790 S-probe: a ShadowMDT draw whose face tags all become DROP markers
    bool shadowDest = false; // ... drawn in DEST form: substituted at the dest loop's mgSendVuProg call
    Matrix shadowMat{};      // G790 R probe: the Draw's matrix argument (a2), published per draw
};
static Pending g_pending; // the draw whose AddPacket loop is running (EE thread)

// G790 (CORRUPTING ceiling probe, DC2_G790_SHADOW_S=1 with DC2_G788_WORLD=native): every mgCShadowMDT
// (vtable 0x375020) draw that reaches the shared Draw seam in manager form (dest == 0) has its bucket
// chains' face geometry tags replaced by DROP markers at AddPacket, exactly as WORLD=drop does for
// FixMDT. renderInfo (matrices, SetShadowData), chains, every GS state DIRECT and the shadow RTT pass stay;
// only the faces' VIF unpack / VU1 / PATH1 / parse / raster go: the S-ceiling of a native ShadowMDT.
// ⛔ The first build substituted only at AddPacket and was INERT: every ShadowMDT draw on Dungeon1 and Combat is
// DEST form (`[G790:shs] dest == draws`). In dest form Draw writes CALL renderInfo, then per bucket
// `mgSendVuProg(dest, bucket+0xC)` (return site 0x13F5A0, Draw's s0 = the bucket) and CALL bucket+0x10 — the
// same per-frame bucket chain CreatePacket has just built. The dest-form substitution therefore runs at the ENTRY
// of that mgSendVuProg call (entry-only: safe under mid-function preemption).
constexpr uint32_t kShadowVtable = 0x375020u;
constexpr uint32_t kSendVuProg = 0x145e80u, kRaDestLoop = 0x13f5a0u;
static RecompiledFunction prevSendVuProg = nullptr;
static bool shadowS()
{
    static const bool on = std::getenv("DC2_G790_SHADOW_S") != nullptr;
    return on;
}
struct ShadowSCensus { uint64_t draws = 0, dest = 0, nodes = 0, bad = 0, subst = 0; uint32_t lastSf = 0; };
static ShadowSCensus g_shs;

// G790 R probe (DC2_G790_SHADOW_R=1, NOT corrupting: legacy shadow rendering stays authoritative). At the same
// dest-loop seam it performs the EE-side work a host-native ShadowMDT path would do per face and TIMES it with its
// own clock: source identification by (visual, face); an immutable source frozen once (the face's triangle-list
// indices expanded through the static vertex array self+0x30, exactly what CreateFacePacket re-copies every
// frame); per-draw matrix publication; host transform + per-triangle facing + ADD/SUB run building into a reused
// payload buffer. The output is discarded. GL draws + raster of the same primitives are priced separately by the
// G arm (DC2_G762_ONLY=9); the parse/executor job transport by G788 Phase D.
static bool shadowR()
{
    static const bool on = std::getenv("DC2_G790_SHADOW_R") != nullptr;
    return on;
}
struct ShadowSrc { std::vector<Vector> tri; };
struct ShadowRCensus
{
    uint64_t draws = 0, tris = 0, runs = 0, ns = 0, freezeNs = 0, frozen = 0, refused = 0, frames = 0;
    uint32_t lastSf = 0, lastFrame = ~0u;
    std::vector<Vector> payload;
};
static std::unordered_map<uint64_t, ShadowSrc> g_shSrc;
static ShadowRCensus g_shr;

static uint32_t classify(const uint8_t *ram, R5900Context *ctx, uint32_t self, uint32_t manager, Pending &p)
{
    dc2_g788_econ::Scope econ(dc2_g788_econ::Classify);
    if (getRegU32(ctx, 5) != 0u)
        return kRDest;
    const uint32_t renderer = rd(ram, manager + 0x64) & 0x1fffffffu;
    if (!ok(renderer, 0x1100))
        return kRRenderer;
    const uint32_t config = rd(ram, renderer + 0xfcc) & 0x1fffffffu;
    if (!ok(config, 0x90))
        return kRRenderer;
    if (vecAt(ram, config + 0x80)[3] > 1)
        return kRAlpha;
    if (rd(ram, config + 0x40) || rd(ram, config + 0x2c) || rd(ram, renderer + 0xfc8) || rd(ram, renderer + 0x1010) ||
        rd(ram, config + 0x84) == 1)
        return kRFlags;
    if (!rd(ram, config + 0x60) && rd(ram, renderer + 0xfac) && rd(ram, config + 0x4c))
        return kRFog;
    // VU flags bit 0x20 selects 0xcb0 (material * VU20), rather than directional lighting.
    // The structured lit reference does not represent that variant; preserve its legacy path.
    if (rd(ram, config + 0x60) || !rd(ram, renderer + 0xfac))
        return kRLightingMode;
    // The VU flags word (CreateRenderInfoPacket -> VU 0x26) bits 0/1 = renderer+0xfc0|0xfc4 / 0xfc4. The
    // N independently proves mode0: bit1 clear skips six-plane clipping, bit0 clear
    // skips the screen/depth ADC emitter. Mode1 ADC and mode3 ADC-packed fog remain
    // separate contracts; only independently proven modes enter the native mask.
    const uint32_t clipMode = uint32_t(bool(rd(ram, renderer + 0xfc0) || rd(ram, renderer + 0xfc4))) |
                              (uint32_t(bool(rd(ram, renderer + 0xfc4))) << 1);
    ++g_clipHist[clipMode];
    if (g_g788Mode == 3 && !((clipMask() >> clipMode) & 1u) && !clipAny())
        return kRClipMode;
    const uint32_t lights = renderer + 0x400 + rd(ram, renderer + 0x3f4) * 0x150;
    if (!ok(lights, 0x150))
        return kRLights;
    const uint32_t materialCount = rd(ram, self + 0x40), materials = rd(ram, self + 0x44) & 0x1fffffffu;
    if (!materialCount || materialCount > 256 || !ok(materials, materialCount * 0x30))
        return kRMaterials;
    auto it = g_registry.find(self);
    if (it == g_registry.end() || !it->second.source.have)
        return kRNoMesh;
    Entry &e = it->second;
    if (!e.mesh && !e.buildFail)
        e.buildFail = buildMesh(ram, self, e) ? 2u : 0u;
    if (!e.mesh)
        return kRMeshBuild;
    // The live bucket list must still be the frozen one (count + face count per bucket).
    uint32_t bucket = rd(ram, self + 0x48) & 0x1fffffffu, buckets = 0;
    size_t faces = 0;
    for (uint32_t b = 0; b < 256 && ok(bucket, 0x20); ++b, ++buckets)
    {
        const auto fb = e.buckets.find(bucket);
        if (fb == e.buckets.end())
            return kRBucketShape;
        uint32_t face = rd(ram, bucket + 4) & 0x1fffffffu, n = 0;
        for (; face && n < 256; ++n)
        {
            if (!ok(face, 0x30) || n >= fb->second.faces.size() ||
                std::memcmp(ram + face, fb->second.faces[n].header.data(), 12) != 0)
                return kRBucketShape;
            face = rd(ram, face + 0x10) & 0x1fffffffu;
        }
        if (n != fb->second.faces.size())
            return kRBucketShape;
        faces += n;
        ++g_progHist[rd(ram, bucket + 0xc)];
        bucket = rd(ram, bucket + 8) & 0x1fffffffu;
    }
    if (faces != e.mesh->faces.size())
        return kRBucketShape;
    p.mesh = e.mesh;
    p.entry = &e;
    p.worldPtr = getRegU32(ctx, 6) & 0x1fffffffu;
    p.manager = manager;
    p.clipMode = clipMode;
    if (g_g788Mode == 3 || g_g788Mode == 4)
    {
        // G778 capture, generalised: the structured engine inputs of this draw.
        auto rec = std::make_shared<G788DrawRec>();
        auto &d = rec->draw;
        d.geometry = e.mesh;
        d.frame = sf();
        d.world = matAt(ram, getRegU32(ctx, 6) & 0x1fffffffu);
        d.cameraScreen = matAt(ram, renderer + 0x10);
        d.cameraClip = matAt(ram, renderer + 0x220);
        d.cameraViewport = matAt(ram, renderer + 0x260);
        d.renderFlags = uint32_t(bool(rd(ram, renderer + 0xfc0) || rd(ram, renderer + 0xfc4))) |
                        (uint32_t(bool(rd(ram, renderer + 0xfc4))) << 1);
        for (uint32_t i = 0; i < 3; ++i)
            d.lightDirections[i] = vecAt(ram, lights + i * 16);
        d.lightColours = matAt(ram, lights + 0x40);
        d.ambient = vecAt(ram, lights + 0x80);
        d.ambient[3] *= vecAt(ram, config + 0x40)[1];
        const auto fog = vecAt(ram, renderer + 0xfdc);
        d.fog = {fog[0], fog[2], fog[1], fog[3]};
        for (uint32_t i = 0; i < materialCount; ++i)
            d.materials.push_back({vecAt(ram, materials + i * 0x30), rd(ram, materials + i * 0x30 + 0x20)});
        rec->mesh = e.gpu;
        rec->frame = d.frame;
        rec->clipMode = clipMode;
        if (g_g788Mode == 3 && clipAny())
            d.renderFlags = 3u;
        rec->constants.resize(materialCount);
        for (uint32_t m = 0; m < materialCount; ++m)
        {
            std::array<Vector, 23> c{};
            if (!dc2_native::buildNativeConstants(d, m, 1u, 0, 0, 0, 0, c))
                return kRNonFinite;
            // Loading may publish a zero screen matrix (observed Combat SF475).
            // Every source point then has W=0, outside the proven projection contract.
            // Refuse the whole draw before native commitment; retain original packets/VU.
            // The matrix is already built above: no source vertex transform or worker call.
            if(m==0u && c[0][3]==0.0f && c[1][3]==0.0f && c[2][3]==0.0f && c[3][3]==0.0f)
                return kRProjection;
            for (uint32_t k = 0; k < 21; ++k)
                rec->constants[m][k] = c[k];
        }
        if (d.renderFlags & ~3u)
            return kRFlags;
        p.rec = std::move(rec);
    }
    g_c.buckets += buckets;
    g_c.faces += faces;
    for (const auto &f : e.mesh->faces) {
        g_c.corners += f.indices.size();
        g_c.tris += f.topology == 3 ? f.indices.size() / 3 : (f.indices.size() >= 2 ? f.indices.size() - 2 : 0);
    }
    return kRCount; // eligible
}

// ---- 0x13f4e0 -----------------------------------------------------------------------------------
// Diagnostic missed-cost ceiling only: omit an unsupported FixMDT draw before
// any packet construction. Never a rendering-preserving gain or shipping mode.
static bool dropRefusal(uint8_t* ram,uint32_t self,uint32_t manager,uint32_t reason)
{
    static const int kind=[] {
        const char* e=std::getenv("DC2_G788_REFUSAL_DROP");
        if(!e || !std::strcmp(e,"off"))return -1;
        const char* names[]={"dest","renderer","alpha","flags","fog","lights","materials","nomesh","build","nonfinite","bucket","clip","lighting","projection"};
        for(unsigned i=0;i<kRCount;++i)if(!std::strcmp(e,names[i]))return int(i);
        if(!std::strcmp(e,"all"))return int(kRCount);
        if(!std::strcmp(e,"clip1"))return 100;
        if(!std::strcmp(e,"clip3"))return 101;
        std::fprintf(stderr,"[G788:missed] FATAL invalid category=%s\n",e);std::abort();return -1;
    }();
    if(kind<0 || g_g788Mode!=3 || reason==kRCount)return false;
    if(dc2_g778::requested()) {
        if(self==dc2_g784::kMeshSelf)return false;
        const auto existing=g_registry.find(self);
        if(existing!=g_registry.end())for(const auto& b:existing->second.buckets)
            if(dc2_g784::enabled(b.first))return false;
    }
    bool matches=kind==int(kRCount) || kind==int(reason);
    if(kind>=100 && reason==kRClipMode) {
        const uint32_t renderer=rd(ram,manager+0x64)&0x1fffffffu;
        const unsigned cm=uint32_t(bool(rd(ram,renderer+0xfc0)||rd(ram,renderer+0xfc4))) |
            (uint32_t(bool(rd(ram,renderer+0xfc4)))<<1);
        matches=cm==(kind==100?1u:3u);
    }
    if(matches && censusOn()) {
        static uint64_t drops=0;static uint32_t last=~0u;++drops;
        if(sf()/600!=last){last=sf()/600;std::fprintf(stderr,"[G788:missed] sf=%u kind=%d drops=%llu\n",sf(),kind,(unsigned long long)drops);}
    }
    return matches;
}
static void drawWrapper(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    report();
    ++g_c.seam;
    const uint32_t self = getRegU32(ctx, 4) & 0x1fffffffu;
    if (ok(self, 0x50) && rd(ram, self + 0x1c) == kFixVtable)
    {
        ++g_c.fix;
        const uint32_t manager = getRegU32(ctx, 7) ? getRegU32(ctx, 7) & 0x1fffffffu : 0x003820e0u;
        Pending p;
        p.self = self;
        p.frame = sf();
        const uint32_t memory = rd(ram, manager + 0x60) & 0x1fffffffu;
        if (ok(manager, 0x70) && ok(memory, 0x30))
        {
            p.memory = memory;
            p.renderInfo = (rd(ram, memory + 0x20) + rd(ram, memory + 0x24) * 16u) & 0x1fffffffu;
            const uint32_t r = classify(ram, ctx, self, manager, p);
            if (r == kRCount)
            {
                ++g_c.elig;
                p.eligible = true;
            }
            else
            {
                ++g_c.refused[r];
                if(dropRefusal(ram,self,manager,r)) {
                    g_pending=std::move(p);setReturnU32(ctx,0u);ctx->pc=getRegU32(ctx,31);return;
                }
            }
        }
        else
            ++g_c.refused[kRRenderer];
        g_pending = std::move(p);
    }
    else if ((shadowS() || shadowR()) && g_g788Mode >= 2 && ok(self, 0x50) && rd(ram, self + 0x1c) == kShadowVtable)
    {
        ++g_shs.draws;
        const uint32_t manager = getRegU32(ctx, 7) ? getRegU32(ctx, 7) & 0x1fffffffu : 0x003820e0u;
        Pending p;
        p.self = self;
        p.frame = sf();
        p.shadowDrop = shadowS();
        p.shadowMat = matAt(ram, getRegU32(ctx, 6) & 0x1fffffffu);
        const uint32_t memory = rd(ram, manager + 0x60) & 0x1fffffffu;
        if (getRegU32(ctx, 5) != 0u)
        {
            ++g_shs.dest;
            if (ok(manager, 0x70) && ok(memory, 0x30))
            {
                p.memory = memory;
                p.shadowDest = true;
            }
        }
        else if (ok(manager, 0x70) && ok(memory, 0x30))
        {
            p.memory = memory;
            p.renderInfo = (rd(ram, memory + 0x20) + rd(ram, memory + 0x24) * 16u) & 0x1fffffffu;
            p.eligible = true;
        }
        g_pending = std::move(p);
        if (sf() >= g_shs.lastSf + 600u)
        {
            g_shs.lastSf = sf();
            std::fprintf(stderr, "[G790:shs] sf=%u draws=%llu dest=%llu nodes=%llu bad=%llu subst=%llu\n", sf(),
                         (unsigned long long)g_shs.draws, (unsigned long long)g_shs.dest,
                         (unsigned long long)g_shs.nodes, (unsigned long long)g_shs.bad,
                         (unsigned long long)g_shs.subst);
        }
    }
    prevDraw(ram, ctx, runtime);
}

// Allocate qwords from the draw's packet mgCMemory (the guest's own Alloc semantics, 0x139d20).
static uint32_t allocQw(uint8_t *ram, uint32_t memory, uint32_t qw)
{
    if (rd(ram, memory + 0x1c) != 0u)
        return 0u;
    const uint32_t used = rd(ram, memory + 0x24), cap = rd(ram, memory + 0x28);
    if (used + qw >= cap)
        return 0u;
    const uint32_t at = (rd(ram, memory + 0x20) + used * 16u) & 0x1fffffffu;
    if (!ok(at, size_t(qw) * 16))
        return 0u;
    const uint32_t nu = used + qw;
    std::memcpy(ram + memory + 0x24, &nu, 4);
    return at;
}

// ---- 0x1359d0 -----------------------------------------------------------------------------------
// DIAGNOSTIC fixture (DC2_G788_FIXTURE=N): the first N eligible draws per clip mode, then every 512th:
// clip mode, world matrix, renderer 0..0x300, light block, config, and the COMPLETE renderInfo packet
// (80 qw) as the VU will receive it -> logs/g788/fixture.bin, for offline matrix-composition analysis.
static uint32_t fixtureN()
{
    static const uint32_t v = [] { const char *e = std::getenv("DC2_G788_FIXTURE"); return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u; }();
    return v;
}
static void fixtureDump(const uint8_t *ram)
{
    static FILE *f = nullptr;
    static uint64_t perMode[4]{};
    const uint32_t m = g_pending.clipMode & 3u;
    const uint64_t n = perMode[m]++;
    if (n >= fixtureN() && (n % 512u) != 0u)
        return;
    if (!f)
        f = std::fopen("logs/g788/fixture.bin", "wb");
    if (!f)
        return;
    const uint32_t renderer = rd(ram, g_pending.manager + 0x64) & 0x1fffffffu;
    const uint32_t config = ok(renderer, 0x1100) ? rd(ram, renderer + 0xfcc) & 0x1fffffffu : 0u;
    const uint32_t lights = renderer + 0x400 + rd(ram, renderer + 0x3f4) * 0x150;
    uint32_t hdr[16] = {0x38384746u, m, g_pending.self, renderer, config, g_pending.worldPtr, g_pending.renderInfo,
                        g_pending.frame, g_pending.manager, lights, 0, 0, 0, 0, 0, 0};
    std::fwrite(hdr, sizeof(hdr), 1, f);
    auto block = [&](uint32_t a, size_t bytes) {
        std::vector<uint8_t> z(bytes, 0);
        if (ok(a, bytes))
            std::memcpy(z.data(), ram + a, bytes);
        std::fwrite(z.data(), 1, bytes, f);
    };
    block(g_pending.worldPtr, 64);
    block(renderer, 0x300);
    block(renderer + 0xf80, 0x100);
    block(lights, 0x150);
    block(config, 0x90);
    block(g_pending.renderInfo, 80 * 16);
    std::fflush(f);
}
static void addWrapper(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    g788_packet_on_add(ram,ctx); // inert unless the bounded packet byte oracle is armed
    if(dc2_g788_econ::on())g788_front_on_add(ram,ctx);
    dc2_g788_econ::Scope econ(dc2_g788_econ::Publish, true,
        dc2_g788_econ::on() && g_pending.eligible &&
        (getRegU32(ctx,31) == kRaAdd0 || getRegU32(ctx,31) == kRaAdd1) &&
        dc2_g788_econ_add_allowed(getRegU32(ctx, 17) & 0x1fffffffu));
    const uint32_t ra = getRegU32(ctx, 31);
    if (fixtureN() && (ra == kRaAdd0 || ra == kRaAdd1) && g_pending.eligible && !g_pending.dumped &&
        (getRegU32(ctx, 6) & 0x1fffffffu) == g_pending.renderInfo && g_pending.frame == sf())
    {
        g_pending.dumped = true;
        fixtureDump(ram);
    }
    if ((ra == kRaAdd0 || ra == kRaAdd1) && g_pending.eligible &&
        (getRegU32(ctx, 6) & 0x1fffffffu) == g_pending.renderInfo && g_pending.frame == sf())
    {
        ++g_c.nodes;
        if (g_pending.shadowDrop)
            ++g_shs.nodes;
        const uint32_t bucket = getRegU32(ctx, 17) & 0x1fffffffu; // Draw's s1 = the bucket
        // G778/G784 already owns enabled E8B940 buckets. Its G782 group may delete the original
        // Path1 output, and extra face DIRECT markers change that group's transport contract.
        // Leave the entire existing group untouched, including its legacy fail-closed route.
        if (g_pending.self == dc2_g784::kMeshSelf && dc2_g778::requested() && dc2_g784::enabled(bucket))
        {
            ++g_c.alreadyNative;
            prevAdd(ram, ctx, runtime);
            return;
        }
        const uint32_t chain = getRegU32(ctx, 7) & 0x1fffffffu;
        uint32_t chainQw = ok(bucket, 0x20) ? rd(ram, bucket + 0x14) : 0u;
        // Walk the chain's qwords in order and match the live face tags (face+0x20) in order.
        std::vector<uint32_t> at;
        bool good = ok(bucket, 0x20) && rd(ram, bucket + 0x10) == chain && chainQw > 0u && chainQw < 4096u &&
                    ok(chain, size_t(chainQw) * 16);
        if (good)
        {
            uint32_t face = rd(ram, bucket + 4) & 0x1fffffffu, q = 0;
            for (uint32_t n = 0; face && n < 256 && good; ++n)
            {
                if (!ok(face, 0x30)) { good = false; break; }
                while (q < chainQw && std::memcmp(ram + chain + q * 16, ram + face + 0x20, 16) != 0)
                    ++q;
                if (q == chainQw) { good = false; break; }
                at.push_back(chain + q * 16);
                ++q;
                if (g_tagLogs < 6)
                {
                    ++g_tagLogs;
                    uint32_t w[4];
                    std::memcpy(w, ram + face + 0x20, 16);
                    std::fprintf(stderr, "[G788:facetag] self=%x bucket=%x face=%x tag=%08x %08x %08x %08x chainQw=%u\n",
                                 g_pending.self, bucket, face, w[0], w[1], w[2], w[3], chainQw);
                }
                face = rd(ram, face + 0x10) & 0x1fffffffu;
            }
            if (face)
                good = false;
        }
        if (!good || at.empty())
        {
            ++g_c.chainBad;
            if (g_pending.shadowDrop)
                ++g_shs.bad;
        }
        else
        {
            ++g_c.chainOk;
            // Native: which faces of this bucket go native (job seq per face); the rest keep their tag.
            std::vector<uint32_t> jobOf(at.size(), kG788DropJob);
            size_t natives = at.size();
            if ((g_g788Mode == 3 || g_g788Mode == 4) && !g_pending.shadowDrop)
            {
                natives = 0;
                const Entry *e = g_pending.entry;
                const auto base = e ? e->bucketFaceBase.find(bucket) : decltype(e->bucketFaceBase.end()){};
                if (!e || !g_pending.rec || base == e->bucketFaceBase.end())
                    ++g_c.chainBad;
                else
                    for (size_t k = 0; k < at.size(); ++k)
                    {
                        const uint32_t face = base->second + uint32_t(k);
                        const auto &mesh = *g_pending.rec->draw.geometry;
                        if (face >= mesh.faces.size()) { jobOf[k] = 0u; continue; }
                        const auto &mc = g_pending.rec->draw.materials[mesh.faces[face].material].colour;
                        if (g_g788Mode == 3 && mesh.faces[face].topology == 4 &&
                            (e->faceHold[face] || (mc[0] == 0 && mc[1] == 0 && mc[2] == 0 && mc[3] == 0)))
                        { jobOf[k] = 0u; ++g_c.holdLegacy; continue; } // G52 zero-colour hold: stays legacy
                        if (mesh.faces[face].topology == 4 && mesh.faces[face].batchCorners &&
                            mesh.faces[face].indices.size() > mesh.faces[face].batchCorners)
                        { jobOf[k] = 0u; ++g_c.longStrip; continue; } // multi-batch strip: outside the reference
                        uint32_t seq = ++g_seq;
                        if (seq == 0u || seq == kG788DropJob)
                            seq = g_seq = 1u;
                        G788Job &j = g_g788Ring[seq & (kG788Ring - 1u)];
                        j.seq.store(0u, std::memory_order_release);
                        j.rec = g_pending.rec;
                        j.face = face;
                        j.seq.store(seq, std::memory_order_release);
                        jobOf[k] = seq;
                        ++natives;
                    }
            }
            if (g_g788Mode == 4 && natives)
            {
                // Original geometry and DMA tags remain byte-for-byte legacy. The DMA
                // staging owner inserts diagnostic boundaries in its host copy instead.
                for (size_t k = 0; k < at.size(); ++k)
                {
                    if (!jobOf[k]) continue;
                    uint64_t original = 0;
                    std::memcpy(&original, ram + at[k], sizeof(original));
                    dc2_g788_watchRef(sf(), at[k], original, jobOf[k]);
                    ++g_c.faceSubst;
                }
            }
            else if (g_g788Mode >= 2 && natives)
            {
                const uint32_t blob = allocQw(ram, g_pending.memory, uint32_t(natives) * 2u);
                if (!blob)
                    ++g_c.allocFail;
                else
                {
                    g_c.blobQw += natives * 2u;
                    uint32_t slot = 0;
                    for (size_t k = 0; k < at.size(); ++k)
                    {
                        if (jobOf[k] == 0u)
                            continue;
                        const uint32_t m = blob + (slot++) * 32u;
                        const uint32_t vif[4] = {0u, 0u, 0u, 0x50000001u}; // NOP NOP NOP DIRECT 1qw
                        std::memcpy(ram + m, vif, 16);
                        const uint64_t tagLo = 0x8000ull | (1ull << 60); // NLOOP 0, EOP, PACKED, NREG 1
                        const uint64_t tagHi = (kG788MarkerMagic << 48) | jobOf[k];
                        std::memcpy(ram + m + 16, &tagLo, 8);
                        std::memcpy(ram + m + 24, &tagHi, 8);
                        // REF(blob, 2): keep the original tag's upper 64 bits (VIF codes under TTE).
                        const uint64_t ref = 2ull | (3ull << 28) | (static_cast<uint64_t>(m) << 32);
                        if(dc2_g788_econ::on() && g_g788Mode==3) {
                            uint64_t original=0;std::memcpy(&original,ram+at[k],8);
                            dc2_g788_econ_watch_ref(sf(),at[k],original,ref,jobOf[k]);
                        }
                        std::memcpy(ram + at[k], &ref, 8);
                        ++g_c.faceSubst;
                        if (g_pending.shadowDrop)
                            ++g_shs.subst;
                    }
                    if (g_substFrame != sf())
                    {
                        g_substFrame = sf();
                        g_substChains.clear();
                    }
                    g_substChains.insert(chain);
                }
            }
        }
    }
    if (g_g788Mode == 5 && (ra == kRaAdd0 || ra == kRaAdd1) && g_pending.eligible &&
        (getRegU32(ctx, 6) & 0x1fffffffu) == g_pending.renderInfo && g_pending.frame == sf())
    {
        // DROPALL probe: the node keeps its slot/key/program but transfers nothing (a 1-qw RET).
        const uint32_t ret = allocQw(ram, g_pending.memory, 1u);
        if (ret)
        {
            const uint64_t tag[2] = {6ull << 28, 0ull};
            std::memcpy(ram + ret, tag, 16);
            ctx->r[6] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(ret)));
            ctx->r[7] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(ret)));
            ++g_c.faceSubst;
        }
        else
            ++g_c.allocFail;
    }
    prevAdd(ram, ctx, runtime);
}

// G790 S-probe, dest form: replace every face geometry tag of `bucket`'s chain with a DROP marker REF (the
// marker format and blob allocation are AddPacket's, above).
// ⛔ A ShadowMDT chain is NOT a FixMDT chain: mgCShadowMDT::CreatePacket (0x13A560) rebuilds it EVERY FRAME as one
// REF tag per face (0x30000000 | qwc, ADDR = that face's packet, just written by CreateFacePacket into manager
// memory +0x60), then mgSetPkTexFlush_TagCnt (a CNT tag + payload) and a RET. There is no stored face tag at
// face+0x20 to match (the first v2 build matched FixMDT-style and substituted 619 of 297,687 nodes). Walk the DMA
// tags instead: every REF is a face; their number must equal the bucket's face-list length.
static void shadowSubstitute(uint8_t *ram, uint32_t bucket)
{
    ++g_shs.nodes;
    const uint32_t chain = ok(bucket, 0x20) ? rd(ram, bucket + 0x10) & 0x1fffffffu : 0u;
    const uint32_t chainQw = ok(bucket, 0x20) ? rd(ram, bucket + 0x14) : 0u;
    bool good = chain != 0u && chainQw > 0u && chainQw < 4096u && ok(chain, size_t(chainQw) * 16);
    std::vector<uint32_t> at;
    if (good)
    {
        uint32_t faces = 0;
        for (uint32_t face = rd(ram, bucket + 4) & 0x1fffffffu; face && faces < 4096; ++faces)
            face = ok(face, 0x20) ? rd(ram, face + 0x10) & 0x1fffffffu : 0u;
        for (uint32_t q = 0; q < chainQw;)
        {
            const uint32_t tag = rd(ram, chain + q * 16);
            const uint32_t id = (tag >> 28) & 7u, qwc = tag & 0xffffu;
            if (id == 3u) { at.push_back(chain + q * 16); ++q; }   // REF: one face packet
            else if (id == 1u) q += 1u + qwc;                         // CNT: inline payload (TEXFLUSH)
            else if (id == 6u) break;                                 // RET: end of the chain
            else { good = false; break; }
        }
        if (at.size() != faces)
            good = false;
    }
    const uint32_t blob = (good && !at.empty()) ? allocQw(ram, g_pending.memory, uint32_t(at.size()) * 2u) : 0u;
    if (!blob)
    {
        ++g_shs.bad;
        return;
    }
    for (size_t k = 0; k < at.size(); ++k)
    {
        const uint32_t m = blob + uint32_t(k) * 32u;
        const uint32_t vif[4] = {0u, 0u, 0u, 0x50000001u}; // NOP NOP NOP DIRECT 1qw
        std::memcpy(ram + m, vif, 16);
        const uint64_t tagLo = 0x8000ull | (1ull << 60); // NLOOP 0, EOP, PACKED, NREG 1
        const uint64_t tagHi = (kG788MarkerMagic << 48) | kG788DropJob;
        std::memcpy(ram + m + 16, &tagLo, 8);
        std::memcpy(ram + m + 24, &tagHi, 8);
        const uint64_t ref = 2ull | (3ull << 28) | (static_cast<uint64_t>(m) << 32);
        std::memcpy(ram + at[k], &ref, 8);
        ++g_shs.subst;
    }
}

static void shadowNative(const uint8_t *ram, uint32_t bucket)
{
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t face = ok(bucket, 0x20) ? rd(ram, bucket + 4) & 0x1fffffffu : 0u;
    if (!face || !ok(face, 0x10))
    {
        ++g_shr.refused;
        return;
    }
    const uint64_t key = (uint64_t(g_pending.self) << 32) | face;
    auto it = g_shSrc.find(key);
    if (it == g_shSrc.end())
    {
        ShadowSrc src;
        const uint32_t type = rh(ram, face) & 7u;
        const int count = int16_t(rh(ram, face + 8));
        const uint32_t idx = rd(ram, face + 0xc) & 0x1fffffffu;
        const uint32_t verts = rd(ram, g_pending.self + 0x30) & 0x1fffffffu;
        if (type == 3u && count >= 3 && ok(idx, size_t(count) * 4) && ok(verts, 16))
        {
            src.tri.reserve(size_t(count));
            for (int i = 0; i < count; ++i)
            {
                const uint32_t va = verts + rd(ram, idx + uint32_t(i) * 4u) * 16u;
                if (!ok(va, 16)) { src.tri.clear(); break; }
                src.tri.push_back(vecAt(ram, va));
            }
        }
        ++g_shr.frozen;
        it = g_shSrc.emplace(key, std::move(src)).first;
        g_shr.freezeNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }
    const std::vector<Vector> &tri = it->second.tri;
    if (tri.size() < 3)
    {
        ++g_shr.refused;
        return;
    }
    const Matrix &m = g_pending.shadowMat;
    std::vector<Vector> &out = g_shr.payload;
    out.resize(tri.size());
    int last = -1;
    for (size_t v = 0; v + 2 < tri.size(); v += 3)
    {
        for (size_t k = 0; k < 3; ++k)
        {
            const Vector &p = tri[v + k];
            Vector r{};
            for (int c = 0; c < 4; ++c)
                r[c] = p[0] * m[0][c] + p[1] * m[1][c] + p[2] * m[2][c] + m[3][c];
            const float iw = r[3] != 0.0f ? 1.0f / r[3] : 0.0f;
            out[v + k] = {r[0] * iw, r[1] * iw, r[2] * iw, iw};
        }
        const float area = (out[v + 1][0] - out[v][0]) * (out[v + 2][1] - out[v][1]) -
                           (out[v + 2][0] - out[v][0]) * (out[v + 1][1] - out[v][1]);
        const int facing = area >= 0.0f ? 1 : 0;
        if (facing != last)
        {
            ++g_shr.runs;
            last = facing;
        }
    }
    ++g_shr.draws;
    g_shr.tris += tri.size() / 3;
    if (g_pending.frame != g_shr.lastFrame)
    {
        g_shr.lastFrame = g_pending.frame;
        ++g_shr.frames;
    }
    g_shr.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0).count());
    if (sf() >= g_shr.lastSf + 600u)
    {
        g_shr.lastSf = sf();
        std::fprintf(stderr, "[G790:shr] sf=%u frames=%llu draws=%llu tris=%llu runs=%llu frozen=%llu refused=%llu "
                             "ns=%llu freezeNs=%llu\n", sf(),
                     (unsigned long long)g_shr.frames, (unsigned long long)g_shr.draws,
                     (unsigned long long)g_shr.tris, (unsigned long long)g_shr.runs,
                     (unsigned long long)g_shr.frozen, (unsigned long long)g_shr.refused,
                     (unsigned long long)g_shr.ns, (unsigned long long)g_shr.freezeNs);
    }
}

static void sendVuProgWrapper(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (getRegU32(ctx, 31) == kRaDestLoop && g_pending.shadowDest && g_pending.frame == sf())
    {
        const uint32_t bucket = getRegU32(ctx, 16) & 0x1fffffffu; // Draw's s0 = the bucket
        if (shadowR())
            shadowNative(ram, bucket);
        if (shadowS())
            shadowSubstitute(ram, bucket);
    }
    prevSendVuProg(ram, ctx, runtime);
}

// ---- 0x135720: ordered-run census of one texture slot (entry: the node arrays are final) --------
static void mgrWrapper(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (censusOn())
    {
        const uint32_t dm = getRegU32(ctx, 4) & 0x1fffffffu;
        int32_t slot = static_cast<int32_t>(getRegU32(ctx, 5));
        bool go = ok(dm, 0x60) && slot >= 0;
        if (go)
        {
            const uint32_t lim = rd(ram, dm + 0x58) & 0x1fffffffu;
            go = ok(lim, 0x10) && slot < static_cast<int32_t>(rd(ram, lim + 0xc));
        }
        if (go && rd(ram, dm))
            slot = static_cast<int32_t>(rd(ram, (rd(ram, dm + 4) & 0x1fffffffu) + uint32_t(slot) * 4));
        if (go && slot >= 0)
        {
            const uint32_t arr = rd(ram, (rd(ram, dm + 0x10) & 0x1fffffffu) + uint32_t(slot) * 4) & 0x1fffffffu;
            const uint32_t count = rd(ram, (rd(ram, dm + 0x18) & 0x1fffffffu) + uint32_t(slot) * 4);
            if (arr && count < 65536u && ok(arr, size_t(count) * 4))
            {
                ++g_c.slots;
                uint64_t run = 0;
                for (int64_t i = int64_t(count) - 1; i >= 0; --i) // emission order (Draw iterates down)
                {
                    const uint32_t node = rd(ram, arr + uint32_t(i) * 4) & 0x1fffffffu;
                    if (!node || !ok(node, 0x10))
                        continue;
                    ++g_c.slotNodes;
                    const bool s = g_substFrame == sf() && g_substChains.count(rd(ram, node + 4) & 0x1fffffffu) != 0;
                    if (s)
                    {
                        ++g_c.slotSubst;
                        if (run++ == 0)
                            ++g_c.runs;
                        ++g_c.runNodes;
                        g_c.maxRun = std::max<uint64_t>(g_c.maxRun, run);
                    }
                    else
                        run = 0;
                }
            }
        }
    }
    prevMgr(ram, ctx, runtime);
}
} // namespace g788

static void g788_register(PS2Runtime &runtime)
{
    if (g_g788Mode == 0)
        return;
    auto hook = [&](uint32_t addr, g788::RecompiledFunction &prev, g788::RecompiledFunction w) {
        prev = runtime.lookupFunction(addr);
        if (!prev) { std::fprintf(stderr, "[G788:probe] FATAL no function at %x\n", addr); std::abort(); }
        runtime.registerFunction(addr, w);
    };
    hook(g788::kFixAssign, g788::prevAssign, &g788::assignWrapper);
    hook(g788::kFacePacket, g788::prevFace, &g788::facePacketWrapper);
    hook(g788::kDrawSeam, g788::prevDraw, &g788::drawWrapper);
    hook(g788::kAddPacket, g788::prevAdd, &g788::addWrapper);
    hook(g788::kMgrDraw, g788::prevMgr, &g788::mgrWrapper);
    if (g788::shadowS() || g788::shadowR())
    {
        hook(g788::kSendVuProg, g788::prevSendVuProg, &g788::sendVuProgWrapper);
        std::fprintf(stderr, "[G790:shs] armed (dest-form seam mgSendVuProg ra 0x13f5a0)\n");
    }
    std::fprintf(stderr, "[G788:probe] armed mode=%d census=%d\n", g_g788Mode, g788::censusOn() ? 1 : 0);
}
#else
static void g788_register(PS2Runtime &) {}
#endif
