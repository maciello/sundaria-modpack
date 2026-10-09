#pragma once
// Engine particle effects in the world, for any feature: a game Cascade (UParticleSystem) or Niagara (UNiagaraSystem)
// template spawned attached to an actor/component or at a location, tinted, and removed. Depth-tested, under the
// game UI, moving with what it is attached to: use this for world effects, never ImGui (.claude/rules/design.md).
// Templates + their parameters: skill references/fx.md. Game thread only (inside a game::On / OnWorldTick callback),
// except Release() (Off()).
#include <cstdint>

namespace fx {
    using Id = std::uint64_t;  // 0 = nothing spawned

    struct Place {
        float x = 0, y = 0, z = 0;  // cm: offset from the parent (Attach) or world location (At)
        float scale = 1;            // uniform component scale: volume, sprite size and speed of the template
        float cull = 0;             // cm: not drawn beyond (0 = engine default)
    };

    // path = object path "/Game/Dir/Name.Name"; loaded once per path, cached. owner = the feature's tag for Release().
    // parent = AActor* (its root) or USceneComponent*. 0 = template missing / not a particle system / no parent.
    Id Attach(const char* owner, const wchar_t* path, void* parent, const Place& p);
    Id At(const char* owner, const wchar_t* path, const Place& p);
    bool Alive(Id id);

    // Instance parameters the template exposes (Cascade: a *ParticleParameter distribution's name; Niagara: "User.Name").
    void Color(Id id, const wchar_t* name, float r, float g, float b, float a = 1);
    void Float(Id id, const wchar_t* name, float v);
    void Vector(Id id, const wchar_t* name, float x, float y, float z);
    // Material parameters of emitter `element` (its material gets a dynamic instance on first use). The colour route
    // for Cascade templates, which expose no colour instance parameter: linear colour, HDR allowed.
    void MaterialColor(Id id, int element, const wchar_t* name, float r, float g, float b, float a = 1);
    void MaterialFloat(Id id, int element, const wchar_t* name, float v);

    void Remove(Id id);
    // Off(): every effect `owner` spawned is destroyed on the next world tick (game::Drain), else here after 500 ms.
    void Release(const char* owner);
}
