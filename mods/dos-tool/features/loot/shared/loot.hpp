#pragma once
// Loot actors in the world (items on the floor, chests): tracked from the game's own events, read on the render thread.
// SDK-free API; the SDK side is track.cpp. Facts: references/game-facts.md § loot.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "style.hpp"

namespace loot {
    enum class Kind : std::uint8_t { Item, Chest };

    // Raw state read from the actor (memory reads only).
    struct State {
        Kind kind;
        bool hidden;         // AActor::bHidden
        bool ready;          // item: LootIsReady (false once collected / while respawning); chest: its item factory's IsLootReady
        std::uint8_t anim;   // ABP_TriggerBase_C::mOpenCloseAnimState: 0 closed (an opened chest reads 2)
    };

    // A loot actor the player has not taken yet.
    inline bool Unlooted(const State& s) {
        if (s.hidden || !s.ready) return false;
        return s.kind == Kind::Item || s.anim == 0;
    }

    struct Actor {
        std::uintptr_t id;
        Kind kind;
        float x, y, z;   // world position (root)
        bool unlooted;
        int grade;       // EItemGrade 0..7 (chest: best item inside), -1 unknown
        float seen;      // LastRenderTimeOnScreen of its mesh (occlusion), NaN without a mesh
    };

    // FLinearColor (what GetItemColorForGrade returns) -> the sRGB bytes the game's UI shows (UE ToFColor(true)).
    inline std::uint8_t ToSrgb(float linear) {
        const float c = std::clamp(linear, 0.0f, 1.0f);
        const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        return std::uint8_t(s * 255.0f + 0.5f);
    }

    // Game thread: feed every ProcessEvent from a feature's listener. Tracks loot actors on BeginPlay and once per world.
    void OnEvent(void* obj, void* fn);
    // Render thread: tracked actors within maxDist (cm) of (cx, cy, cz). O(tracked) float compares + reads of the near ones.
    void Read(float cx, float cy, float cz, float maxDist, std::vector<Actor>& out);
    // The game's own grade colours (BP_ArchonClientFunctionLibrary_C::GetItemColorForGrade), read once per world.
    // false until read: use style::rarity meanwhile.
    bool GradeColors(std::array<style::Rgba, 8>& out);
    int Tracked();
    void Reset();  // drop everything (feature off)
}
