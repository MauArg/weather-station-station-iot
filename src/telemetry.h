#pragma once
#include <stdint.h>
#include "sensors.h"

// Reads every sensor and publishes one telemetry payload on TOPIC_TELEMETRY.
//
// Lives in main.cpp, declared here so live_mode.cpp can reuse it instead of
// duplicating the JSON assembly. Two places building the same payload is how a
// field ends up in one and not the other.
//
// liveSeq: 0 in the normal cycle, which publishes the payload unchanged. Any
// other value adds the live-session fields — see the note in main.cpp about why
// a sequence number is needed at all (boot_count stops moving in live mode, and
// it is what the loss detector counts).
//
// Returns what it read, so a caller that also needs the values — live mode
// checks system_v and solar_v against its exit floors — does not have to run a
// second sensors_read() and pay the DS18B20 conversion twice.
SensorData publishTelemetry(uint32_t liveSeq = 0);
