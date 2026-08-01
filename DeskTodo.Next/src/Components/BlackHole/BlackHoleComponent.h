#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "BlackHoleTypes.h"
#include "IBlackHoleRenderBackend.h"

namespace BlackHole {

class Component {
public:
    explicit Component(std::unique_ptr<IBlackHoleRenderBackend> backend);
    ~Component();

    Component(const Component&) = delete;
    Component& operator=(const Component&) = delete;

    bool Initialize(const SurfaceDescriptor& surface);
    bool Resize(uint32_t width, uint32_t height, float dpiScale);
    bool RenderFrame(const FrameContext& frame, FrameStats& stats);
    bool CopyFramePixels(std::vector<uint8_t>& outBuffer,
                         uint32_t& outWidth, uint32_t& outHeight);
    void UpdateSettings(const Settings& settings);
    Settings CurrentSettings() const;
    void Suspend();
    void Resume();
    void NotifyDeviceLost();
    void Shutdown();

    ComponentState State() const { return state_; }

private:
    void AdjustQuality(const FrameContext& frame, const FrameStats& stats);

    std::unique_ptr<IBlackHoleRenderBackend> backend_;
    mutable std::mutex settingsMutex_;
    Settings settings_{};
    ComponentState state_ = ComponentState::Stopped;
    uint32_t slowFrameCount_ = 0;
    uint32_t healthyFrameCount_ = 0;
};

} // namespace BlackHole
