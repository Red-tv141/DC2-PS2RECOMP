#pragma once
// Rectangle evidence is independent of Motion VIF/VU/Path1 geometry.
#include "runtime/dc2_g789_raster.h"
#if defined(PS2X_G775_DIAG) || defined(PS2X_G777_NATIVE_GL)
namespace dc2_g789_rect {
struct Sample {
    uint64_t id=0,parent=0;
    dc2_g789::Identity identity;
    uint32_t frame=0,destination=0,flags=0,billboard=0,width=0,height=0;
    std::vector<uint8_t> raw,bounds,info,seeds,children;
    bool finished=false;
};
inline uint64_t next=0;
inline thread_local std::map<std::pair<uintptr_t,uint32_t>,std::shared_ptr<Sample>> stackNodes;
inline std::shared_ptr<Sample> begin(const dc2_g789::Identity& identity,uint32_t frame,uint32_t destination,uint32_t manager,uint32_t gp,uint64_t parent,const uint8_t* ram){
    using namespace dc2_g789_oracle;
    if(!dc2_g789_raster::on() || identity.frame<option("DC2_G789_ORACLE_FIRST",1800) || identity.frame>option("DC2_G789_ORACLE_LAST",1831))return {};
    auto s=std::make_shared<Sample>();s->id=++next;s->parent=parent;s->identity=identity;s->frame=physical(frame);s->destination=physical(destination);
    if(!manager)manager=0x3820e0u;manager=physical(manager);
    if(!copy(s->raw,ram,s->frame,0x110u) || !copy(s->info,ram,physical(word(ram,manager+0x64u)),0x1020u) || !copy(s->seeds,ram,0x380e30u,32u))return {};
    const uint32_t display=physical(word(ram,s->frame+0xf4u));s->flags=word(ram,(display?display:0x380da0u)+0x18u);s->billboard=display?word(ram,display+0x88u):0u;
    s->width=word(ram,gp-0x7880u);s->height=word(ram,gp-0x787cu);
    const uint32_t bounds=physical(word(ram,s->frame+0xf0u));if(bounds && !copy(s->bounds,ram,bounds,128u))return {};
    std::unordered_map<uint32_t,bool> seen;
    for(uint32_t child=physical(word(ram,s->frame+0x58u));child;child=physical(word(ram,child+0x5cu))){
        if(!range(child,0x110u) || !seen.emplace(child,true).second || seen.size()>1024u)return {};
        const uint32_t d=physical(word(ram,child+0xf4u));const uint32_t row[]={child,d,d?word(ram,d+0x18u):0u};
        const auto* p=reinterpret_cast<const uint8_t*>(row);s->children.insert(s->children.end(),p,p+sizeof(row));
    }
    return s;
}
inline void finish(Sample& s,uint32_t entrySp,uint32_t result,const uint8_t* ram){
    if(s.finished)return;s.finished=true;
    using namespace dc2_g789_oracle;
    std::vector<uint8_t> lw,projected,local,output;
    if(!copy(lw,ram,entrySp-0xc0u,64u) || !copy(projected,ram,entrySp-0x60u,64u) || !copy(local,ram,entrySp-0x80u,32u) || !copy(output,ram,s.destination,32u)){std::fprintf(stderr,"[G789:rect] FATAL capture address fault\n");std::abort();}
    const uint32_t h[]={0x47375242u,1u,s.identity.frame,s.identity.sf,uint32_t(s.id),uint32_t(s.id>>32),uint32_t(s.parent),uint32_t(s.parent>>32),s.identity.object,s.identity.root,s.frame,s.destination,s.flags,result,s.width,s.height,s.billboard};
    FILE* f=file("rect_"+std::to_string(s.id)+".bin");write(f,h,sizeof(h));
    const std::array<const std::vector<uint8_t>*,9> blocks={&s.raw,&s.bounds,&s.info,&lw,&projected,&local,&output,&s.seeds,&s.children};
    for(const auto* v:blocks)block(f,*v);close(f);
    std::fprintf(stderr,"[G789:rect] frame=%u object=%x root=%x frameNode=%x id=%llu parent=%llu flags=%x billboard=%u result=%u children=%zu geometryIndependent=1\n",s.identity.frame,s.identity.object,s.identity.root,s.frame,(unsigned long long)s.id,(unsigned long long)s.parent,s.flags,s.billboard,result,s.children.size()/12u);
}
inline void root(void* context,uint32_t entrySp,const std::shared_ptr<Sample>& sample){if(sample)stackNodes[{reinterpret_cast<uintptr_t>(context),entrySp-0x120u}]=sample;}
inline void lw(void* context,uint32_t bodySp,uint32_t frame,uint32_t gp,const uint8_t* ram){
    const auto key=std::make_pair(reinterpret_cast<uintptr_t>(context),bodySp);
    if(stackNodes.count(key))return;
    const auto parent=stackNodes.find({key.first,bodySp+0x120u});if(parent==stackNodes.end())return;
    auto sample=begin(parent->second->identity,frame,bodySp+0x220u,0u,gp,parent->second->id,ram);
    if(sample)stackNodes[key]=sample;
}
inline void childReturned(void* context,uint32_t parentBodySp,uint32_t result,const uint8_t* ram){
    const auto key=std::make_pair(reinterpret_cast<uintptr_t>(context),parentBodySp-0x120u);
    auto i=stackNodes.find(key);if(i==stackNodes.end())return;finish(*i->second,parentBodySp,result,ram);stackNodes.erase(i);
}
inline void rootReturned(void* context,uint32_t entrySp){stackNodes.erase({reinterpret_cast<uintptr_t>(context),entrySp-0x120u});}
}
#endif
