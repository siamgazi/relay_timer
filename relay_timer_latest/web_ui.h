#pragma once

const char INDEX_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Relay & Pump Timer</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --r1:#0891b2;--r1-soft:rgba(8,145,178,0.12);--r1-glow:rgba(8,145,178,0.25);
  --r2:#7c3aed;--r2-soft:rgba(124,58,237,0.12);--r2-glow:rgba(124,58,237,0.25);
  --r3:#059669;--r3-soft:rgba(5,150,105,0.12);
  --p1:#b45309;--p1-soft:rgba(180,83,9,0.12);
  --on:#059669;--on-soft:rgba(5,150,105,0.14);--on-glow:rgba(5,150,105,0.28);
  --now:#e11d48;
  --glass-bg:rgba(255,255,255,0.78);
  --glass-border:rgba(15,23,42,0.09);
  --text:rgba(15,23,42,0.94);
  --muted:rgba(15,23,42,0.58);
  --label:rgba(15,23,42,0.42);
}
html{scroll-behavior:smooth;font-size:17px}
body{
  min-height:100vh;
  background:radial-gradient(ellipse at 20% 10%,#e0f2fe 0%,transparent 55%),
             radial-gradient(ellipse at 80% 90%,#ede9fe 0%,transparent 55%),
             #f6f7fb;
  font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',system-ui,sans-serif;
  color:var(--text);padding:0 16px 48px;font-size:16px;
}
.orb{position:fixed;border-radius:50%;filter:blur(60px);pointer-events:none;z-index:0;opacity:0.55}
.orb1{width:500px;height:500px;background:radial-gradient(circle,#bae6fd,transparent 70%);top:-120px;right:-160px}
.orb2{width:400px;height:400px;background:radial-gradient(circle,#ddd6fe,transparent 70%);bottom:-80px;left:-120px}
.orb3{width:280px;height:280px;background:radial-gradient(circle,#99f6e4,transparent 70%);top:45%;left:42%;transform:translate(-50%,-50%)}
.wrap{max-width:860px;margin:0 auto;position:relative;z-index:1;padding-top:28px}
.card{
  background:var(--glass-bg);
  backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);
  border:1px solid var(--glass-border);
  border-radius:20px;padding:22px 24px;margin-bottom:14px;
  box-shadow:0 4px 24px -8px rgba(15,23,42,0.08);
  transition:border-color .25s;
}
.card:hover{border-color:rgba(15,23,42,0.16)}
/* While the modal overlay is up, the cards are fully hidden behind it,
   so paying the GPU cost of backdrop-filter every frame is wasted — and
   it's what causes the "slow to close" feeling: the browser has to catch
   that blur back up in one burst the instant the overlay is removed. */
body.modal-open .card{backdrop-filter:none;-webkit-backdrop-filter:none}

/* Header */
.header{text-align:center;padding:36px 24px 28px;border-radius:24px}
.header .chip{
  display:inline-flex;align-items:center;gap:8px;
  background:rgba(15,23,42,0.05);border:1px solid var(--glass-border);
  border-radius:20px;padding:6px 16px;font-size:13px;color:var(--muted);
  letter-spacing:.06em;margin-bottom:20px;
}
.status-dot{width:8px;height:8px;border-radius:50%;background:#059669;flex-shrink:0;
  box-shadow:0 0 6px #059669;animation:blink 2.4s ease-in-out infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.35}}
.clock{
  font-size:clamp(46px,11vw,78px);font-weight:200;letter-spacing:.05em;line-height:1;
  background:linear-gradient(135deg,#0f172a 15%,#0891b2 55%,#059669 100%);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text;
  margin-bottom:10px;
}
.clock-sub{font-size:14px;color:var(--label);letter-spacing:.15em;text-transform:uppercase}

/* Relay Cards */
.relay-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;margin-bottom:14px}
@media(max-width:768px){.relay-grid{grid-template-columns:1fr}}
.r-card{position:relative;overflow:hidden;border-radius:20px;transition:box-shadow .35s,border-color .35s}
.r-card.on{
  border-color:rgba(16,185,129,0.25);
  box-shadow:0 0 40px -10px rgba(16,185,129,0.2),inset 0 0 40px -20px rgba(16,185,129,0.08);
}
.r-glow{position:absolute;inset:0;border-radius:20px;opacity:0;transition:opacity .4s;pointer-events:none;}
.r-card.on .r-glow{opacity:1}
.r-card-1 .r-glow{background:radial-gradient(circle at 15% 50%,var(--r1-soft),transparent 65%)}
.r-card-2 .r-glow{background:radial-gradient(circle at 85% 50%,var(--r2-soft),transparent 65%)}
.r-card-3 .r-glow{background:radial-gradient(circle at 50% 50%,var(--r3-soft),transparent 65%)}
.r-inner{position:relative;z-index:1;padding:20px 22px}
.r-top{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:14px}
.r-tag{font-size:12px;text-transform:uppercase;letter-spacing:.18em;color:var(--label)}
.r-pill{
  padding:4px 13px;border-radius:20px;font-size:13px;font-weight:700;letter-spacing:.08em;
  border:1px solid;transition:all .3s;
}
.r-pill.on{background:var(--on-soft);color:#047857;border-color:rgba(5,150,105,0.5);box-shadow:0 0 10px var(--on-glow)}
.r-pill.off{background:rgba(15,23,42,0.05);color:var(--label);border-color:rgba(15,23,42,0.12)}
.r-name{font-size:24px;font-weight:600;margin-bottom:4px}
.r-mode{font-size:13px;color:var(--muted);letter-spacing:.04em;margin-bottom:14px}
.r-cd{
  padding:10px 14px;background:rgba(15,23,42,0.045);border-radius:11px;
  font-size:14px;color:var(--muted);line-height:1.5;border:1px solid rgba(15,23,42,0.06);
}
.r-cd strong{color:#7c3aed;font-weight:600}

/* Manual override segmented control (relays + pumps) */
.seg{display:flex;gap:6px;margin-top:14px}
.seg button{
  flex:1;padding:9px 6px;border-radius:9px;font-size:13px;font-weight:700;
  letter-spacing:.04em;border:1px solid #000;
  background:#e5e7eb;color:#000;cursor:pointer;
  transition:all .18s;outline:none;
}
.seg button:hover{background:#d1d5db;color:#000}
.seg button.active{background:#059669;color:#fff;border-color:#000}

/* Pump cards */
.pump-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:14px}
@media(max-width:640px){.pump-grid{grid-template-columns:1fr}}
.pump-actual{font-size:14px;color:var(--muted);margin-top:2px;margin-bottom:2px}
.pump-inline{
  margin-top:14px;padding-top:14px;border-top:1px solid rgba(15,23,42,0.08);
  display:none;flex-direction:column;gap:10px;
}

/* Timeline & Common Items */
.sec-title{font-size:12px;text-transform:uppercase;letter-spacing:.2em;color:#000;margin-bottom:14px;font-weight:700}

/* Presets */
.preset-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px}
.preset-count{font-size:13px;color:var(--label);
  background:rgba(15,23,42,0.04);border:1px solid var(--glass-border);
  border-radius:20px;padding:3px 11px;}
.preset-list{display:flex;flex-direction:column;gap:7px}
.p-item{
  display:flex;align-items:center;gap:12px;
  padding:14px 16px;background:rgba(15,23,42,0.025);
  border-radius:13px;border:1px solid rgba(15,23,42,0.06);
  transition:border-color .2s,background .2s;
}
.p-item:hover{background:rgba(15,23,42,0.05);border-color:rgba(15,23,42,0.12)}
.p-item.disabled{opacity:.45}
.sw{position:relative;width:38px;height:21px;flex-shrink:0;display:inline-block}
.sw input{opacity:0;width:0;height:0;position:absolute}
.sw-track{
  position:absolute;inset:0;border-radius:21px;cursor:pointer;
  background:rgba(15,23,42,0.12);border:1px solid rgba(15,23,42,0.16);transition:.28s;
}
.sw-track::before{
  content:'';position:absolute;width:15px;height:15px;border-radius:50%;
  left:3px;top:2px;background:#fff;box-shadow:0 1px 3px rgba(15,23,42,0.3);transition:.28s;
}
.sw input:checked+.sw-track{background:rgba(124,58,237,0.55);border-color:rgba(124,58,237,0.7)}
.sw input:checked+.sw-track::before{transform:translateX(16px);background:#fff}
.p-info{flex:1;min-width:0}
.p-time{font-size:17px;font-weight:600;letter-spacing:.02em}
.p-meta{font-size:13px;color:var(--muted);margin-top:3px;display:flex;align-items:center;gap:7px;flex-wrap:wrap}
.p-tag{padding:3px 9px;border-radius:6px;font-size:12px;font-weight:700;letter-spacing:.07em;border:1px solid;}
.p-tag.relay{background:var(--r2-soft);color:#6d28d9;border-color:rgba(124,58,237,0.35)}
.p-tag.pump{background:var(--p1-soft);color:#92400e;border-color:rgba(180,83,9,0.35)}
.p-active-now{
  font-size:11px;font-weight:700;letter-spacing:.1em;text-transform:uppercase;
  color:#047857;background:var(--on-soft);border:1px solid rgba(5,150,105,0.3);
  border-radius:5px;padding:2px 7px;
}
.p-actions{display:flex;gap:6px;flex-shrink:0}
.p-btn{
  width:34px;height:34px;border-radius:8px;
  border:1px solid #000;background:rgba(15,23,42,0.03);
  color:var(--muted);cursor:pointer;display:flex;align-items:center;justify-content:center;
  font-size:15px;transition:all .18s;outline:none;
}
.p-btn:hover{background:rgba(15,23,42,0.09);color:var(--text)}
.btn-add{
  width:100%;margin-top:9px;padding:14px;border-radius:13px;
  border:1px dashed #000;background:rgba(15,23,42,0.02);
  color:var(--muted);cursor:pointer;font-size:14px;letter-spacing:.04em;
  transition:all .2s;outline:none;
}
.btn-add:hover{background:rgba(124,58,237,0.08);border-color:#000;color:#7c3aed}

/* Modals & Inputs */
.overlay{
  position:fixed;inset:0;
  background:rgba(15,23,42,0.45);
  z-index:200;display:none;align-items:center;justify-content:center;padding:16px;
}
.overlay.open{display:flex}
.modal{
  background:#ffffff;border:1px solid rgba(15,23,42,0.1);
  border-radius:22px;padding:28px;width:100%;max-width:420px;
  box-shadow:0 25px 70px rgba(15,23,42,0.25);
}
.modal-title{font-size:19px;font-weight:700;color:#000;margin-bottom:22px;letter-spacing:.02em}
.fg{margin-bottom:18px}
.fl{font-size:13px;font-weight:600;letter-spacing:.05em;color:var(--text);margin-bottom:7px;display:block;}
.fcol{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.fcol-3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px}
.fc{
  width:100%;padding:11px 14px;background:rgba(15,23,42,0.035);
  border:1px solid rgba(15,23,42,0.15);border-radius:10px;
  color:var(--text);font-size:15px;outline:none;transition:border-color .2s;
}
.fc:focus{border-color:rgba(124,58,237,0.55);box-shadow:0 0 0 3px rgba(124,58,237,0.12)}
select.fc option{background:#fff;color:#0f172a}
.fhint{font-size:13px;color:var(--muted);margin-top:6px;line-height:1.4}
.modal-acts{display:flex;gap:9px;justify-content:flex-end;margin-top:28px}
.btn-sec{
  padding:10px 20px;border-radius:10px;border:1px solid #000;
  background:rgba(15,23,42,0.03);color:var(--muted);
  font-size:15px;cursor:pointer;transition:all .2s;outline:none;
}
.btn-sec:hover{background:rgba(15,23,42,0.08);color:var(--text)}
.btn-primary{
  padding:11px 24px;border-radius:11px;border:1px solid #000;
  background:linear-gradient(135deg,#0891b2,#7c3aed);
  color:#fff;font-size:15px;font-weight:700;cursor:pointer;
  letter-spacing:.05em;transition:all .2s;outline:none;
}
</style>
</head>
<body>
<div class="orb orb1"></div><div class="orb orb2"></div><div class="orb orb3"></div>
<div class="wrap">
  <div class="card header">
    <div class="chip"><div class="status-dot" id="sdot"></div><span id="chip-txt">Connecting…</span></div>
    <div class="clock" id="clk">--:--:--</div>
    <div class="clock-sub" id="clk-sub">relay & pump timer</div>
  </div>

  <div class="relay-grid">
    <div class="card r-card r-card-1" id="rc1">
      <div class="r-glow"></div>
      <div class="r-inner">
        <div class="r-top"><span class="r-tag">CHANNEL 1</span><span class="r-pill off" id="r1pill">OFF</span></div>
        <div class="r-name" style="color:#000" id="r1name">Relay 1</div>
        <div class="fg" style="margin:6px 0 10px;">
          <select class="fc" id="r1nameSel" onchange="relayRename(1,this.value)"></select>
        </div>
        <div class="r-cd" id="r1cd">No active schedule</div>
        <div class="seg">
          <button type="button" id="r1seg-auto" onclick="setRelayMode(1,'auto')">Auto</button>
          <button type="button" id="r1seg-on" onclick="setRelayMode(1,'on')">On</button>
          <button type="button" id="r1seg-off" onclick="setRelayMode(1,'off')">Off</button>
        </div>
      </div>
    </div>
    
    <div class="card r-card r-card-2" id="rc2">
      <div class="r-glow"></div>
      <div class="r-inner">
        <div class="r-top">
          <span class="r-tag">CHANNEL 2</span>
          <div style="display:flex;align-items:center;gap:8px;">
            <label class="sw" title="R2 Depends on R1" style="transform:scale(0.8);margin:0;">
              <input type="checkbox" id="r2dep-sw" onchange="toggleR2Dep(this.checked)">
              <span class="sw-track"></span>
            </label>
            <span class="r-pill off" id="r2pill">OFF</span>
          </div>
        </div>
        <div class="r-name" style="color:#000" id="r2name">Relay 2</div>
        <div class="fg" style="margin:6px 0 10px;">
          <select class="fc" id="r2nameSel" onchange="relayRename(2,this.value)"></select>
        </div>
        <div class="r-cd" id="r2cd">Depends on Relay 1</div>
        <div class="seg">
          <button type="button" id="r2seg-auto" onclick="setRelayMode(2,'auto')">Auto</button>
          <button type="button" id="r2seg-on" onclick="setRelayMode(2,'on')">On</button>
          <button type="button" id="r2seg-off" onclick="setRelayMode(2,'off')">Off</button>
        </div>
      </div>
    </div>

    <div class="card r-card r-card-3" id="rc3">
      <div class="r-glow"></div>
      <div class="r-inner">
        <div class="r-top"><span class="r-tag">CHANNEL 3</span><span class="r-pill off" id="r3pill">OFF</span></div>
        <div class="r-name" style="color:#000" id="r3name">Relay 3</div>
        <div class="fg" style="margin:6px 0 10px;">
          <select class="fc" id="r3nameSel" onchange="relayRename(3,this.value)"></select>
        </div>
        <div class="r-cd" id="r3cd">Independent</div>
        <div class="seg">
          <button type="button" id="r3seg-auto" onclick="setRelayMode(3,'auto')">Auto</button>
          <button type="button" id="r3seg-on" onclick="setRelayMode(3,'on')">On</button>
          <button type="button" id="r3seg-off" onclick="setRelayMode(3,'off')">Off</button>
        </div>
      </div>
    </div>
  </div>

  <div class="card">
    <div class="sec-title">Pumps &mdash; Manual Control</div>
    <div class="pump-grid" id="pumpGrid"></div>
  </div>

  <div class="card">
    <div class="preset-header">
      <div class="sec-title" style="margin-bottom:0">Timer Schedules</div>
      <span class="preset-count" id="pcnt">0 / 8</span>
    </div>
    <div class="preset-list" id="plist"></div>
    <button type="button" class="btn-add" id="badd" onclick="openModal()">＋ &nbsp;Create New Schedule</button>
  </div>
</div>

<div class="overlay" id="ov">
  <div class="modal">
    <div class="modal-title" id="modal-ttl">Setup Schedule</div>
    <input type="hidden" id="ei" value="-1">
    
    <div class="fg">
      <label class="fl">What do you want to control?</label>
      <select class="fc" id="ftarget" onchange="togglePumpFields()">
        <option value="0">Relay 1</option>
        <option value="1">Relay 2 (Depends on R1)</option>
        <option value="2">Relay 3 (Independent)</option>
        <option value="3">Pump 1 (RS485)</option>
        <option value="4">Pump 2 (RS485)</option>
        <option value="5">Pump 3 (RS485)</option>
        <option value="6">Pump 4 (RS485)</option>
      </select>
      <div class="fhint">Select the hardware device you want this timer to activate.</div>
    </div>

    <div class="fg" id="pumpOpts" style="display:none;">
      <label class="fl">Pump Settings</label>
      <div class="fcol">
        <select class="fc" id="fptype">
          <option value="0">Pentair</option>
          <option value="1">Emaux</option>
          <option value="2">Black &amp; Decker</option>
        </select>
        <input class="fc" type="number" id="fspeed" min="800" max="3450" placeholder="Speed (RPM)">
      </div>
    </div>

    <div class="fg">
      <label class="fl">When should it turn on?</label>
      <div class="fcol-3">
        <input class="fc" type="number" id="fsh" min="1" max="12" placeholder="Hour (1-12)">
        <input class="fc" type="number" id="fsm" min="0" max="59" placeholder="Min (0-59)">
        <select class="fc" id="fampm">
          <option value="AM">AM</option>
          <option value="PM">PM</option>
        </select>
      </div>
    </div>

    <div class="fg">
      <label class="fl">How long should it run?</label>
      <div class="fcol">
        <input class="fc" type="number" id="fdh" min="0" max="23" placeholder="Hours">
        <input class="fc" type="number" id="fdm" min="0" max="59" placeholder="Minutes">
      </div>
      <div class="fhint">Set the exact duration the device should remain active.</div>
    </div>

    <div class="fg">
      <label class="fl">Enable this schedule?</label>
      <select class="fc" id="fen">
        <option value="1">Yes, turn it ON</option>
        <option value="0">No, keep it OFF for now</option>
      </select>
    </div>

    <div class="modal-acts">
      <button type="button" class="btn-sec" onclick="closeModal()">Cancel</button>
      <button type="button" class="btn-primary" onclick="savePreset()">Save Schedule</button>
    </div>
  </div>
</div>

<script>
var G=null;
const T_LABELS = ["Relay 1", "Relay 2", "Relay 3", "Pump 1", "Pump 2", "Pump 3", "Pump 4"];
const P_TYPES = ["Pentair", "Emaux", "Black & Decker"];
const MAX_PUMPS_UI = 4;
const MAX_RELAYS_UI = 3;
// Master name pool shared by relays AND pumps -- a name picked anywhere
// (any relay or any pump) is removed from every other dropdown until
// freed. SPA LITE and POOL LITE (indices 2,3) are relay-only.
const NAME_OPTIONS = ["MAIN PUMP","PSWP PUMP","SPA LITE","POOL LITE","FTN PUMP1","FTN PUMP2","OTHER1","OTHER2","CLNR PUMP"];
function nameAllowedForPump(idx){ return idx !== 2 && idx !== 3; }
let phoneTimeSynced = false;

var PUMP_COLORS = ['#000','#000','#000','#000'];

// Tracks which pumps currently have their inline speed/type picker open
// because the user clicked "On" but hasn't hit "Turn On" yet. The poll
// loop re-renders every 2s from server state, and the server doesn't
// know about this in-progress choice (nothing has been sent yet), so
// without this flag renderPumps() would think mode !== 'on' and slam
// the picker shut out from under the user.
var pumpPickerOpen = {1:false,2:false,3:false,4:false};

function buildPumpCards(){
  var grid = document.getElementById('pumpGrid');
  var html = '';
  for (var i = 1; i <= MAX_PUMPS_UI; i++) {
    html += ''
      + '<div class="card r-card" id="pc'+i+'">'
      + '  <div class="r-inner">'
      + '    <div class="r-top"><span class="r-tag">PUMP '+i+'</span><span class="r-pill off" id="p'+i+'pill">OFF</span></div>'
      + '    <div class="r-name" style="color:'+PUMP_COLORS[i-1]+'" id="p'+i+'name">Pump '+i+'</div>'
      + '    <div class="fg" style="margin:6px 0 10px;">'
      + '      <select class="fc" id="p'+i+'nameSel" onchange="pumpRename('+i+',this.value)"></select>'
      + '    </div>'
      + '    <div class="pump-actual" id="p'+i+'actual">-- RPM</div>'
      + '    <div class="r-cd" id="p'+i+'cd" style="margin-bottom:10px;">No active schedule</div>'
      + '    <div class="seg">'
      + '      <button type="button" id="p'+i+'seg-auto" onclick="pumpSetMode('+i+',\'auto\')">Auto</button>'
      + '      <button type="button" id="p'+i+'seg-on" onclick="pumpSetMode('+i+',\'on\')">On</button>'
      + '      <button type="button" id="p'+i+'seg-off" onclick="pumpSetMode('+i+',\'off\')">Off</button>'
      + '    </div>'
      + '    <div class="pump-inline" id="p'+i+'inline">'
      + '      <div class="fcol">'
      + '        <select class="fc" id="p'+i+'type"><option value="0">Pentair</option><option value="1">Emaux</option>'+(i===1?'<option value="2">Black & Decker</option>':'')+'</select>'
      + '        <input class="fc" type="number" id="p'+i+'speed" placeholder="Speed (RPM)">'
      + '      </div>'
      + '      <div style="display:flex;gap:8px;">'
      + '        <button type="button" class="btn-sec" style="flex:1" onclick="pumpCancel('+i+')">Cancel</button>'
      + '        <button type="button" class="btn-primary" style="flex:1" onclick="pumpApply('+i+')">Turn On</button>'
      + '      </div>'
      + '    </div>'
      + '  </div>'
      + '</div>';
  }
  grid.innerHTML = html;
}

function setRelayMode(relay, mode){
  fetch('/api/manual', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({relay: relay, mode: mode})
  });
}

function pumpSetMode(pump, mode){
  if (mode === 'on') {
    // Reveal the inline speed/type picker instead of firing immediately —
    // the pump needs a speed before it can actually be told to run.
    // Nothing is sent to the server yet, so mark this pump as "picker
    // open" locally — otherwise the next poll (server still reports the
    // old mode) would think this picker shouldn't be showing and hide it.
    pumpPickerOpen[pump] = true;
    document.getElementById('p'+pump+'inline').style.display = 'flex';
    return;
  }
  pumpPickerOpen[pump] = false;
  document.getElementById('p'+pump+'inline').style.display = 'none';
  fetch('/api/pumpmanual', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({pump: pump, mode: mode})
  });
}

function pumpCancel(pump){
  pumpPickerOpen[pump] = false;
  document.getElementById('p'+pump+'inline').style.display = 'none';
  // Snap the segmented control back to whatever the server actually has.
  if (G && G.pumps && G.pumps[pump-1]) {
    setSegActive('p'+pump+'seg', G.pumps[pump-1].mode);
  }
}

function pumpApply(pump){
  var pumpType = parseInt(document.getElementById('p'+pump+'type').value);
  var speed = parseInt(document.getElementById('p'+pump+'speed').value || 0);
  if (!speed) { alert('Enter a speed (RPM) first'); return; }
  pumpPickerOpen[pump] = false;
  document.getElementById('p'+pump+'inline').style.display = 'none';
  fetch('/api/pumpmanual', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({pump: pump, mode: 'on', pumpType: pumpType, speed: speed})
  }).then(function(r){ return r.json(); }).then(function(res){
    if (res && res.error) alert(res.error);
  }).catch(function(){});
}

function pad2(n){return String(n).padStart(2,'0')}

// Returns the display label for pump i (1-based): its assigned custom
// name if set, otherwise the default "PUMPi".
function pumpLabel(i, pd){
  var idx = pd ? pd.nameIndex : -1;
  return (idx != null && idx >= 0 && idx < NAME_OPTIONS.length) ? NAME_OPTIONS[idx] : ('PUMP'+i);
}

// Returns the display label for relay i (1-based): its assigned custom
// name if set, otherwise the default "Relay i".
function relayLabel(i, nameIdx){
  return (nameIdx != null && nameIdx >= 0 && nameIdx < NAME_OPTIONS.length) ? NAME_OPTIONS[nameIdx] : ('Relay '+i);
}

// Tracks a rename that's been picked locally but not yet confirmed by the
// server (pumpNum -> nameIndex, or null if nothing pending). While a
// pump has a pending value, it overrides whatever the latest /api/status
// poll reports for that pump -- this is what makes renaming race-proof:
// a GET that was already in flight before the rename was sent can no
// longer stomp the just-picked name back to its old value when it lands.
var pumpNamePending = {1:null, 2:null, 3:null, 4:null};
// Same idea, for relays.
var relayNamePending = {1:null, 2:null, 3:null};

// Builds the <option> list for a rename dropdown. The name pool is
// shared globally across relays AND pumps, so a name held by ANY of
// them (other than the entity being edited) is excluded here.
// `selfIsPump`/`selfNum` identify the entity being edited (excluded from
// its own "taken" check). `pumpEffective`/`relayEffective` are the
// current (server-confirmed or locally-pending) nameIndex arrays for
// all 4 pumps / all 3 relays. `defaultLabel` is the "no custom name"
// option's text. `pumpOnly` restricts the list to pump-eligible names
// (excludes SPA LITE/POOL LITE) when true.
function buildNameOptionsHTML(selfIsPump, selfNum, pumpEffective, relayEffective, defaultLabel, pumpOnly){
  var taken = {};
  for (var j = 0; j < MAX_PUMPS_UI; j++) {
    if (selfIsPump && j === selfNum - 1) continue;
    var nm = pumpEffective[j];
    if (nm != null && nm >= 0) taken[nm] = true;
  }
  for (var k = 0; k < MAX_RELAYS_UI; k++) {
    if (!selfIsPump && k === selfNum - 1) continue;
    var nm2 = relayEffective[k];
    if (nm2 != null && nm2 >= 0) taken[nm2] = true;
  }
  var html = '<option value="-1">'+defaultLabel+'</option>';
  for (var i = 0; i < NAME_OPTIONS.length; i++) {
    if (taken[i]) continue;
    if (pumpOnly && !nameAllowedForPump(i)) continue;
    html += '<option value="'+i+'">'+NAME_OPTIONS[i]+'</option>';
  }
  return html;
}

function pumpRename(pumpNum, val){
  var nameIndex = parseInt(val);
  pumpNamePending[pumpNum] = nameIndex;
  if (G) render(G); // reflect the pick immediately, independent of polling (also refreshes relay dropdowns, since the pool is shared)

  fetch('/api/pumpname', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({pump: pumpNum, nameIndex: nameIndex})
  }).then(function(r){ return r.json(); }).then(function(res){
    if (res && res.error) {
      alert(res.error);
      // Only clear if nothing newer has superseded this pick in the
      // meantime (user may have already changed their mind again).
      if (pumpNamePending[pumpNum] === nameIndex) {
        pumpNamePending[pumpNum] = null;
        if (G) render(G);
      }
    }
    // On success we deliberately leave pumpNamePending set -- the next
    // /api/status poll will report the new value and render() will
    // clear it then. Clearing it here instead would reopen the exact
    // race this is meant to close: a poll already in flight when this
    // request was sent could still land with the old value and stomp it.
  }).catch(function(){
    if (pumpNamePending[pumpNum] === nameIndex) {
      pumpNamePending[pumpNum] = null;
      alert('Could not save pump name — check the connection and try again.');
      if (G) render(G);
    }
  });
}

function relayRename(relayNum, val){
  var nameIndex = parseInt(val);
  relayNamePending[relayNum] = nameIndex;
  if (G) render(G); // reflect the pick immediately, independent of polling (also refreshes pump dropdowns, since the pool is shared)

  fetch('/api/relayname', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({relay: relayNum, nameIndex: nameIndex})
  }).then(function(r){ return r.json(); }).then(function(res){
    if (res && res.error) {
      alert(res.error);
      if (relayNamePending[relayNum] === nameIndex) {
        relayNamePending[relayNum] = null;
        if (G) render(G);
      }
    }
    // On success we leave relayNamePending set until the next poll
    // confirms it, for the same race-proofing reason as pumpRename().
  }).catch(function(){
    if (relayNamePending[relayNum] === nameIndex) {
      relayNamePending[relayNum] = null;
      alert('Could not save relay name — check the connection and try again.');
      if (G) render(G);
    }
  });
}

function format12(h, m) {
  var ampm = h >= 12 ? 'PM' : 'AM';
  var h12 = h % 12;
  h12 = h12 ? h12 : 12;
  return h12 + ':' + pad2(m) + ' ' + ampm;
}
function format12Sec(h, m, s) {
  var ampm = h >= 12 ? 'PM' : 'AM';
  var h12 = h % 12;
  h12 = h12 ? h12 : 12;
  return h12 + ':' + pad2(m) + ':' + pad2(s) + ' ' + ampm;
}

function fmtMins(m){
  if(m<=0)return'0m';
  var h=Math.floor(m/60),mn=m%60;
  if(h&&mn)return h+'h '+pad2(mn)+'m';
  return h?h+'h':mn+'m';
}

async function poll(){
  try{
    var r=await fetch('/api/status');
    if(!r.ok)throw 0;
    G=await r.json();
    
    if (!phoneTimeSynced) {
      var now = new Date();
      fetch('/api/settime', {
        method: 'POST', 
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ hour: now.getHours(), minute: now.getMinutes() })
      });
      phoneTimeSynced = true;
    }

    render(G);
  }catch(e){
    document.getElementById('chip-txt').textContent='Reconnecting…';
  } finally {
    setTimeout(poll, 2000); 
  }
}

// Reconciles any pending pump/relay renames against the latest poll, then
// returns {pumps:[4], relays:[3]} — the "effective" nameIndex for every
// pump and relay (locally-pending pick if one is outstanding, otherwise
// whatever the server just reported). Computed once per poll and shared
// by both the relay cards and the pump cards, since the two dropdowns
// pull from one global pool and each needs to know what the other holds.
function computeEffectiveNames(d){
  for (var i = 1; i <= MAX_PUMPS_UI; i++) {
    var pend = pumpNamePending[i];
    if (pend !== null && pend !== undefined) {
      var serverVal = (d.pumps && d.pumps[i-1]) ? d.pumps[i-1].nameIndex : -1;
      if (serverVal === pend) pumpNamePending[i] = null;
    }
  }
  for (var r = 1; r <= MAX_RELAYS_UI; r++) {
    var rpend = relayNamePending[r];
    if (rpend !== null && rpend !== undefined) {
      var serverVal2 = d['relay'+r+'NameIndex'];
      if (serverVal2 === undefined) serverVal2 = -1;
      if (serverVal2 === rpend) relayNamePending[r] = null;
    }
  }

  var effPumps = [];
  for (var j = 0; j < MAX_PUMPS_UI; j++) {
    var sIdx = (d.pumps && d.pumps[j]) ? d.pumps[j].nameIndex : -1;
    var pend2 = pumpNamePending[j+1];
    effPumps.push((pend2 !== null && pend2 !== undefined) ? pend2 : sIdx);
  }
  var effRelays = [];
  for (var k = 0; k < MAX_RELAYS_UI; k++) {
    var sIdx2 = d['relay'+(k+1)+'NameIndex'];
    if (sIdx2 === undefined) sIdx2 = -1;
    var pend3 = relayNamePending[k+1];
    effRelays.push((pend3 !== null && pend3 !== undefined) ? pend3 : sIdx2);
  }
  return {pumps: effPumps, relays: effRelays};
}

function render(d){
  document.getElementById('clk').textContent = format12Sec(d.hour, d.minute, d.second);
  document.getElementById('chip-txt').textContent='Connected · '+d.ip;
  
  var c1=document.getElementById('rc1'), c2=document.getElementById('rc2'), c3=document.getElementById('rc3');
  
  if(d.relay1){c1.classList.add('on');document.getElementById('r1pill').textContent='ON';}else{c1.classList.remove('on');document.getElementById('r1pill').textContent='OFF';}
  if(d.relay2){c2.classList.add('on');document.getElementById('r2pill').textContent='ON';}else{c2.classList.remove('on');document.getElementById('r2pill').textContent='OFF';}
  if(d.relay3){c3.classList.add('on');document.getElementById('r3pill').textContent='ON';}else{c3.classList.remove('on');document.getElementById('r3pill').textContent='OFF';}
  
  document.getElementById('r2dep-sw').checked = d.r2Depends;

  var eff = computeEffectiveNames(d);

  for (var i = 1; i <= MAX_RELAYS_UI; i++) {
    var label = relayLabel(i, eff.relays[i-1]);
    document.getElementById('r'+i+'name').textContent = label;
    var sel = document.getElementById('r'+i+'nameSel');
    if (sel) {
      sel.innerHTML = buildNameOptionsHTML(false, i, eff.pumps, eff.relays, 'Relay '+i, false);
      sel.value = String(eff.relays[i-1]);
    }
    T_LABELS[i-1] = label;
  }

  document.getElementById('r2cd').innerHTML = d.r2Depends ? 'Depends on Relay 1' : 'Independent';
  var opt0 = document.querySelector('#ftarget option[value="0"]');
  if (opt0) opt0.textContent = T_LABELS[0];
  var opt1 = document.querySelector('#ftarget option[value="1"]');
  if (opt1) opt1.textContent = T_LABELS[1] + (d.r2Depends ? " (Depends on R1)" : "");
  var opt2 = document.querySelector('#ftarget option[value="2"]');
  if (opt2) opt2.textContent = T_LABELS[2] + " (Independent)";

  setSegActive('r1seg', d.relay1Mode);
  setSegActive('r2seg', d.relay2Mode);
  setSegActive('r3seg', d.relay3Mode);

  renderPumps(d, eff);
  renderPresets(d.presets,d.hour,d.minute);
}

function setSegActive(prefix, mode){
  ['auto','on','off'].forEach(function(md){
    var btn = document.getElementById(prefix+'-'+md);
    if (btn) btn.classList.toggle('active', mode === md);
  });
}

function fmtScheduleCd(cd){
  if (!cd) return 'No active schedule';
  if (cd.active) return '<strong>Schedule active</strong> &middot; ' + fmtMins(cd.minsLeft) + ' left';
  if (cd.minsUntilNext >= 0) return 'Next run in ' + fmtMins(cd.minsUntilNext);
  return 'No active schedule';
}

function renderPumps(d, eff){
  var pumps = d.pumps;
  if (!pumps) return;

  // eff.{pumps,relays} was already computed once in render() (pending
  // renames reconciled, effective indices for the whole shared name pool).
  var effective = eff.pumps;

  for (var i = 1; i <= MAX_PUMPS_UI; i++) {
    var pd = pumps[i-1] || {mode:'auto',active:false,speed:0,nameIndex:-1};
    var pill = document.getElementById('p'+i+'pill');
    var card = document.getElementById('pc'+i);
    if (!pill || !card) continue;

    if (pd.active) {
      card.classList.add('on'); pill.textContent = 'ON';
      pill.classList.add('on'); pill.classList.remove('off');
    } else {
      card.classList.remove('on'); pill.textContent = 'OFF';
      pill.classList.remove('on'); pill.classList.add('off');
    }
    document.getElementById('p'+i+'actual').textContent = pd.active ? (pd.speed + ' RPM') : '-- RPM';

    var effIdx = effective[i-1];
    var label = pumpLabel(i, {nameIndex: effIdx});
    document.getElementById('p'+i+'name').textContent = label;

    var sel = document.getElementById('p'+i+'nameSel');
    if (sel) {
      sel.innerHTML = buildNameOptionsHTML(true, i, eff.pumps, eff.relays, 'PUMP'+i, true);
      sel.value = String(effIdx);
    }

    // Target index for pump i is TGT_PUMP1(3) + (i-1), i.e. i+2 — this is
    // the same "cdN" schedule countdown the backend already computes for
    // relays, just never wired up here before. It shows whether a timer
    // *wants* this pump running, independently of whether the pump has
    // physically acknowledged over RS485 yet (that's what pd.active/ON
    // reflects, and can lag behind while the hardware handshake retries).
    var cdEl = document.getElementById('p'+i+'cd');
    if (cdEl) cdEl.innerHTML = fmtScheduleCd(d['cd'+(i+2)]);

    setSegActive('p'+i+'seg', pd.mode);

    // Keep the inline speed picker visible while actively choosing "On"
    // (either the user has it open locally, or the server has confirmed
    // manual-on and is just waiting for us to reflect it).
    if (pd.mode !== 'on' && !pumpPickerOpen[i]) {
      document.getElementById('p'+i+'inline').style.display = 'none';
    }

    // Keep the preset-target dropdown and schedule tags showing this
    // pump's current friendly name instead of the plain "Pump i".
    T_LABELS[2+i] = label;
    var opt = document.querySelector('#ftarget option[value="'+(2+i)+'"]');
    if (opt) opt.textContent = label + ' (RS485)';
  }
}

function togglePumpFields(){
  var t = parseInt(document.getElementById('ftarget').value);
  var isPump = t >= 3;
  document.getElementById('pumpOpts').style.display = isPump ? 'block' : 'none';
  // Black & Decker is pump-1-only (the pump is fixed at Modbus address 1),
  // so the option is selectable only while the target is Pump 1 (index 3).
  var sel = document.getElementById('fptype');
  var bd = sel ? sel.querySelector('option[value="2"]') : null;
  if (bd) {
    bd.disabled = (t !== 3);
    if (t !== 3 && sel.value === '2') sel.value = '0';
  }
}

function renderPresets(presets,h,m){
  var curMin=h*60+m;
  var list=document.getElementById('plist');
  var used=presets.filter(function(p){return p.used});
  document.getElementById('pcnt').textContent=used.length+' / 8';
  
  if(used.length===0){list.innerHTML='<div style="text-align:center;padding:20px;opacity:0.5;">No schedules active.</div>';return;}
  list.innerHTML='';
  
  presets.forEach(function(p,i){
    if(!p.used)return;
    var sMin=p.startHour*60+p.startMinute;
    var eMin=sMin+(p.durHour*60+p.durMinute);
    var eh=Math.floor(eMin/60)%24,em=eMin%60;
    var isNow=(eMin<=1440)?(curMin>=sMin&&curMin<eMin):(curMin>=sMin||curMin<(eMin-1440));
    
    var tClass = p.target < 3 ? 'relay' : 'pump';
    var extra = p.target >= 3 ? ` &nbsp;·&nbsp; <strong>${p.speed} RPM</strong> (${P_TYPES[p.pumpType]})` : '';
    
    var pTimeStr = format12(p.startHour, p.startMinute) + ' &rarr; ' + format12(eh, em);

    var row=document.createElement('div');
    row.className='p-item'+(p.enabled?'':' disabled');
    row.innerHTML=`
      <label class="sw"><input type="checkbox" ${p.enabled?'checked':''} onchange="toggleP(${i},this.checked)"><span class="sw-track"></span></label>
      <div class="p-info">
        <div class="p-time">${pTimeStr}</div>
        <div class="p-meta">
          <span class="p-tag ${tClass}">${T_LABELS[p.target]}</span>
          ${pad2(p.durHour)}h ${pad2(p.durMinute)}m ${extra}
          ${isNow&&p.enabled?'&nbsp;<span class="p-active-now">ACTIVE NOW</span>':''}
        </div>
      </div>
      <div class="p-actions">
        <button type="button" class="p-btn" onclick="editP(${i})">&#9998;</button>
        <button type="button" class="p-btn del" onclick="delP(${i})">&times;</button>
      </div>`;
    list.appendChild(row);
  });
}

function openModal(preset,idx){
  document.getElementById('ei').value=idx>=0?idx:-1;
  if(preset){
    document.getElementById('ftarget').value=preset.target;
    document.getElementById('fptype').value=preset.pumpType||0;
    document.getElementById('fspeed').value=preset.speed||2000;
    
    var sh12 = preset.startHour % 12;
    document.getElementById('fsh').value = sh12 ? sh12 : 12;
    document.getElementById('fsm').value = preset.startMinute;
    document.getElementById('fampm').value = preset.startHour >= 12 ? 'PM' : 'AM';
    
    document.getElementById('fdh').value=preset.durHour;
    document.getElementById('fdm').value=preset.durMinute;
    document.getElementById('fen').value=preset.enabled?'1':'0';
  }else{
    document.getElementById('ftarget').value='0';
    document.getElementById('fspeed').value='2000';
    document.getElementById('fsh').value='6';
    document.getElementById('fsm').value='0';
    document.getElementById('fampm').value='AM';
    document.getElementById('fdh').value='1';
    document.getElementById('fdm').value='0';
    document.getElementById('fen').value='1';
  }
  togglePumpFields();
  
  // Drop the expensive background blur while it's hidden behind the
  // overlay — no visual difference, but it means there's nothing for
  // the browser to "catch up on" when the modal closes.
  document.body.classList.add('modal-open');

  // Instant open setup
  document.getElementById('ov').style.display = 'flex';
  requestAnimationFrame(function() {
    document.getElementById('ov').classList.add('open');
  });
}

// INSTANT MODAL DISMISSAL (GUARANTEED NO LAG)
function closeModal(){
  document.getElementById('ov').classList.remove('open');
  document.getElementById('ov').style.display = 'none';
  // Re-enable the background blur AFTER the modal is already gone, on
  // the next frame, so that (comparatively expensive) repaint never
  // blocks or delays the modal's own close.
  requestAnimationFrame(function(){
    document.body.classList.remove('modal-open');
  });
}

function editP(idx){if(G)openModal(G.presets[idx],idx)}

// TRUE OPTIMISTIC ZERO-DELAY SAVE
function savePreset() {
  // 1. Force the layout engine to immediately strip down the overlay window graphics
  closeModal();

  // 2. Queue data structure changes and requests to execute after frame rendering drops completely clear
  requestAnimationFrame(function() {
    setTimeout(function() {
      var idx = parseInt(document.getElementById('ei').value);
      var sh12 = parseInt(document.getElementById('fsh').value || 12);
      var ampm = document.getElementById('fampm').value;
      var sh24 = sh12 % 12; 
      if (ampm === 'PM') sh24 += 12;

      var presetData = {
        used: true,
        target: parseInt(document.getElementById('ftarget').value),
        pumpType: parseInt(document.getElementById('fptype').value),
        speed: parseInt(document.getElementById('fspeed').value || 0),
        startHour: sh24,
        startMinute: parseInt(document.getElementById('fsm').value || 0),
        durHour: parseInt(document.getElementById('fdh').value || 0),
        durMinute: parseInt(document.getElementById('fdm').value || 0),
        enabled: document.getElementById('fen').value === '1'
      };

      if (G && G.presets) {
        if (idx >= 0) {
          G.presets[idx] = presetData;
        } else {
          var emptyIdx = G.presets.findIndex(function(p) { return !p.used; });
          if (emptyIdx >= 0) G.presets[emptyIdx] = presetData;
          else G.presets.push(presetData);
        }
        render(G); 
      }

      var payload = Object.assign({ action: idx >= 0 ? 'update' : 'add', index: idx }, presetData);
      
      fetch('/api/preset', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      }).catch(function(e) {
        console.error("Background save failed:", e);
      });
    }, 0);
  });
}

// COMPLETE FIRE AND FORGET FOR SLIDERS
function toggleP(idx,en){
  if (G && G.presets && G.presets[idx]) {
    G.presets[idx].enabled = en;
    render(G);
  }
  
  requestAnimationFrame(function() {
    setTimeout(function() {
      fetch('/api/preset',{
        method:'POST',
        headers:{'Content-Type':'application/json'},
        body:JSON.stringify({action:'toggle',index:idx,enabled:en})
      });
    }, 0);
  });
}

// ANTI-FREEZE LAYOUT CODE FOR CONFIRMATION HANDLING
function delP(idx){
  // 1. Let the system capture dialogue selection input natively
  var confirmation = confirm('Delete schedule?');
  
  if(confirmation){
    // 2. Instantly purge the graphic layout locally so the row disappears immediately
    if (G && G.presets && G.presets[idx]) {
      G.presets[idx].used = false;
      render(G);
    }

    // 3. Defer background network payload transmission until AFTER confirmation dialog rendering loop clears completely out of threads
    requestAnimationFrame(function() {
      setTimeout(function() {
        fetch('/api/preset',{
          method:'POST',
          headers:{'Content-Type':'application/json'},
          body:JSON.stringify({action:'delete',index:idx})
        });
      }, 0);
    });
  }
}

function toggleR2Dep(val){
  if(G) {
    G.r2Depends = val;
    render(G);
  }
  requestAnimationFrame(function() {
    setTimeout(function() {
      fetch('/api/config', {
        method: 'POST', 
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({r2Depends: val})
      });
    }, 0);
  });
}

buildPumpCards();
poll(); 
</script>
</body>
</html>
)html";

const char SETUP_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wi-Fi Setup</title>
<style>
  body { background: linear-gradient(135deg,#e0f2fe,#ede9fe); color: #0f172a; font-family: sans-serif; display: flex; align-items: center; justify-content: center; height: 100vh; margin: 0; }
  .card { background: #ffffff; padding: 30px; border-radius: 15px; border: 1px solid rgba(15,23,42,0.08); box-shadow: 0 20px 50px rgba(15,23,42,0.12); width: 90%; max-width: 350px; text-align: center; }
  h2 { margin-top: 0; color: #7c3aed; }
  select, input { width: 100%; padding: 12px; margin: 10px 0 20px; border-radius: 8px; border: 1px solid rgba(15,23,42,0.16); background: #f8fafc; color: #0f172a; box-sizing: border-box; }
  button { width: 100%; padding: 12px; background: #7c3aed; color: #fff; border: 1px solid #000; border-radius: 8px; font-weight: bold; cursor: pointer; }
  button:hover { background: #6d28d9; }
</style>
</head>
<body>
  <div class="card">
    <h2>Device Setup</h2>
    <p style="font-size:12px; color:#64748b; margin-bottom:20px;">Connect to your home Wi-Fi</p>
    <select id="ssid"></select>
    <input type="password" id="pass" placeholder="Wi-Fi Password">
    <button type="button" onclick="save()">Connect & Restart</button>
  </div>
  <script>
    fetch('/api/scan').then(r=>r.json()).then(data=>{
      let sel = document.getElementById('ssid');
      if(data.length === 0) sel.innerHTML = '<option>No networks found</option>';
      data.forEach(n => {
        let opt = document.createElement('option');
        opt.value = opt.textContent = n.ssid;
        sel.appendChild(opt);
      });
    });
    function save(){
      let s=document.getElementById('ssid').value;
      let p=document.getElementById('pass').value;
      if(!s) return alert('Select a network');
      document.querySelector('button').innerText = "Rebooting...";
      fetch('/api/save_wifi', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({ssid:s, pass:p})});
    }
  </script>
</body>
</html>
)html";
