#pragma once

struct DiskPreset {
    float temp, incl, roll, inner, outer, opac, dopp, beam, gain, contr, wind, speed, expo, star;
};

struct BlackholeConfig {
    int mode = 0;
    int idleSec = 30;
    float slotSec = 1.0f;
    int playMode = 0;
    bool videoAsIdle = false;
    bool autoStart = false;
    bool fixedSize = false;
    float fixedLevel = 0.0f;
    int captureMode = 0;
    int displayMode = 0;
    int presetCount = 0;
    DiskPreset presets[64]{};
};
