#include "item-upgrade.hpp"
#include "scores.hpp"
#include "../shared/items.hpp"
#include "../shared/tiles.hpp"
#include "../shared/marks.hpp"
#include "cost.hpp"
#include "feature.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "ref.hpp"
#include "style.hpp"
#include "umg.hpp"

#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>

// Item upgrade marks (#117, Alpha, opt-in): on each equipable item slot of the game's inventory and bank, a chip in the
// game's count-chip style: "+4.2%" / "-4.2%" (DPS change for the current hero, game compare green / red) or another hero's name (accent
// orange); the item details panel says the same in lines like its "Learned" line, plus best in slot.
// Spec: references/design-system.md § Item upgrade marks. Game thread only; nothing is searched for: bags and details
// panels come from the game's own events (shared/tiles.hpp), widgets are removed through a world tick (shared/marks.hpp).
namespace {
    using namespace item_upgrade;
    namespace tiles = items::tiles;
    namespace marks = items::marks;

    constexpr marks::Chip kChip{kChipFont, kChipPadL, kChipPadT, kChipPadR, kChipPadB, kChipInset};

    struct Tile {
        ref::Ref slot, box, text;  // UWidgetItemIconContainer_C, our UBorder + UTextBlock (box null: none made yet)
        tiles::Shown shown;        // item the verdict is for
        Mark mark = Mark::None;
        std::string chip, lines;   // shown
    };
    struct Line {
        ref::Ref detail, text;  // UWidgetItemDisplayDetail_C, UTextBlock
        std::string shown;
    };
    marks::Set g_marks;
    std::vector<Tile> g_tiles;
    std::vector<Line> g_lines;
    std::vector<ref::Ref> g_bags, g_newDetails, g_noLine;
    bool g_dirty = false;
    std::atomic<bool> g_on{false};
    thread_local bool t_busy = false;

    void Show(Tile& t, const Verdict& v) {
        const std::string chip = Chip(v);
        t.lines = Lines(v);
        if (scores::Preview() && !t.lines.empty()) t.lines = "Preview, no DPS data yet (#118)\n" + t.lines;
        if (chip.empty() && !t.box.Get()) { t.mark = v.mark; t.chip.clear(); return; }  // chips are made on first need only
        if (!t.box.Get()) {
            const marks::ChipWidgets w = g_marks.ChipIn(t.slot.Get<void>(), kChip);
            if (!w.box) { t.shown = {}; return; }  // no item icon to copy the style from yet: retried next refresh
            t.box = ref::Ref(w.box);
            t.text = ref::Ref(w.text);
            t.mark = Mark::None;
            t.chip.clear();
        }
        void* text = t.text.Get<void>();
        if (!text) return;
        if (v.mark != t.mark && v.mark != Mark::None) marks::SetColor(text, v.mark == Mark::Upgrade ? style::color::kGamePositive : v.mark == Mark::Downgrade ? style::color::kGameNegative : style::color::kAccent);
        if (chip != t.chip && !chip.empty()) marks::SetText(text, chip);
        if (chip.empty() != t.chip.empty()) marks::SetVisible(t.box.Get<void>(), !chip.empty());
        t.mark = v.mark;
        t.chip = chip;
    }

    // O(slots on the open bags); a slot whose item is unchanged is skipped unless the scores changed.
    void UpdateSlots(bool all) {
        std::erase_if(g_tiles, [](const Tile& t) { return !t.slot.Get() || (t.box.ptr && !t.box.Get()); });
        int marked = 0, upgrades = 0, scored = 0;
        for (void* c : tiles::Slots(g_bags)) {
            Tile* t = nullptr;
            for (Tile& x : g_tiles) if (x.slot.ptr == c) t = &x;
            const tiles::Shown now = tiles::OfSlot(c);
            if (!t) t = &g_tiles.emplace_back(Tile{ref::Ref(c), {}, {}, {}, Mark::None, {}, {}});
            else if (!all && t->shown == now) { marked += t->mark != Mark::None; upgrades += t->mark == Mark::Upgrade; continue; }
            t->shown = now;
            tiles::Pos p;
            const Verdict v = tiles::Locate(c, p) ? scores::For(p) : Verdict{};
            scored++;
            Show(*t, v);
            marked += v.mark != Mark::None;
            upgrades += v.mark == Mark::Upgrade;
        }
        static int logged[3] = {-1};
        if (const int now[3] = {int(g_tiles.size()), marked, upgrades}; !std::equal(now, now + 3, logged)) {
            std::copy(now, now + 3, logged);
            logger::log("[item-upgrade] " + std::to_string(now[0]) + " slots, " + std::to_string(marked) + " marks (" + std::to_string(upgrades) +
                        " upgrades), " + std::to_string(scored) + " evaluated; " + scores::Status());
        }
    }

    void UpdateLine(Line& l) {
        void* detail = l.detail.Get<void>();
        void* text = l.text.Get<void>();
        if (!detail || !text) return;
        const tiles::Shown it = tiles::OfDetail(detail);
        std::string want;
        for (const Tile& t : g_tiles)
            if (!t.lines.empty() && t.shown == it && t.slot.Get()) { want = t.lines; break; }
        if (want == l.shown) return;
        if (!want.empty()) marks::SetText(text, want);
        marks::SetVisible(text, !want.empty());
        l.shown = std::move(want);
    }

    void ClearState() {
        g_tiles.clear();
        g_lines.clear();
        g_bags.clear();
        g_newDetails.clear();
        g_noLine.clear();
    }

    void OnEvent(void* obj, void* fn, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !umg::PtrOk(obj) || !game::OnGameThread()) return;
        t_busy = true;
        if (umg::IsWorldTick(fn) && g_marks.Serve()) { ClearState(); t_busy = false; return; }
        void* what = nullptr;
        switch (tiles::Classify(obj, fn, &what)) {
        case tiles::Ev::Detail: {  // O(lines + slots on screen)
            static cost::Path path{"item-upgrade details tick"};
            cost::Scope cs(path);
            bool known = false;
            for (Line& l : g_lines) if (l.detail.Is(what)) { UpdateLine(l); known = true; }
            for (const ref::Ref& t : g_noLine) known |= t.Is(what);
            if (!known) tiles::Note(g_newDetails, what);
            break;
        }
        case tiles::Ev::Bag:  // O(open bags)
            tiles::Note(g_bags, what);
            g_dirty = true;
            break;
        case tiles::Ev::Other: break;
        }
        if (umg::IsWorldTick(fn)) {  // adds widgets: world tick only (#50)
            if (g_dirty) {
                static cost::Path path{"item-upgrade refresh"};
                cost::Scope cs(path);
                tiles::Prune(g_bags);
                bool changed = false;
                if (!g_bags.empty() && scores::Refresh(changed)) UpdateSlots(changed);
            }
            g_dirty = false;
            if (!g_newDetails.empty()) {
                tiles::Prune(g_noLine);
                std::erase_if(g_lines, [](const Line& l) { return !l.detail.Get() || !l.text.Get(); });
                for (const ref::Ref& t : g_newDetails) {
                    void* d = t.Get<void>();
                    if (!d) continue;
                    if (void* tb = g_marks.Line(d)) g_lines.push_back({ref::Ref(d), ref::Ref(tb), {}});
                    else g_noLine.push_back(t);
                }
                g_newDetails.clear();
            }
        }
        t_busy = false;
    }

    struct ItemUpgrade : feature::Feature {
        ItemUpgrade() : Feature("Item upgrade", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook
        void OnFrame(const feature::Frame&) override {
            items::io::Tick();
            if (g_on.load()) return;
            g_on = true;  // game functions resolve on use (ref::Fn; Blueprint classes are null at the main menu, #54)
            tiles::Listen(&OnEvent, true);
        }
        // Runs with the ProcessEvent hook alive (#84): the next world tick removes the chips and lines.
        void Off() override {
            g_marks.Release("item-upgrade");
            g_on = false;
            tiles::Listen(&OnEvent, false);
            ClearState();
            scores::Reset();
        }
    } g_feature;
}
