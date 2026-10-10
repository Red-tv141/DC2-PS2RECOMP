// Diagnostic wrappers cover every registered entry AND resume PC. Original callbacks
// are saved individually; generated code and dispatch/guest return semantics are unchanged.
namespace g788_econ {
static void memory(uint32_t n)
{
    uint64_t cache=0,source=0,nativeMeshes=0,drawBytes=0;
    for(const auto& pair:g788::g_registry) {
        const auto& e=pair.second;cache+=sizeof(e)+e.faceHold.capacity();
        for(const auto& a:e.source.arrays)cache+=a.capacity();
        for(const auto& b:e.buckets) {
            cache+=sizeof(b)+b.second.faces.capacity()*sizeof(g788::FrozenFace);
            for(const auto& f:b.second.faces)cache+=f.indices.capacity();
        }
        cache+=e.bucketFaceBase.size()*sizeof(std::pair<const uint32_t,uint32_t>);
        if(e.mesh) {
            cache+=sizeof(*e.mesh)+e.mesh->vertices.capacity()*sizeof(dc2_native::Vertex)+e.mesh->faces.capacity()*sizeof(dc2_native::Face);
            for(const auto& f:e.mesh->faces)cache+=f.indices.capacity()*sizeof(uint32_t);
        }
        if(e.gpu) {
            ++nativeMeshes;source+=e.gpu->source.size()*sizeof(dc2_native::Vector);
            cache+=sizeof(*e.gpu)+e.gpu->source.capacity()*sizeof(dc2_native::Vector)+e.gpu->faceTri.capacity()*sizeof(uint32_t);
        }
    }
    std::unordered_map<const G788DrawRec*,bool> records;
    for(const auto& job:g_g788Ring)if(job.rec)records.emplace(job.rec.get(),true);
    for(const auto& pair:records) {
        const auto& rec=*pair.first;
        drawBytes+=sizeof(rec)+rec.constants.capacity()*sizeof(std::array<dc2_native::Vector,21>)+
            rec.draw.materials.capacity()*sizeof(dc2_native::Material);
    }
    std::fprintf(stderr,"[G788:memory] n=%u registry=%llu nativeMeshes=%llu trackedCpuCacheBytes=%llu staticSourceUsedBytes=%llu ringBytes=%llu liveDrawRecords=%llu drawDescriptorBytes=%llu gpuStaticCapacity=%llu gpuOutputCapacity=%llu gpuDescriptorCapacity=%llu cpuJobCapacity=%llu gpuIndexBytes=0\n",
        n,(unsigned long long)g788::g_registry.size(),(unsigned long long)nativeMeshes,(unsigned long long)cache,
        (unsigned long long)source,(unsigned long long)sizeof(g_g788Ring),(unsigned long long)records.size(),
        (unsigned long long)drawBytes,(unsigned long long)dc2_g788_econ::gpuStaticCapacity.load(),
        (unsigned long long)dc2_g788_econ::gpuOutputCapacity.load(),(unsigned long long)dc2_g788_econ::gpuDescriptorCapacity.load(),
        (unsigned long long)dc2_g788_econ::cpuJobCapacity.load());
}
using Fn=void(*)(uint8_t*,R5900Context*,PS2Runtime*);
#if defined(PS2X_G775_DIAG)
static Fn priorFaceCensus=nullptr;
static void faceCensus(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    using namespace dc2_g788_econ;
    const uint32_t self=getRegU32(ctx,4)&0x1fffffffu;
    if(g788::ok(self,0x50) && g788::rd(ram,self+0x1c)==g788::kFixVtable) {
        bool owner=self==dc2_g784::kMeshSelf && dc2_g778::requested(),valid=true;
        std::array<uint32_t,256> buckets{};unsigned count=0;
        uint32_t b=g788::rd(ram,self+0x48)&0x1fffffffu;
        while(!owner && b && count<buckets.size()) {
            if(!g788::ok(b,0x20)){valid=false;break;}
            if(!dc2_g788_econ_add_allowed(b)){owner=true;break;}
            buckets[count++]=b;b=g788::rd(ram,b+8)&0x1fffffffu;
        }
        if(owner)event(FixOwnerExcluded);
        else {
            if(b)valid=false;
            uint64_t faces=0;
            for(unsigned i=0;valid && i<count;++i) {
                uint32_t f=g788::rd(ram,buckets[i]+4)&0x1fffffffu;unsigned n=0;
                while(f && n++<256) {
                    if(!g788::ok(f,0x30)){valid=false;break;}
                    ++faces;f=g788::rd(ram,f+0x10)&0x1fffffffu;
                }
                if(f)valid=false;
            }
            if(valid){event(FixLogicalDraws);event(FixLogicalFaces,faces);}
            else event(FixCensusBad);
        }
    }
    priorFaceCensus(ram,ctx,runtime);
}
#endif
static std::unordered_map<uint32_t,Fn> previous[dc2_g788_econ::StageCount];
struct HelperCall { uint32_t ra; bool eligible; };
static std::unordered_map<uint32_t,HelperCall> helperCalls[dc2_g788_econ::StageCount];
template<unsigned Stage,uint32_t Entry,unsigned FrameBytes>
static void helper(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    const uint32_t pc=ctx->pc;
    const bool entry=pc==Entry;
    const uint32_t key=getRegU32(ctx,29)+(entry?0u:FrameBytes);
    auto& calls=helperCalls[Stage];
    if(entry) {
        bool eligible=false;
        for(auto* s=dc2_g788_econ::current;s;s=s->parent)
            if(s->enabled && s->stage==dc2_g788_econ::RenderInfo)eligible=true;
        calls[key]={getRegU32(ctx,31),eligible};
    }
    const auto call=calls.find(key);
    if(call==calls.end()){std::fprintf(stderr,"[G788:econ] FATAL unknown helper resume pc=%x sp=%x\n",pc,key);std::abort();}
    const HelperCall saved=call->second;
    const auto old=previous[Stage].find(pc);
    if(old==previous[Stage].end()){std::fprintf(stderr,"[G788:econ] FATAL missing helper pc=%x\n",pc);std::abort();}
    dc2_g788_econ::Scope scope(Stage,entry,saved.eligible);
    old->second(ram,ctx,runtime);
    if(ctx->pc==saved.ra)calls.erase(key);
}
template<unsigned Stage,uint32_t Entry,unsigned FrameBytes>
static void bindHelper(PS2Runtime& runtime,std::initializer_list<uint32_t> pcs)
{
    for(uint32_t pc:pcs) {
        Fn old=runtime.lookupFunction(pc);
        if(!old){std::fprintf(stderr,"[G788:econ] FATAL missing helper registration pc=%x\n",pc);std::abort();}
        previous[Stage].emplace(pc,old);
        runtime.registerFunction(pc,&helper<Stage,Entry,FrameBytes>);
    }
}
template<unsigned Stage,uint32_t Entry>
static void guest(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    const uint32_t pc=ctx->pc;
    const bool entry=pc==Entry;
    bool eligible=true;
    if constexpr(Stage==dc2_g788_econ::MaterialBuild || Stage==dc2_g788_econ::PmodeBuild || Stage==dc2_g788_econ::TexflushBuild) {
        const uint32_t ra=getRegU32(ctx,31);
        eligible=ra>=0x13f920u && ra<0x13fb48u;
    }
    if constexpr(Stage==dc2_g788_econ::RenderInfo) {
        // This is the actual this pointer saved by 0x140500 before any resume edge.
        const uint32_t sp=getRegU32(ctx,29)&0x1fffffffu;
        const uint32_t self=(entry?getRegU32(ctx,4):g788::rd(ram,sp+172))&0x1fffffffu;
        eligible=g788::ok(self,0x20) && g788::rd(ram,self+0x1c)==g788::kFixVtable;
    }
    if constexpr(Stage==dc2_g788_econ::AddPacket) {
        const uint32_t sp=getRegU32(ctx,29)&0x1fffffffu;
        const uint32_t ra=entry?getRegU32(ctx,31):g788::rd(ram,sp+80);
        const uint32_t bucket=(entry?getRegU32(ctx,17):g788::rd(ram,sp+16))&0x1fffffffu;
        eligible=(ra==g788::kRaAdd0 || ra==g788::kRaAdd1) && dc2_g788_econ_add_allowed(bucket);
    }
    const auto it=previous[Stage].find(pc);
    if(it==previous[Stage].end()){std::fprintf(stderr,"[G788:econ] FATAL unknown resume pc=%x\n",pc);std::abort();}
    dc2_g788_econ::Scope scope(Stage,entry,eligible);
    it->second(ram,ctx,runtime);
}
template<unsigned Stage,uint32_t Entry>
static void bind(PS2Runtime& runtime,std::initializer_list<uint32_t> pcs)
{
    for(uint32_t pc:pcs) {
        Fn old=runtime.lookupFunction(pc);
        if(!old){std::fprintf(stderr,"[G788:econ] FATAL missing registered pc=%x\n",pc);std::abort();}
        previous[Stage].emplace(pc,old);
        runtime.registerFunction(pc,&guest<Stage,Entry>);
    }
}
}
static void g788_econ_register(PS2Runtime& runtime)
{
    if(!dc2_g788_econ::on())return;
#if defined(PS2X_G775_DIAG)
    g788_econ::priorFaceCensus=runtime.lookupFunction(g788::kDrawSeam);
    if(!g788_econ::priorFaceCensus){std::fprintf(stderr,"[G788:econ] FATAL no draw entry\n");std::abort();}
    runtime.registerFunction(g788::kDrawSeam,&g788_econ::faceCensus);
#endif
    // G788_ECON_PC_BINDINGS_BEGIN
    g788_econ::bind<dc2_g788_econ::RenderInfo,0x1404d0u>(runtime,{0x1404d0u,0x140548u,0x140558u,0x1405a8u,0x1405d0u,0x1405e0u,0x1405f4u,0x140658u,0x140698u,0x1406a4u,0x140764u,0x140770u,0x140780u,0x1407c4u,0x1407d0u,0x140880u,0x14089cu,0x1408b4u,0x1408d8u,0x140920u,0x140b28u,0x140b50u,0x140ba4u});
    g788_econ::bind<dc2_g788_econ::CreatePacket,0x13f920u>(runtime,{0x13f920u,0x13f958u,0x13f99cu,0x13f9acu,0x13f9e0u,0x13fa14u,0x13fa30u,0x13fa80u});
    g788_econ::bind<dc2_g788_econ::AddPacket,0x1359d0u>(runtime,{0x1359d0u,0x135a68u});
    g788_econ::bind<dc2_g788_econ::PreEndDraw,0x1354f0u>(runtime,{0x1354f0u,0x135530u,0x135544u,0x135578u,0x1355c4u,0x1355e4u});
    g788_econ::bind<dc2_g788_econ::ReloadTexture,0x135660u>(runtime,{0x135660u,0x1356e8u,0x1356fcu});
    g788_econ::bind<dc2_g788_econ::MaterialBuild,0x13e5f0u>(runtime,{0x13e5f0u});
    g788_econ::bind<dc2_g788_econ::PmodeBuild,0x13e740u>(runtime,{0x13e740u});
    g788_econ::bind<dc2_g788_econ::TexflushBuild,0x13e530u>(runtime,{0x13e530u});
    g788_econ::bindHelper<dc2_g788_econ::MatrixHelper,0x130250u,0u>(runtime,{0x130250u});
    g788_econ::bindHelper<dc2_g788_econ::DrawenvBuild,0x13e850u,48u>(runtime,{0x13e850u,0x13e874u,0x13ea90u,0x13eaa0u});
    g788_econ::bindHelper<dc2_g788_econ::SendDma,0x13e3d0u,48u>(runtime,{0x13e3d0u,0x13e3fcu,0x13e434u});
    // G788_ECON_PC_BINDINGS_END
}
