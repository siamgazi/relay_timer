// Firmware version — bump on EVERY release. The app compares this (as
// reported by the running device) against the GitHub manifest to decide
// whether to offer an OTA update, and the manifest's "version" must match
// this string exactly or the update loop never settles.
#define FW_VERSION "1.0.3"

#include <Wire.h>
#include <Adafruit_GFX.h>

#include "shared_state.h"
#include "rs485_pumps.h" // Includes the new RS485 logic
#include "rtc_clock.h"   // DS3231 real-time clock
// Phone-app connectivity: BLE + HMAC auth + app protocol + BLE-provisioned
// Wi-Fi + the HMAC-authenticated HTTP API. Replaces the old wifi_setup.h
// (AP-fallback hotspot) and web_server.h/web_ui.h (unauthenticated browser
// UI) — the app and the OLED buttons are now the only interfaces.
#include "app_link.h"

// ---------------- USER CONFIG ----------------
#define DISPLAY_TYPE   false // false for SH110X, true for SSD1306

#if DISPLAY_TYPE
  #include <Adafruit_SSD1306.h>
#else
  #include <Adafruit_SH110X.h>
#endif

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_ADDR      0x3C

  #define PIN_UP     14
  #define PIN_ENTER  16
  #define PIN_DOWN   21
  #define PIN_BACK   15

  #define PIN_RELAY1 6
  #define PIN_RELAY2 7
  #define PIN_RELAY3 2 // <-- New Independent Relay Pin

 

#define RELAY1_ACTIVE_HIGH false
#define RELAY2_ACTIVE_HIGH false
#define RELAY3_ACTIVE_HIGH false // <-- New  

#define IDLE_TIMEOUT_MS 20000UL

// ---- Freeze protection thermistor (10k NTC + 10k series divider) ----
#define THERMISTOR_PIN        5        // ADC1 pin — safe alongside Wi-Fi
#define THERM_NOMINAL_RES     10000.0f // resistance at 25°C
#define THERM_NOMINAL_TEMP_C  25.0f
#define THERM_B_COEFF         3950.0f
#define THERM_SERIES_RES      10000.0f // fixed divider resistor
// -----------------------------------------------

#if DISPLAY_TYPE
  Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
  #define COLOR_WHITE SSD1306_WHITE
  #define COLOR_BLACK SSD1306_BLACK
#else
  Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
  #define COLOR_WHITE SH110X_WHITE
  #define COLOR_BLACK SH110X_BLACK
#endif

Preset tempPreset;
int    editingIndex = -1;
bool   editingIsNew = false;

struct BtnState { uint8_t pin; bool lastReading; bool stableState; unsigned long lastChangeMs; };
BtnState btnUp    = {PIN_UP,    HIGH, HIGH, 0};
BtnState btnDown  = {PIN_DOWN,  HIGH, HIGH, 0};
BtnState btnEnter = {PIN_ENTER, HIGH, HIGH, 0};
BtnState btnBack  = {PIN_BACK,  HIGH, HIGH, 0};
#define DEBOUNCE_MS 30

enum Btn { BTN_NONE, BTN_UP, BTN_DOWN, BTN_ENTER, BTN_BACK };
bool checkPressed(BtnState &b) {
  bool reading = digitalRead(b.pin);
  if (reading != b.lastReading) {
    b.lastChangeMs = millis();
    b.lastReading = reading;
  }
  if ((millis() - b.lastChangeMs) > DEBOUNCE_MS) {
    if (reading != b.stableState) {
      b.stableState = reading;
      if (b.stableState == LOW) return true; 
    }
  }
  return false;
}

Btn getButton() {
  if (checkPressed(btnUp)) return BTN_UP;
  if (checkPressed(btnDown)) return BTN_DOWN;
  if (checkPressed(btnEnter)) return BTN_ENTER;
  if (checkPressed(btnBack)) return BTN_BACK;
  return BTN_NONE;
}

enum AppState { ST_HOME, ST_MENU, ST_PRESET_LIST, ST_PRESET_EDIT, ST_MANUAL, ST_SET_TIME, ST_FREEZE, ST_SETTINGS };
AppState state = ST_HOME;
int homePage = 0; // 0 = relay status, 1 = pump status (toggle with UP/DOWN while at Home)

const char* menuItems[] = {"View Presets", "Add Preset", "Manual Control", "Set Time", "Freeze Protection", "Settings"};
#define MENU_COUNT 6
#define MENU_VISIBLE 4 // Maximum number of items visible on screen at once
int menuIndex = 0;
int menuScroll = 0;    // NEW: Tracks the current scroll position of the menu

#define LIST_VISIBLE 4
int presetListIndex = 0;
int presetListScroll = 0;

// Preset edit field IDs
#define F_TARGET 0
#define F_PTYPE  1
#define F_SPEED  2
#define F_SH     3
#define F_SM     4
#define F_DH     5
#define F_DM     6
#define F_EN     7
#define F_SAVE   8
#define F_DELETE 9
#define F_CANCEL 10

int editOrder[11];
int editOrderCount = 0;
int editFieldCursor = 0;

// ---- Manual Control screen row model ----
// The screen is a scrollable list. Rows are rebuilt from live state every
// time the screen is handled/drawn (like buildEditOrder() does for preset
// editing) so it always reflects reality -- including changes made from
// the web UI while the OLED is sitting on this screen.
#define MR_RELAY1     0
#define MR_RELAY2     1
#define MR_RELAY3     2
#define MR_PUMP_MODE  3
#define MR_PUMP_TYPE  4  // only present when that pump's mode == ON
#define MR_PUMP_SPEED 5  // only present when that pump's mode == ON
#define MR_DONE       6
#define MAX_MANUAL_ROWS (3 + MAX_PUMPS * 3 + 1)
int manualRowType[MAX_MANUAL_ROWS];
int manualRowPump[MAX_MANUAL_ROWS]; // 0-based pump index; -1 for non-pump rows
int manualRowCount = 0;
#define MANUAL_VISIBLE 4
int manualFieldCursor = 0;
int manualScroll = 0;
bool manualEditingSpeed = false; // true while UP/DOWN adjust speed instead of moving the cursor

uint8_t setHourTemp = 0, setMinuteTemp = 0;
int timeFieldCursor = 0;

// Freeze-temp edit draft: UP/DOWN change this, ENTER commits it (the
// stored value only moves on an explicit save — no accidental changes).
int16_t freezeTempDraft = 34;

unsigned long lastInteractionMs = 0;
bool needRedraw = true;
unsigned long lastDrawMs = 0;
uint32_t lastSeenStateVersion = 0;

char msgBuf[32] = "";
unsigned long msgUntil = 0;

void setup() {
  Serial.begin(115200);

  pinMode(PIN_UP, INPUT_PULLUP);
  pinMode(PIN_DOWN, INPUT_PULLUP);
  pinMode(PIN_ENTER, INPUT_PULLUP);
  pinMode(PIN_BACK, INPUT_PULLUP);
  
  pinMode(PIN_RELAY1, OUTPUT);
  pinMode(PIN_RELAY2, OUTPUT);
  pinMode(PIN_RELAY3, OUTPUT);

  analogReadResolution(12); // freeze-protection thermistor ADC (0-4095)
  
  setRelay1(false);
  setRelay2(false);
  setRelay3(false);

#if DISPLAY_TYPE
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
    delay(1000);
  }
#else
  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("SH1106 init failed");
    delay(1000);
  }
#endif

  display.setTextWrap(false);
  display.clearDisplay();
  display.display();

  loadPresetsFromFlash();
  rtcBegin();          // loads saved time from DS3231 (or seeds 12:00 if it's empty/new)
  rs485Begin();       

  // BLE service + app protocol + Wi-Fi link. Loads stored credentials
  // and queues the join for loop() — does NOT wait on a router here.
  appLinkBegin();

  lastInteractionMs = millis();
  lastSeenStateVersion = stateVersionGet();
}

void loop() {
  static unsigned long lastTickMs = millis();
  
  while (millis() - lastTickMs >= 1000) {
    lastTickMs += 1000;
    clockTickOneSecond();
  }

  rtcResync(); // no-op most ticks; periodically re-aligns against the DS3231
  rtcServicePendingWrite(); // flushes an app setTime's RTC write on the loop task

  freezeTick(); // 1s thermistor sample + freeze-protection state machine

  // App connectivity: Wi-Fi state machine (starts the HTTP API when the
  // network comes up), BLE auth deadlines, and state broadcasts to any
  // connected phones whenever stateVersion changes.
  appLinkLoop();


  processPendingSaves();

  if (Serial.available()) handleSerialCommand();

  Btn b = getButton();
  if (b != BTN_NONE) {
    lastInteractionMs = millis();
    needRedraw = true;
    handleButton(b);
  }

  if (state != ST_HOME && (millis() - lastInteractionMs > IDLE_TIMEOUT_MS)) {
    state = ST_HOME;
    needRedraw = true;
  }

  updateOutputs();

  uint32_t v = stateVersionGet();
  if (v != lastSeenStateVersion) {
    lastSeenStateVersion = v;
    needRedraw = true;
  }

  if (needRedraw || millis() - lastDrawMs >= 200) {
    drawScreen();
    lastDrawMs = millis();
    needRedraw = false;
  }
}

// ============================================================
//  SERIAL COMMANDS — mirror of the app's pump controls
// ============================================================
// Every command routes through the SAME shared-state mutators the app's
// BLE/HTTP handler uses (pumpManualParamsSet / pumpModeSet) and the same
// validation helpers (appSpeedValidFor, the B&D pump-1 rule) — so the
// dispatcher, confirmation rules, and fire-and-forget semantics behave
// identically no matter which side issued the command. Because
// pumpModeSet bumps the state version, a serial change also pushes to
// any connected phone, and an app change is visible here via `status`.
//
//   pump <1-4> on <rpm> [pentair|emaux|bd]   like the app's Turn on
//   pump <1-4> off                            like the app's Off
//   pump <1-4> auto                           schedule control
//   pump <1-4> type <pentair|emaux|bd>        like the type dropdown
//   pump <1-4> speed <rpm>                    like the speed field
//   status                                    all pumps, set + actual
//   debug on|off                              RS485 wire trace
//   help
int serialParsePumpType(const String &t) {
  if (t == "pentair") return PUMP_PENTAIR;
  if (t == "emaux") return PUMP_EMAUX;
  if (t == "bd" || t == "blackdecker" || t == "b&d") return PUMP_BLACKDECKER;
  return -1;
}

void serialPrintPumpStatus() {
  for (int n = 1; n <= MAX_PUMPS; n++) {
    uint16_t setSpd; uint8_t t;
    pumpManualParamsGet(n, setSpd, t);
    bool act; uint16_t actSpd;
    pumpActualStateGet(n, act, actSpd);
    Serial.printf("Pump %d: mode=%-4s type=%-8s set=%u rpm | actual: ",
                  n, modeStr(pumpModeGet(n)), pumpTypeName(t), setSpd);
    if (act) Serial.printf("ON @ %u rpm\n", actSpd);
    else Serial.println("OFF");
  }
}

void handleSerialCommand() {
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  line.toLowerCase();

  // Tokenize (max 5 tokens: pump <n> <verb> <arg> <arg>)
  String tok[5];
  int nTok = 0;
  int start = 0;
  while (nTok < 5 && start < (int)line.length()) {
    int sp = line.indexOf(' ', start);
    if (sp < 0) sp = line.length();
    if (sp > start) tok[nTok++] = line.substring(start, sp);
    start = sp + 1;
  }

  if (tok[0] == "help") {
    Serial.println("Commands: pump <1-4> on <rpm> [pentair|emaux|bd] | pump <1-4> off | pump <1-4> auto");
    Serial.println("          pump <1-4> type <pentair|emaux|bd> | pump <1-4> speed <rpm> | status | debug on/off");
    return;
  }
  if (tok[0] == "status") { serialPrintPumpStatus(); return; }
  if (tok[0] == "temp") {
    bool fa, fok; float ft;
    freezeStatusGet(fa, ft, fok);
    if (fok) Serial.printf("Temp: %.1fF (threshold %dF) — freeze protection %s\n",
                           ft, (int)freezeTempFGet(), fa ? "ACTIVE" : "idle");
    else Serial.println("Temp: SENSOR FAULT (check the thermistor divider on GPIO 5)");
    return;
  }
  if (tok[0] == "debug" && nTok >= 2) { rs485SetDebug(tok[1] == "on"); return; }

  if (tok[0] != "pump" || nTok < 3) {
    Serial.println("Unknown command — type 'help'.");
    return;
  }
  int n = tok[1].toInt();
  if (n < 1 || n > MAX_PUMPS) {
    Serial.printf("pump must be 1-%d\n", MAX_PUMPS);
    return;
  }

  uint16_t curSpeed; uint8_t curType;
  pumpManualParamsGet(n, curSpeed, curType);

  if (tok[2] == "on") {
    if (nTok < 4) { Serial.println("Usage: pump <n> on <rpm> [pentair|emaux|bd]"); return; }
    int rpm = tok[3].toInt();
    uint8_t type = curType;
    if (nTok >= 5) {
      int t = serialParsePumpType(tok[4]);
      if (t < 0) { Serial.println("type must be pentair, emaux or bd"); return; }
      type = (uint8_t)t;
    }
    // Same rules the app's setPumpConfig enforces:
    if (type == PUMP_BLACKDECKER && n != 1) {
      Serial.println("Black & Decker is only supported on Pump 1");
      return;
    }
    if (!appSpeedValidFor(type, rpm)) {
      Serial.println(appSpeedRangeMsg(type));
      return;
    }
    // Exactly the app's Turn on: params first, then the mode flip that
    // makes the dispatcher drive the pump.
    pumpManualParamsSet(n, (uint16_t)rpm, type);
    pumpModeSet(n, MODE_ON);
    Serial.printf("OK: pump %d ON at %d rpm (%s)%s\n", n, rpm, pumpTypeName(type),
                  type == PUMP_BLACKDECKER ? " — fire-and-forget" : " — waiting for pump ack");
    return;
  }
  if (tok[2] == "off")  { pumpModeSet(n, MODE_OFF);  Serial.printf("OK: pump %d OFF\n", n);  return; }
  if (tok[2] == "auto") { pumpModeSet(n, MODE_AUTO); Serial.printf("OK: pump %d AUTO\n", n); return; }

  if (tok[2] == "type" && nTok >= 4) {
    int t = serialParsePumpType(tok[3]);
    if (t < 0) { Serial.println("type must be pentair, emaux or bd"); return; }
    if (t == PUMP_BLACKDECKER && n != 1) {
      Serial.println("Black & Decker is only supported on Pump 1");
      return;
    }
    pumpManualParamsSet(n, curSpeed, (uint8_t)t);
    markDirty();
    Serial.printf("OK: pump %d type = %s\n", n, pumpTypeName((uint8_t)t));
    return;
  }
  if (tok[2] == "speed" && nTok >= 4) {
    int rpm = tok[3].toInt();
    if (!appSpeedValidFor(curType, rpm)) { Serial.println(appSpeedRangeMsg(curType)); return; }
    pumpManualParamsSet(n, (uint16_t)rpm, curType);
    markDirty();
    Serial.printf("OK: pump %d speed = %d rpm (takes effect on next ON)\n", n, rpm);
    return;
  }

  Serial.println("Unknown command — type 'help'.");
}

// ============================================================
//  OUTPUT CONTROL
// ============================================================
void writeRelay(int pin, bool on, bool activeHigh) {
  bool level = activeHigh ? on : !on;
  digitalWrite(pin, level ? HIGH : LOW);
}
void setRelay1(bool on) { writeRelay(PIN_RELAY1, on, RELAY1_ACTIVE_HIGH); }
void setRelay2(bool on) { writeRelay(PIN_RELAY2, on, RELAY2_ACTIVE_HIGH); }
void setRelay3(bool on) { writeRelay(PIN_RELAY3, on, RELAY3_ACTIVE_HIGH); }

// ============================================================
//  FREEZE PROTECTION  (10k NTC on GPIO 5)
//  Reading <= freezeTempF for 30s straight -> engage; release once the
//  temperature climbs to threshold + 2°F (hysteresis so it can't rapidly
//  cycle at the boundary). A sensor fault — implausible reading from an
//  open/shorted divider — never triggers protection and drops an active
//  one; the app shows the fault so it can't go unnoticed.
// ============================================================
float thermistorReadF() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(THERMISTOR_PIN); // average ADC noise out
  int adc = (int)(sum / 8);
  if (adc < 1) adc = 1;
  if (adc > 4094) adc = 4094; // railed = open/shorted; the range check flags it

  float resistance = THERM_SERIES_RES * (4095.0f / (float)adc - 1.0f);
  float kelvin = resistance / THERM_NOMINAL_RES;
  kelvin = log(kelvin);
  kelvin /= THERM_B_COEFF;
  kelvin += 1.0f / (THERM_NOMINAL_TEMP_C + 273.15f);
  kelvin = 1.0f / kelvin;
  return (kelvin - 273.15f) * 9.0f / 5.0f + 32.0f;
}

void freezeTick() {
  static unsigned long lastSampleMs = 0;
  static unsigned long belowSinceMs = 0;
  static bool active = false;
  if (millis() - lastSampleMs < 1000) return;
  lastSampleMs = millis();

  float tempF = thermistorReadF();
  bool ok = (tempF > -40.0f && tempF < 150.0f);
  int16_t thresh = freezeTempFGet();

  if (!ok) {
    belowSinceMs = 0;
    if (active) Serial.println("[freeze] sensor fault — protection released");
    active = false;
  } else if (tempF <= (float)thresh) {
    if (belowSinceMs == 0) belowSinceMs = millis();
    if (!active && millis() - belowSinceMs >= 30000UL) {
      active = true;
      Serial.printf("[freeze] ENGAGED: %.1fF held at/below %dF for 30s\n", tempF, thresh);
    }
  } else {
    belowSinceMs = 0;
    if (active && tempF >= (float)(thresh + 2)) {
      active = false;
      Serial.printf("[freeze] released: %.1fF reached %dF\n", tempF, thresh + 2);
    }
  }
  freezeStatusSet(active, tempF, ok);
}

void updateOutputs() {
  uint8_t ch, cm, cs; clockGet(ch, cm, cs);
  int curMin = (int)ch * 60 + cm;
  
  bool r1Wants = false, r2Wants = false, r3Wants = false;
  bool pumpWantsRun[MAX_PUMPS] = {false};
  uint16_t pumpWantsSpeed[MAX_PUMPS] = {0};
  uint8_t pumpWantsType[MAX_PUMPS] = {0};

  // No trusted time source yet (fresh boot with no RTC, or a dead RTC
  // battery's fabricated 12:00 seed): AUTO schedules must not fire against
  // a wrong wall clock — a 10:00 pump schedule would start the moment such
  // a board boots. Wants stay false; manual ON/OFF still works. The gate
  // lifts as soon as the RTC (battery-backed) or the app sets real time.
  bool timeTrusted = clockWasSet();
  for (int i = 0; timeTrusted && i < MAX_PRESETS; i++) {
    Preset p;
    if (!presetGet(i, p) || !p.used || !p.enabled) continue;

    if (isWithinSchedule(p, curMin)) {
      if (p.target == TGT_RELAY1) r1Wants = true;
      else if (p.target == TGT_RELAY2) r2Wants = true;
      else if (p.target == TGT_RELAY3) r3Wants = true;
      else if (p.target >= TGT_PUMP1 && p.target <= TGT_PUMP4) {
        int pIdx = p.target - TGT_PUMP1;
        pumpWantsRun[pIdx] = true;
        pumpWantsSpeed[pIdx] = p.speed;
        pumpWantsType[pIdx] = p.pumpType;
      }
    }
  }

  RelayMode m1 = relayModeGet(1), m2 = relayModeGet(2), m3 = relayModeGet(3);
  
  bool r1On = (m1 == MODE_ON) ? true : ((m1 == MODE_OFF) ? false : r1Wants);
  bool r2On = (m2 == MODE_ON) ? true : ((m2 == MODE_OFF) ? false : (r2Wants && (!relay2DependencyGet() || r1On)));
  bool r3On = (m3 == MODE_ON) ? true : ((m3 == MODE_OFF) ? false : r3Wants); // Completely independent

  // Freeze protection overrides EVERYTHING — manual OFF and the R2
  // dependency included: keeping water moving outranks user preference
  // for as long as the event lasts.
  bool frzActive; float frzTemp; bool frzOk;
  freezeStatusGet(frzActive, frzTemp, frzOk);
  if (frzActive) {
    if (relayFreezeGet(1)) r1On = true;
    if (relayFreezeGet(2)) r2On = true;
    if (relayFreezeGet(3)) r3On = true;
  }

  setRelay1(r1On);
  setRelay2(r2On);
  setRelay3(r3On);
  relayActualStatesPublish(r1On, r2On, r3On);

  for (int i = 0; i < MAX_PUMPS; i++) {
    int pumpNum = i + 1;
    RelayMode pMode = pumpModeGet(pumpNum);
    bool runFinal;
    uint16_t speedFinal;
    uint8_t typeFinal;

    if (pMode == MODE_ON) {
      uint16_t ms; uint8_t mt;
      pumpManualParamsGet(pumpNum, ms, mt);
      runFinal = true;
      speedFinal = ms;
      typeFinal = mt;
    } else if (pMode == MODE_OFF) {
      runFinal = false;
      speedFinal = 0;
      typeFinal = pumpWantsType[i];
    } else { // MODE_AUTO -> follow the schedule
      runFinal = pumpWantsRun[i];
      speedFinal = pumpWantsSpeed[i];
      typeFinal = pumpWantsType[i];
    }

    // Freeze protection: a protected pump runs at its freeze speed no
    // matter what mode/schedule said, driven with its configured protocol.
    if (frzActive) {
      bool fEn; uint16_t fSpd;
      pumpFreezeGet(pumpNum, fEn, fSpd);
      if (fEn && fSpd > 0) {
        uint16_t ms; uint8_t mt;
        pumpManualParamsGet(pumpNum, ms, mt);
        runFinal = true;
        speedFinal = fSpd;
        typeFinal = mt;
      }
    }

    updatePumpPhysicalState(pumpNum, typeFinal, runFinal, speedFinal);
  }
}
// ============================================================
//  BUTTON HANDLING (per screen)
// ============================================================

void handleSettings(Btn b) {
  if (b == BTN_BACK || b == BTN_ENTER) {
    state = ST_MENU;
    return;
  }
  if (b == BTN_UP || b == BTN_DOWN) {
    relay2DependencySet(!relay2DependencyGet());
  }
}

void handleFreeze(Btn b) {
  if (b == BTN_BACK) { // discard the draft — nothing changes
    state = ST_MENU;
    return;
  }
  if (b == BTN_ENTER) { // the explicit confirmation: commit + persist
    if (freezeTempDraft != freezeTempFGet()) {
      freezeTempFSet(freezeTempDraft);
      showMessage("Freeze temp saved");
    }
    state = ST_MENU;
    return;
  }
  // Same 20-60°F bounds the app enforces.
  if (b == BTN_UP   && freezeTempDraft < 60) freezeTempDraft++;
  if (b == BTN_DOWN && freezeTempDraft > 20) freezeTempDraft--;
}


void handleButton(Btn b) {
  switch (state) {
    case ST_HOME:        handleHome(b); break;
    case ST_MENU:        handleMenu(b); break;
    case ST_PRESET_LIST: handlePresetList(b); break;
    case ST_PRESET_EDIT: handlePresetEdit(b); break;
    case ST_MANUAL:      handleManual(b); break;
    case ST_SET_TIME:    handleSetTime(b); break;
    case ST_FREEZE:      handleFreeze(b); break;
    case ST_SETTINGS:    handleSettings(b); break;
  }
}


void adjustMenuScroll() {
  if (menuIndex < menuScroll) {
    menuScroll = menuIndex;
  }
  if (menuIndex >= menuScroll + MENU_VISIBLE) {
    menuScroll = menuIndex - MENU_VISIBLE + 1;
  }
}

void handleHome(Btn b) {
  if (b == BTN_ENTER) { 
    state = ST_MENU; 
    menuIndex = 0; 
    menuScroll = 0; // Reset scroll to top when entering from home
  }
  else if (b == BTN_UP || b == BTN_DOWN) {
    homePage = 1 - homePage; // toggle relay status <-> pump status
  }
}

void handleMenu(Btn b) {
  if (b == BTN_UP) {
    menuIndex = (menuIndex + MENU_COUNT - 1) % MENU_COUNT;
    adjustMenuScroll();
  }
  else if (b == BTN_DOWN) {
    menuIndex = (menuIndex + 1) % MENU_COUNT;
    adjustMenuScroll();
  }
  else if (b == BTN_BACK) {
    state = ST_HOME;
  }
  else if (b == BTN_ENTER) {
    switch (menuIndex) {
      case 0: enterPresetList(); break;
      case 1: enterAddPreset(); break;
      case 2: enterManualControl(); break;
      case 3: enterSetTime(); break;
      case 4: freezeTempDraft = freezeTempFGet(); state = ST_FREEZE; break;
      case 5: state = ST_SETTINGS; break;
    }
  }
}

void enterPresetList() {
  presetListIndex = 0;
  presetListScroll = 0;
  state = ST_PRESET_LIST;
}

void adjustScroll() {
  if (presetListIndex < presetListScroll) presetListScroll = presetListIndex;
  if (presetListIndex >= presetListScroll + LIST_VISIBLE) presetListScroll = presetListIndex - LIST_VISIBLE + 1;
}

void handlePresetList(Btn b) {
  int total = presetCountUsed();
  if (b == BTN_BACK) { state = ST_MENU; return; }
  if (total == 0) return;
  
  if (b == BTN_UP) { presetListIndex = (presetListIndex + total - 1) % total; adjustScroll(); }
  else if (b == BTN_DOWN) { presetListIndex = (presetListIndex + 1) % total; adjustScroll(); }
  else if (b == BTN_ENTER) {
    int slot = presetSlotForVisibleIndex(presetListIndex);
    if (slot >= 0) enterEditPreset(slot, false);
  }
}

void enterAddPreset() {
  int freeSlot = presetFindFreeSlot();
  if (freeSlot < 0) { showMessage("Presets full (8/8)"); return; }

  tempPreset.used = false;
  tempPreset.enabled = true;
  tempPreset.target = TGT_RELAY1; 
  tempPreset.pumpType = PUMP_PENTAIR;
  tempPreset.speed = 2000;
  tempPreset.startHour = 6;
  tempPreset.startMinute = 30;
  tempPreset.durHour = 1;
  tempPreset.durMinute = 0;

  enterEditPreset(freeSlot, true);
}

void buildEditOrder() {
  int idx = 0;
  editOrder[idx++] = F_TARGET;
  if (tempPreset.target >= TGT_PUMP1) {
    editOrder[idx++] = F_PTYPE;
    editOrder[idx++] = F_SPEED;
  }
  editOrder[idx++] = F_SH;
  editOrder[idx++] = F_SM;
  editOrder[idx++] = F_DH;
  editOrder[idx++] = F_DM;
  editOrder[idx++] = F_EN;
  editOrder[idx++] = F_SAVE;
  if (!editingIsNew) editOrder[idx++] = F_DELETE;
  editOrder[idx++] = F_CANCEL;
  editOrderCount = idx;
}

void enterEditPreset(int slot, bool isNew) {
  editingIndex = slot;
  editingIsNew = isNew;
  if (!isNew) presetGet(slot, tempPreset);
  
  buildEditOrder();
  editFieldCursor = 0;
  state = ST_PRESET_EDIT;
}

void adjustEditField(int fid, int dir) {
  switch (fid) {
    case F_TARGET:
      tempPreset.target = (tempPreset.target + 7 + dir) % 7;
      // Black & Decker is pump-1-only; moving the target off Pump 1
      // (target index 3) silently drops back to Pentair.
      if (tempPreset.target != 3 && tempPreset.pumpType == PUMP_BLACKDECKER) tempPreset.pumpType = PUMP_PENTAIR;
      buildEditOrder();
      break;
    case F_PTYPE: {
      uint8_t t = tempPreset.pumpType;
      if (t == PUMP_PENTAIR)      t = PUMP_EMAUX;
      else if (t == PUMP_EMAUX)   t = (tempPreset.target == 3) ? PUMP_BLACKDECKER : PUMP_PENTAIR; // B&D: Pump 1 only
      else                        t = PUMP_PENTAIR;
      tempPreset.pumpType = t;
      break;
    }
    case F_SPEED: 
      tempPreset.speed = constrain((int)tempPreset.speed + (dir * 50), 800, 3450); 
      break;
    case F_SH: tempPreset.startHour = (tempPreset.startHour + 24 + dir) % 24; break;
    case F_SM: tempPreset.startMinute = (tempPreset.startMinute + 60 + dir) % 60; break;
    case F_DH: tempPreset.durHour = constrain((int)tempPreset.durHour + dir, 0, 23); break;
    case F_DM: tempPreset.durMinute = (tempPreset.durMinute + 60 + dir) % 60; break;
    case F_EN: tempPreset.enabled = !tempPreset.enabled; break;
  }
}

void handlePresetEdit(Btn b) {
  if (editFieldCursor >= editOrderCount) editFieldCursor = 0; // Bounds safety check
  int fid = editOrder[editFieldCursor];

  if (b == BTN_BACK) {
    if (editFieldCursor == 0) { state = ST_PRESET_LIST; return; }
    editFieldCursor--;
    return;
  }
  if (b == BTN_UP || b == BTN_DOWN) {
    if (fid != F_SAVE && fid != F_DELETE && fid != F_CANCEL) {
      adjustEditField(fid, (b == BTN_UP) ? 1 : -1);
    }
    return;
  }
  if (b == BTN_ENTER) {
    if (fid == F_SAVE) {
      presetWrite(editingIndex, tempPreset);
      presetListIndex = 0; presetListScroll = 0;
      state = ST_PRESET_LIST;
      return;
    }
    if (fid == F_DELETE) {
      presetDelete(editingIndex);
      presetListIndex = 0; presetListScroll = 0;
      state = ST_PRESET_LIST;
      return;
    }
    if (fid == F_CANCEL) {
      state = ST_PRESET_LIST; return;
    }
    if (editFieldCursor < editOrderCount - 1) editFieldCursor++;
  }
}

void enterManualControl() {
  manualFieldCursor = 0;
  manualScroll = 0;
  manualEditingSpeed = false;
  buildManualRows();
  state = ST_MANUAL;
}

// Rebuilds the visible row list from live state. Pump type/speed rows
// only appear while that pump's mode is ON.
void buildManualRows() {
  int idx = 0;
  manualRowType[idx] = MR_RELAY1; manualRowPump[idx] = -1; idx++;
  manualRowType[idx] = MR_RELAY2; manualRowPump[idx] = -1; idx++;
  manualRowType[idx] = MR_RELAY3; manualRowPump[idx] = -1; idx++;
  for (int p = 0; p < MAX_PUMPS; p++) {
    manualRowType[idx] = MR_PUMP_MODE; manualRowPump[idx] = p; idx++;
    if (pumpModeGet(p + 1) == MODE_ON) {
      manualRowType[idx] = MR_PUMP_TYPE;  manualRowPump[idx] = p; idx++;
      manualRowType[idx] = MR_PUMP_SPEED; manualRowPump[idx] = p; idx++;
    }
  }
  manualRowType[idx] = MR_DONE; manualRowPump[idx] = -1; idx++;
  manualRowCount = idx;

  if (manualFieldCursor >= manualRowCount) manualFieldCursor = manualRowCount - 1;
  if (manualFieldCursor < 0) manualFieldCursor = 0;
  if (manualScroll > manualFieldCursor) manualScroll = manualFieldCursor;
  if (manualScroll > manualRowCount - MANUAL_VISIBLE) manualScroll = manualRowCount - MANUAL_VISIBLE;
  if (manualScroll < 0) manualScroll = 0;
}

void adjustManualScroll() {
  if (manualFieldCursor < manualScroll) manualScroll = manualFieldCursor;
  if (manualFieldCursor >= manualScroll + MANUAL_VISIBLE) manualScroll = manualFieldCursor - MANUAL_VISIBLE + 1;
}

inline int pumpRpmMin(uint8_t pumpType) { return (pumpType == PUMP_PENTAIR) ? PENTAIR_RPM_MIN : (pumpType == PUMP_BLACKDECKER) ? BD_RPM_MIN : EMAUX_RPM_MIN; }
inline int pumpRpmMax(uint8_t pumpType) { return (pumpType == PUMP_EMAUX) ? EMAUX_RPM_MAX : (pumpType == PUMP_BLACKDECKER) ? BD_RPM_MAX : PENTAIR_RPM_MAX; }
inline const char* pumpTypeName(uint8_t t) { return (t == PUMP_PENTAIR) ? "Pentair" : (t == PUMP_BLACKDECKER) ? "B&Decker" : "Emaux"; }

// Called the moment a pump is switched to MODE_ON so it has a sane speed
// (first time it's ever used, pumpManualSpeed defaults to 0, which is
// below every valid range).
void ensurePumpManualDefaults(int pumpIdx0) {
  uint16_t speed; uint8_t type;
  pumpManualParamsGet(pumpIdx0 + 1, speed, type);
  int minRpm = pumpRpmMin(type);
  if (speed < minRpm || speed > pumpRpmMax(type)) {
    pumpManualParamsSet(pumpIdx0 + 1, (uint16_t)minRpm, type);
  }
}

void handleManual(Btn b) {
  buildManualRows(); // pick up any changes made from the web UI
  int rt = manualRowType[manualFieldCursor];
  int p  = manualRowPump[manualFieldCursor];

  // ---- Speed sub-edit mode: UP/DOWN change the value, ENTER/BACK exit it ----
  if (manualEditingSpeed) {
    if (b == BTN_UP || b == BTN_DOWN) {
      uint16_t speed; uint8_t type;
      pumpManualParamsGet(p + 1, speed, type);
      int minRpm = pumpRpmMin(type), maxRpm = pumpRpmMax(type);
      int newSpeed = constrain((int)speed + ((b == BTN_UP) ? 100 : -100), minRpm, maxRpm);
      pumpManualParamsSet(p + 1, (uint16_t)newSpeed, type);
      markDirty();
    } else if (b == BTN_ENTER || b == BTN_BACK) {
      manualEditingSpeed = false;
    }
    return;
  }

  if (b == BTN_BACK) { state = ST_MENU; return; }

  if (b == BTN_UP || b == BTN_DOWN) {
    int dir = (b == BTN_UP) ? -1 : 1;
    manualFieldCursor = (manualFieldCursor + manualRowCount + dir) % manualRowCount;
    adjustManualScroll();
    return;
  }

  if (b == BTN_ENTER) {
    switch (rt) {
      case MR_RELAY1: relayModeSet(1, relayModeCycle(relayModeGet(1), 1)); break;
      case MR_RELAY2: relayModeSet(2, relayModeCycle(relayModeGet(2), 1)); break;
      case MR_RELAY3: relayModeSet(3, relayModeCycle(relayModeGet(3), 1)); break;
      case MR_PUMP_MODE: {
        RelayMode newMode = relayModeCycle(pumpModeGet(p + 1), 1);
        if (newMode == MODE_ON) ensurePumpManualDefaults(p);
        pumpModeSet(p + 1, newMode);
        break;
      }
      case MR_PUMP_TYPE: {
        uint16_t speed; uint8_t type;
        pumpManualParamsGet(p + 1, speed, type);
        uint8_t newType;
        if (type == PUMP_PENTAIR)    newType = PUMP_EMAUX;
        else if (type == PUMP_EMAUX) newType = (p == 0) ? PUMP_BLACKDECKER : PUMP_PENTAIR; // B&D: Pump 1 only
        else                         newType = PUMP_PENTAIR;
        int clamped = constrain((int)speed, pumpRpmMin(newType), pumpRpmMax(newType));
        pumpManualParamsSet(p + 1, (uint16_t)clamped, newType);
        markDirty();
        break;
      }
      case MR_PUMP_SPEED: manualEditingSpeed = true; break;
      case MR_DONE: state = ST_MENU; break;
    }
  }
}

void enterSetTime() {
  uint8_t ch, cm, cs;
  clockGet(ch, cm, cs);
  setHourTemp = ch;
  setMinuteTemp = cm;
  timeFieldCursor = 0;
  state = ST_SET_TIME;
}

void handleSetTime(Btn b) {
  if (b == BTN_BACK) {
    if (timeFieldCursor == 0) { state = ST_MENU; return; }
    timeFieldCursor--;
    return;
  }
  if (b == BTN_UP || b == BTN_DOWN) {
    int dir = (b == BTN_UP) ? 1 : -1;
    if (timeFieldCursor == 0) {
      setHourTemp = (setHourTemp + 24 + dir) % 24;
    } else if (timeFieldCursor == 1) {
      setMinuteTemp = (setMinuteTemp + 60 + dir) % 60;
    } else if (timeFieldCursor == 2) {
      // Toggle AM/PM by shifting the 24h clock exactly 12 hours
      setHourTemp = (setHourTemp + 12) % 24;
    }
    return;
  }
  if (b == BTN_ENTER) {
    if (timeFieldCursor == 3) { // Cursor max is now 3 (Hour, Min, AM/PM, Save)
      setDeviceTime(setHourTemp, setMinuteTemp);
      state = ST_MENU;
      return;
    }
    timeFieldCursor++;
  }
}

void showMessage(const char* m) {
  strncpy(msgBuf, m, 31);
  msgBuf[31] = 0;
  msgUntil = millis() + 1800;
  needRedraw = true;
}

// ============================================================
//  DRAWING
// ============================================================

void drawFreeze() {
  display.setCursor(0, 0);
  display.print("Freeze Protection");
  display.drawFastHLine(0, 9, 128, COLOR_WHITE);

  bool fa, fok; float ft;
  freezeStatusGet(fa, ft, fok);
  display.setCursor(0, 14);
  if (fok) display.printf("Now: %.1fF%s", ft, fa ? "  ACTIVE!" : "");
  else     display.print("Now: SENSOR FAULT");

  display.fillRect(0, 26, 128, 11, COLOR_WHITE);
  display.setTextColor(COLOR_BLACK);
  display.setCursor(4, 28);
  bool unsaved = (freezeTempDraft != freezeTempFGet());
  display.printf("Freeze temp: %dF%s", (int)freezeTempDraft, unsaved ? " *" : "");
  display.setTextColor(COLOR_WHITE);

  display.setCursor(0, 44);
  display.printf("Releases at %dF", (int)freezeTempDraft + 2);
  display.setCursor(0, 56);
  display.print(unsaved ? "ENT:Save BACK:Cancel" : "UP/DN:Adjust BACK:OK");
}

void drawSettings() {
  display.setCursor(0, 0);
  display.print("Settings");
  display.drawFastHLine(0, 9, 128, COLOR_WHITE);
  
  bool dep = relay2DependencyGet();
  display.fillRect(0, 11, 128, 11, COLOR_WHITE); 
  display.setTextColor(COLOR_BLACK);
  display.setCursor(4, 13);
  display.print(dep ? "R2 Dep on R1: [ON]" : "R2 Dep on R1: [OFF]");
  
  display.setTextColor(COLOR_WHITE);
  display.setCursor(0, 56);
  display.print("UP/DN:Toggle ENTER:OK");
}


void drawScreen() {
  if (millis() < msgUntil) { drawMessageOverlay(); return; }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(COLOR_WHITE);
  switch (state) {
    case ST_HOME:         drawHome(); break;
    case ST_MENU:         drawMenu(); break;
    case ST_PRESET_LIST:  drawPresetList(); break;
    case ST_PRESET_EDIT:  drawPresetEdit(); break;
    case ST_MANUAL:       drawManual(); break;
    case ST_SET_TIME:     drawSetTime(); break;
    case ST_FREEZE:       drawFreeze(); break;
    case ST_SETTINGS:     drawSettings(); break;
  }
  display.display();
}

void drawMessageOverlay() {
  display.clearDisplay();
  display.setTextColor(COLOR_WHITE);
  display.drawRect(8, 20, 112, 24, COLOR_WHITE);
  display.setCursor(14, 28);
  display.print(msgBuf);
  display.display();
}

void drawHome() {
  if (homePage == 1) { drawHomePumps(); return; }

  uint8_t ch, cm, cs; clockGet(ch, cm, cs);
  bool r1on, r2on, r3on; relayActualStatesGet(r1on, r2on, r3on);

  char buf[16];
  int h12 = ch % 12; if (h12 == 0) h12 = 12;
  const char* ampm = (ch >= 12) ? "PM" : "AM";
  sprintf(buf, "%02d:%02d %s", h12, cm, ampm);
  
  display.setTextSize(2);
  display.setCursor(16, 2); 
  display.print(buf);
  display.setTextSize(1);
  display.drawFastHLine(0, 20, 128, COLOR_WHITE);
  
  bool rOn[3] = {r1on, r2on, r3on};
  for (int i = 0; i < MAX_RELAYS; i++) {
    int relayNum = i + 1;
    char rname[13];
    relayDisplayName(relayNum, rname, sizeof(rname));

    int y = 24 + i * 10;
    display.setCursor(4, y);
    display.print(rname);
    display.setCursor(88, y);
    display.print(rOn[i] ? "ON " : "OFF");
    if (relayModeGet(relayNum) != MODE_AUTO) display.print("(M)");
  }

  display.setCursor(4, 56);
  if (!clockWasSet()) display.print("Set time! ENT=Menu");
  else if (clockSourceIsNTP()) display.print("NTP  ENT=Menu UP=Pumps");
  else if (clockSourceIsRTC()) display.print("RTC  ENT=Menu UP=Pumps");
  else display.print("ENTER=Menu  UP=Pumps");
}

// Second Home page: status of all 4 pumps, shown under their assigned
// friendly name (falls back to "PUMPn" if unnamed). Toggled from the
// relay page with UP/DOWN so it fits without crowding the main screen.
void drawHomePumps() {
  uint8_t ch, cm, cs; clockGet(ch, cm, cs);
  char tbuf[12];
  int h12 = ch % 12; if (h12 == 0) h12 = 12;
  const char* ampm = (ch >= 12) ? "PM" : "AM";
  sprintf(tbuf, "%02d:%02d%s", h12, cm, ampm);

  display.setCursor(0, 0);
  display.print("Pumps");
  display.setCursor(88, 0);
  display.print(tbuf);
  display.drawFastHLine(0, 9, 128, COLOR_WHITE);

  for (int i = 0; i < MAX_PUMPS; i++) {
    int pumpNum = i + 1;
    char name[13];
    pumpDisplayName(pumpNum, name, sizeof(name));

    bool active; uint16_t speed;
    pumpActualStateGet(pumpNum, active, speed);
    RelayMode mode = pumpModeGet(pumpNum);

    int y = 12 + i * 12;
    display.setCursor(4, y);
    display.print(name);
    display.setCursor(88, y);
    display.print(active ? "ON " : "OFF");
    if (mode != MODE_AUTO) display.print("(M)");
  }

  display.setCursor(0, 56);
  display.print("UP/DN=Relays  ENT=Menu");
}

void drawMenu() {
  display.setCursor(0, 0);
  display.println("MENU");
  display.drawFastHLine(0, 9, 128, COLOR_WHITE);
  
  for (int row = 0; row < MENU_VISIBLE; row++) {
    int idx = menuScroll + row;
    
    // Stop drawing if we reach the end of the actual menu items
    if (idx >= MENU_COUNT) break; 
    
    int y = 12 + row * 13; // Back to comfortable 13px spacing
    bool sel = (idx == menuIndex);
    
    if (sel) { 
      display.fillRect(0, y - 1, 128, 11, COLOR_WHITE); 
      display.setTextColor(COLOR_BLACK);
    } else {
      display.setTextColor(COLOR_WHITE);
    }
    
    display.setCursor(4, y);
    display.print(menuItems[idx]);
  }
  display.setTextColor(COLOR_WHITE);
}

void drawPresetList() {
  int total = presetCountUsed();
  char hdr[20];
  sprintf(hdr, "Presets (%d/%d)", total, MAX_PRESETS);
  display.setCursor(0, 0);
  display.println(hdr);
  display.drawFastHLine(0, 9, 128, COLOR_WHITE);
  
  if (total == 0) {
    display.setCursor(4, 28);
    display.print("No presets yet.");
    display.setCursor(4, 40);
    display.print("BACK to return");
    return;
  }

  for (int row = 0; row < LIST_VISIBLE; row++) {
    int idx = presetListScroll + row;
    if (idx >= total) break;
    
    int slot = presetSlotForVisibleIndex(idx);
    Preset p; presetGet(slot, p);
    
    int y = 12 + row * 10;
    bool sel = (idx == presetListIndex);
    if (sel) { display.fillRect(0, y - 1, 128, 11, COLOR_WHITE); display.setTextColor(COLOR_BLACK); }
    else display.setTextColor(COLOR_WHITE);

    char tname[13];
    targetDisplayName(p.target, tname, sizeof(tname));

    char line[32];
    // Shorten AM/PM to just a/p for screen space
    int h12 = p.startHour % 12; if (h12 == 0) h12 = 12;
    const char* ampm = (p.startHour >= 12) ? "p" : "a"; 
    
    snprintf(line, sizeof(line), "%s %02d:%02d%s +%dh%02d %s",
            tname, h12, p.startMinute, ampm, p.durHour, p.durMinute,
            p.enabled ? "ON" : "--");
            
    display.setCursor(2, y);
    display.print(line);
  }
  display.setTextColor(COLOR_WHITE);
}

const char* fieldLabel(int fid) {
  switch (fid) {
    case F_TARGET: return "Target HW:";
    case F_PTYPE:  return "Pump Type:";
    case F_SPEED:  return "Speed(RPM):";
    case F_SH:     return "Start Hour:";
    case F_SM:     return "Start Minute:";
    case F_DH:     return "Duration Hours:";
    case F_DM:     return "Duration Mins:";
    case F_EN:     return "Enabled:";
    default:       return "";
  }
}

void fieldValueString(int fid, char* out) {
  switch (fid) {
    case F_TARGET: targetDisplayName(tempPreset.target, out, 16); break;
    case F_PTYPE:  sprintf(out, "%s", pumpTypeName(tempPreset.pumpType)); break;
    case F_SPEED:  sprintf(out, "%d", tempPreset.speed); break;
    case F_SH: {
      // Show as 12-hour format during editing
      int h12 = tempPreset.startHour % 12; if (h12 == 0) h12 = 12;
      const char* ampm = (tempPreset.startHour >= 12) ? "PM" : "AM";
      sprintf(out, "%02d %s", h12, ampm); 
      break;
    }
    case F_SM:     sprintf(out, "%02d", tempPreset.startMinute); break;
    case F_DH:     sprintf(out, "%02d", tempPreset.durHour); break;
    case F_DM:     sprintf(out, "%02d", tempPreset.durMinute); break;
    case F_EN:     sprintf(out, "%s", tempPreset.enabled ? "ON" : "OFF"); break;
    case F_SAVE:   sprintf(out, "[ SAVE ]"); break;
    case F_DELETE: sprintf(out, "[DELETE]"); break;
    case F_CANCEL: sprintf(out, "[CANCEL]"); break;
  }
}

void drawPresetEdit() {
  if (editFieldCursor >= editOrderCount) return; 
  int fid = editOrder[editFieldCursor];
  char hdr[24];
  sprintf(hdr, "%s (%d/%d)", editingIsNew ? "Add Preset" : "Edit Preset",
          editFieldCursor + 1, editOrderCount);
          
  display.setCursor(0, 0);
  display.print(hdr);
  display.drawFastHLine(0, 9, 128, COLOR_WHITE);

  display.setCursor(4, 16);
  display.print(fieldLabel(fid));

  char valbuf[16];
  fieldValueString(fid, valbuf);
  display.setTextSize(2);
  int textWidth = strlen(valbuf) * 12;
  int x = (128 - textWidth) / 2;
  if (x < 0) x = 0;
  display.setCursor(x, 32);
  display.print(valbuf);
  display.setTextSize(1);

  display.setCursor(0, 56);
  if (fid == F_SAVE || fid == F_DELETE || fid == F_CANCEL) {
    display.print("ENTER:Confirm BACK:Prev");
  } else {
    display.print("UP/DN:Set  ENTER:Next");
  }
}

const char* modeStr(RelayMode m) {
  if (m == MODE_AUTO) return "AUTO";
  if (m == MODE_ON) return "ON";
  return "OFF";
}

void manualRowLabel(int rowIdx, char* out, size_t outLen) {
  int rt = manualRowType[rowIdx];
  int p  = manualRowPump[rowIdx];
  switch (rt) {
    case MR_RELAY1:
    case MR_RELAY2:
    case MR_RELAY3: {
      int rn = rt - MR_RELAY1 + 1;
      char name[13]; relayDisplayName(rn, name, sizeof(name));
      snprintf(out, outLen, "%s: %s", name, modeStr(relayModeGet(rn)));
      break;
    }
    case MR_PUMP_MODE: {
      char name[13]; pumpDisplayName(p + 1, name, sizeof(name));
      snprintf(out, outLen, "%s: %s", name, modeStr(pumpModeGet(p + 1)));
      break;
    }
    case MR_PUMP_TYPE: {
      uint16_t speed; uint8_t type; pumpManualParamsGet(p + 1, speed, type);
      snprintf(out, outLen, "  Type: %s", pumpTypeName(type));
      break;
    }
    case MR_PUMP_SPEED: {
      uint16_t speed; uint8_t type; pumpManualParamsGet(p + 1, speed, type);
      snprintf(out, outLen, "  Speed: %d%s", speed, manualEditingSpeed ? " *" : "");
      break;
    }
    case MR_DONE: snprintf(out, outLen, "[ DONE ]"); break;
  }
}

void drawManual() {
  buildManualRows();

  display.setCursor(0, 0);
  display.print("Manual Control");
  display.drawFastHLine(0, 9, 128, COLOR_WHITE);

  for (int row = 0; row < MANUAL_VISIBLE; row++) {
    int idx = manualScroll + row;
    if (idx >= manualRowCount) break;

    int y = 12 + row * 12;
    bool sel = (idx == manualFieldCursor);
    if (sel) { display.fillRect(0, y - 1, 128, 11, COLOR_WHITE); display.setTextColor(COLOR_BLACK); }
    else display.setTextColor(COLOR_WHITE);

    char line[24];
    manualRowLabel(idx, line, sizeof(line));
    display.setCursor(4, y);
    display.print(line);
  }
  display.setTextColor(COLOR_WHITE);

  display.setCursor(0, 56);
  if (manualEditingSpeed) display.print("UP/DN:+-100  ENT:Done");
  else display.print("UP/DN:Move  ENT:Select");
}

void drawSetTime() {
  display.setCursor(0, 0);
  display.print("Set Time");
  display.drawFastHLine(0, 9, 128, COLOR_WHITE);

  char buf[16];
  int h12 = setHourTemp % 12; if (h12 == 0) h12 = 12;
  const char* ampm = (setHourTemp >= 12) ? "PM" : "AM";
  sprintf(buf, "%02d:%02d %s", h12, setMinuteTemp, ampm);
  
  display.setTextSize(2);
  display.setCursor(16, 18);
  display.print(buf);
  display.setTextSize(1);

  // Draw underlines for the active field
  if (timeFieldCursor == 0) display.drawFastHLine(16, 36, 24, COLOR_WHITE);
  else if (timeFieldCursor == 1) display.drawFastHLine(52, 36, 24, COLOR_WHITE);
  else if (timeFieldCursor == 2) display.drawFastHLine(88, 36, 24, COLOR_WHITE); // AM/PM underline

  int y = 46;
  bool sel = (timeFieldCursor == 3);
  if (sel) { display.fillRect(0, y - 1, 128, 11, COLOR_WHITE); display.setTextColor(COLOR_BLACK); }
  else display.setTextColor(COLOR_WHITE);
  display.setCursor(4, y);
  display.print("[ SAVE ]");
  display.setTextColor(COLOR_WHITE);

  display.setCursor(0, 56);
  display.print("UP/DN:Set  ENTER:Next");
}