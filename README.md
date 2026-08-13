# relay_timer

Swimming pool controller system featuring 3 relays and 4 RS-485 pumps (Pentair, Emaux, and Black & Decker), with pump scheduling and manual control. Wireless control from a mobile app over BLE and Wi-Fi, plus a built-in web dashboard and an on-device display UI.

The system has two halves: an **ESP32-S3 firmware** that owns all hardware state (relays, pumps, schedules, clock), and a **React Native (Expo) app** that talks to it over Bluetooth Low Energy or the local network — whichever is available — with automatic, seamless failover between the two.

## Features

- **3 relay channels** with manual override (Auto / On / Off) and named assignments (e.g. SPA LITE, POOL LITE).
- **4 variable-speed pumps over RS-485**, slot N = bus address N:
  - **Pentair IntelliFlo** (proprietary protocol, 9600 baud, addresses 0x60+)
  - **Emaux** (Modbus RTU, 9600 baud)
  - **Black & Decker** (Modbus RTU, **38400 baud**, supported on Pump 1 only — the pump is fixed at bus address 1). The driver retunes the UART per transaction so both bus speeds share one wire.
  - Pump ON status and RPM shown only once the pump **acknowledges** the command on the bus — never optimistically.
- **8 timer schedules**, each with its own target, start time, duration, and (for pumps) speed/protocol. Schedules persist across reboots and evaluate against NTP- or phone-synced time.
- **Four control surfaces**: the mobile app, the ESP32-hosted web dashboard, an on-device rotary/display menu, and serial commands for diagnostics.

## Connectivity & failover

- **BLE** is the pairing and provisioning channel: scan the QR code the device prints on its serial monitor, and the app connects, authenticates, and (if needed) provisions Wi-Fi credentials.
- **Wi-Fi (HTTP on the LAN)** is the preferred day-to-day transport once provisioned; the app promotes to it automatically and falls back to BLE within seconds when the network drops — and back again when it returns. Every outage path self-heals: router loss, phone Wi-Fi/Bluetooth toggles, device reboots, walking out of and back into range.
- **Multi-device**: several controllers can be paired and switched between; devices are identified by their Wi-Fi MAC, and the app verifies that identity before trusting a cached IP address.

## Security model

Connecting is not controlling. Every session — BLE or HTTP — must prove knowledge of a compiled-in shared secret via an HMAC-SHA256 challenge/response before a single command is accepted. Nonces are single-use (no replay), comparisons are constant-time, and Wi-Fi passwords are additionally encrypted with a session-derived keystream so they never cross the air in the clear. A generic BLE tool can see and connect to the device, but is hung up on before it can toggle anything. The BLE link also requires encryption/bonding (one-time iOS pairing prompt).

## Repository layout

| Path | What it is |
|---|---|
| `app/index.tsx` | The entire React Native app (Expo SDK 54): Home control UI, device pairing/switching, Wi-Fi setup, transport failover. |
| `relay_timer_latest/` | **The firmware that ships to the board.** Modular: `relay_timer_latest.ino` (core + display UI), `app_link.h` (BLE/app protocol), `web_server.h` + `web_ui.h` (LAN API + dashboard), `rs485_pumps.h` (pump drivers), `shared_state.h`, `wifi_setup.h`, `rtc_clock.h`. |
| `relay_timer_connection/` | Standalone reference sketch where the app↔device protocol was developed; kept in sync with `app_link.h`. |

## Building

**Firmware** — Arduino IDE, board "ESP32S3 Dev Module", partition scheme **Huge APP (3MB)**. Libraries: NimBLE-Arduino v2.x, ArduinoJson v6+. Flash `relay_timer_latest/relay_timer_latest.ino`. On boot the serial monitor (115200) prints the pairing QR payload and the device identity.

**App** — requires a dev build (BLE and camera don't run in Expo Go):

```bash
npm install
npx expo run:ios        # or run:android
```

Release APK: `npx expo prebuild --platform android && cd android && ./gradlew assembleRelease`, or `eas build -p android --profile preview`.

> The `DEVICE_SECRET` in `app/index.tsx` must match the one in the firmware byte for byte — change both before shipping.
