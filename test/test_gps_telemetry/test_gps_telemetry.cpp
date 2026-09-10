// Host-side tests for addGpsFixTelemetry (GpsTelemetry.h).
//
// Verifies the gating logic (isValid, gps_active, plausibility) against the
// recording CayenneLPP mock. Does NOT prove the real wire bytes — that is
// verified empirically against the ElectronicCats library (see plan §wire-layout).
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

// Expect exactly one unix-time call with the given timestamp.
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

static void expectSats(const CayenneLPP& lpp, uint32_t expected) {
  bool found = false;
  for (const auto& c : lpp.calls) {
    if (c.type == 0x64) {
      ASSERT_EQ(c.channel, 1u);
      ASSERT_EQ(c.value, expected);
      found = true;
    }
  }
  ASSERT_TRUE(found) << "expected sats entry with value " << expected;
}

static void expectNoSats(const CayenneLPP& lpp) {
  for (const auto& c : lpp.calls) {
    ASSERT_NE(c.type, 0x64) << "unexpected sats entry";
  }
}

TEST(GpsTelemetry, ValidFixPlausibleTime) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = 1757500000L;
  p.sats = 9;

  addGpsFixTelemetry(lpp, &p, true);

  EXPECT_EQ(lpp.calls.size(), 2u);   // 0x85 + 0x64
  expectUnixTime(lpp, 1757500000u);
  expectSats(lpp, 9u);
}

TEST(GpsTelemetry, ValidFixImplausibleTimeZero) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = 0;
  p.sats = 7;

  addGpsFixTelemetry(lpp, &p, true);

  // implausible → no unix-time, sats still emitted
  EXPECT_EQ(lpp.calls.size(), 1u);
  expectNoUnixTime(lpp);
  expectSats(lpp, 7u);
}

TEST(GpsTelemetry, ValidFixImplausibleTimeBeforeThreshold) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = 1577836799L;   // 1 second before 2020-01-01
  p.sats = 3;

  addGpsFixTelemetry(lpp, &p, true);

  EXPECT_EQ(lpp.calls.size(), 1u);
  expectNoUnixTime(lpp);
  expectSats(lpp, 3u);
}

TEST(GpsTelemetry, NoFix) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = false;
  p.ts  = 1757500000L;
  p.sats = 5;

  addGpsFixTelemetry(lpp, &p, true);

  // no fix → no unix-time, sats still emitted
  EXPECT_EQ(lpp.calls.size(), 1u);
  expectNoUnixTime(lpp);
  expectSats(lpp, 5u);
}

TEST(GpsTelemetry, GpsInactive) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = 1757500000L;
  p.sats = 9;

  addGpsFixTelemetry(lpp, &p, false);  // gps_active = false

  // gps off → nothing emitted
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
  p.ts  = 1757500000L;
  p.sats = 12;

  addGpsFixTelemetry(lpp, &p, true);

  for (const auto& c : lpp.calls) {
    EXPECT_EQ(c.channel, 1u) << "all entries must be on channel 1 (TELEM_CHANNEL_SELF)";
  }
}
