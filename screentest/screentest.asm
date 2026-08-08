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

    ; P2000 cartridge header: signature, zero-length checksum, and the
    ; 11-byte cartridge label.  A zero byte count and checksum are accepted
    ; by the monitor, which starts execution at address 0x1010.
    db 0x5e, 0x00, 0x00, 0x00, 0x00
    db "SCREEN TEST"

    jp start

start:
    di

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

    ; Animate a spinner in row 11, column 50, just to the right of the box.
    ; Appendix D of the P2000 T&M System Reference Manual maps 0x5c to the
    ; fraction 1/2, not backslash, so use only known P2000 glyphs here.
    ld hl,0x53a2
    ld de,spinner_frames

spinner_loop:
    ld a,(de)
    cp 0xff
    jr nz,spinner_ready
    ld de,spinner_frames
    ld a,(de)

spinner_ready:
    ld (hl),a
    inc de

    ; Busy-wait long enough for each frame to remain visible.  Video-memory
    ; contention can vary the precise rate slightly on the real machine.
    ld b,150

delay_outer:
    ld c,0

delay_inner:
    dec c
    jr nz,delay_inner
    djnz delay_outer
    jr spinner_loop

box_top:
    db "+----------------+"
box_top_end:

box_middle:
    db "| P2000M VID2VGA |"
box_middle_end:

box_bottom:
    db "+----------------+"
box_bottom_end:

spinner_frames:
    db '|', '/', '-', 0xff

    ; Emit a complete 16 KiB SLOT1 image; unused ROM bytes remain erased.
    defs 0x5000 - $, 0xff
