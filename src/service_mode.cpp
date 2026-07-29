#include "service_mode.h"
#include "config.h"
#include "sensors.h"        // monitor de batería durante la sesión
#include "logging.h"        // dump de logs — el nodo está despierto y conectado
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <math.h>

// ─── Variables RTC (definición) ───────────────────────────────────────────────
RTC_DATA_ATTR bool     rtc_inServiceMode     = false;
RTC_DATA_ATTR int      rtc_serviceTimeoutMin = SERVICE_MODE_DEFAULT_TIMEOUT_MIN;
RTC_DATA_ATTR uint32_t rtc_serviceElapsedSec = 0;

// ─── Detección de comando limpiado ────────────────────────────────────────────
// A nivel de archivo y no dentro de serviceMode_run() porque hay que poder
// reinstalar el callback después de una reconexión de MQTT.
static volatile bool _cmdCleared = false;

// ─── Pedidos de log pendientes ────────────────────────────────────────────────
// El callback sólo parsea y deja el pedido acá; la respuesta se publica desde
// el loop. Publicar desde adentro del callback de PubSubClient es reentrar en
// el mismo buffer que se está leyendo.
static volatile bool     _logReqPending = false;
static volatile uint8_t  _logReqKind    = 0;   // 1=página  2=diccionario  3=clear
static volatile uint16_t _logReqArg     = 0;
static volatile bool     _logReqKeep    = false;

static void _serviceCmdCallback(char* topic, byte* payload, unsigned int length) {
    if (strcmp(topic, TOPIC_CMD) == 0 && length == 0) {
        _cmdCleared = true;
        LOG_V("Servidor limpió el comando — saliendo de service mode");
        return;
    }

    if (strcmp(topic, TOPIC_LOG_REQ) == 0 && length > 0) {
        JsonDocument doc;
        if (deserializeJson(doc, payload, length)) {
            LOG_E("log/req: JSON inválido");
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
            LOG_E("log/req: pedido no reconocido");
            return;
        }
        _logReqPending = true;
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

// ─── Respuestas del dump de logs ──────────────────────────────────────────────

static void _publishLogPage(PubSubClient& mqtt, uint16_t page) {
    // 60 entries × 8 B = 480 B binarios → 640 B de base64 + terminador.
    static char b64[LOG_ENTRIES_PER_PAGE * 8 * 4 / 3 + 8];
    uint16_t n = 0;

    JsonDocument doc;
    doc["page"]  = page;
    doc["pages"] = logging_pageCount();

    if (!logging_encodePage(page, b64, sizeof(b64), &n)) {
        doc["error"] = "no_page";
    } else {
        doc["count"]   = logging_count();
        // Lo pisado por wraparound: es lo que distingue una captura completa de
        // una truncada. Sin este número no se sabe si la ventana llegó a cubrir
        // el evento que se estaba buscando.
        doc["dropped"] = logging_dropped();
        doc["entries"] = n;
        doc["b64"]     = b64;
    }

    char buf[768];
    size_t len = serializeJson(doc, buf);
    if (!mqtt.publish(TOPIC_LOG_DATA, buf, false)) {
        LOG_E("log page %u no se pudo publicar (%u B)", page, (unsigned)len);
    }
}

// El diccionario no entra en un solo mensaje, así que también va paginado — por
// índice de código en vez de por offset de bytes. El backend lo pide una vez por
// versión de firmware y lo cachea.
static void _publishLogDictPage(PubSubClient& mqtt, uint16_t from) {
    JsonDocument doc;
    doc["dict"] = true;
    doc["from"] = from;
    doc["fw"]   = FIRMWARE_VERSION;   // clave de caché del backend
    JsonArray codes = doc["codes"].to<JsonArray>();

    size_t   budget = 560;            // presupuesto de cuerpo JSON, con margen
    uint16_t i      = from;
    const uint8_t total = logging_codeCount();

    for (; i < total; i++) {
        const char* name = logging_codeName(i);
        const char* tmpl = logging_codeTemplate(i);
        size_t cost = strlen(name) + strlen(tmpl) + 24;  // llaves, claves, comillas

        // Siempre emitir al menos un código: si uno solo excediera el
        // presupuesto, cortar acá dejaría al backend pidiendo la misma página
        // para siempre sin avanzar nunca.
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
        LOG_E("dict page desde %u no se pudo publicar (%u B)", from, (unsigned)len);
    }
}

// Borrado en dos fases: el nodo llega acá sólo cuando el backend ya confirmó
// que tiene todas las páginas. Después de horas de captura, una transferencia
// incompleta no puede costar la sesión entera.
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
    LOG_V("Logs borrados (keep=%d, activo=%d)", keep, logging_isActive());
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

// Reconecta MQTT sin abortar la sesión. El WiFi del ESP32 reasocia solo cuando el
// AP vuelve, así que los reintentos espaciados le dan tiempo a recuperarse; lo que
// hay que rehacer a mano es la suscripción, y el callback por las dudas.
static bool _reconnectMqtt(PubSubClient& mqtt) {
    for (int attempt = 1; attempt <= SERVICE_MODE_MQTT_RETRIES; attempt++) {
        LOG_E("MQTT caído en service mode — reintento %d/%d", attempt, SERVICE_MODE_MQTT_RETRIES);
        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            mqtt.setCallback(_serviceCmdCallback);
            mqtt.subscribe(TOPIC_CMD);
            mqtt.subscribe(TOPIC_LOG_REQ);
            LOG_V("MQTT reconectado — sesión continúa");
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
        LOG_V("OTA iniciando: %s", type.c_str());
    });

    ArduinoOTA.onEnd([]() {
        otaSuccess = true;
        LOG_V("OTA completado");

        // Devolver el presupuesto entero a la sesión de después del flasheo.
        //
        // El reinicio del OTA pasa dentro de ArduinoOTA.handle() y nunca pasa por
        // serviceMode_exit(), así que el acumulador queda con lo consumido antes
        // del flash. Si esta sesión venía de arrastre —por ejemplo 10 min de un
        // presupuesto de 15— la sesión post-flash nacería con casi nada y el nodo
        // se dormiría antes de publicar el service_mode_active con la versión
        // nueva, que es de donde la UI saca la verificación del OTA.
        //
        // No reabre el ciclo infinito que motivó el acumulador: esto solo corre
        // cuando alguien flasheó a propósito, y el deadline del backend sigue
        // acotando el total pase lo que pase.
        rtc_serviceElapsedSec = 0;
        // Acá había un esp_ota_mark_app_valid_cancel_rollback(). Se sacó porque no
        // hacía lo que decía el comentario: onEnd corre en el firmware VIEJO, antes
        // del reinicio, así que marcaba válida la partición que ya estaba corriendo
        // y no la recién escrita. Y la función cancela el rollback, no lo habilita.
        //
        // La imagen nueva la valida el core de Arduino en initArduino(), antes de
        // setup(): si la partición está en ESP_OTA_IMG_PENDING_VERIFY llama a
        // verifyOta() —weak, devuelve true por defecto— y la marca válida. Ver
        // esp32-hal-misc.c. O sea que hoy toda imagen que bootee se acepta sin
        // chequear nada; ver la nota sobre verifyOta() en aprendizajes_y_roadmap.md.
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        LOG_V("OTA progreso: %u%%", (progress / (total / 100)));
    });

    ArduinoOTA.onError([](ota_error_t error) {
        otaSuccess = false;
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
    logging_write(LOG_SERVICE_ENTER, 0, (int16_t)(budgetMs / 1000));
    _setupOTA();

    uint32_t startMs         = millis();
    uint32_t lastHeartbeatMs = 0;
    const uint32_t HEARTBEAT_INTERVAL_MS = (uint32_t)SERVICE_MODE_HEARTBEAT_SEC * 1000;

    // ── Keepalive largo para la sesión ────────────────────────────────────────
    // El nodo llegó hasta acá con el keepalive del ciclo normal, que es corto a
    // propósito para que el broker expire la sesión antes del próximo wake y no
    // haya takeover por client-ID duplicado (ver connectMQTT en main.cpp). Acá el
    // compromiso es el opuesto: la sesión dura minutos, ArduinoOTA.handle() puede
    // bloquear decenas de segundos sin que el nodo mande nada, y el margen del
    // broker es lo único que la sostiene. Takeover no hay: el nodo no se duerme
    // en el medio.
    //
    // El valor que gobierna al broker es el que viajó en el CONNECT, así que
    // cambiarlo obliga a reconectar. Va DESPUÉS del service_mode_active y del
    // setup de OTA a propósito: de ese status sale la verificación del flasheo en
    // la UI y no se puede arriesgar a demorarlo, y de acá en adelante el flasheo
    // ya no depende de MQTT. Un solo intento y sin reintentos — si falla, el loop
    // de abajo reconecta igual, y con el keepalive nuevo, que queda seteado en el
    // cliente. En el camino de re-entrada tras el reinicio del OTA la conexión
    // puede no existir (connectMQTT ignora su retorno ahí): entonces sólo queda
    // seteado el valor y lo aplica _reconnectMqtt.
    mqtt.setKeepAlive(MQTT_KEEPALIVE_SERVICE_SEC);
    if (mqtt.connected()) {
        mqtt.disconnect();
        if (!mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            LOG_E("Reconexión con keepalive largo falló (%d) — el loop reintenta", mqtt.state());
        }
    }

    // Las suscripciones y el callback van después del bloque de arriba a propósito:
    // una reconexión los pierde, y este orden los deja bien puestos tanto si se
    // reconectó como si no.
    _cmdCleared    = false;
    _logReqPending = false;
    mqtt.subscribe(TOPIC_CMD);
    mqtt.subscribe(TOPIC_LOG_REQ);
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

        // ── Atender pedidos de log ────────────────────────────────────────────
        // Acá y no en el callback: publicar desde adentro del callback de
        // PubSubClient reentra en el buffer que se está leyendo.
        if (_logReqPending) {
            _serveLogRequest(mqtt);
        }

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

    // El motivo se codifica acá y su interpretación viaja en la plantilla del
    // diccionario, así que el backend no necesita conocer estos strings.
    uint8_t reasonCode = 0;
    if      (strcmp(reason, "timeout") == 0)           reasonCode = 1;
    else if (strcmp(reason, "cleared_by_server") == 0) reasonCode = 2;
    else if (strcmp(reason, "mqtt_disconnected") == 0) reasonCode = 3;
    logging_write(LOG_SERVICE_EXIT, reasonCode, (int16_t)sessionSec);

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
    // Este camino no pasa por goToDeepSleep(), así que apaga los INA219 por su
    // cuenta — si no, una salida de service mode los dejaría convirtiendo.
    sensors_sleepMonitors();
    esp_deep_sleep((uint64_t)SLEEP_INTERVAL_SEC * 1000000ULL);
}
