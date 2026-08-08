; SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
; SPDX-License-Identifier: GPL-3.0-or-later
;
; P2000M cartridge screen test
;
; The P2000M maps its 2 KiB character RAM at 0x5000 and its 2 KiB
; attribute RAM at 0x5800.  The first 80 * 24 character locations are
; visible.  Space switches between a timer/marquee screen-test ruler and an
; animated Matrix-style character-rain display.

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
    xor a
    ld (0x6219),a               ; current mode: 0=timer/marquee, 1=matrix
    ld (0x621a),a               ; Space-key debounce latch
    ld hl,(0x6010)
    ld a,h
    xor 0xa5
    ld h,a
    ld a,h
    or l
    jr nz,random_seed_ready
    inc hl

random_seed_ready:
    ld (0x621b),hl              ; seed the Matrix pseudo-random generator
    ei

    call draw_regular_screen

main_loop:
    ld a,(0x6219)
    or a
    jr nz,matrix_frame

regular_frame:
    call update_marquee
    ld b,150
    call frame_delay
    call update_timer
    call poll_mode_switch
    jr z,main_loop
    call enter_selected_mode
    jr main_loop

matrix_frame:
    call update_matrix
    ld b,30
    call frame_delay
    call update_timer
    call poll_mode_switch
    jr z,main_loop
    call enter_selected_mode
    jr main_loop

; Render whichever mode was selected by Space.  Keeping this separate from the
; animation loops makes switching redraw the complete screen in one operation.
enter_selected_mode:
    ld a,(0x6219)
    or a
    jp z,draw_regular_screen
    jp init_matrix_screen

; Clear the complete character and attribute planes.
clear_screen:
    ld a,' '
    ld hl,0x5000
    ld (hl),a
    ld de,0x5001
    ld bc,0x07ff
    ldir

    xor a
    ld hl,0x5800
    ld (hl),a
    ld de,0x5801
    ld bc,0x07ff
    ldir
    ret

draw_regular_screen:

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

    ; Returning from Matrix mode must show the accumulated time immediately;
    ; it may not yet be time for the next one-second timer update.
    call render_timer
    ret

; Scroll the adapter name from right to left through the 16-character interior
; of the box.  The next source character is tracked in application RAM.
update_marquee:
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
    ret

; Busy-wait with B selecting the frame duration.  Video-memory contention can
; vary the precise rate slightly on the real machine.
frame_delay:
    ld c,0

frame_delay_inner:
    dec c
    jr nz,frame_delay_inner
    djnz frame_delay
    ret

; Toggle modes once per Space press using the monitor's keyboard buffer.  The
; monitor scans the keyboard every 20 ms and exposes non-blocking STATUSKEY at
; 0x0029 and buffered READKEY at 0x0026.  Space has matrix keycode 0x11 (row 2,
; bit 1); these routines return keycodes rather than display/ASCII values.
;
; Do not access keyboard ports directly here.  In particular, port 0x10 is a
; write-only latch shared by the cassette, printer, and keyboard interrupt.
; A non-zero return value reports a new mode selection.
poll_mode_switch:
    ; The monitor's last-key state becomes 0xff when all keys are released.
    ; Clear our latch as soon as Space is no longer the current key.  Buffered
    ; auto-repeat events are then harmlessly consumed below while it is held.
    ld a,(0x621a)
    or a
    jr z,mode_key_buffer
    ld a,(0x600d)
    cp 0x11
    jr z,mode_key_buffer
    xor a
    ld (0x621a),a

mode_key_buffer:
    call 0x0029                   ; STATUSKEY: Z means buffer empty
    ret z
    call 0x0026                   ; READKEY: keycode returned in A
    cp 0x11
    jr nz,mode_key_buffer         ; discard unrelated buffered keys

    ld a,(0x621a)
    or a
    jr nz,mode_key_buffer         ; ignore Space auto-repeat events
    inc a
    ld (0x621a),a
    ld a,(0x6219)
    xor 1
    ld (0x6219),a
    ld a,1
    or a                          ; successful toggle must return NZ in both directions
    ret

; Set up eighty independent rain columns.  Head positions are staggered over
; the visible area and the blank gap below it; countdowns create four speeds.
init_matrix_screen:
    call clear_screen
    ld ix,0x6220                 ; head positions, 80 bytes
    ld iy,0x6270                 ; speed countdowns, 80 bytes
    ld b,80

matrix_column_init:
    call random_byte
    and 0x1f
    ld (ix+0),a
    call random_byte
    and 3
    ld (iy+0),a
    inc ix
    inc iy
    djnz matrix_column_init

    ; A calm, centred inverse-video footer identifies the control.  Rain uses
    ; rows 0-22 and the footer occupies row 23.
    ld hl,matrix_footer
    ld de,0x5744
    ld bc,matrix_footer_end - matrix_footer
    ldir
    ld a,0x08
    ld hl,0x5f44
    ld b,matrix_footer_end - matrix_footer

matrix_footer_attributes:
    ld (hl),a
    inc hl
    djnz matrix_footer_attributes
    ret

; Advance all rain columns by one animation frame.  Existing trail characters
; remain in VRAM, so characters change mainly as a drop moves or during the
; single random "code flicker" performed at the end of each frame.
update_matrix:
    ld ix,0x6220
    ld iy,0x6270
    ld b,80
    ld c,0

matrix_column_loop:
    ld a,(iy+0)
    or a
    jr z,matrix_advance_column
    dec (iy+0)
    jp matrix_next_column

matrix_advance_column:
    ; Column number selects a stable delay of one through four frames.
    ld a,c
    and 3
    inc a
    ld (iy+0),a

    ; The old leading glyph becomes an underlined near-head glyph and changes
    ; once.  Inverse video on the new head substitutes for unavailable levels
    ; of phosphor brightness.
    ld a,(ix+0)
    cp 23
    jr nc,matrix_normalize_follower
    call row_column_address
    push hl
    call random_matrix_character
    pop hl
    ld (hl),a
    ld de,0x0800
    add hl,de
    ld (hl),0x02

matrix_normalize_follower:
    ; Remove underline from the next glyph back, leaving a clean dense trail.
    ld a,(ix+0)
    or a
    jr z,matrix_erase_tail
    dec a
    cp 23
    jr nc,matrix_erase_tail
    call row_column_address
    ld de,0x0800
    add hl,de
    ld (hl),0

matrix_erase_tail:
    ; Character density supplies a tiny visual fade where variable brightness
    ; is unavailable: the final visible trail cell becomes a single dot.
    ld a,(ix+0)
    cp 4
    jr c,matrix_clear_expired_tail
    sub 4
    cp 23
    jr nc,matrix_clear_expired_tail
    call row_column_address
    ld (hl),'.'
    ld de,0x0800
    add hl,de
    ld (hl),0

matrix_clear_expired_tail:
    ; Six glyphs form a drop.  Erase the cell that has just fallen behind it.
    ld a,(ix+0)
    cp 5
    jr c,matrix_move_head
    sub 5
    cp 23
    jr nc,matrix_move_head
    call row_column_address
    ld (hl),' '
    ld de,0x0800
    add hl,de
    ld (hl),0

matrix_move_head:
    ld a,(ix+0)
    inc a
    cp 36                       ; visible rows + trail + a blank inter-drop gap
    jr c,matrix_store_head
    xor a

matrix_store_head:
    ld (ix+0),a
    cp 23
    jr nc,matrix_next_column
    call row_column_address
    push hl
    call random_matrix_character
    pop hl
    ld (hl),a
    ld de,0x0800
    add hl,de
    ld (hl),0x08

matrix_next_column:
    inc ix
    inc iy
    inc c
    dec b
    jp nz,matrix_column_loop

    ; Occasionally alter one existing trail glyph, echoing the restless code
    ; changes in Matrix rain without making the whole screen shimmer at once.
matrix_pick_flicker_row:
    call random_byte
    and 0x1f
    cp 23
    jr nc,matrix_pick_flicker_row
    push af

matrix_pick_flicker_column:
    call random_byte
    and 0x7f
    cp 80
    jr nc,matrix_pick_flicker_column
    ld c,a
    pop af
    call row_column_address
    ld a,(hl)
    cp ' '
    ret z
    push hl
    call random_matrix_character
    pop hl
    ld (hl),a
    ret

; Convert row A (0-23) and column C (0-79) into a character-RAM address.
row_column_address:
    add a,a
    ld e,a
    ld d,0
    ld hl,screen_rows
    add hl,de
    ld e,(hl)
    inc hl
    ld d,(hl)
    ex de,hl
    ld e,c
    ld d,0
    add hl,de
    ret

; 16-bit Galois LFSR.  It is small, deterministic, and more than adequate for
; varying character rain on a 2.5 MHz machine.
random_byte:
    ld hl,(0x621b)
    srl h
    rr l
    jr nc,random_store
    ld a,h
    xor 0xb4
    ld h,a

random_store:
    ld (0x621b),hl
    ld a,l
    ret

; Produce A-Z most of the time and digits 0-5 for visual punctuation.
random_matrix_character:
    call random_byte
    and 0x1f
    cp 26
    jr c,matrix_letter
    sub 26
    add a,'0'
    ret

matrix_letter:
    add a,'A'
    ret

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
    jr render_timer_if_visible

timer_store_minutes:
    ld (0x6215),a
    jr render_timer_if_visible

timer_store_seconds:
    ld (0x6214),a

render_timer_if_visible:
    ld a,(0x6219)
    or a
    ret nz

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
    db "P2000M VID2VGA    SPACE: MATRIX MODE    ", 0xff

matrix_footer:
    db " MATRIX RAIN // SPACE: TIMER + MARQUEE "
matrix_footer_end:

; Character-RAM address of the first cell in every visible row.
screen_rows:
    dw 0x5000, 0x5050, 0x50a0, 0x50f0, 0x5140, 0x5190
    dw 0x51e0, 0x5230, 0x5280, 0x52d0, 0x5320, 0x5370
    dw 0x53c0, 0x5410, 0x5460, 0x54b0, 0x5500, 0x5550
    dw 0x55a0, 0x55f0, 0x5640, 0x5690, 0x56e0, 0x5730

    ; Emit a complete 16 KiB SLOT1 image; unused ROM bytes remain erased.
    defs 0x5000 - $, 0xff
