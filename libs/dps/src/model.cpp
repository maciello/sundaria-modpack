// Stub (#118): the API compiles and links; the model lands in the next commits.
#include "dps/dps.hpp"

namespace dps {
    struct Model::Impl { Tables t; };
    class Prepared {};

    Model::Model(Tables tables) : impl_(new Impl{std::move(tables)}) {}
    Model::~Model() = default;
    const Tables& Model::tables() const { return impl_->t; }
    std::unique_ptr<Model> Model::Load(Source& source, std::string& error) {
        Tables t;
        if (!source.Load(t, error)) return nullptr;
        return std::make_unique<Model>(std::move(t));
    }
    Result Model::Dps(const Build&, const Scenario&) const { return {.error = "not implemented"}; }
    std::shared_ptr<const Prepared> Model::Prepare(const Build&, const Scenario&) const { return std::make_shared<Prepared>(); }
    Result Model::Dps(const Prepared&) const { return {.error = "not implemented"}; }
    ItemScore Model::ScoreItem(const Prepared&, const Item&, int) const { return {}; }
    ItemScore Model::ScoreItem(const Build&, const Scenario&, const Item&, int) const { return {}; }
    BestInSlotResult Model::BestInSlot(const Build&, const Scenario&, std::span<const Item>) const { return {.error = "not implemented"}; }
    BestInSlotResult Model::BestInSlot(std::string_view, int, const Scenario&, std::span<const Item>) const { return {.error = "not implemented"}; }
    std::vector<StatWeight> Model::StatWeights(const Build&, const Scenario&, int) const { return {}; }

    std::unique_ptr<Source> FileSource(std::string) { return nullptr; }
}
