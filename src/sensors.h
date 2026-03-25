#pragma once

struct SensorData {
    float temperature_c;       // SHT31 — sensor principal
    float humidity_pct;        // SHT31
    float bmp_temperature_c;   // BMP085 — referencia interna
    float pressure_hpa;        // BMP085 — presión local
    float pressure_qnh;        // BMP085 — presión a nivel del mar (QNH)
    float solar_v;             // INA219 — voltaje panel solar
    float solar_mA;            // INA219 — corriente panel solar
    float solar_mW;            // INA219 — potencia panel solar
    float system_v;            // INA219 — voltaje sistema (ESP32)
    float system_mA;           // INA219 — corriente sistema
    float system_mW;           // INA219 — potencia sistema
    bool  sht31_ok;
    bool  bmp_ok;
    bool  solar_ok;
    bool  system_ok;
};

// Inicializa todos los sensores. Retorna false si alguno falla (no crítico).
bool sensors_init();

// Lee todos los sensores. Campos fallidos quedan como NAN.
SensorData sensors_read();
