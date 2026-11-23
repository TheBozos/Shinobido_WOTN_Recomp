// Shinobido: Way of the Ninja's native port boundary. No hardware execution or interpreter fallback.
//
// Overrides: each registerFunction(0x...) below is listed for bozo-externals by tools/shino_overrides_list.py, so a
// direct call in the generated code reaches the port's function. Native instruction sites: each
// `case 0x...u: // native site` line is listed as a "native 0xADDR" external: the instruction is replaced by the
// handler, inline. docs/RECOMP_NOTES.md says why each one is there.
#include "shino_port.h"
#include "bozo/ee/dispatcher.h"

namespace shino
{
    namespace
    {
        void returnZero(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }

        bool nativeInstruction(uint8_t *, R5900Context *ctx, Runtime *rt, uint32_t pc)
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
