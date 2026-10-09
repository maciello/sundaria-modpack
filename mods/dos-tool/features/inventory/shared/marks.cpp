#include "marks.hpp"
#include "umg.hpp"

#include <cmath>
#include "UMG_classes.hpp"
#include "UMG_parameters.hpp"
#include "WidgetItemIcon_classes.hpp"
#include "WidgetItemIconContainer_classes.hpp"
#include "WidgetItemDisplayDetail_classes.hpp"

// Game widgets added into the game's item slots and details panels. Game thread, world tick only (#50).
// Styling: copy through the game's setters, never byte-copy a brush or font (#61). Facts: references/game-ui.md.
using namespace SDK;
using umg::CallNative;
using umg::PtrOk;

namespace {
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
        ref::Fn remove{UWidget::StaticClass, "Widget", "RemoveFromParent"};
        ref::Fn setColor{UTextBlock::StaticClass, "TextBlock", "SetColorAndOpacity"};
        ref::Fn borderTex{UBorder::StaticClass, "Border", "SetBrushFromTexture"};
    } g_fn;

    UPanelSlot* Add(UPanelWidget* panel, UWidget* w) {
        Params::PanelWidget_AddChild a{};
        a.Content = w;
        CallNative(panel, g_fn.addChild.Get(), &a);
        return PtrOk(a.ReturnValue) ? a.ReturnValue : nullptr;
    }
    void Corner(UPanelSlot* s, EHorizontalAlignment h, EVerticalAlignment v, FMargin pad) {
        Params::OverlaySlot_SetHorizontalAlignment ph{h};
        CallNative(s, g_fn.overH.Get(), &ph);
        Params::OverlaySlot_SetVerticalAlignment pv{v};
        CallNative(s, g_fn.overV.Get(), &pv);
        Params::OverlaySlot_SetPadding pp{pad};
        CallNative(s, g_fn.overPad.Get(), &pp);
    }
}

namespace items::marks {
    void SetVisible(void* w, bool on) {
        Params::Widget_SetVisibility p{on ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed};
        CallNative(static_cast<UObject*>(w), g_fn.setVis.Get(), &p);
    }
    void SetText(void* t, const std::string& utf8) {
        Params::TextBlock_SetText p{};
        p.InText = umg::Text(utf8);
        CallNative(static_cast<UObject*>(t), g_fn.setText.Get(), &p);
    }
    void SetTexture(void* img, void* tex) {
        Params::Image_SetBrushFromTexture p{};
        p.Texture = static_cast<UTexture2D*>(tex);
        CallNative(static_cast<UObject*>(img), g_fn.setTex.Get(), &p);
    }

    void Set::RemoveAll() {
        for (const ref::Ref& r : made_) if (UObject* w = r.Get()) CallNative(w, g_fn.remove.Get(), nullptr);
        made_.clear();
    }
    bool Set::Serve() { return drain_.Serve([this] { RemoveAll(); }); }
    void Set::Release(const char* who) { drain_.Request(!made_.empty(), who, [this] { RemoveAll(); }); }

    void* Set::Icon(void* slot, void* texture, std::initializer_list<const void*> ours, float size, float pad) {
        auto* c = static_cast<UWidgetItemIconContainer_C*>(slot);
        UOverlay* o = c->Overlay_Container;
        for (int i = 0; i < o->Slots.Num(); i++) {  // left by a previous DLL before a hot reload
            UPanelSlot* s = o->Slots[i];
            if (!PtrOk(s) || !PtrOk(s->Content) || !s->Content->IsA(UImage::StaticClass())) continue;
            const UObject* res = static_cast<UImage*>(s->Content)->Brush.ResourceObject;
            for (const void* t : ours)
                if (res && res == t) { made_.push_back(ref::Ref(s->Content)); return s->Content; }
        }
        auto* img = static_cast<UImage*>(umg::Spawn(UImage::StaticClass(), c->WidgetTree));
        if (!img) return nullptr;
        img->Brush.ImageSize = {size, size};  // before the Slate widget exists: plain writes take effect
        img->Visibility = ESlateVisibility::Collapsed;
        SetTexture(img, texture);
        UPanelSlot* s = Add(o, img);
        if (!s) return nullptr;
        Corner(s, EHorizontalAlignment::HAlign_Left, EVerticalAlignment::VAlign_Top, {pad, pad, 0.f, 0.f});
        made_.push_back(ref::Ref(img));
        return img;
    }

    void SetColor(void* t, style::Rgba c) {
        auto lin = [](std::uint8_t v) { return std::pow(v / 255.f, 2.2f); };  // sRGB token → linear (UMG colours are linear)
        Params::TextBlock_SetColorAndOpacity p{};
        p.InColorAndOpacity.SpecifiedColor = {lin(c.r), lin(c.g), lin(c.b), c.a / 255.f};
        p.InColorAndOpacity.ColorUseRule = ESlateColorStylingMode::UseColor_Specified;
        CallNative(static_cast<UObject*>(t), g_fn.setColor.Get(), &p);
    }

    ChipWidgets Set::ChipIn(void* slot, const Chip& spec) {
        auto* c = static_cast<UWidgetItemIconContainer_C*>(slot);
        UOverlay* o = c->Overlay_Container;
        for (int i = 0; i < o->Slots.Num(); i++) {  // ours = a UBorder holding a UTextBlock (the game's Border_IconArea holds the icon)
            UPanelSlot* s = o->Slots[i];
            if (!PtrOk(s) || !PtrOk(s->Content) || !s->Content->IsA(UBorder::StaticClass())) continue;
            auto* b = static_cast<UBorder*>(s->Content);
            if (b->Slots.Num() != 1 || !PtrOk(b->Slots[0]) || !PtrOk(b->Slots[0]->Content) || !b->Slots[0]->Content->IsA(UTextBlock::StaticClass())) continue;
            made_.push_back(ref::Ref(b));
            return {b, b->Slots[0]->Content};
        }
        // the style source: the item icon's own count chip
        UBorder* area = c->Border_IconArea;
        if (!PtrOk(area) || area->Slots.Num() != 1 || !PtrOk(area->Slots[0])) return {};
        UWidget* w = area->Slots[0]->Content;
        if (!PtrOk(w) || !w->IsA(UWidgetItemIcon_C::StaticClass())) return {};
        auto* icon = static_cast<UWidgetItemIcon_C*>(w);
        UBorder* srcBox = icon->Border_Count;
        UTextBlock* srcText = icon->TextBlock_Count;
        if (!PtrOk(srcBox) || !PtrOk(srcText) || !PtrOk(srcBox->Background.ResourceObject) ||
            !srcBox->Background.ResourceObject->IsA(UTexture2D::StaticClass())) return {};

        auto* box = static_cast<UBorder*>(umg::Spawn(UBorder::StaticClass(), c->WidgetTree));
        auto* text = static_cast<UTextBlock*>(umg::Spawn(UTextBlock::StaticClass(), c->WidgetTree));
        if (!box || !text) return {};
        // before the Slate widgets exist: plain values take effect (brush/font objects go through setters, #61)
        box->Background.DrawAs = srcBox->Background.DrawAs;
        box->Background.Margin = srcBox->Background.Margin;
        box->Background.TintColor.SpecifiedColor = srcBox->Background.TintColor.SpecifiedColor;
        box->BrushColor = srcBox->BrushColor;
        box->Padding = {spec.padL, spec.padT, spec.padR, spec.padB};
        box->Visibility = ESlateVisibility::Collapsed;
        Params::Border_SetBrushFromTexture bt{static_cast<UTexture2D*>(srcBox->Background.ResourceObject)};
        CallNative(box, g_fn.borderTex.Get(), &bt);
        text->ShadowOffset = srcText->ShadowOffset;
        text->ShadowColorAndOpacity = srcText->ShadowColorAndOpacity;
        Params::TextBlock_SetFont f{};
        f.InFontInfo = srcText->Font;
        f.InFontInfo.Size = int32(spec.font);
        CallNative(text, g_fn.setFont.Get(), &f);
        if (!Add(box, text)) return {};
        UPanelSlot* s = Add(o, box);
        if (!s) return {};
        Corner(s, EHorizontalAlignment::HAlign_Left, EVerticalAlignment::VAlign_Bottom, {spec.inset, 0.f, 0.f, spec.inset});
        made_.push_back(ref::Ref(box));
        return {box, text};
    }

    void* Set::Line(void* detail) {
        auto* d = static_cast<UWidgetItemDisplayDetail_C*>(detail);
        UTextBlock* like = d->TextBlock_AlreadyLearned;
        if (!PtrOk(d->WidgetTree) || !PtrOk(like) || !PtrOk(like->Slot) || !PtrOk(like->Slot->Parent)) return nullptr;
        auto* t = static_cast<UTextBlock*>(umg::Spawn(UTextBlock::StaticClass(), d->WidgetTree));
        if (!t) return nullptr;
        t->ColorAndOpacity.SpecifiedColor = like->ColorAndOpacity.SpecifiedColor;
        t->ShadowOffset = like->ShadowOffset;
        t->ShadowColorAndOpacity = like->ShadowColorAndOpacity;
        t->bAutoWrapText = true;
        t->Visibility = ESlateVisibility::Collapsed;
        Params::TextBlock_SetFont f{};
        f.InFontInfo = like->Font;
        CallNative(t, g_fn.setFont.Get(), &f);
        UPanelSlot* s = Add(like->Slot->Parent, t);
        made_.push_back(ref::Ref(t));
        if (!s || !s->IsA(UVerticalBoxSlot::StaticClass())) return t;
        Params::VerticalBoxSlot_SetHorizontalAlignment h{EHorizontalAlignment::HAlign_Center};
        CallNative(s, g_fn.vboxH.Get(), &h);
        return t;
    }
}
