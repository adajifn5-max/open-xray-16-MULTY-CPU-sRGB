////////////////////////////////////////////////////////////////////////////
//	Module 		: script_binder.cpp
//	Created 	: 26.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script objects binder
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "ai_space.h"
#include "xrScriptEngine/script_engine.hpp"
#include "script_binder.h"
#include "xrServer_Objects_ALife.h"
#include "script_binder_object.h"
#include "script_binder_object_wrapper.h"
#include <typeinfo>
#include "script_game_object.h"
#include "GameObject.h"
#include "Level.h"

// comment next string when commiting
//#define DBG_DISABLE_SCRIPTS

CScriptBinder::CScriptBinder(CGameObject* owner)
{
    this->owner = owner;
    init();
}

CScriptBinder::~CScriptBinder() { VERIFY(!m_object); }
void CScriptBinder::init() { m_object = 0; }
void CScriptBinder::clear()
{
    try
    {
        xr_delete(m_object);
    }
    catch (...)
    {
        m_object = 0;
    }
    init();
}

void CScriptBinder::reinit()
{
    if (m_object)
    {
        try
        {
            m_object->reinit();
        }
        catch (...)
        {
            clear();
        }
    }
}

void CScriptBinder::reload(LPCSTR section)
{
#ifndef DBG_DISABLE_SCRIPTS
    VERIFY(!m_object);
    if (!pSettings->line_exist(section, "script_binding"))
        return;

    luabind::functor<void> lua_function;
    if (!GEnv.ScriptEngine->functor(pSettings->r_string(section, "script_binding"), lua_function))
    {
        GEnv.ScriptEngine->script_log(
            LuaMessageType::Error, "function %s is not loaded!", pSettings->r_string(section, "script_binding"));
        return;
    }

    try
    {
        lua_function(owner->lua_game_object());
    }
    catch (...)
    {
        clear();
        return;
    }

    if (m_object)
    {
        try
        {
            m_object->reload(section);
        }
        catch (...)
        {
            clear();
        }
    }
#endif
}

BOOL CScriptBinder::net_Spawn(CSE_Abstract* DC)
{
    CSE_Abstract* abstract = (CSE_Abstract*)DC;
    CSE_ALifeObject* object = smart_cast<CSE_ALifeObject*>(abstract);
    if (object && m_object)
    {
        try
        {
            return ((BOOL)m_object->net_Spawn(object));
        }
        catch (...)
        {
            clear();
        }
    }

    return (TRUE);
}

void CScriptBinder::net_Destroy()
{
    if (m_object)
    {
#ifdef _DEBUG
        Msg("* Core object %s is UNbinded from the script object", owner->cName().c_str());
#endif // _DEBUG
        try
        {
            m_object->net_Destroy();
        }
        catch (...)
        {
            clear();
        }
    }
    xr_delete(m_object);
}

void CScriptBinder::set_object(CScriptBinderObject* object)
{
    if (IsGameTypeSingle())
    {
        VERIFY2(!m_object, "Cannot bind to the object twice!");
#ifdef _DEBUG
        Msg("* Core object %s is binded with the script object", owner->cName().c_str());
#endif // _DEBUG
        m_object = object;
    }
    else
    {
        xr_delete(object);
    }
}

void CScriptBinder::shedule_Update(u32 time_delta)
{
    if (m_object)
    {
        try
        {
            m_object->shedule_Update(time_delta);
        }
        catch (...)
        {
            clear();
        }
    }
}

void CScriptBinder::save(NET_Packet& output_packet)
{
    if (m_object)
    {
        try
        {
            m_object->save(&output_packet);
        }
        catch (...)
        {
            clear();
        }
    }
}

void CScriptBinder::load(IReader& input_packet)
{
    if (m_object)
    {
        try
        {
            m_object->load(&input_packet);
        }
        catch (...)
        {
            clear();
        }
    }
}

BOOL CScriptBinder::net_SaveRelevant()
{
    if (m_object)
    {
        try
        {
            return (m_object->net_SaveRelevant());
        }
        catch (...)
        {
            clear();
        }
    }
    return (FALSE);
}

bool CScriptBinder::can_skip_net_relcase() const
{
    if (!m_object)
        return true;

    // Unknown C++ binders keep their virtual callbacks. Only the stock Lua
    // wrapper has a known empty native net_Relcase implementation.
    auto* wrapper = dynamic_cast<CScriptBinderObjectWrapper*>(m_object);
    if (!wrapper || typeid(*wrapper) != typeid(CScriptBinderObjectWrapper))
        return false;

    return wrapper->can_skip_net_relcase();
}

void CScriptBinder::net_Relcase(IGameObject* object)


    {
#ifdef DEBUG

    // Profile only smart_cover owners.
    if (owner && owner->cNameSect().equal("smart_terrain"))
    {
        struct SmartTerrainProfile
        {
            u32 frame;

            u32 calls;
            u32 noBinder;
            u32 castFail;
            u32 castOk;

            u32 luaObjectCalls;
            u32 callbackCalls;
            u32 exceptions;

            float totalMs;
            float smartCastMs;
            float luaGameObjectMs;
            float binderCallbackMs;
        };

        static SmartTerrainProfile p = {};

        // Print previous frame statistics.
        // Output is delayed until the next smart_cover call.
        if (p.frame != Device.dwFrame)
        {
            if (p.calls)
            {
                const float avgTotalUs =
                    p.totalMs * 1000.f / p.calls;

                const float avgCastUs =
                    (p.calls > p.noBinder)
                    ? p.smartCastMs * 1000.f / (p.calls - p.noBinder)
                    : 0.f;

                const float avgLuaUs =
                    p.luaObjectCalls
                    ? p.luaGameObjectMs * 1000.f / p.luaObjectCalls
                    : 0.f;

                const float avgCallbackUs =
                    p.callbackCalls
                    ? p.binderCallbackMs * 1000.f / p.callbackCalls
                    : 0.f;

                Msg(
                    "ST_BINDER_RELCASE frame=%u "
                    "calls=%u no_binder=%u cast_fail=%u cast_ok=%u "
                    "lua_object_calls=%u callback_calls=%u exceptions=%u | "
                    "total=%.3f ms avg=%.2f us "
                    "smart_cast=%.3f ms avg=%.2f us "
                    "lua_game_object=%.3f ms avg=%.2f us "
                    "binder_callback=%.3f ms avg=%.2f us",

                    p.frame,

                    p.calls,
                    p.noBinder,
                    p.castFail,
                    p.castOk,

                    p.luaObjectCalls,
                    p.callbackCalls,
                    p.exceptions,

                    p.totalMs,
                    avgTotalUs,

                    p.smartCastMs,
                    avgCastUs,

                    p.luaGameObjectMs,
                    avgLuaUs,

                    p.binderCallbackMs,
                    avgCallbackUs
                );
            }

            p = SmartTerrainProfile{};
            p.frame = Device.dwFrame;
        }

        CTimer tTotal;
        tTotal.Start();

        ++p.calls;

        // 1. Binder check.
        if (!m_object)
        {
            ++p.noBinder;

            p.totalMs += tTotal.GetElapsed_sec() * 1000.f;
            return;
        }

        // 2. smart_cast.
        CTimer tStage;
        tStage.Start();

        CGameObject* game_object =
            smart_cast<CGameObject*>(object);

        p.smartCastMs +=
            tStage.GetElapsed_sec() * 1000.f;

        if (!game_object)
        {
            ++p.castFail;

            p.totalMs += tTotal.GetElapsed_sec() * 1000.f;
            return;
        }

        ++p.castOk;

        // 3. lua_game_object + 4. Binder callback.
        bool measuringLuaObject = true;

        tStage.Start();

        try
        {
            ++p.luaObjectCalls;

            CScriptGameObject* luaObject =
                game_object->lua_game_object();

            p.luaGameObjectMs +=
                tStage.GetElapsed_sec() * 1000.f;

            measuringLuaObject = false;

            ++p.callbackCalls;

            tStage.Start();

            m_object->net_Relcase(luaObject);

            p.binderCallbackMs +=
                tStage.GetElapsed_sec() * 1000.f;
        }
        catch (...)
        {
            const float msException =
                tStage.GetElapsed_sec() * 1000.f;

            if (measuringLuaObject)
                p.luaGameObjectMs += msException;
            else
                p.binderCallbackMs += msException;

            ++p.exceptions;

            clear();
        }

        p.totalMs +=
            tTotal.GetElapsed_sec() * 1000.f;

        return;
    }

#endif // DEBUG

    // Normal path for all other objects.
    if (!m_object)
        return;

    CGameObject* game_object =
        smart_cast<CGameObject*>(object);

    if (!game_object)
        return;

    try
    {
        m_object->net_Relcase(
            game_object->lua_game_object()
        );
    }
    catch (...)
    {
        clear();
    }

    /* // No script binder: nothing to process.
    if (!m_object)
        return;

    CGameObject* game_object = smart_cast<CGameObject*>(object);

    if (!game_object)
        return;

    try
    {
        m_object->net_Relcase(game_object->lua_game_object());
    }
    catch (...)
    {
        clear();

    /*CGameObject* game_object = smart_cast<CGameObject*>(object);
    if (m_object && game_object)
    {
        try
        {
            m_object->net_Relcase(game_object->lua_game_object());
        }
        catch (...)
        {
            clear();
        }*/
    }

