# AGENTS.md

## Project overview

This repository is a single-sketch **ESP32-S3 Arduino firmware** project: `dashboard.ino`.
It is "TC RADIOS", an internet-radio player for a **Freenove FNK0104AB-compatible** board
(2.8" ILI9341 touch display + ES8311 audio codec). It connects to Wi-Fi, fetches a playlist
JSON from GitHub, streams internet radio via I2S/ES8311, and renders a touch UI with LovyanGFX.

## Cursor Cloud specific instructions

This is embedded firmware — there is **no server/app to run** in the VM and no way to execute
it without the physical Freenove ESP32-S3 board (display, touch, ES8311 codec, speaker).
The meaningful build/lint/verification step is **compiling the sketch** with `arduino-cli`,
which produces a flashable ESP32-S3 image. A successful compile is the environment's "it works".

### Toolchain (installed in the VM snapshot; refreshed by the update script)
- `arduino-cli` (v1.5.1) in `/usr/local/bin`.
- ESP32 board core `esp32:esp32` (3.3.10) — provides `WiFi`, `HTTPClient`, `Wire`,
  `Preferences`, `ESP_I2S`.
- Arduino libraries (in `~/Arduino/libraries`): `LovyanGFX`, `ArduinoJson`,
  `ESP32-audioI2S-master` (provides `Audio.h`).

### `es8311` driver is a companion dependency NOT committed to this repo
`dashboard.ino` does `#include "es8311.h"` and calls `es8311_codec_init(void)`, but the
`es8311.h` / `es8311.cpp` / `es8311_reg.h` driver files are **not in this repo** — they ship
alongside the Freenove FNK0104 example sketches. They are installed as a local Arduino library
at `~/Arduino/libraries/es8311/` (fetched from the Freenove
`Freenove_ESP32_S3_Display` repo, `Tutorial_With_Touch/Sketches/Sketch_07.1_Music`).
The update script recreates this library if missing. If you ever edit codec behavior, that is
where `es8311_codec_init()` lives — not in the repo.

### How to build (non-obvious gotchas)
1. `arduino-cli` requires the `.ino` to sit in a folder whose name matches the file. This repo
   keeps `dashboard.ino` at the repo root, so you must copy it into a `dashboard/` folder first:
   ```
   mkdir -p /tmp/build/dashboard && cp dashboard.ino /tmp/build/dashboard/
   arduino-cli compile \
     --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=8M,PartitionScheme=huge_app" \
     /tmp/build/dashboard
   ```
2. `PartitionScheme=huge_app` is **required**: the firmware is ~2.1 MB and does not fit the
   default 1.3 MB app partition (you get "Sketch too big / text section exceeds available space").
3. Board = generic **ESP32S3 Dev Module** (`esp32:esp32:esp32s3`); the Freenove N8R8 module
   uses octal PSRAM (`PSRAM=opi`), 8 MB flash.

### Known harmless warning
Building with ArduinoJson 7 emits `'DynamicJsonDocument' is deprecated` because the sketch uses
the ArduinoJson 6 API. It still compiles and works; do not "fix" it unless asked.

### Other notes
- Default Wi-Fi credentials are hard-coded in `dashboard.ino` (`defaultSsid`/`defaultPassword`);
  at runtime they can be overridden via the on-device Wi-Fi setup UI (stored in `Preferences`).
- To flash to real hardware (not possible in this VM): `arduino-cli upload -p <port> --fqbn <fqbn> /tmp/build/dashboard`.
