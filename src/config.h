#pragma once

// ─── Firmware ─────────────────────────────────────────────────────────────────
// The real version is defined by -DFIRMWARE_VERSION in platformio.ini, per
// environment. This fallback is a sentinel, not a version: if it shows up in
// telemetry, it means the build came from an environment that forgot to
// define the flag. It used to say "1.0.0", which was a version that
// genuinely existed — a misconfigured build was indistinguishable from a
// legitimate 1.0.0 deploy.
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "0.0.0-nobuildflag"
#endif

// ─── Network ──────────────────────────────────────────────────────────────────
// Credentials: do NOT go here. They arrive via -D from `secrets.ini`, which
// is in .gitignore — see `secrets.ini.example` for the template and the two
// escaping warnings. This file is version-controlled and the repo lives on
// GitHub.
//
// The #error is deliberate, not paranoia: without it, a build missing
// secrets.ini would still compile with empty macros and flash a node that
// can't associate with anything. In a sealed enclosure out in the field,
// that gets fixed by opening the box and plugging in USB, because there
// would be no OTA either.
#if !defined(WIFI_SSID) || !defined(WIFI_PASSWORD) || !defined(MQTT_USER)  || !defined(MQTT_PASSWORD) || !defined(OTA_PASSWORD)
  #error "Missing credentials: copy secrets.ini.example to secrets.ini and fill it in."
#endif
// Timeout per association attempt. Lowered from 15 s to 5 s on 2026-07-31,
// with the missing data point: `STATUS.md` had a note to revisit this budget
// but asked to first "capture a window with worse signal", because the
// worst case of 3 × 15 s = 45 s had never been observed. A level-1 log
// capture over ~830 cycles showed it **twice** (boots 668 and 1074): three
// consecutive failed attempts and `WIFI_GIVEUP` at 45.9 s awake, against the
// 2.3 s of a healthy cycle.
//
// The imbalance is the same one the MQTT socket timeout had: a successful
// association takes **275 ms** (measured, dozens of samples with minimal
// variance) and the timeout was 15,000. And the codes it fails with are
// "this isn't going to work" states, not "it's taking a while" ones —
// `WL_NO_SSID_AVAIL` and `WL_DISCONNECTED`, not a handshake in progress.
//
// 5 s and not less because attempt 1 uses the cached BSSID and the ones
// after it **scan**: a scan of the 13 channels plus association can
// legitimately run 2-4 s. Cutting below that would cut off retries that do
// work — and they do: on boot 1057 attempt 1 failed and attempt 2 connected.
//
// The savings are modest and shouldn't be overstated: ~1.4 mAh/day out of an
// active budget of ~47. What it really prevents is a 45 s awake spike on a
// battery that today closes negative by mid-morning.
#define WIFI_TIMEOUT_MS     5000
#define WIFI_MAX_RETRIES    3

// ─── WiFi transmission rate ───────────────────────────────────────────────────
// Forces 802.11b mode (1 / 2 / 5.5 / 11 Mbps) instead of letting the rate
// control climb to OFDM (11g/n, up to 72 Mbps).
//
// The reason came from the sniffer, and it's direct evidence, not a theory.
// Over a 6844-frame capture with the sniffer next to the AP:
//
//   frames TOWARD the node captured ........ 4550
//   management frames FROM the node (at 1 Mbps) .. captured fine
//   DATA frames FROM the node (OFDM) ........ ZERO, not one in an hour
//
// Same distance, same antenna, same moment. The only thing that changes
// between the ones that decode and the ones that don't is **the modulation
// rate**. In other words, the node's high-rate transmissions are at the edge
// of what's demodulable, and the AP —which is a better receiver— gets them
// but pays 16-37% in retries.
//
// At 1-11 Mbps the required sensitivity is 5-10 dB lower than OFDM: exactly
// the margin that was missing. The cost is airtime (a 660 B frame takes
// 0.5 ms at 11 Mbps against 0.1 ms at 54), which on a node transmitting a
// handful of frames per minute shows up in neither energy nor channel use.
//
// Set to 0 to go back to the default behavior.
#define WIFI_FORCE_11B      1

// ─── WiFi power save ──────────────────────────────────────────────────────────
// The Arduino-ESP32 default is modem sleep (`WIFI_PS_MIN_MODEM`): the radio
// turns off between DTIM beacons and wakes up to receive.
//
// TESTED AND RULED OUT as the cause of telemetry loss (2026-07-29). Turned
// off, 33 cycles measured, and the loss rate stayed the same. Turned back on
// because it bought nothing and costs ~22 mAh/day out of an active budget of
// ~47.
//
// The verification matters as much as the result: it was very easy for
// `WiFi.setSleep(false)` to have no effect —the mode applies to the
// already-initialized driver and `WiFi.begin()` can overwrite it— so it was
// confirmed from outside the firmware before drawing conclusions. Two
// independent signals:
//
//   ICMP RTT: 42 ms median with power save  ->  7 ms without it (min 3)
//   system_v series in Grafana: with power save it comes out spiky, without
//     it it comes out as a continuous line. The INA219 samples once per
//     cycle, and with the radio going in and out of modem sleep the sample
//     sometimes lands with the radio asleep and sometimes awake; without
//     power save they all look alike.
//
// The real cause turned out to be something else, on a different layer: the
// link is asymmetric and the AP doesn't hear the node well (-74 dBm against
// the -62 the node thinks it has). See ../STATUS.md → "ROOT CAUSE".
//
// The `#define` is left in place instead of deleting the code: turning it
// off is the first thing worth trying again if the picture changes, and
// this way there's no need to re-derive how it was done or where the call goes.
#define WIFI_POWER_SAVE     1

// ─── MQTT ─────────────────────────────────────────────────────────────────────
#define MQTT_BROKER         "192.168.18.250"   // Raspberry Pi's IP
#define MQTT_PORT           1883
#define MQTT_CLIENT_ID      "weather-station-01"

// Topics
#define TOPIC_TELEMETRY     "station/01/telemetry"
#define TOPIC_STATUS        "station/01/status"
#define TOPIC_CMD           "station/01/cmd"         // retained, written by N8N

// Wait time to receive the retained message from the broker
#define MQTT_RETAINED_WAIT_MS  800

// ─── MQTT keepalive ───────────────────────────────────────────────────────────
// Two values, because the firmware's two paths have OPPOSITE needs and a
// single number can't serve both.
//
// The keepalive doesn't just govern the client's PINGREQ: it also defines
// how long the broker takes to declare a session dead, which per spec is
// 1.5 × keepalive. And with a fixed client ID, an old session still alive
// when the node reconnects forces a duplicate takeover on every cycle.
//
// NORMAL CYCLE — short. The node lives ~2.2 s, so its own PINGREQ never
// gets a chance to fire and the value is free on the client side. What 30 s
// buys is the broker expiring the session at 45 s, below the ~63 s cycle
// (57 s in the worst case of the deep sleep timer's ±5%): every wake finds
// the client ID free and there's no takeover. At 60 s the expiration fell
// to 90 s and the old session stayed alive across EVERY reconnection — it's
// the leading hypothesis for the 38-42% of lost payloads (see ../STATUS.md).
//
// SERVICE MODE — long. There the node lives for minutes and an
// ArduinoOTA.handle() can block for tens of seconds without sending
// anything, so the broker's margin is the only thing holding the session
// up; at 15 s a single lost PINGREQ would take it down, and that's what was
// cutting off and restarting sessions. There's no takeover to avoid on this
// path: the node doesn't sleep in between. serviceMode_run() reconnects to
// renegotiate it, because the value that governs the broker is the one that
// traveled in the CONNECT.
#define MQTT_KEEPALIVE_NORMAL_SEC    30
#define MQTT_KEEPALIVE_SERVICE_SEC   60

// PubSubClient buffer size, and the usable payload left for the telemetry
// topic once the fixed header (5), the length (2) and the topic are
// subtracted. Deriving it instead of hardcoding 741 keeps it from drifting
// out of sync if the buffer or the topic name changes — and `publish()`
// silently drops the entire message when it doesn't fit.
#define MQTT_BUFFER_BYTES      768
#define MQTT_TELEMETRY_BUDGET  (MQTT_BUFFER_BYTES - 5 - 2 - (int)(sizeof(TOPIC_TELEMETRY) - 1))

// ─── I2C ──────────────────────────────────────────────────────────────────────
#define I2C_SDA             6
#define I2C_SCL             5

// ─── Sensors ──────────────────────────────────────────────────────────────────
#define ALTITUDE_M          780.0f    // Site altitude in meters ASL
#define INA219_SOLAR_ADDR   0x41      // Solar panel INA219
#define INA219_SYSTEM_ADDR  0x40      // ESP32 consumption INA219

// ─── Deep sleep ───────────────────────────────────────────────────────────────
#define SLEEP_INTERVAL_SEC  60   // 300s = 5 minutes (normal mode)

// ─── OTA / Service mode ───────────────────────────────────────────────────────
#define OTA_HOSTNAME        "weather-station-01"

#define SERVICE_MODE_DEFAULT_TIMEOUT_MIN  15
#define SERVICE_MODE_MAX_TIMEOUT_MIN      60          // absolute ceiling, regardless of what the server requests
#define SERVICE_MODE_HEARTBEAT_SEC        30

// MQTT reconnection during service mode. Before, a broker drop would abort
// the entire session: the node would sleep unable to clear the retained
// command and start from zero on waking.
//
// The worst case isn't the 10s that 5×2s suggests: each attempt can also
// consume the socket timeout waiting for the CONNACK. With
// setSocketTimeout(5) in connectMQTT() it comes out to 5×(5+2) ≈ 35s. This
// matters because ArduinoOTA.handle() doesn't get called during that block,
// so if the link drops right when you start a flash, esptool may give up —
// it just retries, but it's worth knowing that before assuming the node hung.
#define SERVICE_MODE_MQTT_RETRIES         5
#define SERVICE_MODE_MQTT_RETRY_DELAY_MS  2000

// ─── Logging ──────────────────────────────────────────────────────────────────
// LOG_LEVEL: 0=off, 1=error, 2=verbose
#ifndef LOG_LEVEL
  #define LOG_LEVEL 0
#endif

#if LOG_LEVEL >= 2
  #define LOG_V(fmt, ...) Serial.printf("[V] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_V(fmt, ...) do {} while(0)
#endif

#if LOG_LEVEL >= 1
  #define LOG_E(fmt, ...) Serial.printf("[E] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG_E(fmt, ...) do {} while(0)
#endif

// ─── Runtime logging system (via MQTT) ────────────────────────────────────────
// Not to be confused with LOG_LEVEL above: that one is compile-time and goes
// out over Serial. This one is turned on via command and retrieved remotely.
// See logging_system_design.md.
//
// The ring lives in RTC memory, which on the ESP32-C3 is 8176 B total
// (memory.ld → rtc_iram_seg, 0x2000 - 0x10) shared with the rest of the RTC
// variables and whatever the system reserves. 768 × 8 B = 6144 B leaves margin.
// If it's exceeded, the linker fails with "region rtc_iram_seg overflowed"
// — the error shows up at compile time, not in the field.
#define LOG_RING_ENTRIES      768
#define LOG_MAX_LEVEL         3

// Entries per dump page.
//
// Real budget: the MQTT buffer is 768 B, minus header (5), length (2) and
// the `station/01/log/data` topic (19) → 742 B of usable payload. The JSON
// wrapper in its worst case —`dropped` is uint32 and can reach 10 digits—
// measures 77 B:
//   {"page":12,"pages":13,"count":768,"dropped":4294967295,"entries":55,"b64":""}
// That leaves 665 B for the base64, which at 3/4 is 498 B binary = 62 entries.
//
// 55 is chosen for margin: 55 × 8 B = 440 B → 588 B of base64 → 665 B of
// payload, 77 B under the limit. At 60 the margin dropped to 25 B, and
// going over doesn't fail loudly from the start: `serializeJson` silently
// truncates and only afterward does `publish()` reject the message, leaving
// that page impossible to recover no matter how many times the backend
// retries it.
#define LOG_ENTRIES_PER_PAGE  55

// Dump topics. No retain on purpose: `cmd` is retained and single-slot, and
// a back-and-forth of N pages there would fight against that semantics on
// top of blocking the slot for any other command.
#define TOPIC_LOG_REQ       "station/01/log/req"
#define TOPIC_LOG_DATA      "station/01/log/data"

// ─── Network (static IP) ─────────────────────────────────────────────────────
#define WIFI_STATIC_IP      IPAddress(192, 168, 18, 105)
#define WIFI_GATEWAY        IPAddress(192, 168, 18, 1)
#define WIFI_SUBNET         IPAddress(255, 255, 255, 0)
#define WIFI_DNS            IPAddress(8, 8, 8, 8)

// ─── Rail control (NPN BC337 transistors on main PCB) ─────────────────────────
// GPIO HIGH → NPN conducts → GND of the connected rail → sensors powered
#define PIN_RAIL_A          7   // Rail A: outdoor sensors (SHT31 + BMP085)
#define PIN_RAIL_B          8   // Rail B: enclosure sensors (DHT22, photo, rain)
// TODO [low power]: turn off Rail B at Tier 2 and Rail A at Tier 3
//   based on battery threshold — implement in battery.h

// ─── New sensor pins ──────────────────────────────────────────────────────────
#define PIN_DS18B20         10  // OneWire outdoor temperature (always-on)
#define PIN_DHT22            0  // DHT22 DATA (Rail B) — same pin as the former DHT11
#define PIN_PHOTORESISTOR    3  // Photoresistor ADC (Rail B)
#define PIN_RAIN_SENSOR      4  // Rain sensor AO ADC (Rail B)
#define PIN_ANEMOMETER       2  // FALLING pulse — anemometer (always-on)
#define PIN_RAIN_GAUGE       1  // FALLING pulse — rain gauge (always-on)
// TODO [pulses]: implement accumulated counting (RTC memory) for the
//   anemometer and rain gauge between deep sleep cycles

// ─── Analog sensor calibration ────────────────────────────────────────────────
// Photoresistor: divider 3V3 → R10kΩ → signal → photo → GND
//   R_photo = R_pullup * V / (3.3 - V)
#define PHOTO_PULLUP_KOHM   10.0f

// Rain sensor: divider 3V3 → R1‖R2(4.95kΩ) → signal → sensor → GND
//   R_rain = R_pullup * V / (3.3 - V)
#define RAIN_PULLUP_KOHM    4.95f

// ADC
#define ADC_VREF            3.3f
#define ADC_MAX_RAW      4095.0f

// ─── DHT22 (Rail B) ───────────────────────────────────────────────────────────
// No humidity calibration constants: the ones that used to be here
// (DHT_HUM_RAW/REAL_*) corrected the bias of the faulty DHT11. The DHT22
// comes factory-calibrated (±2% RH, ±0.5 °C) — any correction here would
// reintroduce bias into the reading.
//
// Warmup after powering Rail B. The AM2302 datasheet asks for ≥1s of
// "unstable status"; 2s is a conservative margin. Measured from the
// rail-on, not from boot — see sensors_railsOn().
//
// NOTE — since 1.5.0 this constant is what sets the floor on the awake
// time. With the rail-on moved to the start of the cycle, awake time is
//   max(network path, DHT_WARMUP_MS) + sensor reading (~240 ms)
// and the network path measures ~1270 ms (275 WiFi + 42 MQTT + 800 retained
// + ~150 init), meaning this is the term that dominates. The delay left
// over is ~730 ms of pure waiting.
//
// Counterintuitive consequence: **shortening MQTT_RETAINED_WAIT_MS no
// longer saves anything.** Lowering it from 800 to 200 ms just grows this
// warmup from 730 to 1330 ms and total awake time stays the same — the
// pending item in ../STATUS.md noting "the 800 ms retained wait is 24% of
// awake time" stopped applying once it ended up behind this barrier.
//
// The lever that does still work is this same number: 2000 ms is 2× the
// datasheet minimum. Lowering it to ~1200-1500 ms would cut awake time
// almost 1:1, but it needs to be validated against real DHT22 readings
// before touching it — a short warmup was already suspected of causing
// erratic readings once (see ../STATUS.md, the 2026-07-25 bug where the
// warmup never actually ran).
#define DHT_WARMUP_MS           2000

// Minimum DHT22 sampling period (datasheet: ≥2s between readings).
// Retry spacing for when the first frame comes out corrupted.
#define DHT_RETRY_INTERVAL_MS   2000