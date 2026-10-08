#include "inventory-badges.hpp"
#include "item-sell.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "umg.hpp"
#include "ref.hpp"
#include "cost.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "UMG_classes.hpp"
#include "UMG_parameters.hpp"
#include "WidgetItemBag_classes.hpp"
#include "WidgetItemInventory_classes.hpp"
#include "WidgetItemStorage_classes.hpp"
#include "WidgetItemIconContainer_classes.hpp"
#include "WidgetItemDisplayDetail_classes.hpp"
#include "FItemContainerFunctions_classes.hpp"
#include "FItemContainerFunctions_parameters.hpp"

// Each suggested item slot gets the game's own action icon (Tooltip_Sell / Tooltip_Salvage, the icon the game shows
// for an item's action) in its top-left corner. The game's item details panel gets one line under its text,
// styled like the panel's own orange "Learned" line: "Sell suggested: <reason>".
// Game thread only. Facts: references/game-ui.md § Item slot.
using namespace SDK;
using umg::CallNative;
using umg::PtrOk;

namespace {
    using item_sell::api::Suggestion;

    // Game thread. Blueprint classes come and go with the map (#63): ref::Fn / ref::Cached re-resolve them.
    struct Fns {
        ref::Fn addChild{UPanelWidget::StaticClass, "PanelWidget", "AddChild"};
        ref::Fn overH{UOverlaySlot::StaticClass, "OverlaySlot", "SetHorizontalAlignment"};
        ref::Fn overV{UOverlaySlot::StaticClass, "OverlaySlot", "SetVerticalAlignment"};
        ref::Fn overPad{UOverlaySlot::StaticClass, "OverlaySlot", "SetPadding"};
        ref::Fn vboxH{UVerticalBoxSlot::StaticClass, "VerticalBoxSlot", "SetHorizontalAlignment"};
        ref::Fn setVis{UWidget::StaticClass, "Widget", "SetVisibility"};
        ref::Fn setTex{UImage::StaticClass, "Image", "SetBrushFromTexture"};
        ref::Fn setFont{UTextBlock::StaticClass, "TextBlock", "SetFont"};
        ref::Fn setText{UTextBlock::StaticClass, "TextBlock", "SetText"};
        ref::Fn convert{UFItemContainerFunctions_C::StaticClass, "FItemContainerFunctions_C", "ConvertCompressedItemSlot"};
        ref::Fn detailTick{UWidgetItemDisplayDetail_C::StaticClass, "WidgetItemDisplayDetail_C", "Tick"};
    } g_fn;
    ref::Ref g_tex[2];  // UTexture2D by Action: Sell, Salvage
    bool g_texSearched = false;

    struct Badge {
        ref::Ref slot, img;  // UWidgetItemIconContainer_C, UImage
        int shown;            // Action shown, -1 = hidden
        std::string reason;   // of the suggestion shown
    };
    struct Line {
        ref::Ref detail, text;  // UWidgetItemDisplayDetail_C, UTextBlock
        std::string shown;  // text shown, empty = hidden
    };
    std::vector<Badge> g_badges;
    std::vector<Line> g_lines;
    // Bags and detail panels the game itself reported through their own events; nothing is searched for.
    std::vector<ref::Ref> g_bags;        // UWidgetItemBag_C
    std::vector<ref::Ref> g_newDetails;  // UWidgetItemDisplayDetail_C without a line yet
    std::vector<ref::Ref> g_noLine;      // detail panels whose layout took no line; not retried
    bool g_dirty = false, g_texRetry = false;
    int g_profile = -1;
    ref::Cached<UClass> g_bagCls{[] { return UWidgetItemBag_C::StaticClass(); }};
    ref::Cached<UClass> g_invCls{[] { return UWidgetItemInventory_C::StaticClass(); }};
    ref::Cached<UClass> g_storCls{[] { return UWidgetItemStorage_C::StaticClass(); }};
    std::atomic<bool> g_on{false};
    thread_local bool t_busy = false;

    // The game's action icons load with its item tooltip class. Looked up once per newly seen bag or detail panel
    // until found (game::FindSingleton logs the cost); cached for the session.
    void FindTextures() {
        g_tex[0] = ref::Ref(game::FindSingleton("Texture2D", "Tooltip_Sell"));
        if (!g_tex[0].ptr) return;
        g_tex[1] = ref::Ref(game::FindSingleton("Texture2D", "Tooltip_Salvage"));
        if (!g_tex[1].ptr) g_tex[1] = g_tex[0];
        g_texSearched = true;
    }
    // A collected icon (its tooltip class unloaded) is searched again with the next newly seen bag.
    UTexture2D* Tex(int action) {
        auto* t = g_tex[action].Get<UTexture2D>();
        if (!t) g_texSearched = false;
        return t;
    }

    void SetVis(UWidget* w, ESlateVisibility v) {
        Params::Widget_SetVisibility p{v};
        CallNative(w, g_fn.setVis.Get(), &p);
    }
    void SetTexture(UImage* img, UTexture2D* tex) {
        Params::Image_SetBrushFromTexture p{};
        p.Texture = tex;
        CallNative(img, g_fn.setTex.Get(), &p);
    }
    UPanelSlot* Add(UPanelWidget* panel, UWidget* w) {
        Params::PanelWidget_AddChild a{};
        a.Content = w;
        CallNative(panel, g_fn.addChild.Get(), &a);
        return PtrOk(a.ReturnValue) ? a.ReturnValue : nullptr;
    }

    // Our badge in this slot (also one left by a previous DLL before a hot reload): a UImage with an action icon.
    UImage* Existing(UOverlay* o) {
        for (int i = 0; i < o->Slots.Num(); i++) {
            UPanelSlot* s = o->Slots[i];
            if (!PtrOk(s) || !PtrOk(s->Content) || !s->Content->IsA(UImage::StaticClass())) continue;
            UObject* res = static_cast<UImage*>(s->Content)->Brush.ResourceObject;
            if (res && (g_tex[0].Is(res) || g_tex[1].Is(res))) return static_cast<UImage*>(s->Content);
        }
        return nullptr;
    }
    UImage* NewBadge(UWidgetItemIconContainer_C* c) {
        auto* img = static_cast<UImage*>(umg::Spawn(UImage::StaticClass(), c->WidgetTree));
        if (!img) return nullptr;
        img->Brush.ImageSize = {32.f, 32.f};  // before the Slate widget exists: plain writes take effect
        img->Visibility = ESlateVisibility::Collapsed;
        SetTexture(img, Tex(0));
        UPanelSlot* s = Add(c->Overlay_Container, img);
        if (!s) return nullptr;
        Params::OverlaySlot_SetHorizontalAlignment h{EHorizontalAlignment::HAlign_Left};
        CallNative(s, g_fn.overH.Get(), &h);
        Params::OverlaySlot_SetVerticalAlignment v{EVerticalAlignment::VAlign_Top};
        CallNative(s, g_fn.overV.Get(), &v);
        Params::OverlaySlot_SetPadding p{{6.f, 6.f, 0.f, 0.f}};
        CallNative(s, g_fn.overPad.Get(), &p);
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
        CallNative(t, g_fn.setFont.Get(), &f);
        UPanelSlot* s = Add(ref->Slot->Parent, t);
        if (!s || !s->IsA(UVerticalBoxSlot::StaticClass())) return t;
        Params::VerticalBoxSlot_SetHorizontalAlignment h{EHorizontalAlignment::HAlign_Center};
        CallNative(s, g_fn.vboxH.Get(), &h);
        return t;
    }

    const Suggestion* Find(const std::vector<Suggestion>& all, UWidgetItemIconContainer_C* c) {
        Params::FItemContainerFunctions_C_ConvertCompressedItemSlot p{};
        p.CompressedItemSlot = c->CompressedItemSlot;
        p.__WorldContext = c;
        UFunction* fn = g_fn.convert.Get();
        if (!fn) return nullptr;
        UFItemContainerFunctions_C::GetDefaultObj()->ProcessEvent(fn, &p);
        for (const Suggestion& s : all)
            if (s.bank == c->IsStorage && s.slot == p.ItemSlot && s.containerType == uint8(p.ContainerType)) return &s;
        return nullptr;
    }

    // O(slots on the tracked bags x suggestions); runs only after a bag event or a profile change.
    void UpdateSlots(const std::vector<Suggestion>& all) {
        std::erase_if(g_badges, [](const Badge& b) { return !b.slot.Get() || !b.img.Get(); });
        for (const ref::Ref& t : g_bags) {
            auto* bag = t.Get<UWidgetItemBag_C>();
            if (!bag) continue;
            auto& slots = bag->ItemContainers;
            for (int k = 0; k < slots.Num(); k++) {
                UWidgetItemIconContainer_C* c = slots[k];
                if (!umg::Live(c) || !PtrOk(c->Overlay_Container) || !PtrOk(c->WidgetTree)) continue;
                Badge* b = nullptr;
                for (Badge& x : g_badges) if (x.slot.ptr == c) b = &x;  // all live: dead ones were erased above
                const Suggestion* s = Find(all, c);
                if (!b) {
                    if (!s) continue;  // badges are created on first need only
                    UImage* img = Existing(c->Overlay_Container);
                    if (!img) img = NewBadge(c);
                    if (!img) continue;
                    b = &g_badges.emplace_back(Badge{ref::Ref(c), ref::Ref(img), -2, {}});
                }
                const int want = s ? int(s->action) : -1;
                if (s) b->reason = s->reason;
                if (want == b->shown) continue;
                auto* img = b->img.Get<UImage>();
                if (want >= 0) SetTexture(img, Tex(want));
                SetVis(img, want >= 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
                b->shown = want;
            }
        }
    }

    // The details panel shows the item in LoadedItemUIData; the same item's slot widget tells whether it is suggested.
    void UpdateLine(Line& l) {
        auto* detail = l.detail.Get<UWidgetItemDisplayDetail_C>();
        auto* text = l.text.Get<UTextBlock>();
        if (!detail || !text) return;
        const FSItemUIData& it = detail->LoadedItemUIData;
        std::string want;
        for (const Badge& b : g_badges) {
            auto* slot = b.slot.Get<UWidgetItemIconContainer_C>();
            if (b.shown < 0 || !slot) continue;
            const FSItemUIData& s = slot->ItemData;
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
            CallNative(text, g_fn.setText.Get(), &p);
        }
        SetVis(text, want.empty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
        l.shown = std::move(want);
    }

    void Track(std::vector<ref::Ref>& v, UObject* w) {
        for (const ref::Ref& t : v) if (t.Is(w)) return;
        v.push_back(ref::Ref(w));
        g_texRetry = true;
    }

    // Per event O(1) plus the lines of one detail panel; the world-tick work only runs when something changed.
    void OnEvent(void* objp, void* fnp, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !PtrOk(objp) || !game::OnGameThread()) return;  // shared state, UFunction calls and ref resolution: game thread only
        t_busy = true;
        auto* obj = static_cast<UObject*>(objp);
        if (g_fn.detailTick.Is(fnp)) {  // O(lines + badges on screen)
            static cost::Path path{"item-sell details tick"};
            cost::Scope cs(path);
            bool known = false;
            for (Line& l : g_lines) if (l.detail.Is(obj)) { UpdateLine(l); known = true; }
            for (const ref::Ref& t : g_noLine) known |= t.Is(obj);
            if (!known) Track(g_newDetails, obj);
        } else {
            UClass* c = obj->Class;
            UClass* bagCls = g_bagCls.Get();
            UObject* bag = !bagCls                  ? nullptr
                         : c == bagCls              ? obj
                         : c == g_invCls.Get()      ? static_cast<UWidgetItemInventory_C*>(obj)->widget_ItemBag
                         : c == g_storCls.Get()     ? static_cast<UWidgetItemStorage_C*>(obj)->widget_ItemStorageBag
                                                    : nullptr;
            if (PtrOk(bag) && bag->Class == bagCls) {  // O(open bags)
                static cost::Path path{"item-sell bag event"};
                cost::Scope cs(path);
                Track(g_bags, bag);
                g_dirty = true;
            }
        }
        if (umg::IsWorldTick(fnp)) {  // adds widgets: world tick only (#50)
            const int profile = items::profiles::ActiveIndex();
            if (profile != g_profile) g_dirty = !g_bags.empty();
            g_profile = profile;
            if (!g_texSearched && g_texRetry) FindTextures();
            g_texRetry = false;
            if (g_texSearched && g_dirty) {
                LARGE_INTEGER t0, t1, f;
                QueryPerformanceCounter(&t0);
                std::erase_if(g_bags, [](const ref::Ref& t) { return !t.Get(); });
                const auto& all = item_sell::api::Suggested();
                UpdateSlots(all);
                QueryPerformanceCounter(&t1);
                QueryPerformanceFrequency(&f);
                static size_t logged[3] = {~size_t(0)};  // log on change only: bag events fire on every hover
                if (const size_t now[3] = {g_bags.size(), g_badges.size(), all.size()}; !std::equal(now, now + 3, logged)) {
                    std::copy(now, now + 3, logged);
                    char buf[128];
                    std::snprintf(buf, sizeof buf, "[item-sell] badges: %zu bags, %zu badges, %zu suggestions, %.2f ms", now[0], now[1], now[2],
                                  double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart));
                    logger::log(buf);
                }
            }
            g_dirty = false;
            if (g_texSearched && !g_newDetails.empty()) {
                std::erase_if(g_noLine, [](const ref::Ref& t) { return !t.Get(); });
                std::erase_if(g_lines, [](const Line& l) { return !l.detail.Get() || !l.text.Get(); });
                for (const ref::Ref& t : g_newDetails) {
                    auto* d = t.Get<UWidgetItemDisplayDetail_C>();
                    if (!d || !PtrOk(d->WidgetTree)) continue;
                    if (UTextBlock* tb = NewLine(d)) g_lines.push_back({ref::Ref(d), ref::Ref(tb), {}});
                    else g_noLine.push_back(t);
                }
                g_newDetails.clear();
            }
        }
        t_busy = false;
    }
}

namespace item_sell::badges {
    void Frame() {
        if (g_on.load()) return;
        g_on = true;  // game functions resolve on use (ref::Fn; Blueprint classes are null at the main menu, #54)
        game::SetEventListener(&OnEvent, true);
    }
    // ponytail: shown badges stay (inert) until the game rebuilds its menus; hiding needs the game thread
    void Off() {
        g_on = false;
        game::SetEventListener(&OnEvent, false);
        g_badges.clear();
        g_lines.clear();
        g_bags.clear();
        g_newDetails.clear();
        g_noLine.clear();
    }
}
