#ifndef XRAY_LINEAR_COLOR_H
#define XRAY_LINEAR_COLOR_H
// IEC 61966-2-1 transfer functions. Alpha and packed G-buffer data bypass these.
float3 srgb_to_linear(float3 value)
{
    value = max(value, 0.0);
    return float3(value.r <= 0.04045 ? value.r / 12.92 : pow((value.r + 0.055) / 1.055, 2.4),
        value.g <= 0.04045 ? value.g / 12.92 : pow((value.g + 0.055) / 1.055, 2.4),
        value.b <= 0.04045 ? value.b / 12.92 : pow((value.b + 0.055) / 1.055, 2.4));
}
float3 linear_to_srgb(float3 value)
{
    value = max(value, 0.0);
    return float3(value.r <= 0.0031308 ? value.r * 12.92 : 1.055 * pow(value.r, 1.0 / 2.4) - 0.055,
        value.g <= 0.0031308 ? value.g * 12.92 : 1.055 * pow(value.g, 1.0 / 2.4) - 0.055,
        value.b <= 0.0031308 ? value.b * 12.92 : 1.055 * pow(value.b, 1.0 / 2.4) - 0.055);
}
float3 linear_display_map(float3 color)
{
    // A shared peak channel shoulder retains RGB ratios and maps finite HDR to SDR.
    color = max(color, 0.0);
    return color / (1.0 + max(color.r, max(color.g, color.b)));
}
float4 vertex_color_linear(float4 color)
{
#ifdef USE_LINEAR_RENDERING
    color.rgb = srgb_to_linear(color.rgb);
#endif
    return color;
}
#endif
