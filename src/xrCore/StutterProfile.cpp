#include "stdafx.h"
#include "StutterProfile.h"

#if XRAY_ENABLE_STUTTER_TIMERS
#include <chrono>
#include <fstream>
#include <locale>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace StutterProfile
{
std::atomic<std::uint32_t> ActiveMask{0};
namespace
{
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t Capacity = 65536;
struct Event
{
    std::uint64_t id, parent, started, ended, tid, subject, related;
    std::uint32_t frameBegin, frameEnd, worker;
    Category category;
    char name[64], detail[96];
};
std::mutex bufferMutex;
std::vector<Event> events;
std::uint32_t count = 0;
std::atomic<std::uint32_t> selectedMask{FocusMask}, frame{0}, dropped{0};
std::atomic<std::uint64_t> generation{0}, nextId{0};
std::atomic<std::uint32_t> thresholds[]{1000, 1000, 2000, 2000, 2000, 50000, 2000};
std::uint64_t session = 0, anchor = 0;
thread_local Scope* parentScope = nullptr;
thread_local bool npcSpawn = false;
thread_local SoundInitTotals* soundTotals = nullptr;
constexpr const char* Names[]{"spawn", "sound", "relcase", "wait", "phase", "frame", "task"};

std::uint64_t Now() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
std::uint64_t ThreadId() noexcept
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}
template<std::size_t N> void Copy(char (&to)[N], const char* from) noexcept
{
    std::size_t i = 0;
    if (from) for (; i + 1 < N && from[i]; ++i) to[i] = from[i];
    to[i] = 0;
}
void CsvText(std::ostream& out, const char* text)
{
    out << '"';
    for (; *text; ++text)
    {
        if (*text == '"') out << '"';
        out << ((*text == '\r' || *text == '\n') ? ' ' : *text);
    }
    out << '"';
}
}


void MarkPresent(std::uint32_t frame) noexcept
{
    if (!Enabled(Category::Wait)) return;
    Scope point;
    point.Begin(Category::Wait, "renderer.present_anchor", "PM alignment/256", frame);
    point.AsPoint();
}
bool InNpcSpawn() noexcept { return npcSpawn; }
NpcSpawnContext::NpcSpawnContext(bool enabled) noexcept : previous_(npcSpawn) { npcSpawn = enabled; }
NpcSpawnContext::~NpcSpawnContext() noexcept { npcSpawn = previous_; }
SoundInitAccumulator::SoundInitAccumulator() noexcept : previous_(soundTotals) { soundTotals = &totals_; }
SoundInitAccumulator::~SoundInitAccumulator() noexcept { soundTotals = previous_; }
bool SoundInitActive() noexcept { return soundTotals != nullptr; }
void NoteSoundCatalog(bool ready) noexcept
{
    if (soundTotals) { ++soundTotals->catalogQueries; if (ready) ++soundTotals->catalogReady; }
}
SoundInitPart::SoundInitPart(bool registration) noexcept
    : totals_(soundTotals), started_(totals_ ? Now() : 0), registration_(registration) {}
SoundInitPart::~SoundInitPart() noexcept
{
    if (!totals_) return;
    const auto us = (Now() - started_) / 1000;
    if (registration_) { totals_->registerUs += us; ++totals_->registers; }
    else { totals_->pathsUs += us; ++totals_->paths; }
}
void Scope::SetDetail(const char* detail) noexcept { if (id_) Copy(detail_, detail); }

bool Start()
{
    Stop();
    std::lock_guard<std::mutex> lock(bufferMutex);
    generation.fetch_add(1, std::memory_order_acq_rel);
    try { events.resize(Capacity); }
    catch (const std::bad_alloc&) { return false; }
    count = 0;
    dropped.store(0, std::memory_order_relaxed);
    nextId.store(0, std::memory_order_relaxed);
    session = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    anchor = Now();
    ActiveMask.store(selectedMask.load(std::memory_order_relaxed), std::memory_order_release);
    return ActiveMask.load(std::memory_order_relaxed) != 0;
}
void Stop() noexcept { ActiveMask.store(0, std::memory_order_release); }
bool Clear()
{
    std::lock_guard<std::mutex> lock(bufferMutex);
    if (ActiveMask.load(std::memory_order_acquire)) return false;
    generation.fetch_add(1, std::memory_order_acq_rel);
    count = 0;
    dropped.store(0, std::memory_order_relaxed);
    return true;
}
void SetMask(std::uint32_t mask) noexcept
{
    mask &= FocusMask;
    selectedMask.store(mask, std::memory_order_relaxed);
    if (ActiveMask.load(std::memory_order_acquire)) ActiveMask.store(mask, std::memory_order_release);
}
void SetThreshold(Category category, std::uint32_t us) noexcept
{
    if (category < Category::Count) thresholds[static_cast<unsigned>(category)].store(us, std::memory_order_relaxed);
}
void SetFrame(std::uint32_t value) noexcept { frame.store(value, std::memory_order_relaxed); }
Status GetStatus()
{
    std::lock_guard<std::mutex> lock(bufferMutex);
    Status s{session, ActiveMask.load(), selectedMask.load(), count, Capacity, dropped.load(), {}};
    for (unsigned i = 0; i < static_cast<unsigned>(Category::Count); ++i) s.thresholdsUs[i] = thresholds[i].load();
    return s;
}
bool Dump(const char* path)
{
    Stop();
    std::lock_guard<std::mutex> lock(bufferMutex);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.imbue(std::locale::classic());
    out << "SessionUtcNs,EventId,ParentId,Category,Name,FrameBegin,FrameEnd,TID,Worker,Subject,Related,StartUs,EndUs,DurationUs,Detail\n";
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto& e = events[i];
        out << session << ',' << e.id << ',' << e.parent << ',' << Names[static_cast<unsigned>(e.category)] << ',';
        CsvText(out, e.name);
        out << ',' << e.frameBegin << ',' << e.frameEnd << ',' << e.tid << ',';
        if (e.worker != UnknownWorker) out << e.worker;
        out << ',' << e.subject << ',' << e.related << ',' << (e.started-anchor)/1000 << ','
            << (e.ended-anchor)/1000 << ',' << (e.ended-e.started)/1000 << ',';
        CsvText(out, e.detail);
        out << '\n';
    }
    out.flush();
    return out.good();
}
void Scope::Begin(Category category, const char* name, const char* detail,
    std::uint64_t subject, std::uint64_t related, std::uint32_t worker) noexcept
{
    if (!Enabled(category)) return;
    generation_ = generation.load(std::memory_order_acquire);
    previous_ = parentScope;
    parent_ = previous_ && previous_->generation_ == generation_ ? previous_->id_ : 0;
    id_ = nextId.fetch_add(1, std::memory_order_relaxed) + 1;
    category_ = category;
    name_ = name;
    subject_ = subject;
    related_ = related;
    worker_ = worker;
    frame_ = frame.load(std::memory_order_relaxed);
    Copy(detail_, detail); // Snapshot before a callback can invalidate the object/name.
    parentScope = this;
    started_ = Now();
}
void Scope::End() noexcept
{
    const auto ended = Now();
    parentScope = previous_;
    if (!Enabled(category_) || generation.load(std::memory_order_acquire) != generation_) return;
    if (!point_ && (ended-started_)/1000 < thresholds[static_cast<unsigned>(category_)].load(std::memory_order_relaxed)) return;
    // Never make a measured game/worker thread wait for the diagnostic buffer.
    std::unique_lock<std::mutex> lock(bufferMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        if (Enabled(category_) && generation.load(std::memory_order_acquire) == generation_)
            dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!Enabled(category_) || generation.load(std::memory_order_acquire) != generation_) return;
    if (count == events.size()) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
    auto& e = events[count++];
    e.id = id_; e.parent = parent_; e.started = started_; e.ended = ended;
    e.tid = ThreadId(); e.subject = subject_; e.related = related_;
    e.frameBegin = frame_; e.frameEnd = frame.load(std::memory_order_relaxed);
    e.worker = worker_; e.category = category_;
    Copy(e.name, name_); Copy(e.detail, detail_);
}
}
#endif
