; Probe: when does a light pen trigger set the interrupt flag ($D019 bit 3)?
; The pen input is CIA 1 port B bit 4, pulled low here through the port.
; Result to $D7FF: bit 0 = the flag after a trigger in the middle of a frame,
;                  bit 1 = the flag after the line was held low over the start of a frame.
; The first chip revisions (6569R1, 6567R56A) set it only in the second case.
        * = $0801
        .basic
        sei
        lda #$7f
        sta $dc0d
        sta $dd0d
        lda $dc0d
        lda $dd0d
        lda #0
        sta $d01a           ; flags only, no interrupts
        sta result
        lda #$ff
        sta $dc01
        lda #$10
        sta $dc03           ; port B bit 4 is an output
        ldx #3              ; let a few frames pass
f1      bit $d011
        bpl f1
f2      bit $d011
        bmi f2
        dex
        bne f1
        jsr line100
        lda #$0f
        sta $d019           ; clear the flags
        lda #$ef
        sta $dc01           ; pen line low: the position is latched
        nop
        nop
        lda $d019
        and #$08
        beq no1
        lda #1
        sta result
no1     lda #$ff
        sta $dc01
        lda #$0f
        sta $d019
        lda #200            ; later in the same frame: no second latch
w200    cmp $d012
        bne w200
        lda #$ef
        sta $dc01           ; held low until after the next frame has started
        jsr line100
        lda $d019
        and #$08
        beq no2
        lda result
        ora #2
        sta result
no2     lda #$ff
        sta $dc01
        lda result
        sta $d7ff
hang    jmp hang

line100 lda #100            ; raster line 100 (no line 356 exists)
w100    cmp $d012
        bne w100
        rts
result  = $fb
