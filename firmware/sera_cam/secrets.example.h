// Bu dosyayı "secrets.h" adıyla kopyalayıp doldurun (GitHub'a yüklenmez).
// Değerler ana kartın (sera_esp32) secrets.h dosyasıyla AYNI olmalı.
#pragma once

#define WIFI_SSID "WIFI_ADINIZ"
#define WIFI_PASS "WIFI_SIFRENIZ"
#define DEVICE_ID "sera-demo-4821"

#define MQTT_HOST "broker.hivemq.com"
#define MQTT_PORT 1883
#define MQTT_TLS 0
#define MQTT_USER ""
#define MQTT_PASS ""

// #define MQTT_HOST "xxxxxxxx.s1.eu.hivemq.cloud"
// #define MQTT_PORT 8883
// #define MQTT_TLS 1
// #define MQTT_USER "sera"
// #define MQTT_PASS "guclu-bir-sifre"

#define CAM_INTERVAL_SEC 60
