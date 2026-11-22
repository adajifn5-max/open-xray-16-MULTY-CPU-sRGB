////////////////////////////////////////////////////////////////////////////
//	Module 		: sound_player.cpp
//	Created 	: 27.12.2003
//  Modified 	: 27.12.2003
//	Author		: Dmitriy Iassenev
//	Description : Sound player
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "xrCore/StutterProfile.h"
#include "sound_player.h"
#include "xrScriptEngine/script_engine.hpp"
#include "ai/stalker/ai_stalker_space.h"
#include "ai_space.h"
#include "xrEngine/xr_object.h"
#include "xrCore/Animation/Bone.hpp"
#include "Include/xrRender/Kinematics.h"
#include "xrEngine/profiler.h"
#include "sound_collection_storage.h"
#include "xrSound/SoundCatalog.h"
#include "Common/object_broker.h"
#include "xrCore/SoundProfileLite.h"

CSoundPlayer::CSoundPlayer(IGameObject* object)
{
    VERIFY(object);
    m_object = object;
    seed(u32(CPU::QPC() & 0xffffffff));
    m_sound_prefix = "";
}

CSoundPlayer::~CSoundPlayer() { clear(); }
void CSoundPlayer::clear()
{
    m_pending_plays.clear();

    m_sounds.clear();

    xr_vector<CSoundSingle>::iterator I = m_playing_sounds.begin();
    xr_vector<CSoundSingle>::iterator E = m_playing_sounds.end();
    for (; I != E; ++I)
        (*I).destroy();

    m_playing_sounds.clear();

    m_sound_mask = 0;
}

CSoundPlayer::CSoundCollectionEntry::~CSoundCollectionEntry()
{
    xr_delete(collection);
}

void CSoundPlayer::reinit() {}
void CSoundPlayer::reload(LPCSTR section)
{
    VERIFY(m_playing_sounds.empty());
    clear();
    m_sound_prefix = "";
}

void CSoundPlayer::unload()
{
    m_pending_plays.clear();
    remove_active_sounds(u32(-1));
    VERIFY(m_playing_sounds.empty());
}

u32 CSoundPlayer::add(LPCSTR prefix, u32 max_count, ESoundTypes type, u32 priority, u32 mask, u32 internal_type,
    LPCSTR bone_name, CSound_UserDataPtr data)
{
    XR_STUTTER_DISABLED_SCOPE(Phase, "CSoundPlayer::add", prefix, internal_type);
    SoundProfileLite::Scope profiler(
    SoundProfileLite::PlayerAdd(),
    "CSoundPlayer::add",
    prefix,
    Device.dwFrame,
    1000 // журналювати виклики >= 1 мс
);
    SOUND_COLLECTIONS::iterator I = m_sounds.find(internal_type);
    if (m_sounds.end() != I)
       // return (0);

        {
    SoundProfileLite::PlayerEntries().Hit();
    return 0;
}

SoundProfileLite::PlayerEntries().Miss();

    CSoundCollectionParamsFull sound_params;
    // max_count = 0; //regression

    // Обмеження: 1 базовий звук + 54 числових варіанти
if (max_count > 54)
    max_count = 54;

    sound_params.m_priority = priority;
    sound_params.m_synchro_mask = mask;
    sound_params.m_bone_name = bone_name;
    sound_params.m_sound_prefix = prefix;
    sound_params.m_sound_player_prefix = m_sound_prefix;
    sound_params.m_max_count = max_count;
    sound_params.m_type = type;
    sound_params.m_data = data;

    /*typedef CSoundCollectionStorage::SOUND_COLLECTION_PAIR SOUND_COLLECTION_PAIR;
    const SOUND_COLLECTION_PAIR& pair = sound_collection_storage().object(sound_params);
    VERIFY(pair.first == (CSoundCollectionParams&)sound_params);
    VERIFY(pair.second);
    m_sounds.emplace(internal_type, std::make_pair(sound_params, pair.second));
 return (pair.second->m_sounds.size());*/

auto entry = sound_collection_storage().object(sound_params);

VERIFY(entry);
VERIFY(entry->params == (const CSoundCollectionParams&)sound_params);

m_sounds.emplace(
    internal_type,
    std::make_pair(sound_params, entry));

// Return the stable file count even while collection construction is queued.
// A queued collection already has a validated, stable variant list.
return static_cast<u32>(entry->files.size());

}

void CSoundPlayer::remove(u32 internal_type)
{
    m_pending_plays.erase(
    std::remove_if(
        m_pending_plays.begin(),
        m_pending_plays.end(),
        [internal_type](const SPendingPlay& request)
        {
            return request.internal_type == internal_type;
        }),
    m_pending_plays.end()
);
    
    
    SOUND_COLLECTIONS::iterator I = m_sounds.find(internal_type);
    VERIFY(m_sounds.end() != I);
    m_sounds.erase(I);
}

bool CSoundPlayer::check_sound_legacy(u32 internal_type) const
{
    SOUND_COLLECTIONS::const_iterator J = m_sounds.find(internal_type);
    if (m_sounds.end() == J)
    {
#ifdef DEBUG
        GEnv.ScriptEngine->script_log(LuaMessageType::Message,
            "Can't find sound with internal type %d (sound_script = %d)", internal_type,
            StalkerSpace::eStalkerSoundScript);
#endif
        return (false);
    }

    VERIFY(m_sounds.end() != J);
    const CSoundCollectionParamsFull& sound = (*J).second.first;
    if (sound.m_synchro_mask & m_sound_mask)
        return (false);

    xr_vector<CSoundSingle>::const_iterator I = m_playing_sounds.begin();
    xr_vector<CSoundSingle>::const_iterator E = m_playing_sounds.end();
    for (; I != E; ++I)
        if ((*I).m_synchro_mask & sound.m_synchro_mask)
            if ((*I).m_priority <= sound.m_priority)
                return (false);
    return (true);
}

void CSoundPlayer::update_pending_plays()
{
    constexpr u32 MAX_PENDING_TIME = 1000;

    const u32 now = Device.dwTimeGlobal;

    for (size_t i = 0; i < m_pending_plays.size();)
    {
        const SPendingPlay& request = m_pending_plays[i];

        // Час очікування вичерпано
        if (now - request.queued_at > MAX_PENDING_TIME)
        {
            m_pending_plays.erase(m_pending_plays.begin() + i);
            continue;
        }

        // Шукаємо колекцію
        auto it = m_sounds.find(request.internal_type);

        if (it == m_sounds.end())
        {
            m_pending_plays.erase(m_pending_plays.begin() + i);
            continue;
        }

        const auto& entry = it->second.second;

        // Колекція ще не готова
        if (!entry || !entry->ready())
        {
            ++i;
            continue;
        }

        // Перевіряємо готовність моделі NPC
        if (!m_object->Visual() ||
            !smart_cast<IKinematics*>(m_object->Visual()))
        {
            ++i;
            continue;
        }

        // Зберігаємо параметри перед видаленням
        const SPendingPlay ready_request = request;

        // Видаляємо запит до повторного play()
        m_pending_plays.erase(m_pending_plays.begin() + i);

        // Відтворюємо звук
        play(
            ready_request.internal_type,
            ready_request.max_start_time,
            ready_request.min_start_time,
            ready_request.max_stop_time,
            ready_request.min_stop_time,
            ready_request.id
        );
    }
}


/* Копіюємо параметри перед видаленням
        SPendingPlay ready_request = request;

        // Видаляємо запит із черги до виклику play()
        m_pending_plays.erase(m_pending_plays.begin() + i);

        // Повторюємо початковий виклик
        play(
            ready_request.internal_type,
            ready_request.max_start_time,
            ready_request.min_start_time,
            ready_request.max_stop_time,
            ready_request.min_stop_time,
            ready_request.id
        );
    }
}*/

/*void CSoundPlayer::update(float time_delta)
{
    START_PROFILE("Sound Player")
    remove_inappropriate_sounds(m_sound_mask);
    update_playing_sounds();
    STOP_PROFILE*/
void CSoundPlayer::update(float time_delta)
{
    START_PROFILE("Sound Player")

    remove_inappropriate_sounds(m_sound_mask);
    update_playing_sounds();

    // Обробляємо відкладені запити на звук
    update_pending_plays();

    STOP_PROFILE

}

void CSoundPlayer::remove_inappropriate_sounds(u32 sound_mask)
{
    m_playing_sounds.erase(
        std::remove_if(m_playing_sounds.begin(), m_playing_sounds.end(), CInappropriateSoundPredicate(sound_mask)),
        m_playing_sounds.end());
}

void CSoundPlayer::update_playing_sounds()
{
    xr_vector<CSoundSingle>::iterator I = m_playing_sounds.begin();
    xr_vector<CSoundSingle>::iterator E = m_playing_sounds.end();
    for (; I != E; ++I)
    {
        if ((*I).m_sound->_feedback())
            (*I).m_sound->_feedback()->set_position(compute_sound_point(*I));
        else if (!(*I).started() && (Device.dwTimeGlobal >= (*I).m_start_time))
            (*I).play_at_pos(m_object, compute_sound_point(*I));
    }
}

bool CSoundPlayer::need_bone_data() const
{
    xr_vector<CSoundSingle>::const_iterator I = m_playing_sounds.begin();
    xr_vector<CSoundSingle>::const_iterator E = m_playing_sounds.end();
    for (; I != E; ++I)
    {
        if ((*I).m_sound->_feedback())
            return (true);
        else if (!(*I).started() && (Device.dwTimeGlobal >= (*I).m_start_time))
            return (true);
    }
    return (false);
}

void CSoundPlayer::play(
    u32 internal_type, u32 max_start_time, u32 min_start_time, u32 max_stop_time, u32 min_stop_time, u32 id)
{
    if (!check_sound_legacy(internal_type))
        return;

    SOUND_COLLECTIONS::iterator I = m_sounds.find(internal_type);

    const auto& entry = I->second.second;

if (!entry->ready())
{
    // Створюємо відкладений запит
    SPendingPlay request = {
        internal_type,
        max_start_time,
        min_start_time,
        max_stop_time,
        min_stop_time,
        id,
        Device.dwTimeGlobal
    };

    // Якщо запит цього типу вже є,
    // оновлюємо його замість створення нового.
    for (auto& pending : m_pending_plays)
    {
        if (pending.internal_type == internal_type)
        {
            
             const u32 original_time = pending.queued_at;

    pending = request;
    pending.queued_at = original_time;
            //pending = request;
            return;
        }
    }

    // Захист від переповнення черги
    if (m_pending_plays.size() < 16)
    {
        m_pending_plays.push_back(request);
    }

    return;
}

//CSoundCollection& collection = *entry->collection;

    /*const auto& entry = I->second.second;

if (!entry->ready())
    return;*/

// Колекція готова: видаляємо старий запит цього типу
if (!m_pending_plays.empty())
{
    m_pending_plays.erase(
        std::remove_if(
            m_pending_plays.begin(),
            m_pending_plays.end(),
            [internal_type](const SPendingPlay& pending)
            {
                return pending.internal_type == internal_type;
            }),
        m_pending_plays.end()
    );
}


CSoundCollection& collection = *entry->collection;

    VERIFY(m_sounds.end() != I);
    CSoundCollectionParamsFull& sound = (*I).second.first;
    //if ((*I).second.second->m_sounds.empty())
    if (collection.m_sounds.empty())
    {
#ifdef DEBUG
        Msg("There are no sounds in sound collection \"%s\" with internal type %d (sound_script = %d)",
            sound.m_sound_prefix.c_str(), internal_type, StalkerSpace::eStalkerSoundScript);
#endif
        return;
    }

    remove_inappropriate_sounds(sound.m_synchro_mask);

    CSoundSingle sound_single;
    (CSoundParams&)sound_single = (CSoundParams&)sound;
    sound_single.m_bone_id = smart_cast<IKinematics*>(m_object->Visual())->LL_BoneID(sound.m_bone_name);
    R_ASSERT(sound_single.m_bone_id != BI_NONE);

    sound_single.m_sound = xr_new<ref_sound>();
    /**
    sound_single.m_sound->clone	(
        *(*I).second.second->m_sounds[
            id == u32(-1)
            ?
            (*I).second.second->random(
                (*I).second.second->m_sounds.size()
            )
            :
            id
        ],
        st_Effect,
        sg_SourceType
    );
    **/
    //sound_single.m_sound->clone((*I).second.second->random(id), st_Effect, sg_SourceType);

    sound_single.m_sound->clone(
    collection.random(id), st_Effect, sg_SourceType);

    sound_single.m_sound->_get()->g_object = m_object;
    sound_single.m_sound->_get()->g_userdata = (*I).second.first.m_data;
    VERIFY(sound_single.m_sound->_handle());

    VERIFY(max_start_time >= min_start_time);
    VERIFY(max_stop_time >= min_stop_time);
    u32 random_time = 0;

    if (max_start_time)
        random_time = (max_start_time > min_start_time) ? random(max_start_time - min_start_time) + min_start_time :
                                                          max_start_time;

    sound_single.m_start_time = Device.dwTimeGlobal + random_time;

    random_time = 0;
    if (max_stop_time)
        random_time =
            (max_stop_time > min_stop_time) ? random(max_stop_time - min_stop_time) + min_stop_time : max_stop_time;

    sound_single.m_stop_time =
        sound_single.m_start_time + iFloor(sound_single.m_sound->get_length_sec() * 1000.0f) + random_time;
    m_playing_sounds.push_back(sound_single);

    if (Device.dwTimeGlobal >= m_playing_sounds.back().m_start_time)
        m_playing_sounds.back().play_at_pos(m_object, compute_sound_point(m_playing_sounds.back()));
}

IC Fvector CSoundPlayer::compute_sound_point(const CSoundSingle& sound)
{
    Fmatrix l_tMatrix;
    l_tMatrix.mul_43(m_object->XFORM(),
        smart_cast<IKinematics*>(m_object->Visual())->LL_GetBoneInstance(sound.m_bone_id).mTransform);
    return (l_tMatrix.c);
}


CSoundPlayer::CSoundCollection::CSoundCollection(
    const CSoundCollectionParams& params, const xr_vector<shared_str>& files)
    : m_last_sound_id(u32(-1))
{
    seed(u32(CPU::QPC() & 0xffffffff));
    m_sounds.reserve(files.size());
    for (const auto& name : files)
    {
        // Metadata was validated before spawn. Preserve all declared indices.
        ref_sound* sound = add(params.m_type, name.c_str());
        R_ASSERT3(sound, "Catalog sound could not be instantiated", name.c_str());
        m_sounds.push_back(sound);
    }
}

/*CSoundPlayer::CSoundCollection::CSoundCollection(const CSoundCollectionParams& params)
    : m_last_sound_id(u32(-1))
{
    seed(u32(CPU::QPC() & 0xffffffff));
    m_sounds.clear();
    for (int j = 0, N = _GetItemCount(params.m_sound_prefix.c_str()); j < N; ++j)
    {
        string_path fn, s, temp;
        pstr S = (pstr)&s;
        _GetItem(params.m_sound_prefix.c_str(), j, temp);
        strconcat(sizeof(s), S, params.m_sound_player_prefix.c_str(), temp);
        if (FS.exist(fn, "$game_sounds$", S, ".ogg"))
        {
            ref_sound* temp = add(params.m_type, S);
            if (temp)
                m_sounds.push_back(temp);
        }
        for (u32 i = 0; i < params.m_max_count; ++i)
        {
            string256 name;
            xr_sprintf(name, "%s%d", S, i);
            if (FS.exist(fn, "$game_sounds$", name, ".ogg"))
            {
                ref_sound* temp = add(params.m_type, name);
                if (temp)
                    m_sounds.push_back(temp);
            }
        }
    }
#ifdef DEBUG
    if (m_sounds.empty())
        Msg("- There are no sounds with prefix %s", params.m_sound_prefix.c_str());
#endif
}*/

CSoundPlayer::CSoundCollection::~CSoundCollection()
{
#ifdef DEBUG
    
    xr_vector<ref_sound*>::iterator I = m_sounds.begin();
    xr_vector<ref_sound*>::iterator E = m_sounds.end();
    for (; I != E; ++I)
    {
        VERIFY(*I);
        VERIFY(!(*I)->_feedback());
    }
#endif
    delete_data(m_sounds);
}

const ref_sound& CSoundPlayer::CSoundCollection::random(u32 id)
{

    VERIFY(!m_sounds.empty());
  
    if (id != u32(-1) && id < m_sounds.size())
    {
        m_last_sound_id = id;
        return (*m_sounds[id]);
    }

    if (m_sounds.size() <= 2)
    {
        m_last_sound_id = CRandom32::random(m_sounds.size());
        return (*m_sounds[m_last_sound_id]);
    }

    u32 result;
    do
    {
        result = CRandom32::random(m_sounds.size());
    } while (result == m_last_sound_id);

    m_last_sound_id = result;
    return *m_sounds[result];
}
