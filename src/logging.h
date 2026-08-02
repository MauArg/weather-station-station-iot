#pragma once
#include <Arduino.h>

// ═════════════════════════════════════════════════════════════════════════════
// Node logging system — see ../logging_system_design.md
// ═════════════════════════════════════════════════════════════════════════════
//
// This is NOT LOG_LEVEL. That one is compile-time and goes out over Serial;
// this one is turned on at runtime via MQTT command and retrieved remotely.
// It's compiled into the production build on purpose: if it stayed behind
// LOG_LEVEL>0, debugging would require flashing a debug build, which is
// exactly what this system exists to avoid.
//
// The buffer lives in RTC memory because deep sleep wipes normal RAM. The
// ESP32-C3 has 8176 B of RTC memory total and nothing more (memory.ld), so
// the capture window is measured in hours, not days.
//
// Specifically it lives in `.rtc_noinit` and NOT in `.rtc.data`. The
// difference matters: per esp_attr.h, RTC_DATA_ATTR preserves the value
// "during a deep sleep / wake cycle" while RTC_NOINIT_ATTR preserves it
// "after restart or during a deep sleep / wake cycle". With RTC_DATA_ATTR, a
// panic, a watchdog, a brownout or a reboot command would wipe the entire
// capture and also leave the level at 0 — meaning the capture would disarm
// itself, silently, on exactly the events most worth investigating. Worse:
// the LOG_BOOT entry with esp_reset_reason(), which exists to distinguish a
// brownout from a panic, would never get written because the level was
// already 0 by the time setup() tried it.
//
// The cost of `.rtc_noinit` is that it starts out garbage on power-on, so
// the state is validated against a magic word and its geometry in logging_begin().

// ─── Codes ────────────────────────────────────────────────────────────────────
// Single source of truth: this X-macro generates the enum, the level table
// and the text templates, so code and text can never drift out of sync.
//
// The node is the authority on the dictionary: the backend receives
// (code, a, b) plus the template and substitutes, without knowing anything
// about the domain. That's why reordering or adding codes here is safe — the
// dictionary travels along with the firmware version, and the backend caches
// it by that key.
//
// Level: the entry is written if the active level is >= the code's level.
//   1 = anomalies    2 = per-cycle summary    3 = verbose
//
//                 name       level  template
// `b` is int16_t (it has to be able to carry negative RSSI), so long
// durations go in units of 100 ms: a cycle that fails to connect can burn
// 45 s, and 45000 doesn't fit in an int16.
#define LOG_CODES(X) \
    X(LOG_CAPTURE_START,  1, "capture started - level %a, capacity %b events") \
    X(LOG_BOOT,           2, "boot - reset=%b (8=deep sleep wake; any other value resets boot_count)") \
    X(LOG_WIFI_TRY,       3, "wifi attempt %a (cached channel %b, 0=scan)") \
    X(LOG_WIFI_OK,        2, "wifi ok - attempt %a, rssi %b dBm") \
    X(LOG_WIFI_FAIL,      1, "wifi timeout - attempt %a, status %b") \
    X(LOG_WIFI_GIVEUP,    1, "wifi exhausted retries - %b x100ms lost") \
    X(LOG_MQTT_OK,        2, "mqtt connected in %b ms") \
    X(LOG_MQTT_FAIL,      1, "mqtt rejected - state %b") \
    X(LOG_CMD_RX,         3, "retained command - type %a (1=maintenance 2=reboot 3=config 4=calibrate 5=ping 6=log_on)") \
    X(LOG_PUBLISH_OK,     2, "telemetry published - %b B") \
    X(LOG_PUBLISH_FAIL,   1, "publish failed - %b B, cause %a (1=doesn't fit in buffer, 2=connection dropped)") \
    X(LOG_SERVICE_ENTER,  2, "entering service mode - %b s budget") \
    X(LOG_SERVICE_EXIT,   2, "exiting service mode - reason %a (1=timeout 2=server 3=mqtt down), %b s") \
    X(LOG_SLEEP,          2, "sleeping - %b x100ms awake")

enum LogCode : uint8_t {
#define X(name, level, tmpl) name,
    LOG_CODES(X)
#undef X
    LOG_CODE_COUNT
};

// ─── Entry ────────────────────────────────────────────────────────────────────
// 8 bytes, no pointers or strings: writing one is a memcpy. Capturing is
// energetically free — the only real cost in the system is the dump, which
// is operator-initiated.
//
// There is no timestamp because the node has no clock and millis() resets on
// every cycle. boot_count + ms are stored, and the backend reconstructs
// wall-clock time by anchoring on the cycles that did publish telemetry.
struct __attribute__((packed)) LogEntry {
    uint16_t boot;   // boot_count truncated to 16 bits — 45 days at 60 s
    uint16_t ms;     // ms since the cycle started (saturates at 65535)
    uint8_t  code;   // LogCode
    uint8_t  a;      // small argument: attempt number, command type, reason
    int16_t  b;      // large argument: RSSI, bytes, duration in ms
};

static_assert(sizeof(LogEntry) == 8, "LogEntry defines the wire format — "
                                     "if its size changes, the backend cannot decode it");

// ─── API ─────────────────────────────────────────────────────────────────────

// Validates the state left in `.rtc_noinit` and resets it if it's not ours.
//
// MUST be called at the start of setup(), before the first logging_write():
// on power-on that memory starts out garbage, and a garbage `rtc_logHead`
// would write outside the ring. Idempotent and costs a few comparisons.
void logging_begin();

// Logs an event. Cheap and safe to call always: if logging is off or the
// code is above the active level, it returns without doing anything.
//
// NOT safe from an ISR: the head/count/dropped update is not atomic, so an
// interrupt partway through can leave the ring inconsistent. This matters
// because `config.h` has pending interrupt-based counting for the
// anemometer and rain gauge — if those handlers ever want to log, they need
// to queue and write from the main thread.
void logging_write(uint8_t code, uint8_t a, int16_t b);

// Turns it on (level 1-3) or off (level 0). Always clears the ring: turning
// it on means starting a new capture, not continuing the previous one.
// `entries` can only SHRINK the ring — RTC memory doesn't get resized at
// runtime. 0 uses the full compiled-in capacity.
void logging_configure(uint8_t level, uint16_t entries);

bool     logging_isActive();
uint8_t  logging_level();
uint16_t logging_count();      // live entries
uint32_t logging_dropped();    // lost to wraparound — distinguishes a
                               // complete capture from a truncated one
uint16_t logging_pageCount();
void     logging_clear();

// Encodes a page in base64 into `out` (ends up null-terminated).
// Entries come out in chronological order, not the ring's physical order.
// Returns false if the page doesn't exist or doesn't fit in the buffer.
bool logging_encodePage(uint16_t page, char* out, size_t outSize, uint16_t* outEntries);

// Dictionary — the backend requests it once per firmware version.
uint8_t     logging_codeCount();
const char* logging_codeName(uint8_t code);
const char* logging_codeTemplate(uint8_t code);
