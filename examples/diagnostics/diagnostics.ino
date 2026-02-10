/**
 * @file   diagnostics.ino
 * @brief  OrbiSync ESP32 Node 진단/디버깅 예제
 * @details
 * - 상세 상태 로깅 (heap, ping, 재연결 백오프 등)
 * - 터널 연결 상태 모니터링
 * - 운영 환경 디버깅용
 * 
 * 설정 필요:
 * - WIFI_SSID, WIFI_PASS: WiFi 자격증명
 * - HUB_BASE_URL: Hub 서버 주소
 * - SLOT_ID: 슬롯 ID
 * - LOGIN_TOKEN: 슬롯 로그인 토큰
 */

#if defined(ESP8266)
  #include <ESP8266WiFi.h>
#elif defined(ESP32)
  #include <WiFi.h>
#else
  #error "Unsupported board"
#endif

#include <OrbiSyncNode.h>

// ---- 설정 ----
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* HUB_BASE_URL = "https://hub.orbisync.io";
const char* SLOT_ID = "YOUR_SLOT_ID";
const char* FIRMWARE_VERSION = "1.0.0";
const char* LOGIN_TOKEN = "YOUR_LOGIN_TOKEN";

// Capabilities
const char* capabilities[] = {"heartbeat", "commands", "api"};
const uint8_t capabilityCount = sizeof(capabilities) / sizeof(capabilities[0]);

// LED 핀
static const uint8_t LED_PIN = 2;

// ---- OrbiSyncNode 설정 ----
static OrbiSyncNode::Config makeConfig() {
  OrbiSyncNode::Config cfg{};
  
  cfg.hubBaseUrl = HUB_BASE_URL;
  cfg.slotId = SLOT_ID;
  cfg.firmwareVersion = FIRMWARE_VERSION;
  cfg.capabilities = capabilities;
  cfg.capabilityCount = capabilityCount;
  
  cfg.heartbeatIntervalMs = 5000;
  cfg.ledPin = LED_PIN;
  cfg.blinkOnHeartbeat = false;
  
  cfg.allowInsecureTls = true;
  cfg.rootCaPem = nullptr;
  
  cfg.enableCommandPolling = true;
  cfg.commandPollIntervalMs = 10000;
  
  cfg.machineIdPrefix = "node-";
  cfg.nodeNamePrefix = "Node-";
  cfg.appendUniqueSuffix = true;
  cfg.useMacForUniqueId = true;
  
  cfg.enableTunnel = true;
  cfg.enableNodeRegistration = true;
  cfg.registerRetryMs = 4000;
  cfg.preferRegisterBySlot = true;
  
  cfg.enableSelfApprove = true;
  cfg.approveEndpointPath = "/api/device/approve";
  cfg.selfApproveKey = "YOUR_KEY";
  cfg.approveRetryMs = 3000;
  
  cfg.sessionEndpointPath = "/api/device/session";
  cfg.debugHttp = true;  // 상세 HTTP 로그
  cfg.maskMacInLogs = false;
  
  cfg.loginToken = LOGIN_TOKEN;
  cfg.pairingCode = nullptr;
  cfg.internalKey = nullptr;
  
  cfg.maxTunnelBodyBytes = 4096;
  cfg.tunnelReconnectMs = 5000;
  
  return cfg;
}

static const OrbiSyncNode::Config nodeConfig = makeConfig();
static OrbiSyncNode::OrbiSyncNode node(nodeConfig);

// ---- 상태 문자열 변환 ----
static const char* stateToString(OrbiSyncNode::State s) {
  switch (s) {
    case OrbiSyncNode::State::BOOT: return "BOOT";
    case OrbiSyncNode::State::HELLO: return "HELLO";
    case OrbiSyncNode::State::PAIR_SUBMIT: return "PAIR_SUBMIT";
    case OrbiSyncNode::State::PENDING_POLL: return "PENDING_POLL";
    case OrbiSyncNode::State::GRANTED: return "GRANTED";
    case OrbiSyncNode::State::ACTIVE: return "ACTIVE";
    case OrbiSyncNode::State::TUNNEL_CONNECTING: return "TUNNEL_CONNECTING";
    case OrbiSyncNode::State::TUNNEL_CONNECTED: return "TUNNEL_CONNECTED";
    case OrbiSyncNode::State::ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

// ---- 진단 정보 출력 ----
static void printDiagnostics() {
  Serial.println("========================================");
  Serial.println("[DIAGNOSTICS]");
  Serial.println("========================================");
  
  // 상태
  Serial.printf("State: %s\n", stateToString(node.getState()));
  
  // WiFi
  Serial.printf("WiFi: %s", (WiFi.status() == WL_CONNECTED) ? "CONNECTED" : "DISCONNECTED");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(" | IP: %s | RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println();
  }
  
  // Node 정보
  Serial.printf("NodeID: %s\n", node.getNodeId() ? node.getNodeId() : "(none)");
  Serial.printf("TunnelID: %s\n", node.getTunnelId() ? node.getTunnelId() : "(none)");
  Serial.printf("TunnelURL: %s\n", node.getTunnelUrl() ? node.getTunnelUrl() : "(none)");
  
  // 토큰 (일부만 표시)
  const char* sessionToken = node.getSessionToken();
  if (sessionToken && sessionToken[0]) {
    Serial.printf("SessionToken: %.20s...\n", sessionToken);
  } else {
    Serial.println("SessionToken: (none)");
  }
  
  // 메모리
  Serial.printf("Free Heap: %u bytes\n", ESP.getFreeHeap());
  
#if defined(ESP8266)
  Serial.printf("Heap Fragmentation: %u%%\n", ESP.getHeapFragmentation());
#elif defined(ESP32)
  Serial.printf("Largest Free Block: %u bytes\n", ESP.getMaxAllocHeap());
#endif
  
  // Uptime
  Serial.printf("Uptime: %lu ms\n", millis());
  
  Serial.println("========================================");
}

// ---- 콜백 함수 ----
static void onStateChange(OrbiSyncNode::State oldState, OrbiSyncNode::State newState) {
  Serial.printf("[STATE] %s -> %s\n",
    stateToString(oldState),
    stateToString(newState));
  
  // ACTIVE 진입 시 진단 정보 출력
  if (newState == OrbiSyncNode::State::ACTIVE) {
    Serial.println("[INFO] ACTIVE state entered");
    printDiagnostics();
  }
  
  // TUNNEL_CONNECTED 진입 시
  if (newState == OrbiSyncNode::State::TUNNEL_CONNECTED) {
    Serial.println("[INFO] Tunnel connected");
    printDiagnostics();
  }
}

static void onError(const char* error) {
  if (error && error[0]) {
    Serial.printf("[ERROR] %s\n", error);
  }
}

static void onRegistered(const char* nodeId) {
  if (nodeId && nodeId[0]) {
    Serial.printf("[REG] Node registered: %s\n", nodeId);
    Serial.printf("[REG] Tunnel URL: %s\n", node.getTunnelUrl() ? node.getTunnelUrl() : "(none)");
    printDiagnostics();
  }
}

static void onTunnelChange(bool connected, const char* url) {
  if (connected) {
    Serial.printf("[TUNNEL] Connected: %s\n", url ? url : "(none)");
  } else {
    Serial.println("[TUNNEL] Disconnected");
    Serial.println("[TUNNEL] Will reconnect with backoff...");
  }
}

// ---- 터널 메시지 콜백 (디버깅용) ----
static void onTunnelMessage(const char* json) {
  if (json && json[0]) {
    Serial.printf("[TUNNEL_MSG] %s\n", json);
  }
}

// ---- Arduino 진입점 ----
void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println("\n=== OrbiSync Diagnostics Example ===");
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
  node.onTunnelMessage(onTunnelMessage);
  
  // WiFi 연결 시작
  Serial.println("\n[WiFi] Connecting...");
  node.beginWiFi(WIFI_SSID, WIFI_PASS);
  
  Serial.println("[SETUP] Complete\n");
  
  // 초기 진단 정보
  printDiagnostics();
}

void loop() {
  // 메인 루프: 상태머신 실행
  node.loopTick();
  
  // 주기적 진단 정보 출력 (30초마다)
  static uint32_t lastDiagMs = 0;
  uint32_t now = millis();
  if (now - lastDiagMs >= 30000) {
    lastDiagMs = now;
    printDiagnostics();
  }
  
  // WDT 방지
  yield();
  delay(10);
}
