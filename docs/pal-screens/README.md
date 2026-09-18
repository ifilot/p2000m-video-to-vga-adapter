# PAL monochrome designs

Three static designs drawn at **640×288, one bit per pixel**. No resizing,
row removal or greyscale is used by the firmware. The amber previews show
an approximate 4:3 CRT presentation; actual geometry depends on the monitor.

| Command | Design | Preview |
| --- | --- | --- |
| `pal-demo radar` | Deep Field: radar-inspired signal-loss concept | [Amber](radar-amber.png) |
| `pal-demo circuit` | Logic Core: chip and routed bus traces | [Amber](circuit-amber.png) |
| `pal-demo scope` | Signal Atlas: illustrative timing instrument | [Amber](scope-amber.png) |
| `pal-demo off` | Return PAL to normal captured video / signal-loss screen | |

Select a design, then run `save` to retain it across resets, for example:

```text
pal-demo radar
save
```

Use `pal-demo off` followed by `save` to restore normal output at boot.
`settings` reports `pal_demo=0|1|2|3` (off, radar, circuit, scope) and whether
there are unsaved changes. Older settings load with the demo off and retain
existing timing and display preferences. Factory reset restores demo off.
These commands do not alter timing, VGA, or the USB captured image. The selected artwork appears only while source sync is missing. Live P2000M
video takes priority automatically; the artwork returns when the signal is lost. `pal-test on|off` exits demo mode;
selecting a demo exits the calibration pattern. Exiting a demo with `pal-test`
marks the choice as modified; use `save` if that change should persist. Status reports demo IDs 0–3.
These are visual designs, not live radar, bus-state or waveform measurements.
The `off` choice uses the original plain signal-loss card.

Regenerate the native PNGs, previews and packed C header with:

```sh
python3 scripts/generate_pal_screens.py
```

The generator requires Pillow and uses the project's existing bitmap font.
The three frames occupy 69,120 bytes of flash; rendering reuses the normal
packed-pixel path without another RAM framebuffer or procedural scanline work.
