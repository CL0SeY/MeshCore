# RAK 1W Combined Repeater + USB Client Implementation Plan

**Target device:** RAK 1W (RAK3401 + RAK13302) — nRF52840 + SX1262
**Firmware:** Combined mesh repeater + USB companion client for MeshCore bot
**Date:** 2026-07-26 (revised)

---

## Architecture Overview

```
                     LoRa Mesh
                       │
    ┌──────────────────┼──────────────────┐
    │   RAK 1W (single device)             │
    │                                       │
    │  ┌─────────────────────────────────┐ │
    │  │  MeshCore Firmware              │ │
    │  │                                 │ │
    │  │  ┌──────────┐  ┌────────────┐  │ │
    │  │  │ Repeater  │  │ USB CDC    │  │ │
    │  │  │ Mesh Stack│  │ Client     │  │ │
    │  │  │ (LoRa)    │  │ Protocol   │  │ │
    │  │  └──────────┘  └─────┬──────┘  │ │
    │  │                      │         │ │
    │  │              nRF52 USB D+ / D- │ │
    │  └─────────────────────────────────┘ │
    └──────────────────────────────────────┘
                       │
                USB-C / Micro-USB
                       │
                ┌──────▼──────┐
                │ MeshCore Bot │
                │ (Python/App) │
                └───────────────┘
```

One firmware binary, one PK, one node. The device:
- Runs a full **mesh repeater** (flood forwarding, ACL, region map, discovery, telemetry)
- Exposes a **companion USB client** over nRF52840 native USB CDC
- The bot connects via `/dev/ttyACM0` (Linux) or `COMx` (Windows) using the MeshCore companion protocol
- No display, no GPS, no BLE

---

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Base firmware | `examples/simple_repeater` | Has full mesh repeater features (ACL, region map, flood forwarding, discovery, telemetry, admin CLI) |
| Serial interface | `ArduinoSerialInterface` over USB CDC | Bot already speaks companion protocol (KISS-like framing) |
| Identities | Single `self_id` (one PK) | Mesh routing is PK-based; ADV_TYPE is metadata only; one PK simplifies storage and ACL |
| Display | None | Saves 50-80KB flash + 10-15KB RAM |
| GPS | Undefined via `-UENV_INCLUDE_GPS` | Not needed for repeater + USB client role; `-UENV_INCLUDE_GPS` tells the SDK to exclude GPS code |
| BLE | Not compiled in | Only USB CDC companion interface; reduces code size |
| Build variant | Modify existing `[env:RAK_3401_repeater_companion_usb]` in `variants/rak3401/platformio.ini` | Environment already exists (partially configured), but is missing build_flags and needs MyMesh changes |

**Important:** The `[env:RAK_3401_repeater_companion_usb]` environment already exists at `variants/rak3401/platformio.ini:137`. It has the correct board config, `ArduinoSerialInterface.cpp` in build_src_filter, pin defines, GPS undefined, contact limits, `ADVERT_TYPE=1`, and `CLIENT_REPEAT_DEFAULT=1`. What is MISSING is `-DMESH_DEBUG=1` and — critically — the **application code changes** in `simple_repeater/` that wire up the companion protocol handler.

---

## Current State of `variants/rak3401/platformio.ini` (lines 137-157)

The existing `[env:RAK_3401_repeater_companion_usb]` has:

```ini
[env:RAK_3401_repeater_companion_usb]
extends = rak3401
board_build.ldscript = boards/nrf52840_s140_v6_extrafs.ld
board_upload.maximum_size = 712704
build_flags =

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

   densaugeo/base64 @ ~1.4.0
```

### What the existing env has that the original plan omitted:

| Missing from original plan | Current value |
|---|---|
| `-UENV_INCLUDE_GPS` | Present (undefined, not just disabled) |
| `-D ADVERT_TYPE=1` | Present (ADV_TYPE_REPEATER) |
| `-D CLIENT_REPEAT_DEFAULT=1` | Present |
| Pin defines (`PIN_USER_BTN`, etc.) | Present |
| `-I examples/simple_repeater` | Present |
| `MAX_CONTACTS=200`, `MAX_GROUP_CHANNELS=20` | Present |

### What needs to be ADDED to the existing env:

| Addition | Value | Rationale |
|---|---|---|
| `-DMESH_DEBUG=1` | New line in `build_flags` | Debug output on startup; can be removed for production |
| `-DMESH_PACKET_LOGGING=1` | Optional, commented out | Only if packet logging is needed for debugging |

### build_src_filter changes needed:

The existing env already has `+<../examples/simple_repeater>` and `+<../helpers/ArduinoSerialInterface.cpp>`. No changes needed to build_src_filter at this stage. No display UI files need removal because `DISPLAY_CLASS` is not defined, so the `#ifdef DISPLAY_CLASS` guards in the source code already exclude them.

### `lib_deps` note:

`densaugeo/base64 @ ~1.4.0` is present because the companion protocol uses base64 for data encoding (DataStore / contact export). Even though the repeater doesn't use DataStore, `handleCmdFrame()` adaptation may use base64 for `CMD_IMPORT_CONTACT` / `CMD_EXPORT_CONTACT`. If those commands are not implemented, base64 can be removed.

---

## File Changes

### 1. Environment: `variants/rak3401/platformio.ini`

Add `-DMESH_DEBUG=1` to `build_flags` of the existing `[env:RAK_3401_repeater_companion_usb]`. Optionally uncomment `-DMESH_PACKET_LOGGING=1` for debugging.

**No other changes to this file are needed** — all other required flags (`-UENV_INCLUDE_GPS`, `-D ADVERT_TYPE=1`, `-D CLIENT_REPEAT_DEFAULT=1`, pin defs, `ArduinoSerialInterface.cpp`, `base64`) are already present.

### 2. `examples/simple_repeater/main.cpp`

**Current behavior (lines 30-163):**
- `Serial.begin(115200)` in `setup()` for debug
- Serial CLI command loop in `loop()` that reads chars, accumulates, calls `the_mesh.handleCommand(0, command, reply)`, prints reply
- `sensors.loop()` and `rtc_clock.tick()` called at end of `loop()`
- `#ifdef DISPLAY_CLASS` blocks guard `UITask` — these are no-ops when `DISPLAY_CLASS` is undefined

**Required changes:**

(a) **Remove the Serial CLI command loop from `loop()`** (lines 107-131). This is the `while (Serial.available())` accumulation block, the command buffer handling, and the `the_mesh.handleCommand()` call.

(b) **Add ArduinoSerialInterface initialization.** After `the_mesh.begin(fs)` (line 92), add:
```cpp
#include <helpers/ArduinoSerialInterface.h>
```
At global scope (line ~16, after `StdRNG fast_rng;`):
```cpp
ArduinoSerialInterface serial_interface;
```
After `the_mesh.begin(fs)` in `setup()`:
```cpp
serial_interface.begin(Serial);
the_mesh.startInterface(serial_interface);
```

(c) **Preserve** the following in `loop()` exactly as-is:
- `sensors.loop()` (line 149)
- `rtc_clock.tick()` (line 153)
- Power saving / sleep logic (lines 155-163)
- `#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)` block (lines 133-146) — this is unrelated to the USB role
- `#ifdef DISPLAY_CLASS` / `ui_task.loop()` block (lines 150-152) — already excluded when `DISPLAY_CLASS` is undefined

(d) **`setup()` changes:**
- Keep `Serial.begin(115200)` — needed for debug output
- Keep `#ifdef DISPLAY_CLASS` blocks — they become no-ops when `DISPLAY_CLASS` is undefined
- After `the_mesh.begin(fs)`, add `serial_interface.begin(Serial)` and `the_mesh.startInterface(serial_interface)`
- Keep `command[0] = 0;` — it was used by the CLI loop and will no longer be needed, but removing it is low priority

### 3. `examples/simple_repeater/MyMesh.h`

**Current structure:** `class MyMesh : public mesh::Mesh, public CommonCLICallbacks` — does NOT extend `BaseChatMesh`.

**Required additions:**

(a) Add companion protocol includes (after existing `#include <helpers/...>` lines):
```cpp
#include <helpers/BaseSerialInterface.h>
```

(b) Add companion protocol command/response defines. These go near the top of the file (after existing `#define` constants, around line 82):
```cpp
// Companion protocol command codes
#define CMD_APP_START                 1
#define CMD_SEND_TXT_MSG              2
#define CMD_SEND_CHANNEL_TXT_MSG      3
#define CMD_GET_CONTACTS              4
#define CMD_GET_DEVICE_TIME           5
#define CMD_SET_DEVICE_TIME           6
#define CMD_SEND_SELF_ADVERT          7
#define CMD_SET_ADVERT_NAME           8
#define CMD_RESET_PATH                13
#define CMD_SET_ADVERT_LATLON         14
#define CMD_GET_STATS                 56
#define CMD_SEND_RAW_DATA             25
#define CMD_SEND_RAW_PACKET           65
#define CMD_DEVICE_QUERY              22
#define CMD_SEND_CHANNEL_DATA         62
#define CMD_SET_RADIO_PARAMS          11
#define CMD_SET_RADIO_TX_POWER        12
#define CMD_SET_DEFAULT_FLOOD_SCOPE   63
#define CMD_GET_DEFAULT_FLOOD_SCOPE   64
#define CMD_LOGIN                     26
#define CMD_LOGOUT                    29
#define CMD_SEND_LOGIN                26  // alias for CMD_LOGIN in some protocol versions
#define CMD_HAS_CONNECTION            28

// Response codes
#define RESP_CODE_OK                  0
#define RESP_CODE_ERR                 1
#define RESP_CODE_CONTACTS_START      2
#define RESP_CODE_CONTACT             3
#define RESP_CODE_END_OF_CONTACTS     4
#define RESP_CODE_SELF_INFO           5
#define RESP_CODE_SENT                6
#define RESP_CODE_NO_MORE_MESSAGES    10
#define RESP_CODE_DEVICE_INFO         13
#define RESP_CODE_STATS               24
#define RESP_ALLOWED_REPEAT_FREQ      26
#define RESP_CODE_DEFAULT_FLOOD_SCOPE 28

// Error codes
#define ERR_CODE_UNSUPPORTED_CMD      1
#define ERR_CODE_NOT_FOUND            2
#define ERR_CODE_TABLE_FULL           3
#define ERR_CODE_BAD_STATE            4
#define ERR_CODE_ILLEGAL_ARG          6

// Stats sub-types
#define STATS_TYPE_CORE               0
#define STATS_TYPE_RADIO              1
#define STATS_TYPE_PACKETS            2

// Helper macros
#define MAX_FRAME_SIZE  176
```

(c) Add `BaseSerialInterface* _serial;` member in the private section (after existing members, near line 104).

(d) Add `cmd_frame` and `out_frame` buffers in the private section:
```cpp
uint8_t cmd_frame[MAX_FRAME_SIZE + 1];
uint8_t out_frame[MAX_FRAME_SIZE + 1];
```

(e) Add method declarations in the public section (after existing methods, before closing `};`):
```cpp
void startInterface(BaseSerialInterface &serial);
void handleCmdFrame(size_t len);
void checkSerialInterface();
void writeOKFrame();
void writeErrFrame(uint8_t err_code);
void writeDisabledFrame();
```

(f) **Do NOT add `#include "BaseChatMesh.h"`** — the repeater does not extend `BaseChatMesh`. The repeater has its own ACL-based contact management and no message queue. `queueMessage()` does not exist.

(g) **Handle the `WITH_BRIDGE` / `AbstractBridge` code** (lines 16-24, 38-40, 116-120, 229-248): These are harmless dead code when neither RS232 nor ESP-NOW bridge is enabled. No action required for this build, but a subagent should not remove them as they are conditionally compiled and used by other variants.

### 4. `examples/simple_repeater/MyMesh.cpp`

This is the largest change. The repeater's MyMesh.cpp needs companion protocol support added.

#### A. Add helper methods for companion wire protocol (near top of file, after existing `#define` constants)

Add response frame output helpers (matching companion radio's `writeOKFrame`, `writeErrFrame`, etc.):

```cpp
void MyMesh::writeOKFrame() {
  uint8_t buf[1];
  buf[0] = RESP_CODE_OK;
  _serial->writeFrame(buf, 1);
}

void MyMesh::writeErrFrame(uint8_t err_code) {
  uint8_t buf[2];
  buf[0] = RESP_CODE_ERR;
  buf[1] = err_code;
  _serial->writeFrame(buf, 2);
}

void MyMesh::writeDisabledFrame() {
  uint8_t buf[1];
  buf[0] = RESP_CODE_DISABLED;
  _serial->writeFrame(buf, 1);
}
```

#### B. Add `startInterface()` implementation

```cpp
void MyMesh::startInterface(BaseSerialInterface &serial) {
  _serial = &serial;
  serial.enable();
}
```

#### C. Add `checkSerialInterface()` method (called from `loop()`)

```cpp
void MyMesh::checkSerialInterface() {
  size_t len = _serial->checkRecvFrame(cmd_frame);
  if (len > 0) {
    handleCmdFrame(len);
  }
}
```

#### D. Add `handleCmdFrame(size_t len)` — the main companion protocol dispatcher

This is a large method (~400-500 lines after adaptation). It must:

1. Parse the binary command from `cmd_frame[0]` and route to the appropriate handler
2. Write response frames to `out_frame[]` and call `_serial->writeFrame(out_frame, out_len)`
3. Use repeater-native methods (not BaseChatMesh methods) for all actions

**Commands to implement for the repeater role:**

| Command | Handler action |
|---------|---------------|
| `CMD_DEVICE_QUERY` | Return `RESP_CODE_DEVICE_INFO` with FIRMWARE_VER_CODE, firmware version, build date, ADVERT_TYPE=1 (REPEATER) |
| `CMD_APP_START` | Return `RESP_CODE_SELF_INFO` with ADV_TYPE_REPEATER, self_id.pub_key, node name, advert lat/lon. Note: use `ADVERT_TYPE=1` (REPEATER), not `ADV_TYPE_CHAT` |
| `CMD_SEND_TXT_MSG` | Extract text, find recipient via ACL lookup, call `sendMessage()` or `sendCommandData()` equivalent. Reply `RESP_CODE_SENT` or `RESP_CODE_ERR` |
| `CMD_GET_CONTACTS` | Iterate ACL contacts, send `RESP_CODE_CONTACT` frames for each, then `RESP_CODE_END_OF_CONTACTS`. Note: ACL contacts have a different structure than BaseChatMesh ContactInfo — adapt the frame format |
| `CMD_SYNC_NEXT_MESSAGE` | The repeater has no outgoing message queue like BaseChatMesh. Reply `RESP_CODE_NO_MORE_MESSAGES` |
| `CMD_SEND_SELF_ADVERT` | Call `createSelfAdvert()` and `sendZeroHop()` or `sendFloodScoped()` based on param. Reply `RESP_CODE_OK` |
| `CMD_RESET_PATH` | Find ACL contact by pubkey, reset path. Reply `RESP_CODE_OK` or `RESP_CODE_ERR` |
| `CMD_SET_ADVERT_NAME` | Update `_prefs.node_name`, call `savePrefs()`. Reply `RESP_CODE_OK` |
| `CMD_SET_ADVERT_LATLON` | This firmware has GPS disabled, so lat/lon should be rejected or ignored. Reply `RESP_CODE_ERR` with `ERR_CODE_ILLEGAL_ARG` |
| `CMD_GET_DEVICE_TIME` | Return `RESP_CODE_CURR_TIME` with `getRTCClock()->getCurrentTime()` |
| `CMD_SET_DEVICE_TIME` | Set RTC time, reply `RESP_CODE_OK` |
| `CMD_GET_STATS` | Return `RESP_CODE_STATS` with RepeaterStats (queue depth, air time, SNR, packet counts, error events, etc.) |
| `CMD_SEND_RAW_DATA` | Create raw data packet and send direct. Reply `RESP_CODE_OK` or `RESP_CODE_ERR` |
| `CMD_SEND_RAW_PACKET` | Parse raw mesh packet and send. Reply `RESP_CODE_OK` or `RESP_CODE_ERR` |
| `CMD_SEND_CHANNEL_DATA` | The repeater has no channel support equivalent. Reply `RESP_CODE_ERR` with `ERR_CODE_UNSUPPORTED_CMD` |
| `CMD_SET_RADIO_PARAMS` | Apply temporary radio params, save to prefs. Same logic as companion radio |
| `CMD_SET_RADIO_TX_POWER` | Set TX power, save pref. Same logic as companion radio |
| `CMD_SET_DEFAULT_FLOOD_SCOPE` | Set default flood scope key/name. Same logic as companion radio |
| `CMD_GET_DEFAULT_FLOOD_SCOPE` | Return current default flood scope |
| `CMD_LOGIN` | Authenticate with ACL password. Return `RESP_CODE_SELF_INFO`-style response or `RESP_CODE_OK` |
| `CMD_LOGOUT` | Clear any active session. Reply `RESP_CODE_OK` |
| `CMD_HAS_CONNECTION` | Check if a contact is currently in range / connected. Reply `RESP_CODE_OK` or `RESP_CODE_ERR` |
| **Unknown command** | Reply `RESP_CODE_ERR` with `ERR_CODE_UNSUPPORTED_CMD` |

**Key adaptation differences from companion radio:**
- No `BaseChatMesh::sendMessage()` — use the repeater's existing message-sending methods or expose a new one
- No `lookupContactByPubKey()` as in BaseChatMesh — use ACL's `getClient()` or similar method to find contacts
- No `DataStore` for persistent contact storage — ACL is stored separately
- No `ContactsIterator` or `startContactsIterator()` — ACL iterate differently
- No `_prefs` fields for chat-specific settings (no `ble_pin`, `client_repeat` in NodePrefs — wait, `CLIENT_REPEAT_DEFAULT` is a build flag that sets the default, but NodePrefs may still have `client_repeat` enabled)
- No `sensors.node_lat` / `sensors.node_lon` in GPS-less builds — lat/lon commands should be rejected
- The `writeContactRespFrame()` helper from the companion radio cannot be used directly — ACL contacts have different fields than BaseChatMesh ContactInfo

**Adapter note:** For commands that BaseChatMesh provides (CMD_GET_CONTACTS, CMD_RESET_PATH, etc.), the repeater must use its own ACL/contact interfaces. The minimal viable approach is to implement only commands that the repeater can fully support (APP_START, DEVICE_QUERY, SEND_TXT_MSG, GET_STATS, SEND_SELF_ADVERT, GET_DEVICE_TIME, SET_DEVICE_TIME, SEND_RAW_DATA, SET_RADIO_PARAMS, SET_RADIO_TX_POWER) and return `ERR_CODE_UNSUPPORTED_CMD` for the rest.

#### E. Add `RepeaterStats` population method for CMD_GET_STATS

```cpp
void MyMesh::writeRepeaterStats(uint8_t stats_type) {
  int i = 0;
  out_frame[i++] = RESP_CODE_STATS;
  out_frame[i++] = stats_type;
  if (stats_type == STATS_TYPE_CORE) {
    uint16_t battery_mv = board.getBattMilliVolts();
    uint32_t uptime_secs = getRTCClock()->getCurrentTime();
    uint8_t queue_len = (uint8_t)_mgr->getOutboundTotal();
    // ... populate out_frame with core stats
  } else if (stats_type == STATS_TYPE_RADIO) {
    // ... populate out_frame with radio stats (noise floor, RSSI, SNR, air time)
  } else if (stats_type == STATS_TYPE_PACKETS) {
    // ... populate out_frame with packet stats (recv/sent/flood/direct/errors)
  }
  _serial->writeFrame(out_frame, i);
}
```

The exact field layout should match what the bot expects for repeater-type nodes.

### 5. `examples/simple_repeater/MyMesh::loop()`

**Current behavior** (MyMesh.cpp line 1265):
```cpp
void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif
  mesh::Mesh::loop();
  // ... advert timers, radio params, ACL save, uptime tracking
}
```

**Required change:** Add `checkSerialInterface()` call. This is the critical fix that the previous plan missed — `mesh::Mesh::loop()` does NOT call `checkRecvFrame()`. Without this call, no companion protocol frames will ever be processed.

```cpp
void MyMesh::loop() {
#ifdef WITH_BRIDGE
  bridge.loop();
#endif

  mesh::Mesh::loop();

  // Companion protocol: process incoming USB CDC frames
  checkSerialInterface();

  // ... existing advert timer logic (unchanged)
  if (next_flood_advert && millisHasNowPassed(next_flood_advert)) { ... }
  else if (next_local_advert && millisHasNowPassed(next_local_advert)) { ... }

  // ... existing radio param timer logic (unchanged)
  // ... existing ACL save logic (unchanged)
  // ... existing uptime tracking (unchanged)
}
```

### 6. `examples/simple_repeater/MyMesh::hasPendingWork()`

**Current behavior** (MyMesh.cpp line 1311):
```cpp
bool MyMesh::hasPendingWork() const {
#if defined(WITH_BRIDGE)
  if (bridge.isRunning()) return true;  // bridge needs WiFi radio, can't sleep
#endif
  return _mgr->getOutboundTotal() > 0;
}
```

**No change required** — the `WITH_BRIDGE` block is already conditionally compiled and is a no-op when neither bridge is defined. The existing code is correct.

### 7. Build, flash, test

Build: `pio run -e RAK_3401_repeater_companion_usb`
Flash to RAK 1W via USB or DFU.
Connect bot over USB CDC (`/dev/ttyACM0` or `COMx`).

Verification checklist:
- [ ] Device appears as USB CDC serial endpoint
- [ ] Bot sends `CMD_DEVICE_QUERY` and receives `RESP_CODE_DEVICE_INFO` with `ADV_TYPE_REPEATER` (1)
- [ ] Bot sends `CMD_APP_START` and receives `RESP_CODE_SELF_INFO` with correct node identity and advert type REPEATER
- [ ] Bot can send text messages via mesh (`CMD_SEND_TXT_MSG`)
- [ ] Bot receives mesh messages from other nodes
- [ ] Bot can query repeater stats (`CMD_GET_STATS`)
- [ ] Mesh packets from other nodes are forwarded by the device (repeater forwarding still works)
- [ ] Admin clients (over LoRa mesh) can still use CLI commands
- [ ] Debug output on serial does not corrupt companion protocol framing

---

## What the Bot Gets Over USB

| Command | Description | Supported? |
|---------|-------------|:---:|
| `CMD_DEVICE_QUERY` | Bot identifies itself, gets firmware version | Yes |
| `CMD_APP_START` | Bot establishes connection, receives node identity + advert type REPEATER | Yes |
| `CMD_SEND_TXT_MSG` | Bot sends text message to a contact via mesh | Yes |
| `CMD_GET_CONTACTS` | Bot syncs contact list | Partial — ACL contacts only, not BaseChatMesh-style |
| `CMD_SYNC_NEXT_MESSAGE` | Bot drains message queue | Stub — returns NO_MORE_MESSAGES (repeater has no outgoing queue) |
| `CMD_SEND_SELF_ADVERT` | Bot triggers zero-hop or flood advert | Yes |
| `CMD_RESET_PATH` | Bot resets route to a contact | Partial — ACL contact path reset |
| `CMD_SET_ADVERT_NAME` | Bot sets node name | Yes |
| `CMD_SET_ADVERT_LATLON` | Bot sets GPS location | No — GPS is disabled; returns ERR_CODE_ILLEGAL_ARG |
| `CMD_GET_DEVICE_TIME` | Bot gets node RTC time | Yes |
| `CMD_SET_DEVICE_TIME` | Bot sets node RTC time | Yes |
| `CMD_GET_STATS` | Bot queries repeater stats | Yes |
| `CMD_SEND_RAW_DATA` | Bot sends custom raw data | Yes |
| `CMD_SEND_RAW_PACKET` | Bot sends raw mesh packet (debug/diag) | Yes |
| `CMD_SEND_CHANNEL_DATA` | Bot sends group channel data | No — returns ERR_CODE_UNSUPPORTED_CMD |
| `CMD_SET_RADIO_PARAMS` | Bot adjusts LoRa params | Yes |
| `CMD_SET_RADIO_TX_POWER` | Bot adjusts TX power | Yes |
| `CMD_SEND_CHANNEL_TXT_MSG` | Bot sends group channel text | No — returns ERR_CODE_UNSUPPORTED_CMD |
| `CMD_ADD_UPDATE_CONTACT` | Bot adds/updates a contact | No — returns ERR_CODE_UNSUPPORTED_CMD |
| `CMD_REMOVE_CONTACT` | Bot removes a contact | No — returns ERR_CODE_UNSUPPORTED_CMD |
| `CMD_SHARE_CONTACT` | Bot shares a contact zero-hop | No — returns ERR_CODE_UNSUPPORTED_CMD |
| `CMD_EXPORT_CONTACT` | Bot exports a contact | No — returns ERR_CODE_UNSUPPORTED_CMD |
| `CMD_IMPORT_CONTACT` | Bot imports a contact | No — returns ERR_CODE_UNSUPPORTED_CMD |
| `CMD_LOGIN` | Bot authenticates with password | Yes — routes to ACL authentication |
| `CMD_LOGOUT` | Bot disconnects | Yes — stub, returns OK |
| `CMD_SET_FLOOD_SCOPE_KEY` | Bot sets flood scope key | Partial — set/unset scope key |
| `CMD_GET_DEFAULT_FLOOD_SCOPE` | Bot gets default flood scope | Yes |
| `CMD_SET_DEFAULT_FLOOD_SCOPE` | Bot sets default flood scope | Yes |
| `CMD_REBOOT` | Bot reboots the device | Yes |

Push codes the bot receives:
| Push Code | Description |
|-----------|-------------|
| `PUSH_CODE_ADVERT` (0x80) | New contact advertisement |
| `PUSH_CODE_MSG_WAITING` (0x83) | Message queue has entries (not applicable for repeater) |
| `PUSH_CODE_SEND_CONFIRMED` (0x82) | Message ACK received |
| `PUSH_CODE_STATUS_RESPONSE` (0x87) | Reply to status query |

---

## What Other Mesh Nodes See

| Property | Value |
|----------|-------|
| Advert type | `ADV_TYPE_REPEATER` (1) |
| Advert name | Configurable via `ADVERT_NAME` or `CMD_SET_ADVERT_NAME` |
| Adv lat/lon | Configurable via build flags `ADVERT_LAT`/`ADVERT_LON` (GPS disabled, so these have no hardware source) |
| Forwarding | Enabled (all flood packets forwarded from other nodes) |
| ACL | Enforced (clients authenticate with password) |
| Admin CLI | Available over USB CDC only (not over LoRa) |
| GPS | Disabled (`-UENV_INCLUDE_GPS`) |
| BLE | Not compiled in |

---

## Flash/RAM Budget Estimate

| Component | Flash (approx) | RAM (approx) |
|-----------|:-:|:-:|
| nRF52840 + SX1262 driver | 30 KB | 8 KB |
| MeshCore mesh stack (Mesh, Dispatcher, Packet, Crypto) | 80 KB | 30 KB |
| Simple repeater (ACL, region map, neighbour table, telemetry) | 60 KB | 20 KB |
| ArduinoSerialInterface + framing | 5 KB | 3 KB |
| Companion protocol handlers (handleCmdFrame, response codes) | 15 KB | 5 KB |
| Storage (IdentityStore, ACL store) | 10 KB | 8 KB |
| cmd_frame + out_frame buffers (2 × 177 bytes) | < 1 KB | < 1 KB |
| Free (headroom) | ~50 KB | ~30 KB |
| **Total** | **~251 KB** | **~74 KB** |

nRF52840 has 1 MB flash / 256 KB RAM. Both are comfortably within limits.

---

## Risk Items

| Risk | Mitigation |
|------|-----------|
| `handleCmdFrame()` from companion radio depends on BaseChatMesh members not present in simple repeater | Copy/adapt only the command parsing logic; route to existing repeater methods. Do NOT port BaseChatMesh-dependent code (ContactsIterator, DataStore, contact storage). Return `ERR_CODE_UNSUPPORTED_CMD` for commands that need BaseChatMesh |
| Two Serial consumers (debug output + companion framing) on same `Serial` USB CDC | Both use the same `Stream` abstraction; ArduinoSerialInterface consumes bytes via Serial.read() — debug output goes out the same Serial. The companion protocol's frame parser (`<` header + 2-byte big-endian length + payload) handles framing correctly even interleaved with debug text |
| `mesh::Mesh::loop()` does NOT call `checkRecvFrame()` | **This was the critical gap in the original plan.** The repeater's `MyMesh::loop()` MUST explicitly call `checkSerialInterface()` which calls `_serial->checkRecvFrame(cmd_frame)` + `handleCmdFrame(len)`. Without this, no USB frames are ever processed |
| `CMD_GET_CONTACTS` needs ACL-based contact iteration | The repeater has `ClientACL` which stores `ClientInfo` entries (pubkey + permissions), not `ContactInfo` entries (pubkey + name + type + path + gps + lastmod). The `handleCmdFrame()` adapter must convert ACL contacts to the companion protocol's frame format, or return a minimal contact list |
| `CMD_SET_ADVERT_LATLON` and GPS-dependent features | GPS is disabled via `-UENV_INCLUDE_GPS`. Set `sensors.node_lat`/`sensors.node_lon` to 0.0 or use build flags `ADVERT_LAT`/`ADVERT_LON` for static coordinates. Reject lat/lon set commands with `ERR_CODE_ILLEGAL_ARG` or silently ignore them |
| `base64` lib_dep (densaugeo/base64) only needed if import/export contact commands are implemented | If those commands are stubbed as unsupported, the base64 dependency can be removed from lib_deps |
| `WITH_BRIDGE` / `AbstractBridge` dead code in MyMesh.h/cpp | These blocks are conditionally compiled (`#ifdef WITH_BRIDGE`) and harmless. Not an error to have them present |
| `FIRMWARE_ROLE` consistency | Set build flag `-D ADVERT_TYPE=1` (already present in existing env) so the node advertises as REPEATER to the mesh |
| `CLIENT_REPEAT_DEFAULT=1` already set in existing env | Ensures repeater forwarding works out of the box |

---

## Implementation Steps (sequential, for subagent)

### Step 1: Update build environment
**File:** `variants/rak3401/platformio.ini`
- Add `-DMESH_DEBUG=1` to `build_flags` of `[env:RAK_3401_repeater_companion_usb]`
- No other changes to this file are needed (all other flags are present)
- Status: **Ready to write**

### Step 2: Update `simple_repeater/main.cpp`
**File:** `examples/simple_repeater/main.cpp`
- Remove Serial CLI command loop from `loop()` (lines 107-131)
- Add `#include <helpers/ArduinoSerialInterface.h>` after existing includes
- Add `ArduinoSerialInterface serial_interface;` as global variable
- Add `serial_interface.begin(Serial)` and `the_mesh.startInterface(serial_interface)` after `the_mesh.begin(fs)` in `setup()`
- Preserve `sensors.loop()`, `rtc_clock.tick()`, power saving, and `#ifdef DISPLAY_CLASS` blocks exactly as-is
- Status: **Ready to write**

### Step 3: Update `simple_repeater/MyMesh.h`
**File:** `examples/simple_repeater/MyMesh.h`
- Add `#include <helpers/BaseSerialInterface.h>`
- Add companion protocol command/response/error defines (full set)
- Add `BaseSerialInterface* _serial;` member
- Add `uint8_t cmd_frame[MAX_FRAME_SIZE + 1];` and `uint8_t out_frame[MAX_FRAME_SIZE + 1];` members
- Add method declarations: `startInterface()`, `handleCmdFrame()`, `checkSerialInterface()`, `writeOKFrame()`, `writeErrFrame()`, `writeDisabledFrame()`
- Do NOT add `BaseChatMesh.h` include or `#include "BaseChatMesh.h"`
- Status: **Ready to write**

### Step 4: Update `simple_repeater/MyMesh.cpp`
**File:** `examples/simple_repeater/MyMesh.cpp`
- Add `writeOKFrame()`, `writeErrFrame()`, `writeDisabledFrame()` implementations
- Add `startInterface()` implementation
- Add `checkSerialInterface()` implementation (calls `_serial->checkRecvFrame(cmd_frame)` then `handleCmdFrame(len)`)
- Add `handleCmdFrame(size_t len)` — full companion protocol dispatcher (~400-500 lines). Implement the commands listed in the command support table above. Return `ERR_CODE_UNSUPPORTED_CMD` for commands that cannot be supported without BaseChatMesh.
- Add `writeRepeaterStats(uint8_t stats_type)` helper for `CMD_GET_STATS`
- Status: **Ready to write** (largest change by far)

### Step 5: Update `simple_repeater/MyMesh::loop()`
**File:** `examples/simple_repeater/MyMesh.cpp`, line 1265
- Add `checkSerialInterface();` call after `mesh::Mesh::loop();`
- This is the critical fix for processing USB companion protocol frames
- Status: **Ready to write**

### Step 6: Verify `simple_repeater/MyMesh::hasPendingWork()`
**File:** `examples/simple_repeater/MyMesh.cpp`, line 1311
- No change needed — existing code is correct
- Status: **Verified — no change required**

### Step 7: Build, flash, test
- Build: `pio run -e RAK_3401_repeater_companion_usb`
- Flash to RAK 1W via USB or DFU
- Connect bot over USB CDC; verify each item in the verification checklist above
- Status: Ready to execute

---

## Open Questions

1. Does the bot's `CMD_APP_START` response need `ADV_TYPE_REPEATER` (1) or `ADV_TYPE_CHAT` (2)? → **Use REPEATER (1)** since this firmware is a repeater.
2. Should `client_repeat` preference be set at build time default or left as runtime pref? → **Set at build time** (`-DCLIENT_REPEAT_DEFAULT=1`) to ensure forwarding works out of the box. The existing env already has this.
3. Do we need the packet log (`PACKET_LOG_FILE`) for debugging? → **Optional** — add `-DMESH_PACKET_LOGGING=1` build flag if needed, then remove for production.
4. Which companion protocol commands are in MVP scope vs deferred? The table in "What the Bot Gets Over USB" marks supported commands. Recommend MVP: `CMD_DEVICE_QUERY`, `CMD_APP_START`, `CMD_SEND_TXT_MSG`, `CMD_GET_STATS`, `CMD_SEND_SELF_ADVERT`, `CMD_GET_DEVICE_TIME`, `CMD_SET_DEVICE_TIME`, `CMD_SET_RADIO_PARAMS`, `CMD_SET_RADIO_TX_POWER`, `CMD_LOGIN`. Defer `CMD_GET_CONTACTS`, `CMD_RESET_PATH`, `CMD_SYNC_NEXT_MESSAGE` to a follow-up.
5. Should `CMD_SEND_TXT_MSG` in the repeater context forward the message as a mesh flood or route it to a specific contact? → **Route to specific contact** (direct send) since the repeater is a USB client endpoint, not a chat node. Use `sendCommandData()` for CLI data or a new direct message send path.
6. Does the bot need `CMD_SET_FLOOD_SCOPE_KEY` / `CMD_SET_DEFAULT_FLOOD_SCOPE` support? → **Yes** — the repeater should allow the bot to configure flood scope for repeat-scoped messages.
