#ifndef TERRAIN_DETAIL_LOD_H
#define TERRAIN_DETAIL_LOD_H
#include "common.h"
#include "sload.h"

// View-space distance in metres. Keep all four material layers nearby.
static const float terrainDetailFadeStart = 35.0;
static const float terrainDetailFadeEnd = 50.0;

void addTerrainLayer(Texture2D diffuseMap, Texture2D normalMap, float weight,
    float2 uv, float2 dx, float2 dy, inout float3 colour,
    inout float3 tangentNormal, inout float gloss)
{
    // Most mask texels contain only one or two layers. A zero contribution
    // needs neither its diffuse nor its normal/gloss texture.
    [branch] if (weight > 0.0)
    {
        colour += diffuseMap.SampleGrad(smp_base, uv, dx, dy).rgb * weight;
        float4 n = normalMap.SampleGrad(smp_linear, uv, dx, dy).wzyx;
        tangentNormal += (n.xyz - 0.5) * weight;
        gloss += n.w * weight;
    }
}

f_deffer main(p_bumped I)
{
    float4 D = tbase(I.tcdh);
    float3 geometricN = normalize(float3(I.M1.z, I.M2.z, I.M3.z));
    float3 N = geometricN;
    float G = 0.0;
    float distanceSquared = dot(I.position.xyz, I.position.xyz);

    // Derivatives must be evaluated before the distance-dependent branch.
    float2 baseDx = ddx(I.tcdh), baseDy = ddy(I.tcdh);
    float2 detailDx = ddx(I.tcdbump), detailDy = ddy(I.tcdbump);
    [branch] if (distanceSquared < terrainDetailFadeEnd * terrainDetailFadeEnd)
    {
        float detailWeight = 1.0 - smoothstep(terrainDetailFadeStart,
            terrainDetailFadeEnd, sqrt(distanceSquared));
        float4 mask = s_mask.SampleGrad(smp_base, I.tcdh, baseDx, baseDy);
        mask /= dot(mask, 1.0);
        float3 dt = 0.0;
        float3 tangentN = 0.0;
        float Gdetail = 0.0;
        addTerrainLayer(s_dt_r, s_dn_r, mask.r, I.tcdbump, detailDx, detailDy, dt, tangentN, Gdetail);
        addTerrainLayer(s_dt_g, s_dn_g, mask.g, I.tcdbump, detailDx, detailDy, dt, tangentN, Gdetail);
        addTerrainLayer(s_dt_b, s_dn_b, mask.b, I.tcdbump, detailDx, detailDy, dt, tangentN, Gdetail);
        addTerrainLayer(s_dt_a, s_dn_a, mask.a, I.tcdbump, detailDx, detailDy, dt, tangentN, Gdetail);
        D.rgb *= lerp(1.0, 2.0 * dt, detailWeight);
        // Preserve the original, deliberately stronger terrain normal mapping.
        tangentN.z *= 0.5;
        float3 detailedN = normalize(mul(float3x3(I.M1, I.M2, I.M3), tangentN));
        N = normalize(lerp(geometricN, detailedN, detailWeight));
        G = Gdetail * detailWeight;
    }
    float material = xmaterial;
#ifdef USE_R2_STATIC_SUN
    material = s_lmap.Sample(smp_base, I.tcdh).w;
#endif
    return pack_gbuffer(float4(N, D.w),
        float4(I.position.xyz + N * def_virtualh / 2.0, material), float4(D.rgb, G));
}
#endif
