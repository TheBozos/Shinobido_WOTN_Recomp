// Native 3D (docs/NATIVE_3D.md): the 3D drawn by BozoEngine's renderer from the game's own vertex data instead of the
// GS packets its VU1 programs build. Stage 1: the main renderer's batches (microcode set 0x539c40, the variant at pair
// 1023): the VU program runs its setup and lighting, stops at the variant (vu::Unit::stopAt), and the batch is drawn
// from VU1 memory.
#pragma once

#include <cstdint>

namespace shino::vu
{
    struct Unit;
}

namespace shino::native3d
{
    // SHINO_NATIVE_3D: 0 (or unset) off; 1 on (the batch drawn natively, the variant still run for what it leaves in
    // VU1 memory and registers, its GS packets not sent); "skip" (the variant not run: its first pass's results written
    // back, the registers left as the stop leaves them; experimental); "verify" (each batch made natively and also by
    // the VU program, the clip positions and cull flags compared, the VU's drawn).
    enum class Mode
    {
        Off,
        On,
        Skip,
        Verify,
    };
    Mode mode();

    // The VU1 code memory holds a renderer some of whose variants are drawn natively: their entry pairs marked (the
    // program stops there, vu::Unit::stopPairs); null when none.
    const uint8_t *takeOverAt(const vu::Unit &v);

    // The batch the VU program stopped for at the variant entered at `pc`: drawn (submit) and remembered for compare().
    // In skip mode its buffer is left as the variant's first pass leaves it.
    void batch(vu::Unit &v, uint32_t pc, bool submit);
    // Verify: the VU program having run the variant on the same batch, its clip positions (slot 0) and cull flags
    // (slot 1's w) against the native batch's.
    void compare(const vu::Unit &v);
    void report();
    // Verify: while the VU runs a compared batch's variant, its XGKICKs' GIF packets are read here (each vertex's ST,
    // RGBAQ and XYZF2) to compare with the native batch's (compare()).
    // SHINO_NATIVE_OBJECTS (with SHINO_NATIVE_3D=1): 1, the main renderer's objects whose variant and lights are read
    // made here whole, no VU code run for them (their setup, lighting and draw); "verify", made here and by the VU, VU1
    // memory compared after. 0 off.
    int objectsMode();
    // A program starting at pc (bytes) with its batch at TOP (top), ITOP (itop) vertices: made here (drawn when
    // submit) when it is a native object's; false when the VU must run it.
    bool programNative(vu::Unit &v, uint32_t pc, uint32_t top, uint32_t itop, bool submit);
    // Verify: the VU having run the program, its VU1 memory (the inputs and constants) against native's.
    void compareObject(const vu::Unit &v, const uint8_t *native);
    // A program of a renderer drawn natively starts: its batch is at TOP (top), ITOP (itop) vertices.
    void programStart(const vu::Unit &v, uint32_t top, uint32_t itop);
    bool capturing();
    void captureKick(const uint8_t *vuMemory, uint32_t address);
    // A replayed variant having run (its kicks held and kept): its packets replayed in order, each strip of the batch
    // drawn natively, the rest sent to the GS translator.
    void finish(const vu::Unit &v);
}
