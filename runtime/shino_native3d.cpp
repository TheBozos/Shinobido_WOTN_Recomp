// Native 3D, stage 1 (docs/NATIVE_3D.md): the main renderer's batches drawn from VU1 memory by BozoEngine's renderer.
//
// The main renderer (microcode set 0x539c40) draws an object in batches: `MSCAL 0` (the setup), then `MSCAL 0x3968`
// for each next batch (back to the batch's prelude at pair 51). The prelude reads the batch's buffer and count (`xtop
// vi1`, `xitop vi9`: four quadwords a vertex, position, UV, colour), the lighting makes each vertex's colour, then `jr
// vi12` enters the variant the object's flags chose. The main variant (pair 1023, 95% of the set's batches) transforms
// the strip by the object's model-view-projection (1008-1011), sets each triangle's ADC from its facing (the cull mode
// at 1023.z), and builds the GS packet: STQ from the UV, RGBA from the colour, XYZF with the fog.
//
// Here the program stops at the variant's entry (vu::Unit::stopAt) and the batch is drawn as triangles in the object's
// space with a matrix to GS screen space, the same transform, colours, fog and culling, without the GS packet.
#include "shino_native3d.h"

#include "bozo/render/renderer.h"
#include "shino_gs.h"
#include "shino_vu.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace shino::native3d
{
    namespace
    {
        constexpr uint32_t kMainPacket = 0x539c30u; // the main renderer's microcode (its MPG packet in the executable)
        constexpr uint32_t kVariant = 1023u;        // the main variant's entry pair
        constexpr uint32_t kVertexProgram = 0x72du; // MSCAL 0x3968: the next batches' entry

        struct Stats
        {
            uint64_t batches = 0, vertices = 0, triangles = 0, culled = 0, rejected = 0, clipped = 0;
            uint64_t compared = 0, flagDiffers = 0, positionDiffers = 0;
            double positionError = 0.0;
        } g_stats;

        // Verify: the last batch made natively (clip positions as the variant stores them, the ADC flags).
        struct Last
        {
            uint32_t base = 0, count = 0;
            std::vector<std::array<float, 4>> clip;
            std::vector<uint16_t> adc;
        } g_last;

        const vu::Image *mainImage()
        {
            static const vu::Image *image = [] {
                for (uint32_t id = 0; id < vu::kImageCount; ++id)
                    if (vu::kImages[id].source == kMainPacket)
                        return &vu::kImages[id];
                return static_cast<const vu::Image *>(nullptr);
            }();
            return image;
        }

        float lane(const vu::Unit &v, uint32_t qw, int i) { return reinterpret_cast<const float *>(v.data + 16u * (qw & v.qwMask))[i]; }
        uint32_t word(const vu::Unit &v, uint32_t qw, int i) { return reinterpret_cast<const uint32_t *>(v.data + 16u * (qw & v.qwMask))[i]; }
    }

    Mode mode()
    {
        static const Mode m = [] {
            const char *e = std::getenv("SHINO_NATIVE_3D");
            if (!e || !*e || *e == '0')
                return Mode::Off;
            return std::strcmp(e, "verify") == 0 ? Mode::Verify : std::strcmp(e, "skip") == 0 ? Mode::Skip : Mode::On;
        }();
        return m;
    }

    uint32_t takeOverAt(const vu::Unit &v)
    {
        const vu::Image *im = mainImage();
        if (!im || v.vu0)
            return 0;
        for (const uint32_t p : {0u, kVariant, kVertexProgram})
        {
            if (p < im->start || p >= im->start + im->count)
                return 0;
            const uint32_t k = p - im->start;
            if (v.micro[2 * p] != im->code[2 * k] || v.micro[2 * p + 1] != im->code[2 * k + 1])
                return 0;
        }
        return kVariant * 8u;
    }

    void batch(vu::Unit &v, bool submit)
    {
        const uint32_t base = v.vi[1], count = v.vi[9];
        ++g_stats.batches;
        g_stats.vertices += count;
        // The camera rows as the setup leaves them in vf28-vf31: x and y less half of w, times the screen scale (1015);
        // rows 0-2 without z, row 3's z the depth scale. The variant takes z = w for its facing test.
        float row[4][4];
        const float sx = lane(v, 1015, 0), sy = lane(v, 1015, 1), sz = lane(v, 1015, 2);
        for (uint32_t c = 0; c < 4; ++c)
        {
            const float x = lane(v, 1008 + c, 0), y = lane(v, 1008 + c, 1), w = lane(v, 1008 + c, 3);
            const float half = w * 0.5f;
            row[c][0] = (x - half) * sx;
            row[c][1] = (y - half) * sy;
            row[c][2] = c == 3 ? sz : 0.0f;
            row[c][3] = w;
        }
        // The GS screen point: 1016 + 1017 + the row sums over w (X, Y), 1016.z + the depth over w (Z).
        const float cx = lane(v, 1016, 0) + lane(v, 1017, 0), cy = lane(v, 1016, 1) + lane(v, 1017, 1), cz = lane(v, 1016, 2);
        float matrix[16];
        for (uint32_t c = 0; c < 4; ++c)
        {
            matrix[4 * c + 0] = row[c][0] + cx * row[c][3];
            matrix[4 * c + 1] = row[c][1] + cy * row[c][3];
            matrix[4 * c + 2] = row[c][2] + cz * row[c][3];
            matrix[4 * c + 3] = row[c][3];
        }
        // Fog (1014.zw): F = scale x (w - far), from 0 to 255.
        const float fogScale = lane(v, 1014, 2), fogFar = lane(v, 1014, 3);
        // The facing test's cull mode (1023.z): ADC = (mode + 0x7fe0 | 0x7fe0) + 32 if the determinant is negative, plus
        // 0x8000 on odd triangles (a strip's winding), its bit 15.
        const uint16_t mode7 = static_cast<uint16_t>((static_cast<uint16_t>(word(v, 1023, 2)) + 0x7FE0u) | 0x7FE0u);

        // The first pass covers the vertices in pairs (to vi11 = vi1 + (4 count + 4 & 0x7ff8)): one past the batch's
        // own when the count is odd.
        const uint32_t passed = std::max<uint32_t>(((4u * count + 4u) & 0x7FF8u) / 4u, 2u);
        std::vector<std::array<float, 4>> clip(passed);
        std::vector<uint16_t> adc(passed, 0);
        std::vector<bozo::render::Vertex> corner(count);
        for (uint32_t k = 0; k < passed; ++k)
        {
            const uint32_t q = base + 4u * k;
            const float px = lane(v, q, 0), py = lane(v, q, 1), pz = lane(v, q, 2);
            // As the variant does it: ACC = row 3, then + row 0 x, + row 1 y, then + row 2 z.
            std::array<float, 4> r;
            for (int i = 0; i < 4; ++i)
            {
                const float r3 = i == 2 ? row[3][3] : row[3][i]; // vf24: row 3 with z = w
                const float r0 = i == 2 ? row[0][3] : row[0][i], r1 = i == 2 ? row[1][3] : row[1][i], r2 = i == 2 ? row[2][3] : row[2][i];
                float a = r3;
                a = a + r0 * px;
                a = a + r1 * py;
                r[i] = a + r2 * pz;
            }
            clip[k] = r;
            if (k >= count)
                continue;
            bozo::render::Vertex &out = corner[k];
            out.x = px;
            out.y = py;
            out.z = pz;
            out.u = lane(v, q + 1, 0);
            out.v = lane(v, q + 1, 1);
            uint32_t rgba = 0;
            for (int i = 0; i < 4; ++i)
            {
                const float c = lane(v, q + 2, i);
                const int32_t n = std::isnan(c) ? 0 : c >= 2147483520.0f ? INT32_MAX : c <= -2147483648.0f ? INT32_MIN : static_cast<int32_t>(c);
                rgba |= (static_cast<uint32_t>(n) & 0xFFu) << (8 * i);
            }
            out.rgba = rgba;
            float f = fogScale * (r[3] - fogFar);
            f = std::min(std::max(f, 0.0f), 255.0f);
            out.fog = static_cast<float>(static_cast<int32_t>(f)) / 255.0f;
            out.q = 1.0f;
        }
        auto cross = [](const std::array<float, 4> &a, const std::array<float, 4> &b) {
            // OPMULA / OPMSUB with fs = a, ft = b: a.yzx b.zxy - b.yzx a.zxy, on (x, y, w)
            const float ax = a[0], ay = a[1], aw = a[2], bx = b[0], by = b[1], bw = b[2];
            return std::array<float, 3>{ay * bw - by * aw, aw * bx - bw * ax, ax * by - bx * ay};
        };
        auto dot = [](const std::array<float, 3> &c, const std::array<float, 4> &d) {
            const float px = c[0] * d[0], py = c[1] * d[1], pz = c[2] * d[2];
            return (pz + px) + py;
        };
        std::array<float, 3> last{};
        for (uint32_t k = 2; k < passed; ++k)
        {
            float d;
            if ((k & 1u) == 0)
            {
                last = cross(clip[k], clip[k - 1]);
                d = dot(last, clip[k - 2]);
            }
            else
                d = dot(last, clip[k]);
            const uint16_t sign = std::signbit(d) ? 32u : 0u;
            const uint16_t sum = static_cast<uint16_t>(mode7 + sign + ((k & 1u) ? 0x8000u : 0u));
            adc[k] = sum & 0x8000u;
        }
        // The clip flags as the variant's CLIP makes them (a vertex outside the guard band, 1021.y / 1022.y of a pixel
        // either side, or nearer than w 1 / farther than w 2500 by 1021.z / 1022.z, against |w|): a triangle all of
        // whose corners are outside one plane is rejected, as the variant's clipper does; the others are drawn whole and
        // the GPU clips them (a triangle reaching behind the camera too: the renderer's w).
        const float guardX = lane(v, 1021, 1), guardY = lane(v, 1022, 1), depth0 = lane(v, 1021, 2), depth1 = lane(v, 1022, 2);
        std::vector<uint8_t> outside(count);
        for (uint32_t k = 0; k < count; ++k)
        {
            const float x = clip[k][0] * guardX, y = clip[k][1] * guardY, w = clip[k][3], aw = std::fabs(w);
            const float z = depth0 + depth1 * w;
            outside[k] = static_cast<uint8_t>((x > aw ? 1u : 0u) | (x < -aw ? 2u : 0u) | (y > aw ? 4u : 0u) | (y < -aw ? 8u : 0u) |
                                              (z > aw ? 16u : 0u) | (z < -aw ? 32u : 0u));
        }
        std::vector<bozo::render::Vertex> triangles;
        triangles.reserve(count * 3);
        for (uint32_t k = 2; k < count; ++k)
        {
            if (adc[k])
            {
                ++g_stats.culled;
                continue;
            }
            // As the clipper orders it: all three nearer than the near plane (or farther than the far one) rejected;
            // a guard-band plane only decides with every corner in front (behind the camera a corner's x and y are
            // mirrored: what is left after the near cut may well be on the picture).
            const uint8_t all = outside[k] & outside[k - 1] & outside[k - 2], any = outside[k] | outside[k - 1] | outside[k - 2];
            if ((all & 48u) || ((all & 15u) && !(any & 32u)))
            {
                ++g_stats.rejected;
                continue;
            }
            // Across the near or the far plane (the depth flags): cut there as the variant's clipper does, the corners'
            // attributes interpolated in clip space (linear in the object's space); the guard band's planes are left
            // to the GPU (what lies past them is off the picture).
            if (!((outside[k] | outside[k - 1] | outside[k - 2]) & 48u))
            {
                triangles.push_back(corner[k - 2]);
                triangles.push_back(corner[k - 1]);
                triangles.push_back(corner[k]);
                continue;
            }
            ++g_stats.clipped;
            struct Corner
            {
                bozo::render::Vertex v;
                float w;
            };
            Corner poly[9] = {{corner[k - 2], clip[k - 2][3]}, {corner[k - 1], clip[k - 1][3]}, {corner[k], clip[k][3]}};
            uint32_t n = 3;
            // inside: near, z >= -|w| (w >= 1); far, z <= |w| (w <= 2500), z = depth0 + depth1 w
            auto distance = [&](float w, int plane) {
                const float z = depth0 + depth1 * w, aw = std::fabs(w);
                return plane == 0 ? z + aw : aw - z;
            };
            for (int plane = 0; plane < 2 && n >= 3; ++plane)
            {
                Corner out[9];
                uint32_t m = 0;
                for (uint32_t i = 0; i < n; ++i)
                {
                    const Corner &a = poly[i], &b = poly[(i + 1) % n];
                    const float da = distance(a.w, plane), db = distance(b.w, plane);
                    if (da >= 0.0f)
                        out[m++] = a;
                    if ((da >= 0.0f) != (db >= 0.0f) && m < 9)
                    {
                        const float t = da / (da - db);
                        Corner c;
                        auto mix = [t](float x, float y) { return x + (y - x) * t; };
                        c.w = mix(a.w, b.w);
                        c.v.x = mix(a.v.x, b.v.x);
                        c.v.y = mix(a.v.y, b.v.y);
                        c.v.z = mix(a.v.z, b.v.z);
                        c.v.u = mix(a.v.u, b.v.u);
                        c.v.v = mix(a.v.v, b.v.v);
                        c.v.fog = mix(a.v.fog, b.v.fog);
                        c.v.q = 1.0f;
                        uint32_t rgba = 0;
                        for (int ch = 0; ch < 4; ++ch)
                        {
                            const float ca = float((a.v.rgba >> (8 * ch)) & 0xFFu), cb = float((b.v.rgba >> (8 * ch)) & 0xFFu);
                            rgba |= (static_cast<uint32_t>(mix(ca, cb) + 0.5f) & 0xFFu) << (8 * ch);
                        }
                        c.v.rgba = rgba;
                        out[m++] = c;
                    }
                }
                std::copy(out, out + m, poly);
                n = m;
            }
            for (uint32_t i = 1; i + 1 < n; ++i)
            {
                triangles.push_back(poly[0].v);
                triangles.push_back(poly[i].v);
                triangles.push_back(poly[i + 1].v);
            }
        }
        g_stats.triangles += triangles.size() / 3;
        // The buffer as the variant's first pass leaves it: each vertex's clip position (x, y, w, w) in slot 0, its ADC
        // in slot 1's w. Programs after it read them: the light passes over the same vertices.
        if (mode() == Mode::Skip) // (otherwise the VU's own variant writes them)
            for (uint32_t k = 0; k < passed; ++k)
            {
                const uint32_t q = base + 4u * k;
                std::memcpy(v.data + 16u * (q & v.qwMask), clip[k].data(), 16);
                const uint32_t flag = adc[k];
                std::memcpy(v.data + 16u * ((q + 1) & v.qwMask) + 12, &flag, 4);
            }
        if (mode() == Mode::Verify)
        {
            g_last.base = base;
            g_last.count = count;
            g_last.clip = std::move(clip);
            g_last.adc = std::move(adc);
        }
        if (!submit || triangles.empty())
            return;
        // The vertex GIF tag (1018): PRIM in its word 1 (bits 15-25) when PRE is set (bit 14).
        const uint32_t tag1 = word(v, 1018, 1);
        const uint64_t prim = (tag1 >> 14) & 1u ? (tag1 >> 15) & 0x7FFu : ~0ull;
        gs::nativeTriangles(prim, matrix, triangles.data(), static_cast<uint32_t>(triangles.size()));
    }

    void compare(const vu::Unit &v)
    {
        const Last &l = g_last;
        // The variant's first pass covers the vertices in pairs: what it wrote for the batch's own.
        for (uint32_t k = 0; k < l.clip.size(); ++k)
        {
            const uint32_t q = l.base + 4u * k;
            const uint16_t vuAdc = static_cast<uint16_t>(word(v, q + 1, 3));
            if (k >= 2 && (vuAdc & 0x8000u) != l.adc[k] && g_stats.flagDiffers++ < 20)
                std::printf("[shino:native3d] batch %llu vertex %u: ADC %x, the VU's %x%c", (unsigned long long)g_stats.batches, k,
                            l.adc[k], vuAdc, 10);
            for (int i = 0; i < 4; ++i)
            {
                const float a = l.clip[k][i], b = lane(v, q, i);
                const double e = std::fabs(double(a) - double(b)) / std::max(1.0, std::fabs(double(b)));
                g_stats.positionError = std::max(g_stats.positionError, e);
                if (e > 1e-5 && g_stats.positionDiffers++ < 20)
                    std::printf("[shino:native3d] batch %llu vertex %u lane %d: %g, the VU's %g%c", (unsigned long long)g_stats.batches,
                                k, i, a, b, 10);
            }
        }
        ++g_stats.compared;
    }

    void report()
    {
        if (mode() == Mode::Off)
            return;
        const Stats &s = g_stats;
        std::printf("[shino:native3d] batches %llu, vertices %llu, triangles %llu drawn, %llu culled, %llu outside, %llu cut at near / far",
                    (unsigned long long)s.batches, (unsigned long long)s.vertices, (unsigned long long)s.triangles, (unsigned long long)s.culled,
                    (unsigned long long)s.rejected, (unsigned long long)s.clipped);
        if (mode() == Mode::Verify)
            std::printf("; compared %llu: ADC differed %llu, positions %llu (largest relative error %.3g)", (unsigned long long)s.compared,
                        (unsigned long long)s.flagDiffers, (unsigned long long)s.positionDiffers, s.positionError);
        std::printf("%c", 10);
    }
}
