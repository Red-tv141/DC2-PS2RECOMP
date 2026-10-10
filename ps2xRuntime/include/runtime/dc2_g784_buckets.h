#pragma once
// G784: Dark Cloud 2 FixMDT native buckets of the E8B940 mesh (plans/phase-G784-ledger.md).
//
// Not a generic PS2 renderer abstraction. One row per FixMDT material bucket of mesh `self
// 0xe8b940`; every value is a proof-derived difference between buckets:
//   structured contract  material index, source corners, topology mask (G777 structured input,
//                        logs/g778/g779_roadmap_evidence.json; checked against the live mesh)
//   transport contract   faces and MTVU kicks per group (original VIF stream, G784 audit
//                        logs/g784/gs_packet_audit.json); internal DIRECTs allowed or not
//   deadline             the MTVU decision core. `deadline == kicks` = GS-ONLY: every core runs the
//                        original program (no VU omission, no CLIP authority needed) and G150
//                        drops the whole group's Path1 output. `deadline < kicks` needs an
//                        individual partial-omission replay proof (logs/g784/sweep_singles.json)
//   prim                 the IIP/TME/ABE class every original packet of the bucket uses
//                        (0x5B list / 0x5C strip / 0x5D clipped fan; audit persistent regs = {})
// The live enable set is DC2_G784_BUCKETS (comma list of bucket hex, "all", "none"); unset =
// kDefaultMask. DC2_G784_BUCKETS=e91f00 reproduces G783 exactly.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "dc2_g783_prof.h"

namespace dc2_g784 {
constexpr uint32_t kMeshSelf=0xe8b940u;
struct NativeFixBucketContract {
    uint32_t bucket;       // FixMDT material bucket identity (fingerprinted by the structured mesh)
    uint32_t material;     // material index (draw.materials[material])
    uint32_t corners;      // source corners over every face of the bucket
    uint32_t topologyMask; // bit 3 = list faces, bit 4 = strip faces
    uint32_t faces;        // transport faces (MSCAL ... FLUSHA sequences)
    uint32_t kicks;        // MTVU jobs per group (MSCAL + MSCNT)
    uint32_t deadline;     // MTVU decision core; == kicks => GS-only
    uint32_t prim;         // native envelope PRIM (IIP|TME|ABE triangle)
    bool internalDirect;   // TEXFLUSH/PRMODE DIRECTs may sit between faces (payload checked; every
                           // multi-face bucket carries them live, fixture d E91460/E9A860 did not)
    const char* name;
};
// Index order = original ordered bucket order inside one E8B940 invocation.
inline constexpr std::array<NativeFixBucketContract,8> kBuckets={{
    {0xe8bb40u,0u, 69u,(1u<<3)|(1u<<4), 5u, 6u, 6u,0x5bu,true ,"e8bb40"},
    {0xe8c920u,1u,299u,(1u<<3)|(1u<<4),22u,26u,26u,0x5bu,true ,"e8c920"},
    {0xe8f8c0u,2u,138u,(1u<<3)|(1u<<4), 8u,10u,10u,0x5bu,true ,"e8f8c0"},
    {0xe91460u,3u, 44u,(1u<<3)|(1u<<4), 2u, 2u, 2u,0x5bu,true ,"e91460"},
    {0xe9a860u,5u,136u,(1u<<3)|(1u<<4), 8u,10u,10u,0x5bu,true ,"e9a860"},
    {0xe9c4c0u,6u, 27u,(1u<<3)        , 1u, 1u, 1u,0x5bu,false,"e9c4c0"},
    {0xea1760u,6u,  5u,(1u<<4)        , 1u, 1u, 1u,0x5bu,false,"ea1760"},
    {0xe91f00u,4u,534u,(1u<<3)        , 1u,13u,13u,0x5bu,false,"e91f00"}, // G785: GS-only default
}};
constexpr uint32_t kE91F00=7u;
inline int indexOf(uint32_t bucket){
    for(uint32_t i=0;i<kBuckets.size();++i)if(kBuckets[i].bucket==bucket)return int(i);
    return -1;
}
// Default live set: E91F00 (G783) only until a bucket passes every G784 gate.
constexpr uint32_t kDefaultMask=1u<<kE91F00;
inline uint32_t enabledMask(){
    static const uint32_t mask=[](){
        const char* v=std::getenv("DC2_G784_BUCKETS");
        if(!v)return kDefaultMask;
        if(!std::strcmp(v,"all"))return 0xffu;
        if(!std::strcmp(v,"none"))return 0u;
        uint32_t m=0;const char* p=v;
        while(*p){
            char* end=nullptr;const unsigned long b=std::strtoul(p,&end,16);
            if(end==p)break;
            const int i=indexOf(uint32_t(b));
            if(i>=0)m|=1u<<i;
            p=end;while(*p==',' || *p==' ' || *p=='+')++p;
        }
        std::fprintf(stderr,"[G784:buckets] mask=%02x spec=%s\n",m,v);
        return m;
    }();
    return mask;
}
inline const NativeFixBucketContract* enabled(uint32_t bucket){
    const int i=indexOf(bucket);
    return (i>=0 && (enabledMask()>>i)&1u)?&kBuckets[size_t(i)]:nullptr;
}
// Per-bucket deadline. DC2_G784_K_<name>=k overrides one bucket (diagnostic/proof sweeps only).
// Clamped to [0, kicks]. G785: E91F00 is GS-ONLY by default (deadline = kicks = 13: every MTVU core
// runs, G150 drops the whole group's Path1, no CLIP authority / deadline race). G784 measured k=5
// and k=13 equal on MAP15 (plans/phase-G784-ledger.md section 12). Rollback to the G783 partial VU
// bypass: DC2_G783_DEADLINE_CORE=5 (or DC2_G784_K_e91f00=5).
inline bool vuCeiling();
inline uint32_t deadline(uint32_t index){
    static const std::array<uint32_t,8> values=[](){
        std::array<uint32_t,8> out{};
        for(uint32_t i=0;i<kBuckets.size();++i){
            const auto& c=kBuckets[i];uint32_t k=c.deadline;
            char key[32];std::snprintf(key,sizeof(key),"DC2_G784_K_%s",c.name);
            if(const char* v=std::getenv(key))k=uint32_t(std::strtoul(v,nullptr,0));
            out[i]=k>c.kicks?c.kicks:k;
        }
        return out;
    }();
    if(index==kE91F00 && !std::getenv("DC2_G784_K_e91f00") && std::getenv("DC2_G783_DEADLINE_CORE"))
        return dc2_g783::deadlineCore();
    if(index!=kE91F00 && vuCeiling())return 0u;
    return values[index];
}
// DC2_G784_FORCE_LEGACY=1: diagnostic census mode. Every enabled group posts its G150 marker and
// evaluates the contract (admission timeline / eligibility), then rejects: all groups legacy.
inline bool forceLegacy(){static const bool on=std::getenv("DC2_G784_FORCE_LEGACY")!=nullptr;return on;}
// DC2_G784_CEILING_DROP=1: timing-only CEILING probe (never a correctness configuration). GS-only
// groups other than E91F00 drop their Path1 output and draw nothing (see orderedNativeNonblocking).
inline bool ceilingDrop(){static const bool on=std::getenv("DC2_G784_CEILING_DROP")!=nullptr;return on;}
// DC2_G785_E91F00_DROP=1: the same timing-only CEILING probe for E91F00 itself (GS-only deadline): the
// group commits, G150 drops its Path1 output, nothing is drawn. Never a correctness configuration.
inline bool e91f00Drop(){static const bool on=std::getenv("DC2_G785_E91F00_DROP")!=nullptr;return on;}
// DC2_G785_E91F00_NOKICK=1: G785 timing-only ABLATION. E91F00 stays legacy (G150 rejects), and its Path1
// packets have every drawing kick rewritten to the no-kick register (XYZF2->XYZF3, XYZ2->XYZ3): the GS
// front end parses every byte and vertex, but no primitive (no G144 entry, no executor/GPU draw) results.
inline bool e91f00NoKick(){static const bool on=std::getenv("DC2_G785_E91F00_NOKICK")!=nullptr;return on;}
// DC2_G785_E91F00_SPLIT=1: G785 timing-only ABLATION. At the E91F00 group's G150 admission the open parse batch
// is closed into G713 exactly as the native commit does (g713ParseFlushOpen), and then the group stays legacy
// (or, with DC2_G785_E91F00_DROP, is dropped). Isolates the batch split from the native draw.
inline bool e91f00Split(){static const bool on=std::getenv("DC2_G785_E91F00_SPLIT")!=nullptr;return on;}
// DC2_G785_SPLIT_ALL=1: the same split at EVERY enabled bucket's admission (with DC2_G784_BUCKETS), group legacy.
inline bool splitAll(){static const bool on=std::getenv("DC2_G785_SPLIT_ALL")!=nullptr;return on;}
// DC2_G785_E91F00_EXECPOINT=1: the split PLUS the native node's exec-thread drain (g778FlushExec: execute every
// pending G260 batch), posted as a G713 call node at the E91F00 admission; the group then stays legacy (or is
// dropped with DC2_G785_E91F00_DROP). Isolates the forced mid-frame execution point from the native draw.
inline bool e91f00ExecPoint(){static const bool on=std::getenv("DC2_G785_E91F00_EXECPOINT")!=nullptr;return on;}
// Rewrites drawing-kick registers in GIF packets in place (PACKED REGS nibbles and A+D addresses,
// REGLIST nibbles). Returns the number of rewritten register slots.
inline uint64_t g785StripKicks(std::vector<uint8_t>& pk){
    uint64_t n=0;size_t off=0;const size_t size=pk.size();
    auto kick=[](uint32_t r){return r==0x4u?0xCu:(r==0x5u?0xDu:r);};
    while(off+16<=size){
        uint64_t lo,hi;std::memcpy(&lo,&pk[off],8);std::memcpy(&hi,&pk[off+8],8);
        const uint32_t nloop=uint32_t(lo&0x7fffu),flg=uint32_t((lo>>58)&3u);
        uint32_t nreg=uint32_t((lo>>60)&0xfu);if(!nreg)nreg=16u;
        uint64_t regs=hi,newRegs=0;
        for(uint32_t r=0;r<nreg;++r){const uint32_t x=uint32_t((regs>>(4u*r))&0xfu),y=kick(x);if(x!=y)++n;newRegs|=uint64_t(y)<<(4u*r);}
        if(flg!=2u && nreg<16u)newRegs|=regs&~((1ull<<(4u*nreg))-1ull);
        if(flg!=2u){std::memcpy(&pk[off+8],&newRegs,8);}
        off+=16;
        if(flg==0u){ // PACKED: one qword per register; A+D (0xE) carries the address in the high word
            for(uint32_t i=0;i<nloop && off+16<=size;++i)for(uint32_t r=0;r<nreg && off+16<=size;++r,off+=16){
                if(((regs>>(4u*r))&0xfu)==0xEu){uint64_t a;std::memcpy(&a,&pk[off+8],8);const uint64_t b=(a&~0xffull)|kick(uint32_t(a&0xffu));if(a!=b){++n;std::memcpy(&pk[off+8],&b,8);}}
            }
        }else if(flg==1u){const size_t words=size_t(nloop)*nreg;off+=((words+1u)/2u)*16u;}
        else off+=size_t(nloop)*16u;
    }
    return n;
}
// DC2_G784_CEILING_VU=1: timing-only VU+GS CEILING probe for the seven non-E91F00 buckets: deadline 0,
// and the MTVU commits the group native itself at core 0 (no race), so every core is bypassed and
// nothing is drawn. All seven omitted together = the replay-proven cluster (logs/g784/sweep_chains).
inline bool vuCeiling(){static const bool on=std::getenv("DC2_G784_CEILING_VU")!=nullptr;return on;}

// Per-bucket live counters (release-safe: relaxed atomics on the per-group path only).
struct BucketCounters {
    std::atomic<uint64_t> captures{0},hooks{0},contractRefused{0},groups{0},native{0},legacy{0};
    std::atomic<uint64_t> deadlineLegacy{0},parseReject{0},droppedItems{0},droppedPackets{0},droppedBytes{0};
    std::atomic<uint64_t> bypassGroups{0},avoidedCores{0},eligible{0},noCommand{0},directRefused{0};
};
inline std::array<BucketCounters,8> counters{};
inline void report(uint32_t sf){
    for(uint32_t i=0;i<kBuckets.size();++i){
        const auto& c=counters[i];
        if(!c.captures.load() && !c.hooks.load())continue;
        std::fprintf(stderr,"[G784:bucket] sf=%u b=%s k=%u/%u captures=%llu hooks=%llu noCommand=%llu contractRefused=%llu directRefused=%llu groups=%llu eligible=%llu native=%llu legacy=%llu deadline=%llu reject=%llu droppedItems=%llu droppedPackets=%llu droppedBytes=%llu bypass=%llu avoidedCores=%llu\n",
            sf,kBuckets[i].name,deadline(i),kBuckets[i].kicks,
            (unsigned long long)c.captures.load(),(unsigned long long)c.hooks.load(),(unsigned long long)c.noCommand.load(),
            (unsigned long long)c.contractRefused.load(),(unsigned long long)c.directRefused.load(),
            (unsigned long long)c.groups.load(),(unsigned long long)c.eligible.load(),
            (unsigned long long)c.native.load(),(unsigned long long)c.legacy.load(),
            (unsigned long long)c.deadlineLegacy.load(),(unsigned long long)c.parseReject.load(),
            (unsigned long long)c.droppedItems.load(),(unsigned long long)c.droppedPackets.load(),
            (unsigned long long)c.droppedBytes.load(),(unsigned long long)c.bypassGroups.load(),
            (unsigned long long)c.avoidedCores.load());
    }
}

#if defined(PS2X_G784_CENSUS)
// ===== DIAGNOSTIC ROUTE CENSUS (DC2_G784_CENSUS=1, compiled out of release) =====================
// Every E8B940 bucket's legacy pipeline cost, attributed by the narrow association that already
// tags each MTVU item (g775Draw) -- no broad provenance.
inline bool censusOn(){static const bool on=std::getenv("DC2_G784_CENSUS")!=nullptr;return on;}
struct Census {
    // MTVU worker
    std::atomic<uint64_t> items{0},starts{0},resumes{0},pairs{0},vuTicks{0},packets{0},gifBytes{0},gifLoops{0};
    std::atomic<uint64_t> drawVerts{0},nodrawVerts{0},groups{0},nonempty{0};
    std::array<std::atomic<uint64_t>,32> ordinalTicks{},ordinalItems{};
    // G150 parse
    std::atomic<uint64_t> parsePackets{0},parseTicks{0},parseBatches{0},parseEntries{0};
};
inline std::array<Census,8> census{};
inline std::atomic<uint64_t> otherPath1Ticks{0},otherPath1Packets{0},otherPath23Ticks{0},otherPath23Packets{0},windows{0};
inline thread_local std::vector<uint8_t> collectTags;  // G150: bucket index + 1 per collected Path1 packet
inline thread_local uint64_t parseBatchesPosted=0;     // G150: g713 batches posted (flush edges)
inline thread_local uint64_t parseEntriesPosted=0;
// GIF packet walk: loops, and PACKED XYZ(F)2 (drawing kick) vs XYZ(F)3 (no kick) vertices.
inline void gifVerts(const uint8_t* p,size_t bytes,uint32_t& loops,uint32_t& draw,uint32_t& nodraw){
    size_t at=0;
    while(at+16<=bytes){
        uint64_t lo,hi;std::memcpy(&lo,p+at,8);std::memcpy(&hi,p+at+8,8);
        const uint32_t nloop=uint32_t(lo&0x7fffu),flg=uint32_t((lo>>58)&3u),nreg0=uint32_t((lo>>60)&15u);
        const uint32_t nreg=nreg0?nreg0:16u;loops+=nloop;at+=16;
        if(flg==0){
            for(uint32_t r=0;r<nreg;++r){
                const uint32_t reg=uint32_t((hi>>(4*r))&15u);
                if(reg==4u || reg==5u)draw+=nloop;else if(reg==12u || reg==13u)nodraw+=nloop;
            }
            at+=size_t(nloop)*nreg*16u;
        }else if(flg==1)at+=size_t((nloop*nreg+1u)/2u)*16u;else at+=size_t(nloop)*16u;
        if(lo&(1ull<<15))break; // EOP
    }
}
inline void censusReport(uint32_t n){
    const double tpn=dc2_g783::ticksPerNs();
    for(uint32_t i=0;i<kBuckets.size();++i){
        const auto& c=census[i];
        if(!c.items.load() && !c.parsePackets.load())continue;
        char ord[512];size_t at=0;ord[0]=0;
        for(uint32_t k=0;k<kBuckets[i].kicks && k<32u && at+16<sizeof(ord);++k){
            const uint64_t items=c.ordinalItems[k].load();
            at+=size_t(std::snprintf(ord+at,sizeof(ord)-at,"%s%.1f",k?",":"",items?double(c.ordinalTicks[k].load())/tpn/1e3/double(items):0.));
        }
        std::fprintf(stderr,"[G784:census] n=%u b=%s groups=%llu nonempty=%llu items=%llu starts=%llu resumes=%llu pairs=%llu vuMs=%.3f packets=%llu gifBytes=%llu gifLoops=%llu drawVerts=%llu nodrawVerts=%llu parsePackets=%llu parseMs=%.3f parseBatches=%llu parseEntries=%llu ordUs=%s\n",
            n,kBuckets[i].name,(unsigned long long)c.groups.load(),(unsigned long long)c.nonempty.load(),
            (unsigned long long)c.items.load(),(unsigned long long)c.starts.load(),(unsigned long long)c.resumes.load(),
            (unsigned long long)c.pairs.load(),double(c.vuTicks.load())/tpn/1e6,(unsigned long long)c.packets.load(),
            (unsigned long long)c.gifBytes.load(),(unsigned long long)c.gifLoops.load(),(unsigned long long)c.drawVerts.load(),
            (unsigned long long)c.nodrawVerts.load(),(unsigned long long)c.parsePackets.load(),double(c.parseTicks.load())/tpn/1e6,
            (unsigned long long)c.parseBatches.load(),(unsigned long long)c.parseEntries.load(),ord);
    }
    std::fprintf(stderr,"[G784:census-other] n=%u windows=%llu path1Packets=%llu path1Ms=%.3f path23Packets=%llu path23Ms=%.3f\n",n,
        (unsigned long long)windows.load(),(unsigned long long)otherPath1Packets.load(),double(otherPath1Ticks.load())/tpn/1e6,
        (unsigned long long)otherPath23Packets.load(),double(otherPath23Ticks.load())/tpn/1e6);
}
#endif
}
