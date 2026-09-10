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
    calls.push_back({channel, 0x85, value});
  }
  void addGenericSensor(uint8_t channel, float value) {
    calls.push_back({channel, 0x64, static_cast<uint32_t>(value)});
  }
  void addGPS(uint8_t channel, float lat, float lon, float alt) {
    (void)lat; (void)lon; (void)alt;
    calls.push_back({channel, 0x88, 0});
  }

  std::vector<LppCall> calls;
};
