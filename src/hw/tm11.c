#include "tm11.h"

enum {
    GO = 000001, IE = 000100, READY = 000200, CLEAR = 010000, ERR = 0100000,
    TUR = 000001, REWIND = 000002, WRL = 000004, BOT = 000040, SELR = 000100,
    NXM = 000200, BTE = 000400, RLE = 001000, PARITY = 010000, EOF_MARK = 040000, ILC = 0100000
};

static bool online(const Tm11 *tm)
{
    return tm->medium.bytes && !tm->offline && !(tm->command & 003400);
}

static unsigned function(const Tm11 *tm)
{
    return (tm->command >> 1) & 7;
}

void tm11_reset(Tm11 *tm)
{
    tm->command = READY;
    tm->errors = tm->count = tm->dma = tm->data = tm->read_lines = 0;
    tm->ready_at = 0;
    tm->irq = false;
}

void tm11_changed_medium(Tm11 *tm)
{
    tm11_reset(tm);
    tm->offline = false;
    tm->medium.position = 0;
}

static uint16_t status(const Tm11 *tm)
{
    uint16_t value = tm->errors;
    if (!online(tm))
        return value;
    value |= SELR | WRL;
    if (tm->ready_at && function(tm) == 7)
        value |= REWIND;
    if (!tm->ready_at)
        value |= TUR;
    if (!tm->medium.position)
        value |= BOT;
    return value;
}

bool tm11_io(Tm11 *tm, uint16_t reg, bool write, bool byte, uint16_t *v, uint64_t now)
{
    uint16_t *dest = NULL;
    uint16_t old = 0;
    switch (reg & 0177776) {
    case 0172520: old = status(tm); break;
    case 0172522: old = tm->command; break;
    case 0172524: dest = &tm->count; break;
    case 0172526: dest = &tm->dma; break;
    case 0172530: dest = &tm->data; break;
    case 0172532: dest = &tm->read_lines; break;
    default: return false;
    }
    if (dest)
        old = *dest;
    unsigned shift = byte && (reg & 1) ? 8 : 0;
    unsigned mask = byte ? 0377u << shift : 0177777u;
    if (!write) {
        *v = (uint16_t)((old & mask) >> shift);
        return true;
    }
    uint16_t value = (uint16_t)((old & ~mask) | (((unsigned)*v << shift) & mask));
    if ((reg & 0177776) == 0172522) {
        if (value & CLEAR) {
            tm11_reset(tm);
        } else if (tm->ready_at) {
            tm->command = (uint16_t)((old & ~IE) | (value & IE));
            if (!(value & IE))
                tm->irq = false;
        } else {
            tm->command = (uint16_t)((old & (READY | ERR)) | (value & 067577));
            if (!(value & IE))
                tm->irq = false;
            else if (!(old & IE))
                tm->irq = true;
            if ((mask & 1) && (value & GO)) {
                tm->command &= (uint16_t)~(READY | ERR | GO);
                tm->errors = 0;
                tm->irq = false;
                tm->ready_at = now + (function(tm) == 7 ? 2400 : 320);
            }
        }
    } else if (dest && !tm->ready_at) {
        if ((reg & 0177776) == 0172532)
            value &= 030000;
        *dest = value;
    }
    return true;
}

static const TapeRecord *next_record(Tm11 *tm, bool reverse)
{
    const TapeRecord *r = tape_step(&tm->medium, reverse);
    if (!r)
        tm->errors |= reverse ? 0 : BTE;
    else if (r->kind == TAPE_MARK)
        tm->errors |= EOF_MARK;
    else if (r->kind == TAPE_BAD && function(tm) == 1)
        tm->errors |= PARITY;
    return r;
}

static void read_record(Tm11 *tm, Pdp11 *cpu)
{
    const TapeRecord *r = next_record(tm, false);
    if (!r || r->kind == TAPE_MARK)
        return;
    unsigned count = tm->count ? 65536u - tm->count : 65536u;
    if (r->length > count)
        tm->errors |= RLE;
    unsigned limit = r->length < count ? r->length : count;
    uint32_t address = tm->dma | ((uint32_t)(tm->command & 060) << 12);
    unsigned transferred = 0;
    while (transferred < limit) {
        uint32_t physical = pdp_dma_address(cpu, address);
        uint16_t value = tm->medium.bytes[r->offset + transferred];
        if (physical >= 017760000u || !cpu->bus.access(cpu->bus.context, physical, true, true, &value)) {
            tm->errors |= NXM;
            break;
        }
        tm->data = value;
        address = (address + 1) & 0777777u;
        ++transferred;
    }
    tm->count = (uint16_t)(tm->count + transferred);
    tm->dma = (uint16_t)address;
    tm->command = (uint16_t)((tm->command & ~060) | ((address >> 12) & 060));
}

void tm11_tick(Tm11 *tm, Pdp11 *cpu, uint64_t now)
{
    if (!tm->ready_at || now < tm->ready_at)
        return;
    tm->ready_at = 0;
    if (!online(tm)) {
        tm->errors |= ILC;
    } else {
        switch (function(tm)) {
        case 0:
            tm->medium.position = 0;
            tm->offline = true;
            break;
        case 1: read_record(tm, cpu); break;
        case 4: case 5: {
            bool reverse = function(tm) == 5;
            do {
                ++tm->count;
                if (!next_record(tm, reverse))
                    break;
            } while (tm->count && !tm->errors);
            break;
        }
        case 7: tm->medium.position = 0; break;
        default: tm->errors |= ILC; break;
        }
    }
    tm->command |= READY;
    if (tm->errors)
        tm->command |= ERR;
    tm->irq = (tm->command & IE) != 0;
}
