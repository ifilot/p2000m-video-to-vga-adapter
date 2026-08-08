<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-FileCopyrightText: 2020 Raspberry Pi (Trading) Ltd.
SPDX-FileCopyrightText: 2018 hathach (tinyusb.org)
SPDX-License-Identifier: CC-BY-4.0 AND BSD-3-Clause AND MIT
-->

# Third-party notices

The firmware build downloads the pinned Raspberry Pi Pico SDK 2.3.0 and the
matching `pico-extras` release. The generated firmware incorporates portions of
those projects and TinyUSB. Those portions retain their original licenses and
copyright notices. The project's GPL license does not replace them.

The exact dependency source is available from:

- <https://github.com/raspberrypi/pico-sdk/tree/2.3.0>
- <https://github.com/raspberrypi/pico-extras/tree/sdk-2.3.0>
- <https://github.com/hathach/tinyusb/tree/86ad6e56c1700e85f1c5678607a762cfe3aa2f47>

Release packages of the optional desktop viewer also contain dynamically
linked Qt 6 runtime libraries and plugins. Those unmodified libraries retain
their Qt Project licensing, including the GNU Lesser General Public License
version 3 where applicable. Viewer packaging copies the license files supplied
with the exact Qt distribution into the package. Qt source releases and the
corresponding license information are available from <https://www.qt.io/>.

Viewer packages include a private FFmpeg 8.1.2 executable built from unmodified
FFmpeg source and statically linked with x264 revision
`b35605ace3ddf7c1a5d67a2eb553f034aef41d55`. The executable is a separate
process connected to the viewer through standard input. FFmpeg is configured
with `--enable-gpl --enable-version3 --enable-libx264`, so the resulting
runtime is distributed under GNU GPL version 3 or later. x264 is distributed
under GNU GPL version 2 or later.

Every viewer package contains the complete, exact FFmpeg and x264 source
archives used for that platform build, their license texts, SHA-256 checksums,
the full configure arguments, and the runtime's own version report under
`licenses/ffmpeg`. The authoritative upstream locations are:

- <https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz>
- <https://code.videolan.org/videolan/x264/-/commit/b35605ace3ddf7c1a5d67a2eb553f034aef41d55>

FFmpeg copyright © 2000–2026 the FFmpeg developers. x264 copyright © 2003–2025
the x264 project. No endorsement by either project is implied.

## Raspberry Pi Pico SDK and pico-extras

Copyright 2020 (c) 2020 Raspberry Pi (Trading) Ltd.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

License: `BSD-3-Clause`

## TinyUSB

Copyright (c) 2018, hathach (tinyusb.org)

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

License: `MIT`
