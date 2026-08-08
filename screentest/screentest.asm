; SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
; SPDX-License-Identifier: GPL-3.0-or-later
;
; P2000M cartridge screen test
;
; The P2000M maps its 2 KiB character RAM at 0x5000 and its 2 KiB
; attribute RAM at 0x5800.  The first 80 * 24 character locations are
; visible.  This program clears every attribute and draws a row/column ruler
; that makes missing, duplicated, or shifted screen data easy to recognize.

    org 0x1000

    ; P2000 cartridge header: signature, byte count, checksum, and the
    ; 11-byte cartridge label.  The build-time signing script replaces the
    ; zero placeholders with a count and checksum covering the first 8 KiB.
    db 0x5e, 0x00, 0x00, 0x00, 0x00
    db "SCREEN TEST"

    jp start

start:
    ; Start the elapsed timer from the monitor's 50 Hz clock at 0x6010.
    ; Timer state and the marquee pointer live in application RAM above the
    ; monitor stack, which grows down from 0x6200.
    di
    ld hl,(0x6010)
    ld (0x6210),hl
    xor a
    ld (0x6212),a
    ld (0x6213),a
    ld (0x6214),a
    ld (0x6215),a
    ld (0x6216),a
    ld hl,marquee_text
    ld (0x6217),hl
    ei

    ; Clear all 2 KiB of attribute RAM.  Attribute bits 0-3 select graphic,
    ; underline, blinking, and inverse video; zero selects ordinary text.
    xor a
    ld hl,0x5800
    ld (hl),a
    ld de,0x5801
    ld bc,0x07ff
    ldir

    ; Each visible line has this exact 80-column layout:
    ;
    ; 00|.......10........20........30........40........50........60........70.......|
    ; 01|+++++++10++++++++20++++++++30++++++++40++++++++50++++++++60++++++++70+++++++|
    ;
    ; The first number is the row (00-23).  The other numbers start at their
    ; corresponding columns.  Alternating filler distinguishes adjacent rows.
    ld hl,0x5000
    ld b,0

row_loop:
    ; Convert the binary row number in B to two decimal digits.  D temporarily
    ; holds the units digit.
    ld a,b
    cp 20
    jr c,row_below_20
    sub 20
    ld d,a
    ld a,'2'
    jr write_row_number

row_below_20:
    cp 10
    jr c,row_below_10
    sub 10
    ld d,a
    ld a,'1'
    jr write_row_number

row_below_10:
    ld d,a
    ld a,'0'

write_row_number:
    ld (hl),a
    inc hl
    ld a,d
    add a,'0'
    ld (hl),a
    inc hl
    ld a,'|'
    ld (hl),a
    inc hl

    ; Even rows use dots and odd rows use plus signs.
    ld a,b
    and 1
    ld e,'.'
    jr z,filler_ready
    ld e,'+'

filler_ready:
    ; Complete the first ten-column block after the row number and separator.
    ld c,7

fill_block:
    ld a,e
    ld (hl),a
    inc hl
    dec c
    jr nz,fill_block

    ; Write markers 10 through 70.  Every marker plus eight filler characters
    ; is exactly ten columns wide, so each marker is aligned to its value.
    ld d,'1'

column_loop:
    ld a,d
    ld (hl),a
    inc hl
    ld a,'0'
    ld (hl),a
    inc hl
    ld c,8

fill_column:
    ld a,e
    ld (hl),a
    inc hl
    dec c
    jr nz,fill_column

    inc d
    ld a,d
    cp '8'
    jr nz,column_loop

    ; Replace the filler at column 79 with the closing separator.
    dec hl
    ld a,'|'
    ld (hl),a
    inc hl

    inc b
    ld a,b
    cp 24
    jr nz,row_loop

    ; The final 128 character-RAM locations are outside the visible 80 * 24
    ; area.  Initialize them to spaces so the complete 2 KiB plane is known.
    ld a,' '
    ld b,128

clear_unused:
    ld (hl),a
    inc hl
    djnz clear_unused

    ; Draw an 18-character box on rows 10-12, centred in columns 31-48.
    ld hl,box_top
    ld de,0x533f
    ld bc,box_top_end - box_top
    ldir
    ld hl,box_middle
    ld de,0x538f
    ld bc,box_middle_end - box_middle
    ldir
    ld hl,box_bottom
    ld de,0x53df
    ld bc,box_bottom_end - box_bottom
    ldir

    ; Draw the elapsed-time box on rows 15-17, centred in columns 34-45.
    ld hl,timer_box_top
    ld de,0x54d2
    ld bc,timer_box_top_end - timer_box_top
    ldir
    ld hl,timer_box_middle
    ld de,0x5522
    ld bc,timer_box_middle_end - timer_box_middle
    ldir
    ld hl,timer_box_bottom
    ld de,0x5572
    ld bc,timer_box_bottom_end - timer_box_bottom
    ldir

    ; Scroll the adapter name from right to left through the 16-character
    ; interior of the box.  The next source character is tracked in RAM so
    ; monitor interrupts cannot disturb it.

marquee_loop:
    ; Shift the interior one position left without touching either '|'.
    ld hl,0x5391
    ld de,0x5390
    ld bc,15
    ldir

    ; Insert the next character at the right edge.  The sentinel restarts the
    ; stream after the four-space gap following the title.
    ld hl,(0x6217)
    ld a,(hl)
    cp 0xff
    jr nz,marquee_character_ready
    ld hl,marquee_text
    ld a,(hl)

marquee_character_ready:
    ld (0x539f),a
    inc hl
    ld (0x6217),hl

    ; Busy-wait long enough for each frame to remain visible.  Video-memory
    ; contention can vary the precise rate slightly on the real machine.
    ld b,150

delay_outer:
    ld c,0

delay_inner:
    dec c
    jr nz,delay_inner
    djnz delay_outer
    call update_timer
    jr marquee_loop

; Accumulate ticks from the monitor's 50 Hz clock.  Subtraction naturally
; handles wraparound of the 16-bit system counter.
update_timer:
    di
    ld hl,(0x6010)
    ei
    ld de,(0x6210)
    ld (0x6210),hl
    or a
    sbc hl,de
    ld de,(0x6212)
    add hl,de

timer_second_check:
    ld de,50
    or a
    sbc hl,de
    jr c,timer_store_remainder
    push hl
    call increment_elapsed_time
    pop hl
    jr timer_second_check

timer_store_remainder:
    add hl,de
    ld (0x6212),hl
    ret

increment_elapsed_time:
    ld a,(0x6214)
    inc a
    cp 60
    jr c,timer_store_seconds
    xor a
    ld (0x6214),a

    ld a,(0x6215)
    inc a
    cp 60
    jr c,timer_store_minutes
    xor a
    ld (0x6215),a

    ld a,(0x6216)
    inc a
    cp 100
    jr c,timer_store_hours
    xor a

timer_store_hours:
    ld (0x6216),a
    jr render_timer

timer_store_minutes:
    ld (0x6215),a
    jr render_timer

timer_store_seconds:
    ld (0x6214),a

render_timer:
    ld a,(0x6216)
    ld hl,0x5524
    call write_two_digits
    ld a,(0x6215)
    ld hl,0x5527
    call write_two_digits
    ld a,(0x6214)
    ld hl,0x552a
    call write_two_digits
    ret

; Write A as a zero-padded decimal value in the range 00-99.
write_two_digits:
    ld b,'0'

write_tens:
    cp 10
    jr c,write_units
    sub 10
    inc b
    jr write_tens

write_units:
    ld (hl),b
    inc hl
    add a,'0'
    ld (hl),a
    ret

box_top:
    db "+----------------+"
box_top_end:

box_middle:
    db "|                |"
box_middle_end:

box_bottom:
    db "+----------------+"
box_bottom_end:

timer_box_top:
    db "+----------+"
timer_box_top_end:

timer_box_middle:
    db "| 00:00:00 |"
timer_box_middle_end:

timer_box_bottom:
    db "+----------+"
timer_box_bottom_end:

marquee_text:
    db "P2000M VID2VGA    ", 0xff

    ; Emit a complete 16 KiB SLOT1 image; unused ROM bytes remain erased.
    defs 0x5000 - $, 0xff
