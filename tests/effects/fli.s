; FLI (VIC-II article 3.14.3): YSCROLL is set to the raster line's value in every line, so
; every raster line is a Bad Line and reads the video matrix again. The write may not come
; before cycle 14, or the row counter would be cleared in every line; so the first three
; accesses happen before the VIC-II has the bus and read $FF as the character, with the low
; four bits of the byte the processor is waiting to read as its color (the "lda" after
; the write, $BD: color 13). The video matrix base is switched in every line as well.
;
; Raster lines 79-110 are made Bad Lines by a write in cycle 14. Four video matrices take
; turns, one raster line each: the letters at $0400 and three filled with the digits 1, 2
; and 3. Expected in these 32 lines: three columns of the character $FF in light green on
; the left, and to the right of them every raster line showing its pixel row of the
; character the line's matrix holds, text lines changing every eight raster lines as usual.
; One pass of the loop is one raster line: the VIC-II stops the processor for the 40
; cycles 15-54, the rest is the loop.
IRQLINE = 76
D011    = $1b
LINES   = 32
        .include "stable.inc"

setup   ldx #0
fill    lda #$31
        sta $2000,x
        sta $2100,x
        sta $2200,x
        sta $2300,x
        lda #$32
        sta $2400,x
        sta $2500,x
        sta $2600,x
        sta $2700,x
        lda #$33
        sta $2800,x
        sta $2900,x
        sta $2a00,x
        sta $2b00,x
        inx
        bne fill
        lda #LINES
        sta count
        rts

; Entered in cycle 12 of line 78; the loop's first instruction starts in cycle 7 of line 79.
count   = $fb
        * = $0b00           ; the loop's branch must stay inside one page (a cycle more otherwise)
effect  ldx #0
PAD1    = CYCLES - 7
        .fill (PAD1 - 3 * (PAD1 & 1)) / 2, $ea
.if PAD1 & 1
        bit $ea
.endif
line    lda yscroll,x
set     sta $d011           ; in cycle 14: the Bad Line starts here
        lda matrix,x
        sta $d018
        inx
.if CYCLES == 63
        cpx #LINES
.endif
.if CYCLES == 64
        cpx count           ; a cycle longer
.endif
.if CYCLES == 65
        nop
        cpx #LINES
.endif
        bne line
        .assert (line >> 8) == (* >> 8)
        lda #$1b
        sta $d011
        lda #$15
        sta $d018
        rts

        * = $0c00           ; the tables inside one page: an indexed read across a page takes a cycle more
; YSCROLL for raster lines 79, 80, ...
yscroll .byte $1f, $18, $19, $1a, $1b, $1c, $1d, $1e, $1f, $18, $19, $1a, $1b, $1c, $1d, $1e
        .byte $1f, $18, $19, $1a, $1b, $1c, $1d, $1e, $1f, $18, $19, $1a, $1b, $1c, $1d, $1e
; $D018 for the raster line after: matrix at $2000, $2400, $2800, $0400 in turn
matrix  .byte $85, $95, $a5, $15, $85, $95, $a5, $15, $85, $95, $a5, $15, $85, $95, $a5, $15
        .byte $85, $95, $a5, $15, $85, $95, $a5, $15, $85, $95, $a5, $15, $85, $95, $a5, $15
