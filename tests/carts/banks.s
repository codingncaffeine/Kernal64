; Our own cartridge: checks the switching logic of one cartridge hardware type from inside
; the cartridge. The KERNAL finds the signature at $8004 after a reset and jumps here.
;   -D TYPE=n     the hardware type (the number in the cartridge file's header)
;   -D BANK=b     which bank this image is
;   -D NBANKS=n   how many banks the cartridge has
;   -D PART=0|1   0 = the chip at $8000 (the code); 1 = a chip at $A000 (types 4 and 18),
;                 or the second half of a 16K bank (types 3, 21, 53; type 70: at $E000)
; Type 3 has a freeze button: the test harness presses it when the program reaches the
; label press1 (k64run --freeze-pc) and holds it for a while.
; Every bank at $8000 holds the same code and its own number at $9FF0 (and $8FF0), so the
; code goes on running when the bank under it changes; what would pull the ROM away from
; under itself runs from RAM ($0340).
; Result to $D7FF: 0 = passed; else the number of the check that failed. A pass also
; writes "CART PASS" to the top of the screen memory, for checks that read the screen.
step    = $02
ram     = $0340
MARK    = $5a               ; written to the RAM under the ROM
HIGH    = $c4               ; in the chips at $A000, plus the bank number

.if PART == 1
.if TYPE == 70
        ; The Ultimax configuration: this half is at $E000 and holds the vectors.
        * = $e000
        .fill $1ff0, $ff
        .byte HIGH + BANK
        .fill 9, $ff
        .word $8009, $8009, $8009
.else
.if TYPE == 3
        ; After a freeze this half is at $E000 and the NMI vector leads here. A write to
        ; the register takes it away again, so that is done from the first I/O area,
        ; which shows the first half's page $9E00 in every configuration.
        * = $a000
fz3     jmp $de00
        .fill $bff0 - *, $ff
        .byte HIGH + BANK
        .fill 9, $ff
        .word fz3 + $4000, fz3 + $4000, fz3 + $4000
.else
        * = $a000
        .fill $1ff0, $ff
        .byte HIGH + BANK
        .fill 15, $ff
.endif
.endif
.else
        * = $8000
        .word start
        .word start
        .byte $c3, $c2, $cd, $38, $30
start
.if TYPE == 3
        jmp begin
        jmp frozen3         ; $800C: where the freeze comes back to
begin
.endif
        sei
        ldx #$ff
        txs
        cld
        lda #0
        sta step
        ldx #ramend - ramcode
copy    lda ramcode - 1,x
        sta ram - 1,x
        dex
        bne copy

.if (TYPE == 5) | (TYPE == 7) | (TYPE == 15) | (TYPE == 17) | (TYPE == 19) | (TYPE == 3) | (TYPE == 21) | (TYPE == 53) | (TYPE == 70)
        ; 1: every bank shows its own number.
        inc step
        ldx #0
banks
.if TYPE == 5
        txa
        ora #$80            ; the register's upper bits do nothing
        sta $de00
.endif
.if TYPE == 7
        lda regs,x
        sta $de00
.endif
.if TYPE == 15
        sta $de00,x         ; the address is the bank
.endif
.if TYPE == 17
        lda $de00,x         ; a read; the address is the bank
.endif
.if (TYPE == 19) | (TYPE == 21)
        stx $de00
.endif
.if TYPE == 53
        txa
        asl a               ; bit 1 the half, bits 3-2 the chip
        sta $de80
.endif
.if TYPE == 70
        sta $de00,x         ; the address is the bank
.endif
.if TYPE == 3
        txa
        ora #$40            ; bit 6: the NMI line let go
        sta $dfff
.endif
        cpx $9ff0
        bne tofail
.if (TYPE == 3) | (TYPE == 21) | (TYPE == 53) | (TYPE == 70)
        txa                 ; the bank's second half
        clc
        adc #HIGH
.if TYPE == 70
        cmp $fff0
.else
        cmp $bff0
.endif
        bne tofail
.endif
.if (TYPE == 5) & (NBANKS != 64)
        cpx $bff0           ; the same bank in the second window
        bne tofail
.endif
        inx
        cpx #NBANKS
        bne banks
.if (TYPE == 5) & (NBANKS == 64)
        ; 2: the kind with 64 banks has one window: the second one is not the cartridge's.
        inc step
        dex
        cpx $bff0
        beq tofail
.endif
.endif

.if TYPE == 21
        ; 2: the 8K configuration: the second half is not the cartridge's.
        lda #0
        sta $de00
        inc step
        lda #$40
        sta $de00
        lda $bff0
        cmp #HIGH
        beq tofail
        lda #0
        sta $de00
.endif
.if TYPE == 53
        ; 2: the register is in the upper half of the I/O area only.
        lda #0
        sta $de80
        inc step
        lda #$02
        sta $de00
        lda $9ff0
        bne tofail
.endif
.if TYPE == 70
        ; 2: bit 7 of the address written: no more changes.
        inc step
        sta $de80           ; bank 0, and that is that
        sta $de05
        lda $9ff0
        bne tofail
.endif
.if TYPE == 3
        ; 2: the I/O areas show the last two pages of the bank's first half.
        inc step
        lda #$41
        sta $dfff
        lda $dff0
        cmp #1
        bne tofail
        lda $def0
        cmp #$e1
        bne tofail
        ; 3: the GAME line let go: the second half is not the cartridge's.
        inc step
        lda #$60
        sta $dfff
        lda $bff0
        cmp #HIGH
        beq tofail
        lda #$40
        sta $dfff
        ; 4: switched off, the RAM underneath shows.
        inc step
        lda #MARK
        sta $9ff0
        jsr ram
        cpx #MARK
        bne tofail
        ; 5 and 6, from RAM: off with the register hidden, then the freeze.
        inc step
        jmp ram + (hide3 - ramcode)
.endif

.if (TYPE == 7) | (TYPE == 19) | (TYPE == 21)
        ; 2: switched off, the RAM underneath shows; 3: switched on again, bank 0.
        inc step
        lda #MARK
        sta $9ff0           ; a write goes to the RAM under the ROM
        jsr ram
        cpx #MARK
        bne tofail
.if TYPE == 21
        cpy #$ff            ; (and the empty socket reads as that)
        bne tofail
.endif
        inc step
        lda $9ff0
        bne tofail
.endif

.if TYPE == 15
        ; 2: a read in the register's area goes back to bank 0.
        inc step
        ldx #3
        sta $de00,x
        lda $9ff0
        cmp #3
        bne tofail
        lda $de00
        lda $9ff0
        bne tofail
.endif

.if TYPE == 4
        ; 1: after a reset the second chip is in; 2: a read in the register's area
        ; takes it out; 3: a write puts it back.
        inc step
        lda $bff0
        cmp #HIGH
        bne tofail
        inc step
        lda $de00
        lda $bff0
        cmp #HIGH
        beq tofail
        inc step
        sta $de00
        lda $bff0
        cmp #HIGH
        bne tofail
.endif

.if (TYPE == 10) | (TYPE == 18) | (TYPE == 53)
        ; The routine in RAM returns the number of its check that failed, or 0.
        jsr ram
        sta step
        bne tofail
.endif

passed  ldx #passend - pass
show    lda pass - 1,x
        sta $0400 - 1,x
        dex
        bne show
        lda #0
        sta $d7ff
hang    jmp hang
pass    .byte 3, 1, 18, 20, 32, 16, 1, 19, 19      ; screen codes
passend
tofail  lda step
        sta $d7ff
        jmp hang
.if TYPE == 3
        ; 6: the button brought us here through the cartridge's own NMI vector.
frozen3 ldx #$ff
        txs
        lda step
        cmp #6
        bne tofail
        lda $bff0           ; the write to the register was taken: the 16K configuration
        cmp #HIGH
        bne tofail
        lda #$40            ; the lines let go
        sta $dfff
        jmp passed
.endif

.if TYPE == 7
regs    .byte $00, $08, $10, $18, $20, $28, $30, $38, $01, $09, $11, $19, $21, $29, $31, $39
.endif

; ---- copied to RAM ----
ramcode
.if TYPE == 21
        lda #$e0            ; the top three bits: off
        sta $de00
        ldx $9ff0
        lda #$04            ; a bank of the second socket, which is empty
        sta $de00
        ldy $9ff0
        lda #$00
        sta $de00
.endif
.if TYPE == 3
        lda #$70            ; both lines let go
        sta $dfff
        ldx $9ff0
        lda #$40
        sta $dfff
        rts
        ; 5: off, and bit 7: the register takes no more writes.
hide3   lda #$f0
        sta $dfff
        lda #$40            ; (this would switch the cartridge on)
        sta $dfff
        lda $9ff0
        cmp #MARK
        bne fail3
        ; 6: the button: NMI at once, GAME seven cycles later: with EXROM let go, the
        ; vector is read from the cartridge's second half.
        inc step
wait3   ldx #0
        ldy #0
w3      dex
        bne w3
        dey
        bne w3
fail3   lda step
        sta $d7ff
hang3   jmp ram + (hang3 - ramcode)
.endif
.if TYPE == 53
        ; 3: the RAM chosen: 16K in both windows, two halves; 4: off: the computer's RAM,
        ; which took the same writes.
        lda #$08            ; chip 2 = the RAM, first half
        sta $de80
        lda #$11
        sta $8123
        lda #$22
        sta $a123
        lda #$0a            ; second half
        sta $de80
        lda #$33
        sta $8123
        lda #$08
        sta $de80
        lda $8123
        cmp #$11
        bne r3
        lda $a123
        cmp #$22
        bne r3
        lda #$0a
        sta $de80
        lda $8123
        cmp #$33
        bne r3
        lda #$10            ; off
        sta $de80
        lda $8123
        cmp #$33
        bne r4
        lda #$00
        sta $de80
        lda #0
        rts
r3      lda #$00
        sta $de80
        lda #3
        rts
r4      lda #$00
        sta $de80
        lda #4
        rts
.endif
.if TYPE == 19
        lda #$80            ; bit 7: off
        sta $de00
        ldx $9ff0
        lda #$00
        sta $de00
.endif
.if TYPE == 7
        lda #$86            ; off
        sta $de00
        ldx $9ff0
        lda #$00
        sta $de00
.endif
.if TYPE == 18
        ; The 4K chip shows twice; which copy was read last picks the bank at $A000.
        lda $8ff0
        cmp $9ff0
        bne r1
        lda $9ff0           ; the upper copy: bank 1
        lda $bff0
        cmp #HIGH + 1
        bne r2
        lda $8ff0           ; the lower copy: bank 0
        lda $bff0
        cmp #HIGH
        bne r3
        lda #0
        rts
r1      lda #1
        rts
r2      lda #2
        rts
r3      lda #3
        rts
.endif
.if TYPE == 10
        ; A read of the ROM or of the first I/O area keeps the cartridge on for 512 cycles.
        lda #MARK
        sta $9ff0           ; the RAM under the ROM
        lda $de00           ; on
        ldx #60             ; about 300 cycles
w1      dex
        bne w1
        lda $9ff0           ; still the ROM (and this read counts as well)
        bne r1
        ldx #60
w2      dex
        bne w2
        lda $9ff0           ; so it is still there
        bne r2
        ldx #140            ; about 700 cycles
w3      dex
        bne w3
        lda $9ff0           ; gone: the RAM
        cmp #MARK
        bne r3
        lda $dff0           ; the second I/O area shows the ROM's last page all the same
        bne r4
        lda $9ff0           ; and that read did not bring the ROM back
        cmp #MARK
        bne r5
        lda $de00           ; back on
        lda $9ff0
        bne r6
        lda #0
        rts
r1      lda #1
        rts
r2      lda #2
        rts
r3      lda #3
        rts
r4      lda #4
        rts
r5      lda #5
        rts
r6      lda #6
        rts
.endif
        rts
ramend
        .assert (ramend - ramcode) / 200 == 0       ; fits the cassette buffer
        .assert (* - $8000) / $0ff0 == 0            ; fits below the bank number
        .fill $8ff0 - *, $ff
        .byte BANK
        .fill 15, $ff
.if TYPE == 3
        .fill $0e00, $ff
        lda #$00            ; $9E00 = $DE00: hold the NMI and GAME lines through the
        sta $dfff           ; register (the 16K configuration), then on at $800C
        jmp $800c
        .fill $9ef0 - *, $ff
        .byte $e0 + BANK    ; $9EF0: seen in the first I/O area
        .fill 15, $ff
        .fill $00f0, $ff
        .byte BANK
        .fill 15, $ff
.else
.if TYPE != 18
        .fill $0ff0, $ff
        .byte BANK
        .fill 15, $ff
.endif
.endif
.if TYPE == 3
press1  = ram + (wait3 - ramcode)
.endif
.endif
