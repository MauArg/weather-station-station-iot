#include <math.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_INA219.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>

#include "sensors.h"
#include "config.h"

// ─── Driver objects ───────────────────────────────────────────────────────────
static Adafruit_SHT31  sht31;
static Adafruit_BMP085 bmp;
static Adafruit_INA219 ina219_solar(INA219_SOLAR_ADDR);
static Adafruit_INA219 ina219_system(INA219_SYSTEM_ADDR);

static OneWire           _oneWire(PIN_DS18B20);
static DallasTemperature _ds18b20(&_oneWire);
static DHT               _dht(PIN_DHT22, DHT22);

// ─── Initialization state ─────────────────────────────────────────────────────
static bool _sht31_ok   = false;
static bool _bmp_ok     = false;
static bool _solar_ok   = false;
static bool _system_ok  = false;
static bool _ds18b20_ok = false;

// ─── Rails ────────────────────────────────────────────────────────────────────
// Moment the rails delivered power, to anchor the DHT22 warmup. In .bss, so
// they start at zero/false on every boot — including a deep sleep wake,
// where only RTC memory survives.
static uint32_t _rail_on_ms = 0;
static bool     _rails_on   = false;

// =============================================================================
//  Battery monitor for service mode
// =============================================================================
// See sensors.h for the why: sensors_init() does not run in service mode, so
// without this the heartbeat can't report voltage. Only touches the system
// INA219 (0x40), which is on the always-powered I2C bus — turns on no rail.

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

// See sensors.h for the why behind the guard: powerSave() does not check
// i2c_dev, and there are paths that reach sleep without having initialized
// anything.
void sensors_sleepMonitors() {
    if (_solar_ok)  ina219_solar.powerSave(true);
    if (_system_ok) ina219_system.powerSave(true);
}

// =============================================================================
//  Initialization
// =============================================================================

// Idempotent on purpose: setup() calls it right at startup, so the DHT22
// warmup runs in parallel with WiFi+MQTT instead of in series. But
// sensors_init() calls it again too, so it stays correct even if someone
// adds a path that reaches init without going through setup(). The
// timestamp is the one from the first call — the moment the rail actually
// delivered power.
void sensors_railsOn() {
    if (_rails_on) return;

    // TODO [low power]: turn off Rail B at Tier 2 and Rail A at Tier 3
    //   based on battery threshold — see battery.h
    pinMode(PIN_RAIL_A, OUTPUT); digitalWrite(PIN_RAIL_A, HIGH);
    pinMode(PIN_RAIL_B, OUTPUT); digitalWrite(PIN_RAIL_B, HIGH);

    _rail_on_ms = millis();   // reference point for the DHT22 warmup
    _rails_on   = true;
}

bool sensors_init() {
    // No-op if setup() already turned them on, which is the normal case.
    sensors_railsOn();

    // ── DS18B20 ───────────────────────────────────────────────────────────────
    _ds18b20.begin();
    _ds18b20.setResolution(9);   // ~93 ms conversion
    _ds18b20_ok = (_ds18b20.getDeviceCount() > 0);

    // ── DHT22 ─────────────────────────────────────────────────────────────────
    _dht.begin();

    // ── Pulse sensors (always-on) — pins configured, data deferred ───────────
    // TODO [pulses]: implement accumulated counting in RTC memory
    pinMode(PIN_ANEMOMETER, INPUT_PULLUP);
    pinMode(PIN_RAIN_GAUGE,  INPUT_PULLUP);

    // ── I2C sensors (Wire already initialized in setup() before reaching here) ─
    _sht31_ok  = sht31.begin(0x44);
    _bmp_ok    = bmp.begin();
    _solar_ok  = ina219_solar.begin();
    _system_ok = ina219_system.begin();

    // ── DHT22 warmup ──────────────────────────────────────────────────────────
    // Measured from when Rail B delivers power, NOT from boot. With the
    // rail-on moved to the start of setup() (1.5.0), by the time execution
    // reaches here WiFi, MQTT and the retained-command wait have already
    // happened, so most of the warmup has already elapsed and this delay
    // ends up a few hundred ms instead of a full 2 s. It goes at the end of
    // init so the I2C bus and DS18B20 time also counts as part of the
    // warmup instead of adding on top.
    uint32_t elapsed = millis() - _rail_on_ms;
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
//  Reading
// =============================================================================

// Excites the rain sensor with a short pulse instead of continuous voltage
// to minimize electrolytic corrosion on the electrodes. GPIO4 acts as an
// output during excitation and then as an ADC for the reading.
// Rail B must be active when calling this function.
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

    // ── Status flags ──────────────────────────────────────────────────────────
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

    // ── INA219 system ─────────────────────────────────────────────────────────
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
    // The fields are still called dht11_* on purpose: they're the telemetry
    // JSON keys, and renaming them would split the NAS InfluxDB's historical
    // series. The physical sensor has been a DHT22 since 2026-07-25.
    {
        uint32_t t0 = millis();
        float t = _dht.readTemperature();
        float h = _dht.readHumidity();   // reuses the cached frame, doesn't reread the bus

        if (isnan(t) || isnan(h)) {
            // Honor the minimum sampling period before forcing another frame.
            uint32_t elapsed = millis() - t0;
            if (elapsed < DHT_RETRY_INTERVAL_MS) {
                delay(DHT_RETRY_INTERVAL_MS - elapsed);
            }
            if (_dht.read(true)) {           // a single forced frame...
                t = _dht.readTemperature();  // ...and both values come from it
                h = _dht.readHumidity();
            }
        }

        if (!isnan(t) && !isnan(h)) {
            d.dht11_temp_c  = t;   // DHT22: -40..+80 °C, 0.1 resolution (handles below zero)
            d.dht11_hum_pct = h;   // factory-calibrated — no empirical correction
            d.dht11_ok      = true;
        } else {
            d.dht11_temp_c  = NAN;
            d.dht11_hum_pct = NAN;
            d.dht11_ok      = false;
        }
    }

    // ── Photoresistor ADC ─────────────────────────────────────────────────────
    // Circuit: 3V3 → R10kΩ → signal → photoresistor → GND
    // R_photo = R_pullup * V / (3.3 - V)
    {
        int   raw   = analogRead(PIN_PHOTORESISTOR);
        float v     = (raw / ADC_MAX_RAW) * ADC_VREF;
        float denom = ADC_VREF - v;
        d.photo_kohm = (denom > 0.01f)
                     ? (PHOTO_PULLUP_KOHM * v / denom)
                     : 9999.0f;   // total darkness or sensor disconnected
        d.photo_ok = true;
    }

    // ── Rain sensor ADC ───────────────────────────────────────────────────────
    // PCB circuit: 3V3 → R1‖R2(4.95kΩ) → signal → C1(100nF)‖sensor → GND
    // R_rain = R_pullup * V / (3.3 - V)
    {
        int   raw   = readRainSensorPulsed();
        float v     = (raw / ADC_MAX_RAW) * ADC_VREF;
        float denom = ADC_VREF - v;
        d.rain_kohm = (denom > 0.01f)
                    ? (RAIN_PULLUP_KOHM * v / denom)
                    : 9999.0f;   // dry / disconnected sensor
        d.rain_ok  = true;
    }

    return d;
}
