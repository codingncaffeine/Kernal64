; Sprite stretching (VIC-II article 3.14.7): a sprite line is shown again as long as the
; Y expansion flip-flop is clear when the data counter base would be taken over in cycle
; 16. Clearing the sprite's Y expansion bit sets the flip-flop, setting the bit again lets
; cycle 55/56 clear it: done in every raster line after cycle 16, the same sprite line is
; fetched again and again. Clearing the bit in cycle 15 instead makes the counter base
; advance by one byte instead of three ("sprite crunch"): the data is read out of step
; from there on, and the sprite does not end after 21 lines.
;
; Sprite 0, Y expanded, Y = 51; its lines carry different patterns. The screen has no Bad
; Lines (the display is enabled only after raster line 48), so the window shows the idle
; pattern and the processor only loses the five cycles of the sprite's accesses per line.
; From raster line 52 on, in LINES raster lines, the Y expansion bit is cleared by a write
; in cycle AT and set again six cycles later.
; Expected: AT 17, LINES 20: the sprite's first line 22 raster lines high, the rest as
; usual. LINES 1: AT 16 or 17 the first line three raster lines high, AT 15 the crunch,
; AT 14 an unstretched sprite.
IRQLINE = 47
D011    = $1b
YS      = 51
        .include "stable.inc"

setup   ldx #62             ; the sprite's 21 lines: a count, a bar, a dot
        ldy #21
shape   lda #$81
        sta $3000,x
        dex
        lda bars,y
        sta $3000,x
        dex
        tya
        asl
        asl
        ora #1
        sta $3000,x
        dey
        dex
        bpl shape
        lda #0              ; the 64th byte: read when the crunch puts the counter out of step
        sta $303f
        lda #$c0
        sta $07f8
        lda #120
        sta $d000
        lda #YS
        sta $d001
        lda #0
        sta $d010
        sta $d01c
        sta $d01d
        sta $d01b
        lda #1
        sta $d027
        sta $d017
        sta $d015
        rts
bars    .byte $00, $ff, $7f, $3f, $1f, $0f, $07, $03, $01, $ff, $7f, $3f, $1f, $0f, $07, $03, $01, $ff, $7f, $3f, $1f, $0f

; Entered in cycle 12 of line 49. The display is turned on here, after line 48, so that
; the frame has no Bad Lines; it is turned off again at the end for the next frame.
        * = $0b00           ; the loop's branch must stay inside one page (a cycle more otherwise)
effect  lda #$1b
        sta $d011
        ldx #LINES
; to the "lda" five cycles before the write in line 52: three lines, less the six cycles
; above, the two of "ldx" and the five the sprite's accesses take in line 51
PAD1    = 3 * CYCLES + AT - 5 - 12 - 8 - 5
        .fill (PAD1 - 3 * (PAD1 & 1)) / 2, $ea
.if PAD1 & 1
        bit $ea
.endif
again   lda #$00
clear   sta $d017           ; in cycle AT
        lda #$01
        sta $d017
PAD2    = CYCLES - 22       ; one pass is one raster line, five cycles of it the sprite's
        .fill (PAD2 - 3 * (PAD2 & 1)) / 2, $ea
.if PAD2 & 1
        bit $ea
.endif
        dex
        bne again
        .assert (again >> 8) == (* >> 8)
        lda #$0b
        sta $d011
        rts
