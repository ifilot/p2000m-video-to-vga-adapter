<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-License-Identifier: CC-BY-4.0
-->

# Monitor 80 composite timing investigation

Investigation date: 2026-09-18. Source comparison: working tree against
`dd01de3` (`HEAD` during this investigation).

The user reports a complete picture with `pal-sync 7`, `pal-shift 0`, and
adjustments to the monitor's rear controls. This establishes a working
combination on this monitor; it does not establish a universal PAL correction.
The particular controls adjusted and their original positions were not recorded.

## Signal and timing

The adapter reconstructs composite video from a decoded framebuffer; it does
not forward the P2000M's original sync waveform. Composite has picture and
sync levels on one wire, while VGA has RGB and separate horizontal/vertical
sync outputs. Both outputs read the same decoded source, with independent
framebuffer holds and scanout hardware.

| Property | Composite | VGA |
| --- | --- | --- |
| Output clock | 14 MHz sample rate | 25.2 MHz pixel clock |
| Line length | 896 samples / 64 us | 800 pixels / 31.746 us |
| Horizontal rate | 15.625 kHz | 31.5 kHz |
| Frame structure | 625 lines, 312.5 lines per field | 525 lines, progressive |
| Refresh | 50 fields/s, 25 frames/s | 60 frames/s |
| Picture | 640 x 288 source pixels in each field | 640 x 480 output raster |
| Hardware | PIO2, GPIO14–15 | PIO0, GPIO0–13 |

Composite emits the same source-row geometry in both interlaced fields,
selecting the latest available source independently at each field boundary.
It does not alternate odd/even rows of a 576-row source framebuffer.

Ordinary composite horizontal sync is 66 samples (4.714 us); equalising
pulses are 33 samples (2.357 us); broad sync pulses are 382 samples
(27.286 us), repeated every 448 samples (32 us). Source pixels start 191
samples (13.643 us) after the horizontal timing origin. None of these pulse
widths or the source pixel clock changed during the experiments.

ITU-R BT.470 Figure 2 and Table 1-2 specify pre-equalising, broad-sync and
post-equalising intervals of 2.5 lines each for the relevant 625-line systems,
with half-line pulse spacing. Thus the original 2.5-line pre-equalising
interval is expected, not by itself an error. The earlier suggestion that
this interval alone identified the bug was too strong.

Reference: [ITU-R BT.470-6](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.470-6-199811-S!!PDF-E.pdf).

## What the experiments establish

- Moving the picture down one line produced no reported visible improvement.
- Advancing broad sync in half-line steps produced progressive reported
  improvement. The image remained stable.
- Full coverage finally required monitor adjustments as well as the current
  diagnostic timing. Timing, vertical geometry and monitor adjustment are
  therefore not yet isolated from each other.
- The photographs are not precise measurements of missing scanlines or field
  phase. Earlier confident counts of the upper guide bars were unreliable.

At `pal-shift 0`, source rows occupy zero-based waveform lines 23–310 and
336–623, both inclusive. In normal sync mode, broad sync begins at waveform
line 2.5 and 315. In mode 7 it begins at line -1 (wrapped to line 624) and
311.5. The picture is unchanged, so broad sync precedes it by an additional
3.5 lines, or 224 us. The first source sample follows broad-sync onset by
about 1549.64 us in one field and 1581.64 us in the other, versus 1325.64 us
and 1357.64 us in mode 0.

The broad-sync onset is a generated waveform reference, not a measurement
of when the monitor's internal sync separator actually triggers retrace.

Mode 7 has **no pre-equalising pulses**. It keeps five broad and five
post-equalising pulses and moves that shortened sequence one full line across
the frame boundary. This is a nonstandard compatibility experiment. A full,
stable screen on one CRT does not demonstrate standards compliance or correct
field interlace on other displays.

The changes add no picture rows, discard no source rows and add no vertical
overscan border. They alter the relationship between sync and the existing
picture. The observed improvement is consistent with retrace/blanking response
or raster phase being involved, but does not uniquely diagnose either.

With the original picture position, the final source row ends at sample 831
of line 623 and mode-7 broad sync starts at line 624: 65 sample periods
(4.643 us) remain. Advancing another half-line would overlap that last row.
This is why the current approach stops at 3.5 lines and modes 6/7 force
`pal-shift 0`.

## VGA impact review

Source comparison confirmed identical VGA timing and mode structures, VGA
frame selection, scanline renderer, pause service and the core-1 scheduling
loop. Capture C/PIO/header files and the composite PIO program are also
unchanged. VGA pin allocation, scanline-buffer count, SDK dependencies and
system clock configuration are unchanged by this work.

The firmware changes outside the composite modules add console controls,
diagnostic status fields and USB bootloader entry. The binary USB screen
encoder/header and captured-pixel format were not changed.

There is nevertheless a shared-resource path: core 1 generates both VGA and
composite scanlines, and both use DMA/SRAM resources. Additional composite
work could theoretically cause missed VGA deadlines even though VGA protocol
parameters are unchanged. The scheduler still prioritises available VGA work.
The test pattern is generated procedurally, without an additional framebuffer.

Read-only COM20 snapshots, approximately 20 seconds apart, with mode 7 and the
test pattern active showed:

| Counter | First | Second |
| --- | ---: | ---: |
| VGA frames | 10641 | 11881 |
| VGA scanline gaps | 0 | 0 |
| Composite generated fields | 8868 | 9902 |
| Composite DMA underruns | 0 | 0 |
| Captured source frames | 186 | 186 |
| Decoded source frames | 185 | 185 |

Capture reported `locked=no` in both snapshots. The device had previously
decoded 185 frames with a maximum decode time of 17317 us and no accumulated
VGA gaps, but this observation interval was not a sustained live-capture or
USB-streaming stress test. No VGA connector waveform was measured with a
scope. Conclusion: **VGA protocol configuration is unchanged, and no VGA
scheduling regression was observed; full-load/electrical validation remains
outstanding.**

USB bootloader entry at 1200 baud resets the entire adapter and temporarily
interrupts both outputs, just as a manual BOOTSEL/reflash does. Normal serial
operation at 115200 baud does not do this.

## Verification and follow-up

The host waveform test passes. It checks every sample of all 625 lines for
each of the eight sync modes and both requested picture positions, including
wrapped sync and the forced original position in modes 6/7. Additional tests
check source-row mapping and the diagnostic card. These verify the generated
digital waveform, not monitor response or analogue voltage/slew.

The next controlled comparison is normal sync versus mode 7 **with the monitor
controls now held fixed**. This determines whether the nonstandard sequence is
still needed after monitor adjustment. For a final timing decision, capture
both field transitions and the first/last picture lines at the RCA output,
including sync amplitude into the actual terminated input. Sustained P2000M
capture with VGA and composite enabled, followed by USB streaming, would
complete the shared-load regression check.

No output settings were changed during this review. Mode 7 remains running.
At the time of this review, diagnostic modes were volatile. A subsequent
persistence change adds version-five settings: `save` now retains sync mode and
picture shift across resets. Older saved records retain their other preferences
and use sync 0 / shift 1 until timing is explicitly saved. The test pattern
remains temporary and starts off at boot.

## Philips BM75xx specifications

The original Philips family manual covers BM7500, BM7502, BM7520, BM7522,
BM7542, BM7550 and BM7552. Its technical specifications (printed page 4,
PDF page 4) state:

| Parameter | Philips specification |
| --- | --- |
| Video input | Composite, negative synchronisation, 1 V peak-to-peak ±0.5 V, 75 ohms |
| Line frequency | 15,625 Hz ±600 Hz |
| Raster frequency | 50/60 Hz |
| Video bandwidth | Greater than 20 MHz |
| Tube | 12 inch; green, amber or white depending on type |
| Text capacity | 80 characters ×25 rows |

Source: [Philips BM75xx manual, original scan](https://ebsoft.fr/dms/www/op/op.Download.php?documentid=154&version=1).
The archive calls this a service manual; the PDF includes operating instructions,
the specifications and circuit drawings. The rear label is needed to confirm
the exact model and suffix; the photograph establishes amber phosphor but not
the full type number.

Our 15,625 Hz / 50 Hz output matches these nominal scanning frequencies.
PAL names a colour encoding system, so for this monochrome monitor composite
levels and synchronisation are the relevant compatibility requirements.
These specifications do not document exact vertical blanking/recovery limits,
and do not establish why the first picture lines were hidden. The working
advanced-sync mode is evidence of a useful compatibility adjustment, not proof
that the monitor is incompatible with standard 625/50 synchronisation.

The proposed 288-to-272-row fit experiment was removed before upload at the
user's request: it cannot preserve arbitrary source pixels. Full-height output
continues to transmit all 288 source rows without scaling. The waveform tests
and firmware build pass after removal; the device and saved settings were not
changed during this specification review.
