## Plan: Heltec V4 RS232 Bridge Support

### TL;DR
Add a new build environment in `variants/heltec_v4/platformio.ini` that mirrors the existing `Heltec_v3_repeater_bridge_rs232` and `Xiao_S3_WIO_repeater_bridge_rs232` environments. The new env will enable `WITH_RS232_BRIDGE=Serial2`, set the RS232 RX/TX pins for Heltec V4, and include `helpers/bridges/RS232Bridge.cpp` in the build.

## Requirements
1. **Board configuration**
   - Create a new PlatformIO environment for Heltec V4 acting as an RS232 bridge.
   - Use the same “repeater” base config as existing Heltec V4 `heltec_v4_repeater`/`heltec_v4_oled` environments.

2. **RS232 bridge feature**
   - Enable `WITH_RS232_BRIDGE` and select a UART instance (likely `Serial2`).
   - Define `WITH_RS232_BRIDGE_RX` and `WITH_RS232_BRIDGE_TX` to match Heltec V4 pins that are free (not used by LoRa, GPS, display, etc.).

3. **Source inclusion**
   - Ensure `helpers/bridges/RS232Bridge.cpp` is compiled in this environment.

4. **Documentation / testing**
   - Document the chosen pins and the expected wiring/level conversion for RS232.
   - Provide a basic verification checklist (build, flash, connect adapter, observe packet relay).

## Implementation Plan

### Step 1: Identify RS232 UART pins on Heltec V4 (REQUIRED)
- Review Heltec V4 pin mapping (in `variants/heltec_v4/pins_arduino.h` and board documentation).
- Pick a pair of GPIOs that:
  - Are not already used by other peripherals in the build (LoRa, OLED, GPS, ADC).
  - Are safe to use as UART RX/TX on ESP32-S3.
- **Suggested starting point:** GPIO 5 (RX) and GPIO 6 (TX) (mirrors Heltec v3 and other boards), unless those pins are reserved by the V4 design.

### Step 2: Add new PlatformIO environment
- Edit `variants/heltec_v4/platformio.ini`.
- Add a new env block, e.g. `[env:heltec_v4_repeater_bridge_rs232]`.
- Base it on `heltec_v4_oled` using the `extends = env:heltec_v4_oled` directive (so display + repeater UI matches other envs).
- Mirror the `Heltec_v3_repeater_bridge_rs232` config:
  - Append to existing build flags using `build_flags = ${env:heltec_v4_oled.build_flags}` or `${env.build_flags}`.
  - `  -D DISPLAY_CLASS=SSD1306Display`
  - `  -D ADVERT_NAME="RS232 Bridge"`
  - `  -D ADMIN_PASSWORD="password"`
  - `  -D MAX_NEIGHBOURS=50`
  - `  -D WITH_RS232_BRIDGE=Serial2`
  - `  -D WITH_RS232_BRIDGE_RX=<chosen RX pin>`
  - `  -D WITH_RS232_BRIDGE_TX=<chosen TX pin>`
- Add `helpers/bridges/RS232Bridge.cpp` to `build_src_filter` using PlatformIO interpolation: `build_src_filter = ${env.build_src_filter} +<helpers/bridges/RS232Bridge.cpp>`

### Step 3: Validate build and correct pins
- Build the new env via PlatformIO (`pio run -e heltec_v4_repeater_bridge_rs232`).
- If the build fails due to missing pins or conflicts, adjust pin selection.

### Step 4: Hardware verification
- Flash the board with the new firmware.
- Connect a USB-RS232/TTL adapter (or a true RS232 converter, depending on desired interface) to the chosen RX/TX pins.
- Send/receive data over the bridge and confirm packet exchange with another MeshCore node.

## Verification Checklist
1. `pio run -e heltec_v4_repeater_bridge_rs232` completes successfully.
2. Firmware boots and the display shows the repeater advert name.
3. A connected external serial device can send/receive mesh packets via the bridge (use known packet format or a peer node).
4. Optional: Enable `BRIDGE_DEBUG` to verify RS232 traffic in logs.

## Notes / Open Questions
- **Pin selection:** I recommend validating the chosen UART pins against Heltec V4 schematics; if 5/6 are already used for something else, we need another free UART-capable GPIO pair.
- **Level shifting:** The bridge code expects TTL-level serial. Connecting true RS232 (+/- 12V) directly will damage the ESP32 (which expects 3.3V). If you need true RS232 levels, an external 3.3V level shifter (e.g., MAX3232) is required.
