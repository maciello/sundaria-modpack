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
        // TextBlock under the details panel's text, styled like its "Learned" line, hidden. null: its layout has no room.
        void* Line(void* detail);
    };
}
