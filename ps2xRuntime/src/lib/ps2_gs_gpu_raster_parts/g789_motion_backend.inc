// G789 ordinary MotionMDT W-path same-frame GPU raster oracle.
// Private GL-owner include. Diagnostic uploads/readback are not timing authority.
#if defined(PS2X_G775_DIAG) && defined(PS2X_G777_NATIVE_GL)
inline static const char* kG789MotionCS=R"GLSL(
#version 430 core
layout(local_size_x=64) in;
struct Input {vec4 position;vec4 normal;vec4 uv;uvec4 offsets;vec4 weights;};
layout(std430,binding=0) readonly buffer Source {Input source[];};
layout(std430,binding=1) readonly buffer Constants {vec4 constants[];};
layout(std430,binding=2) buffer Output {uint vertices[];};
layout(std430,binding=3) readonly buffer Metadata {uvec4 metadata[];};
layout(location=0) uniform int count;
precise vec4 point(uint base,vec4 p){
    precise vec4 v=constants[base]*p.x;
    v=v+constants[base+1u]*p.y;v=v+constants[base+2u]*p.z;v=v+constants[base+3u]*p.w;return v;
}
void main(){
    uint id=gl_GlobalInvocationID.x;if(id>=uint(count))return;
    Input v=source[id];uvec4 meta=metadata[id];uint pb=meta.y,c=meta.x;
    precise vec4 wp=v.position*v.weights.x,wn=v.normal*v.weights.x;
    precise vec4 p=constants[pb+v.offsets.x]*wp.x,n=constants[pb+v.offsets.x]*wn.x;
    for(uint slot=0u;slot<4u;++slot){
        wp=v.position*v.weights[slot];wn=v.normal*v.weights[slot];
        for(uint axis=0u;axis<4u;++axis){if(slot==0u && axis==0u)continue;precise vec4 term=constants[pb+v.offsets[slot]+axis]*wp[axis];p=p+term;}
        for(uint axis=0u;axis<3u;++axis){if(slot==0u && axis==0u)continue;uint col=(slot==3u && axis==2u)?1u:axis;precise vec4 term=constants[pb+v.offsets[slot]+col]*wn[axis];n=n+term;}
    }
    precise vec4 screen=point(c,p);
    float q=float(1.0/double(screen.w));precise vec4 xyz=screen*q;
    uvec3 positionBits=uvec3(uint(int(xyz.x*16.0))&65535u,uint(int(xyz.y*16.0))&65535u,uint(int(xyz.z))&0xffffffu);
    precise vec4 light=constants[c+12u]*n.x;light=light+constants[c+13u]*n.y;light=light+constants[c+14u]*n.z;light=max(light,vec4(0));
    precise vec4 colour=constants[c+19u];colour=colour+constants[c+15u]*light.x;colour=colour+constants[c+16u]*light.y;
    colour=colour+constants[c+17u]*light.z;colour=colour+constants[c+18u]*light.w;colour=min(colour,vec4(255));
    uvec4 rgba=uvec4(ivec4(colour))&uvec4(255);
    precise float fog=constants[c+20u].w*q;fog=constants[c+20u].x+fog;fog=max(constants[c+20u].z,min(constants[c+20u].y,fog));
    uint at=meta.z*8u;vertices[at]=floatBitsToUint(float(positionBits.x)/16.0-constants[c+21u].z);
    vertices[at+1u]=floatBitsToUint(float(positionBits.y)/16.0-constants[c+21u].w);vertices[at+2u]=floatBitsToUint(float(positionBits.z));
    precise float s=v.uv.x*q,t=v.uv.y*q;
    float gq=(q==0.0)?1.0:q;float iq=(abs(gq)>1e-8)?gq:1.0;
    vertices[at+3u]=floatBitsToUint((meta.w&1u)!=0u?s:0.0);vertices[at+4u]=floatBitsToUint((meta.w&1u)!=0u?t:0.0);
    vertices[at+5u]=floatBitsToUint((meta.w&1u)!=0u?iq:1.0);vertices[at+6u]=rgba.x|(rgba.y<<8u)|(rgba.z<<16u)|(rgba.w<<24u);
    vertices[at+7u]=uint(int(fog))&255u;
}
)GLSL";
GLuint m_g789Program=0,m_g789Meta=0,m_g789Constants=0,m_g789Output=0,m_g789Vao=0;
struct G789ResidentSource {GLuint buffer=0;size_t bytes=0;std::vector<uint64_t> order;};
std::unordered_map<uint64_t,G789ResidentSource> m_g789ResidentSources;
const G178Batch* m_g789NativeBatch=nullptr;
const G178Batch* m_g789ProductionBatch=nullptr;
bool g789NativeRequested(){static const bool v=[] {const char* e=std::getenv("DC2_G789_W_NATIVE");return e && std::atoi(e)!=0;}();return v;}
bool m_g789Active=false,m_g789Failed=false;
uint32_t m_g789Samples=0;
struct G789WVersion {uint64_t draw=0,batch=0,run=0;uint32_t fbp=0,key=0;GLuint texture=0;std::map<int,std::array<GLuint,4>> snapshots;std::array<GLuint,2> fullW{};int fbH=0;};
std::map<int,std::array<GLuint,4>> m_g789PendingSnapshots;
std::array<GLuint,2> m_g789PendingFullW{};
unsigned m_g789WShadowArm=0;
std::unordered_map<uint64_t,bool> m_g789NativeDraws;
std::unordered_map<uint64_t,G789WVersion> m_g789WVersions;
std::shared_ptr<const dc2_g789_oracle::Pose> m_g789PendingPose;
std::vector<uint32_t> m_g789PendingColour;
std::vector<float> m_g789PendingDepth;
void g789Destroy(){
    for(GLuint* buffer:{&m_g789Meta,&m_g789Constants,&m_g789Output})if(*buffer){m_f.glDeleteBuffers_(1,buffer);*buffer=0;}
    for(auto& entry:m_g789ResidentSources)if(entry.second.buffer)m_f.glDeleteBuffers_(1,&entry.second.buffer);m_g789ResidentSources.clear();
    if(m_g789Program){m_f.glDeleteProgram_(m_g789Program);m_g789Program=0;}
    if(m_g789Vao){m_f.glDeleteVertexArrays_(1,&m_g789Vao);m_g789Vao=0;}
    for(auto& entry:m_g789WVersions){g789ReleaseSnapshots(entry.second.snapshots);for(GLuint texture:entry.second.fullW)if(texture)glDeleteTextures(1,&texture);}g789ReleaseSnapshots(m_g789PendingSnapshots);
    for(GLuint texture:m_g789PendingFullW)if(texture)glDeleteTextures(1,&texture);m_g789PendingFullW={};
    if(m_g789CopyFbo){m_f.glDeleteFramebuffers_(1,&m_g789CopyFbo);m_g789CopyFbo=0;}
}
bool g789Init(){
    if(m_g789Program)return true;if(m_g789Failed)return false;m_g789Failed=true;
    if(!g788Init() || (!m_g788GetBufferSubData && !loadProc(m_g788GetBufferSubData,"glGetBufferSubData")))return false;
    GLuint shader=m_f.glCreateShader_(0x91b9u);m_f.glShaderSource_(shader,1,&kG789MotionCS,nullptr);m_f.glCompileShader_(shader);
    GLint ok=0;m_f.glGetShaderiv_(shader,GL158_COMPILE_STATUS,&ok);
    if(!ok){char log[4096]{};m_f.glGetShaderInfoLog_(shader,sizeof(log),nullptr,log);std::fprintf(stderr,"[G789:wshader] %s\n",log);m_f.glDeleteShader_(shader);return false;}
    m_g789Program=m_f.glCreateProgram_();m_f.glAttachShader_(m_g789Program,shader);m_f.glLinkProgram_(m_g789Program);m_f.glDeleteShader_(shader);
    m_f.glGetProgramiv_(m_g789Program,GL158_LINK_STATUS,&ok);if(!ok){g789Destroy();return false;}
    for(GLuint* buffer:{&m_g789Meta,&m_g789Constants,&m_g789Output})m_f.glGenBuffers_(1,buffer);
    m_f.glGenVertexArrays_(1,&m_g789Vao);m_f.glBindVertexArray_(m_g789Vao);m_f.glBindBuffer_(GL158_ARRAY_BUFFER,m_g789Output);
    m_f.glVertexAttribPointer_(0,3,GL_FLOAT,GL_FALSE,sizeof(G178Vtx),reinterpret_cast<const void*>(0));
    m_f.glVertexAttribPointer_(1,3,GL_FLOAT,GL_FALSE,sizeof(G178Vtx),reinterpret_cast<const void*>(12));
    m_f.glVertexAttribPointer_(2,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(G178Vtx),reinterpret_cast<const void*>(24));
    m_f.glVertexAttribPointer_(3,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(G178Vtx),reinterpret_cast<const void*>(28));
    for(unsigned attribute=0;attribute<4u;++attribute)m_f.glEnableVertexAttribArray_(attribute);
    return true;
}
struct G789StaticInput {dc2_native::Vector position,normal,uv;std::array<uint32_t,4> offsets;dc2_native::Vector weights;};
struct G789Meta {std::array<uint32_t,4> meta;};
static_assert(sizeof(G789StaticInput)==80u && sizeof(G789Meta)==16u && sizeof(G178Vtx)==32u);
bool g789Generate(const G178Batch& original,G178Batch& candidate,const std::vector<dc2_g789_raster::Job>& jobs,size_t& attributeBad,bool compare=true){
    if(!g789Init() || jobs.empty())return false;
    constexpr GLenum storage=0x90d2u;
    const uint64_t generation=jobs.front().primitive->pose->source->generation;
    auto resident=m_g789ResidentSources.find(generation);
    bool staticUpload=false;
    if(resident==m_g789ResidentSources.end()){
        std::vector<G789StaticInput> source;std::vector<uint64_t> order;source.reserve(jobs.size()*3u);order.reserve(jobs.size());
        for(const auto& job:jobs){const auto& p=*job.primitive;if(p.pose->source->generation!=generation)return false;const auto& s=*p.pose->source;
            order.push_back((uint64_t(p.ordinal)<<32u)|p.triangle);
            for(unsigned k=0;k<3u;++k){const auto& corner=p.corners[k];G789StaticInput input{};
                input.position=dc2_g789_raster::vector(s.vectors[0],corner.position*16u);
                input.normal=dc2_g789_raster::vector(s.vectors[1],corner.normal*16u);
                input.uv=dc2_g789_raster::vector(s.vectors[3],corner.st*16u);
                std::memcpy(input.offsets.data(),s.weights.data()+corner.position*32u,16);
                std::memcpy(input.weights.data(),s.weights.data()+corner.position*32u+16u,16);
                for(uint32_t offset:input.offsets)if(offset%4u || offset+3u>=p.pose->palette.size()/16u)return false;
                source.push_back(input);
            }
        }
        G789ResidentSource value{};m_f.glGenBuffers_(1,&value.buffer);if(!value.buffer)return false;
        value.bytes=source.size()*sizeof(G789StaticInput);value.order=std::move(order);
        m_f.glBindBuffer_(storage,value.buffer);m_f.glBufferData_(storage,static_cast<GLsizeiptrLocal>(value.bytes),source.data(),GL158_STATIC_DRAW);
        resident=m_g789ResidentSources.emplace(generation,std::move(value)).first;staticUpload=true;
    }
    if(resident->second.order.size()!=jobs.size())return false;
    std::vector<G789Meta> metas;metas.reserve(jobs.size()*3u);std::vector<dc2_native::Vector> constants;
    const auto pose=jobs.front().primitive->pose;
    if(pose->palette.empty() || pose->palette.size()%16u)return false;
    for(size_t i=0;i<pose->palette.size();i+=16u)constants.push_back(dc2_g789_raster::vector(pose->palette,i));
    const size_t paletteBytes=constants.size()*sizeof(dc2_native::Vector);
    std::map<std::array<int,3>,uint32_t> materialConstants;
    size_t ordinal=0;
    for(const auto& job:jobs){
        if(job.first+3u>candidate.verts.size() || !(job.flags&2u))return false;
        const auto& p=*job.primitive;const auto& s=*p.pose->source;
        if(p.pose!=pose || s.generation!=generation || resident->second.order[ordinal++]!=((uint64_t(p.ordinal)<<32u)|p.triangle))return false;
        constexpr uint32_t kPaletteLimit=65535u;
        const uint32_t material=p.pose->source->faces[p.face].header[1]&kPaletteLimit;
        const std::array<int,3> key{int(material),job.ofx,job.ofy};
        auto found=materialConstants.find(key);uint32_t cb=0;
        if(found==materialConstants.end()){
            std::array<dc2_native::Vector,23> local;
            if(!dc2_g789_raster::constants(p,local,original.fbW,original.fbH,job.ofx,job.ofy))return false;
            cb=uint32_t(constants.size());constants.insert(constants.end(),local.begin(),local.end());materialConstants.emplace(key,cb);
        }else cb=found->second;
        constexpr uint32_t pb=0u;
        for(unsigned k=0;k<3;++k)metas.push_back(G789Meta{{cb,pb,job.first+k,job.flags}});
    }
    const auto upload=[&](GLuint buffer,const void* data,size_t size,unsigned binding){m_f.glBindBuffer_(storage,buffer);m_f.glBufferData_(storage,static_cast<GLsizeiptrLocal>(size),data,GL178_STREAM_DRAW);m_e.glBindBufferBase_(storage,binding,buffer);};
    m_e.glBindBufferBase_(storage,0,resident->second.buffer);
    upload(m_g789Meta,metas.data(),metas.size()*sizeof(G789Meta),3);
    upload(m_g789Constants,constants.data(),constants.size()*sizeof(dc2_native::Vector),1);
    // Unselected primitives preserve the original batch in both independent arms.
    upload(m_g789Output,candidate.verts.data(),candidate.verts.size()*sizeof(G178Vtx),2);
    m_f.glUseProgram_(m_g789Program);m_f.glUniform1i_(0,GLint(metas.size()));
    m_e.glDispatchCompute_(uint32_t((metas.size()+63u)/64u),1,1);m_e.glMemoryBarrier_(0x2000u|0x0200u|0x0001u);
    std::fprintf(stderr,"[G789:resident] source=%llu staticBytes=%zu staticUpload=%u vertices=%zu metadataBytes=%zu paletteBytes=%zu materialGroups=%zu constantsBytes=%zu\n",(unsigned long long)generation,resident->second.bytes,unsigned(staticUpload),metas.size(),metas.size()*sizeof(G789Meta),paletteBytes,materialConstants.size(),constants.size()*sizeof(dc2_native::Vector));
    m_f.glBindBuffer_(storage,m_g789Output);m_g788GetBufferSubData(storage,0,static_cast<GLsizeiptrLocal>(candidate.verts.size()*sizeof(G178Vtx)),candidate.verts.data());
    attributeBad=0;
    if(compare)for(const auto& job:jobs)for(unsigned k=0;k<3;++k){const auto& a=original.verts[job.first+k];const auto& b=candidate.verts[job.first+k];
        const bool exact=a.x==b.x && a.y==b.y && a.z==b.z && a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a && a.fog==b.fog;
        const bool stq=std::fabs(a.sq-b.sq)<=2e-6f && std::fabs(a.tq-b.tq)<=2e-6f && std::fabs(a.iq-b.iq)<=2e-6f;
        if(!exact || !stq){++attributeBad;if(attributeBad<=3u)std::fprintf(stderr,"[G789:wattr] draw=%llu batch=%u face=%u k=%u expected=(%g,%g,%g c=%u,%u,%u,%u f=%u stq=%g,%g,%g) candidate=(%g,%g,%g c=%u,%u,%u,%u f=%u stq=%g,%g,%g)\n",(unsigned long long)job.primitive->pose->identity.draw,job.primitive->ordinal,job.primitive->face,k,a.x,a.y,a.z,a.r,a.g,a.b,a.a,a.fog,a.sq,a.tq,a.iq,b.x,b.y,b.z,b.r,b.g,b.b,b.a,b.fog,b.sq,b.tq,b.iq);}
    }
    return glGetError()==GL_NO_ERROR;
}
// Complete structured-source topology, including primitives the GS frontend
// rejected. Legacy vertices supply only the independent attribute oracle and
// existing material-state templates; they never select candidate geometry.
bool g789FullSource(const G178Batch& original,G178Batch& candidate,const std::vector<dc2_g789_raster::Job>& oracle,std::vector<dc2_g789_raster::Job>& complete,size_t& attributeBad){
    if(oracle.empty())return false;const auto pose=oracle.front().primitive->pose;
    const auto refuse=[&](unsigned reason){std::fprintf(stderr,"[G789:wsource] draw=%llu refused=%u vertices=%zu selected=%zu draws=%zu\n",(unsigned long long)pose->identity.draw,reason,original.verts.size(),oracle.size()*3u,original.draws.size());return false;};
    FILE* state=dc2_g789_oracle::file("rasterstate_"+std::to_string(pose->identity.draw)+".json");
    std::fprintf(state,"{\"draw\":%llu,\"vertices\":%zu,\"draws\":[",(unsigned long long)pose->identity.draw,original.verts.size());
    for(size_t d=0;d<original.draws.size();++d){const auto& v=original.draws[d];std::fprintf(state,"%s{\"first\":%d,\"count\":%d,\"texture\":%llu,\"source\":%u,\"blend\":%u,\"tfx\":%u,\"tcc\":%u,\"fog\":%u,\"depth\":%u,\"depthWrite\":%u,\"scissor\":[%u,%u,%u,%u]}",d?",":"",v.firstVtx,v.vtxCount,(unsigned long long)v.texKey,v.srcFbp,v.blend,v.tfx,v.tcc,v.fge,v.depthFunc,unsigned(v.depthWrite),v.scX0,v.scY0,v.scX1,v.scY1);}
    std::fprintf(state,"],\"geometry\":[");for(size_t n=0;n<oracle.size();++n){const auto& j=oracle[n];const auto& p=*j.primitive;std::fprintf(state,"%s[%u,%u,%u,%u,%u,%d,%d,%u]",n?",":"",j.first,p.face,p.ordinal,p.triangle,unsigned(p.pose==pose),j.ofx,j.ofy,j.flags);}std::fprintf(state,"]}\n");dc2_g789_oracle::close(state);
    auto topology=dc2_g789_raster::topology(pose->source->generation);if(!topology)return refuse(10u);
    if(original.verts.size()%3u)return refuse(1u);
    std::vector<int> bucketDraw(topology->buckets,-1);std::vector<const dc2_g789_raster::Job*> occupied(original.verts.size()/3u,nullptr),stateJobs(original.draws.size(),nullptr);
    for(const auto& job:oracle){
        const auto& p=*job.primitive;
        if(p.pose!=pose || job.first%3u || job.first+3u>original.verts.size() || !p.ordinal || p.ordinal>topology->packets.size() || occupied[job.first/3u])return refuse(2u);
        occupied[job.first/3u]=&job;const auto& packet=topology->packets[p.ordinal-1u];if(packet.face!=p.face)return refuse(3u);
        int draw=-1;for(size_t d=0;d<original.draws.size();++d)if(job.first>=unsigned(original.draws[d].firstVtx) && job.first+3u<=unsigned(original.draws[d].firstVtx+original.draws[d].vtxCount)){if(draw>=0)return false;draw=int(d);}
        if(draw<0 || (bucketDraw[packet.bucket]>=0 && bucketDraw[packet.bucket]!=draw))return refuse(4u);bucketDraw[packet.bucket]=draw;
        const auto previous=stateJobs[draw];if(previous && (previous->ofx!=job.ofx || previous->ofy!=job.ofy || previous->flags!=job.flags))return refuse(5u);stateJobs[draw]=&job;
    }
    if(std::any_of(bucketDraw.begin(),bucketDraw.end(),[](int d){return d<0;}) || !std::is_sorted(bucketDraw.begin(),bucketDraw.end()))return refuse(6u);
    uint32_t firstSelected=uint32_t(original.verts.size()),lastSelected=0;
    for(const auto& job:oracle){firstSelected=std::min(firstSelected,job.first);lastSelected=std::max(lastSelected,job.first+3u);}
    if(lastSelected-firstSelected!=oracle.size()*3u)return refuse(11u);
    candidate.verts.clear();candidate.draws.clear();std::unordered_map<uint64_t,uint32_t> candidateIndex;
    for(size_t draw=0;draw<original.draws.size();++draw){
        auto state=original.draws[draw];state.firstVtx=int(candidate.verts.size());state.vtxCount=0;
        const uint32_t start=uint32_t(original.draws[draw].firstVtx),end=start+uint32_t(original.draws[draw].vtxCount);
        if(start%3u || end>original.verts.size())return refuse(12u);
        const auto keep=[&](uint32_t lo,uint32_t hi){if(lo>=hi)return;auto retained=original.draws[draw];retained.firstVtx=int(candidate.verts.size());retained.vtxCount=int(hi-lo);candidate.verts.insert(candidate.verts.end(),original.verts.begin()+lo,original.verts.begin()+hi);candidate.draws.push_back(retained);};
        if(!stateJobs[draw]){keep(start,end);continue;}
        keep(start,std::min(end,firstSelected));state.firstVtx=int(candidate.verts.size());
        for(const auto& packet:topology->packets)if(bucketDraw[packet.bucket]==int(draw)){
            for(uint32_t c=2u,triangle=0u;c<packet.corners.size();c+=packet.kind==3u?3u:1u,++triangle){
                auto p=std::make_shared<dc2_g789_raster::Primitive>();p->pose=pose;p->face=packet.face;p->ordinal=packet.ordinal;p->triangle=triangle;
                for(unsigned k=0;k<3u;++k)p->corners[k]=packet.corners[c-2u+k];
                dc2_g789_raster::Job job;job.first=uint32_t(candidate.verts.size());job.primitive=std::move(p);
                // XYOFFSET/TME/IIP are raster state, not derived from source bytes.
                const auto templateJob=stateJobs[draw];job.ofx=templateJob->ofx;job.ofy=templateJob->ofy;job.flags=templateJob->flags;
                candidateIndex.emplace((uint64_t(packet.ordinal)<<32u)|triangle,job.first);complete.push_back(std::move(job));
                candidate.verts.resize(candidate.verts.size()+3u);state.vtxCount+=3;
            }
        }
        if(state.vtxCount)candidate.draws.push_back(state);
        keep(std::max(start,lastSelected),end);
    }
    if(complete.size()!=topology->triangles)return false;
    if(!g789Generate(original,candidate,complete,attributeBad,false))return false;
    FILE* dump=dc2_g789_oracle::file("sourcegpu_"+std::to_string(pose->identity.draw)+".bin");
    const uint32_t h[]={0x47375347u,1u,uint32_t(pose->identity.draw),uint32_t(pose->identity.draw>>32u),40u,0u,uint32_t(complete.size()),uint32_t(candidate.verts.size())};
    dc2_g789_oracle::write(dump,h,sizeof(h));
    for(const auto& job:complete){const uint32_t row[]={job.primitive->ordinal,job.primitive->triangle,job.first,uint32_t(job.ofx),uint32_t(job.ofy)};dc2_g789_oracle::write(dump,row,sizeof(row));}
    dc2_g789_oracle::write(dump,candidate.verts.data(),candidate.verts.size()*sizeof(G178Vtx));dc2_g789_oracle::close(dump);
    attributeBad=0;
    if(!dc2_g789_live::deletion())for(const auto& job:oracle){const auto& p=*job.primitive;auto at=candidateIndex.find((uint64_t(p.ordinal)<<32u)|p.triangle);if(at==candidateIndex.end())return false;
        for(unsigned k=0;k<3u;++k){const auto& a=original.verts[job.first+k];const auto& b=candidate.verts[at->second+k];
            attributeBad+=!(a.x==b.x && a.y==b.y && a.z==b.z && a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a && a.fog==b.fog && std::fabs(a.sq-b.sq)<=2e-6f && std::fabs(a.tq-b.tq)<=2e-6f && std::fabs(a.iq-b.iq)<=2e-6f);}
    }
    return true;
}
// Production transport can split a material bucket at legacy-culling gaps.
// Private oracle arms restore the original state runs and clear every production
// pointer. The already accepted complete-source model remains unchanged.
bool g789LegacyOracleArm(const G178Batch& input,G178Batch& arm){
    arm=input;arm.g789Live.reset();arm.draws.clear();
    std::shared_ptr<const dc2_g789_live::Draw> previous;
    const auto same=[](const G178Draw& a,const G178Draw& b){return
        a.texKey==b.texKey && a.srcFbp==b.srcFbp && a.blend==b.blend && a.tfx==b.tfx && a.tcc==b.tcc &&
        a.fge==b.fge && a.fogCol==b.fogCol && a.depthFunc==b.depthFunc && a.depthWrite==b.depthWrite &&
        a.bilinear==b.bilinear && a.wrapU==b.wrapU && a.wrapV==b.wrapV &&
        a.uvOriginX==b.uvOriginX && a.uvOriginY==b.uvOriginY && a.uvRoundU==b.uvRoundU && a.uvRoundV==b.uvRoundV &&
        a.uvPeriodU==b.uvPeriodU && a.uvPeriodV==b.uvPeriodV && a.screenSprite==b.screenSprite &&
        a.scX0==b.scX0 && a.scY0==b.scY0 && a.scX1==b.scX1 && a.scY1==b.scY1 && !a.g788Seq && !b.g788Seq;};
    for(const auto& draw:input.draws){
        auto legacy=draw;legacy.g789Live.reset();legacy.g789First=legacy.g789Count=0;
        if(draw.g789Live && previous==draw.g789Live && !arm.draws.empty() &&
           arm.draws.back().firstVtx+arm.draws.back().vtxCount==legacy.firstVtx && same(arm.draws.back(),legacy))
            arm.draws.back().vtxCount+=legacy.vtxCount;
        else arm.draws.push_back(legacy);
        previous=draw.g789Live;
    }
    return true;
}
void g789ShadowRaster(G178Batch& original,uint64_t depthKey,const std::vector<float>* depthUpload,int depthY,int depthRows){
    if(m_g789Active)return;m_g789NativeBatch=nullptr;m_g789ProductionBatch=nullptr;g789ReleaseSnapshots(m_g789PendingSnapshots);
    for(GLuint texture:m_g789PendingFullW)if(texture)glDeleteTextures(1,&texture);m_g789PendingFullW={};
    if(!dc2_g789_raster::on() || m_g789Samples>=256u)return;
    const auto owner=dc2_g789_raster::batch(original.g778Shadow);if(!owner || owner->empty())return;const auto& jobs=*owner;
    if(original.fbp<0x139u || original.fbp>=0x1b9u || !original.g788Jobs.empty() || original.g777Native || original.g778Shadow)return;
    if(std::any_of(original.draws.begin(),original.draws.end(),[](const G178Draw& d){return d.srcFbp!=0u;}))return;
    m_g789Active=true;struct Reset {bool& value;~Reset(){value=false;}} reset{m_g789Active};
    const auto& identity=jobs.front().primitive->pose->identity;
    const unsigned sample=++m_g789Samples;
    const auto fail=[&](unsigned reason){std::fprintf(stderr,"[G789:wshadow] frame=%u draw=%llu sample=%u refused=%u\n",identity.frame,(unsigned long long)identity.draw,sample,reason);};
    if(dc2_g789_live::deletion()){
        bool committed=false;
        for(const auto& draw:original.draws)if(draw.g789Live && draw.g789Live->identity.draw==identity.draw){
            if(!draw.g789Live->committed){fail(12);return;}committed=true;
        }
        if(!committed){fail(13);return;}
    }
    constexpr uint32_t expectedKey=0x78910001u,candidateKey=0x78910002u;
    constexpr uint64_t expectedDepth=0x7891000100000001ull,candidateDepth=0x7891000200000001ull;
    GLint drawFbo=0,readFbo=0,viewport[4]{};glGetIntegerv(0x8ca6,&drawFbo);glGetIntegerv(0x8caa,&readFbo);glGetIntegerv(GL_VIEWPORT,viewport);
    struct Restore {decltype(m_f)& f;GLint draw,read;GLint* viewport;~Restore(){f.glBindFramebuffer_(0x8ca9,draw);f.glBindFramebuffer_(0x8ca8,read);glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);}} restore{m_f,drawFbo,readFbo,viewport};
    if(!g778SeedShadow(original,depthKey,expectedKey,expectedDepth,depthUpload!=nullptr) || !g778SeedShadow(original,depthKey,candidateKey,candidateDepth,depthUpload!=nullptr)){fail(1);return;}
    G178Batch legacy;
    if(!g789LegacyOracleArm(original,legacy)){fail(7);return;}
    auto expected=legacy,candidate=legacy;expected.fboKey=expectedKey;candidate.fboKey=candidateKey;
    expected.skipReadback=candidate.skipReadback=true;expected.g778Shadow.reset();candidate.g778Shadow.reset();
    std::vector<dc2_g789_raster::Job> complete;size_t attributeBad=0;
    if(dc2_g789_live::deletion()){
        // Reuse accepted complete-source construction as the independent reference.
        // Markers supply exact identity/state ranges, never placeholder attributes.
        if(!g789FullSource(legacy,expected,jobs,complete,attributeBad)){fail(6);return;}
        expected.fboKey=expectedKey;expected.skipReadback=true;
        candidate=original;candidate.fboKey=candidateKey;candidate.skipReadback=true;candidate.g778Shadow.reset();
    }else if(!g789FullSource(legacy,candidate,jobs,complete,attributeBad)){fail(6);return;}
    std::vector<uint32_t> seed,a,b;std::vector<float> seedZ,az,bz;
    g778ReadShadow(expectedKey,depthKey?expectedDepth:0u,expected,seed,seedZ);
    m_g789WShadowArm=g789NativeRequested()?1u:0u;
    if(!renderBatch(expected,depthKey?expectedDepth:0u,depthUpload,depthY,depthRows)){m_g789WShadowArm=0;fail(3);return;}
    m_g789WShadowArm=0;
    g778ReadShadow(expectedKey,depthKey?expectedDepth:0u,expected,a,az);
    if(g789NativeRequested())g789CaptureVersions(0u);
    if(g789NativeRequested()){const auto& surface=m_fbos.at(g349FboKey(expectedKey,original.fbW));m_g789PendingFullW[0]=g789CloneVersion(surface.colorTex,surface.physW,surface.physH);}
    m_g789WShadowArm=g789NativeRequested()?2u:0u;
    if(!renderBatch(candidate,depthKey?candidateDepth:0u,depthUpload,depthY,depthRows)){m_g789WShadowArm=0;fail(4);return;}
    m_g789WShadowArm=0;
    g778ReadShadow(candidateKey,depthKey?candidateDepth:0u,candidate,b,bz);
    if(g789NativeRequested())g789CaptureVersions(1u);
    if(g789NativeRequested()){const auto& surface=m_fbos.at(g349FboKey(candidateKey,original.fbW));m_g789PendingFullW[1]=g789CloneVersion(surface.colorTex,surface.physW,surface.physH);}
    if(a.size()!=b.size() || az.size()!=bz.size() || seed.size()!=a.size() || glGetError()!=GL_NO_ERROR){fail(5);return;}
    size_t colourBad=0,depthBad=0,touched=0;
    for(size_t i=0;i<a.size();++i){colourBad+=a[i]!=b[i];depthBad+=i<az.size() && std::memcmp(&az[i],&bz[i],4)!=0;
        touched+=a[i]!=seed[i] || (i<az.size() && i<seedZ.size() && std::memcmp(&az[i],&seedZ[i],4)!=0);}
    if(sample==1u){
        const auto& surface=m_fbos.at(g349FboKey(expectedKey,original.fbW));
        FILE* dump=dc2_g789_oracle::file("wshadow_"+std::to_string(identity.draw)+".bin");
        const uint32_t header[]={0x47375753u,1u,identity.frame,identity.sf,uint32_t(identity.draw),uint32_t(identity.draw>>32),identity.object,identity.visual,original.fbp,uint32_t(surface.physW),uint32_t(surface.physH),uint32_t(depthKey),uint32_t(depthKey>>32),uint32_t(jobs.size())};
        dc2_g789_oracle::write(dump,header,sizeof(header));
        for(const auto* pixels:{&seed,&a,&b})dc2_g789_oracle::write(dump,pixels->data(),pixels->size()*4u);
        for(const auto* z:{&seedZ,&az,&bz})dc2_g789_oracle::write(dump,z->data(),z->size()*4u);
        dc2_g789_oracle::close(dump);
    }
    std::fprintf(stderr,"[G789:wsource] draw=%llu source=40 packets=131 corners=1240 triangles=%zu survivingLegacy=%zu orderedSourceOnly=1 nativeAdmission=0\n",(unsigned long long)identity.draw,complete.size(),jobs.size());
    std::fprintf(stderr,"[G789:wshadow] frame=%u sf=%u draw=%llu object=%x visual=%x instance=%llu W=%x size=%dx%d depth=%llx sample=%u triangles=%zu pixels=%zu touched=%zu attributeBad=%zu colourDifferent=%zu depthDifferent=%zu published=%llu consumed=%llu matched=%llu unmatched=%llu refused=%llu\n",identity.frame,identity.sf,(unsigned long long)identity.draw,identity.object,identity.visual,(unsigned long long)jobs.front().primitive->pose->instance,original.fbp,original.fbW,original.fbH,(unsigned long long)depthKey,sample,jobs.size(),a.size(),touched,attributeBad,colourBad,depthBad,(unsigned long long)dc2_g789_raster::published.load(),(unsigned long long)dc2_g789_raster::consumed.load(),(unsigned long long)dc2_g789_raster::matched.load(),(unsigned long long)dc2_g789_raster::unmatched.load(),(unsigned long long)dc2_g789_raster::refused.load());
    m_g789PendingPose=jobs.front().primitive->pose;m_g789PendingColour=std::move(b);m_g789PendingDepth=std::move(bz);
    if(g789NativeRequested()){
        // This prototype retains every original kick as the same-frame oracle.
        // Refuse changed frontend ranges until native markers own those ranges;
        // G786 snapshot boundaries must retain their exact original indices.
        if(attributeBad || colourBad || depthBad || (!dc2_g789_live::deletion() && original.verts.size()!=candidate.verts.size()) || dc2_g789_raster::unmatched.load() || dc2_g789_raster::refused.load()){
            std::fprintf(stderr,"[G789:wnative] draw=%llu committed=0 refused=oracleOrRanges kicksSuppressed=0\n",(unsigned long long)identity.draw);
        }else{
            if(dc2_g789_live::requested()){
                size_t covered=0;
                for(const auto& job:jobs)for(const auto& draw:original.draws)
                    if(draw.g789Live && draw.g789Live->identity.draw==identity.draw && job.first>=unsigned(draw.firstVtx) && job.first+3u<=unsigned(draw.firstVtx+draw.vtxCount))++covered;
                if(covered!=jobs.size()){
                    std::fprintf(stderr,"[G789:wnative] draw=%llu committed=0 refused=productionCoverage covered=%zu expected=%zu kicksSuppressed=0\n",(unsigned long long)identity.draw,covered,jobs.size());return;
                }
                m_g789ProductionBatch=&original;
            }else{
                original.verts=std::move(candidate.verts);original.draws=std::move(candidate.draws);m_g789NativeBatch=&original;
            }
            m_g789NativeDraws.emplace(identity.draw,true);
            const char* mechanism=dc2_g789_live::requested()?(dc2_g789_live::mechanism()==dc2_g789_live::Mechanism::Vertex?"productionVertex":"productionCompute"):"computeDirectGpu";
            std::fprintf(stderr,"[G789:wnative] draw=%llu committed=1 source=40 instance=%llu triangles=%zu mechanism=%s kicksSuppressed=%u legacyGeometryAbsent=%u\n",(unsigned long long)identity.draw,(unsigned long long)m_g789PendingPose->instance,complete.size(),mechanism,dc2_g789_live::deletion()?131u:0u,unsigned(dc2_g789_live::deletion()));
        }
    }
}
void g789MainFinished(const G178Batch& b,uint64_t depthKey){
    if(m_g789Active || !m_g789PendingPose)return;
    auto jobs=dc2_g789_raster::batch(b.g778Shadow);if(!jobs || jobs->empty() || jobs->front().primitive->pose!=m_g789PendingPose)return;
    const auto& pose=*m_g789PendingPose;const auto& id=pose.identity;
    const uint32_t key=b.fboKey?b.fboKey:b.fbp;auto i=m_fbos.find(g349FboKey(key,b.fbW));if(i==m_fbos.end())return;auto& fbo=i->second;
    GLint draw=0,read=0;glGetIntegerv(0x8ca6,&draw);glGetIntegerv(0x8caa,&read);
    std::vector<uint32_t> colour;std::vector<float> z;g778ReadShadow(key,depthKey,b,colour,z);
    if(depthKey)m_e.glFramebufferRenderbuffer_(0x8d40,0x8d00,GL178_RENDERBUFFER,fbo.depthRb);
    m_f.glBindFramebuffer_(0x8ca9,draw);m_f.glBindFramebuffer_(0x8ca8,read);
    size_t colourBad=0,depthBad=0;if(colour.size()!=m_g789PendingColour.size() || z.size()!=m_g789PendingDepth.size()){colourBad=depthBad=size_t(-1);}
    else for(size_t n=0;n<colour.size();++n){colourBad+=colour[n]!=m_g789PendingColour[n];depthBad+=std::memcmp(&z[n],&m_g789PendingDepth[n],4)!=0;}
    m_g789WVersions[id.draw]={id.draw,m_batchCounter,m_g786RunNo,b.fbp,key,fbo.colorTex};
    if(m_g789NativeBatch==&b || m_g789ProductionBatch==&b){auto& version=m_g789WVersions[id.draw];version.snapshots=std::move(m_g789PendingSnapshots);version.fullW=m_g789PendingFullW;version.fbH=b.fbH;m_g789PendingFullW={};}
    std::fprintf(stderr,"[G789:wproducer] frame=%u sf=%u draw=%llu object=%x visual=%x instance=%llu W=%x key=%x textureId=%u batch=%llu run=%llu snapshots=%d pixels=%zu colourDifferent=%zu depthDifferent=%zu glError=%u\n",id.frame,id.sf,(unsigned long long)id.draw,id.object,id.visual,(unsigned long long)pose.instance,b.fbp,key,fbo.colorTex,(unsigned long long)m_batchCounter,(unsigned long long)m_g786RunNo,g_g786SnapN,colour.size(),colourBad,depthBad,unsigned(glGetError()));
    m_g789PendingPose.reset();m_g789PendingColour.clear();m_g789PendingDepth.clear();
}
// Read-only exact consumer observation. It runs around the actual ordered draw;
// texture selection, G786 snapshots and G787 sprite expansion are unchanged.
struct G789ConsumerTrace {
    std::shared_ptr<const dc2_g789_raster::Composite> consumer;
    std::vector<uint32_t> before;
    uint32_t target=0,source=0;int width=0,height=0,first=0,count=0;
    GLuint texture=0;int slot=-1;uint64_t run=0,snapshotBatch=0;
};
std::shared_ptr<G789ConsumerTrace> m_g789ConsumerTrace;
unsigned m_g789ConsumerSamples=0;
bool m_g789BodyDumped=false;
void g789ReadCurrent(std::vector<uint32_t>& pixels,int width,int height){
    GLint draw=0,read=0;glGetIntegerv(0x8ca6,&draw);glGetIntegerv(0x8caa,&read);
    m_f.glBindFramebuffer_(0x8ca8,draw);pixels.resize(size_t(width)*height);
    glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    m_f.glBindFramebuffer_(0x8ca8,read);
}
std::shared_ptr<void> g789TraceDraw(const G178Batch& b,const G178Draw& d){
    // 1024 covered only frames 1800-1806 of the 32-pose interval (~150 consumers/pose).
    if(!dc2_g789_raster::on() || m_g789Active || m_g789ConsumerSamples>=8192u)return {};
    auto transport=dc2_g789_raster::transport(b.g778Shadow);if(!transport || !transport->consumers)return {};
    std::shared_ptr<const dc2_g789_raster::Composite> consumer;unsigned covered=0;
    for(const auto& j:*transport->consumers)if(j.first>=unsigned(d.firstVtx) && j.first+j.count<=unsigned(d.firstVtx+d.vtxCount)){
        if(consumer && consumer!=j.composite)return {};consumer=j.composite;covered+=j.count;
    }
    if(!consumer || covered!=unsigned(d.vtxCount)){
        if(consumer)std::fprintf(stderr,"[G789:wtransport] stage=draw refused=coverage seq=%u first=%d count=%d covered=%u\n",consumer->seq,d.firstVtx,d.vtxCount,covered);
        return {};
    }
    const auto fbo=m_fbos.find(g349FboKey(b.fboKey?b.fboKey:b.fbp,b.fbW));if(fbo==m_fbos.end())return {};
    auto value=std::make_shared<G789ConsumerTrace>();value->consumer=consumer;value->target=b.fbp;
    value->width=fbo->second.physW;value->height=fbo->second.physH;value->first=d.firstVtx;value->count=d.vtxCount;
    g789ReadCurrent(value->before,value->width,value->height);m_g789ConsumerTrace=value;
    return std::shared_ptr<void>(nullptr,[this,value](void*){
        std::vector<uint32_t> after;g789ReadCurrent(after,value->width,value->height);
        size_t changed=0;int x0=value->width,y0=value->height,x1=-1,y1=-1;
        for(size_t i=0;i<after.size();++i)if(after[i]!=value->before[i]){++changed;const int x=int(i%value->width),y=value->height-1-int(i/value->width);x0=std::min(x0,x);x1=std::max(x1,x);y0=std::min(y0,y);y1=std::max(y1,y);}
        const auto& c=*value->consumer;const auto& p=*c.pose;const auto& id=p.identity;
        const unsigned sample=++m_g789ConsumerSamples;
        if(sample<=4u || (c.kind==2u && changed && !m_g789BodyDumped)){if(c.kind==2u && changed)m_g789BodyDumped=true;FILE* f=dc2_g789_oracle::file("consumer_"+std::to_string(c.seq)+"_"+std::to_string(value->first)+".bin");
            const uint32_t h[]={0x47374350u,1u,id.frame,id.sf,uint32_t(id.draw),uint32_t(id.draw>>32),id.object,id.visual,c.seq,c.kind,value->target,value->source,uint32_t(value->width),uint32_t(value->height),uint32_t(value->first),uint32_t(value->count)};
            dc2_g789_oracle::write(f,h,sizeof(h));dc2_g789_oracle::write(f,value->before.data(),value->before.size()*4u);dc2_g789_oracle::write(f,after.data(),after.size()*4u);dc2_g789_oracle::close(f);}
        const auto producer=m_g789WVersions.find(id.draw);const uint64_t producerBatch=producer==m_g789WVersions.end()?0:producer->second.batch;
        const uint32_t producerW=producer==m_g789WVersions.end()?0:producer->second.fbp;
        std::fprintf(stderr,"[G789:wfinal] frame=%u sf=%u draw=%llu object=%x visual=%x instance=%llu seq=%u kind=%u target=%x source=%x textureId=%u slot=%d versionRun=%llu snapshotBatch=%llu producerBatch=%llu producerW=%x first=%d vertices=%d pixels=%zu changed=%zu bounds=%d,%d,%d,%d sample=%u glError=%u\n",id.frame,id.sf,(unsigned long long)id.draw,id.object,id.visual,(unsigned long long)p.instance,c.seq,c.kind,value->target,value->source,value->texture,value->slot,(unsigned long long)value->run,(unsigned long long)value->snapshotBatch,(unsigned long long)producerBatch,producerW,value->first,value->count,after.size(),changed,x0,y0,x1,y1,sample,unsigned(glGetError()));
        m_g789ConsumerTrace.reset();
    });
}
GLuint g789TraceTexture(GLuint texture,uint32_t source){
    if(m_g789FinalReplay)return g789FinalTexture(texture,source);
    if(!m_g789ConsumerTrace)return texture;auto& v=*m_g789ConsumerTrace;v.texture=texture;v.source=source;
    for(int slot=0;slot<kG786MaxSlots;++slot)if(m_g786Valid[slot] && (texture==m_g786WTex[slot] || texture==m_g786ATex[slot])){
        v.slot=slot;v.run=m_g786SlotRun[slot];v.snapshotBatch=m_g786SnapBatchNo[slot];break;
    }
    return texture;
}
#endif
