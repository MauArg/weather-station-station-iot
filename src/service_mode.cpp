#include "service_mode.h"
#include "config.h"
#include "sensors.h"        // monitor de batería durante la sesión
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>    // para rollback
#include <math.h>

// ─── Variables RTC (definición) ───────────────────────────────────────────────
RTC_DATA_ATTR bool     rtc_inServiceMode     = false;
RTC_DATA_ATTR int      rtc_serviceTimeoutMin = SERVICE_MODE_DEFAULT_TIMEOUT_MIN;
RTC_DATA_ATTR uint32_t rtc_serviceElapsedSec = 0;

// ─── Detección de comando limpiado ────────────────────────────────────────────
// A nivel de archivo y no dentro de serviceMode_run() porque hay que poder
// reinstalar el callback después de una reconexión de MQTT.
static volatile bool _cmdCleared = false;

static void _serviceCmdCallback(char* topic, byte* payload, unsigned int length) {
    (void)payload;
    if (strcmp(topic, TOPIC_CMD) == 0 && length == 0) {
        _cmdCleared = true;
        LOG_V("Servidor limpió el comando — saliendo de service mode");
    }
}

// ─── Helpers internos ─────────────────────────────────────────────────────────

static void _publishStatus(PubSubClient& mqtt, const char* state,
                            int remainingSec = -1, const char* extra = nullptr) {
    JsonDocument doc;
    doc["firmware"]  = FIRMWARE_VERSION;
    doc["state"]     = state;
    if (remainingSec >= 0) doc["remaining_sec"] = remainingSec;
    if (extra)             doc["info"]          = extra;

    // Voltaje de batería medido bajo carga de service mode: el nodo está despierto
    // sin deep sleep que le permita recuperar tensión, así que este número es el
    // relevante para decidir si conviene arrancar un flash. Se omite si el INA219
    // no respondió, para no publicar un cero que se lea como batería agotada.
    float vbat = sensors_readSystemVoltage();
    if (!isnan(vbat)) doc["system_v"] = vbat;

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

// Reconecta MQTT sin abortar la sesión. El WiFi del ESP32 reasocia solo cuando el
// AP vuelve, así que los reintentos espaciados le dan tiempo a recuperarse; lo que
// hay que rehacer a mano es la suscripción, y el callback por las dudas.
static bool _reconnectMqtt(PubSubClient& mqtt) {
    for (int attempt = 1; attempt <= SERVICE_MODE_MQTT_RETRIES; attempt++) {
        LOG_E("MQTT caído en service mode — reintento %d/%d", attempt, SERVICE_MODE_MQTT_RETRIES);
        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            mqtt.setCallback(_serviceCmdCallback);
            mqtt.subscribe(TOPIC_CMD);
            LOG_V("MQTT reconectado — sesión continúa");
            return true;
        }
        delay(SERVICE_MODE_MQTT_RETRY_DELAY_MS);
    }
    return false;
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
            rtc_serviceTimeoutMin = cmd.timeout_min;
            rtc_inServiceMode     = true;
            // rtc_serviceElapsedSec NO se reinicia acá a propósito. Este camino se
            // recorre tanto en el primer armado como al re-entrar después de una
            // salida fallida (MQTT caído, sin poder limpiar el retenido), y desde
            // acá no se distinguen. Reiniciarlo devolvería el presupuesto entero en
            // cada caída, que es justamente el bug. Se pone en cero cuando la sesión
            // cierra bien, en serviceMode_exit(); si quedó un resto de una sesión
            // que nunca pudo cerrar, el próximo armado arranca con menos margen —
            // conservador, que es el lado correcto para equivocarse.
            LOG_V("Entrando en service mode (timeout: %d min, ya consumidos: %u s)",
                  cmd.timeout_min, rtc_serviceElapsedSec);
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

    // Solo el INA219 de sistema, sin encender rails — habilita el campo system_v
    // de los heartbeats. Si falla, la sesión sigue igual: el campo se omite.
    if (!sensors_initSystemMonitor()) {
        LOG_E("INA219 de sistema no respondió — heartbeats sin voltaje");
    }

    // Presupuesto absoluto: el timeout pedido menos lo ya consumido en sesiones
    // anteriores que no pudieron cerrar. Sin esto cada reinicio estrenaba el
    // timeout entero y el nodo podía quedar en ciclo indefinidamente.
    const uint32_t totalSec = (uint32_t)timeoutMin * 60;
    if (rtc_serviceElapsedSec >= totalSec) {
        LOG_V("Presupuesto de service mode agotado (%u/%u s) — saliendo",
              rtc_serviceElapsedSec, totalSec);
        serviceMode_exit(mqtt, "timeout");
        return;
    }
    const uint32_t budgetMs = (totalSec - rtc_serviceElapsedSec) * 1000;

    // remaining_sec informa el saldo TOTAL, no el de esta sesión: si el nodo se
    // reinició, la UI tiene que ver el tiempo real que queda y no un contador que
    // vuelve a arrancar de cero.
    _publishStatus(mqtt, "service_mode_active", (int)(budgetMs / 1000));
    _setupOTA();

    uint32_t startMs         = millis();
    uint32_t lastHeartbeatMs = 0;
    const uint32_t HEARTBEAT_INTERVAL_MS = (uint32_t)SERVICE_MODE_HEARTBEAT_SEC * 1000;

    _cmdCleared = false;
    mqtt.subscribe(TOPIC_CMD);
    mqtt.setCallback(_serviceCmdCallback);

    while (true) {
        uint32_t now     = millis();
        uint32_t elapsed = now - startMs;
        int      remaining = (int)((budgetMs - elapsed) / 1000);

        // ── Condición de salida: timeout ──────────────────────────────────────
        if (elapsed >= budgetMs) {
            LOG_V("Timeout de service mode alcanzado");
            serviceMode_exit(mqtt, "timeout", elapsed / 1000);
            return;
        }

        // ── Condición de salida: servidor limpió el comando ───────────────────
        if (_cmdCleared) {
            serviceMode_exit(mqtt, "cleared_by_server", elapsed / 1000);
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
        // Reconectar en vez de abandonar. Abandonar era caro: la salida no podía
        // limpiar el retenido con el broker caído, así que al despertar el nodo
        // releía el comando y arrancaba una sesión nueva.
        if (!mqtt.connected() && !_reconnectMqtt(mqtt)) {
            break;
        }
        mqtt.loop();

        // ── Manejar OTA ───────────────────────────────────────────────────────
        ArduinoOTA.handle();

        delay(100);
    }

    // Solo se llega acá si MQTT se cayó y no reconectó tras todos los reintentos.
    // El tiempo consumido se acumula igual, así que la próxima entrada arranca con
    // el saldo y no con el presupuesto entero.
    serviceMode_exit(mqtt, "mqtt_disconnected", (millis() - startMs) / 1000);
}

void serviceMode_exit(PubSubClient& mqtt, const char* reason, uint32_t sessionSec) {
    LOG_V("Saliendo de service mode: %s (sesión: %u s)", reason, sessionSec);

    rtc_inServiceMode = false;

    // Acumular siempre, antes de saber si se va a poder cerrar limpio: si esta
    // salida es por MQTT caído, el retenido sigue puesto y el nodo va a re-entrar,
    // y tiene que hacerlo con el saldo y no con el presupuesto entero.
    rtc_serviceElapsedSec += sessionSec;

    // Limpiar comando retenido en broker (si MQTT está disponible)
    if (mqtt.connected()) {
        _clearRetainedCmd(mqtt);
        _publishStatus(mqtt, "service_mode_ended", -1, reason);
        mqtt.loop();
        delay(200); // dar tiempo a que el broker procese los mensajes

        // Se logró limpiar el retenido, así que no puede haber re-entrada: la
        // sesión terminó de verdad y el próximo armado arranca de cero.
        rtc_serviceElapsedSec = 0;
        rtc_serviceTimeoutMin = SERVICE_MODE_DEFAULT_TIMEOUT_MIN;
    }

    // Volver al ciclo normal
    LOG_V("Entrando en deep sleep desde service_mode_exit");
    esp_deep_sleep((uint64_t)SLEEP_INTERVAL_SEC * 1000000ULL);
}
