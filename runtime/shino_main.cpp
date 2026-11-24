// Shinobido: Way of the Ninja on BozoEngine: the headless runner (shino-headless) and the window (shinobido).
// The headless runner prints what is not native yet: the end reason, unknown device accesses by site, library calls by
// name (SHINO_CALL_STATS=1). Each stop is the next thing to name or answer.
#include "bozo/app/frame_pacer.h"
#include "bozo/audio/wav.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"
#include "shino_cheats.h"
#include "shino_functions.h"
#include "shino_port.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    bool mute = false; // --mute: no sound through the speakers (hidden runs, tests, captures)
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
    // The boot waits for the IOP sound modules' RPC servers before any frame ends: they advance while it waits.
    options.advanceLibrariesWhileStalled = true;
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
        else if (a == "--mute")
            mute = true;
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
        else if (a == "--hidden") // no visible window: it presents offscreen (screenshots and captures still work)
            windowOptions.hidden = true;
#endif
        else
        {
            std::fprintf(stderr, "Usage: shinobido [--frames N] [--headless] [--elf FILE] [--iso FILE] [--symbols FILE]\n"
                                 "                 [--card-dir DIR] [--pad-script FILE] [--wav FILE] [--out FILE]\n"
                                 "                 [--profile-io] [--quiet] [--mute]\n"
                                 "                 [--dump-ram FILE] [--load-state FILE] [--save-state FILE --save-state-at N]\n"
                                 "                 [--fullscreen] [--no-vsync] [--hidden]\n");
            return a == "--help" ? 0 : 2;
        }
    }
    options.realTime = window;
    options.gameTag = "SCES-53931"; // a save state loads only into Shinobido
    bozo::host::FreeHost host(options);
    bozo::ee::Dispatcher d(host);
    shino::installAudio(host.libraries(), d.runtime().memory.ram());
    // The movies (libmpeg native): the SDK's results, pictures as soon as decoded (the player queues them and shows
    // them from its own vblank handler), and a read-ahead long enough for the audio the player waits for before its
    // first picture (the first audio packet comes after 14 pictures; it wants 0x6000 bytes).
    host.libraries().mpegOptions = {.referenceLayout = false, .paced = false, .sdkNodata = false, .picturesAhead = 32, .sdkDemux = true};
    // A PAL disc (SYSTEM.CNF: VMODE = PAL): the game's graphics init asks for the console's own video mode (0), which
    // is PAL's 50 fields a second here; the movie player asks for PAL explicitly.
    host.libraries().palConsole = true;
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
    // Each frame held to the game's own time: the vblanks (PAL fields, 50 a second) it spanned. The game flips every
    // second vblank (its vblank handler's minimum is 2: 25 frames a second, menus and gameplay alike), so a cap of 50
    // frames a second ran it at twice its speed wherever the port kept up. PS2X_FRAME_CAP overrides.
    const bool capFromEnvironment = std::getenv("PS2X_FRAME_CAP") != nullptr;
    if (!capFromEnvironment)
        pacer.setCap(25);
    uint64_t lastFrameCycles = 0;
    host.onFrame = [&](uint64_t f, const R5900Context *ctx)
    {
        const uint64_t now = d.now();
        if (!capFromEnvironment && lastFrameCycles && now > lastFrameCycles)
        {
            const double fields = double(now - lastFrameCycles) / double(options.framePeriodCycles);
            const uint32_t spanned = static_cast<uint32_t>(std::clamp(fields + 0.5, 1.0, 10.0));
            pacer.setCap(50u / spanned);
        }
        lastFrameCycles = now;
        // SHINO_DUMP_RAM_AT=<frame>,<frame>...: the EE's RAM at each of those frames' end, as build/ram_<frame>.bin (the
        // memory searches that found the cheats' fields, docs/RECOMP_NOTES.md).
        if (static const char *at = std::getenv("SHINO_DUMP_RAM_AT"); at)
            for (const char *p = at; *p;)
            {
                char *next = nullptr;
                const uint64_t want = std::strtoull(p, &next, 10);
                if (want == f)
                {
                    const std::string name = "build/ram_" + std::to_string(f) + ".bin";
                    if (FILE *file = std::fopen(name.c_str(), "wb"))
                    {
                        std::fwrite(d.runtime().memory.ram(), 1, 0x2000000u, file);
                        std::fclose(file);
                    }
                }
                if (!next || next == p)
                    break;
                p = *next ? next + 1 : next;
            }
        // SHINO_TRACE_PLAYER=<every>: the player's object (the pointer at gp-0x6a0c, 0x625d64), its first 8 KB, every n
        // frames into build/player_trace.bin (frame, then the bytes): the search for its fields.
        if (static const char *every = std::getenv("SHINO_TRACE_PLAYER"); every && f % std::max(1ull, std::strtoull(every, nullptr, 10)) == 0)
        {
            static FILE *trace = std::fopen("build/player_trace.bin", "wb");
            const uint8_t *ram = d.runtime().memory.ram();
            uint32_t object;
            std::memcpy(&object, ram + 0x625d64u, 4);
            if (trace && object)
            {
                const uint64_t frame = f;
                std::fwrite(&frame, 8, 1, trace);
                std::fwrite(ram + (object & 0x1FFFFFFu), 1, 0x2000u, trace);
                std::fflush(trace);
            }
        }
        // SHINO_POKE=<frame>:<address>:<x>,<y>,<z>[;...]: three floats written at a frame's end (the search for the
        // player's position: does writing there move him).
        if (static const char *poke = std::getenv("SHINO_POKE"); poke)
            for (const char *p = poke; p && *p;)
            {
                char *e = nullptr;
                const uint64_t at = std::strtoull(p, &e, 10);
                const uint32_t address = static_cast<uint32_t>(std::strtoul(e + 1, &e, 16));
                float v[3] = {};
                for (int k = 0; k < 3 && e && *e; ++k)
                    v[k] = std::strtof(e + 1, &e);
                if (at == f)
                {
                    std::memcpy(d.runtime().memory.ram() + (address & 0x1FFFFFFu), v, sizeof(v));
                    std::printf("[shino:poke] frame %llu: %g %g %g at %x\n", (unsigned long long)f, v[0], v[1], v[2], address);
                }
                p = e ? std::strchr(e, ';') : nullptr;
                if (p)
                    ++p;
            }
        shino::cheats::apply(d.runtime().memory.ram()); // the frame boundary: the game thread
        pacer.frameEnd();
        shino::traceSound(f);
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
        if (window && !mute)
            audio.push(pcm, count);
#endif
    };
    auto run = [&]()
    {
#ifdef SHINO_WITH_WINDOW
        if (window && !mute)
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
        shino::reportRender();
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
        shino::cheats::registerMenu(); // the Cheats tab (F2)
        result = bozo::app::runWindow(windowOptions, game, &devices);
    }
    else
#endif
        result = run();
    recording.close();
    return result;
}
