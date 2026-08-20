#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include "shared_state.h"

// ============================================================
//  DS3231 real-time clock
//
//  The DS3231 is the source of truth for time-of-day across power
//  cycles. This project has no day/date-based scheduling -- only
//  hour/minute -- so we always write/read the DS3231 using a fixed,
//  meaningless date and ignore the date on read.
//
//  - rtcBegin(): call once from setup(), AFTER Wire/the OLED has been
//    initialized.
//      * RTC not found on the bus -> silently fall back to the
//        software-only clock (time will NOT survive a reboot).
//      * RTC found, lostPower() true -> the DS3231's Oscillator Stop
//        Flag is set, meaning its battery was missing/dead or this is
//        a brand-new module. Whatever it currently reads can't be
//        trusted, so we seed it with a default of 12:00:00 and load
//        that as the starting time.
//      * RTC found, lostPower() false -> battery-backed and has a
//        previously-saved time; trust it and load it as-is. This is
//        what makes time survive a reboot/power loss.
//  - setDeviceTime(h, m): the ONE place user-initiated time changes
//    should go through (OLED "Set Time" screen, and the webserver's
//    /api/settime) -- updates the live clock immediately AND persists
//    it to the RTC so it's still correct after the next power cycle.
//  - rtcResync(): call every loop() iteration; internally throttles
//    itself to roughly once per RTC_RESYNC_INTERVAL_MS. Re-aligns the
//    software clock against the RTC to correct any drift in the
//    ESP32's own crystal over long uptimes.
// ============================================================

RTC_DS3231 rtc;
bool rtcAvailable = false;
// False while the DS3231 holds the fabricated 12:00 seed (battery was
// dead/missing at boot). Resyncs must not promote that seed to a trusted
// wall clock — schedules are gated on clockWasSet(). Becomes true once a
// real time lands: battery-backed value at boot, or a user/app set.
bool rtcTimeTrusted = false;

#define RTC_RESYNC_INTERVAL_MS (30UL * 60UL * 1000UL) // 30 minutes

// The DS3231 always stores a full date+time; nothing in this project
// uses the date part, so we just pick a fixed one and ignore it on read.
inline DateTime rtcMakeDateTime(uint8_t h, uint8_t m, uint8_t s) {
  return DateTime(2024, 1, 1, h, m, s);
}

inline void rtcWriteTime(uint8_t h, uint8_t m, uint8_t s) {
  if (!rtcAvailable) return;
  rtc.adjust(rtcMakeDateTime(h, m, s));
}

inline void rtcBegin() {
  rtcAvailable = rtc.begin();
  if (!rtcAvailable) {
    Serial.println("DS3231 not found on I2C bus - time will NOT survive a reboot/power loss");
    return;
  }

  if (rtc.lostPower()) {
    Serial.println("DS3231 lost power (empty/new module) - seeding default 12:00:00 (untrusted; schedules wait for a real time)");
    rtcWriteTime(12, 0, 0);
    clockSeedUntrusted(12, 0, 0); // display runs, but this must not fire schedules
  } else {
    DateTime now = rtc.now();
    Serial.println("DS3231 OK - time loaded from RTC");
    rtcTimeTrusted = true;
    clockSetFromRTC(now.hour(), now.minute(), now.second());
  }
}

// Re-aligns the software clock against the RTC. Cheap to call every
// loop() tick -- it only actually touches I2C once it's due.
inline void rtcResync() {
  static unsigned long lastResyncMs = 0;
  if (!rtcAvailable || !rtcTimeTrusted) return; // never resync FROM the untrusted seed
  if (millis() - lastResyncMs < RTC_RESYNC_INTERVAL_MS) return;
  lastResyncMs = millis();
  DateTime now = rtc.now();
  clockSetFromRTC(now.hour(), now.minute(), now.second());
}

// User-initiated time change (OLED "Set Time" screen, or the app's
// setTime command) -- updates the live clock immediately AND queues the
// RTC write so it's remembered across the next power cycle.
//
// The RTC write is DEFERRED to loop(): setTime arrives on the BLE/HTTP
// tasks, but Wire is also mid-flight there every OLED redraw and has no
// cross-task lock -- an interleaved transaction garbles the frame or
// corrupts the RTC write. rtcServicePendingWrite() below does the I2C.
volatile bool    rtcPendingWrite = false;
volatile uint8_t rtcPendingH = 0, rtcPendingM = 0;

inline void setDeviceTime(uint8_t h, uint8_t m) {
  clockSetManual(h, m);
  rtcPendingH = h;
  rtcPendingM = m;
  rtcPendingWrite = true;
}

// Call every loop() iteration (same task as the OLED redraw).
inline void rtcServicePendingWrite() {
  if (!rtcPendingWrite) return;
  rtcPendingWrite = false;
  rtcWriteTime(rtcPendingH, rtcPendingM, 0);
  rtcTimeTrusted = true; // a user/app-set time replaces the untrusted seed
}
