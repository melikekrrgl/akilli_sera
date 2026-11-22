/*
 * ============================================================
 *  AKILLI SERA - ESP32 ana kart
 * ============================================================
 *  Sensörler : DHT11/DHT22 (sıcaklık-nem), kapasitif toprak nemi,
 *              LDR (ışık), su seviyesi sensörü (depo)
 *  Çıkışlar  : Fan ve su pompası (L9110 sürücü, PWM), buzzer, OLED
 *  Bulut     : MQTT (varsayılan: HiveMQ public broker)
 *
 *  Aynı kod hem Wokwi simülasyonunda hem fiziksel devrede çalışır:
 *   - secrets.h YOKSA  -> Wokwi ayarları (Wokwi-GUEST, DHT22, açık broker)
 *   - secrets.h VARSA  -> fiziksel devre ayarları (secrets.example.h'a bakın)
 *
 *  MQTT konuları (BASE = "akillisera/<DEVICE_ID>"):
 *   BASE/telemetry  -> 5 sn'de bir sensör + durum (JSON)
 *   BASE/status     -> "online" / "offline" (retained, LWT)
 *   BASE/config     -> güncel ayarlar (retained, JSON)
 *   BASE/event      -> alarm ve bilgi mesajları (JSON)
 *   BASE/cmd        <- komutlar (JSON), ör: {"pump":true} {"fan":false}
 *                      {"mode":"auto"} {"fanSpeed":70}
 *                      {"config":{"tMax":30,"soilMin":40}} {"buzzer":"test"}
 * ============================================================
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

#if __has_include("secrets.h")
#include "secrets.h"  // fiziksel devre ayarları (GitHub'a yüklenmez)
#endif

// ---------------- Varsayılan ayarlar (Wokwi) ----------------
#ifndef WIFI_SSID
#define WIFI_SSID "Wokwi-GUEST"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef MQTT_HOST
#define MQTT_HOST "broker.hivemq.com"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASS
#define MQTT_PASS ""
#endif
#ifndef MQTT_TLS
#define MQTT_TLS 0  // HiveMQ Cloud için 1 (port 8883)
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "sera-demo-4821"  // Panelde de aynısı yazılmalı
#endif
#ifndef DHT_TYPE
#define DHT_TYPE DHT22  // Wokwi'de DHT22, fiziksel devrede DHT11
#endif

// Kalibrasyon (ham ADC 0..4095). Wokwi'de sürgülü potansiyometre kullanılır.
#ifndef SOIL_RAW_DRY
#define SOIL_RAW_DRY 0      // tamamen kuru topraktaki ham değer
#endif
#ifndef SOIL_RAW_WET
#define SOIL_RAW_WET 4095   // suya batırılmış haldeki ham değer
#endif
#ifndef WATER_RAW_EMPTY
#define WATER_RAW_EMPTY 0
#endif
#ifndef WATER_RAW_FULL
#define WATER_RAW_FULL 4095
#endif
#ifndef LDR_SERIES_R
#define LDR_SERIES_R 2000.0  // LDR modülündeki sabit direnç (ohm)
#endif

// ---------------- Pinler ----------------
#define PIN_DHT     15
#define PIN_SOIL    34   // analog
#define PIN_LDR     35   // analog (modülün AO ucu)
#define PIN_WATER   32   // analog
#define PIN_FAN     25   // L9110 A-IA (A-IB -> GND)
#define PIN_PUMP    26   // L9110 B-IA (B-IB -> GND)
#define PIN_BUZZER  27
// OLED: SDA=21, SCL=22 (I2C, adres 0x3C)

// ---------------- Zamanlamalar ----------------
const unsigned long TELEMETRY_MS = 5000;
const unsigned long SENSOR_MS    = 2000;
const unsigned long DISPLAY_MS   = 1000;
const unsigned long RECONNECT_MS = 5000;
const int WATER_LOW_PCT          = 15;   // depo bu yüzdenin altındaysa pompa çalışmaz

// ---------------- Nesneler ----------------
DHT dht(PIN_DHT, DHT_TYPE);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
Preferences prefs;
#if MQTT_TLS
WiFiClientSecure net;
#else
WiFiClient net;
#endif
PubSubClient mqtt(net);

String BASE = String("akillisera/") + DEVICE_ID;

// ---------------- Ayarlar (panelden değiştirilebilir, kalıcı) ----------------
struct Config {
  float tMax = 30.0;     // bu sıcaklığın üstünde fan açılır (°C)
  float hMax = 80.0;     // bu nemin üstünde fan açılır (%)
  int soilMin = 40;      // toprak nemi bunun altına düşünce sulanır (%)
  int pumpSec = 3;       // otomatik sulamada pompa çalışma süresi (sn)
  int soakSec = 30;      // sulamadan sonra suyun toprağa işlemesi için bekleme (sn)
  int pumpMaxSec = 15;   // elle açılan pompanın güvenlik için kapanma süresi (sn)
  int fanSpeed = 100;    // fan hızı (%)
} cfg;

// ---------------- Durum ----------------
bool autoMode = true;
bool pumpOn = false, fanOn = false;
unsigned long pumpStartMs = 0, pumpRunMs = 0, lastWaterMs = 0;
float tempC = NAN, hum = NAN;
int soilPct = 0, waterPct = 0;
float lux = 0;
bool waterLow = false, sensorError = false;
bool stateChanged = false;
unsigned long lastSensor = 0, lastTelemetry = 0, lastDisplay = 0, lastMqttTry = 0, lastWifiTry = 0, lastBeep = 0;

// ============================================================
//  Yardımcılar
// ============================================================
int toPercent(int raw, int raw0, int raw100) {
  long p = map(raw, raw0, raw100, 0, 100);
  return constrain(p, 0, 100);
}

float readLux(int raw) {
  // Wokwi foto direnç modülü formülü (3.3V için uyarlanmış). Fiziksel modülde yaklaşık değerdir.
  float v = raw / 4095.0 * 3.3;
  if (v >= 3.29) return 0;      // tam karanlık
  if (v <= 0.01) return 100000; // çok parlak
  float r = LDR_SERIES_R * v / (1.0 - v / 3.3);
  const float GAMMA = 0.7, RL10 = 50;
  return pow(RL10 * 1e3 * pow(10, GAMMA) / r, 1.0 / GAMMA);
}

void beep(int ms = 120, int freq = 2000) { tone(PIN_BUZZER, freq, ms); }

void publishEvent(const char* level, const String& msg) {
  if (!mqtt.connected()) return;
  JsonDocument d;
  d["level"] = level;
  d["msg"] = msg;
  String out;
  serializeJson(d, out);
  mqtt.publish((BASE + "/event").c_str(), out.c_str());
  Serial.printf("[OLAY] %s: %s\n", level, msg.c_str());
}

// ============================================================
//  Çıkışlar
// ============================================================
void setPump(bool on, const char* reason) {
  if (on && waterLow) {
    publishEvent("warn", "Depoda su az, pompa çalıştırılmadı");
    on = false;
  }
  if (on == pumpOn) return;
  pumpOn = on;
  analogWrite(PIN_PUMP, on ? 255 : 0);
  if (on) pumpStartMs = millis();
  else lastWaterMs = millis();
  stateChanged = true;
  Serial.printf("Pompa %s (%s)\n", on ? "AÇIK" : "KAPALI", reason);
}

void setFan(bool on) {
  fanOn = on;
  analogWrite(PIN_FAN, on ? map(cfg.fanSpeed, 0, 100, 0, 255) : 0);
  stateChanged = true;
}

// ============================================================
//  Ayarların kaydı
// ============================================================
void loadConfig() {
  prefs.begin("sera", true);
  cfg.tMax = prefs.getFloat("tMax", cfg.tMax);
  cfg.hMax = prefs.getFloat("hMax", cfg.hMax);
  cfg.soilMin = prefs.getInt("soilMin", cfg.soilMin);
  cfg.pumpSec = prefs.getInt("pumpSec", cfg.pumpSec);
  cfg.soakSec = prefs.getInt("soakSec", cfg.soakSec);
  cfg.pumpMaxSec = prefs.getInt("pumpMaxSec", cfg.pumpMaxSec);
  cfg.fanSpeed = prefs.getInt("fanSpeed", cfg.fanSpeed);
  autoMode = prefs.getBool("auto", true);
  prefs.end();
}

void saveConfig() {
  prefs.begin("sera", false);
  prefs.putFloat("tMax", cfg.tMax);
  prefs.putFloat("hMax", cfg.hMax);
  prefs.putInt("soilMin", cfg.soilMin);
  prefs.putInt("pumpSec", cfg.pumpSec);
  prefs.putInt("soakSec", cfg.soakSec);
  prefs.putInt("pumpMaxSec", cfg.pumpMaxSec);
  prefs.putInt("fanSpeed", cfg.fanSpeed);
  prefs.putBool("auto", autoMode);
  prefs.end();
}

void publishConfig() {
  if (!mqtt.connected()) return;
  JsonDocument d;
  d["tMax"] = cfg.tMax;
  d["hMax"] = cfg.hMax;
  d["soilMin"] = cfg.soilMin;
  d["pumpSec"] = cfg.pumpSec;
  d["soakSec"] = cfg.soakSec;
  d["pumpMaxSec"] = cfg.pumpMaxSec;
  d["fanSpeed"] = cfg.fanSpeed;
  d["mode"] = autoMode ? "auto" : "manual";
  String out;
  serializeJson(d, out);
  mqtt.publish((BASE + "/config").c_str(), out.c_str(), true);
}

void publishTelemetry() {
  if (!mqtt.connected()) return;
  JsonDocument d;
  if (!isnan(tempC)) d["t"] = round(tempC * 10) / 10.0;
  if (!isnan(hum)) d["h"] = round(hum);
  d["soil"] = soilPct;
  d["lux"] = (int)round(lux);
  d["water"] = waterPct;
  d["waterLow"] = waterLow;
  d["pump"] = pumpOn;
  d["fan"] = fanOn;
  d["fanSpeed"] = cfg.fanSpeed;
  d["mode"] = autoMode ? "auto" : "manual";
  d["sensorError"] = sensorError;
  d["rssi"] = WiFi.RSSI();
  d["up"] = millis() / 1000;
  String out;
  serializeJson(d, out);
  mqtt.publish((BASE + "/telemetry").c_str(), out.c_str());
  lastTelemetry = millis();
  stateChanged = false;
}

// ============================================================
//  MQTT komutları
// ============================================================
void onMessage(char* topic, byte* payload, unsigned int len) {
  JsonDocument d;
  if (deserializeJson(d, payload, len)) {
    publishEvent("warn", "Anlaşılmayan komut");
    return;
  }
  Serial.print("Komut: ");
  serializeJson(d, Serial);
  Serial.println();

  if (d["mode"].is<const char*>()) {
    autoMode = String((const char*)d["mode"]) == "auto";
    saveConfig();
    publishConfig();
    stateChanged = true;
  }
  // Elle pompa / fan komutu gelirse sistem elle kontrol moduna geçer
  if (d["pump"].is<bool>()) {
    autoMode = false;
    setPump(d["pump"], "uzaktan komut");
    saveConfig();
    publishConfig();
  }
  if (d["fanSpeed"].is<int>()) {
    cfg.fanSpeed = constrain((int)d["fanSpeed"], 0, 100);
    saveConfig();
    publishConfig();
    if (fanOn) setFan(true);
  }
  if (d["fan"].is<bool>()) {
    autoMode = false;
    setFan(d["fan"]);
    saveConfig();
    publishConfig();
  }
  if (d["config"].is<JsonObject>()) {
    JsonObject c = d["config"];
    if (c["tMax"].is<float>()) cfg.tMax = constrain((float)c["tMax"], 0, 60);
    if (c["hMax"].is<float>()) cfg.hMax = constrain((float)c["hMax"], 0, 100);
    if (c["soilMin"].is<int>()) cfg.soilMin = constrain((int)c["soilMin"], 0, 100);
    if (c["pumpSec"].is<int>()) cfg.pumpSec = constrain((int)c["pumpSec"], 1, 30);
    if (c["soakSec"].is<int>()) cfg.soakSec = constrain((int)c["soakSec"], 5, 600);
    if (c["pumpMaxSec"].is<int>()) cfg.pumpMaxSec = constrain((int)c["pumpMaxSec"], 1, 60);
    saveConfig();
    publishConfig();
    publishEvent("info", "Ayarlar güncellendi");
  }
  if (d["buzzer"].is<const char*>()) beep(300);
  publishTelemetry();
}

// ============================================================
//  Bağlantılar (bloklamayan yeniden bağlanma)
// ============================================================
void connectWifi() {
  Serial.printf("WiFi'ye bağlanılıyor: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  if (String(WIFI_SSID) == "Wokwi-GUEST") WiFi.begin(WIFI_SSID, WIFI_PASS, 6);  // Wokwi'de daha hızlı
  else WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiTry = millis();
}

void connectMqtt() {
  lastMqttTry = millis();
  String clientId = "sera-esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  String willTopic = BASE + "/status";
  Serial.printf("MQTT'ye bağlanılıyor: %s:%d ... ", MQTT_HOST, MQTT_PORT);
  bool ok = strlen(MQTT_USER) > 0
              ? mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, willTopic.c_str(), 1, true, "offline")
              : mqtt.connect(clientId.c_str(), willTopic.c_str(), 1, true, "offline");
  if (!ok) {
    Serial.printf("başarısız (kod %d)\n", mqtt.state());
    return;
  }
  Serial.println("bağlandı");
  Serial.printf("Konu: %s/#\n", BASE.c_str());
  mqtt.publish(willTopic.c_str(), "online", true);
  mqtt.subscribe((BASE + "/cmd").c_str(), 1);
  publishConfig();
  publishTelemetry();
  publishEvent("info", "Sera çevrimiçi");
}

// ============================================================
//  Sensörler ve otomatik kontrol
// ============================================================
void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  sensorError = isnan(t) || isnan(h);
  if (!sensorError) { tempC = t; hum = h; }

  soilPct = toPercent(analogRead(PIN_SOIL), SOIL_RAW_DRY, SOIL_RAW_WET);
  waterPct = toPercent(analogRead(PIN_WATER), WATER_RAW_EMPTY, WATER_RAW_FULL);
  lux = readLux(analogRead(PIN_LDR));

  bool wasLow = waterLow;
  waterLow = waterPct < WATER_LOW_PCT;
  if (waterLow && !wasLow) publishEvent("alarm", "Su deposu boşalmak üzere!");
  if (!waterLow && wasLow) publishEvent("info", "Su deposu dolduruldu");
}

void autoControl() {
  unsigned long now = millis();

  // Güvenlik: pompa hiçbir modda süresiz çalışmaz, su biterse durur
  if (pumpOn) {
    unsigned long limit = (autoMode ? cfg.pumpSec : cfg.pumpMaxSec) * 1000UL;
    if (now - pumpStartMs >= limit) setPump(false, "süre doldu");
    if (waterLow) setPump(false, "su bitti");
  }

  if (!autoMode) return;

  // Sulama: kısa darbeler + suyun işlemesi için bekleme (taşmayı önler)
  bool soaked = (now - lastWaterMs) >= cfg.soakSec * 1000UL || lastWaterMs == 0;
  if (!pumpOn && soilPct < cfg.soilMin && !waterLow && soaked) {
    setPump(true, "toprak kuru");
    publishEvent("info", "Toprak kuru, sulama başladı");
  }

  // Havalandırma: histerezisli (sürekli aç-kapa yapmaz)
  if (!isnan(tempC) && !isnan(hum)) {
    bool hot = tempC > cfg.tMax || hum > cfg.hMax;
    bool cool = tempC < cfg.tMax - 1.0 && hum < cfg.hMax - 3.0;
    if (hot && !fanOn) { setFan(true); publishEvent("info", "Sıcaklık/nem yüksek, fan açıldı"); }
    else if (cool && fanOn) setFan(false);
  }
}

void alarms() {
  unsigned long now = millis();
  bool tooHot = !isnan(tempC) && tempC > cfg.tMax + 5;
  if ((waterLow || tooHot) && now - lastBeep > 5000) {
    beep(150, waterLow ? 1500 : 2500);
    lastBeep = now;
  }
}

// ============================================================
//  OLED ekran
// ============================================================
void drawDisplay() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("AKILLI SERA ");
  oled.print(WiFi.status() == WL_CONNECTED ? (mqtt.connected() ? "BULUT" : "WiFi") : "----");

  oled.setTextSize(2);
  oled.setCursor(0, 12);
  if (isnan(tempC)) oled.print("--.-C");
  else { oled.print(tempC, 1); oled.print("C"); }
  oled.setCursor(76, 12);
  if (isnan(hum)) oled.print("--%");
  else { oled.print((int)hum); oled.print("%"); }

  oled.setTextSize(1);
  oled.setCursor(0, 32);
  oled.printf("Toprak:%3d%%  Su:%3d%%", soilPct, waterPct);
  oled.setCursor(0, 42);
  oled.printf("Isik: %d lux", (int)lux);
  oled.setCursor(0, 54);
  oled.printf("%s P:%s F:%s", autoMode ? "OTO " : "ELLE", pumpOn ? "ON " : "OFF", fanOn ? "ON " : "OFF");
  if (waterLow) { oled.setCursor(104, 42); oled.print("SU!"); }
  oled.display();
}

// ============================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== AKILLI SERA ===");
  pinMode(PIN_FAN, OUTPUT);
  pinMode(PIN_PUMP, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  analogWrite(PIN_FAN, 0);
  analogWrite(PIN_PUMP, 0);

  loadConfig();
  dht.begin();
  Wire.begin(21, 22);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED bulunamadı");
  oled.clearDisplay();
  oled.display();

#if MQTT_TLS
  net.setInsecure();  // okul projesi için: sertifika doğrulaması kapalı
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);

  connectWifi();
  beep(80);
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTry > 15000) { WiFi.disconnect(); connectWifi(); }
  } else if (!mqtt.connected()) {
    if (now - lastMqttTry > RECONNECT_MS || lastMqttTry == 0) {
      Serial.printf("WiFi bağlı, IP: %s\n", WiFi.localIP().toString().c_str());
      connectMqtt();
    }
  } else {
    mqtt.loop();
  }

  if (now - lastSensor >= SENSOR_MS) {
    lastSensor = now;
    readSensors();
    autoControl();
    alarms();
  }
  if (pumpOn) autoControl();  // pompa süresini hassas takip et

  if (stateChanged || now - lastTelemetry >= TELEMETRY_MS) publishTelemetry();
  if (now - lastDisplay >= DISPLAY_MS) { lastDisplay = now; drawDisplay(); }
}
