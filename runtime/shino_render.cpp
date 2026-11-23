// Shinobido's render scheduler, native.
//
// The game builds each frame as a list of 8-byte commands {word0, word1} (opcode in word0's low byte) and hands it to
// its own scheduler, 0x4429d0, which runs in the DMAC/INTC handlers: it starts VIF1 (opcode 0x01) and GIF (0x02)
// transfers from the list, both at once (0x21, MSKPATH3 first), waits for the display flip (0x40 / 0x41: the vblank
// handler 0x442fb0 flips and resumes it), decrements counters (0x42), calls functions (0x4f), jumps (0x7f). There is no
// DMAC, VIF or GIF: both handlers are answered here. The list is drained in one go; every transfer's packets go to the
// GS translator (shino_gs.cpp) as they are reached, and the flip publishes the frame. docs/RECOMP_NOTES.md.
#include "shino_port.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/libs/libraries.h"
#include "bozo/render/movie.h"
#include "bozo/render/renderer.h"
#include "bozo/render/texture.h"
#include "bozo/runtime_api.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

namespace shino
{
    namespace
    {
        constexpr uint32_t kGp = 0x0062C770u; // crt0's $gp (0x1001ac); the handlers load it from gp-0x6838
        constexpr uint32_t kListCurrent = kGp - 0x6810u, kListEnd = kGp - 0x6814u;
        constexpr uint32_t kBusy = kGp - 0x6800u, kWaitFlip = kGp - 0x6804u, kFlipBuffer = kGp - 0x6808u;
        constexpr uint32_t kDisplayEnv = kGp - 0x680cu, kFlipCount = kGp - 0x6820u;
        constexpr uint32_t kVblanks = kGp - 0x6828u, kVblanksPerFrame = kGp - 0x6850u;
        constexpr uint32_t kInterlace = kGp - 0x6818u, kFieldParity = kGp - 0x681cu;

        uint8_t *g_ram = nullptr;
        uint32_t rd32(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + (a & 0x1FFFFFFu), 4); return v; }
        uint64_t rd64(uint32_t a) { uint64_t v; std::memcpy(&v, g_ram + (a & 0x1FFFFFFu), 8); return v; }
        uint8_t rd8(uint32_t a) { return g_ram[a & 0x1FFFFFFu]; }
        void wr32(uint32_t a, uint32_t v) { std::memcpy(g_ram + (a & 0x1FFFFFFu), &v, 4); }
        void wr8(uint32_t a, uint8_t v) { g_ram[a & 0x1FFFFFFu] = v; }
        const uint8_t *ptr(uint32_t a) { return g_ram + (a & 0x1FFFFFFu); }

        bool trace() { static const bool on = std::getenv("SHINO_TRACE_LIST") != nullptr; return on; }

        // A source-chain DMA transfer (VIF1 or GIF), its data handed to `take` in order. The tags: refe 0, cnt 1,
        // next 2, ref 3, refs 4, call 5, ret 6, end 7 (two call levels). With TTE (VIF1), the tag's upper 64 bits are
        // two VIF codes sent before the data.
        template <class Take> void walkChain(uint32_t tadr, bool tte, Take &&take)
        {
            uint32_t stack[2] = {}, depth = 0;
            for (int guard = 0; guard < 100000; ++guard)
            {
                const uint64_t tag = rd64(tadr);
                const uint32_t qwc = static_cast<uint32_t>(tag & 0xFFFFu), id = static_cast<uint32_t>(tag >> 28) & 7u;
                const uint32_t addr = static_cast<uint32_t>(tag >> 32) & 0x7FFFFFF0u;
                if (tte)
                    take(ptr(tadr + 8), 8u, true);
                switch (id)
                {
                case 0: take(ptr(addr), qwc * 16u, false); return;                       // refe
                case 1: take(ptr(tadr + 16), qwc * 16u, false); tadr += 16 + qwc * 16; break; // cnt
                case 2: take(ptr(tadr + 16), qwc * 16u, false); tadr = addr; break;          // next
                case 3: case 4: take(ptr(addr), qwc * 16u, false); tadr += 16; break;        // ref, refs
                case 5: // call
                    take(ptr(tadr + 16), qwc * 16u, false);
                    if (depth >= 2) return;
                    stack[depth++] = tadr + 16 + qwc * 16;
                    tadr = addr;
                    break;
                case 6: // ret
                    take(ptr(tadr + 16), qwc * 16u, false);
                    if (!depth) return;
                    tadr = stack[--depth];
                    break;
                default: take(ptr(tadr + 16), qwc * 16u, false); return; // end
                }
            }
        }

        struct Counts { uint64_t gifQw = 0, vifWords = 0, transfers = 0, calls = 0, flips = 0; } g_counts;

        void gifData(const uint8_t *data, uint32_t bytes) { g_counts.gifQw += bytes / 16; gs::packets(data, bytes); }
        void vifData(const uint8_t *data, uint32_t bytes, bool tagWords)
        {
            g_counts.vifWords += bytes / 4;
            vif::codes(data, bytes, tagWords);
        }

        // One transfer of the list: word0 bits 8-9 chain (with 0x1000 TTE for VIF1), else normal with QWC in 12-27.
        void transfer(bool vif1, uint32_t word0, uint32_t address)
        {
            ++g_counts.transfers;
            if (word0 & 0x300u)
                walkChain(address, vif1 && (word0 & 0x1000u), [&](const uint8_t *p, uint32_t n, bool tagWords) {
                    if (vif1) vifData(p, n, tagWords); else gifData(p, n);
                });
            else
            {
                const uint32_t qwc = (word0 >> 12) & 0xFFFFu;
                if (vif1) vifData(ptr(address), qwc * 16u, false); else gifData(ptr(address), qwc * 16u);
            }
        }

        // 0x4429d0 (handler or kick, a0 = DMAC channel, INTC cause or -1): runs the list on until it ends or waits.
        void scheduler(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            g_ram = rdram;
            const int32_t why = static_cast<int32_t>(ctx->r[4].uw[0]);
            const uint32_t caller = ctx->r[31].uw[0];
            if ((why == 1 || why == 2) && rd8(kBusy))
                wr8(kBusy, static_cast<uint8_t>(rd8(kBusy) - 1)); // a completion (none are pending natively)
            while (!rd8(kBusy) && !rd8(kWaitFlip) && rd32(kListCurrent) != rd32(kListEnd))
            {
                const uint32_t at = rd32(kListCurrent), w0 = rd32(at), w1 = rd32(at + 4);
                const uint32_t op = w0 & 0xFFu;
                if (trace())
                    std::printf("[shino:list] %08x op=%02x w0=%08x w1=%08x\n", at, op, w0, w1);
                uint32_t next = at + 8;
                switch (op)
                {
                case 0x01: transfer(true, w0, w1); break;
                case 0x02: transfer(false, w0, w1); break;
                case 0x21: // GIF chain (its tag at +0xc) and VIF1 chain (+4, with TTE) together, PATH3 masked first
                    walkChain(rd32(at + 0xC), false, [](const uint8_t *p, uint32_t n, bool) { gifData(p, n); });
                    walkChain(w1, true, [](const uint8_t *p, uint32_t n, bool t) { vifData(p, n, t); }); // CHCR 0x145: TTE
                    g_counts.transfers += 2;
                    next = at + 16;
                    break;
                case 0x40: case 0x41: // wait for the vblank's flip to the display environment w1 (0x41: buffer 1)
                    wr8(kWaitFlip, 1);
                    wr32(kDisplayEnv, w1);
                    wr8(kFlipBuffer, op == 0x41 ? 1 : 0);
                    wr8(kFlipCount, static_cast<uint8_t>(rd8(kFlipCount) - 1));
                    break;
                case 0x42: // a counter's decrement: by bits 12-27, or 1
                    wr32(w1, rd32(w1) - (((w0 >> 12) & 0xFFFFu) ? ((w0 >> 12) & 0xFFFFu) : 1u));
                    break;
                case 0x4f: // a function's call: (handler context, 0 for a kick), 0 = wait for its completion
                {
                    ++g_counts.calls;
                    wr32(kListCurrent, next); // the original advances after the call; a nested kick must not rerun it
                    ctx->r[4].ud[0] = why != -1 ? 1u : 0u;
                    ctx->r[31].ud[0] = caller;
                    bozo_call_indirect(rdram, ctx, rt, w1);
                    if (ctx->leaving)
                        return;
                    wr8(kBusy, ctx->r[2].uw[0] == 0u ? 1 : 0);
                    ctx->r[2].ud[0] = static_cast<uint64_t>(-1);
                    ctx->pc = caller;
                    continue;
                }
                case 0x7f: next = w1; break; // a jump to another list
                default: break;               // 0x43 and the rest: nothing
                }
                wr32(kListCurrent, next);
            }
            ctx->r[2].ud[0] = static_cast<uint64_t>(-1);
            ctx->pc = caller;
        }

        // 0x442fb0, the vblank handler (INTC cause 2): counts vblanks up to the frame's minimum, then makes the flip
        // the list waits for: the display environment's buffer shown (DISPFB / DISPLAY, the frame published here),
        // the field's GIF chain (interlace) sent, the list resumed.
        void vblank(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            g_ram = rdram;
            const uint32_t caller = ctx->r[31].uw[0];
            ctx->r[2].ud[0] = 0;
            const uint8_t count = rd8(kVblanks), minimum = rd8(kVblanksPerFrame);
            if (count < minimum)
            {
                if (minimum < 0xFF)
                    wr8(kVblanks, static_cast<uint8_t>(count + 1));
                ctx->pc = caller;
                return;
            }
            if (ctx->r[4].uw[0] != 2u || !rd8(kWaitFlip))
            {
                ctx->pc = caller;
                return;
            }
            wr8(kWaitFlip, 0);
            wr8(kVblanks, 1);
            const uint32_t field = (bozo::port::vblankCount(rt) & 1u) ? 2u : 0u; // CSR.FIELD, bit 13 >> 12
            const uint32_t env = rd32(kDisplayEnv), buffer = rd8(kFlipBuffer) & 1u;
            if (rd8(kInterlace))
            {
                wr8(kFieldParity, field ? 0 : 1);
                const uint32_t bit = (field ^ 2u) << 2, a = buffer ? 0x1F4u : 0x84u, b = buffer ? 0x274u : 0x104u;
                wr32(env + a, (rd32(env + a) & ~8u) | bit);
                wr32(env + b, (rd32(env + b) & ~8u) | bit);
            }
            ++g_counts.flips;
            const uint32_t display = buffer ? (field ? 0x3B0u : 0x340u) : (field ? 0x3A0u : 0x330u); // DISPFB1, then DISPLAY1
            gs::flip(rt, ctx, rd64(env + display), rd64(env + display + 8));
            if (const uint32_t chain = rd32(env + 0x3C0u + 4u * (buffer | field)))
                walkChain(chain, false, [](const uint8_t *p, uint32_t n, bool) { gifData(p, n); });
            ctx->r[4].ud[0] = static_cast<uint64_t>(-1);
            ctx->r[31].ud[0] = caller;
            scheduler(rdram, ctx, rt);
            ctx->r[2].ud[0] = 0;
            ctx->pc = caller;
        }
        // ---- Movies ----------------------------------------------------------------------------------------------
        // The movie player (0x327130..0x3298b0) decodes with libmpeg (native: the pictures come to onMoviePicture) and
        // shows each picture from its own vblank handler 0x326860, which swaps the display with sceGsSwapDBuff: that
        // is the movie's frame boundary.
        std::mutex g_movieMutex;
        std::shared_ptr<const bozo::render::Texture> g_moviePicture;
        uint64_t g_movieVersion = 0, g_movieFrames = 0;

        void onMoviePicture(uint32_t, uint32_t width, uint32_t height, const uint8_t *rgba)
        {
            auto t = std::make_shared<bozo::render::Texture>();
            t->width = width;
            t->height = height;
            t->rgba.resize(static_cast<size_t>(width) * height);
            std::memcpy(t->rgba.data(), rgba, t->rgba.size() * 4);
            for (uint32_t &p : t->rgba)
                p |= 0xFF000000u; // the movie is opaque
            t->key = 0x73686d6f76696500ull; // "shmovie"
            std::lock_guard<std::mutex> lock(g_movieMutex);
            t->version = ++g_movieVersion;
            g_moviePicture = std::move(t);
        }

        // sceGsSwapDBuff (0x2c2b50), called only by the movie's vblank handler: the movie's latest picture published
        // over the whole screen and the frame ended.
        void movieSwap(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            g_ram = rdram;
            const uint32_t caller = ctx->r[31].uw[0];
            // libgraph's swap puts the buffer's display and draw environments: nothing natively (the picture is the
            // movie's, published below); it returns 0.
            ctx->r[2].ud[0] = 0;
            std::shared_ptr<const bozo::render::Texture> picture;
            {
                std::lock_guard<std::mutex> lock(g_movieMutex);
                picture = g_moviePicture;
            }
            auto &renderer = bozo::render::Renderer::instance();
            bozo::render::Screen screen;
            screen.originX = 0;
            screen.originY = 0;
            screen.width = 512;
            screen.height = 512;
            renderer.beginFrame(screen);
            if (picture)
                renderer.setBackdrop(picture, screen, 0, 0, 512, 512);
            renderer.publishOverlay(screen);
            renderer.endFrame();
            ++g_movieFrames;
            const uint64_t v = ctx->r[2].ud[0];
            ctx->pc = caller;
            bozo::port::frameEnd(rt, ctx);
            ctx->r[2].ud[0] = v;
        }
        // 0x327130(name), the movie player's play: until the movie's sound path runs (its audio pre-buffer counts the
        // bytes sent to the IOP's sound buffer through sceSdRemote block transfers: docs/RECOMP_NOTES.md), each movie
        // is answered as a finished playback. The player stops and restores the game's own renderer around a movie;
        // skipped, the renderer is never stopped.
        uint64_t g_moviesSkipped = 0;
        void playMovie(uint8_t *rdram, R5900Context *ctx, Runtime *)
        {
            ++g_moviesSkipped;
            std::printf("[shino:movie] skipped %s%c", reinterpret_cast<const char *>(rdram + (ctx->r[4].uw[0] & 0x1FFFFFFu)), 10);
            ctx->r[2].ud[0] = 1;
            ctx->pc = ctx->r[31].uw[0];
        }
    } // namespace

    namespace
    {
        // 0x231ec8 (a0 = a VIF0 DMA chain): the 3D's VU0 microcode loader. It sends the chain (TTE: its tags' VIF codes
        // too) to VIF0 and waits for it, unless it is the chain loaded last (kept at 0x523228); 1 when it sent it. The
        // chain's MPG and UNPACK codes go to the port's VU0 (runtime/shino_vu1.cpp) instead.
        void loadVu0(uint8_t *rdram, R5900Context *ctx, Runtime *rt)
        {
            g_ram = rdram;
            const uint32_t chain = ctx->r[4].uw[0];
            constexpr uint32_t kLoaded = 0x523228u;
            if (rd32(kLoaded) == chain)
                ctx->r[2].ud[0] = 0;
            else
            {
                vif0Chain(rdram, rt, chain);
                wr32(kLoaded, chain);
                ctx->r[2].ud[0] = 1;
            }
            ctx->pc = ctx->r[31].uw[0];
        }

        // The game's VCALLMS entries (each in VU0 code memory, 8 bytes a pair), run by the VU0 interpreter.
        template <uint32_t kEntry> void vu0Program(uint8_t *, R5900Context *ctx, Runtime *rt) { vu0::run(rt, ctx, kEntry * 8u); }
    }

    void vif0Chain(uint8_t *rdram, Runtime *rt, uint32_t tadr)
    {
        g_ram = rdram;
        walkChain(tadr, true, [rt](const uint8_t *p, uint32_t n, bool) { vif::codes0(rt, p, n); });
    }

    void reportRender()
    {
        std::printf("[shino:render] movie_frames=%llu flips=%llu transfers=%llu gif_qw=%llu vif_words=%llu calls=%llu\n",
                    (unsigned long long)g_movieFrames, (unsigned long long)g_counts.flips, (unsigned long long)g_counts.transfers,
                    (unsigned long long)g_counts.gifQw, (unsigned long long)g_counts.vifWords,
                    (unsigned long long)g_counts.calls);
        gs::report();
        vif::report();
    }

    void installRender(bozo::port::Registry &r)
    {
        // The port's frame is the whole picture: the window shows only what the port publishes (TTA's set-up).
        auto &renderer = bozo::render::Renderer::instance();
        renderer.setEnabled(true);
        renderer.setPresentsAlone(true);
        bozo::render::setMoviePictureHook(onMoviePicture);
        r.registerFunction(0x2c2b50, movieSwap);
        if (std::getenv("SHINO_SKIP_MOVIES")) // bring-up switch: each movie answered as a finished playback
            r.registerFunction(0x327130, playMovie);
        r.registerFunction(0x4429d0, scheduler);
        r.registerFunction(0x442fb0, vblank);
        r.registerFunction(0x231ec8, loadVu0);
        // The EE reads VU0's code memory too: 0x1ebb30's search indexes the VU0 data arrays with the lane its last
        // round chose, -1 when none, which reads just below them (0x11003fec), in the code memory's mirror.
        r.vu0CodeMemory(vu0::codeMemory());
        // Every VCALLMS in the executable (no VCALLMSR): tools/shino_vcallms.py lists them.
        r.vu0Program(0x000 * 8, vu0Program<0x000>);
        r.vu0Program(0x002 * 8, vu0Program<0x002>);
        r.vu0Program(0x009 * 8, vu0Program<0x009>);
        r.vu0Program(0x010 * 8, vu0Program<0x010>);
        r.vu0Program(0x017 * 8, vu0Program<0x017>);
        r.vu0Program(0x01e * 8, vu0Program<0x01e>);
        r.vu0Program(0x025 * 8, vu0Program<0x025>);
        r.vu0Program(0x027 * 8, vu0Program<0x027>);
        r.vu0Program(0x033 * 8, vu0Program<0x033>);
        r.vu0Program(0x03a * 8, vu0Program<0x03a>);
        r.vu0Program(0x07e * 8, vu0Program<0x07e>);
        r.vu0Program(0x096 * 8, vu0Program<0x096>);
        r.vu0Program(0x09d * 8, vu0Program<0x09d>);
        r.vu0Program(0x0a4 * 8, vu0Program<0x0a4>);
        r.vu0Program(0x181 * 8, vu0Program<0x181>);
        r.vu0Program(0x18b * 8, vu0Program<0x18b>);
        r.vu0Program(0x1ba * 8, vu0Program<0x1ba>);
        r.vu0Program(0x1be * 8, vu0Program<0x1be>);
        r.vu0Program(0x1cd * 8, vu0Program<0x1cd>);
    }
} // namespace shino
