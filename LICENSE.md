<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-License-Identifier: CC-BY-4.0
-->

# Licensing

Copyright 2026 Ivo Filot <ivo@ivofilot.nl>

This repository contains software, open-hardware design files, documentation,
and third-party material. They are licensed separately as follows.

## Software

The project-owned firmware and supporting software are licensed under the
**GNU General Public License, version 3 or (at your option) any later version**
(`GPL-3.0-or-later`). This covers:

- `src/`
- `screentest/`, including its source, build file, and generated cartridge image
- the top-level `CMakeLists.txt`
- `.github/workflows/`
- `.gitignore`

The complete license text is in
[`LICENSES/GPL-3.0-or-later.txt`](LICENSES/GPL-3.0-or-later.txt).

The two `cmake/pico_*_import.cmake` helpers are derived from Raspberry Pi
files and remain under `BSD-3-Clause`, as stated in their headers. The complete
license text is in
[`LICENSES/BSD-3-Clause.txt`](LICENSES/BSD-3-Clause.txt).

## Hardware

The hardware design is licensed under the **CERN Open Hardware Licence
Version 2—Strongly Reciprocal** (`CERN-OHL-S-2.0`). This covers:

- `pcb/`, including the editable KiCad project, custom footprints, PDF, and
  fabrication configuration
- `images/`, containing renders and other views derived from the design

The preferred source for modification is the KiCad project in `pcb/`. The
permanent source location is:

<https://github.com/ifilot/p2000m-video-to-vga-adapter>

The complete license text is in
[`LICENSES/CERN-OHL-S-2.0.txt`](LICENSES/CERN-OHL-S-2.0.txt).

## Documentation

`README.md`, `CHANGELOG.md`, `LICENSE.md`, `THIRD_PARTY_NOTICES.md`, and
`pcb/LICENSE.md` are licensed under **Creative Commons Attribution 4.0
International** (`CC-BY-4.0`), except for quoted third-party license notices,
which remain under their stated terms. The complete license text is in
[`LICENSES/CC-BY-4.0.txt`](LICENSES/CC-BY-4.0.txt).

## Machine-readable declarations

Text source files carry SPDX headers. File formats that cannot safely contain
comments, including KiCad and image files, are covered by `REUSE.toml`.
Third-party components retain their original copyright and license notices;
see [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
