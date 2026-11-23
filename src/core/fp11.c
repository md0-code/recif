#include "pdp11_priv.h"

#include <string.h>

/* FP11 floating-point processor as described by the DEC 1979 processor
 * handbook (pp. 341ff): F and D formats, FPS modes, FEC/FEA and the
 * exception trap through 000244. Accumulators hold the memory image with
 * the sign in bit 63, the excess-128 exponent in 62:55 and the fraction
 * below it. Fractions are worked on with the hidden bit at 55, shifted
 * left by three guard bits for rounding. */

#define SIGN (UINT64_C(1) << 63)
#define EXPONENT (UINT64_C(0377) << 55)
#define FRACTION ((UINT64_C(1) << 55) - 1)
#define HIDDEN (UINT64_C(1) << 55)
#define HIGH ((uint64_t)UINT32_MAX << 32)
#define GUARD 3
#define G_HIDDEN (UINT64_C(1) << 58)
#define G_CARRY (UINT64_C(1) << 59)
#define BIAS 0200

enum {
    FER = 0100000, FID = 040000, FIUV = 04000, FIU = 02000, FIV = 01000, FIC = 0400,
    FD = 0200, FL = 0100, FT = 040, FN = 010, FZ = 04, FV = 02, FC = 01,
    FPS_WRITABLE = 0147757
};
enum { FEC_OP = 2, FEC_DIVZERO = 4, FEC_CONVERT = 6, FEC_OVERFLOW = 8, FEC_UNDERFLOW = 10, FEC_UNDEFINED = 12 };

typedef struct {
    Pdp11 *c;
    bool double_mode, trap;
} Fp;

void fp11_reset(Fp11 *fp)
{
    memset(fp, 0, sizeof(*fp));
}

static unsigned exponent(uint64_t v)
{
    return (unsigned)((v >> 55) & 0377);
}

/* Records an exception. True when its interrupt is disabled, so the
 * instruction goes on with the documented substitute result. */
static bool masked(Fp *f, unsigned code)
{
    static const uint16_t enable[] = {0, 0, 0, FIC, FIV, FIU, FIUV};
    Fp11 *fp = &f->c->fp;
    if (code >= FEC_CONVERT && !(fp->fps & enable[code >> 1]))
        return true;
    fp->fps |= FER;
    fp->fec = (uint16_t)code;
    fp->fea = f->c->instruction_pc;
    if (!(fp->fps & FID))
        f->trap = true;
    return false;
}

static void set_cc(Fp *f, uint64_t v, unsigned overflow)
{
    uint16_t *fps = &f->c->fp.fps;
    *fps = (uint16_t)((*fps & ~(FN | FZ | FV | FC)) | overflow |
                      (v & SIGN ? FN : 0) | (exponent(v) ? 0 : FZ));
}

static uint64_t ac_get(const Fp *f, unsigned ac, bool wide)
{
    uint64_t v = f->c->fp.ac[ac];
    return wide ? v : v & HIGH;
}

static void ac_put(Fp *f, unsigned ac, uint64_t v, bool wide)
{
    uint64_t *slot = &f->c->fp.ac[ac];
    *slot = wide ? v : (*slot & ~HIGH) | (v & HIGH);
}

/* Effective address of a floating operand of `length` bytes. Mode 0
 * names AC0-AC5; AC6 and AC7 are an illegal-mode error. */
static bool address(Fp *f, unsigned spec, unsigned length, PdpOperand *o)
{
    if (!(spec & 070)) {
        if ((spec & 7) >= 6) {
            masked(f, FEC_OP);
            return false;
        }
        *o = (PdpOperand){0, (uint8_t)(spec & 7), true, true};
        return true;
    }
    *o = pdp_operand(f->c, spec, length);
    return !f->c->fault;
}

static bool read_float(Fp *f, unsigned spec, PdpOperand o, unsigned length, uint64_t *v)
{
    if (o.direct) {
        *v = ac_get(f, o.reg, length == 8);
        return true;
    }
    unsigned words = spec == 027 ? 1 : length / 2;
    *v = 0;
    for (unsigned i = 0; i < words; ++i)
        *v |= (uint64_t)pdp_read(f->c, (uint16_t)(o.addr + 2 * i), o.data) << (48 - 16 * i);
    if (f->c->fault)
        return false;
    return !((*v & SIGN) && !exponent(*v)) || masked(f, FEC_UNDEFINED);
}

static void write_float(Fp *f, unsigned spec, PdpOperand o, unsigned length, uint64_t v)
{
    if (o.direct) {
        ac_put(f, o.reg, v, length == 8);
        return;
    }
    unsigned words = spec == 027 ? 1 : length / 2;
    for (unsigned i = 0; i < words && !f->c->fault; ++i)
        pdp_write(f->c, (uint16_t)(o.addr + 2 * i), o.data, (uint16_t)(v >> (48 - 16 * i)));
}

/* Integer operands keep the significant word in the high half. */
static uint32_t read_integer(Fp *f, unsigned spec, unsigned length)
{
    if (!(spec & 070))
        return (uint32_t)f->c->r[spec & 7] << 16;
    PdpOperand o = pdp_operand(f->c, spec, length);
    uint32_t v = (uint32_t)pdp_read(f->c, o.addr, o.data) << 16;
    if (length == 4 && spec != 027)
        v |= pdp_read(f->c, (uint16_t)(o.addr + 2), o.data);
    return v;
}

static void write_integer(Fp *f, unsigned spec, unsigned length, uint32_t v)
{
    if (!(spec & 070)) {
        f->c->r[spec & 7] = (uint16_t)(v >> 16);
        return;
    }
    PdpOperand o = pdp_operand(f->c, spec, length);
    pdp_write(f->c, o.addr, o.data, (uint16_t)(v >> 16));
    if (length == 4 && spec != 027)
        pdp_write(f->c, (uint16_t)(o.addr + 2), o.data, (uint16_t)v);
}

static uint16_t read_word(Fp *f, unsigned spec)
{
    if (!(spec & 070))
        return f->c->r[spec & 7];
    PdpOperand o = pdp_operand(f->c, spec, 2);
    return pdp_read(f->c, o.addr, o.data);
}

static void write_word(Fp *f, unsigned spec, uint16_t v)
{
    if (!(spec & 070)) {
        f->c->r[spec & 7] = v;
        return;
    }
    PdpOperand o = pdp_operand(f->c, spec, 2);
    pdp_write(f->c, o.addr, o.data, v);
}

static uint64_t guarded(uint64_t v)
{
    return ((v & FRACTION) | HIDDEN) << GUARD;
}

/* Rounds (unless chopping), packs and checks the exponent range. */
static uint64_t pack(Fp *f, uint64_t sign, int exp, uint64_t frac, bool round, unsigned *overflow)
{
    uint16_t fps = f->c->fp.fps;
    if (round && !(fps & FT)) {
        frac += fps & FD ? UINT64_C(1) << 2 : UINT64_C(1) << 34;
        if (frac & G_CARRY) {
            frac >>= 1;
            ++exp;
        }
    }
    frac >>= GUARD;
    uint64_t v = sign | ((uint64_t)(exp & 0377) << 55) | (frac & FRACTION);
    if (exp > 0377) {
        *overflow = FV;
        if (masked(f, FEC_OVERFLOW))
            v = 0;
    } else if (exp <= 0 && masked(f, FEC_UNDERFLOW)) {
        v = 0;
    }
    return v;
}

static uint64_t normalized(uint64_t frac, int *exp)
{
    while (frac && !(frac & G_HIDDEN)) {
        frac <<= 1;
        --*exp;
    }
    return frac;
}

static uint64_t add(Fp *f, uint64_t a, uint64_t b, unsigned *overflow)
{
    if ((a & ~SIGN) < (b & ~SIGN)) {
        uint64_t t = a;
        a = b;
        b = t;
    }
    if (!exponent(a))
        return exponent(b) ? b : 0;
    if (!exponent(b))
        return a;
    unsigned diff = exponent(a) - exponent(b);
    if (diff >= 60)
        return a;
    int exp = (int)exponent(a);
    uint64_t fa = guarded(a), fb = guarded(b) >> diff;
    if ((a ^ b) & SIGN) {
        fa -= fb;
        if (!fa)
            return 0;
        fa = normalized(fa, &exp);
    } else {
        fa += fb;
        if (fa & G_CARRY) {
            fa >>= 1;
            ++exp;
        }
    }
    return pack(f, a & SIGN, exp, fa, true, overflow);
}

/* Fraction product with the hidden bit at 58 and the exponent adjusted. */
static uint64_t product(uint64_t a, uint64_t b, int *exp)
{
    uint64_t x = (a & FRACTION) | HIDDEN, y = (b & FRACTION) | HIDDEN;
    uint64_t x0 = (uint32_t)x, x1 = x >> 32, y0 = (uint32_t)y, y1 = y >> 32;
    uint64_t p00 = x0 * y0, p01 = x0 * y1, p10 = x1 * y0, p11 = x1 * y1;
    uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    uint64_t lo = (mid << 32) | (uint32_t)p00;
    uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    uint64_t frac = (hi << 12) | (lo >> 52);
    *exp = (int)exponent(a) + (int)exponent(b) - BIAS;
    if (frac & G_CARRY)
        frac >>= 1;
    else
        --*exp;
    return frac;
}

static uint64_t multiply(Fp *f, uint64_t a, uint64_t b, unsigned *overflow)
{
    if (!exponent(a) || !exponent(b))
        return 0;
    int exp;
    uint64_t frac = product(a, b, &exp);
    return pack(f, (a ^ b) & SIGN, exp, frac, true, overflow);
}

static uint64_t divide(Fp *f, uint64_t a, uint64_t b, unsigned *overflow)
{
    if (!exponent(a))
        return 0;
    uint64_t rem = (a & FRACTION) | HIDDEN, divisor = (b & FRACTION) | HIDDEN, q = 0;
    for (int i = 0; i < 60; ++i) {
        q <<= 1;
        if (rem >= divisor) {
            rem -= divisor;
            q |= 1;
        }
        rem <<= 1;
    }
    int exp = (int)exponent(a) - (int)exponent(b) + BIAS;
    if (q & G_CARRY) {
        q >>= 1;
        ++exp;
    }
    return pack(f, (a ^ b) & SIGN, exp, q, true, overflow);
}

/* MODF: integer part to AC|1, fraction to AC. */
static void modulo(Fp *f, unsigned ac, uint64_t a, uint64_t b)
{
    bool wide = f->double_mode;
    unsigned overflow = 0;
    uint64_t whole = 0, part = 0;
    if (exponent(a) && exponent(b)) {
        uint64_t sign = (a ^ b) & SIGN;
        int exp;
        uint64_t frac = product(a, b, &exp);
        if (exp <= BIAS) {
            part = pack(f, sign, exp, frac, true, &overflow);
        } else if (exp > BIAS + (wide ? 56 : 24)) {
            whole = pack(f, sign, exp, frac, false, &overflow);
        } else {
            unsigned bits = (unsigned)(exp - BIAS);
            uint64_t mask = (G_CARRY - 1) >> bits;
            uint64_t rest = frac & mask;
            if (rest) {
                int rest_exp = BIAS;
                rest = normalized(rest << bits, &rest_exp);
                part = pack(f, sign, rest_exp, rest, true, &overflow);
            }
            whole = pack(f, sign, exp, frac & ~mask, false, &overflow);
        }
    }
    ac_put(f, ac | 1, whole, wide);
    ac_put(f, ac, part, wide);
    set_cc(f, part, overflow);
}

/* Rounds a D value to F precision; a carry out of the exponent overflows. */
static uint64_t round_single(Fp *f, uint64_t v, unsigned *overflow)
{
    uint64_t sign = v & SIGN;
    uint64_t rounded = (v & ~SIGN) + (UINT64_C(1) << 31);
    if (rounded & SIGN) {
        *overflow = FV;
        return masked(f, FEC_OVERFLOW) ? 0 : sign | (rounded & ~SIGN);
    }
    return sign | rounded;
}

static int compare(uint64_t a, uint64_t b)
{
    if (a == b)
        return 0;
    bool a_negative = a & SIGN, b_negative = b & SIGN;
    if (a_negative != b_negative)
        return a_negative ? -1 : 1;
    bool smaller = (a & ~SIGN) < (b & ~SIGN);
    return smaller != a_negative ? -1 : 1;
}

static void load_integer(Fp *f, unsigned ac, unsigned spec)
{
    bool wide = f->c->fp.fps & FL;
    uint32_t raw = read_integer(f, spec, wide ? 4 : 2);
    if (f->c->fault)
        return;
    uint64_t v = 0;
    if (raw) {
        bool negative = raw & 0x80000000u;
        uint32_t magnitude = negative ? 0u - raw : raw;
        int exp = BIAS + (wide ? 32 : 16);
        while (!(magnitude & 0x80000000u)) {
            magnitude <<= 1;
            --exp;
        }
        v = (negative ? SIGN : 0) | ((uint64_t)exp << 55) | ((uint64_t)(magnitude & 0x7fffffffu) << 24);
        if (!(f->c->fp.fps & (FD | FT))) {
            unsigned ignored = 0;
            v = round_single(f, v, &ignored);
        }
    }
    ac_put(f, ac, v, f->double_mode);
    set_cc(f, v, 0);
}

static void store_integer(Fp *f, unsigned ac, unsigned spec)
{
    bool wide = f->c->fp.fps & FL;
    uint64_t v = ac_get(f, ac, f->double_mode);
    unsigned exp = exponent(v), limit = BIAS + (wide ? 32 : 16);
    bool negative = v & SIGN, error = false;
    int64_t n = 0;
    if (exp > limit) {
        error = true;
    } else if (exp > BIAS) {
        uint64_t magnitude = ((v & FRACTION) | HIDDEN) >> (56 - (exp - BIAS));
        uint64_t maximum = (wide ? UINT64_C(0x80000000) : 0x8000u) - (negative ? 0 : 1);
        if (magnitude > maximum)
            error = true;
        else
            n = negative ? -(int64_t)magnitude : (int64_t)magnitude;
    }
    uint32_t scaled = wide ? (uint32_t)n : (uint32_t)((uint32_t)n << 16);
    unsigned cc = (scaled & 0x80000000u ? FN : 0) | (scaled ? 0 : FZ) | (error ? FC : 0);
    if (error)
        masked(f, FEC_CONVERT);
    uint16_t *fps = &f->c->fp.fps;
    *fps = (uint16_t)((*fps & ~(FN | FZ | FV | FC)) | cc);
    pdp_set_cc(f->c, cc);
    write_integer(f, spec, wide ? 4 : 2, scaled);
}

static void load_exponent(Fp *f, unsigned ac, unsigned spec)
{
    int n = (int16_t)read_word(f, spec);
    if (f->c->fault)
        return;
    uint64_t v = ac_get(f, ac, f->double_mode);
    v = (v & ~EXPONENT) | ((uint64_t)((n + BIAS) & 0377) << 55);
    unsigned overflow = 0;
    if (n > 0177) {
        overflow = FV;
        if (masked(f, FEC_OVERFLOW))
            v = 0;
    } else if (n < -0177 && masked(f, FEC_UNDERFLOW)) {
        v = 0;
    }
    ac_put(f, ac, v, f->double_mode);
    set_cc(f, v, overflow);
}

static void store_exponent(Fp *f, unsigned ac, unsigned spec)
{
    uint16_t n = (uint16_t)(exponent(f->c->fp.ac[ac]) - BIAS);
    unsigned cc = (n & 0100000 ? FN : 0) | (n ? 0 : FZ);
    uint16_t *fps = &f->c->fp.fps;
    *fps = (uint16_t)((*fps & ~(FN | FZ | FV | FC)) | cc);
    pdp_set_cc(f->c, cc);
    write_word(f, spec, n);
}

static void group_zero(Fp *f, uint16_t op, unsigned ac, unsigned spec)
{
    Fp11 *fp = &f->c->fp;
    switch (ac) {
    case 0:
        switch (op) {
        case 0170000: pdp_set_cc(f->c, fp->fps & 017); break;
        case 0170001: fp->fps &= (uint16_t)~FD; break;
        case 0170002: fp->fps &= (uint16_t)~FL; break;
        case 0170011: fp->fps |= FD; break;
        case 0170012: fp->fps |= FL; break;
        default: masked(f, FEC_OP); break;
        }
        break;
    case 1: {
        uint16_t v = read_word(f, spec);
        if (!f->c->fault)
            fp->fps = v & FPS_WRITABLE;
        break;
    }
    case 2:
        fp->fps &= FPS_WRITABLE;
        write_word(f, spec, fp->fps);
        break;
    default:
        if (!(spec & 070))
            f->c->r[spec & 7] = fp->fec;
        else
            write_integer(f, spec, 4, (uint32_t)fp->fec << 16 | fp->fea);
        break;
    }
}

static void group_one(Fp *f, unsigned kind, unsigned spec, unsigned length)
{
    PdpOperand o;
    uint64_t v = 0;
    if (!address(f, spec, length, &o))
        return;
    if (kind == 0) {
        write_float(f, spec, o, length, 0);
        if (!f->c->fault)
            set_cc(f, 0, 0);
        return;
    }
    if (!read_float(f, spec, o, length, &v))
        return;
    if (kind != 1) {
        v = exponent(v) ? (kind == 2 ? v & ~SIGN : v ^ SIGN) : 0;
        write_float(f, spec, o, length, v);
        if (f->c->fault)
            return;
    }
    set_cc(f, v, 0);
}

unsigned fp11_execute(Pdp11 *c, uint16_t op)
{
    Fp f = {c, (c->fp.fps & FD) != 0, false};
    unsigned ac = (op >> 6) & 3, spec = op & 077;
    unsigned length = f.double_mode ? 8 : 4, other = 12 - length;
    unsigned overflow = 0;
    PdpOperand o;
    uint64_t src, acc;
    switch ((op >> 8) & 017) {
    case 000: group_zero(&f, op, ac, spec); break;
    case 001: group_one(&f, ac, spec, length); break;
    case 005:
        if (address(&f, spec, length, &o) && read_float(&f, spec, o, length, &src)) {
            ac_put(&f, ac, src, f.double_mode);
            set_cc(&f, src, 0);
        }
        break;
    case 010:
        if (address(&f, spec, length, &o))
            write_float(&f, spec, o, length, ac_get(&f, ac, f.double_mode));
        break;
    case 017:
        if (address(&f, spec, other, &o) && read_float(&f, spec, o, other, &src)) {
            if (!exponent(src))
                src = 0;
            if (!(c->fp.fps & (FD | FT)))
                src = round_single(&f, src, &overflow);
            ac_put(&f, ac, src, f.double_mode);
            set_cc(&f, src, overflow);
        }
        break;
    case 014:
        acc = ac_get(&f, ac, f.double_mode);
        if (!exponent(acc))
            acc = 0;
        if ((c->fp.fps & (FD | FT)) == FD)
            acc = round_single(&f, acc, &overflow);
        if (address(&f, spec, other, &o)) {
            write_float(&f, spec, o, other, acc);
            if (!c->fault)
                set_cc(&f, acc, overflow);
        }
        break;
    case 007:
        if (address(&f, spec, length, &o) && read_float(&f, spec, o, length, &src)) {
            acc = ac_get(&f, ac, f.double_mode);
            if (!exponent(src))
                src = 0;
            if (!exponent(acc))
                acc = 0;
            int order = compare(src, acc);
            if (!order && !src)
                ac_put(&f, ac, 0, f.double_mode);
            c->fp.fps = (uint16_t)((c->fp.fps & ~(FN | FZ | FV | FC)) |
                                   (order < 0 ? FN : 0) | (order ? 0 : FZ));
        }
        break;
    case 015: load_exponent(&f, ac, spec); break;
    case 012: store_exponent(&f, ac, spec); break;
    case 016: load_integer(&f, ac, spec); break;
    case 013: store_integer(&f, ac, spec); break;
    case 002: case 003: case 004: case 006: case 011:
        if (!address(&f, spec, length, &o) || !read_float(&f, spec, o, length, &src))
            break;
        acc = ac_get(&f, ac, f.double_mode);
        switch ((op >> 8) & 017) {
        case 002: acc = multiply(&f, acc, src, &overflow); break;
        case 003: modulo(&f, ac, acc, src); return f.trap ? 0244 : 0;
        case 004: acc = add(&f, acc, src, &overflow); break;
        case 006: acc = add(&f, acc, exponent(src) ? src ^ SIGN : src, &overflow); break;
        default:
            if (!exponent(src)) {
                masked(&f, FEC_DIVZERO);
                return f.trap ? 0244 : 0;
            }
            acc = divide(&f, acc, src, &overflow);
            break;
        }
        ac_put(&f, ac, acc, f.double_mode);
        set_cc(&f, acc, overflow);
        break;
    }
    return f.trap && !c->fault ? 0244 : 0;
}
