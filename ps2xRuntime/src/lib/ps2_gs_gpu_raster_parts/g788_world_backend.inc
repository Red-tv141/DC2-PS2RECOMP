// ===== G788 — WHOLE-WORLD NATIVE FixMDT, GL backend (plans/phase-G788-ledger.md) =====
// Class-body include (private section, after g778_gpu_shadow.inc). Runs on the GL worker.
//
// Static source: every mesh's per-triangle source (13 vec4, G788MeshGpu) is uploaded ONCE into one
// growable arena SSBO, keyed by the mesh content identity. Warm frames upload only the per-batch
// job table + constants (23 vec4 per face).
// Per batch with native faces: ONE compute dispatch over every source triangle of every face (the
// G777 mathematics: transform, directional lighting, fog, six-plane homogeneous clipping), writing
// G178Vtx into a FIXED SLOT LAYOUT that is the VU emission order by construction: per face, per
// source batch of n triangles, [n x 21 vertices: each clipped triangle's fan] then [n x 3 vertices:
// each unchanged triangle]; every unused slot is a zero-area triangle. The draw loop then draws each
// native face's contiguous output range in its placeholder's place: one GL draw per face, ordered
// with the legacy draws of the same batch, inside the batch's single worker round trip.
inline static const char *kG788WorldCS = R"GLSL(
#version 430 core
layout(local_size_x=64) in;
layout(std430,binding=0) readonly buffer Source {vec4 source[];};
layout(std430,binding=1) readonly buffer Constants {vec4 constants[];};
layout(std430,binding=2) readonly buffer Jobs {uvec4 jobs[];};
layout(std430,binding=3) writeonly buffer Vertices {uint vertices[];};
layout(std430,binding=4) buffer Stat {uint failures;uint pad0;uint pad1;uint pad2;};
struct Vertex {vec4 screen;vec4 clip;vec4 colour;vec4 uv;};
int C;
precise vec4 matrixPoint(int base,vec4 point){
    precise vec4 outValue=constants[C+base]*point.x;
    outValue=outValue+constants[C+base+1]*point.y;
    outValue=outValue+constants[C+base+2]*point.z;
    outValue=outValue+constants[C+base+3]*point.w;
    return outValue;
}
float divide32(float numerator,float denominator){return float(double(numerator)/double(denominator));}
Vertex transformVertex(uint at){
    Vertex outValue;vec4 position=source[at],normal=source[at+1u];
    outValue.screen=matrixPoint(0,position);outValue.clip=matrixPoint(4,position);
    precise vec4 light=constants[C+12]*normal.x;
    light=light+constants[C+13]*normal.y;light=light+constants[C+14]*normal.z;
    light=max(light,vec4(0));
    precise vec4 colour=constants[C+19];
    colour=colour+constants[C+15]*light.x;colour=colour+constants[C+16]*light.y;
    colour=colour+constants[C+17]*light.z;colour=colour+constants[C+18]*light.w;
    colour=min(colour,vec4(255));colour=colour*source[at+3u];
    outValue.colour=min(colour,vec4(255));outValue.uv=source[at+2u];return outValue;
}
Vertex interpolateVertex(Vertex a,Vertex b,float fraction){
    Vertex v;
    precise vec4 delta=b.clip-a.clip;precise vec4 value=delta*fraction;v.clip=a.clip+value;
    delta=b.screen-a.screen;value=delta*fraction;v.screen=a.screen+value;
    delta=b.colour-a.colour;value=delta*fraction;v.colour=a.colour+value;
    delta=b.uv-a.uv;value=delta*fraction;v.uv=a.uv+value;return v;
}
bool outside(vec4 p){return any(greaterThan(abs(p.xyz),vec3(abs(p.w))));}
bool project(Vertex v,bool clipped,out uvec3 position,out float q){
    precise vec4 screen=clipped?matrixPoint(8,v.clip):v.screen;
    position=uvec3(0);q=0.0;
    if(isnan(screen.w)||isinf(screen.w)||screen.w==0.0)return false;
    q=divide32(1.0,screen.w);precise vec4 projected=screen*q;
    if(any(isnan(projected.xyz)) || any(isinf(projected.xyz)) || abs(projected.x)>1e7 || abs(projected.y)>1e7 || abs(projected.z)>1e9)return false;
    position=uvec3(uint(int(projected.x*16.0))&65535u,uint(int(projected.y*16.0))&65535u,uint(int(projected.z))&0xffffffu);
    return true;
}
// One G178Vtx (8 words), exactly as g178PushVertex builds it from the GSVertex the legacy kick makes.
void storeVertex(Vertex v,vec4 colourSrc,uvec3 position,float q,uint index,uint flags,float ofx,float ofy){
    uint at=index*8u;
    vertices[at]=floatBitsToUint(float(position.x)/16.0-ofx);
    vertices[at+1u]=floatBitsToUint(float(position.y)/16.0-ofy);
    vertices[at+2u]=floatBitsToUint(float(position.z));
    if((flags&1u)!=0u){
        precise float s=v.uv.x*q;precise float t=v.uv.y*q;
        float gq=(q==0.0)?1.0:q;              // RGBAQ: Q == 0 -> 1
        float iq=(abs(gq)>1.0e-8)?gq:1.0;     // fabsQ
        vertices[at+3u]=floatBitsToUint(s);vertices[at+4u]=floatBitsToUint(t);vertices[at+5u]=floatBitsToUint(iq);
    }else{
        vertices[at+3u]=floatBitsToUint(0.0);vertices[at+4u]=floatBitsToUint(0.0);vertices[at+5u]=floatBitsToUint(1.0);
    }
    uvec4 colour=uvec4(ivec4(colourSrc))&uvec4(255);
    vertices[at+6u]=colour.x|(colour.y<<8u)|(colour.z<<16u)|(colour.w<<24u);
    precise float fog=constants[C+20].w*q;fog=constants[C+20].x+fog;
    fog=max(constants[C+20].z,min(constants[C+20].y,fog));
    vertices[at+7u]=uint(int(fog))&255u;
}
void zeroVertex(uint index){uint at=index*8u;for(uint k=0u;k<8u;++k)vertices[at+k]=0u;}
void main(){
    uint t=gl_GlobalInvocationID.x;
    uint jobCount=jobs[0].x,total=jobs[0].y;
    if(t>=total)return;
    uint lo=0u,hi=jobCount-1u;
    while(lo<hi){uint mid=(lo+hi+1u)/2u;if(jobs[1u+2u*mid].x<=t)lo=mid;else hi=mid-1u;}
    uvec4 j0=jobs[1u+2u*lo],j1=jobs[2u+2u*lo];
    uint local=t-j0.x;uint src=(j0.z+local)*13u;
    C=int(j1.w);uint flags=j1.x;float ofx=float(j1.y),ofy=float(j1.z);
    uvec4 info=uvec4(source[src+12u]);
    uint batchFirst=info.y,batchCount=info.z;
    uint baseOut=j0.w+24u*batchFirst;
    uint slotA=baseOut+21u*(local-batchFirst);
    uint slotB=baseOut+21u*batchCount+3u*(local-batchFirst);
    Vertex polygon[12];Vertex outputPolygon[12];
    for(uint i=0u;i<3u;++i)polygon[i]=transformVertex(src+i*4u);
    bool clipping=(uint(constants[C+22].y)&2u)!=0u;
    bool clipped=clipping && (outside(polygon[0].clip)||outside(polygon[1].clip)||outside(polygon[2].clip));
    uint emitted=0u;
    if(!clipped){
        uvec3 pa,pb,pc;float qa,qb,qc;
        bool ok=project(polygon[0],false,pa,qa)&&project(polygon[1],false,pb,qb)&&project(polygon[2],false,pc,qc);
        if(ok){
            vec4 flatC=polygon[2].colour;bool iip=(flags&2u)!=0u;
            storeVertex(polygon[0],iip?polygon[0].colour:flatC,pa,qa,slotB,flags,ofx,ofy);
            storeVertex(polygon[1],iip?polygon[1].colour:flatC,pb,qb,slotB+1u,flags,ofx,ofy);
            storeVertex(polygon[2],flatC,pc,qc,slotB+2u,flags,ofx,ofy);
            emitted=3u;
        }else{atomicAdd(failures,1u);for(uint k=0u;k<3u;++k)zeroVertex(slotB+k);}
        for(uint k=0u;k<21u;++k)zeroVertex(slotA+k);
    }else{
        for(uint k=0u;k<3u;++k)zeroVertex(slotB+k);
        int polygonCount=3;bool fail=false;
        for(int plane=0;plane<6 && polygonCount>0 && !fail;++plane){
            int axis=plane<2?2:(plane<4?0:1);float sign=(plane&1)==0?1.0:-1.0;int outputCount=0;
            for(int edge=0;edge<polygonCount;++edge){
                Vertex previous=polygon[edge],current=polygon[(edge+1)%polygonCount];
                precise float d0=abs(previous.clip.w)+sign*previous.clip[axis];
                precise float d1=abs(current.clip.w)+sign*current.clip[axis];
                bool before=d0>=0.0,after=d1>=0.0;
                if(before){if(outputCount>=12){fail=true;break;}outputPolygon[outputCount++]=previous;}
                if(before!=after){
                    d0=previous.clip.w+sign*previous.clip[axis];d1=current.clip.w+sign*current.clip[axis];
                    precise float difference=d1-d0;float fraction=abs(divide32(d0,difference));
                    if(isnan(fraction)||isinf(fraction)||outputCount>=12){fail=true;break;}
                    outputPolygon[outputCount++]=interpolateVertex(previous,current,fraction);
                }
            }
            polygonCount=outputCount;for(int i=0;i<polygonCount;++i)polygon[i]=outputPolygon[i];
        }
        if(fail){atomicAdd(failures,1u);polygonCount=0;}
        bool iip=(flags&2u)!=0u;
        for(int corner=2;corner<polygonCount;++corner){
            Vertex a=polygon[0],b=polygon[corner-1],c=polygon[corner];uvec3 pa,pb,pc;float qa,qb,qc;
            if(!(project(a,true,pa,qa)&&project(b,true,pb,qb)&&project(c,true,pc,qc))){atomicAdd(failures,1u);continue;}
            double area=double(int(pb.x)-int(pa.x))*double(int(pc.y)-int(pa.y))-double(int(pb.y)-int(pa.y))*double(int(pc.x)-int(pa.x));
            if(area==0.0)continue;
            if(emitted+3u>21u){atomicAdd(failures,1u);break;}
            vec4 flatC=c.colour;
            storeVertex(a,iip?a.colour:flatC,pa,qa,slotA+emitted,flags,ofx,ofy);
            storeVertex(b,iip?b.colour:flatC,pb,qb,slotA+emitted+1u,flags,ofx,ofy);
            storeVertex(c,flatC,pc,qc,slotA+emitted+2u,flags,ofx,ofy);
            emitted+=3u;
        }
        for(uint k=emitted;k<21u;++k)zeroVertex(slotA+k);
    }
}
)GLSL";

struct G788OutSet { GLuint out = 0, vao = 0; size_t capVerts = 0; };
GLuint m_g788Prog = 0, m_g788Arena = 0, m_g788JobBuf = 0, m_g788ConstBuf = 0, m_g788Stat = 0;
size_t m_g788ArenaCapTris = 0, m_g788ArenaUsedTris = 0, m_g788JobCap = 0, m_g788ConstCap = 0;
std::unordered_map<uint64_t, uint32_t> m_g788MeshBase; // mesh identity -> first triangle in the arena
std::array<G788OutSet, 4> m_g788Out{};
uint32_t m_g788OutNext = 0, m_g788OutCur = 0;
bool m_g788Failed = false;
using G788BufferSubData = void(APIENTRY *)(GLenum, GLsizeiptrLocal, GLsizeiptrLocal, const void *);
using G788CopyBufferSubData = void(APIENTRY *)(GLenum, GLenum, GLsizeiptrLocal, GLsizeiptrLocal, GLsizeiptrLocal);
G788BufferSubData m_g788BufferSubData = nullptr;
G788CopyBufferSubData m_g788CopyBufferSubData = nullptr;
std::vector<uint32_t> m_g788JobScratch;
std::vector<dc2_native::Vector> m_g788ConstScratch;

void g788Destroy()
{
    for (GLuint *b : {&m_g788Arena, &m_g788JobBuf, &m_g788ConstBuf, &m_g788Stat})
        if (*b) { m_f.glDeleteBuffers_(1, b); *b = 0; }
    for (auto &s : m_g788Out)
    {
        if (s.out) { m_f.glDeleteBuffers_(1, &s.out); s.out = 0; }
        if (s.vao) { m_f.glDeleteVertexArrays_(1, &s.vao); s.vao = 0; }
        s.capVerts = 0;
    }
    if (m_g788Prog) { m_f.glDeleteProgram_(m_g788Prog); m_g788Prog = 0; }
    m_g788MeshBase.clear();
    m_g788ArenaCapTris = m_g788ArenaUsedTris = m_g788JobCap = m_g788ConstCap = 0;
}
// Nonblocking timestamp pairs. Timeline spans include GPU scheduling/CPU submission gaps;
// they are diagnostic spans, not a claim of frame-critical recoverable GPU work.
void g788GpuCost(unsigned kind, bool begin)
{
    if (!dc2_g788_econ::on() || kind > 1) return;
    using Gen = void(APIENTRY*)(GLsizei,GLuint*);
    using Counter = void(APIENTRY*)(GLuint,GLenum);
    using Get64 = void(APIENTRY*)(GLuint,GLenum,uint64_t*);
    using Get32 = void(APIENTRY*)(GLuint,GLenum,GLuint*);
    constexpr unsigned slots=256;
    struct State {
        Gen gen=nullptr; Counter counter=nullptr; Get64 get64=nullptr; Get32 get32=nullptr;
        GLuint q[2][slots][2]{}; uint8_t used[2][slots]{};
        unsigned next[2]{}, active[2]{slots,slots}; bool tried=false,ok=false;
    };
    static State s;
    if (!s.tried) {
        s.tried=true;
        s.ok=loadProc(s.gen,"glGenQueries") && loadProc(s.counter,"glQueryCounter") &&
            loadProc(s.get64,"glGetQueryObjectui64v") && loadProc(s.get32,"glGetQueryObjectuiv");
        if(s.ok)s.gen(2*slots*2,&s.q[0][0][0]);
        std::fprintf(stderr,"[G788:gpu-cost-init] available=%d\n",int(s.ok));
    }
    if(!s.ok)return;
    if(begin) {
        const unsigned slot=s.next[kind];
        if(s.used[kind][slot]==2) {
            GLuint ready=0; s.get32(s.q[kind][slot][1],0x8867,&ready);
            if(!ready) {
                s.active[kind]=slots;
                dc2_g788_econ::gpuDropped[kind].fetch_add(1,std::memory_order_relaxed);
                return;
            }
            uint64_t t[2]{};
            for(unsigned i=0;i<2;++i)s.get64(s.q[kind][slot][i],0x8866,&t[i]);
            if(t[1]>=t[0]) {
                dc2_g788_econ::gpuNs[kind].fetch_add(t[1]-t[0],std::memory_order_relaxed);
                dc2_g788_econ::gpuSamples[kind].fetch_add(1,std::memory_order_relaxed);
            }
        }
        s.active[kind]=slot; s.next[kind]=(slot+1)%slots; s.used[kind][slot]=1;
        s.counter(s.q[kind][slot][0],0x8E28);
    } else if(s.active[kind]!=slots) {
        const unsigned slot=s.active[kind];
        s.counter(s.q[kind][slot][1],0x8E28); s.used[kind][slot]=2; s.active[kind]=slots;
    }
}
void g788WorkerCpuNote()
{
#if defined(_WIN32)
    if (!dc2_g788_econ::on()) return;
    FILETIME create{},exit{},kernel{},user{};
    if(!GetThreadTimes(GetCurrentThread(),&create,&exit,&kernel,&user))return;
    const uint64_t now=(uint64_t(kernel.dwHighDateTime)<<32|kernel.dwLowDateTime)+
        (uint64_t(user.dwHighDateTime)<<32|user.dwLowDateTime);
    static const uint64_t origin=now;
    dc2_g788_econ::glCpuNs.store((now-origin)*100,std::memory_order_relaxed);
#endif
}
bool g788Init()
{
    if (m_g788Prog)
        return true;
    if (m_g788Failed)
        return false;
#if defined(PS2X_G775_DIAG)
    dc2_g788_econ::Scope cold(dc2_g788_econ::ColdGpuInit);
#endif
    m_g788Failed = true;
    if (!loadProc(m_e.glDispatchCompute_, "glDispatchCompute") || !loadProc(m_e.glBindBufferBase_, "glBindBufferBase") ||
        !loadProc(m_e.glMemoryBarrier_, "glMemoryBarrier") || !loadProc(m_g788BufferSubData, "glBufferSubData") ||
        !loadProc(m_g788CopyBufferSubData, "glCopyBufferSubData"))
        return false;
    const GLuint shader = m_f.glCreateShader_(0x91B9);
    if (!shader)
        return false;
    m_f.glShaderSource_(shader, 1, &kG788WorldCS, nullptr);
    dc2_g788_econ::event(dc2_g788_econ::ShaderCompile);
    m_f.glCompileShader_(shader);
    GLint ok = 0;
    m_f.glGetShaderiv_(shader, GL158_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[4096]{};
        m_f.glGetShaderInfoLog_(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[G788:gl] shader: %s\n", log);
        m_f.glDeleteShader_(shader);
        return false;
    }
    m_g788Prog = m_f.glCreateProgram_();
    m_f.glAttachShader_(m_g788Prog, shader);
    m_f.glLinkProgram_(m_g788Prog);
    m_f.glDeleteShader_(shader);
    m_f.glGetProgramiv_(m_g788Prog, GL158_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[4096]{};
        m_f.glGetProgramInfoLog_(m_g788Prog, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[G788:gl] link: %s\n", log);
        g788Destroy();
        return false;
    }
    for (GLuint *b : {&m_g788Arena, &m_g788JobBuf, &m_g788ConstBuf, &m_g788Stat})
        m_f.glGenBuffers_(1, b);
    for (auto &s : m_g788Out)
    {
        m_f.glGenBuffers_(1, &s.out);
        m_f.glGenVertexArrays_(1, &s.vao);
        m_f.glBindVertexArray_(s.vao);
        m_f.glBindBuffer_(GL158_ARRAY_BUFFER, s.out);
        m_f.glVertexAttribPointer_(0, 3, GL_FLOAT, GL_FALSE, sizeof(G178Vtx), reinterpret_cast<const void *>(0));
        m_f.glVertexAttribPointer_(1, 3, GL_FLOAT, GL_FALSE, sizeof(G178Vtx), reinterpret_cast<const void *>(12));
        m_f.glVertexAttribPointer_(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(G178Vtx), reinterpret_cast<const void *>(24));
        m_f.glVertexAttribPointer_(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(G178Vtx), reinterpret_cast<const void *>(28));
        for (unsigned a = 0; a < 4; ++a)
            m_f.glEnableVertexAttribArray_(a);
        s.capVerts = 0;
    }
    constexpr GLenum storage = 0x90D2;
    const uint32_t zero[4] = {0, 0, 0, 0};
    m_f.glBindBuffer_(storage, m_g788Stat);
    m_f.glBufferData_(storage, sizeof(zero), zero, 0x88E8); // GL_DYNAMIC_READ
    m_f.glBindVertexArray_(m_vao);
    m_f.glBindBuffer_(GL158_ARRAY_BUFFER, m_vbo);
    m_g788Failed = glGetError() != GL_NO_ERROR;
    if (m_g788Failed)
    {
        g788Destroy();
        return false;
    }
    std::fprintf(stderr, "[G788:gl] native world compute ready\n");
    return true;
}
// Make the mesh resident (once per content identity). Grows the arena by doubling (GPU copy).
bool g788EnsureMesh(const G788MeshGpu &m, uint32_t &base)
{
    const auto it = m_g788MeshBase.find(m.identity);
    if (it != m_g788MeshBase.end())
    {
        base = it->second;
        return true;
    }
    constexpr GLenum storage = 0x90D2;
    constexpr size_t triBytes = 13u * sizeof(dc2_native::Vector);
#if defined(PS2X_G775_DIAG)
    dc2_g788_econ::Scope cold(dc2_g788_econ::ColdMeshUpload);
#endif
    if (m.source.size() != size_t(m.triangles) * 13u)
        return false;
    if (m_g788ArenaUsedTris + m.triangles > m_g788ArenaCapTris)
    {
        size_t cap = std::max<size_t>(m_g788ArenaCapTris * 2u, 1u << 17);
        while (cap < m_g788ArenaUsedTris + m.triangles)
            cap *= 2u;
        GLuint grown = 0;
        m_f.glGenBuffers_(1, &grown);
        m_f.glBindBuffer_(storage, grown);
        m_f.glBufferData_(storage, static_cast<GLsizeiptrLocal>(cap * triBytes), nullptr, 0x88E4); // GL_STATIC_DRAW
        if (m_g788ArenaUsedTris)
        {
            m_f.glBindBuffer_(0x8F36, m_g788Arena); // GL_COPY_READ_BUFFER
            m_f.glBindBuffer_(0x8F37, grown);       // GL_COPY_WRITE_BUFFER
            m_g788CopyBufferSubData(0x8F36, 0x8F37, 0, 0, static_cast<GLsizeiptrLocal>(m_g788ArenaUsedTris * triBytes));
        }
        m_f.glDeleteBuffers_(1, &m_g788Arena);
        m_g788Arena = grown;
        m_g788ArenaCapTris = cap;
        g_g788C.gArenaGrow.fetch_add(1u, std::memory_order_relaxed);
    }
    m_f.glBindBuffer_(storage, m_g788Arena);
    m_g788BufferSubData(storage, static_cast<GLsizeiptrLocal>(m_g788ArenaUsedTris * triBytes),
                        static_cast<GLsizeiptrLocal>(size_t(m.triangles) * triBytes), m.source.data());
    base = static_cast<uint32_t>(m_g788ArenaUsedTris);
    m_g788ArenaUsedTris += m.triangles;
    m_g788MeshBase.emplace(m.identity, base);
    g_g788C.gMeshUploads.fetch_add(1u, std::memory_order_relaxed);
    g_g788C.gUploadBytes.fetch_add(size_t(m.triangles) * triBytes, std::memory_order_relaxed);
    return glGetError() == GL_NO_ERROR;
}
// Before the batch's draw loop: one dispatch for every native face of the batch.
bool g788Prepare(G178Batch &b)
{
    dc2_g788_econ::Scope econ(dc2_g788_econ::PrepareGpu);
    if (b.g788Jobs.empty())
        return true;
    if (!g788Init())
    {
        g_g788C.gFail.fetch_add(1u, std::memory_order_relaxed);
        return false;
    }
    const size_t n = b.g788Jobs.size();
    uint32_t tri = 0, outVerts = 0;
    {
    dc2_g788_econ::Scope pack(dc2_g788_econ::PackJobs);
    m_g788JobScratch.assign(4u + 8u * n, 0u);
    m_g788ConstScratch.assign(23u * n, dc2_native::Vector{});
    for (size_t j = 0; j < n; ++j)
    {
        const G788BatchJob &job = b.g788Jobs[j];
        const G788MeshGpu &mesh = *job.rec->mesh;
        uint32_t base = 0;
        if (!g788EnsureMesh(mesh, base))
        {
            g_g788C.gFail.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        const auto &face = job.rec->draw.geometry->faces[job.face];
        uint32_t *w = &m_g788JobScratch[4u + 8u * j];
        w[0] = tri;
        w[1] = job.triCount;
        w[2] = base + mesh.faceTri[job.face];
        w[3] = job.outBase;
        w[4] = job.flags;
        w[5] = job.ofx;
        w[6] = job.ofy;
        w[7] = static_cast<uint32_t>(23u * j);
        dc2_native::Vector *c = &m_g788ConstScratch[23u * j];
        const auto &mc = job.rec->constants[face.material];
        for (unsigned k = 0; k < 21; ++k)
            c[k] = mc[k];
        c[21] = {0.0f, 0.0f, float(job.ofx), float(job.ofy)};
        c[22] = {float(job.triCount), float(job.rec->draw.renderFlags), 0.0f, 0.0f};
        tri += job.triCount;
        outVerts = std::max(outVerts, job.outBase + 24u * job.triCount);
    }
    m_g788JobScratch[0] = static_cast<uint32_t>(n);
    m_g788JobScratch[1] = tri;
    }
    if (!tri)
        return true;
    constexpr GLenum storage = 0x90D2;
    m_g788OutCur = m_g788OutNext++ % uint32_t(m_g788Out.size());
    G788OutSet &set = m_g788Out[m_g788OutCur];
    const size_t needVerts = std::max<size_t>(outVerts, 3u);
    if (needVerts > set.capVerts)
    {
        size_t cap = std::max<size_t>(set.capVerts * 2u, 1u << 16);
        while (cap < needVerts)
            cap *= 2u;
        m_f.glBindBuffer_(storage, set.out);
        m_f.glBufferData_(storage, static_cast<GLsizeiptrLocal>(cap * sizeof(G178Vtx)), nullptr, GL178_STREAM_DRAW);
        set.capVerts = cap;
    }
    auto upload = [&](GLuint name, size_t &cap, size_t bytes, const void *data) {
        m_f.glBindBuffer_(storage, name);
        if (bytes > cap)
        {
            cap = std::max<size_t>(bytes, cap * 2u);
            m_f.glBufferData_(storage, static_cast<GLsizeiptrLocal>(cap), nullptr, GL178_STREAM_DRAW);
        }
        m_g788BufferSubData(storage, 0, static_cast<GLsizeiptrLocal>(bytes), data);
    };
    {
        dc2_g788_econ::Scope uploadJobs(dc2_g788_econ::UploadJobs);
        upload(m_g788JobBuf, m_g788JobCap, m_g788JobScratch.size() * sizeof(uint32_t), m_g788JobScratch.data());
    }
    {
        dc2_g788_econ::Scope uploadConstants(dc2_g788_econ::UploadConstants);
        upload(m_g788ConstBuf, m_g788ConstCap, m_g788ConstScratch.size() * sizeof(dc2_native::Vector), m_g788ConstScratch.data());
    }
    m_e.glBindBufferBase_(storage, 0, m_g788Arena);
    m_e.glBindBufferBase_(storage, 1, m_g788ConstBuf);
    m_e.glBindBufferBase_(storage, 2, m_g788JobBuf);
    m_e.glBindBufferBase_(storage, 3, set.out);
    m_e.glBindBufferBase_(storage, 4, m_g788Stat);
    m_f.glUseProgram_(m_g788Prog);
    {
    dc2_g788_econ::Scope dispatch(dc2_g788_econ::DispatchCompute);
    g788GpuCost(0,true);
    m_e.glDispatchCompute_((tri + 63u) / 64u, 1, 1);
    m_e.glMemoryBarrier_(0x1u | 0x2000u); // VERTEX_ATTRIB_ARRAY | SHADER_STORAGE
    g788GpuCost(0,false);
    }
    g_g788C.gDispatch.fetch_add(1u, std::memory_order_relaxed);
    g_g788C.gJobs.fetch_add(n, std::memory_order_relaxed);
    g_g788C.gTris.fetch_add(tri, std::memory_order_relaxed);
    if(dc2_g788_econ::on()) {
        uint64_t outBytes=0;for(const auto& o:m_g788Out)outBytes+=o.capVerts*sizeof(G178Vtx);
        dc2_g788_econ::gpuStaticCapacity.store(m_g788ArenaCapTris*13u*sizeof(dc2_native::Vector));
        dc2_g788_econ::gpuOutputCapacity.store(outBytes);
        dc2_g788_econ::gpuDescriptorCapacity.store(m_g788JobCap+m_g788ConstCap+16u);
        dc2_g788_econ::cpuJobCapacity.store(m_g788JobScratch.capacity()*sizeof(uint32_t)+m_g788ConstScratch.capacity()*sizeof(dc2_native::Vector));
    }
    static const uint32_t gverify = [] { const char *e = std::getenv("DC2_G788_GVERIFY"); return e ? uint32_t(std::strtoul(e, nullptr, 0)) : 0u; }();
    if (gverify && (g_g788C.gDispatch.load(std::memory_order_relaxed) % gverify) == 0u)
        g788GpuOracle(b, set, outVerts);
    // Economics arm: keep cold allocation/program checks; vary only the final
    // warm error-query cadence. Default 1 reproduces P. No geometry/state changes.
    static const uint32_t checkEvery = [] {
        const char *e=std::getenv("DC2_G788_CHECK_EVERY");
        return e ? uint32_t(std::strtoul(e,nullptr,0)) : 1u;
    }();
    const uint64_t dispatchNo=g_g788C.gDispatch.load(std::memory_order_relaxed);
    if(gverify || dispatchNo==1u || (checkEvery && dispatchNo%checkEvery==0u)) {
        dc2_g788_econ::event(dc2_g788_econ::ErrorQueries);
        dc2_g788_econ::Scope check(dc2_g788_econ::CheckGpu);
        return glGetError() == GL_NO_ERROR;
    }
    dc2_g788_econ::event(dc2_g788_econ::ErrorQueriesSkipped);
    return true;
}
// DIAGNOSTIC (DC2_G788_GVERIFY=N, every N-th dispatch): read the compute output back (a GL sync) and
// compare every non-degenerate output triangle of every face, in order, against the per-face CPU
// reference converted exactly as g178PushVertex converts the GSVertex a legacy kick makes.
using G788GetBufferSubData = void(APIENTRY *)(GLenum, GLsizeiptrLocal, GLsizeiptrLocal, void *);
G788GetBufferSubData m_g788GetBufferSubData = nullptr;
void g788GpuOracle(const G178Batch &b, const G788OutSet &set, uint32_t outVerts)
{
    constexpr GLenum storage = 0x90D2;
    if (!m_g788GetBufferSubData && !loadProc(m_g788GetBufferSubData, "glGetBufferSubData"))
        return;
    std::vector<G178Vtx> out(outVerts);
    m_f.glBindBuffer_(storage, set.out);
    m_g788GetBufferSubData(storage, 0, static_cast<GLsizeiptrLocal>(out.size() * sizeof(G178Vtx)), out.data());
    uint32_t stat[4]{};
    m_f.glBindBuffer_(storage, m_g788Stat);
    m_g788GetBufferSubData(storage, 0, sizeof(stat), stat);
    if (stat[0])
    {
        g_g788C.gvShaderFail.fetch_add(stat[0], std::memory_order_relaxed);
        const uint32_t zero[4] = {0, 0, 0, 0};
        m_g788BufferSubData(storage, 0, sizeof(zero), zero);
    }
    g_g788C.gvBatches.fetch_add(1u, std::memory_order_relaxed);
    std::vector<dc2_native::ReferencePrimitive> prims;
    static uint32_t logs = 0;
    for (const G788BatchJob &job : b.g788Jobs)
    {
        prims.clear();
        if (!g788FacePrims(*job.rec, job.face, prims, false)) {
            g_g788C.gvRefRefused.fetch_add(1u,std::memory_order_relaxed);
            static unsigned refusedLogs=0;
            if(refusedLogs++<24u) {
                const auto& draw=job.rec->draw;
                const auto matrix=dc2_native::multiply(draw.cameraScreen,draw.world);
                const auto& face=draw.geometry->faces[job.face];
                for(unsigned corner=0;corner<face.indices.size();++corner) {
                    const auto& p=draw.geometry->vertices[face.indices[corner]].position;
                    const auto screen=dc2_native::transform(matrix,p);
                    const float q=screen[3]!=0?1.0f/screen[3]:0.0f;
                    const auto projected=dc2_native::product(screen,dc2_native::splat(q));
                    if(screen[3]!=0 && std::isfinite(q) && std::isfinite(projected[0]) &&
                       std::isfinite(projected[1]) && std::isfinite(projected[2]) &&
                       std::fabs(projected[0])<=1e7f && std::fabs(projected[1])<=1e7f && std::fabs(projected[2])<=1e9f)continue;
                    std::fprintf(stderr,"[G788:projection-refuse] frame=%u mesh=%llx face=%u material=%u corner=%u wRow=%.9g,%.9g,%.9g,%.9g source=%.9g,%.9g,%.9g,%.9g screen=%.9g,%.9g,%.9g,%.9g projected=%.9g,%.9g,%.9g q=%.9g\n",
                        job.rec->frame,(unsigned long long)job.rec->mesh->identity,job.face,face.material,corner,
                        matrix[0][3],matrix[1][3],matrix[2][3],matrix[3][3],p[0],p[1],p[2],p[3],
                        screen[0],screen[1],screen[2],screen[3],projected[0],projected[1],projected[2],q);
                    break;
                }
            }
            continue;
        }
        g_g788C.gvFaces.fetch_add(1u, std::memory_order_relaxed);
        const bool tme = (job.flags & 1u) != 0u, iip = (job.flags & 2u) != 0u;
        size_t cursor = 0;
        for (uint32_t v = job.outBase; v + 3u <= job.outBase + 24u * job.triCount && v + 3u <= out.size(); v += 3u)
        {
            const G178Vtx &a = out[v], &bb = out[v + 1u], &c = out[v + 2u];
            const double area = (double(bb.x) - a.x) * (double(c.y) - a.y) - (double(bb.y) - a.y) * (double(c.x) - a.x);
            if (area == 0.0)
                continue;
            if (cursor >= prims.size())
            {
                g_g788C.gvExtra.fetch_add(1u, std::memory_order_relaxed);
                continue;
            }
            const auto &p = prims[cursor++];
            bool same = true;
            for (unsigned k = 0; k < 3 && same; ++k)
            {
                const auto &r = p.vertices[k];
                G178Vtx e{};
                e.x = float(r.x & 0xFFFFu) / 16.0f - float(job.ofx);
                e.y = float(r.y & 0xFFFFu) / 16.0f - float(job.ofy);
                e.z = float(r.z & 0xFFFFFFu);
                if (tme)
                {
                    const float gq = r.q == 0.0f ? 1.0f : r.q;
                    e.sq = r.s;
                    e.tq = r.t;
                    e.iq = std::fabs(gq) > 1.0e-8f ? gq : 1.0f;
                }
                else
                {
                    e.sq = 0.0f;
                    e.tq = 0.0f;
                    e.iq = 1.0f;
                }
                const auto &col = iip ? r.rgba : p.vertices[2].rgba;
                e.r = col[0]; e.g = col[1]; e.b = col[2]; e.a = col[3];
                e.fog = r.fog;
                same = std::memcmp(&e, &out[v + k], sizeof(G178Vtx)) == 0;
                if (!same && logs < 16u)
                {
                    ++logs;
                    const G178Vtx &g = out[v + k];
                    std::fprintf(stderr,
                                 "[G788:gvbad] face=%u prim=%zu k=%u clipped=%u exp=(%.6f,%.6f,%.0f st=%.9g,%.9g,%.9g c=%u,%u,%u,%u f=%u) "
                                 "got=(%.6f,%.6f,%.0f st=%.9g,%.9g,%.9g c=%u,%u,%u,%u f=%u)\n",
                                 job.face, cursor - 1u, k, unsigned(p.clipped), double(e.x), double(e.y), double(e.z), double(e.sq),
                                 double(e.tq), double(e.iq), e.r, e.g, e.b, e.a, e.fog, double(g.x), double(g.y), double(g.z),
                                 double(g.sq), double(g.tq), double(g.iq), g.r, g.g, g.b, g.a, g.fog);
                }
            }
            g_g788C.gvTris.fetch_add(1u, std::memory_order_relaxed);
            if (!same)
                g_g788C.gvBad.fetch_add(1u, std::memory_order_relaxed);
        }
        if (cursor < prims.size())
            g_g788C.gvMissing.fetch_add(prims.size() - cursor, std::memory_order_relaxed);
    }
}

// Same-frame diagnostic only. Two private colour/depth clones of the live batch input:
// exact legacy reference vertices versus native GPU output, with identical ordered draw state.
// Render neither arm into the live surface. Static texture/GPU mesh authority is shared.
bool m_g788ShadowActive = false;
uint32_t m_g788ShadowBucket = ~0u, m_g788ShadowInBucket = 0u, m_g788ShadowSamples = 0u;
void g788ShadowRaster(const G178Batch &original, uint64_t depthKey,
                     const std::vector<float> *depthUpload, int depthY, int depthRows)
{
    static const bool enabled = [] { const char *e = std::getenv("DC2_G788_SHADOW"); return e && std::atoi(e); }();
    if (!enabled || m_g788ShadowActive || original.g788Jobs.empty() || original.g777Native || original.g778Shadow)
        return;
    const uint32_t sf = original.g788Jobs.front().rec->draw.frame;
    if (sf < 1400u || sf > 20000u || m_g788ShadowSamples >= 64u) return;
    const uint32_t bucket = sf / 600u;
    if (bucket != m_g788ShadowBucket) { m_g788ShadowBucket = bucket; m_g788ShadowInBucket = 0u; }
    if (m_g788ShadowInBucket >= 2u) return;
    if (std::any_of(original.draws.begin(), original.draws.end(), [](const G178Draw &d) { return d.srcFbp != 0u; }))
        return;
    ++m_g788ShadowInBucket; ++m_g788ShadowSamples;
    m_g788ShadowActive = true;
    struct Reset { bool &active; ~Reset() { active = false; } } reset{m_g788ShadowActive};
    const auto fail = [&](unsigned reason) {
        std::fprintf(stderr, "[G788:shadow] sf=%u sample=%u refused=%u\n", sf, m_g788ShadowSamples, reason);
    };
    constexpr uint32_t expectedKey = 0x78810001u, candidateKey = 0x78810002u;
    constexpr uint64_t expectedDepth = 0x7881000100000001ull, candidateDepth = 0x7881000200000001ull;
    GLint drawFbo = 0, readFbo = 0, viewport[4]{};
    glGetIntegerv(0x8ca6, &drawFbo); glGetIntegerv(0x8caa, &readFbo); glGetIntegerv(GL_VIEWPORT, viewport);
    struct Restore {
        decltype(m_f) &f; GLint draw, read; GLint *viewport;
        ~Restore() { f.glBindFramebuffer_(0x8ca9, draw); f.glBindFramebuffer_(0x8ca8, read);
                     glViewport(viewport[0], viewport[1], viewport[2], viewport[3]); }
    } restore{m_f, drawFbo, readFbo, viewport};
    if (!g778SeedShadow(original, depthKey, expectedKey, expectedDepth, depthUpload != nullptr) ||
        !g778SeedShadow(original, depthKey, candidateKey, candidateDepth, depthUpload != nullptr)) { fail(1); return; }
    auto expected = original, candidate = original;
    expected.fboKey = expectedKey; candidate.fboKey = candidateKey;
    expected.skipReadback = candidate.skipReadback = true;
    expected.g788Jobs.clear(); expected.verts.clear();
    std::vector<dc2_native::ReferencePrimitive> primitives;
    size_t nativeFaces = 0, triangles = 0;
    for (auto &d : expected.draws)
    {
        const int first = d.firstVtx, count = d.vtxCount;
        d.firstVtx = static_cast<int>(expected.verts.size());
        if (!d.g788Seq)
        {
            if (first < 0 || count < 0 || size_t(first) + size_t(count) > original.verts.size()) { fail(2); return; }
            expected.verts.insert(expected.verts.end(), original.verts.begin() + first, original.verts.begin() + first + count);
            continue;
        }
        const auto job = std::find_if(original.g788Jobs.begin(), original.g788Jobs.end(),
                                      [&](const G788BatchJob &j) { return j.outBase == d.g788Out; });
        if (job == original.g788Jobs.end()) { fail(3); return; }
        const uint32_t rangeEnd = d.g788Out + d.g788Count;
        for (auto member = job; member != original.g788Jobs.end() && member->outBase < rangeEnd; ++member)
        {
            if (member->outBase + 24u * member->triCount > rangeEnd) { fail(8); return; }
            primitives.clear();
            if (!g788FacePrims(*member->rec, member->face, primitives, false)) { fail(4); return; }
            ++nativeFaces; triangles += primitives.size();
            for (const auto &p : primitives) for (const auto &r : p.vertices)
            {
                G178Vtx v{};
                v.x = float(r.x & 65535u) / 16.0f - float(member->ofx);
                v.y = float(r.y & 65535u) / 16.0f - float(member->ofy);
                v.z = float(r.z & 0xffffffu);
                const float q = r.q == 0.0f ? 1.0f : r.q;
                if (member->flags & 1u) { v.sq = r.s; v.tq = r.t; v.iq = std::fabs(q) > 1.0e-8f ? q : 1.0f; }
                else { v.sq = v.tq = 0.0f; v.iq = 1.0f; }
                const auto &colour = member->flags & 2u ? r.rgba : p.vertices[2].rgba;
                v.r = colour[0]; v.g = colour[1]; v.b = colour[2]; v.a = colour[3]; v.fog = r.fog;
                expected.verts.push_back(v);
            }
        }
        d.g788Seq = d.g788Out = d.g788Count = 0u;
        d.vtxCount = static_cast<int>(expected.verts.size()) - d.firstVtx;
    }
    std::vector<uint32_t> seed, before, after;
    std::vector<float> seedZ, beforeZ, afterZ;
    g778ReadShadow(expectedKey, depthKey ? expectedDepth : 0u, expected, seed, seedZ);
    if (!renderBatch(expected, depthKey ? expectedDepth : 0u, depthUpload, depthY, depthRows)) { fail(5); return; }
    g778ReadShadow(expectedKey, depthKey ? expectedDepth : 0u, expected, before, beforeZ);
    if (!renderBatch(candidate, depthKey ? candidateDepth : 0u, depthUpload, depthY, depthRows)) { fail(6); return; }
    g778ReadShadow(candidateKey, depthKey ? candidateDepth : 0u, candidate, after, afterZ);
    if (before.size() != after.size() || beforeZ.size() != afterZ.size() || glGetError() != GL_NO_ERROR) { fail(7); return; }
    size_t colourDifferent = 0, depthDifferent = 0, touched = 0;
    for (size_t i = 0; i < before.size(); ++i)
    {
        colourDifferent += before[i] != after[i];
        const bool zChanged = i < beforeZ.size() && std::memcmp(&beforeZ[i], &afterZ[i], sizeof(float)) != 0;
        depthDifferent += zChanged;
        touched += before[i] != seed[i] || (i < beforeZ.size() && std::memcmp(&beforeZ[i], &seedZ[i], sizeof(float)) != 0);
    }
    std::fprintf(stderr, "[G788:shadow] sf=%u sample=%u faces=%zu triangles=%zu pixels=%zu touched=%zu colourDifferent=%zu depthDifferent=%zu\n",
                 sf, m_g788ShadowSamples, nativeFaces, triangles, before.size(), touched, colourDifferent, depthDifferent);
}
