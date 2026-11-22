#include "stdafx.h"
#include "xrCore/StutterProfile.h"
#pragma hdrstop

#include <DirectXTex.h>
#include "dx11TextureMipSelection.h"
#include "dx11ColorSpace.h"

namespace xray::render::RENDER_NAMESPACE
{
void fix_texture_name(pstr fn)
{
    pstr _ext = strext(fn);
    if (_ext && (!xr_stricmp(_ext, ".tga") || !xr_stricmp(_ext, ".dds") || !xr_stricmp(_ext, ".bmp") ||
        !xr_stricmp(_ext, ".ogm")))
    {
        *_ext = 0;
    }
}

int get_texture_load_lod(LPCSTR fn)
{
    CInifile::Sect& sect = pSettings->r_section("reduce_lod_texture_list");
    auto it_ = sect.Data.cbegin();
    auto it_e_ = sect.Data.cend();

    ENGINE_API bool is_enough_address_space_available();
    static bool enough_address_space_available = is_enough_address_space_available();

    auto it = it_;
    auto it_e = it_e_;

    for (; it != it_e; ++it)
    {
        if (strstr(fn, it->first.c_str()))
        {
            if (psTextureLOD < 1)
            {
                if (enough_address_space_available)
                    return 0;
                else
                    return 1;
            }
            else if (psTextureLOD < 3)
                return 1;
            else
                return 2;
        }
    }

    if (psTextureLOD < 2)
    {
        //if (enough_address_space_available)
        return 0;
        //else
        //    return 1;
    }
    else if (psTextureLOD < 4)
        return 1;
    else
        return 2;
}

u32 calc_texture_size(int lod, u32 mip_cnt, size_t orig_size)
{
    if (1 == mip_cnt)
        return orig_size;

    int _lod = lod;
    float res = float(orig_size);

    while (_lod > 0)
    {
        --_lod;
        res -= res / 1.333f;
    }
    return iFloor(res);
}

//////////////////////////////////////////////////////////////////////
// Utility pack
//////////////////////////////////////////////////////////////////////

IC void Reduce(size_t& w, size_t& h, size_t& l, int skip)
{
    while ((l > 1) && skip)
    {
        w /= 2;
        h /= 2;
        l -= 1;

        skip--;
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
}

ID3DBaseTexture* CRender::texture_load(LPCSTR fRName, u32& ret_msize)
{
    XR_STUTTER_DISABLED_SCOPE(Phase, "render.dds_load", fRName);
    ret_msize = 0;
    R_ASSERT1_CURE(fRName && fRName[0], { return nullptr; });

    ID3DBaseTexture* pTexture2D{};
    string_path fn;
    {
        // make file name
        string_path fname;
        xr_strcpy(fname, fRName);
        fix_texture_name(fname);

        // Call to FS.exist WRITES to fn !

        if (!FS.exist(fn, "$game_textures$", fname, ".dds") && strstr(fname, "_bump"))
        {
            Msg("! Fallback to default bump map: %s", fname);
            if (strstr(fname, "_bump#"))
                R_ASSERT1_CURE(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_bump#", ".dds"), return nullptr);
            else
                R_ASSERT1_CURE(FS.exist(fn, "$game_textures$", "ed\\ed_dummy_bump", ".dds"), return nullptr);
        }
        else
        {
            bool exist = false;

            for (cpcstr folder : { "$level$", "$game_saves$", "$game_textures$" })
            {
                exist = FS.exist(fn, folder, fname, ".dds");
                if (exist)
                    break;
            }

            if (!exist)
            {
                Msg("! Can't find texture '%s'", fname);
                if (!FS.exist(fn, "$game_textures$", "ed\\ed_not_existing_texture", ".dds"))
                    return nullptr;
            }
        }
    }

    // Load and get header
    IReader* S = [&]()
    {
        XR_STUTTER_DISABLED_SCOPE(Phase, "render.dds_open", fn);
        return FS.r_open(fn);
    }();
    R_ASSERT3_CURE(S, "Can't open texture", fn, { return nullptr; });

    size_t img_size = S->length();
#ifdef DEBUG
    Msg("* Loaded: %s[%zu]", fn, img_size);
#endif // DEBUG

    DirectX::DDS_FLAGS dds_flags{ DirectX::DDS_FLAGS_PERMISSIVE };

    for (int i = 1; i <= 3; ++i) // 3 attempts
    {
        DirectX::TexMetadata IMG;
        DirectX::ScratchImage texture;
        auto hresult = [&]()
        {
            XR_STUTTER_DISABLED_SCOPE(Phase, "render.dds_parse", fRName, i, dds_flags);
            return LoadFromDDSMemory(static_cast<u8*>(S->pointer()), img_size, dds_flags, &IMG, texture);
        }();

        R_ASSERT3_CURE(SUCCEEDED(hresult), "Failed to load texture from memory", fn,
        {
            FS.r_close(S);
            return nullptr;
        });

        // === DDS DIAGNOSTICS: BEFORE LOD REDUCTION ===
Msg("[DDS] %s | format=%u | %zux%zu | mips=%zu | sRGB=%d",
    fn,
    static_cast<unsigned>(IMG.format),
    IMG.width,
    IMG.height,
    IMG.mipLevels,
    DirectX::IsSRGB(IMG.format) ? 1 : 0);
// ============================================
        // Check for LMAP and compress if needed
        xr_strlwr(fn);

        const int img_loaded_lod = get_texture_load_lod(fn);

        // These packed colour formats have no hardware sRGB view.
        if (RImplementation.o.linear_rendering &&
            (IMG.format == DXGI_FORMAT_B5G6R5_UNORM || IMG.format == DXGI_FORMAT_B5G5R5A1_UNORM ||
             IMG.format == DXGI_FORMAT_B4G4R4A4_UNORM))
        {
            DirectX::ScratchImage converted;
            hresult = DirectX::Convert(texture.GetImages(), texture.GetImageCount(), IMG,
                DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.f, converted);
            R_ASSERT3_CURE(SUCCEEDED(hresult), "Failed to expand packed colour texture", fn,
            {
                FS.r_close(S);
                return nullptr;
            });
            texture = std::move(converted);
            IMG = texture.GetMetadata();
        }
        std::vector<DirectX::Image> selectedImages;
        size_t selectedBytes = 0;
        const size_t mip_lod = texture_loading::SelectMips(texture,
            static_cast<size_t>(std::max(img_loaded_lod, 0)), IMG, selectedImages, selectedBytes);
        R_ASSERT3_CURE(!selectedImages.empty(), "Invalid texture mip layout", fn,
        {
            FS.r_close(S);
            return nullptr;
        });

        Msg("[DDS_LOD] %s | requestedLOD=%d | skippedMips=%zu | GPU=%zux%zu | remainingMips=%zu",
    fn,
    img_loaded_lod,
    mip_lod,
    IMG.width,
    IMG.height,
    IMG.mipLevels);

        // DirectX requires compressed texture size to be
        // a multiple of 4. Make sure to meet this requirement.
        if (DirectX::IsCompressed(IMG.format))
        {
            IMG.width = (IMG.width + 3u) & ~0x3u;
            IMG.height = (IMG.height + 3u) & ~0x3u;
        }

        if (RImplementation.o.linear_rendering)
        {
            IMG.format = texture_color::ResourceFormat(IMG.format);
            for (auto& image : selectedImages)
                image.format = IMG.format; // Identical storage; DirectXTex validates matching descriptors.
        }

        hresult = [&]()
        {
            XR_STUTTER_DISABLED_SCOPE(Phase, "render.dds_gpu", fRName, i, dds_flags);
            return CreateTextureEx(HW.pDevice, selectedImages.data(), selectedImages.size(), IMG,
            D3D_USAGE_IMMUTABLE, D3D_BIND_SHADER_RESOURCE, 0, IMG.miscFlags, DirectX::CREATETEX_DEFAULT,
            &pTexture2D);
        }();

        if (SUCCEEDED(hresult))
        {
            // OK
            ret_msize = static_cast<u32>(std::min(selectedBytes, size_t(0xffffffffu)));
            break;
        }

        if (i == 1)
            dds_flags |= DirectX::DDS_FLAGS::DDS_FLAGS_NO_16BPP; // System isn't WDDM 1.2 compliant
        else if (i == 2)
            dds_flags |= DirectX::DDS_FLAGS::DDS_FLAGS_FORCE_RGB; // Not even WDDM 1.1 compliant
        else if (i == 3)
            Msg("! Could not load texture [%s] after %d attempts", fn, i);
        else
        {
            NODEFAULT;
        }
    }

    FS.r_close(S);
    return pTexture2D;
}
} // namespace xray::render::RENDER_NAMESPACE
