; DMA delay (VIC-II article 3.14.6): a Bad Line Condition that first appears in cycles
; 15-53 of a raster line, while the sequencer is idle, starts the video matrix accesses in
; the middle of the line. Fewer than 40 of them happen, the video counter is left that
; many short, and every following text line starts that much further left in memory: the
; picture is shifted right. The first three accesses happen before the VIC-II has the bus:
; they read $FF as the character and, as its color, the low four bits of the byte the
; processor is waiting to read (here the NOP after the write, $EA: color 10).
;
; The display starts with YSCROLL 5, so the first Bad Line would be raster line 53, and
; lines 51 and 52, the first two of the window, are in idle state. YSCROLL is set to 4 by
; a write in cycle AT of line 52 and stays there until the end of the display.
; Expected for AT 15-53: line 51 and the left of line 52 show the idle pattern; in line 52
; the first text line starts with three $FF characters, and the picture is shifted right
; by AT - 14 columns, the columns that leave on the right coming back on the left one text
; line lower. AT 54: too late to start the accesses in this line.
;
; In the cycle in which the accesses start, the chip fetches its idle byte from $38FF
; (6567/6569) or $3807 (8562/8565) instead of $3FFF (measured chips; the reference suite's
; vsp-tester). $38FF gets the pattern MARK and $3807 the pattern MARK2: with $55, the idle
; pattern, in both, the picture is the same either way; any other value shows in the eight
; pixels left of the three $FF characters, which the reference emulator does not reproduce.
IRQLINE = 50
D011    = $1d
        .include "stable.inc"

setup   lda #MARK
        sta $38ff
        lda #MARK2
        sta $3807
        rts

; Entered in cycle 12 of line 52.
effect  lda #$1c
PAD1    = AT - 17
        .fill (PAD1 - 3 * (PAD1 & 1)) / 2, $ea
.if PAD1 & 1
        bit $ea
.endif
set     sta $d011           ; in cycle AT
        nop
        lda #248            ; YSCROLL back to 5 below the last possible Bad Line
low     cmp $d012
        bne low
        lda #D011
        sta $d011
        rts
