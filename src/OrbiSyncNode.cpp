/**
 * @file    OrbiSyncNode.cpp
 * @author  jihun kang
 * @date    2026-01-29
 * @brief   OrbiSyncNode 라이브러리의 핵심 구현
 *
 * @details
 * 이 파일은 OrbiSyncNode 클래스의 모든 메서드를 구현합니다.
 * 상태 머신 실행, Hub와의 HTTP/HTTPS 통신, WebSocket 터널링,
 * 노드 등록, 이벤트 콜백 디스패치 등의 로직을 포함합니다.
 *
 * - 상태 머신 구현 (HELLO → PENDING_POLL → ACTIVE 전이 처리)
 * - Hub API 통신 (hello, session, heartbeat, commands, node registration)
 * - WebSocket 터널 연결 및 HTTP 요청/응답 멀티플렉싱
 * - 이벤트 콜백 디스패치 (상태 변경, 에러, 등록, 터널 연결)
 * - 지수 백오프를 통한 네트워크 오류 처리
 *
 * 본 코드는 OrbiSync 오픈소스 프로젝트의 일부입니다.
 * 라이선스 및 사용 조건은 LICENSE 파일을 참고하세요.
 */

 #include "OrbiSyncNode.h"

 #include <ArduinoJson.hpp>
 #include <cstring>
 #if defined(ESP32)
 #include <esp_system.h>
 #endif
 #include <algorithm>
 #include <time.h>
 
 // ArduinoJson 7.x 네임스페이스 alias (cpp 파일에서도 사용)
 namespace AJ = ArduinoJson;
 
 #define ORBI_SYNC_WS_DEBUG 0
 
 namespace {
   constexpr uint32_t kDefaultPendingRetryMs = 3000;
   constexpr uint32_t kInitialBackoffMs = 1000;
   constexpr uint32_t kMaxBackoffMs = 30000;
 
   // ESP8266/ESP32에서 TLS(HTTPS) 첫 연결이 불안정할 때가 많아
   // - NTP 시간 동기화 + (선택) 약간의 안정화 딜레이를 통해 실패(-1/-5)를 줄입니다.
   static void syncTimeOnce() {
     static bool done = false;
     if (done) return;
 
     // UTC 기준(타임존은 로컬 표시만 영향, TLS 검증엔 epoch가 중요)
     configTime(0, 0, "pool.ntp.org", "time.nist.gov");
 
     const uint32_t start = millis();
     time_t now = time(nullptr);
     // 1700000000 ~= 2023-11-14 (대략), 이보다 작으면 시간이 아직 1970년대일 가능성이 큼
     while (now < 1700000000 && (millis() - start) < 10000) {
       delay(200);
      yield();  // 워치독 타이머 리셋 방지
       now = time(nullptr);
     }
 
     Serial.printf("[TIME] epoch=%ld\n", (long)now);
     done = true;
   }
 
   // Stream을 감싸서 "최대 읽기 바이트"를 강제하는 래퍼
   class CountingStream : public Stream {
    public:
     CountingStream(Stream& inner, size_t max_bytes)
         : inner_(inner), max_bytes_(max_bytes) {}
 
     bool overflow() const { return overflow_; }
     size_t bytesRead() const { return bytes_read_; }
 
     int available() override { return inner_.available(); }
 
     int read() override {
       if (bytes_read_ >= max_bytes_) {
         overflow_ = true;
         return -1;
       }
       int c = inner_.read();
       if (c >= 0) bytes_read_++;
       return c;
     }
 
     int peek() override {
       if (bytes_read_ >= max_bytes_) {
         overflow_ = true;
         return -1;
       }
       return inner_.peek();
     }
 
     void flush() override { inner_.flush(); }
 
     size_t write(uint8_t b) override { return inner_.write(b); }
 
    private:
     Stream& inner_;
     size_t max_bytes_;
     size_t bytes_read_ = 0;
     bool overflow_ = false;
   };
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
     stream_open_(false),
     request_handler_(nullptr),
     state_change_callback_(nullptr),
     error_callback_(nullptr),
     register_callback_(nullptr),
     tunnel_callback_(nullptr),
     ws_connected_prev_(false),
     state_prev_(State::BOOT) {
   session_token_[0] = '\0';
   last_error_[0] = '\0';
   node_id_[0] = '\0';
   node_auth_token_[0] = '\0';
   tunnel_url_[0] = '\0';
   active_stream_id_[0] = '\0';
   request_buf_.reserve(1024);
   is_registered_ = !config.enableNodeRegistration;
   ws_extra_headers_.reserve(64);
   machine_id_ptr_ = nullptr;
   node_name_ptr_ = nullptr;
   ensureIdentityInitialized();
   // applyTlsPolicy(); // 생성자에서 제거 - 첫 네트워크 요청 전에 호출
   if (config_.hubBaseUrl == nullptr || config_.slotId == nullptr) {
     setLastError("설정 오류: hubBaseUrl 또는 slotId 누락");
     transitionTo(State::ERROR);
   }
   randomSeed(ESP.getChipId() ^ micros());
   s_ws_instance = this;
   ws_client_.onEvent(webSocketEvent);
 }
 
 OrbiSyncNode::~OrbiSyncNode() {
   http_.end();
   ws_client_.disconnect();
   ws_client_.loop();
 
 #if defined(ESP8266)
   if (ca_cert_ != nullptr) {
     delete ca_cert_;
     ca_cert_ = nullptr;
   }
 #endif
 }
 
 bool OrbiSyncNode::beginWiFi(const char* ssid, const char* pass) {
   WiFi.mode(WIFI_STA);
   wifi_ssid_ = ssid;
   wifi_password_ = pass;
   wifi_backoff_ms_ = kInitialBackoffMs;
   next_wifi_action_ms_ = 0;
   return ensureWiFi();
 }
 
 /**
  * @brief Wi-Fi 연결 상태를 확인하고 필요 시 재연결을 시도한다.
  *
  * @details
  * - 이미 Wi-Fi가 연결되어 있으면 즉시 true를 반환한다.
  * - 아직 재시도 시간이 되지 않았다면 아무 동작 없이 false를 반환한다.
  * - SSID 또는 비밀번호가 비어 있으면 에러를 기록하고 백오프를 설정한다.
  * - 재연결이 가능한 시점이면 WiFi.begin()을 호출해 연결을 시도한다.
  *
  * @return
  *  - true  : Wi-Fi가 이미 연결된 상태
  *  - false : 아직 연결되지 않았거나 재시도 대기 중
  */
  bool OrbiSyncNode::ensureWiFi() {
 
   // 이미 Wi-Fi가 연결되어 있다면
   if (WiFi.status() == WL_CONNECTED) {
     // 재연결 실패 상태를 정상 상태로 되돌리기 위해 백오프 시간을 초기값으로 리셋
     wifi_backoff_ms_ = kInitialBackoffMs;
     return true;
   }
 
   // 현재 시간(ms 단위) 확인
   uint32_t now = millis();
 
   // 아직 다음 Wi-Fi 동작 시점이 아니라면 아무것도 하지 않음
   if (now < next_wifi_action_ms_) {
     return false;
   }
 
   // SSID 또는 비밀번호가 설정되지 않은 경우
   if (wifi_ssid_.isEmpty() || wifi_password_.isEmpty()) {
     // 마지막 에러 메시지 기록
     setLastError("Wi-Fi 인증 정보 부족");
 
     // 초기 백오프 시간 후에 다시 시도하도록 예약
     scheduleNextWiFi(kInitialBackoffMs);
     return false;
   }
 
   // Wi-Fi 연결 시도
   // (비동기 방식이며, 실제 연결 여부는 다음 루프에서 확인됨)
   WiFi.begin(wifi_ssid_.c_str(), wifi_password_.c_str());
 
   // 즉시 다음 루프에서 상태를 다시 확인할 수 있도록 예약
   scheduleNextWiFi(0);
 
   // 아직 연결되지 않았으므로 false 반환
   return false;
 }
 
 bool OrbiSyncNode::applyTlsPolicy() {
   if (!use_https_) return true;
 
   // HTTPS 첫 연결 안정화: 시간 동기화(1회) 후 TLS 설정
   syncTimeOnce();
 
 #if defined(ESP8266)
   // ESP8266(BearSSL)
   if (config_.allowInsecureTls || config_.rootCaPem == nullptr) {
     if (!config_.allowInsecureTls && config_.rootCaPem == nullptr) {
       Serial.println("[TLS] rootCaPem 없음 -> setInsecure()로 폴백 (권장: CA 설정)");
     }
     secure_client_.setInsecure();
     return true;
   }
 
   // CA가 바뀌었으면 갱신
   if (ca_cert_ == nullptr || last_root_ca_pem_ != config_.rootCaPem) {
     if (ca_cert_ != nullptr) {
       delete ca_cert_;
       ca_cert_ = nullptr;
     }
     ca_cert_ = new BearSSL::X509List(config_.rootCaPem);
     last_root_ca_pem_ = config_.rootCaPem;
   }
 
   secure_client_.setTrustAnchors(ca_cert_);
   return true;
 
 #else
   // ESP32(WiFiClientSecure)
   if (config_.allowInsecureTls || config_.rootCaPem == nullptr) {
     if (!config_.allowInsecureTls && config_.rootCaPem == nullptr) {
       Serial.println("[TLS] rootCaPem 없음 -> setInsecure()로 폴백 (권장: CA 설정)");
     }
     secure_client_.setInsecure();
     return true;
   }
 
   secure_client_.setCACert(config_.rootCaPem);
   return true;
 #endif
 }
 
 
 bool OrbiSyncNode::sendHello() {
   Serial.println("\n================ HELLO BEGIN ================");
 
   if (state_ == State::ERROR) {
     Serial.println("[HELLO] state=ERROR -> skip");
     return false;
   }
 
   if (WiFi.status() != WL_CONNECTED) {
     Serial.println("[HELLO] WiFi disconnected");
     setLastError("HELLO 전 Wi-Fi 끊김");
     scheduleNextNetwork();
     return false;
   }
 
   // ✅ (권장) WiFi 연결 직후 첫 HTTPS 튕김 완화
   // - 이미 연결된 상태에서도 라우팅/DNS가 늦게 안정화되는 경우가 있어요 "최초 1회"만 해도 효과 큼
   // delay(1000);
 
   String url = String(config_.hubBaseUrl) + "/api/device/hello";
   Serial.printf("[HELLO] URL: %s\n", url.c_str());
 
   // ✅ TLS/소켓 옵션은 begin 전에!
   if (use_https_) {
     if (!applyTlsPolicy()) {
       Serial.println("[HELLO] TLS 정책 적용 실패");
       setLastError("TLS 정책 적용 실패");
       scheduleNextNetwork();
       return false;
     }
     secure_client_.setTimeout(15000);
     // secure_client_.setBufferSizes(512, 512); // 메모리 여유 있으면 시도
   } else {
     insecure_client_.setTimeout(15000);
   }
 
 // ✅ begin은 분기해서 명확히 (레퍼런스 캐스팅 제거)
   bool ok = false;
   if (use_https_) ok = http_.begin(secure_client_, url);
   else           ok = http_.begin(insecure_client_, url);
 
   if (!ok) {
     Serial.println("[HELLO] http.begin() 실패");
     setLastError("HELLO HTTP 연결 실패");
     scheduleNextNetwork();
     return false;
   }
 
   http_.setTimeout(15000);
   http_.setReuse(false);  // keep-alive 끄기
   http_.setUserAgent("OrbiSyncNode/1.0.0 (ESP8266)");
   http_.addHeader("Content-Type", "application/json");
 
   StaticJsonDocument<768> payload;
   payload["slot_id"] = config_.slotId;
   payload["nonce"] = createNonce();
   payload["firmware"] = config_.firmwareVersion;
   payload["capabilities_hash"] = capabilitiesHash();
 
   JsonObject device = payload.createNestedObject("device_info");
   device["platform"] = "NodeMCU";
   device["firmware"] = config_.firmwareVersion;
 
   String reqBody;
   AJ::serializeJson(payload, reqBody);
 
   Serial.println("[HELLO] >>> REQUEST BODY");
   Serial.println(reqBody);
 
   int http_code = http_.POST(reqBody);
   Serial.printf("[HELLO] HTTP CODE: %d\n", http_code);
 
   if (http_code <= 0) {
     Serial.printf("[HELLO] HTTP ERROR: %s\n",
                   http_.errorToString(http_code).c_str());
     http_.end();
     setLastError("HELLO 네트워크 실패");
     scheduleNextNetwork();
     return false;
   }
 
   String respBody = http_.getString();
   Serial.println("[HELLO] <<< RESPONSE RAW");
   Serial.println(respBody);
 
   DynamicJsonDocument doc(1024);
   DeserializationError derr = deserializeJson(doc, respBody);
   if (derr) {
     Serial.printf("[HELLO] JSON ERROR: %s\n", derr.c_str());
     http_.end();
     setLastError("HELLO JSON 파싱 실패");
     scheduleNextNetwork();
     return false;
   }
 
   Serial.println("[HELLO] <<< RESPONSE JSON (pretty)");
   serializeJsonPretty(doc, Serial);
   Serial.println();
 
   http_.end();
 
   if (http_code < 200 || http_code >= 300) {
     Serial.println("[HELLO] HTTP 오류 코드 (non-2xx)");
     setLastError("HELLO HTTP 오류");
     scheduleNextNetwork();
     return false;
   }
 
   const char* status = doc["status"];
   Serial.printf("[HELLO] parsed status: %s\n", status ? status : "(null)");
 
   if (!status) {
     setLastError("HELLO 응답에 status 누락");
     scheduleNextNetwork();
     return false;
   }
 
   uint32_t now = millis();
 
   if (strcmp(status, "PENDING") == 0 || strcmp(status, "APPROVED") == 0) {
     Serial.println("[HELLO] -> transition PENDING_POLL");
     transitionTo(State::PENDING_POLL);
 
     uint32_t retry = doc["retry_after_ms"] | kDefaultPendingRetryMs;
     Serial.printf("[HELLO] retry_after_ms=%lu\n", retry);
 
     next_net_action_ms_ = now + retry;
     net_backoff_ms_ = kInitialBackoffMs;
     last_error_[0] = '\0';
 
     Serial.println("================ HELLO END (PENDING) ================");
     return true;
   }
 
   if (strcmp(status, "DENIED") == 0) {
     Serial.println("[HELLO] DENIED by server");
 
     setLastError("HELLO 거절됨");
     clearSession();
     transitionTo(State::HELLO);
     scheduleNextNetwork();
 
     Serial.println("================ HELLO END (DENIED) ================");
     return false;
   }
 
   Serial.println("[HELLO] 알 수 없는 status 값");
   setLastError("HELLO 상태 알 수 없음");
   scheduleNextNetwork();
 
   Serial.println("================ HELLO END (UNKNOWN) ================");
   return false;
 }
 
 
 /*
 bool OrbiSyncNode::pollSession() {
    Serial.printf("\n[SESSION] poll begin now=%lu next=%lu state=%d\n",
                millis(), next_net_action_ms_, (int)state_);

   if (WiFi.status() != WL_CONNECTED) {
     setLastError("SESSION 전 Wi-Fi 끊김");
     scheduleNextNetwork();
     return false;
   }
 
   String url = String(config_.hubBaseUrl) + "/api/device/session";
   if (use_https_) {
     if (!applyTlsPolicy()) {
       setLastError("TLS 정책 적용 실패");
       scheduleNextNetwork();
       return false;
     }
   }
   bool ok = false;
   if (use_https_) ok = http_.begin(secure_client_, url);
   else           ok = http_.begin(insecure_client_, url);
   if (!ok) {
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
     transitionTo(State::HELLO);
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
     transitionTo(State::PENDING_POLL);
     next_net_action_ms_ = now + retry;
     return true;
   }
 
   if (strcmp(status, "GRANTED") == 0) {
     const char* token = doc["session_token"];
     if (token == nullptr || strlen(token) >= sizeof(session_token_)) {
       setLastError("SESSION 토큰 오류");
       clearSession();
       transitionTo(State::HELLO);
       scheduleNextNetwork();
       return false;
     }
     strncpy(session_token_, token, sizeof(session_token_) - 1);
     session_token_[sizeof(session_token_) - 1] = '\0';
     uint32_t ttl = doc["ttl_seconds"] | 3600;
     session_expires_at_ms_ = now + ttl * 1000;
     transitionTo(State::ACTIVE);
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
     transitionTo(State::HELLO);
     scheduleNextNetwork();
     return false;
   }
 
   setLastError("SESSION 상태 알 수 없음");
   scheduleNextNetwork();
   return false;
 }
 */

bool OrbiSyncNode::pollSession() {

  uint32_t now = millis();

  Serial.println("\n================ SESSION POLL BEGIN ================");
  Serial.printf("[SESSION] now=%lu next=%lu state=%d wifi=%d heap=%u\n",
                now,
                next_net_action_ms_,
                (int)state_,
                (int)WiFi.status(),
                ESP.getFreeHeap());

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SESSION] WiFi disconnected -> retry later");
    setLastError("SESSION 전 Wi-Fi 끊김");
    scheduleNextNetwork();
    return false;
  }

  String url = String(config_.hubBaseUrl) + "/api/device/session";
  Serial.printf("[SESSION] URL: %s\n", url.c_str());

  // TLS
  if (use_https_) {
    Serial.println("[SESSION] HTTPS mode");
    if (!applyTlsPolicy()) {
      Serial.println("[SESSION] TLS policy FAILED");
      setLastError("TLS 정책 적용 실패");
      scheduleNextNetwork();
      return false;
    }
  } else {
    Serial.println("[SESSION] HTTP mode");
  }

  bool ok = false;
  if (use_https_) ok = http_.begin(secure_client_, url);
  else           ok = http_.begin(insecure_client_, url);

  if (!ok) {
    Serial.println("[SESSION] http.begin() FAILED");
    setLastError("SESSION HTTP 준비 실패");
    scheduleNextNetwork();
    return false;
  }

  http_.setTimeout(15000);
  http_.setReuse(false);
  http_.addHeader("Content-Type", "application/json");

  // ==============================
  // 요청 바디 출력
  // ==============================
  StaticJsonDocument<256> payload;
  payload["slot_id"] = config_.slotId;
  payload["nonce"] = createNonce();

  String body;
  AJ::serializeJson(payload, body);

  Serial.println("[SESSION] >>> REQUEST BODY");
  Serial.println(body);

  // ==============================
  // HTTP 요청
  // ==============================
  int http_code = http_.POST(body);

  Serial.printf("[SESSION] HTTP CODE: %d\n", http_code);

  if (http_code <= 0) {
    Serial.printf("[SESSION] HTTP ERROR: %s\n",
                  http_.errorToString(http_code).c_str());
    http_.end();
    scheduleNextNetwork();
    return false;
  }

  // ==============================
  // RAW 응답 먼저 출력 (디버그 핵심)
  // ==============================
  String raw = http_.getString();

  Serial.println("[SESSION] <<< RESPONSE RAW");
  Serial.println(raw);

  // ==============================
  // JSON 파싱
  // ==============================
  DynamicJsonDocument doc(512);
  auto err = deserializeJson(doc, raw);

  if (err) {
    Serial.printf("[SESSION] JSON PARSE FAILED: %s\n", err.c_str());
    http_.end();
    scheduleNextNetwork();
    return false;
  }

  Serial.println("[SESSION] <<< RESPONSE JSON (pretty)");
  serializeJsonPretty(doc, Serial);
  Serial.println();

  http_.end();

  // ==============================
  // 상태 분기 로그
  // ==============================
  const char* status = doc["status"];

  Serial.printf("[SESSION] parsed status: %s\n", status ? status : "(null)");

  if (!status) {
    Serial.println("[SESSION] status missing -> retry");
    scheduleNextNetwork();
    return false;
  }

  // ------------------------------
  // PENDING
  // ------------------------------
  if (strcmp(status, "PENDING") == 0) {
    uint32_t retry = doc["retry_after_ms"] | kDefaultPendingRetryMs;

    Serial.printf("[SESSION] PENDING -> retry_after_ms=%lu\n", retry);

    transitionTo(State::PENDING_POLL);
    next_net_action_ms_ = now + retry;

    Serial.println("================ SESSION END (PENDING) ================");
    return true;
  }

  // ------------------------------
  // GRANTED (ACTIVE 진입)
  // ------------------------------
  if (strcmp(status, "GRANTED") == 0) {
    const char* token = doc["session_token"];

    Serial.printf("[SESSION] GRANTED token_len=%d\n",
                  token ? strlen(token) : -1);

    if (!token || strlen(token) >= sizeof(session_token_)) {
      Serial.println("[SESSION] token invalid -> reset");
      scheduleNextNetwork();
      return false;
    }

    strncpy(session_token_, token, sizeof(session_token_) - 1);

    uint32_t ttl = doc["ttl_seconds"] | 3600;

    Serial.printf("[SESSION] TTL=%lu sec -> ACTIVE MODE\n", ttl);

    session_expires_at_ms_ = now + ttl * 1000;

    transitionTo(State::ACTIVE);

    last_heartbeat_ms_ = now;
    next_net_action_ms_ = now;

    Serial.println("================ SESSION END (GRANTED -> ACTIVE) ================");
    return true;
  }

  // ------------------------------
  // DENIED
  // ------------------------------
  if (strcmp(status, "DENIED") == 0) {
    Serial.println("[SESSION] DENIED -> back to HELLO");

    clearSession();
    transitionTo(State::HELLO);
    scheduleNextNetwork();

    Serial.println("================ SESSION END (DENIED) ================");
    return false;
  }

  // ------------------------------
  // UNKNOWN
  // ------------------------------
  Serial.println("[SESSION] UNKNOWN STATUS -> retry");
  scheduleNextNetwork();

  Serial.println("================ SESSION END (UNKNOWN) ================");
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
   if (use_https_) {
     if (!applyTlsPolicy()) {
       setLastError("TLS 정책 적용 실패");
       scheduleNextNetwork();
       return false;
     }
   }
   bool ok = false;
   if (use_https_) ok = http_.begin(secure_client_, url);
   else           ok = http_.begin(insecure_client_, url);
   if (!ok) {
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
     transitionTo(State::HELLO);
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
   if (use_https_) {
     if (!applyTlsPolicy()) {
       setLastError("TLS 정책 적용 실패");
       scheduleNextNetwork();
       return false;
     }
   }
   bool ok = false;
   if (use_https_) ok = http_.begin(secure_client_, url);
   else           ok = http_.begin(insecure_client_, url);
   if (!ok) {
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
 
   String url = String(config_.hubBaseUrl) + "/api/device/commands/ack";
   if (use_https_) {
     if (!applyTlsPolicy()) {
       setLastError("TLS 정책 적용 실패");
       return false;
     }
   }
   bool ok = false;
   if (use_https_) ok = http_.begin(secure_client_, url);
   else           ok = http_.begin(insecure_client_, url);
   if (!ok) {
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
 
   // WS 연결 상태 변화 감지
   bool ws_connected_current = ws_connected_;
   if (config_.enableTunnel) {
     ws_client_.loop();
     if (!ws_connected_) {
       connectTunnelWs(now);
     }
   }
   if (ws_connected_current != ws_connected_) {
     // WS 연결 상태 변화는 handleWsEvent에서 처리됨
   }
 
   if (state_ == State::ERROR) {
     return;
   }
 
   if (state_ == State::BOOT && WiFi.status() == WL_CONNECTED) {
     transitionTo(State::HELLO);
     next_net_action_ms_ = now;
   }
 
   if (state_ == State::ACTIVE && (!isSessionValid() || WiFi.status() != WL_CONNECTED)) {
     clearSession();
     transitionTo(State::HELLO);
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
     is_registered_ = true;
     // 등록 성공 콜백 호출
     if (register_callback_ != nullptr && node_id_[0] != '\0') {
       register_callback_(node_id_);
     }
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
   String url = String(config_.hubBaseUrl) + "/api/nodes/register_by_slot";
   if (use_https_) {
     if (!applyTlsPolicy()) {
       setLastError("TLS 정책 적용 실패");
       return false;
     }
   }
   bool ok = false;
   if (use_https_) ok = http_.begin(secure_client_, url);
   else           ok = http_.begin(insecure_client_, url);
   if (!ok) {
     setLastError("register_by_slot HTTP 준비 실패");
     return false;
   }
   http_.setTimeout(15000);
   http_.addHeader("Content-Type", "application/json");
 
   StaticJsonDocument<768> payload;
   payload["slot_id"] = config_.slotId;
   payload["login_token"] = (config_.loginToken && config_.loginToken[0]) ? config_.loginToken : "";
   // machine_id / node_name은 라이브러리 내부에서 (prefix + 고유 suffix)로 자동 생성
   payload["machine_id"] = machine_id_ptr_ ? machine_id_ptr_ : "";
   payload["node_name"]  = node_name_ptr_  ? node_name_ptr_  : "";
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
 
   return true;
 }
 
 bool OrbiSyncNode::registerByPairing() {
   if (!config_.pairingCode || !config_.slotId) {
     setLastError("pairing 등록 정보 부족");
     return false;
   }
   String url = String(config_.hubBaseUrl) + "/api/nodes/register";
   if (use_https_) {
     if (!applyTlsPolicy()) {
       setLastError("TLS 정책 적용 실패");
       return false;
     }
   }
   bool ok = false;
   if (use_https_) ok = http_.begin(secure_client_, url);
   else           ok = http_.begin(insecure_client_, url);
   if (!ok) {
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
   payload["machine_id"] = machine_id_ptr_ ? machine_id_ptr_ : "";
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
       if (ws_connected_) {
         ws_connected_prev_ = ws_connected_;
       }
       ws_connected_ = false;
       ws_register_frame_sent_ = false;
       if (ws_connected_prev_) {
         ws_connected_prev_ = false;
         // 터널 해제 콜백 호출
         if (tunnel_callback_ != nullptr) {
           tunnel_callback_(false, tunnel_url_);
         }
       }
       scheduleWsRetry(millis());
       break;
     case WStype_CONNECTED:
       if (!ws_connected_) {
         ws_connected_prev_ = false;
       }
       ws_connected_ = true;
       ws_register_frame_sent_ = false;
       ws_last_heartbeat_ms_ = millis();
       ws_backoff_ms_ = kInitialBackoffMs;
       next_ws_action_ms_ = 0;
       if (!ws_connected_prev_) {
         ws_connected_prev_ = true;
         // 터널 연결 콜백 호출
         if (tunnel_callback_ != nullptr) {
           tunnel_callback_(true, tunnel_url_);
         }
       }
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
   } else if (strcmp(cmd, "close_stream") == 0) {
     if (strcmp(active_stream_id_, streamId) == 0) {
       stream_open_ = false;
       request_buf_.clear();
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
   String jsonBody;
   const char* contentType = "application/json";
   
   // 먼저 등록된 handler 호출
   bool useHandler = false;
   if (request_handler_ != nullptr) {
     Request req;
     req.proto = Protocol::HTTP;
     req.method = method.c_str();
     req.path = path.c_str();
     req.body = reinterpret_cast<const uint8_t*>(body.c_str());
     req.body_len = body.length();
     
     Response resp;
     resp.status = 200;
     resp.content_type = "application/json";
     resp.body = nullptr;
     resp.body_len = 0;
     
     bool handled = request_handler_(req, resp);
     if (handled && resp.body != nullptr && resp.body_len > 0) {
       // handler가 true 반환하고 응답 제공: 그 응답 사용
       statusCode = resp.status;
       contentType = resp.content_type ? resp.content_type : "application/json";
       // ESP8266 메모리 고려: String 대신 직접 복사
       jsonBody.reserve(resp.body_len + 1);
       jsonBody = "";
       for (size_t i = 0; i < resp.body_len; i++) {
         jsonBody += static_cast<char>(resp.body[i]);
       }
       useHandler = true;
     }
   }
   
   // handler가 처리하지 않았거나 미등록: 기본 라우팅으로 fallback
   if (!useHandler) {
     jsonBody = routeHttpRequest(method, path, body, statusCode);
   }
   
   String raw = buildHttpRawResponse(statusCode, jsonBody, contentType);
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
 
 String OrbiSyncNode::buildHttpRawResponse(int statusCode, const String& jsonBody, const char* contentType) {
   const char* statusText = (statusCode == 200) ? "OK" : (statusCode == 404 ? "Not Found" : "Error");
   String raw = "HTTP/1.1 " + String(statusCode) + " " + statusText + "\r\n";
   raw += "Content-Type: " + String(contentType ? contentType : "application/json") + "\r\n";
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
   if (msg == nullptr) {
     return;
   }
   bool error_changed = (strcmp(last_error_, msg) != 0);
   strncpy(last_error_, msg, sizeof(last_error_) - 1);
   last_error_[sizeof(last_error_) - 1] = '\0';
   if (error_changed && error_callback_ != nullptr) {
     error_callback_(last_error_);
   }
 }
 
 void OrbiSyncNode::transitionTo(State newState) {
   if (state_ != newState) {
     State oldState = state_;
     state_prev_ = state_;
     state_ = newState;
     if (state_change_callback_ != nullptr) {
       state_change_callback_(oldState, newState);
     }
   }
 }
 
 void OrbiSyncNode::onRequest(RequestCallback callback) {
   request_handler_ = callback;
 }
 
 void OrbiSyncNode::onStateChange(StateChangeCallback callback) {
   state_change_callback_ = callback;
 }
 
 void OrbiSyncNode::onError(ErrorCallback callback) {
   error_callback_ = callback;
 }
 
 void OrbiSyncNode::onRegistered(RegisterCallback callback) {
   register_callback_ = callback;
 }
 
 void OrbiSyncNode::onTunnelChange(TunnelCallback callback) {
   tunnel_callback_ = callback;
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
 
 
 void OrbiSyncNode::ensureIdentityInitialized() {
   if (machine_id_ptr_ != nullptr && node_name_ptr_ != nullptr) {
     return;
   }
 
   const String suffix = makeUniqueSuffix();
 
   const char* midPrefix =
       (config_.machineIdPrefix && config_.machineIdPrefix[0]) ? config_.machineIdPrefix :
       (config_.machineId && config_.machineId[0]) ? config_.machineId : "";
 
   const char* namePrefix =
       (config_.nodeNamePrefix && config_.nodeNamePrefix[0]) ? config_.nodeNamePrefix :
       (config_.nodeName && config_.nodeName[0]) ? config_.nodeName : "Node-";
 
   if (config_.appendUniqueSuffix) {
     machine_id_str_ = String(midPrefix) + suffix;
     node_name_str_  = String(namePrefix) + suffix;
     machine_id_ptr_ = machine_id_str_.c_str();
     node_name_ptr_  = node_name_str_.c_str();
   } else {
     // 사용자가 완전한 문자열(고정값)을 직접 주고 싶을 때 사용
     machine_id_ptr_ = midPrefix;
     node_name_ptr_  = namePrefix;
   }
 }
 
 String OrbiSyncNode::makeUniqueSuffix() const {
   if (config_.useMacForUniqueId) {
     String mac = WiFi.macAddress(); // "AA:BB:CC:DD:EE:FF"
     if (mac.length() > 0) {
       mac.replace(":", "");
       mac.toLowerCase();
       return mac;
     }
   }
 
 #if defined(ESP8266)
   return String(ESP.getChipId(), HEX);
 #elif defined(ESP32)
   char buf[17];
   snprintf(buf, sizeof(buf), "%llx", (unsigned long long)ESP.getEfuseMac());
   return String(buf);
 #else
   return String(millis(), HEX);
 #endif
 }
 
 const char* OrbiSyncNode::getMachineId() const {
   const_cast<OrbiSyncNode*>(this)->ensureIdentityInitialized();
   return machine_id_ptr_ ? machine_id_ptr_ : "";
 }
 
 const char* OrbiSyncNode::getNodeName() const {
   const_cast<OrbiSyncNode*>(this)->ensureIdentityInitialized();
   return node_name_ptr_ ? node_name_ptr_ : "";
 }
 
 void OrbiSyncNode::setLoginToken(const char* loginToken) {
   if (loginToken == nullptr) {
     login_token_str_ = "";
     config_.loginToken = nullptr;
     return;
   }
   // 문자열 수명 보장(외부 포인터를 그대로 들고 있지 않음)
   login_token_str_ = loginToken;
   config_.loginToken = login_token_str_.c_str();
 }
 
 
 
 bool OrbiSyncNode::responseToJson(JsonDocument& doc) {
   // NOTE:
   // - HTTPClient::getSize()는 Content-Length를 int로 반환합니다.
   // - 서버가 Content-Length를 주지 않으면 -1이 올 수 있는데,
   //   이를 size_t로 받으면 매우 큰 값으로 변환되어 "응답 크기 제한 초과"가 오탐지됩니다.
   const int contentLength = http_.getSize();
 
   if (contentLength > 0 &&
       contentLength > static_cast<int>(OrbiSyncNodeConstants::kMaxResponseLength)) {
     Serial.printf("[ERROR] 응답 크기 제한 초과 (Content-Length=%d, limit=%u)\n",
                   contentLength,
                   static_cast<unsigned>(OrbiSyncNodeConstants::kMaxResponseLength));
     setLastError("응답 크기 제한 초과");
     http_.end();
     return false;
   }
 
   WiFiClient* stream = http_.getStreamPtr();
   if (stream == nullptr) {
     setLastError("응답 스트림 없음");
     http_.end();
     return false;
   }
 
   // Content-Length가 없거나(-1) 큰 경우에도, 실제 읽는 바이트를 제한하며 파싱합니다.
   CountingStream limited(*stream, OrbiSyncNodeConstants::kMaxResponseLength);
 
   DeserializationError err = AJ::deserializeJson(doc, limited);
   if (!err && !limited.overflow()) {
     return true;
   }
 
   if (limited.overflow()) {
     Serial.printf("[ERROR] 응답 크기 제한 초과 (read=%u, limit=%u)\n",
                   static_cast<unsigned>(limited.bytesRead()),
                   static_cast<unsigned>(OrbiSyncNodeConstants::kMaxResponseLength));
     setLastError("응답 크기 제한 초과");
     http_.end();
     return false;
   }
 
   // 스트림 파싱 실패 시, 디버깅을 위해 제한 내에서 문자열로 한 번 더 시도합니다.
   String response = http_.getString();
   if (response.length() >
       static_cast<int>(OrbiSyncNodeConstants::kMaxResponseLength)) {
     setLastError("응답 크기 제한 초과");
     http_.end();
     return false;
   }
 
   err = AJ::deserializeJson(doc, response);
   if (err) {
     Serial.printf("[ERROR] JSON parse failed: %s\n", err.c_str());
     setLastError("JSON 파싱 실패");
     http_.end();
     return false;
   }
 
   return true;
 }