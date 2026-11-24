; Probe: at which cycle does the CPU take a CIA 1 interrupt, counted by timer B?
; -D SHIFT=0/1: one extra cycle before the NOP slide; -D MASK=$88 serial port, $81 timer A.
; The result (timer B low byte at the handler's first instruction) goes to $D7FF and $0400.
        * = $0801
        .basic
        sei
        lda #$7f
        sta $dc0d
        sta $dd0d
        lda $dc0d
        lda $dd0d
        lda #0
        sta $dc0e
        sta $dc0f
        sta $d011          ; display off: no bad lines
        sta $d015
        lda #$35
        sta $01
        lda #<irq
        sta $fffe
        lda #>irq
        sta $ffff
        ldx #3             ; a few frames, so that the display is off for a whole frame
w1      bit $d011
        bpl w1
w2      bit $d011
        bmi w2
        dex
        bne w1
        lda #$ff
        sta $dc06
        sta $dc07
        lda #3
        sta $dc04
        lda #0
        sta $dc05
        lda #MASK
        sta $dc0d
        lda #$11
        sta $dc0f          ; timer B: start, force load
        lda #$51
        sta $dc0e          ; timer A: start, force load, serial output
        lda #$55
        sta $dc0c
        cli
.if SHIFT
        bit $ea
.endif
        .fill 120, $ea
        lda #$fe           ; no interrupt came
        sta $d7ff
hang    jmp hang
irq     lda $dc06
        sta $d7ff
        sta $0400          ; also on the screen, for a run without the debug cartridge
hang2   jmp hang2
