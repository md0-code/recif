#ifndef RECIF_PDP11_H
#define RECIF_PDP11_H

#include <stdbool.h>
#include <stdint.h>

enum { PDP_C = 1, PDP_V = 2, PDP_Z = 4, PDP_N = 8, PDP_T = 16 };
enum { PDP_IO_PAGE = 017760000u, PDP_IO_VIRTUAL = 0160000u };

typedef enum { PDP_04, PDP_34, PDP_44 } PdpModel;
const char *pdp_reference_name(PdpModel model);
unsigned pdp_ram_limit(PdpModel model);
bool pdp_fpu_option(PdpModel model);

typedef struct {
    void *context;
    bool (*access)(void *, uint32_t address, bool write, bool byte, uint16_t *value);
    unsigned (*interrupt)(void *, unsigned ipl, bool acknowledge);
    void (*reset)(void *);
} PdpBus;

/* FP11 floating-point processor: six 64-bit accumulators, each held as the
 * memory image (sign 63, exponent 62:55, fraction 54:0). */
typedef struct {
    uint64_t ac[6];
    uint16_t fps, fec, fea;
} Fp11;

typedef struct {
    PdpBus bus;
    PdpModel model;
    bool fpu;
    uint16_t r[8], sp[4], psw;
    uint16_t par[4][2][8], pdr[4][2][8];
    uint16_t mmr[4], pirq, cpuerr, ccsr, cmr;
    uint16_t mmr3_alias;        /* second I/O address of MMR3, 0 if none */
    bool direct_dma;            /* 22-bit device addresses, no UNIBUS map */
    uint32_t unibus[31];
    Fp11 fp;
    uint16_t instruction_pc, opcode, last_vector, fault;
    uint32_t last_address;
    uint64_t instructions;
    unsigned restart_bytes;
    bool waiting, halted, double_fault, yellow;
} Pdp11;

void pdp_start(Pdp11 *cpu, PdpModel model, bool fpu, PdpBus bus, uint16_t pc);
void pdp_step(Pdp11 *cpu);
uint32_t pdp_dma_address(const Pdp11 *cpu, uint32_t address);

#endif
