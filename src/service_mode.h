#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include "command.h"

// ─── State persisted in RTC memory ───────────────────────────────────────────
// Survives deep sleep and software restarts.
// Only lost on a full power-off (battery failure, etc.).
extern RTC_DATA_ATTR bool     rtc_inServiceMode;
extern RTC_DATA_ATTR int      rtc_serviceTimeoutMin;

// Seconds of service mode already consumed, accumulated across restarts.
//
// Exists because the timeout bounded nothing: if MQTT dropped, the node
// would exit via deep sleep unable to clear the retained command, and on
// waking it would read it again and start a brand new session with the full
// timeout. With a link that drops often, that repeats indefinitely — the
// node stays awake at 50-140 mA in one-minute cycles with nothing cutting it
// off. By accumulating here, a restart picks up the remaining balance
// instead of getting a fresh budget.
//
// Only reset to zero when the session truly ends, i.e. when the retained
// command was successfully cleared and therefore no re-entry can happen.
extern RTC_DATA_ATTR uint32_t rtc_serviceElapsedSec;

// ─── Public API ───────────────────────────────────────────────────────────────

// Call on waking BEFORE making any decisions.
// Evaluates the received command + the RTC state to determine
// whether to enter/continue/exit service mode.
void serviceMode_evaluate(PubSubClient& mqtt, const Command& cmd);

// Returns true if the device is currently in service mode
// (may be from a previous cycle persisted in RTC).
bool serviceMode_isActive();

// Blocks until an OTA firmware is received, the timeout runs out,
// or the server clears the command. Calls goToDeepSleep() when done.
void serviceMode_run(PubSubClient& mqtt, int timeoutMin);

// Clears the RTC state and publishes an empty payload to the retained topic
// to clear the broker. sessionSec is how many seconds this session lasted,
// which get added to the RTC accumulator so the timeout stays absolute if
// the node enters again.
void serviceMode_exit(PubSubClient& mqtt, const char* reason, uint32_t sessionSec = 0);
