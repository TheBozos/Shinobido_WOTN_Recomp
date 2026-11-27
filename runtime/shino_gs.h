// Shinobido's GS translator: the GS registers the game's GIF packets set, its vertices drawn natively and its image
// transfers kept for the textures (runtime/shino_gs_state.cpp). The GIF and VIF parsing is runtime/shino_gs.cpp.
#pragma once
#include <cstdint>

namespace bozo::render
{
    struct Vertex;
}

namespace shino::gs
{
    // A native draw (runtime/shino_native3d.cpp): triangles in the object's space with their object-to-GS-screen matrix
    // (column c is matrix[4c..4c+3]: GS pixels times w, GS Z times w, w), drawn with PRIM `prim`'s attributes (~0: the
    // PRIM set) and its context's registers and texture, in the frame's order with the GS packets' draws.
    void nativeTriangles(uint64_t prim, const float matrix[16], const bozo::render::Vertex *vertices, uint32_t count);
    // A native 2D draw: primitives of PRIM `prim` whose vertices are already the GS's (XY 12.4 as XYZ2 takes it, Z, F,
    // RGBA, S T Q), drawn as the GS translator draws its own (the same vertices, sprites, Q), the last vertex of each
    // with ADC set not drawn; the GS's registers left as the packets would leave them (PRIM, RGBAQ, ST, Q).
    struct ScreenVertex
    {
        uint32_t x = 0, y = 0, z = 0, fog = 0xFF, rgba = 0;
        float s = 0, t = 0, q = 1;
        bool adc = false;
    };
    // Widescreen (Hor+): the picture shown at 16:9, the main camera's view widened by the game itself (shino_port.cpp);
    // Place for the 2D: the particles (in the 3D's place: as their view is) or the HUD (each triangle 3/4 as wide
    // towards its third of the screen: the left edge, the centre or the right edge, its own shape kept).
    enum class Place
    {
        Scene,
        Hud,
    };
    void screenPrimitives(uint64_t prim, const ScreenVertex *vertices, uint32_t count, Place place = Place::Scene);
    // The window's hooks (window builds): whether widescreen is wanted (the Video tab's setting), and the picture's
    // shape for the frame flipped (16:9 or the default). SHINO_WIDESCREEN=1 wants it without a window (tests).
    extern bool (*widescreenWanted)();
    extern void (*widescreenShown)(bool wide);
    bool widescreenOn(); // wanted now (the setting, SHINO_WIDESCREEN)
    // The view a scene pass sets up, as the port marks its GS set-up (DTHE's bits 8-15, shino_port.cpp): the main
    // camera's (widened for 16:9 by the game itself, or not) or another's (the radar's). A frame whose main view was
    // widened is shown 16:9. (The game draws all the views' objects before the last view's set-up: a mark does not
    // tell whose draws follow.)
    enum class ViewKind : uint8_t
    {
        Unmarked = 0,
        MainWide = 1,
        Main = 2,
        Other = 3,
    };
    // A GS register written with its raw value (PRIM 0x00 ... LABEL 0x62): A+D, REGLIST, a tag's PRE.
    void writeRegister(uint32_t address, uint64_t value);
    // A PACKED quadword for register descriptor `r` (0x0 PRIM ... 0xe A+D, 0xf NOP).
    void packed(uint32_t r, uint64_t lo, uint64_t hi);
    // A PACKED tag's data in VU1 data memory (XGKICK): loops x nreg quadwords from quadword `at`, wrapping at 1024.
    void packedRun(const uint8_t *vuMemory, uint32_t at, uint64_t regs, uint32_t nreg, uint32_t loops);
    // A REGLIST value for descriptor `r` (the register address, 0x0-0xd).
    void reglist(uint32_t r, uint64_t value);
    // An IMAGE transfer's start (the BITBLTBUF / TRXPOS / TRXREG set before it) and its quadwords.
    void imageBegin();
    void imageData(const uint8_t *quadword);
    void reportState();
    // The flips so far (for traces keyed by frame).
    uint64_t flips();
}
