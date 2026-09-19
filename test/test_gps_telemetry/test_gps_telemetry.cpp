// Host-side tests for addGpsFixTelemetry (GpsTelemetry.h).
//
// Verifies the gating logic (isValid, gps_active, plausibility), the packed
// sats+clock encoding, and the last-known-fix memory (remembered stamp, count
// and position after the provider stops reporting a fix) against the recording
// CayenneLPP mock. Does NOT prove the real wire bytes — that is verified
// empirically against the ElectronicCats library (see plan §wire-layout).
//
// NOTE: env:native resolves <helpers/SensorManager.h> against real src/Mesh.h
// (the -I src order precedes -I test/mocks). The sibling env:native_kiss_modem
// has its own include order and would not support this test.

#include <helpers/sensors/GpsTelemetry.h>

#include <gtest/gtest.h>
#include <vector>

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// LocationProvider declares sendSentence() but never defines it; the providers
// supply their own (firmware-only) overrides. Provide a minimal body here to
// satisfy the linker without polluting the public interface.
void LocationProvider::sendSentence(const char*) {}

class FakeLocationProvider : public LocationProvider {
public:
  long lat       = 0;
  long lon       = 0;
  long alt       = 0;
  long sats      = 0;
  long ts        = 0;
  bool fix       = false;

  long getLatitude()  override { return lat; }
  long getLongitude() override { return lon; }
  long getAltitude()  override { return alt; }
  long satellitesCount() override { return sats; }
  bool isValid()      override { return fix; }
  long getTimestamp() override { return ts; }
  void reset()        override {}
  void begin()        override {}
  void stop()         override {}
  void loop()         override {}
  bool isEnabled()    override { return true; }
};

// The memory is keyed on provider identity, and the tests use stack providers
// — so each test drains it first, or GTest's stack reuse would leak state
// (and memory) from an earlier case into a later one.
class GpsTelemetryFixture : public ::testing::Test {
protected:
  void SetUp() override { gpsFixMemoryReset(); }
};

// 2026-01-01 17:03:30 UTC — a timestamp with a known wall clock, so the
// arithmetic is guarded against a known vector rather than only against itself.
static const long TS_170330 = 1767287010L;

static void expectUnixTime(const CayenneLPP& lpp, uint32_t expected_ts) {
  bool found = false;
  for (const auto& c : lpp.calls) {
    if (c.type == 0x85) {
      ASSERT_EQ(c.channel, 1u);
      ASSERT_EQ(c.value, expected_ts);
      found = true;
    }
  }
  ASSERT_TRUE(found) << "expected unix-time entry with value " << expected_ts;
}

static void expectNoUnixTime(const CayenneLPP& lpp) {
  for (const auto& c : lpp.calls) {
    ASSERT_NE(c.type, 0x85) << "unexpected unix-time entry";
  }
}

// The single channel-1 0x64 entry packs sats in the high digits and the UTC
// wall clock in the low six. Exactly one such entry is expected: a second
// would be collapsed by companion UIs that key LPP by (channel, type).
static void expectPacked(const CayenneLPP& lpp, uint32_t sats, uint32_t hhmmss) {
  int seen = 0;
  for (const auto& c : lpp.calls) {
    if (c.type != 0x64) continue;
    seen++;
    EXPECT_EQ(c.channel, 1u);
    EXPECT_EQ(c.value, sats * GPS_TELEM_CLOCK_MODULUS + hhmmss)
        << "packed value should be sats*1e6 + HHMMSS";
  }
  EXPECT_EQ(seen, 1) << "expected exactly one packed 0x64 entry, saw " << seen;
}

TEST(GpsTelemetry, HhmmssKnownVector) {
  EXPECT_EQ(gpsFixTimeHhmmss(TS_170330), 170330u);
  EXPECT_EQ(gpsFixTimeHhmmss(1767225600L), 0u);        // midnight -> 0
  EXPECT_EQ(gpsFixTimeHhmmss(1767311999L), 235959u);   // 23:59:59 -> 235959
}

TEST(GpsTelemetry, ClockStaysBelowModulus) {
  // The clock must never carry into the satellite field.
  EXPECT_LT(gpsFixTimeHhmmss(1767311999L), GPS_TELEM_CLOCK_MODULUS);
}

TEST_F(GpsTelemetryFixture, AwakeWithFixPacksSatsAndClock) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = TS_170330;
  p.sats = 12;

  addGpsFixTelemetry(lpp, &p, true);

  EXPECT_EQ(lpp.calls.size(), 2u);   // 0x85 + packed 0x64
  expectUnixTime(lpp, 1767287010u);
  expectPacked(lpp, 12u, 170330u);   // 12170330
}

TEST_F(GpsTelemetryFixture, AwakeWithoutFixPacksSatsWithZeroClock) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = false;
  p.ts  = TS_170330;
  p.sats = 7;

  addGpsFixTelemetry(lpp, &p, true);

  EXPECT_EQ(lpp.calls.size(), 1u);   // no stamp, packed entry only
  expectNoUnixTime(lpp);
  expectPacked(lpp, 7u, 0u);
}

TEST_F(GpsTelemetryFixture, ImplausibleClockReadsZeroNotGarbage) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = 1577836799L;   // 1 second before 2020-01-01
  p.sats = 3;

  addGpsFixTelemetry(lpp, &p, true);

  EXPECT_EQ(lpp.calls.size(), 1u);
  expectNoUnixTime(lpp);
  expectPacked(lpp, 3u, 0u);
}

TEST_F(GpsTelemetryFixture, AsleepWithCachedFixKeepsStampAndClock) {
  // NOTE: this exercises the live-but-asleep path (a still-valid provider
  // while the receiver is off), not the reset scenario — the count is now
  // retained, not forced to 0. See SearchingAfterFixKeepsLastKnownReading for
  // the provider-stops-reporting case.
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;            // latched valid fix...
  p.ts  = TS_170330;
  p.sats = 9;

  addGpsFixTelemetry(lpp, &p, false);  // ...but GPS asleep

  EXPECT_EQ(lpp.calls.size(), 2u);
  expectUnixTime(lpp, 1767287010u);
  expectPacked(lpp, 9u, 170330u);
}

TEST_F(GpsTelemetryFixture, AsleepWithoutFixEmitsNothing) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = false;
  p.ts  = 0;
  p.sats = 0;

  addGpsFixTelemetry(lpp, &p, false);

  EXPECT_EQ(lpp.calls.size(), 0u);
}

TEST_F(GpsTelemetryFixture, NullProvider) {
  CayenneLPP lpp(64);

  addGpsFixTelemetry(lpp, nullptr, true);

  EXPECT_EQ(lpp.calls.size(), 0u);
}

TEST_F(GpsTelemetryFixture, ChannelOne) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = TS_170330;
  p.sats = 12;

  addGpsFixTelemetry(lpp, &p, true);

  for (const auto& c : lpp.calls) {
    EXPECT_EQ(c.channel, 1u) << "all entries must be on channel 1 (TELEM_CHANNEL_SELF)";
  }
}

TEST_F(GpsTelemetryFixture, SearchingAfterFixKeepsLastKnownReading) {
  // The reported scenario: a poll re-powers the receiver, and until it
  // re-locks the provider stops reporting a fix (RAK12500 clears _fix/_sats;
  // MicroNMEA serves a quality-0 GGA). The remembered stamp and count must
  // survive — the stamp's age, not its presence, marks the reading stale.
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = TS_170330;
  p.sats = 12;
  p.lat = 51123456L; p.lon = -1134567L; p.alt = 545000L;

  addGpsFixTelemetry(lpp, &p, true);

  // Receiver re-powered, still searching:
  p.fix = false;
  p.ts  = 0;
  p.sats = 0;

  CayenneLPP lpp2(64);
  addGpsFixTelemetry(lpp2, &p, true);

  EXPECT_EQ(lpp2.calls.size(), 2u);
  expectUnixTime(lpp2, 1767287010u);
  expectPacked(lpp2, 12u, 170330u);
}

TEST_F(GpsTelemetryFixture, MemoryIsNotClobberedByInvalidReadings) {
  // An invalid reading between two live ones must not erase the memory.
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = TS_170330;
  p.sats = 12;

  addGpsFixTelemetry(lpp, &p, true);

  p.fix = false;
  p.ts  = 0;
  CayenneLPP junk(64);
  addGpsFixTelemetry(junk, &p, true);

  p.fix = true;
  p.ts  = TS_170330;
  p.sats = 9;
  CayenneLPP lpp2(64);
  addGpsFixTelemetry(lpp2, &p, true);

  expectUnixTime(lpp2, 1767287010u);
  expectPacked(lpp2, 9u, 170330u);
}

TEST_F(GpsTelemetryFixture, ImplausibleClockDoesNotClobberAMemory) {
  // A remembered reading exists; a later live reading with an unset/bad epoch
  // must neither emit the remembered stamp as live nor erase it.
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = TS_170330;
  p.sats = 12;

  addGpsFixTelemetry(lpp, &p, true);

  p.ts = 0;   // epoch unset, provider still valid
  CayenneLPP lpp2(64);
  addGpsFixTelemetry(lpp2, &p, true);

  expectUnixTime(lpp2, 1767287010u);
}

TEST_F(GpsTelemetryFixture, PositionCachedWithoutAPlausibleEpoch) {
  // A fix without a UTC lock (valid position, unset epoch) still caches the
  // position — the stamp is what reads absent, not the cached row.
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = 0;
  p.sats = 5;
  p.lat = 51123456L; p.lon = -1134567L; p.alt = 545000L;

  addGpsFixTelemetry(lpp, &p, true);

  expectNoUnixTime(lpp);
  expectPacked(lpp, 5u, 0u);
  EXPECT_TRUE(addCachedGpsPosition(lpp, &p));
}

TEST_F(GpsTelemetryFixture, CachedPositionComesFromTheMemory) {
  // The cached 0x88 is the remembered provider fix, provider units converted
  // exactly as the live row (degrees×1e6 -> degrees, mm -> m) — and never the
  // caller's (0,0) when nothing was ever remembered.
  CayenneLPP lpp(64);
  FakeLocationProvider bare;
  bare.fix = false;
  EXPECT_FALSE(addCachedGpsPosition(lpp, &bare));
  EXPECT_EQ(lpp.calls.size(), 0u) << "never-fixed provider must emit no position";

  CayenneLPP lpp2(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = TS_170330;
  p.sats = 9;
  p.lat = 51123456L; p.lon = -1134567L; p.alt = 545000L;

  addGpsFixTelemetry(lpp2, &p, true);
  EXPECT_TRUE(addCachedGpsPosition(lpp2, &p));

  bool found = false;
  for (const auto& c : lpp2.calls) {
    if (c.type != 0x88) continue;
    found = true;
    EXPECT_EQ(c.channel, 1u);
    EXPECT_NEAR(c.lat, 51.123456f, 1e-4f);
    EXPECT_NEAR(c.lon, -1.134567f, 1e-4f);
    EXPECT_NEAR(c.alt, 545.0f, 1e-3f);
  }
  ASSERT_TRUE(found) << "expected a cached 0x88 position row";
}

TEST_F(GpsTelemetryFixture, GpsFixAvailableFromMemory) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = TS_170330;
  p.sats = 8;

  addGpsFixTelemetry(lpp, &p, true);

  p.fix = false;
  p.ts  = 0;
  EXPECT_TRUE(gpsFixAvailable(&p));
}

TEST_F(GpsTelemetryFixture, GpsFixAvailableFalseWhenNeverFixed) {
  FakeLocationProvider p;
  p.fix = false;
  p.ts  = 0;
  EXPECT_FALSE(gpsFixAvailable(&p));
  EXPECT_FALSE(gpsFixAvailable(nullptr));
}

TEST_F(GpsTelemetryFixture, TwoProvidersKeepSeparateMemories) {
  CayenneLPP lpp(64);
  FakeLocationProvider a, b;
  a.fix = true; a.ts = TS_170330; a.sats = 12;
  b.fix = true; b.ts = TS_170330 + 3600; b.sats = 5;

  addGpsFixTelemetry(lpp, &a, true);
  addGpsFixTelemetry(lpp, &b, true);

  a.fix = false; a.ts = 0; a.sats = 0;
  CayenneLPP la(64);
  addGpsFixTelemetry(la, &a, true);
  expectUnixTime(la, 1767287010u);
  expectPacked(la, 12u, 170330u);
}

TEST_F(GpsTelemetryFixture, GpsFixAvailableDoesNotClaimASlot) {
  // A read must not consume a slot: a never-fixed provider asked repeatedly
  // must leave the array empty, so two real providers still fit.
  FakeLocationProvider bare;
  EXPECT_FALSE(gpsFixAvailable(&bare));
  EXPECT_EQ(gpsFixMemoryFind(&bare), nullptr);
  for (int i = 0; i < 2; i++) EXPECT_EQ(gpsFixMemorySlots()[i].location, nullptr);
}

TEST_F(GpsTelemetryFixture, ThirdProviderEvictsSlotZeroAndReKeys) {
  // Eviction policy: exact match, else first unused slot, else slot 0. With
  // more providers than slots, a third provider takes slot 0 — which must be
  // re-keyed (and cleared), or &a would read back C's fix.
  CayenneLPP lpp(64);
  FakeLocationProvider a, b, c;
  a.fix = true; a.ts = TS_170330; a.sats = 12;
  b.fix = true; b.ts = TS_170330; b.sats = 5;
  c.fix = true; c.ts = TS_170330 + 7200; c.sats = 3;

  addGpsFixTelemetry(lpp, &a, true);
  addGpsFixTelemetry(lpp, &b, true);
  addGpsFixTelemetry(lpp, &c, true);

  EXPECT_EQ(gpsFixMemoryFind(&a), nullptr) << "A must be evicted, not aliased";
  ASSERT_NE(gpsFixMemoryFind(&b), nullptr) << "B must survive";
  ASSERT_NE(gpsFixMemoryFind(&c), nullptr) << "C owns slot 0";
  EXPECT_EQ(gpsFixMemoryFind(&c)->fix.satellites, 3);

  // A re-claims a slot (evicting C), and since its own fix went with the
  // eviction it must NOT read back C's remembered stamp — that is the aliasing
  // bug this pins.
  a.fix = false; a.ts = 0; a.sats = 0;
  CayenneLPP la(64);
  addGpsFixTelemetry(la, &a, true);
  expectNoUnixTime(la);
  expectPacked(la, 0u, 0u);
}
