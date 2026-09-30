// Structured DC2 geometry mathematics. No VU instruction/state representation.
inline static const char* kG777NativeGeometryCS=R"GLSL(
#version 430 core
layout(local_size_x=64) in;
layout(std430,binding=0) readonly buffer Source {vec4 source[];};
layout(std430,binding=1) readonly buffer Constants {vec4 constants[];};
layout(std430,binding=2) writeonly buffer Vertices {uint vertices[];};
layout(std430,binding=3) buffer Count {uint count;uint failure;uint capacity;uint unused;};
layout(std430,binding=5) writeonly buffer Sizes {uvec2 sizes[];};
struct Vertex {vec4 screen;vec4 clip;vec4 colour;vec4 uv;};
precise vec4 matrixPoint(int base,vec4 point){
    precise vec4 outValue=constants[base]*point.x;
    outValue=outValue+constants[base+1]*point.y;
    outValue=outValue+constants[base+2]*point.z;
    outValue=outValue+constants[base+3]*point.w;
    return outValue;
}
float divide32(float numerator,float denominator){return float(double(numerator)/double(denominator));}
Vertex transformVertex(int at){
    Vertex outValue;vec4 position=source[at],normal=source[at+1];
    outValue.screen=matrixPoint(0,position);outValue.clip=matrixPoint(4,position);
    precise vec4 light=constants[12]*normal.x;
    light=light+constants[13]*normal.y;light=light+constants[14]*normal.z;
    light=max(light,vec4(0));
    precise vec4 colour=constants[19];
    colour=colour+constants[15]*light.x;colour=colour+constants[16]*light.y;
    colour=colour+constants[17]*light.z;colour=colour+constants[18]*light.w;
    colour=min(colour,vec4(255));colour=colour*source[at+3];
    outValue.colour=min(colour,vec4(255));outValue.uv=source[at+2];return outValue;
}
Vertex interpolateVertex(Vertex a,Vertex b,float fraction){
    Vertex v;
    precise vec4 delta=b.clip-a.clip;precise vec4 value=delta*fraction;v.clip=a.clip+value;
    delta=b.screen-a.screen;value=delta*fraction;v.screen=a.screen+value;
    delta=b.colour-a.colour;value=delta*fraction;v.colour=a.colour+value;
    delta=b.uv-a.uv;value=delta*fraction;v.uv=a.uv+value;return v;
}
bool outside(vec4 p){return any(greaterThan(abs(p.xyz),vec3(abs(p.w))));}
uvec3 project(Vertex v,bool clipped,out float q){
    precise vec4 screen=clipped?matrixPoint(8,v.clip):v.screen;
    q=divide32(1.0,screen.w);precise vec4 projected=screen*q;
    if(any(isnan(projected)) || any(isinf(projected)) || abs(projected.x)>1e7 || abs(projected.y)>1e7 || abs(projected.z)>1e9){atomicOr(failure,1u);return uvec3(0);}
    return uvec3(uint(int(projected.x*16.0))&65535u,uint(int(projected.y*16.0))&65535u,uint(int(projected.z))&0xffffffu);
}
void storeVertex(Vertex v,uvec3 position,float q,uint index){
    uint at=index*8u;
    vertices[at]=floatBitsToUint(float(position.x)/16.0-constants[21].z);
    vertices[at+1u]=floatBitsToUint(float(position.y)/16.0-constants[21].w);
    vertices[at+2u]=floatBitsToUint(float(position.z));
    precise float s=v.uv.x*q;precise float t=v.uv.y*q;
    vertices[at+3u]=floatBitsToUint(s);vertices[at+4u]=floatBitsToUint(t);vertices[at+5u]=floatBitsToUint(q);
    uvec4 colour=uvec4(ivec4(v.colour))&uvec4(255);
    vertices[at+6u]=colour.x|(colour.y<<8u)|(colour.z<<16u)|(colour.w<<24u);
    precise float fog=constants[20].w*q;fog=constants[20].x+fog;
    vertices[at+7u]=uint(int(clamp(fog,constants[20].z,constants[20].y)))&255u;
}
void triangle(int triangleIndex){
    uint emitted=0u;
    int base=triangleIndex*13;uvec4 info=uvec4(source[base+12]);
    Vertex polygon[12];Vertex outputPolygon[12];
    for(int i=0;i<3;++i)polygon[i]=transformVertex(base+i*4);
    bool clipped=(uint(constants[22].y)&2u)!=0u && (outside(polygon[0].clip)||outside(polygon[1].clip)||outside(polygon[2].clip));
    sizes[triangleIndex]=uvec2(0u,clipped?1u:0u);
    int polygonCount=3;
    if(clipped)for(int plane=0;plane<6 && polygonCount>0;++plane){
        int axis=plane<2?2:(plane<4?0:1);float sign=(plane&1)==0?1.0:-1.0;int outputCount=0;
        for(int edge=0;edge<polygonCount;++edge){
            Vertex previous=polygon[edge],current=polygon[(edge+1)%polygonCount];
            precise float d0=abs(previous.clip.w)+sign*previous.clip[axis];
            precise float d1=abs(current.clip.w)+sign*current.clip[axis];
            bool before=d0>=0.0,after=d1>=0.0;
            if(before){if(outputCount>=12){atomicOr(failure,2u);return;}outputPolygon[outputCount++]=previous;}
            if(before!=after){
                d0=previous.clip.w+sign*previous.clip[axis];d1=current.clip.w+sign*current.clip[axis];
                precise float difference=d1-d0;float fraction=abs(divide32(d0,difference));
                if(isnan(fraction)||isinf(fraction)||outputCount>=12){atomicOr(failure,4u);return;}
                outputPolygon[outputCount++]=interpolateVertex(previous,current,fraction);
            }
        }
        polygonCount=outputCount;for(int i=0;i<polygonCount;++i)polygon[i]=outputPolygon[i];
    }
    for(int corner=2;corner<polygonCount;++corner){
        Vertex a=polygon[0],b=polygon[corner-1],c=polygon[corner];float qa,qb,qc;
        uvec3 pa=project(a,clipped,qa),pb=project(b,clipped,qb),pc=project(c,clipped,qc);
        double area=double(int(pb.x)-int(pa.x))*double(int(pc.y)-int(pa.y))-double(int(pb.y)-int(pa.y))*double(int(pc.x)-int(pa.x));
        if(area==0.0)continue;if(emitted+3u>30u){atomicOr(failure,8u);return;}
        uint at=uint(triangleIndex)*30u+emitted;
        storeVertex(a,pa,qa,at);storeVertex(b,pb,qb,at+1u);storeVertex(c,pc,qc,at+2u);emitted+=3u;
    }
    sizes[triangleIndex].x=emitted;atomicAdd(count,emitted);
}
void main(){
    uint i=gl_GlobalInvocationID.x;if(i>=uint(constants[22].x))return;
    triangle(int(i));
}
)GLSL";

// Geometry runs independently per source triangle. This second pass only
// compacts its bounded fans into original face/batch/clipped/source ordering.
inline static const char* kG778NativeCompactCS=R"GLSL(
#version 430 core
layout(local_size_x=64) in;
layout(std430,binding=0) readonly buffer Source {vec4 source[];};
layout(std430,binding=1) readonly buffer Constants {vec4 constants[];};
layout(std430,binding=2) readonly buffer Scratch {uint scratch[];};
layout(std430,binding=3) readonly buffer Count {uint count;uint failure;uint capacity;uint unused;};
layout(std430,binding=4) writeonly buffer Metadata {uvec4 metadata[];};
layout(std430,binding=5) readonly buffer Sizes {uvec2 sizes[];};
layout(std430,binding=6) writeonly buffer Vertices {uint vertices[];};
void main(){
    uint i=gl_GlobalInvocationID.x,n=uint(constants[22].x);
    if(i>=n || failure!=0u)return;
    uvec4 info=uvec4(source[i*13u+12u]);uint offset=0u;
    for(uint j=0u;j<n;++j){
        uvec4 other=uvec4(source[j*13u+12u]);
        bool same=other.x==info.x && other.z==info.z;
        bool before=(!same && j<i) || (same && (sizes[j].y>sizes[i].y || (sizes[j].y==sizes[i].y && j<i)));
        if(before)offset+=sizes[j].x;
    }
    for(uint v=0u;v<sizes[i].x;++v){
        for(uint word=0u;word<8u;++word)vertices[(offset+v)*8u+word]=scratch[(i*30u+v)*8u+word];
        if(v%3u==0u)metadata[(offset+v)/3u]=uvec4(info.xyz,info.w|(sizes[i].y<<16u));
    }
}
)GLSL";

// GPU-side draw length. The six clipping planes can add at most one polygon
// vertex each: 3 + 6 = 9 vertices, or 7 triangles / 21 emitted vertices per
// source triangle. The existing 30-vertex stride leaves spare capacity.
// Failures suppress raster without a GPU->CPU control read on the live path.
inline static const char* kG779NativeIndirectCS=R"GLSL(
#version 430 core
layout(local_size_x=1) in;
layout(std430,binding=3) readonly buffer Count {uint count;uint failure;uint capacity;uint unused;};
layout(std430,binding=7) writeonly buffer Indirect {uint vertexCount;uint instanceCount;uint firstVertex;uint baseInstance;};
void main(){
    vertexCount=(failure==0u && count<=capacity && count%3u==0u)?count:0u;
    instanceCount=1u;firstVertex=0u;baseInstance=0u;
}
)GLSL";
