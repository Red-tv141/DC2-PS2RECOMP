#if defined(PS2X_G777_NATIVE_GL)
// Bring-up uses the accepted renderer worker/context and raster program. Readbacks here
// are diagnostic parity evidence; they are not a shipping submission strategy.
#include "g777_native_geometry_shader.inc"
struct G777MeshBuffer
{
    std::shared_ptr<const dc2_native::Mesh> owner;
    uint32_t bucket=0,material=0,triangles=0;
    GLuint buffer=0;
};
std::vector<G777MeshBuffer> m_g777Meshes;
std::atomic<bool> m_g782NativeReady{false};
GLuint m_g777Program=0,m_g778CompactProgram=0,m_g779IndirectProgram=0,m_g778Scratch=0,m_g778Sizes=0,m_g777Constants=0,m_g777Output=0,m_g777Count=0,m_g777Metadata=0,m_g779Indirect=0,m_g777Vao=0;
using G779DrawArraysIndirect=void(APIENTRY*)(GLenum,const void*);
G779DrawArraysIndirect m_g779DrawArraysIndirect=nullptr;
using G777GetBufferSubData=void(APIENTRY*)(GLenum,GLsizeiptrLocal,GLsizeiptrLocal,void*);
G777GetBufferSubData m_g777GetBufferSubData=nullptr;
using G778BufferSubData=void(APIENTRY*)(GLenum,GLsizeiptrLocal,GLsizeiptrLocal,const void*);
G778BufferSubData m_g778BufferSubData=nullptr;
size_t m_g778BufferCapacity=0;

void g777Destroy()
{
    m_g782NativeReady.store(false,std::memory_order_release);
    for(auto& mesh:m_g777Meshes)if(mesh.buffer)m_f.glDeleteBuffers_(1,&mesh.buffer);
    m_g777Meshes.clear();
    m_g778BufferCapacity=0;
    for(GLuint* buffer:{&m_g777Constants,&m_g777Output,&m_g777Count,&m_g777Metadata,&m_g778Scratch,&m_g778Sizes,&m_g779Indirect})
        if(*buffer){m_f.glDeleteBuffers_(1,buffer);*buffer=0;}
    if(m_g777Vao){m_f.glDeleteVertexArrays_(1,&m_g777Vao);m_g777Vao=0;}
    for(GLuint* program:{&m_g777Program,&m_g778CompactProgram,&m_g779IndirectProgram})if(*program){m_f.glDeleteProgram_(*program);*program=0;}
}
bool g777Initialize()
{
    if(m_g777Program)return true;
    if(sizeof(G178Vtx)!=32 || !loadProc(m_e.glDispatchCompute_,"glDispatchCompute") ||
       !loadProc(m_e.glBindBufferBase_,"glBindBufferBase") ||
       !loadProc(m_e.glMemoryBarrier_,"glMemoryBarrier") ||
       !loadProc(m_g777GetBufferSubData,"glGetBufferSubData") ||
       !loadProc(m_g779DrawArraysIndirect,"glDrawArraysIndirect") ||
       !loadProc(m_g778BufferSubData,"glBufferSubData"))return false;
    for(unsigned pass=0;pass<3;++pass){
    auto& program=pass==2?m_g779IndirectProgram:(pass?m_g778CompactProgram:m_g777Program);
    const auto source=pass==2?kG779NativeIndirectCS:(pass?kG778NativeCompactCS:kG777NativeGeometryCS);
    const GLuint shader=m_f.glCreateShader_(0x91B9);if(!shader)return false;
    m_f.glShaderSource_(shader,1,&source,nullptr);m_f.glCompileShader_(shader);
    GLint ok=0;m_f.glGetShaderiv_(shader,GL158_COMPILE_STATUS,&ok);
    if(!ok){char log[4096]{};m_f.glGetShaderInfoLog_(shader,sizeof(log),nullptr,log);
        std::fprintf(stderr,"G777 native shader: %s\n",log);m_f.glDeleteShader_(shader);return false;}
    program=m_f.glCreateProgram_();m_f.glAttachShader_(program,shader);
    m_f.glLinkProgram_(program);m_f.glDeleteShader_(shader);
    m_f.glGetProgramiv_(program,GL158_LINK_STATUS,&ok);
    if(!ok){char log[4096]{};m_f.glGetProgramInfoLog_(program,sizeof(log),nullptr,log);
        std::fprintf(stderr,"G777 native link: %s\n",log);g777Destroy();return false;}
    }
    for(GLuint* buffer:{&m_g777Constants,&m_g777Output,&m_g777Count,&m_g777Metadata,&m_g778Scratch,&m_g778Sizes,&m_g779Indirect})m_f.glGenBuffers_(1,buffer);
    m_f.glGenVertexArrays_(1,&m_g777Vao);m_f.glBindVertexArray_(m_g777Vao);
    m_f.glBindBuffer_(GL158_ARRAY_BUFFER,m_g777Output);
    m_f.glVertexAttribPointer_(0,3,GL_FLOAT,GL_FALSE,32,reinterpret_cast<void*>(0));
    m_f.glVertexAttribPointer_(1,3,GL_FLOAT,GL_FALSE,32,reinterpret_cast<void*>(12));
    m_f.glVertexAttribPointer_(2,4,GL_UNSIGNED_BYTE,GL_TRUE,32,reinterpret_cast<void*>(24));
    m_f.glVertexAttribPointer_(3,4,GL_UNSIGNED_BYTE,GL_TRUE,32,reinterpret_cast<void*>(28));
    for(unsigned at=0;at<4;++at)m_f.glEnableVertexAttribArray_(at);
    return true;
}
bool g782PrewarmNative()
{
    if(!g777Initialize())return false;
    // Exact E91F00 source has 534 corners = 178 triangles. Reserve every
    // per-draw scratch/indirect object before G150 may commit a draw.
    constexpr uint32_t triangles=178u,capacity=triangles*30u;
    constexpr GLenum storage=0x90D2;
    const auto reserve=[&](GLuint name,GLsizeiptrLocal bytes){
        if(name==0u)return false;
        m_f.glBindBuffer_(storage,name);
        m_f.glBufferData_(storage,bytes,nullptr,GL178_STREAM_DRAW);
        return glGetError()==GL_NO_ERROR;
    };
    if(!reserve(m_g777Constants,23u*sizeof(dc2_native::Vector)) ||
       !reserve(m_g778Scratch,capacity*sizeof(G178Vtx)) ||
       !reserve(m_g777Count,4u*sizeof(uint32_t)) ||
       !reserve(m_g777Metadata,(capacity/3u)*sizeof(std::array<uint32_t,4>)) ||
       !reserve(m_g778Sizes,triangles*sizeof(std::array<uint32_t,2>)) ||
       !reserve(m_g777Output,capacity*sizeof(G178Vtx)) ||
       !reserve(m_g779Indirect,4u*sizeof(uint32_t)))return false;
    m_g778BufferCapacity=capacity;
    m_g782NativeReady.store(true,std::memory_order_release);
    return true;
}
bool g777Prepare(G178Batch& batch)
{
    using namespace dc2_native;
    auto& probe=*batch.g777Native;probe.error=0;probe.gpuDraws=0;probe.sourceUploadBytes=0;
    if(!probe.draw || !probe.draw->geometry || probe.draw->renderFlags&~3u || batch.draws.size()!=1 || !g777Initialize())
        {probe.error=10;return false;}
    if(probe.live && (batch.draws[0].srcFbp || batch.draws[0].blend>1 || batch.draws[0].screenSprite))
        {probe.error=17;return false;}
    probe.readGeometry=!probe.live || dc2_g778::statsRequested();
    probe.vertexCount=0;probe.cacheReplacements=0;
    const auto& draw=*probe.draw;G777MeshBuffer* cached=nullptr;
    for(auto& mesh:m_g777Meshes)if(mesh.owner->identity==draw.geometry->identity && mesh.bucket==probe.bucket){
        if(mesh.owner.get()!=draw.geometry.get()){
            mesh.owner=draw.geometry;++probe.cacheReplacements;
        }cached=&mesh;break;}
    constexpr GLenum storage=0x90D2;
    if(!cached){
        G777MeshBuffer mesh;mesh.owner=draw.geometry;mesh.bucket=probe.bucket;
        std::vector<Vector> source;bool materialFound=false;
        for(uint32_t faceIndex=0;faceIndex<mesh.owner->faces.size();++faceIndex){
            const auto& face=mesh.owner->faces[faceIndex];if(face.bucket!=probe.bucket)continue;
            if(face.material>=draw.materials.size() || (face.topology!=3 && face.topology!=4) ||
               (face.topology==4 && face.batchCorners && face.indices.size()>face.batchCorners) ||
               (materialFound && mesh.material!=face.material)){probe.error=12;return false;}
            mesh.material=face.material;materialFound=true;
            for(uint32_t corner=2;corner<face.indices.size();++corner){
                if(face.topology==3 && corner%3!=2)continue;
                std::array<uint32_t,3> indices={corner-2,corner-1,corner};
                if(face.topology==4 && (corner&1))std::swap(indices[0],indices[1]);
                for(auto at:indices){if(face.indices[at]>=mesh.owner->vertices.size()){probe.error=13;return false;}
                    const auto& vertex=mesh.owner->vertices[face.indices[at]];
                    source.push_back(vertex.position);source.push_back(vertex.normal);source.push_back(vertex.uv);source.push_back(vertex.colour);}
                source.push_back({float(faceIndex),float(corner),float(face.batchCorners?corner/face.batchCorners:0),float(face.topology)});
                ++mesh.triangles;
            }
        }
        if(!mesh.triangles || mesh.triangles>65536){probe.error=14;return false;}
        m_f.glGenBuffers_(1,&mesh.buffer);m_f.glBindBuffer_(storage,mesh.buffer);
        probe.sourceUploadBytes=source.size()*sizeof(Vector);
        m_f.glBufferData_(storage,static_cast<GLsizeiptrLocal>(probe.sourceUploadBytes),source.data(),0x88E4);
        m_g777Meshes.push_back(std::move(mesh));cached=&m_g777Meshes.back();
    }
    std::array<Vector,23> constants{};
    if(!buildNativeConstants(draw,cached->material,cached->triangles,batch.fbW,batch.fbH,
                             probe.offsetX,probe.offsetY,constants)){probe.error=17;return false;}
    const uint32_t capacity=cached->triangles*30;probe.capacity=capacity;std::array<uint32_t,4> counts={0,0,capacity,0};
#if defined(PS2X_G684_HOT_DIAG)
    static const bool g780Cpu=std::getenv("DC2_G780_GLCPU")!=nullptr;
    const auto g780BufStart=g780Cpu?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
#endif
    const bool grow=capacity>m_g778BufferCapacity;
    const auto buffer=[&](GLuint name,GLsizeiptrLocal bytes,const void* data,unsigned binding){
        m_f.glBindBuffer_(storage,name);
        if(grow)m_f.glBufferData_(storage,bytes,data,GL178_STREAM_DRAW);
        else if(data)m_g778BufferSubData(storage,0,bytes,data);
        m_e.glBindBufferBase_(storage,binding,name);
    };
    m_e.glBindBufferBase_(storage,0,cached->buffer);
    buffer(m_g777Constants,sizeof(constants),constants.data(),1);
    buffer(m_g778Scratch,capacity*sizeof(G178Vtx),nullptr,2);buffer(m_g777Count,sizeof(counts),counts.data(),3);
    buffer(m_g777Metadata,(capacity/3)*sizeof(std::array<uint32_t,4>),nullptr,4);
    buffer(m_g778Sizes,cached->triangles*sizeof(std::array<uint32_t,2>),nullptr,5);
    buffer(m_g777Output,capacity*sizeof(G178Vtx),nullptr,6);
    buffer(m_g779Indirect,4*sizeof(uint32_t),nullptr,7);
    if(grow)m_g778BufferCapacity=capacity;
#if defined(PS2X_G684_HOT_DIAG)
    const auto g780DispatchStart=g780Cpu?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
#endif
    g780GpuStamp(0);
    m_f.glUseProgram_(m_g777Program);m_e.glDispatchCompute_((cached->triangles+63)/64,1,1);m_e.glMemoryBarrier_(0x2000);
    m_f.glUseProgram_(m_g778CompactProgram);m_e.glDispatchCompute_((cached->triangles+63)/64,1,1);m_e.glMemoryBarrier_(0x2000|1);
    g780GpuStamp(1);
    m_f.glUseProgram_(m_g779IndirectProgram);m_e.glDispatchCompute_(1,1,1);
    m_e.glMemoryBarrier_(0x2000|0x40|1); // SSBO, indirect command, vertex attributes; no CPU wait
    g780GpuStamp(2);
#if defined(PS2X_G684_HOT_DIAG)
    if(g780Cpu){
        static uint64_t n=0,bufferNs=0,dispatchNs=0,grows=0;
        const auto end=std::chrono::steady_clock::now();
        ++n;grows+=grow;bufferNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(g780DispatchStart-g780BufStart).count();
        dispatchNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(end-g780DispatchStart).count();
        if((n%128u)==0u)std::fprintf(stderr,"[G780:glcpu] n=%llu grows=%llu avgMs(buffer=%.3f dispatch=%.3f)\n",
            static_cast<unsigned long long>(n),static_cast<unsigned long long>(grows),
            double(bufferNs)/1e6/n,double(dispatchNs)/1e6/n);
    }
#endif
    batch.verts.clear();probe.metadata.clear();
    if(probe.readGeometry){
        m_f.glBindBuffer_(storage,m_g777Count);m_g777GetBufferSubData(storage,0,sizeof(counts),counts.data());
        if(counts[1] || counts[0]>capacity || counts[0]%3){probe.error=counts[1]?counts[1]:15;return false;}
        probe.vertexCount=counts[0];
        batch.verts.resize(counts[0]);probe.metadata.resize(counts[0]/3);
        m_f.glBindBuffer_(storage,m_g777Output);if(counts[0])m_g777GetBufferSubData(storage,0,batch.verts.size()*sizeof(G178Vtx),batch.verts.data());
        m_f.glBindBuffer_(storage,m_g777Metadata);if(counts[0])m_g777GetBufferSubData(storage,0,probe.metadata.size()*sizeof(probe.metadata[0]),probe.metadata.data());
    }
    batch.draws[0].firstVtx=0;batch.draws[0].vtxCount=probe.readGeometry?int(counts[0]):int(capacity);
    if(probe.readGeometry && !counts[0])batch.draws.clear();
    if(glGetError()!=GL_NO_ERROR){probe.error=16;return false;}
    return true;
}
#endif
