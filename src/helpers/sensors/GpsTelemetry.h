#pragma once

#include <helpers/SensorManager.h>
#include <helpers/sensors/LocationProvider.h>

// Earliest fix time we will report: 2020-01-01. The node has no trusted notion
// of "now" to compare against, so this only rejects zero/unset provider clocks.
#define GPS_FIX_TIME_MIN_UNIX 1577836800L

inline bool gpsFixTimeIsPlausible(long timestamp) {
  return timestamp >= GPS_FIX_TIME_MIN_UNIX;
}

// HHMMSS wall clock (UTC) derived from a unix timestamp, e.g. 170330 for
// 17:03:30. The node has no timezone — UTC always. Valid range 0..235959.
inline uint32_t gpsFixTimeHhmmss(long timestamp) {
  long daySecs = timestamp % 86400L;
  if (daySecs < 0) daySecs += 86400L;
  uint32_t hh = (uint32_t)(daySecs / 3600L);
  uint32_t mm = (uint32_t)((daySecs % 3600L) / 60L);
  uint32_t ss = (uint32_t)(daySecs % 60L);
  return hh * 10000u + mm * 100u + ss;
}

// The channel-1 generic-sensor diagnostic packs BOTH facts into one entry:
// satellite count in the high digits, fix wall clock (UTC) in the low six —
// e.g. 12170330 = 12 sats at 17:03:30. One 0x64 entry rather than two, because
// companion UIs that key LPP entries by (channel, type) collapse duplicates
// and hide the second one.
//
// The clock is 0 when the provider holds no plausible fix, so 00:00:00 UTC is
// indistinguishable from "no clock". That is cosmetic: 0x85 carries the
// machine-readable stamp, and this entry exists for at-a-glance field reading.
#define GPS_TELEM_CLOCK_MODULUS 1000000u

inline uint32_t gpsFixTelemetryValue(int satellites, long timestamp, bool has_clock) {
  uint32_t clock = has_clock ? gpsFixTimeHhmmss(timestamp) : 0u;
  return (uint32_t)satellites * GPS_TELEM_CLOCK_MODULUS + clock;
}

// Emits the GPS fix-metadata entries for the node's own position onto
// TELEM_CHANNEL_SELF:
//   - a Cayenne LPP unix-time stamp (0x85) when the provider holds a valid fix
//     with a plausible clock — even when GPS is asleep, so a cached position
//     carries its age instead of looking live;
//   - the packed satellite count + clock (0x64) whenever GPS is active, or
//     when a cached fix exists while asleep (count reads 0 then).
// The position row itself stays with the caller, so each board keeps its own
// gating of the GPS entry.
//
// Shared by every board's SensorManager so a requester sees one layout.
inline void addGpsFixTelemetry(CayenneLPP& telemetry, LocationProvider* location, bool gps_active) {
  if (location == NULL) return;

  // The fix stamp is liveness evidence, not a power-state report: emit it
  // whenever the provider holds a plausible fix, even with GPS asleep, so a
  // cached position arrives with its age.
  long timestamp = location->getTimestamp();
  bool stampOk = location->isValid() && gpsFixTimeIsPlausible(timestamp);
  if (stampOk) {
    telemetry.addUnixTime(TELEM_CHANNEL_SELF, (uint32_t) timestamp);
  }
  if (!gps_active && !stampOk) return;  // asleep and never fixed: nothing to say

  // Sats-in-use is meaningless with the receiver off, so it reads 0 while asleep.
  int sats = gps_active ? (int) location->satellitesCount() : 0;
  telemetry.addGenericSensor(TELEM_CHANNEL_SELF,
                             (float) gpsFixTelemetryValue(sats, timestamp, stampOk));
}
