// wifi-sniffer — captura 802.11 en modo promiscuo para diagnosticar por qué se
// muere el enlace del nodo a mitad de ciclo.
//
// El problema, medido desde tres lados independientes (contadores $SYS del
// broker, sondeo ICMP y los campos pv_* del propio firmware): el nodo se asocia
// bien, funciona un rato variable y después deja de estar en la red en los DOS
// sentidos, mientras su driver sigue reportando WL_CONNECTED con un RSSI normal.
// Nadie le avisa nada. Ver ../../../STATUS.md.
//
// Lo que falta saber está en el aire, y no hace falta descifrar nada: las tramas
// que contestan la pregunta viajan en claro. El bit de Retry vive en la cabecera
// MAC, y los deauth/disassoc son management frames sin cifrar que además traen su
// reason code. Tres ramas mutuamente excluyentes:
//
//   el nodo sigue transmitiendo, con Retry=1 y sin ACK del AP
//        -> el AP lo dio de baja en silencio y no le contesta más
//   el nodo deja de transmitir
//        -> se le colgó el driver, y el problema es del lado del nodo
//   aparece un deauth/disassoc
//        -> nos dice quién lo mandó y por qué (reason code)
//
// Limitaciones que conviene tener presentes al leer la salida:
//   - Escucha UN canal por vez. No es un problema acá porque el canal es fijo.
//   - Es un receptor más: sólo ve lo que le llega a SU antena. Conviene ponerlo
//     cerca del AP, que es el punto de vista que importa.
//   - Los ACK son tramas muy cortas que salen inmediatamente después del dato;
//     el promiscuo del ESP32 no siempre los entrega. Una ausencia de ACK en la
//     captura NO prueba que el AP no haya contestado — pero si se ven ACKs para
//     unos frames y no para otros, ESO sí es señal.
#include <Arduino.h>
#include <WiFi.h>
#include "esp_wifi.h"

// ─── Qué escuchar ─────────────────────────────────────────────────────────────
// MAC del nodo (Espressif) y BSSID de la SSID de IoT sobre la que se asocia.
static const uint8_t NODE_MAC[6] = {0x80, 0xF1, 0xB2, 0x6D, 0xF9, 0xFC};
static const uint8_t AP_BSSID[6] = {0xBE, 0xF1, 0x7E, 0xE9, 0xF7, 0x2F};

#ifndef SNIFF_CHANNEL
  #define SNIFF_CHANNEL 1
#endif

// Silencio que cierra una ráfaga. El nodo vive ~2,3 s y duerme ~60 s, así que
// 5 s separan un ciclo del siguiente sin ninguna ambigüedad.
static const uint32_t BURST_GAP_MS = 5000;

// ─── Cabecera 802.11 ──────────────────────────────────────────────────────────
// Ojo: un ACK mide 10 bytes y sólo trae addr1. Leer addr2 en ese caso sería leer
// fuera de la trama, por eso todo acceso va guardado por la longitud real.
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
    uint8_t  n_addr;      // cuántas direcciones son válidas en esta trama
    uint16_t reason;      // sólo deauth/disassoc
    uint16_t len;
};

static QueueHandle_t q;

// ─── Estadística por ráfaga (un ciclo del nodo) ───────────────────────────────
static uint32_t burst_first_ms = 0, burst_last_ms = 0;
static uint32_t n_from_node = 0, n_to_node = 0, n_retry = 0, n_ack_to_node = 0;
static uint32_t n_deauth = 0, n_assoc = 0, n_auth = 0, n_null = 0;
static bool     burst_open = false;
static uint32_t burst_num = 0;

static inline bool mac_eq(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

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
        case 4:  return "null";       // el "estoy acá" del power save
        case 8:  return "qos-data";
        case 12: return "qos-null";
        default: return "data?";
    }
}

// ─── Callback de promiscuo ────────────────────────────────────────────────────
// Corre en la tarea de WiFi: acá sólo se filtra y se encola. Imprimir desde
// adentro haría perder tramas, que es justo lo que no queremos.
static void IRAM_ATTR sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
    const uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 10) return;

    const wifi_hdr_t* h = (const wifi_hdr_t*)pkt->payload;
    const uint16_t fc = h->frame_ctrl;
    const uint8_t  ftype = (fc >> 2) & 0x3;
    const uint8_t  fsub  = (fc >> 4) & 0xF;

    // Beacons y probe-resp de toda la vecindad: son la mayoría del aire y no
    // aportan nada acá.
    if (ftype == 0 && (fsub == 8 || fsub == 5)) return;

    // Cuántas direcciones trae realmente esta trama.
    uint8_t n_addr = 0;
    if (len >= 10) n_addr = 1;
    if (len >= 16) n_addr = 2;
    if (len >= 24) n_addr = 3;

    // Nos interesa todo lo que toque al nodo o a su AP.
    bool relevant = false;
    if (n_addr >= 1 && (mac_eq(h->addr1, NODE_MAC) || mac_eq(h->addr1, AP_BSSID))) relevant = true;
    if (n_addr >= 2 && (mac_eq(h->addr2, NODE_MAC) || mac_eq(h->addr2, AP_BSSID))) relevant = true;
    if (n_addr >= 3 && (mac_eq(h->addr3, NODE_MAC) || mac_eq(h->addr3, AP_BSSID))) relevant = true;
    // Un deauth broadcast también nos importa aunque no nombre al nodo.
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
    // El reason code de un deauth/disassoc son los 2 primeros bytes del cuerpo.
    if (ftype == 0 && (fsub == 12 || fsub == 10) && len >= 26) {
        r.reason = pkt->payload[24] | (pkt->payload[25] << 8);
    }

    xQueueSendFromISR(q, &r, NULL);
}

static void fmt_mac(char* out, const uint8_t* m) {
    sprintf(out, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void close_burst() {
    if (!burst_open) return;
    Serial.printf(
        "\n──── fin de ráfaga #%u ── duró %u ms ────\n"
        "     del nodo: %u   al nodo: %u   ACK al nodo: %u   con Retry: %u\n"
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

    q = xQueueCreate(512, sizeof(rec_t));

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    // Management + data + control. Los ACK son control, y son los que dicen si
    // el AP le está contestando al nodo.
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
    Serial.printf("canal %d   nodo %s   AP %s\n", SNIFF_CHANNEL, n, a);
    Serial.println("beacons y probe-resp filtrados; el resto se imprime\n");
}

void loop() {
    rec_t r;
    while (xQueueReceive(q, &r, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (!burst_open) {
            burst_open = true;
            burst_num++;
            burst_first_ms = r.t_ms;
            Serial.printf("──── ráfaga #%u ── t=%u ms ────\n", burst_num, r.t_ms);
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

        Serial.printf("%8u %4d %-13s %s%s a1=%s a2=%s a3=%s %3uB",
                      r.t_ms, r.rssi, name,
                      r.retry ? "RETRY " : "      ",
                      r.prot  ? "enc " : "    ",
                      s1, s2, s3, r.len);
        if (r.type == 0 && (r.subtype == 12 || r.subtype == 10)) {
            Serial.printf("  ← reason=%u", r.reason);
        }
        if (from_node) Serial.print("  [nodo→]");
        if (to_node)   Serial.print("  [→nodo]");
        Serial.println();
    }

    if (burst_open && (millis() - burst_last_ms) > BURST_GAP_MS) {
        close_burst();
    }
}
