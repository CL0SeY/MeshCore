#include "RS232Bridge.h"

#include <HardwareSerial.h>

#ifdef WITH_RS232_BRIDGE

RS232Bridge::RS232Bridge(NodePrefs *prefs, Stream &serial, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc), _serial(&serial) {}

void RS232Bridge::begin() {
#if !defined(WITH_RS232_BRIDGE_RX) || !defined(WITH_RS232_BRIDGE_TX)
  #error "WITH_RS232_BRIDGE_RX and WITH_RS232_BRIDGE_TX must be defined"
#endif

  pinMode(WITH_RS232_BRIDGE_RX, INPUT_PULLUP);
  pinMode(WITH_RS232_BRIDGE_TX, OUTPUT);

#if defined(ESP32)
  ((HardwareSerial *)_serial)->setPins(WITH_RS232_BRIDGE_RX, WITH_RS232_BRIDGE_TX);
#elif defined(NRF52_PLATFORM)
  ((Uart *)_serial)->setPins(WITH_RS232_BRIDGE_RX, WITH_RS232_BRIDGE_TX);
#elif defined(RP2040_PLATFORM)
  ((SerialUART *)_serial)->setRX(WITH_RS232_BRIDGE_RX);
  ((SerialUART *)_serial)->setTX(WITH_RS232_BRIDGE_TX);
#elif defined(STM32_PLATFORM)
  ((HardwareSerial *)_serial)->setRx(WITH_RS232_BRIDGE_RX);
  ((HardwareSerial *)_serial)->setTx(WITH_RS232_BRIDGE_TX);
#else
  #error RS232Bridge was not tested on the current platform
#endif
  ((HardwareSerial *)_serial)->begin(_prefs->bridge_baud);

  BRIDGE_DEBUG_PRINTLN("Serial bridge initialized\n");

  _initialized = true;
}

void RS232Bridge::end() {
  ((HardwareSerial *)_serial)->end();
  _initialized = false;
}

void RS232Bridge::loop() {
  // Guard against uninitialized state
  if (_initialized == false) {
    return;
  }

  while (_serial->available()) {
    uint8_t b = _serial->read();

    if (_rx_buffer_pos < 2) {
      // Waiting for magic word
      if ((_rx_buffer_pos == 0 && b == ((BRIDGE_PACKET_MAGIC >> 8) & 0xFF)) ||
          (_rx_buffer_pos == 1 && b == (BRIDGE_PACKET_MAGIC & 0xFF))) {
        _rx_buffer[_rx_buffer_pos++] = b;
      } else {
        // Invalid magic byte, reset and start over
        _rx_buffer_pos = 0;
        // Check if this byte could be the start of a new magic word
        if (b == ((BRIDGE_PACKET_MAGIC >> 8) & 0xFF)) {
          _rx_buffer[_rx_buffer_pos++] = b;
        }
      }
    } else {
      // Reading length, payload, and checksum
      _rx_buffer[_rx_buffer_pos++] = b;

      if (_rx_buffer_pos >= 4) {
        uint16_t len = (_rx_buffer[2] << 8) | _rx_buffer[3];

        if (len > (MAX_TRANS_UNIT + 1)) {
          resyncBuffer();
          continue;
        }

        if (_rx_buffer_pos == len + SERIAL_OVERHEAD) {
          uint16_t received_checksum = (_rx_buffer[4 + len] << 8) | _rx_buffer[5 + len];

          if (validateChecksum(_rx_buffer + 4, len, received_checksum)) {
            BRIDGE_DEBUG_PRINTLN("RX, len=%d crc=0x%04x\n", len, received_checksum);
            mesh::Packet *pkt = _mgr->allocNew();
            if (pkt) {
              if (pkt->readFrom(_rx_buffer + 4, len)) {
                onPacketReceived(pkt);
              } else {
                BRIDGE_DEBUG_PRINTLN("RX failed to parse packet\n");
                _mgr->free(pkt);
              }
            } else {
              BRIDGE_DEBUG_PRINTLN("RX failed to allocate packet\n");
            }
          } else {
            BRIDGE_DEBUG_PRINTLN("RX checksum mismatch, rcv=0x%04x\n", received_checksum);
            resyncBuffer();
            continue;
          }
          _rx_buffer_pos = 0;
        }
      }
    }
  }
}

void RS232Bridge::sendPacket(mesh::Packet *packet) {
  if (_initialized == false) {
    return;
  }

  if (!packet) {
    return;
  }

  if (!_seen_packets.hasSeen(packet)) {

    uint8_t buffer[MAX_SERIAL_PACKET_SIZE];
    uint16_t len = packet->writeTo(buffer + 4);

    if (len > (MAX_TRANS_UNIT + 1)) {
      BRIDGE_DEBUG_PRINTLN("TX packet too large (payload=%d, max=%d)\n", len, MAX_TRANS_UNIT + 1);
      return;
    }

    buffer[0] = (BRIDGE_PACKET_MAGIC >> 8) & 0xFF;
    buffer[1] = BRIDGE_PACKET_MAGIC & 0xFF;
    buffer[2] = (len >> 8) & 0xFF;
    buffer[3] = len & 0xFF;

    uint16_t checksum = fletcher16(buffer + 4, len);
    buffer[4 + len] = (checksum >> 8) & 0xFF;
    buffer[5 + len] = checksum & 0xFF;

    _serial->write(buffer, len + SERIAL_OVERHEAD);

    BRIDGE_DEBUG_PRINTLN("TX, len=%d crc=0x%04x\n", len, checksum);
  }
}

void RS232Bridge::onPacketReceived(mesh::Packet *packet) {
  handleReceivedPacket(packet);
}

void RS232Bridge::resyncBuffer() {
  static constexpr uint8_t MAGIC_HIGH = (BRIDGE_PACKET_MAGIC >> 8) & 0xFF;
  static constexpr uint8_t MAGIC_LOW = BRIDGE_PACKET_MAGIC & 0xFF;

  // Scan buffer for magic bytes pattern to find where next valid packet starts
  for (uint16_t i = 0; i < _rx_buffer_pos - 1; i++) {
    if (_rx_buffer[i] == MAGIC_HIGH && _rx_buffer[i + 1] == MAGIC_LOW) {
      // Found magic pattern - shift remaining data to start of buffer
      uint16_t remaining = _rx_buffer_pos - (i + 2);
      for (uint16_t j = 0; j < remaining; j++) {
        _rx_buffer[j] = _rx_buffer[i + 2 + j];
      }
      _rx_buffer_pos = remaining;
      return;
    }
  }

  // No magic pattern found, reset completely
  _rx_buffer_pos = 0;
}

#endif
