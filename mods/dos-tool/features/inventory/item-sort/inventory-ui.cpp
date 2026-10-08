#include "inventory-ui.hpp"
#include "item-sort.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "umg.hpp"

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
using umg::Alive;
using umg::CallNative;
using umg::PtrOk;

namespace {

    struct Fns {
        UFunction *create, *addChild, *removeChild, *setSize, *setPad, *setH, *setV, *setText, *clicked;
        bool ok() const { return create && addChild && removeChild && setSize && setPad && setH && setV && setText && clicked; }
    } g_fn{};
    struct Placed { UWidgetitemBagHeaderMenu_C* header; int32 headerIdx; UWidgetButton01_C* button; int32 buttonIdx; };
    std::vector<Placed> g_placed;
    std::atomic<bool> g_on{false};
    thread_local bool t_busy = false;
    int g_shown = -1;  // profile index the labels show
    UClass *g_headerCls = nullptr, *g_invCls = nullptr, *g_storCls = nullptr;

    bool Resolve() {
        g_headerCls = UWidgetitemBagHeaderMenu_C::StaticClass();
        g_invCls = UWidgetItemInventory_C::StaticClass();
        g_storCls = UWidgetItemStorage_C::StaticClass();
        g_fn.create = UWidgetBlueprintLibrary::StaticClass()->GetFunction("WidgetBlueprintLibrary", "Create");
        g_fn.addChild = UPanelWidget::StaticClass()->GetFunction("PanelWidget", "AddChild");
        g_fn.removeChild = UPanelWidget::StaticClass()->GetFunction("PanelWidget", "RemoveChild");
        g_fn.setSize = UHorizontalBoxSlot::StaticClass()->GetFunction("HorizontalBoxSlot", "SetSize");
        g_fn.setPad = UHorizontalBoxSlot::StaticClass()->GetFunction("HorizontalBoxSlot", "SetPadding");
        g_fn.setH = UHorizontalBoxSlot::StaticClass()->GetFunction("HorizontalBoxSlot", "SetHorizontalAlignment");
        g_fn.setV = UHorizontalBoxSlot::StaticClass()->GetFunction("HorizontalBoxSlot", "SetVerticalAlignment");
        g_fn.setText = UWidgetButton01_C::StaticClass()->GetFunction("WidgetButton01_C", "SetButtonText");
        g_fn.clicked = UWidgetButton01_C::StaticClass()->GetFunction("WidgetButton01_C", "BndEvt__WidgetButton01_Button_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature");
        return g_fn.ok();
    }

    void SetLabel(UWidgetButton01_C* b, const std::string& s) {  // labels change only on click
        Params::WidgetButton01_C_SetButtonText p{};
        p.Text_0 = umg::Text(s);
        b->ProcessEvent(g_fn.setText, &p);
    }
    std::string Label(int i) {
        const std::vector<std::string> names = item_sort::api::ProfileNames();
        return "Profile: " + (i >= 0 && i < int(names.size()) ? names[i] : std::string("-"));
    }

    void SlotLike(UHorizontalBoxSlot* to, const UHorizontalBoxSlot& from) {
        Params::HorizontalBoxSlot_SetSize sz{from.Size};
        CallNative(to, g_fn.setSize, &sz);
        Params::HorizontalBoxSlot_SetPadding pd{from.Padding};
        CallNative(to, g_fn.setPad, &pd);
        Params::HorizontalBoxSlot_SetHorizontalAlignment h{from.HorizontalAlignment};
        CallNative(to, g_fn.setH, &h);
        Params::HorizontalBoxSlot_SetVerticalAlignment v{from.VerticalAlignment};
        CallNative(to, g_fn.setV, &v);
    }
    UHorizontalBoxSlot* Add(UPanelWidget* row, UWidget* w) {
        Params::PanelWidget_AddChild a{};
        a.Content = w;
        CallNative(row, g_fn.addChild, &a);
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
        CallNative(UWidgetBlueprintLibrary::GetDefaultObj(), g_fn.create, &c);
        auto* b = static_cast<UWidgetButton01_C*>(c.ReturnValue);
        if (!PtrOk(b) || !PtrOk(b->Button) || !PtrOk(b->Text)) return nullptr;
        UButton* sort = h->Button_Sort;
        b->Button->WidgetStyle = sort->WidgetStyle;  // before the Slate widget exists: Sort's brushes, paddings, sounds
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
            CallNative(row, g_fn.removeChild, &r);
        }
        if (UHorizontalBoxSlot* s = Add(row, b)) SlotLike(s, *static_cast<UHorizontalBoxSlot*>(sort->Slot));
        for (const Moved& m : after)
            if (UHorizontalBoxSlot* s = Add(row, m.w)) SlotLike(s, m.slot);
        logger::log(std::string("[item-sort] profile button placed in ") + (h->IsStorage ? "bank" : "inventory") + " header");
        return b;
    }

    // One bag header: adopt our button already in it, else place one. true = newly tracked.
    bool Consider(UWidgetitemBagHeaderMenu_C* h, APlayerController* pc) {
        for (const Placed& p : g_placed) if (p.header == h) return false;
        if ((int(h->Flags) & 0x30) || !PtrOk(h->Button_Sort) || !PtrOk(h->Button_Sort->Slot) || !PtrOk(h->Button_Sort->Slot->Parent)) return false;  // CDO / archetype: never touch
        UPanelWidget* row = h->Button_Sort->Slot->Parent;
        if (!row->IsA(UHorizontalBox::StaticClass()) || !h->Button_Sort->Slot->IsA(UHorizontalBoxSlot::StaticClass())) return false;
        UWidgetButton01_C* b = Existing(row);
        if (!b) b = Place(h, row, pc);
        if (b) g_placed.push_back({h, h->Index, b, b->Index});
        return b != nullptr;
    }

    struct Pending { UWidgetitemBagHeaderMenu_C* h; int32 idx; };
    std::vector<Pending> g_pending;  // headers seen since the last world tick

    void OnEvent(void* objp, void* fnp, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed)) return;
        t_busy = true;
        if (fnp == g_fn.clicked)
            for (const Placed& p : g_placed)
                if (p.button == objp) {
                    const int n = int(item_sort::api::ProfileNames().size());
                    if (n > 0) item_sort::api::SetActiveProfile((item_sort::api::ActiveProfile() + 1) % n);
                    item_sort::api::RequestSort(p.header->IsStorage);
                    break;
                }
        const int active = item_sort::api::ActiveProfile();
        // Headers come from events, never a GObjects walk: the header's own calls, or the bag screens that own one
        // (WidgetItemInventory_C / WidgetItemStorage_C: Construct runs each time the screen opens). Three class
        // compares per event; placing waits for the world tick (#50).
        if (auto* obj = static_cast<UObject*>(objp); PtrOk(obj)) {
            UObject* h = obj->Class == g_headerCls ? obj
                       : obj->Class == g_invCls    ? static_cast<UWidgetItemInventory_C*>(obj)->WidgetitemBagHeaderMenu
                       : obj->Class == g_storCls   ? static_cast<UWidgetItemStorage_C*>(obj)->WidgetitemBagHeaderMenu
                                                   : nullptr;
            if (PtrOk(h) && h->Class == g_headerCls && std::none_of(g_pending.begin(), g_pending.end(), [&](const Pending& p) { return p.h == h; }))
                g_pending.push_back({static_cast<UWidgetitemBagHeaderMenu_C*>(h), h->Index});
        }
        if (umg::IsWorldTick(fnp) && !g_pending.empty()) {
            if (APlayerController* pc = umg::LocalPC()) {
                std::erase_if(g_placed, [](const Placed& p) { return !Alive(p.header, p.headerIdx) || !Alive(p.button, p.buttonIdx); });
                bool added = false;
                for (const Pending& p : g_pending) if (Alive(p.h, p.idx)) added |= Consider(p.h, pc);
                if (added) g_shown = -1;
                g_pending.clear();
            }
        }
        if (active != g_shown) {
            for (const Placed& p : g_placed) SetLabel(p.button, Label(active));
            g_shown = active;
        }
        t_busy = false;
    }
}

namespace item_sort::ui {
    void Frame() {
        if (g_on.load()) return;
        static bool resolved = false, failed = false;
        if (!resolved && !failed) {
            resolved = Resolve();
            failed = !resolved;
            logger::log(resolved ? "[item-sort] inventory UI ready" : "[item-sort] inventory UI: game functions not found");
        }
        if (!resolved) return;
        g_on = true;
        game::SetEventListener(&OnEvent, true);
    }
    // ponytail: placed buttons stay (inert) until the game rebuilds its menus; hiding needs the game thread
    void Off() {
        g_on = false;
        game::SetEventListener(&OnEvent, false);
        g_placed.clear();
        g_pending.clear();
    }
}
