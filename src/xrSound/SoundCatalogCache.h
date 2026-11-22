#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace SoundCatalogCache
{
struct Record
{
    std::uint64_t fingerprint = 0, samples = 0;
    std::uint32_t rate = 0, channels = 0, gameType = 0;
    float volume = 1.f, minDist = 1.f, maxDist = 300.f, maxAIDist = 300.f;
    std::uint8_t valid = 0;
};
using Records = std::unordered_map<std::string, Record>;
inline std::uint64_t Hash(const void* bytes, size_t count, std::uint64_t h = 14695981039346656037ull)
{
    const auto* p = static_cast<const unsigned char*>(bytes);
    for (size_t i = 0; i < count; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}
inline bool ReadGameComment(const char* p, size_t length, Record& r)
{
    if (!p || length < 4) return false;
    std::uint32_t version;std::memcpy(&version,p,4);
    if (!((version==1 && length>=16)||(version==2 && length>=20)||(version==3 && length>=24))) return false;
    std::memcpy(&r.minDist,p+4,4);std::memcpy(&r.maxDist,p+8,4);r.maxAIDist=r.maxDist;
    if(version==1) std::memcpy(&r.gameType,p+12,4);
    else
    {
        std::memcpy(&r.volume,p+12,4);std::memcpy(&r.gameType,p+16,4);
        if(version==3) std::memcpy(&r.maxAIDist,p+20,4);
    }
    return true;
}
inline bool Valid(const Record& r)
{
    return r.valid <= 1 && (!r.valid ||
        (r.rate > 0 && r.rate <= 1000000 && r.channels > 0 && r.channels <= 16383 &&
         std::uint64_t(r.rate)*r.channels*4 <= 0xffffffffull &&
         r.samples > 0 && r.samples <= 0xffffffffull / (r.channels * 4ull) &&
         std::isfinite(r.volume) && std::isfinite(r.minDist) &&
         std::isfinite(r.maxDist) && std::isfinite(r.maxAIDist) &&
         r.maxDist >= .1f && r.maxAIDist >= .1f));
}
template<class T> inline void Put(std::ostream& s, const T& v) { s.write(reinterpret_cast<const char*>(&v), sizeof(v)); }
template<class T> inline bool Get(std::istream& s, T& v) { return bool(s.read(reinterpret_cast<char*>(&v), sizeof(v))); }
inline bool Write(std::ostream& out, const Records& records)
{
    std::ostringstream data(std::ios::out | std::ios::binary);
    if (records.size() > 1000000) return false;
    Put(data, std::uint32_t(records.size()));
    for (const auto& [name,r] : records)
    {
        if (name.empty() || name.size() > 4096 || !Valid(r)) return false;
        Put(data, std::uint32_t(name.size())); data.write(name.data(), name.size());
        Put(data,r.fingerprint);Put(data,r.samples);Put(data,r.rate);Put(data,r.channels);
        Put(data,r.gameType);Put(data,r.volume);Put(data,r.minDist);Put(data,r.maxDist);Put(data,r.maxAIDist);Put(data,r.valid);
    }
    const auto bytes=data.str();
    out.write("XROGGC01",8); Put(out,std::uint32_t(1)); Put(out,std::uint64_t(bytes.size())); Put(out,Hash(bytes.data(),bytes.size()));
    out.write(bytes.data(),bytes.size()); return bool(out);
}
inline bool Read(std::istream& in, Records& result)
{
    char magic[8];std::uint32_t version;std::uint64_t size,hash;
    if (!in.read(magic,8) || std::memcmp(magic,"XROGGC01",8) || !Get(in,version) || version!=1 ||
        !Get(in,size) || size<4 || size>128*1024*1024 || !Get(in,hash)) return false;
    std::string bytes(size,'\0');
    if (!in.read(bytes.data(),size) || in.peek()!=std::char_traits<char>::eof() || Hash(bytes.data(),bytes.size())!=hash) return false;
    std::istringstream data(bytes,std::ios::in|std::ios::binary);std::uint32_t count;
    if (!Get(data,count) || count>1000000 || count>(size-4)/50) return false;
    Records parsed;
    for (std::uint32_t i=0;i<count;++i)
    {
        std::uint32_t len;Record r;
        if (!Get(data,len) || !len || len>4096) return false;
        std::string name(len,'\0');
        if (!data.read(name.data(),len) || name.find('\0')!=std::string::npos ||
            !Get(data,r.fingerprint)||!Get(data,r.samples)||!Get(data,r.rate)||!Get(data,r.channels)||
            !Get(data,r.gameType)||!Get(data,r.volume)||!Get(data,r.minDist)||!Get(data,r.maxDist)||!Get(data,r.maxAIDist)||!Get(data,r.valid)||!Valid(r)||
            !parsed.emplace(name,r).second) return false;
    }
    if (data.peek()!=std::char_traits<char>::eof()) return false;
    result=std::move(parsed);return true;
}
}
