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
      break;   // FIRST 0x64 is the sat count; a second 0x64 is the HHMMSS clock
    }
  }
  ASSERT_TRUE(found) << "expected sats entry with value " << expected;
}

// Expect the HHMMSS clock as the SECOND 0x64 entry with the given value.
static void expectHhmmss(const CayenneLPP& lpp, uint32_t expected) {
  int seen = 0;
  for (const auto& c : lpp.calls) {
    if (c.type == 0x64) {
      seen++;
      if (seen == 2) {
        EXPECT_EQ(c.channel, 1u);
        EXPECT_EQ(c.value, expected);
        return;
      }
    }
  }
  FAIL() << "expected second 0x64 (HHMMSS) entry with value " << expected << " (saw " << seen << " 0x64 entries)";
}

static void expectNoHhmmss(const CayenneLPP& lpp) {
  int seen = 0;
  for (const auto& c : lpp.calls) {
    if (c.type == 0x64) seen++;
  }
  ASSERT_LE(seen, 1) << "unexpected HHMMSS (second 0x64) entry";
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

  EXPECT_EQ(lpp.calls.size(), 3u);   // 0x85 + 0x64 (sats) + 0x64 (HHMMSS)
  expectUnixTime(lpp, 1757500000u);
  expectSats(lpp, 9u);
  expectHhmmss(lpp, gpsFixTimeHhmmss(1757500000L));
}

TEST(GpsTelemetry, HhmmssKnownVector) {
  // 2026-01-01 17:03:30 UTC -> 170330. Guards the /% arithmetic, not just
  // self-consistency with the helper.
  EXPECT_EQ(gpsFixTimeHhmmss(1767287010L), 170330u);
  EXPECT_EQ(gpsFixTimeHhmmss(1767225600L), 0u);        // midnight -> 0
  EXPECT_EQ(gpsFixTimeHhmmss(1767311999L), 235959u);   // 23:59:59 -> 235959
}

TEST(GpsTelemetry, ValidFixImplausibleTimeZero) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;
  p.ts  = 0;
  p.sats = 7;

  addGpsFixTelemetry(lpp, &p, true);

  // implausible → no unix-time and no HHMMSS, sats still emitted
  EXPECT_EQ(lpp.calls.size(), 1u);
  expectNoUnixTime(lpp);
  expectSats(lpp, 7u);
  expectNoHhmmss(lpp);
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
  expectNoHhmmss(lpp);
}

TEST(GpsTelemetry, NoFix) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = false;
  p.ts  = 1757500000L;
  p.sats = 5;

  addGpsFixTelemetry(lpp, &p, true);

  // no fix → no unix-time and no HHMMSS, sats still emitted
  EXPECT_EQ(lpp.calls.size(), 1u);
  expectNoUnixTime(lpp);
  expectSats(lpp, 5u);
  expectNoHhmmss(lpp);
}

TEST(GpsTelemetry, GpsInactiveCachedFix) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = true;            // latched valid fix...
  p.ts  = 1757500000L;
  p.sats = 9;

  addGpsFixTelemetry(lpp, &p, false);  // ...but GPS asleep

  // Cached position carries its stamp + HHMMSS clock; the zero placeholder
  // holds the first-0x64 slot so decoders read sats=0, clock second.
  EXPECT_EQ(lpp.calls.size(), 3u);
  expectUnixTime(lpp, 1757500000u);
  expectSats(lpp, 0u);
  expectHhmmss(lpp, gpsFixTimeHhmmss(1757500000L));
}

TEST(GpsTelemetry, GpsInactiveNoFix) {
  CayenneLPP lpp(64);
  FakeLocationProvider p;
  p.fix = false;
  p.ts  = 0;
  p.sats = 0;

  addGpsFixTelemetry(lpp, &p, false);

  // asleep and never fixed → nothing emitted
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

TEST(GpsTelemetry, WireOrderSatsBeforeHhmmss) {
  // Decoders take the FIRST 0x64 as sats: sats must precede HHMMSS on the
  // wire in every layout, awake or asleep.
  {
    CayenneLPP lpp(64);
    FakeLocationProvider p;
    p.fix = true;
    p.ts  = 1767287010L;   // 17:03:30 UTC
    p.sats = 12;
    addGpsFixTelemetry(lpp, &p, true);
    ASSERT_EQ(lpp.calls.size(), 3u);
    EXPECT_EQ(lpp.calls[0].type, 0x85);
    EXPECT_EQ(lpp.calls[1].type, 0x64);
    EXPECT_EQ(lpp.calls[1].value, 12u);
    EXPECT_EQ(lpp.calls[2].type, 0x64);
    EXPECT_EQ(lpp.calls[2].value, 170330u);
  }
  {
    CayenneLPP lpp(64);
    FakeLocationProvider p;
    p.fix = true;
    p.ts  = 1767287010L;
    p.sats = 12;
    addGpsFixTelemetry(lpp, &p, false);
    ASSERT_EQ(lpp.calls.size(), 3u);
    EXPECT_EQ(lpp.calls[0].type, 0x85);
    EXPECT_EQ(lpp.calls[1].type, 0x64);
    EXPECT_EQ(lpp.calls[1].value, 0u);
    EXPECT_EQ(lpp.calls[2].type, 0x64);
    EXPECT_EQ(lpp.calls[2].value, 170330u);
  }
}
