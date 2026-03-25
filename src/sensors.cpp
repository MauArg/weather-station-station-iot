#include <math.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_BMP085.h>
#include <Adafruit_INA219.h>

#include "sensors.h"
#include "config.h"

static Adafruit_SHT31  sht31;
static Adafruit_BMP085 bmp;
static Adafruit_INA219 ina219_solar(INA219_SOLAR_ADDR);
static Adafruit_INA219 ina219_system(INA219_SYSTEM_ADDR);

static bool _sht31_ok  = false;
static bool _bmp_ok    = false;
static bool _solar_ok  = false;
static bool _system_ok = false;

bool sensors_init() {
    _sht31_ok  = sht31.begin(0x44);
    _bmp_ok    = bmp.begin();
    _solar_ok  = ina219_solar.begin();
    _system_ok = ina219_system.begin();

    LOG_V("Sensores — SHT31:%s BMP085:%s INA_solar:%s INA_system:%s",
        _sht31_ok  ? "OK" : "ERR",
        _bmp_ok    ? "OK" : "ERR",
        _solar_ok  ? "OK" : "ERR",
        _system_ok ? "OK" : "ERR");

    return _sht31_ok && _bmp_ok && _solar_ok && _system_ok;
}

SensorData sensors_read() {
    SensorData d;
    d.sht31_ok  = _sht31_ok;
    d.bmp_ok    = _bmp_ok;
    d.solar_ok  = _solar_ok;
    d.system_ok = _system_ok;

    // SHT31
    if (_sht31_ok) {
        d.temperature_c = sht31.readTemperature();
        d.humidity_pct  = sht31.readHumidity();
    } else {
        d.temperature_c = NAN;
        d.humidity_pct  = NAN;
    }

    // BMP085
    if (_bmp_ok) {
        d.bmp_temperature_c = bmp.readTemperature();
        d.pressure_hpa      = bmp.readPressure() / 100.0f;
        d.pressure_qnh      = bmp.readSealevelPressure(ALTITUDE_M) / 100.0f;
    } else {
        d.bmp_temperature_c = NAN;
        d.pressure_hpa      = NAN;
        d.pressure_qnh      = NAN;
    }

    // INA219 solar
    if (_solar_ok) {
        d.solar_v  = ina219_solar.getBusVoltage_V();
        d.solar_mA = ina219_solar.getCurrent_mA();
        d.solar_mW = ina219_solar.getPower_mW();
    } else {
        d.solar_v  = NAN;
        d.solar_mA = NAN;
        d.solar_mW = NAN;
    }

    // INA219 sistema
    if (_system_ok) {
        d.system_v  = ina219_system.getBusVoltage_V();
        d.system_mA = ina219_system.getCurrent_mA();
        d.system_mW = ina219_system.getPower_mW();
    } else {
        d.system_v  = NAN;
        d.system_mA = NAN;
        d.system_mW = NAN;
    }

    return d;
}
