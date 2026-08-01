#include "BlackHoleComponent.h"

#include <utility>

namespace BlackHole {

Component::Component(std::unique_ptr<IBlackHoleRenderBackend> backend)
    : backend_(std::move(backend)) {}

Component::~Component() {
    Shutdown();
}

bool Component::Initialize(const SurfaceDescriptor& surface) {
    // device/target are optional: the D3D11 backend creates its own device.
    if (!backend_ || surface.width == 0 || surface.height == 0) {
        state_ = ComponentState::Failed;
        return false;
    }
    if (!backend_->Initialize(surface)) {
        state_ = ComponentState::Failed;
        return false;
    }
    state_ = ComponentState::Ready;
    return true;
}

bool Component::Resize(uint32_t width, uint32_t height, float dpiScale) {
    if (!backend_ || width == 0 || height == 0 || state_ == ComponentState::Stopped) {
        return false;
    }
    if (!backend_->Resize(width, height, dpiScale)) {
        state_ = ComponentState::DeviceLost;
        return false;
    }
    return true;
}

bool Component::RenderFrame(const FrameContext& frame, FrameStats& stats) {
    if (!backend_ || state_ == ComponentState::Stopped ||
        state_ == ComponentState::Failed || state_ == ComponentState::DeviceLost) {
        return false;
    }
    if (!frame.hostVisible || frame.sessionLocked) {
        Suspend();
        return true;
    }
    if (state_ == ComponentState::Suspended) Resume();

    Settings frameSettings = CurrentSettings();
    if (frame.onBatterySaver) {
        frameSettings.quality = QualityLevel::Low;
        frameSettings.renderScale = (std::min)(frameSettings.renderScale, 0.5f);
        frameSettings.bloom = false;
    }
    if (!backend_->Render(frameSettings, frame, stats)) {
        state_ = ComponentState::DeviceLost;
        return false;
    }
    state_ = ComponentState::Running;
    AdjustQuality(frame, stats);
    return true;
}

void Component::UpdateSettings(const Settings& settings) {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    settings_ = Settings::Sanitize(settings);
}

bool Component::CopyFramePixels(std::vector<uint8_t>& outBuffer,
                                uint32_t& outWidth, uint32_t& outHeight) {
    if (!backend_) return false;
    return backend_->CopyFramePixels(outBuffer, outWidth, outHeight);
}

Settings Component::CurrentSettings() const {
    std::lock_guard<std::mutex> lock(settingsMutex_);
    return settings_;
}

void Component::Suspend() {
    if (!backend_ || state_ == ComponentState::Suspended ||
        state_ == ComponentState::Stopped) return;
    backend_->Suspend();
    state_ = ComponentState::Suspended;
}

void Component::Resume() {
    if (!backend_ || state_ != ComponentState::Suspended) return;
    backend_->Resume();
    state_ = ComponentState::Ready;
}

void Component::NotifyDeviceLost() {
    state_ = ComponentState::DeviceLost;
}

void Component::Shutdown() {
    if (backend_ && state_ != ComponentState::Stopped) backend_->Shutdown();
    state_ = ComponentState::Stopped;
    slowFrameCount_ = 0;
    healthyFrameCount_ = 0;
}

void Component::AdjustQuality(const FrameContext& frame, const FrameStats& stats) {
    const float frameCost = (std::max)(stats.cpuMilliseconds, stats.gpuMilliseconds);
    const float budget = frame.onBatterySaver ? 12.0f : 8.0f;
    if (frameCost > budget) {
        ++slowFrameCount_;
        healthyFrameCount_ = 0;
    } else if (frameCost < budget * 0.65f) {
        ++healthyFrameCount_;
        slowFrameCount_ = 0;
    } else {
        slowFrameCount_ = 0;
        healthyFrameCount_ = 0;
    }

    std::lock_guard<std::mutex> lock(settingsMutex_);
    if (slowFrameCount_ >= 8) {
        settings_.renderScale = (std::max)(0.35f, settings_.renderScale - 0.1f);
        slowFrameCount_ = 0;
    } else if (healthyFrameCount_ >= 180) {
        settings_.renderScale = (std::min)(1.0f, settings_.renderScale + 0.05f);
        healthyFrameCount_ = 0;
    }
}

} // namespace BlackHole
