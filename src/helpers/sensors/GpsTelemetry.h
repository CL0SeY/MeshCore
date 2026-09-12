#pragma once

#include <helpers/SensorManager.h>
#include <helpers/sensors/LocationProvider.h>

// Earliest fix time we will report: 2020-01-01. The node has no trusted notion
// of "now" to compare against, so this only rejects zero/unset provider clocks.
#define GPS_FIX_TIME_MIN_UNIX 1577836800L

inline bool gpsFixTimeIsPlausible(long timestamp) {
  return timestamp >= GPS_FIX_TIME_MIN_UNIX;
}

// Emits the GPS fix-metadata entries for the node's own position onto
// TELEM_CHANNEL_SELF:
//   - a Cayenne LPP unix-time stamp (0x85) when the provider holds a valid
//     fix with a plausible clock — even when GPS is asleep, so a cached
//     position carries its age instead of looking live;
//   - the satellite count (0x64) whenever GPS is active;
//   - the fix clock as HHMMSS (0x64, second entry) under the same gate as
//     the stamp, for field diagnosis without converting unix time.
// The position row itself stays with the caller, so each board keeps its own
// gating of the GPS entry.
//
// Shared by every board's SensorManager so a requester sees one layout.
//
// Wire order on channel 1: 0x85, 0x64 (sats), 0x64 (HHMMSS). Decoders take
// the FIRST 0x64 as the sat count and must ignore later ones.
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

inline void addGpsFixTelemetry(CayenneLPP& telemetry, LocationProvider* location, bool gps_active) {
  if (location == NULL) return;

  // The fix stamp is liveness evidence, not a power-state report: emit it
  // whenever the provider holds a plausible fix, even with GPS asleep, so a
  // cached position arrives with its age. The sat count stays gated on
  // gps_active — sats-in-use is meaningless with the receiver off.
  long timestamp = location->getTimestamp();
  bool stampOk = location->isValid() && gpsFixTimeIsPlausible(timestamp);
  if (stampOk) {
    telemetry.addUnixTime(TELEM_CHANNEL_SELF, (uint32_t) timestamp);
  }
  if (!gps_active) {
    // Asleep: no live sat count — but a cached fix still carries its stamp
    // plus the human-readable clock, so the position arrives with its age.
    // The zero sat-count placeholder holds the first-0x64 slot (decoders
    // take the first 0x64 as sats), so HHMMSS is always the second 0x64.
    if (stampOk) {
      telemetry.addGenericSensor(TELEM_CHANNEL_SELF, (float) 0);
      telemetry.addGenericSensor(TELEM_CHANNEL_SELF, (float) gpsFixTimeHhmmss(timestamp));
    }
    return;
  }
  telemetry.addGenericSensor(TELEM_CHANNEL_SELF, (float) location->satellitesCount());
  if (stampOk) {
    telemetry.addGenericSensor(TELEM_CHANNEL_SELF, (float) gpsFixTimeHhmmss(timestamp));
  }
}
