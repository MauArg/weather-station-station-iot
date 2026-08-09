#pragma once
#include <stdint.h>
#include "config.h"
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
// nextSec: how long until the node intends to publish again — the sleep interval
// in the normal cycle, the live interval during a session. Published as `next_s`.
//
// The node is the only party that knows this. The backend used to hardcode 60 + 4
// s of measured wake overhead, which is fine until the gap stops being constant:
// live mode makes it a command parameter (2-60 s), and the power tiers would make
// it a function of pack voltage that no command declares at all. Reporting it
// costs ~12 B on a payload with ~220 B of headroom and removes the whole class of
// backend-guesses-the-node drift.
SensorData publishTelemetry(uint32_t liveSeq = 0, uint16_t nextSec = SLEEP_INTERVAL_SEC);
