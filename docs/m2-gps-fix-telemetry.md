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
| Fix time | UnixTime `0x85` | 4 B big-endian **uint32** seconds since epoch | Live while the provider holds a valid fix with a plausible clock; otherwise the **remembered** fix time — emitted whenever the live or remembered clock is ≥ 2020-01-01, **including while GPS is asleep or searching** (the stamp's age is the liveness signal) |
| Sats + clock | GenericSensor `0x64` | 4 B big-endian **uint32**, `sats × 10⁶ + HHMMSS` — e.g. `12170330` = 12 sats at 17:03:30 UTC | Live values under a live fix, otherwise the **remembered** `sats × 10⁶ + HHMMSS`; the live count with clock `0` while active with no lock and nothing remembered |
| Position | GPS `0x88` | lat/lon/alt, caller's row | On both families: the live row while `gps_active`; the **remembered** position (`addCachedGpsPosition`) while off, when a fix was ever cached. The 7 MicroNMEA variants also emit the row on location permission alone (their upstream shape), so a never-fixed node there can send `(0,0)` — pre-existing, not this feature |

At most one `0x88`, one `0x85`, one `0x64` per reply. Wire order on channel 1 is
`0x88` then `0x85` then `0x64`. Reply shapes:

| State | `0x88` | `0x85` | `0x64` |
|---|---|---|---|
| live fix | live row | live stamp | live `sats × 10⁶ + HHMMSS` |
| fix remembered, no live fix | remembered row | remembered stamp | remembered `sats × 10⁶ + HHMMSS` |
| active, no lock, nothing remembered | live row (variants always; ESM while `gps_active`) | — | live count, clock `0` |
| valid position, clock never plausible | remembered row | — | live count, clock `0` |
| never fixed and asleep | variants only — `(0,0)` row on location permission | — | — |

"Stamp absent" means the node never held a fix or a plausible clock (searching
with nothing remembered, or never fixed) — **not** "GPS is asleep". A positive
count beside an aged stamp means *last-known*, not currently tracking; judge by
the stamp's age. Both-absent means never fixed and asleep, or pre-M2 firmware.
"Count reads 0" with a stamp present is legacy M2 firmware behaviour
(pre-memory); current builds keep the remembered count.

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

All boards share `addGpsFixTelemetry` plus the per-provider last-known-fix
memory; they differ in provider, position-row gating, and what feeds the count.

| Board family | Provider | Position row (0x88) gating | Sat count source |
|---|---|---|---|
| 7 variant managers (t1000-e, meshtracker_x1, thinknode_m1, meshadventurer, nano_g2_ultra, heltec_mesh_solar, heltec_tracker) | `MicroNMEALocationProvider` over serial NMEA | Emitted on location permission alone — present even with GPS off | GGA field 7 via `nmea.getNumSatellites()` (GSV never contributes) |
| Shared `EnvironmentSensorManager` (ESP32 boards, u-blox/RAK12500 path) | `RAK12500LocationProvider` (`getSIV(2)`); memory cached on `isValid()` | `addCachedGpsPosition` emits the remembered position when off, so the row is present too | Memory keeps the count from the last valid fix; `_sats` resets to 0 live when the fix drops |

Two consequences for decoders:

1. **Position-row presence no longer distinguishes on/off, but diagnostics do.**
   Both families emit the remembered position row when the receiver is off, so a
   position row carrying remembered coordinates with no diagnostics means
   pre-memory firmware; presence vs absence of `0x85`/`0x64` is the version
   signal. Two current exceptions carry a row without diagnostics: a never-fixed
   variant node (location permission alone emits its `(0,0)` row), and an asleep
   fix remembered *without* a plausible clock (`addCachedGpsPosition` emits the
   row, `addGpsFixTelemetry` returns early). (Historic note: builds before the
   fix-memory change did differ here — t1000-e emitted the row on location
   permission alone while ESP32 boards gated it on `gps_active` — so the hedge
   "No fix held yet, or this node doesn't report health" stays honest for old
   firmware.)
2. **t1000-e GNSS is the LR1110** (`variants/t1000-e/target.cpp`, `MODE_GNSS`),
   a snapshot/assisted part. Its GGA sats-in-use field is live (observed 25 on
   clear sky, dropping under cover) — the earlier "sends 0 regardless" claim
   was the uint32/float decode bug above, not the hardware.

## Tests

`test/test_gps_telemetry/` (19 host-side cases against a recording `CayenneLPP`
mock in `test/mocks/CayenneLPP.h`) covers the emit conditions, the packing, and
the last-known-fix memory: the HHMMSS known vector (17:03:30 UTC ⇒ 170330) and
that the clock never reaches the modulus, awake-with-fix ⇒ `0x85` + packed
`(12, 170330)`, awake-without-fix ⇒ packed `(7, 0)`, implausible clock ⇒
`(3, 0)` not garbage, asleep-with-cached-fix ⇒ `0x85` + packed `(9, 170330)`
(the count is retained, not zeroed), searching-after-fix ⇒ the remembered
`0x85` + `(12, 170330)`, cached-position units and the never-fixed no-row case,
`gpsFixAvailable` (memory-backed, false when never fixed) and that a read does
not claim a slot, per-provider memory isolation and the >2-provider eviction
policy, asleep-without-fix and null provider ⇒ nothing, and channel-1-only. Note
the mock records calls, not bytes — a decoder-side regression test with raw wire
bytes lives with each consumer, not here. All cases run under a fixture whose
`SetUp()` resets the memory, because the tests use stack providers and the slots
are keyed on provider identity.

## GPS leases (remote power control)

`MyMesh.cpp` holds up to 4 per-prefix leases (`gpsLeases`):
`!gps on` (favourite-only DM) arms a poll-renewed lease — refreshed by every
LOC telemetry poll from that requester, expires 5 min after the last poll;
`!gps N` keeps a fixed N-minute window; `!gps off` clears. A permissioned
LOC telemetry request with **no** existing lease now auto-arms a renewable
one (same 5-min window) instead of answering from a sleeping GPS — one-shot
polls from apps just work, no `!gps` DM needed first. Fixed-window leases
are never overwritten by polls. Expiry is swept in `updateGpsLeases()`;
with no leases left, `reconcileGpsFromLeases` sleeps the hardware unless the
policy is `on` (see `gps_policy` below).

Slot 0 (`LOCAL_GPS_LEASE_SLOT`) is reserved for the LOCAL lease: the watch's
own self-telemetry polls (`CMD_SEND_TELEMETRY_REQ`, len 4) renew it via
`renewLocalGpsLease()`, so local GPS lingers warm for 5 min after the last
poll instead of cutting off on an immediate `gps:0`. Remote triggers and the
auto-arm path never take or evict slot 0 (they scan from slot 1 and evict
slot 1 when full). The watch sends `gps:0` only as an explicit ask (the
force-off compensation or the wearer's "turn node GPS off"); standing intent
travels as `gps_policy` below.

Under `powersave`, a `gps:0` arriving while any lease is active is ignored
rather than powering the receiver down or clearing `_prefs.gps_enabled` — the
retained backwards compatibility that stops a build which still sends `gps:0`
from cutting a live lease short. Under `on` or `off`, explicit `gps:0`
applies immediately.

## `gps_policy` (persistent intent)

Companion builds with `ENV_INCLUDE_GPS=1` append `,gps_policy:<value>` to the
`CMD_GET_CUSTOM_VARS` reply whenever the settings they enumerated included a
readable `gps` key (and the reply's 140-char body budget still allows it), and
they accept `SET_CUSTOM_VAR "gps_policy" "<value>"` with `off|powersave|on`;
the value persists in `NodePrefs.gps_policy` (JSON key `pol`). So the pair
`gps` + `gps_policy` is the companion's "Full capability" signal, and a board
whose receiver was never detected (an ESP32/nRF build without a GPS module)
advertises neither key and stays remote-GPS-uncontrolled; the t1000-e always
exposes `gps`. `gps:0|1` remains the transient power token.

| Policy | sweep on lease-zero | `gps:0` (down-ask) | `gps:1` (up-ask) | new leases |
|---|---|---|---|---|
| `on` | never sleeps | applied now (re-arm needs a later request) | applied | taken |
| `powersave` (default; also the no-key value for existing nodes) | sleeps | ignored while a lease is active, else applied | applied | taken |
| `off` | sleeps regardless of leases | applied regardless of leases | applied (explicit command) | refused (`!gps`, LOC polls, self-telemetry) |

A `gps_policy=off` write itself powers the receiver down and clears every
lease (a reconnect sync sends no `gps:0` to hang it on, and a hold that
survived `off` would block a later `powersave` flip's down-ask), and it is
authoritative when it comes from a second client: the connected watch's next
asks are refused and its reply carries the off receiver, rather than the two
writers fighting. A `gps_policy=on` write powers it up (the mirror of `off`);
a `gps_policy=powersave` write releases power instead — with no lease live it
sleeps the receiver and clears `gps_enabled`, because that is the only chance
to (the sweep runs only when a lease expires) and the powersave boot fallback
would otherwise re-arm it after every reboot, which is how leaving Always on
used to leave the radio running forever. With a lease live it releases
nothing: the lease is the standing reason for power, so power and the mirror
are left as they are and the expiry sleep ends it — which means a reboot
inside such a window still arms through the powersave fallback until the next
unleased policy write (a watch connect sync does one). Explicit power asks
always win: on the BLE companion protocol the `'gps'` sensor setting is
reachable through `SET_CUSTOM_VAR` (`0x29`, `key:value`), the same command
MeshCoreKmp exposes as `DeviceConnection.setCustomVar`; the exact phone-app UI
control that issues it is client-specific (unverified — confirm first). Under
`off`, therefore, a client's bare `gps:1` arms the receiver, while `off` still
refuses leases and boot-time power. That armed state lapses at the next reboot
or `gps_policy=off` re-assert unless the client writes the policy, and only
`gps_policy=on` is the durable override for an armed receiver: a
`gps_policy=powersave` write releases it instead (it powers down immediately
when no lease is live, see above). Boot derives power from the policy via
`applyGpsPrefs()`: `on` arms even when `gps_enabled=0`, `off` sleeps even when
it is 1, `powersave` falls back to `gps_enabled`. `gps_enabled` keeps being
persisted as the legacy power mirror (node UI screens read it) but is no
longer intent. Explicit on-device writes (node UI toggle, `CommonCLI`
`gps on|off` on text-CLI builds) are not policy-aware — a person at the node
outranks remote intent, and the next policy write or boot re-asserts the
policy.

Why this exists: the receiver cold-starts on every session when the watch
cuts power immediately, so reacquire takes longest exactly when the wearer
just asked for position. A lingering lease keeps the almanac warm across
closely-spaced sessions at the cost of up to 5 min of extra GPS power after
each use.

## GPS state on the status LED (t1000-e)

The t1000-e's single status LED (`PIN_STATUS_LED=24`, `LED_STATE_ON=HIGH`) has
one colour, so GPS state is reported by **pulse count** in the 4 s `ui-orig`
heartbeat:

| Pulses | Meaning |
|---|---|
| 1 | GPS off |
| 2 | GPS powered, no fix yet |
| 3 | GPS powered, fix held |

Pulses are 20 ms lit with a 160 ms dark gap between them, and the trailing dark
period is computed from what the beat actually consumed, so the cycle stays 4 s.
The beat is sampled as it starts, so its shape cannot change part way through.
GPS state is therefore readable on any cycle, not only at the instant GPS is
switched on.

- Only boards on the single-LED path are affected (`#elif defined(PIN_STATUS_LED)`
  in `ui-orig/UITask.cpp`); RGB boards (`STATUS_LED_RGB`) are untouched and have
  no equivalent yet.
- Both inputs come from the generic `SensorManager` interface: GPS enabled from
  `getSettingByKey("gps")` (the same `"1"`/`"0"` `T1000SensorManager::getSettingValue`
  exposes), lock state from `getLocationProvider()->isValid()` — MicroNMEA's GGA
  fix-quality flag (`'1'`–`'5'` = valid, `MicroNMEA.cpp:323`), the same signal the
  t1000-e target already uses to publish position. So no new coupling to board
  internals.
- Caveat: `isValid()` only refreshes while sentences are being parsed. A receiver
  that goes quiet leaves its last value in place, so a hung module can read as
  "fix held". Acceptable for an indicator; it is the same staleness that makes it
  unusable as a freshness signal over the air.
- Unread messages keep their long single 200 ms flash and take priority; the
  GPS counts only appear on the idle cycle.
- This supersedes the transient double-flash inside
  `T1000SensorManager::start_gps()` (commit `4985e7dd`): it fired only at the
  moment of enable, was easy to miss, and blocked the command handler for
  480 ms.
