#pragma once

#if defined(USE_DX11)
namespace xray::render::RENDER_NAMESPACE::ShaderCompileTasks
{
// A bounded CPU-only wave. All tasks are joined before the pass leaves its owner
// thread, including errors and unused speculative variants.
class Batch
{
    void* state_ = nullptr;
public:
    Batch(const char* vs, const char* ps, const char* gs = nullptr,
        const char* hs = nullptr, const char* ds = nullptr);
    ~Batch();
    Batch(const Batch&) = delete;
    Batch& operator=(const Batch&) = delete;
};
bool Preparing();
}
#endif
