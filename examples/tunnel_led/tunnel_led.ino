/**
 * @file   tunnel_led.ino
 * @brief  OrbiSync ESP32 Node 터널 HTTP 요청 처리 예제
 * @details
 * - Hub → Node HTTP 요청 수신 (HTTP_REQ)
 * - /led/on, /led/off 경로로 LED 제어
 * - stream_id 매칭하여 HTTP_RES 응답 전송
 * 
 * 설정 필요:
 * - WIFI_SSID, WIFI_PASS: WiFi 자격증명
 * - HUB_BASE_URL: Hub 서버 주소
 * - SLOT_ID: 슬롯 ID
 * - LOGIN_TOKEN: 슬롯 로그인 토큰
 * 
 * 테스트:
 * - 외부에서 https://{tunnel_id}.orbisync.io/led/on 호출 시 LED ON
 * - https://{tunnel_id}.orbisync.io/led/off 호출 시 LED OFF
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
  cfg.debugHttp = true;
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

// ---- 터널 HTTP 요청 핸들러 ----
/// Hub → Node HTTP 요청 처리
static void handleTunnelHttpRequest(const OrbiSyncNode::TunnelHttpRequest& req,
                                    OrbiSyncNode::TunnelHttpResponseWriter& res) {
  if (!req.method || !req.path) {
    res.setStatus(400);
    res.end();
    return;
  }
  
  Serial.printf("[HTTP_REQ] stream_id=%s method=%s path=%s\n",
    req.streamId ? req.streamId : "(none)",
    req.method,
    req.path);
  
  // stream_id 검증 (응답 매칭 필수)
  if (!req.streamId || !req.streamId[0]) {
    Serial.println("[HTTP_REQ] ERROR: missing stream_id");
    res.setStatus(400);
    res.setHeader("Content-Type", "text/plain");
    res.write("Missing stream_id");
    res.end();
    return;
  }
  
  // GET /led/on
  if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/led/on") == 0) {
    digitalWrite(LED_PIN, LOW);  // LOW = ON (ESP32)
    Serial.println("[HTTP_REQ] LED ON");
    
    res.setStatus(200);
    res.setHeader("Content-Type", "text/plain");
    res.write("OK LED ON");
    res.end();
    return;
  }
  
  // GET /led/off
  if (strcmp(req.method, "GET") == 0 && strcmp(req.path, "/led/off") == 0) {
    digitalWrite(LED_PIN, HIGH);  // HIGH = OFF (ESP32)
    Serial.println("[HTTP_REQ] LED OFF");
    
    res.setStatus(200);
    res.setHeader("Content-Type", "text/plain");
    res.write("OK LED OFF");
    res.end();
    return;
  }
  
  // POST /led/on (body에 value 포함 가능)
  if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/led/on") == 0) {
    digitalWrite(LED_PIN, LOW);
    Serial.println("[HTTP_REQ] LED ON (POST)");
    
    res.setStatus(200);
    res.setHeader("Content-Type", "application/json");
    res.write("{\"ok\":true,\"led\":\"on\"}");
    res.end();
    return;
  }
  
  // POST /led/off
  if (strcmp(req.method, "POST") == 0 && strcmp(req.path, "/led/off") == 0) {
    digitalWrite(LED_PIN, HIGH);
    Serial.println("[HTTP_REQ] LED OFF (POST)");
    
    res.setStatus(200);
    res.setHeader("Content-Type", "application/json");
    res.write("{\"ok\":true,\"led\":\"off\"}");
    res.end();
    return;
  }
  
  // 404 Not Found
  Serial.printf("[HTTP_REQ] 404: %s %s\n", req.method, req.path);
  res.setStatus(404);
  res.setHeader("Content-Type", "text/plain");
  res.write("Not Found");
  res.end();
}

// ---- 콜백 함수 ----
static void onStateChange(OrbiSyncNode::State oldState, OrbiSyncNode::State newState) {
  Serial.printf("[STATE] %d -> %d\n", static_cast<int>(oldState), static_cast<int>(newState));
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
  }
}

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
  
  Serial.println("\n=== OrbiSync Tunnel LED Example ===");
  Serial.printf("Hub: %s\n", HUB_BASE_URL);
  Serial.printf("Slot: %s\n", SLOT_ID);
  
  // LED 초기화
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // OFF
  
  // 콜백 등록
  node.onStateChange(onStateChange);
  node.onError(onError);
  node.onRegistered(onRegistered);
  node.onTunnelChange(onTunnelChange);
  
  // 터널 HTTP 요청 핸들러 등록 (핵심!)
  node.setHttpRequestHandler(handleTunnelHttpRequest);
  
  // WiFi 연결 시작
  Serial.println("\n[WiFi] Connecting...");
  node.beginWiFi(WIFI_SSID, WIFI_PASS);
  
  Serial.println("[SETUP] Complete\n");
}

void loop() {
  // 메인 루프: 상태머신 실행
  node.loopTick();
  
  // WDT 방지
  yield();
  delay(10);
}
