/*
  ESP32 RADAR SCANNER — RED HACKING THEME
  Developed by sreenand
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define USE_ACCESS_POINT 1

static const char AP_SSID[] = "ESP32_RADAR";
static const char AP_PASSWORD[] = "radar12345";
static const uint8_t AP_CHANNEL = 1;
static const uint8_t AP_MAX_CLIENTS = 4;

static const uint32_t WIFI_CYCLE_INTERVAL_MS = 14000;
static const uint32_t WIFI_PASSIVE_DWELL_MS = 80;
static const uint32_t BLE_SCAN_SECONDS = 3;
static const uint32_t DEVICE_EXPIRATION_MS = 70000;
static const uint32_t EXPIRATION_CHECK_MS = 1000;

static const uint8_t MAX_WIFI_DEVICES = 20;
static const uint8_t MAX_BLE_DEVICES = 20;
static const size_t DEVICE_JSON_CAPACITY = 8192;

struct DeviceRecord {
  char mac[18];
  char name[33];
  char security[17];
  int16_t rssi;
  int8_t channel;
  uint32_t lastSeenMs;
};

struct JsonWriter {
  char *data;
  size_t capacity;
  size_t length;
  bool ok;
};

static DeviceRecord wifiDevices[MAX_WIFI_DEVICES];
static DeviceRecord bleDevices[MAX_BLE_DEVICES];
static DeviceRecord wifiDeviceSnapshot[MAX_WIFI_DEVICES];
static DeviceRecord bleDeviceSnapshot[MAX_BLE_DEVICES];
static uint8_t wifiDeviceCount = 0;
static uint8_t bleDeviceCount = 0;
static uint32_t dataRevision = 1;
static portMUX_TYPE deviceMux = portMUX_INITIALIZER_UNLOCKED;

enum ScanPhase : uint8_t { PHASE_IDLE, PHASE_WIFI, PHASE_BLE };
static ScanPhase scanPhase = PHASE_IDLE;
static BLEScan *bleScanner = nullptr;
static volatile bool bleScanFinished = false;
static bool accessPointReady = false;
static bool bleReady = false;

static const char *wifiState = "STARTING";
static const char *bleState = "STARTING";
static uint32_t nextCycleAtMs = 0;
static uint32_t lastExpirationCheckMs = 0;

WebServer httpServer(80);

static const char DASHBOARD_HTML[] PROGMEM = R"RADAR_PAGE(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#090303">
<title>ESP32 RED RADAR SCANNER</title>
<style>
:root{color-scheme:dark;--bg:#090303;--panel:#120606;--line:#3a1212;--red:#ff2247;--orange:#ff7733;--muted:#9b7b7b;--text:#ffe5e8}
*{box-sizing:border-box}html{background:var(--bg)}body{margin:0;background:radial-gradient(ellipse at 50% 42%,#1a0707 0,#090303 62%);color:var(--text);font-family:ui-monospace,SFMono-Regular,Consolas,monospace;min-height:100vh}
.wrap{width:min(1120px,100% - 28px);margin:0 auto;padding:24px 0 30px}.header{border:1px solid #431616;background:rgba(17,7,7,.88);box-shadow:0 0 24px rgba(255,34,71,.15);padding:18px 20px;border-radius:12px}
.brandrow{display:flex;align-items:flex-start;justify-content:space-between;gap:18px}.eyebrow{font-size:10px;letter-spacing:.22em;color:var(--orange);text-transform:uppercase}.title{font-size:clamp(22px,4vw,34px);letter-spacing:.07em;margin:5px 0 0;color:#fff}.credit{font-size:12px;color:var(--red);white-space:nowrap;padding-top:6px}.statusgrid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:9px;margin-top:18px}
.stat{min-height:62px;padding:10px 12px;border:1px solid #351818;background:#100606;border-radius:8px}.label{font-size:9px;letter-spacing:.15em;color:var(--muted);text-transform:uppercase}.value{font-size:14px;margin-top:6px;color:#f9d8dc}.status{display:flex;align-items:center;gap:8px}.dot{width:8px;height:8px;border-radius:50%;background:#645959}.dot.scan{background:var(--red);box-shadow:0 0 9px var(--red)}.dot.ready{background:var(--orange);box-shadow:0 0 7px var(--orange)}.dot.bad{background:#555}
.main{margin-top:14px;border:1px solid #431616;border-radius:12px;background:linear-gradient(145deg,#110707dd,#080303dd);padding:14px;position:relative;overflow:hidden}.mainhead{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:3px 4px 10px}.mainhead h2{font-weight:500;letter-spacing:.14em;font-size:12px;margin:0;color:var(--red)}.live{font-size:10px;letter-spacing:.12em;color:var(--muted)}.radarbox{position:relative;width:min(100%,680px);margin:0 auto}.radar{display:block;width:100%;height:auto;aspect-ratio:1/1;touch-action:manipulation}.globe{position:absolute;right:10px;top:10px;width:clamp(88px,16vw,146px);aspect-ratio:1;border-radius:50%;overflow:hidden;filter:drop-shadow(0 0 13px rgba(255,34,71,.3));pointer-events:none}.globe canvas{width:100%;height:100%;display:block}.globelabel{position:absolute;bottom:4px;left:0;right:0;text-align:center;color:#c88787;font-size:8px;letter-spacing:.16em;text-shadow:0 1px 4px #000}
.legend{display:flex;flex-wrap:wrap;justify-content:center;gap:18px;margin:4px 0 0;color:#c9a7a7;font-size:10px}.key{display:flex;align-items:center;gap:7px}.swatch{width:8px;height:8px;border-radius:50%;background:var(--red);box-shadow:0 0 7px var(--red)}.swatch.ble{border-radius:1px;transform:rotate(45deg);outline:1px solid var(--orange)}.note{text-align:center;color:#9d7d7d;font-size:10px;line-height:1.5;margin:12px 4px 2px}.detail{margin:15px auto 0;max-width:680px;min-height:76px;padding:12px 14px;border:1px solid #3a1717;border-radius:8px;background:#0d0505;color:#d7b9b9;font-size:11px;line-height:1.65;white-space:pre-line}.detail strong{color:var(--orange);font-weight:500}.footer{text-align:center;color:#644848;font-size:9px;letter-spacing:.08em;margin:12px 0 0}.errorline{color:#ff8793}
</style>
</head>
<body>
<div class="wrap">
  <header class="header">
    <div class="brandrow"><div><div class="eyebrow">Red Cyberpunk RF Monitor</div><h1 class="title">ESP32 RED RADAR</h1></div><div class="credit">Developed by sreenand</div></div>
    <div class="statusgrid">
      <div class="stat"><div class="label">Wi-Fi scanner</div><div class="value status"><i id="wifiDot" class="dot"></i><span id="wifiState">STARTING</span></div></div>
      <div class="stat"><div class="label">BLE scanner</div><div class="value status"><i id="bleDot" class="dot"></i><span id="bleState">STARTING</span></div></div>
      <div class="stat"><div class="label">Detected / Wi-Fi · BLE</div><div class="value"><span id="wifiCount">0</span> · <span id="bleCount">0</span></div></div>
      <div class="stat"><div class="label">Free heap · last update</div><div class="value"><span id="heap">—</span> · <span id="updated">—</span></div></div>
    </div>
  </header>
  <main class="main">
    <div class="mainhead"><h2>TACTICAL RED RADAR</h2><div id="connection" class="live">CONNECTING…</div></div>
    <div class="radarbox">
      <canvas id="radar" class="radar" aria-label="Radar scan"></canvas>
      <div class="globe"><canvas id="earth" aria-label="Rotating earth"></canvas><div class="globelabel">GLOBAL GRID</div></div>
    </div>
    <div class="legend"><span class="key"><i class="swatch"></i>Wi-Fi Access Point</span><span class="key"><i class="swatch ble"></i>BLE Device</span></div>
    <div id="detail" class="detail"><strong>DEVICE DETAILS</strong>
Click or tap on any radar blip to view SSID, MAC address, RSSI, channel, and security info.</div>
    <p class="note">High-speed tactical sweep mode active. Signal coordinates update in real time.</p>
  </main>
  <div class="footer">ESP32-WROOM-32 · TACTICAL SURVEILLANCE DASHBOARD</div>
</div>
<script>
'use strict';
const radarCanvas=document.getElementById('radar'),radarCtx=radarCanvas.getContext('2d');
const earthCanvas=document.getElementById('earth'),earthCtx=earthCanvas.getContext('2d');
let radarSize=0,earthSize=0,devices=[],selectedMac='',lastRevision=-1,polling=false;
const clamp=(n,a,b)=>Math.max(a,Math.min(b,n));
function resizeCanvas(canvas,ctx,setSize){const box=canvas.getBoundingClientRect(),dpr=Math.min(window.devicePixelRatio||1,2);if(!box.width)return;canvas.width=Math.round(box.width*dpr);canvas.height=Math.round(box.height*dpr);ctx.setTransform(dpr,0,0,dpr,0,0);setSize(box.width)}
function resizeAll(){resizeCanvas(radarCanvas,radarCtx,n=>radarSize=n);resizeCanvas(earthCanvas,earthCtx,n=>earthSize=n)}
function hashText(s){let h=2166136261;for(let i=0;i<s.length;i++){h^=s.charCodeAt(i);h=Math.imul(h,16777619)}return h>>>0}
let radarPoints=[];
function drawRadar(t){if(!radarSize)return;const c=radarSize/2,R=radarSize*.44;radarCtx.clearRect(0,0,radarSize,radarSize);radarCtx.save();radarCtx.translate(c,c);
  const bg=radarCtx.createRadialGradient(0,0,2,0,0,R*1.08);bg.addColorStop(0,'#1a0707');bg.addColorStop(1,'#090303');radarCtx.fillStyle=bg;radarCtx.beginPath();radarCtx.arc(0,0,R*1.08,0,Math.PI*2);radarCtx.fill();
  radarCtx.save();radarCtx.beginPath();radarCtx.arc(0,0,R,0,Math.PI*2);radarCtx.clip();radarCtx.strokeStyle='rgba(255,34,71,.25)';radarCtx.lineWidth=1;
  for(let k=1;k<=5;k++){radarCtx.beginPath();radarCtx.arc(0,0,R*k/5,0,Math.PI*2);radarCtx.stroke()}
  radarCtx.strokeStyle='rgba(255,119,51,.22)';radarCtx.beginPath();radarCtx.moveTo(-R,0);radarCtx.lineTo(R,0);radarCtx.moveTo(0,-R);radarCtx.lineTo(0,R);radarCtx.stroke();
  radarCtx.strokeStyle='rgba(255,119,51,.09)';for(let i=0;i<4;i++){const a=Math.PI/4+i*Math.PI/2;radarCtx.beginPath();radarCtx.moveTo(-R*Math.cos(a),-R*Math.sin(a));radarCtx.lineTo(R*Math.cos(a),R*Math.sin(a));radarCtx.stroke()}
  const sweep=t*.0008; // Increased sweep speed
  radarCtx.beginPath();radarCtx.moveTo(0,0);radarCtx.lineTo(R*Math.cos(sweep-.55),R*Math.sin(sweep-.55));radarCtx.arc(0,0,R,sweep-.55,sweep);radarCtx.closePath();radarCtx.fillStyle='rgba(255,34,71,.12)';radarCtx.fill();radarCtx.beginPath();radarCtx.moveTo(0,0);radarCtx.lineTo(R*Math.cos(sweep),R*Math.sin(sweep));radarCtx.strokeStyle='rgba(255,70,95,.9)';radarCtx.shadowColor='#ff2247';radarCtx.shadowBlur=14;radarCtx.stroke();radarCtx.shadowBlur=0;
  radarPoints=[];for(const d of devices){const h=hashText(d.m||d.n||'');const angle=(h%628319)/100000;const strength=clamp(((Number(d.r)||-100)+100)/65,0,1);const dist=R*(.87-strength*.68);const x=Math.cos(angle)*dist,y=Math.sin(angle)*dist;radarCtx.save();radarCtx.shadowColor='#ff2247';radarCtx.shadowBlur=12;radarCtx.fillStyle='#ff2247';radarCtx.strokeStyle=d.t==='ble'?'#ff7733':'#ff8792';radarCtx.lineWidth=1.5;if(d.t==='ble'){radarCtx.translate(x,y);radarCtx.rotate(Math.PI/4);radarCtx.fillRect(-4,-4,8,8);radarCtx.strokeRect(-4,-4,8,8)}else{radarCtx.beginPath();radarCtx.arc(x,y,4.2,0,Math.PI*2);radarCtx.fill();radarCtx.stroke()}radarCtx.restore();radarPoints.push({x:x+c,y:y+c,d})}
  radarCtx.restore();radarCtx.restore();
}
function drawEarth(t){if(!earthSize)return;const c=earthSize/2,R=earthSize*.41,spin=t*.0003;earthCtx.clearRect(0,0,earthSize,earthSize);const glow=earthCtx.createRadialGradient(c,c,R*.72,c,c,R*1.3);glow.addColorStop(0,'#681919');glow.addColorStop(.78,'#9b0909');glow.addColorStop(1,'rgba(225,36,36,0)');earthCtx.fillStyle=glow;earthCtx.beginPath();earthCtx.arc(c,c,R*1.28,0,Math.PI*2);earthCtx.fill();
  earthCtx.save();earthCtx.beginPath();earthCtx.arc(c,c,R,0,Math.PI*2);earthCtx.clip();const ocean=earthCtx.createLinearGradient(c-R,c-R,c+R,c+R);ocean.addColorStop(0,'#780b0b');ocean.addColorStop(.5,'#ac0808');ocean.addColorStop(1,'#500606');earthCtx.fillStyle=ocean;earthCtx.fillRect(c-R,c-R,2*R,2*R);
  earthCtx.strokeStyle='rgba(238,104,104,.30)';earthCtx.lineWidth=.8;for(let i=-3;i<=3;i++){const phase=spin+i*Math.PI/7;const rx=Math.max(1,Math.abs(Math.cos(phase))*R*.94);earthCtx.beginPath();earthCtx.ellipse(c+Math.sin(phase)*R*.10,c,rx,R*.96,0,0,Math.PI*2);earthCtx.stroke()}for(let j=-2;j<=2;j++){const yy=c+j*R*.32,rx=R*Math.sqrt(Math.max(.02,1-Math.pow((yy-c)/R,2)));earthCtx.beginPath();earthCtx.ellipse(c,yy,rx,Math.max(1,R*.10),0,0,Math.PI*2);earthCtx.stroke()}
  earthCtx.restore();earthCtx.strokeStyle='rgba(247,104,104,.75)';earthCtx.lineWidth=1;earthCtx.beginPath();earthCtx.arc(c,c,R,0,Math.PI*2);earthCtx.stroke()
}
function animate(t){drawRadar(t);drawEarth(t);requestAnimationFrame(animate)}
function renderDetail(d){const box=document.getElementById('detail');if(!d){box.innerHTML='<strong>DEVICE DETAILS</strong>\nClick or tap on any radar blip to view SSID, MAC address, RSSI, channel, and security info.';return}const title=d.t==='ble'?'BLE DEVICE':'WI-FI ACCESS POINT';const name=d.n||'(Unnamed Device)';box.textContent=`${title}\nName: ${name}\nMAC Address: ${d.m||'N/A'}  ·  RSSI: ${d.r} dBm${d.t==='wifi'?`  ·  Channel: ${d.c}  ·  Security: ${d.s||'Unknown'}`:'  ·  Type: BLE Advertisement'}\nLast detected ${d.a}s ago`}
function choosePoint(event){const r=radarCanvas.getBoundingClientRect(),x=event.clientX-r.left,y=event.clientY-r.top;let best=null,bestD=20;for(const p of radarPoints){const d=Math.hypot(p.x-x,p.y-y);if(d<bestD){best=p;bestD=d}}if(best){selectedMac=best.d.m;renderDetail(best.d)}}
function hoverPoint(event){const r=radarCanvas.getBoundingClientRect(),x=event.clientX-r.left,y=event.clientY-r.top;let best=null,bestD=16;for(const p of radarPoints){const d=Math.hypot(p.x-x,p.y-y);if(d<bestD){best=p;bestD=d}}radarCanvas.style.cursor=best?'pointer':'default';if(best)renderDetail(best.d)}
radarCanvas.addEventListener('pointerdown',choosePoint);radarCanvas.addEventListener('pointermove',hoverPoint);
function setStateBadge(prefix,state){document.getElementById(prefix+'State').textContent=state;const dot=document.getElementById(prefix+'Dot');dot.className='dot '+(state==='SCANNING'?'scan':(state==='ONLINE'||state==='READY'?'ready':'bad'))}
async function poll(){if(polling)return;polling=true;try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw new Error('status');const s=await r.json();setStateBadge('wifi',s.wifi);setStateBadge('ble',s.ble);document.getElementById('wifiCount').textContent=s.wifiCount;document.getElementById('bleCount').textContent=s.bleCount;document.getElementById('heap').textContent=Math.round(s.heap/1024)+' KB';document.getElementById('connection').textContent=`${s.mode} · ${s.ip}`;document.getElementById('connection').classList.remove('errorline');document.getElementById('updated').textContent=new Date().toLocaleTimeString();if(s.rev!==lastRevision){const dr=await fetch('/api/devices',{cache:'no-store'});if(dr.ok){const body=await dr.json();devices=body.devices||[];lastRevision=body.rev;const chosen=devices.find(d=>d.m===selectedMac);if(chosen)renderDetail(chosen)}}}catch(e){document.getElementById('connection').textContent='API OFFLINE';document.getElementById('connection').classList.add('errorline')}finally{polling=false}}
window.addEventListener('resize',resizeAll);resizeAll();requestAnimationFrame(animate);poll();setInterval(poll,1800);
</script>
</body>
</html>
)RADAR_PAGE";

static void copyText(char *destination, size_t capacity, const char *source) {
  if (capacity == 0) return;
  if (source == nullptr) source = "";
  strncpy(destination, source, capacity - 1);
  destination[capacity - 1] = '\0';
}

static void jsonAppendChar(JsonWriter &writer, char value) {
  if (!writer.ok || writer.length + 1 >= writer.capacity) {
    writer.ok = false;
    return;
  }
  writer.data[writer.length++] = value;
  writer.data[writer.length] = '\0';
}

static void jsonAppendFormat(JsonWriter &writer, const char *format, ...) {
  if (!writer.ok || writer.length >= writer.capacity) {
    writer.ok = false;
    return;
  }
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(writer.data + writer.length,
                                writer.capacity - writer.length, format, args);
  va_end(args);
  if (written < 0 || (size_t)written >= writer.capacity - writer.length) {
    writer.ok = false;
    writer.data[writer.capacity - 1] = '\0';
    return;
  }
  writer.length += (size_t)written;
}

static void jsonAppend(JsonWriter &writer, const char *text) {
  if (text == nullptr) return;
  while (*text && writer.ok) jsonAppendChar(writer, *text++);
}

static void jsonAppendString(JsonWriter &writer, const char *value) {
  jsonAppendChar(writer, '"');
  if (value == nullptr) value = "";
  for (const unsigned char *p = (const unsigned char *)value; *p && writer.ok; ++p) {
    const unsigned char c = *p;
    if (c == '"' || c == '\\') {
      jsonAppendChar(writer, '\\');
      jsonAppendChar(writer, (char)c);
    } else if (c < 0x20) {
      jsonAppendFormat(writer, "\\u%04x", (unsigned)c);
    } else {
      jsonAppendChar(writer, (char)c);
    }
  }
  jsonAppendChar(writer, '"');
}

static void upsertDevice(bool isWifi, const char *mac, const char *name,
                         int rssi, int channel, const char *security) {
  if (mac == nullptr || mac[0] == '\0') return;
  DeviceRecord *records = isWifi ? wifiDevices : bleDevices;
  uint8_t *count = isWifi ? &wifiDeviceCount : &bleDeviceCount;
  const uint8_t limit = isWifi ? MAX_WIFI_DEVICES : MAX_BLE_DEVICES;
  const uint32_t now = millis();

  portENTER_CRITICAL(&deviceMux);
  int slot = -1;
  for (uint8_t i = 0; i < *count; ++i) {
    if (strncmp(records[i].mac, mac, sizeof(records[i].mac)) == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    if (*count < limit) {
      slot = (*count)++;
    } else {
      uint32_t greatestAge = 0;
      slot = 0;
      for (uint8_t i = 0; i < *count; ++i) {
        const uint32_t age = now - records[i].lastSeenMs;
        if (age >= greatestAge) {
          greatestAge = age;
          slot = i;
        }
      }
    }
  }

  DeviceRecord &record = records[slot];
  copyText(record.mac, sizeof(record.mac), mac);
  copyText(record.name, sizeof(record.name), name);
  copyText(record.security, sizeof(record.security), security);
  record.rssi = (int16_t)rssi;
  record.channel = (int8_t)channel;
  record.lastSeenMs = now;
  ++dataRevision;
  portEXIT_CRITICAL(&deviceMux);
}

static void expireArray(DeviceRecord *records, uint8_t &count, uint32_t now) {
  uint8_t i = 0;
  while (i < count) {
    if ((uint32_t)(now - records[i].lastSeenMs) > DEVICE_EXPIRATION_MS) {
      for (uint8_t j = i + 1; j < count; ++j) records[j - 1] = records[j];
      --count;
      ++dataRevision;
    } else {
      ++i;
    }
  }
}

static void expireDevices(uint32_t now) {
  portENTER_CRITICAL(&deviceMux);
  expireArray(wifiDevices, wifiDeviceCount, now);
  expireArray(bleDevices, bleDeviceCount, now);
  portEXIT_CRITICAL(&deviceMux);
}

static void getDeviceCounts(uint8_t &wifiCount, uint8_t &bleCount, uint32_t &revision) {
  portENTER_CRITICAL(&deviceMux);
  wifiCount = wifiDeviceCount;
  bleCount = bleDeviceCount;
  revision = dataRevision;
  portEXIT_CRITICAL(&deviceMux);
}

static void onBleScanComplete(BLEScanResults results) {
  (void)results;
  bleScanFinished = true;
}

class PassiveBleCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    String address = advertisedDevice.getAddress().toString();
    String advertisedName;
    if (advertisedDevice.haveName()) advertisedName = advertisedDevice.getName();

    char mac[18];
    char name[33];
    copyText(mac, sizeof(mac), address.c_str());
    if (advertisedName.length() > 0) {
      advertisedName.toCharArray(name, sizeof(name));
    } else {
      copyText(name, sizeof(name), "(Unnamed BLE)");
    }
    upsertDevice(false, mac, name, advertisedDevice.getRSSI(), 0, "BLE");
  }
};

static PassiveBleCallbacks bleCallbacks;

static bool initializeBle() {
  BLEDevice::init("ESP32_RADAR");
  bleScanner = BLEDevice::getScan();
  if (bleScanner == nullptr) {
    bleState = "ERROR";
    return false;
  }
  bleScanner->setActiveScan(false);
  bleScanner->setInterval(100);
  bleScanner->setWindow(50);
  bleScanner->setAdvertisedDeviceCallbacks(&bleCallbacks, false);
  bleState = "READY";
  return true;
}

static const char *wifiSecurityName(uint8_t auth) {
  switch (auth) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    default: return "Protected";
  }
}

static bool initializeNetwork() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("esp32-radar");

#if USE_ACCESS_POINT
  WiFi.mode(WIFI_AP_STA);
  accessPointReady = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CLIENTS);
  if (!accessPointReady) {
    wifiState = "ERROR";
    return false;
  }
  wifiState = "READY";
  return true;
#else
  WiFi.mode(WIFI_STA);
  wifiState = "OFFLINE";
  return true;
#endif
}

static bool networkAvailable() {
#if USE_ACCESS_POINT
  return accessPointReady;
#else
  return WiFi.status() == WL_CONNECTED;
#endif
}

static void processWifiResults(int16_t found) {
  for (int16_t i = 0; i < found; ++i) {
    String ssid = WiFi.SSID(i);
    uint8_t *bssid = WiFi.BSSID(i);
    if (bssid == nullptr) continue;

    char name[33];
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned)bssid[0], (unsigned)bssid[1], (unsigned)bssid[2],
             (unsigned)bssid[3], (unsigned)bssid[4], (unsigned)bssid[5]);
    if (ssid.length() == 0) {
      copyText(name, sizeof(name), "(Hidden SSID)");
    } else {
      ssid.toCharArray(name, sizeof(name));
    }
    upsertDevice(true, mac, name, WiFi.RSSI(i), WiFi.channel(i),
                 wifiSecurityName(WiFi.encryptionType(i)));
  }
}

static void startBleScan() {
  if (!bleReady || bleScanner == nullptr) {
    bleState = "ERROR";
    scanPhase = PHASE_IDLE;
    nextCycleAtMs = millis() + WIFI_CYCLE_INTERVAL_MS;
    return;
  }

  bleScanFinished = false;
  if (!bleScanner->start(BLE_SCAN_SECONDS, onBleScanComplete, false)) {
    bleState = "ERROR";
    scanPhase = PHASE_IDLE;
    nextCycleAtMs = millis() + WIFI_CYCLE_INTERVAL_MS;
    return;
  }
  scanPhase = PHASE_BLE;
  bleState = "SCANNING";
}

static void startWifiScan() {
  if (!networkAvailable()) return;
  scanPhase = PHASE_WIFI;
  wifiState = "SCANNING";
  bleState = bleReady ? "READY" : "ERROR";
  WiFi.scanNetworks(true, true, true, WIFI_PASSIVE_DWELL_MS);
}

static void serviceScanner() {
  const uint32_t now = millis();
  if ((uint32_t)(now - lastExpirationCheckMs) >= EXPIRATION_CHECK_MS) {
    expireDevices(now);
    lastExpirationCheckMs = now;
  }

  if (scanPhase == PHASE_WIFI) {
    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) return;
    if (result >= 0) {
      processWifiResults(result);
      WiFi.scanDelete();
      wifiState = networkAvailable() ? "READY" : "OFFLINE";
    } else {
      WiFi.scanDelete();
      wifiState = networkAvailable() ? "ERROR" : "OFFLINE";
    }
    startBleScan();
    return;
  }

  if (scanPhase == PHASE_BLE) {
    if (!bleScanFinished) return;
    bleScanFinished = false;
    if (bleScanner != nullptr) bleScanner->clearResults();
    scanPhase = PHASE_IDLE;
    bleState = "READY";
    nextCycleAtMs = now + WIFI_CYCLE_INTERVAL_MS;
    return;
  }

  if (!networkAvailable()) {
    wifiState = accessPointReady ? "OFFLINE" : "ERROR";
    bleState = bleReady ? "READY" : "ERROR";
    return;
  }
  if ((int32_t)(now - nextCycleAtMs) >= 0) startWifiScan();
}

static char statusJson[512];
static char deviceJson[DEVICE_JSON_CAPACITY];

static void sendStatus() {
  uint8_t wifiCount = 0, bleCount = 0;
  uint32_t revision = 0;
  getDeviceCounts(wifiCount, bleCount, revision);
  char ip[16] = "0.0.0.0";
#if USE_ACCESS_POINT
  if (accessPointReady) {
    const IPAddress address = WiFi.softAPIP();
    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
  }
  const char *mode = "AP";
#else
  const char *mode = "STA";
#endif
  snprintf(statusJson, sizeof(statusJson),
           "{\"wifi\":\"%s\",\"ble\":\"%s\",\"wifiCount\":%u,\"bleCount\":%u,\"rev\":%lu,\"heap\":%u,\"mode\":\"%s\",\"ip\":\"%s\"}",
           wifiState, bleState, (unsigned)wifiCount, (unsigned)bleCount,
           (unsigned long)revision, (unsigned)ESP.getFreeHeap(), mode, ip);
  httpServer.sendHeader("Cache-Control", "no-store, max-age=0");
  httpServer.send(200, "application/json; charset=utf-8", statusJson);
}

static void appendDevice(JsonWriter &writer, const DeviceRecord &d,
                         const char *kind, uint32_t now, bool &first) {
  if (!first) jsonAppendChar(writer, ',');
  first = false;
  jsonAppendFormat(writer, "{\"t\":\"%s\",\"m\":", kind);
  jsonAppendString(writer, d.mac);
  jsonAppend(writer, ",\"n\":");
  jsonAppendString(writer, d.name);
  jsonAppendFormat(writer, ",\"r\":%d,\"c\":%d,\"s\":", (int)d.rssi, (int)d.channel);
  jsonAppendString(writer, d.security);
  jsonAppendFormat(writer, ",\"a\":%lu}",
                   (unsigned long)((uint32_t)(now - d.lastSeenMs) / 1000U));
}

static void sendDevices() {
  uint8_t wifiCount = 0, bleCount = 0;
  uint32_t revision = 0;
  portENTER_CRITICAL(&deviceMux);
  wifiCount = wifiDeviceCount;
  bleCount = bleDeviceCount;
  revision = dataRevision;
  memcpy(wifiDeviceSnapshot, wifiDevices, wifiCount * sizeof(DeviceRecord));
  memcpy(bleDeviceSnapshot, bleDevices, bleCount * sizeof(DeviceRecord));
  portEXIT_CRITICAL(&deviceMux);

  JsonWriter writer = {deviceJson, sizeof(deviceJson), 0, true};
  deviceJson[0] = '\0';
  jsonAppendFormat(writer, "{\"rev\":%lu,\"devices\":[", (unsigned long)revision);
  bool first = true;
  const uint32_t now = millis();
  for (uint8_t i = 0; i < wifiCount && writer.ok; ++i)
    appendDevice(writer, wifiDeviceSnapshot[i], "wifi", now, first);
  for (uint8_t i = 0; i < bleCount && writer.ok; ++i)
    appendDevice(writer, bleDeviceSnapshot[i], "ble", now, first);
  jsonAppend(writer, "]}");

  if (!writer.ok) {
    httpServer.send(500, "application/json; charset=utf-8", "{\"error\":\"too large\"}");
    return;
  }
  httpServer.sendHeader("Cache-Control", "no-store, max-age=0");
  httpServer.chunkResponseBegin("application/json; charset=utf-8");
  httpServer.chunkWrite(deviceJson, writer.length);
  httpServer.chunkResponseEnd();
}

static void configureWebServer() {
  httpServer.on("/", HTTP_GET, []() {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.send_P(200, PSTR("text/html; charset=utf-8"), DASHBOARD_HTML);
  });
  httpServer.on("/api/status", HTTP_GET, sendStatus);
  httpServer.on("/api/devices", HTTP_GET, sendDevices);
  httpServer.begin();
}

void setup() {
  Serial.begin(115200);
  delay(150);
  initializeNetwork();
  bleReady = initializeBle();
  configureWebServer();
  nextCycleAtMs = millis() + 1000;
}

void loop() {
  httpServer.handleClient();
  serviceScanner();
  delay(2);
}