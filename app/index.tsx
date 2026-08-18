/*
 *
 * App.tsx
 * ---------------------------------------------------------------
 * Single-file React Native app with a custom bottom tab bar:
 *
 *   Home    -> visual replica of web_ui.h (INDEX_HTML) — relay
 *              cards, pump cards, timer schedules, add/edit modal.
 *              Now driven by PoolProvider: state comes from the
 *              ESP32-S3 over BLE when connected, and falls back to
 *              local mock state when it isn't (so the UI still
 *              works for demoing without hardware).
 *   Device  -> "Scan QR to Connect". Two-way sync with the ESP32-S3
 *              via react-native-ble-plx.
 *   Account -> local login/signup mock (no backend) + profile view.
 *
 * BLE protocol (must match PoolController.ino):
 *   Service UUID   6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *   RX (write)     6e400002-...  app -> esp32, commands
 *   TX (notify)    6e400003-...  esp32 -> app, full state + acks
 *   Messages are newline-delimited JSON, chunked to fit the MTU.
 *
 * ===============================================================
 *  SECURITY MODEL  (see the matching block in relay_timer_connection.ino)
 * ===============================================================
 * The controller refuses to act on ANY command until the connection
 * has proven it knows DEVICE_SECRET. The app does this silently, in
 * the ~200ms after the BLE link comes up — there is no prompt, no
 * passkey to type, and nothing for the user to do:
 *
 *   1. Subscribe to TX, then send {"cmd":"hello"}.
 *   2. ESP32 -> {"type":"challenge","nonce":"<32 hex chars>"}
 *   3. App   -> {"cmd":"auth","hmac":"<64 hex chars>"} where
 *      hmac = HMAC-SHA256(key = DEVICE_SECRET, msg = nonce hex).
 *   4. ESP32 -> {"type":"ack","cmd":"auth","ok":true}
 *
 * Only then does status flip to 'connected' and commands start
 * flowing. If the handshake fails or times out we disconnect and
 * surface an error.
 *
 * Why this and not a BLE passkey: a React Native app cannot supply a
 * pairing passkey to the OS. iOS hands pairing entirely to
 * CoreBluetooth with no injection point, and Android's setPasskey()
 * is a hidden framework API that react-native-ble-plx doesn't expose.
 * Doing the secret at the application layer gets the same outcome —
 * only this app can drive the hardware — with no system dialog.
 *
 * A generic BLE tool (nRF Connect, LightBlue) can still connect, but
 * it has no secret, fails step 3, and is hung up on by the firmware
 * before it can toggle a single relay.
 *
 * >>> DEVICE_SECRET must match the firmware byte for byte. <<<
 *
 * QR payload (encoded by the sticker on the device):
 *   {"v":2,"name":"PoolCtrl-XXXXXX","svc":"6e400001-...dcca9e"}
 *   The QR carries no secret — it only says which peripheral to look
 *   for. We match by advertised local name + service UUID rather than
 *   MAC address, because iOS/CoreBluetooth never exposes real MAC
 *   addresses over BLE — a MAC-based QR would silently fail to
 *   connect on iPhone.
 *
 * New dependencies (on top of what the file already used):
 *   npx expo install expo-camera
 *   (SHA-256/HMAC are implemented in-file so no crypto dependency
 *   and no extra native rebuild is required.)
 * ---------------------------------------------------------------
 */

import AsyncStorage from '@react-native-async-storage/async-storage';
import { BarcodeScanningResult, CameraView, useCameraPermissions } from 'expo-camera';
import React, {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import {
  ActivityIndicator,
  Alert,
  Animated,
  Easing,
  Modal,
  PermissionsAndroid,
  Platform,
  ScrollView,
  StatusBar,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';
import { BleManager, State as BleState, Device } from 'react-native-ble-plx';
import { SafeAreaView } from 'react-native-safe-area-context';
import Svg, { Circle as SvgCircle, Path as SvgPath, Rect as SvgRect } from 'react-native-svg';

// ============================================================
//  THEME  (ported from web_ui.h :root CSS variables)
// ============================================================
const COLORS = {
  r1: '#0891b2', r1Soft: 'rgba(8,145,178,0.12)',
  r2: '#7c3aed', r2Soft: 'rgba(124,58,237,0.12)',
  r3: '#059669', r3Soft: 'rgba(5,150,105,0.12)',
  on: '#059669', onSoft: 'rgba(5,150,105,0.14)',
  now: '#e11d48',
  bg: '#f6f7fb',
  glassBg: 'rgba(255,255,255,0.88)',
  glassBorder: 'rgba(15,23,42,0.09)',
  text: 'rgba(15,23,42,0.94)',
  muted: 'rgba(15,23,42,0.58)',
  label: 'rgba(15,23,42,0.42)',
  chipBg: 'rgba(15,23,42,0.05)',
  segBg: '#e5e7eb',
  segBorder: '#000000',
  danger: '#dc2626',
};

const NAME_OPTIONS = [
  'MAIN PUMP', 'PSWP PUMP', 'SPA LITE', 'POOL LITE',
  'FTN PUMP1', 'FTN PUMP2', 'OTHER1', 'OTHER2', 'CLNR PUMP',
];

// ============================================================
//  BLE PROTOCOL CONSTANTS  — must match PoolController.ino
// ============================================================
const SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const CHAR_RX_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // app -> esp32
const CHAR_TX_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // esp32 -> app

// Shared secret — MUST match DEVICE_SECRET in relay_timer_connection.ino.
// Used verbatim (as UTF-8 bytes) as the HMAC-SHA256 key. Regenerate with:
//   openssl rand -hex 32
// Changing it means reflashing the firmware and reshipping the app together.
const DEVICE_SECRET =
  '3f9a1c7e5b28d04a6ef391c8b7d25a06e4c8193bd7f6025a8c1e93b47dfa6210';

// How long the whole silent handshake (connect -> hello -> challenge ->
// auth ack) may take before we give up. The firmware's own deadline is
// 10s; staying under it means we report the failure rather than just
// getting hung up on.
const AUTH_TIMEOUT_MS = 8000;

// ============================================================
//  TYPES
// ============================================================
type RelayMode = 'auto' | 'on' | 'off';
// 'Black & Decker' talks Modbus at 38400 baud and is fixed at bus address
// 1, so the firmware accepts it on Pump 1 only — the pickers below offer
// it only there. The strings travel over the protocol verbatim.
type PumpType = 'Pentair' | 'Emaux' | 'Black & Decker';

interface RelayState {
  id: 1 | 2 | 3;
  tag: string;
  accent: string;
  accentSoft: string;
  name: string;
  mode: RelayMode;
  active: boolean;
  countdown: string;
}

interface PumpState {
  id: 1 | 2 | 3 | 4;
  name: string;
  mode: RelayMode;
  active: boolean;
  speedRpm: number;    // CONFIRMED speed — 0 until the pump acknowledges over RS-485
  setSpeedRpm: number; // configured manual set-point — what the speed input shows
  pumpType: PumpType;
  countdown: string;
}

interface Preset {
  id: string;
  targetLabel: string;
  timeStr: string;
  durationStr: string;
  enabled: boolean;
  // raw fields kept alongside the display strings above so we can
  // round-trip cleanly with the ESP32, which stores hour24/minute/durationMin
  hour24: number;
  minute: number;
  durationMin: number;
  // Per-schedule pump parameters (only meaningful for Pump targets) —
  // each preset carries its own speed/type, exactly like the firmware's
  // preset model and the old web UI's preset editor.
  speedRpm: number;
  pumpType: PumpType;
}

// One shared shape for creating/updating a schedule.
interface ScheduleInput {
  target: string;
  hour24: number;
  minute: number;
  durationMin: number;
  enabled: boolean;
  speedRpm: number;
  pumpType: PumpType;
}

interface QrPayload {
  v: number;
  name: string;
  svc: string;
}

// ============================================================
//  MOCK / FALLBACK INITIAL DATA  (used until a device connects)
// ============================================================
const RELAY_META: { tag: string; accent: string; accentSoft: string }[] = [
  { tag: 'CHANNEL 1', accent: COLORS.r1, accentSoft: COLORS.r1Soft },
  { tag: 'CHANNEL 2', accent: COLORS.r2, accentSoft: COLORS.r2Soft },
  { tag: 'CHANNEL 3', accent: COLORS.r3, accentSoft: COLORS.r3Soft },
];

const initialRelays: RelayState[] = [1, 2, 3].map((id) => ({
  id: id as 1 | 2 | 3,
  tag: RELAY_META[id - 1].tag,
  accent: RELAY_META[id - 1].accent,
  accentSoft: RELAY_META[id - 1].accentSoft,
  name: `Relay ${id}`,
  mode: 'auto',
  active: false,
  countdown: id === 2 ? 'Depends on Relay 1' : id === 3 ? 'Independent' : 'No active schedule',
}));

const initialPumps: PumpState[] = [1, 2, 3, 4].map((i) => ({
  id: i as 1 | 2 | 3 | 4,
  name: `Pump ${i}`,
  mode: 'auto',
  active: false,
  speedRpm: 0,
  setSpeedRpm: 0,
  pumpType: 'Pentair',
  countdown: 'No active schedule',
}));

const initialPresets: Preset[] = [
  { id: 'p1', targetLabel: 'Relay 1', timeStr: '6:30 AM', durationStr: '1h 00m', enabled: true, hour24: 6, minute: 30, durationMin: 60, speedRpm: 2000, pumpType: 'Pentair' },
  { id: 'p2', targetLabel: 'Pump 1', timeStr: '8:00 AM', durationStr: '2h 30m', enabled: false, hour24: 8, minute: 0, durationMin: 150, speedRpm: 2000, pumpType: 'Pentair' },
];

const TARGET_OPTIONS = ['Relay 1', 'Relay 2', 'Relay 3', 'Pump 1', 'Pump 2', 'Pump 3', 'Pump 4'];

// ============================================================
//  FIRMWARE OTA  — the app checks this manifest on GitHub, compares its
//  version against what the DEVICE reports, downloads the .bin, and
//  pushes it to the controller's /update endpoint over the LAN.
//  version.json shape: { "version": "1.1.0", "url": "https://github.com/<user>/<repo>/releases/download/v1.1.0/firmware.bin", "notes": "..." }
// ============================================================
const FW_MANIFEST_URL = 'https://raw.githubusercontent.com/siamgazi/relay-timer-firmware/main/version.json';

// Only ONE firmware-update dialog may be presented at a time — a second
// native Modal opened over an existing one is silently dropped by the OS
// and can wedge an invisible, touch-eating layer over the screen.
const fwUpdateUiOpen = { current: false };
// Set after a successful update so the auto-prompt doesn't re-suggest the
// same version while the app still holds that device's pre-reboot state.
// Scoped to the device MAC: switching to ANOTHER device on an older
// firmware must still trigger the suggestion.
const fwJustUpdatedTo: { current: { mac: string; version: string } | null } = { current: null };

// true when a is strictly newer than b ("1.2.0" vs "1.1.9")
function versionNewer(a: string, b: string): boolean {
  const pa = a.split('.').map((n) => parseInt(n, 10) || 0);
  const pb = b.split('.').map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < 3; i++) {
    if ((pa[i] || 0) !== (pb[i] || 0)) return (pa[i] || 0) > (pb[i] || 0);
  }
  return false;
}

// ============================================================
//  base64 <-> utf8 helpers
//  (react-native-ble-plx reads/writes characteristic values as
//  base64 strings; RN/Hermes has no built-in Buffer or btoa/atob)
// ============================================================
const B64_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function utf8Bytes(str: string): number[] {
  const bytes: number[] = [];
  for (let i = 0; i < str.length; i++) {
    let code = str.charCodeAt(i);
    // Fold surrogate pairs into the real code point — encoding the halves
    // separately yields CESU-8 the firmware's UTF-8 can't match (an emoji
    // in an SSID/password would fail the join with no useful error).
    if (code >= 0xd800 && code <= 0xdbff && i + 1 < str.length) {
      const lo = str.charCodeAt(i + 1);
      if (lo >= 0xdc00 && lo <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + (lo - 0xdc00);
        i++;
      }
    }
    if (code < 0x80) {
      bytes.push(code);
    } else if (code < 0x800) {
      bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
      bytes.push(0xe0 | (code >> 12), 0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    } else {
      bytes.push(
        0xf0 | (code >> 18),
        0x80 | ((code >> 12) & 0x3f),
        0x80 | ((code >> 6) & 0x3f),
        0x80 | (code & 0x3f)
      );
    }
  }
  return bytes;
}

function utf8ToBase64(str: string): string {
  const bytes = utf8Bytes(str);
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i], b1 = bytes[i + 1], b2 = bytes[i + 2];
    out += B64_CHARS[b0 >> 2];
    out += B64_CHARS[((b0 & 3) << 4) | (b1 === undefined ? 0 : b1 >> 4)];
    out += b1 === undefined ? '=' : B64_CHARS[((b1 & 15) << 2) | (b2 === undefined ? 0 : b2 >> 6)];
    out += b2 === undefined ? '=' : B64_CHARS[b2 & 63];
  }
  return out;
}

function base64ToUtf8(b64: string): string {
  const clean = b64.replace(/[^A-Za-z0-9+/]/g, '');
  const bytes: number[] = [];
  for (let i = 0; i < clean.length; i += 4) {
    const e1 = B64_CHARS.indexOf(clean[i]);
    const e2 = B64_CHARS.indexOf(clean[i + 1]);
    const e3 = B64_CHARS.indexOf(clean[i + 2]);
    const e4 = B64_CHARS.indexOf(clean[i + 3]);
    const b0 = (e1 << 2) | (e2 >> 4);
    bytes.push(b0);
    if (e3 !== -1 && clean[i + 2] !== undefined) {
      const b1 = ((e2 & 15) << 4) | (e3 >> 2);
      bytes.push(b1);
    }
    if (e4 !== -1 && clean[i + 3] !== undefined) {
      const b2 = ((e3 & 3) << 6) | e4;
      bytes.push(b2);
    }
  }
  let out = '';
  let i = 0;
  while (i < bytes.length) {
    const b0 = bytes[i++];
    if (b0 < 0x80) {
      out += String.fromCharCode(b0);
    } else if (b0 >> 5 === 0x6) {
      const b1 = bytes[i++];
      out += String.fromCharCode(((b0 & 0x1f) << 6) | (b1 & 0x3f));
    } else if (b0 >> 4 === 0xe) {
      const b1 = bytes[i++];
      const b2 = bytes[i++];
      out += String.fromCharCode(((b0 & 0xf) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f));
    } else {
      // 4-byte lead (non-BMP) — was misread as a 3-byte sequence before.
      const b1 = bytes[i++];
      const b2 = bytes[i++];
      const b3 = bytes[i++];
      out += String.fromCodePoint(
        ((b0 & 0x7) << 18) | ((b1 & 0x3f) << 12) | ((b2 & 0x3f) << 6) | (b3 & 0x3f)
      );
    }
  }
  return out;
}

// ============================================================
//  SHA-256 / HMAC-SHA256
//  Implemented in-file on purpose: Hermes ships no WebCrypto and no
//  Node crypto, and expo-crypto exposes digests but no HMAC. Rolling
//  it here keeps the pairing handshake dependency-free and avoids
//  forcing another native rebuild.
// ============================================================
const K256 = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

function rotr32(x: number, n: number): number {
  return ((x >>> n) | (x << (32 - n))) >>> 0;
}

function sha256Bytes(msg: number[]): number[] {
  const h = new Uint32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  ]);

  // Pad to a multiple of 64 bytes: 0x80, then zeros, then the message
  // length in bits as a big-endian 64-bit integer.
  const bitLen = msg.length * 8;
  const hi = Math.floor(bitLen / 0x100000000);
  const lo = bitLen >>> 0;
  const p = msg.slice();
  p.push(0x80);
  while (p.length % 64 !== 56) p.push(0);
  p.push((hi >>> 24) & 0xff, (hi >>> 16) & 0xff, (hi >>> 8) & 0xff, hi & 0xff);
  p.push((lo >>> 24) & 0xff, (lo >>> 16) & 0xff, (lo >>> 8) & 0xff, lo & 0xff);

  const w = new Uint32Array(64);
  for (let off = 0; off < p.length; off += 64) {
    for (let i = 0; i < 16; i++) {
      const j = off + i * 4;
      w[i] = ((p[j] << 24) | (p[j + 1] << 16) | (p[j + 2] << 8) | p[j + 3]) >>> 0;
    }
    for (let i = 16; i < 64; i++) {
      const s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >>> 3);
      const s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
    }

    let a = h[0], b = h[1], c = h[2], d = h[3];
    let e = h[4], f = h[5], g = h[6], hh = h[7];

    for (let i = 0; i < 64; i++) {
      const S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      const ch = (e & f) ^ (~e & g);
      const t1 = (hh + S1 + ch + K256[i] + w[i]) >>> 0;
      const S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;

      hh = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }

    h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0;
    h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0;
    h[4] = (h[4] + e) >>> 0; h[5] = (h[5] + f) >>> 0;
    h[6] = (h[6] + g) >>> 0; h[7] = (h[7] + hh) >>> 0;
  }

  const out: number[] = [];
  for (let i = 0; i < 8; i++) {
    out.push((h[i] >>> 24) & 0xff, (h[i] >>> 16) & 0xff, (h[i] >>> 8) & 0xff, h[i] & 0xff);
  }
  return out;
}

function bytesToHex(bytes: number[]): string {
  let out = '';
  for (let i = 0; i < bytes.length; i++) {
    out += (bytes[i] >>> 4).toString(16) + (bytes[i] & 0x0f).toString(16);
  }
  return out;
}

// HMAC-SHA256(key, msg) as 64 lowercase hex chars. Both key and msg are
// treated as UTF-8, matching hmacSha256Hex() in the firmware.
function hmacSha256Hex(key: string, msg: string): string {
  const BLOCK = 64;
  let k = utf8Bytes(key);
  if (k.length > BLOCK) k = sha256Bytes(k);
  while (k.length < BLOCK) k.push(0);

  const ipad = k.map((b) => b ^ 0x36);
  const opad = k.map((b) => b ^ 0x5c);

  const inner = sha256Bytes(ipad.concat(utf8Bytes(msg)));
  return bytesToHex(sha256Bytes(opad.concat(inner)));
}

// ============================================================
//  DEVICE REGISTRY  (persisted)
// ============================================================
// A device's identity is its Wi-Fi station MAC, reported by the firmware
// in every state payload. The user-facing name is just a label stored
// against that MAC, so renaming a device never makes the app treat it as
// a new one — and two controllers with the same name stay distinct.
interface DeviceRecord {
  mac: string;            // identity — never changes
  name: string;           // user-assigned label
  bleName: string;        // advertised name, used to re-find it over BLE
  svc: string;            // service UUID from the QR payload
  lastIp: string | null;  // cached for the LAN transport; refreshed over BLE
  ssid: string | null;    // network it was last provisioned onto
  addedAt: number;
}

const DEVICES_KEY = 'relaytimer.devices.v1';
const ACTIVE_KEY = 'relaytimer.activeMac.v1';

async function loadDevices(): Promise<DeviceRecord[]> {
  try {
    const raw = await AsyncStorage.getItem(DEVICES_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return []; // corrupt store — start clean rather than wedging the app
  }
}

async function saveDevices(devices: DeviceRecord[]): Promise<void> {
  try {
    await AsyncStorage.setItem(DEVICES_KEY, JSON.stringify(devices));
  } catch (err) {
    console.warn('Could not persist device list', err);
  }
}

async function loadActiveMac(): Promise<string | null> {
  try {
    return await AsyncStorage.getItem(ACTIVE_KEY);
  } catch {
    return null;
  }
}

async function saveActiveMac(mac: string | null): Promise<void> {
  try {
    if (mac) await AsyncStorage.setItem(ACTIVE_KEY, mac);
    else await AsyncStorage.removeItem(ACTIVE_KEY);
  } catch {}
}

// ============================================================
//  WI-FI TYPES
// ============================================================
type WifiPhase = 'unconfigured' | 'connecting' | 'online' | 'offline';

interface WifiStatus {
  state: WifiPhase;
  ssid: string;
  ip?: string;
  rssi?: number;
  // Firmware-reported: the last full join attempt ran out its 20s clock
  // (bad password / AP unreachable). 'offline' WITHOUT this flag means a
  // transient drop the device is already retrying — not a failure verdict.
  joinFailed?: boolean;
}

interface WifiNetwork {
  ssid: string;
  rssi: number;
  secure: boolean;
}

// ============================================================
//  HTTP TRANSPORT  (the LAN path — preferred when reachable)
// ============================================================
// Mirrors the BLE handshake exactly: fetch a nonce, prove the shared
// secret with an HMAC, receive a bearer token for subsequent calls.
// Same secret, same crypto, so there is one security model to reason
// about rather than two.
// A healthy LAN round-trip is well under 300ms, so 2.5s already means
// "gone" — and this timeout bounds how long failure detection can take.
const HTTP_TIMEOUT_MS = 2500;
const HTTP_POLL_MS = 2000;        // how often the LAN transport refreshes state
const HTTP_FAIL_THRESHOLD = 2;    // consecutive poll failures before falling back to BLE
// How often, while controlling over BLE, to test whether the device is
// reachable on the LAN again. This poll is the ONLY promotion path when
// the outage was on the phone's side (its Wi-Fi toggled, roamed APs) —
// the device was online the whole time, so no "came online" event will
// ever arrive to react to. A failed probe is sub-second and silent, so
// polling tightly is cheap and makes the BLE→Wi-Fi switch feel instant.
const WIFI_PROMOTE_RETRY_MS = 3000;

async function httpFetch(ip: string, path: string, init?: RequestInit): Promise<Response> {
  // React Native's fetch has no timeout, and a device that dropped off
  // the network would otherwise hang the transport for ~60s before the
  // OS gave up — far too slow to fail over to BLE.
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), HTTP_TIMEOUT_MS);
  try {
    return await fetch(`http://${ip}${path}`, { ...init, signal: controller.signal });
  } finally {
    clearTimeout(timer);
  }
}

// Performs the challenge/auth exchange and returns a bearer token.
// The nonce comes back too: Wi-Fi passwords sent over this transport are
// encrypted against it, exactly like the BLE session nonce.
// Throws if the device is unreachable or rejects our secret.
async function httpAuthenticate(ip: string): Promise<{ token: string; mac: string; nonce: string }> {
  const chalRes = await httpFetch(ip, '/challenge');
  if (!chalRes.ok) throw new Error(`challenge failed (${chalRes.status})`);
  const chal = await chalRes.json();
  if (typeof chal?.nonce !== 'string') throw new Error('malformed challenge');

  const hmac = hmacSha256Hex(DEVICE_SECRET, chal.nonce);
  const authRes = await httpFetch(ip, '/auth', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ hmac }),
  });
  if (!authRes.ok) throw new Error(`auth rejected (${authRes.status})`);
  const auth = await authRes.json();
  if (!auth?.token) throw new Error('no token issued');

  return { token: auth.token, mac: chal.mac || '', nonce: chal.nonce };
}

async function httpGetState(ip: string, token: string): Promise<any> {
  const res = await httpFetch(ip, '/state', { headers: { Authorization: `Bearer ${token}` } });
  if (res.status === 401) throw new Error('token expired');
  if (!res.ok) throw new Error(`state failed (${res.status})`);
  return res.json();
}

async function httpSendCommand(ip: string, token: string, cmd: object): Promise<any> {
  const res = await httpFetch(ip, '/cmd', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', Authorization: `Bearer ${token}` },
    body: JSON.stringify(cmd),
  });
  if (res.status === 401) throw new Error('token expired');
  if (!res.ok) throw new Error(`command failed (${res.status})`);
  return res.json();
}

// ============================================================
//  CREDENTIAL ENCRYPTION  (matches keystreamDecryptHex in the firmware)
// ============================================================
// XORs the Wi-Fi password with a keystream both sides derive from the
// shared secret and the session nonce, so the password is never exposed
// to a BLE sniffer even if link-layer encryption is turned off.
function keystreamEncryptHex(nonce: string, plain: string): string {
  const bytes = utf8Bytes(plain);
  const out: number[] = [];
  let block: number[] = [];
  for (let i = 0; i < bytes.length; i++) {
    if (i % 32 === 0) block = sha256HmacBytes(DEVICE_SECRET, `${nonce}:${Math.floor(i / 32)}`);
    out.push(bytes[i] ^ block[i % 32]);
  }
  return bytesToHex(out);
}

// Raw-byte HMAC, used by the keystream above.
function sha256HmacBytes(key: string, msg: string): number[] {
  const BLOCK = 64;
  let k = utf8Bytes(key);
  if (k.length > BLOCK) k = sha256Bytes(k);
  while (k.length < BLOCK) k.push(0);
  const inner = sha256Bytes(k.map((b) => b ^ 0x36).concat(utf8Bytes(msg)));
  return sha256Bytes(k.map((b) => b ^ 0x5c).concat(inner));
}

// ============================================================
//  FORMAT HELPERS  (hour24/minute/durationMin <-> display strings)
// ============================================================
function to12Hour(hour24: number): { hour: number; ampm: 'AM' | 'PM' } {
  const ampm = hour24 >= 12 ? 'PM' : 'AM';
  const hour = hour24 % 12 || 12;
  return { hour, ampm };
}
function to24Hour(hour12: number, ampm: 'AM' | 'PM'): number {
  let h = hour12 % 12;
  if (ampm === 'PM') h += 12;
  return h;
}
function formatTimeStr(hour24: number, minute: number): string {
  const { hour, ampm } = to12Hour(hour24);
  return `${hour}:${minute.toString().padStart(2, '0')} ${ampm}`;
}
function formatDurationStr(durationMin: number): string {
  const h = Math.floor(durationMin / 60);
  const m = durationMin % 60;
  return `${h}h ${m.toString().padStart(2, '0')}m`;
}

// ============================================================
//  POOL CONTEXT  — BLE connection + shared relay/pump/preset state
// ============================================================
type ConnectionStatus = 'disconnected' | 'connecting' | 'authenticating' | 'connected' | 'error';

type TransportKind = 'ble' | 'wifi';

interface PoolContextValue {
  relays: RelayState[];
  pumps: PumpState[];
  presets: Preset[];
  status: ConnectionStatus;
  deviceLabel: string | null;
  lastError: string | null;

  // ---- multi-device ----
  devices: DeviceRecord[];
  activeMac: string | null;
  activeDevice: DeviceRecord | null;
  transport: TransportKind | null;
  bleUp: boolean;
  deviceFw: string | null;
  otaPerform: (binUrl: string, targetVersion: string, onPhase: (label: string, pct: number) => void) => Promise<void>;
  selectDevice: (mac: string) => Promise<void>;
  renameDevice: (mac: string, name: string) => Promise<void>;
  removeDevice: (mac: string) => Promise<void>;

  // ---- wi-fi ----
  wifi: WifiStatus | null;
  wifiNetworks: WifiNetwork[];
  wifiScanning: boolean;
  scanWifiNetworks: () => Promise<void>;
  provisionWifi: (ssid: string, password: string) => Promise<void>;
  forgetWifi: () => Promise<void>;
  refreshState: () => Promise<void>;

  connectWithQrData: (raw: string) => Promise<void>;
  disconnect: () => Promise<void>;
  forceReset: () => Promise<void>;
  setRelayMode: (id: number, mode: RelayMode) => void;
  setRelayName: (id: number, name: string) => void;
  setPumpMode: (id: number, mode: RelayMode) => void;
  setPumpConfig: (id: number, patch: Partial<Pick<PumpState, 'pumpType' | 'speedRpm' | 'name'>>) => void;
  addSchedule: (input: ScheduleInput) => void;
  updateSchedule: (id: string, input: ScheduleInput) => void;
  deleteSchedule: (id: string) => void;
  toggleSchedule: (id: string, enabled: boolean) => void;

  // Relay 2's dependency on Relay 1 — the switch the web UI had.
  r2Depends: boolean;
  setRelay2Dependency: (depends: boolean) => void;
}

const PoolContext = createContext<PoolContextValue | null>(null);
function usePool(): PoolContextValue {
  const ctx = useContext(PoolContext);
  if (!ctx) throw new Error('usePool must be used within PoolProvider');
  return ctx;
}

let _bleManager: BleManager | null = null;
let _bleUnavailableReason: string | null = null;
function getBleManager(): BleManager | null {
  if (_bleManager) return _bleManager;
  if (_bleUnavailableReason) return null;
  try {
    _bleManager = new BleManager();
    return _bleManager;
  } catch (err) {
    _bleUnavailableReason =
      'Bluetooth native module not available. Run this app from a custom ' +
      'dev build (npx expo run:ios / run:android), not Expo Go.';
    console.warn(_bleUnavailableReason, err);
    return null;
  }
}

async function requestBlePermissions(): Promise<boolean> {
  if (Platform.OS === 'android') {
    const apiLevel = parseInt(Platform.Version.toString(), 10);
    if (apiLevel >= 31) {
      const granted = await PermissionsAndroid.requestMultiple([
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
        PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
        PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
      ]);
      return (
        granted[PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN] === PermissionsAndroid.RESULTS.GRANTED &&
        granted[PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT] === PermissionsAndroid.RESULTS.GRANTED
      );
    } else {
      const granted = await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION);
      return granted === PermissionsAndroid.RESULTS.GRANTED;
    }
  }
  return true; // iOS handled via Info.plist + native prompt
}

function waitForPoweredOn(manager: BleManager): Promise<boolean> {
  return new Promise((resolve) => {
    manager.state().then((state) => {
      if (state === BleState.PoweredOn) { resolve(true); return; }
      const sub = manager.onStateChange((newState) => {
        if (newState === BleState.PoweredOn) { sub.remove(); resolve(true); }
        else if (newState === BleState.PoweredOff || newState === BleState.Unauthorized || newState === BleState.Unsupported) {
          sub.remove(); resolve(false);
        }
      }, true);
      setTimeout(() => { sub.remove(); resolve(false); }, 5000);
      // A state() failure must resolve too — a pending promise here would
      // wedge the connect flow with connectInFlightRef stuck true.
    }).catch(() => resolve(false));
  });
}

function PoolProvider({ children }: { children: React.ReactNode }) {
  const [relays, setRelays] = useState<RelayState[]>(initialRelays);
  const [pumps, setPumps] = useState<PumpState[]>(initialPumps);
  const [presets, setPresets] = useState<Preset[]>(initialPresets);
  const [r2Depends, setR2Depends] = useState(true); // matches the firmware default
  const [status, setStatus] = useState<ConnectionStatus>('disconnected');
  const [deviceLabel, setDeviceLabel] = useState<string | null>(null);
  const [lastError, setLastError] = useState<string | null>(null);

  // ---- multi-device registry ----
  const [devices, setDevices] = useState<DeviceRecord[]>([]);
  const [activeMac, setActiveMac] = useState<string | null>(null);
  const [transport, setTransport] = useState<TransportKind | null>(null);
  // True while a BLE link to the device is up, even when Wi-Fi is the
  // active transport — drives the Bluetooth indicator on the Device tab.
  const [bleUp, setBleUp] = useState(false);
  // Firmware version the connected device reports in its state JSON.
  const [deviceFw, setDeviceFw] = useState<string | null>(null);

  // ---- wi-fi ----
  const [wifi, setWifi] = useState<WifiStatus | null>(null);
  const [wifiNetworks, setWifiNetworks] = useState<WifiNetwork[]>([]);
  const [wifiScanning, setWifiScanning] = useState(false);

  // The LAN transport's bearer token and the IP it belongs to. Held in a
  // ref because the polling loop and every command read it without
  // wanting to re-subscribe on each change.
  const httpRef = useRef<{ ip: string; token: string; nonce: string } | null>(null);
  const transportRef = useRef<TransportKind | null>(null);
  const activeMacRef = useRef<string | null>(null);
  const devicesRef = useRef<DeviceRecord[]>([]);

  // The nonce from the current BLE handshake. The firmware derives the
  // Wi-Fi credential keystream from it, so it must survive past auth.
  const sessionNonceRef = useRef<string | null>(null);

  // Mirror of `status` readable from callbacks without adding a dependency.
  const statusRef = useRef<ConnectionStatus>('disconnected');
  useEffect(() => { statusRef.current = status; }, [status]);

  const setTransportBoth = useCallback((t: TransportKind | null) => {
    transportRef.current = t;
    setTransport(t);
  }, []);

  const deviceRef = useRef<Device | null>(null);
  const rxBufferRef = useRef<string>('');
  const isConnectedRef = useRef(false);
  const lastRxAtRef = useRef(0);

  // Set only once the HMAC challenge has been answered and accepted.
  // sendCommand() refuses to transmit until this is true, so a half-open
  // link can never leak a relay command the firmware would reject anyway.
  const isAuthedRef = useRef(false);

  // True from the moment a connect attempt starts until it reaches a terminal
  // state. Serialises connectWithQrData against itself — see the comment there.
  const connectInFlightRef = useRef(false);

  // Connection generation. Bumped by every disconnect() and every new connect
  // attempt. Async callbacks capture the generation they belong to and bail
  // if it has moved on — this is what stops a superseded scan callback, scan
  // timeout, or a LATE onDisconnected event from a torn-down connection from
  // clobbering the session that replaced it. Without it, "scan QR while
  // already connected" raced disconnect against reconnect and silently killed
  // the pairing chain before the Wi-Fi step.
  const connectGenRef = useRef(0);

  // In-flight handshake. Held in a ref (not state) so handleIncomingLine
  // can drive it without being re-created on every render.
  const authRef = useRef<{
    device: Device;
    settle: (ok: boolean, message?: string) => void;
  } | null>(null);

  // Insert or update a device record, keyed on MAC. Returns the merged
  // record. Renames are preserved: an existing user-assigned name always
  // wins over the device's advertised default.
  const upsertDevice = useCallback(async (patch: Partial<DeviceRecord> & { mac: string }) => {
    const current = devicesRef.current;
    const existing = current.find((d) => d.mac === patch.mac);
    const merged: DeviceRecord = {
      mac: patch.mac,
      name: existing?.name ?? patch.name ?? patch.bleName ?? patch.mac,
      bleName: patch.bleName ?? existing?.bleName ?? '',
      svc: patch.svc ?? existing?.svc ?? SERVICE_UUID,
      lastIp: patch.lastIp !== undefined ? patch.lastIp : existing?.lastIp ?? null,
      ssid: patch.ssid !== undefined ? patch.ssid : existing?.ssid ?? null,
      addedAt: existing?.addedAt ?? Date.now(),
    };
    const next = existing
      ? current.map((d) => (d.mac === patch.mac ? merged : d))
      : [...current, merged];
    // Ground truth for "device not appearing in My Devices" reports: every
    // identity the app ever files, and the registry size after each.
    console.log(
      `[registry] ${existing ? 'update' : 'ADD'} ${patch.mac} (${merged.bleName || merged.name}) — ${next.length} device(s)`
    );
    devicesRef.current = next;
    setDevices(next);
    await saveDevices(next);
    return merged;
  }, []);

  // ---- apply a full state push from the ESP32 into React state ----
  const applyRemoteState = useCallback((msg: any) => {
    // Identity + transport info. Caching the IP here on every state push
    // is what makes a DHCP-reassigned address self-heal: the next BLE
    // connection silently refreshes it.
    if (msg.wifi) {
      setWifi({
        state: msg.wifi.state as WifiPhase,
        ssid: msg.wifi.ssid || '',
        ip: msg.wifi.ip,
        rssi: msg.wifi.rssi,
        joinFailed: msg.wifi.joinFailed === true,
      });
    }
    if (typeof msg.fwVersion === 'string' && msg.fwVersion.length > 0) setDeviceFw(msg.fwVersion);
    if (typeof msg.mac === 'string' && msg.mac.length > 0) {
      activeMacRef.current = msg.mac;
      setActiveMac(msg.mac);
      upsertDevice({
        mac: msg.mac,
        bleName: msg.name,
        lastIp: msg.wifi?.ip ?? undefined,
        ssid: msg.wifi?.ssid || null,
      });
    }

    // Array checks (like wifiScan below): a torn-but-parseable frame with a
    // non-array field would otherwise throw inside the notify callback.
    if (Array.isArray(msg.relays)) {
      setRelays((prev) =>
        prev.map((r) => {
          const found = msg.relays.find((x: any) => x.id === r.id);
          if (!found) return r;
          return { ...r, name: found.name, mode: found.mode, active: found.active, countdown: found.countdown };
        })
      );
    }
    if (Array.isArray(msg.pumps)) {
      setPumps((prev) =>
        prev.map((p) => {
          const found = msg.pumps.find((x: any) => x.id === p.id);
          if (!found) return p;
          return {
            ...p,
            name: found.name,
            mode: found.mode,
            active: found.active,
            speedRpm: found.speedRpm,
            // Older firmware doesn't send the set-point — keep the local one.
            setSpeedRpm: typeof found.setSpeedRpm === 'number' ? found.setSpeedRpm : p.setSpeedRpm,
            pumpType: found.pumpType,
            countdown: found.countdown,
          };
        })
      );
    }
    if (Array.isArray(msg.schedules)) {
      setPresets(
        msg.schedules.map((s: any) => ({
          id: s.id,
          targetLabel: s.target,
          timeStr: formatTimeStr(s.hour24, s.minute),
          durationStr: formatDurationStr(s.durationMin),
          enabled: s.enabled,
          hour24: s.hour24,
          minute: s.minute,
          durationMin: s.durationMin,
          speedRpm: typeof s.speedRpm === 'number' ? s.speedRpm : 2000,
          pumpType: (s.pumpType === 'Emaux' || s.pumpType === 'Black & Decker' ? s.pumpType : 'Pentair') as PumpType,
        }))
      );
    }
    if (typeof msg.r2Depends === 'boolean') setR2Depends(msg.r2Depends);
  }, [upsertDevice]);

  // Raw write — used by the handshake, which must be able to transmit
  // *before* isAuthedRef is set. Everything else goes through sendCommand.
  const writeLine = useCallback(async (device: Device, obj: object) => {
    const b64 = utf8ToBase64(JSON.stringify(obj) + '\n');
    await device.writeCharacteristicWithResponseForService(SERVICE_UUID, CHAR_RX_UUID, b64);
  }, []);

  const handleIncomingLine = useCallback((line: string) => {
    if (!line) return;
    let msg: any;
    try {
      msg = JSON.parse(line);
    } catch {
      if (__DEV__) console.log(`[rx] UNPARSEABLE line (${line.length} chars)`);
      return; // partial/corrupt chunk — ignore, next full line will resync
    }
    // What the device actually sends us, message by message. Splits the
    // world cleanly: state arriving without wifi = firmware; nothing
    // arriving = transport; wifi arriving but no modal = app logic.
    if (__DEV__) console.log(`[rx] type=${msg.type ?? '?'}${msg.type === 'state' ? ` wifi=${msg.wifi?.state ?? 'MISSING'}` : ''}${msg.type === 'ack' ? ` cmd=${msg.cmd} ok=${msg.ok}` : ''}`);

    // ---- handshake step 2 -> 3: answer the challenge -----------------
    // The nonce is fresh per connection, so this HMAC is single-use and
    // a captured frame is worthless against a later session.
    if (msg.type === 'challenge') {
      const auth = authRef.current;
      if (!auth) return;
      if (typeof msg.nonce !== 'string' || msg.nonce.length === 0) {
        auth.settle(false, 'Device sent a malformed pairing challenge.');
        return;
      }
      // Remember it: the firmware derives the Wi-Fi credential keystream
      // from this same nonce, so provisioning later in the session needs it.
      sessionNonceRef.current = msg.nonce;
      const hmac = hmacSha256Hex(DEVICE_SECRET, msg.nonce);
      writeLine(auth.device, { cmd: 'auth', hmac }).catch((err) =>
        auth.settle(false, `Could not answer the pairing challenge: ${err?.message || err}`)
      );
      return;
    }

    // ---- handshake step 4: the verdict --------------------------------
    if (msg.type === 'ack' && msg.cmd === 'auth') {
      // The success ack carries the device's identity (single-notify
      // frame — if auth got through, this got through). Register it HERE,
      // not on the first full state push: state is a large multi-chunk
      // payload, and a device whose state pushes were getting torn never
      // made it into the registry at all — "My Devices" kept showing one
      // entry with the old device's MAC no matter what was paired.
      if (msg.ok === true && typeof msg.mac === 'string' && msg.mac.length > 0) {
        activeMacRef.current = msg.mac;
        setActiveMac(msg.mac);
        upsertDevice({
          mac: msg.mac,
          bleName: typeof msg.name === 'string' && msg.name ? msg.name : undefined,
        });
      }
      authRef.current?.settle(
        msg.ok === true,
        msg.ok === true ? undefined : msg.message || 'Device rejected this app.'
      );
      return;
    }

    // Result of a scanWifi request — the firmware runs the scan off the
    // BLE task and pushes the list when it finishes, so this arrives
    // several seconds after the ack.
    if (msg.type === 'wifiScan') {
      setWifiNetworks(Array.isArray(msg.networks) ? msg.networks : []);
      setWifiScanning(false);
      return;
    }

    // Per-network scan result frames (small single-notify messages that
    // survive where the one big wifiScan push got torn over BLE). The
    // list fills progressively; wifiScanEnd stops the spinner.
    if (msg.type === 'wifiNet') {
      if (typeof msg.ssid === 'string' && msg.ssid.length > 0) {
        setWifiNetworks((prev) => {
          const base = msg.i === 0 ? [] : prev;
          if (base.some((n) => n.ssid === msg.ssid)) return base === prev ? prev : base;
          return [...base, { ssid: msg.ssid, rssi: typeof msg.rssi === 'number' ? msg.rssi : -70, secure: msg.secure === true }];
        });
      }
      return;
    }
    if (msg.type === 'wifiScanEnd') {
      setWifiScanning(false);
      return;
    }

    if (msg.type === 'wifi') {
      // Deliberately unconditional (not just __DEV__): this line in the
      // Metro terminal, matched against the firmware's "[ble] wifi event ->
      // conn N: delivered" serial line, proves the report crossed the air.
      // Only after this arrives does the UI say "connected".
      console.log(
        `[ble] wifi event received: state=${msg.state} ssid=${msg.ssid ?? ''} ip=${msg.ip ?? '-'} joinFailed=${msg.joinFailed === true}`
      );
      setWifi({
        state: msg.state as WifiPhase,
        ssid: msg.ssid || '',
        ip: msg.ip,
        joinFailed: msg.joinFailed === true,
      });
      // Only file the IP under the MAC the event itself carries. Guessing
      // via activeMacRef mid-pairing attributed the NEW device's IP to the
      // OLD device's record — the second device then never got a registry
      // entry of its own, and "My Devices" showed one row. Events from
      // firmware too old to send a mac just skip the cache; the next full
      // state push (which always carries msg.mac) fills it correctly.
      if (typeof msg.mac === 'string' && msg.mac.length > 0) {
        upsertDevice({
          mac: msg.mac,
          lastIp: typeof msg.ip === 'string' && msg.ip ? msg.ip : undefined,
          ssid: msg.ssid || null,
        });
      }
      return;
    }

    // Pump confirmation frame — small single-notify message that survives
    // where the big multi-chunk state push gets torn over BLE. This is
    // what flips the pill / clears "Turning on…" promptly on Bluetooth.
    if (msg.type === 'pump') {
      if (typeof msg.id === 'number') {
        setPumps((ps) =>
          ps.map((p) =>
            p.id === msg.id
              ? {
                  ...p,
                  active: msg.active === true,
                  speedRpm: typeof msg.speedRpm === 'number' ? msg.speedRpm : p.speedRpm,
                  name: typeof msg.name === 'string' && msg.name ? msg.name : p.name,
                }
              : p
          )
        );
      }
      return;
    }

    if (msg.type === 'state') applyRemoteState(msg);
    else if (msg.type === 'ack' && msg.ok === false) setLastError(`${msg.cmd}: ${msg.message || 'failed'}`);
  }, [applyRemoteState, writeLine, upsertDevice]);

  // ---- send a command over whichever transport is currently active ----
  // Wi-Fi is preferred; if it fails mid-flight the command is retried over
  // BLE when that link is also up, so a router hiccup doesn't lose a tap.
  //
  const sendCommandNow = useCallback(async (cmd: object) => {
    const http = httpRef.current;

    if (transportRef.current === 'wifi' && http) {
      try {
        const res = await httpSendCommand(http.ip, http.token, cmd);
        if (res?.type === 'ack' && res.ok === false) {
          setLastError(`${res.cmd}: ${res.message || 'failed'}`);
        }
        return;
      } catch (err: any) {
        // Fall through to BLE rather than surfacing an error, and count
        // the failure toward the health threshold so a drop noticed via a
        // tapped button demotes on the NEXT poll instead of two later.
        httpFailuresRef.current += 1;
        console.warn('Wi-Fi command failed, trying BLE', err?.message || err);
      }
    }

    const device = deviceRef.current;
    // Not connected, or connected but not yet authenticated: fall back to
    // local-only (the caller already applied the change optimistically).
    if (!device || !isConnectedRef.current || !isAuthedRef.current) return;
    try {
      await writeLine(device, cmd);
    } catch (err: any) {
      setLastError(`Send failed: ${err?.message || err}`);
    }
  }, [writeLine]);

  // Commands are SERIALIZED through this queue. Over HTTP, two rapid
  // sendCommand calls ("Turn on" fires setPumpConfig then setPumpMode)
  // used to race as parallel POSTs on separate connections — if the mode
  // flip landed first, the pump started with the previous set-point/type.
  const cmdQueueRef = useRef<Promise<void>>(Promise.resolve());
  const sendCommand = useCallback((cmd: object) => {
    const run = () => sendCommandNow(cmd);
    const next = cmdQueueRef.current.then(run, run);
    cmdQueueRef.current = next;
    return next;
  }, [sendCommandNow]);

  const disconnect = useCallback(async (opts?: { keepWifiUi?: boolean }) => {
    const device = deviceRef.current;
    connectGenRef.current += 1; // invalidate every in-flight callback of the old session
    // A superseded attempt's scan callbacks deliberately never touch the
    // scanner (they'd kill a successor's scan) — so the scan a still-running
    // attempt owns must be stopped HERE, or it leaks and runs forever.
    try { _bleManager?.stopDeviceScan(); } catch {}
    isConnectedRef.current = false;
    setBleUp(false);
    setDeviceFw(null); // stale pre-disconnect version must not drive update prompts
    isAuthedRef.current = false;
    authRef.current = null;
    connectInFlightRef.current = false;
    sessionNonceRef.current = null;

    // Tear down the LAN transport too — disconnect means "stop talking to
    // this device", not "stop talking over Bluetooth".
    if (httpPollRef.current) {
      clearInterval(httpPollRef.current);
      httpPollRef.current = null;
    }
    httpRef.current = null;
    transportRef.current = null;
    setTransport(null);

    try {
      if (device) await device.cancelConnection();
    } catch {}
    deviceRef.current = null;
    rxBufferRef.current = '';
    setStatus('disconnected');
    setDeviceLabel(null);

    // Clear per-device transient state, or the next pairing could act on
    // the PREVIOUS device's Wi-Fi status before the new one reports in.
    if (!opts?.keepWifiUi) {
      setWifi(null);
      setWifiNetworks([]);
      setWifiScanning(false);
    }
  }, []);

  // Runs the silent HMAC handshake described at the top of this file.
  // Resolves once the controller has accepted us; rejects on a bad
  // secret, a malformed reply, or a timeout. Nothing is shown to the
  // user while this runs — it completes in a couple hundred ms.
  const authenticate = useCallback((device: Device) => {
    return new Promise<void>((resolve, reject) => {
      let done = false;

      const timer = setTimeout(
        () => settle(false, 'The controller did not complete the security handshake in time.'),
        AUTH_TIMEOUT_MS
      );

      function settle(ok: boolean, message?: string) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        authRef.current = null;
        if (ok) resolve();
        else reject(new Error(message || 'Authentication failed.'));
      }

      authRef.current = { device, settle };

      // Kicks off step 1. We ask for the challenge rather than having the
      // firmware push it on connect, because a notify sent before our CCCD
      // subscribe lands would simply be dropped.
      writeLine(device, { cmd: 'hello' }).catch((err) =>
        settle(false, `Could not start the security handshake: ${err?.message || err}`)
      );
    });
  }, [writeLine]);

  // Finishes wiring up a connected Device: discover services, bump MTU,
  // subscribe to notifications, authenticate, and request the current
  // full state. Shared by both "fresh scan -> connect" and "reuse a stale
  // native connection" paths below.
  const attachToDevice = useCallback(async (raw: Device, advertisedName: string) => {
    const myGen = connectGenRef.current;

    const connected = await raw.discoverAllServicesAndCharacteristics();
    try { await connected.requestMTU(517); } catch {}

    deviceRef.current = connected;
    isConnectedRef.current = true;
    setBleUp(true);
    isAuthedRef.current = false;
    rxBufferRef.current = '';
    setDeviceLabel(advertisedName);
    setLastError(null);

    connected.onDisconnected(() => {
      // A disconnect event from a connection that has since been replaced
      // (scan-while-connected tears one down while building the next) must
      // not wipe the session that superseded it.
      if (connectGenRef.current !== myGen) return;
      isConnectedRef.current = false;
      setBleUp(false);
      setDeviceFw(null); // re-learned from the first state push after reconnect
      isAuthedRef.current = false;
      authRef.current?.settle(false, 'The controller closed the connection.');
      deviceRef.current = null;

      // Wi-Fi is carrying the session: losing the standby BLE link is not
      // a user-visible event. Stay "connected"; the demote path re-opens
      // BLE automatically if Wi-Fi later drops too.
      if (transportRef.current === 'wifi' && httpRef.current) return;

      // BLE was the active transport. A drop here is usually a supervision
      // timeout from a radio-contention burst (the device retrying Wi-Fi
      // joins is the classic one) — recoverable, so reconnect on the
      // user's behalf instead of parking the UI at "Not connected".
      setDeviceLabel(null);
      setStatus('connecting');
      setLastError('Bluetooth link dropped — reconnecting…');
      setTimeout(() => {
        if (connectGenRef.current !== myGen) return; // something newer took over
        if (connectInFlightRef.current || isConnectedRef.current) return;
        const rec = devicesRef.current.find((d) => d.mac === activeMacRef.current);
        if (rec?.bleName) {
          connectWithQrDataRef.current?.(
            JSON.stringify({ v: 2, name: rec.bleName, svc: rec.svc })
          ).catch(() => {});
        } else {
          setStatus('disconnected');
        }
      }, 1500); // give the OS a moment to release the dead connection
    });

    connected.monitorCharacteristicForService(SERVICE_UUID, CHAR_TX_UUID, (err, characteristic) => {
      if (err) {
        if (connectGenRef.current !== myGen) return;
        console.warn('[ble] TX monitor died:', err.message);
        lastRxAtRef.current = 0;
        return;
      }
      // A notify queued by a session that has since been torn down must
      // not write state — after "remove device" it re-inserted the very
      // record the user deleted.
      if (connectGenRef.current !== myGen) return;
      if (!characteristic?.value) return;
      lastRxAtRef.current = Date.now();
      rxBufferRef.current += base64ToUtf8(characteristic.value);
      let idx: number;
      while ((idx = rxBufferRef.current.indexOf('\n')) >= 0) {
        const line = rxBufferRef.current.slice(0, idx);
        rxBufferRef.current = rxBufferRef.current.slice(idx + 1);
        handleIncomingLine(line);
      }
    });
    lastRxAtRef.current = Date.now();

    setStatus('authenticating');
    try {
      await authenticate(connected);
    } catch (err: any) {
      // Failed the challenge — drop the link rather than sit on a
      // connection the controller will ignore anyway. Rethrow instead of
      // setting the error state here: the caller knows whether this attempt
      // can fall back (the stale-reuse path retries with a fresh scan) or
      // should surface the failure to the user.
      // Only wipe the shared refs if this attempt still owns the session —
      // a superseded attempt's 8s auth timer lands here AFTER a replacement
      // has fully connected, and wiping its refs left a zombie UI.
      if (connectGenRef.current === myGen) {
        isConnectedRef.current = false;
        setBleUp(false);
        isAuthedRef.current = false;
        deviceRef.current = null;
        setDeviceLabel(null);
      }
      try { await connected.cancelConnection(); } catch {}
      throw err;
    }

    if (connectGenRef.current !== myGen) {
      // Superseded while the handshake was in flight (disconnect or a newer
      // attempt bumped the generation just as the auth ack landed) — this
      // link is no longer the session's. Release it and back out.
      try { await connected.cancelConnection(); } catch {}
      throw new Error('Connection attempt superseded.');
    }

    isAuthedRef.current = true;
    setTransportBoth('ble');
    setStatus('connected');

    // Push the phone's clock. Without this a device with no Wi-Fi (and so
    // no NTP) sits near the epoch and its schedules never fire.
    await writeLine(connected, {
      cmd: 'setTime',
      epoch: Math.floor(Date.now() / 1000),
      tzOffsetSec: -new Date().getTimezoneOffset() * 60,
    });

    // The firmware pushes full state on a successful auth, but ask again
    // so a dropped notify can't leave us showing mock data.
    await writeLine(connected, { cmd: 'getState' });
  }, [handleIncomingLine, authenticate, writeLine, setTransportBoth]);

  // ============================================================
  //  WI-FI TRANSPORT LIFECYCLE
  // ============================================================
  const httpPollRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const httpFailuresRef = useRef(0);

  const stopHttpPolling = useCallback(() => {
    if (httpPollRef.current) {
      clearInterval(httpPollRef.current);
      httpPollRef.current = null;
    }
  }, []);

  // Assigned right after connectWithQrData is defined (it lives further
  // down the file because of its own dependencies). demoteToBle needs it
  // to fail over automatically, and a ref avoids the use-before-declaration
  // cycle between the two callbacks.
  const connectWithQrDataRef = useRef<((raw: string) => Promise<void>) | null>(null);

  // Wi-Fi is gone. Keep control working: instantly if a standby BLE link
  // is already up, otherwise by reconnecting over BLE on the user's
  // behalf. A session that began over Wi-Fi has no BLE link open, and
  // stopping at "disconnected, walk closer" made every router hiccup a
  // manual chore.
  const demoteToBle = useCallback(() => {
    stopHttpPolling();
    httpRef.current = null;
    if (isAuthedRef.current && isConnectedRef.current) {
      setTransportBoth('ble');
      setLastError('Wi-Fi unavailable — now controlling over Bluetooth.');
      return;
    }
    setTransportBoth(null);
    const rec = devicesRef.current.find((d) => d.mac === activeMacRef.current);
    if (rec?.bleName) {
      if (!connectInFlightRef.current) {
        setStatus('connecting');
        setLastError('Wi-Fi lost — switching to Bluetooth…');
        connectWithQrDataRef.current?.(
          JSON.stringify({ v: 2, name: rec.bleName, svc: rec.svc })
        ).catch(() => {});
      }
      return;
    }
    setStatus('disconnected');
    setLastError('Wi-Fi disconnected. Move within Bluetooth range of the device to keep controlling it.');
  }, [stopHttpPolling, setTransportBoth]);

  // HTTP has no push channel, so state is polled. Repeated failures mean
  // the device fell off the LAN (router down, DHCP change, powered off).
  const startHttpPolling = useCallback(() => {
    stopHttpPolling();
    httpFailuresRef.current = 0;

    // Second opinion, taken 500ms after a first failure instead of a full
    // poll interval later. A single blip recovers here and nobody notices;
    // a real outage reaches the demotion threshold in well under 2s when
    // the network errors are immediate (phone Wi-Fi switched off).
    const confirmFailure = async () => {
      const http = httpRef.current;
      if (!http || httpFailuresRef.current === 0) return; // recovered meanwhile
      try {
        const state = await httpGetState(http.ip, http.token);
        // The transport this response belongs to may have been torn down
        // while the request was in flight (disconnect, device removed) —
        // applying it would resurrect the old device's state.
        if (httpRef.current !== http) return;
        httpFailuresRef.current = 0;
        applyRemoteState(state);
      } catch {
        if (httpRef.current !== http) return;
        httpFailuresRef.current += 1;
        if (httpFailuresRef.current >= HTTP_FAIL_THRESHOLD) demoteToBle();
      }
    };

    httpPollRef.current = setInterval(async () => {
      const http = httpRef.current;
      if (!http) return;
      try {
        const state = await httpGetState(http.ip, http.token);
        if (httpRef.current !== http) return; // torn down mid-request — see above
        httpFailuresRef.current = 0;
        applyRemoteState(state);
      } catch {
        if (httpRef.current !== http) return;
        httpFailuresRef.current += 1;
        if (httpFailuresRef.current >= HTTP_FAIL_THRESHOLD) demoteToBle();
        else setTimeout(confirmFailure, 500);
      }
    }, HTTP_POLL_MS);
  }, [stopHttpPolling, applyRemoteState, demoteToBle]);

  // Attempts the LAN transport against a cached IP. Returns false (rather
  // than throwing) so callers can simply fall through to BLE.
  //
  // expectedMac is the identity check: every controller on the LAN accepts
  // the same shared secret, so an IP that silently changed hands (DHCP, or
  // a stale cache during multi-device pairing) would otherwise connect
  // "successfully" to the WRONG box — two paired devices then look and act
  // like one. The /challenge response carries the responder's MAC; if it
  // isn't the device this record points at, repair the caches and bail to
  // BLE, which matches by advertised name and cannot mis-target.
  const connectHttp = useCallback(async (ip: string, expectedMac?: string): Promise<boolean> => {
    let mine: { ip: string; token: string; nonce: string } | null = null;
    try {
      const { token, mac, nonce } = await httpAuthenticate(ip);
      // A retry/promote tick that was already in flight when the user
      // removed this device would otherwise finish the connect and
      // re-register the record they just deleted (the serial log showed
      // exactly this: "[http] client authenticated" right after the
      // removal). If the target is gone from the registry, abandon.
      if (expectedMac && !devicesRef.current.some((d) => d.mac === expectedMac)) {
        console.log(`[http] ${ip}: target ${expectedMac} was removed mid-attempt — abandoning`);
        return false;
      }
      if (expectedMac && mac && mac !== expectedMac) {
        console.log(`[http] ${ip} answered as ${mac}, expected ${expectedMac} — refusing and re-homing the IP`);
        await upsertDevice({ mac, lastIp: ip });            // the IP really belongs to this device
        await upsertDevice({ mac: expectedMac, lastIp: null }); // stop retrying the stale address
        return false;
      }
      mine = { ip, token, nonce };
      httpRef.current = mine;

      const state = await httpGetState(ip, token);
      applyRemoteState(state);

      await httpSendCommand(ip, token, {
        cmd: 'setTime',
        epoch: Math.floor(Date.now() / 1000),
        tzOffsetSec: -new Date().getTimezoneOffset() * 60,
      }).catch(() => {});

      setTransportBoth('wifi');
      setStatus('connected');
      setLastError(null);
      startHttpPolling();
      return true;
    } catch {
      // Only tear down what THIS attempt installed: overlapping attempts
      // (the promote tick and the wifi-online effect both run this) meant
      // a late failure here could null a live transport a concurrent
      // attempt had just established — leaving polling wedged on !http.
      if (mine && httpRef.current === mine) httpRef.current = null;
      return false;
    }
  }, [applyRemoteState, startHttpPolling, setTransportBoth, upsertDevice]);

  // Cancels any native-level connection to a peripheral matching the given
  // id, ignoring errors. Used to clear out a connection that survived a JS
  // reload (Metro fast refresh, app relaunch without a clean disconnect,
  // etc.) — the OS still thinks it's connected even though our deviceRef
  // was wiped, which is exactly what causes "operation was cancelled" on
  // the next connect attempt.
  const forceReleaseNative = useCallback(async (manager: BleManager, id: string) => {
    try { await manager.cancelDeviceConnection(id); } catch {}
  }, []);

  const connectWithQrData = useCallback(async (raw: string) => {
    // Hard serialisation of the whole connect flow. Two overlapping runs each
    // start their own scan and each call connect() on the same peripheral —
    // and in react-native-ble-plx the later connect cancels the earlier one,
    // so neither completes and the device never even logs a connection. A
    // flag local to this function cannot prevent that, because each
    // invocation would get its own copy; it has to live in a ref.
    if (connectInFlightRef.current) return;
    connectInFlightRef.current = true;

    // Claim a fresh generation. If disconnect() (or a newer attempt) bumps
    // it while we're mid-flight, every later step of THIS attempt backs out.
    const myGen = ++connectGenRef.current;

    const fail = (message: string) => {
      if (connectGenRef.current !== myGen) return; // superseded — not ours to report
      connectInFlightRef.current = false;
      setLastError(message);
      setStatus('error');
    };

    setLastError(null);
    setStatus('connecting');

    let payload: QrPayload;
    try {
      payload = JSON.parse(raw);
      if (!payload.name || !payload.svc) throw new Error('missing name/svc');
    } catch {
      fail('QR code is not a valid device pairing code.');
      return;
    }

    const manager = getBleManager();
    if (!manager) {
      fail("Bluetooth isn't available in this build. Run the app from a dev build (npx expo run:ios/android), not Expo Go.");
      return;
    }

    // The catch matters: a throw here (requestMultiple can reject) would
    // leave connectInFlightRef stuck true, silently eating every later
    // connect attempt until the user taps Disconnect.
    const hasPermission = await requestBlePermissions().catch(() => false);
    if (!hasPermission) {
      fail('Bluetooth permission was denied. Enable it in Settings.');
      return;
    }

    const poweredOn = await waitForPoweredOn(manager);
    if (!poweredOn) {
      fail('Turn on Bluetooth to connect.');
      return;
    }

    // ---- Recover a connection that survived a JS reload ----
    // If our JS state was wiped (fast refresh, app relaunch) but the OS
    // still holds an active GATT connection to this device, scanning for
    // it won't find it (already-connected peripherals often don't show up
    // in scan results) and a fresh connect() call gets auto-cancelled by
    // the OS. Check for and reuse that connection instead of fighting it.
    try {
      const already = await manager.connectedDevices([payload.svc]);
      const stale = already.find((d) => (d.name || d.localName) === payload.name);
      if (stale) {
        try {
          await attachToDevice(stale, payload.name);
          connectInFlightRef.current = false;
          return;
        } catch {
          // The lingering connection was half-dead — typical right after a
          // disconnect the OS hasn't finished releasing (scan-QR-while-
          // connected does exactly this). Force it out and fall through to
          // a fresh scan instead of stranding the pairing chain.
          await forceReleaseNative(manager, stale.id);
          await new Promise((r) => setTimeout(r, 500));
        }
      }
    } catch {
      // ignore — fall through to a normal scan
    }
    if (connectGenRef.current !== myGen) return; // superseded while recovering

    let found = false;

    // startDeviceScan fires its callback once per ADVERTISEMENT PACKET, and
    // the ESP32 advertises continuously (every few hundred ms). stopDeviceScan
    // does not retroactively cancel invocations already queued by the native
    // side, so without this guard the callback re-enters while the first
    // connect() is still in flight — and a second connect() to the same
    // peripheral cancels the first. That is the "Operation was cancelled"
    // error, and because the attempts keep superseding each other the link
    // may never establish at all (which is why the ESP32 logged nothing).
    let claimed = false;

    const scanTimeout = setTimeout(() => {
      if (!found && connectGenRef.current === myGen) {
        manager.stopDeviceScan();
        fail(`Couldn't find "${payload.name}" nearby. Make sure the device is powered on and try again.`);
      }
    }, 12000);

    manager.startDeviceScan([payload.svc], null, async (error, scannedDevice) => {
      if (claimed) return; // already connecting to this device — ignore further packets
      // Superseded: bail WITHOUT touching the scanner. stopDeviceScan()
      // is global, and a leftover callback from a replaced attempt calling
      // it here killed the SUCCESSOR's scan — which then sat silent for
      // 12s and reported "couldn't find nearby" for a device that was
      // advertising the whole time. (Remove device → re-scan QR hit this
      // reliably: the background retry's scan overlapped the new one.)
      if (connectGenRef.current !== myGen) return;

      if (error) {
        clearTimeout(scanTimeout);
        manager.stopDeviceScan();
        fail(error.message || 'Scan failed.');
        return;
      }
      if (!scannedDevice) return;
      const advertisedName = scannedDevice.name || scannedDevice.localName || '';
      if (advertisedName !== payload.name) return; // wrong device, keep scanning

      claimed = true;
      found = true;
      clearTimeout(scanTimeout);
      manager.stopDeviceScan();

      // ---- phase 1: establish the link -------------------------------
      // Try once, and if the OS cancels the attempt (stale connection
      // record it hasn't let go of yet), force-release and retry once.
      let connected: Device | null = null;
      for (let attempt = 0; attempt < 2 && !connected; attempt++) {
        try {
          connected = await scannedDevice.connect();
        } catch (err: any) {
          const msg = String(err?.message || err || '');
          if (attempt === 0 && /cancel/i.test(msg)) {
            await forceReleaseNative(manager, scannedDevice.id);
            await new Promise((r) => setTimeout(r, 500));
            continue;
          }
          fail(`Connection failed: ${msg}`);
          return;
        }
      }
      if (!connected) {
        fail('Connection failed: could not establish a link.');
        return;
      }
      if (connectGenRef.current !== myGen) {
        // Something newer owns the session now — release the link we built
        // rather than fighting over shared state.
        try { await connected.cancelConnection(); } catch {}
        return;
      }

      // ---- phase 2: discovery, auth, first sync ----------------------
      // Deliberately NOT inside the retry loop above. Once the link is up,
      // re-running connect() would cancel the connection we already hold and
      // report a misleading "cancelled" error for what is really a setup
      // failure.
      try {
        await attachToDevice(connected, advertisedName);
        connectInFlightRef.current = false;
      } catch (err: any) {
        try { await connected.cancelConnection(); } catch {}
        fail(`Connected, but setup failed: ${err?.message || err}`);
      }
    });
  }, [attachToDevice, forceReleaseNative]);

  // Keep the failover path (demoteToBle, defined above) pointed at the
  // current connect implementation.
  useEffect(() => { connectWithQrDataRef.current = connectWithQrData; }, [connectWithQrData]);

  // Recover from the phone's Bluetooth being toggled off and on. The
  // radio dying takes the link down and the reconnect attempt correctly
  // fails with "Turn on Bluetooth" — but nothing watched for the radio
  // coming BACK, so the app sat at "Connection problem" until a manual
  // tap. This listens for the adapter's PoweredOn transition and picks
  // the session back up by itself.
  useEffect(() => {
    const manager = getBleManager();
    if (!manager) return;
    const sub = manager.onStateChange((state) => {
      if (state !== BleState.PoweredOn) return;
      if (transportRef.current === 'wifi' && httpRef.current) return; // Wi-Fi is carrying the session
      if (connectInFlightRef.current || isConnectedRef.current) return;
      if (statusRef.current === 'connected') return;
      const rec = devicesRef.current.find((d) => d.mac === activeMacRef.current);
      if (!rec?.bleName) return;
      setStatus('connecting');
      setLastError('Bluetooth is back — reconnecting…');
      connectWithQrDataRef.current?.(
        JSON.stringify({ v: 2, name: rec.bleName, svc: rec.svc })
      ).catch(() => {});
    }, false); // transitions only — initial connect is the mount effect's job
    return () => sub.remove();
  }, []);

  // While on BLE, keep testing whether Wi-Fi has come back. When it has,
  // the LAN transport takes over silently — the BLE link is deliberately
  // left open so a subsequent Wi-Fi drop fails back instantly rather than
  // having to re-scan and re-authenticate.
  useEffect(() => {
    if (transport !== 'ble') return;
    const timer = setInterval(async () => {
      if (transportRef.current !== 'ble') return;
      const rec = devicesRef.current.find((d) => d.mac === activeMacRef.current);
      if (!rec?.lastIp) return;
      await connectHttp(rec.lastIp, rec.mac);
    }, WIFI_PROMOTE_RETRY_MS);
    return () => clearInterval(timer);
  }, [transport, connectHttp]);

  // Promote the moment the device reports Wi-Fi online, rather than
  // making the user stare at "Wi-Fi · ready" for up to 15s of retry
  // interval. The periodic loop above stays as the retry path.
  useEffect(() => {
    if (wifi?.state !== 'online' || transport !== 'ble') return;
    const ip = wifi.ip
      ?? devicesRef.current.find((d) => d.mac === activeMacRef.current)?.lastIp;
    if (ip) connectHttp(ip, activeMacRef.current ?? undefined);
  }, [wifi?.state, wifi?.ip, transport, connectHttp]);

  useEffect(() => stopHttpPolling, [stopHttpPolling]);

  // ============================================================
  //  WI-FI PROVISIONING  (BLE only — see the firmware's reasoning)
  // ============================================================
  // The BLE scan path's spinner-clear timer, tracked so a rescan cancels
  // the previous scan's timer instead of inheriting its 20s deadline.
  const wifiScanTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const scanWifiNetworks = useCallback(async () => {
    const device = deviceRef.current;
    const http = httpRef.current;
    setWifiNetworks([]);
    setWifiScanning(true);
    // Cancel the previous scan's spinner-clear timer: left running, it
    // killed a rescan's spinner at its own 20s mark with results pending.
    if (wifiScanTimeoutRef.current) {
      clearTimeout(wifiScanTimeoutRef.current);
      wifiScanTimeoutRef.current = null;
    }

    // The device caches its latest scan at /wifiScan — polled as the
    // result channel over HTTP, and as the safety net for the BLE push
    // (a large multi-chunk message that radio contention can tear).
    const pollResults = (h: { ip: string; token: string; nonce: string }) => {
      const deadline = Date.now() + 20000;
      const poll = async () => {
        if (httpRef.current !== h) { setWifiScanning(false); return; }
        try {
          const res = await httpFetch(h.ip, '/wifiScan', { headers: { Authorization: `Bearer ${h.token}` } });
          const json = await res.json();
          if (Array.isArray(json?.networks)) {
            setWifiNetworks(json.networks);
            setWifiScanning(false);
            return;
          }
        } catch { /* transient — keep polling until the deadline */ }
        if (Date.now() < deadline) setTimeout(poll, 1500);
        else setWifiScanning(false);
      };
      setTimeout(poll, 2500); // the device-side scan itself takes a couple of seconds
    };

    if (device && isAuthedRef.current) {
      try {
        await writeLine(device, { cmd: 'scanWifi' });
      } catch (err: any) {
        setWifiScanning(false);
        setLastError(`Could not start Wi-Fi scan: ${err?.message || err}`);
        return;
      }
      // Results arrive as a wifiScan push; the HTTP poll (when available)
      // covers a torn push. Clear the spinner if nothing ever lands.
      if (http) pollResults(http);
      wifiScanTimeoutRef.current = setTimeout(() => setWifiScanning(false), 20000);
      return;
    }

    if (http) {
      try {
        await httpSendCommand(http.ip, http.token, { cmd: 'scanWifi' });
      } catch (err: any) {
        setWifiScanning(false);
        setLastError(`Could not start Wi-Fi scan: ${err?.message || err}`);
        return;
      }
      pollResults(http);
      return;
    }

    setWifiScanning(false);
    setLastError('Connect to the device to scan for networks.');
  }, [writeLine]);

  const provisionWifi = useCallback(async (ssid: string, password: string) => {
    const device = deviceRef.current;
    const nonce = sessionNonceRef.current;
    if (device && isAuthedRef.current && nonce) {
      try {
        await writeLine(device, {
          cmd: 'setWifi',
          ssid,
          // Encrypted against this session's nonce so the password is never
          // exposed on air, independent of link-layer encryption.
          passEnc: keystreamEncryptHex(nonce, password),
          tzOffsetSec: -new Date().getTimezoneOffset() * 60,
        });
        setWifi({ state: 'connecting', ssid });
      } catch (err: any) {
        setLastError(`Could not send Wi-Fi credentials: ${err?.message || err}`);
      }
      return;
    }

    // No BLE link — provision over the LAN (encrypted against the HTTP
    // auth nonce). Joining a different network drops this link mid-flight;
    // the app's BLE fallback then picks the device back up.
    const http = httpRef.current;
    if (http) {
      try {
        // Re-authenticate first: the firmware keeps a single HTTP nonce, so
        // any /challenge since ours (another phone, our own OTA flow) has
        // rotated it and the password would decrypt to garbage.
        const fresh = await httpAuthenticate(http.ip);
        httpRef.current = { ip: http.ip, token: fresh.token, nonce: fresh.nonce };
        await httpSendCommand(http.ip, fresh.token, {
          cmd: 'setWifi',
          ssid,
          passEnc: keystreamEncryptHex(fresh.nonce, password),
          tzOffsetSec: -new Date().getTimezoneOffset() * 60,
        });
        setWifi({ state: 'connecting', ssid });
      } catch (err: any) {
        setLastError(`Could not send Wi-Fi credentials: ${err?.message || err}`);
      }
      return;
    }

    setLastError('Connect to the device to set up Wi-Fi.');
  }, [writeLine]);

  // Asks the device for fresh state over whichever transport is live.
  const refreshState = useCallback(async () => {
    const http = httpRef.current;
    if (transportRef.current === 'wifi' && http) {
      try {
        applyRemoteState(await httpGetState(http.ip, http.token));
        return;
      } catch { /* fall through to BLE */ }
    }
    const device = deviceRef.current;
    if (device && isAuthedRef.current) {
      await writeLine(device, { cmd: 'getState' }).catch(() => {});
    }
  }, [applyRemoteState, writeLine]);

  // While a join is in flight, poll for the outcome. If the link drops OR
  // goes deaf (subscription died / notifies torn by Wi-Fi coexistence),
  // reconnect so we can actually hear the "online" report.
  useEffect(() => {
    if (wifi?.state !== 'connecting') return;
    const startedAt = Date.now();
    let reconnecting = false;
    const timer = setInterval(async () => {
      if (Date.now() - startedAt > 60000) {
        clearInterval(timer);
        setWifi((w) => (w && w.state === 'connecting' ? { ...w, state: 'offline', joinFailed: true } : w));
        setLastError('No answer from the device while joining Wi-Fi. Check the password and try again.');
        return;
      }
      if (statusRef.current === 'connected') {
        // Deafness is a BLE-notify concept: on a Wi-Fi-only session lastRxAt
        // never ticks, so without this gate a healthy HTTP transport would
        // always read as deaf and get torn down mid-join.
        const bleLinkUp = deviceRef.current != null && isAuthedRef.current;
        const deaf = bleLinkUp && (lastRxAtRef.current === 0 || Date.now() - lastRxAtRef.current > 8000);
        if (!deaf) { refreshState(); return; }
        console.warn('[ble] link is deaf — reconnecting');
        await disconnect({ keepWifiUi: true });
        return;
      }
      if (reconnecting || connectInFlightRef.current) return;
      reconnecting = true;
      try {
        const rec = devicesRef.current.find((d) => d.mac === activeMacRef.current)
          ?? devicesRef.current[devicesRef.current.length - 1];
        if (rec?.bleName) {
          await connectWithQrData(JSON.stringify({ v: 2, name: rec.bleName, svc: rec.svc }));
        }
      } finally {
        reconnecting = false;
      }
    }, 2000);
    return () => clearInterval(timer);
  }, [wifi?.state, refreshState, connectWithQrData, disconnect]);

  const forgetWifi = useCallback(async () => {
    // Deliver the command BEFORE tearing the LAN transport down — over
    // BLE when that link is up, otherwise over the very network being
    // forgotten (tearing down first silently dropped the command when
    // the app was connected via Wi-Fi only).
    const device = deviceRef.current;
    const http = httpRef.current;
    if (device && isAuthedRef.current) {
      await writeLine(device, { cmd: 'forgetWifi' }).catch(() => {});
    } else if (http) {
      await httpSendCommand(http.ip, http.token, { cmd: 'forgetWifi' }).catch(() => {});
    }
    stopHttpPolling();
    httpRef.current = null;
    if (transportRef.current === 'wifi') {
      if (isAuthedRef.current) {
        setTransportBoth('ble');
      } else {
        // That was the only transport: reflect a real disconnect, or the UI
        // stays "Wi-Fi Connected" forever with every command silently
        // dropped and the reconnect loop (gated on disconnected/error)
        // never engaging.
        setTransportBoth(null);
        setStatus('disconnected');
        setDeviceLabel(null);
      }
    }
    // Reflect it immediately rather than waiting for the device's push —
    // the firmware's announce/state broadcast then confirms it. Without
    // this the UI kept showing the old network even though the device had
    // already dropped off Wi-Fi.
    setWifi({ state: 'unconfigured', ssid: '' });
    setWifiNetworks([]);
    const mac = activeMacRef.current;
    if (mac) await upsertDevice({ mac, lastIp: null, ssid: null });
  }, [writeLine, stopHttpPolling, setTransportBoth, upsertDevice]);

  // ============================================================
  //  FIRMWARE OTA — download the release image and push it to the
  //  device's /update endpoint over the LAN. Independent of the current
  //  transport: it authenticates its own HTTP session against the
  //  device's IP, so it works even while the app is controlling via BLE
  //  (as long as the phone is on the same network).
  // ============================================================
  const otaPerform = useCallback(async (
    binUrl: string,
    targetVersion: string,
    onPhase: (label: string, pct: number) => void
  ) => {
    const ip = wifi?.ip
      || (activeMacRef.current ? devicesRef.current.find((d) => d.mac === activeMacRef.current)?.lastIp : undefined);
    if (!ip) throw new Error('No known IP address for this device — connect it to Wi-Fi first.');

    onPhase('Checking the device on this network…', 0);
    let pingMac = '';
    try {
      const res = await httpFetch(ip, '/ping');
      const json = await res.json();
      pingMac = json?.mac || '';
    } catch {
      throw new Error(`Can't reach the controller at ${ip}. Connect your phone to the same Wi-Fi network as the device (router client-isolation also blocks this).`);
    }
    if (activeMacRef.current && pingMac && pingMac !== activeMacRef.current)
      throw new Error(`A different device answered at ${ip} — reconnect over Bluetooth to refresh its address.`);

    const { token } = await httpAuthenticate(ip);

    onPhase('Downloading firmware…', 0);
    const dl = await fetch(binUrl);
    if (!dl.ok) throw new Error(`Download failed (${dl.status}) — check the release URL.`);
    const blob = await dl.blob();
    if (blob.size < 100000) throw new Error('Downloaded file is too small to be a firmware image.');

    onPhase('Uploading to device…', 0);
    await new Promise<void>((resolve, reject) => {
      // XHR instead of fetch for upload progress events.
      const xhr = new XMLHttpRequest();
      xhr.open('POST', `http://${ip}/update`);
      xhr.setRequestHeader('Authorization', `Bearer ${token}`);
      xhr.timeout = 120000;
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) onPhase('Uploading to device…', e.loaded / e.total);
      };
      xhr.onload = () => {
        try {
          const json = JSON.parse(xhr.responseText);
          if (xhr.status === 200 && json?.ok !== false) resolve();
          else reject(new Error(json?.message || `Device rejected the update (${xhr.status}).`));
        } catch {
          xhr.status === 200 ? resolve() : reject(new Error(`Device rejected the update (${xhr.status}).`));
        }
      };
      xhr.onerror = () => reject(new Error('Upload failed — the connection to the device dropped.'));
      xhr.ontimeout = () => reject(new Error('Upload timed out.'));
      xhr.send(blob);
    });

    // The device verifies the image and reboots. Poll /ping until the NEW
    // version answers — the proof the update actually took.
    onPhase('Device is rebooting…', 1);
    const deadline = Date.now() + 90000;
    let lastFw: string | null = null;
    while (Date.now() < deadline) {
      await new Promise((r) => setTimeout(r, 3000));
      try {
        const res = await httpFetch(ip, '/ping');
        const json = await res.json();
        if (json?.fw === targetVersion) return;
        // The OLD firmware can legitimately answer here — it flushes the
        // /update response before its scheduled reboot fires. Not a verdict
        // yet: remember it, keep polling, and only conclude at the deadline.
        if (typeof json?.fw === 'string') lastFw = json.fw;
      } catch {
        // still rebooting — keep polling
      }
    }
    if (lastFw && lastFw !== targetVersion) {
      throw new Error(`Device came back reporting ${lastFw} — the update did not stick.`);
    }
    throw new Error('Device did not come back within 90s. Power-cycle it and check the firmware version — the previous firmware still boots if the update failed.');
  }, [wifi?.ip]);

  // ============================================================
  //  DEVICE REGISTRY MANAGEMENT
  // ============================================================
  const renameDevice = useCallback(async (mac: string, name: string) => {
    const trimmed = name.trim();
    if (!trimmed) return;
    // The name is only ever a label on the MAC, so this never affects
    // which physical device the app is talking to.
    const next = devicesRef.current.map((d) => (d.mac === mac ? { ...d, name: trimmed } : d));
    devicesRef.current = next;
    setDevices(next);
    await saveDevices(next);
  }, []);

  const removeDevice = useCallback(async (mac: string) => {
    // Disconnect FIRST. Filtering the list while the link was still up
    // left a window where the device's next state push re-inserted the
    // record that was just removed (and restored it as active) — the
    // recovery loop then silently reconnected to it, and a device added
    // afterwards was fighting a registry that never let go of the old one.
    if (activeMacRef.current === mac) {
      activeMacRef.current = null;
      setActiveMac(null);
      await saveActiveMac(null);
      await disconnect();
    }
    const next = devicesRef.current.filter((d) => d.mac !== mac);
    devicesRef.current = next;
    setDevices(next);
    await saveDevices(next);
  }, [disconnect]);

  // Connects to an already-registered device: Wi-Fi first when we have an
  // address for it, BLE otherwise. The BLE path also refreshes the cached
  // IP, which is how a DHCP-reassigned address repairs itself.
  const connectToRecord = useCallback(async (rec: DeviceRecord) => {
    activeMacRef.current = rec.mac;
    setActiveMac(rec.mac);
    await saveActiveMac(rec.mac);
    setLastError(null);

    if (rec.lastIp) {
      setStatus('connecting');
      if (await connectHttp(rec.lastIp, rec.mac)) return;
    }
    await connectWithQrData(JSON.stringify({ v: 2, name: rec.bleName, svc: rec.svc }));
  }, [connectHttp, connectWithQrData]);

  // Last-resort recovery loop. Once every failover above has been
  // exhausted the app lands at 'error' or 'disconnected' — and stayed
  // there, because the events that would end the outage (the phone's
  // Wi-Fi coming back, the device powering up, walking back into BLE
  // range) are ones we get no notification for. So while a device is
  // still selected, quietly retry the full connect (Wi-Fi first, BLE
  // fallback) until one sticks. Re-entering the state restarts the
  // interval, so each failed attempt naturally spaces out the next one.
  const autoRetryBusyRef = useRef(false);
  useEffect(() => {
    if (status !== 'error' && status !== 'disconnected') return;

    // Fast lane: probe the cached IP over HTTP every 3s. A failed probe
    // costs a fraction of a second and touches no UI state (no status
    // flapping), so it can run this often — it's what makes recovery
    // feel immediate once the network is back.
    const fastTimer = setInterval(async () => {
      if (autoRetryBusyRef.current || connectInFlightRef.current) return;
      if (statusRef.current !== 'error' && statusRef.current !== 'disconnected') return;
      const rec = devicesRef.current.find((d) => d.mac === activeMacRef.current);
      if (!rec?.lastIp) return;
      autoRetryBusyRef.current = true;
      try { await connectHttp(rec.lastIp, rec.mac); } catch {} finally { autoRetryBusyRef.current = false; }
    }, 3000);

    // Slow lane: the full connect (Wi-Fi, then a 12s BLE scan) every 15s,
    // for outages the probe can't fix — no cached IP, DHCP moved the
    // device, or it's only reachable over Bluetooth.
    const fullTimer = setInterval(async () => {
      if (autoRetryBusyRef.current || connectInFlightRef.current) return;
      if (statusRef.current !== 'error' && statusRef.current !== 'disconnected') return;
      const rec = devicesRef.current.find((d) => d.mac === activeMacRef.current);
      if (!rec) return; // nothing selected (e.g. device was removed) — stay quiet
      autoRetryBusyRef.current = true;
      try { await connectToRecord(rec); } catch {} finally { autoRetryBusyRef.current = false; }
    }, 15000);

    return () => { clearInterval(fastTimer); clearInterval(fullTimer); };
  }, [status, connectToRecord, connectHttp]);

  const selectDevice = useCallback(async (mac: string) => {
    const rec = devicesRef.current.find((d) => d.mac === mac);
    if (!rec) return;
    if (activeMacRef.current === mac && statusRef.current === 'connected') return;
    await disconnect();
    // Clear the PREVIOUS device's data rather than showing it under the
    // new device's name until the first state push lands — with two
    // similar controllers that stale carry-over reads as "switching does
    // nothing".
    setRelays(initialRelays);
    setPumps(initialPumps);
    setPresets(initialPresets);
    await connectToRecord(rec);
  }, [disconnect, connectToRecord]);

  // Restore the saved device list on launch and reconnect to whichever
  // device was last in use, so the app comes back where you left it.
  useEffect(() => {
    let cancelled = false;
    (async () => {
      const [stored, lastMac] = await Promise.all([loadDevices(), loadActiveMac()]);
      if (cancelled) return;
      devicesRef.current = stored;
      setDevices(stored);
      const rec = stored.find((d) => d.mac === lastMac) ?? stored[0];
      if (rec) {
        activeMacRef.current = rec.mac;
        setActiveMac(rec.mac);
        connectToRecord(rec);
      }
    })();
    return () => { cancelled = true; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Manual recovery: releases any native-level connection the OS is still
  // holding for this device, even if our own JS state thinks we're
  // disconnected. Exposed in the Device tab as "Reset Bluetooth Connection".
  const forceReset = useCallback(async () => {
    const manager = getBleManager();
    if (!manager) return;
    try {
      const already = await manager.connectedDevices([SERVICE_UUID]);
      await Promise.all(already.map((d) => forceReleaseNative(manager, d.id)));
    } catch {}
    deviceRef.current = null;
    isConnectedRef.current = false;
    setBleUp(false);
    isAuthedRef.current = false;
    authRef.current = null;
    connectInFlightRef.current = false;
    rxBufferRef.current = '';
    setStatus('disconnected');
    setDeviceLabel(null);
    setLastError('Bluetooth connection reset. Try connecting again.');
  }, [forceReleaseNative]);

  useEffect(() => {
    return () => { disconnect(); };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // ---- mutators: optimistic local update + fire-and-forget command ----
  const setRelayModeFn = useCallback((id: number, mode: RelayMode) => {
    setRelays((rs) => rs.map((r) => (r.id === id ? { ...r, mode, active: mode === 'on' ? true : mode === 'off' ? false : r.active } : r)));
    sendCommand({ cmd: 'setRelayMode', id, mode });
  }, [sendCommand]);

  const setRelayNameFn = useCallback((id: number, name: string) => {
    setRelays((rs) => rs.map((r) => (r.id === id ? { ...r, name } : r)));
    sendCommand({ cmd: 'setRelayName', id, name });
  }, [sendCommand]);

  const setPumpModeFn = useCallback((id: number, mode: RelayMode) => {
    // Mode is reflected optimistically. `active` stays confirmation-driven
    // for Pentair/Emaux (their pill means "the pump acknowledged over
    // RS-485") — but Black & Decker is fire-and-forget by design: no reply
    // is ever read, the firmware marks it applied on send, so the pill
    // flips right here on both ON and OFF ("sent" semantics).
    setPumps((ps) => ps.map((p) => {
      if (p.id !== id) return p;
      if (p.pumpType === 'Black & Decker') {
        return { ...p, mode, active: mode === 'on' ? true : mode === 'off' ? false : p.active };
      }
      return { ...p, mode };
    }));
    sendCommand({ cmd: 'setPumpMode', id, mode });
  }, [sendCommand]);

  const setPumpConfigFn = useCallback((id: number, patch: Partial<Pick<PumpState, 'pumpType' | 'speedRpm' | 'name'>>) => {
    // A speed sent to the device is the manual SET-POINT — mirror it there
    // locally too, so the input shows it even before the device echoes it.
    if (patch.speedRpm !== undefined) {
      setPumps((ps) => ps.map((p) => (p.id === id ? { ...p, setSpeedRpm: patch.speedRpm! } : p)));
    }
    setPumps((ps) => ps.map((p) => (p.id === id ? { ...p, ...patch } : p)));
    sendCommand({ cmd: 'setPumpConfig', id, ...patch });
  }, [sendCommand]);

  const addScheduleFn = useCallback((input: ScheduleInput) => {
    const localId = `local-${Date.now()}`;
    setPresets((ps) => [...ps, {
      id: localId,
      targetLabel: input.target,
      timeStr: formatTimeStr(input.hour24, input.minute),
      durationStr: formatDurationStr(input.durationMin),
      enabled: input.enabled,
      hour24: input.hour24,
      minute: input.minute,
      durationMin: input.durationMin,
      speedRpm: input.speedRpm,
      pumpType: input.pumpType,
    }]);
    sendCommand({ cmd: 'addSchedule', schedule: input });
  }, [sendCommand]);

  const updateScheduleFn = useCallback((id: string, input: ScheduleInput) => {
    setPresets((ps) => ps.map((p) => (p.id === id ? {
      ...p,
      targetLabel: input.target,
      timeStr: formatTimeStr(input.hour24, input.minute),
      durationStr: formatDurationStr(input.durationMin),
      hour24: input.hour24,
      minute: input.minute,
      durationMin: input.durationMin,
      speedRpm: input.speedRpm,
      pumpType: input.pumpType,
    } : p)));
    // A just-added row still carries its optimistic "local-…" id until the
    // next state push delivers the real "p<n>" — the firmware can only
    // reject that id, and the doomed command's revert made the UI flicker.
    // Ask for fresh state instead so the id resolves quickly.
    if (id.startsWith('local-')) { refreshState().catch(() => {}); return; }
    sendCommand({ cmd: 'updateSchedule', id, schedule: input });
  }, [sendCommand, refreshState]);

  const deleteScheduleFn = useCallback((id: string) => {
    setPresets((ps) => ps.filter((p) => p.id !== id));
    if (id.startsWith('local-')) { refreshState().catch(() => {}); return; }
    sendCommand({ cmd: 'deleteSchedule', id });
  }, [sendCommand, refreshState]);

  const toggleScheduleFn = useCallback((id: string, enabled: boolean) => {
    setPresets((ps) => ps.map((p) => (p.id === id ? { ...p, enabled } : p)));
    if (id.startsWith('local-')) { refreshState().catch(() => {}); return; }
    sendCommand({ cmd: 'toggleSchedule', id, enabled });
  }, [sendCommand, refreshState]);

  const setRelay2DependencyFn = useCallback((depends: boolean) => {
    setR2Depends(depends); // optimistic; the next state push confirms
    sendCommand({ cmd: 'setRelay2Dependency', depends });
  }, [sendCommand]);

  const activeDevice = useMemo(
    () => devices.find((d) => d.mac === activeMac) ?? null,
    [devices, activeMac]
  );

  const value: PoolContextValue = {
    relays, pumps, presets, status, deviceLabel, lastError,
    devices, activeMac, activeDevice, transport, bleUp,
    deviceFw, otaPerform,
    selectDevice, renameDevice, removeDevice,
    wifi, wifiNetworks, wifiScanning, scanWifiNetworks, provisionWifi, forgetWifi, refreshState,
    connectWithQrData, disconnect, forceReset,
    setRelayMode: setRelayModeFn,
    setRelayName: setRelayNameFn,
    setPumpMode: setPumpModeFn,
    setPumpConfig: setPumpConfigFn,
    addSchedule: addScheduleFn,
    updateSchedule: updateScheduleFn,
    deleteSchedule: deleteScheduleFn,
    toggleSchedule: toggleScheduleFn,
    r2Depends,
    setRelay2Dependency: setRelay2DependencyFn,
  };

  return <PoolContext.Provider value={value}>{children}</PoolContext.Provider>;
}

// ============================================================
//  SMALL REUSABLE UI PIECES
// ============================================================
function Card({ children, style }: { children: React.ReactNode; style?: any }) {
  return <View style={[styles.card, style]}>{children}</View>;
}

function Pill({ on }: { on: boolean }) {
  return (
    <View style={[stylesPill.pill, on ? stylesPill.on : stylesPill.off]}>
      <Text style={[stylesPill.txt, on ? stylesPill.txtOn : stylesPill.txtOff]}>
        {on ? 'ON' : 'OFF'}
      </Text>
    </View>
  );
}
const stylesPill = StyleSheet.create({
  pill: { paddingHorizontal: 13, paddingVertical: 4, borderRadius: 20, borderWidth: 1 },
  on: { backgroundColor: COLORS.onSoft, borderColor: 'rgba(5,150,105,0.5)' },
  off: { backgroundColor: 'rgba(15,23,42,0.05)', borderColor: 'rgba(15,23,42,0.12)' },
  txt: { fontSize: 12, fontWeight: '700', letterSpacing: 1 },
  txtOn: { color: '#047857' },
  txtOff: { color: COLORS.label },
});

function Segmented({ value, onChange }: { value: RelayMode; onChange: (m: RelayMode) => void }) {
  const opts: { key: RelayMode; label: string }[] = [
    { key: 'auto', label: 'Auto' },
    { key: 'on', label: 'On' },
    { key: 'off', label: 'Off' },
  ];
  return (
    <View style={styles.seg}>
      {opts.map((o) => {
        const active = value === o.key;
        return (
          <TouchableOpacity key={o.key} onPress={() => onChange(o.key)} style={[styles.segBtn, active && styles.segBtnActive]}>
            <Text style={[styles.segTxt, active && styles.segTxtActive]}>{o.label}</Text>
          </TouchableOpacity>
        );
      })}
    </View>
  );
}

// ============================================================
//  WHEEL PICKER — iOS Clock-style snap wheel. Pure ScrollView, no
//  native dependency, so it looks identical on Android and iOS.
// ============================================================
const WHEEL_ITEM_H = 36;
const WHEEL_ROWS = 5; // visible rows — odd, so one row sits dead center
function WheelPicker({ items, index, onChange, width, rows = WHEEL_ROWS }: {
  items: string[]; index: number; onChange: (i: number) => void; width: number; rows?: number;
}) {
  const ref = useRef<ScrollView | null>(null);
  const lastEmitted = useRef(index);
  const pad = ((rows - 1) / 2) * WHEEL_ITEM_H;
  // The row currently sitting in the center band, tracked LIVE while the
  // wheel scrolls — the bold highlight follows the finger, not just the
  // final settled value.
  const [centerIdx, setCenterIdx] = useState(index);

  // Initial position (the contentOffset prop is unreliable on Android).
  useEffect(() => {
    requestAnimationFrame(() => ref.current?.scrollTo({ y: lastEmitted.current * WHEEL_ITEM_H, animated: false }));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // External form changes (opening the editor on an existing schedule)
  // move the wheel; values the wheel itself reported are not re-applied.
  useEffect(() => {
    if (index !== lastEmitted.current) {
      lastEmitted.current = index;
      setCenterIdx(index);
      ref.current?.scrollTo({ y: index * WHEEL_ITEM_H, animated: false });
    }
  }, [index]);

  const settle = (y: number) => {
    let i = Math.round(y / WHEEL_ITEM_H);
    i = Math.max(0, Math.min(items.length - 1, i));
    if (i !== lastEmitted.current) {
      lastEmitted.current = i;
      onChange(i);
    }
  };

  return (
    <View style={{ width, height: WHEEL_ITEM_H * rows }}>
      <View pointerEvents="none" style={{ position: 'absolute', top: pad, left: 2, right: 2, height: WHEEL_ITEM_H, borderRadius: 8, backgroundColor: 'rgba(15,23,42,0.05)' }} />
      <ScrollView
        ref={ref}
        showsVerticalScrollIndicator={false}
        snapToInterval={WHEEL_ITEM_H}
        decelerationRate="fast"
        nestedScrollEnabled
        scrollEventThrottle={16}
        onScroll={(e) => {
          let i = Math.round(e.nativeEvent.contentOffset.y / WHEEL_ITEM_H);
          i = Math.max(0, Math.min(items.length - 1, i));
          setCenterIdx(i);
        }}
        onMomentumScrollEnd={(e) => settle(e.nativeEvent.contentOffset.y)}
        contentContainerStyle={{ paddingVertical: pad }}
      >
        {items.map((it, i) => (
          <View key={i} style={{ height: WHEEL_ITEM_H, alignItems: 'center', justifyContent: 'center' }}>
            <Text style={{ fontSize: 20, fontWeight: i === centerIdx ? '700' : '400', color: i === centerIdx ? COLORS.text : '#b6bcc8' }}>
              {it}
            </Text>
          </View>
        ))}
      </ScrollView>
    </View>
  );
}
const WHEEL_H12 = Array.from({ length: 12 }, (_, i) => String(i + 1));
const WHEEL_M60 = Array.from({ length: 60 }, (_, i) => String(i).padStart(2, '0'));
const WHEEL_H24 = Array.from({ length: 24 }, (_, i) => String(i));

// ============================================================
//  SLIDE-UP MODAL — the native "slide" animation moves the dimmed
//  backdrop up together with the sheet, which reads as a glitch. Here
//  the backdrop FADES while the content slides on an eased curve, and
//  the modal unmounts only after the closing animation finishes.
// ============================================================
// How many native modals are currently presented. Programmatic popups
// (the firmware-update prompt) defer while this is non-zero: presenting a
// second Modal over one that is opening/closing gets silently dropped by
// the OS and can wedge an invisible, touch-eating layer over the app.
const modalDepth = { current: 0 };

function SlideUpModal({ visible, onClose, children, overlayStyle, distance = 640, closeOnBackdrop = true }: {
  visible: boolean;
  onClose: () => void;
  children: React.ReactNode;
  overlayStyle?: object;
  distance?: number;
  closeOnBackdrop?: boolean;
}) {
  const [mounted, setMounted] = useState(visible);
  const anim = useRef(new Animated.Value(0)).current;

  useEffect(() => {
    if (visible) {
      setMounted(true);
      Animated.timing(anim, { toValue: 1, duration: 280, easing: Easing.out(Easing.cubic), useNativeDriver: true }).start();
    } else {
      Animated.timing(anim, { toValue: 0, duration: 220, easing: Easing.in(Easing.cubic), useNativeDriver: true }).start(({ finished }) => {
        if (finished) setMounted(false);
      });
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [visible]);

  // Watchdog: if the closing animation's completion callback is ever lost
  // (interrupted animation, re-render storm at close time), the modal MUST
  // still dismiss — a lingering transparent Modal eats every touch.
  useEffect(() => {
    if (visible || !mounted) return;
    const t = setTimeout(() => setMounted(false), 400);
    return () => clearTimeout(t);
  }, [visible, mounted]);

  useEffect(() => {
    if (!mounted) return;
    modalDepth.current += 1;
    return () => { modalDepth.current -= 1; };
  }, [mounted]);

  // The Modal element stays in the tree and dismisses via its `visible`
  // prop — the reliable native path. Unmounting a presented Modal (the
  // old approach) intermittently leaked a touch-blocking layer on Android.
  return (
    <Modal visible={mounted} transparent animationType="none" onRequestClose={onClose}>
      <Animated.View style={[StyleSheet.absoluteFill, { backgroundColor: 'rgba(15,23,42,0.5)', opacity: anim }]}>
        <TouchableOpacity style={{ flex: 1 }} activeOpacity={1} onPress={closeOnBackdrop ? onClose : undefined} />
      </Animated.View>
      <Animated.View
        pointerEvents="box-none"
        style={[
          { flex: 1, justifyContent: 'flex-end' },
          overlayStyle,
          { transform: [{ translateY: anim.interpolate({ inputRange: [0, 1], outputRange: [distance, 0] }) }] },
        ]}
      >
        {children}
      </Animated.View>
    </Modal>
  );
}

// SPA LITE and POOL LITE describe lighting circuits — the firmware only
// allows them on relays, so the pump picker hides them (as the web UI did).
const RELAY_ONLY_NAMES = ['SPA LITE', 'POOL LITE'];

function NamePicker({ value, onChange, extraOptionsDisabled, forPump, defaultLabel }: { value: string; onChange: (v: string) => void; extraOptionsDisabled?: string[]; forPump?: boolean; defaultLabel: string }) {
  const [open, setOpen] = useState(false);
  const options = forPump ? NAME_OPTIONS.filter((n) => !RELAY_ONLY_NAMES.includes(n)) : NAME_OPTIONS;
  return (
    <>
      <TouchableOpacity style={styles.fc} onPress={() => setOpen(true)}>
        <Text style={{ color: COLORS.text }}>{value}</Text>
      </TouchableOpacity>
      <SlideUpModal visible={open} onClose={() => setOpen(false)}>
        <View style={styles.pickerSheet}>
            <Text style={styles.modalTitle}>Choose a name</Text>
            <ScrollView style={{ maxHeight: 320 }}>
              {/* The slot's own label ("Relay 1", "Pump 2") replaces the old
                  generic "Default" option, and reads as selected when the
                  slot carries no custom name. */}
              <PickerRow label={defaultLabel} selected={value === defaultLabel} onPress={() => { onChange(defaultLabel); setOpen(false); }} />
              {options.map((n) => {
                const disabled = extraOptionsDisabled?.includes(n);
                return (
                  <PickerRow key={n} label={n} selected={value === n} disabled={disabled}
                    onPress={() => { if (!disabled) { onChange(n); setOpen(false); } }} />
                );
              })}
            </ScrollView>
        </View>
      </SlideUpModal>
    </>
  );
}
function PickerRow({ label, selected, disabled, onPress }: { label: string; selected: boolean; disabled?: boolean; onPress: () => void }) {
  return (
    <TouchableOpacity onPress={onPress} disabled={disabled} style={styles.pickerRow}>
      <Text style={{ color: disabled ? '#c4c9d4' : selected ? COLORS.on : COLORS.text, fontWeight: selected ? '700' : '400' }}>
        {label}{disabled ? '  (taken)' : ''}
      </Text>
    </TouchableOpacity>
  );
}

// Dropdown for the pump protocol. Black & Decker is offered only where
// allowBlackDecker is set — Pump 1 and schedules targeting Pump 1 — since
// that pump is fixed at RS-485 address 1.
function PumpTypePicker({ value, onChange, allowBlackDecker }: { value: PumpType; onChange: (v: PumpType) => void; allowBlackDecker?: boolean }) {
  const [open, setOpen] = useState(false);
  const options: PumpType[] = allowBlackDecker ? ['Pentair', 'Emaux', 'Black & Decker'] : ['Pentair', 'Emaux'];
  return (
    <>
      <TouchableOpacity style={styles.fc} onPress={() => setOpen(true)}>
        <Text style={{ color: COLORS.text }}>{value}  ▾</Text>
      </TouchableOpacity>
      <SlideUpModal visible={open} onClose={() => setOpen(false)}>
        <View style={styles.pickerSheet}>
            <Text style={styles.modalTitle}>Pump type</Text>
            {options.map((t) => (
              <PickerRow key={t} label={t} selected={value === t} onPress={() => { onChange(t); setOpen(false); }} />
            ))}
        </View>
      </SlideUpModal>
    </>
  );
}

// ============================================================
//  HOME TAB — replica of web_ui.h / INDEX_HTML, backed by PoolContext
// ============================================================
function HomeScreen() {
  const {
    relays, pumps, presets, status, deviceLabel, activeDevice,
    setRelayMode, setRelayName, setPumpMode, setPumpConfig,
    addSchedule, updateSchedule, deleteSchedule, toggleSchedule,
    r2Depends, setRelay2Dependency,
  } = usePool();

  const [now, setNow] = useState(new Date());
  const [modalOpen, setModalOpen] = useState(false);
  const [editingId, setEditingId] = useState<string | null>(null);

  const blink = useRef(new Animated.Value(1)).current;
  useEffect(() => {
    const loop = Animated.loop(
      Animated.sequence([
        Animated.timing(blink, { toValue: 0.35, duration: 1200, useNativeDriver: true }),
        Animated.timing(blink, { toValue: 1, duration: 1200, useNativeDriver: true }),
      ])
    );
    loop.start();
    return () => loop.stop();
  }, [blink]);

  useEffect(() => {
    const t = setInterval(() => setNow(new Date()), 1000);
    return () => clearInterval(t);
  }, []);

  const clockStr = useMemo(() => {
    let h = now.getHours();
    const m = now.getMinutes();
    const s = now.getSeconds();
    const ampm = h >= 12 ? 'PM' : 'AM';
    h = h % 12 || 12;
    const pad = (n: number) => n.toString().padStart(2, '0');
    return `${pad(h)}:${pad(m)}:${pad(s)} ${ampm}`;
  }, [now]);

  const usedNames = useMemo(
    () => [...relays.map((r) => r.name), ...pumps.map((p) => p.name)].filter((n) => n !== 'Default' && !/^Relay \d$/.test(n) && !/^Pump \d$/.test(n)),

    [relays, pumps]
  );

  // Schedules address targets by their canonical slot ("Relay 1", "Pump 2"),
  // but everywhere the user SEES a target it wears its assigned name.
  const targetName = (t: string) => {
    const rm = t.match(/^Relay (\d)$/);
    if (rm) return relays.find((r) => r.id === Number(rm[1]))?.name || t;
    const pm = t.match(/^Pump (\d)$/);
    if (pm) {
      const p = pumps.find((pp) => pp.id === Number(pm[1]));
      return p && !/^(Pump |PUMP)\d$/.test(p.name) ? p.name : t;
    }
    return t;
  };
  const [targetOpen, setTargetOpen] = useState(false);

  // Per-pump inline picker drafts (mirrors web_ui.h's pumpPickerOpen).
  // Tapping "On" only opens the picker — NOTHING is sent to the device
  // until "Turn on", because the pump needs a validated speed before it
  // can be told to run. Present in the map = picker open for that pump.
  const [pumpPicker, setPumpPicker] = useState<Record<number, { pumpType: PumpType; speed: string }>>({});

  // Pumps whose "Turn on" was tapped but whose RS-485 acknowledgment
  // hasn't come back yet (id -> tap timestamp). While pending, the Turn
  // on button turns amber — "command sent, waiting for the pump".
  // Cleared when the device confirms (active flips true) or after 20s.
  const [pumpPending, setPumpPending] = useState<Record<number, number>>({});
  useEffect(() => {
    const check = () => {
      setPumpPending((prev) => {
        const ids = Object.keys(prev);
        if (ids.length === 0) return prev;
        const next: Record<number, number> = { ...prev };
        let changed = false;
        for (const idStr of ids) {
          const id = Number(idStr);
          const pump = pumps.find((pp) => pp.id === id);
          // Black & Decker pending is cleared by its own fixed 1s timer
          // (set in turnOn), not by confirmation — its pill flips
          // optimistically, which would otherwise cancel the flash early.
          if (pump?.pumpType === 'Black & Decker') continue;
          const elapsed = Date.now() - prev[id];
          // Clear on confirmation, but never before 1s so the
          // "Turning on…" flash stays visible; give up after 20s.
          if ((pump?.active && elapsed >= 1000) || elapsed > 20000) {
            delete next[id];
            changed = true;
          }
        }
        return changed ? next : prev;
      });
    };
    check();
    // A confirmation landing inside the first second is skipped by the
    // check above — re-check shortly after so it still clears at ~1s.
    const t = setTimeout(check, 1100);
    return () => clearTimeout(t);
    // Runs on every state push (2-5s cadence), which is also often enough
    // to enforce the 20s give-up without a dedicated timer.
  }, [pumps]);

  const emptyForm = {
    target: 'Relay 1',
    pumpType: 'Pentair' as PumpType,
    speed: '2000',
    hour: '6',
    minute: '30',
    ampm: 'AM' as 'AM' | 'PM',
    durH: '1',
    durM: '0',
    enabled: true,
  };
  const [form, setForm] = useState(emptyForm);

  // RPM wheel for the schedule editor: 10-rpm steps across the selected
  // pump family's valid envelope (same limits the firmware enforces).
  const rpmRange = form.pumpType === 'Pentair'
    ? { min: 450, max: 3450 }
    : form.pumpType === 'Black & Decker'
      ? { min: 600, max: 3450 }
      : { min: 800, max: 3400 };
  const rpmItems = useMemo(() => {
    const arr: string[] = [];
    for (let v = rpmRange.min; v <= rpmRange.max; v += 10) arr.push(String(v));
    return arr;
  }, [rpmRange.min, rpmRange.max]);
  const rpmIndex = Math.max(0, Math.min(rpmItems.length - 1,
    Math.round(((parseInt(form.speed, 10) || rpmRange.min) - rpmRange.min) / 10)));

  const openAdd = () => { setEditingId(null); setForm(emptyForm); setModalOpen(true); };
  const openEdit = (p: Preset) => {
    setEditingId(p.id);
    const { hour, ampm } = to12Hour(p.hour24);
    setForm({
      target: p.targetLabel,
      pumpType: p.pumpType,
      speed: String(p.speedRpm || 2000),
      hour: String(hour),
      minute: String(p.minute).padStart(2, '0'),
      ampm,
      durH: String(Math.floor(p.durationMin / 60)),
      durM: String(p.durationMin % 60).padStart(2, '0'),
      enabled: p.enabled,
    });
    setModalOpen(true);
  };

  const savePreset = () => {
    const hour24 = to24Hour(parseInt(form.hour, 10) || 12, form.ampm);
    const minute = parseInt(form.minute, 10) || 0;
    const durationMin = (parseInt(form.durH, 10) || 0) * 60 + (parseInt(form.durM, 10) || 0);
    // Speed/type belong to THE SCHEDULE (each preset runs its pump at its
    // own speed, as the web preset editor did) — not to the pump's manual
    // override config, which is what the old setPumpConfig call here set.
    const input: ScheduleInput = {
      target: form.target,
      hour24,
      minute,
      durationMin,
      enabled: form.enabled,
      speedRpm: parseInt(form.speed, 10) || 2000,
      pumpType: form.pumpType,
    };

    if (editingId) updateSchedule(editingId, input);
    else addSchedule(input);
    setModalOpen(false);
  };

  const isPumpTarget = form.target.startsWith('Pump');

  return (
    <ScrollView style={styles.screen} contentContainerStyle={{ paddingBottom: 32 }}>
      {/* ---- Header ---- */}
      <Card style={styles.headerCard}>
        <View style={styles.chip}>
          <Animated.View style={[styles.statusDot, { opacity: blink, backgroundColor: status === 'connected' ? COLORS.on : COLORS.label }]} />
          <Text style={styles.chipTxt}>
            {/* The user-assigned registry name, live through renames — the
                raw advertised-name snapshot was stale after a rename and
                literally "null" on Wi-Fi-only connects. */}
            {status === 'connected' ? `Connected · ${activeDevice?.name || deviceLabel || 'device'}`
              : status === 'authenticating' ? 'Verifying device…'
              : status === 'connecting' ? 'Connecting…'
              : 'Not connected (demo mode)'}
          </Text>
        </View>
        <Text style={styles.clock}>{clockStr}</Text>
        <Text style={styles.clockSub}>RELAY & PUMP TIMER</Text>
      </Card>

      {/* ---- Relay cards ---- */}
      {relays.map((r) => (
        <Card key={r.id} style={[styles.rCard, r.active && { borderColor: 'rgba(16,185,129,0.35)' }]}>
          <View style={styles.rTop}>
            <Text style={styles.rTag}>{r.tag}</Text>
            <Pill on={r.active} />
          </View>
          <Text style={[styles.rName, { color: '#000' }]}>{r.name}</Text>
          <View style={{ marginVertical: 6 }}>
            <NamePicker
              value={r.name}
              defaultLabel={`Relay ${r.id}`}
              onChange={(v) => setRelayName(r.id, v)}
              extraOptionsDisabled={usedNames}
            />
          </View>
          <View style={styles.rCd}>
            <Text style={{ color: COLORS.muted, fontSize: 13 }}>{r.countdown}</Text>
          </View>
          {r.id === 2 && (
            <View style={[styles.rTop, { marginBottom: 6 }]}>
              <Text style={{ color: COLORS.muted, fontSize: 13 }}>Depends on Relay 1</Text>
              <Switch value={r2Depends} onValueChange={setRelay2Dependency} />
            </View>
          )}
          <Segmented value={r.mode} onChange={(m) => setRelayMode(r.id, m)} />
        </Card>
      ))}

      {/* ---- Pumps ---- */}
      <Card>
        <Text style={styles.secTitle}>PUMPS — MANUAL CONTROL</Text>
        {pumps.map((p) => {
          const draft = pumpPicker[p.id];
          const pending = !!pumpPending[p.id];
          const seedSpeed = p.setSpeedRpm || p.speedRpm;
          const shownType = draft ? draft.pumpType : p.pumpType;
          const shownSpeed = draft ? draft.speed : String(seedSpeed || '');
          const openDraft = (patch?: Partial<{ pumpType: PumpType; speed: string }>) =>
            setPumpPicker((prev) => {
              const base = prev[p.id] ?? { pumpType: p.pumpType, speed: seedSpeed ? String(seedSpeed) : '' };
              return { ...prev, [p.id]: { ...base, ...patch } };
            });
          const closeDraft = () =>
            setPumpPicker((prev) => {
              const next = { ...prev };
              delete next[p.id];
              return next;
            });
          const turnOn = () => {
            const speed = parseInt(shownSpeed, 10) || 0;
            const minRpm = shownType === 'Pentair' ? 450 : shownType === 'Black & Decker' ? 600 : 800;
            const maxRpm = shownType === 'Emaux' ? 3400 : 3450;
            if (speed < minRpm || speed > maxRpm) {
              // Same limits the firmware enforces — catch it before the round-trip.
              Alert.alert('Pump speed', `Enter ${minRpm}–${maxRpm} RPM for ${shownType} pumps.`);
              return;
            }
            // Two commands, matching the firmware's split: parameters
            // first, then the mode flip that makes the dispatcher drive
            // the pump over RS-485 with them. The panel deliberately stays
            // OPEN with the entered values — only Cancel hides it, so the
            // RPM you set never vanishes from view.
            setPumpConfig(p.id, { pumpType: shownType, speedRpm: speed });
            setPumpMode(p.id, 'on');
            setPumpPending((prev) => ({ ...prev, [p.id]: Date.now() }));
            if (shownType === 'Black & Decker') {
              // Fire-and-forget: no acknowledgment will ever come. Show
              // "Turning on…" for a fixed 1s as tap feedback, then the
              // button returns to normal; the pill has already flipped ON
              // optimistically via setPumpMode ("sent" semantics).
              setTimeout(() => {
                setPumpPending((prev) => {
                  if (!(p.id in prev)) return prev;
                  const next = { ...prev };
                  delete next[p.id];
                  return next;
                });
              }, 1000);
            }
            // Other families: the amber state holds until the pump
            // acknowledges (cleared by the effect above), or 20s.
          };
          return (
            <View key={p.id} style={styles.pumpItem}>
              <View style={styles.rTop}>
                <Text style={styles.rTag}>PUMP {p.id}</Text>
                {/* Pill and RPM reflect CONFIRMED state only: `active` is
                    true once the pump acknowledges over RS-485, never
                    merely because "On" was requested. */}
                <Pill on={p.active} />
              </View>
              <Text style={[styles.rName, { color: '#000' }]}>{p.name}</Text>
              <View style={{ marginVertical: 6 }}>
                <NamePicker
                  value={p.name.match(/^(Pump |PUMP)\d$/) ? `Pump ${p.id}` : p.name}
                  defaultLabel={`Pump ${p.id}`}
                  onChange={(v) => setPumpConfig(p.id, { name: v })}
                  extraOptionsDisabled={usedNames}
                  forPump
                />
              </View>
              <Text style={styles.pumpActual}>{p.active ? `${p.speedRpm} RPM` : '-- RPM'}</Text>
              <View style={styles.rCd}>
                <Text style={{ color: COLORS.muted, fontSize: 13 }}>{p.countdown}</Text>
              </View>
              <Segmented
                value={draft ? 'on' : p.mode}
                onChange={(m) => {
                  if (m === 'on') {
                    openDraft(); // picker only — nothing is sent until "Turn on"
                    return;
                  }
                  closeDraft();
                  // Leaving "on" cancels any waiting-for-ack state.
                  setPumpPending((prev) => {
                    if (!(p.id in prev)) return prev;
                    const next = { ...prev };
                    delete next[p.id];
                    return next;
                  });
                  setPumpMode(p.id, m);
                }}
              />
              {/* Visible only while a draft is open: Cancel closes it (even
                  with the pump running), tapping "On" reopens it. */}
              {draft && (
                <View style={styles.pumpInline}>
                  <View style={{ flexDirection: 'row', gap: 10 }}>
                    <View style={{ flex: 1 }}>
                      <PumpTypePicker
                        value={shownType}
                        onChange={(t) => openDraft({ pumpType: t })}
                        allowBlackDecker={p.id === 1}
                      />
                    </View>
                    <TextInput
                      style={[styles.fc, { flex: 1 }]}
                      keyboardType="numeric"
                      placeholder="Speed (RPM)"
                      value={shownSpeed}
                      onFocus={() => openDraft()}
                      onChangeText={(t) => openDraft({ speed: t.replace(/[^0-9]/g, '') })}
                    />
                  </View>
                  <View style={{ flexDirection: 'row', gap: 10, marginTop: 10 }}>
                    <TouchableOpacity style={[styles.btnSec, { flex: 1 }]} onPress={closeDraft}>
                      <Text style={styles.btnSecTxt}>Cancel</Text>
                    </TouchableOpacity>
                    <TouchableOpacity
                      style={[styles.btnPrimary, { flex: 1 }, pending && { backgroundColor: '#d97706' }]}
                      onPress={turnOn}
                      disabled={pending}
                    >
                      <Text style={styles.btnPrimaryTxt}>{pending ? 'Turning on…' : 'Turn on'}</Text>
                    </TouchableOpacity>
                  </View>
                </View>
              )}
            </View>
          );
        })}
      </Card>

      {/* ---- Timer Schedules ---- */}
      <Card>
        <View style={styles.presetHeader}>
          <Text style={[styles.secTitle, { marginBottom: 0 }]}>TIMER SCHEDULES</Text>
          <View style={styles.presetCount}>
            <Text style={{ fontSize: 13, color: COLORS.label }}>{presets.length}/8</Text>
          </View>
        </View>

        {presets.length === 0 && <Text style={{ color: COLORS.muted, paddingVertical: 12 }}>No schedules yet.</Text>}

        {presets.map((p) => (
          <View key={p.id} style={[styles.pItem, !p.enabled && { opacity: 0.45 }]}>
            <Switch value={p.enabled} onValueChange={(v) => toggleSchedule(p.id, v)} />
            <View style={{ flex: 1, marginLeft: 12 }}>
              <Text style={{ fontWeight: '700' }}>{p.timeStr} · +{p.durationStr}</Text>
              <Text style={{ color: COLORS.muted, fontSize: 13 }}>
                {targetName(p.targetLabel)}
                {p.targetLabel.startsWith('Pump') ? ` · ${p.pumpType} @ ${p.speedRpm} RPM` : ''}
              </Text>
            </View>
            <TouchableOpacity style={styles.pBtn} onPress={() => openEdit(p)}>
              <FigIcon name="pencil" size={22} color={COLORS.text} />
            </TouchableOpacity>
            <TouchableOpacity style={styles.pBtn} onPress={() => deleteSchedule(p.id)}>
              <FigIcon name="trash" size={22} color="#dc2626" />
            </TouchableOpacity>
          </View>
        ))}

        <TouchableOpacity style={styles.btnAdd} onPress={openAdd}>
          <Text style={styles.btnAddTxt}>＋  Create New Schedule</Text>
        </TouchableOpacity>
      </Card>

      {/* ---- Add/Edit Schedule modal ---- */}
      <SlideUpModal visible={modalOpen} onClose={() => setModalOpen(false)} closeOnBackdrop={false}>
        <ScrollView style={styles.modalCard} contentContainerStyle={{ paddingBottom: 20 }}>
            <Text style={styles.modalTitle}>{editingId ? 'Edit Schedule' : 'Setup Schedule'}</Text>

            <Text style={styles.fieldLabel}>Target Device</Text>
            <TouchableOpacity style={styles.fc} onPress={() => setTargetOpen(true)}>
              <Text style={{ color: COLORS.text }}>{targetName(form.target)}  ▾</Text>
            </TouchableOpacity>
            <Text style={styles.fhint}>Select the hardware device you want this timer to activate.</Text>

            <SlideUpModal visible={targetOpen} onClose={() => setTargetOpen(false)}>
              <View style={styles.pickerSheet}>
                <Text style={styles.modalTitle}>Target device</Text>
                  <ScrollView style={{ maxHeight: 320 }}>
                    {TARGET_OPTIONS.map((t) => (
                      <PickerRow
                        key={t}
                        label={targetName(t)}
                        selected={form.target === t}
                        onPress={() => {
                          setForm((f) => ({
                            ...f,
                            target: t,
                            // Black & Decker is Pump-1-only — moving the target off
                            // Pump 1 falls back to Pentair, matching the firmware rule.
                            pumpType: t !== 'Pump 1' && f.pumpType === 'Black & Decker' ? 'Pentair' : f.pumpType,
                          }));
                          setTargetOpen(false);
                        }}
                      />
                    ))}
                  </ScrollView>
              </View>
            </SlideUpModal>

            {isPumpTarget && (
              <>
                <View style={{ marginTop: 14 }}>
                  <PumpTypePicker
                    value={form.pumpType}
                    onChange={(t) => setForm((f) => {
                      // Snap the speed into the new family's envelope on a
                      // 10-rpm grid so the wheel and the value always agree.
                      const r = t === 'Pentair' ? { min: 450, max: 3450 }
                        : t === 'Black & Decker' ? { min: 600, max: 3450 }
                        : { min: 800, max: 3400 };
                      let sp = parseInt(f.speed, 10) || r.min;
                      sp = Math.max(r.min, Math.min(r.max, Math.round(sp / 10) * 10));
                      return { ...f, pumpType: t, speed: String(sp) };
                    })}
                    allowBlackDecker={form.target === 'Pump 1'}
                  />
                </View>
              </>
            )}

            {/* Start time + speed side by side, 3-row wheels — the whole
                editor fits on screen without scrolling. */}
            <View style={{ flexDirection: 'row', gap: 8 }}>
              <View style={{ flex: 1.5 }}>
                <Text style={styles.fieldLabel}>Start Time</Text>
                <View style={{ flexDirection: 'row', justifyContent: 'center', alignItems: 'center' }}>
                  <WheelPicker
                    width={44}
                    rows={5}
                    items={WHEEL_H12}
                    index={Math.max(0, Math.min(11, (parseInt(form.hour, 10) || 12) - 1))}
                    onChange={(i) => setForm((f) => ({ ...f, hour: WHEEL_H12[i] }))}
                  />
                  <Text style={{ fontSize: 18, fontWeight: '700', color: COLORS.text }}>:</Text>
                  <WheelPicker
                    width={44}
                    rows={5}
                    items={WHEEL_M60}
                    index={Math.max(0, Math.min(59, parseInt(form.minute, 10) || 0))}
                    onChange={(i) => setForm((f) => ({ ...f, minute: WHEEL_M60[i] }))}
                  />
                  <WheelPicker
                    width={48}
                    rows={5}
                    items={['AM', 'PM']}
                    index={form.ampm === 'PM' ? 1 : 0}
                    onChange={(i) => setForm((f) => ({ ...f, ampm: i === 1 ? 'PM' : 'AM' }))}
                  />
                </View>
              </View>
              {isPumpTarget && (
                <View style={{ flex: 1 }}>
                  <Text style={styles.fieldLabel}>Speed (RPM)</Text>
                  <View style={{ flexDirection: 'row', justifyContent: 'center', alignItems: 'center' }}>
                    <WheelPicker
                      width={74}
                      rows={5}
                      items={rpmItems}
                      index={rpmIndex}
                      onChange={(i) => setForm((f) => ({ ...f, speed: rpmItems[i] }))}
                    />
                  </View>
                </View>
              )}
            </View>

            <Text style={styles.fieldLabel}>Duration</Text>
            <View style={{ flexDirection: 'row', justifyContent: 'center', alignItems: 'center', gap: 6 }}>
              <WheelPicker
                width={56}
                rows={5}
                items={WHEEL_H24}
                index={Math.max(0, Math.min(23, parseInt(form.durH, 10) || 0))}
                onChange={(i) => setForm((f) => ({ ...f, durH: WHEEL_H24[i] }))}
              />
              <Text style={{ fontSize: 13, color: COLORS.muted }}>hours</Text>
              <WheelPicker
                width={56}
                rows={5}
                items={WHEEL_M60}
                index={Math.max(0, Math.min(59, parseInt(form.durM, 10) || 0))}
                onChange={(i) => setForm((f) => ({ ...f, durM: WHEEL_M60[i] }))}
              />
              <Text style={{ fontSize: 13, color: COLORS.muted }}>min</Text>
            </View>

            <View style={styles.rTop}>
              <Text style={styles.fieldLabel}>Enabled</Text>
              <Switch value={form.enabled} onValueChange={(v) => setForm((f) => ({ ...f, enabled: v }))} />
            </View>

            <View style={styles.modalActs}>
              <TouchableOpacity style={[styles.btnSec, { flex: 1 }]} onPress={() => setModalOpen(false)}>
                <Text style={styles.btnSecTxt}>Cancel</Text>
              </TouchableOpacity>
              <TouchableOpacity style={[styles.btnPrimary, { flex: 1 }]} onPress={savePreset}>
                <Text style={styles.btnPrimaryTxt}>Save Schedule</Text>
              </TouchableOpacity>
            </View>
        </ScrollView>
      </SlideUpModal>
    </ScrollView>
  );
}

// ============================================================
//  QR SCANNER MODAL  (expo-camera)
// ============================================================
function QrScannerModal({ visible, onClose, onScanned }: { visible: boolean; onClose: () => void; onScanned: (data: string) => void }) {
  const [permission, requestPermission] = useCameraPermissions();

  // A ref, NOT state. onBarcodeScanned fires many times per second while the
  // code is in frame, and setState is asynchronous — with a state flag, every
  // callback before the next render still sees `handled === false` and fires
  // onScanned again. That produced several concurrent connect attempts to the
  // same peripheral, which cancel each other ("operation was cancelled").
  // A ref updates synchronously, so the first frame wins.
  const handledRef = useRef(false);

  useEffect(() => {
    if (visible) {
      handledRef.current = false;
      if (!permission?.granted) requestPermission();
    }
  }, [visible]); // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (!visible) return;
    modalDepth.current += 1;
    return () => { modalDepth.current -= 1; };
  }, [visible]);

  const handleBarcode = (result: BarcodeScanningResult) => {
    if (handledRef.current) return;
    handledRef.current = true;
    onScanned(result.data);
  };

  return (
    <Modal visible={visible} animationType="slide" onRequestClose={onClose}>
      <View style={styles.scannerRoot}>
        {permission?.granted ? (
          <CameraView
            style={{ flex: 1 }}
            facing="back"
            barcodeScannerSettings={{ barcodeTypes: ['qr'] }}
            onBarcodeScanned={handleBarcode}
          >
            <View style={styles.scannerOverlay}>
              <View style={styles.scannerFrame} />
              <Text style={styles.scannerHint}>Point the camera at the QR code on your device</Text>
            </View>
          </CameraView>
        ) : (
          <View style={styles.scannerPermissionBox}>
            <Text style={{ color: '#fff', textAlign: 'center', marginBottom: 16 }}>
              Camera access is needed to scan the device&apos;s pairing QR code.
            </Text>
            <TouchableOpacity style={styles.btnPrimary} onPress={requestPermission}>
              <Text style={styles.btnPrimaryTxt}>Grant Camera Access</Text>
            </TouchableOpacity>
          </View>
        )}
        <TouchableOpacity style={styles.scannerClose} onPress={onClose}>
          <Text style={{ color: '#fff', fontWeight: '700' }}>Cancel</Text>
        </TouchableOpacity>
      </View>
    </Modal>
  );
}

// ============================================================
//  DEVICE TAB — QR pairing (primary) + manual BLE scan (fallback)
// ============================================================
// ============================================================
//  WI-FI SETUP MODAL
// ============================================================
// Provisioning is BLE-only by design: re-pointing a device at a new
// network over the network you're about to leave strands it.
function signalBars(rssi: number): string {
  if (rssi > -55) return '▂▄▆█';
  if (rssi > -67) return '▂▄▆';
  if (rssi > -78) return '▂▄';
  return '▂';
}

type WifiStep = 'pick' | 'password' | 'joining' | 'done' | 'failed';

function WifiSetupModal({ visible, onClose, notice }: { visible: boolean; onClose: () => void; notice?: string | null }) {
  const { wifi, wifiNetworks, wifiScanning, scanWifiNetworks, provisionWifi, forgetWifi } = usePool();
  const [step, setStep] = useState<WifiStep>('pick');
  const [selected, setSelected] = useState<WifiNetwork | null>(null);
  const [password, setPassword] = useState('');
  const [showPassword, setShowPassword] = useState(false);

  // Wi-Fi status pushes that PREDATE this join attempt — the device
  // reporting 'unconfigured' (or 'offline') from before it even received
  // the password — must not be judged as the outcome. Without this gate
  // the joining step read the last pre-join heartbeat and flipped to
  // "failed" while the serial monitor showed the device joining fine.
  // Armed once the status flips to 'connecting' for THIS attempt (set
  // locally by provisionWifi right after the credentials are written).
  const joinArmedRef = useRef(false);

  // Whether the device was ALREADY online when the sheet opened. In that
  // case this is the "manage my network" flow (opened from the Wi-Fi
  // button) — being online is the starting point, not a verdict to act on.
  const openedOnlineRef = useRef(false);

  useEffect(() => {
    if (visible) {
      setStep('pick');
      setSelected(null);
      setPassword('');
      setShowPassword(false);
      openedOnlineRef.current = wifi?.state === 'online';
      scanWifiNetworks();
    }
  }, [visible]); // eslint-disable-line react-hooks/exhaustive-deps

  // The modal stays open through the join and reports the outcome, rather
  // than closing optimistically and leaving the user to guess.
  useEffect(() => {
    if (step !== 'joining') return;
    if (wifi?.state === 'connecting') { joinArmedRef.current = true; return; }
    if (!joinArmedRef.current) return; // stale pre-join status — not a verdict
    if (wifi?.state === 'online') setStep('done');
    // 'offline' on its own is not a verdict either: Wi-Fi/BLE radio
    // sharing can drop the link for a moment right after it came up, and
    // the device retries by itself. Only a join the firmware itself gave
    // up on (joinFailed: wrong password or AP out of reach) is terminal;
    // the provider's 60s watchdog backstops the rest.
    else if (
      wifi?.state === 'unconfigured' ||
      (wifi?.state === 'offline' && wifi.joinFailed !== false)
    ) setStep('failed');
  }, [wifi?.state, wifi?.joinFailed, step]);

  // If the join never even arms (the setWifi write failed, so the status
  // never reached 'connecting'), nothing above will ever settle the step —
  // time out rather than spin forever.
  useEffect(() => {
    if (step !== 'joining') return;
    const timer = setTimeout(() => {
      if (!joinArmedRef.current) setStep('failed');
    }, 20000);
    return () => clearTimeout(timer);
  }, [step]);

  // The device CAME online while the user was still picking a network or
  // typing a password (its stored network or a background join attempt
  // succeeded). The question this popup asks has been answered — leave
  // immediately instead of letting the user re-provision a working device.
  // Deliberately a transition detector: when the sheet was OPENED on an
  // online device (managing/changing the network), staying online must
  // not slam the sheet shut in the same frame it appeared.
  useEffect(() => {
    if (!visible) return;
    if (step !== 'pick' && step !== 'password') return;
    if (wifi?.state !== 'online') {
      openedOnlineRef.current = false; // from here, reaching online again IS a fresh join
      return;
    }
    if (!openedOnlineRef.current) onClose();
  }, [visible, step, wifi?.state, onClose]);

  const submit = async () => {
    if (!selected) return;
    joinArmedRef.current = false;
    setStep('joining');
    await provisionWifi(selected.ssid, selected.secure ? password : '');
  };

  const canConnect = !!selected && (!selected.secure || password.length > 0);

  return (
    <SlideUpModal visible={visible} onClose={onClose}>
        <View style={[styles.modalCard, { minHeight: 440 }]}>

          {/* ---------- STEP 1: pick a network ---------- */}
          {step === 'pick' && (
            <>
              <View style={styles.rTop}>
                <Text style={styles.modalTitle}>Connect to Wi-Fi</Text>
                <TouchableOpacity onPress={scanWifiNetworks} disabled={wifiScanning}>
                  <Text style={{ color: wifiScanning ? COLORS.muted : COLORS.r1, fontWeight: '600' }}>
                    {wifiScanning ? 'Scanning…' : '⟳ Rescan'}
                  </Text>
                </TouchableOpacity>
              </View>
              {notice ? (
                <View style={[styles.noticeWarn, { marginTop: 10 }]}>
                  <Text style={styles.noticeTxt}>{notice}</Text>
                </View>
              ) : (
                <Text style={styles.fhint}>
                  Wi-Fi gives you whole-house range and keeps the timer&apos;s clock accurate.
                  Bluetooth keeps working as a fallback either way.
                </Text>
              )}

              {/* Currently connected network, with the forget action */}
              {wifi?.state === 'online' && wifi.ssid ? (
                <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, marginTop: 12, padding: 12, backgroundColor: 'rgba(34,197,94,0.08)', borderRadius: 12, borderWidth: 1, borderColor: 'rgba(34,197,94,0.25)' }}>
                  <FigIcon name="wifi" size={18} color="#22c55e" />
                  <View style={{ flex: 1, minWidth: 0 }}>
                    <Text style={{ fontSize: 14, fontWeight: '700', color: COLORS.text }} numberOfLines={1}>{wifi.ssid}</Text>
                    <Text style={{ fontSize: 12, color: '#22c55e', fontWeight: '600' }}>Connected</Text>
                  </View>
                  <TouchableOpacity onPress={() => { forgetWifi(); onClose(); }}>
                    <Text style={{ color: COLORS.danger, fontSize: 13, fontWeight: '600' }}>Forget</Text>
                  </TouchableOpacity>
                </View>
              ) : null}

              <ScrollView style={{ maxHeight: 290, marginVertical: 12 }}>
                {wifiScanning && wifiNetworks.length === 0 && (
                  <View style={{ alignItems: 'center', marginTop: 48 }}>
                    <ActivityIndicator color={COLORS.r1} />
                    <Text style={{ color: COLORS.muted, marginTop: 12 }}>Looking for networks…</Text>
                  </View>
                )}
                {!wifiScanning && wifiNetworks.length === 0 && (
                  <Text style={{ color: COLORS.muted, textAlign: 'center', marginTop: 48, paddingHorizontal: 20 }}>
                    No networks found. Move the controller closer to your router and rescan.
                  </Text>
                )}
                {wifiNetworks.map((net, i) => (
                  <TouchableOpacity
                    key={`${net.ssid}-${i}`}
                    style={styles.wifiRow}
                    onPress={() => {
                      setSelected(net);
                      if (!net.secure) joinArmedRef.current = false;
                      setStep(net.secure ? 'password' : 'joining');
                      if (!net.secure) provisionWifi(net.ssid, '');
                    }}
                  >
                    <Text style={styles.wifiIcon}>{net.secure ? '🔒' : '🌐'}</Text>
                    <Text style={styles.wifiSsid} numberOfLines={1}>{net.ssid}</Text>
                    <Text style={styles.wifiSignal}>{signalBars(net.rssi)}</Text>
                  </TouchableOpacity>
                ))}
              </ScrollView>

              <TouchableOpacity style={styles.btnSec} onPress={onClose}>
                <Text style={styles.btnSecTxt}>Skip for now</Text>
              </TouchableOpacity>
              <Text style={[styles.fhint, { textAlign: 'center' }]}>
                You can set this up later from the Device tab.
              </Text>
            </>
          )}

          {/* ---------- STEP 2: password ---------- */}
          {step === 'password' && selected && (
            <>
              <Text style={styles.modalTitle}>{selected.ssid}</Text>
              <Text style={styles.fieldLabel}>Password</Text>
              <View style={{ flexDirection: 'row', gap: 8 }}>
                <TextInput
                  style={[styles.fc, { flex: 1 }]}
                  placeholder="Wi-Fi password"
                  autoCapitalize="none"
                  autoCorrect={false}
                  autoFocus
                  secureTextEntry={!showPassword}
                  value={password}
                  onChangeText={setPassword}
                  onSubmitEditing={() => canConnect && submit()}
                />
                <TouchableOpacity
                  style={[styles.fc, { justifyContent: 'center' }]}
                  onPress={() => setShowPassword((v) => !v)}
                >
                  <Text>{showPassword ? '🙈' : '👁'}</Text>
                </TouchableOpacity>
              </View>
              <Text style={styles.fhint}>
                🔐 Encrypted with this session&apos;s key before it leaves your phone —
                it is never broadcast in the clear.
              </Text>

              <View style={styles.modalActs}>
                <TouchableOpacity style={[styles.btnSec, { flex: 1 }]} onPress={() => setStep('pick')}>
                  <Text style={styles.btnSecTxt}>Back</Text>
                </TouchableOpacity>
                <TouchableOpacity
                  style={[styles.btnPrimary, { flex: 1, opacity: canConnect ? 1 : 0.4 }]}
                  onPress={submit}
                  disabled={!canConnect}
                >
                  <Text style={styles.btnPrimaryTxt}>Connect</Text>
                </TouchableOpacity>
              </View>
            </>
          )}

          {/* ---------- STEP 3: joining ---------- */}
          {step === 'joining' && (
            <View style={styles.wifiCentre}>
              <ActivityIndicator size="large" color={COLORS.r1} />
              <Text style={styles.wifiBigTitle}>Joining {selected?.ssid}…</Text>
              <Text style={[styles.fhint, { textAlign: 'center' }]}>
                The controller is connecting to your network. This usually takes a few seconds.
              </Text>
              <TouchableOpacity style={[styles.btnSec, { marginTop: 24, alignSelf: 'stretch' }]} onPress={onClose}>
                <Text style={styles.btnSecTxt}>Continue in background</Text>
              </TouchableOpacity>
            </View>
          )}

          {/* ---------- STEP 4: success ---------- */}
          {step === 'done' && (
            <View style={styles.wifiCentre}>
              <Text style={{ fontSize: 52 }}>✅</Text>
              <Text style={styles.wifiBigTitle}>Connected to {wifi?.ssid}</Text>
              <Text style={[styles.fhint, { textAlign: 'center' }]}>
                Address {wifi?.ip}. The app will now use Wi-Fi and fall back to
                Bluetooth automatically if the network drops.
              </Text>
              <TouchableOpacity style={[styles.btnPrimary, { marginTop: 24, alignSelf: 'stretch' }]} onPress={onClose}>
                <Text style={styles.btnPrimaryTxt}>Done</Text>
              </TouchableOpacity>
            </View>
          )}

          {/* ---------- STEP 5: failure ---------- */}
          {step === 'failed' && (
            <View style={styles.wifiCentre}>
              <Text style={{ fontSize: 52 }}>📡</Text>
              <Text style={styles.wifiBigTitle}>Couldn&apos;t join {selected?.ssid}</Text>
              <Text style={[styles.fhint, { textAlign: 'center' }]}>
                The most likely cause is a wrong password. The controller also has to be
                on 2.4GHz — it cannot see 5GHz-only networks.
              </Text>
              <View style={[styles.modalActs, { alignSelf: 'stretch' }]}>
                <TouchableOpacity style={[styles.btnSec, { flex: 1 }]} onPress={onClose}>
                  <Text style={styles.btnSecTxt}>Skip for now</Text>
                </TouchableOpacity>
                <TouchableOpacity
                  style={[styles.btnPrimary, { flex: 1 }]}
                  onPress={() => { setPassword(''); setStep(selected?.secure ? 'password' : 'pick'); }}
                >
                  <Text style={styles.btnPrimaryTxt}>Try again</Text>
                </TouchableOpacity>
              </View>
            </View>
          )}
        </View>
    </SlideUpModal>
  );
}

// ============================================================
//  DEVICE SWITCHER  (top-left of the Device tab)
// ============================================================

// Palette + shared fragments for the Device tab's Figma design
// (https://pun-vast-23582148.figma.site/ — replicated 1:1).
const FIG = {
  green: '#22c55e', text: '#111827', gray: '#4b5563', muted: '#9ca3af',
  bg: '#f9fafb', border: '#e5e7eb', white: '#ffffff', blue: '#3b82f6', red: '#ef4444',
} as const;
const figCard = {
  backgroundColor: FIG.white, borderRadius: 16, borderWidth: 1, borderColor: FIG.border,
  paddingVertical: 18, paddingHorizontal: 16, marginBottom: 16,
  shadowColor: '#000', shadowOpacity: 0.07, shadowRadius: 4, shadowOffset: { width: 0, height: 1 }, elevation: 2,
} as const;
const figTile = {
  flex: 1, backgroundColor: FIG.bg, borderRadius: 12,
  paddingVertical: 12, paddingHorizontal: 12, borderWidth: 1, borderColor: FIG.border,
} as const;
const figSection = {
  fontSize: 11, fontWeight: '600', color: FIG.muted, letterSpacing: 1,
  textTransform: 'uppercase', marginTop: 6, marginBottom: 8,
} as const;

// Line icons with the exact stroke paths the Figma design uses.
type FigIconName =
  | 'monitor' | 'chevronDown' | 'wifi' | 'wifiOff' | 'bluetooth'
  | 'qr' | 'trash' | 'check' | 'pencil' | 'x' | 'plus' | 'home' | 'user'
  | 'gear' | 'refresh' | 'chevronRight' | 'info' | 'download';
function FigIcon({ name, size = 18, color = FIG.text, strokeWidth }: { name: FigIconName; size?: number; color?: string; strokeWidth?: number }) {
  const p = (d: string, sw = 1.8) => (
    <SvgPath d={d} stroke={color} strokeWidth={strokeWidth ?? sw} strokeLinecap="round" strokeLinejoin="round" fill="none" />
  );
  return (
    <Svg width={size} height={size} viewBox="0 0 24 24" fill="none">
      {name === 'monitor' && (
        <>
          <SvgRect x={2} y={3} width={20} height={14} rx={2} stroke={color} strokeWidth={1.8} fill="none" />
          {p('M8 21h8M12 17v4')}
        </>
      )}
      {name === 'chevronDown' && p('M6 9l6 6 6-6', 2)}
      {name === 'wifi' && p('M5 12.55a11 11 0 0114.08 0M1.42 9a16 16 0 0121.16 0M8.53 16.11a6 6 0 016.95 0M12 20h.01', 2)}
      {name === 'wifiOff' && (
        <>
          {p('M1 1l22 22')}
          {p('M16.72 11.06A10.94 10.94 0 0119 12.55M5 12.55a10.94 10.94 0 015.17-2.8M10.71 5.05A16 16 0 0122.56 9M1.42 9a15.91 15.91 0 014.7-2.88M8.53 16.11a6 6 0 016.95 0M12 20h.01')}
        </>
      )}
      {name === 'bluetooth' && p('M6.5 6.5l11 11L12 23V1l5.5 5.5-11 11')}
      {name === 'qr' && (
        <>
          <SvgRect x={3} y={3} width={8} height={8} rx={1} stroke={color} strokeWidth={1.8} fill="none" />
          <SvgRect x={13} y={3} width={8} height={8} rx={1} stroke={color} strokeWidth={1.8} fill="none" />
          <SvgRect x={3} y={13} width={8} height={8} rx={1} stroke={color} strokeWidth={1.8} fill="none" />
          {p('M13 13h2v2h-2zM17 13h4M13 17h2v4M19 17v4M19 13v2')}
        </>
      )}
      {name === 'trash' && (
        <>
          {p('M3 6h18M8 6V4h8v2M19 6l-1 14H6L5 6')}
          {p('M10 11v6M14 11v6')}
        </>
      )}
      {name === 'check' && p('M20 6L9 17l-5-5', 2.4)}
      {name === 'pencil' && (
        <>
          {p('M11 4H4a2 2 0 00-2 2v14a2 2 0 002 2h14a2 2 0 002-2v-7')}
          {p('M18.5 2.5a2.121 2.121 0 013 3L12 15l-4 1 1-4 9.5-9.5z')}
        </>
      )}
      {name === 'x' && p('M18 6L6 18M6 6l12 12', 2)}
      {name === 'plus' && p('M12 5v14M5 12h14', 2.2)}
      {name === 'home' && (
        <>
          {p('M3 9.5L12 3l9 6.5V20a1 1 0 01-1 1H4a1 1 0 01-1-1V9.5z')}
          {p('M9 21V12h6v9')}
        </>
      )}
      {name === 'user' && (
        <>
          <SvgCircle cx={12} cy={8} r={4} stroke={color} strokeWidth={1.8} fill="none" />
          {p('M4 20c0-4 3.58-7 8-7s8 3 8 7')}
        </>
      )}
      {name === 'gear' && (
        <>
          <SvgCircle cx={12} cy={12} r={3} stroke={color} strokeWidth={1.8} fill="none" />
          {p('M19.4 15a1.65 1.65 0 00.33 1.82l.06.06a2 2 0 010 2.83 2 2 0 01-2.83 0l-.06-.06a1.65 1.65 0 00-1.82-.33 1.65 1.65 0 00-1 1.51V21a2 2 0 01-4 0v-.09A1.65 1.65 0 009 19.4a1.65 1.65 0 00-1.82.33l-.06.06a2 2 0 01-2.83-2.83l.06-.06a1.65 1.65 0 00.33-1.82 1.65 1.65 0 00-1.51-1H3a2 2 0 010-4h.09A1.65 1.65 0 004.6 9a1.65 1.65 0 00-.33-1.82l-.06-.06a2 2 0 012.83-2.83l.06.06a1.65 1.65 0 001.82.33H9a1.65 1.65 0 001-1.51V3a2 2 0 014 0v.09a1.65 1.65 0 001 1.51 1.65 1.65 0 001.82-.33l.06-.06a2 2 0 012.83 2.83l-.06.06a1.65 1.65 0 00-.33 1.82V9a1.65 1.65 0 001.51 1H21a2 2 0 010 4h-.09a1.65 1.65 0 00-1.51 1z')}
        </>
      )}
      {name === 'refresh' && (
        <>
          {p('M23 4v6h-6M1 20v-6h6')}
          {p('M3.51 9a9 9 0 0114.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0020.49 15')}
        </>
      )}
      {name === 'chevronRight' && p('M9 18l6-6-6-6', 2)}
      {name === 'info' && (
        <>
          <SvgCircle cx={12} cy={12} r={10} stroke={color} strokeWidth={1.8} fill="none" />
          {p('M12 16v-4M12 8h.01', 2)}
        </>
      )}
      {name === 'download' && (
        <>
          {p('M21 15v4a2 2 0 01-2 2H5a2 2 0 01-2-2v-4')}
          {p('M7 10l5 5 5-5M12 15V3')}
        </>
      )}
    </Svg>
  );
}

function DeviceSwitcherModal({
  visible, onClose, onAddDevice,
}: { visible: boolean; onClose: () => void; onAddDevice: () => void }) {
  const { devices, activeMac, selectDevice, renameDevice } = usePool();
  const [renamingMac, setRenamingMac] = useState<string | null>(null);
  const [draftName, setDraftName] = useState('');

  const startRename = (d: DeviceRecord) => { setRenamingMac(d.mac); setDraftName(d.name); };
  const commitRename = async () => {
    if (renamingMac) await renameDevice(renamingMac, draftName);
    setRenamingMac(null);
  };

  return (
    <SlideUpModal visible={visible} onClose={onClose}>
        {/* Bottom sheet (figma "My Devices") */}
          <View
            style={{ backgroundColor: FIG.white, borderTopLeftRadius: 20, borderTopRightRadius: 20, paddingTop: 12, paddingHorizontal: 20, paddingBottom: 44 }}
          >
            <View style={{ width: 40, height: 4, backgroundColor: FIG.border, borderRadius: 2, alignSelf: 'center', marginBottom: 22 }} />
            <Text style={{ fontSize: 20, fontWeight: '800', color: FIG.text, marginBottom: 16 }}>My Devices</Text>

            <ScrollView style={{ maxHeight: 360 }}>
              {devices.length === 0 && (
                <Text style={{ color: FIG.muted, paddingVertical: 16, textAlign: 'center' }}>
                  No devices yet. Scan a QR code to add one.
                </Text>
              )}

              {devices.map((d) => {
                const active = d.mac === activeMac;
                return (
                  <View
                    key={d.mac}
                    style={{ flexDirection: 'row', alignItems: 'center', gap: 12, padding: 14, backgroundColor: FIG.bg, borderRadius: 14, borderWidth: 1, borderColor: FIG.border, marginBottom: 10 }}
                  >
                    <TouchableOpacity
                      onPress={() => { selectDevice(d.mac); onClose(); }}
                      style={{ width: 46, height: 46, borderRadius: 12, backgroundColor: active ? 'rgba(34,197,94,0.09)' : FIG.white, borderWidth: 1, borderColor: active ? 'rgba(34,197,94,0.22)' : FIG.border, alignItems: 'center', justifyContent: 'center' }}
                    >
                      <FigIcon name="monitor" size={22} color={active ? FIG.green : FIG.gray} />
                    </TouchableOpacity>
                    <TouchableOpacity style={{ flex: 1, minWidth: 0 }} onPress={() => { selectDevice(d.mac); onClose(); }}>
                      <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5, marginBottom: 3 }}>
                        <Text style={{ fontSize: 15, fontWeight: '700', color: active ? FIG.green : FIG.text }} numberOfLines={1}>
                          {d.name}
                        </Text>
                        {active && <Text style={{ fontSize: 12, color: FIG.green, fontWeight: '500' }}>· active</Text>}
                      </View>
                      <Text style={{ fontSize: 12, color: FIG.muted }} numberOfLines={1}>
                        {d.mac}{d.ssid ? ` · ${d.ssid}` : ' · Bluetooth only'}
                      </Text>
                    </TouchableOpacity>
                    <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10 }}>
                      {active && (
                        <View style={{ width: 28, height: 28, borderRadius: 14, borderWidth: 1.5, borderColor: FIG.green, alignItems: 'center', justifyContent: 'center' }}>
                          <FigIcon name="check" size={13} color={FIG.green} />
                        </View>
                      )}
                      <TouchableOpacity onPress={() => startRename(d)} style={{ padding: 4 }}>
                        <FigIcon name="pencil" size={21} color={FIG.gray} strokeWidth={2.3} />
                      </TouchableOpacity>
                    </View>
                  </View>
                );
              })}
            </ScrollView>

            <TouchableOpacity
              onPress={() => { onClose(); onAddDevice(); }}
              style={{ marginTop: 10, paddingVertical: 15, borderRadius: 14, backgroundColor: '#111827', flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 8 }}
            >
              <FigIcon name="plus" size={16} color={FIG.white} />
              <Text style={{ color: FIG.white, fontSize: 15, fontWeight: '700' }}>Add a new device</Text>
            </TouchableOpacity>
          </View>

        {/* Rename dialog (figma "Rename Device") — overlay inside the same
            modal: nesting a second <Modal> misbehaves on iOS. */}
        {renamingMac !== null && (
          <View style={{ position: 'absolute', top: 0, left: 0, right: 0, bottom: 0, backgroundColor: 'rgba(0,0,0,0.55)', alignItems: 'center', justifyContent: 'center', paddingHorizontal: 24 }}>
            <View style={{ backgroundColor: FIG.white, borderRadius: 20, paddingTop: 28, paddingHorizontal: 24, paddingBottom: 24, width: '100%', maxWidth: 380 }}>
              <Text style={{ fontSize: 20, fontWeight: '800', color: FIG.text, marginBottom: 6 }}>Rename Device</Text>
              <Text style={{ fontSize: 14, color: FIG.muted, marginBottom: 18 }}>Enter a new name for this device.</Text>
              <TextInput
                value={draftName}
                onChangeText={setDraftName}
                autoFocus
                onSubmitEditing={commitRename}
                style={{ paddingVertical: 13, paddingHorizontal: 16, borderRadius: 12, borderWidth: 1.5, borderColor: FIG.border, fontSize: 15, color: FIG.text, backgroundColor: FIG.white, marginBottom: 20 }}
              />
              <View style={{ flexDirection: 'row', gap: 10 }}>
                <TouchableOpacity
                  onPress={() => setRenamingMac(null)}
                  style={{ flex: 1, paddingVertical: 13, borderRadius: 12, backgroundColor: FIG.bg, alignItems: 'center' }}
                >
                  <Text style={{ color: FIG.gray, fontSize: 15, fontWeight: '600' }}>Cancel</Text>
                </TouchableOpacity>
                <TouchableOpacity
                  onPress={commitRename}
                  style={{ flex: 1, paddingVertical: 13, borderRadius: 12, backgroundColor: '#111827', alignItems: 'center' }}
                >
                  <Text style={{ color: FIG.white, fontSize: 15, fontWeight: '700' }}>Save</Text>
                </TouchableOpacity>
              </View>
            </View>
          </View>
        )}
    </SlideUpModal>
  );
}

function DeviceScreen({ onPairingComplete }: { onPairingComplete?: () => void }) {
  const {
    status, lastError, transport, bleUp, wifi,
    activeDevice, activeMac, devices, connectWithQrData, disconnect,
    selectDevice, removeDevice,
  } = usePool();

  const [qrOpen, setQrOpen] = useState(false);
  const [switcherOpen, setSwitcherOpen] = useState(false);
  const [wifiOpen, setWifiOpen] = useState(false);
  const [removeOpen, setRemoveOpen] = useState(false);

  // ---- pairing sequence ----
  // Scanning a QR starts a guided chain: BLE connect + auth, then a Wi-Fi
  // decision, then Home. `pairing` is true from scan until that decision.
  const [pairing, setPairing] = useState(false);
  const [pairingNotice, setPairingNotice] = useState<string | null>(null);

  // The "pick from nearby Bluetooth devices" list used to live here. It was
  // removed deliberately: picking a peripheral off a raw scan list is exactly
  // the manual, out-of-app connection path this build is meant to close off.
  // Pairing now goes through the QR code only.
  //
  // No stopDeviceScan cleanup on unmount: this screen unmounts on every tab
  // switch, and the scan belongs to the provider's connect flow — killing it
  // here aborted in-flight connects ("couldn't find nearby" for a device
  // that was advertising). The connect flow's own 12s timeout and
  // disconnect() handle scanner cleanup.

  // One Wi-Fi prompt per connection. Without this flag the effect below
  // would reopen the picker on every 5s state push after the user skipped.
  const [wifiPrompted, setWifiPrompted] = useState(false);

  // A failed pairing ends the chain; the error shows on the status card.
// ---- PAIRING CHAIN: scan QR -> BLE connect + auth -> Wi-Fi decision ----
// Waits for the device's first Wi-Fi status rather than popping the modal
// blindly on connect: a device that already has credentials stored gets
// to try them first (the firmware kicks that join the moment the app
// authenticates). The popup appears only for a genuinely unconfigured
// device, or after the stored network conclusively failed.
useEffect(() => {
  if (!pairing || status !== 'connected') return;
  if (wifiOpen || wifiPrompted) return;
  if (!wifi) return; // first status report is seconds away — wait for it

  if (wifi.state === 'online') {
    // Already on a network right now — pairing is complete, no popup.
    setWifiPrompted(true);
    setPairing(false);
    onPairingComplete?.();
  } else {
    // ADDING a device is a binary check: connected to Wi-Fi at this
    // moment, or not. Anything short of 'online' — unconfigured,
    // offline, or mid-attempt on stored credentials — gets the picker
    // immediately; stored credentials are deliberately NOT waited on
    // here, because a silent 20s join attempt made "add device" feel
    // stuck. Reconnects to already-registered devices keep the patient
    // verdict-based flow in the effect below.
    setWifiPrompted(true);
    setPairingNotice(null);
    setWifiOpen(true);
  }
}, [pairing, status, wifi, wifiOpen, wifiPrompted, onPairingComplete]);

// ---- RECONNECTS (no QR involved): prompt only when Wi-Fi needs setup ----
useEffect(() => {
  if (status !== 'connected') {
    if (wifiPrompted) setWifiPrompted(false); // re-arm for next connection
    return;
  }
  if (pairing || wifiOpen || wifiPrompted || !wifi) return;

  if (wifi.state === 'online') {
    setWifiPrompted(true); // nothing to set up
  } else if (wifi.state === 'unconfigured') {
    setWifiPrompted(true);
    setPairingNotice(null);
    setWifiOpen(true);
  } else if (wifi.state === 'offline' && wifi.joinFailed) {
    // Only a conclusive failure earns the popup — plain 'offline' means
    // the device is about to try its stored network (the firmware starts
    // a join whenever the app connects), so wait for that verdict.
    setWifiPrompted(true);
    setPairingNotice(
      `Couldn't join "${wifi.ssid}". Choose a network below, or pick the same one to re-enter its password.`
    );
    setWifiOpen(true);
  }
}, [status, wifi, wifiOpen, wifiPrompted, pairing]);

  // FALLBACK: never leave the user stranded without the Wi-Fi dialog.
  // The effects above wait for the device's join verdict — but if that
  // verdict is lost in transit (or no Wi-Fi report arrives at all), the
  // popup would simply never come. 25s of connected-but-not-online is
  // past every legitimate wait (the stored-network attempt caps at 20s;
  // 'unconfigured' pops immediately), so at that point open the dialog
  // regardless.
  useEffect(() => {
    if (status !== 'connected' || wifiOpen || wifiPrompted) return;
    if (wifi?.state === 'online') return;
    const ssid = wifi?.ssid;
    const t = setTimeout(() => {
      setWifiPrompted(true);
      setPairingNotice(
        ssid
          ? `Couldn't confirm the connection to "${ssid}". Choose a network below, or pick the same one to re-enter its password.`
          : null
      );
      setWifiOpen(true);
    }, 25000);
    return () => clearTimeout(t);
  }, [status, wifi?.state, wifi?.ssid, wifiOpen, wifiPrompted]);

  // Connected but the device never reports a wifi object -> it is running
  // firmware from before Wi-Fi support. Say so, instead of silently never
  // showing the Wi-Fi step (which looks exactly like a broken app).
  const [fwMissingWifi, setFwMissingWifi] = useState(false);
  useEffect(() => {
    if (status !== 'connected' || wifi) { setFwMissingWifi(false); return; }
    const t = setTimeout(() => setFwMissingWifi(true), 4000);
    return () => clearTimeout(t);
  }, [status, wifi]);

  const onQrScanned = async (data: string) => {
    setQrOpen(false);
    setPairingNotice(null);
    setPairing(true);
    // Drop whatever device we were talking to first — "add device" while
    // connected would otherwise leave the old native connection dangling.
    await disconnect();
    await connectWithQrData(data);
  };

  // Both outcomes of the Wi-Fi step end the pairing chain on the Home tab:
  // "Done" after a successful join, and "Skip for now" (BLE-only is a valid
  // operating state — Wi-Fi can be added later from this tab).
  const closeWifiModal = () => {
    setWifiOpen(false);
    if (pairing) {
      setPairing(false);
      onPairingComplete?.();
    }
  };

  const openWifiSetup = () => {
    setPairingNotice(null);
    setWifiOpen(true);
  };

  const connected = status === 'connected';
  const busy = status === 'connecting' || status === 'authenticating';

  const statusText =
    connected ? (transport === 'ble' ? 'Bluetooth Connected' : 'Wi-Fi Connected')
      : status === 'authenticating' ? 'Verifying device…'
      : status === 'connecting' ? 'Connecting…'
      : status === 'error' ? 'Connection problem'
      : 'Not Connected';

  const wifiUp = wifi?.state === 'online';
  const signalLabel = !connected
    ? 'None'
    : wifiUp && wifi?.rssi !== undefined
      ? (wifi.rssi > -60 ? 'Strong' : wifi.rssi > -75 ? 'Good' : 'Weak')
      : 'Strong';
  const signalColor =
    signalLabel === 'None' ? FIG.muted
      : signalLabel === 'Weak' ? FIG.red
      : signalLabel === 'Good' ? '#f59e0b'
      : FIG.green;

  const deviceName = activeDevice?.name || 'No Device';
  const deviceIp = wifi?.ip || activeDevice?.lastIp;

  return (
    <ScrollView style={[styles.screen, { backgroundColor: FIG.white }]} contentContainerStyle={{ paddingBottom: 40 }}>
      {/* ---- Top bar: device selector pill + radio squares (figma) ---- */}
      <View style={{ paddingHorizontal: 16, paddingTop: 16, paddingBottom: 14, flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' }}>
        <TouchableOpacity
          onPress={() => setSwitcherOpen(true)}
          style={{ flexDirection: 'row', alignItems: 'center', gap: 8, paddingVertical: 11, paddingHorizontal: 16, backgroundColor: FIG.white, borderWidth: 1, borderColor: FIG.border, borderRadius: 14, shadowColor: '#000', shadowOpacity: 0.06, shadowRadius: 4, shadowOffset: { width: 0, height: 1 }, elevation: 2 }}
        >
          <FigIcon name="monitor" size={18} color={FIG.gray} />
          <Text style={{ fontSize: 15, fontWeight: '700', color: FIG.text, maxWidth: 150 }} numberOfLines={1}>{deviceName}</Text>
          <FigIcon name="chevronDown" size={14} color={FIG.muted} />
        </TouchableOpacity>
        <View style={{ flexDirection: 'row', gap: 10 }}>
          <View style={{ width: 52, height: 52, borderRadius: 14, backgroundColor: bleUp ? 'rgba(34,197,94,0.09)' : FIG.bg, alignItems: 'center', justifyContent: 'center' }}>
            <FigIcon name="bluetooth" size={21} color={bleUp ? FIG.green : FIG.muted} />
          </View>
          <TouchableOpacity
            // Wi-Fi management needs a connected device to talk to. Without
            // one the square is a dead indicator, like the Bluetooth one.
            // Tapping opens the network list (scan + join any network, and
            // forget the connected one from inside the sheet).
            disabled={!connected}
            onPress={openWifiSetup}
            style={{ width: 52, height: 52, borderRadius: 14, backgroundColor: wifiUp ? 'rgba(34,197,94,0.09)' : FIG.bg, alignItems: 'center', justifyContent: 'center', opacity: connected ? 1 : 0.5 }}
          >
            <FigIcon name="wifi" size={21} color={wifiUp ? FIG.green : FIG.muted} />
          </TouchableOpacity>
        </View>
      </View>

      <View style={{ paddingHorizontal: 14 }}>
        {/* ---- Status card ---- */}
        <View style={figCard}>
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 9, marginBottom: 18 }}>
            <View style={{ width: 11, height: 11, borderRadius: 6, backgroundColor: connected ? FIG.green : '#d1d5db' }} />
            <Text style={{ fontSize: 19, fontWeight: '800', color: connected ? FIG.green : FIG.muted }}>{statusText}</Text>
            {busy && <ActivityIndicator size="small" color={FIG.blue} style={{ marginLeft: 4 }} />}
          </View>

          <View style={{ flexDirection: 'row', gap: 9, marginBottom: 18 }}>
            <View style={figTile}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5, marginBottom: 4 }}>
                <FigIcon name="wifi" size={14} color={wifiUp ? FIG.text : FIG.muted} />
                <Text style={{ fontSize: 13, fontWeight: '700', color: wifiUp ? FIG.text : FIG.muted }}>Wi-Fi</Text>
              </View>
              <Text style={{ fontSize: 12, color: FIG.muted, fontWeight: '500' }} numberOfLines={1}>
                {wifiUp ? wifi?.ssid || 'Connected' : 'Not connected'}
              </Text>
            </View>
            <View style={figTile}>
              <View style={{ flexDirection: 'row', alignItems: 'center', gap: 5, marginBottom: 4 }}>
                <FigIcon name="bluetooth" size={14} color={bleUp ? FIG.text : FIG.muted} />
                <Text style={{ fontSize: 13, fontWeight: '700', color: bleUp ? FIG.text : FIG.muted }}>Bluetooth</Text>
              </View>
              <Text style={{ fontSize: 12, color: FIG.muted, fontWeight: '500' }} numberOfLines={1}>
                {bleUp ? deviceName : 'Not connected'}
              </Text>
            </View>
            <View style={[figTile, { flex: 0, minWidth: 64, alignItems: 'center', justifyContent: 'center', gap: 4 }]}>
              <FigIcon name="wifi" size={16} color={signalColor} />
              <Text style={{ fontSize: 12, fontWeight: '700', color: signalColor }}>{signalLabel}</Text>
            </View>
          </View>

          <Text style={{ fontSize: 16, fontWeight: '600', color: FIG.text }}>
            {connected ? `${deviceName}${deviceIp ? ` · ${deviceIp}` : ''}` : '—'}
          </Text>
          {connected && activeDevice && (
            <Text style={{ fontSize: 13, color: FIG.green, fontWeight: '500', marginTop: 4 }}>{activeDevice.mac}</Text>
          )}

          {wifi?.state === 'offline' && (
            <View style={styles.noticeWarn}>
              <Text style={styles.noticeTxt}>
                Wi-Fi is down. Stay within Bluetooth range to keep control — the app
                switches back to Wi-Fi on its own once the network returns.
              </Text>
            </View>
          )}
          {fwMissingWifi && (
            <View style={styles.noticeWarn}>
              <Text style={styles.noticeTxt}>
                ⚠️ This controller&apos;s firmware doesn&apos;t support Wi-Fi. Flash the
                latest firmware to enable Wi-Fi setup — until then it works over
                Bluetooth only.
              </Text>
            </View>
          )}
          {lastError && (
            <View style={styles.noticeErr}>
              <Text style={[styles.noticeTxt, { color: COLORS.danger }]}>{lastError}</Text>
            </View>
          )}
        </View>

        {/* ---- Add Another Device (figma) ---- */}
        <Text style={figSection}>{devices.length > 0 ? 'Add Another Device' : 'Pair a Device'}</Text>
        <View style={figCard}>
          <TouchableOpacity
            onPress={() => setQrOpen(true)}
            style={{ paddingVertical: 16, borderRadius: 13, backgroundColor: '#111827', flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 10, marginBottom: activeDevice ? 10 : 0 }}
          >
            <FigIcon name="qr" size={22} color={FIG.white} />
            <Text style={{ color: FIG.white, fontSize: 15, fontWeight: '700' }}>Scan QR</Text>
          </TouchableOpacity>
          {/* Nothing to remove or reconnect without a paired device — these
              two only exist once a device is known. */}
          {activeDevice && (
            <>
              <TouchableOpacity
                onPress={() => setRemoveOpen(true)}
                style={{ paddingVertical: 16, borderRadius: 13, borderWidth: 1, borderColor: FIG.border, backgroundColor: FIG.white, flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 10, marginBottom: 10 }}
              >
                <FigIcon name="trash" size={18} color={FIG.text} />
                <Text style={{ color: FIG.text, fontSize: 15, fontWeight: '600' }}>Remove this device</Text>
              </TouchableOpacity>
              <TouchableOpacity
                onPress={() => (connected || busy ? disconnect() : selectDevice(activeDevice.mac))}
                style={{ paddingVertical: 16, borderRadius: 13, borderWidth: 1, borderColor: connected || busy ? '#fecaca' : '#bbf7d0', backgroundColor: connected || busy ? '#fff5f5' : '#f0fdf4', flexDirection: 'row', alignItems: 'center', justifyContent: 'center', gap: 10 }}
              >
                {connected || busy
                  ? <FigIcon name="wifiOff" size={18} color={FIG.red} />
                  : <FigIcon name="wifi" size={18} color={FIG.green} />}
                <Text style={{ color: connected || busy ? FIG.red : FIG.green, fontSize: 15, fontWeight: '700' }}>
                  {connected || busy ? 'Disconnect' : 'Reconnect'}
                </Text>
              </TouchableOpacity>
            </>
          )}
        </View>
      </View>

      {/* ---- Remove Device confirmation (figma) ---- */}
      <SlideUpModal
        visible={removeOpen}
        onClose={() => setRemoveOpen(false)}
        overlayStyle={{ justifyContent: 'center', alignItems: 'center', paddingHorizontal: 24 }}
        distance={80}
      >
          <View
            style={{ backgroundColor: FIG.white, borderRadius: 20, paddingTop: 28, paddingHorizontal: 24, paddingBottom: 24, width: '100%', maxWidth: 380 }}
          >
            <Text style={{ fontSize: 20, fontWeight: '800', color: FIG.text, marginBottom: 8 }}>Remove Device</Text>
            <Text style={{ fontSize: 14, color: FIG.muted, marginBottom: 24, lineHeight: 21 }}>
              Are you sure you want to remove <Text style={{ color: FIG.text, fontWeight: '700' }}>{deviceName}</Text>?
              This will disconnect and unpair the device.
            </Text>
            <View style={{ flexDirection: 'row', gap: 10 }}>
              <TouchableOpacity
                onPress={() => setRemoveOpen(false)}
                style={{ flex: 1, paddingVertical: 13, borderRadius: 12, backgroundColor: FIG.bg, alignItems: 'center' }}
              >
                <Text style={{ color: FIG.gray, fontSize: 15, fontWeight: '600' }}>Cancel</Text>
              </TouchableOpacity>
              <TouchableOpacity
                onPress={() => { setRemoveOpen(false); if (activeMac) removeDevice(activeMac); }}
                style={{ flex: 1, paddingVertical: 13, borderRadius: 12, backgroundColor: FIG.red, alignItems: 'center' }}
              >
                <Text style={{ color: FIG.white, fontSize: 15, fontWeight: '700' }}>Remove</Text>
              </TouchableOpacity>
            </View>
          </View>
      </SlideUpModal>

      <QrScannerModal visible={qrOpen} onClose={() => setQrOpen(false)} onScanned={onQrScanned} />
      <WifiSetupModal visible={wifiOpen} onClose={closeWifiModal} notice={pairingNotice} />
      <DeviceSwitcherModal
        visible={switcherOpen}
        onClose={() => setSwitcherOpen(false)}
        // Wait for the sheet's closing animation to finish and its Modal to
        // unmount — a modal presented while another is still up is silently
        // dropped by the OS, which left the scanner never appearing.
        onAddDevice={() => setTimeout(() => setQrOpen(true), 300)}
      />
    </ScrollView>
  );
}

// ============================================================
//  SETTINGS TAB — firmware version + OTA update
// ============================================================
type OtaState =
  | { kind: 'checking' }
  | { kind: 'uptodate'; v: string }
  | { kind: 'available'; v: string; url: string; notes?: string }
  | { kind: 'blocked'; msg: string }
  | { kind: 'busy'; label: string; pct: number }
  | { kind: 'done'; v: string }
  | { kind: 'error'; msg: string };

function FirmwareUpdateModal({ visible, onClose }: { visible: boolean; onClose: () => void }) {
  const { deviceFw, pumps, otaPerform, status, activeMac } = usePool();
  const [ota, setOta] = useState<OtaState>({ kind: 'checking' });
  const busy = ota.kind === 'busy';

  useEffect(() => {
    if (!visible) return;
    fwUpdateUiOpen.current = true;
    return () => { fwUpdateUiOpen.current = false; };
  }, [visible]);

  useEffect(() => {
    if (!visible) return;
    let cancelled = false;
    (async () => {
      setOta({ kind: 'checking' });
      if (status !== 'connected' || !deviceFw) {
        setOta({
          kind: 'error',
          msg: !deviceFw && status === 'connected'
            ? "The device didn't report a firmware version — it runs a build from before OTA support. Flash it over USB once."
            : 'Connect to the device first — the app compares the update against the version the device itself reports.',
        });
        return;
      }
      try {
        const res = await fetch(FW_MANIFEST_URL);
        if (!res.ok) throw new Error(`manifest fetch failed (${res.status})`);
        const m = await res.json();
        if (typeof m?.version !== 'string' || typeof m?.url !== 'string') throw new Error('malformed version.json');
        if (cancelled) return;
        if (versionNewer(m.version, deviceFw)) setOta({ kind: 'available', v: m.version, url: m.url, notes: m.notes });
        else setOta({ kind: 'uptodate', v: deviceFw });
      } catch (err: any) {
        if (!cancelled) setOta({ kind: 'error', msg: `Could not check for updates: ${err?.message || err}` });
      }
    })();
    return () => { cancelled = true; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [visible]);

  const startUpdate = async () => {
    if (ota.kind !== 'available') return;
    // Updating stalls the controller for ~20s — never while a pump runs.
    const manual = pumps.filter((pp) => pp.mode === 'on' || (pp.active && pp.mode !== 'auto'));
    if (manual.length) {
      setOta({ kind: 'blocked', msg: `${manual.map((pp) => pp.name).join(', ')} is running. Turn the pump off, then try again.` });
      return;
    }
    const scheduled = pumps.filter((pp) => pp.active && pp.mode === 'auto');
    if (scheduled.length) {
      setOta({ kind: 'blocked', msg: `${scheduled.map((pp) => pp.name).join(', ')} is running from a schedule. Disable that schedule first, then try again.` });
      return;
    }
    const target = ota;
    try {
      await otaPerform(target.url, target.v, (label, pct) => setOta({ kind: 'busy', label, pct }));
      fwJustUpdatedTo.current = { mac: activeMac || '', version: target.v };
      setOta({ kind: 'done', v: target.v });
    } catch (err: any) {
      setOta({ kind: 'error', msg: err?.message || String(err) });
    }
  };

  return (
    <SlideUpModal
      visible={visible}
      onClose={busy ? () => {} : onClose}
      closeOnBackdrop={!busy}
      overlayStyle={{ justifyContent: 'center', alignItems: 'center', paddingHorizontal: 24 }}
      distance={80}
    >
      <View style={{ backgroundColor: FIG.white, borderRadius: 20, paddingTop: 28, paddingHorizontal: 24, paddingBottom: 24, width: '100%', maxWidth: 380 }}>
        <View style={{ flexDirection: 'row', alignItems: 'center', gap: 10, marginBottom: 10 }}>
          <FigIcon name="download" size={22} color={FIG.text} />
          <Text style={{ fontSize: 20, fontWeight: '800', color: FIG.text }}>Firmware Update</Text>
        </View>

        {ota.kind === 'checking' && (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingVertical: 14 }}>
            <ActivityIndicator color={FIG.green} />
            <Text style={{ fontSize: 14, color: FIG.muted }}>Checking for updates…</Text>
          </View>
        )}

        {ota.kind === 'uptodate' && (
          <Text style={{ fontSize: 14, color: FIG.muted, lineHeight: 21, marginBottom: 18 }}>
            Your controller is up to date (<Text style={{ color: FIG.text, fontWeight: '700' }}>v{ota.v}</Text>).
          </Text>
        )}

        {ota.kind === 'available' && (
          <>
            <Text style={{ fontSize: 14, color: FIG.muted, lineHeight: 21, marginBottom: 8 }}>
              Version <Text style={{ color: FIG.text, fontWeight: '700' }}>v{ota.v}</Text> is available
              (installed: v{deviceFw}).
            </Text>
            {!!ota.notes && (
              <Text style={{ fontSize: 13, color: FIG.gray, lineHeight: 19, marginBottom: 8 }}>{ota.notes}</Text>
            )}
            <Text style={{ fontSize: 12, color: FIG.muted, lineHeight: 18, marginBottom: 18 }}>
              Your phone must be on the same Wi-Fi network as the controller. The device restarts
              when the update finishes; all settings and schedules are kept.
            </Text>
          </>
        )}

        {(ota.kind === 'blocked' || ota.kind === 'error') && (
          <Text style={{ fontSize: 14, color: ota.kind === 'blocked' ? '#b45309' : FIG.red, lineHeight: 21, marginBottom: 18 }}>
            {ota.msg}
          </Text>
        )}

        {ota.kind === 'busy' && (
          <View style={{ paddingVertical: 6, marginBottom: 16 }}>
            <View style={{ flexDirection: 'row', alignItems: 'center', gap: 12, marginBottom: 12 }}>
              <ActivityIndicator color={FIG.green} />
              <Text style={{ fontSize: 14, color: FIG.text, fontWeight: '600', flex: 1 }}>{ota.label}</Text>
            </View>
            <View style={{ height: 6, borderRadius: 3, backgroundColor: FIG.bg, overflow: 'hidden' }}>
              <View style={{ height: 6, borderRadius: 3, backgroundColor: FIG.green, width: `${Math.round(ota.pct * 100)}%` }} />
            </View>
            <Text style={{ fontSize: 12, color: FIG.muted, marginTop: 10 }}>
              Keep the app open — do not power the controller off.
            </Text>
          </View>
        )}

        {ota.kind === 'done' && (
          <View style={{ flexDirection: 'row', alignItems: 'center', gap: 8, marginBottom: 18 }}>
            <FigIcon name="check" size={18} color={FIG.green} />
            <Text style={{ fontSize: 14, color: FIG.green, fontWeight: '600', flex: 1 }}>
              Updated to v{ota.v}. The controller is back online.
            </Text>
          </View>
        )}

        <View style={{ flexDirection: 'row', gap: 10 }}>
          {ota.kind === 'available' ? (
            <>
              <TouchableOpacity onPress={onClose} style={{ flex: 1, paddingVertical: 13, borderRadius: 12, backgroundColor: FIG.bg, alignItems: 'center' }}>
                <Text style={{ color: FIG.gray, fontSize: 15, fontWeight: '600' }}>Later</Text>
              </TouchableOpacity>
              <TouchableOpacity onPress={startUpdate} style={{ flex: 1, paddingVertical: 13, borderRadius: 12, backgroundColor: '#111827', alignItems: 'center' }}>
                <Text style={{ color: FIG.white, fontSize: 15, fontWeight: '700' }}>Update</Text>
              </TouchableOpacity>
            </>
          ) : !busy ? (
            <TouchableOpacity onPress={onClose} style={{ flex: 1, paddingVertical: 13, borderRadius: 12, backgroundColor: FIG.bg, alignItems: 'center' }}>
              <Text style={{ color: FIG.gray, fontSize: 15, fontWeight: '600' }}>Close</Text>
            </TouchableOpacity>
          ) : null}
        </View>
      </View>
    </SlideUpModal>
  );
}

function SettingsScreen() {
  const { deviceFw, status } = usePool();
  const [updOpen, setUpdOpen] = useState(false);

  return (
    <ScrollView style={[styles.screen, { backgroundColor: FIG.white }]} contentContainerStyle={{ paddingBottom: 40 }}>
      <View style={{ paddingHorizontal: 2, paddingTop: 18, paddingBottom: 6 }}>
        <Text style={{ fontSize: 24, fontWeight: '800', color: FIG.text }}>Settings</Text>
      </View>

      <Text style={figSection}>Firmware</Text>
      <View style={figCard}>
        {/* Tapping the version row runs the same GitHub-vs-device check
            and pops the update dialog with the verdict. */}
        <TouchableOpacity
          onPress={() => { if (!fwUpdateUiOpen.current) setUpdOpen(true); }}
          style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingVertical: 13 }}
        >
          <FigIcon name="info" size={20} color={FIG.gray} />
          <Text style={{ fontSize: 15, fontWeight: '500', color: FIG.text, flex: 1 }}>Firmware version</Text>
          <Text style={{ fontSize: 15, fontWeight: '700', color: deviceFw ? FIG.green : FIG.muted }}>
            {deviceFw ? `v${deviceFw}` : status === 'connected' ? 'unknown' : '—'}
          </Text>
        </TouchableOpacity>
        <View style={{ height: 1, backgroundColor: FIG.border }} />
        <TouchableOpacity
          onPress={() => { if (!fwUpdateUiOpen.current) setUpdOpen(true); }}
          style={{ flexDirection: 'row', alignItems: 'center', gap: 12, paddingVertical: 13 }}
        >
          <FigIcon name="refresh" size={20} color={FIG.gray} />
          <Text style={{ fontSize: 15, fontWeight: '500', color: FIG.text, flex: 1 }}>Firmware update</Text>
          <FigIcon name="chevronRight" size={18} color={FIG.muted} />
        </TouchableOpacity>
      </View>

      <FirmwareUpdateModal visible={updOpen} onClose={() => setUpdOpen(false)} />
    </ScrollView>
  );
}

// Update watchdog: the moment the connected device reports its version
// (app open / reconnect) the manifest is checked and the popup appears on
// a mismatch — and it re-checks both sides every 10 minutes after that.
function AutoUpdatePrompt() {
  const { deviceFw, status, activeMac } = usePool();
  const [open, setOpen] = useState(false);
  // Refs so the 10-minute interval always sees current values without
  // re-arming itself on every state change.
  const openRef = useRef(open); openRef.current = open;
  const fwRef = useRef(deviceFw); fwRef.current = deviceFw;
  const statusRef = useRef(status); statusRef.current = status;
  const macRef = useRef(activeMac); macRef.current = activeMac;

  const check = useCallback(async () => {
    if (openRef.current || fwUpdateUiOpen.current || modalDepth.current > 0) return; // some dialog is already up
    const fw = fwRef.current;
    if (statusRef.current !== 'connected' || !fw) return;
    try {
      const res = await fetch(FW_MANIFEST_URL);
      if (!res.ok) return;
      const m = await res.json();
      if (openRef.current || fwUpdateUiOpen.current || modalDepth.current > 0) return;
      if (typeof m?.version !== 'string') return;
      // THIS device was just updated to this version — the state the app
      // holds may still predate the reboot; don't re-suggest it. A
      // DIFFERENT device on old firmware still gets the popup.
      const just = fwJustUpdatedTo.current;
      if (just && just.version === m.version && just.mac === (macRef.current || '')) return;
      if (versionNewer(m.version, fw)) setOpen(true);
    } catch { /* offline or manifest missing — stay quiet */ }
  }, []);

  // Immediate check whenever the device's reported version (re)arrives —
  // including after SWITCHING devices: deviceFw is cleared on disconnect
  // and re-learned from the new device's first state push.
  useEffect(() => {
    if (status === 'connected' && deviceFw) check();
  }, [deviceFw, status, activeMac, check]);

  // And the 10-minute recheck cadence.
  useEffect(() => {
    const t = setInterval(check, 10 * 60 * 1000);
    return () => clearInterval(t);
  }, [check]);

  return <FirmwareUpdateModal visible={open} onClose={() => setOpen(false)} />;
}

// ============================================================
//  BOTTOM TAB BAR + ROOT APP
// ============================================================
type TabKey = 'home' | 'device' | 'settings';

function TabButton({ label, icon, active, onPress }: { label: string; icon: FigIconName; active: boolean; onPress: () => void }) {
  return (
    <TouchableOpacity style={styles.tabBtn} onPress={onPress}>
      <FigIcon name={icon} size={22} color={active ? FIG.green : FIG.muted} />
      <Text style={[styles.tabLabel, active && styles.tabLabelActive]}>{label}</Text>
      {active && <View style={styles.tabIndicator} />}
    </TouchableOpacity>
  );
}

export default function App() {
  const [tab, setTab] = useState<TabKey>('home');

  return (
    <PoolProvider>
      <SafeAreaView style={styles.root}>
        <StatusBar barStyle="dark-content" backgroundColor={COLORS.bg} />
        <View style={{ flex: 1 }}>
          {tab === 'home' && <HomeScreen />}
          {tab === 'device' && <DeviceScreen onPairingComplete={() => setTab('home')} />}
          {tab === 'settings' && <SettingsScreen />}
          <AutoUpdatePrompt />
        </View>

        <View style={styles.tabBar}>
          <TabButton label="Home" icon="home" active={tab === 'home'} onPress={() => setTab('home')} />
          <TabButton label="Device" icon="monitor" active={tab === 'device'} onPress={() => setTab('device')} />
          <TabButton label="Settings" icon="gear" active={tab === 'settings'} onPress={() => setTab('settings')} />
        </View>
      </SafeAreaView>
    </PoolProvider>
  );
}

// ============================================================
//  STYLES
// ============================================================
const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: COLORS.bg },
  screen: { flex: 1, backgroundColor: COLORS.bg, paddingHorizontal: 14 },

  card: {
    backgroundColor: COLORS.glassBg,
    borderWidth: 1,
    borderColor: COLORS.glassBorder,
    borderRadius: 20,
    padding: 18,
    marginTop: 14,
    shadowColor: '#0f172a',
    shadowOpacity: 0.08,
    shadowRadius: 12,
    shadowOffset: { width: 0, height: 4 },
    elevation: 2,
  },

  headerCard: { alignItems: 'center', paddingVertical: 28 },
  chip: {
    flexDirection: 'row', alignItems: 'center', gap: 8,
    backgroundColor: COLORS.chipBg, borderWidth: 1, borderColor: COLORS.glassBorder,
    borderRadius: 20, paddingHorizontal: 14, paddingVertical: 6, marginBottom: 16,
  },
  statusDot: { width: 8, height: 8, borderRadius: 4, backgroundColor: COLORS.on },
  chipTxt: { fontSize: 12, color: COLORS.muted, letterSpacing: 1 },
  clock: { fontSize: 46, fontWeight: '200', color: '#0f172a', letterSpacing: 1 },
  clockSub: { fontSize: 12, color: COLORS.label, letterSpacing: 2, marginTop: 6 },

  rCard: { borderWidth: 1, borderColor: 'transparent' },
  rTop: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 10 },
  rTag: { fontSize: 12, letterSpacing: 2, color: COLORS.label, textTransform: 'uppercase' },
  rName: { fontSize: 22, fontWeight: '600' },
  rCd: {
    backgroundColor: 'rgba(15,23,42,0.045)', borderWidth: 1, borderColor: 'rgba(15,23,42,0.06)',
    borderRadius: 11, paddingVertical: 10, paddingHorizontal: 12, marginTop: 4, marginBottom: 4,
  },

  seg: { flexDirection: 'row', gap: 6, marginTop: 12 },
  segBtn: { flex: 1, paddingVertical: 9, borderRadius: 9, borderWidth: 1, borderColor: '#000', backgroundColor: COLORS.segBg, alignItems: 'center' },
  segBtnActive: { backgroundColor: COLORS.on },
  segTxt: { fontSize: 13, fontWeight: '700', color: '#000' },
  segTxtActive: { color: '#fff' },

  secTitle: { fontSize: 12, letterSpacing: 2, fontWeight: '700', color: '#000', marginBottom: 14, textTransform: 'uppercase' },

  pumpItem: { paddingTop: 14, marginTop: 14, borderTopWidth: 1, borderTopColor: 'rgba(15,23,42,0.08)' },
  pumpActual: { fontSize: 14, color: COLORS.muted, marginTop: 2, marginBottom: 4 },
  pumpInline: { marginTop: 14, paddingTop: 14, borderTopWidth: 1, borderTopColor: 'rgba(15,23,42,0.08)' },

  presetHeader: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginBottom: 14 },
  presetCount: { backgroundColor: 'rgba(15,23,42,0.04)', borderWidth: 1, borderColor: COLORS.glassBorder, borderRadius: 20, paddingHorizontal: 11, paddingVertical: 3 },
  pItem: {
    flexDirection: 'row', alignItems: 'center', padding: 14, backgroundColor: 'rgba(15,23,42,0.025)',
    borderRadius: 13, borderWidth: 1, borderColor: 'rgba(15,23,42,0.06)', marginBottom: 8,
  },
  pBtn: { paddingHorizontal: 10, paddingVertical: 6, marginLeft: 4 },

  btnAdd: { marginTop: 6, paddingVertical: 13, borderRadius: 13, borderWidth: 1.5, borderStyle: 'dashed', borderColor: 'rgba(15,23,42,0.2)', alignItems: 'center' },
  btnAddTxt: { color: COLORS.muted, fontWeight: '600' },

  btnPrimary: { backgroundColor: '#0f172a', paddingVertical: 13, borderRadius: 12, alignItems: 'center' },
  btnPrimaryTxt: { color: '#fff', fontWeight: '700' },
  btnSec: { backgroundColor: 'rgba(15,23,42,0.06)', paddingVertical: 13, borderRadius: 12, alignItems: 'center' },
  btnSecTxt: { color: COLORS.text, fontWeight: '700' },

  fc: { backgroundColor: '#fff', borderWidth: 1, borderColor: 'rgba(15,23,42,0.14)', borderRadius: 10, paddingHorizontal: 12, paddingVertical: 11, marginBottom: 10 },
  fieldLabel: { fontSize: 12, fontWeight: '700', color: COLORS.muted, letterSpacing: 1, marginBottom: 6, marginTop: 8, textTransform: 'uppercase' },
  fhint: { fontSize: 12, color: COLORS.label, marginBottom: 4 },

  chipOption: { paddingHorizontal: 12, paddingVertical: 7, borderRadius: 16, borderWidth: 1, borderColor: 'rgba(15,23,42,0.14)', backgroundColor: '#fff' },
  chipOptionActive: { backgroundColor: '#0f172a', borderColor: '#0f172a' },
  chipOptionTxt: { fontSize: 13, color: COLORS.text },
  chipOptionTxtActive: { color: '#fff', fontWeight: '700' },

  modalOverlay: { flex: 1, backgroundColor: 'rgba(15,23,42,0.5)', justifyContent: 'flex-end' },
  modalCard: { backgroundColor: '#fff', borderTopLeftRadius: 24, borderTopRightRadius: 24, padding: 20, maxHeight: '88%' },
  modalTitle: { fontSize: 18, fontWeight: '700', marginBottom: 16 },
  modalActs: { flexDirection: 'row', gap: 10, marginTop: 20 },

  pickerSheet: { backgroundColor: '#fff', borderRadius: 16, padding: 16, marginHorizontal: 30 },
  pickerRow: { paddingVertical: 12, borderBottomWidth: 1, borderBottomColor: 'rgba(15,23,42,0.06)' },

  infoRow: { flexDirection: 'row', justifyContent: 'space-between', paddingVertical: 9, borderBottomWidth: 1, borderBottomColor: 'rgba(15,23,42,0.06)' },

  avatar: { width: 72, height: 72, borderRadius: 36, backgroundColor: COLORS.r2, alignItems: 'center', justifyContent: 'center' },
  avatarTxt: { color: '#fff', fontSize: 24, fontWeight: '700' },
  accountRow: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', paddingVertical: 13, borderBottomWidth: 1, borderBottomColor: 'rgba(15,23,42,0.06)' },

  tabBar: {
    flexDirection: 'row', borderTopWidth: 1, borderTopColor: 'rgba(15,23,42,0.08)',
    backgroundColor: '#fff', paddingTop: 8, paddingBottom: 10,
  },
  tabBtn: { flex: 1, alignItems: 'center', gap: 2 },
  tabIcon: { fontSize: 20, opacity: 0.5 },
  tabLabel: { fontSize: 11, color: COLORS.muted },
  tabLabelActive: { color: '#22c55e', fontWeight: '700' },
  tabIndicator: { position: 'absolute', top: -8, width: 24, height: 3, borderRadius: 2, backgroundColor: '#22c55e' },

  scannerRoot: { flex: 1, backgroundColor: '#000' },
  scannerOverlay: { flex: 1, alignItems: 'center', justifyContent: 'center' },
  scannerFrame: { width: 240, height: 240, borderWidth: 3, borderColor: '#fff', borderRadius: 16, backgroundColor: 'transparent' },
  scannerHint: { color: '#fff', marginTop: 20, fontSize: 14 },
  scannerPermissionBox: { flex: 1, alignItems: 'center', justifyContent: 'center', padding: 24 },
  scannerClose: { position: 'absolute', top: 50, right: 20, padding: 12, backgroundColor: 'rgba(255,255,255,0.15)', borderRadius: 20 },

  // ---- multi-device ----
  deviceBar: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', paddingHorizontal: 4, paddingTop: 14, paddingBottom: 6 },
  deviceBarBtn: { flexDirection: 'row', alignItems: 'center', flex: 1, marginRight: 12 },
  deviceBarName: { fontSize: 20, fontWeight: '800', color: COLORS.text, flexShrink: 1 },
  deviceBarChevron: { fontSize: 16, color: COLORS.muted, marginLeft: 6 },
  deviceRow: { flexDirection: 'row', alignItems: 'center', paddingVertical: 12, borderBottomWidth: 1, borderBottomColor: COLORS.glassBorder },
  deviceBarSub: { fontSize: 12, color: COLORS.label, paddingHorizontal: 4, marginBottom: 8 },
  addBtn: { paddingVertical: 6, paddingHorizontal: 12, borderRadius: 18, backgroundColor: COLORS.r1Soft },
  addBtnTxt: { color: COLORS.r1, fontWeight: '700', fontSize: 13 },

  // ---- hero status ----
  heroTop: { flexDirection: 'row', alignItems: 'center' },
  heroDot: { width: 12, height: 12, borderRadius: 6, marginRight: 10 },
  heroStatus: { fontSize: 19, fontWeight: '800', letterSpacing: -0.3 },
  heroMeta: { marginTop: 12 },
  heroMetaTxt: { fontSize: 12, color: COLORS.label, marginTop: 2 },

  chipRow: { flexDirection: 'row', gap: 8, marginTop: 14, flexWrap: 'wrap' },
  tChip: { paddingVertical: 7, paddingHorizontal: 12, borderRadius: 18, backgroundColor: COLORS.chipBg, borderWidth: 1, borderColor: 'transparent' },
  tChipOn: { backgroundColor: COLORS.onSoft, borderColor: 'rgba(5,150,105,0.45)' },
  tChipTxt: { fontSize: 12, fontWeight: '600', color: COLORS.muted },
  tChipTxtOn: { color: '#047857' },

  noticeWarn: { marginTop: 14, padding: 12, borderRadius: 12, backgroundColor: 'rgba(234,179,8,0.12)', borderWidth: 1, borderColor: 'rgba(234,179,8,0.35)' },
  noticeErr: { marginTop: 14, padding: 12, borderRadius: 12, backgroundColor: 'rgba(220,38,38,0.08)', borderWidth: 1, borderColor: 'rgba(220,38,38,0.28)' },
  noticeTxt: { fontSize: 13, color: COLORS.text, lineHeight: 19 },

  // ---- wi-fi ----
  wifiRow: { flexDirection: 'row', alignItems: 'center', paddingVertical: 14, borderBottomWidth: 1, borderBottomColor: COLORS.glassBorder },
  wifiIcon: { fontSize: 15, width: 26 },
  wifiSsid: { flex: 1, fontSize: 15, fontWeight: '600', color: COLORS.text },
  wifiSignal: { fontSize: 15, color: COLORS.muted, letterSpacing: 1 },
  wifiCentre: { flex: 1, alignItems: 'center', justifyContent: 'center', paddingVertical: 40, paddingHorizontal: 8 },
  wifiBigTitle: { fontSize: 19, fontWeight: '800', color: COLORS.text, marginTop: 18, marginBottom: 8, textAlign: 'center' },
  wifiSummary: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginBottom: 6 },
  wifiSummarySsid: { fontSize: 17, fontWeight: '700', color: COLORS.text, flex: 1 },
  statePill: { paddingHorizontal: 11, paddingVertical: 4, borderRadius: 20, borderWidth: 1 },
  statePillOn: { backgroundColor: COLORS.onSoft, borderColor: 'rgba(5,150,105,0.5)' },
  statePillOff: { backgroundColor: 'rgba(220,38,38,0.08)', borderColor: 'rgba(220,38,38,0.3)' },
  statePillTxt: { fontSize: 11, fontWeight: '800', letterSpacing: 1 },
});