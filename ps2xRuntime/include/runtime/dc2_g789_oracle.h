#pragma once
// G789 diagnostics only: independently frozen engine source and post-replay VU-owner snapshots.
#include "runtime/dc2_g789_census.h"
#include "runtime/ps2_vu1.h"
#include <string>
#include <cmath>
#include <algorithm>
extern void dc2_g541_vu_pipe_snapshot(std::vector<uint8_t>&);

namespace dc2_g789_oracle {
using dc2_g789::Identity;
using dc2_g789::word;
inline bool on(){static const bool v=[] {const char* e=std::getenv("DC2_G789_ORACLE");return e && std::strcmp(e,"0");}();return v && dc2_g789::on();}
inline bool sourceOn(){return on() || dc2_g789_live::requested();}
inline uint32_t option(const char* key,uint32_t fallback){const char* e=std::getenv(key);return e?uint32_t(std::strtoul(e,nullptr,0)):fallback;}
inline bool range(uint32_t address,size_t bytes){return address>=0x10000u && uint64_t(address)+bytes<=0x2000000u;}
inline uint32_t physical(uint32_t address){return address&0x1fffffffu;}
inline uint16_t half(const uint8_t* ram,uint32_t address){uint16_t v=0;if(range(address,2))std::memcpy(&v,ram+address,2);return v;}
inline bool copy(std::vector<uint8_t>& out,const uint8_t* ram,uint32_t address,size_t bytes){if(bytes && !range(address,bytes))return false;out.resize(bytes);if(bytes)std::memcpy(out.data(),ram+address,bytes);return true;}
inline FILE* file(const std::string& leaf){const char* base=std::getenv("DC2_G789_ORACLE_OUT");if(!base){std::fprintf(stderr,"[G789:oracle] FATAL output directory missing\n");std::abort();}FILE* f=std::fopen((std::string(base)+"/"+leaf).c_str(),"wb");if(!f){std::fprintf(stderr,"[G789:oracle] FATAL file open failed\n");std::abort();}return f;}
inline void write(FILE* f,const void* data,size_t bytes){if(bytes && std::fwrite(data,1,bytes,f)!=bytes){std::fprintf(stderr,"[G789:oracle] FATAL short write\n");std::abort();}}
inline void block(FILE* f,const std::vector<uint8_t>& data){uint32_t n=uint32_t(data.size());write(f,&n,4);write(f,data.data(),data.size());}
inline void close(FILE* f){if(std::fclose(f)){std::fprintf(stderr,"[G789:oracle] FATAL file close failed\n");std::abort();}}
struct Face {std::array<uint32_t,4> header{};std::vector<uint8_t> indices;};
struct Source {
    uint64_t generation=0;uint32_t visual=0,mdt=0,motion=0;
    uint32_t counts[4]{},addresses[4]{},weightAddress=0,weightCount=0,materialAddress=0,materialCount=0;
    std::vector<uint8_t> vectors[4],weights,materials;std::vector<uint32_t> dictionary;
    std::vector<Face> faces;bool valid=false,sealed=false;
    uint64_t contract=0;
};
inline uint64_t sourceContract(const Source& source){
    uint64_t hash=14695981039346656037ull;
    const auto bytes=[&](const void* data,size_t size){const auto* p=static_cast<const uint8_t*>(data);for(size_t i=0;i<size;++i)hash=(hash^p[i])*1099511628211ull;};
    for(unsigned i:{0u,1u,3u})bytes(source.vectors[i].data(),source.vectors[i].size());
    bytes(source.weights.data(),source.weights.size());bytes(source.dictionary.data(),source.dictionary.size()*4u);
    for(const auto& face:source.faces){const uint32_t semantic[]={face.header[0],face.header[1]&65535u,face.header[2]&65535u};bytes(semantic,sizeof(semantic));bytes(face.indices.data(),face.indices.size());}
    return hash;
}
inline bool ordinarySource(const Source& source){
    return source.sealed && source.counts[0]==395u && source.counts[1]==395u &&
           source.counts[2]==0u && source.counts[3]==234u && source.dictionary.size()==21u &&
           source.faces.size()==115u && source.contract==0xdf1e0093fe4e1764ull;
}
inline uint64_t sourceGeneration=0;
inline uint64_t instanceGeneration=0;
inline std::unordered_map<uint32_t,std::shared_ptr<Source>> sources;
inline std::unordered_map<uint32_t,uint64_t> instances;
// Preserve the first rejected stream state; never silently replace frozen source.
inline void mutation(const Source& s,uint32_t visual,const uint8_t* ram,uint32_t phase){
    if(!on())return;
    static std::unordered_map<uint64_t,bool> captured;
    const uint64_t key=(uint64_t(visual)<<32)|(uint64_t(phase)<<24)|s.generation;
    if(captured.count(key))return;captured[key]=true;
    FILE* f=file("mutation_"+std::to_string(s.generation)+"_"+std::to_string(visual)+"_"+std::to_string(phase)+".bin");
    const uint32_t header[]={0x47374d55u,1u,phase,uint32_t(s.generation),uint32_t(s.generation>>32),dc2_g789::frame(),visual};
    write(f,header,sizeof(header));write(f,ram+visual,0x110u);
    for(uint32_t i=0;i<4u;++i){std::vector<uint8_t> current;const uint32_t count=word(ram,visual+0x20u+i*4u),address=physical(word(ram,visual+0x30u+i*4u));
        if(count<=65536u)copy(current,ram,address,size_t(count)*16u);block(f,current);
        if(current!=s.vectors[i]){size_t at=0;while(at<current.size() && at<s.vectors[i].size() && current[at]==s.vectors[i][at])++at;std::fprintf(stderr,"[G789:source_mutation] phase=%u frame=%u visual=%x mesh=%llu stream=%u byte=%zu oldCount=%u count=%u oldAddress=%x address=%x\n",phase,dc2_g789::frame(),visual,(unsigned long long)s.generation,i,at,s.counts[i],count,s.addresses[i],address);}}
    std::vector<uint8_t> current;const uint32_t count=word(ram,visual+0x100u),address=physical(word(ram,visual+0x104u));
    if(count<=65536u)copy(current,ram,address,size_t(count)*32u);block(f,current);
    if(current!=s.weights)std::fprintf(stderr,"[G789:source_mutation] phase=%u frame=%u visual=%x mesh=%llu weights=1 oldAddress=%x address=%x\n",phase,dc2_g789::frame(),visual,(unsigned long long)s.generation,s.weightAddress,address);
    close(f);
}
inline std::atomic<uint64_t> sourceRefusals{0},poseRefusals{0},sourceMutations{0},borrowedStreamChanges{0},palettes{0},ownerChecks{0},ownerMismatches{0},witnesses{0};
inline std::atomic<uint64_t> setupChecks{0},setupMismatches{0},geometryChecks{0},geometryMismatches{0};
inline void beginLoad(uint32_t visual,uint32_t mdt,uint32_t motion){if(!sourceOn())return;auto s=std::make_shared<Source>();s->generation=++sourceGeneration;s->visual=visual;s->mdt=mdt;s->motion=motion;sources[visual]=s;instances[visual]=++instanceGeneration;}
inline void clone(uint32_t source,uint32_t destination,const uint8_t* ram){
    if(!sourceOn() || !destination)return;
    auto it=sources.find(source);if(it==sources.end() || !it->second->sealed || !range(destination,0x110u)){++sourceRefusals;return;}
    const auto& s=*it->second;bool exact=true;
    for(uint32_t i=0;i<4u;++i)exact=exact && word(ram,destination+0x20u+i*4u)==s.counts[i] && physical(word(ram,destination+0x30u+i*4u))==s.addresses[i];
    exact=exact && word(ram,destination+0x100u)==s.weightCount && physical(word(ram,destination+0x104u))==s.weightAddress;
    for(uint32_t i=0;i<s.dictionary.size();++i)exact=exact && word(ram,destination+0x80u+i*4u)==s.dictionary[i];
    if(!exact){++sourceRefusals;return;}
    sources[destination]=it->second;instances[destination]=++instanceGeneration;
    if(!on())return;
    static FILE* f=file("instances.jsonl");
    std::fprintf(f,"{\"sourceVisual\":%u,\"visual\":%u,\"meshGeneration\":%llu,\"instanceGeneration\":%llu,\"sourceInstance\":%llu,\"frame\":%u}\n",source,destination,(unsigned long long)s.generation,(unsigned long long)instances[destination],(unsigned long long)instances[source],dc2_g789::frame());std::fflush(f);
}
inline void face(uint32_t visual,uint32_t address,const uint8_t* ram){
    if(!sourceOn())return;
    auto it=sources.find(visual);if(it==sources.end()){++sourceRefusals;return;}auto& s=*it->second;
    if(s.sealed || !range(address,16)){s.valid=false;++sourceRefusals;return;}
    if(s.faces.empty()){
        if(!on() && (word(ram,visual+0x20u)!=395u || word(ram,visual+0x24u)!=395u ||
                     word(ram,visual+0x28u)!=0u || word(ram,visual+0x2cu)!=234u))return;
        bool ok=true;
        s.dictionary.clear();
        for(unsigned i=0;i<4;++i){s.counts[i]=word(ram,visual+0x20u+i*4u);s.addresses[i]=physical(word(ram,visual+0x30u+i*4u));ok=ok && s.counts[i]<=65536u && copy(s.vectors[i],ram,s.addresses[i],size_t(s.counts[i])*16u);}
        s.weightAddress=physical(word(ram,visual+0x104u));s.weightCount=word(ram,visual+0x100u);
        s.materialCount=word(ram,visual+0x40u);s.materialAddress=physical(word(ram,visual+0x44u));
        ok=ok && s.weightCount==s.counts[0] && copy(s.weights,ram,s.weightAddress,size_t(s.weightCount)*32u);
        ok=ok && s.materialCount<=1024u && copy(s.materials,ram,s.materialAddress,size_t(s.materialCount)*48u);
        for(uint32_t b=0;b<32u;++b){uint32_t index=word(ram,visual+0x80u+b*4u);if(int32_t(index)<0)break;s.dictionary.push_back(index);}
        s.valid=ok && !s.dictionary.empty();if(!s.valid){++sourceRefusals;return;}
    }
    const uint16_t flags=half(ram,address);const uint32_t variant=((flags&0x100u)?1u:0u)+((flags&0x10u)?2u:0u)+((flags&0x200u)?4u:0u);
    static constexpr uint32_t widths[8]={3,4,2,3,2,3,1,2};
    const uint32_t width=half(ram,address+2u);const int32_t corners=int16_t(half(ram,address+8u));
    if(!s.valid || width!=widths[variant] || corners<=0 || s.faces.size()>=256u){s.valid=false;++sourceRefusals;return;}
    Face f;std::memcpy(f.header.data(),ram+address,16);
    if(!copy(f.indices,ram,physical(word(ram,address+12u)),size_t(corners)*width*4u)){s.valid=false;++sourceRefusals;return;}
    s.faces.push_back(std::move(f));
}
inline void finishLoad(uint32_t visual){
    if(!sourceOn())return;auto it=sources.find(visual);if(it==sources.end())return;auto& s=*it->second;
    s.sealed=s.valid && !s.faces.empty();if(!s.sealed){++sourceRefusals;return;}
    s.contract=sourceContract(s);
    if(!on())return;
    FILE* f=file("source_"+std::to_string(s.generation)+".bin");
    const uint32_t header[]={0x47373839u,1u,uint32_t(s.generation),uint32_t(s.generation>>32),s.visual,s.mdt,s.motion,
        s.counts[0],s.counts[1],s.counts[2],s.counts[3],s.weightCount,s.materialCount,uint32_t(s.dictionary.size()),uint32_t(s.faces.size())};
    write(f,header,sizeof(header));for(const auto& v:s.vectors)block(f,v);block(f,s.weights);block(f,s.materials);
    write(f,s.dictionary.data(),s.dictionary.size()*4u);
    for(const auto& face:s.faces){write(f,face.header.data(),16);block(f,face.indices);}close(f);
}
struct Pose {
    Identity identity;std::shared_ptr<const Source> source;uint32_t frameArray=0,bindArray=0,bindSelector=0,rootFrame=0;
    uint64_t instance=0;
    uint64_t generation=0;
    std::vector<uint8_t> palette,objectMatrix,renderInfo,bindMatrices,materials,rootBounds,visualBounds,config;std::vector<uint32_t> boneFrames;
};
inline std::mutex poseMutex;
inline std::unordered_map<uint64_t,std::shared_ptr<const Pose>> poses;
inline uint32_t selectedVisual=0;
inline uint64_t poseGeneration=0;
inline std::array<std::shared_ptr<Pose>,32> productionPoses;
inline std::shared_ptr<Pose> acquireProductionPose(){
    for(auto& p:productionPoses)if(!p){
        p=std::make_shared<Pose>();p->palette.reserve(1344);p->objectMatrix.reserve(64);
        p->renderInfo.reserve(0x1020);p->bindMatrices.reserve(1408);p->materials.reserve(432);
        p->rootBounds.reserve(32);p->visualBounds.reserve(32);p->config.reserve(0x90);p->boneFrames.reserve(21);
    }
    for(auto& p:productionPoses)if(p.use_count()==1){
        p->identity={};p->source.reset();p->palette.clear();p->objectMatrix.clear();p->renderInfo.clear();
        p->bindMatrices.clear();p->materials.clear();p->rootBounds.clear();p->visualBounds.clear();p->config.clear();p->boneFrames.clear();return p;
    }
    return {};
}
inline std::shared_ptr<const Pose> findPose(uint64_t draw){
    std::lock_guard<std::mutex> lock(poseMutex);
    if(on()){auto i=poses.find(draw);if(i!=poses.end())return i->second;}
    for(const auto& p:productionPoses)if(p && p->identity.draw==draw)return p;
    return {};
}
inline void palette(const Identity& identity,uint32_t destination,uint32_t matrix,uint32_t info,uint32_t result,const uint8_t* ram,const uint8_t* scratch){
    if(!sourceOn() || !identity.draw || identity.mesh!=dc2_g789::Motion)return;
    const uint32_t first=option("DC2_G789_ORACLE_FIRST",1800),last=option("DC2_G789_ORACLE_LAST",1831);
    const bool capture=on() && identity.frame>=first && identity.frame<=last;
    // Diagnostic capture bounds must not disable normal live publication/warming.
    if(!capture && !dc2_g789_live::requested())return;
    const uint32_t visual=identity.visual;
    auto it=sources.find(visual);if(it==sources.end() || !it->second->sealed){++poseRefusals;return;}const auto& s=*it->second;
    if(!capture && (identity.pass!=dc2_g789::Silhouette || !ordinarySource(s)))return;
    if(s.counts[0]!=option("DC2_G789_ORACLE_POSITIONS",395) || s.dictionary.size()!=option("DC2_G789_ORACLE_PALETTE",21))return;
    if(!selectedVisual)selectedVisual=visual;if(visual!=selectedVisual)return;
    bool stable=true,borrowedChanged=false;
    for(unsigned i=0;i<4;++i){stable=stable && word(ram,visual+0x20u+i*4u)==s.counts[i] && physical(word(ram,visual+0x30u+i*4u))==s.addresses[i];
        borrowedChanged=borrowedChanged || (!s.vectors[i].empty() && std::memcmp(s.vectors[i].data(),ram+s.addresses[i],s.vectors[i].size())!=0);}
    stable=stable && word(ram,visual+0x100u)==s.weightCount && physical(word(ram,visual+0x104u))==s.weightAddress && std::memcmp(s.weights.data(),ram+s.weightAddress,s.weights.size())==0;
    for(uint32_t b=0;b<s.dictionary.size();++b)stable=stable && word(ram,visual+0x80u+b*4u)==s.dictionary[b];
    if(!stable){++sourceMutations;mutation(s,visual,ram,2u);return;}
    // The original loader's attribute storage is borrowed, while both the frozen
    // source and legacy VIF packets retain their own copies. Record this change;
    // it grants no native admission. Independent worker input/output proof follows.
    if(borrowedChanged){++borrowedStreamChanges;mutation(s,visual,ram,3u);}
    const uint32_t count=uint32_t(s.dictionary.size());if(result!=1u+count*4u){++poseRefusals;return;}
    auto p=capture?std::make_shared<Pose>():acquireProductionPose();if(!p){++poseRefusals;return;}p->identity=identity;p->source=it->second;p->generation=++poseGeneration;
    p->instance=instances[visual];
    p->frameArray=physical(word(ram,visual+0x50u));p->bindArray=physical(word(ram,visual+0x58u));p->bindSelector=word(ram,visual+0x54u);
    const size_t paletteBytes=size_t(count)*64u;bool paletteCopied=false;
    if((destination&0xffffc000u)==0x70000000u){
        const size_t offset=(destination&0x3fffu)+16u;
        if(scratch && offset+paletteBytes<=0x4000u){p->palette.assign(scratch+offset,scratch+offset+paletteBytes);paletteCopied=true;}
    }else paletteCopied=copy(p->palette,ram,physical(destination)+16u,paletteBytes);
    if(!paletteCopied || !copy(p->objectMatrix,ram,matrix,64u) || !copy(p->renderInfo,ram,info,0x1020u)){++poseRefusals;return;}
    std::vector<uint32_t> indices{p->bindSelector};indices.insert(indices.end(),s.dictionary.begin(),s.dictionary.end());
    for(uint32_t index:indices){const uint64_t at=uint64_t(p->bindArray)+uint64_t(index)*64u;if(index>65535u || at+64u>0x2000000u || !range(uint32_t(at),64)){++poseRefusals;return;}p->bindMatrices.insert(p->bindMatrices.end(),ram+at,ram+at+64u);}
    for(uint32_t index:s.dictionary){const uint64_t at=uint64_t(p->frameArray)+uint64_t(index)*4u;if(at+4u>0x2000000u || !range(uint32_t(at),4u)){++poseRefusals;return;}const uint32_t bone=physical(word(ram,uint32_t(at)));if(!range(bone,0x100u)){++poseRefusals;return;}p->boneFrames.push_back(bone);}
    p->rootFrame=physical(word(ram,p->frameArray));
    const uint32_t materialCount=word(ram,visual+0x40u);
    if(materialCount!=s.materialCount || !copy(p->materials,ram,physical(word(ram,visual+0x44u)),size_t(materialCount)*48u) || !copy(p->visualBounds,ram,visual+0x60u,32u)){++poseRefusals;return;}
    if(identity.root && !copy(p->rootBounds,ram,identity.root+0xf0u,32u)){++poseRefusals;return;}
    if(!copy(p->config,ram,physical(word(ram,info+0xfccu)),0x90u)){++poseRefusals;return;}
    if(!capture)return;
    {std::lock_guard<std::mutex> lock(poseMutex);if(poses.count(identity.draw)){++poseRefusals;return;}poses[identity.draw]=p;}
    const uint32_t header[]={0x4737504fu,3u,identity.frame,identity.sf,uint32_t(identity.draw),uint32_t(identity.draw>>32),identity.object,identity.root,visual,identity.pass,
        uint32_t(s.generation),uint32_t(s.generation>>32),count,p->frameArray,p->bindArray,p->bindSelector,uint32_t(p->instance),uint32_t(p->instance>>32),p->rootFrame};
    FILE* f=file("pose_"+std::to_string(identity.draw)+".bin");write(f,header,sizeof(header));block(f,p->palette);block(f,p->objectMatrix);block(f,p->renderInfo);block(f,p->bindMatrices);write(f,p->boneFrames.data(),p->boneFrames.size()*4u);block(f,p->materials);block(f,p->rootBounds);block(f,p->visualBounds);block(f,p->config);close(f);
    ++palettes;
}
inline void report(uint32_t frame){
    if(!on())return;static uint32_t last=~0u;if(frame%60u || frame==last)return;last=frame;
    std::fprintf(stderr,"[G789:oracle_state] frame=%u sources=%zu selected=%x palettes=%llu sourceRefused=%llu poseRefused=%llu sourceMutation=%llu borrowedStreamChanged=%llu ownerChecks=%llu ownerMismatch=%llu setupChecks=%llu setupMismatch=%llu geometryChecks=%llu geometryMismatch=%llu\n",frame,sources.size(),selectedVisual,(unsigned long long)palettes.load(),(unsigned long long)sourceRefusals.load(),(unsigned long long)poseRefusals.load(),(unsigned long long)sourceMutations.load(),(unsigned long long)borrowedStreamChanges.load(),(unsigned long long)ownerChecks.load(),(unsigned long long)ownerMismatches.load(),(unsigned long long)setupChecks.load(),(unsigned long long)setupMismatches.load(),(unsigned long long)geometryChecks.load(),(unsigned long long)geometryMismatches.load());
}
struct Witness {
    std::shared_ptr<const Pose> pose;uint32_t ordinal=0,start=0,resume=0,top=0,itop=0,stage=0,budget=0;
    std::vector<uint8_t> code,data,state,pipeline;bool paletteMatch=false;
};
inline std::shared_ptr<Witness> before(const Identity& identity,const uint8_t* code,size_t codeBytes,const uint8_t* data,size_t dataBytes,uint32_t start,bool resume,uint32_t top,uint32_t itop,const VU1State* state,uint32_t budget){
    if(!on() || (identity.stage!=dc2_g789::Geometry && identity.stage!=dc2_g789::Setup) || identity.mesh!=dc2_g789::Motion)return {};
    std::shared_ptr<const Pose> p;{std::lock_guard<std::mutex> lock(poseMutex);auto it=poses.find(identity.draw);if(it==poses.end())return {};p=it->second;}
    ++ownerChecks;
    const bool match=dataBytes>=0x3c0u+p->palette.size() && std::memcmp(data+0x3c0u,p->palette.data(),p->palette.size())==0;
    if(!match)++ownerMismatches;
    if(identity.stage==dc2_g789::Setup){++setupChecks;if(!match)++setupMismatches;}
    else {++geometryChecks;if(!match)++geometryMismatches;}
    static std::unordered_map<uint64_t,uint32_t> kickCounts;
    const uint32_t ordinal=++kickCounts[identity.draw*4u+identity.stage];
    if(ordinal>option("DC2_G789_ORACLE_KICKS",4))return {};
    auto w=std::make_shared<Witness>();w->pose=p;w->ordinal=ordinal;w->start=start;w->resume=resume;w->top=top;w->itop=itop;w->stage=identity.stage;w->budget=budget;w->paletteMatch=match;
    w->code.assign(code,code+codeBytes);w->data.assign(data,data+dataBytes);
    if(!state){std::fprintf(stderr,"[G789:oracle] FATAL owner state missing\n");std::abort();}
    const auto* stateBytes=reinterpret_cast<const uint8_t*>(state);w->state.assign(stateBytes,stateBytes+sizeof(VU1State));dc2_g541_vu_pipe_snapshot(w->pipeline);return w;
}
inline void after(const std::shared_ptr<Witness>& w,const uint8_t* data,size_t bytes,const std::vector<std::vector<uint8_t>>& packets,const std::vector<uint32_t>& addresses){
    if(!w)return;const auto& d=w->pose->identity;
    const uint32_t header[]={0x47375655u,2u,d.frame,d.sf,uint32_t(d.draw),uint32_t(d.draw>>32),d.object,d.visual,d.pass,w->stage,w->ordinal,w->start,w->resume,w->top,w->itop,w->paletteMatch?1u:0u,uint32_t(packets.size()),uint32_t(addresses.size()),w->budget};
    FILE* f=file("vu_"+std::to_string(d.draw)+"_"+std::to_string(w->stage)+"_"+std::to_string(w->ordinal)+".bin");write(f,header,sizeof(header));block(f,w->code);block(f,w->data);uint32_t size=uint32_t(bytes);write(f,&size,4);write(f,data,bytes);
    for(const auto& packet:packets)block(f,packet);write(f,addresses.data(),addresses.size()*4u);block(f,w->state);block(f,w->pipeline);close(f);
    const uint64_t n=++witnesses;if(n==1u || n%64u==0u)std::fprintf(stderr,"[G789:oracle] snapshots=%llu palettes=%llu ownerChecks=%llu ownerMismatch=%llu setupChecks=%llu setupMismatch=%llu geometryChecks=%llu geometryMismatch=%llu sourceRefused=%llu poseRefused=%llu sourceMutation=%llu visual=%x frame=%u\n",(unsigned long long)n,(unsigned long long)palettes.load(),(unsigned long long)ownerChecks.load(),(unsigned long long)ownerMismatches.load(),(unsigned long long)setupChecks.load(),(unsigned long long)setupMismatches.load(),(unsigned long long)geometryChecks.load(),(unsigned long long)geometryMismatches.load(),(unsigned long long)sourceRefusals.load(),(unsigned long long)poseRefusals.load(),(unsigned long long)sourceMutations.load(),d.visual,d.frame);
}
} // namespace dc2_g789_oracle
