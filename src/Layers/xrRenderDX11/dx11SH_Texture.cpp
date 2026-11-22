#include "stdafx.h"
#include "xrCore/StutterProfile.h"
#pragma hdrstop

#include "Layers/xrRender/ResourceManager.h"
#include "dx11ColorSpace.h"
#include <d3d11sdklayers.h> //перевірка
#include "xrEngine/tntQAVI.h"
#include "xrEngine/xrTheora_Surface.h"
#include "StateManager/dx11ShaderResourceStateCache.h"

#define PRIORITY_HIGH 12
#define PRIORITY_NORMAL 8
#define PRIORITY_LOW 4

namespace xray::render::RENDER_NAMESPACE
{
void resptrcode_texture::create(LPCSTR _name) { _set(RImplementation.Resources->_CreateTexture(_name)); }
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////
CTexture::CTexture()
{
    pSurface = NULL;
    m_pSRView = NULL;
    pAVI = NULL;
    pTheora = NULL;
    desc_cache = 0;
    seqMSPF = 0;
    flags.MemoryUsage = 0;
    flags.bLoaded = false;
    flags.bUser = false;
    flags.seqCycles = FALSE;
    m_material = 1.0f;
    bind = fastdelegate::FastDelegate2<CBackend&,u32>(this, &CTexture::apply_load);
}

CTexture::~CTexture()
{
    Unload();

    // release external reference
    RImplementation.Resources->_DeleteTexture(this);
}

void CTexture::surface_set(ID3DBaseTexture* surf)
{
#if 0//def DEBUG
    string_path msg_buff;
    xr_sprintf(msg_buff, sizeof(msg_buff), "* Changing texture [%s] current pSurface RefCount =", cName.c_str());
    _SHOW_REF(msg_buff, pSurface);
#endif // DEBUG

    if (surf)
        surf->AddRef();
    _RELEASE(pSurface);

    pSurface = surf;

    if (pSurface)
    {
        desc_update();

        D3D_RESOURCE_DIMENSION type;
        pSurface->GetType(&type);
        if (D3D_RESOURCE_DIMENSION_TEXTURE2D == type)
        {
            D3D_SHADER_RESOURCE_VIEW_DESC ViewDesc{};

            if (desc.MiscFlags & D3D_RESOURCE_MISC_TEXTURECUBE)
            {
                ViewDesc.ViewDimension = D3D_SRV_DIMENSION_TEXTURECUBE;
                ViewDesc.TextureCube.MostDetailedMip = 0;
                ViewDesc.TextureCube.MipLevels = desc.MipLevels;
            }
            else
            {
                const bool isArray = desc.ArraySize > 1;
                if (desc.SampleDesc.Count <= 1)
                {
                    ViewDesc.ViewDimension = isArray ? D3D_SRV_DIMENSION_TEXTURE2DARRAY : D3D_SRV_DIMENSION_TEXTURE2D;
                    if (isArray)
                    {
                        ViewDesc.Texture2DArray.MipLevels = desc.MipLevels;
                        ViewDesc.Texture2DArray.ArraySize = desc.ArraySize;
                    }
                    else
                    {
                        ViewDesc.Texture2D.MipLevels = desc.MipLevels;
                    }
                }
                else
                {
                    ViewDesc.ViewDimension = isArray ? D3D_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D_SRV_DIMENSION_TEXTURE2DMS;
                    if (isArray)
                    {
                        ViewDesc.Texture2DMSArray.ArraySize = desc.ArraySize;
                    }
                }
            }

            if (RImplementation.o.linear_rendering &&
                texture_color::ResourceFormat(texture_color::LinearViewFormat(desc.Format)) == desc.Format)
                ViewDesc.Format = texture_color::LinearViewFormat(desc.Format);

            switch (desc.Format)
            {
            case DXGI_FORMAT_R32G8X24_TYPELESS:
                ViewDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                break;

            case DXGI_FORMAT_R24G8_TYPELESS:
                ViewDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                break;

            case DXGI_FORMAT_R32_TYPELESS:
                ViewDesc.Format = DXGI_FORMAT_R32_FLOAT;
                break;

            case DXGI_FORMAT_R16_TYPELESS:
                ViewDesc.Format = DXGI_FORMAT_R16_FLOAT;
                break;
            }

   //_RELEASE(srv_all);

            /*// SRV diagnostic only: keep the original ViewDesc and texture formats unchanged.
            // The D3D11 debug layer, when enabled at device creation, supplies InfoQueue messages.
            ID3D11InfoQueue* pInfoQueue = nullptr;
            const HRESULT infoQueueHr = HW.pDevice->QueryInterface(
                __uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&pInfoQueue));
            const UINT64 firstMessage = pInfoQueue ? pInfoQueue->GetNumStoredMessages() : 0;

            const HRESULT srvHr = HW.pDevice->CreateShaderResourceView(
                pSurface, &ViewDesc, &srv_all);

            Msg("[SRV_CHECK] %s | resource=%u | requested=%u | samples=%u | hr=0x%08X | debug=%u",
                cName.c_str(),
                static_cast<unsigned>(desc.Format),
                static_cast<unsigned>(ViewDesc.Format),
                static_cast<unsigned>(desc.SampleDesc.Count),
                static_cast<unsigned>(srvHr),
                SUCCEEDED(infoQueueHr) && pInfoQueue ? 1u : 0u);

            if (SUCCEEDED(srvHr) && srv_all)
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC actualDesc{};
                srv_all->GetDesc(&actualDesc);
                Msg("[SRV_ACTUAL] %s | format=%u | dimension=%u",
                    cName.c_str(),
                    static_cast<unsigned>(actualDesc.Format),
                    static_cast<unsigned>(actualDesc.ViewDimension));
            }

            if (pInfoQueue)
            {
                const UINT64 messageCount = pInfoQueue->GetNumStoredMessages();
                for (UINT64 messageIndex = firstMessage; messageIndex < messageCount; ++messageIndex)
                {
                    SIZE_T messageSize = 0;
                    if (FAILED(pInfoQueue->GetMessage(messageIndex, nullptr, &messageSize)) || !messageSize)
                        continue;

                    xr_vector<u8> messageBuffer(messageSize);
                    D3D11_MESSAGE* message = reinterpret_cast<D3D11_MESSAGE*>(messageBuffer.data());
                    if (SUCCEEDED(pInfoQueue->GetMessage(messageIndex, message, &messageSize)) &&
                        (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING ||
                         message->Severity == D3D11_MESSAGE_SEVERITY_ERROR ||
                         message->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION))
                    {
                        Msg("[D3D11_DEBUG] %s | severity=%u | %s",
                            cName.c_str(),
                            static_cast<unsigned>(message->Severity),
                            message->pDescription);
                    }
                }
                pInfoQueue->Release();
            }
            */
            // Preserve the original OpenXRay error handling after diagnostics.
            // CHK_DX(srvHr); // Disabled: srvHr is declared only in the diagnostics block.
            _RELEASE(srv_all);
                CHK_DX(HW.pDevice->CreateShaderResourceView(
    pSurface, &ViewDesc, &srv_all));
           /* if (m_pSRView &&
    strstr(cName.c_str(), "wpn\\"))
{
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    m_pSRView->GetDesc(&srvDesc);

    Msg("[SRV] %s | format=%u | dimension=%u",
        cName.c_str(),
        static_cast<unsigned>(srvDesc.Format),
        static_cast<unsigned>(srvDesc.ViewDimension));
}*/
            srv_per_slice.resize(desc.ArraySize);
            for (u32 id = 0; id < desc.ArraySize; ++id)
            {
                _RELEASE(srv_per_slice[id]);

                if (desc.SampleDesc.Count <= 1)
                {
                    ViewDesc.Texture2DArray.ArraySize = 1;
                    ViewDesc.Texture2DArray.FirstArraySlice = id;
                }
                else
                {
                    ViewDesc.Texture2DMSArray.ArraySize = 1;
                    ViewDesc.Texture2DMSArray.FirstArraySlice = id;
                }
                CHK_DX(HW.pDevice->CreateShaderResourceView(pSurface, &ViewDesc, &srv_per_slice[id]));
            }
            set_slice(-1);
        }
        else
        {
            _RELEASE(m_pSRView);
            {
                XR_STUTTER_DISABLED_SCOPE(Phase, "render.texture_srv", cName.c_str());
                CHK_DX(RImplementation.o.linear_rendering
                    ? texture_color::CreateLinearView(HW.pDevice, pSurface, &m_pSRView)
                    : HW.pDevice->CreateShaderResourceView(pSurface, NULL, &m_pSRView));
            }
        }
    }
}

ID3DBaseTexture* CTexture::surface_get() const
{
    if (pSurface)
        pSurface->AddRef();
    return pSurface;
}

void CTexture::PostLoad()
{
    if (pTheora)
        bind = fastdelegate::FastDelegate2<CBackend&,u32>(this, &CTexture::apply_theora);
    else if (pAVI)
        bind = fastdelegate::FastDelegate2<CBackend&,u32>(this, &CTexture::apply_avi);
    else if (!seqDATA.empty())
        bind = fastdelegate::FastDelegate2<CBackend&,u32>(this, &CTexture::apply_seq);
    else
        bind = fastdelegate::FastDelegate2<CBackend&,u32>(this, &CTexture::apply_normal);
}

void CTexture::apply_load(CBackend& cmd_list, u32 dwStage)
{
    if (!flags.bLoaded)
        Load();
    else
        PostLoad();
    bind(cmd_list, dwStage);
};

ID3DShaderResourceView* CTexture::UIShaderView() const
{
    return RImplementation.o.linear_rendering ? ColorView() : m_pSRView;
}

ID3DShaderResourceView* CTexture::ColorView() const
{
    if (!m_pSRView)
        return nullptr;
    D3D_SHADER_RESOURCE_VIEW_DESC view;
    m_pSRView->GetDesc(&view);
    const auto colorFormat = DirectX::MakeSRGB(view.Format);
    if (colorFormat == view.Format)
        return m_pSRView; // Floating point HDR and data formats have no sRGB view.
    if (m_srgbSource != m_pSRView)
    {
        _RELEASE(m_srgbView);
        _RELEASE(m_srgbSource);
        m_srgbSource = m_pSRView;
        m_srgbSource->AddRef(); // Prevent stale pointer reuse for sequence/array views.
        ID3D11Resource* resource = nullptr;
        m_pSRView->GetResource(&resource);
        view.Format = colorFormat;
        const auto hr = HW.pDevice->CreateShaderResourceView(resource, &view, &m_srgbView);
        _RELEASE(resource);
        if (FAILED(hr))
            Msg("! sRGB view unavailable: %s, HRESULT=%08X", cName.c_str(), unsigned(hr));
    }
    return m_srgbView ? m_srgbView : m_pSRView;
}

void CTexture::Apply(CBackend& cmd_list, u32 dwStage) const
{
    const bool color = (dwStage & texture_color::StageSRGB) != 0;
    dwStage &= ~texture_color::StageSRGB;
    auto* resourceView = color ? ColorView() : m_pSRView;
    // if( !RImplementation.o.msaa )
    //   VERIFY( !((!pSurface)^(!m_pSRView)) );	//	Both present or both missing
    // else
    //{
    // if( ((!pSurface)^(!m_pSRView)) )
    //   return;
    //}

    if (dwStage < rstVertex) //	Pixel shader stage resources
    {
        cmd_list.SRVSManager.SetPSResource(dwStage, resourceView);
    }
    else if (dwStage < rstGeometry) //	Vertex shader stage resources
    {
        cmd_list.SRVSManager.SetVSResource(dwStage - rstVertex, resourceView);
    }
    else if (dwStage < rstHull) //	Geometry shader stage resources
    {
        cmd_list.SRVSManager.SetGSResource(dwStage - rstGeometry, resourceView);
    }
    else if (dwStage < rstDomain) //	Geometry shader stage resources
    {
        cmd_list.SRVSManager.SetHSResource(dwStage - rstHull, resourceView);
    }
    else if (dwStage < rstCompute) //	Geometry shader stage resources
    {
        cmd_list.SRVSManager.SetDSResource(dwStage - rstDomain, resourceView);
    }
    else if (dwStage < rstInvalid) //	Geometry shader stage resources
    {
        cmd_list.SRVSManager.SetCSResource(dwStage - rstCompute, resourceView);
    }
    else
        VERIFY("Invalid stage");
}

void CTexture::apply_theora(CBackend& cmd_list, u32 dwStage)
{
    if (pTheora->Update(m_play_time != 0xFFFFFFFF ? m_play_time : Device.dwTimeContinual))
    {
        D3D_RESOURCE_DIMENSION type;
        pSurface->GetType(&type);
        R_ASSERT(D3D_RESOURCE_DIMENSION_TEXTURE2D == type);
        ID3DTexture2D* T2D = (ID3DTexture2D*)pSurface;
        D3D_MAPPED_TEXTURE2D mapData;
        RECT rect;
        rect.left = 0;
        rect.top = 0;
        rect.right = pTheora->Width(true);
        rect.bottom = pTheora->Height(true);
        R_CHK(HW.get_context(cmd_list.context_id)->Map(T2D, 0, D3D_MAP_WRITE_DISCARD, 0, &mapData));
        u32 _w = mapData.RowPitch / 4;
        int _pos = 0;
        pTheora->DecompressFrame((u32*)mapData.pData, _w - rect.right, _pos);
        VERIFY(u32(_pos) == rect.bottom * _w);

        HW.get_context(cmd_list.context_id)->Unmap(T2D, 0);
    }
    Apply(cmd_list, dwStage);
}

void CTexture::apply_avi(CBackend& cmd_list, u32 dwStage) const
{
    // AVI
    if (pAVI->NeedUpdate())
    {
        D3D_RESOURCE_DIMENSION type;
        pSurface->GetType(&type);
        R_ASSERT(D3D_RESOURCE_DIMENSION_TEXTURE2D == type);
        ID3DTexture2D* T2D = (ID3DTexture2D*)pSurface;
        D3D_MAPPED_TEXTURE2D mapData;

        u8* ptr{};
        pAVI->GetFrame(&ptr);

        R_CHK(HW.get_context(CHW::IMM_CTX_ID)->Map(T2D, 0, D3D_MAP_WRITE_DISCARD, 0, &mapData));
        size_t rowSize = size_t(pAVI->m_dwWidth) * 4;
        if (mapData.RowPitch == rowSize)
            CopyMemory(mapData.pData, ptr, rowSize * pAVI->m_dwHeight);
        else
        {
            u8* destRow = static_cast<u8*>(mapData.pData);
            for (u32 row = 0; row < pAVI->m_dwHeight; ++row)
            {
                CopyMemory(destRow, ptr, rowSize);
                ptr += rowSize;
                destRow += mapData.RowPitch;
            }
        }
        HW.get_context(CHW::IMM_CTX_ID)->Unmap(T2D, 0);
    }
    Apply(cmd_list, dwStage);
}

void CTexture::apply_seq(CBackend& cmd_list, u32 dwStage)
{
    // SEQ
    u32 frame = Device.dwTimeContinual / seqMSPF; // Device.dwTimeGlobal
    u32 frame_data = seqDATA.size();
    if (flags.seqCycles)
    {
        u32 frame_id = frame % (frame_data * 2);
        if (frame_id >= frame_data)
            frame_id = (frame_data - 1) - (frame_id % frame_data);
        pSurface = seqDATA[frame_id];
        m_pSRView = m_seqSRView[frame_id];
    }
    else
    {
        u32 frame_id = frame % frame_data;
        pSurface = seqDATA[frame_id];
        m_pSRView = m_seqSRView[frame_id];
    }
    Apply(cmd_list, dwStage);
}

void CTexture::apply_normal(CBackend& cmd_list, u32 dwStage) const
{
    Apply(cmd_list, dwStage);
}

void CTexture::set_slice(int slice)
{
    m_pSRView = (slice < 0) ? srv_all : srv_per_slice[slice];
    curr_slice = slice;
}

void CTexture::Preload()
{
    m_bumpmap = RImplementation.Resources->m_textures_description.GetBumpName(cName);
    m_material = RImplementation.Resources->m_textures_description.GetMaterial(cName);
}

void CTexture::Load()
{
    flags.bLoaded = true;
    desc_cache = 0;
    // SEQ resources already exist before the first apply_seq selects pSurface.
    if (pSurface || !seqDATA.empty())
        return;

    XR_STUTTER_DISABLED_SCOPE(Phase, "render.texture_load", cName.c_str());
    flags.bUser = false;
    flags.MemoryUsage = 0;
    if (0 == xr_stricmp(cName.c_str(), "$null"))
        return;
    // we need to check only the beginning of the string,
    // so let's use strncmp instead of strstr.
    if (0 == strncmp(cName.c_str(), "$user$", sizeof("$user$") - 1))
    {
        flags.bUser = true;
        return;
    }

    ZoneScoped;

    Preload();

    bool bCreateView = true;

    // Check for OGM
    string_path fn;
    if (FS.exist(fn, "$game_textures$", cName.c_str(), ".ogm"))
    {
        // AVI
        pTheora = xr_new<CTheoraSurface>();
        m_play_time = 0xFFFFFFFF;

        if (!pTheora->Load(fn))
        {
            xr_delete(pTheora);
            FATAL("Can't open video stream");
        }
        else
        {
            flags.MemoryUsage = pTheora->Width(true) * pTheora->Height(true) * 4;
            pTheora->Play(TRUE, Device.dwTimeContinual);

            // Now create texture
            ID3DTexture2D* pTexture = 0;
            u32 _w = pTheora->Width(false);
            u32 _h = pTheora->Height(false);

            D3D_TEXTURE2D_DESC desc;
            desc.Width = _w;
            desc.Height = _h;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = RImplementation.o.linear_rendering ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D_USAGE_DYNAMIC;
            desc.BindFlags = D3D_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D_CPU_ACCESS_WRITE;
            desc.MiscFlags = 0;
            HRESULT hrr = HW.pDevice->CreateTexture2D(&desc, 0, &pTexture);

            pSurface = pTexture;
            if (FAILED(hrr))
            {
                FATAL("Invalid video stream");
                R_CHK(hrr);
                xr_delete(pTheora);
                pSurface = 0;
                m_pSRView = 0;
            }
            else
            {
                CHK_DX(RImplementation.o.linear_rendering ? texture_color::CreateLinearView(HW.pDevice, pSurface, &m_pSRView) : HW.pDevice->CreateShaderResourceView(pSurface, 0, &m_pSRView));
            }
        }
    }
    else if (FS.exist(fn, "$game_textures$", cName.c_str(), ".avi"))
    {
        // AVI
        pAVI = xr_new<CAviPlayerCustom>();

        if (!pAVI->Load(fn))
        {
            xr_delete(pAVI);
            FATAL("Can't open video stream");
        }
        else
        {
            flags.MemoryUsage = pAVI->m_dwWidth * pAVI->m_dwHeight * 4;

            // Now create texture
            ID3DTexture2D* pTexture = 0;
            D3D_TEXTURE2D_DESC desc;
            desc.Width = pAVI->m_dwWidth;
            desc.Height = pAVI->m_dwHeight;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = RImplementation.o.linear_rendering ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Usage = D3D_USAGE_DYNAMIC;
            desc.BindFlags = D3D_BIND_SHADER_RESOURCE;
            desc.CPUAccessFlags = D3D_CPU_ACCESS_WRITE;
            desc.MiscFlags = 0;
            HRESULT hrr = HW.pDevice->CreateTexture2D(&desc, 0, &pTexture);

            pSurface = pTexture;
            if (FAILED(hrr))
            {
                FATAL("Invalid video stream");
                R_CHK(hrr);
                xr_delete(pAVI);
                pSurface = 0;
                m_pSRView = 0;
            }
            else
            {
                CHK_DX(RImplementation.o.linear_rendering ? texture_color::CreateLinearView(HW.pDevice, pSurface, &m_pSRView) : HW.pDevice->CreateShaderResourceView(pSurface, 0, &m_pSRView));
            }
        }
    }
    else if (FS.exist(fn, "$game_textures$", cName.c_str(), ".seq"))
    {
        XR_STUTTER_DISABLED_SCOPE(Phase, "render.texture_seq", cName.c_str());
        // Sequence
        string256 buffer;
        IReader* _fs = FS.r_open(fn);

        flags.seqCycles = FALSE;
        _fs->r_string(buffer, sizeof(buffer));
        if (0 == xr_stricmp(buffer, "cycled"))
        {
            flags.seqCycles = TRUE;
            _fs->r_string(buffer, sizeof(buffer));
        }
        u32 fps = atoi(buffer);
        seqMSPF = 1000 / fps;

        while (!_fs->eof())
        {
            _fs->r_string(buffer, sizeof(buffer));
            _Trim(buffer);
            if (buffer[0])
            {
                XR_STUTTER_DISABLED_SCOPE(Phase, "render.seq_frame", buffer, seqDATA.size());
                // Load another texture
                u32 mem = 0;
                pSurface = RImplementation.texture_load(buffer, mem);
                if (pSurface)
                {
                    seqDATA.push_back(pSurface);
                    m_seqSRView.push_back(0);
                    {
                        XR_STUTTER_DISABLED_SCOPE(Phase, "render.texture_srv", buffer);
                        CHK_DX(RImplementation.o.linear_rendering
                            ? texture_color::CreateLinearView(HW.pDevice, seqDATA.back(), &m_seqSRView.back())
                            : HW.pDevice->CreateShaderResourceView(seqDATA.back(), NULL, &m_seqSRView.back()));
                    }
                    flags.MemoryUsage += mem;
                }
            }
        }
        pSurface = 0;
        FS.r_close(_fs);
    }
    else
    {
        // Normal texture
        u32 mem = 0;
        pSurface = RImplementation.texture_load(cName.c_str(), mem);

        // Calc memory usage and preload into vid-mem
        if (pSurface)
        {
            flags.MemoryUsage = mem;
            {
                XR_STUTTER_DISABLED_SCOPE(Phase, "render.texture_srv", cName.c_str());
                CHK_DX(RImplementation.o.linear_rendering
                    ? texture_color::CreateLinearView(HW.pDevice, pSurface, &m_pSRView)
                    : HW.pDevice->CreateShaderResourceView(pSurface, NULL, &m_pSRView));
            #if 0 // OLD SRV LOGGING DISABLED
                // === SRV DIAGNOSTICS ===
        Msg("[SRV_TRACE] %s | created=%d",
            cName.c_str(),
            m_pSRView ? 1 : 0);
        if (m_pSRView)
        {
            D3D_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            m_pSRView->GetDesc(&srvDesc);

            Msg("[SRV] %s | format=%u | dimension=%u",
                cName.c_str(),
                static_cast<unsigned>(srvDesc.Format),
                static_cast<unsigned>(srvDesc.ViewDimension));
        }
        // =======================
            #endif // OLD SRV LOGGING DISABLED
                }
        }
    }

#ifdef DEBUG
    if (pSurface)
    {
        pSurface->SetPrivateData(WKPDID_D3DDebugObjectName, cName.size(), cName.c_str());
    }
#endif

    PostLoad();
}

void CTexture::Unload()
{
    ZoneScoped;
#ifdef DEBUG
    string_path msg_buff;
    xr_sprintf(msg_buff, sizeof(msg_buff), "* Unloading texture [%s] pSurface RefCount =", cName.c_str());
    _SHOW_REF(msg_buff, pSurface);
#endif // DEBUG

    _RELEASE(m_srgbView);
    _RELEASE(m_srgbSource);
    flags.bLoaded = FALSE;
    if (!seqDATA.empty())
    {
        for (u32 I = 0; I < seqDATA.size(); I++)
        {
            if (m_pSRView == m_seqSRView[I] && m_seqSRView[I])
                m_pSRView = nullptr;
            _RELEASE(seqDATA[I]);
            _RELEASE(m_seqSRView[I]);
        }
        seqDATA.clear();
        m_seqSRView.clear();
        pSurface = 0;
    }

    _RELEASE(pSurface);
    if (m_pSRView == srv_all && srv_all)
        m_pSRView = nullptr;
    _RELEASE(srv_all);
    for (auto& srv : srv_per_slice)
    {
        if (m_pSRView == srv&& srv)
            m_pSRView = nullptr;
        _RELEASE(srv);
    }
    _RELEASE(m_pSRView);

    xr_delete(pAVI);
    xr_delete(pTheora);

    bind = fastdelegate::FastDelegate2<CBackend&,u32>(this, &CTexture::apply_load);
}

void CTexture::desc_update()
{
    desc_cache = pSurface;
    if (pSurface)
    {
        D3D_RESOURCE_DIMENSION type;
        pSurface->GetType(&type);
        if (D3D_RESOURCE_DIMENSION_TEXTURE2D == type)
        {
            ID3DTexture2D* T = (ID3DTexture2D*)pSurface;
            T->GetDesc(&desc);
            m_width = desc.Width;
            m_height = desc.Height;
        }
    }
}

D3D_USAGE CTexture::GetUsage()
{
    D3D_USAGE res = D3D_USAGE_DEFAULT;

    if (pSurface)
    {
        D3D_RESOURCE_DIMENSION type;
        pSurface->GetType(&type);
        switch (type)
        {
        case D3D_RESOURCE_DIMENSION_TEXTURE1D:
        {
            ID3DTexture1D* T = (ID3DTexture1D*)pSurface;
            D3D_TEXTURE1D_DESC descr;
            T->GetDesc(&descr);
            res = descr.Usage;
        }
        break;

        case D3D_RESOURCE_DIMENSION_TEXTURE2D:
        {
            ID3DTexture2D* T = (ID3DTexture2D*)pSurface;
            D3D_TEXTURE2D_DESC descr;
            T->GetDesc(&descr);
            res = descr.Usage;
        }
        break;

        case D3D_RESOURCE_DIMENSION_TEXTURE3D:
        {
            ID3DTexture3D* T = (ID3DTexture3D*)pSurface;
            D3D_TEXTURE3D_DESC descr;
            T->GetDesc(&descr);
            res = descr.Usage;
        }
        break;

        default: VERIFY(!"Unknown texture format???");
        }
    }

    return res;
}

void CTexture::video_Play(BOOL looped, u32 _time)
{
    if (pTheora)
        pTheora->Play(looped, (_time != 0xFFFFFFFF) ? (m_play_time = _time) : Device.dwTimeContinual);
}

void CTexture::video_Pause(BOOL state) const
{
    if (pTheora)
        pTheora->Pause(state);
}

void CTexture::video_Stop() const
{
    if (pTheora)
        pTheora->Stop();
}

BOOL CTexture::video_IsPlaying() const
{
    return (pTheora) ? pTheora->IsPlaying() : FALSE;
}
} // namespace xray::render::RENDER_NAMESPACE
