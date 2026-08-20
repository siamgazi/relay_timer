#pragma once
#include <Arduino.h>
#include <Preferences.h>

#define MAX_PRESETS 8
#define MAX_PUMPS 4
#define MAX_RELAYS 3

// Selectable "friendly" names shown in the web UI dropdowns and on the
// OLED, for BOTH relays and pumps. -1 (stored per-relay/per-pump) means
// "no custom name" -> falls back to the default "Relay N" / "PUMPn"
// label. Every name in this list may only be assigned to ONE entity at
// a time across the *entire* device (a relay and a pump can never share
// a name, nor can two relays or two pumps) -- see pumpNameIndexSet() and
// relayNameIndexSet(), which both check across both arrays.
#define NAME_OPTION_COUNT 9
static const char* const NAME_OPTIONS[NAME_OPTION_COUNT] = {
  "MAIN PUMP", "PSWP PUMP", "SPA LITE", "POOL LITE",
  "FTN PUMP1", "FTN PUMP2", "OTHER1", "OTHER2", "CLNR PUMP"
};

// SPA LITE and POOL LITE are relay-only names (they describe lighting
// circuits, not pumps) -- pumps may not select them, though relays can
// select any name including the pump-flavored ones (e.g. "MAIN PUMP" is
// handy for a relay that happens to power the main pump's contactor).
inline bool nameAllowedForPump(int8_t idx) {
  return idx != 2 && idx != 3; // SPA LITE, POOL LITE
}

// 0: Relay 1, 1: Relay 2, 2: Pump 1, 3: Pump 2, 4: Pump 3, 5: Pump 4
enum TargetType { TGT_RELAY1 = 0, TGT_RELAY2 = 1, TGT_RELAY3 = 2, TGT_PUMP1 = 3, TGT_PUMP2 = 4, TGT_PUMP3 = 5, TGT_PUMP4 = 6 };
// PUMP_BLACKDECKER talks Modbus RTU at 38400 baud (Pentair/Emaux run at
// 9600) and is supported on pump slot 1 ONLY — the pump is fixed at
// Modbus address 1. Enforced at every input path (app, web, on-device
// UI) and once more in the RS485 dispatcher as the last line of defense.
enum PumpType { PUMP_PENTAIR = 0, PUMP_EMAUX = 1, PUMP_BLACKDECKER = 2 };
enum RelayMode { MODE_AUTO = 0, MODE_ON = 1, MODE_OFF = 2 };

struct Preset {
  bool     used;
  bool     enabled;
  uint8_t  target;      // TargetType
  uint8_t  pumpType;    // PumpType (only used if target is a pump)
  uint16_t speed;       // RPM (only used if target is a pump)
  uint8_t  startHour;
  uint8_t  startMinute;
  uint8_t  durHour;
  uint8_t  durMinute;
};

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
struct StateLock {
  StateLock()  { portENTER_CRITICAL(&stateMux); }
  ~StateLock() { portEXIT_CRITICAL(&stateMux); }
};

namespace _state {
  Preset presets[MAX_PRESETS];
  uint8_t curHour = 0, curMinute = 0, curSecond = 0;
  bool timeWasSet = false, timeSource_NTP = false, timeSource_RTC = false;
  
  // --- 2. ADD RELAY 3 STATE ---
  bool relay1ActualState = false, relay2ActualState = false, relay3ActualState = false;
  RelayMode relay1Mode = MODE_AUTO, relay2Mode = MODE_AUTO, relay3Mode = MODE_AUTO;
  
  bool relay2DependsOn1 = true; 
  volatile bool pendingDepSave = false; 
  volatile uint32_t stateVersion = 0;
  Preferences prefs;

  // --- Pump manual override state (mirrors relay1Mode/2Mode/3Mode) ---
  RelayMode pumpMode[MAX_PUMPS] = {MODE_AUTO, MODE_AUTO, MODE_AUTO, MODE_AUTO};
  uint16_t  pumpManualSpeed[MAX_PUMPS] = {0, 0, 0, 0};
  uint8_t   pumpManualType[MAX_PUMPS]  = {0, 0, 0, 0};

  // --- Pump friendly-name assignment. -1 = unassigned (default "PUMPn"),
  // else an index into NAME_OPTIONS. Enforced unique across pumps AND
  // relays (see _nameIndexTakenElsewhere()). ---
  int8_t pumpNameIndex[MAX_PUMPS] = {-1, -1, -1, -1};
  volatile bool pendingPumpNameSave = false;

  // --- Relay friendly-name assignment. -1 = unassigned (default
  // "Relay N"), else an index into NAME_OPTIONS. Enforced unique across
  // relays AND pumps. ---
  int8_t relayNameIndex[MAX_RELAYS] = {-1, -1, -1};
  volatile bool pendingRelayNameSave = false;

  // Preset saves are deferred to loop() like the flags above: the accessors
  // run on the BLE and HTTP tasks too, and the single Preferences object
  // must never be driven from two tasks at once.
  volatile bool pendingPresetSave = false;

  // --- Freeze protection ---------------------------------------------
  // Per-channel opt-in flags + per-pump freeze speed, all persisted.
  // freezeTempF is the trigger threshold (°F); protection engages after
  // the thermistor reads <= threshold for 30s straight and releases at
  // threshold + 2°F. Live status lives in RAM only.
  bool     relayFreeze[MAX_RELAYS] = {false, false, false};
  bool     pumpFreeze[MAX_PUMPS]   = {false, false, false, false};
  uint16_t pumpFreezeSpeed[MAX_PUMPS] = {0, 0, 0, 0};
  int16_t  freezeTempF = 34;
  volatile bool pendingFreezeSave = false;
  bool     freezeActive = false;   // protection currently engaged
  bool     tempSensorOk = false;   // reading is in a plausible range
  float    curTempF = 0.0f;        // latest reading (valid when tempSensorOk)
}

// 1. Put markDirty back on the list so everyone can use it
inline void markDirty() { 
  _state::stateVersion++; 
}

// 2. Put savePresetsToFlash back on the list
inline void savePresetsToFlash() {
  using namespace _state;
  prefs.begin("relaytimer", false);
  prefs.putBytes("presets", presets, sizeof(presets));
  prefs.end();
}

// 3. Your updated load function
inline void loadPresetsFromFlash() {
  using namespace _state;
  prefs.begin("relaytimer", true);
  if (prefs.getBytesLength("presets") == sizeof(presets)) {
    prefs.getBytes("presets", presets, sizeof(presets));
  } else {
    for (int i = 0; i < MAX_PRESETS; i++) presets[i].used = false;
  }
  relay2DependsOn1 = prefs.getBool("r2dep", true);
  if (prefs.getBytesLength("pumpNames") == sizeof(pumpNameIndex)) {
    prefs.getBytes("pumpNames", pumpNameIndex, sizeof(pumpNameIndex));
  } else {
    for (int i = 0; i < MAX_PUMPS; i++) pumpNameIndex[i] = -1;
  }
  if (prefs.getBytesLength("relayNames") == sizeof(relayNameIndex)) {
    prefs.getBytes("relayNames", relayNameIndex, sizeof(relayNameIndex));
  } else {
    for (int i = 0; i < MAX_RELAYS; i++) relayNameIndex[i] = -1;
  }
  if (prefs.getBytesLength("rlyFrz") == sizeof(relayFreeze))
    prefs.getBytes("rlyFrz", relayFreeze, sizeof(relayFreeze));
  if (prefs.getBytesLength("pmpFrz") == sizeof(pumpFreeze))
    prefs.getBytes("pmpFrz", pumpFreeze, sizeof(pumpFreeze));
  if (prefs.getBytesLength("pmpFrzSp") == sizeof(pumpFreezeSpeed))
    prefs.getBytes("pmpFrzSp", pumpFreezeSpeed, sizeof(pumpFreezeSpeed));
  freezeTempF = prefs.getShort("frzTempF", 34);
  prefs.end();

  // Boot inventory: makes "settings are gone" diagnosable. All-default
  // names + 0 presets right after a USB flash means NVS was erased
  // (Erase All Flash / changed partition scheme) — not a sync bug.
  int usedP = 0, namedPumps = 0, namedRelays = 0, frzFlags = 0;
  for (int i = 0; i < MAX_PRESETS; i++) if (presets[i].used) usedP++;
  for (int i = 0; i < MAX_PUMPS; i++) if (pumpNameIndex[i] >= 0) namedPumps++;
  for (int i = 0; i < MAX_RELAYS; i++) if (relayNameIndex[i] >= 0) namedRelays++;
  for (int i = 0; i < MAX_PUMPS; i++) if (pumpFreeze[i]) frzFlags++;
  for (int i = 0; i < MAX_RELAYS; i++) if (relayFreeze[i]) frzFlags++;
  Serial.printf("[nvs] loaded: %d presets, %d pump names, %d relay names, %d freeze flags, freeze temp %dF\n",
                usedP, namedPumps, namedRelays, frzFlags, (int)freezeTempF);
}

inline void relay2DependencySet(bool depends) {
  {
    StateLock lock;
    _state::relay2DependsOn1 = depends;
    _state::pendingDepSave = true; 
  }
  markDirty();
}

inline bool relay2DependencyGet() {
  StateLock lock;
  return _state::relay2DependsOn1;
}

inline void processPendingSaves() {
  bool doSave = false;
  bool valToSave = false;
  bool doNameSave = false;
  int8_t nameSnapshot[MAX_PUMPS];
  bool doRelayNameSave = false;
  int8_t relayNameSnapshot[MAX_RELAYS];
  bool doPresetSave = false;
  static Preset presetSnapshot[MAX_PRESETS]; // loop()-only; too big for the stack
  bool doFreezeSave = false;
  bool rlyFrzSnap[MAX_RELAYS]; bool pmpFrzSnap[MAX_PUMPS];
  uint16_t pmpFrzSpSnap[MAX_PUMPS]; int16_t frzTempSnap = 34;

  {
    StateLock lock;
    if (_state::pendingFreezeSave) {
      doFreezeSave = true;
      memcpy(rlyFrzSnap, _state::relayFreeze, sizeof(rlyFrzSnap));
      memcpy(pmpFrzSnap, _state::pumpFreeze, sizeof(pmpFrzSnap));
      memcpy(pmpFrzSpSnap, _state::pumpFreezeSpeed, sizeof(pmpFrzSpSnap));
      frzTempSnap = _state::freezeTempF;
      _state::pendingFreezeSave = false;
    }
    if (_state::pendingDepSave) {
      doSave = true;
      valToSave = _state::relay2DependsOn1;
      _state::pendingDepSave = false;
    }
    if (_state::pendingPresetSave) {
      doPresetSave = true;
      memcpy(presetSnapshot, _state::presets, sizeof(presetSnapshot));
      _state::pendingPresetSave = false;
    }
    if (_state::pendingPumpNameSave) {
      doNameSave = true;
      memcpy(nameSnapshot, _state::pumpNameIndex, sizeof(nameSnapshot));
      _state::pendingPumpNameSave = false;
    }
    if (_state::pendingRelayNameSave) {
      doRelayNameSave = true;
      memcpy(relayNameSnapshot, _state::relayNameIndex, sizeof(relayNameSnapshot));
      _state::pendingRelayNameSave = false;
    }
  }
  
  if (doSave) {
    _state::prefs.begin("relaytimer", false);
    _state::prefs.putBool("r2dep", valToSave);
    _state::prefs.end();
  }
  if (doNameSave) {
    _state::prefs.begin("relaytimer", false);
    _state::prefs.putBytes("pumpNames", nameSnapshot, sizeof(nameSnapshot));
    _state::prefs.end();
  }
  if (doRelayNameSave) {
    _state::prefs.begin("relaytimer", false);
    _state::prefs.putBytes("relayNames", relayNameSnapshot, sizeof(relayNameSnapshot));
    _state::prefs.end();
  }
  if (doPresetSave) {
    _state::prefs.begin("relaytimer", false);
    _state::prefs.putBytes("presets", presetSnapshot, sizeof(presetSnapshot));
    _state::prefs.end();
  }
  if (doFreezeSave) {
    _state::prefs.begin("relaytimer", false);
    _state::prefs.putBytes("rlyFrz", rlyFrzSnap, sizeof(rlyFrzSnap));
    _state::prefs.putBytes("pmpFrz", pmpFrzSnap, sizeof(pmpFrzSnap));
    _state::prefs.putBytes("pmpFrzSp", pmpFrzSpSnap, sizeof(pmpFrzSpSnap));
    _state::prefs.putShort("frzTempF", frzTempSnap);
    _state::prefs.end();
  }
}

// ... Keep existing presetCountUsed, presetFindFreeSlot, clockGet, etc. exactly the same as before ...
// (Ensure you rename `p.target` to `p.target` in your helper functions like isWithinSchedule if you rely on it there, though isWithinSchedule only checks time).



// ============================================================
//  PRESET ACCESSORS (used by BOTH button UI and web API)
// ============================================================
inline int presetCountUsed() {
  StateLock lock;
  int c = 0;
  for (int i = 0; i < MAX_PRESETS; i++) if (_state::presets[i].used) c++;
  return c;
}

// Maps a 0-based "visible row index" (only counting used slots) to the
// real underlying slot index. Returns -1 if out of range.
inline int presetSlotForVisibleIndex(int n) {
  StateLock lock;
  int count = -1;
  for (int i = 0; i < MAX_PRESETS; i++) {
    if (_state::presets[i].used) {
      count++;
      if (count == n) return i;
    }
  }
  return -1;
}

inline int presetFindFreeSlot() {
  StateLock lock;
  for (int i = 0; i < MAX_PRESETS; i++) {
    if (!_state::presets[i].used) return i;
  }
  return -1;
}

// Safe copy-out of a single preset slot (by absolute slot index 0..MAX_PRESETS-1)
inline bool presetGet(int slot, Preset &out) {
  if (slot < 0 || slot >= MAX_PRESETS) return false;
  StateLock lock;
  out = _state::presets[slot];
  return true;
}

// Writes a full preset into a slot, marks used, saves to flash, bumps version.
inline bool presetWrite(int slot, const Preset &p) {
  if (slot < 0 || slot >= MAX_PRESETS) return false;
  {
    StateLock lock;
    _state::presets[slot] = p;
    _state::presets[slot].used = true;
    _state::pendingPresetSave = true;
  }
  markDirty();
  return true;
}

inline bool presetDelete(int slot) {
  if (slot < 0 || slot >= MAX_PRESETS) return false;
  {
    StateLock lock;
    _state::presets[slot].used = false;
    _state::pendingPresetSave = true;
  }
  markDirty();
  return true;
}

inline bool presetSetEnabled(int slot, bool en) {
  if (slot < 0 || slot >= MAX_PRESETS) return false;
  {
    StateLock lock;
    if (!_state::presets[slot].used) return false;
    _state::presets[slot].enabled = en;
    _state::pendingPresetSave = true;
  }
  markDirty();
  return true;
}

// ============================================================
//  CLOCK ACCESSORS
// ============================================================
inline void clockGet(uint8_t &h, uint8_t &m, uint8_t &s) {
  StateLock lock;
  h = _state::curHour; m = _state::curMinute; s = _state::curSecond;
}

inline bool clockWasSet() {
  StateLock lock;
  return _state::timeWasSet;
}

inline bool clockSourceIsNTP() {
  StateLock lock;
  return _state::timeSource_NTP;
}

inline bool clockSourceIsRTC() {
  StateLock lock;
  return _state::timeSource_RTC;
}

// Used by both the "Set Time" menu screen and the /api/settime endpoint.
inline void clockSetManual(uint8_t h, uint8_t m) {
  {
    StateLock lock;
    _state::curHour = h % 24;
    _state::curMinute = m % 60;
    _state::curSecond = 0;
    _state::timeWasSet = true;
    _state::timeSource_NTP = false;
    _state::timeSource_RTC = false;
  }
  markDirty();
}

// Used only by the NTP sync task - does not clobber a more-recent manual
// override; NTP is allowed to set/refresh time freely since it's "ground
// truth" when available, but we tag the source so the UI can show it.
inline void clockSetFromNTP(uint8_t h, uint8_t m, uint8_t s) {
  {
    StateLock lock;
    _state::curHour = h;
    _state::curMinute = m;
    _state::curSecond = s;
    _state::timeWasSet = true;
    _state::timeSource_NTP = true;
    _state::timeSource_RTC = false;
  }
  markDirty();
}

// Used when loading time from the DS3231 -- at boot, and during periodic
// drift-correction resyncs. Does NOT write back to the RTC; callers that
// already read the value FROM the RTC don't need to round-trip it.
inline void clockSetFromRTC(uint8_t h, uint8_t m, uint8_t s) {
  {
    StateLock lock;
    _state::curHour = h;
    _state::curMinute = m;
    _state::curSecond = s;
    _state::timeWasSet = true;
    _state::timeSource_NTP = false;
    _state::timeSource_RTC = true;
  }
  markDirty();
}

// Seeds the display clock WITHOUT marking time as set — for the fabricated
// 12:00 a dead-battery RTC gets at boot. Schedules are gated on
// clockWasSet(), so a made-up wall clock must never count as a time source.
inline void clockSeedUntrusted(uint8_t h, uint8_t m, uint8_t s) {
  {
    StateLock lock;
    _state::curHour = h;
    _state::curMinute = m;
    _state::curSecond = s;
    _state::timeWasSet = false;
    _state::timeSource_NTP = false;
    _state::timeSource_RTC = false;
  }
  markDirty();
}

// Called once per second from loop() to advance the software clock.
inline void clockTickOneSecond() {
  StateLock lock;
  _state::curSecond++;
  if (_state::curSecond >= 60) {
    _state::curSecond = 0;
    _state::curMinute++;
    if (_state::curMinute >= 60) {
      _state::curMinute = 0;
      _state::curHour++;
      if (_state::curHour >= 24) _state::curHour = 0;
    }
  }
}

// ============================================================
//  RELAY MODE ACCESSORS (manual override, used by both UIs)
// ============================================================
inline RelayMode relayModeGet(int relayNum) {
  StateLock lock;
  if (relayNum == 1) return _state::relay1Mode;
  if (relayNum == 2) return _state::relay2Mode;
  return _state::relay3Mode;
}

inline void relayModeSet(int relayNum, RelayMode m) {
  {
    StateLock lock;
    if (relayNum == 1) _state::relay1Mode = m;
    else if (relayNum == 2) _state::relay2Mode = m;
    else _state::relay3Mode = m;
  }
  markDirty();
}

inline RelayMode relayModeCycle(RelayMode m, int dir) {
  int v = (int)m;
  v = (v + 3 + dir) % 3;
  return (RelayMode)v;
}

inline void relayActualStatesGet(bool &r1, bool &r2, bool &r3) {
  StateLock lock;
  r1 = _state::relay1ActualState;
  r2 = _state::relay2ActualState;
  r3 = _state::relay3ActualState;
}

inline void relayActualStatesPublish(bool r1, bool r2, bool r3) {
  bool changed;
  {
    StateLock lock;
    changed = (_state::relay1ActualState != r1) || (_state::relay2ActualState != r2) || (_state::relay3ActualState != r3);
    _state::relay1ActualState = r1;
    _state::relay2ActualState = r2;
    _state::relay3ActualState = r3;
  }
  if (changed) markDirty();
}

// ============================================================
//  PUMP MODE ACCESSORS (manual override, used by web /api/pumpmanual)
//  pumpNum is 1-based (1..MAX_PUMPS), matching relayNum convention.
// ============================================================
inline RelayMode pumpModeGet(int pumpNum) {
  if (pumpNum < 1 || pumpNum > MAX_PUMPS) return MODE_AUTO;
  StateLock lock;
  return _state::pumpMode[pumpNum - 1];
}

inline void pumpModeSet(int pumpNum, RelayMode m) {
  if (pumpNum < 1 || pumpNum > MAX_PUMPS) return;
  {
    StateLock lock;
    _state::pumpMode[pumpNum - 1] = m;
  }
  markDirty();
}

// Stores the speed/type to use the next time this pump is put into
// MODE_ON manually. Only meaningful together with pumpModeSet(pumpNum, MODE_ON).
inline void pumpManualParamsSet(int pumpNum, uint16_t speed, uint8_t pumpType) {
  if (pumpNum < 1 || pumpNum > MAX_PUMPS) return;
  StateLock lock;
  _state::pumpManualSpeed[pumpNum - 1] = speed;
  _state::pumpManualType[pumpNum - 1] = pumpType;
}

inline void pumpManualParamsGet(int pumpNum, uint16_t &speed, uint8_t &pumpType) {
  speed = 0; pumpType = 0;
  if (pumpNum < 1 || pumpNum > MAX_PUMPS) return;
  StateLock lock;
  speed = _state::pumpManualSpeed[pumpNum - 1];
  pumpType = _state::pumpManualType[pumpNum - 1];
}

// ============================================================
//  NAME ACCESSORS (pumps AND relays share one global name pool)
//  pumpNum / relayNum are 1-based. nameIdx is -1 (unassigned) or an
//  index into NAME_OPTIONS. Every name may only be held by ONE entity
//  at a time across the whole device -- a relay and a pump can never
//  show the same custom name, nor can two relays or two pumps.
// ============================================================

// Internal: true if nameIdx is currently held by some entity other than
// the one being changed. Caller must already hold StateLock.
inline bool _nameIndexTakenElsewhere(int8_t nameIdx, bool changingIsPump, int changingNum) {
  if (nameIdx < 0) return false;
  for (int i = 0; i < MAX_PUMPS; i++) {
    if (changingIsPump && i == (changingNum - 1)) continue;
    if (_state::pumpNameIndex[i] == nameIdx) return true;
  }
  for (int i = 0; i < MAX_RELAYS; i++) {
    if (!changingIsPump && i == (changingNum - 1)) continue;
    if (_state::relayNameIndex[i] == nameIdx) return true;
  }
  return false;
}

inline int8_t pumpNameIndexGet(int pumpNum) {
  if (pumpNum < 1 || pumpNum > MAX_PUMPS) return -1;
  StateLock lock;
  return _state::pumpNameIndex[pumpNum - 1];
}

// Returns false (and leaves state unchanged) if nameIdx is already held
// by another pump or relay, or is a relay-only name (SPA LITE/POOL LITE).
inline bool pumpNameIndexSet(int pumpNum, int8_t nameIdx) {
  if (pumpNum < 1 || pumpNum > MAX_PUMPS) return false;
  if (nameIdx < -1 || nameIdx >= NAME_OPTION_COUNT) return false;
  if (nameIdx >= 0 && !nameAllowedForPump(nameIdx)) return false;

  bool ok = true;
  {
    StateLock lock;
    if (_nameIndexTakenElsewhere(nameIdx, true, pumpNum)) ok = false;
    if (ok) {
      _state::pumpNameIndex[pumpNum - 1] = nameIdx;
      _state::pendingPumpNameSave = true;
      // MAIN PUMP is always freeze protected: naming a pump MAIN PUMP
      // (index 0) enables its freeze flag, with a sane default speed.
      if (nameIdx == 0 && !_state::pumpFreeze[pumpNum - 1]) {
        _state::pumpFreeze[pumpNum - 1] = true;
        if (_state::pumpFreezeSpeed[pumpNum - 1] == 0)
          _state::pumpFreezeSpeed[pumpNum - 1] = 1200;
        _state::pendingFreezeSave = true;
      }
    }
  }
  if (ok) markDirty();
  return ok;
}

// Writes this pump's display label ("PUMPn" or its assigned name) into out.
inline void pumpDisplayName(int pumpNum, char *out, size_t outLen) {
  if (outLen == 0) return;
  if (pumpNum < 1 || pumpNum > MAX_PUMPS) { out[0] = 0; return; }
  int8_t idx = pumpNameIndexGet(pumpNum);
  if (idx >= 0 && idx < NAME_OPTION_COUNT) {
    strncpy(out, NAME_OPTIONS[idx], outLen - 1);
    out[outLen - 1] = 0;
  } else {
    snprintf(out, outLen, "PUMP%d", pumpNum);
  }
}

inline int8_t relayNameIndexGet(int relayNum) {
  if (relayNum < 1 || relayNum > MAX_RELAYS) return -1;
  StateLock lock;
  return _state::relayNameIndex[relayNum - 1];
}

// Returns false (and leaves state unchanged) if nameIdx is already held
// by another relay or pump. Unlike pumps, relays may use ANY name in
// NAME_OPTIONS, including the relay-only SPA LITE/POOL LITE entries.
inline bool relayNameIndexSet(int relayNum, int8_t nameIdx) {
  if (relayNum < 1 || relayNum > MAX_RELAYS) return false;
  if (nameIdx < -1 || nameIdx >= NAME_OPTION_COUNT) return false;

  bool ok = true;
  {
    StateLock lock;
    if (_nameIndexTakenElsewhere(nameIdx, false, relayNum)) ok = false;
    if (ok) {
      _state::relayNameIndex[relayNum - 1] = nameIdx;
      _state::pendingRelayNameSave = true;
      // MAIN PUMP is always freeze protected — same rule when the name
      // lands on a relay (e.g. one driving the main pump's contactor).
      if (nameIdx == 0 && !_state::relayFreeze[relayNum - 1]) {
        _state::relayFreeze[relayNum - 1] = true;
        _state::pendingFreezeSave = true;
      }
    }
  }
  if (ok) markDirty();
  return ok;
}

// Writes this relay's display label ("Relay N" or its assigned name) into out.
inline void relayDisplayName(int relayNum, char *out, size_t outLen) {
  if (outLen == 0) return;
  if (relayNum < 1 || relayNum > MAX_RELAYS) { out[0] = 0; return; }
  int8_t idx = relayNameIndexGet(relayNum);
  if (idx >= 0 && idx < NAME_OPTION_COUNT) {
    strncpy(out, NAME_OPTIONS[idx], outLen - 1);
    out[outLen - 1] = 0;
  } else {
    snprintf(out, outLen, "Relay %d", relayNum);
  }
}

// ============================================================
//  FREEZE PROTECTION ACCESSORS
// ============================================================
inline bool relayFreezeGet(int relayNum) {
  if (relayNum < 1 || relayNum > MAX_RELAYS) return false;
  StateLock lock;
  return _state::relayFreeze[relayNum - 1];
}
inline void relayFreezeSet(int relayNum, bool en) {
  if (relayNum < 1 || relayNum > MAX_RELAYS) return;
  {
    StateLock lock;
    _state::relayFreeze[relayNum - 1] = en;
    _state::pendingFreezeSave = true;
  }
  markDirty();
}
inline void pumpFreezeGet(int pumpNum, bool &en, uint16_t &speed) {
  en = false; speed = 0;
  if (pumpNum < 1 || pumpNum > MAX_PUMPS) return;
  StateLock lock;
  en = _state::pumpFreeze[pumpNum - 1];
  speed = _state::pumpFreezeSpeed[pumpNum - 1];
}
inline void pumpFreezeSet(int pumpNum, bool en, uint16_t speed) {
  if (pumpNum < 1 || pumpNum > MAX_PUMPS) return;
  {
    StateLock lock;
    _state::pumpFreeze[pumpNum - 1] = en;
    if (en && speed > 0) _state::pumpFreezeSpeed[pumpNum - 1] = speed;
    _state::pendingFreezeSave = true;
  }
  markDirty();
}
inline int16_t freezeTempFGet() {
  StateLock lock;
  return _state::freezeTempF;
}
inline void freezeTempFSet(int16_t t) {
  {
    StateLock lock;
    _state::freezeTempF = t;
    _state::pendingFreezeSave = true;
  }
  markDirty();
}
inline void freezeStatusGet(bool &active, float &tempF, bool &sensorOk) {
  StateLock lock;
  active = _state::freezeActive;
  tempF = _state::curTempF;
  sensorOk = _state::tempSensorOk;
}
// Loop-only publisher. Bumps the state version only when active/sensorOk
// FLIP — the per-second temperature sample must not trigger a state push
// every second (the 5s heartbeat carries the latest reading regardless).
inline void freezeStatusSet(bool active, float tempF, bool sensorOk) {
  bool changed;
  {
    StateLock lock;
    changed = (_state::freezeActive != active) || (_state::tempSensorOk != sensorOk);
    _state::freezeActive = active;
    _state::curTempF = tempF;
    _state::tempSensorOk = sensorOk;
  }
  if (changed) markDirty();
}

// Unified display name for a preset's target (0-2 = relays, 3-6 = pumps),
// used anywhere a preset/schedule needs to show what it controls.
inline void targetDisplayName(uint8_t target, char *out, size_t outLen) {
  if (target <= TGT_RELAY3) {
    relayDisplayName(target + 1, out, outLen);
  } else {
    pumpDisplayName(target - TGT_PUMP1 + 1, out, outLen);
  }
}

inline uint32_t stateVersionGet() {
  return _state::stateVersion;
}

// ============================================================
//  SCHEDULE MATH (shared - used by relay logic, OLED countdowns,
//  and the web timeline/countdown display logic on the backend
//  if ever needed; currently the web countdown math lives in JS,
//  but isWithinSchedule() itself is also used by relay_timer.ino)
// ============================================================
inline bool isWithinSchedule(const Preset &p, int curMin) {
  int durMin = (int)p.durHour * 60 + p.durMinute;
  if (durMin <= 0) return false;
  int startMin = (int)p.startHour * 60 + p.startMinute;
  int endMin = startMin + durMin;
  if (endMin <= 1440) {
    return (curMin >= startMin && curMin < endMin);
  } else {
    int wrapEnd = endMin - 1440;
    return (curMin >= startMin || curMin < wrapEnd);
  }
}
