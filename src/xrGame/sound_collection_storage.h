////////////////////////////////////////////////////////////////////////////
//	Module 		: sound_collection_storage.h
//	Created 	: 13.10.2005
//  Modified 	: 13.10.2005
//	Author		: Dmitriy Iassenev
//	Description : sound collection storage
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "sound_player.h"
#include <deque>
#include <unordered_map>
#include <thread>

class CSoundCollectionStorage
{
public:
    std::thread::id m_ownerThreadId = std::this_thread::get_id();
    //typedef CSoundPlayer::CSoundCollectionParams CSoundCollectionParams;
    //typedef CSoundPlayer::CSoundCollection CSoundCollection;
    //typedef std::pair<CSoundCollectionParams, CSoundCollection*> SOUND_COLLECTION_PAIR;
    //typedef xr_vector<SOUND_COLLECTION_PAIR> OBJECTS;
    using CSoundCollectionParams = CSoundPlayer::CSoundCollectionParams;
    using CSoundCollection = CSoundPlayer::CSoundCollection;
    using CollectionEntry = CSoundPlayer::CSoundCollectionEntry;
    using CollectionHandle = CSoundPlayer::CollectionHandle;

private:
    //OBJECTS m_objects;
    u32 m_lastSpawnFrame = u32(-1);
    struct CollectionKeyHash
    {
        size_t operator()(const CSoundCollectionParams& p) const noexcept
        {
            size_t hash = std::hash<shared_str>{}(p.m_sound_prefix);
            auto mix = [&hash](size_t value) { hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2); };
            mix(std::hash<shared_str>{}(p.m_sound_player_prefix));
            mix(p.m_max_count);
            mix(static_cast<size_t>(p.m_type));
            return hash;
        }
    };
    std::unordered_map<CSoundCollectionParams, CollectionHandle, CollectionKeyHash> m_objects;
    std::deque<CollectionHandle> m_pending;
    bool m_hasProcessedFrame = false;
    u32 m_lastProcessedFrame = 0;

public:

    void notify_object_spawn(u32 frame)
{
    m_lastSpawnFrame = frame;
}

    virtual ~CSoundCollectionStorage();

     CollectionHandle object(const CSoundCollectionParams& params);
    void update();

    //const SOUND_COLLECTION_PAIR& object(const CSoundCollectionParams& params);
};

extern CSoundCollectionStorage* g_sound_collection_storage;

IC CSoundCollectionStorage& sound_collection_storage();

#include "sound_collection_storage_inline.h"
