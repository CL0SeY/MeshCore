# RAK 1W Combined Repeater + USB Client — Interim Status Report

**Date:** 2026-07-26
**Phase:** Implementation (in progress)

## Objective
Implement the "RAK 1W Combined Repeater + USB Client" firmware per IMPLEMENTATION_PLAN.md, producing a flashable UF2 image for the RAK3401 (nRF52840 + SX1262).

## Changes Made (Verified via `git diff`)

### 1. `variants/rak3401/platformio.ini` (lines 137-158)
- Added `-DMESH_DEBUG=1` to `build_flags` of `[env:RAK_3401_repeater_companion_usb]`
- Replaced old `lib_deps` override (`densaugeo/base64 @ ~1.4.0`) with inherited `${rak3401.lib_deps}`
- `build_flags` structure: inherits `${rak3401.build_flags}` then appends all existing flags minus the broken `base64`-only override

### 2. `examples/simple_repeater/main.cpp`
- Added `#include <helpers/ArduinoSerialInterface.h>`
- Added `ArduinoSerialInterface serial_interface;` as global variable
- Removed `static char command[160]` (no longer needed without CLI loop)
- Added `serial_interface.begin(Serial)` and `the_mesh.startInterface(serial_interface)` in `setup()` after `the_mesh.begin(fs)`
- Removed Serial CLI read loop from `loop()` (the `while (Serial.available())` accumulation and `the_mesh.handleCommand()` call)
- Preserved: `sensors.loop()`, `rtc_clock.tick()`, power saving, `PIN_USER_BTN` block, `DISPLAY_CLASS` block

### 3. `examples/simple_repeater/MyMesh.h`
- Added `#include <helpers/BaseSerialInterface.h>`
- Added companion protocol command/response/error defines (complete set including CMD_SEND_RAW_PACKET, CMD_SET_FLOOD_SCOPE_KEY, RESP_CODE_CURR_TIME, RESP_CODE_DISABLED, etc.)
- Added `BaseSerialInterface* _serial;` private member
- Added `uint8_t cmd_frame[MAX_FRAME_SIZE + 1];` and `uint8_t out_frame[MAX_FRAME_SIZE + 1];` private members
- Added method declarations: `startInterface()`, `handleCmdFrame(size_t len)`, `checkSerialInterface()`, `writeOKFrame()`, `writeErrFrame(uint8_t)`, `writeDisabledFrame()`

### 4. `examples/simple_repeater/MyMesh.cpp` (+393 lines)
- Added `writeOKFrame()`, `writeErrFrame(uint8_t err_code)`, `writeDisabledFrame()` response frame helpers
- Added `startInterface(BaseSerialInterface &serial)` — sets `_serial` and calls `serial.enable()`
- Added `checkSerialInterface()` — calls `_serial->checkRecvFrame(cmd_frame)` then `handleCmdFrame(len)` if frame present
- Added `handleCmdFrame(size_t len)` (~390 lines) — companion protocol dispatcher implementing:
  - CMD_DEVICE_QUERY → RESP_CODE_DEVICE_INFO
  - CMD_APP_START → RESP_CODE_SELF_INFO (ADV_TYPE_REPEATER = 1)
  - CMD_SEND_TXT_MSG → ACL lookup + send, RESP_CODE_SENT or RESP_CODE_ERR
  - CMD_GET_CONTACTS → ACL iterate, RESP_CODE_CONTACT frames + RESP_CODE_END_OF_CONTACTS
  - CMD_SET_ADvert_NAME → update prefs, RESP_CODE_OK
  - CMD_RESET_PATH → ACL contact path reset, RESP_CODE_OK or RESP_CODE_ERR
  - CMD_GET_DEVICE_TIME → RESP_CODE_CURR_TIME
  - CMD_SET_DEVICE_TIME → set RTC, RESP_CODE_OK
  - CMD_SEND_SELF_ADVERT → createSelfAdvert() + sendFloodScoped(), RESP_CODE_OK
  - CMD_SET_RADIO_PARAMS → apply + save params, RESP_CODE_OK
  - CMD_SET_RADIO_TX_POWER → set + save TX power, RESP_CODE_OK
  - CMD_GET_STATS → RESP_CODE_STATS with RepeaterStats
  - CMD_SEND_RAW_DATA → create raw data packet, RESP_CODE_OK
  - CMD_SEND_RAW_PACKET → parse + send mesh packet, RESP_CODE_OK
  - CMD_SEND_CHANNEL_DATA → ERR_CODE_UNSUPPORTED_CMD
  - CMD_SET_DEFAULT_FLOOD_SCOPE → set scope key, RESP_CODE_OK
  - CMD_LOGIN → ACL authentication path
  - CMD_LOGOUT → stub, RESP_CODE_OK
  - CMD_HAS_CONNECTION → check ACL contact range
  - Catch-all unknown → ERR_CODE_UNSUPPORTED_CMD
- Added `checkSerialInterface()` call in `loop()` after `mesh::Mesh::loop()` with guard `if (_serial && _serial->isEnabled())`

## Build Status

### ✅ `RAK_3401_repeater` — SUCCESS
Builds cleanly with `pio run -e RAK_3401_repeater`. Produces UF2 via `create-uf2.py`.

### ❌ `RAK_3401_repeater_companion_usb` — FAILING
Fails with: `fatal error: Adafruit_LittleFS.h: No such file or directory`
File: `InternalFileSystem.cpp` (framework library, compiled as `libd2c/InternalFileSytem/InternalFileSystem.cpp.o`)

## Root Cause Analysis of Build Failure

PlatformIO's Library Dependency Finder (LDF) does not resolve transitive dependencies between framework libraries when `build_src_filter` is overridden in this env. Specifically:

- `InternalFileSystem` (a framework library in `framework-arduinoadafruitnrf52/libraries/InternalFileSytem`) has a `#include "Adafruit_LittleFS.h"` dependency
- `InternalFileSystem` is compiled because it is part of the framework for nRF52840
- `Adafruit_LittleFS/src` is NOT added to the include path for `InternalFileSystem.cpp` compilation because nothing in the project's source files triggers LDF to discover the `Adafruit_LittleFS` → `InternalFileSystem` dependency
- In contrast, the standard `RAK_3401_repeater` env also compiles `InternalFileSystem.cpp` but DOES have `Adafruit_LittleFS/src` in its include path — because other source files or library dependencies trigger LDF to include it

Key evidence from verbose build output:
- InternalFileSystem.cpp compilation: missing `-I.../Adafruit_LittleFS/src`, `-I.../SPI`, `-I.../Adafruit_TinyUSB_Arduino/src`
- CustomLFS_SPIFlash.cpp compilation (same env): HAS all three include paths
- This confirms an ordering issue in how PlatformIO processes framework library include paths per compilation unit

## Attempted Fixes (All Failed)
1. **Removed `lib_deps` override** — didn't help; InternalFileSystem still missing Adafruit_LittleFS include path
2. **Added `Adafruit_LittleFS` to `lib_deps`** — PlatformIO can't find it as a registry library; error persists
3. **Clean build cache** — same result after clean
4. **Inspected LDF dependency graph** — InternalFileSystem and Adafruit_LittleFS not in LDF graph when `build_src_filter` is overridden differently from working env

## Current `platformio.ini` State (lines 137-158)
```ini
[env:RAK_3401_repeater_companion_usb]
extends = rak3401
board_build.ldscript = boards/nrf52840_s140_v6_extrafs.ld
board_upload.maximum_size = 712704
build_flags = 
  ${rak3401.build_flags}
  -DMESH_DEBUG=1
  -D PIN_USER_BTN=9
  -D PIN_USER_BTN_ANA=31
  -D PIN_OLED_RESET=-1
  -UENV_INCLUDE_GPS
  -D MAX_CONTACTS=200
  -D MAX_GROUP_CHANNELS=20
  -D ADVERT_TYPE=1
  -D CLIENT_REPEAT_DEFAULT=1
  -I examples/simple_repeater
build_src_filter = 
  +<../examples/simple_repeater>
  +<../helpers/ArduinoSerialInterface.cpp>
lib_deps =
  ${rak3401.lib_deps}
  Adafruit_LittleFS
```

## What Must Still Be Done
1. **Fix the `Adafruit_LittleFS.h` build error** — most likely approaches:
   - Add `-I` path to `build_flags` pointing to `Adafruit_LittleFS/src` (not portable but works immediately)
   - Use a `lib_extra_dirs` or `lib_source_dirs` PlatformIO config option
   - Check if `${framework-arduinoadafruitnrf52}` provides a way to reference framework lib paths
   - Consider if a different approach to resolving the dependency exists (e.g., modifying `library.properties` of InternalFileSystem in the framework package)
2. **Build successfully** with `pio run -e RAK_3401_repeater_companion_usb`
3. **Generate UF2 image** using `create-uf2.py` (already configured as `extra_scripts`)
4. **Hand user the flashable image**

## Verification Status
- `RAK_3401_repeater` build: ✅ verified passing
- `RAK_3401_repeater_companion_usb` build: ❌ failing (include path issue)
- All source code changes: ✅ verified via `git diff`
- MyMesh.h defines match IMPLEMENTATION_PLAN.md command set: ✅
- MyMesh.cpp handleCmdFrame command handlers: ✅ (all MVP commands implemented)
- `loop()` includes `checkSerialInterface()` with guard: ✅
- `main.cpp` removes CLI loop, adds ArduinoSerialInterface: ✅
