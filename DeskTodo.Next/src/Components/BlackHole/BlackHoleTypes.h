#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace BlackHole {

enum class QualityLevel : uint8_t {
    Low,
    Balanced,
    High
};

enum class BackendKind : uint8_t {
    Direct3D11,
    OpenGL,
    SoftwareFallback
};

enum class ComponentState : uint8_t {
    Stopped,
    Ready,
    Running,
    Suspended,
    DeviceLost,
    Failed
};

// Settings mirror the uniforms from rossning92/Blackhole (blackhole_main.frag
// + main.cpp ImGui controls).  Field names match the repo's uniform names
// so the HLSL port stays a faithful 1:1 translation.
struct Settings {
    // ---- Infrastructure (used by Component / Background) ----
    bool enabled = false;
    float renderScale = 0.75f;
    int maxFps = 30;
    QualityLevel quality = QualityLevel::Balanced;

    // ---- Black hole + camera (blackhole_main.frag uniforms) ----
    bool renderBlackHole = true;          // renderBlackHole
    bool gravatationalLensing = true;     // gravatationalLensing (repo spelling)
    bool mouseControl = false;            // mouseControl
    float cameraRoll = 0.0f;              // cameraRoll (degrees, -180..180)
    bool frontView = false;               // frontView
    bool topView = false;                 // topView
    float fovScale = 1.0f;               // fovScale

    // Mouse position in render-target pixels (for mouseControl camera).
    float mouseX = 0.0f;
    float mouseY = 0.0f;

    // ---- Accretion disk (adisk* uniforms) ----
    bool adiskEnabled = true;             // adiskEnabled
    bool adiskParticle = true;            // adiskParticle
    float adiskHeight = 0.55f;            // adiskHeight
    float adiskLit = 0.25f;               // adiskLit
    float adiskDensityV = 2.0f;           // adiskDensityV
    float adiskDensityH = 4.0f;           // adiskDensityH
    float adiskNoiseScale = 0.8f;         // adiskNoiseScale
    float adiskNoiseLOD = 5.0f;           // adiskNoiseLOD
    float adiskSpeed = 0.5f;             // adiskSpeed

    // ---- Bloom ----
    bool bloom = true;
    int bloomIterations = 8;              // 1..8
    float bloomStrength = 0.1f;          // bloomStrength

    // ---- Tone mapping ----
    bool tonemappingEnabled = true;
    float gamma = 2.5f;                  // gamma

    // ---- ghostty-blackhole preset controls ----
    // These defaults and ranges match its Inferno preset/config panel.
    float holeRadius = 0.020f;
    float diskTemperature = 5500.0f;
    float diskInclination = 1.50f;
    float diskRoll = 0.35f;
    float diskInnerRadius = 1.8f;
    float diskOuterRadius = 8.0f;
    float diskOpacity = 0.90f;
    float dopplerMix = 0.60f;
    float beamExponent = 2.5f;
    float diskGain = 2.20f;
    float streakContrast = 1.60f;
    float winding = 7.0f;
    float diskRotationSpeed = 5.0f;
    float diskExposure = 1.40f;
    float starBrightness = 0.0f;

    static Settings Sanitize(Settings v) {
        v.renderScale = std::clamp(v.renderScale, 0.25f, 1.0f);
        v.maxFps = std::clamp(v.maxFps, 10, 120);
        v.cameraRoll = std::clamp(v.cameraRoll, -180.0f, 180.0f);
        v.fovScale = std::clamp(v.fovScale, 0.1f, 4.0f);
        v.adiskHeight = std::clamp(v.adiskHeight, 0.0f, 1.0f);
        v.adiskLit = std::clamp(v.adiskLit, 0.0f, 4.0f);
        v.adiskDensityV = std::clamp(v.adiskDensityV, 0.0f, 10.0f);
        v.adiskDensityH = std::clamp(v.adiskDensityH, 0.0f, 10.0f);
        v.adiskNoiseScale = std::clamp(v.adiskNoiseScale, 0.0f, 10.0f);
        v.adiskNoiseLOD = std::clamp(v.adiskNoiseLOD, 1.0f, 12.0f);
        v.adiskSpeed = std::clamp(v.adiskSpeed, 0.0f, 1.0f);
        v.bloomIterations = std::clamp(v.bloomIterations, 1, 8);
        v.bloomStrength = std::clamp(v.bloomStrength, 0.0f, 1.0f);
        v.gamma = std::clamp(v.gamma, 1.0f, 4.0f);
        v.holeRadius = std::clamp(v.holeRadius, 0.005f, 0.20f);
        v.diskTemperature = std::clamp(v.diskTemperature, 1000.0f, 30000.0f);
        v.diskInclination = std::clamp(v.diskInclination, 0.0f, 3.0f);
        v.diskRoll = std::clamp(v.diskRoll, -1.0f, 1.0f);
        v.diskInnerRadius = std::clamp(v.diskInnerRadius, 0.5f, 10.0f);
        v.diskOuterRadius = std::clamp(v.diskOuterRadius, 1.0f, 30.0f);
        v.diskOuterRadius = (std::max)(v.diskOuterRadius, v.diskInnerRadius + 0.5f);
        v.diskOpacity = std::clamp(v.diskOpacity, 0.0f, 1.0f);
        v.dopplerMix = std::clamp(v.dopplerMix, 0.0f, 1.5f);
        v.beamExponent = std::clamp(v.beamExponent, 0.5f, 10.0f);
        v.diskGain = std::clamp(v.diskGain, 0.0f, 5.0f);
        v.streakContrast = std::clamp(v.streakContrast, 0.1f, 5.0f);
        v.winding = std::clamp(v.winding, 1.0f, 20.0f);
        v.diskRotationSpeed = std::clamp(v.diskRotationSpeed, 0.5f, 15.0f);
        v.diskExposure = std::clamp(v.diskExposure, 0.1f, 5.0f);
        v.starBrightness = std::clamp(v.starBrightness, 0.0f, 2.0f);
        return v;
    }
};

struct SurfaceDescriptor {
    BackendKind backend = BackendKind::Direct3D11;
    void* device = nullptr;
    void* deviceContext = nullptr;
    void* target = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    float dpiScale = 1.0f;
};

struct FrameContext {
    double monotonicSeconds = 0.0;
    float deltaSeconds = 0.0f;
    bool hostVisible = true;
    bool sessionLocked = false;
    bool onBatterySaver = false;
};

struct FrameStats {
    float cpuMilliseconds = 0.0f;
    float gpuMilliseconds = 0.0f;
    float effectiveRenderScale = 1.0f;
    uint32_t integrationSteps = 0;
};

} // namespace BlackHole
