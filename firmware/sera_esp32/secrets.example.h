// =============================================================
//  FİZİKSEL DEVRE AYARLARI
//  Bu dosyanın bir kopyasını "secrets.h" adıyla aynı klasöre kaydedin
//  ve bilgilerinizi yazın. secrets.h GitHub'a YÜKLENMEZ (.gitignore).
//  secrets.h yoksa kod Wokwi ayarlarıyla çalışır.
// =============================================================
#pragma once

// --- WiFi (ESP32 sadece 2.4 GHz ağlara bağlanır) ---
#define WIFI_SSID "WIFI_ADINIZ"
#define WIFI_PASS "WIFI_SIFRENIZ"

// --- Cihaz kimliği: paneldeki "Cihaz kimliği" ile AYNI olmalı ---
// Kimsenin tahmin edemeyeceği bir şey seçin, ör: "sera-kaan-7x92"
#define DEVICE_ID "sera-demo-4821"

// --- MQTT broker ---
// Seçenek 1 (kolay, demo): HiveMQ açık broker - şifresiz
#define MQTT_HOST "broker.hivemq.com"
#define MQTT_PORT 1883
#define MQTT_TLS 0
#define MQTT_USER ""
#define MQTT_PASS ""

// Seçenek 2 (güvenli, önerilen): HiveMQ Cloud ücretsiz hesap
// #define MQTT_HOST "xxxxxxxx.s1.eu.hivemq.cloud"
// #define MQTT_PORT 8883
// #define MQTT_TLS 1
// #define MQTT_USER "sera"
// #define MQTT_PASS "guclu-bir-sifre"

// --- Sensör tipi: okuldaki sensör DHT11 ---
#define DHT_TYPE DHT11

// --- Kalibrasyon (Seri Monitör'deki ham değerlere göre düzenleyin) ---
// Kapasitif toprak sensörü: kuruyken ham değer YÜKSEK, ıslakken DÜŞÜK
#define SOIL_RAW_DRY 3000
#define SOIL_RAW_WET 1300
// Su seviyesi sensörü: kuruyken ~0, tamamen batınca ~1800 civarı
#define WATER_RAW_EMPTY 0
#define WATER_RAW_FULL 1800
// LDR modülündeki seri direnç (çoğu modülde 10k)
#define LDR_SERIES_R 10000.0
