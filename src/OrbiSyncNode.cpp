/**
 * @file   OrbiSyncNode.cpp
 * @brief  OrbiSync Hub 터널 클라이언트 구현
 * @details
 * - Hub API 호출 (hello/approve/session/register)
 * - WebSocket 터널 연결 및 HTTP 요청 처리
 * - 메모리 안정화: static 클라이언트 재사용, payload 직접 파싱
 */

 #if defined(ESP8266)
 #include <ESP8266WiFi.h>
 #include <WiFiClient.h>
 #include <WiFiClientSecureBearSSL.h>
#elif defined(ESP32)
 #include <WiFi.h>
 #include <WiFiClient.h>
 #include <WiFiClientSecure.h>
 #include <esp_system.h>
#else
 #error "Unsupported board"
#endif

#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>

#include "OrbiSyncNode.h"

// -----------------------------
// Tunables
// -----------------------------
static constexpr uint32_t kBackoffMinMs = 2000;   // hello 재시도 최소 2s (폭주 방지)
static constexpr uint32_t kBackoffMaxMs = 60000; // hello 403 등 시 최대 60s

static constexpr uint32_t kTunnelPingIntervalMs = 25000;
static const uint32_t kTunnelBackoffMs[] = {2000, 4000, 8000, 15000, 60000}; // 터널 끊김 시 재연결 백오프 (폭주 방지)
static constexpr uint8_t kTunnelBackoffSteps = 5;

static constexpr size_t kDefaultMaxTunnelBody = 4096;

// HTTPS fail → HTTP fallback (개발용)
static uint8_t s_httpsFailCount = 0;
static constexpr uint8_t MAX_HTTPS_FAIL_COUNT = 2;

static char s_macBuf[24];

// WebSocket globals
static WebSocketsClient* s_wsClient = nullptr;
static OrbiSyncNode::OrbiSyncNode* s_nodeForWs = nullptr;
// Defer disconnect/delete to main loop; never delete inside WebSocket callback (prevents LoadProhibited).
static bool s_tunnelDisconnectPending = false;

// 터널 로그 rate limit (10초에 1회)
static constexpr uint32_t kTunnelStatusLogIntervalMs = 10000;
static uint32_t s_lastTunnelStatusLogMs = 0;
static uint32_t s_lastTunnelSkipLogMs = 0;

// tunnel_url에서 tunnel_id(서브도메인), tunnel_host 추출. buf_id/buf_host 최대 64바이트.
static void parseTunnelUrlParts(const char* url, char* buf_id, size_t sz_id, char* buf_host, size_t sz_host) {
  if (!url || !buf_id || sz_id == 0) return;
  buf_id[0] = '\0';
  if (buf_host && sz_host) buf_host[0] = '\0';
  const char* hostStart = (strncmp(url, "wss://", 6) == 0) ? url + 6 : (strncmp(url, "ws://", 5) == 0) ? url + 5 : nullptr;
  if (!hostStart) return;
  const char* pathStart = strchr(hostStart, '/');
  size_t hostLen = pathStart ? (size_t)(pathStart - hostStart) : strlen(hostStart);
  if (hostLen == 0 || hostLen >= 128) return;
  if (buf_host && sz_host > 0) {
    size_t cp = hostLen < sz_host - 1 ? hostLen : sz_host - 1;
    memcpy(buf_host, hostStart, cp);
    buf_host[cp] = '\0';
  }
  const char* dot = strchr(hostStart, '.');
  if (dot && dot > hostStart) {
    size_t idLen = (size_t)(dot - hostStart);
    if (idLen >= sz_id) idLen = sz_id - 1;
    memcpy(buf_id, hostStart, idLen);
    buf_id[idLen] = '\0';
  } else {
    size_t cp = hostLen < sz_id - 1 ? hostLen : sz_id - 1;
    memcpy(buf_id, hostStart, cp);
    buf_id[cp] = '\0';
  }
}

// 응답 body preview (최대 200바이트, 제어문자 치환)
static void logBodyPreview(const char* tag, const char* body, size_t len) {
  constexpr size_t kMaxPreview = 200;
  if (!body) return;
  size_t pl = len;
  if (pl > kMaxPreview) pl = kMaxPreview;
  Serial.printf("[%s] response body_len=%u preview=", tag, (unsigned)len);
  for (size_t i = 0; i < pl; i++) {
    char c = body[i];
    if (c == '\r' || c == '\n') Serial.print(' ');
    else if (c >= 32 && c < 127) Serial.print(c);
    else Serial.print('.');
  }
  if (len > kMaxPreview) Serial.print("...");
  Serial.println();
}

// -----------------------------
// Base64
// -----------------------------
static const char kBase64Chars[] =
 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64Decode(uint8_t* out, const char* in, size_t inLen) {
 size_t outLen = 0;
 int val = 0, valb = -8;
 for (size_t i = 0; i < inLen; i++) {
   unsigned char c = (unsigned char)in[i];
   int d = -1;
   if (c >= 'A' && c <= 'Z') d = c - 'A';
   else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
   else if (c >= '0' && c <= '9') d = c - '0' + 52;
   else if (c == '+') d = 62;
   else if (c == '/') d = 63;
   else if (c == '=') break;
   else continue;

   val = (val << 6) + d;
   valb += 6;
   if (valb >= 0) {
     out[outLen++] = (uint8_t)((val >> valb) & 0xFF);
     valb -= 8;
   }
 }
 return outLen;
}

static size_t base64Encode(char* out, const uint8_t* in, size_t inLen) {
 size_t i = 0, j = 0;
 for (; i + 2 < inLen; i += 3) {
   out[j++] = kBase64Chars[(in[i] >> 2) & 0x3F];
   out[j++] = kBase64Chars[((in[i] & 0x03) << 4) | (in[i+1] >> 4)];
   out[j++] = kBase64Chars[((in[i+1] & 0x0F) << 2) | (in[i+2] >> 6)];
   out[j++] = kBase64Chars[in[i+2] & 0x3F];
 }
 if (i < inLen) {
   out[j++] = kBase64Chars[(in[i] >> 2) & 0x3F];
   if (i + 1 < inLen) {
     out[j++] = kBase64Chars[((in[i] & 0x03) << 4) | (in[i+1] >> 4)];
     out[j++] = kBase64Chars[((in[i+1] & 0x0F) << 2)];
     out[j++] = '=';
   } else {
     out[j++] = kBase64Chars[(in[i] & 0x03) << 4];
     out[j++] = '=';
     out[j++] = '=';
   }
 }
 out[j] = '\0';
 return j;
}

// =============================
// Namespace OrbiSyncNode
// =============================
namespace OrbiSyncNode {

// -----------------------------
// Helpers
// -----------------------------
static const char* stateStr(State s) {
 switch (s) {
   case State::BOOT: return "BOOT";
   case State::HELLO: return "HELLO";
   case State::PAIR_SUBMIT: return "PAIR_SUBMIT";
   case State::PENDING_POLL: return "PENDING_POLL";
   case State::GRANTED: return "GRANTED";
   case State::ACTIVE: return "ACTIVE";
   case State::TUNNEL_CONNECTING: return "TUNNEL_CONNECTING";
   case State::TUNNEL_CONNECTED: return "TUNNEL_CONNECTED";
   case State::ERROR: return "ERROR";
   default: return "UNKNOWN";
 }
}

// -----------------------------
// TunnelHttpRequest::getHeader
// -----------------------------
const char* TunnelHttpRequest::getHeader(const char* key) const {
 if (!key) return nullptr;
 for (uint8_t i = 0; i < headerCount; i++) {
   size_t j = 0;
   while (key[j] && headers[i].key[j] && key[j] == headers[i].key[j]) j++;
   if (key[j] == '\0' && headers[i].key[j] == '\0') return headers[i].value;
 }
 return nullptr;
}

// -----------------------------
// TunnelHttpResponseWriter
// -----------------------------
TunnelHttpResponseWriter::TunnelHttpResponseWriter()
 : node_(nullptr), statusCode_(200), headerCount_(0), bodyLen_(0), ended_(false) {
 requestId_[0] = '\0';
}

void TunnelHttpResponseWriter::setStatus(int code) { statusCode_ = code; }

void TunnelHttpResponseWriter::setHeader(const char* key, const char* value) {
 if (!key || !value || headerCount_ >= TUNNEL_MAX_HEADERS) return;
 strncpy(headers_[headerCount_].key, key, 23);
 headers_[headerCount_].key[23] = '\0';
 strncpy(headers_[headerCount_].value, value, 79);
 headers_[headerCount_].value[79] = '\0';
 headerCount_++;
}

void TunnelHttpResponseWriter::write(const uint8_t* data, size_t len) {
 if (!data || ended_) return;
 size_t remain = sizeof(body_) - bodyLen_;
 if (len > remain) len = remain;
 memcpy(body_ + bodyLen_, data, len);
 bodyLen_ += len;
}

void TunnelHttpResponseWriter::write(const char* str) {
 if (!str) return;
 write((const uint8_t*)str, strlen(str));
}

void TunnelHttpResponseWriter::end() {
 if (ended_) return;
 ended_ = true;
 if (node_) {
   static_cast<OrbiSyncNode*>(node_)->tunnelSendProxyResponse(*this);
 }
}

// -----------------------------
// Config defaults helper
// -----------------------------
static uint32_t cfgOrDefaultU32(uint32_t v, uint32_t defv) { return v ? v : defv; }
static size_t cfgOrDefaultSz(size_t v, size_t defv) { return v ? v : defv; }

// -----------------------------
// safePostJson (NO new/delete)
// -----------------------------
// - static TLS/plain client reuse
// - header read: connected() 조건 제거 + first-byte wait
// - timeout을 넉넉히(HELLO 단계에서 header read 늦게 오는 경우 방지)
static bool safePostJson(
 const Config& cfg,
 const char* host,
 uint16_t port,
 bool useTls,
 const char* path,
 const char* jsonBody,
 int* outStatus,
 char* outBody,
 size_t outBodyMax,
 const char* logPrefix
) {
 if (!host || !path || !outStatus || !outBody || outBodyMax == 0) return false;
 outBody[0] = '\0';
 *outStatus = 0;

 yield();

 const uint32_t CONNECT_TIMEOUT_MS = 12000;
 const uint32_t FIRST_BYTE_WAIT_MS = 3000;
 const uint32_t HEADER_TIMEOUT_MS = 15000; // 핵심: 기존 8초는 짧아서 header 못 받는 케이스 많음
 const uint32_t BODY_TIMEOUT_MS   = 15000;

 // --- static clients ---
#if defined(ESP8266)
 static BearSSL::WiFiClientSecure s_tls;
 static WiFiClient s_plain;
 WiFiClient* c = nullptr;

 if (useTls) {
   // TLS 설정
   if (cfg.allowInsecureTls) {
     s_tls.setInsecure();
   } else {
     // rootCaPem을 쓰고 싶으면 여기서 setTrustAnchors로 넣어야 함(현재는 allowInsecureTls 권장)
     s_tls.setInsecure(); // 현실적으로 CA 넣기 전까지는 insecure가 안정적
   }
   s_tls.setTimeout(CONNECT_TIMEOUT_MS / 1000);
   s_tls.setNoDelay(true);
   s_tls.setBufferSizes(512, 512);
   s_tls.stop();

   c = (WiFiClient*)&s_tls;
 } else {
   s_plain.setTimeout(CONNECT_TIMEOUT_MS / 1000);
   s_plain.stop();
   c = &s_plain;
 }
#elif defined(ESP32)
 static WiFiClientSecure s_tls;
 static WiFiClient s_plain;
 WiFiClient* c = nullptr;

 if (useTls) {
   if (cfg.allowInsecureTls) s_tls.setInsecure();
   else s_tls.setInsecure();
   s_tls.setTimeout(CONNECT_TIMEOUT_MS / 1000);
   s_tls.stop();
   c = (WiFiClient*)&s_tls;
 } else {
   s_plain.setTimeout(CONNECT_TIMEOUT_MS / 1000);
   s_plain.stop();
   c = &s_plain;
 }
#endif

 // connect
 if (cfg.debugHttp) {
   Serial.printf("[%s] connect try host=%s port=%u tls=%d\n", logPrefix, host, port, useTls ? 1 : 0);
 }

 uint32_t t0 = millis();
 bool connected = c->connect(host, port);
 uint32_t elapsed = millis() - t0;

 if (!connected || elapsed > CONNECT_TIMEOUT_MS) {
   if (cfg.debugHttp) {
     Serial.printf("[%s] connect failed elapsed=%u\n", logPrefix, (unsigned)elapsed);
   }
   if (useTls) {
     s_httpsFailCount++;
     if (s_httpsFailCount >= MAX_HTTPS_FAIL_COUNT) {
       if (cfg.debugHttp) Serial.printf("[%s] HTTPS failcount=%u -> fallback HTTP\n", logPrefix, s_httpsFailCount);
       return safePostJson(cfg, host, 80, false, path, jsonBody, outStatus, outBody, outBodyMax, logPrefix);
     }
   }
   c->stop();
   return false;
 }

 // send request
 size_t bodyLen = jsonBody ? strlen(jsonBody) : 0;

 // 작은 버퍼로 헤더 작성
 char req[512];
 int reqLen = snprintf(req, sizeof(req),
   "POST %s HTTP/1.1\r\n"
   "Host: %s\r\n"
   "Content-Type: application/json\r\n"
   "Content-Length: %u\r\n"
   "Connection: close\r\n"
   "\r\n",
   path, host, (unsigned)bodyLen
 );
 if (reqLen <= 0 || (size_t)reqLen >= sizeof(req)) {
   c->stop();
   return false;
 }

 if (c->write((const uint8_t*)req, (size_t)reqLen) != (size_t)reqLen) {
   c->stop();
   return false;
 }
 yield();

 if (bodyLen > 0) {
   if (c->write((const uint8_t*)jsonBody, bodyLen) != bodyLen) {
     c->stop();
     return false;
   }
 }
 yield();

 // ---- FIRST BYTE WAIT (중요) ----
 uint32_t fb0 = millis();
 while (!c->available() && (millis() - fb0) < FIRST_BYTE_WAIT_MS) {
   delay(1);
   yield();
 }

 // header read (connected() 조건 제거!)
 uint32_t h0 = millis();
 bool headerDone = false;
 size_t headerBytes = 0;
 size_t contentLength = 0;

 char line[256];
 size_t lp = 0;

 while ((millis() - h0) < HEADER_TIMEOUT_MS && headerBytes < 2048) {
   if (!c->available()) {
     // 연결은 유지되는데 available이 늦는 경우가 있음
     delay(1);
     yield();
     continue;
   }
   int ch = c->read();
   if (ch < 0) { delay(1); yield(); continue; }

   headerBytes++;
   if (ch == '\r') continue;

   if (ch == '\n') {
     line[lp] = '\0';
     lp = 0;

     if (line[0] == '\0') { // empty line => end header
       headerDone = true;
       break;
     }

     if (strncmp(line, "HTTP/", 5) == 0) {
       const char* sp = strchr(line, ' ');
       if (sp) *outStatus = atoi(sp + 1);
     }

     if (strncasecmp(line, "Content-Length:", 15) == 0) {
       contentLength = (size_t)atoi(line + 15);
     }

     yield();
     continue;
   }

   if (lp < sizeof(line) - 1) {
     line[lp++] = (char)ch;
   }

   if ((headerBytes % 64) == 0) yield();
 }

 if (!headerDone) {
   if (cfg.debugHttp) {
     Serial.printf("[%s] header timeout (read=%u)\n", logPrefix, (unsigned)headerBytes);
   }
   c->stop();
   return false;
 }

 // body read
 uint32_t b0 = millis();
 size_t total = 0;

 while ((millis() - b0) < BODY_TIMEOUT_MS && total < outBodyMax - 1) {
   if (!c->available()) {
     // Content-Length가 있고 다 읽었으면 종료
     if (contentLength > 0 && total >= contentLength) break;
     // 연결이 끊겼고 더 없으면 종료
     if (!c->connected()) break;
     delay(1);
     yield();
     continue;
   }

   size_t toRead = outBodyMax - 1 - total;
   if (toRead > 256) toRead = 256;
   int r = c->read((uint8_t*)(outBody + total), toRead);
   if (r > 0) total += (size_t)r;

   if ((total % 128) == 0) yield();
   if (contentLength > 0 && total >= contentLength) break;
 }

 outBody[total] = '\0';

 if (useTls && total > 0 && *outStatus > 0) s_httpsFailCount = 0;

 c->stop();
 yield();

 return (total > 0 && *outStatus > 0);
}

// -----------------------------
// URL parse helper
// -----------------------------
struct ParsedBaseUrl {
 char host[128];
 uint16_t port;
 bool useTls;
 char basePath[128]; // optional
};

static bool parseBaseUrl(const char* base, ParsedBaseUrl& out) {
 if (!base || !base[0]) return false;

 const char* p = base;
 out.useTls = true;
 out.port = 443;
 out.basePath[0] = '\0';

 if (strncmp(p, "https://", 8) == 0) { p += 8; out.useTls = true; out.port = 443; }
 else if (strncmp(p, "http://", 7) == 0) { p += 7; out.useTls = false; out.port = 80; }
 else { /* no scheme => https */ out.useTls = true; out.port = 443; }

 const char* hostStart = p;
 const char* portPos = strchr(hostStart, ':');
 const char* pathPos = strchr(hostStart, '/');

 size_t hostLen = 0;
 if (portPos && (!pathPos || portPos < pathPos)) {
   hostLen = (size_t)(portPos - hostStart);
   out.port = (uint16_t)atoi(portPos + 1);
 } else if (pathPos) {
   hostLen = (size_t)(pathPos - hostStart);
 } else {
   hostLen = strlen(hostStart);
 }

 if (hostLen == 0 || hostLen >= sizeof(out.host)) return false;
 memcpy(out.host, hostStart, hostLen);
 out.host[hostLen] = '\0';

 if (pathPos) {
   strncpy(out.basePath, pathPos, sizeof(out.basePath) - 1);
   out.basePath[sizeof(out.basePath) - 1] = '\0';
   // strip trailing '/'
   size_t L = strlen(out.basePath);
   if (L > 1 && out.basePath[L - 1] == '/') out.basePath[L - 1] = '\0';
 } else {
   out.basePath[0] = '\0';
 }

 return true;
}

// 허브 통일 규칙: WS endpoint는 /ws/tunnel 로 고정.
static bool buildWsTunnelUrl(const char* hubBaseUrl, char* outTunnelUrl, size_t urlSz) {
 if (!hubBaseUrl || !hubBaseUrl[0] || !outTunnelUrl || urlSz < 32) return false;
 ParsedBaseUrl u;
 if (!parseBaseUrl(hubBaseUrl, u)) return false;
 int n = snprintf(outTunnelUrl, urlSz, "wss://%s/ws/tunnel", u.host);
 if (n <= 0 || (size_t)n >= urlSz) return false;
 return true;
}

// DEBUG ONLY: full token logging (remove in production)
static void logTokenPrefix(const char* tag, const char* token) {
 if (!token || !token[0]) {
   Serial.printf("[TUNNEL] %s (empty)\n", tag);
   return;
 }
 size_t len = strlen(token);
 Serial.printf("[TUNNEL] %s bearer_token=%s (len=%u)\n", tag, token, (unsigned)len);
}

static bool joinPath(const char* basePath, const char* path, char* out, size_t outSz) {
 if (!out || outSz == 0 || !path) return false;
 if (!basePath || !basePath[0]) {
   strncpy(out, path, outSz - 1);
   out[outSz - 1] = '\0';
   return true;
 }
 // basePath + path
 size_t a = strlen(basePath);
 size_t b = strlen(path);
 if (a + b + 1 >= outSz) return false;
 strncpy(out, basePath, outSz - 1);
 out[outSz - 1] = '\0';
 strncat(out, path, outSz - strlen(out) - 1);
 return true;
}

// -----------------------------
// Constructor
// -----------------------------
OrbiSyncNode::OrbiSyncNode(const Config& config)
 : cfg_(config),
   state_(State::BOOT),
   nextHelloMs_(0), nextPairMs_(0), nextApproveMs_(0), nextSessionPollMs_(0), nextRegisterBySlotMs_(0),
   netBackoffMs_(kBackoffMinMs), pairBackoffMs_(kBackoffMinMs),
   nextTunnelConnectMs_(0),
   tunnelBackoffMs_(kTunnelBackoffMs[0]),
   tunnelBackoffIndex_(0),
   lastTunnelPingMs_(0),
   tunnelRegistered_(false),
   approveMissingMacFailed_(false),
   lastHeartbeatMs_(0),
   wifiConnecting_(false),
   httpBusy_(false),
   stateChangeCb_(nullptr),
   errorCb_(nullptr),
   registeredCb_(nullptr),
   sessionInvalidCb_(nullptr),
   tunnelChangeCb_(nullptr),
   requestHandler_(nullptr),
   tunnelMessageCb_(nullptr),
   httpRequestCb_(nullptr) {

 nodeId_[0] = '\0';
 nodeToken_[0] = '\0';
 tunnelUrl_[0] = '\0';
 tunnelId_[0] = '\0';
 sessionToken_[0] = '\0';
 sessionExpiresAt_[0] = '\0';
 pairingCode_[0] = '\0';
 pairingExpiresAt_[0] = '\0';
 pairingCodeValid_ = false;
}

// -----------------------------
// WiFi
// -----------------------------
void OrbiSyncNode::beginWiFi(const char* ssid, const char* pass) {
 WiFi.mode(WIFI_STA);
 WiFi.begin(ssid, pass);
 wifiConnecting_ = true;
}

void OrbiSyncNode::ensureWiFi() {
 if (WiFi.status() == WL_CONNECTED) {
   wifiConnecting_ = false;
   return;
 }
 wifiConnecting_ = true;
 yield();
}

// -----------------------------
// State
// -----------------------------
void OrbiSyncNode::setState(State s) {
 if (state_ == s) return;
 State old = state_;
 state_ = s;
 Serial.printf("[STATE] %s -> %s\n", stateStr(old), stateStr(s));
 if (s == State::ACTIVE && cfg_.enableTunnel) {
   Serial.printf("[TUNNEL] ACTIVE entered tunnel_url_set=%d (next connect in %ums)\n",
     tunnelUrl_[0] ? 1 : 0, (unsigned)nextTunnelConnectMs_);
   if (tunnelUrl_[0]) nextTunnelConnectMs_ = 0;
 }
 if (stateChangeCb_) stateChangeCb_(old, s);
}

// -----------------------------
// MAC / IDs
// -----------------------------
const char* OrbiSyncNode::getMacCStr() {
 uint8_t mac[6];
 WiFi.macAddress(mac);
 snprintf(s_macBuf, sizeof(s_macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
 return s_macBuf;
}

void OrbiSyncNode::getMachineId(char* buf, size_t size) {
 if (!buf || size == 0) return;
 const char* prefix = (cfg_.machineIdPrefix && cfg_.machineIdPrefix[0]) ? cfg_.machineIdPrefix : "node-";
 const char* mac = getMacCStr();
 snprintf(buf, size, "%s%s", prefix, mac ? mac : "");
}

uint32_t OrbiSyncNode::computeCapabilitiesHash() const {
 uint32_t h = 0;
 for (uint8_t i = 0; i < cfg_.capabilityCount && cfg_.capabilities; i++) {
   const char* s = cfg_.capabilities[i];
   if (!s) continue;
   while (*s) h = h * 31 + (uint8_t)(*s++);
 }
 return h;
}

void OrbiSyncNode::getDeviceInfoJson(char* buf, size_t size) {
 const char* mac = getMacCStr();
 const char* mid = (cfg_.machineIdPrefix && cfg_.machineIdPrefix[0]) ? cfg_.machineIdPrefix : "node-";
 const char* nn  = (cfg_.nodeNamePrefix && cfg_.nodeNamePrefix[0]) ? cfg_.nodeNamePrefix : "Node-";

 snprintf(buf, size,
   "{\"mac\":\"%s\",\"machine_id\":\"%s%s\",\"node_name\":\"%s%s\",\"platform\":\"esp\"}",
   mac ? mac : "",
   mid, mac ? mac : "",
   nn, mac ? mac : ""
 );
}

// -----------------------------
// Pairing helpers
// -----------------------------
void OrbiSyncNode::clearPairingCode() {
 pairingCode_[0] = '\0';
 pairingExpiresAt_[0] = '\0';
 pairingCodeValid_ = false;
}

void OrbiSyncNode::storePairingFromHello(const char* code, const char* expiresAt) {
 pairingCodeValid_ = false;
 if (!code || !code[0]) return;

 size_t n = 0;
 while (code[n] && n < kPairingCodeMax - 1) { pairingCode_[n] = code[n]; n++; }
 pairingCode_[n] = '\0';

 n = 0;
 if (expiresAt) {
   while (expiresAt[n] && n < sizeof(pairingExpiresAt_) - 1) {
     pairingExpiresAt_[n] = expiresAt[n];
     n++;
   }
 }
 pairingExpiresAt_[n] = '\0';
 pairingCodeValid_ = (pairingCode_[0] != '\0');
}

bool OrbiSyncNode::isPairingExpired() const {
 // NTP 없는 MCU에서 expires_at 엄밀 비교는 의미가 적음 → 유효로 간주, 실패 시 폐기
 if (!pairingCodeValid_) return true;
 if (pairingExpiresAt_[0] == '\0') return false;
 return false;
}

// -----------------------------
// Backoff
// -----------------------------
void OrbiSyncNode::advanceNetBackoff() {
  uint32_t next = netBackoffMs_ * 2;
  if (next > kBackoffMaxMs) next = kBackoffMaxMs;
  netBackoffMs_ = next;
}
void OrbiSyncNode::advancePairBackoff() { pairBackoffMs_ = (pairBackoffMs_ < kBackoffMaxMs / 2) ? (pairBackoffMs_ * 2) : kBackoffMaxMs; }
void OrbiSyncNode::resetNetBackoff() { netBackoffMs_ = kBackoffMinMs; }
void OrbiSyncNode::resetPairBackoff() { pairBackoffMs_ = kBackoffMinMs; }

// -----------------------------
// HTTP unified
// -----------------------------
bool OrbiSyncNode::postJsonUnified(const char* path, const char* body,
                                  int* outStatus, char* outBody, size_t outBodyMax) {
 if (!cfg_.hubBaseUrl || !path || !outStatus || !outBody || outBodyMax == 0) return false;

 ParsedBaseUrl u;
 if (!parseBaseUrl(cfg_.hubBaseUrl, u)) return false;

 // full path = basePath + path
 char fullPath[256];
 if (!joinPath(u.basePath, path, fullPath, sizeof(fullPath))) return false;

 bool useTls = u.useTls;
 uint16_t port = u.port;

 if (useTls && s_httpsFailCount >= MAX_HTTPS_FAIL_COUNT) {
   useTls = false;
   port = 80;
   if (cfg_.debugHttp) Serial.printf("[HTTP] HTTPS failed %u times -> force HTTP\n", s_httpsFailCount);
 }

 return safePostJson(cfg_, u.host, port, useTls, fullPath, body, outStatus, outBody, outBodyMax, "HTTP");
}

// -----------------------------
// HELLO
// -----------------------------
static char s_helloBuf[512];
static char s_helloResp[1024];

void OrbiSyncNode::tryHello() {
 if (httpBusy_) return;
 uint32_t now = millis();
 if (now < nextHelloMs_) return;

 StaticJsonDocument<384> doc;
 doc["slot_id"] = cfg_.slotId ? cfg_.slotId : "";
 doc["firmware"] = (cfg_.firmwareVersion && cfg_.firmwareVersion[0]) ? cfg_.firmwareVersion : "1.0.0";

 if (cfg_.sendReconnectHintInHello) {
   doc["reconnect"] = true;
#if defined(ESP32)
   int r = (int)esp_reset_reason();
   doc["boot_reason"] = (r == 1) ? "power" : (r == 3) ? "sw" : (r == 4) ? "watchdog" : "reboot";
#else
   doc["boot_reason"] = "reboot";
#endif
 }

 char capHashStr[9];
 snprintf(capHashStr, sizeof(capHashStr), "%08x", (unsigned)computeCapabilitiesHash());
 doc["capabilities_hash"] = capHashStr;

 char nonceStr[9];
 snprintf(nonceStr, sizeof(nonceStr), "%08x", (unsigned)random(0x7FFFFFFF));
 doc["nonce"] = nonceStr;

 JsonObject di = doc.createNestedObject("device_info");
 di["mac"] = getMacCStr();
 di["platform"] = "esp";

 size_t n = serializeJson(doc, s_helloBuf, sizeof(s_helloBuf));
 if (n == 0 || n >= sizeof(s_helloBuf)) return;
 s_helloBuf[n] = '\0';

 httpBusy_ = true;
 int status = 0;
 s_helloResp[0] = '\0';
 bool ok = postJsonUnified("/api/device/hello", s_helloBuf, &status, s_helloResp, sizeof(s_helloResp));
 httpBusy_ = false;

 yield();
 handleHelloResponse(ok ? status : -1, s_helloResp, strlen(s_helloResp));
}

static void maskPairingForLog(const char* s, char* buf, size_t bufLen) {
 if (!buf || bufLen == 0) return;
 buf[0] = '\0';
 if (!s) return;
 size_t len = strlen(s);
 if (len == 0) return;
 if (len <= 2) { strncpy(buf, "**", bufLen - 1); buf[bufLen-1]='\0'; return; }
 if (len == 3) { snprintf(buf, bufLen, "%.1s**", s); return; }
 snprintf(buf, bufLen, "%.2s**%s", s, s + len - 2);
}

void OrbiSyncNode::handleHelloResponse(int status, const char* body, size_t len) {
 if (status == 410) {
   Serial.println("[HELLO] 410 pairing expired -> clear pairing, retry HELLO with backoff");
   clearPairingCode();
   if (sessionInvalidCb_) sessionInvalidCb_();
   nextHelloMs_ = millis() + netBackoffMs_;
   advanceNetBackoff();
   setState(State::HELLO);
   return;
 }
 if (status == 403) {
   Serial.printf("[HELLO] web_auth_failed/403 -> backoff %ums (2s~60s, no loop)\n", (unsigned)netBackoffMs_);
   nextHelloMs_ = millis() + netBackoffMs_;
   advanceNetBackoff();
   setState(State::HELLO);
   return;
 }
 if (status == 401) {
   Serial.printf("[HELLO] 401 -> backoff %ums\n", (unsigned)netBackoffMs_);
   nextHelloMs_ = millis() + netBackoffMs_;
   advanceNetBackoff();
   setState(State::HELLO);
   return;
 }
 if (status < 200 || status >= 300 || !body || len == 0) {
   Serial.printf("[HELLO] fail status=%d\n", status);
   advanceNetBackoff();
   nextHelloMs_ = millis() + netBackoffMs_;
   return;
 }

 StaticJsonDocument<512> doc;
 size_t parseLen = (len > 768) ? 768 : len; // 너무 길면 일부만 파싱
 DeserializationError err = deserializeJson(doc, body, parseLen);
 if (err) {
   Serial.printf("[HELLO] parse err (len=%u)\n", (unsigned)len);
   advanceNetBackoff();
   nextHelloMs_ = millis() + netBackoffMs_;
   return;
 }

 const char* st = doc["status"] | "";
 int retryMs = doc["retry_after_ms"] | 3000;

 if (strcmp(st, "DENIED") == 0) {
   Serial.println("[HELLO] DENIED");
   setState(State::ERROR);
   if (errorCb_) errorCb_("HELLO denied");
   nextHelloMs_ = millis() + (uint32_t)retryMs;
   return;
 }

 // pairing key variants
 const char* pc = nullptr;
 const char* usedKey = "";
 if (doc["pairing_code"].is<const char*>()) { pc = doc["pairing_code"]; usedKey = "pairing_code"; }
 else if (doc["pairing"].is<const char*>()) { pc = doc["pairing"]; usedKey = "pairing"; }
 else if (doc["code"].is<const char*>()) { pc = doc["code"]; usedKey = "code"; }
 if (!pc) pc = "";

 const char* exp = doc["pairing_expires_at"] | doc["expires_at"] | "";

 if (pc[0]) {
   storePairingFromHello(pc, exp);
   char masked[16]; maskPairingForLog(pc, masked, sizeof(masked));
   Serial.printf("[HELLO] pairing key=%s value=%s expires=%s\n", usedKey, masked, (exp && exp[0]) ? exp : "(none)");
 } else {
   clearPairingCode();
   Serial.println("[HELLO] no pairing_code (pending)");
 }

 resetNetBackoff();
 nextHelloMs_ = millis() + (uint32_t)retryMs;
 nextSessionPollMs_ = millis() + (uint32_t)retryMs;
 nextApproveMs_ = millis() + 500;
 nextPairMs_ = millis() + 500;

 if (pairingCodeValid_) {
   if (cfg_.enableSelfApprove && cfg_.approveEndpointPath && cfg_.approveEndpointPath[0]) {
     setState(State::PENDING_POLL);
   } else {
     setState(State::PAIR_SUBMIT);
   }
 } else {
   setState(State::HELLO);
 }
}

// -----------------------------
// PAIR
// -----------------------------
bool OrbiSyncNode::postDevicePair(const char* code, int* outStatus,
                                 char* outBody, size_t outBodyMax) {
 StaticJsonDocument<512> doc;
 doc["slot_id"] = cfg_.slotId ? cfg_.slotId : "";
 doc["pairing_code"] = code ? code : "";
 doc["firmware"] = (cfg_.firmwareVersion && cfg_.firmwareVersion[0]) ? cfg_.firmwareVersion : "1.0.0";
 JsonObject di = doc.createNestedObject("device_info");
 di["mac"] = getMacCStr();
 di["platform"] = "esp";

 char buf[512];
 size_t n = serializeJson(doc, buf, sizeof(buf));
 if (n == 0 || n >= sizeof(buf)) return false;
 buf[n] = '\0';

 return postJsonUnified("/api/device/pair", buf, outStatus, outBody, outBodyMax);
}

void OrbiSyncNode::tryPairIfNeeded() {
 if (!pairingCodeValid_ || !pairingCode_[0]) return;
 if (httpBusy_) return;
 uint32_t now = millis();
 if (now < nextPairMs_) return;

 if (isPairingExpired()) {
   clearPairingCode();
   setState(State::HELLO);
   nextHelloMs_ = millis() + 1000;
   return;
 }

 httpBusy_ = true;
 int status = 0;
 char resp[1024];
 resp[0] = '\0';
 bool ok = postDevicePair(pairingCode_, &status, resp, sizeof(resp));
 httpBusy_ = false;

 yield();

 if (!ok || status < 200 || status >= 300) {
   Serial.printf("[PAIR] fail status=%d\n", status);
   clearPairingCode();
   advancePairBackoff();
   setState(State::HELLO);
   nextHelloMs_ = millis() + pairBackoffMs_;
   return;
 }

 StaticJsonDocument<1024> doc;
 if (deserializeJson(doc, resp)) {
   Serial.println("[PAIR] parse err");
   clearPairingCode();
   setState(State::HELLO);
   nextHelloMs_ = millis() + 3000;
   return;
 }

 bool success = doc["ok"] | false;
 if (!success) {
   Serial.println("[PAIR] ok=false");
   clearPairingCode();
   advancePairBackoff();
   setState(State::HELLO);
   nextHelloMs_ = millis() + pairBackoffMs_;
   return;
 }

 const char* nid = doc["node_id"] | doc["canonical_node_id"] | doc["resolved_node_id"] | "";
 const char* stok = doc["session_token"] | "";
 const char* ntok = doc["node_token"] | "";
 const char* tun  = doc["tunnel_url"] | "";

 if (nid[0]) {
   strncpy(nodeId_, nid, sizeof(nodeId_) - 1);
   nodeId_[sizeof(nodeId_) - 1] = '\0';
   Serial.printf("[PAIR] canonical node_id=%s (from hub)\n", nodeId_);
 }
 if (stok[0]) { strncpy(sessionToken_, stok, sizeof(sessionToken_) - 1); sessionToken_[sizeof(sessionToken_) - 1] = '\0'; }
 if (ntok[0]) { strncpy(nodeToken_, ntok, sizeof(nodeToken_) - 1); nodeToken_[sizeof(nodeToken_) - 1] = '\0'; }
 if (cfg_.hubBaseUrl && buildWsTunnelUrl(cfg_.hubBaseUrl, tunnelUrl_, sizeof(tunnelUrl_))) {
   nextTunnelConnectMs_ = 0;
   Serial.printf("[TUNNEL] from pair ws_url=%s\n", tunnelUrl_);
 } else if (tun[0]) {
   // (레거시) 서버가 tunnel_url을 내려주는 경우
   strncpy(tunnelUrl_, tun, sizeof(tunnelUrl_) - 1);
   tunnelUrl_[sizeof(tunnelUrl_) - 1] = '\0';
   nextTunnelConnectMs_ = 0;
   Serial.printf("[TUNNEL] from pair legacy tunnel_url=%s\n", tunnelUrl_);
 }

 resetPairBackoff();
 clearPairingCode();

 Serial.println("[PAIR] ok -> ACTIVE");
 if (registeredCb_) registeredCb_(nodeId_[0] ? nodeId_ : "");
 setState(State::ACTIVE);
 lastHeartbeatMs_ = millis();
 nextSessionPollMs_ = millis() + 60000;
}

// -----------------------------
// APPROVE (self-approve)
// ---- Hub 승인 API 호출 ----
static char s_approveBuf[512];
static char s_approveResp[2048];

/// Hub에 approve 요청 전송 (세션 토큰 획득)
void OrbiSyncNode::tryApprove() {
 if (httpBusy_ || approveMissingMacFailed_) return;
 if (!cfg_.approveEndpointPath || !cfg_.approveEndpointPath[0]) return;

 uint32_t now = millis();
 if (now < nextApproveMs_) return;

 if (!pairingCodeValid_ || !pairingCode_[0]) {
   nextApproveMs_ = millis() + cfgOrDefaultU32(cfg_.approveRetryMs, 3000);
   return;
 }

 const char* mac = getMacCStr();
 if (!mac || !mac[0]) {
   if (errorCb_) errorCb_("approve: MAC unavailable");
   approveMissingMacFailed_ = true;
   return;
 }

 char machineId[80];
 getMachineId(machineId, sizeof(machineId));

 const char* fw = (cfg_.firmwareVersion && cfg_.firmwareVersion[0]) ? cfg_.firmwareVersion : "1.0.0";

 int n = snprintf(s_approveBuf, sizeof(s_approveBuf),
   "{\"slot_id\":\"%s\",\"pairing_code\":\"%s\",\"mac\":\"%s\",\"machine_id\":\"%s\",\"firmware\":\"%s\"}",
   cfg_.slotId ? cfg_.slotId : "",
   pairingCode_,
   mac,
   machineId,
   fw
 );
 if (n <= 0 || (size_t)n >= sizeof(s_approveBuf)) {
   nextApproveMs_ = millis() + 3000;
   return;
 }

 Serial.printf("[TUNNEL] request: method=POST path=%s body_len=%u\n", cfg_.approveEndpointPath ? cfg_.approveEndpointPath : "", (unsigned)n);

 // approve는 postJsonUnified를 쓰되 path만 approveEndpointPath로
 httpBusy_ = true;
 int status = 0;
 s_approveResp[0] = '\0';

 bool ok = postJsonUnified(cfg_.approveEndpointPath, s_approveBuf, &status, s_approveResp, sizeof(s_approveResp));
 httpBusy_ = false;

 yield();

 size_t respLen = strlen(s_approveResp);
 Serial.printf("[TUNNEL] response: status=%d body_len=%u\n", status, (unsigned)respLen);
 if (respLen > 0) logBodyPreview("APPROVE", s_approveResp, respLen);

 if (!ok) {
   Serial.println("[APPROVE] fail (timeout or connect)");
   advanceNetBackoff();
   nextApproveMs_ = millis() + cfgOrDefaultU32(cfg_.approveRetryMs, 3000);
   return;
 }

 if (status == 400 && strstr(s_approveResp, "missing_mac")) {
   Serial.println("[APPROVE] 400 missing_mac -> stop retry");
   if (errorCb_) errorCb_("approve: missing_mac");
   approveMissingMacFailed_ = true;
   return;
 }
 if (status == 401 || status == 403 || status == 410) {
   Serial.printf("[APPROVE] %d auth/pairing invalid -> clear session & pairing, back to HELLO\n", status);
   clearPairingCode();
   sessionToken_[0] = '\0';
   sessionExpiresAt_[0] = '\0';
   if (sessionInvalidCb_) sessionInvalidCb_();
   advanceNetBackoff();
   setState(State::HELLO);
   nextHelloMs_ = millis() + netBackoffMs_;
   return;
 }

 if (status < 200 || status >= 300) {
   Serial.printf("[APPROVE] fail http status=%d\n", status);
   nextApproveMs_ = millis() + cfgOrDefaultU32(cfg_.approveRetryMs, 3000);
   return;
 }

 StaticJsonDocument<1536> doc;
 if (deserializeJson(doc, s_approveResp)) {
   Serial.println("[APPROVE] parse err");
   nextApproveMs_ = millis() + 3000;
   return;
 }

 const char* st = doc["status"] | "";
 const char* tok = doc["session_token"] | "";
 const char* exp = doc["expires_at"] | doc["session_expires_at"] | "";
 const char* ntok = doc["register_token"] | doc["node_token"] | "";
 const char* tun = doc["tunnel_url"] | "";
 const char* nid = doc["node_id"] | doc["canonical_node_id"] | doc["resolved_node_id"] | "";

 if (tok && tok[0]) {
   strncpy(sessionToken_, tok, sizeof(sessionToken_) - 1);
   sessionToken_[sizeof(sessionToken_) - 1] = '\0';
 }
 if (exp && exp[0]) {
   size_t el = strlen(exp);
   if (el < sizeof(sessionExpiresAt_)) {
     strncpy(sessionExpiresAt_, exp, sizeof(sessionExpiresAt_) - 1);
     sessionExpiresAt_[sizeof(sessionExpiresAt_) - 1] = '\0';
   }
 } else {
   sessionExpiresAt_[0] = '\0';
 }
 if (ntok && ntok[0]) {
   strncpy(nodeToken_, ntok, sizeof(nodeToken_) - 1);
   nodeToken_[sizeof(nodeToken_) - 1] = '\0';
 }
 if (nid && nid[0]) {
   strncpy(nodeId_, nid, sizeof(nodeId_) - 1);
   nodeId_[sizeof(nodeId_) - 1] = '\0';
   Serial.printf("[APPROVE] canonical node_id=%s (from hub)\n", nodeId_);
 }
 if (tok && tok[0] && cfg_.hubBaseUrl && buildWsTunnelUrl(cfg_.hubBaseUrl, tunnelUrl_, sizeof(tunnelUrl_))) {
   nextTunnelConnectMs_ = 0;
   Serial.printf("[TUNNEL] from approve ws_url=%s\n", tunnelUrl_);
 } else if (tun && tun[0]) {
   // (레거시) 서버가 tunnel_url을 내려주는 경우
   strncpy(tunnelUrl_, tun, sizeof(tunnelUrl_) - 1);
   tunnelUrl_[sizeof(tunnelUrl_) - 1] = '\0';
   nextTunnelConnectMs_ = 0;
 }

 resetNetBackoff();
 approveMissingMacFailed_ = false;

 if (registeredCb_) registeredCb_(nodeId_[0] ? nodeId_ : "");
 setState(State::ACTIVE);
 lastHeartbeatMs_ = millis();
 nextSessionPollMs_ = millis() + 60000;
}

// ---- 세션 갱신 (저장된 token으로 재부팅 시 먼저 시도) ----
bool OrbiSyncNode::trySessionRefresh() {
 if (!sessionToken_[0] || httpBusy_) return false;

 const char* path = (cfg_.sessionEndpointPath && cfg_.sessionEndpointPath[0]) ? cfg_.sessionEndpointPath : "/api/device/session";

 StaticJsonDocument<384> doc;
 doc["slot_id"] = cfg_.slotId ? cfg_.slotId : "";
 doc["session_token"] = sessionToken_;

 char buf[384];
 size_t n = serializeJson(doc, buf, sizeof(buf));
 if (n == 0 || n >= sizeof(buf)) return false;
 buf[n] = '\0';

 if (cfg_.debugHttp) Serial.printf("[SESSION] refresh with stored token path=%s\n", path);
 httpBusy_ = true;
 int status = 0;
 char resp[1024];
 resp[0] = '\0';
 bool ok = postJsonUnified(path, buf, &status, resp, sizeof(resp));
 httpBusy_ = false;

 yield();

 size_t rl = strlen(resp);
 if (cfg_.debugHttp && rl > 0) logBodyPreview("SESSION", resp, rl);

 if (!ok) {
   Serial.println("[SESSION] refresh fail (timeout/connect)");
   return false;
 }
 if (status == 401 || status == 403 || status == 410) {
   Serial.printf("[SESSION] refresh %d -> clear token, fallback HELLO\n", status);
   sessionToken_[0] = '\0';
   sessionExpiresAt_[0] = '\0';
   if (sessionInvalidCb_) sessionInvalidCb_();
   return false;
 }
 if (status != 200 && status != 201) {
   Serial.printf("[SESSION] refresh status=%d -> fallback HELLO\n", status);
   return false;
 }

 StaticJsonDocument<512> r;
 size_t pl = (rl > 512) ? 512 : rl;
 if (deserializeJson(r, resp, pl)) {
   Serial.println("[SESSION] refresh parse err");
   return false;
 }

 const char* st = r["status"] | "";
 if (strcmp(st, "GRANTED") != 0) {
   Serial.printf("[SESSION] refresh status body=%s -> fallback HELLO\n", st);
   return false;
 }

 const char* tok = r["session_token"] | "";
 const char* tun = r["tunnel_url"] | "";
 const char* exp = r["expires_at"] | r["session_expires_at"] | "";
 if (tok[0]) {
   strncpy(sessionToken_, tok, sizeof(sessionToken_) - 1);
   sessionToken_[sizeof(sessionToken_) - 1] = '\0';
 }
 if (exp[0]) {
   size_t el = strlen(exp);
   if (el < sizeof(sessionExpiresAt_)) {
     strncpy(sessionExpiresAt_, exp, sizeof(sessionExpiresAt_) - 1);
     sessionExpiresAt_[sizeof(sessionExpiresAt_) - 1] = '\0';
   }
 } else {
   sessionExpiresAt_[0] = '\0';
 }
 if (cfg_.hubBaseUrl && buildWsTunnelUrl(cfg_.hubBaseUrl, tunnelUrl_, sizeof(tunnelUrl_))) {
   nextTunnelConnectMs_ = 0;
 } else if (tun[0]) {
   strncpy(tunnelUrl_, tun, sizeof(tunnelUrl_) - 1);
   tunnelUrl_[sizeof(tunnelUrl_) - 1] = '\0';
   nextTunnelConnectMs_ = 0;
 }

 resetNetBackoff();
 setState(State::ACTIVE);
 lastHeartbeatMs_ = millis();
 nextSessionPollMs_ = millis() + 60000;
 Serial.println("[SESSION] refresh GRANTED -> ACTIVE (skip HELLO/approve)");
 return true;
}

// ---- 세션 폴링 ----
/// Hub에 session 폴링 요청 (PENDING → GRANTED 대기)
void OrbiSyncNode::trySessionPoll() {
 if (httpBusy_) return;
 uint32_t now = millis();
 if (now < nextSessionPollMs_) return;

 const char* path = (cfg_.sessionEndpointPath && cfg_.sessionEndpointPath[0]) ? cfg_.sessionEndpointPath : "/api/device/session";

 StaticJsonDocument<256> doc;
 doc["slot_id"] = cfg_.slotId ? cfg_.slotId : "";
 char nonceStr[9];
 snprintf(nonceStr, sizeof(nonceStr), "%08x", (unsigned)random(0x7FFFFFFF));
 doc["nonce"] = nonceStr;

 char buf[256];
 size_t n = serializeJson(doc, buf, sizeof(buf));
 if (n == 0 || n >= sizeof(buf)) return;
 buf[n] = '\0';

 Serial.printf("[TUNNEL] request: method=POST path=%s body_len=%u\n", path, (unsigned)n);
 httpBusy_ = true;
 int status = 0;
 char resp[1024];
 resp[0] = '\0';
 bool ok = postJsonUnified(path, buf, &status, resp, sizeof(resp));
 httpBusy_ = false;

 yield();

 size_t rl = strlen(resp);
 Serial.printf("[TUNNEL] response: status=%d body_len=%u\n", status, (unsigned)rl);
 if (rl > 0) logBodyPreview("SESSION", resp, rl);

 if (!ok) {
   Serial.println("[SESSION] fail (timeout or connect)");
   advanceNetBackoff();
   nextSessionPollMs_ = millis() + netBackoffMs_;
   return;
 }

 if (status == 404) {
   Serial.printf("[SESSION] fail http 404 path=%s\n", path);
   nextSessionPollMs_ = millis() + 5000;
   return;
 }
 if (status == 401 || status == 403 || status == 410) {
   Serial.printf("[SESSION] %d invalid -> clear session & pairing, HELLO\n", status);
   sessionToken_[0] = '\0';
   sessionExpiresAt_[0] = '\0';
   clearPairingCode();
   if (sessionInvalidCb_) sessionInvalidCb_();
   advanceNetBackoff();
   setState(State::HELLO);
   nextHelloMs_ = millis() + netBackoffMs_;
   return;
 }

 StaticJsonDocument<512> r;
 size_t pl = (rl > 512) ? 512 : rl;
 if (deserializeJson(r, resp, pl)) {
   Serial.println("[SESSION] fail json parse");
   nextSessionPollMs_ = millis() + 3000;
   return;
 }

 const char* st = r["status"] | "";
 int retryMs = r["retry_after_ms"] | 3000;

 if (strcmp(st, "GRANTED") == 0) {
   const char* tok = r["session_token"] | "";
   const char* exp = r["expires_at"] | r["session_expires_at"] | "";
   const char* tun = r["tunnel_url"] | "";
   if (tok[0]) { strncpy(sessionToken_, tok, sizeof(sessionToken_) - 1); sessionToken_[sizeof(sessionToken_) - 1] = '\0'; }
   if (exp[0] && strlen(exp) < sizeof(sessionExpiresAt_)) {
     strncpy(sessionExpiresAt_, exp, sizeof(sessionExpiresAt_) - 1);
     sessionExpiresAt_[sizeof(sessionExpiresAt_) - 1] = '\0';
   } else {
     sessionExpiresAt_[0] = '\0';
   }
   if (tok[0] && cfg_.hubBaseUrl && buildWsTunnelUrl(cfg_.hubBaseUrl, tunnelUrl_, sizeof(tunnelUrl_))) {
     nextTunnelConnectMs_ = 0;
     Serial.printf("[TUNNEL] from session ws_url=%s\n", tunnelUrl_);
   } else if (tun[0]) {
     // (레거시)
     strncpy(tunnelUrl_, tun, sizeof(tunnelUrl_) - 1);
     tunnelUrl_[sizeof(tunnelUrl_) - 1] = '\0';
     nextTunnelConnectMs_ = 0;
   }
   resetNetBackoff();
   setState(State::ACTIVE);
   lastHeartbeatMs_ = millis();
 } else if (strcmp(st, "DENIED") == 0) {
   setState(State::ERROR);
   if (errorCb_) errorCb_("Session denied");
 }

 nextSessionPollMs_ = millis() + (uint32_t)retryMs;
}

// -----------------------------
// register_by_slot
// -----------------------------
void OrbiSyncNode::tryRegisterBySlot() {
 if (!cfg_.preferRegisterBySlot || !cfg_.loginToken || !cfg_.loginToken[0]) return;
 if (httpBusy_) return;

 uint32_t now = millis();
 if (now < nextRegisterBySlotMs_) return;

 char machineId[80];
 getMachineId(machineId, sizeof(machineId));

 StaticJsonDocument<384> doc;
 doc["slot_id"] = cfg_.slotId ? cfg_.slotId : "";
 doc["login_token"] = cfg_.loginToken;
 doc["machine_id"] = machineId;
 doc["platform"] = "esp";
 if (cfg_.firmwareVersion && cfg_.firmwareVersion[0]) doc["agent_version"] = cfg_.firmwareVersion;

 char buf[384];
 size_t n = serializeJson(doc, buf, sizeof(buf));
 if (n == 0 || n >= sizeof(buf)) return;
 buf[n] = '\0';

 Serial.printf("[TUNNEL] request: method=POST path=/api/nodes/register_by_slot body_len=%u\n", (unsigned)n);
 httpBusy_ = true;
 int status = 0;
 char resp[1024];
 resp[0] = '\0';
 bool ok = postJsonUnified("/api/nodes/register_by_slot", buf, &status, resp, sizeof(resp));
 httpBusy_ = false;

 yield();

 nextRegisterBySlotMs_ = now + cfgOrDefaultU32(cfg_.registerRetryMs, 4000);

 size_t rl = strlen(resp);
 Serial.printf("[TUNNEL] response: status=%d body_len=%u\n", status, (unsigned)rl);
 if (rl > 0) logBodyPreview("REG_SLOT", resp, rl);

 if (!ok) {
   Serial.println("[REG_SLOT] fail (timeout or connect)");
   return;
 }
 if (status < 200 || status >= 300) {
   Serial.printf("[REG_SLOT] fail http status=%d\n", status);
   return;
 }

 StaticJsonDocument<512> r;
 if (deserializeJson(r, resp)) {
   Serial.println("[REG_SLOT] fail json parse");
   return;
 }

 const char* nid = r["node_id"] | "";
 const char* authTok = r["node_auth_token"] | "";
 const char* tun = r["tunnel_url"] | "";
 if (!nid[0] || !authTok[0] || !tun[0]) {
   Serial.println("[REG_SLOT] fail missing fields");
   return;
 }

 strncpy(nodeId_, nid, sizeof(nodeId_) - 1);
 nodeId_[sizeof(nodeId_) - 1] = '\0';
 strncpy(nodeToken_, authTok, sizeof(nodeToken_) - 1);
 nodeToken_[sizeof(nodeToken_) - 1] = '\0';
 strncpy(tunnelUrl_, tun, sizeof(tunnelUrl_) - 1);
 tunnelUrl_[sizeof(tunnelUrl_) - 1] = '\0';

 char tid[64], thost[64];
 parseTunnelUrlParts(tunnelUrl_, tid, sizeof(tid), thost, sizeof(thost));
 Serial.printf("[TUNNEL] from register_by_slot tunnel_url=%s tunnel_id=%s tunnel_host=%s node=%s\n", tunnelUrl_, tid, thost, nodeId_);
 nextTunnelConnectMs_ = 0;
 tunnelBackoffIndex_ = 0;
 tunnelBackoffMs_ = kTunnelBackoffMs[0];

 if (registeredCb_) registeredCb_(nodeId_);
 setState(State::ACTIVE);
 lastHeartbeatMs_ = millis();
}

// -----------------------------
// Heartbeat (단편화 방지: String 금지)
// -----------------------------
void OrbiSyncNode::tryHeartbeat() {
 if (!sessionToken_[0]) return;
 if (httpBusy_) return;

 uint32_t now = millis();
 if (now - lastHeartbeatMs_ < cfgOrDefaultU32(cfg_.heartbeatIntervalMs, 60000)) return;

 StaticJsonDocument<256> doc;
 doc["slot_id"] = cfg_.slotId ? cfg_.slotId : "";
 char nonceStr[9];
 snprintf(nonceStr, sizeof(nonceStr), "%08x", (unsigned)random(0x7FFFFFFF));
 doc["nonce"] = nonceStr;
 doc["firmware"] = (cfg_.firmwareVersion && cfg_.firmwareVersion[0]) ? cfg_.firmwareVersion : "1.0.0";
 doc["uptime_ms"] = (unsigned long)now;
#if defined(ESP8266) || defined(ESP32)
 doc["free_heap"] = ESP.getFreeHeap();
 doc["rssi"] = WiFi.RSSI();
#endif
 char capHashStr[9];
 snprintf(capHashStr, sizeof(capHashStr), "%08x", (unsigned)computeCapabilitiesHash());
 doc["capabilities_hash"] = capHashStr;

 char buf[256];
 size_t n = serializeJson(doc, buf, sizeof(buf));
 if (n == 0 || n >= sizeof(buf)) return;
 buf[n] = '\0';

 // Authorization 헤더는 safePostJson에서 처리하지 않으므로(간소화),
 // 여기서는 heartbeat를 생략하거나, 별도 구현이 필요.
 // 지금 안정화 목표는 "HELLO/APPROVE/SESSION/터널"이라 heartbeat는 기본 OFF를 권장.
 // 필요하면 이후 "Authorization: Bearer" 포함 POST를 별도로 추가하자.
 lastHeartbeatMs_ = now;
}

// -----------------------------
// State machine
// -----------------------------
void OrbiSyncNode::runStateMachine() {
 yield();
 ensureWiFi();
 if (WiFi.status() != WL_CONNECTED) return;

 switch (state_) {
   case State::BOOT: {
     if (sessionToken_[0]) {
       if (trySessionRefresh()) {
         break;
       }
       setState(State::HELLO);
       nextHelloMs_ = millis() + netBackoffMs_;
     } else {
       setState(State::HELLO);
       nextHelloMs_ = millis();
     }
     break;
   }

   case State::HELLO:
     tryHello();
     break;

   case State::PAIR_SUBMIT:
     tryPairIfNeeded();
     break;

   case State::PENDING_POLL: {
     if (cfg_.preferRegisterBySlot && cfg_.loginToken && cfg_.loginToken[0]) {
       tryRegisterBySlot();
     }
     if (cfg_.enableSelfApprove && cfg_.approveEndpointPath && cfg_.approveEndpointPath[0] &&
         !sessionToken_[0] && !approveMissingMacFailed_) {
       tryApprove();
     }
     if (!sessionToken_[0]) {
       trySessionPoll();
     }
     break;
   }

   case State::GRANTED:
     setState(State::ACTIVE);
     lastHeartbeatMs_ = millis();
     break;

   case State::ACTIVE:
   case State::TUNNEL_CONNECTING:
   case State::TUNNEL_CONNECTED:
     tunnelLoop();
     // tryHeartbeat(); // 필요하면 켜기
     break;

   case State::ERROR:
     nextHelloMs_ = millis() + netBackoffMs_;
     setState(State::HELLO);
     break;

   default:
     break;
 }

 // (선택) 5초마다 힙 단편화 진단
#if defined(ESP8266)
 static uint32_t lastDiag = 0;
 if (millis() - lastDiag > 5000) {
   lastDiag = millis();
   Serial.printf("[DIAG] heap=%u maxblk=%u frag=%u%% state=%s\n",
     ESP.getFreeHeap(),
     ESP.getMaxFreeBlockSize(),
     ESP.getHeapFragmentation(),
     stateStr(state_)
   );
 }
#endif
}

void OrbiSyncNode::loopTick() {
 runStateMachine();
 yield();
}

// ---- WebSocket 이벤트 콜백 (메모리 할당 없음) ----
static void wsEvent(WStype_t type, uint8_t* payload, size_t len) {
 if (!s_nodeForWs) return;

 switch (type) {
   case WStype_CONNECTED: {
     if (!s_nodeForWs) return;
     const char* url = s_nodeForWs->getTunnelUrl();
     Serial.println("========================================");
     Serial.println("[TUNNEL] WebSocket Handshake SUCCESS");
     Serial.println("========================================");
     Serial.printf("URL: %s\n", url ? url : "(none)");
     Serial.println("HTTP Upgrade: 101 Switching Protocols");
     Serial.println("Connection: Upgrade");
     Serial.println("Upgrade: websocket");
     Serial.println("Sec-WebSocket-Accept: <server-response>");
     Serial.println("========================================");
     Serial.println("[TUNNEL] Sending register message...");
     s_nodeForWs->tunnelSendRegister();
     break;
   }

   case WStype_DISCONNECTED: {
     // Try to get close code/reason if available (library-dependent)
     Serial.printf("[TUNNEL] disconnected len=%u (will reconnect with backoff)\n", (unsigned)len);
     if (len > 0 && payload) {
       // Some libraries pass close code as first 2 bytes
       if (len >= 2) {
         uint16_t closeCode = (payload[0] << 8) | payload[1];
         Serial.printf("[TUNNEL] close_code=%u\n", closeCode);
         if (len > 2) {
           constexpr size_t kReasonPreview = 64;
           size_t rlen = (len - 2) > kReasonPreview ? kReasonPreview : (len - 2);
           Serial.printf("[TUNNEL] close_reason=");
           for (size_t i = 2; i < 2 + rlen; i++) {
             char c = (char)payload[i];
             Serial.print((c >= 32 && c < 127) ? c : '.');
           }
           if (len > 2 + kReasonPreview) Serial.print("...");
           Serial.println();
         }
       }
     }
     s_tunnelDisconnectPending = true;
     break;
   }

   case WStype_ERROR: {
     Serial.println("========================================");
     Serial.println("[TUNNEL] WebSocket Handshake FAILED");
     Serial.println("========================================");
     Serial.printf("Error payload length: %u\n", (unsigned)len);
     if (len > 0 && payload) {
       constexpr size_t kErrorPreview = 128;
       size_t pl = len > kErrorPreview ? kErrorPreview : len;
       Serial.printf("Error data: ");
       for (size_t i = 0; i < pl; i++) {
         char c = (char)payload[i];
         Serial.print((c >= 32 && c < 127) ? c : '.');
       }
       if (len > kErrorPreview) Serial.print("...");
       Serial.println();
     }
     Serial.println("Possible causes:");
     Serial.println("1. TLS/SSL handshake failed (certificate/SNI issue)");
     Serial.println("2. Server rejected HTTP Upgrade request");
     Serial.println("3. Network connectivity issue");
     Serial.println("4. Wrong host/port/path");
     Serial.println("5. Authorization header rejected");
     Serial.println("========================================");
     s_tunnelDisconnectPending = true;
     break;
   }

   case WStype_TEXT:
     if (s_nodeForWs && payload && len > 0) {
       constexpr size_t kWsRxPreview = 256;
       size_t pl = len > kWsRxPreview ? kWsRxPreview : len;
       Serial.printf("[WS_RX] len=%u data=", (unsigned)len);
       for (size_t i = 0; i < pl; i++) {
         char c = (char)payload[i];
         Serial.print((c >= 32 && c < 127) ? c : '.');
       }
       if (len > kWsRxPreview) Serial.print("...");
       Serial.println();
       s_nodeForWs->tunnelHandleMessage(payload, len);
     }
     break;

   case WStype_BIN:
     Serial.printf("[TUNNEL] rx BIN len=%u (ignored)\n", (unsigned)len);
     break;

   case WStype_PING:
     Serial.println("[TUNNEL] rx PING");
     break;

   case WStype_PONG:
     Serial.println("[TUNNEL] rx PONG");
     break;

   default:
     Serial.printf("[TUNNEL] ws_event type=%d len=%u\n", (int)type, (unsigned)len);
     break;
 }
}

// -----------------------------
// ---- 터널 연결 루프 (재연결/keepalive) ----
/// 터널 연결 상태 확인 및 재연결 처리
void OrbiSyncNode::tunnelLoop() {
 uint32_t now = millis();

 if (s_tunnelDisconnectPending) {
   s_tunnelDisconnectPending = false;
   WebSocketsClient* client = s_wsClient;
   OrbiSyncNodeType* node = s_nodeForWs;
   s_wsClient = nullptr;
   s_nodeForWs = nullptr;
   if (client) {
     client->disconnect();
     delete client;
   }
   if (node) node->tunnelDisconnectCleanup();
   return;
 }

 if (!cfg_.enableTunnel) {
   if (now - s_lastTunnelSkipLogMs >= kTunnelStatusLogIntervalMs) {
     s_lastTunnelSkipLogMs = now;
     Serial.println("[TUNNEL] skip: enableTunnel=0");
   }
   return;
 }
 if (!tunnelUrl_[0]) {
   if (now - s_lastTunnelSkipLogMs >= kTunnelStatusLogIntervalMs) {
     s_lastTunnelSkipLogMs = now;
     Serial.println("[TUNNEL] skip: no tunnel_url (session/pair/approve not returned tunnel_url)");
   }
   return;
 }
 if (!nodeToken_[0] && !sessionToken_[0]) {
   if (now - s_lastTunnelSkipLogMs >= kTunnelStatusLogIntervalMs) {
     s_lastTunnelSkipLogMs = now;
     Serial.println("[TUNNEL] skip: no node_token or session_token");
   }
   return;
 }

 if (s_wsClient) {
   s_wsClient->loop();
   if (!s_wsClient) return;

   if (s_wsClient->isConnected()) {
     if (now - s_lastTunnelStatusLogMs >= kTunnelStatusLogIntervalMs) {
       s_lastTunnelStatusLogMs = now;
       Serial.printf("[TUNNEL] connected=%s (registered=%d)\n", tunnelRegistered_ ? "true" : "false", tunnelRegistered_ ? 1 : 0);
     }
     if (tunnelRegistered_ && (now - lastTunnelPingMs_ >= kTunnelPingIntervalMs)) {
       StaticJsonDocument<64> pingDoc;
       pingDoc["type"] = "ping";
       char pingBuf[64];
       size_t n = serializeJson(pingDoc, pingBuf, sizeof(pingBuf));
       if (n > 0 && n < sizeof(pingBuf)) {
         pingBuf[n] = '\0';
         if (tunnelSendText(pingBuf)) {
           lastTunnelPingMs_ = now;
           Serial.println("[TUNNEL] ping sent");
         } else {
           Serial.println("[TUNNEL] ping send failed");
         }
       }
     }
   }
   yield();
   return;
 }

 if (now < nextTunnelConnectMs_) return;

 Serial.printf("[TUNNEL] start attempt state=%s heap=%u millis=%lu\n", stateStr(state_), (unsigned)ESP.getFreeHeap(), (unsigned long)now);
 Serial.printf("[TUNNEL] reconnect: node_id=%s tunnel_id=%s\n", nodeId_[0] ? nodeId_ : "(none)", tunnelId_[0] ? tunnelId_ : "(none)");
 setState(State::TUNNEL_CONNECTING);
 tunnelConnect();
 nextTunnelConnectMs_ = now + tunnelBackoffMs_;
}

/// WebSocket 연결 시작 (wss://hub.orbisync.io/ws/tunnel)
void OrbiSyncNode::tunnelConnect() {
 if (s_wsClient) return;

 const char* url = tunnelUrl_;
 if (!url || !url[0]) return;

 const char* auth = sessionToken_[0] ? sessionToken_ : nullptr;
 if (!auth || !auth[0]) {
   Serial.println("[TUNNEL] skip connect: session_token empty (run approve first)");
   nextTunnelConnectMs_ = millis() + 3000;
   nextApproveMs_ = 0;
   return;
 }

 bool ssl = (strncmp(url, "wss://", 6) == 0);
 if (!ssl && strncmp(url, "ws://", 5) != 0) {
   Serial.printf("[TUNNEL] invalid URL scheme: %s (expected wss:// or ws://)\n", url);
   return;
 }

 const char* hostStart = ssl ? url + 6 : url + 5;
 const char* pathStart = strchr(hostStart, '/');
 if (!pathStart) pathStart = "/";

 char host[128];
 size_t hostLen = (size_t)(pathStart - hostStart);
 if (hostLen >= sizeof(host)) hostLen = sizeof(host) - 1;
 memcpy(host, hostStart, hostLen);
 host[hostLen] = '\0';

 uint16_t port = ssl ? 443 : 80;

 // Validate path (must be /ws/tunnel)
 if (strcmp(pathStart, "/ws/tunnel") != 0) {
   Serial.printf("[TUNNEL] WARNING: path=%s (expected /ws/tunnel)\n", pathStart);
 }

 Serial.println("========================================");
 Serial.println("[TUNNEL] WebSocket Handshake Debug");
 Serial.println("========================================");
 Serial.printf("URL: %s\n", url);
 Serial.printf("Host: %s\n", host);
 Serial.printf("Port: %u\n", port);
 Serial.printf("Path: %s\n", pathStart);
 Serial.printf("SSL/TLS: %s\n", ssl ? "YES (wss://)" : "NO (ws://)");
 Serial.println("----------------------------------------");
 Serial.println("Expected HTTP Upgrade Request:");
 Serial.printf("GET %s HTTP/1.1\r\n", pathStart);
 Serial.printf("Host: %s\r\n", host);
 Serial.println("Connection: Upgrade");
 Serial.println("Upgrade: websocket");
 Serial.println("Sec-WebSocket-Key: <base64-random>");
 Serial.println("Sec-WebSocket-Version: 13");
 Serial.printf("Authorization: Bearer <token>\r\n");
 Serial.println("========================================");

 s_wsClient = new WebSocketsClient();
 s_nodeForWs = this;

 // Enable debug mode if available (Links2004 WebSocketsClient may support this)
 // Note: Some versions use enableHeartbeat() or setReconnectInterval() for keepalive
 // For TLS insecure mode, WebSocketsClient internally uses WiFiClientSecure
 // which may need setInsecure() - but we can't access it directly here.
 // The library should handle this based on beginSSL() parameters.

 if (ssl) {
   // beginSSL(host, port, path) - TLS connection
   // Links2004 WebSocketsClient uses WiFiClientSecure internally
   // For ESP32: WiFiClientSecure defaults to insecure if no CA is set
   // For ESP8266: BearSSL needs explicit setInsecure() - but we can't access it here
   Serial.println("[TUNNEL] Calling beginSSL() - TLS handshake will start");
   s_wsClient->beginSSL(host, port, pathStart);
 } else {
   Serial.println("[TUNNEL] Calling begin() - non-TLS connection");
   s_wsClient->begin(host, port, pathStart);
 }

 // Set authorization header BEFORE onEvent (order matters)
 char authHeader[320];
 int ahLen = snprintf(authHeader, sizeof(authHeader), "Bearer %s", auth);
 if (ahLen > 0 && (size_t)ahLen < sizeof(authHeader)) {
   s_wsClient->setAuthorization(authHeader);
   // DEBUG ONLY: full token logging (remove in production)
   logTokenPrefix("[TUNNEL] auth_header_set=1", auth);
   Serial.printf("[TUNNEL] Authorization header length: %d bytes\n", ahLen);
   Serial.printf("[TUNNEL] Authorization header: %s\n", authHeader);
 } else {
   Serial.println("[TUNNEL] ERROR: auth_header_set=0 (buffer fail)");
   Serial.printf("[TUNNEL] Token length: %zu, Buffer size: %zu\n", strlen(auth), sizeof(authHeader));
 }

 // Set event callback
 s_wsClient->onEvent(wsEvent);

 Serial.printf("[TUNNEL] WebSocket client initialized\n");
 Serial.printf("[TUNNEL] SNI/Host=%s (should match hub.orbisync.io)\n", host);
 Serial.println("[TUNNEL] Waiting for connection result...");
 Serial.println("========================================");
}

void OrbiSyncNode::tunnelDisconnectCleanup() {
 tunnelRegistered_ = false;
 if (tunnelChangeCb_) tunnelChangeCb_(false, tunnelUrl_[0] ? tunnelUrl_ : "");

 if (state_ == State::TUNNEL_CONNECTING || state_ == State::TUNNEL_CONNECTED) {
   setState(State::ACTIVE);
 }

 if (tunnelBackoffIndex_ < kTunnelBackoffSteps - 1) tunnelBackoffIndex_++;
 tunnelBackoffMs_ = kTunnelBackoffMs[tunnelBackoffIndex_];
 nextTunnelConnectMs_ = millis() + tunnelBackoffMs_;
 Serial.printf("[TUNNEL] fail disconnected backoff=%ums step=%u\n", (unsigned)tunnelBackoffMs_, (unsigned)tunnelBackoffIndex_);
}

void OrbiSyncNode::tunnelDisconnect() {
 WebSocketsClient* client = s_wsClient;
 s_wsClient = nullptr;
 s_nodeForWs = nullptr;

 if (client) {
   client->disconnect();
   delete client;
 }
 tunnelDisconnectCleanup();
}

void OrbiSyncNode::setSessionToken(const char* token) {
 if (!token) {
   sessionToken_[0] = '\0';
   sessionExpiresAt_[0] = '\0';
   return;
 }
 strncpy(sessionToken_, token, sizeof(sessionToken_) - 1);
 sessionToken_[sizeof(sessionToken_) - 1] = '\0';
}

void OrbiSyncNode::setSessionExpiresAt(const char* expiresAt) {
 if (!expiresAt) { sessionExpiresAt_[0] = '\0'; return; }
 size_t el = strlen(expiresAt);
 if (el >= sizeof(sessionExpiresAt_)) el = sizeof(sessionExpiresAt_) - 1;
 memcpy(sessionExpiresAt_, expiresAt, el);
 sessionExpiresAt_[el] = '\0';
}

bool OrbiSyncNode::tunnelSendText(const char* text) {
 if (!s_wsClient || !s_wsClient->isConnected() || !text) return false;
 return s_wsClient->sendTXT(text);
}

void OrbiSyncNode::tunnelSendRegister() {
 if (!s_wsClient || !s_wsClient->isConnected()) return;
 if (tunnelChangeCb_) tunnelChangeCb_(true, tunnelUrl_[0] ? tunnelUrl_ : "");

 /// Hub에 register 요청 전송 (터널 등록)
 // /ws/tunnel registry: auth_token 필수(세션 토큰)
 if (!sessionToken_[0]) {
   Serial.println("[TUNNEL] register skip: session_token empty");
   return;
 }

 char machineId[80];
 getMachineId(machineId, sizeof(machineId));

 StaticJsonDocument<512> doc;
 doc["type"] = "register";
 if (nodeId_[0]) doc["node_id"] = nodeId_;
 doc["slot_id"] = cfg_.slotId && cfg_.slotId[0] ? cfg_.slotId : "";
 doc["machine_id"] = machineId;
 doc["mac"] = getMacCStr();
 doc["firmware"] = (cfg_.firmwareVersion && cfg_.firmwareVersion[0]) ? cfg_.firmwareVersion : "1.0.0";
 doc["auth_token"] = sessionToken_;

 char buf[512];
 size_t n = serializeJson(doc, buf, sizeof(buf));
 if (n == 0 || n >= sizeof(buf)) return;
 buf[n] = '\0';

 Serial.print("[TUNNEL] register payload: ");
 Serial.println(buf);

 bool ok = tunnelSendText(buf);
 Serial.printf("[TUNNEL] register sent ok=%d\n", ok ? 1 : 0);
}

// ---- WebSocket 메시지 핸들러 ----
void OrbiSyncNode::tunnelHandleMessage(const char* payload) {
 if (!payload) return;
 tunnelHandleMessage((const uint8_t*)payload, strlen(payload));
}

/// Hub → Node 메시지 처리 (HTTP_REQ, register_ack, RPC 등)
void OrbiSyncNode::tunnelHandleMessage(const uint8_t* payload, size_t len) {
 if (!payload || len == 0) return;

 // JSON 파싱 (복사 없음)
 StaticJsonDocument<1536> peek;
 DeserializationError err = deserializeJson(peek, payload, len);
 if (err) {
   Serial.printf("[TUNNEL] rx parse err len=%u\n", (unsigned)len);
   return;
 }

 // RPC envelope 처리: {id, method, path, body}
 if (peek.containsKey("id") && peek.containsKey("path")) {
   const char* method = peek["method"] | "GET";
   const char* path = peek["path"] | "/";
   JsonVariant bodyV = peek["body"];
   size_t bodyLen = 0;
   if (bodyV.is<JsonObject>() || bodyV.is<JsonArray>()) bodyLen = measureJson(bodyV);
   else if (bodyV.is<const char*>()) bodyLen = strlen(bodyV.as<const char*>());
 
   if (peek["id"].is<const char*>()) {
     Serial.printf("[HTTP_TUNNEL] id=%s method=%s path=%s body_len=%u\n", peek["id"].as<const char*>(), method, path, (unsigned)bodyLen);
   } else {
     Serial.printf("[HTTP_TUNNEL] id=%ld method=%s path=%s body_len=%u\n", peek["id"].as<long>(), method, path, (unsigned)bodyLen);
   }
 
   bool isLedOn = (strcmp(path, "/led/on") == 0) || (strstr(path, "led/on") != nullptr);
   int value = 1;
   if (bodyV.is<JsonObject>()) value = bodyV["value"] | 1;
 
   if (isLedOn && cfg_.ledPin >= 0) {
     digitalWrite(cfg_.ledPin, value ? LOW : HIGH);
   }
 
   StaticJsonDocument<384> resp;
   if (peek["id"].is<const char*>()) resp["id"] = peek["id"].as<const char*>();
   else resp["id"] = peek["id"].as<long>();
   resp["status"] = 200;
   JsonObject b = resp.createNestedObject("body");
   b["ok"] = true;
   if (isLedOn) b["value"] = value ? 1 : 0;
 
   char out[512];
   size_t n = serializeJson(resp, out, sizeof(out));
   if (n > 0 && n < sizeof(out)) {
     out[n] = '\0';
     tunnelSendText(out);
   }
   return;
 }

 const char* type = peek["type"] | "";
 if (!type[0]) return;

 // register_ack 처리: Hub가 register 요청 승인
 if (strcmp(type, "register_ack") == 0) {
   const char* st = peek["status"] | "";
   const char* reason = peek["reason"] | "";
   const char* detail = peek["detail"] | "";
   const char* nid = peek["node_id"] | "";
   const char* tid = peek["tunnel_id"] | "";
   const char* tunUrl = peek["tunnel_url"] | peek["ws_url"] | "";
   const char* thost = peek["tunnel_host"] | peek["domain"] | peek["host"] | "";

   Serial.println("================================\n[TUNNEL REGISTER ACK]");
   Serial.printf("status    = %s\n", st);
   if (nid && nid[0]) Serial.printf("node_id   = %s\n", nid);
   if (tid && tid[0]) Serial.printf("tunnel_id = %s\n", tid);
   if (tunUrl && tunUrl[0]) Serial.printf("url       = %s\n", tunUrl);
   if (thost && thost[0]) Serial.printf("host      = %s\n", thost);
   if (reason[0]) Serial.printf("reason    = %s\n", reason);
   if (detail[0]) Serial.printf("detail    = %s\n", detail);
   Serial.println("================================");

   if (strcmp(st, "ok") == 0) {
     bool updated = false;
     if (nid && nid[0]) {
       strncpy(nodeId_, nid, sizeof(nodeId_) - 1);
       nodeId_[sizeof(nodeId_) - 1] = '\0';
       Serial.printf("[TUNNEL_ACK] ok node_id=%s\n", nodeId_);
       updated = true;
     }
     if (tid && tid[0]) {
       strncpy(tunnelId_, tid, sizeof(tunnelId_) - 1);
       tunnelId_[sizeof(tunnelId_) - 1] = '\0';
       Serial.printf("[TUNNEL_ACK] ok tunnel_id=%s\n", tunnelId_);
       updated = true;
     }
     if (tunUrl && tunUrl[0]) {
       // Store tunnel_url if provided (may differ from constructed URL)
       strncpy(tunnelUrl_, tunUrl, sizeof(tunnelUrl_) - 1);
       tunnelUrl_[sizeof(tunnelUrl_) - 1] = '\0';
       Serial.printf("[TUNNEL_ACK] ok tunnel_url=%s\n", tunnelUrl_);
       updated = true;
     }
     if (!updated) {
       Serial.println("[TUNNEL_ACK] ok (no node_id/tunnel_id/tunnel_url in response)");
     }
     tunnelRegistered_ = true;
     tunnelBackoffIndex_ = 0;
     tunnelBackoffMs_ = kTunnelBackoffMs[0];
     setState(State::TUNNEL_CONNECTED);
     lastTunnelPingMs_ = millis();
     Serial.println("[TUNNEL] connected=true (registered=1)");
     if (tunnelMessageCb_) { }
     return;
   }

   Serial.printf("[TUNNEL] register_ack status=error reason=%s detail=%s\n", reason, detail[0] ? detail : "(none)");
   if (strcmp(reason, "MISSING_AUTH_TOKEN") == 0) {
     Serial.println("[TUNNEL] action: re-run approve to get session_token");
     sessionToken_[0] = '\0';
     nextApproveMs_ = 0;
     nextTunnelConnectMs_ = millis() + 3000;
   } else if (strcmp(reason, "SLOT_ID_MISMATCH") == 0) {
     Serial.println("[TUNNEL] action: align slot_id with token or fix payload");
     nextTunnelConnectMs_ = millis() + tunnelBackoffMs_;
   } else if (strcmp(reason, "SESSION_TOKEN_MISSING_SLOT_ID") == 0) {
     Serial.println("[TUNNEL] action: check approve response / token type");
     nextApproveMs_ = 0;
     nextTunnelConnectMs_ = millis() + 3000;
   } else {
     nextTunnelConnectMs_ = millis() + tunnelBackoffMs_;
   }
   return;
 }

 // Hub → Node HTTP 요청 수신
 if (strcmp(type, "HTTP_REQ") == 0) {
   const char* streamId = peek["stream_id"] | "";
   const char* method = peek["method"] | "GET";
   const char* path = peek["path"] | "/";
   
   Serial.printf("[HTTP_REQ] stream_id=%s method=%s path=%s\n", streamId ? streamId : "(none)", method, path);
   
   // stream_id 검증 (응답 매칭 필수)
   if (!streamId || !streamId[0]) {
     Serial.println("[HTTP_REQ] ERROR: missing stream_id, cannot send HTTP_RES");
     return;
   }
   
   // Handle LED control
   bool ledOn = false;
   bool ledOff = false;
   if (strcmp(path, "/led/on") == 0) {
     ledOn = true;
   } else if (strcmp(path, "/led/off") == 0) {
     ledOff = true;
   }
   
   int status = 200;
   const char* bodyText = "OK";
   
   if (ledOn && cfg_.ledPin >= 0) {
     digitalWrite(cfg_.ledPin, LOW);  // LOW = ON for most ESP32 boards
     bodyText = "OK LED ON";
     Serial.println("[HTTP_REQ] LED turned ON");
   } else if (ledOff && cfg_.ledPin >= 0) {
     digitalWrite(cfg_.ledPin, HIGH);  // HIGH = OFF for most ESP32 boards
     bodyText = "OK LED OFF";
     Serial.println("[HTTP_REQ] LED turned OFF");
   } else if ((ledOn || ledOff) && cfg_.ledPin < 0) {
     status = 500;
     bodyText = "LED pin not configured";
     Serial.println("[HTTP_REQ] ERROR: LED pin not configured");
   }
   
   // Node → Hub HTTP 응답 전송
   StaticJsonDocument<512> respDoc;
   respDoc["type"] = "HTTP_RES";
   respDoc["stream_id"] = streamId;  // 요청과 동일한 stream_id 사용
   respDoc["status"] = status;
   JsonObject headersObj = respDoc.createNestedObject("headers");
   headersObj["content-type"] = "text/plain";
   respDoc["body"] = bodyText;
   
   char respBuf[512];
   size_t respLen = serializeJson(respDoc, respBuf, sizeof(respBuf));
   if (respLen > 0 && respLen < sizeof(respBuf)) {
     respBuf[respLen] = '\0';
     bool sent = tunnelSendText(respBuf);
     if (sent) {
       Serial.printf("[HTTP_RES] sent stream_id=%s status=%d body=%s\n", streamId, status, bodyText);
     } else {
       Serial.printf("[HTTP_RES] FAILED to send stream_id=%s status=%d (WS not connected?)\n", streamId, status);
     }
   } else {
     Serial.printf("[HTTP_RES] FAILED: buffer too small (needed %u, have %u)\n", (unsigned)respLen, (unsigned)sizeof(respBuf));
     // Try to send error response with smaller body
     StaticJsonDocument<256> errDoc;
     errDoc["type"] = "HTTP_RES";
     errDoc["stream_id"] = streamId;
     errDoc["status"] = 500;
     JsonObject errHeaders = errDoc.createNestedObject("headers");
     errHeaders["content-type"] = "text/plain";
     errDoc["body"] = "Internal error: response buffer overflow";
     char errBuf[256];
     size_t errLen = serializeJson(errDoc, errBuf, sizeof(errBuf));
     if (errLen > 0 && errLen < sizeof(errBuf)) {
       errBuf[errLen] = '\0';
       tunnelSendText(errBuf);
     }
   }
   return;
 }

 // proxy_request 처리 (레거시 형식)
 if (strcmp(type, "proxy_request") == 0) {
   const char* reqId = peek["request_id"] | peek["req_id"] | "";
   const char* method = peek["method"] | "GET";
   const char* path = peek["path"] | "/";
   const char* bodyB64 = peek["body"] | "";
   size_t bodyLen = bodyB64 && bodyB64[0] ? (strlen(bodyB64) / 4) * 3 : 0;
   Serial.printf("[HTTP_TUNNEL] req_id=%s method=%s path=%s body_len=%u\n", reqId, method, path, (unsigned)bodyLen);
   tunnelHandleProxyRequest(payload, len);
   return;
 }

 if (cfg_.debugHttp) {
   Serial.printf("[TUNNEL] rx type=%s len=%u\n", type, (unsigned)len);
 }
}

void OrbiSyncNode::tunnelHandleProxyRequest(const uint8_t* payload, size_t len) {
 if (!payload || len == 0) return;

 StaticJsonDocument<1536> doc;
 if (deserializeJson(doc, payload, len)) {
   Serial.println("[HTTP_REQ] parse err");
   return;
 }

 const char* reqId = doc["request_id"] | doc["req_id"] | "";
 const char* method = doc["method"] | "GET";
 const char* path = doc["path"] | "/";
 const char* query = doc["query"] | "";
 const char* bodyB64 = doc["body"] | "";

 size_t maxBody = cfgOrDefaultSz(cfg_.maxTunnelBodyBytes, kDefaultMaxTunnelBody);
 size_t bodyLen = 0;
 uint8_t* bodyDec = nullptr;

 if (bodyB64 && bodyB64[0]) {
   size_t b64Len = strlen(bodyB64);
   bodyLen = (b64Len / 4) * 3 + 4;
   if (bodyLen > maxBody) {
     Serial.printf("[HTTP_REQ] body too large %u -> 413\n", (unsigned)bodyLen);
     TunnelHttpResponseWriter res;
     res.node_ = this;
     strncpy(res.requestId_, reqId, sizeof(res.requestId_) - 1);
     res.requestId_[sizeof(res.requestId_) - 1] = '\0';
     res.setStatus(413);
     res.setHeader("Content-Type", "text/plain");
     res.write("Payload Too Large");
     res.end();
     return;
   }
   bodyDec = (uint8_t*)malloc(bodyLen);
   if (bodyDec) {
     bodyLen = base64Decode(bodyDec, bodyB64, strlen(bodyB64));
   } else {
     bodyLen = 0;
   }
 }

 TunnelHttpRequest req = {};
 req.requestId = reqId;
 req.streamId = reqId;
 req.tunnelId = nodeId_;
 req.method = method;
 req.path = path;
 req.query = query;
 req.body = bodyDec;
 req.bodyLen = bodyLen;
 req.headerCount = 0;

 JsonObject headers = doc["headers"];
 if (headers) {
   for (JsonPair p : headers) {
     if (req.headerCount >= TUNNEL_MAX_HEADERS) break;
     const char* k = p.key().c_str();
     const char* v = p.value().as<const char*>();
     if (k && v) {
       strncpy(req.headers[req.headerCount].key, k, 23);
       req.headers[req.headerCount].key[23] = '\0';
       strncpy(req.headers[req.headerCount].value, v, 79);
       req.headers[req.headerCount].value[79] = '\0';
       req.headerCount++;
     }
   }
 }

 TunnelHttpResponseWriter res;
 res.node_ = this;
 strncpy(res.requestId_, reqId, sizeof(res.requestId_) - 1);
 res.requestId_[sizeof(res.requestId_) - 1] = '\0';

 bool isLedOn = (strstr(path, "led/on") != nullptr);

 if (isLedOn) {
   res.setStatus(200);
   res.setHeader("Content-Type", "application/json");
   res.write("{\"ok\":true,\"value\":1}");
   res.end();
 } else {
   res.setStatus(200);
   res.setHeader("Content-Type", "application/json");
   char minBody[128];
   int n = snprintf(minBody, sizeof(minBody), "{\"ok\":true,\"request_id\":\"%s\"}", reqId[0] ? reqId : "");
   if (n > 0 && (size_t)n < sizeof(minBody)) res.write(minBody);
   res.end();
 }

 if (httpRequestCb_) {
   httpRequestCb_(req, res);
 }

 if (bodyDec) free(bodyDec);
 if (!res.ended_) res.end();
}

void OrbiSyncNode::tunnelSendProxyResponse(TunnelHttpResponseWriter& res) {
 if (!s_wsClient || !s_wsClient->isConnected()) return;

 size_t b64Len = (res.bodyLen_ / 3 + 1) * 4 + 1;
 char* b64 = (char*)malloc(b64Len);
 if (!b64) return;

 base64Encode(b64, res.body_, res.bodyLen_);

 StaticJsonDocument<1024> doc;
 doc["type"] = "proxy_response";
 doc["request_id"] = res.requestId_;
 doc["status_code"] = res.statusCode_;

 JsonObject headersObj = doc.createNestedObject("headers");
 for (uint8_t i = 0; i < res.headerCount_; i++) {
   headersObj[res.headers_[i].key] = res.headers_[i].value;
 }

 doc["body"] = b64;

 char buf[1536];
 size_t n = serializeJson(doc, buf, sizeof(buf));
 if (n > 0 && n < sizeof(buf)) {
   buf[n] = '\0';
   tunnelSendText(buf);
 }
 free(b64);

 Serial.printf("[HTTP_RESP] status=%d len=%u\n", res.statusCode_, (unsigned)res.bodyLen_);
}

} // namespace OrbiSyncNode
