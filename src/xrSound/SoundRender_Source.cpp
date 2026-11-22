#include "stdafx.h"
#include "xrCore/StutterProfile.h"

#include "SoundRender_Core.h"
#include "SoundRender_Source.h"
#include "SoundCatalog.h"
#include "xrCore/SoundProfileLite.h" //profiler
#include <vorbis/vorbisfile.h>

CSoundRender_Source::~CSoundRender_Source() { unload(); }

namespace
{
bool ov_can_continue_read(long res)
{
    switch (res)
    {
    case 0:
        return false;
    // info
    case OV_HOLE:
        Msg("Vorbisfile encoutered missing or corrupt data in the bitstream. Recovery is normally automatic and this "
            "return code is for informational purposes only.");
        return true;
    case OV_EBADLINK:
        Msg("The given link exists in the Vorbis data stream, but is not decipherable due to garbage or corruption.");
        return true;
    // error
    case OV_FALSE: Msg("Not true, or no data available"); return false;
    case OV_EREAD: Msg("Read error while fetching compressed data for decode"); return false;
    case OV_EFAULT: Msg("Internal inconsistency in decode state. Continuing is likely not possible."); return false;
    case OV_EIMPL: Msg("Feature not implemented"); return false;
    case OV_EINVAL:
        Msg("Either an invalid argument, or incompletely initialized argument passed to libvorbisfile call");
        return false;
    case OV_ENOTVORBIS: Msg("The given file/data was not recognized as Ogg Vorbis data."); return false;
    case OV_EBADHEADER:
        Msg("The file/data is apparently an Ogg Vorbis stream, but contains a corrupted or undecipherable header.");
        return false;
    case OV_EVERSION: Msg("The bitstream format revision of the given stream is not supported."); return false;
    case OV_ENOSEEK: Msg("The given stream is not seekable"); return false;
    }
    return false;
}
}

void CSoundRender_Source::decompress(void* dest, u32 byte_offset, u32 size, OggVorbis_File* ovf) const
{
    ZoneScoped;

    // seek
    const auto sample_offset = ogg_int64_t(byte_offset / m_data_info.blockAlign);
    const u32 cur_pos = u32(ov_pcm_tell(ovf));
    if (cur_pos != sample_offset)
        ov_pcm_seek(ovf, sample_offset);

    // decompress
    if (m_data_info.format == SoundFormat::Float32)
        i_decompress(ovf, static_cast<float*>(dest), size);
    else
        i_decompress(ovf, static_cast<char*>(dest), size);
}

void CSoundRender_Source::i_decompress(OggVorbis_File* ovf, char* _dest, u32 size) const
{
    long TotalRet = 0;

    // Read loop
    while (TotalRet < static_cast<long>(size))
    {
        const auto ret = ov_read(ovf, _dest + TotalRet, size - TotalRet, 0, 2, 1, nullptr);
        if (ret <= 0 && !ov_can_continue_read(ret))
            break;
        TotalRet += ret;
    }
}

void CSoundRender_Source::i_decompress(OggVorbis_File* ovf, float* _dest, u32 size) const
{
    s32 left = s32(size / m_data_info.blockAlign);
    while (left)
    {
        float** pcm;
        long samples = ov_read_float(ovf, &pcm, left, nullptr);

        if (samples <= 0 && !ov_can_continue_read(samples))
            break;

        if (samples > left)
            samples = left;

        for (long j = 0; j < samples; j++)
            for (long i = 0; i < m_data_info.channels; i++)
                *_dest++ = clampr(pcm[i][j], -1.0f, 1.0f);

        left -= samples;
    }
}

constexpr ov_callbacks g_ov_callbacks =
{
    // read
    [](void* ptr, size_t size, size_t nmemb, void* datasource) -> size_t
    {
        auto* file = static_cast<IReader*>(datasource);
        const size_t exist_block = _max(0ul, iFloor(file->elapsed() / (float)size));
        const size_t read_block = std::min(exist_block, nmemb);
        file->r(ptr, read_block * size);
        return read_block;
    },
    // seek
    [](void* datasource, ogg_int64_t offset, int whence) -> int
    {
        //SEEK_SET 0 File beginning
        //SEEK_CUR 1 Current file pointer position
        //SEEK_END 2 End-of-file
        switch (whence)
        {
        case SEEK_SET: ((IReader*)datasource)->seek((int)offset); break;
        case SEEK_CUR: ((IReader*)datasource)->advance((int)offset); break;
        case SEEK_END: ((IReader*)datasource)->seek((int)offset + ((IReader*)datasource)->length()); break;
        }
        return 0;
    },
    // close
    [](void* datasource) -> int
    {
        auto* file = static_cast<IReader*>(datasource);
        FS.r_close(file);
        return 0;
    },
    // tell
    [](void* datasource) -> long
    {
        const auto file = static_cast<IReader*>(datasource);
        return static_cast<long>(file->tell());
    },
};

OggVorbis_File* CSoundRender_Source::open() const
{
    const auto file = FS.r_open(pname.c_str());
    R_ASSERT3(file && file->length(), "Can't open wave file:", pname.c_str());

    const auto ovf = xr_new<OggVorbis_File>();
    ov_open_callbacks(file, ovf, nullptr, 0, g_ov_callbacks);

    return ovf;
}

void CSoundRender_Source::close(OggVorbis_File*& ovf) const
{
    ov_clear(ovf);
    xr_delete(ovf);
}

bool CSoundRender_Source::LoadWave(pcstr pName)
{
    XR_STUTTER_DISABLED_SCOPE(Sound, "CSoundRender_Source::LoadWave", pName);
    pname = pName;
    return SoundCatalogLoadMetadata(pName, SoundRender->supports_float_pcm,
        m_data_info, m_info, dwBytesTotal, fTimeTotal);
}

bool CSoundRender_Source::load(pcstr name)
{
    string_path fn, N;
    xr_strcpy(N, name);
#ifdef XR_PLATFORM_WINDOWS
    xr_strlwr(N);
#endif

    if (strext(N))
        *strext(N) = 0;

    fname = N;

    xr_string resolved;
    if (!SoundCatalogResolve(N, resolved))
    {
#ifdef _EDITOR
        if (!SoundCatalogResolve("$no_sound", resolved))
#endif
            return false;
    }
    return LoadWave(resolved.c_str());
}

void CSoundRender_Source::unload()
{
    fTimeTotal = 0.0f;
    dwBytesTotal = 0;
}
