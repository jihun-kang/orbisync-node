#ifndef ORBI_SYNC_NODE_H
#define ORBI_SYNC_NODE_H

#include <Arduino.h>
#include <ArduinoJson.hpp>

namespace AJ = ArduinoJson;
using JsonArray = AJ::JsonArray;
using JsonDocument = AJ::JsonDocument;
using JsonObject = AJ::JsonObject;
using DeserializationError = AJ::DeserializationError;
using DynamicJsonDocument = AJ::DynamicJsonDocument;
template <size_t N>
using StaticJsonDocument = AJ::StaticJsonDocument<N>;

#if defined(ESP32)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#else
#error "Unsupported board for OrbiSyncNode"
#endif
#include <WebSocketsClient.h>

namespace OrbiSyncNodeConstants {
  constexpr size_t kMaxResponseLength = 2048;
  constexpr size_t kMaxStreamRequestSize = 4096;
  constexpr uint32_t kWsHeartbeatIntervalMs = 30000;
}

class OrbiSyncNode {
public:
  enum class State {
    BOOT,
    HELLO,
    PENDING_POLL,
    ACTIVE,
    ERROR
  };

  struct Config {
    const char* hubBaseUrl;
    const char* slotId;
    const char* firmwareVersion;
    const char* const* capabilities;
    uint8_t capabilityCount;
    uint32_t heartbeatIntervalMs;
    uint8_t ledPin;
    bool blinkOnHeartbeat;
    bool allowInsecureTls;
    const char* rootCaPem;
    bool enableCommandPolling;
    uint32_t commandPollIntervalMs;
    const char* loginToken;
    const char* machineId;
    const char* nodeName;
    const char* pairingCode;
    const char* internalKey;
    bool enableNodeRegistration;
    uint32_t registerRetryMs;
    bool preferRegisterBySlot;
    bool enableTunnel;
  };

  explicit OrbiSyncNode(const Config& config);
  ~OrbiSyncNode();

  /**
   * 역할: STA 모드 Wi-Fi 연결을 시작하고 타이머 초기화
   * 인자: SSID, 비밀번호
   * 반환: 현재 Wi-Fi 상태가 연결되어 있으면 true
   * 실패 처리: 인증 정보 부족 시 last_error_ 설정 후 반복
   */
  bool beginWiFi(const char* ssid, const char* pass);

  /**
   * 역할: /api/device/hello POST 호출 (slot_id, device_info, nonce, firmware, capabilities_hash 포함)
   * 인자: 없음
   * 반환: 서버 응답을 정상 처리하면 true
   * 실패 처리: last_error_ 기록 + next_net_action_ms_ 연기
   */
  bool sendHello();

  /**
   * 역할: /api/device/session 폴링 (slot_id, nonce)
   * 인자: 없음
   * 반환: 상태 파악 후 HELLO/PENDING/ACTIVE 전이 성공하면 true
   * 실패 처리: HTTP 에러는 backoff, 401/403은 세션 폐기 후 HELLO
   */
  bool pollSession();

  /**
   * 역할: /api/device/heartbeat POST 보내기 (slot_id, nonce, uptime, rssi 등 포함)
   * 인자: 없음
   * 반환: 2xx 응답 시 true
   * 실패 처리: 401/403은 세션 폐기, 비인증 에러는 backoff
   */
  bool sendHeartbeat();

  /**
   * 역할: (선택) /api/device/commands/pull로 명령 폴링 및 ack 전송
   * 인자: 없음
   * 반환: 통신 성공이면 true
   * 실패 처리: HTTP 에러는 backoff
   */
  bool pullCommands();

  /**
   * 역할: RAM 세션 유효성 판단
   * 인자: 없음
   * 반환: session_token_가 존재하고 TTL이 지나지 않았다면 true
   */
  bool isSessionValid() const;

  bool isRegistered() const;
  const char* getNodeId() const;
  const char* getNodeAuthToken() const;
  const char* getTunnelUrl() const;
  bool isTunnelConnected() const;

  /**
   * 역할: 전체 상태 머신 루프 (Wi-Fi, HELLO, PENDING, ACTIVE 등 처리)
   * 인자: 없음
   * 반환: 없음
   */
  void loopTick();

  State getState() const;
  const char* getLastError() const;
  void clearSession();

  bool registerNodeIfNeeded(uint32_t now);
  bool registerBySlot();
  bool registerByPairing();
  bool connectTunnelWs(uint32_t now);
  bool sendRegisterFrame();
  bool sendHeartbeatFrame();
  void handleWsEvent(WStype_t type, uint8_t* payload, size_t length);

private:
  bool ensureWiFi();
  String createNonce();
  void setLastError(const char* msg);
  void scheduleNextNetwork(uint32_t delayMs = 0);
  void scheduleNextWiFi(uint32_t delayMs = 0);
  void processActive(uint32_t now);
  bool applyTlsPolicy();
  bool handleCommand(JsonObject command);
  String capabilitiesHash() const;
  bool responseToJson(JsonDocument& doc);
  void scheduleRegisterRetry(uint32_t now);
  void scheduleWsRetry(uint32_t now);
  bool parseTunnelUrl(const char* raw, String& host, uint16_t& port, String& path, bool& secure) const;
  void handleWsMessage(const uint8_t* payload, size_t length);
  void handleControl(JsonDocument& doc);
  void handleData(JsonDocument& doc);
  bool tryProcessHttpRequest();
  String buildHttpRawResponse(int statusCode, const String& jsonBody);
  String routeHttpRequest(const String& method, const String& path, const String& body, int& statusCode);
  bool sendDataFrame(const char* streamId, const uint8_t* data, size_t len);
  static String base64Encode(const uint8_t* data, size_t length);
  static bool base64Decode(const char* input, uint8_t* output, size_t& outLen);

  Config config_;
  State state_;
  String wifi_ssid_;
  String wifi_password_;
#if defined(ESP8266)
  BearSSL::WiFiClientSecure secure_client_;
#else
  WiFiClientSecure secure_client_;
#endif
  WiFiClient insecure_client_;
#if defined(ESP8266)
  HTTPClient http_;
#else
  HTTPClient http_;
#endif
  bool use_https_;
  uint32_t next_wifi_action_ms_;
  uint32_t next_net_action_ms_;
  uint32_t next_command_action_ms_;
  uint32_t wifi_backoff_ms_;
  uint32_t net_backoff_ms_;
  uint32_t register_backoff_ms_;
  uint32_t next_register_action_ms_;
  uint32_t session_expires_at_ms_;
  uint32_t last_heartbeat_ms_;
  char session_token_[256];
  char node_id_[64];
  char node_auth_token_[128];
  char tunnel_url_[192];
  char active_stream_id_[64];
  bool stream_open_;
  String request_buf_;
  bool is_registered_;
  char last_error_[128];
  bool led_state_;
  WebSocketsClient ws_client_;
  String ws_extra_headers_;
  bool ws_connected_;
  bool ws_register_frame_sent_;
  uint32_t ws_backoff_ms_;
  uint32_t next_ws_action_ms_;
  uint32_t ws_last_heartbeat_ms_;
};

#endif // ORBI_SYNC_NODE_H
