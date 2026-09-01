# Third-Party Notices

This file documents the licensing situation of external dependencies used by
the firmware in this repository. It is informational only and does not
constitute legal advice.

## Scope

All source code in this repository (`server/`, `firmware/src/`,
`firmware/include/`, `docs/`) is original work and is licensed under the MIT
License — see [`LICENSE`](LICENSE). None of the third-party code described
below is copied, vendored, or otherwise included in this repository. It is
resolved and downloaded separately by PlatformIO at build time, based on the
`lib_deps` entries in [`firmware/platformio.ini`](firmware/platformio.ini).

## `heltec-eink-modules`

Repository: https://github.com/todd-herbert/heltec-eink-modules

The firmware depends on this Arduino/PlatformIO library for E-Ink display
drawing (`EInkDisplay_WirelessPaperV1_2`, i.e. the `E0213A367` display
driver). It is pulled in exclusively via `lib_deps` and is not part of this
repository.

As of this writing, the `heltec-eink-modules` repository does not carry a
root `LICENSE`/`COPYING` file or a `license` field in `library.properties` /
`library.json`, and GitHub's own license detection reports no license for
it. The licensing terms of the wrapper project's own code (platform
detection, display driver glue code, etc.) are therefore not clearly stated.

That library in turn bundles further third-party code with their own,
differing licenses:

- **`GFX_Root`** (`src/GFX_Root/`, originally from
  [`ZinggJM/GFX_Root`](https://github.com/ZinggJM/GFX_Root), itself derived
  from Adafruit_GFX) — licensed under the **GNU General Public License v3.0
  (GPLv3)**. This is the drawing primitives library the firmware's display
  calls (`print()`, `drawRect()`, `fillRect()`, `setCursor()`,
  `setTextSize()`, etc.) resolve to at build time.
- **`SdFat`** (`src/SDWrapper/SdFat/`, from
  [`greiman/SdFat`](https://github.com/greiman/SdFat)) — licensed under the
  **MIT License**.

## Practical implications

- Building the firmware locally with PlatformIO and flashing it to your own
  device does not distribute anything to third parties.
- Distributing a compiled firmware **binary** (e.g. sharing a `.bin` file)
  would combine this repository's MIT-licensed code with the
  GPLv3-licensed `GFX_Root` component at the binary level. Anyone doing so
  should independently review the applicable GPLv3 obligations for that
  binary (such as source availability) before distributing it.
- **This repository does not publish or distribute any pre-compiled
  firmware binaries.** Only source code is provided; building and flashing
  is left to the user.
- Because `heltec-eink-modules` itself has no clearly stated license, using,
  modifying, or redistributing that library's own code beyond the scope of
  a normal PlatformIO build dependency should be clarified directly with
  its author before relying on it in contexts with stricter licensing
  requirements (e.g. commercial distribution).

## Other build dependencies

- [`ArduinoJson`](https://arduinojson.org/) (`bblanchon/ArduinoJson`) —
  MIT License.
- Arduino ESP32 core (WiFi, HTTPClient) — part of the
  `framework = arduino` toolchain for `espressif32`, not redistributed by
  this repository.
