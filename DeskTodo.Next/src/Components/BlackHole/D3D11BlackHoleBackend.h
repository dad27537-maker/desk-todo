#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "IBlackHoleRenderBackend.h"

namespace BlackHole {

// Direct3D 11 backend faithfully porting rossning92/Blackhole.
//
// Render pipeline (identical pass order to the repo's main.cpp):
//   1. Scene pass        — blackhole_main.frag → HDR scene (R16G16B16A16)
//   2. Brightness extract — bloom_brightness_pass.frag
//   3. Downsample chain   — bloom_downsample.frag (up to 8 levels)
//   4. Upsample chain     — bloom_upsample.frag
//   5. Composite          — bloom_composite.frag
//   6. Tone mapping       — tonemapping.frag → LDR output (B8G8R8A8)
//   7. Staging readback   — CopyResource to staging for D2D bridging
//
// Assets are generated procedurally (no external file loading):
//   - Cubemap starfield (replaces assets/skybox_nebula_dark)
//   - Color gradient (replaces assets/color_map.png)
class D3D11BlackHoleBackend : public IBlackHoleRenderBackend {
public:
    D3D11BlackHoleBackend() = default;
    ~D3D11BlackHoleBackend() override;

    bool Initialize(const SurfaceDescriptor& surface) override;
    bool Resize(uint32_t width, uint32_t height, float dpiScale) override;
    bool Render(const Settings& settings, const FrameContext& frame,
                FrameStats& stats) override;
    bool CopyFramePixels(std::vector<uint8_t>& outBuffer,
                         uint32_t& outWidth, uint32_t& outHeight) override;
    void Suspend() override;
    void Resume() override;
    void Shutdown() override;

private:
    static constexpr uint32_t kMaxBloomLevels = 8;

    struct BloomLevel {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    bool CreateDevice();
    bool CreateSizeResources();
    bool CreateShaders();
    bool CreateSamplers();
    bool CreateProceduralAssets();  // cubemap + color map
    void ReleaseSizeResources();
    void ReleaseAll();

    // Pipeline passes (match repo order)
    void RenderScenePass(const Settings& s, const FrameContext& frame);
    void RenderBrightnessPass();
    void RenderDownsampleChain(int levels);
    void RenderUpsampleChain(int levels);
    void RenderCompositePass(const Settings& s);
    void RenderTonemapPass(const Settings& s);
    bool CopyToStaging();

    // D3D11 device
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;

    // Shaders
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> scenePS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> brightExtractPS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> downsamplePS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> upsamplePS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> compositePS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> tonemapPS_;

    // Constant buffers
    Microsoft::WRL::ComPtr<ID3D11Buffer> sceneCB_;       // SceneConstants
    Microsoft::WRL::ComPtr<ID3D11Buffer> postCB_;        // PostConstants
    Microsoft::WRL::ComPtr<ID3D11Buffer> resolutionCB_;  // ResolutionConstants

    // Samplers
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linearSampler_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;

    // Blend state for additive upsample pass
    Microsoft::WRL::ComPtr<ID3D11BlendState> additiveBlendState_;

    // Procedural assets
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cubemapTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> cubemapSRV_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> colorMapTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> colorMapSRV_;

    // HDR scene target
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sceneTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> sceneRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneSRV_;

    // Brightness extract target
    Microsoft::WRL::ComPtr<ID3D11Texture2D> brightnessTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> brightnessRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> brightnessSRV_;

    // Bloom chain
    BloomLevel bloomDown_[kMaxBloomLevels];  // downsampled
    BloomLevel bloomUp_[kMaxBloomLevels];    // upsampled

    // Composite target (HDR)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> compositeTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> compositeRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> compositeSRV_;

    // LDR output (final)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outputSRV_;

    // CPU readback
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture_;

    // Fullscreen triangle vertex buffer (optional; we use SV_VertexID)
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout_;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    float dpiScale_ = 1.0f;
    std::atomic<bool> suspended_{false};
    std::atomic<bool> initialized_{false};

    // CPU-accessible frame buffer (BGRA, top-to-bottom).
    std::mutex bufferMutex_;
    std::vector<uint8_t> frameBuffer_;
    uint32_t frameWidth_ = 0;
    uint32_t frameHeight_ = 0;
    std::atomic<bool> frameReady_{false};
};

} // namespace BlackHole
