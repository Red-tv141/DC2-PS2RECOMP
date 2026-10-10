#pragma once
// Freeze actual loader packet placement and bucket/DMA order, not inferred order.
#include "runtime/dc2_g789_raster.h"
#if defined(PS2X_G775_DIAG) || defined(PS2X_G777_NATIVE_GL)
namespace dc2_g789_topology {
struct Span {uint32_t face=0,start=0,qwords=0;};
inline std::unordered_map<uint32_t,std::vector<Span>> spans;
inline void face(uint32_t visual,uint32_t index,uint32_t destination,uint32_t qwords,const uint8_t* ram){
    using namespace dc2_g789_oracle;
    if(!dc2_g789_raster::on() && !dc2_g789_live::requested())return;
    auto i=sources.find(visual);if(i==sources.end() ||
        (dc2_g789_raster::on()?i->second->generation!=40u:i->second->counts[0]!=395u))return;
    const uint32_t start=physical(destination);if(!qwords || qwords>65536u || !range(start,size_t(qwords)*16u)){
        std::fprintf(stderr,"[G789:topology] stage=face face=%u destination=%x qwords=%u refused=range\n",index,destination,qwords);return;}
    spans[visual].push_back({index,start,qwords});
    if(!dc2_g789_raster::on())return;
    FILE* f=file("facepacket_40_"+std::to_string(index)+".bin");
    const uint32_t h[]={0x47374650u,1u,40u,0u,visual,index,start,qwords};write(f,h,sizeof(h));write(f,ram+start,size_t(qwords)*16u);close(f);
}
inline void loaded(uint32_t visual,const uint8_t* ram){
    using namespace dc2_g789_oracle;
    auto i=sources.find(visual);if(i==sources.end() ||
        (dc2_g789_raster::on()?i->second->generation!=40u:!dc2_g789_live::requested() || !ordinarySource(*i->second)))return;
    FILE* f=nullptr;
    if(dc2_g789_raster::on()){
        FILE* memory=file("loader_ram_40.bin");write(memory,ram,0x2000000u);close(memory);
        f=file("loader_topology_40.bin");const uint32_t h[]={0x47375450u,2u,40u,0u,visual,uint32_t(spans[visual].size())};write(f,h,sizeof(h));
    }
    const auto refuse=[&](uint32_t bucket,uint32_t at,unsigned reason){
        if(f){const uint32_t row[]={3u,bucket,at,reason};write(f,row,sizeof(row));close(f);
        std::fprintf(stderr,"[G789:topology] source=40 visual=%x bucket=%x at=%x refused=%u nativeAdmission=0\n",visual,bucket,at,reason);}
    };
    if(f)for(const auto& s:spans[visual]){const uint32_t row[]={s.face,s.start,s.qwords};write(f,row,sizeof(row));}
    auto frozen=std::make_shared<dc2_g789_raster::Topology>();frozen->source=i->second->generation;
    const auto& source=*i->second;std::unordered_map<uint32_t,bool> bucketSeen,nodeSeen,faceSeen;
    for(uint32_t bucket=physical(word(ram,visual+0x48u));bucket;bucket=physical(word(ram,bucket+8u))){
        if(!range(bucket,32u) || !bucketSeen.emplace(bucket,true).second || bucketSeen.size()>256u){refuse(bucket,bucket,1u);return;}
        if(f){const uint32_t marker=1u;write(f,&marker,4);write(f,&bucket,4);write(f,ram+bucket,32u);}
        // The live +0x10 geometry packet is not built yet. Cold +4 owns the
        // actual ordered mgCFace nodes; +0x20/+0x24 is their exact DMA REF.
        const uint32_t material=word(ram,bucket);
        for(uint32_t at=physical(word(ram,bucket+4u));at;at=physical(word(ram,at+0x10u))){
            if(!range(at,48u) || !nodeSeen.emplace(at,true).second || nodeSeen.size()>source.faces.size()){refuse(bucket,at,5u);return;}
            if(f){const uint32_t row[]={4u,at};write(f,row,sizeof(row));write(f,ram+at,48u);}
            const Span* selected=nullptr;const uint32_t payload=physical(word(ram,at+0x24u));
            for(const auto& span:spans[visual])if(span.start==payload){if(selected){refuse(bucket,at,6u);return;}selected=&span;}
            if(!selected || word(ram,at+0x20u)!=(0x30000000u|selected->qwords) || selected->face>=source.faces.size() || !faceSeen.emplace(selected->face,true).second){refuse(bucket,at,7u);return;}
            const auto& face=source.faces[selected->face];const uint32_t kind=face.header[0]&7u,count=face.header[2]&65535u;
            if(std::memcmp(face.header.data(),ram+at,16u)!=0 || (face.header[0]&0x310u) || (face.header[0]>>16u)!=3u || (face.header[1]&65535u)!=material || (kind!=3u && kind!=4u) || (kind==3u && count%3u) || (kind==4u && (count<3u || count>26u))){refuse(bucket,at,8u);return;}
            for(uint32_t offset=0;offset<count;offset+=kind==3u?27u:count){
                dc2_g789_raster::TopologyPacket p;p.face=selected->face;p.offset=offset;p.ordinal=uint32_t(frozen->packets.size()+1u);p.bucket=frozen->buckets;p.material=material;p.kind=kind;
                const uint32_t size=std::min(count-offset,kind==3u?27u:count);
                for(uint32_t c=0;c<size;++c){const uint8_t* tuple=face.indices.data()+size_t(offset+c)*12u;dc2_g789_raster::Corner corner{dc2_g789_raster::u32(tuple),dc2_g789_raster::u32(tuple+4u),dc2_g789_raster::u32(tuple+8u)};
                    if(corner.position>=source.counts[0] || corner.normal>=source.counts[1] || corner.st>=source.counts[3]){refuse(bucket,at,9u);return;}p.corners.push_back(corner);}
                frozen->triangles+=kind==3u?size/3u:size-2u;frozen->packets.push_back(std::move(p));
            }
        }
        ++frozen->buckets;
    }
    if(f){const uint32_t end=0u;write(f,&end,4);close(f);}
    if(faceSeen.size()!=115u || frozen->packets.size()!=131u || frozen->triangles!=652u){if(dc2_g789_raster::on())std::fprintf(stderr,"[G789:topology] source=40 refused=incomplete nativeAdmission=0\n");return;}
    dc2_g789_raster::publishTopology(frozen);
    if(dc2_g789_raster::on())std::fprintf(stderr,"[G789:topology] source=40 visual=%x faces=%zu buckets=%zu packets=%zu triangles=%u loaderOwned=1\n",visual,faceSeen.size(),bucketSeen.size(),frozen->packets.size(),frozen->triangles);
}
}
#endif
