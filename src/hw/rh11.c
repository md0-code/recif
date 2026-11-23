#include "rh11.h"

#include <stdlib.h>
#include <string.h>

/* Register semantics from the DEC RH11 and RP04/05/06 and RM02/03
 * maintenance documentation, cross-checked against the pinned Open SIMH
 * revision. Images are flat arrays of 512-byte sectors in cylinder,
 * track, sector order; unwritten space reads as zero. */

enum {
    CS1_SC = 0100000, CS1_TRE = 040000, CS1_MCPE = 020000, CS1_DVA = 04000,
    CS1_BAE = 01400, CS1_RDY = 0200, CS1_IE = 0100, CS1_FNC = 076, CS1_GO = 1,
    CS2_ERR = 0177400, CS2_WCE = 040000, CS2_NED = 010000, CS2_NEM = 04000,
    CS2_PGE = 02000, CS2_OR = 0200, CS2_IR = 0100, CS2_CLR = 040, CS2_BAI = 010,
    CS2_RW = 021037,
    DS_ATA = 0100000, DS_ERR = 040000, DS_PIP = 020000, DS_MOL = 010000,
    DS_WRL = 04000, DS_LST = 02000, DS_DPR = 0400, DS_DRY = 0200, DS_VV = 0100, DS_OFM = 1,
    ER1_ILF = 1, ER1_RMR = 4, ER1_AOE = 01000, ER1_IAE = 02000, ER1_WLE = 04000, ER1_UNS = 040000,
    OF_WRITABLE = 016377, DA_WRITABLE = 037477, DC_WRITABLE = 01777
};
enum {
    F_NOP = 0, F_UNLOAD = 1, F_SEEK = 2, F_RECAL = 3, F_DCLR = 4, F_RELEASE = 5,
    F_OFFSET = 6, F_RETURN = 7, F_PRESET = 010, F_PACK = 011, F_SEARCH = 014,
    F_TRANSFER = 024, F_WCHK = 024, F_WRITE = 030, F_WRITEH = 031, F_READ = 034, F_READH = 035
};
enum { WORDS = 256, SEEK_TICKS = 10, ROTATION_TICKS = 10 };

typedef struct {
    unsigned sectors, surfaces, cylinders;
    uint16_t type;
    bool rm;
} Geometry;

static const Geometry geometry[] = {
    [DISK_RP06] = {22, 19, 815, 020022, false},
    [DISK_RM03] = {32, 5, 823, 020024, true},
};

const char *rh11_type_name(DiskType type)
{
    return type == DISK_RM03 ? "RM03" : "RP06";
}

uint32_t rh11_blocks(DiskType type)
{
    const Geometry *g = &geometry[type];
    return g->sectors * g->surfaces * g->cylinders;
}

static unsigned unit(const Rh11 *rh)
{
    return rh->cs2 & 7;
}

static bool mounted(const RhDrive *d)
{
    return d->file != NULL;
}

static void update_status(RhDrive *d, uint16_t set)
{
    d->ds |= DS_DPR;
    if (mounted(d))
        d->ds |= DS_MOL;
    else
        d->ds &= (uint16_t)~(DS_MOL | DS_VV | DS_DRY);
    if (d->er1)
        d->ds |= DS_ERR;
    else
        d->ds &= (uint16_t)~DS_ERR;
    d->ds |= set;
}

static bool attention(const Rh11 *rh)
{
    for (unsigned i = 0; i < RH11_DRIVES; ++i)
        if (rh->drive[i].ds & DS_ATA)
            return true;
    return false;
}

static uint16_t cs1_status(const Rh11 *rh)
{
    uint16_t value = rh->cs1;
    if (rh->cs2 & CS2_ERR)
        value |= CS1_TRE;
    if ((value & CS1_TRE) || attention(rh))
        value |= CS1_SC;
    return value;
}

bool rh11_interrupt(const Rh11 *rh)
{
    uint16_t cs1 = cs1_status(rh);
    return rh->iff || ((cs1 & CS1_SC) && (cs1 & CS1_RDY) && (cs1 & CS1_IE));
}

void rh11_acknowledge(Rh11 *rh)
{
    rh->cs1 &= (uint16_t)~CS1_IE;
    rh->iff = false;
}

static void set_done(Rh11 *rh, bool error)
{
    if (!(rh->cs1 & CS1_RDY))
        rh->iff = (rh->cs1 & CS1_IE) != 0;
    rh->cs1 |= CS1_RDY;
    if (error)
        rh->cs1 |= CS1_TRE;
}

static void drive_error(RhDrive *d, uint16_t error)
{
    d->er1 |= error;
    update_status(d, DS_ATA);
}

static void reset_drive(RhDrive *d)
{
    d->done_at = 0;
    d->function = d->da = d->er1 = d->mr = d->of = d->dc = d->cylinder = d->hr = d->mr2 = 0;
    d->ds = (uint16_t)(d->ds & DS_VV);
    if (mounted(d))
        d->ds |= DS_DRY | DS_MOL | (d->read_only ? DS_WRL : 0);
    update_status(d, 0);
}

void rh11_reset(Rh11 *rh)
{
    rh->cs1 = CS1_RDY;
    rh->ba = rh->cs2 = rh->db = rh->bae = rh->cs3 = 0;
    rh->iff = false;
    for (unsigned i = 0; i < RH11_DRIVES; ++i)
        reset_drive(&rh->drive[i]);
}

bool rh11_mount(Rh11 *rh, unsigned u, const char *path, char *error, size_t size)
{
    if (u >= RH11_DRIVES)
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
    RhDrive *d = &rh->drive[u];
    if (d->file)
        fclose(d->file);
    d->file = file;
    d->read_only = read_only;
    /* A new pack: the drive comes on line with volume valid clear. */
    d->ds = 0;
    reset_drive(d);
    update_status(d, DS_ATA);
    return true;
}

void rh11_eject(Rh11 *rh, unsigned u)
{
    if (u >= RH11_DRIVES || !rh->drive[u].file)
        return;
    fclose(rh->drive[u].file);
    rh->drive[u].file = NULL;
    rh->drive[u].done_at = 0;
    update_status(&rh->drive[u], DS_ATA);
}

void rh11_close(Rh11 *rh)
{
    for (unsigned i = 0; i < RH11_DRIVES; ++i)
        rh11_eject(rh, i);
}

bool rh11_busy(const Rh11 *rh)
{
    for (unsigned i = 0; i < RH11_DRIVES; ++i)
        if (rh->drive[i].done_at)
            return true;
    return false;
}

static bool valid_address(const RhDrive *d, uint16_t cylinder)
{
    const Geometry *g = &geometry[d->type];
    unsigned track = (d->da >> 8) & 037u, sector = d->da & 037u;
    return cylinder < g->cylinders && track < g->surfaces && sector < g->sectors;
}

static void schedule(RhDrive *d, uint16_t cylinder, uint64_t now, unsigned extra)
{
    unsigned distance = (unsigned)abs((int)cylinder - (int)d->cylinder);
    d->done_at = now + extra + SEEK_TICKS * (distance ? distance : 1);
    d->cylinder = cylinder;
}

/* Starts a drive function. False when it refused, so a data transfer
 * leaves the adapter ready. */
static bool go(Rh11 *rh, RhDrive *d, uint64_t now)
{
    unsigned f = d->function;
    d->ds &= (uint16_t)~DS_ATA;
    if (f != F_DCLR && (d->ds & DS_ERR)) {
        drive_error(d, ER1_ILF);
        return false;
    }
    uint16_t cylinder = d->dc;
    switch (f) {
    case F_RELEASE: case F_DCLR:
        d->er1 = 0;
        if (geometry[d->type].rm)
            d->mr = 0;
        update_status(d, 0);
        return true;
    case F_NOP:
        return true;
    case F_PRESET:
        d->dc = d->da = d->of = 0;
        /* fall through */
    case F_PACK:
        if (mounted(d))
            d->ds |= DS_VV;
        update_status(d, 0);
        return true;
    case F_UNLOAD:
        if (geometry[d->type].rm) {
            drive_error(d, ER1_ILF);
            return false;
        }
        rh11_eject(rh, (unsigned)(d - rh->drive));
        return true;
    case F_OFFSET: case F_RETURN:
        if (!mounted(d)) {
            drive_error(d, ER1_UNS);
            return false;
        }
        d->ds = (uint16_t)((d->ds & ~DS_DRY) | DS_PIP);
        d->done_at = now + SEEK_TICKS;
        return true;
    case F_RECAL:
        cylinder = 0;
        /* fall through */
    case F_SEEK: case F_SEARCH:
        if (!mounted(d)) {
            drive_error(d, ER1_UNS);
            return false;
        }
        if (!valid_address(d, cylinder)) {
            drive_error(d, ER1_IAE);
            return false;
        }
        d->ds = (uint16_t)((d->ds & ~DS_DRY) | DS_PIP);
        schedule(d, cylinder, now, 0);
        return true;
    case F_WCHK: case F_WRITE: case F_WRITEH: case F_READ: case F_READH:
        if (!mounted(d)) {
            drive_error(d, ER1_UNS);
            return false;
        }
        if (!valid_address(d, cylinder)) {
            drive_error(d, ER1_IAE);
            return false;
        }
        d->ds &= (uint16_t)~DS_DRY;
        schedule(d, cylinder, now, ROTATION_TICKS);
        return true;
    default:
        drive_error(d, ER1_ILF);
        return false;
    }
}

static bool dma(Rh11 *rh, Pdp11 *cpu, uint32_t *address, bool write, uint16_t *word)
{
    uint32_t wrap = rh->extended ? 017777777 : 0777777;
    uint32_t physical = pdp_dma_address(cpu, *address);
    if (physical >= PDP_IO_PAGE || !cpu->bus.access(cpu->bus.context, physical, write, false, word)) {
        rh->cs2 |= CS2_NEM;
        return false;
    }
    if (!(rh->cs2 & CS2_BAI))
        *address = (*address + 2) & wrap;
    return true;
}

static void transfer(Rh11 *rh, RhDrive *d, Pdp11 *cpu)
{
    const Geometry *g = &geometry[d->type];
    uint32_t total = rh11_blocks(d->type) * WORDS;
    uint32_t block = ((uint32_t)d->dc * g->surfaces + ((d->da >> 8) & 037)) * g->sectors + (d->da & 037);
    uint32_t start = block * WORDS;
    uint32_t words = rh->wc ? 0200000u - rh->wc : 0200000u;
    bool error = false;
    if (start + words > total) {
        d->er1 |= ER1_AOE;
        words = total - start;
        error = true;
    }
    if (d->function == F_WRITE && d->read_only) {
        drive_error(d, ER1_WLE);
        set_done(rh, true);
        return;
    }
    uint32_t sectors = (words + WORDS - 1) / WORDS;
    uint8_t *buffer = calloc(sectors ? sectors : 1, WORDS * 2);
    if (!buffer) {
        drive_error(d, ER1_UNS);
        set_done(rh, true);
        return;
    }
    long offset = (long)start * 2;
    bool writing = d->function == F_WRITE;
    if (!writing && fseek(d->file, offset, SEEK_SET) == 0)
        (void)fread(buffer, 1, (size_t)sectors * WORDS * 2, d->file);
    uint32_t address = rh->extended ? (uint32_t)rh->bae << 16 | rh->ba :
        (uint32_t)rh->ba | ((uint32_t)(rh->cs1 & CS1_BAE) << 8);
    uint32_t done = 0;
    for (; done < words; ++done) {
        uint16_t word = (uint16_t)(buffer[done * 2] | buffer[done * 2 + 1] << 8);
        if (writing) {
            if (!dma(rh, cpu, &address, false, &word))
                break;
            buffer[done * 2] = (uint8_t)word;
            buffer[done * 2 + 1] = (uint8_t)(word >> 8);
        } else if (d->function == F_WCHK) {
            uint16_t memory = 0;
            uint32_t at = address;
            if (!dma(rh, cpu, &address, false, &memory))
                break;
            if (memory != word) {
                rh->cs2 |= CS2_WCE;
                address = at;
                break;
            }
        } else if (!dma(rh, cpu, &address, true, &word)) {
            break;
        }
        rh->db = word;
    }
    if (writing && done && fseek(d->file, offset, SEEK_SET) == 0) {
        size_t bytes = (size_t)((done + WORDS - 1) / WORDS) * WORDS * 2;
        if (fwrite(buffer, 1, bytes, d->file) != bytes || fflush(d->file))
            d->er1 |= ER1_UNS;
    }
    free(buffer);

    rh->wc = (uint16_t)(rh->wc + done);
    rh->ba = (uint16_t)address;
    rh->cs1 = (uint16_t)((rh->cs1 & ~CS1_BAE) | ((address >> 8) & CS1_BAE));
    if (rh->extended)
        rh->bae = (uint16_t)(address >> 16 & 077);
    uint32_t next = (start + done + WORDS - 1) / WORDS;
    if (next >= rh11_blocks(d->type)) {
        d->ds |= DS_LST;
        next = rh11_blocks(d->type) - 1;
    }
    d->da = (uint16_t)(((next / g->sectors % g->surfaces) << 8) | (next % g->sectors));
    d->dc = d->cylinder = (uint16_t)(next / g->sectors / g->surfaces);
    error |= d->er1 != 0 || (rh->cs2 & CS2_ERR);
    update_status(d, d->er1 ? DS_ATA : 0);
    set_done(rh, error);
}

void rh11_tick(Rh11 *rh, Pdp11 *cpu, uint64_t now)
{
    for (unsigned i = 0; i < RH11_DRIVES; ++i) {
        RhDrive *d = &rh->drive[i];
        if (!d->done_at || now < d->done_at)
            continue;
        d->done_at = 0;
        d->ds = (uint16_t)((d->ds & ~DS_PIP) | DS_DRY);
        if (!mounted(d)) {
            drive_error(d, ER1_UNS);
            if (d->function >= F_TRANSFER)
                set_done(rh, true);
            continue;
        }
        switch (d->function) {
        case F_OFFSET: update_status(d, DS_OFM | DS_ATA); break;
        case F_RETURN: d->ds &= (uint16_t)~DS_OFM; update_status(d, DS_ATA); break;
        case F_SEEK: case F_RECAL: case F_SEARCH: update_status(d, DS_ATA); break;
        case F_WRITEH: update_status(d, 0); set_done(rh, false); break;
        default: transfer(rh, d, cpu); break;
        }
    }
}

static uint16_t drive_read(const Rh11 *rh, const RhDrive *d, unsigned reg, uint64_t now)
{
    const Geometry *g = &geometry[d->type];
    switch (reg) {
    case 3: return d->da;
    case 5: return d->ds;
    case 6: return d->er1;
    case 7: {
        uint16_t as = 0;
        for (unsigned i = 0; i < RH11_DRIVES; ++i)
            if (rh->drive[i].ds & DS_ATA)
                as |= (uint16_t)(1u << i);
        return as;
    }
    case 8: return (uint16_t)((now / 4 % g->sectors) << 6);
    case 10: return d->mr;
    case 11: return g->type;
    case 12: return (uint16_t)(020 | ((d - rh->drive) + 1));
    case 13: return d->of;
    case 14: return d->dc;
    case 15: return g->rm ? (uint16_t)~d->hr : d->cylinder;
    case 16: return g->rm ? d->mr2 : 0;
    default: return 0;
    }
}

static void drive_write(Rh11 *rh, RhDrive *d, unsigned reg, uint16_t value)
{
    if (reg == 7) {
        for (unsigned i = 0; i < RH11_DRIVES; ++i)
            if (value & (1u << i))
                rh->drive[i].ds &= (uint16_t)~DS_ATA;
        return;
    }
    if (d->done_at) {
        drive_error(d, ER1_RMR);
        return;
    }
    d->hr = value;
    switch (reg) {
    case 3: d->da = value & DA_WRITABLE; break;
    case 10: d->mr = value; break;
    case 13: d->of = value & OF_WRITABLE; break;
    case 14: d->dc = value & DC_WRITABLE; break;
    case 16: if (geometry[d->type].rm) d->mr2 = value; break;
    default: break;
    }
    update_status(d, 0);
}

static uint16_t merge(uint16_t old, uint16_t value, bool byte, bool high)
{
    if (!byte)
        return value;
    return high ? (uint16_t)((old & 0377) | (value << 8)) : (uint16_t)((old & 0177400) | (value & 0377));
}

bool rh11_io(Rh11 *rh, uint16_t address, bool write, bool byte, uint16_t *value, uint64_t now)
{
    if (address < RH11_CSR || address > RH11_CSR + (rh->extended ? 053 : 047))
        return false;
    unsigned reg = (address - RH11_CSR) >> 1;
    bool high = byte && (address & 1);
    bool present = unit(rh) < RH11_DRIVES;
    RhDrive *d = present ? &rh->drive[unit(rh)] : NULL;
    bool internal = reg == 0 || reg == 1 || reg == 2 || reg == 4 || reg == 9 || reg >= 024;

    if (!write) {
        uint16_t v;
        switch (reg) {
        case 0:
            v = (uint16_t)(cs1_status(rh) | (d ? (d->function << 1) | CS1_DVA : 0));
            if (!d)
                rh->cs2 |= CS2_NED;
            break;
        case 1: v = rh->wc; break;
        case 2: v = rh->ba; break;
        case 4: v = rh->cs2 | CS2_IR | CS2_OR; break;
        case 9: v = rh->db; break;
        case 024: v = rh->bae; break;
        case 025: v = rh->cs3; break;
        default:
            if (!d) {
                rh->cs2 |= CS2_NED;
                v = 0;
            } else {
                v = drive_read(rh, d, reg, now);
            }
            break;
        }
        *value = byte ? (uint16_t)((high ? v >> 8 : v) & 0377) : v;
        return true;
    }

    uint16_t data = *value;
    if (!internal) {
        if (high)
            data = (uint16_t)(data << 8);
        if (!d)
            rh->cs2 |= CS2_NED;
        else
            drive_write(rh, d, reg, data);
        return true;
    }
    switch (reg) {
    case 0:
        if (high)
            data = (uint16_t)(data << 8);
        if (data & CS1_TRE) {
            rh->cs1 &= (uint16_t)~CS1_TRE;
            rh->cs2 &= (uint16_t)~CS2_ERR;
        }
        if ((!byte || high) && (rh->cs1 & CS1_RDY)) {
            rh->cs1 = (uint16_t)((rh->cs1 & ~CS1_BAE) | (data & CS1_BAE));
            rh->bae = (uint16_t)((rh->bae & ~3u) | (data & CS1_BAE) >> 8);
        }
        if (!byte || !high) {
            if ((data & CS1_RDY) && (data & CS1_IE))
                rh->iff = true;
            rh->cs1 = (uint16_t)((rh->cs1 & ~CS1_IE) | (data & CS1_IE));
            unsigned function = (data & CS1_FNC) >> 1;
            bool transfer_start = (data & CS1_GO) && function >= F_TRANSFER;
            if (transfer_start && !(rh->cs1 & CS1_RDY)) {
                rh->cs2 |= CS2_PGE;
            } else if (!d) {
                rh->cs2 |= CS2_NED;
            } else if (d->done_at) {
                drive_error(d, ER1_RMR);
            } else {
                d->function = (uint16_t)function;
                if ((data & CS1_GO) && go(rh, d, now) && transfer_start) {
                    rh->cs1 &= (uint16_t)~(CS1_TRE | CS1_MCPE | CS1_RDY);
                    rh->cs2 &= (uint16_t)~CS2_ERR;
                }
            }
        }
        break;
    case 1: rh->wc = merge(rh->wc, data, byte, high); break;
    case 2: rh->ba = (uint16_t)(merge(rh->ba, data, byte, high) & ~1u); break;
    case 4:
        if (high)
            data = (uint16_t)(data << 8);
        if (data & CS2_CLR) {
            rh11_reset(rh);
        } else {
            if (byte)
                data = (uint16_t)((rh->cs2 & (high ? 0377 : 0177400)) | data);
            rh->cs2 = (uint16_t)((rh->cs2 & ~CS2_RW) | (data & CS2_RW));
        }
        break;
    case 9: rh->db = merge(rh->db, data, byte, high); break;
    case 024:
        if (rh->cs1 & CS1_RDY) {
            rh->bae = (uint16_t)(merge(rh->bae, data, byte, high) & 077);
            rh->cs1 = (uint16_t)((rh->cs1 & ~CS1_BAE) | (rh->bae & 3) << 8);
        }
        break;
    case 025: rh->cs3 = (uint16_t)(merge(rh->cs3, data, byte, high) & 0140); break;
    }
    return true;
}
