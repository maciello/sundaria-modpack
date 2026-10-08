#pragma once
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

// Minimal entity-component registry for ability mods (SDK-free, tested in test/ecs_test.cpp).
// Entities are index + generation (a destroyed entity's handle never aliases a new one); each component
// type has a sparse-set pool; each<A, B>(f) visits entities that have all listed components.
// Single-threaded: the owning feature touches it only on the game thread.
namespace ecs {
    struct Entity {
        uint32_t index = UINT32_MAX, gen = 0;
        bool operator==(const Entity&) const = default;
        explicit operator bool() const { return index != UINT32_MAX; }
    };

    class Registry {
        struct PoolBase {
            virtual ~PoolBase() = default;
            virtual void Remove(uint32_t index) = 0;
            virtual void Clear() = 0;
        };
        template <class T> struct Pool : PoolBase {
            std::vector<int32_t> sparse;   // entity index -> slot, -1 = none
            std::vector<uint32_t> owner;   // slot -> entity index
            std::vector<T> data;           // slot -> component
            T* Find(uint32_t i) { return i < sparse.size() && sparse[i] >= 0 ? &data[sparse[i]] : nullptr; }
            void Remove(uint32_t i) override {
                if (i >= sparse.size() || sparse[i] < 0) return;
                const int32_t slot = sparse[i], last = static_cast<int32_t>(data.size()) - 1;
                if (slot != last) {
                    data[slot] = std::move(data[last]);
                    owner[slot] = owner[last];
                    sparse[owner[slot]] = slot;
                }
                data.pop_back(); owner.pop_back(); sparse[i] = -1;
            }
            void Clear() override { sparse.clear(); owner.clear(); data.clear(); }
        };

        std::vector<uint32_t> gens_;
        std::vector<bool> alive_;
        std::vector<uint32_t> free_;
        std::vector<std::unique_ptr<PoolBase>> pools_;  // by TypeId<T>()

        static uint32_t NextId() { static uint32_t n = 0; return n++; }
        template <class T> static uint32_t TypeId() { static const uint32_t id = NextId(); return id; }
        template <class T> Pool<T>& P() {
            const uint32_t id = TypeId<T>();
            if (id >= pools_.size()) pools_.resize(id + 1);
            if (!pools_[id]) pools_[id] = std::make_unique<Pool<T>>();
            return *static_cast<Pool<T>*>(pools_[id].get());
        }

    public:
        Entity Create() {
            uint32_t i;
            if (!free_.empty()) { i = free_.back(); free_.pop_back(); alive_[i] = true; }
            else { i = static_cast<uint32_t>(gens_.size()); gens_.push_back(0); alive_.push_back(true); }
            return {i, gens_[i]};
        }
        bool Alive(Entity e) const { return e.index < gens_.size() && alive_[e.index] && gens_[e.index] == e.gen; }
        void Destroy(Entity e) {
            if (!Alive(e)) return;
            for (auto& p : pools_) if (p) p->Remove(e.index);
            alive_[e.index] = false;
            gens_[e.index]++;
            free_.push_back(e.index);
        }
        void Clear() {
            for (auto& p : pools_) if (p) p->Clear();
            for (uint32_t i = 0; i < gens_.size(); i++)
                if (alive_[i]) { alive_[i] = false; gens_[i]++; free_.push_back(i); }
        }
        size_t Count() const { size_t n = 0; for (bool a : alive_) n += a; return n; }

        template <class T> T& Add(Entity e, T value = {}) {
            Pool<T>& p = P<T>();
            if (T* have = Get<T>(e)) return *have = std::move(value);
            if (e.index >= p.sparse.size()) p.sparse.resize(e.index + 1, -1);
            p.sparse[e.index] = static_cast<int32_t>(p.data.size());
            p.owner.push_back(e.index);
            p.data.push_back(std::move(value));
            return p.data.back();
        }
        template <class T> T* Get(Entity e) { return Alive(e) ? P<T>().Find(e.index) : nullptr; }
        template <class T> void Remove(Entity e) { if (Alive(e)) P<T>().Remove(e.index); }
        template <class T> size_t Count() { return P<T>().data.size(); }

        // f(Entity, First&, Rest&...) for every entity with all components. Safe to create/destroy
        // entities or remove components inside f: it walks a copy of the owner list and re-checks.
        template <class First, class... Rest, class F> void Each(F&& f) {
            const std::vector<uint32_t> owners = P<First>().owner;
            for (uint32_t i : owners) {
                const Entity e{i, gens_[i]};
                First* a = Get<First>(e);
                if (!a) continue;
                if constexpr (sizeof...(Rest) == 0) f(e, *a);
                else {
                    auto rest = std::make_tuple(Get<Rest>(e)...);
                    if (!std::apply([](auto*... p) { return (... && (p != nullptr)); }, rest)) continue;
                    std::apply([&](auto*... p) { f(e, *a, *p...); }, rest);
                }
            }
        }
    };
}
