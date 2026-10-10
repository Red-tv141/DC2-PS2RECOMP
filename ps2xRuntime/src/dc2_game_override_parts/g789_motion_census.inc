// G789 Phase 1: exact guest invocation lifetime, including separately dispatched resumes.
#if defined(PS2X_G775_DIAG) || defined(PS2X_G777_NATIVE_GL)
namespace g789_probe {
using namespace dc2_g789;
using Fn=PS2Runtime::RecompiledFunction;
enum Type { CharacterBody, CharacterShadow, CharacterEffect, Direct, Mdt, Add, Queued, MotionLoad, MotionFace, Palette, MotionCopy, Outline, Composite4, CompositeBody, FrameRect, RectLW, RectBBoard };
struct Hook {uint32_t pc,base,size;Type type;Fn previous=nullptr;};
#include "g789_census_bindings.inc"
struct Invocation {
    R5900Context* context=nullptr;uint32_t base=0,sp=0,ra=0,destination=0,state=0,geometry=0;
    Type type=Mdt;Identity identity{};
    std::shared_ptr<const dc2_g789_oracle::Pose> motionPose;
    std::shared_ptr<const dc2_g789_raster::Composite> composite;
    std::shared_ptr<dc2_g789_rect::Sample> rect;
};
static thread_local std::vector<std::shared_ptr<Invocation>> invocations;
static std::shared_ptr<Invocation> find(R5900Context* ctx,uint32_t base,uint32_t sp) {
    for(auto i=invocations.rbegin();i!=invocations.rend();++i)if((*i)->context==ctx && (*i)->base==base && (*i)->sp==sp)return *i;
    return {};
}
static Identity scope(R5900Context* ctx,uint32_t sp) {
    Identity d{};uint32_t best=~0u;
    for(const auto& i:invocations)if(i->context==ctx && (i->type==CharacterBody || i->type==CharacterShadow || i->type==CharacterEffect || i->type==Direct || i->type==Queued || i->type==Outline) && sp<=i->sp && i->sp<best){d=i->identity;best=i->sp;}
    return d;
}
static void watchDraw(Identity d,uint32_t state,uint32_t geometry,const uint8_t* ram) {
    d.stage=Setup;watch(state,d,ram);d.stage=Geometry;watch(geometry,d,ram);
}
template<size_t I> void wrapper(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    auto& h=hooks[I];
    const uint32_t currentSp=getRegU32(ctx,29)&0x1fffffffu;
    const bool entry=h.pc==h.base;
    const uint32_t entrySp=entry?currentSp:currentSp+h.size;
    if(h.type==FrameRect && h.pc==0x138624u)dc2_g789_rect::childReturned(ctx,currentSp,getRegU32(ctx,2),ram);
    auto invocation=entry?std::shared_ptr<Invocation>{}:find(ctx,h.base,entrySp);
    if(entry){
        // Equal guest frame keys cannot coexist. Refuse ambiguity; never infer a previous pose.
        if(find(ctx,h.base,entrySp)){++ambiguous;}
        invocation=std::make_shared<Invocation>();auto& v=*invocation;
        v.context=ctx;v.base=h.base;v.sp=entrySp;v.ra=getRegU32(ctx,31);v.type=h.type;
        v.identity=scope(ctx,currentSp);v.identity.frame=frame();v.identity.sf=g_dc2ScriptFrame.load(std::memory_order_relaxed);
        if(h.type==Mdt){
            auto& d=v.identity;d.draw=++nextDraw;d.visual=getRegU32(ctx,4)&0x1fffffffu;
            d.caller=v.ra;
            d.vtable=word(ram,d.visual+0x1cu)&0x1fffffffu;
            d.mesh=word(ram,d.vtable+0x20u)==0x28a660u?Motion:(word(ram,d.vtable+0x34u)==0x13f920u?Fix:Other);
            // Verified mgCFrame visual call at 0x1380dc; s5 is the actual frame, not a stack guess.
            if(v.ra==0x1380e4u){const uint32_t part=getRegU32(ctx,21)&0x1fffffffu;if(!d.object)d.object=part;}
            if(!d.object)d.object=d.visual;
            v.destination=getRegU32(ctx,5)&0x1fffffffu;
        }else if(h.type==Palette){
            // Original Base RI calls the virtual extension at 0x140b48. The live
            // guest frames are Base RI 0x250, Motion RI 0x20, and MDT Draw 0x60.
            const auto parent=find(ctx,0x13f4e0u,currentSp+0x2d0u);
            if(v.ra==0x140b50u && parent && parent->identity.visual==(getRegU32(ctx,4)&0x1fffffffu))v.identity=parent->identity;
            else {v.identity={};++dc2_g789_oracle::poseRefusals;}
            v.destination=getRegU32(ctx,5);v.state=getRegU32(ctx,6)&0x1fffffffu;v.geometry=getRegU32(ctx,7)&0x1fffffffu;
        }else if(h.type==MotionLoad || h.type==MotionFace || h.type==MotionCopy){
            v.identity={};v.identity.frame=frame();v.identity.sf=g_dc2ScriptFrame.load(std::memory_order_relaxed);
            v.identity.visual=getRegU32(ctx,4)&0x1fffffffu;v.identity.mesh=Motion;v.identity.vtable=word(ram,v.identity.visual+0x1cu);
            if(h.type==MotionLoad)dc2_g789_oracle::beginLoad(v.identity.visual,getRegU32(ctx,5)&0x1fffffffu,getRegU32(ctx,6)&0x1fffffffu);
            else if(h.type==MotionFace){
                v.destination=getRegU32(ctx,5);auto i=dc2_g789_oracle::sources.find(v.identity.visual);v.state=i==dc2_g789_oracle::sources.end()?~0u:uint32_t(i->second->faces.size());
                dc2_g789_oracle::face(v.identity.visual,getRegU32(ctx,6)&0x1fffffffu,ram);
            }
        }else if(h.type==Outline){
            v.state=getRegU32(ctx,4)&0x1fffffffu;
            v.identity.root=word(ram,v.state+0x34u)&0x1fffffffu;
        }else if(h.type==Composite4 || h.type==CompositeBody){
            const auto parent=find(ctx,0x17c2d0u,currentSp+0x3f0u);
            const bool four=h.type==Composite4;
            if(dc2_g789_raster::on() && parent && parent->motionPose && v.ra==(four?0x17c7d0u:0x17c888u) && g_g787Mode!=0){
                const auto g=g787::readGeom(ram,ctx,four);
                if(g787SpriteCount(g)){
                    auto c=std::make_shared<dc2_g789_raster::Composite>();c->pose=parent->motionPose;
                    c->seq=g787::g_seq+1u;c->kind=g.kind;c->outline=parent->state;c->texture=getRegU32(ctx,6)&0x1fffffffu;
                    c->rect={g.x0,g.y0,g.x1,g.y1};const uint32_t col=getRegU32(ctx,7)&0x1fffffffu;
                    for(unsigned k=0;k<4;++k)c->colour[k]=word(ram,col+k*4u);
                    c->tex0=uint64_t(word(ram,c->texture+0x38u))|(uint64_t(word(ram,c->texture+0x3cu))<<32);
                    c->tex1=uint64_t(word(ram,c->texture+0x40u))|(uint64_t(word(ram,c->texture+0x44u))<<32);
                    v.composite=c;dc2_g789_raster::publishComposite(c);
                }
            }
        }else if(h.type==FrameRect){
            uint64_t parent=0;uint32_t nearest=~0u;
            for(const auto& i:invocations)if(i->context==ctx && i->type==FrameRect && i->rect && i->sp>currentSp && i->sp<nearest){parent=i->rect->id;nearest=i->sp;}
            v.rect=dc2_g789_rect::begin(v.identity,getRegU32(ctx,4),getRegU32(ctx,5),getRegU32(ctx,6),getRegU32(ctx,28),parent,ram);
            dc2_g789_rect::root(ctx,currentSp,v.rect);
        }else if(h.type==RectLW || h.type==RectBBoard){
            if(v.ra==(h.type==RectLW?0x13826cu:0x13825cu))dc2_g789_rect::lw(ctx,currentSp,getRegU32(ctx,4),getRegU32(ctx,28),ram);
        }else if(h.type==Add){
            // The two verified AddPacket sites in MDT Draw retain its complete guest frame.
            if(v.ra==0x13f644u || v.ra==0x13f668u){
                const auto parent=find(ctx,0x13f4e0u,currentSp+0x60u);
                if(parent && parent->identity.visual==(getRegU32(ctx,16)&0x1fffffffu)){
                    v.identity=parent->identity;v.identity.bucket=getRegU32(ctx,17)&0x1fffffffu;
                    v.state=getRegU32(ctx,6)&0x1fffffffu;v.geometry=getRegU32(ctx,7)&0x1fffffffu;
                }else {v.identity={};++missingResume;}
            }else v.identity={};
        }else {
            auto& d=v.identity;d.operation=++nextOperation;
            if(h.type==Direct || h.type==Queued){
                d.root=getRegU32(ctx,4)&0x1fffffffu;if(!d.object)d.object=d.root;
                d.producer=v.ra;
                if(h.type==Queued){if(v.ra==0x17304cu)d.pass=Body;else if(!d.pass)d.pass=QueuedFrame;}
                else if(v.ra==0x17c61cu){
                    d.pass=Silhouette;
                    // mgSetPkFrameBuffer has already published the engine W viewport.
                    const uint32_t gp=getRegU32(ctx,28)&0x1fffffffu;
                    d.wWidth=word(ram,gp-0x7880u);d.wHeight=word(ram,gp-0x787cu);
                    d.wOffsetX=2048u+word(ram,gp-0x7878u);d.wOffsetY=2048u+word(ram,gp-0x7874u);
                    for(const auto& parent:invocations)if(parent->context==ctx && parent->type==Outline && parent->identity.root==d.root){
                        const uint32_t texture=word(ram,parent->state+0x30u)&0x1fffffffu;
                        if(dc2_g789_oracle::range(texture,0x48u))d.wtex0=uint64_t(word(ram,texture+0x38u))|(uint64_t(word(ram,texture+0x3cu))<<32u);
                    }
                }
                else if(v.ra==0x17c398u)d.pass=Body;
                else if(!d.pass)d.pass=OtherDirect;
            }else {
                d.object=getRegU32(ctx,4)&0x1fffffffu;
                d.root=word(ram,d.object+(h.type==CharacterShadow?0x2c0u:0x70u))&0x1fffffffu;
                d.pass=h.type==CharacterShadow?Shadow:(h.type==CharacterEffect?Effect:Body);
            }
            d.objectVtable=word(ram,d.object)&0x1fffffffu;
        }
        invocations.push_back(invocation);
    }else if(!invocation && h.type!=FrameRect)++missingResume;
    h.previous(ram,ctx,runtime);
    if(entry && invocation && invocation->composite){
        const auto& c=*invocation->composite;const auto& d=c.pose->identity;
        const bool valid=g787::g_seq==c.seq && g_g787Ring[c.seq&(kG787Ring-1u)].seq==c.seq;
        std::fprintf(stderr,"[G789:wconsumer] frame=%u sf=%u draw=%llu object=%x visual=%x instance=%llu outline=%x seq=%u kind=%u rect=%d,%d,%d,%d texture=%x tex0=%llx tex1=%llx colour=%u,%u,%u,%u descriptorValid=%u\n",d.frame,d.sf,(unsigned long long)d.draw,d.object,d.visual,(unsigned long long)c.pose->instance,c.outline,c.seq,c.kind,c.rect[0],c.rect[1],c.rect[2],c.rect[3],c.texture,(unsigned long long)c.tex0,(unsigned long long)c.tex1,c.colour[0],c.colour[1],c.colour[2],c.colour[3],unsigned(valid));
        if(!valid){std::fprintf(stderr,"[G789:wconsumer] FATAL descriptor identity fault\n");std::abort();}
    }
    // A preempted generated call does not return to its caller. Keep immutable invocation alive.
    if(invocation && ctx->pc==invocation->ra && (getRegU32(ctx,29)&0x1fffffffu)==invocation->sp){
        const auto& v=*invocation;
        if(v.rect){dc2_g789_rect::finish(*v.rect,v.sp,getRegU32(ctx,2),ram);dc2_g789_rect::rootReturned(ctx,v.sp);}
        if(v.type==MotionFace)dc2_g789_topology::face(v.identity.visual,v.state,v.destination,getRegU32(ctx,2),ram);
        if(v.type==MotionLoad){
            dc2_g789_oracle::finishLoad(v.identity.visual);dc2_g789_topology::loaded(v.identity.visual,ram);
            if(dc2_g789_live::requested()){
                const auto source=dc2_g789_oracle::sources.find(v.identity.visual);
                if(source!=dc2_g789_oracle::sources.end())dc2_g789_publication::source(*source->second,ram);
            }
        }
        if(v.type==MotionCopy)dc2_g789_oracle::clone(v.identity.visual,getRegU32(ctx,2)&0x1fffffffu,ram);
        if(v.type==Palette)dc2_g789_oracle::palette(v.identity,v.destination,v.state,v.geometry,getRegU32(ctx,2),ram,runtime->memory().getScratchpad());
        if(v.type==Mdt && dc2_g789_raster::on()){
            std::shared_ptr<const dc2_g789_oracle::Pose> pose;
            {std::lock_guard<std::mutex> lock(dc2_g789_oracle::poseMutex);auto i=dc2_g789_oracle::poses.find(v.identity.draw);if(i!=dc2_g789_oracle::poses.end())pose=i->second;}
            if(pose)for(const auto& i:invocations)if(i->context==ctx && i->type==Outline && i->identity.object==v.identity.object && i->identity.root==v.identity.root){
                if(i->motionPose){std::fprintf(stderr,"[G789:wconsumer] FATAL ambiguous producer\n");std::abort();}i->motionPose=pose;
            }
        }
        dc2_g789_oracle::report(frame());
        if(v.type!=Add && v.type<=MotionCopy)engine(v.identity,ram,v.type,v.ra,getRegU32(ctx,2));
        if(v.type==Add && v.identity.draw){
            Identity publishedIdentity=v.identity;
            if(dc2_g789_live::requested()){
                if(const auto carrier=dc2_g789_publication::publish(v.identity,v.identity.wtex0,ram))publishedIdentity.commitment=carrier->identity.commitment;
                dc2_g789_live::report(frame());
            }
            watchDraw(publishedIdentity,v.state,v.geometry,ram);
            if(admissionAuditOn()){
                uint32_t gates=0u;
                if(v.identity.mesh==Motion && v.identity.pass==Silhouette && dc2_g789_oracle::range(v.geometry,16u))gates|=1u;
                std::shared_ptr<const dc2_g789_oracle::Pose> pose;
                {std::lock_guard<std::mutex> lock(dc2_g789_oracle::poseMutex);
                    auto it=dc2_g789_oracle::poses.find(v.identity.draw);
                    if(it!=dc2_g789_oracle::poses.end())pose=it->second;}
                if(pose)gates|=2u;
                const auto liveInstance=dc2_g789_oracle::instances.find(v.identity.visual);
                if(pose && pose->source && pose->source->sealed &&
                   pose->source->generation==40u && pose->source->counts[0]==395u &&
                   pose->source->counts[1]==395u && pose->source->counts[3]==234u &&
                   pose->source->dictionary.size()==21u && pose->source->faces.size()==115u &&
                   pose->palette.size()==21u*64u)gates|=4u;
                if(pose && liveInstance!=dc2_g789_oracle::instances.end() && liveInstance->second==pose->instance)gates|=8u;
                if(pose && pose->source){
                    auto topology=dc2_g789_raster::topology(pose->source->generation);
                    if(topology && topology->packets.size()==131u && topology->triangles==652u){
                        gates|=16u;
                        if(gates==31u){
                        AdmissionProbe probe{};probe.identity=v.identity;
                        probe.source=pose->source->generation;probe.instance=pose->instance;
                        probe.geometry=v.geometry;probe.packets=uint32_t(topology->packets.size());
                        probe.triangles=topology->triangles;probe.paletteBytes=uint32_t(pose->palette.size());
                        std::memcpy(probe.first.data(),ram+v.geometry,16u);
                        admissionMark(probe);
                        }
                    }
                }
                admissionSeen(v.identity,v.geometry,gates);
            }
        }
        if(v.type==Mdt && v.destination && v.identity.draw){
            // Direct form starts with CALL renderInfo then ordered program/geometry CALLs.
            const uint32_t qw=getRegU32(ctx,2);bool valid=qw && qw<65536u && v.destination>=0x10000u && uint64_t(v.destination)+uint64_t(qw)*16<=0x2000000u;
            Identity d=v.identity;d.stage=Geometry;
            if(valid){
                // The ordinary W class uses MDT's direct-destination branch,
                // which does not call AddPacket. This is the same verified
                // guest RA/restored-SP completion, before its caller sends DMA.
                // An outer generated host return is never publication authority.
                if(dc2_g789_live::requested()){
                    if(const auto carrier=dc2_g789_publication::publish(v.identity,v.identity.wtex0,ram))d.commitment=carrier->identity.commitment;
                    dc2_g789_live::report(frame());
                }
                watch(v.destination,d,ram);ends[v.destination+qw*16]=true;
                for(uint32_t q=0;q<qw;++q){const uint32_t a=v.destination+q*16;
                    if(((word(ram,a)>>28)&7u)!=5u)continue;
                    const uint32_t target=word(ram,a+4)&0x1fffffffu;
                    if(q==0u){d.stage=Setup;watch(target,d,ram);}
                    else {
                        uint32_t bucket=word(ram,d.visual+0x48u)&0x1fffffffu;
                        for(uint32_t b=0;bucket && b<256u;++b){if((word(ram,bucket+0x10u)&0x1fffffffu)==target){d.bucket=bucket;d.stage=Geometry;watch(target,d,ram);break;}bucket=word(ram,bucket+8u)&0x1fffffffu;}
                    }
                }
            }else ++mismatch;
        }
        for(auto i=invocations.begin();i!=invocations.end();++i)if(i->get()==invocation.get()){invocations.erase(i);break;}
    }
}
template<size_t... I> void bind(PS2Runtime& runtime,std::index_sequence<I...>) {
    auto one=[&](size_t i,Fn f){auto& h=hooks[i];
        if(h.type>=MotionLoad && h.type<=MotionCopy && !dc2_g789_oracle::sourceOn())return;
        if(h.type>=Composite4 && !dc2_g789_oracle::on())return;
        if(h.type==Outline && !dc2_g789_oracle::sourceOn())return;
        h.previous=runtime.lookupFunction(h.pc);if(!h.previous){std::fprintf(stderr,"[G789:census] FATAL missing guest binding %x\n",h.pc);std::abort();}runtime.registerFunction(h.pc,f);};
    (one(I,&wrapper<I>),...);
}
} // namespace g789_probe
#endif
static void g789_register(PS2Runtime& runtime) {
#if defined(PS2X_G775_DIAG) || defined(PS2X_G777_NATIVE_GL)
    if(!dc2_g789::trackingOn())return;
    g789_probe::bind(runtime,std::make_index_sequence<std::size(g789_probe::hooks)>{});
    std::fprintf(stderr,"[G789:%s] armed bindings=%zu legacy-authoritative\n",dc2_g789::on()?"census":"publication",std::size(g789_probe::hooks));
#else
    (void)runtime;
#endif
}
