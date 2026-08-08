#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include "command.h"

// ─── Live mode ────────────────────────────────────────────────────────────────
// The node stops sleeping and publishes every LIVE_*_INTERVAL_SEC seconds while
// the panel is producing surplus. See config.h for the thresholds and why they
// are where they are.
//
// # Why there is no resume-after-restart path
//
// Service mode has one, because an OTA restarts the node mid-session and the
// session has to survive that. Live mode deliberately does not: if the node
// restarts, it comes up in the normal cycle, reads the still-retained `live`
// command and enters again through the front door — which re-runs the entry
// checks. Resuming blind would skip them, and a restart is exactly the moment
// you least want to skip a check.
//
// What does survive a restart is the budget accumulator below, so re-entering
// cannot hand out a fresh timeout every time. That is the service-mode lesson:
// a timeout that restarts with the session bounds nothing.

// Seconds of live mode already consumed, accumulated across re-entries.
// Only cleared when the retained command is successfully removed, i.e. when no
// re-entry is possible.
extern RTC_DATA_ATTR uint32_t rtc_liveElapsedSec;

// Runs the live session until a floor trips, the budget runs out, the server
// clears the retained command, or MQTT is lost beyond recovery. Never returns:
// every exit path goes through liveMode_exit(), which deep-sleeps.
//
// force: skip the panel-voltage floor so the mode can be exercised at night or
// on the bench. It skips that floor and nothing else — the battery floor, the
// budget and the MQTT floor all still apply, and parseCommand() caps a forced
// session at LIVE_FORCED_MAX_TIMEOUT_MIN because removing the sun floor also
// removes what would normally end a session that should not be running.
void liveMode_run(PubSubClient& mqtt, int timeoutMin, uint16_t intervalSec, bool force);

// Ends the session: reports the reason, adds the elapsed time to the RTC
// accumulator, tries to clear the retained command, and deep-sleeps.
void liveMode_exit(PubSubClient& mqtt, const char* reason, uint32_t sessionSec);
