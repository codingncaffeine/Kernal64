; Our own cartridge for hardware type 32: checks its two flash chips' command logic from
; inside the cartridge. The chip behind ROMH holds this code (the same in banks 0 and 8, so
; it goes on running when the bank changes) and is left alone; the chip behind ROML is
; identified, programmed and erased. The machine starts the cartridge in the Ultimax
; configuration from its own reset vector, and stays in it: only there do writes reach
; the chips.
;   -D PART=0     the code, for the ROMH chip at $E000
;   -D PART=1     a ROML bank: all $FF but its first byte, MARK0 (bank 0) or MARK8 (bank 8)
;   -D BANK=b     which bank a PART=1 image is
;   -D STRICT=1   also the checks of what the chip's data sheet says and the reference
;                 emulator does differently (status while a byte is being programmed,
;                 the time-out's status bit, reads and programming during a suspended erase)
; Result to $D7FF: 0 = passed; else the number of the check that failed.
step    = $02
ptr     = $fb               ; what wait reads
want    = $fd
inner   = $fe
outer   = $ff
MARK0   = $a5
MARK8   = $5a

.if PART == 1
        * = $8000
.if BANK == 0
        .byte MARK0
.else
        .byte MARK8
.endif
        .fill $1fff, $ff
.else
        * = $e000
reset   sei
        ldx #$ff
        txs
        cld
        lda #0
        sta step
        sta $de00           ; bank 0

        ; 1: the chip shows its contents.
        inc step
        lda $8000
        cmp #MARK0
        bne tofail0

        ; 2: the identifier codes: manufacturer, device, sector not protected.
        inc step
        ldx #$90
        jsr cmd
        lda $8000
        cmp #$01
        bne tofail0
        lda $8001
        cmp #$a4
        bne tofail0
        lda $8002
        bne tofail0

        ; 3: a reset command ends that.
        inc step
        lda #$f0
        sta $8000
        lda $8000
        cmp #MARK0
        bne tofail0

        jmp ok3
tofail0 jmp tofail
ok3
        ; 4: only the low 11 address bits count in a command.
        inc step
        lda #$aa
        sta $9555
        lda #$55
        sta $8aaa
        lda #$90
        sta $8d55
        lda $8001
        cmp #$a4
        bne tofail1
        lda #$f0
        sta $8000

        ; 5: a command at the wrong address is none.
        inc step
        lda #$aa
        sta $8555
        lda #$55
        sta $8555
        lda #$90
        sta $8555
        lda $8001
        cmp #$ff
        bne tofail1
        lda #$f0
        sta $8000

        ; 6: programming a byte.
        inc step
        ldx #$a0
        jsr cmd
        lda #$3c
        sta $9f00
.if STRICT
        lda $9f00           ; still busy: bit 7 is the complement of the byte's bit 7
        bpl tofail1
.endif
        lda #$3c
        jsr wait9f00
        bcs tofail1
        lda $9f00
        cmp #$3c
        beq ok6
tofail1 jmp tofail
ok6
        ; 7: a 0 does not go back to 1: the chip reports the failure (bit 5) until it is
        ; reset, and the byte keeps its zeros. ($43 over $3C leaves $00: neither bit 5 nor
        ; bit 7 can come from the byte itself.)
        inc step
        ldx #$a0
        jsr cmd
        lda #$43
        sta $9f00
        ldx #0
f7      lda $9f00
        and #$20
        bne g7
        inx
        bne f7
        beq tofail1
g7      lda $9f00
        bpl tofail1         ; bit 7: the complement of $43's
        lda #$f0
        sta $8000
        lda $9f00
        bne tofail1

        ; 8: erasing a sector: all $FF, the next sector untouched.
        inc step
        jsr erase0
.if STRICT
        lda $9f00           ; in the time-out for naming more sectors: bit 3 clear,
        and #$08
        bne tofail1
.endif
        lda $9f00           ; and bit 6 changes from read to read
        eor $9f00
        and #$40
        beq tofail1
        ldx #30             ; past the time-out
d8      dex
        bne d8
        lda $9f00
        bmi tofail1         ; bit 7 clear while erasing
        and #$08
        beq tofail1         ; bit 3 set: the erase has begun
        lda #$ff
        jsr wait9f00
        bcs tofail2
        lda $8000
        cmp #$ff
        bne tofail2
        lda #8
        sta $de00
        lda $8000
        cmp #MARK8
        bne tofail2
        lda #0
        sta $de00

        ; 9: another command during the time-out calls the erase off.
        inc step
        ldx #$a0
        jsr cmd
        lda #MARK0
        sta $8000
        lda #MARK0
        jsr wait8000
        bcs tofail2
        jsr erase0
        lda #$f0
        sta $8000
        ldx #30
d9      dex
        bne d9
        lda $8000
        cmp #MARK0
        bne tofail2
        lda $8000
        cmp #MARK0
        beq ok9
tofail2 jmp tofail
ok9
        ; 10: an erase put on hold and taken up again.
        inc step
        jsr erase0
        ldx #30
d10     dex
        bne d10
        lda #$b0
        sta $8000
.if STRICT
        lda $9f00           ; the sector on hold: bit 7 set,
        bpl tofail2
        eor $9f00           ; bit 6 steady, bit 2 changing from read to read
        and #$44
        cmp #$04
        bne tofail2
        lda #8              ; another sector reads as usual and can be programmed
        sta $de00
        lda $8000
        cmp #MARK8
        bne tofail2
        ldx #$a0
        jsr cmd
        lda #$0f
        sta $9f00
        lda #$0f
        jsr wait9f00
        bcs tofail2
        lda #0
        sta $de00
.endif
        lda #$30
        sta $8000
        lda #$ff
        jsr wait8000
        bcs tofail2

        ; 11: erasing the whole chip.
        inc step
        ldx #$80
        jsr cmd
        ldx #$10
        jsr cmd
        lda #8
        sta $de00
        lda #$ff
        jsr wait8000
        bcs tofail
        lda $9f00
        cmp #$ff
        bne tofail

        lda #0
        sta $d7ff
hang    jmp hang
tofail  lda step
        sta $d7ff
        jmp hang

; The two unlocking writes, then X as the command.
cmd     lda #$aa
        sta $8555
        lda #$55
        sta $82aa
        stx $8555
        rts

; The sector erase command for the first sector.
erase0  ldx #$80
        jsr cmd
        lda #$aa
        sta $8555
        lda #$55
        sta $82aa
        lda #$30
        sta $8000
        rts

; Reads $9F00 / $8000 until it gives A (for up to 14 million cycles: a whole-chip erase
; takes 8 million). Carry clear when it did.
wait9f00
        ldx #$00
        ldy #$9f
        bne wait
wait8000
        ldx #$00
        ldy #$80
wait    stx ptr
        sty ptr + 1
        sta want
        lda #9
        sta outer
w0      ldx #0
        stx inner
w1      ldy #0
        lda (ptr),y
        cmp want
        beq wok
        pha
        pla
        inx
        bne w1
        inc inner
        bne w1
        dec outer
        bne w0
        sec
        rts
wok     clc
        rts

        .assert (* - $e000) / $1ffa == 0
        .fill $fffa - *, $ff
        .word reset         ; NMI
        .word reset         ; RESET
        .word reset         ; IRQ
.endif
