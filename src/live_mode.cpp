#include "live_mode.h"
#include "config.h"
#include "sensors.h"
#include "telemetry.h"
#include "logging.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <math.h>

RTC_DATA_ATTR uint32_t rtc_liveElapsedSec = 0;

// Set from the MQTT callback when the server clears the retained command, which
// is the normal way a session ends. At file scope, not inside the loop, because
// it has to survive a reconnect reinstalling the callback — the same bug that
// left service mode's deactivate button mute.
static volatile bool _cmdCleared = false;

// Last pack voltage read in the session, so the exit status can report it even
// though liveMode_exit() does not run a sensor read of its own.
static float _lastBattV = NAN;

static void _liveCmdCallback(char* topic, byte* payload, unsigned int length) {
    // ONLY an empty payload counts as "cleared", exactly as service mode does.
    //
    // Treating any message here as end-of-session looks equivalent and is not:
    // the `live` command is retained, so the broker redelivers it the moment
    // this session subscribes. The session would end on its first loop pass,
    // every single time, and live mode would never run for more than an
    // interval.
    //
    // The cost of this choice is that replacing the retained command with a
    // different one (say `maintenance`) does not interrupt an active session —
    // it is picked up after the session ends. The backend has to clear first
    // and then set. Service mode has the same semantics.
    if (strcmp(topic, TOPIC_CMD) == 0 && length == 0) {
        _cmdCleared = true;
        LOG_V("Server cleared the command — ending live mode");
    }
}

static void _publishStatus(PubSubClient& mqtt, const char* state,
                           const char* reason, uint32_t elapsedSec,
                           uint32_t seq, float battV) {
    JsonDocument doc;
    doc["state"]     = state;
    doc["firmware"]  = FIRMWARE_VERSION;
    doc["mode"]      = "live";
    doc["elapsed_s"] = elapsedSec;
    doc["seq"]       = seq;
    if (reason) doc["reason"] = reason;
    // Same rationale as the service-mode heartbeat: live mode is precisely when
    // the node is draining, so a status message without the pack voltage is
    // blind where it matters most. Omitted rather than sent as zero if the
    // INA219 did not answer — a zero reads as a dead battery.
    if (!isnan(battV)) doc["system_v"] = battV;

    char buf[192];
    serializeJson(doc, buf);
    mqtt.publish(TOPIC_STATUS, buf, false);
    mqtt.loop();
}

static bool _reconnectMqtt(PubSubClient& mqtt) {
    for (int attempt = 1; attempt <= LIVE_MQTT_RETRIES; attempt++) {
        LOG_E("MQTT down in live mode — retry %d/%d", attempt, LIVE_MQTT_RETRIES);
        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            // Reinstalling both is mandatory: without the callback and the
            // subscription the node can still publish but can no longer be
            // told to stop, which turns a recoverable drop into a session that
            // only the budget can end.
            mqtt.setCallback(_liveCmdCallback);
            mqtt.subscribe(TOPIC_CMD);
            LOG_V("MQTT reconnected — live session continues");
            return true;
        }
        delay(LIVE_MQTT_RETRY_DELAY_MS);
    }
    return false;
}

void liveMode_exit(PubSubClient& mqtt, const char* reason, uint32_t sessionSec) {
    LOG_V("Live mode ending — reason: %s, %u s", reason, (unsigned)sessionSec);

    rtc_liveElapsedSec += sessionSec;

    if (mqtt.connected()) {
        // _lastBattV rather than NAN: the exit is exactly the moment the pack
        // voltage is worth knowing, and it is the whole story when the reason
        // is low_battery.
        _publishStatus(mqtt, "live_mode_ended", reason, sessionSec, 0, _lastBattV);
        // Clearing the retained command is what actually ends the session. If
        // it stays on the broker the node reads it again on the next wake and
        // re-enters — the reboot-loop failure mode, except here each iteration
        // costs a full live session instead of a restart.
        mqtt.publish(TOPIC_CMD, "", true);
        mqtt.loop();
        delay(200);

        // Only now is re-entry impossible, so only now does the budget reset.
        rtc_liveElapsedSec = 0;
    } else {
        // Could not clear it. The accumulator above keeps the ceiling absolute
        // across the re-entry that is now going to happen.
        LOG_E("Live mode ended with no broker — retained command still set");
    }

    mqtt.disconnect();
    delay(200);
    WiFi.disconnect(true);
    delay(100);
    sensors_sleepMonitors();
    esp_deep_sleep((uint64_t)SLEEP_INTERVAL_SEC * 1000000ULL);
}

void liveMode_run(PubSubClient& mqtt, int timeoutMin, uint16_t intervalSec) {
    const uint32_t budgetSec = (uint32_t)timeoutMin * 60;

    if (rtc_liveElapsedSec >= budgetSec) {
        // Re-entered after using up the whole budget in earlier sessions that
        // could not clear the retained command.
        liveMode_exit(mqtt, "budget_exhausted", 0);
        return;
    }
    const uint32_t remainingSec = budgetSec - rtc_liveElapsedSec;

    LOG_V("Live mode: %u s remaining of %u s budget, publishing every %u s",
          (unsigned)remainingSec, (unsigned)budgetSec, (unsigned)intervalSec);

    _cmdCleared = false;
    mqtt.setCallback(_liveCmdCallback);
    mqtt.subscribe(TOPIC_CMD);
    // The node stays connected for the whole session, so the short keepalive of
    // the normal cycle would have the broker dropping it between publishes at
    // the longer intervals.
    mqtt.setKeepAlive(MQTT_KEEPALIVE_SERVICE_SEC);

    const uint32_t startMs = millis();
    uint32_t seq           = 0;
    uint8_t  lowBattStrikes = 0;
    uint8_t  noSunStrikes   = 0;

    // Entry status carries the pack voltage read on the way in: the operator's
    // first question when a session starts is whether it should have.
    _lastBattV = sensors_readSystemVoltage();
    _publishStatus(mqtt, "live_mode_active", nullptr, 0, 0, _lastBattV);

    for (;;) {
        const uint32_t elapsed = (millis() - startMs) / 1000;

        if (elapsed >= remainingSec) {
            liveMode_exit(mqtt, "timeout", elapsed);
            return;
        }

        if (!mqtt.connected() && !_reconnectMqtt(mqtt)) {
            // Without a broker there is nothing to publish to and no way to
            // receive the stop command, so staying awake only burns the pack.
            liveMode_exit(mqtt, "mqtt_lost", elapsed);
            return;
        }

        SensorData s = publishTelemetry(++seq);
        if (!isnan(s.system_v)) _lastBattV = s.system_v;

        // ── Exit floors ──────────────────────────────────────────────────────
        // Both need consecutive strikes: a single sample dips on a passing
        // cloud or a load transient, and ending the session on one reading
        // would make live mode flap. A NaN is not a strike — a sensor that did
        // not answer says nothing about the panel or the pack, and treating it
        // as a floor breach would end sessions on an I2C hiccup.
        if (!isnan(s.solar_v) && s.solar_v < LIVE_MIN_PANEL_V) {
            if (++noSunStrikes >= LIVE_FLOOR_STRIKES) {
                liveMode_exit(mqtt, "no_sun", elapsed);
                return;
            }
        } else {
            noSunStrikes = 0;
        }

        if (!isnan(s.system_v) && s.system_v < LIVE_MIN_BATTERY_V) {
            if (++lowBattStrikes >= LIVE_FLOOR_STRIKES) {
                liveMode_exit(mqtt, "low_battery", elapsed);
                return;
            }
        } else {
            lowBattStrikes = 0;
        }

        // ── Wait out the interval, keeping MQTT pumped ───────────────────────
        // millis() arithmetic on uint32_t handles the ~49-day rollover as long
        // as the difference is taken signed, which is why this compares a delta
        // rather than `millis() < until`.
        const uint32_t until = millis() + (uint32_t)intervalSec * 1000UL;
        while ((int32_t)(until - millis()) > 0) {
            mqtt.loop();
            if (_cmdCleared) {
                liveMode_exit(mqtt, "cleared_by_server", (millis() - startMs) / 1000);
                return;
            }
            delay(20);
        }
    }
}
