#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// ─── Tipos de comando ─────────────────────────────────────────────────────────
// Extensible: agregar aquí y en parseCommand()
enum class CommandType {
    NONE,
    MAINTENANCE,   // service mode: suspende sleep, inicia ArduinoOTA
    REBOOT,        // reinicio limpio
    CONFIG,        // cambia parámetros en runtime (ej: intervalo de sleep)
    CALIBRATE,     // fuerza una rutina de calibración
    PING,          // responde con status sin alterar el ciclo
    LOG            // activa/desactiva el sistema de logs en runtime
};

// ─── Payload del comando ──────────────────────────────────────────────────────
// Ejemplo JSON en MQTT:
// {"cmd":"maintenance","timeout_min":15,"issued_at":"2026-03-24T10:00:00Z"}
// {"cmd":"config","params":{"sleep_interval_sec":60}}
// {"cmd":"reboot"}
// {"cmd":"ping"}
// {"cmd":"log_on","level":2,"entries":768}   — level 0 desactiva
struct Command {
    CommandType type        = CommandType::NONE;
    int         timeout_min = 0;          // para MAINTENANCE
    uint8_t     log_level   = 0;          // para LOG — 0=off, 1=anomalías, 2=resumen, 3=verboso
    uint16_t    log_entries = 0;          // para LOG — 0 = capacidad compilada entera
    String      raw;                      // payload crudo para procesamiento adicional
    bool        valid       = false;
};

Command     parseCommand(const String& json);
const char* commandTypeToString(CommandType type);
