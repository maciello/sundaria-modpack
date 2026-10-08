#pragma once
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

// SDK-free: the hub layout file (dos-tool-tavern.ini next to the game exe), one "name=x,y,z,yaw" line per entry:
// NPC spots, ROOM / ROOM_WALLS, MAP / MAP_SIZE, PROP:<actor>. Shared by the hub features.
namespace hub_layout {
    struct Spot { std::string name; float x, y, z, yaw; };

    inline std::vector<Spot> ParseLayout(const std::string& text) {
        std::vector<Spot> out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0) continue;
            Spot s{line.substr(0, eq), 0, 0, 0, 0};
            if (std::sscanf(line.c_str() + eq + 1, "%f,%f,%f,%f", &s.x, &s.y, &s.z, &s.yaw) != 4) continue;
            for (Spot& o : out) if (o.name == s.name) { o = s; s.name.clear(); }  // a repeated name: last line wins
            if (!s.name.empty()) out.push_back(s);
        }
        return out;
    }

    inline std::string WriteLayout(const std::vector<Spot>& spots) {
        std::string out = "# tavern hub: NPC=x,y,z,yaw (written by dos-tool)\n";
        char buf[256];
        for (const Spot& s : spots) {
            std::snprintf(buf, sizeof(buf), "%s=%.1f,%.1f,%.1f,%.1f\n", s.name.c_str(), s.x, s.y, s.z, s.yaw);
            out += buf;
        }
        return out;
    }

    // Insert or replace the spot for s.name.
    inline void SetSpot(std::vector<Spot>& spots, const Spot& s) {
        for (Spot& o : spots) if (o.name == s.name) { o = s; return; }
        spots.push_back(s);
    }
}
