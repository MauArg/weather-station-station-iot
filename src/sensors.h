#pragma once

struct SensorData {
    // ── Existing I2C sensors (Rail A) ─────────────────────────────────────────
    float temperature_c;       // SHT31 — main sensor
    float humidity_pct;        // SHT31
    float bmp_temperature_c;   // BMP085 — internal reference
    float pressure_hpa;        // BMP085 — local pressure
    float pressure_qnh;        // BMP085 — sea-level pressure (QNH)
    float solar_v;             // INA219 — solar panel voltage
    float solar_mA;            // INA219 — solar panel current
    float solar_mW;            // INA219 — solar panel power
    float system_v;            // INA219 — system voltage (ESP32)
    float system_mA;           // INA219 — system current
    float system_mW;           // INA219 — system power
    bool  sht31_ok;
    bool  bmp_ok;
    bool  solar_ok;
    bool  system_ok;

    // ── New sensors (auxiliary PCB) ───────────────────────────────────────────
    float ds18b20_c;           // DS18B20 — outdoor temperature (always-on)
    bool  ds18b20_ok;

    // Historical dht11_* names: the physical sensor has been a DHT22 since
    // 2026-07-25 (same module, same pin). Kept because they're the
    // telemetry JSON keys, and renaming them would split the historical
    // series in InfluxDB.
    float dht11_temp_c;        // DHT22 — temperature (Rail B), -40..+80 °C
    float dht11_hum_pct;       // DHT22 — humidity (Rail B), factory-calibrated
    bool  dht11_ok;

    float photo_kohm;          // Photoresistor — estimated resistance (Rail B)
    bool  photo_ok;

    float rain_kohm;           // Rain sensor — estimated resistance in kΩ (Rail B)
    bool  rain_ok;

    // TODO [pulses]: anemometer (GPIO2) and rain gauge (GPIO1) — deferred
    //   Requires an accumulated-count strategy in RTC memory between deep sleeps
};

// ─── Rails ────────────────────────────────────────────────────────────────────
// Powers Rail A and Rail B and starts the DHT22 warmup clock.
//
// setup() calls it right at startup, before WiFi. The reason is energy: the
// DHT22 needs ~2 s to stabilize after getting power, and while the rail-on
// was inside sensors_init() —which runs after WiFi+MQTT and the retained
// command wait— those 2 s were paid in full at the end of the cycle, with
// the radio drawing current the whole time. Measured in the field on
// 2026-07-28: that was 61% of the 3.3 s awake window. By moving the rail-on
// earlier, the warmup happens while the node is doing useful work, and the
// remaining delay drops to a few hundred ms.
//
// Idempotent: sensors_init() calls it again and nothing happens. The
// timestamp that counts is the one from the first call.
//
// Does not change the total time the rails stay powered —the cycle shortens
// by exactly as much as the power-on moves earlier— so the rain sensor's
// exposure to continuous voltage, which the pulsed reading is meant to
// minimize, stays the same as before.
void sensors_railsOn();

// Initializes all sensors. Returns false if any fail (not critical).
// Calls sensors_railsOn() on its own if setup() hasn't already.
bool sensors_init();

// Reads all sensors. Failed fields are left as NAN.
SensorData sensors_read();

// ─── Battery monitor for service mode ─────────────────────────────────────────
// sensors_init() does not run in service mode (see main.cpp): the node
// resumes the session without going through the normal cycle, so the INA219s
// never get initialized and the heartbeat can't report the voltage. That
// leaves it blind exactly when it matters most — in service mode the node
// stays awake draining 50-140 mA with no deep sleep to let the voltage recover.
//
// These two functions initialize and read ONLY the system INA219 (0x40),
// without touching Rail A or Rail B: the INA219s hang off the
// always-powered I2C bus (GPIO5/GPIO6, no switched rail — see the pin table
// in componentes_y_conexiones.md), so there's no extra sensor consumption.
// Requires a prior Wire.begin(), which setup() already does before entering
// the mode.
bool  sensors_initSystemMonitor();

// System INA219 bus voltage (battery). NAN if not initialized.
float sensors_readSystemVoltage();

// ─── Low power ─────────────────────────────────────────────────────────────────
// Puts both INA219s into power-down before sleeping: ~6 µA each against the
// ~0.7-1 mA of continuous conversion (INA219 datasheet, consumption table).
// They hang off the 3V3 bus, which the ESP32's regulator keeps powered
// during deep sleep, so without this they keep converting for the ~57 s of
// the cycle that nobody reads them. It's the dominant consumption outside
// the awake window.
//
// Only acts on the ones that got initialized: Adafruit_INA219::powerSave()
// uses i2c_dev without a null check, and the network-failure path enters
// deep sleep without going through sensors_init() — meaning without the
// guard it would crash exactly on the cycles that fail to connect.
//
// No need to wake them back up: begin() rewrites the config register with
// continuous mode, and the first conversion takes ~532 µs, well under the
// time that passes before sensors_read().
void sensors_sleepMonitors();
