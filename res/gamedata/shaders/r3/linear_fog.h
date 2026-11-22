#ifndef XRAY_LINEAR_FOG_H
#define XRAY_LINEAR_FOG_H

#ifdef USE_LINEAR_RENDERING
// Weather descriptors contain authored sRGB colours. Decode before mixing fog
// with the scene; exposure and display mapping remain in the common final pass.
float3 fog_color_for_scene()
{
    return srgb_to_linear(fog_color.rgb);
}

// The original uniform keeps its name and binding. The macro is declared only
// after the helper has referred to the raw value, and covers deferred/forward/VS.
#define fog_color fog_color_for_scene()
#endif

#endif
