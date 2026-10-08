#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "element.hpp"

// SDK-free combat service: diff per-actor health between samples into stacked hit events,
// learn the typical hit, keep fight stats. Consumed by features (damage numbers, DPS meter).
namespace combat {
    // 0 = unknown; seen = game time the mesh was last on screen.
    // hit* = the actor's last hit record (AArchonCharacter::LastTakeHitInfo): hitStamp changes per record,
    // hitType = opaque id of damage-type class × instigator (the class is mostly one per ability: Range_AimedShot vs
    // Range_C; DoTs have their own class), element from the class name,
    // hitDamage = that record's damage (the game sums same-frame hits from one instigator into it, type = the last one),
    // hitBy = the record's instigator pawn (an id like `id`, 0 = none).
    struct Sample {
        uintptr_t id; float x, y, z; float health; bool isPlayer; float maxHealth = 0; float level = 0; float seen = 0;
        unsigned hitStamp = 0; uintptr_t hitType = 0; Element element = Element::Physical; float hitDamage = 0;
        uintptr_t hitBy = 0;
    };

    enum class Kind { Dealt, Taken, Heal };
    struct Number {
        float x, y, z;
        float amount;  // always > 0; running total while stacking
        Kind kind;
        float scale;   // relative size: 1 = your typical single hit
        float drift;   // -1..1 sideways direction
        double born;   // first hit
        uintptr_t id = 0;
        int hits = 1;
        double bump = born;  // last hit merged in
        uintptr_t type = 0;  // damage-type class of the hits stacked here; 0 = untagged (HP drop without a recorded hit)
        Element element = Element::Physical;
        float hitScale = 0;  // biggest single merged hit's size: a crit keeps it, stack growth alone is capped (Shown)
    };

    // Size to draw: a stack grows at most to `cap` (menu "Max stack size"), never below its biggest single hit.
    inline float Shown(const Number& n, float cap) { return n.hits > 1 ? std::max(n.hitScale, std::min(n.scale, cap)) : n.scale; }

    // Fight = damage dealt to non-players with no gap longer than `gap` seconds.
    struct Fight {
        double start = 0, last = 0;
        double total = 0;
        double Duration() const { return std::max(1.0, last - start); }
        double Dps() const { return total / Duration(); }
    };

    struct Tracker {
        double lifetime = 1.4;   // seconds on screen after the last hit
        double grace = 1.5;      // ignore changes this long after first sight (spawn HP fill-up)
        double gap = 5.0;        // fight ends after this long without damage dealt
        double stack = 1.3;      // hits of the same damage type on the same target closer than this merge (DoT ticks ~1.0 s apart)
        // HP loss is the amount; hit records say whose it is. A record claims up to its damage from loss not yet shown,
        // or from loss arriving within `settle` after it (we sample from the render thread: record and HP drop land in
        // either order, and one hit's drop can split over two frames). Loss no record claims shows untagged after `settle`.
        double settle = 0.1;
        float typical = 0;       // EMA of single dealt hits, the "1.0" for scale

        struct Owed { uintptr_t type; Element element; float left; double at; };
        struct Ledger { Sample s; float unclaimed = 0; double at = 0; Owed owed{0, Element::Physical, 0, -1e9}; };
        std::unordered_map<uintptr_t, float> last;
        std::unordered_map<uintptr_t, double> firstSeen;
        std::unordered_map<uintptr_t, unsigned> stamp;   // per actor: last hit-record stamp
        std::unordered_map<uintptr_t, Ledger> ledger;    // per actor: loss/record not matched yet (outlives the actor: kills)
        std::vector<Number> live;
        Fight fight;
        bool inFight = false;

        float Rel(float amount) const {
            const float t = typical > 0 ? typical : amount;
            return std::clamp(1.0f + 0.45f * std::log2(amount / t), 0.8f, 2.4f);
        }
        float Grow(float total) const {  // stack size from its running total: half as steep as a single hit's
            const float t = typical > 0 ? typical : total;
            return std::clamp(1.0f + 0.25f * std::log2(total / t), 0.8f, 2.4f);
        }
        void Learn(float amount) { typical = typical <= 0 ? amount : typical + 0.08f * (amount - typical); }
        float Scale(float amount) { Learn(amount); return Rel(amount); }

        // Stack key = target + kind + damage type (≈ ability). type 0 = no hit record: its own stack.
        void Add(const Sample& s, float amount, Kind kind, double now, uintptr_t type = 0, Element el = Element::Physical) {
            for (Number& n : live)
                if (n.id == s.id && n.kind == kind && n.type == type && now - n.bump <= stack) {
                    n.amount += amount;
                    n.hits++;
                    n.bump = now;
                    n.x = s.x; n.y = s.y; n.z = s.z;
                    if (kind == Kind::Dealt) { n.hitScale = std::max(n.hitScale, Rel(amount)); n.scale = std::max(n.hitScale, Grow(n.amount)); }
                    return;
                }
            const float drift = float((s.id >> 4) % 200) / 100.0f - 1.0f;
            const float scale = kind == Kind::Dealt ? Rel(amount) : kind == Kind::Taken ? 0.95f : 0.9f;
            live.push_back({s.x, s.y, s.z, amount, kind, scale, drift, now, s.id, 1, now, type, el, scale});
        }

        void Show(const Sample& s, float amount, double now, uintptr_t type, Element el) {
            Add(s, amount, s.isPlayer ? Kind::Taken : Kind::Dealt, now, type, el);
            if (!s.isPlayer) Learn(amount);
        }

        // Pay what the open record still claims out of the ledger's unshown loss.
        void Pay(Ledger& l, double now) {
            const float x = std::min(l.unclaimed, l.owed.left);
            if (x <= 0) return;
            l.unclaimed -= x; l.owed.left -= x;
            Show(l.s, x, now, l.owed.type, l.owed.element);
        }

        // HP bouncing back (co-op correction, prediction rollback) first cancels unshown loss, then a live damage
        // stack on this actor, so a dip-rebound-dip counts once: stacks show net HP lost.
        void Rebound(const Sample& s, float heal, double now) {
            if (auto l = ledger.find(s.id); l != ledger.end()) {
                const float back = std::min(heal, l->second.unclaimed);
                l->second.unclaimed -= back; heal -= back;
                if (!s.isPlayer) fight.total -= back;
            }
            for (Number& n : live)
                if (heal > 0 && n.id == s.id && n.kind != Kind::Heal && now - n.bump <= stack) {
                    const float back = std::min(heal, n.amount);
                    n.amount -= back; heal -= back;
                    if (n.kind == Kind::Dealt) { fight.total -= back; n.scale = n.hits > 1 ? Grow(std::max(n.amount, 1.0f)) : (n.hitScale = Rel(std::max(n.amount, 1.0f))); }
                }
            std::erase_if(live, [](const Number& n) { return n.amount <= 0; });
            if (heal > 0) Add(s, heal, Kind::Heal, now);
        }

        void Loss(const Sample& s, float delta, double now) {
            Ledger& l = ledger[s.id];
            l.s = s; l.unclaimed += delta; l.at = now;
            if (now - l.owed.at <= settle) Pay(l, now);  // its record came first
            if (s.isPlayer) return;
            // DPS counts HP lost when it happens, whatever the type
            if (!inFight || now - fight.last > gap) { fight = {now, now, 0}; inFight = true; }
            fight.total += delta;
            fight.last = now;
        }

        void Record(const Sample& s, double now) {
            Ledger& l = ledger[s.id];
            l.s = s;
            l.owed = {s.hitType, s.element, s.hitDamage, now};  // a newer record supersedes: it sums same-frame hits
            Pay(l, now);
        }

        void Update(const std::vector<Sample>& samples, double now) {
            std::unordered_map<uintptr_t, float> seen;
            std::unordered_map<uintptr_t, double> first;
            std::unordered_map<uintptr_t, unsigned> stamps;
            for (const Sample& s : samples) {
                seen[s.id] = s.health;
                stamps[s.id] = s.hitStamp;
                auto fs = firstSeen.find(s.id);
                first[s.id] = fs != firstSeen.end() ? fs->second : now;
                auto it = last.find(s.id);
                const bool settled = it != last.end() && now - first[s.id] >= grace;  // spawn HP fill-up is not a heal
                if (settled && it->second > 0 && s.health < it->second) Loss(s, it->second - s.health, now);
                else if (settled && it->second > 0 && s.health > it->second) Rebound(s, s.health - it->second, now);
                auto st = stamp.find(s.id);
                if (settled && st != stamp.end() && st->second != s.hitStamp) Record(s, now);  // also after death: lagging kill record
            }
            for (auto it = ledger.begin(); it != ledger.end();) {  // loss no record claimed in time: untagged
                Ledger& l = it->second;
                if (l.unclaimed > 0 && now - l.at >= settle) { Show(l.s, l.unclaimed, now, 0, Element::Physical); l.unclaimed = 0; }
                if (l.unclaimed <= 0 && now - l.owed.at > settle) it = ledger.erase(it); else ++it;
            }
            last.swap(seen);  // actors that vanished are forgotten (no number on despawn)
            firstSeen.swap(first);
            stamp.swap(stamps);
            std::erase_if(live, [&](const Number& n) { return now - n.bump > lifetime; });
        }

        bool FightActive(double now) const { return inFight && now - fight.last <= gap; }
    };

    // UE camera POV (cm, degrees; FOV horizontal).
    struct View { float x, y, z, pitch, yaw, roll, fov; };

    // World point -> screen pixels, UE axis conventions (X fwd, Y right, Z up). false if behind camera.
    inline bool Project(const View& v, float px, float py, float pz, float w, float h, float& sx, float& sy) {
        constexpr float d2r = 3.14159265f / 180.0f;
        const float sp = std::sin(v.pitch * d2r), cp = std::cos(v.pitch * d2r);
        const float sy_ = std::sin(v.yaw * d2r), cy = std::cos(v.yaw * d2r);
        const float sr = std::sin(v.roll * d2r), cr = std::cos(v.roll * d2r);
        const float ax[3] = {cp * cy, cp * sy_, sp};
        const float ay[3] = {sr * sp * cy - cr * sy_, sr * sp * sy_ + cr * cy, -sr * cp};
        const float az[3] = {-(cr * sp * cy + sr * sy_), cy * sr - cr * sp * sy_, cr * cp};
        const float d[3] = {px - v.x, py - v.y, pz - v.z};
        const float right = d[0] * ay[0] + d[1] * ay[1] + d[2] * ay[2];
        const float up = d[0] * az[0] + d[1] * az[1] + d[2] * az[2];
        const float fwd = d[0] * ax[0] + d[1] * ax[1] + d[2] * ax[2];
        if (fwd < 1.0f) return false;
        const float cx = w * 0.5f, cyy = h * 0.5f;
        const float f = cx / std::tan(v.fov * d2r * 0.5f);
        sx = cx + right * f / fwd;
        sy = cyy - up * f / fwd;
        return true;
    }
}
