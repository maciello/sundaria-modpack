// C ABI for offline hosts (scripts/dps.py): raw kernel (threads) + model requests as line text, answers as YAML.
// Offline only: std::thread is not allowed in DoS-Tool.dll (gotchas.md), so this file is not in the DLL build.
//
// Request: one record per line, tokens separated by spaces, "-" = empty:
//   cmd dps|score|bis|weights|predict|bench
//   scenario <name> <targets> <fightSeconds> <targetLevelDelta> <armor> <magicResist> <glancing> <deflect> <incoming> <event 0|1>
//   resist <damage type> <value>
//   build <class> <level> <abilityLevel> | primary <6 values> | learned <ability> <level> | heroism <node> <points>
//   equipped|cand <spec> <equipSlot> <weaponType> <grade> <level> <slot> <name, rest of line>
//   stat <name> <value>            (adds to the last equipped/cand item)
//   grade <EItemGrade value>       (weights)  | reps <n> (bench)
#include "../src/compile.hpp"
#include "../src/items.hpp"
#include "../src/kernel.hpp"
#include "../src/prepared.hpp"
#include "dps/c_api.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>
#include <thread>

struct dps_model { std::unique_ptr<dps::Model> m; };

extern "C" void dps_eval(int n, const float* attrs, const dps_gear* g, const dps_ctx* c, float* out, float* per_ability) {
    const char* e = std::getenv("DPS_THREADS");
    const int nt = std::max(1, std::min(e ? std::atoi(e) : 1, n / 1024));
    if (nt == 1) { dps::kernel::EvalRange(0, n, attrs, g, *c, out, per_ability); return; }
    std::vector<std::thread> th;
    for (int i = 0; i < nt; ++i)
        th.emplace_back(dps::kernel::EvalRange, int((long)n * i / nt), int((long)n * (i + 1) / nt), attrs, g, std::cref(*c), out, per_ability);
    for (auto& t : th) t.join();
}

extern "C" dps_model* dps_open(const char* path, char* err, int len) {
    std::string e;
    auto src = dps::FileSource(path);
    auto m = dps::Model::Load(*src, e);
    if (!m) { std::snprintf(err, len, "%s", e.c_str()); return nullptr; }
    return new dps_model{std::move(m)};
}
extern "C" void dps_close(dps_model* m) { delete m; }

namespace {
    using namespace dps;
    std::string Q(const std::string& s) {
        std::string o = "\"";
        for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
        return o + "\"";
    }
    std::string N(double v) { char b[32]; std::snprintf(b, sizeof b, "%.6g", v); return b; }
    std::string Opt(const std::string& s) { return s == "-" ? std::string() : s; }
    std::string Canon(const Tables& t, const std::string& s) {   // item stat name -> EStatType name (Map -> MAP)
        for (const auto& x : t.stats) if (compile::Lower(x) == compile::Lower(s)) return x;
        return s;
    }

    struct Request {
        std::string cmd = "dps";
        Scenario sc;
        Build b;
        std::vector<Item> cands;
        int grade = 3, reps = 20;
        Item* last = nullptr;
    };
    bool Parse(const char* text, Request& r, std::string& err) {
        std::istringstream all(text);
        std::string line;
        while (std::getline(all, line)) {
            std::istringstream in(line);
            std::string k;
            if (!(in >> k)) continue;
            bool ok = true;
            if (k == "cmd") ok = bool(in >> r.cmd);
            else if (k == "scenario") {
                int ev = 0;
                ok = bool(in >> r.sc.name >> r.sc.targets >> r.sc.fightSeconds >> r.sc.targetLevelDelta >> r.sc.armor >> r.sc.magicResist
                             >> r.sc.glancing >> r.sc.deflect >> r.sc.incomingDamageMod >> ev);
                r.sc.eventSim = ev;
            }
            else if (k == "resist") { std::string n; float v; ok = bool(in >> n >> v); r.sc.resist.push_back({n, v}); }
            else if (k == "build") ok = bool(in >> r.b.cls >> r.b.level >> r.b.abilityLevel);
            else if (k == "primary") { r.b.primary.assign(6, 0.f); for (float& v : r.b.primary) ok = ok && bool(in >> v); }
            else if (k == "learned") { std::string n; int v; ok = bool(in >> n >> v); r.b.abilities.push_back({n, v}); }
            else if (k == "heroism") { std::string n; int v; ok = bool(in >> n >> v); r.b.heroism.push_back({n, v}); }
            else if (k == "equipped" || k == "cand") {
                Item it;
                ok = bool(in >> it.spec >> it.equipSlot >> it.weaponType >> it.grade >> it.level >> it.slot);
                it.equipSlot = Opt(it.equipSlot), it.weaponType = Opt(it.weaponType);
                std::getline(in >> std::ws, it.name);
                auto& v = k == "cand" ? r.cands : r.b.equipped;
                v.push_back(std::move(it));
                r.last = &v.back();
            }
            else if (k == "stat") { Stat s; ok = r.last && bool(in >> s.name >> s.value); if (ok) r.last->stats.push_back(s); }
            else if (k == "grade") ok = bool(in >> r.grade);
            else if (k == "reps") ok = bool(in >> r.reps);
            else ok = false;
            if (!ok) { err = "bad request line: " + line; return false; }
        }
        return true;
    }

    std::string DpsYaml(const Result& res) {
        if (!res.error.empty()) return "error: " + Q(res.error) + "\n";
        std::string y = "dps: " + N(res.dps) + "\nweapon: " + Q(res.weaponType) + "\nabilities:\n";
        for (const auto& a : res.abilities)
            y += "  - {name: " + Q(a.name) + ", dps: " + N(a.dps) + ", damage_per_cast: " + N(a.damagePerCast) + ", cast_s: " +
                 N(a.castSeconds) + ", cooldown_s: " + N(a.cooldown) + ", casts: " + N(a.casts) + "}\n";
        y += "ignored_stats: [";
        for (size_t i = 0; i < res.ignoredStats.size(); ++i) y += (i ? ", " : "") + Q(res.ignoredStats[i]);
        return y + "]\n";
    }

    // every stat of an item predicted from (spec, slot, grade, level); src = which pick list can give it
    std::string Predict(const Tables& t, const Item& it) {
        auto sp = t.specs.find(it.spec);
        if (sp == t.specs.end()) return "  - {name: " + Q(it.name) + ", error: \"unknown spec\"}\n";
        const std::string g = it.grade >= 0 && it.grade < int(t.grades.size()) ? t.grades[it.grade] : "";
        const auto pool = items::CraftPool(t, sp->second.tag, g);
        std::string y = "  - name: " + Q(it.name) + "\n    slot: " + Q(sp->second.equipSlot) + "\n    grade: " + Q(g) + "\n    level: " +
                        std::to_string(it.level) + "\n    tag: " + Q(sp->second.tag) + "\n    stats:\n";
        const char* names[] = {"additional", "filler", "mandatory_by_grade"};
        for (const Stat& s : it.stats) {
            const std::string st = Canon(t, s.name);
            std::string src;
            if (std::find(pool.mandatory.begin(), pool.mandatory.end(), st) != pool.mandatory.end()) src = "mandatory";
            for (int i = 0; i < 3 && src.empty(); ++i)
                if (std::find(pool.lists[i].stats.begin(), pool.lists[i].stats.end(), st) != pool.lists[i].stats.end()) src = names[i];
            y += "      " + s.name + ": {actual: " + N(s.value);
            if (src.empty()) { y += ", pred: null, src: \"not in pool\"}\n"; continue; }
            const bool weapon = !sp->second.weaponType.empty();
            const float v = items::CraftValue(t, st, sp->second.equipSlot, it.level, g, weapon ? "" : sp->second.armorType,
                                              weapon ? sp->second.weaponType : "");
            y += ", pred: " + N(v) + ", src: " + src + "}\n";
        }
        return y;
    }

    double Ms(std::chrono::steady_clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

    std::string Bench(const Model& m, const Request& r) {
        using clk = std::chrono::steady_clock;
        auto t0 = clk::now();
        auto p = m.Prepare(r.b, r.sc);
        const double prep = Ms(clk::now() - t0);
        if (p->best < 0) return "error: " + Q(p->error) + "\n";
        // kernel: 10k rows = the build's row with every attribute jittered (+-50 %)
        const auto& md = p->modes[p->best];
        const dps_ctx c = md.c.Ctx();
        const int n = 10000, A = c.A;
        std::vector<float> rows((size_t)n * A), out(n);
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> u(0.5f, 1.5f);
        for (int b = 0; b < n; ++b)
            for (int i = 0; i < A; ++i) rows[(size_t)b * A + i] = i == layout::I_WELIDX ? md.row.x[i] : md.row.x[i] * u(rng);
        double kernel = 1e9, score = 1e9;
        for (int k = 0; k < r.reps; ++k) {
            t0 = clk::now();
            dps_eval(n, rows.data(), nullptr, &c, out.data(), nullptr);
            kernel = std::min(kernel, Ms(clk::now() - t0));
            t0 = clk::now();
            for (const Item& it : r.cands) (void)m.ScoreItem(*p, it);
            score = std::min(score, Ms(clk::now() - t0));
        }
        return "prepare_ms: " + N(prep) + "\nkernel_10k_builds_ms_best: " + N(kernel) + "\nscore_items: " + std::to_string(r.cands.size()) +
               "\nscore_all_ms_best: " + N(score) + "\nreps: " + std::to_string(r.reps) + "\n";
    }
}

extern "C" int dps_call(dps_model* h, const char* request, char* out, int cap) {
    if (!h || !request) return -1;
    Request r;
    std::string err, y;
    const Model& m = *h->m;
    if (!Parse(request, r, err)) y = "error: " + Q(err) + "\n";
    else if (r.cmd == "dps") y = DpsYaml(m.Dps(r.b, r.sc));
    else if (r.cmd == "score") {
        auto p = m.Prepare(r.b, r.sc);
        y = "dps: " + N(p->dps()) + "\n" + (p->error.empty() ? "" : "error: " + Q(p->error) + "\n") + "items:\n";
        for (size_t i = 0; i < r.cands.size(); ++i) {
            const ItemScore s = m.ScoreItem(*p, r.cands[i]);
            y += "  - {i: " + std::to_string(i) + ", name: " + Q(r.cands[i].name) + ", slot: " + Q(r.cands[i].equipSlot) + ", fits: " +
                 (s.fits ? "true" : "false") + ", delta_pct: " + N(s.deltaPct) + ", dps: " + N(s.dps) + ", replaces: " +
                 std::to_string(s.replacesSlot) + "}\n";
        }
    }
    else if (r.cmd == "bis") {
        const BestInSlotResult b = m.BestInSlot(r.b, r.sc, r.cands);
        if (!b.error.empty()) y = "error: " + Q(b.error) + "\n";
        else {
            y = "dps: " + N(b.dps) + "\nweapon: " + Q(b.weaponType) + "\npicks:\n";
            for (const auto& p : b.picks)
                y += "  - {slot: " + Q(p.equipSlot) + ", i: " + std::to_string(p.candidate) + ", name: " +
                     (p.candidate >= 0 ? Q(r.cands[p.candidate].name) : "null") + "}\n";
        }
    }
    else if (r.cmd == "weights") {
        y = "weights:\n";
        for (const auto& w : m.StatWeights(r.b, r.sc, r.grade))
            y += "  - {stat: " + Q(w.stat) + ", pct_best_slot: " + N(w.pctBestSlot) + ", best_slot: " + Q(w.bestSlot) + ", value: " +
                 N(w.value) + ", pct_expected_per_item: " + N(w.pctExpectedPerItem) + "}\n";
    }
    else if (r.cmd == "predict") {
        y = "items:\n";
        for (const Item& it : r.b.equipped) y += Predict(m.tables(), it);
    }
    else if (r.cmd == "bench") y = Bench(m, r);
    else y = "error: " + Q("unknown cmd " + r.cmd) + "\n";
    const int n = int(std::min<size_t>(y.size(), size_t(cap > 0 ? cap - 1 : 0)));
    if (cap > 0) std::memcpy(out, y.data(), n), out[n] = 0;
    return int(y.size());
}

extern "C" float dps_item_stat(dps_model* h, int spec, const char* stat, int level, int grade) {
    const Tables& t = h->m->tables();
    auto sp = t.specs.find(spec);
    if (sp == t.specs.end() || grade < 0 || grade >= int(t.grades.size())) return -1.f;
    const bool weapon = !sp->second.weaponType.empty();
    return items::CraftValue(t, Canon(t, stat), sp->second.equipSlot, level, t.grades[grade], weapon ? "" : sp->second.armorType,
                             weapon ? sp->second.weaponType : "");
}
