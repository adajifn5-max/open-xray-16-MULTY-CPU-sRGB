#pragma once
// Temporary diagnostic instrumentation for OpenXRay (Windows, C++17).
// Include AFTER the project's precompiled header, so Msg(...) is declared.
// Diagnostic only: no sound caching behavior is changed.
#ifndef XRAY_SOUND_DIAGNOSTICS
#define XRAY_SOUND_DIAGNOSTICS 0
#endif

#include <cstdint>
#if XRAY_SOUND_DIAGNOSTICS
#include <Windows.h>
#include <atomic>
#endif

namespace SoundProfileLite
{
#if XRAY_SOUND_DIAGNOSTICS
    inline std::int64_t Frequency()
    {
        static const std::int64_t value = [] {
            LARGE_INTEGER v{};
            QueryPerformanceFrequency(&v);
            return static_cast<std::int64_t>(v.QuadPart);
        }();
        return value;
    }

    struct Stats
    {
        std::atomic<std::uint64_t> calls{0};
        std::atomic<std::uint64_t> totalUs{0};
        std::atomic<std::uint64_t> maxUs{0};

        void Add(std::uint64_t us)
        {
            calls.fetch_add(1, std::memory_order_relaxed);
            totalUs.fetch_add(us, std::memory_order_relaxed);
            auto old = maxUs.load(std::memory_order_relaxed);
            while (us > old && !maxUs.compare_exchange_weak(
                       old, us, std::memory_order_relaxed)) {}
        }

        void Report(const char* name) const
        {
            const auto n = calls.load(std::memory_order_relaxed);
            const auto total = totalUs.load(std::memory_order_relaxed);
            const auto maximum = maxUs.load(std::memory_order_relaxed);
            Msg("[SOUND_PROFILE_SUMMARY] name=%s calls=%llu total_ms=%.3f avg_ms=%.4f max_ms=%.3f",
                name, static_cast<unsigned long long>(n), total / 1000.0,
                n ? (total / 1000.0) / n : 0.0, maximum / 1000.0);
        }
    };

    class Scope
    {
        Stats& stats_;
        const char* tag_;
        const char* detail_;
        std::uint32_t frame_;
        std::uint64_t slowLimitUs_;
        LARGE_INTEGER started_{};

    public:
        Scope(Stats& stats, const char* tag, const char* detail = "",
              std::uint32_t frame = 0, std::uint64_t slowLimitUs = 2000)
            : stats_(stats), tag_(tag), detail_(detail ? detail : ""),
              frame_(frame), slowLimitUs_(slowLimitUs)
        {
            QueryPerformanceCounter(&started_);
        }

        ~Scope()
        {
            LARGE_INTEGER ended{};
            QueryPerformanceCounter(&ended);
            const auto us = static_cast<std::uint64_t>(
                (ended.QuadPart - started_.QuadPart) * 1000000LL / Frequency());
            stats_.Add(us);
            if (us >= slowLimitUs_)
            {
                Msg("[SOUND_PROFILE_SLOW] tag=%s ms=%.3f frame=%u tid=%lu qpc_start=%lld qpc_end=%lld detail=%s",
                    tag_, us / 1000.0, frame_,
                    static_cast<unsigned long>(GetCurrentThreadId()),
                    static_cast<long long>(started_.QuadPart),
                    static_cast<long long>(ended.QuadPart), detail_);
            }
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    struct CacheStats
    {
        std::atomic<std::uint64_t> hits{0};
        std::atomic<std::uint64_t> misses{0};

        void Hit() { hits.fetch_add(1, std::memory_order_relaxed); }
        void Miss() { misses.fetch_add(1, std::memory_order_relaxed); }

        void Report(const char* name) const
        {
            const auto h = hits.load(std::memory_order_relaxed);
            const auto m = misses.load(std::memory_order_relaxed);
            const auto total = h + m;
            Msg("[SOUND_CACHE] name=%s hits=%llu misses=%llu hit_rate=%.2f%%",
                name, static_cast<unsigned long long>(h),
                static_cast<unsigned long long>(m),
                total ? 100.0 * static_cast<double>(h) / total : 0.0);
        }
    };

#else
    // Keep call sites available without timers, atomics, or diagnostic logging.
    struct Stats
    {
        void Add(std::uint64_t) {}
        void Report(const char*) const {}
    };

    class Scope
    {
    public:
        Scope(Stats&, const char*, const char* = "", std::uint32_t = 0,
              std::uint64_t = 2000) {}
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    struct CacheStats
    {
        void Hit() {}
        void Miss() {}
        void Report(const char*) const {}
    };
#endif
    // One instance per DLL for each function-local static (C++17 inline function).
    // Use Player* and Collections only from xrGame; Wave* and Sources only from xrSound.
    inline Stats& PlayerAdd() { static Stats value; return value; }
    inline Stats& WaveLoad() { static Stats value; return value; }
    inline CacheStats& PlayerEntries() { static CacheStats value; return value; }
    inline CacheStats& Collections() { static CacheStats value; return value; }
    inline CacheStats& Sources() { static CacheStats value; return value; }
}
