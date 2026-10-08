#include "pickup-toast.hpp"
#include "../shared/items.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "umg.hpp"
#include "ref.hpp"
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
#include "BP_ArchonClientFunctionLibrary_classes.hpp"
#include "BP_ArchonClientFunctionLibrary_parameters.hpp"

// Pickup toast (#64): each newly owned item pops up as the game's own loot-toast row (WidgetLootToastEntry_C: the
// item's atlas icon, quality border and name, coloured by the game for its grade), stacked right of the character.
// Trigger: the controller's OnItemAddedDispatcherEvent (also fired per slot on every reorder/sort) marks the owned
// set dirty (O(1)); the next world tick diffs owned counts (inventory + equipped + bank) and toasts only what rose.
// Spec: references/design-system.md § Pickup toast. Facts: references/game-ui.md § Loot toast.
using namespace SDK;
using umg::CallNative;
using umg::PtrOk;
using namespace pickup_toast;
namespace io = items::io;

namespace {
    // Game thread. Blueprint classes come and go with the map (#63): ref::Fn / ref::Cached re-resolve them.
    struct Fns {
        ref::Fn create{UWidgetBlueprintLibrary::StaticClass, "WidgetBlueprintLibrary", "Create"};
        ref::Fn toViewport{UUserWidget::StaticClass, "UserWidget", "AddToViewport"};
        ref::Fn anchors{UUserWidget::StaticClass, "UserWidget", "SetAnchorsInViewport"};
        ref::Fn align{UUserWidget::StaticClass, "UserWidget", "SetAlignmentInViewport"};
        ref::Fn gradeColor{UBP_ArchonClientFunctionLibrary_C::StaticClass, "BP_ArchonClientFunctionLibrary_C", "GetItemColorForGrade"};
        ref::Fn textColor{UTextBlock::StaticClass, "TextBlock", "SetColorAndOpacity"};
        ref::Fn imageColor{UImage::StaticClass, "Image", "SetColorAndOpacity"};
        ref::Fn translate{UWidget::StaticClass, "Widget", "SetRenderTranslation"};
        ref::Fn opacity{UWidget::StaticClass, "Widget", "SetRenderOpacity"};
        ref::Fn remove{UWidget::StaticClass, "Widget", "RemoveFromParent"};
        ref::Fn listSet{UWidgetLootToastEntry_C::StaticClass, "WidgetLootToastEntry_C", "OnListItemObjectSet"};
        ref::Fn itemAdded{ABP_PlayerControllerGame_C::StaticClass, "BP_PlayerControllerGame_C", "OnItemAddedDispatcherEvent"};
        bool Ok() {
            for (ref::Fn* f : {&create, &toViewport, &anchors, &align, &gradeColor, &textColor, &imageColor, &translate, &opacity, &remove, &listSet, &itemAdded})
                if (!f->Get()) return false;
            return true;
        }
    } g_fn;
    ref::Cached<UClass> g_entryCls{[] { return UWidgetLootToastEntry_C::StaticClass(); }};
    ref::Cached<UClass> g_objCls{[] { return UWidgetLootToastEntryObject_C::StaticClass(); }};

    struct Toast {
        ref::Ref w;  // UUserWidget
        double born, end;      // seconds (QPC); end = fade-out done
        int row; float rowFrom; double rowAt;
        float x = -1, y = -1, o = -1;  // last values sent to the widget
    };
    std::vector<Toast> g_toasts;
    Owned g_owned;
    bool g_baseline = false, g_bankSeen = false, g_dirty = false;
    std::atomic<bool> g_on{false}, g_drain{false};  // g_drain: Off() asks the game thread to remove every toast
    thread_local bool t_busy = false;

    double Now() {
        LARGE_INTEGER t, f;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&f);
        return double(t.QuadPart) / double(f.QuadPart);
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
        c.WidgetType = g_entryCls.Get();
        c.OwningPlayer = pc;
        CallNative(UWidgetBlueprintLibrary::GetDefaultObj(), g_fn.create.Get(), &c);
        UUserWidget* w = c.ReturnValue;
        if (!PtrOk(w)) return;
        auto* data = static_cast<UWidgetLootToastEntryObject_C*>(umg::Spawn(g_objCls.Get(), w));
        if (!data) return;
        data->DisplayName = umg::Text(it.name);  // takes over the reference umg::Text leaves unowned: one owner, no copy
        data->IconId = io::IconId(it.specId);
        data->Grade = uint8(it.grade);

        Params::UserWidget_AddToViewport v{10};
        CallNative(w, g_fn.toViewport.Get(), &v);
        Params::UserWidget_SetAnchorsInViewport a{};
        a.Anchors = {{kAnchorX, kAnchorY}, {kAnchorX, kAnchorY}};
        CallNative(w, g_fn.anchors.Get(), &a);
        Params::UserWidget_SetAlignmentInViewport al{{0.f, 1.f}};  // bottom-left corner on the anchor
        CallNative(w, g_fn.align.Get(), &al);
        // No SetPositionInViewport: it resets the anchors to the top-left corner.
        Params::Widget_SetRenderOpacity o{0.f};
        CallNative(w, g_fn.opacity.Get(), &o);
        Params::WidgetLootToastEntry_C_OnListItemObjectSet s{data};  // the game's own fill: icon and name (both left white)
        if (UFunction* fn = g_fn.listSet.Get()) w->ProcessEvent(fn, &s);
        // Rarity: the game's own grade colour (the one its chat "Received" lines and item names use) on name and quality border.
        Params::BP_ArchonClientFunctionLibrary_C_GetItemColorForGrade g{};
        g.ItemGrade = EItemGrade(it.grade);
        g.__WorldContext = pc;
        UBP_ArchonClientFunctionLibrary_C::GetDefaultObj()->ProcessEvent(g_fn.gradeColor.Get(), &g);
        auto* row = static_cast<UWidgetLootToastEntry_C*>(w);
        if (PtrOk(row->TextBlock_ItemName)) {
            Params::TextBlock_SetColorAndOpacity tc{};
            tc.InColorAndOpacity.SpecifiedColor = g.ItemColor;  // ColorUseRule 0 = specified
            CallNative(row->TextBlock_ItemName, g_fn.textColor.Get(), &tc);
        }
        if (PtrOk(row->Image_ContentBorder)) {
            Params::Image_SetColorAndOpacity ic{g.ItemColor};
            CallNative(row->Image_ContentBorder, g_fn.imageColor.Get(), &ic);
        }

        for (Toast& t : g_toasts) {  // older rows move up one
            t.rowFrom = -RowY(t.rowFrom, float(t.row), float(now - t.rowAt)) / kRowH;
            t.row++;
            t.rowAt = now;
            if (t.row >= kMaxRows) t.end = std::min(t.end, std::max(now, t.born + kIn.dur) + kOut.dur);
        }
        g_toasts.push_back({ref::Ref(w), now, now + kHold + kOut.dur, 0, 0.f, now});
    }

    void Animate(double now) {  // O(toasts on screen)
        std::erase_if(g_toasts, [&](Toast& t) {
            auto* w = t.w.Get<UUserWidget>();
            if (!w) return true;  // the game cleared the viewport (travel)
            if (now >= t.end) { CallNative(w, g_fn.remove.Get(), nullptr); return true; }
            const Pose pose = PoseAt(float(now - t.born), float(t.end - t.born));
            const float y = RowY(t.rowFrom, float(t.row), float(now - t.rowAt));
            if (pose.x != t.x || y != t.y) {
                Params::Widget_SetRenderTranslation tr{{pose.x, y}};
                CallNative(w, g_fn.translate.Get(), &tr);
                t.x = pose.x, t.y = y;
            }
            if (pose.opacity != t.o) {
                Params::Widget_SetRenderOpacity o{pose.opacity};
                CallNative(w, g_fn.opacity.Get(), &o);
                t.o = pose.opacity;
            }
            return false;
        });
    }

    void RemoveAll() {  // game thread (or Off() after its wait ran out)
        for (Toast& t : g_toasts)
            if (auto* w = t.w.Get<UUserWidget>()) CallNative(w, g_fn.remove.Get(), nullptr);
        g_toasts.clear();
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
                    logger::log("[pickup-toast] " + it.name + " (grade " + std::to_string(it.grade) + ", lv " + std::to_string(it.level) +
                                ", icon " + std::to_string(io::IconId(it.specId)) + ", diff " + std::to_string((Now() - now) * 1000).substr(0, 5) + " ms)");
                    break;
                }
    }

    void OnEvent(void*, void* fnp, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed)) return;
        if (g_fn.itemAdded.Is(fnp)) { g_dirty = true; return; }  // per slot, also on every reorder: O(1)
        if (!umg::IsWorldTick(fnp) || !game::OnGameThread()) return;  // widgets change on the world tick only (#50)
        t_busy = true;
        if (g_drain.exchange(false)) RemoveAll();
        if (g_fn.Ok() && g_entryCls.Get() && g_objCls.Get()) {  // O(1) each; Blueprint classes load with a world (#54)
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
        PickupToast() : Feature("Pickup toast", feature::Stage::Beta) {}
        void OnFrame(const feature::Frame&) override {
            io::Tick();
            if (g_on.load()) return;
            g_on = true;
            game::SetEventListener(&OnEvent, true);
        }
        // Off() runs with the ProcessEvent hook alive (overlay::Shutdown, #84): the next world tick removes the widgets.
        // No tick within 500 ms (game thread blocked): removed here instead, once.
        void Off() override {
            g_drain = !g_toasts.empty();  // nothing on screen: nothing to wait for
            for (int i = 0; i < 250 && g_drain.load(); i++) Sleep(2);
            if (g_drain.exchange(false)) {
                RemoveAll();
                logger::log("[pickup-toast] toasts removed off the game thread (no world tick within 500 ms)");
            }
            g_on = false;
            game::SetEventListener(&OnEvent, false);
            g_toasts.clear();
            g_owned.clear();
            g_baseline = g_dirty = false;
        }
    } g_feature;
}
