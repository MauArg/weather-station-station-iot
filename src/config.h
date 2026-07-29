#pragma once

// ─── Firmware ─────────────────────────────────────────────────────────────────
// La versión real la define -DFIRMWARE_VERSION en platformio.ini, por entorno.
// Este fallback es un centinela, no una versión: si aparece en la telemetría,
// significa que el build salió de un entorno que se olvidó de definir el flag.
// Antes decía "1.0.0", que era una versión que existió de verdad — un build mal
// configurado quedaba indistinguible de un deploy legítimo de 1.0.0.
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "0.0.0-nobuildflag"
#endif

// ─── Red ──────────────────────────────────────────────────────────────────────
#define WIFI_SSID           "Ire y Mau"
#define WIFI_PASSWORD       "Lady-350!"
#define WIFI_TIMEOUT_MS     15000
#define WIFI_MAX_RETRIES    3

// ─── Power save de WiFi ───────────────────────────────────────────────────────
// El default del Arduino-ESP32 es modem sleep (`WIFI_PS_MIN_MODEM`): la radio se
// apaga entre beacons DTIM y despierta a recibir.
//
// Medido el 2026-07-29, y es el motivo por el que esto es un `#define` y no el
// default: **la asociación WiFi se muere a un tiempo variable después de
// asociarse**, y ahí se explica toda la pérdida de telemetría. Sondeando el nodo
// con ICMP cada 150 ms, la ventana en que responde predice exactamente hasta
// dónde llega el ciclo:
//
//     0-180 ms alcanzable   -> ni siquiera conecta al broker
//   470-1100 ms alcanzable  -> conecta, no llega a publicar (el publish va a 2,3 s)
//  2150-2700 ms alcanzable  -> publica y cierra limpio
//
// Dos indicios apuntan al power save: los pings vuelven en 33-74 ms dentro de una
// LAN (es el AP guardando el paquete hasta el DTIM del nodo), y agregar tráfico
// de bajada durante el sleep hace que la asociación muera ANTES, no después.
//
// Apagarlo no es gratis: son ~+22 mAh/día sobre un presupuesto activo de ~47, así
// que si resuelve la pérdida hay que decidir el tradeoff con el dato en la mano —
// y se puede medir solo, porque el nodo publica `system_mA` en cada telemetría.
#define WIFI_POWER_SAVE     0

// ─── MQTT ─────────────────────────────────────────────────────────────────────
#define MQTT_BROKER         "192.168.18.250"   // IP de la Raspberry Pi
#define MQTT_PORT           1883
#define MQTT_USER           "weather_station_iot"
#define MQTT_PASSWORD       "aXdC7nE2gLEe"
#define MQTT_CLIENT_ID      "weather-station-01"

// Topics
#define TOPIC_TELEMETRY     "station/01/telemetry"
#define TOPIC_STATUS        "station/01/status"
#define TOPIC_CMD           "station/01/cmd"         // retained, escrito por N8N
#define TOPIC_DEBUG         "station/01/dbg"         // balizas de uplink, sin retain

// ─── Balizas de diagnóstico del uplink (experimento, temporal) ────────────────
// Medido el 2026-07-29 desde un tercer punto de vista (contadores $SYS del
// broker): en un ciclo perdido el broker recibe el CONNECT y el SUBSCRIBE (~88 B)
// y después NADA — ni el PUBLISH de telemetría ni el DISCONNECT. O sea que el
// camino nodo→broker muere entre los ~330 ms del handshake y los ~2290 ms del
// publish, y el nodo no se puede enterar: en QoS 0 no hay ack, y publish() sólo
// informa que lwIP aceptó los bytes.
//
// Las balizas acotan ese hueco. Son dos publishes de ~42 B en un topic propio,
// con el boot_count adentro para poder cruzarlos contra la telemetría faltante:
//
//   pre_sensors (~1170 ms, apenas termina la espera del retenido)
//   pre_publish (~2280 ms, inmediatamente antes de la telemetría)
//
// Lo que discrimina, por ciclo perdido:
//   ninguna llega        → murió durante la espera del retenido
//   sólo pre_sensors     → murió durante la lectura de sensores
//   las dos llegan       → el enlace estaba vivo 10 ms antes; lo que no pasa es
//                          el frame de 503 B de la telemetría, y eso apunta al
//                          tamaño, no al enlace
//   la telemetría llega  → el tráfico extra la arregló, y entonces el problema es
//                          el hueco de inactividad de ~1,7 s (modem sleep)
//
// Poner en 0 para volver al camino de red sin perturbar, que es el baseline
// contra el que se comparan las mediciones.
#define UPLINK_BEACON       0

// Tiempo de espera para recibir el mensaje retenido del broker
#define MQTT_RETAINED_WAIT_MS  800

// ─── Keepalive de MQTT ────────────────────────────────────────────────────────
// Dos valores, porque los dos caminos del firmware tienen necesidades OPUESTAS y
// un único número no puede servir a los dos.
//
// El keepalive no gobierna sólo el PINGREQ del cliente: define también cuánto
// tarda el broker en dar por muerta una sesión, que por spec es 1,5 × keepalive.
// Y con un client-ID fijo, una sesión vieja que sigue viva cuando el nodo vuelve
// a conectar fuerza un takeover por duplicado en cada ciclo.
//
// CICLO NORMAL — corto. El nodo vive ~2,2 s, así que su propio PINGREQ nunca
// llega a dispararse y el valor es gratis del lado del cliente. Lo que se compra
// con 30 s es que el broker expire la sesión a los 45 s, por debajo del ciclo de
// ~63 s (57 s en el peor caso del ±5% del timer de deep sleep): cada wake
// encuentra el client-ID libre y no hay takeover. Con 60 s la expiración caía a
// los 90 s y la sesión vieja seguía viva en TODAS las reconexiones — es la
// hipótesis principal del 38-42% de payloads perdidos (ver ../STATUS.md).
//
// SERVICE MODE — largo. Ahí el nodo vive minutos y un ArduinoOTA.handle() puede
// bloquear decenas de segundos sin mandar nada, así que el margen del broker es
// lo único que sostiene la sesión; con 15 s un solo PINGREQ perdido la tumbaba, y
// eso era lo que cortaba y reiniciaba las sesiones. No hay takeover que evitar en
// este camino: el nodo no se duerme entre medio. serviceMode_run() reconecta para
// renegociarlo, porque el valor que gobierna al broker es el que viajó en el
// CONNECT.
#define MQTT_KEEPALIVE_NORMAL_SEC    30
#define MQTT_KEEPALIVE_SERVICE_SEC   60

// Tamaño del buffer de PubSubClient, y el payload útil que queda para el topic de
// telemetría una vez descontados el header fijo (5), el largo (2) y el topic.
// Derivarlo en vez de escribir 741 a mano evita que se desincronice si cambia el
// buffer o el nombre del topic — y `publish()` descarta el mensaje entero, en
// silencio, cuando no entra.
#define MQTT_BUFFER_BYTES      768
#define MQTT_TELEMETRY_BUDGET  (MQTT_BUFFER_BYTES - 5 - 2 - (int)(sizeof(TOPIC_TELEMETRY) - 1))

// ─── I2C ──────────────────────────────────────────────────────────────────────
#define I2C_SDA             6
#define I2C_SCL             5

// ─── Sensores ─────────────────────────────────────────────────────────────────
#define ALTITUDE_M          780.0f    // Altitud del lugar en metros SNM
#define INA219_SOLAR_ADDR   0x41      // INA219 panel solar
#define INA219_SYSTEM_ADDR  0x40      // INA219 consumo ESP32

// ─── Deep sleep ───────────────────────────────────────────────────────────────
#define SLEEP_INTERVAL_SEC  60   // 300s = 5 minutos (modo normal)

// ─── OTA / Service mode ───────────────────────────────────────────────────────
#define OTA_HOSTNAME        "weather-station-01"
#define OTA_PASSWORD        "rnLm43G7wcYr"            // mismo que upload_flags en platformio.ini

#define SERVICE_MODE_DEFAULT_TIMEOUT_MIN  15
#define SERVICE_MODE_MAX_TIMEOUT_MIN      60          // techo absoluto ignorando lo que pida el servidor
#define SERVICE_MODE_HEARTBEAT_SEC        30

// Reconexión de MQTT durante service mode. Antes una caída del broker abortaba la
// sesión entera: el nodo dormía sin poder limpiar el retenido y al despertar
// arrancaba de cero.
//
// El peor caso no son los 10s que sugieren 5×2s: cada intento puede además
// consumir el socket timeout esperando el CONNACK. Con setSocketTimeout(5) en
// connectMQTT() queda en 5×(5+2) ≈ 35s. Importa porque durante ese bloqueo no se
// llama a ArduinoOTA.handle(), así que si el enlace se cae justo cuando arrancás
// un flash, espota puede darse por vencido — se reintenta y listo, pero conviene
// saberlo antes de creer que el nodo se colgó.
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

// ─── Sistema de logs en runtime (por MQTT) ────────────────────────────────────
// No confundir con LOG_LEVEL de arriba: aquel es compile-time y sale por Serial.
// Éste se activa por comando y se recupera a distancia. Ver logging_system_design.md.
//
// El ring vive en RTC memory, que en el ESP32-C3 son 8176 B en total
// (memory.ld → rtc_iram_seg, 0x2000 - 0x10) compartidos con el resto de las
// variables RTC y con lo que reserve el sistema. 768 × 8 B = 6144 B deja margen.
// Si se pasa, el linker falla con "region rtc_iram_seg overflowed" — el error
// aparece al compilar, no en campo.
#define LOG_RING_ENTRIES      768
#define LOG_MAX_LEVEL         3

// Entries por página del dump.
//
// Presupuesto real: el buffer MQTT son 768 B, menos header (5), largo (2) y
// topic `station/01/log/data` (19) → 742 B útiles de payload. El wrapper JSON en
// su peor caso —`dropped` es uint32 y puede llegar a 10 dígitos— mide 77 B:
//   {"page":12,"pages":13,"count":768,"dropped":4294967295,"entries":55,"b64":""}
// Quedan 665 B para el base64, que a 3/4 son 498 B binarios = 62 entries.
//
// 55 se elige por margen: 55 × 8 B = 440 B → 588 B de base64 → 665 B de payload,
// 77 B por debajo del límite. Con 60 el margen bajaba a 25 B, y pasarse no falla
// ruidosamente de entrada: `serializeJson` trunca en silencio y sólo después
// `publish()` rechaza el mensaje, dejando esa página imposible de recuperar por
// más que el backend la reintente.
#define LOG_ENTRIES_PER_PAGE  55

// Topics del dump. Sin retain a propósito: `cmd` es retenido y de slot único, y
// un ida y vuelta de N páginas ahí pelearía contra esa semántica además de
// bloquear el slot para cualquier otro comando.
#define TOPIC_LOG_REQ       "station/01/log/req"
#define TOPIC_LOG_DATA      "station/01/log/data"

// ─── Red (IP estática) ────────────────────────────────────────────────────────
#define WIFI_STATIC_IP      IPAddress(192, 168, 18, 105)
#define WIFI_GATEWAY        IPAddress(192, 168, 18, 1)
#define WIFI_SUBNET         IPAddress(255, 255, 255, 0)
#define WIFI_DNS            IPAddress(8, 8, 8, 8)

// ─── Rail control (transistores NPN BC337 en PCB principal) ───────────────────
// GPIO HIGH → NPN conduce → GND del rail conectado → sensores alimentados
#define PIN_RAIL_A          7   // Rail A: sensores exteriores (SHT31 + BMP085)
#define PIN_RAIL_B          8   // Rail B: sensores enclosure (DHT22, foto, lluvia)
// TODO [bajo consumo]: apagar Rail B en Tier 2 y Rail A en Tier 3
//   según umbral de batería — implementar en battery.h

// ─── Pines nuevos sensores ────────────────────────────────────────────────────
#define PIN_DS18B20         10  // OneWire temperatura exterior (always-on)
#define PIN_DHT22            0  // DHT22 DATA (Rail B) — mismo pin que el ex DHT11
#define PIN_PHOTORESISTOR    3  // ADC fotorresistencia (Rail B)
#define PIN_RAIN_SENSOR      4  // ADC sensor de lluvia AO (Rail B)
#define PIN_ANEMOMETER       2  // Pulso FALLING — anemómetro (always-on)
#define PIN_RAIN_GAUGE       1  // Pulso FALLING — pluviómetro (always-on)
// TODO [pulsos]: implementar conteo acumulado (RTC memory) para
//   anemómetro y pluviómetro entre ciclos de deep sleep

// ─── Calibración sensores analógicos ─────────────────────────────────────────
// Fotorresistencia: divisor 3V3 → R10kΩ → señal → foto → GND
//   R_foto = R_pullup * V / (3.3 - V)
#define PHOTO_PULLUP_KOHM   10.0f

// Rain sensor: divisor 3V3 → R1‖R2(4.95kΩ) → señal → sensor → GND
//   R_rain = R_pullup * V / (3.3 - V)
#define RAIN_PULLUP_KOHM    4.95f

// ADC
#define ADC_VREF            3.3f
#define ADC_MAX_RAW      4095.0f

// ─── DHT22 (Rail B) ───────────────────────────────────────────────────────────
// Sin constantes de calibración de humedad: las que había (DHT_HUM_RAW/REAL_*)
// corregían el sesgo del DHT11 defectuoso. El DHT22 viene calibrado de fábrica
// (±2% RH, ±0.5 °C) — cualquier corrección acá volvería a sesgar la lectura.
//
// Warmup tras energizar Rail B. El datasheet del AM2302 pide ≥1s de "unstable
// status"; 2s es margen conservador. Se mide desde el rail-on, no desde el boot
// — ver sensors_railsOn().
//
// OJO — desde 1.5.0 esta constante es la que fija el piso del tiempo despierto.
// Con el rail-on adelantado al arranque del ciclo, el despierto es
//   max(camino de red, DHT_WARMUP_MS) + lectura de sensores (~240 ms)
// y el camino de red mide ~1270 ms (275 WiFi + 42 MQTT + 800 retenido + ~150
// init), o sea que el término que manda es este. El delay que queda son ~730 ms
// de espera pura.
//
// Consecuencia contraintuitiva: **acortar MQTT_RETAINED_WAIT_MS ya no ahorra
// nada.** Bajarlo de 800 a 200 ms sólo hace crecer este warmup de 730 a 1330 ms
// y el despierto total queda igual — el pendiente que figura en ../STATUS.md
// como "los 800 ms del retenido son el 24% del despierto" dejó de aplicar al
// quedar detrás de esta barrera.
//
// La palanca que sí queda es este mismo número: 2000 ms es 2× el mínimo del
// datasheet. Bajarlo a ~1200-1500 ms recortaría el despierto casi 1:1, pero hay
// que validarlo contra lecturas reales del DHT22 antes de tocarlo — el warmup
// corto ya fue sospechoso de lecturas erráticas una vez (ver ../STATUS.md, el
// bug de 2026-07-25 donde el warmup nunca se ejecutaba).
#define DHT_WARMUP_MS           2000

// Período mínimo de muestreo del DHT22 (datasheet: ≥2s entre lecturas).
// Espaciado del reintento cuando la primera trama sale corrupta.
#define DHT_RETRY_INTERVAL_MS   2000