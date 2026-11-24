// SHINO_PROFILE=1: where the game thread's time goes, by part (shino_port.h, prof). SHINO_RATE=1: the flip rate alone,
// no timing scopes (for comparing builds: the scopes' own cost stays out).
//
// SHINO_SAMPLE=<first flip>:<last flip>: the game thread's instruction pointer sampled every millisecond between those
// flips, written to build/samples.txt as offsets in the executable (tools/shino_samples.py names them).
#include "shino_port.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#endif

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

#ifdef _WIN32
    namespace
    {
        std::atomic<bool> g_sampling{false};
        std::map<uint64_t, uint64_t> g_samples; // instruction pointer -> samples (the sampler thread's)
        std::map<uint64_t, uint64_t> g_callers; // a sample outside the executable: the executable's caller -> samples
        std::atomic<bool> g_samplerStop{false};
        std::thread g_sampler;

        // Each sample as "<module> <offset in it> <count>" (the executable's offsets for tools/shino_samples.py).
        void dumpSamples()
        {
            g_samplerStop = true;
            if (g_sampler.joinable())
                g_sampler.join();
            FILE *f = std::fopen("build/samples.txt", "w");
            if (!f)
                return;
            for (const auto &[rip, n] : g_samples)
            {
                HMODULE module = nullptr;
                char name[MAX_PATH] = "?";
                uint64_t base = 0;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCSTR>(rip), &module))
                {
                    GetModuleFileNameA(module, name, sizeof(name));
                    base = reinterpret_cast<uint64_t>(module);
                    if (module == GetModuleHandleA(nullptr))
                        std::strcpy(name, "exe");
                }
                for (char *c = name; *c; ++c)
                    if (*c == ' ')
                        *c = '_';
                std::fprintf(f, "%s %llx %llu\n", name, (unsigned long long)(rip - base), (unsigned long long)n);
            }
            const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleA(nullptr));
            for (const auto &[rip, n] : g_callers)
                std::fprintf(f, "caller %llx %llu\n", (unsigned long long)(rip - exe), (unsigned long long)n);
            std::fclose(f);
        }

        void sample(uint64_t flips)
        {
            static const char *spec = std::getenv("SHINO_SAMPLE");
            if (!spec)
                return;
            static const uint64_t first = std::strtoull(spec, nullptr, 10);
            static const uint64_t last = [] { const char *c = std::strchr(spec, ':'); return c ? std::strtoull(c + 1, nullptr, 10) : ~0ull; }();
            if (flips == first && !g_sampler.joinable())
            {
                HANDLE game = nullptr;
                DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game, 0, FALSE, DUPLICATE_SAME_ACCESS);
                g_sampling = true;
                g_sampler = std::thread([game] {
                    // the executable's range: a sample outside it is unwound to the executable's caller
                    const uint64_t exe = reinterpret_cast<uint64_t>(GetModuleHandleA(nullptr));
                    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(exe);
                    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(exe + dos->e_lfanew);
                    const uint64_t exeEnd = exe + nt->OptionalHeader.SizeOfImage;
                    // a high-resolution timer: about 1000 samples a second (Sleep(1) gives about 64)
                    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
                    while (!g_samplerStop)
                    {
                        if (timer)
                        {
                            LARGE_INTEGER due;
                            due.QuadPart = -10000; // 1 ms
                            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                            WaitForSingleObject(timer, INFINITE);
                        }
                        else
                            Sleep(1);
                        if (!g_sampling)
                            continue;
                        if (SuspendThread(game) == DWORD(-1))
                            continue;
                        CONTEXT c{};
                        c.ContextFlags = CONTEXT_FULL;
                        const bool got = GetThreadContext(game, &c) != 0;
                        uint64_t caller = 0;
                        if (got && (c.Rip < exe || c.Rip >= exeEnd))
                        {
                            CONTEXT u = c; // unwound on the suspended thread's stack, up to a frame in the executable
                            for (int depth = 0; depth < 16 && (u.Rip < exe || u.Rip >= exeEnd); ++depth)
                            {
                                DWORD64 imageBase = 0;
                                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(u.Rip, &imageBase, nullptr);
                                if (!fn)
                                {
                                    u.Rip = *reinterpret_cast<const DWORD64 *>(u.Rsp); // a leaf: its return address on top
                                    u.Rsp += 8;
                                    continue;
                                }
                                PVOID handler = nullptr;
                                DWORD64 frame = 0;
                                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, u.Rip, fn, &u, &handler, &frame, nullptr);
                            }
                            if (u.Rip >= exe && u.Rip < exeEnd)
                                caller = u.Rip;
                        }
                        ResumeThread(game);
                        if (got) // after the resume: the map may allocate, and the game thread may hold the heap's lock
                        {
                            ++g_samples[c.Rip];
                            if (caller)
                                ++g_callers[caller];
                        }
                    }
                });
                std::atexit(dumpSamples);
            }
            if (flips == last)
                g_sampling = false;
        }
    }
#endif

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
#ifdef _WIN32
        sample(g_flips + 1);
#endif
        if (++g_flips, (!on() && !rate) || g_flips % 250 != 0)
            return;
        const double wall = std::chrono::duration<double>(Clock::now() - g_windowStart).count();
        std::printf("[shino:profile] flips %llu: %.1f flips/s", (unsigned long long)g_flips, 250.0 / wall);
#ifdef _WIN32
        {
            // The game thread's own CPU time a flip: steadier than the rate while other programs load the machine.
            FILETIME created, exited, kernel, user;
            static uint64_t before = 0;
            if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
            {
                const uint64_t now = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                                     (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
                std::printf(" (cpu %.2f ms a flip)", double(now - before) / 1e4 / 250.0);
                before = now;
            }
        }
#endif
        if (on())
        {
            charge();
            static const char *names[Count] = {"other", "vu1", "vu0", "gs", "decode", "submit", "vif", "upload"};
            std::printf(";");
            for (int b = 0; b < Count; ++b)
                std::printf(" %s %.1f ms", names[b], g_seconds[b] * 1000.0 / 250.0);
            static uint64_t pairsBefore = 0, programsBefore = 0;
            const uint64_t pairs = vif::vu1Pairs(), programs = vif::vu1Programs();
            std::printf(" (per flip); vu1 %.2fM pairs, %.0f programs a flip", double(pairs - pairsBefore) / 250e6,
                        double(programs - programsBefore) / 250.0);
            pairsBefore = pairs;
            programsBefore = programs;
            for (double &s : g_seconds)
                s = 0;
        }
        std::printf("\n");
        g_windowStart = Clock::now();
    }
}
