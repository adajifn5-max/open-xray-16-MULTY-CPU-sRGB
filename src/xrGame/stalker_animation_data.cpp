////////////////////////////////////////////////////////////////////////////
//	Module 		: stalker_animation_data.cpp
//	Created 	: 13.10.2005
//  Modified 	: 13.10.2005
//	Author		: Dmitriy Iassenev
//	Description : Stalker animation data
////////////////////////////////////////////////////////////////////////////

#include "StdAfx.h"
#include "stalker_animation_data.h"
#include "spawn_preparation.h"

CStalkerAnimationData::CStalkerAnimationData(IKinematicsAnimated* skeleton_animated)
{
    XR_STUTTER_NPC_SCOPE(Phase, "spawn.prepare.animations", "animation tables",
        reinterpret_cast<std::uintptr_t>(this));

    if (!SpawnPreparation::CanRunTasks(SpawnPreparation::AnimationTables))
    {
        m_part_animations.Load(skeleton_animated, "");
        m_head_animations.Load(skeleton_animated, "");
        m_global_animations.Load(skeleton_animated, "item_");
        return;
    }

    size_t stateCount = 0;
    while (state_names[stateCount])
        ++stateCount;

    // Resize before queuing: each task owns a different state, including its
    // nested vectors. The visual and its loaded motion maps stay alive and
    // unchanged until all tasks have joined. No animation playback occurs here.
    m_part_animations.A.resize(stateCount);
    SpawnPreparation::TaskGroup tasks(stateCount, "animation tables", this);
    for (size_t i = 1; i < stateCount; ++i)
    {
        tasks.Add([this, skeleton_animated, i]
        {
            XR_STUTTER_SCOPE(Task, "spawn.task.animations", state_names[i],
                reinterpret_cast<std::uintptr_t>(this), i);
            m_part_animations.A[i].Load(skeleton_animated, state_names[i]);
        });
    }

    // Useful independent work before the join, rather than AddTask + Wait.
    if (stateCount)
        m_part_animations.A[0].Load(skeleton_animated, state_names[0]);
    m_head_animations.Load(skeleton_animated, "");
    m_global_animations.Load(skeleton_animated, "item_");
    tasks.Join();
}
