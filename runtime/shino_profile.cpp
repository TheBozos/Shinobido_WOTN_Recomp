// SHINO_PROFILE=1: where the game thread's time goes, by part (shino_port.h, prof). SHINO_RATE=1: the flip rate alone,
// no timing scopes (for comparing builds: the scopes' own cost stays out).
#include "shino_port.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace shino::prof
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        Bucket g_current = Other;
        Clock::time_point g_since = Clock::now(), g_windowStart = Clock::now();
        double g_seconds[Count] = {};
        uint64_t g_flips = 0;

        void charge()
        {
            const Clock::time_point now = Clock::now();
            g_seconds[g_current] += std::chrono::duration<double>(now - g_since).count();
            g_since = now;
        }
    }

    bool on()
    {
        static const bool enabled = std::getenv("SHINO_PROFILE") != nullptr;
        return enabled;
    }

    void enter(Bucket b, Bucket &previous)
    {
        charge();
        previous = g_current;
        g_current = b;
    }

    void leave(Bucket previous)
    {
        charge();
        g_current = previous;
    }

    void flip()
    {
        static const bool rate = std::getenv("SHINO_RATE") != nullptr;
        if ((!on() && !rate) || ++g_flips % 250 != 0)
            return;
        const double wall = std::chrono::duration<double>(Clock::now() - g_windowStart).count();
        std::printf("[shino:profile] flips %llu: %.1f flips/s", (unsigned long long)g_flips, 250.0 / wall);
        if (on())
        {
            charge();
            static const char *names[Count] = {"other", "vu1", "vu0", "gs", "decode", "submit", "vif", "upload"};
            std::printf(";");
            for (int b = 0; b < Count; ++b)
                std::printf(" %s %.1f ms", names[b], g_seconds[b] * 1000.0 / 250.0);
            std::printf(" (per flip)");
            for (double &s : g_seconds)
                s = 0;
        }
        std::printf("\n");
        g_windowStart = Clock::now();
    }
}
