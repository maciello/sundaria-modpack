#include "draw.hpp"
#include "ref.hpp"
#include "tmap.hpp"
#include "umg.hpp"

#include <algorithm>
#include <cmath>
#include "UMG_classes.hpp"
#include "UMG_parameters.hpp"
#include "WidgetMinimap_classes.hpp"

// Placement like the game's own minimap icons (game-ui.md § Minimap): anchors (0,1), alignment 0.5, position in
// map pixels; the canvas pans and turns with the map. Engine setters only.
using namespace SDK;
using umg::CallNative;
using umg::PtrOk;

namespace {
    struct Fns {
        ref::Fn addChild{UPanelWidget::StaticClass, "PanelWidget", "AddChild"};
        ref::Fn anchors{UCanvasPanelSlot::StaticClass, "CanvasPanelSlot", "SetAnchors"};
        ref::Fn align{UCanvasPanelSlot::StaticClass, "CanvasPanelSlot", "SetAlignment"};
        ref::Fn pos{UCanvasPanelSlot::StaticClass, "CanvasPanelSlot", "SetPosition"};
        ref::Fn size{UCanvasPanelSlot::StaticClass, "CanvasPanelSlot", "SetSize"};
        ref::Fn z{UCanvasPanelSlot::StaticClass, "CanvasPanelSlot", "SetZOrder"};
        ref::Fn angle{UWidget::StaticClass, "Widget", "SetRenderTransformAngle"};
        ref::Fn vis{UWidget::StaticClass, "Widget", "SetVisibility"};
        ref::Fn remove{UWidget::StaticClass, "Widget", "RemoveFromParent"};
        ref::Fn color{UImage::StaticClass, "Image", "SetColorAndOpacity"};
    } g_fn;

    struct Item {
        ref::Ref img;  // UImage
        dungeon_map::Quad last{};
        bool shown = false, placed = false;
    };
    std::vector<Item> g_pool;
    ref::Ref g_canvas;  // the CanvasPanel_DynamicMinimp the pool lives in

    float Lin(std::uint8_t c) { return std::pow(c / 255.f, 2.2f); }  // sRGB token → linear tint
    bool Same(float a, float b, float eps) { return std::abs(a - b) < eps; }

    void SetVis(UWidget* w, bool on) {
        if (!w) return;
        Params::Widget_SetVisibility p{on ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed};
        CallNative(w, g_fn.vis.Get(), &p);
    }

    UImage* Spawn(UWidgetMiniMap_C* m) {
        auto* img = static_cast<UImage*>(umg::Spawn(UImage::StaticClass(), m->WidgetTree));
        if (!img) return nullptr;
        img->Visibility = ESlateVisibility::HitTestInvisible;  // before its Slate widget exists: a plain write holds
        Params::PanelWidget_AddChild a{};
        a.Content = img;
        CallNative(m->CanvasPanel_DynamicMinimp, g_fn.addChild.Get(), &a);
        if (!PtrOk(a.ReturnValue) || !a.ReturnValue->IsA(UCanvasPanelSlot::StaticClass())) return nullptr;
        Params::CanvasPanelSlot_SetAnchors an{};
        an.InAnchors.Minimum = {0.f, 1.f};
        an.InAnchors.Maximum = {0.f, 1.f};
        CallNative(a.ReturnValue, g_fn.anchors.Get(), &an);
        Params::CanvasPanelSlot_SetAlignment al{{0.5f, 0.5f}};
        CallNative(a.ReturnValue, g_fn.align.Get(), &al);
        return img;
    }

    void Apply(Item& it, const dungeon_map::Quad& q) {
        auto* img = it.img.Get<UImage>();
        if (!img || !PtrOk(img->Slot)) return;
        const dungeon_map::Quad& o = it.last;
        const bool all = !it.placed;
        if (all || !Same(o.x, q.x, 0.05f) || !Same(o.y, q.y, 0.05f)) {
            Params::CanvasPanelSlot_SetPosition p{{q.x, q.y}};
            CallNative(img->Slot, g_fn.pos.Get(), &p);
        }
        if (all || !Same(o.w, q.w, 0.05f) || !Same(o.h, q.h, 0.05f)) {
            Params::CanvasPanelSlot_SetSize p{{q.w, q.h}};
            CallNative(img->Slot, g_fn.size.Get(), &p);
        }
        if (all || o.z != q.z) {
            Params::CanvasPanelSlot_SetZOrder p{q.z};
            CallNative(img->Slot, g_fn.z.Get(), &p);
        }
        if (all || !Same(o.angle, q.angle, 0.05f)) {
            Params::Widget_SetRenderTransformAngle p{q.angle};
            CallNative(img, g_fn.angle.Get(), &p);
        }
        if (all || !Same(o.alpha, q.alpha, 0.01f) || o.c.r != q.c.r || o.c.g != q.c.g || o.c.b != q.c.b || o.c.a != q.c.a) {
            Params::Image_SetColorAndOpacity p{{Lin(q.c.r), Lin(q.c.g), Lin(q.c.b), q.c.a / 255.f * q.alpha}};
            CallNative(img, g_fn.color.Get(), &p);
        }
        it.last = q;
        it.placed = true;
    }

    // Images a previous DLL left in this canvas (hot reload): untextured ones that are not the game's own icons.
    void RemoveLeftovers(UWidgetMiniMap_C* m) {
        std::vector<UImage*> game, ours;
        tmap::ForEach(m->DynamicMinimapObjects, [&](UObject*, UImage* img) { game.push_back(img); });
        UCanvasPanel* c = m->CanvasPanel_DynamicMinimp;
        for (int i = 0; i < c->Slots.Num(); i++) {
            UPanelSlot* s = c->Slots[i];
            if (!PtrOk(s) || !PtrOk(s->Content) || !s->Content->IsA(UImage::StaticClass())) continue;
            auto* img = static_cast<UImage*>(s->Content);
            if (!img->Brush.ResourceObject && std::find(game.begin(), game.end(), img) == game.end()) ours.push_back(img);
        }
        for (UImage* img : ours) CallNative(img, g_fn.remove.Get(), nullptr);
    }
}

namespace dungeon_map::draw {
    void Sync(void* minimap, const std::vector<Quad>& qs) {
        auto* m = static_cast<UWidgetMiniMap_C*>(minimap);
        if (!umg::Live(m) || !PtrOk(m->CanvasPanel_DynamicMinimp) || !PtrOk(m->WidgetTree)) return;
        if (!g_canvas.Is(m->CanvasPanel_DynamicMinimp)) {  // a new minimap (map travel, hot reload): its widgets are new too
            g_pool.clear();
            RemoveLeftovers(m);
            g_canvas = ref::Ref(m->CanvasPanel_DynamicMinimp);
        }
        std::erase_if(g_pool, [](const Item& it) { return !it.img.Get(); });
        for (size_t i = 0; i < qs.size(); i++) {
            if (i == g_pool.size()) {
                UImage* img = Spawn(m);
                if (!img) return;
                g_pool.push_back({ref::Ref(img)});
                g_pool.back().shown = true;
            }
            Item& it = g_pool[i];
            Apply(it, qs[i]);
            if (!it.shown) SetVis(it.img.Get<UImage>(), it.shown = true);
        }
        for (size_t i = qs.size(); i < g_pool.size(); i++)
            if (g_pool[i].shown) SetVis(g_pool[i].img.Get<UImage>(), g_pool[i].shown = false);
    }

    void Clear() {
        for (const Item& it : g_pool)
            if (auto* img = it.img.Get<UImage>()) CallNative(img, g_fn.remove.Get(), nullptr);
        g_pool.clear();
        g_canvas = {};
    }
}
