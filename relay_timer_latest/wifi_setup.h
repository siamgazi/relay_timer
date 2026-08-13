#pragma once
#include <WiFi.h>
#include "shared_state.h"

#define AP_FALLBACK_SSID      "RelayTimer-Setup"
#define AP_FALLBACK_PASSWORD  "relaytimer"
#define STA_TIMEOUT_MS        15000 // 15 seconds to try connecting

char wifiStatusIP[16] = "0.0.0.0";
bool wifiIsAPFallback = false;

// Async State Trackers
bool _wifiIsConnecting = false;
bool _wifiIsDone = false;
unsigned long _wifiStartAttempt = 0;

inline void saveWiFiCreds(String ssid, String pass) {
  _state::prefs.begin("wifi", false);
  _state::prefs.putString("ssid", ssid);
  _state::prefs.putString("pass", pass);
  _state::prefs.end();
}

inline void loadWiFiCreds(String &ssid, String &pass) {
  _state::prefs.begin("wifi", true);
   ssid = _state::prefs.getString("ssid", "TP-Link_0DFF");
    pass = _state::prefs.getString("pass", "16025769");
  _state::prefs.end();
}

inline void wifiSetupBegin() {
  WiFi.disconnect(true);
  delay(100); // Tiny pause strictly for hardware radio reset

  String savedSSID, savedPass;
  loadWiFiCreds(savedSSID, savedPass);

  Serial.println("Attempting to connect to: " + savedSSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());

  // Set flags to let the loop know we are waiting in the background
  _wifiStartAttempt = millis();
  _wifiIsConnecting = true;
  _wifiIsDone = false;
}

// This function now returns TRUE on the exact millisecond the connection finishes
inline bool wifiSetupLoop() {
  if (_wifiIsDone || !_wifiIsConnecting) return false;

  // 1. Did we successfully connect?
  if (WiFi.status() == WL_CONNECTED) {
    wifiIsAPFallback = false;
    strncpy(wifiStatusIP, WiFi.localIP().toString().c_str(), sizeof(wifiStatusIP) - 1);
    Serial.println("Connected! IP: " + String(wifiStatusIP));
    _wifiIsConnecting = false;
    _wifiIsDone = true;
    markDirty();
    return true; // Tell the main code to start the web server
  }

  // 2. Did we run out of time? (15 seconds passed)
  if (millis() - _wifiStartAttempt >= STA_TIMEOUT_MS) {
    Serial.println("Connection failed. Starting AP Mode...");
    WiFi.disconnect(true);
    delay(150);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_FALLBACK_SSID, AP_FALLBACK_PASSWORD);
    wifiIsAPFallback = true;
    strncpy(wifiStatusIP, WiFi.softAPIP().toString().c_str(), sizeof(wifiStatusIP) - 1);
    Serial.println("AP Mode Active. IP: " + String(wifiStatusIP));
    
    _wifiIsConnecting = false;
    _wifiIsDone = true;
    markDirty();
    return true; // Tell the main code to start the web server
  }

  // Still trying to connect, continue running the rest of the ESP32 code!
  return false; 
}
