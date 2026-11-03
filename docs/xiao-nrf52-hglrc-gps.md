# XIAO nRF52840 + HGLRC GPS (BLE companion)

Working setup, confirmed 2026-11-01. The node reported a real position. The generic sensor read `5061422`: 5 satellites at 06:14:22 UTC.

Firmware is `Xiao_nrf52_companion_radio_ble` only. The repeater, USB companion, room server, and kiss modem builds do not include this GPS.

```
pio run -e Xiao_nrf52_companion_radio_ble
```

Artifacts: `MeshCore/.pio/build/Xiao_nrf52_companion_radio_ble/`.

## Wiring

| Module | Connection |
| --- | --- |
| TX | D7 (silk **RX**, P1.12). This is the MCU receive pin. |
| RX | D6 (silk **TX**, P1.11). This is the MCU transmit pin. |
| GND | GND |
| VCC | Drain of a P-channel MOSFET |

MOSFET: source to the XIAO **3V3** pin, drain to module VCC, gate to D0. One 100 kΩ resistor from the gate to 3V3. No series resistor in the gate. D0 low turns the module on. D0 high turns it off.

Use a P-channel logic-level MOSFET with Vgs(th) between −0.5 V and −2.0 V so a 3.3 V gate swing turns it fully on. Do not substitute an N-channel between 3V3 and VCC. Do not also tie module VCC to the XIAO 3V3 pin. Do not tie D0 to 3V3.

Until that MOSFET is fitted, module VCC may stay on the XIAO 3V3 pin with D0 left open. Flashing this firmware in that state does not cut power, because D0 is open. Powersave cuts the rail only after VCC comes only from the drain.

The module has its own regulator. The 5V pin is USB VBUS and is not a 5 V output on battery.

The ceramic patch must face the sky. Face-down, next to a T1000-E that had 18 satellites and a fix, this module sat on 3–4 satellites and never solved a position. Turning the patch up produced the lock.

## Firmware pins

Flags live only on `[env:Xiao_nrf52_companion_radio_ble]` in `variants/xiao_nrf52/platformio.ini`:

- `ENV_INCLUDE_GPS=1`
- `PIN_GPS_TX=D7`, `PIN_GPS_RX=D6` (`Uart::setPins` takes RX, then TX, so `PIN_GPS_TX` is the MCU RX pin)
- `PIN_GPS_EN=D0`, `PIN_GPS_EN_ACTIVE=LOW`
- `GPS_BAUD_RATE=9600`
- `ENV_SKIP_GPS_DETECT=1`
- `GPS_RELEASE_UART_WHEN_OFF=1`
- `PIN_WIRE_SDA=17`, `PIN_WIRE_SCL=16` (onboard sensor bus, P0.07 / P0.27)

`build_unflags` removes the base `PIN_WIRE_SCL=D6`, `PIN_WIRE_SDA=D7`, and `PIN_USER_BTN=PIN_BUTTON1`. Do not `-U` those macros. PlatformIO emits every `-U` after every `-D`, so `-U` deletes the replacement defines and Wire comes up with no pins. D0 is `PIN_BUTTON1`; leaving the button defined puts a pull-up on the GPS enable pin.

LoRa stays on D1–D5 and D8–D10. SPI is SPIM3, Wire is TWIM0, and the GPS UART on D6/D7 is fine beside them.

9600 is the rate that parsed. A wrong baud cannot produce a satellite count. Do not change it unless new NMEA stops parsing.

## Power control

The app setting is `gps` / `gps_policy`: `off`, `powersave` (default), or `on`. There is no `position.power_pin`.

- `on` holds D0 low.
- `off` holds D0 high and refuses leases.
- `powersave` drives D0 low only during a lease or location poll.

CLI `gps on` / `gps off` is not policy-aware.

D0 only drives the MOSFET gate. It does not source the module.

## How to read the app

Location stays 0,0 until `MicroNMEA` reports a valid fix (GGA quality `1`–`5`, or RMC status `A`). 0,0 is the empty saved position, not a GPS coordinate.

The generic sensor (`0x64`) is `satellites × 1000000 + HHMMSS` UTC. The clock digits stay 0 until the fix is valid and the time is plausible (year 2020 or later).

| Reading | Meaning |
| --- | --- |
| `3000000` | 3 satellites, no fix. UART, baud, and NMEA parsing are working. |
| `4000000` | 4 satellites, still searching. |
| `5061422` | 5 satellites at 06:14:22 UTC, and a real position. |

## Boot notes that had to stay

`gpsUartRelease()` writes one byte and `flush()` before `Serial1.end()` on nRF52, and skips `end()` when `Serial1` is already stopped. Adafruit `Uart::end()` waits forever for `EVENTS_TXSTOPPED` if no TX was ever started, which wedges the node at the first BLE connection.

`initBasicGPS()` opens the UART, powers the enable pin, waits 1 s, forces `gps_detected` (`ENV_SKIP_GPS_DETECT`), then `stop_gps()` so the UART is released until a lease or `gps_policy on` starts it again. With the P-channel fitted, that probe drives D0 low for the 1 s delay (module on) and `stop_gps()` then drives D0 high (module off).
