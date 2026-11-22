#include "stdafx.h"
#pragma hdrstop
#include "DetailModel.h"
#include "xrCore/FMesh.hpp"

#if !defined(_EDITOR) && defined(USE_DX11)
#include "xrstripify.h"
#endif

namespace xray::render::RENDER_NAMESPACE
{
CDetail::~CDetail() {}
void CDetail::Unload()
{
    if (vertices)
    {
        xr_free(vertices);
        vertices = nullptr;
    }
    if (indices)
    {
        xr_free(indices);
        indices = nullptr;
    }
    shader.destroy();
}

/** Dirty hack - transfer indices (in 32bit lines) (2 indices by pass)
 * @brief CDetail::transfer_indices
 * @param iDest
 * @param iOffset
 */
void CDetail::transfer_indices(u32* iDest, u32 iOffset)
{
    for (u32 i = 0; i < number_indices; ++i)
        iDest[i] = indices[i] + iOffset;
}

void CDetail::transfer(Fmatrix& mXform, fvfVertexOut* vDest, u32 C, u32* iDest, u32 iOffset)
{
    // Transfer vertices
    {
        CDetail::fvfVertexIn *srcIt = vertices, *srcEnd = vertices + number_vertices;
        CDetail::fvfVertexOut* dstIt = vDest;
        for (; srcIt != srcEnd; srcIt++, dstIt++)
        {
            mXform.transform_tiny(dstIt->P, srcIt->P);
            dstIt->C = C;
            dstIt->u = srcIt->u;
            dstIt->v = srcIt->v;
        }
    }

    transfer_indices(iDest, iOffset);
}

void CDetail::transfer(Fmatrix& mXform, fvfVertexOut* vDest, u32 C, u32* iDest, u32 iOffset, float du, float dv)
{
    // Transfer vertices
    {
        CDetail::fvfVertexIn *srcIt = vertices, *srcEnd = vertices + number_vertices;
        CDetail::fvfVertexOut* dstIt = vDest;
        for (; srcIt != srcEnd; srcIt++, dstIt++)
        {
            mXform.transform_tiny(dstIt->P, srcIt->P);
            dstIt->C = C;
            dstIt->u = srcIt->u + du;
            dstIt->v = srcIt->v + dv;
        }
    }

    transfer_indices(iDest, iOffset);
}

void CDetail::Load(IReader* S)
{
    // Shader
    string256 fnT, fnS;
    S->r_stringZ(fnS, sizeof(fnS));
    S->r_stringZ(fnT, sizeof(fnT));
    shader.create(fnS, fnT);

    // Params
    m_Flags.assign(S->r_u32());
    m_fMinScale = S->r_float();
    m_fMaxScale = S->r_float();
    number_vertices = S->r_u32();
    const u32 taggedCount = S->r_u32();
    const bool wideIndices = (taggedCount & xrOGF_Index32) != 0;
    number_indices = taggedCount & ~xrOGF_Index32;
    R_ASSERT2(number_vertices && number_indices && 0 == (number_indices % 3), "Invalid detail model geometry counts");

    // Vertices
    u32 size_vertices = number_vertices * sizeof(fvfVertexIn);
    vertices = xr_alloc<CDetail::fvfVertexIn>(number_vertices);
    S->r(vertices, size_vertices);

    // Indices
    const size_t indexBytes = size_t(number_indices) * (wideIndices ? sizeof(u32) : sizeof(u16));
    R_ASSERT2(indexBytes <= size_t(S->elapsed()), "Truncated detail model index data");
    indices = xr_alloc<u32>(number_indices);
    for (u32 i = 0; i < number_indices; ++i)
    {
        indices[i] = wideIndices ? S->r_u32() : S->r_u16();
        R_ASSERT2(indices[i] < number_vertices, "Detail model index exceeds vertex count");
    }

    // Calc BB & SphereRadius
    bv_bb.invalidate();
    for (u32 i = 0; i < number_vertices; i++)
        bv_bb.modify(vertices[i].P);
    bv_bb.getsphere(bv_sphere.P, bv_sphere.R);

#if !defined(_EDITOR) && defined(USE_DX11)
    Optimize();
#endif
}

#if !defined(_EDITOR) && defined(USE_DX11)
void CDetail::Optimize()
{
    if (number_vertices > 65535)
        return; // legacy stripifier and vertex permutation use u16
    xr_vector<u16> vec_indices, vec_permute;
    const int cache = HW.Caps.geometry.dwVertexCache;

    // Stripify
    vec_indices.reserve(number_indices);
    for (u32 i = 0; i < number_indices; ++i)
        vec_indices.push_back(u16(indices[i]));
    vec_permute.resize(number_vertices);
    int vt_old = xrSimulate(vec_indices, cache);
    xrStripify(vec_indices, vec_permute, cache, 0);
    int vt_new = xrSimulate(vec_indices, cache);
    if (vt_new < vt_old)
    {
        // Msg					("* DM: %d verts, %d indices, VT: %d/%d",number_vertices,number_indices,vt_old,vt_new);

        // Copy faces
        for (size_t i = 0; i < vec_indices.size(); ++i)
            indices[i] = vec_indices[i];

        // Permute vertices
        xr_vector<fvfVertexIn> verts;
        verts.assign(vertices, vertices + number_vertices);
        for (u32 i = 0; i < verts.size(); i++)
            vertices[i] = verts[vec_permute[i]];
    }
}
#endif
} // namespace xray::render::RENDER_NAMESPACE
