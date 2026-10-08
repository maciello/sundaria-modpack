#pragma once
// Cost of a hot path (event/tick handler): QPC ms, logged at the first call and at every new max.
// Usage: static cost::Path p{"item-sell bag event"}; cost::Scope s(p);   One thread per Path.
#include <cstdio>
#include <string>

namespace cost {
    struct Path {
        const char* name;
        double max = -1;
        int calls = 0;
        bool Record(double ms) {  // true = first call or new max: log it
            calls++;
            if (ms <= max) return false;
            max = ms;
            return true;
        }
        std::string Line(double ms) const {
            char b[160];
            std::snprintf(b, sizeof b, "[cost] %s: %.3f ms (%s, call %d)", name, ms, calls == 1 ? "first" : "max", calls);
            return b;
        }
    };
}

#ifdef _WIN32
#include <Windows.h>
#include "logger.hpp"
namespace cost {
    struct Scope {
        Path& p;
        LARGE_INTEGER t0;
        explicit Scope(Path& path) : p(path) { QueryPerformanceCounter(&t0); }
        ~Scope() {
            LARGE_INTEGER t1, f;
            QueryPerformanceCounter(&t1);
            QueryPerformanceFrequency(&f);
            const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
            if (p.Record(ms)) logger::log(p.Line(ms));
        }
    };
}
#endif
