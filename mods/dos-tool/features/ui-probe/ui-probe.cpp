#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"

#include <Windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>
#include "Engine_classes.hpp"
#include "UMG_classes.hpp"

// UI probe (dev): dumps the game's live UMG widget trees with their style data (button brushes, fonts, colours,
// paddings, slot layout) and the designer templates of loaded widget classes (+ property bindings, named slots) to dos-tool-ui.yaml next to the DLL. Agent tool: `just ui [class-substrings]` writes
// dos-tool-ui.request (its text = space-separated root class substrings); the probe answers and deletes it once a
// matching widget exists (open the screen in game), so a request may wait.
// Game thread (the GObjects walk races with the game freeing objects otherwise, #80), plain memory reads of
// UPROPERTY fields only. Feeds references/game-ui.md.
using namespace SDK;

namespace {
    bool PtrOk(const void* p) {
        const uintptr_t v = reinterpret_cast<uintptr_t>(p);
        return v > 0x10000 && v < 0x7FFFFFFFFFFFull;
    }
    std::string Dir() {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Dir), &self);
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(self, buf, MAX_PATH);
        std::string p = buf;
        return p.substr(0, p.find_last_of("\\/") + 1);
    }
    std::string Name(const UObject* o) { return PtrOk(o) ? o->GetName() : "-"; }
    std::string Cls(const UObject* o) { return PtrOk(o) && PtrOk(o->Class) ? o->Class->GetName() : "-"; }
    std::string Text(const FText& t) { return PtrOk(t.TextData) ? t.ToString() : ""; }
    std::string F(const char* fmt, ...) {
        char b[512];
        va_list a;
        va_start(a, fmt);
        std::vsnprintf(b, sizeof(b), fmt, a);
        va_end(a);
        return b;
    }
    std::string Col(const FLinearColor& c) { return F("[%.3f, %.3f, %.3f, %.3f]", c.R, c.G, c.B, c.A); }
    std::string Mar(const FMargin& m) { return F("[%.1f, %.1f, %.1f, %.1f]", m.Left, m.Top, m.Right, m.Bottom); }
    std::string Brush(const FSlateBrush& b) {
        return F("{res: '%s', cls: %s, size: [%.0f, %.0f], drawAs: %d, margin: %s, tint: %s, rule: %d}",
                 Name(b.ResourceObject).c_str(), Cls(b.ResourceObject).c_str(), b.ImageSize.X, b.ImageSize.Y, int(b.DrawAs),
                 Mar(b.Margin).c_str(), Col(b.TintColor.SpecifiedColor).c_str(), int(b.TintColor.ColorUseRule));
    }
    std::string Font(const FSlateFontInfo& f) {
        return F("{font: '%s', typeface: %s, size: %d, spacing: %d, outline: %d, outlineColor: %s}", Name(f.FontObject).c_str(),
                 f.TypefaceFontName.ToString().c_str(), f.Size, f.LetterSpacing, f.OutlineSettings.OutlineSize,
                 Col(f.OutlineSettings.OutlineColor).c_str());
    }

    std::string SlotOf(UWidget* w) {
        UPanelSlot* s = w->Slot;
        if (!PtrOk(s)) return "";
        if (s->IsA(UHorizontalBoxSlot::StaticClass())) {
            auto* h = static_cast<UHorizontalBoxSlot*>(s);
            return F("hbox {pad: %s, size: %.2f/%d, h: %d, v: %d}", Mar(h->Padding).c_str(), h->Size.Value, int(h->Size.SizeRule), int(h->HorizontalAlignment), int(h->VerticalAlignment));
        }
        if (s->IsA(UVerticalBoxSlot::StaticClass())) {
            auto* v = static_cast<UVerticalBoxSlot*>(s);
            return F("vbox {pad: %s, size: %.2f/%d, h: %d, v: %d}", Mar(v->Padding).c_str(), v->Size.Value, int(v->Size.SizeRule), int(v->HorizontalAlignment), int(v->VerticalAlignment));
        }
        if (s->IsA(UCanvasPanelSlot::StaticClass())) {
            const FAnchorData& d = static_cast<UCanvasPanelSlot*>(s)->LayoutData;
            return F("canvas {offsets: %s, anchors: [%.2f, %.2f, %.2f, %.2f], align: [%.2f, %.2f], z: %d}", Mar(d.Offsets).c_str(),
                     d.Anchors.Minimum.X, d.Anchors.Minimum.Y, d.Anchors.Maximum.X, d.Anchors.Maximum.Y, d.Alignment.X, d.Alignment.Y,
                     static_cast<UCanvasPanelSlot*>(s)->ZOrder);
        }
        if (s->IsA(UOverlaySlot::StaticClass())) return "overlay {pad: " + Mar(static_cast<UOverlaySlot*>(s)->Padding) + "}";
        if (s->IsA(UButtonSlot::StaticClass())) return "button {pad: " + Mar(static_cast<UButtonSlot*>(s)->Padding) + "}";
        if (s->IsA(UBorderSlot::StaticClass())) return "border {pad: " + Mar(static_cast<UBorderSlot*>(s)->Padding) + "}";
        if (s->IsA(UGridSlot::StaticClass())) {
            auto* g = static_cast<UGridSlot*>(s);
            return F("grid {pad: %s, row: %d, col: %d}", Mar(g->Padding).c_str(), g->Row, g->Column);
        }
        if (s->IsA(UUniformGridSlot::StaticClass())) {
            auto* g = static_cast<UUniformGridSlot*>(s);
            return F("ugrid {row: %d, col: %d}", g->Row, g->Column);
        }
        return Cls(s);
    }

    std::string Style(UWidget* w, const std::string& in) {
        std::string o;
        if (w->IsA(UButton::StaticClass())) {
            auto* b = static_cast<UButton*>(w);
            const FButtonStyle& s = b->WidgetStyle;
            o += in + "normal: " + Brush(s.Normal) + "\n" + in + "hovered: " + Brush(s.Hovered) + "\n" + in + "pressed: " + Brush(s.Pressed) + "\n";
            o += in + "disabled: " + Brush(s.Disabled) + "\n";
            o += in + F("padding: {normal: %s, pressed: %s}\n", Mar(s.NormalPadding).c_str(), Mar(s.PressedPadding).c_str());
            o += in + F("sound: {pressed: '%s', hovered: '%s'}\n", Name(s.PressedSlateSound.ResourceObject).c_str(), Name(s.HoveredSlateSound.ResourceObject).c_str());
            o += in + "color: " + Col(b->ColorAndOpacity) + ", background: " + Col(b->BackgroundColor) + ", styleAsset: '" + Name(reinterpret_cast<const UObject*>(b->Style)) + "'\n";
        } else if (w->IsA(UTextBlock::StaticClass())) {
            auto* t = static_cast<UTextBlock*>(w);
            o += in + "text: '" + Text(t->Text) + "'\n" + in + "font: " + Font(t->Font) + "\n";
            o += in + "color: " + Col(t->ColorAndOpacity.SpecifiedColor) + F(", rule: %d, justify: %d", int(t->ColorAndOpacity.ColorUseRule), int(t->Justification));
            o += F(", shadow: [%.1f, %.1f] ", t->ShadowOffset.X, t->ShadowOffset.Y) + Col(t->ShadowColorAndOpacity) + F(", transform: %d\n", int(t->TextTransformPolicy));
        } else if (w->IsA(UImage::StaticClass())) {
            auto* i = static_cast<UImage*>(w);
            o += in + "brush: " + Brush(i->Brush) + "\n" + in + "color: " + Col(i->ColorAndOpacity) + "\n";
        } else if (w->IsA(UBorder::StaticClass())) {
            auto* b = static_cast<UBorder*>(w);
            o += in + "background: " + Brush(b->Background) + "\n" + in + "brushColor: " + Col(b->BrushColor) + ", padding: " + Mar(b->Padding) + "\n";
        } else if (w->IsA(USizeBox::StaticClass())) {
            auto* s = static_cast<USizeBox*>(w);
            o += in + F("size: {w: %.0f%s, h: %.0f%s, minW: %.0f, minH: %.0f}\n", s->WidthOverride, s->bOverride_WidthOverride ? "" : "(off)",
                        s->HeightOverride, s->bOverride_HeightOverride ? "" : "(off)", s->MinDesiredWidth, s->MinDesiredHeight);
        }
        return o;
    }

    void Walk(UWidget* w, int depth, std::string& out, int& budget) {
        if (!PtrOk(w) || depth > 40 || --budget < 0) return;
        const std::string in(depth * 2, ' ');
        out += in + "- " + Name(w) + ": " + Cls(w) + F(" vis=%d", int(w->Visibility));
        const std::string slot = SlotOf(w);
        if (!slot.empty()) out += " slot=" + slot;
        out += "\n" + Style(w, in + "    ");
        if (w->IsA(UUserWidget::StaticClass())) {
            UWidgetTree* t = static_cast<UUserWidget*>(w)->WidgetTree;
            if (PtrOk(t)) Walk(t->RootWidget, depth + 1, out, budget);
        } else if (w->IsA(UPanelWidget::StaticClass())) {
            auto* p = static_cast<UPanelWidget*>(w);
            for (int i = 0; i < p->Slots.Num(); i++)
                if (PtrOk(p->Slots[i])) Walk(p->Slots[i]->Content, depth + 1, out, budget);
        }
    }

    bool Wanted(const std::string& cls, const std::string& filter) {
        size_t a = 0;
        while (a < filter.size()) {
            size_t b = filter.find(' ', a);
            if (b == std::string::npos) b = filter.size();
            if (b > a && cls.find(filter.substr(a, b - a)) != std::string::npos) return true;
            a = b + 1;
        }
        return false;
    }

    // Roots = user widgets not nested in another widget tree (added to the viewport or owned by the HUD).
    bool Dump(std::string filter) {
        if (filter.find_first_not_of(" \r\n\t") == std::string::npos) filter = "Inventory Storage Character Option Merchant Menu";
        for (char& c : filter) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        std::string out = "# dos-tool UI probe: live UMG trees. vis 0 Visible 1 Collapsed 2 Hidden 3 HitTestInvisible 4 SelfHitTestInvisible\nroots:\n";
        int roots = 0, budget = 20000;
        for (int i = 0; i < UObject::GObjects->Num(); i++) {
            UObject* o = UObject::GObjects->GetByIndex(i);
            if (!PtrOk(o) || o->IsDefaultObject()) continue;
            if (o->IsA(UWidgetBlueprintGeneratedClass::StaticClass())) {  // designer template: exists while the class is loaded
                auto* c = static_cast<UWidgetBlueprintGeneratedClass*>(o);
                if (!Wanted(Name(c), filter) || !PtrOk(c->WidgetTree)) continue;
                roots++;
                out += "- class: " + Name(c) + "  # designer template\n  bindings:\n";
                for (int k = 0; k < c->Bindings.Num(); k++)
                    out += "    - " + c->Bindings[k].ObjectName.ToString() + "." + c->Bindings[k].PropertyName.ToString() + " <- " +
                           c->Bindings[k].FunctionName.ToString() + "\n";
                out += "  namedSlots:";
                for (int k = 0; k < c->NamedSlots.Num(); k++) out += " " + c->NamedSlots[k].ToString();
                out += "\n  tree:\n";
                Walk(c->WidgetTree->RootWidget, 2, out, budget);
                continue;
            }
            if (!o->IsA(UUserWidget::StaticClass())) continue;
            const std::string cls = Cls(o);
            if (!Wanted(cls, filter)) continue;
            UObject* tree = o->Outer;  // nested in a matching user widget: printed under that one
            if (PtrOk(tree) && tree->IsA(UWidgetTree::StaticClass()) && Wanted(Cls(tree->Outer), filter)) continue;
            roots++;
            out += F("- root: %s  # outer %s%s\n", cls.c_str(), Cls(o->Outer).c_str(), (int(o->Flags) & 0x20) ? ", archetype (class template, not live)" : "");
            Walk(static_cast<UWidget*>(o), 1, out, budget);
        }
        if (!roots) return false;  // not open yet: keep the request, retry
        HANDLE h = CreateFileA((Dir() + "dos-tool-ui.yaml").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD n = 0;
            WriteFile(h, out.data(), DWORD(out.size()), &n, nullptr);
            CloseHandle(h);
        }
        logger::log(F("[ui-probe] %d roots (filter '%s'), %d bytes -> dos-tool-ui.yaml", roots, filter.c_str(), int(out.size())));
        return true;
    }

    // render thread → game thread: 0 idle, 1 asked (g_filter), 2 dumped, 3 no matching root yet
    std::atomic<int> g_state{0};
    std::string g_filter;  // written before g_state = 1

    void OnEvent(void*, void*, void*) {
        if (game::OnGameThread() && g_state.load() == 1) g_state = Dump(g_filter) ? 2 : 3;
    }

    struct UiProbe : feature::Feature {
        double next = 0;
        bool listening = false;
        UiProbe() : Feature("UI probe", feature::Stage::Alpha) {}  // Alpha: dev installs only; idle until requested

        void Listen(bool on) {
            if (listening != on) game::OnGameTick(&OnEvent, listening = on);
        }

        void OnFrame(const feature::Frame& f) override {
            if (f.now < next) return;
            next = f.now + 0.5;
            const int st = g_state.load();
            if (st == 1) return;
            const std::string req = Dir() + "dos-tool-ui.request";
            if (st == 2) { DeleteFileA(req.c_str()); g_state = 0; Listen(false); return; }
            HANDLE h = CreateFileA(req.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) { g_state = 0; Listen(false); return; }  // request withdrawn
            char buf[512] = {};
            DWORD n = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
            CloseHandle(h);
            g_filter.assign(buf, n);
            g_state = 1;  // retried every 0.5 s until the screen is open; the listener stays meanwhile
            Listen(true);
        }

        void Off() override { Listen(false); g_state = 0; }
    } g_ui_probe;
}
