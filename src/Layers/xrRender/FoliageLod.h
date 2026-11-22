#pragma once

namespace xray::render::RENDER_NAMESPACE::foliage_lod
{
// Object/material policy, independent of the user's global graphics settings.
// Use the nearest point of the bounding sphere, not its center. An object
// whose crown reaches within the configured distance keeps the original path.
inline float farDistance = 70.f; // тут обрізка якості після якої вимикається те, що навантажує

inline bool isFar(float cameraDistanceSquared, float sphereRadius)
{
    const float limit = farDistance + sphereRadius;
    return cameraDistanceSquared > limit * limit;
}
} // namespace xray::render::RENDER_NAMESPACE::foliage_lod
