<p align="center"><img src="assets/logo-github.png" alt="Kernal64" width="420"></p>

# Kernal64

Another Commodore 64 emulator: written from scratch, cycle-exact, and shipped as a
libretro core for RetroArch and Emutastic.

**Status:** early development. The 6510 CPU, the memory map, both generations of the CIA
with keyboard and joysticks, and the VIC-II with sprites, in its old and new chip
generations for PAL and NTSC, are in: the machine boots to BASIC and takes typing. The
SID is in for the 6581 and the 8580, from the oscillators to the filter and the volume
stage, and can be heard. Cartridges and single-file programs start from the frontend.
Tape and disk are still to come.

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
- `1541-c000.325302-01.bin` and `1541-e000.901229-05.bin` — disk drive (a combined 16 KB
  image also works; without them drive 8 is off)

Files are recognized by their contents, not their names.

## Content

- Cartridge files (`.crt`): plain 8K, 16K and Ultimax cartridges, the bank-switched
  hardware types 4, 5, 7, 8, 10, 15, 17, 18, 19, 21, 32, 53, 61 and 70 of the file
  format, and the freezer types 1, 3, 9 and 36 (both boards of type 36). What a type 32 cartridge writes to
  its flash chips is kept in save states, not yet from one session to the next. Other
  types are refused with a message.
- A freezer cartridge's button is Page Down on the keyboard and a click of the right
  stick on the RetroPad (remappable in the frontend). Without such a cartridge it does
  nothing.
- Program files (`.prg`, `.p00`) and the first program of a `.t64` container: put into
  memory once the machine has booted, and started with `RUN`.
- Disk images (`.d64` with or without error bytes and with 35, 40 or 42 tracks, `.g64`,
  and `.nib` raw reads mounted directly, without converting them first) in drive 8, a 1541 emulated
  down to its read circuit and running its own DOS ROM: started with `LOAD"*",8,1` and
  `RUN`, as on the real machine. What the drive writes (SAVE, formatting, a game's saves)
  goes into a copy in the frontend's save folder (`<name>.g64`), which is loaded in place of
  the original next time; your own image file is never written.
- Tapes (`.tap`, versions 0 and 1) in the datasette: started with PLAY down, `LOAD` and `RUN`
  typed, turbo loaders included. The RetroPad's L2 is PLAY / STOP and R2 rewinds (remappable);
  what the datasette records goes into a copy in the save folder (`<name>.tap`).
- Disk lists (`.m3u`, one image path per line) for software on several disks: swap with the
  frontend's disk control (eject, choose, insert); the DOS sees the change as it would on a
  real drive.

## Core options

| Option | Values (default first) | What it selects |
| --- | --- | --- |
| `kernal64_model` | `pal`, `pal-new`, `pal-old`, `ntsc`, `ntsc-new`, `ntsc-old` | The video chip: 6569R3, 8565, 6569R1, 6567R8, 8562, 6567R56A |
| `kernal64_cia` | `6526`, `8521` | The CIA generation |
| `kernal64_sid` | `6581`, `8580` | The SID chip |
| `kernal64_sid_noise_pulse` | `drains it`, `leaves it alone` | 6581 only. Real 6581s differ in what noise combined with pulse does to the noise generator: on most it empties it within a few cycles, on some it does not |
| `kernal64_border` | `full`, `small`, `none` | How much of the border is shown |
| `kernal64_drive8` | `on`, `off` | A 1541 as drive 8 on the serial bus (needed for disk images) |
| `kernal64_joyport` | `2`, `1` | The control port the RetroPad's joystick is in |

## Test cartridges

The repository has its own test cartridges, written for this project. They run on any
emulator that takes `.crt` files, and should run on the real hardware of each type (we
have not done that).

- `tests/carts/banks.s` checks a bank-switched cartridge type from inside the cartridge.
  One source is assembled once per bank and per hardware type (3, 4, 5, 7, 10, 15, 17,
  18, 19, 21, 53, 70). Every bank holds the same code and its own number at a fixed address, so the code
  goes on running when the bank under it changes, and it can ask each bank who it is.
  What would switch the ROM away from under itself runs from RAM. It walks every bank
  through the type's register, then that type's extras: the second window, the off
  switch and the RAM that shows behind it, switches made by a read, a cartridge's own
  RAM in its halves, a register that locks or hides itself, and for type 10 the
  512-cycle timer from both sides (still on after 300 cycles, off after 700). Type 3's
  last step is its freeze button (NMI at once, the GAME line seven cycles later).
- `tests/carts/flash.s` checks type 32's two flash chips. It runs from one chip and works
  on the other: identifier codes, which address bits count in a command, a command at
  the wrong address, programming a byte, a refused 0-to-1, sector erase, an erase called
  off in its time-out, an erase put on hold and resumed, chip erase. `-D STRICT=1` adds
  checks taken from the chip's data sheet: status bits while busy, and reading and
  programming other sectors while an erase is on hold.

- `tests/carts/freezer.s` checks the freeze of types 1, 9 and 36. The test harness
  presses the cartridge's button when the program reaches a label. It checks that the
  freeze comes through the NMI vector with bank 0 where the vectors are; through the IRQ
  vector when the NMI line is already held low; not at all while both are blocked, also
  not on the two write cycles of a subroutine call; and late, once the CPU takes an
  interrupt again. Types 1 and 9: the cartridge's RAM in the freeze. Type 36: the button
  as a readable bit, the bit that switches the function off, the bit that takes the
  cartridge out of the memory map. This one needs a harness that can press the button
  at an address (`k64run --freeze-pc`), so VICE is not asked.

A cartridge reports one byte to `$D7FF`: 0 for a pass, otherwise the number of the check
that failed. That is the register of VICE's debug cartridge (`x64sc -debugcart` exits
with the value); `banks.s` also writes `CART PASS` into screen memory.

Why the result can be trusted:

- The same chips are also packed under another type's header, where the check has to
  fail at a named step (`tests/carts/expected.txt`).
- VICE reports the same value as this core on every line (`K64_CARTS_REF=1`), except
  where it departs from the flash chip's data sheet and where a step needs the button
  pressed at a chosen moment; those lines record its value.
- For each rule of our implementation a deliberately planted fault turns a line red.
- Type 36's freeze is also compared with real cartridges: `tools/freezer.sh` runs the
  freeze test program of the VICE test suite and compares what it finds, byte by byte,
  with the dumps people saved with that program on five cartridges of both boards.

`tools/carts.sh` builds and runs them all. It needs only Python 3: `tools/k64asm.py` is
the assembler, `tools/mkcrt.py` packs the images into `.crt` files (in `scratch/carts/`).

## License

GPL-3.0-or-later; see `LICENSE`.
