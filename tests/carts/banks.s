; Our own cartridge: checks the switching logic of one cartridge hardware type from inside
; the cartridge. The KERNAL finds the signature at $8004 after a reset and jumps here.
;   -D TYPE=n     the hardware type (the number in the cartridge file's header)
;   -D BANK=b     which bank this image is
;   -D NBANKS=n   how many banks the cartridge has
;   -D PART=0|1   0 = the chip at $8000 (the code); 1 = a chip at $A000 (types 4 and 18)
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
        * = $a000
        .fill $1ff0, $ff
        .byte HIGH + BANK
        .fill 15, $ff
.else
        * = $8000
        .word start
        .word start
        .byte $c3, $c2, $cd, $38, $30
start   sei
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

.if (TYPE == 5) | (TYPE == 7) | (TYPE == 15) | (TYPE == 17) | (TYPE == 19)
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
.if TYPE == 19
        stx $de00
.endif
        cpx $9ff0
        bne tofail
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

.if (TYPE == 7) | (TYPE == 19)
        ; 2: switched off, the RAM underneath shows; 3: switched on again, bank 0.
        inc step
        lda #MARK
        sta $9ff0           ; a write goes to the RAM under the ROM
        jsr ram
        cpx #MARK
        bne tofail
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

.if (TYPE == 10) | (TYPE == 18)
        ; The routine in RAM returns the number of its check that failed, or 0.
        jsr ram
        sta step
        bne tofail
.endif

        ldx #passend - pass
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

.if TYPE == 7
regs    .byte $00, $08, $10, $18, $20, $28, $30, $38, $01, $09, $11, $19, $21, $29, $31, $39
.endif

; ---- copied to RAM ----
ramcode
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
.if TYPE != 18
        .fill $0ff0, $ff
        .byte BANK
        .fill 15, $ff
.endif
.endif
