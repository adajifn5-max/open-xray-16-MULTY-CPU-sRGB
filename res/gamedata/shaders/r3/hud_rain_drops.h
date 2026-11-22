#ifndef XRAY_HUD_RAIN_DROPS_H
#define XRAY_HUD_RAIN_DROPS_H

float2 hud_rain_hash2(float2 cell)
{
    return frac(sin(float2(dot(cell, float2(127.1, 311.7)),
                           dot(cell, float2(269.5, 183.3)))) * 43758.5453);
}

float2 hud_rain_drop_slope(float2 surface, float time)
{
    // Approximately one-centimetre cells; each impact appears and fades on its
    // own clock. These are surface-normal ripples, not screen-space droplets.
    float2 grid = surface * 96.0f;
    float2 cell = floor(grid);
    float2 seed = hud_rain_hash2(cell);
    float phase = frac(time * 1.7f + seed.x * 7.0f);
    float cycle = floor(time * 1.7f + seed.x * 7.0f);
    float2 centre = 0.30f + hud_rain_hash2(cell + cycle * float2(17.4f, 41.7f)) * 0.40f;
    float2 delta = frac(grid) - centre;
    float radius = lerp(0.04f, 0.22f, phase);
    float distanceToCentre = length(delta);
    float ring = (distanceToCentre - radius) / 0.045f;
    float envelope = smoothstep(0.0f, 0.08f, phase) *
        (1.0f - smoothstep(0.25f, 0.9f, phase));
    float slope = 1.5f * ring * exp(-ring * ring) * envelope;
    return delta / max(distanceToCentre, 0.0001f) * slope;
}

float3 hud_rain_surface_normal(float3 worldPosition, float3 worldNormal, float time)
{
    float3 weights = pow(abs(worldNormal), 4.0f);
    weights /= max(dot(weights, 1.0f), 0.0001f);
    float2 top = hud_rain_drop_slope(worldPosition.xz, time);
    float2 sideX = hud_rain_drop_slope(worldPosition.zy, time + 0.37f);
    float2 sideZ = hud_rain_drop_slope(worldPosition.xy, time + 0.71f);
    float3 slope = float3(top.x, 0.0f, top.y) * weights.y +
        float3(0.0f, sideX.y, sideX.x) * weights.x +
        float3(sideZ.x, sideZ.y, 0.0f) * weights.z;
    // Keep the perturbation tangent to the material surface.
    return slope - worldNormal * dot(slope, worldNormal);
}

#endif
