#include "service_mode.h"
#include "config.h"
#include "sensors.h"        // battery monitor during the session
#include "logging.h"        // log dump — the node is awake and connected
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <math.h>

// ─── RTC variables (definition) ──────────────────────────────────────────────
RTC_DATA_ATTR bool     rtc_inServiceMode     = false;
RTC_DATA_ATTR int      rtc_serviceTimeoutMin = SERVICE_MODE_DEFAULT_TIMEOUT_MIN;
RTC_DATA_ATTR uint32_t rtc_serviceElapsedSec = 0;

// ─── Cleared command detection ────────────────────────────────────────────────
// At file scope and not inside serviceMode_run() because the callback needs
// to be reinstallable after an MQTT reconnect.
static volatile bool _cmdCleared = false;

// ─── Pending log requests ─────────────────────────────────────────────────────
// The callback only parses and leaves the request here; the response is
// published from the loop. Publishing from inside PubSubClient's callback
// means re-entering the very buffer being read.
static volatile bool     _logReqPending = false;
static volatile uint8_t  _logReqKind    = 0;   // 1=page  2=dictionary  3=clear
static volatile uint16_t _logReqArg     = 0;
static volatile bool     _logReqKeep    = false;

static void _serviceCmdCallback(char* topic, byte* payload, unsigned int length) {
    if (strcmp(topic, TOPIC_CMD) == 0 && length == 0) {
        _cmdCleared = true;
        LOG_V("Server cleared the command — exiting service mode");
        return;
    }

    if (strcmp(topic, TOPIC_LOG_REQ) == 0 && length > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, payload, length)) {
            LOG_E("log/req: invalid JSON");
            return;
        }

        if (doc["clear"].as<bool>()) {
            _logReqKind = 3;
            _logReqKeep = doc["keep"] | false;
        } else if (doc["dict"].as<bool>()) {
            _logReqKind = 2;
            int from    = doc["from"] | 0;
            _logReqArg  = (from < 0) ? 0 : (uint16_t)from;
        } else if (doc["page"].is<int>()) {
            _logReqKind = 1;
            int page    = doc["page"] | 0;
            _logReqArg  = (page < 0) ? 0 : (uint16_t)page;
        } else {
            LOG_E("log/req: unrecognized request");
            return;
        }
        _logReqPending = true;
    }
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

static void _publishStatus(PubSubClient& mqtt, const char* state,
                            int remainingSec = -1, const char* extra = nullptr) {
    JsonDocument doc;
    doc["firmware"]  = FIRMWARE_VERSION;
    doc["state"]     = state;
    if (remainingSec >= 0) doc["remaining_sec"] = remainingSec;
    if (extra)             doc["info"]          = extra;

    // Battery voltage measured under service mode load: the node is awake
    // with no deep sleep to let the voltage recover, so this is the number
    // that matters when deciding whether to start a flash. Omitted if the
    // INA219 didn't respond, to avoid publishing a zero that would read as
    // a dead battery.
    float vbat = sensors_readSystemVoltage();
    if (!isnan(vbat)) doc["system_v"] = vbat;

    char buf[256];
    serializeJson(doc, buf);
    // retain=false for status (we don't want the broker to keep this)
    mqtt.publish(TOPIC_STATUS, buf, false);
    LOG_V("Status published: %s", buf);
}

static void _clearRetainedCmd(PubSubClient& mqtt) {
    // Publishing an empty payload with retain=true clears the topic on the
    // broker. That way the next cycle doesn't receive the command again.
    mqtt.publish(TOPIC_CMD, "", true);
    LOG_V("cmd topic cleared on broker");
}

// ─── Log dump responses ───────────────────────────────────────────────────────

static void _publishLogPage(PubSubClient& mqtt, uint16_t page) {
    // 60 entries × 8 B = 480 B binary → 640 B of base64 + terminator.
    static char b64[LOG_ENTRIES_PER_PAGE * 8 * 4 / 3 + 8];
    uint16_t n = 0;

    JsonDocument doc;
    doc["page"]  = page;
    doc["pages"] = logging_pageCount();

    if (!logging_encodePage(page, b64, sizeof(b64), &n)) {
        doc["error"] = "no_page";
    } else {
        doc["count"]   = logging_count();
        // What got overwritten by wraparound: this is what distinguishes a
        // complete capture from a truncated one. Without this number there's
        // no way to know whether the window managed to cover the event
        // being looked for.
        doc["dropped"] = logging_dropped();
        doc["entries"] = n;
        doc["b64"]     = b64;
    }

    char buf[768];
    size_t len = serializeJson(doc, buf);
    if (!mqtt.publish(TOPIC_LOG_DATA, buf, false)) {
        LOG_E("log page %u could not be published (%u B)", page, (unsigned)len);
    }
}

// The dictionary doesn't fit in a single message, so it's also paginated —
// by code index instead of by byte offset. The backend requests it once per
// firmware version and caches it.
static void _publishLogDictPage(PubSubClient& mqtt, uint16_t from) {
    JsonDocument doc;
    doc["dict"] = true;
    doc["from"] = from;
    doc["fw"]   = FIRMWARE_VERSION;   // backend cache key
    JsonArray codes = doc["codes"].to<JsonArray>();

    size_t   budget = 560;            // JSON body budget, with margin
    uint16_t i      = from;
    const uint8_t total = logging_codeCount();

    for (; i < total; i++) {
        const char* name = logging_codeName(i);
        const char* tmpl = logging_codeTemplate(i);
        size_t cost = strlen(name) + strlen(tmpl) + 24;  // braces, keys, quotes

        // Always emit at least one code: if a single one exceeded the
        // budget, cutting off here would leave the backend requesting the
        // same page forever without ever advancing.
        if (cost > budget && i > from) break;
        budget = (cost > budget) ? 0 : budget - cost;

        JsonObject o = codes.add<JsonObject>();
        o["c"] = i;
        o["n"] = name;
        o["t"] = tmpl;
    }

    if (i < total) doc["next"] = i;
    else           doc["done"] = true;

    char buf[768];
    size_t len = serializeJson(doc, buf);
    if (!mqtt.publish(TOPIC_LOG_DATA, buf, false)) {
        LOG_E("dict page from %u could not be published (%u B)", from, (unsigned)len);
    }
}

// Two-phase clear: the node only gets here once the backend has already
// confirmed it has every page. After hours of capture, an incomplete
// transfer can't cost the whole session.
static void _handleLogClear(PubSubClient& mqtt, bool keep) {
    logging_clear();
    if (!keep) logging_configure(0, 0);

    JsonDocument doc;
    doc["cleared"] = true;
    doc["keep"]    = keep;
    doc["active"]  = logging_isActive();

    char buf[128];
    serializeJson(doc, buf);
    mqtt.publish(TOPIC_LOG_DATA, buf, false);
    LOG_V("Logs cleared (keep=%d, active=%d)", keep, logging_isActive());
}

static void _serveLogRequest(PubSubClient& mqtt) {
    _logReqPending = false;
    const uint8_t  kind = _logReqKind;
    const uint16_t arg  = _logReqArg;
    const bool     keep = _logReqKeep;

    switch (kind) {
        case 1: _publishLogPage(mqtt, arg);     break;
        case 2: _publishLogDictPage(mqtt, arg); break;
        case 3: _handleLogClear(mqtt, keep);    break;
        default: break;
    }
    mqtt.loop();
}

// Reconnects MQTT without aborting the session. The ESP32's WiFi
// re-associates on its own once the AP comes back, so spaced-out retries
// give it time to recover; what needs to be redone by hand is the
// subscription, and the callback just in case.
static bool _reconnectMqtt(PubSubClient& mqtt) {
    for (int attempt = 1; attempt <= SERVICE_MODE_MQTT_RETRIES; attempt++) {
        LOG_E("MQTT down in service mode — retry %d/%d", attempt, SERVICE_MODE_MQTT_RETRIES);
        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            mqtt.setCallback(_serviceCmdCallback);
            mqtt.subscribe(TOPIC_CMD);
            mqtt.subscribe(TOPIC_LOG_REQ);
            LOG_V("MQTT reconnected — session continues");
            return true;
        }
        delay(SERVICE_MODE_MQTT_RETRY_DELAY_MS);
    }
    return false;
}

// ─── OTA setup ────────────────────────────────────────────────────────────────

static bool otaSuccess = false;

static void _setupOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        otaSuccess = false;
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
        LOG_V("OTA starting: %s", type.c_str());
    });

    ArduinoOTA.onEnd([]() {
        otaSuccess = true;
        LOG_V("OTA complete");

        // Return the full budget to the session after flashing.
        //
        // The OTA restart happens inside ArduinoOTA.handle() and never goes
        // through serviceMode_exit(), so the accumulator is left with
        // whatever was consumed before the flash. If this session carried
        // over from before —say 10 min out of a 15 min budget— the
        // post-flash session would start with almost nothing and the node
        // would go back to sleep before publishing the service_mode_active
        // with the new version, which is where the UI gets its OTA
        // verification from.
        //
        // This doesn't reopen the infinite loop that motivated the
        // accumulator in the first place: this only runs when someone
        // deliberately flashed, and the backend's deadline still bounds the
        // total no matter what.
        rtc_serviceElapsedSec = 0;
        // There used to be an esp_ota_mark_app_valid_cancel_rollback() here.
        // It was removed because it didn't do what the comment said: onEnd
        // runs on the OLD firmware, before the restart, so it was marking
        // the partition already running as valid, not the freshly written
        // one. And the function cancels the rollback, it doesn't enable it.
        //
        // The new image is validated by the Arduino core in initArduino(),
        // before setup(): if the partition is in ESP_OTA_IMG_PENDING_VERIFY
        // it calls verifyOta() —weak, returns true by default— and marks it
        // valid. See esp32-hal-misc.c. So today every image that boots gets
        // accepted with no checks at all; see the note on verifyOta() in
        // aprendizajes_y_roadmap.md.
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        LOG_V("OTA progress: %u%%", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        otaSuccess = false;
        LOG_E("OTA error [%u]", error);
        // Do not restart — return to the service mode loop to keep waiting
    });

    ArduinoOTA.begin();
    LOG_V("ArduinoOTA ready — hostname: %s", OTA_HOSTNAME);
}

// ─── Public API ───────────────────────────────────────────────────────────────

bool serviceMode_isActive() {
    return rtc_inServiceMode;
}

void serviceMode_evaluate(PubSubClient& mqtt, const Command& cmd) {
    if (cmd.type == CommandType::MAINTENANCE) {
        // Server requested service mode
        if (!rtc_inServiceMode) {
            rtc_serviceTimeoutMin = cmd.timeout_min;
            rtc_inServiceMode     = true;
            // rtc_serviceElapsedSec is deliberately NOT reset here. This
            // path is taken both on the first arming and on re-entering
            // after a failed exit (MQTT down, unable to clear the retained
            // command), and there's no way to tell them apart from here.
            // Resetting it would hand back the full budget on every drop,
            // which is exactly the bug. It's set to zero when the session
            // closes cleanly, in serviceMode_exit(); if a remainder was left
            // from a session that never managed to close, the next arming
            // starts with less margin — conservative, which is the right
            // side to be wrong on.
            LOG_V("Entering service mode (timeout: %d min, already consumed: %u s)",
                  cmd.timeout_min, rtc_serviceElapsedSec);
        } else {
            LOG_V("Continuing service mode (persisted in RTC)");
        }
        serviceMode_run(mqtt, rtc_serviceTimeoutMin);

    } else if (rtc_inServiceMode) {
        // We were in service mode but the command is no longer there (cleared externally)
        // E.g.: N8N cleared the topic due to a server-side timeout
        LOG_V("Service mode active in RTC but no command — clean exit");
        serviceMode_exit(mqtt, "cleared_by_server");

    }
    // If there's no command and we're not in service mode: normal flow (does nothing here)
}

void serviceMode_run(PubSubClient& mqtt, int timeoutMin) {
    LOG_V("=== SERVICE MODE ACTIVE (max %d min) ===", timeoutMin);

    // Only the system INA219, without turning on any rails — enables the
    // system_v field in heartbeats. If it fails, the session continues the
    // same: the field is just omitted.
    if (!sensors_initSystemMonitor()) {
        LOG_E("System INA219 did not respond — heartbeats without voltage");
    }

    // Absolute budget: the requested timeout minus what was already
    // consumed in previous sessions that couldn't close. Without this every
    // restart would get a brand new full timeout and the node could end up
    // looping indefinitely.
    const uint32_t totalSec = (uint32_t)timeoutMin * 60;
    if (rtc_serviceElapsedSec >= totalSec) {
        LOG_V("Service mode budget exhausted (%u/%u s) — exiting",
              rtc_serviceElapsedSec, totalSec);
        serviceMode_exit(mqtt, "timeout");
        return;
    }
    const uint32_t budgetMs = (totalSec - rtc_serviceElapsedSec) * 1000;

    // remaining_sec reports the TOTAL balance, not this session's: if the
    // node restarted, the UI needs to see the real time left, not a counter
    // that starts back at zero.
    _publishStatus(mqtt, "service_mode_active", (int)(budgetMs / 1000));
    logging_write(LOG_SERVICE_ENTER, 0, (int16_t)(budgetMs / 1000));
    _setupOTA();

    uint32_t startMs         = millis();
    uint32_t lastHeartbeatMs = 0;
    const uint32_t HEARTBEAT_INTERVAL_MS = (uint32_t)SERVICE_MODE_HEARTBEAT_SEC * 1000;

    // ── Long keepalive for the session ────────────────────────────────────────
    // The node arrived here with the normal cycle's keepalive, which is
    // short on purpose so the broker expires the session before the next
    // wake and there's no takeover from a duplicate client ID (see
    // connectMQTT in main.cpp). Here the trade-off is the opposite: the
    // session lasts minutes, ArduinoOTA.handle() can block for tens of
    // seconds without the node sending anything, and the broker's margin is
    // the only thing holding it up. There's no takeover risk: the node
    // doesn't go to sleep in the middle.
    //
    // The value that governs the broker is the one that traveled in the
    // CONNECT, so changing it forces a reconnect. It goes AFTER
    // service_mode_active and the OTA setup on purpose: the UI's flash
    // verification comes from that status and can't risk being delayed, and
    // from here on the flash no longer depends on MQTT. A single attempt
    // with no retries — if it fails, the loop below reconnects anyway, and
    // with the new keepalive, which stays set on the client. On the
    // re-entry path after an OTA restart the connection may not exist
    // (connectMQTT ignores its return value there): in that case only the
    // value stays set and _reconnectMqtt applies it.
    mqtt.setKeepAlive(MQTT_KEEPALIVE_SERVICE_SEC);
    if (mqtt.connected()) {
        mqtt.disconnect();
        if (!mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            LOG_E("Reconnect with long keepalive failed (%d) — the loop will retry", mqtt.state());
        }
    }

    // The subscriptions and the callback go after the block above on
    // purpose: a reconnect loses them, and this order leaves them correctly
    // set whether it reconnected or not.
    _cmdCleared    = false;
    _logReqPending = false;
    mqtt.subscribe(TOPIC_CMD);
    mqtt.subscribe(TOPIC_LOG_REQ);
    mqtt.setCallback(_serviceCmdCallback);

    while (true) {
        uint32_t now     = millis();
        uint32_t elapsed = now - startMs;
        int      remaining = (int)((budgetMs - elapsed) / 1000);

        // ── Exit condition: timeout ────────────────────────────────────────────
        if (elapsed >= budgetMs) {
            LOG_V("Service mode timeout reached");
            serviceMode_exit(mqtt, "timeout", elapsed / 1000);
            return;
        }

        // ── Exit condition: server cleared the command ────────────────────────
        if (_cmdCleared) {
            serviceMode_exit(mqtt, "cleared_by_server", elapsed / 1000);
            return;
        }

        // ── OTA complete: already restarted in onEnd() ────────────────────────
        // If we reach here after the OTA, something went wrong with the restart.
        // Shouldn't happen, but just in case:
        if (otaSuccess) {
            LOG_V("OTA complete but no restart happened — forcing one");
            ESP.restart();
        }

        // ── Heartbeat ─────────────────────────────────────────────────────────
        if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
            lastHeartbeatMs = now;
            _publishStatus(mqtt, "service_mode_alive", remaining);
        }

        // ── Keep MQTT alive ────────────────────────────────────────────────────
        // Reconnect instead of giving up. Giving up was expensive: the exit
        // couldn't clear the retained command with the broker down, so on
        // waking the node would read it again and start a new session.
        if (!mqtt.connected() && !_reconnectMqtt(mqtt)) {
            break;
        }
        mqtt.loop();

        // ── Serve log requests ─────────────────────────────────────────────────
        // Here and not in the callback: publishing from inside PubSubClient's
        // callback re-enters the buffer being read.
        if (_logReqPending) {
            _serveLogRequest(mqtt);
        }

        // ── Handle OTA ────────────────────────────────────────────────────────
        ArduinoOTA.handle();

        delay(100);
    }

    // Only reaches here if MQTT dropped and failed to reconnect after every
    // retry. The time consumed is still accumulated, so the next entry
    // starts with the remaining balance, not the full budget.
    serviceMode_exit(mqtt, "mqtt_disconnected", (millis() - startMs) / 1000);
}

void serviceMode_exit(PubSubClient& mqtt, const char* reason, uint32_t sessionSec) {
    LOG_V("Exiting service mode: %s (session: %u s)", reason, sessionSec);

    // The reason is encoded here and its interpretation travels in the
    // dictionary template, so the backend doesn't need to know these strings.
    uint8_t reasonCode = 0;
    if      (strcmp(reason, "timeout") == 0)           reasonCode = 1;
    else if (strcmp(reason, "cleared_by_server") == 0) reasonCode = 2;
    else if (strcmp(reason, "mqtt_disconnected") == 0) reasonCode = 3;
    logging_write(LOG_SERVICE_EXIT, reasonCode, (int16_t)sessionSec);

    rtc_inServiceMode = false;

    // Always accumulate, before knowing whether it will be possible to close
    // cleanly: if this exit is due to MQTT being down, the retained command
    // is still set and the node is going to re-enter, and it has to do so
    // with the remaining balance, not the full budget.
    rtc_serviceElapsedSec += sessionSec;

    // Clear the retained command on the broker (if MQTT is available)
    if (mqtt.connected()) {
        _clearRetainedCmd(mqtt);
        _publishStatus(mqtt, "service_mode_ended", -1, reason);
        mqtt.loop();
        delay(200); // give the broker time to process the messages

        // The retained command was successfully cleared, so there can be no
        // re-entry: the session truly ended and the next arming starts from zero.
        rtc_serviceElapsedSec = 0;
        rtc_serviceTimeoutMin = SERVICE_MODE_DEFAULT_TIMEOUT_MIN;
    }

    // Return to the normal cycle
    LOG_V("Entering deep sleep from service_mode_exit");
    // This path doesn't go through goToDeepSleep(), so it turns off the
    // INA219s on its own — otherwise a service mode exit would leave them converting.
    sensors_sleepMonitors();
    esp_deep_sleep((uint64_t)SLEEP_INTERVAL_SEC * 1000000ULL);
}
