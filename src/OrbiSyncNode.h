/**
 * @file    OrbiSyncNode.h
 * @author  jihun kang
 * @date    2026-01-29
 * @brief   OrbiSyncNode 라이브러리의 public API 및 클래스 정의
 *
 * @details
 * 이 파일은 OrbiSyncNode 라이브러리의 핵심 인터페이스를 정의합니다.
 * ESP8266/ESP32 기반 IoT 노드가 OrbiSync Hub와 통신하기 위한 상태 머신,
 * 콜백 인터페이스, 요청/응답 구조체를 제공합니다.
 *
 * - 상태 머신 정의 (BOOT → HELLO → PENDING_POLL → ACTIVE → ERROR)
 * - 이벤트 콜백 인터페이스 (상태 변경, 에러, 등록, 터널 연결, HTTP 요청)
 * - HTTP/COAP 요청 처리 구조체 (Request/Response)
 * - WiFi 연결, Hub 통신, WebSocket 터널링 API
 *
 * 본 코드는 OrbiSync 오픈소스 프로젝트의 일부입니다.
 * 라이선스 및 사용 조건은 LICENSE 파일을 참고하세요.
 */

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

  enum class Protocol {
    HTTP,
    COAP
  };

  struct Request {
    Protocol proto;
    const char* method;
    const char* path;
    const uint8_t* body;
    size_t body_len;
  };

  struct Response {
    int status;
    const char* content_type;
    const uint8_t* body;
    size_t body_len;
  };

  using RequestHandler = bool(*)(const Request&, Response&);
  using RequestCallback = RequestHandler; // 별칭
  using StateChangeCallback = void(*)(State oldState, State newState);
  using ErrorCallback = void(*)(const char* error);
  using RegisterCallback = void(*)(const char* nodeId);
  using TunnelCallback = void(*)(bool connected, const char* url);

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

  /**
   * 역할: HTTP/COAP 요청 처리 핸들러 등록
   * 인자: 요청 처리 콜백 함수 포인터
   * 반환: 없음
   * 참고: callback이 true를 반환하면 그 응답을 사용, false면 기본 라우팅으로 fallback
   */
  void onRequest(RequestCallback callback);

  /**
   * 역할: 상태 변경 콜백 등록
   * 인자: 상태 변경 시 호출될 콜백 함수 포인터
   * 반환: 없음
   */
  void onStateChange(StateChangeCallback callback);

  /**
   * 역할: 에러 발생 콜백 등록
   * 인자: 에러 발생 시 호출될 콜백 함수 포인터
   * 반환: 없음
   */
  void onError(ErrorCallback callback);

  /**
   * 역할: 노드 등록 완료 콜백 등록
   * 인자: 노드 등록 성공 시 호출될 콜백 함수 포인터
   * 반환: 없음
   */
  void onRegistered(RegisterCallback callback);

  /**
   * 역할: 터널(WebSocket) 연결 상태 변경 콜백 등록
   * 인자: 터널 연결/해제 시 호출될 콜백 함수 포인터
   * 반환: 없음
   */
  void onTunnelChange(TunnelCallback callback);

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
  void transitionTo(State newState);
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
  String buildHttpRawResponse(int statusCode, const String& jsonBody, const char* contentType = "application/json");
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
  RequestHandler request_handler_;
  StateChangeCallback state_change_callback_;
  ErrorCallback error_callback_;
  RegisterCallback register_callback_;
  TunnelCallback tunnel_callback_;
  bool ws_connected_prev_;
  State state_prev_;
};

#endif // ORBI_SYNC_NODE_H
