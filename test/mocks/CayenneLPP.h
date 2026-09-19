#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// A recording mock for unit-testing GpsTelemetry.h; also satisfies
// KissModem.cpp (which only calls getBuffer/getSize) and any other
// code that includes CayenneLPP.h in the native test environment.
//
// NOTE: in env:native the real src/Mesh.h is resolved by the -I src
// include order (before -I test/mocks). This helper relies on that
// working; the sibling env:native_kiss_modem would NOT support
// GpsTelemetry.h under its own include order.

struct LppCall {
  uint8_t  channel;
  uint8_t  type;
  uint32_t value;
  float    lat = 0;
  float    lon = 0;
  float    alt = 0;
};

class CayenneLPP {
public:
  explicit CayenneLPP(size_t) {}
  const uint8_t* getBuffer() const { return nullptr; }
  uint16_t getSize() const { return 0; }
  uint8_t getError() const { return 0; }
  void reset() {}

  // Recording methods used by GpsTelemetry.h
  void addUnixTime(uint8_t channel, uint32_t value) {
    LppCall c{};
    c.channel = channel; c.type = 0x85; c.value = value;
    calls.push_back(c);
  }
  void addGenericSensor(uint8_t channel, float value) {
    LppCall c{};
    c.channel = channel; c.type = 0x64; c.value = static_cast<uint32_t>(value);
    calls.push_back(c);
  }
  void addGPS(uint8_t channel, float lat, float lon, float alt) {
    LppCall c{};
    c.channel = channel; c.type = 0x88; c.lat = lat; c.lon = lon; c.alt = alt;
    calls.push_back(c);
  }

  std::vector<LppCall> calls;
};
