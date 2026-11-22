#pragma once

#include <cstdint>
#include <utility>

#include "xrCore/Threading/TaskManager.hpp"
#include "xrCore/StutterProfile.h"

namespace SpawnPreparation
{
enum : u32 { AnimationTables = 1u << 0, NearestVertex = 1u << 1 };
extern Flags32 Options;

inline bool CanRunTasks(u32 option)
{
    // Options are controlled by Main. Do not read them from worker callbacks.
    return TaskScheduler && TaskManager::GetCurrentWorkerID() == 0 &&
        Options.test(option) && TaskScheduler->GetWorkersCount() > 1;
}

// Scoped joins only: no task pointer or borrowed input survives the caller.
// Reserve before starting work so recording a newly queued task cannot allocate.
class TaskGroup
{
    xr_vector<Task*> tasks_;
    const char* name_;
    std::uintptr_t subject_;

public:
    TaskGroup(size_t capacity, const char* name, const void* subject)
        : name_(name), subject_(reinterpret_cast<std::uintptr_t>(subject))
    {
        tasks_.reserve(capacity);
    }
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    ~TaskGroup() { Join(); }

    template <typename F>
    void Add(F work)
    {
        VERIFY(tasks_.size() < tasks_.capacity());
        tasks_.push_back(&TaskScheduler->AddTask(std::move(work)));
    }

    void Join()
    {
        for (Task* task : tasks_)
        {
            XR_STUTTER_SCOPE(Wait, "spawn.wait.prepare", name_, subject_,
                reinterpret_cast<std::uintptr_t>(task));
            TaskScheduler->Wait(*task);
        }
        tasks_.clear();
    }
};

// Only the distance scan runs in parallel; lazy restriction initialization and
// the subsequent neighbour/inside checks remain on the caller's thread.
u32 SelectNearestVertex(const xr_vector<u32>& vertices, const Fvector& position);
}
