/*
 * PoolController.ino
 * ---------------------------------------------------------------
 * ESP32-S3 firmware for the Relay & Pump Timer app.
 *
 * Exposes a Nordic-UART-style BLE GATT service:
 *   SERVICE_UUID  6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX_UUID       6E400002-...  (app -> ESP32, WRITE / WRITE_NR)
 *   TX_UUID       6E400003-...  (ESP32 -> app, NOTIFY)
 *
 * Protocol: newline-delimited JSON, chunked to fit the negotiated
 * MTU. The ESP32 is the source of truth: the app sends small
 * "cmd" messages, the ESP32 mutates its internal state and pushes
 * the FULL state back down after every change (and on a heartbeat).
 *
 * ===============================================================
 *  SECURITY MODEL  (see also the matching block in app/index.tsx)
 * ===============================================================
 * Connecting to this device is NOT enough to control it. Every
 * connection must prove it knows DEVICE_SECRET before a single
 * relay or pump command is accepted:
 *
 *   1. App connects, discovers services, subscribes to TX, and
 *      sends {"cmd":"hello"}.
 *   2. ESP32 replies {"type":"challenge","nonce":"<32 hex chars>"}
 *      — 16 fresh random bytes from the hardware RNG, per session.
 *   3. App replies {"cmd":"auth","hmac":"<64 hex chars>"} where
 *      hmac = HMAC-SHA256(key = DEVICE_SECRET, msg = nonce hex).
 *   4. ESP32 recomputes the HMAC and compares in constant time.
 *      Match   -> session marked authenticated, state pushed.
 *      No match -> immediate disconnect.
 *
 * A session that has not authenticated within AUTH_TIMEOUT_MS is
 * dropped. Any command other than "hello"/"auth" sent before
 * authentication is refused and the session is dropped.
 *
 * The nonce is single-use and never repeats, so a sniffed auth
 * frame cannot be replayed against a later session.
 *
 * Result: a generic BLE tool (nRF Connect, LightBlue, a phone's
 * Bluetooth settings screen) can still SEE and CONNECT to the
 * device — BLE advertising is public and that cannot be hidden —
 * but without DEVICE_SECRET it fails the challenge and is
 * disconnected before it can toggle anything.
 *
 * >>> CHANGE DEVICE_SECRET BEFORE YOU SHIP. <<<
 * It must match DEVICE_SECRET in app/index.tsx byte for byte.
 *
 * Optional link-layer encryption: set REQUIRE_LINK_ENCRYPTION to 1
 * to additionally require a bonded, encrypted connection. Read the
 * comment on that #define first — it can make iOS show a one-time
 * system pairing prompt.
 *
 * Up to MAX_SESSIONS (3) phones may be connected and authenticated
 * at the same time; a 4th connection is refused immediately.
 *
 * On first boot (and on demand via Serial), this sketch prints the
 * JSON payload that should be turned into a QR code:
 *   {"v":2,"name":"PoolCtrl-XXXXXX","svc":"6e400001-b5a3-f393-e0a9-e50e24dcca9e"}
 * Note the QR does NOT contain the secret — the secret is compiled
 * into both the firmware and the app.
 *
 * Serial commands (type the letter + Enter in the Serial Monitor):
 *   q  reprint the QR payload
 *   s  list active sessions
 *   b  erase all BLE bonds
 *
 * Libraries required (Arduino Library Manager):
 *   - NimBLE-Arduino   (h2zero/NimBLE-Arduino, v2.x)
 *   - ArduinoJson      (bblanchon/ArduinoJson, v6+)
 *
 * Board: "ESP32S3 Dev Module" (or your specific board variant)
 *
 * >>> REQUIRED: Tools -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)"
 * The Wi-Fi + WebServer stack pushes this sketch to ~90% of the default
 * 1.25MB app partition, which leaves no room to grow and will start
 * failing to link as soon as anything else is added. Huge APP moves it
 * to ~37% of 3MB. The tradeoff is no OTA update partition, which this
 * project does not use.
 * ---------------------------------------------------------------
 */

#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>   // built into the ESP32 core — no extra library to install
#include <time.h>
#include "esp_mac.h"     // esp_read_mac / ESP_MAC_BT — not auto-included on newer esp32 cores
#include "esp_random.h"  // esp_random() — hardware RNG for the auth nonce
#include "mbedtls/md.h"  // HMAC-SHA256
#include "freertos/semphr.h"   // with the other includes at the top

SemaphoreHandle_t txMutex = nullptr;

// ============================================================
//  BLE UUIDs — must match SERVICE_UUID / RX / TX in app/index.tsx
// ============================================================
#define SERVICE_UUID        "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_RX   "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // app -> esp32
#define CHARACTERISTIC_TX   "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // esp32 -> app

static const char DEVICE_SECRET[] =
  "3f9a1c7e5b28d04a6ef391c8b7d25a06e4c8193bd7f6025a8c1e93b47dfa6210";

// Auth deadline from the moment a central connects. Generous enough
// to cover GATT service discovery + CCCD subscribe on a slow phone.
#define AUTH_TIMEOUT_MS 10000

// Max simultaneously connected phones. NimBLE's own ceiling is
// CONFIG_BT_NIMBLE_MAX_CONNECTIONS (3 by default) — raising this
// above that has no effect unless you also raise the NimBLE config.
#define MAX_SESSIONS 3

// ------------------------------------------------------------
//  REQUIRE_LINK_ENCRYPTION
// ------------------------------------------------------------
// 0 (default) — the GATT characteristics are readable/writable on a
//   plain connection, and access control is enforced purely by the
//   HMAC challenge above. Guarantees ZERO pairing popups on both
//   iOS and Android.
//
// 1 — additionally marks the characteristics as requiring an
//   encrypted link, so the phone must bond before it can read or
//   write. Pairing uses "Just Works" (no passkey, nothing to type),
//   because a React Native app has no way to feed a passkey to the
//   OS — iOS gives CoreBluetooth exclusive control of pairing, and
//   Android's setPasskey() is a hidden framework method. The
//   tradeoff: iOS commonly shows a one-time "Bluetooth Pairing
//   Request / Pair?" system alert the first time. Turn this on only
//   if you want over-the-air encryption and acycept that prompt.
//
// Either way the HMAC challenge is what actually keeps other apps
// out — Just Works pairing has no secret, so it stops nobody.
#define REQUIRE_LINK_ENCRYPTION 1

// ============================================================
//  WI-FI / HTTP TRANSPORT
// ============================================================
// The app talks to this device over BLE *or* over HTTP on the LAN.
// BLE is the provisioning channel and the fallback; Wi-Fi is the
// preferred day-to-day transport once credentials are stored.
//
// Both transports enforce the SAME HMAC-SHA256 secret. Over HTTP the
// flow is GET /challenge -> POST /auth -> bearer token, mirroring the
// BLE hello/challenge/auth handshake so there is one security model.
#define HTTP_PORT            80
#define MAX_HTTP_SESSIONS     4
#define HTTP_TOKEN_TTL_MS   3600000UL   // 1 hour
#define WIFI_CONNECT_TIMEOUT_MS 20000UL // give up on a join after this
#define WIFI_RETRY_INTERVAL_MS  30000UL // then retry periodically

// NTP. Until Wi-Fi is up the system clock is unset, which means the
// schedule evaluator has been comparing against a 1970 timestamp —
// so schedules only actually became reliable once this landed.
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"

// ============================================================
//  HARDWARE PINS  — adjust to your wiring
// ============================================================
const uint8_t RELAY_PINS[3] = { 4, 5, 6 };     // Relay 1..3
const uint8_t PUMP_ENABLE_PINS[4] = { 7, 8, 9, 10 }; // Pump 1..4 (placeholder — see runPumpDriver())
const bool RELAY_ACTIVE_HIGH = true;

// ============================================================
//  STATE MODEL  — mirrors the TS types in app/index.tsx
// ============================================================
struct Relay {
  uint8_t id;
  char name[24];
  char mode[6];       // "auto" | "on" | "off"
  bool active;
  char countdown[40];
};

struct Pump {
  uint8_t id;
  char name[24];
  char mode[6];
  bool active;
  uint16_t speedRpm;
  char pumpType[8];   // "Pentair" | "Emaux"
  char countdown[40];
};

struct Schedule {
  char id[8];
  char target[16];    // "Relay 1" | "Pump 2" ...
  uint8_t hour24;      // 0-23, stored normalized (not 12h/ampm) for easy comparison
  uint8_t minute;
  uint16_t durationMin;
  bool enabled;
  bool inUse;
};

#define MAX_SCHEDULES 8

Relay relays[3];
Pump pumps[4];
Schedule schedules[MAX_SCHEDULES];

Preferences prefs;

// ============================================================
//  BLE GLOBALS
// ============================================================
NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pTxCharacteristic = nullptr;
NimBLECharacteristic* pRxCharacteristic = nullptr;

char bleLocalName[24];

// ------------------------------------------------------------
//  PER-CONNECTION SESSION STATE
// ------------------------------------------------------------
// Everything that used to be a single global (deviceConnected,
// rxBuffer, negotiatedMtu) is now per-connection, because up to
// MAX_SESSIONS phones can be talking to us at once and each has its
// own auth state, reassembly buffer and MTU.
struct Session {
  bool     inUse;
  uint16_t connHandle;
  bool     authed;
  char     nonceHex[33];   // 16 random bytes, hex — the auth challenge
  bool     nonceIssued;
  uint32_t connectedAtMs;
  uint16_t mtu;
  String   rxBuffer;       // accumulates chunks until a '\n' delimiter
};

Session sessions[MAX_SESSIONS];

Session* sessionByHandle(uint16_t h) {
  for (int i = 0; i < MAX_SESSIONS; i++)
    if (sessions[i].inUse && sessions[i].connHandle == h) return &sessions[i];
  return nullptr;
}

Session* allocSession(uint16_t h) {
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].inUse) continue;
    sessions[i].inUse         = true;
    sessions[i].connHandle    = h;
    sessions[i].authed        = false;
    sessions[i].nonceIssued   = false;
    sessions[i].nonceHex[0]   = '\0';
    sessions[i].connectedAtMs = millis();
    sessions[i].mtu           = 23; // ATT default until onMTUChange
    sessions[i].rxBuffer      = "";
    return &sessions[i];
  }
  return nullptr;
}

void freeSession(uint16_t h) {
  Session* s = sessionByHandle(h);
  if (!s) return;
  s->inUse    = false;
  s->authed   = false;
  s->rxBuffer = "";
  memset(s->nonceHex, 0, sizeof(s->nonceHex));
}

int authedCount() {
  int n = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].inUse && sessions[i].authed) n++;
  return n;
}

int activeCount() {
  int n = 0;
  for (int i = 0; i < MAX_SESSIONS; i++) if (sessions[i].inUse) n++;
  return n;
}

// ------------------------------------------------------------
//  WI-FI STATE
// ------------------------------------------------------------
enum WifiPhase {
  WIFI_UNCONFIGURED,  // no credentials stored — app should offer provisioning
  WIFI_CONNECTING,
  WIFI_ONLINE,
  WIFI_OFFLINE        // credentials stored but not currently associated
};

char      wifiSsid[33] = "";
char      wifiPass[65] = "";
bool      wifiConfigured = false;
WifiPhase wifiPhase = WIFI_UNCONFIGURED;
uint32_t  wifiAttemptStartedMs = 0;
uint32_t  wifiLastRetryMs = 0;
// True only when a full join attempt ran out the 20s clock — i.e. the
// credentials are likely wrong or the AP is unreachable. A momentary drop
// AFTER a successful association leaves this false, which is how the app
// tells "give up and re-ask for the password" apart from "keep waiting,
// the device is retrying on its own".
bool      wifiJoinFailed = false;
long      tzOffsetSec = 0;      // set by the app; schedules run in local time
bool      timeSynced = false;
char      deviceMac[18] = "";   // "A0:B7:65:4A:2F:91" — the stable device identity

const char* wifiPhaseStr() {
  switch (wifiPhase) {
    case WIFI_UNCONFIGURED: return "unconfigured";
    case WIFI_CONNECTING:   return "connecting";
    case WIFI_ONLINE:       return "online";
    default:                return "offline";
  }
}

// A Wi-Fi scan blocks for several seconds. Running it inside a BLE
// write callback would stall the NimBLE host task and drop the very
// connection asking for the scan, so requests are queued here and
// serviced from loop().
bool     wifiScanRequested = false;
uint16_t wifiScanRequesterConn = 0;
bool     wifiJoinRequested = false;
uint16_t wifiJoinRequesterConn = 0;

// ------------------------------------------------------------
//  HTTP SERVER STATE
// ------------------------------------------------------------
WebServer httpServer(HTTP_PORT);
bool httpServerStarted = false;

// One entry per authenticated HTTP client. Tokens are bearer
// credentials with a TTL — the HTTP equivalent of an authed BLE
// session, and subject to exactly the same secret.
struct HttpSession {
  bool     inUse;
  char     token[33];      // 16 random bytes, hex
  uint32_t issuedAtMs;
};
HttpSession httpSessions[MAX_HTTP_SESSIONS];

// The challenge handed out by GET /challenge, consumed by POST /auth.
// Single-use and regenerated on every request, so a captured /auth
// body cannot be replayed.
char httpNonce[33] = "";
bool httpNonceIssued = false;

// ============================================================
//  CRYPTO HELPERS  (HMAC-SHA256 + constant-time compare + nonce)
// ============================================================
// Computes HMAC-SHA256(key, msg) and writes it as 64 lowercase hex
// characters into outHex (which must hold 65 bytes).
void hmacSha256Hex(const char* key, const char* msg, char* outHex) {
  uint8_t mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1 /* hmac */);
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, strlen(msg));
  mbedtls_md_hmac_finish(&ctx, mac);
  mbedtls_md_free(&ctx);

  // NB: not named HEX — Arduino's Print.h does `#define HEX 16`.
  static const char* HEXDIGITS = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    outHex[i * 2]     = HEXDIGITS[mac[i] >> 4];
    outHex[i * 2 + 1] = HEXDIGITS[mac[i] & 0x0f];
  }
  outHex[64] = '\0';
}

// Length-independent, data-independent comparison. A plain strcmp()
// bails out at the first differing byte, which leaks how much of a
// guess was correct via response timing.
bool constantTimeEquals(const char* a, const char* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

// Boot check that the HMAC path actually works before we rely on it to
// gate the relays. Uses RFC 4231 test case 2 — a fixed, published vector
// independent of DEVICE_SECRET, so changing the secret never breaks it.
// If this prints FAIL, no phone will ever authenticate.
void selfTestHmac() {
  char out[65];
  hmacSha256Hex("Jefe", "what do ya want for nothing?", out);
  const char* expected = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
  Serial.printf("[selftest] HMAC-SHA256 (RFC 4231 #2): %s\n",
                strcmp(out, expected) == 0 ? "PASS" : "FAIL");
  if (strcmp(out, expected) != 0) {
    Serial.printf("[selftest]   expected %s\n[selftest]   got      %s\n", expected, out);
  }
}

// Raw 32-byte HMAC-SHA256, used by the keystream below.
void hmacSha256Raw(const char* key, const char* msg, uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1 /* hmac */);
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, strlen(msg));
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes lowercase/uppercase hex into out. Returns byte count, or -1
// on malformed input.
int hexToBytes(const char* hex, uint8_t* out, size_t maxOut) {
  size_t len = strlen(hex);
  if (len % 2 != 0 || len / 2 > maxOut) return -1;
  for (size_t i = 0; i < len; i += 2) {
    int hi = hexNibble(hex[i]), lo = hexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i / 2] = (uint8_t)((hi << 4) | lo);
  }
  return (int)(len / 2);
}

// ------------------------------------------------------------
//  Credential encryption
// ------------------------------------------------------------
// The Wi-Fi password is the one genuinely sensitive value the app
// ever sends us, so it is not trusted to the link layer alone.
// The app XORs it with a keystream both sides derive from the shared
// secret and the session nonce:
//
//   block[i] = HMAC-SHA256(DEVICE_SECRET, "<nonce>:<i>")
//
// The nonce is single-use, so the keystream never repeats and a
// captured setWifi frame cannot be replayed or decrypted offline
// without the secret. This holds even with REQUIRE_LINK_ENCRYPTION 0.
// Writes a NUL-terminated result into outPlain; returns false if the
// ciphertext is malformed or too long.
bool keystreamDecryptHex(const char* nonce, const char* cipherHex, char* outPlain, size_t maxOut) {
  uint8_t cipher[128];
  int n = hexToBytes(cipherHex, cipher, sizeof(cipher));
  if (n < 0 || (size_t)n >= maxOut) return false;

  uint8_t block[32];
  char blockMsg[48];
  for (int i = 0; i < n; i++) {
    if (i % 32 == 0) {
      snprintf(blockMsg, sizeof(blockMsg), "%s:%d", nonce, i / 32);
      hmacSha256Raw(DEVICE_SECRET, blockMsg, block);
    }
    outPlain[i] = (char)(cipher[i] ^ block[i % 32]);
  }
  outPlain[n] = '\0';
  return true;
}

// 16 bytes from the hardware RNG, hex-encoded into out (33 bytes).
void makeNonceHex(char* out) {
  // NB: not named HEX — Arduino's Print.h does `#define HEX 16`.
  static const char* HEXDIGITS = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    uint8_t b = (uint8_t)(esp_random() & 0xff);
    out[i * 2]     = HEXDIGITS[b >> 4];
    out[i * 2 + 1] = HEXDIGITS[b & 0x0f];
  }
  out[32] = '\0';
}

// ============================================================
//  SETUP HELPERS
// ============================================================
void initDefaultState() {
  const char* relayNames[3] = { "Relay 1", "Relay 2", "Relay 3" };
  for (int i = 0; i < 3; i++) {
    relays[i].id = i + 1;
    strlcpy(relays[i].name, relayNames[i], sizeof(relays[i].name));
    strlcpy(relays[i].mode, "auto", sizeof(relays[i].mode));
    relays[i].active = false;
    strlcpy(relays[i].countdown, "No active schedule", sizeof(relays[i].countdown));
  }
  const char* pumpNames[4] = { "Pump 1", "Pump 2", "Pump 3", "Pump 4" };
  for (int i = 0; i < 4; i++) {
    pumps[i].id = i + 1;
    strlcpy(pumps[i].name, pumpNames[i], sizeof(pumps[i].name));
    strlcpy(pumps[i].mode, "auto", sizeof(pumps[i].mode));
    pumps[i].active = false;
    pumps[i].speedRpm = 0;
    strlcpy(pumps[i].pumpType, "Pentair", sizeof(pumps[i].pumpType));
    strlcpy(pumps[i].countdown, "No active schedule", sizeof(pumps[i].countdown));
  }
  for (int i = 0; i < MAX_SCHEDULES; i++) schedules[i].inUse = false;
}

void deriveLocalName() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(bleLocalName, sizeof(bleLocalName), "PoolCtrl-%02X%02X%02X", mac[3], mac[4], mac[5]);

  // The full station MAC is the device's permanent identity. The app
  // keys its device registry on this, so a user-assigned name can be
  // changed freely without the device being mistaken for a new one.
  uint8_t sta[6];
  esp_read_mac(sta, ESP_MAC_WIFI_STA);
  snprintf(deviceMac, sizeof(deviceMac), "%02X:%02X:%02X:%02X:%02X:%02X",
           sta[0], sta[1], sta[2], sta[3], sta[4], sta[5]);
}

// ============================================================
//  WI-FI LIFECYCLE
// ============================================================


uint32_t wifiAnnounceUntilMs = 0;
uint32_t wifiLastAnnounceMs = 0;

// A one-chunk Wi-Fi status frame (~120 bytes — fits a single notify at any
// negotiated MTU above the default). Unlike the full state payload it
// cannot be torn by a dropped chunk, so it survives coexistence congestion.
void broadcastWifiEvent() {
  StaticJsonDocument<256> doc;
  doc["type"] = "wifi";
  // Identity travels WITH the event. The app must never guess which of
  // several paired controllers this report belongs to — guessing from its
  // "currently active device" mid-pairing filed one device's IP under
  // another's record and collapsed the registry to a single entry.
  doc["mac"] = deviceMac;
  doc["state"] = wifiPhaseStr();
  doc["ssid"] = wifiSsid;
  doc["joinFailed"] = wifiJoinFailed;
  if (wifiPhase == WIFI_ONLINE) doc["ip"] = WiFi.localIP().toString();
  String out;
  serializeJson(doc, out);

  // Logged unconditionally: this line on the serial monitor next to the
  // matching "[rx] wifi event" line in the app's Metro terminal is the
  // ground truth for whether the BLE report actually crossed the air.
  if (authedCount() == 0) {
    Serial.printf("[ble] wifi event %s — NO authenticated app session to notify\n", out.c_str());
    return;
  }
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (!sessions[i].inUse || !sessions[i].authed) continue;
    bool ok = sendChunkedTo(&sessions[i], out);
    Serial.printf("[ble] wifi event -> conn %u: %s  %s\n",
                  sessions[i].connHandle, ok ? "delivered" : "DROPPED", out.c_str());
  }
}

void startWifiAnnounce() {          // repeat for 20s so one loss is harmless
  wifiAnnounceUntilMs = millis() + 20000;
  wifiLastAnnounceMs = 0;           // fire on the next loop pass
}
void loadWifiCreds() {
  prefs.begin("poolctrl", true); // read-only
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  tzOffsetSec = prefs.getLong("tz", 0);
  prefs.end();

  strlcpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid));
  strlcpy(wifiPass, pass.c_str(), sizeof(wifiPass));
  wifiConfigured = strlen(wifiSsid) > 0;
  wifiPhase = wifiConfigured ? WIFI_OFFLINE : WIFI_UNCONFIGURED;
}

void saveWifiCreds() {
  prefs.begin("poolctrl", false);
  prefs.putString("ssid", wifiSsid);
  prefs.putString("pass", wifiPass);
  prefs.putLong("tz", tzOffsetSec);
  prefs.end();
}

void clearWifiCreds() {
  prefs.begin("poolctrl", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
  wifiSsid[0] = '\0';
  wifiPass[0] = '\0';
  wifiConfigured = false;
  wifiPhase = WIFI_UNCONFIGURED;
  WiFi.disconnect(true);
}

void beginWifiConnect() {
  if (!wifiConfigured) return;
  Serial.printf("[wifi] connecting to \"%s\"...\n", wifiSsid);
  WiFi.mode(WIFI_STA);
  // Modem sleep must stay ENABLED while BLE is up: the S3 has one 2.4GHz
  // radio and coexistence timeshares it inside Wi-Fi sleep windows. With
  // sleep forced off the join starves BLE (dropped notifies, lost link)
  // and the fresh association itself flaps — the app then hears "offline"
  // moments after serial printed "online". Costs a few ms of HTTP latency.
  WiFi.setSleep(true);
  WiFi.begin(wifiSsid, wifiPass);
  wifiJoinFailed = false;
  wifiPhase = WIFI_CONNECTING;
  wifiAttemptStartedMs = millis();
  // A verdict from a PREVIOUS attempt (or a forget) must stop repeating
  // now — its frames would race this attempt's outcome in the app.
  wifiAnnounceUntilMs = 0;
}

void syncTimeFromNtp() {
  // The system clock is kept in pure UTC and the app-supplied offset is
  // applied in localNow(). Doing it that way means the same code path
  // works whether the clock came from NTP or from the app over BLE.
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
  Serial.printf("[time] NTP requested (tz offset %lds)\n", tzOffsetSec);
}

// Local wall-clock time = UTC + the offset the app told us. Used by both
// the schedule evaluator and the state payload so they can never disagree.
void localNow(struct tm* out) {
  time_t t = time(nullptr) + tzOffsetSec;
  gmtime_r(&t, out);
}

// Drives the Wi-Fi state machine. Called from loop(), never from a
// BLE or HTTP callback.
void wifiTick() {
  if (!wifiConfigured) return;

  wl_status_t st = WiFi.status();

  if (wifiPhase == WIFI_CONNECTING) {
    if (st == WL_CONNECTED) {
      wifiPhase = WIFI_ONLINE;
      Serial.printf("[wifi] online as %s — announcing over BLE\n", WiFi.localIP().toString().c_str());
      syncTimeFromNtp();
      // Small single-notify "wifi connected" frames, repeated for 20s
      // (every 2s from loop()), so the app hears the verdict even if a
      // notify or two is lost to Wi-Fi/BLE radio contention. The app
      // switches its UI to "connected" on receiving one of these.
      startWifiAnnounce();
      broadcastState();   // full state still follows for the IP cache etc.
      return;
    }
    if (millis() - wifiAttemptStartedMs > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println("[wifi] join timed out — announcing over BLE");
      WiFi.disconnect();
      wifiJoinFailed = true;
      wifiPhase = WIFI_OFFLINE;
      wifiLastRetryMs = millis();
      startWifiAnnounce(); // the failure verdict must arrive as reliably as success
      broadcastState();
    }
    return;
  }

  if (wifiPhase == WIFI_ONLINE && st != WL_CONNECTED) {
    Serial.println("[wifi] connection lost");
    wifiPhase = WIFI_OFFLINE;
    wifiLastRetryMs = millis();
    timeSynced = false;
    broadcastState();     // app falls back to BLE on seeing this
    return;
  }

  // Offline with credentials: keep retrying quietly in the background so
  // the link comes back on its own once the router does. Each attempt is
  // up to 20s of scanning/associating on the shared radio — enough
  // contention to starve an active BLE link into a supervision timeout.
  // So while a phone is connected over BLE, back way off: the phone keeps
  // full control over Bluetooth in the meantime, and it can always
  // re-provision to force an immediate join.
  uint32_t retryInterval = activeCount() > 0 ? 120000UL : WIFI_RETRY_INTERVAL_MS;
  if (wifiPhase == WIFI_OFFLINE && millis() - wifiLastRetryMs > retryInterval) {
    wifiLastRetryMs = millis();
    beginWifiConnect();
  }

  if (wifiPhase == WIFI_ONLINE && !timeSynced) {
    time_t now = time(nullptr);
    if (now > 1700000000) { // sometime after Nov 2023 => NTP has answered
      timeSynced = true;
      Serial.printf("[time] synced: %s", ctime(&now));
      broadcastState();
    }
  }
}

void printQrPayload() {
  StaticJsonDocument<128> doc;
  doc["v"] = 2;
  doc["name"] = bleLocalName;
  doc["svc"] = SERVICE_UUID;
  String out;
  serializeJson(doc, out);
  Serial.println();
  Serial.println("=== SCAN THIS INTO A QR CODE ===");
  Serial.println(out);
  Serial.println("=================================");
  Serial.println("(The pairing secret is NOT in the QR — it is compiled");
  Serial.println(" into both this firmware and the app.)");
  Serial.println();
}

// ============================================================
//  RELAY / PUMP OUTPUT
// ============================================================
void applyRelayOutputs() {
  for (int i = 0; i < 3; i++) {
    bool level = relays[i].active ? RELAY_ACTIVE_HIGH : !RELAY_ACTIVE_HIGH;
    digitalWrite(RELAY_PINS[i], level ? HIGH : LOW);
  }
}

// Placeholder: Pentair/Emaux variable-speed pumps are typically driven over
// RS-485 (e.g. Pentair's proprietary protocol, or Modbus for some Emaux
// models), not a simple GPIO. Wire your RS-485 transceiver and fill this in;
// for now it just gates a digital "enable" pin per pump so the rest of the
// system (state sync, scheduling) is fully working out of the box.
void runPumpDriver(Pump& p) {
  digitalWrite(PUMP_ENABLE_PINS[p.id - 1], p.active ? HIGH : LOW);
  // TODO: send p.speedRpm to the pump over RS-485 using p.pumpType's protocol.
}

void applyPumpOutputs() {
  for (int i = 0; i < 4; i++) runPumpDriver(pumps[i]);
}

// ============================================================
//  JSON STATE SERIALIZATION
// ============================================================
String buildStateJson() {
  DynamicJsonDocument doc(3072);
  doc["type"] = "state";

  // Stable identity + transport info. The app keys its device registry
  // on `mac` and caches `wifi.ip` to reach us over the LAN; it refreshes
  // that cache from here on every connection, which is what lets a
  // DHCP-reassigned address heal itself over BLE.
  doc["mac"] = deviceMac;
  doc["name"] = bleLocalName;

  JsonObject w = doc.createNestedObject("wifi");
  w["state"] = wifiPhaseStr();
  w["ssid"] = wifiSsid;
  w["joinFailed"] = wifiJoinFailed;
  if (wifiPhase == WIFI_ONLINE) {
    w["ip"] = WiFi.localIP().toString();
    w["rssi"] = WiFi.RSSI();
  }
  doc["timeSynced"] = timeSynced;

  JsonArray relayArr = doc.createNestedArray("relays");
  for (int i = 0; i < 3; i++) {
    JsonObject r = relayArr.createNestedObject();
    r["id"] = relays[i].id;
    r["name"] = relays[i].name;
    r["mode"] = relays[i].mode;
    r["active"] = relays[i].active;
    r["countdown"] = relays[i].countdown;
  }

  JsonArray pumpArr = doc.createNestedArray("pumps");
  for (int i = 0; i < 4; i++) {
    JsonObject p = pumpArr.createNestedObject();
    p["id"] = pumps[i].id;
    p["name"] = pumps[i].name;
    p["mode"] = pumps[i].mode;
    p["active"] = pumps[i].active;
    p["speedRpm"] = pumps[i].speedRpm;
    p["pumpType"] = pumps[i].pumpType;
    p["countdown"] = pumps[i].countdown;
  }

  JsonArray schedArr = doc.createNestedArray("schedules");
  for (int i = 0; i < MAX_SCHEDULES; i++) {
    if (!schedules[i].inUse) continue;
    JsonObject s = schedArr.createNestedObject();
    s["id"] = schedules[i].id;
    s["target"] = schedules[i].target;
    s["hour24"] = schedules[i].hour24;
    s["minute"] = schedules[i].minute;
    s["durationMin"] = schedules[i].durationMin;
    s["enabled"] = schedules[i].enabled;
  }

  struct tm t;
  localNow(&t);
  char timeStr[9];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  doc["time"] = timeStr;

  String out;
  serializeJson(doc, out);
  return out;
}

// ============================================================
//  BLE SEND (chunked to fit the negotiated MTU, addressed per
//  connection so one phone's traffic never leaks to another)
// ============================================================
// Returns true only when every chunk was accepted by the stack — callers
// that need proof of delivery (the wifi announce) log the verdict.
bool sendChunkedTo(Session* s, const String& payload) {
  if (!s || !s->inUse || pTxCharacteristic == nullptr) return false;

  // One sender at a time. Chunked messages from the NimBLE host task
  // (acks, getState responses) and from the Arduino loop task
  // (heartbeats, wifi broadcasts) otherwise interleave their chunks on
  // the same characteristic — the phone reassembles a hybrid of two
  // messages and BOTH fail to parse. This was invisible on serial:
  // every notify succeeds; the corruption is ordering, not loss.
  if (txMutex && xSemaphoreTake(txMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    Serial.printf("[ble] tx mutex timeout — message skipped for conn %u\n", s->connHandle);
    return false;
  }

  String withDelim = payload + "\n";
  size_t chunkSize = (s->mtu > 3) ? (s->mtu - 3) : 20;
  size_t len = withDelim.length();
  bool ok = true;

  for (size_t offset = 0; ok && offset < len; offset += chunkSize) {
    size_t thisLen = min(chunkSize, len - offset);
    pTxCharacteristic->setValue((uint8_t*)(withDelim.c_str() + offset), thisLen);

    bool sent = false;
    for (int t = 0; t < 5 && !sent; t++) {
      sent = pTxCharacteristic->notify(s->connHandle);
      if (!sent) delay(15);
    }
    if (!sent) {
      Serial.printf("[ble] notify dropped on conn %u — message abandoned\n", s->connHandle);
      ok = false;
    }
    if (ok) delay(10);
  }

  if (txMutex) xSemaphoreGive(txMutex);
  return ok;
}

// Pushes to every AUTHENTICATED session. An unauthenticated session
// never receives device state — only its own challenge.
void broadcastState() {
  if (authedCount() == 0) return;
  String state = buildStateJson();
  for (int i = 0; i < MAX_SESSIONS; i++)
    if (sessions[i].inUse && sessions[i].authed) sendChunkedTo(&sessions[i], state);
}

void sendAckTo(Session* s, const char* cmd, bool ok, const char* message = "") {
  StaticJsonDocument<160> doc;
  doc["type"] = "ack";
  doc["cmd"] = cmd;
  doc["ok"] = ok;
  if (strlen(message) > 0) doc["message"] = message;
  String out;
  serializeJson(doc, out);
  sendChunkedTo(s, out);
}

// ============================================================
//  AUTHENTICATION
// ============================================================
void sendChallenge(Session* s) {
  makeNonceHex(s->nonceHex);
  s->nonceIssued = true;

  StaticJsonDocument<128> doc;
  doc["type"] = "challenge";
  doc["nonce"] = s->nonceHex;
  String out;
  serializeJson(doc, out);
  sendChunkedTo(s, out);

  Serial.printf("[auth] conn %u: challenge issued\n", s->connHandle);
}

void dropSession(Session* s, const char* why) {
  if (!s) return;
  Serial.printf("[auth] conn %u dropped: %s\n", s->connHandle, why);
  uint16_t h = s->connHandle;
  freeSession(h);
  if (pServer) pServer->disconnect(h);
}

// Returns true if the session is allowed to run `cmd`.
bool handleAuthCommand(Session* s, const char* cmd, JsonDocument& doc) {
  if (strcmp(cmd, "hello") == 0) {
    // Sent by the app once it has subscribed to TX notifications.
    // Issuing the challenge here (rather than on connect) removes the
    // race where a notify fires before the CCCD subscribe lands.
    sendChallenge(s);
    return true;
  }

  if (strcmp(cmd, "auth") == 0) {
    if (!s->nonceIssued) {
      sendAckTo(s, "auth", false, "no challenge issued");
      dropSession(s, "auth before hello");
      return true;
    }

    const char* given = doc["hmac"] | "";
    char expected[65];
    hmacSha256Hex(DEVICE_SECRET, s->nonceHex, expected);

    if (strlen(given) != 64 || !constantTimeEquals(given, expected, 64)) {
      sendAckTo(s, "auth", false, "bad credentials");
      // Hang up rather than stall here: this callback runs on the NimBLE
      // host task, so a sleep would freeze the other sessions too. Forcing
      // a full reconnect per guess is rate limit enough against a search
      // space no one is going to walk anyway.
      dropSession(s, "HMAC mismatch");
      return true;
    }

    // Burn the nonce so a replayed auth frame can't reuse it.
    s->nonceIssued = false;
    s->authed = true;
    Serial.printf("[auth] conn %u AUTHENTICATED\n", s->connHandle);

    // A phone just connected — if stored Wi-Fi credentials exist but the
    // link is down, try them NOW instead of waiting out the background
    // retry backoff. The app skips its credentials popup and waits for
    // this attempt's verdict (online / joinFailed), announced within
    // seconds. Clearing the stale verdict BEFORE the state push below is
    // what tells the app "a fresh attempt is pending — don't pop the
    // dialog over last time's failure".
    if (wifiConfigured && wifiPhase == WIFI_OFFLINE) {
      Serial.println("[wifi] app connected — trying stored network now");
      wifiJoinFailed = false;
      wifiJoinRequested = true; // serviced from loop(), never from this callback
    }

    // The success ack carries the device identity. It is a single-notify
    // frame that demonstrably survives (auth itself completed over it),
    // unlike the multi-chunk state push — so the app can register this
    // device the moment the handshake finishes instead of betting its
    // registry on the big payload arriving intact.
    {
      StaticJsonDocument<192> ok;
      ok["type"] = "ack";
      ok["cmd"] = "auth";
      ok["ok"] = true;
      ok["mac"] = deviceMac;
      ok["name"] = bleLocalName;
      String out;
      serializeJson(ok, out);
      sendChunkedTo(s, out);
    }

    // Tell the app the CURRENT Wi-Fi status RIGHT NOW, in one small frame.
    // Its add-device flow decides "skip the picker or show it" on this
    // single fact — and waiting on the announce loop stalled that by
    // 10-15s, because a join queued above cancels the announce window on
    // its very first loop() pass, before one frame ever fires. Sent
    // before the (slow, multi-chunk) state push on purpose.
    broadcastWifiEvent();
    // Keep repeating it for 20s as insurance against the one-shot frame
    // being lost. (A join kicked above still cancels the REPEATS and
    // re-announces its own verdict instead — the immediate frame above
    // has already gone out by then.)
    startWifiAnnounce();

    sendChunkedTo(s, buildStateJson());
    return true;
  }

  return false; // not an auth-phase command
}

// ============================================================
//  COMMAND HANDLING (messages from the app)
// ============================================================
Relay* findRelay(int id) {
  for (int i = 0; i < 3; i++) if (relays[i].id == id) return &relays[i];
  return nullptr;
}
Pump* findPump(int id) {
  for (int i = 0; i < 4; i++) if (pumps[i].id == id) return &pumps[i];
  return nullptr;
}
Schedule* findSchedule(const char* id) {
  for (int i = 0; i < MAX_SCHEDULES; i++)
    if (schedules[i].inUse && strcmp(schedules[i].id, id) == 0) return &schedules[i];
  return nullptr;
}
Schedule* freeScheduleSlot() {
  for (int i = 0; i < MAX_SCHEDULES; i++) if (!schedules[i].inUse) return &schedules[i];
  return nullptr;
}

void persistSchedules() {
  // Simple NVS persistence so timers survive a reboot.
  prefs.begin("poolctrl", false);
  String out = buildStateJson();
  prefs.putString("state", out);
  prefs.end();
}

// ------------------------------------------------------------
//  TRANSPORT-AGNOSTIC COMMAND EXECUTION
// ------------------------------------------------------------
// Applies a relay/pump/schedule command and returns the ack JSON.
// Shared verbatim by the BLE and HTTP paths so the two transports can
// never drift apart in behaviour. `changed` is set when callers should
// push fresh state to everyone.
String buildAck(const char* cmd, bool ok, const char* message = "") {
  StaticJsonDocument<160> doc;
  doc["type"] = "ack";
  doc["cmd"] = cmd;
  doc["ok"] = ok;
  if (strlen(message) > 0) doc["message"] = message;
  String out;
  serializeJson(doc, out);
  return out;
}

String executeDeviceCommand(JsonDocument& doc, bool& changed) {
  const char* cmd = doc["cmd"] | "";
  changed = false;

  // The app pushes its own clock on every connect. This is what keeps
  // schedules working on a BLE-only device that has no NTP, and it also
  // gets the clock right immediately rather than waiting for the first
  // NTP packet after a Wi-Fi join.
  if (strcmp(cmd, "setTime") == 0) {
    long epoch = doc["epoch"] | 0L;
    if (epoch < 1700000000L) return buildAck(cmd, false, "implausible timestamp");
    if (doc.containsKey("tzOffsetSec")) {
      long tz = doc["tzOffsetSec"] | 0L;
      if (tz != tzOffsetSec) { tzOffsetSec = tz; saveWifiCreds(); }
    }
    struct timeval tv;
    tv.tv_sec = (time_t)epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    timeSynced = true;
    changed = true;
    return buildAck(cmd, true);
  }

  if (strcmp(cmd, "setRelayMode") == 0) {
    Relay* r = findRelay(doc["id"] | -1);
    const char* mode = doc["mode"] | "auto";
    if (!r) return buildAck(cmd, false, "no such relay");
    strlcpy(r->mode, mode, sizeof(r->mode));
    if (strcmp(mode, "on") == 0) r->active = true;
    else if (strcmp(mode, "off") == 0) r->active = false;
    applyRelayOutputs();
    Serial.printf("[cmd] relay %u (\"%s\") mode -> %s, output %s\n",
                  r->id, r->name, r->mode, r->active ? "ON" : "OFF");
    changed = true;
    return buildAck(cmd, true);
  }

  if (strcmp(cmd, "setRelayName") == 0) {
    Relay* r = findRelay(doc["id"] | -1);
    if (!r) return buildAck(cmd, false, "no such relay");
    strlcpy(r->name, (const char*)(doc["name"] | "Relay"), sizeof(r->name));
    Serial.printf("[cmd] relay %u renamed to \"%s\"\n", r->id, r->name);
    changed = true;
    return buildAck(cmd, true);
  }

  if (strcmp(cmd, "setPumpMode") == 0) {
    Pump* p = findPump(doc["id"] | -1);
    const char* mode = doc["mode"] | "auto";
    if (!p) return buildAck(cmd, false, "no such pump");
    strlcpy(p->mode, mode, sizeof(p->mode));
    if (strcmp(mode, "on") == 0) p->active = true;
    else if (strcmp(mode, "off") == 0) p->active = false;
    applyPumpOutputs();
    Serial.printf("[cmd] pump %u (\"%s\") mode -> %s, output %s\n",
                  p->id, p->name, p->mode, p->active ? "ON" : "OFF");
    changed = true;
    return buildAck(cmd, true);
  }

  if (strcmp(cmd, "setPumpConfig") == 0) {
    Pump* p = findPump(doc["id"] | -1);
    if (!p) return buildAck(cmd, false, "no such pump");
    if (doc.containsKey("pumpType")) strlcpy(p->pumpType, doc["pumpType"] | "Pentair", sizeof(p->pumpType));
    if (doc.containsKey("speedRpm")) p->speedRpm = doc["speedRpm"] | 0;
    if (doc.containsKey("name")) strlcpy(p->name, doc["name"] | p->name, sizeof(p->name));
    Serial.printf("[cmd] pump %u config: name=\"%s\" type=%s speed=%urpm\n",
                  p->id, p->name, p->pumpType, p->speedRpm);
    changed = true;
    return buildAck(cmd, true);
  }

  if (strcmp(cmd, "addSchedule") == 0 || strcmp(cmd, "updateSchedule") == 0) {
    Schedule* sch = nullptr;
    bool isNew = strcmp(cmd, "addSchedule") == 0;
    if (isNew) {
      sch = freeScheduleSlot();
      if (!sch) return buildAck(cmd, false, "schedule list full (8 max)");
      snprintf(sch->id, sizeof(sch->id), "p%lu", (unsigned long)(millis() % 100000));
      sch->inUse = true;
    } else {
      sch = findSchedule(doc["id"] | "");
      if (!sch) return buildAck(cmd, false, "no such schedule");
    }
    JsonObject sched = doc["schedule"];
    strlcpy(sch->target, sched["target"] | "Relay 1", sizeof(sch->target));
    sch->hour24 = sched["hour24"] | 6;
    sch->minute = sched["minute"] | 0;
    sch->durationMin = sched["durationMin"] | 60;
    sch->enabled = sched["enabled"] | true;
    persistSchedules();
    Serial.printf("[cmd] %s %s: %s at %02u:%02u for %umin, %s\n",
                  isNew ? "schedule added" : "schedule updated", sch->id, sch->target,
                  sch->hour24, sch->minute, sch->durationMin,
                  sch->enabled ? "enabled" : "disabled");
    changed = true;
    return buildAck(cmd, true);
  }

  if (strcmp(cmd, "deleteSchedule") == 0) {
    Schedule* sch = findSchedule(doc["id"] | "");
    if (!sch) return buildAck(cmd, false, "no such schedule");
    sch->inUse = false;
    persistSchedules();
    Serial.printf("[cmd] schedule deleted %s (%s)\n", sch->id, sch->target);
    changed = true;
    return buildAck(cmd, true);
  }

  if (strcmp(cmd, "toggleSchedule") == 0) {
    Schedule* sch = findSchedule(doc["id"] | "");
    if (!sch) return buildAck(cmd, false, "no such schedule");
    sch->enabled = doc["enabled"] | sch->enabled;
    persistSchedules();
    Serial.printf("[cmd] schedule %s (%s) %s\n", sch->id, sch->target,
                  sch->enabled ? "enabled" : "disabled");
    changed = true;
    return buildAck(cmd, true);
  }

  return buildAck(cmd, false, "unrecognized command");
}

void handleCommand(Session* s, const String& msg) {
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    sendAckTo(s, "unknown", false, "bad json");
    return;
  }

  const char* cmd = doc["cmd"] | "";

  // ---- auth gate ----------------------------------------------------
  // "hello" and "auth" are the only things an unauthenticated session
  // may say. Anything else means we're talking to something that isn't
  // our app, so hang up rather than answer.
  if (handleAuthCommand(s, cmd, doc)) return;

  if (!s->authed) {
    sendAckTo(s, cmd, false, "unauthorized");
    dropSession(s, "command before auth");
    return;
  }
  // -------------------------------------------------------------------

  if (strcmp(cmd, "getState") == 0) {
    sendChunkedTo(s, buildStateJson());
    return;
  }

  // ---- Wi-Fi provisioning -------------------------------------------
  if (strcmp(cmd, "scanWifi") == 0) {
    // Queued, not run here: WiFi.scanNetworks() blocks for seconds and
    // this callback runs on the NimBLE host task.
    if (wifiScanRequested) { sendAckTo(s, cmd, false, "scan already in progress"); return; }
    wifiScanRequested = true;
    wifiScanRequesterConn = s->connHandle;
    sendAckTo(s, cmd, true, "scanning");
    return;
  }

  if (strcmp(cmd, "setWifi") == 0) {
    const char* ssid = doc["ssid"] | "";
    const char* passEnc = doc["passEnc"] | "";   // keystream-encrypted, hex
    const char* passPlain = doc["pass"] | "";    // only honoured if passEnc absent

    if (strlen(ssid) == 0) { sendAckTo(s, cmd, false, "missing ssid"); return; }

    char pass[65] = "";
    if (strlen(passEnc) > 0) {
      // Decrypted against the nonce from THIS session's handshake, so a
      // captured frame is useless in any later session.
      if (!keystreamDecryptHex(s->nonceHex, passEnc, pass, sizeof(pass))) {
        sendAckTo(s, cmd, false, "could not decrypt credentials");
        return;
      }
    } else {
      strlcpy(pass, passPlain, sizeof(pass));
    }

    strlcpy(wifiSsid, ssid, sizeof(wifiSsid));
    strlcpy(wifiPass, pass, sizeof(wifiPass));
    if (doc.containsKey("tzOffsetSec")) tzOffsetSec = doc["tzOffsetSec"] | 0L;
    wifiConfigured = true;
    saveWifiCreds();

    // Join from loop() for the same reason as the scan.
    wifiJoinRequested = true;
    wifiJoinRequesterConn = s->connHandle;
    sendAckTo(s, cmd, true, "connecting");
    return;
  }

  if (strcmp(cmd, "forgetWifi") == 0) {
    clearWifiCreds();
    Serial.println("[cmd] forgetWifi: credentials erased, Wi-Fi off — announcing over BLE");
    sendAckTo(s, cmd, true);
    // Same reliable single-notify announce as a join verdict, so the app's
    // UI flips to "not configured" even if the full state push is lost.
    startWifiAnnounce();
    broadcastState();
    return;
  }

  if (strcmp(cmd, "setTimezone") == 0) {
    tzOffsetSec = doc["tzOffsetSec"] | 0L;
    saveWifiCreds();
    if (wifiPhase == WIFI_ONLINE) syncTimeFromNtp();
    sendAckTo(s, cmd, true);
    return;
  }

  // Relay / pump / schedule commands are identical whichever transport
  // they arrived on, so they live in one shared implementation.
  bool changed = false;
  String ack = executeDeviceCommand(doc, changed);
  sendChunkedTo(s, ack);
  if (changed) broadcastState();
}

// ============================================================
//  SCHEDULE EVALUATION  (simple: activate target for durationMin
//  starting at hour24:minute; runs once per day)
// ============================================================
void evaluateSchedules() {
  struct tm t;
  localNow(&t);
  int nowMinutes = t.tm_hour * 60 + t.tm_min;

  for (int i = 0; i < MAX_SCHEDULES; i++) {
    Schedule& s = schedules[i];
    if (!s.inUse || !s.enabled) continue;

    int startMin = s.hour24 * 60 + s.minute;
    int endMin = startMin + s.durationMin;
    bool shouldBeActive = (nowMinutes >= startMin && nowMinutes < endMin);

    // Only affects targets that are in "auto" mode — manual on/off always wins.
    if (strncmp(s.target, "Relay", 5) == 0) {
      int id = s.target[6] - '0';
      Relay* r = findRelay(id);
      if (r && strcmp(r->mode, "auto") == 0 && r->active != shouldBeActive) {
        r->active = shouldBeActive;
        applyRelayOutputs();
        Serial.printf("[sched] %s: relay %u (\"%s\") -> %s\n",
                      s.id, r->id, r->name, r->active ? "ON" : "OFF");
        broadcastState();
      }
    } else if (strncmp(s.target, "Pump", 4) == 0) {
      int id = s.target[5] - '0';
      Pump* p = findPump(id);
      if (p && strcmp(p->mode, "auto") == 0 && p->active != shouldBeActive) {
        p->active = shouldBeActive;
        applyPumpOutputs();
        Serial.printf("[sched] %s: pump %u (\"%s\") -> %s\n",
                      s.id, p->id, p->name, p->active ? "ON" : "OFF");
        broadcastState();
      }
    }
  }
}

// ============================================================
//  DEFERRED WI-FI WORK  (called from loop(), never from a callback)
// ============================================================
void serviceWifiScan() {
  if (!wifiScanRequested) return;
  wifiScanRequested = false;

  Session* s = sessionByHandle(wifiScanRequesterConn);
  if (!s || !s->authed) return; // requester went away

  Serial.println("[wifi] scanning...");
  // Scanning needs the station interface up even when we're not joined.
  if (wifiPhase == WIFI_UNCONFIGURED) WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  Serial.printf("[wifi] %d networks found\n", n);

  DynamicJsonDocument doc(2048);
  doc["type"] = "wifiScan";
  JsonArray arr = doc.createNestedArray("networks");
  // Cap the list: it has to fit the document and a long list is useless
  // in a picker anyway. Strongest signal first.
  int limit = n > 20 ? 20 : n;
  for (int i = 0; i < limit; i++) {
    JsonObject net = arr.createNestedObject();
    net["ssid"] = WiFi.SSID(i);
    net["rssi"] = WiFi.RSSI(i);
    net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  if (n > limit) doc["truncated"] = n - limit;
  WiFi.scanDelete();

  String out;
  serializeJson(doc, out);
  sendChunkedTo(s, out);
}

void serviceWifiJoin() {
  if (!wifiJoinRequested) return;
  wifiJoinRequested = false;
  beginWifiConnect();
}

// ============================================================
//  HTTP TRANSPORT
// ============================================================
// Returns bool rather than HttpSession* on purpose: the Arduino .ino
// preprocessor hoists generated prototypes above the struct definitions,
// so a function returning a locally-declared type fails to compile.
bool httpTokenValid(const char* token) {
  if (!token || strlen(token) != 32) return false;
  uint32_t now = millis();
  for (int i = 0; i < MAX_HTTP_SESSIONS; i++) {
    if (!httpSessions[i].inUse) continue;
    if (now - httpSessions[i].issuedAtMs > HTTP_TOKEN_TTL_MS) { // expired
      httpSessions[i].inUse = false;
      continue;
    }
    if (constantTimeEquals(httpSessions[i].token, token, 32)) return true;
  }
  return false;
}

// Returns the new token, or nullptr if every slot is busy. Oldest slot
// is recycled so a client that never logs out can't lock everyone out.
const char* httpIssueToken() {
  int slot = -1;
  for (int i = 0; i < MAX_HTTP_SESSIONS; i++) {
    if (!httpSessions[i].inUse) { slot = i; break; }
  }
  if (slot < 0) {
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < MAX_HTTP_SESSIONS; i++) {
      if (httpSessions[i].issuedAtMs < oldest) { oldest = httpSessions[i].issuedAtMs; slot = i; }
    }
  }
  makeNonceHex(httpSessions[slot].token);
  httpSessions[slot].inUse = true;
  httpSessions[slot].issuedAtMs = millis();
  return httpSessions[slot].token;
}

// Pulls the bearer token out of the Authorization header.
bool httpAuthorized() {
  if (!httpServer.hasHeader("Authorization")) return false;
  String h = httpServer.header("Authorization");
  if (!h.startsWith("Bearer ")) return false;
  return httpTokenValid(h.substring(7).c_str());
}

void httpSendJson(int code, const String& body) {
  httpServer.sendHeader("Cache-Control", "no-store");
  httpServer.send(code, "application/json", body);
}

void handleHttpChallenge() {
  makeNonceHex(httpNonce);
  httpNonceIssued = true;
  StaticJsonDocument<128> doc;
  doc["type"] = "challenge";
  doc["nonce"] = httpNonce;
  doc["mac"] = deviceMac;
  String out;
  serializeJson(doc, out);
  httpSendJson(200, out);
}

void handleHttpAuth() {
  if (!httpNonceIssued) { httpSendJson(403, buildAck("auth", false, "no challenge issued")); return; }

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, httpServer.arg("plain"))) {
    httpSendJson(400, buildAck("auth", false, "bad json"));
    return;
  }

  const char* given = doc["hmac"] | "";
  char expected[65];
  hmacSha256Hex(DEVICE_SECRET, httpNonce, expected);

  if (strlen(given) != 64 || !constantTimeEquals(given, expected, 64)) {
    // Burn the nonce on a failed attempt too, so a guess can't be
    // retried against the same challenge.
    httpNonceIssued = false;
    httpSendJson(403, buildAck("auth", false, "bad credentials"));
    return;
  }

  httpNonceIssued = false; // single use
  const char* token = httpIssueToken();

  StaticJsonDocument<160> res;
  res["type"] = "ack";
  res["cmd"] = "auth";
  res["ok"] = true;
  res["token"] = token;
  res["ttlMs"] = HTTP_TOKEN_TTL_MS;
  String out;
  serializeJson(res, out);
  httpSendJson(200, out);
  Serial.println("[http] client authenticated");
}

void handleHttpState() {
  if (!httpAuthorized()) { httpSendJson(401, buildAck("getState", false, "unauthorized")); return; }
  httpSendJson(200, buildStateJson());
}

void handleHttpCmd() {
  if (!httpAuthorized()) { httpSendJson(401, buildAck("cmd", false, "unauthorized")); return; }

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, httpServer.arg("plain"))) {
    httpSendJson(400, buildAck("unknown", false, "bad json"));
    return;
  }

  const char* cmd = doc["cmd"] | "";
  if (strcmp(cmd, "getState") == 0) { httpSendJson(200, buildStateJson()); return; }

  // Wi-Fi provisioning stays BLE-only on purpose: re-pointing the device
  // at a different network over the network you're about to leave is a
  // reliable way to strand it.
  if (strcmp(cmd, "setWifi") == 0 || strcmp(cmd, "scanWifi") == 0 || strcmp(cmd, "forgetWifi") == 0) {
    httpSendJson(400, buildAck(cmd, false, "Wi-Fi setup is only available over Bluetooth"));
    return;
  }

  bool changed = false;
  String ack = executeDeviceCommand(doc, changed);
  if (changed) broadcastState(); // keep any BLE-connected app in sync too
  httpSendJson(200, ack);
}

void startHttpServer() {
  if (httpServerStarted) return;
  httpServer.on("/challenge", HTTP_GET, handleHttpChallenge);
  httpServer.on("/auth", HTTP_POST, handleHttpAuth);
  httpServer.on("/state", HTTP_GET, handleHttpState);
  httpServer.on("/cmd", HTTP_POST, handleHttpCmd);
  httpServer.onNotFound([]() { httpSendJson(404, buildAck("unknown", false, "no such endpoint")); });

  const char* headers[] = { "Authorization" };
  httpServer.collectHeaders(headers, 1);

  httpServer.begin();
  httpServerStarted = true;
  Serial.printf("[http] listening on port %d\n", HTTP_PORT);
}

// ============================================================
//  BLE CALLBACKS
// ============================================================
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    uint16_t h = info.getConnHandle();

    Session* s = allocSession(h);
    if (!s) {
      // Already serving MAX_SESSIONS phones — refuse politely and at once.
      Serial.printf("Connection %u refused: %d/%d sessions in use\n", h, activeCount(), MAX_SESSIONS);
      server->disconnect(h);
      return;
    }

    Serial.printf("Central connected (conn %u), awaiting auth. %d/%d slots used.\n",
                  h, activeCount(), MAX_SESSIONS);

    // Keep advertising so the remaining slots are still reachable.
    // NimBLE stops advertising on connect by default.
    if (activeCount() < MAX_SESSIONS) NimBLEDevice::startAdvertising();
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
    uint16_t h = info.getConnHandle();
    freeSession(h);
    Serial.printf("Central disconnected (conn %u, reason %d). %d/%d slots used.\n",
                  h, reason, activeCount(), MAX_SESSIONS);
    NimBLEDevice::startAdvertising();
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    Session* s = sessionByHandle(info.getConnHandle());
    if (s) s->mtu = mtu;
    Serial.printf("MTU negotiated on conn %u: %u\n", info.getConnHandle(), mtu);
  }

#if REQUIRE_LINK_ENCRYPTION
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("Link security on conn %u: encrypted=%d bonded=%d\n",
                  info.getConnHandle(), info.isEncrypted(), info.isBonded());
    if (!info.isEncrypted()) {
      Serial.println("Encryption failed — dropping connection.");
      if (pServer) pServer->disconnect(info.getConnHandle());
    }
  }
#endif
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    Session* s = sessionByHandle(info.getConnHandle());
    if (!s) return; // unknown/refused connection — ignore entirely

    s->rxBuffer += c->getValue().c_str();

    // A client that never sends a newline could otherwise grow this
    // buffer until the heap gives out.
    if (s->rxBuffer.length() > 2048) {
      dropSession(s, "rx buffer overflow");
      return;
    }

    int nl;
    while ((nl = s->rxBuffer.indexOf('\n')) >= 0) {
      String line = s->rxBuffer.substring(0, nl);
      s->rxBuffer = s->rxBuffer.substring(nl + 1);
      if (line.length() > 0) {
        handleCommand(s, line);
        if (!s->inUse) return; // handleCommand dropped us
      }
    }
  }
};

// ============================================================
//  SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  for (int i = 0; i < 3; i++) pinMode(RELAY_PINS[i], OUTPUT);
  for (int i = 0; i < 4; i++) pinMode(PUMP_ENABLE_PINS[i], OUTPUT);

  initDefaultState();
  deriveLocalName();
  selfTestHmac();

  // Stored credentials, if any, are joined from loop() so setup() never
  // blocks on a router that isn't there.
  loadWifiCreds();
  if (wifiConfigured) {
    Serial.printf("[wifi] stored network: \"%s\"\n", wifiSsid);
    beginWifiConnect();
  } else {
    Serial.println("[wifi] no credentials stored — provision over BLE");
  }

  txMutex = xSemaphoreCreateMutex();

  NimBLEDevice::init(bleLocalName);
  NimBLEDevice::setMTU(517); // request the max; actual value settles via onMTUChange

  // ---- Link-layer security -------------------------------------------
  // "Just Works" pairing: bonding on, MITM protection off, LE Secure
  // Connections on, and no input/output capability. There is no passkey
  // to display or type, so no phone-side prompt to fill in — which is
  // exactly why this alone cannot keep other apps out, and why the HMAC
  // challenge in handleAuthCommand() is the real access control.
  NimBLEDevice::setSecurityAuth(true /* bonding */, false /* MITM */, true /* SC */);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService* pService = pServer->createService(SERVICE_UUID);

#if REQUIRE_LINK_ENCRYPTION
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_TX,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC
  );
  pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_RX,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC
  );
#else
  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_TX,
    NIMBLE_PROPERTY::NOTIFY
  );
  pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_RX,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
#endif
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pService->start();

  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();

  // The 31-byte advertising packet cannot hold flags (3B) + a 128-bit
  // service UUID (18B) + the name (17B) at once — 38 bytes. Packed
  // implicitly, NimBLE refuses the payload and start() returns false,
  // which looked like "uploads fine, serial fine, invisible to every
  // scanner". Split it the way BLE intends: UUID in the advertisement
  // (what the app's scan filter matches), name in the scan response
  // (what scanner apps display).
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(NimBLEUUID(SERVICE_UUID));
  pAdvertising->setAdvertisementData(advData);

  NimBLEAdvertisementData scanData;
  scanData.setName(bleLocalName);
  pAdvertising->setScanResponseData(scanData);
  pAdvertising->enableScanResponse(true);

  if (pAdvertising->start()) {
    Serial.printf("BLE advertising as \"%s\"\n", bleLocalName);
  } else {
    Serial.println("!!! BLE advertising FAILED to start — device is invisible");
  }
  Serial.printf("Device MAC (identity): %s\n", deviceMac);
  Serial.printf("Auth: HMAC-SHA256 challenge required. Link encryption: %s. Max sessions: %d.\n",
                REQUIRE_LINK_ENCRYPTION ? "required" : "off", MAX_SESSIONS);
  Serial.printf("Stored bonds: %d\n", NimBLEDevice::getNumBonds());
  printQrPayload();
}

unsigned long lastScheduleCheck = 0;
unsigned long lastHeartbeat = 0;

// Hangs up on any connection that has sat there without completing the
// HMAC challenge. This is what stops a generic BLE tool from simply
// connecting and camping on a session slot.
void enforceAuthTimeouts() {
  uint32_t nowMs = millis();
  for (int i = 0; i < MAX_SESSIONS; i++) {
    Session* s = &sessions[i];
    if (!s->inUse || s->authed) continue;
    if (nowMs - s->connectedAtMs > AUTH_TIMEOUT_MS) dropSession(s, "auth timeout");
  }
}

void loop() {
  unsigned long nowMs = millis();

  enforceAuthTimeouts();

  // ---- Wi-Fi / HTTP -------------------------------------------------
  wifiTick();
  serviceWifiJoin();
  serviceWifiScan();
  
  if (millis() < wifiAnnounceUntilMs && millis() - wifiLastAnnounceMs > 2000) {
    wifiLastAnnounceMs = millis();
    broadcastWifiEvent();
  }
  if (wifiPhase == WIFI_ONLINE) {
    startHttpServer();          // idempotent
    httpServer.handleClient();
  }

  // Schedules are only meaningful once the clock is real. Before the
  // first NTP sync the system time sits near the epoch, so running the
  // evaluator would switch everything off at "00:00 1 Jan 1970".
  if (timeSynced && nowMs - lastScheduleCheck > 1000) {
    lastScheduleCheck = nowMs;
    evaluateSchedules();
  }

  // Heartbeat state push every 5s so authenticated apps stay in sync
  // even if an ack/notify got dropped.
  if (authedCount() > 0 && nowMs - lastHeartbeat > 5000) {
    lastHeartbeat = nowMs;
    broadcastState();
  }

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'q') {
      printQrPayload();
    } else if (c == 's') {
      Serial.printf("Sessions %d/%d:\n", activeCount(), MAX_SESSIONS);
      for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!sessions[i].inUse) continue;
        Serial.printf("  [%d] conn=%u authed=%s mtu=%u age=%lums\n", i, sessions[i].connHandle,
                      sessions[i].authed ? "yes" : "NO", sessions[i].mtu,
                      (unsigned long)(millis() - sessions[i].connectedAtMs));
      }
    } else if (c == 'b') {
      NimBLEDevice::deleteAllBonds();
      Serial.println("All BLE bonds erased.");
    }
  }
}
