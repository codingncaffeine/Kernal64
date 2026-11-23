; Flexible line distance (VIC-II article 3.14.2): a Bad Line starts every text line, so
; holding the Bad Line back by keeping YSCROLL away from the raster line opens a gap
; between two text lines without moving a byte of the screen.
;
; Text lines start at raster lines 51, 59, ... (YSCROLL 3); the fifth one (E) ends in line
; 90. From line 86 on, for LINES raster lines, YSCROLL is set two lines ahead of the
; raster in every line, so line 91 and the following ones are no Bad Lines. Then YSCROLL
; goes back to 3 and the sixth text line starts at the next raster line that ends in 3.
; Expected: lines A-E as usual, a gap in idle state (the pattern of address $3FFF in black
; on the background) down to raster line 106, then F and the rest 16 lines lower than
; usual, the last two text lines cut off by the lower border.
IRQLINE = 84
D011    = $1b
LINES   = 20
        .include "stable.inc"

setup   rts

effect  ldx #LINES
ahead   lda $d012
        clc
        adc #2
        and #7
        ora #$18
        sta $d011
        lda $d012
next    cmp $d012           ; the next raster line
        beq next
        dex
        bne ahead
        lda #$1b
        sta $d011
        rts
