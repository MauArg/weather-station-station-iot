#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "command.h"
#include "service_mode.h"
#include "live_mode.h"
#include "sensors.h"
#include "telemetry.h"
#include "logging.h"
#include <esp_system.h>
#include <esp_wifi.h>   // esp_wifi_set_protocol — see WIFI_FORCE_11B in config.h

// ─── Global clients ───────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ─── RTC state for the normal cycle ───────────────────────────────────────────
RTC_DATA_ATTR uint32_t rtc_bootCount    = 0;
RTC_DATA_ATTR uint8_t  rtc_wifiChannel  = 0;
RTC_DATA_ATTR uint8_t  rtc_wifiBssid[6] = {0};

// ─── Buffers for MQTT command (filled in the callback) ────────────────────────
static String  pendingCmdPayload = "";
static bool    cmdReceived       = false;

// ─── Prototypes ───────────────────────────────────────────────────────────────
bool  connectWiFi();
bool  connectMQTT();
void  mqttCallback(char* topic, byte* payload, unsigned int length);
Command waitForRetainedCommand();
void  handleCommand(const Command& cmd);
void  clearRetainedCommand();
void  goToDeepSleep();

// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    #if LOG_LEVEL > 0
        Serial.begin(115200);
        delay(2000);
        Serial.println("=== Boot ===");
    #endif

    rtc_bootCount++;
    LOG_V("=== Boot #%u ===  Firmware: %s", rtc_bootCount, FIRMWARE_VERSION);

    // Before the first logging_write(): the logging state lives in
    // `.rtc_noinit`, which starts out garbage on power-on.
    logging_begin();

    // The reset reason distinguishes a normal wake from a panic, a watchdog
    // or a brownout. It's free and today there's no way to know it in the
    // field — brownout is a live hypothesis given the solar/battery situation.
    //
    // It also marks where rtc_bootCount got reset: that counter lives in
    // `.rtc.data` and gets wiped on any reset that isn't a deep sleep wake,
    // while the ring now survives. So an entry with reset != 8 is the
    // boundary between two lifetimes of the counter, and the backend needs
    // it to avoid misdating whatever is left on the other side.
    logging_write(LOG_BOOT, 0, (int16_t)esp_reset_reason());

    Wire.begin(I2C_SDA, I2C_SCL);

    // ── If we were in service mode before the restart, resume immediately
    if (serviceMode_isActive()) {
        LOG_V("RTC indicates active service mode — resuming without reading MQTT");
        if (!connectWiFi()) { goToDeepSleep(); return; }
        // connectMQTT()'s return value is deliberately ignored: this path is
        // the one after a reflash, and serviceMode_run() brings up ArduinoOTA
        // even without a broker. Aborting here if MQTT fails would close the
        // OTA window exactly when it's needed most — if the new firmware came
        // out broken, the only way to fix it remotely is for that window to
        // open anyway. No MQTT means no heartbeats, but flashing still works.
        connectMQTT();
        // Create a dummy Command so evaluate() goes straight into run()
        Command resumeCmd;
        resumeCmd.type        = CommandType::MAINTENANCE;
        resumeCmd.timeout_min = rtc_serviceTimeoutMin;
        resumeCmd.valid       = true;
        serviceMode_evaluate(mqtt, resumeCmd);
        return;
    }

    // ── Normal cycle ──────────────────────────────────────────────────────────
    // As early as possible on this path: the DHT22 needs ~2 s to stabilize
    // after getting power, and while the rail-on lived inside sensors_init()
    // —which runs after WiFi, MQTT and the retained-command wait— those 2 s
    // were paid in full at the end of the cycle, with the radio associated.
    // That was 61% of the 3.3 s awake window measured in the field on
    // 2026-07-28. Here the warmup happens in parallel with work that had to
    // be done anyway.
    //
    // Goes AFTER the service mode early-return on purpose: that path doesn't
    // touch the rails today, and a session can last up to 60 min. Turning
    // them on there would leave the rain sensor with continuous voltage on
    // the electrodes for the whole session, which is exactly the
    // electrolytic corrosion the pulsed reading avoids.
    sensors_railsOn();

    if (!connectWiFi()) { goToDeepSleep(); return; }
    if (!connectMQTT()) { goToDeepSleep(); return; }

    // Read the broker's retained command (wait up to MQTT_RETAINED_WAIT_MS)
    Command cmd = waitForRetainedCommand();

    // Initialize sensors only on a normal cycle (not in service mode or reboot)
    sensors_init();

    // ── Dispatch by command ───────────────────────────────────────────────────
    handleCommand(cmd);
}

void loop() {
    // Not used: the device exits via deep sleep or restart
}

// ═════════════════════════════════════════════════════════════════════════════
// WiFi connection
// ═════════════════════════════════════════════════════════════════════════════
bool connectWiFi() {
    const uint32_t wifiStartMs = millis();
    WiFi.mode(WIFI_STA);

    // Before WiFi.begin(): the power save mode applies to the association
    // that follows, and with the default (modem sleep) the association dies
    // mid-cycle. See WIFI_POWER_SAVE in config.h for the measurement that
    // led here.
    WiFi.setSleep(WIFI_POWER_SAVE ? true : false);

#if WIFI_FORCE_11B
    // Goes after WiFi.mode(), which is what initializes and starts the
    // driver: esp_wifi_set_protocol() fails with ESP_ERR_WIFI_NOT_STARTED if
    // called before that. See WIFI_FORCE_11B in config.h — it comes from the
    // sniffer decoding the node's management frames (1 Mbps) and not a
    // single one of its data frames (OFDM), at the same distance and the
    // same moment.
    esp_err_t rate_err = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B);
    if (rate_err != ESP_OK) {
        LOG_E("Could not force 802.11b: %d", (int)rate_err);
    }
#endif

    // Static IP
    if (!WiFi.config(WIFI_STATIC_IP, WIFI_GATEWAY, WIFI_SUBNET, WIFI_DNS)) {
        LOG_E("Failed to configure static IP");
    }

    for (int attempt = 1; attempt <= WIFI_MAX_RETRIES; attempt++) {
        logging_write(LOG_WIFI_TRY, (uint8_t)attempt,
                      (attempt == 1) ? (int16_t)rtc_wifiChannel : 0);

        // First attempt uses the cache if available; the rest scan
        if (attempt == 1 && rtc_wifiChannel > 0) {
            LOG_V("WiFi: attempt %d/%d (cached channel %d)", attempt, WIFI_MAX_RETRIES, rtc_wifiChannel);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD, rtc_wifiChannel, rtc_wifiBssid, true);
        } else {
            LOG_V("WiFi: attempt %d/%d (scan)", attempt, WIFI_MAX_RETRIES);
            rtc_wifiChannel = 0;
            WiFi.disconnect(false);
            delay(100);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - start > WIFI_TIMEOUT_MS) break;
            delay(200);
        }

        if (WiFi.status() == WL_CONNECTED) {
            rtc_wifiChannel = WiFi.channel();
            memcpy(rtc_wifiBssid, WiFi.BSSID(), 6);
            LOG_V("WiFi OK — IP: %s  channel: %d  attempt: %d", WiFi.localIP().toString().c_str(), rtc_wifiChannel, attempt);
            logging_write(LOG_WIFI_OK, (uint8_t)attempt, (int16_t)WiFi.RSSI());
            return true;
        }

        LOG_E("WiFi timeout (attempt %d/%d)", attempt, WIFI_MAX_RETRIES);
        logging_write(LOG_WIFI_FAIL, (uint8_t)attempt, (int16_t)WiFi.status());
    }

    // This is the expensive path: WIFI_MAX_RETRIES × WIFI_TIMEOUT_MS can be
    // 45 s awake at 50-140 mA without publishing anything. Recording how
    // much it cost is half of the question the ~17% of lost cycles can't
    // answer today.
    logging_write(LOG_WIFI_GIVEUP, 0, (int16_t)((millis() - wifiStartMs) / 100));
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// MQTT connection
// ═════════════════════════════════════════════════════════════════════════════
bool connectMQTT() {
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    // 512 was too small: the telemetry payload reaches ~546 B with
    // below-zero temperatures (more digits) and the DHT22's 3 fields.
    // PubSubClient silently drops the entire publish if it doesn't fit
    // (buffer = header 5 + 2 + topic 20 + payload). 768 leaves margin for
    // the pending wind subsystem.
    mqtt.setBufferSize(MQTT_BUFFER_BYTES);

    // Short keepalive on purpose in the normal cycle — not because of the
    // client's PING (the node lives 2.2 s and never gets to send it) but
    // because of the keepalive's other effect: the broker declares a
    // session dead at 1.5 × keepalive. At 30 s that's 45 s, below the ~63 s
    // cycle, so every wake finds the client ID free. With the 60 s it used
    // to be, the expiration fell to 90 s and the previous session stayed
    // alive on every reconnection, forcing a duplicate takeover on every
    // cycle. See MQTT_KEEPALIVE_NORMAL_SEC in config.h and the telemetry
    // loss section in ../STATUS.md.
    //
    // Service mode needs the opposite and renegotiates it on its own, by
    // reconnecting (serviceMode_run) — the value that governs the broker is
    // the one that travels in the CONNECT, so changing it on an
    // already-open connection isn't enough.
    mqtt.setKeepAlive(MQTT_KEEPALIVE_NORMAL_SEC);

    // Socket timeout. Lowered from 5 s to 2 s on 2026-07-30 for energy: it's
    // the time paid in full, awake, on every cycle that fails to connect.
    //
    // The number comes from measurement, not taste. Over 30 consecutive
    // successful cycles the awake time was **2291-2293 ms**, with the
    // publish always landing around 2292: zero variance. In other words,
    // when the MQTT handshake works it takes ~46 ms, and a 5 s timeout is
    // 100× what's needed. At 2 s not a single connection that succeeds
    // today gets cut off, and it saves ~3 s on the ~27% of cycles that fail
    // (≈12 mAh/day out of an active budget of ~47).
    //
    // The risk, noted so it can be detected: in the first field capture
    // (1.3.0, a different router config and worse signal) SUCCESSFUL
    // handshakes of 2400-3200 ms were seen. That regime doesn't show up in
    // the current data, but if it came back, this timeout would cut off
    // connections that would have worked. Detectable with a level-1 log
    // capture: `LOG_MQTT_FAIL` with state -4 would show up, which are
    // nearly nonexistent today. If it happens, raise it to 4 s — that only
    // costs ~4 mAh/day relative to 2 s.
    //
    // Can't set 1.5 s: PubSubClient only takes whole seconds.
    mqtt.setSocketTimeout(2);

    const uint32_t mqttStartMs = millis();
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
        LOG_V("MQTT connected");
        logging_write(LOG_MQTT_OK, 0, (int16_t)(millis() - mqttStartMs));
        mqtt.subscribe(TOPIC_CMD);
        return true;
    }
    LOG_E("MQTT error: %d", mqtt.state());
    // The other half of the question: with WiFi up and this down, the cycle
    // is lost either way but the cause is completely different.
    logging_write(LOG_MQTT_FAIL, 0, (int16_t)mqtt.state());
    return false;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (strcmp(topic, TOPIC_CMD) == 0) {
        pendingCmdPayload = "";
        for (unsigned int i = 0; i < length; i++) {
            pendingCmdPayload += (char)payload[i];
        }
        cmdReceived = true;
        LOG_V("CMD received: %s", pendingCmdPayload.c_str());
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Wait for the broker's retained message
// ═════════════════════════════════════════════════════════════════════════════
Command waitForRetainedCommand() {
    cmdReceived = false;
    uint32_t start = millis();

    while (!cmdReceived && (millis() - start) < MQTT_RETAINED_WAIT_MS) {
        mqtt.loop();
        delay(20);
    }

    if (!cmdReceived) {
        LOG_V("No retained command — normal flow");
        Command none;
        none.type  = CommandType::NONE;
        none.valid = true;
        return none;
    }

    return parseCommand(pendingCmdPayload);
}

// ═════════════════════════════════════════════════════════════════════════════
// Command dispatch
// ═════════════════════════════════════════════════════════════════════════════
void handleCommand(const Command& cmd) {
    if (cmd.type != CommandType::NONE) {
        logging_write(LOG_CMD_RX, (uint8_t)cmd.type, 0);
    }

    if (!cmd.valid) {
        LOG_E("Invalid command — continuing with normal flow");
        publishTelemetry();
        goToDeepSleep();
        return;
    }

    switch (cmd.type) {

        case CommandType::NONE:
            // Normal flow: measure and sleep
            publishTelemetry();
            goToDeepSleep();
            break;

        case CommandType::MAINTENANCE:
            // Fully delegate to the service mode module
            serviceMode_evaluate(mqtt, cmd);
            // serviceMode_evaluate does not return (calls goToDeepSleep internally)
            break;

        case CommandType::PING: {
            // Respond with status and continue the normal cycle
            JsonDocument doc;
            doc["firmware"]   = FIRMWARE_VERSION;
            doc["boot_count"] = rtc_bootCount;
            doc["state"]      = "alive";
            char buf[128];
            serializeJson(doc, buf);
            mqtt.publish(TOPIC_STATUS, buf, false);
            mqtt.loop();
            delay(100);
            mqtt.publish(TOPIC_CMD, "", true);  // clear retained
            mqtt.loop();
            delay(100);
            publishTelemetry();
            goToDeepSleep();
            break;
        }

        case CommandType::REBOOT:
            LOG_V("Reboot command received");
            // Clear the retained command BEFORE restarting. Without this the
            // node reads the same {"cmd":"reboot"} again on the next wake and
            // restarts again, in a loop, until the battery runs out — the
            // command stays retained on the broker and nothing clears it.
            // Unlike PING (which clears, below) and MAINTENANCE (which
            // clears on exiting service mode), REBOOT had no way out.
            mqtt.publish(TOPIC_CMD, "", true);
            mqtt.publish(TOPIC_STATUS, "{\"state\":\"rebooting\"}", false);
            mqtt.loop();
            delay(200);
            ESP.restart();
            break;

        // CONFIG and CALIBRATE are stubs, but they still have to clear the
        // retained command: a command that runs and doesn't get cleared
        // gets read again on every wake, forever. It's the same problem
        // REBOOT had, except here nothing restarts — the node would keep
        // logging "not implemented yet" indefinitely and the retained cmd
        // would never leave the broker. Reachable from the UI's raw JSON
        // console.
        case CommandType::CONFIG:
            // TODO: implement config change in NVS
            LOG_V("Config command — not implemented yet");
            clearRetainedCommand();
            publishTelemetry();
            goToDeepSleep();
            break;

        case CommandType::CALIBRATE:
            // TODO: implement calibration routine
            LOG_V("Calibrate command — not implemented yet");
            clearRetainedCommand();
            publishTelemetry();
            goToDeepSleep();
            break;

        case CommandType::LIVE:
            // Publishes one payload through the normal path first, then hands
            // over. That first publish is not cosmetic: it is the last one that
            // carries a fresh boot_count, so the gap detector has an anchor on
            // the boundary between the sleeping cycle and the live session.
            //
            // The retained command is deliberately NOT cleared here. It is what
            // keeps the session alive across a restart — a watchdog or brownout
            // during live mode drops the node into the normal cycle, where it
            // reads this same command and re-enters through the entry checks.
            // liveMode_exit() is the only place that clears it.
            LOG_V("live command — interval %us, timeout %d min, force %d",
                  cmd.live_interval_sec, cmd.timeout_min, (int)cmd.live_force);
            publishTelemetry();
            liveMode_run(mqtt, cmd.timeout_min, cmd.live_interval_sec, cmd.live_force);
            // liveMode_run does not return (exits via deep sleep)
            break;

        case CommandType::LOG:
            // Does not enter service mode on purpose: logging has to run
            // during the normal 60 s cycles, which is exactly what needs to
            // be observed. The node applies the config, clears the retained
            // command and continues the cycle — the cost of capturing is an
            // 8-byte memcpy, so it changes nothing about consumption.
            LOG_V("log_on command — level %u, entries %u", cmd.log_level, cmd.log_entries);
            logging_configure(cmd.log_level, cmd.log_entries);
            clearRetainedCommand();
            publishTelemetry();
            goToDeepSleep();
            break;

        default:
            LOG_E("Unhandled command — normal flow");
            clearRetainedCommand();
            publishTelemetry();
            goToDeepSleep();
            break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Clear the retained command
// ═════════════════════════════════════════════════════════════════════════════
// An empty payload with retain=true erases the broker's retained message.
// Every command that runs has to do this, otherwise it arrives again on the
// next wake and repeats indefinitely.
void clearRetainedCommand() {
    mqtt.publish(TOPIC_CMD, "", true);
    mqtt.loop();
    delay(100);
}

// ═════════════════════════════════════════════════════════════════════════════
// Telemetry
// ═════════════════════════════════════════════════════════════════════════════
SensorData publishTelemetry(uint32_t liveSeq) {
    SensorData s = sensors_read();

    JsonDocument doc;

    if (!isnan(s.temperature_c))     doc["temperature_c"]     = s.temperature_c;
    if (!isnan(s.humidity_pct))      doc["humidity_pct"]      = s.humidity_pct;
    if (!isnan(s.bmp_temperature_c)) doc["bmp_temperature_c"] = s.bmp_temperature_c;
    if (!isnan(s.pressure_hpa))      doc["pressure_hpa"]      = s.pressure_hpa;
    if (!isnan(s.pressure_qnh))      doc["pressure_qnh"]      = s.pressure_qnh;
    if (!isnan(s.solar_v))           doc["solar_v"]           = s.solar_v;
    if (!isnan(s.solar_mA))          doc["solar_mA"]          = s.solar_mA;
    if (!isnan(s.solar_mW))          doc["solar_mW"]          = s.solar_mW;
    if (!isnan(s.system_v))          doc["system_v"]          = s.system_v;
    if (!isnan(s.system_mA))         doc["system_mA"]         = s.system_mA;
    if (!isnan(s.system_mW))         doc["system_mW"]         = s.system_mW;

    doc["sht31_ok"]  = s.sht31_ok;
    doc["bmp_ok"]    = s.bmp_ok;
    doc["solar_ok"]  = s.solar_ok;
    doc["system_ok"] = s.system_ok;

    if (!isnan(s.ds18b20_c))     doc["ds18b20_c"]     = s.ds18b20_c;
    if (!isnan(s.dht11_temp_c))  doc["dht11_temp_c"]  = s.dht11_temp_c;
    if (!isnan(s.dht11_hum_pct)) doc["dht11_hum_pct"] = s.dht11_hum_pct;
    if (!isnan(s.photo_kohm))    doc["photo_kohm"]    = s.photo_kohm;
    if (!isnan(s.rain_kohm))     doc["rain_kohm"]     = s.rain_kohm;

    doc["ds18b20_ok"] = s.ds18b20_ok;
    doc["dht11_ok"]   = s.dht11_ok;
    doc["photo_ok"]   = s.photo_ok;
    doc["rain_ok"]    = s.rain_ok;

    doc["rssi_dbm"]   = (int)WiFi.RSSI();
    doc["firmware"]   = FIRMWARE_VERSION;
    doc["boot_count"] = rtc_bootCount;

    // Only while a capture is running: zero cost in normal operation, same
    // as the rest of the conditional fields above. Since capturing costs no
    // energy, there's no time-based auto-expiry — the correct hygiene is
    // that it's visible, not a timer that shuts off the capture right when
    // it was useful.
    if (logging_isActive()) {
        doc["log_active"] = logging_level();
        doc["log_count"]  = logging_count();
    }

    // Live mode only. boot_count increments in setup(), so it freezes for the
    // whole session — and it is what the backend's gap detector counts to
    // measure telemetry loss. Without a per-publish counter the loss instrument
    // that this project spent the entire MQTT investigation building would go
    // blind exactly in the mode that publishes the most.
    if (liveSeq > 0) {
        doc["live"]     = true;
        doc["live_seq"] = liveSeq;
    }

    char buf[MQTT_BUFFER_BYTES];
    size_t len = serializeJson(doc, buf);

    if (!mqtt.publish(TOPIC_TELEMETRY, buf, false)) {
        // publish() returns false for two very different reasons: the
        // payload doesn't fit in the buffer, or the socket write failed.
        // Telling them apart here, where the budget is known, keeps the log
        // from blaming the buffer for a dropped connection — which is
        // exactly what happened in the first field capture (2026-07-28):
        // 505 B against 741 available, reported as "short buffer?".
        const bool toobig = (int)len > MQTT_TELEMETRY_BUDGET;
        LOG_E("Telemetry publish failed (%u B of %d usable) — %s",
              (unsigned)len, MQTT_TELEMETRY_BUDGET,
              toobig ? "doesn't fit in the buffer" : "connection dropped");
        logging_write(LOG_PUBLISH_FAIL, toobig ? 1 : 2, (int16_t)len);
    } else {
        LOG_V("Telemetry published (%u B)", (unsigned)len);
        logging_write(LOG_PUBLISH_OK, 0, (int16_t)len);
    }
    mqtt.loop();
    return s;
}

// ═════════════════════════════════════════════════════════════════════════════
// Deep sleep
// ═════════════════════════════════════════════════════════════════════════════
void goToDeepSleep() {
    LOG_V("Entering deep sleep (%d sec)", SLEEP_INTERVAL_SEC);
    // Closes the cycle in the log: awake time is the metric that connects
    // connection failures with consumption (10 s for a healthy cycle
    // against the 45 s of one that exhausts its WiFi retries).
    logging_write(LOG_SLEEP, 0, (int16_t)(millis() / 100));
    mqtt.disconnect();
    delay(200);
    WiFi.disconnect(true);
    delay(100);

    // At the end of teardown on purpose, not between the publish and the
    // disconnect: this way the network path stays identical to 1.3.1's and
    // the 42%-lost-payloads measurement remains comparable against that
    // baseline. It also happens to take I2C out of the critical path —
    // powerSave() does a read-modify-write on two chips and TwoWire has a
    // 50 ms timeout per transaction, so a stuck bus (this project has a
    // history of that) could add up to 200 ms right there.
    sensors_sleepMonitors();

    esp_deep_sleep((uint64_t)SLEEP_INTERVAL_SEC * 1000000ULL);
}