#pragma once
#include "dc2_g775_capture.h"
#include "dc2_native_draw.h"

namespace dc2_g778 {
struct TransportContract {
    uint32_t end=0,unpackBytes=0;
};

// Validate the complete, independently proven bucket before committing a draw.
// Peek through the existing immutable DMA view; never mutate VIF parser state.
//
// G784: a FixMDT bucket's transport is `faces` sequences, each
//     UNPACK V4-32 1 qw @0x27 (face header) | UNPACK V4-32 n<=170 qw (usn) | MSCAL 2
//     { UNPACK V4-32 n qw | MSCNT }*  | FLUSHA
// with NOP padding, and (contract.internalDirect only) DIRECTs between faces whose GIF payload is
// an A+D list writing only TEXFLUSH (0x3f) or PRMODE (0x1b). Every inter-face PRMODE value must be
// the same value V; G150 then proves V equals the live primitive attributes at the first face (or
// PRMODECONT=1), so no GS state the native envelope captured can change inside the group. The face/kick counts must equal the contract row
// exactly (E91F00: 1 face, 13 kicks -- the G778 shape). Every byte must carry the identical
// narrow provenance. Returns end = first byte after the last face's FLUSHA.
struct TransportShape { uint32_t faces=0,kicks=0,directs=0,prmodes=0; uint64_t prmode=0; };
template<class Read>
inline TransportContract transportContract(uint32_t size,uint32_t start,
    const dc2_g775_capture::Draw& provenance,uint32_t expectFaces,uint32_t expectKicks,
    bool allowDirect,Read read,TransportShape* shape=nullptr)
{
    using namespace dc2_g775_capture;
    const auto& origins=activeOrigins();
    if(!origins || !provenance.id)return {};
    size_t origin=activeOriginIndex();
    uint32_t at=start,bytes=0,faces=0,kicks=0,faceKicks=0,directs=0,prmodes=0;uint64_t prmode=0;
    enum {Header,Source,Kick,AfterFace} phase=Header;
    auto owned=[&](uint32_t from,uint32_t length){
        uint32_t covered=from;
        while(covered<from+length){
            while(origin<origins->size() && covered>=(*origins)[origin].end)++origin;
            if(origin==origins->size())return false;
            const auto& span=(*origins)[origin];const auto& owner=span.draw;
            if(covered<span.begin || owner.id!=provenance.id || owner.bucket!=provenance.bucket ||
               owner.state!=provenance.state || owner.geometry!=provenance.geometry ||
               owner.call!=provenance.call || owner.listNode!=provenance.listNode)return false;
            covered=uint32_t(std::min<size_t>(span.end,from+length));
        }
        return true;
    };
    while(at+4<=size){
        uint32_t value=0;read(at,&value,4);
        uint32_t length=4;
        const uint32_t op=(value>>24)&0x7fu;
        if(value==0){/* Preserve padding and command continuity. */}
        else if((phase==Header || phase==AfterFace) && value==0x6c010027u){
            if(faces==expectFaces)return {}; // more faces than the contract row
            length=20;phase=Source;faceKicks=0;
        }
        else if(phase==Source && (value&0xff00ffffu)==0x6c008000u && ((value>>16)&255u) &&
                ((value>>16)&255u)<=170u){
            length=4+((value>>16)&255u)*16;bytes+=length-4;phase=Kick;
        }else if(phase==Kick && value==(faceKicks?0x17000000u:0x14000002u)){
            ++kicks;++faceKicks;phase=Source;
        }else if(phase==Source && faceKicks && value==0x13000000u){
            // FLUSHA itself remains in the original interpreter.
            if(!owned(at,4))return {};
            ++faces;phase=AfterFace;at+=4;
            if(faces==expectFaces){
                if(kicks!=expectKicks)return {};
                if(shape){shape->faces=faces;shape->kicks=kicks;shape->directs=directs;shape->prmodes=prmodes;shape->prmode=prmode;}
                return {at,bytes+16u*faces};
            }
            continue;
        }else if(phase==AfterFace && allowDirect && op==0x50u && (value&0xffffu)>=2u && (value&0xffffu)<=8u){
            // DIRECT n qw: GIFtag (PACKED, NREG 1, REGS = A+D) + n-1 A+D qwords, TEXFLUSH / PRMODE.
            const uint32_t qw=value&0xffffu;length=4+qw*16;
            if(length>size-at)return {};
            uint64_t tag[2]{};read(at+4,tag,16);
            const uint32_t nloop=uint32_t(tag[0]&0x7fffu),flg=uint32_t((tag[0]>>58)&3u),nreg=uint32_t((tag[0]>>60)&15u);
            if(flg!=0u || nreg!=1u || (tag[1]&15u)!=0xeu || nloop!=qw-1u)return {};
            for(uint32_t i=1;i<qw;++i){
                uint64_t ad[2]{};read(at+4+i*16,ad,16);
                const uint32_t reg=uint32_t(ad[1]&0xffu);
                if(reg==0x1bu){
                    if(prmodes && ad[0]!=prmode)return {};
                    prmode=ad[0];++prmodes;
                }else if(reg!=0x3fu)return {};
            }
            ++directs;
        }else return {};
        if(length>size-at)return {};
        if(!owned(at,length))return {};
        at+=length;
    }
    return {};
}

// Architectural CLIP bookkeeping only. No lighting, projection, output vertices,
// GIF construction or raster submission. Six observations erase incoming history.
// G784: any bucket; list and strip faces in the proven reference order (referencePrimitives:
// strip odd corners emit (c, c-1, c-2)).
inline bool finalClip(const dc2_native::DC2NativeDraw& draw,uint32_t bucket,uint32_t& result)
{
    using namespace dc2_native;
    if(!draw.geometry || !(draw.renderFlags&2))return false;
    const auto matrix=multiply(draw.cameraClip,draw.world);
    std::vector<std::array<uint32_t,3>> triangles;
    for(const auto& f:draw.geometry->faces){
        if(f.bucket!=bucket)continue;
        if((f.topology!=3 && f.topology!=4) || (f.topology==3 && f.indices.size()%3))return false;
        for(size_t corner=2;corner<f.indices.size();++corner){
            if(f.topology==3 && corner%3!=2)continue;
            std::array<uint32_t,3> t={f.indices[corner-2],f.indices[corner-1],f.indices[corner]};
            if(f.topology==4 && (corner&1))std::swap(t[0],t[2]);
            triangles.push_back(t);
        }
    }
    ReferenceState total;total.begin(bucket);
    // Only the suffix is needed for the 32-bit six-bit history register.
    for(auto tri=triangles.rbegin();tri!=triangles.rend();++tri){
        ReferenceState state;state.begin(bucket);
        std::vector<Vector> polygon;bool clipped=false;
        for(const uint32_t index:*tri){
            if(index>=draw.geometry->vertices.size())return false;
            auto v=transform(matrix,draw.geometry->vertices[index].position);
            for(float lane:v)if(!std::isfinite(lane))return false;
            polygon.push_back(v);state.classify(v);
            for(unsigned axis=0;axis<3;++axis)clipped|=std::fabs(v[axis])>std::fabs(v[3]);
        }
        if(clipped)for(unsigned axis:{2u,0u,1u})for(float sign:{1.0f,-1.0f}){
            if(polygon.empty())break;
            std::vector<Vector> output;
            for(size_t edge=0;edge<polygon.size();++edge){
                const auto& a=polygon[edge];const auto& b=polygon[(edge+1)%polygon.size()];
                state.classify(a);state.classify(b);
                const bool before=std::fabs(a[3])+sign*a[axis]>=0;
                const bool after=std::fabs(b[3])+sign*b[axis]>=0;
                if(before)output.push_back(a);
                if(before!=after){
                    const float d0=a[3]+sign*a[axis],d1=b[3]+sign*b[axis];
                    const float fraction=std::fabs(d0/(d1-d0));
                    if(!std::isfinite(fraction))return false;
                    Vector delta;for(unsigned lane=0;lane<4;++lane)delta[lane]=b[lane]-a[lane];
                    output.push_back(madd(a,delta,splat(fraction)));
                }
            }
            polygon=std::move(output);
        }
        auto& suffix=total.buckets.back();const auto& prefix=state.buckets.back();
        const unsigned shift=std::min(32u,suffix.observations*6u);
        suffix.clip|=shift==32?0:prefix.clip<<shift;
        suffix.observations+=prefix.observations;
        if(suffix.observations>=6){result=suffix.clip;return true;}
    }
    return false;
}
inline bool finalClip(const dc2_native::DC2NativeDraw& draw,uint32_t& result)
{return finalClip(draw,0xe91f00u,result);}
}
