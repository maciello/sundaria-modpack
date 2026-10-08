#include "inventory-badges.hpp"
#include "item-sell.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "umg.hpp"

#include <atomic>
#include <string>
#include <vector>
#include "UMG_classes.hpp"
#include "UMG_parameters.hpp"
#include "WidgetItemBag_classes.hpp"
#include "WidgetItemIconContainer_classes.hpp"
#include "WidgetItemDisplayDetail_classes.hpp"
#include "FItemContainerFunctions_classes.hpp"
#include "FItemContainerFunctions_parameters.hpp"

// Each suggested item slot gets the game's own action icon (Tooltip_Sell / Tooltip_Salvage, the icon the game shows
// for an item's action) in its top-left corner. The game's item details panel gets one line under its text,
// styled like the panel's own orange "Learned" line: "Sell suggested: <reason>".
// Game thread only. Facts: references/game-ui.md § Item slot.
using namespace SDK;
using umg::Alive;
using umg::CallNative;
using umg::PtrOk;

namespace {
    using item_sell::api::Suggestion;

    struct Fns {
        UFunction *addChild, *overH, *overV, *overPad, *vboxH, *setVis, *setTex, *setFont, *setText, *convert, *detailTick;
        bool ok() const { return addChild && overH && overV && overPad && vboxH && setVis && setTex && setFont && setText && convert && detailTick; }
    } g_fn{};
    UTexture2D* g_tex[2]{};  // by Action: Sell, Salvage
    bool g_texSearched = false;

    struct Badge {
        UWidgetItemIconContainer_C* slot; int32 slotIdx;
        UImage* img; int32 imgIdx;
        int shown;            // Action shown, -1 = hidden
        std::string reason;   // of the suggestion shown
    };
    struct Line {
        UWidgetItemDisplayDetail_C* detail; int32 detailIdx;
        UTextBlock* text; int32 textIdx;
        std::string shown;  // text shown, empty = hidden
    };
    std::vector<Badge> g_badges;
    std::vector<Line> g_lines;
    std::atomic<bool> g_on{false};
    thread_local bool t_busy = false;
    ULONGLONG g_nextScan = 0;

    bool Resolve() {
        g_fn.addChild = UPanelWidget::StaticClass()->GetFunction("PanelWidget", "AddChild");
        g_fn.overH = UOverlaySlot::StaticClass()->GetFunction("OverlaySlot", "SetHorizontalAlignment");
        g_fn.overV = UOverlaySlot::StaticClass()->GetFunction("OverlaySlot", "SetVerticalAlignment");
        g_fn.overPad = UOverlaySlot::StaticClass()->GetFunction("OverlaySlot", "SetPadding");
        g_fn.vboxH = UVerticalBoxSlot::StaticClass()->GetFunction("VerticalBoxSlot", "SetHorizontalAlignment");
        g_fn.setVis = UWidget::StaticClass()->GetFunction("Widget", "SetVisibility");
        g_fn.setTex = UImage::StaticClass()->GetFunction("Image", "SetBrushFromTexture");
        g_fn.setFont = UTextBlock::StaticClass()->GetFunction("TextBlock", "SetFont");
        g_fn.setText = UTextBlock::StaticClass()->GetFunction("TextBlock", "SetText");
        g_fn.convert = UFItemContainerFunctions_C::StaticClass()->GetFunction("FItemContainerFunctions_C", "ConvertCompressedItemSlot");
        g_fn.detailTick = UWidgetItemDisplayDetail_C::StaticClass()->GetFunction("WidgetItemDisplayDetail_C", "Tick");
        return g_fn.ok();
    }

    // The game's action icons are loaded with its item tooltip class; one GObjects pass, logged once.
    void FindTextures() {
        std::string seen;
        UClass* cls = UTexture2D::StaticClass();
        for (int i = 0; i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!PtrOk(o) || !o->IsA(cls)) continue;
            const std::string n = o->GetName();
            if (n.rfind("Tooltip_", 0) != 0) continue;
            seen += " " + n;
            if (n == "Tooltip_Sell") g_tex[0] = static_cast<UTexture2D*>(o);
            if (n == "Tooltip_Salvage") g_tex[1] = static_cast<UTexture2D*>(o);
        }
        if (!g_tex[0]) return;  // not loaded yet: next scan
        if (!g_tex[1]) g_tex[1] = g_tex[0];
        g_texSearched = true;
        logger::log("[item-sell] action icons:" + seen);
    }

    void SetVis(UWidget* w, ESlateVisibility v) {
        Params::Widget_SetVisibility p{v};
        CallNative(w, g_fn.setVis, &p);
    }
    void SetTexture(UImage* img, UTexture2D* tex) {
        Params::Image_SetBrushFromTexture p{};
        p.Texture = tex;
        CallNative(img, g_fn.setTex, &p);
    }
    UPanelSlot* Add(UPanelWidget* panel, UWidget* w) {
        Params::PanelWidget_AddChild a{};
        a.Content = w;
        CallNative(panel, g_fn.addChild, &a);
        return PtrOk(a.ReturnValue) ? a.ReturnValue : nullptr;
    }

    // Our badge in this slot (also one left by a previous DLL before a hot reload): a UImage with an action icon.
    UImage* Existing(UOverlay* o) {
        for (int i = 0; i < o->Slots.Num(); i++) {
            UPanelSlot* s = o->Slots[i];
            if (!PtrOk(s) || !PtrOk(s->Content) || !s->Content->IsA(UImage::StaticClass())) continue;
            UObject* res = static_cast<UImage*>(s->Content)->Brush.ResourceObject;
            if (res && (res == g_tex[0] || res == g_tex[1])) return static_cast<UImage*>(s->Content);
        }
        return nullptr;
    }
    UImage* NewBadge(UWidgetItemIconContainer_C* c) {
        auto* img = static_cast<UImage*>(umg::Spawn(UImage::StaticClass(), c->WidgetTree));
        if (!img) return nullptr;
        img->Brush.ImageSize = {32.f, 32.f};  // before the Slate widget exists: plain writes take effect
        img->Visibility = ESlateVisibility::Collapsed;
        SetTexture(img, g_tex[0]);
        UPanelSlot* s = Add(c->Overlay_Container, img);
        if (!s) return nullptr;
        Params::OverlaySlot_SetHorizontalAlignment h{EHorizontalAlignment::HAlign_Left};
        CallNative(s, g_fn.overH, &h);
        Params::OverlaySlot_SetVerticalAlignment v{EVerticalAlignment::VAlign_Top};
        CallNative(s, g_fn.overV, &v);
        Params::OverlaySlot_SetPadding p{{6.f, 6.f, 0.f, 0.f}};
        CallNative(s, g_fn.overPad, &p);
        return img;
    }

    // Detail line: a TextBlock styled like the panel's own "Learned" line (Narkisim 16, the game's orange).
    UTextBlock* NewLine(UWidgetItemDisplayDetail_C* d) {
        UTextBlock* ref = d->TextBlock_AlreadyLearned;
        if (!PtrOk(ref) || !PtrOk(ref->Slot) || !PtrOk(ref->Slot->Parent)) return nullptr;
        auto* t = static_cast<UTextBlock*>(umg::Spawn(UTextBlock::StaticClass(), d->WidgetTree));
        if (!t) return nullptr;
        t->ColorAndOpacity.SpecifiedColor = ref->ColorAndOpacity.SpecifiedColor;
        t->ShadowOffset = ref->ShadowOffset;
        t->ShadowColorAndOpacity = ref->ShadowColorAndOpacity;
        t->bAutoWrapText = true;
        t->Visibility = ESlateVisibility::Collapsed;
        Params::TextBlock_SetFont f{};
        f.InFontInfo = ref->Font;
        CallNative(t, g_fn.setFont, &f);
        UPanelSlot* s = Add(ref->Slot->Parent, t);
        if (!s || !s->IsA(UVerticalBoxSlot::StaticClass())) return t;
        Params::VerticalBoxSlot_SetHorizontalAlignment h{EHorizontalAlignment::HAlign_Center};
        CallNative(s, g_fn.vboxH, &h);
        return t;
    }

    const Suggestion* Find(const std::vector<Suggestion>& all, UWidgetItemIconContainer_C* c) {
        Params::FItemContainerFunctions_C_ConvertCompressedItemSlot p{};
        p.CompressedItemSlot = c->CompressedItemSlot;
        p.__WorldContext = c;
        UFItemContainerFunctions_C::GetDefaultObj()->ProcessEvent(g_fn.convert, &p);
        for (const Suggestion& s : all)
            if (s.bank == c->IsStorage && s.slot == p.ItemSlot && s.containerType == uint8(p.ContainerType)) return &s;
        return nullptr;
    }

    void ScanSlots(const std::vector<Suggestion>& all) {
        std::erase_if(g_badges, [](const Badge& b) { return !Alive(b.slot, b.slotIdx) || !Alive(b.img, b.imgIdx); });
        UClass* bagCls = UWidgetItemBag_C::StaticClass();
        for (int i = 0; PtrOk(bagCls) && i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!umg::Live(o) || !o->IsA(bagCls)) continue;
            auto& slots = static_cast<UWidgetItemBag_C*>(o)->ItemContainers;
            for (int k = 0; k < slots.Num(); k++) {
                UWidgetItemIconContainer_C* c = slots[k];
                if (!umg::Live(c) || !PtrOk(c->Overlay_Container) || !PtrOk(c->WidgetTree)) continue;
                Badge* b = nullptr;
                for (Badge& x : g_badges) if (x.slot == c) b = &x;
                const Suggestion* s = Find(all, c);
                if (!b) {
                    if (!s) continue;  // badges are created on first need only
                    UImage* img = Existing(c->Overlay_Container);
                    if (!img) img = NewBadge(c);
                    if (!img) continue;
                    b = &g_badges.emplace_back(Badge{c, c->Index, img, img->Index, -2, {}});
                }
                const int want = s ? int(s->action) : -1;
                if (s) b->reason = s->reason;
                if (want == b->shown) continue;
                if (want >= 0) SetTexture(b->img, g_tex[want]);
                SetVis(b->img, want >= 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
                b->shown = want;
            }
        }
    }

    void ScanDetails() {
        std::erase_if(g_lines, [](const Line& l) { return !Alive(l.detail, l.detailIdx) || !Alive(l.text, l.textIdx); });
        UClass* cls = UWidgetItemDisplayDetail_C::StaticClass();
        for (int i = 0; PtrOk(cls) && i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!umg::Live(o) || !o->IsA(cls)) continue;
            auto* d = static_cast<UWidgetItemDisplayDetail_C*>(o);
            bool known = false;
            for (const Line& l : g_lines) known |= l.detail == d;
            if (known || !PtrOk(d->WidgetTree)) continue;
            if (UTextBlock* t = NewLine(d)) g_lines.push_back({d, d->Index, t, t->Index, {}});
        }
    }

    // The details panel shows the item in LoadedItemUIData; the same item's slot widget tells whether it is suggested.
    void UpdateLine(Line& l) {
        const FSItemUIData& it = l.detail->LoadedItemUIData;
        std::string want;
        for (const Badge& b : g_badges) {
            if (b.shown < 0 || !Alive(b.slot, b.slotIdx)) continue;
            const FSItemUIData& s = b.slot->ItemData;
            if (s.SpecId_23_B842031D4333CC5CB157B2ACEF10803B == it.SpecId_23_B842031D4333CC5CB157B2ACEF10803B &&
                s.ChangeID_14_3C4F923F41B7BD67025418A6109516F6 == it.ChangeID_14_3C4F923F41B7BD67025418A6109516F6 &&
                s.IconID_2_B4E9648B461DD2F0023603B7C3264262 == it.IconID_2_B4E9648B461DD2F0023603B7C3264262) {
                want = std::string(b.shown == int(item_sell::api::Action::Salvage) ? "Salvage" : "Sell") + " suggested: " + b.reason;
                break;
            }
        }
        if (want == l.shown) return;
        if (!want.empty()) {
            Params::TextBlock_SetText p{};
            p.InText = umg::Text(want);
            CallNative(l.text, g_fn.setText, &p);
        }
        SetVis(l.text, want.empty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
        l.shown = std::move(want);
    }

    void OnEvent(void* objp, void* fnp, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed)) return;
        t_busy = true;
        const ULONGLONG now = GetTickCount64();
        if (now >= g_nextScan) {
            g_nextScan = now + 500;
            if (!g_texSearched) FindTextures();
            if (g_texSearched) {
                ScanSlots(item_sell::api::Suggested());
                ScanDetails();
            }
        }
        if (fnp == g_fn.detailTick)
            for (Line& l : g_lines)
                if (l.detail == objp) UpdateLine(l);
        t_busy = false;
    }
}

namespace item_sell::badges {
    void Frame() {
        if (g_on.load()) return;
        static bool resolved = false, failed = false;
        if (!resolved && !failed) {
            resolved = Resolve();
            failed = !resolved;
            logger::log(resolved ? "[item-sell] inventory badges ready" : "[item-sell] inventory badges: game functions not found");
        }
        if (!resolved) return;
        g_on = true;
        game::SetEventListener(&OnEvent, true);
    }
    // ponytail: shown badges stay (inert) until the game rebuilds its menus; hiding needs the game thread
    void Off() {
        g_on = false;
        game::SetEventListener(&OnEvent, false);
        g_badges.clear();
        g_lines.clear();
    }
}
