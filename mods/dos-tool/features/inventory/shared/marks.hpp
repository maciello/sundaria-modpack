#pragma once
// Our marks on the game's item slots and details panels (tiles.hpp): game widgets styled like the game's own, owned
// per feature and removed through a world tick in Off() (#84). Game thread only. Handles are SDK pointers as void*.
// Specs: references/design-system.md § Suggested sell/salvage, § Item upgrade marks.
#include "drain.hpp"
#include "ref.hpp"
#include "style.hpp"
#include <initializer_list>
#include <string>
#include <vector>

namespace items::marks {
    void SetVisible(void* widget, bool on);  // HitTestInvisible / Collapsed
    void SetText(void* textBlock, const std::string& utf8);
    void SetTexture(void* image, void* texture);
    void SetColor(void* textBlock, style::Rgba c);  // sRGB token → the text block's linear colour

    struct Chip { float font, padL, padT, padR, padB, inset; };  // UMG units (slot = 100)
    struct ChipWidgets { void* box = nullptr; void* text = nullptr; };  // UBorder (show/hide this), its UTextBlock

    // Widgets one feature put into game screens.
    class Set {
        game::Drain drain_;
        std::vector<ref::Ref> made_;
        void RemoveAll();
    public:
        // World tick, first thing in the listener: true = removal ran this tick, add nothing.
        bool Serve();
        // Off(), before the listener is unsubscribed: the next world tick removes them all (bounded wait).
        void Release(const char* who);
        // UImage in the slot's top-left corner, hidden. Adopts one a previous DLL left whose texture is in `ours`.
        void* Icon(void* slot, void* texture, std::initializer_list<const void*> ours, float size, float pad);
        // The game's count chip look (WidgetItemIcon_C::Border_Count: brush, tint, font, shadow) in the slot's bottom-left
        // corner, hidden. Adopts one a previous DLL left. Empty: the slot shows no item icon to copy the style from yet.
        ChipWidgets ChipIn(void* slot, const Chip& spec);
        // TextBlock under the details panel's text, styled like its "Learned" line, hidden. null: its layout has no room.
        void* Line(void* detail);
    };
}
