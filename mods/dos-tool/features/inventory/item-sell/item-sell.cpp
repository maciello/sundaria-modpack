#include "item-sell.hpp"
#include "inventory-badges.hpp"
#include "feature.hpp"
#include "logger.hpp"

#include <Windows.h>

// Suggested sell/salvage (#23): an owned item (inventory, bank, equipped) at least as good as this one in the same
// slot, keeping the best of each slot (item-sell.hpp). Game thread only: recomputed when the bag, the bank or the
// active profile changes (raw-record hash), otherwise the cached list. One log line per recompute with 5 examples.
namespace {
    using namespace item_sell;
    namespace io = items::io;

    std::uint64_t g_sig = 0;
    std::vector<api::Suggestion> g_cache;

    std::string Place(const Item& it) { return it.where == items::Where::Bank ? "bank" : it.where == items::Where::Equipped ? "equipped" : "bag"; }
}

namespace item_sell::api {
    std::vector<Suggestion> Suggested() {
        if (!io::Ready()) return {};
        const io::Located l = io::Locate();
        const int active = items::profiles::ActiveIndex();
        const std::uint64_t sig = io::Signature(l.bag) ^ (io::Signature(l.bank) * 31) ^ (std::uint64_t(active) << 56) ^ 1;
        if (sig == g_sig) return g_cache;
        g_sig = sig;
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);

        std::vector<Item> owned;
        for (Item& it : io::Read(l.bag, false, true))
            if (it.where == items::Where::Inventory || it.where == items::Where::Equipped) owned.push_back(std::move(it));
        const io::Bank bank = io::ReadBank(true);
        owned.insert(owned.end(), bank.items.begin(), bank.items.end());
        const items::Profile prof = items::profiles::Active();
        const std::vector<Pick> picks = Suggest(owned, prof, io::GetNames().stat);

        g_cache.clear();
        std::string ex;
        for (const Pick& p : picks) {
            const Item &it = owned[p.item], &by = owned[p.by];
            const std::string reason = "worse than " + by.name + " (lv " + std::to_string(by.level) + ", " + Place(by) + ")";
            g_cache.push_back({it.bank, it.containerType, it.slot, io::CanSalvage(it.specId) ? Action::Salvage : Action::Sell, reason});
            if (g_cache.size() <= 5)
                ex += " | " + it.name + " lv " + std::to_string(it.level) + " " + Place(it) + " slot " + std::to_string(it.slot) +
                      (g_cache.back().action == Action::Salvage ? " salvage: " : " sell: ") + reason;
        }
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        logger::log("[item-sell] " + std::to_string(g_cache.size()) + " suggestions of " + std::to_string(owned.size()) + " owned (bank " +
                    (bank.live ? "live" : bank.seen ? "cached" : "not seen yet: open it once") + ", profile '" + prof.name + "', " +
                    std::to_string(double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart)).substr(0, 5) + " ms)" + ex);
        return g_cache;
    }
}

namespace {
    struct ItemSell : feature::Feature {
        ItemSell() : Feature("Item sell", feature::Stage::Alpha) { optIn = true; }  // new game-thread hook
        void OnFrame(const feature::Frame&) override {
            io::Tick();
            item_sell::badges::Frame();
        }
        void Off() override { item_sell::badges::Off(); }
    } g_feature;
}
