#pragma once
#if defined(PS2X_G777_NATIVE_GL)
#include "dc2_g778_flags.h"
#include "dc2_g775_capture.h"
#include "dc2_native_draw.h"
#include "dc2_g778_contract.h"
#include "dc2_g782_commit.h"
#include "dc2_g784_buckets.h"
#include <chrono>
#include <condition_variable>
#include <functional>
class GS;
namespace dc2_g778 {
struct G780NativeItem;
struct Command {
    std::shared_ptr<const dc2_native::DC2NativeDraw> draw;
    dc2_g775_capture::Draw association{};
    uint32_t bucket=0xe91f00,clip=0;
    // G784: the bucket's contract row, MTVU group size and decision core (dc2_g784_buckets.h).
    uint32_t bucketIndex=dc2_g784::kE91F00,kicks=13,deadline=5;
    uint32_t vifKicks=0; // EE only: MSCAL/MSCNT seen inside the active VifScope
    // EE writes before the G150 apply is enqueued: the single inter-face PRMODE value, if any.
    bool interPrmode=false;uint64_t prmode=0;
    std::atomic<uint32_t> coreStarts{0},coreResumes{0},shadowBatches{0},shadowFailures{0};
    std::atomic<uint64_t> corePairs{0},xgkicks{0},gifBytes{0};
    std::atomic<bool> finished{false};
    std::atomic<bool> nativeCommitted{false};
    // G782: retained original VU jobs share one irrevocable draw decision.
    std::shared_ptr<dc2_g782::CommitToken> g782Commit;
    bool g782Group=false; // EE publishes before enqueueing either G150 or MTVU work
    std::atomic<uint32_t> g782CoreOrdinal{0};
    uint32_t transportEnd=0,transportBytes=0;
    std::array<uint32_t,7> vifBefore{}; // DBF, BASE, OFST, TOPS, ITOP, CYCLE, MODE
    bool clipKnown=false;
    std::atomic<uint32_t> avoidedStarts{0},avoidedResumes{0};
    std::shared_ptr<dc2_g783::Timeline> g783Timeline; // DC2_G783_TIMELINE only
    uint32_t g783Collected=0; // G150 only: this group's MTVU items collected so far, in kick order
    // G785 NOKICK ablation (DC2_G785_E91F00_NOKICK, timing probe only): set by the G150 apply before it
    // rejects the token; the collect then rewrites the legacy items' drawing kicks (XYZF2/XYZ2 ->
    // XYZF3/XYZ3) so the group is parsed in full but emits no primitive.
    std::atomic<bool> g785NoKick{false};
};
inline std::mutex commandsMutex;
inline std::unordered_map<uint64_t,std::shared_ptr<Command>> commands;
inline GS* liveGs=nullptr;
inline thread_local std::shared_ptr<Command> coreCommand;
inline thread_local std::shared_ptr<Command> shadowCommand;
inline thread_local std::shared_ptr<Command> nativeCommand;
// Executor-local adapter for the existing G178 batch builder. Populated only from
// the immutable G780 item, never from the parse-side Command after enqueue.
inline thread_local std::shared_ptr<const dc2_native::DC2NativeDraw> g780NativeDraw;
inline thread_local uint32_t g780NativeBucket=0;
// G784 native RUN: consecutive committed native items flushed as ONE batch (one GL-worker round
// trip). Executor-local; one entry per g_g144List envelope, in order. Empty outside a run flush.
struct G784RunEntry { std::shared_ptr<const dc2_native::DC2NativeDraw> draw; uint32_t bucket=0; };
inline thread_local std::vector<G784RunEntry> g784RunEntries;
inline bool g784CoalesceOn(){static const bool on=!std::getenv("DC2_G784_NO_COALESCE") && !statsRequested();return on;}
#if defined(PS2X_G684_HOT_DIAG)
// Parse-only marker for the rejected G779 future-FBP=139 scenario.
inline std::atomic<uint64_t> g780Future139Writes{0};
#endif
inline thread_local std::shared_ptr<Command> gsCommand;
struct Counters {
    std::atomic<uint64_t> captures{0},orderedBegins{0},orderedEnds{0},associationRejected{0};
    std::atomic<uint64_t> shadowBatches{0},shadowPixels{0},shadowDifferent{0},shadowErrors{0};
    std::atomic<uint64_t> sourceUploadBytes{0},cacheHits{0},cacheMisses{0},gpuDraws{0};
    std::atomic<uint64_t> cacheReplacements{0},nativePrimitives{0},liveColourPixels{0},liveDepthPixels{0};
    std::atomic<uint64_t> nativeCommands{0},fallbackCommands{0},unpackBytes{0},avoidedStarts{0},avoidedResumes{0};
    std::atomic<uint64_t> gsStateFailures{0},transportFailures{0};
    std::atomic<uint64_t> legacyPairs{0},legacyKicks{0},legacyGifBytes{0},legacyPrimitives{0},legacyStarts{0},legacyResumes{0};
    std::atomic<uint64_t> g782DeadlineLegacy{0},g782ParseReject{0},g782LostRace{0};
    std::atomic<uint64_t> g782NativeCommit{0},g782NativeComplete{0},g782LegacyGroups{0},g782BypassGroups{0};
    // G782 G150 pre-commit rejections by contract clause (plans/phase-G782-ledger.md).
    std::atomic<uint64_t> g782Eligible{0},g782RejCapture{0},g782RejProcess{0},g782RejClassify{0};
    std::atomic<uint64_t> g782RejDepthAlias{0},g782RejTexAlias{0},g782RejAuthority{0},g782RejConstants{0};
    // DC2_G782_REPORT only: the whole native-specific cost on the producer threads.
    std::atomic<uint64_t> g782EeNs{0},g782EeCalls{0},g782EeMaxNs{0};
    std::atomic<uint64_t> g782MtvuNs{0},g782MtvuCalls{0},g782MtvuMaxNs{0};
    // DC2_G782_REPORT only: EE cost of the native infrastructure that runs on EVERY arm
    // where native is requested, whether or not a draw is ever committed.
    std::atomic<uint64_t> g782CaptureNs{0},g782CaptureCalls{0},g782CaptureMaxNs{0},g782MeshBuilds{0};
    std::atomic<uint64_t> g782HookNs{0},g782HookCalls{0},g782HookMaxNs{0};
    // G775/G776 provenance probes: self time only (the wrapped game function excluded).
    std::atomic<uint64_t> g782ProbeNs{0},g782ProbeCalls{0},g782ProbeMaxNs{0};
    // G783: NativeCommitted groups whose prefix cores ran before the deadline; their Path1 output.
    std::atomic<uint64_t> g783PrefixItems{0},g783PrefixPackets{0};
};
inline void g782NoteCost(std::atomic<uint64_t>& sum,std::atomic<uint64_t>& calls,
                         std::atomic<uint64_t>& peak,uint64_t ns){
    sum.fetch_add(ns,std::memory_order_relaxed);calls.fetch_add(1u,std::memory_order_relaxed);
    uint64_t seen=peak.load(std::memory_order_relaxed);
    while(ns>seen && !peak.compare_exchange_weak(seen,ns,std::memory_order_relaxed)){}
}
// Self-time clock for a provenance probe: run the wrapped game function through call().
struct G782ProbeClock {
    const bool on;
    std::chrono::steady_clock::time_point t0;
    uint64_t innerNs=0;
    explicit G782ProbeClock(bool enabled);
    template<class F> void call(F&& f){
        if(!on){f();return;}
        const auto a=std::chrono::steady_clock::now();f();
        innerNs+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-a).count());
    }
    ~G782ProbeClock();
};
// Scoped EE timer for the infrastructure probes; inert unless DC2_G782_REPORT is set.
struct G782CostScope {
    std::atomic<uint64_t> &sum,&calls,&peak;
    const bool on;
    std::chrono::steady_clock::time_point t0;
    G782CostScope(std::atomic<uint64_t>& s,std::atomic<uint64_t>& c,std::atomic<uint64_t>& p,bool enabled)
        :sum(s),calls(c),peak(p),on(enabled){if(on)t0=std::chrono::steady_clock::now();}
    ~G782CostScope(){
        if(on)g782NoteCost(sum,calls,peak,uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-t0).count()));
    }
};
inline Counters counters;
inline std::atomic<uint32_t> gsDirty{0};
// G788: a native world face leaves the latches dirty with the owner bit 0x80000000; a consumption is
// COUNTED (g788LatchConsumed, a correctness gate that must stay 0), never fatal, and the owner bit is
// dropped once every latch bit has been overwritten.
inline std::atomic<uint64_t> g788LatchConsumed{0};
inline void gsOverwrite(uint32_t mask){
    if(gsDirty.load(std::memory_order_relaxed)){
        const uint32_t r=gsDirty.fetch_and(~mask,std::memory_order_relaxed)&~mask;
        if((r&31u)==0u && r)gsDirty.store(0u,std::memory_order_relaxed);
    }
}
inline void gsConsume(bool textured,bool fst){
    const uint32_t needed=1u|8u|16u|(textured&&!fst?6u:0u);
    const uint32_t g788d=gsDirty.load(std::memory_order_relaxed);
    if((g788d&needed) && (g788d&0x80000000u)){
        g788LatchConsumed.fetch_add(1u,std::memory_order_relaxed);
        gsDirty.store(0u,std::memory_order_relaxed);
        return;
    }
    if(gsDirty.load(std::memory_order_relaxed)&needed){
        ++counters.gsStateFailures;
        std::fprintf(stderr,"[G778:gs-state] omitted latch consumed mask=%x\n",gsDirty.load());std::abort();
    }
}
// G784: one Command per (invocation, enabled bucket).
inline uint64_t commandKey(uint64_t id,uint32_t bucketIndex){return (id<<3)|(bucketIndex&7u);}
inline std::shared_ptr<Command> find(const dc2_g775_capture::Draw& provenance){
    dc2_g783::Scope g783(dc2_g783::MtvuFind);
    if(!requested() || provenance.self!=dc2_g784::kMeshSelf || !dc2_g784::enabled(provenance.bucket) ||
       !provenance.id || !provenance.geometry || !provenance.state || !provenance.call)return {};
    const int index=dc2_g784::indexOf(provenance.bucket);
    std::lock_guard<std::mutex> lock(commandsMutex);
    const auto it=commands.find(commandKey(provenance.id,uint32_t(index)));if(it==commands.end())return {};
    const auto& command=it->second;
    if(command->finished.load(std::memory_order_acquire))return {};
    if(command->association.id && (command->association.state!=provenance.state ||
       command->association.geometry!=provenance.geometry || command->association.call!=provenance.call ||
       command->association.listNode!=provenance.listNode)){++counters.associationRejected;return {};}
    if(!command->association.id)command->association=provenance;return command;
}
void orderedShadow(GS* gs,std::shared_ptr<Command> command,bool begin);
bool orderedNative(GS* gs,std::shared_ptr<Command> command);
bool nonblockingAvailable();
bool orderedNativeNonblocking(GS* gs,std::shared_ptr<Command> command);
// G782: G150-owned superset of every VRAM page the GPU executor can come to own.
// Fed from GS::writeRegister on the parse thread only (FRAME/ZBUF/L2L destination).
void g782NoteTargetRegs(uint32_t fbp,uint32_t fbw,uint32_t fpsm,uint64_t zbuf);
void g782NoteLocalCopy(uint32_t dbp,uint32_t dbw,uint32_t dpsm,uint32_t dsax,uint32_t dsay,
                       uint32_t rrw,uint32_t rrh);
// Last executor refusal detail for a committed native item (diagnostic only).
inline std::atomic<uint32_t> g782LastPrepareError{0};
inline bool g782ReportRequested(){static const bool on=std::getenv("DC2_G782_REPORT")!=nullptr;return on;}
struct VifScope;
inline thread_local VifScope* vifScope=nullptr;
struct VifScope {
    VifScope* previous=vifScope;
    std::shared_ptr<Command> active;
    std::function<void(std::shared_ptr<Command>,bool)> submit;
    explicit VifScope(std::function<void(std::shared_ptr<Command>,bool)> callback):submit(std::move(callback)){vifScope=this;}
    void close(){
        if(!active)return;
        // G782: the group's decision lives in its CommitToken. The G780 end bracket would only
        // add a GIF drain, a forced G150 window, a G713 batch split and a G713 call per draw.
        if(active->g782Group && !g782EndBracket())active->finished.store(true,std::memory_order_release);
        else submit(active,false);
        active.reset();
    }
    ~VifScope(){close();vifScope=previous;}
    void command(uint32_t value,const dc2_g775_capture::Draw& provenance){
        if(active && (provenance.id!=active->association.id || provenance.bucket!=active->bucket ||
           provenance.geometry!=active->association.geometry || provenance.call!=active->association.call))close();
        if(!active && value==0x6c010027u && (shadowSelected(provenance.sf) || statsRequested())){
            active=find(provenance);if(active)submit(active,true);
        }
    }
};
inline void observeCore(bool resume){
    auto command=coreCommand?coreCommand:find(dc2_g775_capture::vifDraw);
    if(command){
        if(resume)++command->coreResumes;else ++command->coreStarts;
        // G784: GS-only groups (deadline == kicks) are classified by G150 at collect time.
        if(command->g782Group && command->deadline<command->kicks &&
           command->g782CoreOrdinal.load(std::memory_order_relaxed)==command->kicks){
            const uint32_t faces=dc2_g784::kBuckets[command->bucketIndex].faces;
            if(command->coreStarts.load(std::memory_order_relaxed)!=faces ||
               command->coreResumes.load(std::memory_order_relaxed)!=command->kicks-faces ||
               command->avoidedStarts.load(std::memory_order_relaxed)!=0u ||
               command->avoidedResumes.load(std::memory_order_relaxed)!=0u){
                ++counters.transportFailures;std::abort();
            }
            ++counters.g782LegacyGroups;
        }
    }
}
template<class State>
inline bool bypassCore(State& state,bool resume,uint32_t entry,uint32_t itop){
    const auto command=coreCommand?coreCommand:find(dc2_g775_capture::vifDraw);
    if(!command)return false;
    if(command->g782Group){
        const uint32_t ordinal=command->g782CoreOrdinal.fetch_add(1u,std::memory_order_relaxed);
        // G784: a group is `kicks` jobs; MSCAL (entry 0x10) opens each transport face, MSCNT
        // continues it. Core 0 is always an MSCAL.
        if(ordinal>=command->kicks || (ordinal==0u && (resume || entry!=0x10u)) || (!resume && entry!=0x10u)){
            ++counters.transportFailures;std::abort();
        }
        // G783: cores before the deadline run the original program unconditionally, without
        // touching the token (partial-omission replay proof, phase-G783 ledger). The irreversible
        // legacy decision moves from core 0 to core `deadline`. G784: per-bucket deadline;
        // deadline == kicks is GS-only and never reaches here.
        const uint32_t deadlineCore=command->deadline;
        if(ordinal<deadlineCore)return false;
        bool deadline=false;
        // The MTVU's whole native-specific decision: one acquire load, at most one CAS.
        if(ordinal==deadlineCore && dc2_g784::vuCeiling() && command->bucketIndex!=dc2_g784::kE91F00)
            command->g782Commit->tryNative(); // ceiling probe only: no native node, nothing drawn
        const bool g782Timed=ordinal==deadlineCore && g782ReportRequested();
        const auto g782T0=g782Timed?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        const auto decision=ordinal==deadlineCore?command->g782Commit->firstCore(&deadline):command->g782Commit->laterCore();
        if(g782Timed)g782NoteCost(counters.g782MtvuNs,counters.g782MtvuCalls,counters.g782MtvuMaxNs,
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-g782T0).count()));
        if(deadline)++counters.g782DeadlineLegacy;
        if(decision==dc2_g782::Decision::LegacyCommitted)return false;
    }else if(!command->nativeCommitted.load(std::memory_order_acquire))return false;
    // This is an actual interpreter core entry, associated through the queued DMA
    // item. Geometry never executes. TOP/DBF remain in the original VIF handlers.
    if(!resume && entry!=0x10u){++counters.transportFailures;std::abort();}
    if(resume){++command->avoidedResumes;++counters.avoidedResumes;}
    else{++command->avoidedStarts;++counters.avoidedStarts;}
    state.pc=0x320;state.ebit=true;state.i=2048.0f;state.itop=itop;
    // Publish recovered final history at the final core boundary only.
    const bool finalCore=command->g782Group?command->g782CoreOrdinal.load(std::memory_order_relaxed)==command->kicks
                                           :command->avoidedResumes.load()==12;
    if(finalCore)state.clip=command->clip;
    if(command->g782Group && finalCore){
        // Prefix (ordinals < k) ran the original program: 1 start + k-1 resumes when k > 0.
        // Every core ran (prefix) or was avoided (suffix): starts + resumes add up to the group.
        const uint32_t k=command->deadline;
        if(command->coreStarts.load(std::memory_order_relaxed)+command->coreResumes.load(std::memory_order_relaxed)!=k ||
           command->avoidedStarts.load(std::memory_order_relaxed)+command->avoidedResumes.load(std::memory_order_relaxed)!=command->kicks-k ||
           (k==0u && command->avoidedStarts.load(std::memory_order_relaxed)==0u) ||
           (k>0u && command->coreStarts.load(std::memory_order_relaxed)==0u)){
            ++counters.transportFailures;std::abort();
        }
        ++counters.g782BypassGroups;
        ++dc2_g784::counters[command->bucketIndex].bypassGroups;
        dc2_g784::counters[command->bucketIndex].avoidedCores.fetch_add(command->kicks-k,std::memory_order_relaxed);
    }
    return true;
}
inline G782ProbeClock::G782ProbeClock(bool enabled):on(enabled){if(on)t0=std::chrono::steady_clock::now();}
inline G782ProbeClock::~G782ProbeClock(){
    if(!on)return;
    const uint64_t total=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count());
    g782NoteCost(counters.g782ProbeNs,counters.g782ProbeCalls,counters.g782ProbeMaxNs,total>innerNs?total-innerNs:0u);
}
inline void reportG782(uint32_t frame){
    std::fprintf(stderr,"[G782:commit] sf=%u native=%llu complete=%llu bypass13=%llu legacy13=%llu deadline=%llu reject=%llu lostRace=%llu\n",frame,
        (unsigned long long)counters.g782NativeCommit.load(),(unsigned long long)counters.g782NativeComplete.load(),
        (unsigned long long)counters.g782BypassGroups.load(),(unsigned long long)counters.g782LegacyGroups.load(),
        (unsigned long long)counters.g782DeadlineLegacy.load(),(unsigned long long)counters.g782ParseReject.load(),
        (unsigned long long)counters.g782LostRace.load());
    std::fprintf(stderr,"[G783:deadline] sf=%u core=%u prefixItems=%llu prefixPackets=%llu\n",frame,dc2_g783::deadlineCore(),
        (unsigned long long)counters.g783PrefixItems.load(),(unsigned long long)counters.g783PrefixPackets.load());
    std::fprintf(stderr,"[G782:contract] sf=%u eligible=%llu capture=%llu process=%llu classify=%llu depthAlias=%llu texAlias=%llu authority=%llu constants=%llu\n",frame,
        (unsigned long long)counters.g782Eligible.load(),(unsigned long long)counters.g782RejCapture.load(),
        (unsigned long long)counters.g782RejProcess.load(),(unsigned long long)counters.g782RejClassify.load(),
        (unsigned long long)counters.g782RejDepthAlias.load(),(unsigned long long)counters.g782RejTexAlias.load(),
        (unsigned long long)counters.g782RejAuthority.load(),(unsigned long long)counters.g782RejConstants.load());
    {
    const auto ee=counters.g782EeCalls.load(),mtvu=counters.g782MtvuCalls.load();
    std::fprintf(stderr,"[G782:cost] sf=%u eeCalls=%llu eeAvgUs=%.3f eeMaxUs=%.3f mtvuDecisions=%llu mtvuAvgUs=%.4f mtvuMaxUs=%.3f\n",frame,
        (unsigned long long)ee,ee?double(counters.g782EeNs.load())/1e3/ee:0.,double(counters.g782EeMaxNs.load())/1e3,
        (unsigned long long)mtvu,mtvu?double(counters.g782MtvuNs.load())/1e3/mtvu:0.,double(counters.g782MtvuMaxNs.load())/1e3);
    const auto cap=counters.g782CaptureCalls.load(),hook=counters.g782HookCalls.load();
    std::fprintf(stderr,"[G782:infra] sf=%u captureCalls=%llu captureTotalMs=%.3f captureAvgUs=%.3f captureMaxUs=%.3f meshBuilds=%llu hookCalls=%llu hookTotalMs=%.3f hookAvgUs=%.3f hookMaxUs=%.3f\n",frame,
        (unsigned long long)cap,double(counters.g782CaptureNs.load())/1e6,cap?double(counters.g782CaptureNs.load())/1e3/cap:0.,
        double(counters.g782CaptureMaxNs.load())/1e3,(unsigned long long)counters.g782MeshBuilds.load(),
        (unsigned long long)hook,double(counters.g782HookNs.load())/1e6,hook?double(counters.g782HookNs.load())/1e3/hook:0.,
        double(counters.g782HookMaxNs.load())/1e3);
    const auto probes=counters.g782ProbeCalls.load();
    std::fprintf(stderr,"[G782:provenance] sf=%u probeCalls=%llu probeSelfTotalMs=%.3f probeAvgUs=%.3f probeMaxUs=%.3f\n",frame,
        (unsigned long long)probes,double(counters.g782ProbeNs.load())/1e6,
        probes?double(counters.g782ProbeNs.load())/1e3/probes:0.,double(counters.g782ProbeMaxNs.load())/1e3);
    }
}
inline void report(uint32_t frame){
    if(!statsRequested()){
        if(g782ReportRequested() && frame%600==0){reportG782(frame);dc2_g784::report(frame);}
        return;
    }
    if(frame%60)return;
    std::fprintf(stderr,"[G778:shadow] sf=%u captures=%llu begin=%llu end=%llu rejected=%llu batches=%llu pixels=%llu different=%llu errors=%llu gpuDraws=%llu upload=%llu hits=%llu misses=%llu\n",frame,
        (unsigned long long)counters.captures.load(),(unsigned long long)counters.orderedBegins.load(),
        (unsigned long long)counters.orderedEnds.load(),(unsigned long long)counters.associationRejected.load(),
        (unsigned long long)counters.shadowBatches.load(),(unsigned long long)counters.shadowPixels.load(),
        (unsigned long long)counters.shadowDifferent.load(),(unsigned long long)counters.shadowErrors.load(),
        (unsigned long long)counters.gpuDraws.load(),(unsigned long long)counters.sourceUploadBytes.load(),
        (unsigned long long)counters.cacheHits.load(),(unsigned long long)counters.cacheMisses.load());
    std::fprintf(stderr,"[G778:native] sf=%u commands=%llu fallback=%llu unpackBytes=%llu startsAvoided=%llu resumesAvoided=%llu transportFailures=%llu gsStateFailures=%llu\n",frame,
        (unsigned long long)counters.nativeCommands.load(),(unsigned long long)counters.fallbackCommands.load(),
        (unsigned long long)counters.unpackBytes.load(),(unsigned long long)counters.avoidedStarts.load(),
        (unsigned long long)counters.avoidedResumes.load(),(unsigned long long)counters.transportFailures.load(),
        (unsigned long long)counters.gsStateFailures.load());
    std::fprintf(stderr,"[G778:legacy-coverage] sf=%u pairs=%llu xgkicks=%llu gifBytes=%llu primitives=%llu starts=%llu resumes=%llu\n",frame,
        (unsigned long long)counters.legacyPairs.load(),(unsigned long long)counters.legacyKicks.load(),
        (unsigned long long)counters.legacyGifBytes.load(),(unsigned long long)counters.legacyPrimitives.load(),
        (unsigned long long)counters.legacyStarts.load(),(unsigned long long)counters.legacyResumes.load());
    std::fprintf(stderr,"[G778:gpu-coverage] sf=%u generatedPrimitives=%llu replacements=%llu liveColour=%llu liveDepth=%llu\n",frame,
        (unsigned long long)counters.nativePrimitives.load(),(unsigned long long)counters.cacheReplacements.load(),
        (unsigned long long)counters.liveColourPixels.load(),(unsigned long long)counters.liveDepthPixels.load());
    reportG782(frame);
    dc2_g784::report(frame);
}
}
#endif
