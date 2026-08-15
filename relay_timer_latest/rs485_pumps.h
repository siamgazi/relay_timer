#pragma once
#include <Arduino.h>

// ============================================================
//  RS485 PUMP DRIVERS — Pentair / Emaux / Black & Decker
// ============================================================
// One bus, two speeds, three protocols:
//
//   Pentair  (slot addr 0x60+n) — proprietary framing, 9600 baud.
//             CONFIRMED: every command waits for a checksum-valid reply
//             (after a one-time "take control" handshake), and the state
//             flag only flips once a status readback (cmd 0x07) reports
//             the commanded speed/stop.
//   Emaux    (slot addr = slot) — Modbus RTU FC06, 9600 baud.
//             CONFIRMED: the write echo is required, then the pump's
//             input registers are read back until they report the
//             commanded state (a pump can echo a speed write yet start
//             at its panel-set speed — the readback catches that and
//             triggers a re-send).
//   B&D      (fixed addr 1, pump slot 1 ONLY) — Modbus RTU FC06, 38400.
//             FIRE-AND-FORGET by design: one setpoint write (speed to
//             run, 0 to stop), NO reply is awaited and RX is never
//             listened to for this pump. "ON" therefore means "command
//             was sent", not "pump acknowledged".
//
// Workflow (per bring-up spec): select the family's baud rate first —
// if that changes the UART, wait 500ms — then send. Same-family
// commands in a row pay no settle delay.

// ---- Debug logging -----------------------------------------
// RS485_DEBUG: compile-time master switch. Set to 0 to strip all debug
// code out of the build entirely (smallest/fastest production build).
#ifndef RS485_DEBUG
#define RS485_DEBUG 1
#endif

#if RS485_DEBUG
  // Runtime switch; output only prints while true. Defaulted ON for
  // bring-up — the log follows a print-on-change discipline, so a
  // healthy system is quiet. Set false once the pumps are proven.
  bool rs485DebugEnabled = true;

  inline void rs485SetDebug(bool on) {
      rs485DebugEnabled = on;
      Serial.print("[RS485] debug ");
      Serial.println(on ? "ENABLED" : "DISABLED");
  }

  #define DBG_PRINT(...)    do { if (rs485DebugEnabled) Serial.print(__VA_ARGS__); } while (0)
  #define DBG_PRINTLN(...)  do { if (rs485DebugEnabled) Serial.println(__VA_ARGS__); } while (0)
  #define DBG_PRINTF(...)   do { if (rs485DebugEnabled) Serial.printf(__VA_ARGS__); } while (0)
#else
  inline void rs485SetDebug(bool on) { (void)on; }
  #define DBG_PRINT(...)
  #define DBG_PRINTLN(...)
  #define DBG_PRINTF(...)
#endif

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

// ---- Bus / pins --------------------------------------------
#define RS485_SERIAL   Serial2
#define DE_RE_PIN      4
#define RS485_RX_GPIO  18
#define RS485_TX_GPIO  17
#define RS485_BAUD     9600UL   // home baud: Pentair + Emaux

#define RESPONSE_TIMEOUT_MS   500  // Emaux: max wait for first reply byte
#define PENTAIR_TIMEOUT_MS    1000 // Pentair replies can be slower (proven value)
#define INTERCHAR_TIMEOUT_MS  15   // max gap between reply bytes
#define RS485_BAUD_SETTLE_MS  500  // spec: settle after every baud change

// ---- Closed-loop verification (Pentair + Emaux) ------------
// After a command is echoed OK, the pump's OWN status is polled until it
// reports the commanded state: ON = reported speed within tolerance of
// the target, OFF = reported stopped. Until the readback matches, the
// command is re-sent periodically (the app's flow: speed -> start ->
// status; if the status doesn't show the speed, send the speed again).
#define VERIFY_POLL_MS    1000  // status readback cadence while unverified
#define VERIFY_RESEND_MS  5000  // re-send the command if still not matching
#define VERIFY_RPM_TOL    50    // regulation jitter allowance on the readback

// ---- Pentair IntelliFlo VS/VF (proprietary RS485) ----------
#define PENTAIR_CTRL_ADDR     0x10  // us, the automation controller
#define PENTAIR_BASE_ADDR     0x60  // slot 1 = 0x60, slot 2 = 0x61, ...
#define PENTAIR_CMD_SPEED     0x01
#define PENTAIR_CMD_CONTROL   0x04  // one-time "take control" handshake
#define PENTAIR_CMD_RUN       0x06
#define PENTAIR_CMD_STATUS    0x07  // readback: run state + current RPM
#define PENTAIR_RUN           0x0A
#define PENTAIR_STOP          0x04
#define PENTAIR_RPM_MIN       450
#define PENTAIR_RPM_MAX       3450
#define PENTAIR_CONTROL_RETRY_MS 3000

// ---- Emaux (Modbus RTU FC06) -------------------------------
#define EMAUX_FC_WRITE       0x06
#define EMAUX_FC_READ_INPUT  0x04
#define EMAUX_REG_RUN        0x0008
#define EMAUX_REG_SPEED      0x0009
#define EMAUX_REG_CUR_SPEED  0x0001  // input reg: current speed (RPM)
#define EMAUX_REG_RUN_STATUS 0x0003  // input reg: 1 = running, 0 = stopped
#define EMAUX_RUN_CMD      1
#define EMAUX_STOP_CMD     2
#define EMAUX_RPM_MIN      800
#define EMAUX_RPM_MAX      3400

// ---- Black & Decker (Modbus RTU FC06, 38400, addr 1 only) --
#define BD_BAUD         38400UL
#define BD_FC_WRITE     0x06
#define BD_REG_SPEED    0x0007
#define BD_RPM_SCALE    5      // register value = rpm * 5
#define BD_RPM_MIN      600    // adjust to your pump's real envelope
#define BD_RPM_MAX      3450
// Fire-and-forget reliability: with no acknowledgment ever read, the
// same frame is fired several times so a single lost/garbled frame
// can't mean a missed command. Writes are idempotent (same register,
// same value), so repeats are harmless. 3 shots with 150ms gaps span
// ~330ms — inside the app's 500ms "Turning on…" feedback window.
#define BD_SEND_REPEATS   3
#define BD_REPEAT_GAP_MS  150

// ---- Shared state ------------------------------------------
struct PumpState {
    bool active = false;
    uint16_t speed = 0;
    uint8_t  type = PUMP_PENTAIR; // protocol this pump was last driven with
};
PumpState currentPumpStates[MAX_PUMPS];

// Pentair needs its handshake once per pump before accepting commands.
bool pentairControlTaken[MAX_PUMPS] = {false, false, false, false};
unsigned long pentairControlLastTry[MAX_PUMPS] = {0, 0, 0, 0};

// Per-pump memo of the last situation the serial log REPORTED — the log
// only speaks when something changes: a new desired state, the first
// failure of it, or the eventual confirmation. Retries stay silent.
struct PumpLogMemo {
    bool     haveDesired = false;
    bool     run = false;
    uint16_t speed = 0;
    uint8_t  type = 0;
    bool     failLogged = false;
};
PumpLogMemo pumpLogMemo[MAX_PUMPS];

// Closed-loop bookkeeping (Pentair/Emaux): a command is only "applied"
// once the pump's own status readback matches it.
struct PumpVerify {
    bool     awaiting = false;     // echoed command awaiting a matching readback
    bool     everSent = false;     // first send of the current goal already logged
    bool     pollLogged = false;   // first status poll already logged
    bool     resendLogged = false;
    uint32_t lastSendMs = 0;
    uint32_t lastPollMs = 0;
};
PumpVerify pumpVerify[MAX_PUMPS];

// ---- Bus lifecycle -----------------------------------------
uint32_t rs485CurrentBaud = RS485_BAUD;

inline void rs485Begin() {
    RS485_SERIAL.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_GPIO, RS485_TX_GPIO);
    RS485_SERIAL.setRxBufferSize(1024);
    pinMode(DE_RE_PIN, OUTPUT);
    digitalWrite(DE_RE_PIN, LOW);
    rs485CurrentBaud = RS485_BAUD;
    DBG_PRINTF("[RS485] begin: baud=%lu RX=%d TX=%d DE/RE=%d\n",
               (unsigned long)RS485_BAUD, RS485_RX_GPIO, RS485_TX_GPIO, DE_RE_PIN);
}

// Retunes the UART when the next transaction targets the other bus
// speed, then settles for 500ms (spec). updateBaudRate only rewrites the
// clock divisor — pin routing is never touched — and the actual rate is
// logged with every TX, so a failed retune would be visible.
inline void rs485UseBaud(uint32_t baud) {
    if (rs485CurrentBaud == baud) return;
    RS485_SERIAL.flush();
    RS485_SERIAL.updateBaudRate(baud);
    rs485CurrentBaud = baud;
    DBG_PRINTF("[RS485] baud switched to %lu (readback %lu), settling %dms\n",
               (unsigned long)baud, (unsigned long)RS485_SERIAL.baudRate(),
               RS485_BAUD_SETTLE_MS);
    delay(RS485_BAUD_SETTLE_MS);
}

// ---- Low-level transceiver ---------------------------------
inline void rs485FlushRx() {
    while (RS485_SERIAL.available()) RS485_SERIAL.read();
}

// Wire time of one byte at the current baud, plus margin — waited after
// flush() so the last stop bit clears the bus before DE drops (the
// proven timing from the standalone Pentair/Emaux sketches).
#define RS485_BYTE_TIME_US (10UL * 1000000UL / rs485CurrentBaud + 300UL)

#define RS485_INTERFRAME_DELAY_MS 10

inline void rs485Send(const uint8_t *frame, uint8_t len) {
    delay(RS485_INTERFRAME_DELAY_MS); // let the bus settle before we grab it
    rs485FlushRx();                   // stale bytes must not pollute the reply
    DBG_PRINT("[RS485] TX (");
    DBG_PRINT(len);
    DBG_PRINT(" bytes @ ");
    DBG_PRINT(RS485_SERIAL.baudRate());
    DBG_PRINT(" baud): ");
    dbgHexDump(frame, len);

    digitalWrite(DE_RE_PIN, HIGH);
    delayMicroseconds(100);
    RS485_SERIAL.write(frame, len);
    RS485_SERIAL.flush();
    delayMicroseconds(RS485_BYTE_TIME_US);
    digitalWrite(DE_RE_PIN, LOW);
    // No post-TX RX discard: an eager cleanup here once ate the front of
    // fast replies. Anything arriving after TX belongs to the receiver.
}

// Two-stage receive with glitch tolerance: leading 0x00 bytes are the
// DE-release artifact (no legitimate reply starts with 0x00 — Modbus
// addresses are 1+, Pentair frames start 0xFF), so they're stripped and
// listening continues until the deadline instead of letting noise
// anchor-and-close the window. Used by Pentair and Emaux only — the
// Black & Decker path never reads RX at all.
inline uint8_t rs485Receive(uint8_t *buf, uint8_t maxLen, uint16_t timeoutMs) {
    unsigned long deadline = millis() + timeoutMs;
    while (true) {
        while (!RS485_SERIAL.available()) {
            if (millis() > deadline) {
                DBG_PRINTLN("[RS485] RX timeout: no bytes received");
                return 0;
            }
        }
        unsigned long firstByteAt = millis();
        uint8_t idx = 0;
        unsigned long last = millis();
        while (idx < maxLen) {
            if (RS485_SERIAL.available()) {
                buf[idx++] = RS485_SERIAL.read();
                last = millis();
            } else if (millis() - last > INTERCHAR_TIMEOUT_MS) {
                break;
            }
        }

        if (idx == maxLen) {
            DBG_PRINTLN("[RS485] WARNING: RX buffer full — tail of the frame may be unread");
        }

        uint8_t skip = 0;
        while (skip < idx && buf[skip] == 0x00) skip++;
        if (skip > 0) {
            DBG_PRINTF("[RS485] stripped %u leading glitch byte(s)\n", skip);
            memmove(buf, buf + skip, idx - skip);
            idx -= skip;
        }
        if (idx == 0) continue; // pure glitch — the real reply may still come

        DBG_PRINT("[RS485] RX (");
        DBG_PRINT(idx);
        DBG_PRINT(" bytes, ");
        DBG_PRINT(millis() - firstByteAt);
        DBG_PRINT("ms after first byte): ");
        dbgHexDump(buf, idx);
        return idx;
    }
}

// ---- Modbus CRC16 (shared by Emaux + B&D) ------------------
inline uint16_t crc16Modbus(const uint8_t *buf, uint8_t len) {
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

// ============================================================
//  Pentair (9600, confirmed)
// ============================================================
inline uint16_t pentairChecksum(const uint8_t *buf, uint8_t len) {
    uint16_t sum = 0;
    for (uint8_t i = 0; i < len; i++) sum += buf[i];
    return sum;
}

inline uint8_t pentairBuildPacket(uint8_t *buf, uint8_t dst, uint8_t cmd,
                                   const uint8_t *data, uint8_t dLen) {
    buf[0] = 0xFF; buf[1] = 0x00; buf[2] = 0xFF;   // preamble
    buf[3] = 0xA5;                                  // SOF / checksum start
    buf[4] = 0x00;                                  // protocol version
    buf[5] = dst;
    buf[6] = PENTAIR_CTRL_ADDR;
    buf[7] = cmd;
    buf[8] = dLen;
    for (uint8_t i = 0; i < dLen; i++) buf[9 + i] = data[i];
    uint16_t ck = pentairChecksum(buf + 3, 6 + dLen);
    buf[9 + dLen]  = (uint8_t)(ck >> 8);            // high byte first
    buf[10 + dLen] = (uint8_t)(ck & 0xFF);
    return 11 + dLen;
}

inline bool pentairValidateResponse(const uint8_t *buf, uint8_t len) {
    int16_t sof = -1;
    for (uint8_t i = 0; i < len; i++) {
        if (buf[i] == 0xA5) { sof = i; break; }
    }
    if (sof < 0) { DBG_PRINTLN("[Pentair] validate FAILED: no 0xA5 SOF"); return false; }
    if ((uint8_t)(len - sof) < 8) { DBG_PRINTLN("[Pentair] validate FAILED: too short after SOF"); return false; }
    uint8_t dataLen = buf[sof + 5];
    if ((uint8_t)(len - sof) < (uint8_t)(8 + dataLen)) {
        DBG_PRINTLN("[Pentair] validate FAILED: declared dataLen exceeds bytes received");
        return false;
    }
    uint16_t computed = pentairChecksum(buf + sof, 6 + dataLen);
    uint16_t received = ((uint16_t)buf[sof + 6 + dataLen] << 8) | buf[sof + 7 + dataLen];
    if (computed != received) {
        DBG_PRINTF("[Pentair] validate FAILED: checksum (computed=0x%04X received=0x%04X)\n", computed, received);
        return false;
    }
    return true;
}

// Slot (1-4) -> bus address 0x60-0x63 (dip-switch convention).
inline uint8_t pentairBusAddress(uint8_t pumpId) {
    return PENTAIR_BASE_ADDR + (pumpId - 1);
}

inline bool pentairSendCmd(uint8_t pumpId, uint8_t cmd, const uint8_t *data, uint8_t dLen) {
    rs485UseBaud(RS485_BAUD);
    uint8_t frame[32];
    uint8_t flen = pentairBuildPacket(frame, pentairBusAddress(pumpId), cmd, data, dLen);
    rs485Send(frame, flen);
    uint8_t resp[40];
    uint8_t rlen = rs485Receive(resp, sizeof(resp), PENTAIR_TIMEOUT_MS);
    if (rlen == 0) return false;
    return pentairValidateResponse(resp, rlen);
}

// One-time handshake per pump, retried on a cooldown.
inline bool pentairEnsureControl(uint8_t pumpId) {
    int idx = pumpId - 1;
    if (pentairControlTaken[idx]) return true;
    unsigned long now = millis();
    if (pentairControlLastTry[idx] != 0 && now - pentairControlLastTry[idx] < PENTAIR_CONTROL_RETRY_MS)
        return false;
    pentairControlLastTry[idx] = now;
    uint8_t data[] = {0xFF, 0xFF}; // "external controller present"
    if (pentairSendCmd(pumpId, PENTAIR_CMD_CONTROL, data, 2)) {
        pentairControlTaken[idx] = true;
        DBG_PRINTF("[Pentair] pump %u control handshake OK\n", pumpId);
        return true;
    }
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

// Status readback: payload is [run mode drivestate watts_hi watts_lo
// rpm_hi rpm_lo ...] — run==0x0A means running.
inline bool pentairReadStatus(uint8_t pumpId, bool &running, uint16_t &rpm) {
    rs485UseBaud(RS485_BAUD);
    uint8_t frame[32];
    uint8_t flen = pentairBuildPacket(frame, pentairBusAddress(pumpId), PENTAIR_CMD_STATUS, nullptr, 0);
    rs485Send(frame, flen);
    uint8_t resp[48];
    uint8_t rlen = rs485Receive(resp, sizeof(resp), PENTAIR_TIMEOUT_MS);
    if (rlen == 0 || !pentairValidateResponse(resp, rlen)) return false;
    int16_t sof = -1;
    for (uint8_t i = 0; i < rlen; i++) {
        if (resp[i] == 0xA5) { sof = i; break; }
    }
    uint8_t dataLen = resp[sof + 5];
    if (dataLen < 7) { DBG_PRINTLN("[Pentair] status payload too short"); return false; }
    const uint8_t *d = resp + sof + 6;
    running = (d[0] == PENTAIR_RUN);
    rpm = ((uint16_t)d[5] << 8) | d[6];
    DBG_PRINTF("[Pentair] pump %u status: %s @ %u rpm\n", pumpId, running ? "RUNNING" : "stopped", rpm);
    return true;
}

// ============================================================
//  Emaux (9600, Modbus FC06, confirmed)
// ============================================================
inline bool emauxWriteRegister(uint8_t pumpAddr, uint16_t reg, uint16_t val) {
    rs485UseBaud(RS485_BAUD);
    DBG_PRINTF("[Emaux] pumpAddr=%u reg=0x%04X val=%u\n", pumpAddr, reg, val);
    uint8_t frame[8] = {
        pumpAddr, EMAUX_FC_WRITE,
        (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
        (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)
    };
    uint16_t crc = crc16Modbus(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    rs485Send(frame, 8);

    // 16-byte headroom: a glitch byte in an exactly-sized buffer once
    // displaced the echo's final CRC byte and failed every confirm.
    uint8_t resp[16];
    uint8_t rlen = rs485Receive(resp, sizeof(resp), RESPONSE_TIMEOUT_MS);
    if (rlen < 8) {
        DBG_PRINTF("[Emaux] writeRegister FAILED: short response (%u bytes)\n", rlen);
        return false;
    }
    uint16_t rReceived = (uint16_t)resp[7] << 8 | resp[6];
    if (rReceived != crc16Modbus(resp, 6)) {
        DBG_PRINTLN("[Emaux] writeRegister FAILED: CRC mismatch");
        return false;
    }
    bool ok = (resp[0] == pumpAddr && resp[1] == EMAUX_FC_WRITE);
    if (!ok) {
        DBG_PRINTF("[Emaux] writeRegister FAILED: unexpected echo (addr=0x%02X fc=0x%02X)%s\n",
                   resp[0], resp[1], (resp[1] & 0x80) ? " -- Modbus exception" : "");
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

// Read one input register (FC04) — the pump's own report of what it is
// actually doing. Returns the value, or -1 on any error.
inline int32_t emauxReadInputRegister(uint8_t pumpAddr, uint16_t reg) {
    rs485UseBaud(RS485_BAUD);
    uint8_t frame[8] = {
        pumpAddr, EMAUX_FC_READ_INPUT,
        (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
        0x00, 0x01
    };
    uint16_t crc = crc16Modbus(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;

    rs485Send(frame, 8);

    // Expected: addr fc byteCount val_hi val_lo crc_lo crc_hi = 7 bytes
    // (16-byte headroom for glitch bytes, same as the write path).
    uint8_t resp[16];
    uint8_t rlen = rs485Receive(resp, sizeof(resp), RESPONSE_TIMEOUT_MS);
    if (rlen < 7) {
        DBG_PRINTF("[Emaux] read reg 0x%04X FAILED: short response (%u bytes)\n", reg, rlen);
        return -1;
    }
    uint16_t rReceived = (uint16_t)resp[6] << 8 | resp[5];
    if (rReceived != crc16Modbus(resp, 5)) {
        DBG_PRINTF("[Emaux] read reg 0x%04X FAILED: CRC mismatch\n", reg);
        return -1;
    }
    if (resp[0] != pumpAddr || resp[1] != EMAUX_FC_READ_INPUT) {
        DBG_PRINTF("[Emaux] read reg 0x%04X FAILED: unexpected reply (addr=0x%02X fc=0x%02X)%s\n",
                   reg, resp[0], resp[1], (resp[1] & 0x80) ? " -- Modbus exception" : "");
        return -1;
    }
    return (int32_t)(((uint16_t)resp[3] << 8) | resp[4]);
}

// ============================================================
//  Black & Decker (38400, Modbus FC06, FIRE-AND-FORGET)
// ============================================================
// One setpoint write: speed to run, 0 to stop. Per spec, NO reply is
// awaited and RX is never read for this pump — the frame goes out and
// the state flag flips immediately ("sent" semantics, user-approved).
inline void bdSendSpeed(uint8_t pumpId, uint16_t rpm) {
    if (rpm > 0) {
        if (rpm < BD_RPM_MIN) rpm = BD_RPM_MIN;
        if (rpm > BD_RPM_MAX) rpm = BD_RPM_MAX;
    }
    rs485UseBaud(BD_BAUD);
    uint16_t val = (uint16_t)(rpm * BD_RPM_SCALE);
    DBG_PRINTF("[B&D] pumpAddr=%u reg=0x%04X val=%u (%u rpm) — fire-and-forget x%d\n",
               pumpId, BD_REG_SPEED, val, rpm, BD_SEND_REPEATS);
    uint8_t frame[8] = {
        pumpId, BD_FC_WRITE,
        (uint8_t)(BD_REG_SPEED >> 8), (uint8_t)(BD_REG_SPEED & 0xFF),
        (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)
    };
    uint16_t crc = crc16Modbus(frame, 6);
    frame[6] = crc & 0xFF;
    frame[7] = (crc >> 8) & 0xFF;
    for (uint8_t i = 0; i < BD_SEND_REPEATS; i++) {
        if (i > 0) delay(BD_REPEAT_GAP_MS);
        rs485Send(frame, 8);
    }
    // Deliberately no rs485Receive() here.
}

// ============================================================
//  Dispatcher — called every tick from relay_timer_latest.ino with each
//  pump's desired state. Sends only when the desired state differs from
//  the last applied one. Pentair/Emaux run a closed loop: command
//  (echo-confirmed) -> status readback until the pump itself reports
//  the commanded state, re-sending on mismatch. Black & Decker marks
//  applied the moment the frame is sent.
// ============================================================
inline void updatePumpPhysicalState(uint8_t pumpId, uint8_t pumpType, bool run, uint16_t speed) {
    if (pumpId < 1 || pumpId > MAX_PUMPS) return;
    int idx = pumpId - 1;

    // Commanding OFF uses the protocol the pump was actually last driven
    // with — a stop frame in the wrong protocol never lands.
    if (!run) pumpType = currentPumpStates[idx].type;

    bool alreadyApplied = (currentPumpStates[idx].active == run) &&
                          (!run || currentPumpStates[idx].speed == speed);
    if (alreadyApplied) return;

    // Print-on-change bookkeeping.
    PumpLogMemo &memo = pumpLogMemo[idx];
    bool desiredChanged = !memo.haveDesired || memo.run != run ||
                          memo.speed != speed || memo.type != pumpType;
    if (desiredChanged) {
        memo.haveDesired = true;
        memo.run = run;
        memo.speed = speed;
        memo.type = pumpType;
        memo.failLogged = false;
    }

    // Black & Decker is pump-1-only (fixed bus address 1).
    if (pumpType == PUMP_BLACKDECKER && pumpId != 1) {
        if (desiredChanged)
            DBG_PRINTF("[Dispatcher] IGNORED: Black & Decker is only supported on pump 1 (got pump %u)\n", pumpId);
        return;
    }

    const char* typeName = (pumpType == PUMP_PENTAIR) ? "PENTAIR"
                         : (pumpType == PUMP_BLACKDECKER) ? "BLACK&DECKER" : "EMAUX";

    bool verboseAttempt = !memo.failLogged;
    if (verboseAttempt && desiredChanged) {
        DBG_PRINTF("[Dispatcher] pump %u: desired run=%d speed=%u type=%s (current active=%d speed=%u)\n",
                   pumpId, run, speed, typeName,
                   currentPumpStates[idx].active, currentPumpStates[idx].speed);
    }

    // ---- Black & Decker: open loop, applied on send ----------------
    if (pumpType == PUMP_BLACKDECKER) {
        bdSendSpeed(pumpId, run ? speed : 0);
        currentPumpStates[idx].active = run;
        currentPumpStates[idx].speed = run ? speed : 0;
        currentPumpStates[idx].type = PUMP_BLACKDECKER;
        DBG_PRINTF("[Dispatcher] pump %u: command SENT open-loop (active=%d speed=%u)\n",
                   pumpId, currentPumpStates[idx].active, currentPumpStates[idx].speed);
        return;
    }

    // ---- Pentair / Emaux: closed loop ------------------------------
    // Phase 1 (send): speed then start (stop alone for OFF), each write
    // confirmed by its echo. Phase 2 (verify): poll the pump's own
    // status; only a readback matching the target flips the state flag.
    // A mismatch after VERIFY_RESEND_MS re-enters phase 1.
    PumpVerify &ver = pumpVerify[idx];
    if (desiredChanged) {
        ver.awaiting = false;
        ver.everSent = false;
        ver.pollLogged = false;
        ver.resendLogged = false;
    }

    // The target the readback must show — the same clamping the setters apply.
    uint16_t target = speed;
    if (run) {
        uint16_t lo = (pumpType == PUMP_PENTAIR) ? PENTAIR_RPM_MIN : EMAUX_RPM_MIN;
        uint16_t hi = (pumpType == PUMP_PENTAIR) ? PENTAIR_RPM_MAX : EMAUX_RPM_MAX;
        if (target < lo) target = lo;
        if (target > hi) target = hi;
    }

    uint32_t nowMs = millis();

    if (!ver.awaiting) {
        // ---- Phase 1: send, confirmed by echo ----------------------
        bool verboseSend = verboseAttempt && !ver.everSent;
#if RS485_DEBUG
        bool dbgSave = rs485DebugEnabled;
        if (!verboseSend) rs485DebugEnabled = false;
#endif
        bool ok = true;
        if (pumpType == PUMP_PENTAIR) {
            if (!pentairEnsureControl(pumpId)) {
#if RS485_DEBUG
                rs485DebugEnabled = dbgSave;
#endif
                if (!memo.failLogged) {
                    memo.failLogged = true;
                    DBG_PRINTF("[Dispatcher] pump %u: Pentair control handshake pending — retrying silently\n", pumpId);
                }
                return;
            }
            if (run) {
                ok = pentairSetSpeed(pumpId, speed) && ok;
                ok = pentairRun(pumpId, true) && ok;
            } else {
                ok = pentairRun(pumpId, false) && ok;
            }
        } else { // PUMP_EMAUX
            if (run) {
                ok = emauxSetSpeed(pumpId, speed) && ok;
                ok = emauxRun(pumpId, true) && ok;
            } else {
                ok = emauxRun(pumpId, false) && ok;
            }
        }
#if RS485_DEBUG
        rs485DebugEnabled = dbgSave;
#endif
        if (ok) {
            ver.awaiting = true;
            ver.everSent = true;
            ver.lastSendMs = nowMs;
            ver.lastPollMs = nowMs; // first status poll one interval from now
            memo.failLogged = false;
            if (verboseSend)
                DBG_PRINTF("[Dispatcher] pump %u: command echoed OK — verifying by status readback\n", pumpId);
        } else if (!memo.failLogged) {
            memo.failLogged = true;
            DBG_PRINTF("[Dispatcher] pump %u: command FAILED — retrying silently until it confirms or the request changes\n",
                       pumpId);
        }
        return;
    }

    // ---- Phase 2: verify against the pump's own report -------------
    if (nowMs - ver.lastPollMs < VERIFY_POLL_MS) return;
    ver.lastPollMs = nowMs;
    bool verbosePoll = !ver.pollLogged;
#if RS485_DEBUG
    bool dbgSave = rs485DebugEnabled;
    if (!verbosePoll) rs485DebugEnabled = false;
#endif
    bool gotReport = false;
    bool match = false;
    uint16_t reported = 0;
    if (pumpType == PUMP_PENTAIR) {
        bool pRunning; uint16_t pRpm;
        if (pentairReadStatus(pumpId, pRunning, pRpm)) {
            gotReport = true;
            reported = pRpm;
            if (run) {
                int32_t diff = (int32_t)pRpm - (int32_t)target;
                if (diff < 0) diff = -diff;
                match = pRunning && diff <= VERIFY_RPM_TOL;
            } else {
                match = !pRunning;
            }
        }
    } else { // PUMP_EMAUX
        if (run) {
            int32_t cur = emauxReadInputRegister(pumpId, EMAUX_REG_CUR_SPEED);
            if (cur >= 0) {
                gotReport = true;
                reported = (uint16_t)cur;
                int32_t diff = cur - (int32_t)target;
                if (diff < 0) diff = -diff;
                match = diff <= VERIFY_RPM_TOL;
            }
        } else {
            int32_t st = emauxReadInputRegister(pumpId, EMAUX_REG_RUN_STATUS);
            if (st >= 0) {
                gotReport = true;
                reported = (uint16_t)st;
                match = (st == 0);
            }
        }
    }
#if RS485_DEBUG
    rs485DebugEnabled = dbgSave;
#endif
    ver.pollLogged = true;

    if (match) {
        currentPumpStates[idx].active = run;
        currentPumpStates[idx].speed = run ? target : 0;
        if (run) currentPumpStates[idx].type = pumpType;
        ver.awaiting = false;
        memo.failLogged = false;
        DBG_PRINTF("[Dispatcher] pump %u: status VERIFIED (%s, readback=%u) — state applied\n",
                   pumpId, run ? "running" : "stopped", reported);
        return;
    }

    if (nowMs - ver.lastSendMs >= VERIFY_RESEND_MS) {
        if (!ver.resendLogged) {
            ver.resendLogged = true;
            if (gotReport)
                DBG_PRINTF("[Dispatcher] pump %u: readback %u doesn't match target — re-sending (silent retries)\n",
                           pumpId, reported);
            else
                DBG_PRINTF("[Dispatcher] pump %u: no status reply — re-sending (silent retries)\n", pumpId);
        }
        ver.awaiting = false; // back to phase 1 next tick (silent)
    }
}

// Read-only accessor for the web UI / status API.
inline void pumpActualStateGet(uint8_t pumpId, bool &active, uint16_t &speed) {
    if (pumpId < 1 || pumpId > MAX_PUMPS) { active = false; speed = 0; return; }
    active = currentPumpStates[pumpId - 1].active;
    speed  = currentPumpStates[pumpId - 1].speed;
}
