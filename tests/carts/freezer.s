; Our own cartridge for the hardware types with a freeze button (1, 9, 36): checks the
; freeze itself from inside the cartridge. The test harness presses the button when the
; program reaches one of the labels press1 .. press4 and lets go of it some milliseconds
; later (k64run --freeze-pc, --freeze-hold).
;   -D TYPE=n       the hardware type (1, 9 or 36)
;   -D BANK=b       which of the four banks this image is (all hold the same code)
;   -D NOFREEZE=1   type 36 only: the program switches the freeze function off first
; What the hardware does (local-notes/hw/cart.md, Z): letting go of the button makes the
; cartridge pull NMI and IRQ and wait for three write cycles in a row, which only the CPU
; taking an interrupt produces; then its ROM bank 0 goes to $E000-$FFFF, so the vector
; the CPU reads next is the cartridge's.
; Checks, by the number reported:
;   1  a freeze with interrupts disabled comes through the NMI vector; bank 0 at $E000
;      although bank 1 was selected; the CPU's flags on the stack are an interrupt's;
;      types 1 and 9: the cartridge's RAM is at $8000 and in the second I/O area, a write
;      to it does not reach the computer's RAM, and it keeps its bytes after the freeze;
;      type 36: the button reads as held while it is, and as let go in the freeze
;   2  with the NMI line already held low by CIA 2, the freeze comes through the IRQ vector
;   3  with that and interrupts disabled, nothing happens, also not on the two write
;      cycles in a row of a subroutine call or of an instruction that changes memory
;   4  ... until the CPU takes an interrupt again: the cartridge was still waiting
;   6  type 36: with bit 2 of the register written, the memory is the computer's own
;   7  types 1 and 9: the register has no read/write input, so a read of it stores what
;      the data bus holds: with the screen off, the byte the VIC-II read last
;   5  (NOFREEZE) the button reads as held, a second write to the register does not
;      switch the function back on, and no freeze comes
; Result to $D7FF: 0 = passed; else the number of the check that failed. A pass also
; writes "CART PASS" to the top of the screen memory.
step    = $02
expect  = $03               ; the vector a freeze may come through: 1 NMI, 2 IRQ, 0 none
ram     = $0340
MARK    = $5a               ; in the computer's RAM under the cartridge
CMARK   = $a5               ; in the cartridge's RAM
HIGH    = $6000             ; from an address at $8000 to the same byte seen at $E000

        * = $8000
        .word start
        .word start
        .byte $c3, $c2, $cd, $38, $30
start   sei
        ldx #$ff
        txs
        cld
        lda #$7f
        sta $dc0d
        sta $dd0d
        lda $dc0d
        lda $dd0d
        lda #0
        sta step
        ldx #ramend - ramcode
copy    lda ramcode - 1,x
        sta ram - 1,x
        dex
        bne copy
        ; An interrupt that reaches the KERNAL's vectors was not a freeze.
        lda #<stray
        sta $0314
        sta $0318
        lda #>stray
        sta $0315
        sta $0319
        lda #MARK
        sta $8123           ; the computer's RAM (a write with ROM here goes to it)

.if NOFREEZE == 1
        ; 5: the function switched off. The register's bits take the first write only.
        lda #5
        sta step
        lda #$04
        sta $de01
        lda #$00
        sta $de01
        lda #0
        sta expect
press1  ldx #0
n1      lda $de00
        and #$04
        bne n2
        dex
        bne n1
        jmp tofail          ; the button never read as held
n2      jsr wait
        cli                 ; nothing is waiting for an interrupt either
        jsr wait
        sei
        jmp passed
.else
        ; 1: through the NMI vector.
        inc step
        lda #$08            ; bank 1 (the same code): the freeze must bring bank 0
        sta $de00
        lda #1
        sta expect
press1
.if TYPE == 36
        ldx #0
h1      lda $de00
        and #$04
        bne h2
        dex
        bne h1
        jmp tofail          ; the button never read as held
h2
.endif
        jsr wait
        jmp tofail          ; no freeze came
cont1   cpx #MARK           ; the computer's RAM kept its byte
        bne fail1
.if TYPE != 36
        cpy #CMARK          ; and the cartridge's RAM its own
        beq ok1
.else
        beq ok1
.endif
fail1   jmp tofail
ok1

        ; 2: CIA 2 holds the NMI line low (its interrupt is never acknowledged), so the
        ; cartridge pulling it makes no edge: the freeze comes through the IRQ vector.
        inc step
        lda #<keep
        sta $0318
        lda #>keep
        sta $0319
        lda #$81
        sta $dd0d
        lda #$10
        sta $dd04
        lda #$00
        sta $dd05
        lda #$19            ; one shot, load, start
        sta $dd0e
        ldx #20
d2      dex
        bne d2
        lda #2
        sta expect
        cli
press2  jsr wait
        jmp tofail
cont2
        ; 3: NMI low and interrupts disabled: nothing happens.
        inc step
        sei
        lda #0
        sta expect
press3  jsr wait
        jsr twice           ; two write cycles in a row are not an interrupt
        inc $0400
        inc $0400
        ; 4: the cartridge was still waiting: the next interrupt the CPU takes is its.
        inc step
        lda #2
        sta expect
        cli
        jsr wait
        jmp tofail
cont4   lda #$7f
        sta $dd0d
        lda $dd0d
.if TYPE == 36
        ; 6: bit 2 takes the cartridge out of the memory map (and its registers with it).
        lda #6
        sta step
        jmp ram + (kill - ramcode)
.else
        ; 7: a read of the register stores the bus's byte. With the screen off the VIC-II
        ; reads $3FFF, a refresh address ($3Fxx) or a sprite pointer ($03F8-$03FF): all
        ; set to the value that selects bank 1.
        lda #7
        sta step
        lda #$08
        ldx #0
f7      sta $3f00,x
        inx
        bne f7
        ldx #7
g7      sta $03f8,x
        dex
        bpl g7
        lda $de00
        lda $9ff0           ; the bank's number
        cmp #1
        beq ok7
        jmp tofail
ok7     lda #$00
        sta $de00
.endif
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
stray
tofail  lda step
        sta $d7ff
        jmp hang
keep    rti                 ; CIA 2's interrupt, left unacknowledged
twice   rts

; About 330,000 cycles: longer than the harness holds the button.
wait    ldx #0
        ldy #0
w1      dex
        bne w1
        dey
        bne w1
        rts

; After a freeze: back to the checks of the step.
back
.if NOFREEZE == 0
        lda step
        cmp #1
        bne b2
        jmp cont1
b2      cmp #2
        bne b4
        jmp cont2
b4      cmp #4
        bne b5
        jmp cont4
b5
.endif
        jmp tofail

; ---- the freeze: this runs at $E000 + (only branches, and jumps to the RAM) ----
nmi     lda #1
        bne frozen
irq     lda #2
frozen  cmp expect
        bne ffail
        lda $fff0           ; bank 0 is where the vectors are
        bne ffail
        tsx
        lda $0101,x         ; the flags as pushed: not BRK's
        and #$10
        bne ffail
.if TYPE == 36
        lda $de00           ; the button is no longer held
        and #$04
        bne ffail
.else
        lda #CMARK          ; the cartridge's RAM where its ROM was, and in the second I/O
        sta $8123           ; area its last page
        cmp $8123
        bne ffail
        lda #$c3
        sta $df45
        cmp $df45
        bne ffail
        cmp $9f45
        bne ffail
.endif
        jmp ram
ffail   lda step
        sta $d7ff
fhang   bne fhang
        beq fhang

; ---- copied to RAM: the freeze is over ----
ramcode lda #$42            ; bit 6: over; both lines let go: the computer's own memory
        sta $de00
        ldx $8123           ; the computer's RAM
        lda #$20            ; 8K configuration with the cartridge's RAM in place of its ROM
        sta $de00
        ldy $8123
        lda #$00            ; and with its ROM, bank 0
        sta $de00
        jmp back
.if TYPE == 36
kill    lda #$04
        sta $de00
        lda $8123           ; the computer's RAM, not the cartridge's ROM
        cmp #MARK
        bne kfail
        lda #$00            ; the register is off: this brings nothing back
        sta $de00
        lda $8123
        cmp #MARK
        bne kfail
        ldx #9
kshow   lda ram + (ktext - ramcode) - 1,x
        sta $0400 - 1,x
        dex
        bne kshow
        lda #0
        sta $d7ff
khang   jmp ram + (khang - ramcode)
kfail   lda step
        sta $d7ff
        jmp ram + (khang - ramcode)
ktext   .byte 3, 1, 18, 20, 32, 16, 1, 19, 19
.endif
ramend
        .assert (ramend - ramcode) / 200 == 0       ; fits the cassette buffer
        .assert (* - $8000) / $1ff0 == 0            ; fits below the bank number
        .fill $9ff0 - *, $ff
        .byte BANK
        .fill 9, $ff
        .word nmi + HIGH    ; $FFFA
        .word nmi + HIGH    ; $FFFC (never used: a reset ends the freeze)
        .word irq + HIGH    ; $FFFE
