#pragma once

#include <DirectXTex.h>
#include <algorithm>
#include <vector>

namespace xray::render::texture_loading
{
// ScratchImage stores a complete mip chain for each array item, and a set of
// depth slices for each volume mip. Advancing one flat pointer is insufficient.
inline size_t SelectMips(const DirectX::ScratchImage& source, size_t requestedSkip,
    DirectX::TexMetadata& metadata, std::vector<DirectX::Image>& images, size_t& bytes)
{
    const auto& original = source.GetMetadata();
    const size_t skip = original.IsCubemap() ? 0 : std::min(requestedSkip, original.mipLevels - 1);
    metadata = original;
    metadata.width = std::max(size_t(1), original.width >> skip);
    metadata.height = std::max(size_t(1), original.height >> skip);
    metadata.depth = std::max(size_t(1), original.depth >> skip);
    metadata.mipLevels -= skip;
    images.clear();
    bytes = 0;
    for (size_t item = 0; item < original.arraySize; ++item)
    {
        for (size_t mip = skip; mip < original.mipLevels; ++mip)
        {
            const size_t slices = original.dimension == DirectX::TEX_DIMENSION_TEXTURE3D
                ? std::max(size_t(1), original.depth >> mip) : 1;
            for (size_t slice = 0; slice < slices; ++slice)
            {
                const auto* image = source.GetImage(mip, item, slice);
                if (!image)
                {
                    images.clear();
                    bytes = 0;
                    return skip;
                }
                images.push_back(*image);
                bytes += image->slicePitch;
            }
        }
    }
    return skip;
}
}
