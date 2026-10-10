// Bounded CreatePacket economics arm. Original leaf packet helpers remain the
// authority. Oracle predictions are checked at AddPacket entry, before G788 tags
// are substituted; no generated function return is used as a completion seam.
#if defined(PS2X_G777_NATIVE_GL)
namespace g788_packet {
using Fn=g788::RecompiledFunction;
static Fn previous=nullptr,material=nullptr,pmode=nullptr,texflush=nullptr;
static int mode() {
    static const int value=[] {const char* e=std::getenv("DC2_G788_PACKET");
        if(!e || !std::strcmp(e,"off"))return 0;
        if(!std::strcmp(e,"oracle"))return 1;
        if(!std::strcmp(e,"native"))return 2;
        std::fprintf(stderr,"[G788:packet] FATAL invalid mode=%s\n",e);std::abort();return 0;}();
    return value;
}
struct Bucket {uint32_t address=0,start=0,qw=0;std::vector<uint32_t> faces;bool checked=false;};
struct Prediction {
    uint32_t self=0,frame=0,packetPool=0,riPool=0,packetStart=0,riStart=0,packetUsed=0,riUsed=0;
    uint64_t generation=0;
    std::vector<Bucket> buckets;
    std::vector<uint8_t> chain,ri;
};
static Prediction pending;
static uint64_t attempted=0,prepared=0,converted=0,oracleDraws=0,oracleBuckets=0,checkedBytes=0,
                refused=0,alreadyNative=0,unpublishedBuckets=0,prepareNs=0;
static void report() {
    if(!g788::censusOn() && !dc2_g788_econ::on())return;
    static uint32_t last=~0u;const uint32_t frame=g788::sf();
    if(frame/600==last)return;last=frame/600;
    std::fprintf(stderr,"[G788:packet] sf=%u mode=%d attempted=%llu prepared=%llu converted=%llu oracleDraws=%llu oracleBuckets=%llu checkedBytes=%llu refused=%llu alreadyNative=%llu unpublishedBuckets=%llu prepareNs=%llu\n",
        frame,mode(),(unsigned long long)attempted,(unsigned long long)prepared,(unsigned long long)converted,
        (unsigned long long)oracleDraws,(unsigned long long)oracleBuckets,(unsigned long long)checkedBytes,
        (unsigned long long)refused,(unsigned long long)alreadyNative,(unsigned long long)unpublishedBuckets,
        (unsigned long long)prepareNs);
}
static void reg(R5900Context& c,unsigned n,uint32_t value) {
    // Match the generated signed-32 assignment while preserving the upper lane.
    c.r[n]=_mm_unpacklo_epi64(_mm_cvtsi64_si128(int64_t(int32_t(value))),_mm_srli_si128(c.r[n],8));
}
static void append(std::vector<uint8_t>& out,const void* data,size_t bytes) {
    const size_t at=out.size();out.resize(at+bytes);std::memcpy(out.data()+at,data,bytes);
}
static void tag(std::vector<uint8_t>& out,uint32_t lo,uint32_t address=0) {
    const uint32_t words[4]={lo,address,0,0};append(out,words,sizeof(words));
}
static bool prepare(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime,Prediction& p) {
    p.self=getRegU32(ctx,4)&0x1fffffffu;p.frame=g788::sf();
    const uint32_t dm=getRegU32(ctx,5)&0x1fffffffu;
    if(!g788::ok(dm,0x68) || !g788::ok(p.self,0x50))return false;
    p.riPool=g788::rd(ram,dm+0x60)&0x1fffffffu;
    p.packetPool=g788::rd(ram,dm+0x5c)&0x1fffffffu;
    const uint32_t renderer=g788::rd(ram,dm+0x64)&0x1fffffffu;
    if(!g788::ok(renderer,0x1100) || !g788::ok(p.riPool,0x2c) || !g788::ok(p.packetPool,0x2c))return false;
    const uint32_t config=g788::rd(ram,renderer+0xfcc)&0x1fffffffu;
    if(!g788::ok(config,0x90))return false;
    p.packetUsed=g788::rd(ram,p.packetPool+0x24);p.riUsed=g788::rd(ram,p.riPool+0x24);
    // Bound this experiment to ordinary physical pool pointers. Preserve legacy
    // address aliases exactly by refusing them, rather than normalizing tag data.
    if((g788::rd(ram,p.packetPool+0x20)&0x1fffffffu)!=g788::rd(ram,p.packetPool+0x20) ||
       (g788::rd(ram,p.riPool+0x20)&0x1fffffffu)!=g788::rd(ram,p.riPool+0x20))return false;
    p.packetStart=(g788::rd(ram,p.packetPool+0x20)+p.packetUsed*16u)&0x1fffffffu;
    p.riStart=(g788::rd(ram,p.riPool+0x20)+p.riUsed*16u)&0x1fffffffu;
    uint32_t bucket=g788::rd(ram,p.self+0x48)&0x1fffffffu,faces=0;
    const uint32_t materials=g788::rd(ram,p.self+0x44)&0x1fffffffu;
    for(unsigned n=0;bucket && n<256;++n) {
        if(!g788::ok(bucket,0x20))return false;
        if(!dc2_g788_econ_add_allowed(bucket)){++alreadyNative;return false;}
        Bucket b;b.address=bucket;
        if(!g788::ok(materials+g788::rd(ram,bucket)*0x30u,0x30))return false;
        uint32_t face=g788::rd(ram,bucket+4)&0x1fffffffu;
        for(unsigned f=0;face && f<256;++f) {
            if(!g788::ok(face,0x30) || ++faces>4096)return false;
            b.faces.push_back(face);face=g788::rd(ram,face+0x10)&0x1fffffffu;
        }
        if(face)return false;
        p.buckets.push_back(std::move(b));bucket=g788::rd(ram,bucket+8)&0x1fffffffu;
    }
    if(bucket || p.buckets.empty())return false;
    const size_t maxPacketQw=3u+p.buckets.size()*5u+faces*2u;
    const size_t maxRiQw=p.buckets.size()*16u+faces*3u;
    if(uint64_t(p.packetUsed)+maxPacketQw>=g788::rd(ram,p.packetPool+0x28) ||
       uint64_t(p.riUsed)+maxRiQw>=g788::rd(ram,p.riPool+0x28) ||
       !g788::ok(p.packetStart,maxPacketQw*16u+256u) || !g788::ok(p.riStart,maxRiQw*16u+256u))return false;
    p.generation=g788::g_pending.mesh->identity;
    const uint32_t prevTex=(getRegU32(ctx,28)+uint32_t(-30888))&0x1fffffffu;
    constexpr uint32_t giftag=0x334120u;
    if(!g788::ok(prevTex,4))return false;
    const uint32_t oldPrev=g788::rd(ram,prevTex),oldGift=g788::rd(ram,giftag),zero=0;
    std::memcpy(ram+prevTex,&zero,4);
    R5900Context leaf=*ctx;
    constexpr uint32_t ret=0x13fb08u;
    auto helper=[&](Fn fn,uint32_t pc,uint32_t destination,uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3,
                    std::vector<uint8_t>& output) {
        std::array<uint8_t,256> before{};
        if(mode()==1)std::memcpy(before.data(),ram+destination,before.size());
        reg(leaf,4,a0);reg(leaf,5,a1);reg(leaf,6,a2);reg(leaf,7,a3);reg(leaf,31,ret);leaf.pc=pc;
        fn(ram,&leaf,runtime);
        if(leaf.pc==pc)leaf.pc=ret; // same direct-call no-op convention as generated code
        const uint32_t qw=getRegU32(&leaf,2);
        if(leaf.pc!=ret || !qw || qw>16) {
            std::fprintf(stderr,"[G788:packet] FATAL nonleaf helper pc=%x end=%x qw=%u\n",pc,leaf.pc,qw);std::abort();
        }
        append(output,ram+destination,qw*16u);
        if(mode()==1)std::memcpy(ram+destination,before.data(),qw*16u);
        return qw;
    };
    helper(texflush,0x13e530u,p.packetStart,p.packetStart,0,0,0,p.chain);
    for(auto& b:p.buckets) {
        b.start=p.packetStart+uint32_t(p.chain.size());
        uint32_t riAt=p.riStart+uint32_t(p.ri.size());
        const uint32_t mat=materials+g788::rd(ram,b.address)*0x30u;
        const uint32_t matQw=helper(material,0x13e5f0u,riAt,p.self,riAt|0x20000000u,mat,g788::rd(ram,config+0x40),p.ri);
        tag(p.chain,0x30000000u|matQw,riAt);
        int32_t lastHeader=-1;
        for(uint32_t face:b.faces) {
            uint16_t header=0;std::memcpy(&header,ram+face,2);
            if(lastHeader!=header) {
                riAt=p.riStart+uint32_t(p.ri.size());
                const uint32_t qw=helper(pmode,0x13e740u,riAt,p.self,riAt|0x20000000u,header,0,p.ri);
                if(qw!=3){std::fprintf(stderr,"[G788:packet] FATAL pmode qw=%u\n",qw);std::abort();}
                tag(p.chain,0x30000003u,riAt);lastHeader=header;
            }
            append(p.chain,ram+face+0x20,16);
        }
        const uint32_t end=p.packetStart+uint32_t(p.chain.size());
        helper(texflush,0x13e530u,end,end,0,0,0,p.chain);tag(p.chain,0x60000000u);
        b.qw=(p.packetStart+uint32_t(p.chain.size())-b.start)/16u;
    }
    if(mode()==1) {
        std::memcpy(ram+prevTex,&oldPrev,4);std::memcpy(ram+giftag,&oldGift,4);
    }
    return true;
}
static void create(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    const auto& gp=g788::g_pending;
    if(ctx->pc!=0x13f920u || g_g788Mode!=3 || !gp.eligible || !gp.mesh ||
       gp.clipMode!=0 || gp.frame!=g788::sf() || gp.self!=(getRegU32(ctx,4)&0x1fffffffu)) {
        previous(ram,ctx,runtime);return;
    }
    // Never touch packet data for the accepted existing native architecture.
    if(gp.self==dc2_g784::kMeshSelf && dc2_g778::requested()) {
        ++alreadyNative;previous(ram,ctx,runtime);return;
    }
    ++attempted;
    for(const auto& b:pending.buckets)if(mode()==1 && !b.checked)++unpublishedBuckets;
    pending=Prediction{};
    const uint64_t start=dc2_g788_econ::on()?dc2_g788_econ::clockNs():0;
    Prediction p;
    if(!prepare(ram,ctx,runtime,p)){++refused;report();previous(ram,ctx,runtime);return;}
    if(start)prepareNs+=dc2_g788_econ::clockNs()-start;
    ++prepared;
    if(mode()==1){pending=std::move(p);report();previous(ram,ctx,runtime);return;}
    std::memcpy(ram+p.packetStart,p.chain.data(),p.chain.size());
    for(const auto& b:p.buckets) {
        std::memcpy(ram+b.address+0x10,&b.start,4);std::memcpy(ram+b.address+0x14,&b.qw,4);
    }
    const uint32_t packetUsed=p.packetUsed+uint32_t(p.chain.size()/16u),riUsed=p.riUsed+uint32_t(p.ri.size()/16u);
    std::memcpy(ram+p.packetPool+0x24,&packetUsed,4);std::memcpy(ram+p.riPool+0x24,&riUsed,4);
    ++converted;setReturnU32(ctx,p.packetStart);ctx->pc=getRegU32(ctx,31);report();
}
static void onAdd(uint8_t* ram,R5900Context* ctx) {
    if(mode()!=1 || pending.buckets.empty())return;
    const auto& gp=g788::g_pending;const uint32_t ra=getRegU32(ctx,31);
    if((ra!=g788::kRaAdd0 && ra!=g788::kRaAdd1) || !gp.eligible ||
       gp.frame!=pending.frame || gp.self!=pending.self)return;
    const uint32_t address=getRegU32(ctx,17)&0x1fffffffu;
    for(auto& b:pending.buckets)if(b.address==address) {
        const size_t offset=b.start-pending.packetStart;
        const bool bad=b.checked || !gp.mesh || gp.mesh->identity!=pending.generation ||
            g788::rd(ram,address+0x10)!=b.start || g788::rd(ram,address+0x14)!=b.qw ||
            std::memcmp(ram+b.start,pending.chain.data()+offset,b.qw*16u)!=0 ||
            std::memcmp(ram+pending.riStart,pending.ri.data(),pending.ri.size())!=0;
        if(bad){std::fprintf(stderr,"[G788:packet] FATAL oracle frame=%u visual=%x bucket=%x chain=%x qw=%u generation=%llx\n",
            gp.frame,gp.self,address,b.start,b.qw,(unsigned long long)pending.generation);std::abort();}
        if(oracleBuckets==0 || std::none_of(pending.buckets.begin(),pending.buckets.end(),[](const Bucket& x){return x.checked;}))++oracleDraws;
        b.checked=true;++oracleBuckets;checkedBytes+=b.qw*16u+pending.ri.size();return;
    }
}
}
static void g788_packet_on_add(uint8_t* ram,R5900Context* ctx){g788_packet::onAdd(ram,ctx);}
static void g788_packet_register(PS2Runtime& runtime) {
    if(!g788_packet::mode())return;
    using namespace g788_packet;
    previous=runtime.lookupFunction(0x13f920u);material=runtime.lookupFunction(0x13e5f0u);
    pmode=runtime.lookupFunction(0x13e740u);texflush=runtime.lookupFunction(0x13e530u);
    if(!previous || !material || !pmode || !texflush){std::fprintf(stderr,"[G788:packet] FATAL missing function\n");std::abort();}
    runtime.registerFunction(0x13f920u,&create);
    std::fprintf(stderr,"[G788:packet] armed mode=%d; existing native visual excluded\n",mode());
}
#else
static void g788_packet_on_add(uint8_t*,R5900Context*){}
static void g788_packet_register(PS2Runtime&){}
#endif
