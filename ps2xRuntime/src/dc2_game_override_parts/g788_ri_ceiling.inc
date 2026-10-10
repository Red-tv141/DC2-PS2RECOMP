// Bounded RI packing experiment. No temporal cache or direct draw/state table.
// The original matrix, draw-env and SPR DMA helpers remain the authority.
#if defined(PS2X_G777_NATIVE_GL)
namespace g788_ri {
using Fn=g788::RecompiledFunction;
static Fn original=nullptr,constructor=nullptr,matrix=nullptr,env=nullptr,spr=nullptr,dma=nullptr;
static int mode() {
    // G791 release: RI packing DEFAULT ON (G788-AA setting); rollback DC2_G788_RI=off.
    static const int v=[] {const char* e=std::getenv("DC2_G788_RI");
        if(!e || !*e)return 2;
        if(!std::strcmp(e,"off"))return 0;
        if(!std::strcmp(e,"oracle"))return 1;
        if(!std::strcmp(e,"native"))return 2;
        std::fprintf(stderr,"[G788:ri] FATAL invalid mode=%s\n",e);std::abort();return 0;}();
    return v;
}
struct Prediction {
    uint32_t frame=0,self=0,at=0,pmode=0;
    uint64_t generation=0;
    std::array<uint8_t,512> body{};
    bool active=false;
};
static Prediction prediction;
static uint64_t prepared=0,checked=0,converted=0,refused=0,alreadyNative=0,bytes=0;
static void report() {
    if(!g788::censusOn() && !dc2_g788_econ::on())return;
    static uint32_t last=~0u;const uint32_t f=g788::sf();
    if(f/600==last)return;last=f/600;
    std::fprintf(stderr,"[G788:ri] sf=%u mode=%d prepared=%llu checked=%llu converted=%llu refused=%llu alreadyNative=%llu bytes=%llu\n",
        f,mode(),(unsigned long long)prepared,(unsigned long long)checked,(unsigned long long)converted,
        (unsigned long long)refused,(unsigned long long)alreadyNative,(unsigned long long)bytes);
}
static void invoke(Fn fn,uint32_t pc,uint32_t ra,uint8_t* ram,R5900Context& c,PS2Runtime* runtime) {
#if defined(PS2X_G775_DIAG)
    const unsigned stage=pc==0x130250u?dc2_g788_econ::MatrixHelper:
        pc==0x13e850u?dc2_g788_econ::DrawenvBuild:
        pc==0x13e3d0u?dc2_g788_econ::SendDma:dc2_g788_econ::StageCount;
    dc2_g788_econ::Scope helper(stage,true,stage!=dc2_g788_econ::StageCount);
#endif
    g788_packet::reg(c,31,ra);c.pc=pc;fn(ram,&c,runtime);
    // These original helpers have no live preempting loops on this contract.
    // A changed callback cannot silently authorize an incomplete prediction.
    if(c.pc!=ra && c.pc!=pc){std::fprintf(stderr,"[G788:ri] FATAL incomplete helper pc=%x next=%x\n",pc,c.pc);std::abort();}
}
static bool allowed(uint8_t* ram,R5900Context* ctx,uint32_t& renderer,uint32_t& config,uint32_t& lights,uint32_t& drawenv) {
    const auto& p=g788::g_pending;
    if((g_g788Mode!=3 && g_g788Mode!=4) || !p.eligible || !p.mesh || !p.entry || p.clipMode || p.frame!=g788::sf() ||
       (getRegU32(ctx,4)&0x1fffffffu)!=p.self)return false;
    if(p.self==dc2_g784::kMeshSelf && dc2_g778::requested()){++alreadyNative;return false;}
    for(const auto& b:p.entry->buckets)if(!dc2_g788_econ_add_allowed(b.first)){++alreadyNative;return false;}
    renderer=getRegU32(ctx,7)&0x1fffffffu;
    if(!g788::ok(renderer,0x1100) || renderer!=(g788::rd(ram,p.manager+0x64)&0x1fffffffu))return false;
    config=g788::rd(ram,renderer+0xfcc)&0x1fffffffu;
    if(!g788::ok(config,0x90) || g788::rd(ram,config+0x40) || g788::rd(ram,config+0x4c) ||
       g788::rd(ram,renderer+0xfc0) || g788::rd(ram,renderer+0xfc4) || g788::rd(ram,renderer+0xfc8) ||
       g788::vecAt(ram,config+0x80)[3]>1.0f || !g788::ok(p.worldPtr,64))return false;
    const uint32_t vt=g788::rd(ram,p.self+0x1c);
    if(vt!=g788::kFixVtable || !g788::ok(vt,0x44) || g788::rd(ram,vt+0x40)!=0x140be0u)return false;
    lights=renderer+0x400+g788::rd(ram,renderer+0x3f4)*0x150u;
    drawenv=g788::rd(ram,p.self+4)&0x1fffffffu;if(!drawenv)drawenv=renderer+0xf20;
    const uint32_t dest=getRegU32(ctx,5);
    const uint32_t pool=g788::rd(ram,p.manager+0x60)&0x1fffffffu;
    const uint32_t sp=getRegU32(ctx,29)&0x1fffffffu;
    return dest==p.renderInfo && dest==(dest&0x1fffffffu) && g788::ok(dest,512) &&
        g788::ok(pool,0x2c) && uint64_t(g788::rd(ram,pool+0x24))+32u<g788::rd(ram,pool+0x28) &&
        g788::ok(lights,0x150) && g788::ok(drawenv,64) && sp>=1024u && g788::ok(sp-1024u,1024u);
}
static void pack(uint8_t* ram,R5900Context& c,PS2Runtime* runtime,uint32_t renderer,uint32_t config,uint32_t lights,uint32_t drawenv,Prediction& out) {
    const auto& p=g788::g_pending;
    invoke(spr,0x13e3b0u,0x140548u,ram,c,runtime);
    const uint32_t addr=getRegU32(&c,2);
    if(addr!=0x70000000u && addr!=0x70002000u){std::fprintf(stderr,"[G788:ri] FATAL unexpected SPR=%x\n",addr);std::abort();}
    auto* body=runtime->memory().getScratchpad()+(addr&0x3fffu);
    auto* w=reinterpret_cast<uint32_t*>(body);
    std::memcpy(body+112,ram+p.worldPtr,64);
    g788_packet::reg(c,4,addr+48);g788_packet::reg(c,5,renderer+16);g788_packet::reg(c,6,addr+112);
    invoke(matrix,0x130250u,0x1405f4u,ram,c,runtime);
    const uint32_t head[8]={0x10000013u,0,0,0,0,g788::rd(ram,p.self+0x10)|0x03000000u,
        g788::rd(ram,p.self+0x14)|0x02000000u,0x6c120003u};
    std::memcpy(body,head,sizeof(head)); // bytes32..47 intentionally retain original SPR contents
    std::memcpy(body+176,ram+lights,48);std::memcpy(body+224,ram+lights+64,64);
    std::memcpy(body+288,ram+lights+128,16);std::memcpy(body+304,ram+renderer+0x1000,16);
    c.f[1]=g788::vecAt(ram,lights+128)[3];c.f[0]=g788::vecAt(ram,config+0x40)[1];
    c.f[0]=FPU_MUL_S(c.f[1],c.f[0]);std::memcpy(body+300,&c.f[0],4);
    c.f[1]=g788::vecAt(ram,renderer+0x1000)[3];c.f[0]=g788::vecAt(ram,config+0x40)[1];
    c.f[0]=FPU_MUL_S(c.f[1],c.f[0]);std::memcpy(body+316,&c.f[0],4);
    out.pmode=0x58u|((g788::rd(ram,config+0x30) && g788::rd(ram,renderer+0xfa4))?0x20u:0u);
    uint32_t fog=g788::rd(ram,renderer+0xfd8)&0xffffffu;
    const uint32_t fm=g788::rd(ram,config+0x30);if(fm==2)fog=0;if(fm==3)fog=0xffffffu;
    const uint32_t tail[28]={0x1000000au,0,0,0x6c010026u,0,0,0,0,
        0,0,0x14000000u,0x50000008u,0x8003u,0x10000000u,0xeu,0,
        0,0,0x1au,0,out.pmode,0,0x1bu,0,fog,0,0x3du,0};
    std::memcpy(body+320,tail,sizeof(tail));std::memcpy(ram+p.self+0xc,&out.pmode,4);
    g788_packet::reg(c,4,p.self);g788_packet::reg(c,5,addr+432);g788_packet::reg(c,6,renderer);g788_packet::reg(c,7,drawenv);
    invoke(env,0x13e850u,0x140b28u,ram,c,runtime);
    if(getRegU32(&c,2)!=4u){std::fprintf(stderr,"[G788:ri] FATAL env qw=%u\n",getRegU32(&c,2));std::abort();}
    const uint32_t ret[4]={0x60000000u,0,0,0};std::memcpy(body+496,ret,16);
    out.self=p.self;out.frame=p.frame;out.at=p.renderInfo;out.generation=p.mesh->identity;
    out.active=mode()==1 || g788::censusOn() || dc2_g788_econ::on();
    if(out.active)std::memcpy(out.body.data(),body,512);
}
static void entry(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    uint32_t renderer=0,config=0,lights=0,drawenv=0;
    if(!allowed(ram,ctx,renderer,config,lights,drawenv)){++refused;original(ram,ctx,runtime);return;}
    if(prediction.active){std::fprintf(stderr,"[G788:ri] FATAL unpublished prediction\n");std::abort();}
    R5900Context work=*ctx;
    const uint32_t sp=getRegU32(ctx,29)&0x1fffffffu;
    std::array<uint8_t,1024> savedStack;std::array<uint8_t,16384> savedSpr;
    const uint32_t savedPmode=g788::rd(ram,g788::g_pending.self+0xc);
    if(mode()==1){std::memcpy(savedStack.data(),ram+sp-1024,1024);std::memcpy(savedSpr.data(),runtime->memory().getScratchpad(),16384);}
    g788_packet::reg(work,29,getRegU32(ctx,29)-512u);
    pack(ram,work,runtime,renderer,config,lights,drawenv,prediction);++prepared;
    if(mode()==1) {
        std::memcpy(ram+sp-1024,savedStack.data(),1024);std::memcpy(runtime->memory().getScratchpad(),savedSpr.data(),16384);
        std::memcpy(ram+g788::g_pending.self+0xc,&savedPmode,4);
        original(ram,ctx,runtime);
    } else {
        g788_packet::reg(work,4,prediction.at);g788_packet::reg(work,5,32u);
        invoke(dma,0x13e3d0u,0x140ba4u,ram,work,runtime);
        // Preserve math-owner state and caller-saved FPR effects. ABI callee-saved
        // GPRs/SP remain the entry values; the caller consumes only RI qword count.
        std::memcpy(ctx->vu0_vf,work.vu0_vf,sizeof(ctx->vu0_vf));ctx->vu0_acc=work.vu0_acc;
        ctx->vu0_mac_flags=work.vu0_mac_flags;ctx->vu0_status=work.vu0_status;
        std::memcpy(ctx->f,work.f,sizeof(ctx->f));setReturnU32(ctx,32u);ctx->pc=getRegU32(ctx,31);++converted;
    }
    report();
}
static void onConstructor(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    if(prediction.active) {
        const auto& p=g788::g_pending;const auto& e=prediction;
        const uint32_t pool=g788::rd(ram,p.manager+0x60)&0x1fffffffu;
        const uint32_t end=(g788::rd(ram,pool+0x20)+g788::rd(ram,pool+0x24)*16u)&0x1fffffffu;
        const bool bad=p.frame!=e.frame || p.self!=e.self || !p.mesh || p.mesh->identity!=e.generation ||
            (getRegU32(ctx,4)&0x1fffffffu)!=e.self || end!=e.at+512u ||
            g788::rd(ram,e.self+0xc)!=e.pmode || std::memcmp(ram+e.at,e.body.data(),512);
        if(bad){
            size_t first=0;while(first<512 && ram[e.at+first]==e.body[first])++first;
            std::fprintf(stderr,"[G788:ri] FATAL packet mismatch frame=%u visual=%x at=%x end=%x firstByte=%llu\n",e.frame,e.self,e.at,end,(unsigned long long)first);std::abort();
        }
        ++checked;bytes+=512;prediction.active=false;
    }
    constructor(ram,ctx,runtime);
}
}
static void g788_ri_register(PS2Runtime& runtime) {
    if(!g788_ri::mode())return;
    auto& r=runtime;g788_ri::original=r.lookupFunction(0x1404d0u);g788_ri::constructor=r.lookupFunction(0x13f920u);
    g788_ri::matrix=r.lookupFunction(0x130250u);g788_ri::env=r.lookupFunction(0x13e850u);
    g788_ri::spr=r.lookupFunction(0x13e3b0u);g788_ri::dma=r.lookupFunction(0x13e3d0u);
    if(!g788_ri::original || !g788_ri::constructor || !g788_ri::matrix || !g788_ri::env || !g788_ri::spr || !g788_ri::dma){std::fprintf(stderr,"[G788:ri] FATAL missing helpers\n");std::abort();}
    r.registerFunction(0x1404d0u,&g788_ri::entry);r.registerFunction(0x13f920u,&g788_ri::onConstructor);
}
#else
static void g788_ri_register(PS2Runtime&){}
#endif
