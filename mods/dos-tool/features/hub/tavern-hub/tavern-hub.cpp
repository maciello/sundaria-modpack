#include "feature.hpp"
#include "tavern-hub.hpp"
#include "../shared/hub_ui.hpp"
#include "../shared/mini_map.hpp"
#include "../shared/props.hpp"
#include "logger.hpp"
#include "imgui.h"

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <string>

// Tavern hub (step 1+2 of the walkable hub): F7 takes control (dungeon-style: game input captured, mouse look) of the hero standing in the village and walks it
// with a third-person camera; the menu places NPCs (and their click zones) where the hero stands. The layout is
// saved next to the game exe and re-applied whenever the village loads.
namespace {
    bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    bool GameFocused() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    std::string GamePath(const char* file) {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1) + file;
    }

    // Win32 file I/O like logger.hpp: no <fstream> (keeps the DLL off msvcp140's stream code under Proton).
    std::string ReadText(const std::string& path) {
        std::string out;
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return out;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
        CloseHandle(h);
        return out;
    }

    void WriteText(const std::string& path, const std::string& text) {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD n = 0;
        WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr);
        CloseHandle(h);
    }

    struct TavernHub : feature::Feature {
        // walk
        bool walking = false, keyWas = false, looking = false, placeWas = false;
        std::string diag;  // last logged hero state
        float camYaw = 0, camPitch = -20, dist = 420, height = 70, sens = 0.12f;
        double last = 0;
        // placement
        std::vector<game::Npc> npcs;
        std::vector<tavern_hub::Spot> layout;
        std::vector<tavern_hub::Spot> home;  // where each NPC stands in vanilla, saved the first time it is seen unmoved
        bool fixCollision = true;
        float roomInset[4] = {60, 60, 60, 60}, roomHeight = 600.0f;  // per wall, saved as ROOM_WALLS=x,y,z,yaw
        bool roomWas = false, roomBuilt = false, nudgeWas = false, showWalls = false;  // built once per walk session from the saved spot
        double nextFix = 0;
        bool loaded = false, applyLayout = true;
        int pick = 0;
        double nextScan = 0;
        int homesSaved = 0;
        std::string world, roomWorld;

        TavernHub() : Feature("Tavern hub", feature::Stage::Alpha) {}

        void Load() {
            layout = tavern_hub::ParseLayout(ReadText(GamePath("dos-tool-tavern.ini")));
            for (const tavern_hub::Spot& s : layout)
                if (s.name == "ROOM_WALLS") { roomInset[0] = s.x; roomInset[1] = s.y; roomInset[2] = s.z; roomInset[3] = s.yaw; }
            home = tavern_hub::ParseLayout(ReadText(GamePath("dos-tool-tavern-home.ini")));
            loaded = true;
        }

        void Save() { WriteText(GamePath("dos-tool-tavern.ini"), tavern_hub::WriteLayout(layout)); }

        const tavern_hub::Spot* Home(const std::string& name) const {
            for (const tavern_hub::Spot& s : home) if (s.name == name) return &s;
            return nullptr;
        }

        // The building the hero stands in becomes a closed room (invisible floor + 4 walls); the spot is saved
        // as "ROOM" in the layout and rebuilt on later walks.
        // Walking: floor at the hero's feet. Otherwise (free camera hovering at head height inside): floor 160 below
        // the camera, and the hero is put there. ROOM's yaw field: 1 = z is feet height, 0 = character centre.
        void CloseRoom() {
            tavern_hub::Spot spot{"ROOM", 0, 0, 0, 0};
            if (walking) {
                const game::Hero h = game::HubHero();
                if (!h.found) return;
                spot = {"ROOM", h.x, h.y, h.z, 0};
                game::MakeRoom(h.x, h.y, h.z, false, Insets(), roomHeight, false);
            } else {
                combat::View v{};
                if (!game::GetView(v)) return;
                spot = {"ROOM", v.x, v.y, v.z - 160.0f, 1};
                game::MakeRoom(spot.x, spot.y, spot.z, true, Insets(), roomHeight, true);
            }
            tavern_hub::SetSpot(layout, spot);
            Save();
            roomBuilt = true;
            roomWorld = world;
        }

        // Page Up / Page Down: the saved room floor 10 units up or down, rebuilt at once.
        void NudgeFloor(float dz) {
            for (tavern_hub::Spot& s : layout)
                if (s.name == "ROOM") { s.z += dz; Save(); RebuildRoom(); return; }
        }

        game::RoomInsets Insets() const { return {roomInset[0], roomInset[1], roomInset[2], roomInset[3]}; }

        // teleport: put the hero onto the room floor at the saved spot (the closed door keeps it out otherwise)
        void RebuildRoom(bool teleport = false) {
            for (const tavern_hub::Spot& s : layout)
                if (s.name == "ROOM") { game::MakeRoom(s.x, s.y, s.z, s.yaw > 0.5f, Insets(), roomHeight, teleport); roomBuilt = true; }
        }

        bool HasRoom() const {
            for (const tavern_hub::Spot& s : layout) if (s.name == "ROOM") return true;
            return false;
        }

        void StartWalk() {
            logger::log("[tavern] walk requested, world " + world);
            const game::Hero h = game::HubHero();
            if (!h.found) { logger::log("[tavern] no hero in this map"); return; }
            camYaw = h.yaw;
            walking = true;
            ui = false;
            hub_ui::SetButtonsHidden(true);  // the hub's own indicators and click targets off while you walk
            RebuildRoom(true);  // you start in the tavern
            RebuildMap();
            roomWorld = world;
            logger::log("[tavern] walk on");
        }

        void StopWalk() {
            if (walking) logger::log("[tavern] walk off");
            const bool was = walking;
            walking = looking = ui = false;
            game::SetHubWalk(nullptr);
            game::SetFreeCam(nullptr, 0);
            if (was) {
                EndBuild();
                hub_ui::Focus(false, 0, 0, 0); hub_ui::SetButtonsHidden(false); hub_ui::Stop(); mini_map::Stop(); props::Stop();
            }
            focusedName.clear();
        }

        void Place(const game::Npc& n, float x, float y, float z, float yaw) { game::PlaceNpc(n.id, x, y, z, yaw); }

        void Scan(const feature::Frame& f) {
            if (f.now < nextScan) return;
            nextScan = f.now + 1.0;
            if (f.snap.worldName != world && f.snap.worldName == "Hub") {
                if (HasRoom()) RebuildRoom();  // floor first: placed NPCs stand in the tavern and would fall through it
                roomWorld = f.snap.worldName;
                RebuildMap();
                ApplyProps();
            }
            world = f.snap.worldName;
            npcs = game::ListNpcs();
            std::sort(npcs.begin(), npcs.end(), [](const game::Npc& a, const game::Npc& b) { return a.name < b.name; });
            bool newHome = false;
            for (const game::Npc& n : npcs) {
                bool moved = false;  // in the layout: its current spot may be ours, not its home
                for (const tavern_hub::Spot& s : layout) moved |= s.name == n.name;
                if (!Home(n.name) && !moved) { home.push_back({n.name, n.x, n.y, n.z, n.yaw}); newHome = true; }
                if (!applyLayout) continue;
                for (const tavern_hub::Spot& s : layout) {  // first sight or wandered off (AI walking home): put back
                    if (s.name != n.name) continue;
                    const float dx = n.x - s.x, dy = n.y - s.y, dz = n.z - s.z;  // z too: fell through a floor
                    if (dx * dx + dy * dy + dz * dz > 150.0f * 150.0f) Place(n, s.x, s.y, s.z + 10.0f, s.yaw);
                }
            }
            if (newHome) WriteText(GamePath("dos-tool-tavern-home.ini"), tavern_hub::WriteLayout(home));
        }

        void SendHome(const game::Npc& n) {
            if (const tavern_hub::Spot* h = Home(n.name)) Place(n, h->x, h->y, h->z, h->yaw);
        }

        // Dungeon-style controls while walking: the game sees no presses (no building selection) and no cursor.
        // I (inventory), P (party), Enter (chat) and Esc still reach it; while one of those screens is open (`ui`)
        // the game gets everything and the hero stands still.
        bool ui = false;
        bool CapturesInput() const override { return walking && !ui; }
        bool PassesKey(unsigned vk) const override { return vk == 'I' || vk == 'P' || vk == VK_RETURN || vk == VK_ESCAPE; }
        bool uiKeysWas[4] = {}, talkWas = false;
        std::string focusedName;
        bool camHeld = false;  // our follow camera is on screen  // NPC the camera looks at: focused like the gamepad does (its name shows)

        // what the camera aims at (12°, 15 m): an NPC or a dungeon on the miniature map, focused through its own click
        // zone when it changes. Returns false when nothing is aimed at; `at` = that click zone's spot.
        struct Aim { std::string name; float x, y, z; };
        bool UpdateFocus(Aim& at) {
            combat::View v{};
            bool hit = false;
            if (game::GetView(v)) {
                std::vector<Aim> all;
                for (const game::Npc& n : npcs) if (n.hasButton) all.push_back({n.name, n.x, n.y, n.z});
                for (const mini_map::Target& t : mini_map::Targets()) all.push_back({t.name, t.x, t.y, t.z});
                std::vector<tavern_hub::Target> ts;
                for (const Aim& a : all) ts.push_back({a.x, a.y, a.z});
                const int i = tavern_hub::LookedAt(v.x, v.y, v.z, v.pitch, v.yaw, ts, 12.0f, 1500.0f, 0.0f);
                if (i >= 0) { at = all[i]; hit = true; }
            }
            const std::string name = hit ? at.name : "";
            if (name != focusedName) {
                focusedName = name;
                if (hit) hub_ui::Focus(true, at.x, at.y, at.z); else hub_ui::Focus(false, 0, 0, 0);
            }
            return hit;
        }

        // M: the world map, shrunk to mapWidth, at table height in front of you; saved as MAP=x,y,z,yaw (+ MAP_SIZE=w)
        void PlaceMap(const game::Hero& h) {
            const float yaw = std::fmod(camYaw, 360.0f), r = yaw * tavern_hub::kD2R;
            const float ahead = 60.0f + mapWidth / 2;  // its near edge a little in front of you
            const tavern_hub::Spot spot{"MAP", h.x + ahead * std::cos(r), h.y + ahead * std::sin(r), h.z - 5.0f, yaw};
            tavern_hub::SetSpot(layout, spot);
            Save();
            RebuildMap();
        }

        void RebuildMap() {
            for (const tavern_hub::Spot& s : layout) {
                if (s.name == "MAP_SIZE") mapWidth = s.x;
                if (s.name == "MAP") mini_map::Place({s.x, s.y, s.z, s.yaw, mapWidth});
            }
        }

        // + / -: the miniature 25 cm larger / smaller, saved
        void ResizeMap(float d) {
            mapWidth = std::clamp(mapWidth + d, 75.0f, 800.0f);
            tavern_hub::SetSpot(layout, {"MAP_SIZE", mapWidth, 0, 0, 0});
            Save();
            RebuildMap();
        }
        float mapWidth = 250.0f;

        // Build mode (B while walking, also flying with F6): look at a prop (outlined like the NPCs), left click (or E
        // when not flying) picks it up; it rides in front of you - or where the free camera's view meets the floor -
        // at the height it had above the floor; R turns it 15°, Page Up/Down lifts it 5 cm, left click (E) puts it
        // down, right click (walking) / Backspace puts it back. Saved as PROP:<actor name>=x,y,z,yaw.
        bool build = false, buildWas = false, rotWas = false, pickWas = false, cancelWas = false, liftWas = false;
        std::vector<props::Prop> nearProps;
        float scanX = 1e9f, scanY = 1e9f;
        std::string lookedProp, carried;
        float carryYaw = 0, carryAbove = 0, carryRadius = 0;
        tavern_hub::Spot carriedFrom{};                // where it stood: cancel puts it back
        float lastX = 0, lastY = 0, lastZ = 0;         // where it rides now
        static constexpr float kCapsuleHalf = 90.0f;  // hub hero's capsule: centre → feet

        bool CarrySpot(const game::Hero& h, bool flying, float& x, float& y) {
            const float feet = h.z - kCapsuleHalf;
            if (flying) {  // where the free camera looks at the floor you stand on
                combat::View v{};
                return game::GetView(v) && tavern_hub::RayToFloor(v.x, v.y, v.z, v.pitch, v.yaw, feet, 3000.0f, x, y);
            }
            const float r = camYaw * tavern_hub::kD2R, ahead = 80.0f + carryRadius;
            x = h.x + ahead * std::cos(r);
            y = h.y + ahead * std::sin(r);
            return true;
        }

        void Build(const game::Hero& h, bool keys, bool e, bool eWas) {
            const bool flying = game::CamOwner() == 1;
            const bool lmb = keys && Down(VK_LBUTTON), pick = (lmb && !pickWas) || (!flying && e && !eWas);
            pickWas = lmb;
            // right mouse turns the free camera's view: cancel there is Backspace only
            const bool cancelKey = keys && ((!flying && Down(VK_RBUTTON)) || Down(VK_BACK)), cancel = cancelKey && !cancelWas;
            cancelWas = cancelKey;
            const float dx = h.x - scanX, dy = h.y - scanY;
            if (dx * dx + dy * dy > 200.0f * 200.0f) { props::Scan(h.x, h.y, h.z, 2000.0f); scanX = h.x; scanY = h.y; }
            nearProps = props::Near();
            if (!carried.empty()) {
                const bool rot = keys && Down('R');
                if (rot && !rotWas) carryYaw = std::fmod(carryYaw + 15.0f, 360.0f);
                rotWas = rot;
                const bool up = keys && Down(VK_PRIOR), dn = keys && Down(VK_NEXT);
                if ((up || dn) && !liftWas) carryAbove += up ? 5.0f : -5.0f;
                liftWas = up || dn;
                float x = lastX, y = lastY;
                if (CarrySpot(h, flying, x, y)) { lastX = x; lastY = y; lastZ = h.z - kCapsuleHalf + carryAbove; }
                if (cancel) {
                    props::Move(carried, carriedFrom.x, carriedFrom.y, carriedFrom.z, carriedFrom.yaw, false);
                    props::Highlight(carried, false, 0);
                    logger::log("[tavern] put back " + carried);
                    carried.clear();
                    return;
                }
                props::Move(carried, lastX, lastY, lastZ, carryYaw, !pick);
                if (pick) {  // put it down where it rides, and remember
                    props::Highlight(carried, false, 0);
                    tavern_hub::SetSpot(layout, {"PROP:" + carried, lastX, lastY, lastZ, carryYaw});
                    Save();
                    logger::log("[tavern] put down " + carried);
                    carried.clear();
                    scanX = 1e9f;  // positions changed: rescan
                }
                return;
            }
            combat::View v{};
            std::string hit;
            const props::Prop* hp = nullptr;
            if (game::GetView(v)) {
                std::vector<tavern_hub::Target> ts;
                for (const props::Prop& p : nearProps) ts.push_back({p.x, p.y, p.cz});
                const int i = tavern_hub::LookedAt(v.x, v.y, v.z, v.pitch, v.yaw, ts, 8.0f, 2000.0f, 0.0f);
                if (i >= 0) { hp = &nearProps[i]; hit = hp->name; }
            }
            if (hit != lookedProp) {
                if (!lookedProp.empty()) props::Highlight(lookedProp, false, 0);
                if (!hit.empty()) props::Highlight(hit, true, hub_ui::RimStencil());
                lookedProp = hit;
            }
            if (pick && hp) {
                carried = hp->name;
                carriedFrom = {"", hp->x, hp->y, hp->z, hp->yaw};
                carryYaw = hp->yaw;
                carryAbove = hp->z - (h.z - kCapsuleHalf);  // its height above the floor you both stand on
                carryRadius = hp->radius;
                lastX = hp->x; lastY = hp->y; lastZ = hp->z;
                lookedProp.clear();
                logger::log("[tavern] picked up " + carried);
            }
        }

        void EndBuild() {
            if (!lookedProp.empty()) props::Highlight(lookedProp, false, 0);
            if (!carried.empty()) props::Highlight(carried, false, 0);
            lookedProp.clear();
            carried.clear();  // ponytail: a prop still carried stays where it last was, without collision until reload
            build = false;
        }

        void ApplyProps() {
            for (const tavern_hub::Spot& s : layout)
                if (s.name.rfind("PROP:", 0) == 0) props::Move(s.name.substr(5), s.x, s.y, s.z, s.yaw, false);
        }
        bool mapWas = false, sizeWas = false;

        // the NPC within talking range (3 m) closest to the hero, or nullptr
        const game::Npc* TalkTarget(const game::Hero& h) const {
            const game::Npc* best = nullptr;
            float bestD = 300.0f * 300.0f;
            for (const game::Npc& n : npcs) {
                const float dx = n.x - h.x, dy = n.y - h.y, dz = n.z - h.z, d = dx * dx + dy * dy + dz * dz;
                if (n.hasButton && d < bestD) { bestD = d; best = &n; }
            }
            return best;
        }

        double nextDiag = 0;
        void LogDiag(const game::Hero& h, const feature::Frame& f) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[tavern] hero possessed=%d moveMode=%d possessTries=%d modeFixes=%d at %.0f %.0f %.0f",
                          h.possessed ? 1 : 0, h.moveMode, h.possessTries, h.modeFixes, h.x, h.y, h.z);
            if (diag != buf && f.now >= nextDiag) { diag = buf; nextDiag = f.now + 2.0; logger::log(buf); }
        }

        void OnFrame(const feature::Frame& f) override {
            if (!loaded) Load();
            Scan(f);
            const float dt = last > 0 ? float(std::min(f.now - last, 0.1)) : 0.0f;
            last = f.now;
            const bool focused = GameFocused();
            const bool typing = ImGui::GetIO().WantCaptureKeyboard;
            const bool k = focused && !typing && Down(VK_F7);
            if (k && !keyWas) walking ? StopWalk() : StartWalk();
            keyWas = k;
            const bool up = focused && !typing && Down(VK_PRIOR), dn = focused && !typing && Down(VK_NEXT);
            if (carried.empty() && ((up && !nudgeWas) || (dn && !nudgeWas))) NudgeFloor(up ? 10.0f : -10.0f);
            nudgeWas = up || dn;
            const bool rk = focused && !typing && Down(VK_F10);
            if (rk && !roomWas) CloseRoom();
            roomWas = rk;
            const bool pk = focused && !typing && Down(VK_F9);
            if (pk && !placeWas) PlacePicked();
            placeWas = pk;
            if (walking && !f.snap.worldName.empty() && f.snap.worldName != "Hub") {
                logger::log("[tavern] left the hub (" + f.snap.worldName + "): walk off");
                StopWalk();
            }
            if (!walking) return;

            const game::Hero h = game::HubHero();
            if (!h.found) { StopWalk(); return; }
            LogDiag(h, f);
            if (fixCollision && f.now >= nextFix) {  // the hub's meshes mostly have no collision: switch it on around the hero
                nextFix = f.now + 2.0;
                game::FixCollision(h.x, h.y, h.z, 4000.0f, true);
            }
            // I / P / Enter open a game screen (again: close it), Esc closes; E talks to the nearest NPC
            static const int kUiKeys[4] = {'I', 'P', VK_RETURN, VK_ESCAPE};
            for (int i = 0; i < 4; i++) {
                const bool d = focused && !typing && Down(kUiKeys[i]);
                if (d && !uiKeysWas[i]) ui = kUiKeys[i] == VK_ESCAPE ? false : !ui;
                uiKeysWas[i] = d;
            }
            const bool e = focused && !typing && Down('E');
            const bool b = focused && !typing && !ui && Down('B');
            if (b && !buildWas) { if (build) EndBuild(); else { build = true; hub_ui::Focus(false, 0, 0, 0); focusedName.clear(); } }
            buildWas = b;
            if (build && !ui) { Build(h, focused && !typing, e, talkWas); talkWas = e; }
            Aim aim;
            const bool looked = !ui && !build && UpdateFocus(aim);
            if (!build && e && !talkWas && !ui && game::CamOwner() != 1) {  // E is up for the free camera
                // like Space on the focused click zone before; standing right next to an NPC works without aiming
                if (looked) { hub_ui::Talk(aim.x, aim.y, aim.z); ui = true; }
                else if (const game::Npc* n = TalkTarget(h)) { hub_ui::Talk(n->x, n->y, n->z); ui = true; }
            }
            const bool m = focused && !typing && !ui && Down('M');
            if (m && !mapWas) PlaceMap(h);
            mapWas = m;
            const bool bigger = focused && !typing && !ui && (Down(VK_OEM_PLUS) || Down(VK_ADD));
            const bool smaller = focused && !typing && !ui && (Down(VK_OEM_MINUS) || Down(VK_SUBTRACT));
            if ((bigger || smaller) && !sizeWas) ResizeMap(bigger ? 25.0f : -25.0f);
            sizeWas = bigger || smaller;
            talkWas = e;
            float fwd = 0, right = 0;
            bool jump = false;
            const bool flying = game::CamOwner() == 1;  // the free camera has the keys and the view
            if (focused && !typing && !flying && !ui) {
                fwd = float(Down('W')) - float(Down('S'));
                right = float(Down('D')) - float(Down('A'));
                jump = Down(VK_SPACE);
                camYaw += 90.0f * dt * (float(Down(VK_RIGHT)) - float(Down(VK_LEFT)));
                camPitch += 60.0f * dt * (float(Down(VK_UP)) - float(Down(VK_DOWN)));
            }
            // Mouse look like in the dungeon: cursor pinned to the window centre, every move turns the camera.
            // The Insert menu frees the cursor.
            const bool look = focused && !ImGui::GetIO().MouseDrawCursor && !flying && !ui;
            RECT r{};
            const HWND wnd = GetForegroundWindow();
            GetClientRect(wnd, &r);
            POINT centre{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            ClientToScreen(wnd, &centre);
            POINT c{};
            GetCursorPos(&c);
            if (look && f.rawMouse) {  // raw motion: works even when the game freezes the hidden cursor
                camYaw += sens * f.mouseDX;
                camPitch -= sens * f.mouseDY;
            } else if (look && looking) {
                camYaw += sens * float(c.x - centre.x);
                camPitch -= sens * float(c.y - centre.y);
            }
            if (look) SetCursorPos(centre.x, centre.y);
            looking = look;
            camPitch = std::clamp(camPitch, -75.0f, 30.0f);

            game::WalkInput in{};
            tavern_hub::MoveDir(fwd, right, camYaw, in.moveX, in.moveY);
            in.jump = jump;
            game::SetHubWalk(&in);
            if (ui) {  // a game screen (NPC menu, world map) switches camera views itself: let go of the camera
                if (camHeld) { game::SetFreeCam(nullptr, 0); camHeld = false; }
                return;
            }
            const tavern_hub::Pose p = tavern_hub::Follow(h.x, h.y, h.z, camYaw, camPitch, dist, height);
            const game::CamPose cp{p.x, p.y, p.z, p.pitch, p.yaw};
            game::SetFreeCam(&cp, 0);
            camHeld = true;
        }

        // Picked NPC to where the hero stands, facing the camera (= towards the player).
        void PlacePicked() {
            if (npcs.empty() || pick >= int(npcs.size())) return;
            const game::Hero h = game::HubHero();
            if (!h.found) return;
            const game::Npc& n = npcs[pick];
            const float yaw = std::fmod(camYaw + 180.0f, 360.0f);
            Place(n, h.x, h.y, h.z, yaw);
            tavern_hub::SetSpot(layout, {n.name, h.x, h.y, h.z, yaw});
            Save();
            char buf[160];
            std::snprintf(buf, sizeof(buf), "[tavern] placed %s at %.0f %.0f %.0f", n.name.c_str(), h.x, h.y, h.z);
            logger::log(buf);
        }

        void ResetNpc(const std::string& name) {
            layout.erase(std::remove_if(layout.begin(), layout.end(), [&](const tavern_hub::Spot& s) { return s.name == name; }),
                         layout.end());
            for (const game::Npc& n : npcs) if (n.name == name) SendHome(n);
            Save();
        }

        // Every NPC's current spot becomes its home ("Reset" target), e.g. right after loading a save.
        void SaveHomes() {
            for (const game::Npc& n : npcs) tavern_hub::SetSpot(home, {n.name, n.x, n.y, n.z, n.yaw});
            WriteText(GamePath("dos-tool-tavern-home.ini"), tavern_hub::WriteLayout(home));
            homesSaved = int(npcs.size());
            char buf[96];
            std::snprintf(buf, sizeof(buf), "[tavern] %d NPC home spots saved", homesSaved);
            logger::log(buf);
        }

        void ResetAll() {  // NPCs only: the closed room stays
            layout.erase(std::remove_if(layout.begin(), layout.end(), [](const tavern_hub::Spot& s) { return s.name.rfind("ROOM", 0) != 0 && s.name.rfind("MAP", 0) != 0 && s.name.rfind("PROP:", 0) != 0; }),
                         layout.end());
            for (const game::Npc& n : npcs) SendHome(n);
            Save();
            logger::log("[tavern] layout reset");
        }

        void Off() override {
            StopWalk();
            mini_map::Restore();  // the world map back on its plateau
            mini_map::Stop();
        }

        void Menu() override {
            if (ImGui::Button(walking ? "Stop walking (F7)" : "Walk (F7)")) walking ? StopWalk() : StartWalk();
            const game::Hero h = game::HubHero();
            ImGui::SameLine();
            ImGui::TextDisabled(h.found ? (h.possessed ? "hero: controlled" : "hero: found") : "hero: none in this map");
            ImGui::TextDisabled("WASD walk, Space jump, mouse or arrows turn the camera");
            if (walking) ImGui::TextDisabled("%s", diag.c_str());
            ImGui::SetNextItemWidth(150);
            ImGui::SliderFloat("Camera distance", &dist, 150.0f, 1200.0f, "%.0f");
            if (ImGui::Button("Close this building (F10)")) CloseRoom();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", game::RoomStatus().c_str());
            ImGui::TextDisabled("Page Up / Page Down: floor 10 up / down");
            if (ImGui::Checkbox("Show walls", &showWalls)) game::ShowRoom(showWalls);
            ImGui::SameLine();
            // each wall moves in from the building's outer bounds; rebuilt when a slider is let go (tick Show walls)
            static const char* const kWall[4] = {"Wall 1", "Wall 2", "Wall 3", "Wall 4"};
            bool changed = false;
            for (int i = 0; i < 4; i++) {
                ImGui::SetNextItemWidth(160);
                ImGui::SliderFloat(kWall[i], &roomInset[i], 0.0f, 1500.0f, "%.0f");
                changed |= ImGui::IsItemDeactivatedAfterEdit();
                if (i % 2 == 0) ImGui::SameLine();
            }
            ImGui::SetNextItemWidth(160);
            ImGui::SliderFloat("Wall height", &roomHeight, 200.0f, 1500.0f, "%.0f");
            changed |= ImGui::IsItemDeactivatedAfterEdit();
            if (changed && HasRoom()) {
                tavern_hub::SetSpot(layout, {"ROOM_WALLS", roomInset[0], roomInset[1], roomInset[2], roomInset[3]});
                Save();
                RebuildRoom();
            }
            ImGui::Checkbox("Switch on collision around the hero", &fixCollision);
            ImGui::SameLine();
            if (ImGui::SmallButton("now") && h.found) game::FixCollision(h.x, h.y, h.z, 4000.0f, true);

            ImGui::SeparatorText("NPCs");
            if (npcs.empty()) { ImGui::TextDisabled("no NPCs in this map"); return; }
            pick = std::clamp(pick, 0, int(npcs.size()) - 1);
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("##npc", npcs[pick].name.c_str())) {
                for (int i = 0; i < int(npcs.size()); i++)
                    if (ImGui::Selectable(npcs[i].name.c_str(), i == pick)) pick = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("Place at hero (F9)")) PlacePicked();
            if (ImGui::Checkbox("Apply saved layout", &applyLayout) && !applyLayout)
                for (const game::Npc& n : npcs) SendHome(n);  // back home, file kept
            ImGui::SameLine();
            if (ImGui::Button("Reset all")) ResetAll();
            if (ImGui::Button("Save current spots as standard")) SaveHomes();
            if (homesSaved > 0) { ImGui::SameLine(); ImGui::TextDisabled("%d saved", homesSaved); }
            for (const game::Npc& n : npcs) {
                bool placed = false;
                for (const tavern_hub::Spot& s : layout) placed |= s.name == n.name;
                ImGui::PushID(n.name.c_str());
                ImGui::TextDisabled("%s%s%s", placed ? "* " : "  ", n.name.c_str(), n.hasButton ? "" : " (no click zone)");
                if (placed) { ImGui::SameLine(); if (ImGui::SmallButton("reset")) ResetNpc(n.name); }
                ImGui::PopID();
            }
        }
    } g_tavernHub;
}
