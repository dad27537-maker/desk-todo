#include "gui_config.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

static const DiskPreset kDefaults[16] = {
    {5500,1.50f,.35f,1.8f,8,.90f,.60f,2.5f,2.2f,1.6f,7,5,1.40f,0},
    {4500,1.52f,.10f,2.2f,7,.85f,.35f,2,1.4f,.5f,7,5,1.20f,0},
    {3800,.55f,-.30f,2.2f,6,.45f,.90f,3.5f,1.6f,.4f,3,2.5f,1.10f,0},
    {6500,.30f,0,3,10,.50f,.80f,2.5f,1,1.1f,7,5,1,0},
    {15000,1.30f,.35f,3,14,.35f,1,4,1.2f,1.3f,8,5,.80f,0},
    {18000,1.05f,.55f,3,16,.30f,1,5,1,1.5f,9,6,.75f,0},
    {5500,1.50f,.35f,1.8f,8,0,1,2.5f,0,1.6f,7,5,1,.6f},
    {5500,1.50f,.35f,1.8f,8,.90f,.60f,2.5f,2.2f,1.6f,7,5,1.40f,0},
    {3200,1.45f,.60f,2,9,.95f,.20f,1.5f,3,2,5,4,1.60f,0},
    {8000,1.20f,-.50f,2.5f,7,.70f,.75f,2.8f,1.8f,1.4f,8,5.5f,1.30f,0},
    {2500,1.55f,.20f,1.6f,6,.60f,.10f,1.2f,2.6f,1.8f,4,3,1.50f,0},
    {12000,.80f,.45f,2.8f,12,.40f,.95f,3.5f,1.5f,1.2f,8.5f,5,.90f,0},
    {5000,.10f,0,2,9,.85f,.50f,2,1.3f,1.5f,6,4.5f,1.10f,0},
    {22000,1.40f,.70f,3.5f,18,.25f,1,4.5f,.9f,1.7f,10,6.5f,.70f,0},
    {4200,1.48f,.15f,1.9f,7.5f,.80f,.45f,2.2f,2,.8f,6.5f,4.8f,1.25f,0},
    {9000,.45f,-.15f,2.6f,11,.55f,.85f,3,1.1f,1.3f,7.5f,5.2f,1.05f,0},
};

void InitDefaultPresets(BlackholeConfig& cfg) {
    cfg.mode = 0;
    cfg.presetCount = 16;
    for (int i = 0; i < 16; ++i) cfg.presets[i] = kDefaults[i];
}

bool LoadPresetsFromFile(BlackholeConfig& cfg, char names[64][64]) {
    FILE* file = nullptr;
    if (fopen_s(&file, "blackhole_presets.txt", "r") != 0 || !file) {
        InitDefaultPresets(cfg);
        return false;
    }
    char line[256]{};
    if (!fgets(line, sizeof(line), file) || !strstr(line, "v4") ||
        !fgets(line, sizeof(line), file)) { fclose(file); InitDefaultPresets(cfg); return false; }
    int video=0, startup=0, fixed=0;
    sscanf_s(line, "%d %d %f %d %d %d %d %f %d %d", &cfg.mode, &cfg.idleSec,
        &cfg.slotSec, &cfg.playMode, &video, &startup, &fixed, &cfg.fixedLevel,
        &cfg.captureMode, &cfg.displayMode);
    cfg.videoAsIdle=video!=0; cfg.autoStart=startup!=0; cfg.fixedSize=fixed!=0;
    if (!fgets(line, sizeof(line), file)) { fclose(file); InitDefaultPresets(cfg); return false; }
    cfg.presetCount = (std::max)(1, (std::min)(64, atoi(line)));
    for (int i=0;i<cfg.presetCount;++i) {
        if (!fgets(names[i],64,file) || !fgets(line,sizeof(line),file)) break;
        names[i][strcspn(names[i],"\r\n")]=0;
        auto& p=cfg.presets[i];
        sscanf_s(line,"%f %f %f %f %f %f %f %f %f %f %f %f %f %f",
            &p.temp,&p.incl,&p.roll,&p.inner,&p.outer,&p.opac,&p.dopp,&p.beam,
            &p.gain,&p.contr,&p.wind,&p.speed,&p.expo,&p.star);
    }
    fclose(file); return true;
}

void SavePresetsToFile(const BlackholeConfig&, const char[16][64]) {}
bool GUI_ShowConfigPanel(BlackholeConfig&) { return false; }
extern "C" int glfwInit() { return 0; }
extern "C" void glfwTerminate() {}
