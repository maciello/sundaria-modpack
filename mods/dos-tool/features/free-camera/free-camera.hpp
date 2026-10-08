#pragma once
#include <algorithm>
#include <cmath>

// SDK-free fly-camera math in UE axes: X forward, Y right, Z up; pitch/yaw in degrees.
// W/S fly along the view direction (pitch included), A/D strafe flat, up/down is world Z.
namespace free_camera {
    struct Pose { float x, y, z, pitch, yaw; };
    struct Input { float forward, right, up; float lookPitch, lookYaw; bool fast; };  // axes in -1..1, look in degrees

    inline Pose Step(Pose p, const Input& in, float dt, float speed) {
        p.yaw = std::fmod(p.yaw + in.lookYaw, 360.0f);
        p.pitch = std::clamp(p.pitch + in.lookPitch, -89.0f, 89.0f);
        const float k = 3.14159265f / 180.0f, cp = std::cos(p.pitch * k), sp = std::sin(p.pitch * k);
        const float cy = std::cos(p.yaw * k), sy = std::sin(p.yaw * k);
        const float d = speed * (in.fast ? 4.0f : 1.0f) * dt;
        p.x += d * (in.forward * cp * cy - in.right * sy);
        p.y += d * (in.forward * cp * sy + in.right * cy);
        p.z += d * (in.forward * sp + in.up);
        return p;
    }
}
