<p align="center"><img src="assets/logo-github.png" alt="Kernal64" width="420"></p>

# Kernal64

A Commodore 64 emulator written from scratch, cycle-exact, shipped as a libretro core for
RetroArch and Emutastic, for Linux, Windows and macOS.

The whole machine is in: the 6510, both CIA generations, the VIC-II in its PAL and NTSC chip
generations, the 6581 and 8580 SIDs, cartridges (plain, bank-switched and freezers), a 1541
running its own DOS, the datasette, and a CMD HD hard drive running its own DOS.

## What sets it apart

Compared with VICE (3.9), the C64 emulator our tests are checked against:

- `.nib` disk dumps load directly. VICE needs them converted to `.g64` first.
- It passes the same 1,176 programs of the C64 test suite as VICE, plus 16 that VICE fails, among them
  `vsp-tester`.
- 157 of 165 chip-verified SID test programs pass. VICE passes 143.
- It makes CMD HD hard drive images with the drive's DOS installed and the partitions formatted. In VICE you
  install the DOS yourself from the drive's utilities disk.

In more detail:

- Every chip is built from its documents and checked against real hardware: the CPU against a
  transistor-level simulation of the 6502 and all 2,560,000 per-opcode bus traces, the PLA against a real
  chip's read-out for all 65,536 inputs, and the drives and cartridges against bytes captured from real
  hardware.
- The SID's combined waveforms and filter are our own circuit models of the chip.
- The 1541 runs its own DOS ROM on its own 6502 and reads a spinning disk, so fast loaders work as they do on
  the machine.
- The freeze of the type 36 freezer cartridge matches, byte for byte, dumps measured on five real cartridges
  of both boards.
- The CMD HD is emulated as a whole: its processor, boot ROM and SCSI disk, running its own DOS from the disk.
  Frontends can make a ready-to-use drive in seconds, with the drive's own programs installing its DOS and
  formatting its partitions.
- Device 10 is a folder on your computer, on by default, with no size limit. The files in it are ordinary
  files.
- Save states hold everything a replay needs, including the disk surface, the tape and the controllers, so
  rewind, run-ahead and netplay work across a SAVE to disk. The Linux and Windows builds were checked to
  produce the same frames.
- Tapes and disks can load at 2x or 4x with the sound muted, going back to normal speed when loading stops.
- The repository has its own test cartridges, which check bank-switched, flash and freezer cartridge types
  from inside the cartridge, on any emulator.

## Documentation

In the [wiki](https://github.com/codingncaffeine/Kernal64/wiki):
[system files](https://github.com/codingncaffeine/Kernal64/wiki/System-files),
[content](https://github.com/codingncaffeine/Kernal64/wiki/Content),
[drives and media](https://github.com/codingncaffeine/Kernal64/wiki/Drives-and-media),
[hard drives](https://github.com/codingncaffeine/Kernal64/wiki/Hard-drives),
[controls](https://github.com/codingncaffeine/Kernal64/wiki/Controls),
[core options](https://github.com/codingncaffeine/Kernal64/wiki/Core-options) and the
[test cartridges](https://github.com/codingncaffeine/Kernal64/wiki/Test-cartridges).

## Download

The [releases](https://github.com/codingncaffeine/Kernal64/releases) have the core for Linux
(x86-64, glibc and musl), Windows (x86-64) and macOS (arm64 and x86-64). Put it in your
frontend's cores folder (for RetroArch, see
[RetroArch](https://github.com/codingncaffeine/Kernal64/wiki/RetroArch)); Emutastic downloads it by itself.

## System files

The machine's own ROMs are not included: put them in the frontend's system folder. They are
recognized by their contents, not their names. You need BASIC, KERNAL and the character set;
the 1541's two ROMs for disk images; and the CMD HD's boot ROM for hard drives. The full list
is on the [System files](https://github.com/codingncaffeine/Kernal64/wiki/System-files) page.

## Building

Linux, with a C++20 compiler and CMake:

```
tools/build-linux.sh
```

The core lands in `bin/kernal64_libretro.so`; `bin/kernal64` starts it in RetroArch.
`tools/release/build-cores.sh` builds the release cores for Linux and Windows in containers.

## License

GPL-3.0-or-later; see `LICENSE`.
