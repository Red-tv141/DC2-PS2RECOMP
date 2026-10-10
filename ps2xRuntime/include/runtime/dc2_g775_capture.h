#pragma once
#if defined(PS2X_G775_DIAG) || defined(PS2X_G777_NATIVE_GL)
#include "dc2_g778_flags.h"
#include "dc2_g783_prof.h"
#include "dc2_g784_buckets.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace dc2_g775_capture
{
inline bool enabled()
{
#if defined(PS2X_G775_DIAG)
    static const bool value = std::getenv("DC2_G775_CENSUS") || std::getenv("DC2_G775_SHADOW");
    return value;
#else
    return false;
#endif
}
inline bool tracking(){return enabled() || dc2_g778::requested();}
inline bool shadow() {
#if defined(PS2X_G775_DIAG)
    static const bool value = std::getenv("DC2_G775_SHADOW") != nullptr; return value;
#else
    return false;
#endif
}
inline uint32_t firstFrame()
{
    static const uint32_t value = [] { const char* p = std::getenv("DC2_G775_CAPTURE_SF"); return p ? uint32_t(std::strtoul(p, nullptr, 0)) : 1500u; }();
    return value;
}
struct Draw
{
    uint64_t id=0;
    uint32_t sf=0, self=0, seam=0;
    uint32_t parent=0, world=0, manager=0, vtable=0, reason=1, state=0;
    uint32_t geometry=0,listNode=0,bucket=0,call=0;
};
struct Range { uint32_t begin=0, end=0; Draw draw; };
struct Origin { size_t begin=0, end=0; Draw draw; uint32_t source=0; };
inline dc2_g783::ProfMutex mutex;
inline std::atomic<uint64_t> drawCount{0};
inline std::unordered_map<uint32_t,std::vector<Range>> pages;
inline std::unordered_map<const uint8_t*,std::shared_ptr<std::vector<Origin>>> streams;
inline std::unordered_map<uint64_t,bool> captured;
inline uint32_t rangeFrame=0;
struct MeshSource { uint64_t version=0; std::array<uint32_t,4> counts{}; std::array<std::vector<uint8_t>,4> arrays; };
inline std::unordered_map<uint32_t,MeshSource> meshSources;
inline uint64_t sourceVersion=0;
inline std::unordered_map<uint32_t,std::unordered_map<uint32_t,std::vector<uint8_t>>> faceSources;
struct RetainedFace {uint32_t address=0;std::array<uint8_t,0x18> header{};std::vector<uint8_t> indices;};
struct RetainedBucket {std::array<uint8_t,0x20> header{};std::vector<RetainedFace> faces;};
inline std::unordered_map<uint32_t,std::unordered_map<uint32_t,RetainedBucket>> loadedBuckets;
inline thread_local std::vector<uint32_t> assignmentStack;
inline std::atomic<bool> dependencyWindow{false};
inline bool completeStateCapture()
{
#if defined(PS2X_G775_DIAG)
    static const bool value=std::getenv("DC2_G776_STATE_WINDOW")!=nullptr;return value;
#else
    return false;
#endif
}
inline bool instanceContext();
inline thread_local Draw engineDraw{}, vifDraw{};
inline thread_local std::shared_ptr<std::vector<Origin>> inputOrigins;
inline thread_local size_t originIndex=0;

inline bool valid(uint32_t address, size_t bytes)
{ return address >= 0x10000u && address < 0x02000000u && bytes <= 0x02000000u-address; }
inline void rememberMesh(uint32_t self,const uint8_t* ram)
{
    self &= 0x1FFFFFFFu;
    if(!tracking() || (!enabled() && self!=0xe8b940) || !valid(self,0x50))return;
    MeshSource source;
    for(uint32_t i=0;i<4;++i)
    {
        uint32_t address=0;std::memcpy(&source.counts[i],ram+self+0x20+i*4,4);
        std::memcpy(&address,ram+self+0x30+i*4,4);address &= 0x1FFFFFFFu;
        if(source.counts[i]>65536 || (source.counts[i] && !valid(address,size_t(source.counts[i])*16)))return;
        if(source.counts[i])source.arrays[i].assign(ram+address,ram+address+size_t(source.counts[i])*16);
    }
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    source.version=++sourceVersion;meshSources[self]=std::move(source);
}
inline void beginMeshAssignment(uint32_t self)
{
    if(!instanceContext() || (!enabled() && (self&0x1fffffffu)!=0xe8b940))return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    faceSources.erase(self&0x1fffffffu);
    loadedBuckets.erase(self&0x1fffffffu);
    meshSources.erase(self&0x1fffffffu);
    assignmentStack.push_back(self&0x1fffffffu);
}
inline void endMeshAssignment(uint32_t self)
{if(instanceContext() && !assignmentStack.empty() && assignmentStack.back()==(self&0x1fffffffu))assignmentStack.pop_back();}
inline void rememberLoadedBuckets(uint32_t self,const uint8_t* ram,uint32_t firstFace)
{
    self&=0x1fffffffu;
    if(!instanceContext() || (!enabled() && self!=0xe8b940) || !valid(self,0x50) || !valid(firstFace,0x18))return;
    // FIX loading recycles temporary face addresses BETWEEN material buckets.
    // Freeze the complete topology of each not-yet-built bucket, not face pointer identities.
    auto word=[](const uint8_t* p){uint32_t v;std::memcpy(&v,p,4);return v;};
    uint16_t material=0;std::memcpy(&material,ram+firstFace+4,2);
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    auto& retained=loadedBuckets[self];
    uint32_t bucket=word(ram+self+0x48)&0x1fffffffu;
    for(uint32_t b=0;b<256 && valid(bucket,0x20);++b)
    {
        bool contains=false;
        if(word(ram+bucket)==material)
        {
            uint32_t p=word(ram+bucket+4)&0x1fffffffu;
            for(uint32_t n=0;n<256 && valid(p,0x18);++n){if(p==firstFace){contains=true;break;}p=word(ram+p+0x10)&0x1fffffffu;}
        }
        if(contains)
        {
            RetainedBucket copy;std::memcpy(copy.header.data(),ram+bucket,copy.header.size());
            uint32_t face=word(ram+bucket+4)&0x1fffffffu;bool complete=true;
            for(uint32_t n=0;face && n<256;++n)
            {
                if(!valid(face,0x18)){complete=false;break;}
                RetainedFace f;f.address=face;std::memcpy(f.header.data(),ram+face,f.header.size());
                uint16_t words=0;std::memcpy(&words,f.header.data()+6,2);uint32_t address=word(f.header.data()+12)&0x1fffffffu;
                if(!valid(address,size_t(words)*4)){complete=false;break;}
                const auto prior=retained.find(bucket);
                const RetainedFace* cached=nullptr;
                if(prior!=retained.end())for(const auto& previous:prior->second.faces)if(previous.address==face){cached=&previous;break;}
                if(cached)
                {
                    if(cached->indices.size()!=size_t(words)*4 || std::memcmp(cached->header.data(),f.header.data(),12)){complete=false;break;}
                    f.indices=cached->indices;
                }
                else f.indices.assign(ram+address,ram+address+size_t(words)*4);
                copy.faces.push_back(std::move(f));face=word(ram+face+0x10)&0x1fffffffu;
            }
            if(face)complete=false;
            if(complete && !copy.faces.empty())
            {
                static uint32_t reports=0;
                if(reports++<16)std::fprintf(stderr,"[G776:topology] self=%x material=%u bucket=%x faces=%zu loaderScope=%u\n",self,material,bucket,copy.faces.size(),unsigned(!assignmentStack.empty()));
                retained[bucket]=std::move(copy);
            }
        }
        bucket=word(ram+bucket+8)&0x1fffffffu;
    }
}
inline void rememberFace(uint32_t self,uint32_t face,const uint8_t* ram)
{
    if(!instanceContext() || !valid(face,0x18) || (!enabled() && (self&0x1fffffffu)!=0xe8b940u))return;
    uint16_t count=0;uint32_t address=0;
    std::memcpy(&count,ram+face+6,2);std::memcpy(&address,ram+face+12,4);
    address&=0x1fffffffu;
    if(!valid(address,size_t(count)*4))return;
    std::vector<uint8_t> indices(ram+address,ram+address+size_t(count)*4);
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    faceSources[self&0x1fffffffu][face]=std::move(indices);
}
inline void addRange(uint32_t address, size_t bytes, Draw draw)
{
    address &= 0x1FFFFFFFu;
    if (!enabled() || !draw.id || !valid(address,bytes) || !bytes) return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    if (rangeFrame != draw.sf) { pages.clear(); rangeFrame=draw.sf; }
    const Range r{address,uint32_t(address+bytes),draw};
    for(uint32_t page=address/4096;page<=(r.end-1)/4096;++page) pages[page].push_back(r);
}
inline Draw owner(uint32_t address, size_t bytes)
{
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    const auto it=pages.find(address/4096);
    Draw found{};
    if(it==pages.end()) return found;
    for(const auto& r:it->second) if(address>=r.begin && address<r.end && bytes<=r.end-address)
    {
        // Reused packet storage with several draw identities is explicitly unowned.
        if(found.id && found.id!=r.draw.id) { Draw refused; refused.reason=2; return refused; }
        found=r.draw;
    }
    return found;
}
inline Draw exactBucketOwner(uint32_t address,size_t bytes,uint64_t invocation)
{
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);Draw found{};
    const auto page=pages.find(address/4096);if(page==pages.end())return found;
    for(const auto& range:page->second)if(range.draw.id==invocation && range.draw.bucket &&
        address>=range.begin && address<range.end && bytes<=range.end-address)
    {
        if(found.bucket && found.bucket!=range.draw.bucket)return {};
        found=range.draw;
    }
    return found;
}
inline void addPacketRanges(uint32_t start,const uint8_t* ram,Draw draw)
{
    uint32_t pc=start&0x1FFFFFFFu,stack[2]={},depth=0;
    for(uint32_t n=0;n<16384 && valid(pc,16);++n)
    {
        uint32_t words[4];std::memcpy(words,ram+pc,16);
        const uint32_t qwc=words[0]&65535,id=(words[0]>>28)&7,address=words[1]&0x1FFFFFFFu;
        addRange(pc,16,draw);
        if(id==0 || id==3 || id==4)addRange(address,size_t(qwc)*16,draw);
        else addRange(pc+16,size_t(qwc)*16,draw);
        const uint32_t next=pc+16+qwc*16;
        if(id==0 || id==7)return;
        if(id==1)pc=next;
        else if(id==2)pc=address;
        else if(id==3 || id==4)pc+=16;
        else if(id==5){if(depth==2)return;stack[depth++]=next;pc=address;}
        else if(id==6){if(!depth)return;pc=stack[--depth];}
    }
}
#include "dc2_g776_attribution.inc"
#include "dc2_g783_assoc.inc"
struct Chain
{
    std::vector<Origin> origins;
    CallContext context;
    // G783: broad G775/G776 attribution runs only for the census, the DC2_G783_BROAD rollback
    // and the DC2_G783_DIFF differential; the target-scoped contract runs otherwise.
    const bool broad=broadOn();
    const bool narrow=narrowOn() && !dc2_g783::off(dc2_g783::OffChain) &&
        g783::targetEntries.load(std::memory_order_relaxed)!=0u;
    g783::Context narrowContext;
    std::vector<Origin> narrowOrigins;
    void tag(uint32_t address,uint32_t id,uint32_t qwc,uint32_t target,bool scratch)
    {
        if(broad)context.tag(address,id,qwc,target,scratch);
        if(narrow){dc2_g783::Scope g783(dc2_g783::ChainTag);narrowContext.tag(address,id,qwc,target,scratch);}
    }
    void append(size_t offset,uint32_t source,size_t bytes,bool scratch)
    {
        if(!bytes) return;
        if(narrow && !scratch && g783IsTarget(narrowContext.current))
        {
            const Draw& draw=narrowContext.current;
            if(!narrowOrigins.empty() && narrowOrigins.back().end==offset &&
               g783::sameAssociation(narrowOrigins.back().draw,draw))narrowOrigins.back().end=offset+bytes;
            else narrowOrigins.push_back({offset,offset+bytes,draw,source});
        }
        if(!broad || dc2_g783::off(dc2_g783::OffChain)) return;
        dc2_g783::Scope g783(dc2_g783::ChainAppend);
        Draw draw;
        if(scratch) draw.reason=3;
        else
        {
            draw=owner(source,bytes);
            if(instanceContext() && context.current.reason>=4)draw=context.current;
            if(instanceContext() && context.current.id)
            {
                if(draw.id && draw.id!=context.current.id) { draw={}; draw.reason=5; }
                else {
                    draw=context.current;
                    if(!draw.bucket){
                        const auto bucket=exactBucketOwner(source,bytes,draw.id);
                        if(bucket.bucket){draw.bucket=bucket.bucket;draw.geometry=bucket.geometry;}
                    }
                }
            }
        }
        origins.push_back({offset,offset+bytes,draw,source});
    }
    void publish(const uint8_t* bytes)
    {
        if(narrowOn())
        {
            // A chain with no target chunk publishes nothing; drop a stale entry for a reused
            // buffer so a later chain can never read an earlier chain's origins.
            if(!narrowOrigins.empty() || g783::published.load(std::memory_order_relaxed))
            {
                dc2_g783::Scope g783(dc2_g783::ChainPublish);
                std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
                const auto it=g783::streams.find(bytes);
                if(it!=g783::streams.end()){g783::streams.erase(it);g783::published.fetch_sub(1u,std::memory_order_relaxed);}
                if(!narrowOrigins.empty())
                {
                    g783::streams[bytes]=std::make_shared<std::vector<Origin>>(std::move(narrowOrigins));
                    g783::published.fetch_add(1u,std::memory_order_relaxed);
                }
            }
        }
        if(!broad || origins.empty()) return;
        dc2_g783::Scope g783(dc2_g783::ChainPublish);
        std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
        streams[bytes]=std::make_shared<std::vector<Origin>>(std::move(origins));
    }
};
struct InputScope
{
    explicit InputScope(const uint8_t* data)
    {
        if(!tracking() || dc2_g783::off(dc2_g783::OffVif)) return;
        dc2_g783::Scope g783(dc2_g783::InputScope);
        if(narrowOn())
        {
            g783::inputOrigins=nullptr;g783::originIndex=0;g783::nextChange=0;g783::vifNarrow={};
            if(g783::published.load(std::memory_order_relaxed))
            {
                std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
                auto it=g783::streams.find(data);
                if(it!=g783::streams.end())
                {g783::inputOrigins=it->second;g783::streams.erase(it);g783::published.fetch_sub(1u,std::memory_order_relaxed);}
            }
            if(narrowDrives()){inputOrigins=nullptr;originIndex=0;vifDraw={};return;}
        }
        std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
        auto it=streams.find(data);
        inputOrigins=it==streams.end()?nullptr:it->second;
        if(it!=streams.end()) streams.erase(it);
        originIndex=0; vifDraw={};
    }
    ~InputScope() { inputOrigins.reset(); vifDraw={}; g783::inputOrigins.reset(); g783::vifNarrow={}; }
};
// The origins the native path reads: narrow when it drives, broad otherwise (incl. the diff).
inline const std::shared_ptr<std::vector<Origin>>& activeOrigins(){return narrowDrives()?g783::inputOrigins:inputOrigins;}
inline size_t activeOriginIndex(){return narrowDrives()?g783::originIndex:originIndex;}
inline Draw originAt(const std::shared_ptr<std::vector<Origin>>& origins,size_t& index,size_t pos)
{
    while(index<origins->size() && pos>=(*origins)[index].end) ++index;
    return index<origins->size() && pos>=(*origins)[index].begin ? (*origins)[index].draw : Draw{};
}
inline void command(size_t pos)
{
    if(narrowDrives())
    {
        // Shipping: only target chunks carry an origin, and vifDraw changes only at their
        // boundaries; between them a command costs one compare.
        if(!g783::inputOrigins){if(vifDraw.id)vifDraw={};return;}
        if(pos<g783::nextChange)return;
        dc2_g783::Scope g783(dc2_g783::VifCommand);
        const auto& origins=*g783::inputOrigins;
        vifDraw=originAt(g783::inputOrigins,g783::originIndex,pos);
        const size_t i=g783::originIndex;
        g783::nextChange=i>=origins.size()?~size_t(0):(pos>=origins[i].begin?origins[i].end:origins[i].begin);
        return;
    }
    if(!tracking() || !inputOrigins) { vifDraw={}; }
    else
    {
        dc2_g783::Scope g783(dc2_g783::VifCommand);
        vifDraw=originAt(inputOrigins,originIndex,pos);
    }
    if(!narrowOn())return;
    // DC2_G783_DIFF: broad drives native; the narrow answer must never claim a target the
    // broad attribution does not give the identical association.
    g783::vifNarrow=g783::inputOrigins?originAt(g783::inputOrigins,g783::originIndex,pos):Draw{};
    const bool broadTarget=vifDraw.id && vifDraw.self==kG783TargetSelf;
    if(g783IsTarget(g783::vifNarrow))
    {
        if(g783::sameAssociation(g783::vifNarrow,vifDraw))++g783::diff.agree;
        else if(!broadTarget)++g783::diff.narrowOnly;
        else ++g783::diff.mismatch;
    }
    else if(broadTarget && vifDraw.bucket==0xe91f00u && vifDraw.state && vifDraw.call && vifDraw.geometry)
        ++g783::diff.broadOnly;
}
inline bool isCaptured(uint64_t id)
{ std::lock_guard<dc2_g783::ProfMutex> lock(mutex); return captured.find(id)!=captured.end(); }
inline FILE* engineFile()
{
    static FILE* file=std::fopen("logs/g775/engine_capture.bin","wb");
    return file;
}
inline void blob(FILE* file,const uint8_t* ram,uint32_t address,size_t bytes)
{
    address &= 0x1FFFFFFFu;
    uint32_t header[2]={address,valid(address,bytes)?uint32_t(bytes):0u};
    std::fwrite(header,sizeof(header),1,file);
    if(header[1]) std::fwrite(ram+address,1,header[1],file);
}
// Lossless engine memory sections, kept independent of VU/GIF decoding. The replay
// tool resolves only layouts proven from the game's loader and packet builder.
inline void captureEngine(Draw draw,const uint8_t* ram,uint32_t world,uint32_t manager,uint32_t caller)
{
    if(shadow() && completeStateCapture())dependencyWindow.store(draw.sf>=firstFrame() && draw.sf<=firstFrame()+4,std::memory_order_release);
    if(!shadow() || draw.sf<firstFrame() || draw.sf>firstFrame()+4 || !valid(draw.self,0x120)) return;
    std::lock_guard<dc2_g783::ProfMutex> lock(mutex);
    static const uint32_t limit=[] { const char* p=std::getenv("DC2_G776_CAPTURE_DRAWS");return p?std::min(512u,uint32_t(std::strtoul(p,nullptr,0))):32u; }();
    if(captured.size()>=limit || captured.count(draw.id)) return;
    auto u32=[&](uint32_t p){uint32_t v=0;if(valid(p,4))std::memcpy(&v,ram+p,4);return v;};
    auto u16=[&](uint32_t p){uint16_t v=0;if(valid(p,2))std::memcpy(&v,ram+p,2);return v;};
    FILE* file=engineFile(); if(!file) return;
    const uint32_t header[8]={0x45353737u,uint32_t(draw.id),uint32_t(draw.id>>32),draw.sf,draw.self,draw.seam,caller,uint32_t(meshSources.count(draw.self)!=0)|(instanceContext()?2u:0u)|(loadedBuckets.count(draw.self)?4u:0u)};
    std::fwrite(header,sizeof(header),1,file);
    blob(file,ram,draw.self,0x120);
    blob(file,ram,world,64);
    blob(file,ram,manager,0x1100);
    for(uint32_t i=0;i<4;++i)
    {
        uint32_t count=u32(draw.self+0x20+i*4),address=u32(draw.self+0x30+i*4);
        const auto cache=meshSources.find(draw.self);
        if(cache!=meshSources.end() && cache->second.counts[i]==count)
        {
            const auto& array=cache->second.arrays[i];const uint32_t h[2]={address,uint32_t(array.size())};
            std::fwrite(h,sizeof(h),1,file);if(!array.empty())std::fwrite(array.data(),1,array.size(),file);
        }
        else blob(file,ram,address,count<=65536?size_t(count)*16:0);
    }
    uint32_t materials=u32(draw.self+0x40), materialArray=u32(draw.self+0x44);
    blob(file,ram,materialArray,materials<=256?size_t(materials)*0x30:0);
    uint32_t bucket=u32(draw.self+0x48);
    for(uint32_t b=0;b<256 && valid(bucket,0x20);++b)
    {
        blob(file,ram,bucket,0x20);
        const auto retained=loadedBuckets.find(draw.self);
        const auto* cached=retained==loadedBuckets.end()?nullptr:[&]()->const RetainedBucket*{const auto it=retained->second.find(bucket);return it==retained->second.end()?nullptr:&it->second;}();
        if(cached)
        {
            for(const auto& face:cached->faces)
            {
                const uint32_t fh[2]={face.address,uint32_t(face.header.size())};std::fwrite(fh,sizeof(fh),1,file);std::fwrite(face.header.data(),1,face.header.size(),file);
                uint32_t address;std::memcpy(&address,face.header.data()+12,4);
                const uint32_t ih[2]={address,uint32_t(face.indices.size())};std::fwrite(ih,sizeof(ih),1,file);if(!face.indices.empty())std::fwrite(face.indices.data(),1,face.indices.size(),file);
            }
            bucket=u32(bucket+8);continue;
        }
        uint32_t face=u32(bucket+4);
        for(uint32_t n=0;n<256 && valid(face,0x18);++n)
        {
            blob(file,ram,face,0x18);
            const auto mesh=faceSources.find(draw.self);
            const auto* indices=mesh==faceSources.end()?nullptr:[&]() -> const std::vector<uint8_t>* {const auto it=mesh->second.find(face);return it==mesh->second.end()?nullptr:&it->second;}();
            if(indices && indices->size()==size_t(u16(face+6))*4)
            {
                const uint32_t h[2]={u32(face+0x0c),uint32_t(indices->size())};
                std::fwrite(h,sizeof(h),1,file);if(!indices->empty())std::fwrite(indices->data(),1,indices->size(),file);
            }
            else blob(file,ram,u32(face+0x0c),size_t(u16(face+6))*4);
            face=u32(face+0x10);
        }
        bucket=u32(bucket+8);
    }
    for(uint32_t m=0;m<std::min(materials,256u);++m) blob(file,ram,u32(materialArray+m*0x30+0x20),0x100);
    if(instanceContext())
    {
        const uint32_t renderer=u32(manager+0x64)&0x1fffffffu;
        blob(file,ram,renderer,0x1100);
        blob(file,ram,u32(renderer+0xfcc)&0x1fffffffu,0x300);
        blob(file,ram,renderer+0x400+u32(renderer+0x3f4)*0x150,0x150);
    }
    const uint32_t end[2]={0,0}; std::fwrite(end,sizeof(end),1,file); std::fflush(file);
    captured.emplace(draw.id,true);
    std::fprintf(stderr,"[G775:engine-capture] draw=%llu sf=%u self=%x seam=%x\n",static_cast<unsigned long long>(draw.id),draw.sf,draw.self,draw.seam);
}
inline uint64_t hash(const uint8_t* p,size_t n)
{ uint64_t h=14695981039346656037ull;for(size_t i=0;i<n;++i)h=(h^p[i])*1099511628211ull;return h; }
inline void vuResult(Draw draw,uint64_t image,uint32_t entry,bool resume,uint64_t pairs,size_t kicks,size_t bytes)
{
    if(!enabled())return;
    struct Stats { uint64_t calls=0,pairs=0,kicks=0,bytes=0; uint32_t sf=0; };
    using Key=std::tuple<uint64_t,uint32_t,uint32_t,uint32_t,bool,uint32_t>;
    static std::map<Key,Stats> rows;
    static uint64_t calls=0;
    auto& row=rows[{image,draw.seam,draw.self,entry,resume,draw.id?0:draw.reason}];
    ++row.calls;row.pairs+=pairs;row.kicks+=kicks;row.bytes+=bytes;row.sf=draw.sf;
    ++calls;
    if(calls%32768)return;
    FILE* file=std::fopen("logs/g775/vu_draw_census.csv","wb");if(!file)return;
    std::fprintf(file,"image,seam,self,entry,resume,calls,pairs,xgkicks,gif_bytes,last_sf,unowned_reason\n");
    for(const auto& r:rows)std::fprintf(file,"%llx,%x,%x,%x,%u,%llu,%llu,%llu,%llu,%u,%u\n",static_cast<unsigned long long>(std::get<0>(r.first)),std::get<1>(r.first),std::get<2>(r.first),std::get<3>(r.first),unsigned(std::get<4>(r.first)),static_cast<unsigned long long>(r.second.calls),static_cast<unsigned long long>(r.second.pairs),static_cast<unsigned long long>(r.second.kicks),static_cast<unsigned long long>(r.second.bytes),r.second.sf,std::get<5>(r.first));
    std::fclose(file);
}
inline void fixtureLink(Draw draw,uint64_t record)
{
    if(!shadow())return;
    if(!isCaptured(draw.id)){if(!completeStateCapture())return;draw={};}
    static FILE* file=std::fopen("logs/g775/fixture_links.csv","wb");
    if(file){std::fprintf(file,"%llu,%llu,%u,%x\n",static_cast<unsigned long long>(record),static_cast<unsigned long long>(draw.id),draw.sf,draw.self);std::fflush(file);}
    static FILE* scope=std::fopen("logs/g775/bucket_links.csv","wb");
    if(scope){std::fprintf(scope,"%llu,%llu,%x,%x,%x\n",static_cast<unsigned long long>(record),static_cast<unsigned long long>(draw.id),draw.geometry,draw.listNode,draw.bucket);std::fflush(scope);}
}
}
#endif
