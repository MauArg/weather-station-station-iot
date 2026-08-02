#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ─── Command types ────────────────────────────────────────────────────────────
// Extensible: add here and in parseCommand()
enum class CommandType {
    NONE,
    MAINTENANCE,   // service mode: suspends sleep, starts ArduinoOTA
    REBOOT,        // clean restart
    CONFIG,        // changes runtime parameters (e.g. sleep interval)
    CALIBRATE,     // forces a calibration routine
    PING,          // responds with status without altering the cycle
    LOG            // turns the runtime logging system on/off
};

// ─── Command payload ──────────────────────────────────────────────────────────
// Example JSON on MQTT:
// {"cmd":"maintenance","timeout_min":15,"issued_at":"2026-03-24T10:00:00Z"}
// {"cmd":"config","params":{"sleep_interval_sec":60}}
// {"cmd":"reboot"}
// {"cmd":"ping"}
// {"cmd":"log_on","level":2,"entries":768}   — level 0 turns it off
struct Command {
    CommandType type        = CommandType::NONE;
    int         timeout_min = 0;          // for MAINTENANCE
    uint8_t     log_level   = 0;          // for LOG — 0=off, 1=anomalies, 2=summary, 3=verbose
    uint16_t    log_entries = 0;          // for LOG — 0 = full compiled-in capacity
    String      raw;                      // raw payload for further processing
    bool        valid       = false;
};

Command     parseCommand(const String& json);
const char* commandTypeToString(CommandType type);
