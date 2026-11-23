#include "pdp11_priv.h"

#include <limits.h>
#include <string.h>

/* DEC reference CPUs: KD11-D (11/04), KD11-EA (11/34A) and KD11-Z (11/44).
 * Model differences follow the DEC 1979 processor handbook, the 11/44
 * technical manual and the model masks of the pinned Open SIMH revision. */

#define IO18 0760000u
#define MM_ABORT 0160000u

enum { KERNEL = 0, SUPERVISOR = 1, USER = 3 };
enum {
    V_BUS = 0004, V_RESERVED = 0010, V_TRACE = 0014, V_IOT = 0020,
    V_EMT = 0030, V_TRAP = 0034, V_PIRQ = 0240, V_MMU = 0250
};

const char *pdp_reference_name(PdpModel model)
{
    switch (model) {
    case PDP_04: return "DEC PDP-11/04";
    case PDP_34: return "DEC PDP-11/34";
    case PDP_44: return "DEC PDP-11/44";
    }
    return "Unknown DEC reference";
}

unsigned pdp_ram_limit(PdpModel model)
{
    switch (model) {
    case PDP_04: return PDP_IO_VIRTUAL;
    case PDP_34: return IO18;
    case PDP_44: return PDP_IO_PAGE;
    }
    return 0;
}

bool pdp_fpu_option(PdpModel model)
{
    return model != PDP_04;
}

static uint16_t psw_mask(const Pdp11 *c)
{
    return c->model == PDP_04 ? 0000377u : c->model == PDP_34 ? 0170377u : 0170777u;
}

static unsigned mode(const Pdp11 *c)
{
    return c->psw >> 14;
}

static void load_psw(Pdp11 *c, uint16_t value)
{
    unsigned old = mode(c);
    c->psw = value & psw_mask(c);
    if (old != mode(c)) {
        c->sp[old] = c->r[6];
        c->r[6] = c->sp[mode(c)];
    }
}

static void flags(Pdp11 *c, unsigned mask, unsigned value)
{
    c->psw = (uint16_t)((c->psw & ~mask) | (value & mask));
}

void pdp_set_cc(Pdp11 *c, unsigned nzvc)
{
    flags(c, 15, nzvc);
}

static unsigned nz(uint32_t value, uint32_t sign)
{
    return (value & sign ? PDP_N : 0) | (value == 0 ? PDP_Z : 0);
}

static unsigned pirq_level(const Pdp11 *c)
{
    for (unsigned n = 7; n > 0; --n)
        if (c->pirq & (1u << (8 + n)))
            return n;
    return 0;
}

uint32_t pdp_dma_address(const Pdp11 *c, uint32_t address)
{
    if (c->direct_dma)
        return address & 017777777;
    address &= 0777777;
    if (address >= IO18)
        return PDP_IO_PAGE + (address - IO18);
    if (c->model == PDP_44 && (c->mmr[3] & 040))
        return (c->unibus[address >> 13] + (address & 017777)) & 017777777;
    return address;
}

/* CPU registers in the I/O page. `a` is the even word address. */
static bool internal_io(Pdp11 *c, uint16_t a, bool write, unsigned mask, uint16_t *v)
{
    if (a == 0177776) {
        if (write) {
            uint16_t value = (uint16_t)((c->psw & ~mask) | (*v & mask));
            /* Only the 11/04 lets an explicit PSW write change T. */
            if (c->model != PDP_04)
                value = (uint16_t)((value & ~PDP_T) | (c->psw & PDP_T));
            load_psw(c, value);
        } else {
            *v = c->psw;
        }
        return true;
    }
    if (c->model == PDP_04)
        return false;
    if (c->mmr3_alias && a == c->mmr3_alias)
        a = 0172516;

    static const uint16_t bases[] = {0172300, 0172200, 0, 0177600};
    for (unsigned m = 0; m < 4; ++m) {
        if (!bases[m] || a < bases[m] || a >= bases[m] + 0100)
            continue;
        unsigned i = (a - bases[m]) >> 1, space = (i >> 3) & 1;
        if (c->model == PDP_34 && (m == SUPERVISOR || space))
            return false;
        uint16_t *pdr = &c->pdr[m][space][i & 7];
        uint16_t *reg = i & 16 ? &c->par[m][space][i & 7] : pdr;
        if (write) {
            unsigned bits = i & 16 ? 0177777 : 0177416;
            if (c->model == PDP_34)
                bits = i & 16 ? 07777 : 077416;
            *reg = (uint16_t)(((*reg & ~mask) | (*v & mask)) & bits);
            *pdr &= (uint16_t)~0100;
        } else {
            *v = *reg;
        }
        return true;
    }

    if (c->model == PDP_44 && !c->direct_dma && a >= 0170200 && a <= 0170372) {
        unsigned offset = a - 0170200, shift = offset & 2 ? 16 : 0;
        uint32_t *base = &c->unibus[offset >> 2];
        if (write) {
            uint32_t wide = (uint32_t)mask << shift;
            *base = ((*base & ~wide) | ((uint32_t)(*v & mask) << shift)) & 017777776;
        } else {
            *v = (uint16_t)(*base >> shift);
        }
        return true;
    }

    uint16_t old;
    if (c->model == PDP_34 && (a < 0177572 || a > 0177576))
        return false;
    switch (a) {
    case 0177572: old = c->mmr[0]; break;
    case 0177574: old = c->model == PDP_34 ? 0 : c->mmr[1]; break;
    case 0177576: old = c->mmr[2]; break;
    case 0172516: old = c->mmr[3]; break;
    case 0177772: {
        unsigned n = pirq_level(c);
        old = (uint16_t)(c->pirq | (n << 5) | (n << 1));
        break;
    }
    case 0177766: old = c->cpuerr; break;
    case 0177746: old = c->ccsr; break;
    case 0177750: old = c->cmr; break;
    case 0177744: case 0177752: case 0177754: old = 0; break;
    default: return false;
    }
    if (!write) {
        *v = old;
        return true;
    }
    uint16_t value = (uint16_t)((old & ~mask) | (*v & mask));
    switch (a) {
    case 0177572: c->mmr[0] = value & 0160557; break;
    case 0172516: c->mmr[3] = value & 077; break;
    case 0177772: c->pirq = value & 0177000; break;
    case 0177766: c->cpuerr = 0; break;
    case 0177746:
        /* The flush command (bit 8) toggles the valid-bit bank and
         * completes at once; the other writable bits persist. */
        if (value & 000400)
            old ^= 020000;
        c->ccsr = (uint16_t)((old & 020000) | (value & 003315));
        break;
    case 0177750: c->cmr = value; break;
    default: break;
    }
    return true;
}

static uint32_t translate(Pdp11 *c, uint16_t a, unsigned m, bool data, bool write)
{
    bool reloc = c->mmr[0] & 1;
    if (c->model == PDP_04 || (!reloc && !(write && (c->mmr[0] & 0400))))
        return a >= PDP_IO_VIRTUAL ? PDP_IO_PAGE + (a - PDP_IO_VIRTUAL) : a;

    unsigned page = a >> 13;
    static const unsigned dbit[] = {4, 2, 0, 1};
    unsigned space = data && (c->mmr[3] & dbit[m]);
    uint16_t *pdr = &c->pdr[m][space][page];
    unsigned block = (a >> 6) & 0177, limit = (*pdr >> 8) & 0177;
    unsigned access = *pdr & 6;
    unsigned abort = access == 0 || access == 4 ? 0100000 : 0;
    if ((*pdr & 8) ? block < limit : block > limit)
        abort |= 0040000;
    if (write && access == 2)
        abort |= 0020000;
    if (abort) {
        if (!(c->mmr[0] & MM_ABORT))
            c->mmr[0] = (uint16_t)((c->mmr[0] & 0401) | abort | (m << 5) | (space << 4) | (page << 1));
        c->fault = V_MMU;
        c->last_address = a;
        return 0;
    }
    if (write)
        *pdr |= 0100;

    uint32_t physical = ((uint32_t)c->par[m][space][page] << 6) + (a & 017777);
    if (c->mmr[3] & 020)
        return physical & 017777777;
    physical &= 0777777;
    return physical >= IO18 ? PDP_IO_PAGE + (physical - IO18) : physical;
}

static uint16_t access(Pdp11 *c, uint16_t a, unsigned m, bool data, bool byte,
                       bool write, uint16_t value)
{
    if (c->fault)
        return 0;
    if (!byte && (a & 1)) {
        c->cpuerr |= 0100;
        c->fault = V_BUS;
        c->last_address = a;
        return 0;
    }
    uint32_t p = translate(c, a, m, data, write);
    if (c->fault)
        return 0;
    if (c->model == PDP_04 && p >= PDP_IO_PAGE + 017700 && p <= PDP_IO_PAGE + 017707) {
        /* The 11/04 exposes R0-R7 at 177700-177707, one byte address each. */
        uint16_t *reg = &c->r[p & 7];
        if (!write)
            return byte ? (uint16_t)((p & 1 ? *reg >> 8 : *reg) & 0377) : *reg;
        if (!byte)
            *reg = value;
        else if (p & 1)
            *reg = (uint16_t)((*reg & 0377) | (value << 8));
        else
            *reg = (uint16_t)((*reg & 0177400) | (value & 0377));
        return value;
    }
    unsigned shift = byte && (p & 1) ? 8 : 0;
    unsigned mask = byte ? 0377u << shift : 0177777u;
    uint16_t word = (uint16_t)(value << shift);
    if (p >= PDP_IO_PAGE && internal_io(c, (uint16_t)p & 0177776u, write, mask, &word))
        return (uint16_t)((word & mask) >> shift);
    if (c->bus.access(c->bus.context, p, write, byte, &value))
        return value;

    c->last_address = p;
    c->cpuerr |= p >= PDP_IO_PAGE ? 020 : 040;
    c->fault = V_BUS;
    return 0;
}

static uint16_t read_mem(Pdp11 *c, uint16_t a, bool data, bool byte)
{
    return access(c, a, mode(c), data, byte, false, 0);
}

uint16_t pdp_read(Pdp11 *c, uint16_t address, bool data)
{
    return access(c, address, mode(c), data, false, false, 0);
}

void pdp_write(Pdp11 *c, uint16_t address, bool data, uint16_t value)
{
    access(c, address, mode(c), data, false, true, value);
}

static uint16_t fetch(Pdp11 *c)
{
    uint16_t value = read_mem(c, c->r[7], false, false);
    if (!c->fault)
        c->r[7] += 2;
    return value;
}

static void adjust(Pdp11 *c, unsigned reg, int delta)
{
    if (c->fault)
        return;
    c->r[reg] = (uint16_t)(c->r[reg] + delta);
    if (c->model == PDP_44 && !(c->mmr[0] & MM_ABORT) && c->restart_bytes < 2) {
        unsigned change = (((unsigned)delta & 31) << 3) | reg;
        c->mmr[1] |= (uint16_t)(change << (8 * c->restart_bytes++));
    }
}

/* The fixed kernel stack limit of these models: a stack reference below
 * 000400 completes, then traps through 4. */
static void stack_check(Pdp11 *c, uint16_t sp)
{
    if (!c->fault && mode(c) == KERNEL && sp < 0400)
        c->yellow = true;
}

static void push(Pdp11 *c, uint16_t value)
{
    adjust(c, 6, -2);
    stack_check(c, c->r[6]);
    access(c, c->r[6], mode(c), true, false, true, value);
}

static uint16_t pop(Pdp11 *c)
{
    uint16_t value = read_mem(c, c->r[6], true, false);
    adjust(c, 6, 2);
    return value;
}

PdpOperand pdp_operand(Pdp11 *c, unsigned spec, unsigned step)
{
    unsigned reg = spec & 7;
    unsigned form = (spec >> 3) & 7;
    PdpOperand o = {c->r[reg], (uint8_t)reg, form == 0, true};
    if (reg == 7)
        step = 2;
    switch (form) {
    case 1:
        o.data = reg != 7;
        break;
    case 2:
        o.data = reg != 7;
        adjust(c, reg, (int)step);
        break;
    case 3:
        adjust(c, reg, 2);
        o.addr = read_mem(c, o.addr, reg != 7, false);
        break;
    case 4:
        o.data = reg != 7;
        adjust(c, reg, -(int)step);
        o.addr = c->r[reg];
        if (reg == 6)
            stack_check(c, o.addr);
        break;
    case 5:
        adjust(c, reg, -2);
        if (reg == 6)
            stack_check(c, c->r[reg]);
        o.addr = read_mem(c, c->r[reg], reg != 7, false);
        break;
    case 6: case 7: {
        uint16_t offset = fetch(c);
        o.addr = (uint16_t)(c->r[reg] + offset);
        if (form == 7)
            o.addr = read_mem(c, o.addr, true, false);
        break;
    }
    default: break;
    }
    return o;
}

static PdpOperand decode(Pdp11 *c, unsigned spec, bool byte)
{
    return pdp_operand(c, spec, byte && (spec & 7) < 6 ? 1 : 2);
}

static unsigned load_operand(Pdp11 *c, PdpOperand o, bool byte)
{
    unsigned value = o.direct ? c->r[o.reg] : read_mem(c, o.addr, o.data, byte);
    return byte ? value & 0377u : value;
}

static void store_operand(Pdp11 *c, PdpOperand o, unsigned value, bool byte, bool extend)
{
    if (c->fault)
        return;
    if (!o.direct) {
        access(c, o.addr, mode(c), o.data, byte, true, (uint16_t)value);
    } else if (!byte) {
        c->r[o.reg] = (uint16_t)value;
    } else if (extend) {
        c->r[o.reg] = (uint16_t)(value & 0200u ? value | 0177400u : value & 0377u);
    } else {
        c->r[o.reg] = (uint16_t)((c->r[o.reg] & 0177400u) | (value & 0377u));
    }
}

/* Trap or interrupt entry. A fault while reading the vector or pushing
 * the frame halts these models. */
static void enter(Pdp11 *c, unsigned vector)
{
    c->last_vector = (uint16_t)vector;
    c->fault = 0;
    c->waiting = false;
    uint16_t old = c->psw, pc = c->r[7];
    uint16_t next_pc = access(c, (uint16_t)vector, KERNEL, true, false, false, 0);
    uint16_t next_psw = access(c, (uint16_t)(vector + 2), KERNEL, true, false, false, 0);
    if (!c->fault) {
        next_psw &= psw_mask(c);
        load_psw(c, (uint16_t)((next_psw & ~0030000u) | ((old >> 2) & 0030000u)));
        push(c, old);
        push(c, pc);
    }
    if (c->fault) {
        c->double_fault = true;
        c->halted = true;
        return;
    }
    c->r[7] = next_pc;
}

static void stack_trap(Pdp11 *c)
{
    if (!c->yellow || c->halted)
        return;
    c->cpuerr |= 004;
    enter(c, V_BUS);
    c->yellow = false;
}

static void double_operand(Pdp11 *c, uint16_t op, unsigned major)
{
    bool byte = major >= 9 && major <= 13;
    unsigned base = byte ? major - 8 : major;
    unsigned mask = byte ? 0377u : 0177777u, sign = (mask + 1) >> 1;
    unsigned src = load_operand(c, decode(c, op >> 6, byte), byte);
    PdpOperand dst = decode(c, op, byte);
    unsigned d = base == 1 ? 0 : load_operand(c, dst, byte);
    if (c->fault)
        return;

    unsigned r = 0, f = 0, changed = PDP_N | PDP_Z | PDP_V;
    switch (base) {
    case 1: r = src; break;
    case 2:
        r = (src - d) & mask;
        f = ((src ^ d) & (src ^ r) & sign ? PDP_V : 0) | (src < d ? PDP_C : 0);
        changed |= PDP_C;
        break;
    case 3: r = d & src; break;
    case 4: r = d & ~src; break;
    case 5: r = d | src; break;
    case 6:
        r = (d + src) & mask;
        f = (~(src ^ d) & (d ^ r) & sign ? PDP_V : 0) | (d + src > mask ? PDP_C : 0);
        changed |= PDP_C;
        break;
    case 14:
        r = (d - src) & mask;
        f = ((src ^ d) & (d ^ r) & sign ? PDP_V : 0) | (d < src ? PDP_C : 0);
        changed |= PDP_C;
        break;
    }
    uint16_t old = c->psw;
    /* Condition codes first, so an explicit write to the PSW wins. */
    flags(c, changed, f | nz(r, sign));
    if (base != 2 && base != 3)
        store_operand(c, dst, r, byte, base == 1);
    if (c->fault)
        flags(c, 15, old);
}

static void single_operand(Pdp11 *c, uint16_t op, unsigned code, bool byte)
{
    PdpOperand dst = decode(c, op, byte);
    unsigned mask = byte ? 0377u : 0177777u;
    unsigned sign = (mask + 1) >> 1;
    unsigned carry = c->psw & PDP_C;
    unsigned d = code == 005000 || code == 006700 ? 0 : load_operand(c, dst, byte);
    unsigned r = 0, f = 0, changed = 15;
    bool shift = false;
    if (c->fault)
        return;
    switch (code) {
    case 000300:
        r = ((d << 8) | (d >> 8)) & 0177777u;
        sign = 0200;
        break;
    case 005000: break;
    case 005100: r = ~d; f = PDP_C; break;
    case 005200: r = d + 1; f = d == sign - 1 ? PDP_V : 0; changed &= ~PDP_C; break;
    case 005300: r = d - 1; f = d == sign ? PDP_V : 0; changed &= ~PDP_C; break;
    case 005400: r = 0u - d; f = (d == sign ? PDP_V : 0) | (d ? PDP_C : 0); break;
    case 005500: r = d + carry; f = carry && d == sign - 1 ? PDP_V : 0; f |= carry && d == mask; break;
    case 005600: r = d - carry; f = carry && d == sign ? PDP_V : 0; f |= carry && !d; break;
    case 005700: r = d; break;
    case 006000: r = (d >> 1) | (carry ? sign : 0); f = d & 1; shift = true; break;
    case 006100: r = (d << 1) | carry; f = !!(d & sign); shift = true; break;
    case 006200: r = (d >> 1) | (d & sign); f = d & 1; shift = true; break;
    case 006300: r = d << 1; f = !!(d & sign); shift = true; break;
    case 006700: r = c->psw & PDP_N ? mask : 0; changed &= ~(PDP_N | PDP_C); break;
    }
    r &= mask;
    if (shift && (!!(r & sign) != !!(f & PDP_C)))
        f |= PDP_V;
    uint16_t old = c->psw;
    flags(c, changed, f | nz(r & (2 * sign - 1), sign));
    if (code != 005700)
        store_operand(c, dst, r, byte, false);
    if (c->fault)
        flags(c, 15, old);
}

static int32_t signed16(unsigned x)
{
    return (int32_t)(x & 0177777) - (x & 0100000 ? 65536 : 0);
}

static void eis(Pdp11 *c, uint16_t op, unsigned sub)
{
    unsigned r = (op >> 6) & 7, pair = r | 1;
    PdpOperand dst = decode(c, op, false);
    unsigned value = load_operand(c, dst, false);
    if (c->fault)
        return;
    if (sub == 074000) {
        unsigned result = c->r[r] ^ value;
        uint16_t old = c->psw;
        flags(c, PDP_N | PDP_Z | PDP_V, nz(result, 0100000));
        store_operand(c, dst, result, false, false);
        if (c->fault)
            flags(c, 15, old);
        return;
    }
    if (sub == 070000) {
        int32_t product = signed16(c->r[r]) * signed16(value);
        c->r[r] = (uint16_t)((uint32_t)product >> 16);
        c->r[pair] = (uint16_t)product;
        flags(c, 15, nz((uint32_t)product, UINT32_C(0x80000000)) |
              (product < INT16_MIN || product > INT16_MAX ? PDP_C : 0));
        return;
    }
    if (sub == 071000) {
        int32_t divisor = signed16(value);
        int64_t dividend = (int64_t)signed16(c->r[r]) * 65536 + c->r[pair];
        if (!divisor) {
            flags(c, 15, PDP_Z | PDP_V | PDP_C);
        } else {
            int64_t quotient = dividend / divisor;
            if (quotient < INT16_MIN || quotient > INT16_MAX) {
                flags(c, 15, PDP_V | (quotient < 0 ? PDP_N : 0));
            } else {
                c->r[r] = (uint16_t)quotient;
                c->r[pair] = (uint16_t)(dividend % divisor);
                flags(c, 15, nz(c->r[r], 0100000));
            }
        }
        return;
    }
    bool wide = sub == 073000;
    int count = (int)(value & 077) - (value & 040 ? 64 : 0);
    uint32_t sign = wide ? UINT32_C(0x80000000) : 0100000u;
    uint32_t mask = wide ? UINT32_MAX : 0177777u;
    uint32_t bits = wide ? (uint32_t)c->r[r] << 16 | c->r[pair] : c->r[r];
    unsigned f = 0;
    bool carry = false;
    for (; count > 0; --count) {
        carry = (bits & sign) != 0;
        bits = (bits << 1) & mask;
        if (carry != ((bits & sign) != 0))
            f |= PDP_V;
    }
    for (; count < 0; ++count) {
        carry = (bits & 1) != 0;
        bits = (bits >> 1) | (bits & sign);
    }
    if (wide) {
        c->r[r] = (uint16_t)(bits >> 16);
        c->r[pair] = (uint16_t)bits;
    } else {
        c->r[r] = (uint16_t)bits;
    }
    flags(c, 15, f | (carry ? PDP_C : 0) | nz(bits, sign));
}

/* MFPI/MFPD and MTPI/MTPD. The address is computed in the current mode and
 * used in the previous one; MFPI between two user modes reads D space. */
static void prev_move(Pdp11 *c, uint16_t op, bool to_prev, bool data)
{
    unsigned cm = mode(c), pm = (c->psw >> 12) & 3;
    if (to_prev) {
        uint16_t value = pop(c);
        if (c->fault)
            return;
        flags(c, PDP_N | PDP_Z | PDP_V, nz(value, 0100000));
        PdpOperand dst = pdp_operand(c, op & 077, 2);
        if (c->fault)
            return;
        if (!dst.direct)
            access(c, dst.addr, pm, data, false, true, value);
        else if (dst.reg == 6 && pm != cm)
            c->sp[pm] = value;
        else
            c->r[dst.reg] = value;
        return;
    }
    PdpOperand src = pdp_operand(c, op & 077, 2);
    uint16_t value;
    if (!src.direct)
        value = access(c, src.addr, pm, data || (cm == USER && pm == USER), false, false, 0);
    else
        value = src.reg == 6 && pm != cm ? c->sp[pm] : c->r[src.reg];
    if (c->fault)
        return;
    flags(c, PDP_N | PDP_Z | PDP_V, nz(value, 0100000));
    push(c, value);
}

/* CSM (11/44): the supervisor stack starts at the caller's SP. */
static void call_supervisor(Pdp11 *c, uint16_t op)
{
    uint16_t argument = (uint16_t)load_operand(c, decode(c, op, false), false);
    if (c->fault)
        return;
    unsigned cm = mode(c);
    uint16_t old = c->psw, pc = c->r[7], sp = c->r[6];
    access(c, (uint16_t)(sp - 2), SUPERVISOR, true, false, true, (uint16_t)(old & ~017u));
    access(c, (uint16_t)(sp - 4), SUPERVISOR, true, false, true, pc);
    access(c, (uint16_t)(sp - 6), SUPERVISOR, true, false, true, argument);
    if (c->fault)
        return;
    c->sp[cm] = sp;
    c->psw = (uint16_t)((old & ~(0170000u | PDP_T)) | 0040000u | (cm << 12));
    c->r[6] = (uint16_t)(sp - 6);
    uint16_t target = access(c, 0010, SUPERVISOR, false, false, false, 0);
    if (!c->fault)
        c->r[7] = target;
}

static bool branch(Pdp11 *c, uint16_t op)
{
    bool n = c->psw & PDP_N, z = c->psw & PDP_Z;
    bool v = c->psw & PDP_V, cy = c->psw & PDP_C, take;
    switch (op & 0177400) {
    case 000400: take = true; break;
    case 001000: take = !z; break;
    case 001400: take = z; break;
    case 002000: take = n == v; break;
    case 002400: take = n != v; break;
    case 003000: take = !z && n == v; break;
    case 003400: take = z || n != v; break;
    case 0100000: take = !n; break;
    case 0100400: take = n; break;
    case 0101000: take = !cy && !z; break;
    case 0101400: take = cy || z; break;
    case 0102000: take = !v; break;
    case 0102400: take = v; break;
    case 0103000: take = !cy; break;
    case 0103400: take = cy; break;
    default: return false;
    }
    int offset = (op & 0377) - (op & 0200 ? 256 : 0);
    if (take)
        c->r[7] = (uint16_t)(c->r[7] + 2 * offset);
    return true;
}

static unsigned special(Pdp11 *c, uint16_t op)
{
    switch (op) {
    case 0:
        if (mode(c) == KERNEL) {
            c->halted = true;
            return 0;
        }
        if (c->model != PDP_44)
            return V_RESERVED;
        c->cpuerr |= 0200;
        return V_BUS;
    case 1: c->waiting = true; return 0;
    case 2: case 6: {
        uint16_t pc = pop(c), psw = pop(c);
        if (c->fault)
            return 0;
        if (mode(c) != KERNEL)
            psw = (uint16_t)((psw & 0170037) | (c->psw & 0170340));
        c->r[7] = pc;
        load_psw(c, psw);
        return 0;
    }
    case 3: return V_TRACE;
    case 4: return V_IOT;
    case 5:
        if (mode(c) == KERNEL) {
            c->mmr[0] = c->mmr[3] = c->pirq = 0;
            c->bus.reset(c->bus.context);
        }
        return 0;
    case 7:
        if (c->model != PDP_44)
            return V_RESERVED;
        c->r[0] = 1;
        return 0;
    default: return V_RESERVED;
    }
}

static unsigned execute(Pdp11 *c, uint16_t op)
{
    unsigned major = op >> 12, sub = op & 0177000;
    unsigned code = op & 0077700;
    bool byte = (op & 0100000) != 0;
    if ((major >= 1 && major <= 6) || (major >= 9 && major <= 14)) {
        double_operand(c, op, major);
        return 0;
    }
    if (major == 15)
        return c->fpu ? fp11_execute(c, op) : V_RESERVED;
    if (branch(c, op))
        return 0;
    if (sub == 004000 || (op & 0177700) == 000100) {
        if (!(op & 070))
            return c->model == PDP_44 ? V_RESERVED : V_BUS;
        PdpOperand dst = decode(c, op, false);
        if (sub == 004000) {
            unsigned r = (op >> 6) & 7;
            push(c, c->r[r]);
            if (!c->fault)
                c->r[r] = c->r[7];
        }
        if (!c->fault)
            c->r[7] = dst.addr;
        return 0;
    }
    if (major == 7) {
        if (c->model == PDP_04 || sub == 075000 || sub == 076000)
            return V_RESERVED;
        if (sub == 077000) {
            if (--c->r[(op >> 6) & 7])
                c->r[7] = (uint16_t)(c->r[7] - 2 * (op & 077));
        } else {
            eis(c, op, sub);
        }
        return 0;
    }
    if ((!byte && code == 000300) || (code >= 005000 && code <= 006300) ||
        (!byte && code == 006700)) {
        if (code == 006700 && c->model == PDP_04)
            return V_RESERVED;
        single_operand(c, op, code, byte);
        return 0;
    }
    if (code == 006500 || code == 006600) {
        if (c->model == PDP_04)
            return V_RESERVED;
        prev_move(c, op, code == 006600, byte);
        return 0;
    }
    if (!byte && code == 006400) {
        if (c->model == PDP_04)
            return V_RESERVED;
        c->r[6] = (uint16_t)(c->r[7] + (op & 077) * 2);
        c->r[7] = c->r[5];
        uint16_t r5 = pop(c);
        if (!c->fault)
            c->r[5] = r5;
        return 0;
    }
    if (byte && (code == 006400 || code == 006700)) {
        if (c->model != PDP_34)
            return V_RESERVED;
        PdpOperand operand = decode(c, op, true);
        if (code == 006400) {
            unsigned value = load_operand(c, operand, true);
            if (!c->fault)
                flags(c, mode(c) == KERNEL ? 0357u : 017u, value);
        } else if (!c->fault) {
            unsigned value = c->psw & 0377u;
            store_operand(c, operand, value, true, true);
            if (!c->fault)
                flags(c, PDP_N | PDP_Z | PDP_V, nz(value, 0200));
        }
        return 0;
    }
    if (!byte && code == 007000 && c->model == PDP_44 && (c->mmr[3] & 010) && mode(c) != KERNEL) {
        call_supervisor(c, op);
        return 0;
    }
    if ((op & 0177770) == 000200) {
        unsigned r = op & 7;
        c->r[7] = c->r[r];
        uint16_t result = pop(c);
        if (!c->fault)
            c->r[r] = result;
        return 0;
    }
    if ((op & 0177770) == 000230) {
        if (c->model != PDP_44)
            return V_RESERVED;
        if (mode(c) == KERNEL)
            flags(c, 0340, (op & 7) << 5);
        return 0;
    }
    if ((op & 0177740) == 000240) {
        flags(c, op & 15, op & 020 ? 15 : 0);
        return 0;
    }
    if (sub == 0104000)
        return op & 0400 ? V_TRAP : V_EMT;
    return special(c, op);
}

void pdp_start(Pdp11 *c, PdpModel model, bool fpu, PdpBus bus, uint16_t pc)
{
    memset(c, 0, sizeof(*c));
    c->model = model;
    c->fpu = fpu && pdp_fpu_option(model);
    c->bus = bus;
    c->r[7] = pc;
    fp11_reset(&c->fp);
}

void pdp_step(Pdp11 *c)
{
    if (c->halted)
        return;
    /* A device request beats a program interrupt request of the same level. */
    unsigned ipl = (c->psw >> 5) & 7;
    unsigned level = pirq_level(c);
    unsigned threshold = level > ipl ? level - 1 : ipl;
    unsigned vector = c->bus.interrupt(c->bus.context, threshold, false);
    if (vector) {
        c->bus.interrupt(c->bus.context, threshold, true);
        enter(c, vector);
        stack_trap(c);
        return;
    }
    if (level > ipl) {
        enter(c, V_PIRQ);
        stack_trap(c);
        return;
    }
    if (c->waiting)
        return;

    bool trace = (c->psw & PDP_T) != 0;
    c->instruction_pc = c->r[7];
    c->yellow = false;
    if (!(c->mmr[0] & MM_ABORT)) {
        c->restart_bytes = 0;
        c->mmr[1] = 0;
        if (c->model == PDP_44)
            c->mmr[2] = c->r[7];
    }
    uint16_t op = fetch(c);
    vector = 0;
    if (!c->fault) {
        if (c->model == PDP_34 && !(c->mmr[0] & MM_ABORT))
            c->mmr[2] = c->instruction_pc;
        c->opcode = op;
        ++c->instructions;
        vector = execute(c, op);
        /* RTI loading T traps at once; RTT waits one instruction. */
        if (op == 2 && (c->psw & PDP_T))
            trace = true;
    }
    if (c->fault) {
        c->yellow = false;
        enter(c, c->fault);
    } else if (c->halted) {
        return;
    } else if (vector) {
        enter(c, vector);
    } else if (trace) {
        enter(c, V_TRACE);
    }
    stack_trap(c);
}
