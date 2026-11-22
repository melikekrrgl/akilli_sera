/*
 * ============================================================
 *  AKILLI SERA - ESP32-CAM (AI-Thinker) kamera modülü
 * ============================================================
 *  - Belirli aralıklarla ve panelden istenince fotoğraf çeker
 *  - Fotoğrafı JPEG olarak MQTT'ye gönderir (retained: panel açılınca son fotoğraf görünür)
 *
 *  MQTT konuları (BASE = "akillisera/<DEVICE_ID>", ana kartla AYNI kimlik):
 *   BASE/cam/image   -> JPEG fotoğraf (ikili veri, retained)
 *   BASE/cam/status  -> "online" / "offline"
 *   BASE/cam/cmd     <- "capture" (fotoğraf çek) | "flash" (flaşlı çek)
 *
 *  Kart ayarı (Arduino IDE): "AI Thinker ESP32-CAM", PSRAM: Enabled
 *  Yükleme: USB-TTL ile (GPIO0 -> GND iken reset), ayrıntı README'de.
 *  Not: Wokwi kamera desteklemez, bu kod sadece fiziksel kartta çalışır.
 * ============================================================
 */
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "WIFI_ADINIZ"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "WIFI_SIFRENIZ"
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
#define MQTT_TLS 0
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "sera-demo-4821"
#endif
#ifndef CAM_INTERVAL_SEC
#define CAM_INTERVAL_SEC 60       // otomatik fotoğraf aralığı (sn)
#endif
#ifndef CAM_FRAME_SIZE
#define CAM_FRAME_SIZE FRAMESIZE_VGA  // 640x480. Sorun olursa FRAMESIZE_QVGA (320x240)
#endif

// AI-Thinker ESP32-CAM pinleri
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22
#define FLASH_LED 4

#if MQTT_TLS
WiFiClientSecure net;
#else
WiFiClient net;
#endif
PubSubClient mqtt(net);

String BASE = String("akillisera/") + DEVICE_ID;
volatile bool captureRequested = false;
bool useFlash = false;
unsigned long lastShot = 0, lastMqttTry = 0;

bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = psramFound() ? CAM_FRAME_SIZE : FRAMESIZE_QVGA;
  c.jpeg_quality = 12;  // 0-63, küçük = daha kaliteli/büyük dosya
  c.fb_count = psramFound() ? 2 : 1;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("Kamera başlatılamadı: 0x%x\n", err);
    return false;
  }
  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, 0);    // görüntü ters çıkarsa 1 yapın
  s->set_hmirror(s, 0);
  return true;
}

void onMessage(char* topic, byte* payload, unsigned int len) {
  String cmd;
  for (unsigned int i = 0; i < len; i++) cmd += (char)payload[i];
  cmd.trim();
  Serial.printf("Komut: %s\n", cmd.c_str());
  if (cmd.indexOf("capture") >= 0 || cmd.indexOf("flash") >= 0) {
    useFlash = cmd.indexOf("flash") >= 0;
    captureRequested = true;
  }
}

void connectMqtt() {
  lastMqttTry = millis();
  String id = "sera-cam-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  String will = BASE + "/cam/status";
  bool ok = strlen(MQTT_USER) > 0
              ? mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS, will.c_str(), 1, true, "offline")
              : mqtt.connect(id.c_str(), will.c_str(), 1, true, "offline");
  if (!ok) {
    Serial.printf("MQTT başarısız (kod %d)\n", mqtt.state());
    return;
  }
  Serial.println("MQTT bağlandı");
  mqtt.publish(will.c_str(), "online", true);
  mqtt.subscribe((BASE + "/cam/cmd").c_str());
  captureRequested = true;  // bağlanınca ilk fotoğrafı gönder
}

void takeAndSend() {
  if (useFlash) { digitalWrite(FLASH_LED, HIGH); delay(150); }
  camera_fb_t* fb = esp_camera_fb_get();  // eski kareyi at
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();
  digitalWrite(FLASH_LED, LOW);
  useFlash = false;
  if (!fb) { Serial.println("Fotoğraf alınamadı"); return; }

  String topic = BASE + "/cam/image";
  bool ok = mqtt.beginPublish(topic.c_str(), fb->len, true);
  if (ok) {
    const size_t CHUNK = 2048;
    for (size_t i = 0; i < fb->len; i += CHUNK) {
      size_t n = min(CHUNK, fb->len - i);
      if (mqtt.write(fb->buf + i, n) != n) { ok = false; break; }
    }
    ok = mqtt.endPublish() && ok;
  }
  Serial.printf("Fotoğraf %u bayt -> %s\n", fb->len, ok ? "gönderildi" : "GÖNDERİLEMEDİ");
  esp_camera_fb_return(fb);
  lastShot = millis();
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // ESP32-CAM'de sık görülen voltaj düşmesi resetini engeller
  Serial.begin(115200);
  pinMode(FLASH_LED, OUTPUT);
  digitalWrite(FLASH_LED, LOW);

  if (!initCamera()) { delay(3000); ESP.restart(); }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

#if MQTT_TLS
  net.setInsecure();
#endif
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMessage);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(15);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(2000); return; }
  if (!mqtt.connected()) {
    if (millis() - lastMqttTry > 5000 || lastMqttTry == 0) connectMqtt();
    return;
  }
  mqtt.loop();
  if (captureRequested || millis() - lastShot > CAM_INTERVAL_SEC * 1000UL) {
    captureRequested = false;
    takeAndSend();
  }
}
