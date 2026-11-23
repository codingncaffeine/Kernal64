; Doubled text lines (VIC-II article 3.14.5): a text line ends when its eighth raster line
; reaches cycle 58 with the row counter at 7. A Bad Line Condition asserted in cycles 54-57
; of that raster line keeps the sequencer in display state and lets the row counter wrap
; to 0: the same text line is shown again, because no new video matrix data was read.
;
; The fifth text line (E) covers raster lines 83-90. In line 90 YSCROLL is set to 2
; (= 90 and 7) by a write in cycle AT, and back to 3 in line 91.
; Expected for AT 54-57: A-E, then E a second time, then G: while E is repeated the video
; counter runs through F's addresses, so F is never shown. AT 53 and AT 58 are the cycles
; just outside the window; what they show follows from the same rules (53: the condition
; is seen in cycle 54, where a Bad Line still makes a c-access; 58: too late for this
; line's check, the sequencer goes idle). The -D AT=nn variants in expected.txt pin both
; edges against the reference.
IRQLINE = 87
D011    = $1b
        .include "stable.inc"

setup   rts

; Entered in cycle 12 of line 89. One line is CYCLES cycles; the write of "sta" is its
; fourth cycle.
        * = $0b00           ; the loops' branches must stay inside one page (a cycle more otherwise)
effect  ldx #9              ; 2 + 9*5 - 1 = 46 cycles: cycle 58 of line 89
wait    dex
        bne wait
PAD     = CYCLES + AT - 63  ; cycles from here to the "lda" two cycles before the "sta"
        .fill (PAD - 3 * (PAD & 1)) / 2, $ea
.if PAD & 1
        bit $ea
.endif
        lda #$1a
set     sta $d011           ; in cycle AT of line 90
        ldx #12
hold    dex
        bne hold
        lda #$1b
        sta $d011           ; in line 91
        rts
