# Implementation Notes - Xiao nRF52 D6/D7 Serial Bridge

## Date: 2026-04-04

## Current Status

**Phase:** Investigation/Testing

## Findings Summary

### 1. Pin Configuration Conflict Found

**Location:** `variants/xiao_nrf52/platformio.ini`

The Xiao_nrf52 base variant defines:
```ini
-D PIN_WIRE_SCL=D6
-D PIN_WIRE_SDA=D7
```

The Xiao_nrf52_serial_bridge_base variant tries to use the same pins for UART:
```ini
-D WITH_RS232_BRIDGE_RX=D7
-D WITH_RS232_BRIDGE_TX=D6
```

This creates a **pin conflict** between I2C (Wire) and UART (Serial1).

### 2. Root Cause Analysis

**File:** `variants/xiao_nrf52/XiaoNrf52Board.cpp` (lines 47-53)

```cpp
#ifndef WITH_RS232_BRIDGE
#if defined(PIN_WIRE_SDA) && defined(PIN_WIRE_SCL)
  Wire.setPins(PIN_WIRE_SDA, PIN_WIRE_SCL);
#endif
#endif

Wire.begin();
```

**Problem:** When `WITH_RS232_BRIDGE` is defined:
- `Wire.setPins()` is correctly skipped
- BUT `Wire.begin()` is **STILL CALLED** with default pins (D6/D7)

### 3. Pin Mapping Details

From `variants/xiao_nrf52/variant.cpp`:
| Pin | nRF GPIO | Default Function |
|-----|----------|-------------------|
| D6  | P1.11    | UART TX           |
| D7  | P1.12    | UART RX           |

From `variants/xiao_nrf52/variant.h` (lines 129-130):
```cpp
// #define PIN_WIRE_SDA            (17) // 4 and 5 are used for the sx1262 !
// #define PIN_WIRE_SCL            (16) // use WIRE1_SDA
```

This comment suggests I2C was intended to be moved to D16/D17, but the platformio.ini still uses D6/D7.

### 4. Hardware Test Results

- **D6:** Shows constant +3.3V (normal - UART TX idle state is HIGH)
- **D7:** Shows 0V (possible pull-down or initialization issue)

### 5. Additional Investigation: D6/D7 Pin States Explained

**D6 showing +3.3V constant:**
- This is NORMAL behavior for UART TX idle state
- UART lines idle HIGH when not transmitting data
- This indicates the pin IS configured for UART function
- The bridge code in `RS232Bridge.cpp:20` correctly calls `setPins()`

**D7 showing 0V:**
- Could be caused by:
  1. External device pulling RX low
  2. Internal pull-down enabled
  3. RX line not properly initialized
- Need to check if Serial1.begin() is actually being called
- Need to verify no Wire library is interfering

### 6. Code Flow for Bridge Initialization

1. `MyMesh.cpp:838` - Bridge object created with `WITH_RS232_BRIDGE` (Serial1)
2. `MyMesh.cpp:915` - `bridge.begin()` called
3. `RS232Bridge.cpp:20` - UART pins set via `((Uart *)_serial)->setPins()`
4. `RS232Bridge.cpp:30` - `((HardwareSerial *)_serial)->begin(_prefs->bridge_baud)`

## Proposed Fix Options

### Option A: Override PIN_WIRE_SDA/SCL in serial_bridge_base (SELECTED)
Change the platformio.ini for Xiao_nrf52_serial_bridge_base to use different pins for I2C:
```ini
-D PIN_WIRE_SCL=D16
-D PIN_WIRE_SDA=D17
```
**Rationale:** These are the pins suggested by variant.h comments. No sensors are enabled in this variant, so I2C is not needed. Minimal change.

### Option B: Disable Wire entirely in serial_bridge_base
Add a flag to completely disable Wire initialization:
```ini
-D DISABLE_WIRE=1
```
And update XiaoNrf52Board.cpp to check for this flag.

### Option C: Use different UART pins for bridge
NOT VIABLE - D4/D5 are used by SX1262 radio (NSS, RXEN)

## Implemented Fix

**File Modified:** `variants/xiao_nrf52/platformio.ini`

Added to `[Xiao_nrf52_serial_bridge_base]`:
```ini
  ; Override I2C pins to D16/D17 to avoid conflict with UART bridge pins
  -D PIN_WIRE_SCL=D16
  -D PIN_WIRE_SDA=D17
```

This reassigns the I2C pins from D6/D7 to D16/D17, which are:
- D16 = P0.27 (6D_I2C_SCL) - shared with IMU but IMU not present on this board
- D17 = P0.07 (6D_I2C_SDA) - shared with IMU but IMU not present on this board

This avoids the conflict with the UART bridge pins D6/D7.

## Files Modified

1. `variants/xiao_nrf52/platformio.ini` - Added PIN_WIRE_SCL/SDA override to D16/D17
2. `variants/xiao_nrf52/variant.h` - Added D16 and D17 pin definitions

## Implementation Log

### 2026-04-04 - Fix Implemented
- Added PIN_WIRE_SCL=D16 and PIN_WIRE_SDA=D17 to Xiao_nrf52_serial_bridge_base
- This moves I2C from D6/D7 (conflict with UART) to D16/D17
- Added D16 and D17 definitions to variant.h (was missing)

### 2026-04-04 - Build Successful
- Build completed successfully
- RAM: 12.3% (29,068 / 235,520 bytes)
- Flash: 60.0% (425,472 / 708,608 bytes)

## Next Steps

1. [x] Decide on fix option (A, B, or C)
2. [x] Implement the fix in platformio.ini and/or XiaoNrf52Board.cpp
3. [x] Rebuild firmware
4. [ ] Retest D6/D7 voltage levels
5. [ ] Verify bridge communication works

## Questions to Resolve

1. Is I2C actually needed for this variant? (Sensors are configured but may not be present)
2. Should we use D16/D17 for Wire (as variant.h comment suggests)?
3. Or is it acceptable to disable Wire entirely for the bridge variant?

## Related Documentation

- Main plan: `docs/meshcore_xiao_d6_d7_serial_bridge_plan.md`
