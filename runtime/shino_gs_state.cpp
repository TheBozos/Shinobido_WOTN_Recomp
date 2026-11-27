// Shinobido's GS translator: the registers the game's GIF packets set, its primitives drawn natively, its image
// transfers kept for the textures.
//
// Bring-up path (docs/RECOMP_NOTES.md, "Drawing"): the 2D the game sends as GIF packets (movies' frames, menus, text,
// fades) is drawn from the packets themselves. The textures are decoded from a copy of the GS local memory the image
// transfers fill (the GS's block and column arrangement for each format), keyed by TEX0. Only the draws into the
// frame buffer being displayed are shown; draws into other buffers (render-to-texture) are counted. The 3D, drawn by
// the VU1 programs, is not here.
#include "shino_gs.h"
#include "shino_port.h"
#include "bozo/render/renderer.h"
#include "bozo/hash.h"
#include "bozo/render/texture.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace shino::gs
{
    namespace
    {
        using bozo::render::Material;
        using bozo::render::Texture;
        using bozo::render::Vertex;

        // ---- The GS local memory (4 MB) and its arrangements -----------------------------------------------------
        std::vector<uint32_t> g_mem(1u << 20);
        std::array<uint32_t, 512> g_pageGeneration{}; // per 8 KB page, bumped by every write into it
        uint32_t g_generation = 1;

        constexpr uint8_t kBlock32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21}, {2, 3, 6, 7, 18, 19, 22, 23},
                                            {8, 9, 12, 13, 24, 25, 28, 29}, {10, 11, 14, 15, 26, 27, 30, 31}};
        constexpr uint8_t kBlock16[8][4] = {{0, 2, 8, 10},    {1, 3, 9, 11},    {4, 6, 12, 14},   {5, 7, 13, 15},
                                            {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};
        constexpr uint8_t kBlock16S[8][4] = {{0, 2, 16, 18},  {1, 3, 17, 19},   {8, 10, 24, 26},  {9, 11, 25, 27},
                                             {4, 6, 20, 22},  {5, 7, 21, 23},   {12, 14, 28, 30}, {13, 15, 29, 31}};
        constexpr uint8_t kColumn32[8][8] = {{0, 1, 4, 5, 8, 9, 12, 13},         {2, 3, 6, 7, 10, 11, 14, 15},
                                             {16, 17, 20, 21, 24, 25, 28, 29},   {18, 19, 22, 23, 26, 27, 30, 31},
                                             {32, 33, 36, 37, 40, 41, 44, 45},   {34, 35, 38, 39, 42, 43, 46, 47},
                                             {48, 49, 52, 53, 56, 57, 60, 61},   {50, 51, 54, 55, 58, 59, 62, 63}};
        constexpr uint8_t kColumn16[8][16] = {
            {0, 2, 8, 10, 16, 18, 24, 26, 1, 3, 9, 11, 17, 19, 25, 27},
            {4, 6, 12, 14, 20, 22, 28, 30, 5, 7, 13, 15, 21, 23, 29, 31},
            {32, 34, 40, 42, 48, 50, 56, 58, 33, 35, 41, 43, 49, 51, 57, 59},
            {36, 38, 44, 46, 52, 54, 60, 62, 37, 39, 45, 47, 53, 55, 61, 63},
            {64, 66, 72, 74, 80, 82, 88, 90, 65, 67, 73, 75, 81, 83, 89, 91},
            {68, 70, 76, 78, 84, 86, 92, 94, 69, 71, 77, 79, 85, 87, 93, 95},
            {96, 98, 104, 106, 112, 114, 120, 122, 97, 99, 105, 107, 113, 115, 121, 123},
            {100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127}};
        constexpr uint8_t kColumn8[16][16] = {
            {0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54},
            {8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62},
            {33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23},
            {41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31},
            {96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86},
            {104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94},
            {65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119},
            {73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127},
            {128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182},
            {136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190},
            {161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151},
            {169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159},
            {224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214},
            {232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222},
            {193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247},
            {201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255}};
        constexpr uint16_t kColumn4[16][32] = {
            {0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110},
            {16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126},
            {65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47},
            {81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63},
            {192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174},
            {208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190},
            {129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239},
            {145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255},
            {256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366},
            {272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382},
            {321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303},
            {337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319},
            {448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430},
            {464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446},
            {385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495},
            {401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511}};

        // Every table a permutation (a mistyped entry would scramble textures silently).
        template <class T, size_t R, size_t C> void checkPermutation(const T (&t)[R][C], const char *name)
        {
            std::vector<int> seen(R * C, 0);
            for (size_t r = 0; r < R; ++r)
                for (size_t c = 0; c < C; ++c)
                {
                    if (t[r][c] >= R * C || seen[t[r][c]]++)
                        throw std::runtime_error(std::string("GS table not a permutation: ") + name);
                }
        }
        struct TableCheck
        {
            TableCheck()
            {
                checkPermutation(kBlock32, "block32");
                checkPermutation(kBlock16, "block16");
                checkPermutation(kBlock16S, "block16S");
                checkPermutation(kColumn32, "column32");
                checkPermutation(kColumn16, "column16");
                checkPermutation(kColumn8, "column8");
                checkPermutation(kColumn4, "column4");
            }
        } g_tableCheck;

        // Addresses: CT32 words, CT16 halfwords, T8 bytes, T4 nibbles; bp in 64-word blocks, bw in 64-pixel units.
        inline uint32_t addr32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
        {
            const uint32_t block = bp + ((y >> 5) * bw << 5) + ((x >> 6) << 5) + kBlock32[(y >> 3) & 3][(x >> 3) & 7];
            return ((block << 6) + kColumn32[y & 7][x & 7]) & 0xFFFFFu;
        }
        inline uint32_t addr16(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, bool s)
        {
            const uint32_t b = s ? kBlock16S[(y >> 3) & 7][(x >> 4) & 3] : kBlock16[(y >> 3) & 7][(x >> 4) & 3];
            const uint32_t block = bp + ((y >> 6) * bw << 5) + ((x >> 6) << 5) + b;
            return ((block << 7) + kColumn16[y & 7][x & 15]) & 0x1FFFFFu;
        }
        inline uint32_t addr8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
        {
            const uint32_t block = bp + ((y >> 6) * ((bw + 1) >> 1) << 5) + ((x >> 7) << 5) + kBlock32[(y >> 4) & 3][(x >> 4) & 7];
            return ((block << 8) + kColumn8[y & 15][x & 15]) & 0x3FFFFFu;
        }
        inline uint32_t addr4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
        {
            const uint32_t block = bp + ((y >> 7) * ((bw + 1) >> 1) << 5) + ((x >> 7) << 5) + kBlock16[(y >> 4) & 7][(x >> 5) & 3];
            return ((block << 9) + kColumn4[y & 15][x & 31]) & 0x7FFFFFu;
        }

        uint8_t *memBytes() { return reinterpret_cast<uint8_t *>(g_mem.data()); }
        uint64_t g_changes = 0; // values changed in the local memory, all told (texture()'s quick check)
        void touch(uint32_t wordAddress)
        {
            g_pageGeneration[(wordAddress >> 11) & 511] = g_generation;
            ++g_changes;
        }

        // A pixel of format psm written / read (the value in the format's own bits). A page is marked changed only when
        // a value in it does change: the game uploads its textures again each frame, and the same bytes keep their
        // decoded textures (texture()).
        void writePixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y, uint32_t v)
        {
            auto put32 = [](uint32_t a, uint32_t value) {
                if (g_mem[a] != value)
                {
                    g_mem[a] = value;
                    touch(a);
                }
            };
            switch (psm)
            {
            case 0x00: case 0x30: put32(addr32(bp, bw, x, y), v); break;
            case 0x01: case 0x31: { const uint32_t a = addr32(bp, bw, x, y); put32(a, (g_mem[a] & 0xFF000000u) | (v & 0xFFFFFFu)); break; }
            case 0x02: case 0x0A: case 0x32: case 0x3A:
            {
                const uint32_t a = addr16(bp, bw, x, y, psm == 0x0A || psm == 0x3A);
                uint16_t &h = reinterpret_cast<uint16_t *>(g_mem.data())[a];
                if (h != static_cast<uint16_t>(v))
                {
                    h = static_cast<uint16_t>(v);
                    touch(a >> 1);
                }
                break;
            }
            case 0x13:
            {
                const uint32_t a = addr8(bp, bw, x, y);
                if (memBytes()[a] != static_cast<uint8_t>(v))
                {
                    memBytes()[a] = static_cast<uint8_t>(v);
                    touch(a >> 2);
                }
                break;
            }
            case 0x14:
            {
                const uint32_t a = addr4(bp, bw, x, y);
                uint8_t &b = memBytes()[a >> 1];
                const uint8_t n = (a & 1) ? static_cast<uint8_t>((b & 0x0F) | (v << 4)) : static_cast<uint8_t>((b & 0xF0) | (v & 0x0F));
                if (b != n)
                {
                    b = n;
                    touch(a >> 3);
                }
                break;
            }
            case 0x1B: { const uint32_t a = addr32(bp, bw, x, y); put32(a, (g_mem[a] & 0x00FFFFFFu) | (v << 24)); break; }
            case 0x24: { const uint32_t a = addr32(bp, bw, x, y); put32(a, (g_mem[a] & ~0x0F000000u) | ((v & 15u) << 24)); break; }
            case 0x2C: { const uint32_t a = addr32(bp, bw, x, y); put32(a, (g_mem[a] & ~0xF0000000u) | ((v & 15u) << 28)); break; }
            default: break;
            }
        }
        uint32_t readPixel(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
        {
            switch (psm)
            {
            case 0x00: case 0x01: case 0x30: case 0x31: return g_mem[addr32(bp, bw, x, y)];
            case 0x02: case 0x0A: case 0x32: case 0x3A:
                return reinterpret_cast<const uint16_t *>(g_mem.data())[addr16(bp, bw, x, y, psm == 0x0A || psm == 0x3A)];
            case 0x13: return memBytes()[addr8(bp, bw, x, y)];
            case 0x14: { const uint32_t a = addr4(bp, bw, x, y); return (memBytes()[a >> 1] >> ((a & 1) * 4)) & 15u; }
            case 0x1B: return g_mem[addr32(bp, bw, x, y)] >> 24;
            case 0x24: return (g_mem[addr32(bp, bw, x, y)] >> 24) & 15u;
            case 0x2C: return g_mem[addr32(bp, bw, x, y)] >> 28;
            default: return 0;
            }
        }

        // ---- Whole blocks of an upload (CT32 8 x 8, CT16 16 x 8 pixels: 256 bytes), with SSE2 ---------------------
        // The column arrangement in a block (kColumn32 / kColumn16): a CT32 row's pixel pairs are 8-byte runs, two
        // rows interleaved by 8 bytes; a CT16 row's pixels p and p + 8 alternate, two rows interleaved the same way.
        // So a block is a few unpacks of its rows. `rows` points at the block's top-left pixel, `stride` the upload's
        // row in bytes. Each 16 bytes compared with what is there before it is stored: whether any changed.
        inline bool storeChanged(uint8_t *to, __m128i value, bool changed)
        {
            const __m128i old = _mm_loadu_si128(reinterpret_cast<const __m128i *>(to));
            const bool differs = _mm_movemask_epi8(_mm_cmpeq_epi8(old, value)) != 0xFFFF;
            _mm_storeu_si128(reinterpret_cast<__m128i *>(to), value);
            return changed || differs;
        }
        bool writeBlock32(uint8_t *block, const uint8_t *rows, uint32_t stride)
        {
            bool changed = false;
            for (uint32_t r = 0; r < 8; r += 2)
            {
                const uint8_t *a = rows + r * stride, *b = a + stride;
                const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a)); // pixels 0-3
                const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a + 16)); // 4-7
                const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b));
                const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + 16));
                uint8_t *to = block + r * 32; // words 16 * (r / 2): rows r, r + 1
                changed = storeChanged(to, _mm_unpacklo_epi64(a0, b0), changed);       // words 0-3: p0 p1 of both
                changed = storeChanged(to + 16, _mm_unpackhi_epi64(a0, b0), changed);  // 4-7: p2 p3
                changed = storeChanged(to + 32, _mm_unpacklo_epi64(a1, b1), changed);  // 8-11: p4 p5
                changed = storeChanged(to + 48, _mm_unpackhi_epi64(a1, b1), changed);  // 12-15: p6 p7
            }
            return changed;
        }
        bool writeBlock16(uint8_t *block, const uint8_t *rows, uint32_t stride)
        {
            bool changed = false;
            for (uint32_t r = 0; r < 8; r += 2)
            {
                const uint8_t *a = rows + r * stride, *b = a + stride;
                const __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a)); // pixels 0-7
                const __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(a + 16)); // 8-15
                const __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b));
                const __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + 16));
                const __m128i alo = _mm_unpacklo_epi16(a0, a1), ahi = _mm_unpackhi_epi16(a0, a1); // p0 p8 p1 p9 ...
                const __m128i blo = _mm_unpacklo_epi16(b0, b1), bhi = _mm_unpackhi_epi16(b0, b1);
                uint8_t *to = block + r * 32; // halfwords 32 * (r / 2)
                changed = storeChanged(to, _mm_unpacklo_epi64(alo, blo), changed);
                changed = storeChanged(to + 16, _mm_unpackhi_epi64(alo, blo), changed);
                changed = storeChanged(to + 32, _mm_unpacklo_epi64(ahi, bhi), changed);
                changed = storeChanged(to + 48, _mm_unpackhi_epi64(ahi, bhi), changed);
            }
            return changed;
        }
        // The two against the tables, at start-up (a wrong unpack would scramble every texture silently).
        struct BlockWriterCheck
        {
            BlockWriterCheck()
            {
                alignas(16) uint8_t got[256], want[256];
                uint8_t src[16 * 64];
                for (uint32_t i = 0; i < sizeof(src); ++i)
                    src[i] = static_cast<uint8_t>(i * 7 + (i >> 8) * 13);
                std::memset(got, 0, sizeof(got));
                writeBlock32(got, src, 64);
                for (uint32_t r = 0; r < 8; ++r)
                    for (uint32_t c = 0; c < 8; ++c)
                        std::memcpy(want + 4 * kColumn32[r][c], src + r * 64 + 4 * c, 4);
                if (std::memcmp(got, want, 256) != 0)
                    throw std::runtime_error("GS upload: CT32 block writer does not match the column table");
                std::memset(got, 0, sizeof(got));
                writeBlock16(got, src, 64);
                for (uint32_t r = 0; r < 8; ++r)
                    for (uint32_t c = 0; c < 16; ++c)
                        std::memcpy(want + 2 * kColumn16[r][c], src + r * 64 + 2 * c, 2);
                if (std::memcmp(got, want, 256) != 0)
                    throw std::runtime_error("GS upload: CT16 block writer does not match the column table");
            }
        } g_blockWriterCheck;

        // ---- The registers ------------------------------------------------------------------------------------
        struct Context
        {
            uint64_t tex0 = 0, tex1 = 0, clamp = 0, xyoffset = 0, scissor = 0, alpha = 0, test = 0, frame = 0, zbuf = 0;
        };
        struct State
        {
            uint64_t prim = 0, prmode = 0, prmodecont = 1, rgbaq = 0, texa = 0, fogcol = 0, texclut = 0;
            uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
            float s = 0, t = 0, q = 1;
            uint32_t u = 0, v = 0, fog = 0xFF;
            Context ctx[2];
        } g_s;

        struct GsVertex
        {
            float x, y, u, v, q;
            uint32_t z, rgba, fog;
        };
        // The primitive's vertices so far (at most three kept: a strip's last two, a fan's first and last).
        struct VertexQueue
        {
            GsVertex v[4];
            uint32_t n = 0;
            size_t size() const { return n; }
            void clear() { n = 0; }
            GsVertex *data() { return v; }
            void push_back(const GsVertex &x) { v[n++] = x; }
            void eraseAt(uint32_t i) // spelled out (GCC made the loop a memmove call)
            {
                if (i == 0 && n > 1)
                    v[0] = v[1];
                if (i <= 1 && n > 2)
                    v[1] = v[2];
                if (n > 3)
                    v[2] = v[3];
                --n;
            }
        } g_queue;

        // ---- Textures -----------------------------------------------------------------------------------------
        struct Cached
        {
            std::shared_ptr<Texture> texture;
            uint32_t generation = 0;
        };
        std::unordered_map<uint64_t, Cached> g_textures;
        std::unordered_map<uint64_t, std::shared_ptr<Texture>> g_byContent; // by the hash of the bytes decoded
        uint64_t g_decodes = 0;
        uint64_t g_lookups = 0, g_hashes = 0, g_hashedBytes = 0; // SHINO_PROFILE: lookUpTexture's work

        uint32_t expand16(uint32_t c, uint64_t texa)
        {
            const uint32_t r = (c & 31u) << 3, g = ((c >> 5) & 31u) << 3, b = ((c >> 10) & 31u) << 3;
            const bool aemZero = ((texa >> 15) & 1u) && (c & 0x7FFFu) == 0;
            const uint32_t a = aemZero ? 0u : ((c & 0x8000u) ? (texa >> 32) & 0xFFu : texa & 0xFFu);
            return r | g << 8 | b << 16 | a << 24;
        }
        uint32_t toRgba8(uint32_t c) // GS alpha 0x80 = opaque -> 255
        {
            const uint32_t a = c >> 24;
            return (c & 0xFFFFFFu) | (std::min(255u, a * 255u / 128u) << 24);
        }

        // The texture TEX0 names, decoded from the local memory (cached until a page it spans is written).
        std::shared_ptr<Texture> lookUpTexture(uint64_t tex0);
        // The newest page generation over where the texture TEX0 names may read: its rows at its buffer width, its
        // palette's page.
        uint32_t textureGeneration(uint64_t tex0)
        {
            const uint32_t tbp = tex0 & 0x3FFFu, tbw = (tex0 >> 14) & 0x3Fu, psm = (tex0 >> 20) & 0x3Fu;
            const uint32_t tw = std::min<uint32_t>(1u << ((tex0 >> 26) & 0xFu), 1024u);
            const uint32_t th = std::min<uint32_t>(1u << ((tex0 >> 30) & 0xFu), 1024u);
            const uint32_t cbp = (tex0 >> 37) & 0x3FFFu;
            const bool indexed = psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C;
            uint32_t generation = 0;
            const uint32_t bpp = psm == 0x14 || psm == 0x24 || psm == 0x2C ? 4 : psm == 0x13 || psm == 0x1B ? 8 : (psm == 0x02 || psm == 0x0A) ? 16 : 32;
            const uint32_t width = std::max(tw, std::max(tbw, 1u) * 64u);
            const uint32_t bytes = width * th * bpp / 8 + 8192;
            for (uint32_t p = tbp / 32; p <= (tbp * 256 + bytes) / 8192 && p < 512; ++p)
                generation = std::max(generation, g_pageGeneration[p]);
            if (indexed)
                generation = std::max(generation, g_pageGeneration[(cbp / 32) & 511]);
            return generation;
        }

        const std::shared_ptr<Texture> &texture(uint64_t tex0)
        {
            // The textures used lately (a 3D frame alternates between a few): one is still good while nothing was
            // written since, or nothing in its pages (the generation over them the same: the uploads between draws go
            // elsewhere, mostly).
            struct Recent
            {
                uint64_t tex0 = ~0ull, texa = ~0ull, changes = ~0ull;
                uint32_t generation = 0;
                std::shared_ptr<Texture> texture;
            };
            static std::array<Recent, 64> recent;
            Recent &r = recent[(tex0 ^ (tex0 >> 14) ^ (tex0 >> 37) ^ g_s.texa) & 63u];
            if (r.texture && r.tex0 == tex0 && r.texa == g_s.texa)
            {
                if (r.changes == g_changes)
                    return r.texture;
                if (textureGeneration(tex0) == r.generation)
                {
                    r.changes = g_changes;
                    return r.texture;
                }
            }
            r.texture = lookUpTexture(tex0);
            r.tex0 = tex0;
            r.texa = g_s.texa;
            r.changes = g_changes;
            r.generation = textureGeneration(tex0);
            return r.texture;
        }
        std::shared_ptr<Texture> lookUpTexture(uint64_t tex0)
        {
            ++g_lookups;
            const uint32_t tbp = tex0 & 0x3FFFu, tbw = (tex0 >> 14) & 0x3Fu, psm = (tex0 >> 20) & 0x3Fu;
            const uint32_t tw = std::min<uint32_t>(1u << ((tex0 >> 26) & 0xFu), 1024u);
            const uint32_t th = std::min<uint32_t>(1u << ((tex0 >> 30) & 0xFu), 1024u);
            const uint32_t cbp = (tex0 >> 37) & 0x3FFFu, cpsm = (tex0 >> 51) & 0xFu, csa = (tex0 >> 56) & 0x1Fu;
            const bool indexed = psm == 0x13 || psm == 0x14 || psm == 0x1B || psm == 0x24 || psm == 0x2C;
            const uint32_t generation = textureGeneration(tex0);
            const uint32_t bpp = psm == 0x14 || psm == 0x24 || psm == 0x2C ? 4 : psm == 0x13 || psm == 0x1B ? 8 : (psm == 0x02 || psm == 0x0A) ? 16 : 32;
            const uint64_t key = (tex0 & 0x7FFFFFFFFFull) | (indexed ? (tex0 & 0x1FFFFFE000000000ull) : 0) | uint64_t(g_s.texa & 1) << 63;
            Cached &c = g_textures[key ^ (g_s.texa >> 32) * 0x9E3779B97F4A7C15ull];
            if (c.texture && c.generation == generation)
                return c.texture;
            prof::Scope timed(prof::Decode);
            // The palette's colours (the 16 or 256 the texture uses).
            std::array<uint32_t, 256> palette{};
            const uint32_t entries = indexed ? (bpp == 4 ? 16u : 256u) : 0u;
            if (indexed)
            {
                const uint32_t offset = bpp == 4 ? csa * 16 : 0;
                for (uint32_t i = 0; i < entries; ++i)
                {
                    uint32_t x, y;
                    bozo::render::csm1Position((i + offset) & 0xFFu, x, y);
                    const uint32_t raw = cpsm == 0x00 || cpsm == 0x01 ? readPixel(0x00, cbp, 1, x, y)
                                                                       : expand16(readPixel(cpsm == 0x0A ? 0x0A : 0x02, cbp, 1, x, y), g_s.texa);
                    palette[i] = cpsm == 0x01 ? (raw & 0xFFFFFFu) | 0x80000000u : raw;
                }
            }
            // The same picture decoded before (a texture the game streams through a shared area, swapped back in): the
            // bytes it reads and its palette's colours, hashed.
            // Exactly the GS blocks (256 bytes each) the texture's pixels are in: a texture streamed in next to it, in the
            // same pages, leaves its hash (and its decoded copy) alone.
            // Seeded with what the decoded picture depends on besides the bytes: the format, the size, the palette's
            // format and offset, TEXA; not where the texture is (the game streams the same texture into different slots).
            ++g_hashes;
            const uint64_t shape = uint64_t(psm) | uint64_t(tw) << 8 | uint64_t(th) << 20 |
                                   (indexed ? uint64_t(cpsm) << 32 | uint64_t(csa) << 36 | uint64_t((tex0 >> 55) & 1u) << 41 : 0);
            uint64_t content = shape * 0xC2B2AE3D27D4EB4Full ^ (g_s.texa * 0x9E3779B97F4A7C15ull);
            {
                const bool t32 = psm == 0x00 || psm == 0x01 || psm == 0x1B || psm == 0x24 || psm == 0x2C || psm == 0x30 || psm == 0x31;
                const bool t16 = psm == 0x02 || psm == 0x0A || psm == 0x32 || psm == 0x3A;
                const uint32_t bw = t32 ? 8 : t16 ? 16 : psm == 0x13 ? 16 : 32, bh = t32 || t16 ? 8 : 16;
                const uint32_t buffer = std::max(tbw, 1u);
                for (uint32_t by = 0; by < th; by += bh)
                    for (uint32_t bx = 0; bx < tw; bx += bw)
                    {
                        const uint32_t at = t32 ? addr32(tbp, buffer, bx, by) * 4u
                                            : t16 ? addr16(tbp, buffer, bx, by, psm == 0x0A || psm == 0x3A) * 2u
                                            : psm == 0x13 ? addr8(tbp, buffer, bx, by)
                                                          : addr4(tbp, buffer, bx, by) / 2u;
                        content = bozo::hash::xxh64(memBytes() + (at & 0x3FFF00u), 256, content);
                        g_hashedBytes += 256;
                    }
            }
            if (indexed) // the colours used, not the palette area around them (other palettes change there)
                content = bozo::hash::xxh64(palette.data(), entries * 4u, content);
            if (auto known = g_byContent.find(content); known != g_byContent.end())
            {
                c.texture = known->second;
                c.generation = generation;
                return c.texture;
            }
            auto t = std::make_shared<Texture>();
            t->width = tw;
            t->height = th;
            t->key = key;
            t->version = ++g_decodes;
            t->rgba.resize(size_t(tw) * th);
            // The texels, one loop per format (readPixel's switch out of the loop).
            const uint32_t buffer = std::max(tbw, 1u);
            const uint64_t texa = g_s.texa;
            auto decode = [&](auto read) {
                uint32_t *out = t->rgba.data();
                for (uint32_t y = 0; y < th; ++y)
                    for (uint32_t x = 0; x < tw; ++x)
                    {
                        uint32_t c32;
                        const uint32_t p = read(x, y);
                        if (indexed)
                            c32 = palette[p & 0xFFu];
                        else if (psm == 0x00)
                            c32 = p;
                        else if (psm == 0x01)
                            c32 = (p & 0xFFFFFFu) | ((((texa >> 15) & 1u) && (p & 0xFFFFFFu) == 0) ? 0u : (uint32_t(texa & 0xFFu) << 24));
                        else
                            c32 = expand16(p, texa);
                        *out++ = toRgba8(c32);
                    }
            };
            const uint16_t *mem16 = reinterpret_cast<const uint16_t *>(g_mem.data());
            const uint8_t *mem8 = memBytes();
            switch (psm)
            {
            case 0x00: case 0x01: case 0x30: case 0x31: decode([&](uint32_t x, uint32_t y) { return g_mem[addr32(tbp, buffer, x, y)]; }); break;
            case 0x02: case 0x32: decode([&](uint32_t x, uint32_t y) { return uint32_t(mem16[addr16(tbp, buffer, x, y, false)]); }); break;
            case 0x0A: case 0x3A: decode([&](uint32_t x, uint32_t y) { return uint32_t(mem16[addr16(tbp, buffer, x, y, true)]); }); break;
            case 0x13: decode([&](uint32_t x, uint32_t y) { return uint32_t(mem8[addr8(tbp, buffer, x, y)]); }); break;
            case 0x14: decode([&](uint32_t x, uint32_t y) { const uint32_t a = addr4(tbp, buffer, x, y); return (uint32_t(mem8[a >> 1]) >> ((a & 1) * 4)) & 15u; }); break;
            default: decode([&](uint32_t x, uint32_t y) { return readPixel(psm, tbp, buffer, x, y); }); break;
            }
            c.texture = t;
            c.generation = generation;
            if (g_byContent.size() >= 4096)
                g_byContent.clear();
            g_byContent.emplace(content, t);
            // SHINO_DUMP_TEXTURES=<flip>: each texture decoded from that flip on, raw RGBA as build/tex/<tex0>_<w>x<h>.rgba
            if (static const char *d = std::getenv("SHINO_DUMP_TEXTURES"); d && gs::flips() >= static_cast<uint64_t>(std::atoll(d)))
            {
                char name[96];
                std::snprintf(name, sizeof(name), "build/tex/%016llx_%ux%u.rgba", (unsigned long long)tex0, tw, th);
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fwrite(t->rgba.data(), 4, t->rgba.size(), f);
                    std::fclose(f);
                }
            }
            return t;
        }

        // ---- Image transfers (host to local) -------------------------------------------------------------------
        struct Image
        {
            uint32_t psm = 0, bp = 0, bw = 1, x0 = 0, y0 = 0, w = 0, h = 0, x = 0, y = 0;
            bool active = false;
            uint32_t carry = 0, carryBits = 0; // CT24's three-byte pixels across quadwords
            std::vector<uint8_t> data;         // the transfer's quadwords, written when complete (imageData)
            uint32_t need = 0;                 // its bytes
        } g_image;

        std::array<uint64_t, 64> g_uploadBytes{}, g_uploadCount{}; // by PSM (the report)
        uint64_t g_uploadsByBlock = 0, g_uploadsByRow = 0, g_uploadsByPixel = 0;
        uint64_t g_blocksCompared = 0, g_blocksChanged = 0; // SHINO_PROFILE: the block path's work

        void imagePixel(uint32_t value)
        {
            Image &im = g_image;
            if (im.y >= im.h)
                return;
            writePixel(im.psm, im.bp, im.bw, im.x0 + im.x, im.y0 + im.y, value);
            if (++im.x == im.w)
            {
                im.x = 0;
                ++im.y;
            }
        }

        // ---- The frame's draws, by the frame buffer they go into ----------------------------------------------------
        struct Batch
        {
            Material material;
            std::shared_ptr<const Texture> texture;
            uint32_t first = 0, count = 0; // its vertices in the renderer's frame being built (appendVertices)
            uint64_t made = 0; // the material's serial (emit): batches with the same one and texture merge
            // A native draw's batch (nativeTriangles): object-space vertices and their matrix to window pixels; the
            // others are window pixels already (an identity matrix).
            bool native = false;
            std::array<float, 16> matrix{};
        };
        std::unordered_map<uint32_t, std::vector<Batch>> g_frameDraws; // FRAME.FBP -> draws in order
        // The frame's vertices are made straight into the renderer's frame being built (Renderer::appendVertices): a
        // batch is a range of them, drawn at the flip (drawRange) without a copy; a two-pass draw's batches share one.
        bozo::render::Renderer &g_renderer = bozo::render::Renderer::instance();
        std::map<std::string, uint64_t> g_survey;                      // SHINO_SURVEY
        uint32_t g_displayFbp = 0;                                     // the frame buffer shown last
        uint64_t g_prims = 0, g_offscreenPrims = 0, g_flips = 0, g_uploads = 0, g_checksum = 0xCBF29CE484222325ull;
        uint64_t g_nativeChecksum = 0xCBF29CE484222325ull; // SHINO_CHECKSUM: the native batches (gs::nativeTriangles)

        // The frame buffers the game displays (DISPFB's FBP, flip), as TEX0's block pointers: a texture there is a frame.
        std::array<uint32_t, 4> g_frameBuffers{~0u, ~0u, ~0u, ~0u};
        // A texture that is the other displayed frame buffer: the previous frame (the game blends it over the new one,
        // an afterimage, every frame of play). The renderer samples its copy of the frame presented last.
        bool samplesPreviousFrame(const Context &c, uint64_t attrs)
        {
            if (!((attrs >> 4) & 1u))
                return false;
            const uint32_t tbp = c.tex0 & 0x3FFFu, psm = (c.tex0 >> 20) & 0x3Fu, target = (c.frame & 0x1FFu) * 32u;
            if ((psm != 0x00 && psm != 0x01) || tbp == target)
                return false;
            for (const uint32_t b : g_frameBuffers)
                if (b == tbp)
                    return true;
            return false;
        }

        Material material(const Context &c, uint64_t attrs)
        {
            Material m;
            m.textured = (attrs >> 4) & 1u;
            if (samplesPreviousFrame(c, attrs))
            {
                m.fromFrame = 1;
                m.source = Material::kPreviousFrame;
            }
            // FRAME's FBMSK: a channel all of whose 8 bits are masked is not written (the game's depth-only draws mask
            // all 32: textured, about 16 a frame in play).
            const uint32_t fbmsk = static_cast<uint32_t>(c.frame >> 32);
            m.colourMask = 0;
            for (uint32_t channel = 0; channel < 4; ++channel)
                if (((fbmsk >> (8 * channel)) & 0xFFu) != 0xFFu)
                    m.colourMask |= static_cast<uint8_t>(1u << channel);
            // TEST's ZTE / ZTST (GEQUAL, GREATER; ALWAYS no test; NEVER is not drawn, emit), ZBUF's ZMSK.
            const uint32_t ztst = ((c.test >> 16) & 1u) ? uint32_t(c.test >> 17) & 3u : 1u;
            m.depthTest = ztst >= 2;
            m.depthGreater = ztst == 3;
            m.depthWrite = !((c.zbuf >> 32) & 1u);
            m.tex0 = c.tex0;
            m.alphaBlend = (attrs >> 6) & 1u;
            m.blend = m.alphaBlend ? bozo::render::blendOf(c.alpha) : Material::Blend::Opaque; // PRIM.ABE off: no blending
            const uint64_t test = c.test;
            m.alphaTest = test & 1u;
            if (m.alphaTest)
            {
                m.alphaRef = static_cast<uint8_t>((test >> 4) & 0xFFu);
                using F = Material::AlphaFunc;
                static const F funcs[8] = {F::Never, F::Always, F::Less, F::LessEqual, F::Equal, F::GreaterEqual, F::Greater, F::NotEqual};
                m.alphaFunc = funcs[(test >> 1) & 7u];
            }
            using TF = Material::TextureFunction;
            static const TF tfx[4] = {TF::Modulate, TF::Decal, TF::Highlight, TF::Highlight2};
            m.textureFunction = tfx[(c.tex0 >> 35) & 3u];
            m.textureAlpha = (c.tex0 >> 34) & 1u;
            m.clampU = (c.clamp & 3u) == 1u;
            m.clampV = ((c.clamp >> 2) & 3u) == 1u;
            m.nearest = ((c.tex1 >> 5) & 1u) == 0u;
            m.useQ = ((attrs >> 4) & 1u) && !((attrs >> 8) & 1u); // textured with STQ
            // PRIM.FGE: the colour goes toward FOGCOL by 1 - F / 255 (each vertex's F, from XYZF or FOG), after the
            // texture function and before blending, as the GS does
            m.fog = (attrs >> 5) & 1u;
            m.fogColour = m.fog ? static_cast<uint32_t>(g_s.fogcol & 0xFFFFFFu) : 0u;
            return m;
        }

        // A primitive's vertices in GS screen coordinates (over 16) as native triangles.
        // What a primitive's vertices are made with (PRIM's attributes and the context): its texture (or the previous
        // frame's size), U / V's scale. The texture's size is a power of two, so U / 16 / width is U times the exact
        // 1 / (16 x width): the same bits, no division.
        struct Maker
        {
            const Context *c = nullptr;
            uint64_t attrs = 0;
            bool textured = false, fst = false, gouraud = false, previousFrame = false;
            const std::shared_ptr<Texture> *tex = nullptr;
            float su = 1.0f, sv = 1.0f;
            Vertex operator()(const GsVertex &g, uint32_t colour) const
            {
                Vertex r;
                r.x = g.x;
                r.y = g.y;
                r.z = float(g.z); // GS Z, larger nearer (Screen::depthMax from ZBUF's format)
                r.rgba = colour;
                r.fog = float(g.fog) * (1.0f / 255.0f); // F (drawn when PRIM.FGE is set)
                if (fst)
                {
                    r.u = g.u * su;
                    r.v = g.v * sv;
                }
                else
                {
                    // STQ: S / Q, T / Q at the vertex and Q for the renderer to interpolate them in perspective
                    // (Material::useQ), as the GS does; divided and interpolated linearly, a floor's texture smears.
                    const float q = g.q != 0.0f ? g.q : 1.0f;
                    r.u = g.u / q;
                    r.v = g.v / q;
                    r.q = q;
                }
                return r;
            }
        };
        Maker makerFor(uint64_t attrs)
        {
            static const std::shared_ptr<Texture> none;
            Maker m;
            m.attrs = attrs;
            m.c = &g_s.ctx[(attrs >> 9) & 1u];
            m.textured = (attrs >> 4) & 1u;
            m.fst = (attrs >> 8) & 1u;
            m.gouraud = (attrs >> 3) & 1u;
            m.previousFrame = samplesPreviousFrame(*m.c, attrs);
            m.tex = m.textured && !m.previousFrame ? &texture(m.c->tex0) : &none; // no copy a primitive
            const std::shared_ptr<Texture> &tex = *m.tex;
            if (m.fst)
            {
                // The texture's size: decoded, or TEX0's for the previous frame (u, v across the frame, 0 to 1).
                const float tw = tex ? float(tex->width) : m.previousFrame ? float(1u << ((m.c->tex0 >> 26) & 0xFu)) : 1.0f;
                const float th = tex ? float(tex->height) : m.previousFrame ? float(1u << ((m.c->tex0 >> 30) & 0xFu)) : 1.0f;
                m.su = 1.0f / (16.0f * tw);
                m.sv = 1.0f / (16.0f * th);
            }
            return m;
        }

        // The draw's materials from the registers they are made of: made again only when one of those changed. The
        // alpha test's failing pixels (TEST.AFAIL): KEEP drops them; FB_ONLY still writes their colour (not Z),
        // ZB_ONLY their Z (not the colour), RGB_ONLY their RGB: a second pass with the opposite test.
        struct Made
        {
            uint64_t in[9] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
            Material pass, fail;
            bool drawPass = true, twoPass = false;
            uint64_t serial = 0; // even: the pass, odd: the failing pixels
        } g_made;

        // Where a primitive's triangles go: its frame buffer's last batch (or last two: a two-pass draw), a new one when
        // the texture or the materials changed. False for ZTST NEVER (not drawn). The vectors stay good while no other
        // primitive is placed.
        bool targetFor(const Maker &m, Batch *&into, Batch *&alsoInto, const std::array<float, 16> *matrix = nullptr)
        {
            const Context &c = *m.c;
            into = alsoInto = nullptr;
            if (((c.test >> 16) & 1u) && ((c.test >> 17) & 3u) == 0u)
                return false; // ZTST NEVER
            const uint32_t fbp = c.frame & 0x1FFu;
            Made &made = g_made;
            const uint64_t in[9] = {c.tex0, c.tex1, c.clamp, c.alpha, c.test, c.zbuf, m.attrs & 0x170u, c.frame,
                                    (m.attrs & 0x20u) ? g_s.fogcol : 0u}; // (PRIM's TME, FGE, ABE, FST; FOGCOL when fogged)
            bool changed = false;
            for (int k = 0; k < 9; ++k)
                changed |= in[k] != made.in[k];
            if (changed)
            {
                std::memcpy(made.in, in, sizeof(in));
                made.serial += 2;
                made.pass = material(c, m.attrs);
                const uint32_t afail = (c.test >> 12) & 3u;
                made.twoPass = made.pass.alphaTest && afail != 0u && made.pass.alphaFunc != Material::AlphaFunc::Always;
                made.drawPass = true;
                if (made.twoPass)
                {
                    using F = Material::AlphaFunc;
                    static const F opposite[8] = {F::Always, F::Never, F::GreaterEqual, F::Greater, F::NotEqual, F::Less, F::LessEqual, F::Equal};
                    Material &fail = made.fail;
                    fail = made.pass;
                    fail.alphaFunc = opposite[static_cast<uint32_t>(made.pass.alphaFunc)];
                    if (fail.alphaFunc == F::Always)
                        fail.alphaTest = false;
                    if (afail == 1u)
                        fail.depthWrite = false;
                    else if (afail == 2u)
                        fail.colourMask = 0;
                    else
                    {
                        fail.colourMask = 0x7u;
                        fail.depthWrite = false;
                    }
                    made.drawPass = made.pass.alphaFunc != F::Never;
                }
            }
            // (the frame buffer's list kept from the last primitive: the lists stay in the map from frame to frame)
            static uint32_t listFbp = ~0u;
            static std::vector<Batch> *listOf = nullptr;
            if (fbp != listFbp)
            {
                listOf = &g_frameDraws[fbp];
                listFbp = fbp;
            }
            auto &list = *listOf;
            const std::shared_ptr<Texture> &tex = *m.tex;
            // A batch grows only while its vertices are the last made (another frame buffer's draws between: a new one).
            const uint32_t end = g_renderer.builtVertices();
            auto same = [&](const Batch &b, uint64_t serial) {
                return b.made == serial && b.texture == tex && b.first + b.count == end && b.native == (matrix != nullptr) &&
                       (!matrix || b.matrix == *matrix);
            };
            auto fresh = [&](const Material &material, uint64_t serial) {
                Batch b;
                b.material = material;
                if (matrix)
                {
                    b.native = true;
                    b.matrix = *matrix;
                    b.material.useQ = false; // the matrix gives the perspective
                }
                b.texture = tex;
                b.made = serial;
                b.first = end;
                list.push_back(std::move(b));
            };
            const size_t n = list.size();
            if (!made.twoPass || !made.drawPass)
            {
                const uint64_t serial = made.serial + (made.twoPass ? 1u : 0u);
                if (!(n && same(list[n - 1], serial)))
                    fresh(made.twoPass ? made.fail : made.pass, serial);
                into = &list.back();
            }
            else
            {
                if (!(n >= 2 && same(list[n - 2], made.serial) && same(list[n - 1], made.serial + 1) &&
                      list[n - 2].first == list[n - 1].first))
                {
                    fresh(made.pass, made.serial);
                    fresh(made.fail, made.serial + 1);
                }
                into = &list[list.size() - 2];
                alsoInto = &list.back();
            }
            return true;
        }

        // Room for a primitive's n vertices in the frame being built, counted to its batch (and the two-pass draw's
        // other batch: the same range).
        Vertex *place(Batch *into, Batch *alsoInto, uint32_t n)
        {
            Vertex *p = g_renderer.appendVertices(n);
            into->count += n;
            if (alsoInto)
                alsoInto->count += n;
            return p;
        }

        void checksum(const Vertex *p, size_t n) // SHINO_CHECKSUM: FNV-1a over what is drawn
        {
            for (size_t i = 0; i < n; ++i)
                for (const uint8_t b : std::bit_cast<std::array<uint8_t, sizeof(Vertex)>>(p[i]))
                    g_checksum = (g_checksum ^ b) * 0x100000001B3ull;
        }

        void noteDraw(); // widescreen: a draw of the widened main view makes the frame a 16:9 one (below)

        void emit(const GsVertex *v, uint32_t count, uint32_t type, uint64_t attrs)
        {
            if (type == 0 || type == 7)
                return; // points, the undefined type: not drawn
            noteDraw();
            const Maker make = makerFor(attrs);
            const Context &c = *make.c;
            const bool textured = make.textured, fst = make.fst, gouraud = make.gouraud;
            const std::shared_ptr<Texture> &tex = *make.tex;
            ++g_prims;
            Batch *into = nullptr, *alsoInto = nullptr;
            targetFor(make, into, alsoInto);
            Vertex tri[6]; // the primitive's triangles
            uint32_t made = 0;
            if (type == 6) // sprite: two corners
            {
                const uint32_t col = v[1].rgba;
                const Vertex a = make(v[0], col), b = make(v[1], col);
                Vertex tr = a, bl = a;
                tr.x = b.x; tr.u = b.u;
                bl.y = b.y; bl.v = b.v;
                tri[0] = a; tri[1] = tr; tri[2] = b;
                tri[3] = a; tri[4] = b; tri[5] = bl;
                made = 6;
            }
            else if (type >= 3) // triangles (list, strip, fan)
            {
                const uint32_t flat = v[2].rgba;
                for (uint32_t i = 0; i < 3; ++i)
                    tri[i] = make(v[i], gouraud ? v[i].rgba : flat);
                made = 3;
            }
            else // lines as thin quads
            {
                const Vertex a = make(v[0], gouraud ? v[0].rgba : v[1].rgba), b = make(v[1], v[1].rgba);
                Vertex a2 = a, b2 = b;
                a2.y += 1; b2.y += 1;
                tri[0] = a; tri[1] = b; tri[2] = b2;
                tri[3] = a; tri[4] = b2; tri[5] = a2;
                made = 6;
            }
            if (into) // drawn (not ZTST NEVER): into the frame being built
                std::memcpy(place(into, alsoInto, made), tri, made * sizeof(Vertex));
            const size_t madeCount = made;
            struct View
            {
                const Vertex *p;
                size_t n;
                size_t size() const { return n; }
                const Vertex &operator[](size_t i) const { return p[i]; }
                const Vertex *begin() const { return p; }
                const Vertex *end() const { return p + n; }
            } const out{tri, madeCount};
            if (static const char *dump = std::getenv("SHINO_DUMP_PRIMS"); dump && g_flips == static_cast<uint64_t>(std::atoll(dump)))
                std::printf("[shino:prim] type=%u tex=%d fbp=%x tex0=%llx v0=(%.1f,%.1f) v1=(%.1f,%.1f) rgba=%08x abe=%d\n", type,
                            int(textured), unsigned(c.frame & 0x1FFu), (unsigned long long)c.tex0, v[0].x, v[0].y,
                            v[count > 1 ? 1 : 0].x, v[count > 1 ? 1 : 0].y, v[count - 1].rgba, int((attrs >> 6) & 1u));
            if (static const char *dump = std::getenv("SHINO_DUMP_PRIMS"); dump && g_flips == static_cast<uint64_t>(std::atoll(dump)) && textured)
                std::printf("[shino:uv] fst=%d tex1=%llx clamp=%llx stq0=(%g %g %g) stq1=(%g %g %g) stq2=(%g %g %g) uv0=(%.3f %.3f) uv2=(%.3f %.3f)\n",
                            int(fst), (unsigned long long)c.tex1, (unsigned long long)c.clamp, v[0].u, v[0].v, v[0].q,
                            v[count > 1 ? 1 : 0].u, v[count > 1 ? 1 : 0].v, v[count > 1 ? 1 : 0].q, v[count > 2 ? 2 : 0].u,
                            v[count > 2 ? 2 : 0].v, v[count > 2 ? 2 : 0].q, out[0].u, out[0].v, out[out.size() > 2 ? 2 : 0].u,
                            out[out.size() > 2 ? 2 : 0].v);
            // SHINO_PICK=<flip>,<x>,<y>[,<x>,<y>...]: every triangle over those window pixels, in drawing order.
            if (static const char *pick = std::getenv("SHINO_PICK"); pick && g_flips == std::strtoull(pick, nullptr, 10))
                // (the variable read once: getenv a primitive was a good part of the translator's time)
                for (const char *at = std::strchr(pick, ','); at;)
                {
                    float px = std::strtof(at + 1, nullptr);
                    at = std::strchr(at + 1, ',');
                    if (!at)
                        break;
                    float py = std::strtof(at + 1, nullptr);
                    at = std::strchr(at + 1, ',');
                    for (size_t i = 0; i + 2 < out.size(); i += 3)
                    {
                        const Vertex &a = out[i], &b = out[i + 1], &d = out[i + 2];
                        auto side = [&](const Vertex &p, const Vertex &q) { return (q.x - p.x) * (py - p.y) - (q.y - p.y) * (px - p.x); };
                        const float s0 = side(a, b), s1 = side(b, d), s2 = side(d, a);
                        if (!((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0)))
                            continue;
                        std::printf("[shino:pick] at=%.0f,%.0f type=%u tex=%d tex0=%llx tex1=%llx clamp=%llx abe=%d alpha=%llx test=%llx fst=%d %ux%u "
                                    "xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) uvq=(%.3f %.3f %.4f)(%.3f %.3f %.4f)(%.3f %.3f %.4f) rgba=%08x z=%u\n",
                                    px, py, type, int(textured), (unsigned long long)c.tex0, (unsigned long long)c.tex1,
                                    (unsigned long long)c.clamp, int((attrs >> 6) & 1u), (unsigned long long)c.alpha,
                                    (unsigned long long)c.test, int(fst), tex ? tex->width : 0, tex ? tex->height : 0, a.x, a.y,
                                    b.x, b.y, d.x, d.y, a.u, a.v, a.q, b.u, b.v, b.q, d.u, d.v, d.q, a.rgba, v[0].z);
                    }
            }
            if (static const bool sum = std::getenv("SHINO_CHECKSUM") != nullptr; sum) // FNV-1a over what is drawn
                for (const Vertex &x : out)
                    for (const uint8_t b : std::bit_cast<std::array<uint8_t, sizeof(Vertex)>>(x))
                        g_checksum = (g_checksum ^ b) * 0x100000001B3ull;
            // SHINO_SURVEY=1: the primitives by what the renderer does not do yet (a scissor short of the frame, FRAME's
            // FBMSK, a texture where a frame buffer is), every 250 flips.
            if (static const bool survey = std::getenv("SHINO_SURVEY") != nullptr; survey)
            {
                const uint32_t fbmsk = static_cast<uint32_t>(c.frame >> 32), fbw = ((c.frame >> 16) & 0x3Fu) * 64u;
                const uint32_t x1 = (c.scissor >> 16) & 0x7FFu, y1 = (c.scissor >> 48) & 0x7FFu;
                const bool clipped = (c.scissor & 0x7FFu) != 0 || ((c.scissor >> 32) & 0x7FFu) != 0 || x1 + 1 < fbw || y1 + 1 < 448;
                const uint32_t tbp = c.tex0 & 0x3FFFu;
                const bool fromFrame = textured && (tbp == (c.frame & 0x1FFu) * 32u || tbp == 0u || tbp == 0x1180u /* 0x8c*32 */);
                char key[160];
                std::snprintf(key, sizeof(key), "fbp=%x%s%s%s%s", unsigned(c.frame & 0x1FFu),
                              clipped ? " scissor" : "", fbmsk ? " fbmsk" : "", fromFrame ? " tex-from-frame" : "",
                              (c.frame & 0x1FFu) != g_displayFbp ? " offscreen" : "");
                if (clipped || fbmsk || fromFrame)
                {
                    char detail[96];
                    if (fromFrame)
                        std::snprintf(detail, sizeof(detail), " [type %u tex0 %llx alpha %llx xy %.0f,%.0f %.0f,%.0f]", type,
                                      (unsigned long long)c.tex0, (unsigned long long)c.alpha, out[0].x, out[0].y,
                                      out[out.size() / 2].x, out[out.size() / 2].y);
                    else
                        std::snprintf(detail, sizeof(detail), " [scissor %llx fbmsk %08x tbp %x]", (unsigned long long)c.scissor, fbmsk, tbp);
                    std::strncat(key, detail, sizeof(key) - std::strlen(key) - 1);
                }
                ++g_survey[key];
            }
        }

        void vertexKick(uint64_t xyz, uint32_t z, uint32_t fog, bool draw)
        {
            const uint64_t attrs = (g_s.prmodecont & 1u) ? g_s.prim : (g_s.prmode & ~7ull) | (g_s.prim & 7u);
            const Context &c = g_s.ctx[(attrs >> 9) & 1u];
            if (static const char *dump = std::getenv("SHINO_DUMP_KICKS"); dump && g_flips == static_cast<uint64_t>(std::atoll(dump)))
                std::printf("[shino:kick] prim=%03llx prmodecont=%llx draw=%d queue=%zu xy=%llx\n", (unsigned long long)g_s.prim,
                            (unsigned long long)g_s.prmodecont, int(draw), g_queue.size(), (unsigned long long)xyz);
            GsVertex v; // window pixels: the primitive coordinate less the context's XYOFFSET
            v.x = (float(xyz & 0xFFFFu) - float(c.xyoffset & 0xFFFFu)) / 16.0f;
            v.y = (float((xyz >> 32) & 0xFFFFu) - float((c.xyoffset >> 32) & 0xFFFFu)) / 16.0f;
            v.z = z;
            v.fog = fog;
            v.rgba = static_cast<uint32_t>(g_s.rgbaq & 0xFFFFFFFFu);
            v.q = g_s.q;
            if ((attrs >> 8) & 1u)
            {
                v.u = float(g_s.u);
                v.v = float(g_s.v);
            }
            else
            {
                v.u = g_s.s;
                v.v = g_s.t;
            }
            const uint32_t type = attrs & 7u;
            g_queue.push_back(v);
            static const uint32_t need[8] = {1, 2, 2, 3, 3, 3, 2, 1};
            if (g_queue.size() < need[type])
                return;
            if (draw)
                emit(g_queue.data() + g_queue.size() - need[type] - (type == 5 ? 0 : 0), need[type], type, attrs);
            switch (type)
            {
            case 2: g_queue.eraseAt(0); break;                                    // line strip: keep the last
            case 4: g_queue.eraseAt(0); break;                                    // triangle strip: keep two
            case 5: g_queue.eraseAt(1); break;                                    // fan: keep the first and the last
            default: g_queue.clear(); break;
            }
        }

        // The common XGKICK tag, triangles: its registers only ST, RGBAQ, UV, FOG and XYZ (and NOP), so what its
        // primitives are drawn with (PRIM, the context, the texture, the materials, the batch) is the same for every one
        // of them: found once, at the first triangle drawn, and each triangle made into the batch as its vertices come.
        // The same vertices, batches and checksum as register by register (packedOne, vertexKick, emit), which takes
        // any other tag, and every tag while a primitive trace is on.
        bool packedFast(const uint8_t *vuMemory, uint32_t at, uint64_t regs, uint32_t nreg, uint32_t loops)
        {
            static const bool traced = std::getenv("SHINO_DUMP_PRIMS") || std::getenv("SHINO_PICK") ||
                                       std::getenv("SHINO_SURVEY") || std::getenv("SHINO_DUMP_KICKS");
            if (traced || loops == 0)
                return false;
            bool xyz = false;
            for (uint32_t k = 0; k < nreg; ++k)
            {
                const uint32_t r = static_cast<uint32_t>(regs >> (4 * k)) & 0xFu;
                if (r == 0x4 || r == 0x5)
                    xyz = true;
                else if (r != 0x1 && r != 0x2 && r != 0x3 && r != 0xA && r != 0xF)
                    return false;
            }
            State &s = g_s;
            const uint64_t attrs = (s.prmodecont & 1u) ? s.prim : (s.prmode & ~7ull) | (s.prim & 7u);
            const uint32_t type = attrs & 7u;
            if (!xyz || type < 3 || type > 5)
                return false;
            const Context &c = s.ctx[(attrs >> 9) & 1u];
            const float ox = float(c.xyoffset & 0xFFFFu), oy = float((c.xyoffset >> 32) & 0xFFFFu);
            const bool stq = !((attrs >> 8) & 1u), gouraud = (attrs >> 3) & 1u;
            static const bool sum = std::getenv("SHINO_CHECKSUM") != nullptr;
            Maker make;
            bool placed = false, drawn = false;
            Batch *into = nullptr, *alsoInto = nullptr;
            for (uint32_t l = 0; l < loops; ++l)
                for (uint32_t k = 0; k < nreg; ++k, ++at)
                {
                    uint64_t lo, hi;
                    const uint8_t *q = vuMemory + 16 * (at & 0x3FFu);
                    std::memcpy(&lo, q, 8);
                    std::memcpy(&hi, q + 8, 8);
                    const uint32_t r = static_cast<uint32_t>(regs >> (4 * k)) & 0xFu;
                    uint32_t z, fog;
                    switch (r)
                    {
                    case 0x1: // RGBAQ (packedOne)
                        s.rgbaq = (lo & 0xFFu) | ((lo >> 24) & 0xFF00u) | ((hi & 0xFFu) << 16) | ((hi >> 8) & 0xFF000000u) |
                                  (uint64_t(std::bit_cast<uint32_t>(s.q)) << 32);
                        continue;
                    case 0x2: // ST, Q
                        s.s = std::bit_cast<float>(static_cast<uint32_t>(lo));
                        s.t = std::bit_cast<float>(static_cast<uint32_t>(lo >> 32));
                        s.q = std::bit_cast<float>(static_cast<uint32_t>(hi));
                        continue;
                    case 0x3: s.u = lo & 0x3FFFu; s.v = (lo >> 32) & 0x3FFFu; continue;
                    case 0xA: s.fog = (hi >> 36) & 0xFFu; continue;
                    case 0x4: z = (hi >> 4) & 0xFFFFFFu; fog = (hi >> 36) & 0xFFu; break; // XYZF2
                    case 0x5: z = static_cast<uint32_t>(hi); fog = s.fog; break;          // XYZ2
                    default: continue;                                                     // NOP
                    }
                    // the vertex (vertexKick): window pixels, the primitive coordinate less the context's XYOFFSET
                    GsVertex v;
                    v.x = (float(lo & 0xFFFFu) - ox) / 16.0f;
                    v.y = (float((lo >> 32) & 0xFFFFu) - oy) / 16.0f;
                    v.z = z;
                    v.fog = fog;
                    v.rgba = static_cast<uint32_t>(s.rgbaq & 0xFFFFFFFFu);
                    v.q = s.q;
                    v.u = stq ? s.s : float(s.u);
                    v.v = stq ? s.t : float(s.v);
                    g_queue.push_back(v);
                    if (g_queue.size() < 3)
                        continue;
                    if (!((hi >> 47) & 1u)) // ADC clear: drawn
                    {
                        if (!placed)
                        {
                            make = makerFor(attrs);
                            drawn = targetFor(make, into, alsoInto);
                            placed = true;
                            noteDraw();
                        }
                        ++g_prims;
                        const GsVertex *t = g_queue.data() + g_queue.size() - 3;
                        const uint32_t flat = t[2].rgba;
                        const Vertex a = make(t[0], gouraud ? t[0].rgba : flat), b = make(t[1], gouraud ? t[1].rgba : flat),
                                     d = make(t[2], gouraud ? t[2].rgba : flat);
                        if (drawn) // straight into the frame being built
                        {
                            Vertex *p = place(into, alsoInto, 3);
                            p[0] = a;
                            p[1] = b;
                            p[2] = d;
                        }
                        if (sum)
                        {
                            const Vertex three[3] = {a, b, d};
                            checksum(three, 3);
                        }
                    }
                    switch (type)
                    {
                    case 4: g_queue.eraseAt(0); break; // strip: keep two
                    case 5: g_queue.eraseAt(1); break; // fan: keep the first and the last
                    default: g_queue.clear(); break;
                    }
                }
            return true;
        }
    } // namespace

    namespace
    {
        // A raw XYZ register's X (bits 0-15) and Y (16-31) as vertexKick takes them (X low, Y at bit 32).
        uint64_t rawXy(uint64_t value) { return (value & 0xFFFFu) | (((value >> 16) & 0xFFFFu) << 32); }
    }
    void imageFlush();
    void imageQuadword(const uint8_t *quadword);

    bool (*widescreenWanted)() = nullptr;
    void (*widescreenShown)(bool) = nullptr;
    bool widescreenOn()
    {
        static const bool fromEnvironment = std::getenv("SHINO_WIDESCREEN") && *std::getenv("SHINO_WIDESCREEN") == '1';
        return fromEnvironment || (widescreenWanted && widescreenWanted());
    }
    namespace
    {
        constexpr float kWide = 0.75f;        // a 4:3 picture's width in a 16:9 one
        ViewKind g_view = ViewKind::Unmarked; // the view last set up (its GS set-up's mark)
        bool g_frameWide = false;             // this frame's main view widened (the game drew it for 16:9)
        bool g_frameHad3D = false;            // a draw of the widened main view this frame (native or the GS packets')
        bool g_lastWide = false;              // the frame before shown 16:9
        // the HUD narrowed: this frame's main view widened so far, or the last frame's (the HUD may come before the 3D)
        bool narrowing() { return g_frameWide || g_lastWide; }
        void noteDraw()
        {
            if (g_view == ViewKind::MainWide)
                g_frameHad3D = true;
        }
        float frameWidth(const Context &c) { return float(((c.frame >> 16) & 0x3Fu) * 64u); }
        // Where a 4:3 drawing at screen x stays put in the 16:9 picture: the left edge, the centre or the right edge
        // for x in the screen's left, middle or right third (the HUD's elements keep to their side).
        float anchorFor(float x, float width) { return x < width / 3.0f ? 0.0f : x > width * 2.0f / 3.0f ? width : width * 0.5f; }
    }

    void screenPrimitives(uint64_t prim, const ScreenVertex *vertices, uint32_t count, Place place)
    {
        writeRegister(0x00, prim);
        const uint64_t attrs = (g_s.prmodecont & 1u) ? g_s.prim : (g_s.prmode & ~7ull) | (g_s.prim & 7u);
        const Context &c = g_s.ctx[(attrs >> 9) & 1u];
        const uint32_t type = attrs & 7u;
        static const uint32_t need[8] = {1, 2, 2, 3, 3, 3, 2, 1};
        // widescreen: the HUD's triangles 3/4 as wide towards their side; the particles already drawn for 16:9 by the game
        const float width = frameWidth(c);
        const bool narrow = narrowing() && width > 0.0f && place == Place::Hud;
        GsVertex g[3];
        for (uint32_t i = 0; i + need[type] <= count; i += need[type])
        {
            for (uint32_t j = 0; j < need[type]; ++j)
            {
                const ScreenVertex &w = vertices[i + j];
                GsVertex &v = g[j];
                v.x = (float(w.x & 0xFFFFu) - float(c.xyoffset & 0xFFFFu)) / 16.0f;
                v.y = (float(w.y & 0xFFFFu) - float((c.xyoffset >> 32) & 0xFFFFu)) / 16.0f;
                v.z = w.z;
                v.fog = w.fog;
                v.rgba = w.rgba;
                v.q = w.q;
                v.u = w.s;
                v.v = w.t;
            }
            if (narrow)
            {
                float centre = 0.0f;
                for (uint32_t j = 0; j < need[type]; ++j)
                    centre += g[j].x;
                const float anchor = anchorFor(centre / float(need[type]), width);
                for (uint32_t j = 0; j < need[type]; ++j)
                    g[j].x = anchor + (g[j].x - anchor) * kWide;
            }
            if (!vertices[i + need[type] - 1].adc)
                emit(g, need[type], type, attrs);
        }
        if (count) // the registers as the packets leave them
        {
            const ScreenVertex &l = vertices[count - 1];
            g_s.rgbaq = l.rgba | (uint64_t(std::bit_cast<uint32_t>(l.q)) << 32);
            g_s.s = l.s;
            g_s.t = l.t;
            g_s.q = l.q;
            g_s.fog = l.fog;
        }
    }

    void nativeTriangles(uint64_t prim, const float matrix[16], const Vertex *vertices, uint32_t count)
    {
        if (prim != ~0ull)
            writeRegister(0x00, prim);
        const uint64_t attrs = (g_s.prmodecont & 1u) ? g_s.prim : (g_s.prmode & ~7ull) | (g_s.prim & 7u);
        const Maker make = makerFor(attrs);
        const Context &c = *make.c;
        // GS screen pixels to window pixels: less the context's XYOFFSET (12.4), as vertexKick takes it off.
        std::array<float, 16> m;
        std::copy(matrix, matrix + 16, m.begin());
        const float ox = float(c.xyoffset & 0xFFFFu) / 16.0f, oy = float((c.xyoffset >> 32) & 0xFFFFu) / 16.0f;
        for (int col = 0; col < 4; ++col)
        {
            m[4 * col + 0] -= ox * m[4 * col + 3];
            m[4 * col + 1] -= oy * m[4 * col + 3];
        }
        noteDraw(); // widescreen: the views drawn for 16:9 by the game
        Batch *into = nullptr, *alsoInto = nullptr;
        if (!targetFor(make, into, alsoInto, &m))
            return;
        Vertex *p = place(into, alsoInto, count);
        std::copy(vertices, vertices + count, p);
        g_prims += count / 3;
        if (static const bool sum = std::getenv("SHINO_CHECKSUM") != nullptr; sum) // the native batches' own: matrix and vertices
        {
            for (const uint8_t b : std::bit_cast<std::array<uint8_t, sizeof(m)>>(m))
                g_nativeChecksum = (g_nativeChecksum ^ b) * 0x100000001B3ull;
            for (uint32_t i = 0; i < count; ++i)
                for (const uint8_t b : std::bit_cast<std::array<uint8_t, sizeof(Vertex)>>(vertices[i]))
                    g_nativeChecksum = (g_nativeChecksum ^ b) * 0x100000001B3ull;
        }
    }

    void writeRegister(uint32_t address, uint64_t value)
    {
        State &s = g_s;
        switch (address)
        {
        case 0x00: s.prim = value; g_queue.clear(); break;
        case 0x01: s.rgbaq = value; s.q = std::bit_cast<float>(static_cast<uint32_t>(value >> 32)); break;
        case 0x02: s.s = std::bit_cast<float>(static_cast<uint32_t>(value)); s.t = std::bit_cast<float>(static_cast<uint32_t>(value >> 32)); break;
        case 0x03: s.u = value & 0x3FFFu; s.v = (value >> 16) & 0x3FFFu; break;
        case 0x04: vertexKick(rawXy(value), (value >> 32) & 0xFFFFFFu, static_cast<uint32_t>(value >> 56), true); break;
        case 0x05: vertexKick(rawXy(value), static_cast<uint32_t>(value >> 32), s.fog, true); break;
        case 0x0C: vertexKick(rawXy(value), (value >> 32) & 0xFFFFFFu, static_cast<uint32_t>(value >> 56), false); break;
        case 0x0D: vertexKick(rawXy(value), static_cast<uint32_t>(value >> 32), s.fog, false); break;
        case 0x06: case 0x07: s.ctx[address - 0x06].tex0 = value; break;
        case 0x08: case 0x09: s.ctx[address - 0x08].clamp = value; break;
        case 0x0A: s.fog = static_cast<uint32_t>(value >> 56); break;
        case 0x14: case 0x15: s.ctx[address - 0x14].tex1 = value; break;
        case 0x16: case 0x17: // TEX2: TEX0's PSM (bits 20-25) and palette fields (37-63) only
        {
            constexpr uint64_t fields = (0x3Full << 20) | (~0ull << 37);
            uint64_t &t = s.ctx[address - 0x16].tex0;
            t = (t & ~fields) | (value & fields);
            break;
        }
        case 0x18: case 0x19: s.ctx[address - 0x18].xyoffset = value; break;
        case 0x45: // DTHE (not drawn): the port's mark of the view whose draws follow (shino_port.cpp)
            g_view = static_cast<ViewKind>((value >> 8) & 0xFFu);
            if (g_view == ViewKind::MainWide)
                g_frameWide = true;
            break;
        case 0x1A: s.prmodecont = value; break;
        case 0x1B: s.prmode = value; break;
        case 0x1C: s.texclut = value; break;
        case 0x3B: s.texa = value; break;
        case 0x3D: s.fogcol = value; break;
        case 0x40: case 0x41: s.ctx[address - 0x40].scissor = value; break;
        case 0x42: case 0x43: s.ctx[address - 0x42].alpha = value; break;
        case 0x47: case 0x48: s.ctx[address - 0x47].test = value; break;
        case 0x4C: case 0x4D: s.ctx[address - 0x4C].frame = value; break;
        case 0x4E: case 0x4F: s.ctx[address - 0x4E].zbuf = value; break;
        case 0x50: s.bitbltbuf = value; break;
        case 0x51: s.trxpos = value; break;
        case 0x52: s.trxreg = value; break;
        case 0x53: // TRXDIR: 0 host to local starts with the next IMAGE data; 2 local to local is a copy
            if ((value & 3u) == 0)
            {
                Image &im = g_image;
                imageFlush(); // a transfer cut short: what came of it
                im.psm = (s.bitbltbuf >> 56) & 0x3Fu;
                im.bp = (s.bitbltbuf >> 32) & 0x3FFFu;
                im.bw = std::max<uint32_t>((s.bitbltbuf >> 48) & 0x3Fu, 1u);
                im.x0 = (s.trxpos >> 32) & 0x7FFu;
                im.y0 = (s.trxpos >> 48) & 0x7FFu;
                im.w = s.trxreg & 0xFFFu;
                im.h = (s.trxreg >> 32) & 0xFFFu;
                im.x = im.y = 0;
                im.carry = im.carryBits = 0;
                const uint32_t bits = im.psm == 0x01 || im.psm == 0x31 ? 24u : (im.psm & 0xFu) == 0x2 || (im.psm & 0xFu) == 0xA ? 16u
                                      : im.psm == 0x13 || im.psm == 0x1B ? 8u : im.psm == 0x14 || im.psm == 0x24 || im.psm == 0x2C ? 4u : 32u;
                im.need = static_cast<uint32_t>((uint64_t(im.w) * im.h * bits / 8u + 15u) / 16u * 16u);
                im.data.clear();
                im.active = im.need != 0;
                ++g_uploads;
                ++g_generation;
            }
            else if ((value & 3u) == 2) // local to local: a rectangle copied pixel by pixel
            {
                const uint32_t spsm = (s.bitbltbuf >> 24) & 0x3Fu, sbp = s.bitbltbuf & 0x3FFFu, sbw = std::max<uint32_t>((s.bitbltbuf >> 16) & 0x3Fu, 1u);
                const uint32_t dpsm = (s.bitbltbuf >> 56) & 0x3Fu, dbp = (s.bitbltbuf >> 32) & 0x3FFFu, dbw = std::max<uint32_t>((s.bitbltbuf >> 48) & 0x3Fu, 1u);
                const uint32_t sx = s.trxpos & 0x7FFu, sy = (s.trxpos >> 16) & 0x7FFu, dx = (s.trxpos >> 32) & 0x7FFu, dy = (s.trxpos >> 48) & 0x7FFu;
                const uint32_t w = s.trxreg & 0xFFFu, h = (s.trxreg >> 32) & 0xFFFu;
                ++g_generation;
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                        writePixel(dpsm, dbp, dbw, dx + x, dy + y, readPixel(spsm, sbp, sbw, sx + x, sy + y));
            }
            break;
        case 0x54: // HWREG: image data written as a register
        {
            uint8_t q[8];
            std::memcpy(q, &value, 8);
            imageBytes(q, 8); // half a quadword of the image's pixels
            break;
        }
        default: break;
        }
    }

    namespace
    {
        void packedOne(uint32_t r, uint64_t lo, uint64_t hi);
    }
    void packed(uint32_t r, uint64_t lo, uint64_t hi) { packedOne(r, lo, hi); }

    // A PACKED tag's data (loops x nreg quadwords) in VU1 data memory from quadword `at` (wrapping at 1024): the
    // registers in turn, all in this file (no call a quadword).
    void packedRun(const uint8_t *vuMemory, uint32_t at, uint64_t regs, uint32_t nreg, uint32_t loops)
    {
        if (packedFast(vuMemory, at, regs, nreg, loops))
            return;
        for (uint32_t l = 0; l < loops; ++l)
            for (uint32_t k = 0; k < nreg; ++k, ++at)
            {
                uint64_t lo, hi;
                const uint8_t *q = vuMemory + 16 * (at & 0x3FFu);
                std::memcpy(&lo, q, 8);
                std::memcpy(&hi, q + 8, 8);
                packedOne(static_cast<uint32_t>(regs >> (4 * k)) & 0xFu, lo, hi);
            }
    }

    namespace
    {
    inline void packedOne(uint32_t r, uint64_t lo, uint64_t hi)
    {
        State &s = g_s;
        switch (r)
        {
        case 0x0: writeRegister(0x00, lo & 0x7FFu); break;
        case 0x1: // RGBAQ: R G B A in the four words' low bytes, Q from the last ST
            s.rgbaq = (lo & 0xFFu) | ((lo >> 24) & 0xFF00u) | ((hi & 0xFFu) << 16) | ((hi >> 8) & 0xFF000000u) |
                      (uint64_t(std::bit_cast<uint32_t>(s.q)) << 32);
            break;
        case 0x2: // ST, and Q for the next RGBAQ
            s.s = std::bit_cast<float>(static_cast<uint32_t>(lo));
            s.t = std::bit_cast<float>(static_cast<uint32_t>(lo >> 32));
            s.q = std::bit_cast<float>(static_cast<uint32_t>(hi));
            break;
        case 0x3: s.u = lo & 0x3FFFu; s.v = (lo >> 32) & 0x3FFFu; break;
        case 0x4: // XYZF2 (ADC: XYZF3)
            vertexKick((lo & 0xFFFFu) | (((lo >> 32) & 0xFFFFu) << 32), (hi >> 4) & 0xFFFFFFu, (hi >> 36) & 0xFFu, !((hi >> 47) & 1u));
            break;
        case 0x5: // XYZ2 (ADC: XYZ3)
            vertexKick((lo & 0xFFFFu) | (((lo >> 32) & 0xFFFFu) << 32), static_cast<uint32_t>(hi), s.fog, !((hi >> 47) & 1u));
            break;
        case 0xA: s.fog = (hi >> 36) & 0xFFu; break;
        case 0xC: vertexKick((lo & 0xFFFFu) | (((lo >> 32) & 0xFFFFu) << 32), (hi >> 4) & 0xFFFFFFu, (hi >> 36) & 0xFFu, false); break;
        case 0xD: vertexKick((lo & 0xFFFFu) | (((lo >> 32) & 0xFFFFu) << 32), static_cast<uint32_t>(hi), s.fog, false); break;
        case 0xE: writeRegister(static_cast<uint32_t>(hi & 0xFFu), lo); break; // A+D
        case 0xF: break;
        default: writeRegister(r, lo); break; // TEX0_1/2, CLAMP_1/2: the register's value as is
        }
    }

    } // namespace

    void reglist(uint32_t r, uint64_t value) { writeRegister(r, value); }

    void imageBegin() {}

    // An upload's quadwords gathered; complete, written to the local memory, unless they are the bytes the
    // destination already holds from the same upload (Uploaded).
    void imageData(const uint8_t *quadword) { imageBytes(quadword, 16); }

    namespace
    {
        // An upload's pixels written: `src` holds them all (`size` bytes, at least im.need), straight from the DMA data
        // or gathered. The game's uploads are all CT32 or CT16 (its 4- and 8-bit textures sent as 32- and 16-bit
        // pixels): whole blocks when aligned (writeBlock32 / 16: each compared as it is stored), rows otherwise. A page
        // is marked written only where a value changed.
        void writeUpload(const uint8_t *src, size_t size)
        {
            Image &im = g_image;
            g_uploadBytes[im.psm & 63u] += im.need;
            ++g_uploadCount[im.psm & 63u];
            if (size >= im.need && im.x == 0 && im.y == 0 && (im.psm == 0x00 || im.psm == 0x30 || im.psm == 0x02))
            {
                uint16_t *mem16 = reinterpret_cast<uint16_t *>(g_mem.data());
                const bool ct16 = im.psm == 0x02;
                const uint32_t blockW = ct16 ? 16u : 8u, bpp = ct16 ? 2u : 4u;
                if (im.x0 % blockW == 0 && im.w % blockW == 0 && im.y0 % 8u == 0 && im.h % 8u == 0)
                {
                    ++g_uploadsByBlock;
                    const uint32_t stride = im.w * bpp;
                    for (uint32_t by = 0; by < im.h; by += 8)
                        for (uint32_t bx = 0; bx < im.w; bx += blockW)
                        {
                            const uint32_t x = im.x0 + bx, y = im.y0 + by;
                            uint32_t at;
                            if (ct16)
                            {
                                const uint32_t b = im.bp + ((y >> 6) * im.bw << 5) + ((x >> 6) << 5) + kBlock16[(y >> 3) & 7][(x >> 4) & 3];
                                at = ((b << 7) & 0x1FFF80u) * 2u;
                            }
                            else
                            {
                                const uint32_t b = im.bp + ((y >> 5) * im.bw << 5) + ((x >> 6) << 5) + kBlock32[(y >> 3) & 3][(x >> 3) & 7];
                                at = ((b << 6) & 0xFFFC0u) * 4u;
                            }
                            const uint8_t *rows = src + by * stride + bx * bpp;
                            ++g_blocksCompared;
                            if (ct16 ? writeBlock16(memBytes() + at, rows, stride) : writeBlock32(memBytes() + at, rows, stride))
                            {
                                ++g_blocksChanged;
                                touch(at / 4u);
                            }
                        }
                }
                else if (++g_uploadsByRow; ct16)
                    for (uint32_t y = 0; y < im.h; ++y)
                        for (uint32_t x = 0; x < im.w; ++x, src += 2)
                        {
                            uint16_t value;
                            std::memcpy(&value, src, 2);
                            const uint32_t a = addr16(im.bp, im.bw, im.x0 + x, im.y0 + y, false);
                            if (mem16[a] != value)
                            {
                                mem16[a] = value;
                                touch(a >> 1);
                            }
                        }
                else
                    for (uint32_t y = 0; y < im.h; ++y)
                        for (uint32_t x = 0; x < im.w; ++x, src += 4)
                        {
                            uint32_t value;
                            std::memcpy(&value, src, 4);
                            const uint32_t a = addr32(im.bp, im.bw, im.x0 + x, im.y0 + y);
                            if (g_mem[a] != value)
                            {
                                g_mem[a] = value;
                                touch(a);
                            }
                        }
                im.active = false;
                return;
            }
            ++g_uploadsByPixel;
            for (size_t at = 0; at + 16 <= size && im.active; at += 16)
                imageQuadword(src + at);
            im.active = false;
        }
    }

    void imageBytes(const uint8_t *bytes, uint32_t count)
    {
        Image &im = g_image;
        if (!im.active)
            return;
        if (im.data.empty() && count >= im.need) // all of it here: written from the DMA data, no copy
        {
            writeUpload(bytes, count);
            return;
        }
        im.data.insert(im.data.end(), bytes, bytes + count);
        if (im.data.size() < im.need)
            return;
        writeUpload(im.data.data(), im.data.size());
        im.data.clear();
    }

    // What came of an upload cut short (a new TRXDIR before its data was all there), written as it is.
    void imageFlush()
    {
        Image &im = g_image;
        if (!im.active)
            return;
        writeUpload(im.data.data(), im.data.size());
        im.data.clear();
    }

    void imageQuadword(const uint8_t *quadword)
    {
        Image &im = g_image;
        switch (im.psm)
        {
        case 0x00: case 0x30:
            for (int i = 0; i < 4; ++i) { uint32_t v; std::memcpy(&v, quadword + 4 * i, 4); imagePixel(v); }
            break;
        case 0x01: case 0x31: // three bytes a pixel, across quadwords
            for (int i = 0; i < 16; ++i)
            {
                im.carry |= uint32_t(quadword[i]) << im.carryBits;
                if ((im.carryBits += 8) == 24) { imagePixel(im.carry); im.carry = im.carryBits = 0; }
            }
            break;
        case 0x02: case 0x0A: case 0x32: case 0x3A:
            for (int i = 0; i < 8; ++i) { uint16_t v; std::memcpy(&v, quadword + 2 * i, 2); imagePixel(v); }
            break;
        case 0x13: case 0x1B:
            for (int i = 0; i < 16; ++i) imagePixel(quadword[i]);
            break;
        case 0x14: case 0x24: case 0x2C:
            for (int i = 0; i < 16; ++i) { imagePixel(quadword[i] & 15u); imagePixel(quadword[i] >> 4); }
            break;
        default: break;
        }
        if (im.y >= im.h)
            im.active = false;
    }

    // The frame shown (DISPFB's FBP) published: its draws as the overlay, mapped by the context's XYOFFSET.
    void flip(Runtime *rt, R5900Context *ctx, uint64_t dispfb, uint64_t display)
    {
        ++g_flips;
        // Widescreen: this frame shown 16:9 when its main view was drawn so (by the game, widened) and drew 3D; movies
        // and the menus' pictures stay 4:3
        {
            if (static const bool trace = std::getenv("SHINO_TRACE_WIDE") != nullptr; trace && g_flips % 50 == 0)
                std::printf("[shino:wide] flip %llu main view widened %d drew %d\n", static_cast<unsigned long long>(g_flips),
                            g_frameWide, g_frameHad3D);
            g_lastWide = g_frameWide && g_frameHad3D;
            if (widescreenShown)
                widescreenShown(g_lastWide);
            g_frameWide = g_frameHad3D = false;
            g_view = ViewKind::Unmarked;
        }
        auto &renderer = bozo::render::Renderer::instance();
        const uint32_t fbp = dispfb & 0x1FFu;
        g_displayFbp = fbp;
        if (std::find(g_frameBuffers.begin(), g_frameBuffers.end(), fbp * 32u) == g_frameBuffers.end())
        {
            std::rotate(g_frameBuffers.rbegin(), g_frameBuffers.rbegin() + 1, g_frameBuffers.rend());
            g_frameBuffers[0] = fbp * 32u;
        }
        if (!g_survey.empty() && g_flips % 250 == 0)
        {
            std::printf("[shino:survey] flips %llu:\n", (unsigned long long)g_flips);
            for (const auto &[what, count] : g_survey)
                std::printf("[shino:survey]   %8llu %s\n", (unsigned long long)count, what.c_str());
            g_survey.clear();
        }
        bozo::render::Screen screen;
        const uint64_t off = g_s.ctx[0].xyoffset;
        screen.originX = 0.0f; // the vertices are window pixels already (vertexKick)
        screen.originY = 0.0f;
        // DISPLAY1: DW (32-43) + 1 over MAGH (23-26) + 1 is the width in frame pixels; DH (44-54) + 1 the height in
        // lines, the frame's own in interlaced frame mode (Shinobido's 512 x 512 PAL frame).
        const uint32_t magh = ((display >> 23) & 0xFu) + 1u, dw = ((display >> 32) & 0xFFFu) + 1u, dh = ((display >> 44) & 0x7FFu) + 1u;
        screen.width = display ? float(dw / magh) : 512.0f;
        screen.height = display ? float(dh) : 512.0f;
        // The Z buffer's range from ZBUF's PSMZ: Z32, Z24, Z16 / Z16S.
        const uint32_t psmz = (g_s.ctx[0].zbuf >> 24) & 0xFu;
        screen.depthMax = psmz == 0u ? 4294967295.0f : psmz == 1u ? 16777215.0f : 65535.0f;
        std::optional<prof::Scope> timed(std::in_place, prof::Submit); // up to the frame's end (pacing is not counted)
        renderer.continueFrame(screen); // the frame's vertices, made since the last flip, kept
        uint64_t shown = 0;
        // The frame's draws in submission order through the 3D path: the GS's depth test (TEST, ZBUF) on GS Z, the
        // vertices window pixels already (an identity matrix).
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (auto it = g_frameDraws.find(fbp); it != g_frameDraws.end())
            for (const Batch &b : it->second)
            {
                if (b.first + b.count <= renderer.builtVertices()) // (the vertices since the last flip only)
                    renderer.drawRange(b.material, b.native ? b.matrix.data() : kIdentity, b.first, b.count, b.texture);
                shown += b.count / 3;
            }
        if (static const bool onShinoTestQuad = std::getenv("SHINO_TEST_QUAD") != nullptr; onShinoTestQuad) // presentation check: a red quad over the frame
        {
            Material m;
            m.textured = false;
            m.depthTest = false;
            m.depthWrite = false;
            m.alphaBlend = false;
            Vertex a, b, c2, d;
            a.x = 100; a.y = 100; b.x = 300; b.y = 100; c2.x = 300; c2.y = 300; d.x = 100; d.y = 300;
            a.rgba = b.rgba = c2.rgba = d.rgba = 0x800000FFu;
            const Vertex quad[6] = {a, b, c2, a, c2, d};
            renderer.drawOverlay(m, quad, 6, nullptr);
        }
        for (auto &[f, list] : g_frameDraws)
            if (f != fbp)
                for (const Batch &b : list)
                    g_offscreenPrims += b.count / 3;
        // The lists emptied, kept in the map (emit keeps a pointer to the last one); their vertices went with the frame.
        for (auto &[f, list] : g_frameDraws)
            list.clear();
        renderer.publishOverlay(screen);
        renderer.endFrame();
        if (static const bool onShinoTraceFlip = std::getenv("SHINO_TRACE_FLIP") != nullptr; onShinoTraceFlip && g_flips % 30 == 1)
            std::printf("[shino:flip] %llu fbp=%x shown=%llu triangles, screen %.0fx%.0f (DISPLAY %llx), xyoffset=%llx\n",
                        (unsigned long long)g_flips, fbp, (unsigned long long)shown, screen.width, screen.height,
                        (unsigned long long)display, (unsigned long long)off);
        if (static const bool onShinoTraceDraws = std::getenv("SHINO_TRACE_DRAWS") != nullptr; onShinoTraceDraws && g_flips % 60 == 1)
        {
            const auto draws = renderer.takeFrameDraws();
            std::printf("[shino:draws] flip %llu: lists=%u batches=%u vertices=%u enabled=%d alone=%d\n",
                        (unsigned long long)g_flips, draws.lists, draws.batches, draws.vertices, int(renderer.enabled()),
                        int(renderer.presentsAlone()));
        }
        timed.reset();
        if (prof::on() && g_flips % 250 == 0)
        {
            static uint64_t lookups = 0, hashes = 0, hashed = 0, decodes = 0;
            std::printf("[shino:profile] textures a flip: %.0f looked up, %.0f hashed (%.0f KB), %.1f decoded\n",
                        double(g_lookups - lookups) / 250, double(g_hashes - hashes) / 250,
                        double(g_hashedBytes - hashed) / 250 / 1024, double(g_decodes - decodes) / 250);
            lookups = g_lookups, hashes = g_hashes, hashed = g_hashedBytes, decodes = g_decodes;
            static uint64_t uploads = 0, bytes = 0, blocks = 0, changedBlocks = 0, prims = 0;
            uint64_t uploadBytes = 0;
            for (const uint64_t b : g_uploadBytes)
                uploadBytes += b;
            std::printf("[shino:profile] uploads a flip: %.0f, %.0f KB written, %.0f blocks "
                        "compared, %.0f changed; %.0f primitives\n",
                        double(g_uploads - uploads) / 250,
                        double(uploadBytes - bytes) / 250 / 1024, double(g_blocksCompared - blocks) / 250,
                        double(g_blocksChanged - changedBlocks) / 250, double(g_prims - prims) / 250);
            uploads = g_uploads, bytes = uploadBytes, blocks = g_blocksCompared;
            changedBlocks = g_blocksChanged, prims = g_prims;
        }
        prof::flip();
        // SHINO_CHECKSUM: the running checksums every 500 flips (SHINO_CHECKSUM_EVERY: every n): what the GS translator
        // drew, and the native batches
        static const uint64_t checksumEvery = std::getenv("SHINO_CHECKSUM_EVERY") ? std::max<uint64_t>(1, std::strtoull(std::getenv("SHINO_CHECKSUM_EVERY"), nullptr, 10)) : 500;
        if (static const bool onShinoChecksum = std::getenv("SHINO_CHECKSUM") != nullptr; onShinoChecksum && g_flips % checksumEvery == 0)
            std::printf("[shino:checksum] flip %llu %016llx native %016llx\n", (unsigned long long)g_flips, (unsigned long long)g_checksum,
                        (unsigned long long)g_nativeChecksum);
        bozo::port::frameEnd(rt, ctx);
    }

    uint64_t flips() { return g_flips; }

    // A frame the port starts itself (a movie's picture, runtime/shino_render.cpp): the renderer's beginFrame drops the
    // vertices made since the last flip, so the draws over them go too.
    void dropDraws()
    {
        for (auto &[f, list] : g_frameDraws)
            list.clear();
    }

    void reportState()
    {
        std::printf("[shino:upload] written by block %llu, by row %llu, by pixel %llu\n",(unsigned long long)g_uploadsByBlock,
                    (unsigned long long)g_uploadsByRow, (unsigned long long)g_uploadsByPixel);
        for (uint32_t psm = 0; psm < 64; ++psm)
            if (g_uploadCount[psm])
                std::printf("[shino:upload] psm %02x: %llu uploads, %llu bytes\n", psm, (unsigned long long)g_uploadCount[psm],
                            (unsigned long long)g_uploadBytes[psm]);
        std::printf("[shino:gs] prims=%llu offscreen_triangles=%llu flips=%llu uploads=%llu (blocks changed %llu of %llu) texture_decodes=%llu\n",
                    (unsigned long long)g_prims, (unsigned long long)g_offscreenPrims, (unsigned long long)g_flips,
                    (unsigned long long)g_uploads, (unsigned long long)g_blocksChanged, (unsigned long long)g_blocksCompared, (unsigned long long)g_decodes);
    }
} // namespace shino::gs
