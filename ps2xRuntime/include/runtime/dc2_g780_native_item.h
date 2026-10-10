#pragma once
#include "runtime/ps2_gs_gpu.h"
#include "runtime/dc2_native_draw.h"
#include <memory>

namespace dc2_g778 {
// Selected GS context + native source, captured at the ordered G150 parse point.
// Resource identifiers live in ctx (FRAME, ZBUF, TEX0, TEXCLUT); G713 FIFO
// orders authority changes before this item reaches the executor.
struct G780NativeItem {
    GSRasterizer* rasterizer=nullptr; // process-lifetime GS member
    uint8_t* vram=nullptr;            // GS::init storage, stable until shutdown
    uint32_t vramSize=0;
    GSContext ctx{};
    GSPrimReg prim{};
    GSTexaReg texa{};
    GSTexClutReg texclut{};
    bool pabe=false;
    uint32_t fogcol=0;
    std::shared_ptr<const dc2_native::DC2NativeDraw> draw;
    uint32_t bucket=0;
#if defined(PS2X_G684_HOT_DIAG)
    uint64_t future139WritesAtCapture=0;
#endif
    // Only the synchronous, non-G713 rollback uses these.
    GSPrimReg previousPrim{};
    int previousCount=0,previousIndex=0;
};

inline void captureG780GsState(G780NativeItem& item,const GSContext& ctx,
                               const GSPrimReg& prim,const GSTexaReg& texa,
                               const GSTexClutReg& texclut,bool pabe,uint32_t fogcol)
{
    item.ctx=ctx;item.prim=prim;item.texa=texa;item.texclut=texclut;
    item.pabe=pabe;item.fogcol=fogcol;
}

// This is the executor's sole materialization of GS state. It is generic only
// so a standalone test can supply a small entry with the G144 fields.
template<class Entry>
inline void fillG780Envelope(const G780NativeItem& item,Entry& envelope)
{
    envelope.ctx=item.ctx;envelope.prim=item.prim;
    envelope.texa=item.texa;envelope.texclut=item.texclut;
    envelope.pabe=item.pabe;envelope.fogcol=item.fogcol;
    const auto& c=item.ctx;
    const float x=float(c.xyoffset.ofx>>4),y=float(c.xyoffset.ofy>>4);
    envelope.v[0].x=x+c.scissor.x0;envelope.v[0].y=y+c.scissor.y0;
    envelope.v[1].x=x+c.scissor.x1+1;envelope.v[1].y=y+c.scissor.y0;
    envelope.v[2].x=x+c.scissor.x0;envelope.v[2].y=y+c.scissor.y1+1;
    for(auto& vertex:envelope.v){vertex.q=1;vertex.r=vertex.g=vertex.b=vertex.a=128;}
    envelope.v[1].s=1;envelope.v[2].t=1;
    envelope.minY=c.scissor.y0;envelope.maxY=c.scissor.y1;
}
}
