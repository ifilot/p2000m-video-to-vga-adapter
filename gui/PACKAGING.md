<!--
SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
SPDX-License-Identifier: CC-BY-4.0
-->

# Desktop viewer packaging and releases

The viewer is released in four packages:

| Platform | Architecture | Package |
| --- | --- | --- |
| Windows | x86-64 | Graphical offline installer and portable ZIP |
| Linux | x86-64 | AppImage |
| macOS | Intel x86-64 | DMG |
| macOS | Apple silicon arm64 | DMG |

All packages contain a private FFmpeg 8.1.2 runtime with x264 H.264 support.
CI builds it from pinned authoritative source, validates both archives by
SHA-256, and performs a real MP4 encoding smoke test. Packages also contain the
exact FFmpeg and x264 source archives, license texts, checksums, configure
arguments, and runtime version report under `licenses/ffmpeg`.

The Windows installer is produced with Qt Installer Framework. It installs the
complete application under Program Files, creates Start-menu and desktop
shortcuts, displays the project, FFmpeg, and x264 license agreements, and
provides a maintenance tool for removal. The ZIP remains available for users
who specifically need a no-install portable copy.

## Continuous package validation

`.github/workflows/gui-packages.yml` builds, tests, installs, deploys, and
packages all four targets on every push to `master`, on pull requests, and on
manual dispatch. A default-branch push retains its packages for 30 days under
artifact names containing the exact commit SHA.

`.github/workflows/firmware.yml` independently builds the Pico firmware on
every push and pull request. Default-branch pushes retain the tested UF2 and
license files using the same exact-commit convention.

Protecting `master` with the firmware check and all four GUI matrix checks is
recommended. This catches platform-specific compilation and deployment
failures before merge, rather than waiting for a release tag.

## Tagging and release promotion

`.github/workflows/release.yml` is intentionally a promotion workflow, not a
build workflow. For a tag such as `v0.3.0`, it:

1. verifies that the tag matches both CMake project versions and `CHANGELOG.md`;
2. finds successful firmware and GUI default-branch runs for the exact tagged
   commit;
3. downloads the artifacts produced by those runs;
4. verifies every expected package, including both Windows formats; and
5. creates or updates one GitHub release containing firmware and all desktop
   packages.

The release is refused if the commit was not tested successfully on `master`,
if an artifact has expired, or if any version or package is inconsistent.
Consequently, tagging cannot silently publish a partial or newly rebuilt set
of binaries.

Release procedure:

```sh
git switch master
git pull --ff-only

# Confirm Firmware and all four GUI package jobs are green for HEAD.
git tag -a v0.3.0 -m "Release v0.3.0"
git push origin v0.3.0
```

If artifacts have expired, rerun the original successful `master` workflows
for that commit before retrying the tag workflow. Do not recreate a release by
building packages locally: the release workflow accepts CI artifacts only.

## Local Windows package

The release workflow uses Qt 6.8.3 with MSVC 2022. Local MSYS2 UCRT64 builds
remain supported:

```sh
pacman -S --needed \
  base-devel \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-nasm \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-qt6-base

sh gui/packaging/build_ffmpeg.sh \
  "$PWD/ffmpeg-runtime" "$PWD/ffmpeg-work"
cmake -S gui -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui
ctest --test-dir build-gui --output-on-failure
cmake --install build-gui --prefix stage
sh gui/packaging/bundle_ffmpeg.sh \
  "$PWD/ffmpeg-runtime" "$PWD/stage" windows
windeployqt6 --release --compiler-runtime --no-translations \
  stage/p2000m-vid2vga-viewer.exe
```

Archive the contents of `stage`, not the directory itself. To build the
graphical installer, install Qt Installer Framework, ensure `binarycreator` is
on `PATH`, and run:

```powershell
gui/packaging/create_windows_installer.ps1 `
  -Stage stage -Version 0.3.0 `
  -Output dist/p2000m-vid2vga-viewer-0.3.0-windows-x86_64-setup.exe `
  -WorkDirectory installer-work
```

The native Windows backend continues to use SetupAPI for Pico VID filtering
and direct COM I/O.

The Windows installer is currently unsigned because no Authenticode
certificate is configured for the repository. Windows SmartScreen may
therefore request confirmation on first launch. Signing should be added when
a code-signing certificate can be stored through protected GitHub Actions
secrets.

## Local Linux package

The normal build requires Qt 6.4 or newer. Install a C compiler, Make, curl,
pkg-config, and NASM before creating the bundled recording runtime:

```sh
sh gui/packaging/build_ffmpeg.sh \
  "$PWD/ffmpeg-runtime" "$PWD/ffmpeg-work"
cmake -S gui -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui
ctest --test-dir build-gui --output-on-failure
cmake --install build-gui --prefix AppDir/usr
sh gui/packaging/bundle_ffmpeg.sh \
  "$PWD/ffmpeg-runtime" "$PWD/AppDir" linux
```

The CI workflow then runs pinned `linuxdeploy` and Qt-plugin releases to create
the AppImage. Follow the commands in `gui-packages.yml` when reproducing the
exact release package locally. Linux discovers Pico CDC devices as
`/dev/ttyACM*`. The user must have read/write access, normally through the
distribution's serial-device group or an appropriate udev rule.

A typical Debian-family rule is:

```udev
SUBSYSTEM=="tty", ATTRS{idVendor}=="2e8a", MODE="0660", GROUP="dialout", TAG+="uaccess"
```

Install it under `/etc/udev/rules.d/` using the administrator procedure for
the target distribution. Group names differ on some distributions, notably
those using `uucp` instead of `dialout`.

## Local macOS package

Install Qt 6, Ninja, Make, curl, pkg-config, and NASM, then build FFmpeg and
generate the required bundle icon before CMake configuration:

```sh
sh gui/packaging/build_ffmpeg.sh \
  "$PWD/ffmpeg-runtime" "$PWD/ffmpeg-work"
sh gui/packaging/create_macos_icon.sh \
  gui/assets/p2000m-vid2vga-viewer-icon.png \
  build-assets/p2000m-vid2vga-viewer.icns

cmake -S gui -B build-gui -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
  -DP2000M_MACOS_ICON="$PWD/build-assets/p2000m-vid2vga-viewer.icns"
cmake --build build-gui
ctest --test-dir build-gui --output-on-failure
cmake --install build-gui --prefix stage
sh gui/packaging/bundle_ffmpeg.sh \
  "$PWD/ffmpeg-runtime" "$PWD/stage" macos
macdeployqt "stage/P2000M VID2VGA Viewer.app" -always-overwrite \
  -executable="stage/P2000M VID2VGA Viewer.app/Contents/Resources/tools/ffmpeg" \
  -dmg
```

macOS discovers Pico devices as `/dev/cu.usbmodem*`. The CI packages are
architecture-specific rather than universal, avoiding a falsely universal
bundle containing single-architecture Qt libraries.

The current DMGs are unsigned and not notarized because the repository has no
Apple Developer ID credentials. Gatekeeper may therefore require users to
approve the first launch explicitly. Signing and notarization should be added
only when a Developer ID certificate and App Store Connect credentials can be
stored as protected GitHub Actions secrets.

## Deployment and licensing details

- Windows uses `windeployqt`; macOS uses `macdeployqt`; Linux uses the actively
  maintained `linuxdeploy` project and its Qt plugin.
- Packages contain shared Qt libraries and the Qt license files supplied by
  the selected Qt distribution.
- Project license and third-party notice files are installed in every package.
- FFmpeg and x264 are built from the versions and hashes in `ffmpeg.env`.
  Updating either dependency requires updating its version/revision, archive
  URL, SHA-256, notices, and the bundled source archive as one reviewed change.
- The Windows installer presents all applicable GPL agreements before
  installation. Other formats place the same texts and exact corresponding
  sources beside the installed application.
- The release workflow publishes exactly the assets tested on `master`; it
  never substitutes binaries produced by the tag event.
