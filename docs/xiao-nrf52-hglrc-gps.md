# XIAO nRF52840 + HGLRC GPS (BLE companion)

Working setup, confirmed 2026-11-01. The node reported a real position. The generic sensor read `5061422`: 5 satellites at 06:14:22 UTC.

Firmware is `Xiao_nrf52_companion_radio_ble` only. The repeater, USB companion, room server, and kiss modem builds do not include this GPS.

```
pio run -e Xiao_nrf52_companion_radio_ble
```

Artifacts: `MeshCore/.pio/build/Xiao_nrf52_companion_radio_ble/`.

## Wiring

| Module | XIAO |
| --- | --- |
| TX | D7 (silk **RX**, P1.12). This is the MCU receive pin. |
| RX | D6 (silk **TX**, P1.11). This is the MCU transmit pin. |
| GND | GND |
| VCC | **3V3** pin |

Leave D0 disconnected. Do not tie module VCC to D0 and 3V3 at the same time. D0 is driven low to turn the GPS off and will fight a hard 3V3 tie.

The module has its own regulator. This lock was taken from the XIAO 3V3 pin. The 5V pin is USB VBUS and is not a 5 V output on battery.

The ceramic patch must face the sky. Face-down, next to a T1000-E that had 18 satellites and a fix, this module sat on 3–4 satellites and never solved a position. Turning the patch up produced the lock.

## Firmware pins

Flags live only on `[env:Xiao_nrf52_companion_radio_ble]` in `variants/xiao_nrf52/platformio.ini`:

- `ENV_INCLUDE_GPS=1`
- `PIN_GPS_TX=D7`, `PIN_GPS_RX=D6` (`Uart::setPins` takes RX, then TX, so `PIN_GPS_TX` is the MCU RX pin)
- `PIN_GPS_EN=D0`, `PIN_GPS_EN_ACTIVE=HIGH`, `GPS_EN_HIGH_DRIVE=1`
- `GPS_BAUD_RATE=9600`
- `ENV_SKIP_GPS_DETECT=1`
- `GPS_RELEASE_UART_WHEN_OFF=1`
- `PIN_WIRE_SDA=17`, `PIN_WIRE_SCL=16` (onboard sensor bus, P0.07 / P0.27)

`build_unflags` removes the base `PIN_WIRE_SCL=D6`, `PIN_WIRE_SDA=D7`, and `PIN_USER_BTN=PIN_BUTTON1`. Do not `-U` those macros. PlatformIO emits every `-U` after every `-D`, so `-U` deletes the replacement defines and Wire comes up with no pins. D0 is `PIN_BUTTON1`; leaving the button defined puts a pull-up on the GPS enable pin.

LoRa stays on D1–D5 and D8–D10. SPI is SPIM3, Wire is TWIM0, and the GPS UART on D6/D7 is fine beside them.

9600 is the rate that parsed. A wrong baud cannot produce a satellite count. Do not change it unless new NMEA stops parsing.

## Power control

The app setting is `gps` / `gps_policy`: `off`, `powersave` (default), or `on`. There is no `position.power_pin`.

- `on` holds D0 high.
- `off` holds D0 low and refuses leases.
- `powersave` drives D0 high only during a lease or location poll.

CLI `gps on` / `gps off` is not policy-aware.

With VCC hard-wired to 3V3, D0 no longer switches the module. The UART still has to be running: `stop_gps()` releases it, and sentences stop. Keep `gps_policy` on, or keep the app connected so the local lease holds `gps` at 1.

D0 high-drive sources about 15 mA and sags under a searching module. It is an enable, not a supply. A permanent install should let D0 switch the 3V3 rail through an active-high load switch. A P-channel MOSFET with its gate tied straight to D0 turns the rail on when D0 is low, which is the opposite of this firmware.

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

`initBasicGPS()` opens the UART, powers the enable pin, waits 1 s, forces `gps_detected` (`ENV_SKIP_GPS_DETECT`), then `stop_gps()` so the UART is released until a lease or `gps_policy on` starts it again.
