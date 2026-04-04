#pragma once

// =============================================================================
//  Firmware de diagnóstico PCB — Configuración
//  Editar este archivo antes de flashear.
// =============================================================================

// ─── WiFi ─────────────────────────────────────────────────────────────────────
#define DIAG_WIFI_SSID        "Ire y Mau"
#define DIAG_WIFI_PASSWORD    "Lady-350!"
#define DIAG_WIFI_TIMEOUT_MS  15000

// IP estática
#define DIAG_STATIC_IP        IPAddress(192, 168, 18, 105)
#define DIAG_GATEWAY          IPAddress(192, 168, 18, 1)
#define DIAG_SUBNET           IPAddress(255, 255, 255, 0)
#define DIAG_DNS              IPAddress(8, 8, 8, 8)

#define DIAG_HTTP_PORT        80

// ─── Pines ────────────────────────────────────────────────────────────────────
#define PIN_DS18B20           10    // OneWire
#define PIN_DHT11              0    // DHT data (pull-up en el módulo → seguro en boot)
#define PIN_PHOTORESISTOR      3    // ADC: 3.3V → R10K → señal → fotorresistencia → GND
#define PIN_RAIN_SENSOR        4    // ADC: 3.3V → R1∥R2(4.95kΩ) → señal → C1 → GND
#define PIN_ANEMOMETER         2    // Interrupción FALLING (pulso activo bajo)
#define PIN_RAIN_GAUGE         1    // Interrupción FALLING (pulso activo bajo)

// ─── I2C ──────────────────────────────────────────────────────────────────────
#define DIAG_I2C_SDA           6
#define DIAG_I2C_SCL           5
#define DIAG_INA219_SYSTEM     0x40
#define DIAG_INA219_SOLAR      0x41

// ─── Constantes ADC ───────────────────────────────────────────────────────────
// Fotorresistencia: divisor VCC–R10K–señal–fotoresistencia–GND
//   R_foto = R_pullup * V / (3.3 - V)
#define DIAG_PHOTO_PULLUP_KOHM  10.0f

// Rain sensor: pullup efectivo R1∥R2 = 9.9k∥9.9k = 4.95kΩ
//   wetness% = (1 - V/3.3) * 100
#define DIAG_RAIN_PULLUP_KOHM   4.95f

#define DIAG_ADC_VREF           3.3f
#define DIAG_ADC_MAX_RAW        4095.0f

// ─── Temporización ────────────────────────────────────────────────────────────
#define DIAG_DEBOUNCE_MS        50    // debounce interrupción (pulsador manual)
#define DIAG_DHT_WARMUP_MS    2000    // espera DHT11 tras begin()
#define DIAG_SENSOR_PERIOD_MS 2000    // período de lectura en loop
#define DIAG_REFRESH_SEC         5    // meta-refresh página HTML
