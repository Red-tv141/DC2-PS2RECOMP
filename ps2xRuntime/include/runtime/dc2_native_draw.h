#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>
#include <emmintrin.h>

// Engine rendering inputs. No VIF, VU registers, GIF packets, or GS vertices.
namespace dc2_native
{
using Vector=std::array<float,4>;
using Matrix=std::array<Vector,4>; // Four engine column vectors.
struct Vertex {Vector position,normal,uv,colour;};
struct Face {uint16_t topology=0;uint32_t material=0;std::vector<uint32_t> indices;uint32_t bucket=0,batchCorners=0;};
struct Mesh {uint64_t identity=0;std::vector<Vertex> vertices;std::vector<Face> faces;};
struct Material {Vector colour;uint32_t textureIdentity=0;};
struct DC2NativeDraw
{
    uint64_t invocation=0;
    uint32_t frame=0,parent=0;
    std::shared_ptr<const Mesh> geometry;
    Matrix world,cameraScreen,cameraClip,cameraViewport;
    std::array<Vector,3> lightDirections;
    Matrix lightColours;
    Vector ambient,fog; // A, maximum, minimum, B: clamp(A+B/w,min,max).
    uint32_t renderFlags=0;
    std::vector<Material> materials;
};
// CPU oracle/debug output. Kept outside DC2NativeDraw's input contract.
struct ReferenceVertex
{
    uint32_t face=0,corner=0,x=0,y=0,z=0;
    Vector screen,clip,colour,uv;
    float q=0,s=0,t=0;
    std::array<uint8_t,4> rgba{};
    uint8_t fog=0;
};
struct ReferencePrimitive
{
    uint32_t face=0,material=0,bucket=0,batch=0,sourceCorner=0;
    uint16_t sourceTopology=0;
    bool clipped=false;
    std::array<ReferenceVertex,3> vertices;
};
struct ReferenceStats
{
    uint32_t sourceTriangles=0,rejected=0,clipped=0,degenerate=0;
};
struct ReferenceState
{
    struct Bucket {uint32_t bucket=0,clip=0,observations=0;};
    std::vector<Bucket> buckets;
    void begin(uint32_t bucket){if(buckets.empty() || buckets.back().bucket!=bucket)buckets.push_back({bucket,0,0});}
    void classify(const Vector& v)
    {
        auto& state=buckets.back();const float w=std::fabs(v[3]);uint32_t flags=0;
        for(unsigned axis=0;axis<3;++axis){if(v[axis]>w)flags|=1u<<(axis*2);if(v[axis]<-w)flags|=2u<<(axis*2);}
        state.clip=(state.clip<<6)|flags;++state.observations;
    }
};
inline Vector madd(Vector a,Vector b,Vector c)
{
    Vector out;
    _mm_storeu_ps(out.data(),_mm_add_ps(_mm_loadu_ps(a.data()),_mm_mul_ps(_mm_loadu_ps(b.data()),_mm_loadu_ps(c.data()))));
    return out;
}
inline Vector product(Vector a,Vector b)
{Vector out;_mm_storeu_ps(out.data(),_mm_mul_ps(_mm_loadu_ps(a.data()),_mm_loadu_ps(b.data())));return out;}
inline Vector splat(float v){return {v,v,v,v};}
inline Vector transform(const Matrix& matrix,const Vector& point)
{
    Vector value=product(matrix[0],splat(point[0]));
    for(uint32_t i=1;i<4;++i)value=madd(value,matrix[i],splat(point[i]));
    return value;
}
inline Matrix multiply(const Matrix& a,const Matrix& b)
{Matrix out;for(uint32_t i=0;i<4;++i)out[i]=transform(a,b[i]);return out;}
// Shared G150/G713 constant construction. A native commitment must never reach
// the executor's ordinary nonfinite rejection after legacy is no longer legal.
inline bool buildNativeConstants(const DC2NativeDraw& draw,uint32_t materialIndex,
                                 uint32_t triangles,int fbW,int fbH,int offsetX,int offsetY,
                                 std::array<Vector,23>& constants)
{
    if(materialIndex>=draw.materials.size())return false;
    const auto screen=multiply(draw.cameraScreen,draw.world),clip=multiply(draw.cameraClip,draw.world);
    for(unsigned at=0;at<4;++at){constants[at]=screen[at];constants[at+4]=clip[at];constants[at+8]=draw.cameraViewport[at];}
    for(unsigned axis=0;axis<3;++axis){
        constants[12+axis]=product(draw.lightDirections[0],splat(draw.world[axis][0]));
        constants[12+axis]=madd(constants[12+axis],draw.lightDirections[1],splat(draw.world[axis][1]));
        constants[12+axis]=madd(constants[12+axis],draw.lightDirections[2],splat(draw.world[axis][2]));
    }
    Vector lengths=product(constants[12],constants[12]);lengths=madd(lengths,constants[13],constants[13]);
    lengths=madd(lengths,constants[14],constants[14]);Vector reciprocal;
    for(unsigned at=0;at<4;++at)reciprocal[at]=lengths[at]==0?3.402823466e38f:1.0f/std::sqrt(lengths[at]);
    for(unsigned at=12;at<15;++at)constants[at]=product(constants[at],reciprocal);
    const auto& material=draw.materials[materialIndex];
    for(unsigned at=0;at<4;++at)constants[15+at]=product(draw.lightColours[at],material.colour);
    constants[19]=product(draw.ambient,material.colour);constants[20]=draw.fog;
    constants[21]={float(fbW),float(fbH),float(offsetX),float(offsetY)};
    constants[22]={float(triangles),float(draw.renderFlags),0,0};
    for(const auto& value:constants)for(float component:value)if(!std::isfinite(component))return false;
    return true;
}
inline bool referenceTransform(const DC2NativeDraw& draw,std::vector<ReferenceVertex>& result,bool deferProjection=false)
{
    if(!draw.geometry || draw.renderFlags&~3u)return false;
    const auto screen=multiply(draw.cameraScreen,draw.world),clip=multiply(draw.cameraClip,draw.world);
    std::array<Vector,3> directions;
    for(uint32_t axis=0;axis<3;++axis)
    {
        directions[axis]=product(draw.lightDirections[0],splat(draw.world[axis][0]));
        directions[axis]=madd(directions[axis],draw.lightDirections[1],splat(draw.world[axis][1]));
        directions[axis]=madd(directions[axis],draw.lightDirections[2],splat(draw.world[axis][2]));
    }
    Vector lengths=product(directions[0],directions[0]);
    lengths=madd(lengths,directions[1],directions[1]);lengths=madd(lengths,directions[2],directions[2]);
    Vector reciprocal;
    for(uint32_t i=0;i<4;++i)reciprocal[i]=lengths[i]==0?3.402823466e38f:1.0f/std::sqrt(lengths[i]);
    for(auto& direction:directions)direction=product(direction,reciprocal);
    for(uint32_t faceIndex=0;faceIndex<draw.geometry->faces.size();++faceIndex)
    {
        const auto& face=draw.geometry->faces[faceIndex];
        if(face.material>=draw.materials.size() || (face.topology!=3 && face.topology!=4))return false;
        const auto& material=draw.materials[face.material];
        Matrix colours;
        for(uint32_t i=0;i<4;++i)colours[i]=product(draw.lightColours[i],material.colour);
        const auto ambient=product(draw.ambient,material.colour);
        for(uint32_t corner=0;corner<face.indices.size();++corner)
        {
            if(face.indices[corner]>=draw.geometry->vertices.size())return false;
            const auto& vertex=draw.geometry->vertices[face.indices[corner]];
            ReferenceVertex output;output.face=faceIndex;output.corner=corner;
            output.screen=transform(screen,vertex.position);output.clip=transform(clip,vertex.position);
            for(const auto* value:{&output.screen,&output.clip})for(float lane:*value)if(!std::isfinite(lane))return false;
            output.q=output.screen[3]!=0?1.0f/output.screen[3]:0.0f;
            const auto projected=product(output.screen,splat(output.q));
            const bool projectable=output.screen[3]!=0 && std::isfinite(output.q) &&
                std::isfinite(projected[0]) && std::isfinite(projected[1]) && std::isfinite(projected[2]) &&
                std::fabs(projected[0])<=1e7f && std::fabs(projected[1])<=1e7f && std::fabs(projected[2])<=1e9f;
            if(!projectable && !deferProjection)return false;
            if(!projectable)output.q=0;
            Vector light=product(directions[0],splat(vertex.normal[0]));
            light=madd(light,directions[1],splat(vertex.normal[1]));light=madd(light,directions[2],splat(vertex.normal[2]));
            for(auto& value:light)value=std::max(0.0f,value);
            output.colour=ambient;
            for(uint32_t i=0;i<4;++i)output.colour=madd(output.colour,colours[i],splat(light[i]));
            for(uint32_t i=0;i<4;++i)output.colour[i]=std::min(output.colour[i],255.0f);
            output.colour=product(output.colour,vertex.colour);
            for(uint32_t i=0;i<4;++i)
            {output.colour[i]=std::min(output.colour[i],255.0f);output.rgba[i]=uint8_t(int32_t(output.colour[i]));}
            output.uv=vertex.uv;output.s=vertex.uv[0]*output.q;output.t=vertex.uv[1]*output.q;
            if(projectable){
                output.x=uint32_t(int32_t(projected[0]*16.0f))&65535;
                output.y=uint32_t(int32_t(projected[1]*16.0f))&65535;
                output.z=uint32_t(int32_t(projected[2]))&0xffffff;
            }
            const auto fog=madd(splat(draw.fog[0]),splat(draw.fog[3]),splat(output.q))[0];
            output.fog=uint8_t(int32_t(std::max(draw.fog[2],std::min(draw.fog[1],fog))));
            result.push_back(output);
        }
    }
    return true;
}
// DC2 homogeneous clipping semantics. Classification uses absolute W;
// interpolation uses signed plane distances. Preserve the first endpoint
// before emitting its intersection so fan diagonals retain their ordering.
inline bool referencePrimitives(const DC2NativeDraw& draw,std::vector<ReferencePrimitive>& result,ReferenceStats& stats,ReferenceState* state=nullptr)
{
    std::vector<ReferenceVertex> transformed;
    if(!referenceTransform(draw,transformed,true))return false;
    size_t first=0;
    for(uint32_t faceIndex=0;faceIndex<draw.geometry->faces.size();++faceIndex)
    {
        const auto& face=draw.geometry->faces[faceIndex];
        if(state)state->begin(face.bucket);
        if(face.topology==4 && face.batchCorners && face.indices.size()>face.batchCorners)return false;
        const size_t resultStart=result.size();
        for(size_t corner=2;corner<face.indices.size();++corner)
        {
            if(face.topology==3 && corner%3!=2)continue;
            std::array<size_t,3> indices={corner-2,corner-1,corner};
            if(face.topology==4 && corner%2)std::swap(indices[0],indices[2]);
            std::vector<ReferenceVertex> polygon;
            bool clipped=false;
            for(const auto index:indices)
            {
                polygon.push_back(transformed[first+index]);
                const auto& clip=polygon.back().clip;
                if(state && (draw.renderFlags&2))state->classify(clip);
                if(draw.renderFlags&2)for(uint32_t axis=0;axis<3;++axis)
                    clipped|=std::fabs(clip[axis])>std::fabs(clip[3]);
            }
            ++stats.sourceTriangles;
            if(clipped)
            {
                for(const auto axis:{2u,0u,1u})for(const float sign:{1.0f,-1.0f})
                {
                    if(polygon.empty())break;
                    std::vector<ReferenceVertex> output;
                    for(size_t edge=0;edge<polygon.size();++edge)
                    {
                        const auto& previous=polygon[edge];
                        const auto& current=polygon[(edge+1)%polygon.size()];
                        if(state){state->classify(previous.clip);state->classify(current.clip);}
                        const bool before=std::fabs(previous.clip[3])+sign*previous.clip[axis]>=0;
                        const bool after=std::fabs(current.clip[3])+sign*current.clip[axis]>=0;
                        if(before)output.push_back(previous);
                        if(before!=after)
                        {
                            const float d0=previous.clip[3]+sign*previous.clip[axis];
                            const float d1=current.clip[3]+sign*current.clip[axis];
                            const float fraction=std::fabs(d0/(d1-d0));
                            if(!std::isfinite(fraction))return false;
                            ReferenceVertex vertex=previous;
                            auto interpolate=[&](const Vector& a,const Vector& b)
                            {
                                Vector delta;for(uint32_t i=0;i<4;++i)delta[i]=b[i]-a[i];
                                return madd(a,splat(fraction),delta);
                            };
                            vertex.clip=interpolate(previous.clip,current.clip);
                            vertex.screen=interpolate(previous.screen,current.screen);
                            vertex.colour=interpolate(previous.colour,current.colour);
                            vertex.uv=interpolate(previous.uv,current.uv);
                            output.push_back(vertex);
                        }
                    }
                    polygon=std::move(output);
                }
            }
            if(polygon.empty()){++stats.rejected;continue;}
            if(polygon.size()!=3)++stats.clipped;
            const uint32_t sourceCorner=uint32_t(corner);
            for(size_t corner=2;corner<polygon.size();++corner)
            {
                ReferencePrimitive primitive;primitive.face=faceIndex;primitive.material=face.material;
                primitive.sourceTopology=face.topology;primitive.clipped=clipped;
                primitive.bucket=face.bucket;primitive.sourceCorner=sourceCorner;
                primitive.batch=face.batchCorners?sourceCorner/face.batchCorners:0;
                const std::array<size_t,3> fan={0,corner-1,corner};
                for(size_t at=0;at<3;++at)
                {
                    auto vertex=polygon[fan[at]];
                    const auto screen=clipped?transform(draw.cameraViewport,vertex.clip):vertex.screen;
                    if(!std::isfinite(screen[3]) || screen[3]==0)return false;
                    vertex.q=1.0f/screen[3];const auto projected=product(screen,splat(vertex.q));
                    if(!std::isfinite(projected[0]) || !std::isfinite(projected[1]) || !std::isfinite(projected[2]) ||
                       std::fabs(projected[0])>1e7f || std::fabs(projected[1])>1e7f || std::fabs(projected[2])>1e9f)return false;
                    vertex.x=uint32_t(int32_t(projected[0]*16.0f))&65535;
                    vertex.y=uint32_t(int32_t(projected[1]*16.0f))&65535;
                    vertex.z=uint32_t(int32_t(projected[2]))&0xffffff;
                    for(uint32_t i=0;i<4;++i)vertex.rgba[i]=uint8_t(int32_t(vertex.colour[i]));
                    vertex.s=vertex.uv[0]*vertex.q;vertex.t=vertex.uv[1]*vertex.q;
                    const float fog=madd(splat(draw.fog[0]),splat(draw.fog[3]),splat(vertex.q))[0];
                    vertex.fog=uint8_t(int32_t(std::max(draw.fog[2],std::min(draw.fog[1],fog))));
                    primitive.vertices[at]=vertex;
                }
                const auto& a=primitive.vertices[0];const auto& b=primitive.vertices[1];const auto& c=primitive.vertices[2];
                const int64_t area=(int64_t(b.x)-a.x)*(int64_t(c.y)-a.y)-(int64_t(b.y)-a.y)*(int64_t(c.x)-a.x);
                if(area)result.push_back(std::move(primitive));else ++stats.degenerate;
            }
        }
        // Original emits generated clipping fans immediately, then flushes the
        // unchanged primitive stream at the end of each structured source batch.
        std::stable_sort(result.begin()+resultStart,result.end(),[](const ReferencePrimitive& a,const ReferencePrimitive& b)
        {return a.batch!=b.batch?a.batch<b.batch:a.clipped&&!b.clipped;});
        first+=face.indices.size();
    }
    return true;
}
}
