#!/usr/bin/env python3
"""
Local OTA upload page for the relay-timer ESP32-S3.

The firmware already exposes the app's OTA API over HTTP (challenge ->
auth -> POST /update with the raw .bin). This script wraps that flow in a
browser page. No firmware changes are needed — but the ESP32 sends no CORS
headers, so a plain HTML page can't call it from a browser; this little
server relays the requests instead.

Usage:
    python3 ota_uploader.py
    then open http://localhost:8000

Your computer and the ESP32 must be on the same Wi-Fi. Turn pumps and pump
schedules OFF first — the firmware refuses updates while a pump runs. The
device reboots itself after a verified upload.
"""
import hashlib
import hmac
import http.client
import json
import socket
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

# Must match APP_DEVICE_SECRET in relay_timer_latest/app_link.h.
DEVICE_SECRET = b"3f9a1c7e5b28d04a6ef391c8b7d25a06e4c8193bd7f6025a8c1e93b47dfa6210"
PORT = 8000

PAGE = """<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 OTA Upload</title>
<style>
  body { font-family: -apple-system, system-ui, sans-serif; background: #f4f5f7;
         display: flex; justify-content: center; padding-top: 8vh; color: #111; }
  .card { background: #fff; border-radius: 14px; padding: 28px; width: 380px;
          box-shadow: 0 4px 18px rgba(0,0,0,.08); }
  h1 { font-size: 20px; margin: 0 0 4px; }
  p.hint { color: #667; font-size: 13px; margin: 0 0 18px; }
  label { display: block; font-size: 12px; font-weight: 700; color: #556;
          text-transform: uppercase; margin: 14px 0 4px; }
  input[type=text] { width: 100%; box-sizing: border-box; padding: 10px;
          border: 1px solid #ccd; border-radius: 8px; font-size: 15px; }
  input[type=file] { width: 100%; font-size: 14px; }
  button { width: 100%; margin-top: 20px; padding: 12px; border: 0;
           border-radius: 8px; background: #10b981; color: #fff;
           font-size: 16px; font-weight: 700; cursor: pointer; }
  button:disabled { background: #a7b0ba; cursor: default; }
  #status { margin-top: 16px; font-size: 14px; line-height: 1.5;
            white-space: pre-wrap; }
  .ok { color: #0a7d55; } .err { color: #c0392b; }
</style>
</head>
<body>
<div class="card">
  <h1>ESP32 OTA Upload</h1>
  <p class="hint">Same Wi-Fi required. Turn pumps off first &mdash; the
  device refuses updates while a pump is running, and it reboots itself
  when the upload verifies.</p>
  <label>Device IP</label>
  <input type="text" id="ip" placeholder="e.g. 192.168.1.42">
  <label>Firmware (.bin)</label>
  <input type="file" id="file" accept=".bin">
  <button id="go" onclick="upload()">Upload firmware</button>
  <div id="status"></div>
</div>
<script>
const $ = (id) => document.getElementById(id);
$('ip').value = localStorage.getItem('espIp') || '';

function setStatus(msg, cls) {
  $('status').textContent = msg;
  $('status').className = cls || '';
}

async function upload() {
  const ip = $('ip').value.trim();
  const file = $('file').files[0];
  if (!ip) return setStatus('Enter the device IP.', 'err');
  if (!file) return setStatus('Choose a .bin file.', 'err');
  localStorage.setItem('espIp', ip);

  $('go').disabled = true;
  setStatus('Uploading ' + file.name + ' (' + Math.round(file.size / 1024) +
            ' KB)\\u2026 this can take a minute or two.');
  try {
    const res = await fetch('/upload?ip=' + encodeURIComponent(ip),
                            { method: 'POST', body: file });
    const json = await res.json();
    if (!json.ok) { setStatus('Failed: ' + json.message, 'err'); return; }

    setStatus('Upload verified \\u2014 device is rebooting\\u2026');
    // Poll ping until the device answers again, then show its version.
    const deadline = Date.now() + 90000;
    while (Date.now() < deadline) {
      await new Promise(r => setTimeout(r, 3000));
      try {
        const p = await fetch('/ping?ip=' + encodeURIComponent(ip));
        const pj = await p.json();
        if (pj.ok && pj.fw) {
          setStatus('Done \\u2014 device is back online running firmware v' +
                    pj.fw + '.', 'ok');
          return;
        }
      } catch (e) { /* still rebooting */ }
    }
    setStatus('Upload verified, but the device has not answered within ' +
              '90s. Check it \\u2014 the previous firmware still boots if ' +
              'the update failed.', 'err');
  } catch (e) {
    setStatus('Failed: ' + e.message, 'err');
  } finally {
    $('go').disabled = false;
  }
}
</script>
</body>
</html>
"""


def esp_request(ip, path, data=None, headers=None, timeout=15):
    """One HTTP round-trip to the ESP32; JSON error bodies become messages."""
    req = urllib.request.Request(f"http://{ip}{path}", data=data,
                                 headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        # 401/409/400 carry the firmware's ack JSON with the actual reason.
        try:
            body = json.loads(e.read().decode())
            raise RuntimeError(body.get("message") or f"HTTP {e.code}")
        except (ValueError, KeyError):
            raise RuntimeError(f"HTTP {e.code}")


def upload_firmware(ip, token, firmware):
    """POST the image in paced chunks. Bulk-sending the whole body can
    overrun the ESP32 while it stalls on flash-sector erases — the device
    then drops the connection (client sees 'Broken pipe')."""
    conn = http.client.HTTPConnection(ip, 80, timeout=300)
    try:
        conn.putrequest("POST", "/update")
        conn.putheader("Authorization", f"Bearer {token}")
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(len(firmware)))
        conn.endheaders()
        chunk = 4096
        next_mark = 0
        try:
            for off in range(0, len(firmware), chunk):
                conn.send(firmware[off:off + chunk])
                time.sleep(0.004)  # let flash writes drain between segments
                pct = (off + chunk) * 100 // len(firmware)
                if pct >= next_mark:
                    print(f"[ota] sent {min(pct, 100)}%")
                    next_mark += 20
        except (BrokenPipeError, ConnectionResetError, socket.error):
            # The device stopped reading. It may have sent an error reply
            # before closing — try to read it so the user sees the REASON.
            try:
                resp = conn.getresponse()
                body = json.loads(resp.read().decode())
                msg = body.get("message") or f"HTTP {resp.status}"
            except Exception:
                msg = ("device closed the connection mid-upload — check the "
                       "serial log, then POWER-CYCLE the board before "
                       "retrying (on current firmware a retry without a "
                       "power cycle can corrupt the update)")
            raise RuntimeError(msg)
        resp = conn.getresponse()
        raw = resp.read().decode()
        try:
            return json.loads(raw)
        except ValueError:
            raise RuntimeError(f"HTTP {resp.status}: {raw[:200]}")
    finally:
        conn.close()


def authenticate(ip):
    chal = esp_request(ip, "/challenge")
    digest = hmac.new(DEVICE_SECRET, chal["nonce"].encode(),
                      hashlib.sha256).hexdigest()
    auth = esp_request(ip, "/auth",
                       data=json.dumps({"hmac": digest}).encode(),
                       headers={"Content-Type": "application/json"})
    if not auth.get("token"):
        raise RuntimeError("device rejected authentication")
    return auth["token"]


class Handler(BaseHTTPRequestHandler):
    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _ip(self):
        return parse_qs(urlparse(self.path).query).get("ip", [""])[0]

    def do_GET(self):
        if urlparse(self.path).path == "/ping":
            try:
                pong = esp_request(self._ip(), "/ping", timeout=4)
                self._json(200, {"ok": True, "fw": pong.get("fw")})
            except Exception as e:
                self._json(200, {"ok": False, "message": str(e)})
            return
        body = PAGE.encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if urlparse(self.path).path != "/upload":
            self._json(404, {"ok": False, "message": "no such endpoint"})
            return
        ip = self._ip()
        length = int(self.headers.get("Content-Length", 0))
        firmware = self.rfile.read(length)
        if not firmware:
            self._json(200, {"ok": False, "message": "empty upload"})
            return
        try:
            token = authenticate(ip)
            print(f"[ota] authenticated with {ip}, "
                  f"uploading {len(firmware)} bytes …")
            res = upload_firmware(ip, token, firmware)
            ok = bool(res.get("ok"))
            print(f"[ota] device replied: {res.get('message', res)}")
            self._json(200, {"ok": ok,
                             "message": res.get("message", "unknown reply")})
        except Exception as e:
            print(f"[ota] failed: {e}")
            self._json(200, {"ok": False, "message": str(e)})

    def log_message(self, *args):
        pass  # keep the console readable; the OTA prints above are enough


if __name__ == "__main__":
    # Localhost only, deliberately: this process holds the device secret,
    # so it must not be reachable by other machines on the LAN.
    server = HTTPServer(("127.0.0.1", PORT), Handler)
    print(f"OTA upload page: http://localhost:{PORT}  (Ctrl-C to stop)")
    server.serve_forever()
