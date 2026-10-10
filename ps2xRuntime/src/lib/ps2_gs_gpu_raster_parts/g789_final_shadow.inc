// Diagnostic final-image oracle. G786/G787 implementations stay untouched.
// Clone the exact incoming main colour/depth, replay the same ordered consumer
// batch with independent legacy/native W snapshots, and compare whole targets.
#if defined(PS2X_G775_DIAG) && defined(PS2X_G777_NATIVE_GL)
GLuint m_g789CopyFbo=0;
void g789ReleaseSnapshots(std::map<int,std::array<GLuint,4>>& values){for(auto& entry:values)for(GLuint texture:entry.second)if(texture)glDeleteTextures(1,&texture);values.clear();}
GLuint g789CloneVersion(GLuint source,int width,int height){
    if(!source || width<=0 || height<=0)return 0;
    GLint texture=0,read=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegerv(0x8caa,&read);
    if(!m_g789CopyFbo)m_f.glGenFramebuffers_(1,&m_g789CopyFbo);
    m_f.glBindFramebuffer_(0x8ca8,m_g789CopyFbo);m_f.glFramebufferTexture2D_(0x8ca8,0x8ce0,GL_TEXTURE_2D,source,0);
    GLuint result=0;glGenTextures(1,&result);glBindTexture(GL_TEXTURE_2D,result);
    glTexImage2D(GL_TEXTURE_2D,0,GL158_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL158_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL158_CLAMP_TO_EDGE);
    glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,width,height);
    glBindTexture(GL_TEXTURE_2D,texture);m_f.glBindFramebuffer_(0x8ca8,read);return result;
}
void g789CaptureVersions(unsigned arm){
    for(int n=0;n<g_g786SnapN;++n){const int slot=g_g786Snap[n].slot;if(slot<0 || slot>=kG786MaxSlots || !m_g786Valid[slot])continue;
        auto& images=m_g789PendingSnapshots[slot];if(images[arm])glDeleteTextures(1,&images[arm]);images[arm]=g789CloneVersion(m_g786WTex[slot],m_g786WW[slot],m_g786WH[slot]);
    }
}
void g789ObserveSnapshot(const G178Batch& b,int slot){
    if(!m_g789WShadowArm || slot<0 || slot>=kG786MaxSlots)return;
    auto fbo=m_fbos.find(g349FboKey(b.fboKey?b.fboKey:b.fbp,b.fbW));if(fbo==m_fbos.end())return;
    auto& image=m_g789PendingSnapshots[slot][m_g789WShadowArm+1u];if(image)glDeleteTextures(1,&image);
    image=g789CloneVersion(fbo->second.colorTex,fbo->second.physW,fbo->second.physH);
}
struct G789FinalShadow {
    const G178Batch* batch=nullptr;uint64_t depth=0;std::shared_ptr<const dc2_g789_oracle::Pose> pose;
    std::shared_ptr<const std::vector<dc2_g789_raster::CompositeJob>> consumers;
    std::vector<uint32_t> seed;std::vector<float> seedDepth;
};
std::shared_ptr<G789FinalShadow> m_g789Final;
int m_g789FinalReplay=0;
uint32_t m_g789FinalCalls[3]{},m_g789FinalBodies[3]{},m_g789FinalOutlines[3]{};
unsigned m_g789FinalRefused=0,m_g789FinalSamples=0;
int m_g789ReplayFirst=0;
uint64_t m_g789ReplayTexKey=0;
std::shared_ptr<const dc2_g789_raster::Composite> m_g789ReplayConsumer;
void g789FinalDraw(const G178Batch& b,const G178Draw& d){
    m_g789ReplayConsumer.reset();m_g789ReplayFirst=d.firstVtx;m_g789ReplayTexKey=d.texKey;if(!m_g789FinalReplay || !m_g789Final)return;
    unsigned covered=0;
    for(const auto& j:*m_g789Final->consumers)if(j.first>=unsigned(d.firstVtx) && j.first+j.count<=unsigned(d.firstVtx+d.vtxCount)){
        if(m_g789ReplayConsumer && m_g789ReplayConsumer!=j.composite){++m_g789FinalRefused;m_g789ReplayConsumer.reset();return;}
        m_g789ReplayConsumer=j.composite;covered+=j.count;
    }
    if(covered!=unsigned(d.vtxCount)){if(m_g789ReplayConsumer)++m_g789FinalRefused;m_g789ReplayConsumer.reset();}
}
GLuint g789FinalTexture(GLuint texture,uint32_t source){
    if(!m_g789ReplayConsumer || !m_g789NativeDraws.count(m_g789ReplayConsumer->pose->identity.draw))return texture;
    const auto i=m_g789WVersions.find(m_g789ReplayConsumer->pose->identity.draw);
    if(i==m_g789WVersions.end()){++m_g789FinalRefused;return texture;}const auto& v=i->second;
    int slot=-1;for(int n=0;n<kG786MaxSlots;++n)if(m_g786Valid[n] && texture==m_g786WTex[n] && source==m_g786WKey[n] && m_g786SlotRun[n]==v.run){slot=n;break;}
    if(slot<0 && source==0u && ((m_g789ReplayConsumer->tex0&16383u)/32u)==v.fbp){
        for(int n=0;n<g_g786OvrN;++n)if(g_g786Ovr[n].vtxBegin<=m_g789ReplayFirst && m_g789ReplayFirst<g_g786Ovr[n].vtxEnd){slot=g_g786Ovr[n].slot;break;}
        auto image=v.snapshots.find(slot);
        const bool unversioned=slot<0 && v.snapshots.empty() && v.fullW[0] && v.fullW[1];
        if(unversioned || (slot>=0 && image!=v.snapshots.end() && m_g786SlotRun[slot]==v.run)){
            GLuint fork=g789MaterializedFork(texture,v,slot);if(fork){
                ++m_g789FinalCalls[m_g789FinalReplay];if(m_g789ReplayConsumer->kind==2u)++m_g789FinalBodies[m_g789FinalReplay];else ++m_g789FinalOutlines[m_g789FinalReplay];return fork;}
        }
    }
    // An unhoisted original S/C pair can sample the exact live W FBO directly.
    // Route the already captured independent complete arms without fabricating
    // a G786 slot or reading the actual sampler into either candidate source.
    if(slot<0 && source==v.key && source==v.fbp && texture==v.texture && v.snapshots.empty() &&
       ((m_g789ReplayConsumer->tex0&16383u)/32u)==v.fbp && v.fullW[0] && v.fullW[1]){
        ++m_g789FinalCalls[m_g789FinalReplay];
        if(m_g789ReplayConsumer->kind==2u)++m_g789FinalBodies[m_g789FinalReplay];else ++m_g789FinalOutlines[m_g789FinalReplay];
        return v.fullW[m_g789FinalReplay-1];
    }
    auto images=v.snapshots.find(slot);
    if(slot<0 || source!=v.key || images==v.snapshots.end() || !images->second[0] || !images->second[1]){++m_g789FinalRefused;return texture;}
    ++m_g789FinalCalls[m_g789FinalReplay];
    if(m_g789ReplayConsumer->kind==2u)++m_g789FinalBodies[m_g789FinalReplay];else ++m_g789FinalOutlines[m_g789FinalReplay];
    return images->second[m_g789FinalReplay-1];
}
struct G789MaterializedFork {uint64_t draw=0;int slot=0;GLuint source=0;std::array<GLuint,2> texture{};bool valid=false;};
std::vector<G789MaterializedFork> m_g789Forks;
bool g789TexturePixels(GLuint texture,std::vector<uint32_t>& pixels,int& width,int& height){
    GLint bound=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&bound);glBindTexture(GL_TEXTURE_2D,texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);
    if(width>0 && height>0 && width<=4096 && height<=4096){pixels.resize(size_t(width)*height);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());}
    glBindTexture(GL_TEXTURE_2D,bound);return !pixels.empty() && glGetError()==GL_NO_ERROR;
}
GLuint g789MaterializedFork(GLuint texture,const G789WVersion& version,int slot){
    for(const auto& f:m_g789Forks)if(f.draw==version.draw && f.slot==slot && f.source==texture)return f.valid?f.texture[m_g789FinalReplay-1]:0u;
    G789MaterializedFork result;result.draw=version.draw;result.slot=slot;result.source=texture;
    const auto images=version.snapshots.find(slot);
    // Rabbit-F's coverage guard can preserve the original S/C ordering without
    // a G786 hoist/slot. Reuse the accepted two complete W arms for this exact
    // engine-owned unversioned producer; never invent a G786 version identity.
    const bool unversioned=slot<0 && version.snapshots.empty();
    if(!unversioned && (images==version.snapshots.end() || !images->second[2] || !images->second[3]))return 0;
    const GLuint selectedLegacy=unversioned?version.fullW[0]:images->second[2];
    const GLuint selectedNative=unversioned?version.fullW[1]:images->second[3];
    std::vector<uint32_t> actual,legacy,native,current,fullLegacy,fullNative;int width=0,height=0,lw=0,lh=0,nw=0,nh=0,cw=0,ch=0,flw=0,flh=0,fnw=0,fnh=0;
    if(!g789TexturePixels(texture,actual,width,height) || !g789TexturePixels(selectedLegacy,legacy,lw,lh) || !g789TexturePixels(selectedNative,native,nw,nh))return 0;
    if(!version.fullW[0] || !version.fullW[1] || !g789TexturePixels(version.fullW[0],fullLegacy,flw,flh) || !g789TexturePixels(version.fullW[1],fullNative,fnw,fnh) || flw!=lw || flh!=lh || fnw!=lw || fnh!=lh)return 0;
    const bool haveCurrent=version.texture && g789TexturePixels(version.texture,current,cw,ch) && cw==lw && ch==lh;
    // Exact ordinary CT32/TBW8/TW9/TH9 W materialization only. The colour
    // readback's recovered centre tap and GS-row flip are applied literally.
    const uint64_t tex0=m_g789ReplayConsumer->tex0;
    if(width!=512 || height!=512 || lw!=nw || lh!=nh || lw%512 || lh%512 || ((tex0>>14u)&63u)!=8u || ((tex0>>20u)&63u)!=0u || ((tex0>>26u)&15u)!=9u || ((tex0>>30u)&15u)!=9u)return 0;
    const uint32_t* decodedBase=nullptr;
    for(const auto& up:m_g789Final->batch->texUploads)if(up.key==m_g789ReplayTexKey && up.w==512 && up.h==512 && up.pixelCount()==size_t(512)*512){decodedBase=up.pixelData();break;}
    if(!decodedBase){std::fprintf(stderr,"[G789:materialized-base] draw=%llu key=%llu uploads=%zu found=0\n",(unsigned long long)version.draw,(unsigned long long)m_g789ReplayTexKey,m_g789Final->batch->texUploads.size());return 0;}
    const int sx=lw/512,sy=lh/512;
    const int fullRect[4]={0,0,lw,lh};
    const int* physicalRect=unversioned?fullRect:m_g786WRect[slot];
    const int rx0=physicalRect[0]/sx,ry0=physicalRect[1]/sy;
    const int rx1=(physicalRect[0]+physicalRect[2]-1)/sx;
    const int ry1=(physicalRect[1]+physicalRect[3]-1)/sy;
    int x0=511,y0=511,x1=0,y1=0;size_t vertices=0;
    for(const auto& j:*m_g789Final->consumers)if(j.composite->pose->identity.draw==version.draw){
        for(uint32_t k=j.first;k<j.first+j.count;++k){const auto& v=m_g789Final->batch->verts[k];if(v.iq!=1.0f)return 0;
            x0=std::min(x0,int(std::floor(v.sq*512.0f))-2);x1=std::max(x1,int(std::ceil(v.sq*512.0f))+2);
            y0=std::min(y0,int(std::floor(v.tq*512.0f))-2);y1=std::max(y1,int(std::ceil(v.tq*512.0f))+2);++vertices;}
    }
    if(!vertices)return 0;
    // Conservative interpolation/filter footprint. An out-of-range coordinate
    // can wrap, so compare the whole axis in that case.
    if(x0<0 || x1>=512){x0=0;x1=511;}
    if(y0<0 || y1>=512){y0=0;y1=511;}
    // Rebuild every texel from each arm's immutable W boundary. Seeding from the
    // actual materialized texture would make unsampled texels a shared A/B source.
    std::vector<uint32_t> a(size_t(512)*512),b(size_t(512)*512);
    size_t legacyBad=0,nativeBad=0,footprintLegacyBad=0,footprintNativeBad=0,rectLegacyBad=0,currentBad=0,footprintCurrentBad=0,currentVsLegacy=0,vrBase=0,footprintVrBase=0,fullLegacyBad=0,fullNativeBad=0;
    int badX=-1,badY=-1;uint32_t badActual=0,badSource=0;
    for(int y=0;y<512;++y)for(int x=0;x<512;++x){
        const size_t at=size_t(y)*512+x;
        const size_t sourceAt=size_t((511-y)*sy+sy/2)*lw+x*sx+sx/2;
        a[at]=fullLegacy[sourceAt];b[at]=fullNative[sourceAt];
        fullLegacyBad+=a[at]!=actual[at];fullNativeBad+=b[at]!=actual[at];
        // Guest VRAM can retain a prior image where neither selected W version
        // nor either complete W arm has a texel. This base is independent of the
        // selected draw; use its decoded bytes in two separate fork textures.
        if(actual[at] && !a[at] && !b[at] && !legacy[sourceAt] && !native[sourceAt] && decodedBase[at]==actual[at]){
            a[at]=b[at]=decodedBase[at];++vrBase;
            if(x>=x0 && x<=x1 && y>=y0 && y<=y1)++footprintVrBase;
        }
        legacyBad+=a[at]!=actual[at];nativeBad+=b[at]!=actual[at];
        if(haveCurrent){const uint32_t now=current[sourceAt];currentBad+=now!=actual[at];currentVsLegacy+=now!=a[at];}
        if(x>=x0 && x<=x1 && y>=y0 && y<=y1){
            footprintLegacyBad+=a[at]!=actual[at];
            footprintNativeBad+=b[at]!=actual[at];
            if(haveCurrent)footprintCurrentBad+=current[sourceAt]!=actual[at];
        }
        if(x>=rx0 && x<=rx1 && y>=ry0 && y<=ry1 && a[at]!=actual[at]){
            ++rectLegacyBad;
            if(badX<0){badX=x;badY=y;badActual=actual[at];badSource=a[at];}
        }
    }
    if(haveCurrent && m_g789FinalSamples==0u && m_g789Forks.empty()){
        std::vector<uint32_t> current512(size_t(512)*512);
        for(int y=0;y<512;++y)for(int x=0;x<512;++x)current512[size_t(y)*512+x]=current[size_t((511-y)*sy+sy/2)*lw+x*sx+sx/2];
        FILE* dump=dc2_g789_oracle::file("materialized_"+std::to_string(version.draw)+".bin");
        const uint32_t header[]={0x47374d54u,1u,uint32_t(version.draw),uint32_t(version.draw>>32u),uint32_t(slot),uint32_t(texture),uint32_t(x0),uint32_t(y0),uint32_t(x1),uint32_t(y1)};
        dc2_g789_oracle::write(dump,header,sizeof(header));
        for(const auto* pixels:{&actual,&a,&b,&current512})dc2_g789_oracle::write(dump,pixels->data(),pixels->size()*4u);
        dc2_g789_oracle::close(dump);
    }
    // Diagnostic only: force the fork to expose where the existing materialized
    // sampler differs in final pixels. The report still rejects source mismatch.
    const bool force=std::getenv("DC2_G789_MAT_DIAG_FORCE")!=nullptr;
    const bool sourceParity=footprintLegacyBad==0 && footprintNativeBad==0;
    result.valid=sourceParity || force;
    if(result.valid){GLint bound=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&bound);glBindTexture(GL_TEXTURE_2D,texture);
        GLint parameters[4]{};const GLenum names[]={GL_TEXTURE_MIN_FILTER,GL_TEXTURE_MAG_FILTER,GL_TEXTURE_WRAP_S,GL_TEXTURE_WRAP_T};for(unsigned n=0;n<4u;++n)glGetTexParameteriv(GL_TEXTURE_2D,names[n],&parameters[n]);
        for(unsigned arm=0;arm<2u;++arm){glGenTextures(1,&result.texture[arm]);glBindTexture(GL_TEXTURE_2D,result.texture[arm]);const auto& pixels=arm?b:a;glTexImage2D(GL_TEXTURE_2D,0,GL158_RGBA8,512,512,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());for(unsigned n=0;n<4u;++n)glTexParameteri(GL_TEXTURE_2D,names[n],parameters[n]);}
        glBindTexture(GL_TEXTURE_2D,bound);
    }
    std::fprintf(stderr,"[G789:materialized] draw=%llu run=%llu slot=%d texture=%u size=%dx%d physical=%dx%d pixels=%zu footprint=%d,%d,%d,%d footprintPixels=%zu rect=%d,%d,%d,%d rectLegacyDifferent=%zu firstBad=%d,%d,%08x,%08x legacyDifferent=%zu nativeDifferent=%zu footprintLegacyDifferent=%zu footprintNativeDifferent=%zu fullLegacyDifferent=%zu fullNativeDifferent=%zu vrBase=%zu footprintVrBase=%zu currentAvailable=%u currentDifferent=%zu footprintCurrentDifferent=%zu currentVsLegacy=%zu sourceParity=%u forced=%u valid=%u\n",(unsigned long long)version.draw,(unsigned long long)version.run,slot,texture,width,height,lw,lh,a.size(),x0,y0,x1,y1,size_t(x1-x0+1)*size_t(y1-y0+1),rx0,ry0,rx1,ry1,rectLegacyBad,badX,badY,badActual,badSource,legacyBad,nativeBad,footprintLegacyBad,footprintNativeBad,fullLegacyBad,fullNativeBad,vrBase,footprintVrBase,unsigned(haveCurrent),currentBad,footprintCurrentBad,currentVsLegacy,unsigned(sourceParity),unsigned(force),unsigned(result.valid));
    m_g789Forks.push_back(result);return result.valid?result.texture[m_g789FinalReplay-1]:0u;
}
void g789FinalBegin(const G178Batch& b,uint64_t depth,const std::vector<float>* upload){
    if(m_g789Active)return;m_g789Final.reset();
    if(!g789NativeRequested() || m_g789FinalSamples>=64u || (b.fbp!=0u && b.fbp!=0x68u) || g_g786SnapN || !b.g788Jobs.empty() || b.g777Native)return;
    const auto transport=dc2_g789_raster::transport(b.g778Shadow);if(!transport || !transport->consumers || transport->consumers->empty())return;
    std::shared_ptr<const dc2_g789_oracle::Pose> pose;
    for(const auto& j:*transport->consumers)if(m_g789NativeDraws.count(j.composite->pose->identity.draw)){pose=j.composite->pose;break;}
    if(!pose)return;
    constexpr uint32_t legacyKey=0x78920001u,nativeKey=0x78920002u;
    constexpr uint64_t legacyDepth=0x7892000100000001ull,nativeDepth=0x7892000200000001ull;
    GLint draw=0,read=0,viewport[4]{},texture=0;glGetIntegerv(0x8ca6,&draw);glGetIntegerv(0x8caa,&read);glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
    auto value=std::make_shared<G789FinalShadow>();value->batch=&b;value->depth=depth;value->pose=pose;value->consumers=transport->consumers;
    bool ok=g778SeedShadow(b,depth,legacyKey,legacyDepth,upload!=nullptr) && g778SeedShadow(b,depth,nativeKey,nativeDepth,upload!=nullptr);
    if(ok)g778ReadShadow(legacyKey,depth?legacyDepth:0u,b,value->seed,value->seedDepth);
    m_f.glBindFramebuffer_(0x8ca9,draw);m_f.glBindFramebuffer_(0x8ca8,read);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glBindTexture(GL_TEXTURE_2D,texture);
    if(!ok){std::fprintf(stderr,"[G789:finalshadow] draw=%llu refused=seed\n",(unsigned long long)pose->identity.draw);return;}m_g789Final=value;
}
void g789FinalFinish(const G178Batch& b,uint64_t depth,const std::vector<float>* upload,int y,int rows){
    if(m_g789Active || !m_g789Final || m_g789Final->batch!=&b)return;
    const auto value=m_g789Final;m_g789Active=true;
    GLint draw=0,read=0,viewport[4]{};glGetIntegerv(0x8ca6,&draw);glGetIntegerv(0x8caa,&read);glGetIntegerv(GL_VIEWPORT,viewport);
    struct Restore {decltype(m_f)& f;bool& active;int& replay;GLint draw,read;GLint* viewport;~Restore(){active=false;replay=0;f.glBindFramebuffer_(0x8ca9,draw);f.glBindFramebuffer_(0x8ca8,read);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);}} restore{m_f,m_g789Active,m_g789FinalReplay,draw,read,viewport};
    std::vector<uint32_t> actual,a,c;std::vector<float> actualDepth,az,cz;
    g778ReadShadow(b.fboKey?b.fboKey:b.fbp,depth,b,actual,actualDepth);
    auto legacy=b,native=b;legacy.fboKey=0x78920001u;native.fboKey=0x78920002u;legacy.skipReadback=native.skipReadback=true;
    const uint64_t legacyDepth=depth?0x7892000100000001ull:0u,nativeDepth=depth?0x7892000200000001ull:0u;
    const auto run=m_g786RunNo;const bool last=m_g786LastArmedWasC;
    m_g789FinalRefused=0;for(unsigned n=0;n<3u;++n)m_g789FinalCalls[n]=m_g789FinalBodies[n]=m_g789FinalOutlines[n]=0;
    m_g789FinalReplay=1;bool ok=renderBatch(legacy,legacyDepth,upload,y,rows);g778ReadShadow(legacy.fboKey,legacyDepth,legacy,a,az);
    m_g789FinalReplay=2;ok=renderBatch(native,nativeDepth,upload,y,rows) && ok;g778ReadShadow(native.fboKey,nativeDepth,native,c,cz);
    m_g786RunNo=run;m_g786LastArmedWasC=last;m_g789ReplayConsumer.reset();
    for(const auto& f:m_g789Forks)for(GLuint texture:f.texture)if(texture)glDeleteTextures(1,&texture);m_g789Forks.clear();
    size_t legacyColour=0,nativeColour=0,legacyZ=0,nativeZ=0,touched=0;
    if(a.size()!=actual.size() || c.size()!=actual.size() || az.size()!=actualDepth.size() || cz.size()!=actualDepth.size())ok=false;
    if(ok)for(size_t n=0;n<actual.size();++n){legacyColour+=a[n]!=actual[n];nativeColour+=c[n]!=actual[n];touched+=n<value->seed.size() && actual[n]!=value->seed[n];}
    if(ok)for(size_t n=0;n<actualDepth.size();++n){legacyZ+=std::memcmp(&az[n],&actualDepth[n],4u)!=0;nativeZ+=std::memcmp(&cz[n],&actualDepth[n],4u)!=0;}
    const auto& id=value->pose->identity;const unsigned sample=++m_g789FinalSamples;
    const auto surface=m_fbos.find(g349FboKey(b.fboKey?b.fboKey:b.fbp,b.fbW));
    if(sample==1u && ok && surface!=m_fbos.end()){
        FILE* f=dc2_g789_oracle::file("finalshadow_"+std::to_string(id.draw)+".bin");
        const uint32_t h[]={0x47374653u,1u,id.frame,id.sf,uint32_t(id.draw),uint32_t(id.draw>>32u),id.object,id.visual,b.fbp,uint32_t(surface->second.physW),uint32_t(surface->second.physH),uint32_t(m_batchCounter),uint32_t(m_batchCounter>>32u),uint32_t(actualDepth.size()),uint32_t(b.draws.size()),sample};
        dc2_g789_oracle::write(f,h,sizeof(h));for(const auto* pixels:{&value->seed,&a,&c,&actual})dc2_g789_oracle::write(f,pixels->data(),pixels->size()*4u);for(const auto* z:{&value->seedDepth,&az,&cz,&actualDepth})dc2_g789_oracle::write(f,z->data(),z->size()*4u);dc2_g789_oracle::close(f);
    }
    std::fprintf(stderr,"[G789:finalshadow] frame=%u draw=%llu target=%x sample=%u pixels=%zu touched=%zu legacyColour=%zu nativeColour=%zu legacyDepth=%zu nativeDepth=%zu legacySamples=%u nativeSamples=%u legacyBody=%u nativeBody=%u legacyOutline=%u nativeOutline=%u refused=%u ok=%u glError=%u kicksSuppressed=%u legacyGeometryAbsent=%u\n",id.frame,(unsigned long long)id.draw,b.fbp,sample,actual.size(),touched,legacyColour,nativeColour,legacyZ,nativeZ,m_g789FinalCalls[1],m_g789FinalCalls[2],m_g789FinalBodies[1],m_g789FinalBodies[2],m_g789FinalOutlines[1],m_g789FinalOutlines[2],m_g789FinalRefused,unsigned(ok),unsigned(glGetError()),dc2_g789_live::deletion()?131u:0u,unsigned(dc2_g789_live::deletion()));
    m_g789Final.reset();
}
#endif
