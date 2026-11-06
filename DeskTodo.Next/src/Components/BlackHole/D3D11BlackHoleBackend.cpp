#include "D3D11BlackHoleBackend.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace BlackHole {

// ---------------------------------------------------------------------------
// Embedded HLSL shaders — clean-room port of the gravitational physics and
// bloom pipeline.  No code or assets sourced from any third-party repository.
// Shaders are embedded as string literals so no disk loading occurs at runtime.
// ---------------------------------------------------------------------------
namespace {

constexpr const char* kShaderSource = R"HLSL(
// ===== Constant Buffers =====

cbuffer SceneConstants : register(b0) {
    float2 resolution;       // 0
    float  time;             // 8
    float  fovScale;         // 12
    float  cameraRoll;       // 16  (radians)
    float  mouseX;           // 20
    float  mouseY;           // 24
    uint   flags;            // 28  bit0:renderBH bit1:lensing bit2:mouseCtrl
                                  //  bit3:frontView bit4:topView bit5:adiskEn
                                  //  bit6:adiskParticle
    float  adiskHeight;      // 32
    float  adiskLit;         // 36
    float  adiskDensityV;    // 40
    float  adiskDensityH;    // 44
    float  adiskNoiseScale;  // 48
    float  adiskNoiseLOD;    // 52
    float  adiskSpeed;       // 56
    float  scenePad;         // 60
    float  holeRadius;       // 64
    float  diskTemperature;  // 68
    float  diskInclination;  // 72
    float  diskRoll;         // 76
    float  diskInnerRadius;  // 80
    float  diskOuterRadius;  // 84
    float  diskOpacity;      // 88
    float  dopplerMix;       // 92
    float  beamExponent;     // 96
    float  diskGain;         // 100
    float  streakContrast;   // 104
    float  winding;          // 108
    float  diskRotationSpeed;// 112
    float  diskExposure;     // 116
    float  starBrightness;   // 120
    float  presetPad;        // 124
};

cbuffer PostConstants : register(b1) {
    float  bloomStrength;    // 0
    float  brightThreshold;  // 4
    float  gamma;            // 8
    uint   tonemapFlags;     // 12  bit0: tonemappingEnabled
};

cbuffer ResolutionConstants : register(b2) {
    float2 sourceSize;       // 0
    float2 resPad;           // 8
};

// ===== Samplers =====
SamplerState linearSampler : register(s0);
SamplerState pointSampler  : register(s1);

// ===== Vertex Shader (fullscreen triangle via SV_VertexID) =====
struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

PixelInput VSMain(uint vertexId : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0)
    };
    PixelInput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = positions[vertexId] * float2(0.5, -0.5) + 0.5;
    return output;
}

// ===== Simplex 3D Noise (public-domain Gustavson implementation) =====
float3 mod289(float3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float4 mod289(float4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float4 permute(float4 x) { return mod289(((x * 34.0) + 1.0) * x); }
float4 taylorInvSqrt(float4 r) { return 1.79284291400159 - 0.85373472095314 * r; }

float snoise(float3 v) {
    const float2 C = float2(1.0 / 6.0, 1.0 / 3.0);
    const float4 D = float4(0.0, 0.5, 1.0, 2.0);

    float3 i  = floor(v + dot(v, C.yyy));
    float3 x0 = v - i + dot(i, C.xxx);

    float3 g = step(x0.yzx, x0.xyz);
    float3 l = 1.0 - g;
    float3 i1 = min(g.xyz, l.zxy);
    float3 i2 = max(g.xyz, l.zxy);

    float3 x1 = x0 - i1 + C.xxx;
    float3 x2 = x0 - i2 + C.yyy;
    float3 x3 = x0 - D.yyy;

    i = mod289(i);
    float4 p = permute(permute(permute(
        i.z + float4(0.0, i1.z, i2.z, 1.0))
        + i.y + float4(0.0, i1.y, i2.y, 1.0))
        + i.x + float4(0.0, i1.x, i2.x, 1.0));

    float n_ = 0.142857142857;
    float3 ns = n_ * D.wyz - D.xzx;

    float4 j = p - 49.0 * floor(p * ns.z * ns.z);

    float4 x_ = floor(j * ns.z);
    float4 y_ = floor(j - 7.0 * x_);

    float4 x = x_ * ns.x + ns.yyyy;
    float4 y = y_ * ns.x + ns.yyyy;
    float4 h = 1.0 - abs(x) - abs(y);

    float4 b0 = float4(x.xy, y.xy);
    float4 b1 = float4(x.zw, y.zw);

    float4 s0 = floor(b0) * 2.0 + 1.0;
    float4 s1 = floor(b1) * 2.0 + 1.0;
    float4 sh = -step(h, float4(0.0, 0.0, 0.0, 0.0));

    float4 a0 = b0.xzyw + s0.xzyw * sh.xxyy;
    float4 a1 = b1.xzyw + s1.xzyw * sh.zzww;

    float3 p0 = float3(a0.xy, h.x);
    float3 p1 = float3(a0.zw, h.y);
    float3 p2 = float3(a1.xy, h.z);
    float3 p3 = float3(a1.zw, h.w);

    float4 norm = taylorInvSqrt(float4(dot(p0, p0), dot(p1, p1), dot(p2, p2), dot(p3, p3)));
    p0 *= norm.x;
    p1 *= norm.y;
    p2 *= norm.z;
    p3 *= norm.w;

    float4 m = max(0.6 - float4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
    m = m * m;
    return 42.0 * dot(m * m, float4(dot(p0, x0), dot(p1, x1), dot(p2, x2), dot(p3, x3)));
}

// ===== Scene Pixel Shader =====
TextureCube skyboxTex   : register(t0);
Texture2D   colorMapTex : register(t1);

// Accretion disk density at a position in disk-local coordinates.
float adiskDensityFn(float3 pos) {
    float r = length(pos.xz);
    if (r < diskInnerRadius || r > diskOuterRadius) return 0.0;

    float radial = smoothstep(diskInnerRadius, diskInnerRadius * 1.25, r)
                 * (1.0 - smoothstep(diskOuterRadius * 0.70, diskOuterRadius, r));
    float vertical = exp(-abs(pos.y) / max(adiskHeight, 0.001));

    float angle = atan2(pos.z, pos.x) + diskRoll;
    // Differential rotation — inner radii rotate faster (Keplerian-like).
    float rotSpeed = diskRotationSpeed / (r * r + 0.5);
    float3 noisePos = float3(r * adiskNoiseScale, angle * winding,
                              time * rotSpeed);
    float n = snoise(noisePos) * 0.5 + 0.5;

    float density = radial * vertical * (0.35 + streakContrast * n * n) * adiskDensityV;

    // Particle clumping — breaks smooth density into clumps.
    if ((flags & 64u) != 0u) {
        float3 pPos = float3(r * adiskNoiseScale * 2.5,
                              angle * adiskDensityH * 1.5,
                              time * rotSpeed * 1.3);
        float p = snoise(pPos);
        density *= smoothstep(-0.2, 0.6, p);
    }

    return max(density, 0.0);
}

// Accretion disk emission color at a position.
float3 adiskColorFn(float3 pos, float3 rayDir) {
    float r = length(pos.xz);
    float t = saturate((r - diskInnerRadius) / max(diskOuterRadius - diskInnerRadius, 0.5));
    float3 col = colorMapTex.SampleLevel(linearSampler, float2(t, 0.5), 0).rgb;

    // Doppler beaming — approaching side is brighter.
    float3 orbitalDir = normalize(float3(-pos.z, 0.0, pos.x));
    float orbitalVel = 1.0 / sqrt(max(r, 1.0));
    float doppler = 1.0 / max(1.0 - orbitalVel * dot(orbitalDir, rayDir) * dopplerMix, 0.1);
    doppler = clamp(doppler, 0.3, 4.0);

    float thermal = clamp(diskTemperature / 5500.0, 0.25, 5.5);
    return col * pow(doppler, beamExponent) * diskGain * thermal;
}

float4 ScenePS(PixelInput input) : SV_TARGET {
    float2 screen = (input.uv * 2.0 - 1.0);
    screen.x *= resolution.x / max(resolution.y, 1.0);
    float2 drift = float2(0.08 * sin(time * 0.21) + 0.03 * sin(time * 0.083),
                          0.06 * sin(time * 0.157 + 2.0) + 0.03 * sin(time * 0.117));
    screen -= drift;
    screen *= fovScale * (0.070 / max(holeRadius, 0.005));

    // --- Camera setup ---
    float3 camPos;
    bool isFrontView = (flags & 8u)  != 0u;
    bool isTopView   = (flags & 16u) != 0u;
    bool isMouseCtrl = (flags & 4u)  != 0u;

    if (isFrontView) {
        camPos = float3(14.0, 0.8, 0.0);
    } else if (isTopView) {
        camPos = float3(0.0, 14.0, 0.001);
    } else {
        float t = time * 0.12;
        camPos = float3(sin(t) * 14.0,
                        14.0 * cos(diskInclination),
                        -cos(t) * 14.0);
    }

    if (isMouseCtrl) {
        float mx = (mouseX - 0.5) * 2.0;
        float my = (mouseY - 0.5) * 2.0;
        camPos.x += mx * 6.0;
        camPos.y = clamp(camPos.y + my * 4.0, 0.5, 12.0);
    }

    float3 camTarget = float3(0.0, 0.0, 0.0);
    float3 forward = normalize(camTarget - camPos);
    float3 right = normalize(cross(forward, float3(0.0, 1.0, 0.0)));
    float3 up = cross(right, forward);

    // Apply camera roll
    if (abs(cameraRoll) > 0.001) {
        float c = cos(cameraRoll);
        float s = sin(cameraRoll);
        float3 newRight = right * c + up * s;
        float3 newUp    = -right * s + up * c;
        right = newRight;
        up = newUp;
    }

    float3 rayDir = normalize(forward + right * screen.x + up * screen.y);
    float3 pos = camPos;

    bool renderBH = (flags & 1u)  != 0u;
    bool lensing  = (flags & 2u)  != 0u;
    bool adiskEn  = (flags & 32u) != 0u;

    // Angular momentum (conserved along geodesic)
    float3 L = cross(pos, rayDir);
    float L2 = dot(L, L);

    float3 color = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;

    const int STEPS = 300;
    const float stepSize = 0.1;

    [loop]
    for (int i = 0; i < STEPS; ++i) {
        float r2 = dot(pos, pos);

        // Event horizon — ray captured
        if (renderBH && r2 < 1.0) {
            return float4(color, 0.0);
        }

        // Ray escaped to infinity
        if (r2 > 900.0) break;

        // Gravitational lensing — bend ray via geodesic approximation
        if (lensing && renderBH) {
            float r5 = r2 * r2 * sqrt(r2);
            float3 accel = -1.5 * L2 * pos / max(r5, 1e-6);
            rayDir = normalize(rayDir + accel * stepSize);
        }

        pos += rayDir * stepSize;

        // Accretion disk sampling
        if (adiskEn) {
            float density = adiskDensityFn(pos);
            if (density > 0.001) {
                float3 diskCol = adiskColorFn(pos, rayDir);
                color += transmittance * diskCol * density * stepSize * diskExposure;
                transmittance *= exp(-density * stepSize * diskOpacity);
                if (transmittance < 0.01) break;
            }
        }
    }

    // Skybox background
    float3 sky = skyboxTex.SampleLevel(linearSampler, rayDir, 0).rgb;
    color += transmittance * sky * starBrightness;

    return float4(color, 1.0);
}

// ===== Brightness Extract =====
Texture2D brightSourceTex : register(t0);

float4 BrightExtractPS(PixelInput input) : SV_TARGET {
    float3 c = brightSourceTex.SampleLevel(linearSampler, input.uv, 0).rgb;
    float luminance = dot(float3(0.2126, 0.7152, 0.0722), c);
    luminance = max(0.0, luminance - brightThreshold);
    c *= sign(luminance);
    return float4(c, 1.0);
}

// ===== Downsample (4-tap box filter) =====
Texture2D downsampleTex : register(t0);

float4 DownsamplePS(PixelInput input) : SV_TARGET {
    float2 texel = 1.0 / sourceSize;
    float2 uv = input.uv;
    float3 c = downsampleTex.SampleLevel(pointSampler, uv + float2(-0.5, -0.5) * texel, 0).rgb;
    c += downsampleTex.SampleLevel(pointSampler, uv + float2( 0.5, -0.5) * texel, 0).rgb;
    c += downsampleTex.SampleLevel(pointSampler, uv + float2(-0.5,  0.5) * texel, 0).rgb;
    c += downsampleTex.SampleLevel(pointSampler, uv + float2( 0.5,  0.5) * texel, 0).rgb;
    return float4(c * 0.25, 1.0);
}

// ===== Upsample (9-tap gaussian) =====
Texture2D upsampleTex : register(t0);

float4 UpsamplePS(PixelInput input) : SV_TARGET {
    float2 texel = 1.0 / sourceSize;
    float2 uv = input.uv;
    float3 c = upsampleTex.SampleLevel(linearSampler, uv, 0).rgb;
    c += upsampleTex.SampleLevel(linearSampler, uv + float2(-1.0,  0.0) * texel, 0).rgb;
    c += upsampleTex.SampleLevel(linearSampler, uv + float2( 1.0,  0.0) * texel, 0).rgb;
    c += upsampleTex.SampleLevel(linearSampler, uv + float2( 0.0, -1.0) * texel, 0).rgb;
    c += upsampleTex.SampleLevel(linearSampler, uv + float2( 0.0,  1.0) * texel, 0).rgb;
    c += upsampleTex.SampleLevel(linearSampler, uv + float2(-0.5, -0.5) * texel, 0).rgb;
    c += upsampleTex.SampleLevel(linearSampler, uv + float2( 0.5, -0.5) * texel, 0).rgb;
    c += upsampleTex.SampleLevel(linearSampler, uv + float2(-0.5,  0.5) * texel, 0).rgb;
    c += upsampleTex.SampleLevel(linearSampler, uv + float2( 0.5,  0.5) * texel, 0).rgb;
    return float4(c * (1.0 / 9.0), 1.0);
}

// ===== Composite =====
Texture2D compositeSceneTex : register(t0);
Texture2D compositeBloomTex : register(t1);

float4 CompositePS(PixelInput input) : SV_TARGET {
    float3 scene = compositeSceneTex.SampleLevel(linearSampler, input.uv, 0).rgb;
    float3 bloom = compositeBloomTex.SampleLevel(linearSampler, input.uv, 0).rgb;
    return float4(scene + bloom * bloomStrength, 1.0);
}

// ===== Tone Mapping (ACES Filmic + gamma) =====
Texture2D tonemapTex : register(t0);

float3 ACESFilmic(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 TonemapPS(PixelInput input) : SV_TARGET {
    float3 hdr = tonemapTex.SampleLevel(linearSampler, input.uv, 0).rgb;
    float3 ldr;
    if ((tonemapFlags & 1u) != 0u) {
        ldr = ACESFilmic(hdr);
    } else {
        ldr = saturate(hdr);
    }
    ldr = pow(ldr, 1.0 / max(gamma, 0.1));
    return float4(ldr, 1.0);
}
)HLSL";

// ===== CPU-side constant buffer structs (must match HLSL cbuffer layouts) =====

struct SceneConstants {
    float  resolutionX;      // 0
    float  resolutionY;      // 4
    float  time;             // 8
    float  fovScale;         // 12
    float  cameraRoll;       // 16 (radians)
    float  mouseX;           // 20
    float  mouseY;           // 24
    uint32_t flags;          // 28
    float  adiskHeight;      // 32
    float  adiskLit;         // 36
    float  adiskDensityV;    // 40
    float  adiskDensityH;    // 44
    float  adiskNoiseScale;  // 48
    float  adiskNoiseLOD;    // 52
    float  adiskSpeed;       // 56
    float  pad;              // 60
    float  holeRadius;
    float  diskTemperature;
    float  diskInclination;
    float  diskRoll;
    float  diskInnerRadius;
    float  diskOuterRadius;
    float  diskOpacity;
    float  dopplerMix;
    float  beamExponent;
    float  diskGain;
    float  streakContrast;
    float  winding;
    float  diskRotationSpeed;
    float  diskExposure;
    float  starBrightness;
    float  presetPad;
};
static_assert(sizeof(SceneConstants) == 128, "SceneConstants must be 128 bytes");

struct PostConstants {
    float   bloomStrength;    // 0
    float   brightThreshold;  // 4
    float   gamma;            // 8
    uint32_t tonemapFlags;    // 12
};
static_assert(sizeof(PostConstants) == 16, "PostConstants must be 16 bytes");

struct ResolutionConstants {
    float sourceSizeX;       // 0
    float sourceSizeY;       // 4
    float padX;              // 8
    float padY;              // 12
};
static_assert(sizeof(ResolutionConstants) == 16, "ResolutionConstants must be 16 bytes");

// ===== Shader compilation helper =====
bool CompileShaderStage(const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kShaderSource, strlen(kShaderSource),
        "BlackHoleEmbedded", nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &out, &errors);
    if (FAILED(hr) && errors) {
        const char* msg = static_cast<const char*>(errors->GetBufferPointer());
        (void)msg;  // Available in debugger if needed
    }
    return SUCCEEDED(hr);
}

// ===== CPU-side vector math for procedural asset generation =====
struct Float3 { float x, y, z; };

Float3 normalize3(float x, float y, float z) {
    float len = sqrtf(x * x + y * y + z * z);
    if (len < 1e-6f) return {0.0f, 0.0f, 0.0f};
    return {x / len, y / len, z / len};
}

float hash31(float x, float y, float z) {
    float n = sinf(x * 127.1f + y * 311.7f + z * 74.7f) * 43758.5453f;
    return n - floorf(n);
}

// Generate starfield color for a cubemap direction.
Float3 generateStarColor(float dx, float dy, float dz) {
    Float3 dir = normalize3(dx, dy, dz);

    // Deep space background with slight color variation
    Float3 color = {0.008f, 0.012f, 0.035f};

    // Nebula-like glow
    float nebula = hash31(dir.x * 2.5f, dir.y * 2.5f, dir.z * 2.5f);
    if (nebula > 0.65f) {
        float t = (nebula - 0.65f) / 0.35f;
        color.x += 0.015f * t;
        color.y += 0.008f * t;
        color.z += 0.025f * t;
    }

    // Small stars — dense scatter
    float cellX = floorf(dir.x * 420.0f);
    float cellY = floorf(dir.y * 420.0f);
    float cellZ = floorf(dir.z * 420.0f);
    float star = hash31(cellX, cellY, cellZ);
    if (star > 0.9965f) {
        float brightness = (star - 0.9965f) / 0.0035f;
        color.x += brightness * 1.8f;
        color.y += brightness * 1.8f;
        color.z += brightness * 1.8f;
    }

    // Bright stars with color variation — sparse
    float bx = floorf(dir.x * 140.0f);
    float by = floorf(dir.y * 140.0f);
    float bz = floorf(dir.z * 140.0f);
    float brightStar = hash31(bx + 1.0f, by + 2.0f, bz + 3.0f);
    if (brightStar > 0.9985f) {
        float t = hash31(bx, by, bz);
        float intensity = (brightStar - 0.9985f) / 0.0015f;
        // Star color: white to blue-white to yellow-white
        color.x += intensity * 3.5f;
        color.y += intensity * (2.5f + 1.0f * t);
        color.z += intensity * (1.5f + 2.0f * t);
    }

    return color;
}

// Generate accretion disk color map entry.
// t=0 → hot inner disk (white-yellow), t=1 → cool outer disk (dark red)
Float3 generateDiskColor(float t) {
    Float3 hot  = {1.0f, 0.95f, 0.8f};   // white-yellow
    Float3 mid  = {1.0f, 0.5f,  0.1f};   // orange
    Float3 cool = {0.5f, 0.05f, 0.0f};   // dark red

    Float3 color;
    if (t < 0.5f) {
        float s = t * 2.0f;
        color.x = hot.x * (1.0f - s) + mid.x * s;
        color.y = hot.y * (1.0f - s) + mid.y * s;
        color.z = hot.z * (1.0f - s) + mid.z * s;
    } else {
        float s = (t - 0.5f) * 2.0f;
        color.x = mid.x * (1.0f - s) + cool.x * s;
        color.y = mid.y * (1.0f - s) + cool.y * s;
        color.z = mid.z * (1.0f - s) + cool.z * s;
    }
    return color;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// D3D11BlackHoleBackend implementation
// ---------------------------------------------------------------------------

D3D11BlackHoleBackend::~D3D11BlackHoleBackend() {
    Shutdown();
}

bool D3D11BlackHoleBackend::Initialize(const SurfaceDescriptor& surface) {
    if (initialized_.load()) return true;
    if (surface.width == 0 || surface.height == 0) return false;

    width_ = surface.width;
    height_ = surface.height;
    dpiScale_ = surface.dpiScale;

    if (!CreateDevice()) return false;
    if (!CreateShaders())   { ReleaseAll(); return false; }
    if (!CreateSamplers())  { ReleaseAll(); return false; }
    if (!CreateProceduralAssets()) { ReleaseAll(); return false; }
    if (!CreateSizeResources())    { ReleaseAll(); return false; }

    initialized_.store(true);
    return true;
}

bool D3D11BlackHoleBackend::CreateDevice() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL actual{};

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, levels, static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION, &device_, &actual, &context_))) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                flags, levels, static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION, &device_, &actual, &context_))) {
            return false;
        }
    }
    return true;
}

bool D3D11BlackHoleBackend::CreateShaders() {
    ComPtr<ID3DBlob> vsCode, scenePS, brightPS, downPS, upPS, compPS, tonemapPS;
    if (!CompileShaderStage("VSMain", "vs_5_0", vsCode) ||
        !CompileShaderStage("ScenePS", "ps_5_0", scenePS) ||
        !CompileShaderStage("BrightExtractPS", "ps_5_0", brightPS) ||
        !CompileShaderStage("DownsamplePS", "ps_5_0", downPS) ||
        !CompileShaderStage("UpsamplePS", "ps_5_0", upPS) ||
        !CompileShaderStage("CompositePS", "ps_5_0", compPS) ||
        !CompileShaderStage("TonemapPS", "ps_5_0", tonemapPS))
        return false;

    if (FAILED(device_->CreateVertexShader(vsCode->GetBufferPointer(),
            vsCode->GetBufferSize(), nullptr, &vertexShader_)))
        return false;
    if (FAILED(device_->CreatePixelShader(scenePS->GetBufferPointer(),
            scenePS->GetBufferSize(), nullptr, &scenePS_)))
        return false;
    if (FAILED(device_->CreatePixelShader(brightPS->GetBufferPointer(),
            brightPS->GetBufferSize(), nullptr, &brightExtractPS_)))
        return false;
    if (FAILED(device_->CreatePixelShader(downPS->GetBufferPointer(),
            downPS->GetBufferSize(), nullptr, &downsamplePS_)))
        return false;
    if (FAILED(device_->CreatePixelShader(upPS->GetBufferPointer(),
            upPS->GetBufferSize(), nullptr, &upsamplePS_)))
        return false;
    if (FAILED(device_->CreatePixelShader(compPS->GetBufferPointer(),
            compPS->GetBufferSize(), nullptr, &compositePS_)))
        return false;
    if (FAILED(device_->CreatePixelShader(tonemapPS->GetBufferPointer(),
            tonemapPS->GetBufferSize(), nullptr, &tonemapPS_)))
        return false;

    // Create constant buffers
    auto createCB = [&](uint32_t size, ComPtr<ID3D11Buffer>& buf) -> bool {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = size;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(device_->CreateBuffer(&bd, nullptr, &buf));
    };

    if (!createCB(sizeof(SceneConstants), sceneCB_)) return false;
    if (!createCB(sizeof(PostConstants), postCB_)) return false;
    if (!createCB(sizeof(ResolutionConstants), resolutionCB_)) return false;

    // Additive blend state for upsample pass
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blendDesc, &additiveBlendState_)))
        return false;

    return true;
}

bool D3D11BlackHoleBackend::CreateSamplers() {
    // Linear sampler
    D3D11_SAMPLER_DESC linDesc{};
    linDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    linDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    linDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    linDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    linDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    linDesc.MinLOD = 0;
    linDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&linDesc, &linearSampler_)))
        return false;

    // Point sampler (for downsample)
    D3D11_SAMPLER_DESC ptDesc{};
    ptDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ptDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    ptDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    ptDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ptDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ptDesc.MinLOD = 0;
    ptDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&ptDesc, &pointSampler_)))
        return false;

    return true;
}

bool D3D11BlackHoleBackend::CreateProceduralAssets() {
    // --- Procedural starfield cubemap (128x128 per face) ---
    const uint32_t cubeSize = 128;
    const uint32_t facePixels = cubeSize * cubeSize;
    std::vector<uint8_t> cubemapData(facePixels * 6 * 4);

    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < cubeSize; ++y) {
            for (uint32_t x = 0; x < cubeSize; ++x) {
                float u = (x + 0.5f) / cubeSize * 2.0f - 1.0f;
                float v = (y + 0.5f) / cubeSize * 2.0f - 1.0f;
                float dx, dy, dz;
                switch (face) {
                    case 0: dx =  1.0f; dy = v; dz = -u; break;  // +X
                    case 1: dx = -1.0f; dy = v; dz =  u; break;  // -X
                    case 2: dx = u; dy =  1.0f; dz = -v; break;  // +Y
                    case 3: dx = u; dy = -1.0f; dz =  v; break;  // -Y
                    case 4: dx = u; dy = v; dz =  1.0f; break;   // +Z
                    case 5: dx = -u; dy = v; dz = -1.0f; break;  // -Z
                }

                Float3 col = generateStarColor(dx, dy, dz);
                uint32_t idx = (face * cubeSize + y) * cubeSize + x;
                cubemapData[idx * 4 + 0] = static_cast<uint8_t>(
                    (std::min)(col.x * 255.0f, 255.0f));
                cubemapData[idx * 4 + 1] = static_cast<uint8_t>(
                    (std::min)(col.y * 255.0f, 255.0f));
                cubemapData[idx * 4 + 2] = static_cast<uint8_t>(
                    (std::min)(col.z * 255.0f, 255.0f));
                cubemapData[idx * 4 + 3] = 255;
            }
        }
    }

    D3D11_TEXTURE2D_DESC cubeDesc{};
    cubeDesc.Width = cubeSize;
    cubeDesc.Height = cubeSize;
    cubeDesc.MipLevels = 1;
    cubeDesc.ArraySize = 6;
    cubeDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cubeDesc.SampleDesc.Count = 1;
    cubeDesc.Usage = D3D11_USAGE_DEFAULT;
    cubeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cubeDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

    D3D11_SUBRESOURCE_DATA cubeInit[6];
    for (uint32_t i = 0; i < 6; ++i) {
        cubeInit[i].pSysMem = cubemapData.data() + i * facePixels * 4;
        cubeInit[i].SysMemPitch = cubeSize * 4;
        cubeInit[i].SysMemSlicePitch = 0;
    }

    if (FAILED(device_->CreateTexture2D(&cubeDesc, cubeInit, &cubemapTexture_)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC cubeSRVDesc{};
    cubeSRVDesc.Format = cubeDesc.Format;
    cubeSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    cubeSRVDesc.TextureCube.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(cubemapTexture_.Get(),
            &cubeSRVDesc, &cubemapSRV_)))
        return false;

    // --- Procedural color map (256x1 gradient for accretion disk) ---
    const uint32_t colorMapSize = 256;
    std::vector<uint8_t> colorMapData(colorMapSize * 4);
    for (uint32_t i = 0; i < colorMapSize; ++i) {
        float t = static_cast<float>(i) / (colorMapSize - 1);
        Float3 col = generateDiskColor(t);
        colorMapData[i * 4 + 0] = static_cast<uint8_t>(col.x * 255.0f);
        colorMapData[i * 4 + 1] = static_cast<uint8_t>(col.y * 255.0f);
        colorMapData[i * 4 + 2] = static_cast<uint8_t>(col.z * 255.0f);
        colorMapData[i * 4 + 3] = 255;
    }

    D3D11_TEXTURE2D_DESC cmDesc{};
    cmDesc.Width = colorMapSize;
    cmDesc.Height = 1;
    cmDesc.MipLevels = 1;
    cmDesc.ArraySize = 1;
    cmDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cmDesc.SampleDesc.Count = 1;
    cmDesc.Usage = D3D11_USAGE_DEFAULT;
    cmDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA cmInit{};
    cmInit.pSysMem = colorMapData.data();
    cmInit.SysMemPitch = colorMapSize * 4;

    if (FAILED(device_->CreateTexture2D(&cmDesc, &cmInit, &colorMapTexture_)))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC cmSRVDesc{};
    cmSRVDesc.Format = cmDesc.Format;
    cmSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    cmSRVDesc.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(colorMapTexture_.Get(),
            &cmSRVDesc, &colorMapSRV_)))
        return false;

    return true;
}

bool D3D11BlackHoleBackend::CreateSizeResources() {
    if (width_ == 0 || height_ == 0 || !device_) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv2DDesc{};
    srv2DDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv2DDesc.Texture2D.MipLevels = 1;

    auto createRT = [&](uint32_t w, uint32_t h, DXGI_FORMAT format,
                        ComPtr<ID3D11Texture2D>& tex,
                        ComPtr<ID3D11RenderTargetView>& rtv,
                        ComPtr<ID3D11ShaderResourceView>& srv) -> bool {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex))) return false;
        if (FAILED(device_->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) return false;
        srv2DDesc.Format = format;
        if (FAILED(device_->CreateShaderResourceView(tex.Get(), &srv2DDesc, &srv))) return false;
        return true;
    };

    // --- HDR scene target (R16G16B16A16_FLOAT) ---
    if (!createRT(width_, height_, DXGI_FORMAT_R16G16B16A16_FLOAT,
                  sceneTexture_, sceneRTV_, sceneSRV_))
        return false;

    // --- Brightness extract target (half-res HDR) ---
    const uint32_t halfW = (std::max)(1u, width_ / 2);
    const uint32_t halfH = (std::max)(1u, height_ / 2);
    if (!createRT(halfW, halfH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                  brightnessTexture_, brightnessRTV_, brightnessSRV_))
        return false;

    // --- Bloom down chain (8 levels, each half of previous) ---
    // bloomDown_[0] same size as brightness (half-res)
    // bloomDown_[1] = quarter-res, etc.
    uint32_t bw = halfW;
    uint32_t bh = halfH;
    for (uint32_t i = 0; i < kMaxBloomLevels; ++i) {
        if (!createRT(bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                      bloomDown_[i].texture, bloomDown_[i].rtv, bloomDown_[i].srv))
            return false;
        bloomDown_[i].width = bw;
        bloomDown_[i].height = bh;
        bw = (std::max)(1u, bw / 2);
        bh = (std::max)(1u, bh / 2);
    }

    // --- Composite target (HDR, full-res) ---
    if (!createRT(width_, height_, DXGI_FORMAT_R16G16B16A16_FLOAT,
                  compositeTexture_, compositeRTV_, compositeSRV_))
        return false;

    // --- LDR output target (B8G8R8A8_UNORM, full-res) ---
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &outputTexture_)))
            return false;
        if (FAILED(device_->CreateRenderTargetView(outputTexture_.Get(),
                nullptr, &outputRTV_)))
            return false;
    }

    // --- Staging texture for CPU readback ---
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width_;
        td.Height = height_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &stagingTexture_)))
            return false;
    }

    // Pre-allocate frame buffer
    frameBuffer_.resize(static_cast<size_t>(width_) * height_ * 4);
    frameWidth_ = width_;
    frameHeight_ = height_;
    return true;
}

void D3D11BlackHoleBackend::ReleaseSizeResources() {
    for (uint32_t i = 0; i < kMaxBloomLevels; ++i) {
        bloomDown_[i].srv.Reset();
        bloomDown_[i].rtv.Reset();
        bloomDown_[i].texture.Reset();
        bloomDown_[i].width = 0;
        bloomDown_[i].height = 0;

        bloomUp_[i].srv.Reset();
        bloomUp_[i].rtv.Reset();
        bloomUp_[i].texture.Reset();
        bloomUp_[i].width = 0;
        bloomUp_[i].height = 0;
    }
    stagingTexture_.Reset();
    outputSRV_.Reset();
    outputRTV_.Reset();
    outputTexture_.Reset();
    compositeSRV_.Reset();
    compositeRTV_.Reset();
    compositeTexture_.Reset();
    brightnessSRV_.Reset();
    brightnessRTV_.Reset();
    brightnessTexture_.Reset();
    sceneSRV_.Reset();
    sceneRTV_.Reset();
    sceneTexture_.Reset();
}

bool D3D11BlackHoleBackend::Resize(uint32_t w, uint32_t h, float dpiScale) {
    if (!initialized_.load() || w == 0 || h == 0) return false;
    if (w == width_ && h == height_ && dpiScale == dpiScale_) return true;

    width_ = w;
    height_ = h;
    dpiScale_ = dpiScale;

    ReleaseSizeResources();
    if (!CreateSizeResources()) return false;
    return true;
}

bool D3D11BlackHoleBackend::Render(const Settings& settings,
                                   const FrameContext& frame,
                                   FrameStats& stats) {
    if (!initialized_.load() || suspended_.load()) return false;
    if (!device_ || !context_ || !sceneRTV_ || !outputRTV_) return false;

    // --- Pass 1: Scene (HDR ray-marching) ---
    RenderScenePass(settings, frame);

    const int bloomLevels = settings.bloom
        ? std::clamp(settings.bloomIterations, 1, static_cast<int>(kMaxBloomLevels))
        : 0;

    if (bloomLevels > 0) {
        // --- Pass 2: Brightness extract ---
        RenderBrightnessPass();

        // --- Pass 3: Downsample chain ---
        RenderDownsampleChain(bloomLevels);

        // --- Pass 4: Upsample chain (additive) ---
        RenderUpsampleChain(bloomLevels);

        // --- Pass 5: Composite (scene + bloom) ---
        RenderCompositePass(settings);
    } else {
        // Skip bloom — copy scene directly to composite target
        context_->CopyResource(compositeTexture_.Get(), sceneTexture_.Get());
    }

    // --- Pass 6: Tone mapping ---
    RenderTonemapPass(settings);

    // --- Pass 7: Copy to staging for CPU readback ---
    if (!CopyToStaging()) return false;

    stats.effectiveRenderScale = settings.renderScale;
    stats.integrationSteps = 300;
    return true;
}

void D3D11BlackHoleBackend::RenderScenePass(const Settings& s,
                                             const FrameContext& frame) {
    // Map scene constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(sceneCB_.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;

    auto* c = static_cast<SceneConstants*>(mapped.pData);
    c->resolutionX = static_cast<float>(width_);
    c->resolutionY = static_cast<float>(height_);
    c->time = static_cast<float>(frame.monotonicSeconds);
    c->fovScale = s.fovScale;
    c->cameraRoll = s.cameraRoll * 3.14159265f / 180.0f;  // deg → rad
    c->mouseX = s.mouseX;
    c->mouseY = s.mouseY;
    c->flags = (s.renderBlackHole ? 1u : 0u)
             | (s.gravatationalLensing ? 2u : 0u)
             | (s.mouseControl ? 4u : 0u)
             | (s.frontView ? 8u : 0u)
             | (s.topView ? 16u : 0u)
             | (s.adiskEnabled ? 32u : 0u)
             | (s.adiskParticle ? 64u : 0u);
    c->adiskHeight = s.adiskHeight;
    c->adiskLit = s.adiskLit;
    c->adiskDensityV = s.adiskDensityV;
    c->adiskDensityH = s.adiskDensityH;
    c->adiskNoiseScale = s.adiskNoiseScale;
    c->adiskNoiseLOD = s.adiskNoiseLOD;
    c->adiskSpeed = s.adiskSpeed;
    c->pad = 0.0f;
    c->holeRadius = s.holeRadius;
    c->diskTemperature = s.diskTemperature;
    c->diskInclination = s.diskInclination;
    c->diskRoll = s.diskRoll;
    c->diskInnerRadius = s.diskInnerRadius;
    c->diskOuterRadius = s.diskOuterRadius;
    c->diskOpacity = s.diskOpacity;
    c->dopplerMix = s.dopplerMix;
    c->beamExponent = s.beamExponent;
    c->diskGain = s.diskGain;
    c->streakContrast = s.streakContrast;
    c->winding = s.winding;
    c->diskRotationSpeed = s.diskRotationSpeed;
    c->diskExposure = s.diskExposure;
    c->starBrightness = s.starBrightness;
    c->presetPad = 0.0f;
    context_->Unmap(sceneCB_.Get(), 0);

    // Clear and set HDR scene target
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ID3D11RenderTargetView* rtv = sceneRTV_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, clearColor);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f,
        static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(scenePS_.Get(), nullptr, 0);

    ID3D11Buffer* cb = sceneCB_.Get();
    context_->VSSetConstantBuffers(0, 1, &cb);
    context_->PSSetConstantBuffers(0, 1, &cb);

    // Bind cubemap (t0) and color map (t1)
    ID3D11ShaderResourceView* srvs[2] = { cubemapSRV_.Get(), colorMapSRV_.Get() };
    context_->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samplers[2] = { linearSampler_.Get(), pointSampler_.Get() };
    context_->PSSetSamplers(0, 2, samplers);

    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->Draw(3, 0);

    // Unbind SRVs
    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, nullSRVs);
}

void D3D11BlackHoleBackend::RenderBrightnessPass() {
    // Update post CB with bloom threshold
    D3D11_MAPPED_SUBRESOURCE postMapped{};
    if (FAILED(context_->Map(postCB_.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &postMapped)))
        return;
    auto* pc = static_cast<PostConstants*>(postMapped.pData);
    pc->brightThreshold = 1.0f;
    pc->bloomStrength = 0.0f;
    pc->gamma = 1.0f;
    pc->tonemapFlags = 0;
    context_->Unmap(postCB_.Get(), 0);

    // Update resolution CB with scene dimensions
    D3D11_MAPPED_SUBRESOURCE resMapped{};
    if (FAILED(context_->Map(resolutionCB_.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &resMapped)))
        return;
    auto* rc = static_cast<ResolutionConstants*>(resMapped.pData);
    rc->sourceSizeX = static_cast<float>(width_);
    rc->sourceSizeY = static_cast<float>(height_);
    rc->padX = 0.0f;
    rc->padY = 0.0f;
    context_->Unmap(resolutionCB_.Get(), 0);

    // Render to brightness target (half-res)
    ID3D11RenderTargetView* rtv = brightnessRTV_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    const float clearColor[4] = { 0, 0, 0, 0 };
    context_->ClearRenderTargetView(rtv, clearColor);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f,
        static_cast<float>(bloomDown_[0].width),
        static_cast<float>(bloomDown_[0].height), 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);

    context_->PSSetShader(brightExtractPS_.Get(), nullptr, 0);

    ID3D11ShaderResourceView* sceneSrv = sceneSRV_.Get();
    context_->PSSetShaderResources(0, 1, &sceneSrv);

    ID3D11Buffer* cbs[2] = { sceneCB_.Get(), postCB_.Get() };
    context_->PSSetConstantBuffers(0, 2, cbs);

    ID3D11SamplerState* samplers[2] = { linearSampler_.Get(), pointSampler_.Get() };
    context_->PSSetSamplers(0, 2, samplers);

    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    context_->PSSetShaderResources(0, 1, &nullSRV);

    // Copy brightness → bloomDown_[0] (same size)
    context_->CopyResource(bloomDown_[0].texture.Get(),
                           brightnessTexture_.Get());
}

void D3D11BlackHoleBackend::RenderDownsampleChain(int levels) {
    // Downsample: bloomDown_[0] → bloomDown_[1] → ... → bloomDown_[levels-1]
    for (int i = 1; i < levels; ++i) {
        // Update resolution CB with source size
        D3D11_MAPPED_SUBRESOURCE resMapped{};
        if (FAILED(context_->Map(resolutionCB_.Get(), 0,
                D3D11_MAP_WRITE_DISCARD, 0, &resMapped)))
            return;
        auto* rc = static_cast<ResolutionConstants*>(resMapped.pData);
        rc->sourceSizeX = static_cast<float>(bloomDown_[i - 1].width);
        rc->sourceSizeY = static_cast<float>(bloomDown_[i - 1].height);
        rc->padX = 0.0f;
        rc->padY = 0.0f;
        context_->Unmap(resolutionCB_.Get(), 0);

        ID3D11RenderTargetView* rtv = bloomDown_[i].rtv.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        const float clearColor[4] = { 0, 0, 0, 0 };
        context_->ClearRenderTargetView(rtv, clearColor);

        D3D11_VIEWPORT vp{ 0.0f, 0.0f,
            static_cast<float>(bloomDown_[i].width),
            static_cast<float>(bloomDown_[i].height), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp);

        context_->PSSetShader(downsamplePS_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srcSrv = bloomDown_[i - 1].srv.Get();
        context_->PSSetShaderResources(0, 1, &srcSrv);

        ID3D11Buffer* cb = resolutionCB_.Get();
        context_->PSSetConstantBuffers(2, 1, &cb);

        ID3D11SamplerState* samplers[2] = { linearSampler_.Get(), pointSampler_.Get() };
        context_->PSSetSamplers(0, 2, samplers);

        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context_->Draw(3, 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        context_->PSSetShaderResources(0, 1, &nullSRV);
    }
}

void D3D11BlackHoleBackend::RenderUpsampleChain(int levels) {
    // Upsample (additive): bloomDown_[levels-1] → bloomDown_[levels-2] → ... → bloomDown_[1]
    // Each step reads from bloomDown_[i] and additively blends into bloomDown_[i-1]
    for (int i = levels - 1; i >= 1; --i) {
        // Update resolution CB with source size (the smaller texture)
        D3D11_MAPPED_SUBRESOURCE resMapped{};
        if (FAILED(context_->Map(resolutionCB_.Get(), 0,
                D3D11_MAP_WRITE_DISCARD, 0, &resMapped)))
            return;
        auto* rc = static_cast<ResolutionConstants*>(resMapped.pData);
        rc->sourceSizeX = static_cast<float>(bloomDown_[i].width);
        rc->sourceSizeY = static_cast<float>(bloomDown_[i].height);
        rc->padX = 0.0f;
        rc->padY = 0.0f;
        context_->Unmap(resolutionCB_.Get(), 0);

        // Render to bloomDown_[i-1] with additive blending
        ID3D11RenderTargetView* rtv = bloomDown_[i - 1].rtv.Get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);

        D3D11_VIEWPORT vp{ 0.0f, 0.0f,
            static_cast<float>(bloomDown_[i - 1].width),
            static_cast<float>(bloomDown_[i - 1].height), 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp);

        context_->PSSetShader(upsamplePS_.Get(), nullptr, 0);

        ID3D11ShaderResourceView* srcSrv = bloomDown_[i].srv.Get();
        context_->PSSetShaderResources(0, 1, &srcSrv);

        ID3D11Buffer* cb = resolutionCB_.Get();
        context_->PSSetConstantBuffers(2, 1, &cb);

        ID3D11SamplerState* samplers[2] = { linearSampler_.Get(), pointSampler_.Get() };
        context_->PSSetSamplers(0, 2, samplers);

        // Additive blending
        context_->OMSetBlendState(additiveBlendState_.Get(), nullptr, 0xFFFFFFFF);
        context_->Draw(3, 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        context_->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Reset blend state
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

void D3D11BlackHoleBackend::RenderCompositePass(const Settings& s) {
    // Update post CB
    D3D11_MAPPED_SUBRESOURCE postMapped{};
    if (FAILED(context_->Map(postCB_.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &postMapped)))
        return;
    auto* pc = static_cast<PostConstants*>(postMapped.pData);
    pc->bloomStrength = s.bloomStrength;
    pc->brightThreshold = 1.0f;
    pc->gamma = s.gamma;
    pc->tonemapFlags = s.tonemappingEnabled ? 1u : 0u;
    context_->Unmap(postCB_.Get(), 0);

    ID3D11RenderTargetView* rtv = compositeRTV_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    const float clearColor[4] = { 0, 0, 0, 0 };
    context_->ClearRenderTargetView(rtv, clearColor);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f,
        static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);

    context_->PSSetShader(compositePS_.Get(), nullptr, 0);

    // t0 = scene, t1 = bloom (bloomDown_[0] has final bloom result)
    ID3D11ShaderResourceView* srvs[2] = {
        sceneSRV_.Get(),
        bloomDown_[0].srv.Get()
    };
    context_->PSSetShaderResources(0, 2, srvs);

    ID3D11Buffer* cb = postCB_.Get();
    context_->PSSetConstantBuffers(1, 1, &cb);

    ID3D11SamplerState* samplers[2] = { linearSampler_.Get(), pointSampler_.Get() };
    context_->PSSetSamplers(0, 2, samplers);

    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
    context_->PSSetShaderResources(0, 2, nullSRVs);
}

void D3D11BlackHoleBackend::RenderTonemapPass(const Settings& s) {
    // Update post CB if not already set
    D3D11_MAPPED_SUBRESOURCE postMapped{};
    if (FAILED(context_->Map(postCB_.Get(), 0,
            D3D11_MAP_WRITE_DISCARD, 0, &postMapped)))
        return;
    auto* pc = static_cast<PostConstants*>(postMapped.pData);
    pc->bloomStrength = s.bloomStrength;
    pc->brightThreshold = 1.0f;
    pc->gamma = s.gamma;
    pc->tonemapFlags = s.tonemappingEnabled ? 1u : 0u;
    context_->Unmap(postCB_.Get(), 0);

    ID3D11RenderTargetView* rtv = outputRTV_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    const float clearColor[4] = { 0, 0, 0, 0 };
    context_->ClearRenderTargetView(rtv, clearColor);

    D3D11_VIEWPORT vp{ 0.0f, 0.0f,
        static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);

    context_->PSSetShader(tonemapPS_.Get(), nullptr, 0);

    // Source is composite (or scene if bloom was skipped — both go through composite)
    ID3D11ShaderResourceView* srcSrv = compositeSRV_.Get();
    context_->PSSetShaderResources(0, 1, &srcSrv);

    ID3D11Buffer* cb = postCB_.Get();
    context_->PSSetConstantBuffers(1, 1, &cb);

    ID3D11SamplerState* samplers[2] = { linearSampler_.Get(), pointSampler_.Get() };
    context_->PSSetSamplers(0, 2, samplers);

    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSRV = nullptr;
    context_->PSSetShaderResources(0, 1, &nullSRV);
}

bool D3D11BlackHoleBackend::CopyToStaging() {
    context_->CopyResource(stagingTexture_.Get(), outputTexture_.Get());

    D3D11_MAPPED_SUBRESOURCE stagingMapped{};
    if (FAILED(context_->Map(stagingTexture_.Get(), 0,
            D3D11_MAP_READ, 0, &stagingMapped))) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(bufferMutex_);
        const uint32_t srcPitch = stagingMapped.RowPitch;
        const uint32_t dstPitch = frameWidth_ * 4;
        const uint8_t* src = static_cast<const uint8_t*>(stagingMapped.pData);
        for (uint32_t row = 0; row < frameHeight_; ++row) {
            std::memcpy(frameBuffer_.data() + row * dstPitch,
                        src + row * srcPitch, dstPitch);
        }
        frameReady_.store(true);
    }

    context_->Unmap(stagingTexture_.Get(), 0);
    return true;
}

bool D3D11BlackHoleBackend::CopyFramePixels(std::vector<uint8_t>& outBuffer,
                                            uint32_t& outWidth, uint32_t& outHeight) {
    if (!frameReady_.load()) return false;
    std::lock_guard<std::mutex> lock(bufferMutex_);
    outBuffer = frameBuffer_;
    outWidth = frameWidth_;
    outHeight = frameHeight_;
    return true;
}

void D3D11BlackHoleBackend::Suspend() {
    suspended_.store(true);
}

void D3D11BlackHoleBackend::Resume() {
    suspended_.store(false);
}

void D3D11BlackHoleBackend::Shutdown() {
    if (!initialized_.exchange(false)) return;
    ReleaseAll();
}

void D3D11BlackHoleBackend::ReleaseAll() {
    if (context_) context_->ClearState();
    ReleaseSizeResources();
    additiveBlendState_.Reset();
    colorMapSRV_.Reset();
    colorMapTexture_.Reset();
    cubemapSRV_.Reset();
    cubemapTexture_.Reset();
    pointSampler_.Reset();
    linearSampler_.Reset();
    resolutionCB_.Reset();
    postCB_.Reset();
    sceneCB_.Reset();
    tonemapPS_.Reset();
    compositePS_.Reset();
    upsamplePS_.Reset();
    downsamplePS_.Reset();
    brightExtractPS_.Reset();
    scenePS_.Reset();
    vertexShader_.Reset();
    inputLayout_.Reset();
    context_.Reset();
    device_.Reset();
    std::lock_guard<std::mutex> lock(bufferMutex_);
    frameBuffer_.clear();
    frameWidth_ = 0;
    frameHeight_ = 0;
    frameReady_.store(false);
}

} // namespace BlackHole
