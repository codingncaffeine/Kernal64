; Probe: the paddle registers through the switch that connects the control ports to the
; SID. CIA 1 port A line 6 high switches control port 1's pair of paddle lines through,
; line 7 high control port 2's; with both, the two ports' resistances are in parallel.
; Run with a pair of paddles in each port (k64run --paddles):
;   port 1: X1, Y1    port 2: X2, Y2    both in parallel: XB, YB
; Result to $D7FF: 0 = every step read what it should; else the number of the first step
; that did not (1 none selected, 2 port 1, 3 port 2, 4 both, 5 both lines as inputs,
; 6 none again).
        * = $0801
        .basic
        sei                 ; the KERNAL's keyboard scan writes port A
        lda #$c0
        sta $dc02           ; lines 6 and 7 are outputs
        lda #$00
        ldx #$ff
        ldy #$ff
        jsr step            ; 1: no port
        lda #$40
        ldx #X1
        ldy #Y1
        jsr step            ; 2: port 1
        lda #$80
        ldx #X2
        ldy #Y2
        jsr step            ; 3: port 2
        lda #$c0
        ldx #XB
        ldy #YB
        jsr step            ; 4: both
        lda #$00
        sta $dc02           ; both lines are inputs: they float high
        ldx #XB
        ldy #YB
        jsr step            ; 5: both, the port register says none
        lda #$c0
        sta $dc02
        lda #$00
        ldx #$ff
        ldy #$ff
        jsr step            ; 6: no port
        lda #0
        sta $d7ff
hang    jmp hang

; Port A = A; three conversions later (one may have been under way) the registers must
; read X and Y.
step    inc number
        sta $dc00
        stx wantx
        sty wanty
        ldy #2              ; 2 * 256 * 5 cycles and more
        ldx #0
wait    dex
        bne wait
        dey
        bne wait
        lda $d419
        cmp wantx
        bne bad
        lda $d41a
        cmp wanty
        bne bad
        rts
bad     lda number
        sta $d7ff
        jmp hang
number  .byte 0
wantx   .byte 0
wanty   .byte 0
