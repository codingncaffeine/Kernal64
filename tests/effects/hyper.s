; Hyperscreen (VIC-II article 3.14.1): the border is switched on and off where the raster
; position equals a comparison value chosen by RSEL (lines) and CSEL (columns). Changing
; the bit so that the comparison is never met leaves the border as it was.
;
; Upper/lower border: RSEL is cleared in raster line 248 (its comparison value, 247, is
; already past) and set again after line 251: the vertical border never closes, and the
; idle pattern is shown above and below the 25 lines.
; Left/right border, in the LINES raster lines from 252 on (in the opened lower border):
;   SIDE 1: CSEL is cleared by a write in cycle AT and set again six cycles later.
;           AT 56: the right border does not close and the line stays open into the next
;           line's left border. AT 55: it closes at the 38-column position. AT 57: at the
;           40-column position, as if nothing was written.
;   SIDE 0: CSEL is clear at the start of the line and set by a write in cycle AT (and
;           cleared again after the right border closed). AT 17: the left border does not
;           open for the whole line. AT 16: it opens at the 40-column position. AT 18: at
;           the 38-column position.
IRQLINE = 246
D011    = $1b
LINES   = 8
        .include "stable.inc"

setup   rts

.if SIDE
V1      = $c0
V2      = $c8
PADA    = 0
.else
V1      = $c8
V2      = $c0
PADA    = 37
.endif

; Entered in cycle 12 of line 248.
        * = $0b00           ; the loop's branch must stay inside one page (a cycle more otherwise)
effect  lda #$13
        sta $d011
        ldx #LINES
PAD1    = 4 * CYCLES + AT - 5 - 20
        .fill (PAD1 - 3 * (PAD1 & 1)) / 2, $ea
.if PAD1 & 1
        bit $ea
.endif
side    lda #V1
first   sta $d016           ; in cycle AT
        .fill (PADA - 3 * (PADA & 1)) / 2, $ea
.if PADA & 1
        bit $ea
.endif
        lda #V2
        sta $d016
PADB    = CYCLES - 17 - PADA
        .fill (PADB - 3 * (PADB & 1)) / 2, $ea
.if PADB & 1
        bit $ea
.endif
        dex
        bne side
        .assert (side >> 8) == (* >> 8)
        lda #$c8
        sta $d016
        lda #$1b
        sta $d011
        rts
