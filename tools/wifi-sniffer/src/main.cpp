// wifi-sniffer — captures 802.11 in promiscuous mode to diagnose why the
// node's link dies mid-cycle.
//
// The problem, measured from three independent angles (the broker's $SYS
// counters, ICMP probing, and the firmware's own pv_* fields): the node
// associates fine, works for a variable stretch, and then stops being on
// the network in BOTH directions, while its driver keeps reporting
// WL_CONNECTED with a normal RSSI. Nobody tells it anything. See ../../../STATUS.md.
//
// What's missing is in the air, and nothing needs to be decrypted: the
// frames that answer the question travel in the clear. The Retry bit lives
// in the MAC header, and deauth/disassoc are unencrypted management frames
// that also carry their reason code. Three mutually exclusive branches:
//
//   the node keeps transmitting, with Retry=1 and no ACK from the AP
//        -> the AP silently dropped it and stopped answering
//   the node stops transmitting
//        -> its driver hung, and the problem is on the node's side
//   a deauth/disassoc shows up
//        -> it tells us who sent it and why (reason code)
//
// Limitations worth keeping in mind while reading the output:
//   - It listens to ONE channel at a time. Not a problem here since the
//     channel is fixed.
//   - It's just another receiver: it only sees what reaches ITS antenna.
//     Best placed near the AP, which is the viewpoint that matters.
//   - ACKs are very short frames that go out immediately after the data;
//     the ESP32's promiscuous mode doesn't always deliver them. A missing
//     ACK in the capture does NOT prove the AP failed to answer — but if
//     ACKs show up for some frames and not others, THAT is a signal.
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"

// ─── What to listen for ───────────────────────────────────────────────────────
// The node's MAC (Espressif) and the BSSID of the IoT SSID it associates on.
static const uint8_t NODE_MAC[6] = {0x80, 0xF1, 0xB2, 0x6D, 0xF9, 0xFC};
static const uint8_t AP_BSSID[6] = {0xBE, 0xF1, 0x7E, 0xE9, 0xF7, 0x2F};

#ifndef SNIFF_CHANNEL
  #define SNIFF_CHANNEL 1
#endif

// Board's RGB LED. On the ESP32-S3-DevKitC-1 **v1.0** it's GPIO48; on v1.1
// it moved to GPIO38. If the LED doesn't respond, that's the first suspect.
#define STR_(x) #x
#define STR(x) STR_(x)

#ifndef SNIFF_RGB_PIN
  #define SNIFF_RGB_PIN 48
#endif
#ifndef SNIFF_LED
  #define SNIFF_LED 1
#endif

// Pcap mode: instead of readable output, emits every ENTIRE frame in base64
// to reassemble on the PC side and open with Wireshark. This is what makes
// it possible to go from "I see the headers" to "I see the packets",
// because Wireshark decrypts the body with the PSK — and this case is ideal
// for that: the node redoes the 4-way handshake on EVERY cycle, which is
// exactly what Wireshark needs to derive the PTK. Without that, the exact
// moment a client associates would have to be captured.
//
// The "baud rate" doesn't limit anything here: over the native USB the
// output runs at USB speed, not 115200. That's why the whole frame can be
// dumped without a second thought.
#ifndef SNIFF_PCAP
  #define SNIFF_PCAP 0
#endif
// Per-frame cap. A telemetry publish is ~503 B of MQTT plus the
// 802.11/LLC/IP/TCP headers, so 600 fits the whole thing. Anything past
// that gets truncated and Wireshark flags it — nothing breaks, but that
// frame couldn't be decrypted because CCMP's MIC is computed over the whole body.
#define PCAP_SNAPLEN 600

// Silence that closes a burst. The node lives ~2.3 s and sleeps ~60 s, so
// 5 s unambiguously separates one cycle from the next.
static const uint32_t BURST_GAP_MS = 5000;

// ─── 802.11 header ────────────────────────────────────────────────────────────
// Note: an ACK is 10 bytes and only carries addr1. Reading addr2 in that
// case would read past the frame, which is why every access is guarded by
// the real length.
typedef struct {
    uint16_t frame_ctrl;
    uint16_t duration;
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
} __attribute__((packed)) wifi_hdr_t;

struct rec_t {
    uint32_t t_ms;
    int8_t   rssi;
    uint8_t  type;
    uint8_t  subtype;
    bool     retry;
    bool     prot;
    uint8_t  a1[6], a2[6], a3[6];
    uint8_t  n_addr;      // how many addresses are valid in this frame
    uint16_t reason;      // deauth/disassoc only
    uint16_t len;
#if SNIFF_PCAP
    uint32_t us;          // fine-grained timestamp, for the pcap
    uint16_t caplen;
    uint8_t  raw[PCAP_SNAPLEN];
#endif
};

static QueueHandle_t q;

// ─── Per-burst statistics (one node cycle) ───────────────────────────────────
static uint32_t burst_first_ms = 0, burst_last_ms = 0;
static uint32_t n_from_node = 0, n_to_node = 0, n_retry = 0, n_ack_to_node = 0;
static uint32_t n_deauth = 0, n_assoc = 0, n_auth = 0, n_null = 0;
static bool     burst_open = false;
static uint32_t burst_num = 0;

static inline bool mac_eq(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

// ─── RGB LED ──────────────────────────────────────────────────────────────────
// Color encodes the frame type, so it can be diagnosed at a glance without
// reading the serial: a healthy burst is violet (association) and then
// green and cyan alternating. Too much amber means the link is struggling,
// and red is the frame we're hunting for.
//
// Brightness is deliberately kept low: a WS2812 at full brightness is
// blinding and makes the colors indistinguishable. The flash is painted
// when the frame arrives and fades with an exponential decay — without
// that, frames microseconds apart would be one continuous blur.
#if SNIFF_LED
static uint8_t  led_r = 0, led_g = 0, led_b = 0;
static uint32_t led_latch_until = 0;

static void led_flash(uint8_t r, uint8_t g, uint8_t b, uint32_t hold_ms = 0) {
    // A latch (the deauth) isn't allowed to be overwritten by whatever
    // traffic comes after: it's exactly the frame we don't want to go unnoticed.
    if (hold_ms == 0 && millis() < led_latch_until) return;
    led_r = r; led_g = g; led_b = b;
    if (hold_ms) led_latch_until = millis() + hold_ms;
    neopixelWrite(SNIFF_RGB_PIN, led_r, led_g, led_b);
}

static void led_tick() {
    static uint32_t last = 0;
    const uint32_t now = millis();
    if (now - last < 12) return;
    last = now;
    if (now < led_latch_until) return;

    if (led_r || led_g || led_b) {
        led_r = (uint8_t)(led_r * 78 / 100);
        led_g = (uint8_t)(led_g * 78 / 100);
        led_b = (uint8_t)(led_b * 78 / 100);
        neopixelWrite(SNIFF_RGB_PIN, led_r, led_g, led_b);
    } else {
        // Very faint blue breathing during the silence between cycles:
        // confirms it's still alive —that's ~60 s of nothing— and makes the
        // burst stand out by contrast.
        const uint32_t phase = (now / 6) % 512;
        const uint8_t  v = (uint8_t)((phase < 256 ? phase : 511 - phase) / 64);
        neopixelWrite(SNIFF_RGB_PIN, 0, 0, v);
    }
}

// Palette, chosen for what each frame means in this investigation.
static void led_for_frame(uint8_t type, uint8_t subtype, bool retry, bool from_node) {
    if (type == 0 && (subtype == 12 || subtype == 10)) {
        led_flash(160, 0, 0, 1500);            // DEAUTH/disassoc — red, latched
    } else if (type == 0 && (subtype == 11 || subtype <= 3)) {
        led_flash(55, 0, 60);                  // auth / assoc — violet
    } else if (type == 0) {
        led_flash(0, 0, 30);                   // other management — faint blue
    } else if (type == 1) {
        led_flash(22, 22, 22);                 // control (ACK, RTS/CTS) — white
    } else if (retry) {
        led_flash(70, 28, 0);                  // retry — amber
    } else if (from_node) {
        led_flash(0, 55, 0);                   // data from the node — green
    } else {
        led_flash(0, 32, 55);                  // data toward the node — cyan
    }
}
#else
static inline void led_tick() {}
static inline void led_for_frame(uint8_t, uint8_t, bool, bool) {}
#endif

#if SNIFF_PCAP
// Hand-rolled base64, against a static buffer. The core's library returns a
// String and this runs once per frame inside a burst: heap fragmentation
// and allocations on the hot path are not wanted.
static const char B64C[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char b64buf[(PCAP_SNAPLEN + 2) / 3 * 4 + 1];

static const char* b64_encode(const uint8_t* d, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= (uint32_t)d[i + 2];
        b64buf[o++] = B64C[(v >> 18) & 63];
        b64buf[o++] = B64C[(v >> 12) & 63];
        b64buf[o++] = (i + 1 < n) ? B64C[(v >> 6) & 63] : '=';
        b64buf[o++] = (i + 2 < n) ? B64C[v & 63] : '=';
    }
    b64buf[o] = 0;
    return b64buf;
}
#endif

static const char* mgmt_name(uint8_t s) {
    switch (s) {
        case 0:  return "assoc-req";
        case 1:  return "assoc-resp";
        case 2:  return "reassoc-req";
        case 3:  return "reassoc-resp";
        case 4:  return "probe-req";
        case 5:  return "probe-resp";
        case 8:  return "beacon";
        case 10: return "disassoc";
        case 11: return "auth";
        case 12: return "DEAUTH";
        case 13: return "action";
        default: return "mgmt?";
    }
}

static const char* ctrl_name(uint8_t s) {
    switch (s) {
        case 8:  return "block-ack-req";
        case 9:  return "block-ack";
        case 10: return "ps-poll";
        case 11: return "rts";
        case 12: return "cts";
        case 13: return "ack";
        case 14: return "cf-end";
        default: return "ctrl?";
    }
}

static const char* data_name(uint8_t s) {
    switch (s) {
        case 0:  return "data";
        case 4:  return "null";       // power save's "I'm here" frame
        case 8:  return "qos-data";
        case 12: return "qos-null";
        default: return "data?";
    }
}

// ─── Promiscuous callback ─────────────────────────────────────────────────────
// Runs in the WiFi task: only filtering and queueing happen here. Printing
// from inside would drop frames, which is exactly what we don't want.
static void sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 10) return;

    const wifi_hdr_t* h = (const wifi_hdr_t*)pkt->payload;
    const uint16_t fc = h->frame_ctrl;
    const uint8_t  ftype = (fc >> 2) & 0x3;
    const uint8_t  fsub  = (fc >> 4) & 0xF;

    // Beacons and probe-resp from the whole neighborhood: they're most of
    // the airtime and contribute nothing here.
    if (ftype == 0 && (fsub == 8 || fsub == 5)) return;

    // How many addresses this frame actually carries.
    uint8_t n_addr = 0;
    if (len >= 10) n_addr = 1;
    if (len >= 16) n_addr = 2;
    if (len >= 24) n_addr = 3;

    // We care about anything that touches the node or its AP.
    bool relevant = false;
    if (n_addr >= 1 && (mac_eq(h->addr1, NODE_MAC) || mac_eq(h->addr1, AP_BSSID))) relevant = true;
    if (n_addr >= 2 && (mac_eq(h->addr2, NODE_MAC) || mac_eq(h->addr2, AP_BSSID))) relevant = true;
    if (n_addr >= 3 && (mac_eq(h->addr3, NODE_MAC) || mac_eq(h->addr3, AP_BSSID))) relevant = true;
    // A deauth broadcast matters too, even if it doesn't name the node.
    if (ftype == 0 && (fsub == 12 || fsub == 10)) relevant = true;
    if (!relevant) return;

    rec_t r = {};
    r.t_ms    = millis();
    r.rssi    = pkt->rx_ctrl.rssi;
    r.type    = ftype;
    r.subtype = fsub;
    r.retry   = (fc >> 11) & 0x1;
    r.prot    = (fc >> 14) & 0x1;
    r.n_addr  = n_addr;
    r.len     = len;
    if (n_addr >= 1) memcpy(r.a1, h->addr1, 6);
    if (n_addr >= 2) memcpy(r.a2, h->addr2, 6);
    if (n_addr >= 3) memcpy(r.a3, h->addr3, 6);
    // A deauth/disassoc's reason code is the first 2 bytes of the body.
    if (ftype == 0 && (fsub == 12 || fsub == 10) && len >= 26) {
        r.reason = pkt->payload[24] | (pkt->payload[25] << 8);
    }

    // xQueueSend, not xQueueSendFromISR: this is NOT an ISR, it runs in the
    // WiFi task. The FromISR variant called from task context can trigger a
    // FreeRTOS assert. Timeout 0 so the driver is never blocked: if the
    // queue fills up, dropping a frame is preferred over stalling the radio.
#if SNIFF_PCAP
    r.us     = (uint32_t)esp_timer_get_time();
    r.caplen = (len > PCAP_SNAPLEN) ? PCAP_SNAPLEN : len;
    memcpy(r.raw, pkt->payload, r.caplen);
#endif

    xQueueSend(q, &r, 0);
}

static void fmt_mac(char* out, const uint8_t* m) {
    sprintf(out, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void close_burst() {
    if (!burst_open) return;
    Serial.printf(
        "\n──── end of burst #%u ── lasted %u ms ────\n"
        "     from node: %u   to node: %u   ACK to node: %u   with Retry: %u\n"
        "     auth: %u   assoc: %u   null/qos-null: %u   DEAUTH/disassoc: %u\n\n",
        burst_num, burst_last_ms - burst_first_ms,
        n_from_node, n_to_node, n_ack_to_node, n_retry,
        n_auth, n_assoc, n_null, n_deauth);
    burst_open = false;
    n_from_node = n_to_node = n_retry = n_ack_to_node = 0;
    n_deauth = n_assoc = n_auth = n_null = 0;
}

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n\n=== wifi-sniffer ===");

#if SNIFF_PCAP
    // 48 records of ~600 B is ~29 KB. Plenty: the loop drains them over USB
    // at megabyte speed and a whole burst is ~100 frames.
    q = xQueueCreate(48, sizeof(rec_t));
#else
    q = xQueueCreate(512, sizeof(rec_t));
#endif

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    // Management + data + control. ACKs are control, and they're what
    // tells us whether the AP is answering the node.
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA |
                       WIFI_PROMIS_FILTER_MASK_CTRL;
    esp_wifi_set_promiscuous_filter(&filt);

    wifi_promiscuous_filter_t cfilt = {};
    cfilt.filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL;
    esp_wifi_set_promiscuous_ctrl_filter(&cfilt);

    esp_wifi_set_promiscuous_rx_cb(&sniffer_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(SNIFF_CHANNEL, WIFI_SECOND_CHAN_NONE);

    char n[13], a[13];
    fmt_mac(n, NODE_MAC);
    fmt_mac(a, AP_BSSID);
    Serial.printf("channel %d   node %s   AP %s\n", SNIFF_CHANNEL, n, a);
    Serial.println("beacons and probe-resp filtered out; the rest gets printed");
#if SNIFF_LED
    Serial.println(
        "\nLED (GPIO" STR(SNIFF_RGB_PIN) "):  violet=auth/assoc  green=data from node\n"
        "                cyan=data toward node  amber=RETRY  white=ACK/control\n"
        "                SOLID RED=deauth/disassoc  breathing blue=silence\n");
#endif
    Serial.println();
}

void loop() {
    rec_t r;
    // Short timeout on purpose: at 200 ms the LED decay and breathing came
    // out jerky. Here the loop breathes every 10 ms and the animation stays
    // smooth for free — there's nothing else to do anyway.
    while (xQueueReceive(q, &r, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (!burst_open) {
            burst_open = true;
            burst_num++;
            burst_first_ms = r.t_ms;
            Serial.printf("──── burst #%u ── t=%u ms ────\n", burst_num, r.t_ms);
        }
        burst_last_ms = r.t_ms;

        const char* name;
        if      (r.type == 0) name = mgmt_name(r.subtype);
        else if (r.type == 1) name = ctrl_name(r.subtype);
        else                  name = data_name(r.subtype);

        const bool from_node = (r.n_addr >= 2 && mac_eq(r.a2, NODE_MAC));
        const bool to_node   = (r.n_addr >= 1 && mac_eq(r.a1, NODE_MAC));
        if (from_node) { n_from_node++; if (r.retry) n_retry++; }
        if (to_node)   { n_to_node++; }
        if (to_node && r.type == 1 && r.subtype == 13) n_ack_to_node++;
        if (r.type == 0 && (r.subtype == 12 || r.subtype == 10)) n_deauth++;
        if (r.type == 0 && r.subtype == 11) n_auth++;
        if (r.type == 0 && (r.subtype == 0 || r.subtype == 1)) n_assoc++;
        if (r.type == 2 && (r.subtype == 4 || r.subtype == 12)) n_null++;

        char s1[13] = "-", s2[13] = "-", s3[13] = "-";
        if (r.n_addr >= 1) fmt_mac(s1, r.a1);
        if (r.n_addr >= 2) fmt_mac(s2, r.a2);
        if (r.n_addr >= 3) fmt_mac(s3, r.a3);

#if SNIFF_PCAP
        // One line per frame: microseconds, RSSI, ORIGINAL length (so the
        // pcap records the truncation if there was one) and the raw frame
        // in base64. The rest of the readable output stays quiet in this
        // mode so the parser doesn't have to distinguish anything.
        Serial.printf("#P %lu %d %u %s\n", (unsigned long)r.us, (int)r.rssi,
                      (unsigned)r.len, b64_encode(r.raw, r.caplen));
#else
        Serial.printf("%8u %4d %-13s %s%s a1=%s a2=%s a3=%s %3uB",
                      r.t_ms, r.rssi, name,
                      r.retry ? "RETRY " : "      ",
                      r.prot  ? "enc " : "    ",
                      s1, s2, s3, r.len);
        if (r.type == 0 && (r.subtype == 12 || r.subtype == 10)) {
            Serial.printf("  ← reason=%u", r.reason);
        }
        if (from_node) Serial.print("  [node→]");
        if (to_node)   Serial.print("  [→node]");
        Serial.println();
#endif

        led_for_frame(r.type, r.subtype, r.retry, from_node);
    }

    led_tick();

    if (burst_open && (millis() - burst_last_ms) > BURST_GAP_MS) {
        close_burst();
    }

    // Heartbeat. The node transmits for 2.3 s every 63, so silence is the
    // normal state — and without this there's no way to tell "alive and
    // waiting" from "hung", or to know the capture is hooked up if the
    // banner (which only prints once) was missed.
    static uint32_t last_beat = 0;
    if (millis() - last_beat > 15000) {
        last_beat = millis();
        Serial.printf("· alive  t=%lu s  bursts=%u\n",
                      (unsigned long)(millis() / 1000), burst_num);
    }
}
