#pragma once
#include <Arduino.h>

// ---- Debug logging -----------------------------------------
// RS485_DEBUG: compile-time master switch. Set to 0 to strip all debug
// code out of the build entirely (smallest/fastest production build).
#ifndef RS485_DEBUG
#define RS485_DEBUG 1
#endif

#if RS485_DEBUG
  // Runtime on/off switch. Debug code is compiled in either way (as long
  // as RS485_DEBUG above is 1), but output only prints while this is
  // true -- flip it live via rs485SetDebug(), e.g. from a serial command
  // or a web UI toggle, without reflashing.
  bool rs485DebugEnabled = false;

  inline void rs485SetDebug(bool on) {
      rs485DebugEnabled = on;
      Serial.print("[RS485] debug ");
      Serial.println(on ? "ENABLED" : "DISABLED");
  }

  #define DBG_PRINT(...)    do { if (rs485DebugEnabled) Serial.print(__VA_ARGS__); } while (0)
  #define DBG_PRINTLN(...)  do { if (rs485DebugEnabled) Serial.println(__VA_ARGS__); } while (0)
  #define DBG_PRINTF(...)   do { if (rs485DebugEnabled) Serial.printf(__VA_ARGS__); } while (0)
#else
  // Compiled out entirely -- rs485SetDebug() still exists as a no-op so
  // callers don't need to #ifdef around it.
  inline void rs485SetDebug(bool on) { (void)on; }

  #define DBG_PRINT(...)
  #define DBG_PRINTLN(...)
  #define DBG_PRINTF(...)
#endif

// Prints a byte buffer as space-separated hex, e.g. "02 06 00 08 00 01 4C 3B"
inline void dbgHexDump(const uint8_t *buf, uint8_t len) {
#if RS485_DEBUG
    if (!rs485DebugEnabled) return;
    for (uint8_t i = 0; i < len; i++) {
        if (buf[i] < 0x10) Serial.print('0');
        Serial.print(buf[i], HEX);
        Serial.print(' ');
    }
    Serial.println();
#endif
}

#define RS485_SERIAL Serial2
#define DE_RE_PIN 4
#define RS485_BAUD 9600

#define RESPONSE_TIMEOUT_MS   500   // max wait for first reply byte
#define INTERCHAR_TIMEOUT_MS  15    // max gap between reply bytes
// One byte on the wire takes 10 bit-times; wait that long after flush()
// so the UART shift register finishes the stop bit before we pull DE/RE
// low again, or the last byte gets clipped on the bus. Computed from the
// CURRENT baud, because the bus now runs at two speeds: 9600 for
// Pentair/Emaux, 38400 for Black & Decker (see rs485UseBaud below).
uint32_t rs485CurrentBaud = RS485_BAUD;
#define BYTE_TIME_US  (10UL * 1000000UL / rs485CurrentBaud + 300UL)

// ---- Pentair IntelliFlo VS/VF (proprietary RS485, NOT Modbus) ----
#define PENTAIR_CTRL_ADDR     0x10   // us, acting as the automation controller
#define PENTAIR_BASE_ADDR     0x60   // Pentair pumps live at 0x60-0x6F on the bus
                                      // (dip-switch address), NOT raw pump slot 1-4
#define PENTAIR_CMD_SPEED     0x01
#define PENTAIR_CMD_CONTROL   0x04   // must be sent once before the pump accepts remote commands
#define PENTAIR_CMD_RUN       0x06
#define PENTAIR_RUN           0x0A
#define PENTAIR_STOP          0x04
#define PENTAIR_RPM_MIN       450
#define PENTAIR_RPM_MAX       3450
#define PENTAIR_CONTROL_RETRY_MS 3000 // don't hammer the bus retrying "take control"

// ---- Emaux pump (Modbus RTU, function 0x06 write-single-register) ----
#define EMAUX_FC_WRITE     0x06
#define EMAUX_REG_RUN      0x0008
#define EMAUX_REG_SPEED    0x0009
#define EMAUX_RUN_CMD      1
#define EMAUX_STOP_CMD     2
#define EMAUX_RPM_MIN      800
#define EMAUX_RPM_MAX      3400

// ---- Black & Decker pump (Modbus RTU, function 0x06, 38400 baud) ----
// Same frame shape and CRC as the Emaux, but different registers, a
// scaled speed value, and — crucially — a different bus speed: this pump
// listens at 38400 while Pentair/Emaux run at 9600. The driver switches
// the UART baud per transaction (rs485UseBaud). Only ONE Black & Decker
// pump is supported, and it must be pump slot 1: the pump is fixed at
// Modbus address 1.
#define BD_BAUD         38400UL
#define BD_FC_WRITE     0x06
#define BD_REG_SPEED    0x0007
#define BD_REG_RUN      0x1000
#define BD_RUN_CMD      1
#define BD_STOP_CMD     0
#define BD_RPM_SCALE    5      // register value = rpm * 5
#define BD_RPM_MIN      600    // adjust to your pump's real envelope
#define BD_RPM_MAX      3450

struct PumpState {
    bool active = false;
    uint16_t speed = 0;
    uint8_t  type = PUMP_PENTAIR; // protocol this pump was last actually driven with
};
PumpState currentPumpStates[MAX_PUMPS];

// Pentair requires a one-time "take remote control" handshake per pump
// address before it will accept speed/run commands. Tracked per pump so
// pump 1..4 (RS485 addresses 1..4) are each handshaken independently.
bool pentairControlTaken[MAX_PUMPS] = {false, false, false, false};
unsigned long pentairControlLastTry[MAX_PUMPS] = {0, 0, 0, 0};

inline void rs485Begin() {
    pinMode(DE_RE_PIN, OUTPUT);
    digitalWrite(DE_RE_PIN, LOW);
    RS485_SERIAL.begin(RS485_BAUD, SERIAL_8N1, 18, 17); // RX=18, TX=17
    DBG_PRINTF("[RS485] begin: baud=%lu RX=18 TX=17 DE/RE=%d\n", (unsigned long)RS485_BAUD, DE_RE_PIN);
}

// ---------------- Shared low-level transceiver ----------------
inline void rs485FlushRx() {
    while (RS485_SERIAL.available()) RS485_SERIAL.read();
}

// Retunes the UART when the next transaction targets a pump family on the
// other bus speed. Each protocol driver calls this at the top of its
// send, so mixed Pentair/Emaux (9600) and Black & Decker (38400) traffic
// interleaves safely on the same wire.
inline void rs485UseBaud(uint32_t baud) {
    if (rs485CurrentBaud == baud) return;
    RS485_SERIAL.flush();               // never re-tune under an in-flight TX
    RS485_SERIAL.updateBaudRate(baud);
    rs485CurrentBaud = baud;
    DBG_PRINTF("[RS485] baud switched to %lu\n", (unsigned long)baud);
}

// Minimum quiet time to leave the bus before starting a new transaction.
// Applied at the START of the next send (not after this one's receive
// window) so it never eats into a fast slave's response window.
#define RS485_INTERFRAME_DELAY_MS 10

inline void rs485Send(const uint8_t *frame, uint8_t len) {
    delay(RS485_INTERFRAME_DELAY_MS); // let the bus settle before we grab it
    rs485FlushRx();
    DBG_PRINT("[RS485] TX (");
    DBG_PRINT(len);
    DBG_PRINT(" bytes): ");
    dbgHexDump(frame, len);

    digitalWrite(DE_RE_PIN, HIGH);
    delayMicroseconds(100);
    RS485_SERIAL.write(frame, len);
    RS485_SERIAL.flush();
    delayMicroseconds(BYTE_TIME_US); // let the last stop bit clear the wire
    digitalWrite(DE_RE_PIN, LOW);

    // FIX: transceiver echo / transition noise settles within microseconds
    // of DE/RE dropping, NOT milliseconds. The previous version waited
    // 20ms and *then* flushed -- plenty of time for a fast-responding
    // pump's real reply to already be sitting in the buffer, so that
    // flush was silently eating the start of legitimate responses
    // (confirmed by debug output: 9 bytes discarded for an 8-byte TX,
    // immediately followed by an RX timeout). Flush right away instead,
    // with only a brief settle time for the transceiver itself.
    delayMicroseconds(200);
#if RS485_DEBUG
    uint8_t echoCount = 0;
    while (RS485_SERIAL.available()) { RS485_SERIAL.read(); echoCount++; }
    if (echoCount > 0) {
        DBG_PRINT("[RS485] discarded ");
        DBG_PRINT(echoCount);
        DBG_PRINTLN(" echo byte(s) after TX");
    }
#else
    rs485FlushRx();
#endif
}

// Two-stage receive: wait up to RESPONSE_TIMEOUT_MS for the first byte,
// then keep collecting until a short inter-character gap.
inline uint8_t rs485Receive(uint8_t *buf, uint8_t maxLen) {
    unsigned long start = millis();
    while (!RS485_SERIAL.available()) {
        if (millis() - start > RESPONSE_TIMEOUT_MS) {
            DBG_PRINTLN("[RS485] RX timeout: no bytes received");
            return 0;
        }
    }
    unsigned long firstByteAt = millis();
    uint8_t idx = 0;
    start = millis();
    while (idx < maxLen) {
        if (RS485_SERIAL.available()) {
            buf[idx++] = RS485_SERIAL.read();
            start = millis();
        } else if (millis() - start > INTERCHAR_TIMEOUT_MS) {
            break;
        }
    }
    DBG_PRINT("[RS485] RX (");
    DBG_PRINT(idx);
    DBG_PRINT(" bytes, ");
    DBG_PRINT(millis() - firstByteAt);
    DBG_PRINT("ms after first byte): ");
    dbgHexDump(buf, idx);
    if (idx == maxLen) {
        DBG_PRINTLN("[RS485] WARNING: RX buffer full — response may have been truncated, consider raising maxLen");
    }
    return idx;
}

// ============================================================
//  Pentair
// ============================================================
inline uint16_t pentairChecksum(const uint8_t *buf, uint8_t len) {
    uint16_t sum = 0;
    for (uint8_t i = 0; i < len; i++) sum += buf[i];
    return sum;
}

// dst = the target pump's RS485 address (its configured pump ID, 1-4).
inline uint8_t pentairBuildPacket(uint8_t *buf, uint8_t dst, uint8_t cmd,
                                   const uint8_t *data, uint8_t dLen) {
    buf[0] = 0xFF; buf[1] = 0x00; buf[2] = 0xFF;      // preamble
    buf[3] = 0xA5;                                     // SOF / checksum start
    buf[4] = 0x00;                                     // protocol version
    buf[5] = dst;
    buf[6] = PENTAIR_CTRL_ADDR;
    buf[7] = cmd;
    buf[8] = dLen;
    for (uint8_t i = 0; i < dLen; i++) buf[9 + i] = data[i];
    uint16_t ck = pentairChecksum(buf + 3, 6 + dLen);  // covers A5..end of data
    buf[9 + dLen]  = (uint8_t)(ck >> 8);                // high byte first
    buf[10 + dLen] = (uint8_t)(ck & 0xFF);
    return 11 + dLen;
}

inline bool pentairValidateResponse(const uint8_t *buf, uint8_t len) {
    int16_t sof = -1;
    for (uint8_t i = 0; i < len; i++) {
        if (buf[i] == 0xA5) { sof = i; break; }
    }
    if (sof < 0) {
        DBG_PRINTLN("[Pentair] validate FAILED: no 0xA5 start-of-frame found in response");
        return false;
    }
    if ((uint8_t)(len - sof) < 8) {
        DBG_PRINTLN("[Pentair] validate FAILED: response too short after SOF");
        return false;
    }
    uint8_t dataLen = buf[sof + 5];
    if ((uint8_t)(len - sof) < (uint8_t)(8 + dataLen)) {
        DBG_PRINT("[Pentair] validate FAILED: declared dataLen=");
        DBG_PRINT(dataLen);
        DBG_PRINTLN(" exceeds bytes actually received");
        return false;
    }
    uint16_t computed = pentairChecksum(buf + sof, 6 + dataLen);
    uint16_t received = ((uint16_t)buf[sof + 6 + dataLen] << 8) | buf[sof + 7 + dataLen];
    if (computed != received) {
        DBG_PRINTF("[Pentair] validate FAILED: checksum mismatch (computed=0x%04X received=0x%04X)\n", computed, received);
        return false;
    }
    DBG_PRINTLN("[Pentair] validate OK");
    return true;
}

// Converts our internal pump slot (1-4) into the pump's actual RS485 bus
// address. Pentair pumps are addressed 0x60-0x6F (set via dip switches on
// the pump board) - they do NOT use raw Modbus-style 1-4 addressing like
// the Emaux pumps do. Slot 1 -> 0x60, slot 2 -> 0x61, etc. If your physical
// pump's dip switches are set to a different address than this convention,
// adjust the mapping here to match.
inline uint8_t pentairBusAddress(uint8_t pumpId) {
    return PENTAIR_BASE_ADDR + (pumpId - 1);
}

// Sends one Pentair command to pumpId and waits for a checksum-valid reply.
inline bool pentairSendCmd(uint8_t pumpId, uint8_t cmd, const uint8_t *data, uint8_t dLen) {
    rs485UseBaud(RS485_BAUD); // Pentair talks at 9600
    DBG_PRINTF("[Pentair] pumpId=%u addr=0x%02X cmd=0x%02X dLen=%u\n",
               pumpId, pentairBusAddress(pumpId), cmd, dLen);
    uint8_t frame[16];
    uint8_t flen = pentairBuildPacket(frame, pentairBusAddress(pumpId), cmd, data, dLen);
    rs485Send(frame, flen);
    uint8_t resp[24];
    uint8_t rlen = rs485Receive(resp, sizeof(resp));
    if (rlen == 0) {
        DBG_PRINTLN("[Pentair] sendCmd FAILED: no response");
        return false;
    }
    bool ok = pentairValidateResponse(resp, rlen);
    DBG_PRINTF("[Pentair] sendCmd result: %s\n", ok ? "OK" : "FAILED");
    return ok;
}

// Must succeed once per pump before any speed/run command will be
// accepted. Retried on a timer (not every loop tick) so a pump that's
// still booting doesn't get flooded with handshake attempts.
inline bool pentairEnsureControl(uint8_t pumpId) {
    int idx = pumpId - 1;
    if (pentairControlTaken[idx]) return true;
    unsigned long now = millis();
    if (now - pentairControlLastTry[idx] < PENTAIR_CONTROL_RETRY_MS && pentairControlLastTry[idx] != 0) {
        DBG_PRINTF("[Pentair] pump %u control handshake on cooldown, %lums left\n",
                   pumpId, PENTAIR_CONTROL_RETRY_MS - (now - pentairControlLastTry[idx]));
        return false;
    }
    pentairControlLastTry[idx] = now;
    DBG_PRINTF("[Pentair] pump %u attempting 'take control' handshake\n", pumpId);
    uint8_t data[] = {0xFF, 0xFF}; // "external controller present"
    if (pentairSendCmd(pumpId, PENTAIR_CMD_CONTROL, data, 2)) {
        pentairControlTaken[idx] = true;
        DBG_PRINTF("[Pentair] pump %u control handshake SUCCEEDED\n", pumpId);
        return true;
    }
    DBG_PRINTF("[Pentair] pump %u control handshake FAILED, will retry in %lums\n", pumpId, PENTAIR_CONTROL_RETRY_MS);
    return false;
}

inline bool pentairSetSpeed(uint8_t pumpId, uint16_t rpm) {
    if (rpm < PENTAIR_RPM_MIN) rpm = PENTAIR_RPM_MIN;
    if (rpm > PENTAIR_RPM_MAX) rpm = PENTAIR_RPM_MAX;
    uint8_t data[] = { 0x02, 0xC4, (uint8_t)(rpm >> 8), (uint8_t)(rpm & 0xFF) };
    return pentairSendCmd(pumpId, PENTAIR_CMD_SPEED, data, 4);
}

inline bool pentairRun(uint8_t pumpId, bool run) {
    uint8_t data[] = { run ? (uint8_t)PENTAIR_RUN : (uint8_t)PENTAIR_STOP };
    return pentairSendCmd(pumpId, PENTAIR_CMD_RUN, data, 1);
}

// ============================================================
//  Emaux (Modbus RTU write-single-register, FC 0x06)
// ============================================================
inline uint16_t emauxCrc(const uint8_t *buf, uint8_t len) {
    uint16_t crc = 0xFFFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xA001;
            else crc >>= 1;
        }
    }
    return crc;
}

// pumpAddr = the target pump's configured Modbus slave address (1-4).
inline bool emauxWriteRegister(uint8_t pumpAddr, uint16_t reg, uint16_t val) {
    rs485UseBaud(RS485_BAUD); // Emaux talks at 9600
    DBG_PRINTF("[Emaux] pumpAddr=%u reg=0x%04X val=%u\n", pumpAddr, reg, val);
    uint8_t frame[8] = {
        pumpAddr, EMAUX_FC_WRITE,
        (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
        (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)
    };
    uint16_t crc = emauxCrc(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    rs485Send(frame, 8);

    uint8_t resp[8];
    uint8_t rlen = rs485Receive(resp, sizeof(resp));
    if (rlen < 8) {
        DBG_PRINTF("[Emaux] writeRegister FAILED: short response (%u bytes, expected 8)\n", rlen);
        return false;
    }

    uint16_t rReceived = (uint16_t)resp[7] << 8 | resp[6];
    uint16_t rComputed = emauxCrc(resp, 6);
    if (rReceived != rComputed) {
        DBG_PRINTF("[Emaux] writeRegister FAILED: CRC mismatch (computed=0x%04X received=0x%04X)\n", rComputed, rReceived);
        return false;
    }

    // A correct write echoes address + function code back
    bool ok = (resp[0] == pumpAddr && resp[1] == EMAUX_FC_WRITE);
    if (!ok) {
        DBG_PRINTF("[Emaux] writeRegister FAILED: unexpected echo (addr=0x%02X fc=0x%02X)%s\n",
                   resp[0], resp[1], (resp[1] & 0x80) ? " -- looks like a Modbus exception response" : "");
    } else {
        DBG_PRINTLN("[Emaux] writeRegister OK");
    }
    return ok;
}

inline bool emauxSetSpeed(uint8_t pumpId, uint16_t rpm) {
    if (rpm < EMAUX_RPM_MIN) rpm = EMAUX_RPM_MIN;
    if (rpm > EMAUX_RPM_MAX) rpm = EMAUX_RPM_MAX;
    return emauxWriteRegister(pumpId, EMAUX_REG_SPEED, rpm);
}

inline bool emauxRun(uint8_t pumpId, bool run) {
    return emauxWriteRegister(pumpId, EMAUX_REG_RUN, run ? EMAUX_RUN_CMD : EMAUX_STOP_CMD);
}

// ============================================================
//  Black & Decker (Modbus RTU write-single-register, FC 0x06, 38400 baud)
//  pumpAddr follows the slot convention (pump N = address N), and since
//  the pump is fixed at address 1, only slot 1 ever reaches this driver
//  (enforced in the dispatcher below).
// ============================================================
inline bool bdWriteRegister(uint8_t pumpAddr, uint16_t reg, uint16_t val) {
    rs485UseBaud(BD_BAUD);
    DBG_PRINTF("[B&D] pumpAddr=%u reg=0x%04X val=%u\n", pumpAddr, reg, val);
    uint8_t frame[8] = {
        pumpAddr, BD_FC_WRITE,
        (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
        (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)
    };
    uint16_t crc = emauxCrc(frame, 6); // standard Modbus CRC16 — shared
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    rs485Send(frame, 8);

    uint8_t resp[8];
    uint8_t rlen = rs485Receive(resp, sizeof(resp));
    if (rlen < 8) {
        DBG_PRINTF("[B&D] writeRegister FAILED: short response (%u bytes, expected 8)\n", rlen);
        return false;
    }
    uint16_t rReceived = (uint16_t)resp[7] << 8 | resp[6];
    uint16_t rComputed = emauxCrc(resp, 6);
    if (rReceived != rComputed) {
        DBG_PRINTF("[B&D] writeRegister FAILED: CRC mismatch (computed=0x%04X received=0x%04X)\n", rComputed, rReceived);
        return false;
    }
    // A correct write echoes address + function code back
    bool ok = (resp[0] == pumpAddr && resp[1] == BD_FC_WRITE);
    if (!ok) {
        DBG_PRINTF("[B&D] writeRegister FAILED: unexpected echo (addr=0x%02X fc=0x%02X)%s\n",
                   resp[0], resp[1], (resp[1] & 0x80) ? " -- looks like a Modbus exception response" : "");
    } else {
        DBG_PRINTLN("[B&D] writeRegister OK");
    }
    return ok;
}

inline bool bdSetSpeed(uint8_t pumpId, uint16_t rpm) {
    if (rpm < BD_RPM_MIN) rpm = BD_RPM_MIN;
    if (rpm > BD_RPM_MAX) rpm = BD_RPM_MAX;
    return bdWriteRegister(pumpId, BD_REG_SPEED, (uint16_t)(rpm * BD_RPM_SCALE));
}

inline bool bdRun(uint8_t pumpId, bool run) {
    return bdWriteRegister(pumpId, BD_REG_RUN, run ? BD_RUN_CMD : BD_STOP_CMD);
}

// ============================================================
//  Unified dispatcher — called every loop tick from relay_timer.ino
//  with the pump's *desired* state (from schedule or manual override).
//
//  Pump 1 -> RS485 address 1, Pump 2 -> address 2, Pump 3 -> address 3,
//  Pump 4 -> address 4 (set each physical pump's address accordingly).
//
//  Only sends bus traffic when the desired state differs from what was
//  last CONFIRMED applied, and only marks it confirmed once the pump
//  acknowledges the command — so a dropped/failed frame is automatically
//  retried on the next tick instead of silently being considered "done".
// ============================================================
inline void updatePumpPhysicalState(uint8_t pumpId, uint8_t pumpType, bool run, uint16_t speed) {
    if (pumpId < 1 || pumpId > MAX_PUMPS) {
        DBG_PRINTF("[Dispatcher] IGNORED: pumpId %u out of range (1-%d)\n", pumpId, MAX_PUMPS);
        return;
    }
    int idx = pumpId - 1;

    // When commanding OFF, always talk to the pump using whatever protocol
    // it was actually last driven with. The caller's pumpType can be stale
    // here (e.g. RELAY_TIMER_1.ino falls back to the schedule's pumpType
    // when there's no active preset, which defaults to PUMP_PENTAIR) --
    // sending a stop frame in the wrong protocol never gets acknowledged,
    // so the pump looks stuck "on" forever.
    if (!run) {
        uint8_t requestedType = pumpType;
        pumpType = currentPumpStates[idx].type;
        if (requestedType != pumpType) {
            DBG_PRINTF("[Dispatcher] pump %u: overriding requested type %u with last-known type %u for STOP\n",
                       pumpId, requestedType, pumpType);
        }
    }

    bool alreadyApplied = (currentPumpStates[idx].active == run) &&
                          (!run || currentPumpStates[idx].speed == speed);
    if (alreadyApplied) {
        DBG_PRINTF("[Dispatcher] pump %u: state already applied (active=%d speed=%u), skipping\n",
                   pumpId, currentPumpStates[idx].active, currentPumpStates[idx].speed);
        return;
    }

    // Black & Decker exists on pump slot 1 only (fixed Modbus address 1).
    // Every input path validates this already — this is the last line of
    // defense so a mis-addressed command can never reach the bus.
    if (pumpType == PUMP_BLACKDECKER && pumpId != 1) {
        DBG_PRINTF("[Dispatcher] IGNORED: Black & Decker is only supported on pump 1 (got pump %u)\n", pumpId);
        return;
    }

    const char* typeName = (pumpType == PUMP_PENTAIR) ? "PENTAIR"
                         : (pumpType == PUMP_BLACKDECKER) ? "BLACK&DECKER" : "EMAUX";
    DBG_PRINTF("[Dispatcher] pump %u: desired run=%d speed=%u type=%s (current active=%d speed=%u)\n",
               pumpId, run, speed, typeName,
               currentPumpStates[idx].active, currentPumpStates[idx].speed);

    bool ok = true;
    if (pumpType == PUMP_PENTAIR) {
        if (!pentairEnsureControl(pumpId)) {
            DBG_PRINTF("[Dispatcher] pump %u: Pentair control not yet taken, will retry next tick\n", pumpId);
            return; // handshake not done yet, retry next tick
        }
        if (run) {
            ok = pentairSetSpeed(pumpId, speed) && ok;
            ok = pentairRun(pumpId, true) && ok;
        } else {
            ok = pentairRun(pumpId, false) && ok;
        }
    } else if (pumpType == PUMP_BLACKDECKER) {
        if (run) {
            ok = bdSetSpeed(pumpId, speed) && ok;
            ok = bdRun(pumpId, true) && ok;
        } else {
            ok = bdRun(pumpId, false) && ok;
        }
    } else { // PUMP_EMAUX
        if (run) {
            ok = emauxSetSpeed(pumpId, speed) && ok;
            ok = emauxRun(pumpId, true) && ok;
        } else {
            ok = emauxRun(pumpId, false) && ok;
        }
    }

    if (ok) {
        currentPumpStates[idx].active = run;
        currentPumpStates[idx].speed = run ? speed : 0;
        if (run) currentPumpStates[idx].type = pumpType;
        DBG_PRINTF("[Dispatcher] pump %u: state CONFIRMED applied (active=%d speed=%u)\n",
                   pumpId, currentPumpStates[idx].active, currentPumpStates[idx].speed);
    } else {
        DBG_PRINTF("[Dispatcher] pump %u: command FAILED, state left unchanged for retry next tick\n", pumpId);
    }
    // if !ok, currentPumpStates is left unchanged so the next loop tick
    // sees the state as "not yet applied" and retries automatically.
}

// Read-only accessor for the web UI / status API.
inline void pumpActualStateGet(uint8_t pumpId, bool &active, uint16_t &speed) {
    if (pumpId < 1 || pumpId > MAX_PUMPS) { active = false; speed = 0; return; }
    active = currentPumpStates[pumpId - 1].active;
    speed  = currentPumpStates[pumpId - 1].speed;
}
