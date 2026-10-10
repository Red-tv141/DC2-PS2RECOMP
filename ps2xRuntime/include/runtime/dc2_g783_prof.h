#pragma once
// G783: provenance self-time profiler and ablation knobs (plans/phase-G783-ledger.md).
// DC2_G783_PROF=1 arms the rdtsc clocks (diagnostic arms only; never a timing arm).
// DC2_G783_OFF=<comma list> removes one provenance piece at runtime (timing ablation):
//   reg       register no G775/G776 game-function probes at all
//   pktwalk   mesh probe: skip addPacketRanges (census-only work: addRange is inert)
//   meshmaps  mesh probe: skip recordInstance / rememberDirectChain / rememberStateGeometry
//   chain     DMA: no Chain append / tag / publish
//   vif       VIF: no InputScope / per-command origin lookup
//   gsnote    G150: no g782NoteTargetRegs / g782NoteLocalCopy
//   hook      VIF: no E91F00 native hook
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace dc2_g783 {
enum Probe : uint32_t {
    MeshSelf, MeshCapture, MeshInstance, MeshDirectChain, MeshPacketWalk, MeshStateGeom,
    RenderInfo, MdtLoad, FixLoad, AddPacket, SendProg, CreateFace, FacePacket,
    ChainAppend, ChainTag, ChainPublish, InputScope, VifCommand, VifHook, MtvuFind, GsNote,
    MutexWait, MutexHold, ProbeCount
};
inline const char* probeName(uint32_t p){
    static const char* names[ProbeCount]={"meshSelf","meshCapture","meshInstance","meshDirectChain",
        "meshPacketWalk","meshStateGeom","renderInfo","mdtLoad","fixLoad","addPacket","sendProg",
        "createFace","facePacket","chainAppend","chainTag","chainPublish","inputScope","vifCommand",
        "vifHook","mtvuFind","gsNote","mutexWait","mutexHold"};
    return p<ProbeCount?names[p]:"?";
}
inline bool profOn(){static const bool on=std::getenv("DC2_G783_PROF")!=nullptr;return on;}
enum Off : uint32_t { OffReg=1u, OffPktWalk=2u, OffMeshMaps=4u, OffChain=8u, OffVif=16u, OffGsNote=32u, OffHook=64u };
inline uint32_t offMask(){
    static const uint32_t mask=[](){
        const char* v=std::getenv("DC2_G783_OFF");if(!v)return 0u;
        static const struct {const char* n;uint32_t b;} keys[]={{"reg",OffReg},{"pktwalk",OffPktWalk},
            {"meshmaps",OffMeshMaps},{"chain",OffChain},{"vif",OffVif},{"gsnote",OffGsNote},{"hook",OffHook}};
        uint32_t m=0;
        for(const auto& k:keys){const char* p=std::strstr(v,k.n);if(p)m|=k.b;}
        std::fprintf(stderr,"[G783:off] mask=%x spec=%s\n",m,v);
        return m;
    }();
    return mask;
}
inline bool off(uint32_t bit){return (offMask()&bit)!=0u;}
// G783 Direction C: the MTVU legacy deadline is target core `deadlineCore()` (0 = G782: core 0).
// Cores before it always run the original program; if native won by then, cores from it on are
// bypassed and G150 drops the prefix cores' Path1 output. Proof: plans/phase-G783-ledger.md
// (partial-omission replay). DC2_G783_DEADLINE_CORE=0 is the G782 rollback.
inline uint32_t deadlineCore(){
    static const uint32_t k=[](){const char* v=std::getenv("DC2_G783_DEADLINE_CORE");
        const unsigned long n=v?std::strtoul(v,nullptr,0):5ul;return uint32_t(n>12ul?12ul:n);}();
    return k;
}
inline uint64_t ticks(){
#if defined(_MSC_VER)
    return __rdtsc();
#else
    return uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}
struct Stat { uint64_t calls=0,cycles=0,max=0; uint32_t hist[48]{}; };
struct ThreadBlock { Stat s[ProbeCount]; ThreadBlock* next=nullptr; };
inline std::atomic<ThreadBlock*> blocks{nullptr};
inline ThreadBlock& block(){
    thread_local ThreadBlock* mine=[](){
        auto* b=new ThreadBlock();
        ThreadBlock* head=blocks.load(std::memory_order_relaxed);
        do b->next=head; while(!blocks.compare_exchange_weak(head,b,std::memory_order_release,std::memory_order_relaxed));
        return b;
    }();
    return *mine;
}
inline void note(uint32_t p,uint64_t c){
    auto& s=block().s[p];
    ++s.calls;s.cycles+=c;if(c>s.max)s.max=c;
    uint32_t bucket=0;uint64_t v=c;while(v>1u && bucket<47u){v>>=1u;++bucket;}
    ++s.hist[bucket];
}
// Plain scope: everything inside is this probe's self time.
struct Scope {
    const uint32_t p;const bool on;uint64_t t0=0;
    explicit Scope(uint32_t probe):p(probe),on(profOn()){if(on)t0=ticks();}
    ~Scope(){if(on)note(p,ticks()-t0);}
};
// Wrapper scope: the wrapped game function runs through call() and is excluded.
struct SelfScope {
    const uint32_t p;const bool on;uint64_t t0=0,inner=0;
    explicit SelfScope(uint32_t probe):p(probe),on(profOn()){if(on)t0=ticks();}
    template<class F> void call(F&& f){
        if(!on){f();return;}
        const uint64_t a=ticks();f();inner+=ticks()-a;
    }
    ~SelfScope(){if(on){const uint64_t total=ticks()-t0;note(p,total>inner?total-inner:0u);}}
};
// The shared provenance mutex, with contention / wait / hold accounting when profiled.
struct ProfMutex {
    std::mutex m;
    uint64_t acquired=0;
    void lock(){
        if(!profOn()){m.lock();return;}
        if(m.try_lock()){acquired=ticks();note(MutexWait,0);return;}
        const uint64_t a=ticks();m.lock();acquired=ticks();note(MutexWait,acquired-a);
        contended.fetch_add(1u,std::memory_order_relaxed);
    }
    bool try_lock(){if(!m.try_lock())return false;if(profOn())acquired=ticks();return true;}
    void unlock(){if(profOn())note(MutexHold,ticks()-acquired);m.unlock();}
    std::atomic<uint64_t> contended{0};
};
inline double ticksPerNs(){
    static const std::pair<uint64_t,std::chrono::steady_clock::time_point> origin{ticks(),std::chrono::steady_clock::now()};
    const uint64_t t=ticks();const auto w=std::chrono::steady_clock::now();
    const double ns=double(std::chrono::duration_cast<std::chrono::nanoseconds>(w-origin.second).count());
    return ns>1e6?double(t-origin.first)/ns:3.0;
}
// ===== Objective B: admission timeline (DC2_G783_TIMELINE=1, diagnostic arms only) =====
inline bool timelineOn(){static const bool on=std::getenv("DC2_G783_TIMELINE")!=nullptr;return on;}
inline std::atomic<uint64_t> kicksEnq{0},kicksDone{0};   // MTVU items, EE enqueue / worker done
inline thread_local uint64_t mtvuLastEnd=0;               // MTVU worker: end tick of its last item
// G150 GS register provenance: the window/path of each register's last parse-side write.
inline thread_local uint32_t gsPath=0;                    // set per dispatched GIF packet
inline std::atomic<uint64_t> gsWindow{0};                 // windows processed by G150
struct RegWrite { uint64_t window=0; uint32_t path=0; };
inline RegWrite regWrites[128];                           // G150 thread only
inline std::atomic<uint64_t> relevantWrites[4]{};         // contract registers, by path
// Registers the E91F00 contract / G780 item reads (both contexts) plus the page-set inputs.
inline bool contractRegister(uint32_t r){
    switch(r){
    case 0x06:case 0x07:case 0x08:case 0x09:case 0x14:case 0x15:case 0x16:case 0x17:case 0x18:case 0x19:
    case 0x1a:case 0x1b:case 0x1c:case 0x3b:case 0x3d:case 0x40:case 0x41:case 0x42:case 0x43:case 0x44:
    case 0x45:case 0x46:case 0x47:case 0x48:case 0x49:case 0x4a:case 0x4b:case 0x4c:case 0x4d:case 0x4e:
    case 0x4f:case 0x53:return true;
    default:return false;
    }
}
inline void noteRegWrite(uint32_t r){
    if(r>=128u)return;
    regWrites[r]={gsWindow.load(std::memory_order_relaxed),gsPath};
    if(contractRegister(r))relevantWrites[gsPath&3u].fetch_add(1u,std::memory_order_relaxed);
}
struct Timeline {
    uint64_t capture=0,hook=0,apply=0,ready=0,prevEnd=0,backlog=0,applyWindow=0;
    // G784: up to 32 cores per group; `bucket`/`groupKicks` name the FixMDT bucket.
    uint64_t coreStart[32]{},coreEnd[32]{};
    uint32_t kicks[32]{},bytes[32]{};
    uint32_t core=0,contractOk=0,pending=0,decision=0,bucket=0,groupKicks=13;
    // Most recent contract-register write before the apply: its age in windows and path, and
    // how many contract registers were last written by each path.
    uint32_t latestAge=~0u,latestPath=0,lastBy[4]{};
    std::atomic<uint32_t> parts{0};
};
inline std::mutex timelineMutex;
inline void timelineAtApply(Timeline& t){
    const uint64_t w=gsWindow.load(std::memory_order_relaxed);t.applyWindow=w;
    for(uint32_t r=0;r<128u;++r){
        if(!contractRegister(r) || !regWrites[r].window)continue;
        const uint64_t age=w-regWrites[r].window;++t.lastBy[regWrites[r].path&3u];
        if(age<t.latestAge){t.latestAge=uint32_t(age);t.latestPath=regWrites[r].path;}
    }
}
inline void timelineFinish(Timeline& t,uint64_t id,uint32_t sf){
    if(t.parts.fetch_add(1u,std::memory_order_acq_rel)!=1u)return;
    std::lock_guard<std::mutex> lock(timelineMutex);
    static FILE* file=[](){
        const char* path=std::getenv("DC2_G784_TIMELINE_CSV");
        FILE* f=std::fopen(path?path:"logs/g783/timeline.csv","wb");
        if(f){std::fprintf(f,"id,sf,tpn,bucket,groupKicks,capture,hook,apply,ready,prevEnd,backlog,applyWindow,contractOk,pending,decision,latestAge,latestPath,last1,last2,last3");
            for(int i=0;i<32;++i)std::fprintf(f,",s%d,e%d,k%d,b%d",i,i,i,i);std::fprintf(f,"\n");}
        return f;}();
    if(!file)return;
    std::fprintf(file,"%llu,%u,%.5f,%x,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u",(unsigned long long)id,sf,ticksPerNs(),t.bucket,t.groupKicks,
        (unsigned long long)t.capture,(unsigned long long)t.hook,(unsigned long long)t.apply,(unsigned long long)t.ready,
        (unsigned long long)t.prevEnd,(unsigned long long)t.backlog,(unsigned long long)t.applyWindow,t.contractOk,t.pending,
        t.decision,t.latestAge,t.latestPath,t.lastBy[1],t.lastBy[2],t.lastBy[3]);
    for(int i=0;i<32;++i)std::fprintf(file,",%llu,%llu,%u,%u",(unsigned long long)t.coreStart[i],(unsigned long long)t.coreEnd[i],t.kicks[i],t.bytes[i]);
    std::fprintf(file,"\n");std::fflush(file);
}
inline void startClock(){(void)ticksPerNs();}
inline void report(uint32_t frame,const ProfMutex* mutex){
    if(!profOn())return;
    const double tpn=ticksPerNs();
    // Back-to-back clock cost, so a reader can subtract the instrument from tiny probes.
    uint64_t overhead=~0ull;for(int i=0;i<256;++i){const uint64_t a=ticks();const uint64_t b=ticks();if(b-a<overhead)overhead=b-a;}
    std::fprintf(stderr,"[G783:prof] sf=%u ticksPerNs=%.4f clockOverheadNs=%.1f contended=%llu\n",frame,tpn,
        double(overhead)/tpn,(unsigned long long)(mutex?mutex->contended.load():0ull));
    for(uint32_t p=0;p<ProbeCount;++p){
        Stat sum;uint32_t threads=0;
        for(ThreadBlock* b=blocks.load(std::memory_order_acquire);b;b=b->next){
            const Stat& s=b->s[p];if(!s.calls)continue;++threads;
            sum.calls+=s.calls;sum.cycles+=s.cycles;if(s.max>sum.max)sum.max=s.max;
            for(int i=0;i<48;++i)sum.hist[i]+=s.hist[i];
        }
        if(!sum.calls)continue;
        auto pct=[&](double q){uint64_t need=uint64_t(q*double(sum.calls)),seen=0;
            for(int i=0;i<48;++i){seen+=sum.hist[i];if(seen>=need)return double(2ull<<i)/tpn;}return 0.;};
        std::fprintf(stderr,"[G783:probe] sf=%u name=%s calls=%llu totalMs=%.3f avgNs=%.1f p95Ns<=%.0f p99Ns<=%.0f maxUs=%.2f threads=%u\n",
            frame,probeName(p),(unsigned long long)sum.calls,double(sum.cycles)/tpn/1e6,
            double(sum.cycles)/tpn/double(sum.calls),pct(0.95),pct(0.99),double(sum.max)/tpn/1e3,threads);
    }
}
}
