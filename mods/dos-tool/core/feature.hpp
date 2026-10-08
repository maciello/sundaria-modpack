#pragma once
#include <string>
#include <utility>
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
        const std::vector<combat::Sample>& chars; // every character with health, this frame
        float mouseDX = 0, mouseDY = 0;  // raw mouse motion since last frame while a feature captures input (cursor-independent)
        bool rawMouse = false;           // raw mouse input is arriving (else fall back to the cursor position)
    };

    // Kubernetes feature-gate stages. Default on only when Stable; Alpha is shown only with dos-tool.dev.
    // With dos-tool.dev (a developer's install) every non-Deprecated feature defaults on, except optIn ones.
    // The player's on/off choice is local (dos-tool.ini next to the game exe), never in the repo.
    enum class Stage { Alpha, Beta, Stable, Deprecated };

    struct Feature {
        const char* name;
        Stage stage;
        bool enabled;
        bool wasEnabled;
        bool optIn = false;  // off by default even with dos-tool.dev (risky hooks, log spam)
        bool usesCombat = false;  // reads Frame::chars / combat: core samples characters only while an enabled feature sets this
        Feature(const char* n, Stage s) : name(n), stage(s), enabled(s == Stage::Stable), wasEnabled(enabled) { All().push_back(this); }
        virtual void OnFrame(const Frame&) {}
        virtual void Menu() {}
        virtual void Off() {}
        // Own settings, local per install: dos-tool.ini lines "<name>.<key>=<value>". After a change call
        // ImGui::MarkIniSettingsDirty().
        virtual void Load(const char* key, const char* value) {}
        virtual void Save(std::vector<std::pair<std::string, std::string>>& out) {}
        // true = the game gets no key/mouse presses and no cursor (the feature reads keys itself); ignored while the menu is open
        virtual bool CapturesInput() const { return false; }
        virtual bool PassesKey(unsigned vk) const { return false; }  // while capturing: this key still reaches the game
        static std::vector<Feature*>& All() { static std::vector<Feature*> v; return v; }
    };
}
