; Linecrunch (VIC-II article 3.14.4): a Bad Line that is taken back before cycle 14 leaves
; the sequencer in display state with the row counter still at 7 from the text line
; before. In cycle 58 the text line therefore "ends" at once and the video counter base
; moves on by 40: a whole text line is used up in one raster line.
;
; The fifth text line (E) would start in raster line 83. In COUNT raster lines from 83 on,
; YSCROLL is set to the next line's value by a write in cycle AT, so each of these lines
; begins as a Bad Line and stops being one. The line after them is a normal Bad Line.
; Expected: A-D, then COUNT raster lines that each show the last pixel row of a text line
; with the characters fetched before, then the text line COUNT places further on, and the
; rest of the screen COUNT * 7 raster lines higher. With COUNT 30 the counter passes the
; end of the 25 lines: the 24 bytes behind them appear and the matrix starts again.
IRQLINE = 80
D011    = $1b
FIRST   = 83                ; the first crunched raster line
        .include "stable.inc"

setup   rts

; Entered in cycle 12 of line 82. One pass of the loop is one raster line.
        * = $0b00           ; the loop's branch must stay inside one page (a cycle more otherwise)
effect  ldx #0
PAD1    = CYCLES + AT - 27
        .fill (PAD1 - 3 * (PAD1 & 1)) / 2, $ea
.if PAD1 & 1
        bit $ea
.endif
crunch  txa
        clc
        adc #FIRST + 1
        and #7
        ora #$18
set     sta $d011           ; in cycle AT
PAD2    = CYCLES - 21
        .fill (PAD2 - 3 * (PAD2 & 1)) / 2, $ea
.if PAD2 & 1
        bit $ea
.endif
        inx
        cpx #COUNT
        bne crunch
        .assert (crunch >> 8) == (* >> 8)
        lda #248            ; YSCROLL back to 3 below the last possible Bad Line
low     cmp $d012
        bne low
        lda #$1b
        sta $d011
        rts
