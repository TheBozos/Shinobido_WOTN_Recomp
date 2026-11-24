// Shinobido: Way of the Ninja's native port boundary. No hardware execution or interpreter fallback.
//
// Overrides: each registerFunction(0x...) below is listed for bozo-externals by tools/shino_overrides_list.py, so a
// direct call in the generated code reaches the port's function. Native instruction sites: each
// `case 0x...u: // native site` line is listed as a "native 0xADDR" external: the instruction is replaced by the
// handler, inline. docs/RECOMP_NOTES.md says why each one is there.
#include "shino_port.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/ee/kernel.h"
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

        // The frame-end wait (0x443728): its passes, the times the clock went to the next event and the cycles it
        // went, the passes that waited with an idle callback (left as they are).
        struct WaitStats
        {
            uint64_t passes = 0, skips = 0, cycles = 0, withCallback = 0;
        } g_wait;

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
            // The 3D's VU0 data senders (0x1ec118, 0x21c8a0): CHCR |= TTE, then sceDmaSend(VIF0, a static chain). No
            // VIF0: the CHCR read gives 0 and the write is dropped (libdma's native send completes the transfer), and
            // the chain goes to the port's VU0 where the delay slot of the sceDmaSend call makes its address.
            case 0x001ec2ccu: // native site: lw v1,0(v0): VIF0 CHCR
            case 0x0021c8c8u: // native site: lw v1,0(v0): VIF0 CHCR
                ctx->r[3].ud[0] = 0;
                return true;
            case 0x001ec2dcu: // native site: sw v1,0(v0): VIF0 CHCR
            case 0x0021c8d8u: // native site: sw v1,0(v0): VIF0 CHCR
                return true;
            case 0x001ec2ecu: // native site: addiu a1,a1,0x1500: sceDmaSend's chain
            case 0x0021c8e8u: // native site: addiu a1,a1,0x1a80: sceDmaSend's chain
                ctx->r[5].sd[0] = static_cast<int32_t>(ctx->r[5].uw[0] + (pc == 0x001ec2ecu ? 0x1500u : 0x1a80u));
                vif0Chain(rdram, rt, ctx->r[5].uw[0]);
                return true;
            // 0x443640's frame-end wait (0x443728 to 0x443760): it loops, 32 EE cycles a pass, while the two frame
            // counters gp-0x6810 / gp-0x6814 differ or the flag gp-0x6800 is set (the vblank's handler changes them),
            // calling the idle callback gp-0x6854 each pass when there is one. With none, nothing but an event can end
            // the wait: the clock goes to the next event at once (as the dispatcher idles when every thread waits)
            // instead of hundreds of thousands of passes a frame. SHINO_NO_WAIT_SKIP=1: the loop as it is.
            case 0x00443728u: // native site: lw v1,-0x6810(gp): the frame-end wait's poll
            {
                const uint32_t gp = ctx->r[28].uw[0];
                auto w = [&](uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4); return v; };
                const uint32_t shown = w(gp - 0x6810u), queued = w(gp - 0x6814u);
                ctx->r[3].sd[0] = static_cast<int32_t>(shown); // the load the site replaces
                ++g_wait.passes;
                const bool waiting = shown != queued || rdram[(gp - 0x6800u) & 0x1FFFFFFu] != 0;
                static const bool skip = std::getenv("SHINO_NO_WAIT_SKIP") == nullptr;
                const bool callback = w(gp - 0x6854u) != 0;
                if (waiting && callback)
                    ++g_wait.withCallback;
                else if (waiting && skip)
                {
                    const uint64_t now = bozo::port::now(rt), next = bozo::port::kernel(rt).nextEvent();
                    if (next > now && next != ~0ull)
                    {
                        bozo::port::accountCycles(rt, ctx, next - now);
                        ++g_wait.skips;
                        g_wait.cycles += next - now;
                    }
                }
                return true;
            }
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

    void reportWait()
    {
        std::printf("[shino:wait] frame-end wait: %llu passes, %llu went to the next event (%llu cycles), %llu with an idle "
                    "callback\n", (unsigned long long)g_wait.passes, (unsigned long long)g_wait.skips,
                    (unsigned long long)g_wait.cycles, (unsigned long long)g_wait.withCallback);
    }

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
