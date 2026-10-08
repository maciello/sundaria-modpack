#include "item-sell.hpp"
#include "inventory-badges.hpp"
#include "feature.hpp"

// STUB until the item-sell logic lands (#23): two fixed preview suggestions so the badges can be seen.
namespace item_sell::api {
    std::vector<Suggestion> Suggested() {
        return {{false, 0, 0, Action::Sell, "preview, logic pending (#23)"},
                {false, 0, 1, Action::Salvage, "preview, logic pending (#23)"}};
    }
}

namespace {
    struct ItemSell : feature::Feature {
        ItemSell() : Feature("Item sell", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook
        void OnFrame(const feature::Frame&) override { item_sell::badges::Frame(); }
        void Off() override { item_sell::badges::Off(); }
    } g_feature;
}
