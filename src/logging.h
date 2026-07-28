#pragma once
#include <Arduino.h>

// ═════════════════════════════════════════════════════════════════════════════
// Sistema de logs del nodo — ver ../logging_system_design.md
// ═════════════════════════════════════════════════════════════════════════════
//
// NO es LOG_LEVEL. Aquel es compile-time y sale por Serial; éste se activa en
// runtime por comando MQTT y se recupera a distancia. Va compilado en el build
// de producción a propósito: si quedara detrás de LOG_LEVEL>0 habría que
// flashear un build de debug para debuggear, que es justo lo que este sistema
// existe para evitar.
//
// El buffer vive en RTC memory porque el deep sleep borra la RAM normal. El
// ESP32-C3 tiene 8176 B de RTC memory en total y nada más (memory.ld), así que
// la ventana de captura se mide en horas, no en días.

// ─── Códigos ─────────────────────────────────────────────────────────────────
// Fuente única: este X-macro genera el enum, la tabla de niveles y las
// plantillas de texto, así que código y texto no pueden desincronizarse.
//
// El nodo es la autoridad del diccionario: el backend recibe (code, a, b) más
// la plantilla y sustituye, sin saber nada del dominio. Por eso reordenar o
// agregar códigos acá es seguro — el diccionario viaja junto a la versión de
// firmware, y el backend lo cachea por esa clave.
//
// Nivel: la entry se escribe si el nivel activo es >= al nivel del código.
//   1 = anomalías    2 = resumen por ciclo    3 = verboso
//
//                 nombre     nivel  plantilla
// `b` es int16_t (tiene que poder llevar RSSI negativo), así que las duraciones
// largas van en unidades de 100 ms: un ciclo que falla la conexión puede quemar
// 45 s y 45000 no entra en int16.
#define LOG_CODES(X) \
    X(LOG_BOOT,           2, "boot - reset=%b") \
    X(LOG_WIFI_TRY,       3, "wifi intento %a (canal cacheado %b, 0=scan)") \
    X(LOG_WIFI_OK,        2, "wifi ok - intento %a, rssi %b dBm") \
    X(LOG_WIFI_FAIL,      1, "wifi timeout - intento %a, status %b") \
    X(LOG_WIFI_GIVEUP,    1, "wifi agoto los reintentos - %b x100ms perdidos") \
    X(LOG_MQTT_OK,        2, "mqtt conectado en %b ms") \
    X(LOG_MQTT_FAIL,      1, "mqtt rechazado - state %b") \
    X(LOG_CMD_RX,         3, "comando retenido - tipo %a") \
    X(LOG_PUBLISH_OK,     2, "telemetria publicada - %b B") \
    X(LOG_PUBLISH_FAIL,   1, "publish fallo - %b B, buffer corto?") \
    X(LOG_SERVICE_ENTER,  2, "entrando a service mode - %b s de presupuesto") \
    X(LOG_SERVICE_EXIT,   2, "saliendo de service mode - motivo %a (1=timeout 2=servidor 3=mqtt caido), %b s") \
    X(LOG_SLEEP,          2, "durmiendo - %b x100ms despierto")

enum LogCode : uint8_t {
#define X(name, level, tmpl) name,
    LOG_CODES(X)
#undef X
    LOG_CODE_COUNT
};

// ─── Entry ───────────────────────────────────────────────────────────────────
// 8 bytes, sin punteros ni strings: escribirla es un memcpy. La captura es
// energéticamente gratis — el único costo real del sistema es el dump, que es
// operador-iniciado.
//
// No hay timestamp porque el nodo no tiene reloj y millis() se reinicia en cada
// ciclo. Se guarda boot_count + ms y el backend reconstruye la hora de pared
// anclándose en los ciclos que sí publicaron telemetría.
struct __attribute__((packed)) LogEntry {
    uint16_t boot;   // boot_count truncado a 16 bits — 45 días a 60 s
    uint16_t ms;     // ms desde el arranque del ciclo (satura en 65535)
    uint8_t  code;   // LogCode
    uint8_t  a;      // argumento chico: nro de intento, tipo de comando, motivo
    int16_t  b;      // argumento grande: RSSI, bytes, duración en ms
};

static_assert(sizeof(LogEntry) == 8, "LogEntry define el formato de cable — "
                                     "si cambia de tamaño, el backend no puede decodificar");

// ─── API ─────────────────────────────────────────────────────────────────────

// Registra un evento. Barata y segura de llamar siempre: si el logging está
// apagado o el código está por encima del nivel activo, retorna sin hacer nada.
void logging_write(uint8_t code, uint8_t a, int16_t b);

// Activa (level 1-3) o desactiva (level 0). Limpia el ring siempre: activar
// significa empezar una captura nueva, no continuar la anterior.
// `entries` sólo puede ACHICAR el ring — la RTC memory no se redimensiona en
// runtime. 0 usa la capacidad compilada entera.
void logging_configure(uint8_t level, uint16_t entries);

bool     logging_isActive();
uint8_t  logging_level();
uint16_t logging_count();      // entries vivas
uint32_t logging_dropped();    // perdidas por wraparound — distingue una
                               // captura completa de una truncada
uint16_t logging_pageCount();
void     logging_clear();

// Codifica una página en base64 dentro de `out` (queda null-terminated).
// Las entries salen en orden cronológico, no en orden físico del ring.
// Devuelve false si la página no existe o no entra en el buffer.
bool logging_encodePage(uint16_t page, char* out, size_t outSize, uint16_t* outEntries);

// Diccionario — el backend lo pide una vez por versión de firmware.
uint8_t     logging_codeCount();
const char* logging_codeName(uint8_t code);
const char* logging_codeTemplate(uint8_t code);
