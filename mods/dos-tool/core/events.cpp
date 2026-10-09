// game::On / OnWorldTick / OnClass / OnGameTick / OnEvery: subscriptions -> an immutable events::Table the ProcessEvent
// hook (core/game.cpp) reads without a lock. A change publishes a new table; the old one is freed once no hook call
// can still hold it (DrainHook).
#include "events.hpp"
#include "game.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <atomic>
#include <cstring>
#include "CoreUObject_classes.hpp"

using namespace SDK;

namespace {
    SRWLOCK g_mu = SRWLOCK_INIT;
    std::vector<events::Sub> g_subs;              // g_mu
    std::vector<const events::Table*> g_retired;  // g_mu: replaced tables a hook call may still read
    std::atomic<const events::Table*> g_table{nullptr};
    thread_local bool t_resolving = false;
    ULONGLONG g_gameTickAt = 0;  // game thread

    uint64_t Key(const FName& n) {  // {ComparisonIndex, Number}: "X_1" and "X_2" differ
        uint64_t k;
        std::memcpy(&k, &n, sizeof k);
        return k;
    }
    uint64_t Key(const std::string& s) {
        const std::wstring w(s.begin(), s.end());  // UFunction and class names are ASCII
        return Key(BasicFilesImplUtils::StringToName(w.c_str()));
    }

    void Publish() {  // g_mu held
        const events::Table* t = g_subs.empty() ? nullptr : new events::Table(events::Build(g_subs));
        if (const events::Table* old = g_table.exchange(t)) g_retired.push_back(old);
    }

    // Game thread, inside the hook. Making an FName is a ProcessEvent call (KismetStringLibrary): it re-enters the hook.
    // An FName exists for any string, loaded class or not, so each name resolves once.
    void Resolve() {
        if (t_resolving || !TryAcquireSRWLockExclusive(&g_mu)) return;
        t_resolving = true;
        for (events::Sub& s : g_subs)
            if (!s.resolved) {
                s.fnKey = s.fn.empty() ? 0 : Key(s.fn);
                s.clsKey = s.cls.empty() ? 0 : Key(s.cls);
                s.resolved = true;
            }
        Publish();
        t_resolving = false;
        ReleaseSRWLockExclusive(&g_mu);
    }

    void Set(events::Kind kind, const char* cls, const char* fn, events::Cb cb, bool on) {
        AcquireSRWLockExclusive(&g_mu);
        const bool changed = events::Set(g_subs, {kind, cls ? cls : "", fn ? fn : "", cb}, on);
        if (changed) Publish();
        std::vector<const events::Table*> old;
        if (changed && !on) old.swap(g_retired);
        ReleaseSRWLockExclusive(&g_mu);
        if (!changed) return;
        if (!on) {
            if (events::DrainHook()) {  // no call is inside cb any more, nor reading a table retired before now
                for (const events::Table* t : old) delete t;
            } else {
                AcquireSRWLockExclusive(&g_mu);
                g_retired.insert(g_retired.end(), old.begin(), old.end());
                ReleaseSRWLockExclusive(&g_mu);
            }
        }
        events::UpdateHook();
    }
}

bool events::Live() { return g_table.load() != nullptr; }

int events::Dispatch(const void* objc, void* fnp, void* parms) {
    const Table* t = g_table.load(std::memory_order_acquire);
    if (!t) return 0;
    void* obj = const_cast<void*>(objc);
    const auto call = [&](Cb cb) { cb(obj, fnp, parms); };
    int n = 0;
    for (Cb cb : t->every) call(cb), n++;
    const auto* fn = static_cast<const UFunction*>(fnp);
    if (!t->byFn.empty() && umg::PtrOk(fn))
        n += ForFn(*t, Key(fn->Name), umg::PtrOk(fn->Outer) ? Key(fn->Outer->Name) : 0, call);
    const auto* o = static_cast<const UObject*>(objc);
    if (!t->byClass.empty() && umg::PtrOk(o) && umg::PtrOk(o->Class)) n += ForClass(*t, Key(o->Class->Name), call);
    if ((t->unresolved || !t->gameTick.empty()) && game::OnGameThread()) {
        if (t->unresolved) Resolve();  // t stays readable: retired, freed only after this call leaves the hook
        const ULONGLONG now = GetTickCount64();
        if (!t->gameTick.empty() && now - g_gameTickAt >= 8) {
            g_gameTickAt = now;
            for (Cb cb : t->gameTick) cb(nullptr, nullptr, nullptr), n++;
        }
    }
    return n;
}

void game::On(const char* cls, const char* fn, EventListener cb, bool on) { Set(events::Kind::Fn, cls, fn, cb, on); }
void game::OnClass(const char* cls, EventListener cb, bool on) { Set(events::Kind::Class, cls, nullptr, cb, on); }
void game::OnGameTick(EventListener cb, bool on) { Set(events::Kind::GameTick, nullptr, nullptr, cb, on); }
void game::OnEvery(EventListener cb, bool on) { Set(events::Kind::Every, nullptr, nullptr, cb, on); }
void game::OnWorldTick(EventListener cb, bool on) {  // the two classes umg::IsWorldTick knows
    On("BP_PlayerControllerGame_C", "ReceiveTick", cb, on);
    On("BP_PlayerControllerOnline_C", "ReceiveTick", cb, on);
}
