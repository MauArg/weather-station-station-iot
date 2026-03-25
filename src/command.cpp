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
        // Aplicar techo absoluto (el dispositivo es la última línea de defensa)
        cmd.timeout_min = min(requested, SERVICE_MODE_MAX_TIMEOUT_MIN);

    } else if (strcmp(cmdStr, "reboot") == 0) {
        cmd.type = CommandType::REBOOT;

    } else if (strcmp(cmdStr, "config") == 0) {
        cmd.type = CommandType::CONFIG;

    } else if (strcmp(cmdStr, "calibrate") == 0) {
        cmd.type = CommandType::CALIBRATE;

    } else if (strcmp(cmdStr, "ping") == 0) {
        cmd.type = CommandType::PING;

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
        default:                       return "unknown";
    }
}
