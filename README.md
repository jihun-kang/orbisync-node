# OrbiSyncNode

**OrbiSyncNode**는 ESP8266 / ESP32 기반 디바이스를 위한 **Hub 중심 · 세션 기반 IoT 노드 라이브러리**입니다.

이 라이브러리는 디바이스에 **영구 자격 증명을 저장하지 않는 것**을 핵심 원칙으로 설계되었으며,  
모든 인증과 제어는 OrbiSync Hub를 통해 이루어집니다.

---

## ✨ Key Concepts

- 🔒 **RAM-only Session**
  - Flash / EEPROM에 토큰이나 키를 저장하지 않음
  - 재부팅 시 모든 인증 상태는 초기화

- 🔁 **Polling-based Authorization**
  - Node는 주기적으로 Hub에 상태를 질의
  - 승인 여부는 Hub + Web UI에서 제어

- 🧠 **Explicit State Machine**
  - BOOT → HELLO → PENDING_POLL → ACTIVE
  - 상태가 명확하고 디버깅이 쉬움

- 🌐 **HTTP / HTTPS + WebSocket Tunnel**
  - 기본은 HTTP(S)
  - 필요 시 Hub를 통한 WebSocket 터널링 지원

---

## 🧩 Architecture Overview

OrbiSync는 **디바이스 신뢰를 최소화하고**,  
**사람(Web UI)과 Hub가 승인 책임을 갖는 구조**를 채택합니다.

```
시간 →
Arduino(Node)          Hub                     Web(UI)
     |                   |                        |
     |--- HELLO -------->|                        |
     |<-- PENDING -------|                        |
     |--- POLL_SESSION ->|                        |
     |<-- PENDING -------|                        |
     |                   |<--- GET pending list --|
     |                   |---- pending list ----->|
     |                   |<--- APPROVE(slot_id) --|
     |                   | (Hub DB 상태 갱신)       |
     |--- POLL_SESSION ->|                        |
     |<-- GRANTED -------|                        |
     |==== ACTIVE MODE ===========================|
     |--- REQUEST(token)->|                       |
     |<-- RESPONSE -------|                       |
     |                    |                       |
     | (TTL 만료/재부팅)     |                       |
     |--- HELLO/POLL ---->|                       |
```

### ACTIVE MODE
ACTIVE 상태에서는 **모든 요청에 session token이 포함**됩니다.  
토큰은 RAM에만 존재하며, TTL 만료 또는 재부팅 시 자동으로 초기 상태로 복귀합니다.

---

##  Requirements

- ESP8266 또는 ESP32
- ArduinoJson **>= 7.4.0**
- WebSockets **>= 2.7.2** (by Markus Sattler)

---

##  Installation

### Arduino Library Manager
Arduino IDE → Library Manager → `OrbiSyncNode` 검색 후 설치

### Manual Installation
1. 이 저장소를 다운로드 또는 clone
2. `OrbiSyncNode` 폴더를 Arduino `libraries` 디렉토리에 복사
3. Arduino IDE 재시작

---

##  Quick Start (Minimal Example)

> 이 예제는 **가장 최소한의 설정**만 보여줍니다.  
> 고급 옵션은 `examples/reference/example`을 참고하세요.

```cpp
#include <OrbiSyncNode.h>

const char* WIFI_SSID = "your_ssid";
const char* WIFI_PASS = "your_password";
const char* HUB_BASE_URL = "https://hub.orbisync.io";
const char* SLOT_ID = "your_slot_id";

const char* capabilities[] = {"heartbeat", "commands"};

OrbiSyncNode::Config config = {
  HUB_BASE_URL,
  SLOT_ID,
  "1.1.2",
  capabilities,
  2,
  5000,
  LED_BUILTIN
};

OrbiSyncNode node(config);

void setup() {
  Serial.begin(115200);
  node.beginWiFi(WIFI_SSID, WIFI_PASS);
}

void loop() {
  node.loopTick();
  delay(10);
}
```

---

##  Examples

- **`basic_smoke_test`**
  - WiFi 연결 + 기본 동작 확인용 최소 예제

- **`reference/example`**
  - 권장 예제
  - LED 상태 표시, 터널링, throttling, 상세 로그 포함

---

##  Hub API Interaction

- `POST /api/device/hello`
- `POST /api/device/session`
- `POST /api/device/heartbeat`
- `POST /api/nodes/register_by_slot`
- WebSocket Tunnel: `wss://hub.orbisync.io/tunnel/{node_id}`

---

##  Notes & Design Philosophy

- 디바이스는 **절대 신뢰 대상이 아님**
- 장기 토큰 / API Key를 펌웨어에 넣지 않음
- 승인 흐름은 **사람(Web UI) + Hub가 통제**
- IoT 디바이스를 “계정”이 아닌 **세션 참여자**로 취급

---

##  Supported Boards

- ESP8266 (NodeMCU 등)
- ESP32

---

##  License

MIT License
