#include "guest.h"

#include <stddef.h>
#include <string.h>

/* Each routine ends below the next one's start. */
#define PUTC 01000
#define PUTS 01020
#define GETC 01040
#define MONITOR 01100       /* 35 words, to 01206 */
#define READ_RECORD 01300   /* 57 words, to 01462 */
#define SHOW_FILEMARK 01500
#define SHOW_END 01520
#define REWIND 01540        /* 18 words, to 01604 */
#define BOOT 01700          /* tape bootstrap, 31 words */
#define BANNER 02000
#define PROMPT 02500
#define HELP 02510
#define RECORD 03000
#define FILEMARK 03020
#define NO_DATA 03040
#define REWOUND 03100
#define BUFFER 04000
#define DISK_BOOT 010000

#define JSR_PC 004737
#define RTS_PC 000207
#define JMP_ABS 000137
#define MOV_IMM_R0 012700
#define MOV_IMM_R1 012701
#define MOV_IMM_R2 012702
#define MOV_IMM_SP 012706
#define MOV_IMM_ABS 012737
#define CLR_ABS 005037
#define BIT_IMM_ABS 032737
#define CMP_IMM_R0 022700
#define TSTB_ABS 0105737
#define CMPB_IMM_R0 0122700

static void deposit(uint8_t *ram, uint16_t address, const uint16_t *words, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        ram[address++] = (uint8_t)words[i];
        ram[address++] = (uint8_t)(words[i] >> 8);
    }
}

#define DEPOSIT(ram, address, words) deposit(ram, address, words, sizeof(words) / sizeof((words)[0]))

uint16_t guest_monitor(uint8_t *ram, bool tape_controller)
{
    static const uint16_t putc[] = {
        TSTB_ABS, 0177564, 0100375,           /* BPL PUTC */
        0110037, 0177566, RTS_PC              /* MOVB R0,@#XBUF */
    };
    static const uint16_t puts[] = {
        0112100, 0001403, JSR_PC, PUTC,       /* MOVB (R1)+,R0; BEQ done */
        0000773, RTS_PC
    };
    static const uint16_t getc[] = {
        TSTB_ABS, 0177560, 0100375,           /* BPL GETC */
        0113700, 0177562, RTS_PC              /* MOVB @#RBUF,R0 */
    };
    const uint16_t monitor[] = {
        MOV_IMM_SP, 01000,
        MOV_IMM_R1, BANNER, JSR_PC, PUTS,
        /* Prompt. Each command returns here. */
        MOV_IMM_R1, PROMPT, JSR_PC, PUTS,
        JSR_PC, GETC, 0042700, 0040,         /* BIC #40,R0 */
        CMP_IMM_R0, 'R', 0001002, JMP_ABS, tape_controller ? READ_RECORD : SHOW_END,
        CMP_IMM_R0, 'W', 0001002, JMP_ABS, tape_controller ? REWIND : SHOW_END,
        CMP_IMM_R0, 015, 0001002, JMP_ABS, MONITOR + 014,
        MOV_IMM_R1, HELP, JSR_PC, PUTS, JMP_ABS, MONITOR + 014
    };
    static const uint16_t read_record[] = {
        MOV_IMM_ABS, 0177660, 0172524,       /* -80 to byte count */
        MOV_IMM_ABS, BUFFER, 0172526,
        MOV_IMM_ABS, 060003, 0172522,
        TSTB_ABS, 0172522, 0100375,
        BIT_IMM_ABS, 040000, 0172520, 0001402, JMP_ABS, SHOW_FILEMARK,
        BIT_IMM_ABS, 0100400, 0172520, 0001402, JMP_ABS, SHOW_END,
        MOV_IMM_R1, RECORD, JSR_PC, PUTS,
        013702, 0172524, 062702, 80,         /* R2 = actual bytes read */
        0001002, JMP_ABS, MONITOR + 014,
        020227, 24, 0101402, MOV_IMM_R2, 24, /* cap preview at 24 characters */
        MOV_IMM_R1, BUFFER,
        0112100, CMPB_IMM_R0, 040, 0101003,  /* BHI substitute */
        CMPB_IMM_R0, 0176, 0103002,          /* BHIS printable */
        MOV_IMM_R0, '.',
        JSR_PC, PUTC, 005302, 0001363,       /* DEC R2; BNE loop */
        JMP_ABS, MONITOR + 014
    };
    static const uint16_t filemark[] = {
        MOV_IMM_R1, FILEMARK, JSR_PC, PUTS, JMP_ABS, MONITOR + 014
    };
    static const uint16_t end[] = {
        MOV_IMM_R1, NO_DATA, JSR_PC, PUTS, JMP_ABS, MONITOR + 014
    };
    static const uint16_t rewind[] = {
        MOV_IMM_ABS, 060017, 0172522,
        TSTB_ABS, 0172522, 0100375,
        BIT_IMM_ABS, 0100000, 0172520, 0001402, JMP_ABS, SHOW_END,
        MOV_IMM_R1, REWOUND, JSR_PC, PUTS, JMP_ABS, MONITOR + 014
    };
    DEPOSIT(ram, PUTC, putc);
    DEPOSIT(ram, PUTS, puts);
    DEPOSIT(ram, GETC, getc);
    DEPOSIT(ram, MONITOR, monitor);
    DEPOSIT(ram, READ_RECORD, read_record);
    DEPOSIT(ram, SHOW_FILEMARK, filemark);
    DEPOSIT(ram, SHOW_END, end);
    DEPOSIT(ram, REWIND, rewind);

    static const char banner[] = "\r\nRecif monitor (DEC reference)\r\n"
        "R  Read tape record    W  Rewind tape    H  Help\r\n";
    static const char help[] = "\r\nR reads the next record on TM0. W rewinds TM0.\r\n";
    memcpy(ram + BANNER, banner, sizeof(banner));
    memcpy(ram + PROMPT, "\r\n> ", 5);
    memcpy(ram + HELP, help, sizeof(help));
    memcpy(ram + RECORD, "\r\nTM0: ", 8);
    memcpy(ram + FILEMARK, "\r\nTM0 filemark", 15);
    if (tape_controller)
        memcpy(ram + NO_DATA, "\r\nTM0 end or offline", sizeof("\r\nTM0 end or offline"));
    else
        memcpy(ram + NO_DATA, "\r\nTM11 controller is disabled", sizeof("\r\nTM11 controller is disabled"));
    memcpy(ram + REWOUND, "\r\nTM0 rewound", 14);
    return MONITOR;
}

uint16_t guest_tape_boot(uint8_t *ram, bool first_record)
{
    /* DEC-style loader. For ANSI media space past record one, then DMA
     * record two to zero. For unlabeled media read record one directly. */
    static const uint16_t skip[] = {
        MOV_IMM_ABS, 0177777, 0172524,
        MOV_IMM_ABS, 060011, 0172522,
        TSTB_ABS, 0172522, 0100375
    };
    static const uint16_t read[] = {
        MOV_IMM_SP, 01000,
        MOV_IMM_ABS, 0177000, 0172524,
        CLR_ABS, 0172526,
        MOV_IMM_ABS, 060003, 0172522,
        TSTB_ABS, 0172522, 0100375,
        BIT_IMM_ABS, 0150600, 0172520, 0001401,
        000000,                             /* HALT on a tape error */
        /* As the DEC ROMs leave them: R0 = unit, R1 = MTC. */
        0005000, MOV_IMM_R1, 0172522,
        005007                              /* CLR PC */
    };
    uint16_t pc = BOOT;
    if (!first_record) {
        DEPOSIT(ram, pc, skip);
        pc += sizeof(skip);
    }
    DEPOSIT(ram, pc, read);
    return BOOT;
}

uint16_t guest_disk_boot(uint8_t *ram, unsigned unit)
{
    /* Like the DEC RH11 bootstraps: preset the drive, select 16-bit format,
     * read the first 512 words to zero and start there with R0 = unit and
     * R1 = CSR. */
    const uint16_t boot[] = {
        MOV_IMM_SP, DISK_BOOT,
        MOV_IMM_R0, (uint16_t)unit,
        MOV_IMM_R1, 0176700,
        0012761, 0000040, 0000010,        /* MOV #CLR,CS2(R1) */
        0010061, 0000010,                 /* MOV R0,CS2(R1) */
        0012711, 0000021,                 /* MOV #PRESET+GO,(R1) */
        0012761, 0010000, 0000032,        /* MOV #FMT16,OF(R1) */
        0012761, 0177000, 0000002,        /* MOV #-512.,WC(R1) */
        0005061, 0000004,                 /* CLR BA(R1) */
        0005061, 0000006,                 /* CLR DA(R1) */
        0005061, 0000034,                 /* CLR DC(R1) */
        0012711, 0000071,                 /* MOV #READ+GO,(R1) */
        0105711, 0100376,                 /* TSTB (R1); BPL .-2 */
        0032711, 0040000, 0001401,        /* BIT #TRE,(R1); BEQ start */
        0000000,                          /* HALT on a transfer error */
        0005002, 0005003, 0005004, 0005005,
        0005007                           /* CLR PC */
    };
    DEPOSIT(ram, DISK_BOOT, boot);
    return DISK_BOOT;
}

uint16_t guest_rp_boot(uint8_t *ram, unsigned unit)
{
    /* The RP11-C equivalent: read two sectors from cylinder 0, track 0,
     * sector 0 to zero, then start them with R0 = unit and R1 = RPCS. */
    const uint16_t boot[] = {
        MOV_IMM_SP, DISK_BOOT,
        MOV_IMM_R0, (uint16_t)unit,
        MOV_IMM_R1, 0176714,
        0012761, 0177000, 0000002,        /* MOV #-512.,RPWC */
        0005061, 0000004,                 /* CLR RPBA */
        0005061, 0000006,                 /* CLR RPCA */
        0005061, 0000010,                 /* CLR RPDA */
        0010002, 0000302,                 /* MOV R0,R2; SWAB R2 */
        0052702, 0000005,                 /* BIS #READ+GO,R2 */
        0010211,                          /* MOV R2,(R1) */
        0105711, 0100376,                 /* TSTB (R1); BPL .-2 */
        0005711, 0100001,                 /* TST (R1); BPL start */
        0000000,                          /* HALT on an error */
        0005002, 0005003, 0005004, 0005005,
        0005007
    };
    DEPOSIT(ram, DISK_BOOT, boot);
    return DISK_BOOT;
}
