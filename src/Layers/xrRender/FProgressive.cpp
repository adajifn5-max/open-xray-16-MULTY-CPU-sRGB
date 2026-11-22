// FProgressive.cpp: implementation of the FProgressive class.
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#pragma hdrstop

#include "xrCore/FMesh.hpp"
#include "FProgressive.h"

namespace xray::render::RENDER_NAMESPACE
{
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

FProgressive::FProgressive() : Fvisual()
{
    xSWI = nullptr;
    last_lod = 0;
}

FProgressive::~FProgressive() {}
void FProgressive::Release()
{
    Fvisual::Release();
    xr_free(nSWI.sw);
    if (xSWI)
    {
        xr_free(xSWI->sw);
        xr_delete(xSWI);
    }
}

void FProgressive::Load(const char* N, IReader* data, u32 dwFlags)
{
    Fvisual::Load(N, data, dwFlags);

    // normal SWI
    destructor<IReader> lods(data->open_chunk(OGF_SWIDATA));
    ReadSlideWindows(lods(), nSWI);
    for (u32 i = 0; i < nSWI.count; ++i)
    {
        const auto& sw = nSWI.sw[i];
        R_ASSERT2(sw.num_verts <= vCount && u64(sw.offset) + u64(sw.num_tris) * 3 <= iCount,
            "Progressive mesh slide-window exceeds geometry");
    }

// fast
#if RENDER != R_R1
    if (m_fast)
    {
        destructor<IReader> geomdef(data->open_chunk(OGF_FASTPATH));
        destructor<IReader> def(geomdef().open_chunk(OGF_SWIDATA));

        xSWI = xr_new<FSlideWindowItem>();
        ReadSlideWindows(def(), *xSWI);
    }
#endif
}

void FProgressive::Render(CBackend& cmd_list, float LOD, bool use_fast_geo)
{
#if RENDER != R_R1
    if (m_fast && use_fast_geo)
    {
        int lod_id = iFloor((1.f - clampr(LOD, 0.f, 1.f)) * float(xSWI->count - 1) + 0.5f);
        VERIFY(lod_id >= 0 && lod_id < int(xSWI->count));
        FSlideWindow& SW = xSWI->sw[lod_id];
        cmd_list.set_Geometry(m_fast->rm_geom);
        cmd_list.Render(D3DPT_TRIANGLELIST, m_fast->vBase, 0, SW.num_verts, m_fast->iBase + SW.offset, SW.num_tris);
        cmd_list.stat.r.s_static.add(SW.num_verts);
    }
    else
    {
        int lod_id = last_lod;
        if (LOD >= 0.f)
        {
            clamp(LOD, 0.f, 1.f);
            lod_id = iFloor((1.f - LOD) * float(nSWI.count - 1) + 0.5f);
            last_lod = lod_id;
        }
        VERIFY(lod_id >= 0 && lod_id < int(nSWI.count));
        FSlideWindow& SW = nSWI.sw[lod_id];
        cmd_list.set_Geometry(rm_geom);
        cmd_list.Render(D3DPT_TRIANGLELIST, vBase, 0, SW.num_verts, iBase + SW.offset, SW.num_tris);
        cmd_list.stat.r.s_static.add(SW.num_verts);
    }
#else
    int lod_id = last_lod;
    if (LOD >= 0.f)
    {
        clamp(LOD, 0.f, 1.f);
        lod_id = iFloor((1.f - LOD) * float(nSWI.count - 1) + 0.5f);
        last_lod = lod_id;
    }
    VERIFY(lod_id >= 0 && lod_id < int(nSWI.count));
    FSlideWindow& SW = nSWI.sw[lod_id];
    RCache.set_Geometry(rm_geom);
    RCache.Render(D3DPT_TRIANGLELIST, vBase, 0, SW.num_verts, iBase + SW.offset, SW.num_tris);
    RCache.stat.r.s_static.add(SW.num_verts);
#endif
}

#define PCOPY(a) a = pFrom->a
void FProgressive::Copy(dxRender_Visual* pSrc)
{
    Fvisual::Copy(pSrc);
    FProgressive* pFrom = (FProgressive*)pSrc;
    PCOPY(nSWI);
    PCOPY(xSWI);
}
} // namespace xray::render::RENDER_NAMESPACE
