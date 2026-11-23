// Shinobido's VIF1 codes and VU1 microprograms, run natively.
//
// Bring-up path (docs/RECOMP_NOTES.md, "Drawing"): the game draws nearly everything, its 2D included, with VU1
// programs it uploads (MPG) and feeds (UNPACK) through VIF1, which send their GS packets with XGKICK. Until those draws
// are answered at the game's own functions, the VIF1 stream is read here: MPG fills the micro memory, UNPACK the data
// memory (STCYCL, STMOD, STMASK, STROW / STCOL, the TOPS double buffer), MSCAL / MSCALF / MSCNT run the program at
// once, DIRECT goes to the GIF (PATH2) and XGKICK's packets too (PATH1).
//
// The VU1: 32 vector, 16 integer registers, ACC, Q, P, I, R; one pair a step, the lower instruction reading its
// operands before the upper's result is committed; Q lands 7 pairs after DIV / SQRT (13 for RSQRT) or at WAITQ; the
// MAC flags FMAND / FMEQ / FMOR read are those of 4 pairs earlier; branches have a delay slot; the E bit ends the
// program after the next pair. Floats are the PC's (no PS2 overflow rules beyond clamping to the largest finite).
#include "shino_gs.h"
#include "shino_port.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace shino
{
    namespace
    {
        // ---- The VU1 ----------------------------------------------------------------------------------------------
        struct Vec
        {
            float f[4] = {0, 0, 0, 0}; // x y z w
        };
        struct Vu1
        {
            Vu1() { vf[0].f[3] = 1.0f; } // vf0 is the constant (0, 0, 0, 1): writes to it are dropped
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
        } g_vu;

        uint32_t g_programStart = 0;
        bool g_traceThis = false; // SHINO_TRACE_PROGRAM=<flip>:<n>: one program instance stepped pair by pair
        constexpr float kFloatMax = 3.40282347e38f;
        float clampF(float v)
        {
            if (std::isnan(v))
                return 0.0f;
            if (v > kFloatMax)
                return kFloatMax;
            if (v < -kFloatMax)
                return -kFloatMax;
            return v;
        }
        Vec &memVec(uint32_t qw) { return *reinterpret_cast<Vec *>(g_vu.mem + 16 * (qw & 0x3FFu)); }
        uint32_t &memWord(uint32_t qw, int field) { return reinterpret_cast<uint32_t *>(g_vu.mem + 16 * (qw & 0x3FFu))[field]; }
        float asF(uint32_t v) { return std::bit_cast<float>(v); }
        uint32_t asU(float v) { return std::bit_cast<uint32_t>(v); }
        int16_t s16(uint32_t v) { return static_cast<int16_t>(v); }

        // dest bits: x = 8, y = 4, z = 2, w = 1 (instruction bits 24..21)
        bool has(uint32_t dest, int field) { return (dest >> (3 - field)) & 1u; }

        struct UpperResult
        {
            bool writes = false, toAcc = false, setsMac = false, clip = false;
            uint32_t reg = 0, dest = 0;
            Vec value;
            uint32_t clipValue = 0;
        };

        void upper(uint32_t w, UpperResult &out)
        {
            Vu1 &v = g_vu;
            const uint32_t dest = (w >> 21) & 0xFu, ft = (w >> 16) & 31u, fs = (w >> 11) & 31u, fd = (w >> 6) & 31u;
            const uint32_t op = w & 0x3Fu, bc = w & 3u;
            const Vec &S = v.vf[fs], &T = v.vf[ft];
            out.dest = dest;
            auto operand = [&](int kind, int f) -> float { // 0 ft, 1 broadcast, 2 q, 3 i
                switch (kind)
                {
                case 0: return T.f[f];
                case 1: return T.f[bc];
                case 2: return v.q;
                default: return v.i;
                }
            };
            auto arith = [&](int fn, int kind, bool toAcc, uint32_t reg) { // fn: 0 add 1 sub 2 madd 3 msub 4 mul 5 max 6 min
                out.writes = true;
                out.toAcc = toAcc;
                out.reg = reg;
                out.setsMac = fn <= 4;
                for (int f = 0; f < 4; ++f)
                {
                    const float a = S.f[f], b = operand(kind, f);
                    float r = 0;
                    switch (fn)
                    {
                    case 0: r = a + b; break;
                    case 1: r = a - b; break;
                    case 2: r = v.acc.f[f] + a * b; break;
                    case 3: r = v.acc.f[f] - a * b; break;
                    case 4: r = a * b; break;
                    case 5: r = std::max(a, b); break;
                    default: r = std::min(a, b); break;
                    }
                    out.value.f[f] = clampF(r);
                }
            };
            if (op < 0x3C)
            {
                switch (op)
                {
                case 0x00: case 0x01: case 0x02: case 0x03: arith(0, 1, false, fd); return;
                case 0x04: case 0x05: case 0x06: case 0x07: arith(1, 1, false, fd); return;
                case 0x08: case 0x09: case 0x0A: case 0x0B: arith(2, 1, false, fd); return;
                case 0x0C: case 0x0D: case 0x0E: case 0x0F: arith(3, 1, false, fd); return;
                case 0x10: case 0x11: case 0x12: case 0x13: arith(5, 1, false, fd); return;
                case 0x14: case 0x15: case 0x16: case 0x17: arith(6, 1, false, fd); return;
                case 0x18: case 0x19: case 0x1A: case 0x1B: arith(4, 1, false, fd); return;
                case 0x1C: arith(4, 2, false, fd); return; // MULq
                case 0x1D: arith(5, 3, false, fd); return; // MAXi
                case 0x1E: arith(4, 3, false, fd); return; // MULi
                case 0x1F: arith(6, 3, false, fd); return; // MINIi
                case 0x20: arith(0, 2, false, fd); return; // ADDq
                case 0x21: arith(2, 2, false, fd); return; // MADDq
                case 0x22: arith(0, 3, false, fd); return; // ADDi
                case 0x23: arith(2, 3, false, fd); return; // MADDi
                case 0x24: arith(1, 2, false, fd); return; // SUBq
                case 0x25: arith(3, 2, false, fd); return; // MSUBq
                case 0x26: arith(1, 3, false, fd); return; // SUBi
                case 0x27: arith(3, 3, false, fd); return; // MSUBi
                case 0x28: arith(0, 0, false, fd); return; // ADD
                case 0x29: arith(2, 0, false, fd); return; // MADD
                case 0x2A: arith(4, 0, false, fd); return; // MUL
                case 0x2B: arith(5, 0, false, fd); return; // MAX
                case 0x2C: arith(1, 0, false, fd); return; // SUB
                case 0x2D: arith(3, 0, false, fd); return; // MSUB
                case 0x2E: // OPMSUB: fd = ACC - fs.yzx * ft.zxy
                    out.writes = true; out.reg = fd; out.setsMac = true;
                    out.value.f[0] = clampF(v.acc.f[0] - S.f[1] * T.f[2]);
                    out.value.f[1] = clampF(v.acc.f[1] - S.f[2] * T.f[0]);
                    out.value.f[2] = clampF(v.acc.f[2] - S.f[0] * T.f[1]);
                    out.value.f[3] = v.vf[fd].f[3];
                    return;
                case 0x2F: arith(6, 0, false, fd); return; // MINI
                default: ++v.refused; return;
                }
            }
            const uint32_t sp = ((w >> 6) & 0x1Fu) << 2 | (w & 3u);
            switch (sp)
            {
            case 0x00: case 0x01: case 0x02: case 0x03: arith(0, 1, true, 0); return;
            case 0x04: case 0x05: case 0x06: case 0x07: arith(1, 1, true, 0); return;
            case 0x08: case 0x09: case 0x0A: case 0x0B: arith(2, 1, true, 0); return;
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: arith(3, 1, true, 0); return;
            case 0x10: case 0x11: case 0x12: case 0x13: // ITOF0/4/12/15: ft = float(fs) / 2^n
            {
                static const float scale[4] = {1.0f, 1.0f / 16.0f, 1.0f / 4096.0f, 1.0f / 32768.0f};
                out.writes = true; out.reg = ft;
                for (int f = 0; f < 4; ++f)
                    out.value.f[f] = float(static_cast<int32_t>(asU(S.f[f]))) * scale[sp - 0x10];
                return;
            }
            case 0x14: case 0x15: case 0x16: case 0x17: // FTOI0/4/12/15
            {
                static const float scale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
                out.writes = true; out.reg = ft;
                for (int f = 0; f < 4; ++f)
                {
                    const double d = double(S.f[f]) * scale[sp - 0x14];
                    const int32_t n = d >= 2147483647.0 ? INT32_MAX : d <= -2147483648.0 ? INT32_MIN : static_cast<int32_t>(d);
                    out.value.f[f] = asF(static_cast<uint32_t>(n));
                }
                return;
            }
            case 0x18: case 0x19: case 0x1A: case 0x1B: arith(4, 1, true, 0); return;
            case 0x1C: arith(4, 2, true, 0); return; // MULAq
            case 0x1D: // ABS
                out.writes = true; out.reg = ft;
                for (int f = 0; f < 4; ++f)
                    out.value.f[f] = std::fabs(S.f[f]);
                return;
            case 0x1E: arith(4, 3, true, 0); return; // MULAi
            case 0x1F: // CLIP: fs.xyz against ft.w
            {
                const float wv = std::fabs(T.f[3]);
                uint32_t bits = 0;
                if (S.f[0] > wv) bits |= 1; if (S.f[0] < -wv) bits |= 2;
                if (S.f[1] > wv) bits |= 4; if (S.f[1] < -wv) bits |= 8;
                if (S.f[2] > wv) bits |= 16; if (S.f[2] < -wv) bits |= 32;
                out.clip = true;
                out.clipValue = ((v.clip << 6) | bits) & 0xFFFFFFu;
                return;
            }
            case 0x20: arith(0, 2, true, 0); return; // ADDAq
            case 0x21: arith(2, 2, true, 0); return; // MADDAq
            case 0x22: arith(0, 3, true, 0); return; // ADDAi
            case 0x23: arith(2, 3, true, 0); return; // MADDAi
            case 0x24: arith(1, 2, true, 0); return; // SUBAq
            case 0x25: arith(3, 2, true, 0); return; // MSUBAq
            case 0x26: arith(1, 3, true, 0); return; // SUBAi
            case 0x27: arith(3, 3, true, 0); return; // MSUBAi
            case 0x28: arith(0, 0, true, 0); return; // ADDA
            case 0x29: arith(2, 0, true, 0); return; // MADDA
            case 0x2A: arith(4, 0, true, 0); return; // MULA
            case 0x2C: arith(1, 0, true, 0); return; // SUBA
            case 0x2D: arith(3, 0, true, 0); return; // MSUBA
            case 0x2E: // OPMULA: ACC = fs.yzx * ft.zxy
                out.writes = true; out.toAcc = true; out.setsMac = true;
                out.value.f[0] = clampF(S.f[1] * T.f[2]);
                out.value.f[1] = clampF(S.f[2] * T.f[0]);
                out.value.f[2] = clampF(S.f[0] * T.f[1]);
                out.value.f[3] = v.acc.f[3];
                out.dest = 0xE;
                return;
            case 0x2F: return; // NOP
            default: ++v.refused; return;
            }
        }

        void commitUpper(const UpperResult &u)
        {
            Vu1 &v = g_vu;
            if (u.clip)
                v.clip = u.clipValue;
            if (!u.writes)
                return;
            Vec &d = u.toAcc ? v.acc : v.vf[u.reg];
            if (!u.toAcc && u.reg == 0)
                return;
            uint16_t mac = 0;
            for (int f = 0; f < 4; ++f)
                if (has(u.dest, f))
                {
                    d.f[f] = u.value.f[f];
                    if (u.setsMac)
                    {
                        if (u.value.f[f] == 0.0f) mac |= 1u << (3 - f);
                        if (std::signbit(u.value.f[f])) mac |= 1u << (7 - f);
                    }
                }
            if (u.setsMac)
            {
                v.mac = mac;
                v.status = (v.status & ~3u) | ((mac & 0xF) ? 1u : 0u) | ((mac & 0xF0) ? 2u : 0u);
            }
        }

        float efuField(const Vec &s, uint32_t w) { return s.f[(w >> 21) & 3u]; }

        // The lower instruction of a pair. Returns a branch target (byte address) or -1; `end` for XGKICK etc.
        int64_t lower(uint32_t w, uint32_t pc, bool &loi)
        {
            Vu1 &v = g_vu;
            const uint32_t dest = (w >> 21) & 0xFu, it = (w >> 16) & 31u, is = (w >> 11) & 31u, id = (w >> 6) & 31u;
            const int32_t imm11 = static_cast<int32_t>(w << 21) >> 21;
            auto setVi = [&](uint32_t r, uint32_t value) { if (r & 15u) v.vi[r & 15u] = static_cast<uint16_t>(value); };
            auto vi = [&](uint32_t r) -> uint32_t { return v.vi[r & 15u]; };
            auto setVf = [&](uint32_t r, const Vec &value) {
                if (r == 0) return;
                for (int f = 0; f < 4; ++f)
                    if (has(dest, f))
                        v.vf[r].f[f] = value.f[f];
            };
            const int64_t target = static_cast<int64_t>(pc) + 8 + static_cast<int64_t>(imm11) * 8;
            if (loi)
                return -1; // the lower word is I's value (LOI), not an instruction
            if (w >> 31)
            {
                const uint32_t op = w & 0x3Fu;
                switch (op)
                {
                case 0x30: setVi(id, vi(is) + vi(it)); return -1;                      // IADD
                case 0x31: setVi(id, vi(is) - vi(it)); return -1;                      // ISUB
                case 0x32: setVi(it, vi(is) + (static_cast<int32_t>(w << 21) >> 27)); return -1; // IADDI imm5 (bits 6-10)
                case 0x34: setVi(id, vi(is) & vi(it)); return -1;                      // IAND
                case 0x35: setVi(id, vi(is) | vi(it)); return -1;                      // IOR
                default: break;
                }
                if (op < 0x3C)
                {
                    ++v.refused;
                    return -1;
                }
                const uint32_t sp = ((w >> 6) & 0x1Fu) << 2 | (w & 3u);
                switch (sp)
                {
                case 0x30: setVf(it, v.vf[is]); return -1; // MOVE
                case 0x31: { Vec r; r.f[0] = v.vf[is].f[1]; r.f[1] = v.vf[is].f[2]; r.f[2] = v.vf[is].f[3]; r.f[3] = v.vf[is].f[0]; setVf(it, r); return -1; } // MR32
                case 0x34: setVf(it, memVec(vi(is))); setVi(is, vi(is) + 1); return -1; // LQI
                case 0x35: { Vec &m = memVec(vi(it)); for (int f = 0; f < 4; ++f) if (has(dest, f)) m.f[f] = v.vf[is].f[f]; setVi(it, vi(it) + 1); return -1; } // SQI
                case 0x36: setVi(is, vi(is) - 1); setVf(it, memVec(vi(is))); return -1; // LQD
                case 0x37: { setVi(it, vi(it) - 1); Vec &m = memVec(vi(it)); for (int f = 0; f < 4; ++f) if (has(dest, f)) m.f[f] = v.vf[is].f[f]; return -1; } // SQD
                case 0x38: case 0x39: case 0x3A: // DIV, SQRT, RSQRT
                {
                    const float s = v.vf[is].f[(w >> 21) & 3u], t = v.vf[it].f[(w >> 23) & 3u];
                    float r;
                    if (sp == 0x38)
                        r = t != 0.0f ? s / t : (std::signbit(s) != std::signbit(t) ? -kFloatMax : kFloatMax);
                    else if (sp == 0x39)
                        r = std::sqrt(std::fabs(t));
                    else
                        r = t != 0.0f ? s / std::sqrt(std::fabs(t)) : kFloatMax;
                    v.pendingQ = clampF(r);
                    v.qDelay = sp == 0x3A ? 13 : 7;
                    return -1;
                }
                case 0x3B: if (v.qDelay) { v.q = v.pendingQ; v.qDelay = 0; } return -1; // WAITQ
                case 0x3C: setVi(it, asU(v.vf[is].f[(w >> 21) & 3u])); return -1;      // MTIR
                case 0x3D: { Vec r; const float x = asF(static_cast<uint32_t>(static_cast<int32_t>(s16(vi(is))))); for (float &f : r.f) f = x; setVf(it, r); return -1; } // MFIR
                case 0x3E: { for (int f = 0; f < 4; ++f) if (has(dest, f)) { setVi(it, memWord(vi(is), f)); break; } return -1; } // ILWR
                case 0x3F: { for (int f = 0; f < 4; ++f) if (has(dest, f)) memWord(vi(is), f) = vi(it); return -1; } // ISWR
                case 0x40: case 0x41: case 0x42: case 0x43: // RNEXT, RGET, RINIT, RXOR
                {
                    if (sp == 0x42) v.r = 0x3F800000u | (asU(v.vf[is].f[(w >> 21) & 3u]) & 0x7FFFFFu);
                    else if (sp == 0x43) v.r = 0x3F800000u | ((v.r ^ asU(v.vf[is].f[(w >> 21) & 3u])) & 0x7FFFFFu);
                    else
                    {
                        if (sp == 0x40) { uint32_t x = v.r & 0x7FFFFFu; x = (x << 1) | (((x >> 4) ^ (x >> 22)) & 1u); v.r = 0x3F800000u | (x & 0x7FFFFFu); }
                        Vec r; for (float &f : r.f) f = asF(v.r); setVf(it, r);
                    }
                    return -1;
                }
                case 0x64: { Vec r; for (float &f : r.f) f = v.p; setVf(it, r); return -1; } // MFP
                case 0x68: case 0x69: return -1; // XTOP, XITOP: runProgram reads the double buffer for them
                case 0x6C: // XGKICK
                    ++v.kicks;
                    if (static const char *t = std::getenv("SHINO_TRACE_KICK"); t && gs::flips() == static_cast<uint64_t>(std::atoll(t)))
                    {
                        std::printf("[shino:vu1] kick at %03x from program %x (pc %x)\n", vi(is), g_programStart, pc);
                        for (uint32_t q = 0; q < 20; ++q)
                        {
                            const uint32_t *m = &memWord(vi(is) + q, 0);
                            std::printf("  %03x: %08x %08x %08x %08x\n", (vi(is) + q) & 0x3FFu, m[0], m[1], m[2], m[3]);
                        }
                    }
                    gs::path1(v.mem, vi(is));
                    return -1;
                case 0x70: { const Vec &s = v.vf[is]; v.p = s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]; return -1; } // ESADD
                case 0x71: { const Vec &s = v.vf[is]; const float d = s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]; v.p = d ? 1.0f / d : kFloatMax; return -1; }
                case 0x72: { const Vec &s = v.vf[is]; v.p = std::sqrt(s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]); return -1; }
                case 0x73: { const Vec &s = v.vf[is]; const float d = std::sqrt(s.f[0] * s.f[0] + s.f[1] * s.f[1] + s.f[2] * s.f[2]); v.p = d ? 1.0f / d : kFloatMax; return -1; }
                case 0x74: { const Vec &s = v.vf[is]; v.p = std::atan2(s.f[1], s.f[0]); return -1; } // EATANxy
                case 0x75: { const Vec &s = v.vf[is]; v.p = std::atan2(s.f[2], s.f[0]); return -1; } // EATANxz
                case 0x76: { const Vec &s = v.vf[is]; v.p = s.f[0] + s.f[1] + s.f[2] + s.f[3]; return -1; } // ESUM
                case 0x78: v.p = std::sqrt(std::fabs(efuField(v.vf[is], w))); return -1;
                case 0x79: { const float d = std::sqrt(std::fabs(efuField(v.vf[is], w))); v.p = d ? 1.0f / d : kFloatMax; return -1; }
                case 0x7A: { const float d = efuField(v.vf[is], w); v.p = d ? 1.0f / d : kFloatMax; return -1; }
                case 0x7B: return -1; // WAITP
                case 0x7C: v.p = std::sin(efuField(v.vf[is], w)); return -1;
                case 0x7D: v.p = std::atan(efuField(v.vf[is], w)); return -1;
                case 0x7E: v.p = std::exp(-efuField(v.vf[is], w)); return -1;
                default: ++v.refused; return -1;
                }
            }
            const uint32_t op = w >> 25;
            switch (op)
            {
            case 0x00: setVf(it, memVec(vi(is) + imm11)); return -1; // LQ
            case 0x01: { Vec &m = memVec(vi(it) + imm11); for (int f = 0; f < 4; ++f) if (has(dest, f)) m.f[f] = v.vf[is].f[f]; return -1; } // SQ
            case 0x04: { for (int f = 0; f < 4; ++f) if (has(dest, f)) { setVi(it, memWord(vi(is) + imm11, f)); break; } return -1; } // ILW
            case 0x05: { for (int f = 0; f < 4; ++f) if (has(dest, f)) memWord(vi(is) + imm11, f) = vi(it); return -1; } // ISW
            case 0x08: case 0x09: // IADDIU, ISUBIU (imm15)
            {
                const uint32_t imm15 = ((w >> 10) & 0x7800u) | (w & 0x7FFu);
                setVi(it, op == 0x08 ? vi(is) + imm15 : vi(is) - imm15);
                return -1;
            }
            case 0x10: v.vi[1] = (v.clip & 0xFFFFFFu) == (w & 0xFFFFFFu); return -1;                // FCEQ
            case 0x11: v.clip = w & 0xFFFFFFu; return -1;                                           // FCSET
            case 0x12: v.vi[1] = (v.clip & w & 0xFFFFFFu) != 0; return -1;                         // FCAND
            case 0x13: v.vi[1] = ((v.clip | w) & 0xFFFFFFu) == 0xFFFFFFu; return -1;               // FCOR
            case 0x14: case 0x15: case 0x16: case 0x17: // FSEQ, FSSET, FSAND, FSOR (imm12)
            {
                const uint32_t imm12 = ((w >> 10) & 0x800u) | (w & 0x7FFu);
                if (op == 0x14) setVi(it, (v.status & 0xFFFu) == imm12);
                else if (op == 0x15) v.status = (v.status & 0x3Fu) | (imm12 & 0xFC0u);
                else if (op == 0x16) setVi(it, v.status & imm12);
                else setVi(it, v.status | imm12);
                return -1;
            }
            case 0x18: case 0x1A: case 0x1B: // FMEQ, FMAND, FMOR: the MAC flags of 4 pairs earlier
            {
                const uint16_t mac = v.macHistory[v.macAt & 3u];
                if (op == 0x18) setVi(it, vi(is) == mac);
                else if (op == 0x1A) setVi(it, vi(is) & mac);
                else setVi(it, vi(is) | mac);
                return -1;
            }
            case 0x1C: setVi(it, v.clip & 0xFFFu); return -1; // FCGET
            case 0x20: return target;                         // B
            case 0x21: setVi(it, (pc + 16) / 8); return target; // BAL
            case 0x24: return static_cast<int64_t>(vi(is)) * 8; // JR
            case 0x25: { const uint32_t to = vi(is) * 8; setVi(it, (pc + 16) / 8); return to; } // JALR
            case 0x28: return vi(is) == vi(it) ? target : -1;  // IBEQ
            case 0x29: return vi(is) != vi(it) ? target : -1;  // IBNE
            case 0x2C: return s16(vi(is)) < 0 ? target : -1;   // IBLTZ
            case 0x2D: return s16(vi(is)) > 0 ? target : -1;   // IBGTZ
            case 0x2E: return s16(vi(is)) <= 0 ? target : -1;  // IBLEZ
            case 0x2F: return s16(vi(is)) >= 0 ? target : -1;  // IBGEZ
            default: ++v.refused; return -1;
            }
        }

        // ---- VIF1 -------------------------------------------------------------------------------------------------
        struct Vif
        {
            uint32_t cl = 1, wl = 1, mode = 0, mask = 0, row[4] = {}, col[4] = {};
            uint32_t base = 0, offset = 0, tops = 0, top = 0, itops = 0, itop = 0;
            bool dbf = false;
            uint32_t code = 0;          // the command whose data is being gathered
            std::vector<uint8_t> data;  // its data so far
            uint32_t need = 0;          // bytes it needs
            std::map<uint32_t, uint64_t> counts;
        } g_vif;

        void runProgram(uint32_t pcBytes)
        {
            Vu1 &v = g_vu;
            g_programStart = pcBytes;
            {
                static int count = -1;
                static uint64_t at = ~0ull;
                static int which = -1;
                if (const char *tp = std::getenv("SHINO_TRACE_PROGRAM"); tp && which < 0)
                {
                    at = std::strtoull(tp, nullptr, 10);
                    const char *c = std::strchr(tp, ':');
                    which = c ? std::atoi(c + 1) : 0;
                }
                if (gs::flips() == at)
                    ++count;
                g_traceThis = gs::flips() == at && count == which;
            }
            ++v.programs;
            // TOP: the buffer the program reads; the next uses the other (BASE, BASE + OFFSET).
            v.vi[0] = 0;
            uint32_t pc = pcBytes & 0x3FF8u;
            int64_t branch = -1;
            bool ending = false;
            for (int guard = 0; guard < 1 << 22; ++guard)
            {
                const uint32_t lo = v.micro[(pc >> 2) & 0xFFEu], up = v.micro[((pc >> 2) & 0xFFEu) + 1];
                ++v.pairs;
                // Q's pending result lands
                if (v.qDelay && --v.qDelay == 0)
                    v.q = v.pendingQ;
                UpperResult u;
                upper(up, u);
                bool loi = (up >> 31) & 1u;
                // XTOP / XITOP read the double buffer
                if ((lo >> 31) && (((lo >> 6) & 0x1Fu) << 2 | (lo & 3u)) == 0x68 && (lo & 0x3Fu) >= 0x3C)
                {
                    const uint32_t it = (lo >> 16) & 15u;
                    if (it) v.vi[it] = static_cast<uint16_t>(g_vif.top);
                }
                else if ((lo >> 31) && (((lo >> 6) & 0x1Fu) << 2 | (lo & 3u)) == 0x69 && (lo & 0x3Fu) >= 0x3C)
                {
                    const uint32_t it = (lo >> 16) & 15u;
                    if (it) v.vi[it] = static_cast<uint16_t>(g_vif.itop);
                }
                const int64_t to = lower(lo, pc, loi);
                commitUpper(u);
                v.macHistory[v.macAt++ & 3u] = v.mac;
                if (loi)
                    v.i = asF(lo);
                v.vi[0] = 0;
                if (g_traceThis)
                {
                    std::printf("[shino:step] %04x %08x %08x |", pc, lo, up);
                    if (u.writes && !u.toAcc)
                        std::printf(" vf%u=(%g %g %g %g)", u.reg, v.vf[u.reg].f[0], v.vf[u.reg].f[1], v.vf[u.reg].f[2], v.vf[u.reg].f[3]);
                    if (u.writes && u.toAcc)
                        std::printf(" acc=(%g %g %g %g)", v.acc.f[0], v.acc.f[1], v.acc.f[2], v.acc.f[3]);
                    if (u.clip)
                        std::printf(" clip=%06x", v.clip);
                    const uint32_t lit = (lo >> 16) & 31u;
                    std::printf(" | lo-it vf%u=(%g %g %g %g) vi%u=%u vi1=%u Q=%g\n", lit, v.vf[lit].f[0], v.vf[lit].f[1],
                                v.vf[lit].f[2], v.vf[lit].f[3], lit & 15u, v.vi[lit & 15u], v.vi[1], v.q);
                }
                const bool endBit = (up >> 30) & 1u;
                if (ending)
                {
                    v.tpc = pc + 8;
                    return;
                }
                if (branch >= 0)
                {
                    pc = static_cast<uint32_t>(branch) & 0x3FF8u;
                    branch = -1;
                }
                else
                    pc = (pc + 8) & 0x3FF8u;
                if (to >= 0)
                    branch = to;
                if (endBit)
                    ending = true;
            }
            std::printf("[shino:vu1] program at %x did not end\n", pcBytes);
        }

        void startProgram(uint32_t pcBytes)
        {
            if (static const char *d = std::getenv("SHINO_DUMP_MICRO"); d && gs::flips() == static_cast<uint64_t>(std::atoll(d)))
            {
                static int n = 0;
                if (n < 12)
                {
                    char name[64];
                    std::snprintf(name, sizeof(name), "build/vu1_micro_%02d.bin", n);
                    if (FILE *f = std::fopen(name, "wb")) { std::fwrite(g_vu.micro, 4, 4096, f); std::fclose(f); }
                    std::snprintf(name, sizeof(name), "build/vu1_data_%02d.bin", n);
                    if (FILE *f = std::fopen(name, "wb")) { std::fwrite(g_vu.mem, 1, 16384, f); std::fclose(f); }
                    std::printf("[shino:vu1] dump %d: program at %x, top %x itop %x\n", n, pcBytes, g_vif.tops, g_vif.itops);
                    ++n;
                }
            }
            Vif &f = g_vif;
            f.top = f.tops;
            f.itop = f.itops;
            f.dbf = !f.dbf;
            f.tops = f.base + (f.dbf ? f.offset : 0);
            runProgram(pcBytes);
        }

        // UNPACK's element size in bits and count: vn (0 S, 1 V2, 2 V3, 3 V4), vl (0 32, 1 16, 2 8, 3 5-5-5-1).
        uint32_t unpackBytes(uint32_t code, uint32_t cl, uint32_t wl)
        {
            const uint32_t cmd = (code >> 24) & 0x7Fu, vn = (cmd >> 2) & 3u, vl = cmd & 3u;
            uint32_t num = (code >> 16) & 0xFFu;
            if (!num) num = 256;
            uint32_t reads = num;
            if (wl > cl) // filling: only CL of every WL vectors come from the data
                reads = (num / wl) * cl + std::min(num % wl, cl);
            const uint32_t bits = vl == 3 ? 16 : (32u >> vl) * (vn + 1u);
            return ((reads * bits + 31u) / 32u) * 4u;
        }

        void doUnpack(uint32_t code, const uint8_t *data)
        {
            Vif &f = g_vif;
            const uint32_t cmd = (code >> 24) & 0x7Fu, vn = (cmd >> 2) & 3u, vl = cmd & 3u, masked = (cmd >> 4) & 1u;
            const bool usn = (code >> 14) & 1u, flg = (code >> 15) & 1u;
            uint32_t num = (code >> 16) & 0xFFu;
            if (!num) num = 256;
            uint32_t addr = code & 0x3FFu;
            if (flg) addr += f.tops;
            const uint32_t cl = std::max(f.cl, 1u), wl = std::max(f.wl, 1u);
            uint32_t bitPos = 0;
            auto take = [&](uint32_t bits) -> uint32_t {
                uint32_t v = 0;
                std::memcpy(&v, data + bitPos / 8, bits / 8 ? bits / 8 : 1);
                bitPos += bits;
                if (bits < 32) v &= (1u << bits) - 1u;
                return v;
            };
            const uint32_t elemBits = vl == 3 ? 16 : 32u >> vl;
            for (uint32_t i = 0; i < num; ++i)
            {
                const uint32_t cycle = i % wl;
                const uint32_t dest = wl <= cl ? addr + (i / wl) * cl + cycle : addr + i;
                const bool fromData = wl <= cl || cycle < cl;
                uint32_t in[4] = {0, 0, 0, 0};
                if (fromData)
                {
                    if (vl == 3)
                    {
                        const uint32_t c = take(16);
                        in[0] = (c & 31u) << 3; in[1] = ((c >> 5) & 31u) << 3; in[2] = ((c >> 10) & 31u) << 3; in[3] = ((c >> 15) & 1u) << 7;
                    }
                    else
                    {
                        const uint32_t n = vn + 1;
                        for (uint32_t e = 0; e < n; ++e)
                        {
                            uint32_t x = take(elemBits);
                            if (elemBits < 32 && !usn)
                                x = static_cast<uint32_t>(static_cast<int32_t>(x << (32 - elemBits)) >> (32 - elemBits));
                            in[e] = x;
                        }
                        if (vn == 0) in[1] = in[2] = in[3] = in[0];
                        else if (vn == 1) { in[2] = in[0]; in[3] = in[1]; }
                        else if (vn == 2) in[3] = in[2];
                    }
                }
                uint32_t *mem = &memWord(dest, 0);
                for (int e = 0; e < 4; ++e)
                {
                    uint32_t m = 0;
                    if (masked)
                        m = (f.mask >> (std::min(cycle, 3u) * 8 + e * 2)) & 3u;
                    else if (!fromData)
                        m = 1; // a filled vector with no mask: ROW (the hardware's undefined case)
                    uint32_t value = in[e];
                    switch (m)
                    {
                    case 0:
                        if (f.mode == 1) value += f.row[e];
                        else if (f.mode == 2) { value += f.row[e]; f.row[e] = value; }
                        mem[e] = value;
                        break;
                    case 1: mem[e] = f.row[e]; break;
                    case 2: mem[e] = f.col[std::min(cycle, 3u)]; break;
                    default: break; // write-protected
                    }
                }
            }
        }

        void execute(uint32_t code, const uint8_t *data)
        {
            Vif &f = g_vif;
            const uint32_t cmd = (code >> 24) & 0x7Fu, imm = code & 0xFFFFu;
            if (cmd >= 0x60)
            {
                doUnpack(code, data);
                return;
            }
            switch (cmd)
            {
            case 0x01: f.cl = imm & 0xFFu; f.wl = (imm >> 8) & 0xFFu; break;                 // STCYCL
            case 0x02: f.offset = imm & 0x3FFu; f.dbf = false; f.tops = f.base; break;      // OFFSET
            case 0x03: f.base = imm & 0x3FFu; break;                                        // BASE
            case 0x04: f.itops = imm & 0x3FFu; break;                                       // ITOP
            case 0x05: f.mode = imm & 3u; break;                                            // STMOD
            case 0x14: case 0x15: startProgram(imm * 8u); break;                            // MSCAL, MSCALF
            case 0x17: startProgram(g_vu.tpc); break;                                       // MSCNT
            case 0x20: std::memcpy(&f.mask, data, 4); break;                                // STMASK
            case 0x30: std::memcpy(f.row, data, 16); break;                                 // STROW
            case 0x31: std::memcpy(f.col, data, 16); break;                                 // STCOL
            case 0x4A: // MPG: NUM 64-bit words to micro memory at IMM (in pairs)
            {
                uint32_t num = (code >> 16) & 0xFFu;
                if (!num) num = 256;
                for (uint32_t k = 0; k < num; ++k)
                    std::memcpy(&g_vu.micro[((imm + k) & 0x7FFu) * 2], data + 8 * k, 8);
                break;
            }
            case 0x50: case 0x51: // DIRECT, DIRECTHL
                for (uint32_t at = 0; at + 16 <= f.need; at += 16)
                    gs::directQuadword(data + at);
                break;
            default: break; // NOP, MSKPATH3, MARK, FLUSHE, FLUSH, FLUSHA
            }
        }

        uint32_t dataBytes(uint32_t code)
        {
            const uint32_t cmd = (code >> 24) & 0x7Fu, imm = code & 0xFFFFu;
            if (cmd >= 0x60)
                return unpackBytes(code, std::max(g_vif.cl, 1u), std::max(g_vif.wl, 1u));
            switch (cmd)
            {
            case 0x20: return 4;
            case 0x30: case 0x31: return 16;
            case 0x4A: { const uint32_t num = (code >> 16) & 0xFFu; return (num ? num : 256u) * 8u; }
            case 0x50: case 0x51: return (imm ? imm : 0x10000u) * 16u;
            default: return 0;
            }
        }
    } // namespace

    void vif::codes(const uint8_t *data, uint32_t bytes, bool)
    {
        Vif &f = g_vif;
        for (uint32_t at = 0; at + 4 <= bytes;)
        {
            if (f.need)
            {
                const uint32_t n = std::min(f.need - static_cast<uint32_t>(f.data.size()), bytes - at);
                if (static const char *tr = std::getenv("SHINO_TRACE_VIF"); tr && gs::flips() == static_cast<uint64_t>(std::atoll(tr)) && f.need <= 64)
                    for (uint32_t k = 0; k + 4 <= n; k += 4)
                    {
                        uint32_t w;
                        std::memcpy(&w, data + at + k, 4);
                        std::printf("[shino:vif]   data %08x\n", w);
                    }
                f.data.insert(f.data.end(), data + at, data + at + n);
                at += n;
                if (f.data.size() == f.need)
                {
                    execute(f.code, f.data.data());
                    f.need = 0;
                    f.data.clear();
                }
                continue;
            }
            uint32_t code;
            std::memcpy(&code, data + at, 4);
            at += 4;
            const uint32_t cmd = (code >> 24) & 0x7Fu;
            ++f.counts[cmd >= 0x60 ? 0x60 : cmd];
            if (static const char *tr = std::getenv("SHINO_TRACE_VIF"); tr && gs::flips() == static_cast<uint64_t>(std::atoll(tr)))
                std::printf("[shino:vif] %08x cl=%u wl=%u tops=%x base=%x offset=%x mode=%u\n", code, f.cl, f.wl, f.tops, f.base,
                            f.offset, f.mode);
            f.code = code;
            f.need = dataBytes(code);
            // MPG's and DIRECT's data start on the next 64-bit / 128-bit boundary of the stream: their alignment
            // padding (VIF NOPs) is already in the stream as codes, so nothing to skip here.
            if (!f.need)
                execute(code, nullptr);
        }
    }

    void vif::report()
    {
        std::printf("[shino:vif]");
        for (const auto &[cmd, n] : g_vif.counts)
            std::printf(" %02x:%llu", cmd, (unsigned long long)n);
        std::printf("\n[shino:vu1] programs=%llu pairs=%llu kicks=%llu refused=%llu\n", (unsigned long long)g_vu.programs,
                    (unsigned long long)g_vu.pairs, (unsigned long long)g_vu.kicks, (unsigned long long)g_vu.refused);
    }
} // namespace shino
