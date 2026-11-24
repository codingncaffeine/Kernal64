<p align="center"><img src="assets/logo-github.png" alt="Kernal64" width="420"></p>

# Kernal64

Another Commodore 64 emulator: written from scratch, cycle-exact, and shipped as a
libretro core for RetroArch and Emutastic.

**Status:** early development, no release yet. The whole machine is in: the 6510, both CIA
generations, the VIC-II in its PAL and NTSC chip generations, the 6581 and 8580 SIDs,
cartridges (plain, bank-switched and freezers), a 1541 running its own DOS (D64, G64 and NIB
images), the datasette (TAP), and a CMD HD hard drive running its own DOS. Save states,
rewind, run-ahead and netplay work with all of it.

**Documentation is in the [wiki](https://github.com/codingncaffeine/Kernal64/wiki):**
[system files](https://github.com/codingncaffeine/Kernal64/wiki/System-files),
[content](https://github.com/codingncaffeine/Kernal64/wiki/Content),
[drives and media](https://github.com/codingncaffeine/Kernal64/wiki/Drives-and-media),
[hard drives](https://github.com/codingncaffeine/Kernal64/wiki/Hard-drives),
[controls](https://github.com/codingncaffeine/Kernal64/wiki/Controls),
[core options](https://github.com/codingncaffeine/Kernal64/wiki/Core-options) and the project's
[test cartridges](https://github.com/codingncaffeine/Kernal64/wiki/Test-cartridges).

## System files

The machine's own ROMs are not included: put them in the frontend's system folder. They are
recognized by their contents, not their names. You need BASIC, KERNAL and the character set;
the 1541's two ROMs for disk images; and the CMD HD's boot ROM for hard drives. The full list
is on the [System files](https://github.com/codingncaffeine/Kernal64/wiki/System-files) page.

## Building on Linux

Needs a C++20 compiler and CMake.

```
tools/build-linux.sh
```

The core lands in `bin/kernal64_libretro.so`; `bin/kernal64` starts it in RetroArch.

## Test cartridges

The repository has its own test cartridges (`tests/carts/`) that check bank-switched,
flash and freezer cartridge types from inside the cartridge. They run on any emulator that
takes `.crt` files. What each one checks, and why the results can be trusted, is on the
[Test cartridges](https://github.com/codingncaffeine/Kernal64/wiki/Test-cartridges) page.

## License

GPL-3.0-or-later; see `LICENSE`.
