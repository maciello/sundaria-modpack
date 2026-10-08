#pragma once
// SDK-free logic for Ability dump: plain records + YAML writer, testable with the host compiler.
#include <cstdio>
#include <string>
#include <vector>

namespace ability_dump {
    // UBP_GameplayAnimNotify_C::mGameplayAnimNotifyType (EGameplayAnimNotifyType, Archon_structs.hpp)
    enum NotifyType { ApplyEffect = 0, ShootProjectile = 1, AnimLockStart = 2, AnimLockEnd = 3, NotifyTypes };

    struct NotifyCounts {
        int byType[NotifyTypes] = {};
        int other = 0;  // not a UBP_GameplayAnimNotify_C (sounds, vfx, ...)
        void Add(int type) { if (type >= 0 && type < NotifyTypes) byType[type]++; else other++; }
        int Hits() const { return byType[ApplyEffect] + byType[ShootProjectile]; }
    };

    struct Montage {
        std::string path;       // soft path (unloaded) or object name
        bool loaded = false;
        float length = 0;
        NotifyCounts notifies;  // meaningful only when loaded
    };

    struct Magnitude {          // GE DurationMagnitude
        std::string geClass;    // empty = no GE
        int calcType = -1;      // 0 ScalableFloat 1 AttributeBased 2 Custom 3 SetByCaller
        float value = 0;        // ScalableFloat.Value; only meaningful when calcType == 0
        std::string curveRow;   // non-empty = value is scaled by a curve
    };

    struct Ability {
        std::string cls;
        std::vector<std::string> tags;
        Magnitude cooldown, castTime;
        std::vector<Montage> montages;
    };

    struct Live {
        bool valid = false;     // false = no local ability montage playing
        std::string ability;
        Montage montage;
        float position = -1;    // unavailable without UFunction calls
    };

    inline std::string Q(const std::string& s) {  // YAML double-quoted scalar
        std::string o = "\"";
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
            else o += c;
        }
        return o + "\"";
    }

    inline std::string F(float v) { char b[32]; std::snprintf(b, sizeof b, "%.4g", v); return b; }

    inline void Counts(std::string& o, const char* ind, const Montage& m) {
        if (!m.loaded) return;
        o += std::string(ind) + "length: " + F(m.length) + "\n";
        o += std::string(ind) + "notifies: {ApplyEffect: " + std::to_string(m.notifies.byType[ApplyEffect])
           + ", ShootProjectile: " + std::to_string(m.notifies.byType[ShootProjectile])
           + ", AnimLockStart: " + std::to_string(m.notifies.byType[AnimLockStart])
           + ", AnimLockEnd: " + std::to_string(m.notifies.byType[AnimLockEnd])
           + ", other: " + std::to_string(m.notifies.other) + "}\n";
        o += std::string(ind) + "hits: " + std::to_string(m.notifies.Hits()) + "\n";
    }

    inline void Mag(std::string& o, const char* key, const Magnitude& m) {
        if (m.geClass.empty()) { o += std::string("    ") + key + ": null\n"; return; }
        o += std::string("    ") + key + ": {ge: " + Q(m.geClass) + ", calcType: " + std::to_string(m.calcType);
        if (m.calcType == 0) o += ", value: " + F(m.value);
        if (!m.curveRow.empty()) o += ", curveRow: " + Q(m.curveRow);
        o += "}\n";
    }

    inline std::string ToYaml(const std::vector<Ability>& abilities, const Live& live) {
        std::string o = "# dos-tool ability dump\nlive:\n";
        if (!live.valid) o += "  playing: null\n";
        else {
            o += "  ability: " + Q(live.ability) + "\n  montage: " + Q(live.montage.path) + "\n";
            Counts(o, "  ", live.montage);
        }
        o += "abilities:\n";
        for (const Ability& a : abilities) {
            o += "  - class: " + Q(a.cls) + "\n    tags: [";
            for (size_t i = 0; i < a.tags.size(); i++) o += (i ? ", " : "") + Q(a.tags[i]);
            o += "]\n";
            Mag(o, "cooldown", a.cooldown);
            Mag(o, "castTime", a.castTime);
            if (a.montages.empty()) { o += "    montages: []\n"; continue; }
            o += "    montages:\n";
            for (const Montage& m : a.montages) {
                o += "      - path: " + Q(m.path) + "\n        loaded: " + (m.loaded ? "true" : "false") + "\n";
                Counts(o, "        ", m);
            }
        }
        return o;
    }
}
