#pragma once

namespace xray::render::RENDER_NAMESPACE
{
class MeshIndexView
{
    const u8* data;
    u32 stride;
public:
    MeshIndexView(const void* ptr, u32 indexStride) : data(static_cast<const u8*>(ptr)), stride(indexStride) {}
    const void* Data() const { return data; }
    u32 operator[](size_t index) const
    {
        return stride == sizeof(u32) ? reinterpret_cast<const u32*>(data)[index]
                                     : reinterpret_cast<const u16*>(data)[index];
    }
    MeshIndexView operator+(size_t offset) const { return { data + offset * stride, stride }; }
    MeshIndexView& operator+=(size_t offset) { data += offset * stride; return *this; }
};
}
