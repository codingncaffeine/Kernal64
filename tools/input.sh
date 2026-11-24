#!/usr/bin/env bash
# The players' controllers through the built core's API (local-notes/hw/input.md): the
# RetroPad's mapped buttons, paddles, the 1351 mouse, the light pen, the on-screen keyboard
# and the options in each version a frontend may speak. BASIC prints what the machine reads;
# each line that sends an input has a control, the same run without it, which must fail.
# Lines with a state check (--check) replay their second half from a state.
#
#   tools/input.sh [BUILD-FOLDER]      default: bin
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$(realpath "${1:-$ROOT/bin}")"
OUT="$ROOT/scratch/input"
mkdir -p "$OUT"
. "$ROOT/tools/sidebyside.sh"
CORE="$BIN/kernal64_libretro.so"

# core WANT LABEL K64HOST-ARGUMENTS...: WANT = 0, the run must pass; 1, it must fail.
core() {
    local want="$1" label="$2" log got; shift 2
    log="$OUT/host-$(printf '%s' "$label" | tr -c 'A-Za-z0-9.-' '_').log"
    "$BIN/k64host" "$CORE" --system "$ROOT/gitignore/bios" "$@" >"$log" 2>&1
    got=$?
    [ "$got" = 0 ] || got=1
    if [ "$got" = "$want" ]; then echo "ok   $label"; else echo "FAIL $label"; grep -m3 'FAIL' "$log"; fi
}
# pair LABEL "INPUT ARGUMENTS" REST...: the run passes with the input and fails without it.
pair() {
    local label="$1" input="$2"; shift 2
    # shellcheck disable=SC2086
    sbs_start core 0 "$label" "$@" $input
    sbs_start core 1 "$label (control: no input)" "$@"
}

PORT2='10 ?PEEK(56320):GOTO10\nRUN\n'        # control port 2's lines (fire = 16 low)
PORT1='10 ?PEEK(56321):GOTO10\nRUN\n'        # control port 1's
# The SID's paddle registers with port 2 switched through (keyboard interrupt off meanwhile).
POTS2='10 POKE56333,127:POKE56320,128:FORI=1TO9:NEXT:A=PEEK(54297)\n20 B=PEEK(54298):POKE56333,129:?A;B:GOTO10\nRUN\n'
PEN='10 ?PEEK(53267);PEEK(53268):GOTO10\nRUN\n'   # the light pen latch, LPX and LPY

# The RetroPad: B = fire (default), held after typing (a joystick in port 2 shares lines with
# the keyboard, so input during typing would garble it, as on the machine).
pair "B is fire in port 2" "--hold 400 499 0 b 1" --frames 500 --type-at 200 --type "$PORT2" --expect-screen ' 111'
pair "R2 is RETURN" "--hold 360 364 0 r2 1" --frames 450 --type-at 300 --type '?6*7' --expect-screen ' 42'
pair "a button mapped to a key (A = Q)" "--hold 560 564 0 a 1" --frames 600 --option kernal64_pad_a=key_q \
    --type-at 200 --type '10 GETA$:IFA$=""GOTO10\n20 ?ASC(A$)\nRUN\n' --expect-screen ' 81'
pair "repeating fire" "--hold 400 599 0 b 1" --frames 600 --option kernal64_pad_b=turbo --type-at 200 --type "$PORT2" \
    --expect-screen ' 111' --expect-screen ' 127' --check
pair "the swap button moves player 1 to port 1" "--hold 380 381 0 l 1" --frames 500 --option kernal64_pad_l=swap \
    --hold 400 499 0 b 1 --type-at 200 --type "$PORT1" --expect-screen ' 239' --check
pair "player 1 in port 1 by option" "--option kernal64_joyport=1" --frames 500 --hold 400 499 0 b 1 --type-at 200 --type "$PORT1" \
    --expect-screen ' 239'

# Paddles (P1-P3): full left 255, middle 128, full right 0; the right stick is paddle Y.
pair "paddles at both ends" "--device 0 paddles" --frames 1100 --type-at 200 --type "$POTS2" --hold 0 1099 0 lx -32768 \
    --hold 0 1099 0 rx 32767 --expect-screen ' 255  0 ' --check
sbs_start core 0 "paddles in the middle" --device 0 paddles --frames 1100 --type-at 200 --type "$POTS2" --expect-screen ' 128  128 '
# With paddles for both players, player 2's stick turns paddle Y of player 1's port.
sbs_start core 0 "player 2 turns the second paddle" --device 0 paddles --device 1 paddles --frames 1100 --type-at 200 \
    --type "$POTS2" --hold 0 1099 0 lx -32768 --hold 0 1099 1 lx 32767 --expect-screen ' 255  0 '

# The 1351 mouse (M1-M4): 3 x 10 steps right, 2 x 5 toward the user: POTX 60, POTY 108; a jump
# of 100 is passed on 31 a frame and ends at 100 mod 64 = 36 (72); left = fire, right = up.
pair "mouse movement" "--hold 800 802 0 mx 10 --hold 800 801 0 my 5" --frames 1100 --device 0 mouse --type-at 200 \
    --type "$POTS2" --expect-screen ' 60  108 ' --check
sbs_start core 0 "mouse, a long move in one frame" --frames 1100 --device 0 mouse --type-at 200 --type "$POTS2" \
    --hold 800 800 0 mx 100 --expect-screen ' 72  0 '
pair "mouse left button = fire" "--hold 400 499 0 mleft 1" --frames 500 --device 0 mouse --type-at 200 --type "$PORT2" --expect-screen ' 111'
pair "mouse right button = up" "--hold 400 499 0 mright 1" --frames 500 --device 0 mouse --type-at 200 --type "$PORT2" --expect-screen ' 126'

# The light pen (L1-L3) in port 1: pointed at the picture's middle, LPY = raster line 16 + 141,
# LPX = half the X coordinate of the cycle drawing the pixel; another point; in port 2 it sees
# nothing.
pair "light pen, middle of the picture" "--device 0 lightgun" --frames 600 --option kernal64_joyport=1 --type-at 200 --type "$PEN" \
    --hold 0 599 0 gx 0 --hold 0 599 0 gy 0 --expect-screen ' 86  157 ' --check
sbs_start core 0 "light pen, upper left" --frames 600 --option kernal64_joyport=1 --device 0 lightgun --type-at 200 --type "$PEN" \
    --hold 0 599 0 gx -20000 --hold 0 599 0 gy -20000 --expect-screen ' 26  71 '
sbs_start core 1 "light pen in port 2 sees nothing" --frames 600 --device 0 lightgun --type-at 200 --type "$PEN" \
    --hold 0 599 0 gx 0 --hold 0 599 0 gy 0 --expect-screen ' 86  157 '

# The on-screen keyboard: Select opens it on F; B types F, Start is RETURN: BASIC answers.
pair "on-screen keyboard types" "--hold 250 255 0 b 1" --frames 400 --hold 200 202 0 select 1 --hold 300 305 0 start 1 \
    --expect-screen '?SYNTAX' --check

# Blank media (what a frontend's "new blank tape / floppy" makes): a blank tape goes in with RECORD
# and PLAY down, so SAVE records at once (no "PRESS RECORD & PLAY"); started again, the recorded copy
# loads and runs. An empty formatted disk starts at READY with nothing typed.
python3 -I "$ROOT/tools/mkd64.py" "$OUT/blank.d64"
printf 'C64-TAPE-RAW\x01\x00\x00\x00\x00\x00\x00\x00' >"$OUT/blank.tap"
rm -rf "$OUT/tape1" "$OUT/tape2" "$OUT/tape3"
mkdir -p "$OUT/tape1" "$OUT/tape2" "$OUT/tape3"
blank_tape() {
    core 0 "blank tape: SAVE records at once" --content "$OUT/blank.tap" --save "$OUT/tape3" --frames 1500 \
        --type-at 150 --type '10 ?4242\nSAVE\n' --expect-screen 'SAVING' --check || return
    core 0 "blank tape: started again, the recording loads and runs" --content "$OUT/blank.tap" --save "$OUT/tape3" \
        --frames 2600 --expect-screen ' 4242'
}
sbs_start blank_tape
sbs_start core 0 "empty disk: READY, nothing typed" --content "$OUT/blank.d64" --frames 300 --expect-screen 'READY.'
sbs_start core 1 "empty disk (control: something typed)" --content "$OUT/blank.d64" --frames 300 --expect-screen 'LOAD'
# A new blank medium put in while the machine runs, through disk control (a frontend's "new floppy /
# tape" while playing): a disk goes to drive 8, a tape (libretro has no interface for tapes, so the
# core takes one handed over this way) into the datasette ready to record. A program saved to each
# loads back and runs. The states name the media, so a restarted core puts them back (--check).
cp "$OUT/blank.d64" "$OUT/inserted.d64"
cp "$OUT/blank.tap" "$OUT/inserted.tap"
rm -rf "$OUT/ins1" "$OUT/ins2"; mkdir -p "$OUT/ins1" "$OUT/ins2"
sbs_start core 0 "a blank disk put in while running: SAVE, NEW, LOAD, RUN" --save "$OUT/ins1" --frames 1400 --check \
    --insert-at 150 "$OUT/inserted.d64" --type-at 250 --type '10 ?55\nSAVE"PROG",8\n' \
    --type-at 700 --type 'NEW\nLOAD"PROG",8\n' --type-at 1200 --type 'RUN\n' --expect-screen ' 55'
sbs_start core 0 "a blank tape put in while running: SAVE, rewind, PLAY, LOAD, RUN" --save "$OUT/ins2" --frames 2600 --check \
    --option kernal64_pad_l=tape_rewind --option kernal64_pad_r=tape_play --insert-at 150 "$OUT/inserted.tap" \
    --type-at 250 --type '10 ?66\nSAVE\n' --hold 1000 1001 0 l 1 --hold 1010 1011 0 r 1 \
    --type-at 1020 --type 'NEW\nLOAD\n' --type-at 2300 --type 'RUN\n' --expect-screen ' 66'
sbs_start core 1 "a SAVE to tape with no tape in (control)" --frames 1500 --type-at 250 --type '10 ?66\nSAVE\n' --expect-screen 'SAVING'

# The datasette's buttons as mapped actions: the blank tape starts in RECORD; L (PLAY / STOP)
# stops it, SAVE asks for RECORD, R presses it and the KERNAL records; a state is taken in the middle.
TAPE=(--content "$OUT/blank.tap" --frames 1600 --option kernal64_pad_l=tape_play --option kernal64_pad_r=tape_record
      --hold 120 122 0 l 1 --type-at 150 --type '10 ?42\nSAVE\n' --expect-screen 'SAVING')
sbs_start core 0 "datasette RECORD through a mapped button" "${TAPE[@]}" --save "$OUT/tape1" --hold 400 402 0 r 1 --check
sbs_start core 1 "datasette RECORD through a mapped button (control: no input)" "${TAPE[@]}" --save "$OUT/tape2"

# Fast loading (option kernal64_fast_loading: off, auto (the default), 2x, 4x). A KERNAL tape load (a
# program the machine records first) takes about 2000 frames at the real speed, the pause after
# FOUND included: done by frame 600 at 4x, by 1000 at 2x and with auto (2x or 4x, chosen after the
# first second, kept in the state: the auto line replays from a state in a restarted core too);
# off is not done by 1000. A disk program (about 275 frames): done by 200 at 4x, not with off.
"$BIN/k64run" "$ROOT/gitignore/bios" --blank-tape --record --cycles 60000000 --type-at 3000000 $'10 ?4242\r' \
    --type-at 3300000 'SAVE"T"'$'\r' --save-tape "$OUT/fast.tap" >/dev/null 2>&1
fast() {   # fast NAME WANT FRAMES VALUE [MORE...]
    local name="$1" want="$2" frames="$3" value="$4"; shift 4
    rm -rf "$OUT/fast-$name"; mkdir -p "$OUT/fast-$name"
    core "$want" "fast loading: $name" --content "$OUT/fast.tap" --save "$OUT/fast-$name" --frames "$frames" \
        --option "kernal64_fast_loading=$value" --expect-screen ' 4242' "$@"
}
sbs_start fast "a tape at 4x" 0 600 4x --check
sbs_start fast "a tape at 2x" 0 1000 2x
sbs_start fast "a tape with auto" 0 1000 auto --check
sbs_start fast "a tape (control: off)" 1 1000 off
sbs_start core 0 "fast loading: a disk at 4x" --content "$ROOT/gitignore/external/testprogs/vdrive/test.d64" --frames 200 \
    --option kernal64_fast_loading=4x --expect-screen 'ROM:  254'
sbs_start core 1 "fast loading: a disk (control: off)" --content "$ROOT/gitignore/external/testprogs/vdrive/test.d64" \
    --frames 200 --option kernal64_fast_loading=off --expect-screen 'ROM:  254'

# The folder drive (option kernal64_folder_drive, device 10 by default: a host folder as a disk, in the
# save folder, "<content> Drive"): a program saved, NEW, loaded and run (replayed from a state); the "$"
# listing; a sequential file written with PRINT# and read with INPUT#; rename; scratch with its status;
# SAVE"@:" replaces, a second plain SAVE is refused (63 FILE EXISTS); another, absent device is reported
# absent. Control: with the drive off, device 10 is not present.
folder() {   # folder NAME WANT [K64HOST...]
    local name="$1" want="$2"; shift 2
    rm -rf "$OUT/folder-$name"; mkdir -p "$OUT/folder-$name"
    core "$want" "folder drive: $name" --save "$OUT/folder-$name" "$@"
}
sbs_start folder "SAVE, NEW, LOAD, RUN" 0 --frames 1000 --check --type-at 200 --type '10 PRINT 77\nSAVE"HELLO",10\n' \
    --type-at 500 --type 'NEW\nLOAD"HELLO",10\n' --type-at 850 --type 'RUN\n' --expect-screen ' 77'
sbs_start folder "files" 0 --frames 2600 --type-at 200 \
    --type '10 OPEN2,10,2,"DATA,S,W":PRINT#2,"HI THERE":CLOSE2\n20 OPEN2,10,2,"DATA,S,R":INPUT#2,A$:CLOSE2:PRINTA$\n30 OPEN15,10,15,"R:INFO=DATA":CLOSE15:SAVE"GONE",10\n40 OPEN15,10,15,"S:GONE":INPUT#15,E,E$,T,S:CLOSE15:PRINTE;E$;T\nRUN\n' \
    --type-at 2000 --type 'LOAD"$",10\n' --type-at 2400 --type 'LIST\n' \
    --expect-screen 'HI THERE' --expect-screen ' 1 FILES SCRATCHED 1' --expect-screen '"INFO"' --expect-screen ' SEQ'
sbs_start folder "@: replaces, a second SAVE is refused" 0 --frames 2600 --type-at 200 \
    --type '10 PRINT 1\nSAVE"P",10\n' --type-at 500 --type '10 PRINT 2\nSAVE"P",10\n' \
    --type-at 800 --type 'NEW\nLOAD"P",10\n' --type-at 1150 --type 'RUN\n' \
    --type-at 1300 --type '10 PRINT 3\nSAVE"@:P",10\n' --type-at 1650 --type 'NEW\nLOAD"P",10\n' --type-at 2000 --type 'RUN\n' \
    --expect-screen ' 1 ' --expect-screen ' 3 '
sbs_start folder "(control: off, device 10 not present)" 1 --frames 600 --option kernal64_folder_drive=off \
    --type-at 200 --type '10 PRINT 1\nSAVE"P",10\n' --expect-screen 'SAVING'
# Another device addressed while the folder drive is on: it lets go of the bus as a 1541 does, so an
# absent device is reported absent (it used to answer for every device and LOAD hung at SEARCHING).
sbs_start folder "an absent device 12 is not present" 0 --frames 1600 --option kernal64_drive8=off \
    --type-at 800 --type 'LOAD"$",12\n' --expect-screen 'DEVICE NOT PRESENT'

# The keyboard (option kernal64_keyboard). Symbolic, the default: keys type what a US keyboard
# prints (k64host types that way: SHIFT+8 is *, SHIFT+= is +, SHIFT+; is :, SHIFT+' is "); the
# same typing under the positional map must fail. Positional: the C64's own layout, its * is the
# key right of P (RETROK ]), pressed here as a raw key between the typed halves.
SYMS='PRINT 6*7+1;"A:B";(2)\n'
sbs_start core 0 "symbolic keyboard: * + : \" ( ) as printed" --frames 450 --type "$SYMS" --expect-screen ' 43 A:B 2'
sbs_start core 1 "symbolic keyboard (control: the positional map)" --frames 450 --option kernal64_keyboard=positional \
    --type "$SYMS" --expect-screen ' 43 A:B 2'
pair "positional keyboard: the C64's * key" "--hold 300 302 0 key:93 1" --frames 450 --option kernal64_keyboard=positional \
    --type-at 200 --type 'PRINT 6' --type-at 320 --type '7\n' --expect-screen ' 42'

# The machine options through the core, each with what it changes: the CIA generation (the
# serial-port interrupt probe takes its interrupt two timer counts later on the 8521, as in
# tests/probes/expected.txt); drive 8 switched off refuses a disk; the automatic video chip
# from the file name (the KERNAL's own PAL/NTSC flag at $02A6: 1 PAL, 0 NTSC).
python3 -I "$ROOT/tools/k64asm.py" "$ROOT/tests/probes/sdrirq.s" -D SHIFT=0 -D 'MASK=$88' -o "$OUT/sdrirq.prg" >/dev/null
sbs_start core 0 "CIA option 6526" --content "$OUT/sdrirq.prg" --frames 300 --option kernal64_cia=6526 --expect-ram 0400 169
sbs_start core 0 "CIA option 8521" --content "$OUT/sdrirq.prg" --frames 300 --option kernal64_cia=8521 --expect-ram 0400 171
python3 -I "$ROOT/tools/mkd64.py" "$OUT/blank.d64"
sbs_start core 0 "drive 8 off: a disk is refused" --content "$OUT/blank.d64" --frames 10 --option kernal64_drive8=off --expect-load-failure
sbs_start core 0 "drive 8 on: the disk loads" --content "$OUT/blank.d64" --frames 10 --option kernal64_drive8=on
cp "$OUT/sdrirq.prg" "$OUT/probe (NTSC).prg"
cp "$OUT/sdrirq.prg" "$OUT/probe (USA, Europe).prg"
sbs_start core 0 "automatic model: NTSC in the name" --content "$OUT/probe (NTSC).prg" --frames 300 --expect-ram 02a6 0
sbs_start core 0 "automatic model: PAL otherwise" --content "$OUT/probe (USA, Europe).prg" --frames 300 --expect-ram 02a6 1
sbs_start core 0 "automatic model: the option wins" --content "$OUT/probe (NTSC).prg" --frames 300 --option kernal64_model=pal --expect-ram 02a6 1

# Options in each version a frontend may speak (v2 with categories, v1, plain variables).
for v in 2 1 0; do
    sbs_start core 0 "options version $v" --options-version $v --frames 400 --check --type 'PRINT 6*7\n' --expect-screen ' 42'
done
sbs_start core 1 "an option value the core does not declare is caught" --frames 30 --option kernal64_border=huge

sbs_finish
