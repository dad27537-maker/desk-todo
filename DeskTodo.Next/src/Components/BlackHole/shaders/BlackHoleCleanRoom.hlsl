// Clean-room shader skeleton. It contains no code or assets from rossning92/Blackhole.
cbuffer BlackHoleConstants : register(b0) {
    float2 viewportSize;
    float elapsedSeconds;
    float mass;
    float diskInnerRadius;
    float diskOuterRadius;
    float diskThickness;
    float diskBrightness;
    float rotationSpeed;
    float exposure;
    uint integrationSteps;
    float3 cameraPosition;
    float cameraFov;
};

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
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

float Hash31(float3 value) {
    value = frac(value * 0.1031);
    value += dot(value, value.yzx + 19.19);
    return frac((value.x + value.y) * value.z);
}

float3 ProceduralSky(float3 direction) {
    float3 cell = floor(direction * 420.0);
    float star = step(0.9975, Hash31(cell));
    float horizon = pow(saturate(1.0 - abs(direction.y)), 5.0);
    return star.xxx * 2.8 + float3(0.015, 0.025, 0.06) +
           horizon * float3(0.025, 0.012, 0.04);
}

float DiskDensity(float3 position) {
    float radius = length(position.xz);
    float radial = smoothstep(diskInnerRadius, diskInnerRadius + 0.4, radius) *
                   (1.0 - smoothstep(diskOuterRadius - 0.8, diskOuterRadius, radius));
    float vertical = exp(-abs(position.y) / max(diskThickness, 0.001));
    float bands = 0.65 + 0.35 * sin(radius * 8.0 - elapsedSeconds * rotationSpeed * 10.0);
    return radial * vertical * bands;
}

float3 DiskEmission(float3 position, float density) {
    float radius = max(length(position.xz), diskInnerRadius);
    float heat = saturate((diskOuterRadius - radius) /
                          max(diskOuterRadius - diskInnerRadius, 0.001));
    float3 cool = float3(1.0, 0.18, 0.025);
    float3 hot = float3(1.0, 0.92, 0.68);
    return lerp(cool, hot, heat * heat) * density * diskBrightness;
}

float4 PSMain(PixelInput input) : SV_TARGET {
    float2 screen = input.uv * 2.0 - 1.0;
    screen.x *= viewportSize.x / max(viewportSize.y, 1.0);
    float3 rayDirection = normalize(float3(screen * cameraFov, 1.0));
    float3 position = cameraPosition;
    float3 radiance = 0.0;
    float transmittance = 1.0;
    float angularMomentum2 = dot(cross(position, rayDirection),
                                 cross(position, rayDirection));
    float stepLength = 0.09;

    [loop]
    for (uint stepIndex = 0; stepIndex < min(integrationSteps, 160u); ++stepIndex) {
        float radius2 = dot(position, position);
        if (radius2 <= mass * mass) {
            transmittance = 0.0;
            break;
        }
        if (radius2 > diskOuterRadius * diskOuterRadius * 5.0 && stepIndex > 16) break;

        float inverseRadius5 = rsqrt(max(radius2 * radius2 * radius2 * radius2 * radius2,
                                         0.000001));
        float3 bending = -1.5 * mass * angularMomentum2 * position * inverseRadius5;
        rayDirection = normalize(rayDirection + bending * stepLength);
        position += rayDirection * stepLength;

        float density = DiskDensity(position) * stepLength;
        radiance += transmittance * DiskEmission(position, density);
        transmittance *= exp(-density * 0.45);
        if (transmittance < 0.01) break;
    }

    radiance += transmittance * ProceduralSky(rayDirection);
    radiance = 1.0 - exp(-radiance * exposure);
    radiance = pow(saturate(radiance), 1.0 / 2.2);
    return float4(radiance, 1.0);
}
