# Changelog

## 0.8.2 (2026-11-23)
- The main unit and DAF-2020 windows carry the Recif icon on Linux too.

## 0.8.1 (2026-11-23)
- Recif logo for the Windows executables and window icons of the main unit and DAF-2020 terminal.

## 0.8.0 (2026-11-23)
- Linux x64 and ARM64 release archives with both programs and the 2.11BSD pack.
- On Linux the main unit panel opens in an SDL window, worked with the mouse or the keyboard, with a file picker for tapes and disks.
- The DAF-2020 terminal runs on Linux, with host serial ports and pipes as `/tmp` sockets.
- DAF-2020: Print Screen saves the screen as the window shows it, in green and with the CRT effect; `--help` and `--version`.

## 0.7.2 (2026-11-23)
- The 2.11BSD setup applies on every start without saved settings, not only from the release's settings files.

## 0.7.1 (2026-11-23)
- A new release starts set up for the 2.11BSD pack: Recif boots it and the DAF-2020 connects at 7E1 as a VT100.
- The DAF-2020 keeps the personality chosen in F12 across starts and resets.
- Relative tape and disk paths in the panel name files beside `recif.exe`.

## 0.7.0 (2026-11-22)
- A ready-to-boot 2.11BSD pack for the CORAL 4021 ships in `disks/`, installed from the original tape by `tools/unix211.py`.
- Full host speed: a panel option and `--full-speed` run the processor flat out while the guest computes.
- The processor runs at about 600,000 instructions per second instead of 100,000, close to a PDP-11/44.
- The tape bootstrap leaves R0 = unit and R1 = the TM11 address, as the DEC ROMs do, so 2.11BSD boots from tape.
- `bru-restore` handles volumes with more than 27 directories.

## 0.6.2 (2026-11-22)
- DAF-2020 Alt+F12 resets the terminal as ESC c does and clears the screen.

## 0.6.1 (2026-11-22)
- DAF-2020 inverted screen under F12: dark characters on a lit tube.

## 0.6.0 (2026-11-22)
- `bru-restore` rebuilds the disk of a BRU backup tape as a pack image; the RSX41FP tape boots RSX-11M V4.1 on the CORAL 4030.
- CORAL 4030: MMR3 also at 165410, 22-bit disk transfers without a UNIBUS map, RH11 address extension register.
- DAF-2020 keys as in ZetRo: **Restart** under F12 and Ctrl+F12 switch the terminal off and on, Alt+F12 resets it, Shift+F12 quits.

## 0.5.1 (2026-11-22)
- DAF-2020 characters are drawn in a 5 × 7 dot matrix with descenders; no letter loses its right side.

## 0.5.0 (2026-11-22)
- RP11-C disk controller with two 40 MB RP03 drives, the default for the 4021.
- Romanian interface for both programs, with the language chosen in the panel or under F12, and Romanian documentation.
- DAF-2020 settings: Enter selects values from lists, the connection is chosen by kind with the host's serial ports listed, and the terminal mode can be set.
- DAF-2020 shows a green phosphor tube, with a border that keeps the cursor clear of the rounded corners.
- The switch numbers sit under their switches.

## 0.4.0 (2026-11-22)
- FP11 floating-point processor, on for the 4021 and optional for the 4011 and 4030.
- RH11 disk controller with two RM03 or RP06 drives, flat pack images and a disk bootstrap.
- DAF-2020 keyboard switches S1-S14 under F12, and VT100 SET-UP A and B.
- DAF-2020 bell, key click, BREAK, PAGE, RESET, SCRL, LF and CTRL+PF1 special commands.
- Recif and DAF-2020 remember their settings.
- DAF-2020 Tektronix mode as in the manual: capitals until SO, alpha cursor and margins, control characters in graphic mode, window tracking and GIN exit.
- DAF-2020 VT100 mode: ESC 1 returns to Tektronix, full special graphics and UK set, smooth-scroll pacing, ESC Z, ESC # 8 and reset sequences.
- The KW11-L clock defaults to 50 Hz.
- PDP-11 fixes: 11/44 traps for JMP/JSR to a register and HALT in user mode, kernel stack limit, trace timing, MTPI order, MFPI between user modes, CSM, PIRQ priority, protected T bit, 11/04 register addresses and the cache flush.
- The TM11 reports a bad tape at the end of the data, and the monitor's help no longer crashes.

## 0.3.0 (2026-11-22)
- Two executables with independent settings. Recif is the computer control panel and command-line tool. DAF-2020 is the serial terminal.
- Select CORAL 4001, 4011, 4021 or 4030 with each model's published hardware inventory, RAM configuration and designated DEC compatibility core.
- Set the reference clock, serial port and tape controller from the main unit. Mount, eject and bootstrap tapes from its control panel.
- Set the terminal's serial connection, local echo and CRT presentation with F12.

## 0.2.0 (2026-11-22)
- Standalone DAF-2020 terminal and headless Recif machine with their own serial endpoints.
- Timed RS-232 console with selectable baud, word length, parity and stop bits.
- Raw TCP and named-pipe links compatible with ZetRo and MezinXT, plus host COM, loopback and capture-file endpoints.
- Green phosphor and a bright cursor in Tek and VT modes.

## 0.1.0 (2026-11-22)
- PDP-11/44 reference CPU, MMU, console, clock and read-only TM11 tape drive.
- DAF-2020 Tek graphics and VT100/VT52 terminal with CRT display and serial replay.
- SIMH and TDUP tape mounting and conversion, flat BRU recovery, and Files-11 catalog and extraction.
- Windows x64 and ARM64 packages with desktop and command-line applications.
