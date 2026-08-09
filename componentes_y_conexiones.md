\# Estación Meteorológica Solar — Componentes y Conexiones

\*\*Embalse, Córdoba, Argentina · Rev 2.0\*\*

&#x20;

\---

&#x20;

\## Estado actual en campo (actualizado 2026-07-31)

&#x20;

\*\*Hardware en campo: PCB v2 (main + aux), con doble capa de barniz aislante aplicada.\*\* El prototipo de dos perfboards con cold solder joints ya no está en servicio, y el issue de I2C intermitente quedó cerrado — ver `../STATUS.md`.

&#x20;

\*\*Firmware en campo: `1.15.0`\*\* (el `1.16.0` está commiteado, sin flashear). Historial reciente: `1.1.0` (DHT22) → `1.2.0` (service mode + fix del loop de reboot) → `1.3.0`/`1.3.1` (sistema de logs) → `1.4.0` (INA219 en power-down) → `1.5.0` (warmup del DHT22 en paralelo con la red) → `1.6.0`-`1.13.1` (investigación de pérdida de telemetría: keepalive, timeouts, y sobre todo `WIFI_FORCE_11B`, que llevó la pérdida de 39% a 0,3%) → `1.13.2` (limpieza a inglés) → `1.14.0` (warmup del DHT22 2000→1300 ms) → `1.15.0` (live mode) → `1.16.0` (heartbeat de live). Ver `../STATUS.md` para el detalle de cada uno.

&#x20;

\*\*Consumo medido (2026-08-08/09):\*\* ciclo normal \~47 mAh/día de ventana activa — es un \*\*piso\*\*, porque `system_mA` se muestrea con WiFi asociado pero ocioso y no captura la ráfaga de asociación. \*\*Live mode: \~53 mAh por hora\*\*, o sea que una hora de live cuesta como 1,1 días de operación normal. El consumo en deep sleep sigue \*\*sin medir\*\*.

&#x20;

Sistema desplegado y funcionando al aire libre, en el fondo del terreno (lote angosto 12×35m, casa al extremo sur, frente de calle al norte), a la altura especificada para mediciones meteorológicas estándar. Ubicación GPS exacta no documentada todavía en ningún archivo del proyecto.

&#x20;

\*\*Sensores activos:\*\*
\- SHT31-D + BMP180 — en los platos exteriores de la Stevenson screen ✅
\- Rain sensor — montado justo arriba de la caja estanca, con leve inclinación ✅
\- Fotoresistor — dentro de la caja estanca, mirando por una ventanita cortada en la caja para tomar luminosidad ✅
\- DS18B20 (temp\_sistema) — mide temperatura de la caja estanca. ⚠️ falla intermitente (ver abajo)
\- INA219 sistema + INA219 solar + módulo MPPT CN3791 — todos dentro de la caja estanca, cargando la batería. ⚠️ INA solar falla intermitente (ver abajo)

&#x20;

\*\*Removido / no instalado:\*\*
\- AS5600 (veleta) — nunca se conectó. JST5 sigue sin nada conectado. No se empezaron las pruebas de veleta ni anemómetro en campo (el diseño/calibración de "The Windicator V1" existe en papel, pendiente de instalación física).

&#x20;

\*\*Reemplazado y en servicio:\*\*
\- DHT11 → DHT22 — el DHT11 murió (falla crónica, venía fallando ya en proyectos anteriores a esta estación). \*\*2026-07-25:\*\* reemplazo físico completado — se desoldó el DHT11 del módulo Sunfounder y se soldó en su lugar el sensor DHT22 pelado, reutilizando el PCB del módulo (mismo pull-up y filtro ya incluidos ahí, mismo orden de pines VDD-DATA-NC-GND y mismo paso de 2.54mm que el DHT11, mismo conector JST y pin GPIO0 de la placa principal — sin cambios de cableado ni de PCB). Documentado con fotos antes/después del desoldado. \*\*Flasheado y validado en campo el 2026-07-25 — issue cerrado.\*\* Los campos MQTT conservan el nombre histórico `dht11_*` a propósito, para no partir la serie en InfluxDB. Detalle en `../STATUS.md`.

&#x20;

\*\*Sobre el issue de I2C (ver también `../i2c-bus-lockup-investigation.md`):\*\* la causa real no fue el AS5600 (nunca estuvo conectado) sino cold solder joints en las dos perfboards del primer prototipo. Resoldar varias juntas mejoró muchísimo el problema — antes era mucho más crítico (bus I2C se caía random, afectaba INA y más sensores). Ahora solo fallan intermitentemente el INA219 solar y el DS18B20, probablemente por juntas frías remanentes sin resoldar todavía.

&#x20;

\---

&#x20;

\## PCB v2 — en fabricación (actualizado 2026-07-11)

&#x20;

Versión definitiva en desarrollo para reemplazar el prototipo de dos perfboards de arriba y resolver el problema de cold solder joints. **Corresponde a los layouts documentados en este archivo** (secciones "Placa principal — 31×26 holes" y "Placa auxiliar JST — 24×10 holes" más abajo) — diseñados en Fritzing (`Main PCB - ... IoT_v1.4.fzz`, `Aux PCB - ... IoT_v1.2.2.fzz`, raíz de este repo) y fabricados como PCB FR4 simple faz caseras, por transferencia de tóner + cloruro férrico. Los exports de fabricación (capas etch/mask/silk, por intento) están en `PCB/`.

&#x20;

\*\*Estado del armado:\*\* ambas placas (main + aux) perforadas, soldadura de componentes en curso — no terminada todavía.

&#x20;

\*\*Protección contra humedad/estrés térmico — EJECUTADA, doble capa de barniz aplicada.\*\* El procedimiento que se siguió (antes de instalar los módulos enchufables): terminar de soldar todos los componentes → limpiar residuos de flux con alcohol isopropílico → enmascarar con cinta los headers hembra donde van INA219/ESP32 (para que el barniz no llegue a los contactos) → aplicar Flux Protector (ContactFlux) y barniz dielectrico (Delta) sobre toda la placa ya soldada → dejar curar → recién ahí insertar los módulos en los headers.

&#x20;

Importa para interpretar la telemetría: el DHT22 de la caja quedó como monitor de integridad del sellado, y cuando marca humedad alta adentro la pregunta ya no es si la electrónica está expuesta —está barnizada— sino si entró agua que hay que sacar. Ver `../STATUS.md` → "Humedad dentro de la caja".

&#x20;

\---

&#x20;

\## Hardware principal

&#x20;

| Componente | Modelo | Notas |

|---|---|---|

| Microcontrolador | ESP32-C3 SuperMini | 5V via boost, 3.3V I/O |

| Sensor T+H exterior | SHT31-D | I2C 0x44, en plato exterior |

| Sensor presión | BMP180 | I2C 0x77, en plato exterior |

| Monitor corriente solar | INA219 | I2C 0x41 |

| Monitor corriente sistema | INA219 | I2C 0x40 |

| Cargador solar MPPT | CN3791 | Off-board, conectado por cable |

| Step-up boost | — | Calibrado a 5V, alimenta ESP32 |

| Batería | LiPo 1500mAh | Conecta directo a CN3791 (JST PH 1.25mm) |

| Panel solar | 5.2W \~24-cell | 210×165mm, diseñado para carga directa 12V, Voc=14.2V, Vmp≈11-12V |

| Caja estanca | — | 190×140×70mm |

| Mástil | PVC 3/4" marrón | — |

&#x20;

\---

&#x20;

\## GPIOs asignados — ESP32-C3 SuperMini

&#x20;

| GPIO | Señal | Tipo | Rail | Notas |

|---|---|---|---|---|

| GPIO0 | DHT22 DATA (ex DHT11) | Digital 1-wire | Rail B | Sensor reemplazado físicamente 2026-07-25, firmware adaptado — falta flashear. Ver "Estado actual en campo" |

| GPIO1 | Pluviómetro SIGNAL | Interrupt | Always-on | Reed switch, pull-up interno |

| GPIO2 | Anemómetro SIGNAL | Interrupt | Always-on | Reed switch, normalmente abierto → safe boot |

| GPIO3 | Fotoresistor ADC | ADC1\_CH3 | Rail B | — |

| GPIO4 | Rain sensor AO | ADC1\_CH4 | Rail B | JTAG MTMS como entrada en boot → sin conflicto |

| GPIO5 | SCL | I2C clock | — | Bus I2C compartido |

| GPIO6 | SDA | I2C data | — | Bus I2C compartido |

| GPIO7 | Rail A NPN base | Digital out | — | BC337 low-side switch sensores primarios |

| GPIO8 | Rail B NPN base | Digital out | — | BC337 low-side switch sensores secundarios. GPIO8 LOW en boot → Rail B OFF al arrancar ✅ |

| GPIO10 | DS18B20 DATA | 1-Wire | Always-on | Pull-up incluido en módulo Sunfounder |

&#x20;

\*\*GPIOs libres:\*\* GPIO9 (⚠️ botón BOOT físico — no usar), GPIO20, GPIO21

&#x20;

\---

&#x20;

\## I2C bus

&#x20;

\- \*\*SDA:\*\* GPIO6 — cable verde (color UTP)

\- \*\*SCL:\*\* GPIO5 — cable verde/blanco (color UTP)

\- \*\*Pull-ups externos:\*\* 2× 4.92kΩ (SDA→3.3V y SCL→3.3V) soldados en placa principal

\- \*\*Motivo:\*\* pull-ups internos del ESP32-C3 insuficientes para la capacitancia del cable UTP de \~1.5m al plato exterior

\### Dispositivos I2C

&#x20;

| Dirección | Dispositivo | Ubicación |

|---|---|---|

| 0x40 | INA219 system | Placa principal |

| 0x41 | INA219 solar | Placa principal |

| 0x44 | SHT31-D | Plato exterior (via UTP) |

| 0x77 | BMP180 | Plato exterior (via UTP) |

| 0x36 | AS5600 veleta | ⚠️ Nunca conectado — en duda, ver "Componentes pendientes de instalación" (posible alternativa óptica) |

&#x20;

\---

&#x20;

\## Switching de rails — BC337 NPN low-side

&#x20;

Dos transistores BC337 (pinout C-B-E con plano al frente) controlan el GND de los sensores. hFE medido: 250 y 263 en las unidades del prototipo perfboard; 226 en la unidad montada en la PCB Main v1.4. El rango de spec del BC337-25 es 160–400 y el valor depende de la corriente de test del tester — cualquier valor de ese orden está sobrado (se necesita hFE ≥ 4).

&#x20;

\### ⚠️ Orientación física — footprint de Fritzing con serigrafía inconsistente

&#x20;

**El footprint de transistor bipolar de Fritzing (TO92 THT, tipo "NPN (CBE)") tiene la D de la serigrafía dibujada de forma inconsistente con el orden C-B-E de los pads.** Un BC337 real insertado siguiendo la panza de la serigrafía queda con **colector y emisor cruzados**.

&#x20;

En la Main v1.4 esto pasó con Q1 RAIL1 y Q2 RAIL2 — ambos quedaron invertidos. Detectado midiendo continuidad en la placa armada (emisor al bus, colector a tierra). Corregido desoldando y rotando los dos transistores 180° sobre su eje vertical (la base queda en el mismo agujero, las patas externas intercambian lugar).

&#x20;

**Regla de armado:** insertar el BC337 con la parte plana **al revés de lo que marca la serigrafía**.

&#x20;

Notas de diagnóstico, por si reaparece:

&#x20;

\- El cobre y el esquemático de Fritzing **son correctos** — verificado por continuidad (GND, VCC, SDA, SCL, e inter-board contra la aux) y por el esquemático (emisor→tierra). El error es solo del silkscreen del footprint.

\- Cambiar el dropdown de tipo a "NPN (EBC)" **no sirve como fix**: Fritzing intenta invertir las pistas del PCB, porque el swap C/E está acoplado entre esquemático y PCB.

\- Síntoma eléctrico si queda invertido: el transistor opera en modo activo inverso (hFE ~2–5 en vez de 226), con Vce(sat) degradado. El "GND" del rail no queda en 0V sino flotando algunos cientos de mV, de forma variable — referencia sucia para los sensores I2C.

\- Verificación rápida con tester en modo diodo: punta roja en la pata del **centro** (base en ambas convenciones), negra en cada lateral. El que da la caída levemente **mayor** es el emisor, y va a GND del sistema.

&#x20;

\### Circuito (idéntico para Rail A y Rail B)

&#x20;

```

GPIO ──\[2.13kΩ]──┬── Base BC337

&#x20;                │

&#x20;             \[9.9kΩ]

&#x20;                │

&#x20;               GND

&#x20;

VCC sensores (siempre conectado)

&#x20;     │

&#x20; \[sensor]

&#x20;     │

&#x20; GND sensor ── Collector BC337

&#x20;                    │

&#x20;              Emitter BC337

&#x20;                    │

&#x20;                GND sistema

```

&#x20;

| Parámetro | Valor |

|---|---|

| R base | 2.13kΩ (nominal 2.2kΩ, tolerancia 3%) |

| R pull-down | 9.9kΩ (nominal 10kΩ, tolerancia 1%) |

| Ib | 1.22mA |

| Ic máxima estimada | \~5mA |

| hFE necesario | 4 → BC337 en saturación profunda ✅ |

&#x20;

\### Rail A — sensores primarios (GPIO7)

Activo en tiers 1, 2 y 3. Se apaga en tier 4.

\- SHT31-D + BMP180 (via UTP)

\### Rail B — sensores secundarios (GPIO8)

Activo solo en tier 1. Se apaga en tier 2.

\- DHT11

\- Rain sensor (placa sensora directa, sin módulo Sunfounder)

\- Fotoresistor

\- AS5600 veleta (pendiente instalación)

\### Always-on (sin switching)

\- DS18B20 (temperatura enclosure)

\- Anemómetro (reed switch)

\- Pluviómetro (reed switch)

\---

&#x20;

\## Tiers de energía por voltaje de batería

&#x20;

| Tier | Voltaje | Estado |

|---|---|---|

| 1 | ≥ 4.10V | Operación completa. Rail A + Rail B ON |

| 2 | 3.90–4.10V | Rail A ON · Rail B OFF |

| 3 | 3.75–3.90V | Solo SHT31+BMP180 cada 5min |

| 4 | < 3.75V | Heartbeat cada 30min. Todo OFF |

&#x20;

> ⚠️ **Esto es diseño en papel, no está implementado** (verificado contra el firmware el 2026-07-29). `src/battery.h` existe pero está vacío, `SLEEP_INTERVAL_SEC` es fijo en 60 s, `sensors_railsOn()` enciende los dos rails incondicionalmente, y el comando `config` por MQTT se parsea pero es un stub. Los umbrales de la tabla son la intención, no el comportamiento actual.
>
> **Corrección de premisa, importante si se retoma**: los tiers **no ahorran nada durante el deep sleep**. GPIO7 y GPIO8 no son RTC GPIOs en el ESP32-C3 (sólo GPIO0–5 lo son), así que al dormir quedan sin drive y el pull-down de 9,9 kΩ de la base del BC337 corta los dos rails por su cuenta. Lo que los tiers recortan es la ventana despierta, donde los rails son ~2-3 mA contra los 51,2 mA medidos del sistema completo. El ahorro real de este diseño está en los tiers 3 y 4, que cambian el **intervalo de sleep** — no en el switching de rails.

Tiers configurables via MQTT `config` sin reflash (pendiente: el comando existe pero no está implementado).

&#x20;

\---

&#x20;

\## Circuito rain sensor (sin módulo Sunfounder)

&#x20;

Se prescinde del módulo completo. Solo se usa la placa sensora conectada directamente.

&#x20;

```

3V3

&#x20;│

\[R1 9.9kΩ]─┐

\[R2 9.9kΩ]─┴──── nodo señal+ ──── GPIO4 (ADC)

(paralelo = 4.95kΩ)   │

&#x20;                   \[C1 100nF]

&#x20;                      │

&#x20;                   Rail B GND ──── placa sensora pin 2

```

&#x20;

\- R1 ∥ R2 = 4.95kΩ pull-up (impedancia fuente óptima para ADC)

\- C1 100nF cerámico (marcado "104"): filtra ruido 50Hz del cable de 1.5m

\- Salida: solo AO analógica. DO descartado (redundante con lectura ADC por firmware)

\---

&#x20;

\## Placa principal — 31×26 holes

&#x20;

\### Zonas

&#x20;

| Zona | Cols | Rows | Contenido |

|---|---|---|---|

| INA219 #1 Solar | cols 1–9 | rows 1–10 | Header 6× hembra col 9, rows 3–8 — \*\*I2C 0x41\*\* |

| INA219 #2 System | cols 10–18 | rows 1–10 | Header 6× hembra col 18, rows 3–8 — \*\*I2C 0x40\*\* |

| JST Solar + CN3791 | 19–31 | 1–10 | 2× JST 2p acceso superior derecho |

| Step-up boost | 1–14 | 11–16 | 4 pines (VIN col 1, VOUT col 14) |

| ESP32-C3 SuperMini | 15–21 | 11–18 | 8× hembra col 15 + 8× hembra col 21 |

| NPN A + resistencias | 23–27 | 12–14 | BC337 Rail A |

| NPN B + resistencias | 23–27 | 16–18 | BC337 Rail B |

| Pull-ups I2C | 28–29 | 22–25 | R\_SCL (28) y R\_SDA (29) |

| Buses | — | 22–25 | SCL·SDA·GND·3V3 full width |

| Header inter-board | 10–21 | 26 | Macho 1×12, borde inferior |

&#x20;

\### Buses

&#x20;

| Fila | Señal |

|---|---|

| 22 | SCL |

| 23 | SDA |

| 24 | GND |

| 25 | 3V3 |

| 26 | Header inter-board 1×12 (cols 10–21) |

&#x20;

\---

&#x20;

\## Placa auxiliar JST — 24×10 holes

&#x20;

\### Conectores JST

&#x20;

⚠️ **Orientación:** en la PCB v2 los JST habían quedado montados 180° invertidos respecto del prototipo perfboard v1. Se dieron vuelta todos y quedó correcto. Al armar una placa nueva, verificar la orientación del conector contra el pinout de esta tabla antes de soldar — el orden de pines de abajo asume la orientación ya corregida.

&#x20;

| # | Conector | Pines | Pinout | Rail | Ubicación |

|---|---|---|---|---|---|

| 1 | UTP SHT31+BMP180 | 4 | VCC · GND\_A · SCL · SDA | Rail A | cols 2–5, fila 3 |

| 2 | Rain sensor | 2 | señal+ · señal− | Rail B | cols 7–8, fila 3 |

| 3 | DHT11 | 3 | VCC · GND\_B · DATA | Rail B | cols 11–13, fila 3 |

| 4 | Fotoresistor | 3 | VCC · GND\_B · ADC | Rail B | cols 15–17, fila 3 |

| 5 | Veleta AS5600 | 5 | VCC · GND\_B · SCL · SDA · DIR | Rail B | cols 19–23, fila 3 |

| 6 | DS18B20 | 3 | VCC · GND · DATA | Always-on | cols 2–4, fila 7 |

| 7 | Anemómetro | 3 | VCC · GND · SIGNAL | Always-on | cols 6–8, fila 7 |

| 8 | Pluviómetro | 3 | VCC · GND · SIGNAL | Always-on | cols 10–12, fila 7 |

&#x20;

\### Pulsadores de diagnóstico

&#x20;

| Pulsador | Cols | Rows | GPIO | En paralelo con |

|---|---|---|---|---|

| BTN\_ANE | 15–19 | 7–10 | GPIO2 | Anemómetro (JST7 SIGNAL) |

| BTN\_PLU | 20–24 | 7–10 | GPIO1 | Pluviómetro (JST8 SIGNAL) |

&#x20;

Footprint: 5 cols × 4 rows. Pines en las 4 esquinas.

P1 = señal GPIO. P2 = GND. Se cierra al presionar.

&#x20;

\### DIR veleta AS5600

\- Hardwired a GND vía puente de estaño en holes (23,4)→(23,5)

\- Pad de acceso externo en col 24 borde derecho

\- Para invertir: cortar puente con bisturí, conectar (23,4) a 3V3

\---

&#x20;

\## Header inter-board 1×12

&#x20;

Macho en placa principal (fila 26, cols 10–21).

Hembra en placa auxiliar (fila 1, cols 7–18).

&#x20;

| Pin | Col principal | Col auxiliar | Señal | GPIO |

|---|---|---|---|---|

| 1 | 10 | 7 | GND | — |

| 2 | 11 | 8 | 3V3 | — |

| 3 | 12 | 9 | SCL | GPIO5 |

| 4 | 13 | 10 | SDA | GPIO6 |

| 5 | 14 | 11 | Rail A GND | NPN A collector |

| 6 | 15 | 12 | Rail B GND | NPN B collector |

| 7 | 16 | 13 | Anemómetro | GPIO2 |

| 8 | 17 | 14 | Pluviómetro | GPIO1 |

| 9 | 18 | 15 | DHT11 | GPIO0 |

| 10 | 19 | 16 | DS18B20 | GPIO10 |

| 11 | 20 | 17 | Fotoresistor ADC | GPIO3 |

| 12 | 21 | 18 | Rain sensor AO | GPIO4 |

&#x20;

\---

&#x20;

\## Componentes pendientes de instalación

&#x20;

| Componente | Estado | Notas |

|---|---|---|

| DHT22 | Instalado físicamente + firmware adaptado (2026-07-25), falta flashear | Reemplazo del DHT11 (murió, falla crónica). Transplante del sensor pelado al módulo Sunfounder original — ver "Estado actual en campo" |

| AS5600 veleta | 🔄 En validación de banco (actualizado 2026-07-25) | Vuelve a ser candidato fuerte: el módulo vino con un imán chico no notado antes (corrige la conclusión previa de incompatibilidad). Conectores dupont ya soldados; prueba en banco con Arduino Mega en curso — primero a mano sin la veleta, después PoC con la veleta montada. Pendiente confirmar si el imán es diametral — ver `aprendizajes_y_roadmap.md` → "Dirección" |

| Alternativa (plan B): disco óptico Gray code | En sourcing | 3× sensores IR reflectivos TCRT5000 (kit Sunfounder "Tracking Sensor") dan posición absoluta (8 posiciones, resolución 45°) al despertar de deep sleep — resuelve la limitación de posición relativa del AS5600. Mau tiene 1 sensor, necesita 2 más. Queda como respaldo si el AS5600 no resuelve en banco |

| PCF8574 (I2C GPIO expander) | Por comprar | 0x20, sin conflicto de bus — necesario para leer los 3× TCRT5000 (P0–P2) si se va por la vía óptica. GPIO20 controlaría LED\_EN vía BC337 |

| 608ZZ bearings | Por comprar | Para anemómetro y veleta |

| Chasis anemómetro+veleta | ✅ Impreso en PLA | STL "Weather Station One" (Printables/MakerWorld, modelo "The Windicator V1"). Pendiente armado mecánico y prueba en campo — todavía no se empezó |

&#x20;

\### Materiales disponibles para armado

\- Reed switch del kit Sunfounder → apto para anemómetro (detección de pulsos)

\- Imanes pequeños disponibles → pueden funcionar para anemómetro (activar reed switch)

\- ⚠️ Falta imán diametral grande específico para AS5600 (veleta de dirección)

\### Pendiente de diseño

\- Definir cómo instalar imanes y sensores en base al diseño de la carcasa impresa

\- Verificar posición del reed switch relativa al rotor del anemómetro

\- Verificar posición del AS5600 relativa al eje de la veleta (distancia óptima al imán: 0.5–3mm)

\---

&#x20;

\## Notas de diseño importantes

&#x20;

\- \*\*`system\_mW` en InfluxDB no es consumo real\*\* — solo captura la fase WiFi activa (\~10s), no el deep sleep (\~54s). Consumo real inferir de estabilidad de voltaje de batería.

\- \*\*I2C sobre UTP requiere pull-ups externos\*\* — 4.92kΩ validados en prueba de campo de 46 horas.

\- \*\*CN3791 MPPT configurado para Vmp real del panel\*\* — verificar R1/R2 del divisor VMP físicamente. Mismatch con panel de 24 celdas (Vmp≈11.5V) inutiliza el MPPT.

\- \*\*El CN3791 cicla carga/corte cuando el pack está lleno, y eso es normal\*\* (medido 2026-08-07, día despejado, 9 h: 14 rachas de carga y 15 de corte). Al cortar el panel sube a \~14,2 V (circuito abierto) con \~6,5 mA; al arrancar baja a \~11,5 V (Vmp bajo carga) con \~200 mA. Es terminación por CV más auto-recarga con histéresis. Las rachas largas (hasta 3,7 h) son la carga real de la mañana; el cicleo corto aparece recién con el pack lleno. \*\*No confundirlo con una falla\*\* — y ojo que hace oscilar el SoC y el estado energético que muestra el dashboard, ver `../STATUS.md`.

\- \*\*El módulo CN3791 tiene dos LEDs de estado y consumen del panel\*\* (confirmado por Mau, 2026-08-09): \*\*rojo\*\* mientras carga, \*\*azul\*\* con la carga completa. \*\*Sólo encienden cuando el panel entrega\*\*, así que no drenan la batería de noche — verificado en los datos: `solar_mA` va a −0,2 mA en la oscuridad.

  El azul es el que se mide fácil, porque es el que queda encendido en la fase de corte donde no hay corriente de carga que lo tape: son los \*\*\~6,5 mA a \~14 V ≈ 90 mW\*\* del piso de esa fase. Sobre las \~2,2 h diarias que el sistema pasa cortado, son del orden de \*\*50 mAh/día equivalentes\*\* en la batería — comparable a todo el consumo diario del nodo (\~47 mAh/día).

  Hoy no duele porque el azul enciende justo cuando sobra energía, y el rojo (menor caída directa, menos consumo) es el que está durante la carga. \*\*El diseño del módulo acertó\*\*: el LED más caro es el que prende cuando la energía es gratis. Pero en una racha de días nublados esos 90 mW salen del presupuesto, así que si alguna vez hay que exprimir autonomía, desoldar el azul es la palanca — misma lógica que la regla de desoldar LEDs de los módulos Sunfounder.

\- \*\*GPIO8 LOW en boot\*\* — deshabilita mensajes ROM por UART0. Sin impacto en operación del firmware.

\- \*\*GPIO9 reservado\*\* — es el botón BOOT físico del ESP32-C3 SuperMini. No conectar.

\- \*\*Módulos Sunfounder: desoldar LEDs\*\* antes de montar en PCB — BMP180 \*\*(hecho)\*\*, \*\*DS18B20 (hecho)\*\*, DHT11, fotoresistor (verificar si tiene). \*\*Los módulos INA219 no tienen LED de alimentación\*\* — verificado el 2026-07-29. \*\*El LED del propio ESP32-C3 SuperMini tampoco está activo\*\* — desoldado (2026-08-02). Importaba porque cuelgan del bus 3V3, que sigue vivo durante el deep sleep: un LED de 1-3 mA por módulo habría consumido más que todo el resto del sistema junto, y no hay firmware que lo apague.

  \*\*Los LEDs quedan entonces descartados como fuente del consumo de reposo\*\* (ver el punto siguiente). El DHT11 y el fotorresistor cuelgan de Rail B, que se apaga al dormir, así que aunque tuvieran LED no cuentan.

\- \*\*Consumo activo medido: 51,2 mA\*\* (mediana del INA219 de sistema sobre 59 muestras de campo, 2026-07-29), con la ventana despierta en 3,3 s por ciclo antes del firmware `1.5.0`. La cifra de ~100 mA que circulaba por los documentos estaba 2× alta. El consumo en deep sleep sigue \*\*sin medir directamente\*\* — el INA219 sólo muestrea con el nodo despierto, y además el firmware lo pone en power-down al dormir, así que \*\*no hay cambio de firmware que pueda resolverlo\*\*: hace falta un multímetro en serie con la batería.

\- ⚠️ \*\*El reposo pesa más que la ventana activa — estimado, sin medir (2026-08-02).\*\* Estimado indirectamente desde la caída de tensión de la batería durante la noche, que integra todo el consumo porque no entra carga: mediana de \*\*5,10 mA\*\* de consumo medio total sobre 5 noches. La ventana activa explica \*\*1,86 mA\*\* (51,2 mA × 3,64% de ciclo de trabajo), así que quedan \*\*~3,2 mA sin explicar, el 63% del total\*\*. En mAh/día: activo 44,7 · resto 77,7 · \*\*total 122,4\*\*.

  \*\*Caveats\*\*: la conversión volts→mAh usa la curva de SoC del backend, que es un indicador y no una contabilidad; y la temperatura baja de noche, lo que sube la resistencia interna, así que parte de la caída puede ser térmica y no consumo. Aun con un factor 2 de error sigue siendo mA, no µA.

  \*\*Qué queda como sospechoso\*\*, ahora que los LEDs están descartados: el \*\*reposo del step-up boost\*\*, que tiene que estar siempre encendido porque alimenta al ESP32 que despierta (los módulos tipo MT3608 están en 0,5-2 mA y empeoran a carga liviana), más el regulador propio de la SuperMini. Los dos INA219 en power-down son ~12 µA y no explican nada.

  \*\*Pendiente\*\*: medirlo con multímetro en serie con la batería, con el nodo dormido, y después el reposo del boost solo con el ESP32 desconectado. Es la medición de mayor valor pendiente del proyecto — decide si tiene sentido optimizar la ventana activa (44,7 mAh/día) o el reposo (77,7 mAh/día).

\- \*\*Los dos INA219 se apagan entre ciclos desde el firmware `1.4.0`\*\* (`sensors_sleepMonitors()`, modo power-down ~6 µA contra ~0,7-1 mA en conversión continua). Cuelgan del bus 3V3 siempre alimentado, así que antes convertían los ~57 s por ciclo en que nadie los lee. El power-down apaga el ADC del chip pero \*\*no abre el shunt\*\*, así que la carga solar por el CN3791 no se ve afectada.

\- \*\*Rain sensor: prescindir del módulo Sunfounder completo\*\* — conectar placa sensora directamente con pull-up 4.95kΩ y filtro 100nF en placa auxiliar.

