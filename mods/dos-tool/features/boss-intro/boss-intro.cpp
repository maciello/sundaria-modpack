#include "feature.hpp"
#include "boss-intro.hpp"
#include "signals.hpp"
#include "logger.hpp"
#include "imgui.h"

#include <cstdio>
#include <string>

// Boss intro (#13): the game's own boss signals (signals.cpp) -> one intro per encounter.
// Supersedable: built to the spec while the maintainer has not picked a style (#13 comment).
namespace {
    using namespace boss_intro;

    struct BossIntro : feature::Feature {
        Detector detector;
        std::string last = "none yet";

        BossIntro() : Feature("Boss intro", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook

        void OnFrame(const feature::Frame& f) override {
            game_side::Listen(true);
            for (const game_side::Event& e : game_side::Take()) {
                const Verdict v = detector.On(e.signal, e.fight, f.now);
                char buf[400];
                std::snprintf(buf, sizeof(buf), "[boss-intro] %s %s \"%s\" \"%s\" -> %s", Name(e.signal),
                              e.fightClass.empty() ? "-" : e.fightClass.c_str(), e.name.c_str(), e.subtitle.c_str(), Name(v.kind));
                logger::log(buf);
                last = buf + 13;
            }
        }

        void Off() override {
            game_side::Listen(false);
            detector = {};
        }

        void Menu() override { ImGui::TextDisabled("last signal: %s", last.c_str()); }
    } g_feature;
}
