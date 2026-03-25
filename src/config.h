#pragma once

// ─── Firmware ─────────────────────────────────────────────────────────────────
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "1.0.0"
#endif

// ─── Red ──────────────────────────────────────────────────────────────────────
#define WIFI_SSID           "Ire y Mau"
#define WIFI_PASSWORD       "Lady-350!"
#define WIFI_TIMEOUT_MS     15000
#define WIFI_MAX_RETRIES    3

// ─── MQTT ─────────────────────────────────────────────────────────────────────
#define MQTT_BROKER         "192.168.18.250"   // IP de la Raspberry Pi
#define MQTT_PORT           1883
#define MQTT_USER           "weather_station_iot"
#define MQTT_PASSWORD       "aXdC7nE2gLEe"
#define MQTT_CLIENT_ID      "weather-station-01"

// Topics
#define TOPIC_TELEMETRY     "station/01/telemetry"
#define TOPIC_STATUS        "station/01/status"
#define TOPIC_CMD           "station/01/cmd"         // retained, escrito por N8N

// Tiempo de espera para recibir el mensaje retenido del broker
#define MQTT_RETAINED_WAIT_MS  800

// ─── I2C ──────────────────────────────────────────────────────────────────────
#define I2C_SDA             6
#define I2C_SCL             5

// ─── Sensores ─────────────────────────────────────────────────────────────────
#define ALTITUDE_M          780.0f    // Altitud del lugar en metros SNM
#define INA219_SOLAR_ADDR   0x41      // INA219 panel solar
#define INA219_SYSTEM_ADDR  0x40      // INA219 consumo ESP32

// ─── Deep sleep ───────────────────────────────────────────────────────────────
#define SLEEP_INTERVAL_SEC  60   // 300s = 5 minutos (modo normal)

// ─── OTA / Service mode ───────────────────────────────────────────────────────
#define OTA_HOSTNAME        "weather-station-01"
#define OTA_PASSWORD        "rnLm43G7wcYr"            // mismo que upload_flags en platformio.ini

#define SERVICE_MODE_DEFAULT_TIMEOUT_MIN  15
#define SERVICE_MODE_MAX_TIMEOUT_MIN      60          // techo absoluto ignorando lo que pida el servidor
#define SERVICE_MODE_HEARTBEAT_SEC        30

// ─── Logging ──────────────────────────────────────────────────────────────────
// LOG_LEVEL: 0=off, 1=error, 2=verbose
#ifndef LOG_LEVEL
  #define LOG_LEVEL 0
#endif

#if LOG_LEVEL >= 2
  #define LOG_V(fmt, ...) Serial.printf("[V] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_V(fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL >= 1
  #define LOG_E(fmt, ...) Serial.printf("[E] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_E(fmt, ...) do {} while(0)
#endif

// ─── Red (IP estática) ────────────────────────────────────────────────────────
#define WIFI_STATIC_IP      IPAddress(192, 168, 18, 105)
#define WIFI_GATEWAY        IPAddress(192, 168, 18, 1) 
#define WIFI_SUBNET         IPAddress(255, 255, 255, 0)
#define WIFI_DNS            IPAddress(8, 8, 8, 8)