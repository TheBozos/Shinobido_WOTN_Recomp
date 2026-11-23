// Shinobido's sound modules: Sony's LIBSD and SDRDRV and Acquire's TORAST / TORASE, statically compiled from the
// player's disc (tools/shino_bozo_audio.py) and run by BozoEngine's native IRX host. The game reaches them through SIF
// RPC (libsdr's sceSdRemote: SDRDRV's 0x80000701; the driver's own servers) as on the console's IOP; no IOP is
// modelled. From Onimusha_Recomp's runtime/oni_audio.cpp.
#include "shino_audio_modules.h"
#include "shino_port.h"
#include "bozo/libs/libraries.h"
#include <cctype>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace shino
{
    namespace
    {
        std::unique_ptr<bozo::native::irx::CompiledLibraries> sound;
    }

    void installAudio(bozo::libs::Libraries &libraries, uint8_t *)
    {
        sound = std::make_unique<bozo::native::irx::CompiledLibraries>(libraries, kShinoAudioHeapStart);
        addShinoAudioImages(*sound);
        libraries.audioProvider = [](uint64_t cycle) { return sound->advanceTo(cycle); };
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
