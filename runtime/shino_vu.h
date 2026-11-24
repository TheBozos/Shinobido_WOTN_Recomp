// The VU1's state and its instructions' operations, shared by the interpreter (runtime/shino_vu1.cpp) and the
// recompiled microprograms (tools/shino_vu1_recomp.py writes them from the player's executable into the build): both
// run the same operations, so a compiled program computes what the interpreter does, value for value.
#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <immintrin.h>

#define SHINO_VU_INLINE [[gnu::always_inline]] inline

namespace shino::vu
{
    struct Vec
    {
        float f[4] = {0, 0, 0, 0}; // x y z w
    };
    // A vector unit: VU1 (16 KB data, 16 KB code), or VU0 (4 KB each; its registers are the EE's COP2 state, copied
    // in and out around a program, and its data memory is the runtime's).
    struct Unit
    {
        Unit() { vf[0].f[3] = 1.0f; } // vf0 is the constant (0, 0, 0, 1): writes to it are dropped
        Vec vf[32];
        uint16_t vi[16] = {};
        Vec acc;
        float q = 0, p = 0, i = 0;
        uint32_t r = 0x3F800000u;
        uint32_t clip = 0, status = 0;
        uint16_t mac = 0;
        std::array<uint16_t, 4> macHistory{}; // the MAC flags of the last 4 pairs (FMAND's delay)
        uint32_t macAt = 0;
        float pendingQ = 0;
        int qDelay = 0;
        alignas(16) uint8_t mem[16384] = {};
        uint32_t micro[4096] = {}; // 2048 pairs: lower at even, upper at odd
        uint32_t tpc = 0;          // where the last program stopped (MSCNT)
        uint64_t programs = 0, pairs = 0, kicks = 0, refused = 0;
        uint8_t *data = mem;       // the data memory (VU0: the runtime's)
        uint32_t qwMask = 0x3FFu;  // its quadwords
        uint32_t pcMask = 0x3FF8u; // the code memory's bytes (pairs of 8)
        bool vu0 = false;
    };

    constexpr float kFloatMax = 3.40282347e38f;
    SHINO_VU_INLINE float clampF(float v)
    {
        if (std::isnan(v))
            return 0.0f;
        if (v > kFloatMax)
            return kFloatMax;
        if (v < -kFloatMax)
            return -kFloatMax;
        return v;
    }
    SHINO_VU_INLINE float asF(uint32_t v) { return std::bit_cast<float>(v); }
    SHINO_VU_INLINE uint32_t asU(float v) { return std::bit_cast<uint32_t>(v); }
    SHINO_VU_INLINE int16_t s16(uint32_t v) { return static_cast<int16_t>(v); }
    // dest bits: x = 8, y = 4, z = 2, w = 1 (instruction bits 24..21)
    SHINO_VU_INLINE bool has(uint32_t dest, int field) { return (dest >> (3 - field)) & 1u; }
    SHINO_VU_INLINE Vec &memVec(Unit &v, uint32_t qw) { return *reinterpret_cast<Vec *>(v.data + 16 * (qw & v.qwMask)); }
    SHINO_VU_INLINE uint32_t &memWord(Unit &v, uint32_t qw, int field)
    {
        return reinterpret_cast<uint32_t *>(v.data + 16 * (qw & v.qwMask))[field];
    }

    // The lanes a dest field writes (x in lane 0), as masks; and 4 bits reversed (lane order to the MAC's x = bit 3).
    struct LaneMasks
    {
        __m128 m[16];
        uint8_t rev[16];
        LaneMasks()
        {
            for (uint32_t dest = 0; dest < 16; ++dest)
            {
                alignas(16) uint32_t w[4];
                for (int f = 0; f < 4; ++f)
                    w[f] = has(dest, f) ? 0xFFFFFFFFu : 0u;
                m[dest] = _mm_load_ps(reinterpret_cast<const float *>(w));
                rev[dest] = static_cast<uint8_t>(((dest & 1u) << 3) | ((dest & 2u) << 1) | ((dest & 4u) >> 1) | ((dest & 8u) >> 3));
            }
        }
    };
    extern const LaneMasks g_lanes;
    SHINO_VU_INLINE __m128 lanes(uint32_t dest) { return g_lanes.m[dest & 15u]; }

    // ---- The upper arithmetic -----------------------------------------------------------------------------------
    // fn: 0 add 1 sub 2 madd 3 msub 4 mul 5 max 6 min; operand: 0 ft, 1 ft's bc field broadcast, 2 Q, 3 I. MAX / MINI
    // as std::max / std::min (operands swapped for SSE's), NaN 0, clamped to the largest finite.
    SHINO_VU_INLINE __m128 arith(const Unit &v, int fn, int operand, uint32_t fs, uint32_t ft, uint32_t bc)
    {
        const __m128 a = _mm_loadu_ps(v.vf[fs].f);
        __m128 b;
        switch (operand)
        {
        case 0: b = _mm_loadu_ps(v.vf[ft].f); break;
        case 1: b = _mm_set1_ps(v.vf[ft].f[bc]); break;
        case 2: b = _mm_set1_ps(v.q); break;
        default: b = _mm_set1_ps(v.i); break;
        }
        __m128 r;
        switch (fn)
        {
        case 0: r = _mm_add_ps(a, b); break;
        case 1: r = _mm_sub_ps(a, b); break;
        case 2: r = _mm_add_ps(_mm_loadu_ps(v.acc.f), _mm_mul_ps(a, b)); break;
        case 3: r = _mm_sub_ps(_mm_loadu_ps(v.acc.f), _mm_mul_ps(a, b)); break;
        case 4: r = _mm_mul_ps(a, b); break;
        case 5: r = _mm_max_ps(b, a); break; // std::max(a, b): a < b ? b : a
        default: r = _mm_min_ps(b, a); break; // std::min(a, b): b < a ? b : a
        }
        r = _mm_and_ps(r, _mm_cmpord_ps(r, r));
        r = _mm_min_ps(r, _mm_set1_ps(kFloatMax));
        return _mm_max_ps(r, _mm_set1_ps(-kFloatMax));
    }

    // The arithmetic's result written (dest's lanes of fd, or ACC). `flags`: the MAC flags and STATUS's two bits from
    // it (a compiled program leaves them out where nothing reads them before the next result sets them).
    SHINO_VU_INLINE void commitArith(Unit &v, bool toAcc, uint32_t fd, uint32_t dest, bool flags, __m128 r)
    {
        if (!toAcc && fd == 0)
            return; // vf0: no write, no flags
        float *dst = toAcc ? v.acc.f : v.vf[fd].f;
        const __m128 mask = lanes(dest);
        _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(mask, r), _mm_andnot_ps(mask, _mm_loadu_ps(dst))));
        if (flags)
        {
            const uint32_t on = static_cast<uint32_t>(_mm_movemask_ps(mask));
            const uint32_t zero = static_cast<uint32_t>(_mm_movemask_ps(_mm_cmpeq_ps(r, _mm_setzero_ps()))) & on;
            const uint32_t sign = static_cast<uint32_t>(_mm_movemask_ps(r)) & on;
            const uint16_t mac = static_cast<uint16_t>(g_lanes.rev[zero] | (g_lanes.rev[sign] << 4));
            v.mac = mac;
            v.status = (v.status & ~3u) | ((mac & 0xF) ? 1u : 0u) | ((mac & 0xF0) ? 2u : 0u);
        }
    }

    // ---- The lower instructions (as the interpreter's lowerFast does them) --------------------------------------
    SHINO_VU_INLINE uint32_t vi(const Unit &v, uint32_t r) { return v.vi[r & 15u]; }
    SHINO_VU_INLINE void setVi(Unit &v, uint32_t r, uint32_t value)
    {
        if (r & 15u)
            v.vi[r & 15u] = static_cast<uint16_t>(value);
    }
    SHINO_VU_INLINE __m128 vf(const Unit &v, uint32_t r) { return _mm_loadu_ps(v.vf[r].f); }
    SHINO_VU_INLINE void setVf(Unit &v, uint32_t r, uint32_t dest, __m128 value)
    {
        if (r == 0)
            return;
        const __m128 mask = lanes(dest);
        _mm_storeu_ps(v.vf[r].f, _mm_or_ps(_mm_and_ps(mask, value), _mm_andnot_ps(mask, _mm_loadu_ps(v.vf[r].f))));
    }
    SHINO_VU_INLINE __m128 load(Unit &v, uint32_t qw) { return _mm_loadu_ps(memVec(v, qw).f); }
    SHINO_VU_INLINE void store(Unit &v, uint32_t qw, uint32_t r, uint32_t dest)
    {
        float *m = memVec(v, qw).f;
        const __m128 mask = lanes(dest);
        _mm_storeu_ps(m, _mm_or_ps(_mm_and_ps(mask, _mm_loadu_ps(v.vf[r].f)), _mm_andnot_ps(mask, _mm_loadu_ps(m))));
    }
    SHINO_VU_INLINE __m128 mr32(const Unit &v, uint32_t r)
    {
        return _mm_shuffle_ps(_mm_loadu_ps(v.vf[r].f), _mm_loadu_ps(v.vf[r].f), _MM_SHUFFLE(0, 3, 2, 1));
    }
    // ILW / ILWR: the first field dest names; ISW / ISWR: every field it names.
    SHINO_VU_INLINE void ilw(Unit &v, uint32_t it, uint32_t qw, uint32_t dest)
    {
        for (int f = 0; f < 4; ++f)
            if (has(dest, f))
            {
                setVi(v, it, memWord(v, qw, f));
                break;
            }
    }
    SHINO_VU_INLINE void isw(Unit &v, uint32_t it, uint32_t qw, uint32_t dest)
    {
        for (int f = 0; f < 4; ++f)
            if (has(dest, f))
                memWord(v, qw, f) = vi(v, it);
    }
    SHINO_VU_INLINE void mtir(Unit &v, uint32_t it, uint32_t is, uint32_t fsf) { setVi(v, it, asU(v.vf[is].f[fsf])); }
    SHINO_VU_INLINE void mfir(Unit &v, uint32_t it, uint32_t is, uint32_t dest)
    {
        setVf(v, it, dest, _mm_set1_ps(asF(static_cast<uint32_t>(static_cast<int32_t>(s16(vi(v, is)))))));
    }
    // DIV (0), SQRT (1), RSQRT (2): Q's result lands 7 pairs later (13 for RSQRT), or at WAITQ.
    SHINO_VU_INLINE void divide(Unit &v, int kind, uint32_t is, uint32_t fsf, uint32_t it, uint32_t ftf)
    {
        const float s = v.vf[is].f[fsf], t = v.vf[it].f[ftf];
        float r;
        if (kind == 0)
            r = t != 0.0f ? s / t : (std::signbit(s) != std::signbit(t) ? -kFloatMax : kFloatMax);
        else if (kind == 1)
            r = std::sqrt(std::fabs(t));
        else
            r = t != 0.0f ? s / std::sqrt(std::fabs(t)) : kFloatMax;
        v.pendingQ = clampF(r);
        v.qDelay = kind == 2 ? 13 : 7;
    }
    SHINO_VU_INLINE void waitQ(Unit &v)
    {
        if (v.qDelay)
        {
            v.q = v.pendingQ;
            v.qDelay = 0;
        }
    }
    // At each pair's start: Q's pending result counts down and lands.
    SHINO_VU_INLINE void qTick(Unit &v)
    {
        if (v.qDelay && --v.qDelay == 0)
            v.q = v.pendingQ;
    }
    // At each pair's end: its MAC flags into the history FMAND / FMEQ / FMOR read 4 pairs later.
    SHINO_VU_INLINE void macPush(Unit &v) { v.macHistory[v.macAt++ & 3u] = v.mac; }

    // ---- What a compiled program hands to the interpreter (runtime/shino_vu1.cpp), on the unit running ----------
    struct UpperResult
    {
        bool writes = false, toAcc = false, setsMac = false, clip = false;
        uint32_t reg = 0, dest = 0;
        Vec value;
        uint32_t clipValue = 0;
    };
    void upperSlow(uint32_t up, UpperResult &out);  // the interpreter's upper(): ITOF, FTOI, ABS, CLIP, OPMULA...
    void commitSlow(const UpperResult &u);          // and its commit, after the lower instruction
    void lowerSlow(uint32_t lo, uint32_t pc);       // the interpreter's lower(): XGKICK, the flags, the EFU...
    uint32_t top();                                 // XTOP: the VIF's double buffer, the one the program reads
    uint32_t itop();                                // XITOP

    // ---- The compiled programs ----------------------------------------------------------------------------------
    // A compiled program set (an "image": the code one microcode packet of the executable loads) runs from a pc of its
    // own: it returns kEnd at the program's end (TPC set), a pc to go on at elsewhere (out of the image, or a part of it
    // whose code is no longer loaded), or pc | kInterpret for the interpreter to go on at that pair.
    constexpr uint32_t kEnd = 0xFFFFFFFFu, kInterpret = 0x80000000u;
    using ImageFn = uint32_t (*)(Unit &v, uint32_t pc);
    struct Image
    {
        uint32_t start, count;  // pairs of code memory
        const uint32_t *code;   // its words (lower, upper by pair)
        ImageFn run;
        uint32_t source;        // the packet's EE address
    };
    extern const Image kImages[];
    extern const uint32_t kImageCount;

    // Whether an image's code is still what is loaded in a segment (64 pairs) of code memory: checked again only
    // after an MPG changed the segment.
    constexpr uint32_t kSegmentPairs = 64, kSegments = 2048 / kSegmentPairs;
    struct SegmentCheck
    {
        uint32_t version = ~0u;
        bool ok = false;
    };
    extern uint32_t g_segmentVersion[kSegments];
    extern SegmentCheck *g_segmentChecks; // kImageCount x kSegments
    bool recheckSegment(uint32_t image, uint32_t segment);
    SHINO_VU_INLINE bool segmentOk(uint32_t image, uint32_t segment)
    {
        const SegmentCheck &c = g_segmentChecks[image * kSegments + segment];
        return c.version == g_segmentVersion[segment] ? c.ok : recheckSegment(image, segment);
    }
} // namespace shino::vu
