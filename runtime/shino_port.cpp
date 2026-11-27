// Shinobido: Way of the Ninja's native port boundary. No hardware execution or interpreter fallback.
//
// Overrides: each registerFunction(0x...) below is listed for bozo-externals by tools/shino_overrides_list.py, so a
// direct call in the generated code reaches the port's function. Native instruction sites: each
// `case 0x...u: // native site` line is listed as a "native 0xADDR" external: the instruction is replaced by the
// handler, inline. docs/RECOMP_NOTES.md says why each one is there.
#include "shino_cheats.h"
#include "shino_gs.h"
#include "shino_port.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/ee/kernel.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>
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

        // The mission flow's last state (the native site 0x180484), read by the runner's pad assist.
        std::atomic<uint32_t> g_missionFlowState{0};

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

        // The ninja's gravity at any frame rate (docs/RECOMP_NOTES.md, 60 frames a second). His vertical velocity (+0x314)
        // is stepped each frame by 0x110140 (the other characters' by 0x3a25a0, the same law), v += g(v) h (g -450, or -100 - 0.4 |v| while |v| is 15 or less: the top of a
        // jump; a fifth of that gliding), and his height by the new v h after it. On the console the step h is 0.04 (25
        // frames a second): his height runs straight from frame to frame, the slope of each 0.04 s the velocity of that
        // step, which g chose at its start. Here a shadow of that steps at 0.04 s in the frames' own time, and each frame
        // stores the velocity that moves him as far as the console would in that frame's time: his height is the
        // console's, sampled more often (the top's weaker gravity and its overshoot included). A velocity the game's other
        // code wrote since (the take-off's, the landing's) starts the shadow again from it. At the console's step the
        // game's own store.
        bool gravityStep(uint8_t *rdram, R5900Context *ctx, int valueRegister, float factor, int objectRegister = 17)
        {
            constexpr float kConsoleStep = 0.04f;
            // only when the port runs the game at another rate than the console's (SHINO_60FPS, SHINO_FRAME_VBLANKS): at
            // 25 frames a second the game's own steps, slow frames' longer ones included, as on the console
            static const bool otherRate = fps60() ||
                                          (std::getenv("SHINO_FRAME_VBLANKS") && std::atoi(std::getenv("SHINO_FRAME_VBLANKS")) == 1);
            const float h = ctx->f[20];
            if (!otherRate || !(h > 0.0f) || std::fabs(h - kConsoleStep) < 1e-6f)
                return false;
            struct Shadow
            {
                uint32_t stored = 0;   // the velocity stored last (its bits)
                float v = 0.0f;        // the console's velocity, the step under way
                float into = 0.0f;     // time into that step
            };
            static std::unordered_map<uint32_t, Shadow> shadows;
            const uint32_t object = ctx->r[objectRegister].uw[0], at = (object + 0x314u) & 0x1FFFFFFu;
            uint32_t current;
            std::memcpy(&current, rdram + at, 4);
            Shadow &sh = shadows[object];
            if (sh.stored != current) // written by the game's other code (or the first time): the console's velocity now
            {
                std::memcpy(&sh.v, &current, 4);
                sh.into = 0.0f;
            }
            auto gravity = [&](float v) { return factor * (std::fabs(v) > 15.0f ? -450.0f : -100.0f + -0.4f * std::fabs(v)); };
            float left = h, moved = 0.0f;
            for (int guard = 0; left > 1e-7f && guard < 64; ++guard)
            {
                if (sh.into == 0.0f) // a console step begins: its velocity
                    sh.v = sh.v + gravity(sh.v) * kConsoleStep;
                const float dt = std::min(left, kConsoleStep - sh.into);
                moved += sh.v * dt;
                sh.into += dt;
                left -= dt;
                if (sh.into >= kConsoleStep - 1e-7f)
                    sh.into = 0.0f;
            }
            const float v = moved / h;
            ctx->f[valueRegister] = v;
            std::memcpy(rdram + at, &v, 4);
            sh.stored = ctx->fu[valueRegister];
            return true;
        }

        // The main camera: the first the scene made (gp-0x6d44, 0x10ab78), the one the player looks through; whether
        // its view was widened last (the widescreen's 4/3).
        uint32_t mainCamera(const uint8_t *rdram)
        {
            uint32_t camera;
            std::memcpy(&camera, rdram + 0x625a2cu, 4);
            return camera;
        }
        bool g_mainWidened = false;

        bool nativeInstruction(uint8_t *rdram, R5900Context *ctx, Runtime *rt, uint32_t pc)
        {
            switch (pc)
            {
            // 0x110140's gravity steps (gravityStep): the stores of the new vertical velocity
            case 0x0011032cu: // native site: swc1 f0,0x314(s1): the jump's gravity
            case 0x0011095cu: // native site: swc1 f0,0x314(s1): the jump's gravity
                return gravityStep(rdram, ctx, 0, 1.0f);
            case 0x001105b0u: // native site: swc1 f0,0x314(s1): gliding (a fifth)
                return gravityStep(rdram, ctx, 0, 0.2f);
            case 0x001104fcu: // native site: swc1 f1,0x314(s1): gliding up (a fifth), stopped at 0
                return gravityStep(rdram, ctx, 1, 0.2f);
            case 0x003a2640u: // native site: swc1 f0,0x314(s0): the other characters' gravity (0x3a25a0, its law 0x10d970)
                return gravityStep(rdram, ctx, 0, 1.0f, 16);
            // Widescreen at the game's own source (docs/NATIVE_3D.md): each camera's view 4/3 as wide, so the game
            // projects and culls for 16:9 itself. The main camera's centred (Hor+); the others' (the radar's: its needle
            // at its view's top right) also shifted an eighth of the screen right, their drawing 3/4 as wide towards the
            // right edge as the HUD's right side is (its ring). Each view's GS set-up marked with its kind.
            case 0x0010b1f0u: // native site: mul.s f0,f1,f0: a camera's (a0) horizontal tangent, its vertical x the aspect 4/3
            {
                const uint32_t camera = ctx->r[4].uw[0];
                const bool on = gs::widescreenOn(), main = camera == mainCamera(rdram);
                if (main)
                    g_mainWidened = on;
                ctx->f[0] = ctx->f[1] * ctx->f[0] * (on ? 4.0f / 3.0f : 1.0f);
                uint32_t view = 0;
                std::memcpy(&view, rdram + ((camera + 0xf4u) & 0x1FFFFFFu), 4);
                if (!main && view) // the projection's horizontal shift (view +0x78, read by 0x4377a0)
                {
                    static std::unordered_map<uint32_t, float> written; // left alone once the game sets its own
                    float shift;
                    std::memcpy(&shift, rdram + ((view + 0x78u) & 0x1FFFFFFu), 4);
                    auto mine = written.find(view);
                    if (shift == (mine == written.end() ? 0.0f : mine->second))
                    {
                        shift = on ? ctx->f[0] / 16.0f : 0.0f; // an eighth of the screen (measured: the radar's needle in its ring)
                        std::memcpy(rdram + ((view + 0x78u) & 0x1FFFFFFu), &shift, 4);
                        written[view] = shift;
                    }
                }
                if (static const bool trace = std::getenv("SHINO_TRACE_FOV") != nullptr; trace)
                    std::printf("[shino:fov] frame %llu camera %08x (main %08x) tangent %g%s\n",
                                static_cast<unsigned long long>(bozo::port::vblankCount(rt)), camera, mainCamera(rdram), ctx->f[0],
                                on ? (main ? " widened" : " widened, shifted right") : "");
                return true;
            }
            case 0x0044735cu: // native site: pcpyld a1,v0,a0: a view's (s2) DTHE (a0) and its address (v0), its kind in bits 8-15
            {
                const uint32_t view = ctx->r[18].uw[0], camera = mainCamera(rdram);
                uint32_t mainView = 0;
                if (camera)
                    std::memcpy(&mainView, rdram + ((camera + 0xf4u) & 0x1FFFFFFu), 4);
                const gs::ViewKind kind = view == mainView ? (g_mainWidened ? gs::ViewKind::MainWide : gs::ViewKind::Main)
                                                           : gs::ViewKind::Other;
                const uint64_t dthe = ctx->r[4].ud[0], address = ctx->r[2].ud[0];
                ctx->r[5].ud[0] = dthe | (uint64_t(kind) << 8);
                ctx->r[5].ud[1] = address;
                return true;
            }
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
                g_missionFlowState.store(ctx->r[5].uw[0]);
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

    uint32_t missionFlowState() { return g_missionFlowState.load(); }

    bool fps60()
    {
        static const bool on = !(std::getenv("SHINO_60FPS") && *std::getenv("SHINO_60FPS") == '0');
        return on;
    }

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
