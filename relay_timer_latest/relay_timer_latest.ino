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

enum AppState { ST_HOME, ST_MENU, ST_PRESET_LIST, ST_PRESET_EDIT, ST_MANUAL, ST_SET_TIME, ST_SETTINGS };
AppState state = ST_HOME;
int homePage = 0; // 0 = relay status, 1 = pump status (toggle with UP/DOWN while at Home)

const char* menuItems[] = {"View Presets", "Add Preset", "Manual Control", "Set Time", "Settings"};
#define MENU_COUNT 5
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
  
  setRelay1(false);
  setRelay2(false);
  setRelay3(false);

#if DISPLAY_TYPE
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
    for (;;) delay(1000);
  }
#else
  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("SH1106 init failed");
    for (;;) delay(1000);
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

  // App connectivity: Wi-Fi state machine (starts the HTTP API when the
  // network comes up), BLE auth deadlines, and state broadcasts to any
  // connected phones whenever stateVersion changes.
  appLinkLoop();


  processPendingSaves();

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
//  OUTPUT CONTROL
// ============================================================
void writeRelay(int pin, bool on, bool activeHigh) {
  bool level = activeHigh ? on : !on;
  digitalWrite(pin, level ? HIGH : LOW);
}
void setRelay1(bool on) { writeRelay(PIN_RELAY1, on, RELAY1_ACTIVE_HIGH); }
void setRelay2(bool on) { writeRelay(PIN_RELAY2, on, RELAY2_ACTIVE_HIGH); }
void setRelay3(bool on) { writeRelay(PIN_RELAY3, on, RELAY3_ACTIVE_HIGH); }

void updateOutputs() {
  uint8_t ch, cm, cs; clockGet(ch, cm, cs);
  int curMin = (int)ch * 60 + cm;
  
  bool r1Wants = false, r2Wants = false, r3Wants = false;
  bool pumpWantsRun[MAX_PUMPS] = {false};
  uint16_t pumpWantsSpeed[MAX_PUMPS] = {0};
  uint8_t pumpWantsType[MAX_PUMPS] = {0};

  for (int i = 0; i < MAX_PRESETS; i++) {
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


void handleButton(Btn b) {
  switch (state) {
    case ST_HOME:        handleHome(b); break;
    case ST_MENU:        handleMenu(b); break;
    case ST_PRESET_LIST: handlePresetList(b); break;
    case ST_PRESET_EDIT: handlePresetEdit(b); break;
    case ST_MANUAL:      handleManual(b); break;
    case ST_SET_TIME:    handleSetTime(b); break;
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
      case 4: state = ST_SETTINGS; break;
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
    case MR_RELAY1: snprintf(out, outLen, "Relay 1: %s", modeStr(relayModeGet(1))); break;
    case MR_RELAY2: snprintf(out, outLen, "Relay 2: %s", modeStr(relayModeGet(2))); break;
    case MR_RELAY3: snprintf(out, outLen, "Relay 3: %s", modeStr(relayModeGet(3))); break;
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
