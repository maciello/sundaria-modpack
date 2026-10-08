#include "pickup-toast.hpp"
#include "../shared/items.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "umg.hpp"
#include "cost.hpp"

#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>
#include "UMG_classes.hpp"
#include "UMG_parameters.hpp"
#include "BP_PlayerControllerGame_classes.hpp"
#include "WidgetLootToastEntry_classes.hpp"
#include "WidgetLootToastEntry_parameters.hpp"
#include "WidgetLootToastEntryObject_classes.hpp"

// Pickup toast (#64): each newly owned item pops up as the game's own loot-toast row (WidgetLootToastEntry_C: the
// item's atlas icon, quality border and name, coloured by the game for its grade), stacked right of the character.
// Trigger: the controller's OnItemAddedDispatcherEvent (also fired per slot on every reorder/sort) marks the owned
// set dirty (O(1)); the next world tick diffs owned counts (inventory + equipped + bank) and toasts only what rose.
// Spec: references/design-system.md § Pickup toast. Facts: references/game-ui.md § Loot toast.
using namespace SDK;
using umg::Alive;
using umg::CallNative;
using umg::PtrOk;
using namespace pickup_toast;
namespace io = items::io;

namespace {
    struct Fn {
        UFunction* f = nullptr;
        int32 idx = -1;
        void Set(UClass* c, const char* cls, const char* name) {
            f = PtrOk(c) ? c->GetFunction(cls, name) : nullptr;
            idx = f ? f->Index : -1;
        }
        bool Ok() const { return Alive(f, idx); }
    };
    struct Fns {
        Fn create, toViewport, anchors, align, position, translate, opacity, remove, listSet, itemAdded;
        bool Ok() const {
            for (const Fn* f : {&create, &toViewport, &anchors, &align, &position, &translate, &opacity, &remove, &listSet, &itemAdded})
                if (!f->Ok()) return false;
            return true;
        }
    } g_fn;
    struct ClassRef { UClass* c = nullptr; int32 idx = -1; } g_entryCls, g_objCls;

    struct Toast {
        UUserWidget* w; int32 idx;
        double born, end;      // seconds (QPC); end = fade-out done
        int row; float rowFrom; double rowAt;
        float x = -1, y = -1, o = -1;  // last values sent to the widget
    };
    std::vector<Toast> g_toasts;
    Owned g_owned;
    bool g_baseline = false, g_bankSeen = false, g_dirty = false;
    std::atomic<bool> g_on{false}, g_resolved{false};  // resolved: render thread re-resolves (≤ 1/s) when false
    thread_local bool t_busy = false;

    double Now() {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        return double(t.QuadPart) / double(f.QuadPart);
    }

    bool Resolve() {
        UClass* entry = UWidgetLootToastEntry_C::StaticClass();
        UClass* obj = UWidgetLootToastEntryObject_C::StaticClass();
        UClass* pc = ABP_PlayerControllerGame_C::StaticClass();
        if (!PtrOk(entry) || !PtrOk(obj) || !PtrOk(pc)) return false;  // Blueprint classes load with a world (#54)
        g_entryCls = {entry, entry->Index};
        g_objCls = {obj, obj->Index};
        g_fn.create.Set(UWidgetBlueprintLibrary::StaticClass(), "WidgetBlueprintLibrary", "Create");
        g_fn.toViewport.Set(UUserWidget::StaticClass(), "UserWidget", "AddToViewport");
        g_fn.anchors.Set(UUserWidget::StaticClass(), "UserWidget", "SetAnchorsInViewport");
        g_fn.align.Set(UUserWidget::StaticClass(), "UserWidget", "SetAlignmentInViewport");
        g_fn.position.Set(UUserWidget::StaticClass(), "UserWidget", "SetPositionInViewport");
        g_fn.translate.Set(UWidget::StaticClass(), "Widget", "SetRenderTranslation");
        g_fn.opacity.Set(UWidget::StaticClass(), "Widget", "SetRenderOpacity");
        g_fn.remove.Set(UWidget::StaticClass(), "Widget", "RemoveFromParent");
        g_fn.listSet.Set(entry, "WidgetLootToastEntry_C", "OnListItemObjectSet");
        g_fn.itemAdded.Set(pc, "BP_PlayerControllerGame_C", "OnItemAddedDispatcherEvent");
        return g_fn.Ok();
    }

    // Owned counts now. Empty bag = still loading: no answer.
    bool ReadOwned(Owned& out, std::vector<items::Item>& items, bool& bankSeen) {
        const io::Located l = io::Locate();
        if (!l.bag) return false;
        for (items::Item& it : io::Read(l.bag, false, false))
            if (it.where == items::Where::Inventory || it.where == items::Where::Equipped) items.push_back(std::move(it));
        if (items.empty()) return false;
        const io::Bank bank = io::ReadBank(false);
        bankSeen = bank.seen;
        items.insert(items.end(), bank.items.begin(), bank.items.end());
        for (const items::Item& it : items) out[{it.specId, it.level, it.grade}]++;
        return true;
    }

    void Show(const items::Item& it, double now) {
        APlayerController* pc = umg::LocalPC();
        if (!pc) return;
        Params::WidgetBlueprintLibrary_Create c{};
        c.WorldContextObject = pc;
        c.WidgetType = g_entryCls.c;
        c.OwningPlayer = pc;
        CallNative(UWidgetBlueprintLibrary::GetDefaultObj(), g_fn.create.f, &c);
        UUserWidget* w = c.ReturnValue;
        if (!PtrOk(w)) return;
        auto* data = static_cast<UWidgetLootToastEntryObject_C*>(umg::Spawn(g_objCls.c, w));
        if (!data) return;
        data->DisplayName = umg::Text(it.name);  // takes over the reference umg::Text leaves unowned: one owner, no copy
        data->IconId = io::IconId(it.specId);
        data->Grade = uint8(it.grade);

        Params::UserWidget_AddToViewport v{10};
        CallNative(w, g_fn.toViewport.f, &v);
        Params::UserWidget_SetAnchorsInViewport a{};
        a.Anchors = {{kAnchorX, kAnchorY}, {kAnchorX, kAnchorY}};
        CallNative(w, g_fn.anchors.f, &a);
        Params::UserWidget_SetAlignmentInViewport al{{0.f, 1.f}};  // bottom-left corner on the anchor
        CallNative(w, g_fn.align.f, &al);
        Params::UserWidget_SetPositionInViewport p{};
        p.Position = {0.f, 0.f};
        CallNative(w, g_fn.position.f, &p);
        Params::Widget_SetRenderOpacity o{0.f};
        CallNative(w, g_fn.opacity.f, &o);
        Params::WidgetLootToastEntry_C_OnListItemObjectSet s{data};  // the game's own fill: icon, border, name colour
        w->ProcessEvent(g_fn.listSet.f, &s);

        for (Toast& t : g_toasts) {  // older rows move up one
            t.rowFrom = -RowY(t.rowFrom, float(t.row), float(now - t.rowAt)) / kRowH;
            t.row++;
            t.rowAt = now;
            if (t.row >= kMaxRows) t.end = std::min(t.end, std::max(now, t.born + kIn.dur) + kOut.dur);
        }
        g_toasts.push_back({w, w->Index, now, now + kHold + kOut.dur, 0, 0.f, now});
    }

    void Animate(double now) {  // O(toasts on screen)
        std::erase_if(g_toasts, [&](Toast& t) {
            if (!Alive(t.w, t.idx)) return true;  // the game cleared the viewport (travel)
            if (now >= t.end) { CallNative(t.w, g_fn.remove.f, nullptr); return true; }
            const Pose pose = PoseAt(float(now - t.born), float(t.end - t.born));
            const float y = RowY(t.rowFrom, float(t.row), float(now - t.rowAt));
            if (pose.x != t.x || y != t.y) {
                Params::Widget_SetRenderTranslation tr{{pose.x, y}};
                CallNative(t.w, g_fn.translate.f, &tr);
                t.x = pose.x, t.y = y;
            }
            if (pose.opacity != t.o) {
                Params::Widget_SetRenderOpacity o{pose.opacity};
                CallNative(t.w, g_fn.opacity.f, &o);
                t.o = pose.opacity;
            }
            return false;
        });
    }

    // ponytail: a pickup that only grows an existing stack adds no record, so it shows no toast; stack counts are not read
    void Update(double now) {  // once per batch of item events: O(owned items)
        static cost::Path path{"pickup-toast owned diff"};
        cost::Scope cs(path);
        Owned now_;
        std::vector<items::Item> items;
        bool bankSeen = false;
        if (!ReadOwned(now_, items, bankSeen)) return;
        const std::vector<Key> gained = Gained(g_owned, now_);
        const bool resync = !g_baseline || bankSeen != g_bankSeen || int(gained.size()) > 2 * kMaxRows;  // loading, bank first read
        if (resync && g_baseline) logger::log("[pickup-toast] owned set re-read without toasts (" + std::to_string(gained.size()) + " new)");
        g_owned = std::move(now_);
        g_baseline = true;
        g_bankSeen = bankSeen;
        if (resync) return;
        for (const Key& k : gained)
            for (const items::Item& it : items)
                if (it.specId == k.spec && it.level == k.level && it.grade == k.grade) {
                    Show(it, now);
                    logger::log("[pickup-toast] " + it.name + " (grade " + std::to_string(it.grade) + ", lv " + std::to_string(it.level) + ")");
                    break;
                }
    }

    void OnEvent(void*, void* fnp, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed)) return;
        if (fnp == g_fn.itemAdded.f) { g_dirty = true; return; }  // per slot, also on every reorder: O(1)
        if (!umg::IsWorldTick(fnp) || !game::OnGameThread()) return;  // widgets change on the world tick only (#50)
        t_busy = true;
        if (!g_fn.Ok() || !Alive(g_entryCls.c, g_entryCls.idx) || !Alive(g_objCls.c, g_objCls.idx)) {
            g_on = g_resolved = false;  // a Blueprint class was unloaded: OnFrame resolves again
        } else {
            const double now = Now();
            if ((g_dirty || !g_baseline) && io::Ready()) {
                g_dirty = false;
                Update(now);
            }
            if (!g_toasts.empty()) {
                static cost::Path path{"pickup-toast animate"};
                cost::Scope cs(path);
                Animate(now);
            }
        }
        t_busy = false;
    }

    struct PickupToast : feature::Feature {
        ULONGLONG next = 0;
        PickupToast() : Feature("Pickup toast", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook
        void OnFrame(const feature::Frame&) override {
            io::Tick();
            if (g_on.load()) return;
            if (!g_resolved && GetTickCount64() >= next) {  // missing Blueprint class: StaticClass searches, so ≤ 1/s
                next = GetTickCount64() + 1000;
                g_resolved = Resolve();
                if (g_resolved) logger::log("[pickup-toast] ready");
            }
            if (!g_resolved) return;
            g_on = true;
            game::SetEventListener(&OnEvent, true);
        }
        // ponytail: a toast on screen at this moment stays (frozen) until the game clears the viewport; removal needs the game thread
        void Off() override {
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            g_toasts.clear();
            g_owned.clear();
            g_baseline = g_dirty = false;
            g_resolved = false;
        }
    } g_feature;
}
