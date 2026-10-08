#pragma once
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "../../third_party/lua/src/lua.h"
#include "../../third_party/lua/src/lauxlib.h"
#include "../../third_party/lua/src/lualib.h"

// Ability scripts: a sandboxed Lua host (SDK-free, tested in test/script_test.cpp).
// Scripts only declare things and record commands; the feature's C++ systems read the declarations
// and run the commands on the game thread. Lua API (see mods/dos-tool/abilities/_examples.lua):
//   ability.tweak("FireBall", {anim_rate = 1.3, cooldown = 0.5, projectile = "BP_Projectile_PoisonArrow_C"})
//   ability.on("FireBall", "out", function(ctx) ctx:dash(900) end)        -- activate | out | end
//   ability.new{name = "Blink", donor = "Dash", cooldown = 0.5, on_out = function(ctx) ... end}
//   ctx:dash(speed)  ctx:apply_effect("BP_GameplayEffect_X_C")  ctx:play_rate(x)  ctx:cancel()  ctx:log(...)
// Sandbox: only base (minus load/loadfile/dofile), string, table, math, utf8, coroutine; an instruction
// budget per call (endless loops error out) and a memory cap.
namespace ability_script {
    enum class Event { Activate, Out, End };
    constexpr const char* kEventNames[] = {"activate", "out", "end"};

    struct Tweak {
        float animRate = 1.0f, cooldown = 1.0f;
        std::string projectile;  // projectile class to shoot instead (empty = the ability's own)
        bool hasRate = false, hasCooldown = false;
    };
    constexpr float kMinRate = 0.1f, kMaxRate = 5.0f, kMaxDash = 5000.0f;

    struct Command {
        enum class Kind { Dash, ApplyEffect, PlayRate, Cancel } kind;
        float value = 0;
        std::string text;
    };

    struct Error { std::string file, msg; };

    // "BP_GameAbility_FireBall_C" -> "FireBall" (also BP_BonusAbility_); other names lose only "_C".
    inline std::string ShortName(std::string s) {
        if (s.size() > 2 && s.ends_with("_C")) s.resize(s.size() - 2);
        for (const char* p : {"BP_GameAbility_", "BP_BonusAbility_"})
            if (s.starts_with(p)) return s.substr(std::strlen(p));
        return s;
    }

    // Case-insensitive glob ('*' = any run of characters).
    inline bool Glob(const char* p, const char* s) {
        if (*p == '*') return Glob(p + 1, s) || (*s && Glob(p, s + 1));
        if (!*p) return !*s;
        return *s && std::tolower(static_cast<unsigned char>(*p)) == std::tolower(static_cast<unsigned char>(*s))
            && Glob(p + 1, s + 1);
    }

    // A pattern names an ability by its short name ("FireBall", "Bash*", "*") or full class name.
    inline bool Matches(const std::string& pattern, const std::string& cls) {
        return Glob(pattern.c_str(), ShortName(cls).c_str()) || Glob(pattern.c_str(), cls.c_str());
    }

    // Script files the host loads: *.lua, not starting with '_' (examples, disabled files).
    inline bool Wanted(const std::string& name) {
        return name.size() > 4 && name[0] != '_' && name.ends_with(".lua");
    }

    class Host {
    public:
        struct Rule { std::string file, pattern; Tweak tweak; };
        struct Hook { std::string file, pattern; Event ev; int ref; };
        struct New { std::string file, name, donor, label; Tweak tweak; int ref[3] = {LUA_NOREF, LUA_NOREF, LUA_NOREF}; };

        Host() = default;
        Host(const Host&) = delete;
        Host& operator=(const Host&) = delete;
        ~Host() { Close(); }

        long budget = 2'000'000;       // instructions per top-level run or event call
        size_t memoryCap = 32u << 20;  // bytes for the whole Lua state

        bool Open() const { return L_ != nullptr; }
        const std::vector<Rule>& Rules() const { return rules_; }
        const std::vector<Hook>& Hooks() const { return hooks_; }
        const std::vector<New>& News() const { return news_; }
        const std::vector<Error>& Errors() const { return errors_; }
        const std::vector<std::string>& Log() const { return log_; }
        size_t Memory() const { return mem_; }
        unsigned Version() const { return version_; }  // bumps on every change the menu shows

        // An error found outside Lua (e.g. a class a command names doesn't exist).
        void Report(const std::string& file, std::string msg) { AddError(file, std::move(msg)); }

        // New abilities are active (they take over their donor's casts) unless switched off here.
        void SetActive(const std::string& name, bool on) {
            for (auto it = off_.begin(); it != off_.end(); ++it) if (*it == name) { off_.erase(it); break; }
            if (!on) off_.push_back(name);
            version_++;
        }
        bool Active(const std::string& name) const {
            for (const auto& n : off_) if (n == name) return false;
            return true;
        }

        void Close() {
            if (L_) lua_close(L_);
            L_ = nullptr;
            rules_.clear(); hooks_.clear(); news_.clear();
        }

        // Replaces every script. Files run in the given order, each on its own: one that fails is reported
        // (file:line) and its last good version runs instead, so a typo never drops working mods.
        void Load(const std::vector<std::pair<std::string, std::string>>& files) {
            version_++;
            Close();
            errors_.clear();
            if (!OpenState()) { errors_.push_back({"", "cannot create Lua state"}); return; }
            std::vector<std::pair<std::string, std::string>> good;
            for (const auto& [name, src] : files) {
                std::string err;
                if (RunFile(name, src, err)) { good.emplace_back(name, src); continue; }
                errors_.push_back({name, err});
                for (const auto& [gname, gsrc] : good_)
                    if (gname == name && gsrc != src) {
                        std::string ignored;
                        if (RunFile(name, gsrc, ignored)) { good.emplace_back(gname, gsrc); Note(name + ": running the last good version"); }
                    }
            }
            good_ = std::move(good);
        }

        // Combined tweak for one cast: "*" rules, then named rules (later wins), then an active new ability.
        Tweak TweakFor(const std::string& cls) const {
            Tweak t;
            auto merge = [&](const Tweak& r) {
                if (r.hasRate) { t.animRate = r.animRate; t.hasRate = true; }
                if (r.hasCooldown) { t.cooldown = r.cooldown; t.hasCooldown = true; }
                if (!r.projectile.empty()) t.projectile = r.projectile;
            };
            for (int pass = 0; pass < 2; pass++)
                for (const Rule& r : rules_)
                    if ((r.pattern == "*") == (pass == 0) && Matches(r.pattern, cls)) merge(r.tweak);
            if (const New* n = NewFor(cls)) merge(n->tweak);
            return t;
        }

        // The active new ability built on this donor, if any (the first one declared wins).
        const New* NewFor(const std::string& cls) const {
            for (const New& n : news_) if (Active(n.name) && Matches(n.donor, cls)) return &n;
            return nullptr;
        }

        // Runs every handler for this cast's event; their actions land in out.
        void Fire(const std::string& cls, Event ev, std::vector<Command>& out) {
            if (!L_) return;
            struct Call { int ref; std::string file; };
            std::vector<Call> calls;
            for (const Hook& h : hooks_) if (h.ev == ev && Matches(h.pattern, cls)) calls.push_back({h.ref, h.file});
            if (const New* n = NewFor(cls); n && n->ref[int(ev)] != LUA_NOREF) calls.push_back({n->ref[int(ev)], n->file});
            const std::string label = NewFor(cls) ? NewFor(cls)->name : ShortName(cls);
            out_ = &out;
            for (const Call& c : calls) {
                lua_rawgeti(L_, LUA_REGISTRYINDEX, c.ref);
                lua_createtable(L_, 0, 2);
                lua_pushstring(L_, label.c_str()); lua_setfield(L_, -2, "ability");
                lua_pushstring(L_, kEventNames[int(ev)]); lua_setfield(L_, -2, "event");
                luaL_setmetatable(L_, kCtxMeta);
                used_ = 0;
                if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
                    AddError(c.file, lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "error");
                    lua_pop(L_, 1);
                }
            }
            out_ = nullptr;
        }

    private:
        static constexpr const char* kCtxMeta = "dos.ctx";
        static constexpr int kHookStep = 1000;
        static constexpr size_t kMaxLog = 100, kMaxErrors = 20;

        lua_State* L_ = nullptr;
        size_t mem_ = 0;
        long used_ = 0;
        unsigned version_ = 0;
        std::string curFile_;
        std::vector<Command>* out_ = nullptr;
        std::vector<Rule> rules_;
        std::vector<Hook> hooks_;
        std::vector<New> news_;
        std::vector<Error> errors_;
        std::vector<std::string> log_;
        std::vector<std::pair<std::string, std::string>> good_;  // last source of each file that ran clean
        std::vector<std::string> off_;

        static Host* Of(lua_State* L) { void* ud; lua_getallocf(L, &ud); return static_cast<Host*>(ud); }

        void Note(std::string s) {
            version_++;
            if (log_.size() >= kMaxLog) log_.erase(log_.begin());
            log_.push_back(std::move(s));
        }
        void AddError(const std::string& file, std::string msg) {
            for (const Error& e : errors_) if (e.file == file && e.msg == msg) return;
            version_++;
            if (errors_.size() >= kMaxErrors) errors_.erase(errors_.begin());
            errors_.push_back({file, std::move(msg)});
        }

        static void* Alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
            Host* h = static_cast<Host*>(ud);
            if (!ptr) osize = 0;  // osize is a type tag for new blocks
            if (nsize == 0) { std::free(ptr); h->mem_ -= osize; return nullptr; }
            if (nsize > osize && h->mem_ - osize + nsize > h->memoryCap) return nullptr;  // Lua raises "not enough memory"
            void* p = std::realloc(ptr, nsize);
            if (p) h->mem_ = h->mem_ - osize + nsize;
            return p;
        }

        static void CountHook(lua_State* L, lua_Debug*) {
            Host* h = Of(L);
            h->used_ += kHookStep;
            if (h->used_ > h->budget) luaL_error(L, "instruction limit reached (endless loop?)");
        }

        bool OpenState() {
            mem_ = 0;
            L_ = lua_newstate(&Alloc, this);
            if (!L_) return false;
            static const luaL_Reg libs[] = {
                {LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_STRLIBNAME, luaopen_string},
                {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
            for (const luaL_Reg& l : libs) { luaL_requiref(L_, l.name, l.func, 1); lua_pop(L_, 1); }
            for (const char* g : {"dofile", "loadfile", "load"}) { lua_pushnil(L_); lua_setglobal(L_, g); }
            lua_pushcfunction(L_, &LPrint); lua_setglobal(L_, "print");

            static const luaL_Reg api[] = {{"tweak", &LTweak}, {"on", &LOn}, {"new", &LNew}, {nullptr, nullptr}};
            luaL_newlib(L_, api);
            lua_setglobal(L_, "ability");

            static const luaL_Reg ctx[] = {{"dash", &LDash}, {"apply_effect", &LApplyEffect}, {"play_rate", &LPlayRate},
                                           {"cancel", &LCancel}, {"log", &LLog}, {nullptr, nullptr}};
            luaL_newmetatable(L_, kCtxMeta);
            luaL_newlib(L_, ctx);
            lua_setfield(L_, -2, "__index");
            lua_pop(L_, 1);

            lua_sethook(L_, &CountHook, LUA_MASKCOUNT, kHookStep);
            return true;
        }

        // Runs one file; on failure everything it declared is taken back.
        bool RunFile(const std::string& name, const std::string& src, std::string& err) {
            const size_t nRules = rules_.size(), nHooks = hooks_.size(), nNews = news_.size();
            curFile_ = name;
            used_ = 0;
            const std::string chunk = "@" + name;
            int rc = luaL_loadbufferx(L_, src.data(), src.size(), chunk.c_str(), "t");
            if (rc == LUA_OK) rc = lua_pcall(L_, 0, 0, 0);
            curFile_.clear();
            if (rc == LUA_OK) return true;
            err = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "error";
            lua_pop(L_, 1);
            for (size_t i = nHooks; i < hooks_.size(); i++) luaL_unref(L_, LUA_REGISTRYINDEX, hooks_[i].ref);
            for (size_t i = nNews; i < news_.size(); i++)
                for (int r : news_[i].ref) luaL_unref(L_, LUA_REGISTRYINDEX, r);
            rules_.resize(nRules); hooks_.resize(nHooks); news_.resize(nNews);
            return false;
        }

        // ctx methods work as ctx:f(x) and ctx.f(x).
        static int Arg0(lua_State* L) { return lua_istable(L, 1) ? 2 : 1; }

        static float Number(lua_State* L, int idx, const char* what, float lo, float hi) {
            const float v = static_cast<float>(luaL_checknumber(L, idx));
            if (!(v >= lo && v <= hi)) luaL_error(L, "%s must be %f..%f, got %f", what, lo, hi, v);
            return v;
        }

        // Fills t from key/value at the top of the stack; returns false for keys that aren't tweaks.
        static bool TweakKey(lua_State* L, const char* key, Tweak& t) {
            if (!std::strcmp(key, "anim_rate")) { t.animRate = Number(L, -1, "anim_rate", kMinRate, kMaxRate); t.hasRate = true; return true; }
            if (!std::strcmp(key, "cooldown")) {
                // > 1 would need the effect re-applied longer: not supported yet (#42).
                t.cooldown = Number(L, -1, "cooldown", 0.0f, 1.0f); t.hasCooldown = true; return true;
            }
            if (!std::strcmp(key, "projectile")) {
                if (lua_type(L, -1) != LUA_TSTRING || !*lua_tostring(L, -1)) luaL_error(L, "projectile must be a class name");
                t.projectile = lua_tostring(L, -1); return true;
            }
            return false;
        }

        // ability.tweak(pattern, {anim_rate = x, cooldown = y})
        static Host* Declaring(lua_State* L) {
            Host* h = Of(L);
            if (h->curFile_.empty()) luaL_error(L, "declare abilities at the top level of a file, not inside a handler");
            return h;
        }

        static int LTweak(lua_State* L) {
            Host* h = Declaring(L);
            Rule r{h->curFile_, luaL_checkstring(L, 1), {}};
            luaL_checktype(L, 2, LUA_TTABLE);
            lua_pushnil(L);
            while (lua_next(L, 2)) {
                const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "?";
                if (!TweakKey(L, key, r.tweak)) luaL_error(L, "unknown tweak '%s' (anim_rate, cooldown, projectile)", key);
                lua_pop(L, 1);
            }
            h->rules_.push_back(std::move(r));
            return 0;
        }

        static int CheckEvent(lua_State* L, int idx) {
            const char* name = luaL_checkstring(L, idx);
            for (int i = 0; i < 3; i++) if (!std::strcmp(name, kEventNames[i])) return i;
            return luaL_error(L, "unknown event '%s' (activate, out, end)", name);
        }

        // ability.on(pattern, event, function(ctx) ... end)
        static int LOn(lua_State* L) {
            Host* h = Declaring(L);
            std::string pattern = luaL_checkstring(L, 1);
            const int ev = CheckEvent(L, 2);
            luaL_checktype(L, 3, LUA_TFUNCTION);
            lua_pushvalue(L, 3);
            h->hooks_.push_back({h->curFile_, std::move(pattern), Event(ev), luaL_ref(L, LUA_REGISTRYINDEX)});
            return 0;
        }

        // ability.new{name =, donor =, label =, anim_rate =, cooldown =, on_activate =, on_out =, on_end =}
        static int LNew(lua_State* L) {
            Host* h = Declaring(L);
            luaL_checktype(L, 1, LUA_TTABLE);
            New n{h->curFile_};
            int fn[3] = {0, 0, 0};  // stack slots of the handlers
            lua_settop(L, 1);
            lua_pushnil(L);
            while (lua_next(L, 1)) {
                const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "?";
                if (!std::strcmp(key, "name")) n.name = luaL_checkstring(L, -1);
                else if (!std::strcmp(key, "donor")) n.donor = luaL_checkstring(L, -1);
                else if (!std::strcmp(key, "label")) n.label = luaL_checkstring(L, -1);
                else if (TweakKey(L, key, n.tweak)) {}
                else {
                    int ev = -1;
                    for (int i = 0; i < 3; i++) if (std::string("on_") + kEventNames[i] == key) ev = i;
                    if (ev < 0) luaL_error(L, "unknown field '%s' in ability.new", key);
                    luaL_checktype(L, -1, LUA_TFUNCTION);
                    lua_pushvalue(L, -1);
                    lua_insert(L, 2);  // keep below the iteration key; later slots shift up by one
                    for (int& s : fn) if (s) s++;
                    fn[ev] = 2;
                }
                lua_pop(L, 1);
            }
            if (n.name.empty() || n.donor.empty()) luaL_error(L, "ability.new needs name and donor");
            for (const New& o : h->news_) if (o.name == n.name) luaL_error(L, "ability '%s' already exists (%s)", n.name.c_str(), o.file.c_str());
            if (n.label.empty()) n.label = n.name;
            for (int i = 0; i < 3; i++) if (fn[i]) { lua_pushvalue(L, fn[i]); n.ref[i] = luaL_ref(L, LUA_REGISTRYINDEX); }
            h->news_.push_back(std::move(n));
            return 0;
        }

        static void Emit(lua_State* L, Command c) {
            Host* h = Of(L);
            if (!h->out_) luaL_error(L, "ctx actions work only inside an event handler");
            h->out_->push_back(std::move(c));
        }
        static int LDash(lua_State* L) { Emit(L, {Command::Kind::Dash, Number(L, Arg0(L), "dash speed", 0, kMaxDash)}); return 0; }
        static int LPlayRate(lua_State* L) { Emit(L, {Command::Kind::PlayRate, Number(L, Arg0(L), "play_rate", kMinRate, kMaxRate)}); return 0; }
        static int LCancel(lua_State* L) { Emit(L, {Command::Kind::Cancel}); return 0; }
        static int LApplyEffect(lua_State* L) {
            const char* cls = luaL_checkstring(L, Arg0(L));
            if (!*cls) luaL_error(L, "apply_effect needs a GameplayEffect class name");
            Emit(L, {Command::Kind::ApplyEffect, 0, cls});
            return 0;
        }

        static std::string Join(lua_State* L, int from) {
            std::string s;
            for (int i = from, n = lua_gettop(L); i <= n; i++) {
                if (i > from) s += ' ';
                s += luaL_tolstring(L, i, nullptr);
                lua_pop(L, 1);
            }
            return s;
        }
        static int LLog(lua_State* L) { Of(L)->Note(Join(L, Arg0(L))); return 0; }
        static int LPrint(lua_State* L) { Of(L)->Note(Join(L, 1)); return 0; }
    };

    // Directory change detection: the caller lists (name, mtime, size) of every file; returns true when the
    // wanted set differs from the last call's.
    struct Stamp { std::string name; unsigned long long mtime, size; };
    inline bool Changed(const std::vector<Stamp>& files, std::string& memo) {
        std::string now;
        for (const Stamp& f : files)
            if (Wanted(f.name)) now += f.name + '|' + std::to_string(f.mtime) + '|' + std::to_string(f.size) + '\n';
        if (now == memo) return false;
        memo = std::move(now);
        return true;
    }
}
