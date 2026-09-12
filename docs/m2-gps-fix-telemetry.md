# M2 GPS fix telemetry — firmware interface

M2 (`aae9f5f3`, extended by `461ee7cf`) adds two CayenneLPP entries to the
telemetry reply on channel 1 (`TELEM_CHANNEL_SELF`), reporting the node's own
GPS-fix health. This doc is the interface contract for decoder authors (watch
app, stock app, anything parsing `PACKET_TELEMETRY_RESPONSE`). The normative
protocol description lives in `docs/companion_protocol.md` ("Telemetry
Payload"); this file records the encoding details and per-board differences
that the protocol doc does not.

## Interface

Single shared emitter, `src/helpers/sensors/GpsTelemetry.h`
`addGpsFixTelemetry(telemetry, location, gps_active)`:

| Entry | Type | Payload on the wire | Emit condition |
|---|---|---|---|
| Fix time | UnixTime `0x85` | 4 B big-endian **uint32** seconds since epoch | `location->isValid()` **and** timestamp ≥ 2020-01-01 — **even when GPS is asleep**, so a cached position carries its age |
| Sats + clock | GenericSensor `0x64` | 4 B big-endian **uint32**, `sats × 10⁶ + HHMMSS` — e.g. `12170330` = 12 sats at 17:03:30 UTC | GPS active, **or** a cached fix exists while asleep (count reads 0 then) |

Exactly one `0x64` entry, never two. Wire order on channel 1 is `0x85` then
`0x64`. Nothing is emitted when the provider is null or GPS is asleep with no
fix ever.

"Stamp absent" means no plausible fix at emit (searching, or never fixed);
"count reads 0" with a stamp present means the position is cached — judge its
age from the stamp. Both-absent means provider null, GPS off with no cached
fix, or pre-M2 firmware.

Order on channel 1 is voltage (`0x74`), GPS (`0x88`), then `0x85`, `0x64`
(the diagnostics ride the same `querySensors` call, right after `addGPS`).
Environment sensors start at channel 2, so channel-1 `0x64` is unambiguous —
but note `0x64` on channels ≥ 2 is gas resistance / IAQ, never this value.

## Why one packed entry, not two

The satellite count and the clock were originally two separate `0x64` entries.
Companion UIs that key LPP entries by `(channel, type)` **collapse duplicate
types**, so the second entry never rendered — the app showed one "Generic
Sensor" row and the clock was invisible on the wire. Packing both facts into a
single value keeps one entry per type, which every consumer renders.

Decode: `sats = value / GPS_TELEM_CLOCK_MODULUS`, `clock = value % modulus`
(`GPS_TELEM_CLOCK_MODULUS` = `1000000`). The clock is always `0..235959`, so it
can never carry into the satellite field. A clock of `0` means either 00:00:00
UTC or no plausible fix — those are indistinguishable, which is deliberate:
`0x85` carries the machine-readable truth and this entry is for at-a-glance
field reading.

## The `0x64` encoding trap (decoder authors read this)

The call site reads `addGenericSensor(channel, (float) value)`, which looks
like a float payload. It is not. The CayenneLPP library's templated `addField`
(`CayenneLPP.cpp`, `LPP_GENERIC_SENSOR_MULT 1`) multiplies the value by 1 and
writes the **raw uint32 bytes** — the `float` parameter never becomes IEEE-754
bits on the wire. Decoders **must** read `0x64` as big-endian uint32, then
split off the packed fields.

The historical trap: while `0x64` carried a bare count, 25 sats hit the wire as
`00 00 00 19`. A decoder reading float bits sees ~3.5e-44 and truncates to 0 —
every real count decoded as 0. This actually happened (watch app, 2026-09-12):
the "LR1110 always reports 0" story was a decoder bug, and the stock app
(uint32 decode) showed the live 25 all along. Any new decoder should
regression-test with a raw uint32 payload it reads correctly.

## The packed clock (decoder authors read this)

The low six digits of `0x64` are the fix wall clock as `HH*10000 + MM*100 + SS`
(`gpsFixTimeHhmmss`), e.g. 170330 for 17:03:30 — for field diagnosis in apps
that render LPP numerics raw, where a unix stamp costs mental arithmetic.
Derived from the same unix timestamp as `0x85` (no extra provider interface),
present under the same gate. **UTC always** — the node has no timezone, so
don't read local time into it. Range 0..235959; midnight is 0.

## The `0x85` clock (decoder authors read this)

`getTimestamp()` is built from the most recently parsed NMEA date+time fields
(`MicroNMEALocationProvider.h`), or the u-blox epoch on that path. It is the
receiver's **live GNSS clock**, not a "fix solved at" marker: a locked receiver
re-parses sentences continuously, so the value tracks real time within a
second. When the receiver goes quiet, the fields — and `isValid()`, which
latches (reassigned only on new sentences, never aged out) — freeze. A stale
stamp beside a live-looking `isValid` means the receiver stopped producing, so
consumers must judge by stamp **age**, never by stamp presence.

## Board differences

All boards share `addGpsFixTelemetry`; they differ in provider, position-row
gating, and what feeds the count.

| Board family | Provider | Position row (0x88) gating | Sat count source |
|---|---|---|---|
| 7 variant managers (t1000-e, meshtracker_x1, thinknode_m1, meshadventurer, nano_g2_ultra, heltec_mesh_solar, heltec_tracker) | `MicroNMEALocationProvider` over serial NMEA | Emitted on location permission alone — present even with GPS off | GGA field 7 via `nmea.getNumSatellites()` (GSV never contributes) |
| Shared `EnvironmentSensorManager` (ESP32 boards, u-blox/RAK12500 path) | `RAK12500LocationProvider` (`getSIV(2)`, `_sats` zeroed when no fix since M2) | Gated on `gps_active` (`querySensors` checks it) | `getSIV(2)`; `_sats` reset to 0 when the fix drops, so no stale last-fix count |

Two consequences for decoders:

1. **Position-row presence means different things per board.** On the t1000-e a
   position row with no diagnostics means GPS is off; on ESP32 boards the same
   wire state means pre-M2 firmware (their row is `gps_active`-gated). Decoders
   that cannot identify the board must not assert either cause.
2. **t1000-e GNSS is the LR1110** (`variants/t1000-e/target.cpp`, `MODE_GNSS`),
   a snapshot/assisted part. Its GGA sats-in-use field is live (observed 25 on
   clear sky, dropping under cover) — the earlier "sends 0 regardless" claim
   was the uint32/float decode bug above, not the hardware.

## Tests

`test/test_gps_telemetry/` (9 host-side cases against a recording `CayenneLPP`
mock in `test/mocks/CayenneLPP.h`) covers the emit conditions and the packing:
the HHMMSS known vector (17:03:30 UTC ⇒ 170330) and that the clock never
reaches the modulus, awake-with-fix ⇒ `0x85` + packed `(12, 170330)`,
awake-without-fix ⇒ packed `(7, 0)`, implausible clock ⇒ `(3, 0)` not garbage,
asleep-with-cached-fix ⇒ `0x85` + packed `(0, 170330)`, asleep-without-fix and
null provider ⇒ nothing, and channel-1-only. Note the mock records calls, not
bytes — a decoder-side regression test with raw wire bytes lives with each
consumer, not here.

## GPS leases (remote power control)

`MyMesh.cpp` holds up to 4 per-prefix leases (`gpsLeases`):
`!gps on` (favourite-only DM) arms a poll-renewed lease — refreshed by every
LOC telemetry poll from that requester, expires 5 min after the last poll;
`!gps N` keeps a fixed N-minute window; `!gps off` clears. A permissioned
LOC telemetry request with **no** existing lease now auto-arms a renewable
one (same 5-min window) instead of answering from a sleeping GPS — one-shot
polls from apps just work, no `!gps` DM needed first. Fixed-window leases
are never overwritten by polls. Expiry is swept in `updateGpsLeases()`;
with no leases left, `reconcileGpsFromLeases` sleeps the hardware.

Slot 0 (`LOCAL_GPS_LEASE_SLOT`) is reserved for the LOCAL lease: the watch's
own self-telemetry polls (`CMD_SEND_TELEMETRY_REQ`, len 4) renew it via
`renewLocalGpsLease()`, so local GPS lingers warm for 5 min after the last
poll instead of cutting off on an immediate `gps:0`. Remote triggers and the
auto-arm path never take or evict slot 0 (they scan from slot 1 and evict
slot 1 when full). The watch no longer sends `gps:0` at all, so that path is
no longer load-bearing for the watch; pre-lease firmware is out of scope.

The node-side guard in `handleCmdFrame` (`CMD_SET_CUSTOM_VAR`) is
deliberately **kept**: a `gps:0` arriving while any lease is active is
ignored rather than clearing `_prefs.gps_enabled`. That is the only backwards
compatibility retained, and it is what stops a build that still sends `gps:0`
from cutting a live lease short.

Why this exists: the receiver cold-starts on every session when the watch
cuts power immediately, so reacquire takes longest exactly when the wearer
just asked for position. A lingering lease keeps the almanac warm across
closely-spaced sessions at the cost of up to 5 min of extra GPS power after
each use.

## GPS state on the status LED (t1000-e)

The t1000-e's single status LED (`PIN_STATUS_LED=24`, `LED_STATE_ON=HIGH`)
normally gives one 20 ms flash per 4 s heartbeat cycle (`ui-orig`). While the
node's GPS is powered the heartbeat **double-pulses** — on at 0–20 ms, dark
gap 160 ms, on again 180–200 ms, dark for the rest of the cycle — so GPS state
is readable on any cycle, not only at the instant it is switched on.

- Only boards on the single-LED path are affected (`#elif defined(PIN_STATUS_LED)`
  in `ui-orig/UITask.cpp`); RGB boards (`STATUS_LED_RGB`) are untouched.
- GPS state is read from `SensorManager::getSettingByKey("gps")`, the same
  `"1"`/`"0"` value `T1000SensorManager::getSettingValue` exposes, so no new
  coupling to board internals.
- Unread messages keep their long single 200 ms flash and take priority; the
  double beat only appears on the idle cycle.
- This supersedes the earlier transient double-flash inside
  `T1000SensorManager::start_gps()` (commit `4985e7dd`): it fired only at the
  moment of enable, was easy to miss, and blocked the command handler for
  480 ms.
