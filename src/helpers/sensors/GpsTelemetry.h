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
// TELEM_CHANNEL_SELF: a Cayenne LPP unix-time stamp (0x85) only when the
// provider holds a valid fix with a plausible clock, and the satellite count
// (0x64) whenever GPS is active. The position row itself stays with the caller,
// so each board keeps its own gating of the GPS entry.
//
// Shared by every board's SensorManager so a requester sees one layout.
inline void addGpsFixTelemetry(CayenneLPP& telemetry, LocationProvider* location, bool gps_active) {
  if (location == NULL || !gps_active) return;

  long timestamp = location->getTimestamp();
  if (location->isValid() && gpsFixTimeIsPlausible(timestamp)) {
    telemetry.addUnixTime(TELEM_CHANNEL_SELF, (uint32_t) timestamp);
  }
  telemetry.addGenericSensor(TELEM_CHANNEL_SELF, (float) location->satellitesCount());
}
