#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <IRremote.h>

/* MQTT */
#define MQTT_SERVER "broker.hivemq.com"
#define MQTT_PORT 1883

/* IR CODES */
#define IR_R1  0xF6090707
#define IR_R2  0xF50A0707
#define IR_R3  0xF30C0707
#define IR_R4  0xF20D0707
#define IR_PIR 0xD22D0707

/* PINS */
int relayPin[4] = {26,27,32,33};

#define PIR_PIN 34
#define IR_PIN 15
#define WIFI_BTN 13
#define WIFI_LED 2
#define PIR_LED 4

/* PIR TIMER */
#define PIR_TIMEOUT (10UL * 60UL * 1000UL)

/* AP MODE */
const char* AP_SSID="ESP32-Control";
const char* AP_PASS="12345678";

/* OBJECTS */
WebServer server(80);
WiFiClient espClient;
PubSubClient mqtt(espClient);
Preferences prefs;

/* STATES */
bool relayState[4]={0,0,0,0};
bool pirEnabled=false;
bool apMode=false;

unsigned long lastMotionTime=0;
unsigned long lastWifiCheck=0;

#define WIFI_RETRY_INTERVAL 10000

String savedSSID;
String savedPASS;

String deviceID;
String baseTopic;

/* DASHBOARD PAGE */

const char HOME_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Smart Hub</title>

<style>

body{
font-family:Arial;
background:linear-gradient(135deg,#141e30,#243b55);
color:white;
text-align:center;
margin:0;
}

.container{
padding:40px;
}

.card{
background:white;
color:black;
padding:25px;
margin:auto;
max-width:320px;
border-radius:12px;
box-shadow:0 10px 25px rgba(0,0,0,0.4);
}

button{
width:100%;
padding:15px;
margin:10px 0;
font-size:16px;
border:none;
border-radius:8px;
cursor:pointer;
}

.wifi{background:#2196F3;color:white;}
.control{background:#4CAF50;color:white;}

</style>
</head>

<body>

<div class="container">

<h2>ESP32 Smart Hub</h2>

<div class="card">

<button class="wifi" onclick="location.href='/wifi'">Change WiFi</button>

<button class="control" onclick="location.href='/control'">Control Relays</button>

</div>

</div>

</body>
</html>
)rawliteral";

/* WIFI PAGE */

const char WIFI_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>WiFi Setup</title>

<style>

body{
font-family:Arial;
background:#f2f2f2;
text-align:center;
}

.card{
background:white;
padding:20px;
margin:50px auto;
width:300px;
border-radius:10px;
box-shadow:0 5px 15px rgba(0,0,0,0.2);
}

input{
width:100%;
padding:10px;
margin:8px 0;
border:1px solid #ccc;
border-radius:6px;
}

button{
width:100%;
padding:12px;
background:#2196F3;
color:white;
border:none;
border-radius:6px;
}

</style>
</head>

<body>

<div class="card">

<h2>WiFi Setup</h2>

<form action="/save" method="POST">

<input name="s" placeholder="WiFi Name">

<input name="p" type="password" placeholder="Password">

<button>Save & Restart</button>

</form>

</div>

</body>
</html>
)rawliteral";

/* CONTROL PAGE */

const char CONTROL_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>

<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Relay Control</title>

<style>

body{
font-family:Arial;
background:#1e1e1e;
color:white;
text-align:center;
}

.card{
background:#2c2c2c;
padding:25px;
margin:40px auto;
max-width:320px;
border-radius:12px;
}

button{
width:100%;
padding:16px;
margin:8px 0;
font-size:16px;
border:none;
border-radius:8px;
cursor:pointer;
background:#4CAF50;
color:white;
}

</style>

<script>

function toggle(r){

fetch("/toggle?relay="+r);

}

</script>

</head>

<body>

<h2>Relay Control</h2>

<div class="card">

<button onclick="toggle(1)">Relay 1</button>
<button onclick="toggle(2)">Relay 2</button>
<button onclick="toggle(3)">Relay 3</button>
<button onclick="toggle(4)">Relay 4</button>

</div>

</body>
</html>
)rawliteral";

/* WIFI STORAGE */

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

/* MQTT CALLBACK */

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

/* MQTT CONNECT */

void connectMQTT(){

static unsigned long lastTry=0;

if(millis()-lastTry<3000) return;

lastTry=millis();

if(mqtt.connect(deviceID.c_str())){

mqtt.subscribe((baseTopic+"/#").c_str());

Serial.println("MQTT connected");

}

}

/* WIFI RECONNECT */

void checkWiFiReconnect(){

if(apMode) return;

if(WiFi.status()==WL_CONNECTED) return;

if(millis()-lastWifiCheck<WIFI_RETRY_INTERVAL) return;

lastWifiCheck=millis();

WiFi.disconnect();

WiFi.begin(savedSSID.c_str(),savedPASS.c_str());

}

/* SETUP */

void setup(){

Serial.begin(115200);

pinMode(WIFI_LED,OUTPUT);
pinMode(PIR_LED,OUTPUT);
pinMode(WIFI_BTN,INPUT_PULLUP);
pinMode(PIR_PIN,INPUT);

for(int i=0;i<4;i++){

pinMode(relayPin[i],OUTPUT);
digitalWrite(relayPin[i],HIGH);

}

IrReceiver.begin(IR_PIN);

deviceID="ESP32_"+String((uint64_t)ESP.getEfuseMac(),HEX);
baseTopic="home/"+deviceID;

/* AP MODE */

if(digitalRead(WIFI_BTN)==LOW){

WiFi.softAP(AP_SSID,AP_PASS);

apMode=true;

Serial.println("AP MODE");

server.on("/",[](){server.send_P(200,"text/html",HOME_PAGE);});

server.on("/wifi",[](){server.send_P(200,"text/html",WIFI_PAGE);});

server.on("/control",[](){server.send_P(200,"text/html",CONTROL_PAGE);});

server.on("/toggle",[](){

int r=server.arg("relay").toInt()-1;

if(r>=0 && r<4){

relayState[r]=!relayState[r];
digitalWrite(relayPin[r],relayState[r]?LOW:HIGH);

}

server.send(200,"text/plain","OK");

});

server.on("/save",[](){

saveWiFi(server.arg("s"),server.arg("p"));

server.send(200,"text/html","Saved. Rebooting...");

delay(1000);

ESP.restart();

});

server.begin();

}

/* NORMAL WIFI */

else if(loadWiFi(savedSSID,savedPASS)){

WiFi.begin(savedSSID.c_str(),savedPASS.c_str());

}

mqtt.setServer(MQTT_SERVER,MQTT_PORT);

mqtt.setCallback(mqttCallback);

}

/* LOOP */

void loop(){

checkWiFiReconnect();

if(apMode){
server.handleClient();
}

if(WiFi.status()==WL_CONNECTED){

digitalWrite(WIFI_LED,HIGH);

if(!mqtt.connected()) connectMQTT();

mqtt.loop();

}

else{
digitalWrite(WIFI_LED,LOW);
}

/* IR CONTROL */

if(IrReceiver.decode()){

uint32_t c=IrReceiver.decodedIRData.decodedRawData;

if(c==IR_R1){relayState[0]=!relayState[0];digitalWrite(relayPin[0],relayState[0]?LOW:HIGH);}
if(c==IR_R2){relayState[1]=!relayState[1];digitalWrite(relayPin[1],relayState[1]?LOW:HIGH);}
if(c==IR_R3){relayState[2]=!relayState[2];digitalWrite(relayPin[2],relayState[2]?LOW:HIGH);}
if(c==IR_R4){relayState[3]=!relayState[3];digitalWrite(relayPin[3],relayState[3]?LOW:HIGH);}

if(c==IR_PIR){
pirEnabled=!pirEnabled;
digitalWrite(PIR_LED,pirEnabled);
}

IrReceiver.resume();

}

/* PIR */

if(pirEnabled && digitalRead(PIR_PIN)) lastMotionTime=millis();

if(pirEnabled && millis()-lastMotionTime>PIR_TIMEOUT){

for(int i=0;i<4;i++){
relayState[i]=false;
digitalWrite(relayPin[i],HIGH);
}

lastMotionTime=millis();

}

}