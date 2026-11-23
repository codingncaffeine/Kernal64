; Probe: what a program finds when it was put into memory and started with RUN. BASIC's
; LOAD leaves the address after the program's last byte as the start of variables ($2D),
; of arrays ($2F) and of free space ($31), and the KERNAL as its end of load ($AE).
; Result to $D7FF: 0 = all four; else the zero-page address of the first that differs.
; A pass also writes "PRG PASS" into the screen memory (row 12), for checks that read the
; screen.
        * = $0801
        .basic
        ldx #0
next    ldy ptrs,x
        lda $0000,y
        cmp #<end
        bne bad
        lda $0001,y
        cmp #>end
        bne bad
        inx
        cpx #4
        bne next
        ldx #passend - pass
show    lda pass - 1,x
        sta $0400 + 12 * 40 - 1,x
        dex
        bne show
        lda #0
        sta $d7ff
hang    jmp hang
bad     sty $d7ff
        jmp hang
ptrs    .byte $2d, $2f, $31, $ae
pass    .byte 16, 18, 7, 32, 16, 1, 19, 19      ; screen codes
passend
end
