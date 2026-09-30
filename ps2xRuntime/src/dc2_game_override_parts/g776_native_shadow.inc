#if defined(PS2X_G775_DIAG)
static void g776_native_shadow(dc2_g775_capture::Draw invocation,const uint8_t* ram)
{
    using namespace dc2_g775_capture;
    using namespace dc2_native;
    static const bool enabled=std::getenv("DC2_G776_SHADOW")!=nullptr;
    if(!enabled || !isCaptured(invocation.id) || invocation.vtable!=0x375140)return;
    DC2NativeDraw draw;draw.invocation=invocation.id;draw.frame=invocation.sf;draw.parent=invocation.parent;
    auto vectorAt=[&](uint32_t address){Vector value{};if(valid(address,16))std::memcpy(value.data(),ram+address,16);return value;};
    auto matrixAt=[&](uint32_t address){Matrix value{};for(uint32_t i=0;i<4;++i)value[i]=vectorAt(address+i*16);return value;};
    const uint32_t renderer=readWord(ram,invocation.manager+0x64)&0x1fffffffu;
    const uint32_t config=readWord(ram,renderer+0xfcc)&0x1fffffffu;
    if(!valid(renderer,0x1100) || !valid(config,0x90) || vectorAt(config+0x80)[3]>1)return;
    draw.world=matrixAt(invocation.world);draw.cameraScreen=matrixAt(renderer+0x10);draw.cameraClip=matrixAt(renderer+0x220);
    draw.cameraViewport=matrixAt(renderer+0x260);
    const uint32_t flags=readWord(ram,config+0x40);
    draw.renderFlags=bool(readWord(ram,renderer+0xfc0) || readWord(ram,renderer+0xfc4)) | (bool(readWord(ram,renderer+0xfc4))<<1);
    if(flags || readWord(ram,config+0x2c) || readWord(ram,renderer+0xfc8) || readWord(ram,renderer+0x1010) ||
       (!readWord(ram,config+0x60) && readWord(ram,renderer+0xfac) && readWord(ram,config+0x4c)) || readWord(ram,config+0x84)==1)return;
    const uint32_t lights=renderer+0x400+readWord(ram,renderer+0x3f4)*0x150;
    if(!valid(lights,0x150))return;
    for(uint32_t i=0;i<3;++i)draw.lightDirections[i]=vectorAt(lights+i*16);
    draw.lightColours=matrixAt(lights+0x40);draw.ambient=vectorAt(lights+0x80);
    draw.ambient[3]*=vectorAt(config+0x40)[1];
    const auto fog=vectorAt(renderer+0xfdc);draw.fog={fog[0],fog[2],fog[1],fog[3]};
    const uint32_t materialCount=readWord(ram,invocation.self+0x40),materials=readWord(ram,invocation.self+0x44)&0x1fffffffu;
    if(!materialCount || materialCount>256 || !valid(materials,materialCount*0x30))return;
    for(uint32_t i=0;i<materialCount;++i)draw.materials.push_back({vectorAt(materials+i*0x30),readWord(ram,materials+i*0x30+0x20)});
    auto mesh=std::make_shared<Mesh>();mesh->identity=invocation.self;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto source=meshSources.find(invocation.self);
        const auto buckets=loadedBuckets.find(invocation.self);
        if(source==meshSources.end() || buckets==loadedBuckets.end())return;
        auto sourceVector=[&](uint32_t stream,uint32_t index,Vector& value)
        {if(index>=source->second.counts[stream] || size_t(index)*16+16>source->second.arrays[stream].size())return false;std::memcpy(value.data(),source->second.arrays[stream].data()+size_t(index)*16,16);return true;};
        uint32_t bucket=readWord(ram,invocation.self+0x48)&0x1fffffffu;
        for(uint32_t b=0;b<256 && valid(bucket,0x20);++b)
        {
            const auto retained=buckets->second.find(bucket);
            if(retained==buckets->second.end())return;
            const uint32_t material=readWord(ram,bucket);
            for(const auto& face:retained->second.faces)
            {
                uint16_t header[6];std::memcpy(header,face.header.data(),sizeof(header));
                const auto& indices=face.indices;
                const bool colour=bool(header[0]&0x100);
                if(header[0]&~0x107u || (header[0]&7)!=3 && (header[0]&7)!=4 || header[1]!=3+colour ||
                   indices.size()!=size_t(header[3])*4 || header[3]!=header[1]*header[4])return;
                Face output;output.topology=header[0]&7;output.material=material;
                output.bucket=bucket;output.batchCorners=((readWord(ram,invocation.self+0x14)-2)/header[1]/3)*3;
                if(output.batchCorners<3)return;
                for(uint32_t v=0;v<header[4];++v)
                {
                    uint32_t index[4]{};std::memcpy(index,indices.data()+size_t(v)*header[1]*4,header[1]*4);
                    Vertex vertex;vertex.colour={1,1,1,1};
                    if(!sourceVector(0,index[0],vertex.position) || !sourceVector(1,index[1],vertex.normal) ||
                       !sourceVector(3,index[2],vertex.uv) || (colour && !sourceVector(2,index[3],vertex.colour)))return;
                    if(vertex.uv[2]!=1.0f)return;
                    for(const auto* values:{&vertex.position,&vertex.normal,&vertex.uv,&vertex.colour})
                        for(const auto value:*values)if(!std::isfinite(value))return;
                    output.indices.push_back(uint32_t(mesh->vertices.size()));mesh->vertices.push_back(vertex);
                }
                mesh->faces.push_back(std::move(output));
            }
            bucket=readWord(ram,bucket+8)&0x1fffffffu;
        }
    }
    mesh->identity=14695981039346656037ull;
    auto appendHash=[&](const void* value,size_t bytes){const auto* p=static_cast<const uint8_t*>(value);for(size_t i=0;i<bytes;++i)mesh->identity=(mesh->identity^p[i])*1099511628211ull;};
    appendHash(mesh->vertices.data(),mesh->vertices.size()*sizeof(Vertex));
    for(const auto& face:mesh->faces){appendHash(&face.topology,sizeof(face.topology));appendHash(&face.material,sizeof(face.material));appendHash(face.indices.data(),face.indices.size()*sizeof(uint32_t));}
    draw.geometry=mesh;
    std::vector<ReferenceVertex> vertices;
    if(!referenceTransform(draw,vertices,true))return;
    static FILE* file=[] {FILE* f=std::fopen("logs/g775/native_candidates.csv","wb");if(f)std::fprintf(f,"draw,sf,self,face,corner,x,y,z,q,s,t,r,g,b,a,fog\n");return f;}();
    if(!file)return;
    for(const auto& vertex:vertices)
        std::fprintf(file,"%llu,%u,%x,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u\n",static_cast<unsigned long long>(draw.invocation),draw.frame,invocation.self,vertex.face,vertex.corner,vertex.x,vertex.y,vertex.z,double(vertex.q),double(vertex.s),double(vertex.t),vertex.rgba[0],vertex.rgba[1],vertex.rgba[2],vertex.rgba[3],vertex.fog);
    std::fflush(file);
    std::vector<ReferencePrimitive> primitives;ReferenceStats stats;
    if(referencePrimitives(draw,primitives,stats))
    {
        static FILE* primitiveFile=[] {FILE* f=std::fopen("logs/g775/native_primitives.csv","wb");if(f)std::fprintf(f,"draw,primitive,corner,face,material,topology,clipped,x,y,z,q,s,t,r,g,b,a,fog,bucket,batch,source_corner\n");return f;}();
        if(primitiveFile)
        {
            for(size_t index=0;index<primitives.size();++index)for(size_t corner=0;corner<3;++corner)
            {
                const auto& primitive=primitives[index];const auto& vertex=primitive.vertices[corner];
                std::fprintf(primitiveFile,"%llu,%zu,%zu,%u,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%u,%u\n",
                    static_cast<unsigned long long>(draw.invocation),index,corner,primitive.face,primitive.material,primitive.sourceTopology,unsigned(primitive.clipped),
                    vertex.x,vertex.y,vertex.z,double(vertex.q),double(vertex.s),double(vertex.t),vertex.rgba[0],vertex.rgba[1],vertex.rgba[2],vertex.rgba[3],vertex.fog,primitive.bucket,primitive.batch,primitive.sourceCorner);
            }
            std::fflush(primitiveFile);
        }
    }
    std::fprintf(stderr,"[G776:shadow] draw=%llu vertices=%zu original=retained clipping-parity=pending\n",static_cast<unsigned long long>(draw.invocation),vertices.size());
}
#endif
