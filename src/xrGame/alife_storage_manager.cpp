////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_storage_manager.cpp
//	Created 	: 25.12.2002
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife Simulator storage manager
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "alife_storage_manager.h"
#include "alife_simulator_header.h"
#include "alife_time_manager.h"
#include "alife_spawn_registry.h"
#include "alife_object_registry.h"
#include "alife_graph_registry.h"
#include "alife_group_registry.h"
#include "alife_registry_container.h"
#include "xrServer.h"
#include "Level.h"
#include "saved_game_wrapper.h"
#include "xrEngine/IGame_Persistent.h"
#include "autosave_manager.h"

using namespace ALife;

extern string_path g_last_saved_game;

extern ENGINE_API bool bShowPauseString;

namespace
{
class SavePauseController final : public pureFrame
{
    using Clock = std::chrono::steady_clock;

    bool m_ownsPause = false;
    bool m_registered = false;
    bool m_previousPauseText = false;
    unsigned m_saveDepth = 0;
    Clock::time_point m_resumeAt{};

public:
    void Begin()
    {
        ++m_saveDepth;

        // Nested saves must not start separate pause periods.
        if (m_saveDepth != 1 || m_ownsPause)
            return;

        // Preserve an existing user/menu pause.
        if (Device.Paused())
            return;

        if (MainMenu() && MainMenu()->IsActive())
            return;

        const bool oldPauseText = bShowPauseString;

        // Pause simulation only; do not pause audio emitters.
        Device.Pause(true, true, false, "alife_save");

        if (!Device.Paused())
            return;

        m_previousPauseText = oldPauseText;
        m_ownsPause = true;

        // Keep existing save UI; suppress the extra PAUSE label.
        bShowPauseString = false;
    }

    void Finish()
    {
        if (m_saveDepth == 0)
            return;

        --m_saveDepth;

        if (m_saveDepth != 0 || !m_ownsPause)
            return;

        // Full second begins after synchronous save completes.
        m_resumeAt = Clock::now() + std::chrono::seconds(2);

        if (!m_registered)
        {
            m_registered = true;
            Device.seqFrame.Add(this, REG_PRIORITY_LOW);
        }
    }

    void OnFrame() override
    {
        if (!m_registered || m_saveDepth != 0)
            return;

        if (Clock::now() < m_resumeAt)
            return;

        Stop();
    }

    void Stop()
    {
        if (m_registered)
        {
            m_registered = false;
            Device.seqFrame.Remove(this);
        }

        if (m_ownsPause)
        {
            m_ownsPause = false;

            // Do not unpause a menu opened in the meantime.
            const bool menuActive =
                MainMenu() && MainMenu()->IsActive();

            if (Device.Paused() && !menuActive)
                Device.Pause(false, true, false, "alife_save");

            bShowPauseString = m_previousPauseText;
        }

        m_saveDepth = 0;
    }
};

SavePauseController g_savePause;

class SavePauseScope final
{
public:
    SavePauseScope()
    {
        g_savePause.Begin();
    }

    ~SavePauseScope()
    {
        g_savePause.Finish();
    }

    SavePauseScope(const SavePauseScope&) = delete;
    SavePauseScope& operator=(const SavePauseScope&) = delete;
};
}


CALifeStorageManager::~CALifeStorageManager() //{}

{
    g_savePause.Stop();
}

void CALifeStorageManager::save(LPCSTR save_name_no_check, bool update_name)
{
    SavePauseScope savePause;

    pcstr gameSaveExtension = SAVE_EXTENSION;
    if (ShadowOfChernobylMode || ClearSkyMode)
        gameSaveExtension = SAVE_EXTENSION_LEGACY;

    LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

    string_path save_name;
    strncpy_s(save_name, sizeof(save_name), save_name_no_check,
        sizeof(save_name) - 5 - xr_strlen(gameSaveExtension) - xr_strlen(game_saves_path));

    xr_strcpy(g_last_saved_game, save_name);

    string_path saveBackup;
    xr_strcpy(saveBackup, m_save_name);
    if (save_name[0])
    {
        strconcat(sizeof(m_save_name), m_save_name, save_name, gameSaveExtension);
    }
    else
    {
        if (!xr_strlen(m_save_name))
        {
            Log("There is no file name specified!");
            return;
        }
    }

	//Alundaio: To get the savegame fname to make our own custom save states
    luabind::functor<void> funct1;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_before_save", funct1))
        funct1((pcstr)m_save_name);
	//-Alundaio

    u32 source_count;
    u32 dest_count;
    void* dest_data;
    {
        CMemoryWriter stream;
        header().save(stream);
        time_manager().save(stream);
        spawns().save(stream);
        objects().save(stream);
        registry().save(stream);

        source_count = stream.tell();
        void* source_data = stream.pointer();
        dest_count = rtc_csize(source_count);
        dest_data = xr_malloc(dest_count);
        dest_count = rtc_compress(dest_data, dest_count, source_data, source_count);
    }

    string_path temp;
    FS.update_path(temp, "$game_saves$", m_save_name);
    IWriter* writer = FS.w_open(temp);
    if (!writer || !writer->valid())
    {
        Msg("! Cannot open savegame file '%s'", temp);
        if (writer)
            FS.w_close(writer);
        xr_free(dest_data);
        if (!update_name)
            xr_strcpy(m_save_name, saveBackup);
        return;
    }
    writer->w_u32(u32(-1));
    writer->w_u32(ALIFE_VERSION);

    writer->w_u32(source_count);
    writer->w(dest_data, dest_count);
    xr_free(dest_data);
    FS.w_close(writer);
#ifdef DEBUG
    Msg("* Game %s is successfully saved to file '%s' (%d bytes compressed to %d)", m_save_name, temp, source_count,
        dest_count + 4);
#else // DEBUG
    Msg("* Game %s is successfully saved to file '%s'", m_save_name, temp);
#endif // DEBUG

	//Alundaio: To get the savegame fname to make our own custom save states
    luabind::functor<void> funct2;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_save", funct2))
        funct2((pcstr)m_save_name);
    //-Alundaio

    if (!update_name)
        xr_strcpy(m_save_name, saveBackup);
}

void CALifeStorageManager::load(void* buffer, const u32& buffer_size, LPCSTR file_name)
{
    // The file name we were called with comes from $game_saves$, whose path is spelled
    // with backslashes in fsgame.ltx, so it cannot be used as is by scripts that open the
    // file directly. On POSIX the open fails, and CoC's *.scoc save state is then skipped
    // silently, leaving task_manager with no tasks, which it reports as all completed.
    // See issue #1988
    string_path script_file_name;
    xr_strcpy(script_file_name, file_name);
    convert_path_separators(script_file_name);

    //Alundaio: So we can get the fname to make our own custom save states
    luabind::functor<void> funct;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_load", funct))
        funct(script_file_name);
    //-Alundaio

    IReader source(buffer, buffer_size);
    header().load(source);
    time_manager().load(source);
    spawns().load(source, file_name);
    graph().on_load();
    objects().load(source);

    VERIFY(can_register_objects());
    can_register_objects(false);

    for (auto& object : objects().objects())
    {
        ALife::_OBJECT_ID id = object.second->ID;
        object.second->ID = server().PerformIDgen(id);
        VERIFY(id == object.second->ID);
        register_object(object.second, false);
    }

    registry().load(source);

    can_register_objects(true);

    for (auto& object : objects().objects())
        object.second->on_register();

    if (!g_pGameLevel)
        return;

    Level().autosave_manager().on_game_loaded();

    //Neloreck: For consistency with before/after save callbacks.
    luabind::functor<void> funct2;
    if (GEnv.ScriptEngine->functor("alife_storage_manager.CALifeStorageManager_after_load", funct2))
        funct2(script_file_name);
    //-Neloreck
}

bool CALifeStorageManager::load(LPCSTR save_name_no_check)
{
    pcstr gameSaveExtension = SAVE_EXTENSION;
    if (ShadowOfChernobylMode || ClearSkyMode)
        gameSaveExtension = SAVE_EXTENSION_LEGACY;

    LPCSTR game_saves_path = FS.get_path("$game_saves$")->m_Path;

    string_path save_name;
    strncpy_s(save_name, sizeof(save_name), save_name_no_check,
        sizeof(save_name) - 5 - xr_strlen(gameSaveExtension) - xr_strlen(game_saves_path));

    CTimer timer;
    timer.Start();

    string_path saveBackup;
    xr_strcpy(saveBackup, m_save_name);
    if (!save_name[0])
    {
        if (!xr_strlen(m_save_name))
        {
            Log("There is no file name specified!");
            return false;
        }
    }
    else
    {
        strconcat(sizeof(m_save_name), m_save_name, save_name, gameSaveExtension);
    }
    string_path file_name;
    FS.update_path(file_name, "$game_saves$", m_save_name);

    xr_strcpy(g_last_saved_game, save_name);
    xrDebug::SetBugReportFile(file_name);

    IReader* stream = FS.r_open(file_name);
    if (!stream)
    {
        Msg("* Cannot open saved game %s", file_name);
        xr_strcpy(m_save_name, saveBackup);
        return false;
    }

    constexpr pcstr mismatch = "Saved game version mismatch or saved game is corrupted";
    const bool gameSaveIsValid = CSavedGameWrapper::valid_saved_game(*stream);
    VERIFY3(gameSaveIsValid, mismatch, file_name);

    if (!gameSaveIsValid)
    {
        Msg("! %s [%s]", mismatch, file_name);

        xr_strcpy(m_save_name, saveBackup);
        return false;
    }

    string512 temp;
    strconcat(temp, StringTable().translate("st_loading_saved_game").c_str(),
        " \"", save_name, gameSaveExtension, "\"");

    g_pGamePersistent->LoadTitle(temp);

    unload();
    reload(m_section);

    const u32 source_count = stream->r_u32();
    void* source_data = xr_malloc(source_count);
    rtc_decompress(source_data, source_count, stream->pointer(), stream->length() - 3 * sizeof(u32));
    FS.r_close(stream);
    load(source_data, source_count, file_name);
    xr_free(source_data);

    groups().on_after_game_load();

    VERIFY(graph().actor());

    Msg("* Game %s is successfully loaded from file '%s' (%.3fs)", save_name, file_name, timer.GetElapsed_sec());

    return (true);
}

void CALifeStorageManager::save(NET_Packet& net_packet)
{
    SavePauseScope savePause;
    prepare_objects_for_save();

    shared_str game_name;
    net_packet.r_stringZ(game_name);
    save(game_name.c_str(), !!net_packet.r_u8());
}

void CALifeStorageManager::prepare_objects_for_save()
{
    Level().ClientSend();
    Level().ClientSave();
}
