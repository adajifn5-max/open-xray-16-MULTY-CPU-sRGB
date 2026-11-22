#pragma once
#include "Sound.h"
#include "xrCore/xrstring.h"

struct SoundDataInfo;
struct SoundSourceInfo;

// Called on the loading thread, before spawn. Readers use immutable RAM data.
XRSOUND_API void SoundCatalogPrepareGame();
XRSOUND_API void SoundCatalogPrepareLevel();
XRSOUND_API bool SoundCatalogResolve(pcstr name, xr_string& fullPath);
XRSOUND_API bool SoundCatalogLoadMetadata(pcstr fullPath, bool floatPCM,
    SoundDataInfo& data, SoundSourceInfo& info, u32& bytes, float& seconds);
XRSOUND_API void SoundCatalogCollectVariants(pcstr playerPrefix, pcstr prefixes,
    u32 maxCount, xr_vector<shared_str>& names);

// Lua path semantics: base wins, otherwise 1..first missing; only $game_sounds$.
// false means not prepared (caller may use legacy FS); true includes an empty list.
XRSOUND_API bool SoundCatalogCollectLuaPaths(pcstr playerPrefix, pcstr path,
    xr_vector<shared_str>& names);
