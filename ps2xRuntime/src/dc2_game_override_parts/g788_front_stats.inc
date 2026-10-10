// Read-only prerequisites for front-end caching. RI completion is observed at
// CreatePacket entry, not at a possibly-preempted CreateRenderInfoPacket return.
#if defined(PS2X_G777_NATIVE_GL)
namespace g788_front {
static bool on(){return dc2_g788_econ::on() && std::getenv("DC2_G788_FRONT")!=nullptr;}
using Fn=g788::RecompiledFunction;
static Fn previous=nullptr;
static uint32_t frame=~0u,self=0,riAt=0,riBytes=0,matrixBytes=0;
static uint64_t frames=0,generated=0,bytes=0,replays=0,replayBytes=0,matrixReplayBytes=0,
    uniqueBodies=0,uniqueNormalizedBodies=0,uniqueMatrices=0,uniqueWorldMatrices=0,
    invalid=0,alreadyNative=0,materialRefs=0,materialBytes=0,pmodeRefs=0,pmodeBytes=0,
    texflushPackets=0,texflushBytes=0,faces=0;
struct PreviousRI { uint64_t generation=0;std::string body,normalized,matrix,world; };
static std::unordered_map<uint32_t,PreviousRI> temporal;
static uint64_t temporalSeen=0,sameBody=0,sameNormalized=0,sameMatrix=0,sameWorld=0;
static std::unordered_map<std::string,bool> bodies,normalizedBodies,matrices,worldMatrices;
static bool eligible(uint8_t* ram) {
    const auto& p=g788::g_pending;
    if(g_g788Mode!=3 || !p.eligible || !p.mesh || p.clipMode!=0 || p.frame!=g788::sf())return false;
    if(p.self==dc2_g784::kMeshSelf && dc2_g778::requested()){++alreadyNative;return false;}
    if(p.entry)for(const auto& b:p.entry->buckets)if(!dc2_g788_econ_add_allowed(b.first)){++alreadyNative;return false;}
    return true;
}
static void closeFrame() {
    if(frame==~0u)return;
    ++frames;uniqueBodies+=bodies.size();uniqueNormalizedBodies+=normalizedBodies.size();
    uniqueMatrices+=matrices.size();uniqueWorldMatrices+=worldMatrices.size();
    bodies.clear();normalizedBodies.clear();matrices.clear();worldMatrices.clear();
}
static void capture(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    if(ctx->pc==0x13f920u && eligible(ram) && (getRegU32(ctx,4)&0x1fffffffu)==g788::g_pending.self) {
        const auto& p=g788::g_pending;
        const uint32_t pool=g788::rd(ram,p.manager+0x60)&0x1fffffffu;
        const uint32_t end=g788::ok(pool,0x2c)?
            (g788::rd(ram,pool+0x20)+g788::rd(ram,pool+0x24)*16u)&0x1fffffffu:0u;
        const uint32_t start=p.renderInfo;
        if(end<=start || end-start>4096 || (end-start)%16 || !g788::ok(start,end-start))++invalid;
        else {
            if(frame!=p.frame){closeFrame();frame=p.frame;}
            self=p.self;riAt=start;riBytes=end-start;
            ++generated;bytes+=riBytes;
            std::string raw(reinterpret_cast<const char*>(ram+start),riBytes);
            bodies.emplace(raw,true);
            // BASE/OFFSET words are per-mesh VIF layout metadata, not matrices.
            // Normalization is diagnostic grouping only; no packet is changed.
            std::string normalized=raw;
            const uint32_t zero=0;
            if(normalized.size()>=28){std::memcpy(normalized.data()+20,&zero,4);std::memcpy(normalized.data()+24,&zero,4);}
            normalizedBodies.emplace(normalized,true);
            std::string matrix,world;
            const uint64_t first=g788::rd(ram,start);
            const uint32_t firstQw=uint32_t(first)&0xffffu;
            if(((first>>28)&7u)!=1u || (firstQw+1u)*16u>riBytes){++invalid;matrixBytes=0;}
            else {
                matrixBytes=firstQw*16u;
                matrix=normalized.substr(0,(firstQw+1u)*16u);
                matrices.emplace(matrix,true);
            }
            if(g788::ok(p.worldPtr,64)) {
                world.assign(reinterpret_cast<const char*>(ram+p.worldPtr),64);
                worldMatrices.emplace(world,true);
            }
            auto& old=temporal[p.self];
            if(old.generation==p.mesh->identity && !old.body.empty()) {
                ++temporalSeen;
                sameBody+=old.body==raw;sameNormalized+=old.normalized==normalized;
                sameMatrix+=!matrix.empty() && old.matrix==matrix;
                sameWorld+=!world.empty() && old.world==world;
            }
            old={p.mesh->identity,std::move(raw),std::move(normalized),std::move(matrix),std::move(world)};
        }
    }
    previous(ram,ctx,runtime);
}
static void onAdd(uint8_t* ram,R5900Context* ctx) {
    if(!on() || !eligible(ram))return;
    const auto& p=g788::g_pending;const uint32_t ra=getRegU32(ctx,31);
    if((ra!=g788::kRaAdd0 && ra!=g788::kRaAdd1) || p.frame!=frame || p.self!=self || p.renderInfo!=riAt)return;
    ++replays;replayBytes+=riBytes;matrixReplayBytes+=matrixBytes;
    const uint32_t bucket=getRegU32(ctx,17)&0x1fffffffu;
    if(!g788::ok(bucket,0x20)){++invalid;return;}
    const uint32_t chain=g788::rd(ram,bucket+0x10)&0x1fffffffu,count=g788::rd(ram,bucket+0x14);
    if(!count || count>4096 || !g788::ok(chain,count*16u)){++invalid;return;}
    uint32_t face=g788::rd(ram,bucket+4)&0x1fffffffu,q=0;
    while(q<count) {
        const uint32_t at=chain+q*16u;
        if(face && g788::ok(face,0x30) && !std::memcmp(ram+at,ram+face+0x20,16)) {
            ++faces;face=g788::rd(ram,face+0x10)&0x1fffffffu;++q;continue;
        }
        const uint32_t lo=g788::rd(ram,at),id=(lo>>28)&7u,qw=lo&0xffffu;
        if(id==3u) {
            if(q==0){++materialRefs;materialBytes+=qw*16u;}
            else if(qw==3u){++pmodeRefs;pmodeBytes+=48u;}
            else ++invalid;
            ++q;
        } else if(id==1u) {++texflushPackets;texflushBytes+=qw*16u;q+=1u+qw;}
        else if(id==6u){++q;break;}
        else {++invalid;break;}
    }
    if(face || q!=count)++invalid;
}
static void report(uint32_t n,uint32_t sf) {
    std::fprintf(stderr,"[G788:front] n=%u sf=%u frames=%llu generated=%llu bytes=%llu replays=%llu replayBytes=%llu matrixReplayBytes=%llu uniqueBodies=%llu uniqueNormalizedBodies=%llu uniqueMatrices=%llu uniqueWorldMatrices=%llu invalid=%llu alreadyNative=%llu materialRefs=%llu materialBytes=%llu pmodeRefs=%llu pmodeBytes=%llu texflushPackets=%llu texflushBytes=%llu faces=%llu\n",
        n,sf,(unsigned long long)frames,(unsigned long long)generated,(unsigned long long)bytes,
        (unsigned long long)replays,(unsigned long long)replayBytes,(unsigned long long)matrixReplayBytes,
        (unsigned long long)uniqueBodies,(unsigned long long)uniqueNormalizedBodies,
        (unsigned long long)uniqueMatrices,(unsigned long long)uniqueWorldMatrices,
        (unsigned long long)invalid,(unsigned long long)alreadyNative,
        (unsigned long long)materialRefs,(unsigned long long)materialBytes,(unsigned long long)pmodeRefs,
        (unsigned long long)pmodeBytes,(unsigned long long)texflushPackets,(unsigned long long)texflushBytes,
        (unsigned long long)faces);
    std::fprintf(stderr,"[G788:ri-reuse] n=%u sf=%u seen=%llu sameBody=%llu sameNormalized=%llu sameMatrix=%llu sameWorld=%llu\n",
        n,sf,(unsigned long long)temporalSeen,(unsigned long long)sameBody,
        (unsigned long long)sameNormalized,(unsigned long long)sameMatrix,(unsigned long long)sameWorld);
}
}
static void g788_front_on_add(uint8_t* ram,R5900Context* ctx){g788_front::onAdd(ram,ctx);}
static void g788_front_register(PS2Runtime& runtime) {
    if(!g788_front::on())return;
    g788_front::previous=runtime.lookupFunction(0x13f920u);
    if(!g788_front::previous){std::fprintf(stderr,"[G788:front] FATAL missing constructor\n");std::abort();}
    runtime.registerFunction(0x13f920u,&g788_front::capture);
}
#else
static void g788_front_on_add(uint8_t*,R5900Context*){}
static void g788_front_register(PS2Runtime&){}
namespace g788_front {static void report(uint32_t,uint32_t){}}
#endif
