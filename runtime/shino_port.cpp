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
        bool nativeInstruction(uint8_t *, R5900Context *, Runtime *, uint32_t pc)
        {
            switch (pc)
            {
            default:
                return false;
            }
        }
    } // namespace

    void install(Runtime &, bozo::port::Registry &r)
    {
        r.nativeGraphics(&nativeInstruction);
    }
} // namespace shino
