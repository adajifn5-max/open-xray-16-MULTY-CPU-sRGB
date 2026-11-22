#pragma once

#include <map>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xray::render::RENDER_NAMESPACE::ShaderCompileTasks
{
using Bytes = std::vector<char>;
using Includes = std::map<std::string, std::shared_ptr<const Bytes>>;

inline std::string IncludeKey(std::string name)
{
    for (char& c : name)
    {
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    return name;
}

struct Request
{
    Bytes source;
    std::vector<std::pair<std::string, std::string>> macros;
    std::vector<bool> nullDefinitions;
    Includes includes;
    std::string entry, target, name;
    UINT flags = 0;
    decltype(&::D3DCompile) compiler = nullptr;
};

struct Output
{
    HRESULT status = E_FAIL;
    UINT flags = 0;
    ID3DBlob* bytecode = nullptr;
    ID3DBlob* errors = nullptr;
    ~Output()
    {
        if (bytecode) bytecode->Release();
        if (errors) errors->Release();
    }
    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
};

class SnapshotInclude final : public ID3DInclude
{
    const Includes& files_;
public:
    explicit SnapshotInclude(const Includes& files) : files_(files) {}
    HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* size) override
    {
        const auto found = files_.find(IncludeKey(name));
        if (found == files_.end()) return E_FAIL;
        *data = found->second->data();
        *size = static_cast<UINT>(found->second->size() - 1); // owned trailing zero
        return S_OK;
    }
    HRESULT __stdcall Close(LPCVOID) override { return S_OK; }
};

// No FS, renderer state, GPU calls, resource maps or logging in this function.
inline void CompileCPU(const Request& request, Output& output)
{
    std::vector<D3D_SHADER_MACRO> macros;
    macros.reserve(request.macros.size() + 1);
    for (size_t i = 0; i < request.macros.size(); ++i)
        macros.push_back({request.macros[i].first.c_str(),
            request.nullDefinitions[i] ? nullptr : request.macros[i].second.c_str()});
    macros.push_back({nullptr, nullptr});
    SnapshotInclude includes(request.includes);
    output.flags = request.flags;
    const auto compile = [&]()
    {
        return request.compiler(request.source.data(), request.source.size(), "", macros.data(), &includes,
            request.entry.c_str(), request.target.c_str(), output.flags, 0, &output.bytecode, &output.errors);
    };
    output.status = compile();
    if (FAILED(output.status) && output.errors &&
        std::strstr(static_cast<const char*>(output.errors->GetBufferPointer()), "error X3523"))
    {
        if (output.bytecode) { output.bytecode->Release(); output.bytecode = nullptr; }
        output.errors->Release(); output.errors = nullptr;
        output.flags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
        output.status = compile();
    }
}
}
