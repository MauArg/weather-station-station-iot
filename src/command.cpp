#include "command.h"
#include "config.h"

Command parseCommand(const String& json) {
    Command cmd;
    cmd.raw = json;

    if (json.isEmpty()) {
        // Broker cleared the retained topic — normal behavior
        cmd.type  = CommandType::NONE;
        cmd.valid = true;
        return cmd;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        LOG_E("parseCommand: invalid JSON: %s", err.c_str());
        cmd.valid = false;
        return cmd;
    }

    const char* cmdStr = doc["cmd"] | "";

    if (strcmp(cmdStr, "maintenance") == 0) {
        cmd.type = CommandType::MAINTENANCE;
        int requested = doc["timeout_min"] | SERVICE_MODE_DEFAULT_TIMEOUT_MIN;

        // Ceiling and floor: the device is the last line of defense.
        //
        // The floor is not cosmetic. serviceMode_run() computes the budget as
        // (uint32_t)timeoutMin * 60, so a negative value turns into a huge
        // uint32 and produces a ~136-year session — the exact opposite of
        // what the timeout is supposed to guarantee. ArduinoJson's | operator
        // only applies its default when the key is missing or of another
        // type, not when it's <= 0, so a hand-published {"timeout_min":-5}
        // used to arrive here unchanged.
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

        // Same clamping approach as timeout_min, and for the same reason:
        // ArduinoJson's | operator only applies its default when the key is
        // missing or of another type, not when the value is negative or
        // absurd. Both of these fields are reachable from the UI's raw JSON
        // console.
        int level = doc["level"] | 2;
        if (level < 0)             level = 0;
        if (level > LOG_MAX_LEVEL) level = LOG_MAX_LEVEL;
        cmd.log_level = (uint8_t)level;

        // 0 = use the full compiled-in capacity. Values above the ring are
        // not an error: they get clamped, because RTC memory doesn't grow at
        // runtime and the UI has no reason to know the compile-time constant.
        int entries = doc["entries"] | 0;
        if (entries < 0)                entries = 0;
        if (entries > LOG_RING_ENTRIES) entries = LOG_RING_ENTRIES;
        cmd.log_entries = (uint16_t)entries;

    } else if (strcmp(cmdStr, "live") == 0) {
        cmd.type = CommandType::LIVE;

        // Same clamping rationale as timeout_min and level above: ArduinoJson's
        // | operator only fills in the default when the key is missing or of
        // another type, so a hand-published negative or absurd value arrives
        // here untouched, and both fields are reachable from the UI's raw JSON
        // console. Here it matters more than usual — timeout_min feeds a
        // (uint32_t)x*60 budget, which is exactly the multiplication that
        // turned a negative into a ~136-year service-mode session.
        // force skips the sun floor so the mode can be exercised at night or on
        // the bench, where solar_v sits near pack voltage and would end the
        // session in 3 strikes. It skips ONLY that floor — see live_mode.cpp.
        cmd.live_force = doc["force"] | false;

        int timeout = doc["timeout_min"] | LIVE_DEFAULT_TIMEOUT_MIN;
        if (timeout < 1) timeout = 1;

        // A forced session gets a much tighter ceiling, and the reason is
        // arithmetic rather than caution. Normally the sun floor is what ends a
        // session that should not be running; forcing removes it, leaving the
        // battery floor as the only condition-based exit. Going from ~4.05 V to
        // the 3.90 V floor under a ~70 mA load takes on the order of 6 h,
        // because the LiPo curve is flat exactly there — so a forced session
        // could burn a third of the pack overnight before anything tripped.
        //
        // 30 min bounds a forced session to ~35 mAh, ~2% of the pack, and is
        // far more than enough to watch the mechanism work.
        const int ceiling = cmd.live_force ? LIVE_FORCED_MAX_TIMEOUT_MIN
                                           : LIVE_MAX_TIMEOUT_MIN;
        if (timeout > ceiling) timeout = ceiling;
        cmd.timeout_min = timeout;

        int interval = doc["interval_sec"] | LIVE_DEFAULT_INTERVAL_SEC;
        if (interval < LIVE_MIN_INTERVAL_SEC) interval = LIVE_MIN_INTERVAL_SEC;
        if (interval > LIVE_MAX_INTERVAL_SEC) interval = LIVE_MAX_INTERVAL_SEC;
        cmd.live_interval_sec = (uint16_t)interval;

    } else {
        LOG_E("parseCommand: unknown command: %s", cmdStr);
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
        case CommandType::LIVE:        return "live";
        default:                       return "unknown";
    }
}
