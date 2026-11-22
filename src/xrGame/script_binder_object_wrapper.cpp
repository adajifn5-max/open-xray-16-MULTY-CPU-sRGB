////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder_object_wrapper.cpp
//	Created 	: 29.03.2004
//  Modified 	: 29.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script object binder wrapper
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "xrCore/StutterProfile.h"
#include "script_binder_object_wrapper.h"
#include <luabind/function.hpp>
#include <luabind/wrapper_base.hpp>
#include "script_game_object.h"
#include "xrServer_Objects_ALife.h"

#ifdef DEBUG

namespace
{
    struct STWrapperRelcaseStats
    {
        u32 frame;
        u32 cachedBaseCalls;
        u32 luabindCalls;
        u32 fallbackCalls;
    };

    static STWrapperRelcaseStats g_stWrapperStats = {};

    static bool IsSmartTerrainOwner(CScriptGameObject* object)
    {
        return object &&
            object->object().cNameSect().equal("smart_terrain");
    }

    static void UpdateSTWrapperStatsFrame()
    {
        if (g_stWrapperStats.frame != Device.dwFrame)
        {
            if (g_stWrapperStats.cachedBaseCalls ||
                g_stWrapperStats.luabindCalls ||
                g_stWrapperStats.fallbackCalls)
            {
                Msg(
                    "ST_WRAPPER_RELCASE frame=%u "
                    "cached_base=%u luabind=%u fallback=%u "
                    "lua_override_est=%u",

                    g_stWrapperStats.frame,
                    g_stWrapperStats.cachedBaseCalls,
                    g_stWrapperStats.luabindCalls,
                    g_stWrapperStats.fallbackCalls,
                    g_stWrapperStats.luabindCalls -
                        g_stWrapperStats.fallbackCalls
                );
            }

            g_stWrapperStats = {};
            g_stWrapperStats.frame = Device.dwFrame;
        }
    }
}

#endif


CScriptBinderObjectWrapper::CScriptBinderObjectWrapper(CScriptGameObject* object)
    : CScriptBinderObject(object)
    , m_netRelcaseNoLuaOverride(false)
{
}

CScriptBinderObjectWrapper::~CScriptBinderObjectWrapper()
{
}

void CScriptBinderObjectWrapper::reinit()
{
    luabind::call_member<void>(this, "reinit");
}

void CScriptBinderObjectWrapper::reinit_static(
    CScriptBinderObject* script_binder_object)
{
    script_binder_object->CScriptBinderObject::reinit();
}


void CScriptBinderObjectWrapper::reload(LPCSTR section)
{
    luabind::call_member<void>(this, "reload", section);
}

void CScriptBinderObjectWrapper::reload_static(
    CScriptBinderObject* script_binder_object,
    LPCSTR section)
{
    script_binder_object->CScriptBinderObject::reload(section);
}


bool CScriptBinderObjectWrapper::net_Spawn(SpawnType DC)
{
    XR_STUTTER_NPC_SCOPE(Spawn, "script.net_Spawn", DC->s_name.c_str(), DC->ID);
    return luabind::call_member<bool>(this, "net_spawn", DC);
}

bool CScriptBinderObjectWrapper::net_Spawn_static(
    CScriptBinderObject* script_binder_object,
    SpawnType DC)
{
    return script_binder_object->CScriptBinderObject::net_Spawn(DC);
}


void CScriptBinderObjectWrapper::net_Destroy()
{
    luabind::call_member<void>(this, "net_destroy");
}

void CScriptBinderObjectWrapper::net_Destroy_static(
    CScriptBinderObject* script_binder_object)
{
    script_binder_object->CScriptBinderObject::net_Destroy();
}


void CScriptBinderObjectWrapper::net_Import(NET_Packet* net_packet)
{
    luabind::call_member<void>(this, "net_import", net_packet);
}

void CScriptBinderObjectWrapper::net_Import_static(
    CScriptBinderObject* script_binder_object,
    NET_Packet* net_packet)
{
    script_binder_object->CScriptBinderObject::net_Import(net_packet);
}


void CScriptBinderObjectWrapper::net_Export(NET_Packet* net_packet)
{
    luabind::call_member<void>(this, "net_export", net_packet);
}

void CScriptBinderObjectWrapper::net_Export_static(
    CScriptBinderObject* script_binder_object,
    NET_Packet* net_packet)
{
    script_binder_object->CScriptBinderObject::net_Export(net_packet);
}


void CScriptBinderObjectWrapper::shedule_Update(u32 time_delta)
{
    luabind::call_member<void>(this, "update", time_delta);
}

void CScriptBinderObjectWrapper::shedule_Update_static(
    CScriptBinderObject* script_binder_object,
    u32 time_delta)
{
    script_binder_object->CScriptBinderObject::shedule_Update(time_delta);
}


void CScriptBinderObjectWrapper::save(NET_Packet* output_packet)
{
    luabind::call_member<void>(this, "save", output_packet);
}

void CScriptBinderObjectWrapper::save_static(
    CScriptBinderObject* script_binder_object,
    NET_Packet* output_packet)
{
    script_binder_object->CScriptBinderObject::save(output_packet);
}


void CScriptBinderObjectWrapper::load(IReader* input_packet)
{
    luabind::call_member<void>(this, "load", input_packet);
}

void CScriptBinderObjectWrapper::load_static(
    CScriptBinderObject* script_binder_object,
    IReader* input_packet)
{
    script_binder_object->CScriptBinderObject::load(input_packet);
}


bool CScriptBinderObjectWrapper::net_SaveRelevant()
{
    return luabind::call_member<bool>(this, "net_save_relevant");
}

bool CScriptBinderObjectWrapper::net_SaveRelevant_static(
    CScriptBinderObject* script_binder_object)
{
    return script_binder_object->CScriptBinderObject::net_SaveRelevant();
}


// Relcase

bool CScriptBinderObjectWrapper::can_skip_net_relcase()
{
    m_netRelcaseNoLuaOverride = false;
    const auto& self = luabind::detail::wrap_access::ref(*this);
    // bind_object uses adopt<2>(), which keeps this strong Lua reference.
    // Unbound/unknown wrappers remain on the normal path without touching a
    // potentially uninitialized weak_ref.
    if (!self.m_strong_ref.is_valid())
        return false;
    lua_State* state = self.m_strong_ref.state();
    if (!state)
        return false;

    struct StackRestore
    {
        lua_State* state;
        int top;
        ~StackRestore() { lua_settop(state, top); }
    } restore{ state, lua_gettop(state) };

    self.m_strong_ref.get(state);
    if (!lua_isuserdata(state, -1))
        return false;

    // Use precisely the member selection used by wrap_base::call. A Lua
    // override (including an inherited one) must keep receiving every event.
    luabind::detail::do_call_member_selection(state, "net_Relcase");
    const bool emptyBase = lua_isfunction(state, -1) &&
        luabind::detail::is_luabind_function(state, -1);
    m_netRelcaseNoLuaOverride = emptyBase;
    return emptyBase;
}

void CScriptBinderObjectWrapper::net_Relcase(CScriptGameObject* object)
{
    #ifdef DEBUG
    if (IsSmartTerrainOwner(m_object))
    {
        UpdateSTWrapperStatsFrame();

        if (m_netRelcaseNoLuaOverride)
            ++g_stWrapperStats.cachedBaseCalls;
        else
            ++g_stWrapperStats.luabindCalls;
    }
#endif


    // Lua override відсутній — безпосередній виклик базового методу.
    if (m_netRelcaseNoLuaOverride)
    {
        CScriptBinderObject::net_Relcase(object);
        return;
    }

    // Lua override ще не перевірений — використовуємо Luabind.
    luabind::call_member<void>(this, "net_Relcase", object);
}


void CScriptBinderObjectWrapper::net_Relcase_static(
    CScriptBinderObject* script_binder_object,
    CScriptGameObject* object)
{
    CScriptBinderObjectWrapper* wrapper =
        static_cast<CScriptBinderObjectWrapper*>(script_binder_object);

    #ifdef DEBUG
    if (IsSmartTerrainOwner(wrapper->m_object))
    {
        UpdateSTWrapperStatsFrame();
        ++g_stWrapperStats.fallbackCalls;
    }
#endif

    // An explicit base call from a real Lua override must not disable that
    // override on subsequent notifications. Cache only a verified empty base.
    wrapper->m_netRelcaseNoLuaOverride = wrapper->can_skip_net_relcase();

    // Базовий метод викликається тільки один раз.
    script_binder_object->CScriptBinderObject::net_Relcase(object);
}
