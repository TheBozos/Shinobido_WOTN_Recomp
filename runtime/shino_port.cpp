// Shinobido: Way of the Ninja's native port boundary. No hardware execution or interpreter fallback.
//
// Overrides: each registerFunction(0x...) below is listed for bozo-externals by tools/shino_overrides_list.py, so a
// direct call in the generated code reaches the port's function. Native instruction sites: each
// `case 0x...u: // native site` line is listed as a "native 0xADDR" external: the instruction is replaced by the
// handler, inline. docs/RECOMP_NOTES.md says why each one is there.
#include "shino_port.h"
#include "bozo/ee/dispatcher.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace shino
{
    namespace
    {
        void returnZero(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }

        bool nativeInstruction(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t pc)
        {
            switch (pc)
            {
            // 0x444e40, the render scheduler's set-up: VIF0 / VIF1 error masks (ERR.ME0, mask the interrupts of
            // VIF codes' I bits). No VIF: reads give 0, writes nothing.
            case 0x00444f38u: // native site: lw a0,0x3820(at): VIF0_ERR
            case 0x00444f54u: // native site: lw a0,0x3820(at): VIF0_ERR
                ctx->r[4].ud[0] = 0;
                return true;
            case 0x00444f4cu: // native site: sw a0,0x3820(at): VIF0_ERR
            case 0x00444f60u: // native site: sw a0,0x3c20(at): VIF1_ERR
                return true;
            // 0x326860, the movie player's vblank handler: CSR.FIELD (bit 13) picks the field's half offset.
            case 0x00326880u: // native site: ld v0,0x1000(v0): CSR
                ctx->r[2].ud[0] = (bozo::port::vblankCount(rt) & 1u) << 13;
                if (static const bool trace = std::getenv("SHINO_TRACE_MOVIE") != nullptr; trace && bozo::port::vblankCount(rt) % 25 == 0)
                {
                    // The movie's audio feed (0x672540): +0 state, +0x38 EE ring bytes, +0x44 IOP buffer, +0x48 its
                    // size, +0x4c write position, +0x54 bytes sent; the display flag gp-0x6a7c.
                    const uint8_t *ram = rdram;
                    auto w = [&](uint32_t a) { uint32_t v; std::memcpy(&v, ram + a, 4); return v; };
                    const uint32_t s = 0x672540u;
                    std::printf("[shino:movie] vblank %llu state=%u ring=%x iop=%x size=%x pos=%x sent=%x display=%u\n",
                                (unsigned long long)bozo::port::vblankCount(rt), w(s), w(s + 0x38), w(s + 0x44), w(s + 0x48),
                                w(s + 0x4c), w(s + 0x54), w(0x62c770u - 0x6a7cu));
                }
                return true;
            default:
                return false;
            }
        }
    } // namespace

    void install(Runtime &, bozo::port::Registry &r)
    {
        // 0x4454a0, the game's path reset (from 0x44ff70, the graphics init): it resets VIF1, VIF0 and the GIF through
        // their FBRST / CTRL registers and the VUs through VI28 (cfc2/ctc2 FBRST), then calls sceGsResetPath and
        // sceDmaReset, both no-ops natively. There is no VIF, GIF or VU to reset.
        r.registerFunction(0x4454a0, returnZero);
        installRender(r);
        r.nativeGraphics(&nativeInstruction);
    }
} // namespace shino
