#include "service_mode.h"
#include "config.h"
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>    // para rollback

// ─── Variables RTC (definición) ───────────────────────────────────────────────
RTC_DATA_ATTR bool     rtc_inServiceMode    = false;
RTC_DATA_ATTR uint32_t rtc_serviceStartEpoch = 0;
RTC_DATA_ATTR int      rtc_serviceTimeoutMin = SERVICE_MODE_DEFAULT_TIMEOUT_MIN;

// ─── Helpers internos ─────────────────────────────────────────────────────────

static void _publishStatus(PubSubClient& mqtt, const char* state,
                            int remainingSec = -1, const char* extra = nullptr) {
    JsonDocument doc;
    doc["firmware"]  = FIRMWARE_VERSION;
    doc["state"]     = state;
    if (remainingSec >= 0) doc["remaining_sec"] = remainingSec;
    if (extra)             doc["info"]          = extra;

    char buf[256];
    serializeJson(doc, buf);
    // retain=false para status (no queremos que el broker guarde esto)
    mqtt.publish(TOPIC_STATUS, buf, false);
    LOG_V("Status publicado: %s", buf);
}

static void _clearRetainedCmd(PubSubClient& mqtt) {
    // Publicar payload vacío con retain=true limpia el topic en el broker.
    // Así el próximo ciclo no recibe el comando de nuevo.
    mqtt.publish(TOPIC_CMD, "", true);
    LOG_V("Topic cmd limpiado en broker");
}

static int _remainingSeconds(uint32_t startEpoch, int timeoutMin) {
    // Usamos millis() como proxy; no tenemos NTP en esta demo.
    // Si se agrega NTP, reemplazar con epoch real para mayor precisión
    // entre reinicios. Por ahora es suficiente para el timeout de sesión.
    (void)startEpoch;
    static uint32_t enterMillis = 0;
    if (enterMillis == 0) enterMillis = millis();
    long elapsed = (millis() - enterMillis) / 1000;
    long total   = (long)timeoutMin * 60;
    return max(0L, total - elapsed);
}

// ─── OTA setup ────────────────────────────────────────────────────────────────

static bool otaInProgress = false;
static bool otaSuccess    = false;

static void _setupOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        otaInProgress = true;
        otaSuccess    = false;
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
        LOG_V("OTA iniciando: %s", type.c_str());
    });

    ArduinoOTA.onEnd([]() {
        otaInProgress = false;
        otaSuccess    = true;
        LOG_V("OTA completado");
        // Marcar la app como válida ANTES de reiniciar para habilitar rollback
        esp_ota_mark_app_valid_cancel_rollback();
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        LOG_V("OTA progreso: %u%%", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        otaInProgress = false;
        otaSuccess    = false;
        LOG_E("OTA error [%u]", error);
        // No reiniciar — volver al loop del service mode para seguir esperando
    });

    ArduinoOTA.begin();
    LOG_V("ArduinoOTA listo — hostname: %s", OTA_HOSTNAME);
}

// ─── API pública ──────────────────────────────────────────────────────────────

bool serviceMode_isActive() {
    return rtc_inServiceMode;
}

void serviceMode_evaluate(PubSubClient& mqtt, const Command& cmd) {
    if (cmd.type == CommandType::MAINTENANCE) {
        // Servidor pidió service mode
        if (!rtc_inServiceMode) {
            // Primera entrada: registrar timestamp de inicio
            // (usamos millis como proxy; ver nota en _remainingSeconds)
            rtc_serviceStartEpoch = millis() / 1000;
            rtc_serviceTimeoutMin = cmd.timeout_min;
            rtc_inServiceMode     = true;
            LOG_V("Entrando en service mode por comando (timeout: %d min)", cmd.timeout_min);
        } else {
            LOG_V("Continuando service mode (RTC persistido)");
        }
        serviceMode_run(mqtt, rtc_serviceTimeoutMin);

    } else if (rtc_inServiceMode) {
        // Estábamos en service mode pero el comando ya no está (fue limpiado externamente)
        // Ej: N8N limpió el topic por timeout del lado servidor
        LOG_V("Service mode activo en RTC pero sin comando — salida limpia");
        serviceMode_exit(mqtt, "cleared_by_server");

    }
    // Si no hay comando y no estamos en service mode: flujo normal (no hace nada aquí)
}

void serviceMode_run(PubSubClient& mqtt, int timeoutMin) {
    LOG_V("=== SERVICE MODE ACTIVO (max %d min) ===", timeoutMin);

    _publishStatus(mqtt, "service_mode_active", timeoutMin * 60);
    _setupOTA();

    uint32_t startMs           = millis();
    uint32_t timeoutMs         = (uint32_t)timeoutMin * 60 * 1000;
    uint32_t lastHeartbeatMs   = 0;
    uint32_t lastCmdCheckMs    = 0;
    const uint32_t CMD_CHECK_INTERVAL_MS  = 10000;  // chequear cmd topic c/ 10 seg
    const uint32_t HEARTBEAT_INTERVAL_MS  = (uint32_t)SERVICE_MODE_HEARTBEAT_SEC * 1000;

    // Variable local para detectar si el servidor limpió el comando durante la sesión
    // El callback MQTT seteará esto a true si llega un payload vacío en TOPIC_CMD
    static bool cmdCleared = false;
    cmdCleared = false;

    // Re-suscribir para recibir actualizaciones del topic de comando
    mqtt.subscribe(TOPIC_CMD);
    mqtt.setCallback([](char* topic, byte* payload, unsigned int length) {
        if (strcmp(topic, TOPIC_CMD) == 0 && length == 0) {
            cmdCleared = true;
            LOG_V("Servidor limpió el comando — saliendo de service mode");
        }
    });

    while (true) {
        uint32_t now     = millis();
        uint32_t elapsed = now - startMs;
        int      remaining = (int)((timeoutMs - elapsed) / 1000);

        // ── Condición de salida: timeout ──────────────────────────────────────
        if (elapsed >= timeoutMs) {
            LOG_V("Timeout de service mode alcanzado");
            serviceMode_exit(mqtt, "timeout");
            return;
        }

        // ── Condición de salida: servidor limpió el comando ───────────────────
        if (cmdCleared) {
            serviceMode_exit(mqtt, "cleared_by_server");
            return;
        }

        // ── OTA completado: ya se reinició en onEnd() ─────────────────────────
        // Si llegamos aquí después del OTA, algo salió mal en el reinicio.
        // No debería ocurrir, pero por seguridad:
        if (otaSuccess) {
            LOG_V("OTA completado pero sin reinicio — forzando");
            ESP.restart();
        }

        // ── Heartbeat ─────────────────────────────────────────────────────────
        if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
            lastHeartbeatMs = now;
            _publishStatus(mqtt, "service_mode_alive", remaining);
        }

        // ── Mantener MQTT vivo ────────────────────────────────────────────────
        if (!mqtt.connected()) {
            LOG_E("MQTT desconectado durante service mode — reintentando");
            // Aquí podrías agregar lógica de reconexión MQTT
            break;
        }
        mqtt.loop();

        // ── Manejar OTA ───────────────────────────────────────────────────────
        ArduinoOTA.handle();

        delay(100);
    }

    // Llegamos aquí solo si MQTT se desconectó y no reconectó
    serviceMode_exit(mqtt, "mqtt_disconnected");
}

void serviceMode_exit(PubSubClient& mqtt, const char* reason) {
    LOG_V("Saliendo de service mode: %s", reason);

    // Limpiar estado RTC
    rtc_inServiceMode     = false;
    rtc_serviceStartEpoch = 0;
    rtc_serviceTimeoutMin = SERVICE_MODE_DEFAULT_TIMEOUT_MIN;

    // Limpiar comando retenido en broker (si MQTT está disponible)
    if (mqtt.connected()) {
        _clearRetainedCmd(mqtt);
        _publishStatus(mqtt, "service_mode_ended", -1, reason);
        mqtt.loop();
        delay(200); // dar tiempo a que el broker procese los mensajes
    }

    // Volver al ciclo normal
    LOG_V("Entrando en deep sleep desde service_mode_exit");
    esp_deep_sleep((uint64_t)SLEEP_INTERVAL_SEC * 1000000ULL);
}
