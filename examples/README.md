# OrbiSyncNode Examples

OrbiSync ESP32/ESP8266 Node 라이브러리 사용 예제입니다.

## 예제 목록

### 1. basic_connect
**기본 연결 예제** - WiFi 연결 및 Hub 접속, register 성공까지의 최소 예제

**사용법:**
- `WIFI_SSID`, `WIFI_PASS` 설정
- `HUB_BASE_URL`, `SLOT_ID`, `LOGIN_TOKEN` 설정
- 컴파일 후 업로드

**기능:**
- WiFi 연결
- Hub 세션/터널 연결
- register 성공 로그
- 상태 변경 콜백

### 2. tunnel_led
**터널 HTTP 요청 처리 예제** - Hub → Node HTTP 요청 수신 및 응답

**사용법:**
- `basic_connect`와 동일한 설정
- 컴파일 후 업로드
- 외부에서 `https://{tunnel_id}.orbisync.io/led/on` 호출 시 LED ON
- `https://{tunnel_id}.orbisync.io/led/off` 호출 시 LED OFF

**기능:**
- HTTP_REQ 수신 처리
- `/led/on`, `/led/off` 경로로 LED 제어
- stream_id 매칭하여 HTTP_RES 응답 전송

### 3. diagnostics
**진단/디버깅 예제** - 상세 상태 로깅 및 모니터링

**사용법:**
- `basic_connect`와 동일한 설정
- 컴파일 후 업로드
- Serial 모니터에서 상세 로그 확인

**기능:**
- 상태 변경 상세 로깅
- Heap 메모리 모니터링
- 터널 연결 상태 추적
- 30초마다 진단 정보 출력

## 공통 설정

모든 예제에서 다음 설정이 필요합니다:

```cpp
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
const char* HUB_BASE_URL = "https://hub.orbisync.io";
const char* SLOT_ID = "YOUR_SLOT_ID";
const char* LOGIN_TOKEN = "YOUR_LOGIN_TOKEN";
```

## 핵심 API 사용법

### 1. Config 생성
```cpp
OrbiSyncNode::Config cfg{};
cfg.hubBaseUrl = "https://hub.orbisync.io";
cfg.slotId = "YOUR_SLOT_ID";
cfg.firmwareVersion = "1.0.0";
cfg.enableTunnel = true;
cfg.enableNodeRegistration = true;
cfg.loginToken = "YOUR_LOGIN_TOKEN";
// ... 기타 설정
```

### 2. Node 인스턴스 생성 및 초기화
```cpp
OrbiSyncNode::OrbiSyncNode node(cfg);
node.beginWiFi(WIFI_SSID, WIFI_PASS);
```

### 3. 콜백 등록
```cpp
node.onStateChange(onStateChange);
node.onError(onError);
node.onRegistered(onRegistered);
node.onTunnelChange(onTunnelChange);
```

### 4. 터널 HTTP 요청 핸들러 등록
```cpp
node.setHttpRequestHandler(handleTunnelHttpRequest);
```

### 5. 메인 루프
```cpp
void loop() {
  node.loopTick();  // 상태머신 실행
  yield();
  delay(10);
}
```

## HTTP 요청 처리 예제

```cpp
static void handleTunnelHttpRequest(
    const OrbiSyncNode::TunnelHttpRequest& req,
    OrbiSyncNode::TunnelHttpResponseWriter& res) {
  
  // stream_id 검증 (필수!)
  if (!req.streamId || !req.streamId[0]) {
    res.setStatus(400);
    res.end();
    return;
  }
  
  // 경로별 처리
  if (strcmp(req.path, "/led/on") == 0) {
    digitalWrite(LED_PIN, LOW);
    res.setStatus(200);
    res.setHeader("Content-Type", "text/plain");
    res.write("OK LED ON");
    res.end();  // 필수!
    return;
  }
  
  // 404
  res.setStatus(404);
  res.end();
}
```

## 주의사항

1. **stream_id 필수**: HTTP_REQ의 stream_id를 HTTP_RES에 반드시 포함해야 합니다.
2. **res.end() 호출**: 응답 작성 후 반드시 `res.end()`를 호출해야 합니다.
3. **WDT 방지**: `loop()`에서 `yield()`와 `delay(10)` 사용을 권장합니다.
4. **메모리 관리**: ESP8266에서는 큰 버퍼 사용 시 주의가 필요합니다.

## 문제 해결

- **컴파일 에러**: 최신 `OrbiSyncNode.h`가 포함되어 있는지 확인
- **연결 실패**: `HUB_BASE_URL`, `SLOT_ID`, `LOGIN_TOKEN` 확인
- **HTTP 요청 미수신**: `setHttpRequestHandler()` 등록 확인
- **응답 타임아웃**: `stream_id`가 응답에 포함되었는지 확인
