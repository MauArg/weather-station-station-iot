#include "logging.h"
#include "config.h"
#include <mbedtls/base64.h>

// boot_count is defined in main.cpp. Entries use it as a time base: it's the
// only monotonic thing that survives deep sleep, since millis() resets.
extern uint32_t rtc_bootCount;

// ─── State in RTC ─────────────────────────────────────────────────────────────
// In `.rtc_noinit` and not in `.rtc.data`, so the capture survives a panic, a
// watchdog, a brownout or a reboot — see the long note in logging.h. No
// initializers on purpose: this section is not loaded from flash, so an
// initial value here would be a lie. logging_begin() validates it.
static RTC_NOINIT_ATTR LogEntry rtc_logRing[LOG_RING_ENTRIES];
static RTC_NOINIT_ATTR uint16_t rtc_logHead;
static RTC_NOINIT_ATTR uint16_t rtc_logCount;
static RTC_NOINIT_ATTR uint16_t rtc_logCapacity;
static RTC_NOINIT_ATTR uint32_t rtc_logDropped;
static RTC_NOINIT_ATTR uint8_t  rtc_logLevel;

// Identity for the state above: magic word, geometry and dictionary fingerprint.
//
// - The magic word distinguishes "our state" from power-on garbage.
// - The geometry catches a future firmware that changes the ring or entry
//   size, where the persisted data would stop being interpretable.
// - The dictionary fingerprint catches the case the other two do NOT see:
//   since the ring now survives a reflash, a firmware that adds or reorders
//   codes —the most natural future change of all— would inherit old entries
//   with the same geometry, and the backend would decode them with the new
//   dictionary. They would come out silently mislabeled, which is worse than
//   losing them.
//
// NOTE: none of this detects a *layout* change in this block. If variables
// are added or reordered here, LOG_STATE_MAGIC has to be bumped by hand.
static RTC_NOINIT_ATTR uint32_t rtc_logMagic;
static RTC_NOINIT_ATTR uint16_t rtc_logGeomEntries;
static RTC_NOINIT_ATTR uint8_t  rtc_logGeomEntryLen;
static RTC_NOINIT_ATTR uint32_t rtc_logDictHash;

static constexpr uint32_t LOG_STATE_MAGIC = 0x4C4F4731u;  // "LOG1"

// ─── Tables generated from the X-macro ────────────────────────────────────────
// All three come from the same definition in logging.h, so adding a code
// can never leave one table out of date relative to another.
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

// ─── Startup ──────────────────────────────────────────────────────────────────

// FNV-1a over the dictionary's names, templates and levels. Walking ~800
// bytes once per boot is negligible, and it's what makes "can these entries
// be read with my dictionary" a question with an answer.
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
        h = (h ^ 0xFFu) * 16777619u;   // separator: avoids collisions from concatenation
    }
    return h;
}

void logging_begin() {
    // The invariant checks go beyond the magic word on purpose. The magic
    // word alone already distinguishes power-on garbage, but they cost three
    // comparisons and turn any subtle corruption into a clean reset instead
    // of an out-of-range index — which in RTC memory would clobber the
    // neighboring variables.
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
    LOG_V("logging: RTC state reset (cold boot, or firmware with a different geometry/dictionary)");
}

// ─── Writing ──────────────────────────────────────────────────────────────────

void logging_write(uint8_t code, uint8_t a, int16_t b) {
    if (rtc_logLevel == 0)                   return;
    if (rtc_logCapacity == 0)                return;  // defensive: never configured
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

    // Count what got overwritten. Without this there's no way to tell a
    // complete capture from a truncated one, i.e. no way to know whether the
    // window managed to cover the event.
    if (rtc_logCount < rtc_logCapacity) rtc_logCount++;
    else                                rtc_logDropped++;
}

// ─── Configuration ────────────────────────────────────────────────────────────

void logging_configure(uint8_t level, uint16_t entries) {
    if (level > LOG_MAX_LEVEL) level = LOG_MAX_LEVEL;

    // `entries` can only shrink it: the array in RTC is fixed-size, the
    // memory doesn't get resized at runtime. 0 means "use it all".
    uint16_t cap = (entries == 0 || entries > LOG_RING_ENTRIES) ? LOG_RING_ENTRIES
                                                                : entries;

    rtc_logLevel    = level;
    rtc_logCapacity = cap;

    // Turning it on starts a new capture. Keeping the old data would mix
    // entries from different levels and, if the capacity changed, would
    // leave the ring in a state where head and count are no longer consistent.
    logging_clear();

    // First entry of every capture, so the dump explains itself: without
    // this there's no way to know what level it was captured at just by
    // looking at the file, and the LOG_CMD_RX for the very command that
    // started it ends up on the cleared side.
    if (level > 0) logging_write(LOG_CAPTURE_START, level, (int16_t)cap);

    LOG_V("logging: level %u, capacity %u entries", level, cap);
}

void logging_clear() {
    rtc_logHead    = 0;
    rtc_logCount   = 0;
    rtc_logDropped = 0;
}

// ─── Queries ──────────────────────────────────────────────────────────────────

bool     logging_isActive() { return rtc_logLevel > 0; }
uint8_t  logging_level()    { return rtc_logLevel; }
uint16_t logging_count()    { return rtc_logCount; }
uint32_t logging_dropped()  { return rtc_logDropped; }

uint16_t logging_pageCount() {
    return (uint16_t)((rtc_logCount + LOG_ENTRIES_PER_PAGE - 1) / LOG_ENTRIES_PER_PAGE);
}

// ─── Serialization ────────────────────────────────────────────────────────────

bool logging_encodePage(uint16_t page, char* out, size_t outSize, uint16_t* outEntries) {
    if (page >= logging_pageCount()) return false;

    const uint16_t first = (uint16_t)(page * LOG_ENTRIES_PER_PAGE);
    uint16_t n = (uint16_t)(rtc_logCount - first);
    if (n > LOG_ENTRIES_PER_PAGE) n = LOG_ENTRIES_PER_PAGE;

    // Chronological order, not physical order: logical entry 0 is the oldest
    // one still alive, which sits `count` positions behind the head. The
    // backend receives pages already in order and doesn't need to know this
    // is a ring.
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
        // Asks for a bigger buffer than we gave it. With LOG_ENTRIES_PER_PAGE
        // sized correctly this should never happen; if it does, the whole
        // page is lost, and it's better to know that than to send garbage.
        LOG_E("logging: base64 doesn't fit (%u entries, buffer %u B, needs %u)",
              n, (unsigned)outSize, (unsigned)olen);
        return false;
    }

    *outEntries = n;
    return true;
}

// ─── Dictionary ───────────────────────────────────────────────────────────────

uint8_t logging_codeCount() { return LOG_CODE_COUNT; }

const char* logging_codeName(uint8_t code) {
    return (code < LOG_CODE_COUNT) ? LOG_CODE_NAME[code] : "?";
}

const char* logging_codeTemplate(uint8_t code) {
    return (code < LOG_CODE_COUNT) ? LOG_CODE_TEMPLATE[code] : "?";
}
