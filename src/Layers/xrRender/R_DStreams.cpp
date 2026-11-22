#include "stdafx.h"
#pragma hdrstop
#include "ResourceManager.h"
#include "R_DStreams.h"

namespace xray::render::RENDER_NAMESPACE
{
//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

int rsDVB_Size = 4096 * 4; // KiB; shared geometry stream capacity
int rsDIB_Size = 512 * 4; // KiB

#ifdef XR_PLATFORM_WEB
constexpr bool DiscardOnEveryLock = true;
#else
constexpr bool DiscardOnEveryLock = false;
#endif

void _VertexStream::Create()
{
    ZoneScoped;

    RImplementation.Resources->Evict();

    mSize = rsDVB_Size * 1024;
    pVB.Create(mSize);

    mPosition = 0;
    mDiscardID = 0;

    Msg("* DVB created: %dK", mSize / 1024);
}

void _VertexStream::Destroy()
{
    pVB.Release();
    _clear();
}

void* _VertexStream::Lock(u32 vl_Count, u32 Stride, u32& vOffset)
{
#ifdef DEBUG
    PGO(Msg("PGO:VB_LOCK:%d", vl_Count));
    VERIFY(0 == dbg_lock);
    dbg_lock++;
#endif

    // Ensure there is enough space in the VB for this data
    const u32 bytes_need = vl_Count * Stride;
    R_ASSERT2((bytes_need <= mSize) && vl_Count,
        make_string("bytes_need = %d, mSize = %d, vl_Count = %d", bytes_need, mSize, vl_Count));

    // Vertex-local info
    const u32 vl_mSize = mSize / Stride;
    const u32 vl_mPosition = mPosition / Stride + 1;

    // Check if there is need to flush and perform lock
    bool bFlush = false;
    if (DiscardOnEveryLock || (vl_Count + vl_mPosition) >= vl_mSize)
    {
        // FLUSH-LOCK
        mPosition = 0;
        vOffset = 0;
        mDiscardID++;
        bFlush = true;
    }
    else
    {
        // APPEND-LOCK
        mPosition = vl_mPosition * Stride;
        vOffset = vl_mPosition;
    }
    void* pData = pVB.Map(mPosition, bytes_need, bFlush);
    VERIFY(pData);
    return pData;
}

void _VertexStream::Unlock(u32 Count, u32 Stride)
{
#ifdef DEBUG
    PGO(Msg("PGO:VB_UNLOCK:%d", Count));
    VERIFY(1 == dbg_lock);
    dbg_lock--;
#endif
    mPosition += Count * Stride;

#ifdef XR_PLATFORM_WEB
    pVB.Unmap(Count * Stride);
#else
    pVB.Unmap();
#endif
}

void _VertexStream::reset_begin()
{
    old_pVB = pVB;
    Destroy();
}
void _VertexStream::reset_end()
{
    Create();
    //old_pVB = NULL;
}

//////////////////////////////////////////////////////////////////////////
void _IndexStream::Create()
{
    //dxRenderDeviceRender::Instance().Resources->Evict();
    RImplementation.Resources->Evict();

    mSize = rsDIB_Size * 1024;
    pIB.Create(mSize);
    pIB32.Create(size_t(mSize) * 2); // same index capacity with u32 entries
    mPosition32 = 0;

    mPosition = 0;
    mDiscardID = 0;

    Msg("* DIB created: %dK", mSize / 1024);
}

void _IndexStream::Destroy()
{
    pIB.Release();
    pIB32.Release();
    _clear();
}

u16* _IndexStream::Lock(u32 Count, u32& vOffset)
{
    PGO(Msg("PGO:IB_LOCK:%d", Count));
    vOffset = 0;

    // Ensure there is enough space in the VB for this data
    R_ASSERT((2 * Count <= mSize) && Count);

    // If either user forced us to flush,
    // or there is not enough space for the index data,
    // then flush the buffer contents
    bool bFlush = false;
    if (2 * (Count + mPosition) >= mSize)
    {
        mPosition = 0; // clear position
        bFlush = true; // discard it's contens
        mDiscardID++;
    }
    void* pLockedData = pIB.Map(mPosition * sizeof(u16), Count * sizeof(u16), bFlush);
    VERIFY(pLockedData);

    vOffset = mPosition;

    return static_cast<u16*>(pLockedData);
}

void _IndexStream::Unlock(u32 RealCount)
{
    PGO(Msg("PGO:IB_UNLOCK:%d", RealCount));
    mPosition += RealCount;
#ifdef XR_PLATFORM_WEB
    pIB.Unmap(RealCount * sizeof(u16));
#else
    pIB.Unmap();
#endif
}

u32* _IndexStream::Lock32(u32 Count, u32& indexOffset)
{
    const u64 bytes = u64(Count) * sizeof(u32);
    R_ASSERT2(Count && bytes <= GetSize32(), "32-bit index stream request exceeds capacity");
    const bool discard = DiscardOnEveryLock || u64(mPosition32) + Count >= GetSize32() / sizeof(u32);
    if (discard)
        mPosition32 = 0;
    indexOffset = mPosition32;
    void* data = pIB32.Map(size_t(mPosition32) * sizeof(u32), size_t(bytes), discard);
    VERIFY(data);
    return static_cast<u32*>(data);
}

void _IndexStream::Unlock32(u32 RealCount)
{
    mPosition32 += RealCount;
#ifdef XR_PLATFORM_WEB
    pIB32.Unmap(size_t(RealCount) * sizeof(u32));
#else
    pIB32.Unmap();
#endif
}

void _IndexStream::reset_begin()
{
    old_pIB = pIB;
    old_pIB32 = pIB32;
    Destroy();
}
void _IndexStream::reset_end()
{
    Create();
    //old_pIB = NULL;
}
} // namespace xray::render::RENDER_NAMESPACE
