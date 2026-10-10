// Production GPU transport, independent of G789 diagnostic output/readback.
#if defined(PS2X_G777_NATIVE_GL)
enum class G789SkinArm { Legacy, Compute, Vertex };
static G789SkinArm g789LiveArm() {
    switch(dc2_g789_live::mechanism()){
        case dc2_g789_live::Mechanism::Vertex:return G789SkinArm::Vertex;
        case dc2_g789_live::Mechanism::Compute:return G789SkinArm::Compute;
        default:return G789SkinArm::Legacy;
    }
}

struct G789LiveBuffers {GLuint metadata=0,constants=0,output=0,vao=0;uint64_t draw=0;bool executed=false;};
struct G789LiveResident {
    std::shared_ptr<const dc2_g789_live::Source> source;
    GLuint input=0;
    std::array<G789LiveBuffers,16> poses{};
};
std::unordered_map<uint64_t,G789LiveResident> m_g789LiveSources;
GLuint m_g789LiveCompute=0,m_g789LiveVertex=0,m_g789LiveVertexVao=0;
bool m_g789LiveProgramsFailed=false,m_g789LiveUsingVertex=false;
G789LiveBuffers* m_g789LiveBuffers=nullptr;

// Give the sibling exactly the regular raster program's uniform locations.
// Reflection/string editing happens at cold program preparation only.
std::string g789LiveUniforms(std::string shader) {
    const std::string version="#version 330 core";
    size_t ver=shader.find(version);
    if(ver!=std::string::npos)shader.replace(ver,version.size(),"#version 430 core");
    size_t at=0;
    while((at=shader.find("\nuniform ",at))!=std::string::npos) {
        ++at;
        const size_t end=shader.find(';',at);
        if(end==std::string::npos)break;
        const size_t type=shader.find_first_not_of(" \t",at+8);
        const size_t space=shader.find_first_of(" \t",type);
        const size_t name=shader.find_first_not_of(" \t",space);
        const size_t stop=shader.find_first_of("[ ;\t\r\n",name);
        if(type==std::string::npos || space==std::string::npos || name==std::string::npos || stop>end)break;
        const std::string token=shader.substr(name,stop-name);
        const GLint location=m_f.glGetUniformLocation_(m_prog,token.c_str());
        if(location>=0) {
            const std::string qualifier="layout(location="+std::to_string(location)+") ";
            shader.insert(at,qualifier);at=end+qualifier.size()+1;
        } else at=end+1;
    }
    return shader;
}
bool g789LivePrograms(const std::string& selectedVs,const std::string& selectedFs) {
    if(g789LiveArm()==G789SkinArm::Legacy)return true;
    if(g789LiveArm()==G789SkinArm::Vertex?m_g789LiveVertex!=0:m_g789LiveCompute!=0)return true;
    if(m_g789LiveProgramsFailed)return false;
    m_g789LiveProgramsFailed=true;
    if(!g788Init() || !m_e.glBufferSubData_)return false;
    const std::string vertex=g789LiveUniforms(dc2_g789_live::vertexShader(selectedVs));
    const std::string fragment=g789LiveUniforms(selectedFs);
    if(vertex.empty())return false;
    const auto link=[&](const char* first,GLenum type,const char* second)->GLuint {
        GLuint a=0,b=0;std::string error;
        if(!g158CompileShader(m_f,type,first,a,error) ||
           (second && !g158CompileShader(m_f,GL158_FRAGMENT_SHADER,second,b,error))) {
            if(a)m_f.glDeleteShader_(a);if(b)m_f.glDeleteShader_(b);
            std::fprintf(stderr,"[G789:live-resource] shader preparation failed: %s\n",error.c_str());return 0;
        }
        GLuint program=m_f.glCreateProgram_();m_f.glAttachShader_(program,a);
        if(b)m_f.glAttachShader_(program,b);m_f.glLinkProgram_(program);
        GLint ok=0;m_f.glGetProgramiv_(program,GL158_LINK_STATUS,&ok);
        m_f.glDeleteShader_(a);if(b)m_f.glDeleteShader_(b);
        if(!ok){m_f.glDeleteProgram_(program);return 0;}return program;
    };
#if defined(PS2X_G775_DIAG)
    if(g789LiveArm()==G789SkinArm::Compute){
        const std::string compute=dc2_g789_live::computeShader();
        m_g789LiveCompute=link(compute.c_str(),0x91b9u,nullptr);
        ++dc2_g789_live::shaderCompiles;return m_g789LiveCompute!=0;
    }
#endif
    m_g789LiveVertex=link(vertex.c_str(),GL158_VERTEX_SHADER,fragment.c_str());
    ++dc2_g789_live::shaderCompiles;
    if(!m_g789LiveVertex)return false;
    m_f.glGenVertexArrays_(1,&m_g789LiveVertexVao);
    m_f.glUseProgram_(m_g789LiveVertex);
    m_e.glUniform2f_(m_locG675Sprite,0.0f,1.0f);
    m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g789LiveVertex,"uTex"),0);
    for(unsigned unit=1;unit<8;++unit) {
        const std::string name="uG309Src"+std::to_string(unit);
        m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g789LiveVertex,name.c_str()),unit);
    }
    m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g789LiveVertex,"uG309PageMap"),kG309PageMapUnit);
    m_f.glUniform1i_(m_f.glGetUniformLocation_(m_g789LiveVertex,"uG309Base"),kG309BaseUnit);
    m_f.glUseProgram_(m_prog);return true;
}
void g789LiveDestroy() {
    for(auto& entry:m_g789LiveSources) {
        auto& resident=entry.second;resident.source->ready.store(0,std::memory_order_release);
        if(resident.input)m_f.glDeleteBuffers_(1,&resident.input);
        for(auto& pose:resident.poses) {
            for(GLuint* buffer:{&pose.metadata,&pose.constants,&pose.output})if(*buffer)m_f.glDeleteBuffers_(1,buffer);
            if(pose.vao)m_f.glDeleteVertexArrays_(1,&pose.vao);
        }
    }
    m_g789LiveSources.clear();
    for(GLuint* program:{&m_g789LiveCompute,&m_g789LiveVertex})if(*program){m_f.glDeleteProgram_(*program);*program=0;}
    if(m_g789LiveVertexVao){m_f.glDeleteVertexArrays_(1,&m_g789LiveVertexVao);m_g789LiveVertexVao=0;}
    m_g789LiveBuffers=nullptr;
}
bool g789LivePrepare(const G178Batch& batch,const std::shared_ptr<const dc2_g789_live::Draw>& live={}) {
    const auto& carrier=live?live:batch.g789Live;
    m_g789LiveBuffers=nullptr;if(!carrier)return true;
    using namespace dc2_g789_live;
    const Draw& draw=*carrier;
    if(g789LiveArm()==G789SkinArm::Legacy || !draw.source ||
       draw.identity.source!=draw.source->generation || draw.identity.topology!=draw.source->topology ||
       (g789LiveArm()==G789SkinArm::Vertex?!m_g789LiveVertex:!m_g789LiveCompute) || !batch.g788Jobs.empty() || batch.g777Native)return false;
    auto found=m_g789LiveSources.find(draw.identity.source);
    constexpr GLenum storage=0x90d2u;
    if(found==m_g789LiveSources.end()) {
        if(draw.committed){++coldCommitted;return false;} // admission must wait for the immutable resource receipt
        G789LiveResident resident{};resident.source=draw.source;
        m_f.glGenBuffers_(1,&resident.input);m_f.glBindBuffer_(storage,resident.input);
        m_f.glBufferData_(storage,kStaticBytes,draw.source->vertices.data(),GL158_STATIC_DRAW);
        ++staticUploads;staticBytes.fetch_add(kStaticBytes,std::memory_order_relaxed);++sourceReallocations;
        for(auto& pose:resident.poses) {
            const auto allocate=[&](GLuint& buffer,size_t bytes){
                m_f.glGenBuffers_(1,&buffer);m_f.glBindBuffer_(storage,buffer);
                m_f.glBufferData_(storage,bytes,nullptr,GL178_STREAM_DRAW);
            };
            allocate(pose.metadata,kMetadataBytes);allocate(pose.constants,kConstantBytes);
#if defined(PS2X_G775_DIAG)
            if(g789LiveArm()==G789SkinArm::Compute){
                allocate(pose.output,kVertices*sizeof(G178Vtx));
                m_f.glGenVertexArrays_(1,&pose.vao);m_f.glBindVertexArray_(pose.vao);
                m_f.glBindBuffer_(GL158_ARRAY_BUFFER,pose.output);
                m_f.glVertexAttribPointer_(0,3,GL_FLOAT,GL_FALSE,sizeof(G178Vtx),nullptr);
                m_f.glVertexAttribPointer_(1,3,GL_FLOAT,GL_FALSE,sizeof(G178Vtx),reinterpret_cast<void*>(12));
                m_f.glVertexAttribPointer_(2,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(G178Vtx),reinterpret_cast<void*>(24));
                m_f.glVertexAttribPointer_(3,4,GL_UNSIGNED_BYTE,GL_TRUE,sizeof(G178Vtx),reinterpret_cast<void*>(28));
                for(unsigned attribute=0;attribute<4;++attribute)m_f.glEnableVertexAttribArray_(attribute);
            }
#endif
        }
        if(glGetError()!=GL_NO_ERROR)return false;
        found=m_g789LiveSources.emplace(draw.identity.source,std::move(resident)).first;
        draw.source->ready.store(draw.identity.source,std::memory_order_release);
    }
    if(found->second.source!=draw.source)return false;
    G789LiveBuffers& pose=found->second.poses[draw.identity.draw%found->second.poses.size()];
    const auto bind=[&](GLuint buffer,unsigned index){m_e.glBindBufferBase_(storage,index,buffer);};
    bind(found->second.input,0);bind(pose.constants,1);bind(pose.metadata,3);
    if(pose.draw!=draw.identity.draw) {
        const auto upload=[&](GLuint buffer,const void* data,size_t bytes){
            m_f.glBindBuffer_(storage,buffer);m_e.glBufferSubData_(storage,0,bytes,data);
        };
        upload(pose.metadata,draw.metadata.data(),kMetadataBytes);
        upload(pose.constants,draw.constants.data(),kConstantBytes);
        metadataBytes.fetch_add(kMetadataBytes,std::memory_order_relaxed);
        constantBytes.fetch_add(kConstantBytes,std::memory_order_relaxed);
        if(g789LiveArm()==G789SkinArm::Compute) {
            // Order SSBO reuse after its earlier consumers; no CPU wait.
            if(pose.draw)m_e.glMemoryBarrier_(0x2000u);
            bind(pose.output,2);m_f.glUseProgram_(m_g789LiveCompute);
            m_f.glUniform1i_(0,kVertices);m_e.glDispatchCompute_((kVertices+63)/64,1,1);
            // Shader stores -> vertex attribute fetch. No readback/buffer-update barrier.
            m_e.glMemoryBarrier_(0x0001u);
        }
        pose.draw=draw.identity.draw;pose.executed=false;
    }
    m_g789LiveBuffers=&pose;return true;
}
#endif
