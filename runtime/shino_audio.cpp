// Shinobido's sound modules: Sony's LIBSD and SDRDRV and Acquire's TORAST / TORASE, statically compiled from the
// player's disc (tools/shino_bozo_audio.py) and run by BozoEngine's native IRX host. The game reaches them through SIF
// RPC (libsdr's sceSdRemote: SDRDRV's 0x80000701; the driver's own servers) as on the console's IOP; no IOP is
// modelled. From Onimusha_Recomp's runtime/oni_audio.cpp.
#include "shino_audio_modules.h"
#include "shino_port.h"
#include "bozo/audio/mixer.h"
#include "bozo/libs/guest.h"
#include "bozo/libs/libraries.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace shino
{
    namespace
    {
        std::unique_ptr<bozo::native::irx::CompiledLibraries> sound;
        bozo::audio::Mixer *g_mixer = nullptr; // the one advanceTo hands back (SHINO_TRACE_SOUND)
        bozo::libs::Libraries *g_libraries = nullptr;
        // SHINO_TRACE_RPC=1: the drivers' RPC calls by service and command, every 250 frames (each server wrapped once,
        // when the trace first sees it: the modules register theirs as the game loads them).
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_rpcCalls;
        std::set<uint32_t> g_wrapped;
        void wrapServers()
        {
            for (auto &[service, handler] : g_libraries->sif.servers)
                if (g_wrapped.insert(service).second)
                    handler = [service, inner = handler](bozo::libs::Libraries &l, bozo::libs::Guest &g,
                                                         const bozo::libs::NativeRpcRequest &r) {
                        ++g_rpcCalls[{service, r.command}];
                        bool empty = true; // counted apart: the calls whose send buffer holds anything (command | 1)
                        for (uint32_t i = 0; i + 4 <= r.sendBytes && empty; i += 4)
                            empty = g.read32(r.send + i) == 0;
                        if (!empty && r.sendBytes)
                            ++g_rpcCalls[{service, r.command | 1u}];
                        // SHINO_TRACE_RPC_DUMP=<frame>: each call's send buffer from that frame on (16 calls)
                        static const char *dump = std::getenv("SHINO_TRACE_RPC_DUMP");
                        static int dumped = 0;
                        static const char *only = std::getenv("SHINO_TRACE_RPC_SERVICE"); // one service's calls alone
                        if (dump && l.framesEnded >= std::strtoull(dump, nullptr, 10) && dumped < 16 &&
                            (!only || std::strtoul(only, nullptr, 16) == service))
                        {
                            ++dumped;
                            std::printf("[shino:rpc] %x/%x send %x (%u bytes) receive %x (%u):", service, r.command, r.send,
                                        r.sendBytes, r.receive, r.receiveBytes);
                            for (uint32_t i = 0; i < r.sendBytes && i < 512; i += 4)
                                std::printf(" %08x", g.read32(r.send + i));
                            std::printf("|end\n");
                        }
                        inner(l, g, r);
                    };
        }
    }

    void traceSound(uint64_t frame)
    {
        static const bool rpc = std::getenv("SHINO_TRACE_RPC") != nullptr;
        if (rpc && g_libraries)
        {
            wrapServers();
            if (frame % 250 == 0 && !g_rpcCalls.empty())
            {
                std::printf("[shino:rpc] frame %llu:", (unsigned long long)frame);
                for (const auto &[key, count] : g_rpcCalls)
                    std::printf(" %x/%x=%llu", key.first, key.second, (unsigned long long)count);
                std::printf("\n");
                g_rpcCalls.clear();
            }
        }
        static const bool on = std::getenv("SHINO_TRACE_SOUND") != nullptr;
        static const uint64_t every = std::getenv("SHINO_TRACE_SOUND") ? std::max(1ull, std::strtoull(std::getenv("SHINO_TRACE_SOUND"), nullptr, 10)) : 50;
        if (!on || !g_mixer || frame % every != 0)
            return;
        const auto s = g_mixer->stats();
        static uint64_t keyOnsBefore = 0;
        std::printf("[shino:sound] frame %llu: %llu key ons, %d voices playing, peak %d", (unsigned long long)frame,
                    (unsigned long long)(s.keyOns - keyOnsBefore), s.voicesPlaying, s.peak);
        keyOnsBefore = s.keyOns;
        // The input (the movies' PCM) and the volumes and mixes it goes through, per group (core).
        for (int g = 0; g < 2; ++g)
            std::printf("; core %d input %d vol %04x/%04x master %04x/%04x mix %03x", g, int(g_mixer->inputPlaying(g)),
                        g_mixer->inputVolume(g, 0), g_mixer->inputVolume(g, 1), g_mixer->masterVolume(g, 0),
                        g_mixer->masterVolume(g, 1), g_mixer->mix(g));
        std::printf("\n");
    }

    void installAudio(bozo::libs::Libraries &libraries, uint8_t *)
    {
        sound = std::make_unique<bozo::native::irx::CompiledLibraries>(libraries, kShinoAudioHeapStart);
        g_libraries = &libraries;
        // The movie sound keeps its IOP ring full by sceSdBlockTransStatus: the SPU2's transfer address, a block ahead
        // of the sample playing (docs/RECOMP_NOTES.md, Movies).
        sound->useBlockTransStatusAhead();
        addShinoAudioImages(*sound);
        libraries.audioProvider = [](uint64_t cycle) { return g_mixer = sound->advanceTo(cycle); };
        libraries.sif.transfer = [](uint32_t address, const uint8_t *bytes, uint32_t count)
        { sound->writeBuffer(address, bytes, count); };
        libraries.sif.allocateBuffer = [](uint32_t bytes) { return sound->allocateBuffer(bytes); };
        libraries.sif.freeBuffer = [](uint32_t address) { return sound->freeBuffer(address); };
        libraries.sif.resetProvider = [] { sound->reset(); };
        libraries.sif.moduleLoaderWithArguments = [](const std::string &path, const std::vector<std::string> &arguments)
        {
            auto name = path.substr(path.find_last_of("/\\") + 1);
            if (const auto version = name.find(';'); version != std::string::npos)
                name.resize(version);
            for (char &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            // Answered at their EE APIs by BozoEngine's native pad, memory-card and CD libraries; the USB link to a
            // PSP (USBD, USBPSPCM) is not used by the game's play.
            if (name == "SIO2MAN.IRX" || name == "PADMAN.IRX" || name == "MCMAN.IRX" || name == "MCSERV.IRX" ||
                name == "CDVDSTM.IRX" || name == "USBD.IRX" || name == "USBPSPCM.IRX")
                return 1;
            if (name == "LIBSD.IRX" || name == "SDRDRV.IRX" || name == "TORAST.IRX" || name == "TORASE.IRX")
                return sound->startImage(name + ";1", arguments);
            std::printf("[shino:iop] module load not answered: %s\n", path.c_str());
            return -1;
        };
    }
} // namespace shino
