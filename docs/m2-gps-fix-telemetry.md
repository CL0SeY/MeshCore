# M2 GPS fix telemetry — firmware interface

M2 (`aae9f5f3`) adds two CayenneLPP entries to the telemetry reply on channel 1
(`TELEM_CHANNEL_SELF`), reporting the node's own GPS-fix health. This doc is the
interface contract for decoder authors (watch app, stock app, anything parsing
`PACKET_TELEMETRY_RESPONSE`). The normative protocol description lives in
`docs/companion_protocol.md` ("Telemetry Payload"); this file records the
encoding details and per-board differences that the protocol doc does not.

## Interface

Single shared emitter, `src/helpers/sensors/GpsTelemetry.h`
`addGpsFixTelemetry(telemetry, location, gps_active)`:

| Entry | Type | Payload on the wire | Emit condition |
|---|---|---|---|
| Fix time | UnixTime `0x85` | 4 B big-endian **uint32** seconds since epoch | `location->isValid()` **and** timestamp ≥ 2020-01-01 — **even when GPS is asleep**, so a cached position carries its age |
| Sats in use | GenericSensor `0x64` (first) | 4 B big-endian **uint32** count | GPS active (`gps_active`, provider non-null) |
| Fix clock | GenericSensor `0x64` (second) | 4 B big-endian **uint32** HHMMSS, UTC, e.g. 170330 | Same gate as the stamp (valid fix + plausible clock) |

Wire order on channel 1 is always `0x85`, `0x64` (sats), `0x64` (HHMMSS).
When GPS is asleep with a cached fix, the layout is `0x85`, `0x64` (**0**
placeholder), `0x64` (HHMMSS) — the zero holds the first-`0x64` slot so
decoders that take the first `0x64` as sats keep reading correctly. When
GPS is asleep with no fix ever, nothing is emitted.

Both-absent still means: provider null, GPS off with no cached fix, or
pre-M2 firmware. "Count present, stamp absent" (GPS on, searching) is
unchanged. New state: "stamp present, count zero" means the position is
cached — judge its age from the stamp.

Order on channel 1 is voltage (`0x74`), GPS (`0x88`), then `0x85`, `0x64`
(the diagnostics ride the same `querySensors` call, right after `addGPS`).
Environment sensors start at channel 2, so channel-1 `0x64` is unambiguous —
but note `0x64` on channels ≥ 2 is gas resistance / IAQ, never a sat count.

## The `0x64` encoding trap (decoder authors read this)
The call site reads `addGenericSensor(channel, (float) count)`, which looks
like a float payload. It is not. The CayenneLPP library's templated `addField`
(`CayenneLPP.cpp`, `LPP_GENERIC_SENSOR_MULT 1`) multiplies the value by 1 and
writes the **raw uint32 bytes** — the `float` parameter never becomes IEEE-754
bits on the wire. Decoders **must** read `0x64` as big-endian uint32.

Concretely: 25 sats hits the wire as `00 00 00 19`. A decoder reading float
bits sees ~3.5e-44 and truncates to 0 — every real count decodes as 0. This
actually happened (watch app, 2026-09-12): the "LR1110 always reports 0" story
was a decoder bug, and the stock app (uint32 decode) showed the live 25 all
along. Any new decoder should regression-test with raw `00 00 00 19` ⇒ 25.

## The HHMMSS clock (decoder authors read this)

The second channel-1 `0x64` is the fix wall clock as `HH*10000 + MM*100 + SS`
(`gpsFixTimeHhmmss`), e.g. 170330 for 17:03:30 — for field diagnosis in apps
that render LPP numerics raw, where a unix stamp costs mental arithmetic.
Derived from the same unix timestamp as `0x85` (no extra provider interface),
emitted under the same gate. **UTC always** — the node has no timezone, so
don't read local time into it. Range 0..235959; midnight is 0.

Two `0x64` entries share channel 1, distinguished by **order**: sats first,
clock second. Decoders take the first as sats and ignore (or display) the
second. The watch decoder's `satelliteCount == null` guard does exactly this
and needs no change. Stock apps show two "Generic Sensor" rows: count, then
clock.

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

`test/test_gps_telemetry/` (11 host-side cases against a recording
`CayenneLPP` mock in `test/mocks/CayenneLPP.h`) covers the emit conditions:
`0x85` on valid fix + plausible clock **even with GPS asleep**, `0x64` sats
whenever active, zero-placeholder + HHMMSS while asleep with a cached fix,
nothing when asleep with no fix, the HHMMSS known vector (17:03:30 UTC ⇒
170330), and the sats-before-HHMMSS wire order in both layouts. Note the mock
records calls, not bytes — a decoder-side regression test with raw wire bytes
(e.g. `00 00 00 19` ⇒ 25) lives with each consumer, not here.

## GPS leases (remote power control)

Unchanged by the above, recorded here because it governs when the entries
flow. `MyMesh.cpp` holds up to 4 per-prefix leases (`gpsLeases`):
`!gps on` (favourite-only DM) arms a poll-renewed lease — refreshed by every
LOC telemetry poll from that requester, expires 5 min after the last poll;
`!gps N` keeps a fixed N-minute window; `!gps off` clears. A permissioned
LOC telemetry request with **no** existing lease now auto-arms a renewable
one (same 5-min window) instead of answering from a sleeping GPS — one-shot
polls from apps just work, no `!gps` DM needed first. Fixed-window leases
are never overwritten by polls. The watch's `gps:0` (`setCustomVar`) is
ignored while any lease is active. Expiry is swept in `updateGpsLeases()`;
with no leases left, `reconcileGpsFromLeases` sleeps the hardware.
