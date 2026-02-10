/**
 * @file    basic_smoke_test.ino
 * @author  jihun kang
 * @date    2026-01-29
 * @brief   OrbiSyncNode 라이브러리의 최소 동작 테스트 예제
 *
 * @details
 * 이 파일은 OrbiSyncNode 라이브러리의 기본 기능을 최소한의 코드로 테스트하는 예제입니다.
 * WiFi 연결, Hub 통신, 세션 관리, 노드 등록, WebSocket 터널링의 기본 흐름을 검증합니다.
 *
 * - WiFi 연결 및 Hub 초기화
 * - 상태 머신 기반 인증/세션 관리
 * - 이벤트 콜백 등록 (상태 변경, 에러, HTTP 요청)
 * - 최소한의 Serial 로그 출력
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
const char* capabilities[] = {"heartbeat", "commands"};

// Node 등록 설정
const char* LOGIN_TOKEN = "SLOT_LOGIN_TOKEN";
const char* MACHINE_ID = "orbi-basic-node";
const char* NODE_NAME = "BasicNodeMCU";

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

// HTTP 요청 처리 핸들러 예제
// ESP8266 메모리 고려: Response.body는 static const char[] 사용
bool handleRequest(const OrbiSyncNode::Request& req, OrbiSyncNode::Response& resp) {
  // /api/status 엔드포인트 처리 예제
  if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/api/status") == 0) {
    static const char status_json[] = "{\"ok\":true,\"uptime_ms\":12345,\"node_id\":\"test\"}";
    resp.status = 200;
    resp.content_type = "application/json";
    resp.body = reinterpret_cast<const uint8_t*>(status_json);
    resp.body_len = strlen(status_json);
    return true; // 처리 완료
  }
  
  // 다른 경로는 기본 라우팅으로 fallback
  return false;
}

// 상태 변화 콜백 예제
void onStateChanged(OrbiSyncNode::State oldState, OrbiSyncNode::State newState) {
  Serial.print("[StateChange] ");
  Serial.print(static_cast<int>(oldState));
  Serial.print(" -> ");
  Serial.println(static_cast<int>(newState));
}

// 에러 콜백 예제
void onErrorOccurred(const char* error) {
  Serial.print("[Error] ");
  Serial.println(error);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("OrbiSyncNode 기본 테스트 시작");
  
  // 콜백 등록
  node.onRequest(handleRequest);
  node.onStateChange(onStateChanged);
  node.onError(onErrorOccurred);
  
  node.beginWiFi(WIFI_SSID, WIFI_PASS);
}

void loop() {
  node.loopTick();
  
  // 최소한의 상태 출력 (1초마다)
  static uint32_t lastPrintMs = 0;
  uint32_t now = millis();
  if (now - lastPrintMs >= 1000) {
    lastPrintMs = now;
    
    Serial.print("상태: ");
    Serial.print(static_cast<int>(node.getState()));
    
    if (node.isTunnelConnected()) {
      Serial.print(" | WS: 연결됨");
    }
    
    if (node.isRegistered()) {
      Serial.print(" | Node: ");
      Serial.print(node.getNodeId());
    }
    
    const char* err = node.getLastError();
    if (err && err[0]) {
      Serial.print(" | 오류: ");
      Serial.print(err);
    }
    
    Serial.println();
  }
  
  delay(10);
}
