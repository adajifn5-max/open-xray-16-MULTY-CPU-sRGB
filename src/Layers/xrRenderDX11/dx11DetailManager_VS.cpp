#include "stdafx.h"
#include "Layers/xrRender/DetailManager.h"
#include "xrEngine/IGame_Persistent.h"
#include "xrEngine/Environment.h"
#include "Layers/xrRender/BufferUtils.h"
#include "xrCore/Threading/TaskManager.hpp"
#include <atomic>

namespace xray::render::RENDER_NAMESPACE
{
namespace detail_manager
{
extern const int quant;
//extern const int c_hdr;
}

#include "dx11DetailWindCache.h"

void CDetailManager::hw_Load_Shaders()
{
    // Create shader to access constant storage
    ref_shader S;
    S.create("details\\set");
    R_constant_table& T0 = *(S->E[0]->passes[0]->constants);
    R_constant_table& T1 = *(S->E[1]->passes[0]->constants);
    hwc_consts = T0.get("consts");
    hwc_wave = T0.get("wave");
    hwc_wind = T0.get("dir2D");
    hwc_array = T0.get("array");
    hwc_s_consts = T1.get("consts");
    hwc_s_xform = T1.get("xform");
    hwc_s_array = T1.get("array");
    if (!HW.DX10Only && HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        m_windCache = xr_new<WindCache>(objects);
}

void CDetailManager::hw_Render(CBackend& cmd_list, const Fsphere* bounds)
{
    ZoneScoped;
    using namespace detail_manager;

    // R4 cache is immutable for every pass of this frame. It was prepared on
    // the immediate context, before parallel shadow recording began.
    VERIFY(!m_windCache || m_windCache->frame == Device.dwFrame);
    if (m_windCache && m_windCache->diagnostics)
    {
        m_windCache->renderCalls.fetch_add(1, std::memory_order_relaxed);
        if (RImplementation.get_context(cmd_list.context_id).o.phase == CRender::PHASE_SMAP)
            m_windCache->shadowCalls.fetch_add(1, std::memory_order_relaxed);
    }

    // Оновлення вітрової анімації лише один раз на кадр.
    if (!m_windCache && m_windFrameNumber != Device.dwFrame)
    {
        m_windFrameNumber = Device.dwFrame;

        if (m_frameMode == FrameMode::Still)
        {
            // Не накопичуємо пропущений час.
            // Фази та напрямки не обчислюємо.
            m_global_time_old = Device.fTimeGlobal;
        }
        else
        {
            float fDelta = Device.fTimeGlobal - m_global_time_old;
            if (fDelta < 0.f || fDelta > 1.f)
                fDelta = 0.03f;

            m_global_time_old = Device.fTimeGlobal;
            m_time_pos += fDelta * swing_current.speed;

            if (m_frameMode == FrameMode::Wave1)
            {
                m_time_rot_1 += PI_MUL_2 * fDelta / swing_current.rot1;
                m_frameWind.set(_sin(m_time_rot_1), 0.f,
                                _cos(m_time_rot_1), 0.f)
                    .normalize().mul(swing_current.amp1);
                m_frameWave.set(1.f / 5.f, 1.f / 7.f,
                                1.f / 3.f, m_time_pos);
            }
            else // Wave2
            {
                m_time_rot_2 += PI_MUL_2 * fDelta / swing_current.rot2;
                m_frameWind.set(_sin(m_time_rot_2), 0.f,
                                _cos(m_time_rot_2), 0.f)
                    .normalize().mul(swing_current.amp2);
                m_frameWave.set(1.f / 3.f, 1.f / 7.f,
                                1.f / 5.f, m_time_pos);
            }

            m_frameWave.div(PI_MUL_2);
        }
    }

    cmd_list.set_Geometry(hw_Geom);
    const float scale = 1.f / float(quant);
    Fvector4 consts;

    if (m_frameMode == FrameMode::Still)
    {
        consts.set(scale, scale, scale, 1.f);
        Fvector4 noWind;
        noWind.set(0.f, 0.f, 0.f, 0.f);
        hw_Render_dump(cmd_list, consts, noWind, noWind, 0, 1, bounds);
        return;
    }

    consts.set(scale, scale,
               ps_r__Detail_l_aniso, ps_r__Detail_l_ambient);
    const u32 selectedMode = static_cast<u32>(m_frameMode);
    hw_Render_dump(cmd_list, consts, m_frameWave, m_frameWind,
                   selectedMode, 0, bounds);
}

void CDetailManager::hw_Render_dump(CBackend& cmd_list,
    const Fvector4& consts, const Fvector4& wave, const Fvector4& wind, u32 var_id, u32 lod_id, const Fsphere* bounds)
{
    ZoneScoped;

    static shared_str strConsts("consts");
    static shared_str strWave("wave");
    static shared_str strDir2D("dir2D");
    static shared_str strArray("array");
    static shared_str strXForm("xform");
    static shared_str strCacheInfo("detail_cache_info");
    const bool cached = m_windCache && var_id != 0;

    RImplementation.BasicStats.DetailCount = 0;

    // Matrices and offsets
    u32 vOffset = 0;
    u32 iOffset = 0;

    const auto& desc = g_pGamePersistent->Environment().CurrentEnv;
    Fvector c_sun, c_ambient, c_hemi;
    c_sun.set(desc.sun_color.x, desc.sun_color.y, desc.sun_color.z);
    c_sun.mul(.5f);
    c_ambient.set(desc.ambient.x, desc.ambient.y, desc.ambient.z);
    c_hemi.set(desc.hemi_color.x, desc.hemi_color.y, desc.hemi_color.z);

    // Iterate
    for (u32 O = 0; O < objects.size(); O++)
    {
        CDetail& Object = *objects[O];
        bool hasVisible = false;
        for (u32 group = 0; group < 3 && !hasVisible; ++group)
        {
            for (const VisibleItems& visible : m_visibles[group][O])
            {
                if (visible.intersects(bounds))
                {
                    hasVisible = true;
                    break;
                }
            }
        }
        if (hasVisible)
        {
            for (u32 iPass = 0; iPass < Object.shader->E[lod_id]->passes.size(); ++iPass)
            {
                // Setup matrices + colors (and flush it as necessary)
                // RCache.set_Element				(Object.shader->E[lod_id]);
                cmd_list.set_Element(Object.shader->E[lod_id], iPass);
                cmd_list.apply_lmaterial();

                //	This could be cached in the corresponding consatant buffer
                //	as it is done for DX9
                cmd_list.set_c(strConsts, consts);

                // Still (var_id == 0) не використовує вітрові константи.
                if (var_id != 0 && !cached)
                {
                    cmd_list.set_c(strWave, wave);
                    cmd_list.set_c(strDir2D, wind);
                }

               // cmd_list.set_c(strWave, wave);
               // cmd_list.set_c(strDir2D, wind);
                cmd_list.set_c(strXForm, Device.mFullTransform);
                if (cached)
                {
                    cmd_list.set_c(strCacheInfo, float(Object.number_vertices), float(vOffset), 0.f, 0.f);
                    cmd_list.SRVSManager.SetVSResource(3, m_windCache->models[O]->outputView);
                }

                // ref_constant constArray = RCache.get_c(strArray);
                // VERIFY(constArray);

                // u32			c_base				= x_array->vs.index;
                // Fvector4*	c_storage			= RCache.get_ConstantCache_Vertex().get_array_f().access(c_base);
                Fvector4* c_storage = 0;
                //	Map constants to memory directly
                {
                    void* pVData;
                    cmd_list.get_ConstantDirect(strArray, hw_BatchSize * sizeof(Fvector4) * 4, &pVData, 0, 0);
                    c_storage = (Fvector4*)pVData;
                }
                VERIFY(c_storage);

                u32 dwBatch = 0;

                for (u32 group = 0; group < 3; ++group)
                {
                    for (const VisibleItems& visible : m_visibles[group][O])
                    {
                        if (!visible.intersects(bounds))
                            continue;

                        for (SlotItem* item : *visible.items)
                        {
                            SlotItem& Instance = *item;
                            u32 base = dwBatch * 4;

                            // Still / DX10 fallback constructs the matrix here. Cached
                            // draws copy the frozen rows below, without rebuilding them.
                            if (!cached)
                            {
                                const float scale = Instance.scale_calculated;
                                const Fmatrix& M = Instance.mRotY;
                                c_storage[base + 0].set(M._11 * scale, M._21 * scale, M._31 * scale, M._41);
                                c_storage[base + 1].set(M._12 * scale, M._22 * scale, M._32 * scale, M._42);
                                c_storage[base + 2].set(M._13 * scale, M._23 * scale, M._33 * scale, M._43);
                            }
                            // RCache.set_ca(&*constArray, base+0, M._11*scale,	M._21*scale,	M._31*scale,	M._41	);
                            // RCache.set_ca(&*constArray, base+1, M._12*scale,	M._22*scale,	M._32*scale,	M._42	);
                            // RCache.set_ca(&*constArray, base+2, M._13*scale,	M._23*scale,	M._33*scale,	M._43	);

                            // Build color
                            // R2 only needs hemisphere
                            float h = Instance.c_hemi;
                            float s = Instance.c_sun;
                            c_storage[base + 3].set(s, s, s, h);
                            if (cached)
                            {
                                const auto* cachedEntry = m_windCache->models[O]->Find(item, O);
                                R_ASSERT2(cachedEntry, "Detail visibility changed after wind cache preparation");
                                const auto& entry = *cachedEntry;
                                // Freeze coordinate-affecting instance data for all passes.
                                for (u32 row = 0; row < 3; ++row)
                                    c_storage[base + row] = entry.rows[row];
                                c_storage[base + 3].y = float(item->wind_slot);
                            }
                            // RCache.set_ca(&*constArray, base+3, s,				s,				s,				h
                            // );
                            dwBatch++;
                            if (dwBatch == hw_BatchSize)
                            {
                                // flush
                                RImplementation.BasicStats.DetailCount += dwBatch;
                                u32 dwCNT_verts = dwBatch * Object.number_vertices;
                                u32 dwCNT_prims = (dwBatch * Object.number_indices) / 3;
                                // RCache.get_ConstantCache_Vertex().b_dirty				=	TRUE;
                                // RCache.get_ConstantCache_Vertex().get_array_f().dirty	(c_base,c_base+dwBatch*4);
                                cmd_list.Render(D3DPT_TRIANGLELIST, vOffset, 0, dwCNT_verts, iOffset, dwCNT_prims);
                                cmd_list.stat.r.s_details.add(dwCNT_verts);

                                // restart
                                dwBatch = 0;

                                //	Remap constants to memory directly (just in case anything goes wrong)
                                {
                                    void* pVData;
                                    cmd_list.get_ConstantDirect(strArray, hw_BatchSize * sizeof(Fvector4) * 4, &pVData, 0, 0);
                                    c_storage = (Fvector4*)pVData;
                                }
                                VERIFY(c_storage);
                            }
                        }
                    }
                }
                // flush if necessary
                if (dwBatch)
                {
                    RImplementation.BasicStats.DetailCount += dwBatch;
                    u32 dwCNT_verts = dwBatch * Object.number_vertices;
                    u32 dwCNT_prims = (dwBatch * Object.number_indices) / 3;
                    // RCache.get_ConstantCache_Vertex().b_dirty				=	TRUE;
                    // RCache.get_ConstantCache_Vertex().get_array_f().dirty	(c_base,c_base+dwBatch*4);
                    cmd_list.Render(D3DPT_TRIANGLELIST, vOffset, 0, dwCNT_verts, iOffset, dwCNT_prims);
                    cmd_list.stat.r.s_details.add(dwCNT_verts);
                }
            }
        }
        if (cached)
        {
            cmd_list.SRVSManager.SetVSResource(3, nullptr);
            cmd_list.SRVSManager.Apply(cmd_list.context_id);
        }
        vOffset += hw_BatchSize * Object.number_vertices;
        iOffset += hw_BatchSize * Object.number_indices;
    }
}
} // namespace xray::render::RENDER_NAMESPACE
