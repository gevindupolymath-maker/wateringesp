#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <time.h>

// ---------------- WiFi / MQTT ----------------
const char* WIFI1_SSID = "Dialog 4G 759";
const char* WIFI1_PASS = "1c9d03F7";

const char* WIFI2_SSID = "Polymath_Aruba";
const char* WIFI2_PASS = "Poly#@nano123";

const char* MQTT_HOST = "broker.emqx.io";
const int   MQTT_PORT = 1883;

const char* TOPIC_WATERING = "polywatering";
const char* TOPIC_CMD      = "polywatering/cmd";   // phone -> ESP commands

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------------- Watering Pins (NodeMCU labels) ----------------
// D3=GPIO0, D2=GPIO4, D4=GPIO2
static const uint8_t PIN_W1 = 0; // D3
static const uint8_t PIN_W2 = 4; // D2
static const uint8_t PIN_W3 = 2; // D4

// If your relay is active-LOW, set this to true
static const bool RELAY_ACTIVE_LOW = false;

// ---------------- Manual override state ----------------
volatile bool manualMode = false;          // false=AUTO, true=MANUAL
volatile bool manualWaterOn = false;       // used when manualMode=true
unsigned long manualUntilMs = 0;           // 0 = no timer (manual stays until OFF/AUTO)

// ---------------- Timing ----------------
unsigned long lastPublish = 0;
const unsigned long PUBLISH_MS = 1000;

unsigned long lastRainCheck = 0;
// check rain every 60 seconds (you still publish every second)
const unsigned long RAIN_CHECK_MS = 60000;

// ---------------- Time (Asia/Colombo UTC+5:30) ----------------
static const long  TZ_OFFSET_SEC = 5 * 3600 + 30 * 60; // +05:30
static const int   NTP_RESYNC_SEC = 6 * 3600;          // re-sync every 6h
unsigned long lastNtpSync = 0;

// ---------------- Open-Meteo API ----------------
const char* RAIN_URL =
  "https://api.open-meteo.com/v1/forecast?latitude=6.9271&longitude=79.8612&current=rain,precipitation";

// Latest rain info
bool  rainDataValid = false;  // if false => "data not coming" => water anyway
float currentRainMM = 0.0f;   // current.rain
float currentPrecMM = 0.0f;   // current.precipitation

// ---------------- Helpers ----------------
void setWateringOutputs(bool on) {
  int activeLevel = RELAY_ACTIVE_LOW ? LOW : HIGH;
  int idleLevel   = RELAY_ACTIVE_LOW ? HIGH : LOW;

  digitalWrite(PIN_W1, on ? activeLevel : idleLevel);
  digitalWrite(PIN_W2, on ? activeLevel : idleLevel);
  digitalWrite(PIN_W3, on ? activeLevel : idleLevel);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);

  Serial.println("Trying primary WiFi...");
  WiFi.begin(WIFI1_SSID, WIFI1_PASS);
  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to primary WiFi");
    Serial.println(WiFi.localIP());
    return;
  }

  Serial.println("\nPrimary failed. Trying backup WiFi...");
  WiFi.disconnect();
  delay(1000);

  WiFi.begin(WIFI2_SSID, WIFI2_PASS);
  startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to backup WiFi");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nBoth WiFi connections failed. Restarting...");
    delay(3000);
    ESP.restart();
  }
}

// Very small JSON extraction: finds `"key":<number>` and parses float.
bool extractJsonNumber(const String& json, const char* key, float &outVal) {
  String pattern = String("\"") + key + "\":";
  int i = json.indexOf(pattern);
  if (i < 0) return false;
  i += pattern.length();

  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) i++;

  int start = i;
  while (i < (int)json.length()) {
    char c = json[i];
    if ((c >= '0' && c <= '9') || c == '.' || c == '-') { i++; continue; }
    break;
  }
  if (i <= start) return false;

  outVal = json.substring(start, i).toFloat();
  return true;
}

bool updateRainFromApi() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure);
  client->setInsecure(); // simplest: skip cert validation

  HTTPClient https;
  if (!https.begin(*client, RAIN_URL)) return false;

  int code = https.GET();
  if (code <= 0) { https.end(); return false; }

  String body = https.getString();
  https.end();

  float r = 0.0f, p = 0.0f;
  bool okR = extractJsonNumber(body, "rain", r);
  bool okP = extractJsonNumber(body, "precipitation", p);

  if (!okR && !okP) return false;

  if (okR) currentRainMM = r;
  if (okP) currentPrecMM = p;

  return true;
}

bool isRainingNow() {
  return (currentRainMM > 0.0f) || (currentPrecMM > 0.0f);
}

bool getLocalHMS(int &hh, int &mm, int &ss) {
  time_t nowUtc = time(nullptr);
  if (nowUtc < 1700000000) return false; // time not set (rough sanity)
  time_t local = nowUtc + TZ_OFFSET_SEC;
  struct tm *t = gmtime(&local);
  if (!t) return false;
  hh = t->tm_hour;
  mm = t->tm_min;
  ss = t->tm_sec;
  return true;
}

bool inWindow(int hh, int mm, int ss, int startH, int startM, int endH, int endM) {
  int cur = hh * 3600 + mm * 60 + ss;
  int a   = startH * 3600 + startM * 60;
  int b   = endH   * 3600 + endM   * 60;
  return (cur >= a && cur < b);
}

// ---------------- MQTT callback (mobile override commands) ----------------
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  msg.trim();
  msg.toUpperCase();

  Serial.print("MQTT cmd on ");
  Serial.print(topic);
  Serial.print(" => ");
  Serial.println(msg);

  // Commands:
  // AUTO
  // ON
  // OFF
  // ON:120  (seconds)
  if (msg == "AUTO") {
    manualMode = false;
    manualWaterOn = false;
    manualUntilMs = 0;
    return;
  }

  if (msg == "ON") {
    manualMode = true;
    manualWaterOn = true;
    manualUntilMs = 0;
    return;
  }

  if (msg == "OFF") {
    manualMode = true;
    manualWaterOn = false;
    manualUntilMs = 0;
    return;
  }

  if (msg.startsWith("ON:")) {
    int seconds = msg.substring(3).toInt();
    if (seconds > 0) {
      manualMode = true;
      manualWaterOn = true;
      manualUntilMs = millis() + (unsigned long)seconds * 1000UL;
    }
    return;
  }
}

void connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);

  while (!mqtt.connected()) {
    String clientId = "watering-esp8266-" + String(ESP.getChipId(), HEX);
    mqtt.connect(clientId.c_str());
    if (!mqtt.connected()) delay(1000);
  }

  mqtt.subscribe(TOPIC_CMD); // <-- subscribe for phone commands
  Serial.print("Subscribed to: ");
  Serial.println(TOPIC_CMD);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_W1, OUTPUT);
  pinMode(PIN_W2, OUTPUT);
  pinMode(PIN_W3, OUTPUT);
  setWateringOutputs(false);

  connectWiFi();
  connectMQTT();

  // NTP
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.windows.com");
  lastNtpSync = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  unsigned long nowMs = millis();

  // Periodic NTP re-sync
  if (nowMs - lastNtpSync > (unsigned long)NTP_RESYNC_SEC * 1000UL) {
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.windows.com");
    lastNtpSync = nowMs;
  }

  // Periodic rain check
  if (nowMs - lastRainCheck >= RAIN_CHECK_MS) {
    lastRainCheck = nowMs;

    bool ok = updateRainFromApi();
    rainDataValid = ok;

    if (ok) {
      Serial.print("Rain API OK. current.rain=");
      Serial.print(currentRainMM, 3);
      Serial.print(" current.precipitation=");
      Serial.println(currentPrecMM, 3);
    } else {
      Serial.println("Rain API FAILED / missing fields -> will water without considering rain.");
      currentRainMM = 0.0f;
      currentPrecMM = 0.0f;
    }
  }

  // Publish every second
  if (nowMs - lastPublish >= PUBLISH_MS) {
    lastPublish = nowMs;

    int hh=0, mm=0, ss=0;
    bool timeOk = getLocalHMS(hh, mm, ss);

    // Two watering windows:
    bool inMorning = timeOk && inWindow(hh, mm, ss, 7, 59, 8, 0);
    bool inEvening = timeOk && inWindow(hh, mm, ss, 16, 00, 16, 1);
    bool scheduleWantsWater = inMorning || inEvening;

    bool allowByRain = (!rainDataValid) ? true : (!isRainingNow());

    // Manual timer auto-off
    if (manualMode && manualWaterOn && manualUntilMs > 0 && (long)(millis() - manualUntilMs) >= 0) {
      manualWaterOn = false;
      manualUntilMs = 0;
    }

    // FINAL decision
    bool wateringOn;
    const char* modeStr;

    if (manualMode) {
      wateringOn = manualWaterOn; // ignores schedule/rain
      modeStr = "MANUAL";
    } else {
      wateringOn = scheduleWantsWater && allowByRain;
      modeStr = "AUTO";
    }

    setWateringOutputs(wateringOn);

    // Status publish
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{ \"watering\": %s, \"mode\": \"%s\" }",
             wateringOn ? "true" : "false",
             modeStr);

    mqtt.publish(TOPIC_WATERING, payload);
    Serial.println(payload);

    if (timeOk) {
      Serial.printf("Local %02d:%02d:%02d | mode=%s | schedule=%d | rainValid=%d raining=%d | watering=%d\n",
                    hh, mm, ss, modeStr, (int)scheduleWantsWater, (int)rainDataValid, (int)isRainingNow(), (int)wateringOn);
    } else {
      Serial.printf("Time not set yet | mode=%s | watering=%d\n", modeStr, (int)wateringOn);
    }
  }
}