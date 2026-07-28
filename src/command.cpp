#include "command.h"
#include "config.h"

Command parseCommand(const String& json) {
    Command cmd;
    cmd.raw = json;

    if (json.isEmpty()) {
        // Broker limpió el topic retenido — comportamiento normal
        cmd.type  = CommandType::NONE;
        cmd.valid = true;
        return cmd;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        LOG_E("parseCommand: JSON inválido: %s", err.c_str());
        cmd.valid = false;
        return cmd;
    }

    const char* cmdStr = doc["cmd"] | "";

    if (strcmp(cmdStr, "maintenance") == 0) {
        cmd.type = CommandType::MAINTENANCE;
        int requested = doc["timeout_min"] | SERVICE_MODE_DEFAULT_TIMEOUT_MIN;

        // Techo y piso: el dispositivo es la última línea de defensa.
        //
        // El piso no es cosmético. serviceMode_run() calcula el presupuesto como
        // (uint32_t)timeoutMin * 60, así que un valor negativo se convierte en un
        // uint32 enorme y da una sesión de ~136 años — justo lo contrario de lo
        // que el timeout tiene que garantizar. ArduinoJson solo aplica el default
        // del operador | si la clave falta o es de otro tipo, no si es <= 0, así
        // que un {"timeout_min":-5} publicado a mano llegaba tal cual hasta acá.
        if (requested < 1)                            requested = 1;
        if (requested > SERVICE_MODE_MAX_TIMEOUT_MIN) requested = SERVICE_MODE_MAX_TIMEOUT_MIN;
        cmd.timeout_min = requested;

    } else if (strcmp(cmdStr, "reboot") == 0) {
        cmd.type = CommandType::REBOOT;

    } else if (strcmp(cmdStr, "config") == 0) {
        cmd.type = CommandType::CONFIG;

    } else if (strcmp(cmdStr, "calibrate") == 0) {
        cmd.type = CommandType::CALIBRATE;

    } else if (strcmp(cmdStr, "ping") == 0) {
        cmd.type = CommandType::PING;

    } else if (strcmp(cmdStr, "log_on") == 0) {
        cmd.type = CommandType::LOG;

        // Mismo criterio de clamp que timeout_min, y por la misma razón: el
        // operador | de ArduinoJson sólo aplica el default si la clave falta o
        // es de otro tipo, no si el valor es negativo o absurdo. Estos dos
        // campos son alcanzables desde la consola de JSON crudo de la UI.
        int level = doc["level"] | 2;
        if (level < 0)             level = 0;
        if (level > LOG_MAX_LEVEL) level = LOG_MAX_LEVEL;
        cmd.log_level = (uint8_t)level;

        // 0 = usar la capacidad compilada entera. Valores por encima del ring
        // no son un error: se recortan, porque la RTC memory no se agranda en
        // runtime y la UI no tiene por qué conocer la constante de compilación.
        int entries = doc["entries"] | 0;
        if (entries < 0)                entries = 0;
        if (entries > LOG_RING_ENTRIES) entries = LOG_RING_ENTRIES;
        cmd.log_entries = (uint16_t)entries;

    } else {
        LOG_E("parseCommand: comando desconocido: %s", cmdStr);
        cmd.valid = false;
        return cmd;
    }

    cmd.valid = true;
    return cmd;
}

const char* commandTypeToString(CommandType type) {
    switch (type) {
        case CommandType::NONE:        return "none";
        case CommandType::MAINTENANCE: return "maintenance";
        case CommandType::REBOOT:      return "reboot";
        case CommandType::CONFIG:      return "config";
        case CommandType::CALIBRATE:   return "calibrate";
        case CommandType::PING:        return "ping";
        case CommandType::LOG:         return "log_on";
        default:                       return "unknown";
    }
}
