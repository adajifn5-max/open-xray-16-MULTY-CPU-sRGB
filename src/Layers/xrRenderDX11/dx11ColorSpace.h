#pragma once

#include <d3d11.h>
#include <DirectXTex.h>
#include <cstring>
#include <initializer_list>

namespace xray::render::texture_color
{
constexpr unsigned StageSRGB = 0x80000000u;

// The binding, rather than the filename, defines whether RGB is colour or data.
// Detail maps are modulation data with a neutral value of 0.5; alpha is always data.
inline bool IsColorBinding(const char* resource, const char* shader)
{
    if (shader && (std::strstr(shader, "distort") || std::strstr(shader, "shadow") ||
        std::strstr(shader, "particle_s-") || std::strstr(shader, "alphaonly") ||
        std::strstr(shader, "yuv2rgb")))
        return false;
    for (const auto* name : { "s_base", "s_base_hud", "s_env", "s_env0", "s_env1", "s_leaves", "s_dt_r", "s_dt_g", "s_dt_b", "s_dt_a",
        "env_s0", "env_s1", "sky_s0", "sky_s1", "s_sky0", "s_sky1", "s_clouds0", "s_clouds1" })
        if (std::strcmp(resource, name) == 0)
            return true;
    return false;
}

inline DXGI_FORMAT ResourceFormat(DXGI_FORMAT format)
{
    const auto linear = DirectX::MakeLinear(format);
    return DirectX::MakeSRGB(linear) != linear ? DirectX::MakeTypeless(linear) : format;
}

inline DXGI_FORMAT LinearViewFormat(DXGI_FORMAT format)
{
    return DirectX::MakeLinear(DirectX::MakeTypelessUNORM(format));
}

inline HRESULT CreateLinearView(ID3D11Device* device, ID3D11Resource* resource, ID3D11ShaderResourceView** view)
{
    D3D11_RESOURCE_DIMENSION dimension;
    resource->GetType(&dimension);
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        D3D11_TEXTURE2D_DESC desc;
        static_cast<ID3D11Texture2D*>(resource)->GetDesc(&desc);
        srv.Format = LinearViewFormat(desc.Format);
        if (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
        {
            srv.ViewDimension = desc.ArraySize > 6 ? D3D11_SRV_DIMENSION_TEXTURECUBEARRAY : D3D11_SRV_DIMENSION_TEXTURECUBE;
            if (desc.ArraySize > 6) { srv.TextureCubeArray.MipLevels = desc.MipLevels; srv.TextureCubeArray.NumCubes = desc.ArraySize / 6; }
            else srv.TextureCube.MipLevels = desc.MipLevels;
        }
        else if (desc.ArraySize > 1)
        {
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels = desc.MipLevels;
            srv.Texture2DArray.ArraySize = desc.ArraySize;
        }
        else { srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = desc.MipLevels; }
    }
    else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D)
    {
        D3D11_TEXTURE3D_DESC desc;
        static_cast<ID3D11Texture3D*>(resource)->GetDesc(&desc);
        srv.Format = LinearViewFormat(desc.Format);
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = desc.MipLevels;
    }
    else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
    {
        D3D11_TEXTURE1D_DESC desc;
        static_cast<ID3D11Texture1D*>(resource)->GetDesc(&desc);
        srv.Format = LinearViewFormat(desc.Format);
        if (desc.ArraySize > 1) { srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1DARRAY; srv.Texture1DArray.ArraySize = desc.ArraySize; srv.Texture1DArray.MipLevels = desc.MipLevels; }
        else { srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D; srv.Texture1D.MipLevels = desc.MipLevels; }
    }
    else return E_INVALIDARG;
    return device->CreateShaderResourceView(resource, &srv, view);
}
}
