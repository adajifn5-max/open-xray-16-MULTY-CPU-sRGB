/*
    Copyright (c) 2014-2021 OpenXRay

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/
#include "stdafx.h"
#include "xrCore/StutterProfile.h"

#include "Task.hpp"
#include "TaskManager.hpp"

#include "ScopeLock.hpp"

#include <thread>
#include <SDL_events.h>

#include "Math/fast_lc16.hpp"

#include <random>
#include <algorithm>
#include <exception>
#include <chrono>
#include <cstddef>


#if defined(XR_PLATFORM_WINDOWS)
#include <Windows.h>
// PlatformWindows.inl defines NOKERNEL; do not rely on windows.h to expose these APIs.
#include <winbase.h>
#include <processthreadsapi.h>
#include <synchapi.h>
#include <processtopologyapi.h>
#include <sysinfoapi.h>
#endif

#if defined(XR_ARCHITECTURE_X86) || defined(XR_ARCHITECTURE_X64) || defined(XR_ARCHITECTURE_E2K)
#include <immintrin.h>
#elif defined(XR_ARCHITECTURE_ARM) || defined(XR_ARCHITECTURE_ARM64)
#include "sse2neon/sse2neon.h"
#elif defined(XR_ARCHITECTURE_RISCV)
#include "sse2rvv/sse2rvv.h"
#elif defined(XR_ARCHITECTURE_PPC64)
#include <xmmintrin.h>
#else
#error Add your platform here
#endif

xr_unique_ptr<TaskManager> TaskScheduler;
static std::atomic_size_t g_queuesWithTasks{ 0 };

static constexpr size_t OTHER_THREADS_COUNT = 1; // Primary and Secondary thread

class TaskStorageSize
{
    template <int E, typename Dummy = void> // XXX: remove Dummy workaround when gcc will finally match the standard
    struct PowerOfTwo
    {
        static constexpr size_t value = 2 * PowerOfTwo<E - 1>::value;
    };

    template <typename Dummy>
    struct PowerOfTwo<0, Dummy>
    {
        static constexpr size_t value = 1;
    };

public:
    template <size_t Power>
    static constexpr size_t Get()
    {
        return PowerOfTwo<Power>::value;
    }
};

static constexpr size_t TASK_STORAGE_SIZE = TaskStorageSize::Get<12>(); // 4096 tasks
static constexpr size_t TASK_STORAGE_MASK = TASK_STORAGE_SIZE - 1;

class TaskAllocator
{
    size_t m_allocated{};
    Task   m_storage[TASK_STORAGE_SIZE];

public:
    Task* allocate() noexcept
    {
        Task* task = &m_storage[m_allocated++ & TASK_STORAGE_MASK];
        VERIFY(task->IsFinished());
        return task;
    }
} static thread_local s_tl_allocator;

// Multi producer
// Multi consumer
class TaskQueue
{
    std::mutex m_lock;

    size_t m_head_pos{};
    size_t m_tail_pos{};

    // Scheduling hint. Slot access is protected by m_lock.
    std::atomic_size_t m_queuedTasks{};

    Task* m_storage[TASK_STORAGE_SIZE]{};

public:
    void push(Task* task) noexcept
    {
        std::lock_guard guard{ m_lock };

        const size_t queued =
            m_queuedTasks.load(std::memory_order_relaxed);

        R_ASSERT2(queued < TASK_STORAGE_SIZE, "Task queue overflow");

        m_storage[m_tail_pos & TASK_STORAGE_MASK] = task;
        ++m_tail_pos;

        m_queuedTasks.store(queued + 1, std::memory_order_relaxed);

        if (queued == 0)
            g_queuesWithTasks.fetch_add(1, std::memory_order_relaxed);
    }

    Task* pop() noexcept
    {
        std::lock_guard guard{ m_lock };

        const size_t queued =
            m_queuedTasks.load(std::memory_order_relaxed);

        if (queued == 0)
            return nullptr;

        Task*& slot = m_storage[m_head_pos & TASK_STORAGE_MASK];
        Task* task = slot;

        slot = nullptr;
        ++m_head_pos;

        m_queuedTasks.store(queued - 1, std::memory_order_relaxed);

        if (queued == 1)
            g_queuesWithTasks.fetch_sub(1, std::memory_order_relaxed);

        return task;
    }

    Task* steal() noexcept
    {
        return pop();
    }

    size_t size() const noexcept
    {
        return m_queuedTasks.load(std::memory_order_relaxed);
    }

    bool empty() const noexcept
    {
        return size() == 0;
    }
    
    /*std::atomic_size_t  m_head_pos{};
    std::atomic_size_t  m_tail_pos{};
    std::atomic_size_t  m_queuedTasks{};
    Task*               m_storage[TASK_STORAGE_SIZE]{};

    //std::atomic_size_t  m_head_pos{};
   // std::atomic_size_t  m_tail_pos{};
   // Task*               m_storage[TASK_STORAGE_SIZE]{};

public:
    void push(Task* task) noexcept
    {
        // Рахуємо завдання в черзі
    const size_t previous = m_queuedTasks.fetch_add(1, std::memory_order_relaxed);

    // Черга перейшла зі стану empty до non-empty
    if (previous == 0)
        g_queuesWithTasks.fetch_add(1, std::memory_order_relaxed);

    const auto task_pos = m_tail_pos.fetch_add(1, std::memory_order_acq_rel);

    VERIFY2(task_pos - m_head_pos.load(std::memory_order_acquire) < TASK_STORAGE_SIZE,
        "Task queue overflow");

    m_storage[task_pos & TASK_STORAGE_MASK] = task;
    }

    Task* pop() noexcept
    {
        size_t head_pos = m_head_pos.load(std::memory_order_acquire);
        Task* task = m_storage[head_pos & TASK_STORAGE_MASK];
        if (!task)
            return nullptr;

        const bool success = m_head_pos.compare_exchange_weak(head_pos, head_pos + 1, std::memory_order_acq_rel);
        if (success)
        {
            m_storage[head_pos & TASK_STORAGE_MASK] = nullptr;

    const size_t previous = m_queuedTasks.fetch_sub(1, std::memory_order_relaxed);

    VERIFY(previous > 0);

    // Черга стала порожньою
    if (previous == 1)
        g_queuesWithTasks.fetch_sub(1, std::memory_order_relaxed);

    return task;
        }
        return nullptr;
    }

    Task* steal() noexcept
    {
        return pop();
    }

    size_t size() const noexcept
    {
        return m_queuedTasks.load(std::memory_order_relaxed);
    }

    bool empty() const noexcept
    {
        return size() == 0;
    }*/
};

struct TaskWorkerStats
{
    size_t allocatedTasks{};
    size_t pushedTasks{};
    size_t finishedTasks{};
};

class TaskWorker : public TaskQueue, public TaskWorkerStats
{
public:
    fast_lc16        random{ this };
    size_t           id    { size_t(-1) };
#if defined(XR_PLATFORM_WINDOWS) && (defined(DEBUG) || defined(_DEBUG))
    DWORD debugThreadId{};
    GROUP_AFFINITY debugActualAffinity{};
#endif
} static thread_local s_tl_worker;

namespace
{
void RequireTaskManager(bool condition, pcstr message)
{
    if (!condition)
    {
        R_ASSERT2(false, message);
        std::terminate(); // Do not continue with overlapping or empty affinity masks.
    }
}

constexpr auto TASK_THREAD_TIMEOUT = std::chrono::seconds(60);

#if defined(XR_PLATFORM_WINDOWS)
KAFFINITY FirstTaskCpu(KAFFINITY mask)
{
    return mask & (~mask + KAFFINITY{ 1 });
}

size_t CountTaskCpus(KAFFINITY mask)
{
    size_t count = 0;
    for (; mask; mask &= mask - 1)
        ++count;
    return count;
}

xr_vector<GROUP_AFFINITY> GetAvailableTaskCores()
{
    // The buffer contains variable-sized records and must be pointer-aligned.
    xr_vector<u64> buffer;
    DWORD bytes = 0;
    unsigned int attempts = 0;
    for (;;)
    {
        bytes = static_cast<DWORD>(buffer.size() * sizeof(u64));
        auto* data = buffer.empty() ? nullptr :
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, data, &bytes))
            break;

        const DWORD error = GetLastError();
        RequireTaskManager(error == ERROR_INSUFFICIENT_BUFFER && bytes != 0 && ++attempts <= 4,
            "TaskManager: cannot query physical CPU topology.");
        buffer.resize((static_cast<size_t>(bytes) + sizeof(u64) - 1) / sizeof(u64));
    }

    GROUP_AFFINITY initialAffinity{};
    RequireTaskManager(GetThreadGroupAffinity(GetCurrentThread(), &initialAffinity) != FALSE,
        "TaskManager: cannot query the initial processor group.");
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    RequireTaskManager(GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) != FALSE,
        "TaskManager: cannot query process CPU affinity.");

    xr_vector<GROUP_AFFINITY> cores;
    for (size_t offset = 0; offset < bytes;)
    {
        constexpr size_t coreRecordSize = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) +
            offsetof(PROCESSOR_RELATIONSHIP, GroupMask) + sizeof(GROUP_AFFINITY);
        RequireTaskManager(bytes - offset >= coreRecordSize,
            "TaskManager: truncated physical CPU topology record.");
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
            reinterpret_cast<const unsigned char*>(buffer.data()) + offset);
        RequireTaskManager(entry->Size >= coreRecordSize && entry->Size <= bytes - offset &&
            entry->Relationship == RelationProcessorCore && entry->Processor.GroupCount == 1,
            "TaskManager: invalid physical CPU topology record.");

        GROUP_AFFINITY core = entry->Processor.GroupMask[0];
        // Capture this restriction before a probe visits any other groups.
        if (core.Group == initialAffinity.Group && processMask != 0)
            core.Mask &= processMask;
        cores.emplace_back(core);
        offset += entry->Size;
    }

    // A process mask describes only one group (or is zero for a multi-group
    // process). Probe other groups without changing Main's thread affinity.
    if (GetActiveProcessorGroupCount() > 1 || processMask == 0)
    {
        DWORD probeError = ERROR_SUCCESS;
        std::thread probe([&cores, &probeError]
        {
            for (auto& core : cores)
            {
                KAFFINITY allowed = 0;
                for (KAFFINITY remaining = core.Mask; remaining; remaining &= remaining - 1)
                {
                    GROUP_AFFINITY candidate{};
                    candidate.Group = core.Group;
                    candidate.Mask = FirstTaskCpu(remaining);
                    if (SetThreadGroupAffinity(GetCurrentThread(), &candidate, nullptr))
                    {
                        GROUP_AFFINITY actual{};
                        if (!GetThreadGroupAffinity(GetCurrentThread(), &actual))
                        {
                            probeError = GetLastError();
                            return;
                        }
                        if (actual.Group != candidate.Group || actual.Mask != candidate.Mask)
                        {
                            probeError = ERROR_INVALID_DATA;
                            return;
                        }
                        allowed |= actual.Mask;
                    }
                    else
                    {
                        const DWORD error = GetLastError();
                        if (error != ERROR_INVALID_PARAMETER)
                        {
                            probeError = error;
                            return;
                        }
                    }
                }
                core.Mask = allowed;
            }
        });
        probe.join();
        RequireTaskManager(probeError == ERROR_SUCCESS,
            "TaskManager: cannot determine allowed CPUs across processor groups.");
    }

    cores.erase(std::remove_if(cores.begin(), cores.end(),
        [](const GROUP_AFFINITY& core) { return core.Mask == 0; }), cores.end());
    std::sort(cores.begin(), cores.end(), [](const GROUP_AFFINITY& left, const GROUP_AFFINITY& right)
    {
        return left.Group != right.Group ? left.Group < right.Group :
            FirstTaskCpu(left.Mask) < FirstTaskCpu(right.Mask);
    });
    return cores;
}
#endif
} // namespace

#if defined(XR_PLATFORM_WINDOWS)
void TaskManager::InitializeThreadAffinity()
{
    const auto cores = GetAvailableTaskCores();
    size_t logicalCpuCount = 0;
    for (const auto& core : cores)
        logicalCpuCount += CountTaskCpus(core.Mask);

    // Support for smaller configurations is reserved for a separately agreed
    // change. In particular, do not execute tasks in Main's Wait as a fallback.
    RequireTaskManager(logicalCpuCount >= 3 && cores.size() >= 2,
        "TaskManager requires at least 3 available logical CPUs and 2 physical cores; smaller configurations are not implemented.");

    mainThreadAffinity = { cores[0].Mask, cores[0].Group };
    const auto& secondCore = cores[1];
    workerAffinities.push_back({ FirstTaskCpu(secondCore.Mask), secondCore.Group });

    // Merge all remaining physical cores by group, excluding the entire Main
    // core, including its SMT siblings. Worker 1 has a single-CPU mask; its CPU
    // remains available to other workers in that group (no exclusive reservation).
    xr_vector<ThreadAffinity> groups;
    for (size_t i = 1; i < cores.size(); ++i)
    {
        if (groups.empty() || groups.back().group != cores[i].Group)
            groups.push_back({ cores[i].Mask, cores[i].Group });
        else
            groups.back().mask |= cores[i].Mask;
    }
    for (const auto& group : groups)
    {
        const size_t count = CountTaskCpus(group.mask);
        const size_t first = group.group == secondCore.Group ? 1 : 0;
        for (size_t i = first; i < count; ++i)
            workerAffinities.emplace_back(group);
    }
    workerThreadCount = workerAffinities.size();
    RequireTaskManager(workerThreadCount != 0 && CountTaskCpus(workerAffinities[0].mask) == 1,
        "TaskManager: Worker 1 must have exactly one allowed logical CPU.");
    for (const auto& affinity : workerAffinities)
        RequireTaskManager(affinity.mask != 0 && (affinity.group != mainThreadAffinity.group ||
            (affinity.mask & mainThreadAffinity.mask) == 0),
            "TaskManager: worker CPU affinity overlaps Main's physical core.");
}
#endif

TaskManager::TaskManager()
#if defined(XR_PLATFORM_WINDOWS)
    : stopRequested(CreateEventW(nullptr, TRUE, FALSE, nullptr))
#endif
{
    ZoneScoped;
#if defined(XR_PLATFORM_WINDOWS)
    RequireTaskManager(stopRequested.Valid(), "TaskManager: cannot create the shutdown event.");
#endif
#if defined(XR_PLATFORM_WINDOWS)
    InitializeThreadAffinity();
#else
    const size_t availableThreads = std::thread::hardware_concurrency();
    workerThreadCount = availableThreads > OTHER_THREADS_COUNT ? availableThreads - OTHER_THREADS_COUNT : 0;
#endif
    workers.reserve(workerThreadCount + OTHER_THREADS_COUNT);
    RegisterThisThreadAsWorker();
}

void TaskManager::SpawnThreads()
{
    ZoneScoped;
    {
        std::lock_guard guard{ workersLock };
        RequireTaskManager(!threadsSpawned && workers.size() == OTHER_THREADS_COUNT &&
            s_tl_worker.id == 0 && !shouldStop.load(std::memory_order_acquire),
            "TaskManager: SpawnThreads must run once on Main 0 before shutdown.");
        threadsSpawned = true;
    }
    workerThreads.reserve(workerThreadCount);

    const auto spawnWorkers = [this]
    {
        for (size_t i = 0; i < workerThreadCount; ++i)
            workerThreads.emplace_back(Threading::RunThread("Task Worker", &TaskManager::TaskWorkerStart, this));
    };
#if defined(XR_PLATFORM_WINDOWS)
    // The launcher is not a registered worker. Workers inherit its mask outside
    // Main's core, including while Threading::RunThread initializes them.
    std::thread launcher([this, &spawnWorkers]
    {
        RequireTaskManager(!workerAffinities.empty(), "TaskManager: no worker CPU affinity available.");
        const ThreadAffinity& selected = workerAffinities.front();
        GROUP_AFFINITY affinity{};
        affinity.Mask = selected.mask;
        affinity.Group = selected.group;
        RequireTaskManager(SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != FALSE,
            "TaskManager: cannot move the worker launcher off Main's core.");
        spawnWorkers();
    });
    launcher.join();
#else
    spawnWorkers();
#endif

    std::unique_lock lock{ workersLock };
    const bool ready = workerStateChanged.wait_for(lock, TASK_THREAD_TIMEOUT, [this]
    {
        return workers.size() == workerThreadCount + OTHER_THREADS_COUNT;
    });
    RequireTaskManager(ready, "TaskManager: worker registration timed out.");
#if defined(XR_PLATFORM_WINDOWS) && (defined(DEBUG) || defined(_DEBUG))
    struct WorkerDiagnostic
    {
        size_t id;
        DWORD threadId;
        GROUP_AFFINITY actualAffinity;
    };
    xr_vector<WorkerDiagnostic> diagnostics;
    diagnostics.reserve(workers.size());
    for (const auto* worker : workers)
        diagnostics.push_back({ worker->id, worker->debugThreadId, worker->debugActualAffinity });
#endif
    workersReady.store(true, std::memory_order_release);
    lock.unlock();
    workerStateChanged.notify_all();
#if defined(XR_PLATFORM_WINDOWS) && (defined(DEBUG) || defined(_DEBUG))
    // Msg also writes to the debugger Output through Log/AddOne, and keeps the
    // same startup record in the engine log. Never call it under workersLock.
    Msg("[TaskManager][Affinity] BEGIN: Main 0 + %zu workers", workerThreadCount);
    for (const auto& diagnostic : diagnostics)
        Msg("[TaskManager][Affinity] Worker ID=%zu OS Thread ID=%lu group=%u actual mask=0x%016llX",
            diagnostic.id, diagnostic.threadId, static_cast<unsigned int>(diagnostic.actualAffinity.Group),
            static_cast<unsigned long long>(diagnostic.actualAffinity.Mask));
    Msg("[TaskManager][Affinity] END: Worker 1 CPU is shared with other workers in its group");
#endif
}

TaskManager::~TaskManager()
{
    ZoneScoped;
    RequireTaskManager(s_tl_worker.id == 0, "TaskManager must be destroyed on Main 0.");
    shouldStop.store(true, std::memory_order_release);
#if defined(XR_PLATFORM_WINDOWS)
    // Manual-reset event: one signal wakes every worker, including paused ones.
    stopRequested.Set();
#else
    for (size_t i = 0; i < workerThreads.size(); ++i)
        newWorkArrived.Set();
#endif
    workerStateChanged.notify_all();

    while (Task* task = s_tl_worker.pop())
        ExecuteTask(*task);

    // Keep the vector and every thread-local worker alive until all threads
    // finish task execution. Do not erase entries while another thread steals.
    UnregisterThisThreadAsWorker();
    for (auto& thread : workerThreads)
        if (thread.joinable())
            thread.join();

    std::lock_guard guard{ workersLock };
    workers.clear();
}

void TaskManager::RegisterThisThreadAsWorker()
{
    ZoneScoped;
    std::lock_guard guard{ workersLock };

    RequireTaskManager(!shouldStop.load(std::memory_order_acquire) &&
        !workersReady.load(std::memory_order_acquire) && s_tl_worker.id == size_t(-1) &&
        workers.size() < workerThreadCount + OTHER_THREADS_COUNT,
        "TaskManager: invalid, duplicate or late worker registration.");

    const size_t id = workers.size();
#if defined(XR_PLATFORM_WINDOWS)
    RequireTaskManager(id == 0 || id - 1 < workerAffinities.size(),
        "TaskManager: worker affinity index is out of range.");
    const ThreadAffinity& selected = id == 0 ? mainThreadAffinity : workerAffinities[id - 1];
    RequireTaskManager(selected.mask != 0 && (id == 0 || selected.group != mainThreadAffinity.group ||
        (selected.mask & mainThreadAffinity.mask) == 0), "TaskManager: invalid worker affinity mask.");
    GROUP_AFFINITY affinity{};
    affinity.Mask = selected.mask;
    affinity.Group = selected.group;
    RequireTaskManager(SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr) != FALSE,
        "TaskManager: cannot apply the required Main/Worker CPU affinity.");
    GROUP_AFFINITY actual{};
    RequireTaskManager(GetThreadGroupAffinity(GetCurrentThread(), &actual) != FALSE &&
        actual.Group == affinity.Group && actual.Mask == affinity.Mask,
        "TaskManager: installed thread affinity differs from the requested mask.");
#if defined(DEBUG) || defined(_DEBUG)
    // Capture OS values on the actual worker; print the complete snapshot on
    // Main after registration, outside workersLock and without interleaving.
    s_tl_worker.debugThreadId = GetCurrentThreadId();
    s_tl_worker.debugActualAffinity = actual;
#endif
#endif
    // Assign the real ID only after affinity has been applied successfully.
    s_tl_worker.id = id;
    workers.emplace_back(&s_tl_worker);
    workerStateChanged.notify_all();
}

void TaskManager::UnregisterThisThreadAsWorker()
{
    ZoneScoped;
    std::unique_lock lock{ workersLock };
    RequireTaskManager(shouldStop.load(std::memory_order_acquire) &&
        s_tl_worker.id < workers.size() && workers[s_tl_worker.id] == &s_tl_worker,
        "TaskManager: unregistration is only supported during shutdown.");
    ++finishedWorkers;
    workerStateChanged.notify_all();
    const bool finished = workerStateChanged.wait_for(lock, TASK_THREAD_TIMEOUT, [this]
    {
        return finishedWorkers == workerThreads.size() + OTHER_THREADS_COUNT;
    });
    RequireTaskManager(finished, "TaskManager: workers did not finish pending tasks before the shutdown timeout.");
    s_tl_worker.id = size_t(-1);
}

void TaskManager::SetThreadStatus(bool active) noexcept
{
    if (active)
        activeWorkersCount.fetch_add(1, std::memory_order_relaxed);
    else
        activeWorkersCount.fetch_sub(1, std::memory_order_relaxed);


}

void TaskManager::TaskWorkerStart()
{
    RegisterThisThreadAsWorker();
    {
        std::unique_lock lock{ workersLock };
        const bool ready = workerStateChanged.wait_for(lock, TASK_THREAD_TIMEOUT, [this]
        {
            return workersReady.load(std::memory_order_acquire) || shouldStop.load(std::memory_order_acquire);
        });
        RequireTaskManager(ready, "TaskManager: worker start barrier timed out.");
    }
    SetThreadStatus(true);

    while (!shouldStop.load(std::memory_order_consume))
    {
        if (!shouldPause.load(std::memory_order_consume) ||
            mainWaitRequests.load(std::memory_order_acquire) != 0)
        {
            if (ExecuteOneTask())
                continue;
        }

        SetThreadStatus(false);
        {
            do
            {
#if defined(XR_PLATFORM_WINDOWS)
                const HANDLE events[] = { stopRequested.GetHandle(), newWorkArrived.GetHandle() };
                const DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                RequireTaskManager(result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1,
                    "TaskManager: worker wait failed.");
#else
                // Bound the wait so a coalesced auto-reset notification cannot
                // leave a worker asleep after shouldStop has been set.
                newWorkArrived.Wait(50);
#endif
            } while (shouldPause.load(std::memory_order_consume) &&
                mainWaitRequests.load(std::memory_order_acquire) == 0 &&
                !shouldStop.load(std::memory_order_acquire));
        }
        SetThreadStatus(true);
    }

    SetThreadStatus(false);

    // Finish all pending tasks
    while (Task* t = s_tl_worker.pop())
    {
        ExecuteTask(*t);
    }

    // Prepare to exit
    UnregisterThisThreadAsWorker();
}

/*Task* TaskManager::TryToSteal() const
{

    const size_t worker_count = workers.size();
     if (worker_count < 2 || worker_count > std::numeric_limits<u16>::max())
        return nullptr;


    std::uniform_int_distribution<u16> dist(
        1, //0,
        static_cast<u16>(worker_count - 1)
    );
    const size_t queuesWithTasks =
    g_queuesWithTasks.load(std::memory_order_relaxed);

    const size_t maxStealAttempts = queuesWithTasks;
    for (int steal_attempts = maxStealAttempts+2; steal_attempts > 0; --steal_attempts) // якщо maxStealAttempts = 0 тоді 2 спроби
    {
        const size_t idx = dist(s_tl_worker.random);

        TaskWorker* other = workers[idx];

        if (other == &s_tl_worker || other == workers[0])
            continue;

        if (auto* task = other->steal())
        {
            if (!other->empty())
                newWorkArrived.Set();
            return task;
        }
        
    }
   
    return nullptr;*/

Task* TaskManager::TryToSteal() const
{
    const size_t worker_count = workers.size();

    if (worker_count < 2 ||
        worker_count > std::numeric_limits<u16>::max())
        return nullptr;

    // Для фонового воркера при двох загальних
    // воркерах немає допустимого донора.
    if (worker_count == 2 && &s_tl_worker != workers[0])
        return nullptr;

    // Головний воркер уже перевіряється
    // у ExecuteOneTask().
    std::uniform_int_distribution<u16> dist(
        1,
        static_cast<u16>(worker_count - 1)
    );

    const size_t queuesWithTasks =
        g_queuesWithTasks.load(std::memory_order_relaxed);

    // Оптимізовано для 8 воркерів
    // та ймовірності влучання > 75%
    size_t steal_attempts =
        queuesWithTasks == 0 ? 2 :
        queuesWithTasks == 1 ? 8 :
        queuesWithTasks == 2 ? 4 :
        queuesWithTasks == 3 ? 3 :
        1;

    while (steal_attempts > 0)
    {
        const size_t idx = dist(s_tl_worker.random);

        TaskWorker* other = workers[idx];

        // Себе пропускаємо без витрати спроби
        if (other == &s_tl_worker)
            continue;

        --steal_attempts;

        if (auto* task = other->steal())
        {
            if (!other->empty())
                newWorkArrived.Set();

            return task;
        }

    }
    return nullptr;
}


Task* TaskManager::AllocateTask() noexcept
{
    ++s_tl_worker.allocatedTasks;
    return s_tl_allocator.allocate();
}

void TaskManager::PushTask(Task& task) noexcept
{
    if (s_tl_worker.id == 0)
    {
        TaskManager* manager = TaskScheduler.get();
        RequireTaskManager(manager != nullptr && manager->workersReady.load(std::memory_order_acquire) &&
            !manager->shouldStop.load(std::memory_order_acquire),
            "TaskManager: Main PushTask requires a running scheduler and Worker 1.");
        // The acquire above observes the complete, immutable registered-worker
        // list. Main owns scheduler startup/shutdown, so no list lock is needed.
        RequireTaskManager(manager->workers.size() > 1 && manager->workers[0] == &s_tl_worker &&
            manager->workers[1] != nullptr && manager->workers[1]->id == 1,
            "TaskManager: Main PushTask cannot find its registered Worker 1.");

        // Worker 1 is a scheduler ID, not logical CPU 1. Its affinity was set
        // before workersReady; the existing mutex protects cross-thread push.
        manager->workers[1]->push(&task);
    }
    else
        s_tl_worker.push(&task);

    // The shared event may wake any worker; tasks in Worker 1 remain stealable.
    newWorkArrived.Set();
    ++s_tl_worker.pushedTasks;

   // if (s_tl_worker.pushedTasks % 1000 == 0)
//{
   // Msg("[TaskManager] worker=%zu pushedTasks=%zu",
      //  s_tl_worker.id, s_tl_worker.pushedTasks);
//}
}

void TaskManager::ExecuteTask(Task& task)
{
    XR_STUTTER_DISABLED_SCOPE(Task, "TaskManager::ExecuteTask", "task address",
        reinterpret_cast<std::uintptr_t>(&task), 0, static_cast<std::uint32_t>(s_tl_worker.id));
    task();
    ++s_tl_worker.finishedTasks;
}

void TaskManager::RunTask(Task& task)
{
    ExecuteTask(task);
}

void TaskManager::Wait(const Task& task, bool updateSystemEvents /*= false*/) const
{
    XR_STUTTER_DISABLED_SCOPE(Wait, "TaskManager::Wait", "task address",
        reinterpret_cast<std::uintptr_t>(&task), 0, static_cast<std::uint32_t>(s_tl_worker.id));
    ZoneScoped;
    if (s_tl_worker.id != 0)
    {
        // Preserve cooperative waiting on workers, including nested child tasks.
        while (!task.IsFinished())
            ExecuteOneTask();
        return;
    }

    if (task.IsFinished())
        return;

    RequireTaskManager(workersReady.load(std::memory_order_acquire) && workerThreadCount != 0 &&
        !shouldStop.load(std::memory_order_acquire),
        "TaskManager: Main Wait requires running worker threads.");

    struct MainWaitScope
    {
        std::atomic_size_t& requests;

        explicit MainWaitScope(std::atomic_size_t& value) : requests(value)
        {
            requests.fetch_add(1, std::memory_order_acq_rel);
        }
        ~MainWaitScope()
        {
            requests.fetch_sub(1, std::memory_order_acq_rel);
        }
    } waitScope{ mainWaitRequests };

    // Main neither pops nor steals nor executes TaskManager tasks here.
    // The existing event requests worker service; it is not a completion event.
    // Repeat the request because auto-reset signals coalesce, and a worker can
    // finish its bounded steal search without finding the requested work.
    while (!task.IsFinished())
    {
        RequireTaskManager(!shouldStop.load(std::memory_order_acquire),
            "TaskManager: worker shutdown interrupted Main Wait.");
        newWorkArrived.Set();
        if (xrDebug::ProcessingFailure() || updateSystemEvents)
            SDL_PumpEvents(); // Necessary to prevent dead locks.
        std::this_thread::yield();
    }
}

bool TaskManager::ExecuteOneTask() const
{
    Task* task = s_tl_worker.pop();
    // Relay work availability before executing this task, including when
    // Worker 1 consumes its own queue after the stage 4 routing change.
    if (task && !s_tl_worker.empty())
        newWorkArrived.Set();

    // Keep the existing steal-attempt budget: other workers are tried first.
    if (!task)
        task = TryToSteal();

    // A worker may fall back to Main only after that search found no task.
    // Main never steals from itself; its own queue was already checked above.
    if (!task && workers[0] != &s_tl_worker)
    {
        TaskWorker* mainWorker = workers[0];
        task = mainWorker->steal();
        if (task && !mainWorker->empty())
            newWorkArrived.Set();
    }

    if (task)
    {
        ExecuteTask(*task);
        return true;
    }
    return false;
}

size_t TaskManager::GetWorkersCount() const noexcept
{
    return workers.size();
}

size_t TaskManager::GetCurrentWorkerID() noexcept
{
    return s_tl_worker.id;
}

void TaskManager::GetStats(size_t& allocated, size_t& pushed, size_t& finished)
{
    std::lock_guard guard{ workersLock };
    for (const TaskWorker* worker : workers)
    {
        allocated += worker->allocatedTasks;
        pushed += worker->pushedTasks;
        finished += worker->finishedTasks;
    }
}
