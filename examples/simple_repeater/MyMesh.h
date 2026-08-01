#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <target.h>

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
#endif

#ifdef WITH_RS232_BRIDGE
#include "helpers/bridges/RS232Bridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_ESPNOW_BRIDGE
#include "helpers/bridges/ESPNowBridge.h"
#define WITH_BRIDGE
#endif

#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/ChannelDetails.h>
#include <helpers/ClientACL.h>
#include <helpers/CommonCLI.h>
#include <helpers/BaseSerialInterface.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/RegionMap.h>
#include "RateLimiter.h"

#ifdef WITH_BRIDGE
extern AbstractBridge* bridge;
#endif

struct RepeaterStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;                // was 'n_full_events'
  int16_t  last_snr;   // x 4
  uint16_t n_direct_dups, n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
};

#ifndef MAX_CLIENTS
  #define MAX_CLIENTS           32
#endif

struct NeighbourInfo {
  mesh::Identity id;
  uint32_t advert_timestamp;
  uint32_t heard_timestamp;
  int8_t snr; // multiplied by 4, user should divide to get float value
};

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "6 Jun 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.16.0"
#endif

#define FIRMWARE_ROLE "repeater"

#define PACKET_LOG_FILE  "/packet_log"

// Companion protocol command codes
#define CMD_APP_START                 1
#define CMD_SEND_TXT_MSG              2
#define CMD_SEND_CHANNEL_TXT_MSG      3
#define CMD_GET_CONTACTS              4
#define CMD_GET_DEVICE_TIME           5
#define CMD_SET_DEVICE_TIME           6
#define CMD_SEND_SELF_ADVERT          7
#define CMD_SET_ADVERT_NAME           8
#define CMD_SET_RADIO_PARAMS          11
#define CMD_SET_RADIO_TX_POWER        12
#define CMD_RESET_PATH                13
#define CMD_SET_ADVERT_LATLON         14
#define CMD_DEVICE_QUERY              22
#define CMD_SEND_RAW_DATA             25
#define CMD_LOGIN                     26
#define CMD_HAS_CONNECTION            28
#define CMD_LOGOUT                    29
#define CMD_SET_FLOOD_SCOPE_KEY       54
#define CMD_GET_STATS                 56
#define CMD_SEND_CHANNEL_DATA         62
#define CMD_SET_DEFAULT_FLOOD_SCOPE   63
#define CMD_GET_DEFAULT_FLOOD_SCOPE   64
#define CMD_SEND_RAW_PACKET           65

// Response codes
#define RESP_CODE_OK                  0
#define RESP_CODE_ERR                 1
#define RESP_CODE_CONTACTS_START      2
#define RESP_CODE_CONTACT             3
#define RESP_CODE_END_OF_CONTACTS     4
#define RESP_CODE_SELF_INFO           5
#define RESP_CODE_SENT                6
#define RESP_CODE_CURR_TIME           9
#define RESP_CODE_NO_MORE_MESSAGES    10
#define RESP_CODE_DEVICE_INFO         13
#define RESP_CODE_DISABLED            15
#define RESP_CODE_STATS               24
#define RESP_CODE_DEFAULT_FLOOD_SCOPE 28
#define CMD_GET_CHANNEL               31
#define CMD_SET_CHANNEL               32
#define CMD_SYNC_NEXT_MESSAGE         10
#define RESP_CODE_CHANNEL_INFO        18
#define RESP_CODE_CHANNEL_MSG_RECV_V3 17
#define PUSH_CODE_MSG_WAITING         0x83
#define PUSH_CODE_LOG_RX_DATA         0x88
#ifndef OFFLINE_QUEUE_SIZE
  #define OFFLINE_QUEUE_SIZE 16
#endif

// Error codes
#define ERR_CODE_UNSUPPORTED_CMD      1
#define ERR_CODE_NOT_FOUND            2
#define ERR_CODE_TABLE_FULL           3
#define ERR_CODE_BAD_STATE            4
#define ERR_CODE_ILLEGAL_ARG          6

// Stats sub-types
#define STATS_TYPE_CORE               0
#define STATS_TYPE_RADIO              1
#define STATS_TYPE_PACKETS            2

// Helper macros
#define MAX_FRAME_SIZE  176

class MyMesh : public mesh::Mesh, public CommonCLICallbacks {
  FILESYSTEM* _fs;
  uint32_t last_millis;
  uint64_t uptime_millis;
  unsigned long next_local_advert, next_flood_advert;
  bool _logging;
  NodePrefs _prefs;
  ClientACL  acl;
  CommonCLI _cli;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  uint8_t reply_path[MAX_PATH_SIZE];
  int8_t  reply_path_len;
  uint8_t reply_path_hash_size;
  TransportKeyStore key_store;
  RegionMap region_map, temp_map;
  RegionEntry* load_stack[8];
  RegionEntry* recv_pkt_region;
  TransportKey default_scope;
  RateLimiter discover_limiter, anon_limiter;
  uint32_t pending_discover_tag;
  unsigned long pending_discover_until;
  bool region_load_active;
  unsigned long dirty_contacts_expiry;
#if MAX_NEIGHBOURS
  NeighbourInfo neighbours[MAX_NEIGHBOURS];
#endif
  CayenneLPP telemetry;
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;
  int  matching_peer_indexes[MAX_CLIENTS];
#if defined(WITH_RS232_BRIDGE)
  RS232Bridge bridge;
#elif defined(WITH_ESPNOW_BRIDGE)
  ESPNowBridge bridge;
#endif
  BaseSerialInterface* _serial;
  uint8_t cmd_frame[MAX_FRAME_SIZE + 1];
  uint8_t out_frame[MAX_FRAME_SIZE + 1];
#if MAX_GROUP_CHANNELS
  ChannelDetails _channels[MAX_GROUP_CHANNELS];
  uint8_t _num_channels;
#endif
  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
  };
  Frame offline_queue[OFFLINE_QUEUE_SIZE];
  int offline_queue_len = 0;
  bool push_pending = false;
  void putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr);
  uint8_t handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood);
  uint8_t handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);
  mesh::Packet* createSelfAdvert();

  File openAppend(const char* fname);
  File openRead(const char* fname);
  File openWrite(const char* fname);
  bool isLooped(const mesh::Packet* packet, const uint8_t max_counters[]);

protected:
  float getAirtimeBudgetFactor() const override {
    return _prefs.airtime_factor;
  }

  bool allowPacketForward(const mesh::Packet* packet) override;
  const char* getLogDateTime() override;
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;

  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;
  int calcRxDelay(float score, uint32_t air_time) const override;

  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;

  int getInterferenceThreshold() const override {
    return _prefs.interference_threshold;
  }
  int getAGCResetInterval() const override {
    return ((int)_prefs.agc_reset_interval) * 4000;   // milliseconds
  }
  uint8_t getExtraAckTransmitCount() const override {
    return _prefs.multi_acks;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
  }
#endif

  bool filterRecvFloodPacket(mesh::Packet* pkt) override;

  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int searchPeersByHash(const uint8_t* hash) override;
  int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len);
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;

  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) override;

  void sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size);

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);
  void sendNodeDiscoverReq();
  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  NodePrefs* getNodePrefs() {
    return &_prefs;
  }

  void savePrefs() override {
    _cli.savePrefs(_fs);
  }

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size);

  // CommonCLICallbacks
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;

  void setLoggingOn(bool enable) override { _logging = enable; }

  void eraseLogFile() override {
    _fs->remove(PACKET_LOG_FILE);
  }

  void dumpLogFile() override;
  void setTxPower(int8_t power_dbm) override;
  void formatNeighborsReply(char *reply) override;
  void removeNeighbor(const uint8_t* pubkey, int key_len) override;
  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;
  void startRegionsLoad() override;
  bool saveRegions() override;
  void onDefaultRegionChanged(const RegionEntry* r) override;

  mesh::LocalIdentity& getSelfId() override { return self_id; }

  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;

  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);
  void loop();

  // Companion protocol over USB CDC
  void startInterface(BaseSerialInterface &serial);
  void handleCmdFrame(size_t len);
  void checkSerialInterface();
  void writeOKFrame();
  void writeErrFrame(uint8_t err_code);
  void writeDisabledFrame();
  void writeRepeaterStats(uint8_t stats_type);

  // Channel support
  bool addChannel(const char* name, const char* psk_base64);
  bool getChannel(uint8_t idx, mesh::GroupChannel& dest);
  mesh::GroupChannel* findChannelByHash(const uint8_t* hash);
  void relayGroupData(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len);
  void sendChannelTextMsg(uint8_t channel_idx, uint32_t timestamp, const char* msg, size_t msg_len);
  void addToOfflineQueue(const uint8_t frame[], int len);
  int getFromOfflineQueue(uint8_t frame[]);
  uint8_t findChannelIdx(const mesh::GroupChannel& ch);
  bool setChannel(uint8_t idx, const ChannelDetails& src);
  void saveChannels();
  void loadChannels();

#if defined(WITH_BRIDGE)
  void setBridgeState(bool enable) override {
    if (enable == bridge.isRunning()) return;
    if (enable)
    {
      bridge.begin();
    }
    else 
    {
      bridge.end();
    }
  }

  void restartBridge() override {
    if (!bridge.isRunning()) return;
    bridge.end();
    bridge.begin();
  }
#endif

  // To check if there is pending work
  bool hasPendingWork() const;

#if defined(USE_SX1262) || defined(USE_SX1268)
  void setRxBoostedGain(bool enable) override;
#endif
};
