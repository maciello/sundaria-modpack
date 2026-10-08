// just test
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
// Lua itself is part of this test binary. After every std header: its private headers define macros
// (next, ...) that break std headers included later.
#include "../../../third_party/lua/lua_all.cpp"
#include "../script.hpp"

using namespace ability_script;
using Files = std::vector<std::pair<std::string, std::string>>;

static bool Has(const std::vector<Error>& es, const char* file, const char* part) {
    for (const Error& e : es) if (e.file == file && e.msg.find(part) != std::string::npos) return true;
    return false;
}

int main() {
    // Names.
    assert(ShortName("BP_GameAbility_FireBall_C") == "FireBall");
    assert(ShortName("BP_BonusAbility_FireBall_C") == "FireBall");
    assert(ShortName("SomethingElse") == "SomethingElse");
    assert(Matches("FireBall", "BP_GameAbility_FireBall_C") && Matches("fireball", "BP_GameAbility_FireBall_C"));
    assert(Matches("BP_GameAbility_FireBall_C", "BP_GameAbility_FireBall_C"));
    assert(!Matches("Fire", "BP_GameAbility_FireBall_C") && Matches("Fire*", "BP_GameAbility_FireBall_C"));
    assert(Matches("Bash*", "BP_GameAbility_Bash_Scroll_Lv4_C") && Matches("*", "BP_GameAbility_Bash_C"));
    assert(Wanted("a.lua") && !Wanted("_examples.lua") && !Wanted("a.txt") && !Wanted(".lua"));

    // Directory changes.
    std::string memo;
    assert(Changed({{"a.lua", 1, 10}, {"_x.lua", 1, 1}}, memo));
    assert(!Changed({{"a.lua", 1, 10}, {"_x.lua", 2, 1}}, memo));  // ignored file changed
    assert(Changed({{"a.lua", 2, 10}}, memo));
    assert(Changed({}, memo));

    Host h;
    const std::string fire = "BP_GameAbility_FireBall_C", bash = "BP_GameAbility_Bash_C";

    // Tweaks: "*" first, named wins, unknown keys and out-of-range values are errors with file:line.
    h.Load({{"a.lua", "ability.tweak('*', {anim_rate = 1.1})\nability.tweak('FireBall', {anim_rate = 1.3, cooldown = 0.5})\n"},
            {"b.lua", "ability.tweak('FireBall', {speed = 2})\n"},
            {"c.lua", "\nability.tweak('FireBall', {cooldown = 2})\n"}});
    Tweak t = h.TweakFor(fire);
    assert(t.hasRate && t.animRate > 1.29f && t.animRate < 1.31f && t.hasCooldown && t.cooldown == 0.5f);
    t = h.TweakFor(bash);
    assert(t.hasRate && t.animRate > 1.09f && !t.hasCooldown && t.cooldown == 1.0f);
    assert(Has(h.Errors(), "b.lua", "b.lua:1:") && Has(h.Errors(), "b.lua", "unknown tweak 'speed'"));
    assert(Has(h.Errors(), "c.lua", "c.lua:2:") && Has(h.Errors(), "c.lua", "cooldown must be"));
    assert(h.Rules().size() == 2);

    // Projectile swap: a class name, merged like the others.
    h.Load({{"p.lua", "ability.tweak('*', {projectile = 'BP_Projectile_A_C'})\nability.tweak('FireBall', {projectile = 'BP_Projectile_B_C'})"},
            {"q.lua", "ability.tweak('Bash', {projectile = 5})"}});
    assert(h.TweakFor(fire).projectile == "BP_Projectile_B_C" && h.TweakFor(bash).projectile == "BP_Projectile_A_C");
    assert(Has(h.Errors(), "q.lua", "projectile must be"));

    // Syntax error: the file's last good version keeps running; other files are untouched.
    h.Load({{"a.lua", "ability.tweak('FireBall', {anim_rate = 2})"}});
    assert(h.Errors().empty() && h.TweakFor(fire).animRate == 2.0f);
    h.Load({{"a.lua", "ability.tweak('FireBall', {anim_rate = 3}"}});  // missing ')'
    assert(Has(h.Errors(), "a.lua", "a.lua:1:") && h.TweakFor(fire).animRate == 2.0f);
    // A runtime error after a declaration takes that declaration back too.
    h.Load({{"z.lua", "ability.tweak('Bash', {anim_rate = 4})\nerror('boom')"}});
    assert(Has(h.Errors(), "z.lua", "z.lua:2: boom") && !h.TweakFor(bash).hasRate && h.Rules().empty());

    // Endless loops stop at the instruction budget; the host keeps working.
    h.budget = 200'000;
    h.Load({{"loop.lua", "while true do end"}, {"ok.lua", "ability.tweak('Bash', {cooldown = 0})"}});
    assert(Has(h.Errors(), "loop.lua", "instruction limit") && h.TweakFor(bash).cooldown == 0.0f);
    h.Load({{"loopy.lua", "ability.on('*', 'activate', function() while true do end end)"}});
    std::vector<Command> out;
    h.Fire(bash, Event::Activate, out);
    assert(Has(h.Errors(), "loopy.lua", "instruction limit") && out.empty());

    // Memory cap.
    h.Load({{"mem.lua", "local s = string.rep('x', 1e8)"}});
    assert(Has(h.Errors(), "mem.lua", "memory") && h.Memory() < h.memoryCap);
    h.budget = 2'000'000;

    // Sandbox: no files, processes or bytecode.
    h.Load({{"s.lua", "assert(io == nil and os == nil and require == nil and load == nil and dofile == nil and loadfile == nil and debug == nil)"}});
    assert(h.Errors().empty());

    // Events + actions; ctx:f() and ctx.f() both work; actions outside handlers are rejected.
    h.Load({{"e.lua", R"(
        ability.on('FireBall', 'out', function(ctx) ctx:dash(900) ctx.play_rate(1.5) ctx:log(ctx.ability, ctx.event) end)
        ability.on('*', 'activate', function(ctx) ctx:apply_effect('BP_GameplayEffect_Haste_C') end)
        ability.on('Bash', 'end', function(ctx) ctx:cancel() end)
        ability.on('Bash', 'end', function(ctx) ctx:dash(99999) end)
    )"}});
    assert(h.Errors().empty() && h.Hooks().size() == 4);
    out.clear(); h.Fire(fire, Event::Out, out);
    assert(out.size() == 2 && out[0].kind == Command::Kind::Dash && out[0].value == 900.0f);
    assert(out[1].kind == Command::Kind::PlayRate && out[1].value == 1.5f);
    assert(!h.Log().empty() && h.Log().back() == "FireBall out");
    out.clear(); h.Fire(bash, Event::Activate, out);
    assert(out.size() == 1 && out[0].kind == Command::Kind::ApplyEffect && out[0].text == "BP_GameplayEffect_Haste_C");
    out.clear(); h.Fire(bash, Event::End, out);
    assert(out.size() == 1 && out[0].kind == Command::Kind::Cancel && Has(h.Errors(), "e.lua", "dash speed must be"));
    h.Load({{"bad.lua", "ability.on('X', 'boom', function() end)"}, {"bad2.lua", "ability.on('X', 'out', 5)"}});
    assert(Has(h.Errors(), "bad.lua", "unknown event 'boom'") && Has(h.Errors(), "bad2.lua", "bad2.lua:1:"));
    h.Load({{"late.lua", "ability.on('*', 'out', function() ability.tweak('*', {cooldown = 0}) end)"}});
    out.clear(); h.Fire(bash, Event::Out, out);
    assert(Has(h.Errors(), "late.lua", "top level") && h.Rules().empty());

    // New abilities take over their donor's casts until switched off.
    h.Load({{"n.lua", R"(
        ability.tweak('FireBall', {anim_rate = 1.2})
        ability.new{name = 'Blink Shot', donor = 'FireBall', cooldown = 0.6, anim_rate = 1.5,
                    on_activate = function(ctx) ctx:dash(1200) end, on_end = function(ctx) ctx:log(ctx.ability) end}
    )"}, {"dup.lua", "ability.new{name = 'Blink Shot', donor = 'Bash'}"}, {"bad.lua", "ability.new{name = 'X', donor = 'Y', colour = 1}"}});
    assert(h.News().size() == 1 && h.News()[0].label == "Blink Shot");
    assert(Has(h.Errors(), "dup.lua", "already exists") && Has(h.Errors(), "bad.lua", "unknown field 'colour'"));
    t = h.TweakFor(fire);
    assert(t.animRate == 1.5f && t.cooldown > 0.59f && t.cooldown < 0.61f);
    out.clear(); h.Fire(fire, Event::Activate, out);
    assert(out.size() == 1 && out[0].value == 1200.0f);
    out.clear(); h.Fire(fire, Event::End, out);
    assert(h.Log().back() == "Blink Shot");
    h.SetActive("Blink Shot", false);
    assert(h.TweakFor(fire).animRate > 1.19f && h.TweakFor(fire).animRate < 1.21f && !h.TweakFor(fire).hasCooldown);
    out.clear(); h.Fire(fire, Event::Activate, out);
    assert(out.empty());
    h.Load({{"n.lua", "ability.new{name = 'Blink Shot', donor = 'FireBall'}"}});
    assert(!h.Active("Blink Shot"));  // the player's choice survives reloads
    h.SetActive("Blink Shot", true);

    // The shipped examples load cleanly.
    std::ifstream f("mods/dos-tool/abilities/_examples.lua");
    assert(f && "run from the repo root");
    std::stringstream src; src << f.rdbuf();
    h.Load({{"_examples.lua", src.str()}});
    assert(h.Errors().empty() && h.Rules().size() == 3 && h.Hooks().size() == 1 && h.News().size() == 1);

    // The menu republishes on Version() changes.
    const unsigned v = h.Version();
    h.Report("apply_effect", "no GameplayEffect class 'X'");
    assert(h.Version() != v && Has(h.Errors(), "apply_effect", "'X'"));

    h.Close();
    assert(!h.Open() && h.Memory() == 0 && h.Rules().empty());
    std::puts("ok");
}
