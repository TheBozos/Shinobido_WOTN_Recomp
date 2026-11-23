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
    // A VIF0 source chain (TTE: its tags' VIF codes too), its codes read into the port's VU0 (vif::codes0).
    void vif0Chain(uint8_t *rdram, Runtime *rt, uint32_t tadr);

    // The GS translator (runtime/shino_gs.cpp): the GIF packets of the game's transfers drawn natively.
    namespace gs
    {
        // GIF packets (tags and their data), as the GIF would take them from PATH3.
        void packets(const uint8_t *data, uint32_t bytes);
        // IMAGE data (whole quadwords) for the host-to-local transfer under way.
        void imageBytes(const uint8_t *data, uint32_t bytes);
        // One quadword of VIF1 DIRECT data (PATH2).
        void directQuadword(const uint8_t *quadword);
        // A VU1 XGKICK (PATH1): the packets at `address` (quadwords) of VU1 data memory, up to EOP.
        void path1(const uint8_t *vuMemory, uint32_t address);
        // The display flip: the frame drawn so far is published (DISPFB1 of the buffer shown); the game frame ends.
        void flip(Runtime *rt, R5900Context *ctx, uint64_t dispfb, uint64_t display);
        void report();
    }

    // SHINO_PROFILE=1: the game thread's time by part, exclusive (a scope pauses the one it interrupts), printed every
    // 250 flips with the flips' rate; "other" is the recompiled code, the libraries and the host.
    namespace prof
    {
        enum Bucket : int { Other, Vu1, Vu0, Gs, Decode, Submit, Vif, Upload, Count };
        bool on();
        void enter(Bucket b, Bucket &previous);
        void leave(Bucket previous);
        void flip();
        struct Scope
        {
            explicit Scope(Bucket b) : active(on()) { if (active) enter(b, previous); }
            ~Scope() { if (active) leave(previous); }
            bool active;
            Bucket previous = Other;
        };
    }

    // PATH3 held behind VIF1's MSKPATH3 (runtime/shino_render.cpp): the GIF chain sent with a VIF1 chain goes to the
    // GS one EOP packet group at each unmask, between the draws, as the GIF lets it through.
    namespace path3
    {
        void unmasked();
    }

    // VIF1 codes and the VU1 (runtime/shino_vu1.cpp): DIRECT / DIRECTHL go to the GS translator, UNPACK and MPG fill
    // the VU1's memories, MSCAL runs its programs (their XGKICKs go to the GS translator).
    namespace vif
    {
        // tagWords: the two VIF codes of a DMA tag's upper half (TTE).
        void codes(const uint8_t *data, uint32_t bytes, bool tagWords);
        // VIF0 codes (MPG and UNPACK to VU0's memories, MSCAL runs a VU0 program).
        void codes0(Runtime *rt, const uint8_t *data, uint32_t bytes);
        void report();
    }
    // VU0 microprograms (runtime/shino_vu1.cpp's interpreter): the program at pcBytes of the code VIF0 loaded, on the
    // EE's COP2 registers and the runtime's VU0 data memory (VCALLMS).
    namespace vu0
    {
        void run(Runtime *rt, R5900Context *ctx, uint32_t pcBytes);
        // Its 4 KB code memory (the EE's view at 0x11000000: port::Registry::vu0CodeMemory).
        uint8_t *codeMemory();
    }
} // namespace shino
