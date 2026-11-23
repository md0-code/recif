# Notes

The CORAL models, disk packs, 2.11BSD, the terminal, serial connections, tape images and compatibility. The keys are in the [README](../README.md).

## CORAL models

The computer panel shows the hardware from historical sources and the DEC
reference configuration used to run software. The memory column below gives
the default exposed RAM of that reference configuration.

| CORAL model | Documented hardware | Execution reference and default RAM |
| --- | --- | --- |
| 4001 | 64 KB reported and AMD 2901 bit slices. Sources identify the early 16-bit model and RT-11 compatibility. [1979 launch report](https://www.bibliotecadeva.ro/periodice/scanteia/1979/06/scanteia_1979_06_11454.pdf) | PDP-11/04 basic integer subset, 56 KiB |
| 4011 | 256 KB reported and a contemporary PDP-11/34 comparison. [1979 launch report](https://www.bibliotecadeva.ro/periodice/scanteia/1979/06/scanteia_1979_06_11454.pdf), [1984 computer directory](https://retropages.hu/Download/A%20szocialista%20orszagok%20mikro_%20es%20miniszam%C3%ADtogepei.pdf) | PDP-11/34 integer and unified 18-bit MMU subset, 248 KiB |
| 4021 | 2 MB in the dispatch diagram and 4096 KB in the contemporary survey. The survey reports a floating-point processor. The dispatch diagram lists 40 MB HDD, 0.5 MB FDD, magnetic tape, MPX 40 A/B, ALC 41 and territorial I/O DEN 40. [Dispatch diagram, appendix D](https://era.ed.ac.uk/items/3fdbc5a2-088b-46af-9e29-e2d5b4d1ac92), [1984 technical survey](https://www.kshkonyvtar.hu/file-show/L2hvbWUvc2lsaW5mby9hcHBsaWNhdGlvbi93b3JrZGlyL2NrZWRpdG9yQ3VzdG9tRmlsZXMvRsOpbnllcyBFbGVrIGRpZ2l0w6FsaXMgc3RhdGlzenRpa2FpIGvDtm55dnTDoXIvU3rDoW3DrXTDoXN0ZWNobmlrYSAoZm9secOzaXJhdCkvMTk4NC9JSUlfQV8xNDYwX1N6YW1pdGFzdGVjaG5pa2FfODRfMDNfdmoucGRm) | PDP-11/44 extended-memory experiment, 2048 KiB |
| 4030 | A modular asynchronous bus and QB-40 bridge to QBUS. Period sources report up to 4 MB, disk, tape, printer and card reader options, and compatibility with existing 11/44 programs. [1983 CORAL 4030 report](https://www.bibliotecadeva.ro/periodice/revistaeco/1983/08/revista_economica_1983_08_31.pdf), [1984 technical survey](https://www.kshkonyvtar.hu/file-show/L2hvbWUvc2lsaW5mby9hcHBsaWNhdGlvbi93b3JrZGlyL2NrZWRpdG9yQ3VzdG9tRmlsZXMvRsOpbnllcyBFbGVrIGRpZ2l0w6FsaXMgc3RhdGlzenRpa2FpIGvDtm55dnTDoXIvU3rDoW3DrXTDoXN0ZWNobmlrYSAoZm9secOzaXJhdCkvMTk4NC9JSUlfQV8xNDYwX1N6YW1pdGFzdGVjaG5pa2FfODRfMDNfdmoucGRm) | PDP-11/44 integer and split-I/D MMU subset, 4088 KiB |

The reference addressing limit includes an 8 KiB I/O page. The computer
panel shows the usable RAM alongside the selected amount. The DEC reference
devices are a DL11 serial interface, a KW11-L clock, a read-only TM11 with
one tape drive and a disk controller: an RH11 with two RM03 or RP06 drives,
or an RP11-C with two RP03 drives. The clock runs at 50 Hz unless you select
60 Hz or switch it off; the RSX system generation recovered from a CORAL tape
is built for 50 Hz mains. The TM11 starts enabled for the models with a
documented tape. The 4021 starts with the RP11-C, whose 40 MB RP03 matches
the disk of the documented 4021 installation, and the 4030 with the RH11 and
RM03 drives. You can change these in the panel, or with `--tape`, `--disk`
and `--disks` on the command line.

The FP11 floating-point processor follows the DEC FP11 description: F and D
formats, the FPS modes and the exception trap through 244. It starts enabled
for the 4021, whose floating-point processor is reported, and can be enabled
for the 4011 and 4030 with `--fpu` or in the panel. The 4001 reference has no
FP11 option.
[DEC processor handbook](https://bitsavers.trailing-edge.com/pdf/dec/pdp11/handbooks/EB-19402-20_PDP-11_Processor_Handbook_1981.pdf)

Native CORAL CPU microcode, boot ROMs, controller register maps and
the 4021 FPU interface remain missing from the available hardware evidence.
The FP11, RH11 and RP11-C are DEC substitutes for these.

One CORAL 4030 detail comes from software. The RSX-11M V4.1 system on the
RSX41FP tape moves MMR3 to 165410 ("correction for CORAL 4030") and probes
for a UNIBUS map and for 22-bit disk controllers. The 4030 reference therefore
answers MMR3 at both 172516 and 165410, has no UNIBUS map, as on a QBUS, and
gives the RH11 the RH70 bus address extension register at 176750.
The storage and interface boards listed above are hardware records. They
require their native programming specifications for emulation. The
4015, suffix variants and VAX-family 8730 are separate model identities.

Use `recif.exe models` to inspect the profiles and `recif.exe --help`
for the command syntax.

## Disk packs

Each drive takes a flat image of 512-byte sectors in cylinder, track and
sector order.

| Drive | Controller | Geometry | Image size |
| --- | --- | --- | --- |
| RP03 | RP11-C at 176710 | 406 cylinders, 20 tracks, 10 sectors | 41,574,400 bytes |
| RM03 | RH11 at 176700 | 823 cylinders, 5 tracks, 32 sectors | 67,420,160 bytes |
| RP06 | RH11 at 176700 | 815 cylinders, 19 tracks, 22 sectors | 174,423,040 bytes |

Both controllers interrupt through vector 254, so a machine has one of
them. Choose a new file name in **Browse** to create an empty pack.
Space that was never written reads as zeros. A file that cannot be opened
for writing mounts write-locked.

The **Disk unit** boot sources read the first 512 words of the pack to
address zero and start them with the unit number in R0 and the controller's
CSR in R1 (176700 for the RH11, 176714 for the RP11-C), as the DEC
bootstraps do.

`recif.exe bru-restore` rebuilds the source disk of a BRU backup tape as a
pack image. Every block goes back to its original place, so a saved RSX
system finds its installed tasks again; the index file, storage bitmap and
directories are rebuilt. Later backup sets of the same volume on the tape are
added. The RSX41FP tape restores to a bootable RSX-11M V4.1 system for an
RP06 on the CORAL 4030:

```powershell
.\recif.exe bru-restore .\RSX41FP.tap .\rsx41fp.dsk --disk rp06
.\recif.exe run --model 4030 --disks rp06 --disk .\rsx41fp.dsk --boot-disk 0 --serial tcp-listen:2323
```

Answer the date prompt with Enter or a time and date such as
`12:00 22-NOV-86`. The tape holds only the first directories of the disk,
so the startup file reports some missing files. Packs restored from the
other tapes mount as data disks on unit 1, for example `MOU DB1:CTCI102`
and `RUN DB1:[377,15]ANIMAL` for the CORAL4 games tape.

## 2.11BSD

`disks\unix211-rp06.dsk` holds 2.11BSD (April 2000), the Berkeley UNIX for
the PDP-11, installed from its original distribution tape: root, swap and a
`/usr` with the compilers, games and complete sources. It runs on the CORAL
4021 profile, whose 11/44 reference has the UNIBUS map that 2.11BSD expects.

A freshly unpacked release starts set up for it: CORAL 4021, **RH11 with
RP06**, the pack on unit 0 as the boot source, and a DAF-2020 in VT100 mode
that connects to `127.0.0.1:2323` with switches S4 and S5 ON and S7 OFF,
because the BSD console sends 7-bit characters with even parity. Relative
image paths in the panel name files beside `recif.exe`. Settings saved
earlier take precedence; deleting `recif.yaml` and `daf2020.yaml` returns to
this setup.

1. Start `recif.exe` and `daf2020.exe`, then press **Start reference**.
2. Press Enter at the `:` prompt. At the single-user `#` prompt press Ctrl+D,
   then log in as `root` (no password).
3. End with `shutdown -h now` before stopping Recif.

```powershell
.\recif.exe run --model 4021 --disks rp06 --disk .\disks\unix211-rp06.dsk --boot-disk 0 --serial tcp-listen:2323
```

**Full host speed** in the panel, or `--full-speed`, runs the processor as
fast as the computer allows while the guest is busy and at the 11/44's pace
while it waits; the guest clock then runs ahead. In the source tree
`python tools\unix211.py` rebuilds the pack from the tape.
[License notices](THIRD_PARTY.md#211bsd)

## DAF-2020 terminal

The terminal starts in its Tektronix 4010 personality. As the manual
describes, it shows letters as capitals until it receives SO, and its keyboard
sends capitals only. `ESC X 1`, or **CTRL+PF1** followed by **1**, enters
VT100 mode, and `ESC 1` returns to the Tektronix mode. In the Tektronix
mode the arrow keys move the 288-line window over the 390-line work space;
**SCRL+SHIFT** fixes the window and **SCRL+CTRL** makes it follow the
cursor again. In VT100 mode, **SCRL+CTRL** opens SET-UP A, and its key **5**
switches to SET-UP B. The settings you change there last until the terminal
is reset or switched off.

| DAF-2020 key | PC key |
| --- | --- |
| PF1–PF4 | F1–F4 |
| PAGE | F5 |
| RESET | F6 |
| LF | F7 |
| BREAK | F8 or Pause |
| SCRL | F9 or Scroll Lock |
| Reset, clearing the screen (as ESC c) | Alt+F12 |
| Power off and on | Ctrl+F12 or **Restart** under F12 |
| Quit | Shift+F12 |

**F12** opens the settings: the interface language, the Tektronix or VT100
mode, the connection and its address, the 14 keyboard switches, the CRT
effect, a normal or inverted screen and a restart. The arrow keys up and down
choose a row, left and right choose a switch, and **Enter** opens the values
of a row, toggles a switch or edits the address. **Esc** or **F12** leaves the
settings and applies them; the language, the CRT effect and the screen apply
at once. The screen shows a green phosphor tube, or dark characters on a lit
tube when inverted.

The switches are numbered as in the manual. ON is the upper position.

| Switch | Setting |
| --- | --- |
| S1–S3 | Speed of 150 × 2ⁿ baud: S1 ON adds 4 to n, S2 ON adds 2 and S3 ON adds 1 |
| S4 | ON even parity, OFF odd parity |
| S5 | ON parity, OFF none |
| S6 | ON two stop bits |
| S7 | ON 8-bit characters, OFF 7-bit |
| S8 | ON on line, OFF local |
| S9, S10 | GIN replies end with CR when S10 is ON, and with CR EOT when S9 is also ON |
| S11 | ON local echo |
| S12 | ON LF also returns the carriage |
| S13 | ON BREAK when the page is full |
| S14 | ON key click |

## Serial connections

The main unit owns the DL11 endpoint and framing in its control panel.
The DAF-2020 chooses its connection under **F12**: a TCP client or server,
a named pipe, a serial port of the computer or a loopback. Its framing
follows the switches and SET-UP B. When the main unit uses a TCP, pipe or host serial
port, it waits for the first connection before starting the guest.
The **Restart** button restarts the main unit. **Ctrl+F12** switches the
DAF-2020 off and on. In the terminal, **Alt+Enter** toggles fullscreen and
**Print Screen** saves a PNG.

| Endpoint | Use |
| --- | --- |
| `tcp-listen:2323` | Listen on localhost |
| `tcp-listen:HOST:2323` | Listen on the specified IPv4 interface |
| `tcp:127.0.0.1:2323` | Connect to a listener |
| `pipe:NAME` or `pipe:\\.\pipe\NAME` | Open the same local pipe at both ends: `\\.\pipe\NAME` on Windows, the socket `/tmp/NAME` on Linux. The first process serves it |
| `COM3`, `/dev/ttyUSB0` or `serial:COM3` | Open a host serial port: on Windows with its host settings, on Linux raw |
| `loopback` | Feed transmitted data back to the receiver |
| `file:PATH` | Append transmitted bytes to a capture file |
| `none` | Leave the terminal disconnected or send CLI output to the shell |

For example, set ZetRo's `ports.serial` to `tcp-listen:2323` and start
`daf2020.exe --serial tcp:127.0.0.1:2323`. A ZetRo model with a serial port,
such as the aMIC, can then use the terminal from guest software.

For MezinXT with the PC/XT serial adapter, use the following configuration
and start `daf2020.exe --serial pipe:junior-daf`. On the Junior's own SIO
adapter, use `ports.sio_a` for channel A instead.

```yaml
ports:
  adapter: false
  pc_serial: one
  com1: 'pipe:\\.\pipe\junior-daf'
```

Recif and DAF-2020 default to 9600 baud, eight data bits, no parity and one stop
bit. For another format, pass `--serial-format 1200,7E1` to the relevant
Recif program; for the terminal it sets switches S1–S7. Rates from 150
through 19200 baud double at each step. The format accepts 7 or 8 data bits,
N/E/O parity and 1 or 2 stop bits. TCP and named pipes carry raw data bytes.
Each emulator handles its own UART framing. Break and modem-control signals
stay local to the emulator.

Use `daf2020.exe --demo` for Tek graphics or `daf2020.exe --replay STREAM`
to display a serial capture offline. Add `--plain` to disable the CRT effect.

## Tape images

Recif mounts conventional SIMH TAP, TDUP recovery pairs (`.DAT` and `.LEN`),
and the recognized flat DEC BRU captures. Keep both files of a TDUP pair in
the same folder. For a differently named length file, give it as the final
argument to `tape-info`, `tape-convert`, `bru-list` or `bru-extract`.

For a first test, extract `benzi_20230817.tar.gz` and mount
`CONVMIC/TAPEFULL.IMG`. Press **R** in the monitor to see `VOL1BACKUP`. The
original program in the second tape record prints its nonbootable-volume
message when you start from **Labelled tape bootstrap**. `CORAL4/TAPEFULL.IMG` contains the
CNC, JOCURI and PASCAL backup sets. `benzi_20251107.tar.gz` also contains
a TDUP copy of CONVMIC in `25/VARIANT1`. [Z80-Romania tape collections](https://www.z80-romania.ro/Benzi_pentru_FELIX_CORAL_INDEPENDENT_etc_-170)

```powershell
.\recif.exe tape-info .\CONVMIC\TAPEFULL.IMG
.\recif.exe tape-convert .\CONVMIC\TAPEFULL.IMG .\convmic.tap
.\recif.exe bru-list .\CORAL4\TAPEFULL.IMG
.\recif.exe bru-extract .\CONVMIC\TAPEFULL.IMG .\convmic-files
.\recif.exe run --tape .\convmic.tap --boot-tape
```

BRU extraction writes file bytes through the Files-11 end-of-file position,
including any original record framing. The filename includes the backup set,
UIC, file number and version. The catalog marks files with missing or
conflicting blocks as partial. When a flat capture loses record alignment,
Recif mounts the readable prefix and reports the number of bytes outside
it. A TAP exported from that mount contains only the readable prefix.
TDUP pairs retain all supplied records and filemarks. Binary payloads keep
their original encoding, including EBCDIC.

The March 2023 archive carries a split RSX41 backup. Join its `TEST0.TXT`
and `banda0/DEST0.TXT` in that order as binary data, then use `bru-list` or
`bru-extract` on the joined image. Its readable prefix includes two
versions of `RSX11M.SYS`.

**Labelled tape bootstrap** uses a reference loader that skips the volume label and
executes the second record. For an unlabeled tape, use
`recif run --tape IMAGE --boot-first` to execute the first record.
The CLI can also load a flat PDP-11 binary with `--raw IMAGE`, `--load OCTAL`
and `--entry OCTAL`. The default load and entry address is `001000`.

## Language

Both programs are in English or Romanian. They follow the system's
language until you choose one in the **Language** list of the panel or the
**F12** settings of the terminal.

## Compatibility

- The **DAF-2020** follows its 1987 technical manual: Tektronix vectors,
  programmable characters, graphics input, VT100/VT52 text, the keyboard
  switches and SET-UP. It uses a substitute character font. The manual gives
  SI and SO opposite meanings in two places. Recif follows section 2.6.1, in
  which SI selects capitals and is the power-on state. The printer and
  joystick ports are not emulated, so hard copy and transparent printing do
  nothing. [FEPER DAF-2020 technical manual](https://www.z80-romania.ro/1987_Carte_tehnica_DAF_2020-171)
- The archived April 1989 RSX recovery does not boot: its bootstrap is
  nonbootable and its tape data is damaged or missing. The RSX41FP tape
  restores to a system that boots (see [disk packs](#disk-packs)).
- The Windows x64 programs have been run on a real machine. The Windows
  ARM64 programs are cross-built and have not been run. The Linux programs
  have been run headless only, the ARM64 ones under qemu user-mode emulation:
  never in a window on a real display, and their sound output not at all.
