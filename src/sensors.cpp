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
static DHT               _dht(PIN_DHT11, DHT11);

// ─── Estado de inicialización ─────────────────────────────────────────────────
static bool _sht31_ok   = false;
static bool _bmp_ok     = false;
static bool _solar_ok   = false;
static bool _system_ok  = false;
static bool _ds18b20_ok = false;

// =============================================================================
//  Inicialización
// =============================================================================

bool sensors_init() {
    // ── Rails: siempre activos (primera iteración) ────────────────────────────
    // TODO [bajo consumo]: apagar Rail B en Tier 2 y Rail A en Tier 3
    //   según umbral de batería — ver battery.h
    pinMode(PIN_RAIL_A, OUTPUT); digitalWrite(PIN_RAIL_A, HIGH);
    pinMode(PIN_RAIL_B, OUTPUT); digitalWrite(PIN_RAIL_B, HIGH);

    // ── DS18B20 ───────────────────────────────────────────────────────────────
    _ds18b20.begin();
    _ds18b20.setResolution(9);   // ~93 ms conversión
    _ds18b20_ok = (_ds18b20.getDeviceCount() > 0);

    // ── DHT11 ─────────────────────────────────────────────────────────────────
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

    // ── DHT11 warmup: completar espera si el boot fue muy rápido ─────────────
    // El tiempo de WiFi+MQTT (~1.4 s mínimo) generalmente cubre los 2 s.
    // Este delay solo actúa si aún no transcurrieron DHT_WARMUP_MS desde boot.
    uint32_t elapsed = millis();
    if (elapsed < DHT_WARMUP_MS) {
        delay(DHT_WARMUP_MS - elapsed);
    }

    LOG_V("Rails A:HIGH B:HIGH | DS18B20:%s DHT11:init | SHT31:%s BMP:%s INA_sol:%s INA_sys:%s",
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

    // ── DHT11 ─────────────────────────────────────────────────────────────────
    // El DHT11 falla intermitentemente por:
    //   a) interferencia de las interrupciones WiFi durante el protocolo single-wire
    //   b) el caché interno de la librería (MIN_INTERVAL=2s): un retry sin
    //      force=true devuelve el mismo resultado fallido sin releer el sensor.
    // Solución: hasta 2 intentos con force=true y 1 s entre ellos (mínimo
    // de sampleo del DHT11 según datasheet).
    {
        uint32_t t0 = millis();
        float t = _dht.readTemperature();
        float h = _dht.readHumidity();

        if (isnan(t) || isnan(h)) {
            // Retry: el DHT11 necesita 1 s entre lecturas (requerimiento de hardware).
            // Esperamos solo el tiempo que falta desde el primer intento, de modo que
            // si el programa ya tardó ese segundo (WiFi lento, modo realtime, etc.)
            // el delay resultante sea cero. Se usa force=true para saltear el caché
            // interno de la librería, que de lo contrario devolvería el mismo NAN.
            uint32_t elapsed = millis() - t0;
            if (elapsed < 1000) delay(1000 - elapsed);
            t = _dht.readTemperature(false, true);
            h = _dht.readHumidity(true);
        }

        if (!isnan(t) && !isnan(h)) {
            d.dht11_temp_c = t;
            // Calibración lineal medida en caja estanca
            float cal = (h - DHT_HUM_RAW_LO)
                      / (DHT_HUM_RAW_HI - DHT_HUM_RAW_LO)
                      * (DHT_HUM_REAL_HI - DHT_HUM_REAL_LO)
                      + DHT_HUM_REAL_LO;
            if (cal < 0.0f)   cal = 0.0f;
            if (cal > 100.0f) cal = 100.0f;
            d.dht11_hum_pct = cal;
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
    // Circuito PCB: 3V3 → R1‖R2(4.95kΩ) → señal → C1(100nF) → GND
    // Calibración: V_dry=3.3V → 0%, V_wet=2.3V → 100%
    {
        int   raw = analogRead(PIN_RAIN_SENSOR);
        float v   = (raw / ADC_MAX_RAW) * ADC_VREF;
        float pct = (RAIN_V_DRY - v) / (RAIN_V_DRY - RAIN_V_WET) * 100.0f;
        if (pct < 0.0f)   pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        d.rain_pct = pct;
        d.rain_ok  = true;
    }

    return d;
}
