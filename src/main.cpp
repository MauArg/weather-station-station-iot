#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "command.h"
#include "service_mode.h"

// ─── Clientes globales ────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ─── Estado RTC para ciclo normal ─────────────────────────────────────────────
RTC_DATA_ATTR uint32_t rtc_bootCount = 0;

// ─── Buffers para comando MQTT (llenados en callback) ─────────────────────────
static String  pendingCmdPayload = "";
static bool    cmdReceived       = false;

// ─── Prototipos ───────────────────────────────────────────────────────────────
bool  connectWiFi();
bool  connectMQTT();
void  mqttCallback(char* topic, byte* payload, unsigned int length);
Command waitForRetainedCommand();
void  publishTelemetry();
void  handleCommand(const Command& cmd);
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

    Wire.begin(I2C_SDA, I2C_SCL);

    // ── Si estábamos en service mode antes del reinicio, retomar inmediatamente
    if (serviceMode_isActive()) {
        LOG_V("RTC indica service mode activo — retomando sin leer MQTT");
        if (!connectWiFi()) { goToDeepSleep(); return; }
        connectMQTT();
        // Crear un Command dummy para que evaluate() entre al run() directamente
        Command resumeCmd;
        resumeCmd.type        = CommandType::MAINTENANCE;
        resumeCmd.timeout_min = rtc_serviceTimeoutMin;
        resumeCmd.valid       = true;
        serviceMode_evaluate(mqtt, resumeCmd);
        return;
    }

    // ── Ciclo normal ──────────────────────────────────────────────────────────
    if (!connectWiFi()) { goToDeepSleep(); return; }
    if (!connectMQTT()) { goToDeepSleep(); return; }

    // Leer comando retenido del broker (esperar hasta MQTT_RETAINED_WAIT_MS)
    Command cmd = waitForRetainedCommand();

    // ── Despachar según comando ────────────────────────────────────────────────
    handleCommand(cmd);
}

void loop() {
    // No se usa: el dispositivo sale por deep sleep o reinicio
}

// ═════════════════════════════════════════════════════════════════════════════
// Conexión WiFi
// ═════════════════════════════════════════════════════════════════════════════
bool connectWiFi() {
    WiFi.mode(WIFI_STA);

    // IP estática
    if (!WiFi.config(WIFI_STATIC_IP, WIFI_GATEWAY, WIFI_SUBNET, WIFI_DNS)) {
        LOG_E("Fallo al configurar IP estática");
    }

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    LOG_V("Conectando WiFi...");

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > WIFI_TIMEOUT_MS) {
            LOG_E("WiFi timeout");
            return false;
        }
        delay(200);
    }
    LOG_V("WiFi OK — IP: %s", WiFi.localIP().toString().c_str());
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Conexión MQTT
// ═════════════════════════════════════════════════════════════════════════════
bool connectMQTT() {
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);

    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
        LOG_V("MQTT conectado");
        mqtt.subscribe(TOPIC_CMD);
        return true;
    }
    LOG_E("MQTT error: %d", mqtt.state());
    return false;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (strcmp(topic, TOPIC_CMD) == 0) {
        pendingCmdPayload = "";
        for (unsigned int i = 0; i < length; i++) {
            pendingCmdPayload += (char)payload[i];
        }
        cmdReceived = true;
        LOG_V("CMD recibido: %s", pendingCmdPayload.c_str());
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Esperar mensaje retenido del broker
// ═════════════════════════════════════════════════════════════════════════════
Command waitForRetainedCommand() {
    cmdReceived = false;
    uint32_t start = millis();

    while (!cmdReceived && (millis() - start) < MQTT_RETAINED_WAIT_MS) {
        mqtt.loop();
        delay(20);
    }

    if (!cmdReceived) {
        LOG_V("Sin comando retenido — flujo normal");
        Command none;
        none.type  = CommandType::NONE;
        none.valid = true;
        return none;
    }

    return parseCommand(pendingCmdPayload);
}

// ═════════════════════════════════════════════════════════════════════════════
// Despacho de comandos
// ═════════════════════════════════════════════════════════════════════════════
void handleCommand(const Command& cmd) {
    if (!cmd.valid) {
        LOG_E("Comando inválido — continuando con flujo normal");
        publishTelemetry();
        goToDeepSleep();
        return;
    }

    switch (cmd.type) {

        case CommandType::NONE:
            // Flujo normal: medir y dormir
            publishTelemetry();
            goToDeepSleep();
            break;

        case CommandType::MAINTENANCE:
            // Delegar completamente al módulo de service mode
            serviceMode_evaluate(mqtt, cmd);
            // serviceMode_evaluate no retorna (llama a goToDeepSleep internamente)
            break;

        case CommandType::PING: {
            // Responder con status y continuar ciclo normal
            JsonDocument doc;
            doc["firmware"]   = FIRMWARE_VERSION;
            doc["boot_count"] = rtc_bootCount;
            doc["state"]      = "alive";
            char buf[128];
            serializeJson(doc, buf);
            mqtt.publish(TOPIC_STATUS, buf, false);
            mqtt.loop();
            delay(100);
            mqtt.publish(TOPIC_CMD, "", true);  // limpiar retained
            mqtt.loop();
            delay(100);
            publishTelemetry();
            goToDeepSleep();
            break;
        }

        case CommandType::REBOOT:
            LOG_V("Comando reboot recibido");
            mqtt.publish(TOPIC_STATUS, "{\"state\":\"rebooting\"}", false);
            mqtt.loop();
            delay(200);
            ESP.restart();
            break;

        case CommandType::CONFIG:
            // TODO: implementar cambio de config en NVS
            LOG_V("Comando config — pendiente de implementación");
            publishTelemetry();
            goToDeepSleep();
            break;

        case CommandType::CALIBRATE:
            // TODO: implementar rutina de calibración
            LOG_V("Comando calibrate — pendiente de implementación");
            publishTelemetry();
            goToDeepSleep();
            break;

        default:
            LOG_E("Comando no manejado — flujo normal");
            publishTelemetry();
            goToDeepSleep();
            break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Telemetría (stub — integrar con tus sensores existentes)
// ═════════════════════════════════════════════════════════════════════════════
void publishTelemetry() {
    // TODO: reemplazar con lecturas reales de SHT31 y BMP180
    JsonDocument doc;
    doc["temperature_c"]  = 22.5;
    doc["humidity_pct"]   = 77.0;
    doc["pressure_hpa"]   = 930.0;
    doc["pressure_qnh"]   = 1020.0;
    doc["firmware"]       = FIRMWARE_VERSION;
    doc["boot_count"]     = rtc_bootCount;

    char buf[256];
    serializeJson(doc, buf);
    mqtt.publish(TOPIC_TELEMETRY, buf, false);
    mqtt.loop();
    LOG_V("Telemetría publicada");
}

// ═════════════════════════════════════════════════════════════════════════════
// Deep sleep
// ═════════════════════════════════════════════════════════════════════════════
void goToDeepSleep() {
    LOG_V("Entrando en deep sleep (%d seg)", SLEEP_INTERVAL_SEC);
    WiFi.disconnect(true);
    delay(100);
    esp_deep_sleep((uint64_t)SLEEP_INTERVAL_SEC * 1000000ULL);
}