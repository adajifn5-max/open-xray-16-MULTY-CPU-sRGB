#include "stdafx.h"
#include "SoundCatalog.h"
#include "SoundCatalogCache.h"
#include "SoundRender.h"
#include "SoundRender_Source.h"
#include "xrCore/LocatorAPI.h"
#include "xrCore/StutterProfile.h"
#include "xrCore/SoundProfileLite.h"
#include <vorbis/vorbisfile.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <limits>
#include <cstdio>
#if defined(XR_PLATFORM_WINDOWS)
#include <Windows.h>
#endif

namespace
{
using namespace SoundCatalogCache;
struct Snapshot { Records records; std::string gameRoot, levelRoot; };
std::atomic<std::shared_ptr<const Snapshot>> published;
std::shared_ptr<const Snapshot> gameCatalog;
Records diskCache;
std::mutex prepareMutex;
std::string cacheRoot;

std::string Normalize(pcstr path)
{
    std::string s(path ? path : "");
    for (auto& c:s)
    {
        if(c=='/') c='\\';
#if defined(XR_PLATFORM_WINDOWS)
        if(c>='A' && c<='Z') c=char(c-'A'+'a');
#endif
    }
    return s;
}
std::string Root(pcstr alias)
{
    FS_Path* path=nullptr;
    return FS.get_path(alias,&path) && path && path->m_Path ? Normalize(path->m_Path) : std::string();
}
std::uint64_t Fingerprint(const std::string& path, const FS_File& f)
{
    auto h=Hash(path.data(),path.size());
    h=Hash(&f.size,sizeof(f.size),h);h=Hash(&f.time_write,sizeof(f.time_write),h);
    if (const auto* d=FS.GetFileDesc(path.c_str()))
    {
        h=Hash(&d->crc,sizeof(d->crc),h);h=Hash(&d->ptr,sizeof(d->ptr),h);
        h=Hash(&d->size_compressed,sizeof(d->size_compressed),h);
        if(d->vfs<FS.m_archives.size())
        {
            const auto& ar=FS.m_archives[d->vfs];
            if(ar.path.c_str()) h=Hash(ar.path.c_str(),xr_strlen(ar.path.c_str()),h);
            h=Hash(&ar.size,sizeof(ar.size),h);h=Hash(&ar.modif,sizeof(ar.modif),h);
        }
        else
        {
            std::error_code ec;
            const auto stamp=std::filesystem::last_write_time(path,ec);
            if(!ec) {const auto ticks=stamp.time_since_epoch().count();h=Hash(&ticks,sizeof(ticks),h);}
        }
    }
    return h;
}
const ov_callbacks callbacks={
    [](void* out,size_t size,size_t count,void* ptr)->size_t
    {
        auto* r=static_cast<IReader*>(ptr);
        if(!size) return 0;
        const size_t n=std::min(count,size_t(r->elapsed())/size);
        r->r(out,n*size);return n;
    },
    [](void* ptr,ogg_int64_t offset,int origin)->int
    {
        auto* r=static_cast<IReader*>(ptr);
        const auto base=origin==SEEK_SET?0ll:origin==SEEK_CUR?static_cast<long long>(r->tell()):
            origin==SEEK_END?static_cast<long long>(r->length()):-1ll;
        if(base<0 || offset < -base || offset>static_cast<long long>(r->length())-base) return -1;
        r->seek(int(base+offset));return 0;
    },
    [](void* ptr)->int {auto* r=static_cast<IReader*>(ptr);FS.r_close(r);return 0;},
    [](void* ptr)->long {return long(static_cast<IReader*>(ptr)->tell());}
};
Record Parse(const std::string& path,std::uint64_t fingerprint)
{
    Record r;r.fingerprint=fingerprint;
    IReader* file=FS.r_open(path.c_str());
    if(!file) return r;
    if(!file->length()) {FS.r_close(file);return r;}
    OggVorbis_File ovf{};
    const int error=ov_open_callbacks(file,&ovf,nullptr,0,callbacks);
    if(error<0) {FS.r_close(file);Msg("! SOUND_CATALOG: invalid Vorbis (%d): %s",error,path.c_str());return r;}
    const auto* info=ov_info(&ovf,-1);
    const auto samples=ov_pcm_total(&ovf,-1);
    if(info && info->rate>0 && info->channels>0 && samples>0)
    {
        r.rate=std::uint32_t(info->rate);r.channels=std::uint32_t(info->channels);r.samples=std::uint64_t(samples);
        const auto* comment=ov_comment(&ovf,-1);
        if(comment && comment->comments>0 && comment->comment_lengths[0]>=4)
        {
            ReadGameComment(comment->user_comments[0], size_t(comment->comment_lengths[0]), r);
        }
        r.valid=1;
        if(!Valid(r)) r.valid=0;
    }
    ov_clear(&ovf); // Also releases IReader through close callback.
    return r;
}
bool ReplaceFile(const std::string& temp,const std::string& target)
{
#if defined(XR_PLATFORM_WINDOWS)
    return MoveFileExA(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
    return std::rename(temp.c_str(),target.c_str())==0;
#endif
}
void Save()
{
    // Loading phase only. Never called from collection creation or LoadWave.
    const auto bin=cacheRoot+"ogg_catalog.bin";
    const auto temp=bin+".tmp";
    bool ok=false;
    {
        std::ofstream file(temp,std::ios::binary|std::ios::trunc);
        ok=file && Write(file,diskCache);file.flush();ok=ok && bool(file);
    }
    if(!ok || !ReplaceFile(temp,bin)) Msg("! SOUND_CATALOG: cannot save %s; RAM catalog remains available",bin.c_str());
#if XRAY_SOUND_DIAGNOSTICS
    const auto csv=cacheRoot+"ogg_catalog.csv";
    const auto csvTemp=csv+".tmp";
    std::ofstream view(csvTemp,std::ios::trunc);
    view<<"Path,Valid,Rate,Channels,PCMSamples,DurationSec,BaseVolume,MinDist,MaxDist,MaxAIDist,GameType,Fingerprint\n";
    for(const auto& [name,r]:diskCache)
    {
        view<<'"';for(char c:name) {if(c=='"') view<<'"';view<<c;}view<<'"';
        view<<','<<unsigned(r.valid)<<','<<r.rate<<','<<r.channels<<','<<r.samples<<','
            <<std::setprecision(9)<<(r.rate?double(r.samples)/r.rate:0.0)<<','<<r.volume<<','<<r.minDist<<','<<r.maxDist<<','<<r.maxAIDist<<','<<r.gameType<<','<<r.fingerprint<<'\n';
    }
    view.flush();ok=bool(view);view.close();
    if(!ok || !ReplaceFile(csvTemp,csv)) Msg("! SOUND_CATALOG: cannot save %s",csv.c_str());
#endif
}
void Scan(Snapshot& dest,pcstr alias)
{
    const auto root=Root(alias);if(root.empty()) return;
    FS_FileSet files;FS.file_list(files,alias,FS_ListFiles,"*.ogg");
#if XRAY_SOUND_DIAGNOSTICS
    size_t reused=0,parsed=0,invalid=0;
#endif
    for(const auto& f:files)
    {
        const auto path=root+Normalize(f.name.c_str());const auto fingerprint=Fingerprint(path,f);
        auto it=diskCache.find(path);
        if(it==diskCache.end() || it->second.fingerprint!=fingerprint)
        {
            diskCache[path]=Parse(path,fingerprint);
#if XRAY_SOUND_DIAGNOSTICS
            ++parsed;
#endif
        }
#if XRAY_SOUND_DIAGNOSTICS
        else ++reused;
#endif
        const auto& r=diskCache.at(path);
#if XRAY_SOUND_DIAGNOSTICS
        if(!r.valid) ++invalid;
#endif
        dest.records[path]=r;
    }
#if XRAY_SOUND_DIAGNOSTICS
    Msg("* SOUND_CATALOG: %s files=%zu cached=%zu parsed=%zu invalid=%zu",alias,files.size(),reused,parsed,invalid);
#endif
}
}

void SoundCatalogPrepareGame()
{
    std::lock_guard<std::mutex> lock(prepareMutex);
    if(gameCatalog) return;
    XR_STUTTER_DISABLED_SCOPE(Phase,"SoundCatalog::PrepareGame");
    cacheRoot=Root("$fs_root$");
    std::ifstream file(cacheRoot+"ogg_catalog.bin",std::ios::binary);
    if(file && !Read(file,diskCache)) {diskCache.clear();Msg("! SOUND_CATALOG: invalid cache; rebuilding");}
    file.close();
    auto next=std::make_shared<Snapshot>();next->gameRoot=Root("$game_sounds$");Scan(*next,"$game_sounds$");
    gameCatalog=next;published.store(gameCatalog);Save();
}
void SoundCatalogPrepareLevel()
{
    SoundCatalogPrepareGame();
    std::lock_guard<std::mutex> lock(prepareMutex);
    XR_STUTTER_DISABLED_SCOPE(Phase,"SoundCatalog::PrepareLevel");
    auto next=std::make_shared<Snapshot>();next->gameRoot=Root("$game_sounds$");
    // Level archives can add/override global sounds after initial game setup.
    Scan(*next,"$game_sounds$");
    gameCatalog=std::make_shared<Snapshot>(*next);
    next->levelRoot=Root("$level$");
    Scan(*next,"$level$");
    published.store(std::shared_ptr<const Snapshot>(next));Save();
}
bool SoundCatalogResolve(pcstr name,xr_string& fullPath)
{
    const auto snapshot=published.load();if(!snapshot) return false;
    auto id=Normalize(name);
    if(id.size()>=4 && id.compare(id.size()-4,4,".ogg")==0) id.resize(id.size()-4);
    id+=".ogg";
    for(const auto& prefix:{snapshot->levelRoot,snapshot->gameRoot})
    {
        if(prefix.empty()) continue;
        const auto path=prefix+id;const auto it=snapshot->records.find(path);
        if(it!=snapshot->records.end())
        {
            if(!it->second.valid) return false; // A broken level override must not select another file.
            fullPath=path.c_str();return true;
        }
    }
    return false;
}
bool SoundCatalogLoadMetadata(pcstr fullPath,bool floatPCM,SoundDataInfo& data,SoundSourceInfo& info,u32& bytes,float& seconds)
{
    const auto snapshot=published.load();
    if(!snapshot) {return false;}
    const auto it=snapshot->records.find(Normalize(fullPath));
    if(it==snapshot->records.end() || !it->second.valid) {return false;}
    const auto& r=it->second;
    data={};data.samplesPerSec=r.rate;data.channels=u16(r.channels);
    data.format=floatPCM?SoundFormat::Float32:SoundFormat::PCM;data.bitsPerSample=floatPCM?32:16;
    data.blockAlign=u16(data.bitsPerSample/8*data.channels);
    if(!data.blockAlign || std::uint64_t(r.rate)*data.blockAlign>std::numeric_limits<u32>::max()) return false;
    data.avgBytesPerSec=r.rate*data.blockAlign;
    data.bytesPerBuffer=u32(std::uint64_t(sdef_target_block)*data.avgBytesPerSec/1000);
    bytes=u32(r.samples*data.blockAlign);seconds=bytes/float(data.avgBytesPerSec);
    info={};info.baseVolume=r.volume;info.minDist=r.minDist;info.maxDist=r.maxDist;info.maxAIDist=r.maxAIDist;info.gameType=r.gameType;
    return true;
}
void SoundCatalogCollectVariants(pcstr playerPrefix,pcstr prefixes,u32 maxCount,xr_vector<shared_str>& names)
{
    names.clear();const auto snapshot=published.load();if(!snapshot) return;
    auto add=[&](const std::string& relative)
    {
        const auto suffix=Normalize((relative+".ogg").c_str());
        const auto game=snapshot->records.find(snapshot->gameRoot+suffix);
        if(game==snapshot->records.end()) return; // Match legacy game-sound candidate enumeration.
        const auto level=snapshot->levelRoot.empty()?snapshot->records.end():snapshot->records.find(snapshot->levelRoot+suffix);
        const auto selected=level!=snapshot->records.end()?level:game;
        if(selected->second.valid) names.emplace_back(relative.c_str());
    };
    const int count=_GetItemCount(prefixes);
    for(int j=0;j<count;++j)
    {
        string_path item;_GetItem(prefixes,j,item);
        const std::string base=std::string(playerPrefix)+item;add(base);
        // Preserve existing collection order: base, 0 .. maxCount-1, including gaps.
        for(u32 i=0;i<maxCount;++i) add(base+std::to_string(i));
    }
}

bool SoundCatalogCollectLuaPaths(pcstr playerPrefix, pcstr path, xr_vector<shared_str>& names)
{
    XR_STUTTER_DISABLED_SCOPE(Phase, "SoundCatalog::LuaPaths", path);
    names.clear();
    const auto snapshot = published.load();
    const bool ready = snapshot && !snapshot->gameRoot.empty();
    XR_STUTTER_SOUND_CATALOG(ready);
    if (!ready) return false;
    const std::string base = std::string(playerPrefix ? playerPrefix : "") + (path ? path : "");
    const auto exists = [&](const std::string& relative)
    {
        // Match FS:exist presence, even for an invalid OGG; playback validates metadata.
        return snapshot->records.find(snapshot->gameRoot + Normalize((relative + ".ogg").c_str())) != snapshot->records.end();
    };
    if (exists(base))
        names.emplace_back(base.c_str());
    else
        for (u32 i = 1; ; ++i)
        {
            const auto relative = base + std::to_string(i);
            if (!exists(relative)) break;
            names.emplace_back(relative.c_str());
            if (i == std::numeric_limits<u32>::max()) break;
        }
    return true;
}
