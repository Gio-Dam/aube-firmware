#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>

// Variables globales Wi-Fi à remplir par l'utilisateur
const char* ssid = "SFR_0F08";
const char* password = "4btk45txpg2ibu79kkxv";

// Configuration du ruban LED
#define PIN 16
#define NUMPIXELS 30
Adafruit_NeoPixel strip(NUMPIXELS, PIN, NEO_GRB + NEO_KHZ800);

// Variables de configuration de l'aube/coucher
int wakeUpHour = 7;
int wakeUpMinute = 0;
int sleepHour = 22;
int sleepMinute = 0;
int dawnDuration = 30; // en minutes

// Serveur Web Asynchrone sur le port 80
AsyncWebServer server(80);

// Client NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

// Page HTML intégrée
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Simulateur d'Aube</title>
<style>
body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #121212; color: #ffffff; display: flex; flex-direction: column; align-items: center; justify-content: center; min-height: 100vh; margin: 0; }
.card { background-color: #1e1e1e; padding: 2rem; border-radius: 12px; box-shadow: 0 8px 16px rgba(0,0,0,0.5); width: 90%; max-width: 400px; box-sizing: border-box; }
h1 { margin-top: 0; text-align: center; color: #ffb74d; }
.form-group { margin-bottom: 1.2rem; display: flex; flex-direction: column; }
label { margin-bottom: 0.5rem; font-size: 0.9rem; color: #cccccc; }
input { padding: 0.6rem; border: 1px solid #333; border-radius: 6px; background-color: #2c2c2c; color: white; font-size: 1rem; box-sizing: border-box; width: 100%; }
input:focus { outline: 2px solid #ffb74d; border-color: transparent; }
button { padding: 0.8rem; background-color: #ffb74d; color: #121212; border: none; border-radius: 6px; font-size: 1rem; font-weight: bold; cursor: pointer; transition: background-color 0.2s; margin-top: 1rem; width: 100%; box-sizing: border-box;}
button:hover { background-color: #ffa726; }
.toast { visibility: hidden; min-width: 250px; background-color: #4CAF50; color: #fff; text-align: center; border-radius: 6px; padding: 16px; position: fixed; z-index: 1; bottom: 30px; left: 50%; transform: translateX(-50%); }
.toast.show { visibility: visible; animation: fadein 0.5s, fadeout 0.5s 2.5s; }
@keyframes fadein { from {bottom: 0; opacity: 0;} to {bottom: 30px; opacity: 1;} }
@keyframes fadeout { from {bottom: 30px; opacity: 1;} to {bottom: 0; opacity: 0;} }
</style>
</head>
<body>
<div class="card">
  <h1>Simulateur d'Aube</h1>
  <div class="form-group">
    <label for="wakeUpTime">Heure de réveil</label>
    <input type="time" id="wakeUpTime">
  </div>
  <div class="form-group">
    <label for="sleepTime">Heure de coucher</label>
    <input type="time" id="sleepTime">
  </div>
  <div class="form-group">
    <label for="dawnDuration">Durée de l'aube (minutes)</label>
    <input type="number" id="dawnDuration" min="1" max="120">
  </div>
  <button onclick="saveSettings()">Enregistrer</button>
</div>
<div id="toast" class="toast">Paramètres enregistrés !</div>

<script>
window.onload = function() {
  fetch('/get')
    .then(response => response.json())
    .then(data => {
      document.getElementById('wakeUpTime').value = data.wakeUp;
      document.getElementById('sleepTime').value = data.sleep;
      document.getElementById('dawnDuration').value = data.duration;
    });
};

function saveSettings() {
  const wakeUp = document.getElementById('wakeUpTime').value;
  const sleep = document.getElementById('sleepTime').value;
  const duration = document.getElementById('dawnDuration').value;
  
  fetch(`/update?wakeUp=${wakeUp}&sleep=${sleep}&duration=${duration}`)
    .then(response => {
      if(response.ok) {
        const toast = document.getElementById("toast");
        toast.className = "toast show";
        setTimeout(() => { toast.className = toast.className.replace("show", ""); }, 3000);
      }
    });
}
</script>
</body>
</html>
)rawliteral";

uint32_t getColorForProgress(float progress) {
  uint8_t r = 0, g = 0, b = 0;
  
  if (progress <= 0.333f) {
    float p = progress / 0.333f;
    r = p * 255;
  } else if (progress <= 0.666f) {
    float p = (progress - 0.333f) / 0.333f;
    r = 255;
    g = p * 128;
  } else {
    float p = (progress - 0.666f) / 0.334f;
    r = 255;
    g = 128 + p * 127;
  }
  return strip.Color(r, g, b);
}

void setup() {
  Serial.begin(115200);
  
  // Initialisation du ruban LED
  strip.begin();
  strip.clear();
  strip.show();
  
  // Connexion Wi-Fi
  if (String(ssid) != "") {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connexion au WiFi");
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nWiFi connecté.");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("SSID non configuré. Mode hors ligne.");
  }

  // Configuration du fuseau horaire de Paris
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();

  timeClient.begin();

  // Configuration des routes du serveur web
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", (const uint8_t*)index_html, sizeof(index_html) - 1);
  });

  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request){
    char json[128];
    snprintf(json, sizeof(json), "{\"wakeUp\":\"%02d:%02d\",\"sleep\":\"%02d:%02d\",\"duration\":%d}", 
             wakeUpHour, wakeUpMinute, sleepHour, sleepMinute, dawnDuration);
    request->send(200, "application/json", json);
  });

  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("wakeUp")) {
      String wakeUp = request->getParam("wakeUp")->value();
      wakeUpHour = wakeUp.substring(0, 2).toInt();
      wakeUpMinute = wakeUp.substring(3, 5).toInt();
    }
    if (request->hasParam("sleep")) {
      String sleep = request->getParam("sleep")->value();
      sleepHour = sleep.substring(0, 2).toInt();
      sleepMinute = sleep.substring(3, 5).toInt();
    }
    if (request->hasParam("duration")) {
      dawnDuration = request->getParam("duration")->value().toInt();
    }
    request->send(200, "text/plain", "OK");
  });

  server.begin();
}

unsigned long lastUpdate = 0;

void loop() {
  timeClient.update();
  
  unsigned long now = millis();
  // Mise à jour non-bloquante toutes les 500ms
  if (now - lastUpdate > 500) {
    lastUpdate = now;
    
    time_t epochTime = timeClient.getEpochTime();
    struct tm *ptm = localtime(&epochTime);
    int currentHour = ptm->tm_hour;
    int currentMinute = ptm->tm_min;
    int currentSecond = ptm->tm_sec;
    
    int currentTotalMinutes = currentHour * 60 + currentMinute;
    int wakeUpTotalMinutes = wakeUpHour * 60 + wakeUpMinute;
    
    int dawnStartMinutes = wakeUpTotalMinutes - dawnDuration;
    if (dawnStartMinutes < 0) {
        dawnStartMinutes += 1440;
    }

    bool isDawnTime = false;
    float progress = 0.0;
    
    // Détermination si on est dans la période d'aube
    if (dawnStartMinutes <= wakeUpTotalMinutes) {
        if (currentTotalMinutes >= dawnStartMinutes && currentTotalMinutes < wakeUpTotalMinutes) {
            isDawnTime = true;
            int elapsedMinutes = currentTotalMinutes - dawnStartMinutes;
            progress = (float)(elapsedMinutes * 60 + currentSecond) / (dawnDuration * 60.0f);
        }
    } else {
        // L'aube traverse minuit
        if (currentTotalMinutes >= dawnStartMinutes || currentTotalMinutes < wakeUpTotalMinutes) {
            isDawnTime = true;
            int elapsedMinutes;
            if (currentTotalMinutes >= dawnStartMinutes) {
                elapsedMinutes = currentTotalMinutes - dawnStartMinutes;
            } else {
                elapsedMinutes = (1440 - dawnStartMinutes) + currentTotalMinutes;
            }
            progress = (float)(elapsedMinutes * 60 + currentSecond) / (dawnDuration * 60.0f);
        }
    }

    // Gestion de l'état des LEDs
    if (isDawnTime) {
        if (progress > 1.0f) progress = 1.0f;
        if (progress < 0.0f) progress = 0.0f;
        
        uint32_t color = getColorForProgress(progress);
        strip.fill(color);
        strip.show();
    } else if (currentHour == sleepHour && currentMinute == sleepMinute) {
        // A l'heure du coucher, on éteint
        strip.clear();
        strip.show();
    }
  }
}