#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>

#define HARDWARE_MODEL "c8-alpha"
#define FIRMWARE_VERSION "1.0.0"
#define API_BASE_URL "https://iot.comm-unic8.fr"
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

String chipId;
String mqttTopicConfig;
String mqttTopicState;
String mqttTopicStatus;
String mqttClientId;

Preferences preferences;
bool wifiProvisioned = false;
String storedSSID = "";
String storedPass = "";

WebServer server(80);
bool inAPMode = false;

// Variables pour les tentatives de reconnexion WiFi
int wifiFailedAttempts = 0;
const int MAX_WIFI_FAILURES = 6; // 6 tentatives x 5s = 30 secondes avant passage en AP
unsigned long lastWifiReconnectAttempt = 0;

// Configuration du ruban LED
#define PIN 16
uint16_t numLeds = 30;
Adafruit_NeoPixel strip(numLeds, PIN, NEO_GRB + NEO_KHZ800);

// Variables de configuration de l'aube/coucher
struct DayConfig {
  bool isActive;
  uint8_t wakeUpHour;
  uint8_t wakeUpMinute;
  uint8_t sleepHour;
  uint8_t sleepMinute;
  uint8_t fadeWakeUp;
  uint8_t fadeSleep;
};

DayConfig weeklySchedules[7];
String exceptions[10];
int numExceptions = 0;

// Palettes réalistes pour aube et crépuscule
uint32_t wakeUpColors[7] = {0x000000, 0x08081A, 0x2B164D, 0x8B2252, 0xFF4040, 0xFF7F00, 0xFFE4B5};
int numWakeUpColors = 7;
uint32_t sleepColors[7] = {0xFFF1E0, 0xFFB90F, 0xFF6103, 0xB22222, 0x191970, 0x000022, 0x000000};
int numSleepColors = 7;

// Variables du mode Live (Télécommande)
bool isLiveMode = false;
bool isLampOn = true;
uint8_t currentR = 255, currentG = 140, currentB = 0;
uint8_t globalBrightness = 255;
String currentEffect = "static";
uint8_t effectSpeed = 50;
bool useDefaultEffectColors = true;
uint32_t effectColors[10] = {0};
int numEffectColors = 0;

// Variables pour le Timer Live
unsigned long liveTimerStart = 0;
unsigned long activeTimerDuration = 0;
bool isTimerActive = false;

// Variables pour les transitions aube/crépuscule
unsigned long sunriseStartTime = 0;
unsigned long sunriseDurationMillis = 0;
unsigned long sunsetStartTime = 0;
unsigned long sunsetDurationMillis = 0;

// Configuration MQTT
const char* mqtt_server = "76.13.43.190";
const int mqtt_port = 1883;
const char* mqtt_user = "lampe_user";
const char* mqtt_password = "56nq2fd4ntt2yw9g";

WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastMqttReconnectAttempt = 0;

// Client NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

unsigned long lastUpdate = 0;
bool isTimeInitialized = false;

// --- GESTION DES ETATS ---

void publishState() {
  if (!client.connected()) return;
  
  JsonDocument doc;
  doc["state"] = isLampOn ? "ON" : "OFF";
  doc["brightness"] = globalBrightness;
  
  char hexColor[8];
  snprintf(hexColor, sizeof(hexColor), "#%02x%02x%02x", currentR, currentG, currentB);
  doc["color"] = String(hexColor);
  
  doc["effect"] = currentEffect;
  
  if (isTimerActive) {
    long remainingMillis = activeTimerDuration - (millis() - liveTimerStart);
    doc["timer"] = remainingMillis > 0 ? (remainingMillis / 60000) + 1 : 0;
  } else {
    doc["timer"] = 0;
  }

  String output;
  serializeJson(doc, output);
  
  // Publie avec retain=true pour que l'app web lise le dernier état immédiatement
  client.publish(mqttTopicState.c_str(), output.c_str(), true);
  Serial.print("Etat publié: ");
  Serial.println(output);
}

// --- GESTION DES LEDS ---

void pulseOrange() {
  // Oscillation douce entre 0.0 et 1.0 (période de ~3.14s)
  float val = (sin(millis() / 500.0) + 1.0) / 2.0; 
  uint8_t r = val * 255;
  uint8_t g = val * 100; // Un bel orange
  uint8_t b = 0;
  strip.fill(strip.Color(r, g, b));
  strip.show();
}

uint32_t getColorForProgress(float progress, uint32_t* colors, int count) {
  if (count == 0) return strip.Color(0, 0, 0);
  if (count == 1) return colors[0];
  if (progress <= 0.0f) return colors[0];
  if (progress >= 1.0f) return colors[count - 1];

  float scaled = progress * (count - 1);
  int index = (int)scaled;
  float fraction = scaled - index;

  uint8_t r1 = (colors[index] >> 16) & 0xFF;
  uint8_t g1 = (colors[index] >> 8) & 0xFF;
  uint8_t b1 = colors[index] & 0xFF;

  uint8_t r2 = (colors[index + 1] >> 16) & 0xFF;
  uint8_t g2 = (colors[index + 1] >> 8) & 0xFF;
  uint8_t b2 = colors[index + 1] & 0xFF;

  uint8_t r = r1 + (r2 - r1) * fraction;
  uint8_t g = g1 + (g2 - g1) * fraction;
  uint8_t b = b1 + (b2 - b1) * fraction;

  return strip.Color(r, g, b);
}

// Fonction spéciale pour l'aube et le crépuscule : mélange des couleurs et application de la luminosité en calcul flottant 
// pour éviter les sauts de valeurs et la perte de couleurs à basse luminosité.
uint32_t getSmoothColorForProgress(float progress, const uint32_t* colors, int count, float brightnessScale) {
  if (count == 0) return strip.Color(0, 0, 0);
  
  if (progress <= 0.0f) progress = 0.0f;
  if (progress >= 1.0f) progress = 0.99999f; // Evite de dépasser l'index maximum

  float scaled = progress * (count - 1);
  int index = (int)scaled;
  float fraction = scaled - index;

  uint8_t r1 = (colors[index] >> 16) & 0xFF;
  uint8_t g1 = (colors[index] >> 8) & 0xFF;
  uint8_t b1 = colors[index] & 0xFF;

  uint8_t r2 = (colors[index + 1] >> 16) & 0xFF;
  uint8_t g2 = (colors[index + 1] >> 8) & 0xFF;
  uint8_t b2 = colors[index + 1] & 0xFF;

  // 1. Mélange linéaire en flottant
  float r_f = r1 + (r2 - r1) * fraction;
  float g_f = g1 + (g2 - g1) * fraction;
  float b_f = b1 + (b2 - b1) * fraction;

  // 2. Application de l'échelle de luminosité globale (qui inclut déjà la progression gamma temporelle)
  r_f *= brightnessScale;
  g_f *= brightnessScale;
  b_f *= brightnessScale;

  // 3. Conversion en entiers (la courbe exponentielle a déjà été appliquée via brightnessScale)
  return strip.Color(
    (uint8_t)r_f,
    (uint8_t)g_f,
    (uint8_t)b_f
  );
}

uint32_t getWrappedColorForProgress(float progress, uint32_t* colors, int count) {
  if (count == 0) return strip.Color(0, 0, 0);
  if (count == 1) return colors[0];
  
  progress = progress - (long)progress;
  if (progress < 0) progress += 1.0;
  
  float scaled = progress * count;
  int index = (int)scaled;
  float fraction = scaled - index;
  
  int nextIndex = (index + 1) % count;
  
  uint8_t r1 = (colors[index] >> 16) & 0xFF;
  uint8_t g1 = (colors[index] >> 8) & 0xFF;
  uint8_t b1 = colors[index] & 0xFF;
  
  uint8_t r2 = (colors[nextIndex] >> 16) & 0xFF;
  uint8_t g2 = (colors[nextIndex] >> 8) & 0xFF;
  uint8_t b2 = colors[nextIndex] & 0xFF;
  
  uint8_t r = r1 + (r2 - r1) * fraction;
  uint8_t g = g1 + (g2 - g1) * fraction;
  uint8_t b = b1 + (b2 - b1) * fraction;
  
  return strip.Color(r, g, b);
}

// --- GESTION DU TEMPS ET ANIMATION ---

void setupTime() {
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  timeClient.begin();
  isTimeInitialized = true;
}

void updateTime() {
  if (wifiProvisioned && WiFi.status() == WL_CONNECTED) {
    timeClient.update();
  }
}


void checkSchedules() {
  if (!isTimeInitialized) return;

  time_t epochTime = timeClient.getEpochTime();
  if (epochTime < 100000) return;

  struct tm *ptm = localtime(&epochTime);
  int currentTotalMinutes = ptm->tm_hour * 60 + ptm->tm_min;
  
  static int lastTriggeredMinute = -1;
  if (lastTriggeredMinute == currentTotalMinutes) return; // Déjà vérifié pour cette minute

  // Vérification de la date et du jour pour les exceptions
  char dateStr[11];
  snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
  
  bool isException = false;
  for (int i = 0; i < numExceptions; i++) {
    if (exceptions[i] == String(dateStr)) {
      isException = true;
      break;
    }
  }

  if (isException) {
    lastTriggeredMinute = currentTotalMinutes;
    return;
  }

  DayConfig todayConfig = weeklySchedules[ptm->tm_wday];
  if (!todayConfig.isActive) {
    lastTriggeredMinute = currentTotalMinutes;
    return;
  }

  // Calcul du début de l'aube
  int dawnStartMinutes = (todayConfig.wakeUpHour * 60 + todayConfig.wakeUpMinute) - todayConfig.fadeWakeUp;
  if (dawnStartMinutes < 0) dawnStartMinutes += 1440;

  // Calcul du début du crépuscule
  int sleepStartMinutes = (todayConfig.sleepHour * 60 + todayConfig.sleepMinute) - todayConfig.fadeSleep;
  if (sleepStartMinutes < 0) sleepStartMinutes += 1440;

  if (currentTotalMinutes == dawnStartMinutes) {
    currentEffect = "sunrise";
    sunriseDurationMillis = (unsigned long)todayConfig.fadeWakeUp * 60000UL;
    sunriseStartTime = millis();
    isLiveMode = true;
    isLampOn = true;
    publishState();
    Serial.println("⏰ Déclenchement de l'aube locale !");
  } else if (currentTotalMinutes == sleepStartMinutes) {
    currentEffect = "sunset";
    sunsetDurationMillis = (unsigned long)todayConfig.fadeSleep * 60000UL;
    sunsetStartTime = millis();
    isLiveMode = true;
    isLampOn = true;
    publishState();
    Serial.println("⏰ Déclenchement du crépuscule local !");
  } else {
    // Extinction totale à l'heure exacte du coucher
    int sleepTotalMinutes = todayConfig.sleepHour * 60 + todayConfig.sleepMinute;
    if (currentTotalMinutes == sleepTotalMinutes && isLampOn) {
        isLampOn = false;
        publishState();
        Serial.println("⏰ Extinction automatique (fin du crépuscule) !");
    }
  }

  lastTriggeredMinute = currentTotalMinutes;
}

// --- MQTT ---

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrivé sur le topic: ");
  Serial.println(topic);

  JsonDocument doc; 
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("Erreur de parsing JSON: ");
    Serial.println(error.c_str());
    return;
  }

  String action = "alarm"; // fallback
  if (doc["action"].is<String>()) {
    action = doc["action"].as<String>();
  }

  if (doc["numLeds"].is<uint16_t>()) {
    uint16_t newNumLeds = doc["numLeds"];
    if (newNumLeds != numLeds && newNumLeds > 0) {
      numLeds = newNumLeds;
      preferences.begin("config", false);
      preferences.putUShort("numLeds", numLeds);
      preferences.end();
      strip.updateLength(numLeds);
      Serial.print("Nombre de LEDs mis à jour: ");
      Serial.println(numLeds);
    }
  }

  if (action == "live") {
    isLiveMode = true;
    if (doc["state"].is<String>()) isLampOn = (doc["state"].as<String>() == "ON");
    if (doc["brightness"].is<int>()) globalBrightness = doc["brightness"];
    if (doc["effect"].is<String>()) {
      currentEffect = doc["effect"].as<String>();
      if (currentEffect == "sunrise") {
        sunriseDurationMillis = (doc["fadeWakeUp"].is<int>() ? (unsigned long)doc["fadeWakeUp"] : 30) * 60000UL;
        sunriseStartTime = millis();
        Serial.print("Aube démarrée pour ");
        Serial.print(sunriseDurationMillis / 60000UL);
        Serial.println(" minutes.");
      } else if (currentEffect == "sunset") {
        sunsetDurationMillis = (doc["fadeSleep"].is<int>() ? (unsigned long)doc["fadeSleep"] : 30) * 60000UL;
        sunsetStartTime = millis();
        Serial.print("Crépuscule démarré pour ");
        Serial.print(sunsetDurationMillis / 60000UL);
        Serial.println(" minutes.");
      }
    }
    
    if (doc["effectSpeed"].is<int>()) effectSpeed = doc["effectSpeed"];
    if (doc["useDefaultEffectColors"].is<bool>()) useDefaultEffectColors = doc["useDefaultEffectColors"];
    
    if (doc["effectColors"].is<JsonArray>()) {
      JsonArray arr = doc["effectColors"].as<JsonArray>();
      numEffectColors = 0;
      for (JsonVariant v : arr) {
        if (numEffectColors < 10) {
          String hex = v.as<String>();
          if (hex.startsWith("#")) effectColors[numEffectColors++] = strtol(&hex[1], NULL, 16);
        }
      }
    }

    if (doc["color"].is<String>()) {
      String hexColor = doc["color"].as<String>();
      if (hexColor.startsWith("#") && hexColor.length() == 7) {
        long number = strtol(&hexColor[1], NULL, 16);
        currentR = number >> 16;
        currentG = number >> 8 & 0xFF;
        currentB = number & 0xFF;
      }
    }
    
    if (doc["timer"].is<int>()) {
      int t = doc["timer"];
      if (t > 0) {
        activeTimerDuration = t * 60000UL;
        liveTimerStart = millis();
        isTimerActive = true;
        Serial.print("Timer activé pour ");
        Serial.print(t);
        Serial.println(" minutes.");
      } else {
        isTimerActive = false;
      }
    }
    Serial.println("Action: LIVE appliquée");
    publishState();
  } else {
    isLiveMode = false;
    
    preferences.begin("config", false);
    
    if (doc["schedules"].is<JsonArray>()) {
      JsonArray arr = doc["schedules"].as<JsonArray>();
      for (JsonVariant v : arr) {
        if (v["day"].is<int>()) {
          int day = v["day"];
          if (day >= 0 && day <= 6) {
            weeklySchedules[day].isActive = v["isActive"] | false;
            weeklySchedules[day].wakeUpHour = v["wakeUpHour"] | 7;
            weeklySchedules[day].wakeUpMinute = v["wakeUpMinute"] | 0;
            weeklySchedules[day].sleepHour = v["sleepHour"] | 22;
            weeklySchedules[day].sleepMinute = v["sleepMinute"] | 0;
            weeklySchedules[day].fadeWakeUp = v["fadeWakeUp"] | 30;
            weeklySchedules[day].fadeSleep = v["fadeSleep"] | 30;
          }
        }
      }
      preferences.putBytes("schedules", &weeklySchedules, sizeof(weeklySchedules));
    }
    // activeDays is replaced by schedules.
    
    if (doc["exceptions"].is<JsonArray>()) {
      JsonArray arr = doc["exceptions"].as<JsonArray>();
      numExceptions = 0;
      String excStr = "";
      for (JsonVariant v : arr) {
        if (numExceptions < 10) {
          String e = v.as<String>();
          exceptions[numExceptions++] = e;
          if (excStr.length() > 0) excStr += ",";
          excStr += e;
        }
      }
      preferences.putString("exceptions", excStr);
    }
    
    preferences.end();
    
    // Réinitialiser les déclencheurs pour appliquer immédiatement la nouvelle configuration
    // Les variables wasDawnTime et wasSleepTime ont été supprimées
    
    Serial.println("Action: ALARM appliquée et sauvegardée dans la mémoire Flash.");
  }
}

void publishStatus() {
  if (!client.connected()) return;
  JsonDocument doc;
  doc["version"] = FIRMWARE_VERSION;
  doc["model"] = HARDWARE_MODEL;
  String output;
  serializeJson(doc, output);
  client.publish(mqttTopicStatus.c_str(), output.c_str(), true);
  Serial.print("Statut publié: ");
  Serial.println(output);
}

void checkForUpdates() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure secureClient;
    secureClient.setInsecure(); // Contourne la vérification SSL
    
    HTTPClient http;
    String url = String(API_BASE_URL) + "/api/ota?deviceId=" + chipId;
    
    Serial.println("Vérification des mises à jour OTA...");
    http.begin(secureClient, url);
    int httpCode = http.GET();
    
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      JsonDocument doc;
      deserializeJson(doc, payload);
      
      if (doc["updateAvailable"] == true && doc["downloadUrl"].is<String>()) {
        String downloadUrl = doc["downloadUrl"].as<String>();
        Serial.println("Mise à jour disponible ! Téléchargement depuis : " + downloadUrl);
        
        HTTPClient httpOta;
        httpOta.begin(secureClient, downloadUrl);
        int dlCode = httpOta.GET();
        if (dlCode == HTTP_CODE_OK) {
          int contentLength = httpOta.getSize();
          bool canBegin = Update.begin(contentLength);
          if (canBegin) {
            WiFiClient * stream = httpOta.getStreamPtr();
            size_t written = Update.writeStream(*stream);
            if (written == contentLength) {
              Serial.println("Mise à jour réussie. Redémarrage...");
              if (Update.end()) {
                ESP.restart();
              }
            } else {
              Serial.println("Erreur de téléchargement du firmware.");
            }
          }
        }
        httpOta.end();
      } else {
        Serial.println("Aucune mise à jour disponible.");
      }
    } else {
      Serial.printf("Erreur HTTP lors de la vérification OTA : %d\n", httpCode);
    }
    http.end();
  }
}

void maintainMQTTConnection() {
  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt > 5000) {
      lastMqttReconnectAttempt = now;
      Serial.print("Tentative de connexion MQTT...");
      
      if (client.connect(mqttClientId.c_str(), mqtt_user, mqtt_password)) {
        Serial.println("connecté");
        client.subscribe(mqttTopicConfig.c_str());
        publishState();
        publishStatus();
        lastMqttReconnectAttempt = 0;
      } else {
        Serial.print("échec, rc=");
        Serial.print(client.state());
        Serial.println(" - prochaine tentative dans 5s");
      }
    }
  }
}

// --- GESTION DU WI-FI ---

void startAPMode() {
  Serial.println("Démarrage du point d'accès Wi-Fi (AP)...");
  
  WiFi.mode(WIFI_AP);
  String apName = "CommUnic8-Setup";
  
  // Définit l'IP de l'AP sur 192.168.4.1 (comportement par défaut, mais explicite)
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName.c_str());
  
  Serial.print("IP du point d'accès : ");
  Serial.println(WiFi.softAPIP());

  // Lancement du mDNS pour http://setup.local
  if (!MDNS.begin("setup")) {
    Serial.println("Erreur au démarrage du mDNS");
  } else {
    Serial.println("mDNS démarré. Accessible via http://setup.local");
  }

  // Configuration du WebServer
  server.on("/", HTTP_GET, []() {
    String html = R"HTML(
      <!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
      <style>
        body { font-family: -apple-system, system-ui, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif; text-align: center; margin: 0; padding: 20px; background: #0f172a; color: white; }
        .container { max-width: 400px; margin: 0 auto; background: #1e293b; padding: 30px; border-radius: 12px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
        h2 { margin-bottom: 20px; color: #38bdf8; }
        input { padding: 12px; margin: 10px 0; width: 100%; box-sizing: border-box; border-radius: 8px; border: 1px solid #334155; background: #0f172a; color: white; }
        input[type="submit"] { background: #38bdf8; color: #0f172a; font-weight: bold; font-size: 16px; border: none; cursor: pointer; margin-top: 20px; transition: background 0.3s; }
        input[type="submit"]:hover { background: #0ea5e9; }
      </style>
      </head><body>
      <div class="container">
        <h2>CommUnic8<br>Configuration</h2>
        <p style="font-size: 14px; color: #94a3b8; margin-bottom: 20px;">Veuillez entrer les identifiants de votre réseau Wi-Fi.</p>
        <form action="/save" method="POST">
          <input type="text" name="ssid" placeholder="Nom du réseau (SSID)" required>
          <input type="password" name="pass" placeholder="Mot de passe" required>
          <input type="submit" value="Se connecter">
        </form>
      </div>
      </body></html>
    )HTML";
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, []() {
    String ssid = server.arg("ssid");
    String pass = server.arg("pass");
    
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("pass", pass);
    preferences.end();
    
    String html = R"HTML(
      <!DOCTYPE html><html><head><meta name="viewport" content="width=device-width, initial-scale=1">
      <style>body { font-family: Arial, sans-serif; text-align: center; margin-top: 50px; background: #0f172a; color: #38bdf8; }</style>
      </head><body><h2>Sauvegarde réussie !</h2><p style="color:white;">La lampe va redémarrer et tenter de se connecter à : <b>)HTML" + ssid + R"HTML(</b></p></body></html>
    )HTML";
    
    server.send(200, "text/html", html);
    delay(2000);
    ESP.restart();
  });

  server.begin();
  inAPMode = true;
}

void connectToWiFi() {
  Serial.print("Connexion au Wi-Fi : ");
  Serial.println(storedSSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(storedSSID.c_str(), storedPass.c_str());
  
  unsigned long startAttemptTime = millis();
  // On remplace le delay bloquant par une boucle non bloquante pour l'animation
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000) {
    pulseOrange();
    delay(20);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connecté.");
    Serial.println(WiFi.localIP());
    wifiProvisioned = true;
    wifiFailedAttempts = 0;
    strip.clear();
    strip.show();
    if (!isTimeInitialized) {
      setupTime();
    }
  } else {
    Serial.println("\nÉchec de la connexion Wi-Fi.");
    wifiProvisioned = false;
    strip.clear();
    strip.show();
  }
}

// --- SETUP & LOOP ---

void setup() {
  Serial.begin(115200);
  
  uint64_t mac = ESP.getEfuseMac();
  char chipIdBuffer[18];
  snprintf(chipIdBuffer, sizeof(chipIdBuffer), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  chipId = String(chipIdBuffer);
  
  mqttClientId = "COMMUNIC8-" + chipId;
  mqttTopicConfig = "communic8/lampe/" + chipId + "/config";
  mqttTopicState = "communic8/lampe/" + chipId + "/state";
  mqttTopicStatus = "communic8/lampe/" + chipId + "/status";

  // Initialisation des données persistantes depuis la mémoire Flash (NVS)
  preferences.begin("config", false);
  numLeds = preferences.getUShort("numLeds", 30);
  
  size_t schLen = preferences.getBytes("schedules", &weeklySchedules, sizeof(weeklySchedules));
  if (schLen != sizeof(weeklySchedules)) {
    for (int i=0; i<7; i++) {
      weeklySchedules[i].isActive = true;
      weeklySchedules[i].wakeUpHour = 7;
      weeklySchedules[i].wakeUpMinute = 0;
      weeklySchedules[i].sleepHour = 22;
      weeklySchedules[i].sleepMinute = 0;
      weeklySchedules[i].fadeWakeUp = 30;
      weeklySchedules[i].fadeSleep = 30;
    }
  }
  
  // Utilisation des palettes par défaut d'aube et de crépuscule
  
  String excStr = preferences.getString("exceptions", "");
  numExceptions = 0;
  if (excStr.length() > 0) {
    int startIdx = 0;
    while(startIdx < excStr.length() && numExceptions < 10) {
      int commaIdx = excStr.indexOf(',', startIdx);
      if (commaIdx == -1) {
        exceptions[numExceptions++] = excStr.substring(startIdx);
        break;
      } else {
        exceptions[numExceptions++] = excStr.substring(startIdx, commaIdx);
        startIdx = commaIdx + 1;
      }
    }
  }
  preferences.end();
  
  strip.updateLength(numLeds);

  strip.begin();
  strip.clear();
  strip.show();
  
  Serial.println("\n==========================================");
  Serial.println("========= INFORMATIONS APPAREIL =========");
  Serial.println("CHIP ID : " + chipId);
  Serial.println("MQTT Client ID : " + mqttClientId);
  Serial.println("MQTT Topic (Config) : " + mqttTopicConfig);
  Serial.println("MQTT Topic (State) : " + mqttTopicState);
  Serial.println("==========================================");
  Serial.println(">>> LIEN D'ASSOCIATION (QR CODE) <<<");
  Serial.println("https://app.aube.studio/claim?deviceId=" + chipId);
  Serial.println("Ou utilisez l'identifiant manuel : " + chipId);
  Serial.println("==========================================\n");
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  preferences.begin("wifi", false);
  storedSSID = preferences.getString("ssid", "");
  storedPass = preferences.getString("pass", "");
  preferences.end();
  
  if (storedSSID.length() > 0) {
    connectToWiFi();
    if (!wifiProvisioned) {
      startAPMode();
    } else {
      checkForUpdates();
    }
  } else {
    startAPMode();
  }
}

void loop() {
  if (inAPMode) {
    server.handleClient();
    
    // Clignotement bleu franc sur toutes les LEDs en mode AP
    unsigned long now = millis();
    if (now - lastUpdate > 500) {
      lastUpdate = now;
      static bool ledState = false;
      if (ledState) {
        strip.fill(strip.Color(0, 0, 255));
      } else {
        strip.clear();
      }
      strip.show();
      ledState = !ledState;
    }
    return;
  }

  // Vérification de la connexion Wi-Fi
  if (wifiProvisioned && WiFi.status() != WL_CONNECTED) {
    pulseOrange(); // Pulsation douce pendant la recherche de réseau
    
    unsigned long now = millis();
    if (now - lastWifiReconnectAttempt > 5000) {
      lastWifiReconnectAttempt = now;
      wifiFailedAttempts++;
      Serial.print("Perte du WiFi, tentative de reconnexion ");
      Serial.print(wifiFailedAttempts);
      Serial.print("/");
      Serial.println(MAX_WIFI_FAILURES);
      
      if (wifiFailedAttempts >= MAX_WIFI_FAILURES) {
        Serial.println("Échec définitif. Passage en mode Point d'Accès.");
        wifiProvisioned = false;
        WiFi.disconnect();
        strip.clear();
        strip.show();
        startAPMode();
      } else {
        WiFi.disconnect();
        WiFi.reconnect();
      }
    }
    return; // Empêche l'exécution MQTT et horloge pendant la coupure Wi-Fi
  }

  // Retour de la connexion
  if (wifiProvisioned && wifiFailedAttempts > 0 && WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi reconnecté avec succès !");
    wifiFailedAttempts = 0;
    strip.clear();
    strip.show();
  }

  updateTime();
  maintainMQTTConnection();
  
  if (wifiProvisioned && client.connected()) {
    client.loop();
  }
  
  unsigned long now = millis();
  if (now - lastUpdate > 20) { // 20ms = ~50 FPS pour une fluidité parfaite
    lastUpdate = now;
    if (wifiProvisioned && WiFi.status() == WL_CONNECTED) {
      checkSchedules();
      
      // Gestion du timer
      if (isLiveMode && isTimerActive && isLampOn) {
          if (millis() - liveTimerStart >= activeTimerDuration) {
              isLampOn = false;
              isTimerActive = false;
              Serial.println("Fin du timer, extinction de la lampe.");
              publishState();
          }
      }
      
      if (!isLampOn) {
        strip.clear();
        strip.show();
      } else if (isLiveMode) {
        strip.setBrightness(globalBrightness);
        
        static uint16_t rainbowHue = 0;
        float speedMult = effectSpeed > 0 ? (effectSpeed / 50.0f) : 0.01f;
        
        if (currentEffect == "static") {
          strip.fill(strip.Color(currentR, currentG, currentB));
          strip.show();
        } else if (currentEffect == "pulse") {
          float val = (sin(millis() / (500.0 / speedMult)) + 1.0) / 2.0; 
          uint8_t r = val * currentR;
          uint8_t g = val * currentG;
          uint8_t b = val * currentB;
          
          if (!useDefaultEffectColors && numEffectColors > 0) {
            float progress = (sin(millis() / (1000.0 / speedMult)) + 1.0) / 2.0; 
            uint32_t color = getWrappedColorForProgress(progress, effectColors, numEffectColors);
            r = (color >> 16) & 0xFF;
            g = (color >> 8) & 0xFF;
            b = color & 0xFF;
          }
          
          strip.fill(strip.Color(r, g, b));
          strip.show();
        } else if (currentEffect == "wave") {
          for(int i=0; i<numLeds; i++) {
            float wave = (sin((i * 0.3) + (millis() / (300.0 / speedMult))) + 1.0) / 2.0;
            
            if (!useDefaultEffectColors && numEffectColors > 0) {
               float progressOffset = (millis() * speedMult) / 2000.0;
               uint32_t color = getWrappedColorForProgress(progressOffset + (i / 10.0), effectColors, numEffectColors);
               uint8_t r = ((color >> 16) & 0xFF) * wave;
               uint8_t g = ((color >> 8) & 0xFF) * wave;
               uint8_t b = (color & 0xFF) * wave;
               strip.setPixelColor(i, strip.Color(r, g, b));
            } else {
               strip.setPixelColor(i, strip.Color(currentR * wave, currentG * wave, currentB * wave));
            }
          }
          strip.show();
        } else if (currentEffect == "color_cycle") {
          strip.fill(strip.gamma32(strip.ColorHSV(rainbowHue)));
          strip.show();
          rainbowHue += (uint16_t)(256 * speedMult);
        } else if (currentEffect == "rainbow") {
          if (!useDefaultEffectColors && numEffectColors > 0) {
            float progressOffset = (millis() * speedMult) / 5000.0;
            for(int i=0; i<numLeds; i++) {
              float p = progressOffset + (i / (float)numLeds);
              strip.setPixelColor(i, getWrappedColorForProgress(p, effectColors, numEffectColors));
            }
            strip.show();
          } else {
            for(int i=0; i<numLeds; i++) {
              int pixelHue = rainbowHue + (i * 65536L / numLeds);
              strip.setPixelColor(i, strip.gamma32(strip.ColorHSV(pixelHue)));
            }
            strip.show();
            rainbowHue += (uint16_t)(256 * speedMult);
          }
        } else if (currentEffect == "chase") {
          static int chaseStep = 0;
          static unsigned long lastChaseUpdate = 0;
          if (millis() - lastChaseUpdate > (50 / speedMult)) {
            lastChaseUpdate = millis();
            strip.clear();
            for(int i=chaseStep; i<numLeds; i+=3) {
              if (!useDefaultEffectColors && numEffectColors > 0) {
                float p = (millis() * speedMult) / 2000.0;
                strip.setPixelColor(i, getWrappedColorForProgress(p, effectColors, numEffectColors));
              } else {
                strip.setPixelColor(i, strip.Color(currentR, currentG, currentB));
              }
            }
            strip.show();
            chaseStep++;
            if(chaseStep >= 3) chaseStep = 0;
          }
        } else if (currentEffect == "sparkle") {
          for(int i=0; i<numLeds; i++) {
            strip.setPixelColor(i, strip.Color(currentR/2, currentG/2, currentB/2));
          }
          if (random(100) < (50 * speedMult)) {
            int pixel = random(numLeds);
            if (!useDefaultEffectColors && numEffectColors > 0) {
               strip.setPixelColor(pixel, getWrappedColorForProgress(random(100)/100.0, effectColors, numEffectColors));
            } else {
               strip.setPixelColor(pixel, strip.Color(255, 255, 255));
            }
          }
          strip.show();
        } else if (currentEffect == "fire") {
          for(int i = 0; i < numLeds; i++) {
            int flicker = random(0, 50 * speedMult);
            int r1 = currentR - flicker;
            int g1 = currentG - flicker;
            int b1 = currentB - flicker;
            if (r1 < 0) r1 = 0;
            if (g1 < 0) g1 = 0;
            if (b1 < 0) b1 = 0;
            strip.setPixelColor(i, strip.Color(r1, g1, b1));
          }
          strip.show();
        } else if (currentEffect == "nightlight") {
          strip.fill(strip.Color(currentR / 4, currentG / 4, currentB / 4));
          strip.show();
        } else if (currentEffect == "sunrise") {
          float progress = 0.0f;
          if (sunriseDurationMillis > 0) {
            progress = (float)(millis() - sunriseStartTime) / sunriseDurationMillis;
          }
          if (progress > 1.0f) progress = 1.0f;
          if (progress < 0.0f) progress = 0.0f;
          
          // Utilisation de la nouvelle fonction pour une fluidité parfaite à très basse luminosité
          float gammaProgress = pow(progress, 2.0f); // Courbe douce sans écraser les couleurs
          float brightnessScale = gammaProgress * ((float)globalBrightness / 255.0f);
          uint32_t color = getSmoothColorForProgress(progress, wakeUpColors, numWakeUpColors, brightnessScale);
          
          strip.setBrightness(255); // On force à 255 pour ne pas doubler la division d'entiers
          strip.fill(color);
          strip.show();
        } else if (currentEffect == "sunset") {
          float progress = 0.0f;
          if (sunsetDurationMillis > 0) {
            progress = (float)(millis() - sunsetStartTime) / sunsetDurationMillis;
          }
          if (progress > 1.0f) progress = 1.0f;
          if (progress < 0.0f) progress = 0.0f;
          
          // Utilisation de la nouvelle fonction pour une fluidité parfaite à très basse luminosité
          float gammaProgress = pow(1.0f - progress, 2.0f); // Courbe douce sans écraser les couleurs
          float brightnessScale = gammaProgress * ((float)globalBrightness / 255.0f);
          uint32_t color = getSmoothColorForProgress(progress, sleepColors, numSleepColors, brightnessScale);
          
          strip.setBrightness(255); // On force à 255 pour ne pas doubler la division d'entiers
          strip.fill(color);
          strip.show();
        }
      }
    }
  }
}
