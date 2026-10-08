#pragma once
#include <cmath>
#include <cstdio>
#include <string>

// SDK-free logic for Graphics: the engine console variables we set, and how to talk to the console.
namespace graphics {
    struct Row {
        const char* cvar;
        const char* label;
        float value;     // what we set
        float lo, hi;    // menu slider range
        bool on;         // default
    };

    // Values: lower LOD distance scale = full-detail models further away; Sharpen 0.6 counters TAA blur.
    inline Row kRows[] = {
        {"r.MotionBlurQuality",          "Motion blur (0 = off)",              0,    0,    4,    true},
        {"r.OneFrameThreadLag",          "Frame thread lag (0 = less input lag)", 0, 0,    1,    true},
        {"t.MaxFPS",                     "FPS cap (0 = uncapped)",            144,  0,    360,  false},
        {"r.Tonemapper.Sharpen",         "Sharpen",                           0.6f, 0,    2,    true},
        {"r.MaxAnisotropy",              "Texture filtering (anisotropy)",    16,   1,    16,   true},
        {"r.StaticMeshLODDistanceScale", "Model detail distance (lower = sharper)", 0.5f, 0.1f, 2, true},
        {"r.SkeletalMeshLODRadiusScale", "Character detail distance (lower = sharper)", 0.5f, 0.1f, 2, true},
        {"r.ViewDistanceScale",          "View distance",                     1.5f, 0.5f, 3,    true},
        {"foliage.LODDistanceScale",     "Foliage detail distance",           2,    0.5f, 4,    true},
        {"r.DetailMode",                 "Detail meshes (2 = all)",           2,    0,    2,    true},
        {"r.Shadow.MaxResolution",       "Shadow resolution",                 4096, 512,  8192, false},
        {"r.Shadow.DistanceScale",       "Shadow distance",                   1.5f, 0.5f, 3,    false},
        {"r.MipMapLODBias",              "Texture sharpness (negative = sharper, may shimmer)", -1, -2, 1, false},
    };
    constexpr int kCount = int(sizeof(kRows) / sizeof(kRows[0]));

    // "r.Name 2" for whole numbers (int cvars reject "2.000000"), else "r.Name 0.6".
    inline std::string Command(const char* cvar, float v) {
        char buf[160];
        if (std::fabs(v - std::round(v)) < 1e-4f) std::snprintf(buf, sizeof(buf), "%s %d", cvar, int(std::lround(v)));
        else std::snprintf(buf, sizeof(buf), "%s %g", cvar, v);
        return buf;
    }

    // Int cvars round what we send; compare loosely.
    inline bool Same(float a, float b) { return std::fabs(a - b) < 0.01f + 0.001f * std::fabs(b); }

    // Per-row state on the game thread.
    enum class State { Untouched, Applied, Rejected };

    // What to do with one row this tick (current = live cvar value, target = ours).
    enum class Step { None, Capture, Set, Restore };
    inline Step Next(bool want, bool haveOrig, State s, float current, float target) {
        if (!want) return s == State::Applied ? Step::Restore : Step::None;
        if (!haveOrig) return Step::Capture;
        if (s == State::Rejected) return Step::None;
        return Same(current, target) ? Step::None : Step::Set;  // drift: the game's settings menu re-applied its own
    }
}
