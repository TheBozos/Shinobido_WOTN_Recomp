// Shinobido: Way of the Ninja's native port boundary.
#pragma once
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include <cstdint>

namespace shino
{
    // The port's overrides (registerFunction by address) and native instruction sites (runtime/shino_port.cpp).
    void install(Runtime &rt, bozo::port::Registry &registry);
    // The IOP sound modules (runtime/shino_audio.cpp): module loads, SIF transfers and the mixer.
    void installAudio(bozo::libs::Libraries &libraries, uint8_t *eeRam);
    // The render scheduler and the vblank flip, native (runtime/shino_render.cpp).
    void installRender(bozo::port::Registry &registry);
    // What the frames held, for the runner's report.
    void reportRender();

    // The GS translator (runtime/shino_gs.cpp): the GIF packets of the game's transfers drawn natively.
    namespace gs
    {
        // GIF packets (tags and their data), as the GIF would take them from PATH3.
        void packets(const uint8_t *data, uint32_t bytes);
        // One quadword of VIF1 DIRECT data (PATH2).
        void directQuadword(const uint8_t *quadword);
        // A VU1 XGKICK (PATH1): the packets at `address` (quadwords) of VU1 data memory, up to EOP.
        void path1(const uint8_t *vuMemory, uint32_t address);
        // The display flip: the frame drawn so far is published (DISPFB1 of the buffer shown); the game frame ends.
        void flip(Runtime *rt, R5900Context *ctx, uint64_t dispfb, uint64_t display);
        void report();
    }

    // VIF1 codes and the VU1 (runtime/shino_vu1.cpp): DIRECT / DIRECTHL go to the GS translator, UNPACK and MPG fill
    // the VU1's memories, MSCAL runs its programs (their XGKICKs go to the GS translator).
    namespace vif
    {
        // tagWords: the two VIF codes of a DMA tag's upper half (TTE).
        void codes(const uint8_t *data, uint32_t bytes, bool tagWords);
        void report();
    }
} // namespace shino
