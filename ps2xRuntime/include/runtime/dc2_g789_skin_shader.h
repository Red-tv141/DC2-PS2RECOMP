#pragma once
// Settled R-B skin/projection/lighting algebra, shared by live C and V.
// Generated once by tools/g789_extract_live_shader.py; not generated at runtime.
#include <string>
namespace dc2_g789_live {
inline constexpr const char* kSkinAlgebra = R"GLSL(
struct Input {vec4 position;vec4 normal;vec4 uv;uvec4 offsets;vec4 weights;};
layout(std430,binding=0) readonly buffer Source {Input source[];};
layout(std430,binding=1) readonly buffer Constants {vec4 constants[];};
layout(std430,binding=3) readonly buffer Metadata {uvec4 metadata[];};
struct Skin {vec3 position;vec3 stq;uvec4 rgba;uint fog;};
precise vec4 point(uint base,vec4 p){
    precise vec4 v=constants[base]*p.x;
    v=v+constants[base+1u]*p.y;v=v+constants[base+2u]*p.z;v=v+constants[base+3u]*p.w;return v;
}
Skin g789Skin(uint id){
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
    precise float s=v.uv.x*q,t=v.uv.y*q;
    float gq=(q==0.0)?1.0:q;float iq=(abs(gq)>1e-8)?gq:1.0;
    Skin result;
    result.position=vec3(float(positionBits.x)/16.0-constants[c+21u].z,
                         float(positionBits.y)/16.0-constants[c+21u].w,float(positionBits.z));
    result.stq=(meta.w&1u)!=0u?vec3(s,t,iq):vec3(0,0,1);
    result.rgba=rgba;result.fog=uint(int(fog))&255u;return result;
}
)GLSL";
inline std::string computeShader(){
    return std::string("#version 430 core\nlayout(local_size_x=64) in;\n")+kSkinAlgebra+R"GLSL(
layout(std430,binding=2) buffer Output {uint vertices[];};
layout(location=0) uniform int count;
void main(){
    uint id=gl_GlobalInvocationID.x;if(id>=uint(count))return;
    Skin v=g789Skin(id);uint at=metadata[id].z*8u;
    vertices[at]=floatBitsToUint(v.position.x);vertices[at+1u]=floatBitsToUint(v.position.y);
    vertices[at+2u]=floatBitsToUint(v.position.z);vertices[at+3u]=floatBitsToUint(v.stq.x);
    vertices[at+4u]=floatBitsToUint(v.stq.y);vertices[at+5u]=floatBitsToUint(v.stq.z);
    vertices[at+6u]=v.rgba.x|(v.rgba.y<<8u)|(v.rgba.z<<16u)|(v.rgba.w<<24u);
    vertices[at+7u]=v.fog;
}
)GLSL";
}
// Caller supplies the actual selected G178 VS (including its depth policy).
// Only attributes are replaced. Existing fragment/raster semantics stay selected.
inline std::string vertexShader(std::string raster){
    const std::string version="#version 330 core";
    const size_t at=raster.find(version);if(at!=std::string::npos)raster.replace(at,version.size(),"#version 430 core");
    for(const char* input:{"layout(location=0) in vec3 aPos;\n","layout(location=1) in vec3 aSTQ;\n",
                          "layout(location=2) in vec4 aColor;\n","layout(location=3) in vec4 aFog;\n"}){
        const size_t i=raster.find(input);if(i==std::string::npos)return {};raster.erase(i,std::char_traits<char>::length(input));
    }
    const size_t main=raster.find("void main(){\n");if(main==std::string::npos)return {};
    raster.insert(main,kSkinAlgebra);
    const size_t body=raster.find("void main(){\n")+std::string("void main(){\n").size();
    // Match normalized unsigned-byte attribute conversion: round the exact
    // byte/255 quotient to float, rather than a rounded float reciprocal product.
    raster.insert(body,"    Skin skin=g789Skin(uint(gl_VertexID));\n"
        "    vec3 aPos=skin.position,aSTQ=skin.stq;\n"
        "    vec4 aColor=vec4(dvec4(skin.rgba)/double(255.0)),aFog=vec4(float(double(skin.fog)/double(255.0)),0,0,0);\n");
    return raster;
}
}
