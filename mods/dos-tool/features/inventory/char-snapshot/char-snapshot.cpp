#include "../shared/hero.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "reflect.hpp"
#include "umg.hpp"
#include "../shared/sdk.hpp"

#include <Windows.h>
#include <atomic>
#include <fstream>
#include <sstream>
#include "BP_ArchonSaveGame_classes.hpp"
#include "BP_ArchonSaveGame_parameters.hpp"
#include "BP_PersistentPlayerAccount_classes.hpp"

// Character snapshot (#102, Alpha, opt-in). The game's own save/load callbacks for this player
// (AArchonSaveGame delegates bound in BP_ArchonSaveGame_C → ProcessEvent) mark the hero dirty; the next world tick
// writes <exe dir>/dos-tool-chars/<hero slot>.yaml. Build = the account's hero summary, gear = the per-hero
// inventory container (equipped). Facts: references/game-facts.md § characters.
using namespace SDK;
using namespace items::hero;
using items::sdk::PtrOk;

namespace {
    std::atomic<bool> g_dirty{false};
    bool g_listening = false;
    thread_local bool t_busy = false;
    ref::Fn g_saved{ABP_ArchonSaveGame_C::StaticClass, "BP_ArchonSaveGame_C", "OnArchonObjectSavedForUser_Event_0"};
    ref::Fn g_loaded{ABP_ArchonSaveGame_C::StaticClass, "BP_ArchonSaveGame_C", "OnArchonObjectLoadedForUser_Event_0"};

    std::string ExeDir() {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        const std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }

    // Display names of the enum behind a property path (user-defined enums are dumped as NewEnumeratorN).
    std::vector<std::string> EnumAt(const UObject* root, const std::vector<reflect::Step>& steps) {
        reflect::Value v;
        std::string err;
        if (!reflect::Walk(root, steps, v, err) || !v.prop) return {};
        const std::string t = reflect::Type(v.prop);
        UEnum* e = t == "ByteProperty" ? static_cast<const FByteProperty*>(v.prop)->Enum
                 : t == "EnumProperty" ? static_cast<const FEnumProperty*>(v.prop)->Enum : nullptr;
        return items::sdk::EnumNames(e);
    }

    // Game thread (world tick). false = not ready yet (stays dirty).
    bool Write() {
        ABP_PlayerControllerOnline_C* pc = items::sdk::LocalPC();
        if (!pc || !items::io::Ready()) return false;
        UBP_PersistentPlayerAccount_C* acc = pc->AccountComponent;
        UBP_ItemContainerComponent_C* bag = items::sdk::Container(static_cast<UObject*>(items::io::Locate().bag));
        if (!PtrOk(acc) || !bag || !acc->bHasLoaded || !bag->bHasLoaded) return false;
        const int slot = acc->ActiveHeroSlot;
        auto& heroes = acc->AccountData.HeroSlots_7_421843754124357E853230B1B5BD92C7;
        if (slot < 0 || slot >= heroes.Num() || bag->HeroSlot != slot) return false;  // mid hero switch
        const FSPersistentAccountHeroSummary& h = heroes[slot];

        const std::vector<reflect::Step> hero = {{"AccountData"}, {"HeroSlots", slot}};
        auto path = [&](std::initializer_list<reflect::Step> more) { auto s = hero; s.insert(s.end(), more); return s; };
        Snapshot s;
        s.slot = slot;
        s.name = PtrOk(h.HeroName_2_C2288A6B4B211BB4E531D7A46D581B3F.TextData) ? h.HeroName_2_C2288A6B4B211BB4E531D7A46D581B3F.ToString() : "";
        s.cls = At(EnumAt(acc, path({{"HeroClass"}})), int(h.HeroClass_14_C1421BAB4D8C35B6A72D5DB8FA63C47D));
        s.level = h.HeroLevel_8_D63A60DB4CA9586D97A094BC4FC9BA5D;
        s.heroismLevel = h.HeroHeroismLevel_104_2CA79F3949CB5B72B79F04BA75749173;
        for (int v : h.HeroismPoints_124_2D9029C641AF6AF4B04F2D8A5D733625) s.heroismPoints.push_back(v);
        for (float v : h.PrimaryStats_40_E24F57734C4425CE8D9E788412A706EF) s.primaryStats.push_back(v);
        const auto& learned = h.LearnedAbilitySpecs_63_1561BD554F99EC5ACC00A0AE5781FEC3;
        const std::vector<std::string> abil = learned.Num() ? EnumAt(acc, path({{"LearnedAbilitySpecs", 0}, {"AbilityName"}})) : std::vector<std::string>{};
        for (const auto& l : learned)
            s.learned.push_back({At(abil, int(l.AbilityName_2_3AA987B3414FDD8A83181D94D4D42E34)), l.Level_5_163A969045CBC9C75C69D898B344995C});
        const auto& bar = h.AbilityMappings_54_F2250A094B8DA9E3CF066BBF540FBC6D;
        const std::vector<std::string> types = bar.Num() ? EnumAt(acc, path({{"AbilityMappings", 0}, {"ActionBarItem"}, {"Type"}})) : std::vector<std::string>{};
        for (const auto& m : bar) {
            const auto& i = m.ActionBarItem_15_3F75DD174E75B09B6094FB9FABBFC889;
            BarItem b{i.ID_2_B192DADA43F28C263419A7BA69B4B4D2, At(types, int(i.Type_5_C5C951874958396135F8D58C0549B876)), i.IsPassive_8_7127D12447526458FE29B78C5835AC1F, {}};
            for (int x : m.SlotIndices_7_7FC86397422966E02953C886CC624902) b.slots.push_back(x);
            s.bar.push_back(std::move(b));
        }
        for (items::Item& it : items::io::Read(bag, false, true))
            if (it.where == items::Where::Equipped) s.equipped.push_back(std::move(it));

        const items::io::Names& n = items::io::GetNames();
        const std::string yaml = Yaml(s, n.stat, n.equipSlot);
        const std::string dir = ExeDir() + "dos-tool-chars\\", file = dir + std::to_string(slot) + ".yaml";
        std::stringstream old;
        old << std::ifstream(file, std::ios::binary).rdbuf();
        if (old.str() == yaml) return true;
        CreateDirectoryA(dir.c_str(), nullptr);
        std::ofstream(file, std::ios::binary) << yaml;
        logger::log("[char-snapshot] slot " + std::to_string(slot) + " -> dos-tool-chars/" + std::to_string(slot) + ".yaml (" + s.cls + " lv " +
                    std::to_string(s.level) + ", " + std::to_string(s.equipped.size()) + " equipped, " + std::to_string(yaml.size()) + " bytes)");
        return true;
    }

    void OnEvent(void* obj, void* fn, void* parms) {
        if (t_busy || !game::OnGameThread()) return;
        if (g_saved.Is(fn) || g_loaded.Is(fn)) {  // PlayerController is the first parameter of both
            if (static_cast<Params::BP_ArchonSaveGame_C_OnArchonObjectSavedForUser_Event_0*>(parms)->PlayerController == items::sdk::LocalPC())
                g_dirty = true;
            return;
        }
        if (!g_dirty || !umg::IsWorldTick(fn)) return;
        t_busy = true;
        if (Write()) g_dirty = false;
        t_busy = false;
    }

    void Listen(bool on) {
        game::On("BP_ArchonSaveGame_C", "OnArchonObjectSavedForUser_Event_0", &OnEvent, on);
        game::On("BP_ArchonSaveGame_C", "OnArchonObjectLoadedForUser_Event_0", &OnEvent, on);
        game::OnWorldTick(&OnEvent, on);
    }

    struct CharSnapshot : feature::Feature {
        CharSnapshot() : Feature("Character snapshot", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook
        void OnFrame(const feature::Frame&) override {
            if (!g_listening) {
                Listen(g_listening = true);
                g_dirty = true;  // the hero already loaded
            }
            items::io::Tick();
        }
        void Off() override {
            if (g_listening) Listen(g_listening = false);
        }
    } g_feature;
}
