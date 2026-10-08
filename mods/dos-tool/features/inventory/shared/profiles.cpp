#include "order.hpp"
#include <Windows.h>
#include <atomic>

// Profiles shared by item features; any thread.
namespace items::profiles {
    namespace {
        SRWLOCK mu = SRWLOCK_INIT;
        std::vector<Profile> all = Presets();
        std::atomic<int> active{0};
    }
    std::vector<Profile> All() {
        AcquireSRWLockShared(&mu); std::vector<Profile> v = all; ReleaseSRWLockShared(&mu);
        return v;
    }
    void SetAll(std::vector<Profile> v) {
        if (v.empty()) return;
        AcquireSRWLockExclusive(&mu); all = std::move(v); ReleaseSRWLockExclusive(&mu);
        SetActive(active.load());
    }
    int ActiveIndex() { return active.load(); }
    void SetActive(int i) {
        AcquireSRWLockShared(&mu); const int n = int(all.size()); ReleaseSRWLockShared(&mu);
        active = std::clamp(i, 0, n - 1);
    }
    Profile Active() {
        AcquireSRWLockShared(&mu); Profile p = all[std::clamp(active.load(), 0, int(all.size()) - 1)]; ReleaseSRWLockShared(&mu);
        return p;
    }
}
