#pragma once
/*
 * app_link.h
 * ---------------------------------------------------------------
 * Phone-app connectivity for the relay timer, merged in from the
 * standalone relay_timer_connection.ino prototype and bridged onto
 * this project's real state model (shared_state.h), DS3231 clock
 * (rtc_clock.h) and RS485 pump drivers (rs485_pumps.h).
 *
 * Provides:
 *   - BLE GATT service (Nordic-UART style) with an HMAC-SHA256
 *     challenge/response gate: only the app, which compiles in
 *     DEVICE_SECRET, can control the device. Newline-delimited JSON,
 *     chunked to the negotiated MTU.
 *   - Wi-Fi STA provisioning over BLE (scan / setWifi / forgetWifi).
 *     Credentials are keystream-encrypted against the session nonce.
 *     THIS is the only Wi-Fi setup path: the old AP-fallback hotspot
 *     and the browser setup page are gone by design.
 *   - HMAC-authenticated HTTP API on the AsyncWebServer (:80):
 *     GET /challenge, POST /auth -> bearer token, GET /state,
 *     POST /cmd. The old unauthenticated browser UI endpoints are
 *     gone; every network entry point now demands the shared secret.
 *
 * Protocol details (message shapes, the auth handshake, the wifi
 * announce) are identical to relay_timer_connection.ino so the app
 * works unchanged. What differs is everything behind the protocol:
 * state lives in _state (with StateLock + markDirty + the pending-
 * save queues), schedules are Presets, names come from the shared
 * NAME_OPTIONS pool with its uniqueness rules, time is the DS3231,
 * and pumps are the real RS485 drivers.
 *
 * Wiring into the sketch:
 *   setup():  appLinkBegin();   // after rtcBegin()/rs485Begin()
 *   loop():   appLinkLoop();    // every pass
 *
 * >>> DEVICE_SECRET must match app/index.tsx byte for byte. <<<
 * ---------------------------------------------------------------
 */

#include <NimBLEDevice.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <Update.h>

// The sketch defines the real version before including this header.
#ifndef FW_VERSION
#define FW_VERSION "0.0.0"
#endif
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "freertos/semphr.h"

#include "shared_state.h"
#include "rtc_clock.h"     // setDeviceTime() — the RTC stays the time authority
#include "rs485_pumps.h"   // pumpActualStateGet() for live pump state

// ============================================================
//  CONFIG — must match app/index.tsx
// ============================================================
#define APP_SERVICE_UUID  "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define APP_CHAR_RX_UUID  "6e400002-b5a3-f393-e0a9-e50e24dcca9e" // app -> esp32
#define APP_CHAR_TX_UUID  "6e400003-b5a3-f393-e0a9-e50e24dcca9e" // esp32 -> app

static const char APP_DEVICE_SECRET[] =
  "3f9a1c7e5b28d04a6ef391c8b7d25a06e4c8193bd7f6025a8c1e93b47dfa6210";

#define APP_AUTH_TIMEOUT_MS       10000UL
#define APP_MAX_SESSIONS          3
#define APP_REQUIRE_LINK_ENC      1

#define APP_MAX_HTTP_SESSIONS     4
#define APP_HTTP_TOKEN_TTL_MS     3600000UL
#define APP_WIFI_JOIN_TIMEOUT_MS  20000UL
#define APP_WIFI_RETRY_MS         30000UL
#define APP_WIFI_RETRY_BLE_MS     120000UL // gentle while a phone is on BLE (radio contention)

// ============================================================
//  IDENTITY
// ============================================================
char appBleName[24];
char appDeviceMac[18] = "";

inline void appDeriveIdentity() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(appBleName, sizeof(appBleName), "PoolCtrl-%02X%02X%02X", mac[3], mac[4], mac[5]);
  uint8_t sta[6];
  esp_read_mac(sta, ESP_MAC_WIFI_STA);
  snprintf(appDeviceMac, sizeof(appDeviceMac), "%02X:%02X:%02X:%02X:%02X:%02X",
           sta[0], sta[1], sta[2], sta[3], sta[4], sta[5]);
}

// ============================================================
//  CRYPTO (HMAC-SHA256, constant-time compare, hex, keystream)
// ============================================================
inline void appHmacHex(const char* key, const char* msg, char* outHex) {
  uint8_t mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, strlen(msg));
  mbedtls_md_hmac_finish(&ctx, mac);
  mbedtls_md_free(&ctx);
  static const char* HEXD = "0123456789abcdef"; // not HEX — Print.h #defines that
  for (int i = 0; i < 32; i++) {
    outHex[i * 2]     = HEXD[mac[i] >> 4];
    outHex[i * 2 + 1] = HEXD[mac[i] & 0x0f];
  }
  outHex[64] = '\0';
}

inline void appHmacRaw(const char* key, const char* msg, uint8_t out[32]) {
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, info, 1);
  mbedtls_md_hmac_starts(&ctx, (const uint8_t*)key, strlen(key));
  mbedtls_md_hmac_update(&ctx, (const uint8_t*)msg, strlen(msg));
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
}

inline bool appCtEquals(const char* a, const char* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

inline int appHexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

inline int appHexToBytes(const char* hex, uint8_t* out, size_t maxOut) {
  size_t len = strlen(hex);
  if (len % 2 != 0 || len / 2 > maxOut) return -1;
  for (size_t i = 0; i < len; i += 2) {
    int hi = appHexNibble(hex[i]), lo = appHexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i / 2] = (uint8_t)((hi << 4) | lo);
  }
  return (int)(len / 2);
}

// Wi-Fi password decryption: XOR against HMAC(secret, "<nonce>:<block>")
// keystream, matching keystreamEncryptHex() in the app.
inline bool appKeystreamDecrypt(const char* nonce, const char* cipherHex, char* outPlain, size_t maxOut) {
  uint8_t cipher[128];
  int n = appHexToBytes(cipherHex, cipher, sizeof(cipher));
  if (n < 0 || (size_t)n >= maxOut) return false;
  uint8_t block[32];
  char blockMsg[48];
  for (int i = 0; i < n; i++) {
    if (i % 32 == 0) {
      snprintf(blockMsg, sizeof(blockMsg), "%s:%d", nonce, i / 32);
      appHmacRaw(APP_DEVICE_SECRET, blockMsg, block);
    }
    outPlain[i] = (char)(cipher[i] ^ block[i % 32]);
  }
  outPlain[n] = '\0';
  return true;
}

inline void appMakeNonce(char* out /*33 bytes*/) {
  static const char* HEXD = "0123456789abcdef";
  for (int i = 0; i < 16; i++) {
    uint8_t b = (uint8_t)(esp_random() & 0xff);
    out[i * 2]     = HEXD[b >> 4];
    out[i * 2 + 1] = HEXD[b & 0x0f];
  }
  out[32] = '\0';
}

// ============================================================
//  BLE SESSIONS
// ============================================================
struct AppSession {
  bool     inUse;
  uint16_t connHandle;
  bool     authed;
  bool     dropPending; // loop() asked for a disconnect; free happens in onDisconnect
  char     nonceHex[33]; // kept after auth — the setWifi keystream derives from it
  bool     nonceIssued;
  uint32_t connectedAtMs;
  uint16_t mtu;
  String   rxBuffer;
};

NimBLEServer* appBleServer = nullptr;
NimBLECharacteristic* appTxChar = nullptr;
NimBLECharacteristic* appRxChar = nullptr;
SemaphoreHandle_t appTxMutex = nullptr;
AppSession appSessions[APP_MAX_SESSIONS];

inline AppSession* appSessionByHandle(uint16_t h) {
  for (int i = 0; i < APP_MAX_SESSIONS; i++)
    if (appSessions[i].inUse && appSessions[i].connHandle == h) return &appSessions[i];
  return nullptr;
}

inline AppSession* appAllocSession(uint16_t h) {
  for (int i = 0; i < APP_MAX_SESSIONS; i++) {
    if (appSessions[i].inUse) continue;
    appSessions[i] = AppSession{};
    appSessions[i].inUse = true;
    appSessions[i].connHandle = h;
    appSessions[i].connectedAtMs = millis();
    appSessions[i].mtu = 23;
    return &appSessions[i];
  }
  return nullptr;
}

inline void appFreeSession(uint16_t h) {
  AppSession* s = appSessionByHandle(h);
  if (!s) return;
  s->inUse = false;
  s->authed = false;
  s->rxBuffer = "";
  memset(s->nonceHex, 0, sizeof(s->nonceHex));
}

inline int appAuthedCount() {
  int n = 0;
  for (int i = 0; i < APP_MAX_SESSIONS; i++) if (appSessions[i].inUse && appSessions[i].authed) n++;
  return n;
}
inline int appActiveCount() {
  int n = 0;
  for (int i = 0; i < APP_MAX_SESSIONS; i++) if (appSessions[i].inUse) n++;
  return n;
}

// ============================================================
//  WI-FI LINK  (STA only, provisioned over BLE, creds in NVS)
// ============================================================
enum AppWifiPhase { APPWIFI_UNCONFIGURED, APPWIFI_CONNECTING, APPWIFI_ONLINE, APPWIFI_OFFLINE };

namespace _appwifi {
  char ssid[33] = "";
  char pass[65] = "";
  bool configured = false;
  AppWifiPhase phase = APPWIFI_UNCONFIGURED;
  bool joinFailed = false;
  uint32_t attemptStartMs = 0;
  uint32_t lastRetryMs = 0;
  uint32_t announceUntilMs = 0;
  uint32_t lastAnnounceMs = 0;
  // Work queued by BLE callbacks, serviced ONLY from loop(): the NimBLE
  // task must never block on Wi-Fi scans/joins or touch Preferences
  // concurrently with processPendingSaves().
  volatile bool pendingSave = false;
  volatile bool pendingJoin = false;
  volatile bool pendingForget = false;
  volatile bool pendingScan = false;
  uint16_t scanRequesterConn = 0;
}

// Latest completed Wi-Fi scan, cached for the app's GET /wifiScan poll.
// `appScanFresh` marks it current — every new scan request (BLE or HTTP)
// must clear it, or the poll is served the previous scan's list.
String appLastScanJson;
volatile bool appScanFresh = false;

inline const char* appWifiPhaseStr() {
  switch (_appwifi::phase) {
    case APPWIFI_UNCONFIGURED: return "unconfigured";
    case APPWIFI_CONNECTING:   return "connecting";
    case APPWIFI_ONLINE:       return "online";
    default:                   return "offline";
  }
}

inline void appWifiLoadCreds() {
  _state::prefs.begin("appwifi", true);
  String s = _state::prefs.getString("ssid", "");
  String p = _state::prefs.getString("pass", "");
  _state::prefs.end();
  strlcpy(_appwifi::ssid, s.c_str(), sizeof(_appwifi::ssid));
  strlcpy(_appwifi::pass, p.c_str(), sizeof(_appwifi::pass));
  _appwifi::configured = strlen(_appwifi::ssid) > 0;
  _appwifi::phase = _appwifi::configured ? APPWIFI_OFFLINE : APPWIFI_UNCONFIGURED;
}

inline void appWifiSaveCreds() {
  _state::prefs.begin("appwifi", false);
  _state::prefs.putString("ssid", _appwifi::ssid);
  _state::prefs.putString("pass", _appwifi::pass);
  _state::prefs.end();
}

inline void appWifiClearCreds() {
  _state::prefs.begin("appwifi", false);
  _state::prefs.remove("ssid");
  _state::prefs.remove("pass");
  _state::prefs.end();
  _appwifi::ssid[0] = '\0';
  _appwifi::pass[0] = '\0';
  _appwifi::configured = false;
  _appwifi::phase = APPWIFI_UNCONFIGURED;
  WiFi.disconnect(true);
}

inline void appWifiAnnounceStart() { // repeat verdict frames for 20s; one loss is harmless
  _appwifi::announceUntilMs = millis() + 20000;
  _appwifi::lastAnnounceMs = 0;
}

inline void appWifiBeginJoin() {
  if (!_appwifi::configured) return;
  Serial.printf("[wifi] connecting to \"%s\"...\n", _appwifi::ssid);
  WiFi.mode(WIFI_STA);
  // Modem sleep must stay ENABLED while BLE is up: one shared 2.4GHz
  // radio, and coexistence timeshares it inside Wi-Fi sleep windows.
  WiFi.setSleep(true);
  WiFi.begin(_appwifi::ssid, _appwifi::pass);
  _appwifi::joinFailed = false;
  _appwifi::phase = APPWIFI_CONNECTING;
  _appwifi::attemptStartMs = millis();
  _appwifi::announceUntilMs = 0; // stale verdict frames must stop now
}

// ============================================================
//  STATE JSON  (the app's full-state message, built from _state)
// ============================================================
inline const char* appModeStr(RelayMode m) {
  return m == MODE_ON ? "on" : m == MODE_OFF ? "off" : "auto";
}
inline RelayMode appModeParse(const char* s) {
  if (strcmp(s, "on") == 0) return MODE_ON;
  if (strcmp(s, "off") == 0) return MODE_OFF;
  return MODE_AUTO;
}
inline int appTargetParse(const char* t) {
  if (strncmp(t, "Relay ", 6) == 0) { int n = t[6] - '0'; if (n >= 1 && n <= 3) return TGT_RELAY1 + n - 1; }
  if (strncmp(t, "Pump ", 5) == 0)  { int n = t[5] - '0'; if (n >= 1 && n <= 4) return TGT_PUMP1 + n - 1; }
  return -1;
}
inline void appTargetToString(uint8_t t, char* out, size_t n) {
  if (t <= TGT_RELAY3) snprintf(out, n, "Relay %d", t + 1);
  else snprintf(out, n, "Pump %d", t - TGT_PUMP1 + 1);
}
inline int8_t appNameToIndex(const char* name) {
  for (int i = 0; i < NAME_OPTION_COUNT; i++)
    if (strcmp(NAME_OPTIONS[i], name) == 0) return (int8_t)i;
  return -1;
}

// Same RPM windows the web UI enforced on manual pump control.
inline bool appSpeedValidFor(uint8_t pumpType, int speed) {
  int lo = (pumpType == PUMP_PENTAIR) ? 450 : (pumpType == PUMP_BLACKDECKER) ? 600 : 800;
  int hi = (pumpType == PUMP_EMAUX) ? 3400 : 3450;
  return speed >= lo && speed <= hi;
}
inline const char* appSpeedRangeMsg(uint8_t pumpType) {
  if (pumpType == PUMP_PENTAIR)     return "speed must be 450-3450 RPM for Pentair";
  if (pumpType == PUMP_BLACKDECKER) return "speed must be 600-3450 RPM for Black & Decker";
  return "speed must be 800-3400 RPM for Emaux";
}

// PumpType <-> the exact strings the app protocol uses.
inline const char* appPumpTypeStr(uint8_t t) {
  return (t == PUMP_EMAUX) ? "Emaux" : (t == PUMP_BLACKDECKER) ? "Black & Decker" : "Pentair";
}
inline uint8_t appPumpTypeParse(const char* s) {
  if (strcmp(s, "Emaux") == 0) return PUMP_EMAUX;
  if (strcmp(s, "Black & Decker") == 0) return PUMP_BLACKDECKER;
  return PUMP_PENTAIR;
}

// "2h 05m left" / "Next in 0h 40m" / "No active schedule" for one target.
inline void appCountdownText(uint8_t target, char* out, size_t outLen) {
  uint8_t h, m, s; clockGet(h, m, s);
  int curMin = (int)h * 60 + m;
  bool active = false; int minsLeft = 0; int untilNext = 1441;
  for (int i = 0; i < MAX_PRESETS; i++) {
    Preset p;
    if (!presetGet(i, p) || !p.used || !p.enabled || p.target != target) continue;
    int durMin = (int)p.durHour * 60 + p.durMinute;
    if (durMin <= 0) continue;
    int startMin = (int)p.startHour * 60 + p.startMinute;
    int endMin = startMin + durMin;
    if (isWithinSchedule(p, curMin)) {
      int remaining;
      if (endMin <= 1440) remaining = endMin - curMin;
      else {
        int wrapEnd = endMin - 1440;
        remaining = (curMin >= startMin) ? (1440 - curMin) + wrapEnd : wrapEnd - curMin;
      }
      if (!active || remaining > minsLeft) minsLeft = remaining;
      active = true;
    } else {
      int un = (startMin >= curMin) ? startMin - curMin : (1440 - curMin) + startMin;
      if (un < untilNext) untilNext = un;
    }
  }
  if (active)               snprintf(out, outLen, "%dh %02dm left", minsLeft / 60, minsLeft % 60);
  else if (untilNext <= 1440) snprintf(out, outLen, "Next in %dh %02dm", untilNext / 60, untilNext % 60);
  else                      strlcpy(out, "No active schedule", outLen);
}

inline String appBuildStateJson() {
  // 8 presets + 4 pumps + 3 relays + countdown strings need ~2.8 KB of
  // pool at worst; on overflow ArduinoJson silently DROPS members, so
  // keep real headroom.
  DynamicJsonDocument doc(4096);
  doc["type"] = "state";
  doc["mac"] = appDeviceMac;   // stable device identity — the app keys its registry on this
  doc["name"] = appBleName;
  doc["fwVersion"] = FW_VERSION; // the app compares this against the GitHub manifest

  JsonObject w = doc.createNestedObject("wifi");
  w["state"] = appWifiPhaseStr();
  w["ssid"] = _appwifi::ssid;
  w["joinFailed"] = _appwifi::joinFailed;
  if (_appwifi::phase == APPWIFI_ONLINE) {
    w["ip"] = WiFi.localIP().toString();
    w["rssi"] = WiFi.RSSI();
  }
  doc["timeSynced"] = clockWasSet();
  doc["r2Depends"] = relay2DependencyGet(); // the web UI's dependency switch, now in the app

  char nameBuf[24], cdBuf[40];
  bool r1, r2, r3; relayActualStatesGet(r1, r2, r3);
  const bool rAct[3] = { r1, r2, r3 };

  JsonArray relayArr = doc.createNestedArray("relays");
  for (int i = 1; i <= MAX_RELAYS; i++) {
    JsonObject r = relayArr.createNestedObject();
    r["id"] = i;
    relayDisplayName(i, nameBuf, sizeof(nameBuf));
    r["name"] = nameBuf;
    r["mode"] = appModeStr(relayModeGet(i));
    r["active"] = rAct[i - 1];
    // Countdown line mirrors the old web UI exactly: Relay 1 shows its
    // schedule countdown, Relay 2 shows its dependency status, Relay 3
    // is the always-independent channel.
    if (i == 2)      strlcpy(cdBuf, relay2DependencyGet() ? "Depends on Relay 1" : "Independent", sizeof(cdBuf));
    else if (i == 3) strlcpy(cdBuf, "Independent", sizeof(cdBuf));
    else             appCountdownText(TGT_RELAY1 + i - 1, cdBuf, sizeof(cdBuf));
    r["countdown"] = cdBuf;
  }

  JsonArray pumpArr = doc.createNestedArray("pumps");
  for (int i = 1; i <= MAX_PUMPS; i++) {
    JsonObject p = pumpArr.createNestedObject();
    p["id"] = i;
    pumpDisplayName(i, nameBuf, sizeof(nameBuf));
    p["name"] = nameBuf;
    p["mode"] = appModeStr(pumpModeGet(i));
    bool pActive; uint16_t pSpeed;
    pumpActualStateGet(i, pActive, pSpeed);
    p["active"] = pActive;
    p["speedRpm"] = pSpeed;
    uint16_t ms; uint8_t mt; pumpManualParamsGet(i, ms, mt);
    p["pumpType"] = appPumpTypeStr(mt);
    // Configured manual set-point, distinct from speedRpm above (the
    // CONFIRMED speed, 0 while the pump hasn't acknowledged). The app's
    // speed input shows this — conflating the two wiped the typed value
    // the moment an unconfirmed state push arrived.
    p["setSpeedRpm"] = ms;
    appCountdownText(TGT_PUMP1 + i - 1, cdBuf, sizeof(cdBuf));
    p["countdown"] = cdBuf;
  }

  // Presets <-> the app's "schedules". The id is the slot index ("p0".."p7"),
  // stable across edits, which is all the app needs from an id.
  JsonArray schedArr = doc.createNestedArray("schedules");
  for (int i = 0; i < MAX_PRESETS; i++) {
    Preset p;
    if (!presetGet(i, p) || !p.used) continue;
    JsonObject s = schedArr.createNestedObject();
    char idBuf[8]; snprintf(idBuf, sizeof(idBuf), "p%d", i);
    s["id"] = idBuf;
    char tBuf[12]; appTargetToString(p.target, tBuf, sizeof(tBuf));
    s["target"] = tBuf;
    s["hour24"] = p.startHour;
    s["minute"] = p.startMinute;
    s["durationMin"] = (int)p.durHour * 60 + p.durMinute;
    s["enabled"] = p.enabled;
    // Per-preset pump parameters, exactly as the web preset editor had.
    s["speedRpm"] = p.speed;
    s["pumpType"] = appPumpTypeStr(p.pumpType);
  }

  uint8_t th, tm, ts; clockGet(th, tm, ts);
  char timeStr[9];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", th, tm, ts);
  doc["time"] = timeStr;

  String out;
  serializeJson(doc, out);
  return out;
}

// ============================================================
//  TRANSPORT-AGNOSTIC COMMANDS  (BLE and HTTP share this)
// ============================================================
inline String appBuildAck(const char* cmd, bool ok, const char* message = "") {
  StaticJsonDocument<160> doc;
  doc["type"] = "ack";
  doc["cmd"] = cmd;
  doc["ok"] = ok;
  if (strlen(message) > 0) doc["message"] = message;
  String out;
  serializeJson(doc, out);
  return out;
}

// "p<slot>" -> slot index, or -1. Anything else ("p", "pxyz", the app's
// optimistic "local-…" ids) used to atoi() to 0 and silently hit preset 0.
inline int appParsePresetId(const char* id) {
  if (!id || id[0] != 'p' || id[1] == '\0') return -1;
  int slot = 0;
  for (const char* c = id + 1; *c; c++) {
    if (*c < '0' || *c > '9') return -1;
    slot = slot * 10 + (*c - '0');
    if (slot >= MAX_PRESETS) return -1;
  }
  return slot;
}

// Applies a relay/pump/schedule/time command to _state through the same
// accessors the OLED uses (they handle StateLock, markDirty and the
// flash saves). `changed` tells the caller a state broadcast is due —
// though markDirty() already makes the loop's version-watcher push one.
inline String appExecuteCommand(JsonDocument& doc, bool& changed) {
  const char* cmd = doc["cmd"] | "";
  changed = false;

  if (strcmp(cmd, "setTime") == 0) {
    long epoch = doc["epoch"] | 0L;
    long tz = doc["tzOffsetSec"] | 0L;
    if (epoch < 1700000000L) return appBuildAck(cmd, false, "implausible timestamp");
    // The DS3231 stores plain local wall-clock h/m — convert and persist.
    long localSecs = ((epoch + tz) % 86400L + 86400L) % 86400L;
    uint8_t h = localSecs / 3600, m = (localSecs / 60) % 60;
    setDeviceTime(h, m); // live clock + RTC, survives power cycles
    Serial.printf("[cmd] setTime -> %02u:%02u (from app clock)\n", h, m);
    changed = true;
    return appBuildAck(cmd, true);
  }

  if (strcmp(cmd, "setRelayMode") == 0) {
    int id = doc["id"] | -1;
    if (id < 1 || id > MAX_RELAYS) return appBuildAck(cmd, false, "no such relay");
    relayModeSet(id, appModeParse(doc["mode"] | "auto"));
    Serial.printf("[cmd] relay %d mode -> %s\n", id, (const char*)(doc["mode"] | "auto"));
    changed = true;
    return appBuildAck(cmd, true);
  }

  if (strcmp(cmd, "setRelayName") == 0) {
    int id = doc["id"] | -1;
    const char* name = doc["name"] | "";
    if (id < 1 || id > MAX_RELAYS) return appBuildAck(cmd, false, "no such relay");
    int8_t idx = appNameToIndex(name);
    char defBuf[12]; snprintf(defBuf, sizeof(defBuf), "Relay %d", id);
    if (idx < 0 && strcmp(name, defBuf) != 0)
      return appBuildAck(cmd, false, "name not in the allowed list");
    if (!relayNameIndexSet(id, idx)) // idx = -1 resets to the default label
      return appBuildAck(cmd, false, "name already in use");
    Serial.printf("[cmd] relay %d renamed to \"%s\"\n", id, name);
    changed = true;
    return appBuildAck(cmd, true);
  }

  if (strcmp(cmd, "setPumpMode") == 0) {
    int id = doc["id"] | -1;
    if (id < 1 || id > MAX_PUMPS) return appBuildAck(cmd, false, "no such pump");
    pumpModeSet(id, appModeParse(doc["mode"] | "auto"));
    Serial.printf("[cmd] pump %d mode -> %s\n", id, (const char*)(doc["mode"] | "auto"));
    changed = true;
    return appBuildAck(cmd, true);
  }

  if (strcmp(cmd, "setPumpConfig") == 0) {
    int id = doc["id"] | -1;
    if (id < 1 || id > MAX_PUMPS) return appBuildAck(cmd, false, "no such pump");
    uint16_t speed; uint8_t type;
    pumpManualParamsGet(id, speed, type);
    if (doc.containsKey("speedRpm")) speed = doc["speedRpm"] | 0;
    if (doc.containsKey("pumpType"))
      type = appPumpTypeParse(doc["pumpType"] | "Pentair");
    // Black & Decker is pump-1-only: the pump is fixed at Modbus addr 1.
    if (type == PUMP_BLACKDECKER && id != 1)
      return appBuildAck(cmd, false, "Black & Decker is only supported on Pump 1");
    if (doc.containsKey("speedRpm") && !appSpeedValidFor(type, speed))
      return appBuildAck(cmd, false, appSpeedRangeMsg(type));
    pumpManualParamsSet(id, speed, type);
    if (doc.containsKey("name")) {
      const char* name = doc["name"] | "";
      int8_t idx = appNameToIndex(name);
      // Both spellings of the default reset the custom name: the OLED
      // shows "PUMPn", the app sends "Pump n".
      char defBuf[12];  snprintf(defBuf, sizeof(defBuf), "PUMP%d", id);
      char defBuf2[12]; snprintf(defBuf2, sizeof(defBuf2), "Pump %d", id);
      if (idx < 0 && strcmp(name, defBuf) != 0 && strcmp(name, defBuf2) != 0)
        return appBuildAck(cmd, false, "name not in the allowed list");
      if (!pumpNameIndexSet(id, idx))
        return appBuildAck(cmd, false, "name unavailable (in use, or relay-only)");
      Serial.printf("[cmd] pump %d renamed to \"%s\"\n", id, name);
    }
    markDirty();
    Serial.printf("[cmd] pump %d config: speed=%u type=%u\n", id, speed, type);
    changed = true;
    return appBuildAck(cmd, true);
  }

  if (strcmp(cmd, "addSchedule") == 0 || strcmp(cmd, "updateSchedule") == 0) {
    bool isNew = strcmp(cmd, "addSchedule") == 0;
    int slot;
    Preset p{};
    if (isNew) {
      slot = presetFindFreeSlot();
      if (slot < 0) return appBuildAck(cmd, false, "schedule list full (8 max)");
      p.pumpType = PUMP_PENTAIR;
      p.speed = 2000; // pump presets need a speed; the OLED can refine it
    } else {
      const char* id = doc["id"] | "";
      slot = appParsePresetId(id);
      if (slot < 0 || !presetGet(slot, p) || !p.used) return appBuildAck(cmd, false, "no such schedule");
    }
    JsonObject sched = doc["schedule"];
    int target = appTargetParse(sched["target"] | "Relay 1");
    if (target < 0) return appBuildAck(cmd, false, "bad target");
    int durationMin = sched["durationMin"] | 60;
    if (durationMin <= 0) return appBuildAck(cmd, false, "duration must be > 0");
    // Same bounds the web preset editor enforced: 23h59m max (anything
    // longer wraps isWithinSchedule into a 24/7 schedule) and a start
    // time that actually exists (out-of-range hours silently never fire).
    if (durationMin > 23 * 60 + 59) return appBuildAck(cmd, false, "invalid duration");
    int startHour = sched["hour24"] | 6;
    int startMinute = sched["minute"] | 0;
    if (startHour < 0 || startHour > 23 || startMinute < 0 || startMinute > 59)
      return appBuildAck(cmd, false, "invalid start time");
    p.used = true;
    p.enabled = sched["enabled"] | true;
    p.target = (uint8_t)target;
    p.startHour = (uint8_t)startHour;
    p.startMinute = (uint8_t)startMinute;
    p.durHour = durationMin / 60;
    p.durMinute = durationMin % 60;
    // Per-preset pump parameters (mirrors the web preset editor). Absent
    // fields keep the preset's existing values / the new-preset defaults.
    if (sched.containsKey("pumpType"))
      p.pumpType = appPumpTypeParse(sched["pumpType"] | "Pentair");
    if (sched.containsKey("speedRpm")) p.speed = sched["speedRpm"] | 2000;
    // Black & Decker schedules may only target Pump 1 (fixed Modbus addr 1).
    if (p.pumpType == PUMP_BLACKDECKER && target != TGT_PUMP1)
      return appBuildAck(cmd, false, "Black & Decker is only supported on Pump 1");
    if (target >= TGT_PUMP1 && !appSpeedValidFor(p.pumpType, p.speed))
      return appBuildAck(cmd, false, appSpeedRangeMsg(p.pumpType));
    presetWrite(slot, p);
    Serial.printf("[cmd] %s p%d: target=%u %02u:%02u for %dmin, %s\n",
                  isNew ? "schedule added" : "schedule updated", slot, p.target,
                  p.startHour, p.startMinute, durationMin, p.enabled ? "enabled" : "disabled");
    changed = true;
    return appBuildAck(cmd, true);
  }

  if (strcmp(cmd, "deleteSchedule") == 0) {
    const char* id = doc["id"] | "";
    int slot = appParsePresetId(id);
    if (slot < 0 || !presetDelete(slot))
      return appBuildAck(cmd, false, "no such schedule");
    Serial.printf("[cmd] schedule deleted %s\n", id);
    changed = true;
    return appBuildAck(cmd, true);
  }

  if (strcmp(cmd, "setRelay2Dependency") == 0) {
    // The web UI's "Relay 2 depends on Relay 1" switch, now app-driven.
    bool depends = doc["depends"] | true;
    relay2DependencySet(depends);
    Serial.printf("[cmd] relay 2 dependency -> %s\n", depends ? "depends on relay 1" : "independent");
    changed = true;
    return appBuildAck(cmd, true);
  }

  if (strcmp(cmd, "toggleSchedule") == 0) {
    const char* id = doc["id"] | "";
    bool en = doc["enabled"] | true;
    int slot = appParsePresetId(id);
    if (slot < 0 || !presetSetEnabled(slot, en))
      return appBuildAck(cmd, false, "no such schedule");
    Serial.printf("[cmd] schedule %s %s\n", id, en ? "enabled" : "disabled");
    changed = true;
    return appBuildAck(cmd, true);
  }

  return appBuildAck(cmd, false, "unrecognized command");
}

// ============================================================
//  BLE SEND  (chunked to the MTU, one sender at a time)
// ============================================================
inline bool appSendChunkedTo(AppSession* s, const String& payload) {
  if (!s || !s->inUse || appTxChar == nullptr) return false;
  // Chunks from the NimBLE task (acks) and from loop() (broadcasts) must
  // not interleave on the shared characteristic — the phone reassembles
  // a hybrid of two messages and both fail to parse.
  if (appTxMutex && xSemaphoreTake(appTxMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    Serial.printf("[ble] tx mutex timeout — message skipped for conn %u\n", s->connHandle);
    return false;
  }
  String withDelim = payload + "\n";
  size_t chunkSize = (s->mtu > 3) ? (s->mtu - 3) : 20;
  size_t len = withDelim.length();
  bool ok = true;
  for (size_t offset = 0; ok && offset < len; offset += chunkSize) {
    size_t thisLen = min(chunkSize, len - offset);
    appTxChar->setValue((uint8_t*)(withDelim.c_str() + offset), thisLen);
    bool sent = false;
    for (int t = 0; t < 5 && !sent; t++) {
      sent = appTxChar->notify(s->connHandle);
      if (!sent) delay(15); // stack congested (Wi-Fi hogging the radio) — back off
    }
    if (!sent) {
      Serial.printf("[ble] notify dropped on conn %u — message abandoned\n", s->connHandle);
      ok = false;
    }
    if (ok) delay(10);
  }
  if (appTxMutex) xSemaphoreGive(appTxMutex);
  return ok;
}

inline void appBroadcastState() {
  if (appAuthedCount() == 0) return;
  String state = appBuildStateJson();
  for (int i = 0; i < APP_MAX_SESSIONS; i++)
    if (appSessions[i].inUse && appSessions[i].authed) appSendChunkedTo(&appSessions[i], state);
}

inline void appSendAckTo(AppSession* s, const char* cmd, bool ok, const char* message = "") {
  appSendChunkedTo(s, appBuildAck(cmd, ok, message));
}

// Small single-notify Wi-Fi status frame that survives radio contention;
// carries the device identity so the app never files it under the wrong
// controller. Logged unconditionally — this line next to the app's
// "[rx] wifi event" log is the ground truth for delivery.
inline void appBroadcastWifiEvent() {
  StaticJsonDocument<256> doc;
  doc["type"] = "wifi";
  doc["mac"] = appDeviceMac;
  doc["state"] = appWifiPhaseStr();
  doc["ssid"] = _appwifi::ssid;
  doc["joinFailed"] = _appwifi::joinFailed;
  if (_appwifi::phase == APPWIFI_ONLINE) doc["ip"] = WiFi.localIP().toString();
  String out;
  serializeJson(doc, out);
  if (appAuthedCount() == 0) {
    Serial.printf("[ble] wifi event %s — NO authenticated app session to notify\n", out.c_str());
    return;
  }
  for (int i = 0; i < APP_MAX_SESSIONS; i++) {
    if (!appSessions[i].inUse || !appSessions[i].authed) continue;
    bool ok = appSendChunkedTo(&appSessions[i], out);
    Serial.printf("[ble] wifi event -> conn %u: %s  %s\n",
                  appSessions[i].connHandle, ok ? "delivered" : "DROPPED", out.c_str());
  }
}

// Same trick for pump confirmations: the full state push is a large
// multi-chunk BLE message that radio contention can tear, which left
// the app's pump pill stuck at "Turning on…" over Bluetooth while
// Wi-Fi (polled HTTP) worked. This fits in one notify, so the flip of
// a pump's confirmed state reaches the phone reliably and immediately.
// Returns true only when the event reached every authed BLE session (or
// none exists to tell) — the caller keeps the change queued and retries
// on false, because a dropped notify must not swallow a confirmation.
inline bool appBroadcastPumpEvent(uint8_t pumpId, bool active, uint16_t speed) {
  StaticJsonDocument<192> doc;
  doc["type"] = "pump";
  doc["mac"] = appDeviceMac;
  doc["id"] = pumpId;
  doc["active"] = active;
  doc["speedRpm"] = speed;
  // Name rides along so a rename reaches BLE phones even when the big
  // state push is torn by radio contention.
  char nameBuf[16];
  pumpDisplayName(pumpId, nameBuf, sizeof(nameBuf));
  doc["name"] = nameBuf;
  String out;
  serializeJson(doc, out);
  if (appAuthedCount() == 0) return true; // nobody to tell — nothing to retry
  bool allOk = true;
  for (int i = 0; i < APP_MAX_SESSIONS; i++) {
    if (!appSessions[i].inUse || !appSessions[i].authed) continue;
    bool ok = appSendChunkedTo(&appSessions[i], out);
    if (!ok) allOk = false;
    Serial.printf("[ble] pump event -> conn %u: %s  %s\n",
                  appSessions[i].connHandle, ok ? "delivered" : "DROPPED", out.c_str());
  }
  return allOk;
}

// ============================================================
//  BLE AUTH + COMMAND HANDLING
// ============================================================
inline void appDropSession(AppSession* s, const char* why) {
  if (!s) return;
  Serial.printf("[auth] conn %u dropped: %s\n", s->connHandle, why);
  uint16_t h = s->connHandle;
  appFreeSession(h);
  if (appBleServer) appBleServer->disconnect(h);
}

inline void appSendChallenge(AppSession* s) {
  appMakeNonce(s->nonceHex);
  s->nonceIssued = true;
  StaticJsonDocument<128> doc;
  doc["type"] = "challenge";
  doc["nonce"] = s->nonceHex;
  String out;
  serializeJson(doc, out);
  appSendChunkedTo(s, out);
  Serial.printf("[auth] conn %u: challenge issued\n", s->connHandle);
}

// Returns true if `cmd` was an auth-phase command (handled here).
inline bool appHandleAuthCommand(AppSession* s, const char* cmd, JsonDocument& doc) {
  if (strcmp(cmd, "hello") == 0) {
    appSendChallenge(s);
    return true;
  }
  if (strcmp(cmd, "auth") == 0) {
    if (!s->nonceIssued) {
      appSendAckTo(s, "auth", false, "no challenge issued");
      appDropSession(s, "auth before hello");
      return true;
    }
    const char* given = doc["hmac"] | "";
    char expected[65];
    appHmacHex(APP_DEVICE_SECRET, s->nonceHex, expected);
    if (strlen(given) != 64 || !appCtEquals(given, expected, 64)) {
      appSendAckTo(s, "auth", false, "bad credentials");
      appDropSession(s, "HMAC mismatch");
      return true;
    }
    s->nonceIssued = false; // burn against replay; nonceHex stays for the setWifi keystream
    s->authed = true;
    Serial.printf("[auth] conn %u AUTHENTICATED\n", s->connHandle);

    // Stored credentials + no link -> try them NOW, not on the backoff
    // timer. The app waits for this attempt's verdict instead of popping
    // its credentials dialog over a stale failure.
    if (_appwifi::configured && _appwifi::phase == APPWIFI_OFFLINE) {
      Serial.println("[wifi] app connected — trying stored network now");
      _appwifi::joinFailed = false;
      _appwifi::pendingJoin = true;
    }

    // Success ack carries the identity: a single-notify frame that
    // provably survives (auth completed over it), unlike the chunked
    // state push — the app registers the device off this.
    {
      StaticJsonDocument<192> ok;
      ok["type"] = "ack";
      ok["cmd"] = "auth";
      ok["ok"] = true;
      ok["mac"] = appDeviceMac;
      ok["name"] = appBleName;
      String out;
      serializeJson(ok, out);
      appSendChunkedTo(s, out);
    }

    // Tell the app the CURRENT Wi-Fi status RIGHT NOW, in one small frame.
    // Its add-device flow decides "skip the picker or show it" on this
    // single fact — waiting on the announce loop stalled the popup by
    // 10-20s, because a join queued above cancels the announce window on
    // its very first loop() pass, before one frame ever fires. Sent
    // before the (slow, multi-chunk) state push on purpose.
    appBroadcastWifiEvent();
    // Keep repeating for 20s as insurance against the one-shot frame
    // being lost. (A join kicked above still cancels the REPEATS and
    // re-announces its own verdict instead — the immediate frame above
    // has already gone out by then.)
    appWifiAnnounceStart();

    appSendChunkedTo(s, appBuildStateJson());
    return true;
  }
  return false;
}

inline void appHandleCommand(AppSession* s, const String& msg) {
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, msg)) {
    appSendAckTo(s, "unknown", false, "bad json");
    return;
  }
  const char* cmd = doc["cmd"] | "";

  if (appHandleAuthCommand(s, cmd, doc)) return;
  if (!s->authed) {
    appSendAckTo(s, cmd, false, "unauthorized");
    appDropSession(s, "command before auth");
    return;
  }

  if (strcmp(cmd, "getState") == 0) {
    appSendChunkedTo(s, appBuildStateJson());
    return;
  }

  // ---- Wi-Fi provisioning (BLE-only by design) ----------------------
  if (strcmp(cmd, "scanWifi") == 0) {
    if (_appwifi::pendingScan) { appSendAckTo(s, cmd, false, "scan already in progress"); return; }
    _appwifi::scanRequesterConn = s->connHandle;
    appScanFresh = false; // or the app's HTTP fallback poll is served the PREVIOUS scan
    _appwifi::pendingScan = true; // WiFi.scanNetworks() blocks; run from loop()
    appSendAckTo(s, cmd, true, "scanning");
    return;
  }

  if (strcmp(cmd, "setWifi") == 0) {
    const char* ssid = doc["ssid"] | "";
    const char* passEnc = doc["passEnc"] | "";
    const char* passPlain = doc["pass"] | "";
    if (strlen(ssid) == 0) { appSendAckTo(s, cmd, false, "missing ssid"); return; }
    char pass[65] = "";
    if (strlen(passEnc) > 0) {
      if (!appKeystreamDecrypt(s->nonceHex, passEnc, pass, sizeof(pass))) {
        appSendAckTo(s, cmd, false, "could not decrypt credentials");
        return;
      }
    } else {
      strlcpy(pass, passPlain, sizeof(pass));
    }
    strlcpy(_appwifi::ssid, ssid, sizeof(_appwifi::ssid));
    strlcpy(_appwifi::pass, pass, sizeof(_appwifi::pass));
    _appwifi::configured = true;
    _appwifi::pendingSave = true; // NVS writes stay on the loop task
    _appwifi::pendingJoin = true;
    appSendAckTo(s, cmd, true, "connecting");
    return;
  }

  if (strcmp(cmd, "forgetWifi") == 0) {
    _appwifi::pendingForget = true; // serviced from loop()
    appSendAckTo(s, cmd, true);
    return;
  }

  // ---- everything else is shared with the HTTP transport -------------
  bool changed = false;
  String ack = appExecuteCommand(doc, changed);
  appSendChunkedTo(s, ack);
  // changed -> markDirty() already ran inside the accessors; the loop's
  // version watcher broadcasts fresh state to every session.
}

// ============================================================
//  BLE CALLBACKS
// ============================================================
class AppServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& info) override {
    uint16_t h = info.getConnHandle();
    AppSession* s = appAllocSession(h);
    if (!s) {
      Serial.printf("[ble] connection %u refused: %d/%d sessions in use\n", h, appActiveCount(), APP_MAX_SESSIONS);
      server->disconnect(h);
      return;
    }
    Serial.printf("[ble] central connected (conn %u), awaiting auth. %d/%d slots.\n", h, appActiveCount(), APP_MAX_SESSIONS);
    if (appActiveCount() < APP_MAX_SESSIONS) NimBLEDevice::startAdvertising();
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& info, int reason) override {
    uint16_t h = info.getConnHandle();
    appFreeSession(h);
    Serial.printf("[ble] central disconnected (conn %u, reason %d). %d/%d slots.\n", h, reason, appActiveCount(), APP_MAX_SESSIONS);
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& info) override {
    AppSession* s = appSessionByHandle(info.getConnHandle());
    if (s) s->mtu = mtu;
  }
#if APP_REQUIRE_LINK_ENC
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    if (!info.isEncrypted()) {
      Serial.println("[ble] link encryption failed — dropping connection");
      if (appBleServer) appBleServer->disconnect(info.getConnHandle());
    }
  }
#endif
};

class AppRxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    AppSession* s = appSessionByHandle(info.getConnHandle());
    if (!s) return;
    s->rxBuffer += c->getValue().c_str();
    if (s->rxBuffer.length() > 2048) { appDropSession(s, "rx buffer overflow"); return; }
    int nl;
    while ((nl = s->rxBuffer.indexOf('\n')) >= 0) {
      String line = s->rxBuffer.substring(0, nl);
      s->rxBuffer = s->rxBuffer.substring(nl + 1);
      if (line.length() > 0) {
        appHandleCommand(s, line);
        if (!s->inUse) return;
      }
    }
  }
};

// ============================================================
//  HTTP API  (HMAC-authenticated, on the AsyncWebServer)
// ============================================================
AsyncWebServer appServer(80);
bool appServerStarted = false;

struct AppHttpSession {
  bool     inUse;
  char     token[33];
  char     nonce[33];   // the challenge nonce this token was authenticated with
  uint32_t issuedAtMs;
};
AppHttpSession appHttpSessions[APP_MAX_HTTP_SESSIONS];
char appHttpNonce[33] = "";

// ---- OTA ---------------------------------------------------------
// The app downloads the release .bin from GitHub and POSTs it raw to
// /update (bearer-authenticated). The image lands in the inactive OTA
// slot; only a fully verified write reboots into it, so an interrupted
// upload never bricks — the old slot keeps booting.
uint32_t appRebootAtMs = 0;           // set after a successful update; loop restarts
const char* appOtaRefuseMsg = nullptr; // reason the in-flight upload was refused

// Updating stalls the loop while flash sectors are written — refuse
// while any pump is running (manually or from a schedule) so an RS-485
// pump is never left unsupervised mid-command.
inline bool appAnyPumpRunning() {
  for (int i = 1; i <= MAX_PUMPS; i++) {
    bool a; uint16_t sp;
    pumpActualStateGet(i, a, sp);
    if (a || pumpModeGet(i) == MODE_ON) return true;
  }
  return false;
}

// Wi-Fi scans requested over HTTP: there is no push channel to deliver
// the results, so the loop stores the latest scan here and the app polls
// GET /wifiScan for it. The sentinel marks "requested via HTTP".
#define APP_SCAN_HTTP 0xFFFF
bool appHttpNonceIssued = false;

inline bool appHttpTokenValid(const char* token) {
  if (!token || strlen(token) != 32) return false;
  uint32_t now = millis();
  for (int i = 0; i < APP_MAX_HTTP_SESSIONS; i++) {
    if (!appHttpSessions[i].inUse) continue;
    if (now - appHttpSessions[i].issuedAtMs > APP_HTTP_TOKEN_TTL_MS) {
      appHttpSessions[i].inUse = false;
      continue;
    }
    if (appCtEquals(appHttpSessions[i].token, token, 32)) return true;
  }
  return false;
}

inline const char* appHttpIssueToken() {
  int slot = -1;
  for (int i = 0; i < APP_MAX_HTTP_SESSIONS; i++)
    if (!appHttpSessions[i].inUse) { slot = i; break; }
  if (slot < 0) { // recycle the oldest so a hoarder can't lock everyone out
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < APP_MAX_HTTP_SESSIONS; i++)
      if (appHttpSessions[i].issuedAtMs < oldest) { oldest = appHttpSessions[i].issuedAtMs; slot = i; }
  }
  appMakeNonce(appHttpSessions[slot].token);
  // Bind the challenge nonce to this session: Wi-Fi passwords are keystream-
  // encrypted against it, and another client pulling /challenge must not
  // rotate it out from under us (that desync read as "wrong password").
  strlcpy(appHttpSessions[slot].nonce, appHttpNonce, sizeof(appHttpSessions[slot].nonce));
  appHttpSessions[slot].inUse = true;
  appHttpSessions[slot].issuedAtMs = millis();
  return appHttpSessions[slot].token;
}

// The nonce the presenting bearer token was authenticated with; falls back
// to the global slot for malformed requests (setWifi is auth-gated anyway).
inline const char* appHttpSessionNonce(AsyncWebServerRequest* request) {
  if (request->hasHeader("Authorization")) {
    String h = request->header("Authorization");
    if (h.startsWith("Bearer ")) {
      String t = h.substring(7);
      if (t.length() == 32) {
        for (int i = 0; i < APP_MAX_HTTP_SESSIONS; i++)
          if (appHttpSessions[i].inUse && appCtEquals(appHttpSessions[i].token, t.c_str(), 32))
            return appHttpSessions[i].nonce;
      }
    }
  }
  return appHttpNonce;
}

inline bool appHttpAuthorized(AsyncWebServerRequest* request) {
  if (!request->hasHeader("Authorization")) return false;
  String h = request->header("Authorization");
  if (!h.startsWith("Bearer ")) return false;
  return appHttpTokenValid(h.substring(7).c_str());
}

inline void appHttpSendJson(AsyncWebServerRequest* request, int code, const String& body) {
  AsyncWebServerResponse* res = request->beginResponse(code, "application/json", body);
  res->addHeader("Cache-Control", "no-store");
  request->send(res);
}

inline void appHttpRegister() {
  appServer.on("/challenge", HTTP_GET, [](AsyncWebServerRequest* request) {
    appMakeNonce(appHttpNonce);
    appHttpNonceIssued = true;
    StaticJsonDocument<128> doc;
    doc["type"] = "challenge";
    doc["nonce"] = appHttpNonce;
    doc["mac"] = appDeviceMac;
    String out;
    serializeJson(doc, out);
    appHttpSendJson(request, 200, out);
  });

  appServer.on("/auth", HTTP_POST, [](AsyncWebServerRequest* request) {}, nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      // This callback fires once per TCP segment — later segments of a
      // too-large body must stay silent, or one request gets two sends.
      if (index != 0) return; // already rejected on the first segment
      if (len != total) { appHttpSendJson(request, 400, appBuildAck("auth", false, "body too large")); return; }
      if (!appHttpNonceIssued) { appHttpSendJson(request, 403, appBuildAck("auth", false, "no challenge issued")); return; }
      DynamicJsonDocument doc(256);
      if (deserializeJson(doc, data, len)) { appHttpSendJson(request, 400, appBuildAck("auth", false, "bad json")); return; }
      const char* given = doc["hmac"] | "";
      char expected[65];
      appHmacHex(APP_DEVICE_SECRET, appHttpNonce, expected);
      appHttpNonceIssued = false; // single use, burned even on failure
      if (strlen(given) != 64 || !appCtEquals(given, expected, 64)) {
        appHttpSendJson(request, 403, appBuildAck("auth", false, "bad credentials"));
        return;
      }
      StaticJsonDocument<160> res;
      res["type"] = "ack";
      res["cmd"] = "auth";
      res["ok"] = true;
      res["token"] = appHttpIssueToken();
      res["ttlMs"] = APP_HTTP_TOKEN_TTL_MS;
      String out;
      serializeJson(res, out);
      appHttpSendJson(request, 200, out);
      Serial.println("[http] client authenticated");
    });

  appServer.on("/state", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!appHttpAuthorized(request)) { appHttpSendJson(request, 401, appBuildAck("getState", false, "unauthorized")); return; }
    appHttpSendJson(request, 200, appBuildStateJson());
  });

  // Reachability probe for the app's "Ping over Wi-Fi" button. Deliberately
  // unauthenticated — it proves the network path and nothing else (no state,
  // no control). The serial line is the ground truth the user watches for.
  // Latest Wi-Fi scan results for HTTP clients (no push channel exists to
  // deliver them). {"pending":true} until the loop finishes the scan.
  appServer.on("/wifiScan", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (!appHttpAuthorized(request)) { appHttpSendJson(request, 401, appBuildAck("wifiScan", false, "unauthorized")); return; }
    if (appScanFresh && appLastScanJson.length()) { appHttpSendJson(request, 200, appLastScanJson); return; }
    appHttpSendJson(request, 200, String("{\"type\":\"wifiScan\",\"pending\":true}"));
  });

  appServer.on("/ping", HTTP_GET, [](AsyncWebServerRequest* request) {
    Serial.printf("[http] PING from %s — webserver link OK\n",
                  request->client()->remoteIP().toString().c_str());
    StaticJsonDocument<128> doc;
    doc["type"] = "pong";
    doc["mac"] = appDeviceMac;
    doc["fw"] = FW_VERSION; // the OTA flow polls this to confirm the new image booted
    String out;
    serializeJson(doc, out);
    appHttpSendJson(request, 200, out);
  });

  appServer.on("/cmd", HTTP_POST, [](AsyncWebServerRequest* request) {}, nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      // Fires once per TCP segment — reply on the first segment only, or
      // one request gets two sends (reachable pre-auth from the LAN).
      if (index != 0) return; // already answered on the first segment
      if (!appHttpAuthorized(request)) { appHttpSendJson(request, 401, appBuildAck("cmd", false, "unauthorized")); return; }
      if (len != total) { appHttpSendJson(request, 400, appBuildAck("cmd", false, "body too large")); return; }
      DynamicJsonDocument doc(1024);
      if (deserializeJson(doc, data, len)) { appHttpSendJson(request, 400, appBuildAck("unknown", false, "bad json")); return; }
      const char* cmd = doc["cmd"] | "";
      if (strcmp(cmd, "getState") == 0) { appHttpSendJson(request, 200, appBuildStateJson()); return; }
      // Wi-Fi management over the LAN, for phones connected without a BLE
      // link. Joining a different network (or forgetting this one) drops
      // the HTTP transport mid-flight by nature — the app knows and falls
      // back to BLE to pick the device up again.
      if (strcmp(cmd, "scanWifi") == 0) {
        if (_appwifi::pendingScan) { appHttpSendJson(request, 200, appBuildAck(cmd, false, "scan already in progress")); return; }
        _appwifi::scanRequesterConn = APP_SCAN_HTTP;
        appScanFresh = false;
        _appwifi::pendingScan = true; // WiFi.scanNetworks() blocks; runs from loop()
        appHttpSendJson(request, 200, appBuildAck(cmd, true, "scanning"));
        return;
      }
      if (strcmp(cmd, "forgetWifi") == 0) {
        _appwifi::pendingForget = true; // serviced from loop()
        appHttpSendJson(request, 200, appBuildAck(cmd, true));
        return;
      }
      if (strcmp(cmd, "setWifi") == 0) {
        const char* ssid = doc["ssid"] | "";
        const char* passEnc = doc["passEnc"] | "";
        const char* passPlain = doc["pass"] | "";
        if (strlen(ssid) == 0) { appHttpSendJson(request, 200, appBuildAck(cmd, false, "missing ssid")); return; }
        char pass[65] = "";
        if (strlen(passEnc) > 0) {
          // Encrypted against the HTTP auth nonce (this session's challenge).
          if (!appKeystreamDecrypt(appHttpSessionNonce(request), passEnc, pass, sizeof(pass))) {
            appHttpSendJson(request, 200, appBuildAck(cmd, false, "could not decrypt credentials"));
            return;
          }
        } else {
          strlcpy(pass, passPlain, sizeof(pass));
        }
        strlcpy(_appwifi::ssid, ssid, sizeof(_appwifi::ssid));
        strlcpy(_appwifi::pass, pass, sizeof(_appwifi::pass));
        _appwifi::configured = true;
        _appwifi::pendingSave = true; // NVS writes stay on the loop task
        _appwifi::pendingJoin = true;
        appHttpSendJson(request, 200, appBuildAck(cmd, true, "connecting"));
        return;
      }
      bool changed = false;
      String ack = appExecuteCommand(doc, changed);
      appHttpSendJson(request, 200, ack);
      // markDirty() inside the accessors makes the loop broadcast to any
      // BLE-connected phones — no cross-task notify from here.
    });

  // OTA: raw firmware image as the POST body. Chunks stream into the
  // inactive app slot; the final chunk verifies and schedules a reboot.
  appServer.on("/update", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      if (!appHttpAuthorized(request)) { appHttpSendJson(request, 401, appBuildAck("update", false, "unauthorized")); return; }
      if (appOtaRefuseMsg) { appHttpSendJson(request, 409, appBuildAck("update", false, appOtaRefuseMsg)); return; }
      if (Update.hasError()) { appHttpSendJson(request, 400, appBuildAck("update", false, "flash write failed — see serial log")); return; }
      appHttpSendJson(request, 200, appBuildAck("update", true, "update verified — rebooting"));
      appRebootAtMs = millis() + 1500; // let the response leave first
    },
    NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        appOtaRefuseMsg = nullptr;
        if (!appHttpAuthorized(request)) { appOtaRefuseMsg = "unauthorized"; return; }
        if (appAnyPumpRunning()) {
          Serial.println("[ota] REFUSED: a pump is running — stop pumps/schedules first");
          appOtaRefuseMsg = "a pump is running — turn pumps and pump schedules off first";
          return;
        }
        Serial.printf("[ota] receiving firmware image (%u bytes)\n", (unsigned)total);
        // A previous upload that died mid-flight leaves Update running; begin()
        // would fail and later chunks would then append this image's bytes into
        // the stale session — end(true) could boot that mix. Start clean.
        if (Update.isRunning()) Update.abort();
        if (!Update.begin(total)) {
          Update.printError(Serial);
          appOtaRefuseMsg = "could not start update — see serial log";
          return;
        }
      }
      if (appOtaRefuseMsg || Update.hasError()) return;
      if (Update.write(data, len) != len) { Update.printError(Serial); return; }
      if (index + len == total) {
        if (Update.end(true)) Serial.printf("[ota] image verified (%u bytes) — reboot pending\n", (unsigned)total);
        else Update.printError(Serial);
      }
    });

  appServer.onNotFound([](AsyncWebServerRequest* request) {
    appHttpSendJson(request, 404, appBuildAck("unknown", false, "no such endpoint"));
  });
}

// ============================================================
//  DEFERRED WI-FI WORK  (loop() only)
// ============================================================
inline void appServiceWifiScan() {
  // The scan is ASYNC on purpose: the old blocking WiFi.scanNetworks()
  // froze loop() for seconds (HTTP polls timed out, so the app demoted
  // off Wi-Fi mid-scan) and a blocking scan with the STA connected often
  // aborts with zero results. Async keeps the link and the loop alive.
  static bool scanRunning = false;
  static uint16_t scanConn = 0;

  if (_appwifi::pendingScan && !scanRunning) {
    _appwifi::pendingScan = false;
    scanConn = _appwifi::scanRequesterConn;
    if (scanConn != APP_SCAN_HTTP) {
      AppSession* s = appSessionByHandle(scanConn);
      if (!s || !s->authed) return;
    }
    Serial.println("[wifi] scan started (async — STA link stays up)");
    if (_appwifi::phase == APPWIFI_UNCONFIGURED) WiFi.mode(WIFI_STA);
    // 120ms per channel (default 300ms): the radio hops off the AP's
    // channel only briefly per step, so the association — and the HTTP
    // transport riding on it — survives the sweep, the way phones scan
    // while staying connected.
    WiFi.scanNetworks(true /*async*/, false /*hidden*/, false /*passive*/, 120 /*ms per channel*/);
    scanRunning = true;
    return;
  }
  if (!scanRunning) return;

  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return; // still working — check again next pass
  static bool retriedFail = false;
  if (n < 0 && !retriedFail) {
    // The first async attempt right after a mode change / while the STA is
    // busy can fail outright — retry once instead of reporting nothing.
    retriedFail = true;
    Serial.println("[wifi] scan failed — retrying once");
    WiFi.scanNetworks(true, false, false, 120);
    return; // scanRunning stays true
  }
  retriedFail = false;
  scanRunning = false;
  if (n < 0) n = 0;
  Serial.printf("[wifi] %d networks found\n", n);
  DynamicJsonDocument doc(2048);
  doc["type"] = "wifiScan";
  JsonArray arr = doc.createNestedArray("networks");
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
  // Cached for HTTP pollers (GET /wifiScan).
  appLastScanJson = out;
  appScanFresh = true;
  if (scanConn != APP_SCAN_HTTP) {
    AppSession* s = appSessionByHandle(scanConn);
    if (s && s->authed) {
      // One SMALL single-notify frame per network plus an end marker —
      // the old single multi-chunk push was torn by radio contention
      // often enough that the list regularly arrived empty and the user
      // had to mash Rescan. Small frames survive (same fix as pump events).
      int i = 0;
      for (JsonObject net : arr) {
        StaticJsonDocument<224> nd;
        nd["type"] = "wifiNet";
        nd["i"] = i++;
        nd["count"] = limit;
        nd["ssid"] = net["ssid"];
        nd["rssi"] = net["rssi"];
        nd["secure"] = net["secure"];
        String o;
        serializeJson(nd, o);
        appSendChunkedTo(s, o);
      }
      StaticJsonDocument<96> fin;
      fin["type"] = "wifiScanEnd";
      fin["count"] = limit;
      String fo;
      serializeJson(fin, fo);
      appSendChunkedTo(s, fo);
    }
  }
}

inline void appWifiTick() {
  // Queued work from BLE callbacks first (NVS + radio stay on this task).
  if (_appwifi::pendingForget) {
    _appwifi::pendingForget = false;
    appWifiClearCreds();
    Serial.println("[cmd] forgetWifi: credentials erased, Wi-Fi off — announcing over BLE");
    appWifiAnnounceStart();
    markDirty();
  }
  if (_appwifi::pendingSave) {
    _appwifi::pendingSave = false;
    appWifiSaveCreds();
  }
  if (_appwifi::pendingJoin) {
    _appwifi::pendingJoin = false;
    appWifiBeginJoin();
    markDirty();
  }
  appServiceWifiScan();

  if (!_appwifi::configured) return;
  wl_status_t st = WiFi.status();

  if (_appwifi::phase == APPWIFI_CONNECTING) {
    if (st == WL_CONNECTED) {
      _appwifi::phase = APPWIFI_ONLINE;
      Serial.printf("[wifi] online as %s — announcing over BLE\n", WiFi.localIP().toString().c_str());
      if (!appServerStarted) {
        appServer.begin();
        appServerStarted = true;
        Serial.println("[http] app API listening on :80");
      }
      appWifiAnnounceStart();
      markDirty();
      return;
    }
    if (millis() - _appwifi::attemptStartMs > APP_WIFI_JOIN_TIMEOUT_MS) {
      Serial.println("[wifi] join timed out — announcing over BLE");
      WiFi.disconnect();
      _appwifi::joinFailed = true;
      _appwifi::phase = APPWIFI_OFFLINE;
      _appwifi::lastRetryMs = millis();
      appWifiAnnounceStart();
      markDirty();
    }
    return;
  }

  if (_appwifi::phase == APPWIFI_ONLINE && st != WL_CONNECTED) {
    Serial.println("[wifi] connection lost");
    _appwifi::phase = APPWIFI_OFFLINE;
    _appwifi::lastRetryMs = millis();
    markDirty();
    return;
  }

  // Background rejoin. Each attempt is ~20s of radio contention that can
  // starve an active BLE link — back way off while a phone is connected.
  uint32_t retryInterval = appActiveCount() > 0 ? APP_WIFI_RETRY_BLE_MS : APP_WIFI_RETRY_MS;
  if (_appwifi::phase == APPWIFI_OFFLINE && millis() - _appwifi::lastRetryMs > retryInterval) {
    _appwifi::lastRetryMs = millis();
    appWifiBeginJoin();
  }
}

// ============================================================
//  QR PAYLOAD
// ============================================================
inline void appPrintQrPayload() {
  StaticJsonDocument<128> doc;
  doc["v"] = 2;
  doc["name"] = appBleName;
  doc["svc"] = APP_SERVICE_UUID;
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
//  PUBLIC ENTRY POINTS
// ============================================================
inline void appLinkBegin() {
  appDeriveIdentity();
  appTxMutex = xSemaphoreCreateMutex();

  // Stored credentials join from loop() via the pending flag, so setup()
  // never blocks on a router that isn't there.
  appWifiLoadCreds();
  if (_appwifi::configured) {
    Serial.printf("[wifi] stored network: \"%s\"\n", _appwifi::ssid);
    _appwifi::pendingJoin = true;
  } else {
    Serial.println("[wifi] no credentials stored — provision from the app over BLE");
  }

  appHttpRegister(); // routes ready; server begins once Wi-Fi is online

  NimBLEDevice::init(appBleName);
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setSecurityAuth(true /*bond*/, false /*MITM*/, true /*SC*/);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  appBleServer = NimBLEDevice::createServer();
  appBleServer->setCallbacks(new AppServerCallbacks());
  NimBLEService* svc = appBleServer->createService(APP_SERVICE_UUID);
#if APP_REQUIRE_LINK_ENC
  appTxChar = svc->createCharacteristic(APP_CHAR_TX_UUID, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC);
  appRxChar = svc->createCharacteristic(APP_CHAR_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
#else
  appTxChar = svc->createCharacteristic(APP_CHAR_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  appRxChar = svc->createCharacteristic(APP_CHAR_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
#endif
  appRxChar->setCallbacks(new AppRxCallbacks());
  svc->start();

  // Flags(3B) + 128-bit UUID(18B) + name(17B) don't fit one 31-byte
  // advertisement — split: UUID in the advertisement (what the app's
  // scan filter matches), name in the scan response (what scanners show).
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(NimBLEUUID(APP_SERVICE_UUID));
  adv->setAdvertisementData(advData);
  NimBLEAdvertisementData scanData;
  scanData.setName(appBleName);
  adv->setScanResponseData(scanData);
  adv->enableScanResponse(true);
  if (adv->start()) Serial.printf("[ble] advertising as \"%s\"\n", appBleName);
  else Serial.println("!!! BLE advertising FAILED to start — device is invisible");

  Serial.printf("[app] device MAC (identity): %s\n", appDeviceMac);

  // Record the running firmware version in NVS ("eeprom") — the first
  // boot after an OTA logs the transition, and the stored value survives
  // for diagnostics even if a later image fails to report properly.
  {
    _state::prefs.begin("relaytimer", false);
    String prev = _state::prefs.getString("fwver", "");
    if (prev != FW_VERSION) {
      Serial.printf("[ota] firmware version: %s -> %s\n",
                    prev.length() ? prev.c_str() : "(first boot)", FW_VERSION);
      _state::prefs.putString("fwver", FW_VERSION);
    } else {
      Serial.printf("[app] firmware version %s\n", FW_VERSION);
    }
    _state::prefs.end();
  }

  appPrintQrPayload();
}

inline void appLinkLoop() {
  static uint32_t lastBroadcastVersion = 0;
  static unsigned long lastBroadcastMs = 0;
  static unsigned long lastHeartbeatMs = 0;
  unsigned long nowMs = millis();

  // A verified OTA image reboots from here, after its HTTP response left.
  // Signed diff: `nowMs > appRebootAtMs` misfires when the deadline was set
  // just before the 49.7-day millis() wrap (premature reboot mid-response).
  if (appRebootAtMs && (int32_t)(nowMs - appRebootAtMs) >= 0) {
    Serial.println("[ota] rebooting into the new firmware");
    delay(50);
    ESP.restart();
  }

  // Auth deadline: a connection that never completes the HMAC challenge
  // may not camp on a session slot. Only REQUEST the disconnect from here:
  // freeing the session (its String rx buffer) must stay on the NimBLE
  // task, where onWrite may be appending to that buffer right now —
  // onDisconnect does the actual appFreeSession.
  for (int i = 0; i < APP_MAX_SESSIONS; i++) {
    AppSession* s = &appSessions[i];
    if (s->inUse && !s->authed && !s->dropPending && nowMs - s->connectedAtMs > APP_AUTH_TIMEOUT_MS) {
      Serial.printf("[auth] conn %u dropped: auth timeout\n", s->connHandle);
      s->dropPending = true;
      if (!appBleServer || !appBleServer->disconnect(s->connHandle))
        appFreeSession(s->connHandle); // no live link — nothing can be writing
    }
  }

  appWifiTick();

  // Repeat any pending Wi-Fi verdict announce (single-notify frames).
  if (millis() < _appwifi::announceUntilMs && millis() - _appwifi::lastAnnounceMs > 2000) {
    _appwifi::lastAnnounceMs = millis();
    appBroadcastWifiEvent();
  }

  // Pump confirmations ride a small dedicated frame (see
  // appBroadcastPumpEvent) — detected here by diffing the dispatcher's
  // confirmed state against the last snapshot sent.
  {
    static bool     lastPumpActive[MAX_PUMPS];
    static uint16_t lastPumpSpeed[MAX_PUMPS];
    static int8_t   lastPumpName[MAX_PUMPS];
    static unsigned long pumpRetryAtMs[MAX_PUMPS] = {0, 0, 0, 0};
    static bool     pumpSnapInit = false;
    for (int i = 1; i <= MAX_PUMPS; i++) {
      bool a; uint16_t sp;
      pumpActualStateGet(i, a, sp);
      int8_t nameIdx = pumpNameIndexGet(i);
      if (!pumpSnapInit) {
        lastPumpActive[i - 1] = a;
        lastPumpSpeed[i - 1] = sp;
        lastPumpName[i - 1] = nameIdx;
        continue;
      }
      bool changed = (a != lastPumpActive[i - 1] || sp != lastPumpSpeed[i - 1] ||
                      nameIdx != lastPumpName[i - 1]);
      if (!changed) continue;
      if (nowMs < pumpRetryAtMs[i - 1]) continue; // last notify dropped — brief backoff
      if (appBroadcastPumpEvent(i, a, sp)) {
        // Consumed only on successful delivery to every authed session.
        lastPumpActive[i - 1] = a;
        lastPumpSpeed[i - 1] = sp;
        lastPumpName[i - 1] = nameIdx;
        pumpRetryAtMs[i - 1] = 0;
      } else {
        // Dropped by the congested radio — keep the change queued so it
        // re-fires until it lands. Consuming it here left BLE phones
        // stuck on "Turning on…" while the pump was confirmed running.
        pumpRetryAtMs[i - 1] = nowMs + 250;
      }
    }
    pumpSnapInit = true;
  }

  // Push state to BLE phones when it changes (the same version counter
  // the OLED redraws on), debounced; plus a 5s heartbeat so a dropped
  // notify can't leave a phone stale.
  if (appAuthedCount() > 0) {
    uint32_t v = stateVersionGet();
    bool versionDue = (v != lastBroadcastVersion) && (nowMs - lastBroadcastMs > 250);
    bool heartbeatDue = nowMs - lastHeartbeatMs > 5000;
    if (versionDue || heartbeatDue) {
      lastBroadcastVersion = v;
      lastBroadcastMs = nowMs;
      lastHeartbeatMs = nowMs;
      appBroadcastState();
    }
  }
}
