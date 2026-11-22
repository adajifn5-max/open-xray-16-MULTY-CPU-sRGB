////////////////////////////////////////////////////////////////////////////
//	Module 		: alife_schedule_registry_inline.h
//	Created 	: 15.01.2003
//  Modified 	: 12.05.2004
//	Author		: Dmitriy Iassenev
//	Description : ALife schedule registry inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

IC CALifeScheduleRegistry::CALifeScheduleRegistry() { m_objects_per_update = 1; }
IC const u32& CALifeScheduleRegistry::objects_per_update() const { return (m_objects_per_update); }
IC void CALifeScheduleRegistry::objects_per_update(const u32& objects_per_update)
{
    m_objects_per_update = objects_per_update;
}

IC void CALifeScheduleRegistry::request_update()
{
    // A legacy update visits at most objects_per_update distinct objects.
    const u32 count = static_cast<u32>(std::min<size_t>(m_objects_per_update, objects().size()));
    if (!count)
        return;
    m_queued_batches.push_back(count);
    m_pending_objects += count;
}

IC void CALifeScheduleRegistry::finish_batch()
{
    // Registry changes may make a logical batch end before its original quota.
    m_pending_objects -= m_batch_remaining;
    m_batch_remaining = 0;
    m_batch_active = false;
}

IC void CALifeScheduleRegistry::update_budget()
{
    constexpr float total_ms = 9.f; // той самий Limit
    constexpr float min_ms = 0.5f;   // мінімум > 0, бо 0 вимикає ліміт
    const float sched_ms = float(iCeil(psShedulerCurrent)); // як у cycles_limit
    m_budget_seconds = _max(min_ms, total_ms - sched_ms) / 1000.f;
}

IC void CALifeScheduleRegistry::clear()
{
    m_queued_batches.clear();
    m_batch_remaining = 0;
    m_pending_objects = 0;
    m_batch_active = false;
    inherited::clear();
}

IC u32 CALifeScheduleRegistry::process_pending()
{
    if (objects().empty())
    {
        clear();
        return 0;
    }
    update_budget(); // <-- додати
    CTimer sliceTimer;
    sliceTimer.Start();
    u32 processed = 0;
    while (has_pending() && !objects().empty())
    {
        // Always allow one complete object; never interrupt a callback.
        if (processed && m_budget_seconds > 0.f && sliceTimer.GetElapsed_sec() >= m_budget_seconds)
            break;

        if (!m_batch_active)
        {
            VERIFY(!m_queued_batches.empty());
            m_batch_remaining = m_queued_batches.front();
            m_queued_batches.pop_front();
            m_batch_active = true;
            ++m_cycle_count; // One cycle per logical batch, retained across frames.
        }

        _iterator object = next();
        CUpdatePredicate predicate(1);
        if (!predicate(object, m_cycle_count, true))
        {
            finish_batch(); // Do not visit the same object twice after wrapping.
            continue;
        }

        update_next(); // Same removal-safe cursor advancement as the legacy iterator.
        --m_batch_remaining;
        --m_pending_objects;
        ++processed;
        predicate(object, m_cycle_count);
        // No object/iterator access here: update may remove it or clear the registry.
        if (m_batch_active && !m_batch_remaining)
            finish_batch();
    }
    if (objects().empty())
        clear();
    return processed;
}

IC void CALifeScheduleRegistry::update()
{
    request_update();
    process_pending();
}

IC CSE_ALifeSchedulable* CALifeScheduleRegistry::object(const ALife::_OBJECT_ID& id, bool no_assert) const
{
    _const_iterator I = objects().find(id);
    if (I == objects().end())
    {
        THROW2(no_assert, "The spesified object hasn't been found in the schedule registry!");
        return (0);
    }
    return ((*I).second);
}
