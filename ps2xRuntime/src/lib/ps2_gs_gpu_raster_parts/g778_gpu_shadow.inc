#if defined(PS2X_G777_NATIVE_GL)
// Diagnostic only: clone the accepted batch's starting colour/depth, render its
// original geometry and structured GPU geometry with the same accepted resources.
// No frontend ownership is published for either isolated comparison surface.
using G778BlitFramebuffer=void(APIENTRY*)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
G778BlitFramebuffer m_g778BlitFramebuffer=nullptr;
bool g778LiveRasterSelected(const G178Batch& batch)
{
    static const bool on=std::getenv("DC2_G778_LIVE_RASTER_PROBE")!=nullptr;
    const auto* probe=batch.g777Native;
    return on && probe && probe->live && probe->draw && probe->draw->frame>=1400 &&
        probe->draw->frame<=4500 && probe->draw->frame%10==0;
}
void g778LiveRasterFinish(const G178Batch& batch,const std::vector<uint32_t>& before,
    const std::vector<float>& beforeZ,const std::vector<uint32_t>& after,const std::vector<float>& afterZ)
{
    const auto& surface=m_fbos.at(g349FboKey(batch.fboKey?batch.fboKey:batch.fbp,batch.fbW));
    size_t colour=0,depth=0;int x0=surface.physW,y0=surface.physH,x1=-1,y1=-1;
    for(size_t i=0;i<before.size();++i){
        const bool changed=before[i]!=after[i];colour+=changed;depth+=std::memcmp(&beforeZ[i],&afterZ[i],4)!=0;
        if(changed){const int x=int(i%surface.physW),y=int(i/surface.physW);
            x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);}
    }
    dc2_g778::counters.liveColourPixels+=colour;dc2_g778::counters.liveDepthPixels+=depth;
    const auto& draw=*batch.g777Native->draw;
    std::fprintf(stderr,"[G778:live-raster] draw=%llu sf=%u triangles=%u colour=%zu depth=%zu bounds=%d,%d,%d,%d\n",
        (unsigned long long)draw.invocation,draw.frame,batch.g777Native->vertexCount/3,colour,depth,x0,y0,x1,y1);
    if(colour && colour>500){
        auto write=[&](const char* arm,const std::vector<uint32_t>& pixels){
            char path[160];std::snprintf(path,sizeof(path),"logs/g778/live_%u_%llu_%s.ppm",draw.frame,
                (unsigned long long)draw.invocation,arm);
            if(FILE* file=std::fopen(path,"wb")){
                std::fprintf(file,"P6\n%d %d\n255\n",x1-x0+1,y1-y0+1);
                for(int y=y1;y>=y0;--y)for(int x=x0;x<=x1;++x){const auto p=pixels[size_t(y)*surface.physW+x];
                    const unsigned char rgb[3]={uint8_t(p),uint8_t(p>>8),uint8_t(p>>16)};std::fwrite(rgb,3,1,file);}
                std::fclose(file);
            }
        };write("before",before);write("native",after);
    }
}
bool g778SeedShadow(const G178Batch& batch,uint64_t depthKey,uint32_t cloneKey,uint64_t cloneDepth,
                    bool uploadDepth)
{
    if(!m_g778BlitFramebuffer && !loadProc(m_g778BlitFramebuffer,"glBlitFramebuffer"))return false;
    const auto sourceKey=g349FboKey(batch.fboKey?batch.fboKey:batch.fbp,batch.fbW);
    const auto source=m_fbos.find(sourceKey);
    if(!batch.uploadFb && (source==m_fbos.end() || !source->second.fbo ||
       source->second.w!=batch.fbW || source->second.h!=batch.fbH))return false;
    GLuint sourceFbo=source==m_fbos.end()?0:source->second.fbo;
    GLuint sourceDepth=0;
    if(depthKey){
        const auto depth=m_depths.find(depthKey);
        const bool validDepth=depth!=m_depths.end() && depth->second.initialized && depth->second.tex &&
           depth->second.w==batch.fbW && depth->second.h==batch.fbH;
        if(!validDepth && !uploadDepth)return false;
        if(validDepth)sourceDepth=depth->second.tex;
    }
    auto& destination=ensureFbo(cloneKey,batch.fbW,batch.fbH);
    if(!destination.fbo)return false;
    const int width=destination.physW,height=destination.physH;
    m_f.glBindFramebuffer_(0x8ca9,destination.fbo);
    if(depthKey){
        auto& depth=ensureDepth(cloneDepth,batch.fbW,batch.fbH);
        if(!depth.tex)return false;
        m_f.glFramebufferTexture2D_(0x8ca9,0x8d00,GL_TEXTURE_2D,depth.tex,0);
        depth.initialized=sourceDepth!=0;
    }
    if(sourceFbo){
        m_f.glBindFramebuffer_(0x8ca8,sourceFbo);
        if(sourceDepth)m_f.glFramebufferTexture2D_(0x8ca8,0x8d00,GL_TEXTURE_2D,sourceDepth,0);
        const GLbitfield mask=(batch.uploadFb?0:GL_COLOR_BUFFER_BIT) | (sourceDepth?GL_DEPTH_BUFFER_BIT:0);
        const bool scissor=glIsEnabled(GL_SCISSOR_TEST)!=0;glDisable(GL_SCISSOR_TEST);
        if(mask)m_g778BlitFramebuffer(0,0,width,height,0,0,width,height,mask,GL_NEAREST);
        if(scissor)glEnable(GL_SCISSOR_TEST);
    }
    return glGetError()==GL_NO_ERROR;
}
void g778ReadShadow(uint32_t key,uint64_t depthKey,const G178Batch& batch,std::vector<uint32_t>& colour,std::vector<float>& depth)
{
    auto& fbo=m_fbos.at(g349FboKey(key,batch.fbW));
    m_f.glBindFramebuffer_(0x8d40,fbo.fbo);
    // Accepted renderBatch detaches guest depth on return. Read the cloned guest
    // texture, not the FBO's unrelated private renderbuffer.
    if(depthKey)m_f.glFramebufferTexture2D_(0x8d40,0x8d00,GL_TEXTURE_2D,m_depths.at(depthKey).tex,0);
    colour.resize(size_t(fbo.physW)*fbo.physH);depth.resize(colour.size());
    glReadPixels(0,0,fbo.physW,fbo.physH,GL_RGBA,GL_UNSIGNED_BYTE,colour.data());
    glReadPixels(0,0,fbo.physW,fbo.physH,GL_DEPTH_COMPONENT,GL_FLOAT,depth.data());
}
void g778ShadowRaster(const G178Batch& original,uint64_t depthKey,const std::vector<float>* depthUpload,int depthY,int depthRows)
{
    auto command=original.g778Shadow;
    if(!command)return;
    const auto fail=[&](unsigned reason){
        ++dc2_g778::counters.shadowErrors;++command->shadowFailures;
        if(dc2_g778::statsRequested())std::fprintf(stderr,"[G778:raster] draw=%llu sf=%u refused=%u\n",
            (unsigned long long)command->draw->invocation,command->draw->frame,reason);
    };
    if(original.draws.size()!=1 || original.draws[0].srcFbp || !command->draw){fail(1);return;}
    constexpr uint32_t expectedKey=0x77810001,candidateKey=0x77810002;
    constexpr uint64_t expectedDepth=0x7781000100000001ull,candidateDepth=0x7781000200000001ull;
    GLint drawFbo=0,readFbo=0,viewport[4]{};
    glGetIntegerv(0x8ca6,&drawFbo);glGetIntegerv(0x8caa,&readFbo);glGetIntegerv(GL_VIEWPORT,viewport);
    struct Restore {
        decltype(m_f)& f;GLint draw,read;GLint* viewport;
        ~Restore(){f.glBindFramebuffer_(0x8ca9,draw);f.glBindFramebuffer_(0x8ca8,read);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);}
    } restore{m_f,drawFbo,readFbo,viewport};
    if(!g778SeedShadow(original,depthKey,expectedKey,expectedDepth,depthUpload!=nullptr) ||
       !g778SeedShadow(original,depthKey,candidateKey,candidateDepth,depthUpload!=nullptr)){fail(2);return;}
    auto expected=original,candidate=original;
    expected.g778Shadow.reset();candidate.g778Shadow.reset();
    expected.fboKey=expectedKey;candidate.fboKey=candidateKey;
    expected.skipReadback=true;candidate.skipReadback=true;
    G777NativeProbe probe;probe.draw=command->draw.get();probe.bucket=command->bucket;
    // Original vertices are already relative to the actual live XYOFFSET.
    // The engine viewport centre is 2048; context offsets are carried explicitly
    // from the original frontend's accepted state, never inferred from vertices.
    probe.offsetX=original.g778OffsetX;probe.offsetY=original.g778OffsetY;
    candidate.g777Native=&probe;
    std::vector<uint32_t> seed,before,after;std::vector<float> seedZ,beforeZ,afterZ;
    g778ReadShadow(expectedKey,depthKey?expectedDepth:0,expected,seed,seedZ);
    if(!renderBatch(expected,depthKey?expectedDepth:0,depthUpload,depthY,depthRows)){fail(3);return;}
    g778ReadShadow(expectedKey,depthKey?expectedDepth:0,expected,before,beforeZ);
    if(!renderBatch(candidate,depthKey?candidateDepth:0,depthUpload,depthY,depthRows)){fail(100+probe.error);return;}
    g778ReadShadow(candidateKey,depthKey?candidateDepth:0,candidate,after,afterZ);
    if(before.size()!=after.size() || beforeZ.size()!=afterZ.size() || glGetError()!=GL_NO_ERROR){fail(4);return;}
    size_t different=0,depthDifferent=0,touched=0,colourTouched=0,depthTouched=0;
    const auto& surface=m_fbos.at(g349FboKey(expectedKey,original.fbW));
    int x0=surface.physW,y0=surface.physH,x1=-1,y1=-1;
    for(size_t i=0;i<before.size();++i){
        different+=before[i]!=after[i];depthDifferent+=std::memcmp(&beforeZ[i],&afterZ[i],4)!=0;
        const bool colourChanged=before[i]!=seed[i];
        const bool depthChanged=std::memcmp(&beforeZ[i],&seedZ[i],4)!=0;
        colourTouched+=colourChanged;depthTouched+=depthChanged;
        if(colourChanged || depthChanged){
            ++touched;const int x=int(i%surface.physW),y=int(i/surface.physW);
            x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);
        }
    }
    if(touched && (command->draw->frame==1500 || command->draw->frame==1560 || command->draw->frame==2100)){
        x0=std::max(0,x0-8);y0=std::max(0,y0-8);x1=std::min(surface.physW-1,x1+8);y1=std::min(surface.physH-1,y1+8);
        auto writeCrop=[&](const char* arm,const std::vector<uint32_t>& pixels){
            char path[180];std::snprintf(path,sizeof(path),"logs/g778/shadow_%u_%llu_%s.ppm",command->draw->frame,
                (unsigned long long)command->draw->invocation,arm);
            if(FILE* file=std::fopen(path,"wb")){
                std::fprintf(file,"P6\n%d %d\n255\n",x1-x0+1,y1-y0+1);
                for(int y=y1;y>=y0;--y)for(int x=x0;x<=x1;++x){uint32_t p=pixels[size_t(y)*surface.physW+x];
                    const unsigned char rgb[3]={uint8_t(p),uint8_t(p>>8),uint8_t(p>>16)};std::fwrite(rgb,3,1,file);}
                std::fclose(file);
            }
        };writeCrop("legacy",before);writeCrop("native",after);
    }
    ++command->shadowBatches;++dc2_g778::counters.shadowBatches;
    dc2_g778::counters.shadowPixels+=before.size();dc2_g778::counters.shadowDifferent+=different+depthDifferent;
    dc2_g778::counters.sourceUploadBytes+=probe.sourceUploadBytes;
    if(probe.sourceUploadBytes)++dc2_g778::counters.cacheMisses;else ++dc2_g778::counters.cacheHits;
    dc2_g778::counters.gpuDraws+=probe.gpuDraws;
    if(different || depthDifferent)++command->shadowFailures;
    std::fprintf(stderr,"[G778:raster] draw=%llu sf=%u original_triangles=%zu native_triangles=%zu pixels=%zu touched=%zu colour_touched=%zu depth_touched=%zu clear_depth=%u seed_upload=%u/%u xyoffset=%d/%d tex=%llx src=%x blend=%u depth=%u/%u bilinear=%u fog=%u target=%x/%dx%d scissor=%d,%d,%d,%d colour_different=%zu depth_different=%zu gpuDraws=%u upload=%llu\n",
        (unsigned long long)command->draw->invocation,command->draw->frame,original.verts.size()/3,probe.metadata.size(),
        before.size(),touched,colourTouched,depthTouched,unsigned(original.clearDepth),unsigned(original.uploadFb),unsigned(depthUpload!=nullptr),probe.offsetX,probe.offsetY,
        (unsigned long long)original.draws[0].texKey,original.draws[0].srcFbp,original.draws[0].blend,
        original.draws[0].depthFunc,unsigned(original.draws[0].depthWrite),unsigned(original.draws[0].bilinear),original.draws[0].fge,
        original.fbp,original.fbW,original.fbH,original.draws[0].scX0,original.draws[0].scY0,original.draws[0].scX1,original.draws[0].scY1,
        different,depthDifferent,probe.gpuDraws,(unsigned long long)probe.sourceUploadBytes);
}
#endif
