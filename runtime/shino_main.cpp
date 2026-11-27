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

namespace
{
    // SHINO_ANSWER_DANGER=1 (play drivers): after some missions a rival ninja comes ("I sense danger... This might be
    // the end for me... Continue anyway?", the mission flow's states 13 then 14; the cursor starts on No). The assist answers
    // Yes (Left, then Cross), then fights until the flow leaves play: every 120 frames a teleport just behind the
    // nearest enemy and four Square presses. The script's own input is held back meanwhile.
    class DangerAssist final : public bozo::libs::PadInput
    {
    public:
        explicit DangerAssist(bozo::libs::PadInput *inner) : inner_(inner) {}

        void read(uint32_t frame, int player, bozo::libs::PadInputState &out) override
        {
            if (inner_)
                inner_->read(frame, player, out);
            if (player != 0)
                return;
            if (shino::cheats::grabWanted(frame))
            {
                out.buttons = bozo::libs::kPadTriangle;
                return;
            }
            const uint32_t state = shino::missionFlowState();
            if (state == 13 && phase_ == Phase::Idle)
            {
                phase_ = Phase::Answer;
                start_ = frame;
            }
            if (phase_ == Phase::Idle)
                return;
            const uint32_t at = frame - start_;
            out.buttons = 0;
            if (phase_ == Phase::Answer)
            {
                // the dialog is shown when the flow goes on to 14 (which stays through the rating): from then, Left
                // twice (onto Yes), then Cross
                if (state == 14 && shown_ == 0)
                    shown_ = frame;
                if (shown_ == 0)
                {
                    if (at > 600) // no dialog
                        phase_ = Phase::Idle;
                    return;
                }
                const uint32_t since = frame - shown_;
                if ((since >= 30 && since < 36) || (since >= 60 && since < 66))
                    out.buttons = bozo::libs::kPadLeft;
                else if (since >= 90 && since < 96)
                    out.buttons = bozo::libs::kPadCross;
                else if (since >= 100)
                {
                    phase_ = Phase::Fight;
                    start_ = frame;
                    shown_ = 0;
                    std::printf("[shino:assist] danger: Yes at frame %u, fighting\n", frame);
                }
                return;
            }
            // fighting: until the mission is over (12 complete or failed, or back at the hideout), or 9000 frames
            if (state == 12 || state == 15 || state == 17 || state == 0 || at > 9000) // 17: the encounter's scene over
            {
                std::printf("[shino:assist] fight over at frame %u (state %u)\n", frame, state);
                phase_ = Phase::Idle;
                return;
            }
            const uint32_t step = at % 120;
            if (step == 0)
                shino::cheats::teleportToEnemy();
            if ((step >= 15 && step < 21) || (step >= 35 && step < 41) || (step >= 55 && step < 61) || (step >= 75 && step < 81))
                out.buttons = bozo::libs::kPadSquare;
            else if ((step >= 95 && step < 99) || (step >= 107 && step < 111)) // Grab: a soul fragment a leader drops
                out.buttons = bozo::libs::kPadTriangle;
        }

    private:
        enum class Phase { Idle, Answer, Fight };
        bozo::libs::PadInput *inner_;
        Phase phase_ = Phase::Idle;
        uint32_t start_ = 0, shown_ = 0;
    };
} // namespace

int main(int argc, char **argv)
{
    bozo::host::FreeOptions options;
    // SHINO_60FPS=1 (experimental): fields at NTSC's rate and a frame every field; the game's frame timer told so
    // (shino_render.cpp, frameTimer60).
    const bool fps60 = std::getenv("SHINO_60FPS") && *std::getenv("SHINO_60FPS") == '1';
    const uint32_t fieldsPerSecond = fps60 ? 60u : 50u;
    options.framePeriodCycles = 294912000 / fieldsPerSecond; // PAL (VMODE = PAL in SYSTEM.CNF)
    bool window = false;
    bool mute = false; // --mute: no sound through the speakers (hidden runs, tests, captures)
#ifdef SHINO_WITH_WINDOW
    window = true;
    bozo::app::setIdentity({"Shinobido: Way of the Ninja", "Shinobido_WOTN_Recomp", "shinobido-recomp", "SCES_539.31"});
    bozo::app::WindowOptions windowOptions;
    windowOptions.title = "Shinobido: Way of the Ninja - BozoEngine";
    windowOptions.width = 960;
    windowOptions.height = 720;
    // SHINO_CAPTURE_FRAMES=<item>[,<item>...], each <frame> or <first>:<last>:<step>: the window's picture at those game
    // frames (the pad script's frames), as frame_<n>.png in PS2X_CAPTURE_WINDOW_DIR (play tests: what the screen shows).
    if (const char *e = std::getenv("SHINO_CAPTURE_FRAMES"))
        for (const char *p = e; p && *p;)
        {
            unsigned long long first, last, step;
            if (std::sscanf(p, "%llu:%llu:%llu", &first, &last, &step) == 3 && step > 0)
                for (unsigned long long f = first; f <= last; f += step)
                    windowOptions.captureFrames.push_back(f);
            else if (std::sscanf(p, "%llu", &first) == 1)
                windowOptions.captureFrames.push_back(first);
            p = std::strchr(p, ',');
            if (p)
                ++p;
        }
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
    // SHINO_REAL_TIME=0: a window run on the EE clock alone (nothing host-timed: the same frames as headless), for
    // captures to compare between builds or modes.
    options.realTime = window && !(std::getenv("SHINO_REAL_TIME") && *std::getenv("SHINO_REAL_TIME") == '0');
    options.gameTag = "SCES-53931"; // a save state loads only into Shinobido
    bozo::host::FreeHost host(options);
    bozo::ee::Dispatcher d(host);
    shino::installAudio(host.libraries(), d.runtime().memory.ram());
    shino::installState(*host.stateSections());
    // The movies (libmpeg native): the SDK's results, pictures as soon as decoded (the player queues them and shows
    // them from its own vblank handler), and a read-ahead long enough for the audio the player waits for before its
    // first picture (the first audio packet comes after 14 pictures; it wants 0x6000 bytes).
    host.libraries().mpegOptions = {.referenceLayout = false, .paced = false, .sdkNodata = false, .picturesAhead = 32, .sdkDemux = true};
    // A PAL disc (SYSTEM.CNF: VMODE = PAL): the game's graphics init asks for the console's own video mode (0), which
    // is PAL's 50 fields a second here; the movie player asks for PAL explicitly.
    host.libraries().palConsole = !fps60;
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
        pacer.setCap(fps60 ? 60 : 25);
    uint64_t lastFrameCycles = 0;
    host.onFrame = [&](uint64_t f, const R5900Context *ctx)
    {
        if (static bool rateSet = false; fps60 && !rateSet) // a save state keeps the field rate it was made with
        {
            d.kernel().fieldRate(60000, 1001);
            rateSet = true;
        }
        const uint64_t now = d.now();
        if (!capFromEnvironment && lastFrameCycles && now > lastFrameCycles)
        {
            const double fields = double(now - lastFrameCycles) / double(options.framePeriodCycles);
            const uint32_t spanned = static_cast<uint32_t>(std::clamp(fields + 0.5, 1.0, 10.0));
            pacer.setCap(fieldsPerSecond / spanned);
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
        // SHINO_TRACE_CHARACTERS=<every>: every character of the table at gp-0x6d38 (0x625a38: 16-byte entries from +0x20,
        // the object at +4), its first 4 KB, every n frames into build/characters_trace.bin (frame, count, then each
        // object's address and bytes): the search for the guards' fields.
        if (static const char *every = std::getenv("SHINO_TRACE_CHARACTERS"); every && f % std::max(1ull, std::strtoull(every, nullptr, 10)) == 0)
        {
            static FILE *trace = std::fopen("build/characters_trace.bin", "wb");
            const uint8_t *ram = d.runtime().memory.ram();
            uint32_t table;
            std::memcpy(&table, ram + 0x625a38u, 4);
            if (trace && table >= 0x100000u && table < 0x1F00000u)
            {
                uint32_t objects[64];
                uint32_t count = 0;
                for (uint32_t i = 0; i < 64; ++i)
                {
                    uint32_t object, self;
                    std::memcpy(&object, ram + table + 0x20 + 16 * i + 4, 4);
                    if (object < 0x100000u || object >= 0x1FFF000u)
                        continue;
                    std::memcpy(&self, ram + object, 4);
                    if (self == object)
                        objects[count++] = object;
                }
                const uint64_t frame = f;
                std::fwrite(&frame, 8, 1, trace);
                std::fwrite(&count, 4, 1, trace);
                for (uint32_t i = 0; i < count; ++i)
                {
                    std::fwrite(&objects[i], 4, 1, trace);
                    std::fwrite(ram + objects[i], 1, 0x1000u, trace);
                }
                std::fflush(trace);
            }
        }
        // SHINO_TRACE_PLAYER=<every>[:<address>]: the player's object (or the one at that address) (the pointer at gp-0x6a0c, 0x625d64), its first 8 KB, every n
        // frames into build/player_trace.bin (frame, then the bytes): the search for its fields.
        if (static const char *every = std::getenv("SHINO_TRACE_PLAYER"); every && f % std::max(1ull, std::strtoull(every, nullptr, 10)) == 0)
        {
            static FILE *trace = std::fopen("build/player_trace.bin", "wb");
            const uint8_t *ram = d.runtime().memory.ram();
            uint32_t object;
            std::memcpy(&object, ram + 0x625d64u, 4);
            if (const char *colon = std::strchr(every, ':')) // <every>:<address>: that object instead (a guard's)
                object = static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 16));
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
        // SHINO_TELEPORT_AT=<frame>:<x>,<y>,<z>[;...]: teleports at those frames (play tests: a place to look at);
        // <frame>:goal, <frame>:item, <frame>:enemy teleport to those places of the mission, <frame>:char<n> behind the
        // character table's entry n, <frame>:done marks the mission completed, <frame>:walk<x>,<z>[,<step>] queues a
        // walk to (x, z), step units a frame (shino_cheats.h).
        if (static const char *e = std::getenv("SHINO_TELEPORT_AT"); e)
            for (const char *p = e; p && *p;)
            {
                unsigned long long at;
                float v[3];
                char word[8] = {};
                if (std::sscanf(p, "%llu:%f,%f,%f", &at, &v[0], &v[1], &v[2]) == 4 && at == f)
                    shino::cheats::teleportTo(v[0], v[1], v[2]);
                else if (float w[4]; std::sscanf(p, "%llu:fly%f,%f,%f,%f", &at, &w[0], &w[1], &w[2], &w[3]) >= 4 && at == f)
                    shino::cheats::flyTo(w[0], w[1], w[2], std::sscanf(p, "%llu:fly%f,%f,%f,%f", &at, &w[0], &w[1], &w[2], &w[3]) == 5 ? w[3] : 0.2f);
                else if (int n = std::sscanf(p, "%llu:walk%f,%f,%f", &at, &v[0], &v[1], &v[2]); n >= 3 && at == f)
                    shino::cheats::walkTo(v[0], v[1], n == 4 ? v[2] : 0.2f);
                else if (int index; std::sscanf(p, "%llu:char%d", &at, &index) == 2 && at == f)
                    shino::cheats::teleportToCharacter(index);
                else if (std::sscanf(p, "%llu:%5[a-z]", &at, word) == 2 && at == f)
                {
                    if (std::strcmp(word, "goal") == 0)
                        shino::cheats::teleportToGoal();
                    else if (std::strcmp(word, "item") == 0)
                        shino::cheats::teleportToItem();
                    else if (std::strcmp(word, "enemy") == 0)
                        shino::cheats::teleportToEnemy();
                    else if (std::strcmp(word, "done") == 0)
                        shino::cheats::completeMission();
                }
                p = std::strchr(p, ';');
                if (p)
                    ++p;
            }
        // SHINO_POKE_AT=<frame>:<hex address>=<float>[,<float>...][;...]: floats written there at that frame (tests:
        // an object moved by hand), repeated every frame up to <frame>+<n> with <frame>+<n>:... (one entry a frame)
        if (static const char *e = std::getenv("SHINO_POKE_AT"); e)
            for (const char *p = e; p && *p;)
            {
                unsigned long long at;
                unsigned address;
                int used = 0;
                if (std::sscanf(p, "%llu:%x=%n", &at, &address, &used) == 2 && used > 0 && at == f && address < 0x2000000u)
                {
                    const char *q = p + used;
                    uint8_t *ram = d.runtime().memory.ram();
                    for (uint32_t k = 0; q && *q && *q != ';' && address + 4 * k + 4 <= 0x2000000u; ++k)
                    {
                        const float v = std::strtof(q, nullptr);
                        std::memcpy(ram + address + 4 * k, &v, 4);
                        q = std::strpbrk(q, ",;");
                        if (!q || *q == ';')
                            break;
                        ++q;
                    }
                }
                p = std::strchr(p, ';');
                if (p)
                    ++p;
            }
        shino::cheats::setFrame(f);
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
    DangerAssist dangerAssist(host.libraries().pad.input);
    if (const char *e = std::getenv("SHINO_ANSWER_DANGER"); e && *e == '1')
        host.libraries().pad.input = &dangerAssist;
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
