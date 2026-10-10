#include "inventory-badges.hpp"
#include "item-sell.hpp"
#include "../shared/tiles.hpp"
#include "../shared/marks.hpp"
#include "game.hpp"
#include "logger.hpp"
#include "umg.hpp"
#include "ref.hpp"
#include "cost.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

// Each suggested item slot gets the game's own action icon (Tooltip_Sell / Tooltip_Salvage, the icon the game shows
// for an item's action) in its top-left corner. The game's item details panel gets one line under its text,
// styled like the panel's own orange "Learned" line: "Sell suggested: <reason>".
// Game thread only. Slots, panels and widgets: ../shared/tiles.hpp, marks.hpp.
namespace {
    using item_sell::api::Suggestion;
    namespace tiles = items::tiles;
    namespace marks = items::marks;

    marks::Set g_marks;  // Off(): the game thread removes our badges and lines
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
    void* Tex(int action) {
        void* t = g_tex[action].Get<void>();
        if (!t) g_texSearched = false;
        return t;
    }

    const Suggestion* Find(const std::vector<Suggestion>& all, void* slot) {
        tiles::Pos p;
        if (!tiles::Locate(slot, p)) return nullptr;
        for (const Suggestion& s : all)
            if (s.bank == p.bank && s.slot == p.slot && s.containerType == p.containerType) return &s;
        return nullptr;
    }

    // O(slots on the tracked bags x suggestions); runs only after a bag event or a profile change.
    void UpdateSlots(const std::vector<Suggestion>& all) {
        std::erase_if(g_badges, [](const Badge& b) { return !b.slot.Get() || !b.img.Get(); });
        for (void* c : tiles::Slots(g_bags)) {
            Badge* b = nullptr;
            for (Badge& x : g_badges) if (x.slot.ptr == c) b = &x;  // all live: dead ones were erased above
            const Suggestion* s = Find(all, c);
            if (!b) {
                if (!s) continue;  // badges are created on first need only
                void* img = g_marks.Icon(c, Tex(0), {g_tex[0].Get<void>(), g_tex[1].Get<void>()}, 32.f, 6.f);
                if (!img) continue;
                b = &g_badges.emplace_back(Badge{ref::Ref(c), ref::Ref(img), -2, {}});
            }
            const int want = s ? int(s->action) : -1;
            if (s) b->reason = s->reason;
            if (want == b->shown) continue;
            void* img = b->img.Get<void>();
            if (want >= 0) marks::SetTexture(img, Tex(want));
            marks::SetVisible(img, want >= 0);
            b->shown = want;
        }
    }

    // The details panel shows the item in LoadedItemUIData; the same item's slot widget tells whether it is suggested.
    void UpdateLine(Line& l) {
        void* detail = l.detail.Get<void>();
        void* text = l.text.Get<void>();
        if (!detail || !text) return;
        const tiles::Shown it = tiles::OfDetail(detail);
        std::string want;
        for (const Badge& b : g_badges) {
            void* slot = b.slot.Get<void>();
            if (b.shown < 0 || !slot || !(tiles::OfSlot(slot) == it)) continue;
            want = std::string(b.shown == int(item_sell::api::Action::Salvage) ? "Salvage" : "Sell") + " suggested: " + b.reason;
            break;
        }
        if (want == l.shown) return;
        if (!want.empty()) marks::SetText(text, want);
        marks::SetVisible(text, !want.empty());
        l.shown = std::move(want);
    }

    void ClearState() {
        g_badges.clear();
        g_lines.clear();
        g_bags.clear();
        g_newDetails.clear();
        g_noLine.clear();
    }

    // Per event O(1) plus the lines of one detail panel; the world-tick work only runs when something changed.
    void OnEvent(void* objp, void* fnp, void*) {
        if (t_busy || !g_on.load(std::memory_order_relaxed) || !umg::PtrOk(objp) || !game::OnGameThread()) return;  // shared state, UFunction calls and ref resolution: game thread only
        t_busy = true;
        if (umg::IsWorldTick(fnp) && g_marks.Serve()) { ClearState(); t_busy = false; return; }
        void* what = nullptr;
        switch (tiles::Classify(objp, fnp, &what)) {
        case tiles::Ev::Detail: {  // O(lines + badges on screen)
            static cost::Path path{"item-sell details tick"};
            cost::Scope cs(path);
            bool known = false;
            for (Line& l : g_lines) if (l.detail.Is(what)) { UpdateLine(l); known = true; }
            for (const ref::Ref& t : g_noLine) known |= t.Is(what);
            if (!known && tiles::Note(g_newDetails, what)) g_texRetry = true;
            break;
        }
        case tiles::Ev::Bag: {  // O(open bags)
            static cost::Path path{"item-sell bag event"};
            cost::Scope cs(path);
            if (tiles::Note(g_bags, what)) g_texRetry = true;
            g_dirty = true;
            break;
        }
        case tiles::Ev::Equip: g_dirty = true; break;
        case tiles::Ev::Other: break;
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
                tiles::Prune(g_bags);
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
}

namespace item_sell::badges {
    void Frame() {
        if (g_on.load()) return;
        g_on = true;  // game functions resolve on use (ref::Fn; Blueprint classes are null at the main menu, #54)
        tiles::Listen(&OnEvent, true);
    }
    // Runs with the ProcessEvent hook alive (#84): the next world tick removes the badges and lines.
    void Off() {
        g_marks.Release("item-sell");
        g_on = false;
        tiles::Listen(&OnEvent, false);
        ClearState();
    }
}
