/**
 * @file   basic_connect.ino
 * @brief  OrbiSync ESP32 Node 기본 연결 예제
 * @details
 * - WiFi 연결 및 Hub 접속
 * - 세션/터널 연결 및 register 성공 로그
 * - 최소한의 설정으로 동작 확인
 * 
 * 설정 필요:
 * - WIFI_SSID, WIFI_PASS: WiFi 자격증명
 * - HUB_BASE_URL: Hub 서버 주소 (예: "https://hub.orbisync.io")
 * - SLOT_ID: 슬롯 ID
 * - LOGIN_TOKEN: 슬롯 로그인 토큰 (또는 PAIRING_CODE)
 */

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#elif defined(ESP32)
  #include <WiFi.h>
#else
  #error "Unsupported board"
#endif

#include <OrbiSyncNode.h>

// ---- 설정 (secrets.h 또는 여기에 직접 입력) ----
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* HUB_BASE_URL = "https://hub.orbisync.io";
const char* SLOT_ID = "YOUR_SLOT_ID";
const char* FIRMWARE_VERSION = "1.0.0";

// 슬롯 등록용 토큰 (LOGIN_TOKEN 또는 PAIRING_CODE 중 하나)
const char* LOGIN_TOKEN = "YOUR_LOGIN_TOKEN";  // 또는 nullptr
const char* PAIRING_CODE = nullptr;  // 또는 페어링 코드

// Capabilities
const char* capabilities[] = {"heartbeat", "commands", "api"};
const uint8_t capabilityCount = sizeof(capabilities) / sizeof(capabilities[0]);

// LED 핀 (ESP32는 보통 GPIO 2)
static const uint8_t LED_PIN = 2;

// ---- OrbiSyncNode 설정 ----
static OrbiSyncNode::Config makeConfig() {
  OrbiSyncNode::Config cfg{};
  
  // 필수 설정
  cfg.hubBaseUrl = HUB_BASE_URL;
  cfg.slotId = SLOT_ID;
  cfg.firmwareVersion = FIRMWARE_VERSION;
  
  // Capabilities
  cfg.capabilities = capabilities;
  cfg.capabilityCount = capabilityCount;
  
  // Heartbeat
  cfg.heartbeatIntervalMs = 5000;
  cfg.ledPin = LED_PIN;
  cfg.blinkOnHeartbeat = false;
  
  // TLS (개발용)
  cfg.allowInsecureTls = true;
  cfg.rootCaPem = nullptr;
  
  // Commands
  cfg.enableCommandPolling = true;
  cfg.commandPollIntervalMs = 10000;
  
  // Identity
  cfg.machineIdPrefix = "node-";
  cfg.nodeNamePrefix = "Node-";
  cfg.appendUniqueSuffix = true;
  cfg.useMacForUniqueId = true;
  
  // Registration & Tunnel
  cfg.enableTunnel = true;
  cfg.enableNodeRegistration = true;
  cfg.registerRetryMs = 4000;
  cfg.preferRegisterBySlot = true;
  
  // Self Approve
  cfg.enableSelfApprove = true;
  cfg.approveEndpointPath = "/api/device/approve";
  cfg.selfApproveKey = "YOUR_KEY";  // 서버가 검증하면 필수
  cfg.approveRetryMs = 3000;
  
  // Session
  cfg.sessionEndpointPath = "/api/device/session";
  
  // Debug
  cfg.debugHttp = true;
  cfg.maskMacInLogs = false;
  
  // Auth
  cfg.loginToken = LOGIN_TOKEN;
  cfg.pairingCode = PAIRING_CODE;
  cfg.internalKey = nullptr;
  
  // Tunnel
  cfg.maxTunnelBodyBytes = 4096;
  cfg.tunnelReconnectMs = 5000;
  
  return cfg;
}

static const OrbiSyncNode::Config nodeConfig = makeConfig();
static OrbiSyncNode::OrbiSyncNode node(nodeConfig);

// ---- 콜백 함수 ----
/// 상태 변경 콜백
static void onStateChange(OrbiSyncNode::State oldState, OrbiSyncNode::State newState) {
  const char* stateNames[] = {
    "BOOT", "HELLO", "PAIR_SUBMIT", "PENDING_POLL", "GRANTED",
    "ACTIVE", "TUNNEL_CONNECTING", "TUNNEL_CONNECTED", "ERROR"
  };
  Serial.printf("[STATE] %s -> %s\n",
    stateNames[static_cast<int>(oldState)],
    stateNames[static_cast<int>(newState)]);
}

/// 에러 콜백
static void onError(const char* error) {
  if (error && error[0]) {
    Serial.printf("[ERROR] %s\n", error);
  }
}

/// 노드 등록 완료 콜백
static void onRegistered(const char* nodeId) {
  if (nodeId && nodeId[0]) {
    Serial.printf("[REG] Node registered: %s\n", nodeId);
    Serial.printf("[REG] Tunnel URL: %s\n", node.getTunnelUrl() ? node.getTunnelUrl() : "(none)");
  }
}

/// 터널 연결 상태 변경 콜백
static void onTunnelChange(bool connected, const char* url) {
  if (connected) {
    Serial.printf("[TUNNEL] Connected: %s\n", url ? url : "(none)");
  } else {
    Serial.println("[TUNNEL] Disconnected");
  }
}

// ---- Arduino 진입점 ----
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println("\n=== OrbiSync Basic Connect Example ===");
  Serial.printf("Hub: %s\n", HUB_BASE_URL);
  Serial.printf("Slot: %s\n", SLOT_ID);
  Serial.printf("Firmware: %s\n", FIRMWARE_VERSION);
  
  // LED 초기화
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // OFF
  
  // 콜백 등록
  node.onStateChange(onStateChange);
  node.onError(onError);
  node.onRegistered(onRegistered);
  node.onTunnelChange(onTunnelChange);
  
  // WiFi 연결 시작
  Serial.println("\n[WiFi] Connecting...");
  node.beginWiFi(WIFI_SSID, WIFI_PASS);
  
  Serial.println("[SETUP] Complete\n");
}

void loop() {
  // 메인 루프: 상태머신 실행
  node.loopTick();
  
  // 상태 주기적 출력 (5초마다)
  static uint32_t lastStatusMs = 0;
  uint32_t now = millis();
  if (now - lastStatusMs >= 5000) {
    lastStatusMs = now;
    
    Serial.printf("[STATUS] State=%d", static_cast<int>(node.getState()));
    Serial.printf(" | NodeID=%s", node.getNodeId() ? node.getNodeId() : "(none)");
    Serial.printf(" | TunnelID=%s", node.getTunnelId() ? node.getTunnelId() : "(none)");
    Serial.printf(" | Heap=%u\n", ESP.getFreeHeap());
  }
  
  // WDT 방지
  yield();
  delay(10);
}
