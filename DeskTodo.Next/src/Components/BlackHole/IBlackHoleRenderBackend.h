#pragma once

#include <cstdint>
#include <vector>

#include "BlackHoleTypes.h"

namespace BlackHole {

class IBlackHoleRenderBackend {
public:
    virtual ~IBlackHoleRenderBackend() = default;

    virtual bool Initialize(const SurfaceDescriptor& surface) = 0;
    virtual bool Resize(uint32_t width, uint32_t height, float dpiScale) = 0;
    virtual bool Render(const Settings& settings, const FrameContext& frame,
                        FrameStats& stats) = 0;
    // Copy the latest rendered frame into outBuffer (BGRA, top-to-bottom).
    // Returns false if no frame is available yet.
    virtual bool CopyFramePixels(std::vector<uint8_t>& outBuffer,
                                 uint32_t& outWidth, uint32_t& outHeight) = 0;
    virtual void Suspend() = 0;
    virtual void Resume() = 0;
    virtual void Shutdown() = 0;
};

} // namespace BlackHole
