#include <Arduino.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

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

// Configuration MQTT
const char* mqtt_server = "76.13.43.190";
const int mqtt_port = 1883;
const char* mqtt_user = "lampe_user";
const char* mqtt_password = "56nq2fd4ntt2yw9g";

WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastReconnectAttempt = 0;

// Client NTP
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000);

unsigned long lastUpdate = 0;

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
    b = p * 255; // Ajout du bleu pour finir sur un blanc complet (255, 255, 255)
  }
  return strip.Color(r, g, b);
}

void setupWiFi() {
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
}

void setupTime() {
  // Configuration du fuseau horaire de Paris
  setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
  tzset();
  timeClient.begin();
}

void updateTime() {
  timeClient.update();
}

void runDawnAnimation() {
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

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrivé sur le topic: ");
  Serial.println(topic);

  JsonDocument doc; // Compatible avec ArduinoJson 7
  DeserializationError error = deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("Erreur de parsing JSON: ");
    Serial.println(error.c_str());
    return;
  }

  if (doc.containsKey("wakeUpHour")) wakeUpHour = doc["wakeUpHour"];
  if (doc.containsKey("wakeUpMinute")) wakeUpMinute = doc["wakeUpMinute"];
  if (doc.containsKey("sleepHour")) sleepHour = doc["sleepHour"];
  if (doc.containsKey("sleepMinute")) sleepMinute = doc["sleepMinute"];
  if (doc.containsKey("dawnDuration")) dawnDuration = doc["dawnDuration"];

  Serial.println("Configuration mise à jour via MQTT");
}

void maintainConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      Serial.println("Perte du WiFi, tentative de reconnexion...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
    return; // On ne peut pas connecter MQTT sans WiFi
  }

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      Serial.print("Tentative de connexion MQTT...");
      String clientId = "ESP32Client-";
      clientId += String(random(0xffff), HEX);
      
      if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
        Serial.println("connecté");
        client.subscribe("communic8/lampe/config");
        lastReconnectAttempt = 0; // Réinitialise pour les futurs appels
      } else {
        Serial.print("échec, rc=");
        Serial.print(client.state());
        Serial.println(" - prochaine tentative dans 5s");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  // Initialisation du ruban LED
  strip.begin();
  strip.clear();
  strip.show();
  
  setupWiFi();
  setupTime();
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
}

void loop() {
  updateTime();
  maintainConnection();
  
  if (client.connected()) {
    client.loop();
  }
  
  unsigned long now = millis();
  // Mise à jour non-bloquante toutes les 500ms
  if (now - lastUpdate > 500) {
    lastUpdate = now;
    runDawnAnimation();
  }
}