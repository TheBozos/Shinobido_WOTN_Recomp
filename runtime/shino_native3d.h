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
    bool capturing();
    void captureKick(const uint8_t *vuMemory, uint32_t address);
}
