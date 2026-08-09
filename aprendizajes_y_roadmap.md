# Aprendizajes, decisiones y roadmap

> Migrado desde el proyecto legacy en la UI de Claude (Project Knowledge) el 2026-07-11. Contenido de diseño/ingeniería que no encaja en `componentes_y_conexiones.md` (estado actual de hardware) ni en `Readme.md` (operación de infra). Ver también `../STATUS.md` para el estado vivo resumido.

---

## Subsistema de viento (dirección) — en definición

**Anemómetro — resuelto y estable:**
Modelo base: [The Windicator V1](https://makerworld.com/en/models/2052859-the-windicator-v1) (MakerWorld) — diseño integrado anemómetro + veleta, 2× rodamientos 608ZZ. El eje interior es fijo; el cuerpo exterior (veleta) rota alrededor de él — **no al revés** (esto descarta cualquier approach de montaje de eje fijo para la veleta, ver abajo).

- Sensado: reed switch + 3 imanes (uno por brazo de cazoleta). Radio brazo = 130mm centro a centro.
- Debounce: 2ms (soporta hasta ~500 pulsos/s).
- Factor de calibración ≈ 0.218 m/pulso (2π × 0.130m × 0.8 eficiencia ÷ 3 imanes).
- GPIO2, interrupt.
- Diseño/calibración lista en papel — **pendiente armado mecánico y prueba en campo** (no se empezó todavía, ver `componentes_y_conexiones.md` → "Estado actual en campo").

**Dirección — AS5600 vuelve a ser candidato fuerte (actualizado 2026-07-25):**
La conclusión previa (imanes disponibles son axiales, incompatibles con el AS5600) queda corregida: el módulo AS5600 vino con un imán chico que no se había notado antes. Mau ya soldó los conectores dupont del módulo para probarlo en banco con un Arduino Mega — primero "a mano" (rotando el imán frente al sensor sin la veleta montada, mirando magnet-detected/AGC/magnitude por I2C) y después una prueba de concepto con la veleta montada. Todavía no está confirmado si ese imán es diametral o si el AS5600 tolera algo distinto de lo asumido originalmente — eso es justo lo que la prueba de banco va a determinar, no asumir el resultado.

Contexto original del problema, vigente como fallback si la prueba de banco no lo resuelve:
El approach con AS5600 (sensor de efecto Hall angular, I2C 0x36) requiere un imán **diametralmente magnetizado** (N/S en caras opuestas del disco). La conclusión anterior era que solo había imanes de magnetización **axial** disponibles (polos en las caras planas), y que conseguir uno diametral ~6mm N45 en Argentina (MercadoLibre, local) resultaba difícil. Un imán anillo con diámetro interior >8.25mm también es difícil de conseguir localmente.

**Alternativa (plan B si el AS5600 no resuelve en banco) — disco óptico Gray code:**
3× sensores IR reflectivos TCRT5000 (del kit Sunfounder "Tracking Sensor") sobre un disco con patrón Gray code. Da posición **absoluta** (8 posiciones, resolución 45°) inmediatamente al despertar de deep sleep — resuelve la limitación de posición relativa del AS5600 (que necesitaría recalibrar en cada wake). Operación pulsada (2ms cada 10s, duty cycle 0.02%) reduce el consumo promedio a ~0.018mA.

- GPIO expander PCF8574 (I2C 0x20, sin conflicto de bus) lee las 3 líneas digitales (P0–P2).
- GPIO20 controla LED_EN vía transistor BC337.
- Un segundo cable UTP (6 de los 8 pares: VCC, GND, DO1, DO2, DO3, LED_EN) va del Windicator a la caja estanca.
- Mau tiene 1 sensor TCRT5000; necesita 2 más (más fácil de conseguir localmente que el imán diametral).

---

## Aprendizajes técnicos (no cubiertos en otros documentos)

- **DHT11 (y por extensión DHT22) necesita ~1s de estabilización** después de encender Rail B antes de poder leer.
- **Boost converter → pin 5V del ESP32 es el path de alimentación correcto.** Conectar la batería directo a un LDO falló por voltaje de dropout insuficiente con una LiPo a 3.7V.
- **AS5600 no es compatible con el montaje de eje fijo del Windicator V1** — el diseño tiene el eje interior fijo y el cuerpo exterior rotando, lo inverso de lo que ese approach de montaje asume.
- **GitHub devuelve 404 (no 403) para repos privados sin autenticar** — no es una falla de red, hay que reconocerlo como "repo privado, necesita auth" en vez de reintentar la conexión.
- **Grafana: las variables "Constant"** requieren guardar el dashboard explícitamente para que el cambio se aplique.

- **Un campo nuevo en la telemetría no aparece solo en InfluxDB.** El workflow de N8N arma Line Protocol desde un `fieldMap` explícito y descarta en silencio lo que no conoce. `log_active`/`log_count` existían desde el `1.3.0` y nunca llegaron; se descubrió recién en agosto al buscar `live_seq`. Ese script **no está versionado en ningún repo** (vive sólo en el NAS). Regla: al agregar un campo al payload, **decidir explícitamente** si va a N8N en la misma tanda. Detalle en `../STATUS.md`.

  La regla es decidir, no agregar todo: lo que hay que evitar es que un campo se pierda sin que nadie lo note, no que InfluxDB tenga todo. **Caso resuelto así (2026-08-09): `next_s` NO se agregó.** Lo consume el backend leyendo el payload MQTT directo, así que la funcionalidad no depende de InfluxDB; y como dato histórico es derivable (la presencia de `live_seq` ya marca la sesión, y los deltas entre timestamps dan la cadencia real, que además es más honesta que la intención del nodo) y vale 60 en el 99% de los puntos. **Reevaluarlo cuando lleguen los tiers**: ahí `next_s` pasa a valer 60/300/1800 según el voltaje del pack y deja de ser derivable de los otros campos.

- **Comparar una magnitud con ciclo diurno entre dos firmwares exige controlar la hora del día.** Al validar el cambio de `DHT_WARMUP_MS` el delta DHT22−DS18B20 parecía haberse corrido 0,3 °C; separando por franja horaria, el firmware **sin cambiar** daba lo mismo en esa franja. Era el transitorio de enfriamiento de la tarde, donde el DHT22 (masa térmica chica) sigue la caída más rápido que el DS18B20. Es el mismo error de método que ya se había cometido con la posición del sniffer WiFi, en otra dimensión.

- **El rollback de OTA está compilado pero desarmado, y `esp_ota_mark_app_valid_cancel_rollback()` no hace lo que parece.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` en el sdkconfig del core, o sea que una imagen recién flasheada arranca en `ESP_OTA_IMG_PENDING_VERIFY` y el bootloader la revertiría si nadie la valida. Pero el core de Arduino la valida solo: `initArduino()` (en `esp32-hal-misc.c`, antes de `setup()`) llama a `verifyOta()` —función *weak* que devuelve `true` por defecto— y marca la imagen válida. **Toda imagen que bootee se acepta sin chequear nada.** Además, la llamada que había en `ArduinoOTA.onEnd()` corría en el firmware *viejo* y marcaba válida la partición vieja, no la nueva; se eliminó.

  Lo que sí sigue protegido sin hacer nada: un flash **cortado a la mitad** deja una imagen que no pasa la validación del bootloader, y arranca la partición anterior. El agujero es la imagen **completa pero rota**, que bootea y se acepta.

  **Evaluado a fondo el 2026-07-27 — decisión: no se implementa por ahora.** El análisis quedó acá porque la conclusión no es obvia y no conviene re-derivarla.

  **`verifyOta()` es la palanca equivocada**, al contrario de lo que decía la nota original de este mismo bullet. Ese hook corre dentro de `initArduino()`, *antes* de `setup()`: sólo puede juzgar con lo que se sabe en el primer arranque de la imagen, cuando todavía no corrió nada del firmware. Un chequeo de I2C ahí responde "¿está el hardware?", no "¿sirve este firmware?". La palanca real es la otra función weak del mismo bloque, **`verifyRollbackLater()`**: devolviendo `true` el core saltea la validación entera y la decisión queda del lado de la aplicación, para tomarla más tarde con el runtime ya levantado. Ninguna de las dos está declarada en un header — son símbolos C, hay que definirlas con `extern "C"`.

  **El bloqueante que parecía haber, no existe.** `esp_ota_begin()` devuelve `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` si la imagen corriendo está en `PENDING_VERIFY`, lo que dejaría una ventana de OTA abierta pero inútil. No aplica: el `Update` de Arduino no pasa por `esp_ota_begin()` — usa `esp_ota_get_next_update_partition()` + escritura cruda de partición + `esp_ota_set_boot_partition()` (`Updater.cpp`), y ese último no tiene ese código de error. **Con la imagen sin confirmar, el OTA sigue funcionando**, así que diferir la validación es seguro.

  **El criterio de salud no debería ser I2C** sino "la imagen llegó a abrir su propia ventana de OTA", con "llegó a dormir limpio" como red de contención. Las dos propiedades son locales y **un fallo de WiFi cuenta como sano**, que es lo que evita el falso positivo con el ~17% de ciclos que no conectan. Habría que confirmar en cinco puntos: después de `_setupOTA()` en `serviceMode_run()`, en `goToDeepSleep()`, en `serviceMode_exit()` (que llama a `esp_deep_sleep()` directo) y antes de los dos `ESP.restart()` — sin este último, un comando `reboot` durante la ventana sin confirmar dispararía un rollback. La llamada tiene que ir guardada detrás de un `esp_ota_get_state_partition() == ESP_OTA_IMG_PENDING_VERIFY` explícito: sin eso, cada deep sleep podría escribir en `otadata`, que son ~1440 escrituras/día sobre una partición de 8 KB.

  **Por qué no se implementa igual**: de los bugs reales que aparecieron en este firmware — loop de `reboot`, timeout de service mode que no acotaba, overflow del buffer MQTT, warmup del DHT que nunca corría — el rollback no habría atrapado **ninguno**; todos completaban un ciclo limpio. Sólo cubre "crashea antes de abrir el OTA", y como el post-OTA reintenta service mode en cada boot, la ventana real de brickeo es angosta (un crash en `Wire.begin()` o `connectWiFi()`). A cambio agrega un modo de falla insidioso: si un camino de salida nuevo se olvida de confirmar, el nodo revierte firmware solo, semanas después. El argumento a favor sigue siendo válido y es no-técnico — el plan B de un brickeo es subir al techo y **abrir la caja estanca**, el mismo sellado que se monitorea con el DHT22 — así que esto se reconsidera si el firmware se vuelve más riesgoso de flashear.

  **Sin verificar**: el sdkconfig tiene `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=1` y no se pudo determinar offline si el chequeo de `PENDING_VERIFY` del bootloader corre al despertar de deep sleep (el paquete de PlatformIO trae sólo headers, no las fuentes del bootloader). El diseño de arriba está armado para no depender de eso, pero si se implementa hay que confirmarlo en banco.

(Los aprendizajes sobre I2C/pull-ups externos, `system_mW` no siendo consumo real, cold solder joints, InfluxDB `:2` fijo, y el drop-in de systemd para NFS ya están documentados en `componentes_y_conexiones.md`, `../i2c-bus-lockup-investigation.md` y `Readme.md` respectivamente.)

---

## Herramientas y recursos

- **Desarrollo:** VSCode + PlatformIO. Build flags ESP32-C3 SuperMini: `ARDUINO_USB_CDC_ON_BOOT=1`, `ARDUINO_USB_MODE=1`.
- **Pipeline de datos:** ver `Readme.md` → "Pipeline completo de datos".
- **Referencias de hardware:** kit Sunfounder (DHT11, DS18B20, raindrop sensor, fotoresistor, reed switch, TCRT5000 "Tracking Sensor", PCF8591 ADC); módulo MPPT CN3791; transistores NPN BC337; expansor I2C PCF8574; conectores Wago 221 lever; conectores JST XH; prensaestopas PG-7 y PG-9.
- **Sourcing:** Embalse, Córdoba, Argentina — ferreterías locales para mecánica, MercadoLibre para electrónica. Algunos componentes (imanes diametrales, imanes anillo) son difíciles de conseguir localmente.
- **Impresión 3D:** Mau no tiene impresora propia — depende de un familiar en Buenos Aires para las piezas impresas, lo que agrega tiempo de espera a cualquier pieza nueva.

---

## Roadmap — "en el horizonte"

Reconciliado con el estado real al 2026-07-11 (el snapshot original venía del proyecto legacy en la UI y estaba desactualizado en varios puntos — marcado abajo).

- ✅ **Armado físico del prototipo v1** — hecho (dos perfboards, con cold solder joints ya en gran parte resueltos). El snapshot original lo listaba como pendiente; ya no lo está.
- 🔄 **PCB v2 (FR4 casera, transferencia de tóner + cloruro férrico)** — en fabricación. Main + Aux perforadas, soldadura en curso. Pendiente: terminar de soldar, limpiar flux, enmascarar headers hembra, aplicar flux protector + barniz dielectrico, curar, recién ahí instalar módulos enchufables (INA219, ESP32). Ver `componentes_y_conexiones.md` → "PCB v2 — en fabricación".
- ✅ **Perforación de la caja estanca** (ventana PET para fotoresistor, prensaestopas) — hecha, sistema desplegado y operativo.
- ✅ **Backend + frontend custom** — hecho: `weather-station-backend-service` (Go) y `weather-station-frontend-dashboard` (React/Vite), ya no es "un amigo capaz hace el frontend" como decía el snapshot original.
- ⏳ **Resolución de dirección de viento** — AS5600 vuelve a ser candidato fuerte (imán chico encontrado en el módulo, no notado antes); en validación de banco con Arduino Mega desde 2026-07-25. Óptica (2 sensores TCRT5000 más) sigue como plan B.
- ⏳ **Armado mecánico del Windicator** (anemómetro + veleta) — impreso en PLA, pendiente de armar y probar en campo.
- ❓ **Montaje del panel solar** (placa de respaldo policarbonato/fenólico, sellado con silicona, bracket a 45°) — estado sin confirmar en esta sesión; el sistema ya reporta datos solares (INA219 solar) así que probablemente esté al menos parcialmente instalado.
- ❓ **Mejora del Stevenson screen** (protección lateral contra sol, ventilación forzada) — estado sin confirmar. Se sabe que el wrap de aluminio en el mástil redujo contaminación térmica (2.04°C de mejora confirmada por análisis de datos) pero generó un efecto espejo que refleja radiación hacia el BMP180.
- ⏳ **Modos adaptativos de firmware** (día/noche por voltaje solar, tiers de energía por voltaje de batería) — **verificado 2026-07-29: no está implementado.** `src/battery.h` existe pero está vacío (0 bytes), `SLEEP_INTERVAL_SEC` es fijo, y `sensors_railsOn()` enciende los dos rails incondicionalmente (el TODO sigue ahí). Los umbrales de `componentes_y_conexiones.md` son diseño en papel. **Corrección de premisa**: los tiers no ahorran en deep sleep — GPIO7/GPIO8 no son RTC GPIOs en el ESP32-C3, así que al dormir quedan sin drive y el pull-down de 9,9 kΩ corta ambos rails solo. El ahorro real está en T3/T4, que cambian el intervalo de sleep.
- ✅ **Bajo consumo — dos fixes en campo (2026-07-29)**: INA219 en power-down entre ciclos (`1.4.0`) y warmup del DHT22 en paralelo con la red (`1.5.0`). Ver `../STATUS.md` → "Power management".
- ⏳ **Calibración de lluvia en Grafana** — `rain_wet_ref=0.3` sigue siendo un placeholder, falta dato real de lluvia intensa.
- ⏳ **Medir el consumo de reposo con multímetro** — ~3,2 mA de los 5,10 mA totales están sin explicar (63% del presupuesto). No hay firmware que lo resuelva: el INA219 se apaga al dormir. **Es la medición de mayor valor pendiente.** Ver `componentes_y_conexiones.md`.
- ⏳ **Integrar la energía de la ventana activa en el firmware** (`active_mAs` + `awake_ms` en el payload, promediado por hardware del INA219) — acordado 2026-08-02, sin empezar. Ver la sección dedicada más abajo. Hacerlo **después** de medir el reposo.
- ❓ **Alcance de WiFi** — faltaban ~5m de cobertura; se subió la potencia de transmisión del TP-Link AX3000 a "High" como primer intento, resultado sin confirmar en esta sesión.

---

## Medir la energía de la ventana activa en vez de una muestra suelta (acordado 2026-08-02, sin empezar)

**Qué está mal hoy.** `system_mW` es **una** lectura del INA219 de 532 µs tomada en un punto arbitrario de la ventana despierta de ~2,3 s. Cae aleatoriamente dentro o fuera del burst de transmisión de WiFi, así que es un pico y no un promedio — cubre el **0,02%** de la ventana. Sirve para diagnosticar el sag; no sirve para presupuestar energía.

Eso rompió cosas río abajo: el balance energético del dashboard restaba ese pico (continuo en apariencia) contra la potencia del panel, que sí es continua, y daba déficit el 56% del día con buena luz. Se resolvió del lado del backend cambiando el medidor por un estado derivado (ver `../STATUS.md` → 1.2.0), pero **el dato crudo sigue siendo un pico**.

### Por qué va en el nodo y no en el backend

La regla del proyecto es que el cómputo vive en el backend — el punto de rocío se calcula ahí, no en el nodo, aunque el ESP32 podría. **Esto no la viola**, y la distinción importa:

- El punto de rocío es **derivación**: temperatura y humedad viajan enteras en la telemetría, así que el backend tiene toda la información. Se calcula ahí para poder corregir la fórmula sin reflashear.
- La energía de la ventana activa es **adquisición**: la dinámica sub-segundo del burst **no existe en la telemetría**. El nodo publica una muestra; ninguna lógica en el backend recupera lo que nunca se transmitió.

Enunciada mejor, la regla es **el nodo es dueño de la adquisición, el backend de la interpretación**. Integrar al ritmo de muestreo que sólo el nodo tiene es adquisición — la misma categoría que "leer el INA219", sólo que 30 veces en vez de una.

(Nota: el nodo ya calcula `pressure_qnh`, que sí es derivación pura. La regla ya se aplica con flexibilidad.)

### Qué publicar

**La integral y la ventana por separado, no el promedio ya masticado:**

| campo | qué es |
|---|---|
| `active_mAs` (o mWh) | carga/energía integrada sobre la ventana despierta |
| `awake_ms` | cuánto duró esa ventana |
| `system_mW` | **se mantiene** — el instantáneo sigue sirviendo para el sag |

Con esos dos el backend calcula promedio, ciclo de trabajo y mWh/día, y puede cambiar de criterio sin reflashear. Si el nodo manda sólo "el promedio", se pierde la capacidad de recalcular — que es justo la razón por la que el punto de rocío vive en el backend.

**Beneficio concreto**: hoy el ciclo de trabajo (3,6%) es una constante hardcodeada que ya cambió entre firmwares, y por eso el backend se negó a usarla para prorratear. Con `awake_ms` en el payload esa corrección pasa a ser honesta.

Presupuesto de payload: hay ~170 B de margen sobre 741 B, así que entran de sobra.

### Promediado por hardware del INA219

El registro de configuración (`0x00`) tiene dos campos de ADC independientes, **BADC** (bits 10-7, bus) y **SADC** (bits 6-3, shunt), con la misma codificación:

| bits | modo | conversión |
|---|---|---|
| `0011` | 12 bit, 1 muestra | 532 µs ← **lo que corre hoy** |
| `1101` | 32 muestras | 17,02 ms |
| `1111` | **128 muestras** | **68,10 ms** |

El chip promedia internamente; se lee un registro y sale la media. Cero costo de CPU.

**Lo que lo vuelve realmente útil**: en modo continuo el INA219 convierte de corrido. Leyendo cada ~70 ms durante los 2,3 s despierto salen **~33 promedios consecutivos que no se solapan y cubren casi el 100% de la ventana**. Eso no es "tomar varias muestras": es una integral completa.

**Obstáculos conocidos**, en orden de fastidio:

1. `Adafruit_INA219 @ 1.2.1` **no expone el promediado**. Los `setCalibration_*()` arman el registro con `12BIT_1S_532US` hardcodeado. Hay que escribir el registro `0x00` por `Wire` **después** de llamar a `setCalibration_*()`, o el calibrado lo pisa. El header define constantes tipo `INA219_CONFIG_SADCRES_12BIT_128S_69MS` — **verificar los nombres exactos en el header instalado**, no están confirmados.
2. Durante esa ventana el ESP32 está ocupado con WiFi. Un loop bloqueante de 2,3 s no es viable. Salidas: leer en los puntos de transición (antes de WiFi, tras conectar, tras publicar, antes de dormir) acumulando `P × Δt`, o disparar las lecturas desde un timer.
3. Subir el tiempo de conversión alarga el `sensors_read()`; medir el impacto sobre la ventana despierta, que es justamente lo que se está tratando de no inflar.

### ⚠️ Advertencia de prioridad

Esto instrumenta **44,7 mAh/día** (la ventana activa). Los otros **77,7 mAh/día** — el 63% del presupuesto — están sin instrumentar y sin explicar; ver `componentes_y_conexiones.md` → "El reposo pesa más que la ventana activa". Y el INA219 **no puede** medirlos, porque el propio firmware lo pone en power-down al dormir.

**Antes de encarar esto conviene medir el reposo con multímetro.** Es cinco minutos y contesta la mitad grande del presupuesto.

---

## Evolución candidata: nodo "offline" con RTC + almacenamiento local (idea de Mau, 2026-07-27)

Surgida mientras se diseñaba el sistema de logs. La idea: un nodo que **no depende de la red** — recolecta datos, los guarda localmente con timestamp real, y el operador se acerca cada muchos días o meses a descargarlos por Bluetooth. Caso de uso concreto: Mau tiene un terreno sin servicios donde se podría instalar uno.

Es un cambio de categoría, no una feature: el nodo pasa de "sensor conectado" a "data logger con sincronización ocasional". No aplica al nodo actual — aplica a una placa v3.

### El premio real es energético, no de conectividad

En el nodo actual el WiFi domina todo: **3,3 s despierto** a 80-140 mA por ciclo (medido en campo el 2026-07-28; la cifra de "~10 s" que figuraba acá estaba mal por 3×). Un ciclo sin red —despertar, leer sensores, escribir a almacenamiento local, dormir— es del orden de 1 s a ~20 mA, y encima se ahorra la espera del comando retenido. Es más de un orden de magnitud de carga por ciclo, no los dos que decía la estimación original. Eso es lo que habilita el "ultra low power" que mencionaba la idea: meses de autonomía, o el mismo tiempo con un panel mucho más chico. La descarga ocasional por BLE se paga una vez cada muchos días, no 1440 veces por día.

### RTC con pila propia — encaja en el nodo actual

- **No consume GPIOs**: es I2C, cuelga del bus que ya existe (donde ya están los dos INA219). Un DS3231 vive en 0x68, sin conflicto con 0x40/0x41.
- **Cuidado con el consumo**: un DS3231 alimentado consume ~100-200 µA, contra un objetivo de deep sleep de <50 µA. Puesto en el rail always-on arruinaría el presupuesto por 4×.
- **La topología correcta ya existe en la placa**: colgarlo de un **rail conmutado** (Rail A). Mantiene la hora con su pila de botón mientras está sin alimentar, y se energiza sólo durante el wake para leerlo. Costo en deep sleep ≈ 0. Es exactamente lo que la placa ya hace con los otros sensores.
- **A verificar antes de comprar**: en el DS3231 la interfaz I2C se deshabilita cuando corre desde Vbat. Para este uso da igual (sólo se lee con Vcc presente), pero hay que confirmarlo en el datasheet.
- **Beneficio secundario que no es menor**: el timer de deep sleep del ESP32 corre sobre un oscilador RC interno con ±5% de precisión — sobre un día son ±72 min de deriva. Un RTC da alineación a hora de pared real, y de paso vuelve innecesario el esquema de interpolación de timestamps del sistema de logs.

### microSD — bloqueada por pines en la placa actual

**No entra.** Sólo quedan libres **GPIO20 y GPIO21** (ver `componentes_y_conexiones.md` → "GPIOs asignados"; GPIO9 es el botón BOOT físico y no se puede usar). SPI necesita 4 pines (MOSI, MISO, SCK, CS) y SDIO en modo 1-bit necesita 3 más CS. Encima GPIO20 ya está pre-asignado al `LED_EN` del PCF8574 si se va por la vía óptica para dirección de viento.

Además, aun con pines disponibles: las escrituras a SD son ráfagas de 50-100 mA y las tarjetas son notoriamente frágiles ante cortes de energía a mitad de escritura — mal match para un nodo solar.

**Alternativa que sí entra sin tocar un solo pin**: una FRAM o EEPROM I2C, colgada del mismo bus. Una FM24CL64 (8 KB de FRAM) tiene endurance prácticamente infinita (10¹² ciclos) contra el millón de una EEPROM; una 24LC512 da 64 KB. A ~50 B por muestra, 64 KB son ~1300 muestras: 9 días a una muestra cada 10 min, o ~54 días a una por hora. Para el nodo offline probablemente haya que ir a algo más grande o encadenar varios chips, pero la topología es la correcta y el costo en GPIOs es cero.

### Bluetooth para la descarga

El ESP32-C3 tiene BLE 5.0, así que el nodo actual ya tendría la radio. Lo interesante es del lado del operador: **Web Bluetooth desde Chrome en Android permitiría reusar el dashboard React que ya existe**, sin escribir una app nativa. Limitación conocida: Web Bluetooth no funciona en Safari/iOS.

Números para dimensionar: 6 meses a una muestra cada 10 min son ~26.000 muestras ≈ 1,3 MB. A la velocidad realista de BLE en el C3 (~20 KB/s) es alrededor de un minuto de descarga. Perfectamente viable.

### Lo que habría que resolver del lado del backend

El pipeline actual asume datos **en vivo**. Ingerir un lote de muestras de hace seis meses es un camino distinto: hay que aceptar backfill histórico, deduplicar contra lo que ya se importó, y decidir qué pasa si el reloj del nodo derivó o se reinició. No es difícil, pero es trabajo que hoy no existe.

---

## Primera captura de logs en campo (2026-07-28) — el ~17% de ciclos perdidos, resuelto

Primera corrida real del sistema de logs contra el nodo con firmware `1.3.0`: **177 eventos, 30 ciclos, nivel 3 (verboso), 0 eventos pisados**. Esto cierra la pregunta que motivó todo el sistema. Los números de acá salen de esa captura; no hace falta re-derivarlos.

### El fallo no es WiFi

**WiFi asoció en el primer intento las 30 veces**, incluso con la señal degradada: 30 `LOG_WIFI_TRY`, 30 `LOG_WIFI_OK`, **cero** `LOG_WIFI_FAIL` y cero `LOG_WIFI_GIVEUP`. La hipótesis anterior —"el nodo no se asocia por señal marginal"— queda descartada como mecanismo.

Lo que falla es la capa **TCP/MQTT**, y correlaciona fuerte con el RSSI:

| Ventana | RSSI | Ciclos que intentaron publicar | Fallos | `mqtt.connect()` |
|---|---|---|---|---|
| Degradada | **-73 dBm** | 5 | **3 (60%)** | 399 / 412 / 550 ms, más 2 timeouts |
| Normal | -63 a -66 dBm | 21 | **0** | 32–69 ms |

El código de error es `state -4` = `MQTT_CONNECTION_TIMEOUT` (`PubSubClient.h:45`). El mecanismo: el handshake MQTT pasa de **~40 ms a 2400–3200 ms** cuando la señal se degrada —un factor de 60×— y a veces cruza el socket timeout de 5 s, perdiendo el ciclo entero. Tasa global en esta captura: 3 de 26 = 11,5%, consistente con el ~17% histórico.

El `LOG_PUBLISH_FAIL` que apareció decía "¿buffer corto?" con **505 B contra 741 disponibles**: fue una conexión caída a mitad del publish. Ya corregido — la entry ahora distingue las dos causas en su argumento `a`.

### El enlace oscila ~9 dB solo por el ambiente

Confirmado por Mau: entre la ventana degradada y las normales **no hubo ningún cambio de configuración**. La diferencia fue ambiental — una persona moviéndose, una puerta abriéndose. Son 9 dB, casi un orden de magnitud de potencia.

El router (TP-Link Archer) **ya está con la potencia de transmisión al máximo**, y ese fue un parche necesario: en "medium" el nodo directamente no lograba conectarse. O sea que no queda margen de configuración del lado del AP, y la condición marginal no es un caso raro sino el régimen normal de operación.

### El tiempo despierto es 3,3 s, no ~10 s

La cifra de "~10 s despierto" que circulaba por los documentos **estaba mal por 3×**. Mediana sobre 20 ciclos sanos:

| Tramo | Mediana | % del despierto |
|---|---|---|
| boot → primer intento de WiFi | 73 ms | 2% |
| WiFi asociado | 201 ms | 6% |
| MQTT conectado | 40 ms | 1% |
| **red lista → publish** | **3048 ms** | **92%** |
| **Total** | **3300 ms** | |

Es decir: **la red está lista a los ~314 ms y el nodo se queda 3 segundos más con el WiFi asociado y consumiendo**, esperando el comando retenido y el warmup del DHT22. Eso invierte el peso de dos cosas que estaban anotadas con otras proporciones:

- Los **800 ms de `waitForRetainedCommand`** figuraban como "~8% del tiempo despierto". Son **24%**.
- Los **2 s de warmup del DHT22** son el **61%** de toda la ventana despierta.

### Pendientes que salieron de esto

**1. Reintentar `mqtt.connect()` en el ciclo normal.** ⏳ Pendiente, para la sesión dedicada de MQTT. Hoy hay un único intento en `connectMQTT()`: si da timeout, `main.cpp` va directo a `goToDeepSleep()` y el ciclo se pierde. Con WiFi ya arriba —la parte cara, y que nunca falla— y sólo ~800 ms gastados, un segundo intento cuesta a lo sumo otro socket timeout y salva el ciclo. Service mode ya tiene `SERVICE_MODE_MQTT_RETRIES`; el ciclo normal no tiene nada equivalente.

> Ojo con dimensionar el impacto: la segunda captura mostró que el fallo de `connect()` explica ~1,5% de los ciclos, no el grueso de la pérdida. Ver "Segunda captura" abajo.

**2. Mover el rail-on del DHT22 al inicio de `setup()`.** ✅ **Hecho y medido — firmware `1.5.0`, 2026-07-29.** El despierto bajó de 3300 a **2200 ms** (publish absoluto 3365 → 2292 ms), o sea −1073 ms / −33%, ≈ −22 mAh/día. Verificado sobre 116 ciclos sanos; los demás tramos del ciclo quedaron dentro de milisegundos del baseline. Detalle en `../STATUS.md` → "Power management". Efecto secundario importante: el warmup del DHT22 pasó a ser el término que fija el piso del despierto, lo que **anula** el pendiente de acortar `MQTT_RETAINED_WAIT_MS` — documentado en `src/config.h`.

**3. Revisar el presupuesto de reintentos de WiFi.** ⏳ Pendiente. `WIFI_MAX_RETRIES` 3 × `WIFI_TIMEOUT_MS` 15 s da un peor caso de 45 s que está documentado como el costo de un ciclo fallido — pero **nunca ocurrió**: los ciclos fallidos costaron 5,2–6,2 s, y todos fallaron en MQTT, no en WiFi. El presupuesto está dimensionado para un modo de falla que no es el real. Antes de recortarlo conviene capturar una ventana con señal peor todavía, para no optimizar contra una muestra de una sola noche.

---

## Segunda captura de logs en campo (2026-07-29) — `LOG_PUBLISH_OK` no prueba entrega

68 ciclos sobre `1.3.1`, nivel 3, 0 pisados, sin huecos de `boot_count`. Confirmó los tiempos de la primera captura casi exactamente (red lista a 322 ms, red→publish 3047 ms, despierto 3300 ms) y **corrigió la conclusión principal**.

**El número real de pérdida es 42%, no 17%.** 67 ciclos registraron `LOG_PUBLISH_OK` y sólo 39 llegaron. Cruzado contra **dos consumidores independientes** —el backend Go por MQTT directo e InfluxDB vía N8N— que coinciden exactamente en qué llegó: dos caminos separados perdiendo los mismos mensajes ⇒ nunca llegaron al broker.

**El aprendizaje transferible**: `PubSubClient::publish()` devuelve `true` cuando la escritura al socket tuvo éxito. En QoS 0 no hay ack posible, así que eso **no** es evidencia de entrega. El nodo cierra el socket en el mismo milisegundo y apaga la radio 200 ms después; si el segmento TCP necesita retransmisión (RTO del orden de 1-3 s) muere en silencio. Cualquier instrumentación futura que quiera afirmar "se publicó" necesita QoS 1 con PUBACK, o un consumidor que confirme.

**No correlaciona con RSSI**: llegados vs. perdidos tienen distribuciones idénticas de señal, de tiempo de `connect()`, de tamaño de payload y de tiempo despierto.

**Tercera captura (`1.5.0`, 119 ciclos) — dos refinamientos, uno de ellos una corrección:**

- La pérdida se mantiene en **38%**, y el RSSI barrió **10 dB** (-71 a -61) con correlación **cero** (mediana -67 en ambos grupos). La salvedad de la banda angosta queda contestada: no es intensidad de señal.
- **Corrección**: la conclusión de "pérdida independiente y sin memoria" **era incorrecta** — salió de comparar rachas contra una geométrica sobre sólo 67 muestras, donde la desviación quedaba dentro del ruido. Con 112 pares consecutivos, la pérdida **alterna** de forma significativa: `P(perdido | anterior perdido) = 0,279` contra `P(perdido | anterior llegó) = 0,470`, chi² = 3,96 (p < 0,05). **Lección de método**: no declarar "independiente" desde un ajuste de rachas con n chico — el test de correlación a lag 1 tiene mucho más poder y es igual de barato.

**Hipótesis principal (sin confirmar)**: el keepalive de 60 s deja la sesión vieja viva 90 s, más que el ciclo de ~63 s del nodo, así que cada reconexión fuerza un takeover por client-ID duplicado — mecanismo que **predice la alternancia observada**, cosa que la radiofrecuencia no puede. El análisis completo, el experimento de una línea (keepalive corto en el ciclo normal, largo sólo en service mode), los comandos de verificación y las vías alternativas están en `../STATUS.md` → "Pérdida de telemetría".
