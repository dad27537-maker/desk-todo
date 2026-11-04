#include "Components/BlackHole/D3D11BlackHoleBackend.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        BlackHole::D3D11BlackHoleBackend backend;
        BlackHole::SurfaceDescriptor surface; surface.width = surface.height = 128;
        if (!backend.Initialize(surface)) throw std::runtime_error("D3D shader/device initialization");
        BlackHole::Settings settings;
        BlackHole::FrameContext frame; frame.monotonicSeconds = 1.5;
        BlackHole::FrameStats stats;
        std::vector<uint8_t> pixels; uint32_t width = 0, height = 0;
        if (!backend.Render(settings, frame, stats) || !backend.CopyFramePixels(pixels, width, height) ||
            width != 128 || height != 128 || pixels.size() != 128 * 128 * 4) throw std::runtime_error("render/readback");
        bool lit = false, clear = false, solid = false;
        unsigned minAlpha = 255;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            lit |= pixels[i] > 10 || pixels[i + 1] > 10 || pixels[i + 2] > 10;
            // Bloom can leave a faint glow in the sky; it must still reveal
            // the desktop rather than turn the overlay into an opaque card.
            clear |= pixels[i + 3] < 32; solid |= pixels[i + 3] > 250;
            minAlpha = (std::min)(minAlpha, unsigned(pixels[i + 3]));
            if (pixels[i] > pixels[i + 3] || pixels[i + 1] > pixels[i + 3] || pixels[i + 2] > pixels[i + 3])
                throw std::runtime_error("invalid premultiplied alpha");
        }
        if (!lit || !clear || !solid) {
            std::cerr << "lit=" << lit << " clear=" << clear << " solid=" << solid << " minAlpha=" << minAlpha << '\n';
            throw std::runtime_error("missing visible disk, transparent sky or opaque horizon");
        }
        backend.Suspend();
        if (backend.Render(settings, frame, stats)) throw std::runtime_error("suspend failed");
        backend.Resume(); settings.bloom = false;
        if (!backend.Resize(96, 64, 1) || !backend.Render(settings, frame, stats) ||
            !backend.CopyFramePixels(pixels, width, height) || width != 96 || height != 64)
            throw std::runtime_error("resize/resume without bloom");
        backend.Shutdown();
        if (backend.CopyFramePixels(pixels, width, height)) throw std::runtime_error("stale frame after shutdown");
        std::cout << "Black hole rendering: device, shaders, nonblank pixels, alpha, suspend/resume, resize, shutdown passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
