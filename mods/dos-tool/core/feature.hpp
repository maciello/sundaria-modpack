#pragma once
#include <vector>
#include "combat.hpp"
#include "game.hpp"

struct ImFont;

// One player-facing feature = one folder under features/ with one static Feature object.
// The overlay host calls Frame() every Present (render thread: memory reads/writes only)
// and Menu() inside its window. Off() runs once when the toggle goes false: restore vanilla.
namespace feature {
    struct Frame {
        double now;
        float w, h;
        ImFont* font;                  // user-picked display font
        const game::Snapshot& snap;    // refreshed every 100 ms
        const combat::Tracker& combat; // updated every frame
    };

    struct Feature {
        const char* name;
        bool enabled;
        bool wasEnabled;
        Feature(const char* n, bool on) : name(n), enabled(on), wasEnabled(on) { All().push_back(this); }
        virtual void OnFrame(const Frame&) {}
        virtual void Menu() {}
        virtual void Off() {}
        static std::vector<Feature*>& All() { static std::vector<Feature*> v; return v; }
    };
}
