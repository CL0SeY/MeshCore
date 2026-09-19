#pragma once

#include <helpers/SensorManager.h>
#include <helpers/sensors/LocationProvider.h>

// Earliest fix time we will report: 2020-01-01. The node has no trusted notion
// of "now" to compare against, so this only rejects zero/unset provider clocks.
// Note: there is deliberately no upper bound — a node-side wall clock to judge
// a forward-dated GNSS epoch against does not exist here. A forward stamp is
// therefore treated as live and still overwrites the memory; the watch's
// future-skew gate drops it on screen, and the next real fix re-caches.
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

// The node's last-known fix, cached from the provider the last time it held a
// valid position — the "first call works, then the values disappear" case: a
// poll re-powers the receiver, and until it re-locks the live provider reports
// no fix (MicroNMEA quality-0 GGA, or the u-blox path zeroing _fix/_sats).
//
// Cached on POSITION validity, not on clock plausibility: fix and epoch ride
// different provider branches (RAK12500 sets _fix/_lat/_lon/_alt under
// getGnssFixOk but writes _epoch unconditionally), so caching only under a
// live+plausible stamp would remember nothing during a lock-without-UTC
// window. The timestamp is kept only when plausible, so an unset/bad epoch
// never overwrites a good one. `hasFix` (not non-zero coordinates) marks a
// written slot — a fix at 0,0 is legitimate.
//
// Provider units are preserved (degrees×1e6 lat/lon, metres×1000 alt); the
// cached position emitter converts them exactly as the live row does.
struct GpsFixSnapshot {
  bool hasFix = false;
  long timestamp = 0;
  int satellites = 0;
  long lat = 0;
  long lon = 0;
  long alt = 0;
};

struct GpsFixMemory {
  const LocationProvider* location = nullptr;
  GpsFixSnapshot fix;
};

// The single live array. Both accessors go through here, so the test reset
// reaches the same array the emitter reads — never a second array.
inline GpsFixMemory* gpsFixMemorySlots() {
  static GpsFixMemory slots[2];
  return slots;
}

// Read-only lookup: the slot holding this provider, or null. Never claims a
// slot, so a read (gpsFixAvailable / addCachedGpsPosition) cannot evict data
// just by asking.
inline const GpsFixMemory* gpsFixMemoryFind(const LocationProvider* location) {
  if (location == nullptr) return nullptr;
  const GpsFixMemory* slots = gpsFixMemorySlots();
  for (int i = 0; i < 2; i++) {
    if (slots[i].location == location) return &slots[i];
  }
  return nullptr;
}

// Claim/return the slot for a provider: exact match, else the first unused
// slot, else slot 0. A slot is always re-keyed to the caller (and cleared on
// eviction), so a stale fix can never be read back under the previous owner.
// Firmware owns exactly one provider per build, so 2 is headroom, not a limit.
inline GpsFixMemory& gpsFixMemoryFor(const LocationProvider* location) {
  GpsFixMemory* slots = gpsFixMemorySlots();
  for (int i = 0; i < 2; i++) {
    if (slots[i].location == location) return slots[i];
  }
  for (int i = 0; i < 2; i++) {
    if (slots[i].location == nullptr) {
      slots[i].fix = GpsFixSnapshot();
      slots[i].location = location;
      return slots[i];
    }
  }
  slots[0].fix = GpsFixSnapshot();
  slots[0].location = location;
  return slots[0];
}

// Test hook: clears the array the emitter reads. Required because the host
// tests use stack providers and GTest reuses the stack region, so slots would
// alias across tests without it.
inline void gpsFixMemoryReset() {
  GpsFixMemory* slots = gpsFixMemorySlots();
  for (int i = 0; i < 2; i++) slots[i] = GpsFixMemory();
}

// True when the provider holds a live, plausibly-stamped fix — or a remembered
// one worth reporting (a first fix the node has never cached is not available).
inline bool gpsFixAvailable(LocationProvider* location) {
  if (location == NULL) return false;
  long timestamp = location->getTimestamp();
  if (location->isValid() && gpsFixTimeIsPlausible(timestamp)) return true;
  const GpsFixMemory* mem = gpsFixMemoryFind(location);
  return mem != nullptr && mem->fix.hasFix;
}

// The cached position row (0x88) from the memory. The LIVE row stays with the
// caller; only the memory knows the cached one. False + no emission when no
// plausible fix was ever remembered (a never-fixed node sends no position).
inline bool addCachedGpsPosition(CayenneLPP& telemetry, LocationProvider* location) {
  if (location == NULL) return false;
  const GpsFixMemory* mem = gpsFixMemoryFind(location);
  if (mem == nullptr || !mem->fix.hasFix) return false;
  telemetry.addGPS(TELEM_CHANNEL_SELF,
                   (float)(mem->fix.lat / 1000000.0), (float)(mem->fix.lon / 1000000.0),
                   (float)(mem->fix.alt / 1000.0));
  return true;
}

// Emits the GPS fix-metadata entries for the node's own position onto
// TELEM_CHANNEL_SELF:
//   - a Cayenne LPP unix-time stamp (0x85), live or remembered — the age signal,
//     so a cached position carries how old it is instead of looking live;
//   - the packed satellite count + clock (0x64): live values under a live fix,
//     the remembered `sats × 10⁶ + HHMMSS` while the receiver is off or
//     searching. A positive count under an aged stamp means last-known, not
//     currently tracking — judge by the stamp's age, never by the count.
//   - the live position — count + clock 0 — while active with no lock and no
//     remembered fix.
// Nothing is emitted when the provider is null or GPS is asleep with no fix
// ever remembered.
//
// Shared by every board's SensorManager so a requester sees one layout.
inline void addGpsFixTelemetry(CayenneLPP& telemetry, LocationProvider* location, bool gps_active) {
  if (location == NULL) return;

  long timestamp = location->getTimestamp();
  bool live = location->isValid() && gpsFixTimeIsPlausible(timestamp);

  // Cache on position validity, independently of the clock (see above); the
  // clock slot is only overwritten by a plausible epoch.
  GpsFixMemory& mem = gpsFixMemoryFor(location);
  if (location->isValid()) {
    mem.fix.hasFix = true;
    mem.fix.satellites = (int) location->satellitesCount();
    mem.fix.lat = location->getLatitude();
    mem.fix.lon = location->getLongitude();
    mem.fix.alt = location->getAltitude();
    if (gpsFixTimeIsPlausible(timestamp)) mem.fix.timestamp = timestamp;
  }

  bool hasStamp = live || (mem.fix.hasFix && gpsFixTimeIsPlausible(mem.fix.timestamp));
  if (!hasStamp && !gps_active) return;  // asleep and never fixed: nothing to say

  long stamp = live ? timestamp : mem.fix.timestamp;
  if (hasStamp) {
    telemetry.addUnixTime(TELEM_CHANNEL_SELF, (uint32_t) stamp);
  }

  // Retained, not live-zeroed, while there is something to remember: while
  // asleep the receiver reports nothing, so its last-known values are the
  // honest answer. Active with no lock and nothing remembered keeps the live
  // count (clock 0) — the "GPS active, no lock" signal.
  int sats = live ? (int) location->satellitesCount()
                  : (mem.fix.hasFix ? mem.fix.satellites : (int) location->satellitesCount());
  telemetry.addGenericSensor(TELEM_CHANNEL_SELF,
                             (float) gpsFixTelemetryValue(sats, stamp, hasStamp));
}
