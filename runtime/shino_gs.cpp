// Shinobido's GIF packets and VIF1 codes, read natively (runtime/shino_render.cpp hands them over in list order).
//
// GIF: the tags and their data in the three formats, one parser per path (PATH1 the VU1's XGKICK, PATH2 VIF1's
// DIRECT, PATH3 the GIF channel); the GS registers they set are kept, each XYZ2 kicks a vertex (shino_gs_state.cpp).
// VIF1 and the VU1 are runtime/shino_vu1.cpp. docs/RECOMP_NOTES.md.
#include "shino_gs.h"
#include "shino_port.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace shino
{
    namespace
    {
        // ---- GIF ----------------------------------------------------------------------------------------------
        struct Gif
        {
            // The tag being run: loops left, registers per loop, the register in the loop, its format.
            uint32_t loops = 0, nreg = 0, reg = 0, flg = 0;
            uint64_t regs = 0;
            bool inTag = false;
            uint64_t prims = 0, images = 0, packets = 0;
        } g_gif, g_gifPath1;

        uint64_t g_qwPacked = 0, g_qwImage = 0;

        void gifQuadword(Gif &g, const uint8_t *q)
        {
            uint64_t lo, hi;
            std::memcpy(&lo, q, 8);
            std::memcpy(&hi, q + 8, 8);
            if (!g.inTag || g.loops == 0)
            {
                // A tag: NLOOP 0-14, EOP 15, PRE 46, PRIM 47-57, FLG 58-59, NREG 60-63; REGS in the upper half.
                g.loops = static_cast<uint32_t>(lo & 0x7FFFu);
                g.flg = static_cast<uint32_t>(lo >> 58) & 3u;
                g.nreg = static_cast<uint32_t>(lo >> 60);
                if (g.nreg == 0)
                    g.nreg = 16;
                g.regs = hi;
                g.reg = 0;
                g.inTag = g.loops != 0;
                ++g.packets;
                if ((lo >> 46) & 1u)
                    gs::writeRegister(0x00, (lo >> 47) & 0x7FFu); // PRE: PRIM from the tag
                if (g.flg == 2) // IMAGE: NLOOP quadwords of HWREG data
                    gs::imageBegin();
                return;
            }
            switch (g.flg)
            {
            case 0: // PACKED: one register per quadword
            {
                const uint32_t r = static_cast<uint32_t>(g.regs >> (4 * g.reg)) & 0xFu;
                gs::packed(r, lo, hi);
                ++g_qwPacked;
                if (++g.reg == g.nreg)
                {
                    g.reg = 0;
                    --g.loops;
                }
                break;
            }
            case 1: // REGLIST: two 64-bit register values per quadword
                for (const uint64_t v : {lo, hi})
                {
                    if (!g.loops)
                        break;
                    const uint32_t r = static_cast<uint32_t>(g.regs >> (4 * g.reg)) & 0xFu;
                    if (r != 0xE)
                        gs::reglist(r, v);
                    if (++g.reg == g.nreg)
                    {
                        g.reg = 0;
                        --g.loops;
                    }
                }
                break;
            default: // IMAGE
                gs::imageData(q);
                ++g_qwImage;
                --g.loops;
                break;
            }
            if (g.loops == 0)
                g.inTag = false;
        }

    } // namespace

    void gs::packets(const uint8_t *data, uint32_t bytes)
    {
        prof::Scope timed(prof::Gs);
        for (uint32_t at = 0; at + 16 <= bytes;)
        {
            if (g_gif.inTag && g_gif.flg == 2 && g_gif.loops) // a run of IMAGE data at once
            {
                prof::Scope upload(prof::Upload);
                const uint32_t n = std::min(g_gif.loops, (bytes - at) / 16);
                gs::imageBytes(data + at, n * 16); // the run at once
                g_qwImage += n;
                at += n * 16;
                if ((g_gif.loops -= n) == 0)
                    g_gif.inTag = false;
                continue;
            }
            gifQuadword(g_gif, data + at);
            at += 16;
        }
    }

    void gs::path1(const uint8_t *vuMemory, uint32_t address)
    {
        // A VU1 XGKICK: GIF packets from VU1 data memory (1024 quadwords, wrapping) up to the tag with EOP.
        prof::Scope timed(prof::Gs);
        for (int packets = 0; packets < 64; ++packets)
        {
            const uint8_t *tag = vuMemory + 16 * (address & 0x3FFu);
            uint64_t lo;
            std::memcpy(&lo, tag, 8);
            const uint32_t loops = lo & 0x7FFFu, flg = (lo >> 58) & 3u, nreg = (lo >> 60) ? uint32_t(lo >> 60) : 16u;
            const uint32_t qwords = flg == 0 ? loops * nreg : flg == 1 ? (loops * nreg + 1) / 2 : loops;
            if (static const char *t = std::getenv("SHINO_TRACE_PATH1"); t && gs::flips() == static_cast<uint64_t>(std::atoll(t)))
            {
                uint64_t hi;
                std::memcpy(&hi, tag + 8, 8);
                std::printf("[shino:path1] at %03x tag %016llx regs %016llx nloop=%u flg=%u nreg=%u eop=%u\n", address & 0x3FFu,
                            (unsigned long long)lo, (unsigned long long)hi, loops, flg, nreg, unsigned((lo >> 15) & 1u));
            }
            for (uint32_t i = 0; i <= qwords; ++i)
                gifQuadword(g_gifPath1, vuMemory + 16 * ((address + i) & 0x3FFu));
            address += qwords + 1;
            if ((lo >> 15) & 1u)
                break;
        }
    }

    void gs::directQuadword(const uint8_t *quadword) { gifQuadword(g_gif, quadword); }

    void gs::report()
    {
        std::printf("[shino:gif] packets=%llu packed_qw=%llu image_qw=%llu\n", (unsigned long long)(g_gif.packets + g_gifPath1.packets),
                    (unsigned long long)g_qwPacked, (unsigned long long)g_qwImage);
        gs::reportState();
    }

} // namespace shino
