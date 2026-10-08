#pragma once
// SDK-free: what the dungeon map draws this tick, as solid-tint quads in minimap pixels (CanvasPanel_DynamicMinimp space).
// draw.cpp turns each Quad into one UImage. Spec: design-system.md § Dungeon map path.
#include <cmath>
#include <vector>
#include "../shared/plan.hpp"
#include "style.hpp"

namespace dungeon_map {
    // Component geometry (px) and opacity, from the spec.
    constexpr float kLineW = 2, kLineAlpha = 0.55f;
    constexpr float kCapPx = 7, kCapAlpha = 0.9f, kPulsePx = 5;
    constexpr float kPlateBlocked = 16, kPlateLever = 14, kPlateAlpha = 0.85f;
    constexpr float kXLen = 11, kXW = 3, kLeverLen = 9, kLeverW = 3, kLeverAngle = -60;
    constexpr float kDashPx = 6, kDashGap = 5, kLinkAlpha = 0.8f, kJoinPx = 5;  // connector (#83): dashed, same colour
    constexpr int kMaxDashes = 64;
    constexpr float kHaloPx = 22, kHaloAlpha = 0.35f;  // lever on the route (#93): breathing halo behind its plate
    constexpr int kZLine = 0, kZMark = 1, kZIcon = 2;

    // Halo alpha at t seconds: 0 → kHaloAlpha → 0 over one period (sine).
    inline float Halo(double t, float period) {
        return kHaloAlpha * 0.5f * (1 - float(std::cos(6.2831853 * std::fmod(t, double(period)) / period)));
    }

    struct Quad {
        float x, y, w, h, angle;  // centre, size, degrees in canvas space
        style::Rgba c;
        float alpha;
        int z;
    };

    // Square of diagonal d, standing on its tip on screen (the canvas turns by mapAngle).
    inline Quad Diamond(Px p, float d, style::Rgba c, float a, int z, float mapAngle) {
        const float s = d / std::sqrt(2.f);
        return {p.x, p.y, s, s, 45 - mapAngle, c, a, z};
    }
    inline Quad Bar(Px a, Px b, float w, style::Rgba c, float alpha, int z) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        return {(a.x + b.x) / 2, (a.y + b.y) / 2, std::hypot(dx, dy), w, std::atan2(dy, dx) * 57.29578f, c, alpha, z};
    }

    inline Path ToMapPath(const Path& w, float upp) {
        Path px;
        for (const V3& v : w) { const Px p = ToMap(v, upp); px.push_back({p.x, p.y, 0}); }
        return px;
    }

    // main: the main route (world), drawn in full; link: the connector from the player to it (empty = none);
    // upp: minimap UnitToPixel; mapAngle: CanvasPanel_Map render angle (icons counter-rotate); t: seconds, drives the flow.
    inline std::vector<Quad> Scene(const Path& main, const Path& link, const Marks& m, float upp, float mapAngle, double t) {
        using namespace style::color;
        std::vector<Quad> out;
        if (link.size() > 1) {  // dashes spread evenly over the connector, at most kMaxDashes; a diamond where it joins
            const Path px = ToMapPath(link, upp);
            const float len = Length(px);
            const int n = std::clamp(int(len / (kDashPx + kDashGap)), 1, kMaxDashes);
            const float period = len / n, dash = std::min(kDashPx, period);
            for (int i = 0; i < n; i++) {
                const V3 a = At(px, i * period), b = At(px, i * period + dash);
                if (Dist(a, b) > 0.5f) out.push_back(Bar({a.x, a.y}, {b.x, b.y}, kLineW, kAccent, kLinkAlpha, kZLine));
            }
            out.push_back(Diamond({px.back().x, px.back().y}, kJoinPx, kAccent, kLinkAlpha, kZMark, mapAngle));
        }
        if (main.size() > 1) {
            const Path px = ToMapPath(main, upp);  // the drawn line in map pixels
            for (size_t i = 1; i < px.size(); i++)
                if (Dist(px[i - 1], px[i]) > 0.5f) out.push_back(Bar({px[i - 1].x, px[i - 1].y}, {px[i].x, px[i].y}, kLineW, kAccent, kLineAlpha, kZLine));
            for (const Pulse& p : Pulses(Length(px), t)) {
                const V3 at = At(px, p.s);
                out.push_back(Diamond({at.x, at.y}, kPulsePx, kGameHighlight, p.alpha, kZMark, mapAngle));
            }
            out.push_back(Diamond({px.back().x, px.back().y}, kCapPx, kGameHighlight, kCapAlpha, kZMark, mapAngle));
        }
        if (m.locked) {
            const Px d = ToMap(m.door, upp);
            out.push_back(Diamond(d, kPlateBlocked, kInk, kPlateAlpha, kZIcon, mapAngle));
            for (float a : {45.f, -45.f}) out.push_back({d.x, d.y, kXLen, kXW, a - mapAngle, kTaken, 1, kZIcon + 1});
            for (const V3& l : m.levers) {
                const Px p = ToMap(l, upp);
                out.push_back(Diamond(p, kHaloPx, kGameHighlight, Halo(t, style::motion::kPulsePeriod), kZMark, mapAngle));
                out.push_back(Diamond(p, kPlateLever, kInk, kPlateAlpha, kZIcon, mapAngle));
                out.push_back({p.x, p.y, kLeverLen, kLeverW, kLeverAngle - mapAngle, kGameHighlight, 1, kZIcon + 1});
            }
        }
        return out;
    }
}
