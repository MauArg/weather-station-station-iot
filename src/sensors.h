#pragma once

struct SensorData {
    // ── Sensores I2C existentes (Rail A) ──────────────────────────────────────
    float temperature_c;       // SHT31 — sensor principal
    float humidity_pct;        // SHT31
    float bmp_temperature_c;   // BMP085 — referencia interna
    float pressure_hpa;        // BMP085 — presión local
    float pressure_qnh;        // BMP085 — presión a nivel del mar (QNH)
    float solar_v;             // INA219 — voltaje panel solar
    float solar_mA;            // INA219 — corriente panel solar
    float solar_mW;            // INA219 — potencia panel solar
    float system_v;            // INA219 — voltaje sistema (ESP32)
    float system_mA;           // INA219 — corriente sistema
    float system_mW;           // INA219 — potencia sistema
    bool  sht31_ok;
    bool  bmp_ok;
    bool  solar_ok;
    bool  system_ok;

    // ── Nuevos sensores (PCB auxiliar) ────────────────────────────────────────
    float ds18b20_c;           // DS18B20 — temperatura exterior (always-on)
    bool  ds18b20_ok;

    // Nombres históricos dht11_*: el sensor físico es un DHT22 desde 2026-07-25
    // (mismo módulo, mismo pin). Se conservan porque son las claves del JSON de
    // telemetría y renombrarlas partiría la serie histórica en InfluxDB.
    float dht11_temp_c;        // DHT22 — temperatura (Rail B), -40..+80 °C
    float dht11_hum_pct;       // DHT22 — humedad (Rail B), calibrada de fábrica
    bool  dht11_ok;

    float photo_kohm;          // Fotorresistencia — resistencia estimada (Rail B)
    bool  photo_ok;

    float rain_kohm;           // Rain sensor — resistencia estimada en kΩ (Rail B)
    bool  rain_ok;

    // TODO [pulsos]: anemómetro (GPIO2) y pluviómetro (GPIO1) — datos diferidos
    //   Requiere estrategia de conteo acumulado en RTC memory entre deep sleeps
};

// ─── Rails ────────────────────────────────────────────────────────────────────
// Energiza Rail A y Rail B y arranca el reloj del warmup del DHT22.
//
// setup() la llama apenas arranca, antes de WiFi. El motivo es energético: el
// DHT22 pide ~2 s de estabilización tras recibir energía, y mientras el rail
// estuvo dentro de sensors_init() —que corre después de WiFi+MQTT y de la espera
// del comando retenido— esos 2 s se pagaban íntegros al final del ciclo, con la
// radio asociada consumiendo. Medido en campo el 2026-07-28: eran el 61% de los
// 3,3 s de ventana despierta. Anticipando el rail-on, el warmup transcurre
// mientras el nodo hace trabajo útil y el delay restante cae a unos cientos de ms.
//
// Es idempotente: sensors_init() la vuelve a llamar y no pasa nada. El timestamp
// que vale es el de la primera llamada.
//
// No cambia el tiempo total que los rails quedan energizados —el ciclo se acorta
// en la misma medida en que el encendido se adelanta— así que la exposición del
// sensor de lluvia a tensión continua, que es lo que la lectura pulsada busca
// minimizar, queda igual que antes.
void sensors_railsOn();

// Inicializa todos los sensores. Retorna false si alguno falla (no crítico).
// Llama a sensors_railsOn() por las suyas si setup() todavía no lo hizo.
bool sensors_init();

// Lee todos los sensores. Campos fallidos quedan como NAN.
SensorData sensors_read();

// ─── Monitor de batería para service mode ─────────────────────────────────────
// sensors_init() no corre en service mode (ver main.cpp): el nodo retoma la
// sesión sin pasar por el ciclo normal, así que los INA219 nunca se inicializan
// y el heartbeat no puede reportar el voltaje. Eso deja ciego justo cuando más
// importa — en service mode el nodo queda despierto drenando 50-140 mA sin deep
// sleep que permita recuperar tensión.
//
// Estas dos funciones inicializan y leen SOLO el INA219 de sistema (0x40), sin
// tocar Rail A ni Rail B: los INA219 cuelgan del bus I2C siempre alimentado
// (GPIO5/GPIO6, sin rail conmutado — ver tabla de pines en
// componentes_y_conexiones.md), así que no hay consumo extra de sensores.
// Requiere Wire.begin() previo, que setup() ya hace antes de entrar al modo.
bool  sensors_initSystemMonitor();

// Voltaje del bus del INA219 de sistema (batería). NAN si no inicializó.
float sensors_readSystemVoltage();

// ─── Bajo consumo ─────────────────────────────────────────────────────────────
// Deja los dos INA219 en power-down antes de dormir: ~6 µA cada uno contra los
// ~0,7-1 mA de la conversión continua (datasheet INA219, tabla de consumo).
// Cuelgan del bus 3V3, que el regulador del ESP32 sigue alimentando durante el
// deep sleep, así que sin esto siguen convirtiendo los ~57 s del ciclo en que
// nadie los lee. Es el consumo dominante fuera de la ventana despierta.
//
// Sólo actúa sobre los que llegaron a inicializar: Adafruit_INA219::powerSave()
// usa i2c_dev sin chequear null, y el camino de fallo de red entra a deep sleep
// sin pasar por sensors_init() — o sea que sin el guard crashearía justo en los
// ciclos que fallan la conexión.
//
// No hace falta despertarlos: begin() reescribe el registro de configuración con
// el modo continuo, y la primera conversión tarda ~532 µs, muy por debajo del
// tiempo que pasa hasta sensors_read().
void sensors_sleepMonitors();
