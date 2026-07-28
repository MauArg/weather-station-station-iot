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
- ❓ **Modos adaptativos de firmware** (día/noche por voltaje solar, tiers de energía por voltaje de batería) — `componentes_y_conexiones.md` ya documenta los umbrales de tiers como diseño vigente; no se re-verificó línea por línea contra el firmware actual en esta sesión.
- ⏳ **Calibración de lluvia en Grafana** — `rain_wet_ref=0.3` sigue siendo un placeholder, falta dato real de lluvia intensa.
- ❓ **Alcance de WiFi** — faltaban ~5m de cobertura; se subió la potencia de transmisión del TP-Link AX3000 a "High" como primer intento, resultado sin confirmar en esta sesión.
