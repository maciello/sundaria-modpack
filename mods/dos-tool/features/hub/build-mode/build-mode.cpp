#include "feature.hpp"
#include "logger.hpp"
#include "imgui.h"
#include "hud.hpp"
#include "../shared/aim.hpp"
#include "../shared/hub_state.hpp"
#include "../shared/hub_ui.hpp"
#include "../shared/layout.hpp"
#include "../shared/mini_map.hpp"
#include "../shared/props.hpp"
#include "../shared/props_spawn.hpp"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Build mode for the hub (B while walking with the tavern hub, also when flying the free camera): what the crosshair
// is on - a prop or an NPC - gets outlined; pick it up, carry it (in front of you, or where the free camera's view
// meets your floor) at its height above the floor, turn it (R), lift it (wheel, Page Up/Down), put it down or cancel.
// T fetches it next to you, C copies a prop, Page Up/Down + N place a new one from the map's catalog, Del resets.
// Saved: props as PROP:<actor>=x,y,z,yaw, NPCs under their name (the tavern hub's layout), copies in the spawn list.
namespace {
    bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    bool GameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    // one-shot key presses
    struct Edge {
        bool was = false;
        bool operator()(bool now) { const bool e = now && !was; was = now; return e; }
    };

    // something the crosshair can be on
    struct Thing {
        bool npc = false;
        std::string name;        // actor name (prop) / NPC name
        uintptr_t npcId = 0;
        float x = 0, y = 0, z = 0, cz = 0, yaw = 0, radius = 50, base = 0;
        std::string mesh;
        float scale = 1;
        int spawnId = -1;        // one of our copies
    };

    constexpr float kCapsuleHalf = 90.0f;  // hub characters: centre → feet

    struct BuildMode : feature::Feature {
        bool build = false;
        Edge bKey, pickKey, eKey, cancelKey, rotKey, liftUp, liftDn, bringKey, resetKey, dupKey, newKey, prevKey, nextKey;
        std::vector<Thing> things;
        float scanX = 1e9f, scanY = 1e9f;
        std::string looked;                 // name of the targeted thing
        bool lookedNpc = false;
        bool carrying = false;
        Thing held;                         // what is carried
        Thing from;                         // where it stood: cancel puts it back
        float carryYaw = 0, carryAbove = 0;
        bool nearHero = false;              // fetched (T) / new: rides next to you even when flying
        int pendingSpawn = -1;              // a copy on its way: carried once the game made it
        float lastX = 0, lastY = 0, lastZ = 0;
        int catalogIndex = 0;
        std::vector<hub_layout::SpawnRec> spawns;
        bool spawnsLoaded = false;
        std::string world;

        BuildMode() : Feature("Build mode", feature::Stage::Alpha) {}

        void LoadSpawns() {
            if (spawnsLoaded) return;
            spawns = hub_layout::ParseSpawns(hub_layout::ReadText(hub_layout::GamePath("dos-tool-tavern-spawns.ini")));
            spawnsLoaded = true;
        }
        void SaveSpawns() { hub_layout::WriteText(hub_layout::GamePath("dos-tool-tavern-spawns.ini"), hub_layout::WriteSpawns(spawns)); }

        // the hub loaded: moved props back to their spots, our copies back into the world
        void ApplyAll() {
            LoadSpawns();
            for (const hub_layout::Spot& s : hub_layout::Spots())
                if (s.name.rfind("PROP:", 0) == 0) props::Move(s.name.substr(5), s.x, s.y, s.z, s.yaw, false);
            for (const hub_layout::SpawnRec& r : spawns) props_spawn::Spawn(r.id, r.mesh, r.x, r.y, r.z, r.yaw, r.scale);
        }

        void Rescan(float x, float y, float z) {
            things.clear();
            props::Scan(x, y, z, 2500.0f, mini_map::SceneNames());
            for (const props::Prop& p : props::Near()) {
                Thing t;
                t.name = p.name; t.x = p.x; t.y = p.y; t.z = p.z; t.cz = p.cz; t.yaw = p.yaw; t.radius = p.radius;
                t.base = p.base; t.mesh = p.mesh; t.scale = p.scale; t.spawnId = props_spawn::IdOf(p.name);
                things.push_back(t);
            }
            for (const game::Npc& n : game::ListNpcs()) {
                if (!n.hasButton) continue;
                Thing t;
                t.npc = true; t.name = n.name; t.npcId = n.id; t.x = n.x; t.y = n.y; t.z = n.z; t.cz = n.z; t.yaw = n.yaw;
                t.radius = 60; t.base = kCapsuleHalf;
                things.push_back(t);
            }
            scanX = x; scanY = y;
        }

        void Outline(const Thing& t, bool on) {
            if (t.npc) hub_ui::Focus(on, t.x, t.y, t.z);  // the NPC's own outline + name
            else props::Highlight(t.name, on, hub_ui::RimStencil());
        }

        void Place(const Thing& t, float x, float y, float z, float yaw, bool riding) {
            if (t.npc) game::PlaceNpc(t.npcId, x, y, z, yaw);
            else props::Move(t.name, x, y, z, yaw, riding);
        }

        bool CarrySpot(const game::Hero& h, bool flying, float& x, float& y) {
            if (flying && !nearHero) {  // where the free camera looks at the floor you stand on
                combat::View v{};
                return game::GetView(v) && hub_aim::RayToFloor(v.x, v.y, v.z, v.pitch, v.yaw, h.z - kCapsuleHalf, 3000.0f, x, y);
            }
            const float r = hub_state::camYaw * hub_aim::kD2R, ahead = 80.0f + held.radius;
            x = h.x + ahead * std::cos(r);
            y = h.y + ahead * std::sin(r);
            return true;
        }

        void PickUp(const Thing& t, const game::Hero& h, bool fetch) {
            held = from = t;
            carrying = true;
            nearHero = fetch;
            carryYaw = t.yaw;
            // its height above the floor you both stand on; fetched from elsewhere: resting on your floor
            carryAbove = fetch ? t.base : t.z - (h.z - kCapsuleHalf);
            lastX = t.x; lastY = t.y; lastZ = t.z;
            if (t.npc) hub_state::carriedNpc = t.name;
            looked.clear();
            logger::log(std::string("[build] ") + (fetch ? "fetched " : "picked up ") + t.name);
        }

        // a copy of `mesh` (or of the targeted prop) spawned at the carry spot, carried as soon as the game made it
        void SpawnCopy(const std::string& mesh, float yaw, float scale, float above, float radius, const game::Hero& h) {
            LoadSpawns();
            const int id = hub_layout::NextSpawnId(spawns);
            float x = h.x, y = h.y;
            held = Thing{};
            from = Thing{};  // nothing to put back: cancelling throws the copy away
            held.radius = radius;
            carrying = true;
            nearHero = true;
            CarrySpot(h, false, x, y);
            const float z = h.z - kCapsuleHalf + above;
            spawns.push_back({id, mesh, x, y, z, yaw, scale});
            SaveSpawns();
            props_spawn::Spawn(id, mesh, x, y, z, yaw, scale);
            pendingSpawn = id;
            held.mesh = mesh; held.scale = scale; held.spawnId = id; held.yaw = yaw; held.base = above;
            carryYaw = yaw;
            carryAbove = above;
            logger::log("[build] new copy " + std::to_string(id) + " of " + mesh);
        }

        void PutDown() {
            Place(held, lastX, lastY, lastZ, carryYaw, false);
            if (!held.npc) props::Highlight(held.name, false, 0);
            if (held.npc) {
                hub_layout::SetSpot(hub_layout::Spots(), {held.name, lastX, lastY, lastZ, carryYaw});  // the tavern hub keeps it there
                hub_layout::Save();
            } else if (held.spawnId >= 0) {
                for (hub_layout::SpawnRec& r : spawns)
                    if (r.id == held.spawnId) { r.x = lastX; r.y = lastY; r.z = lastZ; r.yaw = carryYaw; }
                SaveSpawns();
            } else {
                hub_layout::SetSpot(hub_layout::Spots(), {"PROP:" + held.name, lastX, lastY, lastZ, carryYaw});
                hub_layout::Save();
            }
            logger::log("[build] put down " + held.name);
            Drop();
        }

        void Cancel() {
            if (held.spawnId >= 0 && from.name.empty()) {  // a new copy: cancelling throws it away
                props_spawn::Remove(held.spawnId);
                spawns.erase(std::remove_if(spawns.begin(), spawns.end(), [&](const hub_layout::SpawnRec& r) { return r.id == held.spawnId; }),
                             spawns.end());
                SaveSpawns();
            } else {
                Place(held, from.x, from.y, from.z, from.yaw, false);
            }
            if (!held.npc && !held.name.empty()) props::Highlight(held.name, false, 0);
            logger::log("[build] cancelled " + held.name);
            Drop();
        }

        void Drop() {
            carrying = false;
            pendingSpawn = -1;
            nearHero = false;
            hub_state::carriedNpc.clear();
            scanX = 1e9f;  // positions changed: rescan
        }

        // Del: back where the game had it (a copy: gone), and forget our placement
        void Reset(const Thing& t) {
            Outline(t, false);
            if (t.spawnId >= 0) {
                props_spawn::Remove(t.spawnId);
                spawns.erase(std::remove_if(spawns.begin(), spawns.end(), [&](const hub_layout::SpawnRec& r) { return r.id == t.spawnId; }),
                             spawns.end());
                SaveSpawns();
            } else {
                if (!t.npc) props::Reset(t.name);
                const std::string key = t.npc ? t.name : "PROP:" + t.name;
                auto& spots = hub_layout::Spots();
                spots.erase(std::remove_if(spots.begin(), spots.end(), [&](const hub_layout::Spot& s) { return s.name == key; }), spots.end());
                hub_layout::Save();
            }
            logger::log("[build] reset " + t.name);
            looked.clear();
            scanX = 1e9f;
        }

        void Carry(const game::Hero& h, bool flying, bool keys, float wheel, bool pick, bool cancel) {
            if (pendingSpawn >= 0) {  // the copy isn't in the world yet
                const std::string n = props_spawn::NameOf(pendingSpawn);
                if (n.empty()) return;
                held.name = n;
                pendingSpawn = -1;
                props::Highlight(n, true, hub_ui::RimStencil());
            }
            if (rotKey(keys && Down('R'))) carryYaw = std::fmod(carryYaw + 15.0f, 360.0f);
            if (liftUp(keys && Down(VK_PRIOR))) carryAbove += 5.0f;
            if (liftDn(keys && Down(VK_NEXT))) carryAbove -= 5.0f;
            carryAbove += 5.0f * wheel;
            float x = lastX, y = lastY;
            if (CarrySpot(h, flying, x, y)) { lastX = x; lastY = y; lastZ = h.z - kCapsuleHalf + carryAbove; }
            if (cancel) { Cancel(); return; }
            if (pick) { PutDown(); return; }
            Place(held, lastX, lastY, lastZ, carryYaw, true);
        }

        void Aim(const game::Hero& h, bool flying, bool keys, bool pick) {
            combat::View v{};
            const bool view = game::GetView(v);
            // things around the eye that looks: the hero when walking, the free camera when flying
            const float ex = flying && view ? v.x : h.x, ey = flying && view ? v.y : h.y, ez = flying && view ? v.z : h.z;
            const float dx = ex - scanX, dy = ey - scanY;
            if (dx * dx + dy * dy > 200.0f * 200.0f) Rescan(ex, ey, ez);
            const Thing* hit = nullptr;
            if (view) {
                std::vector<hub_aim::Sphere> ss;
                for (const Thing& t : things) ss.push_back({t.x, t.y, t.cz, t.radius * 0.8f});
                const int i = hub_aim::RayPick(v.x, v.y, v.z, v.pitch, v.yaw, ss, 3000.0f);
                if (i >= 0) hit = &things[i];
            }
            const std::string name = hit ? hit->name : "";
            if (name != looked) {
                for (const Thing& t : things) if (t.name == looked) Outline(t, false);
                if (hit) Outline(*hit, true);
                looked = name;
            }
            const bool bring = bringKey(keys && Down('T')), reset = resetKey(keys && Down(VK_DELETE));
            const bool dup = dupKey(keys && Down('C')), make = newKey(keys && Down('N'));  // D strafes
            const std::vector<props_spawn::Item> catalog = props_spawn::Catalog();
            // Page Up / Page Down browse the catalog while nothing is carried ([ ] sit badly on German keyboards)
            if (prevKey(keys && Down(VK_PRIOR)) && !catalog.empty()) catalogIndex = (catalogIndex + int(catalog.size()) - 1) % int(catalog.size());
            if (nextKey(keys && Down(VK_NEXT)) && !catalog.empty()) catalogIndex = (catalogIndex + 1) % int(catalog.size());
            if (make && !catalog.empty()) {
                SpawnCopy(catalog[catalogIndex % catalog.size()].mesh, hub_state::camYaw + 180.0f, 1.0f, 0.0f, 60.0f, h);
                return;
            }
            if (!hit) return;
            if (reset) { Reset(*hit); return; }
            if (dup && !hit->npc) { const Thing t = *hit; Outline(t, false); SpawnCopy(t.mesh, t.yaw, t.scale, t.base, t.radius, h); return; }
            if (pick || bring) {
                const Thing t = *hit;
                if (t.npc) hub_ui::Focus(false, 0, 0, 0);  // its outline goes; a carried NPC isn't a talk target
                PickUp(t, h, bring);
            }
        }

        void End() {
            if (carrying) Cancel();
            for (const Thing& t : things) if (t.name == looked) Outline(t, false);
            looked.clear();
            build = hub_state::build = hub_state::carrying = false;
        }

        void OnFrame(const feature::Frame& f) override {
            if (f.snap.worldName != world) { world = f.snap.worldName; if (world == "Hub") ApplyAll(); }
            const bool keys = GameFocused() && !ImGui::GetIO().WantCaptureKeyboard;
            const bool e = eKey(keys && Down('E'));
            if (!hub_state::walking) { if (build) End(); return; }
            if (hub_state::screenOpen) return;  // a game screen has the input
            if (bKey(keys && Down('B'))) { if (build) End(); else build = hub_state::build = true; }
            if (!build) return;
            const game::Hero h = game::HubHero();
            if (!h.found) return;
            const bool flying = game::CamOwner() == 1;
            const bool pick = pickKey(keys && Down(VK_LBUTTON)) || (!flying && e);
            // right mouse turns the free camera's view: cancel there is Backspace only
            const bool cancel = cancelKey(keys && ((!flying && Down(VK_RBUTTON)) || Down(VK_BACK)));
            if (carrying) Carry(h, flying, keys, f.wheel, pick, cancel);
            else Aim(h, flying, keys, pick);
            hub_state::carrying = carrying;

            const std::vector<props_spawn::Item> catalog = props_spawn::Catalog();
            build_hud::State s;
            s.target = looked;
            s.carried = carrying ? (held.name.empty() ? std::string("(appearing…)") : held.name) : "";
            s.catalogCount = int(catalog.size());
            s.catalogIndex = catalog.empty() ? 0 : catalogIndex % int(catalog.size());
            s.catalog = catalog.empty() ? "" : catalog[s.catalogIndex].label;
            s.flying = flying;
            build_hud::Draw(f, s);
        }

        void Off() override { End(); props::Stop(); props_spawn::Stop(); }
    } g_buildMode;
}
