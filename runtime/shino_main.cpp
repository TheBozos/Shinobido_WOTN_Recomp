// Shinobido: Way of the Ninja on BozoEngine: the headless runner (shino-headless) and the window (shinobido).
// The headless runner prints what is not native yet: the end reason, unknown device accesses by site, library calls by
// name (SHINO_CALL_STATS=1). Each stop is the next thing to name or answer.
#include "bozo/app/frame_pacer.h"
#include "bozo/audio/wav.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"
#include "shino_functions.h"
#include "shino_port.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>
#ifdef SHINO_WITH_WINDOW
#include "bozo/app/app.h"
#include "bozo/app/audio_device.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/window.h"
#endif

int main(int argc, char **argv)
{
    bozo::host::FreeOptions options;
    options.framePeriodCycles = 294912000 / 50; // PAL (VMODE = PAL in SYSTEM.CNF)
    bool window = false;
#ifdef SHINO_WITH_WINDOW
    window = true;
    bozo::app::setIdentity({"Shinobido: Way of the Ninja", "Shinobido_WOTN_Recomp", "shinobido-recomp", "SCES_539.31"});
    bozo::app::WindowOptions windowOptions;
    windowOptions.title = "Shinobido: Way of the Ninja - BozoEngine";
    windowOptions.width = 960;
    windowOptions.height = 720;
#endif
    options.elf = SHINO_DEFAULT_ELF;
    options.iso = SHINO_DEFAULT_ISO;
    // SCES_539.31 is stripped: the symbol map (recomp/shino_symbols.txt) names the functions the engine answers.
    options.symbolMap = SHINO_SYMBOL_MAP;
    options.frames = window ? 0 : 600;
    options.printGameOutput = true;
    std::string wav, dumpRam;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)
            options.frames = std::stoul(argv[++i]);
        else if (a == "--pad-script" && i + 1 < argc)
            options.padScript = argv[++i];
        else if (a == "--card-dir" && i + 1 < argc)
            options.cardDir = argv[++i];
        else if (a == "--out" && i + 1 < argc)
            options.out = argv[++i];
        else if (a == "--elf" && i + 1 < argc)
            options.elf = argv[++i];
        else if (a == "--iso" && i + 1 < argc)
            options.iso = argv[++i];
        else if (a == "--symbols" && i + 1 < argc)
            options.symbolMap = argv[++i];
        else if (a == "--wav" && i + 1 < argc)
            wav = argv[++i];
        else if (a == "--headless")
            window = false;
        else if (a == "--profile-io")
            options.profileIo = true;
        else if (a == "--quiet")
            options.printGameOutput = false;
        else if (a == "--dump-ram" && i + 1 < argc)
            dumpRam = argv[++i];
        else if (a == "--load-state" && i + 1 < argc)
            options.loadState = argv[++i];
        else if (a == "--save-state" && i + 1 < argc)
            options.saveState = argv[++i];
        else if (a == "--save-state-at" && i + 1 < argc)
            options.saveStateFrame = std::stoul(argv[++i]);
#ifdef SHINO_WITH_WINDOW
        else if (a == "--no-vsync")
            windowOptions.vsync = false;
        else if (a == "--fullscreen")
            windowOptions.fullscreen = true;
#endif
        else
        {
            std::fprintf(stderr, "Usage: shinobido [--frames N] [--headless] [--elf FILE] [--iso FILE] [--symbols FILE]\n"
                                 "                 [--card-dir DIR] [--pad-script FILE] [--wav FILE] [--out FILE]\n"
                                 "                 [--profile-io] [--quiet]\n"
                                 "                 [--dump-ram FILE] [--load-state FILE] [--save-state FILE --save-state-at N]\n"
                                 "                 [--fullscreen] [--no-vsync]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    options.realTime = window;
    options.gameTag = "SCES-53931"; // a save state loads only into Shinobido
    bozo::host::FreeHost host(options);
    bozo::ee::Dispatcher d(host);
    d.setFunctions(shino_function_table, shino_function_count, shino_resume_table, shino_resume_count);
    bozo::port::Registry registry;
    shino::install(d.runtime(), registry);
    std::string error;
    if (!host.setUp(d, shino_external_table, shino_external_count, &registry, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }

    bozo::app::FramePacer pacer;
    pacer.setPaced(window);
    host.onFrame = [&](uint64_t f, const R5900Context *ctx)
    {
        pacer.frameEnd();
        if (f % 60 == 0)
            std::printf("[shino] frame=%llu pc=%x cycles=%llu\n", (unsigned long long)f, ctx->pc,
                        (unsigned long long)d.now());
    };
    bozo::audio::WavWriter recording;
    if (!wav.empty() && !recording.open(wav))
    {
        std::fprintf(stderr, "cannot open WAV %s\n", wav.c_str());
        return 2;
    }
#ifdef SHINO_WITH_WINDOW
    bozo::app::DevicePad devices;
    bozo::app::AudioDevice audio;
    if (window && options.padScript.empty())
        host.libraries().pad.input = &devices;
#endif
    host.onAudio = [&](uint64_t, const int16_t *pcm, size_t count)
    {
        if (recording.isOpen())
            recording.write(pcm, count);
#ifdef SHINO_WITH_WINDOW
        if (window)
            audio.push(pcm, count);
#endif
    };
    auto run = [&]()
    {
#ifdef SHINO_WITH_WINDOW
        if (window)
            audio.open();
#endif
        uint64_t frames = 0;
        try
        {
            frames = host.run();
        }
        catch (const std::exception &e)
        {
            std::fprintf(stderr, "[shino] native failure: %s\n", e.what());
            return 1;
        }
        if (!dumpRam.empty())
            if (FILE *f = std::fopen(dumpRam.c_str(), "wb"))
            {
                std::fwrite(d.runtime().memory.ram(), 1, 0x2000000u, f);
                std::fclose(f);
            }
        std::printf("[shino] frames=%llu end=%s\n", (unsigned long long)frames, host.endReason().c_str());
        const auto &stats = host.stats();
        std::printf("[shino] compatibility: %llu register accesses, %llu unknown; %llu native VU0 calls\n",
                    (unsigned long long)stats.ioAccesses, (unsigned long long)stats.ioUnknown,
                    (unsigned long long)stats.vu0Calls);
        std::printf("[shino] calls: %llu kernel, %llu library, %llu port overrides\n",
                    (unsigned long long)stats.kernelCalls, (unsigned long long)stats.libraryCalls,
                    (unsigned long long)stats.overrideCalls);
        for (const auto &[name, count] : stats.unanswered)
            std::printf("[shino:unanswered] %10llu %s\n", (unsigned long long)count, name.c_str());
        if (std::getenv("SHINO_CALL_STATS"))
        {
            std::vector<std::pair<uint64_t, std::string>> byName;
            for (const auto &[name, count] : stats.libraryByName)
                byName.emplace_back(count, name);
            std::sort(byName.rbegin(), byName.rend());
            for (const auto &[count, name] : byName)
                std::printf("[shino:lib] %10llu %s\n", (unsigned long long)count, name.c_str());
        }
        for (const auto &[site, counts] : stats.ioSites)
            std::printf("[shino:io] pc=%08x address=%08x reads=%llu writes=%llu %s\n", site.first, site.second,
                        (unsigned long long)counts[0], (unsigned long long)counts[1],
                        host.functionAt(site.first).c_str());
        return host.endReason().rfind("FAULT:", 0) == 0 ? 1 : 0;
    };
    int result;
#ifdef SHINO_WITH_WINDOW
    if (window)
    {
        bozo::app::Game game;
        game.run = run;
        game.stop = [&] { host.requestStop(); };
        game.framesEnded = [&] { return host.framesEnded(); };
        game.saveState = [&](int slot) { host.requestSave(bozo::app::stateSlotPath(slot)); };
        game.loadState = [&](int slot) { host.requestLoad(bozo::app::stateSlotPath(slot)); };
        host.onStateMessage = [](bool, const std::string &message) { bozo::app::notify(message); };
        result = bozo::app::runWindow(windowOptions, game, &devices);
    }
    else
#endif
        result = run();
    recording.close();
    return result;
}
