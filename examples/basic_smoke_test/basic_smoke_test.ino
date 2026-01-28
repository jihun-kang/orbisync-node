/**
 * basic_smoke_test.ino
 * 
 * 최소 예제: OrbiSyncNode 라이브러리 기본 동작 테스트
 * - WiFi 연결
 * - Hub 연결 및 세션 관리
 * - 최소한의 Serial 출력
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

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("OrbiSyncNode 기본 테스트 시작");
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
