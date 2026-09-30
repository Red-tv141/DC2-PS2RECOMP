#if defined(PS2X_G775_DIAG) || defined(PS2X_G777_NATIVE_GL)
static void g775_mesh_draw_probe(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    using namespace dc2_g775_capture;
    const uint32_t self=getRegU32(ctx,4)&0x1FFFFFFFu;
    const uint32_t world=getRegU32(ctx,6)&0x1FFFFFFFu;
    const uint32_t manager=getRegU32(ctx,7)?getRegU32(ctx,7)&0x1FFFFFFFu:0x003820e0u;
    const uint32_t caller=getRegU32(ctx,31);
    const Draw previous=engineDraw;
    Draw draw{drawCount.fetch_add(1)+1,g_dc2ScriptFrame.load(std::memory_order_relaxed),self,0x13f4e0u};
    draw.world=world;draw.manager=manager;draw.vtable=readWord(ram,self+0x1c);draw.reason=0;
    // Proven caller 0x1380dc dispatches visual from mgCFrame in saved s5.
    draw.parent=caller==0x1380e4u?getRegU32(ctx,21)&0x1fffffffu:0;
    const uint32_t destination=getRegU32(ctx,5)&0x1fffffffu;
    recordInstance(draw,caller,destination,ram);
    engineDraw=draw;
#if defined(PS2X_G775_DIAG)
    captureEngine(draw,ram,world,manager,caller);
    g776_native_shadow(draw,ram);
#endif
#if defined(PS2X_G777_NATIVE_GL)
    g778_capture_draw(draw,ram);
#endif
    Draw__12mgCVisualMDTFPUiPA4_fP14mgCDrawManager_0x13f4e0(ram,ctx,runtime);
    if(destination && ctx->pc==caller)
    {
        const size_t bytes=size_t(getRegU32(ctx,2))*16;
        addRange(destination,bytes,draw);
        rememberDirectChain(destination,bytes,draw,ram);
    }
    if(valid(self,0x50))
    {
        uint32_t bucket=dc2_read_u32(ram,self+0x48);
        for(uint32_t n=0;n<256 && valid(bucket,0x20);++n)
        {
            auto owned=draw;owned.bucket=bucket;owned.geometry=readWord(ram,bucket+0x10)&0x1fffffffu;
            addPacketRanges(owned.geometry,ram,owned);
            rememberStateGeometry(engineDraw.state,owned.geometry,owned);
            bucket=dc2_read_u32(ram,bucket+8);
        }
    }
    engineDraw=previous;
#if defined(PS2X_G775_DIAG)
    if(enabled() && (draw.id==1 || !(draw.id%32768)))std::fprintf(stderr,"[G775:engine] draws=%llu sf=%u self=%x caller=%x\n",static_cast<unsigned long long>(draw.id),draw.sf,self,caller);
#endif
}
static void g776_add_packet_probe(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    using namespace dc2_g775_capture;
    const uint32_t manager=getRegU32(ctx,4)&0x1fffffffu;
    const uint32_t state=getRegU32(ctx,6)&0x1fffffffu,geometry=getRegU32(ctx,7)&0x1fffffffu;
    const uint32_t list=readWord(ram,manager+0x50),before=readWord(ram,list);
    AddPacket__14mgCDrawManagerFiP1P1i_0x1359d0(ram,ctx,runtime);
    const uint32_t node=readWord(ram,list);
    if(node!=before){auto owned=engineDraw;owned.bucket=exactBucket(owned,geometry,ram);rememberList(node,state,geometry,owned);}
}
static void g776_draw_manager_probe(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    const bool previous=dc2_g775_capture::managerScope;
    dc2_g775_capture::managerScope=true;
    g91_drawmgr_flush_probe(ram,ctx,runtime);
    dc2_g775_capture::managerScope=previous;
}
static void g776_send_program_probe(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    using namespace dc2_g775_capture;
    const uint32_t caller=getRegU32(ctx,31),destination=getRegU32(ctx,4)&0x1fffffffu;
    const uint32_t node=managerScope && caller==0x135834u?readWord(ram,getRegU32(ctx,16)&0x1fffffffu):0;
    const uint32_t previousState=getRegU32(ctx,18)&0x1fffffffu,state=readWord(ram,node)&0x1fffffffu;
    mgSendVuProg__FPUii_0x145e80(ram,ctx,runtime);
    if(node && ctx->pc==caller)
        rememberCall(destination+getRegU32(ctx,2)*4+(state!=previousState?16:0),node,ram);
}
static void g775_render_info_probe(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    const uint32_t destination=getRegU32(ctx,5);
    CreateRenderInfoPacket__12mgCVisualMDTFPUiPA4_fP13mgRENDER_INFO_0x1404d0(ram,ctx,runtime);
    dc2_g775_capture::addRange(destination,size_t(getRegU32(ctx,2))*16,dc2_g775_capture::engineDraw);
    dc2_g775_capture::engineDraw.state=destination&0x1fffffffu;
    dc2_g775_capture::rememberState(destination&0x1fffffffu,dc2_g775_capture::engineDraw);
}
static void g775_mdt_load_probe(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    const uint32_t self=getRegU32(ctx,4);
    dc2_g775_capture::beginMeshAssignment(self);
    DataAssignMDT__12mgCVisualMDTFP10MDT_HEADERP9mgCMemoryP17mgCTextureManager_0x13f290(ram,ctx,runtime);
    dc2_g775_capture::rememberMesh(self,ram);
    dc2_g775_capture::endMeshAssignment(self);
}
static void g775_fix_load_probe(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    const uint32_t self=getRegU32(ctx,4);
    dc2_g775_capture::beginMeshAssignment(self);
    DataAssignMDT__15mgCVisualFixMDTFP10MDT_HEADERP9mgCMemoryP17mgCTextureManager_0x13f360(ram,ctx,runtime);
    if(!dc2_g775_capture::loadedBuckets.count(self&0x1fffffffu))dc2_g775_capture::rememberMesh(self,ram);
    dc2_g775_capture::endMeshAssignment(self);
}
static void g776_create_face_probe(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    const uint32_t self=getRegU32(ctx,4),output=getRegU32(ctx,8)&0x1fffffffu;
    CreateFace__12mgCVisualMDTFP8FACES_IDP9mgCMemoryP9mgCMemoryPP7mgCFace_0x13f010(ram,ctx,runtime);
    if(output)dc2_g775_capture::rememberFace(self,dc2_g775_capture::readWord(ram,output)&0x1fffffffu,ram);
}
static void g776_face_packet_probe(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime)
{
    // Last structured lifetime point, before any VIF packet is generated.
    static unsigned reports=0;
    if(dc2_g775_capture::enabled() && reports++<8)std::fprintf(stderr,"[G776:source-seam] self=%x caller=%x face=%x scope=%zu\n",getRegU32(ctx,4),getRegU32(ctx,31),getRegU32(ctx,6),dc2_g775_capture::assignmentStack.size());
    dc2_g775_capture::rememberLoadedBuckets(getRegU32(ctx,4),ram,getRegU32(ctx,6)&0x1fffffffu);
    dc2_g775_capture::rememberFace(getRegU32(ctx,4),getRegU32(ctx,6)&0x1fffffffu,ram);
    CreateFacePacket__12mgCVisualMDTFPUiP7mgCFace_0x13ff60(ram,ctx,runtime);
}
#endif
