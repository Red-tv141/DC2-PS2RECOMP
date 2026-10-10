#pragma once
// G789 live raster diagnostic. Legacy geometry remains the only live producer.
// Packet addresses transport exact ownership; output bytes are oracle data only.
#if defined(PS2X_G775_DIAG) || defined(PS2X_G789_DIAG) || defined(PS2X_G777_NATIVE_GL)
#include "runtime/dc2_g789_oracle.h"
#include "runtime/dc2_native_draw.h"
#include <map>
namespace dc2_g789_raster {
using Pose = dc2_g789_oracle::Pose;
using Source = dc2_g789_oracle::Source;
using Vector = dc2_native::Vector;
struct Corner { uint32_t position=0,normal=0,st=0; };
// Frozen before the first draw, from the MotionMDT loader's actual cold nodes.
// Duplicate material buckets remain distinct; source encounter order is not order.
struct TopologyPacket {uint32_t face=0,offset=0,ordinal=0,bucket=0,material=0,kind=0;std::vector<Corner> corners;};
struct Topology {uint64_t source=0;std::vector<TopologyPacket> packets;uint32_t buckets=0,triangles=0;};
inline std::mutex topologyMutex;
inline std::unordered_map<uint64_t,std::shared_ptr<const Topology>> topologies;
inline void publishTopology(std::shared_ptr<const Topology> value){std::lock_guard<std::mutex> lock(topologyMutex);topologies.emplace(value->source,std::move(value));}
inline std::shared_ptr<const Topology> topology(uint64_t source){std::lock_guard<std::mutex> lock(topologyMutex);auto i=topologies.find(source);return i==topologies.end()?std::shared_ptr<const Topology>{}:i->second;}
struct Vertex { float x=0,y=0,z=0,s=0,t=0,q=0; uint8_t rgba[4]{},fog=0; };
struct Primitive {
    uint64_t id=0;
    std::shared_ptr<const Pose> pose;
    uint32_t face=0,ordinal=0,packet=0,triangle=0;
    std::array<Corner,3> corners{};
    std::array<Vertex,3> expected{};
};
struct Packet { std::vector<std::shared_ptr<const Primitive>> primitives; };
// Immutable EE producer/consumer identity. G787 itself remains untouched.
struct Composite {
    std::shared_ptr<const Pose> pose;
    uint32_t seq=0,kind=0,outline=0,texture=0;
    std::array<int32_t,4> rect{};
    std::array<uint32_t,4> colour{};
    uint64_t tex0=0,tex1=0;
};
inline std::mutex compositeMutex;
inline std::unordered_map<uint32_t,std::shared_ptr<const Composite>> composites;
inline void publishComposite(const std::shared_ptr<const Composite>& value){std::lock_guard<std::mutex> lock(compositeMutex);composites.emplace(value->seq,value);}
inline std::shared_ptr<const Composite> composite(uint32_t seq){std::lock_guard<std::mutex> lock(compositeMutex);auto i=composites.find(seq);return i==composites.end()?std::shared_ptr<const Composite>{}:i->second;}
inline thread_local std::shared_ptr<const Composite> currentConsumer;
struct CompositeScope {
    std::shared_ptr<const Composite> previous;
    explicit CompositeScope(uint32_t seq):previous(currentConsumer){currentConsumer=composite(seq);}
    ~CompositeScope(){currentConsumer=std::move(previous);}
};
inline bool on(){ static const bool v=[] {const char* e=std::getenv("DC2_G789_W_SHADOW");return e && std::atoi(e)!=0;}();return v && dc2_g789_oracle::on(); }
inline std::mutex packetMutex;
inline std::unordered_map<const uint8_t*,std::pair<size_t,std::shared_ptr<const Packet>>> packets;
inline std::atomic<uint64_t> nextPrimitive{0};
inline std::atomic<uint64_t> published{0},consumed{0},refused{0},matched{0},unmatched{0};
inline thread_local std::shared_ptr<const Packet> current;
inline thread_local size_t next=0;
inline thread_local std::shared_ptr<const Primitive> markerPrimitive;
struct PacketScope {
    std::shared_ptr<const Packet> previous; size_t previousNext;
    PacketScope(const uint8_t* data,size_t bytes):previous(current),previousNext(next){
        current.reset();next=0;
        if(!on())return;
        std::lock_guard<std::mutex> lock(packetMutex);
        const auto i=packets.find(data);if(i==packets.end())return;
        if(i->second.first==bytes){current=i->second.second;++consumed;}else ++refused;
        packets.erase(i);
    }
    ~PacketScope(){current=std::move(previous);next=previousNext;}
};
inline uint32_t u32(const uint8_t* p){uint32_t v;std::memcpy(&v,p,4);return v;}
inline uint64_t u64(const uint8_t* p){uint64_t v;std::memcpy(&v,p,8);return v;}
inline Vector vector(const std::vector<uint8_t>& v,size_t offset){Vector out{};if(offset+16<=v.size())std::memcpy(out.data(),v.data()+offset,16);return out;}
inline dc2_native::Matrix matrix(const std::vector<uint8_t>& v,size_t offset){dc2_native::Matrix out;for(unsigned i=0;i<4;++i)out[i]=vector(v,offset+16*i);return out;}
inline bool constants(const Primitive& p,std::array<Vector,23>& out,int width,int height,int ofx,int ofy){
    const auto& pose=*p.pose;const auto& info=pose.renderInfo;
    const auto& face=pose.source->faces[p.face];const uint32_t material=face.header[1]&65535u;
    const uint32_t light=u32(info.data()+0x3f4u);const size_t at=0x400u+size_t(light)*0x150u;
    if(at+0x90u>info.size() || material*48u+16u>pose.materials.size() || pose.config.size()<0x90u)return false;
    dc2_native::DC2NativeDraw d{};
    d.world=matrix(pose.objectMatrix,0);d.cameraScreen=matrix(info,0x10);d.cameraClip=matrix(info,0x220);d.cameraViewport=matrix(info,0x260);
    for(unsigned i=0;i<3;++i)d.lightDirections[i]=vector(info,at+i*16u);
    for(unsigned i=0;i<4;++i)d.lightColours[i]=vector(info,at+0x40u+i*16u);
    d.ambient=vector(info,at+0x80u);float alpha;std::memcpy(&alpha,pose.config.data()+0x44u,4);d.ambient[3]*=alpha;
    auto fog=vector(info,0xfdcu);d.fog={fog[0],fog[2],fog[1],fog[3]};
    d.materials.resize(1);d.materials[0].colour=vector(pose.materials,material*48u);
    return dc2_native::buildNativeConstants(d,0,1,width,height,ofx,ofy,out);
}
// Unique, byte-exact incoming source correspondence. Never copy worker inputs
// into the candidate. This development diagnostic refuses clipping and ADC.
inline void publish(const std::shared_ptr<dc2_g789_oracle::Witness>& witness,const std::vector<std::vector<uint8_t>>& output){
    if(!on() || !witness || witness->stage!=dc2_g789::Geometry || !witness->paletteMatch)return;
    const auto& pose=*witness->pose;const auto& s=*pose.source;
    if(s.generation!=40u || s.counts[0]!=395u || s.counts[1]!=395u || s.counts[3]!=234u || s.dictionary.size()!=21u || s.faces.size()!=115u)return;
    const auto& before=witness->data;const size_t top=size_t(witness->top)*16u;
    if(top+32u>before.size() || u32(before.data()+0x260u)!=0x40u){++refused;return;}
    const uint32_t count=u32(before.data()+top)&32767u,kind=u32(before.data()+top+28u);
    if(!count || (kind!=3u && kind!=4u) || top+32u+size_t(count)*80u>before.size()){++refused;return;}
    size_t found=0;uint32_t selectedFace=0,selectedOffset=0;
    for(uint32_t f=0;f<s.faces.size();++f){
        const auto& face=s.faces[f];if((face.header[0]&0x310u) || (face.header[0]&7u)!=kind || (face.header[0]>>16)!=3u)continue;
        const uint32_t size=face.header[2]&65535u;
        for(uint32_t offset=0;offset+count<=size;offset+=(kind==3u?3u:2u)){
            bool exact=true;
            for(uint32_t c=0;c<count && exact;++c){
                const uint8_t* indices=face.indices.data()+size_t(offset+c)*12u;
                const uint32_t p=u32(indices),n=u32(indices+4),st=u32(indices+8);
                if(p>=s.counts[0] || n>=s.counts[1] || st>=s.counts[3]){exact=false;break;}
                const uint8_t* record=s.weights.data()+size_t(p)*32u;
                float fourth;std::memcpy(&fourth,record+28u,4);if(fourth!=0){exact=false;break;}
                const size_t start=top+32u;
                exact=std::memcmp(s.vectors[0].data()+p*16u,before.data()+start+c*16u,16)==0 &&
                      std::memcmp(s.vectors[1].data()+n*16u,before.data()+start+(count+c)*16u,16)==0 &&
                      std::memcmp(s.vectors[3].data()+st*16u,before.data()+start+(count*2u+c)*16u,16)==0 &&
                      std::memcmp(record,before.data()+start+count*48u+c*32u,32)==0;
            }
            if(exact){++found;selectedFace=f;selectedOffset=offset;}
        }
    }
    if(found!=1u || output.size()!=1u){++refused;return;}
    const auto& bytes=output.front();if(bytes.size()<16u){++refused;return;}
    const uint64_t tag=u64(bytes.data()),regs=u64(bytes.data()+8u);
    if((tag&32767u)!=count || ((tag>>58u)&3u)!=0u || (tag>>60u)!=3u || (regs&0xfffu)!=0x412u || bytes.size()!=16u+count*48u || ((tag>>47u)&7u)!=kind){++refused;return;}
    auto packet=std::make_shared<Packet>();const auto& face=s.faces[selectedFace];
    for(uint32_t c=2u;c<count;c+=(kind==3u?3u:1u)){
        auto prim=std::make_shared<Primitive>();prim->id=++nextPrimitive;prim->pose=witness->pose;prim->face=selectedFace;prim->ordinal=witness->ordinal;prim->triangle=uint32_t(packet->primitives.size());
        for(unsigned k=0;k<3;++k){
            const uint32_t corner=c-2u+k;const uint8_t* indices=face.indices.data()+size_t(selectedOffset+corner)*12u;
            prim->corners[k]={u32(indices),u32(indices+4),u32(indices+8)};
            const uint8_t* v=bytes.data()+16u+corner*48u;auto& expected=prim->expected[k];
            std::memcpy(&expected.s,v,4);std::memcpy(&expected.t,v+4,4);std::memcpy(&expected.q,v+8,4);
            for(unsigned a=0;a<4;++a)expected.rgba[a]=uint8_t(u32(v+16+a*4));
            expected.x=float(u32(v+32)&65535u)/16.0f;expected.y=float(u32(v+36)&65535u)/16.0f;expected.z=float((u32(v+40)>>4u)&0xffffffu);
            const uint32_t wf=u32(v+44);expected.fog=uint8_t(wf>>4u);if(wf&0x8000u){++refused;return;}
        }
        packet->primitives.push_back(std::move(prim));
    }
    {std::lock_guard<std::mutex> lock(packetMutex);if(!packets.emplace(bytes.data(),std::make_pair(bytes.size(),packet)).second){++refused;return;}}
    ++published;
}
inline void marker(const dc2_g789_live::OwnedPacket& owner){
    markerPrimitive.reset();if(!on() || !owner.draw || !owner.draw->committed)return;
    if(owner.draw->identity.frame<dc2_g789_oracle::option("DC2_G789_ORACLE_FIRST",1800) ||
       owner.draw->identity.frame>dc2_g789_oracle::option("DC2_G789_ORACLE_LAST",1831))return;
    const auto pose=dc2_g789_oracle::findPose(owner.draw->identity.draw);
    const auto frozen=topology(owner.draw->identity.source);
    if(!pose || !frozen || owner.ordinal>=frozen->packets.size())dc2_g789_live::fault("diagnostic marker identity missing",owner.draw->identity.draw);
    const auto& packet=frozen->packets[owner.ordinal];
    auto primitive=std::make_shared<Primitive>();primitive->id=++nextPrimitive;primitive->pose=pose;
    primitive->face=packet.face;primitive->ordinal=packet.ordinal;primitive->triangle=0;
    for(unsigned k=0;k<3;++k)primitive->corners[k]=packet.corners[k];
    markerPrimitive=std::move(primitive);++published;++consumed;++matched;
}
template<class V> inline bool same(const Vertex& a,const V& b){return a.x==b.x && a.y==b.y && a.z==b.z && a.s==b.s && a.t==b.t && a.q==b.q && a.rgba[0]==b.r && a.rgba[1]==b.g && a.rgba[2]==b.b && a.rgba[3]==b.a && a.fog==b.fog;}
template<class V> inline std::shared_ptr<const Primitive> observe(const V* vertices){
    if(!current)return {};
    // GS rejects invisible/degenerate primitives before capture; retain the
    // exact order of every surviving primitive, with no cross-packet matching.
    for(size_t i=next;i<current->primitives.size();++i){const auto& p=current->primitives[i];
        if(same(p->expected[0],vertices[0]) && same(p->expected[1],vertices[1]) && same(p->expected[2],vertices[2])){next=i+1u;++matched;return p;}}
    ++unmatched;return {};
}
struct Job {uint32_t first=0;int ofx=0,ofy=0;uint32_t flags=0;std::shared_ptr<const Primitive> primitive;};
struct CompositeJob {uint32_t first=0,count=0;std::shared_ptr<const Composite> composite;};
// Preserve the established hot carrier layouts. An alias keeps the exact
// original pointer (including null), while its control block retains immutable
// diagnostic metadata and the original resource's lifetime. No raw byte/hash
// association, no ownership inferred from texture or vertex similarity.
struct Transport {std::shared_ptr<const void> resource;std::shared_ptr<const Primitive> primitive;std::shared_ptr<const Composite> composite;std::shared_ptr<const std::vector<Job>> jobs;std::shared_ptr<const std::vector<CompositeJob>> consumers;};
inline std::mutex transportMutex;
inline std::map<std::weak_ptr<const void>,std::shared_ptr<const Transport>,std::owner_less<std::weak_ptr<const void>>> transports;
template<class T> inline void attach(std::shared_ptr<T>& carrier,std::shared_ptr<Transport> value){
    const auto original=carrier;
    value->resource=carrier;carrier=std::shared_ptr<T>(value,carrier.get());
    dc2_g789_live::inherit(carrier,original);
    std::lock_guard<std::mutex> lock(transportMutex);transports.emplace(std::weak_ptr<const void>(carrier),std::move(value));
}
template<class T> inline std::shared_ptr<const Transport> transport(const std::shared_ptr<T>& carrier){
    if(!on())return {};
    std::lock_guard<std::mutex> lock(transportMutex);const auto i=transports.find(std::weak_ptr<const void>(carrier));return i==transports.end()?std::shared_ptr<const Transport>{}:i->second;
}
template<class T> inline void capture(std::shared_ptr<T>& carrier,const std::shared_ptr<const Primitive>& primitive){
    if(!primitive)return;auto value=std::make_shared<Transport>();value->primitive=primitive;attach(carrier,std::move(value));
}
template<class T> inline void captureComposite(std::shared_ptr<T>& carrier){
    auto c=currentConsumer;if(!c)return;const uint32_t seq=c->seq;
    std::fprintf(stderr,"[G789:wtransport] stage=capture seq=%u draw=%llu matched=%llu\n",seq,(unsigned long long)c->pose->identity.draw,(unsigned long long)matched.load());
    auto value=std::make_shared<Transport>();value->composite=std::move(c);attach(carrier,std::move(value));
}
template<class T> inline std::shared_ptr<const Composite> consumer(const std::shared_ptr<T>& carrier){const auto value=transport(carrier);return value?value->composite:std::shared_ptr<const Composite>{};}
template<class T> inline std::shared_ptr<const Primitive> geometry(const std::shared_ptr<T>& carrier){const auto value=transport(carrier);return value?value->primitive:std::shared_ptr<const Primitive>{};}
template<class T> inline void seal(std::shared_ptr<T>& carrier,std::vector<Job>&& jobs,std::vector<CompositeJob>&& consumers){
    if((jobs.empty() && consumers.empty()) || carrier)return;
    std::fprintf(stderr,"[G789:wtransport] stage=seal geometry=%zu consumers=%zu\n",jobs.size(),consumers.size());
    auto value=std::make_shared<Transport>();value->jobs=std::make_shared<const std::vector<Job>>(std::move(jobs));value->consumers=std::make_shared<const std::vector<CompositeJob>>(std::move(consumers));attach(carrier,std::move(value));
}
template<class T> inline std::shared_ptr<const std::vector<Job>> batch(const std::shared_ptr<T>& carrier){const auto value=transport(carrier);return value?value->jobs:std::shared_ptr<const std::vector<Job>>{};}
} // namespace dc2_g789_raster
#endif
