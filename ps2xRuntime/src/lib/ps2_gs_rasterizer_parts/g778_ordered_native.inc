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
       !g780GeometryEligible(*command->draw,command->bucket,&material,&triangles)){
        if(deferPrim)++dc2_g778::counters.g782RejCapture;
        return false;
    }
    if(deferPrim && !g782_backend_ready_no_adopt()){++dc2_g778::counters.g782RejProcess;return false;}
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
    // G784: an inter-face PRMODE inside the group (multi-face buckets) must be a no-op: with
    // PRMODECONT=0 it rewrites exactly these attribute bits, so they must already hold its value.
    if(command->interPrmode && !m_prmodecont){
        const uint64_t v=command->prmode;
        if(selected.iip!=bool((v>>3)&1) || selected.tme!=bool((v>>4)&1) || selected.fge!=bool((v>>5)&1) ||
           selected.abe!=bool((v>>6)&1) || selected.aa1!=bool((v>>7)&1) || selected.fst!=bool((v>>8)&1) ||
           selected.ctxt!=bool((v>>9)&1) || selected.fix!=bool((v>>10)&1)){
            if(deferPrim)++dc2_g778::counters.g782RejCapture;
            else{m_prim=item->previousPrim;m_vtxCount=item->previousCount;m_vtxIndex=item->previousIndex;}
            return false;
        }
    }
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
        else ++dc2_g778::counters.g782RejCapture;
        return false;
    }
    item->rasterizer=&m_rasterizer;
    item->vram=m_vram;item->vramSize=m_vramSize;
    item->draw=command->draw;item->bucket=command->bucket;
    if(deferPrim){
        std::array<dc2_native::Vector,23> constants{};
        if(!dc2_native::buildNativeConstants(*item->draw,material,triangles,512,512,
                                              c.xyoffset.ofx>>4,c.xyoffset.ofy>>4,constants)){
            ++dc2_g778::counters.g782RejConstants;
            return false;
        }
        // The live register pair of both contexts is already in the page set from
        // its writes; repeat it so GS reset state is covered without a writeRegister.
        for(const auto& ctx:m_ctx)dc2_g778::g782NoteTargetRegs(ctx.frame.fbp,ctx.frame.fbw,ctx.frame.psm,ctx.zbuf);
        if(!item->rasterizer->g782NativeEnvelopeEligible(*item))return false;
        ++dc2_g778::counters.g782Eligible;
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

// ===== G782: THE FINAL G150 CONTRACT ===================================================
// A winning tryNative() is the transaction boundary: after it legacy can no longer run, so
// g178TryFlushGpu/renderBatch/g777Prepare must have no NORMAL refusal left for this item.
// plans/phase-G782-ledger.md section 2 classifies every executor refusal site for the
// E91F00 envelope (FBP 0/0x68, FBW 8, CT32, one TRIANGLE entry, TME, blend 0/1):
//   A  pure functions of the immutable item        -> proved here, before the CAS
//   B  unreachable for this envelope / deterministic -> no test needed
//   C  process configuration                       -> g782ProcessContract (cached)
//   D  GL/driver failure                           -> catastrophic, fatal diagnostic
// Every clause below is conservative: it may refuse a draw the executor would accept
// (coverage only), never accept one the executor would refuse.
namespace {
// Pages the GPU executor can come to own: every FRAME/ZBUF surface ever configured
// and every local->local destination, whole-surface (512 rows), in parse order.
// Written and read on the G150 parse thread only; atomics keep a stray EE-side
// writeRegister (G150 disarmed, so no reader) race-free.
std::array<std::atomic<uint64_t>,8> g_g782GpuPages{};

void g782MarkRange(const G144BlockRange& r)
{
    if(!r.valid)return;
    const uint32_t first=r.first>>5u,last=std::min<uint32_t>(r.last>>5u,511u);
    if(r.last>=0x3FFFu){ // clamped at the VRAM end: the GS wraps, so own everything
        for(auto& word:g_g782GpuPages)word.store(~0ull,std::memory_order_relaxed);
        return;
    }
    for(uint32_t page=first;page<=last;++page){
        auto& word=g_g782GpuPages[page>>6u];
        const uint64_t bit=1ull<<(page&63u);
        if(!(word.load(std::memory_order_relaxed)&bit))word.fetch_or(bit,std::memory_order_relaxed);
    }
}

bool g782RangeMaybeGpuOwned(const G144BlockRange& r)
{
    if(!r.valid)return false;
    const uint32_t first=r.first>>5u,last=std::min<uint32_t>(r.last>>5u,511u);
    for(uint32_t page=first;page<=last;++page)
        if(g_g782GpuPages[page>>6u].load(std::memory_order_relaxed)&(1ull<<(page&63u)))return true;
    return false;
}

bool g782ProcessContract()
{
    // Verification oracles refuse a whole flush after drawing it (TryFlush tail), and the
    // opt-in G242 ownership model adds three depth-sync refusals. Neither runs while the
    // nonblocking path is armed.
    static const bool ok=!g242GpuDepthOn() && !g255VerifyOn() && !g286VerifyOn() &&
        !g570Verify13dOn() && !g570Verify139On() && !g583Verify13bOn() &&
        !g586Verify141On() && !g587Verify15dOn() && !g604VerifyDispOn() &&
        !g604VerifyDiscovOn() && !g712VerifyOn()
#if defined(PS2X_G654_GPUDOMAIN)
        // G630 linear views are the one FBO-source binder without a PSM/FST gate. Shipping
        // builds compile the whole G629-G636 domain out (g654_stub_gpudomain.inc).
        && !s_g636LinView
#endif
        ;
    // G633 attaches mutable executor-owned residency plans inside the classifier.
    return ok && g_g633AttachTextureHook==nullptr;
}
}

void dc2_g778::g782NoteTargetRegs(uint32_t fbp,uint32_t fbw,uint32_t fpsm,uint64_t zbuf)
{
    dc2_g783::Scope g783(dc2_g783::GsNote);
    // G783: marking only ever sets page bits (never cleared), so a surface already marked is
    // a no-op. Per calling thread (G150 parse; the G780 capture runs there too).
    thread_local std::array<std::pair<uint64_t,uint64_t>,64> seen{};
    thread_local std::array<bool,64> used{};
    const uint64_t key=uint64_t(fbp)|(uint64_t(fbw)<<16)|(uint64_t(fpsm)<<32);
    const uint32_t slot=uint32_t(((key*0x9E3779B97F4A7C15ull)^(zbuf*0xC2B2AE3D27D4EB4Full))>>58);
    if(used[slot] && seen[slot].first==key && seen[slot].second==zbuf)return;
    used[slot]=true;seen[slot]={key,zbuf};
    const uint32_t width=std::max<uint32_t>(fbw,1u)*64u;
    g782MarkRange(g144RangeForRect(framePageBaseToBlock(fbp),static_cast<uint8_t>(fpsm),
                                   fbw,0u,0u,width,512u));
    const uint32_t zbp=static_cast<uint32_t>(zbuf&0x1FFu);
    const uint8_t zpsm=static_cast<uint8_t>(0x30u|((zbuf>>24u)&0xFu));
    g782MarkRange(g144RangeForRect(framePageBaseToBlock(zbp),zpsm,fbw,0u,0u,width,512u));
}

void dc2_g778::g782NoteLocalCopy(uint32_t dbp,uint32_t dbw,uint32_t dpsm,uint32_t dsax,
                                 uint32_t dsay,uint32_t rrw,uint32_t rrh)
{
    dc2_g783::Scope g783(dc2_g783::GsNote);
    // Whole rows: a destination wider than DBW wraps inside its row band.
    const uint32_t width=std::max(std::max<uint32_t>(dbw,1u)*64u,dsax+rrw);
    g782MarkRange(g144RangeForRect(dbp,static_cast<uint8_t>(dpsm),dbw,0u,dsay,width,rrh));
}

bool GSRasterizer::g782NativeEnvelopeEligible(const dc2_g778::G780NativeItem& item)
{
    auto& counters=dc2_g778::counters;
    if(!g782ProcessContract()){++counters.g782RejProcess;return false;}
    G144Entry envelope{};
    dc2_g778::fillG780Envelope(item,envelope);
    // A: the classifier is a pure function of the envelope plus cached process flags. The
    // executor re-runs it on the same immutable envelope; its only non-dry differences
    // (t_g603RelaxBlend/t_g604RelaxDepth) relax, so dry acceptance implies executor
    // acceptance with the identical blend/depth state.
    G178EntryState state{};
    if(!g178ClassifyEntry(envelope,state,true) || state.skip ||
       state.blend>1u || item.prim.type!=GS_PRIM_TRIANGLE){
        ++counters.g782RejClassify;return false;
    }
    const auto& c=item.ctx;
    const uint32_t fbp=c.frame.fbp,fbw=c.frame.fbw,fbBlock=framePageBaseToBlock(fbp);
    const uint32_t fbW=fbw*64u;
    // The executor's colour and depth rows for one entry are its scissor rows (TryFlush
    // st.scY0..scY1); every later narrowing (G282/G290) only shrinks them.
    const uint32_t rowLo=state.scY0,rows=uint32_t(state.scY1)-state.scY0+1u;
    if(state.guestDepth){
        // TryFlush kG266FrDepthSetup: depth must be Z24 with a non-empty row window.
        if(state.zpsm!=1u || state.scY1<state.scY0){++counters.g782RejDepthAlias;return false;}
        const uint32_t zbpBlock=uint32_t(state.zbpPage)<<5u;
        // TryFlush colorZAlias, exact form for the measured G273 pair, conservative else;
        // its G282/G290/transient exemptions are ignored (refuse more, never less).
        static const bool exactAlias=!envFlagEnabled("DC2_G273_NO_EXACT_COLORZ_ALIAS") &&
                                     !envFlagDisabled("DC2_G273_EXACT_COLORZ_ALIAS") &&
                                     g265DisplayZWaveOn();
        const bool measuredPair=exactAlias && fbp==0x68u && fbBlock==0x0d00u &&
                                zbpBlock==0x1a00u && fbw==8u;
        const bool colorZ=measuredPair
            ?g273AlignedRectsOverlapExact(fbBlock,fbw,GS_PSM_CT32,0u,rowLo,fbW,rows,
                                          zbpBlock,fbw,GS_PSM_Z24,0u,rowLo,fbW,rows)
            :g242RectsOverlap(fbBlock,fbw,GS_PSM_CT32,0u,rowLo,fbW,rows,
                              zbpBlock,fbw,GS_PSM_Z24,0u,rowLo,fbW,rows);
        if(colorZ){++counters.g782RejDepthAlias;return false;}
        // TryFlush kG266FrTexAliasZ; the G568/G702 exemptions are ignored.
        const bool texZ=g242RectsOverlap(c.tex0.tbp0,c.tex0.tbw,c.tex0.psm,0u,0u,
                                         uint32_t(state.texW),uint32_t(state.texH),
                                         zbpBlock,fbw,GS_PSM_Z24,0u,rowLo,fbW,rows);
        const bool clutZ=g144IsPalettedPsm(c.tex0.psm) &&
            g242RectsOverlap(c.tex0.cbp,1u,GS_PSM_CT32,0u,0u,64u,32u,
                             zbpBlock,fbw,GS_PSM_Z24,0u,rowLo,fbW,rows);
        if(texZ || clutZ){++counters.g782RejTexAlias;return false;}
    }
    // Texture-source authority (G700/G718/G289/...): g777Prepare must refuse a draw the
    // executor binds from a producer FBO (srcFbp), because the front end T-flips such a
    // draw's CPU vertices and the native GPU vertices carry no flip. Every srcFbp binder in
    // TryFlush is gated on PRIM.FST (G289 direct/owner, G309/G310, G342, G280, the G248/G766
    // resident loop) or on a direct-colour texture PSM equal to a surface PSM (G623 CT32,
    // G700 s.psm==tpsm); G630 is excluded by the process contract. Native PRIM.FST is 0, so a
    // PALETTED texture can never be FBO-sourced. A direct-colour texture is admitted only
    // outside every page the GPU could own (whole-surface FRAME/ZBUF + L2L destinations
    // parsed before this marker). The CLUT is never an FBO source and never a refusal.
    const G144BlockRange tex=g144TextureRange(envelope);
    if(item.prim.fst || !tex.valid ||
       (!g144IsPalettedPsm(c.tex0.psm) && g782RangeMaybeGpuOwned(tex))){
        ++counters.g782RejAuthority;
        if(dc2_g778::g782ReportRequested()){
            static std::atomic<uint32_t> logged{0};
            if(logged.fetch_add(1u,std::memory_order_relaxed)<8u)
                std::fprintf(stderr,"[G782:authority] tbp0=0x%x tbw=%u psm=0x%x cbp=0x%x fbp=0x%x\n",
                             c.tex0.tbp0,c.tex0.tbw,c.tex0.psm,c.tex0.cbp,fbp);
        }
        return false;
    }
    return true;
}

bool GSRasterizer::nativeE91F00(const dc2_g778::G780NativeItem& item)
{
    if(!g178GpuOn() || !g178_backend_ready())return false;
    g778FlushExec();
    if(!g_g144List.empty() || dc2_g778::g780NativeDraw || dc2_g778::shadowCommand)return false;
    G144Entry envelope{};
    dc2_g778::fillG780Envelope(item,envelope);
    // G783 fix: g_g144LastGs is also the EXEC thread's flush identity — the G144 flush closure
    // returns WITHOUT rendering while it is null (rasterizer_tilebin_capture.inc). The immutable
    // native flush must not see live GS state, but it must hand both globals back unchanged:
    // G780 left LastGs null, so every later batch the executor flushed before the parse thread's
    // next capture was silently dropped and frames were presented missing every draw after a
    // native E91F00 (MAP15 flicker, plans/phase-G783-ledger.md). Restored on every exit.
    struct G783RestoreFlushIdentity {
        GS* gs=g_g144LastGs;uint8_t* vram=g_g144LastVram;
        ~G783RestoreFlushIdentity(){g_g144LastGs=gs;g_g144LastVram=vram;}
    } g783Restore;
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
// ===== G784: NATIVE RUN ==================================================================
// Every native batch pays one synchronous GL-worker round trip (the async paths refuse depth
// batches, and the blocking wait is what yields the core: G701/G704). Legacy draws the contiguous
// E8B940 run (E8BB40..EA1760) as ONE batch; one native batch per bucket turned it into seven round
// trips (0.51 ms executor wall each, plans/phase-G784-ledger.md section 6). Consecutive committed
// native nodes with an identical target envelope are therefore flushed as ONE batch: no legacy work
// interleaves (the run is consecutive G713 native nodes), every envelope keeps its own texture state
// and draw, and draw order = node order. The run is flushed before ANY other G713 node runs and before
// the executor publishes the tail of its last visible node (ps2_g713_pipeline.cpp), so nothing can
// observe the framebuffer without the run. Rollback DC2_G784_NO_COALESCE=1.
extern bool g_g784RunPending;               // ps2_g713_pipeline.cpp, executor thread only
extern void (*g_g784NativeBarrier)();
namespace {
constexpr size_t kG784MaxRun=8;              // = rotating native GPU object sets
thread_local std::vector<std::unique_ptr<G780ExecPayload>> t_g784Run;
bool g784SameTarget(const dc2_g778::G780NativeItem& a,const dc2_g778::G780NativeItem& b)
{
    const auto& x=a.ctx;const auto& y=b.ctx;
    return a.rasterizer==b.rasterizer && a.vram==b.vram && a.vramSize==b.vramSize &&
        x.frame.fbp==y.frame.fbp && x.frame.fbw==y.frame.fbw && x.frame.psm==y.frame.psm && x.frame.fbmsk==y.frame.fbmsk &&
        x.scissor.x0==y.scissor.x0 && x.scissor.x1==y.scissor.x1 && x.scissor.y0==y.scissor.y0 && x.scissor.y1==y.scissor.y1 &&
        x.xyoffset.ofx==y.xyoffset.ofx && x.xyoffset.ofy==y.xyoffset.ofy && x.zbuf==y.zbuf &&
        x.alpha==y.alpha && x.test==y.test && x.fba==y.fba && a.pabe==b.pabe && a.fogcol==b.fogcol &&
        a.prim.type==b.prim.type && a.prim.iip==b.prim.iip && a.prim.tme==b.prim.tme && a.prim.fge==b.prim.fge &&
        a.prim.abe==b.prim.abe && a.prim.aa1==b.prim.aa1 && a.prim.fst==b.prim.fst && a.prim.ctxt==b.prim.ctxt &&
        a.prim.fix==b.prim.fix;
}
#if defined(PS2X_G684_HOT_DIAG)
struct G784RunStats { uint64_t runs=0,items=0,maxRun=0,ns=0,tryNs=0; };
G784RunStats g784RunStats;
#endif
}
static void g784FlushNativeRun()
{
    if(t_g784Run.empty()){g_g784RunPending=false;return;}
    std::vector<std::unique_ptr<G780ExecPayload>> run;run.swap(t_g784Run);
    g_g784RunPending=false;
#if defined(PS2X_G684_HOT_DIAG)
    const auto runStart=std::chrono::steady_clock::now();
#endif
    const auto& first=*run.front()->item;
    bool submitted=false;
    try{
        if(run.size()==1u)submitted=first.rasterizer->nativeE91F00(first);
        else if(g178GpuOn() && g178_backend_ready()){
            g778FlushExec();
            if(g_g144List.empty() && !dc2_g778::g780NativeDraw && !dc2_g778::shadowCommand){
                // Same flush-identity contract as nativeE91F00 (G783-F): hand both globals back.
                struct G784RestoreFlushIdentity {
                    GS* gs=g_g144LastGs;uint8_t* vram=g_g144LastVram;
                    ~G784RestoreFlushIdentity(){g_g144LastGs=gs;g_g144LastVram=vram;dc2_g778::g784RunEntries.clear();}
                } g784Restore;
                g_g144LastGs=nullptr;g_g144LastVram=first.vram;
                for(const auto& p:run){
                    G144Entry envelope{};
                    dc2_g778::fillG780Envelope(*p->item,envelope);
                    g_g144List.push_back(std::move(envelope));
                    dc2_g778::g784RunEntries.push_back({p->item->draw,p->item->bucket});
                }
#if defined(PS2X_G684_HOT_DIAG)
                const auto tryStart=std::chrono::steady_clock::now();
#endif
                submitted=g178TryFlushGpu(first.rasterizer,nullptr,first.vram,first.vramSize);
#if defined(PS2X_G684_HOT_DIAG)
                g784RunStats.tryNs+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-tryStart).count());
#endif
                g_g144List.clear();
            }
        }
    }catch(...){++dc2_g778::counters.transportFailures;submitted=false;}
    if(!submitted){
        std::fprintf(stderr,"[G780:late-failure] run=%zu fbp=%u fbw=%u bucket=%x rej=%d prepareErr=%u\n",
                     run.size(),first.ctx.frame.fbp,first.ctx.frame.fbw,first.bucket,
                     int(g_g266FlushRej),dc2_g778::g782LastPrepareError.load());
        std::fflush(stderr);
        std::abort();
    }
    for(const auto& p:run)if(p->g782Command)++dc2_g778::counters.g782NativeComplete;
#if defined(PS2X_G684_HOT_DIAG)
    auto& st=g784RunStats;++st.runs;st.items+=run.size();st.maxRun=std::max<uint64_t>(st.maxRun,run.size());
    st.ns+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-runStart).count());
    if(g780ProfileOn() && (st.runs%256u)==0u)
        std::fprintf(stderr,"[G784:run] runs=%llu items=%llu avgRun=%.2f maxRun=%llu flushMsPerRun=%.3f flushMsPerItem=%.3f tryFlushMsPerRun=%.3f\n",
            (unsigned long long)st.runs,(unsigned long long)st.items,double(st.items)/double(st.runs),
            (unsigned long long)st.maxRun,double(st.ns)/1e6/double(st.runs),double(st.ns)/1e6/double(st.items),
            double(st.tryNs)/1e6/double(st.runs));
#endif
}
static void g780ExecNative(void* value)
{
    std::unique_ptr<G780ExecPayload> payload(static_cast<G780ExecPayload*>(value));
    if(payload->g782Command){
        const auto choice=payload->g782Command->g782Commit->decision();
        if(choice==dc2_g782::Decision::LegacyCommitted)return; // lost CAS: no GPU side effect
        if(choice!=dc2_g782::Decision::NativeCommitted)std::abort();
    }
    if(payload->g782Command && dc2_g778::g784CoalesceOn()){
        // G784: join (or start) the native run; a different target closes the previous run first.
        if(!t_g784Run.empty() && (t_g784Run.size()>=kG784MaxRun || !g784SameTarget(*t_g784Run.back()->item,*payload->item)))
            g784FlushNativeRun();
        t_g784Run.push_back(std::move(payload));
        g_g784NativeBarrier=&g784FlushNativeRun;g_g784RunPending=true;
        return;
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
        // G782: past the transaction boundary legacy cannot run; a refusal here is either a
        // contract defect or a catastrophic backend failure. Name the site and stop.
        std::fprintf(stderr,"[G780:late-failure] fbp=%u fbw=%u bucket=%x rej=%d prepareErr=%u committed=%u\n",
                     payload->item->ctx.frame.fbp,payload->item->ctx.frame.fbw,payload->item->bucket,
                     int(g_g266FlushRej),dc2_g778::g782LastPrepareError.load(),
                     unsigned(bool(payload->g782Command)));
        std::fflush(stderr);
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
