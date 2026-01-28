#include "OrbiSyncNode.h"

#include <ArduinoJson.hpp>
#include <cstring>
#if defined(ESP32)
#include <esp_system.h>
#endif
#include <algorithm>

// ArduinoJson 7.x 네임스페이스 alias (cpp 파일에서도 사용)
namespace AJ = ArduinoJson;

#define ORBI_SYNC_WS_DEBUG 0

namespace {
  constexpr uint32_t kDefaultPendingRetryMs = 3000;
  constexpr uint32_t kInitialBackoffMs = 1000;
  constexpr uint32_t kMaxBackoffMs = 30000;
}

static OrbiSyncNode* s_ws_instance = nullptr;

static void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (s_ws_instance) {
    s_ws_instance->handleWsEvent(type, payload, length);
  }
}

OrbiSyncNode::OrbiSyncNode(const Config& config)
  : config_(config),
    state_(State::BOOT),
    use_https_(strncmp(config.hubBaseUrl ? config.hubBaseUrl : "", "https", 5) == 0),
    next_wifi_action_ms_(0),
    next_net_action_ms_(0),
    next_command_action_ms_(0),
    wifi_backoff_ms_(kInitialBackoffMs),
    net_backoff_ms_(kInitialBackoffMs),
    register_backoff_ms_(config.registerRetryMs ? config.registerRetryMs : kInitialBackoffMs),
    next_register_action_ms_(0),
    session_expires_at_ms_(0),
    last_heartbeat_ms_(0),
    led_state_(false),
    ws_backoff_ms_(kInitialBackoffMs),
    next_ws_action_ms_(0),
    ws_last_heartbeat_ms_(0),
    ws_connected_(false),
    ws_register_frame_sent_(false),
    stream_open_(false) {
  session_token_[0] = '\0';
  last_error_[0] = '\0';
  node_id_[0] = '\0';
  node_auth_token_[0] = '\0';
  tunnel_url_[0] = '\0';
  active_stream_id_[0] = '\0';
  request_buf_.reserve(1024);
  is_registered_ = !config.enableNodeRegistration;
  ws_extra_headers_.reserve(64);
  applyTlsPolicy();
  if (config_.hubBaseUrl == nullptr || config_.slotId == nullptr) {
    setLastError("설정 오류: hubBaseUrl 또는 slotId 누락");
    state_ = State::ERROR;
  }
  randomSeed(ESP.getChipId() ^ micros());
  s_ws_instance = this;
  ws_client_.onEvent(webSocketEvent);
}

OrbiSyncNode::~OrbiSyncNode() {
  http_.end();
  ws_client_.disconnect();
  ws_client_.loop();
}

bool OrbiSyncNode::beginWiFi(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  wifi_ssid_ = ssid;
  wifi_password_ = pass;
  wifi_backoff_ms_ = kInitialBackoffMs;
  next_wifi_action_ms_ = 0;
  return ensureWiFi();
}

bool OrbiSyncNode::ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifi_backoff_ms_ = kInitialBackoffMs;
    return true;
  }
  uint32_t now = millis();
  if (now < next_wifi_action_ms_) {
    return false;
  }
  if (wifi_ssid_.isEmpty() || wifi_password_.isEmpty()) {
    setLastError("Wi-Fi 인증 정보 부족");
    scheduleNextWiFi(kInitialBackoffMs);
    return false;
  }
  WiFi.begin(wifi_ssid_.c_str(), wifi_password_.c_str());
  scheduleNextWiFi(0);
  return false;
}

bool OrbiSyncNode::applyTlsPolicy() {
#if defined(ESP8266)
  if (use_https_) {
    if (config_.allowInsecureTls) {
      secure_client_.setInsecure();
    } else if (config_.rootCaPem != nullptr) {
      // TODO: BearSSL의 trust anchor 세팅 코드 추가 필요
    }
  }
#else
  if (use_https_) {
    if (config_.allowInsecureTls) {
      secure_client_.setInsecure();
    } else if (config_.rootCaPem != nullptr) {
      secure_client_.setCACert(config_.rootCaPem);
    }
  }
#endif
  return true;
}

bool OrbiSyncNode::sendHello() {
  if (state_ == State::ERROR) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setLastError("HELLO 전 Wi-Fi 끊김");
    scheduleNextNetwork();
    return false;
  }

  String url = String(config_.hubBaseUrl) + "/api/device/hello";
  WiFiClient& client = use_https_ ? static_cast<WiFiClient&>(secure_client_) : insecure_client_;
  if (!http_.begin(client, url)) {
    setLastError("HELLO HTTP 연결 실패");
    scheduleNextNetwork();
    return false;
  }
  http_.setTimeout(15000);
  http_.addHeader("Content-Type", "application/json");

  StaticJsonDocument<768> payload;
  payload["slot_id"] = config_.slotId;
  payload["nonce"] = createNonce();
  payload["firmware"] = config_.firmwareVersion;
  payload["capabilities_hash"] = capabilitiesHash();
  JsonObject device = payload.createNestedObject("device_info");
  device["platform"] = "NodeMCU";
  device["firmware"] = config_.firmwareVersion;

  String body;
  AJ::serializeJson(payload, body);
  int http_code = http_.POST(body);
  DynamicJsonDocument doc(512);
  if (!responseToJson(doc)) {
    scheduleNextNetwork();
    return false;
  }
  http_.end();

  if (http_code < 200 || http_code >= 300) {
    setLastError("HELLO HTTP 오류");
    scheduleNextNetwork();
    return false;
  }

  const char* status = doc["status"];
  if (status == nullptr) {
    setLastError("HELLO 응답에 status 누락");
    scheduleNextNetwork();
    return false;
  }

  uint32_t now = millis();
  if (strcmp(status, "PENDING") == 0 || strcmp(status, "APPROVED") == 0) {
    state_ = State::PENDING_POLL;
    uint32_t retry = doc["retry_after_ms"] | kDefaultPendingRetryMs;
    next_net_action_ms_ = now + retry;
    net_backoff_ms_ = kInitialBackoffMs;
    last_error_[0] = '\0';
    return true;
  }

  if (strcmp(status, "DENIED") == 0) {
    setLastError("HELLO 거절됨");
    clearSession();
    state_ = State::HELLO;
    scheduleNextNetwork();
    return false;
  }

  setLastError("HELLO 상태 알 수 없음");
  scheduleNextNetwork();
  return false;
}

bool OrbiSyncNode::pollSession() {
  if (WiFi.status() != WL_CONNECTED) {
    setLastError("SESSION 전 Wi-Fi 끊김");
    scheduleNextNetwork();
    return false;
  }

  String url = String(config_.hubBaseUrl) + "/api/device/session";
  WiFiClient& client = use_https_ ? static_cast<WiFiClient&>(secure_client_) : insecure_client_;
  if (!http_.begin(client, url)) {
    setLastError("SESSION HTTP 준비 실패");
    scheduleNextNetwork();
    return false;
  }
  http_.setTimeout(15000);
  http_.addHeader("Content-Type", "application/json");

  StaticJsonDocument<256> payload;
  payload["slot_id"] = config_.slotId;
  payload["nonce"] = createNonce();
  String body;
  AJ::serializeJson(payload, body);
  int http_code = http_.POST(body);
  DynamicJsonDocument doc(512);
  if (!responseToJson(doc)) {
    scheduleNextNetwork();
    return false;
  }
  http_.end();

  uint32_t now = millis();
  if (http_code == 401 || http_code == 403) {
    setLastError("SESSION 인증 실패");
    clearSession();
    state_ = State::HELLO;
    scheduleNextNetwork();
    return false;
  }

  if (http_code < 200 || http_code >= 300) {
    setLastError("SESSION HTTP 오류");
    scheduleNextNetwork();
    return false;
  }

  const char* status = doc["status"];
  if (status == nullptr) {
    setLastError("SESSION 응답에 status 없음");
    scheduleNextNetwork();
    return false;
  }

  if (strcmp(status, "PENDING") == 0) {
    uint32_t retry = doc["retry_after_ms"] | kDefaultPendingRetryMs;
    state_ = State::PENDING_POLL;
    next_net_action_ms_ = now + retry;
    return true;
  }

  if (strcmp(status, "GRANTED") == 0) {
    const char* token = doc["session_token"];
    if (token == nullptr || strlen(token) >= sizeof(session_token_)) {
      setLastError("SESSION 토큰 오류");
      clearSession();
      state_ = State::HELLO;
      scheduleNextNetwork();
      return false;
    }
    strncpy(session_token_, token, sizeof(session_token_) - 1);
    session_token_[sizeof(session_token_) - 1] = '\0';
    uint32_t ttl = doc["ttl_seconds"] | 3600;
    session_expires_at_ms_ = now + ttl * 1000;
    state_ = State::ACTIVE;
    last_heartbeat_ms_ = now;
    next_net_action_ms_ = now;
    net_backoff_ms_ = kInitialBackoffMs;
    uint32_t commandDelay = config_.commandPollIntervalMs ? config_.commandPollIntervalMs : config_.heartbeatIntervalMs;
    next_command_action_ms_ = now + commandDelay;
    last_error_[0] = '\0';
    return true;
  }

  if (strcmp(status, "DENIED") == 0) {
    setLastError("SESSION 거절됨");
    clearSession();
    state_ = State::HELLO;
    scheduleNextNetwork();
    return false;
  }

  setLastError("SESSION 상태 알 수 없음");
  scheduleNextNetwork();
  return false;
}

bool OrbiSyncNode::sendHeartbeat() {
  if (!isSessionValid()) {
    setLastError("세션 없어서 Heartbeat 생략");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setLastError("Heartbeat 전 Wi-Fi 끊김");
    return false;
  }

  String url = String(config_.hubBaseUrl) + "/api/device/heartbeat";
  WiFiClient& client = use_https_ ? static_cast<WiFiClient&>(secure_client_) : insecure_client_;
  if (!http_.begin(client, url)) {
    setLastError("Heartbeat HTTP 준비 실패");
    scheduleNextNetwork();
    return false;
  }
  http_.setTimeout(10000);
  http_.addHeader("Content-Type", "application/json");
  if (session_token_[0] != '\0') {
    http_.addHeader("Authorization", String("Bearer ") + session_token_);
  }

  StaticJsonDocument<512> payload;
  payload["slot_id"] = config_.slotId;
  payload["nonce"] = createNonce();
  payload["firmware"] = config_.firmwareVersion;
  payload["uptime_ms"] = millis();
  payload["rssi"] = WiFi.RSSI();
  payload["free_heap"] = ESP.getFreeHeap();
  payload["capabilities_hash"] = capabilitiesHash();
  payload["led_state"] = led_state_;
  String body;
  AJ::serializeJson(payload, body);

  int http_code = http_.POST(body);
  DynamicJsonDocument doc(256);
  if (!responseToJson(doc)) {
    scheduleNextNetwork();
    return false;
  }
  http_.end();

  uint32_t now = millis();
  if (http_code == 401 || http_code == 403) {
    setLastError("Heartbeat 인증 실패");
    clearSession();
    state_ = State::HELLO;
    scheduleNextNetwork();
    return false;
  }
  if (http_code < 200 || http_code >= 300) {
    setLastError("Heartbeat HTTP 오류");
    scheduleNextNetwork();
    return false;
  }

  if (doc.containsKey("ttl_seconds")) {
    uint32_t ttl = doc["ttl_seconds"];
    session_expires_at_ms_ = now + ttl * 1000;
  }

  if (config_.blinkOnHeartbeat) {
    led_state_ = !led_state_;
    pinMode(config_.ledPin, OUTPUT);
    digitalWrite(config_.ledPin, led_state_ ? HIGH : LOW);
  }

  last_heartbeat_ms_ = now;
  net_backoff_ms_ = kInitialBackoffMs;
  next_net_action_ms_ = now + config_.heartbeatIntervalMs;
  return true;
}

bool OrbiSyncNode::pullCommands() {
  if (!config_.enableCommandPolling || !isSessionValid()) {
    return true;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setLastError("Command pull: Wi-Fi 끊김");
    return false;
  }

  String url = String(config_.hubBaseUrl) + "/api/device/commands/pull";
  WiFiClient& client = use_https_ ? static_cast<WiFiClient&>(secure_client_) : insecure_client_;
  if (!http_.begin(client, url)) {
    setLastError("Command pull HTTP 준비 실패");
    scheduleNextNetwork();
    return false;
  }
  http_.setTimeout(10000);
  http_.addHeader("Content-Type", "application/json");
  if (session_token_[0] != '\0') {
    http_.addHeader("Authorization", String("Bearer ") + session_token_);
  }

  StaticJsonDocument<256> payload;
  payload["slot_id"] = config_.slotId;
  payload["nonce"] = createNonce();
  String body;
  AJ::serializeJson(payload, body);

  int http_code = http_.POST(body);
  DynamicJsonDocument doc(768);
  if (!responseToJson(doc)) {
    scheduleNextNetwork();
    return false;
  }
  http_.end();

  if (http_code < 200 || http_code >= 300) {
    setLastError("Command pull HTTP 오류");
    scheduleNextNetwork();
    return false;
  }

  if (!doc.containsKey("commands")) {
    net_backoff_ms_ = kInitialBackoffMs;
    last_error_[0] = '\0';
    return true;
  }

  JsonArray commands = doc["commands"].as<JsonArray>();
  for (JsonObject command : commands) {
    handleCommand(command);
  }

  net_backoff_ms_ = kInitialBackoffMs;
  last_error_[0] = '\0';
  return true;
}

bool OrbiSyncNode::handleCommand(JsonObject command) {
  const char* cmd_id = command["id"];
  const char* action = command["action"];
  if (cmd_id == nullptr || action == nullptr) {
    setLastError("명령 구조 오류");
    return false;
  }
  Serial.printf("[OrbiSync] 명령 수신 id=%s action=%s\n", cmd_id, action);

  String url = String(config_.hubBaseUrl) + "/api/device/commands/ack";
  WiFiClient& client = use_https_ ? static_cast<WiFiClient&>(secure_client_) : insecure_client_;
  if (!http_.begin(client, url)) {
    setLastError("Command ack HTTP 실패");
    return false;
  }
  http_.setTimeout(10000);
  http_.addHeader("Content-Type", "application/json");
  if (session_token_[0] != '\0') {
    http_.addHeader("Authorization", String("Bearer ") + session_token_);
  }

  StaticJsonDocument<256> ackPayload;
  ackPayload["slot_id"] = config_.slotId;
  ackPayload["command_id"] = cmd_id;
  ackPayload["nonce"] = createNonce();
  ackPayload["status"] = "handled";
  String ackBody;
  AJ::serializeJson(ackPayload, ackBody);
  int http_code = http_.POST(ackBody);
  http_.end();

  if (http_code < 200 || http_code >= 300) {
    setLastError("Command ack 실패");
    return false;
  }
  return true;
}

bool OrbiSyncNode::isSessionValid() const {
  if (session_token_[0] == '\0') {
    return false;
  }
  if (session_expires_at_ms_ == 0) {
    return true;
  }
  return millis() < session_expires_at_ms_;
}

void OrbiSyncNode::loopTick() {
  uint32_t now = millis();
  ensureWiFi();

  if (config_.enableTunnel) {
    ws_client_.loop();
    if (!ws_connected_) {
      connectTunnelWs(now);
    }
  }

  if (state_ == State::ERROR) {
    return;
  }

  if (state_ == State::BOOT && WiFi.status() == WL_CONNECTED) {
    state_ = State::HELLO;
    next_net_action_ms_ = now;
  }

  if (state_ == State::ACTIVE && (!isSessionValid() || WiFi.status() != WL_CONNECTED)) {
    clearSession();
    state_ = State::HELLO;
    next_net_action_ms_ = now;
  }

  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  switch (state_) {
    case State::HELLO:
      if (now >= next_net_action_ms_) {
        sendHello();
      }
      break;
    case State::PENDING_POLL:
      if (now >= next_net_action_ms_) {
        pollSession();
      }
      break;
    case State::ACTIVE:
      processActive(now);
      break;
    default:
      break;
  }
}

void OrbiSyncNode::processActive(uint32_t now) {
  registerNodeIfNeeded(now);
  if (config_.enableTunnel) {
    connectTunnelWs(now);
    if (ws_connected_ && (now - ws_last_heartbeat_ms_) >= OrbiSyncNodeConstants::kWsHeartbeatIntervalMs) {
      sendHeartbeatFrame();
    }
  }
  if (now >= next_net_action_ms_) {
    sendHeartbeat();
  }
  if (config_.enableCommandPolling) {
    uint32_t commandDelay = config_.commandPollIntervalMs ? config_.commandPollIntervalMs : config_.heartbeatIntervalMs;
    if (now >= next_command_action_ms_) {
      pullCommands();
      next_command_action_ms_ = now + commandDelay;
    }
  }
}

bool OrbiSyncNode::registerNodeIfNeeded(uint32_t now) {
  if (!config_.enableNodeRegistration) {
    return true;
  }
  if (is_registered_) {
    return true;
  }
  if (now < next_register_action_ms_) {
    return false;
  }

  bool attempted = false;
  bool success = false;
  if (config_.preferRegisterBySlot) {
    if (config_.loginToken && config_.slotId) {
      attempted = true;
      success = registerBySlot();
    }
    if (!success && config_.pairingCode) {
      attempted = true;
      success = registerByPairing();
    }
  } else {
    if (config_.pairingCode) {
      attempted = true;
      success = registerByPairing();
    }
    if (!success && config_.loginToken && config_.slotId) {
      attempted = true;
      success = registerBySlot();
    }
  }

  if (!attempted) {
    setLastError("등록 방식 설정 누락");
    scheduleRegisterRetry(now);
    return false;
  }

  if (success) {
    register_backoff_ms_ = config_.registerRetryMs ? config_.registerRetryMs : kInitialBackoffMs;
    next_register_action_ms_ = now;
    Serial.printf("[OrbiSync] 노드 등록 완료 node_id=%s\n", node_id_);
    is_registered_ = true;
    return true;
  }

  scheduleRegisterRetry(now);
  return false;
}

bool OrbiSyncNode::registerBySlot() {
  if (!config_.slotId || !config_.loginToken) {
    setLastError("slot 기반 등록 정보 부족");
    return false;
  }
  Serial.printf("[OrbiSync] register_by_slot slot=%s\n", config_.slotId);
  String url = String(config_.hubBaseUrl) + "/api/nodes/register_by_slot";
  WiFiClient& client = use_https_ ? static_cast<WiFiClient&>(secure_client_) : insecure_client_;
  if (!http_.begin(client, url)) {
    setLastError("register_by_slot HTTP 준비 실패");
    return false;
  }
  http_.setTimeout(15000);
  http_.addHeader("Content-Type", "application/json");

  StaticJsonDocument<768> payload;
  payload["slot_id"] = config_.slotId;
  payload["login_token"] = config_.loginToken;
  payload["machine_id"] = (config_.machineId && config_.machineId[0]) ? config_.machineId : "";
  String nodeName = (config_.nodeName && config_.nodeName[0]) ? config_.nodeName : String("Node ") + String(ESP.getChipId(), HEX);
  payload["node_name"] = nodeName;
  #if defined(ESP8266)
  payload["platform"] = "esp8266";
  #else
  payload["platform"] = "esp32";
  #endif
  payload["agent_version"] = config_.firmwareVersion;

  String body;
  AJ::serializeJson(payload, body);
  int http_code = http_.POST(body);
  DynamicJsonDocument doc(512);
  if (!responseToJson(doc)) {
    http_.end();
    return false;
  }
  http_.end();

  if (http_code != 201 && http_code != 200) {
    setLastError("register_by_slot HTTP 응답 실패");
    return false;
  }

  const char* nodeId = doc["node_id"];
  const char* auth = doc["node_auth_token"];
  const char* tunnel = doc["tunnel_url"];
  if (nodeId == nullptr || auth == nullptr) {
    setLastError("등록 응답 토큰 누락");
    return false;
  }

  strncpy(node_id_, nodeId, sizeof(node_id_) - 1);
  strncpy(node_auth_token_, auth, sizeof(node_auth_token_) - 1);
  if (tunnel) {
    strncpy(tunnel_url_, tunnel, sizeof(tunnel_url_) - 1);
  } else {
    tunnel_url_[0] = '\0';
  }

  Serial.printf("[OrbiSync] register_by_slot success id=%s auth=%s\n", node_id_, node_auth_token_);
  return true;
}

bool OrbiSyncNode::registerByPairing() {
  if (!config_.pairingCode || !config_.slotId) {
    setLastError("pairing 등록 정보 부족");
    return false;
  }
  Serial.printf("[OrbiSync] register by pairing slot=%s\n", config_.slotId);
  String url = String(config_.hubBaseUrl) + "/api/nodes/register";
  WiFiClient& client = use_https_ ? static_cast<WiFiClient&>(secure_client_) : insecure_client_;
  if (!http_.begin(client, url)) {
    setLastError("pairing HTTP 준비 실패");
    return false;
  }
  http_.setTimeout(15000);
  http_.addHeader("Content-Type", "application/json");
  if (config_.internalKey && config_.internalKey[0]) {
    http_.addHeader("X-Internal-Key", config_.internalKey);
  }

  StaticJsonDocument<768> payload;
  payload["slot_id"] = config_.slotId;
  payload["pairing_code"] = config_.pairingCode;
  JsonObject info = payload.createNestedObject("node_info");
  info["os"] = "arduino";
  #if defined(ESP8266)
  info["arch"] = "esp8266";
  #else
  info["arch"] = "esp32";
  #endif
  info["version"] = config_.firmwareVersion;

  String body;
  AJ::serializeJson(payload, body);
  int http_code = http_.POST(body);
  DynamicJsonDocument doc(512);
  if (!responseToJson(doc)) {
    http_.end();
    return false;
  }
  http_.end();

  if (http_code != 200 && http_code != 201) {
    setLastError("pairing 등록 HTTP 실패");
    return false;
  }

  const char* nodeId = doc["node_id"];
  const char* auth = doc["node_auth_token"];
  const char* tunnel = doc["tunnel_url"];
  if (nodeId == nullptr || auth == nullptr) {
    setLastError("pairing 응답 누락");
    return false;
  }

  strncpy(node_id_, nodeId, sizeof(node_id_) - 1);
  strncpy(node_auth_token_, auth, sizeof(node_auth_token_) - 1);
  if (tunnel) {
    strncpy(tunnel_url_, tunnel, sizeof(tunnel_url_) - 1);
  } else {
    tunnel_url_[0] = '\0';
  }
  Serial.printf("[OrbiSync] pairing 등록 성공 id=%s auth=%s\n", node_id_, node_auth_token_);
  return true;
}

bool OrbiSyncNode::connectTunnelWs(uint32_t now) {
  if (!config_.enableTunnel || !is_registered_ || node_auth_token_[0] == '\0' || tunnel_url_[0] == '\0') {
    return false;
  }
  if (ws_connected_) {
    return true;
  }
  if (now < next_ws_action_ms_) {
    return false;
  }

  String host, path;
  uint16_t port = 0;
  bool secure = false;
  if (!parseTunnelUrl(tunnel_url_, host, port, path, secure)) {
    setLastError("터널 URL 파싱 실패");
    scheduleWsRetry(now);
    return false;
  }

  ws_extra_headers_ = String("Authorization: Bearer ") + node_auth_token_;
  ws_client_.setExtraHeaders(ws_extra_headers_.c_str());
  ws_client_.setReconnectInterval(0);
  if (secure) {
    ws_client_.beginSSL(host.c_str(), port, path.c_str());
  } else {
    ws_client_.begin(host.c_str(), port, path.c_str());
  }

  Serial.printf("[OrbiSync] WS 연결 시도 %s://%s%s\n", secure ? "wss" : "ws", host.c_str(), path.c_str());
  uint32_t delay = ws_backoff_ms_ ? ws_backoff_ms_ : kInitialBackoffMs;
  next_ws_action_ms_ = now + delay;
  ws_backoff_ms_ = min(ws_backoff_ms_ ? ws_backoff_ms_ * 2 : kInitialBackoffMs, kMaxBackoffMs);
  return true;
}

bool OrbiSyncNode::sendRegisterFrame() {
  if (!ws_connected_ || ws_register_frame_sent_) {
    return true;
  }
  if (node_id_[0] == '\0') {
    setLastError("WS register: node_id 없음");
    return false;
  }
  StaticJsonDocument<256> payload;
  payload["action"] = "register";
  payload["node_id"] = node_id_;
  payload["slot_id"] = config_.slotId ? config_.slotId : "";
  payload["machine_id"] = config_.machineId ? config_.machineId : "";
  payload["version"] = config_.firmwareVersion;
  #if defined(ESP8266)
  payload["platform"] = "esp8266";
  #else
  payload["platform"] = "esp32";
  #endif
  payload["timestamp"] = millis();
  String frame;
  AJ::serializeJson(payload, frame);
  if (ORBI_SYNC_WS_DEBUG) {
    Serial.printf("[OrbiSync] WS reg payload: %s\n", frame.c_str());
  }
  bool ok = ws_client_.sendTXT(frame);
  Serial.println("[OrbiSync] WS register frame 전송");
  if (ok) {
    ws_register_frame_sent_ = true;
    ws_last_heartbeat_ms_ = millis();
  } else {
    setLastError("WS register frame 실패");
    scheduleWsRetry(millis());
  }
  return ok;
}

bool OrbiSyncNode::sendHeartbeatFrame() {
  if (!ws_connected_ || !ws_register_frame_sent_) {
    return false;
  }
  StaticJsonDocument<256> payload;
  payload["action"] = "heartbeat";
  payload["node_id"] = node_id_;
  payload["timestamp"] = millis();
  if (config_.slotId) {
    payload["slot_id"] = config_.slotId;
  }
  String frame;
  AJ::serializeJson(payload, frame);
  if (ORBI_SYNC_WS_DEBUG) {
    Serial.printf("[OrbiSync] WS heartbeat payload: %s\n", frame.c_str());
  }
  bool ok = ws_client_.sendTXT(frame);
  Serial.println("[OrbiSync] WS heartbeat 전송");
  if (ok) {
    ws_last_heartbeat_ms_ = millis();
  } else {
    setLastError("WS heartbeat 실패");
    ws_connected_ = false;
    ws_register_frame_sent_ = false;
    ws_client_.disconnect();
    scheduleWsRetry(millis());
  }
  return ok;
}

void OrbiSyncNode::handleWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      ws_connected_ = false;
      ws_register_frame_sent_ = false;
      Serial.println("[OrbiSync] WS 이벤트: 끊김");
      scheduleWsRetry(millis());
      break;
    case WStype_CONNECTED:
      ws_connected_ = true;
      ws_register_frame_sent_ = false;
      ws_last_heartbeat_ms_ = millis();
      ws_backoff_ms_ = kInitialBackoffMs;
      next_ws_action_ms_ = 0;
      Serial.println("[OrbiSync] WS 이벤트: 연결됨");
      sendRegisterFrame();
      break;
    case WStype_TEXT:
      handleWsMessage(payload, length);
      break;
    default:
      break;
  }
}

void OrbiSyncNode::handleWsMessage(const uint8_t* payload, size_t length) {
  DynamicJsonDocument doc(512);
  auto err = AJ::deserializeJson(doc, payload, length);
  if (err) {
    setLastError("WS 메시지 JSON 파싱 실패");
    return;
  }
  const char* type = doc["type"];
  if (type == nullptr) {
    return;
  }
  if (strcmp(type, "control") == 0) {
    handleControl(doc);
  } else if (strcmp(type, "data") == 0) {
    handleData(doc);
  }
}

void OrbiSyncNode::handleControl(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  const char* streamId = doc["stream_id"];
  if (cmd == nullptr || streamId == nullptr) {
    return;
  }
  if (strcmp(cmd, "open_stream") == 0) {
    strncpy(active_stream_id_, streamId, sizeof(active_stream_id_) - 1);
    active_stream_id_[sizeof(active_stream_id_) - 1] = '\0';
    stream_open_ = true;
    request_buf_.clear();
    Serial.printf("[OrbiSync] stream open id=%s\n", active_stream_id_);
  } else if (strcmp(cmd, "close_stream") == 0) {
    if (strcmp(active_stream_id_, streamId) == 0) {
      stream_open_ = false;
      request_buf_.clear();
      Serial.printf("[OrbiSync] stream close id=%s\n", streamId);
    }
  }
}

void OrbiSyncNode::handleData(JsonDocument& doc) {
  if (!stream_open_) {
    return;
  }
  const char* direction = doc["direction"];
  const char* payload64 = doc["payload_base64"];
  if (direction == nullptr || payload64 == nullptr) {
    return;
  }
  if (strcmp(direction, "c2n") != 0) {
    return;
  }
  uint8_t decoded[OrbiSyncNodeConstants::kMaxStreamRequestSize];
  size_t decodedLen = 0;
  if (!base64Decode(payload64, decoded, decodedLen)) {
    setLastError("WS 데이터 base64 디코딩 실패");
    return;
  }
  if (request_buf_.length() + decodedLen > OrbiSyncNodeConstants::kMaxStreamRequestSize) {
    int status = 413;
    String json = "{\"ok\":false,\"error\":\"payload_too_large\"}";
    String raw = buildHttpRawResponse(status, json);
    sendDataFrame(active_stream_id_, (const uint8_t*)raw.c_str(), raw.length());
    stream_open_ = false;
    request_buf_.clear();
    return;
  }
  request_buf_.concat(reinterpret_cast<const char*>(decoded), decodedLen);
  Serial.println("[OrbiSync] data(c2n) 수신");
  tryProcessHttpRequest();
}

bool OrbiSyncNode::tryProcessHttpRequest() {
  int headerEnd = request_buf_.indexOf("\r\n\r\n");
  if (headerEnd < 0) {
    return false;
  }
  String requestLine = request_buf_.substring(0, request_buf_.indexOf("\r\n"));
  int firstSpace = requestLine.indexOf(' ');
  int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  if (firstSpace < 0 || secondSpace < 0) {
    return false;
  }
  String method = requestLine.substring(0, firstSpace);
  String path = requestLine.substring(firstSpace + 1, secondSpace);
  String header = request_buf_.substring(0, headerEnd);
  int contentLen = 0;
  int idx = header.indexOf("Content-Length:");
  if (idx >= 0) {
    int endLine = header.indexOf('\r', idx);
    if (endLine < 0) {
      endLine = header.length();
    }
    String lenStr = header.substring(idx + 15, endLine);
    contentLen = lenStr.toInt();
  }
  int bodyStart = headerEnd + 4;
  int availableBody = request_buf_.length() - bodyStart;
  if (contentLen > availableBody) {
    return false;
  }
  String body;
  if (contentLen > 0) {
    body = request_buf_.substring(bodyStart, bodyStart + contentLen);
  }
  int statusCode = 200;
  String json = routeHttpRequest(method, path, body, statusCode);
  String raw = buildHttpRawResponse(statusCode, json);
  sendDataFrame(active_stream_id_, (const uint8_t*)raw.c_str(), raw.length());
  stream_open_ = false;
  request_buf_.clear();
  active_stream_id_[0] = '\0';
  return true;
}

String OrbiSyncNode::routeHttpRequest(const String& method, const String& path, const String& body, int& statusCode) {
  String lowerPath = path;
  lowerPath.toLowerCase();
  String response;
  uint32_t uptime = (millis() / 1000);
  if (method == "GET" && (lowerPath == "/ping" || lowerPath == "/api/ping")) {
    statusCode = 200;
    response = "{\"ok\":true}";
  } else if (method == "GET" && (lowerPath == "/status" || lowerPath == "/api/status")) {
    statusCode = 200;
    response = "{\"ok\":true,\"uptime_ms\":" + String(millis()) + ",\"node_id\":\"" + node_id_ + "\"}";
  } else {
    statusCode = 404;
    response = "{\"ok\":false,\"error\":\"not_found\"}";
  }
  return response;
}

String OrbiSyncNode::buildHttpRawResponse(int statusCode, const String& jsonBody) {
  const char* statusText = (statusCode == 200) ? "OK" : (statusCode == 404 ? "Not Found" : "Error");
  String raw = "HTTP/1.1 " + String(statusCode) + " " + statusText + "\r\n";
  raw += "Content-Type: application/json\r\n";
  raw += "Content-Length: " + String(jsonBody.length()) + "\r\n";
  raw += "Connection: close\r\n";
  raw += "\r\n";
  raw += jsonBody;
  return raw;
}

bool OrbiSyncNode::sendDataFrame(const char* streamId, const uint8_t* data, size_t len) {
  if (streamId == nullptr || streamId[0] == '\0') {
    return false;
  }
  String encoded = base64Encode(data, len);
  DynamicJsonDocument doc(512);
  doc["type"] = "data";
  doc["stream_id"] = streamId;
  doc["direction"] = "n2c";
  doc["payload_base64"] = encoded;
  String frame;
  AJ::serializeJson(doc, frame);
  if (ORBI_SYNC_WS_DEBUG) {
    Serial.printf("[OrbiSync] WS data payload: %s\n", frame.c_str());
  }
  Serial.println("[OrbiSync] WS data(n2c) 전송");
  return ws_client_.sendTXT(frame);
}

String OrbiSyncNode::base64Encode(const uint8_t* data, size_t length) {
  static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String encoded;
  encoded.reserve(((length + 2) / 3) * 4);
  for (size_t i = 0; i < length; i += 3) {
    uint32_t val = data[i] << 16;
    if (i + 1 < length) val |= data[i + 1] << 8;
    if (i + 2 < length) val |= data[i + 2];
    encoded += table[(val >> 18) & 0x3F];
    encoded += table[(val >> 12) & 0x3F];
    encoded += (i + 1 < length) ? table[(val >> 6) & 0x3F] : '=';
    encoded += (i + 2 < length) ? table[val & 0x3F] : '=';
  }
  return encoded;
}

bool OrbiSyncNode::base64Decode(const char* input, uint8_t* output, size_t& outLen) {
  auto decodeChar = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  int val = 0;
  int valb = -8;
  outLen = 0;
  for (const char* ptr = input; *ptr; ++ptr) {
    if (*ptr == '=') {
      break;
    }
    int d = decodeChar(*ptr);
    if (d < 0) {
      continue;
    }
    val = (val << 6) | d;
    valb += 6;
    if (valb >= 0) {
      output[outLen++] = (val >> valb) & 0xFF;
      valb -= 8;
    }
  }
  return true;
}


void OrbiSyncNode::scheduleWsRetry(uint32_t now) {
  uint32_t delay = ws_backoff_ms_ ? ws_backoff_ms_ : kInitialBackoffMs;
  Serial.printf("[OrbiSync] WS retry in %lu ms\n", delay);
  next_ws_action_ms_ = now + delay;
  ws_backoff_ms_ = min(ws_backoff_ms_ ? ws_backoff_ms_ * 2 : kInitialBackoffMs, kMaxBackoffMs);
  ws_register_frame_sent_ = false;
}

bool OrbiSyncNode::parseTunnelUrl(const char* raw, String& host, uint16_t& port, String& path, bool& secure) const {
  if (raw == nullptr || raw[0] == '\0') {
    return false;
  }
  String url(raw);
  path = "/";
  secure = false;
  if (url.startsWith("wss://")) {
    secure = true;
    url = url.substring(6);
  } else if (url.startsWith("ws://")) {
    url = url.substring(5);
  } else if (url.startsWith("wss:")) {
    secure = true;
    url = url.substring(4);
    if (url.startsWith("//")) {
      url = url.substring(2);
    }
  }
  int slashIdx = url.indexOf('/');
  if (slashIdx >= 0) {
    host = url.substring(0, slashIdx);
    path = url.substring(slashIdx);
  } else {
    host = url;
  }
  int portIdx = host.indexOf(':');
  if (portIdx >= 0) {
    port = host.substring(portIdx + 1).toInt();
    host = host.substring(0, portIdx);
  } else {
    port = secure ? 443 : 80;
  }
  return host.length() > 0;
}

bool OrbiSyncNode::isRegistered() const {
  return is_registered_;
}

const char* OrbiSyncNode::getNodeId() const {
  return node_id_;
}

const char* OrbiSyncNode::getNodeAuthToken() const {
  return node_auth_token_;
}

const char* OrbiSyncNode::getTunnelUrl() const {
  return tunnel_url_;
}

bool OrbiSyncNode::isTunnelConnected() const {
  return ws_connected_;
}

void OrbiSyncNode::scheduleRegisterRetry(uint32_t now) {
  uint32_t delay = register_backoff_ms_ ? register_backoff_ms_ : kInitialBackoffMs;
  next_register_action_ms_ = now + delay;
  register_backoff_ms_ = min(register_backoff_ms_ ? register_backoff_ms_ * 2 : kInitialBackoffMs, kMaxBackoffMs);
}

OrbiSyncNode::State OrbiSyncNode::getState() const {
  return state_;
}

const char* OrbiSyncNode::getLastError() const {
  return last_error_;
}

void OrbiSyncNode::clearSession() {
  session_token_[0] = '\0';
  session_expires_at_ms_ = 0;
}

String OrbiSyncNode::createNonce() {
#if defined(ESP32)
  uint32_t seedA = esp_random();
#else
  uint32_t seedA = static_cast<uint32_t>(random(0xFFFFFFFF)) ^ ESP.getChipId() ^ micros();
#endif
  uint32_t seedB = static_cast<uint32_t>(micros()) ^ (seedA << 5);
  char buffer[24];
  snprintf(buffer, sizeof(buffer), "%08X-%08X", seedA, seedB);
  return String(buffer);
}

void OrbiSyncNode::setLastError(const char* msg) {
  strncpy(last_error_, msg, sizeof(last_error_) - 1);
  last_error_[sizeof(last_error_) - 1] = '\0';
}

void OrbiSyncNode::scheduleNextNetwork(uint32_t delayMs) {
  uint32_t now = millis();
  uint32_t delay = delayMs ? delayMs : net_backoff_ms_;
  next_net_action_ms_ = now + delay;
  net_backoff_ms_ = min(net_backoff_ms_ * 2, kMaxBackoffMs);
}

void OrbiSyncNode::scheduleNextWiFi(uint32_t delayMs) {
  uint32_t now = millis();
  uint32_t delay = delayMs ? delayMs : wifi_backoff_ms_;
  next_wifi_action_ms_ = now + delay;
  wifi_backoff_ms_ = min(wifi_backoff_ms_ * 2, kMaxBackoffMs);
}

String OrbiSyncNode::capabilitiesHash() const {
  uint32_t hash = 0;
  for (uint8_t i = 0; i < config_.capabilityCount; ++i) {
    const char* cap = config_.capabilities[i];
    if (cap == nullptr) continue;
    for (size_t j = 0; j < strlen(cap); ++j) {
      hash = hash * 31 + static_cast<uint8_t>(cap[j]);
    }
  }
  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%08X", hash);
  return String(buffer);
}

bool OrbiSyncNode::responseToJson(JsonDocument& doc) {
  size_t contentLength = http_.getSize();
  if (contentLength > OrbiSyncNodeConstants::kMaxResponseLength) {
    setLastError("응답 크기 제한 초과");
    http_.end();
    return false;
  }

  WiFiClient* stream = http_.getStreamPtr();
  if (stream != nullptr) {
    DeserializationError err = AJ::deserializeJson(doc, *stream);
    if (!err) {
      return true;
    }
  }

  String response = http_.getString();
  if (response.length() > static_cast<int>(OrbiSyncNodeConstants::kMaxResponseLength)) {
    setLastError("응답이 너무 큼");
    return false;
  }
  DeserializationError err = AJ::deserializeJson(doc, response);
  if (err) {
    setLastError("JSON 파싱 실패");
    return false;
  }
  return true;
}
