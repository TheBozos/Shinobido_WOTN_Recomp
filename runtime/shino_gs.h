// Shinobido's GS translator: the GS registers the game's GIF packets set, its vertices drawn natively and its image
// transfers kept for the textures (runtime/shino_gs_state.cpp). The GIF and VIF parsing is runtime/shino_gs.cpp.
#pragma once
#include <cstdint>

namespace shino::gs
{
    // A GS register written with its raw value (PRIM 0x00 ... LABEL 0x62): A+D, REGLIST, a tag's PRE.
    void writeRegister(uint32_t address, uint64_t value);
    // A PACKED quadword for register descriptor `r` (0x0 PRIM ... 0xe A+D, 0xf NOP).
    void packed(uint32_t r, uint64_t lo, uint64_t hi);
    // A REGLIST value for descriptor `r` (the register address, 0x0-0xd).
    void reglist(uint32_t r, uint64_t value);
    // An IMAGE transfer's start (the BITBLTBUF / TRXPOS / TRXREG set before it) and its quadwords.
    void imageBegin();
    void imageData(const uint8_t *quadword);
    void reportState();
    // The flips so far (for traces keyed by frame).
    uint64_t flips();
}
