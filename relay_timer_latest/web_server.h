#pragma once

#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "shared_state.h"
#include "wifi_setup.h"
#include "web_ui.h"
#include "rs485_pumps.h"
#include "rtc_clock.h"

AsyncWebServer server(80);

// ---------------- Countdown helper ----------------
struct CountdownInfo {
  bool active;
  int  minsLeft;
  int  minsUntilNext;
};

static CountdownInfo computeCountdown(uint8_t target, int curMin, Preset *snapshot, int count) {
  CountdownInfo info{false, 0, -1};
  int bestUntilNext = 1441;

  for (int i = 0; i < count; i++) {
    Preset &p = snapshot[i];
    if (!p.used || !p.enabled || p.target != target) continue;
    
    int durMin = (int)p.durHour * 60 + p.durMinute;
    if (durMin <= 0) continue;
    int startMin = (int)p.startHour * 60 + p.startMinute;
    int endMin = startMin + durMin;

    if (isWithinSchedule(p, curMin)) {
      int effectiveEnd = (endMin <= 1440) ? endMin : (endMin - 1440);
      int remaining;
      if (endMin <= 1440) {
        remaining = endMin - curMin;
      } else {
        if (curMin >= startMin) remaining = (1440 - curMin) + effectiveEnd;
        else remaining = effectiveEnd - curMin;
      }
      if (!info.active || remaining > info.minsLeft) {
        info.minsLeft = remaining;
      }
      info.active = true;
    } else {
      int untilNext;
      if (startMin >= curMin) untilNext = startMin - curMin;
      else untilNext = (1440 - curMin) + startMin;
      if (untilNext < bestUntilNext) bestUntilNext = untilNext;
    }
  }

  if (!info.active && bestUntilNext <= 1440) info.minsUntilNext = bestUntilNext;
  return info;
}

// ---------------- /api/status ----------------
static void handleApiStatus(AsyncWebServerRequest *request) {
  uint8_t h, m, s;
  clockGet(h, m, s);
  bool r1on, r2on, r3on;
  relayActualStatesGet(r1on, r2on, r3on); // <-- Updated
  int curMin = (int)h * 60 + m;
  

  Preset snapshot[MAX_PRESETS];
  for (int i = 0; i < MAX_PRESETS; i++) presetGet(i, snapshot[i]);

  DynamicJsonDocument doc(4096);
  doc["hour"] = h;
  doc["minute"] = m;
  doc["second"] = s;
  doc["timeWasSet"] = clockWasSet();
  doc["timeSourceNTP"] = clockSourceIsNTP();
  doc["ip"] = wifiIsAPFallback ? String(wifiStatusIP) + " (setup AP)" : String(wifiStatusIP);

  doc["relay1"] = r1on;
  doc["relay2"] = r2on;
  doc["relay3"] = r3on; // <-- New

  const char* modeStrs[3] = {"auto", "on", "off"};
  doc["relay1Mode"] = modeStrs[(int)relayModeGet(1)];
  doc["relay2Mode"] = modeStrs[(int)relayModeGet(2)];
  doc["relay3Mode"] = modeStrs[(int)relayModeGet(3)]; // <-- New
  doc["relay1NameIndex"] = relayNameIndexGet(1);
  doc["relay2NameIndex"] = relayNameIndexGet(2);
  doc["relay3NameIndex"] = relayNameIndexGet(3);
  doc["r2Depends"] = relay2DependencyGet();

  for(int i = 0; i < 7; i++) {
    CountdownInfo cd = computeCountdown(i, curMin, snapshot, MAX_PRESETS);
    JsonObject cObj = doc.createNestedObject(String("cd") + String(i));
    cObj["active"] = cd.active;
    cObj["minsLeft"] = cd.minsLeft;
    cObj["minsUntilNext"] = cd.minsUntilNext;
  }

  const char* pumpModeStrs[3] = {"auto", "on", "off"};
  JsonArray pumpsArr = doc.createNestedArray("pumps");
  for (int i = 0; i < MAX_PUMPS; i++) {
    JsonObject po = pumpsArr.createNestedObject();
    po["mode"] = pumpModeStrs[(int)pumpModeGet(i + 1)];
    bool active; uint16_t curSpeed;
    pumpActualStateGet(i + 1, active, curSpeed);
    po["active"] = active;
    po["speed"] = curSpeed;
    po["nameIndex"] = pumpNameIndexGet(i + 1); // -1 = unassigned (default "PUMPn")
  }

  JsonArray arr = doc.createNestedArray("presets");
  for (int i = 0; i < MAX_PRESETS; i++) {
    JsonObject po = arr.createNestedObject();
    po["used"] = snapshot[i].used;
    po["enabled"] = snapshot[i].enabled;
    po["target"] = snapshot[i].target;
    po["pumpType"] = snapshot[i].pumpType;
    po["speed"] = snapshot[i].speed;
    po["startHour"] = snapshot[i].startHour;
    po["startMinute"] = snapshot[i].startMinute;
    po["durHour"] = snapshot[i].durHour;
    po["durMinute"] = snapshot[i].durMinute;
  }

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

// ---------------- /api/manual ----------------
static void handleApiManualBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  int relay = doc["relay"] | 0;
  const char* modeStr = doc["mode"] | "";

 if (relay < 1 || relay > 3) {
    request->send(400, "application/json", "{\"error\":\"relay must be 1, 2, or 3\"}");
    return;
  }
  RelayMode mode;
  if (strcmp(modeStr, "auto") == 0) mode = MODE_AUTO;
  else if (strcmp(modeStr, "on") == 0) mode = MODE_ON;
  else if (strcmp(modeStr, "off") == 0) mode = MODE_OFF;
  else {
    request->send(400, "application/json", "{\"error\":\"mode must be auto/on/off\"}");
    return;
  }

  relayModeSet(relay, mode);
  request->send(200, "application/json", "{\"ok\":true}");
}

// ---------------- /api/pumpmanual ----------------
static void handleApiPumpManualBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  int pump = doc["pump"] | 0;
  const char* modeStr = doc["mode"] | "";

  if (pump < 1 || pump > MAX_PUMPS) {
    request->send(400, "application/json", "{\"error\":\"pump must be 1-4\"}");
    return;
  }

  RelayMode mode;
  if (strcmp(modeStr, "auto") == 0) mode = MODE_AUTO;
  else if (strcmp(modeStr, "on") == 0) mode = MODE_ON;
  else if (strcmp(modeStr, "off") == 0) mode = MODE_OFF;
  else {
    request->send(400, "application/json", "{\"error\":\"mode must be auto/on/off\"}");
    return;
  }

  if (mode == MODE_ON) {
    int pumpType = doc["pumpType"] | 0;
    int speed = doc["speed"] | 0;
    if (pumpType != PUMP_PENTAIR && pumpType != PUMP_EMAUX && pumpType != PUMP_BLACKDECKER) {
      request->send(400, "application/json", "{\"error\":\"invalid pumpType\"}");
      return;
    }
    // Black & Decker is pump-1-only: the pump is fixed at Modbus address 1.
    if (pumpType == PUMP_BLACKDECKER && pump != 1) {
      request->send(400, "application/json", "{\"error\":\"Black & Decker is only supported on Pump 1\"}");
      return;
    }
    int minRpm = (pumpType == PUMP_PENTAIR) ? 450 : (pumpType == PUMP_BLACKDECKER) ? 600 : 800;
    int maxRpm = (pumpType == PUMP_EMAUX) ? 3400 : 3450;
    if (speed < minRpm || speed > maxRpm) {
      request->send(400, "application/json",
        String("{\"error\":\"speed must be ") + minRpm + "-" + maxRpm + " RPM for this pump type\"}");
      return;
    }
    pumpManualParamsSet(pump, (uint16_t)speed, (uint8_t)pumpType);
  }

  pumpModeSet(pump, mode);
  request->send(200, "application/json", "{\"ok\":true}");
}

// ---------------- /api/pumpname ----------------
static void handleApiPumpNameBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  int pump = doc["pump"] | 0;
  int nameIndex = doc["nameIndex"] | -1;

  if (pump < 1 || pump > MAX_PUMPS) {
    request->send(400, "application/json", "{\"error\":\"pump must be 1-4\"}");
    return;
  }
  if (nameIndex < -1 || nameIndex >= NAME_OPTION_COUNT) {
    request->send(400, "application/json", "{\"error\":\"invalid nameIndex\"}");
    return;
  }
  if (nameIndex >= 0 && !nameAllowedForPump((int8_t)nameIndex)) {
    request->send(400, "application/json", "{\"error\":\"that name is reserved for relays\"}");
    return;
  }
  if (!pumpNameIndexSet(pump, (int8_t)nameIndex)) {
    request->send(409, "application/json", "{\"error\":\"that name is already assigned to another pump or relay\"}");
    return;
  }
  request->send(200, "application/json", "{\"ok\":true}");
}

// ---------------- /api/relayname ----------------
static void handleApiRelayNameBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  DynamicJsonDocument doc(128);
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  int relay = doc["relay"] | 0;
  int nameIndex = doc["nameIndex"] | -1;

  if (relay < 1 || relay > MAX_RELAYS) {
    request->send(400, "application/json", "{\"error\":\"relay must be 1-3\"}");
    return;
  }
  if (nameIndex < -1 || nameIndex >= NAME_OPTION_COUNT) {
    request->send(400, "application/json", "{\"error\":\"invalid nameIndex\"}");
    return;
  }
  if (!relayNameIndexSet(relay, (int8_t)nameIndex)) {
    request->send(409, "application/json", "{\"error\":\"that name is already assigned to another relay or pump\"}");
    return;
  }
  request->send(200, "application/json", "{\"ok\":true}");
}

// ---------------- /api/settime ----------------
static void handleApiSetTimeBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  int h = doc["hour"] | -1;
  int m = doc["minute"] | -1;
  if (h < 0 || h > 23 || m < 0 || m > 59) {
    request->send(400, "application/json", "{\"error\":\"hour 0-23, minute 0-59\"}");
    return;
  }
  setDeviceTime((uint8_t)h, (uint8_t)m);
  request->send(200, "application/json", "{\"ok\":true}");
}

// ---------------- /api/preset ----------------
static void handleApiPresetBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  const char* action = doc["action"] | "";

  if (strcmp(action, "add") == 0 || strcmp(action, "update") == 0) {
    int target = doc["target"] | 0;
    int pType = doc["pumpType"] | 0;
    int speed = doc["speed"] | 2000;
    int sh = doc["startHour"] | 0;
    int sm = doc["startMinute"] | 0;
    int dh = doc["durHour"] | 0;
    int dm = doc["durMinute"] | 0;
    bool en = doc["enabled"] | true;

    if (target < 0 || target > 6) { request->send(400, "application/json", "{\"error\":\"invalid target\"}"); return; }
    if (pType < PUMP_PENTAIR || pType > PUMP_BLACKDECKER) { request->send(400, "application/json", "{\"error\":\"invalid pumpType\"}"); return; }
    // Black & Decker schedules may only target Pump 1 (target index 3).
    if (pType == PUMP_BLACKDECKER && target != 3) { request->send(400, "application/json", "{\"error\":\"Black & Decker is only supported on Pump 1\"}"); return; }
    if (sh < 0 || sh > 23 || sm < 0 || sm > 59) { request->send(400, "application/json", "{\"error\":\"invalid start time\"}"); return; }
    if (dh < 0 || dh > 23 || dm < 0 || dm > 59) { request->send(400, "application/json", "{\"error\":\"invalid duration\"}"); return; }
    if (dh == 0 && dm == 0) { request->send(400, "application/json", "{\"error\":\"duration must be > 0\"}"); return; }

    int slot = -1;
    if (strcmp(action, "add") == 0) {
      slot = presetFindFreeSlot();
      if (slot < 0) { request->send(409, "application/json", "{\"error\":\"presets full (8/8)\"}"); return; }
    } else {
      int visIdx = doc["index"] | -1;
      slot = presetSlotForVisibleIndex(visIdx);
      if (slot < 0) { request->send(404, "application/json", "{\"error\":\"preset not found\"}"); return; }
    }

    Preset p;
    p.used = true; p.enabled = en; 
    p.target = (uint8_t)target;
    p.pumpType = (uint8_t)pType;
    p.speed = (uint16_t)speed;
    p.startHour = (uint8_t)sh; p.startMinute = (uint8_t)sm;
    p.durHour = (uint8_t)dh; p.durMinute = (uint8_t)dm;
    
    presetWrite(slot, p);
    request->send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (strcmp(action, "toggle") == 0) {
    int visIdx = doc["index"] | -1;
    bool en = doc["enabled"] | true;
    int slot = presetSlotForVisibleIndex(visIdx);
    if (slot < 0) { request->send(404, "application/json", "{\"error\":\"preset not found\"}"); return; }
    presetSetEnabled(slot, en);
    request->send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (strcmp(action, "delete") == 0) {
    int visIdx = doc["index"] | -1;
    int slot = presetSlotForVisibleIndex(visIdx);
    if (slot < 0) { request->send(404, "application/json", "{\"error\":\"preset not found\"}"); return; }
    presetDelete(slot);
    request->send(200, "application/json", "{\"ok\":true}");
    return;
  }

  request->send(400, "application/json", "{\"error\":\"unknown action\"}");
}

template<void(*HANDLER)(AsyncWebServerRequest*, uint8_t*, size_t)>
static void bodyHandlerWrapper(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                                size_t index, size_t total) {
  static uint8_t buf[1024];
  if (total > sizeof(buf)) {
    request->send(413, "application/json", "{\"error\":\"payload too large\"}");
    return;
  }
  memcpy(buf + index, data, len);
  if (index + len == total) {
    HANDLER(request, buf, total);
  }
}


// ---------------- /api/config ----------------
static void handleApiConfigBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  if (doc.containsKey("r2Depends")) {
    relay2DependencySet(doc["r2Depends"].as<bool>());
  }
  request->send(200, "application/json", "{\"ok\":true}");
}
// Add this helper near the top of web_server.h
class CaptiveRequestHandler : public AsyncWebHandler {
public:
  CaptiveRequestHandler() {}
  virtual ~CaptiveRequestHandler() {}
  bool canHandle(AsyncWebServerRequest *request) { return true; } // Catch everything
  void handleRequest(AsyncWebServerRequest *request) {
    request->redirect("/");
  }
};

// Replace your webServerBegin() function with this:
inline void webServerBegin() {
  if (wifiIsAPFallback) {
    // ---- AP MODE ROUTES (Wi-Fi Setup) ----
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", SETUP_HTML);
    });

    server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
      int n = WiFi.scanNetworks();
      DynamicJsonDocument doc(2048);
      JsonArray arr = doc.to<JsonArray>();
      for (int i = 0; i < n; ++i) {
        JsonObject net = arr.createNestedObject();
        net["ssid"] = WiFi.SSID(i);
        net["rssi"] = WiFi.RSSI(i);
      }
      String out;
      serializeJson(doc, out);
      request->send(200, "application/json", out);
    });

    server.on("/api/save_wifi", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, 
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        DynamicJsonDocument doc(256);
        deserializeJson(doc, data, len);
        String s = doc["ssid"] | "";
        String p = doc["pass"] | "";
        if(s != "") saveWiFiCreds(s, p);
        request->send(200, "application/json", "{\"ok\":true}");
        delay(1000);
        ESP.restart(); // Reboot to apply new credentials
    });

    // Captive Portal Redirect (Forces phones to open the page automatically)
    server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);

  } else {
    // ---- STATION MODE ROUTES (Normal Operation) ----
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", INDEX_HTML);
    });
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/manual", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, bodyHandlerWrapper<handleApiManualBody>);
    server.on("/api/pumpmanual", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, bodyHandlerWrapper<handleApiPumpManualBody>);
    server.on("/api/pumpname", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, bodyHandlerWrapper<handleApiPumpNameBody>);
    server.on("/api/relayname", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, bodyHandlerWrapper<handleApiRelayNameBody>);
    server.on("/api/settime", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, bodyHandlerWrapper<handleApiSetTimeBody>);
    server.on("/api/preset", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, bodyHandlerWrapper<handleApiPresetBody>);
    server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *request) {}, nullptr, bodyHandlerWrapper<handleApiConfigBody>);
    
    server.onNotFound([](AsyncWebServerRequest *request) {
      request->send(404, "text/plain", "Not found");
    });
  }

  server.begin();
  Serial.println("Web server started");
}
