#pragma once

// ─── Firmware ─────────────────────────────────────────────────────────────────
// La versión real la define -DFIRMWARE_VERSION en platformio.ini, por entorno.
// Este fallback es un centinela, no una versión: si aparece en la telemetría,
// significa que el build salió de un entorno que se olvidó de definir el flag.
// Antes decía "1.0.0", que era una versión que existió de verdad — un build mal
// configurado quedaba indistinguible de un deploy legítimo de 1.0.0.
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "0.0.0-nobuildflag"
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

// ─── Rail control (transistores NPN BC337 en PCB principal) ───────────────────
// GPIO HIGH → NPN conduce → GND del rail conectado → sensores alimentados
#define PIN_RAIL_A          7   // Rail A: sensores exteriores (SHT31 + BMP085)
#define PIN_RAIL_B          8   // Rail B: sensores enclosure (DHT22, foto, lluvia)
// TODO [bajo consumo]: apagar Rail B en Tier 2 y Rail A en Tier 3
//   según umbral de batería — implementar en battery.h

// ─── Pines nuevos sensores ────────────────────────────────────────────────────
#define PIN_DS18B20         10  // OneWire temperatura exterior (always-on)
#define PIN_DHT22            0  // DHT22 DATA (Rail B) — mismo pin que el ex DHT11
#define PIN_PHOTORESISTOR    3  // ADC fotorresistencia (Rail B)
#define PIN_RAIN_SENSOR      4  // ADC sensor de lluvia AO (Rail B)
#define PIN_ANEMOMETER       2  // Pulso FALLING — anemómetro (always-on)
#define PIN_RAIN_GAUGE       1  // Pulso FALLING — pluviómetro (always-on)
// TODO [pulsos]: implementar conteo acumulado (RTC memory) para
//   anemómetro y pluviómetro entre ciclos de deep sleep

// ─── Calibración sensores analógicos ─────────────────────────────────────────
// Fotorresistencia: divisor 3V3 → R10kΩ → señal → foto → GND
//   R_foto = R_pullup * V / (3.3 - V)
#define PHOTO_PULLUP_KOHM   10.0f

// Rain sensor: divisor 3V3 → R1‖R2(4.95kΩ) → señal → sensor → GND
//   R_rain = R_pullup * V / (3.3 - V)
#define RAIN_PULLUP_KOHM    4.95f

// ADC
#define ADC_VREF            3.3f
#define ADC_MAX_RAW      4095.0f

// ─── DHT22 (Rail B) ───────────────────────────────────────────────────────────
// Sin constantes de calibración de humedad: las que había (DHT_HUM_RAW/REAL_*)
// corregían el sesgo del DHT11 defectuoso. El DHT22 viene calibrado de fábrica
// (±2% RH, ±0.5 °C) — cualquier corrección acá volvería a sesgar la lectura.
//
// Warmup tras energizar Rail B. El datasheet del AM2302 pide ≥1s de "unstable
// status"; 2s es margen conservador. Se mide desde el rail-on, no desde el boot
// — ver sensors_init().
#define DHT_WARMUP_MS           2000

// Período mínimo de muestreo del DHT22 (datasheet: ≥2s entre lecturas).
// Espaciado del reintento cuando la primera trama sale corrupta.
#define DHT_RETRY_INTERVAL_MS   2000