#pragma once
// Production transport. No VU output, comparison vertices or capture buffers.
// The first admitted contract is the independently proven ordinary W source.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdio>

namespace dc2_g789_live {
enum class Mechanism { Legacy, Compute, Vertex };
inline Mechanism mechanism() {
    static const Mechanism value=[] {
        const char* v=std::getenv("DC2_G789_SKIN");
#if defined(PS2X_G775_DIAG)
        // Losing mechanism is diagnostic-only after the live A/C/V decision.
        if(v && std::strcmp(v,"compute")==0)return Mechanism::Compute;
#endif
        if(v && std::strcmp(v,"vertex")==0)return Mechanism::Vertex;
        return Mechanism::Legacy;
    }();
    return value;
}
inline bool requested(){return mechanism()!=Mechanism::Legacy;}
inline bool deletion(){static const bool value=[] {const char* v=std::getenv("DC2_G789_DELETE");return v && std::strcmp(v,"1")==0;}();return requested() && value;}
constexpr uint32_t kVertices = 1956;
constexpr uint32_t kPackets = 131;
constexpr uint32_t kBuckets = 10;
constexpr uint32_t kPaletteVectors = 84;
constexpr uint32_t kMaterialGroups = 9;
constexpr uint32_t kConstantVectors = kPaletteVectors + kMaterialGroups * 23;
using Vector = std::array<float, 4>;
struct Input {
    Vector position, normal, uv;
    std::array<uint32_t, 4> offsets;
    Vector weights;
};
struct Meta { std::array<uint32_t, 4> value; };
static_assert(sizeof(Input) == 80 && sizeof(Meta) == 16);
struct Range {
    uint32_t first = 0, count = 0, firstPacket = 0, packets = 0, material = 0;
};
struct Packet {
    uint32_t commandAddress=0, command=0, first=0, count=0, bucket=0, kind=0, corners=0;
};
struct Source {
    uint64_t generation = 0, topology = 0, dictionary = 0, contract = 0;
    std::array<Input, kVertices> vertices{};
    std::array<Range, kBuckets> ranges{};
    std::array<Packet, kPackets> packets{};
    std::array<uint32_t,kBuckets> bucketAddresses{};
    // Receipt publishes complete GL preparation for this immutable generation.
    // It grants no permission to inspect mutable/future GS state.
    mutable std::atomic<uint64_t> ready{0};
};
struct Identity {
    uint64_t draw = 0, operation = 0, source = 0, instance = 0;
    uint64_t palette = 0, dictionary = 0, bind = 0, material = 0, topology = 0;
    uint32_t frame = 0, pass = 0, wfbp = 0, bindSelector = 0;
    uint32_t variant = 0, clipping = 0, adc = 0;
    uint64_t resource = 0, wtex0 = 0, commitment = 0;
};
struct Draw {
    Identity identity{};
    std::shared_ptr<const Source> source;
    std::array<Vector, kConstantVectors> constants{};
    std::array<Meta, kVertices> metadata{};
    std::array<int32_t, 4> rectangle{};
    bool committed = false; // irreversible geometry suppression was admitted
};
constexpr std::size_t kStaticBytes = sizeof(Input) * kVertices;
constexpr std::size_t kMetadataBytes = sizeof(Meta) * kVertices;
constexpr std::size_t kConstantBytes = sizeof(Vector) * kConstantVectors;
static_assert(kStaticBytes == 156480 && kMetadataBytes == 31296 && kConstantBytes == 4656);
// Publication/ownership transport. Source and pose are final before DMA copy.
// Command addresses come from the loader-frozen VIF command stream.
struct OwnedPacket {std::shared_ptr<const Draw> draw;uint32_t ordinal=0;};
struct Triangle {std::shared_ptr<const Draw> draw;uint32_t first=0,count=3;};
inline std::atomic<uint64_t> published{0},accepted{0},sourceValid{0},paletteValid{0},wValid{0},topologyValid{0},nativeW{0},refused{0},ownershipFaults{0};
inline std::atomic<uint64_t> staticUploads{0},staticBytes{0},sourceReallocations{0},shaderCompiles{0},metadataBytes{0},constantBytes{0},coldCommitted{0},nativeVertices{0};
inline std::atomic<uint64_t> nextCommitment{0},committedDraws{0},expectedGeometry{0},suppressedGeometry{0},geometryEnqueued{0},geometryExecuted{0},path1Geometry{0},nativeMarkers{0},refusedCold{0};
inline void fault(const char* reason,uint64_t draw=0){
    ++ownershipFaults;std::fprintf(stderr,"[G789:deletion] FATAL %s draw=%llu\n",reason,(unsigned long long)draw);std::abort();
}
inline bool stats(){static const bool v=[] {const char* e=std::getenv("DC2_G789_LIVE_STAT");return e && std::atoi(e)!=0;}();return v;}
struct FrameCounters {uint64_t committed=0,expected=0,suppressed=0,enqueued=0,executed=0,path1=0,producers=0,markers=0,setup=0;};
inline std::mutex frameMutex;
inline std::map<uint32_t,FrameCounters> frameCounters;
inline void count(uint32_t frame,uint64_t FrameCounters::*field,uint64_t value=1){
    if(!stats())return;std::lock_guard<std::mutex> lock(frameMutex);frameCounters[frame].*field+=value;
}
inline void produced(const Draw& draw){
    ++nativeW;if(draw.committed)count(draw.identity.frame,&FrameCounters::producers);
}
inline void suppressed(const Draw& draw){
    ++suppressedGeometry;count(draw.identity.frame,&FrameCounters::suppressed);
}
inline void parsed(const Draw& draw){
    ++nativeMarkers;count(draw.identity.frame,&FrameCounters::markers);
}
inline void reportFrames(uint32_t frame){
    if(!stats() || frame<8u)return;
    std::lock_guard<std::mutex> lock(frameMutex);
    for(auto i=frameCounters.begin();i!=frameCounters.end() && i->first+8u<=frame;){
        const auto& c=i->second;
        const bool pass=c.expected==c.suppressed && c.expected==c.markers && c.committed==c.producers &&
            c.enqueued==0u && c.executed==0u && c.path1==0u && c.setup==c.committed;
        std::fprintf(stderr,"[G789:zero-leak] frame=%u committed=%llu expected=%llu suppressed=%llu enqueued=%llu executed=%llu path1=%llu producers=%llu markers=%llu setup=%llu pass=%u\n",
            i->first,(unsigned long long)c.committed,(unsigned long long)c.expected,(unsigned long long)c.suppressed,
            (unsigned long long)c.enqueued,(unsigned long long)c.executed,(unsigned long long)c.path1,
            (unsigned long long)c.producers,(unsigned long long)c.markers,(unsigned long long)c.setup,unsigned(pass));
        if(!pass)fault("zero-leak frame failed");i=frameCounters.erase(i);
    }
}
inline std::mutex drawMutex;
struct Pending {std::shared_ptr<const Draw> draw;uint32_t packets=0;std::array<bool,kPackets> seen{};};
inline std::unordered_map<uint64_t,Pending> draws;
inline void publish(std::shared_ptr<const Draw> draw){
    const uint64_t id=draw->identity.draw;
    std::lock_guard<std::mutex> lock(drawMutex);
    const bool committed=draw->committed;
    if(!draws.emplace(id,Pending{std::move(draw),0,{}}).second)fault("duplicate publication",id);
    ++published;if(committed){
        ++committedDraws;expectedGeometry.fetch_add(kPackets,std::memory_order_relaxed);
        count(draws.at(id).draw->identity.frame,&FrameCounters::committed);
        count(draws.at(id).draw->identity.frame,&FrameCounters::expected,kPackets);
    }
}
inline OwnedPacket own(uint64_t id,uint32_t bucket,uint32_t address,uint32_t start,bool resume,uint64_t commitment=0){
    if(!requested() || !id)return {};
    std::lock_guard<std::mutex> lock(drawMutex);
    auto it=draws.find(id);if(it==draws.end()){if(commitment)fault("committed owner missing",id);return {};}
    if(it->second.draw->identity.commitment!=commitment)fault("commitment generation mismatch",id);
    const auto draw=it->second.draw;
    for(uint32_t i=0;i<kPackets;++i){const auto& p=draw->source->packets[i];
        if(p.commandAddress!=address)continue;
        if(draw->source->bucketAddresses[p.bucket]!=bucket ||
           ((p.command>>24u)&127u)!=(resume?0x17u:0x14u) || (!resume && start!=0x10u)){
            if(draw->committed)fault("committed command mismatch",id);++ownershipFaults;return {};
        }
        if(it->second.seen[i])fault("duplicate geometry ordinal",id);
        it->second.seen[i]=true;
        if(++it->second.packets==kPackets)draws.erase(it);
        return {draw,i};
    }
    if(draw->committed)fault("unknown committed geometry command",id);++ownershipFaults;return {};
}
inline std::mutex packetMutex;
inline std::unordered_map<const uint8_t*,std::pair<size_t,OwnedPacket>> packets;
inline void packet(const uint8_t* data,size_t bytes,const OwnedPacket& owner){
    if(!owner.draw)return;
    if(owner.draw->committed){++path1Geometry;count(owner.draw->identity.frame,&FrameCounters::path1);fault("committed legacy Path1 geometry",owner.draw->identity.draw);}
    const auto& source=owner.draw->source->packets[owner.ordinal];
    uint64_t lo=0,hi=0;if(bytes>=16u){std::memcpy(&lo,data,8u);std::memcpy(&hi,data+8u,8u);}
    if(bytes!=16u+size_t(source.corners)*48u || (lo&32767u)!=source.corners ||
       ((lo>>47u)&2047u)!=(source.kind==3u?0x5bu:0x5cu) || hi!=0x412u){++ownershipFaults;return;}
    std::lock_guard<std::mutex> lock(packetMutex);
    if(!packets.emplace(data,std::make_pair(bytes,owner)).second)++ownershipFaults;
}
inline thread_local OwnedPacket currentPacket{};
inline thread_local Triangle currentTriangle{};
inline thread_local uint32_t triangleIndex=0;
inline thread_local bool priming=false,primerConsumed=false;
constexpr uint64_t kMarkerMagic=0x789d;
inline void marker(const uint8_t* data,size_t bytes,const OwnedPacket& owner){
    if(!owner.draw || !owner.draw->committed || bytes!=16u)fault("invalid native marker");
    std::lock_guard<std::mutex> lock(packetMutex);
    if(!packets.emplace(data,std::make_pair(bytes,owner)).second)fault("duplicate native marker",owner.draw->identity.draw);
}
struct PacketScope {
    PacketScope(const uint8_t* data,size_t bytes){
        if(!requested())return;currentPacket={};currentTriangle={};triangleIndex=0;
        std::lock_guard<std::mutex> lock(packetMutex);auto i=packets.find(data);
        if(i!=packets.end()){
            if(i->second.first==bytes)currentPacket=i->second.second;else ++ownershipFaults;
            packets.erase(i);
        }
    }
    ~PacketScope(){if(requested()){currentPacket={};currentTriangle={};}}
};
inline void triangle(bool drawing){
    if(!currentPacket.draw || priming)return;
    const auto& p=currentPacket.draw->source->packets[currentPacket.ordinal];
    currentTriangle={};
    if(!drawing || triangleIndex*3u>=p.count){++ownershipFaults;return;}
    currentTriangle={currentPacket.draw,p.first+triangleIndex++*3u};
}
// Preserve the existing decoded resource pointer and its lifetime. Exact live
// identity travels in its control block; no attribute/texture matching occurs.
struct Transport {std::shared_ptr<const void> resource;Triangle triangle;};
inline std::mutex transportMutex;
inline std::map<std::weak_ptr<const void>,std::weak_ptr<Transport>,std::owner_less<std::weak_ptr<const void>>> transports;
template<class T> inline void capture(std::shared_ptr<T>& resource){
    if(!currentTriangle.draw)return;
    auto value=std::make_shared<Transport>();value->resource=resource;value->triangle=currentTriangle;
    resource=std::shared_ptr<T>(value,resource.get());
    std::lock_guard<std::mutex> lock(transportMutex);
    if(transports.size()>1024u)for(auto i=transports.begin();i!=transports.end();){if(i->first.expired())i=transports.erase(i);else ++i;}
    transports[std::weak_ptr<const void>(resource)]=value;
}
template<class T> inline Triangle geometry(const std::shared_ptr<T>& resource){
    if(!requested())return {};
    // Compare owner control blocks, never the (possibly null/shared) pixel pointer.
    std::lock_guard<std::mutex> lock(transportMutex);
    auto i=transports.find(std::weak_ptr<const void>(resource));if(i==transports.end())return {};
    auto value=i->second.lock();return value?value->triangle:Triangle{};
}
template<class T> inline void inherit(const std::shared_ptr<T>& carrier,const std::shared_ptr<T>& resource){
    if(!requested())return;std::lock_guard<std::mutex> lock(transportMutex);
    auto i=transports.find(std::weak_ptr<const void>(resource));
    if(i!=transports.end())transports[std::weak_ptr<const void>(carrier)]=i->second;
}
template<class Entries> inline void requireGpu(const Entries& entries){
    if(!deletion())return;
    for(const auto& entry:entries){const auto native=geometry(entry.decoded);
        if(native.draw && native.draw->committed)fault("committed geometry cannot replay on CPU",native.draw->identity.draw);
    }
}
inline void report(uint32_t frame){
    if(!stats() || !requested())return;reportFrames(frame);static uint32_t last=~0u;if(frame==last || frame%120u)return;last=frame;
    std::fprintf(stderr,"[G789:live] frame=%u published=%llu accepted=%llu receipt=%llu palette=%llu w=%llu topology=%llu nativeW=%llu refused=%llu ownershipFaults=%llu staticUploads=%llu staticBytes=%llu realloc=%llu compile=%llu metadataBytes=%llu constantBytes=%llu coldCommitted=%llu nativeVertices=%llu geometrySuppressed=%llu committed=%llu expected=%llu enqueued=%llu executed=%llu path1=%llu markers=%llu refusedCold=%llu cpuSkinOutput=0 gpuReadback=0 mapWait=0 outputUpload=0\n",frame,
        (unsigned long long)published.load(),(unsigned long long)accepted.load(),(unsigned long long)sourceValid.load(),(unsigned long long)paletteValid.load(),(unsigned long long)wValid.load(),(unsigned long long)topologyValid.load(),(unsigned long long)nativeW.load(),(unsigned long long)refused.load(),(unsigned long long)ownershipFaults.load(),(unsigned long long)staticUploads.load(),(unsigned long long)staticBytes.load(),(unsigned long long)sourceReallocations.load(),(unsigned long long)shaderCompiles.load(),(unsigned long long)metadataBytes.load(),(unsigned long long)constantBytes.load(),(unsigned long long)coldCommitted.load(),(unsigned long long)nativeVertices.load(),(unsigned long long)suppressedGeometry.load(),(unsigned long long)committedDraws.load(),(unsigned long long)expectedGeometry.load(),(unsigned long long)geometryEnqueued.load(),(unsigned long long)geometryExecuted.load(),(unsigned long long)path1Geometry.load(),(unsigned long long)nativeMarkers.load(),(unsigned long long)refusedCold.load());
}
}
