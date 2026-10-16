#pragma once
#include <string>
#include <string_view>

// Insert menu drawers (#142), SDK-free: a feature's coarse category, read from the folder its .cpp lives in
// (features/<top>/[<sub>/]x.cpp, captured by feature::Feature's constructor). A new feature inside an existing folder
// lands in the right drawer without being listed anywhere; a new top folder falls into Other until added here.
namespace menu_groups {
    enum class Group { Combat, Abilities, Movement, CameraGraphics, Loot, World, Dev, Other, Count };

    inline const char* Name(Group g) {
        static const char* const kNames[] = {"Combat HUD", "Abilities", "Movement & feel", "Camera & graphics", "Loot & items",
                                             "World & dungeons", "Dev tools", "Other"};
        return kNames[int(g)];
    }

    // The folder names below features/ ("targeting", "cast-mode") from a source path with / or \ separators.
    inline void Folders(std::string_view path, std::string& top, std::string& sub) {
        std::string p(path);
        for (char& c : p) if (c == '\\') c = '/';
        top.clear(), sub.clear();
        size_t at = p.rfind("/features/");
        if (at != std::string::npos) at += 10;
        else if (p.rfind("features/", 0) == 0) at = 9;  // a relative path from the mod folder
        else return;
        std::string_view rest = std::string_view(p).substr(at);  // "<top>/<sub>/x.cpp" or "<top>/x.cpp"
        const size_t a = rest.find('/');
        if (a == std::string::npos) return;
        top = std::string(rest.substr(0, a));
        rest = rest.substr(a + 1);
        const size_t b = rest.find('/');
        if (b != std::string::npos) sub = std::string(rest.substr(0, b));
    }

    inline Group Of(std::string_view source) {
        std::string top, sub;
        Folders(source, top, sub);
        // developer tools first: they sit inside the domains they probe
        for (const std::string& f : {top, sub})
            if (f.find("probe") != std::string::npos || f == "live-bridge" || f == "anim-test" || f == "pose-recorder" || f == "char-snapshot")
                return Group::Dev;
        struct Map { const char* folder; Group group; };
        static const Map kMap[] = {
            {"damage-numbers", Group::Combat}, {"health-bars", Group::Combat}, {"dps-meter", Group::Combat}, {"cast-indicator", Group::Combat},
            {"ability-mods", Group::Abilities}, {"targeting", Group::Abilities}, {"input-feel", Group::Abilities},
            {"dual-wield-damage", Group::Abilities},
            {"air-control", Group::Movement}, {"game-feel", Group::Movement},
            {"camera", Group::CameraGraphics}, {"free-camera", Group::CameraGraphics}, {"graphics", Group::CameraGraphics},
            {"inventory", Group::Loot}, {"loot", Group::Loot},
            {"dungeon", Group::World}, {"hub", Group::World}, {"boss-intro", Group::World}, {"progress", Group::World},
        };
        for (const Map& m : kMap) if (top == m.folder) return m.group;
        return Group::Other;
    }
}
