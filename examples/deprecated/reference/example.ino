/**
 * @file    example.ino
 * @author  jihun kang
 * @date    2026-01-29
 * @brief   OrbiSyncNode 라이브러리의 참조 예제 (NodeMCU ESP8266)
 *
 * @details
 * 이 파일은 OrbiSyncNode 라이브러리의 완전한 사용 예제를 제공합니다.
 * WiFi 연결, Hub 통신, 상태 머신 관리, LED 패턴 표시, 상세 로깅 등의 기능을 포함합니다.
 *
 * - WiFi 연결 및 Hub 초기화
 * - 상태 머신 기반 인증/세션 관리
 * - 상태별 LED 패턴 표시 (BOOT/HELLO/PENDING/ACTIVE/ERROR)
 * - Serial 로그를 통한 상세 상태 모니터링
 * - WebSocket 터널 연결 및 노드 등록 상태 확인
 *
 * 보드: NodeMCU ESP8266
 * 내장 LED: D4(GPIO2), Active-Low (LOW=켜짐)
 *
 * 본 코드는 OrbiSync 오픈소스 프로젝트의 일부입니다.
 * 라이선스 및 사용 조건은 LICENSE 파일을 참고하세요.
 */

 #include <Arduino.h>
 #include <OrbiSyncNode.h>
 
 // ==============================
 // 사용자 설정
 // ==============================
 static const char* WIFI_SSID = "YOUR_WIFI_SSID";
 static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
 
 // 첫 테스트는 HTTP 권장(ESP8266 TLS 메모리 부담 큼)
 // 예: "http://192.168.0.10:8081"
 // HTTPS를 쓰면 allowInsecureTls/rootCaPem 등을 Config에서 맞춰야 함
static const char* HUB_BASE_URL = "http://192.168.0.10:8081";
 
 static const char* SLOT_ID = "SLOT-ESP8266-001";
 static const char* FW_VERSION = "0.1.0";
 
 // capabilities는 “짧게” 시작 (ESP8266 JSON 크기/메모리 고려)
static const char* CAPS[] = {
   "device.info",
   "led.set",
   "ping"
 };
 static const uint8_t CAPS_COUNT = sizeof(CAPS) / sizeof(CAPS[0]);
 
static const char* LOGIN_TOKEN = "SLOT_LOGIN_TOKEN";
static const char* MACHINE_ID = "esp8266-node-001";
static const char* NODE_NAME = "NodeMCU-Orbi";
static const char* PAIRING_CODE = nullptr;
static const char* INTERNAL_KEY = nullptr;
// 외부PC는 Hub 터널을 통해 /api/ping 또는 /api/status 경로로 Arduino에 접근할 수 있음
 // ==============================
 // NodeMCU 내장 LED 설정
 // ==============================
 
 // NodeMCU(ESP8266) 내장 LED는 보통 D4(GPIO2)
 #ifndef LED_BUILTIN
 #define LED_BUILTIN 2
 #endif
 
 // 보통 Active-Low: LOW가 켜짐
 static const bool LED_ACTIVE_LOW = true;
 
 // ==============================
 // 유틸: 상태 표시 / LED 제어
 // ==============================
 static void setLed(bool on) {
   if (LED_ACTIVE_LOW) {
     digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
   } else {
     digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
   }
 }
 
 // 상태에 따른 LED 패턴(선택)
 // - BOOT/HELLO/PENDING: 느리게 점멸
 // - ACTIVE: 짧게 2번 점멸 후 대기
 static void blinkPatternForState(OrbiSyncNode::State s) {
   static uint32_t lastBlinkMs = 0;
   static uint8_t phase = 0;
   uint32_t now = millis();
 
   // 너무 자주 토글하지 않도록
   if (now - lastBlinkMs < 150) return;
 
   switch (s) {
     case OrbiSyncNode::State::BOOT:
     case OrbiSyncNode::State::HELLO:
     case OrbiSyncNode::State::PENDING_POLL: {
       // 1초 간격 토글
       if (now - lastBlinkMs >= 1000) {
         static bool on = false;
         on = !on;
         setLed(on);
         lastBlinkMs = now;
       }
       break;
     }
     case OrbiSyncNode::State::ACTIVE: {
       // ACTIVE는 2번 빠르게 점멸(150ms), 그리고 2초 대기
       // phase: 0 ON, 1 OFF, 2 ON, 3 OFF, 4 대기
       if (phase == 0) { setLed(true);  phase = 1; lastBlinkMs = now; }
       else if (phase == 1) { setLed(false); phase = 2; lastBlinkMs = now; }
       else if (phase == 2) { setLed(true);  phase = 3; lastBlinkMs = now; }
       else if (phase == 3) { setLed(false); phase = 4; lastBlinkMs = now; }
       else if (phase == 4) {
         // 2초 대기 후 다시
         if (now - lastBlinkMs >= 2000) {
           phase = 0;
           lastBlinkMs = now;
         }
       }
       break;
     }
     case OrbiSyncNode::State::ERROR: {
       // ERROR는 빠르게 깜빡 (200ms)
       if (now - lastBlinkMs >= 200) {
         static bool on = false;
         on = !on;
         setLed(on);
         lastBlinkMs = now;
       }
       break;
     }
   }
 }
 
 // ==============================
 // OrbiSyncNode 설정
 // ==============================
 //
 // ⚠️ 여기 Config 필드는 네가 리팩토링 프롬프트대로 확장될 수 있음.
 // 예) allowInsecureTls, blinkOnHeartbeat 등.
 // 아래는 현재 네 헤더 기준 필드만 넣었고,
 // 추가 필드가 생기면 그에 맞게 채워줘.
 //
OrbiSyncNode::Config g_cfg = {
  HUB_BASE_URL,
  SLOT_ID,
  FW_VERSION,
  CAPS,
  CAPS_COUNT,
  10 * 1000,       // heartbeatIntervalMs: 10초 (테스트용)
  LED_BUILTIN,     // ledPin
  false,           // blinkOnHeartbeat
  true,            // allowInsecureTls (테스트용)
  nullptr,         // rootCaPem
  true,            // enableCommandPolling
  10000,           // commandPollIntervalMs
  LOGIN_TOKEN,
  MACHINE_ID,
  NODE_NAME,
  PAIRING_CODE,
  INTERNAL_KEY,
  true,            // enableNodeRegistration
  4000,            // registerRetryMs
  true,            // preferRegisterBySlot
  true             // enableTunnel
};
 
 OrbiSyncNode g_client(g_cfg);
 
 // ==============================
 // Serial 상태 출력(너무 자주 출력하면 느려짐)
 // ==============================
 static const char* stateToStr(OrbiSyncNode::State s) {
   switch (s) {
     case OrbiSyncNode::State::BOOT: return "BOOT";
     case OrbiSyncNode::State::HELLO: return "HELLO";
     case OrbiSyncNode::State::PENDING_POLL: return "PENDING_POLL";
     case OrbiSyncNode::State::ACTIVE: return "ACTIVE";
     case OrbiSyncNode::State::ERROR: return "ERROR";
   }
   return "UNKNOWN";
 }
 
 static void printStatusThrottled() {
   static uint32_t lastPrintMs = 0;
   uint32_t now = millis();
   if (now - lastPrintMs < 1000) return; // 1초마다 출력
   lastPrintMs = now;
 
   auto st = g_client.getState();
   Serial.print("[STATE] ");
   Serial.print(stateToStr(st));
 
   Serial.print(" | WiFi=");
   Serial.print((WiFi.status() == WL_CONNECTED) ? "OK" : "DOWN");
 
   if (WiFi.status() == WL_CONNECTED) {
     Serial.print(" | IP=");
     Serial.print(WiFi.localIP());
     Serial.print(" | RSSI=");
     Serial.print(WiFi.RSSI());
   }
 
   const char* err = g_client.getLastError();
   if (err && err[0]) {
     Serial.print(" | ERR=");
     Serial.print(err);
   }

  Serial.print(" | WS=");
  Serial.print(g_client.isTunnelConnected() ? "CONNECTED" : "DISCONNECTED");

  if (g_client.isRegistered()) {
    Serial.print(" | NODE=");
    Serial.print(g_client.getNodeId());
    Serial.print(" | TUNNEL=");
    Serial.print(g_client.getTunnelUrl());
  }
 
   Serial.println();
 }
 
 // ==============================
 // setup / loop
 // ==============================
 void setup() {
   // 내장 LED 초기화
   pinMode(LED_BUILTIN, OUTPUT);
   setLed(false);
 
   Serial.begin(115200);
   delay(200);
 
   Serial.println();
   Serial.println("=== OrbiSync NodeMCU(ESP8266) Example ===");
   Serial.print("FW: "); Serial.println(FW_VERSION);
   Serial.print("Slot: "); Serial.println(SLOT_ID);
   Serial.print("Hub: "); Serial.println(HUB_BASE_URL);
 
   // Wi-Fi 시작
   bool wifiOk = g_client.beginWiFi(WIFI_SSID, WIFI_PASS);
   Serial.print("beginWiFi() returned: ");
   Serial.println(wifiOk ? "true" : "false");
 
   // 첫 인디케이터
   blinkPatternForState(g_client.getState());
 }
 
 void loop() {
   // 핵심: 상태머신 진행
   g_client.loopTick();
 
   // 상태 로그 출력
   printStatusThrottled();
 
   // LED 패턴(선택)
   blinkPatternForState(g_client.getState());
 
   // loop 과부하 방지
   delay(10);
 }
 