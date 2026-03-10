#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <IRremote.hpp>

/* ===================== CONFIG ===================== */
#define MQTT_SERVER "broker.hivemq.com"
#define MQTT_PORT 1883

#define IR_R1  0xF6090707
#define IR_R2  0xF50A0707
#define IR_R3  0xF30C0707
#define IR_R4  0xF20D0707
#define IR_PIR 0xD22D0707

int relayPin[4] = {26,27,32,33};
#define PIR_PIN 34
#define IR_PIN 15
#define WIFI_BTN 13
#define WIFI_LED 2
#define PIR_LED 4

#define PIR_TIMEOUT (10UL * 60UL * 1000UL)
#define WIFI_RETRY_INTERVAL 10000
const char* AP_SSID="ESP32-Control";
const char* AP_PASS="12345678";

/* ===================== OBJECTS ===================== */
WebServer server(80);
WiFiClient espClient;
PubSubClient mqtt(espClient);
Preferences prefs;

/* ===================== STATES ===================== */
bool relayState[4]={0,0,0,0};
bool pirEnabled=false;
bool apMode=false;
unsigned long lastMotionTime=0;
unsigned long lastWifiCheck=0;
String savedSSID;
String savedPASS;
String deviceID;
String baseTopic;

/* ===================== WEB PAGES ===================== */
const char MENU_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 AP Menu</title>
<style>
body{font-family:Arial;text-align:center;background:#1e1e1e;color:white;padding:50px;}
button{width:200px;padding:15px;margin:15px;font-size:18px;border:none;border-radius:8px;cursor:pointer;}
.relay{background:#4CAF50;color:white;}
.wifi{background:#2196F3;color:white;}
</style>
</head>
<body>
<h2>ESP32 AP Menu</h2>
<button class="relay" onclick="location.href='/relay'">Relay Control</button><br>
<button class="wifi" onclick="location.href='/wifi'">Wi-Fi Setup</button>
</body>
</html>
)rawliteral";

const char WIFI_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Wi-Fi Setup</title>
<style>
body{font-family:Arial;text-align:center;background:#f2f2f2;padding:50px;}
input{width:80%;padding:10px;margin:10px;}
button{padding:12px;width:50%;background:#2196F3;color:white;border:none;border-radius:6px;cursor:pointer;}
</style>
</head>
<body>
<h2>Wi-Fi Setup</h2>
<form action="/savewifi" method="POST">
<input name="s" placeholder="SSID"><br>
<input name="p" placeholder="Password"><br>
<button>Save & Reboot</button>
</form>
</body>
</html>
)rawliteral";

const char RELAY_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Relay Control</title>
<style>
body{font-family:'Segoe UI';text-align:center;background:#1e1e1e;color:white;padding:20px;}
.container{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:20px;max-width:600px;margin:auto;}
.card{background: rgba(255,255,255,0.1);padding:20px;border-radius:15px;box-shadow:0 8px 25px rgba(0,0,0,0.5);transition: transform 0.2s;}
.card:hover{transform: scale(1.05);}
button{width:100%;padding:12px;margin-top:10px;font-size:16px;border:none;border-radius:8px;cursor:pointer;transition:0.2s;}
button.on{background:#20bf6b;color:white;}
button.off{background:#ff6b6b;color:white;}
.status{font-weight:bold;margin-top:8px;}
</style>
<script>
function toggleRelay(n){ fetch("/toggle?relay="+n).then(()=>updateStatus(n)); }
function updateStatus(n){ fetch("/status?relay="+n).then(resp=>resp.text()).then(state=>{ const btn=document.getElementById('btn'+n); const stat=document.getElementById('status'+n); if(state==='1'){btn.className='on'; stat.innerText='ON';} else{btn.className='off'; stat.innerText='OFF';} }); }
function refreshStatus(){ for(let i=1;i<=4;i++){ updateStatus(i); } }
setInterval(refreshStatus,2000);
window.onload=refreshStatus;
</script>
</head>
<body>
<h2>Relay Control</h2>
<div class="container">
  <div class="card"><h3>Relay 1</h3><button id="btn1" onclick="toggleRelay(1)">Toggle</button><div class="status" id="status1">--</div></div>
  <div class="card"><h3>Relay 2</h3><button id="btn2" onclick="toggleRelay(2)">Toggle</button><div class="status" id="status2">--</div></div>
  <div class="card"><h3>Relay 3</h3><button id="btn3" onclick="toggleRelay(3)">Toggle</button><div class="status" id="status3">--</div></div>
  <div class="card"><h3>Relay 4</h3><button id="btn4" onclick="toggleRelay(4)">Toggle</button><div class="status" id="status4">--</div></div>
</div>
</body>
</html>
)rawliteral";

/* ===================== WIFI STORAGE ===================== */
bool loadWiFi(String &s,String &p){
  prefs.begin("wifi",true);
  s=prefs.getString("s","");
  p=prefs.getString("p","");
  prefs.end();
  return s.length();
}
void saveWiFi(String s,String p){
  prefs.begin("wifi",false);
  prefs.putString("s",s);
  prefs.putString("p",p);
  prefs.end();
}

/* ===================== MQTT CALLBACK ===================== */
void mqttCallback(char* topic,byte* payload,unsigned int len){
  String msg;
  for(int i=0;i<len;i++) msg+=(char)payload[i];
  if(String(topic)==baseTopic+"/pir"){
    pirEnabled=(msg=="ON");
    digitalWrite(PIR_LED,pirEnabled);
    lastMotionTime=millis();
  }
  for(int i=0;i<4;i++){
    if(String(topic)==baseTopic+"/relay/"+String(i+1)){
      relayState[i]=(msg=="ON");
      digitalWrite(relayPin[i],relayState[i]?LOW:HIGH);
    }
  }
}

/* ===================== MQTT CONNECT ===================== */
void connectMQTT(){
  static unsigned long lastTry=0;
  if(millis()-lastTry<3000) return;
  lastTry=millis();
  if(mqtt.connect(deviceID.c_str())){
    mqtt.subscribe((baseTopic+"/#").c_str());
    Serial.println("MQTT connected");
  }
}

/* ===================== WIFI RECONNECT ===================== */
void checkWiFiReconnect(){
  if(apMode) return;
  if(WiFi.status()==WL_CONNECTED) return;
  if(millis()-lastWifiCheck<WIFI_RETRY_INTERVAL) return;
  lastWifiCheck=millis();
  WiFi.disconnect();
  WiFi.begin(savedSSID.c_str(),savedPASS.c_str());
}

/* ===================== SETUP ===================== */
void setup(){
  Serial.begin(115200);
  pinMode(WIFI_LED,OUTPUT); pinMode(PIR_LED,OUTPUT);
  pinMode(WIFI_BTN,INPUT_PULLUP); pinMode(PIR_PIN,INPUT);
  for(int i=0;i<4;i++){ pinMode(relayPin[i],OUTPUT); digitalWrite(relayPin[i],HIGH); }

  IrReceiver.begin(IR_PIN);

  deviceID="ESP32_"+String((uint64_t)ESP.getEfuseMac(),HEX);
  baseTopic="home/"+deviceID;

  /* ===================== AP MENU MODE ===================== */
  if(digitalRead(WIFI_BTN)==LOW){
    WiFi.softAP(AP_SSID,AP_PASS);
    apMode=true;
    Serial.println("AP MODE: Menu");
    Serial.print("AP IP Address: "); Serial.println(WiFi.softAPIP());

    server.on("/",[](){ server.send_P(200,"text/html",MENU_PAGE); });
    server.on("/wifi",[](){ server.send_P(200,"text/html",WIFI_PAGE); });
    server.on("/savewifi",[]{ saveWiFi(server.arg("s"),server.arg("p")); server.send(200,"text/html","Saved. Rebooting..."); delay(1000); ESP.restart(); });
    server.on("/relay",[](){ server.send_P(200,"text/html",RELAY_PAGE); });
    server.on("/toggle",[]( ){ int r=server.arg("relay").toInt()-1; if(r>=0 && r<4){ relayState[r]=!relayState[r]; digitalWrite(relayPin[r],relayState[r]?LOW:HIGH);} server.send(200,"text/plain","OK"); });
    server.on("/status",[]( ){ int r=server.arg("relay").toInt()-1; if(r>=0 && r<4) server.send(200,"text/plain",relayState[r]?"1":"0"); else server.send(200,"text/plain","0"); });

    server.begin();
    return;
  }

  /* ===================== NORMAL WIFI ===================== */
  else if(loadWiFi(savedSSID,savedPASS)){
    WiFi.begin(savedSSID.c_str(),savedPASS.c_str());
    Serial.print("Connecting to Wi-Fi");
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis()-start < 15000){
      Serial.print(".");
      delay(500);
    }
    if(WiFi.status() == WL_CONNECTED){
      Serial.println();
      Serial.print("STA IP Address: "); Serial.println(WiFi.localIP());
    } else Serial.println("\nFailed to connect to Wi-Fi.");
  }

  mqtt.setServer(MQTT_SERVER,MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  server.on("/",[](){ server.send_P(200,"text/html",RELAY_PAGE); });
  server.on("/toggle",[]( ){ int r=server.arg("relay").toInt()-1; if(r>=0 && r<4){ relayState[r]=!relayState[r]; digitalWrite(relayPin[r],relayState[r]?LOW:HIGH);} server.send(200,"text/plain","OK"); });
  server.on("/status",[]( ){ int r=server.arg("relay").toInt()-1; if(r>=0 && r<4) server.send(200,"text/plain",relayState[r]?"1":"0"); else server.send(200,"text/plain","0"); });
  server.begin();
}

/* ===================== LOOP ===================== */
void loop(){
  checkWiFiReconnect();
  server.handleClient();

  if(WiFi.status()==WL_CONNECTED){
    digitalWrite(WIFI_LED,HIGH);
    if(!mqtt.connected()) connectMQTT();
    mqtt.loop();
  } else digitalWrite(WIFI_LED,LOW);

  /* IR CONTROL */
  if(IrReceiver.decode()){
    uint32_t c=IrReceiver.decodedIRData.decodedRawData;
    if(c==IR_R1){relayState[0]=!relayState[0]; digitalWrite(relayPin[0],relayState[0]?LOW:HIGH);}
    if(c==IR_R2){relayState[1]=!relayState[1]; digitalWrite(relayPin[1],relayState[1]?LOW:HIGH);}
    if(c==IR_R3){relayState[2]=!relayState[2]; digitalWrite(relayPin[2],relayState[2]?LOW:HIGH);}
    if(c==IR_R4){relayState[3]=!relayState[3]; digitalWrite(relayPin[3],relayState[3]?LOW:HIGH);}
    if(c==IR_PIR){ pirEnabled=!pirEnabled; digitalWrite(PIR_LED,pirEnabled);}
    IrReceiver.resume();
  }

  /* PIR AUTOMATION */
  if(pirEnabled && digitalRead(PIR_PIN)) lastMotionTime=millis();
  if(pirEnabled && millis()-lastMotionTime>PIR_TIMEOUT){
    for(int i=0;i<4;i++){ relayState[i]=false; digitalWrite(relayPin[i],HIGH); }
    lastMotionTime=millis();
  }
}