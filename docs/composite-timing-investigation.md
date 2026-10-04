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

## Stability investigation, 2026-10-04

The user reports subtle apparent frame-to-frame movement; the monitor was
stable on a signal generator. The connected Pico 2 was found on Windows COM20.
Initial status showed both outputs enabled, saved `pal-sync 7`, `pal-shift 0`,
and radar fallback artwork. PAL DMA underruns and VGA scanline gaps were zero.
Capture was not locked during inspection, so the observed output was static
artwork rather than live P2000M capture.

The first test firmware keeps the existing waveform, field structure, picture
position and saved settings. It gives the two PAL DMA channels high priority
in the DMA scheduler, primes the PIO transmit FIFO before enabling output,
and restarts the clock divider at stream startup. It also exposes
`pio_stall_observations` in `status`: observations of the hardware's sticky
TXSTALL flag, cleared by the output service loop. Multiple stalls between
polls can coalesce. This detects brief empty-FIFO stalls that stretch output
timing without necessarily exhausting both DMA buffers. Startup priming avoids
counting the deliberate wait for the first DMA data as a runtime stall.

The host PAL waveform test and firmware build passed. The test firmware was
uploaded through the USB 1200-baud reset and RP2350 UF2 volume. Two status
snapshots approximately 20 seconds apart showed:

| Counter | First | Second |
| --- | ---: | ---: |
| PAL fields | 830 | 1867 |
| PAL DMA underruns | 0 | 0 |
| PAL PIO stall observations | 0 | 0 |
| VGA frames | 995 | 2239 |
| VGA scanline gaps | 0 | 0 |
| Captured source frames | 0 | 0 |

This verifies continuous operation with both outputs and static artwork,
not live-capture/USB-streaming load or an improvement in monitor appearance.
No settings were saved or changed. A copy of the pre-change local UF2 is at
`build/pal-investigation/baseline-v0.6.0.uf2`; the flashed test UF2 is at
`build/src/p2000m-vid2vga-firmware.uf2`.

Pending visual feedback, compare normal sync (`pal-sync 0`) against the saved
mode 7 with the monitor controls held fixed. Mode 7 omits pre-equalisation;
its success in restoring coverage does not establish equally stable field
placement. The two fields also repeat the same 288-row source image on an
interlaced raster, so static fine edges can exhibit interlace twitter. A static
test pattern separates those effects from input sampling phase changes.
Neither possible cause has been established by the digital counters.

The user subsequently enabled the P2000M and reported that stuttering remained.
At their request, standard sync was restored with `pal-sync 0` over COM20;
`pal-shift 0` and both enabled outputs were retained. This change is in RAM,
not saved to flash. During an approximately ten-second live-input observation,
capture advanced from 3252 to 3795 frames and remained locked at approximately
50.095 Hz. PAL fields advanced from 5748 to 6290 with zero DMA underruns and
zero PIO stall observations; VGA frames advanced from 6897 to 7547 with zero
scanline gaps. Monitor feedback on stability and missing rows with standard
sync is pending before changing picture placement.

The user reports unchanged oscillation with normal sync, missing top lines,
and predominantly vertical movement resembling an odd/even effect. The next
trial adds temporary `pal-interlace off`: two identical-phase 312-line rasters
per 624-line pair, approximately 50.080 scans/s. Standard sync retains all
five pre-equalising, five broad and five post-equalising pulses. The same 288
source rows are shown one-to-one on each scan. The 14 MHz sample clock,
64 us scanlines, picture origin and VGA configuration are retained. This is
experimental progressive monochrome composite timing, not standard 625/50.
It tests interlaced placement as the cause; visual improvement is not yet
established. `pal-interlace on` restores interlace at a pair boundary. The
diagnostic is not persisted, and reboot restores interlace.

Tests verify the existing interlaced waveform and progressive phase identity,
sync samples and every source pixel across all eight sync settings and both
picture positions. The firmware build passed and the new UF2 was flashed;
`pal-sync 0` and `pal-interlace off` were then applied without saving settings.
Both outputs remain enabled. Over approximately ten seconds of live capture,
PAL scans advanced from 789 to 1326, captured frames from 789 to 1326 and VGA
frames from 946 to 1589. Capture stayed locked; PAL DMA underruns, PIO stall
observations and VGA gaps all remained zero. Visual feedback is pending.

The user reports reduced stuttering with progressive output. This supports
field placement as a contributor, but does not establish the cause of any
remaining movement. `pal-test on` was then enabled to compare a static
calibration pattern while live capture and VGA continue. Progressive timing,
normal sync and picture position remain unchanged. At 5988 PAL scans and 7175
VGA frames, stall/underrun/gap counters were still zero and capture was locked.
The calibration command temporarily clears the radar fallback selection;
restore `pal-demo radar` after this comparison to return to live video and
the original fallback artwork. No settings have been saved.

The user reports a quite steady progressive calibration pattern but missing
top scanlines. `pal-shift 1` was applied as a one-scanline downward positioning
comparison, retaining all 288 rows on lines 24--311 of each 312-line raster.
At 11215 PAL scans, output stalls/underruns and VGA gaps remained zero. The
test pattern is still enabled; feedback on the top border and ROW 00 is pending.

The progressive rate follows directly from keeping 64 us line timing:
`1 / (312 * 64 us) = 50.080128... Hz`. Standard interlace gets exactly 50 Hz
with 312.5 lines per field. Exactly 50 Hz with a fixed 312-line progressive
raster would instead require approximately 64.102564 us scanlines (15.600 kHz
horizontal rate). The current trial preserves the 15.625 kHz horizontal rate
and integer sample clock; it is not claimed to have exactly 50 Hz refresh.

The user reports that the bottom remains complete with `pal-shift 1`, but
the top still does not expose all lines. A temporary `pal-top 0..8` progressive
blanking diagnostic was added. It extends every raster by the requested number
of whole lines and moves the picture by the same amount, retaining every source
row. Full standard vertical pulse counts are preserved with `pal-sync 0`.
The option is ignored in interlaced mode and is not saved to flash.

Waveform tests pass for 0, 1, 3 and 8 extra lines, all sync modes and both picture
positions. They check scan phase identity, every sync sample and every source
pixel, and verify that extra blanking does not alter interlaced output. The
firmware build passed and the UF2 was flashed. The running trial is progressive
with `pal-sync 0`, `pal-shift 1`, `pal-top 3` and the calibration pattern enabled.
It uses 315 lines at 49.603 Hz, with 192 us more sync-to-picture time than the
previous test. Picture rows occupy lines 27--314 of each scan. Both outputs
are enabled; all 288 source rows remain in the digital waveform.

During approximately ten seconds, PAL scans advanced from 1147 to 1679 and
VGA frames from 1376 to 2019. Live capture stayed locked; PAL underruns, PIO
stall observations and VGA gaps stayed zero. Monitor feedback on top coverage,
bottom coverage and stability is pending. `pal-top 0` restores the 312-line
trial without flashing. No settings have been saved.

The user again reports a complete bottom but incomplete top. `pal-top 6` was
applied for the next comparison: 318 lines at 49.135 Hz, with source rows on
lines 30--317 and the calibration pattern still enabled. Full sync and
progressive phase are retained. At 3761 PAL scans and 4538 VGA frames, capture
was locked and all stall/underrun/gap counters remained zero. The waveform
test also covers six extra lines. Feedback on the first visible numbered row
and whether the top edge changes is needed to distinguish retrace recovery
from picture geometry. No settings have been saved.

The user reports that the six-line blanking trial exposes the complete screen
and looks very stable. This establishes a successful calibration-pattern
configuration on this monitor: progressive 318-line scans at 49.135 Hz,
`pal-sync 0`, `pal-shift 1`, `pal-top 6`, with VGA enabled. It does not establish
standard 625/50 compatibility or confirm the remaining live-input behaviour.

Live P2000M video was restored using `pal-demo radar`, which disables the test
pattern and restores the original fallback artwork. The successful timing was
retained. At 13163 PAL scans and 16019 VGA frames, input was locked and all
stall/underrun/gap counters were still zero. Live-picture feedback is pending
before making progressive mode and extra blanking persistable. Those two
diagnostics currently reset on reboot; no settings have been saved.

On live video the user subsequently reports horizontal expansion/contraction
("breathing"), with minimal flicker. At 21336 PAL scans the active output was
still 318 lines at 49.135 Hz and native 14 MHz sampling; capture was locked
and stall/underrun/gap counters were zero. Beating with a 50 Hz disturbance is
a hypothesis, not an established cause. No analogue waveform has been measured.

A reversible `pal-rate native|50` progressive-clock diagnostic was added.
Near-50 mode uses the nearest 16.8 PIO divider for 50 scans/s at the requested
line count. For 318 lines and nominal 252 MHz system clock, divider 17.6875
produces calculated 50.003334 Hz refresh, 14.247350 MHz sampling and
15.901060 kHz horizontal sync. Timing in microseconds, including sync widths
and horizontal picture width, scales with the clock. This retains the full
318-line layout and every source row, but is experimental nonstandard timing.
The fractional divider is not a lock to mains or to the source oscillator.
VGA and capture clocks are unchanged. A divider change restarts PAL once with
primed FIFO and fresh buffers. Interlaced mode always uses native timing.

The waveform test includes native divider verification and near-50 calculations
for all progressive line counts from 312 to 320. The full waveform test and
firmware build passed. The UF2 was flashed and `pal-rate 50` enabled on live
video with `pal-sync 0`, `pal-shift 1`, `pal-top 6`, radar fallback and VGA on.
During approximately ten seconds, PAL scans advanced from 827 to 1363, VGA
frames from 992 to 1635 and captured frames from 827 to 1364. Input remained
locked; PAL underruns/stall observations and VGA gaps stayed zero. `status`
reports calculated 50003 millihertz. Feedback on breathing and edge coverage
is pending. All progressive diagnostics remain temporary; nothing was saved.

The user confirms that breathing disappeared with the near-50 Hz trial, but
reports a tiny hint of flicker. The rate change is a successful observed
remedy on this setup; beating with a 50 Hz disturbance remains an inference.
For this divider the calculated line period is 62.888889 us (previously 64 us)
and each raster lasts 19.998667 ms. The six extra blank lines, progressive
placement and all 288 source rows are retained.

The static calibration pattern was enabled again without changing timing to
separate capture-dependent variation from output/display flicker. At 6811
PAL scans and 8172 VGA frames, capture was locked and all output fault counters
remained zero. Pattern feedback is pending. If flicker persists in the static
pattern, a temporary VGA-off comparison can isolate shared load/interference;
if the pattern is steady, investigate captured-frame variation. Radar fallback
was temporarily cleared by the test command and should be restored with
`pal-demo radar` afterwards. No settings have been saved.

The user reports a steady calibration screen and attributes the earlier
flickering to interlace handling. The successful raster has interlace disabled;
this observation supports the progressive configuration rather than proving
a particular defect in standards-compliant interlaced sync. Live video was
restored with `pal-demo radar`, preserving `pal-rate 50`, `pal-top 6`,
`pal-shift 1`, standard sync and VGA. At 12320 PAL scans and 14782 VGA frames,
capture was locked and all output fault counters remained zero. Confirmation
of live-picture steadiness is pending. Progressive settings remain volatile.

The user clarifies that flicker remains despite stable geometry, describing
moving patches most apparent in brightly illuminated areas. A VGA-off
comparison was attempted with PAL timing unchanged. The USB serial operation
stalled and no state acknowledgement was received. A bounded-timeout retry
also stalled; only the agent's own serial-reader processes were terminated to
release the port. Windows still lists the Pico on COM20 with device status OK,
but the 1200-baud reset attempt returned access denied. No other viewer/terminal
process was identified by the limited process-name check. USB reconnection or
device reset is needed before the comparison can be verified. VGA-off must
not be recorded as successfully applied, and no visual result is established.

During read-only inspection, the pinned pico-extras `scanvideo_timing_enable`
was found to call `pio_claim_sm_mask` on each timing-state change. The SDK claim
function asserts if a state machine is already claimed. This is a separate
VGA-toggle correctness concern to review before another toggle; it does not
establish the reason the initial status operation stalled. No workaround has
been applied or tested. No settings have been saved.

The user further describes waves from top-left to bottom-right. This makes
interference worth checking, but wave direction alone does not establish its
source. A further COM20 status attempt stalled and its reader was terminated;
no bootloader drive was mounted. Physical USB reconnection is pending.

A repository-local workaround for the confirmed repeated-claim issue was
prepared in `set_vga_timing_enabled`: release scanvideo's software claims on
PIO0 SM0/SM3 immediately before the pinned library reclaims them on a state
change. A compile-time assertion restricts this workaround to the one-plane
configuration; only scanvideo owns PIO0 and no PIO allocation occurs at runtime.
Capture and PAL use different PIO blocks. The firmware build and diff check
pass. The updated UF2 has not been flashed and hardware VGA toggles have not
been validated. Recovery, restoration of the working PAL settings, and a
verified VGA-off comparison remain outstanding.

After the user replugged USB, COM20 responded again. The old saved interlaced
mode 7 had returned, confirming that diagnostic options were volatile. The
built VGA-toggle workaround UF2 was flashed through the USB bootloader, and
the working PAL settings were reapplied (`pal-sync 0`, `pal-shift 1`,
`pal-top 6`, `pal-interlace off`, `pal-rate 50`, radar fallback, live picture).
`vga off` then completed and the console remained responsive. This validates
the disable path of the workaround; the resume path has not yet been tested.

Two subsequent VGA-off snapshots showed PAL scans advancing from 2720 to
3769 and captured frames from 2723 to 3774, while VGA frames stayed at 2630.
Input remained locked and PAL stall/underrun counters stayed zero. PAL remained
318-line progressive at calculated 50.003 Hz and 14.247349 MHz samples. The
VGA-off visual comparison is now active; feedback on diagonal flicker is
pending. Nothing has been saved to flash.

The user reports vastly worse flicker during the VGA-off comparison. Interlace
was disabled throughout, so this comparison does not identify interlacing as
the cause of the residual flicker. `vga on` was applied to reverse the trial;
the console acknowledged both outputs enabled while PAL retained 318-line
progressive timing, standard sync, six extra blank lines and near-50 Hz clock.
This also validates the resume path of the PIO-claim workaround. At 29590 PAL
scans and 2661 resumed VGA frames, capture was locked and all fault counters
were zero. Feedback on return to the earlier lower-flicker appearance is
pending. No settings were saved.

At the user's request the interlace implementation was audited again against
ITU-R BT.470-6 Figure 2 and Table 1-2. Normal sync has 312.5-line field spacing,
five pre-equalising, five broad and five post-equalising pulses. No new
confirmed sync-phase or source-row off-by-one defect was identified. Both
fields intentionally repeat the same 288-row geometry; they are not alternate
rows of a separately captured 576-row image.

One temporal difference is testable: the provider currently selects a new
captured image independently for each field. BT.470 Figure 2-1 Note 4 places
picture-material changes at the first field. A temporary `pal-pair on|off`
diagnostic now optionally retains one immutable source framebuffer through
both fields/scans. Its provider hold is released only at the next first field.
It reduces source image updates to approximately 25/s, while scanout continues
approximately 50/s. It does not alter sync or picture geometry and defaults
off after reboot. This tests field-pair temporal differences; it is not proof
of the cause of the reported flicker.

The host waveform tests pass. The old build's uppercase cached paths became
read-only under the current lowercase workspace permission root, so a fresh
offline build was configured at `build/pal-audit` with the existing SDK,
extras and picotool. The build passed and
`build/pal-audit/src/p2000m-vid2vga-firmware.uf2` was flashed.

The current trial retains the successful progressive 318-line near-50 Hz
geometry, full normal sync and VGA enabled, and adds `pal-pair on`. This
isolates temporal pairing before a separate interlaced-raster comparison.
Across approximately ten seconds, PAL scans increased from 1326 to 1862;
source swaps increased from 1295 to 1562 and repeats from 28 to 297, verifying
roughly one new source per two scans. Capture stayed locked, all PAL stall/
underrun and VGA gap counters remained zero, and refresh remained calculated
50.003 Hz. Visual flicker feedback is pending. No settings were saved.

The user reports unchanged flicker with frame pairing and requests an
interlaced comparison. `pal-interlace on` was applied over COM20. Output now
uses 625/50 at 14 MHz, standard sync and `pal-shift 1`; VGA and frame pairing
remain enabled, with live capture locked. Extra top blanking and near-50 clock
options remain requested but are ignored in interlaced mode. Top clipping may
therefore return. At 12250 PAL fields all output fault counters remained zero.
Visual comparison is pending. `pal-interlace off` restores the prior progressive
geometry and clock options. No settings were saved.

The user reports unchanged flicker in interlaced mode, with top clipping
returning. They authorised a static white-area test. The calibration pattern
now includes a uniform 320 x 144 white panel at source x=128..447, y=72..215,
and fixed two-white/two-black pixel stripes at x=480..543 beside it. The panel
replaces the middle text rows; edge borders/rulers, top/bottom text and inset
guides remain. Host tests verify the panel's uniformity, stripe samples and
identical pattern in both fields, alongside the existing waveform tests.

The host tests and firmware build passed; the new UF2 from `build/pal-audit`
was flashed. Full-screen progressive timing was restored: standard sync,
`pal-shift 1`, `pal-top 6`, `pal-rate 50`, pairing on and VGA on. `pal-test on`
is active, temporarily clearing the radar fallback in RAM. During approximately
ten seconds PAL scans advanced from 979 to 1516 and VGA frames from 1173 to
1817; input remained locked and all fault counters remained zero. Output
remains calculated 50.003 Hz. Feedback on brightness waves inside the solid
panel versus shimmer at stripe edges is pending. No settings were saved.

The user reports very noticeable flicker, brightness waves and small black
streaks inside the uniform white panel. This excludes captured-pixel variation
as a necessary cause: the panel is generated directly from fixed DAC codes
and does not read the captured framebuffer. Interlacing is disabled during
this observation. The generated output or analogue signal path remains to
be investigated; clean counters do not establish electrical correctness.

At 12270 PAL scans and 14721 VGA frames, the pattern remained active at
calculated 50.003 Hz with pairing and VGA on, and all PAL underrun/stall and
VGA gap counters remained zero. The documented 660-ohm/220-ohm DAC yields
ideal levels of roughly 0 V sync, 0.258 V black and 1.03 V white into 75 ohms;
actual loaded GPIO voltage, supply ripple and waveform have not been measured.

The next physical isolation comparison is to disconnect only the P2000M
video-input cable while retaining USB power, composite connection and the
static pattern. This removes input switching and the source's signal-ground
connection without changing generated PAL artwork. If artifacts change, input
coupling/grounding or load becomes a candidate, not a uniquely diagnosed cause.
If available, an oscilloscope measurement at the terminated RCA input and the
adapter's 3.3 V rail would distinguish voltage disturbances from timing errors.
No firmware or saved settings were changed during this review.

The user reports unchanged white-panel waves/streaks after disconnecting the
P2000M video-input cable. Input switching and that source-ground connection
are therefore not necessary to reproduce this observation. The USB power/
ground and composite path remain connected; the result does not rule them out.
Further timing changes are deferred pending analogue measurement or a
controlled USB power/ground isolation test. The existing progressive pattern
and timing remain active. Diagnostic settings are volatile, so powering from
a separate source requires a boot configuration that restores this pattern
before that comparison can be valid. No settings were saved.

The user reports both power and signal output appear fine and requests that
investigation focus on timing. A controlled integer-clock comparison is now
active: `pal-rate native`, retaining the static white panel, progressive
318-line raster, shift 1, six extra top lines, pairing and VGA on. This replaces
the 17.6875 fractional PIO divider with integer divider 18, giving 14 MHz
samples, 64 us lines and calculated 49.135 Hz scans. Picture layout is unchanged.
At 51316 PAL scans, underruns and PIO stall observations remained zero; VGA
scanline gaps also remained zero. Feedback on white-panel waves and black
streaks is pending. Horizontal breathing may return at the lower scan rate;
that should be assessed separately from the panel artifacts. No firmware or
saved settings were changed for this comparison.

The user reports unchanged brightness waves and streaks with the integer
divider. The near-50 Hz clock was restored over COM20. Status confirmed the
318-line progressive test raster at calculated 50.003 Hz, with VGA enabled
and zero PAL underruns/stall observations and VGA scanline gaps. This result
weakens fractional-divider jitter as the explanation; it does not validate
all generated sync timings. No further sync trial was applied before the
user requested a critical schematic review.

Schematic review used the current KiCad schematic, PDF and PCB. The README
preview PNG is older and omits the composite output. The current files agree
on R45/R44/R43 in series (660 ohms) from COMP0/GPIO14, R46 (220 ohms) from
COMP1/GPIO15 and the joined output at J7. RCA shield is on GND; PCB Pico ground
pads including 18 are on GND, with an F.Cu ground fill. There is no composite
buffer or deliberate output filter. Firmware sets both pins to fast slew and
12 mA drive, and updates them together using one PIO OUT instruction.

With ideal 3.3 V GPIO outputs and a 75-ohm receiver, computed levels are
0 V sync, 0.257813 V black and 1.031250 V white (0.773438 V black-to-white).
Thus sync depth is about 14% below nominal 0.3 V and luminance span about
10% above nominal 0.7 V. The ideal source impedance is 660 || 220 = 165 ohms,
plus GPIO output resistance, rather than a matched 75-ohm source. This reduces
absorption of any reflections returning from receiver/cable discontinuities;
an ideally matched receiver does not reflect solely because the source is
mismatched. White-state currents are approximately 3.44 and 10.31 mA, so the
ideal calculation must not be substituted for measured loaded GPIO levels.

These are concrete output-stage limitations, not proof of the reported
moving waves. A constant supply can coexist with imperfect sync-edge shape
or level. A slower GPIO slew comparison would test edge sensitivity without
changing sample clock, artwork or nominal DAC codes. A future hardware
revision could use a video buffer and matched output with corrected sync/
luminance proportions; adding a 75-ohm series resistor to this existing DAC
would attenuate the levels and is not a complete fix. TI SLOA057A, sections
2.4 and 3.1, documents nominal PAL luminance span and cable-driver matching:
https://www.ti.com.cn/cn/lit/pdf/sloa057 . No hardware files were modified.

At the user's request, both composite pins were changed from fast to slow
GPIO slew, retaining 12 mA drive strength. This is the only firmware change
in this trial; DAC codes, PIO program, divider calculation and sync renderer
are unchanged. The Release build and existing host waveform tests passed,
as did `git diff --check`. The UF2 from `build/pal-audit` was flashed through
the Pico's RP2350 boot volume, and COM20 returned.

The previous white-panel comparison settings were restored: normal sync,
shift 1, six extra top lines, progressive, near-50 Hz, pair hold, test pattern
and VGA enabled. Two status samples showed PAL scans increasing from 893 to
1074 and VGA frames from 1070 to 1287, with zero PAL underruns/stall
observations and VGA scanline gaps. Input remained absent, as in the preceding
comparison. Calculated scan rate remained 50.003 Hz. Visual feedback on the
waves/streaks is pending. No settings were saved; slow slew is compiled into
this experimental firmware but the comparison settings remain volatile.

The user found the slow-slew result hard to distinguish and approximately
unchanged. This is inconclusive rather than evidence of an improvement.
Slow slew remains fixed for the next single-variable comparison: `pal-sync 5`
was applied over COM20. This existing diagnostic omits the five pre-equalising
pulses and starts the five broad pulses 2.5 lines earlier; the five
post-equalising pulses remain. It is a nonstandard vertical-sync experiment,
not a proposed permanent PAL correction. Progressive 318-line timing,
calculated 50.003 Hz, shift 1, six extra top lines, white panel, pair hold and
VGA remain unchanged. A vertical position change is possible even though
source-row coordinates are unchanged. Status at 8786 PAL scans showed zero
underruns/stall observations and VGA scanline gaps. Feedback on waves/streaks
is pending. No firmware was flashed and no settings were saved for this trial.

The user reports approximately unchanged artifacts with sync mode 5. Normal
sync (`pal-sync 0`) was restored before preparing the next diagnostic.

The next build removes per-line PAL preparation from the steady-state path.
`P2000M_PAL_STATIC_RASTER_DIAGNOSTIC=ON` allocates a 71,232-byte SRAM raster
and reduces raw capture buffering from three buffers to two to fit it. This
option defaults OFF; the regular build remains available in `build/pal-audit`.
The diagnostic build in `build/pal-static` has 440,564 bytes BSS. When test
pattern, progressive mode and six extra top lines are selected, the complete
318-line waveform is generated once using the existing renderer. Each chained
DMA channel then reads the same immutable raster. CPU service rearms each
completed channel once per scan; it does not write its pixel/sync data. Thus
this is not completely CPU-independent DMA looping, but removes the previous
two-line buffer refill deadline and all ongoing PAL waveform construction.
Configuration changes stop the stream before regenerating the fixed raster.

Both build variants and the host waveform tests passed. A new test verifies
word-for-word equality of all 318 first/second-scan test-pattern lines, so
repeating the first raster preserves the previous displayed pattern and sync.
`git diff --check` also passed. The diagnostic UF2 was flashed over USB and
the previous settings restored: normal sync, shift 1, six extra top lines,
progressive near-50 Hz, pair hold, white-panel test and VGA on. Slow slew and
12 mA drive remain fixed. The source input remains absent.

Status confirmed `static_raster=on` and calculated 50.003 Hz. PAL scans rose
from 955 to 1136 and VGA frames from 1145 to 1361. PAL underruns and stall
observations were zero. VGA recorded one gap during the switch/precomputation,
with no increase between the two steady-state samples. The source-frame
counters do not advance in the immutable-raster path. Feedback on waves and
streaks is pending; no settings were saved.

The user reports exactly unchanged artifacts with the immutable-raster DMA
diagnostic. Ongoing scanline preparation is therefore not necessary for this
observation. This does not uniquely distinguish waveform synthesis from GPIO/
DAC, cable or monitor response. The user then requested that the current
configuration be locked as the default and uploaded to the Pico.

The static-raster build option now defaults ON. A boot profile applies normal
sync, shift 1, six extra top lines, progressive near-50 Hz output, pair hold
and the white-panel test after legacy settings are loaded. Both outputs start
enabled. Thus older saved PAL sync/output flags cannot undo the requested
profile. Factory reset applies the same profile. Slow slew remains compiled
in. Selecting the build option OFF retains these timing defaults but boots
live composite video with three raw capture buffers instead of two. Runtime
commands remain available, with reboot returning to the compiled profile.
Saved display colours and styles continue to load; settings flash was not
erased or rewritten for this change.

Both build variants, waveform tests and `git diff --check` passed. The locked
default UF2 was flashed and the device rebooted. Only read-only `status` and
`settings` commands were sent afterwards: the Pico confirmed progressive
318-line output at calculated 50.003 Hz, test on, shift 1, sync 0, six extra
lines, pair hold on, static raster on and both outputs enabled without setup
commands. PAL scans rose from 1287 to 1473 and VGA frames from 1544 to 1768,
with zero PAL underruns/stall observations and VGA scanline gaps. Input was
absent. The residual waves/streaks have not been fixed by locking defaults.

An exact copy of the uploaded UF2 is at
`build/pal-investigation/locked-defaults.uf2`, SHA-256
`1dec9f6e3b60a7bdc8111f4580e18f674061ddf07bd7cde32b22c393c46fc1c5`.

The user clarified that only the current non-interlaced timing should become
the default, with live P2000M capture and the existing no-signal artwork.
The preceding white-panel default was an incorrect interpretation and is
superseded. The static-raster CMake option now defaults OFF again, restoring
three raw capture buffers. Boot timing remains progressive 318 lines near
50.003 Hz, normal sync, shift 1, six extra top blanking lines, pair hold and
slow slew. The startup profile disables test output while preserving saved
fallback artwork; radar is the factory fallback. Saved output enables and
display styles are retained instead of being overridden. Factory reset also
restores radar. No saved-settings flash was erased or rewritten.

The live-video build and waveform tests passed, along with `git diff --check`.
Its UF2 was uploaded and boot verified using only read-only commands. Initial
status showed test off, static raster off, radar fallback (`demo=1`), both
outputs on and the intended timing. The next sample detected the P2000M:
23 capture frames, input locked at 50.095 Hz, 22 decoded frames, and source
swaps on both outputs. PAL scans advanced from 979 to 1165 and VGA frames
from 1174 to 1396; PAL underruns/stall observations and VGA gaps remained
zero. Thus live input was verified after the correction, not only configured.

The corrected uploaded artifact is
`build/pal-investigation/progressive-live-defaults.uf2`, SHA-256
`e8e9e8544c9144d0a67bd597d6409f2d4a057c5b5b298c9895e2924bff561bc7`.
The earlier `locked-defaults.uf2` is the superseded white-panel diagnostic.

On the user's repeated report of missing input, two further read-only checks
confirmed that the corrected firmware remained active: test and static raster
off, radar fallback, progressive 318 lines near 50.003 Hz, both outputs on.
The first sample had 2337 captured frames but input lock was absent; the next
had 2898 frames and lock restored, with 2881 decoded frames and PAL source
swaps advancing. No configuration commands or firmware changes were made
between these observations. This establishes an input interruption/recovery
as observed by capture, without identifying its physical or firmware cause.
All PAL stall/underrun and VGA gap counters remained zero.
