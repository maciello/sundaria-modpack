#include "inventory-ui.hpp"
#include "item-sort.hpp"
#include "game.hpp"
#include "logger.hpp"

#include <Windows.h>
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

// Item-sort controls inside the game's own inventory and bank: a game button (WidgetButton01_C, the game's
// Button05 art, styled exactly like the header's Sort) placed right after Sort in the bag header row.
// Click = next sort profile + re-sort that bag. Spec: references/design-system.md § Inventory sort profile.
// Game thread only (ProcessEvent listener). Facts: references/game-ui.md.
using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }
    void CallNative(const UObject* obj, UFunction* fn, void* parms) {  // same call shape as Dumper-7's native bodies
        auto flags = fn->FunctionFlags;
        fn->FunctionFlags |= 0x400;
        obj->ProcessEvent(fn, parms);
        fn->FunctionFlags = flags;
    }

    struct Fns {
        UFunction *create, *addChild, *removeChild, *setSize, *setPad, *setH, *setV, *toText, *setText, *clicked;
        bool ok() const { return create && addChild && removeChild && setSize && setPad && setH && setV && toText && setText && clicked; }
    } g_fn{};
    struct Placed { UWidgetitemBagHeaderMenu_C* header; int32 headerIdx; UWidgetButton01_C* button; int32 buttonIdx; };
    std::vector<Placed> g_placed;
    std::atomic<bool> g_on{false};
    thread_local bool t_busy = false;
    ULONGLONG g_nextScan = 0;
    int g_shown = -1;  // profile index the labels show

    bool Alive(UObject* o, int32 idx) { return PtrOk(o) && UObject::GObjects->GetByIndex(idx) == o; }

    bool Resolve() {
        g_fn.create = UWidgetBlueprintLibrary::StaticClass()->GetFunction("WidgetBlueprintLibrary", "Create");
        g_fn.addChild = UPanelWidget::StaticClass()->GetFunction("PanelWidget", "AddChild");
        g_fn.removeChild = UPanelWidget::StaticClass()->GetFunction("PanelWidget", "RemoveChild");
        g_fn.setSize = UHorizontalBoxSlot::StaticClass()->GetFunction("HorizontalBoxSlot", "SetSize");
        g_fn.setPad = UHorizontalBoxSlot::StaticClass()->GetFunction("HorizontalBoxSlot", "SetPadding");
        g_fn.setH = UHorizontalBoxSlot::StaticClass()->GetFunction("HorizontalBoxSlot", "SetHorizontalAlignment");
        g_fn.setV = UHorizontalBoxSlot::StaticClass()->GetFunction("HorizontalBoxSlot", "SetVerticalAlignment");
        g_fn.toText = UKismetTextLibrary::StaticClass()->GetFunction("KismetTextLibrary", "Conv_StringToText");
        g_fn.setText = UWidgetButton01_C::StaticClass()->GetFunction("WidgetButton01_C", "SetButtonText");
        g_fn.clicked = UWidgetButton01_C::StaticClass()->GetFunction("WidgetButton01_C", "BndEvt__WidgetButton01_Button_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature");
        return g_fn.ok();
    }

    APlayerController* LocalPC() {
        UWorld* w = UWorld::GetWorld();
        if (!PtrOk(w) || !PtrOk(w->OwningGameInstance) || w->OwningGameInstance->LocalPlayers.Num() < 1) return nullptr;
        ULocalPlayer* lp = w->OwningGameInstance->LocalPlayers[0];
        return PtrOk(lp) && PtrOk(lp->PlayerController) ? lp->PlayerController : nullptr;
    }

    // ponytail: each label change leaks one FText reference (a few bytes); labels change only on click
    void SetLabel(UWidgetButton01_C* b, const std::string& s) {
        std::wstring w(s.begin(), s.end());
        Params::KismetTextLibrary_Conv_StringToText t{};
        t.inString = FString(w.c_str());
        CallNative(UKismetTextLibrary::GetDefaultObj(), g_fn.toText, &t);
        Params::WidgetButton01_C_SetButtonText p{};
        p.Text_0 = t.ReturnValue;
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

    bool Scan() {  // true = a button was placed or adopted
        bool added = false;
        APlayerController* pc = LocalPC();
        if (!pc) return false;
        std::erase_if(g_placed, [](const Placed& p) { return !Alive(p.header, p.headerIdx) || !Alive(p.button, p.buttonIdx); });
        UClass* cls = UWidgetitemBagHeaderMenu_C::StaticClass();
        for (int i = 0; PtrOk(cls) && i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!PtrOk(o) || !o->IsA(cls) || (int(o->Flags) & 0x30)) continue;  // CDO / archetype (class templates): never touch
            auto* h = static_cast<UWidgetitemBagHeaderMenu_C*>(o);
            bool known = false;
            for (const Placed& p : g_placed) known |= p.header == h;
            if (known || !PtrOk(h->Button_Sort) || !PtrOk(h->Button_Sort->Slot) || !PtrOk(h->Button_Sort->Slot->Parent)) continue;
            UPanelWidget* row = h->Button_Sort->Slot->Parent;
            if (!row->IsA(UHorizontalBox::StaticClass()) || !h->Button_Sort->Slot->IsA(UHorizontalBoxSlot::StaticClass())) continue;
            UWidgetButton01_C* b = Existing(row);
            if (!b) b = Place(h, row, pc);
            if (b) { g_placed.push_back({h, h->Index, b, b->Index}); added = true; }
        }
        return added;
    }

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
        const ULONGLONG now = GetTickCount64();
        if (now >= g_nextScan) { g_nextScan = now + 500; if (Scan()) g_shown = -1; }
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
    }
}
