////////////////////////////////////////////////////////////////////////////
//	Module 		: sound_collection_storage.cpp
//	Created 	: 13.10.2005
//  Modified 	: 13.10.2005
//	Author		: Dmitriy Iassenev
//	Description : sound collection storage
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "xrCore/StutterProfile.h"
#include "sound_collection_storage.h"
#include "xrSound/SoundCatalog.h"
#include "Common/object_broker.h"
#include "xrCore/SoundProfileLite.h"

CSoundCollectionStorage* g_sound_collection_storage = 0;

/*class collection_predicate
{
private:
    typedef CSoundCollectionStorage::CSoundCollectionParams CSoundCollectionParams;
    typedef CSoundCollectionStorage::SOUND_COLLECTION_PAIR SOUND_COLLECTION_PAIR;

private:
    const CSoundCollectionParams* m_params;

public:
    IC collection_predicate(const CSoundCollectionParams& params) { m_params = &params; }
    IC bool operator()(const SOUND_COLLECTION_PAIR& pair) const { return (*m_params == pair.first); }
};*/

//CSoundCollectionStorage::~CSoundCollectionStorage() { delete_data(m_objects); }
CSoundCollectionStorage::~CSoundCollectionStorage()
{
    SoundProfileLite::PlayerAdd().Report("CSoundPlayer::add");
    SoundProfileLite::PlayerEntries().Report("NPC sound entries");
    SoundProfileLite::Collections().Report("Sound collections");

    //delete_data(m_objects);
    m_pending.clear();
m_objects.clear();
}

CSoundCollectionStorage::CollectionHandle
CSoundCollectionStorage::object(const CSoundCollectionParams& params)
{
    VERIFY(std::this_thread::get_id() == m_ownerThreadId);

    XR_STUTTER_DISABLED_SCOPE(Phase, "SoundCollections::lookup", params.m_sound_prefix.c_str(), m_objects.size());
    const auto found = m_objects.find(params);
    if (found != m_objects.end())
    {
        SoundProfileLite::Collections().Hit();
        return found->second;
    }

    SoundProfileLite::Collections().Miss();

    auto entry = std::make_shared<CollectionEntry>(params);
    {
        XR_STUTTER_DISABLED_SCOPE(Phase, "SoundCollections::variants", params.m_sound_prefix.c_str(), params.m_max_count);
        SoundCatalogCollectVariants(params.m_sound_player_prefix.c_str(), params.m_sound_prefix.c_str(),
            params.m_max_count, entry->files);
    }

    m_objects.emplace(params, entry);
    m_pending.push_back(entry);

    return entry;

/*const CSoundCollectionStorage::SOUND_COLLECTION_PAIR& CSoundCollectionStorage::object(
    const CSoundCollectionParams& params)
{
    #ifdef DEBUG
VERIFY(std::this_thread::get_id() == m_ownerThreadId);
#endif
    OBJECTS::const_iterator I =
        std::find_if(
            m_objects.begin(),
            m_objects.end(),
            collection_predicate(params));

    if (I != m_objects.end())
    {
        SoundProfileLite::Collections().Hit();
        return *I;
    }

    SoundProfileLite::Collections().Miss();

    return m_objects.emplace_back(
        params,
        xr_new<CSoundCollection>(params));*/



    /*OBJECTS::const_iterator I = std::find_if(m_objects.begin(), m_objects.end(), collection_predicate(params));
    if (I != m_objects.end())
        return (*I);

    return m_objects.emplace_back(params, xr_new<CSoundCollection>(params));*/
}
void CSoundCollectionStorage::update()
{

    // Відкладаємо створення звукових колекцій,
// якщо в цьому або попередньому кадрі був Spawn.
if (m_lastSpawnFrame != u32(-1) &&
    Device.dwFrame - m_lastSpawnFrame <= 1)
{
    return;
}


    VERIFY(std::this_thread::get_id() == m_ownerThreadId);

    const u32 frame = Device.dwFrame;
    constexpr u32 frameInterval = 2;
    if (m_hasProcessedFrame && u32(frame - m_lastProcessedFrame) < frameInterval)
        {
        return;
        }
    m_hasProcessedFrame = true;
    m_lastProcessedFrame = frame;

    constexpr u32 limit = 40; // розподілення навантаження
    u32 created = 0;

    while (!m_pending.empty() && created < limit)
    {
        auto entry = m_pending.front();

        entry->collection =
            xr_new<CSoundCollection>(entry->params, entry->files);

        m_pending.pop_front();
        ++created;
    }

#if XRAY_SOUND_DIAGNOSTICS
    if (created)
    {
        Msg("[SOUND_QUEUE] frame=%u created=%u pending=%u",
            frame,
            created,
            static_cast<u32>(m_pending.size()));
    }
#endif
}
