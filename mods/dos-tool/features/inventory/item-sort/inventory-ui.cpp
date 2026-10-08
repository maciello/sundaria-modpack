#include "inventory-ui.hpp"
#include "item-sort.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "umg.hpp"
#include "ref.hpp"
#include "drain.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "Engine_classes.hpp"
#include "Engine_parameters.hpp"
#include "UMG_classes.hpp"
#include "UMG_parameters.hpp"
#include "WidgetButton01_classes.hpp"
#include "WidgetButton01_parameters.hpp"
#include "WidgetitemBagHeaderMenu_classes.hpp"
#include "WidgetItemInventory_classes.hpp"
#include "WidgetItemStorage_classes.hpp"

// Item-sort controls inside the game's own inventory and bank: a game button (WidgetButton01_C, the game's
// Button05 art, styled exactly like the header's Sort) placed right after Sort in the bag header row.
// Click = next sort profile + re-sort that bag. Spec: references/design-system.md § Inventory sort profile.
// Game thread only (ProcessEvent listener). Facts: references/game-ui.md.
using namespace SDK;
using umg::CallNative;
using umg::PtrOk;

namespace {

    // Game thread. Blueprint classes come and go with the map (#63): ref::Fn / ref::Cached re-resolve them.
    struct Fns {
        ref::Fn create{UWidgetBlueprintLibrary::StaticClass, "WidgetBlueprintLibrary", "Create"};
        ref::Fn addChild{UPanelWidget::StaticClass, "PanelWidget", "AddChild"};
        ref::Fn remove{UWidget::StaticClass, "Widget", "RemoveFromParent"};
        ref::Fn removeChild{UPanelWidget::StaticClass, "PanelWidget", "RemoveChild"};
        ref::Fn setSize{UHorizontalBoxSlot::StaticClass, "HorizontalBoxSlot", "SetSize"};
        ref::Fn setPad{UHorizontalBoxSlot::StaticClass, "HorizontalBoxSlot", "SetPadding"};
        ref::Fn setH{UHorizontalBoxSlot::StaticClass, "HorizontalBoxSlot", "SetHorizontalAlignment"};
        ref::Fn setV{UHorizontalBoxSlot::StaticClass, "HorizontalBoxSlot", "SetVerticalAlignment"};
        ref::Fn setStyle{UButton::StaticClass, "Button", "SetStyle"};
        ref::Fn setText{UWidgetButton01_C::StaticClass, "WidgetButton01_C", "SetButtonText"};
        ref::Fn clicked{UWidgetButton01_C::StaticClass, "WidgetButton01_C",
                        "BndEvt__WidgetButton01_Button_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature"};
    } g_fn;
    struct Placed { ref::Ref header, button; };  // UWidgetitemBagHeaderMenu_C, UWidgetButton01_C
    std::vector<Placed> g_placed;
    game::Drain g_drain;  // Off(): the game thread removes our buttons
    std::atomic<bool> g_on{false};
    thread_local bool t_busy = false;
    int g_shown = -1;  // profile index the labels show
    ref::Cached<UClass> g_headerCls{[] { return UWidgetitemBagHeaderMenu_C::StaticClass(); }};
    ref::Cached<UClass> g_invCls{[] { return UWidgetItemInventory_C::StaticClass(); }};
    ref::Cached<UClass> g_storCls{[] { return UWidgetItemStorage_C::StaticClass(); }};

    void SetLabel(UWidgetButton01_C* b, const std::string& s) {  // labels change only on click
        Params::WidgetButton01_C_SetButtonText p{};
        p.Text_0 = umg::Text(s);
        if (UFunction* fn = g_fn.setText.Get()) b->ProcessEvent(fn, &p);
    }
    std::string Label(int i) {
        const std::vector<std::string> names = item_sort::api::ProfileNames();
        return "Profile: " + (i >= 0 && i < int(names.size()) ? names[i] : std::string("-"));
    }

    void SlotLike(UHorizontalBoxSlot* to, const UHorizontalBoxSlot& from) {
        Params::HorizontalBoxSlot_SetSize sz{from.Size};
        CallNative(to, g_fn.setSize.Get(), &sz);
        Params::HorizontalBoxSlot_SetPadding pd{from.Padding};
        CallNative(to, g_fn.setPad.Get(), &pd);
        Params::HorizontalBoxSlot_SetHorizontalAlignment h{from.HorizontalAlignment};
        CallNative(to, g_fn.setH.Get(), &h);
        Params::HorizontalBoxSlot_SetVerticalAlignment v{from.VerticalAlignment};
        CallNative(to, g_fn.setV.Get(), &v);
    }
    UHorizontalBoxSlot* Add(UPanelWidget* row, UWidget* w) {
        Params::PanelWidget_AddChild a{};
        a.Content = w;
        CallNative(row, g_fn.addChild.Get(), &a);
        return PtrOk(a.ReturnValue) && a.ReturnValue->IsA(UHorizontalBoxSlot::StaticClass()) ? static_cast<UHorizontalBoxSlot*>(a.ReturnValue) : nullptr;
    }

    // Our button already in this header (also one left by a previous DLL before a hot reload)?
    UWidgetButton01_C* Existing(UPanelWidget* row) {
        for (int i = 0; i < row->Slots.Num(); i++) {
            UPanelSlot* s = row->Slots[i];
            if (PtrOk(s) && PtrOk(s->Content) && s->Content->IsA(UWidgetButton01_C::StaticClass())) return static_cast<UWidgetButton01_C*>(s->Content);
        }
        return nullptr;
    }

    // New button right after Sort: same style and slot as Sort; the siblings after Sort are re-added behind it.
    UWidgetButton01_C* Place(UWidgetitemBagHeaderMenu_C* h, UPanelWidget* row, APlayerController* pc) {
        Params::WidgetBlueprintLibrary_Create c{};
        c.WorldContextObject = h;
        c.WidgetType = UWidgetButton01_C::StaticClass();
        c.OwningPlayer = pc;
        CallNative(UWidgetBlueprintLibrary::GetDefaultObj(), g_fn.create.Get(), &c);
        auto* b = static_cast<UWidgetButton01_C*>(c.ReturnValue);
        if (!PtrOk(b) || !PtrOk(b->Button) || !PtrOk(b->Text)) return nullptr;
        UButton* sort = h->Button_Sort;
        // Sort's brushes, paddings, sounds. Through the game's setter, never `WidgetStyle = …`: each FSlateBrush holds a
        // TSharedPtr (resource handle) that a C++ byte copy duplicates without a reference, so the second widget to die
        // frees it again and corrupts the heap (crash in GC on the next map change, #61).
        Params::Button_SetStyle st{};
        st.InStyle = sort->WidgetStyle;  // byte copy: takes no reference, releases none (never destroyed)
        CallNative(b->Button, g_fn.setStyle.Get(), &st);  // the engine's copy takes the one reference the button owns
        UWidget* label = sort->Slots.Num() > 0 && PtrOk(sort->Slots[0]) ? sort->Slots[0]->Content : nullptr;
        if (PtrOk(label) && label->IsA(UTextBlock::StaticClass()))
            b->Text->ColorAndOpacity.SpecifiedColor = static_cast<UTextBlock*>(label)->ColorAndOpacity.SpecifiedColor;

        struct Moved { UWidget* w; UHorizontalBoxSlot slot; };
        std::vector<Moved> after;
        bool past = false;
        for (int i = 0; i < row->Slots.Num(); i++) {
            UPanelSlot* s = row->Slots[i];
            if (!PtrOk(s) || !PtrOk(s->Content)) continue;
            if (past && s->IsA(UHorizontalBoxSlot::StaticClass())) after.push_back({s->Content, *static_cast<UHorizontalBoxSlot*>(s)});
            if (s->Content == sort) past = true;
        }
        for (const Moved& m : after) {
            Params::PanelWidget_RemoveChild r{};
            r.Content = m.w;
            CallNative(row, g_fn.removeChild.Get(), &r);
        }
        if (UHorizontalBoxSlot* s = Add(row, b)) SlotLike(s, *static_cast<UHorizontalBoxSlot*>(sort->Slot));
        for (const Moved& m : after)
            if (UHorizontalBoxSlot* s = Add(row, m.w)) SlotLike(s, m.slot);
        logger::log(std::string("[item-sort] profile button placed in ") + (h->IsStorage ? "bank" : "inventory") + " header");
        return b;
    }

    // One bag header: adopt our button already in it, else place one. true = newly tracked.
    bool Consider(UWidgetitemBagHeaderMenu_C* h, APlayerController* pc) {
        for (const Placed& p : g_placed) if (p.header.Is(h)) return false;
        if ((int(h->Flags) & 0x30) || !PtrOk(h->Button_Sort) || !PtrOk(h->Button_Sort->Slot) || !PtrOk(h->Button_Sort->Slot->Parent)) return false;  // CDO / archetype: never touch
        UPanelWidget* row = h->Button_Sort->Slot->Parent;
        if (!row->IsA(UHorizontalBox::StaticClass()) || !h->Button_Sort->Slot->IsA(UHorizontalBoxSlot::StaticClass())) return false;
        UWidgetButton01_C* b = Existing(row);
        if (!b) b = Place(h, row, pc);
        if (b) g_placed.push_back({ref::Ref(h), ref::Ref(b)});
        return b != nullptr;
    }

    std::vector<ref::Ref> g_pending;  // UWidgetitemBagHeaderMenu_C seen since the last world tick

    void RemoveAll() {  // game thread (or Off() after its wait ran out)
        for (const Placed& p : g_placed) if (UObject* b = p.button.Get()) CallNative(b, g_fn.remove.Get(), nullptr);
        g_placed.clear();
        g_pending.clear();
    }

    void OnEvent(void* objp, void* fnp, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !game::OnGameThread()) return;  // shared state, UFunction calls and ref resolution: game thread only
        t_busy = true;
        if (umg::IsWorldTick(fnp) && g_drain.Serve(RemoveAll)) { t_busy = false; return; }
        if (g_fn.clicked.Is(fnp))
            for (const Placed& p : g_placed)
                if (auto* h = p.header.Get<UWidgetitemBagHeaderMenu_C>(); h && p.button.Is(objp)) {
                    const int n = int(item_sort::api::ProfileNames().size());
                    if (n > 0) item_sort::api::SetActiveProfile((item_sort::api::ActiveProfile() + 1) % n);
                    item_sort::api::RequestSort(h->IsStorage);
                    break;
                }
        const int active = item_sort::api::ActiveProfile();
        // Headers come from events, never a GObjects walk: the header's own calls, or the bag screens that own one
        // (WidgetItemInventory_C / WidgetItemStorage_C: Construct runs each time the screen opens). Three class
        // compares per event; placing waits for the world tick (#50).
        if (auto* obj = static_cast<UObject*>(objp); PtrOk(obj)) {
            UClass* headerCls = g_headerCls.Get();
            UObject* h = !headerCls                   ? nullptr
                       : obj->Class == headerCls      ? obj
                       : obj->Class == g_invCls.Get()  ? static_cast<UWidgetItemInventory_C*>(obj)->WidgetitemBagHeaderMenu
                       : obj->Class == g_storCls.Get() ? static_cast<UWidgetItemStorage_C*>(obj)->WidgetitemBagHeaderMenu
                                                       : nullptr;
            if (PtrOk(h) && h->Class == headerCls && std::none_of(g_pending.begin(), g_pending.end(), [&](const ref::Ref& p) { return p.Is(h); }))
                g_pending.push_back(ref::Ref(h));
        }
        if (umg::IsWorldTick(fnp) && !g_pending.empty()) {
            if (APlayerController* pc = umg::LocalPC()) {
                std::erase_if(g_placed, [](const Placed& p) { return !p.header.Get() || !p.button.Get(); });
                bool added = false;
                for (const ref::Ref& p : g_pending) if (auto* h = p.Get<UWidgetitemBagHeaderMenu_C>()) added |= Consider(h, pc);
                if (added) g_shown = -1;
                g_pending.clear();
            }
        }
        if (active != g_shown) {
            for (const Placed& p : g_placed) if (auto* b = p.button.Get<UWidgetButton01_C>()) SetLabel(b, Label(active));
            g_shown = active;
        }
        t_busy = false;
    }
}

namespace item_sort::ui {
    void Frame() {
        if (g_on.load()) return;
        g_on = true;  // game functions resolve on use (ref::Fn)
        game::SetEventListener(&OnEvent, true);
    }
    // Runs with the ProcessEvent hook alive (#84): the next world tick removes the buttons.
    void Off() {
        g_drain.Request(!g_placed.empty(), "item-sort", RemoveAll);
        g_on = false;
        game::SetEventListener(&OnEvent, false);
        g_placed.clear();
        g_pending.clear();
    }
}
