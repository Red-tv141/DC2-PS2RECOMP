#if defined(PS2X_G777_NATIVE_GL)
#include "runtime/dc2_g780_native_item.h"

static bool g780GeometryEligible(const dc2_native::DC2NativeDraw& draw,uint32_t bucket,
                                 uint32_t* outMaterial=nullptr,uint32_t* outTriangles=nullptr)
{
    if(!draw.geometry || (draw.renderFlags&~3u))return false;
    uint32_t material=~0u,triangles=0;
    for(const auto& face:draw.geometry->faces){
        if(face.bucket!=bucket)continue;
        if(face.material>=draw.materials.size() || (face.topology!=3 && face.topology!=4) ||
           (face.topology==4 && face.batchCorners && face.indices.size()>face.batchCorners) ||
           (material!=~0u && material!=face.material))return false;
        material=face.material;
        for(uint32_t corner=2;corner<face.indices.size();++corner){
            if(face.topology==3 && corner%3!=2)continue;
            const uint32_t indices[]={corner-2,corner-1,corner};
            for(auto at:indices)if(face.indices[at]>=draw.geometry->vertices.size())return false;
            if(++triangles>65536u)return false;
        }
    }
    if(triangles==0)return false;
    if(outMaterial)*outMaterial=material;
    if(outTriangles)*outTriangles=triangles;
    return true;
}

bool GS::captureNativeE91F00(const std::shared_ptr<dc2_g778::Command>& command,
                             std::shared_ptr<const dc2_g778::G780NativeItem>& out,
                             bool deferPrim)
{
    uint32_t material=0,triangles=0;
    if(!command || !command->draw || !g178GpuOn() ||
       !g780GeometryEligible(*command->draw,command->bucket,&material,&triangles))return false;
    if(deferPrim && !g782_backend_ready_no_adopt())return false;
    auto item=std::make_shared<dc2_g778::G780NativeItem>();
    item->previousPrim=m_prim;
    item->previousCount=m_vtxCount;item->previousIndex=m_vtxIndex;
    // G782 predicts the exact PRIM write without mutating ordered parse state.
    // Only the winning native CAS may apply the write; legacy keeps the old PRIM.
    GSPrimReg selected=m_prim;
    if(deferPrim){
        selected.type=static_cast<GSPrimType>(0x5bu&7u);
        if(m_prmodecont){
            selected.iip=true;selected.tme=true;selected.fge=false;selected.abe=true;
            selected.aa1=false;selected.fst=false;selected.ctxt=false;selected.fix=false;
        }
    }else{writeRegister(GS_REG_PRIM,0x5b);selected=m_prim;}
    dc2_g778::captureG780GsState(*item,m_ctx[selected.ctxt?1:0],selected,
                                 m_texa,m_texclut,m_pabe,g_g364FogCol);
    const auto& c=item->ctx;
    if((c.frame.fbp!=0 && c.frame.fbp!=0x68) || c.frame.fbw!=8 ||
       c.frame.psm!=GS_PSM_CT32 || c.frame.fbmsk || c.fba || item->pabe ||
       !item->prim.tme || !item->prim.iip || item->prim.fst || item->prim.aa1 ||
       c.scissor.x1>511 || c.scissor.y1>511 ||
       c.scissor.x1<c.scissor.x0 || c.scissor.y1<c.scissor.y0 ||
       c.tex0.tbw==0 || c.tex0.tw>10 || c.tex0.th>10 ||
       (c.clamp&3u)>1u || ((c.clamp>>2u)&3u)>1u){
        if(!deferPrim){m_prim=item->previousPrim;m_vtxCount=item->previousCount;m_vtxIndex=item->previousIndex;}
        return false;
    }
    item->rasterizer=&m_rasterizer;
    item->vram=m_vram;item->vramSize=m_vramSize;
    item->draw=command->draw;item->bucket=command->bucket;
    if(deferPrim){
        std::array<dc2_native::Vector,23> constants{};
        if(!dc2_native::buildNativeConstants(*item->draw,material,triangles,512,512,
                                              c.xyoffset.ofx>>4,c.xyoffset.ofy>>4,constants))
            return false;
        if(!item->rasterizer->g782NativeEnvelopeEligible(*item))return false;
    }
#if defined(PS2X_G684_HOT_DIAG)
    item->future139WritesAtCapture=dc2_g778::g780Future139Writes.load(std::memory_order_acquire);
#endif
    out=std::move(item);
    return true;
}

void GS::commitNativeE91F00(){writeRegister(GS_REG_PRIM,0x5b);}

void GS::rollbackNativeE91F00(const dc2_g778::G780NativeItem& item)
{
    m_prim=item.previousPrim;m_vtxCount=item.previousCount;m_vtxIndex=item.previousIndex;
}

bool GSRasterizer::g782NativeEnvelopeEligible(const dc2_g778::G780NativeItem& item)
{
    // G633 attaches mutable executor-owned residency plans inside the classifier.
    // The narrow G782 contract declines this optional plan mode at G150.
    if(g_g633AttachTextureHook!=nullptr)return false;
    G144Entry envelope{};
    dc2_g778::fillG780Envelope(item,envelope);
    G178EntryState state{};
    if(!g178ClassifyEntry(envelope,state,true))return false;
    // g777Prepare's live backend supports only source-free blend 0/1 tris.
    return state.blend<=1u && item.prim.type==GS_PRIM_TRIANGLE;
}

bool GSRasterizer::nativeE91F00(const dc2_g778::G780NativeItem& item)
{
    if(!g178GpuOn() || !g178_backend_ready())return false;
    g778FlushExec();
    if(!g_g144List.empty() || dc2_g778::g780NativeDraw || dc2_g778::shadowCommand)return false;
    G144Entry envelope{};
    dc2_g778::fillG780Envelope(item,envelope);
    g_g144LastGs=nullptr;g_g144LastVram=item.vram;
    dc2_g778::g780NativeDraw=item.draw;dc2_g778::g780NativeBucket=item.bucket;
    g_g144List.push_back(std::move(envelope));
    const bool submitted=g178TryFlushGpu(this,nullptr,item.vram,item.vramSize);
    g_g144List.clear();dc2_g778::g780NativeDraw.reset();dc2_g778::g780NativeBucket=0;
    return submitted;
}

struct G780ExecPayload {
    std::shared_ptr<const dc2_g778::G780NativeItem> item;
    std::shared_ptr<dc2_g778::Command> g782Command;
#if defined(PS2X_G684_HOT_DIAG)
    std::chrono::steady_clock::time_point posted;
#endif
};
#if defined(PS2X_G684_HOT_DIAG)
static bool g780ProfileOn(){static const bool on=std::getenv("DC2_G780_PROFILE")!=nullptr;return on;}
static uint64_t g780Ns(std::chrono::steady_clock::time_point a,
                       std::chrono::steady_clock::time_point b){
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count());
}
struct G780CpuStats {
    std::atomic<uint64_t> eeWaitNs{0},eeCalls{0},parseQueueNs{0},parseWorkNs{0},
        parseFlushNs{0},postNs{0};
};
static G780CpuStats g780CpuStats;
#endif
static void g780ExecNative(void* value)
{
    std::unique_ptr<G780ExecPayload> payload(static_cast<G780ExecPayload*>(value));
    if(payload->g782Command){
        const auto choice=payload->g782Command->g782Commit->decision();
        if(choice==dc2_g782::Decision::LegacyCommitted)return; // lost CAS: no GPU side effect
        if(choice!=dc2_g782::Decision::NativeCommitted)std::abort();
    }
#if defined(PS2X_G684_HOT_DIAG)
    const auto execStart=g780ProfileOn()?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    static const int leadDelayUs=[](){const char* v=std::getenv("DC2_G780_LEAD_DELAY_US");return v?std::atoi(v):0;}();
    if(leadDelayUs>0)std::this_thread::sleep_for(std::chrono::microseconds(leadDelayUs));
    const auto future139=dc2_g778::g780Future139Writes.load(std::memory_order_acquire);
    if(future139>payload->item->future139WritesAtCapture){
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1u,std::memory_order_relaxed)<32u)
            std::fprintf(stderr,"[G780:lead] item_fbp=%u fbw=%u future139=%llu\n",
                         payload->item->ctx.frame.fbp,payload->item->ctx.frame.fbw,
                         static_cast<unsigned long long>(future139-payload->item->future139WritesAtCapture));
    }
#endif
    bool submitted=false;
    try{submitted=payload->item->rasterizer->nativeE91F00(*payload->item);}
    catch(...){++dc2_g778::counters.transportFailures;}
#if defined(PS2X_G684_HOT_DIAG)
    if(g780ProfileOn()){
        static uint64_t n=0,queueNs=0,execNs=0;
        const auto end=std::chrono::steady_clock::now();
        ++n;queueNs+=g780Ns(payload->posted,execStart);execNs+=g780Ns(execStart,end);
        if((n%128u)==0u){
            const auto& s=g780CpuStats;
            const auto ee=s.eeCalls.load(std::memory_order_relaxed);
            std::fprintf(stderr,"[G780:cpu] n=%llu ee=%llu eeWaitMs=%.3f parseQueueMs=%.3f parseWorkMs=%.3f flushMs=%.3f postMs=%.3f execQueueMs=%.3f execMs=%.3f\n",
                static_cast<unsigned long long>(n),static_cast<unsigned long long>(ee),
                ee?double(s.eeWaitNs.load(std::memory_order_relaxed))/1e6/ee:0.,
                ee?double(s.parseQueueNs.load(std::memory_order_relaxed))/1e6/ee:0.,
                ee?double(s.parseWorkNs.load(std::memory_order_relaxed))/1e6/ee:0.,
                double(s.parseFlushNs.load(std::memory_order_relaxed))/1e6/n,
                double(s.postNs.load(std::memory_order_relaxed))/1e6/n,
                double(queueNs)/1e6/n,double(execNs)/1e6/n);
        }
    }
#endif
    if(!submitted){
        std::fprintf(stderr,"[G780:late-failure] fbp=%u fbw=%u bucket=%x\n",
                     payload->item->ctx.frame.fbp,payload->item->ctx.frame.fbw,payload->item->bucket);
        std::abort();
    }
    if(payload->g782Command)++dc2_g778::counters.g782NativeComplete;
}

struct G780AdmissionJob {
    GS* gs;std::shared_ptr<dc2_g778::Command> command;
    std::mutex mutex;std::condition_variable ready;bool done=false,result=false;
#if defined(PS2X_G684_HOT_DIAG)
    std::chrono::steady_clock::time_point enqueued;
#endif
};
#endif
