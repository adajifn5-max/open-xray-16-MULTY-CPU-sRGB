#include "stdafx.h"
#include "xrCore/StutterProfile.h"
#include "r2.h"
#include "Layers/xrRender/ShaderResourceTraits.h"
#include "xrCore/FileCRC32.h"
#include <cstring>
#include <cctype>
#include <cstdint>
#include "Layers/xrRender/ShaderCompileTasks.h"
#include "Layers/xrRender/ShaderCompileCPU.h"
#include "xrCore/Threading/TaskManager.hpp"

namespace xray::render::RENDER_NAMESPACE
{
void CRender::addShaderOption(const char* name, const char* value)
{
    D3D_SHADER_MACRO macro = {name, value};
    m_ShaderOptions.push_back(macro);
}

template <typename T>
static HRESULT create_shader(DWORD const* buffer, size_t const buffer_size, LPCSTR const file_name,
    T*& result, bool const dx9compatibility)
{
    XR_STUTTER_DISABLED_SCOPE(Phase, "render.shader_hw", file_name);
    HRESULT _hr = ShaderTypeTraits<T>::CreateHWShader(buffer, buffer_size, result->sh);
    if (!SUCCEEDED(_hr))
    {
        Log("! Shader: ", file_name);
        Msg("! CreateHWShader hr == 0x%08x", _hr);
        return E_FAIL;
    }

#ifdef DEBUG
    if (result->sh)
    {
        result->sh->SetPrivateData(WKPDID_D3DDebugObjectName, xr_strlen(file_name), file_name);
    }
#endif

    ID3DShaderReflection* pReflection = 0;
    _hr = D3DReflect(buffer, buffer_size, IID_ID3DShaderReflection, (void**)&pReflection);

    if (SUCCEEDED(_hr) && pReflection)
    {
        result->constants.dx9compatibility = dx9compatibility;
        // Parse constant table data
        result->constants.parse(pReflection, ShaderTypeTraits<T>::GetShaderDest());

        _RELEASE(pReflection);
    }
    else
    {
        Msg("! D3DReflectShader %s hr == 0x%08x", file_name, _hr);
    }

    return _hr;
}

static HRESULT create_shader(LPCSTR const pTarget, DWORD const* buffer, size_t const buffer_size, LPCSTR const file_name,
    void*& result, bool const disasm, bool dx9compatibility)
{
    HRESULT _result = E_FAIL;
    pcstr extension = ".hlsl";
    if (pTarget[0] == 'p')
    {
        extension = ".ps";
        _result = create_shader(buffer, buffer_size, file_name, (SPS*&)result, dx9compatibility);
    }
    else if (pTarget[0] == 'v')
    {
        extension = ".vs";
        SVS* svs_result = (SVS*)result;
        _result = create_shader(buffer, buffer_size, file_name, svs_result, dx9compatibility);
        if (SUCCEEDED(_result))
        {
            //	Store input signature (need only for VS)
            ID3DBlob* pSignatureBlob;
            CHK_DX(D3DGetInputSignatureBlob(buffer, buffer_size, &pSignatureBlob));
            VERIFY(pSignatureBlob);

            svs_result->signature = RImplementation.Resources->_CreateInputSignature(pSignatureBlob);

            _RELEASE(pSignatureBlob);
        }
    }
    else if (pTarget[0] == 'g')
    {
        extension = ".gs";
        _result = create_shader(buffer, buffer_size, file_name, (SGS*&)result, dx9compatibility);
    }
    else if (pTarget[0] == 'c')
    {
        extension = ".cs";
        _result = create_shader(buffer, buffer_size, file_name, (SCS*&)result, dx9compatibility);
    }
    else if (pTarget[0] == 'h')
    {
        extension = ".hs";
        _result = create_shader(buffer, buffer_size, file_name, (SHS*&)result, dx9compatibility);
    }
    else if (pTarget[0] == 'd')
    {
        extension = ".ds";
        _result = create_shader(buffer, buffer_size, file_name, (SDS*&)result, dx9compatibility);
    }
    else
    {
        NODEFAULT;
    }

    if (disasm)
    {
        ID3DBlob* disasm = 0;
        D3DDisassemble(buffer, buffer_size, FALSE, 0, &disasm);
        if (!disasm)
            return _result;

        string_path dname;
        strconcat(sizeof(dname), dname, "disasm" DELIMITER, file_name, extension);
        IWriter* W = FS.w_open("$app_data_root$", dname);
        W->w(disasm->GetBufferPointer(), disasm->GetBufferSize());
        FS.w_close(W);
        _RELEASE(disasm);
    }

    return _result;
}

class includer : public ID3DInclude
{
public:
    HRESULT __stdcall Open(
        D3D10_INCLUDE_TYPE IncludeType, LPCSTR pFileName, LPCVOID pParentData, LPCVOID* ppData, UINT* pBytes)
    {
        string_path pname;
        strconcat(pname, RImplementation.getShaderPath(), pFileName);
        IReader* R = FS.r_open("$game_shaders$", pname);
        if (nullptr == R)
        {
            // possibly in shared directory or somewhere else - open directly
            R = FS.r_open("$game_shaders$", pFileName);
            if (nullptr == R)
                return E_FAIL;
        }

        // duplicate and zero-terminate
        const size_t size = R->length();
        u8* data = xr_alloc<u8>(size + 1);
        CopyMemory(data, R->pointer(), size);
        data[size] = 0;
        FS.r_close(R);

        *ppData = data;
        *pBytes = size;
        return D3D_OK;
    }
    HRESULT __stdcall Close(LPCVOID pData)
    {
        auto mutableData = const_cast<LPVOID>(pData);
        xr_free(mutableData);
        return D3D_OK;
    }
};

namespace ShaderCompileTasks
{
struct Job
{
    std::string key;
    std::shared_ptr<const Request> request;
    Output output;
    Task* task = nullptr;
};
struct BatchState
{
    bool preparing = false;
    std::vector<std::shared_ptr<Job>> jobs;
    Includes files;
};
static thread_local BatchState* currentBatch = nullptr;

bool Preparing() { return currentBatch && currentBatch->preparing; }

static void Join(Job& job)
{
    if (!job.task) return;
    XR_STUTTER_SCOPE(Wait, "render.shader_cpu.wait", job.request->name.c_str(),
        reinterpret_cast<std::uintptr_t>(&job), reinterpret_cast<std::uintptr_t>(job.task));
    TaskScheduler->Wait(*job.task);
    job.task = nullptr;
}

Batch::Batch(const char* vs, const char* ps, const char* gs, const char* hs, const char* ds)
{
    if (currentBatch || !TaskScheduler || TaskManager::GetCurrentWorkerID() != 0 ||
        TaskScheduler->GetWorkersCount() < 2 || HW.D3DCompile != &::D3DCompile ||
        strstr(Core.Params, "-no_shader_tasks"))
        return;
    auto* state = new BatchState;
    state->jobs.reserve(2);
    state_ = currentBatch = state;
    state->preparing = true;
    // No resource is created/registered in preparation mode. CPU requests retain
    // owned input, while normal CreateShader below publishes on this same thread.
    if (ps) RImplementation.Resources->_CreatePS(ps);
    if (vs) RImplementation.Resources->_CreateVS(vs);
    if (gs && state->jobs.size() < 2) RImplementation.Resources->_CreateGS(gs);
    if (hs && state->jobs.size() < 2) RImplementation.Resources->_CreateHS(hs);
    if (ds && state->jobs.size() < 2) RImplementation.Resources->_CreateDS(ds);
    state->preparing = false;
}

Batch::~Batch()
{
    if (!state_) return;
    auto* state = static_cast<BatchState*>(state_);
    for (const auto& job : state->jobs) Join(*job);
    currentBatch = nullptr;
    delete state;
}

static std::string Key(const char* path, u32 crc, u32 flags,
    const char* entry, const char* target, const D3D_SHADER_MACRO* macros)
{
    std::string key;
    const auto append = [&](const char* value)
    {
        key += value ? std::to_string(std::strlen(value)) + ":" + value : "null:";
        key += ';';
    };
    append(path); append(entry); append(target);
    key += std::to_string(crc) + ";" + std::to_string(flags) + ";";
    for (const auto* macro = macros; macro->Name; ++macro)
    {
        append(macro->Name); append(macro->Definition);
    }
    return key;
}

// Capture every literal include, including inactive conditional branches. Macro
// include names use the original synchronous compiler, rather than guessing a path.
static bool SnapshotIncludes(const Bytes& source, Includes& files)
{
    bool blockComment = false;
    std::string clean;
    for (size_t i = 0; i < source.size(); ++i)
    {
        char c = source[i];
        if (blockComment)
        {
            if (c == '*' && i + 1 < source.size() && source[i + 1] == '/')
            { blockComment = false; ++i; }
            else if (c == '\n') clean += c;
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*')
        { blockComment = true; clean += ' '; ++i; continue; }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/')
        {
            while (i < source.size() && source[i] != '\n') ++i;
            clean += '\n'; continue;
        }
        clean += c;
    }
    size_t begin = 0;
    while (begin < clean.size())
    {
        const size_t newline = clean.find('\n', begin);
        const auto line = clean.substr(begin, newline == std::string::npos ? newline : newline - begin);
        begin = newline == std::string::npos ? clean.size() : newline + 1;
        size_t at = line.find_first_not_of(" \t\r");
        if (at == std::string::npos || line[at++] != '#') continue;
        at = line.find_first_not_of(" \t", at);
        if (at == std::string::npos || line.compare(at, 7, "include") != 0) continue;
        at += 7;
        if (at < line.size() && (std::isalnum(static_cast<unsigned char>(line[at])) || line[at] == '_')) continue;
        at = line.find_first_not_of(" \t", at);
        if (at == std::string::npos || (line[at] != '"' && line[at] != '<')) return false;
        const char end = line[at++] == '"' ? '"' : '>';
        const size_t stop = line.find(end, at);
        if (stop == std::string::npos) return false;
        const std::string name = line.substr(at, stop - at);
        const auto key = IncludeKey(name);
        if (files.find(key) != files.end()) continue;
        if (files.size() >= 1024) return false;
        includer reader;
        LPCVOID data = nullptr;
        UINT bytes = 0;
        if (FAILED(reader.Open(D3D_INCLUDE_LOCAL, name.c_str(), nullptr, &data, &bytes)))
            continue; // D3DCompile diagnoses an actually used missing include.
        auto owned = std::make_shared<Bytes>(static_cast<const char*>(data), static_cast<const char*>(data) + bytes);
        reader.Close(data);
        owned->push_back(0);
        files.emplace(key, owned); // Insert before descending: cycles are bounded.
        if (!SnapshotIncludes(*owned, files)) return false;
    }
    return true;
}

static void Prepare(const std::string& key, const char* name, IReader* source,
    const D3D_SHADER_MACRO* macros, const char* entry, const char* target, UINT flags)
{
    if (currentBatch->jobs.size() >= 2) return;
    for (const auto& job : currentBatch->jobs) if (job->key == key) return;
    auto request = std::make_shared<Request>();
    const char* bytes = static_cast<const char*>(source->pointer());
    request->source.assign(bytes, bytes + source->length());
    for (const auto* macro = macros; macro->Name; ++macro)
    {
        request->macros.emplace_back(macro->Name, macro->Definition ? macro->Definition : "");
        request->nullDefinitions.push_back(macro->Definition == nullptr);
    }
    request->includes = currentBatch->files;
    if (!SnapshotIncludes(request->source, request->includes)) return;
    currentBatch->files = request->includes;
    request->entry = entry; request->target = target; request->name = name;
    request->flags = flags; request->compiler = HW.D3DCompile;
    auto job = std::make_shared<Job>();
    job->key = key; job->request = std::move(request);
    currentBatch->jobs.push_back(job);
    job->task = &TaskScheduler->AddTask([job]()
    {
        XR_STUTTER_SCOPE(Task, "render.shader_cpu.task", job->request->name.c_str(),
            reinterpret_cast<std::uintptr_t>(job.get()));
        CompileCPU(*job->request, job->output);
    });
}

static Job* Find(const std::string& key)
{
    if (currentBatch)
        for (const auto& job : currentBatch->jobs) if (job->key == key) return job.get();
    return nullptr;
}
} // namespace ShaderCompileTasks

class shader_name_holder
{
    size_t pos{};
    string_path name;

public:
    void append(cpcstr string)
    {
        const size_t size = xr_strlen(string);
        for (size_t i = 0; i < size; ++i)
        {
            name[pos] = string[i];
            ++pos;
        }
    }

    void append(u32 value)
    {
        name[pos] = '0' + char(value); // NOLINT
        ++pos;
    }

    void finish()
    {
        name[pos] = '\0';
    }

    pcstr c_str() const { return name; }
};

class shader_options_holder
{
    size_t pos{};
    D3D_SHADER_MACRO m_options[128];

public:
    void add(const xr_vector<D3D_SHADER_MACRO>& macros)
    {
        for (auto macro : macros)
        {
            m_options[pos] = std::move(macro);
            ++pos;
        }
    }

    void add(cpcstr name, cpcstr value)
    {
        m_options[pos] = { name, value };
        ++pos;
    }

    void finish()
    {
        m_options[pos] = { nullptr, nullptr };
    }

    D3D_SHADER_MACRO* data() { return m_options; }
};

HRESULT CRender::shader_compile(pcstr name, IReader* fs,
    pcstr pFunctionName, pcstr pTarget, u32 Flags, void*& result)
{
    XR_STUTTER_SCOPE(Phase, "render.shader_compile", name);
    /*#ifdef XR_SHADER_CACHE_DIAG
    Msg("[SHADER_CALL] name=%s entry=%s target=%s flags=0x%08X",
        name ? name : "<null>",
        pFunctionName ? pFunctionName : "<null>",
        pTarget ? pTarget : "<null>",
        Flags);
#endif*/
    shader_options_holder options;
    shader_name_holder sh_name;

    // Don't move these variables to lower scope!
    string32 c_smap;
    string32 c_gloss;
    string32 c_sun_shafts;
    string32 c_ssao;
    string32 c_sun_quality;
    char c_msaa_samples[2];
    char c_msaa_current_sample[2];

    // options:
    const auto appendShaderOption = [&](u32 option, cpcstr macro, cpcstr value)
    {
        if (option)
            options.add(macro, value);

        sh_name.append(option);
    };

    appendShaderOption(o.linear_rendering, "USE_LINEAR_RENDERING", "1");

    // External defines
    options.add(m_ShaderOptions);

    // Shadow map size
    {
        xr_itoa(m_SMAPSize, c_smap, 10);
        options.add("SMAP_size", c_smap);
        sh_name.append(c_smap);
    }

    // FP16 Filter
    appendShaderOption(o.fp16_filter, "FP16_FILTER", "1");

    // FP16 Blend
    appendShaderOption(o.fp16_blend, "FP16_BLEND", "1");

    // HW smap
    appendShaderOption(o.HW_smap, "USE_HWSMAP", "1");

    // HW smap PCF
    appendShaderOption(o.HW_smap_PCF, "USE_HWSMAP_PCF", "1");

    // Fetch4
    appendShaderOption(o.HW_smap_FETCH4, "USE_FETCH4", "1");

    // SJitter
    appendShaderOption(o.sjitter, "USE_SJITTER", "1");

    // Branching
    appendShaderOption(HW.Caps.raster_major >= 3, "USE_BRANCHING", "1");

    // Vertex texture fetch
    appendShaderOption(HW.Caps.geometry.bVTF, "USE_VTF", "1");

    // Tshadows
    appendShaderOption(o.Tshadows, "USE_TSHADOWS", "1");

    // Motion blur
    appendShaderOption(o.mblur, "USE_MBLUR", "1");

    // Sun filter
    appendShaderOption(o.sunfilter, "USE_SUNFILTER", "1");

    // Static sun on R2 and higher
    appendShaderOption(o.sunstatic, "USE_R2_STATIC_SUN", "1");

    // Force gloss
    {
        xr_sprintf(c_gloss, "%f", o.forcegloss_v);
        appendShaderOption(o.forcegloss, "FORCE_GLOSS", c_gloss);
    }

    // Force skinw
    appendShaderOption(o.forceskinw, "SKIN_COLOR", "1");

    // SSAO Blur
    appendShaderOption(o.ssao_blur_on, "USE_SSAO_BLUR", "1");

    // SSAO HDAO
    if (o.ssao_hdao)
    {
        options.add("HDAO", "1");
        sh_name.append(static_cast<u32>(1)); // HDAO on
        sh_name.append(static_cast<u32>(0)); // HBAO off
        sh_name.append(static_cast<u32>(0)); // Half data off
    }
    else // SSAO HBAO
    {
        sh_name.append(static_cast<u32>(0)); // HDAO off
        sh_name.append(o.ssao_hbao);         // HBAO on/off
        sh_name.append(o.ssao_half_data);    // Half data on/off

        if (o.ssao_hbao)
        {
            if (o.ssao_half_data)
                options.add("SSAO_OPT_DATA", "2");
            else
                options.add("SSAO_OPT_DATA", "1");

            if (o.hbao_vectorized)
                options.add("VECTORIZED_CODE", "1");

            options.add("USE_HBAO", "1");
        }
    }

    // skinning
    // SKIN_NONE
    appendShaderOption(m_skinning < 0, "SKIN_NONE", "1");

    // SKIN_0
    appendShaderOption(0 == m_skinning, "SKIN_0", "1");

    // SKIN_1
    appendShaderOption(1 == m_skinning, "SKIN_1", "1");

    // SKIN_2
    appendShaderOption(2 == m_skinning, "SKIN_2", "1");

    // SKIN_3
    appendShaderOption(3 == m_skinning, "SKIN_3", "1");

    // SKIN_4
    appendShaderOption(4 == m_skinning, "SKIN_4", "1");

    //	Igor: need restart options
    // Soft water
    {
        const bool softWater = RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_SOFT_WATER);
        appendShaderOption(softWater, "USE_SOFT_WATER", "1");
    }

    // Soft particles
    {
        const bool useSoftParticles = RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_SOFT_PARTICLES);
        appendShaderOption(useSoftParticles, "USE_SOFT_PARTICLES", "1");
    }

    // Depth of field
    {
        const bool dof = RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_DOF);
        appendShaderOption(dof, "USE_DOF", "1");
    }

    // Sun shafts
    if (RImplementation.o.advancedpp && ps_r_sun_shafts)
    {
        xr_sprintf(c_sun_shafts, "%d", ps_r_sun_shafts);
        options.add("SUN_SHAFTS_QUALITY", c_sun_shafts);
        sh_name.append(ps_r_sun_shafts);
    }
    else
        sh_name.append(static_cast<u32>(0));

    if (RImplementation.o.advancedpp && ps_r_ssao)
    {
        xr_sprintf(c_ssao, "%d", ps_r_ssao);
        options.add("SSAO_QUALITY", c_ssao);
        sh_name.append(ps_r_ssao);
    }
    else
        sh_name.append(static_cast<u32>(0));

    // Sun quality
    if (RImplementation.o.advancedpp && ps_r_sun_quality)
    {
        xr_sprintf(c_sun_quality, "%d", ps_r_sun_quality);
        options.add("SUN_QUALITY", c_sun_quality);
        sh_name.append(ps_r_sun_quality);
    }
    else
        sh_name.append(static_cast<u32>(0));

    // Steep parallax
    {
        const bool steepParallax = RImplementation.o.advancedpp && ps_r2_ls_flags.test(R2FLAG_STEEP_PARALLAX);
        appendShaderOption(steepParallax, "ALLOW_STEEPPARALLAX", "1");
    }

    // Geometry buffer optimization
    appendShaderOption(o.gbuffer_opt, "GBUFFER_OPTIMIZATION", "1");

    // Shader Model 4.1
    appendShaderOption(o.dx11_sm4_1, "SM_4_1", "1");

    // Shader Model 5.0
    appendShaderOption(HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0, "SM_5", "1");

    // Double precision
    appendShaderOption(HW.DoublePrecisionFloatShaderOps, "DOUBLE_PRECISION", "1");

    // Extended doubles instructions
    appendShaderOption(HW.ExtendedDoublesShaderInstructions, "EXTENDED_DOUBLES", "1");

    // SAD4 intrinsic support
    appendShaderOption(HW.SAD4ShaderInstructions, "SAD4_SUPPORTED", "1");

    // Minmax SM
    appendShaderOption(o.minmax_sm, "USE_MINMAX_SM", "1");

    // Be carefull!!!!! this should be at the end to correctly generate
    // compiled shader name;
    // add a #define for DX10_1 MSAA support
    if (o.msaa)
    {
        appendShaderOption(o.msaa, "USE_MSAA", "1");

        // Number of samples
        {
            c_msaa_samples[0] = char(o.msaa_samples) + '0';
            c_msaa_samples[1] = 0;
            appendShaderOption(o.msaa_samples, "MSAA_SAMPLES", c_msaa_samples);
        }
        // Current sample
        {
            if (m_MSAASample < 0 || o.msaa_opt)
                c_msaa_current_sample[0] = '0';
            else
                c_msaa_current_sample[0] = '0' + char(m_MSAASample);
            c_msaa_current_sample[1] = 0;

            appendShaderOption(m_MSAASample >= 0 ? m_MSAASample : 0,
                "ISAMPLE", c_msaa_current_sample);
        }

        appendShaderOption(o.msaa_opt, "MSAA_OPTIMIZATION", "1");

        switch (o.msaa_alphatest)
        {
        case MSAA_ATEST_DX10_0_ATOC:
            options.add("MSAA_ALPHATEST_DX10_0_ATOC", "1");

            sh_name.append(static_cast<u32>(1)); // DX10_0_ATOC   on
            sh_name.append(static_cast<u32>(0)); // DX10_1_ATOC   off
            sh_name.append(static_cast<u32>(0)); // DX10_1_NATIVE off
            break;
        case MSAA_ATEST_DX10_1_ATOC:
            options.add("MSAA_ALPHATEST_DX10_1_ATOC", "1");

            sh_name.append(static_cast<u32>(0)); // DX10_0_ATOC   off
            sh_name.append(static_cast<u32>(1)); // DX10_1_ATOC   on
            sh_name.append(static_cast<u32>(0)); // DX10_1_NATIVE off
            break;
        case MSAA_ATEST_DX10_1_NATIVE:
            options.add("MSAA_ALPHATEST_DX10_1", "1");

            sh_name.append(static_cast<u32>(0)); // DX10_0_ATOC   off
            sh_name.append(static_cast<u32>(0)); // DX10_1_ATOC   off
            sh_name.append(static_cast<u32>(1)); // DX10_1_NATIVE on
            break;
        default:
            sh_name.append(static_cast<u32>(0)); // DX10_0_ATOC   off
            sh_name.append(static_cast<u32>(0)); // DX10_1_ATOC   off
            sh_name.append(static_cast<u32>(0)); // DX10_1_NATIVE off
        }
    }
    else
    {
        sh_name.append(static_cast<u32>(0)); // MSAA off
        sh_name.append(static_cast<u32>(0)); // No MSAA samples
        sh_name.append(static_cast<u32>(0)); // No MSAA optimization
        sh_name.append(static_cast<u32>(0)); // DX10_0_ATOC   off
        sh_name.append(static_cast<u32>(0)); // DX10_1_ATOC   off
        sh_name.append(static_cast<u32>(0)); // DX10_1_NATIVE off
    }

    // finish
    options.finish();
    sh_name.finish();

    /*#ifdef XR_SHADER_CACHE_DIAG
    Msg("[SHADER_OPTIONS] shader=%s",
        name ? name : "<null>");

    for (const D3D_SHADER_MACRO* macro = options.data();
         macro->Name != nullptr;
         ++macro)
    {
        Msg("  %s = %s",
            macro->Name,
            macro->Definition ? macro->Definition : "");
    }

    Msg("[SHADER_KEY] %s", sh_name.c_str());
#endif*/

    HRESULT _result = E_FAIL;

    char extension[3];
    strncpy_s(extension, pTarget, 2);

    pcstr renderer;
    if (HW.FeatureLevel >= D3D_FEATURE_LEVEL_11_0)
        renderer = "r4" DELIMITER;
    else if (HW.FeatureLevel >= D3D_FEATURE_LEVEL_10_0)
        renderer = "r3" DELIMITER;
    else
    {
        renderer = "r4_level9" DELIMITER;
        R_ASSERT(!"Feature levels lower than 10.0 are unsupported");
    }

    string_path filename;
    strconcat(sizeof(filename), filename, renderer, name, ".", extension);

    string_path file_name;
    {
        string_path file;
        strconcat(sizeof(file), file, "shaders_cache_oxr" DELIMITER, filename, DELIMITER, sh_name.c_str());
        strconcat(sizeof(filename), filename, filename, DELIMITER, sh_name.c_str());
        FS.update_path(file_name, "$app_data_root$", file);
        /*#ifdef XR_SHADER_CACHE_DIAG
    Msg("[SHADER_CACHE_KEY] name=%s cache=%s",
        name ? name : "<null>",
        file_name);
#endif*/
    }

    string_path shadersFolder;
    FS.update_path(shadersFolder, "$game_shaders$", RImplementation.getShaderPath());

    u32 fileCrc = 0;
    {
        XR_STUTTER_SCOPE(Phase, "render.shader_crc", name);
        getFileCrc32(fs, shadersFolder, fileCrc);
    }
    fs->seek(0);

    if (FS.exist(file_name))
    {
        XR_STUTTER_SCOPE(Phase, "render.shader_cache", name);
        IReader* file = FS.r_open(file_name);
        //if (file->length() > 4)

        if (file && file->length() > 3 * sizeof(u32))

        {
            const bool dx9compatibility = file->r_u32();

            u32 savedFileCrc = file->r_u32();
            if (savedFileCrc == fileCrc)
            {
                u32 savedBytecodeCrc = file->r_u32();
                u32 bytecodeCrc = crc32(file->pointer(), file->elapsed());
                if (bytecodeCrc == savedBytecodeCrc)
                {
#ifdef DEBUG
                    Log("* Loading shader:", file_name);
#endif
                    _result = ShaderCompileTasks::Preparing() ? S_OK :
                        create_shader(pTarget, (DWORD*)file->pointer(), file->elapsed(),
                            filename, result, o.disasm, dx9compatibility);
                }
            }
        }
        if (file)
        file->close();
    }

    if (FAILED(_result))
    {
        const auto requestKey = ShaderCompileTasks::Key(file_name, fileCrc, Flags, pFunctionName, pTarget, options.data());
        if (ShaderCompileTasks::Preparing())
        {
            ShaderCompileTasks::Prepare(requestKey, name, fs, options.data(), pFunctionName, pTarget, Flags);
            return S_OK;
        }
        includer Includer;
        LPD3DBLOB pShaderBuf = NULL;
        LPD3DBLOB pErrorBuf = NULL;
        auto* prepared = ShaderCompileTasks::Find(requestKey);
        if (prepared) ShaderCompileTasks::Join(*prepared);
        if (prepared && SUCCEEDED(prepared->output.status))
        {
            _result = prepared->output.status;
            Flags = prepared->output.flags;
            pShaderBuf = prepared->output.bytecode;
            pErrorBuf = prepared->output.errors;
            if (pShaderBuf) pShaderBuf->AddRef();
            if (pErrorBuf) pErrorBuf->AddRef();
        }
        else
        {
            // Preserve the original compiler/diagnostics if a mod's include
            // construct could not be captured by the conservative scanner.
            _result = [&]()
            {
                XR_STUTTER_SCOPE(Phase, "render.shader_d3dcompile", name, 1, Flags);
                return HW.D3DCompile(fs->pointer(), fs->length(), "", options.data(),
                    &Includer, pFunctionName, pTarget, Flags, 0, &pShaderBuf, &pErrorBuf);
            }();
            if (FAILED(_result) && pErrorBuf)
            {
                const char* errorText = static_cast<const char*>(pErrorBuf->GetBufferPointer());
                if (strstr(errorText, "error X3523") != nullptr)
                {
                    _RELEASE(pShaderBuf);
                    _RELEASE(pErrorBuf);
                    Flags |= D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
                    _result = [&]()
                    {
                        XR_STUTTER_SCOPE(Phase, "render.shader_retry_X3523", name, 2, Flags);
                        return HW.D3DCompile(fs->pointer(), fs->length(), "", options.data(), &Includer,
                            pFunctionName, pTarget, Flags, 0, &pShaderBuf, &pErrorBuf);
                    }();
                }
            }
        }
        if (SUCCEEDED(_result))
        {
            const bool dx9compatibility = Flags & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY;
            IWriter* file = FS.w_open(file_name);

            file->w_u32(dx9compatibility);
            file->w_u32(fileCrc);

            u32 bytecodeCrc = crc32(pShaderBuf->GetBufferPointer(), pShaderBuf->GetBufferSize());
            file->w_u32(bytecodeCrc); // Do not write anything below this line, take a look at reading (crc32)

            file->w(pShaderBuf->GetBufferPointer(), pShaderBuf->GetBufferSize());
            FS.w_close(file);
#ifdef DEBUG
            Log("- Compile shader:", file_name);
#endif
            _result = create_shader(pTarget, (DWORD*)pShaderBuf->GetBufferPointer(), pShaderBuf->GetBufferSize(),
                filename, result, o.disasm, dx9compatibility);
        }
        else
        {
            Log("! ", file_name);
            if (pErrorBuf)
                Log("! error: ", (LPCSTR)pErrorBuf->GetBufferPointer());
            else
                Msg("Can't compile shader hr=0x%08x", _result);
        }
     // Звільнення буферів після використання.
        _RELEASE(pShaderBuf);
        _RELEASE(pErrorBuf);
    }


    return _result;
}
} // namespace xray::render::RENDER_NAMESPACE
