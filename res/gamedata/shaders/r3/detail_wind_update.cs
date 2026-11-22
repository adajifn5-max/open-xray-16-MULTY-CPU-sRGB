// Dispatch only from CDetailManager::hw_PrepareWind, never from a draw pass.
// Reuse the same wind functions as the legacy detail shader.
#include "shared\common.h"

StructuredBuffer<float4> detail_source : register(t0); // xyz + quantized height fraction
Buffer<float4> detail_jobs : register(t1); // 3 matrix rows + uint4(slot, count, 0, 0)
RWStructuredBuffer<float4> detail_positions : register(u0);

cbuffer DetailWind
{
    float4 detail_wind_wave;
    float4 detail_wind_dir;
    float4 detail_job_start;
};

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint job = (group.x + (uint)detail_job_start.x) * 4;
    float4 m0 = detail_jobs[job];
    float4 m1 = detail_jobs[job + 1];
    float4 m2 = detail_jobs[job + 2];
    uint2 info = asuint(detail_jobs[job + 3].xy);
    for (uint v = lane; v < info.y; v += 64)
    {
        float4 source = detail_source[v];
        float4 local = float4(source.xyz, 1);
        float4 pos = float4(dot(m0, local), dot(m1, local), dot(m2, local), 1);
        float intensity = (pos.y - m1.w) * calc_cyclic(dot(pos, detail_wind_wave));
        pos.xz += calc_xz_wave(detail_wind_dir.xz * intensity, source.w);
        detail_positions[info.x * info.y + v] = pos;
    }
}
