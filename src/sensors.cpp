#include <math.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_INA219.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>

#include "sensors.h"
#include "config.h"

// ─── Objetos de driver ────────────────────────────────────────────────────────
static Adafruit_SHT31  sht31;
static Adafruit_BMP085 bmp;
static Adafruit_INA219 ina219_solar(INA219_SOLAR_ADDR);
static Adafruit_INA219 ina219_system(INA219_SYSTEM_ADDR);

static OneWire           _oneWire(PIN_DS18B20);
static DallasTemperature _ds18b20(&_oneWire);
static DHT               _dht(PIN_DHT22, DHT22);

// ─── Estado de inicialización ─────────────────────────────────────────────────
static bool _sht31_ok   = false;
static bool _bmp_ok     = false;
static bool _solar_ok   = false;
static bool _system_ok  = false;
static bool _ds18b20_ok = false;

// =============================================================================
//  Monitor de batería para service mode
// =============================================================================
// Ver sensors.h para el porqué: sensors_init() no corre en service mode, así que
// sin esto el heartbeat no puede reportar voltaje. Solo toca el INA219 de sistema
// (0x40), que está en el bus I2C siempre alimentado — no enciende ningún rail.

bool sensors_initSystemMonitor() {
    if (!_system_ok) {
        _system_ok = ina219_system.begin();
    }
    return _system_ok;
}

float sensors_readSystemVoltage() {
    if (!_system_ok) return NAN;
    return ina219_system.getBusVoltage_V();
}

// Ver sensors.h para el porqué del guard: powerSave() no chequea i2c_dev, y hay
// caminos que llegan a dormir sin haber inicializado nada.
void sensors_sleepMonitors() {
    if (_solar_ok)  ina219_solar.powerSave(true);
    if (_system_ok) ina219_system.powerSave(true);
}

// =============================================================================
//  Inicialización
// =============================================================================

bool sensors_init() {
    // ── Rails: siempre activos (primera iteración) ────────────────────────────
    // TODO [bajo consumo]: apagar Rail B en Tier 2 y Rail A en Tier 3
    //   según umbral de batería — ver battery.h
    pinMode(PIN_RAIL_A, OUTPUT); digitalWrite(PIN_RAIL_A, HIGH);
    pinMode(PIN_RAIL_B, OUTPUT); digitalWrite(PIN_RAIL_B, HIGH);
    const uint32_t rail_on_ms = millis();   // referencia del warmup del DHT22

    // ── DS18B20 ───────────────────────────────────────────────────────────────
    _ds18b20.begin();
    _ds18b20.setResolution(9);   // ~93 ms conversión
    _ds18b20_ok = (_ds18b20.getDeviceCount() > 0);

    // ── DHT22 ─────────────────────────────────────────────────────────────────
    _dht.begin();

    // ── Sensores de pulso (always-on) — pines configurados, datos diferidos ───
    // TODO [pulsos]: implementar conteo acumulado en RTC memory
    pinMode(PIN_ANEMOMETER, INPUT_PULLUP);
    pinMode(PIN_RAIN_GAUGE,  INPUT_PULLUP);

    // ── Sensores I2C (Wire ya inicializado en setup() antes de llegar aquí) ───
    _sht31_ok  = sht31.begin(0x44);
    _bmp_ok    = bmp.begin();
    _solar_ok  = ina219_solar.begin();
    _system_ok = ina219_system.begin();

    // ── DHT22 warmup ──────────────────────────────────────────────────────────
    // Se mide desde que Rail B entrega energía, NO desde el boot: sensors_init()
    // corre después de WiFi+MQTT (ver main.cpp), así que acá millis() ya vale
    // 2-5s y anclarlo al boot dejaba el warmup efectivo en cero — el sensor se
    // leía apenas energizado. Va al final del init para que el tiempo del bus
    // I2C y del DS18B20 cuente como parte del warmup en vez de sumarse.
    uint32_t elapsed = millis() - rail_on_ms;
    if (elapsed < DHT_WARMUP_MS) {
        delay(DHT_WARMUP_MS - elapsed);
    }

    LOG_V("Rails A:HIGH B:HIGH | DS18B20:%s | SHT31:%s BMP:%s INA_sol:%s INA_sys:%s",
        _ds18b20_ok ? "OK" : "ERR",
        _sht31_ok   ? "OK" : "ERR",
        _bmp_ok     ? "OK" : "ERR",
        _solar_ok   ? "OK" : "ERR",
        _system_ok  ? "OK" : "ERR");

    return _sht31_ok && _bmp_ok && _solar_ok && _system_ok;
}

// =============================================================================
//  Lectura
// =============================================================================

// Excita el sensor de lluvia con un pulso corto en lugar de tensión continua
// para minimizar corrosión electrolítica en los electrodos. GPIO4 actúa como
// salida durante la excitación y luego como ADC para la lectura.
// Rail B debe estar activo al llamar esta función.
static int readRainSensorPulsed() {
    const int DISCHARGE_MS  = 2;
    const int EXCITATION_MS = 10;   // τ = 4.95kΩ × 100nF = 0.495ms → ×20
    const int POST_MS       = 1;

    pinMode(PIN_RAIN_SENSOR, OUTPUT);
    digitalWrite(PIN_RAIN_SENSOR, LOW);
    delay(DISCHARGE_MS);

    digitalWrite(PIN_RAIN_SENSOR, HIGH);
    delay(EXCITATION_MS);

    pinMode(PIN_RAIN_SENSOR, INPUT);
    delayMicroseconds(100);
    int value = (analogRead(PIN_RAIN_SENSOR) + analogRead(PIN_RAIN_SENSOR)) / 2;

    pinMode(PIN_RAIN_SENSOR, OUTPUT);
    digitalWrite(PIN_RAIN_SENSOR, LOW);
    delay(POST_MS);

    pinMode(PIN_RAIN_SENSOR, INPUT);
    return value;
}

SensorData sensors_read() {
    SensorData d;

    // ── Flags de estado ───────────────────────────────────────────────────────
    d.sht31_ok   = _sht31_ok;
    d.bmp_ok     = _bmp_ok;
    d.solar_ok   = _solar_ok;
    d.system_ok  = _system_ok;
    d.ds18b20_ok = _ds18b20_ok;

    // ── SHT31 ─────────────────────────────────────────────────────────────────
    if (_sht31_ok) {
        d.temperature_c = sht31.readTemperature();
        d.humidity_pct  = sht31.readHumidity();
    } else {
        d.temperature_c = NAN;
        d.humidity_pct  = NAN;
    }

    // ── BMP085 ────────────────────────────────────────────────────────────────
    if (_bmp_ok) {
        d.bmp_temperature_c = bmp.readTemperature();
        d.pressure_hpa      = bmp.readPressure() / 100.0f;
        d.pressure_qnh      = bmp.readSealevelPressure(ALTITUDE_M) / 100.0f;
    } else {
        d.bmp_temperature_c = NAN;
        d.pressure_hpa      = NAN;
        d.pressure_qnh      = NAN;
    }

    // ── INA219 solar ──────────────────────────────────────────────────────────
    if (_solar_ok) {
        d.solar_v  = ina219_solar.getBusVoltage_V();
        d.solar_mA = ina219_solar.getCurrent_mA();
        d.solar_mW = ina219_solar.getPower_mW();
    } else {
        d.solar_v  = NAN;
        d.solar_mA = NAN;
        d.solar_mW = NAN;
    }

    // ── INA219 sistema ────────────────────────────────────────────────────────
    if (_system_ok) {
        d.system_v  = ina219_system.getBusVoltage_V();
        d.system_mA = ina219_system.getCurrent_mA();
        d.system_mW = ina219_system.getPower_mW();
    } else {
        d.system_v  = NAN;
        d.system_mA = NAN;
        d.system_mW = NAN;
    }

    // ── DS18B20 ───────────────────────────────────────────────────────────────
    if (_ds18b20_ok) {
        _ds18b20.requestTemperatures();
        float t = _ds18b20.getTempCByIndex(0);
        if (t != DEVICE_DISCONNECTED_C) {
            d.ds18b20_c  = t;
        } else {
            d.ds18b20_c  = NAN;
            d.ds18b20_ok = false;
        }
    } else {
        d.ds18b20_c = NAN;
    }

    // ── DHT22 (Rail B) ────────────────────────────────────────────────────────
    // Los campos siguen llamándose dht11_* a propósito: son las claves del JSON
    // de telemetría y renombrarlas partiría la serie histórica del InfluxDB del
    // NAS. El sensor físico es un DHT22 desde 2026-07-25.
    {
        uint32_t t0 = millis();
        float t = _dht.readTemperature();
        float h = _dht.readHumidity();   // reusa la trama cacheada, no relee el bus

        if (isnan(t) || isnan(h)) {
            // Respetar el período mínimo de muestreo antes de forzar otra trama.
            uint32_t elapsed = millis() - t0;
            if (elapsed < DHT_RETRY_INTERVAL_MS) {
                delay(DHT_RETRY_INTERVAL_MS - elapsed);
            }
            if (_dht.read(true)) {           // una sola trama forzada...
                t = _dht.readTemperature();  // ...y ambos valores salen de ella
                h = _dht.readHumidity();
            }
        }

        if (!isnan(t) && !isnan(h)) {
            d.dht11_temp_c  = t;   // DHT22: -40..+80 °C, resolución 0.1 (soporta bajo cero)
            d.dht11_hum_pct = h;   // calibrado de fábrica — sin corrección empírica
            d.dht11_ok      = true;
        } else {
            d.dht11_temp_c  = NAN;
            d.dht11_hum_pct = NAN;
            d.dht11_ok      = false;
        }
    }

    // ── Fotorresistencia ADC ──────────────────────────────────────────────────
    // Circuito: 3V3 → R10kΩ → señal → fotorresistencia → GND
    // R_foto = R_pullup * V / (3.3 - V)
    {
        int   raw   = analogRead(PIN_PHOTORESISTOR);
        float v     = (raw / ADC_MAX_RAW) * ADC_VREF;
        float denom = ADC_VREF - v;
        d.photo_kohm = (denom > 0.01f)
                     ? (PHOTO_PULLUP_KOHM * v / denom)
                     : 9999.0f;   // oscuridad total o sensor desconectado
        d.photo_ok = true;
    }

    // ── Rain sensor ADC ───────────────────────────────────────────────────────
    // Circuito PCB: 3V3 → R1‖R2(4.95kΩ) → señal → C1(100nF)‖sensor → GND
    // R_rain = R_pullup * V / (3.3 - V)
    {
        int   raw   = readRainSensorPulsed();
        float v     = (raw / ADC_MAX_RAW) * ADC_VREF;
        float denom = ADC_VREF - v;
        d.rain_kohm = (denom > 0.01f)
                    ? (RAIN_PULLUP_KOHM * v / denom)
                    : 9999.0f;   // sensor seco / desconectado
        d.rain_ok  = true;
    }

    return d;
}
