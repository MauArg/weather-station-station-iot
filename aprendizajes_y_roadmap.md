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

**Dirección — problema abierto:**
El approach original con AS5600 (sensor de efecto Hall angular, I2C 0x36) requiere un imán **diametralmente magnetizado** (N/S en caras opuestas del disco). Los imanes disponibles son de magnetización **axial** (polos en las caras planas) — confirmado incompatible por prueba física. Conseguir un imán diametral ~6mm N45 en Argentina (MercadoLibre, local) está resultando difícil. Un imán anillo con diámetro interior >8.25mm también es difícil de conseguir localmente.

**Alternativa preferida — disco óptico Gray code:**
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
- ⏳ **Resolución de dirección de viento** — sigue abierto: conseguir imán diametral (AS5600) o 2 sensores TCRT5000 más (vía óptica, preferida actualmente).
- ⏳ **Armado mecánico del Windicator** (anemómetro + veleta) — impreso en PLA, pendiente de armar y probar en campo.
- ❓ **Montaje del panel solar** (placa de respaldo policarbonato/fenólico, sellado con silicona, bracket a 45°) — estado sin confirmar en esta sesión; el sistema ya reporta datos solares (INA219 solar) así que probablemente esté al menos parcialmente instalado.
- ❓ **Mejora del Stevenson screen** (protección lateral contra sol, ventilación forzada) — estado sin confirmar. Se sabe que el wrap de aluminio en el mástil redujo contaminación térmica (2.04°C de mejora confirmada por análisis de datos) pero generó un efecto espejo que refleja radiación hacia el BMP180.
- ❓ **Modos adaptativos de firmware** (día/noche por voltaje solar, tiers de energía por voltaje de batería) — `componentes_y_conexiones.md` ya documenta los umbrales de tiers como diseño vigente; no se re-verificó línea por línea contra el firmware actual en esta sesión.
- ⏳ **Calibración de lluvia en Grafana** — `rain_wet_ref=0.3` sigue siendo un placeholder, falta dato real de lluvia intensa.
- ❓ **Alcance de WiFi** — faltaban ~5m de cobertura; se subió la potencia de transmisión del TP-Link AX3000 a "High" como primer intento, resultado sin confirmar en esta sesión.
