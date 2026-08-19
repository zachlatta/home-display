# Panel simulator

Renders the dashboard exactly as the reTerminal E1002 would, on a Mac, with no
device attached — then writes both a flat PNG and an approximation of how the
e-paper actually looks.

```sh
make -C sim fixtures   # build payloads using the real gateway code
make -C sim check      # render everything, fail on panel-level problems
make -C sim run        # render one fixture to sim/out/
```

Output lands in `sim/out/<name>-ideal.png` and `sim/out/<name>-panel.png`.

## Why it is trustworthy

The simulator does not re-implement the layout. `src/dashboard_view.h` holds the
payload parser and every drawing call, and is compiled by both targets:

| | firmware | simulator |
| --- | --- | --- |
| layout + parser | `src/dashboard_view.h` | same file |
| glyph rasteriser | Adafruit_GFX | same library, same version |
| fonts | `Fonts/FreeSans*` | same font data |
| JSON | ArduinoJson 7 | same library |
| colour quantisation | `GxEPD2_7C::color7` | transcribed in `sim/panel.h` |
| wire codes | `GDEP073E01::_convert_to_native` | transcribed in `sim/panel.h` |
| framebuffer | 12 pages × 40 rows | same paging |
| base64 | mbedtls | `sim/shim/base64.cpp` |

A layout reimplemented in JavaScript would render something plausible and miss
the truncation, clipping, and page-boundary bugs that matter. This renders the
real thing.

The 40-row paging is reproduced rather than flattened because the firmware draws
the entire layout twelve times, once per band, keeping only the rows that fall
inside the current page. Every render is also repeated as a single 480-row page
and the two frames compared; a mismatch means the layout depends on where page
boundaries land, and `make check` fails.

## The panel is Spectra 6, not 7-colour

`GDEP073E01` is an E Ink Spectra 6 panel: **black, white, yellow, red, green,
blue**. There is no orange. GxEPD2 drives it through the `GxEPD2_7C` base class
— a legacy name from the older 7-colour ACeP panels — and `GxEPD_ORANGE` maps to
native wire code `0x4`, which Spectra 6 does not define. Waveshare's driver for
the same panel has that constant commented out for exactly this reason.

The simulator counts any pixel that lands on an unsupported colour and fails the
check. Use `--flag-unsupported` to paint them magenta and see where they are.

## Colour accuracy

Good Display publishes no colorimetry for this panel; its datasheet's optical
characteristics table is an empty stub. The palette in `sim/appearance.cpp` is
the median of five independent calibrated measurements of this exact panel from
open-source projects, which agree closely on everything except black.

Three palettes are available via `--palette`:

- `measured` — as photographed. Dim, but it is what a camera sees.
- `adapted` — normalised so panel white maps to `#FFFFFF`, approximating what
  your eye accepts as white after looking at the panel for a moment.
- `blend` (default) — halfway between, which reads as e-paper on a bright
  monitor without making legibility judgements overly pessimistic.

The panel render also models the physical surface: a Gaussian pigment spread
that blends dithering the way the real microcups do, multiplicative surface
grain, and slow reflectance mottle. Tune with `--blur`, `--grain`, `--mottle`,
and `--ambient`. Contrast is roughly 30:1 on the real panel, which is why
`make check` warns about any colour below 3:1 against the background.

Calibrate against reality once hardware is available: photograph the panel in
even light, sample the six patches, and replace `kMeasured` in
`sim/appearance.cpp`.

## What still needs the device

SPI and the BUSY handshake, the 10–30 s refresh and its ghosting, deep sleep and
power draw, Wi-Fi and TLS, the provisioning AP, the GPIO 44/43 serial bridge,
and PSRAM allocation. The simulator covers what reaches the glass, not how it
gets there.

## Files

- `simulate.cpp` — CLI, cross-checks, and the report
- `panel.h` — GxEPD2-equivalent paged framebuffer and colour quantisation
- `appearance.cpp` — measured palettes and the optical model
- `png.cpp` — zlib PNG writer
- `shim/` — the slice of the Arduino core Adafruit_GFX needs
- `make-fixtures.mjs` — builds payloads through the real gateway code
- `check.mjs` — renders everything and enforces the failure conditions
- `fetch-deps.sh` — pins and vendors Adafruit_GFX and ArduinoJson
