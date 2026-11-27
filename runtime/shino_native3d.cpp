// Native 3D (docs/NATIVE_3D.md): the 3D renderers' batches drawn from VU1 memory by BozoEngine's renderer; the main
// renderer's objects made here whole (SHINO_NATIVE_OBJECTS), no VU code run for them.
//
// The main renderer (microcode set 0x539c40) draws an object in batches: `MSCAL 0` (the setup), then `MSCNT` for each
// next batch (at 0x3968, where the variant ended: back to the batch's prelude at pair 51). The prelude reads the batch's buffer and count (`xtop
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
#include "shino_port.h"
#include "shino_vu.h"

#include <algorithm>
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
        // The variants drawn natively: the renderer's microcode (its MPG packet in the executable), the variant's entry
        // pair, the next batches' program (MSCAL), and what the variant does with a strip.
        struct Kind
        {
            uint32_t packet, variant, program;
            bool clipper;        // the main renderer's: ADC by the clip-space determinant, the clipper, the first pass written back
            bool replay = false; // the variant run (its kicks held), then its packets replayed: strips drawn natively
            bool list = false;   // a clipper's triangle list (three vertices a triangle) rather than strips
        };
        constexpr Kind kKinds[] = {
            {0x539c30u, 1023u, 0x72du, true},  // the main renderer (next batches: MSCNT at 0x3968), flags 0xa / 0xb
            {0x539c30u, 554u, 0x72du, false},  // the main renderer, its unclipped variant (the characters' 644, the same code)
            {0x539c30u, 1438u, 0x72du, true, false, true}, // the main renderer's clipped triangle lists (flags 0xe / 0xf)
            {0x579a00u, 644u, 0x787u, false},  // the characters' (MSCNT at 0x3c38), flags 0x9: no clipping, screen-space culling
            {0x579a00u, 1113u, 0x787u, true},  // the characters' clipped variant (flags 0xb): the main 1023's code, smaller buffers
            // Sets 0x5cc000 and 0x5e7cd0: the main renderer's and the characters' code (their buffers elsewhere), the
            // variants 21 pairs on, the setup's UVs transformed
            {0x5cc000u, 1044u, 0x742u, true},              // = 1023
            {0x5cc000u, 575u, 0x742u, false},              // = 554
            {0x5cc000u, 1459u, 0x742u, true, false, true}, // = 1438
            {0x5e7cd0u, 665u, 0x79cu, false},              // = 644
            {0x5e7cd0u, 1134u, 0x79cu, true},              // = 1113
            // Replayed: the variant's own GS packets, each strip drawn natively from the batch's positions (four quadwords a
            // vertex at vi1) where its vertices are the batch's own on the screen, the other packets as they are.
            {0x599510u, 657u, 0x74du, false, true}, // set 0x599510 (MSCAL 0x3a68): each strip twice, a second texture
            {0x59d290u, 520u, 0x5f1u, false, true},  // set 0x59d290 (MSCAL 0x2f88)
        };
        constexpr uint32_t kKindCount = sizeof(kKinds) / sizeof(kKinds[0]);
        uint32_t g_kind = 0;   // the kind of the batch drawn last
        // A replayed variant's batch: its vertices in the object's space and on the GS screen, its matrix, and the GIF
        // packets the variant sent (each a tag and its data, EOP set), in order.
        struct Replay
        {
            bool active = false;
            uint32_t count = 0;
            std::vector<std::array<float, 3>> position;
            std::vector<std::array<float, 2>> screen;
            float matrix[16] = {};
            std::vector<std::vector<uint8_t>> packets;
        } g_replay;
        struct ReplayStats
        {
            uint64_t batches = 0, strips = 0, native = 0, other = 0, misaligned = 0;
        } g_replayStats;
        uint32_t g_packet = 0; // the renderer takeOverAt found last

        // ---- The main renderer's setup, natively (verify only, for now) -------------------------------------------
        // The program's VU1 data memory as it starts (the batch at TOP, ITOP vertices, its morph targets after it).
        std::vector<uint8_t> g_raw;
        uint32_t g_rawTop = 0, g_rawCount = 0;
        bool g_rawValid = false;
        struct SetupStats
        {
            uint64_t batches = 0, vertices = 0, positionDiffers = 0, uvDiffers = 0, normalDiffers = 0, colourDiffers = 0, unsupported = 0;
        };
        std::array<SetupStats, 4> g_setup; // by kObjectRenderers

        // What the setup leaves in the batch (four quadwords a vertex: position, UV, colour, normal), from the raw batch:
        // the colours and normals converted (itof0, the normals over 128), the positions and normals blended towards the
        // morph targets after the batch when 1020.w (the weight) is not 0, then the light list from 980 (type 2 a
        // constant colour, 128 a point light) and the colours clamped to 255; times the material colour (1019) in the
        // end. False for a light type not read yet. As the VU's loops do it: most go four vertices at a time, to the
        // batch rounded up to four (vi11) and over the stale vertices past it; the morph of a lit object and the point
        // lights to the batch's own end (vi10). The outputs cover the rounded batch.
        //
        // The characters' (skin): the batch comes in at five quadwords a vertex (position, UV, colour, normal, and four
        // bone weights whose low 16 bits are the bones' indices) and leaves at four, in place: each vertex's matrix the
        // four bones' rows (696-699 past the index) blended by the weights, the position (morphed first, as above, with
        // the targets after the five-quadword batch) and the normal transformed by it; unlit, the colour times the
        // material and slot 3 left alone. The lights after are the main renderer's code, word for word.
        struct SetupOut
        {
            std::vector<std::array<float, 4>> position, uv, colour, normal;
        };
        bool nativeSetupCore(const uint8_t *mem, uint32_t base, uint32_t count, SetupOut &out, bool skin)
        {
            auto F = [&](uint32_t qw, int i) {
                float x;
                std::memcpy(&x, mem + 16u * (qw & 0x3FFu) + 4u * static_cast<uint32_t>(i), 4);
                return x;
            };
            auto I = [&](uint32_t qw, int i) {
                int32_t x;
                std::memcpy(&x, mem + 16u * (qw & 0x3FFu) + 4u * static_cast<uint32_t>(i), 4);
                return x;
            };
            using V4 = std::array<float, 4>;
            const auto C = vu::clampF; // each result as the VU's arithmetic leaves it
            const uint32_t all = ((4u * count + 12u) & 0x7FF0u) / 4u; // vi11
            out.position.assign(all, {});
            out.uv.assign(all, {});
            out.colour.assign(all, {});
            out.normal.assign(all, {});
            const float weight = F(1020, 3);
            const bool morph = weight + 0.0f != 0.0f;
            const float w1 = 1.0f - weight, w2 = 0.0f + weight; // vf27.w, vf27.x
            const float inv128 = 0.0078125f;
            const V4 material = {F(1019, 0), F(1019, 1), F(1019, 2), F(1019, 3)};
            const bool lit = (I(980, 3) & 0xFFFF) != 0;
            const uint32_t converted = lit && morph ? count : all;
            auto F4 = [&](uint32_t qw) { return V4{F(qw, 0), F(qw, 1), F(qw, 2), F(qw, 3)}; };
            if (skin)
            {
                for (uint32_t k = count; k < all; ++k) // past the batch: as they are
                {
                    const uint32_t q = base + 4u * k;
                    out.position[k] = F4(q);
                    out.uv[k] = F4(q + 1);
                    out.colour[k] = F4(q + 2);
                    out.normal[k] = F4(q + 3);
                }
                auto plus0 = [&](V4 x) { // ADDx by vf0.x: a copy, clamped (and -0 made +0)
                    for (float &f : x)
                        f = C(f + 0.0f);
                    return x;
                };
                for (uint32_t k = 0; k < count; ++k)
                {
                    const uint32_t in = base + 5u * k;
                    V4 pos = F4(in), uv = F4(in + 1);
                    if (k > 0) // the first vertex's straight from memory, the others' copied by ADDx
                    {
                        uv = plus0(uv);
                        if (!morph)
                            pos = plus0(pos);
                    }
                    if (morph)
                        for (int i = 0; i < 4; ++i)
                            pos[i] = C(C(pos[i] * w1) + F(base + 5u * count + k, i) * w2);
                    // the blended rows: 699 (the translation), 696, 697, 698
                    uint32_t bone[4];
                    float wt[4];
                    for (int j = 0; j < 4; ++j)
                    {
                        bone[j] = static_cast<uint32_t>(I(in + 4, j)) & 0xFFFFu;
                        wt[j] = F(in + 4, j);
                    }
                    V4 R[4]; // 696, 697, 698, 699
                    for (int r = 0; r < 4; ++r)
                        for (int i = 0; i < 4; ++i)
                        {
                            float acc = C(F(696u + r + bone[0], i) * wt[0]);
                            acc = C(acc + F(696u + r + bone[1], i) * wt[1]);
                            acc = C(acc + F(696u + r + bone[2], i) * wt[2]);
                            R[r][i] = C(acc + F(696u + r + bone[3], i) * wt[3]);
                        }
                    V4 &P = out.position[k];
                    for (int i = 0; i < 4; ++i)
                        P[i] = C(C(C(C(R[3][i] * 1.0f) + R[0][i] * pos[0]) + R[1][i] * pos[1]) + R[2][i] * pos[2]);
                    out.uv[k] = uv;
                    for (int i = 0; i < 4; ++i)
                        out.colour[k][i] = static_cast<float>(I(in + 2, i));
                    if (lit)
                    {
                        V4 n;
                        for (int i = 0; i < 4; ++i)
                            n[i] = morph ? C(C(static_cast<float>(I(in + 3, i)) * C(w1 * inv128)) +
                                             static_cast<float>(I(base + 6u * count + k, i)) * C(w2 * inv128))
                                         : C(static_cast<float>(I(in + 3, i)) * inv128);
                        for (int i = 0; i < 4; ++i)
                            out.normal[k][i] = C(C(C(R[0][i] * n[0]) + R[1][i] * n[1]) + R[2][i] * n[2]);
                    }
                    else
                    {
                        for (int i = 0; i < 4; ++i)
                            out.colour[k][i] = C(out.colour[k][i] * material[i]);
                        out.normal[k] = F4(base + 4u * k + 3); // slot 3 not written
                    }
                }
                if (!lit)
                    return true; // (unlit: no pass over the rounded batch)
            }
            for (uint32_t k = 0; !skin && k < all; ++k)
            {
                const uint32_t q = base + 4u * k;
                out.uv[k] = F4(q + 1);
                V4 p = {F(q, 0), F(q, 1), F(q, 2), F(q, 3)};
                if (k >= converted) // left as they are (the lit morph's loop ends at the batch's end)
                {
                    out.position[k] = p;
                    out.colour[k] = {F(q + 2, 0), F(q + 2, 1), F(q + 2, 2), F(q + 2, 3)};
                    out.normal[k] = {F(q + 3, 0), F(q + 3, 1), F(q + 3, 2), F(q + 3, 3)};
                    continue;
                }
                if (morph) // ACC = p (1 - a), then + target a
                {
                    const uint32_t t = base + 4u * count + k;
                    for (int i = 0; i < 4; ++i)
                        p[i] = C(C(p[i] * w1) + F(t, i) * w2);
                }
                out.position[k] = p;
                for (int i = 0; i < 4; ++i)
                    out.colour[k][i] = static_cast<float>(I(q + 2, i));
                if (lit)
                {
                    V4 n;
                    if (morph)
                    {
                        const uint32_t t = base + 4u * count + count + k;
                        for (int i = 0; i < 4; ++i)
                            n[i] = C(C(static_cast<float>(I(q + 3, i)) * C(w1 * inv128)) + static_cast<float>(I(t, i)) * C(w2 * inv128));
                    }
                    else
                        for (int i = 0; i < 4; ++i)
                            n[i] = C(static_cast<float>(I(q + 3, i)) * inv128);
                    out.normal[k] = n;
                }
                else
                    for (int i = 0; i < 4; ++i)
                        out.normal[k][i] = F(q + 3, i);
            }
            if (!lit)
            {
                for (V4 &c : out.colour)
                    for (int i = 0; i < 4; ++i)
                        c[i] = C(c[i] * material[i]);
                return true;
            }
            // the world rows (976-979): the lights brought into the object's space
            V4 rows[4];
            for (int r = 0; r < 4; ++r)
                rows[r] = {F(976 + r, 0), F(976 + r, 1), F(976 + r, 2), F(976 + r, 3)};
            const V4 c1020 = {F(1020, 0), F(1020, 1), F(1020, 2), F(1020, 3)};
            for (uint32_t at = 980, guard = 0; guard < 16; ++guard)
            {
                const int type = I(at, 3) & 0xFFFF;
                V4 rec0 = {F(at, 0), F(at, 1), F(at, 2), F(at, 3)}, rec1 = {F(at + 1, 0), F(at + 1, 1), F(at + 1, 2), F(at + 1, 3)};
                // ACC = row0 x + row1 y + row2 z, + row3 (the position)
                V4 lp;
                for (int i = 0; i < 3; ++i)
                {
                    float acc = C(rows[0][i] * rec1[0]);
                    acc = C(acc + rows[1][i] * rec1[1]);
                    acc = C(acc + rows[2][i] * rec1[2]);
                    lp[i] = C(acc + rows[3][i] * 1.0f);
                }
                if (type == 2)
                {
                    for (int i = 0; i < 3; ++i)
                        rec0[i] = C(rec0[i] * c1020[0]);
                    for (V4 &c : out.colour)
                        for (int i = 0; i < 3; ++i)
                            c[i] = C(c[i] + rec0[i]);
                    at += 1;
                    continue;
                }
                if (type == 128)
                {
                    // A point light (VU 0x6b8): its colour times nd (the normal against the way to the light), times
                    // 1/distance - 1/radius (the radius rec1.w times 976.w), added where positive. The 1/radius is the
                    // DIV the RSQRT after it waits for (shino_vu.h divideV): without that wait, a stale Q.
                    const float radius = rec1[3] * rows[0][3];
                    for (int i = 0; i < 3; ++i)
                        rec0[i] = rec0[i] * c1020[2];
                    const float q0 = 1.0f / radius, minus = 0.0f - q0;
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        const V4 &p = out.position[k], &n = out.normal[k];
                        const float dx = lp[0] - p[0], dy = lp[1] - p[1], dz = lp[2] - p[2];
                        // ACC.x = d.x d.x + d.z d.z (addaz), then + 1 d.y d.y (maddy by vf25 = 1)
                        const float dist2 = (dx * dx + dz * dz) + 1.0f * (dy * dy);
                        float nd = (dx * n[0] + dz * n[2]) + 1.0f * (dy * n[1]);
                        nd = std::max(nd, 0.0f);
                        const float q1 = 1.0f / std::sqrt(dist2);
                        V4 &c = out.colour[k];
                        for (int i = 0; i < 3; ++i)
                        {
                            const float lc = rec0[i] * nd;
                            float acc = c[i] + 0.0f;
                            acc = acc + lc * minus;
                            const float r = acc + lc * q1;
                            c[i] = std::max(r, c[i]);
                        }
                    }
                    at += 2;
                    continue;
                }
                if (type == 129)
                {
                    // A spotlight (VU 0x830; the characters' 0x990): the point light's falloff, added only where the
                    // vertex is inside the cone: the cosine (the way to the light against the spot's direction, rec2's
                    // xyz turned into the object's space and scaled by 979.w) against the cutoff (rec2.w times 979.w),
                    // by the sign flag of cutoff - cosine (FMAND of the MAC flags 4 pairs on). Each step clamped.
                    const V4 rec2 = {F(at + 2, 0), F(at + 2, 1), F(at + 2, 2), F(at + 2, 3)};
                    V4 dir;
                    for (int i = 0; i < 3; ++i)
                        dir[i] = C(C(C(C(rows[0][i] * rec2[0]) + rows[1][i] * rec2[1]) + rows[2][i] * rec2[2]) * rows[3][3]);
                    const float cutoff = C(rec2[3] * rows[3][3]);
                    const float radius = C(rec1[3] * rows[0][3]);
                    V4 colour;
                    for (int i = 0; i < 3; ++i)
                        colour[i] = C(rec0[i] * c1020[2]);
                    const float minus = C(0.0f - vu::divideResult(0, 1.0f, radius));
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        const V4 &P = out.position[k], &n = out.normal[k];
                        float d[3], dd[3], dn[3], ds[3];
                        for (int i = 0; i < 3; ++i)
                        {
                            d[i] = C(lp[i] - P[i]);
                            dd[i] = C(d[i] * d[i]);
                            dn[i] = C(d[i] * n[i]);
                            ds[i] = C(d[i] * dir[i]);
                        }
                        const float dist2 = C(C(dd[0] + dd[2]) + 1.0f * dd[1]);
                        const float nd = std::max(C(C(dn[0] + dn[2]) + 1.0f * dn[1]), 0.0f);
                        const float q = vu::divideResult(2, 1.0f, dist2);
                        const float cosine = C(C(C(ds[0] + ds[2]) + 1.0f * ds[1]) * q);
                        if (std::signbit(C(cutoff - cosine)))
                            continue; // outside the cone
                        V4 &c = out.colour[k];
                        for (int i = 0; i < 3; ++i)
                        {
                            const float lc = C(colour[i] * nd);
                            const float add = std::max(C(C(lc * minus) + lc * q), 0.0f);
                            c[i] = C(c[i] + add);
                        }
                    }
                    at += 3;
                    continue;
                }
                if (type == 0)
                    break;
                return false; // a light type not read yet
            }
            for (V4 &c : out.colour) // MINII then MUL, each clamped (a NaN 0)
                for (int i = 0; i < 4; ++i)
                    c[i] = C(C(std::min(c[i], 255.0f)) * material[i]);
            return true;
        }

        // Sets 0x5cc000 and 0x5e7cd0 (the main renderer's and the characters' code, their variants 21 pairs on) end their
        // setup with a texture transform: each UV (the batch rounded up to four) becomes 958 + 956 u + 957 v (x and y).
        bool nativeSetup(const uint8_t *mem, uint32_t base, uint32_t count, SetupOut &out, bool skin, bool uvTransform)
        {
            if (!nativeSetupCore(mem, base, count, out, skin))
                return false;
            if (!uvTransform)
                return true;
            auto F = [&](uint32_t qw, int i) {
                float x;
                std::memcpy(&x, mem + 16u * (qw & 0x3FFu) + 4u * static_cast<uint32_t>(i), 4);
                return x;
            };
            const auto C = vu::clampF;
            for (std::array<float, 4> &uv : out.uv)
            {
                const float u = uv[0], v = uv[1];
                for (int i = 0; i < 2; ++i)
                    uv[i] = C(C(C(F(958, i) * 1.0f) + F(956, i) * u) + F(957, i) * v);
            }
            return true;
        }

        struct Stats
        {
            uint64_t batches = 0, vertices = 0, triangles = 0, culled = 0, rejected = 0, clipped = 0;
            uint64_t compared = 0, flagDiffers = 0, positionDiffers = 0;
            double positionError = 0.0;
        } g_stats;

        // A GS vertex as a variant's packet holds it (PACKED ST, RGBAQ, XYZF2).
        struct Out
        {
            uint32_t s = 0, t = 0, q = 0, rgba = 0, x = 0, y = 0, z = 0, f = 0, adc = 0;
        };
        // Verify: the last batch made natively (clip positions as the variant stores them, the ADC flags, the GS vertices
        // its fast path would send) and the GS vertices the VU's variant sent.
        struct Last
        {
            uint32_t base = 0, count = 0;
            std::vector<std::array<float, 4>> clip;
            std::vector<uint16_t> adc;
            std::vector<Out> out;
        } g_last;
        std::vector<Out> g_kicked;
        bool g_capturing = false, g_fastPath = false;
        struct OutStats
        {
            uint64_t batches = 0, differs = 0, vertices = 0;
            std::array<uint64_t, 7> byField{}; // ST, Q, RGBA, XY, Z, fog, ADC
        } g_out;
        uint32_t bitsOf(float f)
        {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        }
        int32_t ftoi(float f, float scale)
        {
            const float g = f * scale;
            return std::isnan(g) ? 0 : g >= 2147483520.0f ? INT32_MAX : g <= -2147483648.0f ? INT32_MIN : static_cast<int32_t>(g);
        }

        const vu::Image *imageOf(uint32_t packet)
        {
            for (uint32_t id = 0; id < vu::kImageCount; ++id)
                if (vu::kImages[id].source == packet)
                    return &vu::kImages[id];
            return nullptr;
        }

        float lane(const vu::Unit &v, uint32_t qw, int i) { return reinterpret_cast<const float *>(v.data + 16u * (qw & v.qwMask))[i]; }
        uint32_t word(const vu::Unit &v, uint32_t qw, int i) { return reinterpret_cast<const uint32_t *>(v.data + 16u * (qw & v.qwMask))[i]; }

        // ---- Whole objects natively (SHINO_NATIVE_OBJECTS) ----------------------------------------------------------
        // The main renderer's object whose variant is drawn natively and whose lights are all read: none of its programs
        // run on the VU (its setup and batches made here).
        bool g_objectNative = false;
        uint32_t g_objectVariant = 0;
        bool g_writeBack = false; // batch(): the first pass's results written back (as in skip mode)
        struct ObjectStats
        {
            uint64_t objects = 0, native = 0, batches = 0, vertices = 0, compared = 0, differs = 0, words = 0, tpcDiffers = 0;
            std::array<uint64_t, 2> notNative{}; // a variant not computed here, a light not read
        };
        std::array<ObjectStats, 4> g_objects; // by kObjectRenderers

        // The setup's choice of variant (pairs 0x28-0x188) from the flags (1023.x): bit 1 the clipping camera, then bits
        // 3, 2 and 0; the main renderer's entries, or the characters' (the same code, their own variants).
        uint32_t variantOf(uint32_t flags, bool characters)
        {
            const bool b0 = flags & 1u, b2 = flags & 4u, b3 = flags & 8u;
            if (flags & 2u)
            {
                if (b3)
                    return characters ? (b2 ? 1528u : 1113u) : (b2 ? 1438u : 1023u);
                return characters ? (b2 ? (b0 ? 1037u : 875u) : (b0 ? 933u : 800u)) : (b2 ? (b0 ? 947u : 785u) : (b0 ? 843u : 710u));
            }
            return characters ? (b2 ? (b0 ? 732u : 589u) : (b0 ? 644u : 524u)) : (b2 ? (b0 ? 642u : 499u) : (b0 ? 554u : 434u));
        }
        // The renderers made whole here: the microcode, the next batches' program (MSCNT's TPC), the output buffers
        // (left out of verify's compare), and whether the batch comes in skinned (five quadwords a vertex).
        struct ObjectRenderer
        {
            uint32_t packet, program, outFirst, outEnd;
            bool characters;        // the characters' code (skinned), else the main renderer's
            uint32_t variantOffset; // its variants' entries past the template's
            bool uvTransform;       // its setup ends with the texture transform (956-958)
            const char *name;
        };
        constexpr ObjectRenderer kObjectRenderers[] = {
            {0x539c30u, 0x72du, 552u, 1008u, false, 0u, false, "main"},       // 1023, 554, 1438 native
            {0x579a00u, 0x787u, 430u, 696u, true, 0u, false, "characters'"}, // 644, 1113 native (the bones from 696)
            {0x5cc000u, 0x742u, 544u, 956u, false, 21u, true, "0x5cc000"},   // the main renderer's, the UVs transformed
            {0x5e7cd0u, 0x79cu, 430u, 696u, true, 21u, true, "0x5e7cd0"},    // the characters', the UVs transformed
        };
        constexpr uint32_t kObjectRendererCount = sizeof(kObjectRenderers) / sizeof(kObjectRenderers[0]);
        const ObjectRenderer *g_objectRenderer = nullptr;
        const ObjectRenderer *objectRenderer(uint32_t packet)
        {
            for (const ObjectRenderer &o : kObjectRenderers)
                if (o.packet == packet)
                    return &o;
            return nullptr;
        }
        bool variantNative(const ObjectRenderer &r, uint32_t variant)
        {
            const uint32_t v = variant - r.variantOffset;
            return r.characters ? v == 644u || v == 1113u : v == 1023u || v == 554u || v == 1438u;
        }
        // The light list at 980 has only the types nativeSetup reads (0 ends it; 2 a constant colour; 128 a point light;
        // 129 a spotlight).
        bool lightsRead(const uint8_t *mem)
        {
            for (uint32_t at = 980, guard = 0; guard < 16; ++guard)
            {
                uint32_t w;
                std::memcpy(&w, mem + 16u * (at & 0x3FFu) + 12u, 4);
                switch (w & 0xFFFFu)
                {
                case 0: return true;
                case 2: at += 1; break;
                case 128: at += 2; break;
                case 129: at += 3; break;
                default: return false;
                }
            }
            return false;
        }
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

    const uint8_t *takeOverAt(const vu::Unit &v)
    {
        // A renderer: its microcode's image, its entries checked (the setup at pair 0, the next batches' program, each
        // native variant's), and those variants' pairs marked.
        struct Renderer
        {
            uint32_t packet = 0;
            const vu::Image *image = nullptr;
            std::vector<uint32_t> pairs;
            std::array<uint8_t, 2048> stops{};
        };
        static std::vector<Renderer> renderers = [] {
            std::vector<Renderer> list;
            for (const Kind &k : kKinds)
            {
                auto it = std::find_if(list.begin(), list.end(), [&](const Renderer &r) { return r.packet == k.packet; });
                if (it == list.end())
                {
                    list.push_back({});
                    it = list.end() - 1;
                    it->packet = k.packet;
                    it->image = imageOf(k.packet);
                    it->pairs = {0u, k.program};
                }
                it->pairs.push_back(k.variant);
                it->stops[k.variant] = 1;
            }
            return list;
        }();
        if (v.vu0)
            return nullptr;
        for (const Renderer &r : renderers)
        {
            const vu::Image *im = r.image;
            bool loaded = im != nullptr;
            for (const uint32_t p : r.pairs)
            {
                if (!loaded || p < im->start || p >= im->start + im->count)
                {
                    loaded = false;
                    break;
                }
                const uint32_t k = p - im->start;
                loaded = v.micro[2 * p] == im->code[2 * k] && v.micro[2 * p + 1] == im->code[2 * k + 1];
            }
            if (loaded)
            {
                g_packet = r.packet;
                return r.stops.data();
            }
        }
        return nullptr;
    }

    void batch(vu::Unit &v, uint32_t pc, bool submit)
    {
        const uint32_t base = v.vi[1], count = v.vi[9];
        for (uint32_t i = 0; i < kKindCount; ++i)
            if (kKinds[i].packet == g_packet && kKinds[i].variant == pc / 8u)
                g_kind = i;
        const Kind &kind = kKinds[g_kind];
        const ObjectRenderer *setupOf = objectRenderer(kind.packet);
        const bool skin = setupOf && setupOf->characters, uvOut = skin || (setupOf && setupOf->uvTransform);
        if (g_rawValid && setupOf && g_rawTop == base && g_rawCount == count)
        {
            SetupOut s;
            SetupStats &st = g_setup[static_cast<size_t>(setupOf - kObjectRenderers)];
            ++st.batches;
            if (!nativeSetup(g_raw.data(), base, count, s, skin, setupOf->uvTransform))
                ++st.unsupported;
            else
                for (uint32_t k = 0; k < s.position.size(); ++k) // (the batch rounded up to four)
                {
                    const uint32_t q = base + 4u * k;
                    st.vertices++;
                    bool dp = false, du = false, dn = false, dc = false;
                    for (int i = 0; i < 4; ++i)
                    {
                        dp |= std::memcmp(&s.position[k][i], v.data + 16u * (q & v.qwMask) + 4u * i, 4) != 0;
                        du |= uvOut && std::memcmp(&s.uv[k][i], v.data + 16u * ((q + 1) & v.qwMask) + 4u * i, 4) != 0;
                        dc |= std::memcmp(&s.colour[k][i], v.data + 16u * ((q + 2) & v.qwMask) + 4u * i, 4) != 0;
                        dn |= std::memcmp(&s.normal[k][i], v.data + 16u * ((q + 3) & v.qwMask) + 4u * i, 4) != 0;
                    }
                    st.positionDiffers += dp;
                    st.uvDiffers += du;
                    st.normalDiffers += dn;
                    st.colourDiffers += dc;
                    if ((dp || du || dc || dn) && st.positionDiffers + st.uvDiffers + st.colourDiffers + st.normalDiffers <= 6)
                        std::printf("[shino:native3d] setup %s vertex %u of %u (slots differ: %d%d%d%d): position %g %g %g %g, the VU's %g %g %g %g; colour %g %g %g, the VU's %g %g %g%c",
                                    setupOf->name, k, count, dp, du, dc, dn, s.position[k][0], s.position[k][1], s.position[k][2], s.position[k][3],
                                    lane(v, q, 0), lane(v, q, 1), lane(v, q, 2), lane(v, q, 3), s.colour[k][0], s.colour[k][1], s.colour[k][2],
                                    lane(v, q + 2, 0), lane(v, q + 2, 1), lane(v, q + 2, 2), 10);
                }
        }
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
        // (a list's: three at a time, the batch's own)
        const uint32_t passed = kind.clipper && !kind.list ? std::max<uint32_t>(((4u * count + 4u) & 0x7FF8u) / 4u, 2u) : count;
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
                // each step clamped as the VU's (no NaN, nothing past the largest finite: what stale vertices past the
                // batch may hold)
                float a = vu::clampF(r3);
                a = vu::clampF(a + r0 * px);
                a = vu::clampF(a + r1 * py);
                r[i] = vu::clampF(a + r2 * pz);
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
        if (kind.replay && mode() == Mode::On)
        {
            Replay &r = g_replay;
            r.active = true;
            r.count = count;
            r.position.resize(count);
            r.screen.resize(count);
            std::copy(matrix, matrix + 16, r.matrix);
            for (uint32_t k = 0; k < count; ++k)
            {
                r.position[k] = {corner[k].x, corner[k].y, corner[k].z};
                r.screen[k] = {cx + clip[k][0] / clip[k][3], cy + clip[k][1] / clip[k][3]};
            }
            r.packets.clear();
            ++g_replayStats.batches;
            return;
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
        if (!kind.clipper)
        {
            // The characters' variant: each triangle's facing from its corners on the screen as it makes them (P = the
            // clip position times Q, Q = kz / (the rows' w scaled by kz)), the z of the edges' cross product as its
            // OPMULA / OPMSUB leave it (A.x B.y - B.x A.y), the edges taken one way on alternate vertices and the other
            // way between (the strip's winding); ADC where the sign differs from the cull mode (1023.z).
            const uint32_t cull = word(v, 1023, 2) & 0xFFFFu;
            const float kz = lane(v, 1014, 2);
            std::vector<std::array<float, 2>> P(count);
            for (uint32_t j = 0; j < count; ++j)
            {
                const uint32_t q = base + 4u * j;
                const float px = lane(v, q, 0), py = lane(v, q, 1), pz = lane(v, q, 2);
                float wk = row[3][3] * kz;
                wk = wk + (row[0][3] * kz) * px;
                wk = wk + (row[1][3] * kz) * py;
                wk = wk + (row[2][3] * kz) * pz;
                const float Qk = kz / wk;
                P[j] = {clip[j][0] * Qk, clip[j][1] * Qk};
            }
            for (uint32_t k = 2; k < count; ++k)
            {
                std::array<float, 2> A, B;
                if ((k & 1u) != 0) // the odd vertices: A from this one back, B from the one before two back
                {
                    A = {P[k][0] - P[k - 1][0], P[k][1] - P[k - 1][1]};
                    B = {P[k - 2][0] - P[k - 1][0], P[k - 2][1] - P[k - 1][1]};
                }
                else
                {
                    A = {P[k - 1][0] - P[k - 2][0], P[k - 1][1] - P[k - 2][1]};
                    B = {P[k - 1][0] - P[k][0], P[k - 1][1] - P[k][1]};
                }
                const float ab = A[0] * B[1], ba = B[0] * A[1];
                const float z = ab - ba;
                const uint32_t sign = std::signbit(z) ? 32u : 0u;
                adc[k] = sign != cull ? 0x8000u : 0u;
            }
        }
        // A triangle list's (1438): each triangle's facing, cross(v1, v0) . v2, on its third vertex; the first two 0.
        for (uint32_t k = 2; kind.clipper && kind.list && k < passed; k += 3)
        {
            const float d = dot(cross(clip[k - 1], clip[k - 2]), clip[k]);
            const uint16_t sign = std::signbit(d) ? 32u : 0u;
            adc[k] = static_cast<uint16_t>(mode7 + sign) & 0x8000u;
        }
        for (uint32_t k = 2; kind.clipper && !kind.list && k < passed; ++k)
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
        std::vector<uint8_t> outside(passed);
        for (uint32_t k = 0; kind.clipper && k < passed; ++k)
        {
            const float x = clip[k][0] * guardX, y = clip[k][1] * guardY, w = clip[k][3], aw = std::fabs(w);
            const float z = depth0 + depth1 * w;
            outside[k] = static_cast<uint8_t>((x > aw ? 1u : 0u) | (x < -aw ? 2u : 0u) | (y > aw ? 4u : 0u) | (y < -aw ? 8u : 0u) |
                                              (z > aw ? 16u : 0u) | (z < -aw ? 32u : 0u));
        }
        // Every vertex the first pass went over outside one plane: the variant draws none of the batch.
        uint8_t allOutside = kind.clipper ? 63u : 0u;
        for (uint32_t k = 0; kind.clipper && k < passed; ++k)
            allOutside &= outside[k];
        std::vector<bozo::render::Vertex> triangles;
        triangles.reserve(count * 3);
        for (uint32_t k = 2; k < count && !allOutside; k += kind.list ? 3u : 1u)
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
        if ((mode() == Mode::Skip || g_writeBack) && kind.clipper) // (otherwise the VU's own variant writes them)
            for (uint32_t k = 0; k < passed; ++k)
            {
                const uint32_t q = base + 4u * k;
                std::memcpy(v.data + 16u * (q & v.qwMask), clip[k].data(), 16);
                const uint32_t flag = adc[k];
                std::memcpy(v.data + 16u * ((q + 1) & v.qwMask) + 12, &flag, 4);
            }
        if (mode() == Mode::Verify)
        {
            // The fast path's GS vertices (the variant's output loop): STQ = (s, t, 1) Q, Q = 1 / w; RGBA by ftoi0;
            // XYZ = ftoi4 of 1016 + 1017 + the row sums times Q; F = ftoi4 of the fog, ADC.
            g_last.out.assign(count, {});
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t q = base + 4u * k;
                const float w = clip[k][3], Q = 1.0f / w;
                Out &o = g_last.out[k];
                o.s = bitsOf(lane(v, q + 1, 0) * Q);
                o.t = bitsOf(lane(v, q + 1, 1) * Q);
                o.q = bitsOf(1.0f * Q);
                o.rgba = corner[k].rgba;
                o.x = static_cast<uint32_t>(ftoi(cx + clip[k][0] * Q, 16.0f)) & 0xFFFFu;
                o.y = static_cast<uint32_t>(ftoi(cy + clip[k][1] * Q, 16.0f)) & 0xFFFFu;
                o.z = static_cast<uint32_t>(ftoi(cz + sz * Q, 16.0f) >> 4) & 0xFFFFFFu;
                float f = fogScale * (w - fogFar);
                f = std::min(std::max(f, 0.0f), 255.0f);
                o.f = static_cast<uint32_t>(ftoi(f, 16.0f) >> 4) & 0xFFu;
                o.adc = adc[k] ? 1u : 0u;
                if (!kind.clipper)
                {
                    // The characters' variant: w by rows scaled by kz (1014.z), Q = kz / that; the fog word
                    // min(max(w, a), b) - a, a = 1014.w kz, b = 255 + a; with ADC 2048.5 more (bit 15 of ftoi4).
                    const float kz = lane(v, 1014, 2), a = lane(v, 1014, 3) * kz, b = 255.0f + lane(v, 1014, 3) * kz;
                    const float px = lane(v, q, 0), py = lane(v, q, 1), pz = lane(v, q, 2);
                    float wk = row[3][3] * kz;
                    wk = wk + (row[0][3] * kz) * px;
                    wk = wk + (row[1][3] * kz) * py;
                    wk = wk + (row[2][3] * kz) * pz;
                    const float Qk = kz / wk;
                    o.s = bitsOf(lane(v, q + 1, 0) * Qk);
                    o.t = bitsOf(lane(v, q + 1, 1) * Qk);
                    o.q = bitsOf(1.0f * Qk);
                    o.x = static_cast<uint32_t>(ftoi(clip[k][0] * Qk - (0.0f - cx), 16.0f)) & 0xFFFFu;
                    o.y = static_cast<uint32_t>(ftoi(clip[k][1] * Qk - (0.0f - cy), 16.0f)) & 0xFFFFu;
                    o.z = static_cast<uint32_t>(ftoi(sz * Qk - (0.0f - cz), 16.0f) >> 4) & 0xFFFFFFu;
                    const float fw = std::min(std::max(wk, a), b);
                    const uint32_t word3 = static_cast<uint32_t>(ftoi(adc[k] ? fw - (a - 2048.5f) : fw - a, 16.0f));
                    o.f = (word3 >> 4) & 0xFFu;
                    o.adc = (word3 >> 15) & 1u;
                }
            }
            g_kicked.clear();
            g_capturing = true;
            g_fastPath = true; // no vertex outside a clip plane: the variant's output loop, not its clipper
            for (uint32_t k = 0; k < passed && k < outside.size(); ++k)
                g_fastPath &= outside[k] == 0;
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
        g_capturing = false;
        if (g_fastPath && g_kicked.size() == g_last.out.size() && !g_kicked.empty()) // the fast path: a GS vertex a batch vertex
        {
            ++g_out.batches;
            for (size_t k = 0; k < g_kicked.size(); ++k)
            {
                const Out &a = g_last.out[k], &b = g_kicked[k];
                ++g_out.vertices;
                const bool d[7] = {a.s != b.s || a.t != b.t, a.q != b.q, a.rgba != b.rgba, a.x != b.x || a.y != b.y, a.z != b.z,
                                   a.f != b.f, k >= 2 && a.adc != b.adc}; // (ADC: the strip's first two vertices draw nothing)
                bool any = false;
                for (int i = 0; i < 7; ++i)
                    if (d[i])
                    {
                        ++g_out.byField[i];
                        any = true;
                    }
                if (any && g_out.differs++ < 12)
                    std::printf("[shino:native3d] packet vertex %zu: native st %08x %08x q %08x rgba %08x xy %x %x z %x f %x adc %u; the VU's "
                                "st %08x %08x q %08x rgba %08x xy %x %x z %x f %x adc %u%c", k, a.s, a.t, a.q, a.rgba, a.x, a.y, a.z, a.f, a.adc,
                                b.s, b.t, b.q, b.rgba, b.x, b.y, b.z, b.f, b.adc, 10);
            }
        }
        const Last &l = g_last;
        if (!kKinds[g_kind].clipper)
        {
            ++g_stats.compared;
            return;
        }
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

    int objectsMode()
    {
        static const int m = [] {
            const char *e = std::getenv("SHINO_NATIVE_OBJECTS"); // (unset: on, with native 3D on)
            if (mode() != Mode::On || (e && *e == '0'))
                return 0;
            return e && std::strcmp(e, "verify") == 0 ? 2 : 1;
        }();
        return m;
    }

    bool programNative(vu::Unit &v, uint32_t pc, uint32_t top, uint32_t itop, bool submit)
    {
        const ObjectRenderer *r = objectRenderer(g_packet);
        if (!r)
            return false;
        ObjectStats &st = g_objects[static_cast<size_t>(r - kObjectRenderers)];
        if (pc == 0) // an object's setup: native when its variant and its lights are
        {
            ++st.objects;
            const uint32_t variant = variantOf(word(v, 1023, 0) & 0xFFFFu, r->characters) + r->variantOffset;
            const bool native = variantNative(*r, variant);
            const bool lit = (word(v, 980, 3) & 0xFFFFu) != 0;
            g_objectNative = native && (!lit || lightsRead(v.data));
            g_objectVariant = variant;
            g_objectRenderer = r;
            if (!g_objectNative)
            {
                ++st.notNative[native ? 1 : 0];
                return false;
            }
            ++st.native;
            const uint32_t entry = variant; // isw.y vi12, 1023: the variant's entry kept in 1023.y
            std::memcpy(v.data + 16u * 1023u + 4u, &entry, 4);
        }
        else if (pc != r->program * 8u || !g_objectNative || g_objectRenderer != r) // (MSCNT: the object's next batch)
            return false;
        ++st.batches;
        st.vertices += itop;
        // The setup in place: positions (skinned, morphed), UVs, colours (lit), normals, as the VU's leaves the buffer
        SetupOut s;
        nativeSetup(v.data, top, itop, s, r->characters, r->uvTransform);
        for (uint32_t k = 0; k < s.position.size(); ++k) // (the batch rounded up to four vertices)
        {
            const uint32_t q = top + 4u * k;
            std::memcpy(v.data + 16u * (q & v.qwMask), s.position[k].data(), 16);
            if (r->characters || r->uvTransform)
                std::memcpy(v.data + 16u * ((q + 1) & v.qwMask), s.uv[k].data(), 16);
            std::memcpy(v.data + 16u * ((q + 2) & v.qwMask), s.colour[k].data(), 16);
            std::memcpy(v.data + 16u * ((q + 3) & v.qwMask), s.normal[k].data(), 16);
        }
        v.vi[1] = static_cast<uint16_t>(top);
        v.vi[9] = static_cast<uint16_t>(itop);
        g_writeBack = true;
        batch(v, g_objectVariant * 8u, submit);
        g_writeBack = false;
        v.tpc = r->program * 8u; // as the variant ends (its E bit's pair before the program): the next batch is an MSCNT
        return true;
    }

    void compareObject(const vu::Unit &v, const uint8_t *native)
    {
        // everything but the output packets (the main renderer's 552-1007, the characters' 430-695)
        const ObjectRenderer &r = *g_objectRenderer;
        ObjectStats &st = g_objects[static_cast<size_t>(&r - kObjectRenderers)];
        ++st.compared;
        uint32_t words = 0;
        for (uint32_t q = 0; q < 1024; ++q)
        {
            if (q >= r.outFirst && q < r.outEnd)
                continue;
            for (int i = 0; i < 4; ++i)
                if (std::memcmp(v.data + 16 * q + 4 * i, native + 16 * q + 4 * i, 4) != 0)
                {
                    if (++words <= 4 && st.differs < 8)
                    {
                        uint32_t a, b;
                        std::memcpy(&a, native + 16 * q + 4 * i, 4);
                        std::memcpy(&b, v.data + 16 * q + 4 * i, 4);
                        std::printf("[shino:native3d] object memory %u.%c: native %08x, the VU's %08x (variant %u)%c", q, "xyzw"[i], a, b,
                                    g_objectVariant, 10);
                    }
                }
        }
        st.differs += words != 0;
        st.words += words;
        st.tpcDiffers += v.tpc != r.program * 8u;
    }

    void programStart(const vu::Unit &v, uint32_t top, uint32_t itop)
    {
        g_rawValid = mode() == Mode::Verify && objectRenderer(g_packet) != nullptr;
        if (!g_rawValid)
            return;
        g_raw.assign(v.data, v.data + 16384);
        g_rawTop = top;
        g_rawCount = itop;
    }

    bool capturing() { return g_capturing || g_replay.active; }

    void captureKick(const uint8_t *mem, uint32_t address)
    {
        if (g_replay.active) // each packet kept whole (its tag with EOP set, then its data)
            for (int packets = 0, at = static_cast<int>(address); packets < 64; ++packets)
            {
                const uint8_t *tag = mem + 16 * (static_cast<uint32_t>(at) & 0x3FFu);
                uint64_t lo;
                std::memcpy(&lo, tag, 8);
                const uint32_t loops = lo & 0x7FFFu, flg = (lo >> 58) & 3u, nreg = (lo >> 60) ? uint32_t(lo >> 60) : 16u;
                const uint32_t qwords = flg == 0 ? loops * nreg : flg == 1 ? (loops * nreg + 1) / 2 : loops;
                std::vector<uint8_t> p(16u * (qwords + 1));
                for (uint32_t q = 0; q <= qwords; ++q)
                    std::memcpy(p.data() + 16 * q, mem + 16 * ((static_cast<uint32_t>(at) + q) & 0x3FFu), 16);
                const uint64_t eop = lo | (1ull << 15);
                std::memcpy(p.data(), &eop, 8);
                g_replay.packets.push_back(std::move(p));
                at += static_cast<int>(qwords + 1);
                if ((lo >> 15) & 1u)
                    break;
            }
        for (int packets = 0; packets < 64; ++packets)
        {
            const uint8_t *tag = mem + 16 * (address & 0x3FFu);
            uint64_t lo, regs;
            std::memcpy(&lo, tag, 8);
            std::memcpy(&regs, tag + 8, 8);
            const uint32_t loops = lo & 0x7FFFu, flg = (lo >> 58) & 3u, nreg = (lo >> 60) ? uint32_t(lo >> 60) : 16u;
            const uint32_t qwords = flg == 0 ? loops * nreg : flg == 1 ? (loops * nreg + 1) / 2 : loops;
            if (flg == 0)
                for (uint32_t i = 0; i < loops; ++i)
                {
                    Out o;
                    bool vertex = false;
                    for (uint32_t r = 0; r < nreg; ++r)
                    {
                        uint32_t w[4];
                        std::memcpy(w, mem + 16 * ((address + 1 + i * nreg + r) & 0x3FFu), 16);
                        switch ((regs >> (4 * r)) & 15u)
                        {
                        case 0x2: // ST (Q in the third word)
                            o.s = w[0];
                            o.t = w[1];
                            o.q = w[2];
                            break;
                        case 0x1: // RGBAQ
                            o.rgba = (w[0] & 0xFFu) | (w[1] & 0xFFu) << 8 | (w[2] & 0xFFu) << 16 | (w[3] & 0xFFu) << 24;
                            break;
                        case 0x4: // XYZF2
                            o.x = w[0] & 0xFFFFu;
                            o.y = w[1] & 0xFFFFu;
                            o.z = (w[2] >> 4) & 0xFFFFFFu;
                            o.f = (w[3] >> 4) & 0xFFu;
                            o.adc = (w[3] >> 15) & 1u;
                            vertex = true;
                            break;
                        default:
                            break;
                        }
                    }
                    if (vertex)
                        g_kicked.push_back(o);
                }
            address += qwords + 1;
            if ((lo >> 15) & 1u)
                break;
        }
    }

    void finish(const vu::Unit &)
    {
        Replay &r = g_replay;
        if (!r.active)
            return;
        r.active = false;
        static std::vector<uint8_t> memory(16384);
        for (const std::vector<uint8_t> &p : r.packets)
        {
            uint64_t lo, regs;
            std::memcpy(&lo, p.data(), 8);
            std::memcpy(&regs, p.data() + 8, 8);
            const uint32_t loops = lo & 0x7FFFu, flg = (lo >> 58) & 3u, nreg = (lo >> 60) ? uint32_t(lo >> 60) : 16u;
            // A strip of the batch: PACKED, ST, RGBAQ and XYZF2 (or XYZ2) a vertex, as many as the batch's
            int st = -1, rgbaq = -1, xyz = -1, uv = -1;
            for (uint32_t i = 0; i < nreg; ++i)
                switch ((regs >> (4 * i)) & 15u)
                {
                case 0x2: st = static_cast<int>(i); break;
                case 0x1: rgbaq = static_cast<int>(i); break;
                case 0x3: uv = static_cast<int>(i); break;
                case 0x4: case 0x5: xyz = static_cast<int>(i); break;
                default: break;
                }
            bool strip = flg == 0 && xyz >= 0 && uv < 0 && loops == r.count && r.count >= 3;
            std::vector<bozo::render::Vertex> corners;
            std::vector<bool> adc;
            if (strip)
            {
                ++g_replayStats.strips;
                corners.resize(loops);
                adc.resize(loops);
                const bool xyzf = ((regs >> (4 * xyz)) & 15u) == 0x4u;
                for (uint32_t k = 0; k < loops && strip; ++k)
                {
                    auto field = [&](int reg) {
                        std::array<uint32_t, 4> w;
                        std::memcpy(w.data(), p.data() + 16 * (1 + k * nreg + static_cast<uint32_t>(reg)), 16);
                        return w;
                    };
                    const auto x = field(xyz);
                    // the packet's vertex on the screen against the batch's: the same vertex, or not this strip
                    const float px = float(x[0] & 0xFFFFu) / 16.0f, py = float(x[1] & 0xFFFFu) / 16.0f;
                    if (std::fabs(px - r.screen[k][0]) > 0.5f || std::fabs(py - r.screen[k][1]) > 0.5f)
                    {
                        strip = false;
                        ++g_replayStats.misaligned;
                        break;
                    }
                    bozo::render::Vertex &c = corners[k];
                    c.x = r.position[k][0];
                    c.y = r.position[k][1];
                    c.z = r.position[k][2];
                    if (st >= 0)
                    {
                        const auto s = field(st);
                        float S, T, Q;
                        std::memcpy(&S, &s[0], 4);
                        std::memcpy(&T, &s[1], 4);
                        std::memcpy(&Q, &s[2], 4);
                        c.u = Q != 0.0f ? S / Q : S;
                        c.v = Q != 0.0f ? T / Q : T;
                    }
                    if (rgbaq >= 0)
                    {
                        const auto cw = field(rgbaq);
                        c.rgba = (cw[0] & 0xFFu) | (cw[1] & 0xFFu) << 8 | (cw[2] & 0xFFu) << 16 | (cw[3] & 0xFFu) << 24;
                    }
                    c.fog = xyzf ? float((x[3] >> 4) & 0xFFu) / 255.0f : 1.0f;
                    c.q = 1.0f;
                    adc[k] = (x[3] >> 15) & 1u;
                }
            }
            if (strip)
            {
                std::vector<bozo::render::Vertex> triangles;
                triangles.reserve(3 * loops);
                for (uint32_t k = 2; k < loops; ++k)
                    if (!adc[k])
                    {
                        triangles.push_back(corners[k - 2]);
                        triangles.push_back(corners[k - 1]);
                        triangles.push_back(corners[k]);
                    }
                const uint64_t prim = (lo >> 46) & 1u ? (lo >> 47) & 0x7FFu : ~0ull;
                if (!triangles.empty())
                    gs::nativeTriangles(prim, r.matrix, triangles.data(), static_cast<uint32_t>(triangles.size()));
                else if (prim != ~0ull)
                    gs::writeRegister(0x00, prim);
                ++g_replayStats.native;
                continue;
            }
            // anything else as the GS takes it
            ++g_replayStats.other;
            std::memcpy(memory.data(), p.data(), std::min<size_t>(p.size(), memory.size()));
            gs::path1(memory.data(), 0);
        }
        r.packets.clear();
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
        {
            std::printf("; compared %llu: ADC differed %llu, positions %llu (largest relative error %.3g)", (unsigned long long)s.compared,
                        (unsigned long long)s.flagDiffers, (unsigned long long)s.positionDiffers, s.positionError);
            std::printf("; fast-path packets %llu, %llu vertices: ST %llu, Q %llu, RGBA %llu, XY %llu, Z %llu, fog %llu, ADC %llu differ",
                        (unsigned long long)g_out.batches, (unsigned long long)g_out.vertices, (unsigned long long)g_out.byField[0],
                        (unsigned long long)g_out.byField[1], (unsigned long long)g_out.byField[2], (unsigned long long)g_out.byField[3],
                        (unsigned long long)g_out.byField[4], (unsigned long long)g_out.byField[5], (unsigned long long)g_out.byField[6]);
        }
        for (uint32_t r = 0; r < kObjectRendererCount; ++r)
            if (const SetupStats &st = g_setup[r]; st.batches)
                std::printf("; native setup (%s) %llu batches (%llu with a light not read), %llu vertices: position %llu, UV %llu, normal %llu, colour %llu differ",
                            kObjectRenderers[r].name, (unsigned long long)st.batches, (unsigned long long)st.unsupported,
                            (unsigned long long)st.vertices, (unsigned long long)st.positionDiffers, (unsigned long long)st.uvDiffers,
                            (unsigned long long)st.normalDiffers, (unsigned long long)st.colourDiffers);
        for (uint32_t r = 0; r < kObjectRendererCount; ++r)
        {
            const ObjectStats &o = g_objects[r];
            const char *name = kObjectRenderers[r].name;
            if (o.objects)
                std::printf("; %s objects %llu, %llu whole natively (%llu batches, %llu vertices; not: %llu by variant, %llu by light)", name,
                            (unsigned long long)o.objects, (unsigned long long)o.native, (unsigned long long)o.batches,
                            (unsigned long long)o.vertices, (unsigned long long)o.notNative[0], (unsigned long long)o.notNative[1]);
            if (o.compared)
                std::printf("; %s programs compared %llu: memory differs in %llu (%llu words), TPC in %llu", name, (unsigned long long)o.compared,
                            (unsigned long long)o.differs, (unsigned long long)o.words, (unsigned long long)o.tpcDiffers);
        }
        if (g_replayStats.batches)
            std::printf("; replayed %llu batches: strips %llu (%llu drawn natively, %llu not the batch's on the screen), %llu other packets",
                        (unsigned long long)g_replayStats.batches, (unsigned long long)g_replayStats.strips,
                        (unsigned long long)g_replayStats.native, (unsigned long long)g_replayStats.misaligned,
                        (unsigned long long)g_replayStats.other);
        std::printf("%c", 10);
    }
}
