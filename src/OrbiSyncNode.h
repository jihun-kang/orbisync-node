/**
 * @file   OrbiSyncNode.h
 * @brief  ESP32/ESP8266용 OrbiSync Hub 터널 클라이언트 라이브러리
 * @details
 * - Hub와 WebSocket 터널 연결 및 HTTP 요청 처리
 * - 상태머신 기반 등록/승인/세션 관리
 * - 메모리 안정화: static 클라이언트 재사용, String 최소화
 */
 #ifndef ORBISYNC_NODE_H
 #define ORBISYNC_NODE_H
 
 #include <stdint.h>
 #include <stddef.h>
 
 #define ORBISYNC_HAS_TUNNEL_CONFIG 1
 #define ORBISYNC_HAS_TUNNEL_STATES 1
 
namespace OrbiSyncNode {

/// 노드 상태머신
enum class State {
  BOOT,              /// 초기화 중
  HELLO,             /// Hub에 hello 전송
  PAIR_SUBMIT,       /// 페어링 코드 제출
  PENDING_POLL,      /// 세션 폴링 대기
  GRANTED,           /// 세션 승인됨
  ACTIVE,            /// 세션 활성화됨
  TUNNEL_CONNECTING, /// 터널 연결 시도 중
  TUNNEL_CONNECTED,  /// 터널 연결됨
  ERROR,             /// 에러 상태
};

enum class Protocol { HTTP, WS };
 
#define TUNNEL_MAX_HEADERS 8
/// Hub → Node HTTP 요청 구조체
struct TunnelHttpRequest {
  const char* requestId;
  const char* streamId;  /// 응답 매칭용 ID
  const char* tunnelId;
  const char* method;
  const char* path;
  const char* query;
  const uint8_t* body;
  size_t bodyLen;

  const char* getHeader(const char* key) const;

  uint8_t headerCount;
  struct { char key[24]; char value[80]; } headers[TUNNEL_MAX_HEADERS];
};

class OrbiSyncNode;

/// Node → Hub HTTP 응답 작성기 (2KB body 버퍼)
class TunnelHttpResponseWriter {
  public:
   void setStatus(int code);
   void setHeader(const char* key, const char* value);
   void write(const uint8_t* data, size_t len);
   void write(const char* str);
   void end();
 
  private:
   friend class OrbiSyncNode;
   TunnelHttpResponseWriter();
   void* node_;
   char requestId_[48];
   int statusCode_;
   uint8_t headerCount_;
   struct { char key[24]; char value[80]; } headers_[TUNNEL_MAX_HEADERS];
   uint8_t body_[2048];
   size_t bodyLen_;
   bool ended_;
 };
 
 typedef void (*HttpRequestCallback)(const TunnelHttpRequest& req, TunnelHttpResponseWriter& res);
 
 struct Config {
   const char* hubBaseUrl;          // "https://hub.orbisync.io" 권장
   const char* slotId;
   const char* firmwareVersion;
 
   const char* const* capabilities;
   uint8_t capabilityCount;
 
   uint32_t heartbeatIntervalMs;
   int ledPin;
   bool blinkOnHeartbeat;
 
   bool allowInsecureTls;           // true면 setInsecure(개발용)
   const char* rootCaPem;           // (선택) CA PEM
 
   bool enableCommandPolling;
   uint32_t commandPollIntervalMs;
 
   const char* machineIdPrefix;     // default "node-"
   const char* nodeNamePrefix;      // default "Node-"
   bool appendUniqueSuffix;
   bool useMacForUniqueId;
 
   bool enableTunnel;               // true면 WS 터널 사용
   bool enableNodeRegistration;
 
   uint32_t registerRetryMs;
 
   bool preferRegisterBySlot;
   bool enableSelfApprove;
   const char* approveEndpointPath; // 예: "/api/device/approve"
   const char* selfApproveKey;
   uint32_t approveRetryMs;
 
   const char* sessionEndpointPath; // default "/api/device/session"
 
   bool debugHttp;
   bool maskMacInLogs;
 
   const char* loginToken;          // register_by_slot 사용 시
   const char* pairingCode;         // (옵션) 고정 pairing
   const char* internalKey;
 
   size_t maxTunnelBodyBytes;       // default 4096
   uint32_t tunnelReconnectMs;      // default 5000

   /// HELLO 요청에 reconnect=true, boot_reason 등 재연결 힌트 포함 (재부팅/복귀 시 Hub가 403 등 재연결로 처리 가능)
   bool sendReconnectHintInHello;
 };
 
 struct Request {
   Protocol proto;
   const char* method;
   const char* path;
   const uint8_t* body;
   size_t body_len;
 };
 
 struct Response {
   int status;
   const char* content_type;
   const uint8_t* body;
   size_t body_len;
 };
 
 typedef void (*StateChangeCB)(State oldState, State newState);
 typedef void (*ErrorCB)(const char* error);
 typedef void (*RegisteredCB)(const char* nodeId);
 typedef void (*SessionInvalidCB)(void);  /// 401/410 등으로 세션/페어링 무효 시 (스케치에서 NVS 등 클리어)
 typedef void (*TunnelChangeCB)(bool connected, const char* url);
 typedef bool (*RequestHandler)(const Request& req, Response& resp);
 typedef void (*TunnelMessageCB)(const char* json);
 
/// ESP32 터널 연결 및 HTTP 요청 처리 담당 클래스
class OrbiSyncNode {
 public:
   explicit OrbiSyncNode(const Config& config);

   /// WiFi 연결 시작
   void beginWiFi(const char* ssid, const char* pass);
   /// 메인 루프 (상태머신 실행)
   void loopTick();

   void onStateChange(StateChangeCB cb) { stateChangeCb_ = cb; }
   void onError(ErrorCB cb) { errorCb_ = cb; }
   void onRegistered(RegisteredCB cb) { registeredCb_ = cb; }
   void onSessionInvalid(SessionInvalidCB cb) { sessionInvalidCb_ = cb; }
   void onTunnelChange(TunnelChangeCB cb) { tunnelChangeCb_ = cb; }
   void onRequest(RequestHandler h) { requestHandler_ = h; }
   void onTunnelMessage(TunnelMessageCB cb) { tunnelMessageCb_ = cb; }
   void onHttpRequest(HttpRequestCallback cb) { httpRequestCb_ = cb; }
   void setHttpRequestHandler(HttpRequestCallback cb) { httpRequestCb_ = cb; }

   State getState() const { return state_; }
   const char* getNodeId() const { return nodeId_; }
   const char* getTunnelUrl() const { return tunnelUrl_; }
   const char* getTunnelId() const { return tunnelId_; }
   const char* getSessionToken() const { return sessionToken_; }
   const char* getSessionExpiresAt() const { return sessionExpiresAt_; }
   const char* getNodeToken() const { return nodeToken_; }

   /// 세션 토큰 외부 설정 (NVS 복원 시)
   void setSessionToken(const char* token);
   /// 세션 만료 시각 (ISO 문자열) 외부 설정. NVS 복원 시 스케치에서 설정 가능.
   void setSessionExpiresAt(const char* expiresAt);

   // ---- 터널 관련 public 함수 ----
   /// 터널 연결 루프 (재연결/keepalive 처리)
   void tunnelLoop();
   /// WebSocket 연결 시작
   void tunnelConnect();
   /// WebSocket 연결 종료
   void tunnelDisconnect();
   /// Hub에 register 요청 전송
   void tunnelSendRegister();

   // ---- WebSocket 메시지 핸들러 ----
   void tunnelHandleMessage(const char* payload);                 /// 호환용
   void tunnelHandleMessage(const uint8_t* payload, size_t len);  /// 권장(복사/할당 없음)
   /// proxy_request 타입 메시지 처리
   void tunnelHandleProxyRequest(const uint8_t* payload, size_t len);

   /// WebSocket으로 텍스트 전송
   bool tunnelSendText(const char* text);
   /// HTTP 응답 전송 (stream_id 매칭)
   void tunnelSendProxyResponse(TunnelHttpResponseWriter& res);
 
  private:
   Config cfg_;
   State state_;
 
   char nodeId_[64];
   char nodeToken_[256];
   char tunnelUrl_[256];
   char tunnelId_[64];
   char sessionToken_[256];
   char sessionExpiresAt_[32];   // ISO datetime from Hub (expires_at / session_expires_at)

   static constexpr size_t kPairingCodeMax = 32;
   char pairingCode_[kPairingCodeMax];
   char pairingExpiresAt_[32];
   bool pairingCodeValid_;
 
   uint32_t nextHelloMs_;
   uint32_t nextPairMs_;
   uint32_t nextApproveMs_;
   uint32_t nextSessionPollMs_;
   uint32_t nextRegisterBySlotMs_;
 
   uint32_t netBackoffMs_;
   uint32_t pairBackoffMs_;
 
   uint32_t nextTunnelConnectMs_;
   uint32_t tunnelBackoffMs_;
   uint8_t  tunnelBackoffIndex_;
   uint32_t lastTunnelPingMs_;
   bool tunnelRegistered_;
   bool approveMissingMacFailed_;
 
   uint32_t lastHeartbeatMs_;
   bool wifiConnecting_;
   bool httpBusy_;
 
   StateChangeCB stateChangeCb_;
   ErrorCB errorCb_;
   RegisteredCB registeredCb_;
   SessionInvalidCB sessionInvalidCb_;
   TunnelChangeCB tunnelChangeCb_;
   RequestHandler requestHandler_;
   TunnelMessageCB tunnelMessageCb_;
   HttpRequestCallback httpRequestCb_;
 
   // ---- 내부 상태 관리 ----
   void setState(State s);
   void ensureWiFi();
   void runStateMachine();

   // ---- Hub API 호출 ----
   void tryHello();
   void handleHelloResponse(int status, const char* body, size_t len);
   void tryPairIfNeeded();
   bool postDevicePair(const char* code, int* outStatus, char* outBody, size_t outBodyMax);
   void tryApprove();
   void trySessionPoll();
   /// 저장된 session_token으로 /api/device/session 갱신 시도. 성공 시 ACTIVE, 실패(401/410) 시 토큰 클리어 후 false
   bool trySessionRefresh();
   void tryRegisterBySlot();
   bool postJsonUnified(const char* path, const char* body, int* outStatus,
                        char* outBody, size_t outBodyMax);

   // ---- 유틸리티 ----
   const char* getMacCStr();
   void getMachineId(char* buf, size_t size);
   void getDeviceInfoJson(char* buf, size_t size);
   uint32_t computeCapabilitiesHash() const;
   bool isPairingExpired() const;
   void clearPairingCode();
   void storePairingFromHello(const char* code, const char* expiresAt);

   // ---- 백오프 관리 ----
   void advanceNetBackoff();
   void advancePairBackoff();
   void resetNetBackoff();
   void resetPairBackoff();

   /// 터널 연결 종료 후 정리 (상태/백오프/콜백만, 포인터 삭제 없음)
   void tunnelDisconnectCleanup();

   /// Hub에 heartbeat 전송
   void tryHeartbeat();
 };
 
 }  // namespace OrbiSyncNode
 
 using OrbiSyncNodeType = OrbiSyncNode::OrbiSyncNode;
 
 #endif  // ORBISYNC_NODE_H
 