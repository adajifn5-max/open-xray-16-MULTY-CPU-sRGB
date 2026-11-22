// Cached R4 detail geometry: no cyclic function, wind constants, or deformation.
#include "common.h"

uniform float4 consts;
uniform float4 array[64*4];
uniform float4 detail_cache_info; // x: vertices/model; y is reserved (legacy CPU base offset)
StructuredBuffer<float4> detail_positions : register(t3);

v2p_flat main(v_detail v, uint vertexID : SV_VertexID)
{
    v2p_flat O;
    uint i = (uint)v.misc.w;
    float4 m0 = array[i];
    float4 m1 = array[i + 1];
    float4 m2 = array[i + 2];
    float4 c0 = array[i + 3];
    uint vertexCount = (uint)detail_cache_info.x;
    // SV_VertexID is the index-buffer value, without DrawIndexed's base vertex.
    // misc.w already identifies the instance within the shared model batch.
    uint vertex = vertexID - (i / 4) * vertexCount;
    float4 pos = detail_positions[(uint)c0.y * vertexCount + vertex];
    float3 norm = pos.xyz - float3(m0.w, m1.w, m2.w) + float3(0, .75f, 0);
    O.hpos = mul(m_WVP, pos);
    O.N = mul((float3x3)m_WV, normalize(norm));
    float3 Pe = mul(m_WV, pos);
    O.tcdh = float4((v.misc * consts).xyyy);
#ifdef USE_R2_STATIC_SUN
    O.tcdh.w = c0.x;
#endif
    O.position = float4(Pe, c0.w);
    return O;
}
