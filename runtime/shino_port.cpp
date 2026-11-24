// Shinobido: Way of the Ninja's native port boundary. No hardware execution or interpreter fallback.
//
// Overrides: each registerFunction(0x...) below is listed for bozo-externals by tools/shino_overrides_list.py, so a
// direct call in the generated code reaches the port's function. Native instruction sites: each
// `case 0x...u: // native site` line is listed as a "native 0xADDR" external: the instruction is replaced by the
// handler, inline. docs/RECOMP_NOTES.md says why each one is there.
#include "shino_cheats.h"
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

        // 0x112d60(character, position, type, source): a character's event record (+0x3e4 the type, +0x3e8 the position
        // it is at, +0x3e0 the one it is about: the guards' "seen", type 17, among them), the game's three stores made
        // natively. With the Unseen cheat a "seen" whose source is the player is not recorded (with the stimulus push's
        // native site, 0x16b93c). SHINO_TRACE_SEEN=1: each "seen" and its caller.
        void characterEvent(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t character = ctx->r[4].uw[0], position = ctx->r[5].uw[0], type = ctx->r[6].uw[0];
            const uint32_t source = ctx->r[7].uw[0];
            const bool seen = type == 0x11u;
            if (static const char *all = std::getenv("SHINO_TRACE_EVENTS"); all && std::strtoul(all, nullptr, 16) == character)
                std::printf("[shino:event] %x type %x source %x position %x (from %x) vblank %llu\n", character, type, source,
                            position, ctx->r[31].uw[0], (unsigned long long)bozo::port::vblankCount(rt));
            if (static const bool trace = std::getenv("SHINO_TRACE_SEEN") != nullptr; trace && seen)
                std::printf("[shino:seen] %x sees %x (from %x)\n", character, source, ctx->r[31].uw[0]);
            if (!(seen && cheats::hidesSource(rdram, source, position)))
            {
                auto put = [&](uint32_t at, uint32_t v) { std::memcpy(rdram + ((character + at) & 0x1FFFFFFu), &v, 4); };
                put(0x3e4u, type);
                put(0x3e8u, position);
                put(0x3e0u, source);
            }
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
            // 0x16b8c0(queue a1, source a2, priority a3, kind t0, position t1), the first step of 0x16b3a0, the push of a
            // stimulus into a guard's brain (its three queues at +0x394, +0x3a4, +0x3b4, from which the brain takes its
            // target): it refreshes the queue's record of that source and answers 1 when there was one, and the push then
            // adds nothing. The Unseen cheat answers 1 for the player: no new stimulus of his reaches a guard's brain (a
            // refresh calls sceVu0CopyVector, so its argument registers are gone here: only a record already queued, from
            // before the cheat, is refreshed). SHINO_TRACE_STIMULI=1: each new stimulus and the push's callers.
            case 0x0016b93cu: // native site: ld ra,0(sp): the stimulus update's return
            {
                std::memcpy(&ctx->r[31].ud[0], rdram + (ctx->r[29].uw[0] & 0x1FFFFFFu), 8);
                if (ctx->r[2].uw[0] != 0)
                    return true;
                const uint32_t source = ctx->r[6].uw[0];
                const bool hidden = cheats::hidesSource(rdram, source, 0);
                if (static const bool trace = std::getenv("SHINO_TRACE_STIMULI") != nullptr; trace)
                {
                    // The push's caller (0x16b3a0 keeps ra at +0x70 of its 0xc0 frame) and, through 0x16b790 (ra at +0x60
                    // of 0x80), the one above.
                    auto w = [&](uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x1FFFFFFu), 4); return v; };
                    float at[3];
                    std::memcpy(at, rdram + (ctx->r[9].uw[0] & 0x1FFFFFFu), sizeof at);
                    const uint32_t sp = ctx->r[29].uw[0], caller = w(sp + 0x10 + 0x70), above = w(sp + 0xd0 + 0x60);
                    std::printf("[shino:stimulus] queue %x source %x priority %d kind %d at %.1f %.1f %.1f%s from %x, %x vblank "
                                "%llu\n", ctx->r[5].uw[0], source, ctx->r[7].sw[0], ctx->r[8].sw[0], at[0], at[1], at[2],
                                hidden ? " hidden" : "", caller, above, (unsigned long long)bozo::port::vblankCount(rt));
                }
                if (hidden)
                    ctx->r[2].ud[0] = 1;
                return true;
            }
            // 0x373c40(mission, event, flag): the mission's event (its byte +0x50, what the script's GetEventTiming reads:
            // its start, the target found, the item delivered, the player dead...). SHINO_TRACE_MISSION_EVENTS=1: each.
            case 0x00373c54u: // native site: sb s1,0x50(a0): the mission's event
            {
                rdram[(ctx->r[4].uw[0] + 0x50u) & 0x1FFFFFFu] = static_cast<uint8_t>(ctx->r[17].uw[0]);
                if (static const bool trace = std::getenv("SHINO_TRACE_MISSION_EVENTS") != nullptr; trace)
                    std::printf("[shino:mission] event %u (flag %u) from %x vblank %llu%c", ctx->r[17].uw[0] & 0xffu,
                                ctx->r[6].uw[0], ctx->r[31].uw[0], (unsigned long long)bozo::port::vblankCount(rt), 10);
                return true;
            }
            // 0x180470(manager, state): the mission flow's state machine (+0xc its state, 23 of them; some raise the
            // mission's event through 0x2f7bf0). SHINO_TRACE_MISSION_EVENTS=1: each change too.
            case 0x00180484u: // native site: sw a1,0xc(a0): the mission flow's state
            {
                const uint32_t at = (ctx->r[4].uw[0] + 0xcu) & 0x1FFFFFFu;
                if (static const bool trace = std::getenv("SHINO_TRACE_MISSION_EVENTS") != nullptr; trace)
                {
                    uint32_t old;
                    std::memcpy(&old, rdram + at, 4);
                    std::printf("[shino:mission] %x state %u -> %u from %x vblank %llu%c", ctx->r[4].uw[0], old,
                                ctx->r[5].uw[0], ctx->r[31].uw[0], (unsigned long long)bozo::port::vblankCount(rt), 10);
                }
                std::memcpy(rdram + at, &ctx->r[5].uw[0], 4);
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
        r.registerFunction(0x112d60, characterEvent);
        installRender(r);
        r.nativeGraphics(&nativeInstruction);
    }
} // namespace shino
