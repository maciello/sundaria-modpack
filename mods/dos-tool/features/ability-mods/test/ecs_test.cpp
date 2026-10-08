// just test
#include "../ecs.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <tuple>

struct Pos { float x = 0; };
struct Tag { std::string name; };

int main() {
    ecs::Registry r;
    ecs::Entity a = r.Create(), b = r.Create(), c = r.Create();
    r.Add(a, Pos{1}); r.Add(b, Pos{2}); r.Add(c, Pos{3});
    r.Add(b, Tag{"b"}); r.Add(c, Tag{"c"});
    assert(r.Count() == 3 && r.Count<Pos>() == 3 && r.Count<Tag>() == 2);

    // Query: only entities with both components.
    float sum = 0; int n = 0;
    r.Each<Pos, Tag>([&](ecs::Entity, Pos& p, Tag&) { sum += p.x; n++; });
    assert(n == 2 && sum == 5);

    // Add again replaces; Get on a missing component is null.
    r.Add(a, Pos{10});
    assert(r.Get<Pos>(a)->x == 10 && r.Count<Pos>() == 3 && !r.Get<Tag>(a));

    // Remove keeps the others' data intact (swap-remove).
    r.Remove<Pos>(a);
    assert(!r.Get<Pos>(a) && r.Get<Pos>(b)->x == 2 && r.Get<Pos>(c)->x == 3);

    // Destroy: the old handle is dead even after its index is reused.
    r.Destroy(b);
    assert(!r.Alive(b) && !r.Get<Pos>(b) && r.Count<Tag>() == 1);
    ecs::Entity d = r.Create();
    assert(d.index == b.index && d.gen != b.gen && !r.Alive(b) && r.Alive(d) && !r.Get<Pos>(d));

    // Destroying inside a query is safe.
    r.Add(d, Pos{4});
    n = 0;
    r.Each<Pos>([&](ecs::Entity e, Pos&) { r.Destroy(e); n++; });
    assert(n == 2 && r.Count<Pos>() == 0 && r.Count() == 1);  // a has no Pos and survives

    r.Clear();
    assert(r.Count() == 0 && !r.Alive(a) && r.Count<Tag>() == 0);
    std::puts("ok");
}
