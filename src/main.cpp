#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"
#include "command.h"
#include "service_mode.h"
#include "sensors.h"
#include "logging.h"
#include <esp_system.h>

// ─── Clientes globales ────────────────────────────────────────────────────────
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ─── Estado RTC para ciclo normal ─────────────────────────────────────────────
RTC_DATA_ATTR uint32_t rtc_bootCount    = 0;
RTC_DATA_ATTR uint8_t  rtc_wifiChannel  = 0;
RTC_DATA_ATTR uint8_t  rtc_wifiBssid[6] = {0};

// ─── Foto del enlace al cerrar el ciclo, para diagnosticar los perdidos ───────
// El problema de observabilidad de los ciclos perdidos es que, por definición, no
// publican nada. Pero el ciclo SIGUIENTE sí publica ~60% de las veces, y la RTC
// memory sobrevive al deep sleep: alcanza con guardar acá el estado del enlace en
// el último instante del ciclo y mandarlo en la telemetría de después.
//
// Contesta la pregunta que decide cuál es el arreglo: al final de un ciclo que se
// perdió, ¿el nodo SABE que se quedó sin enlace (`WiFi.status()` != 3) o cree que
// sigue asociado? Si lo sabe, alcanza con re-chequear y reasociarse antes de
// publicar. Si no lo sabe, el driver está sordo y mudo creyéndose conectado, y
// hace falta confirmación de entrega en banda.
//
// Sin captura de logs ni service mode, que es lo que hacía cara esta pregunta.
RTC_DATA_ATTR uint32_t rtc_prevBoot     = 0;
RTC_DATA_ATTR uint8_t  rtc_prevWifiSt   = 0;
RTC_DATA_ATTR int8_t   rtc_prevRssi     = 0;
RTC_DATA_ATTR int8_t   rtc_prevMqttSt   = 0;
RTC_DATA_ATTR uint16_t rtc_prevAwakeMs  = 0;

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
void  clearRetainedCommand();
void  goToDeepSleep();
void  publishUplinkBeacon(const char* mark);

// ═════════════════════════════════════════════════════════════════════════════
void setup() {
    #if LOG_LEVEL > 0
        Serial.begin(115200);
        delay(2000);
        Serial.println("=== Boot ===");
    #endif

    rtc_bootCount++;
    LOG_V("=== Boot #%u ===  Firmware: %s", rtc_bootCount, FIRMWARE_VERSION);

    // Antes del primer logging_write(): el estado del logging vive en
    // `.rtc_noinit`, que en un power-on arranca con basura.
    logging_begin();

    // El motivo del reset distingue un wake normal de un panic, un watchdog o
    // una brownout. Es gratis y hoy no hay forma de saberlo en campo — la
    // brownout es una hipótesis viva dada la situación solar/batería.
    //
    // Además marca dónde se reinició rtc_bootCount: ese contador vive en
    // `.rtc.data` y se borra en cualquier reset que no sea wake de deep sleep,
    // mientras que el ring ahora sobrevive. O sea que una entry con reset != 8
    // es la frontera entre dos vidas del contador, y el backend la necesita para
    // no fechar mal lo que quedó del otro lado.
    logging_write(LOG_BOOT, 0, (int16_t)esp_reset_reason());

    Wire.begin(I2C_SDA, I2C_SCL);

    // ── Si estábamos en service mode antes del reinicio, retomar inmediatamente
    if (serviceMode_isActive()) {
        LOG_V("RTC indica service mode activo — retomando sin leer MQTT");
        if (!connectWiFi()) { goToDeepSleep(); return; }
        // El retorno de connectMQTT() se ignora a propósito: este camino es el de
        // después de un reflash, y serviceMode_run() levanta ArduinoOTA aunque no
        // haya broker. Abortar acá si MQTT falla cerraría la ventana de OTA justo
        // cuando más la necesitás — si el firmware nuevo salió mal, la única forma
        // de corregirlo a distancia es que esa ventana se abra igual. Sin MQTT no
        // hay heartbeats, pero el flasheo funciona.
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
    // Lo antes posible dentro de este camino: el DHT22 pide ~2 s de estabilización
    // tras recibir energía, y mientras el rail-on vivía dentro de sensors_init()
    // —que corre después de WiFi, MQTT y la espera del retenido— esos 2 s se
    // pagaban enteros al final del ciclo, con la radio asociada. Eran el 61% de
    // los 3,3 s de ventana despierta medidos en campo el 2026-07-28. Acá el warmup
    // transcurre en paralelo con trabajo que había que hacer igual.
    //
    // Va DESPUÉS del early-return de service mode a propósito: ese camino no toca
    // los rails hoy, y una sesión puede durar hasta 60 min. Encenderlos ahí dejaría
    // al sensor de lluvia con tensión continua sobre los electrodos durante toda la
    // sesión, que es justo la corrosión electrolítica que la lectura pulsada evita.
    sensors_railsOn();

    if (!connectWiFi()) { goToDeepSleep(); return; }
    if (!connectMQTT()) { goToDeepSleep(); return; }

    // Leer comando retenido del broker (esperar hasta MQTT_RETAINED_WAIT_MS)
    Command cmd = waitForRetainedCommand();

    // Baliza: último instante conocido antes de la lectura de sensores, que es el
    // tramo más largo del ciclo sin tráfico de red. Ver UPLINK_BEACON en config.h.
    publishUplinkBeacon("pre_sensors");

    // Inicializar sensores solo en ciclo normal (no en service mode ni reboot)
    sensors_init();

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
    const uint32_t wifiStartMs = millis();
    WiFi.mode(WIFI_STA);

    // Antes de WiFi.begin(): el modo de power save se aplica a la asociación que
    // viene, y con el default (modem sleep) la asociación se muere a mitad del
    // ciclo. Ver WIFI_POWER_SAVE en config.h para la medición que llevó acá.
    WiFi.setSleep(WIFI_POWER_SAVE ? true : false);

    // IP estática
    if (!WiFi.config(WIFI_STATIC_IP, WIFI_GATEWAY, WIFI_SUBNET, WIFI_DNS)) {
        LOG_E("Fallo al configurar IP estática");
    }

    for (int attempt = 1; attempt <= WIFI_MAX_RETRIES; attempt++) {
        logging_write(LOG_WIFI_TRY, (uint8_t)attempt,
                      (attempt == 1) ? (int16_t)rtc_wifiChannel : 0);

        // Primer intento usa caché si está disponible; los siguientes escanean
        if (attempt == 1 && rtc_wifiChannel > 0) {
            LOG_V("WiFi: intento %d/%d (canal cacheado %d)", attempt, WIFI_MAX_RETRIES, rtc_wifiChannel);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD, rtc_wifiChannel, rtc_wifiBssid, true);
        } else {
            LOG_V("WiFi: intento %d/%d (scan)", attempt, WIFI_MAX_RETRIES);
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
            LOG_V("WiFi OK — IP: %s  canal: %d  intento: %d", WiFi.localIP().toString().c_str(), rtc_wifiChannel, attempt);
            logging_write(LOG_WIFI_OK, (uint8_t)attempt, (int16_t)WiFi.RSSI());
            return true;
        }

        LOG_E("WiFi timeout (intento %d/%d)", attempt, WIFI_MAX_RETRIES);
        logging_write(LOG_WIFI_FAIL, (uint8_t)attempt, (int16_t)WiFi.status());
    }

    // Este es el camino caro: WIFI_MAX_RETRIES × WIFI_TIMEOUT_MS pueden ser 45 s
    // despierto a 50-140 mA sin publicar nada. Registrar cuánto costó es la mitad
    // de la pregunta que el ~17% de ciclos perdidos no puede responder hoy.
    logging_write(LOG_WIFI_GIVEUP, 0, (int16_t)((millis() - wifiStartMs) / 100));
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Conexión MQTT
// ═════════════════════════════════════════════════════════════════════════════
bool connectMQTT() {
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    // 512 quedaba corto: el payload de telemetría llega a ~546 B con temperaturas
    // bajo cero (más dígitos) y los 3 campos del DHT22. PubSubClient descarta el
    // publish entero y en silencio si no entra (buffer = header 5 + 2 + topic 20
    // + payload). 768 deja margen para el subsistema de viento pendiente.
    mqtt.setBufferSize(MQTT_BUFFER_BYTES);

    // Keepalive corto a propósito en el ciclo normal — no por el PING del cliente
    // (el nodo vive 2,2 s y nunca llega a mandarlo) sino por el otro efecto del
    // keepalive: el broker da por muerta una sesión a los 1,5 × keepalive. A 30 s
    // eso son 45 s, por debajo del ciclo de ~63 s, así que cada wake encuentra el
    // client-ID libre. Con los 60 s que había antes la expiración caía a los 90 s
    // y la sesión anterior seguía viva en cada reconexión, forzando un takeover por
    // duplicado en todos los ciclos. Ver MQTT_KEEPALIVE_NORMAL_SEC en config.h y
    // la sección de pérdida de telemetría en ../STATUS.md.
    //
    // Service mode necesita lo contrario y lo renegocia por su cuenta, reconectando
    // (serviceMode_run) — el valor que gobierna al broker es el que viaja en el
    // CONNECT, así que no alcanza con cambiarlo sobre una conexión ya abierta.
    mqtt.setKeepAlive(MQTT_KEEPALIVE_NORMAL_SEC);

    // Socket timeout. Bajado de 5 s a 2 s el 2026-07-30 por energía: es el tiempo
    // que se paga entero, despierto, en cada ciclo que no logra conectar.
    //
    // El número sale de la medición, no del gusto. Sobre 30 ciclos exitosos
    // consecutivos el tiempo despierto fue de **2291-2293 ms**, con el publish
    // saliendo siempre a los ~2292: varianza cero. O sea que cuando el handshake
    // MQTT funciona tarda ~46 ms, y un timeout de 5 s es 100× lo que hace falta.
    // Con 2 s no se corta ni una conexión de las que hoy prosperan, y se ahorran
    // ~3 s en el ~27% de ciclos que fallan (≈12 mAh/día sobre un presupuesto
    // activo de ~47).
    //
    // El riesgo, anotado para poder detectarlo: en la primera captura de campo
    // (1.3.0, otra configuración de router y peor señal) se vieron handshakes
    // EXITOSOS de 2400-3200 ms. Ese régimen no aparece en los datos actuales,
    // pero si volviera, este timeout cortaría conexiones que habrían funcionado.
    // Se detecta sin ambigüedad: los ciclos perdidos con `pv_mq = -4` subirían
    // mientras el tiempo despierto de los sanos sigue en 2292 ms. Si pasa, subir
    // a 4 s — cuesta sólo ~4 mAh/día respecto de 2 s.
    //
    // No se puede poner 1,5 s: PubSubClient toma segundos enteros.
    mqtt.setSocketTimeout(2);

    const uint32_t mqttStartMs = millis();
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
        LOG_V("MQTT conectado");
        logging_write(LOG_MQTT_OK, 0, (int16_t)(millis() - mqttStartMs));
        mqtt.subscribe(TOPIC_CMD);
        return true;
    }
    LOG_E("MQTT error: %d", mqtt.state());
    // La otra mitad de la pregunta: con WiFi arriba y esto abajo, el ciclo se
    // pierde igual pero la causa es completamente distinta.
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
    if (cmd.type != CommandType::NONE) {
        logging_write(LOG_CMD_RX, (uint8_t)cmd.type, 0);
    }

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
            // Limpiar el retained ANTES de reiniciar. Sin esto el nodo vuelve a
            // leer el mismo {"cmd":"reboot"} en el siguiente wake y reinicia otra
            // vez, en loop, hasta agotar la batería — el comando queda retenido en
            // el broker y nada lo borra. A diferencia de PING (que limpia, más
            // abajo) y de MAINTENANCE (que limpia al salir de service mode),
            // REBOOT no tenía salida.
            mqtt.publish(TOPIC_CMD, "", true);
            mqtt.publish(TOPIC_STATUS, "{\"state\":\"rebooting\"}", false);
            mqtt.loop();
            delay(200);
            ESP.restart();
            break;

        // CONFIG y CALIBRATE son stubs, pero igual tienen que limpiar el retenido:
        // un comando que se ejecuta y no se borra se vuelve a leer en cada wake,
        // para siempre. Es el mismo problema que tenía REBOOT, solo que acá no
        // reinicia nada — el nodo quedaría logueando "pendiente de implementación"
        // indefinidamente y el cmd retenido nunca se iría del broker. Alcanzable
        // desde la consola de JSON crudo de la UI.
        case CommandType::CONFIG:
            // TODO: implementar cambio de config en NVS
            LOG_V("Comando config — pendiente de implementación");
            clearRetainedCommand();
            publishTelemetry();
            goToDeepSleep();
            break;

        case CommandType::CALIBRATE:
            // TODO: implementar rutina de calibración
            LOG_V("Comando calibrate — pendiente de implementación");
            clearRetainedCommand();
            publishTelemetry();
            goToDeepSleep();
            break;

        case CommandType::LOG:
            // No entra en service mode a propósito: el logging tiene que correr
            // durante los ciclos normales de 60 s, que es justamente lo que se
            // quiere observar. El nodo aplica la config, limpia el retenido y
            // sigue de largo con el ciclo — el costo de capturar es un memcpy
            // de 8 bytes, así que no cambia nada del consumo.
            LOG_V("Comando log_on — nivel %u, entries %u", cmd.log_level, cmd.log_entries);
            logging_configure(cmd.log_level, cmd.log_entries);
            clearRetainedCommand();
            publishTelemetry();
            goToDeepSleep();
            break;

        default:
            LOG_E("Comando no manejado — flujo normal");
            clearRetainedCommand();
            publishTelemetry();
            goToDeepSleep();
            break;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Limpiar el comando retenido
// ═════════════════════════════════════════════════════════════════════════════
// Un payload vacío con retain=true borra el mensaje retenido del broker. Todo
// comando que se ejecuta tiene que hacer esto, si no vuelve a llegar en el
// próximo wake y se repite indefinidamente.
void clearRetainedCommand() {
    mqtt.publish(TOPIC_CMD, "", true);
    mqtt.loop();
    delay(100);
}

// ═════════════════════════════════════════════════════════════════════════════
// Baliza de diagnóstico del uplink
// ═════════════════════════════════════════════════════════════════════════════
// Marcador chico en un topic propio, con el boot_count adentro: eso es lo que
// permite cruzarlo contra la telemetría que falta y decir "el ciclo 208 llegó
// hasta acá". Sin retain — no tiene sentido que sobreviva al ciclo, y el topic
// retenido de comandos es de slot único.
//
// No se chequea el retorno a propósito: en QoS 0 no significa entrega (es
// justamente lo que estamos midiendo) y un fallo local se vería igual en el
// LOG_PUBLISH_FAIL de la telemetría, dos líneas después.
void publishUplinkBeacon(const char* mark) {
#if UPLINK_BEACON
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"boot\":%u,\"mark\":\"%s\",\"ms\":%lu}",
             (unsigned)rtc_bootCount, mark, (unsigned long)millis());
    mqtt.publish(TOPIC_DEBUG, buf, false);
    mqtt.loop();
#else
    (void)mark;
#endif
}

// ═════════════════════════════════════════════════════════════════════════════
// Telemetría
// ═════════════════════════════════════════════════════════════════════════════
void publishTelemetry() {
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

    // Cómo cerró el ciclo ANTERIOR (ver el bloque de rtc_prev* arriba). El
    // `pv_boot` es imprescindible: sin él no se puede saber a qué ciclo
    // corresponde la foto, y son justamente los ciclos que faltan los que
    // interesan. Se omite en el primer ciclo post-flasheo, donde la RTC memory
    // está en cero y los valores no significan nada.
    //
    // ~55 B extra sobre un payload de ~480 y un presupuesto de 741, así que no
    // se acerca al límite del buffer. Es temporal: sale cuando la pregunta esté
    // contestada.
    if (rtc_prevBoot != 0) {
        doc["pv_boot"] = rtc_prevBoot;
        doc["pv_st"]   = rtc_prevWifiSt;    // wl_status_t: 3 = WL_CONNECTED
        doc["pv_rssi"] = rtc_prevRssi;
        doc["pv_mq"]   = rtc_prevMqttSt;    // PubSubClient::state(): 0 = conectado
        doc["pv_ms"]   = rtc_prevAwakeMs;
    }

    // Sólo mientras hay una captura corriendo: costo cero en operación normal,
    // igual que el resto de los campos condicionales de arriba. Como capturar no
    // cuesta energía, no hay auto-expiración por tiempo — la higiene correcta es
    // que se vea, no un timer que apague la captura justo cuando servía.
    if (logging_isActive()) {
        doc["log_active"] = logging_level();
        doc["log_count"]  = logging_count();
    }

    char buf[MQTT_BUFFER_BYTES];
    size_t len = serializeJson(doc, buf);

    // Baliza inmediatamente antes del publish grande. Si esta llega y la
    // telemetría no, el enlace estaba vivo 10 ms antes y lo que no pasa es el
    // frame de 503 B — que es una conclusión muy distinta a que se caiga el
    // enlace. Ver UPLINK_BEACON en config.h.
    publishUplinkBeacon("pre_publish");

    if (!mqtt.publish(TOPIC_TELEMETRY, buf, false)) {
        // publish() devuelve false por dos motivos muy distintos: el payload no
        // entra en el buffer, o la escritura al socket falló. Distinguirlos acá,
        // que es donde se conoce el presupuesto, evita que el log culpe al buffer
        // de una conexión caída — que fue exactamente lo que pasó en la primera
        // captura de campo (2026-07-28): 505 B contra 741 disponibles, reportados
        // como "¿buffer corto?".
        const bool toobig = (int)len > MQTT_TELEMETRY_BUDGET;
        LOG_E("Publish de telemetría falló (%u B de %d útiles) — %s",
              (unsigned)len, MQTT_TELEMETRY_BUDGET,
              toobig ? "no entra en el buffer" : "conexión caída");
        logging_write(LOG_PUBLISH_FAIL, toobig ? 1 : 2, (int16_t)len);
    } else {
        LOG_V("Telemetría publicada (%u B)", (unsigned)len);
        logging_write(LOG_PUBLISH_OK, 0, (int16_t)len);
    }
    mqtt.loop();
}

// ═════════════════════════════════════════════════════════════════════════════
// Deep sleep
// ═════════════════════════════════════════════════════════════════════════════
void goToDeepSleep() {
    // ANTES de cualquier teardown: mqtt.disconnect() y WiFi.disconnect(true)
    // destruyen exactamente lo que queremos observar, así que medir después daría
    // WL_DISCONNECTED siempre y el dato no valdría nada. Este es además el único
    // punto por el que pasan TODOS los caminos, incluidos los que abortan sin
    // publicar (WiFi o MQTT fallidos), que son justo los que no se pueden ver
    // desde afuera.
    rtc_prevBoot    = rtc_bootCount;
    rtc_prevWifiSt  = (uint8_t)WiFi.status();
    rtc_prevRssi    = (int8_t)WiFi.RSSI();
    rtc_prevMqttSt  = (int8_t)mqtt.state();
    const uint32_t awake = millis();
    rtc_prevAwakeMs = (uint16_t)(awake > 65535 ? 65535 : awake);

    LOG_V("Entrando en deep sleep (%d seg)", SLEEP_INTERVAL_SEC);
    // Cierra el ciclo en el log: el tiempo despierto es la métrica que conecta
    // los fallos de conexión con el consumo (10 s de un ciclo sano contra los
    // 45 s de uno que agota los reintentos de WiFi).
    logging_write(LOG_SLEEP, 0, (int16_t)(millis() / 100));
    mqtt.disconnect();
    delay(200);
    WiFi.disconnect(true);
    delay(100);

    // Al final del teardown a propósito, y no entre el publish y el disconnect:
    // así el camino de red queda idéntico al de 1.3.1 y la medición del 42% de
    // payloads perdidos sigue siendo comparable contra ese baseline. De paso saca
    // el I2C del camino crítico — powerSave() hace read-modify-write sobre dos
    // chips y TwoWire tiene 50 ms de timeout por transacción, así que un bus
    // trabado (hay historial en este proyecto) metería hasta 200 ms justo ahí.
    sensors_sleepMonitors();

    esp_deep_sleep((uint64_t)SLEEP_INTERVAL_SEC * 1000000ULL);
}