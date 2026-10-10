#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
void dc2_g788_econ_memory(uint32_t n);
void dc2_g788_econ_front(uint32_t n,uint32_t sf);

// Diagnostic accounting only. No transport, admission, state or rendering changes.
namespace dc2_g788_econ {
inline bool on() {
    static const bool enabled = std::getenv("DC2_G788_ECON") != nullptr;
    return enabled;
}
enum Stage : unsigned {
    RenderInfo, CreatePacket, AddPacket, PreEndDraw, ReloadTexture,
    Classify, Publish, ParseMarker, BuildJobs, PrepareGpu, SubmitWait, RenderBatch,
    PackJobs, UploadJobs, UploadConstants, DispatchCompute, CheckGpu,
    MaterialBuild, PmodeBuild, TexflushBuild, TopologyConstruction,
    MatrixHelper, DrawenvBuild, SendDma, VuSetup0, VifUnpack,
    GsMaterialParse, GsMaterialExec, GsEnvParse, GsEnvExec,
    GsPmodeParse, GsPmodeExec, GsFlushParse, GsFlushExec, ColdGpuInit, ColdMeshUpload, StageCount
};
enum Event : unsigned {
    TopologyBuild, ShaderCompile, SourceTransform, ReferenceExpansion,
    VifBytes, Path1Bytes, GsPackets, GsRegisters, G144Entries, G144Batches,
    NativeRefsPublished, NativeRefsConsumed, AcceptedLegacyRefs, NativeRefsUnobserved,
    NativeRefsMismatch, ErrorQueries, ErrorQueriesSkipped,
    FixLogicalFaces, FixLogicalDraws, FixOwnerExcluded, FixCensusBad, EventCount
};
inline constexpr const char* names[StageCount] = {
    "renderInfo", "createPacket", "addPacket", "preEndDraw", "reloadTexture",
    "classify", "publish", "parseMarker", "buildJobs", "prepareGpu", "submitWait", "renderBatch",
    "packJobs", "uploadJobs", "uploadConstants", "dispatchCompute", "checkGpu",
    "materialBuild", "pmodeBuild", "texflushBuild", "topologyConstruction",
    "matrixHelper", "drawenvBuild", "sendDma", "vuSetup0", "vifUnpack",
    "gsMaterialParse", "gsMaterialExec", "gsEnvParse", "gsEnvExec",
    "gsPmodeParse", "gsPmodeExec", "gsFlushParse", "gsFlushExec", "coldGpuInit", "coldMeshUpload"
};
struct Stat { std::atomic<uint64_t> chunks{0}, entries{0}, ns{0}, selfNs{0}; };
inline Stat stats[StageCount];
inline std::atomic<uint64_t> events[EventCount]{};
inline std::atomic<uint64_t> gpuNs[2]{}, gpuSamples[2]{}, gpuDropped[2]{};
inline std::atomic<uint64_t> glCpuNs{0};
inline std::atomic<uint64_t> gpuStaticCapacity{0},gpuOutputCapacity{0},gpuDescriptorCapacity{0},cpuJobCapacity{0};
inline uint64_t clockNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Scope;
inline thread_local Scope* current = nullptr;
// Every generated invocation constructs its scope BEFORE resume-PC dispatch.
// Every preemption/early return closes it; resumed chunks open a fresh scope.
// No timer assumes that an entry invocation reaches the guest function's return.
struct Scope {
    unsigned stage;
    bool enabled;
    uint64_t begin = 0, child = 0;
    Scope* parent = nullptr;
    explicit Scope(unsigned s, bool entry = true, bool eligible = true)
        : stage(s), enabled(eligible && on()) {
        if (!enabled) return;
        stats[s].chunks.fetch_add(1, std::memory_order_relaxed);
        if (entry) stats[s].entries.fetch_add(1, std::memory_order_relaxed);
        parent = current; current = this; begin = clockNs();
    }
    ~Scope() {
        if (!enabled) return;
        const uint64_t elapsed = clockNs() - begin;
        stats[stage].ns.fetch_add(elapsed, std::memory_order_relaxed);
        stats[stage].selfNs.fetch_add(elapsed > child ? elapsed-child : 0, std::memory_order_relaxed);
        current = parent;
        if (parent) parent->child += elapsed;
    }
};
inline void event(unsigned kind, uint64_t count = 1) {
    if (on()) events[kind].fetch_add(count, std::memory_order_relaxed);
}
inline void report(uint32_t n, uint32_t sf) {
    if (!on() || n % 60 != 1) return;
    for (unsigned i=0; i<StageCount; ++i) {
        const auto& s=stats[i];
        std::fprintf(stderr, "[G788:econ] n=%u sf=%u stage=%s chunks=%llu entries=%llu ns=%llu selfNs=%llu\n",
            n,sf,names[i],(unsigned long long)s.chunks.load(),(unsigned long long)s.entries.load(),
            (unsigned long long)s.ns.load(),(unsigned long long)s.selfNs.load());
    }
    std::fprintf(stderr,"[G788:warm] n=%u sf=%u topologyBuild=%llu shaderCompile=%llu sourceTransform=%llu referenceExpansion=%llu\n",
        n,sf,(unsigned long long)events[TopologyBuild].load(),(unsigned long long)events[ShaderCompile].load(),
        (unsigned long long)events[SourceTransform].load(),(unsigned long long)events[ReferenceExpansion].load());
    std::fprintf(stderr,"[G788:wire] n=%u sf=%u vifBytes=%llu path1Bytes=%llu gsPackets=%llu gsRegisters=%llu g144Entries=%llu g144Batches=%llu nativeRefsPublished=%llu nativeRefsConsumed=%llu acceptedLegacyRefs=%llu unobserved=%llu mismatch=%llu errorQueries=%llu errorQueriesSkipped=%llu\n",
        n,sf,(unsigned long long)events[VifBytes].load(),(unsigned long long)events[Path1Bytes].load(),
        (unsigned long long)events[GsPackets].load(),(unsigned long long)events[GsRegisters].load(),
        (unsigned long long)events[G144Entries].load(),(unsigned long long)events[G144Batches].load(),
        (unsigned long long)events[NativeRefsPublished].load(),(unsigned long long)events[NativeRefsConsumed].load(),
        (unsigned long long)events[AcceptedLegacyRefs].load(),(unsigned long long)events[NativeRefsUnobserved].load(),
        (unsigned long long)events[NativeRefsMismatch].load(),(unsigned long long)events[ErrorQueries].load(),
        (unsigned long long)events[ErrorQueriesSkipped].load());
    for(unsigned i=0;i<2;++i)
        std::fprintf(stderr,"[G788:gpu-cost] n=%u sf=%u stage=%s samples=%llu ns=%llu dropped=%llu\n",
            n,sf,i?"rasterSpan":"computeSpan",(unsigned long long)gpuSamples[i].load(),
            (unsigned long long)gpuNs[i].load(),(unsigned long long)gpuDropped[i].load());
    std::fprintf(stderr,"[G788:face-scale] n=%u sf=%u logicalFaces=%llu logicalDraws=%llu ownerExcluded=%llu invalid=%llu\n",
        n,sf,(unsigned long long)events[FixLogicalFaces].load(),(unsigned long long)events[FixLogicalDraws].load(),
        (unsigned long long)events[FixOwnerExcluded].load(),(unsigned long long)events[FixCensusBad].load());
    std::fprintf(stderr,"[G788:worker-cpu] n=%u sf=%u cpuNs=%llu\n",n,sf,
        (unsigned long long)glCpuNs.load());
    ::dc2_g788_econ_front(n,sf);
    if(n%600==1)::dc2_g788_econ_memory(n);
}
}

// Existing accepted native buckets are excluded even from AddPacket instrumentation.
bool dc2_g788_econ_add_allowed(uint32_t bucket);
void dc2_g788_econ_watch_ref(uint32_t frame,uint32_t address,uint64_t original,uint64_t committed,uint32_t seq);
void dc2_g788_econ_observe_ref(uint32_t address,uint64_t tag);
