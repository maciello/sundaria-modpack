#pragma once
#include <cstdio>
#include <string>
#include <vector>

// Skeleton probe logic (SDK-free): checks that a raw bone table read from memory is a real bone tree and
// formats it as the text file we rebuild a matching skeleton from (UE 4.27 editor / Blender).
namespace skeleton_probe {
    struct Bone {
        std::string name;
        int parent;
        float t[3], q[4], s[3];  // local reference pose: translation, rotation quat (x y z w), scale
    };

    // A bone table is a tree when bone 0 is the only root (parent -1) and every other bone's parent comes
    // before it (UE's FReferenceSkeleton guarantees both), and no name is empty.
    inline bool ValidTree(const std::vector<Bone>& b) {
        if (b.empty() || b[0].parent != -1 || b[0].name.empty()) return false;
        for (size_t i = 1; i < b.size(); i++)
            if (b[i].parent < 0 || b[i].parent >= int(i) || b[i].name.empty()) return false;
        return true;
    }

    inline int Depth(const std::vector<Bone>& b, size_t i) {
        int d = 0;
        for (int p = b[i].parent; p >= 0 && d < 256; p = b[p].parent) d++;
        return d;
    }

    // Copy vs original: same names and parents at every index (animations address bones by index), and how far the
    // reference pose drifted (translation in cm; rotation as 1-|dot| of the quats, q and -q being the same rotation).
    struct Diff { bool sameOrder; int firstMismatch; float maxT, maxQ; };
    inline Diff Compare(const std::vector<Bone>& a, const std::vector<Bone>& b) {
        Diff d{a.size() == b.size(), -1, 0, 0};
        const size_t n = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < n; i++) {
            if (a[i].name != b[i].name || a[i].parent != b[i].parent) {
                if (d.firstMismatch < 0) d.firstMismatch = int(i);
                d.sameOrder = false;
                continue;
            }
            for (int k = 0; k < 3; k++) {
                const float dt = a[i].t[k] - b[i].t[k];
                d.maxT = (dt < 0 ? -dt : dt) > d.maxT ? (dt < 0 ? -dt : dt) : d.maxT;
            }
            float dot = 0;
            for (int k = 0; k < 4; k++) dot += a[i].q[k] * b[i].q[k];
            const float dq = 1.0f - (dot < 0 ? -dot : dot);
            d.maxQ = dq > d.maxQ ? dq : d.maxQ;
        }
        if (d.firstMismatch < 0 && a.size() != b.size()) d.firstMismatch = int(n);
        return d;
    }

    // One line per bone: "index parent <indented name>  t x y z  q x y z w  s x y z".
    inline std::string Format(const std::vector<Bone>& b) {
        std::string out;
        char buf[384];
        for (size_t i = 0; i < b.size(); i++) {
            const Bone& x = b[i];
            std::snprintf(buf, sizeof(buf), "%3zu %3d %*s%-*s  t %.3f %.3f %.3f  q %.5f %.5f %.5f %.5f  s %.3f %.3f %.3f\n", i, x.parent,
                          Depth(b, i) * 2, "", 40 - Depth(b, i) * 2 > 0 ? 40 - Depth(b, i) * 2 : 1, x.name.c_str(), x.t[0], x.t[1], x.t[2],
                          x.q[0], x.q[1], x.q[2], x.q[3], x.s[0], x.s[1], x.s[2]);
            out += buf;
        }
        return out;
    }
}
