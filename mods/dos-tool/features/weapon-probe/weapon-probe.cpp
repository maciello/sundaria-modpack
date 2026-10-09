#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "tmap.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdarg>
#include <map>
#include <string>
#include "Engine_classes.hpp"
#include "ArchonSpecSystem_classes.hpp"
#include "BP_SpecItemWeapon_classes.hpp"
#include "BP_SpecItemWeapon_parameters.hpp"
#include "BP_GameAbilityBase_classes.hpp"
#include "BP_GameAbility_ShootArrow_classes.hpp"
#include "BP_GameAbility_RapidShot_classes.hpp"
#include "BP_GameAbility_Salvo_classes.hpp"

// Weapon probe (dev, file trigger weapon-probe.probe next to the exe -> dos-tool-weapons.yaml). Game thread.
// Reads every loaded weapon spec by weapon type, plus the ability CDOs that name weapon types.
using namespace SDK;
using umg::PtrOk;

namespace {
    std::atomic<bool> g_probe{false};
    bool g_listening = false;
    thread_local bool t_busy = false;

    std::string F(const char* fmt, ...) {
        char b[768];
        va_list a;
        va_start(a, fmt);
        std::vsnprintf(b, sizeof b, fmt, a);
        va_end(a);
        return b;
    }

    std::string ExeDir() {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        const std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }

    // BP enums are dumped as NewEnumeratorN; the UUserDefinedEnum keeps the display names.
    std::vector<std::string> EnumNames(const char* name) {
        std::vector<std::string> out;
        UEnum* e = UObject::FindObjectFast<UEnum>(name, EClassCastFlags::Enum);
        if (!PtrOk(e)) return out;
        std::map<std::string, std::string> display;
        if (e->IsA(UUserDefinedEnum::StaticClass()))
            tmap::ForEach(static_cast<UUserDefinedEnum*>(e)->DisplayNameMap, [&](const FName& k, const FText& t) {
                if (PtrOk(t.TextData)) display[k.ToString()] = t.ToString();
            });
        for (int i = 0; i < e->Names.Num(); i++) {
            std::string n = e->Names[i].Key().ToString();
            if (auto p = n.rfind("::"); p != std::string::npos) n = n.substr(p + 2);
            const int64 v = e->Names[i].Value();
            if (v < 0 || v > 255 || n.ends_with("_MAX")) continue;
            if (int(out.size()) <= v) out.resize(v + 1);
            auto it = display.find(n);
            out[v] = it != display.end() && !it->second.empty() ? it->second : n;
        }
        return out;
    }
    std::string At(const std::vector<std::string>& n, int i) { return i >= 0 && i < int(n.size()) && !n[i].empty() ? n[i] : F("#%d", i); }

    std::string Report() {
        const auto wt = EnumNames("EWeaponType"), dt = EnumNames("EWeaponDamageType");
        auto slot = EnumNames("EBP_ItemEquipmentSlotEnum");
        if (slot.empty()) slot = EnumNames("BP_ItemEquipmentSlotEnum");
        std::string y = "enums:\n";
        for (const char* en : {"EWeaponType", "EWeaponDamageType", "EAdvanceClass", "EJobClass", "ECharacterClass", "EItemQuality"}) {
            const auto n = EnumNames(en);
            if (n.empty()) continue;
            y += F("  %s: [", en);
            for (size_t i = 0; i < n.size(); i++) y += F("%s%s", i ? ", " : "", n[i].c_str());
            y += "]\n";
        }
        auto* mgr = static_cast<UObject*>(game::FindSingleton("BP_SpecManagerItem_C"));
        y += "weapon_specs:\n";
        int n = 0;
        if (PtrOk(mgr) && mgr->IsA(UArchonSpecManager::StaticClass()))
            tmap::ForEach(static_cast<UArchonSpecManager*>(mgr)->mLoadedSpecMap, [&](int32 id, UArchonSpec* sp) {
                if (!PtrOk(sp) || !sp->IsA(UBP_SpecItemWeapon_C::StaticClass())) return;
                auto* w = static_cast<UBP_SpecItemWeapon_C*>(sp);
                const auto& d = w->WeaponItemSpecData;
                FSWeaponTypeStats st{};
                if (UFunction* fn = w->Class->GetFunction("BP_SpecItemWeapon_C", "GetWeaponTypeStat")) {
                    Params::BP_SpecItemWeapon_C_GetWeaponTypeStat p{};
                    w->ProcessEvent(fn, &p);
                    st = p.WeaponStat;
                }
                y += F("  %d: {name: %s, type: %s, anim: %s, dmgType: %s, specDmgType: %d, job: %d, slot: %d, equipSlot: %s, lvl: %d, quality: %d, "
                       "statAnim: %s, dmgMod: %.3f, estAnimSpeed: %.3f, attackSpeed: %.3f}\n",
                       id, sp->GetName().c_str(), d.mWeaponType_101_7C40707C4775D05C3C1AAB8DBA0BEA47.ToString().c_str(),
                       At(wt, int(w->WeaponAnimationType)).c_str(), At(dt, int(w->WeaponDamageType)).c_str(),
                       int(d.mWeaponDamageType_95_D2C3E99049654CD252B82D952914CE6D), int(d.JobClass_98_AD99E8AE43A0DA3616EFDFAA4E20BDAE),
                       int(d.mEquipmentSlot_100_8AEEF0F04E70266322D42483C4D134BC), At(slot, int(w->equipSlot)).c_str(),
                       d.LevelRequirement_62_2388117C42B068CF807D6CB262F76C48, int(d.Quality_96_942CB48645A93622669B6C848F6ACD8F),
                       st.AnimationType_2_7AD72AA9410CD35CEB4F69A9C1C739B6.ToString().c_str(), st.DamageModifier_5_52250031437C09A7C029A59A92FB49DC,
                       st.EstimatedAnimationSpeed_11_B4EA233C473D002E82AC7384A20F2DF6, st.AttackSpeed_12_3F598CB344F2FF777DF71DA639B7C2AE);
                n++;
            });
        y += F("weapon_spec_count: %d\n", n);
        y += "abilities:  # CDOs with WeaponTypesQualifier, per-type play rates or Salvo scale\n";
        for (int i = 0; UObject::GObjects && i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!PtrOk(o) || !PtrOk(o->Class) || !o->IsDefaultObject() || !o->IsA(UBP_GameAbilityBase_C::StaticClass())) continue;
            auto* a = static_cast<UBP_GameAbilityBase_C*>(o);
            std::string req, rates, extra;
            for (int k = 0; k < a->WeaponTypesQualifier.Num(); k++) req += (k ? ", " : "") + At(wt, int(a->WeaponTypesQualifier[k]));
            auto rate = [&](const TMap<EWeaponType, float>& m) {
                tmap::ForEach(m, [&](EWeaponType t, float v) { rates += F("%s%s: %.3f", rates.empty() ? "" : ", ", At(wt, int(t)).c_str(), v); });
            };
            if (o->IsA(UBP_GameAbility_ShootArrow_C::StaticClass())) {
                auto* s = static_cast<UBP_GameAbility_ShootArrow_C*>(o);
                rate(s->WeaponTypePlayRate);
                extra = F(", canHold: %d", int(s->CanHold));
            } else if (o->IsA(UBP_GameAbility_RapidShot_C::StaticClass()))
                rate(static_cast<UBP_GameAbility_RapidShot_C*>(o)->WeaponTypePlayRate);
            else if (o->IsA(UBP_GameAbility_Salvo_C::StaticClass()))
                extra = F(", playRateScaleCrossbow: %.3f", static_cast<UBP_GameAbility_Salvo_C*>(o)->PlayRateScaleCrossbow);
            if (req.empty() && rates.empty() && extra.empty()) continue;
            y += F("  %s: {requires: [%s], playRate: {%s}, holdToRepeat: %d, maxHold: %d, holdInterval: %.3f%s}\n", o->Class->GetName().c_str(), req.c_str(),
                   rates.c_str(), int(a->HoldToRepeatAbility), a->kMaxHoldLevel, a->mHoldInterval, extra.c_str());
        }
        return y;
    }

    void Write() {
        const std::string out = Report();
        HANDLE h = CreateFileA((ExeDir() + "dos-tool-weapons.yaml").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        DWORD w = 0;
        WriteFile(h, out.data(), DWORD(out.size()), &w, nullptr);
        CloseHandle(h);
        logger::log("[weapon-probe] " + std::to_string(out.size()) + " bytes -> dos-tool-weapons.yaml");
    }

    void OnEvent(void*, void* fn, void*) {
        if (t_busy || !game::OnGameThread() || !umg::IsWorldTick(fn)) return;
        t_busy = true;
        if (g_probe.exchange(false)) Write();
        t_busy = false;
    }

    struct WeaponProbe : feature::Feature {
        double next = 0;
        std::string path;
        WeaponProbe() : Feature("Weapon probe", feature::Stage::Alpha) { optIn = true; }
        void OnFrame(const feature::Frame& f) override {
            if (!g_listening) game::OnWorldTick(&OnEvent, g_listening = true);
            if (f.now < next) return;
            next = f.now + 1.0;
            if (path.empty()) path = ExeDir() + "weapon-probe.probe";
            if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
            DeleteFileA(path.c_str());
            g_probe = true;
        }
    } g_weapon_probe;
}
