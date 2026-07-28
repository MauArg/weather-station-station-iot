#include "logging.h"
#include "config.h"
#include <mbedtls/base64.h>

// boot_count se define en main.cpp. Las entries lo usan como base temporal: es
// lo único monótono que sobrevive al deep sleep, ya que millis() se reinicia.
extern uint32_t rtc_bootCount;

// ─── Estado en RTC ────────────────────────────────────────────────────────────
// En `.rtc_noinit` y no en `.rtc.data`, para que la captura sobreviva a un panic,
// un watchdog, una brownout o un reboot — ver la nota larga en logging.h. Sin
// inicializadores a propósito: esta sección no se carga desde flash, así que un
// valor inicial acá sería una mentira. logging_begin() la valida.
static RTC_NOINIT_ATTR LogEntry rtc_logRing[LOG_RING_ENTRIES];
static RTC_NOINIT_ATTR uint16_t rtc_logHead;
static RTC_NOINIT_ATTR uint16_t rtc_logCount;
static RTC_NOINIT_ATTR uint16_t rtc_logCapacity;
static RTC_NOINIT_ATTR uint32_t rtc_logDropped;
static RTC_NOINIT_ATTR uint8_t  rtc_logLevel;

// Identidad del estado de arriba: mágica, geometría y huella del diccionario.
//
// - La mágica distingue "estado nuestro" de basura de un power-on.
// - La geometría atrapa un firmware futuro que cambie el tamaño del ring o de la
//   entry, donde lo persistido dejaría de ser interpretable.
// - La huella del diccionario atrapa el caso que las otras dos NO ven: como el
//   ring ahora sobrevive a un reflash, un firmware que agregue o reordene códigos
//   —el cambio futuro más natural de todos— heredaría entries viejas con la misma
//   geometría, y el backend las decodificaría con el diccionario nuevo. Saldrían
//   silenciosamente mal etiquetadas, que es peor que perderlas.
//
// OJO: nada de esto detecta un cambio de *layout* de este bloque. Si se agregan o
// reordenan variables acá, hay que bumpear LOG_STATE_MAGIC a mano.
static RTC_NOINIT_ATTR uint32_t rtc_logMagic;
static RTC_NOINIT_ATTR uint16_t rtc_logGeomEntries;
static RTC_NOINIT_ATTR uint8_t  rtc_logGeomEntryLen;
static RTC_NOINIT_ATTR uint32_t rtc_logDictHash;

static constexpr uint32_t LOG_STATE_MAGIC = 0x4C4F4731u;  // "LOG1"

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

// ─── Arranque ─────────────────────────────────────────────────────────────────

// FNV-1a sobre nombres, plantillas y niveles del diccionario. Recorrer ~800 bytes
// una vez por boot es despreciable, y es lo que hace que "estas entries se pueden
// leer con mi diccionario" sea una pregunta con respuesta.
static uint32_t _dictFingerprint() {
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < LOG_CODE_COUNT; i++) {
        for (const char* p = LOG_CODE_NAME[i]; *p; ++p) {
            h = (h ^ (uint8_t)*p) * 16777619u;
        }
        for (const char* p = LOG_CODE_TEMPLATE[i]; *p; ++p) {
            h = (h ^ (uint8_t)*p) * 16777619u;
        }
        h = (h ^ LOG_CODE_LEVEL[i]) * 16777619u;
        h = (h ^ 0xFFu) * 16777619u;   // separador: evita colisiones por concatenación
    }
    return h;
}

void logging_begin() {
    // Los chequeos de invariantes van más allá de la mágica a propósito. La mágica
    // sola ya distingue basura de power-on, pero cuestan tres comparaciones y
    // convierten cualquier corrupción sutil en un reinicio limpio en vez de en un
    // índice fuera de rango — que en RTC memory pisaría las variables de al lado.
    const uint32_t dictHash = _dictFingerprint();

    const bool sane =
        rtc_logMagic        == LOG_STATE_MAGIC &&
        rtc_logGeomEntries  == LOG_RING_ENTRIES &&
        rtc_logGeomEntryLen == sizeof(LogEntry) &&
        rtc_logDictHash     == dictHash &&
        rtc_logLevel        <= LOG_MAX_LEVEL &&
        rtc_logCapacity     <= LOG_RING_ENTRIES &&
        rtc_logCount        <= rtc_logCapacity &&
        (rtc_logCapacity == 0 ? rtc_logHead == 0 : rtc_logHead < rtc_logCapacity);

    if (sane) return;

    rtc_logMagic        = LOG_STATE_MAGIC;
    rtc_logGeomEntries  = LOG_RING_ENTRIES;
    rtc_logGeomEntryLen = sizeof(LogEntry);
    rtc_logDictHash     = dictHash;
    rtc_logLevel        = 0;
    rtc_logCapacity     = 0;
    rtc_logHead         = 0;
    rtc_logCount        = 0;
    rtc_logDropped      = 0;
    LOG_V("logging: estado en RTC reiniciado (arranque en frio, o firmware con otra geometria/diccionario)");
}

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

    // Primera entry de toda captura, para que el volcado se explique solo: sin
    // esto no hay forma de saber a qué nivel se capturó mirando el archivo, y el
    // LOG_CMD_RX del propio comando que la inició queda del lado borrado.
    if (level > 0) logging_write(LOG_CAPTURE_START, level, (int16_t)cap);

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
