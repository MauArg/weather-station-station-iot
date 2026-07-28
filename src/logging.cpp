#include "logging.h"
#include "config.h"
#include <mbedtls/base64.h>

// boot_count se define en main.cpp. Las entries lo usan como base temporal: es
// lo único monótono que sobrevive al deep sleep, ya que millis() se reinicia.
extern uint32_t rtc_bootCount;

// ─── Estado en RTC ────────────────────────────────────────────────────────────
// La sección .rtc.data se recarga desde flash en cualquier boot que NO sea wake
// de deep sleep, así que un reflash o un power-on dejan el logging apagado y el
// ring vacío. Es el comportamiento que queremos: nadie hereda una captura vieja.
static RTC_DATA_ATTR LogEntry rtc_logRing[LOG_RING_ENTRIES];
static RTC_DATA_ATTR uint16_t rtc_logHead     = 0;
static RTC_DATA_ATTR uint16_t rtc_logCount    = 0;
static RTC_DATA_ATTR uint16_t rtc_logCapacity = 0;
static RTC_DATA_ATTR uint32_t rtc_logDropped  = 0;
static RTC_DATA_ATTR uint8_t  rtc_logLevel    = 0;

// ─── Tablas generadas desde el X-macro ────────────────────────────────────────
// Las tres salen de la misma definición en logging.h, así que agregar un código
// no puede dejar una tabla desactualizada respecto de otra.
static const uint8_t LOG_CODE_LEVEL[] = {
#define X(name, level, tmpl) level,
    LOG_CODES(X)
#undef X
};

static const char* const LOG_CODE_NAME[] = {
#define X(name, level, tmpl) #name,
    LOG_CODES(X)
#undef X
};

static const char* const LOG_CODE_TEMPLATE[] = {
#define X(name, level, tmpl) tmpl,
    LOG_CODES(X)
#undef X
};

// ─── Escritura ────────────────────────────────────────────────────────────────

void logging_write(uint8_t code, uint8_t a, int16_t b) {
    if (rtc_logLevel == 0)                   return;
    if (rtc_logCapacity == 0)                return;  // defensivo: nunca configurado
    if (code >= LOG_CODE_COUNT)              return;
    if (LOG_CODE_LEVEL[code] > rtc_logLevel) return;

    uint32_t now = millis();

    LogEntry& e = rtc_logRing[rtc_logHead];
    e.boot = (uint16_t)rtc_bootCount;
    e.ms   = (now > 65535) ? 65535 : (uint16_t)now;
    e.code = code;
    e.a    = a;
    e.b    = b;

    rtc_logHead = (uint16_t)((rtc_logHead + 1) % rtc_logCapacity);

    // Contar lo pisado. Sin esto no se puede distinguir una captura completa de
    // una truncada, o sea no se sabe si la ventana llegó a cubrir el evento.
    if (rtc_logCount < rtc_logCapacity) rtc_logCount++;
    else                                rtc_logDropped++;
}

// ─── Configuración ────────────────────────────────────────────────────────────

void logging_configure(uint8_t level, uint16_t entries) {
    if (level > LOG_MAX_LEVEL) level = LOG_MAX_LEVEL;

    // `entries` sólo puede achicar: el array en RTC es de tamaño fijo, la
    // memoria no se redimensiona en runtime. 0 significa "usar todo".
    uint16_t cap = (entries == 0 || entries > LOG_RING_ENTRIES) ? LOG_RING_ENTRIES
                                                                : entries;

    rtc_logLevel    = level;
    rtc_logCapacity = cap;

    // Activar arranca una captura nueva. Conservar lo viejo mezclaría entries
    // de niveles distintos y, si la capacidad cambió, dejaría el ring en un
    // estado donde head y count ya no son consistentes.
    logging_clear();

    LOG_V("logging: nivel %u, capacidad %u entries", level, cap);
}

void logging_clear() {
    rtc_logHead    = 0;
    rtc_logCount   = 0;
    rtc_logDropped = 0;
}

// ─── Consultas ────────────────────────────────────────────────────────────────

bool     logging_isActive() { return rtc_logLevel > 0; }
uint8_t  logging_level()    { return rtc_logLevel; }
uint16_t logging_count()    { return rtc_logCount; }
uint32_t logging_dropped()  { return rtc_logDropped; }

uint16_t logging_pageCount() {
    return (uint16_t)((rtc_logCount + LOG_ENTRIES_PER_PAGE - 1) / LOG_ENTRIES_PER_PAGE);
}

// ─── Serialización ────────────────────────────────────────────────────────────

bool logging_encodePage(uint16_t page, char* out, size_t outSize, uint16_t* outEntries) {
    if (page >= logging_pageCount()) return false;

    const uint16_t first = (uint16_t)(page * LOG_ENTRIES_PER_PAGE);
    uint16_t n = (uint16_t)(rtc_logCount - first);
    if (n > LOG_ENTRIES_PER_PAGE) n = LOG_ENTRIES_PER_PAGE;

    // Orden cronológico, no orden físico: la entry lógica 0 es la más vieja
    // viva, que está `count` posiciones atrás de la cabeza. El backend recibe
    // las páginas ya ordenadas y no tiene que saber que esto es un ring.
    const uint16_t oldest =
        (uint16_t)((rtc_logHead + rtc_logCapacity - rtc_logCount) % rtc_logCapacity);

    static LogEntry pageBuf[LOG_ENTRIES_PER_PAGE];
    for (uint16_t i = 0; i < n; i++) {
        pageBuf[i] = rtc_logRing[(oldest + first + i) % rtc_logCapacity];
    }

    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char*)out, outSize, &olen,
                                   (const unsigned char*)pageBuf,
                                   (size_t)n * sizeof(LogEntry));
    if (rc != 0) {
        // Pide un buffer más grande del que le dimos. Con LOG_ENTRIES_PER_PAGE
        // bien dimensionado no debería pasar nunca; si pasa, la página se pierde
        // entera y es mejor saberlo que mandar basura.
        LOG_E("logging: base64 no entra (%u entries, buffer %u B, necesita %u)",
              n, (unsigned)outSize, (unsigned)olen);
        return false;
    }

    *outEntries = n;
    return true;
}

// ─── Diccionario ──────────────────────────────────────────────────────────────

uint8_t logging_codeCount() { return LOG_CODE_COUNT; }

const char* logging_codeName(uint8_t code) {
    return (code < LOG_CODE_COUNT) ? LOG_CODE_NAME[code] : "?";
}

const char* logging_codeTemplate(uint8_t code) {
    return (code < LOG_CODE_COUNT) ? LOG_CODE_TEMPLATE[code] : "?";
}
