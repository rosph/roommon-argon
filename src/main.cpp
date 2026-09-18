#include <Arduino.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <atomic>
#include <math.h>

namespace {
constexpr uint32_t SAMPLE_RATE = 16000;
constexpr size_t BLOCK_SAMPLES = 256;
constexpr size_t AUDIO_RING_BLOCKS = 4;
constexpr uint32_t ACTIVE_HOLD_MS = 2500;
constexpr uint32_t DISPLAY_INTERVAL_MS = 250;
constexpr uint32_t MQTT_INTERVAL_MS = 5000;
constexpr uint32_t BASELINE_WARMUP_MS = 12000;
constexpr uint8_t MAX_WS_CLIENTS = 8;

int16_t audioRing[AUDIO_RING_BLOCKS][BLOCK_SAMPLES];
size_t audioWriteIndex = 0;
std::atomic<int16_t*> completedBlock{nullptr};

WebServer server(80);
WebSocketsServer webSocket(81);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;
WiFiManager wm;

bool wsAuthorized[MAX_WS_CLIENTS] = {false};
bool micMuted = false;
bool soundActive = false;
uint32_t soundStartedAt = 0;
uint32_t lastAboveAt = 0;
uint32_t bootAt = 0;
uint32_t lastDisplayAt = 0;
uint32_t lastMqttAt = 0;

float currentDbfs = -90.0f;
float noiseFloorDbfs = -65.0f;
float aboveBaselineDb = 0.0f;
float peakDbfs = -90.0f;
float thresholdDb = 12.0f;

String deviceName = "Living Room";
String accessPin;
String mqttHost;
String mqttUser;
String mqttPass;
uint16_t mqttPort = 1883;
String mqttBase;
String chipSuffix;

char pMqttHost[65] = {0};
char pMqttPort[7] = "1883";
char pMqttUser[33] = {0};
char pMqttPass[65] = {0};
char pDeviceName[33] = "Living Room";
char pAccessPin[9] = {0};
char pThreshold[8] = "12.0";

bool savePortalParams = false;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Room Monitor</title><style>
:root{font-family:system-ui,-apple-system,sans-serif;color-scheme:dark;background:#0d1117;color:#e6edf3}body{margin:0;padding:24px;max-width:720px;margin:auto}h1{margin:0 0 4px;font-size:28px}.muted{color:#8b949e}.card{border:1px solid #30363d;background:#161b22;border-radius:16px;padding:18px;margin:18px 0}.status{display:flex;gap:12px;align-items:center}.dot{width:12px;height:12px;border-radius:50%;background:#3fb950}.dot.active{background:#d29922}.dot.live{background:#58a6ff}.big{font-size:42px;font-weight:700;margin:8px 0}.bar{height:18px;border-radius:9px;background:#21262d;overflow:hidden}.bar>div{height:100%;width:0;background:#8b949e;transition:width .2s}.grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}.metric{background:#0d1117;border-radius:10px;padding:12px}.metric b{display:block;font-size:20px;margin-top:3px}button,input{font:inherit}button{padding:12px 16px;border:0;border-radius:10px;background:#238636;color:white;font-weight:650;cursor:pointer}button.stop{background:#da3633}input{padding:10px;border-radius:8px;border:1px solid #30363d;background:#0d1117;color:#e6edf3;width:120px}.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}small{color:#8b949e}#audioState{min-height:1.4em}@media(max-width:440px){body{padding:16px}.grid{grid-template-columns:1fr}.big{font-size:36px}}</style></head>
<body><h1 id="title">Room Monitor</h1><div class="muted" id="net">Connecting...</div>
<div class="card"><div class="status"><span id="dot" class="dot"></span><strong id="state">QUIET</strong></div><div class="big"><span id="above">0.0</span> dB <span class="muted" style="font-size:16px">above baseline</span></div><div class="bar"><div id="levelBar"></div></div><div class="grid" style="margin-top:14px"><div class="metric">Mic level<b id="dbfs">-90 dBFS</b></div><div class="metric">Noise floor<b id="floor">-65 dBFS</b></div><div class="metric">Active for<b id="duration">0:00</b></div><div class="metric">Peak<b id="peak">-90 dBFS</b></div></div></div>
<div class="card"><h3>Listen live</h3><p>Audio is never stored. The M5Stick screen changes to <b>REMOTE LISTENING</b> while a stream is active.</p><div class="row"><input id="pin" inputmode="numeric" autocomplete="one-time-code" placeholder="Access PIN"><button id="listen">Listen live</button></div><div id="audioState" class="muted" style="margin-top:10px"></div><small>16 kHz mono PCM. Use headphones to avoid acoustic feedback.</small></div>
<div class="card"><h3>Detection sensitivity</h3><div class="row"><input id="threshold" type="number" min="4" max="30" step="0.5"><button id="saveThreshold">Save</button></div><small>Lower values trigger more easily. 10–14 dB above the learned room baseline is a useful starting range.</small></div>
<script>
const $=id=>document.getElementById(id);let ws=null,audioCtx=null,nextTime=0;const pin=$('pin');pin.value=localStorage.roomMonitorPin||'';
function fmt(s){s=Math.max(0,Math.floor(s));return Math.floor(s/60)+':'+String(s%60).padStart(2,'0')}
async function poll(){try{const r=await fetch('/api/status',{cache:'no-store'});const x=await r.json();$('title').textContent=x.name+' · Room Monitor';$('net').textContent=x.ip+' · '+(x.muted?'MIC MUTED':'Monitoring');$('state').textContent=x.muted?'MIC MUTED':x.streaming?'REMOTE LISTENING':x.active?'SOUND ACTIVE':'QUIET';$('dot').className='dot '+(x.streaming?'live':x.active?'active':'');$('above').textContent=x.above.toFixed(1);$('dbfs').textContent=x.dbfs.toFixed(1)+' dBFS';$('floor').textContent=x.floor.toFixed(1)+' dBFS';$('peak').textContent=x.peak.toFixed(1)+' dBFS';$('duration').textContent=fmt(x.duration);$('levelBar').style.width=Math.max(0,Math.min(100,(x.above/30)*100))+'%';if(document.activeElement!==$('threshold'))$('threshold').value=x.threshold.toFixed(1)}catch(e){$('net').textContent='Device unavailable'}}
setInterval(poll,1000);poll();
$('saveThreshold').onclick=async()=>{const v=Math.max(4,Math.min(30,Number($('threshold').value)||12));await fetch('/api/settings?threshold='+encodeURIComponent(v),{method:'POST'});poll()};
$('listen').onclick=()=>{if(ws){ws.close();return}const p=pin.value.trim();if(!p){$('audioState').textContent='Enter the PIN shown during device setup.';return}localStorage.roomMonitorPin=p;audioCtx=new (window.AudioContext||window.webkitAudioContext)({sampleRate:16000});nextTime=audioCtx.currentTime+.08;ws=new WebSocket('ws://'+location.hostname+':81/?pin='+encodeURIComponent(p));ws.binaryType='arraybuffer';ws.onopen=()=>{$('listen').textContent='Stop listening';$('listen').classList.add('stop');$('audioState').textContent='Live audio connected'};ws.onmessage=e=>{const d=new Int16Array(e.data),f=new Float32Array(d.length);for(let i=0;i<d.length;i++)f[i]=d[i]/32768;const b=audioCtx.createBuffer(1,f.length,16000);b.copyToChannel(f,0);const s=audioCtx.createBufferSource();s.buffer=b;s.connect(audioCtx.destination);const t=Math.max(audioCtx.currentTime+.03,nextTime);s.start(t);nextTime=t+b.duration};ws.onerror=()=>{$('audioState').textContent='Could not connect. Check the PIN and network route.'};ws.onclose=()=>{ws=null;$('listen').textContent='Listen live';$('listen').classList.remove('stop');$('audioState').textContent='Stream stopped';if(audioCtx){audioCtx.close();audioCtx=null}}};
</script></body></html>
)HTML";

String safeTopic(String value) {
  value.toLowerCase();
  for (size_t i=0;i<value.length();++i) {
    char c=value[i];
    if (!isalnum(static_cast<unsigned char>(c))) value.setCharAt(i, '_');
  }
  return value;
}

void onMicBlock(void*, void* data, size_t) {
  completedBlock.store(static_cast<int16_t*>(data), std::memory_order_release);
}

String makePin() {
  uint64_t id = ESP.getEfuseMac();
  uint32_t n = static_cast<uint32_t>((id ^ (id >> 24)) % 900000ULL) + 100000;
  return String(n);
}

void loadPrefs() {
  prefs.begin("roommon", false);
  deviceName = prefs.getString("name", "Living Room");
  accessPin = prefs.getString("pin", "");
  if (accessPin.length() < 4) { accessPin = makePin(); prefs.putString("pin", accessPin); }
  mqttHost = prefs.getString("mhost", "");
  mqttPort = prefs.getUShort("mport", 1883);
  mqttUser = prefs.getString("muser", "");
  mqttPass = prefs.getString("mpass", "");
  thresholdDb = prefs.getFloat("thresh", 12.0f);
  thresholdDb = constrain(thresholdDb, 4.0f, 30.0f);
  strlcpy(pMqttHost, mqttHost.c_str(), sizeof(pMqttHost));
  snprintf(pMqttPort, sizeof(pMqttPort), "%u", mqttPort);
  strlcpy(pMqttUser, mqttUser.c_str(), sizeof(pMqttUser));
  strlcpy(pMqttPass, mqttPass.c_str(), sizeof(pMqttPass));
  strlcpy(pDeviceName, deviceName.c_str(), sizeof(pDeviceName));
  strlcpy(pAccessPin, accessPin.c_str(), sizeof(pAccessPin));
  snprintf(pThreshold, sizeof(pThreshold), "%.1f", thresholdDb);
}

void savePrefsFromPortal() {
  mqttHost = String(pMqttHost); mqttUser = String(pMqttUser); mqttPass = String(pMqttPass);
  mqttPort = constrain(atoi(pMqttPort), 1, 65535);
  deviceName = String(pDeviceName); if (!deviceName.length()) deviceName = "Living Room";
  accessPin = String(pAccessPin); if (accessPin.length() < 4) accessPin = makePin();
  thresholdDb = constrain(String(pThreshold).toFloat(), 4.0f, 30.0f);
  prefs.putString("mhost", mqttHost); prefs.putUShort("mport", mqttPort);
  prefs.putString("muser", mqttUser); prefs.putString("mpass", mqttPass);
  prefs.putString("name", deviceName); prefs.putString("pin", accessPin); prefs.putFloat("thresh", thresholdDb);
}

void drawBoot(const String& line1, const String& line2="") {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString(line1, 120, 54);
  M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  M5.Display.drawString(line2, 120, 82);
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFiManagerParameter h("<p><b>Room Monitor settings</b></p>");
  WiFiManagerParameter mh("mqtt", "MQTT broker (optional)", pMqttHost, sizeof(pMqttHost)-1);
  WiFiManagerParameter mp("mqttport", "MQTT port", pMqttPort, sizeof(pMqttPort)-1);
  WiFiManagerParameter mu("mqttuser", "MQTT username", pMqttUser, sizeof(pMqttUser)-1);
  WiFiManagerParameter mw("mqttpass", "MQTT password", pMqttPass, sizeof(pMqttPass)-1, "type='password'");
  WiFiManagerParameter dn("name", "Room name", pDeviceName, sizeof(pDeviceName)-1);
  WiFiManagerParameter ap("pin", "Listening access PIN", pAccessPin, sizeof(pAccessPin)-1, "inputmode='numeric'");
  WiFiManagerParameter th("threshold", "Trigger dB above baseline", pThreshold, sizeof(pThreshold)-1, "type='number' min='4' max='30' step='0.5'");
  wm.addParameter(&h); wm.addParameter(&dn); wm.addParameter(&ap); wm.addParameter(&th); wm.addParameter(&mh); wm.addParameter(&mp); wm.addParameter(&mu); wm.addParameter(&mw);
  wm.setSaveParamsCallback([](){ savePortalParams = true; });
  wm.setConfigPortalTimeout(300);
  String apName = "RoomMonitor-" + chipSuffix;
  drawBoot("ROOM MONITOR", "Wi-Fi setup if needed");
  bool ok = wm.autoConnect(apName.c_str());
  if (savePortalParams) savePrefsFromPortal();
  if (!ok) ESP.restart();
}

void mqttDiscovery() {
  if (!mqtt.connected()) return;
  String id = "room_monitor_" + chipSuffix;
  String dev = "{\"identifiers\":[\""+id+"\"],\"name\":\""+deviceName+" Room Monitor\",\"manufacturer\":\"DIY / M5Stack\",\"model\":\"M5StickC Plus 1.1\"}";
  auto cfg = [&](const String& component,const String& obj,const String& name,const String& unit,const String& value,const String& devClass="") {
    String topic="homeassistant/"+component+"/"+id+"/"+obj+"/config";
    String payload="{\"name\":\""+name+"\",\"unique_id\":\""+id+"_"+obj+"\",\"state_topic\":\""+mqttBase+"/state\",\"value_template\":\"{{ value_json."+value+" }}\",\"device\":"+dev;
    if(unit.length()) payload += ",\"unit_of_measurement\":\""+unit+"\"";
    if(devClass.length()) payload += ",\"device_class\":\""+devClass+"\"";
    payload += "}";
    mqtt.publish(topic.c_str(), payload.c_str(), true);
  };
  cfg("sensor","level","Sound level","dBFS","dbfs");
  cfg("sensor","above","Above baseline","dB","above");
  cfg("sensor","floor","Noise floor","dBFS","floor");
  cfg("sensor","duration","Sound active","s","duration","duration");
  cfg("sensor","wifi","Wi-Fi RSSI","dBm","rssi","signal_strength");
  cfg("binary_sensor","active","Sound active","","active","sound");
  cfg("binary_sensor","streaming","Remote listening","","streaming","sound");
}

bool mqttConnect() {
  if (!mqttHost.length()) return false;
  if (mqtt.connected()) return true;
  mqtt.setServer(mqttHost.c_str(), mqttPort);
  String clientId="roommon-"+chipSuffix;
  bool ok = mqttUser.length() ? mqtt.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str()) : mqtt.connect(clientId.c_str());
  if (ok) mqttDiscovery();
  return ok;
}

int authorizedCount() {
  int n=0; for (bool x: wsAuthorized) if (x) ++n; return n;
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (num >= MAX_WS_CLIENTS) return;
  if (type == WStype_CONNECTED) {
    String url(reinterpret_cast<char*>(payload), length);
    int p=url.indexOf("pin=");
    String provided;
    if (p>=0) { provided=url.substring(p+4); int amp=provided.indexOf('&'); if(amp>=0) provided=provided.substring(0,amp); provided.replace("%20"," "); }
    wsAuthorized[num] = (provided == accessPin && !micMuted);
    if (!wsAuthorized[num]) webSocket.disconnect(num);
  } else if (type == WStype_DISCONNECTED) {
    wsAuthorized[num] = false;
  }
}

void setupWeb() {
  server.on("/", HTTP_GET, [](){ server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/status", HTTP_GET, [](){
    uint32_t duration = soundActive ? (millis()-soundStartedAt)/1000 : 0;
    String json="{\"name\":\""+deviceName+"\",\"ip\":\""+WiFi.localIP().toString()+"\",\"dbfs\":"+String(currentDbfs,1)+",\"floor\":"+String(noiseFloorDbfs,1)+",\"above\":"+String(aboveBaselineDb,1)+",\"peak\":"+String(peakDbfs,1)+",\"threshold\":"+String(thresholdDb,1)+",\"duration\":"+String(duration)+",\"active\":"+(soundActive?"true":"false")+",\"streaming\":"+(authorizedCount()>0?"true":"false")+",\"muted\":"+(micMuted?"true":"false")+"}";
    server.send(200,"application/json",json);
  });
  server.on("/api/settings", HTTP_POST, [](){
    if(server.hasArg("threshold")){ thresholdDb=constrain(server.arg("threshold").toFloat(),4.0f,30.0f); prefs.putFloat("thresh",thresholdDb); }
    server.send(204,"text/plain","");
  });
  server.onNotFound([](){ server.sendHeader("Location","/",true); server.send(302,"text/plain",""); });
  server.begin();
  webSocket.begin(); webSocket.onEvent(webSocketEvent);
}

void processAudio(int16_t* data) {
  int64_t sum=0; for(size_t i=0;i<BLOCK_SAMPLES;i++) sum+=data[i];
  float mean=(float)sum/BLOCK_SAMPLES;
  double sumSq=0;
  for(size_t i=0;i<BLOCK_SAMPLES;i++){ float v=data[i]-mean; sumSq += v*v; }
  float rms=sqrt(sumSq/BLOCK_SAMPLES);
  currentDbfs=20.0f*log10f(max(rms,1.0f)/32768.0f);
  currentDbfs=constrain(currentDbfs,-90.0f,0.0f);
  peakDbfs=max(peakDbfs,currentDbfs);

  uint32_t now=millis();
  bool warmup=(now-bootAt)<BASELINE_WARMUP_MS;
  float alpha = warmup ? 0.03f : 0.002f;
  if (warmup || !soundActive) noiseFloorDbfs = noiseFloorDbfs*(1.0f-alpha) + currentDbfs*alpha;
  aboveBaselineDb=max(0.0f,currentDbfs-noiseFloorDbfs);
  bool above = aboveBaselineDb >= thresholdDb && currentDbfs > -58.0f;
  if (above) lastAboveAt=now;
  bool shouldActive=(now-lastAboveAt)<ACTIVE_HOLD_MS && !warmup && !micMuted;
  if (shouldActive && !soundActive){soundActive=true;soundStartedAt=now;}
  if (!shouldActive && soundActive){soundActive=false;soundStartedAt=0;peakDbfs=currentDbfs;}

  if (!micMuted && authorizedCount()>0) {
    for(uint8_t i=0;i<MAX_WS_CLIENTS;i++) if(wsAuthorized[i]) webSocket.sendBIN(i,reinterpret_cast<uint8_t*>(data),BLOCK_SAMPLES*sizeof(int16_t));
  }
}

void drawMain() {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(top_left);
  M5.Display.setTextColor(TFT_LIGHTGREY,TFT_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.drawString(deviceName,8,6);
  bool streaming=authorizedCount()>0;
  uint16_t color = micMuted?TFT_RED:streaming?TFT_BLUE:soundActive?TFT_ORANGE:TFT_GREEN;
  M5.Display.fillCircle(226,11,5,color);
  M5.Display.setTextColor(TFT_WHITE,TFT_BLACK);
  M5.Display.setTextSize(2);
  String state=micMuted?"MIC MUTED":streaming?"REMOTE LISTEN":soundActive?"SOUND ACTIVE":"QUIET";
  M5.Display.drawString(state,8,28);
  M5.Display.setTextSize(4);
  M5.Display.setTextColor(color,TFT_BLACK);
  M5.Display.drawString(String(aboveBaselineDb,0)+" dB",8,55);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_LIGHTGREY,TFT_BLACK);
  String bottom=WiFi.localIP().toString();
  if(soundActive) bottom += "  " + String((millis()-soundStartedAt)/60000) + "m";
  M5.Display.drawString(bottom,8,117);
}

void publishMqtt() {
  if (!mqttConnect()) return;
  uint32_t duration=soundActive?(millis()-soundStartedAt)/1000:0;
  String json="{\"dbfs\":"+String(currentDbfs,1)+",\"floor\":"+String(noiseFloorDbfs,1)+",\"above\":"+String(aboveBaselineDb,1)+",\"duration\":"+String(duration)+",\"rssi\":"+String(WiFi.RSSI())+",\"active\":\""+(soundActive?"ON":"OFF")+"\",\"streaming\":\""+(authorizedCount()>0?"ON":"OFF")+"\"}";
  mqtt.publish((mqttBase+"/state").c_str(),json.c_str(),true);
}

void resetConfiguration() {
  drawBoot("RESETTING", "Wi-Fi + settings");
  delay(800);
  wm.resetSettings();
  prefs.clear();
  delay(300);
  ESP.restart();
}
}

void setup() {
  Serial.begin(115200);
  auto cfg=M5.config();
  cfg.internal_mic=true;
  cfg.internal_spk=false;
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(80);
  M5.Speaker.end();

  chipSuffix=String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF),HEX); chipSuffix.toUpperCase();
  loadPrefs();
  setupWiFi();
  mqttBase="roommonitor/"+safeTopic(deviceName)+"_"+chipSuffix;
  MDNS.begin("roommonitor-"+chipSuffix);
  MDNS.addService("http","tcp",80);
  setupWeb();

  auto micCfg=M5.Mic.config();
  micCfg.sample_rate=SAMPLE_RATE;
  micCfg.stereo=false;
  M5.Mic.config(micCfg);
  M5.Mic.setBufferReleaseCallback(nullptr,onMicBlock);
  M5.Mic.begin();

  bootAt=millis(); lastAboveAt=millis()-ACTIVE_HOLD_MS-1;
  drawMain();
}

void loop() {
  M5.update();
  server.handleClient();
  webSocket.loop();
  mqtt.loop();

  if (!micMuted && M5.Mic.isEnabled()) {
    auto* dest=audioRing[audioWriteIndex];
    if (M5.Mic.record(dest,BLOCK_SAMPLES,SAMPLE_RATE,false)) audioWriteIndex=(audioWriteIndex+1)%AUDIO_RING_BLOCKS;
  }
  if (auto* data=completedBlock.exchange(nullptr,std::memory_order_acq_rel)) processAudio(data);

  static uint32_t btnBDown=0;
  if (M5.BtnB.isPressed()) { if(!btnBDown) btnBDown=millis(); if(millis()-btnBDown>5000) resetConfiguration(); }
  else btnBDown=0;

  if (M5.BtnA.wasClicked()) {
    micMuted=!micMuted;
    if (micMuted) { for(uint8_t i=0;i<MAX_WS_CLIENTS;i++) if(wsAuthorized[i]) webSocket.disconnect(i); M5.Mic.end(); }
    else M5.Mic.begin();
  }

  uint32_t now=millis();
  if(now-lastDisplayAt>=DISPLAY_INTERVAL_MS){lastDisplayAt=now;drawMain();}
  if(now-lastMqttAt>=MQTT_INTERVAL_MS){lastMqttAt=now;publishMqtt();}
  delay(1);
}
