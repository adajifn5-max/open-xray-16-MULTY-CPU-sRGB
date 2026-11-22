#pragma once

#ifndef XRAY_ENABLE_STUTTER_TIMERS
#define XRAY_ENABLE_STUTTER_TIMERS 0
#endif

#if XRAY_ENABLE_STUTTER_TIMERS
#include <atomic>
#include <cstdint>

namespace StutterProfile
{
enum class Category : std::uint32_t { Spawn, Sound, Relcase, Wait, Phase, Frame, Task, Count };
constexpr std::uint32_t AllCategories = (1u << static_cast<unsigned>(Category::Count)) - 1;
constexpr std::uint32_t FocusMask = (1u << unsigned(Category::Spawn)) | (1u << unsigned(Category::Wait)) |
    (1u << unsigned(Category::Phase)) | (1u << unsigned(Category::Task));
constexpr std::uint32_t UnknownWorker = ~std::uint32_t(0);
extern XRCORE_API std::atomic<std::uint32_t> ActiveMask;

inline bool Enabled(Category category) noexcept
{
    return (ActiveMask.load(std::memory_order_acquire) & (1u << static_cast<unsigned>(category))) != 0;
}

struct Status
{
    std::uint64_t session;
    std::uint32_t mask, selectedMask, captured, capacity, dropped;
    std::uint32_t thresholdsUs[static_cast<unsigned>(Category::Count)];
};

XRCORE_API bool Start(); // Starts a fresh session, discarding the previous buffer.
XRCORE_API void Stop() noexcept;
XRCORE_API bool Clear(); // Only while stopped.
XRCORE_API void SetMask(std::uint32_t mask) noexcept;
XRCORE_API void SetThreshold(Category category, std::uint32_t microseconds) noexcept;
XRCORE_API void SetFrame(std::uint32_t frame) noexcept;
XRCORE_API Status GetStatus();
XRCORE_API bool Dump(const char* path); // Stops recording; writes only on explicit request.


XRCORE_API void MarkPresent(std::uint32_t frame) noexcept;
XRCORE_API bool InNpcSpawn() noexcept;
class XRCORE_API NpcSpawnContext
{
public:
    explicit NpcSpawnContext(bool enabled) noexcept;
    ~NpcSpawnContext() noexcept;
private:
    bool previous_;
};
struct SoundInitTotals
{
    std::uint64_t registerUs = 0, pathsUs = 0;
    std::uint32_t registers = 0, paths = 0, catalogQueries = 0, catalogReady = 0;
};
class XRCORE_API SoundInitAccumulator
{
public:
    SoundInitAccumulator() noexcept;
    ~SoundInitAccumulator() noexcept;
    const SoundInitTotals& Totals() const noexcept { return totals_; }
private:
    SoundInitTotals totals_;
    SoundInitTotals* previous_;
};
XRCORE_API bool SoundInitActive() noexcept;
XRCORE_API void NoteSoundCatalog(bool ready) noexcept;
class XRCORE_API SoundInitPart
{
public:
    explicit SoundInitPart(bool registration) noexcept;
    ~SoundInitPart() noexcept;
private:
    SoundInitTotals* totals_;
    std::uint64_t started_;
    bool registration_;
};

class Scope
{
public:
    Scope() noexcept = default;
    ~Scope() noexcept { if (id_) End(); }
    void AsPoint() noexcept { point_ = true; }
    XRCORE_API void SetDetail(const char* detail) noexcept;
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    XRCORE_API void Begin(Category category, const char* name, const char* detail = "",
        std::uint64_t subject = 0, std::uint64_t related = 0,
        std::uint32_t worker = UnknownWorker) noexcept;

private:
    XRCORE_API void End() noexcept;
    bool point_ = false;
    Scope* previous_;
    std::uint64_t id_ = 0, parent_, started_, generation_;
    std::uint64_t subject_, related_;
    std::uint32_t frame_, worker_;
    Category category_;
    const char* name_;
    char detail_[96];
};
}

#define XR_STUTTER_JOIN_IMPL(a, b) a##b
#define XR_STUTTER_JOIN(a, b) XR_STUTTER_JOIN_IMPL(a, b)
#define XR_STUTTER_SCOPE(category, ...) \
    ::StutterProfile::Scope XR_STUTTER_JOIN(stutterScope_, __LINE__); \
    if (::StutterProfile::Enabled(::StutterProfile::Category::category)) \
        XR_STUTTER_JOIN(stutterScope_, __LINE__).Begin(::StutterProfile::Category::category, __VA_ARGS__)
#define XR_STUTTER_NPC_SCOPE(category, ...) \
    ::StutterProfile::Scope XR_STUTTER_JOIN(npcScope_, __LINE__); \
    if (::StutterProfile::Enabled(::StutterProfile::Category::category) && ::StutterProfile::InNpcSpawn()) \
        XR_STUTTER_JOIN(npcScope_, __LINE__).Begin(::StutterProfile::Category::category, __VA_ARGS__)
#define XR_STUTTER_NPC_CONTEXT(enabled) \
    ::StutterProfile::NpcSpawnContext XR_STUTTER_JOIN(npcContext_, __LINE__)( \
        (::StutterProfile::Enabled(::StutterProfile::Category::Spawn) || \
         ::StutterProfile::Enabled(::StutterProfile::Category::Phase)) && (enabled))
#define XR_STUTTER_SOUND_CATALOG(ready) ::StutterProfile::NoteSoundCatalog(ready)
#define XR_STUTTER_PRESENT_ANCHOR(frame) do { \
    if (::StutterProfile::Enabled(::StutterProfile::Category::Wait) && ((frame) & 255u) == 0) \
        ::StutterProfile::MarkPresent(frame); } while (false)
#define XR_STUTTER_SET_FRAME(frame) \
    do { if (::StutterProfile::ActiveMask.load(std::memory_order_relaxed)) \
        ::StutterProfile::SetFrame(frame); } while (false)
#else
// Arguments are not evaluated, and no timer objects or calls remain.
#define XR_STUTTER_SCOPE(...) ((void)0)
#define XR_STUTTER_NPC_SCOPE(...) ((void)0)
#define XR_STUTTER_NPC_CONTEXT(...) ((void)0)
#define XR_STUTTER_SOUND_CATALOG(...) ((void)0)
#define XR_STUTTER_PRESENT_ANCHOR(...) ((void)0)
#define XR_STUTTER_SET_FRAME(...) ((void)0)
#endif

// Retired diagnostics: arguments, clocks and scope objects are compiled out.
#define XR_STUTTER_DISABLED_SCOPE(...) ((void)0)
