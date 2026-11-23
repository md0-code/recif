#include "rp11.h"

#include <stdlib.h>
#include <string.h>

/* Registers and functions as in the RP11-C maintenance manual, tables 3-2
 * to 3-6. Images are flat arrays of 512-byte sectors in cylinder, track,
 * sector order (406 x 20 x 10); unwritten space reads as zero. */

enum {
    DS_RDY = 0100000, DS_OL = 040000, DS_RP03 = 020000, DS_SU = 02000, DS_WP = 0400,
    CS_ERR = 0100000, CS_HE = 040000, CS_AIE = 020000, CS_HDR = 04000, CS_RDY = 0200,
    CS_IDE = 0100, CS_MEX = 060, CS_GO = 1, CS_WRITABLE = 037576,
    ER_EOP = 02, ER_NXME = 04, ER_WCE = 010, ER_MODE = 0400, ER_PROG = 02000,
    ER_NXS = 04000, ER_NXT = 010000, ER_NXC = 020000, ER_FUV = 040000, ER_WPV = 0100000,
    ER_DATA = 0370
};
enum { F_RESET, F_WRITE, F_READ, F_WCHK, F_SEEK, F_WRITE_HERE, F_HOME, F_READ_HERE };
enum { WORDS = 256, SEEK_TICKS = 10, ROTATION_TICKS = 10, INITIATE_TICKS = 2 };

uint32_t rp11_blocks(void)
{
    return RP03_CYLINDERS * RP03_TRACKS * RP03_SECTORS;
}

static unsigned function(const Rp11 *rp)
{
    return (rp->cs >> 1) & 7;
}

static unsigned selected(const Rp11 *rp)
{
    return (rp->cs >> 8) & 7;
}

static RpDrive *drive(Rp11 *rp)
{
    unsigned u = selected(rp);
    return u < RP11_DRIVES && rp->drive[u].file ? &rp->drive[u] : NULL;
}

bool rp11_busy(const Rp11 *rp)
{
    if (rp->done_at)
        return true;
    for (unsigned i = 0; i < RP11_DRIVES; ++i)
        if (rp->drive[i].seek_done)
            return true;
    return false;
}

bool rp11_interrupt(const Rp11 *rp)
{
    return rp->done_irq || ((rp->cs & CS_AIE) && rp->attention);
}

/* The attention interrupt clears AIE; the done interrupt its request. */
void rp11_acknowledge(Rp11 *rp)
{
    if (rp->done_irq)
        rp->done_irq = false;
    else
        rp->cs &= (uint16_t)~CS_AIE;
}

static void finish(Rp11 *rp)
{
    rp->done_at = 0;
    if (rp->cs & CS_IDE)
        rp->done_irq = true;
}

static void clear_controller(Rp11 *rp)
{
    rp->cs &= 03400;
    rp->er = rp->wc = rp->ba = rp->ca = rp->da = 0;
    rp->done_at = 0;
    rp->done_irq = false;
}

void rp11_reset(Rp11 *rp)
{
    clear_controller(rp);
    rp->cs = 0;
    for (unsigned i = 0; i < RP11_DRIVES; ++i)
        rp->drive[i].seek_done = 0;
}

bool rp11_mount(Rp11 *rp, unsigned u, const char *path, char *error, size_t size)
{
    if (u >= RP11_DRIVES)
        return false;
    bool read_only = false;
    FILE *file = fopen(path, "r+b");
    if (!file) {
        file = fopen(path, "rb");
        read_only = file != NULL;
    }
    if (!file)
        file = fopen(path, "w+b");
    if (!file) {
        snprintf(error, size, "Cannot open disk image %.300s", path);
        return false;
    }
    RpDrive *d = &rp->drive[u];
    if (d->file)
        fclose(d->file);
    *d = (RpDrive){file, read_only, 0, 0, 0};
    return true;
}

void rp11_eject(Rp11 *rp, unsigned u)
{
    if (u < RP11_DRIVES && rp->drive[u].file) {
        fclose(rp->drive[u].file);
        rp->drive[u] = (RpDrive){0};
    }
}

void rp11_close(Rp11 *rp)
{
    for (unsigned i = 0; i < RP11_DRIVES; ++i)
        rp11_eject(rp, i);
}

static void seek(RpDrive *d, uint16_t cylinder, uint16_t track, uint64_t now)
{
    unsigned distance = (unsigned)abs((int)cylinder - (int)d->cylinder);
    uint64_t from = d->seek_done > now ? d->seek_done : now;
    d->seek_done = from + SEEK_TICKS * (distance ? distance : 1);
    d->cylinder = cylinder;
    d->track = track;
}

static void start(Rp11 *rp, uint64_t now)
{
    unsigned f = function(rp);
    rp->er = 0;
    if (f == F_RESET) {
        clear_controller(rp);
        return;
    }
    RpDrive *d = drive(rp);
    uint16_t cylinder = rp->ca & 0777, track = (rp->da >> 8) & 037, sector = rp->da & 017;
    bool here = f == F_WRITE_HERE || f == F_READ_HERE;
    if (!d) {
        rp->er |= ER_PROG;
    } else if (f == F_SEEK || f == F_HOME) {
        if (f == F_HOME)
            cylinder = 0;
        if (cylinder >= RP03_CYLINDERS)
            rp->er |= ER_NXC;
        else if (track >= RP03_TRACKS)
            rp->er |= ER_NXT;
        else
            seek(d, cylinder, track, now);
        rp->done_at = now + INITIATE_TICKS;
        return;
    } else {
        if (here) {
            cylinder = d->cylinder;
            track = d->track;
        }
        if (!rp->wc)
            rp->er |= ER_PROG;
        if (rp->cs & CS_HDR)
            rp->er |= ER_MODE;
        if (sector >= RP03_SECTORS)
            rp->er |= ER_NXS;
        if (track >= RP03_TRACKS)
            rp->er |= ER_NXT;
        if (cylinder >= RP03_CYLINDERS)
            rp->er |= ER_NXC;
        if ((f == F_WRITE || f == F_WRITE_HERE) && d->read_only)
            rp->er |= ER_WPV;
        if (!rp->er) {
            uint64_t ready = d->seek_done > now ? d->seek_done : now;
            if (!here)
                seek(d, cylinder, track, now);
            rp->done_at = (d->seek_done > ready ? d->seek_done : ready) + ROTATION_TICKS;
            return;
        }
    }
    rp->done_at = now + 1;
}

static bool dma(Rp11 *rp, Pdp11 *cpu, uint32_t *address, bool write, uint16_t *word)
{
    uint32_t physical = pdp_dma_address(cpu, *address);
    if (physical >= PDP_IO_PAGE || !cpu->bus.access(cpu->bus.context, physical, write, false, word)) {
        rp->er |= ER_NXME;
        return false;
    }
    *address = (*address + 2) & 0777777;
    return true;
}

static void transfer(Rp11 *rp, Pdp11 *cpu)
{
    RpDrive *d = drive(rp);
    unsigned f = function(rp);
    if (!d)
        return;
    uint32_t total = rp11_blocks() * WORDS;
    uint32_t block = ((uint32_t)d->cylinder * RP03_TRACKS + d->track) * RP03_SECTORS + (rp->da & 017);
    uint32_t start = block * WORDS, words = 0200000u - rp->wc;
    if (start + words > total) {
        rp->er |= ER_EOP;
        words = total - start;
    }
    uint32_t sectors = (words + WORDS - 1) / WORDS;
    uint8_t *buffer = calloc(sectors ? sectors : 1, WORDS * 2);
    if (!buffer) {
        rp->er |= ER_FUV;
        return;
    }
    bool writing = f == F_WRITE || f == F_WRITE_HERE;
    long offset = (long)start * 2;
    if (!writing && fseek(d->file, offset, SEEK_SET) == 0)
        (void)fread(buffer, 1, (size_t)sectors * WORDS * 2, d->file);
    uint32_t address = rp->ba | ((uint32_t)(rp->cs & CS_MEX) << 12);
    uint32_t done = 0;
    for (; done < words; ++done) {
        uint16_t word = (uint16_t)(buffer[done * 2] | buffer[done * 2 + 1] << 8);
        if (writing) {
            if (!dma(rp, cpu, &address, false, &word))
                break;
            buffer[done * 2] = (uint8_t)word;
            buffer[done * 2 + 1] = (uint8_t)(word >> 8);
        } else if (f == F_WCHK) {
            uint16_t memory = 0;
            if (!dma(rp, cpu, &address, false, &memory))
                break;
            if (memory != word) {
                rp->er |= ER_WCE;
                break;
            }
        } else if (!dma(rp, cpu, &address, true, &word)) {
            break;
        }
    }
    /* A write fills the rest of its last sector with zeros. */
    if (writing && done && fseek(d->file, offset, SEEK_SET) == 0) {
        size_t bytes = (size_t)((done + WORDS - 1) / WORDS) * WORDS * 2;
        if (fwrite(buffer, 1, bytes, d->file) != bytes || fflush(d->file))
            rp->er |= ER_FUV;
    }
    free(buffer);

    rp->wc = (uint16_t)(rp->wc + done);
    rp->ba = (uint16_t)address;
    rp->cs = (uint16_t)((rp->cs & ~CS_MEX) | ((address >> 12) & CS_MEX));
    uint32_t next = (start + done + WORDS - 1) / WORDS;
    if (next >= rp11_blocks())
        next = rp11_blocks() - 1;
    uint16_t cylinder = (uint16_t)(next / (RP03_TRACKS * RP03_SECTORS));
    if (cylinder != d->cylinder)
        rp->attention |= (uint16_t)(1u << selected(rp));
    d->cylinder = cylinder;
    d->track = (uint16_t)(next / RP03_SECTORS % RP03_TRACKS);
    rp->ca = cylinder;
    rp->da = (uint16_t)((d->track << 8) | (next % RP03_SECTORS));
}

void rp11_tick(Rp11 *rp, Pdp11 *cpu, uint64_t now)
{
    for (unsigned i = 0; i < RP11_DRIVES; ++i) {
        RpDrive *d = &rp->drive[i];
        if (d->seek_done && now >= d->seek_done) {
            d->seek_done = 0;
            unsigned f = function(rp);
            /* A seek or home seek reports through attention. */
            if (selected(rp) != i || f == F_SEEK || f == F_HOME || !rp->done_at)
                rp->attention |= (uint16_t)(1u << i);
        }
    }
    if (!rp->done_at || now < rp->done_at)
        return;
    unsigned f = function(rp);
    if (!rp->er && f != F_SEEK && f != F_HOME)
        transfer(rp, cpu);
    finish(rp);
}

static uint16_t status(const Rp11 *rp)
{
    uint16_t value = rp->attention & 0377;
    unsigned u = selected(rp);
    if (u < RP11_DRIVES) {
        const RpDrive *d = &rp->drive[u];
        value |= DS_RP03;
        if (d->file) {
            value |= DS_OL | (d->read_only ? DS_WP : 0);
            value |= d->seek_done ? DS_SU : DS_RDY;
        }
    }
    return value;
}

static uint16_t control(const Rp11 *rp)
{
    uint16_t value = rp->cs & CS_WRITABLE;
    if (!rp->done_at)
        value |= CS_RDY;
    if (rp->er)
        value |= CS_ERR;
    if (rp->er & ~ER_DATA)
        value |= CS_HE;
    return value;
}

bool rp11_io(Rp11 *rp, uint16_t address, bool write, bool byte, uint16_t *value, uint64_t now)
{
    if (address < RP11_CSR || address > RP11_CSR + 027)
        return false;
    unsigned reg = (address - RP11_CSR) >> 1;
    bool high = byte && (address & 1);
    if (!write) {
        uint16_t v = 0;
        switch (reg) {
        case 0: v = status(rp); break;
        case 1: v = rp->er; break;
        case 2: v = control(rp); break;
        case 3: v = rp->wc; break;
        case 4: v = rp->ba; break;
        case 5: v = rp->ca; break;
        case 6: v = (uint16_t)((rp->da & 017417) | ((now / 40 % RP03_SECTORS) << 4)); break;
        case 10: v = drive(rp) ? drive(rp)->cylinder : 0; break;
        case 11: v = rp->silo; break;
        default: break;
        }
        *value = byte ? (uint16_t)((high ? v >> 8 : v) & 0377) : v;
        return true;
    }
    uint16_t data = byte ? (uint16_t)(high ? *value << 8 : *value & 0377) : *value;
    uint16_t mask = byte ? (high ? 0177400 : 0377) : 0177777;
    switch (reg) {
    case 0:
        rp->attention &= (uint16_t)~(data & mask & 0377);
        break;
    case 2: {
        uint16_t next = (uint16_t)((rp->cs & ~mask) | (data & mask));
        if (rp->done_at) {
            /* Busy: only the interrupt enables change; GO is refused. */
            rp->cs = (uint16_t)((rp->cs & ~(CS_IDE | CS_AIE)) | (next & (CS_IDE | CS_AIE)));
            if (data & mask & CS_GO)
                rp->er |= ER_PROG;
            break;
        }
        rp->cs = next & CS_WRITABLE;
        if (data & mask & CS_GO)
            start(rp, now);
        break;
    }
    case 3: rp->wc = (uint16_t)((rp->wc & ~mask) | (data & mask)); break;
    case 4: rp->ba = (uint16_t)(((rp->ba & ~mask) | (data & mask)) & ~1u); break;
    case 5: rp->ca = (uint16_t)(((rp->ca & ~mask) | (data & mask)) & 0777); break;
    case 6: rp->da = (uint16_t)(((rp->da & ~mask) | (data & mask)) & 017417); break;
    case 11: rp->silo = (uint16_t)((rp->silo & ~mask) | (data & mask)); break;
    default: break;
    }
    return true;
}
