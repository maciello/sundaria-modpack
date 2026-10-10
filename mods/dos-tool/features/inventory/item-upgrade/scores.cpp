#include "scores.hpp"
#include "../shared/items.hpp"
#include "../shared/hero.hpp"
#include "../shared/sdk.hpp"
#include "dps/dps.hpp"
#include "logger.hpp"

#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <unordered_map>
#include "BP_PersistentPlayerAccount_classes.hpp"

// Scores per item key for the current hero (its snapshot + the live equipped set) and every other saved hero
// (dos-tool-chars/<slot>.yaml, char-snapshot #102). Game thread. Cost: one bag + bank read when their raw records
// change; ScoreItem once per distinct item per hero until the hero or its equipped set changes; BestInSlot once per
// hero when the owned items change.
namespace {
    namespace io = items::io;
    using namespace item_upgrade;

    struct Hero {
        std::string name, cls;
        dps::Build build;
        std::shared_ptr<const dps::Prepared> prep;
    };
    struct Score { float self = 0; bool selfFits = false; std::vector<Other> others; };

    std::unique_ptr<dps::Model> g_model;
    bool g_modelTried = false;
    bool g_preview = false;  // no tables + dev install: preview marks (fake values, labelled) to check the look
    std::string g_status = "not started";
    std::uint64_t g_ownedSig = 0, g_ctxSig = 0;
    int g_heroSlot = -2;
    std::vector<items::Item> g_owned;                     // bag + bank
    std::vector<dps::Item> g_ownedDps;                    // parallel to g_owned
    std::unordered_map<std::uint64_t, int> g_byPos;       // Pos → index into g_owned
    Hero g_self;
    std::vector<Hero> g_others;
    std::unordered_map<std::uint64_t, Score> g_cache;     // by item key
    std::unordered_map<std::uint64_t, std::vector<std::string>> g_bis;  // item key → classes it is best in slot for
    const dps::Scenario g_scenario{};

    std::uint64_t PosKey(bool bank, std::uint8_t type, int slot) { return (std::uint64_t(bank) << 40) | (std::uint64_t(type) << 32) | std::uint32_t(slot); }
    std::uint64_t KeyOf(const dps::Item& d) { return Key(d.spec, d.level, d.grade, d.stats); }

    std::string ExeDir() {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        const std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }

    const dps::Model* Model() {
        if (g_modelTried) return g_model.get();
        g_modelTried = true;
        g_preview = GetFileAttributesA((ExeDir() + "dos-tool.dev").c_str()) != INVALID_FILE_ATTRIBUTES;
        // ponytail: tables from a local file (`just dps-install`) until the in-game table Source lands (#118); one try per DLL load
        std::string err;
        if (auto src = dps::FileSource(ExeDir() + "dos-tool-dps\\tables.txt")) g_model = dps::Model::Load(*src, err);
        else err = "no table source yet (#118)";
        if (!g_model) g_status = "no DPS tables: " + err;
        logger::log("[item-upgrade] DPS model: " + (g_model ? std::string("loaded") : g_status + (g_preview ? "; dev install: preview marks" : "")));
        return g_model.get();
    }

    dps::Item ToDps(const items::Item& it) {
        const io::Names& n = io::GetNames();
        dps::Item d;
        d.name = it.name;
        d.spec = it.specId;
        d.equipSlot = it.equipSlot >= 0 && it.equipSlot < int(n.equipSlot.size()) ? n.equipSlot[it.equipSlot] : "";
        d.weaponType = it.kind == items::Kind::Weapon ? it.typeName : "";
        d.grade = it.grade;
        d.level = it.level;
        d.slot = it.where == items::Where::Equipped ? it.slot : -1;
        for (const items::Stat& s : it.stats) d.stats.push_back({items::StatName(n.stat, s.type), s.value});
        return d;
    }
    dps::Item ToDps(const items::hero::Gear& g) {
        dps::Item d{g.name, g.spec, g.equipSlot, g.kind == "Weapon" ? g.type : "", g.grade, g.level, g.slot, {}};
        for (const auto& [name, value] : g.stats) d.stats.push_back({name, value});
        return d;
    }
    Hero HeroOf(const items::hero::Read& r) {
        Hero h{r.name, r.cls, {}, nullptr};
        h.build.cls = r.cls;
        h.build.level = r.level;
        h.build.primary = r.primaryStats;
        for (const items::hero::Learned& l : r.learned) h.build.abilities.push_back({l.name, l.level});
        for (const items::hero::Gear& g : r.equipped) h.build.equipped.push_back(ToDps(g));
        return h;
    }

    // Every saved hero; the current one's gear is replaced by the live equipped set afterwards.
    void ReadHeroes(int current) {
        g_self = {};
        g_others.clear();
        for (int slot = 0; slot < 5; slot++) {  // hero slots of the account (char-snapshot: 0..4)
            std::stringstream s;
            s << std::ifstream(ExeDir() + "dos-tool-chars\\" + std::to_string(slot) + ".yaml", std::ios::binary).rdbuf();
            const items::hero::Read r = items::hero::Parse(s.str());
            if (r.slot != slot || r.cls.empty()) continue;
            (slot == current ? g_self : g_others.emplace_back()) = HeroOf(r);
        }
    }

    bool Ready() { return g_self.prep || !g_others.empty() || (g_preview && !g_model); }

    void BestInSlot(const dps::Model& m, const std::vector<dps::Item>& equipped) {
        std::vector<dps::Item> cand = g_ownedDps;  // owned first: pick index < owned size = an owned item
        cand.insert(cand.end(), equipped.begin(), equipped.end());
        for (const Hero& h : g_others) cand.insert(cand.end(), h.build.equipped.begin(), h.build.equipped.end());
        g_bis.clear();
        auto mark = [&](const Hero& h) {
            if (h.cls.empty()) return;
            for (const dps::SlotPick& p : m.BestInSlot(h.build, g_scenario, cand).picks) {
                if (p.candidate < 0 || p.candidate >= int(g_ownedDps.size())) continue;
                auto& classes = g_bis[KeyOf(g_ownedDps[p.candidate])];
                if (std::find(classes.begin(), classes.end(), h.cls) == classes.end()) classes.push_back(h.cls);
            }
        };
        mark(g_self);
        for (const Hero& h : g_others) mark(h);
    }
}

namespace item_upgrade::scores {
    bool Refresh(bool& changed) {
        changed = false;
        if (!io::Ready()) { g_status = "item names not loaded yet"; return false; }
        const dps::Model* m = Model();
        auto* pc = items::sdk::LocalPC();
        if ((!m && !g_preview) || !pc || !items::sdk::PtrOk(pc->AccountComponent)) return false;
        const int hero = pc->AccountComponent->ActiveHeroSlot;
        const io::Located l = io::Locate();
        const std::uint64_t ownedSig = io::Signature(l.bag) ^ (io::Signature(l.bank) * 31) ^ 1;
        if (ownedSig == g_ownedSig && hero == g_heroSlot) return Ready();

        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        g_ownedSig = ownedSig;
        g_owned.clear();
        std::vector<dps::Item> equipped;
        std::uint64_t ctx = std::uint64_t(hero) + 1;
        for (items::Item& it : io::Read(l.bag, false, true)) {
            if (it.where == items::Where::Equipped) {
                equipped.push_back(ToDps(it));
                ctx = (ctx * 1099511628211ull) ^ KeyOf(equipped.back()) ^ std::uint64_t(it.slot);
            } else if (it.where == items::Where::Inventory) g_owned.push_back(std::move(it));
        }
        const io::Bank bank = io::ReadBank(true);
        g_owned.insert(g_owned.end(), bank.items.begin(), bank.items.end());
        g_ownedDps.clear();
        g_byPos.clear();
        for (int i = 0; i < int(g_owned.size()); i++) {
            g_ownedDps.push_back(ToDps(g_owned[i]));
            g_byPos[PosKey(g_owned[i].bank, g_owned[i].containerType, g_owned[i].slot)] = i;
        }
        if (hero != g_heroSlot) { ReadHeroes(hero); g_heroSlot = hero; g_ctxSig = 0; }
        if (ctx != g_ctxSig) {  // hero or its equipped set changed: every score is stale
            g_ctxSig = ctx;
            g_cache.clear();
            g_self.build.equipped = equipped;
            if (m) g_self.prep = g_self.cls.empty() ? nullptr : m->Prepare(g_self.build, g_scenario);
            if (m) for (Hero& h : g_others) if (!h.prep) h.prep = m->Prepare(h.build, g_scenario);
        }
        if (m) BestInSlot(*m, equipped);
        changed = true;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        g_status = std::to_string(g_owned.size()) + " owned (bank " + (bank.live ? "live" : bank.seen ? "cached" : "not seen yet") + "), hero " +
                   (g_self.cls.empty() ? "slot " + std::to_string(hero) + " has no snapshot" : g_self.name + " " + g_self.cls) + ", " +
                   std::to_string(g_others.size()) + " other heroes, " + std::to_string(g_bis.size()) + " best in slot, " +
                   std::to_string(double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart)).substr(0, 5) + " ms";
        return Ready();
    }

    void Invalidate() { g_ownedSig = 0; g_ctxSig = 0; }

    Verdict For(const items::tiles::Pos& p) {
        const auto at = g_byPos.find(PosKey(p.bank, p.containerType, p.slot));
        if (at == g_byPos.end() || (!g_model && !g_preview)) return {};
        const dps::Item& d = g_ownedDps[at->second];
        if (d.equipSlot.empty()) return {};  // not equipable
        if (!g_model) {  // dev preview: every third item an upgrade, every third a mark for another hero
            const int k = at->second % 3;
            return Judge(k == 0 ? 1.f + float(d.spec % 150) / 10.f : 0.f, k == 0, {{"Preview", "no DPS data", k == 1 ? 5.f : 0.f, k == 1}}, {});
        }
        const std::uint64_t key = KeyOf(d);
        auto hit = g_cache.find(key);
        if (hit == g_cache.end()) {
            Score s;
            if (g_self.prep) {
                const dps::ItemScore r = g_model->ScoreItem(*g_self.prep, d);
                s.self = r.deltaPct;
                s.selfFits = r.fits;
            }
            for (const Hero& h : g_others) {
                if (!h.prep) continue;
                const dps::ItemScore r = g_model->ScoreItem(*h.prep, d);
                s.others.push_back({h.name, h.cls, r.deltaPct, r.fits});
            }
            hit = g_cache.emplace(key, std::move(s)).first;
        }
        const auto bis = g_bis.find(key);
        return Judge(hit->second.self, hit->second.selfFits, hit->second.others, bis == g_bis.end() ? std::vector<std::string>{} : bis->second);
    }

    const std::string& Status() { return g_status; }
    bool Preview() { return g_preview && !g_model; }

    void Reset() {
        g_ownedSig = g_ctxSig = 0;
        g_heroSlot = -2;
        g_owned.clear();
        g_ownedDps.clear();
        g_byPos.clear();
        g_self = {};
        g_others.clear();
        g_cache.clear();
        g_bis.clear();
    }
}
