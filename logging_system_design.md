# Sistema de logs del nodo — diseño

> Diseñado el 2026-07-27/28 (Mau + Claude). Los contratos de este documento cruzan los tres repos: firmware (`weather-station-station-iot`), backend Go (`weather-station-backend-service`) y dashboard React (`weather-station-frontend-dashboard`). Antes de tocar cualquiera de los tres, leer al menos las secciones "Formato de entry", "Diccionario" y "Protocolo de recuperación".

## Qué problema resuelve

Hoy el nodo en campo **no tiene observabilidad**. `LOG_V` y `LOG_E` (`src/config.h`) son `Serial.printf` y con `LOG_LEVEL=0` compilan a no-op, así que la única forma de ver algo es abrir la caja estanca y enchufar USB. La alternativa —flashear un build de debug— cuesta el `delay(2000)` por wake y sigue sin poder leerse a distancia.

El disparador concreto: **~17% de los ciclos no publican telemetría** y no hay forma de saber si falla WiFi o MQTT. `boot_count` incrementa antes de la red, así que se sabe que el nodo despertó, y nada más.

### Qué NO es

**No es `LOG_LEVEL`.** Aquel es compile-time y sale por Serial; éste se activa en runtime y sale por MQTT. Conviven, y lo importante es que **este sistema va compilado en el build de producción**. Si quedara detrás de `LOG_LEVEL>0`, para debuggear habría que flashear un build de debug — exactamente lo que el sistema existe para evitar.

## Modelo mental

Como el logging de un router comercial: acumula internamente hasta un límite, se pisa lo viejo, y se puede consultar. La diferencia es que un router está enchufado y este nodo cuida cada mA.

De ahí la decisión de fondo: **el logging se activa a demanda, no corre siempre**. El operador sabe cuándo lo necesita, igual que sabe cuándo conviene flashear según el SoC de la batería.

Flujo completo:

```
necesito debuggear algo
   → activo logging (comando liviano, ciclo normal, el nodo sigue durmiendo)
   → dejo correr N horas
   → entro a service mode y traigo los logs (paginado)
   → el backend confirma que recibió todo
   → el nodo borra y se desactiva solo
   → si no capturé lo que buscaba, repito
```

## Restricciones que dan forma al diseño

| Restricción | Consecuencia |
|---|---|
| El deep sleep borra la RAM normal | El buffer vive en **RTC memory** |
| El ESP32-C3 tiene **8176 B de RTC memory** y nada más (`memory.ld:59`, `0x2000 - 0x10`; en el C3 `rtc_data_seg`/`rtc_slow_seg`/`rtc_iram_seg` son la misma región) | La captura se mide en **horas, no en días** |
| No hay RTC de hardware ni NTP, y `millis()` se reinicia en cada ciclo | No se puede timestampear en el nodo; lo reconstruye el backend |
| El topic `cmd` es **retenido y de slot único** | El comando se consume y se limpia en el acto; el estado vive en RTC |
| Buffer MQTT de 768 B | El dump va **paginado** |
| El nodo está despierto ~10 s por ciclo y no escucha | El dump ocurre en **service mode** |

## Formato de entry — 8 bytes

```c
struct LogEntry {
    uint16_t boot;   // boot_count truncado a 16 bits — 45 días a 60 s
    uint16_t ms;     // ms desde el arranque de ese ciclo (un ciclo fallido son ~45 s, entra)
    uint8_t  code;   // qué pasó — ver diccionario
    uint8_t  a;      // argumento chico: nro de intento, estado MQTT, índice de sensor
    int16_t  b;      // argumento grande: RSSI, bytes, duración en ms
};
```

Ocho bytes, alineado, sin punteros ni strings. Escribir una entry es un `memcpy` de 8 B: **la captura es energéticamente gratis**. Esto importa más de lo que parece — significa que el modo logging no mueve la aguja del consumo mientras corre, y que el único costo real está en el dump, que es operador-iniciado.

> **Corrección respecto de la conversación de diseño**: se había propuesto una guarda que desactivara el logging con batería baja. **Se descarta**: partía de suponer que capturar costaba energía, y no cuesta. La única operación cara es el dump, que ya está cubierta por el semáforo de batería que existe en la vista de service mode.

## Ring en RTC memory

```c
RTC_DATA_ATTR LogEntry rtc_logRing[LOG_RING_ENTRIES];
RTC_DATA_ATTR uint16_t rtc_logHead;      // próxima posición a escribir
RTC_DATA_ATTR uint16_t rtc_logCount;     // entries válidas (satura en LOG_RING_ENTRIES)
RTC_DATA_ATTR uint32_t rtc_logDropped;   // entries perdidas por wraparound
RTC_DATA_ATTR uint8_t  rtc_logLevel;     // 0 = off
```

`LOG_RING_ENTRIES` es **compile-time** — la RTC memory no se puede redimensionar en runtime. Default propuesto: **768 entries = 6 KB**, dejando margen sobre los 8176 B para el sistema y las otras variables RTC. Si se pasa, el linker falla con `region rtc_iram_seg overflowed`, así que el error se detecta al compilar y no en campo.

**Consecuencia para la UI**: el parámetro `entries` del comando de activación sólo puede **achicar** el ring, nunca agrandarlo. Sirve para acortar dumps cuando alcanza con la historia reciente. El parámetro que de verdad mueve la ventana de captura es el **nivel**, no el tamaño.

`rtc_logDropped` no es cosmético: sin él no se puede distinguir una captura completa de una truncada, o sea no se sabe si la ventana llegó a cubrir el evento que se buscaba.

## Niveles y ventana de captura

| Nivel | Qué registra | Entries/ciclo | Captura con 768 entries |
|---|---|---|---|
| 0 | Nada (off) | 0 | — |
| 1 | Sólo anomalías | ~0,7 | **~18 h** |
| 2 | Resumen por ciclo + anomalías | ~1,7 | **~7,5 h** |
| 3 | Verboso (cada intento) | ~5 | **~2,5 h** |

Con ciclos de 60 s. El nivel 2 es el default razonable: a 17% de fallos son ~10 ciclos fallidos por hora, así que en 7 h se capturan ~70 — muchísimo más de lo necesario para separar WiFi de MQTT.

La UI debería mostrar la ventana estimada en vivo al mover el selector de nivel.

## Diccionario — el nodo es la autoridad

El backend no sabe nada del dominio: recibe `(code, a, b)` y una plantilla, y sustituye. Un X-macro genera el enum y la tabla desde **una sola definición**, así que código y texto no pueden desincronizarse ni siquiera dentro del firmware.

```c
#define LOG_CODES(X) \
    X(BOOT,        "boot #%a — reset=%b") \
    X(WIFI_TRY,    "wifi intento %a (%b = canal cacheado, 0 = scan)") \
    X(WIFI_OK,     "wifi ok — intento %a, rssi %b dBm") \
    X(WIFI_FAIL,   "wifi timeout — intento %a, status %b") \
    X(MQTT_OK,     "mqtt conectado en %b ms") \
    X(MQTT_FAIL,   "mqtt rechazado — state %b") \
    X(CMD_RX,      "comando retenido recibido — tipo %a") \
    X(PUBLISH_OK,  "telemetría publicada — %b B") \
    X(PUBLISH_FAIL,"publish falló — %b B, ¿buffer corto?") \
    X(SLEEP,       "durmiendo — %b ms despierto")
```

Las plantillas llevan `%a` y `%b`, así que el nodo define también **cómo interpretar los argumentos**, no sólo los nombres. Eso es lo que hace real el "el nodo manda".

**Caché por versión**: el diccionario se pide sólo cuando el backend ve un `firmware` que no conoce — y ese campo ya viaja en todos los payloads. En régimen no cuesta nada y se auto-repara al flashear.

**El export a disco tiene que llevar el diccionario adentro.** Si no, un archivo de hace seis meses queda ilegible cuando el firmware avanzó, que es el mismo problema de desincronización corrido en el tiempo.

### `esp_reset_reason()` en el evento BOOT

Es gratis y distingue brownout de panic de wake normal. Dada la situación solar/batería, la brownout es una hipótesis viva que hoy no se puede ni confirmar ni descartar.

## Reconstrucción de timestamps

El nodo no puede timestampear. Cada entry lleva `boot` + `ms`, y el **backend** reconstruye la hora de pared:

1. Los boots que **sí** publicaron telemetría dan anclas: el backend conoce la hora de recepción y el `boot_count` del payload.
2. Los huecos se interpolan asumiendo el `SLEEP_INTERVAL_SEC` nominal.
3. Dentro de un ciclo, `ms` da el orden y la duración exactos.

**Precisión honesta**: el timer de deep sleep corre sobre un oscilador RC interno con ±5%, así que interpolar 7 ciclos perdidos acumula ~±21 s de error. Para correlacionar logs sobra, y cada telemetría exitosa vuelve a anclar. Un RTC de hardware eliminaría todo esto — ver `aprendizajes_y_roadmap.md` → "Evolución candidata: nodo offline".

## Comandos

### Activación — `log_on`

```json
{"cmd":"log_on", "level":2, "entries":768}
```

Se procesa en `handleCommand()` como un `CommandType::LOG` nuevo. **No entra en service mode**: setea el estado en RTC, limpia el retenido en el acto, y sigue el ciclo normal (telemetría + sleep). El logging corre durante los ciclos normales de 60 s, que es todo el punto.

> Limpiar el retenido no es opcional. Es *la* clase de bug recurrente de este firmware: el loop de `reboot` y los stubs `CONFIG`/`CALIBRATE` fueron exactamente esto. Un comando que se ejecuta y no se borra se vuelve a leer en cada wake, para siempre — y encima bloquea el slot para cualquier otro comando.

`{"cmd":"log_on","level":0}` desactiva sin dumpear.

### Visibilidad

Mientras el logging está activo, la telemetría incluye `log_active` y `log_count`. Se omiten cuando está apagado, igual que los campos condicionales que ya existen — costo cero en operación normal. Sin esto es fácil activar el logging y olvidarse por meses.

No hay auto-expiración por tiempo: como capturar es gratis, la hygiene correcta es **visibilidad**, no un timer.

## Protocolo de recuperación

Topics nuevos, **sin retain**, para no pelear contra la semántica del slot único de `cmd`:

| Topic | Sentido | Payload |
|---|---|---|
| `station/01/log/req` | backend → nodo | `{"page":3}` · `{"dict":true}` · `{"clear":true,"keep":false}` |
| `station/01/log/data` | nodo → backend | `{"page":3,"pages":13,"dropped":0,"b64":"..."}` |

**Pull, no push.** El backend pide página por página. Si se pierde un chunk, volver a pedirlo es el mismo mensaje de siempre — no hace falta lógica de retransmisión. Y el ack cae solo: el backend tiene todas las páginas → recién ahí manda `clear`.

**El dump ocurre en service mode**, donde el nodo ya está despierto y suscripto. La UI puede encadenar todo detrás de un botón: entrar a service mode → pedir diccionario si hace falta → paginar → verificar → clear → salir.

**Codificación**: base64 del slice binario crudo del ring. JSON con arrays de números infla ~3×; base64 infla 1,33× y el struct es de tamaño fijo, así que cortar en páginas es aritmética simple.

Presupuesto real, contado byte por byte: el buffer MQTT son 768 B, menos header (5), largo (2) y el topic `station/01/log/data` (19) → **742 B útiles**. El wrapper JSON en su peor caso mide 77 B (`dropped` es uint32 y puede llegar a 10 dígitos), así que quedan 665 B para el base64 = 498 B binarios = 62 entries.

Se usan **55 entries por página** (440 B → 588 B de base64 → 665 B de payload, 77 B de margen), o sea **14 páginas** para un ring lleno de 768. Con 60 el margen bajaba a 25 B, y pasarse no falla ruidosamente: `serializeJson` trunca en silencio y recién después `publish()` rechaza el mensaje, dejando esa página irrecuperable por más que el backend la reintente.

### Borrado en dos fases

El nodo **no borra hasta que el backend confirma** que recibió todas las páginas. Después de horas de captura, una transferencia incompleta o una pestaña cerrada a mitad de camino no puede costar la sesión entera.

`{"clear":true,"keep":true}` trae un snapshot **sin** desactivar el logging, para investigaciones largas. El default sigue siendo desactivar y limpiar.

## Reparto de trabajo por repo

**Firmware** (`weather-station-station-iot`)
- `src/logging.{h,cpp}` nuevo: X-macro de códigos, ring en RTC, `log_write(code, a, b)`, serialización a base64 por página.
- `src/command.{h,cpp}`: `CommandType::LOG` + parseo de `level`/`entries` con clamp (mismo criterio que `timeout_min`, ver el bug de overflow ya corregido).
- `src/main.cpp`: instrumentar el camino de conexión, despachar `LOG`, agregar los campos condicionales a la telemetría.
- `src/service_mode.cpp`: atender `log/req` durante la sesión.

**Backend** (`weather-station-backend-service`)
- Suscripción a `log/data`, reensamblado por páginas, reintento de faltantes, emisión del `clear` sólo con la captura completa.
- Caché del diccionario por versión de firmware, **persistente** (si vive sólo en memoria, un reinicio del backend deja ilegible un export viejo).
- Reconstrucción de timestamps por interpolación anclada en telemetría.
- Endpoints bajo `/api/v1/logs/`, en línea con los `/api/v1/service/` que ya existen.

**Frontend** (`weather-station-frontend-dashboard`)
- Panel de logging en la vista de service mode: activar/desactivar, selector de nivel con ventana estimada, estado de captura.
- Visor con filtro por código, y export self-contained (logs + diccionario + anclas de tiempo).
- Aviso explícito de las dos cosas que pasan al traer los logs: se desactiva y se borra del nodo.

## Pendientes de decisión

- ~~**Umbral exacto de `LOG_RING_ENTRIES`**~~ — **resuelto: 768 entries (6144 B)**. Medido sobre el ELF de `production` con 768: las secciones RTC terminan en `0x50001850`, o sea **6224 B usados de los 8176 disponibles**, con **1952 B libres**. Técnicamente entrarían ~1000 entries, pero se deja el margen: los contadores de pulsos del anemómetro y el pluviómetro, que son un TODO conocido en `config.h`, también van a vivir en RTC memory.
- **Formato del export** — NDJSON ya se usa para el visor de payloads de service mode; conviene reusarlo por consistencia, pero necesita un header con el diccionario.
- **Tier de flash (SPIFFS)** — hay 320 KB declarados y sin usar en `partitions_ota.csv`. Daría días en vez de horas y sobreviviría a la pérdida total de energía, a costa de un mount por wake y riesgo de corrupción por brownout. **Decidido no hacerlo por ahora**: RTC sola resuelve el caso de uso concreto. Se reconsidera si aparece una investigación que necesite días.
