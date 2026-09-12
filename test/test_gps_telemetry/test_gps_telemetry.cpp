// Host-side tests for addGpsFixTelemetry (GpsTelemetry.h).
//
// Verifies the gating logic (isValid, gps_active, plausibility) and the packed
// sats+clock encoding against the recording CayenneLPP mock. Does NOT prove the
// real wire bytes — that is verified empirically against the ElectronicCats
// library (see plan §wire-layout).
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

TEST(GpsTelemetry, AwakeWithFixPacksSatsAndClock) {
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

TEST(GpsTelemetry, AwakeWithoutFixPacksSatsWithZeroClock) {
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

TEST(GpsTelemetry, ImplausibleClockReadsZeroNotGarbage) {
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

TEST(GpsTelemetry, AsleepWithCachedFixKeepsStampAndClock) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;            // latched valid fix...
  p.ts  = TS_170330;
  p.sats = 9;

  addGpsFixTelemetry(lpp, &p, false);  // ...but GPS asleep

  // Cached position carries its stamp and clock; the count reads 0 because
  // sats-in-use is meaningless with the receiver off.
  EXPECT_EQ(lpp.calls.size(), 2u);
  expectUnixTime(lpp, 1767287010u);
  expectPacked(lpp, 0u, 170330u);
}

TEST(GpsTelemetry, AsleepWithoutFixEmitsNothing) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = false;
  p.ts  = 0;
  p.sats = 0;

  addGpsFixTelemetry(lpp, &p, false);

  EXPECT_EQ(lpp.calls.size(), 0u);
}

TEST(GpsTelemetry, NullProvider) {
  CayenneLPP lpp(64);

  addGpsFixTelemetry(lpp, nullptr, true);

  EXPECT_EQ(lpp.calls.size(), 0u);
}

TEST(GpsTelemetry, ChannelOne) {
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
