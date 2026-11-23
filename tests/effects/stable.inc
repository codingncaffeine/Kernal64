; Shared start-up of the raster-effect programs: a picture to work on and a raster
; interrupt that calls the program's routine at the same cycle of the same line in every
; frame, on any of the chips.
;
; The including program defines before the include
;   IRQLINE   raster line of the interrupt; neither it nor the next line may be a Bad Line
;   D011      the value of $D011 the display starts with ($1B: 25 lines, YSCROLL 3)
;   CYCLES    cycles per raster line: 63 (6569), 64 (6567R56A) or 65 (6567R8)
;   JIT       1 makes the main program run long instructions, so that the interrupt arrives
;             at varying cycles: the picture must not depend on it
; (its own names start with st_) and after it the routines
;   setup     called once with interrupts off, before the display is turned on
;   effect    called in every frame; its first instruction starts in cycle 12 of line
;             IRQLINE+2. It ends with rts and may use A, X and Y.
;
; The picture: every text line is filled with one letter (A in the first line, B in the
; second, ...), the color RAM gives every column its own color, light blue border, blue
; background. The 24 bytes of the video matrix after the 25 lines hold the digits and
; letters from "0" on, in white. The pattern shown in idle state (address $3FFF) is
; %01010101.

        * = $0801
        .basic
        sei
        lda #$35            ; RAM instead of the ROMs, I/O visible
        sta $01
        lda #$7f            ; no interrupts from the CIAs
        sta $dc0d
        sta $dd0d
        lda $dc0d
        lda $dd0d
        lda #0
        sta $d011           ; display off while the picture is built
        sta $d015
        lda #$55
        sta $3fff

        ldx #0              ; screen: line n holds the letter with screen code n+1
        ldy #1
st_row     tya
st_col
st_scr     sta $0400,x
        inx
        cpx #40
        bne st_col
        ldx #0
        clc
        lda st_scr+1
        adc #40
        sta st_scr+1
        bcc st_same
        inc st_scr+2
st_same    iny
        cpy #26
        bne st_row
        ldx #23             ; the 24 bytes after the 25 lines: digits and letters, white
st_rest    txa
        clc
        adc #48
        sta $07e8,x
        lda #1
        sta $dbe8,x
        dex
        bpl st_rest

        ldx #0              ; color RAM: column c has color (c mod 15) + 1, never the background's
st_tint    txa
st_mod15   cmp #15
        bcc st_small
        sbc #15
        bcs st_mod15
st_small   clc
        adc #1
        cmp #6
        bne st_keep
        lda #1
st_keep    ldy #0
st_paint   sta $d800,x
        sta $d828,x
        sta $d850,x
        sta $d878,x
        sta $d8a0,x
        sta $d8c8,x
        sta $d8f0,x
        sta $d918,x
        sta $d940,x
        sta $d968,x
        sta $d990,x
        sta $d9b8,x
        sta $d9e0,x
        sta $da08,x
        sta $da30,x
        sta $da58,x
        sta $da80,x
        sta $daa8,x
        sta $dad0,x
        sta $daf8,x
        sta $db20,x
        sta $db48,x
        sta $db70,x
        sta $db98,x
        sta $dbc0,x
        inx
        cpx #40
        bne st_tint

        lda #14
        sta $d020
        lda #6
        sta $d021
        lda #$c8            ; 40 columns
        sta $d016
        lda #$15            ; screen at $0400, character set at $1000 (the ROM's)
        sta $d018
        jsr setup

        lda #<st_first
        sta $fffe
        lda #>st_first
        sta $ffff
        lda #<st_back
        sta $fffa
        lda #>st_back
        sta $fffb
        lda #<IRQLINE
        sta $d012
        lda #D011 | ((IRQLINE >> 1) & $80)
        sta $d011
        lda #$01
        sta $d01a
        sta $d019
        cli
.if JIT
st_hang    inc $c000,x         ; a seven-cycle instruction: the first interrupt comes up to six cycles late
        jmp st_hang
.else
st_hang    jmp st_hang
.endif

; First interrupt, in line IRQLINE: it arrives up to two cycles late, depending on the
; instruction it interrupted. It asks for a second interrupt in the next line and waits
; for it in two-cycle instructions, so that one arrives at most one cycle late.
st_first   sta st_keepa+1
        stx st_keepx+1
        sty st_keepy+1
        lda #<st_second
        sta $fffe
        lda #>st_second
        sta $ffff
        inc $d012
        lda #$01
        sta $d019
        tsx
        cli
        .fill 40, $ea

; Second interrupt, in line IRQLINE+1. The last cycle of the cmp reads RASTER in the last
; cycle of the line (then the values are equal and the branch takes three cycles) or, one
; cycle late, in the first cycle of the next line (not equal, two cycles): from here on
; the cycle is the same in every frame.
st_second  txs
        ldx #8
st_wait    dex
        bne st_wait
.if CYCLES == 63
        bit $ea
.endif
.if CYCLES == 64
        nop
        nop
.endif
.if CYCLES == 65
        nop
        bit $ea
.endif
        lda $d012
        cmp $d012
        beq st_level
st_level   nop                 ; cycles 4 and 5 of line IRQLINE+2
        jsr effect          ; six cycles more; the routine starts in cycle 12

        lda #<st_first
        sta $fffe
        lda #>st_first
        sta $ffff
        lda #<IRQLINE
        sta $d012
        lda #$01
        sta $d019
st_keepa   lda #0
st_keepx   ldx #0
st_keepy   ldy #0
st_back    rti
