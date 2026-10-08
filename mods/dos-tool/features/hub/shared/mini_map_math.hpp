#pragma once
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

// SDK-free: the world map diorama shrunk onto a desk. Every scene actor keeps its place relative to the diorama's
// centre, scaled by k and turned by the desk's yaw; its own scale multiplies by k and its yaw adds the desk's.
namespace mini_map {
    struct Xf { float x, y, z, yaw, scale; };

    inline Xf Shrink(const Xf& a, float cx, float cy, float cz, float deskX, float deskY, float deskZ, float deskYaw, float k) {
        const float r = deskYaw * 3.14159265f / 180.0f, c = std::cos(r), s = std::sin(r);
        const float dx = (a.x - cx) * k, dy = (a.y - cy) * k, dz = (a.z - cz) * k;
        return {deskX + dx * c - dy * s, deskY + dx * s + dy * c, deskZ + dz, a.yaw + deskYaw, a.scale * k};
    }

    // The scene as the hub built it, recorded once while it stands on its plateau (dos-tool-worldmap.ini), so a
    // later run never mistakes a moved map for the original. One actor per line: name x y z pitch yaw roll scale button;
    // first line: CENTER x y z.
    struct SceneActor { std::string name; float x, y, z, pitch, yaw, roll, scale; bool button; };
    struct Scene { float cx = 0, cy = 0, cz = 0; std::vector<SceneActor> actors; };

    inline std::string WriteScene(const Scene& s) {
        std::string out;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "CENTER %.2f %.2f %.2f\n", s.cx, s.cy, s.cz);
        out += buf;
        for (const SceneActor& a : s.actors) {
            std::snprintf(buf, sizeof(buf), "%s %.2f %.2f %.2f %.3f %.3f %.3f %.5f %d\n", a.name.c_str(), a.x, a.y, a.z, a.pitch, a.yaw,
                          a.roll, a.scale, a.button ? 1 : 0);
            out += buf;
        }
        return out;
    }

    inline bool ReadScene(const std::string& text, Scene& s) {
        s = Scene{};
        std::istringstream in(text);
        std::string line;
        bool centre = false;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            char name[200] = {};
            SceneActor a{};
            int b = 0;
            if (std::sscanf(line.c_str(), "CENTER %f %f %f", &s.cx, &s.cy, &s.cz) == 3) { centre = true; continue; }
            if (std::sscanf(line.c_str(), "%199s %f %f %f %f %f %f %f %d", name, &a.x, &a.y, &a.z, &a.pitch, &a.yaw, &a.roll, &a.scale, &b) != 9) continue;
            a.name = name;
            a.button = b != 0;
            s.actors.push_back(a);
        }
        return centre && !s.actors.empty();
    }
}
