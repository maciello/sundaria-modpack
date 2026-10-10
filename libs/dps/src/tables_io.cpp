// FileSource: the offline tables file (written by scripts/dps_tables.py from the extracted pak data).
// One record per line, whitespace-separated tokens, "-" = empty. First line "dps-tables 1".
//   stats <EStatType...> | percent <stat...> | master <stat> <50 values> | slot <slot> <v> | ignored <stat...>
//   equiv <row> <MAP RAP SP BaseToPlate ResistElementalBaseToCloth> | grades <EItemGrade...>
//   grade <name> <quality elementalMagicMod addMin addMax fillerMin fillerMax mandMin mandMax>
//   armortypes|equipslots|classes <names...> | weapon <row> <animationType> <damageModifier> | wdtype <row> <type>
//   ap <row> <stat1> <mod1> <stat2> <mod2> <levelMod> | primary <6 values> | maxlevel <n>
//   pool <tag> mandatory <stat...> | pool <tag> <grade> <0 additional|1 filler|2 mandatory-by-grade> <stat...>
//   rangedattack <ability> <types...>
//   spec <id> <equipSlot> <armorType> <weaponType> <tag> <randomStatType> <job> | heroism <node> <maxPoints> <perPoint>
//   ability <class> <name> <projectile 0|1> <activateSection>, then for it: learned <names...> | qualifier <types...>
//     | playrate <type> <v> | animrate <5> | cooldown <5> | montage <name> | section <name> <hits shots lockEnd length>
//     | damage <src> <apRule> <magic> <element> <bonusStat> <dot> <aoe> <aoeCap> | coef <5> | dotdur <5> | dotper <5>
#include "dps/source.hpp"
#include <fstream>
#include <sstream>

namespace dps {
    namespace {
        std::string Opt(const std::string& s) { return s == "-" ? std::string() : s; }
        std::vector<std::string> Rest(std::istringstream& in) {
            std::vector<std::string> v;
            for (std::string s; in >> s;) v.push_back(s);
            return v;
        }
        template<size_t N> bool Floats(std::istringstream& in, std::array<float, N>& a) {
            for (auto& x : a) if (!(in >> x)) return false;
            return true;
        }

        class File : public Source {
        public:
            explicit File(std::string path) : path_(std::move(path)) {}
            bool Load(Tables& t, std::string& error) override {
                std::ifstream f(path_);
                if (!f) { error = "cannot open tables file " + path_; return false; }
                std::string line;
                if (!std::getline(f, line) || line != "dps-tables 1") { error = path_ + ": not a dps-tables 1 file"; return false; }
                t = Tables{};
                t.origin = path_;
                Ability* ab = nullptr;
                DamageComponent* dc = nullptr;
                int n = 1;
                while (std::getline(f, line)) {
                    ++n;
                    std::istringstream in(line);
                    std::string k;
                    if (!(in >> k)) continue;
                    bool ok = true;
                    if (k == "stats") t.stats = Rest(in);
                    else if (k == "percent") t.percentStats = Rest(in);
                    else if (k == "master") { std::string s; in >> s; ok = Floats(in, t.master[s]); }
                    else if (k == "slot") { std::string s; in >> s; ok = bool(in >> t.slotDistribution[s]); }
                    else if (k == "ignored") t.ignoredDistribution = Rest(in);
                    else if (k == "equiv") {
                        std::string r; in >> r;
                        auto& e = t.armorEquivalency[r];
                        ok = bool(in >> e.map >> e.rap >> e.sp >> e.baseToPlate >> e.resistElementalBaseToCloth);
                    }
                    else if (k == "grades") t.grades = Rest(in);
                    else if (k == "grade") {
                        std::string g; in >> g;
                        auto& r = t.gradeRows[g];
                        ok = bool(in >> r.quality >> r.elementalMagicMod >> r.additionalMin >> r.additionalMax >> r.fillerMin >> r.fillerMax
                                     >> r.mandatoryByGradeMin >> r.mandatoryByGradeMax);
                    }
                    else if (k == "armortypes") t.armorTypes = Rest(in);
                    else if (k == "equipslots") t.equipSlots = Rest(in);
                    else if (k == "classes") t.classes = Rest(in);
                    else if (k == "weapon") { std::string r; in >> r; auto& w = t.weaponStats[r]; ok = bool(in >> w.animationType >> w.damageModifier); }
                    else if (k == "wdtype") { std::string r; in >> r; ok = bool(in >> t.weaponDamageType[r]); }
                    else if (k == "ap") { std::string r; in >> r; auto& a = t.attackPower[r]; ok = bool(in >> a.stat1 >> a.mod1 >> a.stat2 >> a.mod2 >> a.levelMod); }
                    else if (k == "primary") ok = Floats(in, t.primaryDefault);
                    else if (k == "rangedattack") { ok = bool(in >> t.rangedAttack.ability); t.rangedAttack.types = Rest(in); }
                    else if (k == "maxlevel") ok = bool(in >> t.maxLevel);
                    else if (k == "pool") {
                        std::string tag, g; in >> tag >> g;
                        auto& p = t.craftPools[tag];
                        if (g == "mandatory") p.mandatory = Rest(in);
                        else { int i = -1; ok = bool(in >> i) && i >= 0 && i < 3; if (ok) p.byGrade[g][i] = Rest(in); }
                    }
                    else if (k == "spec") {
                        int id; ItemSpec s; std::string slot, at, wt, tag;
                        ok = bool(in >> id >> slot >> at >> wt >> tag >> s.randomStatType >> s.job);
                        s.equipSlot = Opt(slot), s.armorType = Opt(at), s.weaponType = Opt(wt), s.tag = Opt(tag);
                        if (ok) t.specs[id] = s;
                    }
                    else if (k == "heroism") { std::string nd; in >> nd; auto& h = t.heroism[nd]; ok = bool(in >> h.maxPoints >> h.perPoint); }
                    else if (k == "ability") {
                        std::string cls, sec; int proj = 0;
                        Ability a;
                        ok = bool(in >> cls >> a.name >> proj >> sec);
                        a.projectile = proj, a.activateSection = Opt(sec);
                        auto& v = t.abilities[cls];
                        v.push_back(std::move(a));
                        ab = &v.back(), dc = nullptr;
                    }
                    else if (!ab) ok = false;
                    else if (k == "learned") ab->learnedAs = Rest(in);
                    else if (k == "qualifier") ab->weaponQualifier = Rest(in);
                    else if (k == "playrate") { std::string w; in >> w; ok = bool(in >> ab->weaponPlayRate[w]); }
                    else if (k == "animrate") ok = Floats(in, ab->animRate);
                    else if (k == "cooldown") ok = Floats(in, ab->cooldown);
                    else if (k == "montage") { ab->montages.push_back({}); ok = bool(in >> ab->montages.back().name); }
                    else if (k == "section") {
                        Section s;
                        ok = !ab->montages.empty() && bool(in >> s.name >> s.hits >> s.shots >> s.lockEnd >> s.length);
                        if (ok) ab->montages.back().sections.push_back(s);
                    }
                    else if (k == "damage") {
                        DamageComponent d; std::string bonus; int magic = 0, dot = 0;
                        ok = bool(in >> d.src >> d.apRule >> magic >> d.element >> bonus >> dot >> d.aoe >> d.aoeCap);
                        d.magic = magic, d.dot = dot, d.bonusStat = Opt(bonus);
                        ab->damage.push_back(d);
                        dc = &ab->damage.back();
                    }
                    else if (!dc) ok = false;
                    else if (k == "coef") ok = Floats(in, dc->coef);
                    else if (k == "dotdur") ok = Floats(in, dc->dotDuration);
                    else if (k == "dotper") ok = Floats(in, dc->dotPeriod);
                    else ok = false;
                    if (!ok) { error = path_ + ":" + std::to_string(n) + ": bad record '" + k + "'"; return false; }
                }
                if (t.stats.empty() || t.master.empty() || t.abilities.empty()) { error = path_ + ": stats, master or abilities missing"; return false; }
                return true;
            }

        private:
            std::string path_;
        };
    }

    std::unique_ptr<Source> FileSource(std::string path) { return std::make_unique<File>(std::move(path)); }
}
