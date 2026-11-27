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
#include "shino_native3d.h"
#include "shino_vu.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace shino
{
    namespace
    {
        // ---- The VU1 (runtime/shino_vu.h: the state and the operations the compiled programs share) ----------------
        using vu::asF;
        using vu::asU;
        using vu::clampF;
        using vu::g_lanes;
        using vu::has;
        using vu::kFloatMax;
        using vu::s16;
        using vu::UpperResult;
        using vu::Vec;
        using Vu1 = vu::Unit;
        Vu1 g_vu, g_vu0;
        Vu1 *g_cur = &g_vu; // the unit a program runs on

        uint32_t g_programStart = 0;
        bool g_traceThis = false; // SHINO_TRACE_PROGRAM=<flip>:<n>: one program instance stepped pair by pair
        bool g_noKick = false;    // SHINO_VU_VERIFY: the interpreter's run of a program already run compiled
        Vec &memVec(uint32_t qw) { return vu::memVec(*g_cur, qw); }
        uint32_t &memWord(uint32_t qw, int field) { return vu::memWord(*g_cur, qw, field); }

        void upper(uint32_t w, UpperResult &out)
        {
            Vu1 &v = *g_cur;
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
                out.writes = true; out.reg = ft;
                out.value = vu::itof(v, fs, static_cast<int>(sp - 0x10));
                return;
            case 0x14: case 0x15: case 0x16: case 0x17: // FTOI0/4/12/15
                out.writes = true; out.reg = ft;
                out.value = vu::ftoi(v, fs, static_cast<int>(sp - 0x14));
                return;
            case 0x18: case 0x19: case 0x1A: case 0x1B: arith(4, 1, true, 0); return;
            case 0x1C: arith(4, 2, true, 0); return; // MULAq
            case 0x1D: // ABS
                out.writes = true; out.reg = ft;
                out.value = vu::absv(v, fs);
                return;
            case 0x1E: arith(4, 3, true, 0); return; // MULAi
            case 0x1F: // CLIP: fs.xyz against ft.w
                out.clip = true;
                out.clipValue = vu::clipFlags(v, fs, ft);
                return;
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
            Vu1 &v = *g_cur;
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

        // ---- The upper instruction, decoded once (Decoded) and run on whole vectors -------------------------------
        // The arithmetic (ADD / SUB / MADD / MSUB / MUL / MAX / MINI and their broadcast, Q, I and ACC forms) and NOP;
        // the rest goes through upper() / commitUpper(). Same results as upper(): MAX / MINI as std::max / std::min
        // (operands swapped for SSE's), NaN 0, clamped to the largest finite.
        enum UpperKind : uint8_t { kUpperScalar, kUpperArith, kUpperNop };
        struct Decoded
        {
            uint32_t lo = 0, up = 0; // the pair it was decoded from (a new MPG makes it decode again)
            bool valid = false;
            uint8_t kind = kUpperScalar, fn = 0, operand = 0, dest = 0, fs = 0, ft = 0, fd = 0, bc = 0;
            bool toAcc = false, setsMac = false;
            uint8_t xtop = 0, xreg = 0; // 1 XTOP, 2 XITOP into vi[xreg]
            // The lower instruction (lowerFast; kLowerSlow: lower()): its kind, registers, dest, the FSF / FTF fields,
            // its immediate (imm5, imm11 or imm15, sign-extended where the instruction does) and branch target.
            uint8_t lk = 0, is = 0, it = 0, id = 0, ldest = 0, fsf = 0, ftf = 0;
            int32_t imm = 0;
            uint32_t target = 0;
        };
        std::array<Decoded, 2048> g_decoded, g_decoded0; // VU1's pairs, VU0's

        enum LowerKind : uint8_t
        {
            kLowerSlow, kLowerNone, kIadd, kIsub, kIaddi, kIand, kIor, kIaddiu, kIsubiu, kMove, kMr32, kLqi, kSqi, kLqd, kSqd,
            kLq, kSq, kIlw, kIsw, kIlwr, kIswr, kMtir, kMfir, kDiv, kSqrt, kRsqrt, kWaitq, kB, kBal, kJr, kJalr, kIbeq, kIbne,
            kIbltz, kIbgtz, kIblez, kIbgez,
        };

        void decodeLower(Decoded &d, uint32_t lo, uint32_t up, uint32_t pc, uint32_t pcMask)
        {
            d.is = (lo >> 11) & 31u;
            d.it = (lo >> 16) & 31u;
            d.id = (lo >> 6) & 31u;
            d.ldest = (lo >> 21) & 0xFu;
            d.fsf = (lo >> 21) & 3u;
            d.ftf = (lo >> 23) & 3u;
            const int32_t imm11 = static_cast<int32_t>(lo << 21) >> 21;
            d.target = static_cast<uint32_t>((static_cast<int64_t>(pc) + 8 + static_cast<int64_t>(imm11) * 8) & pcMask);
            d.lk = kLowerSlow;
            if ((up >> 31) & 1u)
            {
                d.lk = kLowerNone; // LOI: the lower word is I's value
                return;
            }
            if (lo >> 31)
            {
                switch (lo & 0x3Fu)
                {
                case 0x30: d.lk = kIadd; return;
                case 0x31: d.lk = kIsub; return;
                case 0x32: d.lk = kIaddi; d.imm = static_cast<int32_t>(lo << 21) >> 27; return;
                case 0x34: d.lk = kIand; return;
                case 0x35: d.lk = kIor; return;
                default: break;
                }
                if ((lo & 0x3Fu) < 0x3C)
                    return; // refused, counted by lower()
                switch (((lo >> 6) & 0x1Fu) << 2 | (lo & 3u))
                {
                case 0x30: d.lk = kMove; return;
                case 0x31: d.lk = kMr32; return;
                case 0x34: d.lk = kLqi; return;
                case 0x35: d.lk = kSqi; return;
                case 0x36: d.lk = kLqd; return;
                case 0x37: d.lk = kSqd; return;
                case 0x38: d.lk = kDiv; return;
                case 0x39: d.lk = kSqrt; return;
                case 0x3A: d.lk = kRsqrt; return;
                case 0x3B: d.lk = kWaitq; return;
                case 0x3C: d.lk = kMtir; return;
                case 0x3D: d.lk = kMfir; return;
                case 0x3E: d.lk = kIlwr; return;
                case 0x3F: d.lk = kIswr; return;
                case 0x68: case 0x69: d.lk = kLowerNone; return; // XTOP / XITOP: the loop
                default: return;
                }
            }
            switch (lo >> 25)
            {
            case 0x00: d.lk = kLq; d.imm = imm11; return;
            case 0x01: d.lk = kSq; d.imm = imm11; return;
            case 0x04: d.lk = kIlw; d.imm = imm11; return;
            case 0x05: d.lk = kIsw; d.imm = imm11; return;
            case 0x08: case 0x09:
                d.lk = (lo >> 25) == 0x08 ? kIaddiu : kIsubiu;
                d.imm = static_cast<int32_t>(((lo >> 10) & 0x7800u) | (lo & 0x7FFu));
                return;
            case 0x20: d.lk = kB; return;
            case 0x21: d.lk = kBal; return;
            case 0x24: d.lk = kJr; return;
            case 0x25: d.lk = kJalr; return;
            case 0x28: d.lk = kIbeq; return;
            case 0x29: d.lk = kIbne; return;
            case 0x2C: d.lk = kIbltz; return;
            case 0x2D: d.lk = kIbgtz; return;
            case 0x2E: d.lk = kIblez; return;
            case 0x2F: d.lk = kIbgez; return;
            default: return;
            }
        }

        Decoded decodePair(uint32_t lo, uint32_t up, uint32_t pc, uint32_t pcMask)
        {
            Decoded d;
            decodeLower(d, lo, up, pc, pcMask);
            d.lo = lo;
            d.up = up;
            d.valid = true;
            d.dest = (up >> 21) & 0xFu;
            d.ft = (up >> 16) & 31u;
            d.fs = (up >> 11) & 31u;
            d.fd = (up >> 6) & 31u;
            d.bc = up & 3u;
            // fn: 0 add 1 sub 2 madd 3 msub 4 mul 5 max 6 min; operand: 0 ft, 1 broadcast, 2 Q, 3 I
            auto arith = [&](int fn, int operand, bool toAcc) {
                d.kind = kUpperArith;
                d.fn = static_cast<uint8_t>(fn);
                d.operand = static_cast<uint8_t>(operand);
                d.toAcc = toAcc;
                d.setsMac = fn <= 4;
            };
            const uint32_t op = up & 0x3Fu;
            if (op < 0x3C)
            {
                static const int8_t table[0x30][2] = {
                    {0, 1}, {0, 1}, {0, 1}, {0, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {2, 1}, {2, 1}, {2, 1}, {2, 1},
                    {3, 1}, {3, 1}, {3, 1}, {3, 1}, {5, 1}, {5, 1}, {5, 1}, {5, 1}, {6, 1}, {6, 1}, {6, 1}, {6, 1},
                    {4, 1}, {4, 1}, {4, 1}, {4, 1}, {4, 2}, {5, 3}, {4, 3}, {6, 3}, {0, 2}, {2, 2}, {0, 3}, {2, 3},
                    {1, 2}, {3, 2}, {1, 3}, {3, 3}, {0, 0}, {2, 0}, {4, 0}, {5, 0}, {1, 0}, {3, 0}, {-1, 0}, {6, 0}};
                if (op < 0x30 && table[op][0] >= 0)
                    arith(table[op][0], table[op][1], false);
            }
            else
            {
                const uint32_t sp = ((up >> 6) & 0x1Fu) << 2 | (up & 3u);
                if (sp <= 0x0F)
                    arith(static_cast<int>(sp >> 2) /* add sub madd msub */, 1, true);
                else if (sp >= 0x18 && sp <= 0x1B)
                    arith(4, 1, true);
                else if (sp == 0x1C) arith(4, 2, true);
                else if (sp == 0x1E) arith(4, 3, true);
                else if (sp >= 0x20 && sp <= 0x27)
                {
                    static const int8_t accq[8][2] = {{0, 2}, {2, 2}, {0, 3}, {2, 3}, {1, 2}, {3, 2}, {1, 3}, {3, 3}};
                    arith(accq[sp - 0x20][0], accq[sp - 0x20][1], true);
                }
                else if (sp == 0x28) arith(0, 0, true);
                else if (sp == 0x29) arith(2, 0, true);
                else if (sp == 0x2A) arith(4, 0, true);
                else if (sp == 0x2C) arith(1, 0, true);
                else if (sp == 0x2D) arith(3, 0, true);
                else if (sp == 0x2F) d.kind = kUpperNop;
            }
            // XTOP / XITOP: the double buffer, read by the loop
            if ((lo >> 31) && (lo & 0x3Fu) >= 0x3C)
            {
                const uint32_t lsp = ((lo >> 6) & 0x1Fu) << 2 | (lo & 3u);
                if (lsp == 0x68 || lsp == 0x69)
                {
                    d.xtop = lsp == 0x68 ? 1 : 2;
                    d.xreg = (lo >> 16) & 15u;
                }
            }
            return d;
        }

        // The arithmetic's result from the registers before the pair's lower instruction.
        inline __m128 upperArith(const Decoded &d) { return vu::arith(*g_cur, d.fn, d.operand, d.fs, d.ft, d.bc); }
        inline void commitArith(const Decoded &d, __m128 r) { vu::commitArith(*g_cur, d.toAcc, d.fd, d.dest, d.setsMac, r); }

        float efuField(const Vec &s, uint32_t w) { return s.f[(w >> 21) & 3u]; }

        // The lower instruction of a pair. Returns a branch target (byte address) or -1; `end` for XGKICK etc.
        int64_t lower(uint32_t w, uint32_t pc, bool &loi)
        {
            Vu1 &v = *g_cur;
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
            // The branch target wraps around the code memory (Shinobido's 3D program branches back from 0x18f0 to
            // 0x3978): negative is "no branch" to the caller, so it is taken modulo the memory's size here.
            const int64_t target = (static_cast<int64_t>(pc) + 8 + static_cast<int64_t>(imm11) * 8) & g_cur->pcMask;
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
                case 0x6C: // XGKICK (vu::kick: VU1's PATH1, shared with the compiled programs)
                    if (v.vu0)
                        ++v.kicks;
                    else
                        vu::kick(vi(is), pc);
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

        // The lower instruction from its decoding: the common ones here (as lower() does them), the rest through lower().
        inline int64_t lowerFast(const Decoded &d, uint32_t lo, uint32_t pc, bool &loi)
        {
            Vu1 &v = *g_cur;
            auto vi = [&](uint32_t r) -> uint32_t { return v.vi[r & 15u]; };
            auto setVi = [&](uint32_t r, uint32_t value) { if (r & 15u) v.vi[r & 15u] = static_cast<uint16_t>(value); };
            auto setVf = [&](uint32_t r, __m128 value) {
                if (r == 0) return;
                const __m128 mask = g_lanes.m[d.ldest];
                _mm_storeu_ps(v.vf[r].f, _mm_or_ps(_mm_and_ps(mask, value), _mm_andnot_ps(mask, _mm_loadu_ps(v.vf[r].f))));
            };
            auto store = [&](uint32_t qw, uint32_t r) {
                float *m = memVec(qw).f;
                const __m128 mask = g_lanes.m[d.ldest];
                _mm_storeu_ps(m, _mm_or_ps(_mm_and_ps(mask, _mm_loadu_ps(v.vf[r].f)), _mm_andnot_ps(mask, _mm_loadu_ps(m))));
            };
            auto load = [&](uint32_t qw) { return _mm_loadu_ps(memVec(qw).f); };
            switch (d.lk)
            {
            case kLowerNone: return -1;
            case kIadd: setVi(d.id, vi(d.is) + vi(d.it)); return -1;
            case kIsub: setVi(d.id, vi(d.is) - vi(d.it)); return -1;
            case kIaddi: setVi(d.it, vi(d.is) + static_cast<uint32_t>(d.imm)); return -1;
            case kIand: setVi(d.id, vi(d.is) & vi(d.it)); return -1;
            case kIor: setVi(d.id, vi(d.is) | vi(d.it)); return -1;
            case kIaddiu: setVi(d.it, vi(d.is) + static_cast<uint32_t>(d.imm)); return -1;
            case kIsubiu: setVi(d.it, vi(d.is) - static_cast<uint32_t>(d.imm)); return -1;
            case kMove: setVf(d.it, _mm_loadu_ps(v.vf[d.is].f)); return -1;
            case kMr32: setVf(d.it, _mm_shuffle_ps(_mm_loadu_ps(v.vf[d.is].f), _mm_loadu_ps(v.vf[d.is].f), _MM_SHUFFLE(0, 3, 2, 1))); return -1;
            case kLqi: setVf(d.it, load(vi(d.is))); setVi(d.is, vi(d.is) + 1); return -1;
            case kSqi: store(vi(d.it), d.is); setVi(d.it, vi(d.it) + 1); return -1;
            case kLqd: setVi(d.is, vi(d.is) - 1); setVf(d.it, load(vi(d.is))); return -1;
            case kSqd: setVi(d.it, vi(d.it) - 1); store(vi(d.it), d.is); return -1;
            case kLq: setVf(d.it, load(vi(d.is) + static_cast<uint32_t>(d.imm))); return -1;
            case kSq: store(vi(d.it) + static_cast<uint32_t>(d.imm), d.is); return -1;
            case kIlw: case kIlwr:
            {
                const uint32_t qw = vi(d.is) + (d.lk == kIlw ? static_cast<uint32_t>(d.imm) : 0u);
                for (int f = 0; f < 4; ++f)
                    if (has(d.ldest, f))
                    {
                        setVi(d.it, memWord(qw, f));
                        break;
                    }
                return -1;
            }
            case kIsw: case kIswr:
            {
                const uint32_t qw = vi(d.is) + (d.lk == kIsw ? static_cast<uint32_t>(d.imm) : 0u);
                for (int f = 0; f < 4; ++f)
                    if (has(d.ldest, f))
                        memWord(qw, f) = vi(d.it);
                return -1;
            }
            case kMtir: setVi(d.it, asU(v.vf[d.is].f[d.fsf])); return -1;
            case kMfir: setVf(d.it, _mm_set1_ps(asF(static_cast<uint32_t>(static_cast<int32_t>(s16(vi(d.is))))))); return -1;
            case kDiv: case kSqrt: case kRsqrt:
            {
                const float s = v.vf[d.is].f[d.fsf], t = v.vf[d.it].f[d.ftf];
                float r;
                if (d.lk == kDiv)
                    r = t != 0.0f ? s / t : (std::signbit(s) != std::signbit(t) ? -kFloatMax : kFloatMax);
                else if (d.lk == kSqrt)
                    r = std::sqrt(std::fabs(t));
                else
                    r = t != 0.0f ? s / std::sqrt(std::fabs(t)) : kFloatMax;
                v.pendingQ = clampF(r);
                v.qDelay = d.lk == kRsqrt ? 13 : 7;
                return -1;
            }
            case kWaitq: if (v.qDelay) { v.q = v.pendingQ; v.qDelay = 0; } return -1;
            case kB: return static_cast<int64_t>(d.target);
            case kBal: setVi(d.it, (pc + 16) / 8); return static_cast<int64_t>(d.target);
            case kJr: return static_cast<int64_t>(vi(d.is)) * 8;
            case kJalr: { const uint32_t to = vi(d.is) * 8; setVi(d.it, (pc + 16) / 8); return to; }
            case kIbeq: return vi(d.is) == vi(d.it) ? static_cast<int64_t>(d.target) : int64_t(-1);
            case kIbne: return vi(d.is) != vi(d.it) ? static_cast<int64_t>(d.target) : int64_t(-1);
            case kIbltz: return s16(vi(d.is)) < 0 ? static_cast<int64_t>(d.target) : int64_t(-1);
            case kIbgtz: return s16(vi(d.is)) > 0 ? static_cast<int64_t>(d.target) : int64_t(-1);
            case kIblez: return s16(vi(d.is)) <= 0 ? static_cast<int64_t>(d.target) : int64_t(-1);
            case kIbgez: return s16(vi(d.is)) >= 0 ? static_cast<int64_t>(d.target) : int64_t(-1);
            default: return lower(lo, pc, loi);
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
            const uint8_t *from = nullptr; // where its data started (SHINO_VU_SURVEY)
            std::array<uint64_t, 128> counts{}; // by command (UNPACK all at 0x60)
        } g_vif, g_vif0;
        Vif *g_cv = &g_vif; // the VIF whose codes are being read

        // SHINO_VU_SURVEY=1: the MPG uploads (micro address, pairs, EE source; how many, how many changed the code) and
        // the programs run (start, code version: their runs and pairs), printed with the report.
        bool survey() { static const bool on = std::getenv("SHINO_VU_SURVEY") != nullptr; return on; }
        std::map<std::array<uint32_t, 3>, std::array<uint64_t, 2>> g_mpgs;
        std::array<uint32_t, 32> g_regionSrc{}; // the EE address each 64 pairs of code came from
        std::map<std::pair<uint32_t, std::array<uint32_t, 32>>, std::array<uint64_t, 2>> g_starts;
        uint64_t g_microVersion = 0;
        std::map<std::array<uint32_t, 1>, uint64_t> g_unpackForms;

        int g_inFlip = 0; // the program's index in its flip (SHINO_TRACE_PROGRAM's <n>)
        uint32_t interpret(uint32_t pc, bool rejoin = false);
        uint64_t g_interpreted = 0; // pairs the interpreter ran (SHINO_VU_REPORT: of the compiled programs)

        // ---- The compiled programs (runtime/shino_vu.h; tools/shino_vu1_recomp.py) ----------------------------------
        // A VU1 program runs compiled from the image whose code is what is loaded at its pc, image to image as it goes
        // where another's code is loaded, and in the interpreter from a pair no image compiled as an entry.
        // SHINO_VU_INTERP=1: the interpreter alone. SHINO_VU_VERIFY=1: each program run both ways, compared.
        bool compiledOn()
        {
            static const bool on = vu::kImageCount > 0 && !std::getenv("SHINO_VU_INTERP") && !std::getenv("SHINO_VU_SCALAR");
            return on;
        }
        struct CompiledStats
        {
            uint64_t ended = 0, handed = 0, noImage = 0, mismatches = 0, verified = 0;
            std::map<uint32_t, uint64_t> handedAt, noImageAt; // by pc
        } g_compiled;
        bool g_stopped = false;  // the compiled run ended at a pair of Unit::stopPairs
        uint32_t g_stoppedAt = 0; // its pc
        std::array<int, 2048> g_lastImage = [] { std::array<int, 2048> a; a.fill(-1); return a; }(),
                              g_lastImage0 = [] { std::array<int, 2048> a; a.fill(-1); return a; }(); // VU1's, VU0's
        std::vector<std::vector<uint32_t>> g_candidates; // by pair: the images that hold it

        // The image whose code is loaded at pc's segment (its pair first), the last one found there first.
        int findImage(uint32_t pc)
        {
            const Vu1 &u = *g_cur;
            const uint32_t pair = (pc >> 3) & (u.pcMask >> 3), segment = pair / vu::kSegmentPairs;
            int &last = (u.vu0 ? g_lastImage0 : g_lastImage)[pair];
            if (last >= 0 && vu::segmentOk(static_cast<uint32_t>(last), segment))
                return last;
            if (g_candidates.empty())
            {
                g_candidates.resize(2048);
                for (uint32_t id = 0; id < vu::kImageCount; ++id)
                    for (uint32_t k = 0; k < vu::kImages[id].count; ++k)
                        g_candidates[vu::kImages[id].start + k].push_back(id);
            }
            for (const uint32_t id : g_candidates[pair])
            {
                const vu::Image &im = vu::kImages[id];
                const uint32_t k = pair - im.start;
                if (im.start + im.count > (u.pcMask >> 3) + 1) // (VU0: an image wider than its code memory)
                    continue;
                if (im.code[2 * k] != u.micro[2 * pair] || im.code[2 * k + 1] != u.micro[2 * pair + 1])
                    continue;
                if (vu::segmentOk(id, segment))
                {
                    last = static_cast<int>(id);
                    return last;
                }
            }
            return -1;
        }

        // The segment tables of the unit compiled code runs on (VU0's programs run on VU1's compiled images: the code
        // VU0 loads, 0x4f2a70's 478 pairs, is one of the packets tools/shino_vu1_recomp.py finds).
        void selectUnit(const Vu1 &v)
        {
            vu::g_segmentVersionCur = v.vu0 ? vu::g_segmentVersion0 : vu::g_segmentVersion;
            vu::g_segmentChecksCur = v.vu0 ? vu::g_segmentChecks0 : vu::g_segmentChecks;
        }

        void compiledFrom(uint32_t pc)
        {
            Vu1 &v = *g_cur;
            selectUnit(v);
            for (int hops = 0; hops < 1 << 20; ++hops)
            {
                const int image = findImage(pc);
                uint32_t r;
                if (image < 0)
                {
                    ++g_compiled.noImage;
                    ++g_compiled.noImageAt[pc];
                    r = interpret(pc, true);
                }
                else
                {
                    r = vu::kImages[image].run(v, pc);
                    if (r != vu::kEnd && (r & vu::kInterpret))
                    {
                        ++g_compiled.handed;
                        ++g_compiled.handedAt[r & v.pcMask];
                        r = interpret(r & v.pcMask, true);
                    }
                }
                if (r == vu::kEnd)
                {
                    ++g_compiled.ended;
                    return;
                }
                if (!(r & vu::kInterpret) && v.stopPairs && v.stopPairs[(r & v.pcMask) >> 3]) // a native draw takes over
                {
                    g_stopped = true;
                    g_stoppedAt = r & v.pcMask;
                    return;
                }
                pc = r & v.pcMask;
            }
            std::printf("[shino:vu1] compiled program at %x did not end\n", g_programStart);
        }

        // The compiled run and the interpreter's from the same state: what they leave compared (the interpreter's kept).
        void verifyRun(uint32_t pc)
        {
            static Vu1 before, compiled;
            static std::vector<uint8_t> beforeData, compiledData;
            static vu::Vec beforeVf[32], compiledVf[32]; // (a copy of the unit shares its vf)
            Vu1 &unit = *g_cur;
            const size_t dataBytes = 16u * (unit.qwMask + 1u);
            before = unit;
            std::copy(unit.vf, unit.vf + 32, beforeVf);
            beforeData.assign(unit.data, unit.data + dataBytes);
            compiledFrom(pc);
            compiled = unit;
            std::copy(unit.vf, unit.vf + 32, compiledVf);
            compiledData.assign(unit.data, unit.data + dataBytes);
            unit = before;
            std::copy(beforeVf, beforeVf + 32, unit.vf);
            std::memcpy(unit.data, beforeData.data(), dataBytes);
            g_noKick = true;
            interpret(pc);
            g_noKick = false;
            ++g_compiled.verified;
            const Vu1 &a = compiled, &b = unit;
            if (std::memcmp(compiledData.data(), unit.data, dataBytes) != 0 && g_compiled.mismatches++ < 40)
                std::printf("[shino:verify] %s program %x: data memory differs%c", unit.vu0 ? "vu0" : "vu1", g_programStart, 10);
            std::string diff;
            auto check = [&](const char *name, const void *x, const void *y, size_t n) {
                if (std::memcmp(x, y, n) != 0)
                    diff += std::string(" ") + name;
            };
            for (int r = 0; r < 32; ++r)
                if (std::memcmp(compiledVf[r].f, b.vf[r].f, 16) != 0)
                    diff += " vf" + std::to_string(r);
            check("vi", a.vi, b.vi, sizeof(a.vi));
            check("acc", a.acc.f, b.acc.f, 16);
            check("q", &a.q, &b.q, 4);
            check("p", &a.p, &b.p, 4);
            check("i", &a.i, &b.i, 4);
            check("r", &a.r, &b.r, 4);
            check("clip", &a.clip, &b.clip, 4);
            check("status", &a.status, &b.status, 4);
            check("mac", &a.mac, &b.mac, 2);
            check("pendingQ", &a.pendingQ, &b.pendingQ, 4);
            check("qDelay", &a.qDelay, &b.qDelay, 4);
            check("tpc", &a.tpc, &b.tpc, 4);
            check("kicks", &a.kicks, &b.kicks, 8);
            check("mem", a.mem, b.mem, sizeof(a.mem));
            for (uint32_t k = 0; k < 4; ++k) // the history's last 4, oldest first
                if (a.macHistory[(a.macAt + k) & 3u] != b.macHistory[(b.macAt + k) & 3u])
                {
                    diff += " macHistory";
                    break;
                }
            if (!diff.empty() && g_compiled.mismatches++ < 40)
            {
                std::printf("[shino:verify] %s program %x (flip %llu, #%d):%s\n", unit.vu0 ? "vu0" : "vu1", g_programStart,
                            (unsigned long long)gs::flips(), g_inFlip, diff.c_str());
                if (g_compiled.mismatches <= 3) // the values: compiled, then interpreted; the entry's
                {
                    for (int r = 1; r < 32; ++r)
                        if (std::memcmp(compiledVf[r].f, b.vf[r].f, 16) != 0)
                            std::printf("  vf%d %g %g %g %g | %g %g %g %g | entry %g %g %g %g\n", r, compiledVf[r].f[0], compiledVf[r].f[1],
                                        compiledVf[r].f[2], compiledVf[r].f[3], b.vf[r].f[0], b.vf[r].f[1], b.vf[r].f[2], b.vf[r].f[3],
                                        beforeVf[r].f[0], beforeVf[r].f[1], beforeVf[r].f[2], beforeVf[r].f[3]);
                    std::printf("  status %x | %x, mac %x | %x, q %g | %g, pairs %llu | %llu, vi1-15:", a.status, b.status, a.mac,
                                b.mac, a.q, b.q, (unsigned long long)(a.pairs - before.pairs),
                                (unsigned long long)(b.pairs - before.pairs));
                    for (int r = 1; r < 16; ++r)
                        std::printf(" %x/%x", a.vi[r], b.vi[r]);
                    std::printf("\n");
                }
            }
        }

        void runCompiled(uint32_t pc)
        {
            static const bool verify = std::getenv("SHINO_VU_VERIFY") != nullptr;
            if (verify)
                verifyRun(pc);
            else
                compiledFrom(pc);
        }

        // The interpreter: the program on the unit g_cur from the pair at pc (bytes) to its end (kEnd), or with `rejoin`
        // to the next pair a compiled image holds (its pc).
        uint32_t interpret(uint32_t pc, bool rejoin)
        {
            Vu1 &v = *g_cur;
            auto &decoded = v.vu0 ? g_decoded0 : g_decoded;
            int64_t branch = -1;
            bool ending = false;
            for (int guard = 0; guard < 1 << 22; ++guard)
            {
                const uint32_t lo = v.micro[pc >> 2], up = v.micro[(pc >> 2) + 1];
                Decoded &d = decoded[(pc >> 3) & 2047u];
                if (!d.valid || d.lo != lo || d.up != up)
                    d = decodePair(lo, up, pc, v.pcMask);
                ++v.pairs;
                // Q's pending result lands
                if (v.qDelay && --v.qDelay == 0)
                    v.q = v.pendingQ;
                static UpperResult u; // made afresh only for upper()
                __m128 arithResult;
                static const bool scalarOnly = std::getenv("SHINO_VU_SCALAR") != nullptr; // the reference path, for checks
                static const uint64_t slowKinds = std::getenv("SHINO_LOWER_SLOW") ? std::strtoull(std::getenv("SHINO_LOWER_SLOW"), nullptr, 0) : 0;
                const UpperKind kind = g_traceThis || scalarOnly ? kUpperScalar : static_cast<UpperKind>(d.kind); // traces print upper()'s
                if (kind == kUpperArith)
                    arithResult = upperArith(d);
                else if (kind == kUpperScalar)
                {
                    u = UpperResult{};
                    upper(up, u);
                }
                bool loi = (up >> 31) & 1u;
                // XTOP / XITOP read the double buffer
                if (d.xtop && d.xreg)
                    v.vi[d.xreg] = static_cast<uint16_t>(d.xtop == 1 ? g_cv->top : g_cv->itop);
                const int64_t to = g_traceThis || scalarOnly || ((slowKinds >> d.lk) & 1u) ? lower(lo, pc, loi) : lowerFast(d, lo, pc, loi);
                if (kind == kUpperArith)
                    commitArith(d, arithResult);
                else if (kind == kUpperScalar)
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
                    return vu::kEnd;
                }
                if (branch >= 0)
                {
                    pc = static_cast<uint32_t>(branch) & v.pcMask;
                    branch = -1;
                }
                else
                    pc = (pc + 8) & v.pcMask;
                if (to >= 0)
                    branch = to;
                if (endBit)
                    ending = true;
                ++g_interpreted;
                // Back to the compiled code at the next pair it can take (nothing pending: no branch, no end).
                if (rejoin && branch < 0 && !ending && findImage(pc) >= 0)
                    return pc;
            }
            std::printf("[shino:%s] program at %x did not end (flip %llu, program %d of it)\n", v.vu0 ? "vu0" : "vu1",
                        g_programStart, (unsigned long long)gs::flips(), v.vu0 ? -1 : g_inFlip);
            return vu::kEnd;
        }


        void runProgram(uint32_t pcBytes)
        {
            Vu1 &v = *g_cur;
            prof::Scope timed(v.vu0 ? prof::Vu0 : prof::Vu1);
            g_programStart = pcBytes;
            {
                static int count = -1;
                static uint64_t at = ~0ull;
                static int which = -1;
                static const char *tp = std::getenv("SHINO_TRACE_PROGRAM"); // read once: getenv a program was 12%
                if (tp && which < 0)
                {
                    at = std::strtoull(tp, nullptr, 10);
                    const char *c = std::strchr(tp, ':');
                    which = c ? std::atoi(c + 1) : 0;
                }
                if (!v.vu0 && gs::flips() == at)
                    ++count;
                g_traceThis = !v.vu0 && gs::flips() == at && count == which;
            }
            ++v.programs;
            static uint64_t flipSeen = ~0ull;
            if (!v.vu0)
            {
                if (gs::flips() != flipSeen)
                {
                    flipSeen = gs::flips();
                    g_inFlip = 0;
                }
                else
                    ++g_inFlip;
            }
            // TOP: the buffer the program reads; the next uses the other (BASE, BASE + OFFSET).
            v.vi[0] = 0;
            const uint32_t pc = pcBytes & v.pcMask;
            static const bool vu0Compiled = !std::getenv("SHINO_VU0_INTERP"); // SHINO_VU0_INTERP=1: VU0 interpreted
            static const bool verifying = std::getenv("SHINO_VU_VERIFY") != nullptr;
            // Native 3D (shino_native3d.cpp): a renderer's program stops at a variant drawn natively and the batch is drawn
            // natively; verify: drawn by the VU as well, the two compared.
            const native3d::Mode nativeMode = native3d::mode();
            const uint8_t *stops = !v.vu0 && !g_traceThis && !verifying && nativeMode != native3d::Mode::Off && compiledOn()
                                       ? native3d::takeOverAt(v)
                                       : nullptr;
            if (stops)
            {
                v.stopPairs = stops;
                g_stopped = false;
                compiledFrom(pc);
                v.stopPairs = nullptr;
                if (g_stopped)
                {
                    const uint32_t takeOver = g_stoppedAt;
                    native3d::batch(v, takeOver, nativeMode != native3d::Mode::Verify);
                    if (nativeMode == native3d::Mode::Verify)
                    {
                        compiledFrom(takeOver); // the variant, as without native drawing
                        native3d::compare(v);
                    }
                    else if (nativeMode == native3d::Mode::Skip)
                        v.vi[15] = 51; // as the variant leaves it: the next batch's prelude (pair 51)
                    else
                    {
                        // The variant run for what it leaves (VU1 memory and registers: later programs read them), its
                        // GS packets not sent: the batch is drawn natively.
                        g_noKick = true;
                        compiledFrom(takeOver);
                        g_noKick = false;
                        native3d::finish(v);
                    }
                }
            }
            else if (!g_traceThis && compiledOn() && (!v.vu0 || vu0Compiled))
                runCompiled(pc);
            else
                interpret(pc);
        }

        void startProgram(uint32_t pcBytes)
        {
            if (survey() && !g_cur->vu0)
            {
                const uint64_t before = g_vu.pairs;
                auto &e = g_starts[{pcBytes, g_regionSrc}];
                Vif &f = *g_cv;
                f.top = f.tops;
                f.itop = f.itops;
                f.dbf = !f.dbf;
                f.tops = f.base + (f.dbf ? f.offset : 0);
                runProgram(pcBytes);
                ++e[0];
                e[1] += g_vu.pairs - before;
                return;
            }
            if (static const char *d = std::getenv("SHINO_DUMP_MICRO"); d && gs::flips() == static_cast<uint64_t>(std::atoll(d)))
            {
                static int n = 0, seen = -1;
                ++seen; // SHINO_DUMP_MICRO=<flip>[:<first>]: the flip's programs from the first-th (12 at most);
                // SHINO_DUMP_MICRO_PC=<pc>: only the programs starting there
                const char *colon = std::strchr(d, ':');
                static const long onlyPc = std::getenv("SHINO_DUMP_MICRO_PC") ? std::strtol(std::getenv("SHINO_DUMP_MICRO_PC"), nullptr, 16) : -1;
                if (n < 12 && seen >= (colon ? std::atoi(colon + 1) : 0) && (onlyPc < 0 || static_cast<long>(pcBytes) == onlyPc))
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
            Vif &f = *g_cv;
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

        // UNPACK's common case (no mask, no fill, STMOD 0, 32 / 16 / 8-bit elements), one element shape a function:
        // VN + 1 elements of 4 >> VL bytes (sign-extended unless USN) a vector, the vector filled out to four (S: x to
        // all, V2: xyxy, V3: w = z), WL vectors written of every CL. The same values as the general way below.
        template <uint32_t VN, uint32_t VL, bool USN>
        void unpackSimple(uint8_t *mem, uint32_t qwMask, uint32_t addr, uint32_t num, uint32_t cl, uint32_t wl, const uint8_t *p)
        {
            constexpr uint32_t n = VN + 1, size = 4u >> VL;
            uint32_t row = addr, k = 0; // the destination: addr + (i / wl) * cl + i % wl, kept as it steps
            for (uint32_t i = 0; i < num; ++i)
            {
                uint32_t in[4];
                if constexpr (VL == 0)
                {
                    std::memcpy(in, p, 4 * n);
                    p += 4 * n;
                }
                else
                    for (uint32_t e = 0; e < n; ++e, p += size)
                    {
                        uint32_t x = size == 2 ? uint32_t(p[0] | (p[1] << 8)) : uint32_t(p[0]);
                        if constexpr (!USN)
                            x = size == 2 ? uint32_t(int32_t(int16_t(x))) : uint32_t(int32_t(int8_t(x)));
                        in[e] = x;
                    }
                if constexpr (VN == 0)
                    in[1] = in[2] = in[3] = in[0];
                else if constexpr (VN == 1)
                {
                    in[2] = in[0];
                    in[3] = in[1];
                }
                else if constexpr (VN == 2)
                    in[3] = in[2];
                std::memcpy(mem + 16 * ((row + k) & qwMask), in, 16);
                if (++k == wl)
                {
                    k = 0;
                    row += cl;
                }
            }
        }

        void doUnpack(uint32_t code, const uint8_t *data)
        {
            Vif &f = *g_cv;
            const uint32_t cmd = (code >> 24) & 0x7Fu, vn = (cmd >> 2) & 3u, vl = cmd & 3u, masked = (cmd >> 4) & 1u;
            const bool usn = (code >> 14) & 1u, flg = (code >> 15) & 1u;
            uint32_t num = (code >> 16) & 0xFFu;
            if (!num) num = 256;
            uint32_t addr = code & 0x3FFu;
            if (flg) addr += f.tops;
            const uint32_t cl = std::max(f.cl, 1u), wl = std::max(f.wl, 1u);
            if (survey()) // the UNPACK forms: vn vl masked mode fill (what it costs: vectors)
                g_unpackForms[{vn | vl << 2 | masked << 4 | f.mode << 5 | uint32_t(wl > cl) << 7 | uint32_t(cl != wl) << 8 | uint32_t(usn) << 9}] += num;
            // The common case: no mask, no fill, STMOD 0, 32 / 16 / 8-bit elements: the vectors straight from the data
            // (the same values as the general way below).
            if (!masked && f.mode == 0 && wl <= cl && vl != 3)
            {
                using Simple = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, const uint8_t *);
                static constexpr Simple kSimple[4][3][2] = {
                    {{unpackSimple<0, 0, false>, unpackSimple<0, 0, true>}, {unpackSimple<0, 1, false>, unpackSimple<0, 1, true>}, {unpackSimple<0, 2, false>, unpackSimple<0, 2, true>}},
                    {{unpackSimple<1, 0, false>, unpackSimple<1, 0, true>}, {unpackSimple<1, 1, false>, unpackSimple<1, 1, true>}, {unpackSimple<1, 2, false>, unpackSimple<1, 2, true>}},
                    {{unpackSimple<2, 0, false>, unpackSimple<2, 0, true>}, {unpackSimple<2, 1, false>, unpackSimple<2, 1, true>}, {unpackSimple<2, 2, false>, unpackSimple<2, 2, true>}},
                    {{unpackSimple<3, 0, false>, unpackSimple<3, 0, true>}, {unpackSimple<3, 1, false>, unpackSimple<3, 1, true>}, {unpackSimple<3, 2, false>, unpackSimple<3, 2, true>}}};
                kSimple[vn][vl][usn](g_cur->data, g_cur->qwMask, addr, num, cl, wl, data);
                return;
            }
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
            Vif &f = *g_cv;
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
            case 0x17: startProgram(g_cur->tpc); break;                                     // MSCNT
            case 0x20: std::memcpy(&f.mask, data, 4); break;                                // STMASK
            case 0x30: std::memcpy(f.row, data, 16); break;                                 // STROW
            case 0x31: std::memcpy(f.col, data, 16); break;                                 // STCOL
            case 0x4A: // MPG: NUM 64-bit words to micro memory at IMM (in pairs)
            {
                uint32_t num = (code >> 16) & 0xFFu;
                if (!num) num = 256;
                if (survey() && !g_cur->vu0)
                {
                    const bool changed = std::memcmp(&g_cur->micro[(imm & 0x7FFu) * 2], data, std::min(num, 2048u - (imm & 0x7FFu)) * 8) != 0;
                    const uint8_t *from = data == g_cv->data.data() ? g_cv->from : data;
                    const uint32_t src = vif::eeRam && from >= vif::eeRam && from < vif::eeRam + 0x2000000 ? uint32_t(from - vif::eeRam) : 0xFFFFFFFFu;
                    for (uint32_t k = 0; k < num; k += 0x40)
                        g_regionSrc[((imm + k) >> 6) & 31u] = src + 8 * k;
                    auto &e = g_mpgs[{imm, num, src}];
                    ++e[0];
                    e[1] += changed;
                    g_microVersion += changed;
                }
                for (uint32_t k = 0; k < num; ++k)
                {
                    const uint32_t pair = (imm + k) & (g_cur->pcMask >> 3);
                    uint32_t *to = &g_cur->micro[pair * 2];
                    if (std::memcmp(to, data + 8 * k, 8) != 0) // the compiled code there to be checked again
                        ++(g_cur->vu0 ? vu::g_segmentVersion0 : vu::g_segmentVersion)[pair / vu::kSegmentPairs];
                    std::memcpy(to, data + 8 * k, 8);
                }
                break;
            }
            case 0x50: case 0x51: // DIRECT, DIRECTHL (VIF1 only)
                if (prof::Scope timed(prof::Gs); true)
                for (uint32_t at = 0; !g_cur->vu0 && at + 16 <= f.need; at += 16)
                    gs::directQuadword(data + at);
                break;
            case 0x06: // MSKPATH3: an unmask lets the GIF chain's next packet group through (path3::unmasked)
                if (!g_cur->vu0 && !(imm & 0x8000u))
                    path3::unmasked();
                break;
            default: break; // NOP, MARK, FLUSHE, FLUSH, FLUSHA
            }
        }

        uint32_t dataBytes(uint32_t code)
        {
            const uint32_t cmd = (code >> 24) & 0x7Fu, imm = code & 0xFFFFu;
            if (cmd >= 0x60)
                return unpackBytes(code, std::max(g_cv->cl, 1u), std::max(g_cv->wl, 1u));
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

    // ---- What the compiled programs share with the interpreter (runtime/shino_vu.h) -----------------------------
    namespace vu
    {
        const LaneMasks g_lanes;
        uint32_t g_segmentVersion[kSegments] = {}, g_segmentVersion0[kSegments] = {};
        SegmentCheck *g_segmentChecks = new SegmentCheck[kImageCount * kSegments];
        SegmentCheck *g_segmentChecks0 = new SegmentCheck[kImageCount * kSegments];
        uint32_t *g_segmentVersionCur = g_segmentVersion;
        SegmentCheck *g_segmentChecksCur = g_segmentChecks;
        bool recheckSegment(uint32_t image, uint32_t segment)
        {
            const Image &im = kImages[image];
            const uint32_t lo = std::max(segment * kSegmentPairs, im.start);
            const uint32_t hi = std::min((segment + 1) * kSegmentPairs, im.start + im.count);
            const Vu1 &u = *g_cur;
            const bool ok = lo < hi && hi <= (u.pcMask >> 3) + 1 &&
                            std::memcmp(&u.micro[lo * 2], &im.code[(lo - im.start) * 2], (hi - lo) * 8) == 0;
            SegmentCheck &c = g_segmentChecksCur[image * kSegments + segment];
            c.version = g_segmentVersionCur[segment];
            c.ok = ok;
            return ok;
        }
        void upperSlow(uint32_t up, UpperResult &out) { upper(up, out); }
        void commitSlow(const UpperResult &u) { commitUpper(u); }
        void lowerSlow(uint32_t lo, uint32_t pc)
        {
            bool loi = false;
            lower(lo, pc, loi);
        }
        void kick(uint32_t address, uint32_t pc)
        {
            if (g_cur->vu0) // VU0 has no PATH1 (the interpreter counts it the same)
            {
                ++g_cur->kicks;
                return;
            }
            Unit &v = g_vu;
            ++v.kicks;
            static const char *t = std::getenv("SHINO_TRACE_KICK");
            if (t && gs::flips() == static_cast<uint64_t>(std::atoll(t)))
            {
                std::printf("[shino:vu1] kick at %03x from program %x (pc %x)\n", address & 0x3FFu, g_programStart, pc);
                for (uint32_t q = 0; q < 20; ++q)
                {
                    const uint32_t *m = &vu::memWord(v, address + q, 0);
                    std::printf("  %03x: %08x %08x %08x %08x\n", (address + q) & 0x3FFu, m[0], m[1], m[2], m[3]);
                }
            }
            if (native3d::capturing())
                native3d::captureKick(v.mem, address);
            if (!g_noKick)
                gs::path1(v.mem, address);
        }
        uint32_t top() { return g_cv->top; }
        uint32_t itop() { return g_cv->itop; }
    } // namespace vu

    void vif::codes(const uint8_t *data, uint32_t bytes, bool)
    {
        prof::Scope timed(prof::Vif);
        Vif &f = *g_cv;
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
            bool traced = false; // SHINO_TRACE_VIF prints the data as it is gathered
            if (static const char *tr = std::getenv("SHINO_TRACE_VIF"); tr && gs::flips() == static_cast<uint64_t>(std::atoll(tr)) && (traced = true))
                std::printf("[shino:vif] %08x cl=%u wl=%u tops=%x base=%x offset=%x mode=%u\n", code, f.cl, f.wl, f.tops, f.base,
                            f.offset, f.mode);
            f.code = code;
            f.need = dataBytes(code);
            f.from = data + at;
            // MPG's and DIRECT's data start on the next 64-bit / 128-bit boundary of the stream: their alignment
            // padding (VIF NOPs) is already in the stream as codes, so nothing to skip here.
            if (!f.need)
                execute(code, nullptr);
            else if (f.need <= bytes - at && !traced) // all its data here: run from the stream, not a copy
            {
                execute(code, data + at);
                at += f.need;
                f.need = 0;
            }
        }
    }

    void vif::codes0(Runtime *rt, const uint8_t *data, uint32_t bytes)
    {
        g_vu0.data = bozo_vu0_memory(rt);
        g_vu0.qwMask = 0xFFu;
        g_vu0.pcMask = 0xFF8u;
        g_vu0.vu0 = true;
        Vu1 *unit = g_cur;
        Vif *in = g_cv;
        g_cur = &g_vu0;
        g_cv = &g_vif0;
        codes(data, bytes, false);
        g_cur = unit;
        g_cv = in;
    }

    namespace
    {
        // VU0's programs (about 10,000 a frame, of about 7 pairs) run compiled without runProgram's VU1 bookkeeping: the
        // image last found at the pc, while its segment holds the same code; otherwise as runProgram does.
        bool vu0Direct()
        {
            static const bool on = compiledOn() && !std::getenv("SHINO_VU0_INTERP") && !std::getenv("SHINO_VU_VERIFY");
            return on;
        }
        void runVu0Compiled(uint32_t pcBytes)
        {
            Vu1 &v = *g_cur;
            prof::Scope timed(prof::Vu0);
            g_programStart = pcBytes;
            ++v.programs;
            v.vi[0] = 0;
            selectUnit(v);
            const uint32_t pc = pcBytes & v.pcMask, pair = pc >> 3;
            const int last = g_lastImage0[pair];
            if (last < 0 || !vu::segmentOk(static_cast<uint32_t>(last), pair / vu::kSegmentPairs))
            {
                compiledFrom(pc);
                return;
            }
            uint32_t r = vu::kImages[last].run(v, pc);
            if (r != vu::kEnd && (r & vu::kInterpret))
            {
                ++g_compiled.handed;
                ++g_compiled.handedAt[r & v.pcMask];
                r = interpret(r & v.pcMask, true);
            }
            if (r == vu::kEnd)
                ++g_compiled.ended;
            else
                compiledFrom(r & v.pcMask);
        }
    }

    // The registers copied one by one, not vectorized: the EE code and the compiled program write them one at a time
    // (ctx->vi as words, the unit's vi as halfwords), and a 16-byte load of what narrower stores just wrote stalls.
#if defined(__GNUC__) && !defined(__clang__)
    __attribute__((optimize("no-tree-vectorize", "no-tree-slp-vectorize")))
#endif
    void vu0::run(Runtime *rt, R5900Context *ctx, uint32_t pcBytes)
    {
        Vu1 &v = g_vu0;
        v.data = bozo_vu0_memory(rt);
        v.qwMask = 0xFFu;
        v.pcMask = 0xFF8u;
        v.vu0 = true;
        static_assert(sizeof(ctx->vu0_vf[0]) == sizeof(vu::Vec) && sizeof(ctx->vu0_vf) == 32 * sizeof(vu::Vec));
        v.vf = reinterpret_cast<vu::Vec *>(ctx->vu0_vf); // used in place: 10k programs a frame, of about 7 pairs
        std::memcpy(v.acc.f, ctx->vu0_acc.f, 16);
        for (int r = 0; r < 16; ++r)
            v.vi[r] = static_cast<uint16_t>(ctx->vi[r]);
        v.q = ctx->vu0_q;
        v.i = ctx->vu0_i;
        v.p = ctx->vu0_p;
        v.r = ctx->vu0_r | 0x3F800000u;
        v.status = ctx->vu0_status;
        v.mac = static_cast<uint16_t>(ctx->vu0_mac);
        v.clip = ctx->vu0_clip_flags;
        v.qDelay = 0;
        Vu1 *unit = g_cur;
        Vif *in = g_cv;
        g_cur = &v;
        g_cv = &g_vif0;
        static long traceLeft = std::getenv("SHINO_TRACE_VU0") ? std::atol(std::getenv("SHINO_TRACE_VU0")) : 0;
        const bool trace = traceLeft > 0;
        if (trace)
        {
            --traceLeft;
            std::printf("[shino:vu0] program %03x (ra %x) vi1-6 %x %x %x %x %x %x\n", pcBytes, ctx->r[31].uw[0], v.vi[1], v.vi[2],
                        v.vi[3], v.vi[4], v.vi[5], v.vi[6]);
            std::printf("  nonzero qw:");
            for (uint32_t q = 0; q < 256; ++q)
            {
                const uint32_t *m = reinterpret_cast<const uint32_t *>(v.data + 16 * q);
                if (m[0] | m[1] | m[2] | m[3])
                    std::printf(" %02x", q);
            }
            std::printf("\n");
        }
        if (!trace && vu0Direct())
            runVu0Compiled(pcBytes);
        else
            runProgram(pcBytes);
        if (trace)
            for (int r = 1; r < 12; ++r)
                std::printf("  vf%-2d %g %g %g %g\n", r, v.vf[r].f[0], v.vf[r].f[1], v.vf[r].f[2], v.vf[r].f[3]);
        g_cur = unit;
        g_cv = in;
        if (v.qDelay)
        {
            v.q = v.pendingQ;
            v.qDelay = 0;
        }
        std::memcpy(ctx->vu0_acc.f, v.acc.f, 16);
        for (int r = 1; r < 16; ++r)
            ctx->vi[r] = v.vi[r];
        ctx->vu0_q = v.q;
        ctx->vu0_i = v.i;
        ctx->vu0_p = v.p;
        ctx->vu0_r = v.r & 0x7FFFFFu;
        ctx->vu0_status = v.status;
        ctx->vu0_mac = v.mac;
        ctx->vu0_clip_flags = v.clip;
    }

    uint8_t *vu0::codeMemory() { return reinterpret_cast<uint8_t *>(g_vu0.micro); }

    uint64_t vif::vu1Pairs() { return g_vu.pairs; }
    uint64_t vif::vu1Programs() { return g_vu.programs; }

    void vif::report()
    {
        native3d::report();
        std::printf("[shino:vif]");
        for (uint32_t cmd = 0; cmd < g_vif.counts.size(); ++cmd)
            if (g_vif.counts[cmd])
                std::printf(" %02x:%llu", cmd, (unsigned long long)g_vif.counts[cmd]);
        std::printf("\n[shino:vu1] programs=%llu pairs=%llu kicks=%llu refused=%llu\n", (unsigned long long)g_vu.programs,
                    (unsigned long long)g_vu.pairs, (unsigned long long)g_vu.kicks, (unsigned long long)g_vu.refused);
        std::printf("[shino:vu0] programs=%llu pairs=%llu refused=%llu\n", (unsigned long long)g_vu0.programs,
                    (unsigned long long)g_vu0.pairs, (unsigned long long)g_vu0.refused);
        std::printf("[shino:vu1] compiled: %u images; programs %llu, handed to the interpreter %llu times, no image %llu; "
                    "pairs interpreted %llu of %llu",
                    vu::kImageCount, (unsigned long long)g_compiled.ended, (unsigned long long)g_compiled.handed,
                    (unsigned long long)g_compiled.noImage, (unsigned long long)g_interpreted, (unsigned long long)g_vu.pairs);
        if (g_compiled.verified)
            std::printf("; verified %llu, %llu differed", (unsigned long long)g_compiled.verified,
                        (unsigned long long)g_compiled.mismatches);
        std::printf("\n");
        for (const auto &[pc, n] : g_compiled.handedAt)
            std::printf("[shino:vu1]   handed at %04x: %llu\n", pc, (unsigned long long)n);
        for (const auto &[pc, n] : g_compiled.noImageAt)
            std::printf("[shino:vu1]   no image at %04x: %llu\n", pc, (unsigned long long)n);
        for (const auto &[k, n] : g_unpackForms)
            std::printf("[shino:survey] unpack vn %u vl %u masked %u mode %u fill %u cl!=wl %u usn %u: %llu vectors\n", k[0] & 3,
                        (k[0] >> 2) & 3, (k[0] >> 4) & 1, (k[0] >> 5) & 3, (k[0] >> 7) & 1, (k[0] >> 8) & 1, (k[0] >> 9) & 1,
                        (unsigned long long)n);
        for (const auto &[k, e] : g_mpgs)
            std::printf("[shino:survey] mpg at %03x pairs %3u from %08x: %llu times, %llu changed\n", k[0], k[1], k[2],
                        (unsigned long long)e[0], (unsigned long long)e[1]);
        for (const auto &[k, e] : g_starts)
        {
            std::printf("[shino:survey] program %04x: %llu runs, %llu pairs; code from", k.first, (unsigned long long)e[0],
                        (unsigned long long)e[1]);
            for (uint32_t r = 0; r < 32; ++r)
                std::printf(" %x", k.second[r]);
            std::printf("\n");
        }
    }
} // namespace shino
