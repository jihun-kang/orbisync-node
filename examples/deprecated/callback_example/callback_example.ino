/**
 * @file    callback_example.ino
 * @author  jihun kang
 * @date    2026-01-29
 * @brief   OrbiSyncNode 라이브러리의 콜백 기반 서비스 처리 예제
 *
 * @details
 * 이 파일은 OrbiSyncNode 라이브러리의 이벤트 콜백 시스템을 활용한 예제입니다.
 * 터널을 통해 들어온 HTTP 요청을 사용자 정의 핸들러로 처리하고,
 * 상태 변경 및 에러 이벤트를 콜백으로 수신합니다.
 *
 * - onRequest() 콜백을 통한 커스텀 HTTP 엔드포인트 처리 (/api/status, /api/ping, /api/led)
 * - onStateChange() 콜백을 통한 상태 변화 감지 및 로깅
 * - onError() 콜백을 통한 에러 처리
 * - ESP8266 메모리 안정성을 위한 정적 버퍼 사용 (static const char[])
 *
 * 본 코드는 OrbiSync 오픈소스 프로젝트의 일부입니다.
 * 라이선스 및 사용 조건은 LICENSE 파일을 참고하세요.
 */

#include <ESP8266WiFi.h>
#include <OrbiSyncNode.h>

// WiFi 설정
const char* WIFI_SSID = "your_ssid";
const char* WIFI_PASS = "your_password";

// Hub 설정
const char* HUB_BASE_URL = "http://192.168.0.100:8000";
const char* SLOT_ID = "slot_node";
const char* FIRMWARE_VERSION = "1.0.0";

// Capabilities
const char* capabilities[] = {"heartbeat", "commands", "tunnel"};

// Node 등록 설정
const char* LOGIN_TOKEN = "SLOT_LOGIN_TOKEN";
const char* MACHINE_ID = "orbi-callback-node";
const char* NODE_NAME = "CallbackNodeMCU";

// OrbiSyncNode 설정
const OrbiSyncNode::Config clientConfig = {
  HUB_BASE_URL,
  SLOT_ID,
  FIRMWARE_VERSION,
  capabilities,
  static_cast<uint8_t>(sizeof(capabilities) / sizeof(capabilities[0])),
  5000,              // heartbeatIntervalMs
  LED_BUILTIN,       // ledPin
  false,             // blinkOnHeartbeat
  true,              // allowInsecureTls
  nullptr,           // rootCaPem
  true,              // enableCommandPolling
  10000,             // commandPollIntervalMs
  LOGIN_TOKEN,
  MACHINE_ID,
  NODE_NAME,
  nullptr,           // pairingCode
  nullptr,           // internalKey
  true,              // enableNodeRegistration
  4000,              // registerRetryMs
  true,              // preferRegisterBySlot
  true               // enableTunnel
};

OrbiSyncNode node(clientConfig);

// ============================================
// HTTP 요청 처리 핸들러
// ============================================
bool handleRequest(const OrbiSyncNode::Request& req, OrbiSyncNode::Response& resp) {
  // ESP8266 메모리 고려: static const char[] 사용
  static const char json_ok[] = "{\"ok\":true}";
  static const char json_not_found[] = "{\"ok\":false,\"error\":\"not_found\"}";
  static const char json_bad_method[] = "{\"ok\":false,\"error\":\"method_not_allowed\"}";
  
  // GET /api/status - 노드 상태 조회
  if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/status") == 0) {
    // 실제 구현에서는 동적으로 JSON 생성 가능 (하지만 메모리 주의)
    // 여기서는 예제로 static 문자열 사용
    static char status_json[128];
    snprintf(status_json, sizeof(status_json),
             "{\"ok\":true,\"uptime_ms\":%lu,\"node_id\":\"%s\",\"state\":%d}",
             millis(), node.getNodeId(), static_cast<int>(node.getState()));
    
    resp.status = 200;
    resp.content_type = "application/json";
    resp.body = reinterpret_cast<const uint8_t*>(status_json);
    resp.body_len = strlen(status_json);
    return true; // 처리 완료
  }
  
  // GET /api/ping - 핑 응답
  if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/ping") == 0) {
    resp.status = 200;
    resp.content_type = "application/json";
    resp.body = reinterpret_cast<const uint8_t*>(json_ok);
    resp.body_len = strlen(json_ok);
    return true;
  }
  
  // POST /api/led - LED 제어 (예제)
  if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/api/led") == 0) {
    // body 파싱 (간단한 예제)
    if (req.body != nullptr && req.body_len > 0) {
      // 실제로는 JSON 파싱 필요하지만 예제에서는 생략
      // LED 제어 로직
      static const char json_led_on[] = "{\"ok\":true,\"led\":\"on\"}";
      static const char json_led_off[] = "{\"ok\":true,\"led\":\"off\"}";
      
      resp.status = 200;
      resp.content_type = "application/json";
      resp.body = reinterpret_cast<const uint8_t*>(json_led_on);
      resp.body_len = strlen(json_led_on);
      return true;
    }
    
    resp.status = 400;
    resp.content_type = "application/json";
    resp.body = reinterpret_cast<const uint8_t*>(json_bad_method);
    resp.body_len = strlen(json_bad_method);
    return true;
  }
  
  // 처리하지 않음: 기본 라우팅으로 fallback
  return false;
}

// ============================================
// 상태 변화 콜백
// ============================================
void onStateChanged(OrbiSyncNode::State oldState, OrbiSyncNode::State newState) {
  const char* stateNames[] = {"BOOT", "HELLO", "PENDING_POLL", "ACTIVE", "ERROR"};
  Serial.print("[StateChange] ");
  Serial.print(stateNames[static_cast<int>(oldState)]);
  Serial.print(" -> ");
  Serial.println(stateNames[static_cast<int>(newState)]);
}

// ============================================
// 에러 콜백
// ============================================
void onErrorOccurred(const char* error) {
  Serial.print("[Error] ");
  Serial.println(error);
}

// ============================================
// setup / loop
// ============================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== OrbiSyncNode 콜백 예제 ===");
  
  // 콜백 등록
  node.onRequest(handleRequest);
  node.onStateChange(onStateChanged);
  node.onError(onErrorOccurred);
  
  Serial.println("콜백 등록 완료");
  Serial.println("WiFi 연결 시작...");
  
  node.beginWiFi(WIFI_SSID, WIFI_PASS);
}

void loop() {
  node.loopTick();
  
  // 최소한의 상태 출력 (5초마다)
  static uint32_t lastPrintMs = 0;
  uint32_t now = millis();
  if (now - lastPrintMs >= 5000) {
    lastPrintMs = now;
    
    Serial.print("[Status] State=");
    Serial.print(static_cast<int>(node.getState()));
    
    if (node.isTunnelConnected()) {
      Serial.print(" | WS=CONNECTED");
    }
    
    if (node.isRegistered()) {
      Serial.print(" | Node=");
      Serial.print(node.getNodeId());
    }
    
    Serial.println();
  }
  
  delay(10);
}
