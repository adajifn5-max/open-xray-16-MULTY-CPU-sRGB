// Distant alpha-tested TREE foliage. Original geometry/UV/wind; inexpensive
// vertex lighting. Selected per visual, never through global quality settings.
#include "common.h"

uniform float3x4 m_xform;
uniform float4 consts;
uniform float4 c_scale, c_bias, wind, wave;
uniform float2 c_sun;

struct foliage_far_out
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 light : TEXCOORD1;
    float fog : TEXCOORD2;
};

foliage_far_out main(v_tree I)
{
    foliage_far_out O;
    I.Nh = unpack_D3DCOLOR(I.Nh);
    float3 pos = mul(m_xform, I.P);
#ifndef FOLIAGE_STATIC
    float dp = calc_cyclic(wave.w + dot(pos, wave.xyz));
    float H = pos.y - m_xform._24;
    float2 offset = calc_xz_wave(wind.xz * (H * dp), I.tc.z * consts.x);
#ifndef USE_TREEWAVE
    pos.xz += offset;
#endif
#endif
    float4 world = float4(pos, 1);
    float3 N = normalize(mul((float3x3)m_xform, unpack_bx2(I.Nh.xyz)));
    float hemi = saturate(I.Nh.w * c_scale.w + c_bias.w);
    float sunOcclusion = saturate(I.Nh.w * c_sun.x + c_sun.y);
    float diffuse = saturate(dot(N, -L_sun_dir_w));
    O.light = L_ambient.rgb + L_hemi_color.rgb * hemi +
        L_sun_color * sunOcclusion * diffuse;
    O.position = mul(m_VP, world);
    O.uv = I.tc.xy * consts.xy;
    O.fog = saturate(calc_fogging(world));
    return O;
}
