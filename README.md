# OrbiSyncNode

ESP8266/ESP32용 OrbiSync Node 라이브러리입니다.

## Features
- RAM-only 세션 (Flash/EEPROM 저장 없음)
- State Machine (BOOT → HELLO → PENDING_POLL → ACTIVE)
- HTTP/HTTPS + WebSocket 터널링 지원
- Heartbeat / Command Pull
- Node 등록 및 터널 연결

## Requirements
- ESP8266 또는 ESP32
- ArduinoJson >= 7.4.0
- WebSockets >= 2.7.2 (by Markus Sattler)

## Installation

### Arduino Library Manager
Arduino IDE에서 `OrbiSyncNode` 검색하여 설치

### Manual Installation
1. Download or clone this repository
2. Place `OrbiSyncNode` folder in your Arduino `libraries` directory
3. Restart Arduino IDE

## Usage

```cpp
#include <OrbiSyncNode.h>

const char* WIFI_SSID = "your_ssid";
const char* WIFI_PASS = "your_password";
const char* HUB_BASE_URL = "https://hub.orbisync.io";
const char* SLOT_ID = "your_slot_id";
const char* LOGIN_TOKEN = "your_login_token";

const char* capabilities[] = {"heartbeat", "commands"};

const OrbiSyncNode::Config config = {
  HUB_BASE_URL,
  SLOT_ID,
  "1.0.0",
  capabilities,
  2,
  5000,
  LED_BUILTIN,
  false,
  true,
  nullptr,
  true,
  10000,
  LOGIN_TOKEN,
  "machine-id",
  "NodeName",
  nullptr,
  nullptr,
  true,
  4000,
  true,
  true
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

## Examples

- **`basic_smoke_test`**: 최소 예제 - 빠른 테스트용 (WiFi 연결, 기본 동작 확인)
- **`reference/example`**: 권장 예제 - 상세 기능 포함 (LED 패턴, 상태 출력, throttling 등)

빠른 테스트는 `basic_smoke_test`, 기능 학습 및 확장은 `reference/example`을 참고하세요.

## API Endpoints

The library communicates with OrbiSync Hub using the following endpoints:
- `POST /api/device/hello` - Device registration request
- `POST /api/device/session` - Session polling
- `POST /api/device/heartbeat` - Heartbeat (when ACTIVE)
- `POST /api/nodes/register_by_slot` - Node registration
- WebSocket tunnel: `wss://hub.orbisync.io/tunnel/{node_id}`

## Notes
- This library requires an OrbiSync Hub server
- Session tokens are stored only in RAM (no flash/EEPROM)
- All authentication state is lost on reboot
- Supports both ESP8266 (NodeMCU) and ESP32
