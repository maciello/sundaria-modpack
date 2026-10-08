// just test
#include "skeleton-probe.hpp"
#include <cassert>
#include <cstdio>

using skeleton_probe::Bone;

static Bone B(const char* n, int p) { return Bone{n, p, {0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}}; }

int main() {
    using namespace skeleton_probe;
    std::vector<Bone> ok = {B("root", -1), B("pelvis", 0), B("spine_01", 1), B("thigh_l", 1)};
    assert(ValidTree(ok));
    assert(Depth(ok, 0) == 0 && Depth(ok, 2) == 2 && Depth(ok, 3) == 2);

    assert(!ValidTree({}));
    assert(!ValidTree({B("root", 0)}));                    // root must have parent -1
    assert(!ValidTree({B("root", -1), B("a", -1)}));       // second root = wrong stride / garbage
    assert(!ValidTree({B("root", -1), B("a", 1)}));        // parent must come before the bone
    assert(!ValidTree({B("root", -1), B("", 0)}));         // unresolved name

    Diff same = Compare(ok, ok);
    assert(same.sameOrder && same.firstMismatch == -1 && same.maxT == 0 && same.maxQ == 0);
    std::vector<Bone> moved = ok;
    moved[2].t[2] = 1.5f;
    for (float& c : moved[3].q) c = -c;  // -q = same rotation
    Diff d = Compare(ok, moved);
    assert(d.sameOrder && d.maxT == 1.5f && d.maxQ < 1e-6f);
    std::vector<Bone> swapped = {B("root", -1), B("pelvis", 0), B("thigh_l", 1), B("spine_01", 1)};
    d = Compare(ok, swapped);
    assert(!d.sameOrder && d.firstMismatch == 2);
    d = Compare(ok, {B("root", -1), B("pelvis", 0)});
    assert(!d.sameOrder && d.firstMismatch == 2);

    const std::string s = Format(ok);
    assert(s.find("  0  -1 root") == 0);
    assert(s.find("  2   1     spine_01") != std::string::npos);  // indented two levels
    assert(s.find("q 0.00000 0.00000 0.00000 1.00000") != std::string::npos);
    std::puts("ok");
}
