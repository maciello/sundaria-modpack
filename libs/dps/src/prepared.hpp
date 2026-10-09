#pragma once
// A build compiled per weapon set (2H set, 1H set), with its attribute rows ready: scoring an item = a row delta + one
// kernel call. Shared by model.cpp (Dps, Prepare), score.cpp, bis.cpp, weights.cpp.
#include "compile.hpp"
#include "dps/dps.hpp"
#include "kernel.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace dps {
    namespace detail {
        // an item's stats in one compiled row layout
        struct Sparse {
            std::vector<std::pair<int, float>> attr;
            std::array<float, layout::kElements> elem{};   // WeaponDamage_<element>
            std::vector<std::string> ignored;
        };
        // offhand: a weapon in the other hand adds its stats to the hero, but its WeaponDamage and WeaponDamage_<element>
        // stay out (damage reads the attacking weapon's set: GetWeaponDamage; which hand attacks: unverified, main assumed)
        Sparse ToSparse(const compile::Compiled& c, const Item& it, bool offhand = false);
        struct Row {
            std::vector<float> x;                          // without the weapon-element pair
            std::array<float, layout::kElements> elem{};
            void Add(const Sparse& s, float sign = 1.f);
        };
        // kernel row: x + weapon element = first element (EMagicDamageType order) with a value (GetWeaponDamage)
        float Eval(const compile::Compiled& c, const Row& r, kernel::Detail* d = nullptr);

        bool IsWeaponSlot(const std::string& equipSlot);
        int Capacity(const std::string& equipSlot);   // equip slots of this kind on a hero (Ring, Trinket: 2)
        std::array<float, 6> Primary(const Tables& t, const Build& b);

        struct Mode {                     // one weapon set
            compile::Compiled c;
            const Item* main = nullptr;
            const Item* off = nullptr;    // 1H set: shield / orb / second weapon
            Row row;                      // base + common gear + main + off
            float dps = 0;
        };
        struct WeaponSet { const Item* main = nullptr; const Item* off = nullptr; };
        // compile + assemble one weapon set over the build's common gear; error set when nothing compiles
        Mode MakeMode(const Tables& t, const Build& b, const Scenario& sc, const std::vector<const Item*>& common, WeaponSet w,
                      std::string& error);
        std::string AnimOf(const Tables& t, const Item* it);
    }

    class Prepared {
    public:
        Prepared() = default;
        Prepared(const Prepared&) = delete;   // modes point into build.equipped
        Build build;
        Scenario scenario;
        std::vector<const Item*> common;   // equipped non-weapons (point into build.equipped)
        std::vector<detail::Mode> modes;   // weapon sets that compiled
        int best = -1;                     // index into modes
        std::string error;
        float dps() const { return best < 0 ? 0.f : modes[best].dps; }
    };
}
