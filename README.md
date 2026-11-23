<p align="center"><img src="assets/logo-github.png" alt="Kernal64" width="420"></p>

# Kernal64

Another Commodore 64 emulator: written from scratch, cycle-exact, and shipped as a
libretro core for RetroArch and Emutastic.

**Status:** early development. The 6510 CPU, the memory map and the VIC-II's text and
bitmap display are in: the machine boots to the BASIC start-up screen on PAL and NTSC
models. Keyboard, sound, sprites, cartridges, tape and disk are still to come.

## Building on Linux

Needs a C++20 compiler and CMake.

```
tools/build-linux.sh
```

The core lands in `bin/kernal64_libretro.so`; `bin/kernal64` starts it in RetroArch.

## System files

The core needs the machine's own ROMs, which are not included. Put them in the
frontend's system folder:

- `basic.901226-01.bin` — BASIC
- `kernal.901227-03.bin` — KERNAL (a combined 16 KB BASIC + KERNAL image also works)
- `characters.901225-01.bin` — character set
- `1541-c000.325302-01.bin` and `1541-e000.901229-05.bin` — disk drive

Files are recognized by their contents, not their names.

## License

GPL-3.0-or-later; see `LICENSE`.
