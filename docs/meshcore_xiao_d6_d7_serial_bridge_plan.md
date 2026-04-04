# MeshCore XIAO nRF52840 + Wio-SX1262 custom build plan

This plan is for creating a custom MeshCore build for the Seeed XIAO nRF52840 + Wio-SX1262 kit, with GPIO **D6/D7** assigned to a UART-based serial bridge instead of any conflicting default use. MeshCore uses **PlatformIO** for builds, and nRF targets are packaged for DFU flashing as ZIP artifacts.[cite:1] The XIAO nRF52840 exposes **1 UART and 11 GPIO**, so moving D6/D7 to bridge duty likely means replacing an existing UART/GPS assignment rather than adding a second independent UART.[cite:8]

## Objective

Produce a repeatable custom MeshCore target that:

- Builds from the MeshCore source tree with PlatformIO.[cite:1]
- Targets the XIAO nRF52840 + Wio-SX1262 hardware family.[cite:8]
- Assigns **D6/D7** to the serial bridge UART.
- Disables or remaps any conflicting GPS/UART pin usage.
- Outputs a flashable nRF DFU ZIP package.[cite:1]

## Constraints

- The Seeed XIAO nRF52840 has limited exposed GPIO and only one documented UART, so UART pin assignment must be treated as a scarce resource.[cite:8]
- The SX1262 radio daughterboard already consumes board-specific radio control and SPI-related pins, so only the remaining exposed GPIO should be considered for bridge use.[cite:8]
- MeshCore release `repeater-v1.14.1` adds more GPS-related behavior, including automatic time sync for GPS-enabled nodes every 30 minutes, which increases the chance that GPS-related code paths may still be active unless deliberately disabled for the custom build.[cite:72]

## Agent workflow

### 1. Pin down the exact source baseline

- Check out the exact source tree that matches the intended firmware line, preferably the `repeater-v1.14.1` tag if the build is meant to track that release.[cite:72]
- Record the commit hash in the work log so later binary behavior can be traced back to source.
- Confirm whether XIAO nRF52840 + Wio-SX1262 support already exists in-tree, or whether the board support is only partial/community-maintained.[cite:29][cite:48]

### 2. Inspect PlatformIO targets

- Open `platformio.ini` and locate all environments referencing `xiao`, `nrf52`, `nrf52840`, `sx1262`, `repeater`, `companion`, `gps`, `uart`, or `serial`.[cite:1]
- Identify the closest existing environment for this hardware.
- Duplicate that environment into a custom target name, for example `xiao_nrf52840_wio_sx1262_serialbridge`, so the stock target is preserved.
- Keep build changes isolated to the custom environment and any dedicated board-configuration files.

### 3. Locate the board pin map

- Search the repo for the XIAO target's board-definition files and any pin-map constants for:
  - SX1262 SPI pins
  - Radio `busy`, `reset`, and DIO lines
  - UART TX/RX pins
  - GPS enable / GPS TX / GPS RX
  - Optional feature flags tied to GPS or serial roles
- Expect the relevant definitions to live in board-specific source/header files or target-specific compile flags rather than high-level routing code.[cite:1]

### 4. Establish the current UART ownership

- Determine which logical serial interface the current XIAO build uses for:
  - USB serial console
  - External UART
  - GPS
  - Companion / bridge / repeater features
- Verify whether D6/D7 are already referenced directly, indirectly through Arduino-style aliases, or through raw nRF GPIO numbers.
- Build a small mapping table in the work log that links:
  - MeshCore symbol name
  - XIAO pin alias
  - physical pin label on the board
  - current role
  - desired role

### 5. Decide the desired bridge mapping

Use this target mapping unless the source tree proves the TX/RX naming is inverted by board convention:

| Function | Pin |
|---|---|
| Bridge TX | D6 |
| Bridge RX | D7 |
| GND | Common ground to the other node |

For node-to-node UART, final hardware wiring remains **crossed** between devices, so one node's TX goes to the other node's RX and vice versa. The firmware pin definition itself should still represent the local node's own TX and RX roles.

### 6. Disable or remove conflicting GPS assignments

- If the current XIAO target uses the same UART instance or the same pins for GPS, disable GPS for this custom target instead of trying to multiplex both functions.
- Remove or override any compile-time flags enabling GPS support for the custom environment.
- Remove or override any board constants that bind GPS TX/RX or GPS enable pins to D6/D7.
- Check for any release-line features that assume GPS is present, because `repeater-v1.14.1` adds GPS-related time synchronization behavior.[cite:72]

### 7. Reassign the bridge UART pins

- Modify the XIAO board definition so the bridge UART uses D6/D7.
- Keep the radio pin definitions unchanged.
- If MeshCore distinguishes between console UART and application UART, ensure the **bridge** is mapped to the application-facing UART rather than a debug console.
- If MeshCore requires explicit baud-rate or serial-mode settings for the bridge role, set them in the same target so the build is self-contained.

### 8. Confirm no hidden conflicts

Before compiling, search the source tree for any remaining references to D6 or D7 in the XIAO target path.

- If D6/D7 are reused for sensors, GPS power, LEDs, or interrupts, either disable that feature for the custom target or choose a different pin plan.
- Verify that no startup code still initializes those pins for another purpose.
- Verify that the board variant does not reserve D6/D7 through a lower-level HAL or Arduino-variant mapping.

### 9. Build the custom environment

- Create a Python virtual environment if needed.
- Install PlatformIO inside that environment.[cite:1]
- Run the custom target build with `pio run -e <custom_environment>`.[cite:1]
- Archive the full build log.
- Confirm that the expected nRF DFU ZIP artifact is produced, since MeshCore documents ZIP-based DFU packaging for nRF targets.[cite:1]

### 10. Flash to hardware

- Put the XIAO nRF52840 into bootloader/DFU mode using the standard nRF workflow for the board.
- Flash the generated ZIP package using the nRF DFU flow documented by MeshCore, which references `adafruit-nrfutil` packaging/DFU usage for nRF devices.[cite:1]
- Label the flashed unit clearly as the custom serial-bridge build.

### 11. Validate bridge behavior on-bench

Run validation in stages:

1. **Boot validation** — device boots, radio initializes, no pin-conflict crashes.
2. **Serial validation** — confirm the configured UART is active on D6/D7 with the expected baud and framing.
3. **Isolation validation** — verify GPS-related features are absent or inactive on this custom build.
4. **Node-to-node validation** — wire two nodes with TX/RX crossed and common ground; confirm the serial bridge passes data as intended.
5. **Regression validation** — confirm LoRa radio behavior still works after the UART reassignment.

### 12. Produce a maintainable patch set

The final deliverable should include:

- A patch or branch containing only the custom-target changes.
- A short changelog describing the pin reassignment and GPS disablement.
- A build note listing:
  - source tag/commit
  - PlatformIO version
  - custom environment name
  - resulting artifact name
  - UART settings used for the bridge

## Suggested repo tasks for the implementing agent

- Search for all XIAO-related target definitions.
- Identify the exact board file controlling UART/GPS pin assignment.
- Create the new custom PlatformIO environment.
- Patch UART pin definitions to D6/D7.
- Patch out conflicting GPS definitions.
- Build and verify the DFU ZIP output.
- Flash a test unit.
- Bench-test two-node crossed-UART operation.
- Document the exact files changed.

## Acceptance criteria

The implementation is complete when all of the following are true:

- The custom build compiles cleanly from the chosen MeshCore source baseline.[cite:1]
- The resulting firmware flashes successfully to the XIAO nRF52840 + Wio-SX1262 hardware.[cite:1][cite:8]
- D6/D7 act as the intended UART bridge pins on the flashed node.
- No GPS code path or pin mapping still claims D6/D7 on this custom target.[cite:72]
- The node can participate in MeshCore radio operation while also exposing the intended serial bridge behavior.
- A second agent can reproduce the build from the notes without reverse-engineering the changes.

## Open questions to resolve during implementation

These should be answered from the checked-out source tree before coding is finalized:

- What is the exact PlatformIO environment name for the XIAO nRF52840 + Wio-SX1262 target, if one already exists?
- Which file actually owns the UART pin mapping for that target?
- Does MeshCore name the bridge role as `serial`, `uart`, `companion`, or something target-specific in code?
- Are D6/D7 referenced by Arduino pin aliases, nRF GPIO numbers, or both?
- Does the target use one UART for both debug and application traffic, or separate logical channels over USB vs external pins?

## Notes for implementation discipline

Do not start by editing broad application logic. Start at the board-definition layer, because the desired change is first a **hardware target configuration** problem. Only move upward into feature code if the source tree shows that serial-bridge behavior is hardcoded or gated behind a feature flag that the custom target must explicitly enable.[cite:1]
