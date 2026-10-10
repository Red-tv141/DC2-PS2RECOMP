#pragma once
// G789 Phase 1 only. This header is included exclusively in G775 diagnostic TUs.
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <utility>

extern std::atomic<uint32_t> g_dc2RenderedFrame;
extern std::atomic<uint32_t> g_dc2ScriptFrame;

#include "runtime/dc2_g789_live.h"
namespace dc2_g789 {
inline bool on() { static const bool v=[] { const char* e=std::getenv("DC2_G789_CENSUS");return e && std::strcmp(e,"0"); }();return v; }
inline bool trackingOn(){return on() || dc2_g789_live::requested();}
inline uint32_t frame() { return g_dc2RenderedFrame.load(std::memory_order_relaxed)+1u; }
enum Pass : uint32_t { Unknown, Body, Silhouette, Shadow, Effect, OtherDirect, QueuedFrame };
enum Mesh : uint32_t { Other, Motion, Fix };
enum Stage : uint32_t { Unowned, Geometry, Setup };
struct Identity {
    uint64_t draw=0, operation=0;
    uint32_t frame=0,sf=0,object=0,root=0,visual=0,vtable=0,bucket=0,pass=0,mesh=0,stage=0;
    uint32_t producer=0,caller=0,objectVtable=0,commandAddress=0;
    uint64_t wtex0=0,commitment=0;
    uint32_t wWidth=0,wHeight=0,wOffsetX=0,wOffsetY=0;
};
// Read-only source-side admission probe. A marker is published by the exact
// AddPacket return, before DMA/VIF reaches MTVU. It never suppresses a kick.
inline bool admissionAuditOn() { static const bool v=[] { const char* e=std::getenv("DC2_G789_ADMISSION_AUDIT");return e && std::strcmp(e,"0"); }();return v; }
struct AdmissionProbe {
    Identity identity{};
    uint64_t source=0,instance=0;
    uint32_t geometry=0,packets=0,triangles=0,paletteBytes=0;
    std::array<uint32_t,4> first{};
};
inline std::mutex admissionAuditMutex;
inline std::unordered_map<uint64_t,AdmissionProbe> admissionProbes;
inline FILE* admissionAuditFile() {
    static FILE* f=[] {
        const char* path=std::getenv("DC2_G789_ADMISSION_OUT");
        FILE* p=std::fopen(path?path:"logs/g789/admission_probe.jsonl","wb");
        if(!p){std::fprintf(stderr,"[G789:admission] FATAL output open failed\n");std::abort();}
        std::setvbuf(p,nullptr,_IOFBF,1u<<20);return p;
    }();return f;
}
inline void admissionSeen(const Identity& id,uint32_t geometry,uint32_t reason) {
    if(!admissionAuditOn() || id.frame<1800u || id.frame>1831u)return;
    std::lock_guard<std::mutex> lock(admissionAuditMutex);
    std::fprintf(admissionAuditFile(),"{\"event\":\"seen\",\"frame\":%u,\"draw\":%llu,\"visual\":%u,\"bucket\":%u,\"pass\":%u,\"mesh\":%u,\"geometry\":%u,\"reason\":%u}\n",
        id.frame,(unsigned long long)id.draw,id.visual,id.bucket,id.pass,id.mesh,geometry,reason);
    std::fflush(admissionAuditFile());
}
inline void admissionMark(const AdmissionProbe& probe) {
    if(!admissionAuditOn())return;
    std::lock_guard<std::mutex> lock(admissionAuditMutex);
    const bool unique=admissionProbes.emplace(probe.identity.draw,probe).second;
    FILE* f=admissionAuditFile();
    std::fprintf(f,"{\"event\":\"add\",\"frame\":%u,\"draw\":%llu,\"visual\":%u,\"bucket\":%u,\"pass\":%u,\"geometry\":%u,\"source\":%llu,\"instance\":%llu,\"packets\":%u,\"triangles\":%u,\"paletteBytes\":%u,\"first\":[%u,%u,%u,%u],\"unique\":%u}\n",
        probe.identity.frame,(unsigned long long)probe.identity.draw,probe.identity.visual,probe.identity.bucket,probe.identity.pass,probe.geometry,
        (unsigned long long)probe.source,(unsigned long long)probe.instance,probe.packets,probe.triangles,probe.paletteBytes,
        probe.first[0],probe.first[1],probe.first[2],probe.first[3],unsigned(unique));
    std::fflush(f);
}
inline void admissionKick(const Identity& id,uint32_t startPC,uint32_t itop,uint32_t top,bool resume) {
    if(!admissionAuditOn() || !id.draw)return;
    std::lock_guard<std::mutex> lock(admissionAuditMutex);
    const auto found=admissionProbes.find(id.draw);
    if(found==admissionProbes.end())return;
    const auto& p=found->second;
    const bool exact=p.identity.frame==id.frame && p.identity.visual==id.visual && p.identity.bucket==id.bucket &&
        p.identity.pass==id.pass && p.identity.mesh==id.mesh && (id.stage==Geometry || id.stage==Setup);
    FILE* f=admissionAuditFile();
    std::fprintf(f,"{\"event\":\"kick\",\"frame\":%u,\"draw\":%llu,\"bucket\":%u,\"stage\":%u,\"startPC\":%u,\"itop\":%u,\"top\":%u,\"resume\":%u,\"exact\":%u}\n",
        id.frame,(unsigned long long)id.draw,id.bucket,id.stage,startPC,itop,top,unsigned(resume),unsigned(exact));
    std::fflush(f);
}
inline uint32_t word(const uint8_t* ram,uint32_t at) { uint32_t v=0;if(at>=0x10000u && at<=0x2000000u-4u)std::memcpy(&v,ram+at,4);return v; }
inline std::atomic<uint64_t> nextDraw{0},nextOperation{0},ambiguous{0},mismatch{0},overwritten{0},missingResume{0};
struct Watch { Identity identity;std::array<uint32_t,4> first{}; };
inline thread_local std::unordered_map<uint32_t,Watch> watches;
inline thread_local std::unordered_map<uint32_t,bool> ends;
inline thread_local uint32_t watchFrame=0;
inline void watch(uint32_t address,Identity id,const uint8_t* ram) {
    if(!trackingOn() || address<0x10000u || address>0x2000000u-16u)return;
    if(watchFrame!=id.frame){watches.clear();ends.clear();watchFrame=id.frame;}
    Watch w;w.identity=id;std::memcpy(w.first.data(),ram+address,16);
    auto it=watches.find(address);
    if(it!=watches.end() && it->second.identity.draw!=id.draw){it->second.identity={};++ambiguous;return;}
    watches[address]=w;
}
struct Origin { size_t begin=0,end=0;Identity identity;uint32_t address=0; };
inline std::mutex streamMutex;
inline std::unordered_map<const uint8_t*,std::shared_ptr<const std::vector<Origin>>> streams;
struct Chain {
    const uint8_t* ram=nullptr;Identity current{},next{};std::vector<Identity> stack;std::vector<Origin> origins;
    explicit Chain(const uint8_t* r):ram(r){}
    void tag(uint32_t address,uint32_t id,uint32_t target,bool scratch) {
        if(!trackingOn())return;
        if(ends.count(address))current={};
        // Direct packet segments need no surrounding CALL. Their exact first-tag identity
        // and explicit end boundary prevent context from leaking to adjacent effects.
        if(!scratch){auto own=watches.find(address);if(own!=watches.end()){
            if(own->second.identity.draw && std::memcmp(own->second.first.data(),ram+address,16)==0)current=own->second.identity;
            else {current={};++mismatch;}
        }}
        next=current;
        if(id==5u){
            stack.push_back(current);
            if(!scratch){auto it=watches.find(target&0x1fffffffu);if(it!=watches.end()){
                const auto& w=it->second;
                if(w.identity.draw && std::memcmp(w.first.data(),ram+(target&0x1fffffffu),16)==0)next=w.identity;
                else {next={};++mismatch;}
            }}
        }else if(id==6u){if(!stack.empty()){next=stack.back();stack.pop_back();}else next={};}
    }
    void finish(){if(trackingOn())current=next;}
    void append(size_t offset,size_t bytes,uint32_t address=0) {
        if(!trackingOn() || !bytes || !current.draw)return;
        if(!origins.empty() && origins.back().end==offset && origins.back().identity.draw==current.draw && origins.back().identity.stage==current.stage && origins.back().address+origins.back().end-origins.back().begin==address)origins.back().end+=bytes;
        else origins.push_back({offset,offset+bytes,current,address});
    }
    void publish(const uint8_t* data) {
        if(!trackingOn())return;
        std::lock_guard<std::mutex> lock(streamMutex);
        if(streams.erase(data))++overwritten;
        if(!origins.empty())streams[data]=std::make_shared<const std::vector<Origin>>(std::move(origins));
    }
};
inline thread_local Identity vifIdentity{};
inline thread_local std::shared_ptr<const std::vector<Origin>> vifOrigins;
inline thread_local size_t vifIndex=0;
struct InputScope {
    explicit InputScope(const uint8_t* data) {
        if(!trackingOn())return;
        vifIdentity={};vifIndex=0;vifOrigins.reset();
        std::lock_guard<std::mutex> lock(streamMutex);auto it=streams.find(data);
        if(it!=streams.end()){vifOrigins=std::move(it->second);streams.erase(it);}
    }
    ~InputScope(){if(trackingOn()){vifIdentity={};vifOrigins.reset();}}
};
inline void command(size_t pos) {
    if(!trackingOn())return;
    vifIdentity={};if(!vifOrigins)return;
    while(vifIndex<vifOrigins->size() && pos>=(*vifOrigins)[vifIndex].end)++vifIndex;
    if(vifIndex<vifOrigins->size() && pos>=(*vifOrigins)[vifIndex].begin){
        const auto& origin=(*vifOrigins)[vifIndex];vifIdentity=origin.identity;
        vifIdentity.commandAddress=origin.address+uint32_t(pos-origin.begin);
    }
}
struct Row {
    Identity identity{};uint64_t kicks=0,resumes=0,ns=0,zeroKicks=0,zeroNs=0,bytes=0,writes=0;
};
struct RowKey {
    uint64_t draw;uint32_t bucket,stage;
    bool operator==(const RowKey& b)const{return draw==b.draw && bucket==b.bucket && stage==b.stage;}
};
struct RowHash {size_t operator()(const RowKey& k)const{return size_t(k.draw*0x9e3779b97f4a7c15ull)^size_t(k.bucket*31u+k.stage);}};
inline FILE* output() {
    static FILE* f=[] {
        const char* name=std::getenv("DC2_G789_OUT");
        FILE* p=std::fopen(name?name:"logs/g789/census.jsonl","wb");
        if(!p){std::fprintf(stderr,"[G789:census] FATAL output open failed\n");std::abort();}
        std::setvbuf(p,nullptr,_IOFBF,1u<<20);return p;
    }();return f;
}
inline void engine(const Identity& d,const uint8_t* ram,uint32_t kind,uint32_t caller,uint32_t result) {
    if(!on())return;
    static FILE* f=[] {
        const char* name=std::getenv("DC2_G789_ENGINE_OUT");if(!name)return static_cast<FILE*>(nullptr);
        FILE* p=std::fopen(name,"wb");if(!p){std::fprintf(stderr,"[G789:census] FATAL engine output open failed\n");std::abort();}
        std::setvbuf(p,nullptr,_IOFBF,1u<<20);return p;
    }();
    if(!f)return;
    static uint32_t lastFrame=0;
    if(lastFrame!=d.frame){std::fflush(f);lastFrame=d.frame;}
    uint32_t palette=0;
    if(d.mesh==Motion)while(palette<32u && int32_t(word(ram,d.visual+0x80u+palette*4u))>=0)++palette;
    std::fprintf(f,"{\"frame\":%u,\"finishFrame\":%u,\"sf\":%u,\"kind\":%u,\"draw\":%llu,\"op\":%llu,\"object\":%u,\"root\":%u,\"objectVtable\":%u,\"producer\":%u,\"caller\":%u,\"visual\":%u,\"vtable\":%u,\"pass\":%u,\"mesh\":%u,\"result\":%u,\"positions\":%u,\"positionCount\":%u,\"normals\":%u,\"weights\":%u,\"weightCount\":%u,\"paletteCount\":%u,\"frames\":%u,\"bindSelector\":%u,\"bindMatrices\":%u}\n",
        d.frame,frame(),d.sf,kind,(unsigned long long)d.draw,(unsigned long long)d.operation,d.object,d.root,d.objectVtable,d.producer,caller,d.visual,d.vtable,d.pass,d.mesh,result,
        d.visual?word(ram,d.visual+0x30u):0,d.visual?word(ram,d.visual+0x20u):0,d.visual?word(ram,d.visual+0x34u):0,
        d.mesh==Motion?word(ram,d.visual+0x104u):0,d.mesh==Motion?word(ram,d.visual+0x100u):0,palette,
        d.mesh==Motion?word(ram,d.visual+0x50u):0,d.mesh==Motion?word(ram,d.visual+0x54u):0,d.mesh==Motion?word(ram,d.visual+0x58u):0);
}
struct WorkerCensus {
    uint32_t currentFrame=0;std::unordered_map<RowKey,Row,RowHash> rows;
    void flush() {
        if(!currentFrame)return;
        FILE* f=output();
        for(const auto& pair:rows){const auto& r=pair.second;const auto& d=r.identity;
            std::fprintf(f,"{\"frame\":%u,\"sf\":%u,\"draw\":%llu,\"op\":%llu,\"object\":%u,\"root\":%u,\"visual\":%u,\"vtable\":%u,\"bucket\":%u,\"pass\":%u,\"mesh\":%u,\"stage\":%u,\"producer\":%u,\"caller\":%u,\"objectVtable\":%u,\"kicks\":%llu,\"resumes\":%llu,\"ns\":%llu,\"zeroKicks\":%llu,\"zeroNs\":%llu,\"path1Bytes\":%llu,\"writeBytes\":%llu}\n",
                currentFrame,d.sf,(unsigned long long)d.draw,(unsigned long long)d.operation,d.object,d.root,d.visual,d.vtable,d.bucket,d.pass,d.mesh,d.stage,
                d.producer,d.caller,d.objectVtable,
                (unsigned long long)r.kicks,(unsigned long long)r.resumes,(unsigned long long)r.ns,(unsigned long long)r.zeroKicks,(unsigned long long)r.zeroNs,(unsigned long long)r.bytes,(unsigned long long)r.writes);
        }
        std::fprintf(f,"{\"completeFrame\":%u,\"rows\":%zu,\"ambiguous\":%llu,\"mismatch\":%llu,\"overwritten\":%llu,\"missingResume\":%llu}\n",currentFrame,rows.size(),(unsigned long long)ambiguous.load(),(unsigned long long)mismatch.load(),(unsigned long long)overwritten.load(),(unsigned long long)missingResume.load());
        std::fflush(f);rows.clear();
    }
    void add(Identity id,bool resume,bool zero,uint64_t ns,size_t bytes,size_t writes) {
        if(!on())return;
        if(id.frame!=currentFrame){flush();currentFrame=id.frame;}
        // Bucket/stage remain separate; the draw itself is immutable and unique.
        const RowKey key{id.draw,id.bucket,id.stage};
        auto& r=rows[key];
        r.identity=id;++r.kicks;r.resumes+=resume;r.ns+=ns;r.bytes+=bytes;r.writes+=writes;
        if(zero){++r.zeroKicks;r.zeroNs+=ns;}
    }
};
} // namespace dc2_g789
