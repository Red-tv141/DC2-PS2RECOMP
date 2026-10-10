#pragma once
// Production publication reuses the loader/source/pose and constant authorities.
// It never consumes a VU output vertex or a diagnostic correspondence registry.
#include "runtime/dc2_g789_topology.h"
namespace dc2_g789_publication {
inline std::unordered_map<uint64_t,std::shared_ptr<const dc2_g789_live::Source>> sources;
inline uint64_t hash(const void* data,size_t bytes){uint64_t h=14695981039346656037ull;const auto* p=static_cast<const uint8_t*>(data);for(size_t i=0;i<bytes;++i)h=(h^p[i])*1099511628211ull;return h;}
inline bool commands(const dc2_g789_topology::Span& span,const uint8_t* ram,std::vector<std::pair<uint32_t,uint32_t>>& kicks){
    const size_t bytes=size_t(span.qwords)*16u;size_t at=0;
    while(at+4u<=bytes){
        const uint32_t command=dc2_g789::word(ram,span.start+uint32_t(at));const uint32_t code=(command>>24u)&127u;
        const uint32_t address=span.start+uint32_t(at);at+=4u;
        if(code==0x6cu){at+=size_t(((command>>16u)&255u)?((command>>16u)&255u):256u)*16u;if(at>bytes)return false;}
        else if(code==0x14u || code==0x17u){if(code==0x14u && (command&65535u)!=2u)return false;kicks.emplace_back(address,command);}
        else if(code!=0u && code!=1u && code!=2u && code!=3u && code!=4u && code!=5u && code!=6u && code!=7u && code!=0x10u && code!=0x11u && code!=0x13u)return false;
    }
    return at==bytes;
}
inline std::shared_ptr<const dc2_g789_live::Source> source(const dc2_g789_oracle::Source& original,const uint8_t* ram){
    using namespace dc2_g789_live;
    auto existing=sources.find(original.generation);if(existing!=sources.end())return existing->second;
    if(original.generation!=40u || !dc2_g789_oracle::ordinarySource(original))return {};
    const auto topology=dc2_g789_raster::topology(original.generation);
    const auto spans=dc2_g789_topology::spans.find(original.visual);
    if(!topology || spans==dc2_g789_topology::spans.end() || topology->buckets!=kBuckets || topology->packets.size()!=kPackets || topology->triangles*3u!=kVertices)return {};
    auto result=std::make_shared<Source>();result->generation=original.generation;result->contract=original.contract;
    result->dictionary=hash(original.dictionary.data(),original.dictionary.size()*4u);
    std::unordered_map<uint32_t,std::vector<std::pair<uint32_t,uint32_t>>> faceCommands;
    for(const auto& span:spans->second)if(!commands(span,ram,faceCommands[span.face]))return {};
    uint32_t bucket=dc2_g789_oracle::physical(dc2_g789::word(ram,original.visual+0x48u));
    for(uint32_t b=0;b<kBuckets;++b){if(!dc2_g789_oracle::range(bucket,32u))return {};result->bucketAddresses[b]=bucket;bucket=dc2_g789_oracle::physical(dc2_g789::word(ram,bucket+8u));}
    if(bucket)return {};
    uint32_t first=0;std::vector<uint32_t> order;
    for(uint32_t i=0;i<kPackets;++i){const auto& p=topology->packets[i];
        auto& commands=faceCommands[p.face];if(commands.empty() || p.bucket>=kBuckets || p.material>=kMaterialGroups)return {};
        const auto kick=commands.front();commands.erase(commands.begin());
        auto& packet=result->packets[i];packet={kick.first,kick.second,first,0,p.bucket,p.kind,uint32_t(p.corners.size())};
        auto& range=result->ranges[p.bucket];if(!range.packets){range.first=first;range.firstPacket=i;range.material=p.material;}
        if(range.material!=p.material)return {};
        const uint32_t count=uint32_t(p.corners.size());if(p.kind!=3u && p.kind!=4u)return {};
        for(uint32_t c=2u;c<count;c+=p.kind==3u?3u:1u)for(uint32_t k=0;k<3u;++k){
            if(first>=kVertices)return {};const auto& corner=p.corners[c-2u+k];auto& input=result->vertices[first++];
            input.position=dc2_g789_raster::vector(original.vectors[0],corner.position*16u);
            input.normal=dc2_g789_raster::vector(original.vectors[1],corner.normal*16u);
            input.uv=dc2_g789_raster::vector(original.vectors[3],corner.st*16u);
            std::memcpy(input.offsets.data(),original.weights.data()+corner.position*32u,16u);
            std::memcpy(input.weights.data(),original.weights.data()+corner.position*32u+16u,16u);
            for(uint32_t offset:input.offsets)if(offset%4u || offset+3u>=kPaletteVectors)return {};
            order.insert(order.end(),{p.face,p.offset,p.ordinal,p.bucket,p.material,p.kind,corner.position,corner.normal,corner.st});
        }
        packet.count=first-packet.first;range.count+=packet.count;++range.packets;
    }
    for(const auto& face:faceCommands)if(!face.second.empty())return {};
    if(first!=kVertices)return {};
    // Every command has one exact ordinal, even across duplicate material buckets.
    for(uint32_t i=0;i<kPackets;++i)for(uint32_t j=0;j<i;++j)
        if(result->packets[i].commandAddress==result->packets[j].commandAddress)return {};result->topology=hash(order.data(),order.size()*4u);
    sources.emplace(result->generation,result);return result;
}
inline std::array<std::shared_ptr<dc2_g789_live::Draw>,32> pool;
inline std::shared_ptr<dc2_g789_live::Draw> acquire(){
    for(auto& p:pool)if(!p)p=std::make_shared<dc2_g789_live::Draw>();
    for(auto& p:pool)if(p.use_count()==1){*p=dc2_g789_live::Draw{};return p;}
    return {};
}
inline std::shared_ptr<const dc2_g789_live::Draw> publish(const dc2_g789::Identity& identity,uint64_t tex0,const uint8_t* ram){
    using namespace dc2_g789_live;
    if(!requested() || identity.mesh!=dc2_g789::Motion || identity.pass!=dc2_g789::Silhouette)return {};
    {std::lock_guard<std::mutex> lock(drawMutex);auto i=draws.find(identity.draw);if(i!=draws.end())return i->second.draw;}
    const auto pose=dc2_g789_oracle::findPose(identity.draw);
    if(!pose || !pose->source || !pose->instance || pose->palette.size()!=1344u){++refused;return {};}
    const auto sourceIt=sources.find(pose->source->generation);
    if(sourceIt==sources.end()){++refused;return {};}
    const auto frozen=sourceIt->second;
    // Engine-owned TEX0, captured before mgDrawDirect, supplies W dimensions and target.
    if(((tex0>>14u)&63u)!=8u || ((tex0>>20u)&63u)!=0u || ((tex0>>26u)&15u)!=9u || ((tex0>>30u)&15u)!=9u || (tex0&31u)){++refused;return {};}
    auto draw=acquire();if(!draw){++refused;return {};}
    draw->source=frozen;auto& id=draw->identity;
    id.draw=identity.draw;id.operation=identity.operation;id.source=frozen->generation;id.instance=pose->instance;
    id.palette=pose->generation;id.dictionary=frozen->dictionary;id.bind=hash(pose->bindMatrices.data(),pose->bindMatrices.size());
    id.material=hash(pose->materials.data(),pose->materials.size());id.topology=frozen->topology;
    id.frame=identity.frame;id.pass=identity.pass;id.wfbp=uint32_t(tex0&16383u)>>5u;id.wtex0=tex0;id.bindSelector=pose->bindSelector;
    id.resource=frozen->ready.load(std::memory_order_acquire);
    std::memcpy(draw->constants.data(),pose->palette.data(),pose->palette.size());
    for(uint32_t material=0;material<kMaterialGroups;++material){
        dc2_g789_raster::Primitive primitive;primitive.pose=pose;
        bool found=false;for(const auto& packet:dc2_g789_raster::topology(id.source)->packets)if(packet.material==material){primitive.face=packet.face;found=true;break;}
        std::array<Vector,23> constants;
        if(!found || !dc2_g789_raster::constants(primitive,constants,512,512,1792,1840)){++refused;return {};}
        std::copy(constants.begin(),constants.end(),draw->constants.begin()+kPaletteVectors+material*23u);
    }
    for(const auto& range:frozen->ranges)for(uint32_t v=range.first;v<range.first+range.count;++v)
        draw->metadata[v].value={kPaletteVectors+range.material*23u,0u,v,3u};
    draw->committed=false;
    if(deletion()){
        const auto instance=dc2_g789_oracle::instances.find(identity.visual);
        // Existing 32-pose P-J authority: literal loader dictionary/weights,
        // variant-0 packer, unchanged bind records and all nine material groups.
        const bool supported=instance!=dc2_g789_oracle::instances.end() && instance->second==id.instance &&
            frozen->contract==0xdf1e0093fe4e1764ull && pose->bindSelector==70u &&
            id.bind==0x2b02aba20f4bf55full && id.material==0xc005e19554d91a1full &&
            hash(pose->config.data(),pose->config.size())==0xc879d6f5249eeb7full &&
            identity.wWidth==512u && identity.wHeight==416u && identity.wOffsetX==1792u && identity.wOffsetY==1840u;
        if(!supported){++refused;return {};}
        if(id.resource==id.source){
            id.commitment=++nextCommitment;draw->committed=true;draw->rectangle={0,0,511,415};
        }else ++refusedCold;
    }
    ++accepted;++paletteValid;++wValid;++topologyValid;if(id.resource==id.source)++sourceValid;
    dc2_g789_live::publish(draw);return draw;
}
}
