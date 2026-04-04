// =============================================================================
//  Firmware de diagnóstico PCB — ESP32-C3
//  Propósito: verificar sensores y circuitos antes del firmware de producción.
//  Flash: USB  |  Acceso: HTTP 192.168.18.105  |  Sin MQTT, sin deep sleep.
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <Adafruit_INA219.h>
#include "config_diagnostic.h"

// ─── Objetos de driver ────────────────────────────────────────────────────────
static OneWire           oneWire(PIN_DS18B20);
static DallasTemperature ds18b20(&oneWire);
static DHT               dht(PIN_DHT11, DHT11);
static Adafruit_INA219   ina_system(DIAG_INA219_SYSTEM);
static Adafruit_INA219   ina_solar(DIAG_INA219_SOLAR);
static WebServer         server(DIAG_HTTP_PORT);

// ─── Contadores de pulso (escritos en ISR) ────────────────────────────────────
static volatile uint32_t isr_anem_count    = 0;
static volatile uint32_t isr_anem_last_ms  = 0;
static volatile uint32_t isr_rain_count    = 0;
static volatile uint32_t isr_rain_last_ms  = 0;

// ─── Estado global de diagnóstico ────────────────────────────────────────────
struct DiagState {
    // DS18B20
    bool  ds18b20_ok   = false;
    float ds18b20_c    = 0.0f;
    // DHT11
    bool  dht11_ok     = false;
    float dht11_temp_c  = 0.0f;
    float dht11_hum_raw = 0.0f;   // valor directo del sensor
    float dht11_hum_cal = 0.0f;   // valor calibrado
    // Fotorresistencia (ADC GPIO3)
    int   photo_raw    = 0;
    float photo_v      = 0.0f;
    float photo_kohm   = 0.0f;
    char  photo_label[16] = "---";
    // Rain sensor (ADC GPIO4)
    int   rain_raw     = 0;
    float rain_v       = 0.0f;
    float rain_pct     = 0.0f;
    char  rain_label[16] = "---";
    // Contadores de pulso (copia atómica desde ISR)
    uint32_t anem_count      = 0;
    uint32_t anem_last_ms    = 0;
    uint32_t rain_count      = 0;
    uint32_t rain_last_ms    = 0;
    // INA219
    bool  ina_sys_ok   = false;
    float ina_sys_v    = 0.0f;
    float ina_sys_mA   = 0.0f;
    float ina_sys_mW   = 0.0f;
    bool  ina_sol_ok   = false;
    float ina_sol_v    = 0.0f;
    float ina_sol_mA   = 0.0f;
    float ina_sol_mW   = 0.0f;
    // WiFi
    bool  wifi_ok      = false;
    char  ip[16]       = "0.0.0.0";
    // Tiempo
    uint32_t uptime_ms = 0;
};

static DiagState state;

// =============================================================================
//  ISR
// =============================================================================

void IRAM_ATTR isr_anemometer() {
    uint32_t now = millis();
    if (now - isr_anem_last_ms >= DIAG_DEBOUNCE_MS) {
        isr_anem_count++;
        isr_anem_last_ms = now;
    }
}

void IRAM_ATTR isr_rain_gauge() {
    uint32_t now = millis();
    if (now - isr_rain_last_ms >= DIAG_DEBOUNCE_MS) {
        isr_rain_count++;
        isr_rain_last_ms = now;
    }
}

// =============================================================================
//  Inicialización de sensores (non-fatal)
// =============================================================================

static void init_ds18b20() {
    ds18b20.begin();
    ds18b20.setResolution(9);   // conversión ~93ms en lugar de 750ms
    state.ds18b20_ok = (ds18b20.getDeviceCount() > 0);
    Serial.printf("[DIAG] DS18B20: %s (%d dispositivo/s)\n",
        state.ds18b20_ok ? "OK" : "FAIL", ds18b20.getDeviceCount());
}

static void init_dht11() {
    dht.begin();
    state.dht11_ok = false;     // se valida en la primera lectura
    Serial.println("[DIAG] DHT11: init OK (esperando warmup)");
}

static void init_ina219() {
    state.ina_sys_ok = ina_system.begin();
    state.ina_sol_ok = ina_solar.begin();
    Serial.printf("[DIAG] INA219 system(0x%02X): %s  solar(0x%02X): %s\n",
        DIAG_INA219_SYSTEM, state.ina_sys_ok ? "OK" : "FAIL",
        DIAG_INA219_SOLAR,  state.ina_sol_ok ? "OK" : "FAIL");
}

// =============================================================================
//  Lectura de sensores
// =============================================================================

static void sensors_update() {
    state.uptime_ms = millis();

    // ── DS18B20 ───────────────────────────────────────────────────────────────
    if (state.ds18b20_ok) {
        ds18b20.requestTemperatures();
        float t = ds18b20.getTempCByIndex(0);
        if (t != DEVICE_DISCONNECTED_C) {
            state.ds18b20_c  = t;
        } else {
            state.ds18b20_ok = false;
        }
    }

    // ── DHT11 ─────────────────────────────────────────────────────────────────
    {
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h)) {
            state.dht11_temp_c  = t;
            state.dht11_hum_raw = h;
            // Calibración lineal: mapea [RAW_LO..RAW_HI] → [REAL_LO..REAL_HI]
            float cal = (h - DIAG_DHT_HUM_RAW_LO)
                      / (DIAG_DHT_HUM_RAW_HI - DIAG_DHT_HUM_RAW_LO)
                      * (DIAG_DHT_HUM_REAL_HI - DIAG_DHT_HUM_REAL_LO)
                      + DIAG_DHT_HUM_REAL_LO;
            if (cal < 0.0f)   cal = 0.0f;
            if (cal > 100.0f) cal = 100.0f;
            state.dht11_hum_cal = cal;
            state.dht11_ok      = true;
        } else {
            state.dht11_ok = false;
        }
    }

    // ── Fotorresistencia (ADC GPIO3) ──────────────────────────────────────────
    // Circuito: 3.3V → R10K → señal → fotorresistencia → GND
    // R_foto = R_pullup * V / (3.3 - V)
    {
        int   raw  = analogRead(PIN_PHOTORESISTOR);
        float v    = (raw / DIAG_ADC_MAX_RAW) * DIAG_ADC_VREF;
        float denom = DIAG_ADC_VREF - v;
        float kohm  = (denom > 0.01f)
                    ? (DIAG_PHOTO_PULLUP_KOHM * v / denom)
                    : 9999.0f;   // sensor desconectado u oscuridad total

        state.photo_raw  = raw;
        state.photo_v    = v;
        state.photo_kohm = kohm;

        if      (kohm < 1.0f)  strncpy(state.photo_label, "MUY BRILLANTE", 15);
        else if (kohm < 5.0f)  strncpy(state.photo_label, "BRILLANTE",     15);
        else if (kohm < 20.0f) strncpy(state.photo_label, "TENUE",         15);
        else if (kohm < 50.0f) strncpy(state.photo_label, "OSCURO",        15);
        else                   strncpy(state.photo_label, "MUY OSCURO",    15);
        state.photo_label[15] = '\0';
    }

    // ── Rain sensor (ADC GPIO4) ────────────────────────────────────────────────
    // Circuito PCB: 3.3V → R1∥R2(4.95kΩ) → señal → C1(100nF) → GND
    // Mojado = sensor conduce → V baja; seco = V alta
    // Calibración: V_dry=3.3V → 0%, V_wet=2.3V → 100%
    // wetness% = (V_dry - V) / (V_dry - V_wet) * 100
    {
        int   raw = analogRead(PIN_RAIN_SENSOR);
        float v   = (raw / DIAG_ADC_MAX_RAW) * DIAG_ADC_VREF;
        float pct = (DIAG_RAIN_V_DRY - v) / (DIAG_RAIN_V_DRY - DIAG_RAIN_V_WET) * 100.0f;
        if (pct < 0.0f)   pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;

        state.rain_raw = raw;
        state.rain_v   = v;
        state.rain_pct = pct;

        if      (pct < 10.0f) strncpy(state.rain_label, "SECO",        15);
        else if (pct < 40.0f) strncpy(state.rain_label, "LIGERAMENTE", 15);
        else if (pct < 70.0f) strncpy(state.rain_label, "MOJADO",      15);
        else                  strncpy(state.rain_label, "MUY MOJADO",  15);
        state.rain_label[15] = '\0';
    }

    // ── Contadores de pulso (copia atómica) ───────────────────────────────────
    noInterrupts();
    state.anem_count   = isr_anem_count;
    state.anem_last_ms = isr_anem_last_ms;
    state.rain_count   = isr_rain_count;
    state.rain_last_ms = isr_rain_last_ms;
    interrupts();

    // ── INA219 ────────────────────────────────────────────────────────────────
    if (state.ina_sys_ok) {
        state.ina_sys_v  = ina_system.getBusVoltage_V();
        state.ina_sys_mA = ina_system.getCurrent_mA();
        state.ina_sys_mW = ina_system.getPower_mW();
    }
    if (state.ina_sol_ok) {
        state.ina_sol_v  = ina_solar.getBusVoltage_V();
        state.ina_sol_mA = ina_solar.getCurrent_mA();
        state.ina_sol_mW = ina_solar.getPower_mW();
    }
}

// =============================================================================
//  WiFi
// =============================================================================

static void wifi_connect() {
    WiFi.mode(WIFI_STA);
    WiFi.config(DIAG_STATIC_IP, DIAG_GATEWAY, DIAG_SUBNET, DIAG_DNS);
    WiFi.begin(DIAG_WIFI_SSID, DIAG_WIFI_PASSWORD);

    Serial.printf("[DIAG] Conectando a WiFi '%s'...", DIAG_WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > DIAG_WIFI_TIMEOUT_MS) {
            Serial.println("\n[DIAG] WiFi timeout — continuando sin red");
            return;
        }
        delay(200);
        Serial.print(".");
    }
    state.wifi_ok = true;
    strncpy(state.ip, WiFi.localIP().toString().c_str(), 15);
    state.ip[15] = '\0';
    Serial.printf("\n[DIAG] WiFi OK — IP: %s\n", state.ip);
}

// =============================================================================
//  Helpers HTML
// =============================================================================

static const char* ok_badge(bool ok) {
    return ok
        ? "<span style='background:#2ecc40;color:#fff;padding:2px 8px;border-radius:4px'>OK</span>"
        : "<span style='background:#ff4136;color:#fff;padding:2px 8px;border-radius:4px'>FAIL</span>";
}

static String fmt_ms_ago(uint32_t last_ms) {
    if (last_ms == 0) return "sin pulsos";
    uint32_t ago = millis() - last_ms;
    char buf[32];
    snprintf(buf, sizeof(buf), "hace %lu ms", (unsigned long)ago);
    return String(buf);
}

// =============================================================================
//  Handlers HTTP
// =============================================================================

static void handle_root() {
    sensors_update();

    uint32_t up_s  = state.uptime_ms / 1000;
    uint32_t up_m  = up_s / 60;
    uint32_t up_h  = up_m / 60;
    up_s %= 60; up_m %= 60;

    // Buffer estático: evita uso masivo de stack
    static char buf[6144];
    int pos = 0;

#define APPEND(...) pos += snprintf(buf + pos, sizeof(buf) - pos, __VA_ARGS__)

    APPEND(
        "<!DOCTYPE html><html><head>"
        "<meta charset='UTF-8'>"
        "<meta http-equiv='refresh' content='%d'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Diagnóstico PCB</title>"
        "<style>"
        "body{font-family:monospace;background:#111;color:#eee;margin:0;padding:12px}"
        "h1{margin:0 0 4px;font-size:1.1em;color:#aaa}"
        ".hdr{display:flex;gap:16px;align-items:baseline;margin-bottom:12px;flex-wrap:wrap}"
        ".hdr span{font-size:0.85em;color:#888}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(240px,1fr));gap:10px;margin-bottom:12px}"
        ".card{background:#1e1e1e;border:1px solid #333;border-radius:6px;padding:10px}"
        ".card h2{margin:0 0 8px;font-size:0.9em;color:#aaa;border-bottom:1px solid #333;padding-bottom:4px}"
        ".row{display:flex;justify-content:space-between;font-size:0.85em;margin:3px 0}"
        ".lbl{color:#888}"
        ".val{color:#fff}"
        ".big{font-size:1.4em;color:#0ff;margin:4px 0}"
        ".sec{color:#aaa;font-size:0.8em;margin:8px 0 4px;text-transform:uppercase;letter-spacing:1px}"
        "form{display:inline}"
        "button{background:#444;color:#eee;border:1px solid #666;padding:6px 14px;border-radius:4px;cursor:pointer;font-size:0.85em}"
        "button:hover{background:#555}"
        ".footer{font-size:0.75em;color:#555;margin-top:8px}"
        "</style>"
        "</head><body>",
        DIAG_REFRESH_SEC
    );

    APPEND(
        "<div class='hdr'>"
        "<h1>Diagnóstico PCB — ESP32-C3</h1>"
        "<span>Uptime: %02lu:%02lu:%02lu</span>"
        "<span>IP: %s</span>"
        "<span>WiFi: %s</span>"
        "<span>FW: %s</span>"
        "</div>",
        (unsigned long)up_h, (unsigned long)up_m, (unsigned long)up_s,
        state.ip,
        state.wifi_ok ? "OK" : "FAIL",
        FIRMWARE_VERSION
    );

    // ── Temperatura ──────────────────────────────────────────────────────────
    APPEND("<div class='sec'>Temperatura</div><div class='grid'>");

    // DS18B20
    APPEND(
        "<div class='card'>"
        "<h2>DS18B20 (GPIO%d) &nbsp; %s</h2>",
        PIN_DS18B20, ok_badge(state.ds18b20_ok)
    );
    if (state.ds18b20_ok)
        APPEND("<div class='big'>%.2f °C</div>", state.ds18b20_c);
    else
        APPEND("<div class='big' style='color:#f44'>N/A</div>");
    APPEND("</div>");

    // DHT11
    APPEND(
        "<div class='card'>"
        "<h2>DHT11 (GPIO%d) &nbsp; %s</h2>",
        PIN_DHT11, ok_badge(state.dht11_ok)
    );
    if (state.dht11_ok) {
        APPEND("<div class='big'>%.1f °C</div>", state.dht11_temp_c);
        APPEND("<div class='row'><span class='lbl'>Humedad (cal)</span><span class='val'>%.1f %%</span></div>",
               state.dht11_hum_cal);
        APPEND("<div class='row'><span class='lbl'>Humedad (raw)</span><span class='val' style='color:#888'>%.1f %%</span></div>",
               state.dht11_hum_raw);
    } else {
        APPEND("<div class='big' style='color:#f44'>N/A</div>");
    }
    APPEND("</div>");

    APPEND("</div>"); // fin grid temperatura

    // ── ADC ───────────────────────────────────────────────────────────────────
    APPEND("<div class='sec'>Sensores analógicos (ADC)</div><div class='grid'>");

    // Fotorresistencia
    APPEND(
        "<div class='card'>"
        "<h2>Fotorresistencia (GPIO%d)</h2>"
        "<div class='big'>%s</div>"
        "<div class='row'><span class='lbl'>Raw</span><span class='val'>%d / 4095</span></div>"
        "<div class='row'><span class='lbl'>Voltaje</span><span class='val'>%.3f V</span></div>"
        "<div class='row'><span class='lbl'>R estimada</span><span class='val'>%.1f kΩ</span></div>"
        "</div>",
        PIN_PHOTORESISTOR,
        state.photo_label,
        state.photo_raw,
        state.photo_v,
        state.photo_kohm
    );

    // Rain sensor
    APPEND(
        "<div class='card'>"
        "<h2>Rain Sensor (GPIO%d)</h2>"
        "<div class='big'>%s</div>"
        "<div class='row'><span class='lbl'>Raw</span><span class='val'>%d / 4095</span></div>"
        "<div class='row'><span class='lbl'>Voltaje</span><span class='val'>%.3f V</span></div>"
        "<div class='row'><span class='lbl'>Humedad</span><span class='val'>%.1f %%</span></div>"
        "</div>",
        PIN_RAIN_SENSOR,
        state.rain_label,
        state.rain_raw,
        state.rain_v,
        state.rain_pct
    );

    APPEND("</div>"); // fin grid ADC

    // ── Contadores de pulso ───────────────────────────────────────────────────
    String anem_ago = fmt_ms_ago(state.anem_last_ms);
    String rain_ago = fmt_ms_ago(state.rain_last_ms);

    APPEND("<div class='sec'>Contadores de pulso</div><div class='grid'>");

    APPEND(
        "<div class='card'>"
        "<h2>Anemómetro (GPIO%d)</h2>"
        "<div class='big'>%lu pulsos</div>"
        "<div class='row'><span class='lbl'>Último</span><span class='val'>%s</span></div>"
        "</div>",
        PIN_ANEMOMETER,
        (unsigned long)state.anem_count,
        anem_ago.c_str()
    );

    APPEND(
        "<div class='card'>"
        "<h2>Pluviómetro (GPIO%d)</h2>"
        "<div class='big'>%lu pulsos</div>"
        "<div class='row'><span class='lbl'>Último</span><span class='val'>%s</span></div>"
        "</div>",
        PIN_RAIN_GAUGE,
        (unsigned long)state.rain_count,
        rain_ago.c_str()
    );

    APPEND("</div>"); // fin grid pulsos

    // Botón reset contadores
    APPEND(
        "<form method='POST' action='/api/reset_counters'>"
        "<button type='submit'>&#8635; Reset contadores</button>"
        "</form>"
    );

    // ── INA219 ────────────────────────────────────────────────────────────────
    APPEND("<div class='sec'>Potencia (INA219 I2C)</div><div class='grid'>");

    // INA219 system
    APPEND(
        "<div class='card'>"
        "<h2>Sistema (0x%02X) &nbsp; %s</h2>",
        DIAG_INA219_SYSTEM, ok_badge(state.ina_sys_ok)
    );
    if (state.ina_sys_ok) {
        APPEND(
            "<div class='row'><span class='lbl'>Bus</span><span class='val'>%.3f V</span></div>"
            "<div class='row'><span class='lbl'>Corriente</span><span class='val'>%.2f mA</span></div>"
            "<div class='row'><span class='lbl'>Potencia</span><span class='val'>%.2f mW</span></div>",
            state.ina_sys_v, state.ina_sys_mA, state.ina_sys_mW
        );
    } else {
        APPEND("<div style='color:#f44;font-size:0.85em'>No detectado en I2C</div>");
    }
    APPEND("</div>");

    // INA219 solar
    APPEND(
        "<div class='card'>"
        "<h2>Solar (0x%02X) &nbsp; %s</h2>",
        DIAG_INA219_SOLAR, ok_badge(state.ina_sol_ok)
    );
    if (state.ina_sol_ok) {
        APPEND(
            "<div class='row'><span class='lbl'>Bus</span><span class='val'>%.3f V</span></div>"
            "<div class='row'><span class='lbl'>Corriente</span><span class='val'>%.2f mA</span></div>"
            "<div class='row'><span class='lbl'>Potencia</span><span class='val'>%.2f mW</span></div>",
            state.ina_sol_v, state.ina_sol_mA, state.ina_sol_mW
        );
    } else {
        APPEND("<div style='color:#f44;font-size:0.85em'>No detectado en I2C</div>");
    }
    APPEND("</div>");

    APPEND("</div>"); // fin grid INA219

    APPEND(
        "<div class='footer'>Auto-refresh cada %d s &nbsp;|&nbsp; "
        "<a href='/api/status' style='color:#555'>GET /api/status</a>"
        "</div>"
        "</body></html>",
        DIAG_REFRESH_SEC
    );

#undef APPEND

    server.send(200, "text/html; charset=utf-8", buf);
}

static void handle_json() {
    sensors_update();

    uint32_t anem_ago = (state.anem_last_ms > 0) ? (millis() - state.anem_last_ms) : 0;
    uint32_t rain_ago = (state.rain_last_ms > 0) ? (millis() - state.rain_last_ms) : 0;

    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "{"
        "\"uptime_ms\":%lu,"
        "\"ds18b20\":{\"ok\":%s,\"temp_c\":%.2f},"
        "\"dht11\":{\"ok\":%s,\"temp_c\":%.2f,\"hum_pct_cal\":%.1f,\"hum_pct_raw\":%.1f},"
        "\"photo\":{\"raw\":%d,\"voltage\":%.3f,\"kohm\":%.2f,\"label\":\"%s\"},"
        "\"rain\":{\"raw\":%d,\"voltage\":%.3f,\"wetness_pct\":%.1f,\"label\":\"%s\"},"
        "\"anemometer\":{\"count\":%lu,\"last_ms_ago\":%lu},"
        "\"rain_gauge\":{\"count\":%lu,\"last_ms_ago\":%lu},"
        "\"ina_system\":{\"ok\":%s,\"v\":%.3f,\"mA\":%.2f,\"mW\":%.2f},"
        "\"ina_solar\":{\"ok\":%s,\"v\":%.3f,\"mA\":%.2f,\"mW\":%.2f},"
        "\"wifi\":{\"ok\":%s,\"ip\":\"%s\"}"
        "}",
        (unsigned long)state.uptime_ms,
        state.ds18b20_ok ? "true" : "false", state.ds18b20_c,
        state.dht11_ok   ? "true" : "false", state.dht11_temp_c, state.dht11_hum_cal, state.dht11_hum_raw,
        state.photo_raw, state.photo_v, state.photo_kohm, state.photo_label,
        state.rain_raw,  state.rain_v,  state.rain_pct,   state.rain_label,
        (unsigned long)state.anem_count, (unsigned long)anem_ago,
        (unsigned long)state.rain_count, (unsigned long)rain_ago,
        state.ina_sys_ok ? "true" : "false", state.ina_sys_v, state.ina_sys_mA, state.ina_sys_mW,
        state.ina_sol_ok ? "true" : "false", state.ina_sol_v, state.ina_sol_mA, state.ina_sol_mW,
        state.wifi_ok ? "true" : "false", state.ip
    );

    server.send(200, "application/json", buf);
}

static void handle_reset_counters() {
    noInterrupts();
    isr_anem_count   = 0;
    isr_anem_last_ms = 0;
    isr_rain_count   = 0;
    isr_rain_last_ms = 0;
    interrupts();
    // POST-Redirect-GET para que el browser recargue la página raíz
    server.sendHeader("Location", "/");
    server.send(303);
}

static void setup_http_server() {
    server.on("/",                   HTTP_GET,  handle_root);
    server.on("/api/status",         HTTP_GET,  handle_json);
    server.on("/api/reset_counters", HTTP_POST, handle_reset_counters);
    server.onNotFound([]() {
        server.send(404, "text/plain", "Not found");
    });
    server.begin();
    Serial.printf("[DIAG] HTTP server en http://%s:%d/\n", state.ip, DIAG_HTTP_PORT);
}

// =============================================================================
//  setup / loop
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);    // espera enumeración USB CDC

    Serial.println("\n=== FIRMWARE DIAGNÓSTICO PCB ===");
    Serial.printf("  Version : %s\n", FIRMWARE_VERSION);
    Serial.printf("  Fecha   : %s %s\n", __DATE__, __TIME__);

    // I2C — debe inicializarse antes de los sensores I2C
    Wire.begin(DIAG_I2C_SDA, DIAG_I2C_SCL);

    // Interrupciones de pulso
    pinMode(PIN_ANEMOMETER, INPUT_PULLUP);
    pinMode(PIN_RAIN_GAUGE,  INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ANEMOMETER), isr_anemometer, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_RAIN_GAUGE),  isr_rain_gauge,  FALLING);

    // Sensores (todos non-fatal)
    init_ds18b20();
    init_ina219();
    init_dht11();
    // ADC no requiere init explícita en ESP32 Arduino

    // Warmup DHT11 (bloqueante en setup, no interfiere con loop)
    Serial.printf("[DIAG] DHT11 warmup %d ms...\n", DIAG_DHT_WARMUP_MS);
    delay(DIAG_DHT_WARMUP_MS);

    // WiFi + HTTP
    wifi_connect();
    if (state.wifi_ok) {
        setup_http_server();
    } else {
        Serial.println("[DIAG] Sin WiFi — usar monitor serial para ver lecturas");
    }

    // Primera lectura
    sensors_update();
    Serial.println("[DIAG] Setup completo — entrando en loop");
}

void loop() {
    if (state.wifi_ok) {
        server.handleClient();
    }

    static uint32_t last_update = 0;
    if (millis() - last_update >= DIAG_SENSOR_PERIOD_MS) {
        last_update = millis();
        sensors_update();

        // Log compacto por serial
        Serial.printf(
            "[%7lus] DS18B20:%s%.1f°C  DHT11:%s%.1f°C/%.0f%%(cal)/%.0f%%(raw)  "
            "Photo:%d(%.2fV/%s)  Rain:%d(%.2fV/%.0f%%/%s)  "
            "Anem:%lu  Pluv:%lu  "
            "SysINA:%s%.2fV/%.1fmA  SolINA:%s%.2fV/%.1fmA\n",
            (unsigned long)(state.uptime_ms / 1000),
            state.ds18b20_ok ? "" : "FAIL ", state.ds18b20_c,
            state.dht11_ok   ? "" : "FAIL ", state.dht11_temp_c, state.dht11_hum_cal, state.dht11_hum_raw,
            state.photo_raw, state.photo_v, state.photo_label,
            state.rain_raw,  state.rain_v,  state.rain_pct, state.rain_label,
            (unsigned long)state.anem_count,
            (unsigned long)state.rain_count,
            state.ina_sys_ok ? "" : "FAIL ", state.ina_sys_v, state.ina_sys_mA,
            state.ina_sol_ok ? "" : "FAIL ", state.ina_sol_v, state.ina_sol_mA
        );
    }
}
