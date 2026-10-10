#pragma once
#include <cmath>

// Targeting (#104, #107), SDK-free: the game's own aim for targeted abilities, ported from UE 4.27
// AGameplayAbilityTargetActor_Trace::AimWithPlayerController + ClipCameraRayToAbilityRange (BP_SphereTargetTrace_C
// sets bTraceAffectsAimPitch), and which marker a hit gets.
namespace aim {
    struct V3 { float x, y, z; };
    inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }

    // UE FRotator::Vector(): unit direction of a camera rotation (degrees).
    inline V3 Dir(float pitch, float yaw) {
        const float p = pitch * 3.14159265f / 180.0f, y = yaw * 3.14159265f / 180.0f;
        return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
    }

    // Where the camera ray leaves the sphere of radius `range` around `center` (the ability's reach). False: the ray
    // misses that sphere or points away from its centre; `out` is then left unchanged (UE keeps cam + dir * range).
    inline bool ClipCameraRay(V3 cam, V3 dir, V3 center, float range, V3& out) {
        const V3 toCenter = center - cam;
        const float along = Dot(toCenter, dir);
        if (along < 0) return false;
        const float d2 = Dot(toCenter, toCenter) - along * along, r2 = range * range;
        if (d2 > r2) return false;
        out = cam + dir * (along + std::sqrt(r2 - d2));
        return true;
    }

    // The camera's view end before the line trace (step 1 of the aim).
    inline V3 ViewEnd(V3 cam, V3 dir, V3 start, float range) {
        V3 end = cam + dir * range;
        ClipCameraRay(cam, dir, start, range, end);
        return end;
    }

    // Step 2: the line trace from the camera hit something (hit = true, at `hitAt`) or not. The sphere sweep then runs
    // from `start` towards that point (or the view end), always `range` long.
    inline V3 SweepEnd(V3 cam, V3 dir, V3 start, float range, bool hit, V3 hitAt) {
        const V3 viewEnd = ViewEnd(cam, dir, start, range);
        const bool use = hit && Dot(hitAt - start, hitAt - start) <= range * range;
        V3 aim = (use ? hitAt : viewEnd) - start;
        const float l = Len(aim);
        aim = l > 1e-4f ? aim * (1.0f / l) : dir;
        return start + aim * range;
    }

    // EGameplayEffectTargetingType (Archon): TraceAny 0, TraceEnemy 1, TraceFriend 2, Self 3.
    enum class Kind { None, Ally, Enemy };
    // An ability takes part when it aims at others: TraceFriend always (heals), TraceEnemy only when the ability
    // refuses to fire without a valid target (else it is an area/sweep attack, not a pick).
    inline bool Targeted(int targetingType, bool requiresValidTarget) {
        return targetingType == 2 || (targetingType == 1 && requiresValidTarget);
    }
    // Marker for what the sweep hit: the game decides friend/enemy with GameState.I_AreActorsEnemies afterwards.
    inline Kind Mark(int targetingType, bool hitSomeone, bool hitIsSelf, bool areEnemies, bool alive) {
        if (!hitSomeone || hitIsSelf || !alive) return Kind::None;
        if (targetingType == 2) return areEnemies ? Kind::None : Kind::Ally;
        if (targetingType == 1) return areEnemies ? Kind::Enemy : Kind::None;
        return Kind::None;
    }
}
